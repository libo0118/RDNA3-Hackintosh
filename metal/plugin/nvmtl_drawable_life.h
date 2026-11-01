/*
 * nvmtl_drawable_life.h - where a CAMetalLayer drawable spends its time on the way back to the app (DRIVER-PLAN N73).
 *
 * Bare metal 2026-10-31, round 2 (docs/BAREMETAL-REMOTE.md "Round 2 findings" 3, 6, 11): horizon-client (a wgpu Minecraft
 * client) presents ~230 frames a second where Linux/RADV gives ~2000; its main thread waits 50-60 % in
 * -[CAMetalLayer nextDrawable], the drawables coming back by WindowServer's _XImageQueueCollectable message. A drawable's
 * cycle, with the stages the plugin can see:
 *   app:          nextDrawable (wait) -> encode -> commit ---- pres ----> [drawable present] (NVMTLEventSync.m, after the
 *                 GPU finished the presenting command buffer and its copy-out into the IOSurface, N61/N69)
 *   WindowServer: the IOSurface is wrapped (newTextureWithDescriptor:iosurface:, a new wrapper per frame), uploaded
 *                 (surface-in), composited, flipped; the frame before is released ----> _XImageQueueCollectable
 *   app:          the same IOSurface comes back from nextDrawable: `away` = present -> back
 * With three drawables and WindowServer slower than the app, one frame period is about (encode + pres + WindowServer's
 * latency) / 2, so the app's own present latency counts as much as WindowServer's.
 *
 * Plain C99 (no Objective-C, no Vulkan): tests/test_drawable_life.c checks the counters, the per-surface table, the keep
 * ring's victim choice and the SUMMARY body exactly as NVMTLObjects.m / NVMTLDevice.m use them. The table is not
 * thread-safe: the caller holds its lock; the counters are atomics.
 *
 * The SUMMARY (`vk: drawable: SUMMARY`, tools/triage_present_cost.py row `drawable`), cumulative per process:
 *   next_n next_us_avg next_us_max next_slow (> NVMTL_DL_SLOW_NS) next_nil next_pct (time in nextDrawable, % of the wall
 *   from the first call to the end of the last)   pres_n pres_us_avg pres_us_max (commit -> present call)
 *   away_n away_us_avg away_us_max away_unknown (a drawable back that was never presented here, or the table forgot it)
 *   st_new st_new_us_avg (IOSurface textures made: a private image each) st_alias (wrappers that reused a live image)
 *   st_kept st_evict (the keep ring, NVMTL_SURFTEX_KEEP) st_life_n st_life_us_avg st_life_us_max (wrapper made -> freed;
 *   with the ring on, the wrapper that made an image lives on in the ring: its life includes that time)
 *   keep (the ring's size, 0 off) trace (the nextDrawable hook is installed)
 */
#ifndef NVMTL_DRAWABLE_LIFE_H
#define NVMTL_DRAWABLE_LIFE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define NVMTL_DL_SLOTS    16                 /* surfaces whose present time is remembered (a layer has 2-3, a few layers) */
#define NVMTL_DL_SLOW_NS  1000000ull         /* a nextDrawable longer than 1 ms counts as slow */
#define NVMTL_KEEP_MAX    32                 /* NVMTL_SURFTEX_KEEP is clamped to this */
#define NVMTL_KEEP_AGE_NS 2000000000ull      /* a kept image unused for 2 s is let go (its IOSurface with it) */
#define NVMTL_KEEP_WS_DEFAULT 8              /* WindowServer's default ring size; other processes 0 */

typedef struct { uint32_t id; uint64_t t; } nvmtl_dl_slot;

typedef struct {
    uint64_t next_n, next_ns, next_max_ns, next_slow, next_nil, t_first, t_end;
    uint64_t pres_n, pres_ns, pres_max_ns;
    uint64_t away_n, away_ns, away_max_ns, away_unknown;
    uint64_t st_new, st_new_ns, st_alias, st_kept, st_evict, st_life_n, st_life_ns, st_life_max_ns;
    nvmtl_dl_slot slot[NVMTL_DL_SLOTS];   /* caller-locked */
} nvmtl_dl_stats;

static inline void nvmtl_dl_max(uint64_t *m, uint64_t v)
{
    uint64_t cur = __atomic_load_n(m, __ATOMIC_RELAXED);
    while (v > cur && !__atomic_compare_exchange_n(m, &cur, v, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

/* NVMTL_SURFTEX_KEEP: unset -> the default (WindowServer NVMTL_KEEP_WS_DEFAULT, others 0); a number -> clamped to
 * 0..NVMTL_KEEP_MAX; anything else -> 0 */
static inline unsigned nvmtl_keep_size(const char *env, int is_ws)
{
    if (!env) return is_ws ? NVMTL_KEEP_WS_DEFAULT : 0u;
    unsigned v = 0; const char *p = env;
    if (*p < '0' || *p > '9') return 0;
    for (; *p >= '0' && *p <= '9'; p++) { v = v * 10u + (unsigned)(*p - '0'); if (v > NVMTL_KEEP_MAX) v = NVMTL_KEEP_MAX; }
    return v;
}

/* one nextDrawable call from t0 to t1 (ns, one monotonic clock); got: it returned a drawable */
static inline void nvmtl_dl_next(nvmtl_dl_stats *s, uint64_t t0, uint64_t t1, int got)
{
    const uint64_t d = t1 >= t0 ? t1 - t0 : 0;
    __atomic_fetch_add(&s->next_n, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->next_ns, d, __ATOMIC_RELAXED);
    nvmtl_dl_max(&s->next_max_ns, d);
    if (d > NVMTL_DL_SLOW_NS) __atomic_fetch_add(&s->next_slow, 1, __ATOMIC_RELAXED);
    if (!got) __atomic_fetch_add(&s->next_nil, 1, __ATOMIC_RELAXED);
    uint64_t z = 0;
    (void)__atomic_compare_exchange_n(&s->t_first, &z, t0 ? t0 : 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    nvmtl_dl_max(&s->t_end, t1);
}

/* a command buffer committed at commit_ns presents now_ns */
static inline void nvmtl_dl_pres(nvmtl_dl_stats *s, uint64_t commit_ns, uint64_t now_ns)
{
    if (!commit_ns || now_ns < commit_ns) return;
    const uint64_t d = now_ns - commit_ns;
    __atomic_fetch_add(&s->pres_n, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->pres_ns, d, __ATOMIC_RELAXED);
    nvmtl_dl_max(&s->pres_max_ns, d);
}

/* caller-locked: surface `id` was presented at `now` (replaces its old time, else the oldest slot) */
static inline void nvmtl_dl_present(nvmtl_dl_stats *s, uint32_t id, uint64_t now)
{
    if (!id) return;
    int empty = -1; unsigned oldest = 0;
    for (unsigned i = 0; i < NVMTL_DL_SLOTS; i++) {
        if (s->slot[i].id == id) { s->slot[i].t = now; return; }
        if (!s->slot[i].id) { if (empty < 0) empty = (int)i; continue; }
        if (s->slot[i].t < s->slot[oldest].t || !s->slot[oldest].id) oldest = i;
    }
    const unsigned victim = empty >= 0 ? (unsigned)empty : oldest;
    s->slot[victim].id = id; s->slot[victim].t = now;
}

/* caller-locked: surface `id` came back from nextDrawable at `now`. 1 and *away_ns when it was presented here (the slot is
 * cleared: the next return without a present between counts as unknown), 0 otherwise (counted away_unknown) */
static inline int nvmtl_dl_back(nvmtl_dl_stats *s, uint32_t id, uint64_t now, uint64_t *away_ns)
{
    for (unsigned i = 0; id && i < NVMTL_DL_SLOTS; i++) {
        if (s->slot[i].id != id) continue;
        const uint64_t d = now >= s->slot[i].t ? now - s->slot[i].t : 0;
        s->slot[i].id = 0; s->slot[i].t = 0;
        __atomic_fetch_add(&s->away_n, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s->away_ns, d, __ATOMIC_RELAXED);
        nvmtl_dl_max(&s->away_max_ns, d);
        if (away_ns) *away_ns = d;
        return 1;
    }
    __atomic_fetch_add(&s->away_unknown, 1, __ATOMIC_RELAXED);
    return 0;
}

/* IOSurface textures: a new private image (made in ns), a wrapper that reused a live one, a wrapper's life */
static inline void nvmtl_dl_st_new(nvmtl_dl_stats *s, uint64_t ns)
{
    __atomic_fetch_add(&s->st_new, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->st_new_ns, ns, __ATOMIC_RELAXED);
}
static inline void nvmtl_dl_st_alias(nvmtl_dl_stats *s) { __atomic_fetch_add(&s->st_alias, 1, __ATOMIC_RELAXED); }
static inline void nvmtl_dl_st_life(nvmtl_dl_stats *s, uint64_t born_ns, uint64_t now_ns)
{
    if (!born_ns || now_ns < born_ns) return;
    __atomic_fetch_add(&s->st_life_n, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->st_life_ns, now_ns - born_ns, __ATOMIC_RELAXED);
    nvmtl_dl_max(&s->st_life_max_ns, now_ns - born_ns);
}

/* The keep ring: which entry goes before one more is added (n entries used, touched at t[i]). Any entry unused for over
 * NVMTL_KEEP_AGE_NS first, else the least recently used one when the ring is full; -1: none has to go. cap 0: every
 * entry goes (the ring is off). */
static inline int nvmtl_keep_victim(const uint64_t *t, unsigned n, unsigned cap, uint64_t now)
{
    int oldest = -1;
    for (unsigned i = 0; i < n; i++) {
        if (now > t[i] && now - t[i] > NVMTL_KEEP_AGE_NS) return (int)i;
        if (oldest < 0 || t[i] < t[oldest]) oldest = (int)i;
    }
    return n && n >= cap ? oldest : -1;
}

/* the moved-counter of the SUMMARY */
static inline uint64_t nvmtl_dl_events(const nvmtl_dl_stats *s)
{
    return __atomic_load_n(&s->next_n, __ATOMIC_RELAXED) + __atomic_load_n(&s->pres_n, __ATOMIC_RELAXED)
         + __atomic_load_n(&s->st_new, __ATOMIC_RELAXED) + __atomic_load_n(&s->st_alias, __ATOMIC_RELAXED)
         + __atomic_load_n(&s->st_life_n, __ATOMIC_RELAXED) + __atomic_load_n(&s->st_evict, __ATOMIC_RELAXED);
}

/* the `vk: drawable: SUMMARY` body; 0 when nothing was counted */
static inline int nvmtl_dl_format(const nvmtl_dl_stats *s, unsigned keep, int trace, char *out, size_t cap)
{
#define L_(x) ((unsigned long long)__atomic_load_n(&s->x, __ATOMIC_RELAXED))
#define AVG_(sum, n) (L_(n) ? L_(sum) / L_(n) / 1000u : 0ull)
    if (!cap || !nvmtl_dl_events(s)) return 0;
    const unsigned long long wall = L_(t_end) > L_(t_first) ? L_(t_end) - L_(t_first) : 0;
    const unsigned long long pct10 = wall ? L_(next_ns) * 1000u / wall : 0;   /* tenths of a percent */
    snprintf(out, cap,
             "next_n=%llu next_us_avg=%llu next_us_max=%llu next_slow=%llu next_nil=%llu next_pct=%llu.%llu "
             "pres_n=%llu pres_us_avg=%llu pres_us_max=%llu away_n=%llu away_us_avg=%llu away_us_max=%llu away_unknown=%llu "
             "st_new=%llu st_new_us_avg=%llu st_alias=%llu st_kept=%llu st_evict=%llu st_life_n=%llu st_life_us_avg=%llu "
             "st_life_us_max=%llu keep=%u trace=%d",
             L_(next_n), AVG_(next_ns, next_n), L_(next_max_ns) / 1000u, L_(next_slow), L_(next_nil), pct10 / 10u, pct10 % 10u,
             L_(pres_n), AVG_(pres_ns, pres_n), L_(pres_max_ns) / 1000u, L_(away_n), AVG_(away_ns, away_n), L_(away_max_ns) / 1000u,
             L_(away_unknown), L_(st_new), AVG_(st_new_ns, st_new), L_(st_alias), L_(st_kept), L_(st_evict), L_(st_life_n),
             AVG_(st_life_ns, st_life_n), L_(st_life_max_ns) / 1000u, keep, trace ? 1 : 0);
#undef AVG_
#undef L_
    return 1;
}

#endif
