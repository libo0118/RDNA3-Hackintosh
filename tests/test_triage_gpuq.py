"""tools/triage_reports.py's rows for the Metal plugin's IOAccelCommandQueue per process (DRIVER-PLAN N49 open
item 1) on synthetic data:
 gpu_time_queue_rows: the plugin's 'GPU time queue:' lines give an INFO with the processes that opened theirs, a WARN
     per failure step and IOReturn naming the processes, an INFO for NVMTL_GPUTIME_QUEUE=0, nothing for a log without
     them;
 gpu_time_queue_rows against the kernel's 'pid N used the GPU ... has no IOAccelSubmitter2' lines: a pid that opened its
     queue is the start-up gap (INFO), a pid with a plugin device and no opened queue is a WARN by name (with its failure
     step), a pid without a plugin device is an INFO; a close that failed too is a WARN; plugin device lines with no 'GPU
     time queue:' line at all are a WARN (an older plugin);
 gpu_util_verdict: perf_cmdqueues > 0 is an INFO; 0 while some GPU time had no IOAccelSubmitter2 is an INFO (the last
     walk; the processes may have exited); 0 with every process credited, or a kernel without the counter, gives no
     row; perf_creditwait is an INFO up to GPUQ_CREDITWAIT_WARN, a WARN above.
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

# the kernel's no-submitter lines against the plugin's
DEV = ("2026-10-26 10:00:05.1 Df Chess[812:2b] (RDNA3MTLDriver) NVMTL: -[NVMTLDevice initWithAcceleratorPort:36867] in Chess\n"
       "2026-10-26 10:00:06.1 Df Terminal[900:3c] (RDNA3MTLDriver) NVMTL: -[NVMTLDevice initWithAcceleratorPort:36868] in Terminal\n"
       "2026-10-26 10:00:06.2 Df Photos[950:3c] (RDNA3MTLDriver) NVMTL: -[NVMTLDevice initWithAcceleratorPort:36869] in Photos\n")
KLOG = ("RDNA3Accelerator: perfstats: pid 812 used the GPU (1200 us, 3 job(s)) but has no IOAccelSubmitter2 (context\n"
        "RDNA3Accelerator: perfstats: pid 900 used the GPU (5000 us, 9 job(s)) but has no IOAccelSubmitter2 (context\n"
        "RDNA3Accelerator: perfstats: pid 950 used the GPU (7000 us, 9 job(s)) but has no IOAccelSubmitter2 (context\n"
        "RDNA3Accelerator: perfstats: pid 4242 used the GPU (100 us, 1 job(s)) but has no IOAccelSubmitter2 (context\n")
rows = tr.gpu_time_queue_rows(WS, DEV + APPS, KLOG)
check(any(r[0] == "INFO" and "pid(s) 812 used the GPU before their queue opened: that time stays uncredited" in r[2] for r in rows), "gap: %r" % rows)
check(any(r[0] == "WARN" and "pid 900 (Terminal): 5.0 ms" in r[2] and "FAILED at IOConnectAddClient" in r[2] for r in rows),
      "lost with step: %r" % rows)
check(any(r[0] == "WARN" and "pid 950 (Photos): 7.0 ms" in r[2] and "logged no opened queue" in r[2] for r in rows),
      "lost, no line: %r" % rows)
check(any(r[0] == "INFO" and "pid(s) 4242 used the GPU with no submitter and no Metal device" in r[2] for r in rows),
      "other: %r" % rows)
rows = tr.gpu_time_queue_rows("", DEV, "")
check(any(r[0] == "WARN" and "3 process(es) made a Metal device of the plugin but no 'GPU time queue:' line" in r[2]
          for r in rows), "older plugin: %r" % rows)
BAD = ("2026-10-26 10:00:06.3 E Notes[902:3e] NVMTL: GPU time queue: FAILED at IOConnectAddClient(queue, shared): the kernel's "
       "connectClient / start, IOReturn 0xe00002c2 (closing it FAILED too: IOReturn 0xe00002c0) for Notes (pid 902): Activity\n")
rows = tr.gpu_time_queue_rows("", BAD, "")
check(any(r[0] == "WARN" and "Notes (pid 902): the queue that did not start was NOT closed (IOServiceClose 0xe00002c0)" in r[2]
          for r in rows), "bad close: %r" % rows)
check(any(r[0] == "WARN" and "IOReturn 0xe00002c2 (Notes (902))" in r[2] for r in rows), "bad close group: %r" % rows)

# ECC round 2: a FAILED first try and an opened retry is an INFO, not a WARN; a refused device is no Metal device; a
# queue closed again (Vulkan failed) no longer stands
RETRY = ("2026-10-26 10:00:06.1 Df Terminal[900:3c] (RDNA3MTLDriver) NVMTL: -[NVMTLDevice initWithAcceleratorPort:36868] in Terminal\n"
         "2026-10-26 10:00:06.3 E Terminal[900:3c] NVMTL: GPU time queue: FAILED at IOServiceOpen(accelerator, type 8 = "
         "IOAccelCommandQueue), IOReturn 0xe00002be for Terminal (pid 900): Activity Monitor shows 0 % GPU for it; Metal works "
         "as before; one more try at this process's next Metal device\n"
         "2026-10-26 10:00:07.3 Df Terminal[900:3c] NVMTL: GPU time queue: IOAccelCommandQueue opened for Terminal (pid 900; "
         "connection 0x2a03)\n")
rows = tr.gpu_time_queue_rows("", RETRY, KLOG)
check(not any(r[0] == "WARN" for r in rows if "900" in r[2]), "retry WARN: %r" % rows)
check(any(r[0] == "INFO" and "needed the retry" in r[2] and "Terminal (900)" in r[2] for r in rows), "retry INFO: %r" % rows)
check(any(r[0] == "INFO" and "pid(s) 900 used the GPU before their queue opened" in r[2] for r in rows), "retry gap: %r" % rows)
REF = ("2026-10-26 10:00:06.2 Df Photos[950:3c] (RDNA3MTLDriver) NVMTL: -[NVMTLDevice initWithAcceleratorPort:36869] in Photos\n"
       "2026-10-26 10:00:06.2 Df Photos[950:3c] (RDNA3MTLDriver) NVMTL:   not allow-listed: no device for Photos\n"
       "2026-10-26 10:00:08.1 Df Maps[960:3c] (RDNA3MTLDriver) NVMTL: -[NVMTLDevice initWithAcceleratorPort:36870] in Maps\n"
       "2026-10-26 10:00:08.2 Df Maps[960:3c] NVMTL: GPU time queue: IOAccelCommandQueue opened for Maps (pid 960; connection 0x1)\n"
       "2026-10-26 10:00:08.3 Df Maps[960:3c] NVMTL:   NVK did not come up in this process - refusing to be the Metal device for Maps\n"
       "2026-10-26 10:00:08.3 Df Maps[960:3c] NVMTL: GPU time queue: closed again for Maps (pid 960; connection 0x1, IOServiceClose "
       "0x00000000): Vulkan did not come up: no Metal device; the next Metal device of this process opens it again\n")
K2 = KLOG + "RDNA3Accelerator: perfstats: pid 960 used the GPU (300 us, 1 job(s)) but has no IOAccelSubmitter2 (context\n"
rows = tr.gpu_time_queue_rows("", REF, K2)
check(not any(r[0] == "WARN" and "950" in r[2] for r in rows), "refused device WARN: %r" % rows)
check(any(r[0] == "INFO" and "no Metal device" in r[2] and "950" in r[2] and "960" in r[2] for r in rows), "refused INFO: %r" % rows)
check(not any("960 used the GPU before their queue opened" in r[2] for r in rows), "closed queue gap: %r" % rows)
v = tr.gpu_util_verdict({"perf_rings": 10, "perf_jobs": 5, "perf_pendingns": 10 ** 6, "perf_creditwait": 1})
check(not any("no process got GPU time" in t for _, t in v), "pending time is not 'no process': %r" % v)

base = {"perf_rings": 10, "perf_jobs": 5}
v = tr.gpu_util_verdict(dict(base, perf_cmdqueues=3, perf_gpuns=10 ** 9))
check(any(l == "INFO" and "3 IOAccelCommandQueue(s)" in t for l, t in v), "cmdqueues INFO: %r" % v)
v = tr.gpu_util_verdict(dict(base, perf_cmdqueues=0, perf_nosubns=2 * 10 ** 9))
check(any(l == "INFO" and "no IOAccelCommandQueue under the accelerator at the last credit walk" in t for l, t in v),
      "cmdqueues 0 INFO: %r" % v)
check(not any(l == "WARN" and "IOAccelCommandQueue" in t for l, t in v), "cmdqueues 0 no WARN: %r" % v)
v = tr.gpu_util_verdict(dict(base, perf_creditwait=2, perf_pendingns=3 * 10 ** 6))
check(any(l == "INFO" and "2 take(s) found a process's only submitter not started yet (3.0 ms" in t for l, t in v),
      "creditwait INFO: %r" % v)
v = tr.gpu_util_verdict(dict(base, perf_creditwait=tr.GPUQ_CREDITWAIT_WARN + 1))
check(any(l == "WARN" and "stays unstarted" in t for l, t in v), "creditwait WARN: %r" % v)
v = tr.gpu_util_verdict(dict(base, perf_cmdqueues=0, perf_gpuns=10 ** 9))
check(not any("IOAccelCommandQueue" in t for _, t in v), "0 queues, all credited: %r" % v)
v = tr.gpu_util_verdict(dict(base, perf_nosubns=10 ** 9))
check(not any("IOAccelCommandQueue under" in t for _, t in v), "older kernel: %r" % v)

if fails:
    for f in fails:
        print("FAIL: " + f)
    sys.exit(1)
print("test_triage_gpuq: ok")
