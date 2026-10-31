#!/usr/bin/env python3
"""tools/triage_reports.py gen_verdict on the write generations' counters as the accelerator's sysctls print them
(29-surface-vram.txt / the tests file): bare metal 2026-10-31 (N65), WindowServer without the Metal plugin - the holds
by reason (swaps_genhold_{same,older,unwritten}_h<N>), the presents by the CPU rule (swaps_gencpu_h<N>) and the
swap_gen_cpu / swap_gen_wsmetal state; missing or inconsistent counters are WARNs (ECC review of 48fe3ad)."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from triage_reports import a3_counters, gen_verdict  # noqa: E402

FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("FAIL: " + what)


def warns(rows):
    return [t for lv, t in rows if lv == "WARN"]


# a consistent head 0: 5 holds = 3 same + 0 older + 2 never written; 790 of the 1175 presents by the CPU rule
BASE = ("debug.rdna3accel_swaps_h0: 1180\ndebug.rdna3accel_swap_mode: 0\ndebug.rdna3accel_swap_gen: 1\n"
        "debug.rdna3accel_swaps_genpresent_h0: 1175\ndebug.rdna3accel_swaps_genhold_h0: 5\n"
        "debug.rdna3accel_swaps_gen_judged_h0: 1180\ndebug.rdna3accel_swaps_gencontent_h0: 8\n")
GLOB = "debug.rdna3accel_swap_gen_cpu: 1\ndebug.rdna3accel_swap_gen_wsmetal: 0\n"
HEAD0 = ("debug.rdna3accel_swaps_genhold_same_h0: 3\ndebug.rdna3accel_swaps_genhold_older_h0: 0\n"
         "debug.rdna3accel_swaps_genhold_unwritten_h0: 2\ndebug.rdna3accel_swaps_gencpu_h0: 790\n")
NEW = GLOB + HEAD0

c = a3_counters(BASE + NEW)
check(c.get("swap_gen_cpu") == 1 and c.get("swap_gen_wsmetal") == 0 and c.get("swap_gen") == 1, "globals parsed: %r" % c)
check(c.get("swaps_genhold_same_h0") == 3 and c.get("swaps_gencpu_h0") == 790 and c.get("swaps_genhold_h0") == 5,
      "per-head counters parsed apart from swaps_genhold_h0: %r" % c)
rows = gen_verdict(c)
txt = [t for _l, t in rows]
check(any("WindowServer has NO Metal device" in t and "SAME / OLDER backs present" in t for t in txt), "no-Metal row: %r" % txt)
check(any(t.startswith("head 0: write-generation holds by reason: 3 the panel's buffer (same), 0 older, 2 never written; "
                       "790 SAME / OLDER presented") for t in txt), "split row: %r" % txt)
check(not any("N65" in t or "do not add up" in t or "exceeds" in t for t in warns(rows)), "consistent: no N65 WARN %r" % rows)
# the A/B switch off without Metal: a WARN (small updates may stay off the panel)
rows = gen_verdict(a3_counters(BASE + NEW.replace("swap_gen_cpu: 1", "swap_gen_cpu: 0")))
check(any("HELD as before" in t for t in warns(rows)), "switch off: %r" % rows)
# with a Metal WindowServer the rule is the old one: INFO
rows = gen_verdict(a3_counters(BASE + NEW.replace("wsmetal: 0", "wsmetal: 1")))
check(any(lv == "INFO" and "HAS a Metal device" in t and "the old holds apply" in t for lv, t in rows), "Metal: %r" % rows)
# an older kext without them: no new rows, no WARN
rows = gen_verdict(a3_counters(BASE))
check(not any("without Metal" in t or "holds by reason" in t or "N65" in t for _l, t in rows), "old kext: %r" % rows)
# T1: a current kext with none of the new keys -> WARN naming them
rows = gen_verdict(a3_counters(BASE), current=True)
check(any("swap_gen_cpu, swap_gen_wsmetal" in t and "swaps_gencpu_h<N>" in t for t in warns(rows)), "current, none: %r" % rows)
# wsmetal present, cpu absent -> WARN
rows = gen_verdict(a3_counters(BASE + HEAD0 + "debug.rdna3accel_swap_gen_wsmetal: 0\n"))
check(any("counter missing: swap_gen_cpu" in t for t in warns(rows)) and
      any("swap_gen_cpu ?" in t for _l, t in rows), "cpu absent: %r" % rows)
# wsmetal absent with the others present -> WARN
rows = gen_verdict(a3_counters(BASE + HEAD0 + "debug.rdna3accel_swap_gen_cpu: 1\n"))
check(any("counter missing: swap_gen_wsmetal" in t and "NOT known" in t for t in warns(rows)), "wsmetal absent: %r" % rows)
# the holds by reason do not add up to genhold -> WARN (the 805-vs-5 shape)
rows = gen_verdict(a3_counters(BASE.replace("genhold_h0: 5", "genhold_h0: 805") + NEW))
check(any("holds by reason (5) do not add up to swaps_genhold (805)" in t for t in warns(rows)), "split sum: %r" % rows)
# gencpu > genpresent -> WARN
rows = gen_verdict(a3_counters(BASE + NEW.replace("gencpu_h0: 790", "gencpu_h0: 1500")))
check(any("swaps_gencpu 1500 exceeds the presents" in t for t in warns(rows)), "gencpu > present: %r" % rows)
# a partial per-head split -> WARN naming the missing one
rows = gen_verdict(a3_counters(BASE + GLOB + HEAD0.replace("debug.rdna3accel_swaps_genhold_older_h0: 0\n", "")))
check(any("head 0: the N65 counters are partial: swaps_genhold_older_h0 missing" in t for t in warns(rows)), "partial: %r" % rows)
# head 1 has its own rows
H1 = ("debug.rdna3accel_swaps_h1: 40\ndebug.rdna3accel_swaps_genpresent_h1: 30\ndebug.rdna3accel_swaps_genhold_h1: 10\n"
      "debug.rdna3accel_swaps_gen_judged_h1: 40\ndebug.rdna3accel_swaps_genhold_same_h1: 6\n"
      "debug.rdna3accel_swaps_genhold_older_h1: 3\ndebug.rdna3accel_swaps_genhold_unwritten_h1: 1\n"
      "debug.rdna3accel_swaps_gencpu_h1: 20\n")
rows = gen_verdict(a3_counters(BASE + NEW + H1))
check(any(t.startswith("head 1: write-generation holds by reason: 6 the panel's buffer (same), 3 older, 1 never written; "
                       "20 SAME") for _l, t in rows), "head 1 row: %r" % rows)
check(not any(t.startswith("head 1") and ("N65 counters are partial" in t or "add up" in t) for t in warns(rows)), "head 1 consistent: %r" % warns(rows))
# the log shows WindowServer's plugin but the kernel saw no mark -> WARN
LOG = ("2026-10-31 13:42:15.250360+0100 0x3ca Default 0x0 194 0 WindowServer: (RDNA3MTLDriver) NVMTL: pipeline: rt "
       "slot 0/1 cfmt 58\n")
rows = gen_verdict(a3_counters(BASE + NEW), ws_log=LOG)
check(any("swap_gen_wsmetal 0 but the log shows WindowServer's Metal plugin" in t for t in warns(rows)), "log Metal: %r" % rows)
rows = gen_verdict(a3_counters(BASE + NEW), ws_log="RDNA3Accelerator: something\n")
check(not any("the log shows" in t for _l, t in rows), "no plugin line: no WARN")

if FAILS:
    sys.exit("test_triage_write_gen: %d FAILED" % len(FAILS))
print("test_triage_write_gen: ok")
