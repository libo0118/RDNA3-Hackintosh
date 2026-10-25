"""DRIVER-PLAN N46 (1): tools/triage_reports.py's "display pairing" row and collect.sh's pairing_report()
(16-ioaccel-pairing.txt): the IOAccelTypes / IOAccelIndex / IOAccelRevision WindowServer's
IOAccelFindAccelerator(fb) reads from each IOFramebuffer.
 - every lit head's framebuffer naming RDNA3Accelerator with IOAccelRevision 2 and an IOAccelIndex: PASS
   (the leaf class and the older RDNA3Framebuffer class alike);
 - IOAccelTypes / IOAccelRevision / IOAccelIndex missing, another accelerator named, another revision: WARN;
 - a head with RDNA3,Connected = No is not judged; none connected: INFO; no framebuffer at all: WARN;
 - no 16-ioaccel-pairing.txt (an older report), an empty one or collect.sh's wrapper only: INFO;
 - the kernel's IOAccelTypes leaves out the platform expert device (MacBookPro16,1) that `ioreg -p IOService` shows
   below the root: card run 2026-10-25c (macOS 26.4.1) paired, and the row said WARN "not a registered one";
   the compare tolerates one extra leading component on either side;
 - present_mode_lines carries the rows from a report archive;
 - collect.sh packs the file and pairing_report() prints what the triage reads (a fake ioreg).
Run: python3 -I -B tests/test_triage_pairing.py"""
import io
import os
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


def levels(rows):
    return [r[0] for r in rows]


ACCEL = "accelerator: IOService:/MacPro/PCI0/AppleACPIPCI/GFX0@0/RDNA3Accel/RDNA3Accelerator\n"
TYPES = '"IOAccelTypes" = "IOService:/MacPro/PCI0/AppleACPIPCI/GFX0@0/RDNA3Accel/RDNA3Accelerator"\n'


def fb(head, cls="AMDRDNA3Framebuffer", connected="Yes", types=TYPES, index='"IOAccelIndex" = 0\n',
       revision='"IOAccelRevision" = 2\n'):
    return ("+-o RDNA3Framebuffer  <class %s, id 0x1000%05d, registered, matched, active, busy 0, retain 12>\n"
            '"RDNA3,Connected" = %s\n' % (cls, head, connected)) + types + index + revision


def pfile(*blocks, accel=ACCEL):
    return "$ pairing_report\n== framebuffers (x)\n" + "".join(blocks) + "== accelerators (x)\n" + accel + "[exit 0]\n"


# both heads paired: PASS per framebuffer, nothing else
r = tr.pairing_rows(pfile(fb(1), fb(2)))
check(levels(r) == ["PASS", "PASS"] and all(x[1] == "display pairing" for x in r), "two paired heads: %r" % r)
check("IOAccelRevision 2" in r[0][2] and "id 0x100000001" in r[0][2] and "IOAccelIndex 0" in r[0][2] and "head" not in r[0][2]
      and "AMDRDNA3Framebuffer" in r[0][2] and "registered accelerator IOService:/MacPro/" in r[0][2], "PASS text: %r" % (r[0],))

# the older class (reports before N44) pairs the same way and says it is the old class
r = tr.pairing_rows(pfile(fb(1, cls="RDNA3Framebuffer")))
check(levels(r) == ["PASS"] and "older than DRIVER-PLAN N44" in r[0][2], "old class: %r" % r)

# a key missing, another accelerator, another revision: WARN naming what is wrong and the registered accelerator
cases = (
    ("no IOAccelTypes", fb(1, types=""), "IOAccelTypes is not set"),
    ("other accelerator", fb(1, types='"IOAccelTypes" = "IOService:/PCI0/GFX0/AppleIntelAccelerator"\n'),
     "names something else"),
    ("revision 1", fb(1, revision='"IOAccelRevision" = 1\n'), "IOAccelRevision is 1, not 2"),
    ("no revision", fb(1, revision=""), "IOAccelRevision is not set"),
    ("no index", fb(1, index=""), "IOAccelIndex is not set"),
    ("no key at all", fb(1, types="", index="", revision=""), "IOAccelTypes is not set"),
)
for what, blk, want in cases:
    r = tr.pairing_rows(pfile(blk))
    check(levels(r) == ["WARN"] and want in r[0][2] and "RDNA3Accelerator" in r[0][2], "%s: %r" % (what, r))
# one head fine, one not: one PASS, one WARN
r = tr.pairing_rows(pfile(fb(1), fb(2, types="")))
check(levels(r) == ["PASS", "WARN"], "one bad head: %r" % r)
# IOAccelTypes must be one of the registered accelerators, not any string with the class name in it
other = '"IOAccelTypes" = "IOService:/MacPro/PCI0/GFX1@0/RDNA3Accel/RDNA3Accelerator"\n'
r = tr.pairing_rows(pfile(fb(1, types=other)))
check(levels(r) == ["WARN"] and "not a registered one" in r[0][2] and "GFX0@0" in r[0][2], "unregistered path: %r" % r)
# a bare class name is accepted and printed as the match; the other registered path of two is found
r = tr.pairing_rows(pfile(fb(1, types='"IOAccelTypes" = "RDNA3Accelerator"\n')))
check(levels(r) == ["PASS"] and "registered accelerator RDNA3Accelerator" in r[0][2], "bare class name: %r" % r)
two = ACCEL + "accelerator: IOService:/MacPro/PCI0/GFX1@0/RDNA3Accel/RDNA3Accelerator\n"
r = tr.pairing_rows(pfile(fb(1, types=other), accel=two))
check(levels(r) == ["PASS"] and "registered accelerator IOService:/MacPro/PCI0/GFX1@0" in r[0][2], "second accelerator: %r" % r)
# card run 2026-10-25c, macOS 26.4.1, the real strings: the accelerator path (collect.sh before the fix) has the
# platform expert device, IOAccelTypes (the kernel's getPath) does not
REAL_TYPES = '"IOAccelTypes" = "IOService:/AppleACPIPlatformExpert/PCI0/AppleACPIPCI/S80@10/RDNA3Accelerator"\n'
REAL_ACCEL = "accelerator: IOService:/MacBookPro16,1/AppleACPIPlatformExpert/PCI0/AppleACPIPCI/S80@10/RDNA3Accelerator\n"
r = tr.pairing_rows(pfile(fb(1, types=REAL_TYPES), fb(2, types=REAL_TYPES), accel=REAL_ACCEL))
check(levels(r) == ["PASS", "PASS"] and "registered accelerator IOService:/MacBookPro16,1/" in r[0][2],
      "26.4.1 strings (extra leading component in the accelerator path): %r" % r)
# the fixed collect.sh prints the path without it: equal
r = tr.pairing_rows(pfile(fb(1, types=REAL_TYPES), accel=REAL_ACCEL.replace("MacBookPro16,1/", "")))
check(levels(r) == ["PASS"], "26.4.1 strings (collect.sh without the root component): %r" % r)
# the extra component on the other side tolerated too; a different PCI location is still not the accelerator
r = tr.pairing_rows(pfile(fb(1, types=REAL_TYPES.replace("IOService:/", "IOService:/MacBookPro16,1/")),
                          accel=REAL_ACCEL.replace("MacBookPro16,1/", "")))
check(levels(r) == ["PASS"], "extra component in IOAccelTypes: %r" % r)
r = tr.pairing_rows(pfile(fb(1, types=REAL_TYPES.replace("S80@10", "S81@11")), accel=REAL_ACCEL))
check(levels(r) == ["WARN"] and "not a registered one" in r[0][2], "another slot: %r" % r)
r = tr.pairing_rows(pfile(fb(1, types=REAL_TYPES.replace("PCI0/", "")), accel=REAL_ACCEL))
check(levels(r) == ["WARN"], "a component missing in the middle is no match: %r" % r)
# no accelerator path printed: the class name in the value has to do
r = tr.pairing_rows(pfile(fb(1), accel="(no RDNA3Accelerator in the IOService plane)\n"))
check(levels(r) == ["PASS"] and "registered accelerator" not in r[0][2], "no registered list: %r" % r)
# no accelerator registered: the WARN says none was printed
r = tr.pairing_rows(pfile(fb(1, types=""), accel="(no RDNA3Accelerator in the IOService plane)\n"))
check(levels(r) == ["WARN"] and "none printed" in r[0][2], "no accelerator: %r" % r)

# a disconnected head is not judged; none connected is INFO; a head without the key is judged
r = tr.pairing_rows(pfile(fb(1), fb(2, connected="No", types="")))
check(levels(r) == ["PASS"], "disconnected head skipped: %r" % r)
r = tr.pairing_rows(pfile(fb(1, connected="No"), fb(2, connected="No")))
check(levels(r) == ["INFO"] and "none connected" in r[0][2], "none connected: %r" % r)
r = tr.pairing_rows(pfile(fb(1).replace('"RDNA3,Connected" = Yes\n', "").replace(TYPES, "")))
check(levels(r) == ["WARN"], "no Connected key: judged: %r" % r)

# no framebuffer instance: WARN
r = tr.pairing_rows(pfile())
check(levels(r) == ["WARN"] and "no framebuffer instance" in r[0][2], "no framebuffer: %r" % r)

# an older report (no member), an empty one, collect.sh's wrapper only: INFO
for bad in (None, "", "\n", "$ pairing_report\n[exit 0]\n"):
    r = tr.pairing_rows(bad)
    check(levels(r) == ["INFO"] and r[0][1] == "display pairing" and "16-ioaccel-pairing.txt" in r[0][2], "absent %r: %r" % (bad, r))


def archive(d, members):
    p = os.path.join(d, "rdna3-report-x.tar.gz")
    with tarfile.open(p, "w:gz") as t:
        for n, body in members.items():
            data = body.encode()
            ti = tarfile.TarInfo("rdna3-report/" + n)
            ti.size = len(data)
            t.addfile(ti, io.BytesIO(data))
    return p


# present_mode_lines carries the rows: paired, not paired, an archive without the file
with tempfile.TemporaryDirectory() as d:
    for body, want in ((pfile(fb(1)), "PASS"), (pfile(fb(1, types="")), "WARN"), (None, "INFO")):
        p = archive(d, {tr.PAIRING_MEMBER: body} if body is not None else {"00-system.txt": "x\n"})
        rows = [x for x in tr.present_mode_lines(p, "", "rdna3fb=1", host="") if x[1] == "display pairing"]
        check([x[0] for x in rows] == [want], "present_mode_lines %s: %r" % (want, rows))

# collect.sh packs the file with the member name the triage reads
col = open(os.path.join(ROOT, "tools", "macos", "collect.sh")).read()
check(re.search(r"^run %s pairing_report$" % re.escape(tr.PAIRING_MEMBER), col, re.M) is not None,
      "collect.sh does not run pairing_report into %s" % tr.PAIRING_MEMBER)

# pairing_report() against a fake ioreg: its output is what pairing_rows reads
FAKE = r'''#!/bin/sh
case "$*" in
*"-p IOService"*)
cat <<'EOT'
+-o Root  <class IORegistryEntry, id 0x100000100, retain 9>
  +-o MacBookPro16,1  <class IOPlatformExpertDevice, id 0x100000113, registered, matched, active, busy 0 (8439 ms), retain 52>
    +-o AppleACPIPlatformExpert  <class AppleACPIPlatformExpert, id 0x100000114, registered, matched, active, busy 0 (7125 ms), retain 46>
    | +-o IOPMrootDomain  <class IOPMrootDomain, id 0x100000119, registered, matched, active, busy 0 (1 ms), retain 110>
    | +-o PCI0  <class AppleACPIPCI, id 0x100000120, registered, matched, active, busy 0, retain 20>
    |   +-o AppleACPIPCI  <class AppleACPIPCI, id 0x100000121, registered, matched, active, busy 0, retain 20>
    |     +-o S80@10  <class IOPCIDevice, id 0x100000130, registered, matched, active, busy 0, retain 20>
    |       +-o RDNA3Accelerator  <class RDNA3Accelerator, id 0x100000150, registered, matched, active, busy 0, retain 10>
    +-o USB0  <class IOUSBHostDevice, id 0x100000160, registered, matched, active, busy 0, retain 3>
EOT
;;
*"-c RDNA3Framebuffer"*)
cat <<'EOT'
+-o RDNA3Framebuffer  <class AMDRDNA3Framebuffer, id 0x100000200, registered, matched, active, busy 0, retain 12>
  | {
  |   "IOFBIntegrated" = Yes
  |   "RDNA3,Connected" = Yes
  |   "IOAccelTypes" = "IOService:/AppleACPIPlatformExpert/PCI0/AppleACPIPCI/S80@10/RDNA3Accelerator"
  |   "IOAccelIndex" = 0
  |   "IOAccelRevision" = 2
  | }
  | 
+-o RDNA3Framebuffer  <class AMDRDNA3Framebuffer, id 0x100000201, registered, matched, active, busy 0, retain 12>
  | {
  |   "RDNA3,Connected" = Yes
  | }
  | 
EOT
;;
esac
'''
with tempfile.TemporaryDirectory() as d:
    with open(os.path.join(d, "ioreg"), "w") as f:
        f.write(FAKE)
    os.chmod(os.path.join(d, "ioreg"), 0o755)
    fn = re.search(r"^pairing_report\(\) \{.*?^\}$", col, re.M | re.S)
    check(fn is not None, "collect.sh has no pairing_report()")
    if fn:
        env = dict(os.environ, PATH=d + os.pathsep + os.environ["PATH"])
        res = subprocess.run(["sh", "-c", fn.group(0) + "\npairing_report"], env=env, capture_output=True, text=True)
        out = res.stdout
        check(res.returncode == 0, "pairing_report exit %d: %s" % (res.returncode, res.stderr))
        check("accelerator: IOService:/AppleACPIPlatformExpert/PCI0/AppleACPIPCI/S80@10/RDNA3Accelerator\n" in out,
              "accelerator path: %r" % out)
        check("MacBookPro16,1" not in out, "the platform expert device is left out of the path (as the kernel's getPath): %r" % out)
        check("IOFBIntegrated" not in out, "only the pairing keys are printed: %r" % out)
        rows = tr.pairing_rows("$ pairing_report\n" + out + "[exit 0]\n")
        check("RDNA3,Head" not in out and "IOFBIntegrated" not in out, "only the pairing keys are printed: %r" % out)
        check(levels(rows) == ["PASS", "WARN"], "fake ioreg through the triage (head 1 paired, head 2 without keys): %r\n%s" % (rows, out))
        check("IOAccelTypes is not set" in rows[1][2] if len(rows) > 1 else False, "head 2 WARN text: %r" % rows)

if fails:
    for f in fails:
        print("test_triage_pairing: FAIL " + f)
    sys.exit(1)
print("test_triage_pairing: ok")
