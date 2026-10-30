#!/usr/bin/env bash
# Development build of the two game-process libraries that change most often, libbbcpu.so and
# libbbgpu.so, without the online module (libbbnet20.so) or the tools: what push_runtime.sh puts on
# a phone over the installed app's own runtime. The release build is build_bionic.sh.
# Prerequisites: tools/android/build_bionic_deps.sh (NDK 29 + libraries) and the header-only
# packages (magic_enum, tsl-robin-map, VulkanMemoryAllocator, Boost) in BB_ARM64_HEADERS.
# Output: out/bionic-dev/{cpu,gpu}/lib*.so.
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."
out=out/bionic-dev
root=$PWD/.local-deps/android
ndk=${BB_NDK:-$root/sdk/ndk/29.0.14206865}
api=${BB_ANDROID_API:-30}
prefix=$root/bionic-prefix
headers=${BB_ARM64_HEADERS:-$PWD/.local-deps/root/usr}
toolchain=$ndk/build/cmake/android.toolchain.cmake
J=${BB_JOBS:-$(nproc)}
[[ -f $toolchain ]] || { echo "No NDK: $ndk" >&2; exit 1; }
[[ -f $prefix/.done-ffmpeg ]] || { echo "No Bionic deps in $prefix (tools/android/build_bionic_deps.sh)" >&2; exit 1; }
boost_dir=$(ls -d "$headers"/lib/cmake/Boost-* "$headers"/lib/*/cmake/Boost-* 2>/dev/null | tail -n 1 || true)
boost_headers_dir=$(ls -d "$headers"/lib/cmake/boost_headers-* "$headers"/lib/*/cmake/boost_headers-* 2>/dev/null | tail -n 1 || true)
[[ -n $boost_dir && -d $headers/share/cmake/magic_enum ]] || { echo "No header-only packages in $headers" >&2; exit 1; }
ndk_cmake=(-DCMAKE_TOOLCHAIN_FILE="$toolchain" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$api -DANDROID_STL=c++_shared)
for patch in gpu/patches/fsr-vulkan/*.patch; do
    if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
        git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch"
    fi
done
mkdir -p "$out"

if [[ ! -f $out/cpu/build.ninja ]]; then
    cmake -S cpu -B "$out/cpu" -G Ninja "${ndk_cmake[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
fi
nice -n 10 ninja -C "$out/cpu" -j "$J" bbcpu > "$out/cpu-build.log" 2>&1 \
    || { grep -E "error|FAILED" "$out/cpu-build.log" | head -40 >&2; exit 1; }

export PKG_CONFIG_PATH=$prefix/lib/pkgconfig:$prefix/share/pkgconfig
export PKG_CONFIG_LIBDIR=$PKG_CONFIG_PATH
if [[ ! -f $out/gpu/build.ninja ]]; then
    cmake -S gpu -B "$out/gpu" -G Ninja "${ndk_cmake[@]}" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO=off -DBB_LTO=OFF -DBB_NET=OFF \
        "-DCMAKE_FIND_ROOT_PATH=$prefix;$headers" -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
        -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
        -DCMAKE_PREFIX_PATH="$prefix;$headers" \
        -Dmagic_enum_DIR="$headers/share/cmake/magic_enum" \
        -Dtsl-robin-map_DIR="$headers/share/cmake/tsl-robin-map" \
        -DVulkanMemoryAllocator_DIR="$headers/share/cmake/VulkanMemoryAllocator" \
        -DBoost_DIR="$boost_dir" -Dboost_headers_DIR="$boost_headers_dir" \
        "-DBB_CPU_LIB=$PWD/$out/cpu/libbbcpu.so" >/dev/null
fi
nice -n 10 ninja -C "$out/gpu" -j "$J" bbgpu > "$out/gpu-build.log" 2>&1 \
    || { grep -v '^\[' "$out/gpu-build.log" | tail -40 >&2; exit 1; }
# Without debug info, as build_release_runtime.py ships them (the build directories keep the full ones
# for symbolizing a crash).
strip=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
"$strip" --strip-debug -o "$out/libbbcpu.so" "$out/cpu/libbbcpu.so"
"$strip" --strip-debug -o "$out/libbbgpu.so" "$out/gpu/libbbgpu.so"
ls -l "$out/libbbcpu.so" "$out/libbbgpu.so"
