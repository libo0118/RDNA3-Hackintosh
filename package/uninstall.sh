#!/bin/bash
# Removes what package/install.sh, the .pkg or the release tarball's
# install.sh installed (the .pkg ships this file as
# "/Library/Application Support/RDNA3/uninstall.sh"). Reboot afterwards.
#
#   sudo package/uninstall.sh [--keep-firmware] [--efi DIR] [--keep-opencore]
#
# The OpenCore side goes too, through oc_install.sh --remove (the same
# config.plist backup, only our own kext copies, Kernel/Add entries and
# rdna3* boot-args; your other entries stay). The EFI partition must be
# mounted (sudo diskutil mount disk0s1): --efi DIR names it, /Volumes/EFI
# is looked at otherwise. Nothing is removed at all when OpenCore is in use
# and its EFI cannot be found: mount it, or pass --keep-opencore to leave
# OpenCore alone (then remove its part later with oc_install.sh --remove
# from a source checkout, or by hand).
# --root DIR removes from DIR instead of / (tests; OpenCore only with --efi).
set -euo pipefail
ROOT=""; KEEPFW=0; EFI=""; KEEPOC=0
while [ $# -gt 0 ]; do
    case "$1" in
        --keep-firmware) KEEPFW=1; shift ;;
        --root) ROOT=${2%/}; shift 2 ;;
        --efi) EFI=${2:-}; [ -n "$EFI" ] || { echo "--efi needs the mounted EFI partition"; exit 2; }; EFI=${EFI%/}; shift 2 ;;
        --keep-opencore) KEEPOC=1; shift ;;
        *) echo "unknown option $1"; exit 2 ;;
    esac
done
[ -n "$ROOT" ] || [ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
A="$ROOT/Library/Application Support/RDNA3"

# --- OpenCore first: its helper lives in $A, which is removed below
OCMSG="OpenCore: not in use here, nothing to remove."
if [ $KEEPOC = 1 ]; then
    OCMSG="OpenCore: left as it is (--keep-opencore): EFI/OC/Kexts/RDNA3*.kext, their Kernel/Add entries and the rdna3* boot-args still inject a kext that is gone. Remove them with oc_install.sh --efi <EFI> --remove (source checkout) or by hand."
else
    if [ -z "$EFI" ] && [ -z "$ROOT" ] && [ "$(uname -s)" = Darwin ]; then
        if [ -f /Volumes/EFI/EFI/OC/config.plist ]; then
            EFI=/Volumes/EFI
        elif nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:opencore-version >/dev/null 2>&1; then
            echo "OpenCore booted this Mac but its EFI partition is not mounted at /Volumes/EFI."
            echo "Nothing was removed. Mount it (sudo diskutil mount disk0s1; see diskutil list) and run again"
            echo "(--efi DIR if it is somewhere else), or add --keep-opencore to leave OpenCore alone."
            exit 1
        fi
    fi
    if [ -n "$EFI" ]; then
        D=$(cd "$(dirname "$0")" && pwd)
        OCH=""
        for c in "$D/oc_install.sh" "$A/oc_install.sh"; do [ -f "$c" ] && { OCH=$c; break; }; done
        [ -n "$OCH" ] || { echo "no oc_install.sh next to uninstall.sh or in $A: nothing was removed (--keep-opencore skips OpenCore)"; exit 1; }
        OCOUT=$(bash "$OCH" --efi "$EFI" --remove) || {
            printf '%s\n' "$OCOUT"
            echo "oc_install.sh --remove failed: nothing was removed. Fix that (EFI mounted read-write? run with sudo) or add --keep-opencore."
            exit 1
        }
        printf '%s\n' "$OCOUT"
        OCMSG="OpenCore ($EFI): see above."
    fi
fi

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
fi
echo "Removed: the kexts in /Library/Extensions, the Metal plugin and RADV (/Library/GPUBundles), $A$([ $KEEPFW = 1 ] && echo ' (the firmware stays)')."
echo "$OCMSG"
echo "Left alone: your config.plist.rdna3-* backups and everything of yours in OpenCore. Reboot."
