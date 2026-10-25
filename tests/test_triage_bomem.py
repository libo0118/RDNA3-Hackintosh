#!/usr/bin/env python3
"""tools/triage_reports.py's bomem_rows: the kernel log line of
RDNA3BoMemory::doMap / doUnmap (kext/src/RDNA3Drm.cpp, DRIVER-PLAN N47 (b))
as a FAIL row. The line is built from the kext's own LOG format string, so a
change there that triage no longer reads fails here."""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import triage_reports as tr  # noqa: E402

fails = []
src = open(os.path.join(ROOT, "kext", "src", "RDNA3Drm.cpp")).read()
m = re.search(r'LOG\(("GEM: RDNA3BoMemory::%s called.*?)\s*,\s*what', src, re.S)
if not m:
    fails.append("no LOG format 'GEM: RDNA3BoMemory::%s called' in kext/src/RDNA3Drm.cpp")
    fmt = ""
else:
    fmt = "RDNA3: " + "".join(re.findall(r'"((?:\\.|[^"\\])*)"', m.group(1)))


def line(what, n):
    f = re.sub(r"%(llx|llu|x|d|s)", "{}", fmt)
    return "kernel: " + f.format(what, n, "0x100000", 64, "0x4", "0x10000")


r = tr.bomem_rows("\n".join(["kernel: RDNA3: something else", line("doMap", 1), line("doMap", 8),
                             line("doUnmap", 1)]))
if [(x[0], x[2].split(" called")[0]) for x in r] != [("FAIL", "RDNA3BoMemory::doMap"),
                                                     ("FAIL", "RDNA3BoMemory::doUnmap")]:
    fails.append("calls: %r" % (r,))
elif "at least 8" not in r[0][2]:
    fails.append("the highest count: %r" % (r,))
if tr.bomem_rows("kernel: RDNA3: something else") or tr.bomem_rows("") or tr.bomem_rows(None):
    fails.append("no call: rows")
tsrc = open(os.path.join(ROOT, "tools", "triage_reports.py")).read()
if not re.search(r"klog = kernel_text\(p, d\)\n(.*\n){0,8}.*bomem_rows\(klog\)", tsrc):
    fails.append("bomem_rows is not fed kernel_text(p, d)")

if fails:
    for f in fails:
        print("test_triage_bomem: FAIL " + f)
    sys.exit(1)
print("test_triage_bomem: ok")
