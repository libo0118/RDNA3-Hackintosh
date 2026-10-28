#!/bin/bash
# CPU_MODEL=host in tools/vm/rdna3vm (the card VM on the AMD host's real CPU) and the
# OpenCore side of it, tools/vm/vmtool.py plist --amd-cpu / --no-amd-cpu. No qemu, no
# root, no image, no card:
#  - the vendored AMD_Vanilla file is the one tools/vm/data/amd_vanilla.SOURCE names
#    (sha256), gives the kernel patches only (no IOPCIFamily ones), and the four
#    cpuid_cores_per_package entries take the core count at the byte the AMD_Vanilla
#    README says (B8|BA <count> ...), for 1..255
#  - insert: every entry is marked "rdna3vm-amd:", foreign Kernel/Patch entries stay byte
#    for byte, the quirk / DummyPowerManagement are on, the Cpuid1 spoof is off
#  - a second run (same or another core count) changes nothing / only the count
#  - switching back: the entries go and what they replaced comes back, byte for byte
#    (spoof set, ProvideCurrentCpuInfo false or missing, Patch as <array/>); a config that
#    was never in AMD mode, or has no Kernel/Patch, is left alone
#  - refusals: bad core counts, no Kernel/Patch, an edited patch file
#  - rdna3vm: the -cpu / -smp / vmtool options per CPU_MODEL, CPUS, CPU_THREADS and host
#    vendor; load_conf leaves CPU_MODEL=host alone; the cmd__oc chain (stubbed losetup /
#    mount) applies the patches for the config's CPU_MODEL, and takes them out again when it
#    changes; 'result' puts vm-cpu into 00-host.txt
#  - RestrictEvents (About This Mac names the real CPU): the pins of tools/vm/data/oc_kexts.SOURCE
#    and vmtool.py agree; the fetch is sha256 checked and extracts only Info.plist + the
#    executable; cmd__oc puts Lilu (replacing the image's old one, which is kept) and
#    RestrictEvents (after Lilu) into EFI/OC/Kexts + Kernel/Add in the AMD mode, idempotently,
#    and takes everything out again (old Lilu back) when the mode is off or the fetch failed
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf -- "$T"' EXIT
fail() { echo "test_vm_amd_cpu: FAIL $*"; [ -f "$T/out.txt" ] && sed 's/^/  | /' "$T/out.txt"; exit 1; }
VM=$ROOT/tools/vm/rdna3vm
VTP=$ROOT/tools/vm/vmtool.py
VT="python3 -I $VTP"
FIX=$ROOT/tests/data/oc-config-kernel.plist
bash -n "$VM" || fail "bash -n rdna3vm"

# ---- the vendored file
SRC=$ROOT/tools/vm/data/amd_vanilla.SOURCE
PLIST=$ROOT/tools/vm/data/amd_vanilla-a174cca.plist
sha=$(sha256sum "$PLIST" | cut -d' ' -f1)
grep -q "^sha256:   $sha\$" "$SRC" || fail "amd_vanilla.SOURCE does not carry the sha256 of $PLIST ($sha)"
grep -q "AMD_PATCH_SHA256 = \"$sha\"" "$VTP" || fail "vmtool.py AMD_PATCH_SHA256 is not $sha"
grep -q 'a174cca80efe2377fde3b902666c70863ea66454' "$SRC" || fail "no commit in amd_vanilla.SOURCE"
grep -q '^Licence:' "$SRC" || fail "no licence note in amd_vanilla.SOURCE"
[ "$($VT amdpatches | cut -d' ' -f1)" = 24 ] || fail "amdpatches: $($VT amdpatches)"

# ---- RestrictEvents / Lilu pins: SOURCE file and vmtool.py agree
OSRC=$ROOT/tools/vm/data/oc_kexts.SOURCE
python3 -I - "$ROOT" "$OSRC" <<'PY' || fail "oc_kexts pins (see above)"
import re, sys
root, src = sys.argv[1:3]
sys.path.insert(0, root + "/tools/vm")
import vmtool as v
text = open(src).read()
if [n for n, _, _ in v.OC_KEXTS] != ["Lilu", "RestrictEvents"]:
    sys.exit("Lilu must come before RestrictEvents in OC_KEXTS: %r" % [n for n, _, _ in v.OC_KEXTS])
for name, ver, sha in v.OC_KEXTS:
    if not re.fullmatch(r"[0-9a-f]{64}", sha):
        sys.exit("%s: not a sha256" % name)
    if "sha256 %s" % sha not in text:
        sys.exit("oc_kexts.SOURCE has no 'sha256 %s' for %s" % (sha, name))
    if not re.search(r"^\s+%s\s+%s\s" % (name, re.escape(ver)), text, re.M):
        sys.exit("oc_kexts.SOURCE does not name %s %s" % (name, ver))
    if "releases/download/%s/%s-%s-RELEASE.zip" % (ver, name, ver) not in text:
        sys.exit("oc_kexts.SOURCE has no release URL of %s %s" % (name, ver))
if "acidanthera" not in v.OC_KEXT_URL or "RELEASE.zip" not in v.OC_KEXT_URL:
    sys.exit("OC_KEXT_URL is not an Acidanthera RELEASE zip: %s" % v.OC_KEXT_URL)
PY

# ---- the plist edits (python: the exact bytes matter)
python3 -I - "$ROOT" "$FIX" "$T" <<'PY' || fail "python checks (see above)"
import base64, plistlib, re, sys
root, fix, tmp = sys.argv[1:4]
sys.path.insert(0, root + "/tools/vm")
import vmtool as v

def fail(msg):
    sys.exit(msg)

orig = open(fix).read()
upstream = plistlib.load(open(root + "/tools/vm/data/amd_vanilla-a174cca.plist", "rb"))["Kernel"]["Patch"]
kernel_only = [p for p in upstream if p["Identifier"] == "kernel"]
if len(kernel_only) != 24 or len(upstream) != 27:
    fail("expected 24 kernel patches of 27 in the vendored file (3 are IOPCIFamily), got %d of %d" % (len(kernel_only), len(upstream)))
if any("IOPCIFamily" in p["Identifier"] for p in v.amd_patches()):
    fail("IOPCIFamily patches (Zen 4 boards) must not be applied")

def patches(text):
    # the Kernel dict alone: the fixture has an XML header plistlib needs, Kernel is complete
    return plistlib.loads(text.encode())["Kernel"]["Patch"]

def kernel(text):
    return plistlib.loads(text.encode())["Kernel"]

mine = lambda ps: [p for p in ps if p["Comment"].startswith("rdna3vm-amd: ")]

# --- insert
t8, n = v.set_amd_cpu(orig, 8)
if n != 24:
    fail("set_amd_cpu returned %r patches" % n)
ps = patches(t8)
if len(ps) != 3 + 24 or len(mine(ps)) != 24:
    fail("Kernel/Patch has %d entries, %d marked" % (len(ps), len(mine(ps))))
for a, b in zip(patches(orig), ps[:3]):
    if a != b:
        fail("a foreign Kernel/Patch entry changed: %s" % a["Comment"])
for e in re.findall(r"\t\t\t<dict>.*?\n\t\t\t</dict>\n", orig[orig.index("<key>Patch</key>"):orig.index("<key>Quirks</key>")], re.S):
    if e not in t8:
        fail("a foreign Kernel/Patch entry is not byte for byte in the new text")
k = kernel(t8)
if k["Quirks"]["ProvideCurrentCpuInfo"] is not True or k["Emulate"]["DummyPowerManagement"] is not True:
    fail("quirk / DummyPowerManagement not on")
if k["Emulate"]["Cpuid1Data"] != b"" or k["Emulate"]["Cpuid1Mask"] != b"":
    fail("Cpuid1 spoof not off")
# the entries are the upstream ones (Find, masks, kernel range), with the marker and the count
by_comment = {p["Comment"][len("rdna3vm-amd: "):]: p for p in mine(ps)}
for u in kernel_only:
    m = by_comment.get(u["Comment"])
    if m is None:
        fail("upstream patch missing: " + u["Comment"])
    for key in ("Find", "Mask", "ReplaceMask", "Base", "Identifier", "Arch", "Count", "Limit", "Skip",
                "MinKernel", "MaxKernel", "Enabled"):
        if m.get(key) != u.get(key, b"" if key in ("Mask", "ReplaceMask") else None):
            fail("%s: field %s differs from upstream" % (u["Comment"], key))
    if "cpuid_cores_per_package" not in u["Comment"] and m["Replace"] != u["Replace"]:
        fail("%s: Replace differs from upstream" % u["Comment"])
# upstream's disabled entries (Shaneee's PAT, AM5 hotplug is IOPCIFamily and not here) stay disabled
if any(p["Enabled"] for p in mine(ps) if "Shaneee" in p["Comment"]):
    fail("an alternative PAT patch is enabled")

# --- the core count: the byte after B8|BA, for the four patches; README of AMD_Vanilla
README = {"10.13-10.14": "b8%s0000" "0000", "10.15-11.0": "ba%s0000" "0000", "12.0-13.2": "ba%s0000" "0090",
          "13.3+": "ba%s0000" "00"}
for cores in (1, 6, 8, 12, 16, 24, 32, 255):
    t, _ = v.set_amd_cpu(orig, cores)
    cp = [p for p in mine(patches(t)) if "cpuid_cores_per_package" in p["Comment"]]
    if len(cp) != 4:
        fail("%d core count patches" % len(cp))
    for p in cp:
        want = README[p["Comment"].rsplit(" | ", 1)[1]] % ("%02x" % cores)
        if p["Replace"].hex() != want:
            fail("cores=%d %s: Replace %s, README says %s" % (cores, p["Comment"], p["Replace"].hex(), want))
    cnt, num = v.amd_cpu_state(t)
    if (cnt, num) != (cores, 24):
        fail("amd_cpu_state says %r" % ((cnt, num),))
if v.amd_cpu_state(orig) != (None, 0):
    fail("amd_cpu_state of a config without the patches")

# --- idempotent
t8b, _ = v.set_amd_cpu(t8, 8)
if t8b != t8:
    fail("a second run with the same core count changed the config")
t12, _ = v.set_amd_cpu(t8, 12)
fresh12, _ = v.set_amd_cpu(orig, 12)
if t12 != fresh12:
    fail("8 then 12 is not the same as 12")
if len(mine(patches(t12))) != 24:
    fail("a changed core count duplicated the entries")

# --- switch back, byte for byte
back, removed, restored = v.unset_amd_cpu(t12)
if back != orig or removed != 24 or restored != 4:
    fail("back to the original: equal=%s removed=%d restored=%d" % (back == orig, removed, restored))
again, r2, s2 = v.unset_amd_cpu(back)
if again != orig or (r2, s2) != (0, 0):
    fail("a config that was never in AMD mode changed")

def variant(text, **repl):
    for old, new in repl.values():
        assert old in text, old
        text = text.replace(old, new)
    return text

# a spoof (Cpuid1Data/Mask), the quirk off, DummyPowerManagement off: all come back
spoof = variant(orig,
    d=("<key>Cpuid1Data</key>\n\t\t\t<data>\n\t\t\t</data>", "<key>Cpuid1Data</key>\n\t\t\t<data>wwYDAAAAAAAAAAAAAAAAAA==</data>"),
    m=("<key>Cpuid1Mask</key>\n\t\t\t<data>\n\t\t\t</data>", "<key>Cpuid1Mask</key>\n\t\t\t<data>/////wAAAAAAAAAAAAAAAA==</data>"),
    p=("<key>DummyPowerManagement</key>\n\t\t\t<true/>", "<key>DummyPowerManagement</key>\n\t\t\t<false/>"),
    q=("<key>ProvideCurrentCpuInfo</key>\n\t\t\t<true/>", "<key>ProvideCurrentCpuInfo</key>\n\t\t\t<false/>"))
ts, _ = v.set_amd_cpu(spoof, 12)
ks = kernel(ts)
if (ks["Emulate"]["Cpuid1Data"], ks["Emulate"]["Cpuid1Mask"]) != (b"", b"") or not ks["Emulate"]["DummyPowerManagement"] \
        or not ks["Quirks"]["ProvideCurrentCpuInfo"]:
    fail("the spoof was not turned off: %r" % ks["Emulate"])
tb, _, restored = v.unset_amd_cpu(ts)
if tb != spoof or restored != 4:
    fail("the spoof / quirks did not come back byte for byte (restored %d)" % restored)
# the second AMD run keeps the FIRST run's saved values (not the AMD values it wrote itself)
ts2, _ = v.set_amd_cpu(ts, 12)
if v.unset_amd_cpu(ts2)[0] != spoof:
    fail("a second AMD run overwrote the saved values")

# ProvideCurrentCpuInfo missing from Quirks: set for AMD, gone again afterwards
noq = variant(orig, q=("\t\t\t<key>ProvideCurrentCpuInfo</key>\n\t\t\t<true/>\n", ""))
if "ProvideCurrentCpuInfo" in noq:
    fail("fixture edit")
tq, _ = v.set_amd_cpu(noq, 8)
if kernel(tq)["Quirks"].get("ProvideCurrentCpuInfo") is not True:
    fail("missing quirk not added")
if v.unset_amd_cpu(tq)[0] != noq:
    fail("the added quirk was not taken back byte for byte")

# Patch as <array/>
empty = re.sub(r"<key>Patch</key>\s*<array>.*?\n\t\t</array>", "<key>Patch</key>\n\t\t<array/>", orig, flags=re.S)
if "<array/>" not in empty:
    fail("fixture edit (empty Patch)")
te, _ = v.set_amd_cpu(empty, 8)
if len(patches(te)) != 24:
    fail("<array/> Patch: %d entries" % len(patches(te)))
tbe = v.unset_amd_cpu(te)[0]
if patches(tbe) != [] or kernel(tbe)["Emulate"] != kernel(empty)["Emulate"]:
    fail("<array/> Patch: not back to nothing")

# foreign entries before and after ours survive a round trip; one with the marker only in
# its middle (not a Comment prefix) is not ours
foreign = orig.replace("<string>Disable RTC wake scheduling</string>", "<string>my rdna3vm-amd: note</string>")
if foreign == orig:
    # the trimmed fixture's third patch has another name: put the text in the first entry's Comment
    first = re.search(r"<key>Comment</key>\s*<string>([^<]*)</string>", orig[orig.index("<key>Patch</key>"):]).group(1)
    foreign = orig.replace("<string>%s</string>" % first, "<string>my rdna3vm-amd: note</string>", 1)
tf, _ = v.set_amd_cpu(foreign, 8)
if v.unset_amd_cpu(tf)[0] != foreign or "my rdna3vm-amd: note" not in tf:
    fail("an entry with the marker inside its Comment was treated as ours")

# configs without a Kernel/Patch array: nothing to take back; refused for AMD
nopatch = re.sub(r"\t\t<key>Patch</key>\s*<array>.*?\n\t\t</array>\n", "", orig, flags=re.S)
if "<key>Patch</key>" in nopatch:
    fail("fixture edit (no Patch)")
if v.unset_amd_cpu(nopatch) != (nopatch, 0, 0):
    fail("--no-amd-cpu touched a config without Kernel/Patch")
try:
    v.set_amd_cpu(nopatch, 8)
    fail("--amd-cpu accepted a config without Kernel/Patch")
except v.PlistEditError as e:
    if "no Kernel/Patch array" not in str(e):
        fail("message: %s" % e)
for bad in (0, 256, -1, "8", None):
    try:
        v.set_amd_cpu(orig, bad)
        fail("core count %r accepted" % (bad,))
    except v.PlistEditError as e:
        if "AMD core count" not in str(e):
            fail("message: %s" % e)

# an edited patch file is refused (the sha256 in vmtool.py and amd_vanilla.SOURCE)
raw = open(v.AMD_PATCH_FILE, "rb").read()
open(tmp + "/edited.plist", "wb").write(raw.replace(b"cpuid_cores_per_package", b"cpuid_cores_per_packagE", 1))
v.AMD_PATCH_FILE = tmp + "/edited.plist"
try:
    v.amd_patches()
    fail("an edited patch file was accepted")
except v.PlistEditError as e:
    if "sha256" not in str(e):
        fail("message: %s" % e)

# --- RestrictEvents / Lilu: the fetch (file:// instead of GitHub, the pins swapped for test zips)
import hashlib, io, os, shutil, zipfile
def kext_zip(name, ver, exe=None, ident=None, extra=()):
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w") as z:
        z.writestr("%s.kext/Contents/Info.plist" % name, plistlib.dumps({
            "CFBundleExecutable": exe or name, "CFBundleIdentifier": ident or "as.vit9696." + name,
            "CFBundleVersion": ver}))
        z.writestr("%s.kext/Contents/MacOS/%s" % (name, exe or name), b"bin-" + name.encode())
        z.writestr("%s.kext.dSYM/Contents/Resources/junk" % name, b"x" * 100)
        for member, data in extra:
            z.writestr(member, data)
    return b.getvalue()
rel = tmp + "/rel"
os.makedirs(rel)
zips = {n: kext_zip(n, "9.9") for n in ("Lilu", "RestrictEvents")}
for n, d in zips.items():
    open("%s/%s-9.9-RELEASE.zip" % (rel, n), "wb").write(d)
pins = tuple((n, "9.9", hashlib.sha256(zips[n]).hexdigest()) for n in ("Lilu", "RestrictEvents"))
url = "file://" + rel + "/%(name)s-%(ver)s-RELEASE.zip"
cache = tmp + "/occache"
msgs = []
ready = v.fetch_oc_kexts(cache, pins, url, log=msgs.append)
if ready != cache + "/ready":
    fail("fetch_oc_kexts returned %r" % ready)
tree = sorted(os.path.relpath(os.path.join(d, f), ready) for d, _, fs in os.walk(ready) for f in fs)
if tree != ["Lilu.kext/Contents/Info.plist", "Lilu.kext/Contents/MacOS/Lilu",
            "RestrictEvents.kext/Contents/Info.plist", "RestrictEvents.kext/Contents/MacOS/RestrictEvents"]:
    fail("the fetch extracted %r (only Info.plist + the executable, no dSYM)" % tree)
if open(ready + "/Lilu.kext/Contents/MacOS/Lilu", "rb").read() != b"bin-Lilu":
    fail("the executable was not extracted as it is")
if not any(m.startswith("fetching ") for m in msgs):
    fail("a first fetch says nothing: %r" % msgs)
# cached: no URL needed any more (the zips are checked again, not trusted)
msgs = []
v.fetch_oc_kexts(cache, pins, "file:///nonexistent/%(name)s-%(ver)s.zip", log=msgs.append)
if any(m.startswith("fetching ") for m in msgs):
    fail("a cached zip was fetched again: %r" % msgs)
# a cached zip that no longer matches is fetched again, a bad download is refused
open(cache + "/Lilu-9.9-RELEASE.zip", "ab").write(b"tampered")
msgs = []
v.fetch_oc_kexts(cache, pins, url, log=msgs.append)
if not any("does not match" in m for m in msgs):
    fail("a tampered cached zip was not noticed: %r" % msgs)
bad = tuple((n, ver, "0" * 64) for n, ver, _ in pins)
shutil.rmtree(cache + "-bad", ignore_errors=True)
try:
    v.fetch_oc_kexts(cache + "-bad", bad, url)
except v.PlistEditError as e:
    if "sha256" not in str(e):
        fail("sha256 mismatch message: %s" % e)
else:
    fail("a download with the wrong sha256 was accepted")
if os.path.exists(cache + "-bad/ready") or os.path.exists(cache + "-bad/Lilu-9.9-RELEASE.zip"):
    fail("a refused download left files behind")
before = sorted(os.listdir(ready))
os.remove(cache + "/RestrictEvents-9.9-RELEASE.zip")
try:
    v.fetch_oc_kexts(cache, pins, "file:///nonexistent/%(name)s-%(ver)s.zip")
except v.PlistEditError as e:
    if "cannot fetch" not in str(e):
        fail("offline message: %s" % e)
else:
    fail("an unreachable download was not reported")
if sorted(os.listdir(ready)) != before or os.path.exists(cache + "/ready.new"):
    fail("a failed fetch changed ready/ or left ready.new")
# a zip with the wrong identity / a missing executable / a traversal member is not used
for label, data in (("identity", kext_zip("Lilu", "9.9", ident="com.evil.Lilu")),
                    ("version", kext_zip("Lilu", "1.0")),
                    ("exe path", kext_zip("Lilu", "9.9", exe="../../x"))):
    d = tmp + "/rel-" + label
    os.makedirs(d, exist_ok=True)
    open(d + "/Lilu-9.9-RELEASE.zip", "wb").write(data)
    try:
        v.fetch_oc_kexts(tmp + "/c-" + label, (("Lilu", "9.9", hashlib.sha256(data).hexdigest()),),
                         "file://" + d + "/%(name)s-%(ver)s-RELEASE.zip")
    except v.PlistEditError:
        pass
    else:
        fail("a zip with the wrong %s was accepted" % label)
print("python checks ok")
PY

# ---- the command line
C=$T/c.plist
cp "$FIX" "$C"
$VT plist "$C" --show-cpu > "$T/out.txt"
grep -qx 'amd-cpu: off' "$T/out.txt" || fail "--show-cpu of a plain config"
$VT plist "$C" --amd-cpu 12 > "$T/out.txt"
grep -qx 'amd-cpu: on, 24 AMD_Vanilla kernel patches, core count 12 (0x0C)' "$T/out.txt" || fail "--amd-cpu message"
grep -qx 'boot-args: keepsyms=1 gemini_check=SUCCESS' "$T/out.txt" || fail "--amd-cpu changed the boot-args line"
[ "$($VT plist "$C" --show-cpu)" = "amd-cpu: on, 24 patches, core count 12" ] || fail "--show-cpu after --amd-cpu"
cp "$C" "$T/c-12.plist"
$VT plist "$C" --amd-cpu 12 > /dev/null
cmp -s "$C" "$T/c-12.plist" || fail "a second --amd-cpu 12 changed the file"
$VT plist "$C" --boot-args "-v rdna3lvl=8" --amd-cpu 16 > /dev/null
[ "$($VT plist "$C" --show)" = "-v rdna3lvl=8" ] || fail "--boot-args next to --amd-cpu"
[ "$($VT plist "$C" --show-cpu)" = "amd-cpu: on, 24 patches, core count 16" ] || fail "--amd-cpu 16"
$VT plist "$C" --no-amd-cpu > "$T/out.txt"
grep -qx 'amd-cpu: off (24 AMD_Vanilla patches removed, 4 values restored)' "$T/out.txt" || fail "--no-amd-cpu message"
python3 -I -c "import sys; a=open(sys.argv[1]).read(); b=open(sys.argv[2]).read(); sys.exit(0 if a.replace('keepsyms=1 gemini_check=SUCCESS','-v rdna3lvl=8')==b else 1)" "$FIX" "$C" ||
    fail "back from AMD mode is not the original (apart from the boot-args)"
for bad in "x" "0" "256" ""; do
    ! $VT plist "$C" --amd-cpu $bad >"$T/out.txt" 2>&1 || fail "--amd-cpu [$bad] accepted"
    grep -q "vmtool: --amd-cpu wants\|vmtool: AMD core count" "$T/out.txt" || fail "no message for --amd-cpu [$bad]"
done
cmp -s <(python3 -I -c "print(open('$C').read().replace('-v rdna3lvl=8','keepsyms=1 gemini_check=SUCCESS'),end='')") "$FIX" || fail "a refused --amd-cpu changed the file"

# ---- rdna3vm: the helpers from the script itself
fn() { sed -n "/^$1() {/,/^}/p" "$VM"; }
{
    echo 'say() { :; }; die() { echo "$*" >&2; exit 1; }'
    sed -n '/^CPU_DEFAULT=/p;/^CPU_HOST_BASE=/p;/^CPUINFO=/p' "$VM"
    for f in load_conf cpu_is_host cpu_host_vendor cpu_threads cpu_cores cpu_qemu_model cpu_amd_patches cpu_oc_args cpu_summary; do
        fn $f
    done
} > "$T/fns.sh"
for f in load_conf cpu_is_host cpu_host_vendor cpu_threads cpu_cores cpu_qemu_model cpu_amd_patches cpu_oc_args cpu_summary; do
    grep -q "^$f() {" "$T/fns.sh" || fail "function $f not found in rdna3vm"
done
grep -q '^CPU_HOST_BASE=' "$T/fns.sh" && grep -q '^CPU_DEFAULT=' "$T/fns.sh" && grep -q '^CPUINFO=' "$T/fns.sh" || fail "CPU variables not found"
printf 'processor\t: 0\nvendor_id\t: AuthenticAMD\nmodel name\t: AMD Ryzen 7 5800X 8-Core Processor\n' > "$T/cpuinfo-amd"
printf 'processor\t: 0\nvendor_id\t: GenuineIntel\nmodel name\t: Intel(R) Core(TM) i7\n' > "$T/cpuinfo-intel"
# the same set -euo pipefail as the script; a CONFIG-style env: CPU_MODEL, CPUS, CPU_THREADS, vendor
h() { env CONF=$T/config RDNA3VM_CPUINFO="${VENDOR:-$T/cpuinfo-amd}" bash -c 'set -euo pipefail; . "$1"; shift; "$@"' _ "$T/fns.sh" "$@"; }
BASE="host,kvm=on,-kvm-pv-unhalt,+invtsc,vmware-cpuid-freq=on,check"
[ "$(CPU_MODEL=host CPUS=12 h cpu_qemu_model)" = "$BASE" ] || fail "cpu_qemu_model host"
[ "$(CPU_MODEL=host CPUS=16 CPU_THREADS=2 h cpu_qemu_model)" = "$BASE,+topoext" ] || fail "threads above 1 need +topoext"
[ "$(CPU_MODEL=host,-svm,+x2apic CPUS=8 h cpu_qemu_model)" = "$BASE,-svm,+x2apic" ] || fail "host,extra flags"
case "$(CPU_MODEL=host CPUS=12 h cpu_qemu_model)" in *vendor=*) fail "host must keep the AMD vendor";; esac
SKL="Skylake-Client,-hle,-rtm,kvm=on,vendor=GenuineIntel,+invtsc,vmware-cpuid-freq=on,+ssse3,+sse4.2,+popcnt,+avx,+aes,+xsave,+xsaveopt,check"
[ "$(CPU_MODEL=$SKL CPUS=12 h cpu_qemu_model)" = "$SKL" ] || fail "the Skylake model must pass unchanged"
[ "$(CPUS=12 h cpu_qemu_model)" = "$SKL" ] || fail "no CPU_MODEL = the default"
[ "$(CPU_MODEL=hostile CPUS=12 h cpu_qemu_model)" = hostile ] || fail "a model that only starts with host is not host mode"
[ "$(CPU_MODEL=host CPUS=12 h cpu_cores)" = 12 ] || fail "cores of 12 vCPUs"
[ "$(CPU_MODEL=host CPUS=16 CPU_THREADS=2 h cpu_cores)" = 8 ] || fail "cores of 16 vCPUs, 2 threads"
[ "$(CPU_MODEL=host CPUS=12 h cpu_oc_args)" = "--amd-cpu 12" ] || fail "oc args of host on AMD"
[ "$(CPU_MODEL=host CPUS=16 CPU_THREADS=2 h cpu_oc_args)" = "--amd-cpu 8" ] || fail "the patch's count is cores, not vCPUs"
[ "$(CPU_MODEL=$SKL CPUS=12 h cpu_oc_args)" = "--no-amd-cpu" ] || fail "oc args of the Skylake model"
[ "$(CPUS=12 h cpu_oc_args)" = "--no-amd-cpu" ] || fail "oc args of the default model"
[ "$(CPU_MODEL=host CPUS=12 VENDOR=$T/cpuinfo-intel h cpu_oc_args)" = "--no-amd-cpu" ] || fail "host mode on an Intel host must not get AMD patches"
for bad in "CPUS=10 CPU_THREADS=4" "CPUS=x" "CPUS=" "CPUS=8 CPU_THREADS=0" "CPUS=8 CPU_THREADS=y" "CPUS=512 CPU_THREADS=1"; do
    ! env CPU_MODEL=host $bad CONF=$T/config RDNA3VM_CPUINFO=$T/cpuinfo-amd bash -c 'set -euo pipefail; . "$1"; cpu_cores' _ "$T/fns.sh" >"$T/out.txt" 2>&1 || fail "[$bad] accepted"
    [ -s "$T/out.txt" ] || fail "no message for [$bad]"
done
CPU_MODEL=host CPUS=16 CPU_THREADS=2 h cpu_summary > "$T/out.txt"
grep -q 'AMD Ryzen 7 5800X.*16 vCPUs = 8 cores x 2 threads.*AMD_Vanilla patches on (core count 8' "$T/out.txt" || fail "cpu_summary: $(cat "$T/out.txt")"
CPU_MODEL=$SKL CPUS=12 h cpu_summary > "$T/out.txt"
grep -q '12 vCPUs = 12 cores x 1 threads, AMD_Vanilla patches off' "$T/out.txt" || fail "cpu_summary of Skylake: $(cat "$T/out.txt")"
CPU_MODEL=host CPUS=12 VENDOR=$T/cpuinfo-intel h cpu_summary > "$T/out.txt"
grep -q 'NOT an AMD host' "$T/out.txt" || fail "cpu_summary of host on Intel: $(cat "$T/out.txt")"

# load_conf (the Haswell-noTSX migration) must not undo CPU_MODEL=host
for m in host "host,-svm"; do
    printf 'CPU_MODEL="%s"\nCPUS=12\n' "$m" > "$T/config"
    cp "$T/config" "$T/config.before"
    env CONF=$T/config bash -c 'set -euo pipefail; . "$1"; load_conf; echo "$CPU_MODEL"' _ "$T/fns.sh" > "$T/out.txt"
    [ "$(cat "$T/out.txt")" = "$m" ] || fail "load_conf changed CPU_MODEL=$m to $(cat "$T/out.txt")"
    cmp -s "$T/config" "$T/config.before" || fail "load_conf edited the config of CPU_MODEL=$m"
done
# the migration edits the config with GNU sed -i (rdna3vm runs on the Linux host only): not on CI's macOS runner
if sed --version >/dev/null 2>&1; then
    printf 'CPU_MODEL="Haswell-noTSX,kvm=on"\nCPUS=12\n' > "$T/config"
    env CONF=$T/config bash -c 'set -euo pipefail; . "$1"; load_conf >/dev/null; echo "$CPU_MODEL"' _ "$T/fns.sh" > "$T/out.txt"
    grep -q '^Skylake-Client' "$T/out.txt" || fail "load_conf: the old Haswell-noTSX migration is gone"
fi

# ---- the whole cmd__oc chain with stubbed losetup / mount: the config's CPU_MODEL decides
mkdir -p "$T/bin" "$T/ocw"
cat > "$T/bin/losetup" <<'EOS'
#!/bin/sh
case "$1" in -d) exit 0 ;; esac
echo /dev/loop99
EOS
cat > "$T/bin/mount" <<'EOS'
#!/bin/sh
for a; do mnt=$a; done
mkdir -p "$mnt/EFI/OC"
cp "$FIXTURE" "$mnt/EFI/OC/config.plist"
[ -d "$KSTATE" ] && cp -R "$KSTATE" "$mnt/EFI/OC/Kexts"
exit 0
EOS
cat > "$T/bin/umount" <<'EOS'
#!/bin/sh
cp "$1/EFI/OC/config.plist" "$RESULT"
rm -rf "$KSTATE"; [ -d "$1/EFI/OC/Kexts" ] && cp -R "$1/EFI/OC/Kexts" "$KSTATE"
rm -rf "$1/EFI"
EOS
printf '#!/bin/sh\nexit 0\n' > "$T/bin/mountpoint"
printf '#!/bin/sh\nexit 0\n' > "$T/bin/sync"
chmod +x "$T/bin/"*
: > "$T/ocw/OpenCore-rdna3.img"
# the image: what the previous run left in OpenCore (this chain's FIXTURE is the previous RESULT)
cp "$FIX" "$T/image.plist"
oc() {   # oc ARGS...: one _oc call on the "image"; the result becomes the next call's image
    env PATH="$T/bin:$PATH" RDNA3VM_DIR="$T/ocw" RDNA3VM_CPUINFO="$T/cpuinfo-amd" FIXTURE="$T/image.plist" RESULT="$T/image-out.plist" \
        KSTATE="$T/kstate" RDNA3VM_OCKEXT_DIR="${READY-$T/ready}" bash "$VM" _oc "$@" && cp "$T/image-out.plist" "$T/image.plist"
}
# the image's EFI/OC/Kexts: the OSX-KVM Lilu 1.6.8 and VirtualSMC; "ready" = what vmtool ockexts gives
mk_kext() {   # mk_kext DIR NAME VERSION
    mkdir -p "$1/$2.kext/Contents/MacOS"
    printf '<plist><dict><key>CFBundleExecutable</key>\n<string>%s</string>\n<key>CFBundleVersion</key>\n<string>%s</string></dict></plist>\n' "$2" "$3" > "$1/$2.kext/Contents/Info.plist"
    echo "$2 $3" > "$1/$2.kext/Contents/MacOS/$2"
}
mk_kext "$T/kstate" Lilu 1.6.8; mk_kext "$T/kstate" VirtualSMC 1.3.3
mk_kext "$T/ready" Lilu 1.7.2; mk_kext "$T/ready" RestrictEvents 1.1.6
adds() { python3 -I -c "import plistlib,sys; print(' '.join(a['BundlePath'] for a in plistlib.load(open(sys.argv[1],'rb'))['Kernel']['Add']))" "$1"; }
fver() { sed -n '/CFBundleVersion/{n;s/.*<string>\(.*\)<.string>.*/\1/p;}' "$1/Contents/Info.plist"; }   # ';}': BSD sed (macOS)
cp -R "$T/kstate" "$T/kstate.orig"
printf 'CPU_MODEL="host"\nCPUS=12\n' > "$T/ocw/config"
oc --boot-args "-v keepsyms=1" > "$T/out.txt" || fail "_oc with CPU_MODEL=host failed"
grep -qx 'amd-cpu: on, 24 AMD_Vanilla kernel patches, core count 12 (0x0C)' "$T/out.txt" || fail "_oc did not say the AMD patches are on: $(cat "$T/out.txt")"
[ "$($VT plist "$T/image.plist" --show-cpu)" = "amd-cpu: on, 24 patches, core count 12" ] || fail "the image has no AMD patches after a host run"
# RestrictEvents: Lilu 1.7.2 replaces the image's 1.6.8 (kept), RestrictEvents is listed after Lilu
grep -qx "RestrictEvents 1.1.6 + Lilu 1.7.2 injected (the AMD CPU's name in About This Mac)" "$T/out.txt" || fail "_oc did not say RestrictEvents is in: $(cat "$T/out.txt")"
[ "$(adds "$T/image.plist")" = "Lilu.kext VirtualSMC.kext RestrictEvents.kext" ] || fail "Kernel/Add after a host run: $(adds "$T/image.plist")"
[ "$(fver "$T/kstate/Lilu.kext")" = 1.7.2 ] && [ "$(fver "$T/kstate/Lilu.kext.rdna3vm-orig")" = 1.6.8 ] || fail "Lilu 1.7.2 in, the image's 1.6.8 kept: $(ls -a "$T/kstate")"
[ "$(fver "$T/kstate/RestrictEvents.kext")" = 1.1.6 ] && [ -f "$T/kstate/RestrictEvents.kext/Contents/MacOS/RestrictEvents" ] || fail "RestrictEvents.kext not copied"
[ -d "$T/kstate/VirtualSMC.kext" ] || fail "an unrelated kext of the image went"
python3 -I - "$T/image.plist" <<'PY' || fail "RestrictEvents / Lilu Kernel/Add entries"
import plistlib, sys
add = {a["BundlePath"]: a for a in plistlib.load(open(sys.argv[1], "rb"))["Kernel"]["Add"]}
for k, exe in (("Lilu.kext", "Contents/MacOS/Lilu"), ("RestrictEvents.kext", "Contents/MacOS/RestrictEvents")):
    a = add[k]
    if not a["Enabled"] or a["ExecutablePath"] != exe or a["PlistPath"] != "Contents/Info.plist" \
            or a["MinKernel"] != "" or a["MaxKernel"] != "" or a["Arch"] not in ("Any", "x86_64"):
        sys.exit("%s: %r" % (k, a))
PY
# the next run, same mode: nothing moves
cp "$T/image.plist" "$T/image.1"; cp -R "$T/kstate" "$T/kstate.1"
oc --boot-args "-v keepsyms=1" >/dev/null || fail "_oc second run"
cmp -s "$T/image.plist" "$T/image.1" || fail "a second host run changed the image"
diff -r "$T/kstate" "$T/kstate.1" >/dev/null || fail "a second host run changed EFI/OC/Kexts"
# the fetch failed (no network): loud, the VM still boots, everything RestrictEvents is out again, the old Lilu is back
cp "$T/image.plist" "$T/image.host"; cp -R "$T/kstate" "$T/kstate.host"
https_proxy=http://127.0.0.1:9 READY= oc --boot-args "-v keepsyms=1" > "$T/out.txt" 2>&1 || fail "_oc must not fail when RestrictEvents cannot be fetched"
grep -q '^!! RestrictEvents is not injected' "$T/out.txt" || fail "a failed fetch is not loud: $(cat "$T/out.txt")"
[ "$($VT plist "$T/image.plist" --show-cpu)" = "amd-cpu: on, 24 patches, core count 12" ] || fail "a failed fetch must leave the AMD patches on"
[ "$(adds "$T/image.plist")" = "Lilu.kext VirtualSMC.kext" ] && [ ! -e "$T/kstate/RestrictEvents.kext" ] || fail "a failed fetch left RestrictEvents: $(adds "$T/image.plist")"
[ "$(fver "$T/kstate/Lilu.kext")" = 1.6.8 ] && [ ! -e "$T/kstate/Lilu.kext.rdna3vm-orig" ] || fail "a failed fetch left Lilu 1.7.2: $(ls -a "$T/kstate")"
cp "$T/image.host" "$T/image.plist"; rm -rf "$T/kstate"; cp -R "$T/kstate.host" "$T/kstate"
# --show does not edit (and does not need the CPU)
oc --show >/dev/null && cmp -s "$T/image.plist" "$T/image.1" || fail "_oc --show changed the image"
# CPUS and CPU_THREADS change the count
printf 'CPU_MODEL="host"\nCPUS=16\nCPU_THREADS=2\n' > "$T/ocw/config"
oc --boot-args "-v keepsyms=1" >/dev/null || fail "_oc CPUS=16 CPU_THREADS=2"
[ "$($VT plist "$T/image.plist" --show-cpu)" = "amd-cpu: on, 24 patches, core count 8" ] || fail "16 vCPUs x 2 threads must give core count 8"
# an Intel host: host mode but no AMD patches
cp "$FIX" "$T/image.plist"
env PATH="$T/bin:$PATH" RDNA3VM_DIR="$T/ocw" RDNA3VM_CPUINFO="$T/cpuinfo-intel" FIXTURE="$T/image.plist" RESULT="$T/image-out.plist" \
    KSTATE="$T/kstate-intel" RDNA3VM_OCKEXT_DIR="$T/ready" bash "$VM" _oc --boot-args "-v" >/dev/null && cp "$T/image-out.plist" "$T/image.plist"
[ "$($VT plist "$T/image.plist" --show-cpu)" = "amd-cpu: off" ] || fail "an Intel host got AMD patches"
[ ! -e "$T/kstate-intel" ] || fail "an Intel host got RestrictEvents / Lilu files"
cp "$FIX" "$T/image.plist"
# switch back to the Skylake model: no manual edit, the OpenCore config is the original again
printf 'CPU_MODEL="host"\nCPUS=12\n' > "$T/ocw/config"
oc --boot-args "-v keepsyms=1" >/dev/null || fail "_oc host"
printf 'CPU_MODEL="%s"\nCPUS=12\n' "$SKL" > "$T/ocw/config"
oc --boot-args "keepsyms=1 gemini_check=SUCCESS" > "$T/out.txt" || fail "_oc back to Skylake"
grep -qx 'amd-cpu: off (24 AMD_Vanilla patches removed, 4 values restored)' "$T/out.txt" || fail "_oc back: $(cat "$T/out.txt")"
cmp -s "$T/image.plist" "$FIX" || fail "after switching back the OpenCore config is not the original"
grep -q '^RestrictEvents taken out of OpenCore' "$T/out.txt" && grep -q "^the image's own Lilu 1.6.8 is back" "$T/out.txt" || fail "_oc back: no word about RestrictEvents / Lilu: $(cat "$T/out.txt")"
[ ! -e "$T/kstate/RestrictEvents.kext" ] && [ ! -e "$T/kstate/Lilu.kext.rdna3vm-orig" ] && [ "$(fver "$T/kstate/Lilu.kext")" = 1.6.8 ] || fail "after switching back: $(ls -a "$T/kstate")"
diff -r "$T/kstate" "$T/kstate.orig" >/dev/null || fail "after switching back EFI/OC/Kexts is not the image's original"
# an image without Lilu: the AMD mode adds it (and a marker), the next mode takes it out with its entry
python3 -I - "$FIX" "$T/nolilu.plist" <<'PY'
import re, sys
t = open(sys.argv[1]).read()
t2 = re.sub(r"\t\t\t<dict>(?:(?!</dict>).)*?<string>Lilu\.kext</string>.*?</dict>\n", "", t, count=1, flags=re.S)
assert t2 != t
open(sys.argv[2], "w").write(t2)
PY
cp "$T/nolilu.plist" "$T/image.plist"; rm -rf "$T/kstate"; mk_kext "$T/kstate" VirtualSMC 1.3.3
printf 'CPU_MODEL="host"\nCPUS=12\n' > "$T/ocw/config"
oc --boot-args "-v" >/dev/null || fail "_oc host without a Lilu"
[ "$(adds "$T/image.plist")" = "VirtualSMC.kext Lilu.kext RestrictEvents.kext" ] || fail "no Lilu: Kernel/Add $(adds "$T/image.plist")"
[ "$(fver "$T/kstate/Lilu.kext")" = 1.7.2 ] && [ -e "$T/kstate/.rdna3vm-lilu-added" ] || fail "no Lilu: files $(ls -a "$T/kstate")"
printf 'CPU_MODEL="%s"\nCPUS=12\n' "$SKL" > "$T/ocw/config"
oc --boot-args "-v" >/dev/null || fail "_oc back to Skylake without a Lilu"
[ "$(adds "$T/image.plist")" = "VirtualSMC.kext" ] && [ ! -e "$T/kstate/Lilu.kext" ] && [ ! -e "$T/kstate/RestrictEvents.kext" ] && [ ! -e "$T/kstate/.rdna3vm-lilu-added" ] ||
    fail "no Lilu, back: $(adds "$T/image.plist") / $(ls -a "$T/kstate")"
cp "$FIX" "$T/image.plist"; rm -rf "$T/kstate"; cp -R "$T/kstate.orig" "$T/kstate"
# no config at all (first prepare): the default Skylake model, nothing to take out, no failure
rm -f "$T/ocw/config"
oc --boot-args "keepsyms=1 gemini_check=SUCCESS" >/dev/null || fail "_oc without a config"
cmp -s "$T/image.plist" "$FIX" || fail "_oc without a config changed the image"

# ---- qemu_common and result: the call sites
grep -q -- '-cpu "\$cpu_arg"' "$VM" || fail "qemu_common does not pass the computed -cpu"
! grep -n -e '-cpu "\$CPU_MODEL"' "$VM" || fail "a call site still passes CPU_MODEL straight to qemu"
grep -q 'sed .s/^/vm-cpu: /. "\$WORK/last-cpu"' "$VM" || fail "result does not put last-cpu into 00-host.txt"
grep -q 'VM CPU: ' "$VM" || fail "qemu_common prints no VM CPU line"
h2() { env WORK="$T/qw" CONF=$T/config RDNA3VM_CPUINFO="$T/cpuinfo-amd" CPUS=12 CPU_MODEL=host bash -c 'set -euo pipefail; . "$1"; shift; "$@"' _ "$T/fns2.sh" "$@"; }
{
    cat "$T/fns.sh"
    fn qemu_common
    echo 'OSK=k; RAM_GB=4; OVMF_CODE=/c; MACDISK=/m'
} > "$T/fns2.sh"
mkdir -p "$T/qw"
h2 bash -c 'true' >/dev/null 2>&1 || true
env WORK="$T/qw" CONF=$T/config RDNA3VM_CPUINFO="$T/cpuinfo-amd" CPUS=16 CPU_THREADS=2 CPU_MODEL=host OSK=k RAM_GB=4 OVMF_CODE=/c MACDISK=/m \
    bash -c 'set -euo pipefail; . "$1"; qemu_common; printf "%s\n" "${QARGS[@]}"' _ "$T/fns2.sh" > "$T/out.txt" || fail "qemu_common failed"
grep -qx 'VM CPU: -cpu host,kvm=on,-kvm-pv-unhalt,+invtsc,vmware-cpuid-freq=on,check,+topoext -smp 16,sockets=1,cores=8,threads=2' "$T/out.txt" || fail "qemu_common line: $(cat "$T/out.txt")"
grep -qx -- '-smp' "$T/out.txt" && grep -qx '16,sockets=1,cores=8,threads=2' "$T/out.txt" || fail "QARGS -smp: $(cat "$T/out.txt")"
grep -qx 'host,kvm=on,-kvm-pv-unhalt,+invtsc,vmware-cpuid-freq=on,check,+topoext' "$T/out.txt" || fail "QARGS -cpu"
grep -q 'AMD_Vanilla patches on (core count 8' "$T/qw/last-cpu" || fail "last-cpu: $(cat "$T/qw/last-cpu")"
grep -q -- '-cpu host,kvm=on' "$T/qw/last-cpu" || fail "last-cpu has no -cpu line"
echo "test_vm_amd_cpu: ok"
