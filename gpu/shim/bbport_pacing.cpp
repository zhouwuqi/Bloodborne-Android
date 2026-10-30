// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: display pacing feedback and the frame limiter's phase lock (bbport_pacing.h).
//
// Times: the release fences' signal times (sync_file_info, CLOCK_MONOTONIC) and the hand-over times
// (CLOCK_MONOTONIC here too). Frame F was on screen from the release of F - 1 (F replaced it) to its
// own release; its latency is the release of F - 1 minus the hand-over of F. All of it runs on the
// swap thread (Swapchain::Present and its release handling), the slot adjustment on the present
// thread: one mutex, taken a few times per frame.
#include "bbport_pacing.h"

#ifdef __linux__ // Android in practice: the PC build has no app in between, nothing calls in
#include <fcntl.h>
#include <linux/sync_file.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <vector>

namespace BbPacing {
namespace {

constexpr s64 Ms = 1000000;
constexpr s64 Second = 1000 * Ms;

s64 NowNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return s64(ts.tv_sec) * Second + s64(ts.tv_nsec);
}

bool Flag(const char* name, bool fallback) {
    const char* v = std::getenv(name);
    return v && *v ? v[0] != '0' : fallback;
}

/// The signal time of a sync file (the latest of its fences): 1 = signaled (t set), 0 = still
/// pending, -1 = no usable time.
int FenceTime(int fd, s64& t) {
    sync_file_info info{};
    if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) < 0) {
        return -1;
    }
    if (info.status == 0) {
        return 0;
    }
    std::array<sync_fence_info, 8> fences{};
    if (info.status < 0 || info.num_fences == 0 || info.num_fences > fences.size()) {
        return -1;
    }
    sync_file_info full{};
    full.num_fences = info.num_fences;
    full.sync_fence_info = reinterpret_cast<__u64>(fences.data());
    if (ioctl(fd, SYNC_IOC_FILE_INFO, &full) < 0 || full.status != 1) {
        return full.status == 0 ? 0 : -1;
    }
    __u64 latest = 0;
    for (u32 i = 0; i < full.num_fences; ++i) {
        latest = std::max<__u64>(latest, fences[i].timestamp_ns);
    }
    if (latest == 0) {
        return -1;
    }
    t = s64(latest);
    return 1;
}

/// Display refresh periods a judder can be one of (a frame one refresh too short or too long).
double SnapRefresh(double ns) {
    static constexpr double rates[] = {165.0, 144.0, 120.0, 90.0, 60.0, 50.0, 48.0};
    double best = 0.0, best_error = 0.15;
    for (const double hz : rates) {
        const double period = 1e9 / hz;
        const double error = std::abs(ns - period) / period;
        if (error < best_error) {
            best_error = error;
            best = period;
        }
    }
    return best;
}

struct Sent {
    u64 frame = 0;
    s64 t = 0;
};

struct Pending {
    u64 frame;
    int fd;
    s64 received;
};

/// One stats interval.
struct Window {
    s64 start = 0;
    u32 shown = 0, on = 0, shorter = 0, longer = 0, display_only = 0, late = 0;
    double sum_ms = 0.0, sum_sq_ms = 0.0;
    std::vector<float> latency_ms;
};

struct State {
    std::mutex lock;
    bool stats = true;
    bool lock_on = true;
    bool fifo = false;
    std::array<Sent, 64> sent{}; ///< by frame % 64
    std::deque<Pending> pending; ///< release fences not signaled yet, in release order
    u64 last_off_frame = 0;      ///< the latest frame whose release time is known (0: none)
    s64 last_off_t = 0;
    // The interval the game hands frames over at: median of the last ones (the frame limit, or the
    // game's own rate when it is slower, or the limit times the frame generation multiplier).
    std::array<s64, 31> intervals{};
    u32 interval_count = 0, interval_next = 0;
    s64 last_send = 0;
    u64 last_send_frame = 0;
    // Phase lock: a PLL on the hand-over-to-screen time. The error is the smoothed latency minus the
    // reference (the latency when it locked); the integral is the frame period's difference between
    // the display and the CPU clock, learned; the proportional term pulls the phase back.
    double latency_ema = 0.0, latency_ref = 0.0;
    bool latency_valid = false, ref_valid = false;
    bool fresh = false;    ///< a steady sample came since the last slot
    double drift = 0.0;    ///< ns per frame the slots move (the integral)
    double jump_to = 0.0;  ///< a latency a refresh or more away, seen once: twice in a row is a jump
    u32 jump_streak = 0;
    u32 settle = 30;       ///< steady frames to wait for before a reference is taken
    u32 recovering = 0;    ///< steady frames still to come after a jump before judders count again
    s64 pending_shift = 0; ///< for the next slot: a new phase (the reference moves with it)
    s64 last_shift_t = 0, last_judder_t = 0;
    u32 shifts = 0, shift_streak = 0;
    std::deque<s64> judders; ///< display-only judders of the last seconds
    double refresh_ns = 0.0; ///< the display refresh the judders showed; 0: not seen yet
    Window w;
};

State& S() {
    static State state;
    return state;
}

struct Thermal {
    std::atomic<int> status{-1};
    std::atomic<float> headroom{NAN};
    std::atomic<int> level{0};
};
Thermal& T() {
    static Thermal thermal;
    return thermal;
}

void Init(State& s) {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    s.stats = Flag("BB_PACING_STATS", true);
    s.lock_on = Flag("BB_PACE_LOCK", true);
    s.w.start = NowNs();
    std::printf("Pacing: display feedback from release fences, stats %s, frame limiter phase lock %s "
                "(BB_PACING_STATS, BB_PACE_LOCK; the lock needs vsync=1)\n",
                s.stats ? "on" : "off", s.lock_on ? "on" : "off");
}

s64 MedianInterval(const State& s) {
    if (s.interval_count < 8) {
        return 0;
    }
    std::array<s64, 31> sorted = s.intervals;
    const auto end = sorted.begin() + s.interval_count;
    std::nth_element(sorted.begin(), sorted.begin() + s.interval_count / 2, end);
    return sorted[s.interval_count / 2];
}

void Print(State& s, s64 now) {
    Window& w = s.w;
    const double seconds = double(now - w.start) / double(Second);
    if (w.shown == 0) {
        std::printf("Pacing: no frame times in %.0f s (%s)\n", seconds,
                    s.fifo ? "vsync on" : "vsync off");
    } else {
        const double mean = w.sum_ms / w.shown;
        const double sd = std::sqrt(std::max(0.0, w.sum_sq_ms / w.shown - mean * mean));
        float p5 = 0.0f, p50 = 0.0f, p95 = 0.0f;
        if (!w.latency_ms.empty()) {
            std::sort(w.latency_ms.begin(), w.latency_ms.end());
            const auto at = [&](double q) {
                return w.latency_ms[std::min(w.latency_ms.size() - 1,
                                             std::size_t(q * double(w.latency_ms.size())))];
            };
            p5 = at(0.05);
            p50 = at(0.50);
            p95 = at(0.95);
        }
        const double pct = 100.0 / w.shown;
        const Thermal& t = T();
        const float headroom = t.headroom.load();
        char refresh[32];
        if (s.refresh_ns > 0.0) {
            std::snprintf(refresh, sizeof(refresh), "%.2f ms", s.refresh_ns / 1e6);
        } else {
            std::snprintf(refresh, sizeof(refresh), "not seen (no judder yet)");
        }
        char thermal[64];
        if (t.status.load() < 0) {
            std::snprintf(thermal, sizeof(thermal), "thermal unknown");
        } else if (std::isfinite(headroom)) {
            std::snprintf(thermal, sizeof(thermal), "thermal %d, headroom %.2f, governor %d",
                          t.status.load(), double(headroom), t.level.load());
        } else {
            std::snprintf(thermal, sizeof(thermal), "thermal %d, governor %d", t.status.load(),
                          t.level.load());
        }
        std::printf("Pacing: %.1f FPS on screen (%s), handed over every %.1f ms; on screen for that "
                    "long %.1f%%, shorter %.1f%%, longer %.1f%% (uneven at the display only %.1f%%, "
                    "the game late %.1f%%), %.1f ms sd %.2f; hand-over to screen %.1f ms (p5 %.1f, "
                    "p95 %.1f); refresh %s; lock %s, %u shifts; %s\n",
                    w.shown / seconds, s.fifo ? "vsync on" : "vsync off: approximate",
                    double(MedianInterval(s)) / 1e6, w.on * pct, w.shorter * pct, w.longer * pct,
                    w.display_only * pct, w.late * pct, mean, sd, double(p50), double(p5),
                    double(p95), refresh,
                    !s.lock_on ? "off" : !s.fifo ? "idle (vsync off)" : s.ref_valid ? "on" : "settling",
                    s.shifts, thermal);
    }
    std::fflush(stdout);
    w = Window{};
    w.start = now;
}

/// Frame `frame` was on screen for `duration` from `on_at`.
void Classify(State& s, u64 frame, s64 on_at, s64 duration, s64 now) {
    const s64 interval = MedianInterval(s);
    if (interval <= 0 || duration > Second) {
        return;
    }
    const Sent& cur = s.sent[frame % s.sent.size()];
    const Sent& prev = s.sent[(frame - 1) % s.sent.size()];
    const Sent& next = s.sent[(frame + 1) % s.sent.size()];
    const s64 latency = cur.frame == frame ? on_at - cur.t : -1;
    // The hand-overs before and after the frame: its time on screen ends when the next one comes, so
    // a late next frame keeps it there longer (handed over itself on time or not). The next one was
    // handed over already: it is the one that replaced this frame.
    const s64 send_gap = cur.frame == frame && prev.frame == frame - 1 ? cur.t - prev.t : -1;
    const s64 next_gap = cur.frame == frame && next.frame == frame + 1 ? next.t - cur.t : -1;
    const double tolerance = 0.2 * double(interval);
    const double deviation = double(duration - interval);
    const bool on = std::abs(deviation) < tolerance;
    const auto even_gap = [&](s64 gap) { return gap > 0 && std::abs(double(gap - interval)) < tolerance; };
    const bool even = even_gap(send_gap) && (next_gap < 0 || even_gap(next_gap));
    const s64 late_after = interval + s64(tolerance);

    Window& w = s.w;
    ++w.shown;
    const double ms = double(duration) / 1e6;
    w.sum_ms += ms;
    w.sum_sq_ms += ms * ms;
    if (on) {
        ++w.on;
    } else if (deviation < 0) {
        ++w.shorter;
    } else {
        ++w.longer;
    }
    if (!on && even) {
        // Left the game on time, shown for a refresh more or less: the hand-over sits near a latch.
        // Only a judder at the locked phase says the phase is wrong; one right after a jump (a late
        // frame) is the way back to it.
        ++w.display_only;
        if (s.recovering == 0) {
            s.judders.push_back(now);
            s.last_judder_t = now;
        }
    } else if (!on && (send_gap > late_after || next_gap > late_after)) {
        ++w.late;
    }
    // The display shows whole refreshes: a frame one too short or too long tells the refresh.
    if (!on) {
        if (const double refresh = SnapRefresh(std::abs(deviation)); refresh > 0.0) {
            s.refresh_ns = s.refresh_ns > 0.0 ? std::min(s.refresh_ns, refresh) : refresh;
        }
    }
    if (latency > 0 && latency < Second / 5) {
        w.latency_ms.push_back(float(double(latency) / 1e6));
    }

    // The phase lock's measurement: steady frames only (on time, shown as long as they should be).
    if (on && even && latency > 0) {
        const double l = double(latency);
        const double jump = s.refresh_ns > 0.0 ? 0.35 * s.refresh_ns : 0.15 * double(interval);
        if (!s.latency_valid) {
            s.latency_ema = l;
            s.latency_valid = true;
        } else if (std::abs(l - s.latency_ema) > jump) {
            // Far away (a late frame moved the slots, the frames land a refresh later): twice in a
            // row is the new level. What the lock keeps is the phase within a refresh: the reference
            // moves by the whole refreshes, the proportional term takes the rest back over the next
            // frames (each a little longer or shorter, never a refresh), and the integral, which
            // learns near the reference only, keeps the drift it knows. Without a known refresh the
            // new level is taken as it is.
            if (s.jump_streak > 0 && std::abs(l - s.jump_to) < jump) {
                const double raw = l - (s.ref_valid ? s.latency_ref : s.latency_ema);
                const double whole =
                    s.refresh_ns > 0.0 ? std::round(raw / s.refresh_ns) * s.refresh_ns : raw;
                s.latency_ref += whole;
                s.latency_ema = l;
                s.jump_streak = 0;
                s.recovering = 30;
            } else {
                s.jump_to = l;
                s.jump_streak = 1;
            }
            return;
        } else {
            s.jump_streak = 0;
            s.latency_ema += (l - s.latency_ema) * 0.1;
        }
        if (s.settle > 0) {
            --s.settle;
        } else if (!s.ref_valid) {
            s.latency_ref = s.latency_ema;
            s.ref_valid = true;
        }
        s.fresh = s.ref_valid;
        if (s.recovering > 0) {
            --s.recovering;
        }
    }

    // Uneven at the display again and again: half a refresh away from the deadline. Each try waits
    // longer (4, 8, 16, 32, 64 s) while the judders go on; 20 s without one starts over.
    while (!s.judders.empty() && now - s.judders.front() > 3 * Second) {
        s.judders.pop_front();
    }
    if (s.shift_streak && now - s.last_judder_t > 20 * Second) {
        s.shift_streak = 0;
    }
    const s64 gap = 4 * Second << std::min<u32>(s.shift_streak, 4);
    if (s.lock_on && s.fifo && s.refresh_ns > 0.0 && s.judders.size() >= 3 &&
        now - s.last_shift_t > gap) {
        s.pending_shift = s64(s.refresh_ns / 2.0);
        s.last_shift_t = now;
        ++s.shifts;
        ++s.shift_streak;
        s.judders.clear();
        std::printf("Pacing: frames handed over evenly reached the screen unevenly: the frame "
                    "limiter's slots move by %.1f ms (half a refresh)\n",
                    double(s.pending_shift) / 1e6);
    }
}

/// The release times that are known by now, in release order.
void Poll(State& s, s64 now) {
    while (!s.pending.empty()) {
        Pending& p = s.pending.front();
        s64 t = 0;
        const int state = FenceTime(p.fd, t);
        if (state == 0 && now - p.received < Second / 2) {
            break; // the next ones signal after this one
        }
        close(p.fd);
        const u64 frame = p.frame;
        s.pending.pop_front();
        if (state != 1) {
            s.last_off_frame = 0; // a gap: the next frame has no start time
            continue;
        }
        if (s.last_off_frame && frame == s.last_off_frame + 1 && t > s.last_off_t) {
            Classify(s, frame, s.last_off_t, t - s.last_off_t, now);
        }
        s.last_off_frame = frame;
        s.last_off_t = t;
    }
    if (s.stats && now - s.w.start >= 10 * Second) {
        Print(s, now);
    }
}

} // namespace

void FrameSent(u64 frame) {
    State& s = S();
    std::scoped_lock lk{s.lock};
    Init(s);
    const s64 now = NowNs();
    s.sent[frame % s.sent.size()] = {frame, now};
    if (s.last_send_frame && frame == s.last_send_frame + 1) {
        s.intervals[s.interval_next] = now - s.last_send;
        s.interval_next = (s.interval_next + 1) % s.intervals.size();
        s.interval_count = std::min<u32>(s.interval_count + 1, s.intervals.size());
    }
    s.last_send = now;
    s.last_send_frame = frame;
    Poll(s, now);
}

void FrameReleased(u64 frame, int fence_fd, bool fifo) {
    State& s = S();
    std::scoped_lock lk{s.lock};
    Init(s);
    const s64 now = NowNs();
    if (fifo != s.fifo) {
        s.fifo = fifo;
        s.ref_valid = false;
        s.drift = 0.0;
        s.settle = 30;
    }
    const int fd = fence_fd >= 0 ? fcntl(fence_fd, F_DUPFD_CLOEXEC, 0) : -1;
    if (fd < 0) {
        s.last_off_frame = 0; // no time for this release: the chain starts again after it
    } else {
        if (s.pending.size() >= 16) {
            close(s.pending.front().fd);
            s.pending.pop_front();
            s.last_off_frame = 0;
        }
        s.pending.push_back({frame, fd, now});
    }
    Poll(s, now);
}

s64 AdjustSlot(s64 slot_ns, s64 period_ns) {
    State& s = S();
    std::scoped_lock lk{s.lock};
    if (!s.lock_on || !s.fifo || period_ns <= 0) {
        return slot_ns;
    }
    // Not while the frames come at another rate than the limiter's (frame generation, a game slower
    // than the limit): the slots are not what decides when they go.
    const s64 interval = MedianInterval(s);
    if (interval <= 0 || std::abs(double(interval - period_ns)) > 0.1 * double(period_ns)) {
        return slot_ns;
    }
    s64 adjust = 0;
    if (s.ref_valid) {
        // Later from hand-over to screen than at the reference: the slots run ahead of the display.
        // The integral learns from new measurements only (none during a load or a pause) and not
        // from far off ones; both terms stay within what a panel's clock can be off by (1% and 1.5%
        // of the period).
        const double error = s.latency_ema - s.latency_ref;
        const double limit = 0.01 * double(period_ns);
        const double near = s.refresh_ns > 0.0 ? 0.3 * s.refresh_ns : 5e6;
        if (s.fresh && std::abs(error) < near) {
            s.drift = std::clamp(s.drift + error * 0.0008, -limit, limit);
            s.fresh = false;
        }
        adjust = std::clamp<s64>(s64(s.drift + error * 0.04), -s64(1.5 * limit), s64(1.5 * limit));
    }
    if (s.pending_shift) {
        // Later slots, a shorter hand-over-to-screen time at the same drift: the reference follows.
        adjust += s.pending_shift;
        s.latency_ref -= double(s.pending_shift);
        s.pending_shift = 0;
    }
    return slot_ns + adjust;
}

void NoteThermal(int status, float headroom, int level) {
    Thermal& t = T();
    t.status.store(status);
    t.headroom.store(headroom);
    t.level.store(level);
}

} // namespace BbPacing

#else

namespace BbPacing {
void FrameSent(u64) {}
void FrameReleased(u64, int, bool) {}
s64 AdjustSlot(s64 slot_ns, s64) {
    return slot_ns;
}
void NoteThermal(int, float, int) {}
} // namespace BbPacing

#endif
