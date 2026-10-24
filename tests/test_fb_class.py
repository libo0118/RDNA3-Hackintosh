"""DRIVER-PLAN N44: the framebuffer's leaf class name, the one CoreDisplay's
_GetGPUVendorForFramebufferService reads (IOObjectCopyClass) to decide whether
WindowServer may use IOPresentment.
 (1) one name everywhere: RDNA3_FB_CLASS of fb/rdna3-fb.cpp = both IOClass strings of
     fb/Info.plist = what fb/Makefile's sed extracts = FB_CLASS of tools/triage_reports.py;
     the leaf derives from RDNA3Framebuffer and the service name stays RDNA3Framebuffer;
 (2) the name contains CoreDisplay's "AMD" (case-insensitive) and none of its other strings
     ("Intel", checked first, "AppleParavirt", "NVDA"), and is not a class of Apple's AMD kexts;
 (3) triage: gpu_vendor_rows on CoreDisplay lines of an old report (vendor 1, class
     RDNA3Framebuffer: INFO "IOPresentment off ..."), of the new class (vendor 4: PASS, and the
     useIOP_onAMD lead without planes), the new class still vendor 1 (FAIL: rename), another
     class (FAIL: stale kext), another vendor (WARN), no line / no log (WARN, never silence);
 (4) the ioreg parsers (_fb_blocks, fb_n40_lines) read a block printed with the new class
     exactly as one with the old class, and present_mode_lines carries the vendor row.
Run: python3 -I -B tests/test_fb_class.py"""
import io
import os
import plistlib
import re
import subprocess
import sys
import tarfile
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import triage_reports as tr  # noqa: E402

fails = []


def check(cond, what):
    if not cond:
        fails.append(what)


def rows_of(rows, label="GPU vendor"):
    return [r for r in rows if r[1] == label]


# ---- (1) one name everywhere ----
src = open(os.path.join(ROOT, "fb", "rdna3-fb.cpp")).read()
m = re.search(r"^#define RDNA3_FB_CLASS\s+(\w+)\s*$", src, re.M)
check(m is not None, "fb/rdna3-fb.cpp has no '#define RDNA3_FB_CLASS <name>'")
cls = m.group(1) if m else ""
with open(os.path.join(ROOT, "fb", "Info.plist"), "rb") as f:
    pl = plistlib.load(f)
pers = pl["IOKitPersonalities"]
check(sorted(pers) == ["RDNA3Framebuffer", "RDNA3FramebufferHead"], "personality keys changed: %r" % sorted(pers))
for k, d in pers.items():
    check(d.get("IOClass") == cls, "Info.plist %s IOClass %r, RDNA3_FB_CLASS %r" % (k, d.get("IOClass"), cls))
check(pl["CFBundleIdentifier"] == "io.github.voten641.RDNA3Framebuffer", "bundle id changed")
check(tr.FB_CLASS == cls, "triage FB_CLASS %r, RDNA3_FB_CLASS %r" % (tr.FB_CLASS, cls))
check(tr.FB_CLASS_OLD == "RDNA3Framebuffer", "triage FB_CLASS_OLD %r" % tr.FB_CLASS_OLD)
check(re.search(r"^class RDNA3_FB_CLASS : public RDNA3Framebuffer \{", src, re.M) is not None,
      "the leaf class does not derive from RDNA3Framebuffer")
check("OSDefineMetaClassAndStructors(RDNA3_FB_CLASS, RDNA3Framebuffer)" in src, "no metaclass for the leaf")
check('setName("RDNA3Framebuffer");' in src, "start() no longer names the service RDNA3Framebuffer")
mk = open(os.path.join(ROOT, "fb", "Makefile")).read()
sedm = re.search(r"^FB_CLASS := \$\(shell sed -n '([^']+)' rdna3-fb\.cpp\)$", mk, re.M)
check(sedm is not None, "fb/Makefile has no FB_CLASS sed")
if sedm:
    got = subprocess.run(["sed", "-n", sedm.group(1), os.path.join(ROOT, "fb", "rdna3-fb.cpp")],
                         capture_output=True, text=True).stdout.strip()
    check(got == cls, "fb/Makefile's sed gives %r, RDNA3_FB_CLASS %r" % (got, cls))

# ---- (2) what CoreDisplay makes of the name ----
low = cls.lower()
check("amd" in low, "%s has no AMD in it: CoreDisplay counts it as vendor 1" % cls)
for other in ("intel", "appleparavirt", "nvda"):   # CoreDisplay's other strings (probe on 24H32)
    check(other not in low, "%s contains %r: CoreDisplay may class it as another vendor" % (cls, other))
for apple in ("AMDRadeonX", "AMDFramebuffer", "ATIFramebuffer", "AtiFbStub", "AMDSupport"):
    check(not cls.startswith(apple), "%s looks like a class of Apple's AMD kexts (%s)" % (cls, apple))

# ---- (3) gpu_vendor_rows ----
LINE = ("2026-10-17 19:23:43.429 I  WindowServer[158:28c] [com.apple.CoreDisplay:default] [INFO] - "
        "GetSystemGPUVendors_block_invoke: %s %s %s IOService:/AppleACPIPlatformExpert/PCI0/AppleACPIPCI/S80@10/%s\n")
H0 = "RDNA3Framebuffer"
H1 = "RDNA3Accel/RDNA3DisplayHead/RDNA3Framebuffer"


def cd(vendor, klass, twice=True):
    s = LINE % ("0x1000002f2", vendor, klass, H0) + LINE % ("0x1000002d1", vendor, klass, H1)
    return s + s if twice else s   # CoreDisplay logs them again on a second call: one row per framebuffer


old = cd(1, "RDNA3Framebuffer")
r = rows_of(tr.gpu_vendor_rows(old))
check([x[0] for x in r] == ["INFO", "INFO"], "old report: two INFO rows, got %r" % r)
check(all(x[2].startswith("IOPresentment off: framebuffer classed vendor 1 (unknown)") for x in r), "old text: %r" % r)
check("older than DRIVER-PLAN N44" in r[0][2], "old: why: %r" % r)
check(tr.gpu_vendors(old) == {"0x1000002d1": (1, "RDNA3Framebuffer"), "0x1000002f2": (1, "RDNA3Framebuffer")},
      "gpu_vendors of an old report: %r" % tr.gpu_vendors(old))

new = cd(4, cls)
r = rows_of(tr.gpu_vendor_rows(new, planes=True))
check([x[0] for x in r] == ["PASS", "PASS"], "new class, vendor 4, planes: two PASS rows, got %r" % r)
check("vendor 4 (AMD)" in r[0][2] and cls in r[0][2], "new text: %r" % r)
r = rows_of(tr.gpu_vendor_rows(new, planes=False))
check([x[0] for x in r] == ["PASS", "PASS", "INFO"], "vendor 4 without planes: the useIOP_onAMD lead, got %r" % r)
check("useIOP_onAMD" in r[-1][2], "lead text: %r" % r)

r = rows_of(tr.gpu_vendor_rows(cd(1, cls)))
check([x[0] for x in r] == ["FAIL", "FAIL"], "new class still vendor 1: FAIL, got %r" % r)
check(all("IOPresentment off: framebuffer classed vendor 1 (unknown)" in x[2] and "change RDNA3_FB_CLASS" in x[2]
          for x in r), "rename hint: %r" % r)
r = rows_of(tr.gpu_vendor_rows(cd(1, "SomethingElse")))
check([x[0] for x in r] == ["FAIL", "FAIL"] and "stale" in r[0][2], "another class: stale FAIL, got %r" % r)
r = rows_of(tr.gpu_vendor_rows(cd(8, cls, twice=False)))
check([x[0] for x in r] == ["WARN", "WARN"] and "NVIDIA" in r[0][2], "vendor 8: WARN, got %r" % r)
mixed = LINE % ("0x1000002f2", 4, cls, H0) + LINE % ("0x1000002d1", 1, "RDNA3Framebuffer", H1)
r = rows_of(tr.gpu_vendor_rows(mixed))
check([x[0] for x in r] == ["INFO", "PASS"], "mixed (head 1 old): no useIOP lead, got %r" % r)
r = rows_of(tr.gpu_vendor_rows("2026-10-17 19:23:43.435 I  WindowServer[158:28c] (IOPresentment) Info: x\n"))
check([x[0] for x in r] == ["WARN"] and "no 'GetSystemGPUVendors_block_invoke' line" in r[0][2], "no line: %r" % r)
r = rows_of(tr.gpu_vendor_rows(""))
check([x[0] for x in r] == ["WARN"] and "missing or empty" in r[0][2], "no log: %r" % r)

# ---- (4) the ioreg parsers and present_mode_lines with the new class ----
BLOCK = """+-o RDNA3Framebuffer  <class %s, id 0x1000002d1, registered, matched, active, busy 0 (2 ms), retain 22>
  | {
  |   "IOFBDependentIndex" = %d
  |   "IOFramebufferOpenGLIndex" = %d
  |   "RDNA3,Switches" = {"dispflags"=0,"glorder"=1}
  | }

"""


def ioreg(klass):
    return "$ ioreg -l -w0 -r -c RDNA3Framebuffer\n" + BLOCK % (klass, 0, 0) + BLOCK % (klass, 1, 1) + "[exit 0]\n"


check(tr._fb_blocks(ioreg(cls)) == tr._fb_blocks(ioreg("RDNA3Framebuffer")) == [("0", "0", None), ("1", "1", None)],
      "_fb_blocks differs between the old and new class: %r / %r" % (
          tr._fb_blocks(ioreg(cls)), tr._fb_blocks(ioreg("RDNA3Framebuffer"))))


def archive(d, name, members):
    p = os.path.join(d, name)
    with tarfile.open(p, "w:gz") as t:
        for n, body in members.items():
            data = body.encode()
            ti = tarfile.TarInfo("rdna3-report/" + n)
            ti.size = len(data)
            t.addfile(ti, io.BytesIO(data))
    return p


with tempfile.TemporaryDirectory() as d:
    sw = {}
    for klass, cdtxt in ((cls, new), ("RDNA3Framebuffer", old)):
        p = archive(d, "rdna3-report-%s.tar.gz" % klass, {"12-ioreg-fb.txt": ioreg(klass), "24-log-coredisplay.txt": cdtxt})
        sw[klass] = [r for r in tr.fb_n40_lines(p, "", "rdna3fb=1") if r[1] == "switches"]
        pm = rows_of(tr.present_mode_lines(p, "debug.rdna3accel_iop_ok: 0\ndebug.rdna3accel_iop_empty: 5\n",
                                           "rdna3fb=1", host=""))
        want = ["PASS", "PASS", "INFO"] if klass == cls else ["INFO", "INFO"]
        check([x[0] for x in pm] == want, "present_mode_lines %s: GPU vendor rows %r" % (klass, pm))
        fbr = [r for r in tr.present_mode_lines(p, "", "rdna3fb=1", host="") if r[1] == "fb index"]
        check(fbr and "they agree" in fbr[0][2], "present_mode_lines %s: fb index %r" % (klass, fbr))
    check(sw[cls] == sw["RDNA3Framebuffer"] and len(sw[cls]) == 2, "fb_n40_lines switches differ: %r" % sw)

if fails:
    for f in fails:
        print("test_fb_class: FAIL " + f)
    sys.exit(1)
print("test_fb_class: ok (%s)" % cls)
