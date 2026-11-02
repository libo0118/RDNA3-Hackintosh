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


T0 = "decision 1, pins 1, releases 0, fails 0, unconfirmed 0"
PINNED = ("UCLK pinned (two heads, unsynchronised, no FPO / SubVP: no shared vblank) at 1218 MHz: hard min sent now "
          "(init, " + T0 + ")")
PINNED_AFTER_FAIL = PINNED.replace("fails 0", "fails 2")
PINNED_UNCONF = ("UCLK pinned (two heads, unsynchronised, no FPO / SubVP: no shared vblank) at 1218 MHz: hard min sent "
                 "now, accepted, NOT confirmed by ReturnHardMinStatus (init, decision 1, pins 1, releases 0, fails 0, "
                 "unconfirmed 1)")
GAVE_UP = ("UCLK pin GAVE UP (mode set): 3 refused / unanswered SetHardMinByFreq in a row (last -62): no DAL message "
           "until the next bring-up, UCLK as the PMFW has it (last minimum 0 MHz); two heads lit (decision 3, pins 0, "
           "releases 0, fails 3, unconfirmed 0)")
TIMED_OUT = ("UCLK pin FAILED (mode set): SetHardMinByFreq(UCLK, 1218 MHz) for two heads (unsynchronised, no FPO / "
             "SubVP: no shared vblank) -> -62 (no answer from the DAL mailbox), tried again at the next update, 1 in a "
             "row of 3 allowed (decision 1, pins 0, releases 0, fails 1, unconfirmed 0)")
ONE = ("UCLK switching (one head: its vblank hides the switch; 1 head lit): no hard min sent, the PMFW's own (init, "
       "decision 1, pins 0, releases 0, fails 0)")
RELEASED = ("UCLK switching (one head: its vblank hides the switch; 1 head lit): hard min released to the lowest level "
            "now (mirror off, decision 4, pins 1, releases 1, fails 0)")
TWO_SWITCHING = ("UCLK switching (the heads' timings synchronised: one shared vblank; two heads lit): no hard min sent, "
                 "the PMFW's own (init, decision 1, pins 0, releases 0, fails 0)")
OFF = ("UCLK switching (switched off (rdna3uclkpin=0 / debug.rdna3_uclk_pin 0); two heads lit): hard min released to "
       "the lowest level now (sysctl, decision 2, pins 1, releases 1, fails 0)")
FAILED = ("UCLK pin FAILED (mode set): SetHardMinByFreq(UCLK, 1218 MHz) for two heads (unsynchronised, no FPO / SubVP: "
          "no shared vblank) -> -5 (refused by the PMFW), tried again at the next update, 1 in a row of 3 allowed "
          "(decision 2, pins 0, releases 0, fails 1, unconfirmed 0)")
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
# N74 OPEN (a): rdna3mall=2 as the alternative to the pin
FALLBACK = ("rdna3mall=2 but DC's display power path did not come up (dcn32_power_init -62: the PMFW's DAL mailbox "
            "did not answer): the N74 pin runs instead")
rows = uclk_pin_rows({"DCNPowerFallback": FALLBACK, "DCNPower": PINNED, "DisplayHeads": 2})
check(rows[0][0] == "WARN" and "did not come up" in rows[0][2] and "void" in rows[0][2],
      "the rdna3mall fallback to the pin is a WARN naming the cause")
check(len(rows) == 2 and rows[1][0] == "INFO" and rows[1][2].startswith("UCLK pinned"),
      "the fallback pin's own row still follows")
rows = uclk_pin_rows({"DCNPowerFallback": FALLBACK, "DCNPower": GAVE_UP})
check([r[0] for r in rows] == ["WARN", "FAIL"], "a fallback pin that gave up is still a FAIL")
r = uclk_pin_rows({"DCNPower": MALL, "DisplayHeads": 1, "ExtendHead": 1})[0]
check(r[0] == "INFO" and "N74 alternative" in r[2] and "switches in active" in r[2],
      "the rdna3mall path switching in vactive with two heads is INFO, named as the N74 alternative")
MALL_NOT = "not updated: a head's refresh rate is unknown"
check(level({"DCNPower": MALL_NOT, "DisplayHeads": 2}) == "WARN",
      "the rdna3mall path without a decision and two heads is a WARN (UCLK left to the PMFW)")
check(level({"DCNPower": MALL_NOT, "DisplayHeads": 1}) == "INFO", "the rdna3mall path without a decision, one head: INFO")
MALL_VBLANK = MALL.replace("(vactive)", "(vblank)")
check(level({"DCNPower": MALL_VBLANK, "DisplayHeads": 2}) == "WARN",
      "the rdna3mall path switching in vblank only with two unsynchronised heads is a WARN")
MALL_PIN = MALL.replace("UCLK switching at 96 MHz (vactive)", "UCLK pinned at 1218 MHz (unsupported)")
r = uclk_pin_rows({"DCNPower": MALL_PIN, "DisplayHeads": 2})[0]
check(r[0] == "INFO" and "rdna3mall=2" in r[2], "DC's decision pinning UCLK is INFO with the DET-split hint")
check(level({"DCNPower": PINNED_AFTER_FAIL}) == "WARN", "pinned after failed messages is a WARN")
check("2 SetHardMinByFreq" in uclk_pin_rows({"DCNPower": PINNED_AFTER_FAIL})[0][2], "the earlier failures counted")
check(level({"DCNPower": PINNED_UNCONF}) == "WARN", "an unconfirmed pin is a WARN")
check(level({"DCNPower": GAVE_UP}) == "FAIL", "giving up is a FAIL")
r = uclk_pin_rows({"DCNPower": TIMED_OUT})[0]
check(r[0] == "FAIL" and "did not answer" in r[2] and "refused SetHardMin" not in r[2], "a timeout is not called a refusal")
r = uclk_pin_rows({"DCNPower": FAILED})[0]
check(r[0] == "FAIL" and "the PMFW refused" in r[2], "a refusal is called one")
check(level({"DCNPower": RELEASED.replace("fails 0", "fails 1")}) == "WARN", "a release after a failure is a WARN")
check(level({"DCNPower": PINNED, "UclkPinned": 1}) == "INFO", "RDNA3,UclkPinned 1 (confirmed) is INFO")
r = uclk_pin_rows({"DCNPower": PINNED, "UclkPinned": 2})[0]
check(r[0] == "WARN" and "UclkPinned 2" in r[2], "RDNA3,UclkPinned 2 (unconfirmed) is a WARN")
check(level({"DCNPower": PINNED.replace(")", ", status wait skipped (ReturnHardMinStatus unreliable))")}) == "WARN",
      "the skipped status wait is a WARN")
GAVE_UP_FIRST = ("UCLK pin GAVE UP (mode set): 3 refused / unanswered SetHardMinByFreq in a row (last -5): no DAL message "
                 "until the next bring-up, UCLK as the PMFW has it (last minimum 0 MHz); two heads lit (decision 3, "
                 "pins 0, releases 0, fails 3, unconfirmed 0)")
check(level({"DCNPower": GAVE_UP_FIRST}) == "FAIL", "the giving-up call itself reads GAVE UP")

if FAILS:
    print("test_triage_uclk_pin: %d check(s) failed" % len(FAILS))
    sys.exit(1)
print("test_triage_uclk_pin: all checks passed")
