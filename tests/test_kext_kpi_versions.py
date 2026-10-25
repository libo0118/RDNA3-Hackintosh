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
The C symbols come from xnu-12377's (macOS 26.4) own export lists in
tools/ci/data/ (tools/ci/gen_xnu_kpi_lists.py; ECC review round 1: CI's
macOS 15 runner cannot see what 26 made private), so any private-only or
unsupported-only call is caught here on Linux too, not only
thread_call_cancel_wait / kauth_cred_issuser. The built binaries are checked
on CI's macOS runner by tools/ci/kmutil_kpi_guard.py (kmutil libraries, nm -u
and the same lists).
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
# macOS 26.4.1 (kmutil libraries): exported only by com.apple.kpi.private; the lists must hold these
PRIVATE_ONLY = ("thread_call_cancel_wait",)
# exported by com.apple.kpi.unsupported (and private): the kext must declare unsupported
NEEDS_UNSUPPORTED = ("kauth_cred_issuser",)
DATA = os.path.join(ROOT, "tools", "ci", "data")


def xnu_c_names(kind):
    """the C function names (no C++ manglings, no patterns) of the newest xnu list of that kind"""
    paths = sorted(glob.glob(os.path.join(DATA, "xnu-*-x86_64-%s.txt" % kind)))
    if not paths:
        return None, set()
    tag_of = lambda p: [int(x) if x.isdigit() else x for x in re.split(r"[.-]", os.path.basename(p))]  # noqa: E731
    p = max(paths, key=tag_of)
    with open(p) as f:
        names = [l.strip() for l in f if l.strip() and l[0] not in "#!"]
    return os.path.basename(p), {n[1:] for n in names if n.startswith("_") and not n.startswith("__Z") and "*" not in n}


def code_of(path):
    """the file without comments and string literals"""
    with open(path, errors="replace") as f:
        txt = f.read()
    txt = re.sub(r"/\*.*?\*/", " ", txt, flags=re.S)
    txt = re.sub(r"//[^\n]*", " ", txt)
    return re.sub(r'"(\\.|[^"\\\n])*"', '""', txt)


def called_names(paths):
    """{identifier followed by '(' : [files]} in the code (comments and strings out)"""
    out = {}
    for p in paths:
        for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", code_of(p)):
            out.setdefault(m.group(1), set()).add(os.path.relpath(p, ROOT))
    return out


def main():
    bad = []
    plist_priv, priv = xnu_c_names("private-only")
    plist_uns, uns = xnu_c_names("unsupported-only")
    if not priv or not uns:
        bad.append("no xnu export lists in tools/ci/data (tools/ci/gen_xnu_kpi_lists.py makes them)")
    for sym in PRIVATE_ONLY:
        if priv and sym not in priv:
            bad.append("%s does not list %s as private-only" % (plist_priv, sym))
    for sym in NEEDS_UNSUPPORTED:
        if uns and sym not in uns:
            bad.append("%s does not list %s as unsupported-only" % (plist_uns, sym))
    priv = priv | set(PRIVATE_ONLY)
    uns = uns | set(NEEDS_UNSUPPORTED)
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
        used = called_names(srcs)
        for sym in sorted(priv & set(used)):
            for src in sorted(used[sym]):
                bad.append("%s calls %s: macOS 26 exports it from com.apple.kpi.private only, kmutil refuses "
                           "the kext (thread_call_cancel_wait: use common/rdna3_tcall.h)" % (src, sym))
        if "com.apple.kpi.unsupported" not in libs:
            for sym in sorted(uns & set(used)):
                bad.append("%s: %s calls %s but com.apple.kpi.unsupported is not in OSBundleLibraries"
                           % (rel, ", ".join(sorted(used[sym])), sym))
    if bad:
        for b in bad:
            print("test_kext_kpi_versions: FAIL " + b)
        sys.exit(1)
    print("test_kext_kpi_versions: ok (%d kexts: KPI >= %d.0, none of %s's %d private-only calls, unsupported "
          "declared where one of its %d is used)" % (len(KEXTS), MIN_MAJOR, plist_priv, len(priv), len(uns)))


if __name__ == "__main__":
    main()
