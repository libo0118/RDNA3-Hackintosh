#!/usr/bin/env python3
"""tools/triage_reports.py's tcall_rows: the kernel log lines of
common/rdna3_tcall.h (card run 2026-10-25b, DRIVER-PLAN N47) as triage rows.
The lines are built with the header's own format strings, so a change there
that triage no longer reads fails here."""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import triage_reports as tr  # noqa: E402

fails = []
hdr = open(os.path.join(ROOT, "common", "rdna3_tcall.h")).read()


def fmt(start):
    """the IOLog format string of rdna3_tcall.h that starts with 'start' (adjacent literals joined)"""
    m = re.search(r'IOLog\(("RDNA3-tcall: %s: ' % re.escape("%s") + re.escape(start) + r'.*?)\s*,\s*t->name', hdr, re.S)
    if not m:
        fails.append("no IOLog format starting %r in common/rdna3_tcall.h" % start)
        return ""
    return "".join(re.findall(r'"((?:\\.|[^"\\])*)"', m.group(1))).replace("\\n", "")


def line(f, *vals):
    return f.replace("%d", "{}").replace("%u", "{}").replace("%s", "{}").format(*vals)


slow = fmt("the stop waits")
gave = fmt("the stop GAVE UP")
selfr = fmt("stopped from inside")
drained = fmt("drained after")

klog = "\n".join([
    "kernel: noise RDNA3: something else",
    "kernel: " + line(slow, "core.displayWakeWork", 1, 2000, 1),
    "kernel: " + line(drained, "core.displayWakeWork", 2350),
    "kernel: " + line(gave, "fb.vbl", 30000, 1, 1),
    "kernel: " + line(selfr, "core.hotplugTick", 1),
])
rows = tr.tcall_rows(klog)
by = {(lv, t.split(":")[0]) for lv, _, t in rows}
if ("FAIL", "fb.vbl") not in by:
    fails.append("gave up is not a FAIL: %r" % rows)
if ("WARN", "core.displayWakeWork") not in by or not any("2350" in t for _, _, t in rows):
    fails.append("slow + drained not a WARN with the time: %r" % rows)
if ("WARN", "core.hotplugTick") not in by:
    fails.append("self stop not a WARN: %r" % rows)
if len(rows) != 3:
    fails.append("3 rows expected: %r" % rows)
if tr.tcall_rows("") != [] or tr.tcall_rows(None) != [] or tr.tcall_rows("kernel: " + line(drained, "x", 5)) != []:
    fails.append("no slow stop: no row")

if fails:
    for f in fails:
        print("test_triage_tcall: FAIL " + f)
    sys.exit(1)
print("test_triage_tcall: ok")
