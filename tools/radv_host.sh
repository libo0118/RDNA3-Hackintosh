#!/bin/bash
# radv_host.sh - build Mesa's RADV on a Linux host against libdrm_xnu with
# the loopback backend (uapi/rdna3_drm.c in-process, software SDMA), the
# same pinned Mesa commit and patches as the macOS build
# (userspace/mesa/build_radv.sh), then run tests/rdna3_vktest against it.
# No GPU, no kext, no CI minutes: what RADV asks of our UAPI and libdrm
# core, and how far device creation and a transfer submit get, shows up
# here first.
#
#   tools/radv_host.sh [workdir]          (default build/radv-host)
#
# Needs: meson, ninja, glslangValidator, python3 mako/yaml/packaging, flex,
# bison, a C/C++ compiler. The patches' macOS branches are taken on Linux
# too with -DRADV_DRM_XNU (render node through drm_xnu, no /dev/dri).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
WORK=${1:-$ROOT/build/radv-host}
mkdir -p "$WORK" && WORK=$(cd "$WORK" && pwd)
MESA_COMMIT=$(sed -n 's/^MESA_COMMIT=\${MESA_COMMIT:-\([0-9a-f]*\)}/\1/p' "$ROOT/userspace/mesa/build_radv.sh")
MESA_URL=${MESA_URL:-https://github.com/chaotic-cx/mesa-mirror.git}
MESA_SRC=${MESA_SRC:-$WORK/src}
PREFIX=$WORK/prefix
CC=${CC:-cc}

# ---- libdrm.so.2 (drm_xnu core + loopback + uapi) and libdrm_amdgpu.so.1
say() { printf '\n== %s\n' "$*"; }
say "libdrm_xnu (loopback) for the host"
LIBDRM=$ROOT/third_party/libdrm
XNU=$ROOT/userspace/libdrm_xnu
OBJ=$WORK/obj
rm -rf "$OBJ" && mkdir -p "$OBJ" "$PREFIX/lib/pkgconfig" "$PREFIX/include/libdrm"
CF="-O1 -g -fPIC -D_GNU_SOURCE -std=gnu11"
for f in drm_xnu xf86drm_xnu; do
    $CC $CF -Wall -Wextra -Werror -I"$XNU/override" -I"$XNU" -I"$LIBDRM" -I"$LIBDRM/include/drm" \
        -c "$XNU/$f.c" -o "$OBJ/$f.o"
done
$CC $CF -Wall -Wextra -Werror -I"$XNU" -I"$ROOT/common" -I"$ROOT/hw" -I"$ROOT/uapi" -I"$ROOT/third_party/amd" \
    -c "$XNU/backend_loopback.c" -o "$OBJ/backend_loopback.o"
for f in "$ROOT"/uapi/rdna3_drm.c "$ROOT"/common/*.c; do
    $CC $CF -I"$ROOT/common" -I"$ROOT/hw" -I"$ROOT/uapi" -I"$ROOT/third_party/amd" \
        -c "$f" -o "$OBJ/k_$(basename "$f" .c).o"
done
$CC -shared -Wl,-soname,libdrm.so.2 -o "$PREFIX/lib/libdrm.so.2" "$OBJ"/*.o -lpthread
ln -sf libdrm.so.2 "$PREFIX/lib/libdrm.so"
mkdir -p "$OBJ/amd"
for f in "$LIBDRM"/amdgpu/*.c; do
    $CC $CF -w -I"$XNU/override" -I"$XNU" -I"$LIBDRM" -I"$LIBDRM/amdgpu" -I"$LIBDRM/include/drm" \
        -DHAVE_LIBDRM_ATOMIC_PRIMITIVES=1 -DAMDGPU_ASIC_ID_TABLE="\"$LIBDRM/data/amdgpu.ids\"" \
        -c "$f" -o "$OBJ/amd/$(basename "$f" .c).o"
done
$CC -shared -Wl,-soname,libdrm_amdgpu.so.1 -o "$PREFIX/lib/libdrm_amdgpu.so.1" "$OBJ"/amd/*.o \
    -L"$PREFIX/lib" -ldrm
ln -sf libdrm_amdgpu.so.1 "$PREFIX/lib/libdrm_amdgpu.so"
cp "$LIBDRM/xf86drm.h" "$LIBDRM/libsync.h" "$XNU/drm_xnu.h" "$PREFIX/include/"
cp "$LIBDRM"/include/drm/*.h "$LIBDRM/amdgpu/amdgpu.h" "$PREFIX/include/libdrm/"
for n in libdrm libdrm_amdgpu; do
    lib=${n#lib}
    printf 'prefix=%s\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\n\nName: %s\nDescription: %s over drm_xnu loopback\nVersion: 2.4.134\nLibs: -L${libdir} -l%s\nCflags: -I${includedir} -I${includedir}/libdrm\n' \
        "$PREFIX" "$n" "$n" "$lib" > "$PREFIX/lib/pkgconfig/$n.pc"
done

# ---- Mesa
say "Mesa $MESA_COMMIT + userspace/mesa/patches"
if [ ! -d "$MESA_SRC/.git" ]; then
    git init -q "$MESA_SRC"
    git -C "$MESA_SRC" remote add origin "$MESA_URL"
fi
if [ "$(git -C "$MESA_SRC" rev-parse HEAD 2>/dev/null)" != "$MESA_COMMIT" ]; then
    git -C "$MESA_SRC" fetch -q --depth 1 origin "$MESA_COMMIT"
    git -C "$MESA_SRC" checkout -q -f FETCH_HEAD
fi
git -C "$MESA_SRC" checkout -q -f "$MESA_COMMIT"
git -C "$MESA_SRC" clean -qfdx
for p in "$ROOT"/userspace/mesa/patches/*.patch; do
    echo "applying $(basename "$p")"
    git -C "$MESA_SRC" apply --whitespace=nowarn "$p"
done

# configure Linux like macOS: no KMS (so no VK_KHR_display WSI, which
# needs xf86drmMode), libdrm from patch 0004's darwin branch
sed -i -e "/^system_has_kms_drm = /s/'linux', //" \
    -e "s/^elif host_machine.system() == 'darwin' and with_amd_vk/elif ['darwin', 'linux'].contains(host_machine.system()) and with_amd_vk/" \
    "$MESA_SRC/meson.build"
grep -q "\['darwin', 'linux'\]" "$MESA_SRC/meson.build" || { echo "meson.build host edit did not apply"; exit 1; }

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
if [ ! -f "$WORK/build/build.ninja" ]; then
    meson setup "$WORK/build" "$MESA_SRC" --prefix "$WORK/install" --buildtype debugoptimized \
        -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms= \
        -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false \
        -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled -Damd-use-llvm=false \
        -Dvideo-codecs= -Dbuild-tests=false -Dvulkan-layers= -Dtools= \
        -Dshared-glapi=disabled -Dzstd=disabled -Dexpat=disabled -Dspirv-tools=disabled \
        -Dvalgrind=disabled -Dlibunwind=disabled \
        -Dc_args="-DRADV_DRM_XNU" -Dcpp_args="-DRADV_DRM_XNU" \
        -Dc_link_args="-Wl,-rpath,$PREFIX/lib" -Dcpp_link_args="-Wl,-rpath,$PREFIX/lib" \
        > "$WORK/meson-setup.log" 2>&1 || { tail -30 "$WORK/meson-setup.log"; exit 1; }
fi
ninja -C "$WORK/build" > "$WORK/ninja.log" 2>&1 || { grep -E "error|FAILED" "$WORK/ninja.log" | head -40; exit 1; }
ninja -C "$WORK/build" install > /dev/null
LIB=$(find "$WORK/install" -name libvulkan_radeon.so | head -1)
echo "built $LIB"

# ---- the test
say "rdna3_vktest on the loopback UAPI"
$CC -O1 -g -std=gnu11 -Wall -Wextra -Werror -I"$MESA_SRC/include" -o "$WORK/rdna3_vktest" \
    "$ROOT/userspace/rdna3_vktest.c" -ldl
LD_LIBRARY_PATH="$PREFIX/lib" "$WORK/rdna3_vktest" --loopback "$LIB"
