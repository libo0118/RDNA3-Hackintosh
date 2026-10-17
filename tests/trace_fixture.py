"""Synthetic per-frame traces (common/rdna3_trace.h) for
tests/test_triage_trace.sh: one per hypothesis the trace must tell apart.

  python3 -I tests/trace_fixture.py <kind> <out.bin>

kinds: b2 (card run 2026-10-17b's head 0 in mode 0: the first back drawn and
paged on, the second always empty), copy (mode 0: the shown buffer rewritten
in place at every swap), free (a buffer freed while scanned), cferr (a copy
forward answered an error), ab (an A/B: mode 1 clean, mode 0 rewriting in
place), clean (mode 1, nothing wrong, every question answerable), nopanel
(clean but a core without rdna3DisplayScanAddr), lost (the stream lost
events in the A/B window); ECC review round 1: v1 (ab with version 1
headers: still read), version (a version this decoder does not know),
truncated (the stream cut inside its last blob), corrupt (garbage between
two blobs), gap (a stream read missing from the file), inactive (phase 2
with no swaps), mode0never (phase 2's swaps still in mode 1), nomem (the
ring could not be allocated), short (ab, too few swaps per phase)."""
import struct
import sys

EV = "<QIBBBBIIIIIIQQQ8s8s"
HDR = "<IIIIIIQQQQIIIIIIQII"
HDR1 = "<IIIIIIQQQQIIIIII"
SEC = "<IBBBBHHHHHHHHHHI"
MAGIC = 0x52543352
F, B1, B2, D, HOME = 0x87040000, 0x8b100000, 0x8b8f0000, 0x8d000000, 0x80000000


class W(object):
    def __init__(self):
        self.ev = []
        self.t = 1000000
        self.seq = 0

    def add(self, ty, head=0xff, arg=0, flags=0, obj=0, v=(0, 0, 0, 0, 0), pa=0, pb=0, pc=0, ba=b"\0" * 8,
            bb=b"\0" * 8, dt=100):
        self.t += dt
        self.seq += 1
        self.ev.append(struct.pack(EV, self.t, self.seq, ty, head, arg, flags, obj, v[0], v[1], v[2], v[3], v[4],
                                   pa, pb, pc, ba, bb))

    def blob(self, kind=0, src=0, lost=0, ab=0, evs=None, version=2, nomem=0, written=None):
        evs = self.ev if evs is None else evs
        if written is None:
            written = struct.unpack_from(EV, evs[-1])[1] if evs else self.seq
        if version == 1:
            return struct.pack(HDR1, MAGIC, 1, 80, 32768, len(evs), 1, written, lost, self.t, nomem, 7, src, 1, 1,
                               kind, ab) + b"".join(evs)
        return struct.pack(HDR, MAGIC, version, 80, 32768, len(evs), 1, written, lost, self.t, nomem, 7, src, 1, 1,
                           kind, ab, 1792310400000000, 3, 96) + b"".join(evs)


def band(k):
    return bytes([(k * 7 + i) & 0xff or 1 for i in range(8)])


def swap(w, n, mode, arg, back, backpix, front=F, head=0, pc=0, flags=0, bandk=1, target=0, extra=0, waited=0):
    fl = 1 | (8 if backpix else 0) | (0 if backpix else 16) | flags
    rc2 = 5 if backpix else 4
    w.add(1, head, arg, fl, 0xabcd, (n, 4 | rc2 << 4 | 3 << 8 | mode << 12 | target << 16, 650 if mode else 0,
                                     waited, 0 | 1 << 8), front, back, pc, b"\0" * 8,
          band(bandk) if backpix else b"\0" * 8)
    w.add(22, head, 0, 0, 0, (n, 100, 0, 0, 0), pc or HOME, pc or HOME, pc)   # PANEL


def scan(w, n, idx, phys, pix, changed, head=0):
    w.add(15, head, idx, (1 if pix else 0) | (2 if changed else 0), 0xabcd, (n, 0, 5 if pix else 4, 0, 0), phys,
          0, 0, band(n) if pix else b"\0" * 8)


def common_tail(w):
    w.add(4, arg=1, v=(8100, 40, 0, 0, 0), pa=B1)                     # VMALLOC
    w.add(3, obj=0xabcd, v=(6, 1920 | 1080 << 16, 0x8424, 2 | 2 << 8, 0), pa=F, pb=B1, pc=B2)   # SHAPE
    w.add(8, arg=0, v=(1, 0x80000009, 4, 1920 | 1080 << 16, 0))     # SURFGEM refused
    w.add(2, 0, 16 + 1, 1, 0xabcd, (4, 4, 100, 50, 0), F, B1, F)      # CF ok


def make(kind):
    w = W()
    secs = []
    if kind == "b2":
        for i in range(40):
            w.add(6, arg=1, flags=1, obj=0x1111, v=(8100, 3, 0, 0, 0), pa=B1)   # page-on: B1 only
            swap(w, 2 * i + 1, 0, 1, B1, True, pc=B1, bandk=i % 3)
            scan(w, 2 * i + 1, 2, B2, False, False)
            swap(w, 2 * i + 2, 0, 0, B2, False, pc=B1)
            scan(w, 2 * i + 2, 1, B1, True, False)
            w.add(15, 0, 0xff, 4 | 16, 0xabcd, (2 * i + 2, 4000, 4, 0, 0), B2)   # delayed re-sample: still empty
        common_tail(w)
    elif kind == "copy":
        for i in range(40):
            swap(w, i + 1, 0, 1, B1, True, pc=B1, bandk=i)   # the same buffer shown, new content every swap
            scan(w, i + 1, 2, B2, False, False)
        common_tail(w)
    elif kind == "free":
        for i in range(10):
            swap(w, i + 1, 0, 1, B1, True, pc=B1, bandk=i % 2 + 1 if i < 5 else 1)
            scan(w, i + 1, 2, B2, True, False)
        w.add(5, head=0, arg=1, flags=2, v=(8100, 0, 5, 0, 0), pa=B1)   # freed while shown
        common_tail(w)
    elif kind == "cferr":
        for i in range(10):
            swap(w, i + 1, 1, 4, B1, True, pc=D, flags=64 | 32, target=2)
            scan(w, i + 1, 2, B2, True, False)
        w.add(2, 0, 16 + 1, 1, 0xabcd, (4, 0, 0, 0, 0xe00002c7), F, B1, F)
        common_tail(w)
    elif kind in AB_KINDS:
        ab = kind not in ("clean", "nopanel", "lost")
        n1 = 10 if kind == "short" else 40
        w.add(14, arg=1 if ab else 0, v=(0, 1, 0, 0, 0))   # the A/B's phase 1, else a swap mode change
        for i in range(n1):
            swap(w, i + 1, 1, 4, B1, True, pc=D if i % 2 else HOME, flags=64 | 32, target=2 if i % 2 else 1,
                 bandk=i, waited=5000)
            scan(w, i + 1, 2, B2, False, False)
        if ab:
            w.add(14, arg=1, v=(1, 2, 0, 0, 0))
            for i in range(0 if kind == "inactive" else n1):
                if kind == "mode0never":   # the mode change did not apply: still the copy
                    swap(w, 100 + i, 1, 4, B1, True, pc=D, flags=64 | 32, target=2, bandk=i)
                else:
                    swap(w, 100 + i, 0, 1, B1, True, pc=B1, bandk=i)
                scan(w, 100 + i, 2, B2, False, False)
            w.add(14, arg=1, v=(2, 3, 0, 0, 0))
        if ab:
            w.add(14, arg=1, v=(3, 0, 0, 0, 0))
        common_tail(w)
        w.add(16, arg=0, v=(158, 3, 77, 2 | 40 << 16, 0), pa=0x400000)   # CS
        w.add(17, arg=0, v=(0, 0, 77, 0, 0))                           # FENCE
        secs = [struct.pack(SEC, 5, 0, 1, 1 if ab else 0, 0, 40, 40, 0, 0, 0, 1, 0, 0, 0, 0, 200)]
        if kind == "nopanel":
            w.ev = [e for e in w.ev if struct.unpack_from(EV, e)[2] != 22]
            for i in range(40):
                w.add(22, 0, 0, 0, 0, (i, 0, 0, 0, 0), 0, 0, D)   # PANEL without the HUBP's address
    out = b""
    if kind in AB_KINDS:
        ver = 1 if kind == "v1" else 9 if kind == "version" else 2
        core = [e for e in w.ev if struct.unpack_from(EV, e)[2] in (16, 17)]
        acc = [e for e in w.ev if struct.unpack_from(EV, e)[2] not in (16, 17)]
        if kind == "lost":   # the second read's window began 25 events after the first one ended
            acc = [struct.pack(EV, *((lambda v: v[:1] + (v[1] + (25 if i >= len(acc) // 2 else 0),) + v[2:])(
                struct.unpack_from(EV, e)))) for i, e in enumerate(acc)]
        half = len(acc) // 2
        q = half // 2
        p1, p2 = (1, 2) if ab else (0, 0)
        if kind == "gap":   # three stream reads, the middle one not in the file
            out += w.blob(1, 0, 0, p1, acc[:q]) + w.blob(1, 0, 0, p2, acc[half:])
        else:
            out += w.blob(1, 0, 0, p1, acc[:half], ver) + w.blob(1, 0, 25 if kind == "lost" else 0, p2, acc[half:], ver,
                                                                 nomem=7 if kind == "nomem" else 0)
        out += w.blob(0, 1, 0, 0, core, 1 if kind == "v1" else 2)
        if kind == "v1":
            out += struct.pack(HDR1, MAGIC, 1, 32, 20480, len(secs), 1, w.seq, 0, w.t, 0, 7, 0, 1, 1, 2, 0)
        else:
            out += struct.pack(HDR, MAGIC, 2, 32, 20480, len(secs), 1, w.seq, 0, w.t, 0, 7, 0, 1, 1, 2, 0,
                               1792310400000000, 3, 96)
        out += b"".join(secs)
        if kind == "truncated":
            first = w.blob(1, 0, 0, 1, acc[:half])
            out = first + w.blob(1, 0, 0, 2, acc[half:])[:-100]
        if kind == "corrupt":
            first = w.blob(1, 0, 0, 1, acc[:half])
            out = first + b"garbage!" * 5 + out[len(first):]
    else:
        out = w.blob()
    return out


AB_KINDS = ("ab", "clean", "nopanel", "lost", "v1", "version", "truncated", "corrupt", "gap", "inactive",
            "mode0never", "nomem", "short")


if __name__ == "__main__":
    open(sys.argv[2], "wb").write(make(sys.argv[1]))
