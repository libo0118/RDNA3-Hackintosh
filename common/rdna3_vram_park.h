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

#endif
