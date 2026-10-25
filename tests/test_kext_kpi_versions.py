#!/usr/bin/env python3
"""The driver's kexts link with public KPIs only, at com.apple.kpi.* 19.0 or newer.

Card run 2026-10-25 (macOS 26.4.1): kmutil refused RDNA3Framebuffer and
RDNA3Accelerator ("Failed to bind '_thread_call_cancel_wait' ... could not
find a kext which exports this symbol"). Raising their com.apple.kpi.* from
8.0 to 19.0 (the first fix) did not help (card run 2026-10-25b):
`kmutil libraries -p <kext> --undef-symbols` on 26.4.1 shows the symbol in
com.apple.kpi.private only, which a third-party kext in the Auxiliary KC
cannot link; the core RDNA3Accel only loaded because OpenCore's prelinker
does not enforce the KPI exports. So:
  - no kext source calls a symbol that 26 exports only from
    com.apple.kpi.private (thread_call_cancel_wait: common/rdna3_tcall.h
    stops thread calls with public KPIs);
  - a kext that calls a com.apple.kpi.unsupported symbol (kauth_cred_issuser:
    unsupported or private on 26) declares com.apple.kpi.unsupported;
  - every com.apple.kpi.* stays at 19.0 or newer.
The built binaries are checked on CI's macOS runner by
tools/ci/kmutil_kpi_guard.py (kmutil libraries).
"""
import glob
import os
import plistlib
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MIN_MAJOR = 19
# plist -> the sources compiled into that kext (common/ headers they include count)
KEXTS = {
    "kext/Info.plist": ["kext/src/*.cpp", "kext/src/*.hpp", "kext/src/*.h", "kext/src/*.c"],
    "fb/Info.plist": ["fb/*.cpp"],
    "accel/Info.plist": ["accel/*.cpp", "accel/*.h"],
    "agdc/Info.plist": ["agdc/*.cpp", "agdc/*.h"],
}
COMMON = ["common/*.h"]
# macOS 26.4.1 (kmutil libraries): exported only by com.apple.kpi.private
PRIVATE_ONLY = ("thread_call_cancel_wait",)
# exported by com.apple.kpi.unsupported (and private): the kext must declare unsupported
NEEDS_UNSUPPORTED = ("kauth_cred_issuser",)


def code_of(path):
    """the file without comments and string literals"""
    with open(path, errors="replace") as f:
        txt = f.read()
    txt = re.sub(r"/\*.*?\*/", " ", txt, flags=re.S)
    txt = re.sub(r"//[^\n]*", " ", txt)
    return re.sub(r'"(\\.|[^"\\\n])*"', '""', txt)


def calls(paths, sym):
    rx = re.compile(r"\b%s\s*\(" % re.escape(sym))
    return [os.path.relpath(p, ROOT) for p in paths if rx.search(code_of(p))]


def main():
    bad = []
    common = sorted(p for g in COMMON for p in glob.glob(os.path.join(ROOT, g)))
    for rel, globs in sorted(KEXTS.items()):
        with open(os.path.join(ROOT, rel), "rb") as f:
            libs = plistlib.load(f).get("OSBundleLibraries", {})
        for lib, ver in sorted(libs.items()):
            if not lib.startswith("com.apple.kpi."):
                continue
            if lib == "com.apple.kpi.private":
                bad.append("%s: declares com.apple.kpi.private (a third-party kext cannot link it on macOS 26)" % rel)
            try:
                major = int(str(ver).split(".")[0])
            except ValueError:
                bad.append("%s: %s has an unreadable version %r" % (rel, lib, ver))
                continue
            if major < MIN_MAJOR:
                bad.append("%s: %s %s (needs %d.0 or newer, like the core)" % (rel, lib, ver, MIN_MAJOR))
        srcs = sorted(p for g in globs for p in glob.glob(os.path.join(ROOT, g))) + common
        if not srcs:
            bad.append("%s: no sources found for it (the table in this test is stale)" % rel)
        for sym in PRIVATE_ONLY:
            for src in calls(srcs, sym):
                bad.append("%s calls %s: macOS 26 exports it from com.apple.kpi.private only, kmutil refuses "
                           "the kext (use common/rdna3_tcall.h)" % (src, sym))
        for sym in NEEDS_UNSUPPORTED:
            users = calls(srcs, sym)
            if users and "com.apple.kpi.unsupported" not in libs:
                bad.append("%s: %s calls %s but com.apple.kpi.unsupported is not in OSBundleLibraries"
                           % (rel, ", ".join(users), sym))
    if bad:
        for b in bad:
            print("test_kext_kpi_versions: FAIL " + b)
        sys.exit(1)
    print("test_kext_kpi_versions: ok (%d kexts: KPI >= %d.0, no private-only symbol, unsupported declared where "
          "used)" % (len(KEXTS), MIN_MAJOR))


if __name__ == "__main__":
    main()
