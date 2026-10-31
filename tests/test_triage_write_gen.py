#!/usr/bin/env python3
"""tools/triage_reports.py gen_verdict on the write generations' counters as the accelerator's sysctls print them
(29-surface-vram.txt / the tests file): bare metal 2026-10-31, WindowServer without the Metal plugin - the holds by
reason (swaps_genhold_{same,older,unwritten}_h<N>), the presents by the CPU rule (swaps_gencpu_h<N>) and the
swap_gen_cpu / swap_gen_wsmetal state."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from triage_reports import a3_counters, gen_verdict  # noqa: E402

FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("FAIL: " + what)


BASE = ("debug.rdna3accel_swaps_h0: 1175\ndebug.rdna3accel_swap_mode: 0\ndebug.rdna3accel_swap_gen: 1\n"
        "debug.rdna3accel_swaps_genpresent_h0: 1175\ndebug.rdna3accel_swaps_genhold_h0: 805\n"
        "debug.rdna3accel_swaps_gen_judged_h0: 1980\ndebug.rdna3accel_swaps_gencontent_h0: 8\n")
NEW = ("debug.rdna3accel_swap_gen_cpu: 1\ndebug.rdna3accel_swap_gen_wsmetal: 0\n"
       "debug.rdna3accel_swaps_genhold_same_h0: 3\ndebug.rdna3accel_swaps_genhold_older_h0: 0\n"
       "debug.rdna3accel_swaps_genhold_unwritten_h0: 2\ndebug.rdna3accel_swaps_gencpu_h0: 790\n")

c = a3_counters(BASE + NEW)
check(c.get("swap_gen_cpu") == 1 and c.get("swap_gen_wsmetal") == 0 and c.get("swap_gen") == 1, "globals parsed: %r" % c)
check(c.get("swaps_genhold_same_h0") == 3 and c.get("swaps_gencpu_h0") == 790 and c.get("swaps_genhold_h0") == 805,
      "per-head counters parsed apart from swaps_genhold_h0: %r" % c)
rows = gen_verdict(c)
txt = [t for _l, t in rows]
check(any("WindowServer has NO Metal device" in t and "SAME / OLDER backs present" in t for t in txt), "no-Metal row: %r" % txt)
check(any(t.startswith("head 0: write-generation holds by reason: 3 the panel's buffer (same), 0 older, 2 never written; "
                       "790 SAME / OLDER presented") for t in txt), "split row: %r" % txt)
# the A/B switch off without Metal: a WARN (small updates may stay off the panel)
rows = gen_verdict(a3_counters(BASE + NEW.replace("swap_gen_cpu: 1", "swap_gen_cpu: 0")))
check(any(lv == "WARN" and "HELD as before" in t for lv, t in rows), "switch off: %r" % rows)
# with a Metal WindowServer the rule is the old one: INFO
rows = gen_verdict(a3_counters(BASE + NEW.replace("wsmetal: 0", "wsmetal: 1")))
check(any(lv == "INFO" and "HAS a Metal device" in t and "the old holds apply" in t for lv, t in rows), "Metal: %r" % rows)
# an older kext without them: no new rows
rows = gen_verdict(a3_counters(BASE))
check(not any("without Metal" in t or "holds by reason" in t for _l, t in rows), "old kext: %r" % rows)

if FAILS:
    sys.exit("test_triage_write_gen: %d FAILED" % len(FAILS))
print("test_triage_write_gen: ok")
