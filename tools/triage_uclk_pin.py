#!/usr/bin/env python3
"""DRIVER-PLAN N74: the default path's UCLK pin (kext/src/RDNA3DisplayPower.cpp displayUclkPinUpdate, hw/dcn32_pstate.c
dcn32_uclk_pin_*). Two or more lit heads cannot hide the PMFW's UCLK switch in one shared vblank (bare metal 2026-11-01:
the secondary's top tenth flashed black), so with rdna3mall 0 the core sets UCLK's hard minimum at the top level and
says so in ioreg RDNA3,DCNPower ("UCLK pinned (two heads, ...)", "UCLK switching (...)", "UCLK pin FAILED ...",
"UCLK pin (N74) unavailable ...") and in `display power: UCLK ...` kernel lines.

uclk_pin_rows(props, log) -> [(level, name, text)]: FAIL when the PMFW refused the minimum, WARN when two heads are
lit and UCLK switches (not switched off), when the pin is unavailable, or when a two-head report has no RDNA3,DCNPower
at all; INFO otherwise. 'props' is triage_reports.parse_props()'s dict (keys without "RDNA3,")."""
import re

NAME = "UCLK pin (N74)"


def _lit(text):
    m = re.search(r"\b(two heads|(\d+) heads?)\b", text)
    if not m:
        return None
    return 2 if m.group(1) == "two heads" else int(m.group(2))


def uclk_pin_rows(props, log=""):
    v = props.get("DCNPower")
    heads = props.get("DisplayHeads", 0)
    heads = heads if isinstance(heads, int) else 0
    if "ExtendHead" in props:
        heads += 1
    lines = [l for l in (log or "").splitlines() if "display power: UCLK " in l]
    seen = " (%d decision line(s) in the kernel log, the last: %s)" % (
        len(lines), lines[-1][lines[-1].find("display power:"):][:160]) if lines else ""
    if v is None:
        if heads >= 2:
            return [("WARN", NAME, "no RDNA3,DCNPower with %d heads: a core before N74, or the bring-up never reached the "
                     "PMFW; whether UCLK is pinned is UNKNOWN (the secondary's top may flash at each UCLK switch)%s"
                     % (heads, seen))]
        return []
    v = str(v)
    if v.startswith("UCLK pin FAILED"):
        return [("FAIL", NAME, v + ": the PMFW refused SetHardMinByFreq(UCLK): the `uclk pin:` / `DAL SMU` lines" + seen)]
    if "UCLK pin (N74) unavailable" in v or "UCLK pin (N74) not set up" in v:
        return [("WARN", NAME, v + ": UCLK is the PMFW's (two heads may flash); the `uclk pin:` lines" + seen)]
    if v.startswith("UCLK pinned"):
        return [("INFO", NAME, v + " - expected with two heads (about +20 W idle; rdna3mall=2 is DC's way to let it "
                 "drop)" + seen)]
    if v.startswith("UCLK switching"):
        n = _lit(v)
        if "switched off" in v:
            return [("INFO", NAME, v + " - the A/B switch is off: with two heads the secondary's top may flash" + seen)]
        if n is not None and n >= 2:
            return [("WARN", NAME, v + ": %d heads lit but UCLK switches - the secondary's top flashes at each switch "
                     "(an unsynchronised pair should read `UCLK pinned`)" % n + seen)]
        return [("INFO", NAME, v + seen)]
    # rdna3mall=1/2: DC's whole decision (item O), not this path
    return [("INFO", NAME, "rdna3mall path (item O): " + v + seen)]
