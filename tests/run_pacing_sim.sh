#!/usr/bin/env bash
# Builds and runs tests/test_pacing_sim.cpp on a Linux PC: gpu/shim/bbport_pacing.cpp and the frame limiter's
# slot rule against a simulated display (real time, the runs side by side; the release fences are simulated, see
# the test). Each line: the share of frames not on screen for exactly two refreshes at 30 FPS, without and with
# the phase lock. The cases marked "must" fail the script when the lock leaves more than EXPECT_MAX (1.0) %.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
mkdir -p out/pacing-sim
"${CXX:-c++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -Igpu/shim -Igpu/shadps4 -Wl,--wrap=ioctl \
    tests/test_pacing_sim.cpp gpu/shim/bbport_pacing.cpp -o out/pacing-sim-test
s=${SECONDS_PER_RUN:-40}
cases=(
    "must|59.8 Hz|--hz 59.8"
    "must|60.3 Hz|--hz 60.3"
    "must|59.8 Hz, first slot 12 ms in|--hz 59.8 --phase-ms 12"
    "must|59.8 Hz, GPU 0.5-7 ms|--hz 59.8 --gpu-ms 0.5 7"
    "-|59.8 Hz, GPU 0.5-12 ms|--hz 59.8 --gpu-ms 0.5 12"
    "-|59.8 Hz, 2% of frames 10 ms late|--hz 59.8 --late 2 10"
    "-|59.8 Hz, 5% of frames 4 ms late|--hz 59.8 --late 5 4"
)
for i in "${!cases[@]}"; do
    IFS='|' read -r _ _ args <<< "${cases[$i]}"
    for lock in 0 1; do
        # shellcheck disable=SC2086
        BB_PACE_LOCK=$lock BB_PACING_STATS=0 out/pacing-sim-test --seconds "$s" $args > "out/pacing-sim/$i-$lock.txt" 2>&1 &
    done
done
BB_PACE_LOCK=0 BB_PACING_STATS=0 out/pacing-sim-test --seconds "$s" --hz 59.8 --from-wake > out/pacing-sim/wake.txt 2>&1 &
wait
pct() { sed -n 's/.*(\([0-9.]*\)%)$/\1/p' "$1" | tail -n 1; }
printf '%-36s %10s %10s\n' "case" "no lock" "lock"
printf '%-36s %9s%% %10s\n' "59.8 Hz, 2.2.0's limiter" "$(pct out/pacing-sim/wake.txt)" "-"
failed=0
for i in "${!cases[@]}"; do
    IFS='|' read -r must label _ <<< "${cases[$i]}"
    off=$(pct "out/pacing-sim/$i-0.txt"); on=$(pct "out/pacing-sim/$i-1.txt")
    mark=""
    if [[ $must == must ]] && awk -v v="$on" -v m="${EXPECT_MAX:-1.0}" 'BEGIN { exit !(v > m) }'; then
        mark="  FAIL (more than ${EXPECT_MAX:-1.0}%)"; failed=1
    fi
    printf '%-36s %9s%% %9s%%%s\n' "$label" "$off" "$on" "$mark"
done
exit $failed
