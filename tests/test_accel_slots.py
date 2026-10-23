"""tools/check_accel_slots.py on synthetic assembly (ECC review round 1): the
flip-present hooks' base calls as -fapple-kext compiles them pass; a slot
that moved, a helper that is no tail jump, a missing helper or a call
through another class's vtable fail; a free() override that reaches
OSObject::free instead of its family class's own free fails (ECC review of
N41). The real check runs in
tools/kext_check.sh against the compiled accel/rdna3-accel.cpp.
Run: python3 -I -B tests/test_accel_slots.py"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import check_accel_slots as cas  # noqa: E402


def helper(fn, vt, slot, kind="jmpq"):
    return ("%s:                ## @%s\n## %%bb.0:\n\tpushq\t%%rbp\n\tmovq\t%%rsp, %%rbp\n"
            "\tmovq\t%s@GOTPCREL(%%rip), %%rax\n\tpopq\t%%rbp\n\t%s\t*%d(%%rax)    ## TAILCALL\n"
            "                                        ## -- End function\n" % (fn, fn, vt, kind, slot))


def good(skip=None, change=None):
    out = []
    for fn, (vt, slot) in cas.EXPECTED.items():
        if fn == skip:
            continue
        if change and fn == change[0]:
            out.append(helper(fn, change[1], change[2], change[3]))
            continue
        out.append(helper(fn, vt, slot))
    return "".join(out)


fails = []
if cas.check(good()):
    fails.append("the expected slots fail: %s" % cas.check(good()))
cases = [
    ("a slot moved by one entry", good(change=("_rd3BaseSubmitFlipBuffer", "__ZTV24IOAccelLegacyDisplayPipe",
                                                2520, "jmpq"))),
    ("a helper that calls instead of a tail jump", good(change=("_rd3BaseDidSubmitSwap",
                                                                "__ZTV20IOAccelLegacySurface", 2640, "callq"))),
    ("a missing helper", good(skip="_rd3BaseSubmitUpdate")),
    ("a call through another class's vtable", good(change=("_rd3BasePickPresentType",
                                                          "__ZTV24IOAccelLegacyDisplayPipe", 2672, "jmpq"))),
    ("the lock override's slot moved", good(change=("__ZN16RDNA3Accelerator18acceleratorDidLockEPKci",
                                                    "__ZTV22IOGraphicsAccelerator2", 2136, "callq"))),
    # ECC review of N41: a free() that reaches OSObject's vtable (OSObject::free directly) instead of the
    # family class's own free
    ("RD3VidMemory::free through OSObject's vtable", good(change=("__ZN12RD3VidMemory4freeEv", "__ZTV8OSObject",
                                                                  160, "callq"))),
    ("RD3VidMemory::free at another slot", good(change=("__ZN12RD3VidMemory4freeEv", "__ZTV16IOAccelVidMemory",
                                                        168, "callq"))),
    ("RD3VidMemory::free without its base call", good(skip="__ZN12RD3VidMemory4freeEv")),
]
for what, text in cases:
    if not cas.check(text):
        fails.append("not caught: %s" % what)
# a lock override may call (acceleratorDidLock records the owner after the base)
lock = good(change=("__ZN16RDNA3Accelerator18acceleratorDidLockEPKci", "__ZTV22IOGraphicsAccelerator2", 2144,
                    "callq"))
if cas.check(lock):
    fails.append("a lock override that calls the base fails: %s" % cas.check(lock))
# RD3VidMemory::free calls its base (work after it: the stack check), it need not be a tail jump
vfree = good(change=("__ZN12RD3VidMemory4freeEv", "__ZTV16IOAccelVidMemory", 160, "callq"))
if cas.check(vfree):
    fails.append("RD3VidMemory::free calling its base fails: %s" % cas.check(vfree))
for f in fails:
    print("test_accel_slots: FAIL " + f)
if fails:
    sys.exit(1)
print("test_accel_slots: %d cases ok" % (len(cases) + 3))
