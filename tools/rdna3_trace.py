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
every source on one clock.

  tools/rdna3_trace.py <file>... [--timeline N] [--head H]

tools/triage_reports.py imports it: verdicts per hypothesis (H-COPY,
H-B2, H-FREE, copy forward errors), the A/B side by side, the coverage
self-check of docs/AFTER-TESTS.md's questions Q1-Q11 and the last events
per head.
"""
import re
import struct
import sys

MAGIC = 0x52543352
VERSION = 1
EV_FMT = "<QIBBBBIIIIIIQQQ8s8s"       # struct rd3_trace_ev (80 bytes)
HDR_FMT = "<IIIIIIQQQQIIIIII"         # struct rd3_trace_hdr (80 bytes)
SEC_FMT = "<IBBBBHHHHHHHHHHI"         # struct rd3_trace_sec (32 bytes)
EV_SIZE, HDR_SIZE, SEC_SIZE = struct.calcsize(EV_FMT), struct.calcsize(HDR_FMT), struct.calcsize(SEC_FMT)
assert (EV_SIZE, HDR_SIZE, SEC_SIZE) == (80, 80, 32)

SRC_ACCEL, SRC_CORE = 0, 1
READ_ALL, READ_STREAM, READ_SEC = 0, 1, 2
TYPES = {0: "none", 1: "SWAP", 2: "CF", 3: "SHAPE", 4: "VMALLOC", 5: "VMFREE", 6: "APERTURE", 7: "PAGEOFF",
         8: "SURFGEM", 9: "MODE", 10: "FLIP", 11: "LATCH", 12: "SYNC", 13: "GONE", 14: "MODESET", 15: "SCAN",
         16: "CS", 17: "FENCE", 18: "GPUWAIT", 19: "RESET", 20: "VMFAULT", 21: "SURFRES", 22: "PANEL"}
T = dict((v, k) for k, v in TYPES.items())
SWAP_ARG = {0: "skip", 1: "flip", 2: "flip refused, copied", 3: "flip refused", 4: "copy", 5: "hold", 6: "copy FAILED",
            7: "no head"}
RC = {0: "no res", 1: "no vidmem", 2: "no kva", 3: "no geometry", 4: "empty", 5: "pixels"}
F_A1_FRONT, F_A2_FRONT, F_A1_PIX, F_A2_PIX, F_EMPTY, F_FRONT_OK, F_DOUBLE, F_WAITED = 1, 2, 4, 8, 16, 32, 64, 128
TARGET = {0: "-", 1: "home", 2: "D", 3: "home DIRECT"}
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
        self._seen = set()

    def add_event(self, src, vals):
        ev = Ev(src, vals)
        key = (src, ev.seq, ev.t)
        if key in self._seen:
            return
        self._seen.add(key)
        self.events.append(ev)

    def add_blob(self, data, where=""):
        """parses one file's bytes: binary blobs back to back, else a 'sysctl -x' hex dump"""
        if len(data) >= 4 and struct.unpack_from("<I", data, 0)[0] != MAGIC:
            hx = re.findall(r"0x([0-9a-fA-F]{16,})", data.decode("latin-1", "replace"))
            if hx:
                data = bytes.fromhex("".join(hx))
        off = 0
        n = 0
        while off + HDR_SIZE <= len(data):
            h = struct.unpack_from(HDR_FMT, data, off)
            hdr = dict(zip(("magic", "version", "ev_size", "cap", "count", "swap_mode", "written", "lost", "now",
                            "nomem", "salt_check", "source", "tb_numer", "tb_denom", "read_kind", "ab"), h))
            if hdr["magic"] != MAGIC:
                break
            hdr["where"] = where
            off += HDR_SIZE
            size = hdr["ev_size"] * hdr["count"]
            body = data[off:off + size]
            hdr["truncated"] = len(body) < size
            off += size
            self.headers.append(hdr)
            if hdr["tb_numer"] and hdr["tb_denom"]:
                self.tb[hdr["source"]] = (hdr["tb_numer"], hdr["tb_denom"])
            if hdr["read_kind"] == READ_SEC:
                for i in range(len(body) // SEC_SIZE):
                    v = struct.unpack_from(SEC_FMT, body, i * SEC_SIZE)
                    self.secs.append(dict(zip(("sec", "head", "mode", "ab", "pad", "swaps", "shown", "held", "empty",
                                               "black", "cf", "cferr", "aperture", "vmrefused", "freedshown",
                                               "events"), v)))
            else:
                for i in range(len(body) // EV_SIZE):
                    self.add_event(hdr["source"], struct.unpack_from(EV_FMT, body, i * EV_SIZE))
                    n += 1
        return n

    TR = re.compile(r"\btr (\d+) (\d+) (\d+) (\d+) (\d+) (\d+) ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+) "
                    r"([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+)")

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
        return self

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
    return tr.finish()


# ---------------------------------------------------------------- analysis

def swap_back(e):
    """(back phys, back rc, front phys, front rc) of a SWAP: the argument that
    is a head's framebuffer resource is the front, else a1 (submitSwapCopy)"""
    rc1, rc2 = e.v1 & 0xf, (e.v1 >> 4) & 0xf
    if e.flags & F_A2_FRONT and not e.flags & F_A1_FRONT:
        return e.pa, rc1, e.pb, rc2, e.ba, e.bb
    return e.pb, rc2, e.pa, rc1, e.bb, e.ba


def swap_mode(e):
    return (e.v1 >> 12) & 0xf


def phase_of(trace):
    """each accelerator event's A/B phase (MODESET arg 1 events; before the
    first one: the phase of the oldest stream header that carried one)"""
    cur = 0
    out = {}
    for e in trace.events:
        if e.src == SRC_ACCEL and e.type == T["MODESET"] and e.arg == 1:
            cur = e.v1
        out[id(e)] = cur
    return out


def per_head_buffers(trace):
    """{head: {back phys: [swaps, empty, pixels, aperture hits]}} and the aperture count per phys"""
    ap = {}
    for e in trace.by("APERTURE"):
        if e.arg == 1:
            ap[e.pa] = ap.get(e.pa, 0) + 1
    heads = {}
    for e in trace.by("SWAP"):
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


def segments(trace):
    """the swaps per (swap mode, A/B phase): their counts side by side"""
    ph = phase_of(trace)
    seg = {}
    for e in trace.by("SWAP"):
        k = (swap_mode(e), ph.get(id(e), 0))
        s = seg.setdefault(k, {"swaps": 0, "shown": 0, "held": 0, "black": 0, "empty": 0, "failed": 0, "direct": 0,
                               "double": 0, "waits": 0, "wait_us": 0, "copy_us": 0, "events": []})
        s["swaps"] += 1
        shown = e.arg in (1, 2, 4)
        s["shown"] += shown
        s["held"] += e.arg in (0, 5)
        s["empty"] += bool(e.flags & F_EMPTY)
        s["black"] += bool(e.flags & F_EMPTY) and shown
        s["failed"] += e.arg in (3, 6)
        s["direct"] += e.arg == 4 and ((e.v1 >> 16) & 0xf) == 3
        s["double"] += bool(e.flags & F_DOUBLE)
        if e.flags & F_WAITED:
            s["waits"] += 1
            s["wait_us"] += e.v3
        s["copy_us"] += e.v2
        s["events"].append(e)
    for s in seg.values():
        s["inplace"], s["inplace_checked"] = inplace_rewrites(s["events"])
    return seg


def hypotheses(trace):
    """[(level, name, text)]: CRITICAL / FAIL / WARN / INFO / PASS, the
    hypothesis each trace pattern names"""
    out = []
    heads, ap = per_head_buffers(trace)
    # H-B2: a back the swaps name that never receives WindowServer's frame
    for h, bufs in sorted(heads.items()):
        drawn = [(p, st) for p, st in bufs.items() if st[2] * 2 >= st[0] and st[0] >= 2]
        for p, st in sorted(bufs.items()):
            if st[0] >= 10 and st[1] >= 0.9 * st[0] and drawn:
                where = ", ".join("0x%x (%d of %d swaps with pixels, %d aperture writes)" % (q, s2[2], s2[0], ap.get(q, 0))
                                  for q, s2 in drawn)
                out.append(("FAIL", "H-B2",
                            "head %d: the back 0x%x was named by %d swaps and read EMPTY at %d of them, %d aperture "
                            "(page-on) writes into it; WindowServer's frames land in %s - a back that never receives "
                            "a frame (copying or flipping it shows black)" % (h, p, st[0], st[1], ap.get(p, 0), where)))
    # H-COPY: mode 0 shows the buffer WindowServer draws into
    seg = segments(trace)
    for (mode, phase), s in sorted(seg.items()):
        if mode != 0:
            continue
        if s["inplace_checked"] >= 20 and s["inplace"] >= 0.25 * s["inplace_checked"]:
            out.append(("FAIL", "H-COPY", "mode 0%s: %d of %d flipped buffers were rewritten while on the panel and shown "
                        "again - WindowServer treats the back as free when the swap returns (copy semantics): the "
                        "copy (swap mode 1) is the fix" % (" (" + AB_NAME.get(phase, str(phase)) + ")" if phase else "",
                                                          s["inplace"], s["inplace_checked"])))
        elif s["inplace"]:
            out.append(("WARN", "H-COPY", "mode 0: %d of %d flipped buffers rewritten in place" %
                        (s["inplace"], s["inplace_checked"])))
    # H-FREE: VRAM freed while a head scanned it
    freed = [e for e in trace.by("VMFREE") if e.flags & 0x0e]
    if freed:
        out.append(("WARN", "H-FREE", "%d buffer(s) freed while a head scanned them (e.g. 0x%x on head %s) - the panel "
                    "reads VRAM that may be reused" % (len(freed), freed[0].pa, freed[0].head if freed[0].head != 0xff
                                                       else "?")))
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
    mism = [e for e in trace.by("PANEL") if e.pc and e.pa and not e.flags & 1 and e.pa != e.pc]
    if mism:
        out.append(("WARN", "PANEL", "%d swap(s) found the HUBP scanning 0x%x instead of 0x%x (first) - the panel "
                    "is not where the driver thinks" % (len(mism), mism[0].pa, mism[0].pc)))
    faults = trace.by("VMFAULT")
    if faults:
        out.append(("WARN", "GPU", "%d GPU page fault(s) (first VMID %d at 0x%x, pid %d)" %
                    (len(faults), faults[0].v0, faults[0].pa, faults[0].v2)))
    resets = trace.by("RESET")
    if resets:
        out.append(("WARN", "GPU", "%d hang recover%s (first IP %d seq %d)" % (len(resets), "y" if len(resets) == 1
                                                                              else "ies", resets[0].arg, resets[0].v2)))
    return out


def ab_table(trace):
    """the A/B side by side: one line per (mode, phase) and a verdict"""
    seg = segments(trace)
    lines = []
    for (mode, phase), s in sorted(seg.items(), key=lambda kv: (kv[0][1], kv[0][0])):
        lines.append("mode %d %-10s %5d swaps: %5d shown, %4d held/skipped, %4d empty backs, %4d BLACK frames, "
                     "%4d failed, %4d in-place rewrites of %d, %4d tear-free copies, %4d direct (may tear), "
                     "waits %d (avg %d us), copy avg %d us" %
                     (mode, "(" + AB_NAME.get(phase, str(phase)) + ")", s["swaps"], s["shown"], s["held"], s["empty"],
                      s["black"], s["failed"], s["inplace"], s["inplace_checked"], s["double"], s["direct"],
                      s["waits"], s["wait_us"] // s["waits"] if s["waits"] else 0,
                      s["copy_us"] // max(1, s["double"] + s["direct"])))
    m1 = [s for (m, _), s in seg.items() if m in (1, 2)]
    m0 = [s for (m, _), s in seg.items() if m == 0]
    verdict = None
    if m1 and m0:
        b1, b0 = sum(s["black"] for s in m1), sum(s["black"] for s in m0)
        i0 = sum(s["inplace"] for s in m0)
        f1 = sum(s["failed"] for s in m1)
        if not b1 and not f1 and i0:
            verdict = ("PASS", "the copy (mode 1) showed no black frame and no failed copy, the flip (mode 0) %d "
                               "in-place rewrites and %d black frames: mode 1 fixes the model mismatch" % (i0, b0))
        elif b1 or f1:
            verdict = ("FAIL", "mode 1 still showed %d black frame(s) and %d failed copies (mode 0: %d black, %d "
                               "in-place)" % (b1, f1, b0, i0))
        else:
            verdict = ("INFO", "neither mode showed black frames or in-place rewrites in this window")
    return lines, verdict


def coverage(trace, ctx=None):
    """[(question, ok, text)]: can the report answer Q1-Q11 (docs/AFTER-TESTS.md)?
    ctx: swaps (the counters' swap count), cf_calls, ab_ran (autoreport ran the A/B),
    core_expected (Metal / RADV ran)"""
    ctx = ctx or {}
    out = []
    swaps = trace.by("SWAP")
    acc = [h for h in trace.headers if h["source"] == SRC_ACCEL and h["read_kind"] != READ_SEC]
    core = [h for h in trace.headers if h["source"] == SRC_CORE]
    have_accel = bool(acc) or any(e.src == SRC_ACCEL for e in trace.events)
    if not have_accel:
        out.append(("Q1-Q8", False, "no accelerator trace in the report (36-frame-trace.bin, the -trace-stream.bin "
                                    "next to it, or 'tr' lines)"))
    else:
        out.append(("Q1", bool(swaps) or not ctx.get("swaps"),
                    "%d swap events (counters: %s swaps)" % (len(swaps), ctx.get("swaps", "?"))))
        scans = [e for e in trace.by("SCAN") if e.arg not in (0xfe, 0xfd) and not e.flags & 16]
        out.append(("Q2", bool(scans) or not swaps, "%d samples of the surfaces' other buffers" % len(scans)))
        empty = sum(1 for e in swaps if e.flags & F_EMPTY and e.head != 0xff)
        late = [e for e in trace.by("SCAN") if e.flags & 16]
        out.append(("Q3", bool(late) or empty < 8, "%d empty backs, %d delayed re-samples" % (empty, len(late))))
        cfs = trace.by("CF")
        out.append(("Q4", bool(cfs) or not ctx.get("cf_calls"), "%d copy forward events (counters: %s calls)" %
                    (len(cfs), ctx.get("cf_calls", "?"))))
        out.append(("Q5", True, "%d allocations, %d frees, %d shapes" % (len(trace.by("VMALLOC")),
                                                                       len(trace.by("VMFREE")), len(trace.by("SHAPE")))))
        panel = trace.by("PANEL")
        known = [e for e in panel if e.pa]
        out.append(("Q6", bool(known) or not swaps,
                    "%d panel reads, %d with the HUBP's address%s" % (len(panel), len(known),
                                                                      "" if known or not panel else
                                                                      " (a core without rdna3DisplayScanAddr)")))
        ms = trace.by("MODESET")
        out.append(("Q7", bool([e for e in ms if e.arg == 1]) or not ctx.get("ab_ran"),
                    "%d swap mode / A/B changes, %d display mode events" % (len(ms), len(trace.by("MODE")))))
        streams = [h for h in acc if h["read_kind"] == READ_STREAM]
        if streams:
            # autoreport.sh discards the read that moves the cursor to the A/B's
            # start: every stream read in the files is inside the window
            lost = sum(h["lost"] for h in streams)
            ok8 = lost == 0
            why = "%d stream reads, %d events lost between them" % (len(streams), lost)
        else:
            lost = max([h["lost"] for h in acc] or [0])
            ok8 = lost == 0 or not ctx.get("ab_ran")
            why = "no stream; the ring lost %d events before the read" % lost
        if not trace.secs:
            why += "; NO per-second table"
            ok8 = ok8 and not ctx.get("ab_ran")
        out.append(("Q8", ok8, why))
    cs = trace.by("CS")
    out.append(("Q9", bool(core) or bool(cs) or not ctx.get("core_expected"),
                "%d command submissions, %d fences, %d waits, %d resets, %d faults%s" %
                (len(cs), len(trace.by("FENCE")), len(trace.by("GPUWAIT")), len(trace.by("RESET")),
                 len(trace.by("VMFAULT")), "" if core or cs else " (no core trace: 36-drm-trace.bin)")))
    sg = trace.by("SURFGEM") + trace.by("SURFRES")
    out.append(("Q10", bool(sg) or not have_accel, "%d IOSurface answers / pairs" % len(sg)))
    out.append(("Q11", bool(trace.tb) or not trace.headers, "timebase %s" %
                (", ".join("src %d %d/%d" % (k, v[0], v[1]) for k, v in sorted(trace.tb.items())) or "none")))
    return out


def fmt_event(trace, e, t0=0):
    ms = (trace.ns(e, e.src) - t0) / 1e6
    n = e.name
    if n == "SWAP":
        back, rcb, front, rcf, bb, fb = swap_back(e)
        return ("%10.3f ms SWAP #%d h%s mode %d %s: back 0x%x (%s, %s) front 0x%x (%s) -> panel 0x%x target %s%s%s "
                "copy %d us wait %d us" % (ms, e.v0, e.head if e.head != 0xff else "?", swap_mode(e),
                                          SWAP_ARG.get(e.arg, e.arg), back, RC.get(rcb, rcb), bb.hex(), front,
                                          RC.get(rcf, rcf), e.pc, TARGET.get((e.v1 >> 16) & 0xf, "?"),
                                          " FRONT-OK" if e.flags & F_FRONT_OK else "",
                                          " tear-free" if e.flags & F_DOUBLE else "", e.v2, e.v3))
    if n == "SCAN":
        return "%10.3f ms SCAN h%s buf[%s] 0x%x %s%s%s bands %s (was %s)" % (
            ms, e.head if e.head != 0xff else "?", "%d" % e.arg if e.arg < 0xfd else {0xfe: "scanout", 0xfd: "front",
                                                                                        0xff: "-"}[e.arg],
            e.pa, "pixels" if e.flags & 1 else "empty", " CHANGED" if e.flags & 2 else "",
            " (re-sampled %d us later)" % e.v1 if e.flags & 16 else "", e.ba.hex(), e.bb.hex())
    if n == "CF":
        x0, y0, x1, y1 = struct.unpack("<HHHH", e.ba)
        return "%10.3f ms CF h%s pick %d: front 0x%x -> target 0x%x from 0x%x, %d/%d rects box %d,%d-%d,%d %d KiB %d us -> 0x%x" % (
            ms, e.head if e.head != 0xff else "?", e.arg, e.pa, e.pb, e.pc, e.v1, e.v0, x0, y0, x1, y1, e.v2, e.v3, e.v4)
    if n == "PANEL":
        return "%10.3f ms PANEL h%d scans 0x%x (requested 0x%x, expected 0x%x)%s frame %d line %d" % (
            ms, e.head, e.pa, e.pb, e.pc, " FLIP PENDING" if e.flags & 1 else "", e.v0, e.v1)
    if n == "APERTURE":
        return "%10.3f ms APERTURE %s 0x%x (%d KiB, IOSurface %d)" % (ms, "add" if e.arg == 1 else "desc", e.pa, e.v0,
                                                                      e.v1)
    if n in ("VMALLOC", "VMFREE"):
        return "%10.3f ms %s 0x%x %d KiB%s%s" % (ms, n, e.pa, e.v0, "" if e.arg else " REFUSED 0x%x" % e.v4,
                                                 " while scanned by head %d" % e.head if n == "VMFREE" and e.flags & 0xe
                                                 else "")
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
    for lv, name, text in hypotheses(trace):
        out.append((lv, "%s: %s" % (name, text)))
    lines, verdict = ab_table(trace)
    for l in lines:
        out.append(("INFO", "A/B " + l))
    if verdict:
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
