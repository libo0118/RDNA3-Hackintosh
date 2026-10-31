#!/usr/bin/env python3
"""tools/triage_present_cost.py (DRIVER-PLAN N61) on lines exactly as the plugin and the kernel write them: the plugin's
`vk: present cost: SUMMARY` (metal/plugin/nvmtl_present_cost.h nvmtl_pc_format, the body tests/test_present_cost.c
checks) and the accelerator's debug.rdna3accel_iop_copy_* / iop_gate_us* sysctls (29-surface-vram.txt)."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from triage_present_cost import present_rows as _all_rows, plugin_summaries, kernel_counters, ws_frame_rows, ws_summaries  # noqa: E402


N68_ROWS = ("IOP timing", "IOP latch")


def present_rows(*a):
    """the rows before N68 (its own rows are checked at the end)"""
    return [row for row in _all_rows(*a) if row[1] not in N68_ROWS]

FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("FAIL: " + what)


def body(outs, cpu, dma, out_avg, why_first=0, dma_mode=1, import_fail=0):
    return ("outs=%d cpu=%d dma=%d out_mb=23 out_us_avg=%d out_us_max=24000 cpu_us_avg=12750 cpu_us_max=24000 "
            "dma_us_avg=60 dma_us_max=60 lock_us_avg=11050 gpu_us_avg=1000 memcpy_us_avg=700 memcpy_mbps=11849 slow=1 "
            "why_off=0 why_first=%d why_shape=0 why_ro=0 why_import=0 why_moved=0 why_record=0 "
            "imports=1 import_fail=%d ins=2 in_mb=1 in_us_avg=3000 in_us_max=5000 dma_mode=%d" % (outs, cpu, dma, out_avg, why_first, import_fail, dma_mode))


P = "2026-10-29 10:00:%02d.1 Df WindowServer[207:2cd] (RDNA3MTLDriver) NVMTL: vk: present cost: SUMMARY %s\n"
SV = ("debug.rdna3accel_iop_ok: 5756\ndebug.rdna3accel_iop_copy_n: 1000\ndebug.rdna3accel_iop_copy_bytes: 8294400000\n"
      "debug.rdna3accel_iop_copy_us: 4000000\ndebug.rdna3accel_iop_copy_max_us: 9000\ndebug.rdna3accel_iop_copy_64: 1000\n"
      "debug.rdna3accel_iop_gate_us: 500000\ndebug.rdna3accel_iop_copy_n_h0: 600\ndebug.rdna3accel_iop_copy_us_h0: 2400000\n"
      "debug.rdna3accel_iop_gate_us_h0: 300000\ndebug.rdna3accel_iop_copy_n_h1: 400\ndebug.rdna3accel_iop_copy_us_h1: 1600000\n"
      "debug.rdna3accel_iop_gate_us_h1: 200000\n")

# the newest SUMMARY per process wins; the CPU path in WindowServer with the GPU path on is a WARN
text = P % (1, body(3, 3, 0, 9000)) + P % (11, body(30, 1, 29, 400, why_first=1))
s = plugin_summaries(text)
check(list(s) == ["WindowServer[207]"] and s["WindowServer[207]"]["outs"] == "30", "newest SUMMARY per process: %r" % s)
r = present_rows(text, SV, True)
names = [n for _l, n, _t in r]
check(names == ["IOP copy", "present cost", "ws frame"], "rows: %r" % names)
check(r[2][0] == "INFO" and "before DRIVER-PLAN N64" in r[2][2], "no ws frame line from an older plugin: %r" % (r[2],))
check(r[0][0] == "INFO" and "1000 IOP copies (path CPU), avg 4.00 ms; CPU path: 1000 copies, avg 4.00 ms (2073 MB/s, 1000 of them 8 bytes per access), longest 9.00 ms" in r[0][2]
      and "head 0: 600, avg 4.00 ms, gates avg 0.50 ms" in r[0][2] and "SDMA path" not in r[0][2], "IOP copy row: %r" % (r[0],))
check(r[1][0] == "INFO" and r[1][2].startswith("WindowServer[207]: copy-out avg 0.40 ms (path GPU 29 / CPU 1; NVMTL_SURFOUT_DMA on), IOP copy avg 4.00 ms (CPU), surface-in avg 3.00 ms")
      and "GPU path not taken: first 1" in r[1][2], "present cost row (GPU path): %r" % (r[1],))
# the old path only, slow
r = present_rows(P % (2, body(10, 10, 0, 12000, dma_mode=1)), SV, True)
check(r[1][0] == "WARN" and "path CPU" in r[1][2] and "more than 8 ms" in r[1][2] and "used none" in r[1][2], "slow CPU path: %r" % (r[1],))
# A/B: switched off is said, a refused import is a WARN
r = present_rows(P % (3, body(5, 5, 0, 1000, dma_mode=0)), SV, True)
check(r[1][0] == "INFO" and "NVMTL_SURFOUT_DMA OFF" in r[1][2], "A/B off: %r" % (r[1],))
r = present_rows(P % (4, body(5, 4, 1, 1000, import_fail=2)), SV, True)
check(r[1][0] == "WARN" and "2 import(s) refused" in r[1][2], "import refused: %r" % (r[1],))
# a kernel before N61: a WARN that names the missing counters; no IOP: nothing
r = present_rows("", "debug.rdna3accel_iop_ok: 10\n", True)
check(len(r) == 1 and r[0][0] == "WARN" and "iop_copy" in r[0][2], "old kernel: %r" % r)
check(present_rows("", "", False) == [], "nothing at all: no rows")
# a plugin before N61 (hand-off summaries only): an INFO that says so
r = present_rows("x WindowServer[207:2cd] (RDNA3MTLDriver) NVMTL: vk: handoff: SUMMARY cbs=1\n", SV, True)
check(r[-1][1] == "present cost" and "before DRIVER-PLAN N61" in r[-1][2], "old plugin: %r" % r)
check(kernel_counters(SV)["iop_copy_n_h1"] == 400, "kernel counters parsed")

# N63: the SDMA path took most copies (the CPU only a few fallbacks): both paths and the reasons in the row, the
# present cost row's IOP average over both
SD = ("debug.rdna3accel_iop_ok: 5756\ndebug.rdna3accel_iop_copy_n: 10\ndebug.rdna3accel_iop_copy_bytes: 82944000\n"
      "debug.rdna3accel_iop_copy_us: 200000\ndebug.rdna3accel_iop_copy_max_us: 25000\ndebug.rdna3accel_iop_copy_64: 10\n"
      "debug.rdna3accel_iop_gate_us: 99000\ndebug.rdna3accel_iop_copy_n_h0: 1000\ndebug.rdna3accel_iop_copy_us_h0: 1290000\n"
      "debug.rdna3accel_iop_gate_us_h0: 99000\ndebug.rdna3accel_iop_copy_sdma_state: 1\ndebug.rdna3accel_iop_copy_sdma_n: 990\n"
      "debug.rdna3accel_iop_copy_sdma_us: 1089000\ndebug.rdna3accel_iop_copy_sdma_core_us: 990000\n"
      "debug.rdna3accel_iop_copy_sdma_max_us: 3000\ndebug.rdna3accel_iop_copy_sdma_bytes: 8211456000\n"
      "debug.rdna3accel_iop_copy_sdma_binds: 4\ndebug.rdna3accel_iop_copy_sdma_bind_us: 2000\n"
      "debug.rdna3accel_iop_copy_sdma_unbinds: 1\ndebug.rdna3accel_iop_copy_sdma_ranges: 3\n"
      "debug.rdna3accel_iop_copy_sdma_verified: 300\ndebug.rdna3accel_iop_copy_sdma_fb: 10\n"
      "debug.rdna3accel_iop_copy_sdma_fb_core: 10\ndebug.rdna3accel_iop_copy_sdma_fb_pages: 0\n")
r = present_rows(P % (5, body(30, 1, 29, 400)), SD, True)
check(r[0][0] == "INFO" and r[0][2].startswith("1000 IOP copies (path SDMA 990 / CPU 10), avg 1.29 ms; CPU path: 10 copies, avg 20.00 ms")
      and "SDMA path on: 990 copies, avg 1.10 ms (core 1.00 ms), longest 3.00 ms, 7540 MB/s; 4 bind(s) (avg 0.50 ms), 1 unbind(s), "
          "3 range(s) bound; 300 checked by read-back; fallbacks to the CPU 10 (core 10)" in r[0][2], "SDMA row: %r" % (r[0],))
check("IOP copy avg 1.29 ms (SDMA 990 / CPU 10)" in r[1][2], "present cost over both paths: %r" % (r[1],))
# switched off after failures, read-back mismatches: WARN with the reason
r = present_rows("", SD.replace("sdma_state: 1", "sdma_state: 2") + "debug.rdna3accel_iop_copy_sdma_fb_verify: 3\n", True)
check(r[0][0] == "WARN" and "switched OFF after 3 failures" in r[0][2] and "verify 3" in r[0][2] and "switched itself OFF" in r[0][2],
      "SDMA off: %r" % (r[0],))
r = present_rows("", SD + "debug.rdna3accel_iop_copy_sdma_fb_copy: 2\n", True)
check(r[0][0] == "WARN" and "failed or read back wrong" in r[0][2], "SDMA copy failures: %r" % (r[0],))
# on, but every copy on the CPU (e.g. a core without rdna3IopSdma would say state 3)
r = present_rows("", SV + "debug.rdna3accel_iop_copy_sdma_state: 1\ndebug.rdna3accel_iop_copy_sdma_n: 0\n", True)
check(r[0][0] == "WARN" and "every copy went to the CPU" in r[0][2], "SDMA on, unused: %r" % (r[0],))
r = present_rows("", SV + "debug.rdna3accel_iop_copy_sdma_state: 3\ndebug.rdna3accel_iop_copy_sdma_n: 0\n", True)
check(r[0][0] == "INFO" and "core older than N63" in r[0][2], "old core: %r" % (r[0],))

# bare metal 2026-10-31: l10r frames converted by the CPU (SDMA on: every CPU copy a `format` fallback, no WARN for
# "every copy went to the CPU"), and a frame of another format refused (WARN)
L = ("debug.rdna3accel_iop_ok: 70\ndebug.rdna3accel_iop_copy_n: 70\ndebug.rdna3accel_iop_copy_bytes: 580608000\n"
     "debug.rdna3accel_iop_copy_us: 350000\ndebug.rdna3accel_iop_copy_64: 70\ndebug.rdna3accel_iop_gate_us: 0\n"
     "debug.rdna3accel_iop_copy_n_h0: 35\ndebug.rdna3accel_iop_copy_us_h0: 175000\n"
     "debug.rdna3accel_iop_copy_sdma_state: 1\ndebug.rdna3accel_iop_copy_sdma_n: 0\ndebug.rdna3accel_iop_copy_sdma_fb: 70\n"
     "debug.rdna3accel_iop_copy_sdma_fb_format: 70\ndebug.rdna3accel_iop_copy_fmt_l10r: 70\n"
     "debug.rdna3accel_iop_copy_fmt_l10r_seen: 70\ndebug.rdna3accel_iop_copy_fmt_other: 0\n"
     "debug.rdna3accel_iop_copy_fmt_other_last: 0\ndebug.rdna3accel_iop_flip_fmt: 0\n")
r = present_rows("", L, True)
check([n for _l, n, _t in r] == ["IOP copy", "IOP formats"], "l10r rows: %r" % r)
check(r[0][0] == "INFO" and "fallbacks to the CPU 70 (format 70)" in r[0][2], "l10r IOP copy row: %r" % (r[0],))
check(r[1][0] == "INFO" and r[1][2].startswith("70 l10r (ARGB2101010) frame(s) converted into the 8-bit scanout by the "
                                               "CPU of 70 seen")
      and "fallbacks `format` 70" in r[1][2] and "the last: none" in r[1][2] and "N66" in r[1][2], "l10r row: %r" % (r[1],))
r = present_rows("", L.replace("fmt_other: 0", "fmt_other: 35").replace("other_last: 0", "other_last: 2016686640"), True)
check(r[1][0] == "WARN" and "35 frame(s) of another format refused (the last: 0x78343230 ('x420'))" in r[1][2],
      "other format: %r" % (r[1],))
check(all(n != "IOP formats" for _l, n, _t in present_rows("", SV, True)), "BGRA only: no IOP formats row")
# ECC review of 48fe3ad (T7): only another format -> WARN; seen > converted -> WARN; missing companions -> '?' + WARN
r = present_rows("", L.replace("fmt_l10r: 70", "fmt_l10r: 0").replace("l10r_seen: 70", "l10r_seen: 0")
                 .replace("fmt_other: 0", "fmt_other: 4"), True)
check(r[-1][1] == "IOP formats" and r[-1][0] == "WARN" and "4 frame(s) of another format" in r[-1][2], "only other: %r" % (r,))
r = present_rows("", L.replace("l10r_seen: 70", "l10r_seen: 72"), True)
check(r[-1][0] == "WARN" and "2 l10r frame(s) seen but not converted" in r[-1][2], "seen > converted: %r" % (r[-1],))
r = present_rows("", L.replace("debug.rdna3accel_iop_flip_fmt: 0\n", "").replace("debug.rdna3accel_iop_copy_sdma_fb_format: 70\n", ""), True)
check(r[-1][0] == "WARN" and "fallbacks `format` ?" in r[-1][2] and "refused for the format ?" in r[-1][2]
      and "MISSING from the dump: iop_copy_sdma_fb_format, iop_flip_fmt" in r[-1][2], "missing -> ?: %r" % (r[-1],))
# mixed traffic: 70 CPU copies, only 30 of them format fallbacks: the "every copy went to the CPU" WARN stays
r = present_rows("", L.replace("sdma_fb_format: 70", "sdma_fb_format: 30"), True)
check(r[0][0] == "WARN" and "every copy went to the CPU" in r[0][2], "mixed traffic keeps the SDMA WARN: %r" % (r[0],))
# more format fallbacks than copies (counters read one by one): no crash, no SDMA WARN
r = present_rows("", L.replace("sdma_fb_format: 70", "sdma_fb_format: 90"), True)
check(r[0][0] == "INFO", "format > copies: %r" % (r[0],))
# parity: SDMA_FB is the C header's name table, in order
import re as _re  # noqa: E402
_h = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "common", "rdna3_iop_copy.h")).read()
_m = _re.search(r"rd3_iop_sdma_fb_name\(unsigned r\)\s*\{\s*static const char \*const n\[RD3_ISF_N\] = \{([^}]*)\}", _h)
import triage_present_cost  # noqa: E402
check(_m and tuple(_re.findall(r'"([^"]+)"', _m.group(1))) == triage_present_cost.SDMA_FB,
      "SDMA_FB %r vs the header's %r" % (triage_present_cost.SDMA_FB, _m and _m.group(1)))

# N64: `vk: ws frame: SUMMARY` (metal/plugin/nvmtl_ws_frame.h nvmtl_wf_format, the body tests/test_ws_frame.c checks)
W = "2026-10-29 10:00:%02d.1 Df %s (RDNA3MTLDriver) NVMTL: vk: ws frame: SUMMARY %s\n"


def wbody(frames=800, gpu_n=800, rec=4000, gpu=3000, bi=300000, bm=0, membar=0, ups=400, up_kb=409600):
    return ("frames=%d active_s=40 fps_peak=122 passes=46000 draws=340000 fbf_draws=%d bar_img=%d bar_mem=%d rec_us_avg=%d rec_us_max=24000 "
            "gpu_n=%d gpu_us_avg=%d gpu_us_max=9000 wait_us_avg=1500 ups=%d up_kb=%d up_us_avg=2300 membar=%d" % (
                frames, bi + bm, bi, bm, rec, gpu_n, gpu, ups, up_kb, membar))


text = (W % (1, "WindowServer[207:2cd]", wbody(frames=10)) + W % (11, "WindowServer[207:2cd]", wbody())
        + W % (12, "Terminal[968:1a]", wbody(frames=20, gpu_n=20, bi=100, ups=0, up_kb=0)))
check(ws_summaries(text)["WindowServer[207]"]["frames"] == "800", "newest ws frame SUMMARY: %r" % ws_summaries(text))
r = ws_frame_rows(text)
check([x[2].split(":")[0] for x in r] == ["WindowServer[207]", "Terminal[968]"], "WindowServer first: %r" % r)
check(r[0][0] == "INFO" and r[0][2].startswith("WindowServer[207]: ws frame: avg 7.00 ms (encode 4.00, GPU 3.00, fbfetch barriers 375.0/frame, uploads 0.50 MB/frame)")
      and "800 frame(s) (command buffers that drew) in 40 active second(s) = 20.0/s, busiest second 122" in r[0][2]
      and "barriers: image 300000, OLD memory 0 (NVMTL_FBF_MEMBAR off)" in r[0][2] and "400 surface-in upload(s) (0.5 a frame, avg 2.30 ms, 400 MB in all)" in r[0][2],
      "ws frame row: %r" % (r[0],))
# slow GPU, no timestamps, unexpected memory barriers: WARN for WindowServer with the reasons
r = ws_frame_rows(W % (2, "WindowServer[207:2cd]", wbody(gpu=12000, bm=5)))
check(r[0][0] == "WARN" and "the GPU alone takes over 8 ms" in r[0][2] and "5 draw(s) got the OLD memory barrier" in r[0][2], "slow GPU: %r" % (r[0],))
r = ws_frame_rows(W % (3, "WindowServer[207:2cd]", wbody(gpu_n=0, rec=9000)))
check(r[0][0] == "WARN" and "GPU UNKNOWN" in r[0][2] and "avg 9.00 + GPU ? ms" in r[0][2] and "encoding alone" in r[0][2], "no GPU time: %r" % (r[0],))
# the A/B switch: the old barrier everywhere is said, not a WARN
r = ws_frame_rows(W % (4, "WindowServer[207:2cd]", wbody(bi=0, bm=300000, membar=1)))
check(r[0][0] == "INFO" and "NVMTL_FBF_MEMBAR ON (the old barrier)" in r[0][2] and "OLD memory 300000" in r[0][2], "A/B membar: %r" % (r[0],))
check(ws_frame_rows("") == [], "no plugin log: no ws frame row")

# N68: the transactions' timing and the latched copy (iop_latch_rows), from a bare-metal sysctl.txt
r = [x for x in _all_rows("", "debug.rdna3accel_iop_ok: 10\n", True) if x[1] in N68_ROWS]
check(len(r) == 1 and r[0][0] == "WARN" and "timing is UNKNOWN" in r[0][2], "pre-N68 kernel that presented: %r" % r)
check(not [x for x in _all_rows("", "", True) if x[1] in N68_ROWS], "nothing presented: no N68 row")
N68 = ("debug.rdna3accel_iop_ok: 1000\ndebug.rdna3accel_iop_stale_wait_ms: 12\n"
       "debug.rdna3accel_iop_txn_n_h0: 1000\ndebug.rdna3accel_iop_txn_us_h0: 3000000\ndebug.rdna3accel_iop_txn_max_us_h0: 13000\n"
       "debug.rdna3accel_iop_txn_slow_h0: 7\ndebug.rdna3accel_iop_txn_stale_us_h0: 500000\n"
       "debug.rdna3accel_iop_txn_n_h1: 0\ndebug.rdna3accel_iop_txn_overlap: 40\ndebug.rdna3accel_iop_txn_overlap_same: 0\n"
       "debug.rdna3accel_iop_vbl_n_h0: 1000\ndebug.rdna3accel_iop_vbl_same_h0: 120\ndebug.rdna3accel_iop_vbl_skip_h0: 130\n"
       "debug.rdna3accel_iop_vbl_mid_h0: 900\ndebug.rdna3accel_iop_vbl_bin0_h0: 250\ndebug.rdna3accel_iop_vbl_bin1_h0: 250\n"
       "debug.rdna3accel_iop_vbl_bin2_h0: 200\ndebug.rdna3accel_iop_vbl_bin3_h0: 200\ndebug.rdna3accel_iop_vbl_blank_h0: 100\n"
       "debug.rdna3accel_iop_vbl_unread: 0\ndebug.rdna3accel_iop_latch: 0\ndebug.rdna3accel_iop_latch_vram_bytes: 0\n")
r = triage_present_cost.iop_latch_rows(kernel_counters(N68))
t = [x for x in r if x[1] == "IOP timing"]
check(t and t[0][0] == "WARN" and "head 0: 1000 transaction(s), avg 3.00 ms, longest 13.00 ms, 7 over 8 ms" in t[0][2]
      and "stale wait 0.50 ms a transaction (stale_wait_ms 12)" in t[0][2] and "120 in the SAME refresh" in t[0][2]
      and "130 refresh(es) with NO copy" in t[0][2] and "900 direct copies ending mid-scan" in t[0][2]
      and "scan quarters 250/250/200/200 blank 100" in t[0][2] and "uneven steps" in t[0][2], "timing row: %r" % r)
check(any(x[1] == "IOP timing" and "40 transaction(s) entered while another head's" in x[2] for x in r), "overlap row: %r" % r)
lat = [x for x in r if x[1] == "IOP latch"]
check(lat and lat[0][0] == "INFO" and "off (copied into the live scanout)" in lat[0][2], "latch off: %r" % lat)
ON = N68.replace("iop_latch: 0", "iop_latch: 1") + (
    "debug.rdna3accel_iop_latch_n_h0: 0\ndebug.rdna3accel_iop_latch_fb_h0: 1000\ndebug.rdna3accel_iop_latch_fb_vram: 1000\n"
    "debug.rdna3accel_iop_latch_vram_refused: 2\n")
lat = [x for x in triage_present_cost.iop_latch_rows(kernel_counters(ON)) if x[1] == "IOP latch"]
check(lat and lat[0][0] == "WARN" and "fallbacks: vram 1000" in lat[0][2] and "no frame was latched" in lat[0][2]
      and "refused the second buffer" in lat[0][2], "latch on, no VRAM: %r" % lat)
OK = ON.replace("latch_n_h0: 0", "latch_n_h0: 1000").replace("latch_fb_h0: 1000", "latch_fb_h0: 0").replace(
    "fb_vram: 1000", "fb_vram: 0").replace("vram_refused: 2", "vram_refused: 0").replace(
    "vram_bytes: 0", "vram_bytes: 16777216").replace("mid_h0: 900", "mid_h0: 0").replace("same_h0: 120", "same_h0: 0").replace(
    "skip_h0: 130", "skip_h0: 2").replace("slow_h0: 7", "slow_h0: 0") + "debug.rdna3accel_iop_latch_d_h0: 500\n"
r = triage_present_cost.iop_latch_rows(kernel_counters(OK))
check([x[0] for x in r if x[1] == "IOP timing"][0] == "INFO", "latched, even: timing INFO: %r" % r)
lat = [x for x in r if x[1] == "IOP latch"]
check(lat[0][0] == "INFO" and "head 0: 1000 latched (500 into D" in lat[0][2] and "16 MiB held" in lat[0][2], "latched ok: %r" % lat)
r = triage_present_cost.iop_latch_rows(kernel_counters(N68.replace("debug.rdna3accel_iop_latch: 0\n", "")))
check(any(x[0] == "WARN" and "misses it" in x[2] for x in r), "iop_latch missing: %r" % r)

if FAILS:
    sys.exit("test_triage_present_cost: %d FAILED" % len(FAILS))
print("test_triage_present_cost: ok")
