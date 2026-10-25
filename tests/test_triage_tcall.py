#!/usr/bin/env python3
"""tools/triage_reports.py's tcall_rows: the kernel log lines of
common/rdna3_tcall.h (card run 2026-10-25b, DRIVER-PLAN N47; ECC review
round 1: the stop never gives up) as triage rows. The lines are built from
the header's own IOLog format strings, so a change there that triage no
longer reads fails here."""
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
    m = re.search(r'IOLog\(("RDNA3-tcall: %s: ' + re.escape(start) + r'.*?)\s*,\s*(t->)?name', hdr, re.S)
    if not m:
        fails.append("no IOLog format starting %r in common/rdna3_tcall.h" % start)
        return ""
    return "".join(re.findall(r'"((?:\\.|[^"\\])*)"', m.group(1))).replace("\\n", "")


def line(f, *vals):
    f = re.sub(r"%(llu|lu|u|d|s|x)", "{}", f.replace("0x%x", "0x{}"))
    return f.format(*vals)


def rows_for(*lines):
    return tr.tcall_rows("\n".join(["kernel: noise RDNA3: something else"] + ["kernel: " + x for x in lines]))


waited = fmt("the stop has waited")
drained = fmt("drained after")
selfr = fmt("stopped from inside")
selffree = fmt("FAIL: freed from inside")
refused = fmt("an enter during its stop")
alloc1 = fmt("thread call allocation failed (no memory")
alloc2 = fmt("thread call allocation failed (%s returned")
freer = fmt("thread_call_free refused")

# a slow stop that drained: WARN with the time, "at least"
r = rows_for(line(waited, "core.displayWakeWork", 2000, 1, "", 1), line(drained, "core.displayWakeWork", 2350))
if len(r) != 1 or r[0][0] != "WARN" or "2350" not in r[0][2] or "at least" not in r[0][2]:
    fails.append("slow + drained: %r" % (r,))
# a stop still waiting past 30 s when the log ends: FAIL (a hang)
r = rows_for(line(waited, "fb.vbl", 2000, 1, "", 1),
             line(waited, "fb.vbl", 32000, 1, ", still waiting (as thread_call_cancel_wait would)", 1))
if len(r) != 1 or r[0][0] != "FAIL" or "32 s" not in r[0][2] or "at least 2" not in r[0][2]:
    fails.append("hang: %r" % (r,))
# allocation failures, a self free, self stop, refused enters, thread_call_free refusing
r = rows_for(line(alloc1, "core.statsTick", 120), line(alloc2, "core.vcnIdleTick", "thread_call_allocate", 2, 0),
             line(selffree, "core.hotplugTick"), line(selfr, "core.hotplugTick", 1),
             line(refused, "fb.cursor", 1), line(freer, "fb.cursor", 1000))
lv = sorted((x[0], x[2].split(":")[0]) for x in r)
want = sorted([("FAIL", "core.statsTick"), ("FAIL", "core.vcnIdleTick"), ("FAIL", "core.hotplugTick"),
               ("WARN", "core.hotplugTick"), ("INFO", "fb.cursor"), ("WARN", "fb.cursor")])
if lv != want:
    fails.append("other lines: %r" % (r,))
# no slow stop: no row; no kernel log at all: one INFO row
if rows_for(line(drained, "x", 5)) != [] and rows_for() != []:
    fails.append("no slow stop: rows")
if rows_for() != []:
    fails.append("a log without tcall lines must give no row: %r" % (rows_for(),))
r = tr.tcall_rows("") + tr.tcall_rows(None)
if len(r) != 2 or any(x[0] != "INFO" or "no kernel log" not in x[2] for x in r):
    fails.append("no kernel log: %r" % (r,))
# triage reads the merged kernel text (archive + serial), as the other rows
src = open(os.path.join(ROOT, "tools", "triage_reports.py")).read()
if not re.search(r"klog = kernel_text\(p, d\)\n(.*\n){0,3}.*tcall_rows\(klog\)", src):
    fails.append("tcall_rows is not fed kernel_text(p, d)")

if fails:
    for f in fails:
        print("test_triage_tcall: FAIL " + f)
    sys.exit(1)
print("test_triage_tcall: ok")
