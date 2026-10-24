"""DRIVER-PLAN N45 (2), (3): tools/triage_reports.py's "AGDC N45" row from the tests file's / collect.sh's
`sysctl debug | grep rdna3agdc` lines (agdc/rdna3_agdc_n48.h):
 - the switches, the per-type 0x711 text and the counters in one INFO row;
 - WARN: a boot-arg the sysctl does not show (an unknown value compares with the default), the inline buffer
   asked but never answered with ib bit 0, no zero fill with ib bit 1 while other types were asked, counters
   missing while K5 is armed, switches off their defaults with K5 off, an old RDNA3AGDC under K5 / N45 boot-args;
 - INFO and nothing else when K5 is not armed and no N45 counter exists;
 - agdc_vendor_lines carries the row (every triage path that prints the AGDC vendor answer).
Run: python3 -I -B tests/test_triage_agdc_n48.py"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import triage_reports as tr  # noqa: E402

fails = []


def check(cond, what):
    if not cond:
        fails.append(what)


K5 = "rdna3lvl=8 rdna3agdc=3 rdna3play=1"


def sysctls(ib=0, linkflags=0, scaler=0, types="0x10 2/2, 0x40 2/0, 0x2 2/0", ib_answered=0, zero_filled=0,
            drop=()):
    v = {"ib": ib, "linkflags": linkflags, "scaler": scaler, "ib_answered": ib_answered, "zero_filled": zero_filled,
         "scaler_alt": 0, "linkflags_last": 11, "types": types, "k5_refused": 0, "k5_why": 0, "k5": 1}
    return "".join("debug.rdna3agdc_%s: %s\n" % (k, x) for k, x in v.items() if k not in drop)


def row(txt, args):
    rows = tr.agdc_n48_lines(txt, args)
    check(len(rows) == 1 and rows[0][1] == "AGDC N45", "one AGDC N45 row: %r" % (rows,))
    return rows[0]


# the defaults under K5: INFO with every value
lv, _, t = row(sysctls(), K5)
check(lv == "INFO", "defaults: %s %s" % (lv, t))
for want in ("K5 armed", "ib 0, linkflags 0, scaler 0", "0x10 2/2, 0x40 2/0, 0x2 2/0", "last 0x921 flags 11"):
    check(want in t, "defaults: no %r in %s" % (want, t))

# play-agdcib: ib 3 answered and zero-filled
lv, _, t = row(sysctls(ib=3, types="0x10 2/2, 0x40 2/2, 0x2 2/2", ib_answered=2, zero_filled=2), K5 + " rdna3agdcib=3")
check(lv == "INFO", "ib 3 working: %s %s" % (lv, t))
# the boot-arg did not reach the kext
lv, _, t = row(sysctls(ib=0), K5 + " rdna3agdcib=3")
check(lv == "WARN" and "rdna3agdcib=3 but debug.rdna3agdc_ib is 0" in t, "boot-arg mismatch: %s %s" % (lv, t))
# an unknown boot-arg value: the kext takes the default, that is no mismatch but said
lv, _, t = row(sysctls(ib=0), K5 + " rdna3agdcib=7")
check(lv == "INFO", "unknown boot-arg value taken as the default: %s %s" % (lv, t))
lv, _, t = row(sysctls(scaler=1), K5 + " rdna3agdcscaler=9")
check(lv == "WARN" and "(unknown: the default 0)" in t, "unknown value vs a changed sysctl: %s %s" % (lv, t))
# 0x40 asked, never answered with bit 0
lv, _, t = row(sysctls(ib=1, types="0x10 2/2, 0x40 2/0"), K5 + " rdna3agdcib=1")
check(lv == "WARN" and "never answered" in t, "0x40 n/0: %s %s" % (lv, t))
# no zero fill with bit 1 while other types were asked
lv, _, t = row(sysctls(ib=2, types="0x10 2/2, 0x40 2/0, 0x2 2/0", zero_filled=0), K5 + " rdna3agdcib=2")
check(lv == "WARN" and "none was zero-filled" in t, "no zero fill: %s %s" % (lv, t))
# with bit 1 but only 0x10 / 0x40 / non-types asked: nothing to zero-fill
lv, _, t = row(sysctls(ib=2, types="0x10 2/2, 0x40 2/0, other 3/0"), K5 + " rdna3agdcib=2")
check(lv == "INFO", "only 0x10 / 0x40 / other asked: no zero-fill WARN: %s %s" % (lv, t))
# counters missing while K5 is armed
lv, _, t = row(sysctls(drop=("zero_filled", "types")), K5)
check(lv == "WARN" and "no debug.rdna3agdc_zero_filled, _types" in t, "missing counters: %s %s" % (lv, t))
# switches set, K5 off
txt = sysctls(ib=3).replace("debug.rdna3agdc_k5: 1", "debug.rdna3agdc_k5: 0")
lv, _, t = row(txt, "rdna3lvl=8 rdna3agdc=1 rdna3agdcib=3")
check(lv == "WARN" and "while K5 is off" in t and "K5 off" in t, "K5 off: %s %s" % (lv, t))
# an RDNA3AGDC older than N45 under K5, or N45 boot-args without counters
lv, _, t = row("debug.rdna3agdc_k5: 1\n", K5)
check(lv == "WARN" and "older than DRIVER-PLAN N45" in t, "old kext under K5: %s %s" % (lv, t))
lv, _, t = row("", "rdna3lvl=8 rdna3agdclinkflags=1")
check(lv == "WARN" and "rdna3agdclinkflags=1" in t, "boot-arg without counters: %s %s" % (lv, t))
# not armed, nothing there: INFO only
lv, _, t = row("", "rdna3lvl=8")
check(lv == "INFO" and "not armed" in t, "not armed: %s %s" % (lv, t))
# a different counter name does not count as the switch (debug.rdna3agdc_ib_answered is not _ib)
lv, _, t = row(sysctls(drop=("ib",)), K5)
check(lv == "WARN" and "no debug.rdna3agdc_ib" in t and "ib ?" in t, "ib missing: %s %s" % (lv, t))

# agdc_vendor_lines carries it (a path that is no archive: the tests file alone)
rows = tr.agdc_vendor_lines("/nonexistent/report.tar.gz", "boot-args: %s\n%s" % (K5, sysctls()), "")
check(any(r[1] == "AGDC N45" for r in rows), "agdc_vendor_lines has no AGDC N45 row: %r" % (rows,))

if fails:
    for f in fails:
        print("test_triage_agdc_n48: FAIL " + f)
    sys.exit(1)
print("test_triage_agdc_n48: all checks passed")
