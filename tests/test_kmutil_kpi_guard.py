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
--warn-unresolved (listed and counted); an unknown output line, an empty
listing, a missing 'For extension' line, a missing kmutil, a missing nm -u
list and any crash of the guard are errors (exit 2), never a pass.

ECC review round 1: the runner's kmutil (macOS 15) cannot see what macOS 26
made private, so every import is also checked against xnu-12377.101.15's
export lists (tools/ci/data): a private-only import fails even when the
runner's kmutil resolves it to a public KPI; an unsupported-only one fails
unless com.apple.kpi.unsupported is declared.
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
    check(rc == 2 and "no 'For extension" in out, "empty listing: rc %d\n%s" % (rc, out))
    rc, out = cli("--kmutil-out", w("empty2.txt", "For arch x86_64:\n\tFor extension io.github.voten641.RDNA3Framebuffer:\n"),
                  "--plist", fb_pl, "--undef", undef_of(fb_new))
    check(rc == 2 and "listed no symbol" in out, "no symbol listed: rc %d\n%s" % (rc, out))
    rc, out = cli("--kmutil-out", w("noext.txt", "\n".join(l for l in fb_new.splitlines() if "For extension" not in l)),
                  "--plist", fb_pl, "--undef", undef_of(fb_new))
    check(rc == 2 and "no 'For extension" in out, "missing For extension line: rc %d\n%s" % (rc, out))
    rc, out = cli("--kmutil-out", w("fbx.txt", fb_new), "--plist", fb_pl, "--undef", os.path.join(tmp, "missing"))
    check(rc == 2 and "NOT verified" in out, "a crash (missing file) is not verified: rc %d\n%s" % (rc, out))

    # the macOS 26 gate: the runner's kmutil (older macOS) resolves the symbol to a public KPI, xnu's lists say
    # private / unsupported only
    old_fb = fb_txt.replace("_thread_call_cancel_wait in /System/Library/KernelCollections/BootKernelExtensions.kc: "
                            "com.apple.kpi.private (25.4.0)",
                            "_thread_call_cancel_wait in /System/Library/KernelCollections/BootKernelExtensions.kc: "
                            "com.apple.kpi.mach (24.6.0)")
    check(old_fb != fb_txt, "fixture edit for the 26 gate")
    rc, out = cli("--kmutil-out", w("fb15.txt", old_fb), "--plist", fb_pl, "--undef", undef_of(old_fb))
    check(rc == 1 and "_thread_call_cancel_wait is exported only by com.apple.kpi.private in xnu-12377" in out,
          "26 gate, private-only: rc %d\n%s" % (rc, out))
    i = acc_new.index("\t\t_kauth_cred_issuser in multiple:")
    j = acc_new.index("\t\t_current_task")
    old_acc = acc_new[:i] + ("\t\t_kauth_cred_issuser in /System/Library/KernelCollections/BootKernelExtensions.kc: "
                             "com.apple.kpi.bsd (24.6.0)\n") + acc_new[j:]
    rc, out = cli("--kmutil-out", w("acc15.txt", old_acc), "--plist", acc_pl_old, "--undef", undef_of(old_acc))
    check(rc == 1 and "_kauth_cred_issuser is exported only by com.apple.kpi.unsupported in xnu-12377" in out,
          "26 gate, unsupported-only undeclared: rc %d\n%s" % (rc, out))
    rc, out = cli("--kmutil-out", w("acc15b.txt", old_acc), "--plist", acc_pl_new, "--undef", undef_of(old_acc))
    check(rc == 0, "26 gate, unsupported declared: rc %d\n%s" % (rc, out))
    # --warn-unresolved lists and counts
    und2 = undef_of(fb_new, extra=["__Zfoo1", "__Zfoo2"])
    rc, out = cli("--warn-unresolved", "--kmutil-out", w("fbu3.txt", fb_new), "--plist", fb_pl, "--undef", und2)
    check(rc == 0 and "2 import(s) unresolved on this macOS (warned, not failed): __Zfoo1 __Zfoo2" in out,
          "unresolved count: rc %d\n%s" % (rc, out))
    # a 26-private import this runner cannot resolve at all: --warn-unresolved warns, the 26 gate still fails
    und3 = undef_of(fb_new, extra=["_thread_call_cancel_wait"])
    noline = fb_new.replace([l for l in fb_txt.splitlines() if "_thread_call_cancel_wait" in l][0] + "\n", "")
    rc, out = cli("--warn-unresolved", "--kmutil-out", w("fbu4.txt", noline), "--plist", fb_pl, "--undef", und3)
    check(rc == 1 and "WARN" in out and "only by com.apple.kpi.private in xnu-12377" in out,
          "unresolved here, private on 26: rc %d\n%s" % (rc, out))
    # no lists: an error
    try:
        g.load_xnu_lists(tmp)
        check(False, "load_xnu_lists without lists did not raise")
    except g.GuardError:
        pass
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

# the generator on a tiny config/: aliases, patterns, the x86_64 files, a public exception to a private pattern
import importlib.util  # noqa: E402
spec = importlib.util.spec_from_file_location("gen", os.path.join(ROOT, "tools", "ci", "gen_xnu_kpi_lists.py"))
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)
with tempfile.TemporaryDirectory() as tmp:
    cfg = os.path.join(tmp, "config")
    os.makedirs(cfg)
    files = {"Mach.exports": "_thread_call_cancel\n_pub_alias:_pub_alias_external\n",
             "IOKit.x86_64.exports": "__ZN5IOCPU5startEv\n# a comment\n",
             "BSDKernel.exports": "_kauth_cred_get\n", "Libkern.exports": "_OSAddAtomic\n",
             "MACFramework.exports": "_mac_x\n",
             "Private.exports": "_thread_call_cancel_wait\n_kauth_cred_issuser\n_pub_alias\n",
             "Private.x86_64.MacOSX.exports": "__ZN5IOCPU*\n",
             "Unsupported.exports": "_kauth_cred_issuser\n_unsup_x : _unsup_x_impl\n"}
    for k, v in files.items():
        with open(os.path.join(cfg, k), "w") as f:
            f.write(v)
    import contextlib  # noqa: E402
    import io  # noqa: E402
    with contextlib.redirect_stdout(io.StringIO()):
        gen.main(["--tag", "xnu-1.2.3", "--src", cfg, "--out", tmp])
    t2, p2, u2 = g.load_xnu_lists(tmp)
    check(t2 == "xnu-1.2.3", "generated tag %s" % t2)
    check(g.in_list("_thread_call_cancel_wait", p2) and not g.in_list("_kauth_cred_issuser", p2) and
          not g.in_list("_pub_alias", p2), "generated private-only: %r" % (p2,))
    check(g.in_list("__ZN5IOCPU4stopEv", p2) and not g.in_list("__ZN5IOCPU5startEv", p2),
          "private pattern with a public exception: %r" % (p2,))
    check(g.in_list("_kauth_cred_issuser", u2) and g.in_list("_unsup_x", u2) and not g.in_list("_thread_call_cancel", u2),
          "generated unsupported-only: %r" % (u2,))

# the committed lists: the two symbols of card run 2026-10-25b, and the public KPIs rdna3_tcall.h uses in neither
tag, priv, unsup = g.load_xnu_lists()
check(tag.startswith("xnu-12377."), "lists of macOS 26 (xnu-12377): %s" % tag)
check(g.in_list("_thread_call_cancel_wait", priv), "thread_call_cancel_wait private-only")
check(g.in_list("_kauth_cred_issuser", unsup) and not g.in_list("_kauth_cred_issuser", priv),
      "kauth_cred_issuser unsupported-only")
for pub in ("_thread_call_cancel", "_thread_call_allocate_with_options", "_thread_call_enter_delayed",
            "_thread_call_free", "_current_thread", "_IOSleep", "_IOLog", "_IOMalloc", "_kauth_cred_get"):
    check(not g.in_list(pub, priv) and not g.in_list(pub, unsup), "%s is public in %s" % (pub, tag))
check(len(priv[0]) > 1000 and len(unsup[0]) > 100, "list sizes %d / %d" % (len(priv[0]), len(unsup[0])))

if fails:
    for f in fails:
        print("test_kmutil_kpi_guard: FAIL " + f)
    sys.exit(1)
print("test_kmutil_kpi_guard: ok")
