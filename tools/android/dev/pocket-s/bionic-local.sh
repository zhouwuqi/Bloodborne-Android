# AYANEO Pocket S (Snapdragon G3x Gen 2), sourced last by run-bionic.sh; tools/android/dev/pocket_s.sh puts it there.
# CPUs as on the Snapdragon 8 Gen 2: 0-2 Cortex-A510, 3-6 Cortex-A715/A710, 7 Cortex-X3 (prime).

# The GPU command processor on the prime core (the AYN Thor setting, same CPU): it finishes its bursts sooner and
# sleeps in between. The other threads keep every core (the background helpers use the idle little ones).
export BB_GPUCOMM_CPUS=7
# A/B: the whole game off the little cores.
# export BB_THOR_CPUS=3-7

# Display pacing (gpu/shim/bbport_pacing.h): a "Pacing:" line every 10 s, and the frame limiter's phase lock
# (needs bbport.ini vsync=1). BB_PACE_LOCK=0 for the comparison without it.
export BB_PACING_STATS=1 BB_PACE_LOCK=1

# Thermal governor (gpu/shim/bbport_adpf.cpp): the ADPF target at 85% of the frame budget; the whole budget from a
# headroom forecast of 0.85, the prime core left (but for the GPU command processor's pin) from 0.95.
export BB_THERMAL_GOV=1 BB_ADPF_MARGIN=0.85
