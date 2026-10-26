#!/usr/bin/env python3
"""tools/vm/ocpatchsim.py: the OpenCore 1.0.6 patch semantics (DataPatcher.c ApplyPatch,
PatcherApplyGenericPatch, OcKernelApplyPatches' checks) on synthetic buffers. No kernel needed;
the Base lookup (llvm-nm) is covered by passing a fake symbol table."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "vm"))
import ocpatchsim as o


def fail(msg):
    sys.exit("test_ocpatchsim: FAIL " + msg)


def entry(**kw):
    e = dict(Identifier="kernel", Enabled=True, Arch="Any", Base="", Count=1, Skip=0, Limit=0,
             MinKernel="", MaxKernel="", Mask=b"", ReplaceMask=b"", Comment="t")
    e.update(kw)
    return e


class FakeSyms:
    def __init__(self, table):
        self.t = table

    def file_offset(self, name):
        return self.t.get(name)


data = bytearray(b"\x00" * 8 + b"AB" * 4 + b"\x00" * 8 + b"AB" * 4)
# Count / Skip / Count 0 = all, and the next search starts after the replaced bytes
d = bytearray(data)
r = o.simulate(d, [entry(Find=b"AB", Replace=b"CD", Count=0)], (25, 4, 0))
if len(r[0][3]) != 8 or b"AB" in d:
    fail("Count 0 must replace every match")
d = bytearray(data)
r = o.simulate(d, [entry(Find=b"AB", Replace=b"CD", Count=2, Skip=1)], (25, 4, 0))
if [h[0] for h in r[0][3]] != [10, 12]:
    fail("Skip 1, Count 2: %r" % [h[0] for h in r[0][3]])
# Mask: (data & Mask) == Find; ReplaceMask keeps the other bits
d = bytearray(b"\x00\x12\x34\x00")
r = o.simulate(d, [entry(Find=b"\x10\x30", Mask=b"\xf0\xf0", Replace=b"\xaa\xbb", ReplaceMask=b"\xf0\x0f")], (25, 4, 0))
if bytes(d) != b"\x00\xa2\x3b\x00" or r[0][2] != "applied":
    fail("mask / replace mask: %s" % bytes(d).hex())
d = bytearray(b"\x00\x12\x34\x00")
r = o.simulate(d, [entry(Find=b"\x12\x34", Mask=b"\xf0\xf0", Replace=b"\x00\x00")], (25, 4, 0))
if r[0][2] != "not-found":
    fail("an unmasked Find with a Mask must not match (the bytes are compared after the mask)")
# Base: the search starts at the symbol, a trailing space is part of the name, a missing one fails
d = bytearray(b"XY" + b"\x00" * 6 + b"XY")
r = o.simulate(d, [entry(Find=b"XY", Replace=b"ZZ", Base="_f")], (25, 4, 0), FakeSyms({"_f": 4}))
if [h[0] for h in r[0][3]] != [8] or bytes(d[:2]) != b"XY":
    fail("Base: search from the symbol")
r = o.simulate(bytearray(d), [entry(Find=b"XY", Replace=b"ZZ", Base="_f ")], (25, 4, 0), FakeSyms({"_f": 4}))
if r[0][2] != "no-base":
    fail("'_f ' (trailing space, AMD_Vanilla's CPUFAMILY patch) must not find '_f'")
# Min/MaxKernel, Arch, Enabled, Identifier, borked
cases = [
    (entry(Find=b"A", Replace=b"B", MinKernel="25.4.0"), (25, 3, 9), "skipped-version"),
    (entry(Find=b"A", Replace=b"B", MaxKernel="25.3.99"), (25, 4, 0), "skipped-version"),
    (entry(Find=b"A", Replace=b"B", MinKernel="25.4.0", MaxKernel="25.99.99"), (25, 4, 0), "applied"),
    (entry(Find=b"A", Replace=b"B", Arch="i386"), (25, 4, 0), "skipped-arch"),
    (entry(Find=b"AA", Replace=b"B"), (25, 4, 0), "borked"),
    (entry(Find=b"A", Replace=b"B", Mask=b"\xff\xff"), (25, 4, 0), "borked"),
    (entry(Find=b"A", Replace=b""), (25, 4, 0), "borked"),
]
for e, ver, want in cases:
    got = o.simulate(bytearray(b"AAAA"), [e], ver)[0][2]
    if got != want:
        fail("%r at %r: %s, wanted %s" % (e.get("MinKernel") or e.get("MaxKernel") or e.get("Arch"), ver, got, want))
if o.simulate(bytearray(b"AAAA"), [entry(Find=b"A", Replace=b"B", Enabled=False)], (25, 4, 0)):
    fail("a disabled entry must be ignored")
if o.simulate(bytearray(b"AAAA"), [entry(Find=b"A", Replace=b"B", Identifier="com.apple.x")], (25, 4, 0)):
    fail("a kext entry is not a kernel patch")
# Find empty with Base: Replace is written at the symbol
d = bytearray(b"\x90" * 8)
o.simulate(d, [entry(Find=b"", Replace=b"\xc3", Base="_f")], (25, 4, 0), FakeSyms({"_f": 3}))
if bytes(d) != b"\x90\x90\x90\xc3\x90\x90\x90\x90":
    fail("Find empty: Replace at the symbol")
print("test_ocpatchsim: ok")
