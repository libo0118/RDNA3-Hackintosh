#!/usr/bin/env python3
"""ocpatchsim.py - what OpenCore 1.0.6 does with Kernel/Patch entries, on the files of a real macOS.

  ocpatchsim.py KERNEL_OR_BOOT_KC PATCHES.plist DARWIN [--identifier kernel] [--avoid START-END ...]

KERNEL_OR_BOOT_KC is /System/Library/Kernels/kernel or the Boot KC
(/System/Library/KernelCollections/BootKernelExtensions.kc, the file OpenCore patches on
macOS 11.3+), DARWIN the kernel version, e.g. 25.4.0 (uname -r). PATCHES.plist is an OpenCore
config.plist or a bare patches.plist (tools/vm/data/amd_vanilla-*.plist). Needs llvm-nm only for
entries with a Base. The file is read, never written.

Same semantics as OpenCorePkg 1.0.6 (Library/OcMainLib/OpenCoreKernelPatch.c, OcAppleKernelLib/
KextPatcher.c PatcherApplyGenericPatch, OcMiscLib/DataPatcher.c ApplyPatch):
  - Enabled, Identifier == kernel, Arch, MinKernel/MaxKernel against DARWIN
  - "borked": no Replace, Find and Replace of different size without Base, Mask or
    ReplaceMask of another size than Find
  - Base: first symbol of that exact name (a trailing space is part of the name); the search runs
    from the symbol to the END of the file (Limit caps it), not to the end of the function; without
    Base it runs from byte 0 of the whole file, in a Boot KC through all the kexts
  - a match needs (data & Mask) == Find; Skip matches are passed, then Count are replaced (0 = all);
    the next search starts after the replaced bytes; ReplaceMask keeps the other bits
  - the order of the entries in the file; later entries see the bytes of earlier ones
The quirks that run before the Patch list (ProvideCurrentCpuInfo, DisableLinkeditJettison) are NOT
simulated here. Exit status 1 when an applicable entry with Count > 0 got fewer matches than
Count, an applicable entry got none, or a match lies in an --avoid range (file offsets, hex).
"""
import bisect
import plistlib
import re
import struct
import subprocess
import sys


def parse_version(s):
    if not s:
        return None
    parts = (s.split(".") + ["0", "0", "0"])[:3]
    v = tuple(int(p) for p in parts)
    return None if v == (0, 0, 0) else v


def version_ok(darwin, min_kernel, max_kernel):
    """OcMatchDarwinVersion: an empty bound is open"""
    lo, hi = parse_version(min_kernel), parse_version(max_kernel)
    return (lo is None or darwin >= lo) and (hi is None or darwin <= hi)


def _regex(find, mask):
    if not mask:
        return re.compile(re.escape(bytes(find)), re.S)
    parts = []
    for f, m in zip(find, mask):
        if m == 0xFF:
            parts.append(re.escape(bytes([f])))
        elif m == 0:
            parts.append(b".")
        else:
            ok = [b for b in range(256) if b & m == f]
            parts.append(b"[" + b"".join(re.escape(bytes([b])) for b in ok) + b"]" if ok else b"(?!)")
    return re.compile(b"".join(parts), re.S)


def apply_patch(data, find, mask, replace, replace_mask, start, size, count, skip):
    """DataPatcher.c ApplyPatch on bytearray data[start:start+size]: [(offset, old, new)]"""
    rx, pos, end, out = _regex(find, mask), start, start + size, []
    while True:
        m = rx.search(data, pos, end)
        if not m:
            return out
        o = m.start()
        if skip > 0:
            skip -= 1
            pos = o + len(replace)
            continue
        old = bytes(data[o:o + len(replace)])
        if replace_mask:
            new = bytes((old[i] & ~replace_mask[i] & 0xFF) | (replace[i] & replace_mask[i]) for i in range(len(replace)))
        else:
            new = bytes(replace)
        data[o:o + len(replace)] = new
        out.append((o, old, new))
        pos = o + len(replace)
        if count > 0:
            count -= 1
            if count == 0:
                return out


def macho_segments(data):
    ncmds, = struct.unpack_from("<I", data, 16)
    off, segs = 32, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, off)
        if cmd == 0x19:
            name = bytes(data[off + 8:off + 24]).rstrip(b"\0").decode()
            vm, vs, fo, fs = struct.unpack_from("<QQQQ", data, off + 24)
            segs.append((name, vm, fo, fs))
        off += size
    return segs


class Symbols:
    """first symbol of a name, in symbol table order (what PatcherGetSymbolAddressValue finds)"""

    def __init__(self, path, data):
        self.segs = macho_segments(data)
        self.by_name, self.funcs = {}, []
        out = subprocess.run(["llvm-nm", "-p", path], capture_output=True, text=True, check=True).stdout
        for line in out.splitlines():
            p = line.split()
            if len(p) == 3 and re.fullmatch("[0-9a-f]{16}", p[0]) and p[1].upper() != "U":
                self.by_name.setdefault(p[2], int(p[0], 16))
                if p[1] in "Tt":
                    self.funcs.append((int(p[0], 16), p[2]))
        self.funcs.sort()
        self.addrs = [a for a, _ in self.funcs]

    def file_offset(self, name):
        va = self.by_name.get(name)
        if va is None:
            return None
        for _, vm, fo, fs in self.segs:
            if fs and vm <= va < vm + fs:
                return fo + va - vm
        return None

    def where(self, foff):
        for seg, vm, fo, fs in self.segs:
            if fs and fo <= foff < fo + fs:
                va = vm + foff - fo
                i = bisect.bisect_right(self.addrs, va) - 1
                near = "%s+%#x" % (self.funcs[i][1], va - self.funcs[i][0]) if i >= 0 else "?"
                return "%s %#x %s" % (seg, va, near)
        return "outside every segment"


def load_patches(path):
    with open(path, "rb") as f:
        pl = plistlib.load(f)
    return pl["Kernel"]["Patch"] if "Kernel" in pl else pl["Patch"]


def simulate(data, patches, darwin, symbols=None, identifier="kernel"):
    """[(index, entry, status, [(offset, old, new)])]; status: skipped-* / borked / no-base /
    applied / not-found"""
    res = []
    for idx, p in enumerate(patches):
        if not p.get("Enabled", True) or p.get("Identifier") != identifier:
            continue
        if p.get("Arch", "Any") == "i386":
            res.append((idx, p, "skipped-arch", []))
            continue
        if not version_ok(darwin, p.get("MinKernel", ""), p.get("MaxKernel", "")):
            res.append((idx, p, "skipped-version", []))
            continue
        find, repl = bytes(p.get("Find", b"")), bytes(p.get("Replace", b""))
        mask, rmask = bytes(p.get("Mask", b"")), bytes(p.get("ReplaceMask", b""))
        base = p.get("Base", "")
        if (not repl or (not base and len(find) != len(repl)) or (mask and len(mask) != len(find))
                or (rmask and len(rmask) != len(find))):
            res.append((idx, p, "borked", []))
            continue
        start, size = 0, len(data)
        if base:
            off = symbols.file_offset(base) if symbols else None
            if off is None:
                res.append((idx, p, "no-base", []))
                continue
            start, size = off, len(data) - off
        if not find:
            old = bytes(data[start:start + len(repl)])
            data[start:start + len(repl)] = repl
            res.append((idx, p, "applied", [(start, old, repl)]))
            continue
        limit = p.get("Limit", 0)
        if 0 < limit < size:
            size = limit
        hits = apply_patch(data, find, mask, repl, rmask, start, size, p.get("Count", 0), p.get("Skip", 0))
        res.append((idx, p, "applied" if hits else "not-found", hits))
    return res


def main(argv):
    avoid, args, ident = [], [], "kernel"
    it = iter(argv)
    for a in it:
        if a == "--avoid":
            lo, hi = next(it).split("-")
            avoid.append((int(lo, 16), int(hi, 16)))
        elif a == "--identifier":
            ident = next(it)
        else:
            args.append(a)
    if len(args) != 3:
        sys.exit(__doc__)
    path, plist, ver = args
    darwin = tuple(int(x) for x in ver.split("."))
    data = bytearray(open(path, "rb").read())
    patches = load_patches(plist)
    symbols = Symbols(path, data) if any(p.get("Base") for p in patches) else None
    bad = 0
    for idx, p, status, hits in simulate(data, patches, darwin, symbols, ident):
        want = p.get("Count", 0)
        flag = ""
        if status in ("borked", "no-base", "not-found") or (status == "applied" and want and len(hits) < want):
            flag, bad = "  <== PROBLEM", bad + 1
        print("[%d] %s\n    %s  matches %d of Count %s  Base=%r%s" % (
            idx, p.get("Comment", ""), status, len(hits), want or "all", p.get("Base", ""), flag))
        for o, old, new in hits:
            hit = any(lo <= o + k < hi for lo, hi in avoid for k in range(len(old)))
            if hit:
                bad += 1
            print("    %#x  %s%s\n      %s -> %s" % (o, symbols.where(o) if symbols else "", "  <== IN AN --avoid RANGE" if hit else "",
                                                       old.hex(" "), new.hex(" ")))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
