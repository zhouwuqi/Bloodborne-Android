#!/usr/bin/env bash
# AYANEO Pocket S (Snapdragon G3x Gen 2) tuning over adb, on top of the installed app (com.ducvd89.bloodborne,
# whose APK is debuggable: run-as reaches its files). Nothing in the app is reinstalled; the game, the saves and
# the native code stay. An app update unpacks its own runtime again and so undoes `push`.
#
#   pocket_s.sh push       this checkout's libbbgpu.so (tools/android/dev/build_libbbgpu.sh) over the app's,
#                          the first time keeping the app's own as libbbgpu.so.official; and bionic-local.sh
#   pocket_s.sh profile    only bionic-local.sh (CPU placement, pacing and thermal switches; no new library)
#   pocket_s.sh settings   the 30 FPS settings into bbport.ini (a copy of it first: bbport.ini.before-pocket-s)
#   pocket_s.sh restore    the app's own libbbgpu.so and bbport.ini back, bionic-local.sh removed
#   pocket_s.sh log [N]    the game log of the last run (N=prev: the one before), with a pacing summary
#
# ADB_SERIAL selects the device when more than one is attached.
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."
pkg=com.ducvd89.bloodborne
lib=files/bbport/bionic/lib
here=tools/android/dev
adb_() { adb ${ADB_SERIAL:+-s "$ADB_SERIAL"} "$@"; }
as_app() { adb_ shell "run-as $pkg sh -c '$1'"; }
# Copies a local file into the app's files: through /data/local/tmp, which the app's uid can read.
put() { # local remote(relative to the app's data dir)
    local tmp=/data/local/tmp/bb-$(basename "$2")
    adb_ push "$1" "$tmp" >/dev/null
    adb_ shell "chmod 644 $tmp && run-as $pkg sh -c 'cp $tmp $2.new && mv $2.new $2' ; rm -f $tmp"
}

check() {
    adb_ get-state >/dev/null 2>&1 || { echo "No device (adb devices)" >&2; exit 1; }
    as_app "test -d $lib" || { echo "The app's runtime is not there yet: start the app once" >&2; exit 1; }
}

profile() {
    put "$here/pocket-s/bionic-local.sh" files/bbport/bionic-local.sh
    echo "bionic-local.sh: on the phone (applies at the next game start)"
}

case ${1:-} in
push)
    check
    so=out/bionic-dev/gpu/libbbgpu.so
    [[ -f $so ]] || { echo "No $so: tools/android/dev/build_libbbgpu.sh first" >&2; exit 1; }
    as_app "test -f $lib/libbbgpu.so.official || cp $lib/libbbgpu.so $lib/libbbgpu.so.official"
    put "$so" "$lib/libbbgpu.so"
    echo "libbbgpu.so: $(git rev-parse --short HEAD)$(git diff --quiet || echo +changes) on the phone"
    profile
    ;;
profile)
    check
    profile
    ;;
settings)
    check
    ini=files/bbport/bbport.ini
    as_app "test -f $ini.before-pocket-s || cp $ini $ini.before-pocket-s"
    # One key a line, the last line wins (run-bionic.sh reads the last value): appended, the rest kept.
    adb_ shell "run-as $pkg sh -c 'cat >> $ini'" < "$here/pocket-s/bbport-30fps.ini"
    echo "bbport.ini: the 30 FPS settings appended (the app's settings screen shows them; the copy: $ini.before-pocket-s)"
    ;;
restore)
    check
    as_app "if test -f $lib/libbbgpu.so.official; then mv $lib/libbbgpu.so.official $lib/libbbgpu.so; fi; \
rm -f files/bbport/bionic-local.sh; \
if test -f files/bbport/bbport.ini.before-pocket-s; then mv files/bbport/bbport.ini.before-pocket-s files/bbport/bbport.ini; fi"
    echo "Restored: the app's own libbbgpu.so and settings, no bionic-local.sh"
    ;;
log)
    check
    name=android-game.log
    [[ ${2:-} == prev ]] && name=android-game.prev.log
    mkdir -p out/pocket-s
    dest=out/pocket-s/$(date +%Y%m%d-%H%M%S)-$name
    adb_ exec-out run-as $pkg cat "files/bbport/logs/$name" > "$dest"
    echo "$dest"
    python3 "$here/pacing_report.py" "$dest"
    ;;
*)
    sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
