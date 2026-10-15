#!/bin/bash
# package/make_pkg.sh (tarball only, no pkgbuild needed) and its install.sh,
# preinstall/postinstall, firmware.sh and uninstall.sh against a temporary
# root, with fake CI artifacts and fake firmware (no network, no root).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf -- "$T"' EXIT
fail() { echo "test_make_pkg: FAIL $*"; exit 1; }
FILES=$(sh "$ROOT/package/firmware.sh" --list | tr '\n' ' ' | sed 's/ $//')
MK=$(sed -n '/^FW_FILES *:=/,/[^\\]$/p' "$ROOT/kext/Makefile" | sed 's/^FW_FILES *:=//; s/\\//g' | tr -s ' \n' '  ' |
     sed 's/^ *//; s/ *$//')
[ "$FILES" = "$MK" ] || fail "firmware.sh and kext/Makefile FW_FILES differ: '$FILES' vs '$MK'"

# --- fake artifacts: RDNA3Accel-kext (dir), RDNA3MTL (zip), radv-macos-logs (dir)
a=$T/art
for p in kext:RDNA3Accel fb:RDNA3Framebuffer accel:RDNA3Accelerator agdc:RDNA3AGDC; do
    d=${p%%:*}; n=${p#*:}
    mkdir -p "$a/$n.kext/Contents/MacOS"
    cp "$ROOT/$d/Info.plist" "$a/$n.kext/Contents/"
    echo bin > "$a/$n.kext/Contents/MacOS/$n"
done
mkdir -p "$a/RDNA3Accel.kext/Contents/Resources" "$a/userspace"
echo blob > "$a/RDNA3Accel.kext/Contents/Resources/psp_13_0_10_sos.bin"
for f in rdna3_drminfo rdna3_modetest libdrm.dylib libdrm_amdgpu.dylib; do echo x > "$a/userspace/$f"; done
mkdir -p "$T/mtl/RDNA3MTLDriver.bundle/Contents/MacOS"
cp "$ROOT/metal/bundle/Info.plist" "$T/mtl/RDNA3MTLDriver.bundle/Contents/"
echo x > "$T/mtl/RDNA3MTLDriver.bundle/Contents/MacOS/RDNA3MTLDriver"
echo x > "$T/mtl/librdna3mtl_translate.dylib"
(cd "$T/mtl" && python3 -c 'import os, zipfile
z = zipfile.ZipFile("../RDNA3MTL.zip", "w")
for d, _, fs in os.walk("."):
    for f in fs: z.write(os.path.join(d, f), os.path.relpath(os.path.join(d, f)))')
mkdir -p "$T/radv/install/lib"
echo x > "$T/radv/install/lib/libvulkan_radeon.dylib"

# --- fake firmware: the header's size word = the file size
mkdir -p "$T/lf/amdgpu"
python3 - "$T/lf/amdgpu" "$FILES" <<'PY'
import struct, sys
for f in sys.argv[2].split():
    open(sys.argv[1] + "/" + f, "wb").write(struct.pack("<II", 8192, 32) + bytes(8184))
PY
echo license > "$T/lf/LICENSE.amdgpu"

"$ROOT/package/make_pkg.sh" --no-pkg --from "$a" --from "$T/RDNA3MTL.zip" --from "$T/radv" --out "$T/out" >/dev/null
tgz=$(ls "$T"/out/RDNA3-Hackintosh-*.tar.gz)
mkdir -p "$T/x" && tar -xzf "$tgz" -C "$T/x"
rel=$(ls -d "$T"/x/RDNA3-Hackintosh-*)

# --- install with --fw into a root
S=$T/sys
A="$S/Library/Application Support/RDNA3"
"$rel/install.sh" --root "$S" --fw "$T/lf/amdgpu" >/dev/null
for k in RDNA3Accel RDNA3Framebuffer RDNA3Accelerator RDNA3AGDC; do
    [ -x "$S/Library/Extensions/$k.kext/Contents/MacOS/$k" ] || fail "$k not installed (or not executable)"
done
[ -e "$S/Library/Extensions/RDNA3Accel.kext/Contents/Resources/psp_13_0_10_sos.bin" ] && fail "firmware shipped inside the kext"
sh "$A/firmware.sh" --dest "$A/firmware" --check >/dev/null || fail "firmware not installed"
[ -f "$A/firmware/LICENSE.amdgpu" ] || fail "LICENSE.amdgpu not next to the firmware"
[ "$(readlink "$A/bin/libdrm.2.dylib")" = /Library/GPUBundles/rdna3mtl/libdrm.2.dylib ] || fail "bin/libdrm.2.dylib link"
for f in libdrm.2.dylib libdrm_amdgpu.1.dylib amdgpu.ids libvulkan_radeon.dylib librdna3mtl_translate.dylib; do
    [ -f "$S/Library/GPUBundles/rdna3mtl/$f" ] || fail "rdna3mtl/$f missing"
done
python3 -c 'import json, sys; assert json.load(open(sys.argv[1]))["ICD"]["library_path"].endswith("rdna3mtl/libvulkan_radeon.dylib")' \
    "$S/Library/GPUBundles/rdna3mtl/radeon_icd.x86_64.json" || fail "ICD json"
[ -x "$S/Library/GPUBundles/RDNA3MTLDriver.bundle/Contents/MacOS/RDNA3MTLDriver" ] || fail "Metal plugin"
[ -f "$S/Library/GPUBundles/rdna3mtl-allow.txt" ] || fail "allow list"
for f in autoreport.sh uninstall.sh oc_install.sh tools/macos/collect.sh bin/rdna3_drminfo VERSION LICENSE NOTICE; do
    [ -e "$A/$f" ] || fail "$f missing"
done
python3 -c 'import plistlib, sys
e = plistlib.load(open(sys.argv[1], "rb"))["Kernel"]["Add"]
assert e[0]["BundlePath"] == "RDNA3Accel.kext" and e[0]["Enabled"] and not any(x["Enabled"] for x in e[1:])' \
    "$A/OpenCore/Kernel-Add.plist" || fail "OpenCore/Kernel-Add.plist"

# --- the installed oc_install.sh injects only the core kext of this install
mkdir -p "$T/efi/EFI/OC"
cp "$ROOT/tests/data/oc-config.plist" "$T/efi/EFI/OC/config.plist"
"$A/oc_install.sh" --efi "$T/efi" >/dev/null
[ -f "$T/efi/EFI/OC/Kexts/RDNA3Accel.kext/Contents/MacOS/RDNA3Accel" ] || fail "oc_install.sh: kext not in EFI"
grep -q '<string>RDNA3Accel.kext</string>' "$T/efi/EFI/OC/config.plist" || fail "oc_install.sh: no Kernel/Add entry"
grep -q 'RDNA3Framebuffer' "$T/efi/EFI/OC/config.plist" && fail "oc_install.sh injected the framebuffer"

# --- upgrade: stale files go, the firmware stays (no --fw, no download)
echo stale > "$A/bin/stale"
echo stale > "$S/Library/Extensions/RDNA3Accel.kext/Contents/stale"
RDNA3_FW_URL=file:///nonexistent "$rel/install.sh" --root "$S" >/dev/null
[ -e "$A/bin/stale" ] || [ -e "$S/Library/Extensions/RDNA3Accel.kext/Contents/stale" ] && fail "stale files survived an upgrade"
sh "$A/firmware.sh" --dest "$A/firmware" --check >/dev/null || fail "upgrade lost the firmware"

# --- no firmware, no download: nothing installed
RDNA3_FW_URL=file:///nonexistent "$rel/install.sh" --root "$T/sys2" >/dev/null 2>&1 && fail "installed without firmware"
[ -e "$T/sys2/Library/Extensions" ] && fail "files installed although the firmware was missing"

# --- a damaged file is caught
cp "$A/firmware/sdma_6_0_3.bin" "$T/sdma"
head -c 5000 "$T/sdma" > "$A/firmware/sdma_6_0_3.bin"
sh "$A/firmware.sh" --dest "$A/firmware" --check >/dev/null && fail "truncated firmware passed the check"

# --- firmware bundled with --fw: installs offline
"$ROOT/package/make_pkg.sh" --no-pkg --from "$a" --fw "$T/lf/amdgpu" --out "$T/out2" >/dev/null
mkdir -p "$T/x2" && tar -xzf "$(ls "$T"/out2/*.tar.gz)" -C "$T/x2"
RDNA3_FW_URL=file:///nonexistent "$(ls -d "$T"/x2/RDNA3-Hackintosh-*)/install.sh" --root "$T/sys3" >/dev/null ||
    fail "bundled firmware install"
sh "$T/sys3/Library/Application Support/RDNA3/firmware.sh" --dest "$T/sys3/Library/Application Support/RDNA3/firmware" \
    --check >/dev/null || fail "bundled firmware not installed"

# --- uninstall, with the OpenCore install of above (RDNA3Accel injected, step boot-args,
#     a kext and an entry of the user's own) in a fake EFI
EC=$T/efi/EFI/OC/config.plist
mkdir -p "$T/efi/EFI/OC/Kexts/Lilu.kext"
"$A/oc_install.sh" --efi "$T/efi" --step 9b3 >/dev/null
rm -f "$T/efi/EFI/OC"/config.plist.rdna3-*
# a failing OpenCore removal (no EFI there) stops before anything is removed
"$A/uninstall.sh" --root "$S" --efi "$T/nowhere" >/dev/null 2>&1 && fail "uninstall went on without the EFI"
[ -e "$S/Library/Extensions/RDNA3Accel.kext" ] && [ -d "$A/firmware" ] || fail "failed uninstall removed files"
# --keep-opencore leaves OpenCore alone and says so
out=$("$A/uninstall.sh" --root "$S" --keep-firmware --efi "$T/efi" --keep-opencore)
[ -e "$S/Library/Extensions/RDNA3Accel.kext" ] && fail "uninstall left the kext"
if [ ! -d "$A/firmware" ] || [ -e "$A/bin" ]; then fail "--keep-firmware"; fi
grep -q '<string>RDNA3Accel.kext</string>' "$EC" && [ -d "$T/efi/EFI/OC/Kexts/RDNA3Accel.kext" ] || fail "--keep-opencore touched OpenCore"
ls "$T/efi/EFI/OC"/config.plist.rdna3-* >/dev/null 2>&1 && fail "--keep-opencore wrote a backup"
case "$out" in *"left as it is"*) ;; *) fail "--keep-opencore says nothing: $out";; esac
# the full uninstall (helper from the source tree: --keep-firmware removed the installed one)
out=$("$ROOT/package/uninstall.sh" --root "$S" --efi "$T/efi")
[ -e "$A" ] || [ -e "$S/Library/GPUBundles/rdna3mtl" ] && fail "uninstall left files"
[ -e "$T/efi/EFI/OC/Kexts/RDNA3Accel.kext" ] && fail "uninstall left the kext in EFI/OC/Kexts"
grep -q 'RDNA3' "$EC" && fail "uninstall left Kernel/Add entries"
[ -d "$T/efi/EFI/OC/Kexts/Lilu.kext" ] && grep -q '<string>Lilu.kext</string>' "$EC" || fail "uninstall touched the user's own kext"
[ "$(python3 "$ROOT/tools/vm/vmtool.py" plist "$EC" --show)" = "alcid=1 -v keepsyms=1" ] || fail "uninstall boot-args"
ls "$T/efi/EFI/OC"/config.plist.rdna3-* >/dev/null 2>&1 || fail "uninstall made no config.plist backup"
case "$out" in *Removed*"OpenCore ("*"Left alone"*) ;; *) fail "uninstall does not say what it removed and left: $out";; esac
# without OpenCore in the picture (no --efi, not on macOS): nothing to remove, no error
"$ROOT/package/uninstall.sh" --root "$T/sys4" >/dev/null || fail "uninstall of an empty root"
echo "test_make_pkg: all checks passed"
