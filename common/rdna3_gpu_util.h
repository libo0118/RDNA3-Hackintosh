/*
 * rdna3_gpu_util.h - the accelerator's side of Activity Monitor's GPU
 * numbers (card runs 2026-10-25c..e: "GPU 0 %" for every process). Plain C,
 * no kernel headers: the accelerator uses it, tests/test_gpu_busy.c checks
 * it on the host.
 *
 * What Activity Monitor reads (macOS 26.4.1, read from the binaries; the
 * evidence is in docs/DRIVER-PLAN.md N49):
 *  - Window > GPU History: SafeEjectGPU.framework's -[SafeEjectGPUSession
 *    getPercentUtilization] reads the accelerator's "PerformanceStatistics"
 *    dictionary, key "Device Utilization %" (IORegistryEntryCreateCFProperty
 *    on the GPU's service, no search).
 *  - the per-process "% GPU" / "GPU Time" columns: SafeEjectGPU's
 *    apps_under_service walks the accelerator's subtree (IOService plane,
 *    recursive) and, for every entry that conforms to IOAccelSubmitter2
 *    (IOAccelContext2 and its 2D / GL / CL / video contexts,
 *    IOAccelCommandQueue), parses "IOUserClientCreator" ("pid %lu, %s") and
 *    sums its "accumulatedGPUTime" per pid, the newest "lastSubmittedTime"
 *    telling whether the process is submitting now. IOAccelDevice2 entries
 *    only name the API ("Metal"); IOGPUDeviceUserClient's "AppUsage" is the
 *    IOGPUFamily form. The family creates both numbers in
 *    IOAccelSubmitter2::start and adds to accumulatedGPUTime only from its
 *    block fences (IOAccelBlockFence::notifyClient, OSNumber::addValue of
 *    end - start), which this driver's jobs never use: they go through the
 *    DRM bridge (IOAccelRDNA3Bridge), so the numbers stayed 0.
 *
 * rd3_util_feed: utilisation in percent over the last RD3_UTIL_WIN samples
 * of a busy-time total (the core's ring busy time, rdna3_gpu_busy.h) and the
 * clock it was read at. rd3_ucc_pid: the pid out of IOUserClientCreator.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_GPU_UTIL_H
#define RDNA3_GPU_UTIL_H

#include <stdint.h>
#include <string.h>

#define RD3_UTIL_WIN 4u   /* x 250 ms: a 1 s window, as the family's own sample */

struct rd3_util_win {
    uint64_t busy[RD3_UTIL_WIN], span[RD3_UTIL_WIN];
    uint32_t i, n;
    uint64_t last_busy, last_now;
    uint32_t primed;
};

/* feed one reading; *valid = 0 (and 0 returned) on the first reading and
 * after the totals went back (the core reloaded): the window starts again */
static inline uint32_t rd3_util_feed(struct rd3_util_win *w, uint64_t busy_total, uint64_t now, int *valid)
{
    uint64_t sb = 0, ss = 0;
    uint32_t k;
    *valid = 0;
    if (!w->primed || busy_total < w->last_busy || now <= w->last_now) {
        memset(w, 0, sizeof(*w));
        w->primed = 1;
        w->last_busy = busy_total;
        w->last_now = now;
        return 0;
    }
    w->busy[w->i] = busy_total - w->last_busy;
    w->span[w->i] = now - w->last_now;
    w->i = (w->i + 1) % RD3_UTIL_WIN;
    if (w->n < RD3_UTIL_WIN)
        w->n++;
    w->last_busy = busy_total;
    w->last_now = now;
    for (k = 0; k < w->n; k++) {
        sb += w->busy[k];
        ss += w->span[k];
    }
    *valid = 1;
    if (!ss)
        return 0;
    if (sb >= ss)
        return 100;
    return (uint32_t)((sb * 100 + ss / 2) / ss);
}

/* "pid 814, Chess" -> 814; 0 when the string is not of that form */
static inline int rd3_ucc_pid(const char *s, int32_t *pid)
{
    int64_t v = 0;
    int digits = 0;
    if (!s || strncmp(s, "pid ", 4) != 0)
        return 0;
    s += 4;
    while (*s >= '0' && *s <= '9' && digits < 10) {
        v = v * 10 + (*s - '0');
        s++;
        digits++;
    }
    if (!digits || *s != ',' || v > 0x7fffffff)
        return 0;
    *pid = (int32_t)v;
    return 1;
}

#endif /* RDNA3_GPU_UTIL_H */
