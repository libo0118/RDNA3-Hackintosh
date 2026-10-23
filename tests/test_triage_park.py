"""tools/triage_reports.py's DRIVER-PLAN N41 rows (n41_rows) and tools/rdna3_trace.py's
vram_moves / H-VMMOVE on synthetic data: parking on and working (INFO), off (WARN), a
dirty mark of a moved allocation, a restore that failed, parks costing VRAM (WARN), the
page-off's wait with timeouts (WARN) and pending marks never released (FAIL), marks from
another process (WARN); counters missing from a dump of these sources (WARN, the
collector's grep) against an older kext (INFO) - never silence. The trace side: an
imported surface whose VRAM came back elsewhere (the 2026-10-17d pattern, WARN) against
one that was parked and handed back (INFO only).
ECC review of N41: WindowServer's plugin lines read from 24-log-coredisplay.txt (every gate, ENOSYS for good,
back-offs, refusals, early readies, the stats line), MOOT with rdna3a3ws=0, the parking's verdicts (never handed
back, nothing parked, much parked at the end, release failures, alloc/dealloc not a leak), lost readies, cleared and
IOP flip waits apart, missing keys named, the SUMMARY rows from a synthetic archive; the trace: no H-VMMOVE for an
object address reused after free() (a grow still moves), plain frees in H-VMPARK, IOP waits apart.
Security / round-2 review of N41: the mark gate's rows (notreal, the marking uid), MOOT refusals and their
back-offs (INFO), the timeout budget, lost readies, the parks under pressure / past the cap, readies at the
command buffer's end; the trace: a NOBO park release is no move, a skipped wait WARNs.
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
# ECC review of N41: every key the accelerator of these sources exports
GOOD.update(dict((k, 0) for k in tr.N41_VM_KEYS + tr.N41_SW_KEYS if k not in GOOD))
GOOD.update(vm_alloc=50, vm_dealloc=45, vm_released_bytes=5 << 20, surface_wait_ms=100, surface_wait_late=0,
            surface_wait_us=2000, surface_wait_busy=1, vm_park_cap_mb=512, surface_wait_mark_uid=88,
            surface_wait_mark_pid=148, surface_wait_ws_pid=148)
A3WS = "debug.rdna3accel_a3ws: 1\n"

r = tr.n41_rows(dump(**GOOD) + A3WS, CUR)
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
# silent-failure review 1 / 2: no marks taken - MOOT with rdna3a3ws=0, else a WARN with WindowServer's plugin's
# reason, read from ITS log (24-log-coredisplay.txt), not 26-log-metal-plugin.txt
bad = dict(GOOD, surface_wait_pending=0, surface_wait_ready=0)
CD_OLD = ("2026-10-23 10:00:00.000 Df WindowServer[148:9a2] (RDNA3MTLDriver) NVMTL: surface wait (N41): WindowServer's "
          "IOSurface VRAM writes are marked newest after the GPU only (old order) (libdrm_xnu marks ABSENT, try-lock "
          "dirty present, dirty present; NVK surface dirty absent; headless 0; NVMTL_SURFACE_WAIT=unset)\n")
r = tr.n41_rows(dump(**bad) + "debug.rdna3accel_a3ws: 0\n", CUR, "", CD_OLD)
check(any(lv == "INFO" and t.startswith("MOOT: rdna3a3ws=0") for lv, _, t in r), "a3ws 0, no marks: MOOT (%s)" % r)
r = tr.n41_rows(dump(**bad) + A3WS, CUR, "", CD_OLD)
check(any(lv == "WARN" and "libdrm_xnu marks ABSENT" in t for lv, n, t in r if n == "surface wait"),
      "a3ws 1, no marks: WARN naming the gate from 24-log-coredisplay.txt (%s)" % r)
r = tr.n41_rows(dump(**bad) + A3WS, CUR, CD_OLD, "")
check(any("libdrm_xnu marks ABSENT" in t for _, _, t in r), "the other processes' log is read too")
CD_ENV = CD_OLD.replace("libdrm_xnu marks ABSENT", "libdrm_xnu marks present").replace("=unset", "=0")
r = tr.n41_rows(dump(**bad) + A3WS, CUR, "", CD_ENV)
check(any("NVMTL_SURFACE_WAIT=0" in t for lv, _, t in r if lv == "WARN"), "NVMTL_SURFACE_WAIT=0 named (%s)" % r)
CD_ON = ("WindowServer[148:9a2] (RDNA3MTLDriver) NVMTL: surface wait (N41): WindowServer's IOSurface VRAM writes are "
         "marked pending + newest at commit, ready after the GPU (the kernel's page-off waits) (libdrm_xnu marks "
         "present, try-lock dirty present, dirty present; NVK surface dirty absent; headless 0; NVMTL_SURFACE_WAIT=unset)\n")
CD_GOOD = CD_ON + ("WindowServer[148:9a2] (RDNA3MTLDriver) NVMTL: surface wait (N41): pending mark of surface 77 refused "
                   "(errno 6, Device not configured) - the kext has no marks: old order for good (1 back-off(s))\n")
r = tr.n41_rows(dump(**bad) + A3WS, CUR, "", CD_GOOD)
check(any(lv == "WARN" and "old order for good" in t for lv, _, t in r), "ENOSYS: old order for good WARN (%s)" % r)
CD_BACK = CD_ON + "".join("WindowServer[148:9a2] NVMTL: surface wait (N41): pending mark of surface %d refused (errno 19, "
                          "Operation not supported by device) - old order, asked again in 1 s (%d back-off(s))\n" % (i, i)
                          for i in (1, 2)) + "WindowServer[148:9a2] NVMTL: surface wait (N41): marks taken again\n"
r = tr.n41_rows(dump(**bad) + A3WS, CUR, "", CD_BACK)
check(any(lv == "WARN" and "backed off 2 time(s)" in t and "errno 19" in t and "taken again 1" in t for lv, _, t in r),
      "back-offs counted with their errno (%s)" % r)
CD_REF = CD_ON + "WindowServer[148:9a2] NVMTL: surface wait (N41): ready mark of surface 9 refused: errno 2 (No such file or directory) (1 so far)\n"
r = tr.n41_rows(dump(**GOOD) + A3WS, CUR, "", CD_REF)
check(any(lv == "WARN" and "ready mark of surface 9 refused" in t for lv, _, t in r), "a refused mark WARNs (%s)" % r)
CD_STATS = CD_ON + ("WindowServer[148:9a2] NVMTL: surface wait (N41): stats (every 4096th): 4096 command buffer(s) marked, "
                    "9000 pending mark(s) taken, 0 refused (0 back-off(s)), 12 surface write(s) in system memory not "
                    "marked, 0 command buffer(s) while backed off, 3 ready mark(s) sent before the surface's dirty mark\n")
r = tr.n41_rows(dump(**GOOD) + A3WS, CUR, "", CD_STATS)
check(any(lv == "WARN" and "3 ready mark(s) before" in t for lv, _, t in r), "early readies WARN (%s)" % r)
check(any(lv == "INFO" and "9000 pending mark(s) taken" in t for lv, _, t in r), "the stats line is printed (%s)" % r)
r = tr.n41_rows(dump(**bad) + A3WS, CUR, "", "")
check(any(lv == "WARN" and "no 'surface wait (N41)' decision line" in t for lv, _, t in r),
      "no decision line at all: WARN (%s)" % r)
# missing counters: a WARN for these sources, INFO for an older kext
r = tr.n41_rows("debug.rdna3accel_a3display: 1\n", CUR)
check(levels(r) == ["WARN"], "these sources without vm_sticky in the dump: WARN (%s)" % r)
r = tr.n41_rows("debug.rdna3accel_a3display: 1\n", OLD)
check(levels(r) == ["INFO"], "an older kext: INFO (%s)" % r)
nowait = dict(GOOD)
del nowait["surface_wait"]
check("WARN" in levels(tr.n41_rows(dump(**nowait), CUR), "surface wait"), "surface_wait missing: WARN")
# silent-failure review 3: a missing key is named
nokey = dict(GOOD)
del nokey["surface_wait_iop_waits"], nokey["vm_release_failed"]
r = tr.n41_rows(dump(**nokey) + A3WS, CUR)
check(any(lv == "WARN" and "surface_wait_iop_waits" in t and "vm_release_failed" in t for lv, n, t in r
          if n == "N41 counters"), "missing keys named (%s)" % r)
check(not [x for x in tr.n41_rows(dump(**nokey) + A3WS, OLD) if x[1] == "N41 counters"], "not for an older kext")
# silent-failure review 3: the parking's own verdicts
bad = dict(GOOD, vm_unparked=0)
check(any(lv == "WARN" and "NONE handed back" in t for lv, _, t in tr.n41_rows(dump(**bad) + A3WS, CUR)),
      "parks never handed back WARN")
bad = dict(GOOD, vm_parked=0, vm_unparked=0)
check(any(lv == "WARN" and "NOTHING parked" in t for lv, _, t in tr.n41_rows(dump(**bad) + A3WS, CUR)),
      "sticky on, deallocs, nothing parked WARN")
bad = dict(GOOD, vm_parked_bytes=300 << 20)
check(any(lv == "WARN" and "300 MiB still parked" in t for lv, _, t in tr.n41_rows(dump(**bad) + A3WS, CUR)),
      "much VRAM parked at the end WARN")
# cpp-reviewer L3: vm_alloc / vm_dealloc do not balance with parking - said as INFO, never a leak verdict
r = tr.n41_rows(dump(**GOOD) + A3WS, CUR)
check(any(lv == "INFO" and "do not balance with parking" in t for lv, _, t in r), "the alloc/dealloc note (%s)" % r)
# silent-failure review 6: a release the framebuffer refused
bad = dict(GOOD, vm_release_failed=2)
check(any(lv == "WARN" and "NOT take back" in t for lv, _, t in tr.n41_rows(dump(**bad) + A3WS, CUR)),
      "release failures WARN")
# silent-failure review 5 / 7: lost readies, cleared waits, IOP flip waits apart, notown
bad = dict(GOOD, surface_wait_pending=1000, surface_wait_ready=800, surface_wait_late=20, surface_wait_evicted=2)
check(any(lv == "WARN" and "178 pending mark(s) without a ready" in t for lv, _, t in tr.n41_rows(dump(**bad) + A3WS, CUR)),
      "lost readies WARN")
bad = dict(GOOD, surface_wait_ready=95, surface_wait_late=5)
check(not any("without a ready" in t for _, _, t in tr.n41_rows(dump(**bad) + A3WS, CUR)), "late readies account")
bad = dict(GOOD, surface_wait_cleared=2, surface_wait_iop_cleared=1)
check(any(lv == "WARN" and "3 wait(s) cleared" in t for lv, _, t in tr.n41_rows(dump(**bad) + A3WS, CUR)),
      "cleared waits WARN")
bad = dict(GOOD, surface_wait_iop_timeouts=4)
check(any(lv == "WARN" and "4 IOP flip(s) gave up" in t for lv, _, t in tr.n41_rows(dump(**bad) + A3WS, CUR)),
      "IOP flip timeouts WARN apart")
bad = dict(GOOD, surface_wait_iop_waits=7)
check(any("IOP flips: 7 wait(s)" in t for _, _, t in tr.n41_rows(dump(**bad) + A3WS, CUR)), "IOP waits printed")
bad = dict(GOOD, surface_wait_notown=3)
check(any(lv == "WARN" and "could not look up" in t for lv, _, t in tr.n41_rows(dump(**bad) + A3WS, CUR)),
      "notown WARN")
# security review of N41: the mark gate (name AND uid 88 / the display client), MOOT refusals, the timeout budget;
# round-2 review: lost readies (ghosts, aged); the parks under pressure / past the cap
r = tr.n41_rows(dump(**dict(GOOD, surface_wait_notreal=5)) + A3WS, CUR)
check(any(lv == "WARN" and "5 mark(s) from a process named WindowServer REFUSED" in t for lv, _, t in r), "notreal WARN")
r = tr.n41_rows(dump(**dict(GOOD, surface_wait_ws_pid_refused=3)) + A3WS, CUR)
check(any(lv == "WARN" and "3 legacy swap(s) / display-pipe transaction(s) of a SECOND process named WindowServer"
          in t and "pid 148 lived" in t for lv, _, t in r), "ws_pid_refused WARN (%s)" % r)
r = tr.n41_rows(dump(**GOOD) + A3WS, CUR)
check(any(lv == "INFO" and "pid 148 uid 88 (_windowserver" in t for lv, _, t in r), "the marking uid INFO (%s)" % r)
r = tr.n41_rows(dump(**dict(GOOD, surface_wait_mark_uid=501)) + A3WS, CUR)
check(any(lv == "WARN" and "NOT uid 88" in t for lv, _, t in r), "a mark taken from another uid WARNs (%s)" % r)
r = tr.n41_rows(dump(**dict(GOOD, surface_wait_starved=1, surface_wait_skipped=40)) + A3WS, CUR)
check(any(lv == "WARN" and "budget ran out 1 time(s)" in t and "40 page-off" in t for lv, _, t in r), "budget WARN")
r = tr.n41_rows(dump(**dict(GOOD, surface_wait_ghosts=2, surface_wait_aged=1)) + A3WS, CUR)
check(any(lv == "INFO" and "2 pending mark(s) dropped for good" in t for lv, _, t in r), "ghosts INFO (%s)" % r)
mootd = dict(GOOD, surface_wait_pending=0, surface_wait_ready=0, surface_wait_moot=7)
CD_MOOT = CD_ON + "".join("WindowServer[148:9a2] NVMTL: surface wait (N41): pending mark of surface %d refused (errno 6, "
                          "Device not configured) - old order, asked again in 1 s (%d back-off(s))\n" % (i, i)
                          for i in (1, 2))
r = tr.n41_rows(dump(**mootd) + "debug.rdna3accel_a3ws: 0\n", CUR, "", CD_MOOT)
check(any(lv == "INFO" and t.startswith("MOOT") and "7 pending mark(s) refused as MOOT" in t for lv, _, t in r),
      "MOOT names its refusals (%s)" % r)
check(any(lv == "INFO" and "backed off 2 time(s)" in t and "the MOOT refusals" in t for lv, _, t in r),
      "MOOT back-offs are INFO (%s)" % r)
r = tr.n41_rows(dump(**dict(mootd, surface_wait_moot=0)) + "debug.rdna3accel_a3ws: 0\n", CUR, "", CD_MOOT)
check(any(lv == "WARN" and "backed off 2 time(s)" in t for lv, _, t in r), "back-offs without MOOT still WARN")
r = tr.n41_rows(dump(**dict(GOOD, vm_park_trim_pressure=2, vm_park_trimmed=9, vm_park_trimmed_bytes=90 << 20,
                            vm_park_trim_held=3, vm_alloc_retry_ok=2)) + A3WS, CUR)
check(any(lv == "WARN" and "2 time(s) VRAM ran out" in t and "9 park(s), 90 MiB" in t and "2 granted" in t
          for lv, _, t in r), "pressure releases WARN (%s)" % r)
r = tr.n41_rows(dump(**dict(GOOD, vm_park_trim_cap=4, vm_park_trimmed=4)) + A3WS, CUR)
check(any(lv == "INFO" and "4 time(s) past the cap (512 MiB)" in t for lv, _, t in r), "cap releases INFO (%s)" % r)
r = tr.n41_rows(dump(**dict(GOOD, vm_park_trim_unknown=3)) + A3WS, CUR)
check(any(lv == "WARN" and "rdna3VramRefs" in t for lv, _, t in r), "unknown counts WARN")
r = tr.n41_rows(dump(**dict(GOOD, vm_park_noimport=11)) + A3WS, CUR)
check(any(lv == "INFO" and "11 deallocPhysical(s) freed, not parked" in t for lv, _, t in r), "noimport INFO")
CD_STATS2 = CD_ON + ("WindowServer[148:9a2] NVMTL: surface wait (N41): stats (every 4096th): 4096 command buffer(s) marked, "
                     "9000 pending mark(s) taken, 0 refused (0 back-off(s)), 12 surface write(s) in system memory not "
                     "marked, 0 command buffer(s) while backed off, 0 ready mark(s) sent before the surface's dirty "
                     "mark, 9000 ready mark(s) sent (17 at the command buffer's end)\n")
r = tr.n41_rows(dump(**GOOD) + A3WS, CUR, "", CD_STATS2)
check(any(lv == "INFO" and "17 of its 9000 ready mark(s) at the command buffer's end" in t for lv, _, t in r),
      "readies at the end are printed (%s)" % r)
check(not any("ready mark(s) before" in t for _, _, t in r), "no early readies: no row")


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
        self.v1 = kw.get("v1", 0)
        self.v2 = kw.get("v2", 0)
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
# silent-failure review 12: the vidmem freed (free(): OBJ_GONE), its address reused by a new vidmem that is
# imported and gets new VRAM - not a move
reuse = Trace([Ev("VMALLOC", obj=VM, pa=0x1000), Ev("SURFGEM", arg=1, obj=VM, pa=0x1000, v0=37),
               Ev("VMFREE", obj=VM, pa=0x1000, flags=rt.VM_PARKED),
               Ev("VMFREE", obj=VM, pa=0x1000, flags=rt.VM_PARK_GONE | rt.VM_OBJ_GONE),
               Ev("VMALLOC", obj=VM, pa=0x5000), Ev("SURFGEM", arg=1, obj=VM, pa=0x5000, v0=38)])
m = rt.vram_moves(reuse)
check(not m["moved"] and m["obj_gone"] == 1 and m["imported"] == 2, "an object freed and its address reused: %s" % m)
check("H-VMMOVE" not in [n for _, n, _ in rt.h_vram_moved(reuse)], "no H-VMMOVE for a reused address")
# the same with sticky 0 (a plain free, then free() with nothing held: OBJ_GONE alone)
reuse0 = Trace([Ev("VMALLOC", obj=VM, pa=0x1000), Ev("SURFGEM", arg=1, obj=VM, pa=0x1000, v0=37),
                Ev("VMFREE", obj=VM, pa=0x1000), Ev("VMFREE", obj=VM, pa=0, flags=rt.VM_OBJ_GONE),
                Ev("VMALLOC", obj=VM, pa=0x5000)])
check(not rt.vram_moves(reuse0)["moved"], "sticky 0 then free(): no move")
# security review of N41: a park released because no BO holds it (VRAM ran out / the cap, v2 NOBO) ends the import:
# the object's next allocation elsewhere is no move
nobo = Trace([Ev("VMALLOC", obj=VM, pa=0x1000), Ev("SURFGEM", arg=1, obj=VM, pa=0x1000, v0=37),
              Ev("VMFREE", obj=VM, pa=0x1000, flags=rt.VM_PARKED),
              Ev("VMFREE", obj=VM, pa=0x1000, flags=rt.VM_PARK_GONE, v2=rt.VM_V2_NOBO),
              Ev("VMALLOC", obj=VM, pa=0x9000)])
m = rt.vram_moves(nobo)
check(not m["moved"] and m["park_nobo"] == 1 and m["park_released"] == 1, "a NOBO release is no move: %s" % m)
check("H-VMMOVE" not in [n for _, n, _ in rt.h_vram_moved(nobo)], "no H-VMMOVE after a NOBO release")
skip = Trace([Ev("PAGEOFF", obj=0x5, flags=rt.PAGEOFF_WAIT_SKIPPED, seq=3)])
check(any(lv == "WARN" and "did NOT wait" in t for lv, n, t in rt.h_vram_moved(skip) if n == "H-SURFWAIT"),
      "a skipped wait WARNs")
check("NOT waited" in rt._fmt_event(skip, Ev("PAGEOFF", arg=1, flags=rt.PAGEOFF_WAIT_SKIPPED), 1.0, "PAGEOFF"),
      "a skipped wait is decoded")
# a grow (a park released, not the object) is still a move
grow = Trace([Ev("VMALLOC", obj=VM, pa=0x1000), Ev("SURFGEM", arg=1, obj=VM, pa=0x1000, v0=37),
              Ev("VMFREE", obj=VM, pa=0x1000, flags=rt.VM_PARKED), Ev("VMFREE", obj=VM, pa=0x1000, flags=rt.VM_PARK_GONE),
              Ev("VMALLOC", obj=VM, pa=0x9000)])
check(len(rt.vram_moves(grow)["moved"]) == 1, "a grow still moves: %s" % rt.vram_moves(grow))
# silent-failure review 3: frees without parking get an H-VMPARK row (WARN for an imported surface's VRAM)
rows = rt.h_vram_moved(moved)
check(any(lv == "WARN" and n == "H-VMPARK" and "freed without parking" in t for lv, n, t in rows),
      "plain frees of an imported surface: H-VMPARK WARN (%s)" % rows)
plain = Trace([Ev("VMALLOC", obj=0x99, pa=0x2000), Ev("VMFREE", obj=0x99, pa=0x2000)])
check([lv for lv, n, _ in rt.h_vram_moved(plain) if n == "H-VMPARK"] == ["INFO"], "plain frees elsewhere: INFO")
# silent-failure review 7: IOP flip waits apart, cleared waits WARN
iop = Trace([Ev("PAGEOFF", arg=2, obj=0, flags=rt.PAGEOFF_IOP | rt.PAGEOFF_WAITED, v3=500),
             Ev("PAGEOFF", obj=0x5, flags=rt.PAGEOFF_WAIT_CLEARED, v3=40000, seq=4)])
rows = rt.h_vram_moved(iop)
check(any(lv == "INFO" and "1 IOP flip(s) waited" in t for lv, _, t in rows), "IOP waits apart (%s)" % rows)
check(not any("page-off(s) waited" in t for _, _, t in rows), "an IOP wait is not a page-off wait (%s)" % rows)
check(any(lv == "WARN" and "1 page-off(s) had their wait cleared" in t for lv, _, t in rows), "cleared WARN")
check("IOP flip wait, IOSurface" in rt._fmt_event(iop, Ev("PAGEOFF", arg=2, flags=rt.PAGEOFF_IOP | 1, v3=5), 1.0,
                                                  "PAGEOFF"), "the IOP wait is decoded")
check("CLEARED" in rt._fmt_event(iop, Ev("PAGEOFF", arg=1, flags=rt.PAGEOFF_WAIT_CLEARED, v3=5), 1.0, "PAGEOFF"),
      "a cleared page-off wait is decoded")
check("(the object freed)" in rt._fmt_event(reuse, Ev("VMFREE", obj=VM, flags=rt.VM_OBJ_GONE), 1.0, "VMFREE"),
      "OBJ_GONE is decoded")
ev = Ev("VMFREE", obj=VM, pa=0x1000, flags=rt.VM_PARKED, v0=8100)
check("PARKED, not freed" in rt._fmt_event(parked, ev, 1.0, "VMFREE"), "VMFREE parked is decoded")
ev = Ev("VMALLOC", obj=VM, pa=0x1000, flags=rt.VM_PARKED, v0=8100)
check("the PARKED one handed back" in rt._fmt_event(parked, ev, 1.0, "VMALLOC"), "VMALLOC reuse is decoded")

# silent-failure review 4: the N41 WARN / FAIL rows (and MOOT) reach the SUMMARY, from a synthetic report archive
import contextlib  # noqa: E402
import io  # noqa: E402
import tarfile  # noqa: E402
import tempfile  # noqa: E402


def archive(dirname, members):
    path = os.path.join(dirname, "rdna3-report-2026-10-23-0000.tar.gz")
    with tarfile.open(path, "w:gz") as tar:
        for name, text in members.items():
            data = text.encode()
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    return path


with tempfile.TemporaryDirectory() as d:
    sv = dump(**dict(GOOD, surface_wait_ready=0, surface_wait_timeouts=3)) + A3WS
    path = archive(d, {"01-boot-args.txt": "boot-args rdna3accel=1 rdna3lvl=12\n", "29-surface-vram.txt": sv,
                       "20-log-rdna3.txt": CUR, "24-log-coredisplay.txt": CD_ON})
    rows = tr.n41_lines(path, "rdna3accel=1")
    check(any(lv == "FAIL" and "NO ready mark" in t for lv, _, t in rows), "n41_lines reads the archive (%s)" % rows)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        tr.summary(path)
    out = buf.getvalue()
    check("N41: surface wait" in out and "FAIL" in out and "gave up waiting" in out,
          "the SUMMARY holds the N41 FAIL / WARN rows:\n%s" % out)
    check("parked, " not in out, "INFO rows stay out of the SUMMARY")
    sv = dump(**dict(GOOD, surface_wait_pending=0, surface_wait_ready=0)) + "debug.rdna3accel_a3ws: 0\n"
    path = archive(d, {"01-boot-args.txt": "boot-args rdna3accel=1\n", "29-surface-vram.txt": sv,
                       "20-log-rdna3.txt": CUR})
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        tr.summary(path)
    check("MOOT: rdna3a3ws=0" in buf.getvalue(), "MOOT reaches the SUMMARY:\n%s" % buf.getvalue())

# silent-failure review 11: collect.sh keeps the N41 boot lines in a budget of their own - its grep block run on a
# kernel log whose boot lines are followed by 300 later 'surface wait' lines
import subprocess  # noqa: E402

col = open(os.path.join(ROOT, "tools", "macos", "collect.sh")).read()
blk = col[col.index("# DRIVER-PLAN N41 with a budget of its own"):]
blk = blk[:blk.index("\n    done")]
with tempfile.TemporaryDirectory() as d:
    f = os.path.join(d, "20-log-rdna3.txt")
    with open(f, "w") as fh:
        fh.write("RDNA3Accelerator: vm sticky: 1 = deallocPhysical parks a surface's VRAM\n")
        fh.write("RDNA3Accelerator: surface wait: 1 = WindowServer's pending GPU writes are marked -- MOOT: rdna3a3ws=0\n")
        for i in range(300):
            fh.write("RDNA3Accelerator: surface wait: page-off of surface %d waited 10 us\n" % i)
    got = subprocess.run(["sh", "-c", blk.replace('"$dir/$f"', '"%s"' % f)], capture_output=True, text=True).stdout
    check("vm sticky: 1 =" in got and "surface wait: 1 =" in got and "MOOT" in got,
          "collect.sh's N41 block keeps the boot lines: %s" % got[:300])
    check(got.count("page-off of surface") == 80 and "surface 299 waited" in got, "and the last 80 others")

if fails:
    print("test_triage_park: FAIL\n  " + "\n  ".join(fails))
    sys.exit(1)
print("test_triage_park: all checks passed")
