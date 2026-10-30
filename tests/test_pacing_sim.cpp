// SPDX-License-Identifier: GPL-2.0-or-later
// gpu/shim/bbport_pacing.cpp against a simulated display, in real time. See tests/run_pacing_sim.sh.
//
// The display refreshes every 1/59.8 s (--hz; a panel's clock is never the CPU's exactly). The game
// hands a frame over at each slot of a 30 FPS frame limiter written like the one in
// shadps4/core/libraries/videoout/driver.cpp (the next slot from this one when the flip ran less than
// a millisecond after it, else from the flip; --from-wake: always from the flip, as 2.2.0 did; then
// BbPacing::AdjustSlot);
// the frame is ready 0.5 to 3.5 ms later (GPU work; --gpu-ms LO HI), the first slot --phase-ms after
// the first refresh; --late PCT MS: that share of the frames comes that much after its slot. At every refresh the display shows the next
// frame if it was ready 4 ms before (the compositor's latch), in order (FIFO), and the frame it
// replaces gets a release fence that signals then, handed to the game as the app would. The display
// counts how many refreshes each frame stayed: at 30 FPS on ~60 Hz it should be two, always.
//
// The release fences are memfds standing in for sync files: the test is linked with
// -Wl,--wrap=ioctl and answers SYNC_IOC_FILE_INFO for them the way the kernel does (the count and
// status first, then the fences with their signal time), by inode, so the module's own duplicate of a
// fence is the same fence.
//
// Result line: "frames N, not two refreshes M (P%)"; with --expect-max P the exit status says whether
// P stayed at most that.
#include "bbport_pacing.h"

#include <fcntl.h>
#include <linux/sync_file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

s64 Now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return s64(ts.tv_sec) * 1000000000ll + ts.tv_nsec;
}

void SleepUntil(s64 t) {
    timespec ts{time_t(t / 1000000000ll), long(t % 1000000000ll)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
    }
}

std::mutex fence_lock;
std::unordered_map<ino_t, s64> fence_times; ///< inode -> signal time (0: pending)

/// A fence that signals at `t` (a memfd: its own inode).
int MakeFence(s64 t) {
    const int fd = memfd_create("release", MFD_CLOEXEC);
    struct stat st;
    if (fd >= 0 && fstat(fd, &st) == 0) {
        std::scoped_lock lk{fence_lock};
        fence_times[st.st_ino] = t;
    }
    return fd;
}

struct Frame {
    u64 number;
    s64 ready;
};

struct Release {
    u64 frame;
    int fd;
};

std::mutex lock;
std::deque<Frame> submitted;  // game -> display
std::deque<Release> releases; // display -> game
std::atomic<bool> stop{false};

} // namespace

extern "C" int __real_ioctl(int fd, unsigned long request, ...);
extern "C" int __wrap_ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    struct stat st;
    if (request == SYNC_IOC_FILE_INFO && fstat(fd, &st) == 0) {
        s64 t = -1;
        {
            std::scoped_lock lk{fence_lock};
            if (const auto it = fence_times.find(st.st_ino); it != fence_times.end()) {
                t = it->second;
            }
        }
        if (t >= 0) {
            auto* info = static_cast<sync_file_info*>(arg);
            const bool signaled = t > 0 && t <= Now();
            if (info->num_fences == 0) {
                info->status = signaled ? 1 : 0;
                info->num_fences = 1;
                return 0;
            }
            auto* fences = reinterpret_cast<sync_fence_info*>(info->sync_fence_info);
            std::memset(fences, 0, sizeof(*fences));
            fences[0].status = signaled ? 1 : 0;
            fences[0].timestamp_ns = signaled ? __u64(t) : 0;
            info->status = signaled ? 1 : 0;
            info->num_fences = 1;
            return 0;
        }
    }
    return __real_ioctl(fd, request, arg);
}

int main(int argc, char** argv) {
    double seconds = 40.0, expect_max = -1.0, hz = 59.8;
    double phase_ms = 0.0, gpu_lo_ms = 0.5, gpu_hi_ms = 3.5, late_pct = 0.0, late_ms = 0.0;
    bool from_wake = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        } else if (!std::strcmp(argv[i], "--hz") && i + 1 < argc) {
            hz = std::atof(argv[++i]);
        } else if (!std::strcmp(argv[i], "--phase-ms") && i + 1 < argc) {
            phase_ms = std::atof(argv[++i]);
        } else if (!std::strcmp(argv[i], "--gpu-ms") && i + 2 < argc) {
            gpu_lo_ms = std::atof(argv[++i]);
            gpu_hi_ms = std::atof(argv[++i]);
        } else if (!std::strcmp(argv[i], "--late") && i + 2 < argc) {
            late_pct = std::atof(argv[++i]);
            late_ms = std::atof(argv[++i]);
        } else if (!std::strcmp(argv[i], "--from-wake")) {
            from_wake = true;
        } else if (!std::strcmp(argv[i], "--expect-max") && i + 1 < argc) {
            expect_max = std::atof(argv[++i]);
        }
    }
    const s64 refresh = s64(1e9 / hz);
    const s64 latch_ahead = 4000000;
    const s64 period = 1000000000ll / 30;
    const s64 start = Now() + 50000000;
    const s64 end = start + s64(seconds * 1e9);

    // The display.
    u64 not_two = 0, counted = 0;
    std::thread display([&] {
        u64 shown = 0; // the frame on screen
        s64 shown_at = 0;
        for (s64 n = 1; !stop; ++n) {
            const s64 vsync = start + n * refresh;
            SleepUntil(vsync - latch_ahead);
            Frame next{};
            bool latch = false;
            {
                std::scoped_lock lk{lock};
                if (!submitted.empty() && submitted.front().ready <= vsync - latch_ahead) {
                    next = submitted.front();
                    submitted.pop_front();
                    latch = true;
                }
            }
            SleepUntil(vsync);
            if (!latch) {
                continue;
            }
            if (shown) {
                // The frame that leaves: its release fence signals now.
                const int fd = MakeFence(vsync);
                if (fd >= 0) {
                    std::scoped_lock lk{lock};
                    releases.push_back({shown, fd});
                }
                const s64 refreshes = (vsync - shown_at + refresh / 2) / refresh;
                if (shown_at > start + 3000000000ll) { // after the first seconds (settling)
                    ++counted;
                    not_two += refreshes != 2;
                }
            }
            shown = next.number;
            shown_at = vsync;
        }
    });

    // The game: the frame limiter and the hand-over, with the releases read before each frame (as
    // Swapchain::AcquireNextImage does).
    std::mt19937 rng(12345);
    std::uniform_int_distribution<s64> gpu(s64(gpu_lo_ms * 1e6), s64(gpu_hi_ms * 1e6));
    s64 slot = start + s64(phase_ms * 1e6);
    std::uniform_real_distribution<double> chance(0.0, 100.0);
    for (u64 frame = 1; Now() < end; ++frame) {
        if (Now() < slot) {
            SleepUntil(slot);
        }
        if (late_pct > 0.0 && chance(rng) < late_pct) {
            SleepUntil(Now() + s64(late_ms * 1e6)); // the game's frame came after its slot
        }
        const s64 at = Now();
        const s64 from = !from_wake && at <= slot + std::min<s64>(1000000, period / 10)
                             ? slot
                             : std::max(slot, at);
        slot = BbPacing::AdjustSlot(from + period, period);
        std::vector<Release> got;
        {
            std::scoped_lock lk{lock};
            got.assign(releases.begin(), releases.end());
            releases.clear();
        }
        for (const Release& r : got) {
            BbPacing::FrameReleased(r.frame, r.fd, true);
            close(r.fd);
        }
        BbPacing::FrameSent(frame);
        std::scoped_lock lk{lock};
        submitted.push_back({frame, at + gpu(rng)});
    }
    stop = true;
    display.join();
    const double pct = counted ? 100.0 * double(not_two) / double(counted) : 100.0;
    std::printf("frames %llu, not two refreshes %llu (%.2f%%)\n",
                static_cast<unsigned long long>(counted), static_cast<unsigned long long>(not_two),
                pct);
    return expect_max >= 0.0 && pct > expect_max ? 1 : 0;
}
