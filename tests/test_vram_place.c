/* common/rdna3_vram_place.h on the host: where GEM BOs and the accelerator's
 * allocations may go in VRAM (card run 2026-10-17c: the CPU-visible heap
 * filled with GEM BOs and WindowServer's framebuffers were refused).
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rdna3_vram_place.h"
#include "rdna3_drm.h"
#include "../accel/rdna3k_abi.h"

#define MiB (1ull << 20)
#define KiB (1ull << 10)
static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static range_alloc acc, gem, hi;
static rdna3_vram_pools P;

/* the card run's layout: heap from 112 MiB (144 MiB visible), BAR0 256 MiB, VRAM 16368 MiB with the
 * top 256 MiB the kernel's */
#define START (112 * MiB)
#define VIS_END (256 * MiB)
#define HEAP_END (16368 * MiB - 256 * MiB)

static void setup(int64_t arg)
{
    uint64_t res = rdna3_vram_reserve_bytes(VIS_END - START, arg);
    rdna3_vram_pools_init(&P, &acc, &gem, &hi, START, VIS_END, HEAP_END, res);
}

static uint64_t vis_free(void) { return range_alloc_free_bytes(&acc) + range_alloc_free_bytes(&gem); }

static void test_reserve_size(void)
{
    CHECK(rdna3_vram_reserve_bytes(144 * MiB, -1) == 96 * MiB);          /* the card run's heap */
    CHECK(rdna3_vram_reserve_bytes(1900 * MiB, -1) == 128 * MiB);        /* a 2 GiB BAR: capped */
    CHECK(rdna3_vram_reserve_bytes(144 * MiB, 0) == 0);                  /* rdna3visreserve=0 */
    CHECK(rdna3_vram_reserve_bytes(144 * MiB, 32) == 32 * MiB);
    CHECK(rdna3_vram_reserve_bytes(144 * MiB, 4096) == 144 * MiB);       /* never more than the heap */
    CHECK(rdna3_vram_reserve_bytes(0, -1) == 0);
    CHECK(rdna3_vram_reserve_bytes(90 * KiB, -1) == 0);                 /* below one 64 KiB step: rounds down */
    /* the owners the framebuffer kext sends are the core's owners */
    CHECK(RD3K_VRAM_OWNER_ACCEL == RD3_VO_ACCEL && RD3K_VRAM_OWNER_FB_SURFACE == RD3_VO_FB_SURFACE &&
          RD3K_VRAM_OWNER_FB_CURSOR == RD3_VO_FB_CURSOR && RD3K_VRAM_OWNER_SWAP_D == RD3_VO_SWAP_D);
}

static void test_layout(void)
{
    setup(-1);
    CHECK(acc.base == START && acc.end == START + 96 * MiB);
    CHECK(gem.base == acc.end && gem.end == VIS_END);
    CHECK(hi.base == VIS_END && hi.end == HEAP_END);
    CHECK(rdna3_vram_pool_of(&P, START) == RD3_POOL_ACC);
    CHECK(rdna3_vram_pool_of(&P, acc.end - 1) == RD3_POOL_ACC);
    CHECK(rdna3_vram_pool_of(&P, acc.end) == RD3_POOL_GEM);
    CHECK(rdna3_vram_pool_of(&P, VIS_END - 1) == RD3_POOL_GEM);
    CHECK(rdna3_vram_pool_of(&P, VIS_END) == RD3_POOL_HI);
    /* a BAR smaller than the kernel's windows: empty pools, nothing placed */
    rdna3_vram_pools_init(&P, &acc, &gem, &hi, START, 100 * MiB, 64 * MiB, 10 * MiB);
    CHECK(range_alloc_free_bytes(&acc) == 0 && range_alloc_free_bytes(&gem) == 0 && range_alloc_free_bytes(&hi) == 0);
    CHECK(rdna3_vram_alloc_accel(&P, 4096, 0) == RANGE_ALLOC_FAIL);
}

static void test_align(void)
{
    CHECK(rdna3_vram_place_align(4096, 0) == 4096);
    CHECK(rdna3_vram_place_align(4096, 1) == 4096);
    CHECK(rdna3_vram_place_align(4096, 2 * MiB) == 4096);        /* a hint larger than the BO: its own size */
    CHECK(rdna3_vram_place_align(4 * KiB + 1, 1 * MiB) == 8192); /* rounded to a power of two */
    CHECK(rdna3_vram_place_align(36992 * KiB, 2 * MiB) == 2 * MiB);
    CHECK(rdna3_vram_place_align(36992 * KiB, 3 * MiB) == 4 * MiB);   /* amdgpu: roundup_pow_of_two */
    CHECK(rdna3_vram_place_align(64 * KiB, 64 * KiB) == 64 * KiB);
    CHECK(rdna3_vram_place_align(1 * MiB, 256 * MiB) == 1 * MiB);
}

/* a BO without CPU access never takes visible space, whatever else is full */
static void test_no_cpu_stays_out(void)
{
    uint64_t off, vis0;
    int pool = -1;
    setup(-1);
    vis0 = vis_free();
    off = rdna3_vram_alloc_gem(&P, 4096, 4096, 1, 0, &pool);
    CHECK(off != RANGE_ALLOC_FAIL && pool == RD3_POOL_HI && off >= VIS_END);
    /* beyond BAR0 full: refused (the UAPI places it in GTT), the visible heap untouched */
    for (;;) {
        uint64_t big = range_alloc_largest_free(&hi);
        if (!big)
            break;
        off = rdna3_vram_alloc_gem(&P, big, 4096, 1, 0, &pool);
        CHECK(off != RANGE_ALLOC_FAIL);
    }
    CHECK(range_alloc_free_bytes(&hi) == 0);
    CHECK(vis_free() == vis0);
    off = rdna3_vram_alloc_gem(&P, 4096, 4096, 1, 0, &pool);
    CHECK(off == RANGE_ALLOC_FAIL);
    CHECK(vis_free() == vis0);                      /* the old code took 4 KiB of the visible heap here */
    CHECK(P.own[RD3_VO_GEM_HI].refused == 1 && P.hi_full == 1 && P.refused_total == 1);
    CHECK(P.own[RD3_VO_GEM].refused == 0);
}

/* the card run: 36992 KiB and 4096 KiB BOs the CPU may map fill the general part;
 * then the display's 8 MB buffers still fit */
static void test_reserve_for_the_accelerator(void)
{
    uint64_t off, held[16];
    unsigned n = 0, i;
    int pool;
    setup(-1);
    /* GEM BOs the CPU may map until the visible part says no */
    for (;;) {
        off = rdna3_vram_alloc_gem(&P, 36992 * KiB, 2 * MiB, 0, 0, &pool);
        if (off == RANGE_ALLOC_FAIL)
            break;
        CHECK(pool == RD3_POOL_GEM && off >= acc.end && off + 36992 * KiB <= VIS_END);
        n++;
    }
    CHECK(n >= 1);
    while (rdna3_vram_alloc_gem(&P, 4 * KiB, 4 * KiB, 0, 0, &pool) != RANGE_ALLOC_FAIL)
        ;
    /* the reserve is whole and the GEM refusals saw it */
    CHECK(range_alloc_free_bytes(&acc) == 96 * MiB);
    CHECK(P.gem_reserve_blocked >= 1);
    CHECK(P.own[RD3_VO_GEM].refused >= 2);
    /* WindowServer's 8 MB framebuffers (1920x1080x4 = 8294400 B, page rounded) all fit */
    for (i = 0; i < 8; i++) {
        held[i] = rdna3_vram_alloc_accel(&P, (8294400 + 4095) & ~4095ull, RD3_VO_ACCEL);
        CHECK(held[i] != RANGE_ALLOC_FAIL && held[i] < acc.end && held[i] % (64 * KiB) == 0);
    }
    CHECK(P.own[RD3_VO_ACCEL].live == 8 && P.own[RD3_VO_ACCEL].refused == 0);
    /* a tagged cursor and a D buffer count under their own owners */
    CHECK(rdna3_vram_alloc_accel(&P, 16384, RD3_VO_FB_CURSOR) != RANGE_ALLOC_FAIL);
    CHECK(rdna3_vram_alloc_accel(&P, 8294400, RD3_VO_SWAP_D) != RANGE_ALLOC_FAIL);
    CHECK(P.own[RD3_VO_FB_CURSOR].live == 1 && P.own[RD3_VO_SWAP_D].live == 1);
    /* an unknown owner counts as the accelerator */
    CHECK(rdna3_vram_alloc_accel(&P, 4096, 77) != RANGE_ALLOC_FAIL && P.own[RD3_VO_ACCEL].live == 9);
    /* freeing goes back to the pool the offset lies in */
    for (i = 0; i < 8; i++)
        CHECK(rdna3_vram_free(&P, held[i], (8294400 + 4095) & ~4095ull, RD3_VO_ACCEL) == 0);
    CHECK(P.own[RD3_VO_ACCEL].live == 1 && P.own[RD3_VO_ACCEL].frees == 8);
    CHECK(P.own[RD3_VO_ACCEL].peak_live == 9);
}

/* the accelerator takes the reserve first, then spills into the general part; when both
 * are full it is refused and counted (the card run's 0xe00002db) */
static void test_accel_spill_and_refusal(void)
{
    uint64_t off, sz = 8 * MiB;
    unsigned n = 0;
    setup(-1);
    for (;;) {
        off = rdna3_vram_alloc_accel(&P, sz, RD3_VO_ACCEL);
        if (off == RANGE_ALLOC_FAIL)
            break;
        if (n < 12)
            CHECK(off < acc.end);                 /* 96 MiB reserve = 12 x 8 MiB first */
        else
            CHECK(off >= acc.end);                /* then the general visible heap */
        n++;
    }
    CHECK(n == 18);                               /* all 144 MiB */
    CHECK(vis_free() == 0);
    CHECK(P.own[RD3_VO_ACCEL].refused == 1 && P.own[RD3_VO_ACCEL].refused_bytes == sz &&
          P.own[RD3_VO_ACCEL].refused_max == sz && P.refused_total == 1);
    /* GEM BOs still go beyond BAR0 only if they need no CPU access */
    {
        int pool;
        CHECK(rdna3_vram_alloc_gem(&P, 4096, 4096, 0, 0, &pool) == RANGE_ALLOC_FAIL);
        CHECK(rdna3_vram_alloc_gem(&P, 4096, 4096, 1, 0, &pool) != RANGE_ALLOC_FAIL && pool == RD3_POOL_HI);
    }
}

/* rdna3visreserve=0: no reserve; the accelerator and GEM share the whole heap (the old behaviour,
 * minus the no-CPU fallback) */
static void test_no_reserve(void)
{
    int pool;
    uint64_t off;
    setup(0);
    CHECK(range_alloc_free_bytes(&acc) == 0);
    off = rdna3_vram_alloc_gem(&P, 36992 * KiB, 4096, 0, 0, &pool);
    CHECK(off != RANGE_ALLOC_FAIL && off >= START && pool == RD3_POOL_GEM);
    off = rdna3_vram_alloc_accel(&P, 8 * MiB, RD3_VO_ACCEL);
    CHECK(off != RANGE_ALLOC_FAIL && off >= START);
    CHECK(P.gem_reserve_blocked == 0);
}

/* random mix: nothing overlaps, every byte is free, live, or counted lost */
static void test_soak(void)
{
    struct { uint64_t off, size; int owner; } live[600];
    unsigned nlive = 0, it;
    uint64_t total[RD3_POOL_N], held = 0;
    int p;
    setup(-1);
    for (p = 0; p < RD3_POOL_N; p++)
        total[p] = range_alloc_free_bytes(P.pool[p]);
    srand(11);
    for (it = 0; it < 100000; it++) {
        if (nlive < 600 && (rand() % 3)) {
            uint64_t size = (uint64_t)(1 + rand() % 2000) * 4096;
            uint64_t off;
            int owner, pool;
            switch (rand() % 3) {
            case 0:  off = rdna3_vram_alloc_gem(&P, size, 4096ull << (rand() % 9), 0, 0, &pool); owner = RD3_VO_GEM; break;
            case 1:  off = rdna3_vram_alloc_gem(&P, size, 4096ull << (rand() % 9), 1, 0, &pool); owner = RD3_VO_GEM_HI; break;
            default: owner = rand() % 4; off = rdna3_vram_alloc_accel(&P, size, owner); break;
            }
            if (off == RANGE_ALLOC_FAIL)
                continue;
            live[nlive].off = off; live[nlive].size = size; live[nlive].owner = owner;
            nlive++;
            held += size;
        } else if (nlive) {
            unsigned k = (unsigned)rand() % nlive;
            /* the owner for the stats is the one the allocation was made with; the pool follows the offset */
            CHECK(rdna3_vram_free(&P, live[k].off, live[k].size, live[k].owner) == 0);
            held -= live[k].size;
            live[k] = live[--nlive];
        }
        if ((it & 4095) == 0) {
            uint64_t freeb = 0, lost = 0;
            for (p = 0; p < RD3_POOL_N; p++) {
                freeb += range_alloc_free_bytes(P.pool[p]);
                lost += P.pool[p]->lost_bytes;
            }
            CHECK(freeb + held + lost == total[0] + total[1] + total[2]);
        }
    }
    while (nlive) {
        nlive--;
        CHECK(rdna3_vram_free(&P, live[nlive].off, live[nlive].size, live[nlive].owner) == 0);
    }
    for (p = 0; p < RD3_POOL_N; p++)
        CHECK(range_alloc_free_bytes(P.pool[p]) == total[p] && P.pool[p]->nfree == 1);
    CHECK(P.own[RD3_VO_GEM_HI].bytes == 0 && P.own[RD3_VO_GEM].bytes == 0);
}


/* kernel-internal BOs (seq64, user queue objects) need CPU access and have no GTT fallback:
 * the general part first, then the reserve; a client's BO never takes the reserve */
static void test_internal_spills_to_reserve(void)
{
    int pool = -1;
    uint64_t off, last = 0;
    unsigned n = 0;
    setup(-1);
    /* the GEM pool full of clients' BOs */
    while (rdna3_vram_alloc_gem(&P, 1 * MiB, 4096, 0, 0, &pool) != RANGE_ALLOC_FAIL)
        n++;
    CHECK(n == 48 && range_alloc_free_bytes(&gem) == 0);          /* 144 MiB heap - 96 MiB reserve */
    CHECK(rdna3_vram_alloc_gem(&P, 64 * KiB, 4096, 0, 0, &pool) == RANGE_ALLOC_FAIL);   /* a client: no */
    /* seq64 (64 KiB) and a queue object (CPU access, contiguous, internal) still succeed, from the reserve */
    off = rdna3_vram_alloc_gem(&P, 64 * KiB, 4096, 0, 1, &pool);
    CHECK(off != RANGE_ALLOC_FAIL && pool == RD3_POOL_ACC && off < acc.end);
    CHECK(P.own[RD3_VO_KERNEL].live == 1 && P.own[RD3_VO_KERNEL].bytes == 64 * KiB && P.own[RD3_VO_KERNEL].refused == 0);
    CHECK(rdna3_vram_bo_owner(&P, off, 1) == RD3_VO_KERNEL);
    CHECK(rdna3_vram_bo_owner(&P, off, 0) == RD3_VO_GEM);
    /* the reserve is then 64 KiB short for the display only by what the kernel BO took */
    CHECK(range_alloc_free_bytes(&acc) == 96 * MiB - 64 * KiB);
    CHECK(rdna3_vram_free(&P, off, 64 * KiB, RD3_VO_KERNEL) == 0 && P.own[RD3_VO_KERNEL].live == 0);
    CHECK(range_alloc_free_bytes(&acc) == 96 * MiB);
    /* both full: refused, as the kernel's */
    while (rdna3_vram_alloc_accel(&P, 1 * MiB, RD3_VO_ACCEL) != RANGE_ALLOC_FAIL)
        ;
    last = rdna3_vram_alloc_gem(&P, 64 * KiB, 4096, 0, 1, &pool);
    CHECK(last == RANGE_ALLOC_FAIL && P.own[RD3_VO_KERNEL].refused == 1);
}

/* a free that fails is counted and the owner keeps what it holds; a lost range lets the owner go;
 * a retired range is held by nobody */
static void test_free_failures(void)
{
    int pool;
    uint64_t a1, a2, base, off, i;
    setup(-1);
    a1 = rdna3_vram_alloc_gem(&P, 1 * MiB, 4096, 0, 0, &pool);
    a2 = rdna3_vram_alloc_gem(&P, 1 * MiB, 4096, 0, 0, &pool);
    CHECK(a1 != RANGE_ALLOC_FAIL && a2 != RANGE_ALLOC_FAIL);
    CHECK(rdna3_vram_free(&P, a1, 1 * MiB, RD3_VO_GEM) == 0);
    CHECK(P.own[RD3_VO_GEM].live == 1 && P.own[RD3_VO_GEM].bytes == 1 * MiB);
    /* a double free: refused, counted, the owner's totals untouched */
    CHECK(rdna3_vram_free(&P, a1, 1 * MiB, RD3_VO_GEM) == -1);
    CHECK(P.free_fail == 1 && P.free_fail_bytes == 1 * MiB);
    CHECK(P.own[RD3_VO_GEM].live == 1 && P.own[RD3_VO_GEM].bytes == 1 * MiB && P.own[RD3_VO_GEM].frees == 1);
    /* outside every heap, and a bad owner */
    CHECK(rdna3_vram_free(&P, HEAP_END + 64 * MiB, 4096, RD3_VO_GEM) == -1 && P.free_fail == 2);
    CHECK(rdna3_vram_free(&P, a2, 4096, 99) == -1 && P.free_fail == 3);
    CHECK(rdna3_vram_free(&P, a2, 1 * MiB, RD3_VO_GEM) == 0);
    /* no pool (heaps gone): refused, not a crash */
    {
        rdna3_vram_pools z;
        memset(&z, 0, sizeof(z));
        CHECK(rdna3_vram_free(&z, a1, 4096, RD3_VO_GEM) == -1 && z.free_fail == 1);
    }
    /* a full free list: the put is lost (-2) and the owner lets go */
    setup(0);   /* no reserve: the general part is the whole visible heap */
    base = 0;
    for (i = 0; i < 9000; i++) {
        off = rdna3_vram_alloc_gem(&P, 4096, 4096, 0, 0, &pool);
        if (off == RANGE_ALLOC_FAIL)
            break;
        if (!i)
            base = off;
    }
    CHECK(i >= 9000 && P.own[RD3_VO_GEM].live == 9000);
    {
        unsigned lost = 0, ok = 0;
        for (i = 0; i < 9000; i += 2) {
            int r = rdna3_vram_free(&P, base + i * 4096, 4096, RD3_VO_GEM);
            if (r == -2) lost++; else if (r == 0) ok++; else CHECK(0);
        }
        CHECK(lost > 0 && ok > 0 && P.free_fail == 0);
        CHECK(P.own[RD3_VO_GEM].live == 9000 - 4500);                   /* the lost ones are not held either */
        CHECK(gem.lost_ranges == lost && gem.lost_bytes == (uint64_t)lost * 4096);
    }
    /* a retired range (a clear that timed out) */
    setup(-1);
    off = rdna3_vram_alloc_gem(&P, 1 * MiB, 4096, 0, 0, &pool);
    CHECK(off != RANGE_ALLOC_FAIL && P.own[RD3_VO_GEM].bytes == 1 * MiB);
    rdna3_vram_retire(&P, 1 * MiB, RD3_VO_GEM);
    CHECK(P.own[RD3_VO_GEM].bytes == 0 && P.own[RD3_VO_GEM].live == 0 && P.retired_n == 1 && P.retired_bytes == 1 * MiB);
    CHECK(range_alloc_free_bytes(&gem) == 48 * MiB - 1 * MiB);          /* not free again */
}

/* ---- through the UAPI: GEM_CREATE with the kext's decisions (drmBoAlloc's flow
 * on the shared header, no clear) ---- */
static unsigned live_bos;
static int h_gtt_fail;                     /* the OS's GTT allocation fails */
static unsigned h_hook_calls;
static uint64_t h_hook_size;
static int h_hook_why;
static void h_gtt_failed(void *c, uint64_t size, int err, int why)
{
    (void)c;
    CHECK(err < 0);
    h_hook_calls++; h_hook_size = size; h_hook_why = why;
}
static void *h_zalloc(void *c, size_t n) { (void)c; return calloc(1, n); }
static void h_free(void *c, void *p, size_t n) { (void)c; (void)n; free(p); }
static int h_bo_alloc(void *c, rdna3_bo *bo)
{
    (void)c;
    if (bo->placement == AMDGPU_GEM_DOMAIN_VRAM) {
        int pool, no_cpu = (bo->flags & AMDGPU_GEM_CREATE_NO_CPU_ACCESS) != 0;
        uint64_t off = rdna3_vram_alloc_gem(&P, bo->size, bo->alignment, no_cpu, (bo->flags & RD_BO_FLAG_INTERNAL) != 0, &pool);
        if (off == RANGE_ALLOC_FAIL)
            return -RD_ENOMEM;
        bo->vram_offset = off;
        live_bos++;
        return 0;
    }
    if (h_gtt_fail)
        return -RD_ENOMEM;
    bo->backing = calloc(1, 16);    /* GTT: any backing */
    live_bos++;
    return 0;
}
static void h_bo_free(void *c, rdna3_bo *bo)
{
    (void)c;
    if (bo->placement == AMDGPU_GEM_DOMAIN_VRAM)
        CHECK(rdna3_vram_free(&P, bo->vram_offset, bo->size,
                              rdna3_vram_bo_owner(&P, bo->vram_offset, (bo->flags & RD_BO_FLAG_INTERNAL) != 0)) == 0);
    else
        free(bo->backing);
    live_bos--;
}
static int h_copyin(void *c, void *d, uint64_t u, size_t n) { (void)c; memcpy(d, (void *)(uintptr_t)u, n); return 0; }
static int h_copyout(void *c, uint64_t u, const void *s, size_t n) { (void)c; memcpy((void *)(uintptr_t)u, s, n); return 0; }

static int gem_create(rdna3_drm_file *f, uint32_t dom, uint64_t size, uint64_t align, uint64_t flags,
                      uint32_t *handle)
{
    union drm_amdgpu_gem_create gc;
    int r;
    memset(&gc, 0, sizeof(gc));
    gc.in.bo_size = size; gc.in.alignment = align; gc.in.domains = dom; gc.in.domain_flags = flags;
    r = rdna3_drm_ioctl(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_CREATE), &gc, sizeof(gc));
    if (!r)
        *handle = gc.out.handle;
    return r;
}

static void test_uapi_placement(void)
{
    static rdna3_drm_device dev;
    static rdna3_drm_file file;
    rdna3_drm_ops ops;
    amd_discovery disc;
    uint32_t h;
    rdna3_bo *bo;
    uint64_t vis0, total, big;
    unsigned i;

    setup(-1);
    memset(&ops, 0, sizeof(ops));
    ops.zalloc = h_zalloc; ops.free = h_free; ops.bo_alloc = h_bo_alloc; ops.bo_free = h_bo_free;
    ops.copyin = h_copyin; ops.copyout = h_copyout;
    ops.gtt_fallback_failed = h_gtt_failed;
    memset(&disc, 0, sizeof(disc));
    disc.has_gc_info = 1; disc.gc.num_se = 3; disc.gc.num_sa_per_se = 2; disc.gc.num_wgp0_per_sa = 5;
    disc.gc.num_rb_per_se = 4; disc.gc.wave_size = 32; disc.gc.num_gl2c = 16; disc.gc.gl2c_per_gpu = 4u << 20;
    disc.num_ips = 1; disc.ips[0].hw_id = AMD_HWID_GC; disc.ips[0].major = 11; disc.ips[0].revision = 3;
    rdna3_drm_device_init(&dev, &ops, &disc, 0x747E, 0xC8, 0, 16368ull * MiB, VIS_END, 8ull << 30);
    CHECK(rdna3_drm_file_open(&dev, &file) == 0);
    vis0 = vis_free();

    /* VRAM|GTT without CPU access: beyond BAR0, no visible byte counted or taken */
    CHECK(gem_create(&file, AMDGPU_GEM_DOMAIN_VRAM | AMDGPU_GEM_DOMAIN_GTT, 4096, 2 * MiB,
                     AMDGPU_GEM_CREATE_NO_CPU_ACCESS, &h) == 0);
    bo = rdna3_drm_bo_lookup(&file, h);
    CHECK(bo && bo->placement == AMDGPU_GEM_DOMAIN_VRAM && bo->vram_offset >= VIS_END);
    CHECK(bo && bo->vram_offset % 4096 == 0);                       /* the 2 MiB hint did not need a 2 MiB hole */
    CHECK(dev.vis_vram_usage == 0 && vis_free() == vis0);

    /* beyond BAR0 full: the BO goes to GTT (the UAPI's retry), the visible heap is not touched */
    total = range_alloc_free_bytes(&hi);
    while ((big = range_alloc_largest_free(&hi)) != 0)
        CHECK(rdna3_vram_alloc_gem(&P, big, 4096, 1, 0, NULL) != RANGE_ALLOC_FAIL);
    (void)total;
    CHECK(gem_create(&file, AMDGPU_GEM_DOMAIN_VRAM, 4096, 4096, AMDGPU_GEM_CREATE_NO_CPU_ACCESS, &h) == 0);
    bo = rdna3_drm_bo_lookup(&file, h);
    CHECK(bo && bo->placement == AMDGPU_GEM_DOMAIN_GTT);
    CHECK(vis_free() == vis0 && dev.vis_vram_usage == 0);

    /* a client cannot ask for the kernel's reserve: the internal bit is dropped and not reported back */
    CHECK(gem_create(&file, AMDGPU_GEM_DOMAIN_VRAM, 4096, 4096,
                     AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED | RD_BO_FLAG_INTERNAL, &h) == 0);
    bo = rdna3_drm_bo_lookup(&file, h);
    CHECK(bo && !(bo->flags & RD_BO_FLAG_INTERNAL) && (bo->flags & AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED));
    CHECK(P.own[RD3_VO_KERNEL].allocs == 0 && P.own[RD3_VO_GEM].allocs >= 1);

    /* VRAM refused and the GTT budget is gone: the client's BO fails and it is counted, not silent */
    {
        uint32_t gh[16], hh;
        unsigned ng = 0, k;
        CHECK(dev.gtt_fallback_fail == 0 && h_hook_calls == 0);
        while (dev.gtt_usage + (1ull << 30) <= dev.gtt_size && ng < 16)
            CHECK(gem_create(&file, AMDGPU_GEM_DOMAIN_GTT, 1ull << 30, 4096, 0, &gh[ng++]) == 0);
        if (dev.gtt_usage < dev.gtt_size && ng < 16)   /* the rest of the budget */
            CHECK(gem_create(&file, AMDGPU_GEM_DOMAIN_GTT, dev.gtt_size - dev.gtt_usage, 4096, 0, &gh[ng++]) == 0);
        CHECK(dev.gtt_usage == dev.gtt_size);
        CHECK(gem_create(&file, AMDGPU_GEM_DOMAIN_VRAM, 8192, 4096, AMDGPU_GEM_CREATE_NO_CPU_ACCESS, &hh) == -RD_ENOMEM);
        CHECK(dev.gtt_fallback_fail == 1 && dev.gtt_fallback_fail_bytes == 8192);
        CHECK(h_hook_calls == 1 && h_hook_size == 8192 && h_hook_why == 1);
        for (k = 0; k < ng; k++) {
            struct drm_gem_close cl; cl.handle = gh[k]; cl.pad = 0;
            CHECK(rdna3_drm_ioctl(&file, RD_NR_GEM_CLOSE, &cl, sizeof(cl)) == 0);
        }
        /* GTT within budget but the OS's allocation fails */
        h_gtt_fail = 1;
        CHECK(gem_create(&file, AMDGPU_GEM_DOMAIN_VRAM, 4096, 4096, AMDGPU_GEM_CREATE_NO_CPU_ACCESS, &hh) == -RD_ENOMEM);
        h_gtt_fail = 0;
        CHECK(dev.gtt_fallback_fail == 2 && h_hook_calls == 2 && h_hook_why == 2);
        /* a VRAM|GTT BO that VRAM refuses and GTT takes is no failure */
        CHECK(gem_create(&file, AMDGPU_GEM_DOMAIN_VRAM, 4096, 4096, AMDGPU_GEM_CREATE_NO_CPU_ACCESS, &hh) == 0);
        CHECK(dev.gtt_fallback_fail == 2);
    }

    /* BOs the CPU may map fill the general part, never the reserve; the next goes to GTT */
    for (i = 0; i < 64; i++) {
        CHECK(gem_create(&file, AMDGPU_GEM_DOMAIN_VRAM, 36992 * KiB, 2 * MiB,
                         AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED, &h) == 0);
        bo = rdna3_drm_bo_lookup(&file, h);
        if (bo && bo->placement == AMDGPU_GEM_DOMAIN_GTT)
            break;
        CHECK(bo && bo->vram_offset >= acc.end && bo->vram_offset + bo->size <= VIS_END);
    }
    CHECK(i >= 1 && i < 64);
    CHECK(range_alloc_free_bytes(&acc) == 96 * MiB);
    /* ... so the display's 8 MB buffers are still granted */
    for (i = 0; i < 8; i++)
        CHECK(rdna3_vram_alloc_accel(&P, 8294400, RD3_VO_ACCEL) != RANGE_ALLOC_FAIL);
    rdna3_drm_file_close(&file);
    rdna3_drm_device_fini(&dev);
}

int main(void)
{
    test_reserve_size();
    test_layout();
    test_align();
    test_no_cpu_stays_out();
    test_reserve_for_the_accelerator();
    test_accel_spill_and_refusal();
    test_no_reserve();
    test_soak();
    test_internal_spills_to_reserve();
    test_free_failures();
    test_uapi_placement();
    if (failures) {
        fprintf(stderr, "test_vram_place: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_vram_place: all checks passed\n");
    return 0;
}
