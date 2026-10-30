// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: Android Dynamic Performance Framework hints and a thermal readout (bbport_adpf.h).
//
// Work duration of a frame: the time the game's main thread and the GPU command processor actually
// wanted the CPU between two submitted frames, from /proc/self/task/<tid>/schedstat (running time
// plus run-queue wait: a thread descheduled on a busy phone is the case the hint is for; sleeping
// in the frame limiter, on a vblank or on a semaphore is neither), else from the thread's CPU
// clock. The longer of the two is reported: they work in parallel. The recorder threads spin while
// they wait for work, so their CPU time says nothing about the frame and is not measured (they are
// in the session, which is what the hint's boost applies to).
//
// The target is a share of the frame budget (BB_ADPF_MARGIN, 0.85 by default: the CPU keeps a margin
// for the heavier frames) until the thermal governor below lets it go up to the whole budget.
#include "bbport_adpf.h"
#include "bbport_pacing.h"
#include "core/emulator_settings.h"

#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <fcntl.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace BbAdpf {
namespace {
using u64 = std::uint64_t;
using i64 = std::int64_t;

struct HintManager;
struct HintSession;
struct ThermalManager;

struct Api {
    HintManager* (*get_manager)() = nullptr;
    HintSession* (*create_session)(HintManager*, const std::int32_t*, std::size_t, i64) = nullptr;
    int (*update_target)(HintSession*, i64) = nullptr;
    int (*report_actual)(HintSession*, i64) = nullptr;
    void (*close_session)(HintSession*) = nullptr;
    int (*set_threads)(HintSession*, const std::int32_t*, std::size_t) = nullptr;           // API 34
    int (*set_prefer_power_efficiency)(HintSession*, bool) = nullptr;                       // API 35
    ThermalManager* (*thermal_acquire)() = nullptr;                                         // API 30
    int (*thermal_status)(ThermalManager*) = nullptr;                                       // API 30
    float (*thermal_headroom)(ThermalManager*, int) = nullptr;                              // API 31
};

bool EnvOff(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '0';
}

/// A thread whose scheduling time is measured per frame.
struct Probe {
    int tid = -1;
    int fd = -1;
    bool valid = false;
    u64 last = 0; ///< running + waiting time at the last frame, ns
};

enum class Kind { Main, GpuComm, DrawRec, Recorder, RenderThread };

struct State {
    std::mutex lock;
    bool enabled = false;
    bool failed = false;     ///< the session cannot be made: off for good
    std::atomic<bool> dlopen_done{false};
    Api api;
    HintManager* manager = nullptr;
    HintSession* session = nullptr;
    std::vector<std::int32_t> tids;
    std::size_t session_threads = 0; ///< threads the session has now
    std::atomic<bool> dirty{false};  ///< a thread joined since the session was made
    Probe main_probe, gpu_probe;
    int recorders = 0;
    bool have_main = false, have_gpu = false, have_draw = false, have_render = false;
    i64 target_ns = 0;
    double margin = 0.85;         ///< BB_ADPF_MARGIN: the target's share of the frame budget
    std::atomic<int> level{0};    ///< the thermal governor's level (1 and up: the whole budget)
    u64 last_frame_ns = 0;
    int attempts = 0;
    u64 next_attempt_ns = 0;
    bool measure_logged = false;
    std::once_flag once;
};
State& S() {
    static State state;
    return state;
}

u64 NowNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return u64(ts.tv_sec) * 1000000000ull + u64(ts.tv_nsec);
}

/// libandroid's symbols, once. Without APerformanceHint (API < 33) the hints stay off; the thermal
/// readout needs only AThermal (API 30).
void LoadApi() {
    State& s = S();
    s.enabled = !EnvOff("BB_ADPF");
    if (!s.enabled) {
        return;
    }
    if (const char* v = std::getenv("BB_ADPF_MARGIN"); v && *v) {
        s.margin = std::clamp(std::strtod(v, nullptr), 0.5, 1.0);
    }
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) {
        s.enabled = false;
        return;
    }
    const auto sym = [&](const char* name) { return dlsym(lib, name); };
    Api& a = s.api;
    a.get_manager = reinterpret_cast<decltype(a.get_manager)>(sym("APerformanceHint_getManager"));
    a.create_session =
        reinterpret_cast<decltype(a.create_session)>(sym("APerformanceHint_createSession"));
    a.update_target = reinterpret_cast<decltype(a.update_target)>(
        sym("APerformanceHint_updateTargetWorkDuration"));
    a.report_actual = reinterpret_cast<decltype(a.report_actual)>(
        sym("APerformanceHint_reportActualWorkDuration"));
    a.close_session =
        reinterpret_cast<decltype(a.close_session)>(sym("APerformanceHint_closeSession"));
    a.set_threads = reinterpret_cast<decltype(a.set_threads)>(sym("APerformanceHint_setThreads"));
    a.set_prefer_power_efficiency = reinterpret_cast<decltype(a.set_prefer_power_efficiency)>(
        sym("APerformanceHint_setPreferPowerEfficiency"));
    a.thermal_acquire =
        reinterpret_cast<decltype(a.thermal_acquire)>(sym("AThermal_acquireManager"));
    a.thermal_status =
        reinterpret_cast<decltype(a.thermal_status)>(sym("AThermal_getCurrentThermalStatus"));
    a.thermal_headroom =
        reinterpret_cast<decltype(a.thermal_headroom)>(sym("AThermal_getThermalHeadroom"));
    s.dlopen_done.store(true, std::memory_order_release);
}

void Init() {
    std::call_once(S().once, LoadApi);
}

bool HintsAvailable(const Api& a) {
    return a.get_manager && a.create_session && a.update_target && a.report_actual &&
           a.close_session;
}

i64 TargetNs() {
    const State& s = S();
    const unsigned limit = EmulatorSettings.GetFrameLimit();
    const i64 budget = 1000000000ll / (limit ? limit : 60);
    return s.level.load(std::memory_order_relaxed) >= 1 ? budget : i64(double(budget) * s.margin);
}

void OpenProbe(Probe& p, int tid) {
    p.tid = tid;
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/schedstat", tid);
    p.fd = open(path, O_RDONLY | O_CLOEXEC);
    p.valid = false;
}

/// The thread's running + run-queue waiting time, ns; false when it cannot be read.
bool Sample(Probe& p, u64& total) {
    if (p.tid < 0) {
        return false;
    }
    if (p.fd >= 0) {
        char buf[96];
        const ssize_t n = pread(p.fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            char* end = nullptr;
            const u64 run = std::strtoull(buf, &end, 10);
            const u64 wait = end ? std::strtoull(end, nullptr, 10) : 0;
            total = run + wait;
            return true;
        }
        close(p.fd);
        p.fd = -1;
    }
    // CPU-time clock of the thread: the clock id the kernel gives (~tid << 3 | SCHED | PERTHREAD).
    timespec ts;
    const clockid_t clock = clockid_t((~static_cast<unsigned>(p.tid)) << 3) | 6;
    if (clock_gettime(clock, &ts) == 0) {
        total = u64(ts.tv_sec) * 1000000000ull + u64(ts.tv_nsec);
        return true;
    }
    p.tid = -1;
    return false;
}

/// The session with every thread known now (State::lock held): made, or its threads replaced.
void EnsureSession() {
    State& s = S();
    if (s.failed || !s.enabled || s.tids.empty()) {
        return;
    }
    const u64 now = NowNs();
    if (!s.session) {
        if (now < s.next_attempt_ns) {
            return;
        }
        s.next_attempt_ns = now + 5000000000ull;
        ++s.attempts;
        s.target_ns = TargetNs();
        const char* why = nullptr;
        if (!HintsAvailable(s.api)) {
            why = "no APerformanceHint in libandroid.so (Android below 13)";
            s.attempts = 99;
        } else {
            if (!s.manager) {
                s.manager = s.api.get_manager();
            }
            if (!s.manager) {
                why = "APerformanceHint_getManager returned null";
            } else if (!(s.session = s.api.create_session(s.manager, s.tids.data(), s.tids.size(),
                                                           s.target_ns))) {
                why = "APerformanceHint_createSession returned null";
            }
        }
        if (!s.session) {
            if (s.attempts >= 3) {
                s.failed = true;
                std::printf("ADPF: off (%s)\n", why);
            }
            return;
        }
        s.session_threads = s.tids.size();
        if (s.api.set_prefer_power_efficiency) {
            s.api.set_prefer_power_efficiency(s.session, false);
        }
        std::printf("ADPF: session with %zu threads, target %.1f ms\n", s.session_threads,
                    double(s.target_ns) / 1e6);
        return;
    }
    // A thread joined later: replace the session's threads (API 34), else make the session again.
    if (s.tids.size() == s.session_threads) {
        return;
    }
    if (s.api.set_threads) {
        if (s.api.set_threads(s.session, s.tids.data(), s.tids.size()) != 0) {
            std::printf("ADPF: APerformanceHint_setThreads failed with %zu threads\n",
                        s.tids.size());
            return;
        }
    } else {
        HintSession* fresh = s.api.create_session(s.manager, s.tids.data(), s.tids.size(),
                                                  s.target_ns);
        if (!fresh) {
            std::printf("ADPF: the session could not be made again with %zu threads\n",
                        s.tids.size());
            return;
        }
        s.api.close_session(s.session);
        s.session = fresh;
        if (s.api.set_prefer_power_efficiency) {
            s.api.set_prefer_power_efficiency(s.session, false);
        }
    }
    s.session_threads = s.tids.size();
    std::printf("ADPF: session with %zu threads, target %.1f ms\n", s.session_threads,
                double(s.target_ns) / 1e6);
}

void Add(std::int32_t tid) {
    State& s = S();
    if (std::find(s.tids.begin(), s.tids.end(), tid) == s.tids.end() && s.tids.size() < 8) {
        s.tids.push_back(tid);
        s.dirty.store(true, std::memory_order_release);
    }
}

// ---- Thermal ----
const char* ThermalName(int status) {
    static const char* const names[] = {"none", "light", "moderate", "severe",
                                        "critical", "emergency", "shutdown"};
    return status >= 0 && status < 7 ? names[status] : "unknown";
}

// ---- Thermal governor (BB_THERMAL_GOV=0: off) ----
// Level 0: the hint's target keeps its margin (above). Level 1, from a headroom forecast (10 s) of
// 0.85: the whole frame budget, the clocks only as fast as the frame needs. Level 2, from 0.95 (1.0 is
// where the device starts to throttle): the game's threads also leave the prime core(s), a Cortex-X3
// class core at its top clock draws several times the power of a big core. A level is left after 30 s
// at least 0.10 below the headroom that started it. Without a headroom figure the status counts
// (light: level 1, moderate and up: level 2).
constexpr float kLevelFrom[3] = {0.0f, 0.85f, 0.95f};

struct Governor {
    bool on = false;
    int level = 0;
    u64 below_since = 0;
    bool can_park = false;
    cpu_set_t base;   ///< the process's CPUs at the start
    cpu_set_t parked; ///< base without the prime cores
};

bool ReadLong(const char* path, long& value) {
    FILE* f = std::fopen(path, "re");
    if (!f) {
        return false;
    }
    const bool ok = std::fscanf(f, "%ld", &value) == 1;
    std::fclose(f);
    return ok;
}

/// The prime cores of `set`: the fastest (cpu_capacity, else cpuinfo_max_freq) when there are at most
/// two of them and they are at least 10% faster than the next ones.
bool PrimeCpus(const cpu_set_t& set, cpu_set_t& prime) {
    long speed[64] = {};
    long top = 0;
    for (int cpu = 0; cpu < 64; ++cpu) {
        if (!CPU_ISSET(cpu, &set)) {
            continue;
        }
        char path[96];
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", cpu);
        if (!ReadLong(path, speed[cpu])) {
            std::snprintf(path, sizeof(path),
                          "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
            ReadLong(path, speed[cpu]);
        }
        top = std::max(top, speed[cpu]);
    }
    long next = 0;
    int count = 0;
    CPU_ZERO(&prime);
    for (int cpu = 0; cpu < 64; ++cpu) {
        if (!CPU_ISSET(cpu, &set) || speed[cpu] <= 0) {
            continue;
        }
        if (speed[cpu] == top) {
            ++count;
            CPU_SET(cpu, &prime);
        } else {
            next = std::max(next, speed[cpu]);
        }
    }
    return top > 0 && next > 0 && count <= 2 && top * 10 >= next * 11;
}

/// Every thread of the process runs on `set`; the GPU command processor keeps its own pin when it
/// has one (BB_GPUCOMM_CPUS). `changed_only`: leaves threads that already have it. Returns the
/// threads changed.
int ApplyCpus(const cpu_set_t& set, bool changed_only) {
    const char* pin = std::getenv("BB_GPUCOMM_CPUS");
    const bool keep_gpucomm = pin && *pin;
    DIR* dir = opendir("/proc/self/task");
    if (!dir) {
        return 0;
    }
    int changed = 0;
    while (const dirent* entry = readdir(dir)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
            continue;
        }
        const pid_t tid = pid_t(std::atoi(entry->d_name));
        if (keep_gpucomm) {
            char path[64], name[32] = {};
            std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", int(tid));
            const int fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                const ssize_t n = read(fd, name, sizeof(name) - 1);
                close(fd);
                if (n > 0 && !std::strncmp(name, "shadPS4:GpuComm", 15)) {
                    continue;
                }
            }
        }
        cpu_set_t current;
        if (changed_only && sched_getaffinity(tid, sizeof(current), &current) == 0 &&
            CPU_EQUAL(&current, &set)) {
            continue;
        }
        if (sched_setaffinity(tid, sizeof(set), &set) == 0) {
            ++changed;
        }
    }
    closedir(dir);
    return changed;
}

void StartGovernor(Governor& g) {
    g.on = !EnvOff("BB_THERMAL_GOV");
    if (!g.on) {
        std::printf("Thermal governor: off (BB_THERMAL_GOV=0)\n");
        return;
    }
    CPU_ZERO(&g.base);
    CPU_ZERO(&g.parked);
    cpu_set_t prime;
    if (sched_getaffinity(0, sizeof(g.base), &g.base) == 0 && PrimeCpus(g.base, prime)) {
        CPU_XOR(&g.parked, &g.base, &prime);
        g.can_park = CPU_COUNT(&g.parked) > 0;
    }
    std::string cores;
    for (int cpu = 0; cpu < 64 && g.can_park; ++cpu) {
        if (CPU_ISSET(cpu, &g.base) && !CPU_ISSET(cpu, &g.parked)) {
            cores += (cores.empty() ? "" : ",") + std::to_string(cpu);
        }
    }
    std::printf("Thermal governor: on; ADPF target %.0f%% of the frame budget, the whole budget from "
                "a headroom forecast of %.2f, %s from %.2f\n",
                S().margin * 100.0, double(kLevelFrom[1]),
                g.can_park ? ("prime core(s) " + cores + " left").c_str() : "no prime core to leave",
                double(kLevelFrom[2]));
}

void SetLevel(Governor& g, int level, float headroom) {
    const int old = g.level;
    g.level = level;
    S().level.store(level, std::memory_order_relaxed); // the next frame passes the target on
    int moved = 0;
    if (g.can_park && level >= 2 && old < 2) {
        moved = ApplyCpus(g.parked, false);
    } else if (g.can_park && level < 2 && old >= 2) {
        moved = ApplyCpus(g.base, false);
    }
    std::printf("Thermal governor: level %d (headroom %.2f): ADPF target %.1f ms%s", level,
                double(headroom), double(TargetNs()) / 1e6,
                !g.can_park ? "" : level >= 2 ? ", prime core(s) left" : ", all cores");
    if (moved) {
        std::printf(" (%d threads moved)", moved);
    }
    std::printf("\n");
    std::fflush(stdout);
}

void StepGovernor(Governor& g, int status, float headroom) {
    float h = headroom;
    if (!std::isfinite(h)) {
        h = status >= 2 ? kLevelFrom[2] : status == 1 ? kLevelFrom[1] : 0.0f;
    }
    int want = g.level;
    if (h >= kLevelFrom[2]) {
        want = 2;
    } else if (h >= kLevelFrom[1]) {
        want = std::max(want, 1);
    }
    if (want > g.level) {
        g.below_since = 0;
        SetLevel(g, want, h);
        return;
    }
    if (g.level == 0 || h >= kLevelFrom[g.level] - 0.10f) {
        g.below_since = 0;
        return;
    }
    const u64 now = NowNs();
    if (!g.below_since) {
        g.below_since = now;
    } else if (now - g.below_since >= 30000000000ull) {
        g.below_since = 0;
        SetLevel(g, g.level - 1, h);
    }
}

void ThermalLoop() {
    State& s = S();
    pthread_setname_np(pthread_self(), "bb:Thermal");
    ThermalManager* manager = s.api.thermal_acquire();
    if (!manager) {
        std::printf("Thermal: off (AThermal_acquireManager returned null)\n");
        return;
    }
    Governor g;
    StartGovernor(g);
    int last = -2;
    while (true) {
        const int status = s.api.thermal_status(manager);
        // The headroom (API 31), forecast 10 s ahead: 1.0 is where the device starts to throttle;
        // NaN: unknown (and at most one call a second gives a fresh value).
        const float headroom = s.api.thermal_headroom ? s.api.thermal_headroom(manager, 10) : NAN;
        if (status != last) {
            last = status;
            if (std::isfinite(headroom)) {
                std::printf("Thermal: status %s (%d), headroom %.2f\n", ThermalName(status), status,
                            double(headroom));
            } else {
                std::printf("Thermal: status %s (%d), headroom unknown\n", ThermalName(status),
                            status);
            }
            std::fflush(stdout);
        }
        if (g.on) {
            StepGovernor(g, status, headroom);
        }
        BbPacing::NoteThermal(status, headroom, g.level);
        if (g.level >= 2 && g.can_park) {
            // Android gives the threads the cpuset's CPUs again when it moves the app between
            // cpusets: look again ten times a second while the prime cores are left.
            for (int i = 0; i < 20; ++i) {
                ApplyCpus(g.parked, true);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        } else {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
}

void StartThermal() {
    State& s = S();
    static std::once_flag started;
    std::call_once(started, [&] {
        if (!s.enabled || EnvOff("BB_THERMAL_LOG")) {
            return;
        }
        if (!s.api.thermal_acquire || !s.api.thermal_status) {
            std::printf("Thermal: off (no AThermal in libandroid.so)\n");
            return;
        }
        std::thread(ThermalLoop).detach();
    });
}
} // namespace

void RegisterMainThread() {
    Init();
    State& s = S();
    if (!s.enabled) {
        return;
    }
    {
        std::scoped_lock lk{s.lock};
        if (!s.have_main) {
            s.have_main = true;
            const int tid = int(gettid());
            Add(tid);
            OpenProbe(s.main_probe, tid);
        }
    }
    StartThermal();
}

void NoteThreadName(const char* name) {
    if (!name) {
        return;
    }
    // Cheap rejection before anything is loaded: only these names matter.
    const bool match = !std::strncmp(name, "shadPS4:GpuComm", 15) || !std::strcmp(name, "bb:DrawRec") ||
                       !std::strncmp(name, "bb:VkRecorder", 13) || !std::strncmp(name, "GXRender", 8);
    if (!match) {
        return;
    }
    Init();
    State& s = S();
    if (!s.enabled) {
        return;
    }
    const int tid = int(gettid());
    std::scoped_lock lk{s.lock};
    if (!std::strncmp(name, "shadPS4:GpuComm", 15)) {
        if (!s.have_gpu) {
            s.have_gpu = true;
            Add(tid);
            OpenProbe(s.gpu_probe, tid);
        }
    } else if (!std::strcmp(name, "bb:DrawRec")) {
        if (!s.have_draw) {
            s.have_draw = true;
            Add(tid);
        }
    } else if (!std::strncmp(name, "bb:VkRecorder", 13)) {
        if (s.recorders < 2) { // the first two workers
            ++s.recorders;
            Add(tid);
        }
    } else if (!s.have_render) {
        s.have_render = true;
        Add(tid);
    }
}

void FrameSubmitted() {
    State& s = S();
    if (!s.dlopen_done.load(std::memory_order_acquire) || !s.enabled || s.failed) {
        return;
    }
    std::scoped_lock lk{s.lock};
    const u64 now = NowNs();
    if (!s.session || s.dirty.exchange(false, std::memory_order_acquire)) {
        EnsureSession();
        if (!s.session) {
            return;
        }
    }
    // The thread times at this frame, whether or not it is reported (the next frame's baseline).
    u64 work = 0;
    bool measured = false;
    for (Probe* p : {&s.main_probe, &s.gpu_probe}) {
        u64 total;
        if (!Sample(*p, total)) {
            continue;
        }
        if (p->valid && total >= p->last) {
            work = std::max(work, total - p->last);
            measured = true;
        }
        p->last = total;
        p->valid = true;
    }
    const u64 interval = s.last_frame_ns ? now - s.last_frame_ns : 0;
    s.last_frame_ns = now;
    // The target follows the frame limit (a change of it is passed on at once).
    const i64 target = TargetNs();
    if (target != s.target_ns) {
        s.target_ns = target;
        s.api.update_target(s.session, target);
    }
    // Not a frame of play: the first, a pause or a load (over a second), a doubled flip.
    if (!measured || interval < 500000ull || interval > 1000000000ull || work == 0) {
        return;
    }
    work = std::min(work, interval);
    s.api.report_actual(s.session, i64(work));
}
} // namespace BbAdpf

extern "C" void bbgpu_adpf_register_main(void) {
    BbAdpf::RegisterMainThread();
}

extern "C" void bbgpu_adpf_thread_named(const char* name) {
    BbAdpf::NoteThreadName(name);
}
