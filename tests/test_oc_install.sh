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

# --- an EFI path with a space; foreign Kernel/Add entries before and after ours;
#     a foreign boot-arg with '&'; RDNA3Accelerator is not RDNA3Accel
E="$T/my efi"; V=$ROOT/tools/vm/vmtool.py
mkdir -p "$E/EFI/OC" "$T/build/RDNA3Accelerator.kext/Contents/MacOS"
cp "$ROOT/tests/data/oc-config.plist" "$E/EFI/OC/config.plist"
echo "<plist/>" > "$T/build/RDNA3Accelerator.kext/Contents/Info.plist"
echo bin > "$T/build/RDNA3Accelerator.kext/Contents/MacOS/RDNA3Accelerator"
RDNA3_BUILD=$T/build "$ROOT/package/oc_install.sh" --efi "$E" >/dev/null
python3 "$V" plist "$E/EFI/OC/config.plist" --kext WhateverGreen --boot-args 'alcid=1 foo=a&b<c -v rdna3lvl=2' >/dev/null
entries() { python3 -c 'import plistlib,sys; print(" ".join(e["BundlePath"] for e in plistlib.load(open(sys.argv[1],"rb"))["Kernel"]["Add"]))' "$1"; }
[ "$(entries "$E/EFI/OC/config.plist")" = "Lilu.kext RDNA3Accel.kext RDNA3Framebuffer.kext RDNA3Accelerator.kext WhateverGreen.kext" ] ||
    fail "setup entries: $(entries "$E/EFI/OC/config.plist")"
[ "$(python3 "$V" plist "$E/EFI/OC/config.plist" --show)" = "alcid=1 foo=a&b<c -v rdna3lvl=2" ] || fail "--show does not unescape"
# the entry removal matches the whole BundlePath: RDNA3Accel leaves RDNA3Accelerator
cp "$E/EFI/OC/config.plist" "$T/one.plist"
python3 "$V" plist "$T/one.plist" --remove-kext RDNA3Accel >/dev/null
[ "$(entries "$T/one.plist")" = "Lilu.kext RDNA3Framebuffer.kext RDNA3Accelerator.kext WhateverGreen.kext" ] ||
    fail "--remove-kext RDNA3Accel: $(entries "$T/one.plist")"
"$ROOT/package/oc_install.sh" --efi "$E" --remove >/dev/null
[ "$(entries "$E/EFI/OC/config.plist")" = "Lilu.kext WhateverGreen.kext" ] || fail "--remove entries: $(entries "$E/EFI/OC/config.plist")"
[ "$(python3 "$V" plist "$E/EFI/OC/config.plist" --show)" = "alcid=1 foo=a&b<c -v" ] || fail "--remove changed a foreign boot-arg"
grep -q 'foo=a&amp;b&lt;c' "$E/EFI/OC/config.plist" && ! grep -q '&amp;amp;' "$E/EFI/OC/config.plist" || fail "foreign boot-arg not byte-identical"
"$ROOT/package/oc_install.sh" --efi "$E" --off >/dev/null
[ "$(python3 "$V" plist "$E/EFI/OC/config.plist" --show)" = "alcid=1 foo=a&b<c -v -rdna3off" ] || fail "--off double-escaped"
grep -q '&amp;amp;' "$E/EFI/OC/config.plist" && fail "--off double-escaped in the file"
"$ROOT/package/oc_install.sh" --efi "$E" --remove --dry-run | grep -q 'would remove' || fail "--remove --dry-run says it removed"

# --- a failing config.plist edit leaves the kexts where they are (no Kernel dict here)
F=$T/bad; mkdir -p "$F/EFI/OC/Kexts/RDNA3Accel.kext"
python3 - "$ROOT/tests/data/oc-config.plist" "$F/EFI/OC/config.plist" <<'PY'
import re, sys
t = open(sys.argv[1]).read()
t = re.sub(r"\t<key>Kernel</key>.*?\n\t</dict>\n", "\t<key>Comment</key>\n\t<string>RDNA3Accel.kext</string>\n", t, flags=re.S)
assert "<key>Kernel</key>" not in t
open(sys.argv[2], "w").write(t)
PY
sum=$(cksum < "$F/EFI/OC/config.plist")
"$ROOT/package/oc_install.sh" --efi "$F" --remove >/dev/null 2>&1 && fail "--remove succeeded on a config.plist it cannot edit"
[ -d "$F/EFI/OC/Kexts/RDNA3Accel.kext" ] || fail "kext deleted although the config.plist edit failed"
[ "$(cksum < "$F/EFI/OC/config.plist")" = "$sum" ] || fail "config.plist changed by a failed edit"

# --- backups are never overwritten (date frozen: every run is in the same second)
mkdir -p "$T/fakebin"
printf '#!/bin/sh\necho 20260101-000000\n' > "$T/fakebin/date"; chmod +x "$T/fakebin/date"
G=$T/same; mkdir -p "$G/EFI/OC"; cp "$ROOT/tests/data/oc-config.plist" "$G/EFI/OC/config.plist"
for i in 1 2; do
    PATH=$T/fakebin:$PATH RDNA3_BUILD=$T/build "$ROOT/package/oc_install.sh" --efi "$G" >/dev/null
    PATH=$T/fakebin:$PATH "$ROOT/package/oc_install.sh" --efi "$G" --remove >/dev/null
done
[ "$(ls "$G/EFI/OC" | grep -c 'config.plist.rdna3-')" = 4 ] || fail "same-second backups overwritten: $(ls "$G/EFI/OC")"
grep -q RDNA3 "$G/EFI/OC/config.plist.rdna3-20260101-000000" && fail "the first backup was overwritten"
echo "test_oc_install: all checks passed"
