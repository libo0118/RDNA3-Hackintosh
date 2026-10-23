"""tools/triage_reports.py's DRIVER-PLAN N41 rows (n41_rows) and tools/rdna3_trace.py's
vram_moves / H-VMMOVE on synthetic data: parking on and working (INFO), off (WARN), a
dirty mark of a moved allocation, a restore that failed, parks costing VRAM (WARN), the
page-off's wait with timeouts (WARN) and pending marks never released (FAIL), marks from
another process (WARN); counters missing from a dump of these sources (WARN, the
collector's grep) against an older kext (INFO) - never silence. The trace side: an
imported surface whose VRAM came back elsewhere (the 2026-10-17d pattern, WARN) against
one that was parked and handed back (INFO only).
Run: python3 -I -B tests/test_triage_park.py"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import triage_reports as tr  # noqa: E402
import rdna3_trace as rt  # noqa: E402

fails = []


def check(cond, what):
    if not cond:
        fails.append(what)


def dump(**kv):
    return "\n".join("debug.rdna3accel_%s: %d" % (k, v) for k, v in kv.items()) + "\n"


def levels(rows, name=None):
    return [lvl for lvl, n, _ in rows if name is None or n == name]


CUR = "RDNA3Accelerator: build %s\n" % tr.fb_source_hash(tr.ACCEL_HASH_FILES)
OLD = "RDNA3Accelerator: build 0123456789\n"
GOOD = dict(vm_sticky=1, vm_parked=40, vm_unparked=38, vm_park_released=2, vm_park_grow=1, vm_parked_bytes=8294400,
            vm_released=5, vm_restored=3, vm_restore_miss=0, vm_dirty_moved=0, vm_reparked=0, vm_free_live=0,
            vm_refused=0, surface_wait=1, surface_wait_pending=100, surface_wait_ready=100, surface_wait_waits=4,
            surface_wait_maxus=900, surface_wait_timeouts=0, surface_wait_notws=0, surface_wait_evicted=0,
            surface_wait_saturated=0)

r = tr.n41_rows(dump(**GOOD), CUR)
check(set(levels(r)) == {"INFO"}, "a good run is INFO only: %s" % r)
check(any("40 parked, 38 handed back" in t for _, _, t in r), "the park counts are printed: %s" % r)

bad = dict(GOOD, vm_sticky=0)
check("WARN" in levels(tr.n41_rows(dump(**bad), CUR), "VRAM parking"), "parking off is a WARN")
bad = dict(GOOD, vm_dirty_moved=3)
check("WARN" in levels(tr.n41_rows(dump(**bad), CUR)), "a dirty mark of a moved allocation is a WARN")
bad = dict(GOOD, vm_restore_miss=2)
check("WARN" in levels(tr.n41_rows(dump(**bad), CUR)), "a failed restore is a WARN")
bad = dict(GOOD, vm_refused=4)
check("WARN" in levels(tr.n41_rows(dump(**bad), CUR)), "VRAM refused while parked is a WARN")
bad = dict(GOOD, vm_reparked=1)
check("WARN" in levels(tr.n41_rows(dump(**bad), CUR)), "a replaced park is a WARN")
bad = dict(GOOD, surface_wait_timeouts=2)
check("WARN" in levels(tr.n41_rows(dump(**bad), CUR), "surface wait"), "a wait timeout is a WARN")
bad = dict(GOOD, surface_wait_ready=0)
check("FAIL" in levels(tr.n41_rows(dump(**bad), CUR), "surface wait"), "pending marks never released FAIL")
bad = dict(GOOD, surface_wait_notws=1)
check("WARN" in levels(tr.n41_rows(dump(**bad), CUR), "surface wait"), "a mark from another process is a WARN")
bad = dict(GOOD, surface_wait_pending=0, surface_wait_ready=0)
r = tr.n41_rows(dump(**bad), CUR, "surface wait (N41): WindowServer's IOSurface VRAM writes are marked newest after "
                "the GPU only (old order) (libdrm_xnu marks ABSENT, ...)")
check(levels(r, "surface wait") == ["INFO"] and any("the plugin says" in t for _, _, t in r),
      "no marks taken: INFO with the plugin's reason: %s" % r)
# missing counters: a WARN for these sources, INFO for an older kext
r = tr.n41_rows("debug.rdna3accel_a3display: 1\n", CUR)
check(levels(r) == ["WARN"], "these sources without vm_sticky in the dump: WARN (%s)" % r)
r = tr.n41_rows("debug.rdna3accel_a3display: 1\n", OLD)
check(levels(r) == ["INFO"], "an older kext: INFO (%s)" % r)
nowait = dict(GOOD)
del nowait["surface_wait"]
check("WARN" in levels(tr.n41_rows(dump(**nowait), CUR), "surface wait"), "surface_wait missing: WARN")


# ---- the trace: rdna3_trace.vram_moves on synthetic events ----
class Ev:
    def __init__(self, typ, **kw):
        self.src = rt.SRC_ACCEL
        self.type = [k for k, v in rt.TYPES.items() if v == typ][0]
        self.arg = kw.get("arg", 1)
        self.flags = kw.get("flags", 0)
        self.obj = kw.get("obj", 0)
        self.pa = kw.get("pa", 0)
        self.v0 = kw.get("v0", 0)
        self.v3 = kw.get("v3", 0)
        self.seq = kw.get("seq", 0)
        self.head = 0xff


class Trace:
    def __init__(self, events):
        self.events = events

    def by(self, name):
        return [e for e in self.events if rt.TYPES.get(e.type) == name]


VM = 0x77
moved = Trace([Ev("VMALLOC", obj=VM, pa=0x1000), Ev("SURFGEM", arg=1, obj=VM, pa=0x1000, v0=37),
               Ev("VMFREE", obj=VM, pa=0x1000), Ev("VMALLOC", obj=VM, pa=0x9000),
               Ev("SURFGEM", arg=2, obj=VM, pa=0x9000, v0=37)])
m = rt.vram_moves(moved)
check(len(m["moved"]) == 1 and m["dirty_moved"] == 1 and m["freed"] == 1, "the 17d pattern is found: %s" % m)
check("WARN" in [lvl for lvl, n, _ in rt.h_vram_moved(moved) if n == "H-VMMOVE"], "H-VMMOVE WARNs")
parked = Trace([Ev("VMALLOC", obj=VM, pa=0x1000), Ev("SURFGEM", arg=1, obj=VM, pa=0x1000, v0=37),
                Ev("VMFREE", obj=VM, pa=0x1000, flags=rt.VM_PARKED),
                Ev("VMALLOC", obj=VM, pa=0x1000, flags=rt.VM_PARKED),
                Ev("SURFGEM", arg=2, obj=VM, pa=0x1000, v0=37),
                Ev("PAGEOFF", obj=0x5, flags=rt.PAGEOFF_WAITED, v3=800)])
m = rt.vram_moves(parked)
check(not m["moved"] and m["parked"] == 1 and m["reused"] == 1, "a parked surface never moves: %s" % m)
rows = rt.h_vram_moved(parked)
check(set(lvl for lvl, _, _ in rows) == {"INFO"}, "parking and a wait in time are INFO only: %s" % rows)
late = Trace(parked.events + [Ev("PAGEOFF", obj=0x5, flags=rt.PAGEOFF_WAIT_TIMEOUT, v3=100000, seq=9)])
check("WARN" in [lvl for lvl, n, _ in rt.h_vram_moved(late) if n == "H-SURFWAIT"], "a wait timeout WARNs")
ev = Ev("VMFREE", obj=VM, pa=0x1000, flags=rt.VM_PARKED, v0=8100)
check("PARKED, not freed" in rt._fmt_event(parked, ev, 1.0, "VMFREE"), "VMFREE parked is decoded")
ev = Ev("VMALLOC", obj=VM, pa=0x1000, flags=rt.VM_PARKED, v0=8100)
check("the PARKED one handed back" in rt._fmt_event(parked, ev, 1.0, "VMALLOC"), "VMALLOC reuse is decoded")

if fails:
    print("test_triage_park: FAIL\n  " + "\n  ".join(fails))
    sys.exit(1)
print("test_triage_park: all checks passed")
