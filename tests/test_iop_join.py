#!/usr/bin/env python3
"""DRIVER-PLAN N68: the kernel's RD3_TR_IOP event as tools/rdna3_trace.py decodes it, and tools/plugin_trace.py's join
of it with WindowServer's surface-out (the present age per frame, a publish presented twice, no publish)."""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import plugin_trace as pt  # noqa: E402
import rdna3_trace as rt  # noqa: E402

FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("FAIL: " + what)


MS = 1000000   # ns, with timebase 1/1 both clocks are ns


def iop(seq, t_ms, head, surf, path=3, flags=0x20 | 0x01, frame=100, line=1085, inblank=1, cls=1, b=4, fb=0xff,
        txn=900, stale=0, wait=0):
    bb = struct.pack("<II", stale, wait)
    return (t_ms * MS, seq, rt.T["IOP"], head, path, flags, 0, surf, 77, 50, 400, frame, 0x1000,
            line | (inblank << 32), txn, bytes([fb, cls, b, 0, 0, 0, 0, 0]), bb)


k = rt.Trace()
k.add_event(rt.SRC_ACCEL, iop(1, 20, 0, 5, frame=100))
k.add_event(rt.SRC_ACCEL, iop(2, 37, 0, 6, frame=101, flags=0x20 | 0x08 | 0x02 | 0x04, stale=3000, wait=1200))
k.add_event(rt.SRC_ACCEL, iop(3, 54, 0, 5, frame=102))                       # surface 5 again, no new publish: REUSED
k.add_event(rt.SRC_ACCEL, iop(4, 60, 1, 9, path=1, flags=0x40, fb=1))       # head 1, no publish at all, no VBL
k.add_event(rt.SRC_ACCEL, iop(5, 61, 1, 0))                                 # no surface: not joined

e = k.by("IOP")[1]
f = rt.iop_fields(e)
check(f["path"] == 3 and f["surf"] == 6 and f["stale_us"] == 3000 and f["wait_us"] == 1200 and f["to_d"] and f["stale"]
      and f["stale_moved"] and f["frame"] == 101 and f["scanline"] == 1085 and f["inblank"] and f["fb"] is None, "fields: %r" % f)
line = rt._fmt_event(k, e, 37.0, "IOP")
check("IOP h0 latched CPU surface 0x6" in line and "(D)" in line and "stale wait 3000" in line and "latch wait 1200" in line
      and "frame 101 line 1085 blank (next, blank)" in line and "STALE (moved)" in line, "event line: %s" % line)
f4 = rt.iop_fields(k.by("IOP")[3])
check(f4["fb"] == "vram" and f4["frame"] is None and f4["cls"] is None, "fallback, no VBL: %r" % f4)
check("FALLBACK vram" in rt._fmt_event(k, k.by("IOP")[3], 60.0, "IOP"), "fallback line")


class R(object):
    def __init__(self, abs_ns, seq, surf, path=7):
        self.kind, self.abs, self.seq, self.surf, self.path = pt.K_SURF_OUT, abs_ns, seq, surf, path


class P(object):
    def __init__(self, proc, recs):
        self.proc, self.recs, self.tb_n, self.tb_d = proc, recs, 1, 1


ws = P("WindowServer", [R(10 * MS, 1, 5), R(30 * MS, 2, 6), R(36 * MS, 3, 6), R(50 * MS, 4, 7)])
other = P("Terminal", [R(53 * MS, 9, 5)])   # another process's publish of the same id: not WindowServer's, ignored
rows, dropped = pt.iop_join([ws, other], k)
check(len(rows) == 4 and dropped == 1, "joined rows: %r, dropped %d" % (rows, dropped))
check(abs(rows[0][3] - 10.0) < 1e-6 and not rows[0][4], "frame 1 age 10 ms: %r" % (rows[0],))
check(abs(rows[1][3] - 1.0) < 1e-6, "frame 2: the newest publish before it (36 ms): %r" % (rows[1],))
check(abs(rows[2][3] - 44.0) < 1e-6 and rows[2][4], "frame 3 re-presents publish 1: %r" % (rows[2],))
check(rows[3][3] is None, "head 1: no publish: %r" % (rows[3],))
out = pt.iop_join_lines(rows, frames=2, dropped=dropped)
check(out[0].startswith("IOP join head 0: 3 frame(s) (latched CPU 3)") and "1 over 16.7 ms" in out[0]
      and "present age median 10.0 ms" in out[0]
      and "1 presented a publish ALREADY presented" in out[0] and "next 3" in out[0], "head 0 summary: %s" % out[0])
check(out[1].startswith("IOP join head 1: 1 frame(s) (direct CPU 1)") and "1 with no publish" in out[1]
      and "present age n/a (no frame joined)" in out[1], "head 1: %s" % out[1])
check(out[2].startswith("WARN IOP join head 1: only 0 of 1 frame(s) joined"), "S4 WARN: %s" % out[2])
check(out[3] == "IOP join: 1 IOP event(s) without an IOSurface (a VRAM-resource source) not joined", "S4 dropped: %s" % out[3])
check(len(out) == 6 and "REUSED" not in out[5] and "age ?" in out[5], "frame lines: %r" % out[4:])
check(not any(x.startswith("WARN IOP join head 0") for x in out), "head 0 joined 3 of 3: no WARN")
check(pt.iop_join_lines([])[0].startswith("IOP join: no RD3_TR_IOP"), "no events")
check(pt.iop_join_lines([], dropped=2)[1].startswith("IOP join: 2 IOP event(s) without"), "no rows, dropped said")

if FAILS:
    sys.exit("test_iop_join: %d FAILED" % len(FAILS))
print("test_iop_join: ok")
