#!/bin/bash
# RDNA3-Hackintosh through OpenCore (macOS 15, x86_64): the kexts are
# injected by OpenCore (EFI/OC/Kexts + Kernel/Add) instead of the Auxiliary
# Kernel Collection, which kernelmanagerd may refuse for ad-hoc signed kexts
# ("invalid codeful kernel extension"). The firmware goes to
# /Library/Application Support/RDNA3/firmware, where the kext reads it.
#
#   sudo package/oc_install.sh --efi /Volumes/EFI --fw /path/to/linux-firmware/amdgpu [--step N]
#   sudo package/oc_install.sh --efi /Volumes/EFI --step N      only change the boot-args
#   sudo package/oc_install.sh --efi /Volumes/EFI --off         disable the RDNA3 kexts again
#
# Mount the EFI partition first: 'sudo diskutil mount disk0s1' (see
# 'diskutil list' for the right disk). config.plist is backed up next to
# itself (config.plist.rdna3-<date>) before every change. Needs python3
# (Xcode command line tools, as for building). bash 3.2 compatible.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
# a function, not a string: the installed copy lives under "Application Support"
plist() { python3 "$ROOT/tools/vm/vmtool.py" plist "$@"; }
BUILD=${RDNA3_BUILD:-$ROOT/build}
EFI=""; FW=""; STEP=""; OFF=0; DRY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --efi) EFI=$2; shift 2;;
        --fw) FW=$2; shift 2;;
        --step) STEP=$2; shift 2;;
        --off) OFF=1; shift;;
        --dry-run) DRY=1; shift;;
        *) echo "unknown option $1"; exit 2;;
    esac
done
[ -n "$EFI" ] || { echo "give --efi (the mounted EFI partition, e.g. /Volumes/EFI)"; exit 2; }
CONF=$EFI/EFI/OC/config.plist
[ -f "$CONF" ] || { echo "no $CONF"; exit 1; }
[ $DRY = 1 ] || [ -w "$CONF" ] || { echo "cannot write $CONF: run with sudo"; exit 1; }
[ $DRY = 1 ] || [ -z "$FW" ] || [ "$(id -u)" = 0 ] || { echo "--fw writes to /Library: run with sudo"; exit 1; }
run() { echo "+ $*"; [ $DRY = 1 ] || "$@"; }

# the boot-args OpenCore sets now, without our rdna3* ones
current=$(plist "$CONF" --show)
# shellcheck disable=SC2086 # split into words on purpose
keep=$(printf '%s\n' $current | { grep -v -E '^-?rdna3' || true; } | tr '\n' ' ' | sed 's/ *$//')

if [ $OFF = 1 ]; then
    args="$keep -rdna3off"
elif [ -n "$STEP" ]; then
    a=$(awk -v n="$STEP" '$1 == n { $1 = ""; sub(/^ +/, ""); print; f = 1 } END { exit !f }' \
        "$ROOT/tools/ladder-steps.txt") || { echo "unknown step $STEP (tools/ladder-steps.txt)"; exit 2; }
    # -v and keepsyms for readable logs and panics; kept if already there
    for x in -v keepsyms=1; do
        case " $keep " in *" $x "*) ;; *) keep="$keep $x";; esac
    done
    args="$keep $a"
else
    args=""
fi
args=${args#"${args%%[! ]*}"}

run cp "$CONF" "$CONF.rdna3-$(date +%Y%m%d-%H%M%S)"

kexts=()
if [ -n "$FW" ] || { [ $OFF = 0 ] && [ -z "$STEP" ]; }; then
    # (re)install the kexts: code and Info.plist, the order OpenCore loads them
    for n in RDNA3Accel RDNA3Framebuffer RDNA3Accelerator; do
        k=$BUILD/$n.kext
        [ -f "$k/Contents/MacOS/$n" ] || continue
        run rm -rf "$EFI/EFI/OC/Kexts/$n.kext"
        run mkdir -p "$EFI/EFI/OC/Kexts/$n.kext/Contents/MacOS"
        run cp "$k/Contents/Info.plist" "$EFI/EFI/OC/Kexts/$n.kext/Contents/"
        run cp "$k/Contents/MacOS/$n" "$EFI/EFI/OC/Kexts/$n.kext/Contents/MacOS/"
        kexts+=(--kext "$n")
    done
    [ ${#kexts[@]} -gt 0 ] || { echo "no built kexts in $BUILD (make kext; make -C fb; make -C accel)"; exit 1; }
    for k in /Library/Extensions/RDNA3*.kext; do
        [ -e "$k" ] && echo "NOTE: $k is installed too: fine (the .pkg does it: the framebuffer and accelerator load from there; an injected RDNA3Accel wins over its copy)"
    done
fi

if [ -n "$FW" ]; then
    run make -C "$ROOT/kext" firmware FW_DIR="$FW"
    D="/Library/Application Support/RDNA3/firmware"
    run mkdir -p "$D"
    run cp "$BUILD"/RDNA3Accel.kext/Contents/Resources/*.bin "$D/"
fi

set --
[ -n "$args" ] && set -- --boot-args "$args"
if [ $# -gt 0 ] || [ ${#kexts[@]} -gt 0 ]; then
    if [ $DRY = 1 ]; then
        echo "+ vmtool.py plist $CONF $* ${kexts[*]+${kexts[*]}}"
    else
        plist "$CONF" ${1+"$@"} ${kexts[@]+"${kexts[@]}"}
    fi
fi
echo "Done. Restart; OpenCore applies the boot-args (NVRAM/Add + NVRAM/Delete)."
echo "Way back: pick the backup config.plist.rdna3-* or run with --off."
