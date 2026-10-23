#!/bin/bash
# The present-mode A/B of tools/vm/rdna3vm (card run 2026-10-17d: WindowServer
# presents with page flips; the VM has a laptop SMBIOS model and the card on PCI
# bus 0, which makes IOGraphics set IOFBIntegrated). No qemu, no root, no card:
#  - vmtool.py plist --smbios: only PlatformInfo/Generic/SystemProductName changes
#    (serial, MLB, UUID, ROM and the other SystemProductName keys stay), a model
#    that is not one, and PlatformInfo/Automatic false, are refused
#  - the host-only @smbios= / @gpubus= tokens: read, validated, stripped from the
#    boot-args macOS gets, never reaching package/oc_install.sh's bare-metal boot-args
#  - the whole cmd__oc chain with stubbed losetup / mount: a step's tokens end as
#    boot-args without them and the model in the OpenCore config
#  - the ladder steps play-macpro / play-rootport / play-desktop, in both step lists,
#    with the play step's 30 min limit
#  - GPU_BUS=rootport: the root port's windows from a fake sysfs (256 MiB BAR0,
#    16 GiB BAR0, nothing readable), the pcie0 arguments unchanged, the audio
#    function on the same port, q35's 64-bit hole, the too-small-window warning
#  - mmio64_mb (OVMF's 64-bit window) as before the move into a function
#  - smbios_note: a model change since the last card run is flagged
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf -- "$T"' EXIT
fail() { echo "test_vm_present_ab: FAIL $*"; [ -f "$T/out.txt" ] && sed 's/^/  | /' "$T/out.txt"; exit 1; }
VM=$ROOT/tools/vm/rdna3vm
VT="python3 -I $ROOT/tools/vm/vmtool.py"
bash -n "$VM" || fail "bash -n rdna3vm"
bash -n "$ROOT/tools/probe/coredisplay.sh" || fail "bash -n coredisplay.sh"

# ---- vmtool.py plist --smbios
P=$T/config.plist
cp "$ROOT/tests/data/oc-config-smbios.plist" "$P"
[ "$($VT plist "$P" --show-smbios)" = "MacBookPro16,1" ] || fail "--show-smbios of the fixture"
cp "$P" "$T/before.plist"
$VT plist "$P" --smbios MacPro7,1 --boot-args "-v rdna3lvl=8" > "$T/out.txt"
grep -qx 'smbios: MacPro7,1' "$T/out.txt" || fail "plist did not print the model"
[ "$($VT plist "$P" --show-smbios)" = "MacPro7,1" ] || fail "model not written"
[ "$($VT plist "$P" --show)" = "-v rdna3lvl=8" ] || fail "--boot-args next to --smbios"
# exactly that one line differs, in the Generic section; every other key is byte for byte
diff "$T/before.plist" "$P" | grep -E '^[<>]' | grep -vE 'MacBookPro16,1|MacPro7,1|boot-args|-v rdna3lvl=8|-v keepsyms=1|<string>-v' > "$T/rest.txt" || true
[ ! -s "$T/rest.txt" ] || fail "--smbios changed more than the model: $(cat "$T/rest.txt")"
for k in MLB-SYNTHETIC-0001 SERIAL-SYNTH-01 00000000-0000-0000-0000-000000000001 ESIzRFVm DATAHUB-UNTOUCHED SMBIOS-UNTOUCHED; do
    grep -q "$k" "$P" || fail "$k lost"
done
$VT plist "$P" --smbios iMacPro1,1 >/dev/null
[ "$($VT plist "$P" --show-smbios)" = "iMacPro1,1" ] || fail "second change"
for bad in "MacPro 7,1" "x" "MacPro7" "../etc" "MacPro7,1</string>"; do
    ! $VT plist "$P" --smbios "$bad" >"$T/out.txt" 2>&1 || fail "model [$bad] accepted"
    grep -q "vmtool: SMBIOS model" "$T/out.txt" || fail "no message for [$bad]: $(cat "$T/out.txt")"
done
[ "$($VT plist "$P" --show-smbios)" = "iMacPro1,1" ] || fail "a refused model changed the file"
python3 -I - "$ROOT/tests/data/oc-config-smbios.plist" "$T/auto-false.plist" <<'PY'
import sys
t = open(sys.argv[1]).read()
a = "<key>Automatic</key>\n\t\t<true/>"
assert a in t
open(sys.argv[2], "w").write(t.replace(a, "<key>Automatic</key>\n\t\t<false/>"))
PY
grep -A1 '<key>Automatic</key>' "$T/auto-false.plist" | grep -q '<false/>' || fail "fixture for Automatic false"
! $VT plist "$T/auto-false.plist" --smbios MacPro7,1 >"$T/out.txt" 2>&1 || fail "Automatic false accepted"
grep -q "PlatformInfo/Automatic is false" "$T/out.txt" || fail "no Automatic message: $(cat "$T/out.txt")"
printf '<plist><dict><key>NVRAM</key><dict></dict></dict></plist>\n' > "$T/nopi.plist"
! $VT plist "$T/nopi.plist" --smbios MacPro7,1 >"$T/out.txt" 2>&1 || fail "no PlatformInfo accepted"
grep -q "no PlatformInfo dict" "$T/out.txt" || fail "no PlatformInfo message: $(cat "$T/out.txt")"

# ---- the helpers of rdna3vm, from the script itself
fn() { sed -n "/^$1() {/,/^}/p" "$VM"; }
{
    sed -n '/^SMBIOS_DEFAULT=/,/^PCI_SYSFS=/p' "$VM"
    for f in host_opt strip_host_opts smbios_model gpu_bus mmio64_mb qemu_size pow2_at_least rootport_plan gpu_qargs smbios_note; do
        fn $f
    done
} > "$T/fns.sh"
for f in host_opt smbios_model gpu_bus mmio64_mb rootport_plan gpu_qargs smbios_note; do
    grep -q "^$f() {" "$T/fns.sh" || fail "function $f not found in rdna3vm"
done
h() { RDNA3VM_PCI_SYSFS="$T/sys" WORK="$T/work" CONF=$T/config bash -c '. "$1"; shift; "$@"' _ "$T/fns.sh" "$@"; }

[ "$(h host_opt smbios "a @smbios=MacPro7,1 b")" = "MacPro7,1" ] || fail "host_opt"
[ "$(h host_opt gpubus "a @smbios=MacPro7,1 b")" = "" ] || fail "host_opt of a missing token"
[ "$(h host_opt smbios "@smbios=iMacPro1,1 x @smbios=MacPro7,1")" = "MacPro7,1" ] || fail "host_opt: the last wins"
[ "$(h strip_host_opts "-v rdna3lvl=8 @smbios=MacPro7,1 rdna3play=1 @gpubus=rootport")" = "-v rdna3lvl=8 rdna3play=1" ] || fail "strip_host_opts"
[ "$(h strip_host_opts "-v amfi=0x80")" = "-v amfi=0x80" ] || fail "strip_host_opts changed plain args"
[ "$(h smbios_model)" = "MacBookPro16,1" ] || fail "smbios_model default"
[ "$(h smbios_model "rdna3play=1 @smbios=MacPro7,1")" = "MacPro7,1" ] || fail "smbios_model of a step"
[ "$(SMBIOS_MODEL=iMacPro1,1 h smbios_model)" = "iMacPro1,1" ] || fail "smbios_model from the config"
[ "$(SMBIOS_MODEL=iMacPro1,1 h smbios_model "@smbios=MacPro7,1")" = "MacPro7,1" ] || fail "a step beats the config"
! h smbios_model "@smbios=Macintosh1,1" >"$T/out.txt" 2>&1 || fail "unknown model accepted"
grep -q "unknown SMBIOS model 'Macintosh1,1'" "$T/out.txt" || fail "no message: $(cat "$T/out.txt")"
[ "$(h gpu_bus)" = "pcie0" ] || fail "gpu_bus default"
[ "$(h gpu_bus "@gpubus=rootport")" = "rootport" ] || fail "gpu_bus of a step"
[ "$(GPU_BUS=rootport h gpu_bus)" = "rootport" ] || fail "gpu_bus from the config"
! h gpu_bus "@gpubus=pcie1" >"$T/out.txt" 2>&1 || fail "unknown bus accepted"
grep -q "unknown GPU bus 'pcie1'" "$T/out.txt" || fail "no bus message"

# ---- OVMF's 64-bit window (moved into a function, same numbers)
[ "$(h mmio64_mb 0 256)" = 0 ] || fail "mmio64_mb: 256 MiB BAR0 keeps no window"
[ "$(h mmio64_mb 0 2048)" = 8192 ] || fail "mmio64_mb: 2 GiB BAR0"
[ "$(h mmio64_mb 0 16384)" = 65536 ] || fail "mmio64_mb: 16 GiB BAR0"
[ "$(h mmio64_mb 0 512)" = 4096 ] || fail "mmio64_mb: the 4 GiB floor"
[ "$(h mmio64_mb default 16384)" = default ] || fail "mmio64_mb: default"
[ "$(h mmio64_mb 12345 16384)" = 12345 ] || fail "mmio64_mb: a number from the config"

# ---- a fake sysfs for the root port: the card (VGA, BAR0 256 MiB) and its HDMI audio function
mkres() {   # mkres FILE bar0-size-hex: BAR0 pref 64-bit, BAR2 2 MiB pref, BAR5 512 KiB, ROM 128 KiB
    local f=$1 b0=$2
    {
        printf '0x0000000000000000 0x%016x 0x000000000014220c\n' $((b0 - 1))
        printf '0x0000000000000000 0x0000000000000000 0x0000000000000000\n'
        printf '0x0000000000000000 0x00000000001fffff 0x000000000014220c\n'
        printf '0x0000000000000000 0x0000000000000000 0x0000000000000000\n'
        printf '0x0000000000000000 0x0000000000000000 0x0000000000000000\n'
        printf '0x0000000000000000 0x000000000007ffff 0x0000000000040200\n'
        printf '0x0000000000000000 0x000000000001ffff 0x0000000000046200\n'
    } > "$f"
}
mkdir -p "$T/sys/0000:2d:00.0" "$T/sys/0000:2d:00.1" "$T/sys/0000:2e:00.0"
mkres "$T/sys/0000:2d:00.0/resource" $((256 << 20))
printf '0x0000000000000000 0x0000000000003fff 0x0000000000040200\n' > "$T/sys/0000:2d:00.1/resource"
printf '0x0000000000000000 0x0000000000000fff 0x0000000000040200\n' > "$T/sys/0000:2e:00.0/resource"
[ "$(h rootport_plan 0 0000:2d:00.0 0000:2d:00.1)" = "4 512 pref32" ] || fail "rootport_plan 256 MiB, no 64-bit window: $(h rootport_plan 0 0000:2d:00.0 0000:2d:00.1)"
[ "$(h rootport_plan 4096 0000:2d:00.0 0000:2d:00.1)" = "4 512 pref64" ] || fail "rootport_plan 256 MiB with a 64-bit window"
mkres "$T/sys/0000:2d:00.0/resource" $((16384 << 20))
[ "$(h rootport_plan 65536 0000:2d:00.0 0000:2d:00.1)" = "4 32768 pref64" ] || fail "rootport_plan 16 GiB: $(h rootport_plan 65536 0000:2d:00.0 0000:2d:00.1)"
[ "$(h rootport_plan 0 0000:2d:00.0 0000:2d:00.1)" = "4 32768 pref64" ] || fail "rootport_plan: a BAR0 above 4 GiB cannot be pref32"
[ "$(h rootport_plan 0 0000:99:00.0)" = "2 512 pref32" ] || fail "rootport_plan: nothing readable, no window: $(h rootport_plan 0 0000:99:00.0)"
[ "$(h rootport_plan 8192 0000:99:00.0)" = "2 32768 pref64" ] || fail "rootport_plan: nothing readable, 64-bit window"
[ "$(h qemu_size 2)" = 2M ] && [ "$(h qemu_size 512)" = 512M ] && [ "$(h qemu_size 32768)" = 32G ] && [ "$(h qemu_size 1536)" = 1536M ] ||
    fail "qemu_size"
mkres "$T/sys/0000:2d:00.0/resource" $((256 << 20))

# ---- the QEMU arguments of the card (gpu_qargs)
cat > "$T/q.sh" <<'EOF'
. "$FNS"
WORK=$W; USE_ROMFILE=${USE_ROMFILE:-0}
DEVS=("$@"); QARGS=()
gpu_qargs 0000:2d:00.0 "$BUS" "$MM"
printf '%s\n' "${QARGS[@]}"
EOF
q() {   # q BUS MM DEV...
    local bus=$1 mm=$2; shift 2
    RDNA3VM_PCI_SYSFS="$T/sys" FNS="$T/fns.sh" W="$T/work" BUS=$bus MM=$mm bash "$T/q.sh" "$@" 2>&1
}
mkdir -p "$T/work"
q pcie0 0 0000:2d:00.0 0000:2d:00.1 > "$T/out.txt"
[ "$(grep -c . "$T/out.txt")" -ge 5 ] || fail "q pcie0: too little"
grep -qx -- '  GPU on: pcie0 (root bus 00:10.0: PCI bus 0)' "$T/out.txt" || fail "pcie0: 'GPU on' line"
grep -qx -- 'vfio-pci,host=0000:2d:00.0,bus=pcie.0,addr=10.0,multifunction=on,x-no-kvm-intx=on' "$T/out.txt" || fail "pcie0: the card"
grep -qx -- 'vfio-pci,host=0000:2d:00.1,bus=pcie.0,addr=10.1' "$T/out.txt" || fail "pcie0: the audio function"
! grep -q 'root-port\|-global' "$T/out.txt" || fail "pcie0: root port or hole arguments (the default must not change)"
q rootport 0 0000:2d:00.0 0000:2d:00.1 > "$T/out.txt"
grep -qx -- 'pcie-root-port,id=rp0,bus=pcie.0,chassis=1,slot=1,hotplug=off,mem-reserve=4M,pref32-reserve=512M' "$T/out.txt" || fail "rootport: the port"
grep -qx -- 'vfio-pci,host=0000:2d:00.0,bus=rp0,addr=00.0,multifunction=on,x-no-kvm-intx=on' "$T/out.txt" || fail "rootport: the card"
grep -qx -- 'vfio-pci,host=0000:2d:00.1,bus=rp0,addr=00.1' "$T/out.txt" || fail "rootport: the audio function is not on the same port"
grep -q '^  GPU on: rootport (behind a PCIe root port: PCI bus 1, not bus 0)$' "$T/out.txt" || fail "rootport: 'GPU on' line"
grep -q '^  root port windows: mem-reserve 4M, pref32-reserve 512M$' "$T/out.txt" || fail "rootport: windows line"
! grep -q 'pci-hole64' "$T/out.txt" || fail "rootport without a 64-bit window set a 64-bit hole"
[ "$(grep -c '^pcie-root-port' "$T/out.txt")" = 1 ] || fail "rootport: more than one port"
# a 64-bit window: pref64, q35's hole as large as OVMF's window
mkres "$T/sys/0000:2d:00.0/resource" $((16384 << 20))
q rootport 65536 0000:2d:00.0 0000:2d:00.1 > "$T/out.txt"
grep -qx -- 'pcie-root-port,id=rp0,bus=pcie.0,chassis=1,slot=1,hotplug=off,mem-reserve=4M,pref64-reserve=32G' "$T/out.txt" || fail "rootport 16 GiB: the port"
grep -qx -- '-global' "$T/out.txt" && grep -qx -- 'q35-pcihost.pci-hole64-size=65536M' "$T/out.txt" || fail "rootport 16 GiB: the 64-bit hole"
! grep -q '^!!' "$T/out.txt" || fail "rootport 16 GiB: a warning although the window fits"
# a window smaller than the reserve says so in the log
q rootport 8192 0000:2d:00.0 0000:2d:00.1 > "$T/out.txt"
grep -q "^!! root port: pref64 window 32768 MiB is larger than OVMF's 64-bit window (8192 MiB" "$T/out.txt" || fail "no warning for a too small window"
# "default" (OVMF's own window): pref64, no hole argument, no warning
q rootport default 0000:2d:00.0 0000:2d:00.1 > "$T/out.txt"
grep -q 'pref64-reserve=32G' "$T/out.txt" && ! grep -q 'pci-hole64\|^!!' "$T/out.txt" || fail "rootport with the default window"
# a device of another IOMMU group gets its own port, as before
mkres "$T/sys/0000:2d:00.0/resource" $((256 << 20))
q rootport 0 0000:2d:00.0 0000:2d:00.1 0000:2e:00.0 > "$T/out.txt"
grep -qx -- 'pcie-root-port,id=rp1,bus=pcie.0,chassis=2,slot=2' "$T/out.txt" && grep -qx -- 'vfio-pci,host=0000:2e:00.0,bus=rp1' "$T/out.txt" ||
    fail "the second IOMMU group's port"
# the option ROM argument stays
echo rom > "$T/work/vbios.rom"
USE_ROMFILE=1 q pcie0 0 0000:2d:00.0 > "$T/out.txt"
grep -qx -- "vfio-pci,host=0000:2d:00.0,bus=pcie.0,addr=10.0,multifunction=on,x-no-kvm-intx=on,romfile=$T/work/vbios.rom" "$T/out.txt" || fail "romfile argument: $(cat "$T/out.txt")"
rm -f "$T/work/vbios.rom"

# ---- smbios_note: a change since the last card run is flagged
rm -f "$T/work/last-smbios" "$T/work/smbios-changed"
h smbios_note MacBookPro16,1 > "$T/out.txt"
[ ! -e "$T/work/smbios-changed" ] && [ "$(cat "$T/work/last-smbios")" = MacBookPro16,1 ] || fail "first run flagged a change"
grep -qx '  SMBIOS model: MacBookPro16,1' "$T/out.txt" || fail "smbios_note: model line"
h smbios_note MacBookPro16,1 > "$T/out.txt"
[ ! -e "$T/work/smbios-changed" ] || fail "same model flagged"
h smbios_note MacPro7,1 > "$T/out.txt"
[ "$(cat "$T/work/smbios-changed")" = "MacBookPro16,1 -> MacPro7,1" ] || fail "change not recorded: $(cat "$T/work/smbios-changed" 2>&1)"
grep -q '^  SMBIOS model changed since the last card run: MacBookPro16,1 -> MacPro7,1$' "$T/out.txt" || fail "change not printed"
h smbios_note MacPro7,1 > "$T/out.txt"
[ ! -e "$T/work/smbios-changed" ] || fail "the flag outlived the next run with the same model"

# ---- the whole cmd__oc chain with stubbed losetup / mount (no root, no image)
mkdir -p "$T/bin" "$T/ocw"
cat > "$T/bin/losetup" <<'EOF'
#!/bin/sh
case "$1" in -d) exit 0 ;; esac
echo /dev/loop99
EOF
cat > "$T/bin/mount" <<'EOF'
#!/bin/sh
# mount -t vfat PART MNT: the "image" is the fixture config
for a; do mnt=$a; done
mkdir -p "$mnt/EFI/OC"
cp "$FIXTURE" "$mnt/EFI/OC/config.plist"
EOF
cat > "$T/bin/umount" <<'EOF'
#!/bin/sh
cp "$1/EFI/OC/config.plist" "$RESULT"
rm -rf "$1/EFI"
EOF
printf '#!/bin/sh\nexit 0\n' > "$T/bin/mountpoint"
printf '#!/bin/sh\nexit 0\n' > "$T/bin/sync"
chmod +x "$T/bin/"*
: > "$T/ocw/OpenCore-rdna3.img"
oc() {
    env PATH="$T/bin:$PATH" RDNA3VM_DIR="$T/ocw" FIXTURE="$ROOT/tests/data/oc-config-smbios.plist" RESULT="$T/oc-result.plist" \
        bash "$VM" _oc "$@"
}
step=$(awk '$1 == "play-desktop" { $1 = ""; sub(/^ +/, ""); print }' "$ROOT/tools/ladder-steps.txt")
[ -n "$step" ] || fail "play-desktop is not in ladder-steps.txt"
args="-v keepsyms=1 $step rdna3vmauto=1"
stripped=$(h strip_host_opts "$args")
oc --boot-args "$stripped" --smbios "$(h smbios_model "$args")" --resolution Max >/dev/null || fail "_oc chain failed"
[ "$($VT plist "$T/oc-result.plist" --show-smbios)" = "MacPro7,1" ] || fail "the OpenCore config does not say MacPro7,1"
[ "$($VT plist "$T/oc-result.plist" --show)" = "$stripped" ] || fail "boot-args in the OpenCore config: $($VT plist "$T/oc-result.plist" --show)"
! $VT plist "$T/oc-result.plist" --show | grep -q '@' || fail "a host-only token reached macOS's boot-args"
$VT plist "$T/oc-result.plist" --show | grep -q 'rdna3play=1' || fail "rdna3play=1 lost"
grep -q 'SERIAL-SYNTH-01' "$T/oc-result.plist" || fail "serial lost"

# ---- approve --step: an unknown step / option is refused before anything starts
! RDNA3VM_DIR="$T/ocw" bash "$VM" approve --step zz >"$T/out.txt" 2>&1 || fail "approve --step zz accepted"
grep -q 'unknown step zz' "$T/out.txt" || fail "approve --step zz: no message: $(cat "$T/out.txt")"
! RDNA3VM_DIR="$T/ocw" bash "$VM" approve --bogus >"$T/out.txt" 2>&1 || fail "approve --bogus accepted"
grep -q 'unknown option --bogus' "$T/out.txt" || fail "approve --bogus: no message"
out=$(bash "$VM" 2>&1 || true)
grep -q 'approve \[--card\] \[--step S\]' <<<"$out" || fail "the help does not list approve --step"

# ---- the ladder and the step lists
play=$(awk '$1 == "play" { $1 = ""; sub(/^ +/, ""); print }' "$ROOT/tools/ladder-steps.txt")
[ -n "$play" ] || fail "no play step"
for want in "play-macpro:$play @smbios=MacPro7,1" "play-rootport:$play @gpubus=rootport" "play-desktop:$play @smbios=MacPro7,1 @gpubus=rootport"; do
    n=${want%%:*}; w=${want#*:}
    got=$(awk -v n="$n" '$1 == n { $1 = ""; sub(/^ +/, ""); print }' "$ROOT/tools/ladder-steps.txt")
    [ "$got" = "$w" ] || fail "ladder-steps.txt: $n is [$got], expected [$w]"
    [ "$(grep -c "bisect-agdc play play-macpro play-rootport play-desktop" "$VM")" = 2 ] || fail "rdna3vm: $n is not in both step lists"
done
fn auto_limit > "$T/limit.sh"
for n in play play-macpro play-rootport play-desktop; do
    a=$(awk -v n="$n" '$1 == n { $1 = ""; sub(/^ +/, ""); print }' "$ROOT/tools/ladder-steps.txt")
    m=$(AUTO_TIMEOUT_MIN=8 bash -c '. "$1"; auto_limit "$2"' _ "$T/limit.sh" "$a")
    [ "$m" = 30 ] || fail "auto_limit of $n is [$m], expected 30"
done
# bare metal: package/oc_install.sh drops the host-only tokens
cp "$ROOT/tests/data/oc-config.plist" "$T/ocinst.plist"
mkdir -p "$T/efi/EFI/OC"; cp "$T/ocinst.plist" "$T/efi/EFI/OC/config.plist"
"$ROOT/package/oc_install.sh" --efi "$T/efi" --step play-desktop >/dev/null 2>&1 || fail "oc_install.sh --step play-desktop"
ba=$($VT plist "$T/efi/EFI/OC/config.plist" --show)
case $ba in *@*) fail "oc_install.sh --step play-desktop put a host-only token into the boot-args: $ba" ;; esac
case $ba in *rdna3play=1*) ;; *) fail "oc_install.sh --step play-desktop lost rdna3play=1: $ba" ;; esac

echo "test_vm_present_ab: ok"
