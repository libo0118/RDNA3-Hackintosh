/*
 * Host test for common/rdna3_bo_map.h with the real UAPI (DRIVER-PLAN N47
 * (b)): a CPU mapping of a VRAM or doorbell BO holds the BO, so neither
 * GEM_CLOSE nor closing the file hands its VRAM range (or doorbell page) to
 * anyone else while it is mapped; the last unmap gives it back.
 *
 * The model of the kext: map_ref() is RDNA3Accel::drmMemoryForOffset +
 * drmBoMapDescriptor (classify, reuse the BO's descriptor through its weak
 * link or make one that takes a BO reference), md_release() is
 * RDNA3BoMemory::taggedRelease + free (rd3_bomap_release_unlinks, then the
 * BO reference goes). xnu_map() / xnu_unmap() / conn_close() are what
 * IOConnectMapMemory64, IOConnectUnmapMemory64 and the connection's end do
 * with the descriptor (IOUserClient.cpp, IOMemoryDescriptor.cpp of
 * xnu-12377: the descriptor clientMemoryForType returns is released after
 * createMappingInTask; the IOMemoryMap holds one reference to it; a second
 * map of it in the same task reuses that map; the unmap finds it through
 * clientMemoryForType again). VRAM placement goes through range_alloc as
 * the kext's heap does, so a reused range is visible. A descriptor is
 * charged to the file's uid (rdna3_drm_bomap_charge) while it lives, as
 * drmBoMapDescriptor / drmBoMapRelease do (ECC review round 1).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rdna3_drm.h"
#include "rdna3_bo_map.h"
#include "range_alloc.h"

static int failures;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
    failures++; } } while (0)
#define IOCTL(f, nr, arg) rdna3_drm_ioctl((f), (nr), &(arg), sizeof(arg))

#define BAR0 (256ull << 20)
#define BAR2 (2ull << 20)

static range_alloc heap;
static unsigned live_bos;

static void *h_zalloc(void *c, size_t n) { (void)c; return calloc(1, n); }
static void h_free(void *c, void *p, size_t n) { (void)c; (void)n; free(p); }
static int h_bo_alloc(void *c, rdna3_bo *bo)
{
    (void)c;
    if (bo->placement == AMDGPU_GEM_DOMAIN_VRAM) {
        uint64_t off = range_alloc_get(&heap, bo->size, 4096);
        if (off == RANGE_ALLOC_FAIL)
            return -RD_ENOMEM;
        bo->vram_offset = off;
        bo->backing = &heap;   /* VRAM: no system pages */
    } else {
        bo->backing = calloc(1, (size_t)bo->size);
        if (!bo->backing)
            return -RD_ENOMEM;
    }
    live_bos++;
    return 0;
}
static int surf_holds;   /* the "accelerator's" holds on surface VRAM */
static void h_bo_free(void *c, rdna3_bo *bo)
{
    (void)c;
    if (bo->foreign)
        surf_holds--;
    else if (bo->placement == AMDGPU_GEM_DOMAIN_VRAM)
        CHECK(range_alloc_put(&heap, bo->vram_offset, bo->size) == 0);
    else if (!bo->userptr)
        free(bo->backing);
    live_bos--;
}
static int h_bo_userptr(void *c, void *os, rdna3_bo *bo, uint64_t uaddr, uint32_t flags)
{
    (void)c; (void)os; (void)flags;
    bo->backing = (void *)(uintptr_t)uaddr;
    live_bos++;
    return 0;
}
/* IOSurface 7: 100 MiB of the accelerator's VRAM at 128 MiB (more than
 * half of RD_SURFACE_BO_FILE_MAX's 128 MiB over two imports) */
static int h_surface_bo(void *c, void *os, rdna3_bo *bo, uint32_t id, uint32_t plane)
{
    (void)c; (void)os;
    if (id != 7 || plane)
        return -RD_ENOENT;
    bo->vram_offset = 128ull << 20;
    bo->size = 100ull << 20;
    bo->backing = &surf_holds;
    surf_holds++;
    live_bos++;
    return 0;
}
static int h_copyin(void *c, void *d, uint64_t u, size_t n) { (void)c; memcpy(d, (void *)(uintptr_t)u, n); return 0; }
static int h_copyout(void *c, uint64_t u, const void *s, size_t n) { (void)c; memcpy((void *)(uintptr_t)u, s, n); return 0; }

/* ---- the kext's descriptor (RDNA3BoMemory), one per BO ---- */
typedef struct md {
    int refs;               /* the OSObject retain count */
    int mapped;             /* an IOMemoryMap of it exists in the (single) task */
    rdna3_bo *bo;
    rdna3_drm_device *dev;
} md;
static int md_made, md_freed;

static int last_quota;   /* the last rdna3_drm_bomap_charge refusal */

/* drmMemoryForOffset: a reference for the caller, or NULL with the kind
 * (and last_quota when the uid's quota refused it) */
static md *map_ref(rdna3_drm_file *f, uint64_t off, enum rd3_bomap_kind *kind)
{
    rdna3_bo *bo = rdna3_drm_bo_by_mmap_offset(f, off);
    enum rd3_bomap_kind k = rd3_bomap_classify(bo, BAR0, BAR2);
    md *m;
    int q;
    if (kind)
        *kind = k;
    last_quota = 0;
    if (!rd3_bomap_per_bo(k))
        return NULL;
    m = (md *)rd3_bomap_lookup(&bo->os_map);
    if (m) {
        m->refs++;
        return m;
    }
    q = rdna3_drm_bomap_charge(f->dev, f, bo);
    if (q) {
        last_quota = q;
        return NULL;
    }
    m = (md *)calloc(1, sizeof(*m));
    if (!m) {
        rdna3_drm_bomap_uncharge(f->dev, bo);
        return NULL;
    }
    m->refs = 1;
    m->bo = bo;
    m->dev = f->dev;
    rdna3_drm_bo_get(bo);
    rd3_bomap_link(&bo->os_map, m);
    md_made++;
    return m;
}

/* RDNA3BoMemory::taggedRelease (OSObject's freeWhen 1) and free */
static void md_release(md *m)
{
    if (rd3_bomap_release_unlinks(&m->bo->os_map, m, m->refs, 1)) {
        rdna3_drm_bomap_uncharge(m->dev, m->bo);
        rdna3_drm_bo_put(m->dev, m->bo);
        free(m);
        md_freed++;
        return;
    }
    m->refs--;
}

/* IOConnectMapMemory64: 0, or -1 when clientMemoryForType gave nothing */
static int xnu_map(rdna3_drm_file *f, uint64_t off, md **out)
{
    md *m = map_ref(f, off, NULL);
    if (!m)
        return -1;
    if (!m->mapped) {   /* makeMapping: the IOMemoryMap retains the descriptor */
        m->mapped = 1;
        m->refs++;
    }                   /* else: copyCompatible hands the same map back */
    if (out)
        *out = m;
    md_release(m);      /* mapClientMemory64: memory->release() */
    return 0;
}

/* IOConnectUnmapMemory64 with the BO's memory type */
static int xnu_unmap(rdna3_drm_file *f, uint64_t off)
{
    md *m = map_ref(f, off, NULL);
    int r = -1;
    if (!m)
        return -1;
    if (m->mapped) {    /* kIOMapReference found it: the map goes, then its reference */
        m->mapped = 0;
        md_release(m);
        r = 0;
    }
    md_release(m);
    return r;
}

/* the connection's end (the mappings set released): the map goes */
static void conn_close(md *m)
{
    if (m->mapped) {
        m->mapped = 0;
        md_release(m);
    }
}

static uint32_t gem_create(rdna3_drm_file *f, uint64_t size, uint32_t dom, uint64_t flags)
{
    union drm_amdgpu_gem_create c;
    memset(&c, 0, sizeof(c));
    c.in.bo_size = size;
    c.in.alignment = 4096;
    c.in.domains = dom;
    c.in.domain_flags = flags;
    return IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_CREATE), c) == 0 ? c.out.handle : 0;
}

static void gem_close(rdna3_drm_file *f, uint32_t h)
{
    struct drm_gem_close cl;
    memset(&cl, 0, sizeof(cl));
    cl.handle = h;
    CHECK(IOCTL(f, RD_NR_GEM_CLOSE, cl) == 0);
}

static uint64_t bo_off(rdna3_drm_file *f, uint32_t h)
{
    rdna3_bo *bo = rdna3_drm_bo_lookup(f, h);
    return bo ? bo->mmap_offset : 0;
}

static uint64_t bo_vram(rdna3_drm_file *f, uint32_t h)
{
    rdna3_bo *bo = rdna3_drm_bo_lookup(f, h);
    return bo ? bo->vram_offset : UINT64_MAX;
}

static void test_classify(void)
{
    rdna3_bo bo;
    memset(&bo, 0, sizeof(bo));
    CHECK(rd3_bomap_classify(NULL, BAR0, BAR2) == RD3_BOMAP_NO_BO);
    bo.size = 4096;
    bo.placement = AMDGPU_GEM_DOMAIN_VRAM;
    bo.vram_offset = BAR0 - 4096;
    CHECK(rd3_bomap_classify(&bo, BAR0, BAR2) == RD3_BOMAP_VRAM);
    bo.vram_offset = BAR0;
    CHECK(rd3_bomap_classify(&bo, BAR0, BAR2) == RD3_BOMAP_REFUSE_OUTSIDE);
    bo.vram_offset = UINT64_MAX - 100;   /* no wrap-around past the BAR check */
    CHECK(rd3_bomap_classify(&bo, BAR0, BAR2) == RD3_BOMAP_REFUSE_OUTSIDE);
    bo.vram_offset = 0;
    CHECK(rd3_bomap_classify(&bo, 0, BAR2) == RD3_BOMAP_REFUSE_OUTSIDE);   /* BAR0 not mapped */
    bo.flags = AMDGPU_GEM_CREATE_NO_CPU_ACCESS;
    CHECK(rd3_bomap_classify(&bo, BAR0, BAR2) == RD3_BOMAP_REFUSE_NO_CPU);
    bo.flags = 0;
    bo.placement = AMDGPU_GEM_DOMAIN_GDS;
    CHECK(rd3_bomap_classify(&bo, BAR0, BAR2) == RD3_BOMAP_REFUSE_NO_CPU);
    bo.placement = AMDGPU_GEM_DOMAIN_DOORBELL;
    bo.vram_offset = BAR2 - 4096;
    CHECK(rd3_bomap_classify(&bo, BAR0, BAR2) == RD3_BOMAP_DOORBELL);
    bo.vram_offset = BAR2;
    CHECK(rd3_bomap_classify(&bo, BAR0, BAR2) == RD3_BOMAP_REFUSE_OUTSIDE);
    bo.placement = AMDGPU_GEM_DOMAIN_GTT;
    CHECK(rd3_bomap_classify(&bo, BAR0, BAR2) == RD3_BOMAP_REFUSE_NO_BACKING);
    bo.backing = &bo;
    CHECK(rd3_bomap_classify(&bo, BAR0, BAR2) == RD3_BOMAP_GTT);
    bo.userptr = 1;   /* before anything else, whatever the placement */
    CHECK(rd3_bomap_classify(&bo, BAR0, BAR2) == RD3_BOMAP_REFUSE_USERPTR);
    CHECK(rd3_bomap_refused(RD3_BOMAP_REFUSE_USERPTR) && !rd3_bomap_refused(RD3_BOMAP_GTT) &&
          !rd3_bomap_refused(RD3_BOMAP_NO_BO));
    CHECK(rd3_bomap_per_bo(RD3_BOMAP_VRAM) && rd3_bomap_per_bo(RD3_BOMAP_DOORBELL) &&
          !rd3_bomap_per_bo(RD3_BOMAP_GTT));
    for (int k = 0; k < RD3_BOMAP_KINDS; k++)
        CHECK(strcmp(rd3_bomap_kind_name((enum rd3_bomap_kind)k), "?") != 0);
}

static void test_release_rule(void)
{
    int a, b;
    void *link = &a;
    /* not the last reference: nothing changes, the caller decrements under the lock */
    CHECK(rd3_bomap_release_unlinks(&link, &a, 2, 1) == 0 && link == &a);
    /* the last: the link goes first */
    CHECK(rd3_bomap_release_unlinks(&link, &a, 1, 1) == 1 && link == NULL);
    /* a descriptor the BO no longer links to (a newer one took its place) leaves it alone */
    link = &b;
    CHECK(rd3_bomap_release_unlinks(&link, &a, 1, 1) == 1 && link == &b);
    /* IOMemoryMap-style freeWhen 2 */
    CHECK(rd3_bomap_release_unlinks(&link, &b, 3, 2) == 0 && link == &b);
    CHECK(rd3_bomap_release_unlinks(&link, &b, 2, 2) == 1 && link == NULL);
    rd3_bomap_link(&link, &a);
    rd3_bomap_link(&link, &b);   /* never replaces a live link */
    CHECK(rd3_bomap_lookup(&link) == &a);
}

static void test_quota_rule(void)
{
    const uint64_t max = RD3_BOMAP_OWNER_BYTES_MAX(BAR0);
    CHECK(rd3_bomap_quota_admit(501, 0, 0, 0, 4096, max) == RD3_BOMAP_Q_OK);
    CHECK(rd3_bomap_quota_admit(501, RD_MAX_HANDLES - 1, 0, 0, 4096, max) == RD3_BOMAP_Q_OK);
    CHECK(rd3_bomap_quota_admit(501, RD_MAX_HANDLES, 0, 0, 4096, max) == RD3_BOMAP_Q_OWNER_COUNT);
    CHECK(rd3_bomap_quota_admit(501, 1, max - 4096, 0, 4096, max) == RD3_BOMAP_Q_OK);
    CHECK(rd3_bomap_quota_admit(501, 1, max - 4095, 0, 4096, max) == RD3_BOMAP_Q_OWNER_BYTES);
    CHECK(rd3_bomap_quota_admit(501, 0, 0, 0, UINT64_MAX, max) == RD3_BOMAP_Q_OWNER_BYTES);   /* no wrap */
    /* root and _windowserver meet only the device's limit, as rdna3_vmid_admit */
    CHECK(rd3_bomap_quota_admit(0, RD_MAX_HANDLES * 2, max * 2, 0, 4096, max) == RD3_BOMAP_Q_OK);
    CHECK(rd3_bomap_quota_admit(RD_UID_WINDOWSERVER, RD_MAX_HANDLES * 2, 0, 0, 4096, max) == RD3_BOMAP_Q_OK);
    CHECK(rd3_bomap_quota_admit(0, 0, 0, RD3_BOMAP_DEVICE_MAX, 4096, max) == RD3_BOMAP_Q_DEVICE_COUNT);
    CHECK(rd3_bomap_quota_admit(501, 0, 0, RD3_BOMAP_DEVICE_MAX, 4096, max) == RD3_BOMAP_Q_DEVICE_COUNT);
    for (int q = RD3_BOMAP_Q_OK; q <= RD3_BOMAP_Q_TABLE_FULL; q++)
        CHECK(strcmp(rd3_bomap_quota_name(q), "?") != 0);
}

int main(void)
{
    static rdna3_drm_device dev;
    static rdna3_drm_file f1, f2;
    rdna3_drm_ops ops;
    amd_discovery disc;

    test_classify();
    test_release_rule();
    test_quota_rule();

    memset(&ops, 0, sizeof(ops));
    ops.zalloc = h_zalloc;
    ops.free = h_free;
    ops.bo_alloc = h_bo_alloc;
    ops.bo_free = h_bo_free;
    ops.bo_userptr = h_bo_userptr;
    ops.surface_bo = h_surface_bo;
    ops.copyin = h_copyin;
    ops.copyout = h_copyout;
    memset(&disc, 0, sizeof(disc));
    disc.has_gc_info = 1;
    disc.gc.num_se = 3; disc.gc.num_sa_per_se = 2; disc.gc.num_wgp0_per_sa = 5;
    disc.gc.num_rb_per_se = 4; disc.gc.wave_size = 32; disc.gc.num_gl2c = 16;
    disc.gc.gl2c_per_gpu = 4u << 20;
    disc.num_ips = 1;
    disc.ips[0].hw_id = AMD_HWID_GC; disc.ips[0].major = 11; disc.ips[0].minor = 0;
    disc.ips[0].revision = 3;
    range_alloc_init(&heap, 1u << 20, 96u << 20);   /* below the surface at 128 MiB */
    rdna3_drm_device_init(&dev, &ops, &disc, 0x747E, 0xC8, 0x0, 16ull << 30, BAR0, 8ull << 30);
    CHECK(rdna3_drm_file_open(&dev, &f1) == 0);
    f1.os = (void *)0x7a5c;

    /* 1. GEM_CLOSE while mapped: the range stays the BO's until the unmap */
    {
        uint32_t h = gem_create(&f1, 64 << 10, AMDGPU_GEM_DOMAIN_VRAM, 0), h2, h3;
        uint64_t off = bo_off(&f1, h), vr = bo_vram(&f1, h), free0;
        md *m = NULL, *m2 = NULL;
        CHECK(h && off >= (1ull << 32) && vr != UINT64_MAX);
        CHECK(xnu_map(&f1, off, &m) == 0 && m && m->refs == 1 && m->bo->os_map == m && md_made == 1);
        CHECK(xnu_map(&f1, off, &m2) == 0 && m2 == m && m->refs == 1 && md_made == 1);   /* same map */
        free0 = range_alloc_free_bytes(&heap);
        gem_close(&f1, h);
        CHECK(live_bos == 1 && range_alloc_free_bytes(&heap) == free0);   /* BO and range kept */
        CHECK(md_freed == 0 && m->bo->refcnt == 1);
        h2 = gem_create(&f1, 64 << 10, AMDGPU_GEM_DOMAIN_VRAM, 0);
        CHECK(h2 && bo_vram(&f1, h2) != vr);   /* the mapped range is not handed out */
        /* the unmap needs the BO's offset in this file: closed, it finds nothing (as the kext) */
        CHECK(xnu_unmap(&f1, off) == -1 && md_freed == 0);
        conn_close(m);   /* the connection's end takes the map, and the BO with it */
        CHECK(md_freed == 1 && live_bos == 1);
        h3 = gem_create(&f1, 64 << 10, AMDGPU_GEM_DOMAIN_VRAM, 0);
        CHECK(h3 && bo_vram(&f1, h3) == vr);   /* the range came back */
        gem_close(&f1, h2);
        gem_close(&f1, h3);
        CHECK(live_bos == 0);
    }

    /* 2. the amdgpu order (unmap, then GEM_CLOSE): the unmap gives the BO back to its handle */
    {
        uint32_t h = gem_create(&f1, 8192, AMDGPU_GEM_DOMAIN_VRAM, 0);
        uint64_t off = bo_off(&f1, h);
        md *m = NULL;
        int made = md_made, freed = md_freed;
        CHECK(xnu_map(&f1, off, &m) == 0 && m->bo->refcnt == 2);
        CHECK(xnu_unmap(&f1, off) == 0 && md_freed == freed + 1);
        CHECK(rdna3_drm_bo_lookup(&f1, h)->refcnt == 1 && rdna3_drm_bo_lookup(&f1, h)->os_map == NULL);
        CHECK(xnu_unmap(&f1, off) == -1);   /* nothing mapped any more */
        CHECK(xnu_map(&f1, off, &m) == 0 && md_made == made + 3);   /* new ones: the failed unmap made and dropped one */
        CHECK(xnu_unmap(&f1, off) == 0);
        gem_close(&f1, h);
        CHECK(live_bos == 0);
    }

    /* 3. the file closes while mapped (clientClose; the mappings outlive it on the port) */
    {
        uint32_t h;
        uint64_t off, vr;
        md *m = NULL;
        CHECK(rdna3_drm_file_open(&dev, &f2) == 0);
        f2.os = (void *)0x7a5d;
        h = gem_create(&f2, 128 << 10, AMDGPU_GEM_DOMAIN_VRAM, 0);
        off = bo_off(&f2, h);
        vr = bo_vram(&f2, h);
        CHECK(xnu_map(&f2, off, &m) == 0);
        rdna3_drm_file_close(&f2);
        CHECK(live_bos == 1 && m->bo->refcnt == 1);
        {
            uint32_t h2 = gem_create(&f1, 128 << 10, AMDGPU_GEM_DOMAIN_VRAM, 0);
            CHECK(h2 && bo_vram(&f1, h2) != vr);
            gem_close(&f1, h2);
        }
        conn_close(m);
        CHECK(live_bos == 0);
        h = gem_create(&f1, 128 << 10, AMDGPU_GEM_DOMAIN_VRAM, 0);
        CHECK(bo_vram(&f1, h) == vr);
        gem_close(&f1, h);
    }

    /* 4. a BO shared with another file (export / import): each file's map holds it */
    {
        struct rd_xnu_bo_share x;
        uint32_t h = gem_create(&f1, 4096, AMDGPU_GEM_DOMAIN_VRAM, 0);
        md *m1 = NULL;
        CHECK(rdna3_drm_file_open(&dev, &f2) == 0);
        f2.os = (void *)0x7a5d;
        memset(&x, 0, sizeof(x));
        x.handle = h;
        CHECK(IOCTL(&f1, RD_NR_XNU_BO_EXPORT, x) == 0 && x.token);
        x.handle = 0;
        CHECK(IOCTL(&f2, RD_NR_XNU_BO_IMPORT, x) == 0 && x.handle);
        CHECK(xnu_map(&f1, bo_off(&f1, h), &m1) == 0);
        gem_close(&f1, h);
        rdna3_drm_file_close(&f2);
        /* no handle left: the export entry (until the exporter's file goes) and the mapping */
        CHECK(live_bos == 1 && m1->bo->refcnt == 2);
        {
            rdna3_bo *bo = m1->bo;
            conn_close(m1);
            CHECK(live_bos == 1 && bo->refcnt == 1 && bo->os_map == NULL);
        }
    }

    /* 5. doorbell pages: the mapped page is not handed to the next doorbell BO */
    {
        uint32_t h, h2, h3;
        uint64_t off, page;
        md *m = NULL;
        rdna3_drm_device_set_userq(&dev, 7, BAR2);
        h = gem_create(&f1, 4096, AMDGPU_GEM_DOMAIN_DOORBELL, 0);
        off = bo_off(&f1, h);
        page = bo_vram(&f1, h);
        CHECK(h && xnu_map(&f1, off, &m) == 0 && m->bo->placement == AMDGPU_GEM_DOMAIN_DOORBELL);
        gem_close(&f1, h);
        h2 = gem_create(&f1, 4096, AMDGPU_GEM_DOMAIN_DOORBELL, 0);
        CHECK(h2 && bo_vram(&f1, h2) != page);
        conn_close(m);
        h3 = gem_create(&f1, 4096, AMDGPU_GEM_DOMAIN_DOORBELL, 0);
        CHECK(h3 && bo_vram(&f1, h3) == page);   /* back after the unmap */
        gem_close(&f1, h2);
        gem_close(&f1, h3);
        rdna3_drm_device_set_userq(&dev, 0, 0);
    }

    /* 6. refusals (amdgpu_gem_object_mmap) and offsets naming nothing */
    {
        static uint8_t pages[2 * 4096] __attribute__((aligned(4096)));
        struct drm_amdgpu_gem_userptr u;
        uint32_t hn = gem_create(&f1, 4096, AMDGPU_GEM_DOMAIN_VRAM, AMDGPU_GEM_CREATE_NO_CPU_ACCESS);
        uint32_t hg = gem_create(&f1, 4096, AMDGPU_GEM_DOMAIN_GTT, 0);
        enum rd3_bomap_kind k;
        int made = md_made;
        CHECK(hn && !map_ref(&f1, bo_off(&f1, hn), &k) && k == RD3_BOMAP_REFUSE_NO_CPU);
        CHECK(hg && !map_ref(&f1, bo_off(&f1, hg), &k) && k == RD3_BOMAP_GTT);   /* the GTT path, no descriptor */
        memset(&u, 0, sizeof(u));
        u.addr = (uint64_t)(uintptr_t)pages;
        u.size = sizeof(pages);
        u.flags = AMDGPU_GEM_USERPTR_REGISTER;
        CHECK(IOCTL(&f1, RD_NR_AMDGPU(DRM_AMDGPU_GEM_USERPTR), u) == 0 && u.handle);
        /* a userptr BO has offset 0: the old libdrm_xnu's unmap (memory type 0) found it */
        CHECK(!map_ref(&f1, 0, &k) && k == RD3_BOMAP_REFUSE_USERPTR);
        CHECK(!map_ref(&f1, 1ull << 40, &k) && k == RD3_BOMAP_NO_BO);
        CHECK(md_made == made);
        {
            struct drm_gem_close cl = { u.handle, 0 };
            CHECK(IOCTL(&f1, RD_NR_GEM_CLOSE, cl) == 0);
        }
        CHECK(!map_ref(&f1, 0, &k) && k == RD3_BOMAP_NO_BO);
        gem_close(&f1, hn);
        gem_close(&f1, hg);
    }

    /* 7. the uid's quota (ECC review round 1): a map + GEM_CLOSE loop stops at the
     * uid's limit, a reopened file does not reset it, another uid is not hit */
    {
        static md *held[RD_MAX_HANDLES + 1];
        static rdna3_drm_file f3;
        uint32_t n = 0, h, i, live0 = live_bos;   /* test 4's export lives until f1 closes */
        CHECK(rdna3_drm_file_open(&dev, &f2) == 0);
        f2.os = (void *)0x7a5d;
        f2.owner = 501;
        for (;;) {
            md *m = NULL;
            h = gem_create(&f2, 4096, AMDGPU_GEM_DOMAIN_VRAM, 0);
            if (!h || xnu_map(&f2, bo_off(&f2, h), &m) != 0) {
                if (h)
                    gem_close(&f2, h);
                break;
            }
            held[n++] = m;
            gem_close(&f2, h);
            if (n > RD_MAX_HANDLES)
                break;
        }
        CHECK(n == RD_MAX_HANDLES && last_quota == RD3_BOMAP_Q_OWNER_COUNT);
        CHECK(dev.bomap.n == RD_MAX_HANDLES && dev.bomap.bytes == (uint64_t)RD_MAX_HANDLES * 4096);
        rdna3_drm_file_close(&f2);   /* the mappings live on (the connection port) */
        CHECK(rdna3_drm_file_open(&dev, &f2) == 0);
        f2.owner = 501;
        h = gem_create(&f2, 4096, AMDGPU_GEM_DOMAIN_VRAM, 0);
        CHECK(h && xnu_map(&f2, bo_off(&f2, h), NULL) == -1 && last_quota == RD3_BOMAP_Q_OWNER_COUNT);
        CHECK(rdna3_drm_file_open(&dev, &f3) == 0);   /* another uid */
        f3.owner = 502;
        {
            uint32_t h3 = gem_create(&f3, 4096, AMDGPU_GEM_DOMAIN_VRAM, 0);
            md *m3 = NULL;
            CHECK(h3 && xnu_map(&f3, bo_off(&f3, h3), &m3) == 0 && m3 && m3->bo->map_owner == 502);
            CHECK(xnu_unmap(&f3, bo_off(&f3, h3)) == 0);
            gem_close(&f3, h3);
        }
        rdna3_drm_file_close(&f3);
        for (i = 0; i < n; i++)
            conn_close(held[i]);
        CHECK(dev.bomap.n == 0 && dev.bomap.bytes == 0);
        CHECK(xnu_map(&f2, bo_off(&f2, h), NULL) == 0);   /* room again */
        CHECK(xnu_unmap(&f2, bo_off(&f2, h)) == 0);
        gem_close(&f2, h);
        rdna3_drm_file_close(&f2);
        CHECK(live_bos == live0);
        for (i = 0; i < RD_BOMAP_OWNERS; i++)
            CHECK(!dev.bomap.o[i].used);
    }

    /* 8. the surface quota counts what mappings hold: import, map, GEM_CLOSE, and
     * the next import (100 + 100 MiB > 128 MiB) is refused, in a reopened file too */
    {
        struct rd_xnu_surface_bo s;
        md *m = NULL;
        CHECK(rdna3_drm_file_open(&dev, &f2) == 0);
        f2.os = (void *)0x7a5d;
        f2.owner = 501;
        memset(&s, 0, sizeof(s));
        s.op = RD_XNU_SURFACE_BO_IMPORT;
        s.surface_id = 7;
        CHECK(IOCTL(&f2, RD_NR_XNU_SURFACE_BO, s) == 0 && s.handle);
        CHECK(xnu_map(&f2, bo_off(&f2, s.handle), &m) == 0 && m->bo->foreign);
        gem_close(&f2, s.handle);
        s.handle = 0;
        s.size = 0;
        CHECK(IOCTL(&f2, RD_NR_XNU_SURFACE_BO, s) == -RD_ENOSPC);
        rdna3_drm_file_close(&f2);
        CHECK(rdna3_drm_file_open(&dev, &f2) == 0);
        f2.owner = 501;
        CHECK(IOCTL(&f2, RD_NR_XNU_SURFACE_BO, s) == -RD_ENOSPC);   /* reopen: still pinned */
        conn_close(m);
        CHECK(surf_holds == 0);
        CHECK(IOCTL(&f2, RD_NR_XNU_SURFACE_BO, s) == 0 && s.handle);   /* the mapping went */
        {
            /* mapped while its handle is open: counted once, not twice */
            struct rd_xnu_surface_bo s2 = s;
            CHECK(xnu_map(&f2, bo_off(&f2, s.handle), &m) == 0);
            s2.handle = 0;
            CHECK(IOCTL(&f2, RD_NR_XNU_SURFACE_BO, s2) == -RD_ENOSPC);   /* 200 > 128 either way */
            CHECK(xnu_unmap(&f2, bo_off(&f2, s.handle)) == 0);
        }
        gem_close(&f2, s.handle);
        rdna3_drm_file_close(&f2);
        CHECK(surf_holds == 0);
    }

    rdna3_drm_file_close(&f1);
    rdna3_drm_device_fini(&dev);
    CHECK(live_bos == 0 && md_made == md_freed);
    if (failures) {
        fprintf(stderr, "test_bo_map: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_bo_map: ok\n");
    return 0;
}
