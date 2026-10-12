#!/bin/bash
# build_cts.sh OUT [WORK] - the Vulkan CTS subset of the card report (V2):
# deqp-vk of VK-GL-CTS and the Khronos Vulkan loader it loads RADV through
# (libvulkan.1.dylib + RADV's ICD JSON), x86_64 macOS for the VM. Packed
# into OUT/rdna3-cts.tar.gz with the case patterns the report runs
# (tools/cts/patterns.txt). WORK keeps the clones and the build trees
# between runs (a rebuild at the same tags is incremental).
#
# Needs git, python3, cmake and ninja on PATH (the radv-macos job's venv).
set -euo pipefail
CTS_TAG=vulkan-cts-1.4.6.2      # 2026-08-06
LOADER_TAG=v1.4.360             # 2026-08-13
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
[ $# -ge 1 ] || { echo "usage: $0 OUT [WORK]" >&2; exit 2; }
mkdir -p "$1"
OUT=$(cd "$1" && pwd)
W=${2:-$ROOT/build/cts}
mkdir -p "$W"
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc)
ARCH=()
LIBEXT=so
if [ "$(uname -s)" = Darwin ]; then
    ARCH=(-DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0)
    LIBEXT=dylib
fi
say() { printf '\n== %s\n' "$*"; }

# a shallow clone at TAG, kept while it is that tag
fetch() {
    if [ "$(git -C "$3" describe --tags --exact-match 2>/dev/null || true)" != "$1" ]; then
        rm -rf "$3"
        git -c advice.detachedHead=false clone -q --depth 1 --branch "$1" "$2" "$3"
    fi
}

say "Vulkan loader $LOADER_TAG"
fetch "$LOADER_TAG" https://github.com/KhronosGroup/Vulkan-Loader.git "$W/loader"
cmake -S "$W/loader" -B "$W/loader/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DUPDATE_DEPS=ON \
    -DBUILD_TESTS=OFF "${ARCH[@]}" > "$W/loader-cmake.log"
ninja -C "$W/loader/build" -j "$JOBS" > "$W/loader-ninja.log" || { tail -40 "$W/loader-ninja.log"; exit 1; }
LOADER=$(find "$W/loader/build" -name "libvulkan.1.$LIBEXT" -o -name "libvulkan.$LIBEXT.1" | head -1)
[ -n "$LOADER" ] || { echo "no loader library built" >&2; exit 1; }

say "VK-GL-CTS $CTS_TAG (external sources, then deqp-vk)"
fetch "$CTS_TAG" https://github.com/KhronosGroup/VK-GL-CTS.git "$W/cts"
if [ ! -f "$W/cts/.rdna3-fetched" ]; then
    (cd "$W/cts" && python3 external/fetch_sources.py --protocol https) > "$W/fetch.log" 2>&1 ||
        { tail -40 "$W/fetch.log"; exit 1; }
    touch "$W/cts/.rdna3-fetched"
fi
cmake -S "$W/cts" -B "$W/cts/build" -G Ninja -DCMAKE_BUILD_TYPE=Release "${ARCH[@]}" > "$W/cts-cmake.log" ||
    { tail -40 "$W/cts-cmake.log"; exit 1; }
start=$(date +%s)
ninja -C "$W/cts/build" -j "$JOBS" deqp-vk deqp-vk-data > "$W/cts-ninja.log" 2>&1 ||
    { grep -E "error|FAILED" "$W/cts-ninja.log" | head -40; tail -20 "$W/cts-ninja.log"; exit 1; }
echo "deqp-vk built in $(( ($(date +%s) - start) / 60 )) min"
M=$W/cts/build/external/vulkancts/modules/vulkan
[ -x "$M/deqp-vk" ] && [ -d "$M/vulkan" ] || { echo "no deqp-vk / data in $M" >&2; exit 1; }

say "rdna3-cts.tar.gz"
P=$W/pkg
rm -rf "$P" && mkdir -p "$P/cts"
cp "$M/deqp-vk" "$P/cts/"
cp -R "$M/vulkan" "$P/cts/vulkan"
cp "$LOADER" "$P/cts/"
if [ "$LIBEXT" = dylib ]; then
    ln -sf libvulkan.1.dylib "$P/cts/libvulkan.dylib"
    # what the VM runs is ad-hoc signed like the rest of the bundle
    codesign -f -s - "$P/cts/deqp-vk" "$P/cts/libvulkan.1.dylib" >/dev/null 2>&1 || true
fi
cp "$ROOT/tools/cts/patterns.txt" "$P/cts/"
printf 'VK-GL-CTS %s, Vulkan-Loader %s, built %s from %s\n' "$CTS_TAG" "$LOADER_TAG" \
    "$(date -u '+%F %T UTC')" "$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo '?')" > "$P/cts/VERSION"
tar -czf "$OUT/rdna3-cts.tar.gz" -C "$P" cts
ls -l "$OUT/rdna3-cts.tar.gz"
cat "$P/cts/VERSION"
