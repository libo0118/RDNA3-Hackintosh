#!/usr/bin/env python3
"""kmutil_kpi_guard.py - does a built kext link on this Mac the way kmutil links it?

Card run 2026-10-25 / 2026-10-25b (DRIVER-PLAN N47): macOS 26 puts
RDNA3Framebuffer and RDNA3Accelerator into the Auxiliary KC through kmutil,
which refused both ("Failed to bind '_thread_call_cancel_wait' ... could not
find a kext which exports this symbol"): the symbol is exported only by
com.apple.kpi.private, which a third-party kext cannot link. Nothing in the
build caught it. This runs, for every kext given:

    kmutil libraries -p <kext> --undef-symbols    (every import and the
                                                   library kext exporting it)
    nm -u <kext>/Contents/MacOS/<executable>      (every import)

and FAILS when an import
  - is exported only by com.apple.kpi.private,
  - is exported only by libraries the kext's Info.plist does not declare
    in OSBundleLibraries (kmutil would not bind it there either), or
  - is exported by nothing (kmutil leaves such symbols out of its listing
    silently, so they are found as nm -u minus the listing).
kmutil or nm missing or failing, or a line of kmutil's output this script
does not know, is an error too (exit 2): the guard never passes silently.

The output format it reads (macOS 26.4.1, `kmutil libraries -p X --undef-symbols`):

    No variant specified, falling back to release
    For arch x86_64:
    \tFor extension io.github.voten641.RDNA3Framebuffer:
    \t\t_thread_call_cancel_wait in /System/Library/KernelCollections/BootKernelExtensions.kc: com.apple.kpi.private (25.4.0)
    \t\t_kauth_cred_issuser in multiple:
    \t\t\t/System/Library/KernelCollections/BootKernelExtensions.kc: com.apple.kpi.unsupported (25.4.0)
    \t\t\t/System/Library/KernelCollections/BootKernelExtensions.kc: com.apple.kpi.private (25.4.0)

Usage:
    kmutil_kpi_guard.py [--warn-unresolved] KEXT...
    kmutil_kpi_guard.py --kmutil-out FILE --plist INFO.PLIST --undef NM_U_FILE   (captured output, the host test)
Exit: 0 clean, 1 a symbol kmutil would refuse, 2 the check could not run.
"""
import os
import plistlib
import re
import subprocess
import sys

PRIVATE = "com.apple.kpi.private"

RE_SINGLE = re.compile(r"^\s+(\S+) in (/.+?): (\S+) \(([^)]*)\)\s*$")
RE_MULTI = re.compile(r"^\s+(\S+) in multiple:\s*$")
RE_PROVIDER = re.compile(r"^\s+(/.+?): (\S+) \(([^)]*)\)\s*$")
RE_EXT = re.compile(r"^\s*For extension (\S+?):\s*$")
RE_IGNORE = re.compile(r"^(No variant specified.*|For arch \S+:|\s*)$")


class GuardError(Exception):
    """the check could not run (exit 2)"""


def parse_kmutil(text):
    """{symbol: [library bundle id, ...]} and the extension ids named, from
    kmutil libraries --undef-symbols output; GuardError on a line it does not know"""
    syms, exts, multi = {}, [], None
    for n, line in enumerate(text.splitlines(), 1):
        m = RE_SINGLE.match(line)
        if m:
            syms.setdefault(m.group(1), []).append(m.group(3))
            multi = None
            continue
        m = RE_MULTI.match(line)
        if m:
            multi = m.group(1)
            syms.setdefault(multi, [])
            continue
        m = RE_PROVIDER.match(line)
        if m:
            if multi is None:
                raise GuardError("kmutil output line %d: a library line outside an 'in multiple:' block: %r"
                                 % (n, line))
            syms[multi].append(m.group(2))
            continue
        m = RE_EXT.match(line)
        if m:
            exts.append(m.group(1))
            multi = None
            continue
        if RE_IGNORE.match(line):
            continue
        raise GuardError("kmutil output line %d not understood (a new kmutil format?): %r" % (n, line))
    for s, libs in syms.items():
        if not libs:
            raise GuardError("kmutil output: %s 'in multiple:' with no library listed" % s)
    return syms, exts


def parse_nm_undef(text):
    """the symbol names of `nm -u` output (one per line; 'U name' forms too)"""
    out = set()
    for line in text.splitlines():
        line = line.strip()
        if not line or line.endswith(":"):   # a blank line, "file (for architecture x86_64):"
            continue
        out.add(line.split()[-1])
    return out


def declared_libs(plist_path):
    try:
        with open(plist_path, "rb") as f:
            pl = plistlib.load(f)
    except (OSError, plistlib.InvalidFileException, ValueError) as e:
        raise GuardError("cannot read %s: %s" % (plist_path, e))
    return pl, set((pl.get("OSBundleLibraries") or {}).keys())


def verdicts(syms, undef, declared):
    """[(kind, symbol, libraries)]: kind 'private' (exported only by
    com.apple.kpi.private), 'undeclared' (only by libraries the plist does
    not declare), 'unresolved' (by nothing: in nm -u, not in kmutil's list)"""
    out = []
    for s in sorted(syms):
        libs = sorted(set(syms[s]))
        usable = [lib for lib in libs if lib in declared and lib != PRIVATE]
        if usable:
            continue
        if libs == [PRIVATE]:
            out.append(("private", s, libs))
        else:
            out.append(("undeclared", s, libs))
    for s in sorted(undef - set(syms)):
        out.append(("unresolved", s, []))
    return out


def check(name, kmutil_text, nm_text, plist_path, warn_unresolved=False, expect_id=None):
    """prints the result for one kext; returns 0 clean, 1 refused symbols"""
    syms, exts = parse_kmutil(kmutil_text)
    undef = parse_nm_undef(nm_text)
    pl, declared = declared_libs(plist_path)
    bid = expect_id or pl.get("CFBundleIdentifier")
    if bid and exts and bid not in exts:
        raise GuardError("%s: kmutil listed %s, not the kext's own %s" % (name, ", ".join(exts), bid))
    if not syms:
        raise GuardError("%s: kmutil listed no symbol at all (%d imports by nm -u): its output was not read "
                         "right, or kmutil did not find the kext" % (name, len(undef)))
    if not undef:
        raise GuardError("%s: nm -u listed no import at all: not a kext executable?" % name)
    bad = verdicts(syms, undef, declared)
    rc = 0
    for kind, s, libs in bad:
        if kind == "private":
            print("kmutil_kpi_guard: FAIL %s: %s is exported only by com.apple.kpi.private: kmutil refuses the "
                  "kext on this macOS (use public KPIs: common/rdna3_tcall.h for thread_call_cancel_wait)"
                  % (name, s))
            rc = 1
        elif kind == "undeclared":
            print("kmutil_kpi_guard: FAIL %s: %s is exported only by %s, which Info.plist's OSBundleLibraries "
                  "does not declare" % (name, s, ", ".join(libs)))
            rc = 1
        elif warn_unresolved:
            print("kmutil_kpi_guard: WARN %s: %s is exported by no kext on this macOS (--warn-unresolved: a build "
                  "for another macOS)" % (name, s))
        else:
            print("kmutil_kpi_guard: FAIL %s: %s is exported by no kext on this macOS (kmutil leaves it out of its "
                  "listing; the link fails)" % (name, s))
            rc = 1
    print("kmutil_kpi_guard: %s %s: %d imports, %d resolved by kmutil, libraries declared: %s"
          % ("FAIL" if rc else "ok", name, len(undef), len(syms), ", ".join(sorted(declared)) or "none"))
    return rc


def run(cmd):
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    except OSError as e:
        raise GuardError("cannot run %s: %s (is it installed on this runner?)" % (cmd[0], e))
    if p.returncode != 0:
        raise GuardError("%s exited %d: %s" % (" ".join(cmd), p.returncode, (p.stderr or p.stdout).strip()[:400]))
    return p.stdout


def check_kext(path, warn_unresolved=False):
    path = os.path.realpath(path)   # kmutil finds no extension through a symlink (/tmp on macOS)
    plist = os.path.join(path, "Contents", "Info.plist")
    pl, _ = declared_libs(plist)
    exe = pl.get("CFBundleExecutable")
    if not exe:
        raise GuardError("%s: no CFBundleExecutable in Info.plist" % path)
    binary = os.path.join(path, "Contents", "MacOS", exe)
    if not os.path.isfile(binary):
        raise GuardError("%s: no executable %s" % (path, binary))
    km = run(["kmutil", "libraries", "-p", path, "--undef-symbols"])
    nm = run(["nm", "-u", binary])
    return check(os.path.basename(path), km, nm, plist, warn_unresolved)


def main(argv):
    args = list(argv)
    warn = "--warn-unresolved" in args
    args = [a for a in args if a != "--warn-unresolved"]
    try:
        if args and args[0] == "--kmutil-out":
            opts = dict(zip(args[0::2], args[1::2]))
            if len(args) != 6 or set(opts) != {"--kmutil-out", "--plist", "--undef"}:
                raise GuardError("usage: --kmutil-out FILE --plist INFO.PLIST --undef NM_U_FILE")
            with open(opts["--kmutil-out"]) as f:
                km = f.read()
            with open(opts["--undef"]) as f:
                nm = f.read()
            return check(os.path.basename(opts["--kmutil-out"]), km, nm, opts["--plist"], warn)
        if not args:
            raise GuardError("no kext given")
        rc = 0
        for k in args:
            rc |= check_kext(k, warn)
        return rc
    except GuardError as e:
        print("kmutil_kpi_guard: ERROR %s -- the KPI check did NOT run, the kexts are NOT verified" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
