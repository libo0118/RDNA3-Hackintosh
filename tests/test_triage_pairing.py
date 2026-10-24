"""DRIVER-PLAN N46 (1): tools/triage_reports.py's "display pairing" row and collect.sh's pairing_report()
(16-ioaccel-pairing.txt): the IOAccelTypes / IOAccelIndex / IOAccelRevision WindowServer's
IOAccelFindAccelerator(fb) reads from each IOFramebuffer.
 - every lit head's framebuffer naming RDNA3Accelerator with IOAccelRevision 2 and an IOAccelIndex: PASS
   (the leaf class and the older RDNA3Framebuffer class alike);
 - IOAccelTypes / IOAccelRevision / IOAccelIndex missing, another accelerator named, another revision: WARN;
 - a head with RDNA3,Connected = No is not judged; none connected: INFO; no framebuffer at all: WARN;
 - no 16-ioaccel-pairing.txt (an older report), an empty one or collect.sh's wrapper only: INFO;
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
    return ("+-o RDNA3Framebuffer  <class %s, id 0x10000%04d, registered, matched, active, busy 0, retain 12>\n"
            '"RDNA3,Head" = %d\n"RDNA3,Connected" = %s\n' % (cls, head, head, connected)) + types + index + revision


def pfile(*blocks, accel=ACCEL):
    return "$ pairing_report\n== framebuffers (x)\n" + "".join(blocks) + "== accelerators (x)\n" + accel + "[exit 0]\n"


# both heads paired: PASS per framebuffer, nothing else
r = tr.pairing_rows(pfile(fb(1), fb(2)))
check(levels(r) == ["PASS", "PASS"] and all(x[1] == "display pairing" for x in r), "two paired heads: %r" % r)
check("IOAccelRevision 2" in r[0][2] and "head 1" in r[0][2] and "AMDRDNA3Framebuffer" in r[0][2], "PASS text: %r" % (r[0],))

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
  +-o MacPro  <class IOPlatformExpertDevice, id 0x100000113, registered, matched, active, busy 0 (8328 ms), retain 52>
    +-o PCI0  <class IOPCIBridge, id 0x100000120, registered, matched, active, busy 0, retain 20>
    | +-o GFX0@0  <class IOPCIDevice, id 0x100000130, registered, matched, active, busy 0, retain 20>
    |   +-o RDNA3Accel  <class RDNA3Accel, id 0x100000140, registered, matched, active, busy 0, retain 20>
    |     +-o RDNA3Accelerator  <class RDNA3Accelerator, id 0x100000150, registered, matched, active, busy 0, retain 10>
    +-o USB0  <class IOUSBHostDevice, id 0x100000160, registered, matched, active, busy 0, retain 3>
EOT
;;
*"-c RDNA3Framebuffer"*)
cat <<'EOT'
+-o RDNA3Framebuffer  <class AMDRDNA3Framebuffer, id 0x100000200, registered, matched, active, busy 0, retain 12>
  | {
  |   "IOFBIntegrated" = Yes
  |   "RDNA3,Head" = 1
  |   "RDNA3,Connected" = Yes
  |   "IOAccelTypes" = "IOService:/MacPro/PCI0/GFX0@0/RDNA3Accel/RDNA3Accelerator"
  |   "IOAccelIndex" = 0
  |   "IOAccelRevision" = 2
  | }
  | 
+-o RDNA3Framebuffer  <class AMDRDNA3Framebuffer, id 0x100000201, registered, matched, active, busy 0, retain 12>
  | {
  |   "RDNA3,Head" = 2
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
        check("accelerator: IOService:/MacPro/PCI0/GFX0@0/RDNA3Accel/RDNA3Accelerator\n" in out,
              "accelerator path: %r" % out)
        check("IOFBIntegrated" not in out, "only the pairing keys are printed: %r" % out)
        rows = tr.pairing_rows("$ pairing_report\n" + out + "[exit 0]\n")
        check(levels(rows) == ["PASS", "WARN"], "fake ioreg through the triage (head 1 paired, head 2 without keys): %r\n%s" % (rows, out))
        check("IOAccelTypes is not set" in rows[1][2] if len(rows) > 1 else False, "head 2 WARN text: %r" % rows)

if fails:
    for f in fails:
        print("test_triage_pairing: FAIL " + f)
    sys.exit(1)
print("test_triage_pairing: ok")
