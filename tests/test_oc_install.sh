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
echo "test_oc_install: all checks passed"
