#!/bin/bash
# tools/baremetal: fetch.sh pins, make_usb.sh refusals and step messages, build_efi.py (python test).
# No root, no device, no network: lsblk / parted / mkfs / mount are fakes (MAKE_USB_* hooks) and the
# "stick" is a plain directory. Checked:
#  - pins.txt: 64-hex sha256s, https URLs, no duplicates; tools/baremetal/SOURCE carries every sha;
#    Lilu / RestrictEvents agree with tools/vm/data/oc_kexts.SOURCE and vmtool.py
#  - fetch.sh: a wrong sha256 is refused and the file is not kept (file:// server)
#  - make_usb.sh refuses, writes nothing (no parted / wipefs / mkfs call): a non-USB disk, a
#    partition, a disk with / or /boot mounted, a too small stick, missing EFI / recovery,
#    a wrong or empty confirmation; --force-not-usb needs "ERASE /dev/x"
#  - make_usb.sh on a good stick: steps [1/7]..[7/7], percentages for the big file, the flush
#    message, GPT + FAT32 OPENCORE calls, the copy is byte for byte, a damaged copy is caught
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf -- "$T"' EXIT
fail() { echo "test_baremetal: FAIL $*"; [ -f "$T/out.txt" ] && sed 's/^/  | /' "$T/out.txt"; exit 1; }
BM=$ROOT/tools/baremetal

bash -n "$BM/fetch.sh" || fail "bash -n fetch.sh"
bash -n "$BM/make_usb.sh" || fail "bash -n make_usb.sh"

# ---- pins
n=0
while read -r name ver sha url; do
    case "$name" in ''|'#'*) continue ;; esac
    n=$((n + 1))
    [[ "$sha" =~ ^[0-9a-f]{64}$ ]] || fail "pins.txt: $name has no sha256"
    case "$url" in https://*) ;; *) fail "pins.txt: $name url is not https" ;; esac
    grep -q "$sha" "$BM/SOURCE" || fail "SOURCE does not carry the sha256 of $name"
    [ "$(grep -c "^$name " "$BM/pins.txt")" = 1 ] || fail "pins.txt: $name twice"
done < "$BM/pins.txt"
[ "$n" -ge 9 ] || fail "pins.txt has only $n pins"
for k in Lilu RestrictEvents; do
    v=$(awk -v k="$k" '$1 == k { print $2 }' "$BM/pins.txt")
    sha=$(awk -v k="$k" '$1 == k { print $3 }' "$BM/pins.txt")
    grep -q "$k .*$v" "$ROOT/tools/vm/data/oc_kexts.SOURCE" || fail "$k $v is not the VM's pin"
    grep -q "$sha" "$ROOT/tools/vm/data/oc_kexts.SOURCE" || fail "$k sha256 is not the VM's"
    grep -q "$sha" "$ROOT/tools/vm/vmtool.py" || fail "$k sha256 is not in vmtool.py"
done

# ---- fetch.sh refuses a file whose sha256 is wrong
mkdir -p "$T/srv" "$T/fo"
echo "tampered" > "$T/srv/x.zip"
printf 'Fake 1.0 %064d file://%s/x.zip\n' 0 "$T/srv" > "$T/pins.txt"
if PINS_FILE=$T/pins.txt RECOVERY_SKIP=1 "$BM/fetch.sh" "$T/fo" > "$T/out.txt" 2>&1; then fail "fetch.sh accepted a wrong sha256"; fi
grep -q "sha256" "$T/out.txt" || fail "fetch.sh: no sha256 message"
[ ! -e "$T/fo/dl/x.zip" ] && [ ! -e "$T/fo/dl/x.zip.part" ] || fail "fetch.sh kept a file with a wrong sha256"

# ---- the build_efi.py test
python3 -I "$ROOT/tests/test_baremetal_efi.py" > "$T/out.txt" 2>&1 || fail "test_baremetal_efi.py"

# ---- make_usb.sh with fakes
FAKE=$T/fake
mkdir -p "$FAKE"
LOG=$T/calls.log
cat > "$FAKE/lsblk" <<'EOF'
#!/bin/bash
# fake lsblk: FAKE_INFO is the -P line of the disk, FAKE_MOUNTS its mountpoints
case "$*" in
*"-P"*) printf '%s\n' "$FAKE_INFO" ;;
*MOUNTPOINTS*) printf '%b' "${FAKE_MOUNTS:-}" ;;
*NAME,SIZE*) printf 'sda 57.3G\nsda1 2G\nsda2 55.3G\n' ;;
*NAME*) printf 'sda\nsda1\nsda2\n' ;;
esac
EOF
for t in parted wipefs mkfs.fat mount umount findmnt; do
    cat > "$FAKE/$t" <<EOF
#!/bin/bash
echo "$t \$*" >> "$LOG"
[ "$t" = findmnt ] && exit 1
exit 0
EOF
done
cat > "$FAKE/sync" <<EOF
#!/bin/bash
echo "sync" >> "$LOG"
sleep 0.1
EOF
chmod +x "$FAKE"/*

# a finished fetch.sh / build_efi.py tree: 21 MB "recovery" so the chunked copy runs more than once
D=$T/dir
mkdir -p "$D/efi/EFI/BOOT" "$D/efi/EFI/OC/Kexts/X.kext" "$D/recovery"
echo efi > "$D/efi/EFI/BOOT/BOOTx64.efi"; echo oc > "$D/efi/EFI/OC/OpenCore.efi"
echo "<plist/>" > "$D/efi/EFI/OC/config.plist"; echo k > "$D/efi/EFI/OC/Kexts/X.kext/exe"
head -c 21000000 /dev/urandom > "$D/recovery/BaseSystem.dmg"; echo chunks > "$D/recovery/BaseSystem.chunklist"
: > "$D/recovery/.complete"
STICK=$T/stick
PROC_MOUNTS=$T/proc_mounts     # the kernel's mount table, a fake (empty unless a test fills it)
: > "$PROC_MOUNTS"

NV0='NAME="nvme0n1" RM="0" TRAN="nvme" TYPE="disk" SIZE="1000204886016" MODEL="Crucial" VENDOR=""'
USB='NAME="sda" RM="1" TRAN="usb" TYPE="disk" SIZE="61530439680" MODEL="Ultra Fit" VENDOR="SanDisk"'
run() {   # run INFO MOUNTS INPUT args...: make_usb.sh in test mode, all output in out.txt
    local info=$1 mounts=$2 input=$3; shift 3
    : > "$LOG"
    FAKE_INFO=$info FAKE_MOUNTS=$mounts MAKE_USB_TEST_MODE=1 MAKE_USB_TEST_MNT=$STICK MAKE_USB_PROC_MOUNTS=$PROC_MOUNTS \
    MAKE_USB_LSBLK=$FAKE/lsblk MAKE_USB_PARTED=$FAKE/parted MAKE_USB_WIPEFS=$FAKE/wipefs \
    MAKE_USB_MKFS=$FAKE/mkfs.fat MAKE_USB_MOUNT=$FAKE/mount MAKE_USB_UMOUNT=$FAKE/umount \
    MAKE_USB_SYNC=$FAKE/sync MAKE_USB_FINDMNT=$FAKE/findmnt \
        bash "$BM/make_usb.sh" "$@" < <(printf "$input") > "$T/out.txt" 2>&1
}
refused() {   # refused "what" "message" INFO MOUNTS INPUT args...
    local what=$1 msg=$2; shift 2
    rm -rf "$STICK"
    if run "$@"; then fail "make_usb.sh did not refuse: $what"; fi
    grep -q -- "$msg" "$T/out.txt" || fail "make_usb.sh refusal for '$what' lacks '$msg'"
    if grep -qE "^(parted|wipefs|mkfs.fat|mount) " "$LOG"; then fail "make_usb.sh touched the device despite refusing: $what: $(cat "$LOG")"; fi
    [ ! -e "$STICK/EFI" ] || fail "make_usb.sh copied files despite refusing: $what"
}

refused "an NVMe disk" "not a removable USB disk" \
    'NAME="nvme0n1" RM="0" TRAN="nvme" TYPE="disk" SIZE="1000204886016" MODEL="Crucial" VENDOR=""' '' 'nvme0n1\n' /dev/nvme0n1 --dir "$D"
refused "a fixed disk on USB" "not a removable USB disk" \
    'NAME="sdb" RM="0" TRAN="usb" TYPE="disk" SIZE="61530439680" MODEL="Portable SSD" VENDOR=""' '' 'sdb\n' /dev/sdb --dir "$D"
refused "a partition" "not a whole disk" \
    'NAME="sda1" RM="1" TRAN="usb" TYPE="part" SIZE="61530439680" MODEL="" VENDOR=""' '' 'sda1\n' /dev/sda1 --dir "$D"
refused "system mounts" "system partition mounted" "$USB" '/boot\n' 'sda\n' /dev/sda --dir "$D"
refused "root mount" "system partition mounted" "$USB" '/\n' 'sda\n' /dev/sda --dir "$D"
# lsblk says nothing (old util-linux, a failing call): the kernel's mount table still stops it
printf '/dev/sda2 /boot ext4 rw 0 0\n/dev/sdb1 /home ext4 rw 0 0\n' > "$PROC_MOUNTS"
refused "a system mount only in /proc/mounts" "system partition mounted at /boot" "$USB" '' 'sda\n' /dev/sda --dir "$D"
printf '/dev/nvme0n1p2 / ext4 rw 0 0\n' > "$PROC_MOUNTS"
refused "root on nvme0n1p2 (name with p)" "system partition mounted at /" "$NV0" '' 'x\n' /dev/nvme0n1 --force-not-usb --dir "$D"
printf '/dev/sdb1 /home ext4 rw 0 0\n/dev/sda1 /run/media/u/OPENCORE vfat rw 0 0\n' > "$PROC_MOUNTS"
run "$USB" '' 'sda\n' /dev/sda --dir "$D" || fail "another disk's /home or the stick's own mount stopped a good run: $(cat "$T/out.txt")"
: > "$PROC_MOUNTS"
refused "a 1 GB stick" "4 GiB to 2 TiB" \
    'NAME="sda" RM="1" TRAN="usb" TYPE="disk" SIZE="1000000000" MODEL="Tiny" VENDOR=""' '' 'sda\n' /dev/sda --dir "$D"
mkdir -p "$T/empty"
refused "no EFI / recovery" "is missing" "$USB" '' 'sda\n' /dev/sda --dir "$T/empty"
refused "a wrong confirmation" "not confirmed" "$USB" '' 'sdb\n' /dev/sda --dir "$D"
refused "an empty confirmation" "not confirmed" "$USB" '' '' /dev/sda --dir "$D"
refused "yes as the confirmation" "not confirmed" "$USB" '' 'yes\n' /dev/sda --dir "$D"
NV='NAME="nvme9n1" RM="0" TRAN="nvme" TYPE="disk" SIZE="1000204886016" MODEL="Crucial" VENDOR=""'
refused "--force-not-usb with the device name only" "not confirmed" "$NV" '' 'nvme9n1\n' /dev/nvme9n1 --force-not-usb --dir "$D"
refused "--force-not-usb on a mounted /boot" "system partition mounted" "$NV" '/boot\n' 'ERASE /dev/nvme9n1\n' /dev/nvme9n1 --force-not-usb --dir "$D"

# ---- a good run
rm -rf "$STICK"
run "$USB" '' 'sda\n' /dev/sda --dir "$D" || fail "make_usb.sh failed on a good stick"
for i in 1 2 3 4 5 6 7; do grep -q "^\[$i/7\] " "$T/out.txt" || fail "no step message [$i/7]"; done
grep -q "Model  : SanDisk Ultra Fit" "$T/out.txt" || fail "model not shown"
grep -q "Size   : 61 GB" "$T/out.txt" || fail "size not shown"
grep -q "copying BaseSystem.dmg: 100 %" "$T/out.txt" || fail "no percentage for the recovery copy"
grep -qE "copying BaseSystem.dmg: [0-9]+ % \([0-9]+/20 MB\)" "$T/out.txt" || fail "no MB count in the progress line"
grep -q "all: 100 %" "$T/out.txt" || fail "no overall percentage"
grep -q "Flushing to the USB, this can take a minute" "$T/out.txt" || fail "no flush message"
grep -q "checking BaseSystem.dmg ... ok" "$T/out.txt" || fail "no verification line"
grep -q "^wipefs -a /dev/sda" "$LOG" || fail "no wipefs"
grep -q "^parted -s /dev/sda mklabel gpt" "$LOG" || fail "no GPT"
grep -q "^parted -s /dev/sda mkpart OPENCORE fat32 1MiB 100%" "$LOG" || fail "no partition"
grep -q "^mkfs.fat -F 32 -n OPENCORE /dev/sda1" "$LOG" || fail "no FAT32 OPENCORE: $(cat "$LOG")"
grep -q "^sync" "$LOG" || fail "no sync"
cmp "$D/recovery/BaseSystem.dmg" "$STICK/com.apple.recovery.boot/BaseSystem.dmg" || fail "recovery not copied byte for byte"
cmp "$D/recovery/BaseSystem.chunklist" "$STICK/com.apple.recovery.boot/BaseSystem.chunklist" || fail "chunklist differs"
cmp "$D/efi/EFI/OC/config.plist" "$STICK/EFI/OC/config.plist" || fail "config.plist differs"
cmp "$D/efi/EFI/OC/Kexts/X.kext/exe" "$STICK/EFI/OC/Kexts/X.kext/exe" || fail "nested kext file differs"
[ -f "$STICK/EFI/BOOT/BOOTx64.efi" ] || fail "no BOOTx64.efi"
[ ! -e "$STICK/com.apple.recovery.boot/.complete" ] || fail "the marker file was copied"

# --force-not-usb with the right words goes through
rm -rf "$STICK"
run "$NV" '' 'ERASE /dev/nvme9n1\n' /dev/nvme9n1 --force-not-usb --dir "$D" || fail "--force-not-usb with the typed words refused"
grep -q "WARNING: /dev/nvme9n1 is NOT a removable USB disk" "$T/out.txt" || fail "no warning for --force-not-usb"
grep -q "^mkfs.fat -F 32 -n OPENCORE /dev/nvme9n1p1" "$LOG" || fail "NVMe partition name: $(cat "$LOG")"

# a damaged copy is caught: the fake umount corrupts the file on the "stick"
cat > "$FAKE/umount" <<EOF
#!/bin/bash
echo "umount \$*" >> "$LOG"
[ -f "$STICK/com.apple.recovery.boot/BaseSystem.dmg" ] && printf 'X' | dd of="$STICK/com.apple.recovery.boot/BaseSystem.dmg" bs=1 seek=100 conv=notrunc status=none
exit 0
EOF
rm -rf "$STICK"
if run "$USB" '' 'sda\n' /dev/sda --dir "$D"; then fail "a damaged copy was not caught"; fi
grep -q "DIFFERENT" "$T/out.txt" && grep -q "do not use this stick" "$T/out.txt" || fail "no message for a damaged copy"

echo "test_baremetal: ok"
