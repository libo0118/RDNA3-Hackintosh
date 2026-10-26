#!/usr/bin/env python3
"""tools/triage_attr_ring.py (DRIVER-PLAN N57) on made-up kernel / plugin log lines exactly as the kext (RDNA3Accel::discardableNote,
RDNA3Bringup::discardableMapNote) and the plugin (nvmtl_vk.c nvmtl_radv_env) write them: amdgpu-like placement and fragments (PASS),
the default 4 KiB PTEs (FAIL), a ring in GTT (FAIL), an old kext (WARN), the attrring rung's switch; and the ladder rung's own check."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from triage_attr_ring import attr_ring_rows, parse  # noqa: E402
import triage_glyphs as tg  # noqa: E402

FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("FAIL: " + what)


K = "2026-10-26 12:00:00.000 Df  kernel[0:1f] RDNA3: "
CREATE = K + "GEM DISCARDABLE #1: 4224 KiB, alignment 2048 KiB, VRAM beyond BAR0 at 0x40200000 (64 KiB aligned yes, 2 MiB aligned yes)\n"
MAP_OK = K + ("GEM_VA DISCARDABLE #1: VA 0xc0000000 + 4224 KiB -> contiguous VRAM 0x40200000 (PA 0x8040200000), PTE fragment 5 (128 KiB, "
              "amdgpu_vm_pte_fragment): BIG_PAGE ok\n")
MAP_4K = K + ("GEM_VA DISCARDABLE #1: VA 0xc0000000 + 4224 KiB -> contiguous VRAM 0x40200000 (PA 0x8040200000), PTE fragment 0 (4 KiB, "
              "4 KiB PTEs, rdna3amdgpu=0): BIG_PAGE WRONG: fragment-below-64KiB\n")
MAP_SKEW = K + ("GEM_VA DISCARDABLE #2: VA 0xc0000000 + 4224 KiB -> contiguous VRAM 0x40203000 (PA 0x8040203000), PTE fragment 9 (2048 KiB, "
                "amdgpu_vm_pte_fragment): BIG_PAGE ok\n")   # the kernel's verdict is wrong here on purpose: the triage recomputes it
GTT_CREATE = K + ("GEM DISCARDABLE #3: 4224 KiB, alignment 2048 KiB, GTT, VRAM was asked for and refused - WRONG for the attribute ring: BIG_PAGE "
                  "translates it in 64 KiB pages, system pages are 4 KiB and scattered (DRIVER-PLAN N57)\n")
GTT_MAP = K + ("GEM_VA DISCARDABLE #3: VA 0xc0400000 + 4224 KiB -> GTT system pages (not contiguous VRAM), PTE fragment 0 (4 KiB, scattered): "
               "BIG_PAGE WRONG: system pages fragment-below-64KiB\n")
P = "2026-10-26 12:00:01.000 Df  %s[%d:1f] (RDNA3MTLDriver) NVMTL: "


def plug(proc, pid, val, knob):
    q = lambda v: "unset" if v is None else '"%s"' % v
    return P % (proc, pid) + "vk: RADV_ATTR_RING_BIG_PAGE=%s (NVMTL_ATTR_RING_BIG_PAGE=%s)\n" % (q(val), q(knob))


def lv(rows, name="attribute ring"):
    return [r[0] for r in rows if r[1] == name]


# parse: the numbers come back as written
c, m = parse(CREATE + MAP_4K + GTT_CREATE + GTT_MAP)
check(len(c) == 2 and c[0]["vram"] and c[0]["off"] == 0x40200000 and c[0]["kib"] == 4224 and not c[1]["vram"], "parse creates %r" % (c,))
check(len(m) == 2 and m[0]["frag"] == 0 and m[0]["pa"] == 0x8040200000 and m[0]["va"] == 0xc0000000 and not m[1]["vram"], "parse maps %r" % (m,))

# amdgpu-like: PASS
r = attr_ring_rows(CREATE + MAP_OK, plug("Terminal", 907, None, None))
check(lv(r) == ["PASS"], "amdgpu-like placement is PASS: %r" % (r,))
check(lv(r, "attr ring switch") == ["INFO"], "the default switch is INFO: %r" % (r,))

# the default path: 4 KiB PTEs -> FAIL, the reason named
r = attr_ring_rows(CREATE + MAP_4K)
check(lv(r) == ["FAIL"] and "fragment below 64 KiB" in r[0][2] and "4224 KiB align 2048 KiB in VRAM beyond BAR0" in r[0][2], "4 KiB PTEs are FAIL: %r" % (r,))

# VRAM 4 KiB aligned under a 64 KiB VA: FAIL, and the kernel's disagreeing verdict is called out
r = attr_ring_rows(CREATE + MAP_SKEW)
check("FAIL" in lv(r) and any("congruent" in t for _l, _n, t in r), "a VA/PA skew is FAIL: %r" % (r,))
check(any(l == "WARN" and "disagree" in t for l, _n, t in r), "a kernel verdict that differs is WARN: %r" % (r,))

# GTT: FAIL twice (the create and the map)
r = attr_ring_rows(GTT_CREATE + GTT_MAP)
check(lv(r).count("FAIL") == 2 and any("landed in GTT" in t for _l, _n, t in r) and any("system pages" in t for _l, _n, t in r), "GTT is FAIL: %r" % (r,))

# a create without a map: WARN
r = attr_ring_rows(CREATE)
check(lv(r) == ["WARN"] and "UNKNOWN" in r[0][2], "a create without a map is WARN: %r" % (r,))

# an old kext: WARN when the kernel log has something; nothing at all without a kernel log
check(lv(attr_ring_rows(K + "something else\n")) == ["WARN"], "no lines in a kernel log is WARN")
check(attr_ring_rows("", "") == [], "no kernel log, no plugin log: no row")

# the attrring rung: the switch row WARNs (not the default driver) and the FAIL row says the ring does not matter there
pl = plug("Terminal", 907, "0", "0") + plug("WindowServer", 90, None, None)
r = attr_ring_rows(CREATE + MAP_4K, pl)
sw = [x for x in r if x[1] == "attr ring switch"]
check(sw and sw[0][0] == "WARN" and "Terminal[907]" in sw[0][2] and "WindowServer[90]" in sw[0][2], "the switch row: %r" % (sw,))
check(any(x[0] == "FAIL" and "BIG_PAGE was OFF" in x[2] for x in r), "the FAIL row names the processes without BIG_PAGE: %r" % (r,))
r = attr_ring_rows("", P % ("Terminal", 907) + 'vk: NVMTL_ATTR_RING_BIG_PAGE="off" REJECTED: only 0 (BIG_PAGE off) or 1 (upstream) is taken - RADV_ATTR_RING_BIG_PAGE unchanged\n')
check(any(x[0] == "WARN" and "did not reach RADV" in x[2] for x in r), "a refused value is WARN: %r" % (r,))

# the ladder's attrring rung: the plugin took the knob (`vk: ab switches`) but RADV never got it -> WARN; with the RADV line -> INFO
sw_line = P % ("Terminal", 1001) + 'vk: ab switches [1]: NVMTL_ATTR_RING_BIG_PAGE="0"\n'
summ = P % ("Terminal", 1001) + "vk: ab: SUMMARY wit_ok=1\n" + P % ("Terminal", 1001) + "census: SUMMARY seq=1 surfaces=1 top=\n"
for with_radv, want in ((False, "WARN"), (True, "INFO")):
    log = sw_line + summ + (plug("Terminal", 1001, "0", "0") if with_radv else "")
    rungs = {"attrring": {"conf": ["NVMTL_ATTR_RING_BIG_PAGE=0"], "scope": "apps", "log": log, "looks": [], "verdict": None}}
    rows = [x for x in tg.ladder_rows(rungs, 1) if x[1] == "ladder attrring"]
    check(rows and rows[0][0] == want and (with_radv or "RADV_ATTR_RING_BIG_PAGE=0 is not logged" in rows[0][2]), "ladder attrring (radv line %s): %r" % (with_radv, rows))
# a clean attrring rung under a broken baseline names the attribute ring
rungs = {n: {"conf": [], "scope": "apps", "log": "", "looks": [], "verdict": None} for n in tg.LADDER_RUNGS}
gl = {n: {"verdict": "broken", "text": "BROKEN"} for n in tg.LADDER_RUNGS}
gl["attrring"] = {"verdict": "clean", "text": "CLEAN"}
check(any(x[1] == "ladder glyphs attrring" and "attribute ring" in x[2] for x in tg.ladder_rows(rungs, 1, (), gl)), "a clean attrring rung is named")
check(tg.LADDER_RUNGS[:2] == ("baseline", "attrring"), "attrring runs right after the baseline")

if FAILS:
    sys.exit(1)
print("ok: tools/triage_attr_ring.py")
