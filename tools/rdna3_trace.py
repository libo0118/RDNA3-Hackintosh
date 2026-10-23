#!/usr/bin/env python3
"""Decoder of the kernel's per-frame trace (card run 2026-10-17b,
common/rdna3_trace.h): the accelerator's ring (legacy swaps, copy forward,
shapes, VRAM, the family's aperture access, IOSurfaces, mode changes,
flips, what each HUBP scanned) and the core's (command submissions,
fences, waits, resets, GPU faults), as binary reads of
`sysctl -b debug.rdna3accel_trace[_stream|_sec]` / `debug.rdna3_drmtrace[_stream]`
(one or more blobs, each a header and its events) or "tr" lines of a
kernel log. Times are mach_absolute_time; each header carries the timebase,
so a merged decoder (the Metal plugin's tools/plugin_trace.py) can put
every source on one clock; version 2 headers also carry the read's wall
clock (anchors(), wall_of()) to line the trace up with the logs' dates.
Damaged input never raises: Trace.problems names the file and byte.

  tools/rdna3_trace.py <file>... [--timeline N] [--head H]

tools/triage_reports.py imports it: verdicts per hypothesis (H-COPY,
H-B2, H-STALE, H-GEN, H-FLIPPATH, H-FREE, copy forward errors), the A/B side by side, the coverage
self-check of docs/AFTER-TESTS.md's questions Q1-Q11 and the last events
per head.
"""
import re
import struct
import sys
import time

MAGIC = 0x52543352
VERSION = 2                           # RD3_TRACE_VERSION; version 1 (an 80-byte header) is still read
EV_FMT = "<QIBBBBIIIIIIQQQ8s8s"       # struct rd3_trace_ev (80 bytes)
HDR_FMT_V1 = "<IIIIIIQQQQIIIIII"      # struct rd3_trace_hdr of version 1 (80 bytes)
HDR_FMT = HDR_FMT_V1 + "QII"          # version 2 (96 bytes): + wall_us, flags, hdr_size
SEC_FMT = "<IBBBBHHHHHHHHHHI"         # struct rd3_trace_sec (32 bytes)
EV_SIZE, HDR_SIZE, SEC_SIZE = struct.calcsize(EV_FMT), struct.calcsize(HDR_FMT), struct.calcsize(SEC_FMT)
HDR_SIZE_V1 = struct.calcsize(HDR_FMT_V1)
assert (EV_SIZE, HDR_SIZE, HDR_SIZE_V1, SEC_SIZE) == (80, 96, 80, 32)
HDR_KEYS = ("magic", "version", "ev_size", "cap", "count", "swap_mode", "written", "lost", "now", "nomem",
            "boot_id", "source", "tb_numer", "tb_denom", "read_kind", "ab", "wall_us", "flags", "hdr_size")
HDR_HDP_NOINV, HDR_CHUNKED = 1, 2
# the swaps rdna3accel_swaps_h<N> counts (routed and presented: not skipped, held or headless)
COUNTED_SWAPS = (1, 2, 3, 4, 6)
AB_MIN_SWAPS = 20                     # swaps per head and phase for an A/B to decide anything

SRC_ACCEL, SRC_CORE = 0, 1
READ_ALL, READ_STREAM, READ_SEC = 0, 1, 2
TYPES = {0: "none", 1: "SWAP", 2: "CF", 3: "SHAPE", 4: "VMALLOC", 5: "VMFREE", 6: "APERTURE", 7: "PAGEOFF",
         8: "SURFGEM", 9: "MODE", 10: "FLIP", 11: "LATCH", 12: "SYNC", 13: "GONE", 14: "MODESET", 15: "SCAN",
         16: "CS", 17: "FENCE", 18: "GPUWAIT", 19: "RESET", 20: "VMFAULT", 21: "SURFRES", 22: "PANEL",
         23: "FLIPBUF", 24: "HOOK", 25: "PRESENT"}
T = dict((v, k) for k, v in TYPES.items())
SWAP_ARG = {0: "skip", 1: "flip", 2: "flip refused, copied", 3: "flip refused", 4: "copy", 5: "hold", 6: "copy FAILED",
            7: "no head"}
RC = {0: "no res", 1: "no vidmem", 2: "no kva", 3: "no geometry", 4: "empty", 5: "pixels"}
F_A1_FRONT, F_A2_FRONT, F_A1_PIX, F_A2_PIX, F_EMPTY, F_FRONT_OK, F_DOUBLE, F_WAITED = 1, 2, 4, 8, 16, 32, 64, 128
TARGET = {0: "-", 1: "home", 2: "D", 3: "home DIRECT",
          4: "same buffer, not flipped",   # 4: mode 0, the back already on the panel (card run 2026-10-18b)
          5: "queued behind the flip in flight"}   # 5: the present queue (DRIVER-PLAN N38)
TARGET_SAME = 4
TARGET_QUEUED = 5
V1_BRK_FLIP = 1 << 20                 # SWAP v1: modes 1/2 flipped, the SDMA breaker open
V1_LANDED = 1 << 21                   # SWAP v1: modes 1/2 presented the landed buffer (card run 2026-10-17c)
V1_LANDED_WOULD = 1 << 22             # SWAP v1: one found, not presented (debug.rdna3accel_swap_landed=0)
LANDED_A1 = 0xfe                      # SWAP v4 >> 24 with V1_LANDED(_WOULD): the landed buffer is a1 itself
V1_STALE = 1 << 23                    # SWAP v1 with arg 5: a stale back held (v3: the swap that last showed it)
V1_STALE_WOULD = 1 << 24              # SWAP v1: one would have been held, copied (debug.rdna3accel_swap_stale=0)
V1_GEN_HOLD = 1 << 25                 # SWAP v1: held by the write generations (card run 2026-10-17d; v3 the head's)
V1_GEN_WOULD = 1 << 26                # SWAP v1: the generations would have held it (swap_gen 0, or mode 2)
V1_GEN_WHY_SHIFT, V1_GEN_WHY_MASK = 28, 15   # SWAP v1: the rule's reason (rdna3_write_gen.h enum rd3_wg_why)
V1_FLIPBUF = 1 << 27                  # SWAP v1: a flip-buffer present (debug.rdna3accel_flipbuf_present=1)
GEN_WHY = {0: "not judged", 1: "newer", 2: "first after a reset", 3: "changed with no write seen",
           4: "same as the panel's", 5: "older", 6: "never written", 7: "aged (no present for 250 ms)",
           8: "a newer generation was evicted"}
GEN_PRESENTS, GEN_HOLDS = (1, 2, 3, 7, 8), (4, 5, 6)
COPY_US_CAP = 0xffff                  # SWAP v2's copy time saturates here (v2 >> 16 is the generation)
FLIPBUF_WHAT = {0: "traced", 1: "PRESENTED", 2: "held", 3: "present FAILED", 4: "no head", 5: "no VRAM resource",
                6: "lock busy"}
HOOK_KIND = {0: "pickPresentType", 1: "didSubmitSwap", 2: "isBackBufferReady", 3: "submitUpdate"}
# PRESENT (DRIVER-PLAN N38, common/rdna3_flip_queue.h): rd3_trace_present
PRESENT_ARG = {0: "programmed", 1: "queued", 2: "replaced (newest wins)", 3: "joined the flip in flight",
               4: "Busy, kept", 5: "REFUSED", 6: "latched", 7: "programmed at the latch", 8: "TIMEOUT: completed EARLY",
               9: "UNREADABLE: completed EARLY", 10: "DROPPED", 11: "writer waited",
               12: "the flip pending before it GIVEN UP", 13: "programmed WITHOUT waiting (head stuck)",
               14: "the IOP copy fallback flipped home"}
PQ_KIND = {0: "swap", 1: "flip-buffer", 2: "IOP"}
PQ_DROP_MODE = 0x80                   # PRESENT arg 10's flags: dropped at a display mode change
FLIPPATH_NS = 50 * 10 ** 6            # H-FLIPPATH: a copy forward with no swap of its surface within this
STALE_NS = 20 * 10 ** 6               # H-STALE: a stale copy follows another back's copy within this
AB_NAME = {0: "-", 1: "A: mode 1", 2: "B: mode 0", 3: "back"}


class Ev(object):
    __slots__ = ("src", "t", "seq", "type", "head", "arg", "flags", "obj", "v0", "v1", "v2", "v3", "v4", "pa", "pb",
                 "pc", "ba", "bb")

    def __init__(self, src, vals):
        (self.t, self.seq, self.type, self.head, self.arg, self.flags, self.obj, self.v0, self.v1, self.v2, self.v3,
         self.v4, self.pa, self.pb, self.pc, self.ba, self.bb) = vals
        self.src = src

    @property
    def name(self):
        return TYPES.get(self.type, "type%d" % self.type)


class Trace(object):
    """every event of every blob / log line given, merged and in time order
    (an event read twice, by the full read and a stream read, counts once)"""

    def __init__(self):
        self.events = []
        self.headers = []
        self.secs = []
        self.tb = {}            # source -> (numer, denom)
        self.lines = 0          # events from "tr" lines
        self.problems = []      # (level, question, text): what the files say about their own completeness
        self._seen = set()

    def problem(self, level, q, text):
        self.problems.append((level, q, text))

    def add_event(self, src, vals):
        ev = Ev(src, vals)
        key = (src, ev.seq, ev.t)
        if key in self._seen:
            return
        self._seen.add(key)
        self.events.append(ev)

    def add_blob(self, data, where=""):
        """parses one file's bytes: binary blobs back to back, else a 'sysctl -x'
        hex dump. Never raises on garbage: what cannot be decoded is a problem
        (FAIL, Q8) naming the file and the byte where decoding stopped."""
        if not data:
            self.problem("FAIL", "Q8", "%s: empty" % (where or "a trace file"))
            return 0
        if len(data) < 4 or struct.unpack_from("<I", data, 0)[0] != MAGIC:
            raw = data
            hx = re.findall(r"0x([0-9a-fA-F]{16,})", data.decode("latin-1", "replace"))
            try:
                data = bytes.fromhex("".join(hx)) if hx else b""
            except ValueError:
                data = b""
            if not data:
                self.problem("FAIL", "Q8", "%s: NOT a trace (no R3TR header; it starts %r, %d bytes; sysctl -b "
                                           "printed text, or the file is not binary)" %
                             (where or "a trace file", bytes(raw[:24]), len(raw)))
                return 0
        off = 0
        n = 0
        blobs = 0
        while off < len(data):
            rem = len(data) - off
            if rem < 8:
                self.problem("FAIL", "Q8", "%s: %d trailing byte(s) at %d after %d blob(s): a cut-off read or upload"
                             % (where, rem, off, blobs))
                break
            magic, ver = struct.unpack_from("<II", data, off)
            if magic != MAGIC:
                self.problem("FAIL", "Q8", "%s: no R3TR header at byte %d (after %d blob(s)): %d byte(s) NOT decoded "
                                           "(a corrupt or interleaved read)" % (where, off, blobs, rem))
                break
            if ver not in (1, 2):
                self.problem("FAIL", "Q8", "%s: trace version %d at byte %d (this decoder reads 1-%d): %d byte(s) "
                                           "NOT decoded - decode with the tools of the kernel's sources"
                             % (where, ver, off, VERSION, rem))
                break
            hsize = HDR_SIZE if ver >= 2 else HDR_SIZE_V1
            if rem < hsize:
                self.problem("FAIL", "Q8", "%s: a header cut off at byte %d (%d of %d bytes)" % (where, off, rem, hsize))
                break
            h = struct.unpack_from(HDR_FMT if ver >= 2 else HDR_FMT_V1, data, off)
            hdr = dict(zip(HDR_KEYS, h))
            hdr.setdefault("wall_us", 0)
            hdr.setdefault("flags", 0)
            hdr.setdefault("hdr_size", hsize)
            if hdr["hdr_size"] != hsize:
                self.problem("FAIL", "Q8", "%s: header size %d at byte %d (version %d has %d): NOT decoded further"
                             % (where, hdr["hdr_size"], off, ver, hsize))
                break
            hdr["where"] = where
            hdr["order"] = len(self.headers)
            off += hsize
            want = SEC_SIZE if hdr["read_kind"] == READ_SEC else EV_SIZE
            size = hdr["ev_size"] * hdr["count"]
            if hdr["ev_size"] != want:
                self.problem("FAIL", "Q8", "%s: blob at byte %d has %d-byte records (this decoder reads %d): its %d "
                                           "record(s) skipped" % (where, off - hsize, hdr["ev_size"], want, hdr["count"]))
                off += size
                hdr["truncated"] = False
                hdr["skipped"] = True
                self.headers.append(hdr)
                blobs += 1
                continue
            body = data[off:off + size]
            hdr["truncated"] = len(body) < size
            if hdr["truncated"]:
                self.problem("FAIL", "Q8", "%s: blob at byte %d TRUNCATED: %d of %d record(s) (an upload or read cut "
                                           "off)" % (where, off - hsize, len(body) // want, hdr["count"]))
            off += size
            self.headers.append(hdr)
            blobs += 1
            if hdr["tb_numer"] and hdr["tb_denom"]:
                self.tb[hdr["source"]] = (hdr["tb_numer"], hdr["tb_denom"])
            if hdr["read_kind"] == READ_SEC:
                for i in range(len(body) // SEC_SIZE):
                    v = struct.unpack_from(SEC_FMT, body, i * SEC_SIZE)
                    self.secs.append(dict(zip(("sec", "head", "mode", "ab", "pad", "swaps", "shown", "held", "empty",
                                               "black", "cf", "cferr", "aperture", "vmrefused", "freedshown",
                                               "events"), v)))
            else:
                seqs = []
                for i in range(len(body) // EV_SIZE):
                    vals = struct.unpack_from(EV_FMT, body, i * EV_SIZE)
                    seqs.append(vals[1])
                    self.add_event(hdr["source"], vals)
                    n += 1
                hdr["seqs"] = (seqs[0], seqs[-1]) if seqs else None
                hdr["contiguous"] = all(((b2 - a2) & 0xffffffff) == 1 for a2, b2 in zip(seqs, seqs[1:]))
        return n

    TR = re.compile(r"\btr (\d+) (\d+) (\d+) (\d+) (\d+) (\d+) ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+) "
                    r"([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+)")

    DUMP_START = re.compile(r"trace: events (\d+)\.\.(\d+) \(")
    DUMP_DONE = re.compile(r"trace: dump done, (\d+) line\(s\)")

    def check_dumps(self, text, where="the kernel log"):
        """each kernel log dump ("trace: events A..B" ... "trace: dump done, N
        line(s)") must hold its N "tr" lines: a log that dropped some (log
        show under load, a serial line cut) is a problem"""
        inside, count, missing, dumps = False, 0, 0, 0
        for line in text.splitlines():
            if self.DUMP_START.search(line):
                inside, count = True, 0
            elif self.DUMP_DONE.search(line):
                want = int(self.DUMP_DONE.search(line).group(1))
                dumps += 1
                if inside and count < want:
                    missing += want - count
                elif not inside:
                    missing += want
                inside = False
            elif inside and self.TR.search(line):
                count += 1
        if missing:
            self.problem("WARN", "Q8", "%s: %d 'tr' line(s) of %d dump(s) missing (the log dropped them)" %
                         (where, missing, dumps))
        return missing

    def add_lines(self, text):
        n = 0
        for m in self.TR.finditer(text):
            g = m.groups()
            src, seq, t = int(g[0]), int(g[1]), int(g[2])
            ty, head, arg = int(g[3]), int(g[4]), int(g[5])
            flags, obj, v0, v1, v2, v3, v4 = (int(x, 16) for x in g[6:13])
            pa, pb, pc, ba, bb = (int(x, 16) for x in g[13:18])
            self.add_event(src, (t, seq, ty, head, arg, flags, obj, v0, v1, v2, v3, v4, pa, pb, pc,
                                 struct.pack("<Q", ba & (2 ** 64 - 1)), struct.pack("<Q", bb & (2 ** 64 - 1))))
            n += 1
        self.lines += n
        return n

    def finish(self):
        self.events.sort(key=lambda e: (e.t, e.src, e.seq))
        self._check_streams()
        # version 2 headers carry the boot's random id; version 1 a salt check
        # (the same field): either way two values are two boots
        boots = set(h["boot_id"] for h in self.headers if h["source"] == SRC_ACCEL and h["boot_id"])
        if len(boots) > 1:
            self.problem("FAIL", "Q8", "accelerator blobs of %d different boots (boot id %s): files of two reports "
                                       "mixed" % (len(boots), ", ".join("%08x" % x for x in sorted(boots))))
        for h in self.headers:
            if h["nomem"]:
                self.problem("FAIL", "Q8", "%s: %d event(s) NOT recorded (no memory for the %s ring)" %
                             (h["where"], h["nomem"], "accelerator's" if h["source"] == SRC_ACCEL else "core's"))
                break
        return self

    def _check_streams(self):
        """each file's stream reads must follow one another: read k covers the
        events after read k-1's 'written', so written_k - written_k-1 = count_k +
        lost_k; less is a read missing from the file (another reader, a lost
        upload), more an overlap; the events of a read are consecutive and end
        at its 'written'"""
        groups = {}
        for h in self.headers:
            if h["read_kind"] == READ_STREAM and not h.get("skipped"):
                groups.setdefault((h["where"], h["source"]), []).append(h)
        for (where, src), hs in groups.items():
            name = "core" if src == SRC_CORE else "accelerator"
            for prev, cur in zip(hs, hs[1:]):
                d = cur["written"] - prev["written"]
                got = cur["count"] + cur["lost"]
                if d != got and not cur["truncated"]:
                    self.problem("FAIL", "Q8", "%s (%s stream): %d event(s) between two reads %s (written %d -> %d, "
                                               "this read %d + %d lost)" %
                                 (where, name, abs(d - got), "MISSING from the file (another stream reader, or a "
                                  "read not saved)" if d > got else "read twice (two stream readers)",
                                  prev["written"], cur["written"], cur["count"], cur["lost"]))
            for h in hs:
                sq = h.get("seqs")
                if sq and (not h.get("contiguous") or sq[1] != h["written"] & 0xffffffff) and not h["truncated"]:
                    self.problem("FAIL", "Q8", "%s (%s stream): a read's events are not consecutive up to its "
                                               "'written' (%d..%d, written %d)" % (where, name, sq[0], sq[1],
                                                                                   h["written"]))

    def ns(self, ev_or_t, src=SRC_ACCEL):
        t = ev_or_t.t if isinstance(ev_or_t, Ev) else ev_or_t
        num, den = self.tb.get(src, (1, 1))
        return t * num // den

    def by(self, name, src=None):
        ty = T[name]
        return [e for e in self.events if e.type == ty and (src is None or e.src == src)]


def load(paths, texts=()):
    tr = Trace()
    for p in paths:
        with open(p, "rb") as f:
            tr.add_blob(f.read(), p)
    for t in texts:
        tr.add_lines(t)
        tr.check_dumps(t)
    return tr.finish()


# ---------------------------------------------------------------- analysis

def swap_back(e):
    """(back phys, back rc, front phys, front rc) of a SWAP: the argument that
    is a head's framebuffer resource is the front, else a1 (submitSwapCopy)"""
    rc1, rc2 = e.v1 & 0xf, (e.v1 >> 4) & 0xf
    if e.flags & F_A2_FRONT and not e.flags & F_A1_FRONT:
        return e.pa, rc1, e.pb, rc2, e.ba, e.bb
    return e.pb, rc2, e.pa, rc1, e.bb, e.ba


def swap_events(trace):
    """the legacy swaps (submitSwapCopy): the flip-buffer presents (SWAP with V1_FLIPBUF, card run 2026-10-17d) are
    not swaps of a surface and stay out of H-B2, H-COPY, the A/B and the coverage counts"""
    return [e for e in trace.by("SWAP") if not e.v1 & V1_FLIPBUF]


def copy_us_text(e):
    """a SWAP's copy time: v2's low 16 bits, saturated at 65535 us (the high 16 bits are the generation)"""
    us = e.v2 & COPY_US_CAP
    return ">=%d us (capped)" % us if us == COPY_US_CAP else "%d us" % us


def swap_mode(e):
    return (e.v1 >> 12) & 0xf


def _scans_by_swap(trace):
    """{(head, swap number): [SCAN of the surface's other buffers at that swap]}
    (not the delayed re-samples, copy forward's targets or the samples after a
    copy), built once per trace"""
    idx = getattr(trace, "_scans_by_swap", None)
    if idx is None:
        idx = {}
        for s in trace.by("SCAN"):
            if s.arg < 0xfd and not s.flags & (16 | 32 | 64 | 128):
                idx.setdefault((s.head, s.v0), []).append(s)
        trace._scans_by_swap = idx
    return idx


def swap_landed(trace, e):
    """the landed buffer a mode 1/2 SWAP presented instead of the back it
    named (card run 2026-10-17c): its address; 0 when the swap did not, None
    when no SCAN of that swap names the buffer's index"""
    if not e.v1 & (V1_LANDED | V1_LANDED_WOULD):
        return 0
    i = (e.v4 >> 24) & 0xff
    if i == LANDED_A1:
        return e.pa
    for s in _scans_by_swap(trace).get((e.head, e.v0), ()):
        if s.arg == i:
            return s.pa
    return None


def new_frame_elsewhere(trace, e):
    """the surface's other buffers that had new pixels at a swap (sampled at
    it: pixels and changed since the head's last look), minus the back it
    named: [address] - a frame that swap could have presented"""
    back = swap_back(e)[0]
    return [s.pa for s in _scans_by_swap(trace).get((e.head, e.v0), ()) if s.flags & 3 == 3 and s.pa != back]


def phase_of(trace):
    """each accelerator event's A/B phase (MODESET arg 1 events; before the
    first one: 0)"""
    cur = 0
    out = {}
    for e in trace.events:
        if e.src == SRC_ACCEL and e.type == T["MODESET"] and e.arg == 1:
            cur = e.v1
        out[id(e)] = cur
    return out


def ab_info(trace):
    """the A/B as the trace saw it: {"ran": bool, "set": phases a MODESET set,
    "seen": phases any record carries (events, headers, per-second rows)}"""
    ms = set(e.v1 for e in trace.events if e.src == SRC_ACCEL and e.type == T["MODESET"] and e.arg == 1)
    seen = set(ms) | set(h["ab"] for h in trace.headers if h["source"] == SRC_ACCEL) | set(r["ab"] for r in trace.secs)
    seen.discard(0)
    return {"ran": bool(seen & set((1, 2))), "set": ms, "seen": seen}


def ab_events(trace):
    """the swaps the A/B judges: those of phases 1 and 2 when the A/B ran,
    else all of them (and whether it was restricted)"""
    info = ab_info(trace)
    swaps = swap_events(trace)
    if not info["ran"]:
        return swaps, False
    ph = phase_of(trace)
    return [e for e in swaps if ph.get(id(e), 0) in (1, 2)], True


def ab_validity(trace, expected=False, min_swaps=AB_MIN_SWAPS):
    """[(level, text)]: whether the A/B can decide anything - it ran (when
    'expected'), every phase was labelled, each head that swapped had at
    least min_swaps swaps in both phases, phase 1 ran the copy and phase 2
    the flip"""
    info = ab_info(trace)
    out = []
    if not info["ran"]:
        if expected:
            out.append(("FAIL", "A/B did not run: no phase 1 / 2 in the trace (no MODESET, no header or per-second "
                                "row labelled) although this run should have had it - the tests file's '== swap A/B' "
                                "lines say why"))
        return out
    for p in (1, 2, 3):
        if p not in info["set"]:
            out.append(("FAIL", "A/B inconclusive: no MODESET to phase %d in the trace (the label was never set, or "
                                "its event was lost)" % p))
    ph = phase_of(trace)
    per = {}
    modes = {}
    for e in swap_events(trace):
        p = ph.get(id(e), 0)
        if p not in (1, 2) or e.head == 0xff:
            continue
        per.setdefault(e.head, {1: 0, 2: 0})[p] += 1
        modes.setdefault(p, {})
        m = "brk" if e.v1 & V1_BRK_FLIP else swap_mode(e)
        modes[p][m] = modes[p].get(m, 0) + 1
    if not per:
        out.append(("FAIL", "A/B inconclusive: no swap on any head in phases 1 and 2 (nothing redrew: Activity "
                            "Monitor not running?)"))
    others = sorted(set(e.head for e in swap_events(trace) if e.head != 0xff) - set(per))
    for h in others:
        out.append(("INFO", "A/B: head %d had no swap in phases 1 and 2 (nothing redrew on its display): not judged" % h))
    for h, c in sorted(per.items()):
        for p in (1, 2):
            if c[p] < min_swaps:
                out.append(("FAIL", "A/B inconclusive: phase %d had %d swaps on head %d (need %d)" %
                            (p, c[p], h, min_swaps)))
    m1, m2 = modes.get(1, {}), modes.get(2, {})
    if m1 and not (m1.get(1, 0) + m1.get(2, 0)):
        out.append(("FAIL", "A/B inconclusive: mode 1 never applied in phase 1 (its swaps ran as %s)" %
                    ", ".join("%s: %d" % (k if k == "brk" else "mode %d" % k, v) for k, v in sorted(m1.items(), key=str))))
    elif m1.get("brk"):
        out.append(("WARN", "A/B: %d of phase 1's swaps flipped because the SDMA breaker was open" % m1["brk"]))
    if m2 and not m2.get(0):
        out.append(("FAIL", "A/B inconclusive: mode 0 never applied in phase 2 (its swaps ran in %s)" %
                    ", ".join("mode %s: %d" % (k, v) for k, v in sorted(m2.items(), key=str))))
    return out


def per_head_buffers(trace, swaps=None):
    """{head: {back phys: [swaps, empty, pixels, aperture hits]}} and the aperture count per phys"""
    ap = {}
    for e in trace.by("APERTURE"):
        if e.arg == 1:
            ap[e.pa] = ap.get(e.pa, 0) + 1
    heads = {}
    for e in (swap_events(trace) if swaps is None else swaps):
        if e.head == 0xff:
            continue
        back, rcb = swap_back(e)[:2]
        if not back:
            continue
        st = heads.setdefault(e.head, {}).setdefault(back, [0, 0, 0])
        st[0] += 1
        st[1] += rcb == 4
        st[2] += rcb == 5
    return heads, ap


def inplace_rewrites(swaps):
    """mode 0: the buffer a flip showed, changed by the next swap's entry and
    shown again by it (WindowServer drew into the buffer on the panel);
    returns (in-place, checked)"""
    last = {}
    inpl = chk = 0
    for e in swaps:
        if e.arg not in (1,):   # flips only
            continue
        back, _, _, _, bband, _ = swap_back(e)
        prev = last.get(e.head)
        if prev and prev[0] == back:
            chk += 1
            if prev[1] != bband:
                inpl += 1
        last[e.head] = (e.pc, bband) if e.pc == back else (e.pc, None)
    return inpl, chk


def segments(trace, swaps=None):
    """the swaps per (swap mode, A/B phase): their counts side by side"""
    ph = phase_of(trace)
    seg = {}
    for e in (swap_events(trace) if swaps is None else swaps):
        k = (swap_mode(e), ph.get(id(e), 0))
        s = seg.setdefault(k, {"swaps": 0, "shown": 0, "held": 0, "black": 0, "empty": 0, "failed": 0, "direct": 0,
                               "double": 0, "waits": 0, "wait_us": 0, "copy_us": 0, "events": [], "landed": 0,
                               "held_new": 0, "would": 0, "stale": 0, "same": 0})
        s["swaps"] += 1
        shown = e.arg in (1, 2, 4)
        s["shown"] += shown
        s["held"] += e.arg in (0, 5)
        # card run 2026-10-17c: the landed buffer presented, and holds while another buffer had a new frame
        s["landed"] += e.arg == 4 and bool(e.v1 & V1_LANDED)
        s["held_new"] += e.arg in (0, 5) and not e.v1 & V1_STALE and bool(new_frame_elsewhere(trace, e))
        s["stale"] += e.arg == 5 and bool(e.v1 & V1_STALE)   # a back not redrawn since shown, held (2026-10-17c)
        s["would"] += bool(e.v1 & V1_LANDED_WOULD)   # found, not presented (debug.rdna3accel_swap_landed=0)
        s["empty"] += bool(e.flags & F_EMPTY)
        s["black"] += bool(e.flags & F_EMPTY) and shown
        s["failed"] += e.arg in (3, 6)
        s["direct"] += e.arg == 4 and ((e.v1 >> 16) & 0xf) == 3
        s["same"] += e.arg == 1 and ((e.v1 >> 16) & 0xf) == TARGET_SAME   # mode 0: not flipped again, no wait
        s["double"] += bool(e.flags & F_DOUBLE)
        if e.flags & F_WAITED:
            s["waits"] += 1
            s["wait_us"] += e.v3
        s["copy_us"] += e.v2 & 0xffff   # v2 >> 16: the back's write generation (card run 2026-10-17d)
        s["events"].append(e)
    for s in seg.values():
        s["inplace"], s["inplace_checked"] = inplace_rewrites(s["events"])
    return seg


# VMALLOC / VMFREE flags of DRIVER-PLAN N41 (common/rdna3_trace.h): the parked allocation handed back (VMALLOC),
# parked instead of freed (VMFREE); a parked allocation released; a live one released by free()
VM_PARKED = 16
VM_PARK_GONE = 32
VM_FREE_LIVE = 64
VM_OBJ_GONE = 128   # VMFREE from RD3VidMemory::free(): the object goes, its address may come back (ECC review of N41)
VM_V2_NOBO = 1      # v2 of a VMFREE with VM_PARK_GONE: no BO held it any more (pressure / cap; security review of N41)
# PAGEOFF flags (N41): waited for WindowServer's pending GPU write, that wait timed out, cleared under the wait; arg 2
# with PAGEOFF_IOP: the IOP zero-copy flip's wait (no page-off; v2 the surface)
PAGEOFF_WAITED = 1
PAGEOFF_WAIT_TIMEOUT = 2
PAGEOFF_IOP = 4
PAGEOFF_WAIT_CLEARED = 8
PAGEOFF_WAIT_SKIPPED = 16   # pending, not waited: the timeout budget was spent (security review of N41)


def vram_moves(trace):
    """DRIVER-PLAN N41 (traces 2026-10-17d / 17c): the vidmem of a surface imported as a BO (SURFGEM arg 1) given
    new VRAM at another address after a free, and the dirty marks (SURFGEM arg 2) on such moved VRAM - the plugin's
    BO keeps the first address. Also the parks seen. {"imported", "freed", "moved": [(obj, surface, was, now)],
    "dirty_moved", "parked", "reused", "park_released", "free_live", "freed_plain", "obj_gone", "park_nobo"}. A vidmem's
    VMFREE with VM_OBJ_GONE (free()) ends that object: a new vidmem at the same address starts afresh (ECC review
    of N41: no H-VMMOVE for a reused object address after a real free). freed_plain: VMFREEs of non-scanout VRAM
    without any park flag (vm_sticky 0, never BO-imported, or a kernel before N41). park_nobo: parks released because
    no BO held them any more (VM_V2_NOBO: VRAM ran out or past the cap): the import ends there, no H-VMMOVE after"""
    imported, state, life, ever = {}, {}, {}, set()
    out = {"imported": 0, "freed": 0, "moved": [], "dirty_moved": 0, "parked": 0, "reused": 0, "park_released": 0,
           "free_live": 0, "freed_plain": 0, "obj_gone": 0, "park_nobo": 0}

    for e in trace.events:
        if e.src != SRC_ACCEL:
            continue
        n = TYPES.get(e.type)
        if n == "SURFGEM" and e.arg == 1 and e.obj:
            imported[e.obj] = (e.pa, e.v0)
            ever.add((e.obj, life.get(e.obj, 0)))
            state.pop(e.obj, None)
        elif n == "VMFREE":
            if e.flags & VM_PARKED:
                out["parked"] += 1
            if e.flags & VM_PARK_GONE:
                out["park_released"] += 1
            if e.flags & VM_FREE_LIVE:
                out["free_live"] += 1
            if not e.flags & (1 | VM_PARKED | VM_PARK_GONE | VM_FREE_LIVE | VM_OBJ_GONE):
                out["freed_plain"] += 1
            if e.flags & VM_OBJ_GONE:   # the object goes: its address is a new object's from now on
                out["obj_gone"] += 1
                imported.pop(e.obj, None)
                state.pop(e.obj, None)
                life[e.obj] = life.get(e.obj, 0) + 1
                continue
            if e.flags & VM_PARK_GONE and e.v2 == VM_V2_NOBO:   # no BO holds it: the import is over
                out["park_nobo"] += 1
                imported.pop(e.obj, None)
                state.pop(e.obj, None)
                continue
            if e.obj in imported and not e.flags & VM_PARKED:
                out["freed"] += 1
                state[e.obj] = "freed"
        elif n == "VMALLOC" and e.arg:
            if e.flags & VM_PARKED:
                out["reused"] += 1
            if e.obj in imported and state.get(e.obj) == "freed":
                state[e.obj] = "moved" if e.pa != imported[e.obj][0] else None
                if e.pa != imported[e.obj][0]:
                    out["moved"].append((e.obj, imported[e.obj][1], imported[e.obj][0], e.pa))
        elif n == "SURFGEM" and e.arg == 2 and state.get(e.obj) == "moved" and e.pa != imported[e.obj][0]:
            out["dirty_moved"] += 1
    out["imported"] = len(ever)
    return out


def h_vram_moved(trace):
    """H-VMMOVE (DRIVER-PLAN N41): rows for hypotheses()"""
    m = vram_moves(trace)
    rows = []
    if m["moved"]:
        o = m["moved"][0]
        rows.append(("WARN", "H-VMMOVE", "%d imported surface(s) got new VRAM at another address after a free (e.g. "
                     "surface %d: 0x%x -> 0x%x), %d dirty mark(s) on the moved VRAM - the plugin draws into the old "
                     "one: grey / stale / black windows (vm_sticky 0, or a resource that grew)" % (
                         len(m["moved"]), o[1], o[2], o[3], m["dirty_moved"])))
    if m["parked"] or m["reused"]:
        rows.append(("INFO", "H-VMPARK", "%d allocation(s) parked, %d handed back, %d parked one(s) released, %d "
                     "imported surface(s) never moved" % (m["parked"], m["reused"], m["park_released"],
                                                          m["imported"] - len(set(x[0] for x in m["moved"])))))
    if m["free_live"]:
        rows.append(("INFO", "H-VMPARK", "%d vidmem object(s) freed with VRAM still live (released in free(); "
                     "before N41 those leaked)" % m["free_live"]))
    if m["freed_plain"]:   # ECC review of N41: frees without parking are said, not left to the counters
        rows.append(("WARN" if m["freed"] else "INFO", "H-VMPARK",
                     "%d VRAM allocation(s) freed without parking (no BO imported over them - only a BO's allocation "
                     "is parked since the security review of N41 -, vm_sticky 0, or a kernel before N41)%s" % (
                         m["freed_plain"], ", %d of them an imported surface's (the plugin keeps drawing into the "
                         "old VRAM)" % m["freed"] if m["freed"] else "")))
    po = trace.by("PAGEOFF")
    for iop, who in ((False, "page-off"), (True, "IOP flip")):
        evs = [e for e in po if bool(e.flags & PAGEOFF_IOP) == iop and (not iop or e.arg == 2)]
        waited = [e for e in evs if e.flags & PAGEOFF_WAITED]
        late = [e for e in evs if e.flags & PAGEOFF_WAIT_TIMEOUT]
        cleared = [e for e in evs if e.flags & PAGEOFF_WAIT_CLEARED]
        skipped = [e for e in evs if e.flags & PAGEOFF_WAIT_SKIPPED]
        if skipped:
            rows.append(("WARN", "H-SURFWAIT", "%d %s(s) did NOT wait for WindowServer's pending GPU write: the timeout "
                         "budget was spent (waits in a row timed out with no ready mark; first at %s) - that %s may "
                         "be stale" % (len(skipped), who, skipped[0].seq, "copy" if not iop else "scan-out")))
        if late:
            rows.append(("WARN", "H-SURFWAIT", "%d %s(s) gave up waiting for WindowServer's GPU write (first at %s, "
                         "%d us) - a lost ready mark or a hung GPU; that %s may be stale" % (
                             len(late), who, late[0].seq, late[0].v3, "copy" if not iop else "scan-out")))
        if cleared:
            rows.append(("WARN", "H-SURFWAIT", "%d %s(s) had their wait cleared under them (another wait's timeout, a "
                         "full table or the switch-off; first at %s) - not a finished write" % (
                             len(cleared), who, cleared[0].seq)))
        if waited:
            rows.append(("INFO", "H-SURFWAIT", "%d %s(s) waited for WindowServer's GPU write (longest %d us)" % (
                len(waited), who, max(e.v3 for e in waited))))
    return rows


def hypotheses(trace, ctx=None):
    """[(level, name, text)]: CRITICAL / FAIL / WARN / INFO / PASS, the
    hypothesis each trace pattern names"""
    out = []
    swaps, restricted = ab_events(trace)   # the A/B's phases 1 and 2 when it ran
    heads, ap = per_head_buffers(trace, swaps)
    # H-B2: a back the swaps name that never receives WindowServer's frame
    for h, bufs in sorted(heads.items()):
        drawn = [(p, st) for p, st in bufs.items() if st[2] * 2 >= st[0] and st[0] >= 2]
        for p, st in sorted(bufs.items()):
            if st[0] >= 10 and st[1] >= 0.9 * st[0] and drawn:
                out.append(h_b2(trace, swaps, h, p, st, drawn, ap))
    # H-COPY: mode 0 shows the buffer WindowServer draws into
    seg = segments(trace, swaps)
    for (mode, phase), s in sorted(seg.items()):
        if mode != 0:
            continue
        if s["inplace_checked"] >= 20 and s["inplace"] >= 0.25 * s["inplace_checked"]:
            # WARN since card run 2026-10-17d: the bisect found mode 1 (the copy) broke both monitors and mode 0
            # right on both, so the in-place rewrites are no longer a failure by themselves: judge by the looks
            out.append(("WARN", "H-COPY", "mode 0%s: %d of %d flipped buffers were rewritten while on the panel and shown "
                        "again (WindowServer treats the back as free when the swap returns) - the picture is judged "
                        "by the looks: mode 1 (the copy) broke both monitors in the 2026-10-17d bisect, mode 0 was "
                        "right" % (" (" + AB_NAME.get(phase, str(phase)) + ")" if phase else "",
                                   s["inplace"], s["inplace_checked"])))
        elif s["inplace"]:
            out.append(("WARN", "H-COPY", "mode 0: %d of %d flipped buffers rewritten in place" %
                        (s["inplace"], s["inplace_checked"])))
    # H-FREE: VRAM freed while a head scanned it (a parked allocation, VMFREE flag 16, is not freed: N41)
    freed = [e for e in trace.by("VMFREE") if e.flags & 0x0e and not e.flags & VM_PARKED]
    if freed:
        out.append(("WARN", "H-FREE", "%d buffer(s) freed while a head scanned them (e.g. 0x%x on head %s) - the panel "
                    "reads VRAM that may be reused" % (len(freed), freed[0].pa, freed[0].head if freed[0].head != 0xff
                                                       else "?")))
    out.extend(h_vram_moved(trace))
    # copy forward answered with an error: CoreDisplay switches it off for good
    cferr = [e for e in trace.by("CF") if e.v4]
    if cferr:
        out.append(("CRITICAL", "CF-ERROR", "%d copy forward(s) answered an error to CoreDisplay (first 0x%x, seq %d): "
                    "CoreDisplay disables copy forward for the session" % (len(cferr), cferr[0].v4, cferr[0].seq)))
    refused = [e for e in trace.by("VMALLOC") if e.arg == 0]
    if refused:
        out.append(("WARN", "VRAM", "%d VRAM allocation(s) refused (first 0x%x, %d KiB): the family's buffers then "
                    "have no VRAM (copy forward / page-on fail inside the family)" % (len(refused), refused[0].v4,
                                                                                     refused[0].v0)))
    # the panel scanning another buffer than the swap expected
    # MC addresses (flag 2: no VRAM base yet, or below the aperture) are not comparable
    mism = [e for e in trace.by("PANEL") if e.pc and e.pa and not e.flags & 3 and e.pa != e.pc]
    if mism:
        out.append(("WARN", "PANEL", "%d swap(s) found the HUBP scanning 0x%x instead of 0x%x (first) - the panel "
                    "is not where the driver thinks" % (len(mism), mism[0].pa, mism[0].pc)))
    faults = trace.by("VMFAULT")
    if faults:
        out.append(("WARN", "GPU", "%d GPU page fault(s) (first VMID %d at 0x%x, pid %d)" %
                    (len(faults), faults[0].v0, faults[0].pa, faults[0].v2)))
    capped = [e for e in trace.by("GPUWAIT") if e.v3 == 1]
    if capped:
        out.append(("WARN", "GPU", "%d GPU wait(s) ran out the 10 s cap (a hang: first IP %d seq %d, pid %d) - the "
                    "plugin's fence waits around them time out too" % (len(capped), capped[0].arg, capped[0].v2,
                                                                       capped[0].v0)))
    out.extend(copy_black(trace, swaps))
    out.extend(h_stale(trace, ctx))
    out.extend(h_gen(trace, ctx))
    out.extend(h_flippath(trace, ctx))
    out.extend(h_present(trace))
    resets = trace.by("RESET")
    if resets:
        out.append(("WARN", "GPU", "%d hang recover%s (first IP %d seq %d)" % (len(resets), "y" if len(resets) == 1
                                                                              else "ies", resets[0].arg, resets[0].v2)))
    return out


def h_present(trace):
    """[(level, "H-PRESENT", text)]: the present queue (DRIVER-PLAN N38) per head - presents programmed at once,
    queued, replaced (newest wins), Busy kept, joined; latches seen (average, max); writers that waited for the
    buffer leaving the panel. FAIL a completion before its latch (TIMEOUT / UNREADABLE: the panel may still have read
    the buffer WindowServer had back) and a frame lost with no newer one (DROPPED but at a mode change, REFUSED when
    programmed from the queue); WARN a writer whose wait ran out and a refusal at the present (copied instead).
    No PRESENT event: [] (swap_async 0, or an older accelerator)."""
    evs = trace.by("PRESENT")
    if not evs:
        return []
    out = []
    per = {}
    for e in evs:
        per.setdefault(e.head, []).append(e)
    for h, es in sorted(per.items()):
        n = {}
        for e in es:
            n[e.arg] = n.get(e.arg, 0) + 1
        lat = [e.v1 for e in es if e.arg == 6]
        waits = [e for e in es if e.arg == 11]
        wto = [e for e in waits if e.flags & 4]
        kinds = {}
        for e in es:
            if e.arg in (0, 1, 2, 3):
                kinds[PQ_KIND.get(e.flags & 0x7f, e.flags)] = kinds.get(PQ_KIND.get(e.flags & 0x7f, e.flags), 0) + 1
        hs = h if h != 0xff else "?"
        out.append(("INFO", "H-PRESENT", "head %s: %d present(s) (%s): %d programmed at once, %d queued (%d replaced: "
                    "newest wins, %d Busy kept), %d joined; %d latch(es) seen (avg %d us, max %d us), %d programmed "
                    "at a latch; %d writer wait(s) (max %d us)" % (
                        hs, sum(n.get(a, 0) for a in (0, 1, 2, 3)),
                        ", ".join("%s %d" % (k, v) for k, v in sorted(kinds.items())) or "-",
                        n.get(0, 0), n.get(1, 0) + n.get(2, 0) + n.get(4, 0), n.get(2, 0), n.get(4, 0), n.get(3, 0),
                        len(lat), sum(lat) // len(lat) if lat else 0, max(lat) if lat else 0, n.get(7, 0),
                        len(waits), max([e.v1 for e in waits] or [0]))))
        early = [e for e in es if e.arg in (8, 9)]
        if early:
            out.append(("FAIL", "H-PRESENT", "head %s: %d completion(s) BEFORE the latch was seen (%d not latched in "
                        "time, %d unreadable; first: present #%d at 0x%x, seq %d) - the panel may still have read the "
                        "buffer WindowServer had back" % (hs, len(early), n.get(8, 0), n.get(9, 0), early[0].v0,
                                                          early[0].pa, early[0].seq)))
        lost = [e for e in es if (e.arg == 10 and not e.flags & PQ_DROP_MODE) or (e.arg == 5 and e.v2)]
        if lost:
            out.append(("FAIL", "H-PRESENT", "head %s: %d frame(s) LOST with no newer one (%d dropped: buffer freed, "
                        "%d refused when programmed from the queue; first: present #%d at 0x%x, seq %d)" % (
                            hs, len(lost), sum(1 for e in lost if e.arg == 10), sum(1 for e in lost if e.arg == 5),
                            lost[0].v0, lost[0].pa, lost[0].seq)))
        moded = [e for e in es if e.arg == 10 and e.flags & PQ_DROP_MODE]
        if moded:
            out.append(("INFO", "H-PRESENT", "head %s: %d queued present(s) dropped at a display mode change" %
                        (hs, len(moded))))
        if wto:
            out.append(("WARN", "H-PRESENT", "head %s: %d writer wait(s) ran out (a copy forward / page-on wrote the "
                        "buffer while the panel may still read it; first 0x%x, seq %d)" % (hs, len(wto), wto[0].pa,
                                                                                          wto[0].seq)))
        fto, gup = [e for e in es if e.arg == 12], [e for e in es if e.arg == 13]
        if fto or gup:
            out.append(("WARN", "H-PRESENT", "head %s: the head's flips stayed pending: %d pending flip(s) given up, "
                        "%d present(s) programmed without waiting for them (blanked, DPMS off, a stuck HUBP; first "
                        "at 0x%x, seq %d)" % (hs, len(fto), len(gup), (fto or gup)[0].pa, (fto or gup)[0].seq)))
        home = [e for e in es if e.arg == 14]
        if home:
            out.append(("INFO", "H-PRESENT", "head %s: the IOP copy fallback flipped home %d time(s) over the queue "
                        "(%d in-flight completed, %d queued dropped)" % (hs, len(home), sum(1 for e in home if e.v0),
                                                                          sum(1 for e in home if e.v2))))
        ref = [e for e in es if e.arg == 5 and not e.v2]
        if ref:
            out.append(("WARN", "H-PRESENT", "head %s: %d present(s) refused by the framebuffer at the present (copied "
                        "onto its scanout instead; first 0x%x, IOReturn 0x%x)" % (hs, len(ref), ref[0].pa,
                                                                                   ref[0].v4)))
    return out


def h_b2(trace, swaps, h, p, st, drawn, ap):
    """(level, "H-B2", text) for a back 'p' of head 'h' that (nearly) always
    read empty while another back had WindowServer's frames ('drawn'). What
    the swaps naming it did decides the level (card run 2026-10-17c):
    INFO "handled" when modes 1/2 presented the landed buffer at them; WARN
    when every one of them was held / skipped and no other buffer had a new
    frame at any of them (WindowServer's second swap of a frame the swap
    before had presented: nothing lost); else FAIL (a frame held back while
    it sat in another buffer, or the empty back shown)"""
    where = ", ".join("0x%x (%d of %d swaps with pixels, %d aperture writes)" % (q, s2[2], s2[0], ap.get(q, 0))
                      for q, s2 in drawn)
    base = ("head %d: the back 0x%x was named by %d swaps and read EMPTY at %d of them, %d aperture (page-on) writes "
            "into it; WindowServer's frames land in %s" % (h, p, st[0], st[1], ap.get(p, 0), where))
    ev = [e for e in swaps if e.head == h and swap_back(e)[0] == p]
    landed = [e for e in ev if e.arg == 4 and e.v1 & V1_LANDED]
    would = [e for e in ev if e.v1 & V1_LANDED_WOULD]
    held = [e for e in ev if e.arg in (0, 5)]
    lost = [e for e in held if new_frame_elsewhere(trace, e)]
    sampled = [e for e in held if _scans_by_swap(trace).get((e.head, e.v0))]
    black = [e for e in ev if e.arg in (1, 2, 4) and e.flags & F_EMPTY]
    # how long after the head's swap before each swap naming p came (WindowServer's pairs)
    prev, gaps = None, []
    for e in swaps:
        if e.head != h:
            continue
        if prev is not None and swap_back(e)[0] == p and swap_back(prev)[0] != p:
            gaps.append((trace.ns(e, e.src) - trace.ns(prev, prev.src)) / 1e6)
        prev = e
    gap = " (median %.2f ms after the head's swap of another back)" % sorted(gaps)[len(gaps) // 2] if gaps else ""
    counts = "%d held/skipped, %d of them with a new frame in another buffer, %d shown black%s" % (
        len(held), len(lost), len(black), "; the landed buffer found but NOT presented at %d (debug.rdna3accel_"
        "swap_landed=0: rdna3swaplanded=1 presents it)" % len(would) if would else "")
    if landed:
        miss = sum(1 for e in landed if not swap_landed(trace, e))
        return ("INFO", "H-B2", "%s - handled: landed buffer presented %d times (swap mode 1/2 copied the buffer the "
                "frame was in%s); %s" % (base, len(landed), ", %d without the landed buffer's SCAN" % miss if miss
                                         else "", counts))
    if held and not lost and not black and not would and len(sampled) == len(held):
        return ("WARN", "H-B2", "%s - but no frame lost: all %d of its swaps were held/skipped and no other buffer "
                "had a new frame at any of them%s: WindowServer's second swap of a frame the swap before presented "
                "(copying or flipping this back would show black)" % (base, len(held), gap))
    return ("FAIL", "H-B2", "%s - a back that never receives a frame (copying or flipping it shows black); %s%s%s" % (
        base, counts, gap, "; %d held swaps without samples of the other buffers" % (len(held) - len(sampled))
        if len(sampled) < len(held) else ""))


def accel_lost(trace):
    """events of the accelerator's ring the trace does not hold: every stream
    read's loss (the boot before the pump too), else the full read's"""
    acc = [h for h in trace.headers if h["source"] == SRC_ACCEL and h["read_kind"] != READ_SEC]
    streams = [h for h in acc if h["read_kind"] == READ_STREAM]
    if streams:
        return sum(h["lost"] for h in streams)
    return max([h["lost"] for h in acc] or [0])


def gen_why(e):
    """a SWAP's write-generation reason (rdna3_write_gen.h enum rd3_wg_why; 0: not judged)"""
    return (e.v1 >> V1_GEN_WHY_SHIFT) & V1_GEN_WHY_MASK


def gen_swaps(trace):
    """{head: {"swaps", "judged", "presented", "held", "would", "landed", "other", "flipbuf", "why": {reason: n},
    "real": [SWAP], "changed": [SWAP]}}: the write generations of card run 2026-10-17d per head. "judged" =
    "presented" (the rule let it through and it went to the panel) + "held" (the rule held it) + "would" (it would have
    held: swap_gen 0, mode 2) + "landed" (it would have held, the surface's newer buffer presented instead) + "other"
    (the rule let it through, another rule held it: the empty back, the stale back - or the copy failed). A hold is
    "real" when the decoder saw a write into the held back (an APERTURE add, a copy forward into it) after the head's
    last presentation and before the hold (an evicted generation, a write path out of step), "changed" when the
    held back's band bytes differ from those of its last presentation on the head (ECC review round 1)."""
    out = {}
    last = {}     # head -> ns of its last presentation
    bands = {}    # (head, address) -> band bytes at its last presentation
    writes = {}   # address -> [ns of writes]
    for e in trace.events:
        if e.src != SRC_ACCEL:
            continue
        t = trace.ns(e, e.src)
        if e.type == T["APERTURE"] and e.arg == 1 and e.flags & 1:
            writes.setdefault(e.pa, []).append(t)
            continue
        if e.type == T["CF"] and not e.v4 and not e.flags & 4:
            writes.setdefault(e.pb, []).append(t)
            continue
        if e.type != T["SWAP"] or e.head == 0xff:
            continue
        flip = bool(e.v1 & V1_FLIPBUF)
        back = e.pa if flip else swap_back(e)[0]
        bb = e.ba if flip else swap_back(e)[4]
        d = out.setdefault(e.head, {"swaps": 0, "judged": 0, "presented": 0, "held": 0, "would": 0, "landed": 0,
                                    "other": 0, "flipbuf": 0, "why": {}, "real": [], "changed": [], "modes": set()})
        d["swaps"] += 1
        d["modes"].add(swap_mode(e))
        why = gen_why(e)
        shown = e.arg in (1, 2, 4)
        if why or e.v1 & (V1_GEN_HOLD | V1_GEN_WOULD):
            d["judged"] += 1
            d["why"][why] = d["why"].get(why, 0) + 1
            d["flipbuf"] += flip
            if e.v1 & V1_GEN_WOULD:
                d["would"] += 1
            elif e.v1 & V1_GEN_HOLD:
                d["held"] += 1
                t0 = last.get(e.head)
                if t0 is not None and any(t0 <= x <= t for x in writes.get(back, ())):
                    d["real"].append(e)
                pb = bands.get((e.head, back))
                if pb is not None and bb != pb and any(bb):
                    d["changed"].append(e)
            elif why in GEN_HOLDS and shown and e.v1 & V1_LANDED:
                d["landed"] += 1
            elif shown:
                d["presented"] += 1
            else:
                d["other"] += 1
        if shown:
            last[e.head] = t
            lb = swap_landed(trace, e) if e.v1 & V1_LANDED else 0
            if not lb:
                bands[(e.head, back)] = bb
    return out


def h_gen(trace, ctx=None):
    """[(level, "H-GEN", text)]: one line per head with judged swaps - presented (by reason), held (by reason), the
    surface's newer buffer presented instead (landed), would-hold (swap_gen 0 / mode 2), let through but held by
    another rule. FAIL a held back the decoder saw written since the head's last frame, FAIL a held back whose band
    bytes changed since its last presentation, FAIL swaps of a head in mode 0/1 with swap_gen 1 and none judged (the
    rule inert), WARN the content guard, the age guard, the eviction guard, WARN more than two holds per present,
    WARN the kernel's counters apart from the trace's or missing."""
    ctx = ctx or {}
    c = ctx.get("counters") or {}
    out = []
    lost = accel_lost(trace)
    for h, d in sorted(gen_swaps(trace).items()):
        w = d["why"]
        if not d["judged"]:
            if c.get("swap_gen") == 1 and d["modes"] & set((0, 1)) and d["swaps"] >= 20:
                out.append(("FAIL", "H-GEN", "head %d: %d swaps in mode 0/1 with swap_gen 1, NONE judged by the write "
                            "generations: the rule is inert (no head known at the swaps, or a kext without it - "
                            "swaps_gennotjudged_h%d says)" % (h, d["swaps"], h)))
            continue
        text = ("head %d: %d swaps judged by the write generations: %d presented (newer %d, first after a reset %d, "
                "changed with no write seen %d, aged %d, after an eviction %d), %d held (the panel's %d, older %d, "
                "never written %d), %d the surface's newer buffer instead, %d would have been held (swap_gen 0 / mode "
                "2), %d let through but held by another rule or failed, %d of them flip-buffer presents" % (
                    h, d["judged"], d["presented"], w.get(1, 0), w.get(2, 0), w.get(3, 0), w.get(7, 0), w.get(8, 0),
                    d["held"], w.get(4, 0), w.get(5, 0), w.get(6, 0), d["landed"], d["would"], d["other"],
                    d["flipbuf"]))
        out.append(("INFO", "H-GEN", text))
        if w.get(3):
            out.append(("WARN", "H-GEN", "head %d: %d back(s) changed with no write the accelerator saw (presented by "
                        "the content guard): a writer that is neither the family's page-on nor copy forward - the "
                        "kernel log's 'NOT written since this head presented it' lines name them" % (h, w[3])))
        if w.get(7) or w.get(8):
            out.append(("WARN", "H-GEN", "head %d: %d swap(s) presented by the age guard (no present for 250 ms, 3 holds "
                        "in a row) and %d after an eviction: the rule would have frozen the panel - a write it does not "
                        "see, or a full table" % (h, w.get(7, 0), w.get(8, 0))))
        if d["real"]:
            e = d["real"][0]
            out.append(("FAIL", "H-GEN", "head %d: %d hold(s) of a back the trace shows WRITTEN since the head's last "
                        "frame (first swap #%d, back 0x%x): a frame held back - an evicted generation "
                        "(swaps_gen_evicted) or a write the kernel did not stamp" % (
                            h, len(d["real"]), e.v0, e.pa if e.v1 & V1_FLIPBUF else swap_back(e)[0])))
        if d["changed"]:
            e = d["changed"][0]
            out.append(("FAIL", "H-GEN", "head %d: held a CHANGED back %d time(s) (first swap #%d, back 0x%x): its band "
                        "bytes differ from its last presentation on this head - a frame held back" % (
                            h, len(d["changed"]), e.v0, e.pa if e.v1 & V1_FLIPBUF else swap_back(e)[0])))
        pres = d["presented"] + d["landed"]
        if d["held"] > 2 * max(pres, 1) and d["held"] >= 20:
            out.append(("WARN", "H-GEN", "head %d: %d holds against %d presents (WindowServer's two swaps per frame give "
                        "about one): the panel may look frozen - compare the looks" % (h, d["held"], pres)))
        if pres >= 10 and w.get(2, 0) > 0.3 * pres:
            out.append(("WARN", "H-GEN", "head %d: %d of %d presents were the first after a reset (more than 30%%): the "
                        "head is reset all the time (surfaces taking turns? swaps_gen_resets_h%d) and the rule hardly "
                        "acts" % (h, w[2], pres, h)))
        k = c.get("swaps_genhold_h%d" % h)
        if k is None and c and current_counters(c):
            out.append(("WARN", "H-GEN", "head %d: counter missing: swaps_genhold_h%d (the collector's grep, or a kext "
                        "before the write generations) - the trace alone judges it" % (h, h)))
        elif k is not None and abs(k - d["held"]) > lost:
            out.append(("WARN", "H-GEN", "head %d: the kernel counted %d generation holds, the trace holds %d (%d "
                        "events lost): the trace does not cover the counters' window" % (h, k, d["held"], lost)))
    return out


def current_counters(c):
    """the report's counters come from a kext with the write generations (card run 2026-10-17d)"""
    return "swap_gen" in c


def flippath(trace):
    """{head: {"cf": copy forwards, "alone": those with no swap of their surface within FLIPPATH_NS after,
    "bursts": [(first CF, n, ms)] runs of 3 or more alone in a row, "flipbuf": {kind: n}, "what": {v4: n}}}: the
    family's other present path (card run 2026-10-17d: copy forward rotated a surface's three buffers at ~60 Hz with
    no submitSwapCopy)"""
    swaps = {}
    for e in swap_events(trace):
        if e.src == SRC_ACCEL and e.obj:
            swaps.setdefault(e.obj, []).append(trace.ns(e, e.src))
    out = {}
    run = {}

    def new():
        return {"cf": 0, "alone": 0, "bursts": [], "flipbuf": {}, "what": {}}
    for e in trace.by("CF"):
        if e.src != SRC_ACCEL:
            continue
        h = e.head
        d = out.setdefault(h, new())
        d["cf"] += 1
        t = trace.ns(e, e.src)
        alone = not any(t <= x <= t + FLIPPATH_NS for x in swaps.get(e.obj, ()))
        r = run.setdefault(h, [])
        if alone:
            d["alone"] += 1
            r.append((e, t))
        else:
            if len(r) >= 3:
                d["bursts"].append((r[0][0], len(r), (r[-1][1] - r[0][1]) // 10 ** 6))
            run[h] = []
    for h, r in run.items():
        if len(r) >= 3:
            out[h]["bursts"].append((r[0][0], len(r), (r[-1][1] - r[0][1]) // 10 ** 6))
    for e in trace.by("FLIPBUF"):
        d = out.setdefault(e.head, new())
        d["flipbuf"][e.arg] = d["flipbuf"].get(e.arg, 0) + 1
        d["what"][e.v4] = d["what"].get(e.v4, 0) + 1
    return out


def h_flippath(trace, ctx=None):
    """[(level, "H-FLIPPATH", text)]: per head, the copy forwards no swap of their surface followed within 50 ms (the
    rotation bursts: frames the family presented by another path) and what the flip-buffer hooks saw (FLIPBUF events:
    calls, presents, holds); the surface hooks (HOOK events) in one line. "not judged" for a kext without the hooks;
    WARN bursts with no FLIPBUF event at all (the frames go by yet another path), WARN a failed flip-buffer present
    and the outcomes that did not present (lock busy, no VRAM resource, no head), WARN a missing counter."""
    ctx = ctx or {}
    c = ctx.get("counters") or {}
    out = []
    hooks = trace.by("HOOK")
    fb = trace.by("FLIPBUF")
    hooked = bool(hooks or fb) or current_counters(c)
    for h, d in sorted(flippath(trace).items(), key=lambda x: x[0]):
        hn = "head %s" % (h if h != 0xff else "?")
        if d["cf"]:
            longest = max(d["bursts"], key=lambda b: b[1]) if d["bursts"] else None
            base = "%s: %d of %d copy forwards with no swap of their surface within %d ms, %d burst(s) of 3 or more%s" % (
                hn, d["alone"], d["cf"], FLIPPATH_NS // 10 ** 6, len(d["bursts"]),
                " (the longest: %d copy forwards over %d ms from CF seq %d)" % (
                    longest[1], longest[2], longest[0].seq) if longest else "")
            if not hooked:
                out.append(("INFO", "H-FLIPPATH", base + " - the flip-buffer path not judged: a kext without the "
                            "flip-present hooks"))
            elif d["bursts"] and not fb:
                out.append(("WARN", "H-FLIPPATH", base + " - and NO flip-buffer call in the trace: those frames reach "
                            "the panel by yet another path (or not at all)"))
            else:
                out.append(("INFO", "H-FLIPPATH", base))
        if d["flipbuf"] and h != 0xff and c and current_counters(c) and "flipbuf_h%d" % h not in c:
            out.append(("WARN", "H-FLIPPATH", "%s: counter missing: flipbuf_h%d although the trace has flip-buffer "
                        "calls (the collector's grep)" % (hn, h)))
        if d["flipbuf"]:
            bad = dict((k, v) for k, v in d["what"].items() if k in (3, 4, 5, 6))
            out.append(("WARN" if bad else "INFO", "H-FLIPPATH", "%s: flip-buffer calls %s; %s%s" % (
                hn, ", ".join("%s %d" % ("submitScanoutFlipBuffer" if k == 1 else "submitFlipBuffer", v)
                              for k, v in sorted(d["flipbuf"].items())),
                ", ".join("%s %d" % (FLIPBUF_WHAT.get(k, k), v) for k, v in sorted(d["what"].items())),
                " - NOT presented: %s (the kernel log's 'flipbuf' lines say why)" % ", ".join(
                    "%s %d" % (FLIPBUF_WHAT.get(k, k), v) for k, v in sorted(bad.items())) if bad else "")))
    if hooks:
        per = {}
        notready = 0
        for e in hooks:
            per[e.arg] = per.get(e.arg, 0) + 1
            notready += e.arg == 2 and not e.v2
        out.append(("INFO", "H-FLIPPATH", "surface hooks traced: %s%s" % (
            ", ".join("%s %d" % (HOOK_KIND.get(k, k), v) for k, v in sorted(per.items())),
            "; isBackBufferReady said NOT ready %d time(s)" % notready if notready else "")))
    elif current_counters(c):
        sw = sum(v for k, v in c.items() if re.match(r"swaps_h\d$", k))
        calls = sum(v for k, v in c.items() if k.startswith(("hook_", "flipbuf")) and k != "flipbuf_present")
        if sw > 50 and not calls:
            out.append(("WARN", "H-FLIPPATH", "hook wiring unproven: %d swaps and every hook_* / flipbuf* counter 0 - "
                        "the family never called a hooked method (or the vtable slots are not the ones it calls: the "
                        "boot log's 'flip-present hooks installed' line has them)" % sw))
    return out


def stale_backs(trace):
    """the stale back of card run 2026-10-17c, per head over the whole trace:
    {head: {"copies": mode 1/2 copies decoded, "copied": [(SWAP, the swap #
    that showed the back before)], "held": [SWAP], "would": [SWAP] (stale
    copies the kernel knew of, swap_stale=0), "swaps": the head's swaps,
    "mode12": its mode 1/2 swaps, "real": [(SWAP, why)] holds of a real
    frame}}. A mode 1/2 copy is stale when the back it copied had no copy
    forward into it since this head last presented it, its band bytes are
    those of that presentation, and it came less than STALE_NS after
    another back's copy on the head (WindowServer's second swap of a frame,
    naming a back it did not draw): the panel then shows the older frame
    for most of the frame period. The records are forgotten as the kernel
    forgets them: the address freed (VMFREE), the surface or the head's pipe
    resource gone (GONE), a swap mode change (MODESET arg 0), a display mode
    change (MODE arg 1) and the head's D reallocated (VMALLOC flags 8). The
    bands are RGB only (the kernel's hash has alpha too): an alpha-only
    change reads "unchanged" here."""
    out = {}
    st = {}   # head -> {"rec": {addr: [bands, cf since, swap #, ns]}, "last": addr, "t": ns}
    ap = {}   # address -> [ns of APERTURE adds]
    shown_t = {}   # (head, swap #) -> ns
    forget = (T["VMFREE"], T["GONE"], T["MODESET"], T["MODE"], T["VMALLOC"])

    def head_out(h):
        return out.setdefault(h, {"copies": 0, "copied": [], "held": [], "would": [], "swaps": 0, "mode12": 0,
                                  "real": []})

    for e in trace.events:
        if e.src != SRC_ACCEL or e.type not in (T["SWAP"], T["CF"], T["APERTURE"]) + forget:
            continue
        t = trace.ns(e, e.src)
        if e.type == T["APERTURE"]:
            if e.arg == 1:
                ap.setdefault(e.pa, []).append(t)
            continue
        if e.type == T["VMFREE"]:
            if e.flags & VM_PARKED:   # parked, still allocated: the kernel keeps its records (N41)
                continue
            for hs in st.values():
                hs["rec"].pop(e.pa, None)
                if hs["last"] == e.pa:
                    hs["last"] = None
            continue
        if e.type in forget:
            if e.type == T["GONE"] or (e.type == T["VMALLOC"] and e.flags & 8 and e.arg == 1):  # D granted
                st.pop(e.head, None)
            elif (e.type == T["MODESET"] and e.arg == 0 and e.v0 != e.v1) or (e.type == T["MODE"] and e.arg == 1):
                st.clear()
            continue
        if e.type == T["CF"]:   # the target refreshed: marked in every head's table, as the kernel does
            for hs in st.values():
                if e.pb in hs["rec"]:
                    hs["rec"][e.pb][1] = True
            continue
        if e.head == 0xff or e.v1 & V1_FLIPBUF:
            continue
        o = head_out(e.head)
        o["swaps"] += 1
        if swap_mode(e) == 0:
            continue
        o["mode12"] += 1
        hs = st.setdefault(e.head, {"rec": {}, "last": None, "t": None})
        back, _, _, _, bb, _ = swap_back(e)
        if e.arg == 5 and e.v1 & V1_STALE:
            o["held"].append(e)
            # a real frame held: another buffer had a new frame at the swap, or the CPU wrote the back
            # (the family's aperture) between its presentation and the hold
            new = new_frame_elsewhere(trace, e)
            r = hs["rec"].get(back)
            t0 = r[3] if r else shown_t.get((e.head, e.v3))
            wrote = [x for x in ap.get(back, ()) if t0 is not None and t0 <= x <= t]
            if new:
                o["real"].append((e, "a new frame in 0x%x at that swap" % new[0]))
            elif wrote:
                o["real"].append((e, "%d aperture write(s) into the back since swap #%d" % (len(wrote), e.v3)))
            continue
        if e.arg != 4:
            continue
        o["copies"] += 1
        lb = swap_landed(trace, e) if e.v1 & V1_LANDED else 0
        if lb:   # the landed buffer went to the panel, not the back named; its bands are in that swap's SCAN
            back = lb
            bb = next((s.ba for s in _scans_by_swap(trace).get((e.head, e.v0), ()) if s.pa == lb), None)
        r = hs["rec"].get(back)
        if (r and bb is not None and not r[1] and r[0] == bb and hs["last"] not in (None, back) and
                hs["t"] is not None and t - hs["t"] < STALE_NS):
            o["copied"].append((e, r[2]))
            if e.v1 & V1_STALE_WOULD:
                o["would"].append(e)
        hs["rec"][back] = [bb, False, e.v0, t]
        hs["last"], hs["t"] = back, t
        shown_t[(e.head, e.v0)] = t
    return out


def scanout_aba(trace):
    """{head: A-B-A patterns}: the head's scanout samples after the mode 1/2
    copies (SCAN 0xfe) where a picture came back after one other picture
    (the old frame shown again between two new ones looks so)"""
    seq = {}
    for e in trace.by("SCAN"):
        if e.arg == 0xfe and e.flags & 64 and e.head != 0xff:
            seq.setdefault(e.head, []).append(bytes(e.ba))
    return dict((h, sum(1 for i in range(2, len(s)) if s[i] == s[i - 2] and s[i] != s[i - 1]))
                for h, s in seq.items())


STALE_WHY = (("norecord", "never presented here / forgotten"), ("changed", "changed"),
             ("cf", "copy forward into it"), ("sameshown", "nothing shown after it"),
             ("nopanel", "no frame of ours on the panel"), ("nohead", "no head known"))


def stale_counters(c, h):
    """the kernel's stale-back counters of head h (debug.rdna3accel_swaps_stale*_h<N>): a text naming why
    backs were not held, or None on a kext without them"""
    if not c or ("swaps_stalehold_h%d" % h) not in c:
        return None
    g = lambda k: c.get("%s_h%d" % (k, h), 0)   # noqa: E731
    return ("kernel: %d held, %d would (swap_stale 0); not held: %s; %d evicted (8-slot table), %d second-grid "
            "misses, %d changed without copy forward, %d resets, %d freed, %d changed after the hold" % (
                g("swaps_stalehold"), g("swaps_stalehold_would"),
                ", ".join("%s %d" % (k, g("swaps_stalecheck_" + k)) for k, _ in STALE_WHY),
                g("swaps_staleevict"), g("swaps_stalehold_miss"), g("swaps_stalenocf"), g("swaps_stalereset"),
                g("swaps_stalegone"), g("swaps_stalehold_changed")))


def h_stale(trace, ctx=None):
    """[(level, "H-STALE" / "A-B-A", text)]: one H-STALE line per head with
    mode 1/2 swaps - copies decoded, stale copies, holds, A-B-A on the
    scanout samples. FAIL a stale copy in mode 1 (not on purpose: swap_stale
    0 is WARN), FAIL a hold of a real frame, WARN more than two stale holds
    per copy (a frozen panel), WARN no decodable copy, WARN the kernel's
    hold counter and the trace's holds apart by more than the events lost;
    INFO otherwise. The A-B-A line is its own: the kernel's flicker test
    (swaps_abab_h<N>) hashes the backs it shows, not the panel, so its PASS
    does not cover this."""
    ctx = ctx or {}
    c = ctx.get("counters") or {}
    out = []
    aba = scanout_aba(trace)
    data = stale_backs(trace)
    has_hold = any(e.v1 & (V1_STALE | V1_STALE_WOULD) for e in trace.by("SWAP"))
    lost = accel_lost(trace)
    for h, d in sorted(data.items()):
        if not d["mode12"]:
            if d["swaps"]:   # card run 2026-10-17d: mode 0 (flip) is the default; there is no copy to judge
                out.append(("INFO", "H-STALE", "head %d: %d swaps, all mode 0 (flip): not judged" % (h, d["swaps"])))
            continue
        cp, held, would = d["copied"], d["held"], d["would"]
        cp1 = [x for x in cp if swap_mode(x[0]) == 1]
        cp2 = len(cp) - len(cp1)
        kern = stale_counters(c, h)
        if not d["copies"]:
            out.append(("WARN", "H-STALE", "head %d: %d mode 1/2 swaps but no copy decoded - the stale back is not "
                        "judged%s" % (h, d["mode12"], "; " + kern if kern else "")))
            continue
        base = "head %d: %d mode 1/2 copies decoded, %d stale copied%s, %d held, %d A-B-A on the scanout samples" % (
            h, d["copies"], len(cp), " (%d in mode 2, the copy-everything control)" % cp2 if cp2 else "",
            len(held), aba.get(h, 0))
        if cp1:
            per = {}
            for e, _ in cp1:
                b = swap_back(e)[0]
                per[b] = per.get(b, 0) + 1
            top = ", ".join("0x%x %dx" % (b, k) for b, k in sorted(per.items(), key=lambda x: -x[1])[:3])
            on_purpose = len(would) == len(cp1)
            if kern:
                why = kern
            elif has_hold:
                why = "the kext has the hold (its holds are in the trace); its counters are not in this report"
            else:
                why = ("no stale hold in the trace: a kext before the stale-back hold (2026-10-17c), or none of its "
                       "checks passed (the swaps_stalecheck_*_h<N> counters say which)")
            lvl = "WARN" if on_purpose else "FAIL"
            out.append((lvl, "H-STALE", "%s - %s (%s; first swap #%d, shown before at #%d): %s - no copy forward "
                        "into them since this head presented them, their samples unchanged, < %d ms after another "
                        "back's copy: the panel showed the older frame for most of the frame period and the front "
                        "got it too; %s" % (
                            base, "copied on purpose (debug.rdna3accel_swap_stale=0)" if on_purpose else
                            "stale backs COPIED", top, cp1[0][0].v0, cp1[0][1],
                            "WindowServer did not redraw them", STALE_NS // 10 ** 6, why)))
        else:
            out.append(("INFO", "H-STALE", "%s%s" % (base, "; " + kern if kern else "")))
        for e, why in d["real"][:1]:
            out.append(("FAIL", "H-STALE", "head %d: held a REAL frame at %d stale hold(s) (first swap #%d: %s) - the "
                        "back was redrawn although its samples matched: the panel missed that frame" % (
                            h, len(d["real"]), e.v0, why)))
        # WindowServer's healthy pattern is about one stale hold per copy (B1 drawn, B2 not): more than two
        # per copy means frames that should have reached the panel were held
        if held and len(held) > 2 * d["copies"]:
            out.append(("WARN", "H-STALE", "head %d: %d stale holds against %d copies (more than two per copy; "
                        "WindowServer's two swaps per frame give about one): the panel may look frozen - compare "
                        "the looks" % (h, len(held), d["copies"])))
        if "swaps_stalehold_h%d" % h in c:
            k = c["swaps_stalehold_h%d" % h]
            if abs(k - len(held)) > lost:
                out.append(("WARN", "H-STALE", "head %d: the kernel counted %d stale holds, the trace holds %d (%d "
                            "events lost): the trace does not cover the counters' window, or a hold was not traced" %
                            (h, k, len(held), lost)))
    for h in sorted(aba):
        if aba[h]:
            out.append(("INFO" if not data.get(h, {}).get("copied") else "WARN", "A-B-A",
                        "head %d: %d A-B-A patterns on the scanout samples (a picture back after one other: an old "
                        "frame shown between two new ones) - the kernel's flicker test (swaps_abab_h<N>) hashes the "
                        "backs, not the panel: its PASS does not cover this" % (h, aba[h])))
    return out


def copy_black(trace, swaps=None):
    """[(level, name, text)]: COPY-BLACK - a mode 1/2 copy whose back had
    pixels but the scanout buffer (SCAN 0xfe) or the front (0xfd) sampled
    right after it had none: the copy itself lost the picture"""
    after = {}
    for e in trace.by("SCAN"):
        if e.arg in (0xfe, 0xfd) and e.flags & (64 | 128):
            after.setdefault((e.head, e.v0), []).append(e)
    bad = []
    for e in (swap_events(trace) if swaps is None else swaps):
        if e.arg != 4:
            continue
        back, rcb = swap_back(e)[:2]
        if rcb != 5:
            continue
        for sc in after.get((e.head, e.v0), []):
            if not sc.flags & 1:
                bad.append((e, sc))
                break
    if not bad:
        return []
    e, sc = bad[0]
    return [("FAIL", "COPY-BLACK", "%d copy(ies) of a back WITH pixels left the %s without any (first: swap #%d head "
             "%d, back 0x%x -> 0x%x): the copy lost the picture (SDMA, a wrong source / pitch, or HDP)" %
             (len(bad), "scanout buffer" if sc.arg == 0xfe else "front", e.v0, e.head, swap_back(e)[0], sc.pa))]


def back_surfaces(trace, swaps=None):
    """{head: {back phys: [swaps, IOSurface id or None, type byte or None, swaps with pixels]}}:
    the join of the swaps' backs with the IOSurface ids SURFRES / SCAN carry"""
    ids = {}
    for e in trace.by("SURFRES"):
        if e.pa:
            if e.v0 or e.pa not in ids:
                ids[e.pa] = (e.v0 or None, e.v4)
    for e in trace.by("SCAN"):
        if e.pa and e.v3 and e.pa not in ids:
            ids[e.pa] = (e.v3, e.v4)
    out = {}
    ph = phase_of(trace)
    for e in (swap_events(trace) if swaps is None else swaps):
        if e.head == 0xff:
            continue
        back, rcb = swap_back(e)[:2]
        if not back:
            continue
        sid, ty = ids.get(back, (None, None))
        # [swaps, id, type, with pixels, in phase 1, in phase 2, empty]
        st = out.setdefault(e.head, {}).setdefault(back, [0, sid, ty, 0, 0, 0, 0])
        st[0] += 1
        st[3] += rcb == 5
        p = ph.get(id(e), 0)
        st[4] += p == 1
        st[5] += p == 2
        st[6] += rcb == 4
    return out


def join_lines(trace, plugin=None, covers=None):
    """[(level, text)]: every back the swaps named (phases 1-2 when the A/B
    ran) with its IOSurface id, its swaps per A/B phase and how many read
    empty; WARN for a back without one (the plugin's trace cannot be joined
    to it). plugin: {IOSurface id: what the plugin's trace says of it}
    (tools/plugin_trace.py surface_summary); covers: the plugin's trace is
    complete over the A/B - then a back with empty swaps the plugin never
    saw is a WARN (ECC review round 3)"""
    swaps, _ = ab_events(trace)
    out = []
    for h, bufs in sorted(back_surfaces(trace, swaps).items()):
        for phys, (n, sid, ty, pix, n1, n2, empty) in sorted(bufs.items()):
            if sid is None:
                out.append(("WARN", "join: head %d back 0x%x (%d swaps, %d with pixels) has NO IOSurface id in the "
                                    "trace%s - the plugin's trace cannot be joined to it" %
                            (h, phys, n, pix, " (resource type 0x%02x)" % (ty & 0xff) if ty is not None else
                             " (no SURFRES for it)")))
                continue
            what, lv = "", "INFO"
            if plugin is not None:
                if sid in plugin:
                    what = ": plugin " + plugin[sid]
                elif covers:
                    what = (": plugin NEVER saw this IOSurface although its trace covers the A/B - not rendered by "
                            "Metal (the panel gets whatever is in it)")
                    lv = "WARN" if empty else "INFO"
                else:
                    what = ": plugin NEVER saw this IOSurface (its trace does not cover the A/B, or Metal never drew it)"
            split = ", %d in mode 1 / %d in mode 0 of the A/B" % (n1, n2) if n1 or n2 else ""
            out.append((lv, "join: head %d back 0x%x = IOSurface %d (%d swaps%s, %d with pixels, %d EMPTY)%s" %
                        (h, phys, sid, n, split, pix, empty, what)))
    return out


def ab_window(trace):
    """(start, end) wall seconds of the A/B's phases 1-2 (MODESET arg 1), None
    without a wall anchor or an A/B"""
    ms = [e for e in trace.events if e.src == SRC_ACCEL and e.type == T["MODESET"] and e.arg == 1]
    start = [e for e in ms if e.v1 == 1]
    end = [e for e in ms if e.v1 in (3, 0) and start and e.t > start[0].t]
    if not start:
        return None
    a, b = wall_of(trace, start[0]), wall_of(trace, end[0]) if end else None
    if a is None:
        return None
    return a, b


def ab_table(trace):
    """the A/B side by side: one line per (mode, phase) and a verdict (from
    phases 1 and 2 only when the A/B ran)"""
    swaps, restricted = ab_events(trace)
    seg = segments(trace, swaps)
    lines = []
    for (mode, phase), s in sorted(seg.items(), key=lambda kv: (kv[0][1], kv[0][0])):
        lines.append("mode %d %-10s %5d swaps: %5d shown (%d landed), %4d held/skipped (%d with a new frame "
                     "elsewhere), %d stale backs held, %d landed buffers NOT presented (swap_landed 0), %4d empty "
                     "backs, %4d BLACK frames, "
                     "%4d failed, %4d in-place rewrites of %d, %4d tear-free copies, %4d direct (may tear), "
                     "%d same buffer (not flipped), waits %d (avg %d us), copy avg %d us" %
                     (mode, "(" + AB_NAME.get(phase, str(phase)) + ")", s["swaps"], s["shown"], s["landed"],
                      s["held"], s["held_new"], s["stale"], s["would"], s["empty"],
                      s["black"], s["failed"], s["inplace"], s["inplace_checked"], s["double"], s["direct"],
                      s["same"], s["waits"], s["wait_us"] // s["waits"] if s["waits"] else 0,
                      s["copy_us"] // max(1, s["double"] + s["direct"])))
    m1 = [s for (m, _), s in seg.items() if m in (1, 2)]
    m0 = [s for (m, _), s in seg.items() if m == 0]
    verdict = None
    if m1 and m0:
        b1, b0 = sum(s["black"] for s in m1), sum(s["black"] for s in m0)
        i0 = sum(s["inplace"] for s in m0)
        f1 = sum(s["failed"] for s in m1)
        # card run 2026-10-17c: the swaps naming an empty back - held, or the landed buffer presented
        h1, n1, l1, w1 = (sum(s[k] for s in m1) for k in ("held", "held_new", "landed", "would"))
        held = "; mode 1: %d held (%d with a new frame in another buffer: frames held back), %d landed buffers " \
               "presented, %d found but not presented (debug.rdna3accel_swap_landed=0)" % (h1, n1, l1, w1)
        if not b1 and not f1 and i0:
            verdict = ("PASS", "the copy (mode 1) showed no black frame and no failed copy, the flip (mode 0) %d "
                               "in-place rewrites and %d black frames: mode 1 fixes the model mismatch%s" %
                       (i0, b0, held))
        elif b1 or f1:
            verdict = ("FAIL", "mode 1 still showed %d black frame(s) and %d failed copies (mode 0: %d black, %d "
                               "in-place)%s" % (b1, f1, b0, i0, held))
        else:
            verdict = ("INFO", "neither mode showed black frames or in-place rewrites in this window%s" % held)
    return lines, verdict


def anchors(trace):
    """{source: (now ns, wall us)}: each source's newest read with a wall
    clock (version 2 headers) - mach time and the logs' dates on one line"""
    out = {}
    for h in trace.headers:
        if h.get("wall_us") and h["tb_numer"] and h["tb_denom"]:
            out[h["source"]] = (h["now"] * h["tb_numer"] // h["tb_denom"], h["wall_us"])
    return out


def wall_of(trace, e):
    """an event's wall clock (seconds since 1970, float) through its source's anchor, None without one"""
    an = anchors(trace).get(e.src)
    if not an:
        return None
    return (an[1] * 1000 - (an[0] - trace.ns(e, e.src))) / 1e9


def coverage(trace, ctx=None):
    """[(question, ok, text)]: can the report answer Q1-Q11 (docs/AFTER-TESTS.md)?
    ctx: swaps (the counters' swap count), swaps_window ({head: the counters'
    swaps over the A/B}), cf_calls, ab_ran (autoreport ran the A/B),
    core_expected (Metal / RADV ran), size_problems ([text]: stream files
    whose size differs from their sidecar)"""
    ctx = ctx or {}
    out = []
    ab_ran = bool(ctx.get("ab_ran")) or ab_info(trace)["ran"]
    swaps = swap_events(trace)
    acc = [h for h in trace.headers if h["source"] == SRC_ACCEL and h["read_kind"] != READ_SEC]
    core = [h for h in trace.headers if h["source"] == SRC_CORE]
    have_accel = bool(acc) or any(e.src == SRC_ACCEL for e in trace.events)
    probs = [t for lv, q, t in trace.problems if q == "Q8" and lv == "FAIL"] + list(ctx.get("size_problems") or [])
    if not have_accel:
        out.append(("Q1-Q8", False, "no accelerator trace in the report (36-frame-trace.bin, the -trace-stream.bin "
                                    "next to it, or 'tr' lines)%s" % ("; " + "; ".join(probs[:3]) if probs else "")))
    else:
        win = ctx.get("swaps_window") or {}
        ok1 = bool(swaps) or not ctx.get("swaps")
        why1 = "%d swap events (counters: %s swaps)" % (len(swaps), ctx.get("swaps", "?"))
        short = []
        for h, want in sorted(win.items()):
            got = sum(1 for e in swaps if e.head == h and e.arg in COUNTED_SWAPS)
            if want and got < want * 0.98:
                short.append("head %d: %d in the trace, %d counted over the A/B" % (h, got, want))
        if short:
            ok1 = False
            why1 += "; FEWER than the counters: " + ", ".join(short)
        elif win:
            why1 += "; the A/B window's counters all in it"
        elif ctx.get("no_tests"):
            why1 += " (no tests file: the counters' window not compared)"
        out.append(("Q1", ok1, why1))
        scans = [e for e in trace.by("SCAN") if e.arg not in (0xfe, 0xfd) and not e.flags & 16]
        # card run 2026-10-17d: trace_scan is 0 by default (only empty backs are sampled): no samples is then
        # what the kernel was told to do, not a hole in the trace - but only a report that says so
        # (debug.rdna3accel_trace_scan in its counters) is excused
        off = (ctx.get("counters") or {}).get("trace_scan") == 0
        out.append(("Q2", bool(scans) or not swaps or off, "%d samples of the surfaces' other buffers%s" % (
            len(scans), " (trace_scan 0: only empty backs are sampled)" if off and not scans else "")))
        empty = sum(1 for e in swaps if e.flags & F_EMPTY and e.head != 0xff)
        late = [e for e in trace.by("SCAN") if e.flags & 16]
        out.append(("Q3", bool(late) or empty < 8, "%d empty backs, %d delayed re-samples" % (empty, len(late))))
        cfs = trace.by("CF")
        out.append(("Q4", bool(cfs) or not ctx.get("cf_calls"), "%d copy forward events (counters: %s calls)" %
                    (len(cfs), ctx.get("cf_calls", "?"))))
        allocs, frees, shapes = trace.by("VMALLOC"), trace.by("VMFREE"), trace.by("SHAPE")
        boot_kept = any(h["read_kind"] == READ_ALL and h["lost"] == 0 for h in acc)
        # the buffers the swaps name were allocated and shaped somewhere: with the
        # boot still in the ring, none of that recorded is a hook that never fires
        ok5 = not swaps or bool(shapes) or not boot_kept
        ok5 = ok5 and (not swaps or bool(allocs) or not boot_kept)
        out.append(("Q5", ok5, "%d allocations, %d frees, %d shapes%s" %
                    (len(allocs), len(frees), len(shapes),
                     "" if ok5 else " - none although the ring still holds the whole boot and swaps ran")))
        panel = trace.by("PANEL")
        known = [e for e in panel if e.pa and not e.flags & 2]   # MC addresses are not known (round 2)
        whyp = {}
        for e in panel:
            if not e.pa or e.flags & 2:
                whyp[e.v3] = whyp.get(e.v3, 0) + 1
        reasons = {1: "no core", 2: "the core does not know the head", 3: "the core's rdna3DisplayScanAddr failed",
                   0: "a core without rdna3DisplayScanAddr", 4: "no VRAM base yet (MC addresses)",
                   5: "an address below the framebuffer aperture (MC)"}
        out.append(("Q6", bool(known) or not swaps,
                    "%d panel reads, %d with the HUBP's address%s" %
                    (len(panel), len(known), "" if known or not panel else " (%s)" % ", ".join(
                        "%s: %d" % (reasons.get(k, "reason %d" % k), v) for k, v in sorted(whyp.items())))))
        ms = trace.by("MODESET")
        out.append(("Q7", bool([e for e in ms if e.arg == 1]) or not ab_ran,
                    "%d swap mode / A/B changes, %d display mode events" % (len(ms), len(trace.by("MODE")))))
        streams = [h for h in trace.headers if h["read_kind"] == READ_STREAM]
        if streams:
            # the first read of each stream file is the boot before the pump
            # started (autoreport.sh keeps it, ECC review round 2): what the
            # ring overwrote before it is not a loss of the run
            first = set()
            seen = set()
            for h in streams:
                k = (h["where"], h["source"])
                if k not in seen:
                    seen.add(k)
                    first.add(id(h))
            boot = sum(h["lost"] for h in streams if id(h) in first)
            run_ = [h for h in streams if id(h) not in first]
            lost = sum(h["lost"] for h in run_)
            lc = sum(h["lost"] for h in run_ if h["source"] == SRC_CORE)
            ok8 = lost == 0
            why = "%d stream reads, %d events lost between them%s%s" % (
                len(streams), lost, " (%d of the core's)" % lc if lc else "",
                "; the boot before the pump: %d events overwritten before its first read" % boot if boot else "")
        else:
            lost = max([h["lost"] for h in acc] or [0])
            ok8 = lost == 0 or not ab_ran
            why = "no stream; the ring lost %d events before the read" % lost
        if not trace.secs:
            why += "; NO per-second table"
            ok8 = ok8 and not ab_ran
        if probs:
            ok8 = False
            why += "; " + "; ".join(probs[:4]) + ("; ... %d more" % (len(probs) - 4) if len(probs) > 4 else "")
        out.append(("Q8", ok8, why))
    cs = trace.by("CS")
    bad_cs = [e for e in cs if e.arg == 0xff]
    out.append(("Q9", bool(core) or bool(cs) or not ctx.get("core_expected"),
                "%d command submissions (%d refused), %d fences, %d waits, %d resets, %d faults%s" %
                (len(cs) - len(bad_cs), len(bad_cs), len(trace.by("FENCE")), len(trace.by("GPUWAIT")),
                 len(trace.by("RESET")), len(trace.by("VMFAULT")),
                 "" if core or cs else " (no core trace: 36-drm-trace.bin)")))
    sg = trace.by("SURFGEM") + trace.by("SURFRES")
    out.append(("Q10", bool(sg) or not have_accel, "%d IOSurface answers / pairs" % len(sg)))
    an = anchors(trace)
    v2 = [h for h in trace.headers if h["version"] >= 2]
    ok11 = bool(trace.tb) or not trace.headers
    if v2 and not an:
        ok11 = False
    out.append(("Q11", ok11, "timebase %s; wall clock %s" % (
        ", ".join("src %d %d/%d" % (k, v[0], v[1]) for k, v in sorted(trace.tb.items())) or "none",
        ", ".join("src %d: mach %d ns = %s UTC" % (k, v[0], time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(
            v[1] // 1000000)) + ".%06d" % (v[1] % 1000000)) for k, v in sorted(an.items())) or
        ("NONE in the version 2 headers" if v2 else "none (version 1 headers)"))))
    return out


def fmt_event(trace, e, t0=0):
    ms = (trace.ns(e, e.src) - t0) / 1e6
    n = e.name
    w = wall_of(trace, e)
    if w is not None:   # the logs' clock (UTC), so plugin and kernel lines can be read side by side
        return time.strftime("%H:%M:%S", time.gmtime(w)) + ".%03d " % int((w % 1) * 1000) + _fmt_event(trace, e, ms, n)
    return _fmt_event(trace, e, ms, n)


def _fmt_event(trace, e, ms, n):
    if n == "SWAP":
        back, rcb, front, rcf, bb, fb = swap_back(e)
        lb = swap_landed(trace, e)
        landed = "" if lb == 0 else " %s the landed buffer %s" % (
            "presented" if e.v1 & V1_LANDED else "would present (swap_landed 0)",
            "0x%x" % lb if lb else "buf[%d]" % ((e.v4 >> 24) & 0xff))
        if e.arg == 5 and e.v1 & V1_STALE:
            landed += " held: stale back (shown at #%d)" % e.v3
        if e.v1 & V1_STALE_WOULD:
            landed += " stale: would hold (swap_stale 0)"
        why = gen_why(e)
        if why:
            landed += " gen %d: %s" % (e.v2 >> 16, GEN_WHY.get(why, why))
        if e.v1 & V1_GEN_HOLD:
            landed += " HELD: not written since the head's frame (head's generation %d)" % e.v3
        if e.v1 & V1_GEN_WOULD:
            landed += " gen: would hold (swap_gen 0 or mode 2)"
        if e.v1 & V1_FLIPBUF:
            return ("%10.3f ms SWAP #%d h%s mode %d %s FLIP-BUFFER PRESENT (%s): buffer 0x%x (%s, %s) other 0x%x%s -> "
                    "panel 0x%x target %s copy %s" % (
                        ms, e.v0, e.head if e.head != 0xff else "?", swap_mode(e), SWAP_ARG.get(e.arg, e.arg),
                        "scanout" if (e.v4 >> 24) & 0xff == 1 else "flip", e.pa, RC.get(e.v1 & 0xf, e.v1 & 0xf),
                        e.ba.hex(), e.pb, landed, e.pc, TARGET.get((e.v1 >> 16) & 0xf, "?"), copy_us_text(e)))
        return ("%10.3f ms SWAP #%d h%s mode %d %s: back 0x%x (%s, %s) front 0x%x (%s)%s -> panel 0x%x target %s%s%s "
                "copy %s wait %d us" % (ms, e.v0, e.head if e.head != 0xff else "?", swap_mode(e),
                                          SWAP_ARG.get(e.arg, e.arg), back, RC.get(rcb, rcb), bb.hex(), front,
                                          RC.get(rcf, rcf), landed, e.pc, TARGET.get((e.v1 >> 16) & 0xf, "?"),
                                          " FRONT-OK" if e.flags & F_FRONT_OK else "",
                                          " tear-free" if e.flags & F_DOUBLE else "", copy_us_text(e),
                                          0 if e.arg == 5 and e.v1 & (V1_STALE | V1_GEN_HOLD) else e.v3))
    if n == "FLIPBUF":
        return ("%10.3f ms FLIPBUF h%s %s(%d) #%d res 0x%x / 0x%x -> %s%s%s panel 0x%x" % (
            ms, e.head if e.head != 0xff else "-", "submitScanoutFlipBuffer" if e.arg == 1 else "submitFlipBuffer",
            e.v0, e.v1, e.pa, e.pb, FLIPBUF_WHAT.get(e.v4, e.v4), " (flipbuf_present 1)" if e.flags & 1 else "",
            " (took the family's lock)" if e.flags & 2 else "", e.pc))
    if n == "PRESENT":
        if e.arg == 11:
            return "%10.3f ms PRESENT h%s a %s into 0x%x waited %d us for the flip away from it%s" % (
                ms, e.head if e.head != 0xff else "-", "copy forward" if e.flags & 1 else "page-on", e.pa, e.v1,
                " -- TIMED OUT" if e.flags & 4 else "")
        return "%10.3f ms PRESENT h%s %s #%d 0x%x %s%s%s -> in flight 0x%x, queued 0x%x" % (
            ms, e.head if e.head != 0xff else "-", PQ_KIND.get(e.flags & 0x7f, e.flags), e.v0, e.pa,
            PRESENT_ARG.get(e.arg, e.arg) + (" (at a mode change)" if e.arg == 10 and e.flags & PQ_DROP_MODE else ""),
            " %d us" % e.v1 if e.arg in (6, 8, 12) else "", " (replaced #%d)" % e.v2 if e.arg == 2 else
            " (%d in a row)" % e.v2 if e.arg in (12, 13) else
            " (dropped #%d)" % e.v2 if e.arg == 14 and e.v2 else
            " (IOReturn 0x%x flipResult %d)" % (e.v4, e.v3 - (1 << 32) if e.v3 >> 31 else e.v3) if e.arg in (4, 5)
            else "", e.pb, e.pc)
    if n == "HOOK":
        return "%10.3f ms HOOK h%s %s(%d, %d) -> 0x%x (call %d)" % (
            ms, e.head if e.head != 0xff else "-", HOOK_KIND.get(e.arg, e.arg), e.v0, e.v1, e.v2, e.v3)
    if n == "SCAN":
        return "%10.3f ms SCAN h%s buf[%s] 0x%x %s%s%s%s bands %s (was %s)" % (
            ms, e.head if e.head != 0xff else "?", "%d" % e.arg if e.arg < 0xfd else {0xfe: "scanout", 0xfd: "front",
                                                                                        0xff: "-"}[e.arg],
            e.pa, "pixels" if e.flags & 1 else "empty", " CHANGED" if e.flags & 2 else "",
            " (re-sampled %d us later)" % e.v1 if e.flags & 16 else "",
            " IOSurface %d" % e.v3 if e.v3 else "", e.ba.hex(), e.bb.hex())
    if n == "SURFRES":
        return "%10.3f ms SURFRES 0x%x = IOSurface %s (how %d, %s, type 0x%02x, %d KiB)" % (
            ms, e.pa, e.v0 or "NONE", e.v2, {1: "a1", 2: "a2"}.get(e.v1, "plane %d" % e.v1), e.v4 & 0xff, e.v3)
    if n == "CF":
        x0, y0, x1, y1 = struct.unpack("<HHHH", e.ba)
        return "%10.3f ms CF h%s pick %d: front 0x%x -> target 0x%x from 0x%x, %d/%d rects box %d,%d-%d,%d %d KiB %d us -> 0x%x" % (
            ms, e.head if e.head != 0xff else "?", e.arg, e.pa, e.pb, e.pc, e.v1, e.v0, x0, y0, x1, y1, e.v2, e.v3, e.v4)
    if n == "PANEL":
        return "%10.3f ms PANEL h%d scans 0x%x%s (requested 0x%x, expected 0x%x)%s frame %d line %d" % (
            ms, e.head, e.pa, " (MC)" if e.flags & 2 else "", e.pb, e.pc, " FLIP PENDING" if e.flags & 1 else "",
            e.v0, e.v1)
    if n == "APERTURE":
        return "%10.3f ms APERTURE %s 0x%x (%d KiB, IOSurface %d)" % (ms, "add" if e.arg == 1 else "desc", e.pa, e.v0,
                                                                      e.v1)
    if n == "PAGEOFF":   # N41: the wait for WindowServer's GPU write, and the IOP flip's own wait
        w = ""
        if e.flags & (PAGEOFF_WAITED | PAGEOFF_WAIT_TIMEOUT | PAGEOFF_WAIT_CLEARED):
            w = " (waited %d us for WindowServer's GPU%s)" % (e.v3, " - TIMED OUT" if e.flags & PAGEOFF_WAIT_TIMEOUT
                                                               else " - CLEARED under it" if e.flags & PAGEOFF_WAIT_CLEARED
                                                               else "")
        elif e.flags & PAGEOFF_WAIT_SKIPPED:
            w = " (WindowServer's GPU write pending, NOT waited: the timeout budget was spent)"
        if e.arg == 2 and e.flags & PAGEOFF_IOP:
            return "%10.3f ms IOP flip wait, IOSurface %d%s" % (ms, e.v2, w)
        return "%10.3f ms PAGEOFF 0x%x %d KiB %s in %d us%s" % (ms, e.pa, e.v0, "copied" if e.arg == 1 else "NOT copied",
                                                               e.v1, w)
    if n in ("VMALLOC", "VMFREE"):
        park = ""
        if e.flags & VM_PARKED:
            park = " (the PARKED one handed back)" if n == "VMALLOC" else " PARKED, not freed"
        if n == "VMFREE" and e.flags & VM_PARK_GONE:
            park += " (a parked one RELEASED)"
        if n == "VMFREE" and e.flags & VM_FREE_LIVE:
            park += " (LIVE at free())"
        if n == "VMFREE" and e.flags & VM_OBJ_GONE:
            park += " (the object freed)"
        return "%10.3f ms %s 0x%x %d KiB%s%s%s" % (ms, n, e.pa, e.v0, "" if e.arg else " REFUSED 0x%x" % e.v4,
                                                   " while scanned by head %d" % e.head if n == "VMFREE" and e.flags & 0xe
                                                   else "", park)
    if n == "CS" and e.arg == 0xff:
        return "%10.3f ms CS REFUSED pid %d -> %d (that error %d times, %d not traced before this)" % (
            ms, e.v0, e.v4 - (1 << 32) if e.v4 >> 31 else e.v4, e.v1, e.v2)
    if n == "CS":
        return "%10.3f ms CS pid %d vmid %d ip %d ring %d seq %d ibs %d bos %d -> %d" % (
            ms, e.v0, e.v1, e.arg, e.flags, e.v2, e.v3 & 0xffff, e.v3 >> 16, e.v4 - (1 << 32) if e.v4 >> 31 else e.v4)
    return "%10.3f ms %s h%s arg %d flags 0x%x v %x %x %x %x %x pa 0x%x pb 0x%x pc 0x%x" % (
        ms, n, e.head if e.head != 0xff else "-", e.arg, e.flags, e.v0, e.v1, e.v2, e.v3, e.v4, e.pa, e.pb, e.pc)


def timeline(trace, head=None, n=60):
    """the last n accelerator events of a head (None: every head)"""
    evs = [e for e in trace.events if e.src == SRC_ACCEL and (head is None or e.head == head)]
    if not evs:
        return []
    t0 = trace.ns(trace.events[0], trace.events[0].src)
    return [fmt_event(trace, e, t0) for e in evs[-n:]]


def summary_lines(trace, ctx=None, last=60):
    """[(level, text)] for triage: hypotheses, the A/B, the coverage self-check"""
    out = []
    acc = [e for e in trace.events if e.src == SRC_ACCEL]
    core = [e for e in trace.events if e.src == SRC_CORE]
    out.append(("INFO", "%d accelerator events, %d core events, %d per-second rows, %d blob(s), %d log line(s)" %
                (len(acc), len(core), len(trace.secs), len(trace.headers), trace.lines)))
    for lv, q, text in trace.problems:
        if lv != "FAIL":   # FAILs are named by the coverage check (Q8)
            out.append((lv, "%s: %s" % (q, text)))
    for lv, name, text in hypotheses(trace, ctx):
        out.append((lv, "%s: %s" % (name, text)))
    # the backs without an IOSurface id (the join with the plugin's trace is
    # printed by tools/triage_reports.py, which reads both)
    out.extend(l for l in join_lines(trace) if l[0] != "INFO")
    valid = ab_validity(trace, (ctx or {}).get("ab_expected", False))
    for lv, text in valid:
        out.append((lv, text))
    lines, verdict = ab_table(trace)
    for l in lines:
        out.append(("INFO", "A/B " + l))
    if verdict:
        if any(lv == "FAIL" for lv, _ in valid):
            out.append(("INFO", "A/B verdict (NOT valid, the A/B was inconclusive): " + verdict[1]))
        else:
            out.append((verdict[0], "A/B verdict: " + verdict[1]))
    for q, ok, text in coverage(trace, ctx):
        out.append(("PASS" if ok else "FAIL", ("%s: %s" % (q, text)) if ok else
                    "trace incomplete: %s (%s)" % (q, text)))
    return out


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    n, head, files, texts = 60, None, [], []
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--timeline":
            n = int(argv[i + 1]); i += 2; continue
        if a == "--head":
            head = int(argv[i + 1]); i += 2; continue
        if a.endswith((".log", ".txt")):
            texts.append(open(a, errors="replace").read())
        else:
            files.append(a)
        i += 1
    tr = load(files, texts)
    for lv, t in summary_lines(tr):
        print("%-8s %s" % (lv, t))
    heads = sorted(set(e.head for e in tr.events if e.src == SRC_ACCEL and e.head != 0xff))
    for h in ([head] if head is not None else heads):
        print("== head %d: the last %d events" % (h, n))
        for l in timeline(tr, h, n):
            print("  " + l)


if __name__ == "__main__":
    main(sys.argv)
