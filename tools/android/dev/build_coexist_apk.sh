#!/usr/bin/env bash
# An APK that installs beside the official app: the official release APK with this checkout's libbbgpu.so in its
# runtime, its own package name (its own data: the game folder, native code and saves are set up again in it),
# its own label, and the app update check and "Report a problem" pointed at this fork instead of upstream (an
# update offer would install the official package; a report of a modified build does not belong upstream). The
# GPU drivers, the map packs and the Rebuild Tool still come from upstream. Everything else is the official APK.
#
#   build_coexist_apk.sh [VERSION]   VERSION: the official release to start from (default: android-2.2.0)
#
# Needs: tools/android/dev/build_libbbgpu.sh run first (out/bionic-dev/libbbgpu.so), gh (the release download),
# Java 17, the SDK build-tools (zipalign, apksigner). Apktool is downloaded once into .local-deps/tools.
# The signing key is made once in .local-deps/keys and must stay: an installed copy only takes updates signed
# with the same key. Output: out/release/Bloodborne-PocketS-<version>.apk (+ .sha256).
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."
tag=${1:-android-2.2.0}
pkg=io.github.zhouwuqi.bloodborne
label="Bloodborne PS"
fork=zhouwuqi/Bloodborne-Android
lib=out/bionic-dev/libbbgpu.so
tools=$PWD/.local-deps/tools
keys=$PWD/.local-deps/keys
bt=${BB_BUILD_TOOLS:-$(ls -d "${ANDROID_HOME:-/opt/android-sdk}"/build-tools/* | sort -V | tail -n 1)}
work=out/coexist
[[ -f $lib ]] || { echo "No $lib: tools/android/dev/build_libbbgpu.sh first" >&2; exit 1; }
[[ -x $bt/zipalign && -x $bt/apksigner ]] || { echo "No zipalign/apksigner in $bt (BB_BUILD_TOOLS)" >&2; exit 1; }

mkdir -p "$tools" "$keys" out/release
apktool=$tools/apktool_3.0.3.jar
[[ -f $apktool ]] || curl -fsSL -o "$apktool" https://github.com/iBotPeaches/Apktool/releases/download/v3.0.3/apktool_3.0.3.jar

rm -rf "$work" && mkdir -p "$work/release"
gh release download "$tag" -R ducvd89/Bloodborne-Android -p 'Bloodborne-*.apk' -p 'Bloodborne-*.apk.sha256' -D "$work/release"
(cd "$work/release" && sha256sum -c --quiet ./*.sha256)
official=$(ls "$work"/release/Bloodborne-*.apk)
java -jar "$apktool" d -q -f -o "$work/dec" "$official"
dec=$work/dec
version=$(sed -n 's/^  versionName: //p' "$dec/apktool.yml")

# The runtime: this checkout's libbbgpu.so in place of the official one, the same entries in the same order and
# modes, and the digest RuntimeInstaller checks it against.
mkdir -p "$work/runtime"
tar -xzf "$dec/assets/runtime.payload" -C "$work/runtime"
tar -tzf "$dec/assets/runtime.payload" > "$work/runtime.list"
cp "$lib" "$work/runtime/bbport/bionic/lib/libbbgpu.so"
chmod 755 "$work/runtime/bbport/bionic/lib/libbbgpu.so"
sed -i "s#/data/data/com.ducvd89.bloodborne/files#/data/data/$pkg/files#" "$work/runtime/bbport/run-bionic.sh"
tar --numeric-owner --owner=0 --group=0 --no-recursion -C "$work/runtime" -T "$work/runtime.list" -cf - \
    | gzip -n -9 > "$dec/assets/runtime.payload"
sha256sum "$dec/assets/runtime.payload" | cut -c1-64 > "$dec/assets/runtime.sha256"

# The package, the label and the two names that must be unique per installed app (task affinity, provider).
python3 -I - "$dec/AndroidManifest.xml" "$pkg" "$label" <<'EOF'
import sys
path, pkg, label = sys.argv[1:]
s = open(path, encoding="utf-8").read()
for old, new in (('package="com.ducvd89.bloodborne"', f'package="{pkg}"'),
                 ('android:taskAffinity="com.ducvd89.bloodborne.atlas"', f'android:taskAffinity="{pkg}.atlas"'),
                 ('android:authorities="com.ducvd89.bloodborne.reports"', f'android:authorities="{pkg}.reports"'),
                 ('android:label="Bloodborne"', f'android:label="{label}"')):
    if s.count(old) != 1:
        sys.exit(f"AndroidManifest.xml: expected one {old}")
    s = s.replace(old, new)
open(path, "w", encoding="utf-8").write(s)
EOF
sed -i "s/^  versionName: .*/  versionName: $version-ps.$(git rev-parse --short=7 HEAD)/" "$dec/apktool.yml"

# The update check and the issue form: this fork (the class files are found by their strings, which are unique).
redirect() { # old new
    local files
    files=$(grep -rlF "\"$1\"" "$dec"/smali* || true)
    [[ $(printf '%s\n' "$files" | grep -c .) == 1 ]] || { echo "Expected one smali file with \"$1\"" >&2; exit 1; }
    sed -i "s#\"$1\"#\"$2\"#" "$files"
}
# The releases list is also the GPU driver catalog's (DriverCatalog): only UpdateCatalog's copy changes.
update_api=$(ls "$dec"/smali*/ro/cobrabm/fexdroid/UpdateCatalog.smali)
sed -i "s#\"https://api.github.com/repos/ducvd89/Bloodborne-Android/releases?per_page=50\"#\"https://api.github.com/repos/$fork/releases?per_page=50\"#; \
        s#\"https://github.com/ducvd89/Bloodborne-Android/releases/download/\"#\"https://github.com/$fork/releases/download/\"#" "$update_api"
grep -q "repos/$fork/releases" "$update_api" || { echo "UpdateCatalog: the update check was not redirected" >&2; exit 1; }
redirect "https://github.com/ducvd89/Bloodborne-Android/issues/new" "https://github.com/$fork/issues/new"

java -jar "$apktool" b -q -o "$work/rebuilt.apk" "$dec"
# Apktool rebuilds every dex and re-encodes the compiled resource files: only the dex with the changed classes
# (smali_classesN -> classesN.dex), the manifest, resources.arsc (the package name) and the runtime stay
# rebuilt; every other entry is the official one, byte for byte (the resource ids are checked to be the same).
changed_dex=$(grep -rlF "github.com/$fork/" "$dec"/smali* | sed -E "s#^$dec/(smali(_classes[0-9]+)?)/.*#\1#" | sort -u \
    | sed -E 's#^smali$#classes.dex#; s#^smali_(classes[0-9]+)$#\1.dex#')
ids() { "$bt/aapt2" dump resources "$1" 2>/dev/null | awk '/^ +resource 0x/ {print $2, $3}' | sed -E 's/ [^:]+:/ /' | sort; }
[[ $(ids "$official") == $(ids "$work/rebuilt.apk") ]] || { echo "The rebuilt resource ids differ from the official ones" >&2; exit 1; }
python3 -I - "$official" "$work/rebuilt.apk" "$work/unsigned.apk" $changed_dex <<'EOF'
import sys, zipfile
official, rebuilt, out, *changed = sys.argv[1:]
keep_rebuilt = {"AndroidManifest.xml", "resources.arsc", "assets/runtime.payload", "assets/runtime.sha256", *changed}
with zipfile.ZipFile(official) as zo, zipfile.ZipFile(rebuilt) as zr, zipfile.ZipFile(out, "w") as zw:
    names = [i.filename for i in zo.infolist() if not i.filename.startswith("META-INF/")]
    if sorted(names) != sorted(i.filename for i in zr.infolist() if not i.filename.startswith("META-INF/")):
        sys.exit("The rebuilt APK has other entries than the official one")
    for name in names:
        src = zr if name in keep_rebuilt else zo
        info = src.getinfo(name)
        zw.writestr(zipfile.ZipInfo(name, date_time=info.date_time), src.read(name), compress_type=info.compress_type)
    print("rebuilt:", ", ".join(sorted(keep_rebuilt)))
EOF
"$bt/zipalign" -f -P 16 4 "$work/unsigned.apk" "$work/aligned.apk"

if [[ ! -f $keys/pocket-s.jks ]]; then
    head -c 24 /dev/urandom | base64 | tr -d '/+=' > "$keys/pocket-s.pass"
    keytool -genkeypair -keystore "$keys/pocket-s.jks" -storepass:file "$keys/pocket-s.pass" -alias pocket-s \
        -keyalg RSA -keysize 3072 -validity 36500 -dname "CN=zhouwuqi Bloodborne Pocket S build" >/dev/null 2>&1
    chmod 600 "$keys/pocket-s.jks" "$keys/pocket-s.pass"
fi
apk=out/release/Bloodborne-PocketS-$version-$(git rev-parse --short=7 HEAD).apk
"$bt/apksigner" sign --ks "$keys/pocket-s.jks" --ks-pass "file:$keys/pocket-s.pass" --ks-key-alias pocket-s \
    --out "$apk" "$work/aligned.apk"
"$bt/apksigner" verify "$apk"
(cd out/release && sha256sum "$(basename "$apk")" > "$(basename "$apk").sha256")
echo "$apk"
