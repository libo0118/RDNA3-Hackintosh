#!/bin/bash
# RDNA3-Hackintosh test install (macOS 15, x86_64). Run from a built tree:
#   sudo package/install.sh --fw /path/to/linux-firmware/amdgpu [--radv build/mesa/install]
#
# Installs (nothing overlaps a NullMoth install):
#   /Library/Extensions/RDNA3Accel.kext          kext (+ firmware in Resources)
#   /Library/Extensions/RDNA3Accelerator.kext    Stage 7 accelerator (if built)
#   /Library/Extensions/RDNA3Framebuffer.kext    Stage 3 framebuffer (if built)
#   /Library/GPUBundles/RDNA3MTLDriver.bundle    Metal plugin
#   /Library/GPUBundles/rdna3mtl/                libdrm, libdrm_amdgpu, RADV, ICD json,
#                                                translator, amdgpu.ids
#   /Library/GPUBundles/rdna3mtl-allow.txt
# then rebuilds the Auxiliary Kernel Collection. A reboot is required.
# Remove with package/uninstall.sh.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FW=""; RADV="$ROOT/build/mesa/install"; DRY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --fw) FW=$2; shift 2;;
        --radv) RADV=$2; shift 2;;
        --dry-run) DRY=1; shift;;
        *) echo "unknown option $1"; exit 2;;
    esac
done
[ "$(id -u)" = 0 ] || [ $DRY = 1 ] || { echo "run with sudo"; exit 1; }
run() { echo "+ $*"; [ $DRY = 1 ] || "$@"; }

KEXT=$ROOT/build/RDNA3Accel.kext
BUNDLE=$ROOT/build/metal/RDNA3MTLDriver.bundle
XLATE=$ROOT/build/metal/librdna3mtl_translate.dylib
USR=$ROOT/build/userspace/prefix/lib
GB=/Library/GPUBundles
DIR=$GB/rdna3mtl

[ -d "$KEXT" ] || { echo "missing $KEXT (make kext)"; exit 1; }
if [ -n "$FW" ]; then
    run make -C "$ROOT/kext" firmware FW_DIR="$FW"
else
    echo "NOTE: no --fw given; rdna3lvl>=4 and the default profile need the firmware (package/firmware.sh)"
fi

run rm -rf /Library/Extensions/RDNA3Accel.kext
run cp -R "$KEXT" /Library/Extensions/
run chown -R root:wheel /Library/Extensions/RDNA3Accel.kext
run chmod -R go-w /Library/Extensions/RDNA3Accel.kext
ACCEL=$ROOT/build/RDNA3Accelerator.kext
if [ -d "$ACCEL" ]; then
    run rm -rf /Library/Extensions/RDNA3Accelerator.kext
    run cp -R "$ACCEL" /Library/Extensions/
    run chown -R root:wheel /Library/Extensions/RDNA3Accelerator.kext
    run chmod -R go-w /Library/Extensions/RDNA3Accelerator.kext
    echo "NOTE: the accelerator starts in the default profile (no rdna3lvl) or with rdna3accel=1"
fi
FB=$ROOT/build/RDNA3Framebuffer.kext
if [ -d "$FB" ]; then
    run rm -rf /Library/Extensions/RDNA3Framebuffer.kext
    run cp -R "$FB" /Library/Extensions/
    run chown -R root:wheel /Library/Extensions/RDNA3Framebuffer.kext
    run chmod -R go-w /Library/Extensions/RDNA3Framebuffer.kext
    echo "NOTE: the framebuffer starts in the default profile (no rdna3lvl) or with rdna3fb=1"
fi

run mkdir -p "$DIR"
[ -f "$USR/libdrm.2.dylib" ] && run cp "$USR/libdrm.2.dylib" "$USR/libdrm_amdgpu.1.dylib" "$DIR/"
run cp "$ROOT/third_party/libdrm/data/amdgpu.ids" "$DIR/"
if [ -f "$RADV/lib/libvulkan_radeon.dylib" ]; then
    run cp "$RADV/lib/libvulkan_radeon.dylib" "$DIR/"
    # ICD manifest pointing at the installed dylib
    if [ $DRY = 0 ]; then
        cat > "$DIR/radeon_icd.x86_64.json" <<JSON
{
    "ICD": {
        "api_version": "1.4.0",
        "library_path": "$DIR/libvulkan_radeon.dylib"
    },
    "file_format_version": "1.0.0"
}
JSON
    fi
    # RADV finds libdrm through @rpath
    run install_name_tool -add_rpath "$DIR" "$DIR/libvulkan_radeon.dylib" || true
else
    echo "NOTE: RADV not built (userspace/mesa/build_radv.sh); Vulkan/Metal stay off"
fi
[ -f "$XLATE" ] && run cp "$XLATE" "$DIR/"
if [ -d "$BUNDLE" ]; then
    run rm -rf "$GB/RDNA3MTLDriver.bundle"
    run cp -R "$BUNDLE" "$GB/"
fi
run cp "$ROOT/metal/rdna3mtl-allow.txt" "$GB/"
run chown -R root:wheel "$DIR" "$GB/rdna3mtl-allow.txt"
[ -d "$GB/RDNA3MTLDriver.bundle" ] && run chown -R root:wheel "$GB/RDNA3MTLDriver.bundle"

# ad-hoc signatures over the final bundles (the firmware went into
# RDNA3Accel's Resources above): kernelmanagerd refuses unsigned kexts
for k in RDNA3Accel RDNA3Accelerator RDNA3Framebuffer; do
    [ -d "/Library/Extensions/$k.kext" ] && run codesign --force --deep --sign - "/Library/Extensions/$k.kext"
done
run kmutil install --update-all --check-rebuild || run kmutil load -p /Library/Extensions/RDNA3Accel.kext || true
echo
echo "Installed. Reboot, approve the extension in System Settings > Privacy & Security"
echo "if asked, and reboot again. Without rdna3 boot-args the default profile runs;"
echo "for the step-by-step ladder start with rdna3lvl=0 (docs/TESTING-HW.md)."
