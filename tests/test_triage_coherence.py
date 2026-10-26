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
      "fence_sys (9 jobs)" in text(r, "coherence switches") and "END of the run: on" in text(r, "fence copy"), "fence_sys on")
# stale: FAIL
r = coh_rows(dump(stale=1, skews=1), "")
check("FAIL" in levels(r, "fence skew"), "stale: FAIL")
# a failed self-test: FAIL; no page: WARN
r = coh_rows(dump(selftest="gfx:-1,comp:1,sdma:1"), "")
check(levels(r, "fence copy") == ["FAIL"] and "gfx" in text(r, "fence copy"), "self-test failed: FAIL")
r = coh_rows(dump(selftest="gfx:-2,comp:-2,sdma:-2"), "")
check(levels(r, "fence copy") == ["WARN"], "no GART: WARN")
r = coh_rows(dump(selftest="gfx:1,comp:-3,sdma:0"), "")
check(levels(r, "fence copy") == ["WARN"] and "comp -3" in text(r, "fence copy"), "a ring not tested: WARN, never PASS")
r = coh_rows(dump(selftest="gfx:1,comp:1,sdma:-4"), "")
check(levels(r, "fence copy") == ["WARN"] and "did not run its self-test" in text(r, "fence copy"), "-4: WARN")
r = coh_rows(dump(checks=0), "")
check(levels(r, "fence copy") == ["WARN"] and "no fence check" in text(r, "fence copy"), "no checks: WARN, never PASS")
r = coh_rows(dump(work_pipe_sync=40, kwait_fallback=2, forced=1), "")
check(levels(r, "coherence work") == ["WARN"] and "pipe_sync 40" in text(r, "coherence work"), "work row, a fallback WARNs")
# switches during the run, off at the end; wait_idle timeouts; uc maps; sdma gcr
r = coh_rows(dump(jobs=50, jobs_pipe_sync=50, jobs_wait_idle=20, wait_waits=20, wait_timeouts=1, wait_us=900,
                  wait_max_us=200000, uc_bo=2, uc_switch=30, kcopy_gcr=4, changes=2),
             "RDNA3: coherence: debug.rdna3_coh_pipe_sync=1: switches now pipe_sync (mask 0x4, was 0x0)\n")
check(levels(r, "coherence switches") == ["INFO"] and "pipe_sync 50 jobs" in text(r, "coherence switches"), "during the run")
check(levels(r, "wait idle") == ["WARN"] and levels(r, "uncached maps") == ["INFO"] and levels(r, "sdma gcr") == ["INFO"],
      "wait idle / uc / gcr rows")
check("pipe_sync=1" in text(r, "coherence changes"), "the change line")

# the variant-4 ladder (autoreport.sh LADDER_COH_RUNGS): flags at the start, switches read back, the work each did in the rung, the conclusion
from triage_coherence import coh_ladder_rows, LADDER_COH_RUNGS, LADDER_COH_FLAGS  # noqa: E402
from triage_glyphs import parse_ladder  # noqa: E402

BITS = ("fence_sys", "fence_inv", "pipe_sync", "wait_idle", "uc", "hdp_end", "vm_flush", "sdma_gcr")


def state(flags, work):
    return "flags=0x%x selftest=gfx:1,comp:1,sdma:1 stale=0 %s" % (flags, " ".join("work_%s=%d" % (b, work.get(b, 0)) for b in BITS))


def ladder(rungs):
    """rungs: (name, [(sysctl, value, readback)], start flags or None, {bit: work over the rung} or None for no coh_end, extra lines)"""
    out = ["# rdna3 ladder v1", "variant: 4 (ladder-coh)"]
    for name, sets, flags, work, extra in rungs:
        out += ["== rung " + name, "scope: apps", "looks: ladder-%sa ladder-%sh ladder-%sh2" % (name, name, name), "answer: -"]
        for n, v, rb in sets:
            out.append("sysctl: %s=%s (was 0, read back %s)" % (n, v, rb))
        if flags is not None:
            out.append("coh_start: " + state(flags, dict((b, 100) for b in BITS)))
        if work is not None:
            out.append("coh_end: " + state(flags or 0, dict((b, 100 + work.get(b, 0)) for b in BITS)))
        out += extra
        out.append("== end rung " + name)
    out.append("== end")
    return "\n".join(out) + "\n"


ALLW = {"fence_sys": 5, "fence_inv": 9, "pipe_sync": 9, "hdp_end": 9}
lt = ladder([("baseline", [], 0, {}, []),
             ("coh-all", [("debug.rdna3_coh_all", "1", "1")], 0xff, ALLW, []),
             ("pipe-sync", [("debug.rdna3_coh_pipe_sync", "1", "1")], 4, {"pipe_sync": 40}, []),
             ("wait-idle", [("debug.rdna3_coh_wait_idle", "1", "1")], 8, {}, []),                 # on, did nothing
             ("uc", [("debug.rdna3_coh_uc", "1", "0")], 0x10, {"uc": 3}, []),                    # read back 0
             ("fence-inv", [("debug.rdna3_coh_fence_inv", "1", "1")], 0x2 | 0x4, {"fence_inv": 7},  # contaminated by pipe_sync
              ["WARN: sysctl debug.rdna3_coh_mask NOT restored (is 6, wanted 0): the rungs after this one run with it"]),
             ("hdp-end", [("debug.rdna3_coh_hdp_end", "1", "1")], 0x20, None, [])])             # no coh_end
lv, lr, lp = parse_ladder(lt)
check(lv == 4 and "pipe-sync" in lr and "hdp-end" in lr, "parse_ladder reads variant 4")
gl = dict((n, {"verdict": "broken", "text": ""}) for n in lr)
gl["pipe-sync"]["verdict"] = "clean"
gl["wait-idle"]["verdict"] = "clean"
gl["fence-inv"]["verdict"] = "clean"
gl["hdp-end"]["verdict"] = "clean"
r = coh_ladder_rows(lt, lr, lp, gl)
check(levels(r, "ladder-coh baseline") == ["INFO"], "baseline with flags 0: clean default")
check(levels(r, "ladder-coh pipe-sync") == ["INFO"] and "work_pipe_sync +40" in text(r, "ladder-coh pipe-sync"), "pipe-sync: the rung's work")
check(levels(r, "ladder-coh wait-idle") == ["WARN"] and "did NOTHING" in text(r, "ladder-coh wait-idle"), "on but no work: not engaged")
check(levels(r, "ladder-coh uc") == ["WARN"] and "NOT on" in text(r, "ladder-coh uc"), "uc read back 0: not engaged")
check(levels(r, "ladder-coh fence-inv") == ["WARN"] and "CONTAMINATED" in text(r, "ladder-coh fence-inv"), "a leaked switch: contaminated")
check("NOT restored" in text(r, "ladder-coh put-back") and levels(r, "ladder-coh put-back") == ["WARN"], "a failed put-back is a loud row")
check(levels(r, "ladder-coh hdp-end") == ["WARN"] and "NO debug.rdna3_coh at the rung's end" in text(r, "ladder-coh hdp-end"), "no coh_end: not engaged")
check(levels(r, "ladder-coh verdict") == ["FAIL"] and "pipe-sync" in text(r, "ladder-coh verdict") and
      "wait-idle" not in text(r, "ladder-coh verdict") and "fence-inv" not in text(r, "ladder-coh verdict") and
      "hdp-end" not in text(r, "ladder-coh verdict"), "the conclusion names only the engaged switch")
check(levels(r, "ladder-coh fence-sys") == ["WARN"] and "did not run" in text(r, "ladder-coh fence-sys"), "a rung that did not run")
gl["pipe-sync"]["verdict"] = "broken"
r = coh_ladder_rows(lt, lr, lp, gl)
check(levels(r, "ladder-coh verdict") == ["WARN"] and "NOT the cause" in text(r, "ladder-coh verdict"), "all on and still broken")
# a baseline started with a switch on (the boot-arg rdna3coh): no conclusion at all
lt2 = ladder([("baseline", [], 0x4, {}, []), ("pipe-sync", [("debug.rdna3_coh_pipe_sync", "1", "1")], 4, {"pipe_sync": 4}, [])])
lv2, lr2, lp2 = parse_ladder(lt2)
r = coh_ladder_rows(lt2, lr2, lp2, {"baseline": {"verdict": "broken"}, "pipe-sync": {"verdict": "clean"}})
check("CONTAMINATED" in text(r, "ladder-coh baseline") and "rdna3coh" in text(r, "ladder-coh baseline") and
      levels(r, "ladder-coh verdict") == ["WARN"], "a switch-on baseline: WARN, no verdict")
# a baseline without coh_start (an older autoreport): not a clean default either
lt3 = ladder([("baseline", [], None, None, [])])
lv3, lr3, lp3 = parse_ladder(lt3)
r = coh_ladder_rows(lt3, lr3, lp3, {"baseline": {"verdict": "broken"}})
check(levels(r, "ladder-coh baseline") == ["WARN"] and "NO debug.rdna3_coh" in text(r, "ladder-coh baseline"), "no coh_start: WARN")
check(LADDER_COH_FLAGS["baseline"] == 0 and LADDER_COH_FLAGS["coh-all"] == 0xff and LADDER_COH_FLAGS["uc"] == 0x10, "expected flags")
check(LADDER_COH_RUNGS[0] == "baseline" and LADDER_COH_RUNGS[1] == "coh-all", "rung order")
ar = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "vm", "macos", "autoreport.sh")).read()
import re as _re  # noqa: E402
m = _re.search(r'^LADDER_COH_RUNGS="([^"]*)"$', ar, _re.M)
check(m is not None and tuple(m.group(1).split()) == LADDER_COH_RUNGS, "autoreport.sh's LADDER_COH_RUNGS == triage_coherence's")
for rung, sysname in [(k, v) for k, v in __import__("triage_coherence").LADDER_COH_SWITCH.items()]:
    check(("%s) echo \"sysctl:%s=1\" ;;" % (rung, sysname)) in ar, "autoreport.sh sets %s for %s" % (sysname, rung))
check("ladder_coh_state start" in ar and "ladder_coh_state end" in ar and "NOT restored" in ar, "autoreport.sh records every rung's state and checks the put-back")

if FAILS:
    print("test_triage_coherence: %d FAILED" % len(FAILS))
    sys.exit(1)
print("test_triage_coherence: all passed")
