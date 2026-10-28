#!/usr/bin/env python3
"""tools/triage_bar.py (DRIVER-PLAN N62) on kernel lines exactly as the core kext writes them (RDNA3Drm.cpp drmInit,
RDNA3Bringup::mapVram): BAR0 mapped whole (INFO), a smaller window after failed maps (WARN), rdna3vramwin's cap (INFO),
nothing mapped with the reason (FAIL), the kext of card run 2026-10-28c without a reason (FAIL) and with 256 MiB (INFO)."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from triage_bar import bar_rows  # noqa: E402

FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("FAIL: " + what)


K = "2026-10-28 21:33:00.000 Df  kernel[0:1f] RDNA3: "
OK1G = K + ("VRAM aperture: BAR0 1024 MiB at phys 0x80000000, mapped 1024 MiB (kernel window, rdna3vramwin cap 1024 MiB), "
            "visible heap 912 MiB (15200 MiB beyond it)\n")
HALF = K + ("VRAM aperture: BAR0 1024 MiB at phys 0x80000000, mapped 512 MiB (kernel window, rdna3vramwin cap 1024 MiB; bigger "
            "windows failed to map), visible heap 400 MiB (15712 MiB beyond it)\n")
CAP = K + ("VRAM aperture: BAR0 1024 MiB at phys 0x80000000, mapped 256 MiB (kernel window, rdna3vramwin cap 256 MiB), "
           "visible heap 144 MiB (15968 MiB beyond it)\n")
WHY = ("BAR0 has no IODeviceMemory from IOPCIFamily (config space: BAR0 at 0x900000000, resizable BAR size unknown/0 MiB, "
       "CPUID says VMM): the firmware put it above 4 GiB and macOS in a VM keeps PCI BARs below 4 GiB")
NOT = K + "VRAM aperture: NOT MAPPED: BAR0 0 MiB, mapped 0 MiB, visible heap 0 MiB: " + WHY + "\n"
BRING_NEW = K + "cannot map BAR0 (VRAM aperture): " + WHY + "\n"
OLD_FAIL = (K + "DRM/amdgpu UAPI ready: VRAM 16368 MiB (0 MiB visible), user heap 0 MiB visible + 16000 MiB beyond BAR0\n" +
            K + "cannot map BAR0 (VRAM aperture)\n")
OLD_OK = K + "DRM/amdgpu UAPI ready: VRAM 16368 MiB (256 MiB visible), user heap 144 MiB visible + 15856 MiB beyond BAR0\n"


def levels(rows):
    return [lv for lv, _n, _t in rows]


r = bar_rows(OK1G)
check(levels(r) == ["INFO"] and "BAR0 1024 MiB, mapped 1024 MiB, visible heap 912 MiB" in r[0][2], "1 GiB mapped whole: %r" % r)
check(all(n == "VRAM aperture" for _l, n, _t in r), "row name")
r = bar_rows(HALF)
check(levels(r) == ["WARN"] and "failed to map" in r[0][2] and "mapped 512 MiB" in r[0][2], "a smaller window after failed maps: %r" % r)
r = bar_rows(CAP)
check(levels(r) == ["INFO"] and "rdna3vramwin" in r[0][2], "the cap: %r" % r)
# the failure: one FAIL with the reason; the bring-up's line with the same reason adds nothing
r = bar_rows(NOT + BRING_NEW)
check(levels(r) == ["FAIL"] and "above 4 GiB" in r[0][2], "not mapped, with the reason: %r" % r)
# the bring-up failed alone (the DRM window mapped, a later map did not)
r = bar_rows(OK1G + BRING_NEW)
check(levels(r) == ["INFO", "FAIL"] and "Stage 2 stopped" in r[1][2], "bring-up failure after a good DRM map: %r" % r)
# card run 2026-10-28c's kext: no reason logged
r = bar_rows(OLD_FAIL)
check(levels(r) == ["FAIL", "FAIL"] and "before N62" in r[0][2] and "no reason" in r[1][2], "the old kext's failure: %r" % r)
r = bar_rows(OLD_OK)
check(levels(r) == ["INFO"] and "256 MiB" in r[0][2], "the old kext with 256 MiB: %r" % r)
# the archive log and the serial log both carry the line: one row; the newest line wins
r = bar_rows(CAP, OK1G)
check(levels(r) == ["INFO"] and "mapped 1024 MiB" in r[0][2], "newest line wins: %r" % r)
# no core DRM init at all (a boot without the card): nothing
check(bar_rows("", K + "start, safety level 8\n") == [], "no rows without the core's lines")

if FAILS:
    print("test_triage_bar: %d failure(s)" % len(FAILS))
    sys.exit(1)
print("test_triage_bar: ok")
