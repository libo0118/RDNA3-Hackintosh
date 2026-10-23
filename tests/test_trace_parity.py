"""common/rdna3_trace.h's SWAP constants and tools/rdna3_trace.py's must be
the same values (ECC review round 1 of the stale back, card run
2026-10-17c): every RD3_TRV1_<name> bit is the decoder's V1_<name>, every
rd3_trace_swap value an arg the decoder names (RD3_TRS_HOLD "hold",
RD3_TRS_COPY "copy"), RD3_TRV4_LANDED_A1 its LANDED_A1, and the trace
version its VERSION. Run: python3 -I -B tests/test_trace_parity.py"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import rdna3_trace  # noqa: E402

h = open(os.path.join(ROOT, "common", "rdna3_trace.h")).read()
fails = []
bits = dict((n, 1 << int(b)) for n, b in re.findall(r"^#define RD3_TRV1_(\w+)\s+\(1u << (\d+)\)", h, re.M))
if len(bits) < 4:
    fails.append("only %d RD3_TRV1_* bits parsed from the header" % len(bits))
for name, val in sorted(bits.items()):
    py = getattr(rdna3_trace, "V1_" + name, None)
    if py != val:
        fails.append("RD3_TRV1_%s = 0x%x, the decoder's V1_%s = %s" % (name, val, name,
                                                                      "missing" if py is None else hex(py)))
if len(set(bits.values())) != len(bits):
    fails.append("two RD3_TRV1_* bits share a value: %s" % bits)
m = re.search(r"enum rd3_trace_swap \{(.*?)\};", h, re.S)
trs = dict((n, int(v)) for n, v in re.findall(r"RD3_TRS_(\w+)\s*=\s*(\d+)", m.group(1) if m else ""))
if set(trs.values()) != set(rdna3_trace.SWAP_ARG):
    fails.append("rd3_trace_swap values %s, the decoder's SWAP_ARG keys %s" % (sorted(trs.values()),
                                                                               sorted(rdna3_trace.SWAP_ARG)))
for name, word in (("HOLD", "hold"), ("COPY", "copy"), ("SKIP", "skip"), ("FLIP", "flip")):
    if rdna3_trace.SWAP_ARG.get(trs.get(name)) != word:
        fails.append("RD3_TRS_%s = %s is '%s' in the decoder, not '%s'" % (name, trs.get(name),
                                                                          rdna3_trace.SWAP_ARG.get(trs.get(name)), word))
a1 = re.search(r"#define RD3_TRV4_LANDED_A1 0x([0-9a-f]+)u", h)
if not a1 or int(a1.group(1), 16) != rdna3_trace.LANDED_A1:
    fails.append("RD3_TRV4_LANDED_A1 %s, the decoder's LANDED_A1 0x%x" % (a1 and a1.group(1), rdna3_trace.LANDED_A1))
ver = re.search(r"#define RD3_TRACE_VERSION\s+(\d+)u", h)
if not ver or int(ver.group(1)) != rdna3_trace.VERSION:
    fails.append("RD3_TRACE_VERSION %s, the decoder's VERSION %d" % (ver and ver.group(1), rdna3_trace.VERSION))
# the write generations' reason field (card run 2026-10-17d) and the flip-buffer event's outcomes
sh = re.search(r"#define RD3_TRV1_GEN_WHY_SHIFT (\d+)u", h)
mk = re.search(r"#define RD3_TRV1_GEN_WHY_MASK\s+(\d+)u", h)
if not sh or int(sh.group(1)) != rdna3_trace.V1_GEN_WHY_SHIFT or not mk or int(mk.group(1)) != rdna3_trace.V1_GEN_WHY_MASK:
    fails.append("RD3_TRV1_GEN_WHY_SHIFT / _MASK %s / %s, the decoder's %d / %d" % (
        sh and sh.group(1), mk and mk.group(1), rdna3_trace.V1_GEN_WHY_SHIFT, rdna3_trace.V1_GEN_WHY_MASK))
wg = open(os.path.join(ROOT, "common", "rdna3_write_gen.h")).read()
m2 = re.search(r"enum rd3_wg_why \{(.*?)\};", wg, re.S)
why = dict((int(v), n) for n, v in re.findall(r"RD3_WGW_(\w+)\s*=\s*(\d+)", m2.group(1) if m2 else ""))
if sorted(why) != sorted(rdna3_trace.GEN_WHY) or max(why) > rdna3_trace.V1_GEN_WHY_MASK:
    fails.append("enum rd3_wg_why %s, the decoder's GEN_WHY %s" % (sorted(why), sorted(rdna3_trace.GEN_WHY)))
m3 = re.search(r"enum rd3_trace_flipbuf \{(.*?)\};", h, re.S)
fbw = sorted(int(v) for v in re.findall(r"RD3_TRFB_\w+\s*=\s*(\d+)", m3.group(1) if m3 else ""))
if fbw != sorted(rdna3_trace.FLIPBUF_WHAT):
    fails.append("enum rd3_trace_flipbuf %s, the decoder's FLIPBUF_WHAT %s" % (fbw, sorted(rdna3_trace.FLIPBUF_WHAT)))
# DRIVER-PLAN N41: the VRAM parking's VMALLOC / VMFREE flags and PAGEOFF's wait flags
for cname, pyname in (("RD3_TRVM_PARKED", "VM_PARKED"), ("RD3_TRVM_PARK_GONE", "VM_PARK_GONE"),
                      ("RD3_TRVM_FREE_LIVE", "VM_FREE_LIVE"), ("RD3_TRPO_WAITED", "PAGEOFF_WAITED"),
                      ("RD3_TRPO_TIMEOUT", "PAGEOFF_WAIT_TIMEOUT"), ("RD3_TRVM_OBJ_GONE", "VM_OBJ_GONE"),
                      ("RD3_TRPO_IOP", "PAGEOFF_IOP"), ("RD3_TRPO_CLEARED", "PAGEOFF_WAIT_CLEARED"),
                      ("RD3_TRPO_SKIPPED", "PAGEOFF_WAIT_SKIPPED"), ("RD3_TRVMV2_NOBO", "VM_V2_NOBO")):
    mm = re.search(r"#define %s\s+(\d+)u" % cname, h)
    if not mm or int(mm.group(1)) != getattr(rdna3_trace, pyname, None):
        fails.append("%s %s, the decoder's %s %s" % (cname, mm and mm.group(1), pyname, getattr(rdna3_trace, pyname, None)))
vmf = [int(x) for x in re.findall(r"#define RD3_TRVM_\w+\s+(\d+)u", h)]
if any(f & 0x0f for f in vmf) or len(set(vmf)) != len(vmf):
    fails.append("RD3_TRVM_* flags %s overlap VMFREE's 1..8 or each other" % vmf)
pof = [int(x) for x in re.findall(r"#define RD3_TRPO_\w+\s+(\d+)u", h)]
if len(set(pof)) != len(pof) or any(f & (f - 1) for f in pof) or any(f > 0x80 for f in vmf + pof):
    fails.append("RD3_TRPO_* flags %s not distinct single bits, or a flag beyond the 8-bit field (%s)" % (pof, vmf))
types = dict((int(v), n) for n, v in re.findall(r"RD3_TR_(\w+)\s*=\s*(\d+),", h))
for v, n in sorted((v, n) for v, n in types.items() if v):
    if rdna3_trace.TYPES.get(v) != n:
        fails.append("RD3_TR_%s = %d, the decoder's TYPES[%d] = %s" % (n, v, v, rdna3_trace.TYPES.get(v)))
# the bits the decoder uses that the header documents only in prose (the SDMA breaker, 1 << 20)
if rdna3_trace.V1_BRK_FLIP != 1 << 20 or "1 << 20 when modes 1/2" not in h:
    fails.append("the SDMA breaker's SWAP v1 bit is not 1 << 20 in both")
# SWAP's target 4: mode 0's swap to the buffer already on the panel, not flipped (card run 2026-10-18b)
m4 = re.search(r"#define RD3_TRV1_TARGET_SAME (\d+)u", h)
if not m4 or int(m4.group(1)) != rdna3_trace.TARGET_SAME or rdna3_trace.TARGET_SAME not in rdna3_trace.TARGET:
    fails.append("RD3_TRV1_TARGET_SAME %s, the decoder's TARGET_SAME %d / TARGET %s" %
                 (m4.group(1) if m4 else "missing", rdna3_trace.TARGET_SAME, sorted(rdna3_trace.TARGET)))
# DRIVER-PLAN N38: SWAP's target 5 (queued behind the flip in flight) and the PRESENT event's args
m5 = re.search(r"#define RD3_TRV1_TARGET_QUEUED (\d+)u", h)
if not m5 or int(m5.group(1)) != rdna3_trace.TARGET_QUEUED or rdna3_trace.TARGET_QUEUED not in rdna3_trace.TARGET:
    fails.append("RD3_TRV1_TARGET_QUEUED %s, the decoder's TARGET_QUEUED %d" % (m5.group(1) if m5 else "missing",
                                                                              rdna3_trace.TARGET_QUEUED))
m6 = re.search(r"enum rd3_trace_present \{(.*?)\};", h, re.S)
trp = sorted(int(v) for v in re.findall(r"RD3_TRP_\w+\s*=\s*(\d+)", m6.group(1) if m6 else ""))
if not trp or trp != sorted(rdna3_trace.PRESENT_ARG):
    fails.append("enum rd3_trace_present %s, the decoder's PRESENT_ARG %s" % (trp, sorted(rdna3_trace.PRESENT_ARG)))
# N42: the LATCHED flag of a latch the pflip interrupt ended, above every kind
m8 = re.search(r"#define RD3_TRP_IRQ_DONE 0x([0-9a-fA-F]+)u", h)
if not m8 or int(m8.group(1), 16) != rdna3_trace.PQ_IRQ_DONE or rdna3_trace.PQ_IRQ_DONE & rdna3_trace.PQ_KIND_MASK \
        or max(rdna3_trace.PQ_KIND) > rdna3_trace.PQ_KIND_MASK:
    fails.append("RD3_TRP_IRQ_DONE %s, the decoder's PQ_IRQ_DONE 0x%x / PQ_KIND_MASK 0x%x" % (
        m8.group(1) if m8 else "missing", rdna3_trace.PQ_IRQ_DONE, rdna3_trace.PQ_KIND_MASK))
m7 = re.search(r"enum rd3_trace_pq_kind \{(.*?)\};", h, re.S)
trk = sorted(int(v) for v in re.findall(r"RD3_TRPK_\w+\s*=\s*(\d+)", m7.group(1) if m7 else ""))
if not trk or trk != sorted(rdna3_trace.PQ_KIND):
    fails.append("enum rd3_trace_pq_kind %s, the decoder's PQ_KIND %s" % (trk, sorted(rdna3_trace.PQ_KIND)))
# DRIVER-PLAN N38 (ECC review): every present-queue / perfstats counter and knob the accelerator registers reaches
# the report - the collectors' sysctl grep (tools/macos/collect.sh, tools/vm/macos/autoreport.sh), triage's
# A3_SYSCTL and, per head, its PQ_KEYS - and triage's stuck run is the header's
import triage_reports  # noqa: E402
acc = open(os.path.join(ROOT, "accel", "rdna3-accel.cpp")).read()
pq4 = re.findall(r"^RD3ACCEL_CNT4\(rdna3accel_swaps_pq_(\w+),", acc, re.M)
glob = re.findall(r"^RD3ACCEL_CNT\(rdna3accel_((?:swaps_pq|perf)_\w+),", acc, re.M)
knobs = [k for k in re.findall(r"^SYSCTL_INT\(_debug, OID_AUTO, rdna3accel_(\w+),", acc, re.M)
         if k in ("swap_async", "iop_queue", "iop_wrote", "perfstats", "present_init_failed")]
if len(pq4) < 20 or len(glob) < 5 or len(knobs) != 5:
    fails.append("parsed only %d per-head / %d global present-queue counters and knobs %s from the accelerator" % (
        len(pq4), len(glob), knobs))
greps = []
for coll in ("tools/macos/collect.sh", "tools/vm/macos/autoreport.sh"):
    gm = re.search(r'grep -E "(rdna3accel_\([^"]*\))"', open(os.path.join(ROOT, coll)).read())
    if not gm:
        fails.append("%s: no rdna3accel sysctl grep found" % coll)
    else:
        greps.append((coll, re.compile(gm.group(1))))
names = ["swaps_pq_%s_h0" % k for k in pq4] + glob + knobs
for nm in names:
    line = "debug.rdna3accel_%s: 1" % nm
    if not triage_reports.A3_SYSCTL.search(line):
        fails.append("triage's A3_SYSCTL does not read debug.rdna3accel_%s" % nm)
    for coll, rx in greps:
        if not rx.search(line):
            fails.append("%s's sysctl grep drops debug.rdna3accel_%s" % (coll, nm))
# DRIVER-PLAN N41 (security / round-2 review): every vm_ / surface_ counter and knob the accelerator declares is
# registered (sysctl_register_oid: a declared, unregistered sysctl never reaches the report) and triage names it when
# missing (N41_VM_KEYS / N41_SW_KEYS); vm_alloc_bytes / vm_dealloc_bytes predate N41
n41 = re.findall(r"^(?:RD3ACCEL_CNT\(|SYSCTL_INT\(_debug, OID_AUTO, |SYSCTL_PROC\(_debug, OID_AUTO, )"
                 r"rdna3accel_((?:vm|surface)_\w+),", acc, re.M)
if len(n41) < 50:
    fails.append("parsed only %d N41 counters from the accelerator" % len(n41))
for k in n41:
    if "sysctl__debug_rdna3accel_%s)" % k not in acc:
        fails.append("debug.rdna3accel_%s is declared but never registered (sysctl_register_oid)" % k)
    if k not in triage_reports.N41_VM_KEYS + triage_reports.N41_SW_KEYS and k not in ("vm_alloc_bytes",
                                                                                         "vm_dealloc_bytes"):
        fails.append("debug.rdna3accel_%s is not in triage's N41_VM_KEYS / N41_SW_KEYS" % k)
    for coll, rx in greps:
        if not rx.search("debug.rdna3accel_%s: 1" % k):
            fails.append("%s's sysctl grep drops debug.rdna3accel_%s" % (coll, k))
miss = sorted(set(pq4) - set(triage_reports.PQ_KEYS))
if miss:
    fails.append("triage's PQ_KEYS lacks %s" % ", ".join(miss))
fq = open(os.path.join(ROOT, "common", "rdna3_flip_queue.h")).read()
sr = re.search(r"#define RD3_FQ_STUCK_RUN (\d+)u", fq)
if not sr or int(sr.group(1)) != triage_reports.PQ_STUCK_RUN:
    fails.append("RD3_FQ_STUCK_RUN %s, triage's PQ_STUCK_RUN %d" % (sr and sr.group(1), triage_reports.PQ_STUCK_RUN))
if fails:
    sys.exit("test_trace_parity: FAIL " + "; ".join(fails))
print("test_trace_parity: all checks passed (%d v1 bits, %d swap args)" % (len(bits), len(trs)))
