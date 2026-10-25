#!/usr/bin/env python3
"""Every kext of the driver links against com.apple.kpi.* 19.0 or newer.

Card run 2026-10-25 (macOS 26.4.1): RDNA3Framebuffer and RDNA3Accelerator,
which declared com.apple.kpi.* 8.0, were refused by kmutil ("Failed to bind
'_thread_call_cancel_wait' ... could not find a kext which exports this
symbol"); the core RDNA3Accel, declaring 19.0 and using the same call, linked.
thread_call_cancel_wait is in xnu's Private.exports on Darwin 24 and 25, so a
kext that asks for the old KPI versions does not get it on macOS 26.
"""
import os
import plistlib
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLISTS = ["kext/Info.plist", "fb/Info.plist", "accel/Info.plist", "agdc/Info.plist"]
MIN_MAJOR = 19


def main():
    bad = []
    for rel in PLISTS:
        with open(os.path.join(ROOT, rel), "rb") as f:
            libs = plistlib.load(f).get("OSBundleLibraries", {})
        for lib, ver in sorted(libs.items()):
            if not lib.startswith("com.apple.kpi."):
                continue
            try:
                major = int(str(ver).split(".")[0])
            except ValueError:
                bad.append("%s: %s has an unreadable version %r" % (rel, lib, ver))
                continue
            if major < MIN_MAJOR:
                bad.append("%s: %s %s (needs %d.0 or newer: macOS 26 links the old "
                           "KPI versions without the calls the driver uses)" % (rel, lib, ver, MIN_MAJOR))
    if bad:
        for b in bad:
            print("test_kext_kpi_versions: FAIL " + b)
        sys.exit(1)
    print("test_kext_kpi_versions: ok")


if __name__ == "__main__":
    main()
