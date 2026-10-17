#!/usr/bin/env python3
"""check_accel_slots.py - the flip-present hooks' base calls, as compiled.

The naked wrappers in accel/rdna3-accel.cpp (RD3_HOOK_ASM) call the family's
own implementation through rd3Base<Method>, a C++ call qualified with the
base class that -fapple-kext compiles into a tail jump through the base's
exported vtable symbol: `movq __ZTV<Base>@GOTPCREL(%rip), %rax` then
`jmpq *<slot>(%rax)`. The slot comes from the reconstructed headers
(accel/iofam); a header that drifts by one entry would forward every call to
the wrong family method. The card runs measured these slots (tahoe_fwd.h,
card run 2026-10-17d), so the compiled operands must stay exactly these.
The family-lock overrides (acceleratorDidLock / acceleratorWillUnlock) are
checked the same way. ECC review round 1.

    python3 tools/check_accel_slots.py <rdna3-accel.s>

tools/kext_check.sh compiles accel/rdna3-accel.cpp with -S and runs this.
Exit status 0 when every function holds its expected base call.
"""
import re
import sys

# function (asm label) -> (base class vtable symbol, slot operand)
EXPECTED = {
    "_rd3BaseSubmitFlipBuffer": ("__ZTV24IOAccelLegacyDisplayPipe", 2512),
    "_rd3BaseSubmitScanoutFlipBuffer": ("__ZTV24IOAccelLegacyDisplayPipe", 2504),
    "_rd3BasePickPresentType": ("__ZTV20IOAccelLegacySurface", 2672),
    "_rd3BaseDidSubmitSwap": ("__ZTV20IOAccelLegacySurface", 2640),
    "_rd3BaseIsBackBufferReady": ("__ZTV20IOAccelLegacySurface", 2648),
    "_rd3BaseSubmitUpdate": ("__ZTV20IOAccelLegacySurface", 2664),
    "__ZN16RDNA3Accelerator18acceleratorDidLockEPKci": ("__ZTV22IOGraphicsAccelerator2", 2144),
    "__ZN16RDNA3Accelerator21acceleratorWillUnlockEPKci": ("__ZTV22IOGraphicsAccelerator2", 2152),
}
# the rd3Base* helpers must be a pure tail jump; the lock overrides may call
TAIL_ONLY = {k for k in EXPECTED if k.startswith("_rd3Base")}

LABEL = re.compile(r"^([A-Za-z_.$][\w.$]*):")
VTLOAD = re.compile(r"movq\s+(__ZTV\w+)@GOTPCREL\(%rip\),\s*%(\w+)")
VCALL = re.compile(r"(jmpq|callq)\s+\*(\d+)\(%(\w+)\)")


def functions(text):
    """asm label -> its instruction lines (up to the next global label)"""
    out, cur = {}, None
    for line in text.splitlines():
        m = LABEL.match(line)
        if m and not m.group(1).startswith(("L", ".")):
            cur = m.group(1)
            out[cur] = []
            continue
        if cur is not None:
            out[cur].append(line.split("##")[0].strip())
    return out


def check(text):
    """the list of problems (empty: all good)"""
    fns = functions(text)
    bad = []
    for fn, (vt, slot) in EXPECTED.items():
        body = fns.get(fn)
        if body is None:
            bad.append("%s: not in the assembly (renamed, or inlined away?)" % fn)
            continue
        regs = {}
        calls = []
        for ins in body:
            m = VTLOAD.search(ins)
            if m:
                regs[m.group(2)] = m.group(1)
            m = VCALL.search(ins)
            if m:
                calls.append((m.group(1), int(m.group(2)), regs.get(m.group(3))))
        want = [c for c in calls if c[2] == vt]
        if len(want) != 1:
            bad.append("%s: %d calls through %s's vtable, expected one (calls: %s)" % (fn, len(want), vt, calls))
            continue
        kind, got, _ = want[0]
        if got != slot:
            bad.append("%s: %s *%d through %s, expected slot %d -- the reconstructed header moved"
                       % (fn, kind, got, vt, slot))
        if fn in TAIL_ONLY and (kind != "jmpq" or len(calls) != 1):
            bad.append("%s: not a single tail jump (%s) -- the wrapper's registers would not reach the base"
                       % (fn, calls))
    return bad


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[0])
        print("usage: check_accel_slots.py <rdna3-accel.s>")
        return 2
    with open(argv[1], encoding="utf-8", errors="replace") as f:
        bad = check(f.read())
    for b in bad:
        print("check_accel_slots: FAIL " + b)
    if not bad:
        print("check_accel_slots: %d base calls at their slots" % len(EXPECTED))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
