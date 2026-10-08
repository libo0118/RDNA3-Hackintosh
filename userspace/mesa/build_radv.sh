#!/bin/bash
# Build Mesa's RADV for macOS (x86_64) against our libdrm (userspace/Makefile).
#   userspace/mesa/build_radv.sh [workdir]
# Output: <workdir>/install/lib/libvulkan_radeon.dylib + radeon_icd.x86_64.json
#
# MESA_COMMIT pins the source (mirror of gitlab.freedesktop.org/mesa/mesa).
# Patches in patches/*.patch are applied in order; each one is a small,
# reviewable macOS/drm_xnu adaptation.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
WORK=${1:-$ROOT/build/mesa}
mkdir -p "$WORK" && WORK=$(cd "$WORK" && pwd)
MESA_COMMIT=${MESA_COMMIT:-d381c361aec812e9d772291dd1da9b8e1b234a74}
MESA_URL=${MESA_URL:-https://github.com/chaotic-cx/mesa-mirror.git}
PREFIX=$ROOT/build/userspace/prefix

mkdir -p "$WORK"
make -C "$ROOT/userspace" install

if [ ! -d "$WORK/src/.git" ]; then
    git init -q "$WORK/src"
    git -C "$WORK/src" remote add origin "$MESA_URL"
fi
git -C "$WORK/src" fetch -q --depth 1 origin "$MESA_COMMIT"
git -C "$WORK/src" checkout -q -f FETCH_HEAD
git -C "$WORK/src" clean -qfdx
for p in "$HERE"/patches/*.patch; do
    [ -e "$p" ] || continue
    echo "applying $(basename "$p")"
    git -C "$WORK/src" apply --whitespace=nowarn "$p"
done

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
meson setup --wipe "$WORK/build" "$WORK/src" \
    --prefix "$WORK/install" --buildtype debugoptimized \
    -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms= \
    -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false \
    -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled -Damd-use-llvm=false \
    -Dvideo-codecs= -Dbuild-tests=false -Dvulkan-layers= -Dtools= \
    -Dshared-glapi=disabled -Dzstd=disabled -Dexpat=disabled -Dspirv-tools=disabled \
    -Dc_args="-arch x86_64" -Dcpp_args="-arch x86_64" \
    -Dc_link_args="-arch x86_64 -Wl,-rpath,$PREFIX/lib" \
    -Dcpp_link_args="-arch x86_64 -Wl,-rpath,$PREFIX/lib" \
    2>&1 | tee "$WORK/meson-setup.log"
ninja -C "$WORK/build" -k 0 2>&1 | tee "$WORK/ninja.log"
ninja -C "$WORK/build" install
ls -la "$WORK/install/lib"

# dynamic_lookup hides genuinely missing symbols. nm -m names the dylib each
# undefined symbol binds to; "dynamically looked up" ones have none. A
# non-weak one of those makes dlopen fail (chained fixups bind at load), a
# weak one resolves to NULL (optional entrypoints). missing.txt = the former.
LIB="$WORK/install/lib/libvulkan_radeon.dylib"
if [ -f "$LIB" ]; then
    nm -m "$LIB" | grep '(undefined)' > "$WORK/undefined.txt" || true
    grep 'dynamically looked up' "$WORK/undefined.txt" | grep -v ' weak ' \
        | awk '{for (i = 1; i <= NF; i++) if ($i ~ /^_/) { print $i; break }}' \
        | sort -u > "$WORK/missing.txt" || true
    grep 'dynamically looked up' "$WORK/undefined.txt" | grep ' weak ' \
        | awk '{for (i = 1; i <= NF; i++) if ($i ~ /^_/) { print $i; break }}' \
        | sort -u > "$WORK/weak.txt" || true
    echo "== $(wc -l < "$WORK/undefined.txt") undefined; $(wc -l < "$WORK/weak.txt") weak (NULL at load);" \
         "$(wc -l < "$WORK/missing.txt") MISSING (dlopen would fail):"
    cat "$WORK/missing.txt"
fi
