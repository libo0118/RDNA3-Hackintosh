#!/usr/bin/env python3
"""tools/triage_uclk_pin.py (DRIVER-PLAN N74) on RDNA3,DCNPower values exactly as RDNA3DisplayPower.cpp
displayUclkPinUpdate writes them: pinned with two heads (INFO), released with one (INFO), two heads switching (WARN),
switched off (INFO), the PMFW refusing the minimum (FAIL), unavailable (WARN), a two-head report without the property
(WARN), the rdna3mall path (INFO)."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from triage_uclk_pin import uclk_pin_rows  # noqa: E402

FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("FAIL: " + what)


def level(props, log=""):
    rows = uclk_pin_rows(props, log)
    return rows[0][0] if rows else None


PINNED = ("UCLK pinned (two heads, unsynchronised, no FPO / SubVP: no shared vblank) at 1218 MHz: hard min sent now "
          "(init, decision 1, pins 1, releases 0, fails 0)")
ONE = ("UCLK switching (one head: its vblank hides the switch; 1 head lit): no hard min sent, the PMFW's own (init, "
       "decision 1, pins 0, releases 0, fails 0)")
RELEASED = ("UCLK switching (one head: its vblank hides the switch; 1 head lit): hard min released to the lowest level "
            "now (mirror off, decision 4, pins 1, releases 1, fails 0)")
TWO_SWITCHING = ("UCLK switching (the heads' timings synchronised: one shared vblank; two heads lit): no hard min sent, "
                 "the PMFW's own (init, decision 1, pins 0, releases 0, fails 0)")
OFF = ("UCLK switching (switched off (rdna3uclkpin=0 / debug.rdna3_uclk_pin 0); two heads lit): hard min released to "
       "the lowest level now (sysctl, decision 2, pins 1, releases 1, fails 0)")
FAILED = ("UCLK pin FAILED (mode set): SetHardMinByFreq(UCLK, 1218 MHz) for two heads (unsynchronised, no FPO / SubVP: "
          "no shared vblank) -> -5, tried again at the next update (decision 2, pins 0, releases 0, fails 1)")
UNAVAIL = ("UCLK pin (N74) unavailable (init): the PMFW's DAL mailbox did not answer at the bring-up (-62); two heads "
           "lit, UCLK left to the PMFW")
MALL = ("mode set: state 0, UCLK switching at 96 MHz (vactive), FCLK switching, DCFCLK 506 MHz, deep sleep 10 MHz, WM A "
        "urgent 19 us p-state 419 us, MALL 0 ways, SubVP 0, update 1: 0")
LOG = ("2026-11-02 10:00:00.000 Df kernel[0:1f] RDNA3: display power: " + PINNED + "\n")

check(level({"DCNPower": PINNED, "DisplayHeads": 2}, LOG) == "INFO", "pinned with two heads is INFO")
check("1 decision line" in uclk_pin_rows({"DCNPower": PINNED}, LOG)[0][2], "the kernel log's decision lines counted")
check(level({"DCNPower": ONE, "DisplayHeads": 1}) == "INFO", "one head switching is INFO")
check(level({"DCNPower": RELEASED}) == "INFO", "a release with one head is INFO")
check(level({"DCNPower": TWO_SWITCHING, "DisplayHeads": 2}) == "WARN", "two heads switching is a WARN")
check(level({"DCNPower": OFF, "DisplayHeads": 2}) == "INFO", "switched off is INFO (the A/B)")
check(level({"DCNPower": FAILED}) == "FAIL", "a refused minimum is a FAIL")
check(level({"DCNPower": UNAVAIL}) == "WARN", "unavailable is a WARN")
check(level({"DisplayHeads": 2}) == "WARN", "two heads without RDNA3,DCNPower is a WARN")
check(level({"DisplayHeads": 1, "ExtendHead": 1}) == "WARN", "the extended clone head counts as a second head")
check(level({"DisplayHeads": 1}) is None, "one head without the property: no row")
check(level({"DCNPower": MALL}) == "INFO", "the rdna3mall path is INFO")

if FAILS:
    print("test_triage_uclk_pin: %d check(s) failed" % len(FAILS))
    sys.exit(1)
print("test_triage_uclk_pin: all checks passed")
