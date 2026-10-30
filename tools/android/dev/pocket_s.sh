#!/usr/bin/env bash
# AYANEO Pocket S (Snapdragon G3x Gen 2) helpers over adb, for the Pocket S build that installs beside the official
# app (tools/android/dev/build_coexist_apk.sh: io.github.zhouwuqi.bloodborne) or, with BB_PKG=com.ducvd89.bloodborne,
# for the official app itself. Both APKs are debuggable, so run-as reaches their files: nothing is reinstalled.
#
#   pocket_s.sh migrate    from the official app into the Pocket S one: the native code (no rebuild), the saves, the
#                          settings, the mods list and the chosen game folder (the game itself stays where it is and
#                          serves both); both apps are stopped first
#   pocket_s.sh push       this checkout's libbbgpu.so (tools/android/dev/build_libbbgpu.sh) over the app's, the first
#                          time keeping the app's own as libbbgpu.so.official; and bionic-local.sh
#   pocket_s.sh profile    only bionic-local.sh (CPU placement, pacing and thermal switches; no new library)
#   pocket_s.sh settings   the 30 FPS settings into bbport.ini (a copy of it first: bbport.ini.before-pocket-s)
#   pocket_s.sh restore    the app's own libbbgpu.so and bbport.ini back, bionic-local.sh removed
#   pocket_s.sh log [prev] the game log of the last run (prev: the one before), with a pacing summary
#
# ADB_SERIAL selects the device when more than one is attached; ADB the adb binary.
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."
pkg=${BB_PKG:-io.github.zhouwuqi.bloodborne}
official=com.ducvd89.bloodborne
lib=files/bbport/bionic/lib
here=tools/android/dev
adb=${ADB:-$(command -v adb || echo "${ANDROID_HOME:-/opt/android-sdk}/platform-tools/adb")}
adb_() { "$adb" ${ADB_SERIAL:+-s "$ADB_SERIAL"} "$@"; }
as_app() { adb_ shell "run-as $pkg sh -c '$1'"; }
# Copies a local file into the app's files: through /data/local/tmp, which the app's uid can read.
put() { # local remote(relative to the app's data dir)
    local tmp=/data/local/tmp/bb-$(basename "$2")
    adb_ push "$1" "$tmp" >/dev/null
    adb_ shell "chmod 644 $tmp && run-as $pkg sh -c 'cp $tmp $2.new && mv $2.new $2' ; rm -f $tmp"
}

device() {
    adb_ get-state >/dev/null 2>&1 || { echo "No device (adb devices)" >&2; exit 1; }
}
check() {
    device
    as_app "test -d $lib" || { echo "$pkg: not installed, or its runtime is not there yet (start a game once)" >&2; exit 1; }
}

profile() {
    put "$here/pocket-s/bionic-local.sh" files/bbport/bionic-local.sh
    echo "bionic-local.sh: on the phone (applies at the next game start)"
}

case ${1:-} in
migrate)
    device
    [[ $pkg != "$official" ]] || { echo "migrate copies from $official into the Pocket S app: unset BB_PKG" >&2; exit 1; }
    adb_ shell "run-as $official true" 2>/dev/null || { echo "$official is not installed" >&2; exit 1; }
    adb_ shell "run-as $pkg true" 2>/dev/null || { echo "$pkg is not installed: install the Pocket S APK first" >&2; exit 1; }
    adb_ shell am force-stop "$official"
    adb_ shell am force-stop "$pkg"
    items=""
    for item in files/bbport/aot files/bbport/user files/bbport/bbport.ini files/bbport/mods.json shared_prefs/bloodborne.xml; do
        if adb_ shell "run-as $official test -e $item"; then items="$items $item"; fi
    done
    [[ -n $items ]] || { echo "Nothing to copy in $official" >&2; exit 1; }
    adb_ shell "run-as $pkg mkdir -p files/bbport shared_prefs"
    # Straight from one app's data to the other's (run-as starts in the app's data directory).
    adb_ exec-out "run-as $official tar -cf - $items" | adb_ shell "run-as $pkg tar -xf -"
    # The game folder is in shared storage: the app reads it with the storage permission (legacy storage).
    adb_ shell pm grant "$pkg" android.permission.READ_EXTERNAL_STORAGE 2>/dev/null || true
    adb_ shell pm grant "$pkg" android.permission.WRITE_EXTERNAL_STORAGE 2>/dev/null || true
    echo "Copied into $pkg:$items"
    game=$(adb_ shell "run-as $pkg cat shared_prefs/bloodborne.xml" 2>/dev/null | sed -n 's/.*name="game_dir">\([^<]*\)<.*/\1/p')
    if [[ -n $game ]]; then
        echo "Game folder (shared by both apps): $game"
        case $game in /data/*) echo "  It is inside an app's own storage: choose or install the game again in the Pocket S app" ;; esac
    else
        echo "No game folder chosen in $official: choose it in the Pocket S app"
    fi
    ;;
push)
    check
    so=out/bionic-dev/libbbgpu.so
    [[ -f $so ]] || { echo "No $so: tools/android/dev/build_libbbgpu.sh first" >&2; exit 1; }
    as_app "test -f $lib/libbbgpu.so.official || cp $lib/libbbgpu.so $lib/libbbgpu.so.official"
    put "$so" "$lib/libbbgpu.so"
    echo "libbbgpu.so: $(git rev-parse --short HEAD)$(git diff --quiet -- gpu || echo +changes) in $pkg"
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
    echo "Restored in $pkg: the app's own libbbgpu.so and settings, no bionic-local.sh"
    ;;
log)
    check
    name=android-game.log
    [[ ${2:-} == prev ]] && name=android-game.prev.log
    mkdir -p out/pocket-s
    dest=out/pocket-s/$(date +%Y%m%d-%H%M%S)-$name
    adb_ exec-out run-as "$pkg" cat "files/bbport/logs/$name" > "$dest"
    echo "$dest"
    python3 -I "$here/pacing_report.py" "$dest"
    ;;
*)
    sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
