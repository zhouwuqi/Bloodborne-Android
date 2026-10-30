// SPDX-License-Identifier: GPL-2.0-or-later
// The thermal governor of gpu/shim/bbport_adpf.cpp on a Linux PC (the file is included: the governor is
// in its anonymous namespace): the levels a headroom sequence gives, their hysteresis, the ADPF target
// per level, and moving every thread of the process off a CPU and back (ApplyCpus).
//   c++ -std=c++20 -pthread -Igpu/shim -Igpu/shadps4 tests/test_thermal_governor.cpp gpu/shim/bbport_pacing.cpp
#include "../gpu/shim/bbport_adpf.cpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}
} // namespace

int main() {
    using namespace BbAdpf;
    setenv("BB_FPS_LIMIT", "30", 1);
    const i64 budget = 1000000000ll / 30;

    Governor g;
    g.on = true; // no CPUs to leave: levels and targets only
    StepGovernor(g, 0, 0.50f);
    Check(g.level == 0 && TargetNs() == i64(double(budget) * 0.85), "cool: level 0, target 85% of the budget");
    StepGovernor(g, 0, 0.86f);
    Check(g.level == 1 && TargetNs() == budget, "headroom 0.86: level 1, the whole budget");
    StepGovernor(g, 0, 0.97f);
    Check(g.level == 2, "headroom 0.97: level 2");
    StepGovernor(g, 0, 0.80f);
    Check(g.level == 2, "0.80 is not 0.10 below 0.95: stays at 2");
    StepGovernor(g, 0, 0.70f);
    Check(g.level == 2 && g.below_since != 0, "0.70: the 30 s hold starts, still 2");
    g.below_since = NowNs() - 31000000000ull;
    StepGovernor(g, 0, 0.70f);
    Check(g.level == 1, "after 30 s below: one level down, to 1");
    StepGovernor(g, 0, 0.80f);
    Check(g.level == 1 && g.below_since == 0, "0.80 is not 0.10 below 0.85: stays at 1, the hold ends");
    StepGovernor(g, 0, 0.60f);
    Check(g.level == 1 && g.below_since != 0, "a fresh hold: a single sample below does not step down");
    g.below_since = NowNs() - 31000000000ull;
    StepGovernor(g, 0, 0.60f);
    Check(g.level == 0 && TargetNs() == i64(double(budget) * 0.85), "then level 0 and the margin again");
    StepGovernor(g, 1, NAN);
    Check(g.level == 1, "no headroom figure, status light: level 1");
    StepGovernor(g, 2, NAN);
    Check(g.level == 2, "no headroom figure, status moderate: level 2");

    // Moving the threads: off the first CPU of the set and back.
    cpu_set_t base;
    CPU_ZERO(&base);
    sched_getaffinity(0, sizeof(base), &base);
    if (CPU_COUNT(&base) < 2) {
        std::printf("skip: one CPU only\n");
        return failures ? 1 : 0;
    }
    int first = 0;
    while (!CPU_ISSET(first, &base)) {
        ++first;
    }
    cpu_set_t parked = base;
    CPU_CLR(first, &parked);
    std::atomic<bool> stop{false};
    std::atomic<int> worker_tid{0};
    std::thread worker([&] {
        worker_tid = int(gettid());
        while (!stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    while (!worker_tid) {
        std::this_thread::yield();
    }
    const int moved = ApplyCpus(parked, false);
    cpu_set_t now;
    sched_getaffinity(worker_tid, sizeof(now), &now);
    Check(moved >= 2 && CPU_EQUAL(&now, &parked), "ApplyCpus: every thread off the first CPU");
    Check(ApplyCpus(parked, true) == 0, "ApplyCpus with changed_only: nothing left to change");
    ApplyCpus(base, false);
    sched_getaffinity(worker_tid, sizeof(now), &now);
    Check(CPU_EQUAL(&now, &base), "ApplyCpus: back on every CPU");
    stop = true;
    worker.join();

    cpu_set_t prime;
    std::printf("this PC: %s\n", PrimeCpus(base, prime) ? "a prime core to leave" : "no prime core (all alike)");
    return failures ? 1 : 0;
}
