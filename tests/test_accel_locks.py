#!/usr/bin/env python3
"""accel/rdna3-accel.cpp: the lock and release rules of DRIVER-PLAN N38's ECC review round 2 that no host test can
run (kernel code), checked in the source so a later edit cannot drop them silently:
 - the head's latch record (gLatch) is forgotten only under gPresLock: the present queue's worker rewrites it, so
   every rd3AccelLatchForget call sits in a function that holds the lock (rd3AccelLatchForgetHeads: under
   rd3AccelPresentForgetAll's hold; rd3AccelIopHome: its own hold) or goes through rd3AccelLatchForgetPres
 - the accelerator's stop releases the IOP flip's records (gIopFlipFront / gIopFlipPrev, under gPresLock) and the
   async IOP thread calls (gIopAs[].call), after the family's stop"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "accel", "rdna3-accel.cpp")
fails = []
lines = open(SRC).read().split("\n")
FUNC = re.compile(r"^(?:static\s+)?[A-Za-z_][\w\s\*&:<>]*?\b(\w+)\s*\(")


def enclosing(i):
    """the function whose body holds line i (the last column-0 definition above it)"""
    for j in range(i, -1, -1):
        m = FUNC.match(lines[j])
        if m and not lines[j].rstrip().endswith(";"):
            return m.group(1)
    return None


def body(name):
    """the text of function 'name' (from its definition to the next column-0 closing brace)"""
    for i, ln in enumerate(lines):
        m = FUNC.match(ln)
        if m and m.group(1) == name and not ln.rstrip().endswith(";"):
            out = []
            for k in range(i, len(lines)):
                out.append(lines[k])
                if lines[k].startswith("}"):
                    return "\n".join(out)
    return ""


LOCKED = {"rd3AccelLatchForgetHeads", "rd3AccelLatchForgetPres", "rd3AccelIopHome"}
calls = 0
for i, ln in enumerate(lines):
    if re.search(r"\brd3AccelLatchForget\(", ln) and not FUNC.match(ln):
        calls += 1
        f = enclosing(i)
        if f not in LOCKED:
            fails.append("line %d: rd3AccelLatchForget in %s, which does not hold gPresLock (use "
                         "rd3AccelLatchForgetPres)" % (i + 1, f))
if calls < 3:
    fails.append("only %d rd3AccelLatchForget call(s) found: the check no longer sees the source" % calls)
for name in ("rd3AccelLatchForgetPres", "rd3AccelIopHome"):
    b = body(name)
    if "IOLockLock(l)" not in b or "IOLockUnlock(l)" not in b:
        fails.append("%s does not take and drop gPresLock" % name)
fa = body("rd3AccelPresentForgetAll")
if not re.search(r"IOLockLock\(l\);.*rd3AccelLatchForgetHeads\(why\);.*IOLockUnlock\(l\);", fa, re.S):
    fails.append("rd3AccelPresentForgetAll does not call rd3AccelLatchForgetHeads under its hold of gPresLock")
for need in ("rd3AccelLatchForgetPres(head, \"a flip refused\")",
             "rd3AccelLatchForgetPres(h, \"the SDMA breaker opened\")",
             "rd3AccelLatchForgetPres(h, \"swap mode change\")"):
    if need not in "\n".join(lines):
        fails.append("missing: %s" % need)

stop = "\n".join(lines)
m = re.search(r"virtual void stop\(IOService \*provider\) APPLE_KEXT_OVERRIDE \{(.*?)\n    \}", stop, re.S)
if not m or not re.search(r"IOGraphicsAccelerator2::stop\(provider\);\s*rd3AccelIopStop\(\);", m.group(1)):
    fails.append("stop(): rd3AccelIopStop() does not follow IOGraphicsAccelerator2::stop")
ib = body("rd3AccelIopStop")
# a head still flipped to an IOP surface keeps its front (the HUBP may scan it); the rest are dropped
for need in ("thread_call_cancel_wait", "thread_call_free", "gIopFlipFront[h] && !gIopFlipped[h]",
             "gIopFlipFront[h] = nullptr", "gIopFlipPrev[h] = nullptr", "IOLockLock(l)", "->release()"):
    if need not in ib:
        fails.append("rd3AccelIopStop lacks %s" % need)
if ib.find("->release()") < ib.find("IOLockUnlock(l)"):
    fails.append("rd3AccelIopStop releases a surface while holding gPresLock")

if fails:
    print("test_accel_locks: FAIL\n  " + "\n  ".join(fails))
    sys.exit(1)
print("test_accel_locks: ok (%d rd3AccelLatchForget calls under gPresLock)" % calls)
