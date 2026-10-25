"""tools/triage_reports.py's rows for the Metal plugin's IOAccelCommandQueue per process (DRIVER-PLAN N49 open
item 1) on synthetic data:
 gpu_time_queue_rows: the plugin's 'GPU time queue:' lines give an INFO with the processes that opened theirs, a WARN
     per failure step and IOReturn naming the processes, an INFO for NVMTL_GPUTIME_QUEUE=0, nothing for a log without
     them;
 gpu_util_verdict: perf_cmdqueues > 0 is an INFO; 0 while some GPU time had no IOAccelSubmitter2 is a WARN; 0 with
     every process credited, or a kernel without the counter, gives no row.
Run: python3 -I -B tests/test_triage_gpuq.py"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import triage_reports as tr  # noqa: E402

fails = []


def check(cond, what):
    if not cond:
        fails.append(what)


WS = ("2026-10-26 10:00:01.1 Df WindowServer[180:1a] NVMTL: GPU time queue: IOAccelCommandQueue opened for WindowServer "
      "(pid 180; connection 0x1a03, accelerator 0x1503, shared 0x1903): the kernel credits this process's GPU time to it\n")
APPS = ("2026-10-26 10:00:05.2 Df Chess[812:2b] NVMTL: GPU time queue: IOAccelCommandQueue opened for Chess (pid 812; "
        "connection 0x2a03, accelerator 0x2503, shared 0x2903): the kernel credits\n"
        "2026-10-26 10:00:06.3 E Terminal[900:3c] NVMTL: GPU time queue: FAILED at IOConnectAddClient(queue, shared): the "
        "kernel's connectClient / start, IOReturn 0xe00002c2 (the queue was closed again) for Terminal (pid 900): Activity "
        "Monitor shows 0 % GPU for it; Metal works as before\n"
        "2026-10-26 10:00:07.3 E Mail[901:3d] NVMTL: GPU time queue: FAILED at IOConnectAddClient(queue, shared): the "
        "kernel's connectClient / start, IOReturn 0xe00002c2 (the queue was closed again) for Mail (pid 901): Activity\n"
        "2026-10-26 10:00:08.3 E Notes[902:3e] NVMTL: GPU time queue: FAILED at IOServiceOpen(accelerator, type 8 = "
        "IOAccelCommandQueue), IOReturn 0xe00002be for Notes (pid 902): Activity\n"
        "2026-10-26 10:00:09.3 Df Maps[903:3f] NVMTL: GPU time queue: off (NVMTL_GPUTIME_QUEUE=0) for Maps (pid 903): "
        "Activity Monitor shows 0 % GPU for it\n")

rows = tr.gpu_time_queue_rows(WS, APPS)
info = [r for r in rows if r[0] == "INFO"]
warn = [r for r in rows if r[0] == "WARN"]
check(any("2 process(es) opened" in r[2] and "Chess" in r[2] and "WindowServer" in r[2] for r in info), "opened: %r" % rows)
check(len(warn) == 2, "two failure groups: %r" % warn)
check(any("2 process(es)" in r[2] and "IOReturn 0xe00002c2" in r[2] and "Terminal (900)" in r[2] and "Mail (901)" in r[2]
          for r in warn), "AddClient group: %r" % warn)
check(any("IOServiceOpen(accelerator, type 8" in r[2] and "0xe00002be" in r[2] and "Notes (902)" in r[2] for r in warn),
      "IOServiceOpen group: %r" % warn)
check(any("switched off" in r[2] and "1 process(es)" in r[2] for r in info), "off: %r" % rows)
check(tr.gpu_time_queue_rows("NVMTL: something else", "") == [], "no lines, no rows")

base = {"perf_rings": 10, "perf_jobs": 5}
v = tr.gpu_util_verdict(dict(base, perf_cmdqueues=3, perf_gpuns=10 ** 9))
check(any(l == "INFO" and "3 IOAccelCommandQueue(s)" in t for l, t in v), "cmdqueues INFO: %r" % v)
v = tr.gpu_util_verdict(dict(base, perf_cmdqueues=0, perf_nosubns=2 * 10 ** 9))
check(any(l == "WARN" and "no IOAccelCommandQueue under the accelerator although 2.0 s" in t for l, t in v),
      "cmdqueues WARN: %r" % v)
v = tr.gpu_util_verdict(dict(base, perf_cmdqueues=0, perf_gpuns=10 ** 9))
check(not any("IOAccelCommandQueue" in t for _, t in v), "0 queues, all credited: %r" % v)
v = tr.gpu_util_verdict(dict(base, perf_nosubns=10 ** 9))
check(not any("IOAccelCommandQueue under" in t for _, t in v), "older kernel: %r" % v)

if fails:
    for f in fails:
        print("FAIL: " + f)
    sys.exit(1)
print("test_triage_gpuq: ok")
