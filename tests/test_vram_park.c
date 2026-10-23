/*
 * test_vram_park.c - common/rdna3_vram_park.h on the host (DRIVER-PLAN N41;
 * card traces 2026-10-17d / 17c: the VRAM of surfaces the Metal plugin had
 * imported was freed under the live surface 64 / 83 times and came back at
 * another address 13 / 19 times, so the family, its dirty mark and every
 * page-off moved to VRAM nobody wrote).
 *
 * First each transition, then a model of the family over a fake framebuffer
 * allocator that never hands the same address twice while one is held:
 * a surface imported once by the plugin (its BO keeps the first address),
 * then page-off / CPU access / GPU use cycles (deallocPhysical,
 * allocPhysical), a dirty mark while parked (the restore), a grow, and
 * free(). Checked with parking on: the family's address stays the one the
 * plugin draws into until the resource grows, the generation is kept
 * across a park and is new after a grow, nothing leaks and nothing is
 * released twice. With parking off: the address moves (the bug, kept as
 * the A/B's other side). Then the security review's policy: only a BO's
 * allocation is parked, and parks no BO holds go under pressure or past
 * the cap, oldest first.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "rdna3_vram_park.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* ---- a fake framebuffer: addresses only grow, every allocation is live until released once ---- */
#define FB_MAX 512
static struct { unsigned long long phys, bytes; int live; } fb[FB_MAX];
static unsigned fb_n;
static unsigned long long fb_next = 0x80000000ull, fb_gen;
static unsigned fb_double_free, fb_unknown_free;

static struct rd3_vp_alloc fb_alloc(unsigned long long want)
{
    struct rd3_vp_alloc a;
    rd3_vp_clear(&a);
    if (fb_n >= FB_MAX) return a;
    fb[fb_n].phys = fb_next; fb[fb_n].bytes = (want + 0xffffull) & ~0xffffull; fb[fb_n].live = 1;
    a.handle = &fb[fb_n]; a.kva = (void *)(unsigned long)(0x1000 + fb_n); a.phys = fb_next;
    a.bytes = fb[fb_n].bytes; a.cookie = fb_next - 0x80000000ull; a.gen = ++fb_gen;
    fb_next += fb[fb_n].bytes;
    fb_n++;
    return a;
}

static void fb_release(const struct rd3_vp_alloc *a)
{
    if (!rd3_vp_valid(a)) return;
    for (unsigned i = 0; i < fb_n; i++) if (a->handle == &fb[i]) {
        if (!fb[i].live) fb_double_free++;
        fb[i].live = 0;
        return;
    }
    fb_unknown_free++;
}

static unsigned fb_live(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < fb_n; i++) n += fb[i].live ? 1u : 0u;
    return n;
}

static void fb_reset(void)
{
    memset(fb, 0, sizeof fb);
    fb_n = 0; fb_next = 0x80000000ull; fb_gen = 0; fb_double_free = 0; fb_unknown_free = 0;
}

/* ---- the kernel's three entry points over the state machine ---- */
struct vm { struct rd3_vp vp; unsigned long long want; int sticky; unsigned reuses, grows, parks, frees; };

static int alloc_physical(struct vm *m)
{
    struct rd3_vp_alloc rel;
    if (rd3_vp_valid(&m->vp.live)) return 0;   /* already backed: refused, as the kernel does */
    const enum rd3_vp_take_res t = rd3_vp_take(&m->vp, m->want, &rel);
    if (t == RD3_VPT_REUSE) { m->reuses++; return 1; }
    if (t == RD3_VPT_GROW) { m->grows++; fb_release(&rel); }
    m->vp.live = fb_alloc(m->want);
    return rd3_vp_valid(&m->vp.live);
}

static void dealloc_physical(struct vm *m)
{
    struct rd3_vp_alloc rel;
    const enum rd3_vp_drop_res d = rd3_vp_drop(&m->vp, m->sticky, &rel);
    if (d == RD3_VPD_PARKED || d == RD3_VPD_REPARKED) m->parks++;
    if (d == RD3_VPD_FREED || d == RD3_VPD_REPARKED) { m->frees++; fb_release(&rel); }
}

static unsigned vm_free(struct vm *m)
{
    struct rd3_vp_alloc rel[2];
    const unsigned mask = rd3_vp_free(&m->vp, rel);
    fb_release(&rel[0]);
    fb_release(&rel[1]);
    return mask;
}

static void transitions(void)
{
    struct rd3_vp v;
    struct rd3_vp_alloc rel, a, two[2];
    memset(&v, 0, sizeof v);
    fb_reset();
    /* nothing parked: allocate */
    CHECK(rd3_vp_take(&v, 4096, &rel) == RD3_VPT_NEW && !rd3_vp_valid(&rel));
    a = fb_alloc(0x10000);
    v.live = a;
    /* a live allocation: take changes nothing */
    CHECK(rd3_vp_take(&v, 4096, &rel) == RD3_VPT_NEW && rd3_vp_same(&v.live, &a));
    /* park, then the same allocation back (generation kept) */
    CHECK(rd3_vp_drop(&v, 1, &rel) == RD3_VPD_PARKED && !rd3_vp_valid(&rel));
    CHECK(!rd3_vp_valid(&v.live) && rd3_vp_same(&v.park, &a) && v.park.gen == a.gen);
    CHECK(rd3_vp_can_restore(&v, 0x10000) && rd3_vp_can_restore(&v, 1) && !rd3_vp_can_restore(&v, 0x10001));
    CHECK(!rd3_vp_can_restore(&v, 0));
    CHECK(rd3_vp_take(&v, 0x8000, &rel) == RD3_VPT_REUSE && rd3_vp_same(&v.live, &a) && v.live.gen == a.gen);
    CHECK(!rd3_vp_valid(&v.park) && !rd3_vp_valid(&rel));
    CHECK(!rd3_vp_can_restore(&v, 0x8000));   /* live: nothing to restore */
    /* grow: the park is handed back, nothing live */
    CHECK(rd3_vp_drop(&v, 1, &rel) == RD3_VPD_PARKED);
    CHECK(rd3_vp_take(&v, 0x20000, &rel) == RD3_VPT_GROW && rd3_vp_same(&rel, &a));
    CHECK(!rd3_vp_valid(&v.live) && !rd3_vp_valid(&v.park));
    /* a take of 0 bytes never reuses (the kernel refuses 0 before) */
    v.park = a;
    CHECK(rd3_vp_take(&v, 0, &rel) == RD3_VPT_GROW && rd3_vp_same(&rel, &a));
    /* sticky off: the live allocation is handed back */
    v.live = a;
    CHECK(rd3_vp_drop(&v, 0, &rel) == RD3_VPD_FREED && rd3_vp_same(&rel, &a) && !rd3_vp_valid(&v.live));
    CHECK(rd3_vp_drop(&v, 1, &rel) == RD3_VPD_NONE && !rd3_vp_valid(&rel));
    /* both at once (never by take/drop; defensive): a drop re-parks and hands the old park back */
    v.live = a; v.park = fb_alloc(0x10000);
    {
        const struct rd3_vp_alloc old = v.park;
        CHECK(rd3_vp_drop(&v, 1, &rel) == RD3_VPD_REPARKED && rd3_vp_same(&rel, &old) && rd3_vp_same(&v.park, &a));
    }
    /* free: what is live and what is parked */
    v.live = fb_alloc(0x10000);
    CHECK(rd3_vp_free(&v, two) == 3u && rd3_vp_valid(&two[0]) && rd3_vp_same(&two[1], &a));
    CHECK(!rd3_vp_valid(&v.live) && !rd3_vp_valid(&v.park));
    CHECK(rd3_vp_free(&v, two) == 0u && !rd3_vp_valid(&two[0]) && !rd3_vp_valid(&two[1]));
}

/* the family over one imported surface, as the traces show it */
static void model(int sticky)
{
    struct vm m;
    unsigned long long bo_phys, bo_gen;
    unsigned moved = 0, dirty_moved = 0, cycles = 0;
    memset(&m, 0, sizeof m);
    fb_reset();
    m.want = 1920ull * 1080 * 4; m.sticky = sticky;
    CHECK(alloc_physical(&m));
    bo_phys = m.vp.live.phys; bo_gen = m.vp.live.gen;   /* the plugin's import (RD3K_VRAM_FN_REF holds it) */
    /* another surface allocates meanwhile, so a fresh allocation never lands on the old address */
    for (unsigned i = 0; i < 83; i++) {
        dealloc_physical(&m);                     /* page-off for a CPU access, then the family unwires */
        (void)fb_alloc(0x10000);                  /* someone else's buffer */
        if (i % 4 == 3) {                         /* the plugin's dirty mark arrives while it is parked */
            if (rd3_vp_can_restore(&m.vp, m.want)) {
                const struct rd3_vp_alloc parked = m.vp.park;
                CHECK(alloc_physical(&m) && rd3_vp_same(&m.vp.live, &parked));
            } else if (!rd3_vp_valid(&m.vp.live)) {
                CHECK(!sticky);                    /* parking on: a fitting park always restores */
                CHECK(alloc_physical(&m));         /* parking off: the family's wire allocates anew */
            }
            if (m.vp.live.phys != bo_phys) dirty_moved++;
        } else {
            CHECK(alloc_physical(&m));            /* the GPU needs it again */
        }
        if (m.vp.live.phys != bo_phys) moved++;
        cycles++;
    }
    if (sticky) {
        CHECK(moved == 0 && dirty_moved == 0);
        CHECK(m.vp.live.gen == bo_gen);           /* the same allocation all along: the plugin's mark still holds */
        CHECK(m.reuses == cycles && m.frees == 0 && m.parks == cycles);
    } else {
        CHECK(moved == cycles && dirty_moved > 0);   /* the bug the traces showed */
        CHECK(m.frees == cycles && m.reuses == 0);
    }
    /* the resource grows (a resize): the parked VRAM cannot hold it */
    dealloc_physical(&m);
    m.want *= 2;
    CHECK(alloc_physical(&m));
    CHECK(m.vp.live.phys != bo_phys && m.vp.live.gen != bo_gen);
    if (sticky) CHECK(m.grows == 1);
    /* the object goes: whatever it holds is released once */
    dealloc_physical(&m);
    CHECK(vm_free(&m) == (sticky ? 2u : 0u));
    CHECK(fb_double_free == 0 && fb_unknown_free == 0);
    /* only the other surfaces' buffers are left */
    CHECK(fb_live() == cycles);
    /* free with a live allocation (the family frees without deallocPhysical) */
    CHECK(alloc_physical(&m));
    CHECK(vm_free(&m) == 1u && fb_live() == cycles && fb_double_free == 0);
}

/* ECC review of N41, L1: the aperture descriptor is kept only over what the vidmem holds */
static void apdesc(void)
{
    struct rd3_vp v;
    struct rd3_vp_alloc rel;
    memset(&v, 0, sizeof v);
    v.live = fb_alloc(0x40000);
    const unsigned long long p = v.live.phys, b = v.live.bytes;
    CHECK(rd3_vp_apdesc_keep(v.live.phys, v.live.bytes, &v.park, p, b));
    CHECK(!rd3_vp_apdesc_keep(v.live.phys, v.live.bytes, &v.park, p, b / 2));     /* same address, other size */
    CHECK(!rd3_vp_apdesc_keep(v.live.phys + b, v.live.bytes, &v.park, p, b));     /* moved */
    CHECK(!rd3_vp_apdesc_keep(v.live.phys, v.live.bytes, &v.park, 0, 0));         /* none cached */
    /* parked: the range is still allocated, the descriptor stays */
    CHECK(rd3_vp_drop(&v, 1, &rel) == RD3_VPD_PARKED);
    CHECK(rd3_vp_apdesc_keep(0, 0, &v.park, p, b));
    /* released (sticky off, or grown): nothing holds it any more */
    struct rd3_vp_alloc gone = v.park;
    CHECK(rd3_vp_take(&v, b * 2, &rel) == RD3_VPT_GROW && rel.phys == gone.phys);
    fb_release(&rel);
    CHECK(!rd3_vp_apdesc_keep(0, 0, &v.park, p, b));
    /* a new, bigger allocation elsewhere: rebuilt */
    v.live = fb_alloc(b * 2);
    CHECK(!rd3_vp_apdesc_keep(v.live.phys, v.live.bytes, &v.park, p, b));
    fb_release(&v.live);
}

/* security review of N41: what is parked, and which parks go when VRAM runs out or past the cap */
static void policy(void)
{
    CHECK(rd3_vp_should_park(1, 0, 7, 7));
    CHECK(!rd3_vp_should_park(0, 0, 7, 7));    /* sticky off */
    CHECK(!rd3_vp_should_park(1, 1, 7, 7));    /* a scanout */
    CHECK(!rd3_vp_should_park(1, 0, 7, 0));    /* never imported */
    CHECK(!rd3_vp_should_park(1, 0, 8, 7));    /* imported, then the resource grew: a newer allocation */
    CHECK(!rd3_vp_should_park(1, 0, 0, 0));    /* nothing live */
    const struct rd3_vp_cand c[5] = {
        { 30, 100, 1, 0, 0 }, { 10, 200, 2, 0, 0 }, { 20, 300, 1, 0, 0 }, { 5, 400, 0, 0, 0 }, { 40, 50, 1, 0, 0 },
    };
    unsigned char out[5];
    /* pressure: every park no BO holds; never a BO's (1) or an unknown one (3) */
    CHECK(rd3_vp_pick_release(c, 5, 1050, 0, 1, out) == 450);
    CHECK(out[0] && !out[1] && out[2] && !out[3] && out[4]);
    /* the cap: the oldest no BO holds until under it */
    CHECK(rd3_vp_pick_release(c, 5, 1050, 800, 0, out) == 300);   /* 2 (since 20) only: 750 */
    CHECK(!out[0] && !out[1] && out[2] && !out[3] && !out[4]);
    CHECK(rd3_vp_pick_release(c, 5, 1050, 700, 0, out) == 400);   /* 2, then 0 (since 30): 650 */
    CHECK(out[0] && out[2] && !out[4]);
    /* nothing releasable left: stops, never a BO's */
    CHECK(rd3_vp_pick_release(c, 5, 1050, 1, 0, out) == 450 && !out[1] && !out[3]);
    /* under the cap, or no cap: nothing */
    CHECK(rd3_vp_pick_release(c, 5, 1050, 2000, 0, out) == 0 && !out[0] && !out[2]);
    CHECK(rd3_vp_pick_release(c, 5, 1050, 0, 0, out) == 0);
    CHECK(rd3_vp_pick_release(c, 0, 0, 0, 1, out) == 0);
}

/* ECC review round 3 of N41: the release pages through the parks (accel rd3VmParkTrim's loop, replayed): 200
 * parks a BO holds are the oldest, 5 releasable ones behind them. One page of 128 (before) found none; paging
 * with the held ones dropped (rd3_vp_keep_releasable) finds all 5 */
#define PAGE 128u
static void paged_trim(void)
{
    enum { NPARK = 205 };
    static struct rd3_vp_cand list[NPARK];
    for (unsigned i = 0; i < NPARK; i++) {
        list[i].since = 100 + i; list[i].bytes = 1000; list[i].refs = i < 200 ? 2 : 1;
        list[i].obj = &list[i]; list[i].handle = (void *)(unsigned long)(i + 1);
    }
    struct rd3_vp_cand vc[PAGE];
    unsigned char out[PAGE];
    /* before: the oldest page only */
    for (unsigned i = 0; i < PAGE; i++) vc[i] = list[i];
    CHECK(rd3_vp_pick_release(vc, PAGE, NPARK * 1000ull, 0, 1, out) == 0);
    /* after: pages, the held ones leave each page */
    unsigned n = 0, looked = 0, pages = 0;
    for (int more = 1; more && n < PAGE && pages < 8; pages++) {
        const unsigned base = n;
        unsigned idx = 0;
        more = 0;
        for (unsigned i = 0; i < NPARK; i++) {
            if (idx++ < looked) continue;
            if (n >= PAGE) { more = 1; continue; }
            vc[n] = list[i]; vc[n].refs = 0;
            n++;
        }
        looked += n - base;
        for (unsigned i = base; i < n; i++) vc[i].refs = ((struct rd3_vp_cand *)vc[i].obj)->refs;   /* the core */
        n = base + rd3_vp_keep_releasable(vc + base, n - base);
    }
    CHECK(pages == 2 && looked == NPARK && n == 5);
    CHECK(vc[0].obj == &list[200] && vc[4].obj == &list[204] && vc[0].handle == list[200].handle);   /* in order */
    CHECK(rd3_vp_pick_release(vc, n, NPARK * 1000ull, 0, 1, out) == 5000);
    /* the cap: the oldest releasable first */
    CHECK(rd3_vp_pick_release(vc, n, NPARK * 1000ull, NPARK * 1000ull - 1500, 0, out) == 2000 && out[0] && out[1] &&
          !out[2]);
    /* unknown counts (0) never stay, a page of releasable ones stays whole */
    struct rd3_vp_cand m[4] = { { 1, 1, 0, 0, 0 }, { 2, 2, 1, 0, 0 }, { 3, 3, 3, 0, 0 }, { 4, 4, 1, 0, 0 } };
    CHECK(rd3_vp_keep_releasable(m, 4) == 2 && m[0].since == 2 && m[1].since == 4);
    CHECK(rd3_vp_keep_releasable(m, 2) == 2 && rd3_vp_keep_releasable(m, 0) == 0);
}

int main(void)
{
    transitions();
    model(1);
    model(0);
    fb_reset();
    apdesc();
    policy();
    paged_trim();
    if (fails) { printf("test_vram_park: %d FAILED\n", fails); return 1; }
    printf("test_vram_park: ok\n");
    return 0;
}
