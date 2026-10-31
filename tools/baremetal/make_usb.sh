#!/bin/bash
# make_usb.sh DEVICE [--dir DIR] - the bootable macOS installer USB stick.
#
#   sudo tools/baremetal/make_usb.sh /dev/sda
#
# Wipes DEVICE completely (nothing on it is kept or read), makes a GPT with one FAT32 partition
# labelled OPENCORE and copies DIR/efi/EFI (OpenCore) and DIR/recovery (as
# com.apple.recovery.boot) onto it, plus the helper scripts the owner runs inside macOS
# (tools/baremetal/stick/: install.sh, report.sh, metal.sh) into rdna3/ and everything in
# DIR/rdna3/ (the release tarball RDNA3-Hackintosh-*.tar.gz, metal/ with the Metal plugin and
# RADV; docs/BAREMETAL.md part 5) next to them. DIR is what fetch.sh and build_efi.py made
# (default: ~/rdna3-baremetal of the user who ran sudo).
#
# It refuses, before touching anything:
#   - anything that is not a whole disk that is removable AND on USB (override: --force-not-usb,
#     which asks for a longer confirmation),
#   - a disk with a mounted system partition (/, /boot, /efi, /home, /usr, /var, /mnt/data),
#   - a disk under 4 GiB or over 2 TiB (override: --force-not-usb),
#   - a DIR without the finished EFI and recovery.
# It shows model and size and wants the device name typed (and nothing else) to go on.
# Uses lsblk, parted, wipefs, mkfs.fat, mount, dd, sync: standard Linux tools.
set -euo pipefail

# --- test hooks (tests/test_baremetal.sh): every external tool can be replaced, and the
# "device" may be a plain file. Not for normal use.
LSBLK=${MAKE_USB_LSBLK:-lsblk}
PARTED=${MAKE_USB_PARTED:-parted}
WIPEFS=${MAKE_USB_WIPEFS:-wipefs}
MKFS=${MAKE_USB_MKFS:-mkfs.fat}
MOUNT=${MAKE_USB_MOUNT:-mount}
UMOUNT=${MAKE_USB_UMOUNT:-umount}
SYNC=${MAKE_USB_SYNC:-sync}
FINDMNT=${MAKE_USB_FINDMNT:-findmnt}
TEST_MODE=${MAKE_USB_TEST_MODE:-}     # 1: not root, not a block device, no partition wait
# MAKE_USB_TEST_MNT=DIR: the "mounted" stick is that plain directory

SELF_DIR=$(cd "$(dirname "$0")" && pwd)
STICK_SRC=$SELF_DIR/stick
STICK_SCRIPTS="install.sh report.sh metal.sh"
TOTAL=7
STEP=0
CHUNK_MB=8
SMALL_FILE=$((CHUNK_MB * 1048576))

die() { echo "make_usb.sh: $*" >&2; exit 1; }
step() { STEP=$((STEP + 1)); printf '\n[%d/%d] %s\n' "$STEP" "$TOTAL" "$*"; }
usage() { sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2; exit 2; }

DEV="" DIR="" FORCE=0
while [ $# -gt 0 ]; do
    case "$1" in
    --dir) [ $# -ge 2 ] || usage; DIR=$2; shift 2 ;;
    --force-not-usb) FORCE=1; shift ;;
    -h|--help) usage ;;
    -*) die "unknown option $1" ;;
    *) [ -z "$DEV" ] || die "one device only"; DEV=$1; shift ;;
    esac
done
[ -n "$DEV" ] || usage

if [ -z "$DIR" ]; then
    home=$HOME
    if [ -n "${SUDO_USER:-}" ]; then home=$(getent passwd "$SUDO_USER" | cut -d: -f6); fi
    DIR=$home/rdna3-baremetal
fi

# ---------------------------------------------------------------- checks (nothing is written)
step "Checking $DEV and $DIR"
[ -n "$TEST_MODE" ] || [ "$(id -u)" = 0 ] || die "run it with sudo (it partitions and formats the stick)"
case "$DEV" in /dev/*) ;; *) [ -n "$TEST_MODE" ] || die "$DEV is not a /dev/ path" ;; esac
if [ -z "$TEST_MODE" ]; then
    DEV=$(readlink -f "$DEV")
    [ -b "$DEV" ] || die "$DEV is not a block device"
fi

# lsblk -P: NAME="sda" RM="1" TRAN="usb" TYPE="disk" SIZE="61530439680" MODEL="..." VENDOR="..."
info=$($LSBLK -dnb -P -o NAME,RM,TRAN,TYPE,SIZE,MODEL,VENDOR "$DEV" 2>/dev/null) || die "lsblk cannot read $DEV"
[ -n "$info" ] || die "lsblk knows nothing about $DEV"
field() { printf '%s\n' "$info" | sed -n "s/.*\\b$1=\"\\([^\"]*\\)\".*/\\1/p" | head -1; }
rm_flag=$(field RM); tran=$(field TRAN); type=$(field TYPE); size=$(field SIZE)
model=$(field MODEL); vendor=$(field VENDOR)
[ "$type" = "disk" ] || die "$DEV is a '$type', not a whole disk (give /dev/sdX, not /dev/sdX1)"
case "$size" in ''|*[!0-9]*) die "cannot read the size of $DEV" ;; esac
size_gb=$((size / 1000000000))
size_gib=$((size / 1073741824))

if [ "$tran" != "usb" ] || [ "$rm_flag" != "1" ]; then
    if [ $FORCE = 0 ]; then
        die "$DEV is not a removable USB disk (transport '${tran:-?}', removable '${rm_flag:-?}'). Refusing. This script is for the USB stick only."
    fi
    echo "WARNING: $DEV is NOT a removable USB disk (transport '${tran:-?}', removable '${rm_flag:-?}'), --force-not-usb given."
fi
if [ "$size" -lt $((4 * 1073741824)) ] || [ "$size" -gt $((2 * 1099511627776)) ]; then
    [ $FORCE = 1 ] || die "$DEV is $size_gb GB: a stick for this is 4 GiB to 2 TiB (--force-not-usb to override)"
fi

# mounted partitions of the disk: system ones stop us, the others are unmounted in step 2
# two sources, so that one failing does not let a system disk through: lsblk (MOUNTPOINTS, or
# MOUNTPOINT on an old util-linux) and the kernel's own mount table
mounts=$({ $LSBLK -nr -o MOUNTPOINTS "$DEV" 2>/dev/null || $LSBLK -nr -o MOUNTPOINT "$DEV" 2>/dev/null || true
           awk -v d="$DEV" '$1 == d || index($1, d) == 1 && substr($1, length(d) + 1) ~ /^p?[0-9]+$/ { print $2 }' \
               "${MAKE_USB_PROC_MOUNTS:-/proc/mounts}" 2>/dev/null || true; } | tr ' ' '\n' | sed '/^$/d' | sort -u)
for m in $mounts; do
    case "$m" in
    /|/boot|/boot/*|/efi|/efi/*|/home|/home/*|/usr|/var|/mnt/data|/mnt/data/*|/nix|/nix/*)
        die "$DEV has a system partition mounted at $m. Refusing." ;;
    esac
done

EFI_SRC=$DIR/efi/EFI
REC_SRC=$DIR/recovery
for f in "$EFI_SRC/BOOT/BOOTx64.efi" "$EFI_SRC/OC/OpenCore.efi" "$EFI_SRC/OC/config.plist" \
         "$REC_SRC/BaseSystem.dmg" "$REC_SRC/BaseSystem.chunklist"; do
    [ -s "$f" ] || die "$f is missing: run tools/baremetal/fetch.sh and build_efi.py first"
done

for f in $STICK_SCRIPTS; do
    [ -s "$STICK_SRC/$f" ] || die "$STICK_SRC/$f is missing (the helper script for macOS)"
done

echo
echo "  Device : $DEV"
echo "  Model  : ${vendor:+$vendor }${model:-(unknown)}"
echo "  Size   : $size_gb GB ($size_gib GiB)"
echo "  Bus    : ${tran:-?}, removable: ${rm_flag:-?}"
parts=$($LSBLK -nr -o NAME,SIZE "$DEV" 2>/dev/null | sed '1d' | tr '\n' ';' || true)
echo "  Layout : ${parts:-no partitions}"
echo "  Source : $EFI_SRC + $REC_SRC"
echo
echo "EVERYTHING on $DEV will be erased, the whole disk, and not read first."
if [ $FORCE = 1 ] && { [ "$tran" != "usb" ] || [ "$rm_flag" != "1" ]; }; then
    printf 'This is not a USB stick. To go on type: ERASE %s\n> ' "$DEV"
    read -r answer || answer=""
    [ "$answer" = "ERASE $DEV" ] || die "not confirmed, nothing was changed"
else
    printf 'To go on type the device name (%s): ' "${DEV#/dev/}"
    read -r answer || answer=""
    [ "$answer" = "${DEV#/dev/}" ] || [ "$answer" = "$DEV" ] || die "not confirmed, nothing was changed"
fi

# ---------------------------------------------------------------- the writing starts here
step "Unmounting the partitions of $DEV"
n=0
for p in $($LSBLK -nr -o NAME "$DEV" 2>/dev/null | sed '1d' || true); do
    if $FINDMNT -rn "/dev/$p" >/dev/null 2>&1; then
        echo "  umount /dev/$p"
        $UMOUNT "/dev/$p"
        n=$((n + 1))
    fi
done
echo "  $n unmounted"

step "Wiping $DEV and writing a GPT with one FAT32 partition (OPENCORE)"
echo "  removing old signatures"
$WIPEFS -a "$DEV" >/dev/null 2>&1 || true
if [ -z "$TEST_MODE" ]; then
    # the first and last 16 MiB: old partition tables and boot code (also the backup GPT)
    dd if=/dev/zero of="$DEV" bs=1M count=16 conv=fsync status=none
    tail_mb=$((size / 1048576 - 16))
    dd if=/dev/zero of="$DEV" bs=1M seek=$tail_mb count=16 conv=fsync status=none 2>/dev/null || true
fi
echo "  GPT, partition 1 = 1 MiB .. 100 %, EFI System"
$PARTED -s "$DEV" mklabel gpt
$PARTED -s "$DEV" mkpart OPENCORE fat32 1MiB 100%
$PARTED -s "$DEV" set 1 esp on
case "$DEV" in *[0-9]) PART=${DEV}p1 ;; *) PART=${DEV}1 ;; esac
if [ -z "$TEST_MODE" ]; then
    command -v partprobe >/dev/null && partprobe "$DEV" 2>/dev/null || true
    command -v udevadm >/dev/null && udevadm settle 2>/dev/null || true
    for _ in $(seq 1 20); do [ -b "$PART" ] && break; sleep 0.5; done
    [ -b "$PART" ] || die "the kernel did not show $PART after partitioning"
fi

step "Formatting $PART as FAT32, label OPENCORE"
$MKFS -F 32 -n OPENCORE "$PART" >/dev/null

mkmnt() { if [ -n "${MAKE_USB_TEST_MNT:-}" ]; then mkdir -p "$MAKE_USB_TEST_MNT"; echo "$MAKE_USB_TEST_MNT"; else mktemp -d; fi; }
rmmnt() { [ -n "${MAKE_USB_TEST_MNT:-}" ] || rmdir "$1" 2>/dev/null || true; }
cleanup() { if [ -n "${MNT:-}" ] && [ -d "$MNT" ]; then
                if mountpoint -q "$MNT" 2>/dev/null; then $UMOUNT "$MNT" || true; fi
                rmmnt "$MNT"; fi; }
MNT=$(mkmnt)
trap cleanup EXIT
$MOUNT "$PART" "$MNT"

# ---------------------------------------------------------------- copy with progress
FILES=$(mktemp)
(cd "$DIR" && { find efi/EFI -type f -print; find recovery -type f ! -name '.*' -print; }) | sort > "$FILES"
TOTAL_BYTES=0
while IFS= read -r f; do TOTAL_BYTES=$((TOTAL_BYTES + $(stat -c %s "$DIR/$f"))); done < "$FILES"
DONE_BYTES=0
NFILES=$(wc -l < "$FILES")

dest_of() {   # efi/EFI/OC/x -> EFI/OC/x ; recovery/x -> com.apple.recovery.boot/x
    case "$1" in
    efi/*) printf '%s\n' "${1#efi/}" ;;
    recovery/*) printf 'com.apple.recovery.boot/%s\n' "${1#recovery/}" ;;
    esac
}

progress() {   # progress LABEL DONE_OF_FILE FILE_SIZE
    local label=$1 d=$2 s=$3 pct all
    pct=$(( s > 0 ? d * 100 / s : 100 ))
    all=$(( TOTAL_BYTES > 0 ? (DONE_BYTES + d) * 100 / TOTAL_BYTES : 100 ))
    if [ -t 1 ]; then
        printf '\r  copying %s: %3d %% (%d/%d MB)   all: %3d %%   ' "$label" "$pct" $((d / 1048576)) $((s / 1048576)) "$all"
    elif [ $((pct / 10)) != "${LAST_PRINTED:-x}" ]; then
        LAST_PRINTED=$((pct / 10))
        printf '  copying %s: %d %% (%d/%d MB), all: %d %%\n' "$label" "$pct" $((d / 1048576)) $((s / 1048576)) "$all"
    fi
}

copy_big() {   # copy_big SRC DST LABEL: 8 MiB at a time, with a percentage
    local src=$1 dst=$2 label=$3 size done=0 blk=0
    size=$(stat -c %s "$src")
    : > "$dst"
    LAST_PRINTED=x
    while [ "$done" -lt "$size" ]; do
        dd if="$src" of="$dst" bs=1M skip=$((blk * CHUNK_MB)) seek=$((blk * CHUNK_MB)) count=$CHUNK_MB conv=notrunc,fsync status=none
        blk=$((blk + 1))
        done=$((blk * SMALL_FILE))
        [ "$done" -gt "$size" ] && done=$size
        progress "$label" "$done" "$size"
    done
    [ -t 1 ] && printf '\n'
    return 0
}

step "Copying OpenCore and the macOS recovery ($NFILES files, $((TOTAL_BYTES / 1048576)) MB)"
small=0 small_bytes=0
while IFS= read -r f; do
    d=$MNT/$(dest_of "$f")
    s=$(stat -c %s "$DIR/$f")
    mkdir -p "$(dirname "$d")"
    if [ "$s" -ge "$SMALL_FILE" ]; then
        copy_big "$DIR/$f" "$d" "$(basename "$f")"
    else
        cp "$DIR/$f" "$d"
        small=$((small + 1)); small_bytes=$((small_bytes + s))
    fi
    DONE_BYTES=$((DONE_BYTES + s))
done < "$FILES"
echo "  copied $small small files ($((small_bytes / 1024)) KB), all: 100 %"
rm -f "$FILES"
# rdna3/: whatever the user put in DIR/rdna3 (release tarball, metal/), then the helper
# scripts of this checkout (they win over a stale copy in DIR/rdna3)
mkdir -p "$MNT/rdna3"
if [ -d "$DIR/rdna3" ]; then cp -R "$DIR/rdna3/." "$MNT/rdna3/"; fi
for f in $STICK_SCRIPTS; do cp "$STICK_SRC/$f" "$MNT/rdna3/$f"; done
echo "  rdna3/: $STICK_SCRIPTS$([ -d "$DIR/rdna3" ] && echo " + $(cd "$DIR/rdna3" && find . -type f | wc -l) files of $DIR/rdna3")"

step "Flushing to the USB, this can take a minute (sync, unmount)"
$SYNC &
spid=$!
while kill -0 "$spid" 2>/dev/null; do printf '  still writing...\n'; sleep 3; done
wait "$spid" || true
$UMOUNT "$MNT"
rmmnt "$MNT"
MNT=""
echo "  unmounted: the stick can be removed after the check below"

step "Verifying the copy by reading it back"
if [ -z "$TEST_MODE" ] && [ -w /proc/sys/vm/drop_caches ]; then echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true; fi
MNT=$(mkmnt)
$MOUNT -o ro "$PART" "$MNT"
bad=0
for f in efi/EFI/OC/config.plist efi/EFI/OC/OpenCore.efi efi/EFI/BOOT/BOOTx64.efi \
         recovery/BaseSystem.chunklist recovery/BaseSystem.dmg; do
    d=$MNT/$(dest_of "$f")
    printf '  checking %s ... ' "$(basename "$f")"
    if cmp -s "$DIR/$f" "$d"; then echo ok; else echo "DIFFERENT"; bad=1; fi
done
for f in $STICK_SCRIPTS; do
    printf '  checking rdna3/%s ... ' "$f"
    if cmp -s "$STICK_SRC/$f" "$MNT/rdna3/$f"; then echo ok; else echo "DIFFERENT"; bad=1; fi
done
if [ -d "$DIR/rdna3" ]; then
    while IFS= read -r f; do
        case "$f" in install.sh|report.sh|metal.sh) continue ;; esac
        printf '  checking rdna3/%s ... ' "$f"
        if cmp -s "$DIR/rdna3/$f" "$MNT/rdna3/$f"; then echo ok; else echo "DIFFERENT"; bad=1; fi
    done < <(cd "$DIR/rdna3" && find . -type f | sed 's|^\./||' | sort)
fi
$UMOUNT "$MNT"
rmmnt "$MNT"
MNT=""
[ $bad = 0 ] || die "the copy on $DEV differs from $DIR: do not use this stick, run again (another port or stick if it repeats)"

echo
echo "Done. $DEV is a macOS installer stick (EFI/OC + com.apple.recovery.boot + rdna3/ helper scripts)."
echo "Next: docs/BAREMETAL.md, 'First boot'. Boot it from the BIOS boot menu (F11 on MSI), UEFI entry of the stick."
