/*
 * Host tests for uapi/rdna3_drm.c: drive the DRM/amdgpu ioctls the way
 * libdrm_amdgpu and RADV do during device enumeration.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rdna3_drm.h"

static int failures;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
    failures++; } } while (0)

static unsigned live_bos;
static void *h_zalloc(void *c, size_t n) { (void)c; return calloc(1, n); }
static void h_free(void *c, void *p, size_t n) { (void)c; (void)n; free(p); }
static int h_bo_alloc(void *c, rdna3_bo *bo)
{
    (void)c;
    if (bo->placement == AMDGPU_GEM_DOMAIN_VRAM && bo->size > (64u << 20))
        return -RD_ENOMEM; /* force GTT fallback for big VRAM BOs */
    bo->backing = calloc(1, (size_t)bo->size);
    if (!bo->backing)
        return -RD_ENOMEM;
    live_bos++;
    return 0;
}
static void h_bo_free(void *c, rdna3_bo *bo) { (void)c; free(bo->backing); live_bos--; }
static int h_copyin(void *c, void *d, uint64_t u, size_t n) { (void)c; memcpy(d, (void *)(uintptr_t)u, n); return 0; }
static int h_copyout(void *c, uint64_t u, const void *s, size_t n) { (void)c; memcpy((void *)(uintptr_t)u, s, n); return 0; }

/* fake page tables: count what the uapi asks for */
static int vm_live, vm_maps, vm_unmaps;
static uint64_t vm_mapped;
static int vm_dummy;
static void *h_vm_create(void *c) { (void)c; vm_live++; return &vm_dummy; }
static uint32_t h_query_flags;
static uint32_t h_vm_query(void *c, void *vm) { (void)c; (void)vm; return h_query_flags; }
static void h_vm_destroy(void *c, void *vm) { (void)c; (void)vm; vm_live--; }
static int h_vm_map(void *c, void *vm, rdna3_bo *bo, uint64_t va, uint64_t off, uint64_t size,
                    uint32_t flags)
{
    (void)c; (void)vm; (void)bo; (void)va; (void)off; (void)flags;
    if (va == 0xBAD000)
        return -RD_ENOMEM;
    vm_maps++;
    vm_mapped += size;
    return 0;
}
static void h_vm_unmap(void *c, void *vm, uint64_t va, uint64_t size)
{
    (void)c; (void)vm; (void)va;
    vm_unmaps++;
    vm_mapped -= size;
}

/* fake ring: submissions complete when fence_wait is called or when
 * 'ring_done' is advanced by hand */
static uint64_t ring_next, ring_done;
static uint32_t last_ip, last_ring, last_nibs;
static rdna3_ib last_ibs[RD_MAX_CS_IBS];
static void *last_vm;
static int h_submit(void *c, void *vm, uint32_t ip, uint32_t ring, const rdna3_ib *ibs,
                    uint32_t n, uint64_t *seq)
{
    (void)c;
    last_vm = vm; last_ip = ip; last_ring = ring; last_nibs = n;
    memcpy(last_ibs, ibs, sizeof(*ibs) * n);
    *seq = ++ring_next;
    return 0;
}
static int h_signaled(void *c, uint32_t ip, uint32_t ring, uint64_t seq)
{
    (void)c; (void)ip; (void)ring;
    return seq <= ring_done;
}
static int h_wait(void *c, uint32_t ip, uint32_t ring, uint64_t seq, uint64_t abs_ns)
{
    (void)c; (void)ip; (void)ring;
    if (abs_ns == 1)          /* "already expired" */
        return -RD_ETIME;
    if (seq > ring_done)
        ring_done = seq;
    return 0;
}

static uint64_t chunk_ptrs[8];
static struct drm_amdgpu_cs_chunk chunks[8];
static int cs_submit(rdna3_drm_file *f, uint64_t va, uint32_t bytes, uint32_t ip, uint32_t in_sobj,
                     uint32_t out_sobj, uint64_t *handle)
{
    static struct drm_amdgpu_cs_chunk_ib ib;
    static struct drm_amdgpu_cs_chunk_sem in, out;
    union drm_amdgpu_cs cs;
    uint32_t n = 0;
    int r;
    memset(&ib, 0, sizeof(ib));
    ib.va_start = va; ib.ib_bytes = bytes; ib.ip_type = ip;
    chunks[n].chunk_id = AMDGPU_CHUNK_ID_IB; chunks[n].length_dw = sizeof(ib) / 4;
    chunks[n].chunk_data = (uint64_t)(uintptr_t)&ib; n++;
    if (in_sobj) {
        in.handle = in_sobj;
        chunks[n].chunk_id = AMDGPU_CHUNK_ID_SYNCOBJ_IN; chunks[n].length_dw = 1;
        chunks[n].chunk_data = (uint64_t)(uintptr_t)&in; n++;
    }
    if (out_sobj) {
        out.handle = out_sobj;
        chunks[n].chunk_id = AMDGPU_CHUNK_ID_SYNCOBJ_OUT; chunks[n].length_dw = 1;
        chunks[n].chunk_data = (uint64_t)(uintptr_t)&out; n++;
    }
    for (uint32_t i = 0; i < n; i++)
        chunk_ptrs[i] = (uint64_t)(uintptr_t)&chunks[i];
    memset(&cs, 0, sizeof(cs));
    cs.in.num_chunks = n;
    cs.in.chunks = (uint64_t)(uintptr_t)chunk_ptrs;
    r = rdna3_drm_ioctl(f, RD_NR_AMDGPU(DRM_AMDGPU_CS), &cs, sizeof(cs));
    if (!r && handle)
        *handle = cs.out.handle;
    return r;
}

#define IOCTL(f, nr, arg) rdna3_drm_ioctl((f), (nr), &(arg), sizeof(arg))

static int info(rdna3_drm_file *f, uint32_t query, void *out, uint32_t size)
{
    struct drm_amdgpu_info in;
    memset(&in, 0, sizeof(in));
    in.return_pointer = (uint64_t)(uintptr_t)out;
    in.return_size = size;
    in.query = query;
    return IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in);
}

int main(void)
{
    static rdna3_drm_device dev;
    static rdna3_drm_file file;
    rdna3_drm_ops ops = { NULL, h_zalloc, h_free, h_bo_alloc, h_bo_free, h_copyin, h_copyout, NULL, NULL,
                          h_vm_create, h_vm_destroy, h_vm_map, h_vm_unmap, NULL, NULL, NULL, NULL };
    amd_discovery disc;
    rdna3_drm_file *f = &file;

    memset(&disc, 0, sizeof(disc));
    disc.has_gc_info = 1;
    disc.gc.num_se = 3; disc.gc.num_sa_per_se = 2; disc.gc.num_wgp0_per_sa = 5;
    disc.gc.num_rb_per_se = 4; disc.gc.wave_size = 32; disc.gc.num_gl2c = 16;
    disc.gc.gl2c_per_gpu = 4u << 20;
    disc.num_ips = 1;
    disc.ips[0].hw_id = AMD_HWID_GC; disc.ips[0].major = 11; disc.ips[0].minor = 0;
    disc.ips[0].revision = 3;

    rdna3_drm_device_init(&dev, &ops, &disc, 0x747E, 0xC8, 0x0, 16ull << 30, 256ull << 20, 8ull << 30);
    CHECK(rdna3_drm_file_open(&dev, f) == 0);
    CHECK(vm_live == 1);

    /* drmGetVersion */
    {
        struct drm_version v;
        char name[16] = {0}, desc[64] = {0}, date[16] = {0};
        memset(&v, 0, sizeof(v));
        CHECK(IOCTL(f, RD_NR_VERSION, v) == 0);   /* lengths first */
        CHECK(v.name_len == 6 && v.version_major == 3 && v.version_minor >= 49);
        v.name = name; v.desc = desc; v.date = date;
        v.desc_len = sizeof(desc); v.date_len = sizeof(date);
        CHECK(IOCTL(f, RD_NR_VERSION, v) == 0);
        CHECK(strcmp(name, "amdgpu") == 0);
    }
    /* caps */
    {
        struct drm_get_cap c = { DRM_CAP_SYNCOBJ_TIMELINE, 0 };
        CHECK(IOCTL(f, RD_NR_GET_CAP, c) == 0 && c.value == 1);
    }
    /* amdgpu_query_info(DEV_INFO) */
    {
        struct drm_amdgpu_info_device di;
        uint32_t aw = 7;
        memset(&di, 0, sizeof(di));
        CHECK(info(f, AMDGPU_INFO_DEV_INFO, &di, sizeof(di)) == 0);
        CHECK(di.device_id == 0x747E && di.family == AMDGPU_FAMILY_GC_11_0_0);
        CHECK(di.external_rev == 0x20);
        CHECK(di.num_shader_engines == 3 && di.cu_active_number == 60);
        CHECK(di.num_rb_pipes == 12 && di.wave_front_size == 32);
        CHECK(di.virtual_address_max == 0x0000800000000000ull);
        CHECK(info(f, AMDGPU_INFO_ACCEL_WORKING, &aw, 4) == 0 && aw == 0);
    }
    /* hw ip */
    {
        struct drm_amdgpu_info in;
        struct drm_amdgpu_info_hw_ip ip;
        memset(&in, 0, sizeof(in));
        memset(&ip, 0xff, sizeof(ip));
        in.return_pointer = (uint64_t)(uintptr_t)&ip;
        in.return_size = sizeof(ip);
        in.query = AMDGPU_INFO_HW_IP_INFO;
        in.query_hw_ip.type = AMDGPU_HW_IP_GFX;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0);
        CHECK(ip.hw_ip_version_major == 11 && ip.available_rings == 0);
    }
    /* BO lifecycle like amdgpu_bo_alloc + cpu_map + va_op */
    {
        union drm_amdgpu_gem_create c;
        union drm_amdgpu_gem_mmap m;
        struct drm_amdgpu_gem_va va;
        struct drm_gem_close cl;
        uint64_t vram = 0;
        uint32_t h, h2;

        memset(&c, 0, sizeof(c));
        c.in.bo_size = 1 << 20;
        c.in.alignment = 4096;
        c.in.domains = AMDGPU_GEM_DOMAIN_VRAM;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_CREATE), c) == 0);
        h = c.out.handle;
        CHECK(h == 1);
        CHECK(info(f, AMDGPU_INFO_VRAM_USAGE, &vram, 8) == 0 && vram == (1 << 20));

        memset(&c, 0, sizeof(c));
        c.in.bo_size = 128u << 20;
        c.in.domains = AMDGPU_GEM_DOMAIN_VRAM | AMDGPU_GEM_DOMAIN_GTT;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_CREATE), c) == 0);
        h2 = c.out.handle;
        CHECK(rdna3_drm_bo_lookup(f, h2)->placement == AMDGPU_GEM_DOMAIN_GTT);

        memset(&m, 0, sizeof(m));
        m.in.handle = h;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_MMAP), m) == 0);
        CHECK(rdna3_drm_bo_by_mmap_offset(f, m.out.addr_ptr) == rdna3_drm_bo_lookup(f, h));

        memset(&va, 0, sizeof(va));
        va.handle = h; va.operation = AMDGPU_VA_OP_MAP; va.va_address = 0x100000;
        va.map_size = 1 << 20;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == 0);
        va.map_size = 2 << 20;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == -RD_EINVAL);
        va.operation = AMDGPU_VA_OP_UNMAP; va.map_size = 1 << 20;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == 0);
        CHECK(vm_maps == 1 && vm_unmaps == 1 && vm_mapped == 0);
        /* a failing page-table update fails the ioctl and records nothing */
        va.operation = AMDGPU_VA_OP_MAP; va.va_address = 0xBAD000; va.map_size = 4096;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == -RD_ENOMEM);
        /* unaligned offset */
        va.va_address = 0x300000; va.offset_in_bo = 0x800;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == -RD_EINVAL);
        va.offset_in_bo = 0;
        /* REPLACE drops what it overlaps */
        va.va_address = 0x200000; va.map_size = 2 * 4096;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == 0);
        va.operation = AMDGPU_VA_OP_REPLACE; va.va_address = 0x201000; va.map_size = 4096;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == 0);
        CHECK(vm_maps == 3 && vm_unmaps == 2 && vm_mapped == 4096);
        /* closing the handle drops its mappings, like amdgpu_gem_object_close */

        cl.handle = h; cl.pad = 0;
        CHECK(IOCTL(f, RD_NR_GEM_CLOSE, cl) == 0);
        CHECK(IOCTL(f, RD_NR_GEM_CLOSE, cl) == -RD_EINVAL);
        CHECK(vm_mapped == 0 && vm_unmaps == 3);
        va.operation = AMDGPU_VA_OP_CLEAR; va.va_address = 0x200000; va.map_size = 1 << 20;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == 0);
        CHECK(vm_unmaps == 3);
        CHECK(info(f, AMDGPU_INFO_VRAM_USAGE, &vram, 8) == 0 && vram == 0);
        CHECK(live_bos == 1);
    }
    /* contexts */
    {
        union drm_amdgpu_ctx c;
        uint32_t id;
        memset(&c, 0, sizeof(c));
        c.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CTX), c) == 0);
        id = c.out.alloc.ctx_id;
        CHECK(id != 0);
        memset(&c, 0, sizeof(c));
        c.in.op = AMDGPU_CTX_OP_FREE_CTX; c.in.ctx_id = id;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CTX), c) == 0);
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CTX), c) == -RD_EINVAL);
        /* reset state comes from the OS through vm_query (amdgpu_ctx_query2) */
        memset(&c, 0, sizeof(c));
        c.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CTX), c) == 0);
        id = c.out.alloc.ctx_id;
        dev.ops.vm_query = h_vm_query;
        h_query_flags = AMDGPU_CTX_QUERY2_FLAGS_RESET | AMDGPU_CTX_QUERY2_FLAGS_VRAMLOST;
        memset(&c, 0, sizeof(c));
        c.in.op = AMDGPU_CTX_OP_QUERY_STATE2; c.in.ctx_id = id;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CTX), c) == 0 && c.out.state.flags == h_query_flags);
        memset(&c, 0, sizeof(c));
        c.in.op = AMDGPU_CTX_OP_QUERY_STATE; c.in.ctx_id = id;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CTX), c) == 0 &&
              c.out.state.reset_status == AMDGPU_CTX_INNOCENT_RESET);
        h_query_flags |= AMDGPU_CTX_QUERY2_FLAGS_GUILTY;
        memset(&c, 0, sizeof(c));
        c.in.op = AMDGPU_CTX_OP_QUERY_STATE; c.in.ctx_id = id;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CTX), c) == 0 &&
              c.out.state.reset_status == AMDGPU_CTX_GUILTY_RESET);
        h_query_flags = 0;
        dev.ops.vm_query = NULL;
        memset(&c, 0, sizeof(c));
        c.in.op = AMDGPU_CTX_OP_FREE_CTX; c.in.ctx_id = id;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CTX), c) == 0);
    }
    /* syncobj */
    {
        struct drm_syncobj_create sc = { 0, 0 };
        struct drm_syncobj_array arr;
        struct drm_syncobj_wait w;
        uint32_t h;
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_CREATE, sc) == 0);
        h = sc.handle;
        memset(&w, 0, sizeof(w));
        w.handles = (uint64_t)(uintptr_t)&h; w.count_handles = 1;
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_WAIT, w) == -RD_ETIME);
        memset(&arr, 0, sizeof(arr));
        arr.handles = (uint64_t)(uintptr_t)&h; arr.count_handles = 1;
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_SIGNAL, arr) == 0);
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_WAIT, w) == 0);
    }
    /* CS is honest about missing rings */
    {
        union drm_amdgpu_cs cs;
        memset(&cs, 0, sizeof(cs));
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CS), cs) == -RD_ENODEV);   /* no submit op */
    }
    /* real CS on the DMA ring through fake ring ops */
    {
        union drm_amdgpu_wait_cs w;
        union drm_amdgpu_fence_to_handle fth;
        struct drm_syncobj_create sc;
        struct drm_syncobj_wait sw;
        uint32_t so, so2;
        uint64_t h1 = 0, h2 = 0, pt = 0;
        struct drm_syncobj_timeline_array q;

        dev.ops.submit = h_submit;
        dev.ops.fence_signaled = h_signaled;
        dev.ops.fence_wait = h_wait;
        rdna3_drm_device_set_ring_ready(&dev, AMDGPU_HW_IP_DMA, 1);

        CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_GFX, 0, 0, NULL) == -RD_EINVAL); /* no GFX ring */
        /* a guilty context (hung or faulted) never submits again */
        dev.ops.vm_query = h_vm_query;
        h_query_flags = AMDGPU_CTX_QUERY2_FLAGS_GUILTY | AMDGPU_CTX_QUERY2_FLAGS_RESET;
        CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == -RD_ECANCELED);
        h_query_flags = AMDGPU_CTX_QUERY2_FLAGS_RESET;
        dev.ops.vm_query = NULL;
        CHECK(cs_submit(f, 0x400000, 6, AMDGPU_HW_IP_DMA, 0, 0, NULL) == -RD_EINVAL);   /* not dwords */
        memset(&sc, 0, sizeof(sc));
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_CREATE, sc) == 0);
        so = sc.handle;
        CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, so, &h1) == 0);
        CHECK(h1 == 1 && last_ip == AMDGPU_HW_IP_DMA && last_nibs == 1 &&
              last_ibs[0].va == 0x400000 && last_ibs[0].length_dw == 16 && last_vm == &vm_dummy);
        /* busy until the ring passes it */
        memset(&w, 0, sizeof(w));
        w.in.handle = h1; w.in.ip_type = AMDGPU_HW_IP_DMA; w.in.timeout = 1;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_WAIT_CS), w) == 0 && w.out.status == 1);
        memset(&q, 0, sizeof(q));
        q.handles = (uint64_t)(uintptr_t)&so; q.points = (uint64_t)(uintptr_t)&pt; q.count_handles = 1;
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_QUERY, q) == 0 && pt == 0);
        /* waiting on the out syncobj waits for the fence */
        memset(&sw, 0, sizeof(sw));
        sw.handles = (uint64_t)(uintptr_t)&so; sw.count_handles = 1; sw.timeout_nsec = INT64_MAX;
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_WAIT, sw) == 0 && ring_done == 1);
        w.in.timeout = 1;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_WAIT_CS), w) == 0 && w.out.status == 0);
        /* in-syncobj with a pending fence is waited before submitting */
        CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, so, &h2) == 0 && h2 == 2);
        CHECK(cs_submit(f, 0x500000, 32, AMDGPU_HW_IP_DMA, so, 0, NULL) == 0 && ring_done == 2);
        /* fence -> syncobj */
        memset(&fth, 0, sizeof(fth));
        fth.in.fence.ip_type = AMDGPU_HW_IP_DMA; fth.in.fence.seq_no = 3;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_FENCE_TO_HANDLE), fth) == 0);
        so2 = fth.out.handle;
        sw.handles = (uint64_t)(uintptr_t)&so2; sw.timeout_nsec = 0;
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_WAIT, sw) == -RD_ETIME);   /* poll only */
        sw.timeout_nsec = INT64_MAX;
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_WAIT, sw) == 0 && ring_done == 3);
        /* WAIT_CS on ~0 = latest */
        w.in.handle = ~0ull; w.in.timeout = 1;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_WAIT_CS), w) == 0 && w.out.status == 0);
        dev.ops.submit = NULL;
        dev.available_rings[AMDGPU_HW_IP_DMA] = 0;
        dev.ready_rings[AMDGPU_HW_IP_DMA] = 0;
    }
    /* short payloads are rejected */
    {
        uint32_t tiny = 0;
        CHECK(rdna3_drm_ioctl(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), &tiny, 4) == -RD_EINVAL);
    }

    rdna3_drm_file_close(f);
    CHECK(live_bos == 0 && vm_live == 0);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_uapi: all checks passed\n");
    return 0;
}
