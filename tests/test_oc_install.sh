#!/bin/bash
# package/oc_install.sh against a minimal OpenCore config.plist (no root,
# no firmware): kexts copied and listed under Kernel/Add once, boot-args of
# a ladder step replacing the rdna3* ones and keeping the rest, --off.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf -- "$T"' EXIT
mkdir -p "$T/efi/EFI/OC" "$T/build/RDNA3Accel.kext/Contents/MacOS" "$T/build/RDNA3Framebuffer.kext/Contents/MacOS"
cp "$ROOT/tests/data/oc-config.plist" "$T/efi/EFI/OC/config.plist"
for n in RDNA3Accel RDNA3Framebuffer; do
    echo "<plist/>" > "$T/build/$n.kext/Contents/Info.plist"
    echo bin > "$T/build/$n.kext/Contents/MacOS/$n"
done
C=$T/efi/EFI/OC/config.plist
fail() { echo "test_oc_install: FAIL $*"; exit 1; }
show() { python3 "$ROOT/tools/vm/vmtool.py" plist "$C" --show; }

RDNA3_BUILD=$T/build "$ROOT/package/oc_install.sh" --efi "$T/efi" >/dev/null
[ -f "$T/efi/EFI/OC/Kexts/RDNA3Accel.kext/Contents/MacOS/RDNA3Accel" ] || fail "kext not copied"
[ "$(grep -c '<string>RDNA3Accel.kext</string>' "$C")" = 1 ] || fail "Kernel/Add entry"
grep -q '<string>RDNA3Framebuffer.kext</string>' "$C" || fail "framebuffer entry"
[ "$(show)" = "alcid=1 rdna3lvl=2 -v" ] || fail "plain install changed boot-args: $(show)"

RDNA3_BUILD=$T/build "$ROOT/package/oc_install.sh" --efi "$T/efi" >/dev/null
[ "$(grep -c '<string>RDNA3Accel.kext</string>' "$C")" = 1 ] || fail "second install duplicated the entry"

RDNA3_BUILD=$T/build "$ROOT/package/oc_install.sh" --efi "$T/efi" --step 9b3 >/dev/null
[ "$(show)" = "alcid=1 -v keepsyms=1 rdna3lvl=8 rdna3msi=1 rdna3fb=1 rdna3dmub=3 rdna3mirror=1" ] ||
    fail "step 9b3 boot-args: $(show)"
"$ROOT/package/oc_install.sh" --efi "$T/efi" --off >/dev/null
[ "$(show)" = "alcid=1 -v keepsyms=1 -rdna3off" ] || fail "--off boot-args: $(show)"
"$ROOT/package/oc_install.sh" --efi "$T/efi" --step zz >/dev/null 2>&1 && fail "unknown step accepted"
[ "$(ls "$T/efi/EFI/OC" | grep -c 'config.plist.rdna3-')" -ge 1 ] || fail "no backup"
python3 -c "import plistlib,sys; plistlib.load(open(sys.argv[1],'rb'))" "$C" || fail "config.plist no longer parses"

# --- --remove: only our kexts, entries and rdna3 boot-args go; a backup is made
RDNA3_BUILD=$T/build "$ROOT/package/oc_install.sh" --efi "$T/efi" --step 9b3 >/dev/null
mkdir -p "$T/efi/EFI/OC/Kexts/Lilu.kext/Contents"
rm -f "$T/efi/EFI/OC"/config.plist.rdna3-*
sum=$(cksum < "$C")
"$ROOT/package/oc_install.sh" --efi "$T/efi" --remove --dry-run >/dev/null
[ "$(cksum < "$C")" = "$sum" ] || fail "--remove --dry-run changed config.plist"
[ -d "$T/efi/EFI/OC/Kexts/RDNA3Accel.kext" ] || fail "--remove --dry-run removed a kext"
"$ROOT/package/oc_install.sh" --efi "$T/efi" --remove --off >/dev/null 2>&1 && fail "--remove --off accepted"
out=$("$ROOT/package/oc_install.sh" --efi "$T/efi" --remove)
for n in RDNA3Accel RDNA3Framebuffer; do
    [ -e "$T/efi/EFI/OC/Kexts/$n.kext" ] && fail "--remove left $n.kext in EFI/OC/Kexts"
    grep -q "<string>$n.kext</string>" "$C" && fail "--remove left the $n Kernel/Add entry"
done
[ -d "$T/efi/EFI/OC/Kexts/Lilu.kext" ] || fail "--remove removed a kext that is not ours"
grep -q '<string>Lilu.kext</string>' "$C" || fail "--remove removed an unrelated Kernel/Add entry"
[ "$(show)" = "alcid=1 -v keepsyms=1" ] || fail "--remove boot-args: $(show)"
case "$out" in *RDNA3Accel*Kernel/Add*) ;; *) fail "--remove does not say what it removed: $out";; esac
bk=$(ls "$T/efi/EFI/OC"/config.plist.rdna3-* 2>/dev/null | head -1)
[ -n "$bk" ] && grep -q '<string>RDNA3Accel.kext</string>' "$bk" && grep -q 'rdna3fb=1' "$bk" || fail "--remove made no backup of the old config.plist"
python3 -c "import plistlib,sys; plistlib.load(open(sys.argv[1],'rb'))" "$C" || fail "config.plist no longer parses after --remove"
sum=$(cksum < "$C"); n=$(ls "$T/efi/EFI/OC" | wc -l)
out=$("$ROOT/package/oc_install.sh" --efi "$T/efi" --remove)
[ "$(cksum < "$C")" = "$sum" ] && [ "$(ls "$T/efi/EFI/OC" | wc -l)" = "$n" ] || fail "second --remove changed something"
case "$out" in *"nothing of RDNA3"*) ;; *) fail "second --remove: $out";; esac
RDNA3_BUILD=$T/build "$ROOT/package/oc_install.sh" --efi "$T/efi" >/dev/null
grep -q '<string>RDNA3Accel.kext</string>' "$C" || fail "install after --remove"
echo "test_oc_install: all checks passed"
