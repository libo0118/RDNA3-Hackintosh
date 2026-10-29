/*
 * nvmtl_ws_frame.h - what one frame of a compositor (WindowServer) costs in the plugin, per process (DRIVER-PLAN N64).
 *
 * Plain C99 (no Objective-C, no Vulkan): tests/test_ws_frame.c checks the counters and the SUMMARY body exactly as
 * nvmtl_vk.c uses them.
 *
 * Card run 2026-10-29 12:35 (reports/2026-10-29a): the scanout showed 11.3 fps of a 60 fps page while WindowServer's
 * census counted 314275 framebuffer-fetch draws of 340136 and 6365 surface-in uploads. Nothing in the report said
 * where a WindowServer frame spends its time. A "frame" here is one command buffer that began at least one render
 * pass (WindowServer commits about two per displayed frame); for each one the plugin adds:
 *   rec    CPU time from the command buffer's begin to its submit (the app's encoding plus the plugin's recording;
 *          includes the surface-in uploads done while encoding)
 *   gpu    GPU time between the command buffer's first and last timestamp (TOP_OF_PIPE at begin, BOTTOM_OF_PIPE at
 *          submit); counted only when both timestamps came back (gpu_n says how many)
 *   wait   the completion thread's fence wait for it (queueing behind earlier work included)
 *   passes, draws, framebuffer-fetch draws and the barriers recorded for them, by kind: an image barrier on the
 *          attachment (bar_img, N64's default) or the old global memory barrier (bar_mem, NVMTL_FBF_MEMBAR=1 or
 *          a draw whose attachment had no image to name)
 * Surface-in uploads (IOSurface -> private image) are counted by the caller as they happen (up_*), not per frame.
 * The rate: the frames of each second (CLOCK_MONOTONIC second) are counted; `active_s` is the number of seconds that
 * had a frame, `fps_peak` the most frames one second had.
 */
#ifndef NVMTL_WS_FRAME_H
#define NVMTL_WS_FRAME_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

typedef struct {
    uint64_t frames, passes, draws, fbf_draws, bar_img, bar_mem;
    uint64_t rec_ns, rec_max_ns;
    uint64_t gpu_n, gpu_ns, gpu_max_ns;
    uint64_t wait_ns;
    uint64_t up_n, up_bytes, up_ns;
    uint64_t sec_cur, sec_n, active_s, fps_peak;   /* sec_cur: the second being counted, +1 (0 = none yet) */
} nvmtl_wf_stats;

/* what one command buffer did, filled while it was recorded */
typedef struct {
    uint32_t passes, draws, fbf_draws, bar_img, bar_mem;
} nvmtl_wf_cb;

static inline void nvmtl_wf_max(uint64_t *m, uint64_t v)
{
    uint64_t cur = __atomic_load_n(m, __ATOMIC_RELAXED);
    while (v > cur && !__atomic_compare_exchange_n(m, &cur, v, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

/* the per-second rate; now_ns is CLOCK_MONOTONIC. Two threads that cross a second together may lose one frame of the
 * count of that second: a statistic, never a decision. */
static inline void nvmtl_wf_rate(nvmtl_wf_stats *s, uint64_t now_ns)
{
    const uint64_t sec = now_ns / 1000000000ull + 1u;
    uint64_t cur = __atomic_load_n(&s->sec_cur, __ATOMIC_RELAXED);
    if (cur != sec && __atomic_compare_exchange_n(&s->sec_cur, &cur, sec, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        __atomic_store_n(&s->sec_n, 0, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s->active_s, 1, __ATOMIC_RELAXED);
    }
    nvmtl_wf_max(&s->fps_peak, __atomic_add_fetch(&s->sec_n, 1, __ATOMIC_RELAXED));
}

/* 1 when the command buffer counts as a frame (it began a render pass) */
static inline int nvmtl_wf_is_frame(const nvmtl_wf_cb *cb) { return cb && cb->passes > 0; }

/* one completed command buffer. gpu_ok: gpu_ns was measured (both timestamps). Returns 1 when it was counted. */
static inline int nvmtl_wf_frame(nvmtl_wf_stats *s, const nvmtl_wf_cb *cb, uint64_t rec_ns, int gpu_ok, uint64_t gpu_ns,
                                 uint64_t wait_ns, uint64_t now_ns)
{
    if (!nvmtl_wf_is_frame(cb)) return 0;
    __atomic_fetch_add(&s->frames, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->passes, cb->passes, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->draws, cb->draws, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->fbf_draws, cb->fbf_draws, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->bar_img, cb->bar_img, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->bar_mem, cb->bar_mem, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->rec_ns, rec_ns, __ATOMIC_RELAXED);
    nvmtl_wf_max(&s->rec_max_ns, rec_ns);
    if (gpu_ok) {
        __atomic_fetch_add(&s->gpu_n, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s->gpu_ns, gpu_ns, __ATOMIC_RELAXED);
        nvmtl_wf_max(&s->gpu_max_ns, gpu_ns);
    }
    __atomic_fetch_add(&s->wait_ns, wait_ns, __ATOMIC_RELAXED);
    nvmtl_wf_rate(s, now_ns);
    return 1;
}

static inline void nvmtl_wf_upload(nvmtl_wf_stats *s, uint64_t bytes, uint64_t ns)
{
    __atomic_fetch_add(&s->up_n, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->up_bytes, bytes, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->up_ns, ns, __ATOMIC_RELAXED);
}

/* the moved-counter of the SUMMARY: a line is printed only when it changed */
static inline uint64_t nvmtl_wf_events(const nvmtl_wf_stats *s)
{
    return __atomic_load_n(&s->frames, __ATOMIC_RELAXED) + __atomic_load_n(&s->up_n, __ATOMIC_RELAXED);
}

/* GPU ticks -> ns: (end - start) modulo the timestamp's valid bits, times the period (ns per tick); 0 for a bad period */
static inline uint64_t nvmtl_wf_ticks_ns(uint64_t start, uint64_t end, uint32_t valid_bits, double period_ns)
{
    if (!(period_ns > 0.0) || period_ns > 1e6) return 0;
    const uint64_t mask = valid_bits >= 64 || !valid_bits ? ~0ull : ((1ull << valid_bits) - 1u);
    return (uint64_t)((double)((end - start) & mask) * period_ns);
}

/* the `vk: ws frame: SUMMARY` body (key=value; tools/triage_present_cost.py reads the newest line per process);
 * 0 when nothing was counted. membar: NVMTL_FBF_MEMBAR is set (the old barrier for every framebuffer-fetch draw). */
static inline int nvmtl_wf_format(const nvmtl_wf_stats *s, int membar, char *out, size_t cap)
{
#define L_(x) ((unsigned long long)__atomic_load_n(&s->x, __ATOMIC_RELAXED))
    const unsigned long long fr = L_(frames), gn = L_(gpu_n), un = L_(up_n);
    if ((!fr && !un) || !cap) return 0;
    snprintf(out, cap,
             "frames=%llu active_s=%llu fps_peak=%llu passes=%llu draws=%llu fbf_draws=%llu bar_img=%llu bar_mem=%llu "
             "rec_us_avg=%llu rec_us_max=%llu gpu_n=%llu gpu_us_avg=%llu gpu_us_max=%llu wait_us_avg=%llu "
             "ups=%llu up_kb=%llu up_us_avg=%llu membar=%d",
             fr, L_(active_s), L_(fps_peak), L_(passes), L_(draws), L_(fbf_draws), L_(bar_img), L_(bar_mem),
             fr ? L_(rec_ns) / fr / 1000u : 0, L_(rec_max_ns) / 1000u, gn, gn ? L_(gpu_ns) / gn / 1000u : 0, L_(gpu_max_ns) / 1000u,
             fr ? L_(wait_ns) / fr / 1000u : 0, un, L_(up_bytes) >> 10, un ? L_(up_ns) / un / 1000u : 0, membar ? 1 : 0);
#undef L_
    return 1;
}

#endif
