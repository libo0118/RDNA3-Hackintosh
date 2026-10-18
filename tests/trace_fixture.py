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
ring could not be allocated), short (ab, too few swaps per phase); card
run 2026-10-17c: b2lost (mode 1 held the empty back while the other back
had a new frame), b2landed (mode 1 presented that landed buffer), b2would
(the same held with debug.rdna3accel_swap_landed=0: found, not presented),
stale, stalehold, stalemixed, stalewould, stale20ms, stalereset, stalereal
(head 1, mode 1: copy forward -> B1, swap B1, swap B2 that WindowServer never
redrew; stale_run says how each differs); card run 2026-10-17d: gen,
genreal, flippath (the write generations' holds, a hold of a back the trace
saw written, a copy forward burst with no swap; gen_run says how)."""
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


def swap(w, n, mode, arg, back, backpix, front=F, head=0, pc=0, flags=0, bandk=1, target=0, extra=0, waited=0,
         landed=None, would=None, stale=None, dt=100, stalewould=False):
    """landed: the index of the buffer a mode 1/2 swap presented instead of
    its (empty) back (SWAP v1 bit 21, v4 >> 24; card run 2026-10-17c);
    stale: the swap that last presented the back a mode 1/2 swap held as
    stale (SWAP v1 bit 23, v3)"""
    fl = 1 | (8 if backpix else 0) | (0 if backpix or landed is not None else 16) | flags
    rc2 = 5 if backpix else 4
    if would is not None:   # found, not presented (SWAP v1 bit 22)
        landed_bits, landed = 1 << 22, would
    else:
        landed_bits = 1 << 21 if landed is not None else 0
    if stale is not None:
        landed_bits |= 1 << 23
        waited = stale
    if stalewould:
        landed_bits |= 1 << 24
    w.add(1, head, arg, fl, 0xabcd, (n, 4 | rc2 << 4 | 3 << 8 | mode << 12 | target << 16 |
                                     landed_bits, 650 if mode else 0,
                                     waited, 0 | 1 << 8 | (landed or 0) << 24), front, back, pc, b"\0" * 8,
          band(bandk) if backpix else b"\0" * 8, dt=dt)
    w.add(22, head, 0, 0, 0, (n, 100, 0, 0, 0), pc or HOME, pc or HOME, pc)   # PANEL


STALE_KINDS = ("stale", "stalehold", "stalemixed", "stalewould", "stale20ms", "stalereset", "stalereal")


def stale_run(w, kind):
    """head 1, mode 1: CF -> B1, swap B1 (a new frame), swap B2 (not redrawn since swap 1). stale: B2 copied
    (the old kext); stalehold: held (SWAP v1 bit 23, v3 1); stalemixed: held at even frames, copied at odd;
    stalewould: copied with v1 bit 24 (swap_stale 0); stale20ms: copied 25 ms after B1's copy in the first 15
    frames (not stale by the decoder's window), 19 ms in the last 15; stalereset: copied, but the kernel's
    records are forgotten before each (GONE, a swap mode change there and back, D reallocated); stalereal:
    held while B1 had a new frame (even) or after an aperture write into B2 (odd)"""
    cf(w, 1, B2)
    swap(w, 1, 1, 4, B2, True, head=1, pc=D, flags=64 | 32, target=2, bandk=200)
    w.add(15, 1, 0xfe, 64 | 1, 0, (1, 0, 5, 0, 0), D, 0, 0, band(200))
    for i in range(30):
        n = 2 * i + 2
        cf(w, 1, B1)
        swap(w, n, 1, 4, B1, True, head=1, pc=HOME, flags=64 | 32, target=1, bandk=10 + i)
        w.add(15, 1, 0xfe, 64 | 1, 0, (n, 0, 5, 0, 0), HOME, 0, 0, band(10 + i))   # the scanout: the new frame
        scan(w, n, 2, B2, True, False, head=1)                                    # B2: not redrawn
        hold = kind in ("stalehold", "stalereal") or (kind == "stalemixed" and i % 2 == 0)
        if kind == "stalereset":
            if i % 3 == 0:
                w.add(13, 1, 0, 0, 0xabcd)                                         # GONE: the surface
            elif i % 3 == 1:
                w.add(14, arg=0, v=(1, 2, 0, 0, 0))                                 # MODESET 1 -> 2
                w.add(14, arg=0, v=(2, 1, 0, 0, 0))                                 # ... and back
            else:
                w.add(4, 1, 1, 1 | 8, 0x5000, (8100, 40, 0, 0, 0), D)               # VMALLOC: head 1's new D
        if hold:
            if kind == "stalereal" and i % 2 == 0:
                scan(w, n + 1, 1, B1, True, True, head=1)                          # B1 had a new frame
            elif kind == "stalereal":
                w.add(6, arg=1, flags=1, obj=0x1111, v=(8100, 3, 0, 0, 0), pa=B2)  # the CPU wrote B2
            swap(w, n + 1, 1, 5, B2, True, head=1, pc=HOME, stale=1)
        else:
            swap(w, n + 1, 1, 4, B2, True, head=1, pc=D, flags=64 | 32, target=2, bandk=200,
                 dt=(25 if i < 15 else 19) * 10 ** 6 if kind == "stale20ms" else 100,
                 stalewould=kind == "stalewould")
            w.add(15, 1, 0xfe, 64 | 1, 0, (n + 1, 0, 5, 0, 0), D, 0, 0, band(200))


GEN_KINDS = ("gen", "genreal", "flippath", "genchanged")


def gswap(w, n, arg, back, why, hold=False, head=0, gen=0, hgen=0, dt=100):
    """a mode 1 swap the write generations judged (card run 2026-10-17d): why = enum rd3_wg_why (SWAP v1 >> 28),
    hold = SWAP v1 bit 25 (v3 the head's generation), the back's generation in v2 >> 16"""
    v1 = 4 | 5 << 4 | 3 << 8 | 1 << 12 | why << 28 | (1 << 25 if hold else 0)
    w.add(1, head, arg, 1 | 8 | (64 | 32 if arg == 4 else 0), 0xabcd,
          (n, v1, (650 if arg == 4 else 0) | (gen & 0xffff) << 16, hgen if hold else 0, 1 << 8), F, back,
          D if arg == 4 else HOME, b"\0" * 8, band(n), dt=dt)
    w.add(22, head, 0, 0, 0, (n, 100, 0, 0, 0), D, D, D)   # PANEL


def gen_run(w, kind):
    """head 0, mode 1, per frame: page-on B1, copy forward -> B1, swap B1 (newer: presented), swap B2 (never written:
    held). genreal: at odd frames the CPU also writes B2 (a page-on) before its swap, which is still held - the
    decoder sees the write, the kernel did not stamp it (FAIL). flippath: a copy forward burst rotating F -> B1 -> B2
    -> F with no swap (the family's flip-buffer path), the flip-buffer calls and the surface hooks traced"""
    g = 0
    if kind == "genchanged":   # B2 presented once: every later hold of it has other band bytes (a changed back held)
        gswap(w, 99, 4, B2, 1, gen=1)
    for i in range(30):
        w.add(6, arg=1, flags=1, obj=0x1111, v=(8100, 3, 0, 0, 0), pa=B1)
        cf(w, 0, B1)
        g += 2
        gswap(w, 2 * i + 1, 4, B1, 1, gen=g)
        if kind == "genreal" and i % 2:
            w.add(6, arg=1, flags=1, obj=0x1112, v=(8100, 3, 0, 0, 0), pa=B2)
        gswap(w, 2 * i + 2, 5, B2, 6, hold=True, gen=0, hgen=g)
        if kind == "flippath" and i == 10:
            for k, (src, dst) in enumerate(((F, B1), (B1, B2), (B2, F)) * 3):
                w.add(2, 0, 16 + 1, 1, 0xabcd, (1, 1, 1, 50, 0), src, dst, src, dt=16 * 10 ** 6)
                if k % 3 == 0:
                    w.add(23, 0, 0, 0, 0x7777, (1, k // 3 + 1, 0x11, 0x22, 0), dst, src, D)   # FLIPBUF, traced
            w.add(24, 0, 2, 0, 0xabcd, (1, 0, 1, 1, 0))   # HOOK isBackBufferReady: ready
            w.add(24, 0, 2, 0, 0xabcd, (2, 0, 0, 2, 0))   # ... not ready
            w.add(24, 0, 0, 0, 0xabcd, (1, 0, 0, 1, 0))   # pickPresentType


def cf(w, head, target):
    """a mode 1/2 copy forward from the front into 'target' (CF, pick 16 + 1)"""
    w.add(2, head, 16 + 1, 1, 0xabcd, (4, 4, 100, 50, 0), F, target, F)


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
    if kind != "nojoin":   # the swaps' a1 / a2 with their IOSurface ids (SURFRES how 3)
        for phys, sid in ((F, 100), (B1, 101), (B2, 102)):
            w.add(21, obj=0x2000 + sid, v=(sid, 1, 3, 8100, 0xc0), pa=phys)
    if kind == "b2":
        for i in range(40):
            w.add(6, arg=1, flags=1, obj=0x1111, v=(8100, 3, 0, 0, 0), pa=B1)   # page-on: B1 only
            swap(w, 2 * i + 1, 0, 1, B1, True, pc=B1, bandk=i % 3)
            scan(w, 2 * i + 1, 2, B2, False, False)
            swap(w, 2 * i + 2, 0, 0, B2, False, pc=B1)
            scan(w, 2 * i + 2, 1, B1, True, False)
            w.add(15, 0, 0xff, 4 | 16, 0xabcd, (2 * i + 2, 4000, 4, 0, 0), B2)   # delayed re-sample: still empty
        common_tail(w)
    elif kind in ("b2lost", "b2landed", "b2would"):   # mode 1: B1 drawn, then a swap of the empty B2 with B1 redrawn
        for i in range(40):
            w.add(6, arg=1, flags=1, obj=0x1111, v=(8100, 3, 0, 0, 0), pa=B1)
            swap(w, 2 * i + 1, 1, 4, B1, True, pc=D, flags=64 | 32, target=2, bandk=2 * i)
            scan(w, 2 * i + 1, 2, B2, False, False)
            if kind in ("b2lost", "b2would"):   # held although B1 had a new frame: it waits for the next swap
                scan(w, 2 * i + 2, 1, B1, True, True)
                swap(w, 2 * i + 2, 1, 5, B2, False, pc=D, would=1 if kind == "b2would" else None)
            else:                  # the landed buffer (B1, index 1) copied instead
                scan(w, 2 * i + 2, 1, B1, True, True)
                swap(w, 2 * i + 2, 1, 4, B2, False, pc=HOME, flags=64 | 32, target=1, landed=1)
        common_tail(w)
    elif kind in GEN_KINDS:     # card run 2026-10-17d: the write generations, the flip-buffer path
        gen_run(w, kind)
        common_tail(w)
    elif kind == "samebuf":   # card run 2026-10-18b: mode 0, one back: the first swap flips, the rest are not flipped
        for i in range(40):
            w.add(6, arg=1, flags=1, obj=0x1111, v=(8100, 3, 0, 0, 0), pa=B1)
            swap(w, i + 1, 0, 1, B1, True, pc=B1, bandk=i, target=4 if i else 0, waited=0 if i else 14800,
                 flags=0 if i else 128)
        common_tail(w)
    elif kind in STALE_KINDS:   # card run 2026-10-17c's head 1: B2 drawn once, then never again
        stale_run(w, kind)
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
    elif kind == "copyblack":   # mode 1 copies of a back with pixels; the scanout after them sampled empty
        for i in range(10):
            swap(w, i + 1, 1, 4, B1, True, pc=D, flags=64 | 32, target=2)
            w.add(15, 0, 0xfe, 64, 0, (i + 1, 0, 4, 0, 0), D)
        common_tail(w)
    elif kind == "gpucap":
        for i in range(10):
            swap(w, i + 1, 1, 4, B1, True, pc=D, flags=64 | 32, target=2)
            scan(w, i + 1, 2, B2, True, False)
        w.add(18, arg=0, flags=0, v=(158, 10000000, 77, 1, 0))
        common_tail(w)
    elif kind == "mc":   # the HUBP's address as an MC address (no VRAM base yet): not compared
        for i in range(10):
            swap(w, i + 1, 1, 4, B1, True, pc=D, flags=64 | 32, target=2)
            scan(w, i + 1, 2, B2, True, False)
        w.ev = [e for e in w.ev if struct.unpack_from(EV, e)[2] != 22]
        for i in range(10):
            w.add(22, 0, 0, 2, 0, (i, 0, 0, 4, 0), 0x8000000000, 0x8000000000, D)
        common_tail(w)
    elif kind in AB_KINDS:
        ab = kind not in ("clean", "nopanel", "lost", "nojoin")
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
            out += w.blob(1, 0, 500 if kind == "bootlost" else 0, p1, acc[:half], ver) + \
                w.blob(1, 0, 25 if kind == "lost" else 0, p2, acc[half:], ver,
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


AB_KINDS = ("ab", "clean", "nojoin", "bootlost", "nopanel", "lost", "v1", "version", "truncated", "corrupt", "gap", "inactive",
            "mode0never", "nomem", "short")


# the Metal plugin's trace (metal/plugin/nvmtl_trace.h, tools/plugin_trace.py), WindowServer's: kinds
# join (IOSurface 101 drawn and marked dirty every frame, 102 only cleared and never published), wrap (the
# main ring wrapped: only the last seconds kept), old (opened a year before the kernel trace's clock)
P_HDR = struct.Struct("<8s6I3Q2I4Q4I64s")
P_REC = struct.Struct("<QII4BIi7IQ4I12I")
WALL0 = 1792310400 * 10 ** 9     # the kernel fixture's wall anchor (its headers' wall_us), in ns


def make_plugin(kind):
    nmain = n = 256
    off_main, off_vk = 4096, 4096 + n * 128
    recs = []
    seq = 0
    frames = 40
    sids = ((103, 1),) if kind == "late" else ((101, 1), (102, 2))
    seq = 100000 if kind == "late" else 0      # 'late': the final copy after the ring wrapped (surface 103 only)
    for f in range(frames):
        for sid, path in sids:
            seq += 1
            recs.append(P_REC.pack(1000 + seq * 1000, seq, 1, 5, path, 0, 1, sid, 0, 2560, 1440, 80, 10240, 0, 0, 0,
                                   0, f + 1, 1, 0, 0, *([0] * 12)))                       # PASS_END
        seq += 1
        recs.append(P_REC.pack(1000 + seq * 1000, seq, 1, 3, 1, 0, 1, sids[0][0], 0, 2560, 1440, 80, 10240, 0, 0, 0, 0,
                               f + 1, 0, 0, 0, *([0] * 12)))                              # SURF_OUT dirty
    widx = len(recs)
    if kind == "late":
        widx = 200000
    if kind == "wrap":
        widx = 5000                    # most were overwritten: the ring holds the last ones only
        recs = recs[-nmain:]
    wall = WALL0 - (365 * 86400 * 10 ** 9 if kind == "old" else 0)
    h = P_HDR.pack(b"NVMTLTR1", 1, 128, nmain, n, n, 158, off_main, off_vk, off_vk + n * 128, 1, 1, 0, wall, widx, 0,
                   seq + 1, 0, 88, 28, b"WindowServer")
    body = bytearray(h.ljust(4096, b"\0")) + bytearray(3 * n * 128)
    start = 0 if kind != "wrap" else 4096 * 0
    for i, r in enumerate(recs[:nmain]):
        body[off_main + i * 128: off_main + (i + 1) * 128] = r
    return bytes(body)


if __name__ == "__main__":
    if sys.argv[1].startswith("plugin-"):
        open(sys.argv[2], "wb").write(make_plugin(sys.argv[1][len("plugin-"):]))
    else:
        open(sys.argv[2], "wb").write(make(sys.argv[1]))
