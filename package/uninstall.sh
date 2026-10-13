#!/bin/bash
# Removes what package/install.sh, the .pkg or the release tarball's
# install.sh installed (the .pkg ships this file as
# "/Library/Application Support/RDNA3/uninstall.sh"). Reboot afterwards.
#
#   sudo package/uninstall.sh [--keep-firmware]
#
# OpenCore's copy is not touched: package/oc_install.sh --off (or remove
# EFI/OC/Kexts/RDNA3Accel.kext and its Kernel/Add entry by hand).
# --root DIR removes from DIR instead of / (tests).
set -euo pipefail
ROOT=""; KEEPFW=0
while [ $# -gt 0 ]; do
    case "$1" in
        --keep-firmware) KEEPFW=1; shift ;;
        --root) ROOT=${2%/}; shift 2 ;;
        *) echo "unknown option $1"; exit 2 ;;
    esac
done
[ -n "$ROOT" ] || [ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
A="$ROOT/Library/Application Support/RDNA3"
rm -rf "$ROOT"/Library/Extensions/RDNA3Accel.kext "$ROOT"/Library/Extensions/RDNA3Accelerator.kext \
       "$ROOT"/Library/Extensions/RDNA3Framebuffer.kext "$ROOT"/Library/Extensions/RDNA3AGDC.kext \
       "$ROOT"/Library/GPUBundles/RDNA3MTLDriver.bundle \
       "$ROOT"/Library/GPUBundles/rdna3mtl "$ROOT"/Library/GPUBundles/rdna3mtl-allow.txt
if [ $KEEPFW = 1 ]; then
    [ -d "$A" ] && find "$A" -mindepth 1 -maxdepth 1 ! -name firmware -exec rm -rf {} +
else
    rm -rf "$A"
fi
if [ -z "$ROOT" ] && [ "$(uname -s)" = Darwin ]; then
    pkgutil --forget io.github.voten641.rdna3 >/dev/null 2>&1 || true
    kmutil install --update-all --check-rebuild || true
    for e in /Volumes/EFI/EFI/OC/Kexts/RDNA3*.kext; do
        [ -e "$e" ] && echo "NOTE: OpenCore still injects $e (package/oc_install.sh --off, or remove it and its Kernel/Add entry)"
    done
fi
echo "Removed. Reboot."
