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
#    message, GPT + FAT32 OPENCORE calls, the copy is byte for byte, a damaged copy is caught;
#    rdna3/ gets the helper scripts of tools/baremetal/stick/ (and DIR/rdna3's own files)
#  - the stick's helper scripts (install.sh, report.sh, metal.sh) against a fake macOS: the
#    tarball is unpacked and installed, the report never runs autoreport unless RDNA3_FULL=1,
#    metal.sh puts rdna3agdc=3 into the stick's config.plist and keeps its first backup
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf -- "$T"' EXIT
fail() { echo "test_baremetal: FAIL $*"; [ -f "$T/out.txt" ] && sed 's/^/  | /' "$T/out.txt"; exit 1; }
BM=$ROOT/tools/baremetal

bash -n "$BM/fetch.sh" || fail "bash -n fetch.sh"
bash -n "$BM/make_usb.sh" || fail "bash -n make_usb.sh"
for f in install report metal; do sh -n "$BM/stick/$f.sh" || fail "sh -n stick/$f.sh"; done

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
# byte 100 is fixed to A: the damaged-copy check below writes X there, which on random data was already X
# one run in 256 (CI run 122, macOS: "a damaged copy was not caught")
printf A | dd of="$D/recovery/BaseSystem.dmg" bs=1 seek=100 conv=notrunc status=none
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

# ---- a good run (DIR/rdna3 holds the release tarball, metal/ and a stale copy of a helper script)
mkdir -p "$D/rdna3/metal"
echo tarball > "$D/rdna3/RDNA3-Hackintosh-0.1.0-abc.tar.gz"; echo plugin > "$D/rdna3/metal/libvulkan_radeon.dylib"
echo stale > "$D/rdna3/metal.sh"
rm -rf "$STICK"
run "$USB" '' 'sda\n' /dev/sda --dir "$D" || fail "make_usb.sh failed on a good stick"
for f in install.sh report.sh metal.sh; do
    cmp "$BM/stick/$f" "$STICK/rdna3/$f" || fail "rdna3/$f is not the repo's helper script"
    grep -q "checking rdna3/$f ... ok" "$T/out.txt" || fail "rdna3/$f not verified"
done
cmp "$D/rdna3/RDNA3-Hackintosh-0.1.0-abc.tar.gz" "$STICK/rdna3/RDNA3-Hackintosh-0.1.0-abc.tar.gz" || fail "rdna3 tarball not copied"
cmp "$D/rdna3/metal/libvulkan_radeon.dylib" "$STICK/rdna3/metal/libvulkan_radeon.dylib" || fail "rdna3/metal/ not copied"
grep -q "checking rdna3/metal/libvulkan_radeon.dylib ... ok" "$T/out.txt" || fail "rdna3/metal/ not verified"
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

# ---- the stick's helper scripts against a fake macOS (tools tools/baremetal/stick/*.sh)
MB=$T/mbin
mkdir -p "$MB"
cat > "$MB/id" <<'EOF'
#!/bin/sh
[ "${FAKE_NOT_ROOT:-}" = 1 ] && { [ "$1" = -u ] && echo 1000 || exec /usr/bin/id "$@"; exit 0; }
[ "$1" = -u ] && echo 0 || /usr/bin/id "$@"
EOF
for t in log ioreg kmutil system_profiler nvram chown sync; do
    printf '#!/bin/sh\necho "fake %s $*"\n' "$t" > "$MB/$t"
done
cat > "$MB/sysctl" <<'EOF'
#!/bin/sh
echo "debug.rdna3_fake: 1"
EOF
# plutil -extract KEY raw -o - FILE | plutil -replace KEY -string VALUE FILE (keypath parts split on dots)
cat > "$MB/plutil" <<'EOF'
#!/usr/bin/env python3
import plistlib, sys
a = sys.argv[1:]
if a[0] == "-extract":
    with open(a[-1], "rb") as f:
        d = plistlib.load(f)
    for k in a[1].split("."):
        d = d[k]
    print(d)
elif a[0] == "-replace":
    with open(a[-1], "rb") as f:
        d = plistlib.load(f)
    path = a[1].split(".")
    x = d
    for k in path[:-1]:
        x = x[k]
    x[path[-1]] = a[3]
    with open(a[-1], "wb") as f:
        plistlib.dump(d, f)
else:
    sys.exit(1)
EOF
chmod +x "$MB"/*
SB=$T/mac                                       # the fake macOS: root of Library/, Users/
VOL=$T/vol/OPENCORE                             # the stick as macOS mounts it
mkdir -p "$VOL/rdna3/metal" "$VOL/EFI/OC"
cp "$BM"/stick/*.sh "$VOL/rdna3/"
sm() { env PATH="$MB:$PATH" RDNA3_SYSROOT=$SB RDNA3_STICK_TMP=$T/mtmp "$@"; }

# install.sh: refuses without root and without a tarball; unpacks and runs the release's install.sh
FAKE_NOT_ROOT=1 sm sh "$VOL/rdna3/install.sh" > "$T/out.txt" 2>&1 && fail "install.sh ran without root"
grep -q "uruchom z sudo" "$T/out.txt" || fail "install.sh: no sudo message"
sm sh "$VOL/rdna3/install.sh" > "$T/out.txt" 2>&1 && fail "install.sh ran without a tarball"
grep -q "brak RDNA3-Hackintosh" "$T/out.txt" || fail "install.sh: no message for a missing tarball"
mkdir -p "$T/rel/RDNA3-Hackintosh-0.1.0-abc"
printf '#!/bin/bash\necho installed-from-the-tarball > "%s/marker"\n' "$T" > "$T/rel/RDNA3-Hackintosh-0.1.0-abc/install.sh"
tar czf "$VOL/rdna3/RDNA3-Hackintosh-0.1.0-abc.tar.gz" -C "$T/rel" RDNA3-Hackintosh-0.1.0-abc
sm sh "$VOL/rdna3/install.sh" > "$T/out.txt" 2>&1 || fail "install.sh failed on a good tarball: $(cat "$T/out.txt")"
[ "$(cat "$T/marker")" = installed-from-the-tarball ] || fail "install.sh did not run the release's install.sh"
grep -q "^\[3/3\]" "$T/out.txt" || fail "install.sh: no step 3"
printf '#!/bin/bash\nexit 3\n' > "$T/rel/RDNA3-Hackintosh-0.1.0-abc/install.sh"
tar czf "$VOL/rdna3/RDNA3-Hackintosh-0.1.0-abc.tar.gz" -C "$T/rel" RDNA3-Hackintosh-0.1.0-abc
sm sh "$VOL/rdna3/install.sh" > "$T/out.txt" 2>&1 && fail "install.sh hid a failing release install"
grep -q "instalacja nie powiodla sie" "$T/out.txt" || fail "install.sh: no failure message"

# report.sh: writes into rdna3/report-*, WindowServer reports and .ips, no autoreport by default
mkdir -p "$SB/Library/Logs/DiagnosticReports" "$SB/Library/Application Support/RDNA3" "$SB/Users/Shared/rdna3-reports"
echo ws > "$SB/Library/Logs/DiagnosticReports/WindowServer-2026.ips"; echo other > "$SB/Library/Logs/DiagnosticReports/Other.ips"
echo r > "$SB/Users/Shared/rdna3-reports/x.txt"
printf '#!/bin/sh\necho "$*" > "%s/autoreport.args"\n' "$T" > "$SB/Library/Application Support/RDNA3/autoreport.sh"
sm sh "$VOL/rdna3/report.sh" > "$T/out.txt" 2>&1 || fail "report.sh failed: $(cat "$T/out.txt")"
rep=$(ls -d "$VOL"/rdna3/report-*/ | head -1)
[ -n "$rep" ] || fail "report.sh made no report directory on the stick"
for f in rdna3-3h.log kernel-3h.log boot.log ioreg.txt kexts.txt displays.txt sysctl.txt bootargs.txt diag-list.txt \
         WindowServer-2026.ips Other.ips; do
    [ -e "$rep/$f" ] || fail "report.sh: no $f"
done
[ -e "$rep/rdna3-reports/x.txt" ] || fail "report.sh: /Users/Shared/rdna3-reports not copied"
grep -q rdna3 "$rep/sysctl.txt" || fail "report.sh: sysctl rdna3 empty"
[ -e "$rep/trace-stream.bin" ] && [ -e "$rep/trace.err" ] || fail "report.sh: no kernel frame trace (N68)"
grep -q "NOT a binary trace" "$rep/trace.err" || fail "report.sh: a non-binary trace not said: $(cat "$rep/trace.err")"
[ -e "$rep/31-plugin-trace/none.txt" ] || fail "report.sh: no plugin traces not said"
[ ! -e "$T/autoreport.args" ] || fail "report.sh ran autoreport.sh without RDNA3_FULL=1"
RDNA3_FULL=1 RDNA3_REPORT_DIR=$T/full sm sh "$VOL/rdna3/report.sh" > "$T/out.txt" 2>&1 || fail "report.sh RDNA3_FULL=1 failed"
[ "$(cat "$T/autoreport.args")" = "--standalone" ] || fail "RDNA3_FULL=1 did not run autoreport.sh --standalone"

# metal.sh: refuses without install.sh's files / the stick's metal files; then the config change
python3 - "$VOL/EFI/OC/config.plist" <<'PY'
import plistlib, sys
g = "7C436110-AB2A-4BBB-A880-FE41995C9F82"
plistlib.dump({"NVRAM": {"Add": {g: {"boot-args": "-v keepsyms=1 alcid=13 amfi=0x80"}}}}, open(sys.argv[1], "wb"))
PY
ORIG=$(cat "$VOL/EFI/OC/config.plist" | md5sum)
sm sh "$VOL/rdna3/metal.sh" > "$T/out.txt" 2>&1 && fail "metal.sh ran before install.sh"
grep -q "najpierw sudo sh" "$T/out.txt" || fail "metal.sh: no 'install first' message"
mkdir -p "$SB/Library/GPUBundles/rdna3mtl" "$SB/Library/Application Support/RDNA3/tools/macos"
# like the real sync_metal: --sync-metal DIR puts DIR's RADV into the plugin's directory
printf '#!/bin/sh\necho "$*" >> "%s/sync.args"\n[ "$1" = --sync-metal ] && cp "$2/libvulkan_radeon.dylib" "%s/Library/GPUBundles/rdna3mtl/"\nexit 0\n' "$T" "$SB" > "$SB/Library/Application Support/RDNA3/autoreport.sh"
printf '#!/bin/sh\necho "tracedir $*" >> "%s/sync.args"\n' "$T" > "$SB/Library/Application Support/RDNA3/tools/macos/fix_tracedir.sh"
sm sh "$VOL/rdna3/metal.sh" > "$T/out.txt" 2>&1 && fail "metal.sh ran without the plugin files"
grep -q "brak .*metal/RDNA3MTLDriver.bundle" "$T/out.txt" || fail "metal.sh: no message for a missing plugin: $(cat "$T/out.txt")"
[ "$(md5sum < "$VOL/EFI/OC/config.plist")" = "${ORIG}" ] || fail "metal.sh changed config.plist before the files were there"
mkdir -p "$VOL/rdna3/metal/RDNA3MTLDriver.bundle/Contents/MacOS"
echo bin > "$VOL/rdna3/metal/RDNA3MTLDriver.bundle/Contents/MacOS/RDNA3MTLDriver"
for f in librdna3mtl_translate.dylib libvulkan_radeon.dylib rdna3mtl-allow.txt; do echo "$f" > "$VOL/rdna3/metal/$f"; done
sm sh "$VOL/rdna3/metal.sh" > "$T/out.txt" 2>&1 || fail "metal.sh failed: $(cat "$T/out.txt")"
GBX=$SB/Library/GPUBundles
[ -f "$GBX/RDNA3MTLDriver.bundle/Contents/MacOS/RDNA3MTLDriver" ] && [ -f "$GBX/rdna3mtl/librdna3mtl_translate.dylib" ] \
    && [ -f "$GBX/rdna3mtl-allow.txt" ] || fail "metal.sh did not install plugin / translator / allow list"
grep -q -- "--sync-metal $T/mtmp/rdna3-radv" "$T/sync.args" && [ -f "$T/mtmp/rdna3-radv/libvulkan_radeon.dylib" ] \
    || fail "metal.sh: RADV not handed to autoreport.sh --sync-metal: $(cat "$T/sync.args")"
grep -q "^tracedir $SB" "$T/sync.args" || fail "metal.sh did not run fix_tracedir.sh"
want='-v keepsyms=1 alcid=13 amfi=0x80 rdna3agdc=3'
[ "$(sm plutil -extract NVRAM.Add.7C436110-AB2A-4BBB-A880-FE41995C9F82.boot-args raw -o - "$VOL/EFI/OC/config.plist")" = "$want" ] \
    || fail "metal.sh: boot-args are not '$want'"
BAKF=$VOL/EFI/OC/config-before-metal.plist
[ "$(md5sum < "$BAKF")" = "${ORIG}" ] || fail "metal.sh: config-before-metal.plist is not the original config.plist"
# a second run: the same boot-args, the first backup stays (it is the way back)
sm sh "$VOL/rdna3/metal.sh" > "$T/out.txt" 2>&1 || fail "metal.sh second run failed"
[ "$(md5sum < "$BAKF")" = "${ORIG}" ] || fail "metal.sh second run overwrote config-before-metal.plist"
[ "$(sm plutil -extract NVRAM.Add.7C436110-AB2A-4BBB-A880-FE41995C9F82.boot-args raw -o - "$VOL/EFI/OC/config.plist")" = "$want" ] \
    || fail "metal.sh second run changed the boot-args"
grep -q "juz jest, zostaje" "$T/out.txt" || fail "metal.sh second run: no word on the kept backup"
# an rdna3agdc of an earlier try is replaced, not doubled
sm plutil -replace NVRAM.Add.7C436110-AB2A-4BBB-A880-FE41995C9F82.boot-args -string '-v rdna3agdc=1 amfi=0x80' "$VOL/EFI/OC/config.plist"
sm sh "$VOL/rdna3/metal.sh" > "$T/out.txt" 2>&1 || fail "metal.sh third run failed"
[ "$(sm plutil -extract NVRAM.Add.7C436110-AB2A-4BBB-A880-FE41995C9F82.boot-args raw -o - "$VOL/EFI/OC/config.plist")" = '-v rdna3agdc=3 amfi=0x80' ] \
    || fail "metal.sh did not replace rdna3agdc=1 by 3"

echo "test_baremetal: ok"
