#!/usr/bin/env python3
"""tools/ci/kmutil_kpi_guard.py against kmutil output captured on macOS 26.4.1
(card run 2026-10-25b, DRIVER-PLAN N47).

tests/data/kmutil/*-26.4.1.txt are `kmutil libraries -p <kext> --undef-symbols`
of the kexts kmutil refused (trimmed to the lines that matter plus a few
others, the format unchanged). Checked: thread_call_cancel_wait (exported
only by com.apple.kpi.private) fails both; kauth_cred_issuser (unsupported or
private) fails the accelerator until its Info.plist declares
com.apple.kpi.unsupported; the kexts built today (no thread_call_cancel_wait,
unsupported declared) pass; an import kmutil leaves out of its listing (no
kext exports it: kmutil prints nothing for it) fails, or warns with
--warn-unresolved; an unknown output line, an empty listing, a missing kmutil
and a missing nm -u list are errors (exit 2), never a pass.
"""
import os
import plistlib
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools", "ci"))
import kmutil_kpi_guard as g  # noqa: E402

DATA = os.path.join(ROOT, "tests", "data", "kmutil")
GUARD = os.path.join(ROOT, "tools", "ci", "kmutil_kpi_guard.py")
fails = []


def check(cond, what):
    if not cond:
        fails.append(what)


def read(name):
    with open(os.path.join(DATA, name)) as f:
        return f.read()


def plist(tmp, name, bid, libs):
    p = os.path.join(tmp, name)
    with open(p, "wb") as f:
        plistlib.dump({"CFBundleIdentifier": bid, "CFBundleExecutable": name,
                       "OSBundleLibraries": {k: "19.0" for k in libs}}, f)
    return p


def cli(*args, env=None):
    p = subprocess.run([sys.executable, "-I", "-B", GUARD] + list(args), stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, universal_newlines=True, env=env)
    return p.returncode, p.stdout


FB_LIBS = ["com.apple.iokit.IOGraphicsFamily", "com.apple.iokit.IOPCIFamily", "com.apple.kpi.iokit",
           "com.apple.kpi.libkern", "com.apple.kpi.mach", "com.apple.kpi.bsd"]
ACC_LIBS = ["com.apple.iokit.IOAcceleratorFamily2", "com.apple.iokit.IOGraphicsFamily", "com.apple.iokit.IOPCIFamily",
            "com.apple.iokit.IOSurface", "com.apple.kpi.iokit", "com.apple.kpi.libkern", "com.apple.kpi.mach",
            "com.apple.kpi.bsd"]

fb_txt = read("RDNA3Framebuffer-26.4.1.txt")
acc_txt = read("RDNA3Accelerator-26.4.1.txt")

# the parser on the real format: single lines and the 'in multiple:' block
syms, exts = g.parse_kmutil(acc_txt)
check(exts == ["io.github.voten641.RDNA3Accelerator"], "extension id read: %r" % exts)
check(syms.get("_thread_call_cancel_wait") == ["com.apple.kpi.private"], "cancel_wait -> private only")
check(sorted(syms.get("_kauth_cred_issuser", [])) == ["com.apple.kpi.private", "com.apple.kpi.unsupported"],
      "multi-provider block read: %r" % syms.get("_kauth_cred_issuser"))
check(syms.get("_current_thread") == ["com.apple.kpi.mach"], "a line after the block is a single again")
check(g.parse_nm_undef("_a\n_b\n\nfile (for architecture x86_64):\n                 U _c\n") == {"_a", "_b", "_c"},
      "nm -u forms")

# verdicts: the builds kmutil refused on 26.4.1
fb_syms, _ = g.parse_kmutil(fb_txt)
v = g.verdicts(fb_syms, set(fb_syms), set(FB_LIBS))
check(v == [("private", "_thread_call_cancel_wait", ["com.apple.kpi.private"])], "fb (card run 2026-10-25): %r" % v)
acc_syms, _ = g.parse_kmutil(acc_txt)
v = g.verdicts(acc_syms, set(acc_syms), set(ACC_LIBS))
check(sorted(v) == [("private", "_thread_call_cancel_wait", ["com.apple.kpi.private"]),
                    ("undeclared", "_kauth_cred_issuser", ["com.apple.kpi.private", "com.apple.kpi.unsupported"])],
      "accel (card run 2026-10-25): %r" % v)
# declaring com.apple.kpi.private does not make it usable
v = g.verdicts(fb_syms, set(fb_syms), set(FB_LIBS + ["com.apple.kpi.private"]))
check([k for k, _, _ in v] == ["private"], "a declared kpi.private still fails: %r" % v)

with tempfile.TemporaryDirectory() as tmp:
    def w(name, txt):
        p = os.path.join(tmp, name)
        with open(p, "w") as f:
            f.write(txt)
        return p

    def undef_of(txt, extra=()):
        s, _ = g.parse_kmutil(txt)
        return w("undef-%d.txt" % len(fails), "\n".join(sorted(set(s) | set(extra))) + "\n")

    fb_pl = plist(tmp, "RDNA3Framebuffer", "io.github.voten641.RDNA3Framebuffer", FB_LIBS)
    acc_pl_old = plist(tmp, "RDNA3Accelerator", "io.github.voten641.RDNA3Accelerator", ACC_LIBS)
    acc_pl_new = plist(tmp, "RDNA3AcceleratorNew", "io.github.voten641.RDNA3Accelerator",
                       ACC_LIBS + ["com.apple.kpi.unsupported"])

    # the CLI on the refused builds: FAIL, the symbols printed
    rc, out = cli("--kmutil-out", w("fb.txt", fb_txt), "--plist", fb_pl, "--undef", undef_of(fb_txt))
    check(rc == 1 and "FAIL" in out and "_thread_call_cancel_wait is exported only by com.apple.kpi.private" in out,
          "fb refused build: rc %d\n%s" % (rc, out))
    rc, out = cli("--kmutil-out", w("acc.txt", acc_txt), "--plist", acc_pl_old, "--undef", undef_of(acc_txt))
    check(rc == 1 and "_thread_call_cancel_wait" in out and "_kauth_cred_issuser" in out and
          "does not declare" in out, "accel refused build: rc %d\n%s" % (rc, out))

    # today's builds: thread_call_cancel_wait gone, unsupported declared -> ok
    fb_new = fb_txt.replace([l for l in fb_txt.splitlines() if "_thread_call_cancel_wait" in l][0] + "\n", "")
    acc_new = acc_txt.replace([l for l in acc_txt.splitlines() if "_thread_call_cancel_wait" in l][0] + "\n", "")
    rc, out = cli("--kmutil-out", w("fbn.txt", fb_new), "--plist", fb_pl, "--undef", undef_of(fb_new))
    check(rc == 0 and "kmutil_kpi_guard: ok" in out, "fb fixed: rc %d\n%s" % (rc, out))
    rc, out = cli("--kmutil-out", w("accn.txt", acc_new), "--plist", acc_pl_new, "--undef", undef_of(acc_new))
    check(rc == 0 and "kmutil_kpi_guard: ok" in out, "accel fixed: rc %d\n%s" % (rc, out))

    # an import kmutil does not list (exported by nothing): FAIL; --warn-unresolved: WARN, passes
    und = undef_of(fb_new, extra=["__ZN18IOMemoryDescriptor5doMapEP8ipc_portPyjyy"])
    rc, out = cli("--kmutil-out", w("fbu.txt", fb_new), "--plist", fb_pl, "--undef", und)
    check(rc == 1 and "doMap" in out and "exported by no kext" in out, "unresolved import: rc %d\n%s" % (rc, out))
    rc, out = cli("--warn-unresolved", "--kmutil-out", w("fbu2.txt", fb_new), "--plist", fb_pl, "--undef", und)
    check(rc == 0 and "WARN" in out and "doMap" in out, "--warn-unresolved: rc %d\n%s" % (rc, out))

    # a symbol only in a library the plist does not declare
    rc, out = cli("--kmutil-out", w("fbd.txt", fb_new), "--plist",
                  plist(tmp, "NoMach", "io.github.voten641.RDNA3Framebuffer",
                        [x for x in FB_LIBS if x != "com.apple.kpi.mach"]), "--undef", undef_of(fb_new))
    check(rc == 1 and "com.apple.kpi.mach, which Info.plist" in out, "undeclared library: rc %d\n%s" % (rc, out))

    # never a silent pass: unknown line, nothing listed, another kext's listing, empty nm, no kmutil
    rc, out = cli("--kmutil-out", w("bad.txt", fb_new + "\t\tsomething kmutil never said\n"), "--plist", fb_pl,
                  "--undef", undef_of(fb_new))
    check(rc == 2 and "not understood" in out and "NOT verified" in out, "unknown line: rc %d\n%s" % (rc, out))
    rc, out = cli("--kmutil-out", w("empty.txt", "No variant specified, falling back to release\n"), "--plist",
                  fb_pl, "--undef", undef_of(fb_new))
    check(rc == 2 and "listed no symbol" in out, "empty listing: rc %d\n%s" % (rc, out))
    rc, out = cli("--kmutil-out", w("other.txt", acc_new), "--plist", fb_pl, "--undef", undef_of(acc_new))
    check(rc == 2 and "not the kext's own" in out, "another kext's listing: rc %d\n%s" % (rc, out))
    rc, out = cli("--kmutil-out", w("fbe.txt", fb_new), "--plist", fb_pl, "--undef", w("none.txt", "\n"))
    check(rc == 2 and "nm -u listed no import" in out, "empty nm: rc %d\n%s" % (rc, out))
    # a kext bundle, no kmutil on PATH (Linux, or a runner without it): loud error
    kx = os.path.join(tmp, "X.kext", "Contents")
    os.makedirs(os.path.join(kx, "MacOS"))
    with open(os.path.join(kx, "MacOS", "X"), "w") as f:
        f.write("")
    plistlib.dump({"CFBundleIdentifier": "x", "CFBundleExecutable": "X"}, open(os.path.join(kx, "Info.plist"), "wb"))
    rc, out = cli(os.path.join(tmp, "X.kext"), env={"PATH": tmp})
    check(rc == 2 and "cannot run kmutil" in out and "NOT verified" in out, "no kmutil: rc %d\n%s" % (rc, out))
    rc, out = cli()
    check(rc == 2 and "no kext given" in out, "no argument: rc %d\n%s" % (rc, out))

if fails:
    for f in fails:
        print("test_kmutil_kpi_guard: FAIL " + f)
    sys.exit(1)
print("test_kmutil_kpi_guard: ok")
