/*
 * rdna3_vram_park.h - a surface's VRAM kept across the family's
 * deallocPhysical / allocPhysical (DRIVER-PLAN N41). Plain C, no kernel
 * headers: the accelerator (RD3VidMemory) uses it and tests/test_vram_park.c
 * checks it on the host.
 *
 * Evidence-driven, no amdgpu analogue (macOS IOAccelVidMemory semantics).
 * The IOAccel family frees a surface's VRAM (deallocPhysical) after paging
 * it off for a CPU access and backs the surface with new VRAM at the next
 * allocPhysical. The Metal plugin imported the first allocation once
 * (RD_NR_XNU_SURFACE_BO, the core keeps it alive through RD3K_VRAM_FN_REF)
 * and keeps drawing into it, while the family, the dirty mark and every
 * later page-off move to the new allocation nobody writes. Card traces
 * 2026-10-17d / 17c: the VRAM of imported surfaces was freed under live
 * surfaces 64 / 83 times and came back at another address 13 / 19 times:
 * grey or stale windows, windows that only flash, black captures.
 *
 * The NullMoth NVIDIA driver (47ec8d3, 6372823) parks the allocation
 * instead: deallocPhysical keeps it, the next allocPhysical hands it back
 * when it is big enough, a resource that grew releases it and allocates
 * anew, free() releases what is parked and what is still live. This file
 * is that state machine, written for this driver.
 *
 * At most one allocation is live and at most one parked; after any call
 * below never both (rd3_vp_take empties the park, rd3_vp_drop the live
 * one). Each call hands back what the caller must release to the
 * framebuffer, outside of this state.
 *
 * The generation (RD3VidMemory::fAllocGen, the IOP zero-copy flip's
 * marker, DRIVER-PLAN N38) travels with the allocation: a parked and then
 * reused allocation is the same memory with the same contents (nothing
 * else can be handed that range while it is parked), so the plugin's
 * dirty mark of it (RD3Resource::fPluginWroteGen) still describes it. A
 * new allocation after a grow gets a new generation from the caller.
 *
 * Security review of N41 (parked VRAM never given back under pressure):
 * only an allocation a GEM BO was imported over is parked
 * (rd3_vp_should_park: the plugin draws into exactly that memory; the
 * family pages every other surface's contents itself), and the parks are
 * released when VRAM runs out (an allocPhysical refusal) or the parked
 * bytes pass a cap (rd3_vp_pick_release) - only parks no BO holds any
 * more: one a BO still holds frees nothing when released (the BO keeps the
 * range) and would move its surface off the memory the plugin draws into.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_VRAM_PARK_H
#define RDNA3_VRAM_PARK_H

struct rd3_vp_alloc {
    void *handle;                 /* the framebuffer's allocation (0: none) */
    void *kva;
    unsigned long long phys;      /* its BAR address */
    unsigned long long bytes;
    unsigned long long cookie;    /* the VRAM offset */
    unsigned long long gen;       /* RD3VidMemory::fAllocGen */
};

struct rd3_vp {
    struct rd3_vp_alloc live;     /* what the family sees (+0xc8) */
    struct rd3_vp_alloc park;     /* kept across deallocPhysical */
};

/* what allocPhysical does */
enum rd3_vp_take_res {
    RD3_VPT_NEW = 0,      /* nothing parked: allocate */
    RD3_VPT_REUSE = 1,    /* the parked allocation is live again (same memory, same generation) */
    RD3_VPT_GROW = 2      /* the parked one is too small: release it (handed back), then allocate */
};

/* what deallocPhysical did */
enum rd3_vp_drop_res {
    RD3_VPD_NONE = 0,     /* nothing live */
    RD3_VPD_PARKED = 1,   /* the live allocation is parked */
    RD3_VPD_REPARKED = 2, /* parked; an older park was handed back for release (never expected) */
    RD3_VPD_FREED = 3     /* sticky off: the live allocation is handed back for release */
};

static inline int rd3_vp_valid(const struct rd3_vp_alloc *a) { return a && a->handle != 0; }

static inline void rd3_vp_clear(struct rd3_vp_alloc *a)
{
    a->handle = 0; a->kva = 0; a->phys = 0; a->bytes = 0; a->cookie = 0; a->gen = 0;
}

/* allocPhysical, before asking the framebuffer: 'want' bytes are needed.
 * REUSE: v->live is the parked allocation. GROW: *rel is the parked one
 * (release it), v->live stays empty. NEW: nothing changed. A call with a
 * live allocation changes nothing (NEW): the caller refuses that case. */
static inline enum rd3_vp_take_res rd3_vp_take(struct rd3_vp *v, unsigned long long want, struct rd3_vp_alloc *rel)
{
    rd3_vp_clear(rel);
    if (rd3_vp_valid(&v->live) || !rd3_vp_valid(&v->park))
        return RD3_VPT_NEW;
    if (want && v->park.bytes >= want) {
        v->live = v->park;
        rd3_vp_clear(&v->park);
        return RD3_VPT_REUSE;
    }
    *rel = v->park;
    rd3_vp_clear(&v->park);
    return RD3_VPT_GROW;
}

/* deallocPhysical. sticky (debug.rdna3accel_vm_sticky): park the live
 * allocation, else hand it back in *rel. v->live is empty afterwards. */
static inline enum rd3_vp_drop_res rd3_vp_drop(struct rd3_vp *v, int sticky, struct rd3_vp_alloc *rel)
{
    rd3_vp_clear(rel);
    if (!rd3_vp_valid(&v->live))
        return RD3_VPD_NONE;
    if (!sticky) {
        *rel = v->live;
        rd3_vp_clear(&v->live);
        return RD3_VPD_FREED;
    }
    const int had = rd3_vp_valid(&v->park);
    if (had)
        *rel = v->park;
    v->park = v->live;
    rd3_vp_clear(&v->live);
    return had ? RD3_VPD_REPARKED : RD3_VPD_PARKED;
}

/* free(): both go. rel[0] the live allocation (the family freed the object
 * without deallocPhysical), rel[1] the parked one. Returns a mask: 1 live,
 * 2 parked. */
static inline unsigned rd3_vp_free(struct rd3_vp *v, struct rd3_vp_alloc rel[2])
{
    unsigned m = 0;
    rd3_vp_clear(&rel[0]);
    rd3_vp_clear(&rel[1]);
    if (rd3_vp_valid(&v->live)) { rel[0] = v->live; m |= 1u; }
    if (rd3_vp_valid(&v->park)) { rel[1] = v->park; m |= 2u; }
    rd3_vp_clear(&v->live);
    rd3_vp_clear(&v->park);
    return m;
}

/* the dirty mark of a surface whose VRAM is parked: making the parked
 * allocation live again (the family's wire -> allocPhysical -> REUSE) keeps
 * the memory the plugin drew into. Only when it fits; never a new
 * allocation for a dirty mark (it would hold nothing the plugin wrote). */
static inline int rd3_vp_can_restore(const struct rd3_vp *v, unsigned long long want)
{
    return !rd3_vp_valid(&v->live) && rd3_vp_valid(&v->park) && want && v->park.bytes >= want &&
           v->park.phys && v->park.kva;
}

/* the restore made exactly the parked allocation live again */
static inline int rd3_vp_same(const struct rd3_vp_alloc *a, const struct rd3_vp_alloc *b)
{
    return a->handle == b->handle && a->kva == b->kva && a->phys == b->phys && a->bytes == b->bytes &&
           a->cookie == b->cookie;
}

/* RD3Resource::getApertureMemoryDescriptor's cached descriptor over
 * [ap_phys, ap_phys + ap_bytes): kept only while it covers exactly what the
 * vidmem holds - the live allocation, or with nothing live the parked one
 * (still allocated). A re-backed vidmem (another address or size), or one
 * whose VRAM was released (nothing live, nothing parked there), gets no
 * descriptor over freed VRAM: built again or none (ECC review of N41, L1). */
static inline int rd3_vp_apdesc_keep(unsigned long long live_phys, unsigned long long live_bytes,
                                     const struct rd3_vp_alloc *park, unsigned long long ap_phys,
                                     unsigned long long ap_bytes)
{
    if (!ap_phys || !ap_bytes) return 0;
    if (live_phys) return live_phys == ap_phys && live_bytes == ap_bytes;
    return rd3_vp_valid(park) && park->phys == ap_phys && park->bytes == ap_bytes;
}

/* deallocPhysical: park the live allocation only when sticky (debug.rdna3accel_vm_sticky), not a
 * scanout, and a GEM BO was imported over exactly this allocation (bo_gen, RD3VidMemory::fBoGen, is
 * its generation live_gen); else it is freed as before N41 */
static inline int rd3_vp_should_park(int sticky, int scanout, unsigned long long live_gen, unsigned long long bo_gen)
{
    return sticky && !scanout && live_gen && bo_gen == live_gen;
}

/* a parked allocation, for rd3_vp_pick_release */
struct rd3_vp_cand {
    unsigned long long since;   /* when it was parked: older first */
    unsigned long long bytes;
    unsigned refs;              /* the core's references to it: 1 the accelerator's own only (no BO), more a BO
                                 * holds it, 0 unknown (an older core kext): never released */
    void *obj;                  /* the caller's: the parked object and its allocation's handle (keys only, */
    void *handle;               /* never dereferenced here) */
};

/* the release pages through the parks (ECC review round 3 of N41: it looked only at the oldest page, so
 * parks no BO holds behind a page of held ones were never found): once a page's references are known, the
 * parks a BO holds (refs > 1) or whose count is unknown (0) - never released - leave the page, so the next
 * page fills their room. Compacts c[0..n) in place to the refs == 1 ones, in their order; returns how many */
static inline unsigned rd3_vp_keep_releasable(struct rd3_vp_cand *c, unsigned n)
{
    unsigned k = 0;
    for (unsigned i = 0; i < n; i++)
        if (c[i].refs == 1) c[k++] = c[i];
    return k;
}

/* where the next page of the release starts (DRIVER-PLAN N41 open item 3: the pages skipped the parks earlier
 * pages listed by COUNT, so a park unlinked between two pages - reused, released - shifted the list and the
 * first park not yet looked at was skipped until the next release). The parks are listed oldest first: a park
 * is linked at the tail with the time it was parked (a new park of the same object relinks it, with a new
 * time), so the times along the list never go down. The cursor is the last listed park's time and how many
 * listed parks carried exactly that time: a page skips the parks older than it and the first 'same' of that
 * time, wherever earlier parks left the list. A park relinked meanwhile comes again with its new time (the
 * release takes it once: it checks the handle and the time under the lock). */
struct rd3_vp_cursor {
    unsigned long long since;     /* the time of the last park listed */
    unsigned same;                /* parks listed with exactly that time */
    int valid;                    /* 0: the first page (nothing skipped) */
};

/* 1: the park parked at 'since' was listed by an earlier page. '*ties' counts, along one walk of the list, the
 * parks parked at the cursor's time (0 before the walk) */
static inline int rd3_vp_cursor_seen(const struct rd3_vp_cursor *c, unsigned long long since, unsigned *ties)
{
    if (!c->valid || since > c->since) return 0;
    if (since < c->since) return 1;
    return (*ties)++ < c->same;
}

/* a park parked at 'since' listed by this page (in list order) */
static inline void rd3_vp_cursor_note(struct rd3_vp_cursor *c, unsigned long long since)
{
    if (c->valid && since == c->since) { c->same++; return; }
    c->valid = 1;
    c->since = since;
    c->same = 1;
}

#define RD3_VP_CAP_MB_DEFAULT 512u   /* debug.rdna3accel_vm_park_cap_mb: parked bytes kept at most (0: no cap) */

/* which parks go: under pressure (an allocation was refused) every one no BO holds; else, while the
 * parked bytes exceed 'cap' (0: no cap), the oldest no BO holds. out[i] 1: release c[i]. Returns the
 * bytes released; parks a BO holds (or whose references are unknown) are never picked */
static inline unsigned long long rd3_vp_pick_release(const struct rd3_vp_cand *c, unsigned n,
                                                     unsigned long long parked, unsigned long long cap, int pressure,
                                                     unsigned char *out)
{
    unsigned long long freed = 0;
    for (unsigned i = 0; i < n; i++) out[i] = 0;
    if (pressure) {
        for (unsigned i = 0; i < n; i++)
            if (c[i].refs == 1) { out[i] = 1; freed += c[i].bytes; }
        return freed;
    }
    while (cap && parked > cap) {
        unsigned best = n;
        for (unsigned i = 0; i < n; i++)
            if (!out[i] && c[i].refs == 1 && (best == n || c[i].since < c[best].since)) best = i;
        if (best == n) break;
        out[best] = 1;
        freed += c[best].bytes;
        parked = parked > c[best].bytes ? parked - c[best].bytes : 0;
    }
    return freed;
}

#endif
