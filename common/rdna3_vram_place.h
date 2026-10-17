/*
 * rdna3_vram_place.h - where VRAM goes: the CPU-visible heap (the part of
 * VRAM inside BAR0) is shared by the accelerator's display and legacy
 * buffers and by GEM BOs, and it is small (144 MiB with the VM's 256 MiB
 * BAR). Plain C99, no kernel headers: the core kext uses it
 * (kext/src/RDNA3Platform.cpp, RDNA3Drm.cpp) and tests/test_vram_place.c
 * checks the rules on the host.
 *
 * Card run 2026-10-17c: RADV's BOs the CPU may map filled the visible heap
 * ("GEM: 36992 KiB of VRAM the CPU may map did not fit (visible heap 9568
 * KiB free ..."), then every WindowServer framebuffer was refused by the
 * core ("RD3VidMemory::allocPhysical: 8294400 bytes REFUSED by the
 * framebuffer (0xe00002db)") and the main display froze. The rules:
 *
 *  - amdgpu_bo_placement_from_domain / amdgpu_vram_mgr: a BO created with
 *    AMDGPU_GEM_CREATE_NO_CPU_ACCESS is placed in the invisible part of
 *    VRAM only (TTM_PL_FLAG_... lpfn/fpfn above visible_vram_size);
 *    CPU_ACCESS_REQUIRED stays in the visible part. Here nothing migrates a
 *    BO, so a BO without CPU access never takes visible space (it used to
 *    fall back into the visible heap when the invisible part failed) and a
 *    BO the CPU may map never leaves it.
 *  - The accelerator's allocations (RD3K_VRAM_FN_ALLOC: WindowServer's
 *    framebuffers, the swap copies' D buffers, the framebuffer kext's own
 *    surfaces and cursor) are what the display needs to keep running, so a
 *    reserve at the start of the visible heap belongs to them: they
 *    allocate there first and spill into the general visible heap; GEM BOs
 *    never touch the reserve (TTM has the same idea in the other direction:
 *    the kernel's own allocations - firmware, rings - are carved out before
 *    userspace gets any).
 *  - amdgpu_vram_mgr_new: the block alignment never exceeds the size of
 *    the allocation (the buddy allocator's block order is bounded by the
 *    size), so a 4 KiB BO with a large alignment hint does not need a
 *    large aligned hole.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_VRAM_PLACE_H
#define RDNA3_VRAM_PLACE_H

#include <stdint.h>
#include "range_alloc.h"

#define RD3_VP_PAGE 4096ull
#define RD3_VP_ACCEL_ALIGN 65536ull   /* the accelerator's allocations (platformVramAlloc) */
#define RD3_VP_RESERVE_MAX (128ull << 20)

/* pools, in address order: [heap start, +reserve) = ACC, up to the end of
 * BAR0 = GEM, beyond = HI */
enum rdna3_vram_pool { RD3_POOL_ACC = 0, RD3_POOL_GEM, RD3_POOL_HI, RD3_POOL_N };

/* owners of VRAM in the heaps; ACCEL..SWAP_D match RD3K_VRAM_OWNER_*
 * (accel/rdna3k_abi.h, in the low byte of the request's flags) */
enum rdna3_vram_owner {
    RD3_VO_ACCEL = 0,      /* the accelerator (untagged: WindowServer's framebuffers, legacy surfaces) */
    RD3_VO_FB_SURFACE = 1, /* the framebuffer kext's mode surface / a head's own surface */
    RD3_VO_FB_CURSOR = 2,  /* the hardware cursor */
    RD3_VO_SWAP_D = 3,     /* the swap copy's D buffer */
    RD3_VO_GEM = 4,        /* a GEM BO in the visible heap */
    RD3_VO_GEM_HI = 5,     /* a GEM BO beyond BAR0 (no CPU access) */
    RD3_VO_N = 6
};
#define RD3_VO_ACCEL_LAST RD3_VO_SWAP_D

typedef struct rdna3_vram_stat {
    uint64_t bytes, peak_bytes;    /* live, and the most there ever was */
    uint32_t live, peak_live;
    uint32_t allocs, frees;
    uint32_t refused;              /* allocations that found no room */
    uint64_t refused_bytes, refused_max;
} rdna3_vram_stat;

typedef struct rdna3_vram_pools {
    range_alloc *pool[RD3_POOL_N];
    uint64_t acc_end;              /* offsets below: the reserve */
    uint64_t vis_end;              /* offsets below: inside BAR0 (reserve + general) */
    rdna3_vram_stat own[RD3_VO_N];
    /* a GEM BO the CPU may map was refused while the reserve alone could
     * have held it: the reserve doing its job */
    uint32_t gem_reserve_blocked;
    /* a BO that needs no CPU access found no room beyond BAR0 and goes to
     * GTT; it never takes visible space */
    uint32_t hi_full;
    uint32_t refused_total;        /* all owners */
} rdna3_vram_pools;

static inline uint64_t rdna3_vp_pow2_up(uint64_t v)
{
    uint64_t p = 1;
    while (p < v && p < (1ull << 62))
        p <<= 1;
    return p;
}

/* the physical alignment a BO of this size really needs: the hint rounded
 * up to a power of two (amdgpu: roundup_pow_of_two(byte_align)), at least a
 * page, never more than the allocation's own power-of-two size
 * (amdgpu_vram_mgr_new / drm_buddy: min_block_size <= size) */
static inline uint64_t rdna3_vram_place_align(uint64_t size, uint64_t align)
{
    uint64_t a = align < RD3_VP_PAGE ? RD3_VP_PAGE : rdna3_vp_pow2_up(align);
    uint64_t cap = rdna3_vp_pow2_up(size < RD3_VP_PAGE ? RD3_VP_PAGE : size);
    return a > cap ? cap : a;
}

/* how much of the visible heap the accelerator may count on. vis_total =
 * bytes of the visible heap (BAR0 minus the kernel windows); arg_mib < 0:
 * no boot-arg (default: two thirds of the heap, at most RD3_VP_RESERVE_MAX); else
 * rdna3visreserve=<MiB> (0 = no reserve), never more than the heap itself */
static inline uint64_t rdna3_vram_reserve_bytes(uint64_t vis_total, int64_t arg_mib)
{
    uint64_t r;
    if (arg_mib < 0) {
        r = vis_total / 3 * 2;
        if (r > RD3_VP_RESERVE_MAX)
            r = RD3_VP_RESERVE_MAX;
    } else {
        r = (uint64_t)arg_mib << 20;
        if (r > vis_total)
            r = vis_total;
    }
    return r & ~(RD3_VP_ACCEL_ALIGN - 1);
}

/* the three ranges are initialised by the caller's allocator; this lays
 * them out: [start, start + reserve) ACC, [.., vis_end) GEM, [vis_end,
 * heap_end) HI. A start beyond vis_end / heap_end gives empty pools. */
static inline void rdna3_vram_pools_init(rdna3_vram_pools *p, range_alloc *acc, range_alloc *gem,
                                         range_alloc *hi, uint64_t start, uint64_t vis_end,
                                         uint64_t heap_end, uint64_t reserve)
{
    uint64_t i, acc_end;
    unsigned char *z = (unsigned char *)p;
    for (i = 0; i < sizeof(*p); i++)
        z[i] = 0;
    if (vis_end < start)
        vis_end = start;
    if (heap_end < vis_end)
        heap_end = vis_end;
    if (reserve > vis_end - start)
        reserve = vis_end - start;
    acc_end = start + reserve;
    range_alloc_init(acc, start, acc_end - start);
    range_alloc_init(gem, acc_end, vis_end - acc_end);
    range_alloc_init(hi, vis_end, heap_end - vis_end);
    p->pool[RD3_POOL_ACC] = acc;
    p->pool[RD3_POOL_GEM] = gem;
    p->pool[RD3_POOL_HI] = hi;
    p->acc_end = acc_end;
    p->vis_end = vis_end;
}

static inline int rdna3_vram_pool_of(const rdna3_vram_pools *p, uint64_t off)
{
    return off < p->acc_end ? RD3_POOL_ACC : off < p->vis_end ? RD3_POOL_GEM : RD3_POOL_HI;
}

static inline void rdna3_vram_note_alloc(rdna3_vram_pools *p, int owner, uint64_t size)
{
    rdna3_vram_stat *s = &p->own[owner];
    s->bytes += size;
    s->live++;
    s->allocs++;
    if (s->bytes > s->peak_bytes)
        s->peak_bytes = s->bytes;
    if (s->live > s->peak_live)
        s->peak_live = s->live;
}

static inline void rdna3_vram_note_refused(rdna3_vram_pools *p, int owner, uint64_t size)
{
    rdna3_vram_stat *s = &p->own[owner];
    s->refused++;
    s->refused_bytes += size;
    if (size > s->refused_max)
        s->refused_max = size;
    p->refused_total++;
}

/* A GEM BO. no_cpu: AMDGPU_GEM_CREATE_NO_CPU_ACCESS. Returns the offset
 * (and the pool in *pool_out) or RANGE_ALLOC_FAIL; the UAPI then places the
 * BO in GTT. No_cpu BOs use only HI; the others only GEM. */
static inline uint64_t rdna3_vram_alloc_gem(rdna3_vram_pools *p, uint64_t size, uint64_t align,
                                            int no_cpu, int *pool_out)
{
    int pool = no_cpu ? RD3_POOL_HI : RD3_POOL_GEM;
    int owner = no_cpu ? RD3_VO_GEM_HI : RD3_VO_GEM;
    uint64_t off = range_alloc_get(p->pool[pool], size, rdna3_vram_place_align(size, align));
    if (off == RANGE_ALLOC_FAIL) {
        rdna3_vram_note_refused(p, owner, size);
        if (no_cpu)
            p->hi_full++;
        else if (range_alloc_free_bytes(p->pool[RD3_POOL_ACC]) >= size)
            p->gem_reserve_blocked++;
        return RANGE_ALLOC_FAIL;
    }
    rdna3_vram_note_alloc(p, owner, size);
    if (pool_out)
        *pool_out = pool;
    return off;
}

/* An accelerator allocation (owner RD3_VO_ACCEL..RD3_VO_ACCEL_LAST; unknown
 * owners count as the accelerator): the reserve first, then the general
 * visible heap. */
static inline uint64_t rdna3_vram_alloc_accel(rdna3_vram_pools *p, uint64_t size, int owner)
{
    uint64_t off;
    if (owner < 0 || owner > RD3_VO_ACCEL_LAST)
        owner = RD3_VO_ACCEL;
    off = range_alloc_get(p->pool[RD3_POOL_ACC], size, RD3_VP_ACCEL_ALIGN);
    if (off == RANGE_ALLOC_FAIL)
        off = range_alloc_get(p->pool[RD3_POOL_GEM], size, RD3_VP_ACCEL_ALIGN);
    if (off == RANGE_ALLOC_FAIL) {
        rdna3_vram_note_refused(p, owner, size);
        return RANGE_ALLOC_FAIL;
    }
    rdna3_vram_note_alloc(p, owner, size);
    return off;
}

/* back to the pool the offset lies in; owner as when it was allocated.
 * Returns range_alloc_put's result (-1: outside / overlapping / lost). */
static inline int rdna3_vram_free(rdna3_vram_pools *p, uint64_t off, uint64_t size, int owner)
{
    rdna3_vram_stat *s = &p->own[owner];
    if (s->live) {
        s->live--;
        s->frees++;
        s->bytes = s->bytes >= size ? s->bytes - size : 0;
    }
    return range_alloc_put(p->pool[rdna3_vram_pool_of(p, off)], off, size);
}

/* visible heap bytes held by each side, for the report */
static inline uint64_t rdna3_vram_vis_accel_bytes(const rdna3_vram_pools *p)
{
    uint64_t t = 0;
    int o;
    for (o = 0; o <= RD3_VO_ACCEL_LAST; o++)
        t += p->own[o].bytes;
    return t;
}

#endif
