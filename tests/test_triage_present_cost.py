#!/usr/bin/env python3
"""tools/triage_present_cost.py (DRIVER-PLAN N61) on lines exactly as the plugin and the kernel write them: the plugin's
`vk: present cost: SUMMARY` (metal/plugin/nvmtl_present_cost.h nvmtl_pc_format, the body tests/test_present_cost.c
checks) and the accelerator's debug.rdna3accel_iop_copy_* / iop_gate_us* sysctls (29-surface-vram.txt)."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from triage_present_cost import present_rows, plugin_summaries, kernel_counters  # noqa: E402

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
check(names == ["IOP copy", "present cost"], "rows: %r" % names)
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

if FAILS:
    sys.exit("test_triage_present_cost: %d FAILED" % len(FAILS))
print("test_triage_present_cost: ok")
