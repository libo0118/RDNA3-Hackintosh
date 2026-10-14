#!/usr/bin/env python3
"""Disassemble functions of one kext out of a macOS kernel collection.

Card run 50: a panic at IOAccelLegacySurface::clientClose+0x1cc. Since
macOS 11 the kexts' code lives only in the kernel collections (MH_FILESET);
neither otool nor lldb splits them. This reads the fileset entry of the kext,
its LC_SYMTAB (offsets in the collection file), finds the functions whose
demangled-looking names match, cuts their bytes up to the next symbol and
has clang + otool disassemble them. Call targets inside the collection are
named from the same symbol table.

  kc_func.py KC BUNDLE-ID REGEX [REGEX...]       functions by name
  kc_func.py KC BUNDLE-ID --at ADDR[,ADDR...]     functions by address
  kc_func.py KC BUNDLE-ID --grep REGEX            every instruction of
                                                  __text matching, by function
"""
import re
import struct
import subprocess
import sys
import tempfile
import os

LC_SEGMENT_64, LC_SYMTAB, LC_FILESET_ENTRY = 0x19, 0x2, 0x80000035


def load_commands(buf, off):
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from("<IiiIIIII", buf, off)
    if magic != 0xfeedfacf:
        raise SystemExit("no 64-bit Mach-O header at 0x%x" % off)
    p = off + 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", buf, p)
        yield cmd, p, size
        p += size


def main():
    kc, bundle, pats = sys.argv[1], sys.argv[2], [re.compile(x) for x in sys.argv[3:]]
    buf = open(kc, "rb").read()
    entry = None
    for cmd, p, size in load_commands(buf, 0):
        if cmd == LC_FILESET_ENTRY:
            vmaddr, fileoff, name_off = struct.unpack_from("<QQI", buf, p + 8)
            name = buf[p + name_off:buf.index(b"\0", p + name_off)].decode()
            if name == bundle:
                entry = (vmaddr, fileoff)
    if not entry:
        raise SystemExit("no fileset entry " + bundle)
    segs, syms, sects = [], [], []
    for cmd, p, size in load_commands(buf, entry[1]):
        if cmd == LC_SEGMENT_64:
            segname = buf[p + 8:p + 24].rstrip(b"\0").decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", buf, p + 24)
            segs.append((segname, vmaddr, vmsize, fileoff, filesize))
            nsects = struct.unpack_from("<I", buf, p + 64)[0]
            for k in range(nsects):
                q = p + 72 + 80 * k
                sect = buf[q:q + 16].rstrip(b"\0").decode()
                saddr, ssize = struct.unpack_from("<QQ", buf, q + 32)
                sects.append((segname, sect, saddr, ssize))
        elif cmd == LC_SYMTAB:
            symoff, nsyms, stroff, strsize = struct.unpack_from("<IIII", buf, p + 8)
            for i in range(nsyms):
                strx, ntype, sect, desc, value = struct.unpack_from("<IBBhQ", buf, symoff + 16 * i)
                if ntype & 0xe0 or not sect or not value:   # stabs, undefined
                    continue
                name = buf[stroff + strx:buf.index(b"\0", stroff + strx)].decode(errors="replace")
                syms.append((value, name))
    syms.sort()
    print("entry %s at vmaddr 0x%x, file 0x%x; %d segments, %d symbols" % (bundle, entry[0], entry[1], len(segs), len(syms)))
    for s in segs:
        print("  segment %-16s vm 0x%x+0x%x file 0x%x+0x%x" % s)

    def file_of(addr):
        for _, vm, vsz, fo, fsz in segs:
            if vm <= addr < vm + min(vsz, fsz):
                return fo + addr - vm
        return None

    def name_of(addr):
        lo, hi = 0, len(syms)
        while lo < hi:
            mid = (lo + hi) // 2
            if syms[mid][0] <= addr:
                lo = mid + 1
            else:
                hi = mid
        if lo and addr - syms[lo - 1][0] < 0x4000:
            return "%s+0x%x" % (syms[lo - 1][1], addr - syms[lo - 1][0])
        return None

    demangle = {}
    try:
        names = "\n".join(n for _, n in syms) + "\n"
        out = subprocess.run(["xcrun", "c++filt"], input=names, capture_output=True, text=True).stdout.splitlines()
        demangle = dict(zip([n for _, n in syms], out))
    except OSError:
        pass

    def disasm(code):
        with tempfile.TemporaryDirectory() as d:
            s_, o = os.path.join(d, "f.s"), os.path.join(d, "f.o")
            with open(s_, "w") as fh:
                fh.write(".text\n_f:\n")
                for k in range(0, len(code), 16):
                    fh.write(".byte " + ",".join("0x%02x" % b for b in code[k:k + 16]) + "\n")
            subprocess.run(["xcrun", "clang", "-c", "-arch", "x86_64", s_, "-o", o], check=True)
            return subprocess.run(["otool", "-tv", o], capture_output=True, text=True).stdout.splitlines()

    def pretty_at(a):
        n = name_of(a)
        if not n:
            return "0x%x" % a
        base, _, off = n.rpartition("+")
        return demangle.get(base, base) + "+" + off

    if sys.argv[3] == "--grep":
        rx = re.compile(sys.argv[4])
        for seg, sect, saddr, ssize in sects:
            if (seg, sect) != ("__TEXT", "__text") and not (seg.startswith("__TEXT") and sect == "__text"):
                continue
            fo = file_of(saddr)
            print("== %s,%s 0x%x+0x%x" % (seg, sect, saddr, ssize))
            for line in disasm(buf[fo:fo + ssize])[2:]:
                m = re.match(r"^([0-9a-f]+)\s+(.*)$", line)
                if not m:
                    continue
                ins = m.group(2)
                t = re.search(r"\b(?:callq|jmp\w*|j\w+)\s+0x([0-9a-f]+)\b", ins)
                if t:   # the target by name, so a regex can ask for its callers
                    ins += "    ; " + pretty_at((saddr + int(t.group(1), 16)) & 0xffffffffffffffff)
                if rx.search(ins):
                    print("  %-60s %s" % (pretty_at(saddr + int(m.group(1), 16)), ins))
        return
    if sys.argv[3] == "--at":
        want = [int(x, 16) for x in sys.argv[4].split(",")]
        pats = []
        for a in want:
            n = name_of(a)
            if n:
                pats.append(re.compile("^" + re.escape(n.rpartition("+")[0]) + "$"))

    for i, (addr, name) in enumerate(syms):
        pretty = demangle.get(name, name)
        if not any(p_.search(pretty) or p_.search(name) for p_ in pats):
            continue
        end = next((a for a, _ in syms[i + 1:] if a > addr), addr + 0x400)
        end = min(end, addr + 0x1000)
        fo = file_of(addr)
        print("\n==== %s (%s) at 0x%x, %d bytes" % (pretty, name, addr, end - addr))
        if fo is None:
            print("  not in a file-backed segment")
            continue
        code = buf[fo:fo + (end - addr)]
        dis = disasm(code)
        for line in dis[2:]:
            m = re.match(r"^([0-9a-f]+)\s+(.*)$", line)
            if not m:
                continue
            off = int(m.group(1), 16)
            ins = m.group(2)
            note = ""
            # rip-relative and direct branch targets, as addresses in the collection
            t = re.search(r"\b(?:callq|jmp\w*|j\w+)\s+0x([0-9a-f]+)\b", ins)
            if t:
                tgt = (addr + int(t.group(1), 16)) & 0xffffffffffffffff
                note = pretty_at(tgt)
            r = re.search(r"(-?0x[0-9a-f]+)\(%rip\)", ins)
            if r:
                # the next instruction's offset: otool prints the bytes' length implicitly
                nxt = next((int(x.split()[0], 16) for x in dis[dis.index(line) + 1:] if re.match(r"^[0-9a-f]+\s", x)), off)
                tgt = (addr + nxt + int(r.group(1), 16)) & 0xffffffffffffffff
                note = (note + " " if note else "") + "[rip] " + pretty_at(tgt)
            print("  +0x%03x  %-48s %s" % (off, ins, ("; " + note) if note else ""))


if __name__ == "__main__":
    main()
