#!/usr/bin/env python3
"""tools/triage_coherence.py (DRIVER-PLAN N54) on made-up 42-coherence.txt dumps: an old core, a dump that
is missing while the kernel has N54, a clean run, a skew, a stale copy, a failed self-test, switches on."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from triage_coherence import coh_rows, coh_counters  # noqa: E402

FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("FAIL: " + what)


def dump(**kw):
    base = dict(flags="0x0", on="none", selftest="gfx:1,comp:1,sdma:1", jobs=0, checks=500, sys_held=0, skews=0,
                skews_closed=0, skew_sum_us=0, skew_max_us=0, stale=0, skew_spins=0, uc_bo=0, uc_switch=0,
                vmflush_forced=0, kcopy=10, kcopy_gcr=0, wait_waits=0, wait_timeouts=0, wait_us=0, wait_max_us=0,
                changes=0)
    for b in ("fence_sys", "fence_inv", "pipe_sync", "wait_idle", "uc", "hdp_end", "vm_flush", "sdma_gcr"):
        base["jobs_" + b] = 0
    base.update(kw)
    return "== sysctl debug.rdna3_coh (the end of the run)\ndebug.rdna3_coh: " + " ".join(
        "%s=%s" % (k, v) for k, v in base.items()) + "\n"


def levels(rows, name):
    return [lv for lv, n, _ in rows if n == name]


def text(rows, name):
    return " ".join(t for _, n, t in rows if n == name)


# an old core: INFO, nothing else
r = coh_rows("(no debug.rdna3_coh sysctl: a core older than DRIVER-PLAN N54, or not loaded)\n", "")
check(levels(r, "coherence") == ["INFO"] and len(r) == 1, "old core: one INFO row")
# the kernel has it, the dump is missing: WARN, and a failed self-test line still FAILs
log = "RDNA3: coherence: switches none (mask 0x0; ...)\nRDNA3: coherence: gfx ring: the fence's system copy did NOT land (seq 3)\n"
r = coh_rows("", log)
check(levels(r, "coherence") == ["WARN"] and "FAIL" in levels(r, "fence copy"), "missing dump: WARN + FAIL from the log")
# clean
c = coh_counters(dump())
check(c["checks"] == 500 and c["flags"] == 0 and c["selftest"] == "gfx:1,comp:1,sdma:1", "parse")
r = coh_rows(dump(), "")
check(levels(r, "fence copy") == ["PASS"] and levels(r, "fence skew") == ["PASS"], "clean: PASS rows")
check(levels(r, "coherence switches") == ["INFO"] and not any(lv == "FAIL" for lv, _, _ in r), "clean: no FAIL")
# a skew with the VRAM answer: WARN naming the switch
r = coh_rows(dump(skews=5, skews_closed=4, skew_sum_us=40, skew_max_us=30), "")
check(levels(r, "fence skew") == ["WARN"] and "debug.rdna3_coh_fence_sys=1" in text(r, "fence skew") and
      "mean 10.0 us" in text(r, "fence skew"), "skew: WARN, mean, the switch")
# fence_sys on with skews: says it was on
r = coh_rows(dump(flags="0x1", jobs=9, jobs_fence_sys=9, skews=1, skews_closed=1, skew_sum_us=3, sys_held=2,
                  skew_spins=7), "")
check("it WAS on" in text(r, "fence skew") and levels(r, "coherence switches") == ["WARN"] and
      "fence_sys (9 jobs)" in text(r, "coherence switches") and "SYSTEM" in text(r, "fence copy"), "fence_sys on")
# stale: FAIL
r = coh_rows(dump(stale=1, skews=1), "")
check("FAIL" in levels(r, "fence skew"), "stale: FAIL")
# a failed self-test: FAIL; no page: WARN
r = coh_rows(dump(selftest="gfx:-1,comp:1,sdma:1"), "")
check(levels(r, "fence copy") == ["FAIL"] and "gfx" in text(r, "fence copy"), "self-test failed: FAIL")
r = coh_rows(dump(selftest="gfx:-2,comp:-2,sdma:-2"), "")
check(levels(r, "fence copy") == ["WARN"], "no GART: WARN")
# switches during the run, off at the end; wait_idle timeouts; uc maps; sdma gcr
r = coh_rows(dump(jobs=50, jobs_pipe_sync=50, jobs_wait_idle=20, wait_waits=20, wait_timeouts=1, wait_us=900,
                  wait_max_us=200000, uc_bo=2, uc_switch=30, kcopy_gcr=4, changes=2),
             "RDNA3: coherence: debug.rdna3_coh_pipe_sync=1: switches now pipe_sync (mask 0x4, was 0x0)\n")
check(levels(r, "coherence switches") == ["INFO"] and "pipe_sync 50 jobs" in text(r, "coherence switches"), "during the run")
check(levels(r, "wait idle") == ["WARN"] and levels(r, "uncached maps") == ["INFO"] and levels(r, "sdma gcr") == ["INFO"],
      "wait idle / uc / gcr rows")
check("pipe_sync=1" in text(r, "coherence changes"), "the change line")

# the variant-4 ladder (autoreport.sh LADDER_COH_RUNGS): switches read back, this rung's jobs, the conclusion
from triage_coherence import coh_ladder_rows, LADDER_COH_RUNGS  # noqa: E402
from triage_glyphs import parse_ladder  # noqa: E402


def ladder(rungs):
    out = ["# rdna3 ladder v1", "variant: 4 (ladder-coh)"]
    for name, sets, jobs in rungs:
        out += ["== rung " + name, "scope: apps", "looks: ladder-%sa ladder-%sh ladder-%sh2" % (name, name, name), "answer: -"]
        for n, v, rb in sets:
            out.append("sysctl: %s=%s (was 0, read back %s)" % (n, v, rb))
        if sets:
            out.append("coh_start: flags=0x4 jobs=100 jobs_pipe_sync=100 jobs_fence_sys=3")
            out.append("coh_end: flags=0x4 jobs=%d jobs_pipe_sync=%d jobs_fence_sys=3" % (100 + jobs, 100 + jobs))
        out.append("== end rung " + name)
    out.append("== end")
    return "\n".join(out) + "\n"


lt = ladder([("baseline", [], 0), ("coh-all", [("debug.rdna3_coh_all", "1", "1")], 50),
             ("pipe-sync", [("debug.rdna3_coh_pipe_sync", "1", "1")], 40),
             ("uc", [("debug.rdna3_coh_uc", "1", "0")], 0)])
lv, lr, lp = parse_ladder(lt)
check(lv == 4 and set(lr) == {"baseline", "coh-all", "pipe-sync", "uc"}, "parse_ladder reads variant 4")
gl = {"baseline": {"verdict": "broken", "text": "3 broken stacks"}, "coh-all": {"verdict": "clean", "text": ""},
      "pipe-sync": {"verdict": "clean", "text": ""}, "uc": {"verdict": "broken", "text": ""}}
r = coh_ladder_rows(lt, lr, lp, gl)
check("40 job(s) ran under it" in text(r, "ladder-coh pipe-sync") and levels(r, "ladder-coh pipe-sync") == ["INFO"], "pipe-sync rung: this rung's jobs")
check(levels(r, "ladder-coh uc") == ["WARN"] and "NOT on" in text(r, "ladder-coh uc"), "uc read back 0: not engaged")
check(levels(r, "ladder-coh verdict") == ["FAIL"] and "pipe-sync" in text(r, "ladder-coh verdict"), "conclusion names the switch")
check(levels(r, "ladder-coh fence-sys") == ["WARN"] and "did not run" in text(r, "ladder-coh fence-sys"), "a rung that did not run")
gl["pipe-sync"]["verdict"] = "broken"
gl["coh-all"]["verdict"] = "broken"
r = coh_ladder_rows(lt, lr, lp, gl)
check(levels(r, "ladder-coh verdict") == ["WARN"] and "NOT the cause" in text(r, "ladder-coh verdict"), "all on and still broken")
check(LADDER_COH_RUNGS[0] == "baseline" and LADDER_COH_RUNGS[1] == "coh-all", "rung order")
ar = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "vm", "macos", "autoreport.sh")).read()
import re as _re  # noqa: E402
m = _re.search(r'^LADDER_COH_RUNGS="([^"]*)"$', ar, _re.M)
check(m is not None and tuple(m.group(1).split()) == LADDER_COH_RUNGS, "autoreport.sh's LADDER_COH_RUNGS == triage_coherence's")
for rung, sysname in [(k, v) for k, v in __import__("triage_coherence").LADDER_COH_SWITCH.items()]:
    check(("%s) echo \"sysctl:%s=1\" ;;" % (rung, sysname)) in ar, "autoreport.sh sets %s for %s" % (sysname, rung))

if FAILS:
    print("test_triage_coherence: %d FAILED" % len(FAILS))
    sys.exit(1)
print("test_triage_coherence: all passed")
