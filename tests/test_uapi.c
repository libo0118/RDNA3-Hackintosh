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
#include "amd_fw.h"

static int failures;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
    failures++; } } while (0)

static unsigned live_bos;
static void count_bo(void *ctx, rdna3_bo *bo) { (void)bo; (*(uint32_t *)ctx)++; }
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
static void h_bo_free(void *c, rdna3_bo *bo)
{
    (void)c;
    if (bo->foreign && bo->backing)
        (*(int *)bo->backing)--;
    else if (!bo->userptr)
        free(bo->backing);
    live_bos--;
}
static int h_copyin(void *c, void *d, uint64_t u, size_t n) { (void)c; memcpy(d, (void *)(uintptr_t)u, n); return 0; }
static int h_copyout(void *c, uint64_t u, const void *s, size_t n) { (void)c; memcpy((void *)(uintptr_t)u, s, n); return 0; }

/* fake page tables: count what the uapi asks for */
static int vm_live, vm_maps, vm_unmaps;
static uint64_t vm_mapped;
static uint32_t vm_last_flags;
static int vm_dummy;
/* h_vm_full: the OS has no VM that could run (all VMIDs taken) */
static int h_vm_full;
static void *h_vm_create(void *c)
{
    (void)c;
    if (h_vm_full)
        return NULL;
    vm_live++;
    return &vm_dummy;
}
static uint32_t h_query_flags;
static int h_bo_userptr(void *c, void *os, rdna3_bo *bo, uint64_t uaddr, uint32_t flags)
{
    (void)c; (void)flags;
    if (os != (void *)0x7a5c)
        return -RD_EFAULT;
    bo->backing = (void *)(uintptr_t)uaddr;
    live_bos++;
    return 0;
}
/* IOSurface 7 plane 0 has 64 KiB of the accelerator's VRAM at 1 MiB;
 * surface 9 answers a bad (unaligned) range */
/* the OS's hold on each surface's allocation, as the kext's
 * RDNA3VramHandle refs: every BO takes one, bo_free drops it; surface 11
 * is 100 MiB (more than half of the 256 MiB visible VRAM over two imports) */
static int h_surf_refs[16];
static uint32_t h_dirty_surface, h_dirty_plane;
static int h_surface_bo(void *c, void *os, rdna3_bo *bo, uint32_t id, uint32_t plane)
{
    (void)c;
    if ((os != (void *)0x7a5c && os != (void *)0x7a5d) || plane)
        return -RD_ENOENT;
    if (id == 9) {
        bo->vram_offset = 0x100800; bo->size = 0x10000;
    } else if (id == 7) {
        bo->vram_offset = 0x100000; bo->size = 0x10000;
    } else if (id == 11) {
        bo->vram_offset = 0x10000000; bo->size = 100u << 20;
    } else {
        return -RD_ENOENT;
    }
    h_surf_refs[id]++;
    bo->backing = &h_surf_refs[id];
    live_bos++;
    return 0;
}
static unsigned h_foreign_seen;
static void count_foreign(void *ctx, rdna3_bo *bo) { (void)ctx; if (bo->foreign) h_foreign_seen++; }
static int h_surface_dirty(void *c, void *os, uint32_t id, uint32_t plane)
{
    (void)c; (void)os;
    h_dirty_surface = id; h_dirty_plane = plane;
    return id == 7 ? 0 : -RD_ENOENT;
}
static int h_sensor(void *c, uint32_t type, uint32_t *v)
{
    (void)c;
    switch (type) {
    case AMDGPU_INFO_SENSOR_GFX_SCLK: *v = 2124; return 0;
    case AMDGPU_INFO_SENSOR_GPU_TEMP: *v = 61000; return 0;
    default: return -RD_EINVAL;
    }
}
static int h_fault_info(void *c, void *vm, uint64_t *addr, uint32_t *status, uint32_t *hub)
{
    (void)c; (void)vm;
    *addr = 0x500000000000ull; *status = 0x00a02400; *hub = AMDGPU_VMHUB_TYPE_GFX;
    return 0;
}
static uint32_t h_vm_query(void *c, void *vm) { (void)c; (void)vm; return h_query_flags; }
static void h_vm_destroy(void *c, void *vm) { (void)c; (void)vm; vm_live--; }
static int h_vm_map(void *c, void *vm, rdna3_bo *bo, uint64_t va, uint64_t off, uint64_t size,
                    uint32_t flags)
{
    (void)c; (void)vm; (void)bo; (void)va; (void)off;
    if (va == 0xBAD000)
        return -RD_ENOMEM;
    vm_last_flags = flags;
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
/* h_ring32: the ring answers 32-bit fence numbers, like the kext's rings */
static int h_ring32;
/* h_only: only the fence h_only_seq signals (another ring that is done
 * while this one runs on); the others wait like a hung ring */
static int h_only;
static uint64_t h_only_seq;
static uint32_t last_ip, last_ring, last_nibs;
static rdna3_ib last_ibs[RD_MAX_CS_IBS];
static void *last_vm;
static rdna3_cs_job last_job;
static int h_submit(void *c, void *vm, uint32_t ip, uint32_t ring, const rdna3_ib *ibs,
                    uint32_t n, const rdna3_cs_job *job, uint64_t *seq)
{
    (void)c;
    last_job = *job;
    last_vm = vm; last_ip = ip; last_ring = ring; last_nibs = n;
    memcpy(last_ibs, ibs, sizeof(*ibs) * n);
    *seq = ++ring_next;
    if (h_ring32)
        *seq = (uint32_t)*seq;
    return 0;
}
static int h_signaled(void *c, uint32_t ip, uint32_t ring, uint64_t seq)
{
    (void)c; (void)ip; (void)ring;
    if (h_only)
        return seq == h_only_seq;
    if (h_ring32)
        return (int32_t)((uint32_t)ring_done - (uint32_t)seq) >= 0;
    return seq <= ring_done;
}
/* runs "while the lock is dropped": what another thread could do meanwhile */
static void (*h_during_wait)(void);
/* what the close of a file with a job on the ring still had when it waited */
static int probe_vm_live = -1, probe_vm_unmaps = -1, probe_ioctl = 1;
static rdna3_drm_file *probe_file;
static int cs_submit(rdna3_drm_file *f, uint64_t va, uint32_t bytes, uint32_t ip, uint32_t in_sobj,
                     uint32_t out_sobj, uint64_t *handle);
static void probe_close_wait(void)
{
    probe_vm_live = vm_live;
    probe_vm_unmaps = vm_unmaps;
    probe_ioctl = cs_submit(probe_file, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL);
}
/* a hung GPU (h_hang): no fence ever signals; a slice of a wait-any moves
 * the fake clock 1 ms, a full wait (the caller's own deadline) is where the
 * OS's 10 s cap and recovery would run */
static int h_hang, h_full_waits;
static uint64_t h_clock, h_slices;
static uint64_t h_now(void *c) { (void)c; return h_clock; }
static int h_wait(void *c, uint32_t ip, uint32_t ring, uint64_t seq, uint64_t abs_ns)
{
    (void)c; (void)ip; (void)ring;
    if (h_only && seq == h_only_seq)
        return 0;
    if (h_hang || h_only) {
        if (abs_ns == (uint64_t)INT64_MAX) {
            h_full_waits++;
        } else {
            h_slices++;
            h_clock = abs_ns;
        }
        return -RD_ETIME;
    }
    if (h_during_wait) {
        void (*fn)(void) = h_during_wait;
        h_during_wait = NULL;
        fn();
    }
    if (abs_ns == 1)          /* "already expired" */
        return -RD_ETIME;
    if (h_ring32) {
        if ((int32_t)((uint32_t)seq - (uint32_t)ring_done) > 0)
            ring_done += (uint32_t)((uint32_t)seq - (uint32_t)ring_done);
        return 0;
    }
    if (seq > ring_done)
        ring_done = seq;
    return 0;
}

static rdna3_drm_file *g_wait_file;
static uint32_t g_wait_handle;
static void destroy_during_wait(void)
{
    struct drm_syncobj_destroy d;
    memset(&d, 0, sizeof(d));
    d.handle = g_wait_handle;
    rdna3_drm_ioctl(g_wait_file, RD_NR_SYNCOBJ_DESTROY, &d, sizeof(d));
}

static uint64_t chunk_ptrs[8];
static struct drm_amdgpu_cs_chunk chunks[8];
/* set g_uf.handle to add an AMDGPU_CHUNK_ID_FENCE to cs_submit;
 * g_out_first puts the out-syncobj chunk before the in-syncobj one */
static struct drm_amdgpu_cs_chunk_fence g_uf;
static int g_out_first;
/* g_bl: an AMDGPU_CHUNK_ID_BO_HANDLES chunk; g_bl_handle: cs.in.bo_list_handle */
static struct drm_amdgpu_bo_list_in *g_bl;
static struct drm_amdgpu_cs_chunk_dep *g_dep;   /* a DEPENDENCIES chunk when set */
static uint32_t g_bl_handle;
/* g_shadow: an AMDGPU_CHUNK_ID_CP_GFX_SHADOW chunk of g_shadow_dw dwords */
static struct drm_amdgpu_cs_chunk_cp_gfx_shadow *g_shadow;
static uint32_t g_shadow_dw;
/* cs.in.ctx_id; 0: a context of the file's own (cs_ctx_of), unless
 * g_cs_ctx_raw sends g_cs_ctx as it is */
static uint32_t g_cs_ctx;
static int g_cs_ctx_raw;
static int ctx_alloc(rdna3_drm_file *f, int32_t prio, uint32_t *id);
/* a live context of 'f' for submissions that do not care which (the CS
 * needs one: amdgpu_cs_parser_init's amdgpu_ctx_get) */
static uint32_t cs_ctx_of(rdna3_drm_file *f)
{
    static struct { rdna3_drm_file *f; uint32_t id; } cache[16];
    uint32_t i, id = 0;
    for (i = 0; i < 16; i++)
        if (cache[i].f == f && cache[i].id && f->ctx_used[cache[i].id] && !f->closing &&
            f->ctx_vram_lost[cache[i].id] == f->dev->vram_lost_counter)   /* not refused */
            return cache[i].id;
    if (ctx_alloc(f, AMDGPU_CTX_PRIORITY_NORMAL, &id))
        return 0;
    for (i = 0; i < 16; i++)
        if (cache[i].f == f || !cache[i].f) {
            cache[i].f = f;
            cache[i].id = id;
            break;
        }
    return id;
}
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
    if (out_sobj && g_out_first) {
        out.handle = out_sobj;
        chunks[n].chunk_id = AMDGPU_CHUNK_ID_SYNCOBJ_OUT; chunks[n].length_dw = 1;
        chunks[n].chunk_data = (uint64_t)(uintptr_t)&out; n++;
    }
    if (in_sobj) {
        in.handle = in_sobj;
        chunks[n].chunk_id = AMDGPU_CHUNK_ID_SYNCOBJ_IN; chunks[n].length_dw = 1;
        chunks[n].chunk_data = (uint64_t)(uintptr_t)&in; n++;
    }
    if (out_sobj && !g_out_first) {
        out.handle = out_sobj;
        chunks[n].chunk_id = AMDGPU_CHUNK_ID_SYNCOBJ_OUT; chunks[n].length_dw = 1;
        chunks[n].chunk_data = (uint64_t)(uintptr_t)&out; n++;
    }
    if (g_uf.handle) {
        chunks[n].chunk_id = AMDGPU_CHUNK_ID_FENCE; chunks[n].length_dw = sizeof(g_uf) / 4;
        chunks[n].chunk_data = (uint64_t)(uintptr_t)&g_uf; n++;
    }
    if (g_bl) {
        chunks[n].chunk_id = AMDGPU_CHUNK_ID_BO_HANDLES; chunks[n].length_dw = sizeof(*g_bl) / 4;
        chunks[n].chunk_data = (uint64_t)(uintptr_t)g_bl; n++;
    }
    if (g_dep) {
        chunks[n].chunk_id = AMDGPU_CHUNK_ID_DEPENDENCIES; chunks[n].length_dw = sizeof(*g_dep) / 4;
        chunks[n].chunk_data = (uint64_t)(uintptr_t)g_dep; n++;
    }
    if (g_shadow) {
        chunks[n].chunk_id = AMDGPU_CHUNK_ID_CP_GFX_SHADOW; chunks[n].length_dw = g_shadow_dw;
        chunks[n].chunk_data = (uint64_t)(uintptr_t)g_shadow; n++;
    }
    for (uint32_t i = 0; i < n; i++)
        chunk_ptrs[i] = (uint64_t)(uintptr_t)&chunks[i];
    memset(&cs, 0, sizeof(cs));
    cs.in.num_chunks = n;
    cs.in.chunks = (uint64_t)(uintptr_t)chunk_ptrs;
    cs.in.bo_list_handle = g_bl_handle;
    cs.in.ctx_id = (g_cs_ctx || g_cs_ctx_raw) ? g_cs_ctx : cs_ctx_of(f);
    r = rdna3_drm_ioctl(f, RD_NR_AMDGPU(DRM_AMDGPU_CS), &cs, sizeof(cs));
    if (!r && handle)
        *handle = cs.out.handle;
    return r;
}

#define IOCTL(f, nr, arg) rdna3_drm_ioctl((f), (nr), &(arg), sizeof(arg))

/* GEM_CREATE of a GDS / GWS / OA BO: 0 and its handle, or the error */
static int onchip_create(rdna3_drm_file *f, uint32_t dom, uint64_t size, uint64_t align, uint64_t flags,
                  uint32_t *handle)
{
    union drm_amdgpu_gem_create gc;
    int r;
    memset(&gc, 0, sizeof(gc));
    gc.in.bo_size = size; gc.in.alignment = align; gc.in.domains = dom;
    gc.in.domain_flags = flags;
    r = IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_CREATE), gc);
    if (!r)
        *handle = gc.out.handle;
    return r;
}

static int info(rdna3_drm_file *f, uint32_t query, void *out, uint32_t size)
{
    struct drm_amdgpu_info in;
    memset(&in, 0, sizeof(in));
    in.return_pointer = (uint64_t)(uintptr_t)out;
    in.return_size = size;
    in.query = query;
    return IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in);
}

static int info_sensor(rdna3_drm_file *f, uint32_t type, uint32_t *out)
{
    struct drm_amdgpu_info in;
    memset(&in, 0, sizeof(in));
    in.return_pointer = (uint64_t)(uintptr_t)out;
    in.return_size = 4;
    in.query = AMDGPU_INFO_SENSOR;
    in.sensor_info.type = type;
    return IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in);
}

static uint32_t h_read_reg(void *c, uint32_t dw)
{
    (void)c;
    return 0xA0000000u + dw;
}

static int h_priv;
static int h_privileged(void *c) { (void)c; return h_priv; }

static int ctx_alloc(rdna3_drm_file *f, int32_t prio, uint32_t *id)
{
    union drm_amdgpu_ctx c;
    int r;
    memset(&c, 0, sizeof(c));
    c.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
    c.in.priority = prio;
    r = IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CTX), c);
    *id = r ? 0 : c.out.alloc.ctx_id;
    return r;
}

static int sched(rdna3_drm_file *f, uint32_t op, int32_t fd, int32_t prio, uint32_t ctx)
{
    union drm_amdgpu_sched s;
    memset(&s, 0, sizeof(s));
    s.in.op = op;
    s.in.fd = fd;
    s.in.priority = prio;
    s.in.ctx_id = ctx;
    return IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_SCHED), s);
}

static int h_scanout(void *ctx, struct rd_xnu_scanout *s)
{
    (void)ctx;
    if (s->pipe > 3)
        return -RD_EINVAL;
    s->lit = 1;
    s->nrows = RD_XNU_SCANOUT_ROWS;
    s->rgb[0] = 7;
    return 0;
}

int main(void)
{
    static rdna3_drm_device dev;
    static rdna3_drm_file file;
    rdna3_drm_ops ops = { NULL, h_zalloc, h_free, h_bo_alloc, h_bo_free, h_copyin, h_copyout, NULL, NULL,
                          h_vm_create, h_vm_destroy, h_vm_map, h_vm_unmap, NULL, NULL, NULL, NULL,
                          NULL, NULL, NULL, NULL, NULL, NULL,
                          NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
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
    /* no VM that can run (the kext: no VMID left): the open fails cleanly,
     * nothing of the file stays on the device; the next open works */
    {
        static rdna3_drm_file full;
        uint32_t next = dev.next_file_id;
        h_vm_full = 1;
        CHECK(rdna3_drm_file_open(&dev, &full) == -RD_ENOMEM);
        CHECK(dev.files == f && f->next_file == NULL && dev.next_file_id == next && vm_live == 1);
        h_vm_full = 0;
        CHECK(rdna3_drm_file_open(&dev, &full) == 0 && vm_live == 2 && dev.files == &full);
        rdna3_drm_file_close(&full);
        CHECK(vm_live == 1 && dev.files == f);
    }

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
        {
            /* amdgpu_ring_max_ibs (GFX 192, compute 125, JPEG 16, others 49)
             * within the CS ioctl's RD_MAX_CS_IBS */
            uint32_t max_ibs[AMDGPU_HW_IP_NUM], k;
            CHECK(info(f, AMDGPU_INFO_MAX_IBS, max_ibs, sizeof(max_ibs)) == 0);
            for (k = 0; k < AMDGPU_HW_IP_NUM; k++)
                CHECK(max_ibs[k] == RD_MAX_CS_IBS);
        }
        CHECK(di.cu_bitmap[0][0] == 0x3ff && di.cu_ao_mask == 0 && di.cu_ao_bitmap[0][0] == 0 &&
              di.enabled_rb_pipes_mask == 0xfff);
        /* harvesting read from the GC replaces the defaults */
        {
            uint32_t bm[4][4];
            memset(bm, 0, sizeof(bm));
            bm[0][0] = 0x3ff; bm[0][1] = 0xff; bm[1][0] = 0x3ff; bm[1][1] = 0x3ff; bm[2][0] = 0x3ff;
            rdna3_drm_device_set_cu_info(&dev, bm, 48, 0x3fe);
            memset(&di, 0, sizeof(di));
            CHECK(info(f, AMDGPU_INFO_DEV_INFO, &di, sizeof(di)) == 0);
            CHECK(di.cu_active_number == 48 && di.cu_bitmap[0][1] == 0xff && di.cu_bitmap[2][1] == 0 &&
                  di.enabled_rb_pipes_mask == 0x3fe && di.num_rb_pipes == 12);
            bm[0][1] = 0x3ff; bm[2][1] = 0x3ff;
            rdna3_drm_device_set_cu_info(&dev, bm, 60, 0xfff);
        }
        /* gfx_v11_0_get_tcc_info's mask (rdna3amdgpu=1) */
        CHECK(di.tcc_disabled_mask == 0);
        rdna3_drm_device_set_tcc_disabled(&dev, 0x10007);
        memset(&di, 0, sizeof(di));
        CHECK(info(f, AMDGPU_INFO_DEV_INFO, &di, sizeof(di)) == 0 && di.tcc_disabled_mask == 0x10007);
        rdna3_drm_device_set_tcc_disabled(&dev, 0);
        /* VBIOS / discovery values (rdna3amdgpu=1); 0 keeps */
        rdna3_drm_device_set_asic_info(&dev, 27000, AMDGPU_VRAM_TYPE_HBM, 4096, 96ull << 20);
        memset(&di, 0, sizeof(di));
        CHECK(info(f, AMDGPU_INFO_DEV_INFO, &di, sizeof(di)) == 0 && di.gpu_counter_freq == 27000 &&
              di.vram_type == AMDGPU_VRAM_TYPE_HBM && di.vram_bit_width == 4096 && di.mall_size == (96ull << 20));
        rdna3_drm_device_set_asic_info(&dev, 0, 0, 0, 0);
        memset(&di, 0, sizeof(di));
        CHECK(info(f, AMDGPU_INFO_DEV_INFO, &di, sizeof(di)) == 0 && di.gpu_counter_freq == 27000);
        rdna3_drm_device_set_asic_info(&dev, 100000, AMDGPU_VRAM_TYPE_GDDR6, 256, 64ull << 20);
        /* clocks from DPM: 500..2124 MHz gfx, 96..1218 MHz uclk; 0 keeps */
        rdna3_drm_device_set_clocks(&dev, 500000, 2124000, 96000, 1218000);
        rdna3_drm_device_set_clocks(&dev, 0, 0, 0, 0);
        memset(&di, 0, sizeof(di));
        CHECK(info(f, AMDGPU_INFO_DEV_INFO, &di, sizeof(di)) == 0);
        CHECK(di.min_engine_clock == 500000 && di.max_engine_clock == 2124000 &&
              di.min_memory_clock == 96000 && di.max_memory_clock == 1218000);
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
        /* amdgpu_gem_create_ioctl: VRAM is always handed out cleared */
        CHECK(rdna3_drm_bo_lookup(f, h)->flags & AMDGPU_GEM_CREATE_VRAM_CLEARED);
        CHECK(info(f, AMDGPU_INFO_VRAM_USAGE, &vram, 8) == 0 && vram == (1 << 20));
        /* amdgpu_vram_mgr_vis_size: a BO at offset 0 lies in the CPU-visible window */
        CHECK(info(f, AMDGPU_INFO_VIS_VRAM_USAGE, &vram, 8) == 0 && vram == (1 << 20));

        memset(&c, 0, sizeof(c));
        c.in.bo_size = 128u << 20;
        c.in.domains = AMDGPU_GEM_DOMAIN_VRAM | AMDGPU_GEM_DOMAIN_GTT;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_CREATE), c) == 0);
        h2 = c.out.handle;
        CHECK(rdna3_drm_bo_lookup(f, h2)->placement == AMDGPU_GEM_DOMAIN_GTT);
        /* VRAM only (RADV's descriptor pools: VRAM + CPU access) and no room:
         * amdgpu_gem_create_ioctl retries with GTT added (card run 51: the
         * visible VRAM was full, vkCreateComputePipelines OUT_OF_DEVICE_MEMORY) */
        {
            struct drm_gem_close gcl;
            memset(&c, 0, sizeof(c));
            c.in.bo_size = 128u << 20;
            c.in.domains = AMDGPU_GEM_DOMAIN_VRAM;
            c.in.domain_flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
            CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_CREATE), c) == 0);
            CHECK(rdna3_drm_bo_lookup(f, c.out.handle)->placement == AMDGPU_GEM_DOMAIN_GTT);
            memset(&gcl, 0, sizeof(gcl));
            gcl.handle = c.out.handle;
            CHECK(IOCTL(f, RD_NR_GEM_CLOSE, gcl) == 0);
        }
        /* the eviction list before a sleep: the VRAM BO once, not the GTT one */
        {
            uint32_t n = 0;
            rdna3_drm_device_for_each_vram_bo(&dev, count_bo, &n);
            CHECK(n == 1);
            n = 0;
            rdna3_drm_device_for_each_vram_bo(&dev, count_bo, &n);   /* a second pass sees it again */
            CHECK(n == 1);
        }

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
        /* PR #2 review: offset + size wrapping u64 must not pass as small
         * (it mapped the VRAM below the BO) */
        va.offset_in_bo = ~0ull - 4095; va.map_size = 2 * 4096;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == -RD_EINVAL);
        va.offset_in_bo = (1 << 20) + 4096; va.map_size = 4096;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == -RD_EINVAL);
        va.offset_in_bo = 0; va.va_address = ~0ull - 4095; va.map_size = 2 * 4096;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == -RD_EINVAL);
        va.va_address = 0x300000; va.map_size = 4096;
        CHECK(vm_maps == 1);
        /* REPLACE drops what it overlaps */
        va.va_address = 0x200000; va.map_size = 2 * 4096;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == 0);
        va.operation = AMDGPU_VA_OP_REPLACE; va.va_address = 0x201000; va.map_size = 4096;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == 0);
        CHECK(vm_maps == 3 && vm_unmaps == 2 && vm_mapped == 4096);
        /* PR #2 review: MAP refuses an overlap (amdgpu_vm_bo_map), nothing
         * in the GMC hole or past the VM, the high half is the low one */
        va.operation = AMDGPU_VA_OP_MAP; va.va_address = 0x200000; va.map_size = 2 * 4096;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == -RD_EINVAL && vm_maps == 3);
        va.va_address = 0x0000800000000000ull; va.map_size = 4096;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == -RD_EINVAL);
        va.va_address = (1ull << 48) - 4096;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == -RD_EINVAL);
        va.va_address = 0xffff800000400000ull;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == 0 && vm_maps == 4);
        va.va_address = 0x800000;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == 0);
        va.va_address = 0x0000800000400000ull & 0x0000ffffffffffffull;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == -RD_EINVAL);   /* the hole */
        va.operation = AMDGPU_VA_OP_UNMAP; va.va_address = 0xffff800000400000ull;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == 0);
        va.va_address = 0x800000;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == 0);
        CHECK(vm_maps == 5 && vm_unmaps == 4 && vm_mapped == 4096);
        /* closing the handle drops its mappings, like amdgpu_gem_object_close */

        cl.handle = h; cl.pad = 0;
        CHECK(IOCTL(f, RD_NR_GEM_CLOSE, cl) == 0);
        CHECK(IOCTL(f, RD_NR_GEM_CLOSE, cl) == -RD_EINVAL);
        CHECK(vm_mapped == 0 && vm_unmaps == 5);
        va.operation = AMDGPU_VA_OP_CLEAR; va.va_address = 0x200000; va.map_size = 1 << 20;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == 0);
        CHECK(vm_unmaps == 5);
        CHECK(info(f, AMDGPU_INFO_VRAM_USAGE, &vram, 8) == 0 && vram == 0);
        CHECK(info(f, AMDGPU_INFO_VIS_VRAM_USAGE, &vram, 8) == 0 && vram == 0);
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
        /* reset state comes from the OS through vm_query (amdgpu_ctx_query2);
         * VRAMLOST is the context's own, the VM's is ignored */
        memset(&c, 0, sizeof(c));
        c.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CTX), c) == 0);
        id = c.out.alloc.ctx_id;
        dev.ops.vm_query = h_vm_query;
        h_query_flags = AMDGPU_CTX_QUERY2_FLAGS_RESET | AMDGPU_CTX_QUERY2_FLAGS_VRAMLOST;
        memset(&c, 0, sizeof(c));
        c.in.op = AMDGPU_CTX_OP_QUERY_STATE2; c.in.ctx_id = id;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CTX), c) == 0 &&
              c.out.state.flags == AMDGPU_CTX_QUERY2_FLAGS_RESET);
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
    /* VRAM lost for good (amdgpu_inc_vram_lost): the counter, and the
     * contexts made before it report VRAMLOST (amdgpu_ctx_query2:
     * ctx->generation != amdgpu_vm_generation); one made after does not */
    {
        union drm_amdgpu_ctx c;
        uint32_t before, after, lost = 7;
        CHECK(info(f, AMDGPU_INFO_VRAM_LOST_COUNTER, &lost, 4) == 0 && lost == 0);
        CHECK(ctx_alloc(f, AMDGPU_CTX_PRIORITY_NORMAL, &before) == 0);
        memset(&c, 0, sizeof(c));
        c.in.op = AMDGPU_CTX_OP_QUERY_STATE2; c.in.ctx_id = before;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CTX), c) == 0 && c.out.state.flags == 0);
        rdna3_drm_device_vram_lost(&dev);
        CHECK(info(f, AMDGPU_INFO_VRAM_LOST_COUNTER, &lost, 4) == 0 && lost == 1);
        CHECK(ctx_alloc(f, AMDGPU_CTX_PRIORITY_NORMAL, &after) == 0 && after != before);
        memset(&c, 0, sizeof(c));
        c.in.op = AMDGPU_CTX_OP_QUERY_STATE2; c.in.ctx_id = before;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CTX), c) == 0 &&
              c.out.state.flags == AMDGPU_CTX_QUERY2_FLAGS_VRAMLOST);
        memset(&c, 0, sizeof(c));
        c.in.op = AMDGPU_CTX_OP_QUERY_STATE2; c.in.ctx_id = after;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CTX), c) == 0 && c.out.state.flags == 0);
        /* QUERY_STATE (amdgpu_ctx_query) knows only resets */
        memset(&c, 0, sizeof(c));
        c.in.op = AMDGPU_CTX_OP_QUERY_STATE; c.in.ctx_id = before;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CTX), c) == 0 &&
              c.out.state.reset_status == AMDGPU_CTX_NO_RESET);
        /* a context id freed and handed out again records the new count */
        memset(&c, 0, sizeof(c));
        c.in.op = AMDGPU_CTX_OP_FREE_CTX; c.in.ctx_id = before;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CTX), c) == 0);
        CHECK(ctx_alloc(f, AMDGPU_CTX_PRIORITY_NORMAL, &before) == 0);
        memset(&c, 0, sizeof(c));
        c.in.op = AMDGPU_CTX_OP_QUERY_STATE2; c.in.ctx_id = before;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CTX), c) == 0 && c.out.state.flags == 0);
        /* a second loss: the context made between the two sees it */
        rdna3_drm_device_vram_lost(&dev);
        CHECK(info(f, AMDGPU_INFO_VRAM_LOST_COUNTER, &lost, 4) == 0 && lost == 2);
        {
            uint64_t l64 = ~0ull;   /* 4 bytes, whatever the buffer (min(size, 4u)) */
            CHECK(info(f, AMDGPU_INFO_VRAM_LOST_COUNTER, &l64, 8) == 0 &&
                  (uint32_t)l64 == 2 && (uint32_t)(l64 >> 32) == 0xffffffffu);
        }
        memset(&c, 0, sizeof(c));
        c.in.op = AMDGPU_CTX_OP_QUERY_STATE2; c.in.ctx_id = after;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CTX), c) == 0 &&
              c.out.state.flags == AMDGPU_CTX_QUERY2_FLAGS_VRAMLOST);
        memset(&c, 0, sizeof(c));
        c.in.op = AMDGPU_CTX_OP_FREE_CTX; c.in.ctx_id = before;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CTX), c) == 0);
        c.in.ctx_id = after;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CTX), c) == 0);
    }
    /* context priorities (amdgpu_ctx_ioctl / amdgpu_ctx_priority_permit) and
     * DRM_AMDGPU_SCHED overrides (amdgpu_sched_ioctl), by file id */
    {
        static rdna3_drm_file other;
        struct rd_xnu_file_id fid, oid;
        uint32_t a, b, c2;
        CHECK(rdna3_drm_file_open(&dev, &other) == 0);
        memset(&fid, 0xff, sizeof(fid));
        CHECK(IOCTL(f, RD_NR_XNU_FILE_ID, fid) == 0 && fid.id != 0 && fid.pad == 0);
        CHECK(IOCTL(&other, RD_NR_XNU_FILE_ID, oid) == 0 && oid.id != 0 && oid.id != fid.id);
        /* garbage priority -> NORMAL; above NORMAL needs privilege */
        CHECK(ctx_alloc(&other, 12345, &a) == 0 && rdna3_drm_ctx_priority(&other, a) == AMDGPU_CTX_PRIORITY_NORMAL);
        CHECK(ctx_alloc(&other, AMDGPU_CTX_PRIORITY_LOW, &b) == 0 &&
              rdna3_drm_ctx_priority(&other, b) == AMDGPU_CTX_PRIORITY_LOW);
        CHECK(ctx_alloc(&other, AMDGPU_CTX_PRIORITY_HIGH, &c2) == -RD_EACCES);
        dev.ops.privileged = h_privileged;
        CHECK(ctx_alloc(&other, AMDGPU_CTX_PRIORITY_HIGH, &c2) == -RD_EACCES);
        h_priv = 1;
        CHECK(ctx_alloc(&other, AMDGPU_CTX_PRIORITY_VERY_HIGH, &c2) == 0 &&
              rdna3_drm_ctx_priority(&other, c2) == AMDGPU_CTX_PRIORITY_VERY_HIGH);
        h_priv = 0;
        /* SCHED: DRM_MASTER first (-EACCES), then op, priority, target */
        CHECK(sched(f, AMDGPU_SCHED_OP_CONTEXT_PRIORITY_OVERRIDE, (int32_t)oid.id,
                    AMDGPU_CTX_PRIORITY_HIGH, a) == -RD_EACCES);
        h_priv = 1;
        CHECK(sched(f, 3, (int32_t)oid.id, AMDGPU_CTX_PRIORITY_HIGH, a) == -RD_EINVAL);
        CHECK(sched(f, AMDGPU_SCHED_OP_CONTEXT_PRIORITY_OVERRIDE, (int32_t)oid.id, 100, a) == -RD_EINVAL);
        CHECK(sched(f, AMDGPU_SCHED_OP_CONTEXT_PRIORITY_OVERRIDE, (int32_t)oid.id,
                    AMDGPU_CTX_PRIORITY_UNSET, a) == -RD_EINVAL);
        CHECK(sched(f, AMDGPU_SCHED_OP_CONTEXT_PRIORITY_OVERRIDE, 0, AMDGPU_CTX_PRIORITY_HIGH, a) == -RD_EINVAL);
        CHECK(sched(f, AMDGPU_SCHED_OP_CONTEXT_PRIORITY_OVERRIDE, (int32_t)oid.id,
                    AMDGPU_CTX_PRIORITY_HIGH, 77) == -RD_EINVAL);
        CHECK(sched(f, AMDGPU_SCHED_OP_CONTEXT_PRIORITY_OVERRIDE, (int32_t)oid.id,
                    AMDGPU_CTX_PRIORITY_HIGH, a) == 0);
        CHECK(rdna3_drm_ctx_priority(&other, a) == AMDGPU_CTX_PRIORITY_HIGH &&
              rdna3_drm_ctx_priority(&other, b) == AMDGPU_CTX_PRIORITY_LOW);
        /* process: every context of the file; init priority kept underneath */
        CHECK(sched(f, AMDGPU_SCHED_OP_PROCESS_PRIORITY_OVERRIDE, (int32_t)oid.id,
                    AMDGPU_CTX_PRIORITY_VERY_LOW, 0) == 0);
        CHECK(rdna3_drm_ctx_priority(&other, a) == AMDGPU_CTX_PRIORITY_VERY_LOW &&
              rdna3_drm_ctx_priority(&other, b) == AMDGPU_CTX_PRIORITY_VERY_LOW &&
              rdna3_drm_ctx_priority(&other, c2) == AMDGPU_CTX_PRIORITY_VERY_LOW);
        CHECK(other.ctx_init_prio[c2] == AMDGPU_CTX_PRIORITY_VERY_HIGH);
        CHECK(rdna3_drm_ctx_priority(&other, 200) == AMDGPU_CTX_PRIORITY_UNSET);
        /* a new context starts from its own priority, not the override */
        {
            uint32_t d;
            CHECK(ctx_alloc(&other, AMDGPU_CTX_PRIORITY_NORMAL, &d) == 0 &&
                  rdna3_drm_ctx_priority(&other, d) == AMDGPU_CTX_PRIORITY_NORMAL);
        }
        h_priv = 0;
        dev.ops.privileged = NULL;
        rdna3_drm_file_close(&other);
        /* a closed file is no target any more */
        h_priv = 1;
        dev.ops.privileged = h_privileged;
        CHECK(sched(f, AMDGPU_SCHED_OP_PROCESS_PRIORITY_OVERRIDE, (int32_t)oid.id,
                    AMDGPU_CTX_PRIORITY_LOW, 0) == -RD_EINVAL);
        CHECK(sched(f, AMDGPU_SCHED_OP_PROCESS_PRIORITY_OVERRIDE, (int32_t)fid.id,
                    AMDGPU_CTX_PRIORITY_LOW, 0) == 0);
        h_priv = 0;
        dev.ops.privileged = NULL;
    }
    /* DRM_AMDGPU_PROC_OPTIONS: no KFD (amdgpu without CONFIG_HSA_AMD);
     * AMDGPU_INFO_NUM_HANDLES: unlimited UVD handles since Polaris */
    {
        struct drm_amdgpu_proc_options po;
        struct drm_amdgpu_info_num_handles nh;
        struct drm_amdgpu_info in;
        memset(&po, 0, sizeof(po));
        po.op = AMDGPU_PROC_OPTIONS_OP_KFD_SIGBUS_DELAY;
        po.kfd_sigbus_delay.value = 100;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_PROC_OPTIONS), po) == -RD_EOPNOTSUPP);
        po.op = 1;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_PROC_OPTIONS), po) == -RD_EINVAL);
        memset(&in, 0, sizeof(in));
        in.return_pointer = (uint64_t)(uintptr_t)&nh;
        in.return_size = sizeof(nh);
        in.query = AMDGPU_INFO_NUM_HANDLES;
        in.query_hw_ip.type = AMDGPU_HW_IP_UVD;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == -RD_ENODATA);
        in.query_hw_ip.type = AMDGPU_HW_IP_VCN_DEC;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == -RD_EINVAL);
    }
    /* GEM_USERPTR: the OS wires the client's pages; no mmap, VA maps work */
    {
        static uint8_t user_pages[2 * 4096] __attribute__((aligned(4096)));
        struct drm_amdgpu_gem_userptr u;
        union drm_amdgpu_gem_mmap m;
        struct drm_amdgpu_gem_va va;
        struct drm_gem_close gc;
        unsigned bos0 = live_bos;
        memset(&u, 0, sizeof(u));
        u.addr = (uint64_t)(uintptr_t)user_pages; u.size = sizeof(user_pages);
        u.flags = AMDGPU_GEM_USERPTR_REGISTER;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_USERPTR), u) == -RD_ENOSYS);   /* no op yet */
        dev.ops.bo_userptr = h_bo_userptr;
        f->os = (void *)0x7a5c;
        u.flags = 0;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_USERPTR), u) == -RD_EACCES);
        u.flags = AMDGPU_GEM_USERPTR_REGISTER; u.addr += 16;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_USERPTR), u) == -RD_EINVAL);   /* unaligned */
        u.addr -= 16;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_USERPTR), u) == 0 && u.handle != 0);
        CHECK(live_bos == bos0 + 1 && rdna3_drm_bo_lookup(f, u.handle)->userptr &&
              rdna3_drm_bo_lookup(f, u.handle)->backing == user_pages);
        memset(&m, 0, sizeof(m));
        m.in.handle = u.handle;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_MMAP), m) == -RD_EPERM);
        memset(&va, 0, sizeof(va));
        va.handle = u.handle; va.operation = AMDGPU_VA_OP_MAP; va.va_address = 0x900000;
        va.map_size = sizeof(user_pages);
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == 0);
        va.operation = AMDGPU_VA_OP_UNMAP;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == 0);
        gc.handle = u.handle; gc.pad = 0;
        CHECK(IOCTL(f, RD_NR_GEM_CLOSE, gc) == 0 && live_bos == bos0);
        /* PR #2 review: a READONLY userptr is mapped without WRITEABLE
         * whatever the client asks (amdgpu_vm_bo_update) */
        u.flags = AMDGPU_GEM_USERPTR_READONLY;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_USERPTR), u) == 0 && u.handle != 0);
        va.handle = u.handle; va.operation = AMDGPU_VA_OP_MAP;
        va.flags = AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == 0 &&
              (vm_last_flags & AMDGPU_VM_PAGE_READABLE) && !(vm_last_flags & AMDGPU_VM_PAGE_WRITEABLE));
        /* VRAM lost (a power loss in the sleep): every mapping goes into
         * the new tables again, with the same flags */
        {
            int maps0 = vm_maps;
            uint64_t mapped0 = vm_mapped;
            vm_last_flags = 0;
            CHECK(rdna3_drm_device_vm_replay(&dev) == 0 && vm_maps == maps0 + 1 &&
                  vm_mapped == mapped0 + va.map_size);
            CHECK((vm_last_flags & AMDGPU_VM_PAGE_READABLE) && !(vm_last_flags & AMDGPU_VM_PAGE_WRITEABLE));
            vm_mapped = mapped0;   /* the fake tables count a replay as new pages */
        }
        va.operation = AMDGPU_VA_OP_UNMAP;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == 0);
        va.flags = 0;
        gc.handle = u.handle;
        CHECK(IOCTL(f, RD_NR_GEM_CLOSE, gc) == 0 && live_bos == bos0);
        dev.ops.bo_userptr = NULL;
    }
    /* RD_NR_XNU_SURFACE_BO: a BO over an IOSurface's VRAM (A3) - mapped like
     * any VRAM BO, not counted as the client's VRAM, the OS's hold dropped
     * with the last reference; DIRTY reaches the OS */
    {
        struct rd_xnu_surface_bo sb;
        union drm_amdgpu_gem_mmap m;
        struct rd_xnu_bo_share sh;
        struct drm_gem_close gc;
        unsigned bos0 = live_bos;
        uint64_t used0 = dev.vram_usage, vis0 = dev.vis_vram_usage;
        rdna3_bo *bo;
        memset(&sb, 0, sizeof(sb));
        sb.op = RD_XNU_SURFACE_BO_IMPORT; sb.surface_id = 7;
        CHECK(IOCTL(f, RD_NR_XNU_SURFACE_BO, sb) == -RD_ENOSYS);   /* no OS hook */
        dev.ops.surface_bo = h_surface_bo;
        dev.ops.surface_dirty = h_surface_dirty;
        f->os = (void *)0x7a5c;
        sb.surface_id = 0;
        CHECK(IOCTL(f, RD_NR_XNU_SURFACE_BO, sb) == -RD_EINVAL);
        sb.surface_id = 8;
        CHECK(IOCTL(f, RD_NR_XNU_SURFACE_BO, sb) == -RD_ENOENT && live_bos == bos0);
        sb.surface_id = 9;
        CHECK(IOCTL(f, RD_NR_XNU_SURFACE_BO, sb) == -RD_EINVAL && live_bos == bos0);
        sb.plane = RD_XNU_SURFACE_MAX_PLANE + 1;
        CHECK(IOCTL(f, RD_NR_XNU_SURFACE_BO, sb) == -RD_EINVAL);
        sb.plane = 0;
        sb.op = 5; sb.surface_id = 7;
        CHECK(IOCTL(f, RD_NR_XNU_SURFACE_BO, sb) == -RD_EINVAL);
        sb.op = RD_XNU_SURFACE_BO_IMPORT;
        sb.size = 0x10001;   /* more than the surface has */
        CHECK(IOCTL(f, RD_NR_XNU_SURFACE_BO, sb) == -RD_ENOSPC && live_bos == bos0);
        sb.size = 0xF000;
        CHECK(IOCTL(f, RD_NR_XNU_SURFACE_BO, sb) == 0 && sb.handle != 0 && sb.size == 0x10000);
        bo = rdna3_drm_bo_lookup(f, sb.handle);
        CHECK(bo && bo->foreign && bo->placement == AMDGPU_GEM_DOMAIN_VRAM && bo->vram_offset == 0x100000);
        CHECK(live_bos == bos0 + 1 && dev.vram_usage == used0 && dev.vis_vram_usage == vis0);
        memset(&m, 0, sizeof(m));
        m.in.handle = sb.handle;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_MMAP), m) == 0 && m.out.addr_ptr != 0);
        CHECK(rdna3_drm_bo_by_mmap_offset(f, m.out.addr_ptr) == bo);
        /* no PRIME token for it: every importer goes through its own
         * IOSurface lookup (ECC security review) */
        memset(&sh, 0, sizeof(sh));
        sh.handle = sb.handle;
        CHECK(IOCTL(f, RD_NR_XNU_BO_EXPORT, sh) == -RD_EPERM && sh.token == 0);
        /* a CPU mapping's reference (the OS's) outlives the handle */
        rdna3_drm_bo_get(bo);
        gc.handle = sb.handle; gc.pad = 0;
        CHECK(IOCTL(f, RD_NR_GEM_CLOSE, gc) == 0 && live_bos == bos0 + 1);
        rdna3_drm_bo_put(&dev, bo);
        CHECK(live_bos == bos0 && dev.vram_usage == used0);
        sb.op = RD_XNU_SURFACE_BO_DIRTY; sb.surface_id = 7; sb.plane = 0;
        CHECK(IOCTL(f, RD_NR_XNU_SURFACE_BO, sb) == 0 && h_dirty_surface == 7 && h_dirty_plane == 0);
        sb.surface_id = 3;
        CHECK(IOCTL(f, RD_NR_XNU_SURFACE_BO, sb) == -RD_ENOENT);
        CHECK(strcmp(rdna3_drm_ioctl_name(RD_NR_XNU_SURFACE_BO), "XNU_SURFACE_BO") == 0);
        /* the probe the Metal plugin sends: surface 0 reaches the OS and is
         * not there; a plane past the last one never reaches it */
        sb.surface_id = 0; h_dirty_surface = 99;
        CHECK(IOCTL(f, RD_NR_XNU_SURFACE_BO, sb) == -RD_ENOENT && h_dirty_surface == 0);
        sb.surface_id = 7; sb.plane = RD_XNU_SURFACE_MAX_PLANE + 1; h_dirty_surface = 99;
        CHECK(IOCTL(f, RD_NR_XNU_SURFACE_BO, sb) == -RD_EINVAL && h_dirty_surface == 99);
        sb.plane = 0;
        /* every failure path gives the OS's hold back */
        CHECK(h_surf_refs[7] == 0 && h_surf_refs[9] == 0);
        {
            static rdna3_drm_file file2;
            rdna3_drm_file *f2 = &file2;
            struct drm_amdgpu_gem_va va;
            struct rd_xnu_surface_bo a1, a2, a3;
            int maps0, unmaps0;
            CHECK(rdna3_drm_file_open(&dev, f2) == 0);
            f2->os = (void *)0x7a5d;
            /* two processes, and one of them twice: a BO and a hold each */
            memset(&a1, 0, sizeof(a1)); a1.op = RD_XNU_SURFACE_BO_IMPORT; a1.surface_id = 7;
            a2 = a1; a3 = a1;
            CHECK(IOCTL(f, RD_NR_XNU_SURFACE_BO, a1) == 0 && IOCTL(f2, RD_NR_XNU_SURFACE_BO, a2) == 0 &&
                  IOCTL(f2, RD_NR_XNU_SURFACE_BO, a3) == 0);
            CHECK(h_surf_refs[7] == 3 && a2.handle != a3.handle);
            CHECK(rdna3_drm_bo_lookup(f, a1.handle) != rdna3_drm_bo_lookup(f2, a2.handle) &&
                  rdna3_drm_bo_lookup(f, a1.handle)->vram_offset == rdna3_drm_bo_lookup(f2, a2.handle)->vram_offset);
            gc.handle = a3.handle;
            CHECK(IOCTL(f2, RD_NR_GEM_CLOSE, gc) == 0 && h_surf_refs[7] == 2);
            /* RADV maps it in its VA like any VRAM BO; it is on the VRAM BO
             * list (the kext's eviction skips it: the accelerator's list
             * has the range) and a VRAM loss maps it again */
            maps0 = vm_maps; unmaps0 = vm_unmaps;
            memset(&va, 0, sizeof(va));
            va.handle = a2.handle; va.operation = AMDGPU_VA_OP_MAP; va.va_address = 0x400000;
            va.map_size = 0x10000;
            CHECK(IOCTL(f2, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == 0 && vm_maps == maps0 + 1);
            va.map_size = 0x20000; va.va_address = 0x600000;
            CHECK(IOCTL(f2, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), va) == -RD_EINVAL);   /* past the surface */
            va.map_size = 0x10000;
            h_foreign_seen = 0;
            rdna3_drm_device_for_each_vram_bo(&dev, count_foreign, NULL);
            CHECK(h_foreign_seen == 2);
            CHECK(rdna3_drm_device_vm_replay(&dev) == 0 && vm_maps == maps0 + 2);
            vm_mapped -= 0x10000;   /* the fake tables count a replay as new pages */
            /* the importer goes with the mapping in place: unmapped, hold dropped */
            rdna3_drm_file_close(f2);
            CHECK(vm_unmaps == unmaps0 + 1 && h_surf_refs[7] == 1);
            gc.handle = a1.handle;
            CHECK(IOCTL(f, RD_NR_GEM_CLOSE, gc) == 0 && h_surf_refs[7] == 0);
            CHECK(live_bos == bos0 && dev.vram_usage == used0 && dev.vis_vram_usage == vis0);
            /* one file holds at most half of the visible VRAM in surfaces */
            a1.surface_id = 11; a1.handle = 0; a1.size = 0;
            CHECK(IOCTL(f, RD_NR_XNU_SURFACE_BO, a1) == 0);
            a2 = a1;
            CHECK(IOCTL(f, RD_NR_XNU_SURFACE_BO, a2) == -RD_ENOSPC && h_surf_refs[11] == 1);
            gc.handle = a1.handle;
            CHECK(IOCTL(f, RD_NR_GEM_CLOSE, gc) == 0 && h_surf_refs[11] == 0 && live_bos == bos0);
        }
        dev.ops.surface_bo = NULL;
        dev.ops.surface_dirty = NULL;
        sb.op = RD_XNU_SURFACE_BO_DIRTY;
        CHECK(IOCTL(f, RD_NR_XNU_SURFACE_BO, sb) == -RD_ENOSYS);
    }
    /* sensors and the GPUVM fault cache come from the OS */
    {
        uint32_t v = 0;
        struct drm_amdgpu_info_gpuvm_fault gf;
        CHECK(info_sensor(f, AMDGPU_INFO_SENSOR_GFX_SCLK, &v) == -RD_EINVAL);
        dev.ops.sensor = h_sensor;
        CHECK(info_sensor(f, AMDGPU_INFO_SENSOR_GFX_SCLK, &v) == 0 && v == 2124);
        CHECK(info_sensor(f, AMDGPU_INFO_SENSOR_GPU_TEMP, &v) == 0 && v == 61000);
        CHECK(info_sensor(f, AMDGPU_INFO_SENSOR_VDDNB, &v) == -RD_EINVAL);
        memset(&gf, 0xff, sizeof(gf));
        CHECK(info(f, AMDGPU_INFO_GPUVM_FAULT, &gf, sizeof(gf)) == 0 && gf.addr == 0 && gf.status == 0);
        dev.ops.vm_fault_info = h_fault_info;
        CHECK(info(f, AMDGPU_INFO_GPUVM_FAULT, &gf, sizeof(gf)) == 0 && gf.addr == 0x500000000000ull &&
              gf.status == 0x00a02400 && gf.vmhub == AMDGPU_VMHUB_TYPE_GFX);
        dev.ops.vm_fault_info = NULL;
        dev.ops.sensor = NULL;
    }
    /* syncobj */
    {
        struct drm_syncobj_create sc = { 0, 0 };
        struct drm_syncobj_array arr;
        struct drm_syncobj_wait w;
        struct drm_syncobj_transfer tr;
        struct drm_syncobj_timeline_array ta;
        uint32_t h, h2;
        uint64_t pt;
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_CREATE, sc) == 0);
        h = sc.handle;
        memset(&w, 0, sizeof(w));
        w.handles = (uint64_t)(uintptr_t)&h; w.count_handles = 1;
        /* no fence: -EINVAL, -ETIME with WAIT_FOR_SUBMIT (drm_syncobj_array_wait) */
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_WAIT, w) == -RD_EINVAL);
        w.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_WAIT, w) == -RD_ETIME);
        w.flags = 1u << 7;
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_WAIT, w) == -RD_EINVAL);
        w.flags = 0;
        memset(&arr, 0, sizeof(arr));
        arr.handles = (uint64_t)(uintptr_t)&h; arr.count_handles = 1;
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_SIGNAL, arr) == 0);
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_WAIT, w) == 0);
        /* TRANSFER copies the signalled state (sync_file export/import on
         * Linux); a timeline point that was never submitted is refused */
        sc.handle = 0;
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_CREATE, sc) == 0);
        h2 = sc.handle;
        w.handles = (uint64_t)(uintptr_t)&h2;
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_WAIT, w) == -RD_EINVAL);
        /* PR #2 review: without WAIT_ALL the first ready one ends the wait
         * and is named in first_signaled; with WAIT_ALL all must be */
        {
            uint32_t two[2] = { h2, h };
            w.handles = (uint64_t)(uintptr_t)two; w.count_handles = 2;
            w.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
            CHECK(IOCTL(f, RD_NR_SYNCOBJ_WAIT, w) == 0 && w.first_signaled == 1);
            w.flags |= DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
            CHECK(IOCTL(f, RD_NR_SYNCOBJ_WAIT, w) == -RD_ETIME);
            w.handles = (uint64_t)(uintptr_t)&h2; w.count_handles = 1; w.flags = 0;
        }
        memset(&tr, 0, sizeof(tr));
        tr.src_handle = h; tr.dst_handle = h2;
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_TRANSFER, tr) == 0);
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_WAIT, w) == 0);
        pt = 5;
        memset(&ta, 0, sizeof(ta));
        ta.handles = (uint64_t)(uintptr_t)&h; ta.points = (uint64_t)(uintptr_t)&pt; ta.count_handles = 1;
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_TIMELINE_SIGNAL, ta) == 0);
        tr.src_point = 5; tr.dst_point = 7;
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_TRANSFER, tr) == 0);
        ta.handles = (uint64_t)(uintptr_t)&h2; pt = 0;
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_QUERY, ta) == 0 && pt == 7);
        tr.src_point = 9;
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_TRANSFER, tr) == -RD_EINVAL);
        tr.src_handle = 77;
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_TRANSFER, tr) == -RD_ENOENT);
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
        /* amdgpu_cs_pass1: a context made before VRAM was lost submits
         * nothing more (-ECANCELED); one made after gets through */
        {
            uint32_t old_ctx, new_ctx;
            CHECK(ctx_alloc(f, AMDGPU_CTX_PRIORITY_NORMAL, &old_ctx) == 0);
            g_cs_ctx = old_ctx;
            CHECK(cs_submit(f, 0x400000, 6, AMDGPU_HW_IP_DMA, 0, 0, NULL) == -RD_EINVAL);
            CHECK(dev.vram_lost_cs_refuse == 1);   /* amdgpu's, the default */
            rdna3_drm_device_vram_lost(&dev);
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == -RD_ECANCELED);
            /* the kill switch (kext: rdna3vramlostcs=0): the old context
             * submits again, QUERY_STATE2 still says VRAMLOST */
            rdna3_drm_device_set_vram_lost_cs(&dev, 0);
            CHECK(cs_submit(f, 0x400000, 6, AMDGPU_HW_IP_DMA, 0, 0, NULL) == -RD_EINVAL);
            {
                union drm_amdgpu_ctx qs;
                memset(&qs, 0, sizeof(qs));
                qs.in.op = AMDGPU_CTX_OP_QUERY_STATE2; qs.in.ctx_id = old_ctx;
                CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CTX), qs) == 0 &&
                      qs.out.state.flags == AMDGPU_CTX_QUERY2_FLAGS_VRAMLOST);
            }
            rdna3_drm_device_set_vram_lost_cs(&dev, 1);
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == -RD_ECANCELED);
            CHECK(ctx_alloc(f, AMDGPU_CTX_PRIORITY_NORMAL, &new_ctx) == 0);
            g_cs_ctx = new_ctx;
            CHECK(cs_submit(f, 0x400000, 6, AMDGPU_HW_IP_DMA, 0, 0, NULL) == -RD_EINVAL);
            g_cs_ctx = 0;
            {
                union drm_amdgpu_ctx c;
                memset(&c, 0, sizeof(c));
                c.in.op = AMDGPU_CTX_OP_FREE_CTX; c.in.ctx_id = old_ctx;
                CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CTX), c) == 0);
                c.in.ctx_id = new_ctx;
                CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_CTX), c) == 0);
            }
            /* amdgpu_cs_parser_init: no live context, -EINVAL - id 0, a
             * freed id (the one VRAM loss made stale included) and one
             * past the table do not get around the refusal */
            g_cs_ctx_raw = 1;
            g_cs_ctx = 0;
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == -RD_EINVAL);
            g_cs_ctx = old_ctx;
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == -RD_EINVAL);
            g_cs_ctx = RD_MAX_CTX;
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == -RD_EINVAL);
            g_cs_ctx_raw = 0;
            g_cs_ctx = 0;
        }
        CHECK(cs_submit(f, 0x400000, 6, AMDGPU_HW_IP_DMA, 0, 0, NULL) == -RD_EINVAL);   /* not dwords */
        memset(&sc, 0, sizeof(sc));
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_CREATE, sc) == 0);
        so = sc.handle;
        CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, so, &h1) == 0);
        CHECK(h1 == 1 && last_ip == AMDGPU_HW_IP_DMA && last_nibs == 1 &&
              last_ibs[0].va == 0x400000 && last_ibs[0].length_dw == 16 && last_vm == &vm_dummy);
        /* XNU_SYNCOBJ_FENCE (libdrm_xnu's cross-process sync_file): the
         * fence by value; SET only accepts a fence that was submitted */
        {
            struct rd_xnu_syncobj_fence x;
            uint32_t s3;
            memset(&x, 0, sizeof(x));
            x.handle = so;
            CHECK(IOCTL(f, RD_NR_XNU_SYNCOBJ_FENCE, x) == 0 && !x.signaled &&
                  x.ip == AMDGPU_HW_IP_DMA && x.ring == 0 && x.seq == 1);
            memset(&sc, 0, sizeof(sc));
            CHECK(IOCTL(f, RD_NR_SYNCOBJ_CREATE, sc) == 0);
            s3 = sc.handle;
            x.handle = s3; x.op = RD_XNU_FENCE_SET;
            x.seq = 2;                                   /* never submitted */
            CHECK(IOCTL(f, RD_NR_XNU_SYNCOBJ_FENCE, x) == -RD_EINVAL);
            x.seq = 1; x.ip = AMDGPU_HW_IP_GFX;          /* no such ring */
            CHECK(IOCTL(f, RD_NR_XNU_SYNCOBJ_FENCE, x) == -RD_EINVAL);
            x.ip = AMDGPU_HW_IP_DMA;
            CHECK(IOCTL(f, RD_NR_XNU_SYNCOBJ_FENCE, x) == 0);
            memset(&x, 0, sizeof(x));
            x.handle = s3;
            CHECK(IOCTL(f, RD_NR_XNU_SYNCOBJ_FENCE, x) == 0 && !x.signaled && x.seq == 1);
            x.op = 7;
            CHECK(IOCTL(f, RD_NR_XNU_SYNCOBJ_FENCE, x) == -RD_EINVAL);
            x.op = RD_XNU_FENCE_SET; x.signaled = 1;
            CHECK(IOCTL(f, RD_NR_XNU_SYNCOBJ_FENCE, x) == 0);
            x.op = RD_XNU_FENCE_GET; x.signaled = 0;
            CHECK(IOCTL(f, RD_NR_XNU_SYNCOBJ_FENCE, x) == 0 && x.signaled && !x.seq);
            x.handle = 999;
            CHECK(IOCTL(f, RD_NR_XNU_SYNCOBJ_FENCE, x) == -RD_EINVAL);
            {
                struct drm_syncobj_destroy d = { s3, 0 };
                CHECK(IOCTL(f, RD_NR_SYNCOBJ_DESTROY, d) == 0);
            }
            /* WAIT_CS on a sequence that was never submitted: -EINVAL,
             * not a wait until the hang cap */
            memset(&w, 0, sizeof(w));
            w.in.handle = 5; w.in.ip_type = AMDGPU_HW_IP_DMA; w.in.timeout = 1;
            CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_WAIT_CS), w) == -RD_EINVAL);
        }
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
        /* PR #2 review: a fence that was never submitted is -EINVAL on
         * FENCE_TO_HANDLE and as a CS dependency, as amdgpu_ctx_get_fence
         * (a wait on it ran into the hang cap and reset other clients) */
        {
            union drm_amdgpu_fence_to_handle bad = fth;
            struct drm_amdgpu_cs_chunk_dep dep;
            bad.in.fence.seq_no = 1000;
            CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_FENCE_TO_HANDLE), bad) == -RD_EINVAL);
            memset(&dep, 0, sizeof(dep));
            dep.ip_type = AMDGPU_HW_IP_DMA; dep.handle = 1000;
            g_dep = &dep;
            CHECK(cs_submit(f, 0x500000, 32, AMDGPU_HW_IP_DMA, 0, 0, NULL) == -RD_EINVAL);
            dep.handle = 2;
            CHECK(cs_submit(f, 0x500000, 32, AMDGPU_HW_IP_DMA, 0, 0, NULL) == 0);
            g_dep = NULL;
        }
        sw.handles = (uint64_t)(uintptr_t)&so2; sw.timeout_nsec = 0;
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_WAIT, sw) == -RD_ETIME);   /* poll only */
        sw.timeout_nsec = INT64_MAX;
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_WAIT, sw) == 0 && ring_done == 3);
        /* PR #2 follow-up review: wait-any on two fences of a hung GPU
         * slices for 10 s, then waits on one fence with the full deadline,
         * so the OS's hang cap and recovery run */
        {
            struct drm_syncobj_create c1, c2;
            uint32_t two[2];
            memset(&c1, 0, sizeof(c1)); memset(&c2, 0, sizeof(c2));
            CHECK(IOCTL(f, RD_NR_SYNCOBJ_CREATE, c1) == 0 && IOCTL(f, RD_NR_SYNCOBJ_CREATE, c2) == 0);
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, c1.handle, NULL) == 0 &&
                  cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, c2.handle, NULL) == 0);
            two[0] = c1.handle; two[1] = c2.handle;
            h_hang = 1; h_clock = 5; h_slices = 0; h_full_waits = 0;
            dev.ops.now_ns = h_now;
            memset(&sw, 0, sizeof(sw));
            sw.handles = (uint64_t)(uintptr_t)two; sw.count_handles = 2; sw.timeout_nsec = INT64_MAX;
            CHECK(IOCTL(f, RD_NR_SYNCOBJ_WAIT, sw) == -RD_ETIME && h_full_waits == 1 &&
                  h_slices >= 10000 && h_slices <= 10001);
            h_hang = 0;
            dev.ops.now_ns = NULL;
            sw.timeout_nsec = INT64_MAX; sw.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
            CHECK(IOCTL(f, RD_NR_SYNCOBJ_WAIT, sw) == 0);
            /* PR #2 review: the later fence that is done first ends the
             * wait at once, with first_signaled pointing at it */
            {
                struct drm_syncobj_create c3, c4;
                uint64_t sa = 0, sb = 0;
                memset(&c3, 0, sizeof(c3)); memset(&c4, 0, sizeof(c4));
                CHECK(IOCTL(f, RD_NR_SYNCOBJ_CREATE, c3) == 0 && IOCTL(f, RD_NR_SYNCOBJ_CREATE, c4) == 0);
                CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, c3.handle, &sa) == 0 &&
                      cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, c4.handle, &sb) == 0);
                two[0] = c3.handle; two[1] = c4.handle;
                h_only = 1; h_only_seq = sb; h_clock = 5; h_slices = 0; h_full_waits = 0;
                dev.ops.now_ns = h_now;
                memset(&sw, 0, sizeof(sw));
                sw.handles = (uint64_t)(uintptr_t)two; sw.count_handles = 2; sw.timeout_nsec = INT64_MAX;
                CHECK(IOCTL(f, RD_NR_SYNCOBJ_WAIT, sw) == 0 && sw.first_signaled == 1 &&
                      h_full_waits == 0 && h_slices <= 1);
                h_only = 0;
                dev.ops.now_ns = NULL;
                ring_done = ring_next;
            }
            sw.flags = 0; sw.count_handles = 1;
        }
        /* the syncobj goes away while the wait sleeps (another thread, with
         * the OS lock dropped): the slot must stay free afterwards */
        memset(&sc, 0, sizeof(sc));
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_CREATE, sc) == 0);
        CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, sc.handle, NULL) == 0);
        g_wait_file = f;
        g_wait_handle = sc.handle;
        h_during_wait = destroy_during_wait;
        sw.handles = (uint64_t)(uintptr_t)&sc.handle; sw.timeout_nsec = INT64_MAX;
        CHECK(IOCTL(f, RD_NR_SYNCOBJ_WAIT, sw) == 0 && h_during_wait == NULL);
        CHECK(!f->syncobjs[sc.handle - 1].used && !f->syncobjs[sc.handle - 1].signaled);
        /* the out syncobj goes away while CS waits for an in-fence: the
         * submit still goes through and nothing touches the freed slot */
        {
            uint32_t sin, sout;
            memset(&sc, 0, sizeof(sc));
            CHECK(IOCTL(f, RD_NR_SYNCOBJ_CREATE, sc) == 0);
            sin = sc.handle;
            CHECK(IOCTL(f, RD_NR_SYNCOBJ_CREATE, sc) == 0);
            sout = sc.handle;
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, sin, NULL) == 0);
            g_wait_handle = sout;
            h_during_wait = destroy_during_wait;
            g_out_first = 1;   /* validated before the wait that destroys it */
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, sin, sout, NULL) == 0 &&
                  h_during_wait == NULL);
            g_out_first = 0;
            CHECK(!f->syncobjs[sout - 1].used && !f->syncobjs[sout - 1].has_fence);
            {
                struct drm_syncobj_destroy d = { sin, 0 };
                CHECK(IOCTL(f, RD_NR_SYNCOBJ_DESTROY, d) == 0);
            }
        }
        /* user fence (AMDGPU_CHUNK_ID_FENCE): a one-page, non-userptr BO,
         * 8 bytes at an aligned offset; held until the job is done */
        {
            union drm_amdgpu_gem_create gc;
            struct drm_gem_close cl;
            uint32_t ufh, big;
            unsigned live;
            memset(&gc, 0, sizeof(gc));
            gc.in.bo_size = 4096; gc.in.domains = AMDGPU_GEM_DOMAIN_GTT;
            CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_CREATE), gc) == 0);
            ufh = gc.out.handle;
            memset(&gc, 0, sizeof(gc));
            gc.in.bo_size = 8192; gc.in.domains = AMDGPU_GEM_DOMAIN_GTT;
            CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_CREATE), gc) == 0);
            big = gc.out.handle;
            g_uf.handle = big; g_uf.offset = 0;
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == -RD_EINVAL);
            g_uf.handle = ufh; g_uf.offset = 4092;
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == -RD_EINVAL);
            g_uf.offset = 12;
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == -RD_EINVAL);
            g_uf.handle = 9999; g_uf.offset = 0;
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == -RD_EINVAL);
            g_uf.handle = ufh; g_uf.offset = 16;
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, &h2) == 0);
            CHECK(last_job.uf_bo == rdna3_drm_bo_lookup(f, ufh) && last_job.uf_offset == 16);
            CHECK(dev.uf_pending[0].bo == last_job.uf_bo &&
                  dev.uf_pending[0].fence.seq == h2 && last_job.uf_bo->refcnt == 2);
            /* GEM_CLOSE while the GPU may still write it: the BO stays */
            live = live_bos;
            memset(&cl, 0, sizeof(cl));
            cl.handle = ufh;
            CHECK(IOCTL(f, RD_NR_GEM_CLOSE, cl) == 0 && live_bos == live);
            cl.handle = big;
            CHECK(IOCTL(f, RD_NR_GEM_CLOSE, cl) == 0 && live_bos == live - 1);
            g_uf.handle = 0;
            /* the next CS after the fence passed lets it go */
            ring_done = h2;
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, &h2) == 0 && !last_job.uf_bo);
            CHECK(!dev.uf_pending[0].bo && live_bos == live - 2);
            ring_done = h2;
        }
        /* GDS / GWS / OA BOs: on-chip ranges in units, no CPU or VM
         * access; a job gets them through its BO list */
        {
            union drm_amdgpu_gem_mmap mm;
            struct drm_amdgpu_gem_va gv;
            struct drm_amdgpu_info_gds gi;
            struct drm_amdgpu_bo_list_entry ent[3];
            struct drm_amdgpu_bo_list_in bl;
            union drm_amdgpu_bo_list l;
            struct drm_gem_close cl;
            uint32_t gds, gds2, gws, oa, list, i;
            unsigned live = live_bos;

            memset(&gi, 0, sizeof(gi));
            CHECK(info(f, AMDGPU_INFO_GDS_CONFIG, &gi, sizeof(gi)) == 0 &&
                  gi.gds_total_size == 0x1000 && gi.compute_partition_size == 0x1000 &&
                  gi.gws_per_compute_partition == 64 && gi.oa_per_compute_partition == 16);
            CHECK(onchip_create(f, AMDGPU_GEM_DOMAIN_GDS, 0x100, 4, 0, &gds) == 0);
            CHECK(onchip_create(f, AMDGPU_GEM_DOMAIN_GDS, 0x80, 0x200, 0, &gds2) == 0);
            CHECK(onchip_create(f, AMDGPU_GEM_DOMAIN_GWS, 8, 0, 0, &gws) == 0);
            CHECK(onchip_create(f, AMDGPU_GEM_DOMAIN_OA, 3, 0, 0, &oa) == 0);
            CHECK(rdna3_drm_bo_lookup(f, gds)->vram_offset == 0 &&
                  rdna3_drm_bo_lookup(f, gds2)->vram_offset == 0x200 && live_bos == live);
            CHECK(onchip_create(f, AMDGPU_GEM_DOMAIN_GDS, 0x1000, 0, 0, &i) != 0);    /* no room */
            CHECK(onchip_create(f, AMDGPU_GEM_DOMAIN_OA, 17, 0, 0, &i) != 0);         /* > 16 */
            CHECK(onchip_create(f, AMDGPU_GEM_DOMAIN_GDS | AMDGPU_GEM_DOMAIN_GTT, 16, 0, 0, &i) != 0);
            CHECK(onchip_create(f, AMDGPU_GEM_DOMAIN_GWS, 1, 0, AMDGPU_GEM_CREATE_VM_ALWAYS_VALID, &i) != 0);
            memset(&mm, 0, sizeof(mm));
            mm.in.handle = gds;
            CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_MMAP), mm) == -RD_EPERM);
            memset(&gv, 0, sizeof(gv));
            gv.handle = gds; gv.operation = AMDGPU_VA_OP_MAP; gv.va_address = 0x800000;
            gv.map_size = 4096;
            CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), gv) == -RD_EINVAL);
            /* BO_HANDLES chunk: the partitions reach the job */
            ent[0].bo_handle = gds2; ent[1].bo_handle = gws; ent[2].bo_handle = oa;
            ent[0].bo_priority = ent[1].bo_priority = ent[2].bo_priority = 0;
            memset(&bl, 0, sizeof(bl));
            bl.bo_number = 3; bl.bo_info_size = sizeof(ent[0]);
            bl.bo_info_ptr = (uint64_t)(uintptr_t)ent;
            g_bl = &bl;
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == 0);
            CHECK(last_job.gds_base == 0x200 && last_job.gds_size == 0x80 &&
                  last_job.gws_base == 0 && last_job.gws_size == 8 &&
                  last_job.oa_base == 0 && last_job.oa_size == 3);
            ent[1].bo_handle = 4242;                     /* unknown BO */
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == -RD_ENOENT);
            ent[1].bo_handle = gws;
            g_bl = NULL;
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == 0 &&
                  !last_job.gds_size && !last_job.gws_size && !last_job.oa_size);
            /* a BO list keeps its on-chip BOs past GEM_CLOSE */
            memset(&l, 0, sizeof(l));
            l.in.operation = AMDGPU_BO_LIST_OP_CREATE;
            l.in.bo_number = 2; l.in.bo_info_size = sizeof(ent[0]);
            l.in.bo_info_ptr = (uint64_t)(uintptr_t)ent;   /* gds2, gws */
            CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_BO_LIST), l) == 0 && l.out.list_handle);
            list = l.out.list_handle;
            memset(&cl, 0, sizeof(cl));
            cl.handle = gds2;
            CHECK(IOCTL(f, RD_NR_GEM_CLOSE, cl) == 0);
            g_bl_handle = list;
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == 0 &&
                  last_job.gds_base == 0x200 && last_job.gds_size == 0x80 &&
                  last_job.gws_size == 8 && !last_job.oa_size);
            g_bl = &bl;                                  /* both kinds of list: no */
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == -RD_EINVAL);
            g_bl = NULL;
            /* UPDATE replaces the BOs, DESTROY drops the last reference */
            memset(&l, 0, sizeof(l));
            l.in.operation = AMDGPU_BO_LIST_OP_UPDATE; l.in.list_handle = list;
            l.in.bo_number = 1; l.in.bo_info_size = sizeof(ent[0]);
            l.in.bo_info_ptr = (uint64_t)(uintptr_t)&ent[2];   /* oa */
            CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_BO_LIST), l) == 0 && l.out.list_handle == list);
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == 0 &&
                  !last_job.gds_size && last_job.oa_size == 3);
            CHECK(onchip_create(f, AMDGPU_GEM_DOMAIN_GDS, 0x80, 0x200, 0, &i) == 0 &&
                  rdna3_drm_bo_lookup(f, i)->vram_offset == 0x200);   /* the range came back */
            cl.handle = i;
            CHECK(IOCTL(f, RD_NR_GEM_CLOSE, cl) == 0);
            memset(&l, 0, sizeof(l));
            l.in.operation = AMDGPU_BO_LIST_OP_DESTROY; l.in.list_handle = list;
            CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_BO_LIST), l) == 0);
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == -RD_ENOENT);
            g_bl_handle = 0;
            l.in.operation = AMDGPU_BO_LIST_OP_UPDATE;
            CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_BO_LIST), l) == -RD_ENOENT);
            cl.handle = gds; CHECK(IOCTL(f, RD_NR_GEM_CLOSE, cl) == 0);
            cl.handle = gws; CHECK(IOCTL(f, RD_NR_GEM_CLOSE, cl) == 0);
            cl.handle = oa;  CHECK(IOCTL(f, RD_NR_GEM_CLOSE, cl) == 0);
            for (i = 0; i < RD_GDS_SIZE / 64; i++)
                CHECK(dev.gds_map[i] == 0);
            CHECK(dev.gws_map[0] == 0 && dev.oa_map[0] == 0 && live_bos == live);
            ring_done = ring_next;
        }
        /* AMDGPU_CHUNK_ID_CP_GFX_SHADOW (amdgpu_cs_p2_shadow): flags other
         * than INIT_SHADOW are refused, the VAs reach the job unchecked
         * (0 = no shadowing), a job without the chunk carries none */
        {
            struct drm_amdgpu_cs_chunk_cp_gfx_shadow sh;
            memset(&sh, 0, sizeof(sh));
            sh.shadow_va = 0x0000400000100000ull; sh.csa_va = 0x0000400000200000ull;
            sh.gds_va = 0x0000400000300000ull;
            sh.flags = AMDGPU_CS_CHUNK_CP_GFX_SHADOW_FLAGS_INIT_SHADOW;
            g_shadow = &sh; g_shadow_dw = sizeof(sh) / 4;
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == 0);
            CHECK(last_job.shadow_va == sh.shadow_va && last_job.csa_va == sh.csa_va &&
                  last_job.gds_va == sh.gds_va && last_job.init_shadow == 1);
            sh.flags = 0;
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == 0 &&
                  last_job.shadow_va == sh.shadow_va && last_job.init_shadow == 0);
            sh.flags = 2;
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == -RD_EINVAL);
            sh.flags = AMDGPU_CS_CHUNK_CP_GFX_SHADOW_FLAGS_INIT_SHADOW | (1ull << 32);
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == -RD_EINVAL);
            /* all zero: shadowing off for this job (gfx_v11_0_ring_emit_gfx_shadow
             * with shadow_va 0) */
            memset(&sh, 0, sizeof(sh));
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == 0 &&
                  !last_job.shadow_va && !last_job.csa_va && !last_job.gds_va && !last_job.init_shadow);
            /* a short chunk reads as zeros past its end */
            sh.shadow_va = 0x1000; sh.csa_va = 0x2000; sh.gds_va = 0x3000; sh.flags = 1;
            g_shadow_dw = 2;
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == 0 &&
                  last_job.shadow_va == 0x1000 && !last_job.csa_va && !last_job.init_shadow);
            g_shadow = NULL; g_shadow_dw = 0;
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == 0 &&
                  !last_job.shadow_va && !last_job.init_shadow);
            ring_done = ring_next;
        }
        /* PR #2 review: GEM_WAIT_IDLE is busy while a job may still run */
        {
            union drm_amdgpu_gem_wait_idle wi;
            uint32_t hb = 0;
            uint64_t s1 = 0, s2 = 0;
            CHECK(onchip_create(f, AMDGPU_GEM_DOMAIN_GTT, 4096, 4096, 0, &hb) == 0);
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == 0 && ring_done < ring_next);
            memset(&wi, 0, sizeof(wi));
            wi.in.handle = hb; wi.in.timeout = 1;
            CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_WAIT_IDLE), wi) == 0 && wi.out.status == 1 &&
                  wi.out.domain == AMDGPU_GEM_DOMAIN_GTT);
            memset(&wi, 0, sizeof(wi));
            wi.in.handle = hb; wi.in.timeout = (uint64_t)INT64_MAX - 1;
            CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_WAIT_IDLE), wi) == 0 && wi.out.status == 0 &&
                  ring_done == ring_next);
            memset(&wi, 0, sizeof(wi));
            wi.in.handle = hb; wi.in.timeout = 1;
            CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_WAIT_IDLE), wi) == 0 && wi.out.status == 0);
            wi.in.handle = 9999;
            CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_WAIT_IDLE), wi) == -RD_ENOENT);
            /* timeout 0: a look, no wait */
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == 0 && ring_done < ring_next);
            memset(&wi, 0, sizeof(wi));
            wi.in.handle = hb; wi.in.timeout = 0;
            CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_WAIT_IDLE), wi) == 0 && wi.out.status == 1 &&
                  ring_done < ring_next);
            ring_done = ring_next;
            memset(&wi, 0, sizeof(wi));
            wi.in.handle = hb; wi.in.timeout = 0;
            CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_WAIT_IDLE), wi) == 0 && wi.out.status == 0);
            /* PR #2 review: the handles go on past the ring's 32-bit fence */
            h_ring32 = 1;
            ring_next = ring_done = 0xfffffffeull;
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, &s1) == 0);
            CHECK(cs_submit(f, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, &s2) == 0);
            CHECK(s1 == 0xffffffffull && s2 == 0x100000000ull);
            CHECK(last_job.seq_base == s1);   /* the user fence is widened past it */
            w.in.handle = s2; w.in.timeout = 1;
            CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_WAIT_CS), w) == 0 && w.out.status == 1);
            w.in.handle = s2; w.in.timeout = (uint64_t)INT64_MAX - 1;
            CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_WAIT_CS), w) == 0 && w.out.status == 0);
            w.in.handle = s1; w.in.timeout = 1;
            CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_WAIT_CS), w) == 0 && w.out.status == 0);
            w.in.handle = s2 + 1;
            CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_WAIT_CS), w) == -RD_EINVAL);   /* never submitted */
            h_ring32 = 0;
            ring_next = ring_done = s2;
            {
                struct drm_gem_close c2;
                c2.handle = hb; c2.pad = 0;
                CHECK(IOCTL(f, RD_NR_GEM_CLOSE, c2) == 0);
            }
        }
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
    /* an older UAPI's shorter struct is zero-extended like drm_ioctl does:
     * drm_amdgpu_gem_op without num_entries (16 bytes, Mesa's header) */
    {
        struct drm_amdgpu_gem_create_in ci;
        union drm_amdgpu_gem_create gc;
        struct { uint32_t handle, op; uint64_t value; } old_op;
        memset(&gc, 0, sizeof(gc));
        gc.in.bo_size = 4096;
        gc.in.domains = AMDGPU_GEM_DOMAIN_GTT;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_CREATE), gc) == 0);
        memset(&ci, 0, sizeof(ci));
        old_op.handle = gc.out.handle;
        old_op.op = AMDGPU_GEM_OP_GET_GEM_CREATE_INFO;
        old_op.value = (uintptr_t)&ci;
        CHECK(sizeof(old_op) == 16 && sizeof(struct drm_amdgpu_gem_op) > 16);
        CHECK(rdna3_drm_ioctl(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_OP), &old_op, sizeof(old_op)) == 0);
        CHECK(ci.bo_size == 4096 && ci.domains == AMDGPU_GEM_DOMAIN_GTT);
    }

    /* AMDGPU_INFO_FW_VERSION: the table the kext fills, MEC2 empty, bad indexes refused */
    {
        struct drm_amdgpu_info in;
        struct drm_amdgpu_info_firmware fw;
        dev.fw_version[AMDGPU_INFO_FW_GFX_MEC] = 0xae6;
        dev.fw_feature[AMDGPU_INFO_FW_GFX_MEC] = 29;
        memset(&in, 0, sizeof(in));
        in.return_pointer = (uint64_t)(uintptr_t)&fw;
        in.return_size = sizeof(fw);
        in.query = AMDGPU_INFO_FW_VERSION;
        in.query_fw.fw_type = AMDGPU_INFO_FW_GFX_MEC;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0 && fw.ver == 0xae6 && fw.feature == 29);
        in.query_fw.index = 1;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0 && fw.ver == 0);
        in.query_fw.index = 2;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == -RD_EINVAL);
        /* ASD from the table; TA by TA_FW_TYPE_PSP_* sub-index (XGMI 2 ..
         * SECUREDISPLAY 7), 0 for a TA the file lacks, others refused */
        dev.fw_version[AMDGPU_INFO_FW_ASD] = dev.fw_feature[AMDGPU_INFO_FW_ASD] = 0x21000118;
        dev.ta_version[4] = 0x17000053;   /* HDCP */
        dev.ta_version[1] = 0x21000118;   /* ASD: not a FW_TA sub-index */
        in.query_fw.fw_type = AMDGPU_INFO_FW_ASD;
        in.query_fw.index = 0;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0 && fw.ver == 0x21000118 &&
              fw.feature == 0x21000118);
        in.query_fw.fw_type = AMDGPU_INFO_FW_TA;
        in.query_fw.index = 4;
        fw.feature = 7;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0 && fw.ver == 0x17000053 && fw.feature == 0);
        in.query_fw.index = 7;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0 && fw.ver == 0);
        in.query_fw.index = 1;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == -RD_EINVAL);
        in.query_fw.index = 8;   /* XGMI_AUX: amdgpu_firmware_info has no case for it */
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == -RD_EINVAL);
    }
    /* DEV_INFO shadow_* / csa_* (gfx_v11_0_get_gfx_shadow_info): only with
     * cp_gfx_shadow - firmware new enough (ME 1505, PFP 1600, MEC 512) and,
     * as amdgpu decides it, a VF. The card's firmware qualifies, the card
     * is no VF: zeros, so RADV / radeonsi never ask for shadowing. */
    {
        struct drm_amdgpu_info_device di;
        CHECK(dev.gc_ip_version == 0x0b0003);
        dev.fw_version[AMDGPU_INFO_FW_GFX_ME] = 2540;
        dev.fw_version[AMDGPU_INFO_FW_GFX_PFP] = 2710;
        dev.fw_version[AMDGPU_INFO_FW_GFX_MEC] = 2790;
        memset(&di, 0xff, sizeof(di));
        CHECK(info(f, AMDGPU_INFO_DEV_INFO, &di, sizeof(di)) == 0);
        CHECK(di.shadow_size == 0 && di.shadow_alignment == 0 && di.csa_size == 0 &&
              di.csa_alignment == 0 && di.device_id == 0x747E);
        dev.sriov_vf = 1;
        CHECK(info(f, AMDGPU_INFO_DEV_INFO, &di, sizeof(di)) == 0);
        CHECK(di.shadow_size == 73728 && di.shadow_alignment == 256 && di.csa_size == 484 &&
              di.csa_alignment == 256);
        CHECK(dev.dev_info.shadow_size == 0);   /* answered per query, the table stays */
        dev.fw_version[AMDGPU_INFO_FW_GFX_PFP] = 1599;   /* too old even for a VF */
        CHECK(info(f, AMDGPU_INFO_DEV_INFO, &di, sizeof(di)) == 0 && di.shadow_size == 0 &&
              di.csa_size == 0);
        dev.sriov_vf = 0;
        dev.fw_version[AMDGPU_INFO_FW_GFX_PFP] = 2710;
    }

    /* AMDGPU_INFO_VBIOS: size, image from an offset, strings; none -> 0 / EINVAL */
    {
        static uint8_t rom[1024];
        struct drm_amdgpu_info in;
        struct drm_amdgpu_info_vbios vi;
        uint32_t n = 1;
        uint8_t part[16];
        memset(&in, 0, sizeof(in));
        in.query = AMDGPU_INFO_VBIOS;
        in.vbios_info.type = AMDGPU_INFO_VBIOS_SIZE;
        in.return_pointer = (uint64_t)(uintptr_t)&n;
        in.return_size = 4;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0 && n == 0);
        in.vbios_info.type = AMDGPU_INFO_VBIOS_INFO;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == -RD_EINVAL);
        for (unsigned i = 0; i < sizeof(rom); i++)
            rom[i] = (uint8_t)i;
        dev.vbios = rom;
        dev.vbios_len = sizeof(rom);
        memcpy(dev.vbios_info.vbios_pn, "113-TEST", 9);
        in.vbios_info.type = AMDGPU_INFO_VBIOS_SIZE;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0 && n == sizeof(rom));
        in.vbios_info.type = AMDGPU_INFO_VBIOS_IMAGE;
        in.vbios_info.offset = 0x100;
        in.return_pointer = (uint64_t)(uintptr_t)part;
        in.return_size = sizeof(part);
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0 && part[0] == 0x00 && part[5] == 0x05);
        in.vbios_info.offset = sizeof(rom);
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == -RD_EINVAL);
        in.vbios_info.type = AMDGPU_INFO_VBIOS_INFO;
        in.vbios_info.offset = 0;
        in.return_pointer = (uint64_t)(uintptr_t)&vi;
        in.return_size = sizeof(vi);
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0 && !strcmp((char *)vi.vbios_pn, "113-TEST"));
        dev.vbios = NULL;
        dev.vbios_len = 0;
    }

    /* XNU_SCANOUT: unsupported without the op, else the op fills it */
    {
        static struct rd_xnu_scanout so;
        CHECK(sizeof(so) <= 4096);   /* the IOUserClient's payload limit */
        CHECK(IOCTL(f, RD_NR_XNU_SCANOUT, so) == -RD_EOPNOTSUPP);
        CHECK(rdna3_drm_ioctl(f, RD_NR_XNU_SCANOUT, &so, 64) == -RD_EINVAL);
        dev.ops.scanout = h_scanout;
        so.pipe = 1;
        CHECK(IOCTL(f, RD_NR_XNU_SCANOUT, so) == 0 && so.lit && so.nrows == 4 && so.rgb[0] == 7);
        so.pipe = 9;
        CHECK(IOCTL(f, RD_NR_XNU_SCANOUT, so) == -RD_EINVAL);
        dev.ops.scanout = NULL;
    }

    /* PRIME: the export holds the BO while its exporter's file is open or
     * another file has it; then the token is dead and the BO freed */
    {
        static rdna3_drm_file f1, f2;
        struct rd_xnu_bo_share x, y;
        struct drm_gem_close cl;
        uint32_t h = 0, live0 = live_bos;
        uint64_t tok;
        CHECK(rdna3_drm_file_open(&dev, &f1) == 0 && rdna3_drm_file_open(&dev, &f2) == 0);
        CHECK(onchip_create(&f1, AMDGPU_GEM_DOMAIN_GTT, 65536, 4096, 0, &h) == 0 && h);
        memset(&x, 0, sizeof(x));
        x.handle = h;
        CHECK(IOCTL(&f1, RD_NR_XNU_BO_EXPORT, x) == 0 && x.token && x.size == 65536);
        tok = x.token;
        CHECK(IOCTL(&f1, RD_NR_XNU_BO_EXPORT, x) == 0 && x.token == tok);   /* stable */
        memset(&y, 0, sizeof(y));
        y.token = tok + 1;
        CHECK(IOCTL(&f2, RD_NR_XNU_BO_IMPORT, y) == -RD_ENOENT);
        y.token = tok;
        CHECK(IOCTL(&f2, RD_NR_XNU_BO_IMPORT, y) == 0 && y.handle && y.size == 65536);
        CHECK(rdna3_drm_bo_lookup(&f2, y.handle) == rdna3_drm_bo_lookup(&f1, h));
        x.handle = 999;
        CHECK(IOCTL(&f1, RD_NR_XNU_BO_EXPORT, x) == -RD_EINVAL);
        /* PR #2 review: one client cannot fill the device's export table */
        {
            uint32_t k, hk = 0, n_ok = 0;
            int last = 0;
            for (k = 0; k < RD_MAX_EXPORTS_PER_FILE; k++) {
                if (onchip_create(&f1, AMDGPU_GEM_DOMAIN_GTT, 4096, 4096, 0, &hk) != 0)
                    break;
                memset(&x, 0, sizeof(x));
                x.handle = hk;
                last = IOCTL(&f1, RD_NR_XNU_BO_EXPORT, x);
                n_ok += last == 0;
            }
            CHECK(n_ok == RD_MAX_EXPORTS_PER_FILE - 1 && last == -RD_ENOSPC);   /* the first one counts */
            static rdna3_drm_file f3;   /* another client still can */
            CHECK(rdna3_drm_file_open(&dev, &f3) == 0);
            CHECK(onchip_create(&f3, AMDGPU_GEM_DOMAIN_GTT, 4096, 4096, 0, &hk) == 0);
            memset(&x, 0, sizeof(x));
            x.handle = hk;
            CHECK(IOCTL(&f3, RD_NR_XNU_BO_EXPORT, x) == 0);
            rdna3_drm_file_close(&f3);
        }
        /* the exporter goes, the importer keeps the BO and the token */
        cl.handle = h; cl.pad = 0;
        CHECK(IOCTL(&f1, RD_NR_GEM_CLOSE, cl) == 0);
        rdna3_drm_file_close(&f1);
        CHECK(live_bos == live0 + 1);
        y.handle = 0;
        CHECK(IOCTL(&f2, RD_NR_XNU_BO_IMPORT, y) == 0 && y.handle);   /* the same handle again */
        /* the importer goes too: dead token, BO freed */
        rdna3_drm_file_close(&f2);
        CHECK(live_bos == live0);
        CHECK(rdna3_drm_file_open(&dev, &f1) == 0);
        y.token = tok;
        CHECK(IOCTL(&f1, RD_NR_XNU_BO_IMPORT, y) == -RD_ENOENT);
        rdna3_drm_file_close(&f1);
    }

    /* user queues (amdgpu_userq_ioctl and friends) on a device whose OS has
     * not turned them on: amdgpu without userq_funcs. The end-to-end path
     * runs in sim_navi32 on the simulated MES. */
    {
        union drm_amdgpu_userq u;
        struct drm_amdgpu_userq_signal sg;
        struct drm_amdgpu_userq_wait w;
        struct drm_amdgpu_userq_mqd_compute_gfx11 mc;
        struct drm_amdgpu_info_uq_metadata meta;
        union drm_amdgpu_gem_create gc;
        struct drm_amdgpu_info_device di;
        struct drm_amdgpu_info in;
        uint32_t h = 0, s = 0;
        CHECK(strcmp(rdna3_drm_ioctl_name(RD_NR_AMDGPU(DRM_AMDGPU_USERQ)), "AMDGPU_USERQ") == 0);
        memset(&mc, 0, sizeof(mc));
        memset(&u, 0, sizeof(u));
        u.in.op = AMDGPU_USERQ_OP_CREATE;
        u.in.ip_type = AMDGPU_HW_IP_COMPUTE;
        u.in.queue_va = 0x100000;
        u.in.queue_size = 4096;
        u.in.rptr_va = 0x200000;
        u.in.wptr_va = 0x201000;
        u.in.mqd = (uint64_t)(uintptr_t)&mc;
        u.in.mqd_size = sizeof(mc);
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_USERQ), u) == -RD_EINVAL);   /* no IP has user queues */
        u.in.op = 7;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_USERQ), u) == -RD_EINVAL);
        memset(&u, 0, sizeof(u));
        u.in.op = AMDGPU_USERQ_OP_FREE;
        u.in.queue_id = 1;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_USERQ), u) == -RD_EINVAL);   /* no such queue */
        u.in.flags = 1;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_USERQ), u) == -RD_EINVAL);   /* FREE takes the id only */
        /* doorbell BOs exist only with user queues */
        memset(&gc, 0, sizeof(gc));
        gc.in.bo_size = 4096;
        gc.in.domains = AMDGPU_GEM_DOMAIN_DOORBELL;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_CREATE), gc) == -RD_EOPNOTSUPP);
        /* SIGNAL / WAIT: the handles are checked first, then the queue */
        {
            struct drm_syncobj_create c;
            memset(&c, 0, sizeof(c));
            CHECK(IOCTL(f, RD_NR_SYNCOBJ_CREATE, c) == 0);
            s = c.handle;
        }
        memset(&sg, 0, sizeof(sg));
        sg.queue_id = 1;
        h = 9999;
        sg.syncobj_handles = (uint64_t)(uintptr_t)&h;
        sg.num_syncobj_handles = 1;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_USERQ_SIGNAL), sg) == -RD_ENOENT);   /* bad syncobj */
        sg.syncobj_handles = (uint64_t)(uintptr_t)&s;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_USERQ_SIGNAL), sg) == -RD_ENOENT);   /* no queue 1 */
        sg.bo_read_handles = (uint64_t)(uintptr_t)&h;
        sg.num_bo_read_handles = 1;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_USERQ_SIGNAL), sg) == -RD_ENOENT);   /* bad BO */
        memset(&w, 0, sizeof(w));
        w.waitq_id = 1;
        w.syncobj_handles = (uint64_t)(uintptr_t)&s;
        w.num_syncobj_handles = 1;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_USERQ_WAIT), w) == -RD_ETIME);   /* nothing submitted */
        {
            struct drm_syncobj_array a;
            memset(&a, 0, sizeof(a));
            a.handles = (uint64_t)(uintptr_t)&s;
            a.count_handles = 1;
            CHECK(IOCTL(f, RD_NR_SYNCOBJ_SIGNAL, a) == 0);
        }
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_USERQ_WAIT), w) == 0 && w.num_fences == 1);   /* counted */
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_USERQ_WAIT), w) == -RD_EINVAL);   /* no wait queue 1 */
        /* AMDGPU_INFO_UQ_FW_AREAS: the gfx shadow and CSA, also without user queues */
        memset(&meta, 0, sizeof(meta));
        memset(&in, 0, sizeof(in));
        in.return_pointer = (uint64_t)(uintptr_t)&meta;
        in.return_size = sizeof(meta);
        in.query = AMDGPU_INFO_UQ_FW_AREAS;
        in.query_hw_ip.type = AMDGPU_HW_IP_GFX;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0 && meta.gfx.shadow_size == 73728 &&
              meta.gfx.shadow_alignment == 256 && meta.gfx.csa_size == 484 && meta.gfx.csa_alignment == 256);
        in.query_hw_ip.type = AMDGPU_HW_IP_DMA;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == -RD_EINVAL);
        CHECK(info(f, AMDGPU_INFO_DEV_INFO, &di, sizeof(di)) == 0 && di.userq_ip_mask == 0);
        /* turned on, but the OS hooks are missing: still no queue */
        rdna3_drm_device_set_userq(&dev, 7, 2u << 20);
        CHECK(info(f, AMDGPU_INFO_DEV_INFO, &di, sizeof(di)) == 0 && di.userq_ip_mask == 7);
        memset(&gc, 0, sizeof(gc));
        gc.in.bo_size = 8192;
        gc.in.domains = AMDGPU_GEM_DOMAIN_DOORBELL;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_CREATE), gc) == 0 && gc.out.handle);
        CHECK(rdna3_drm_bo_lookup(f, gc.out.handle)->vram_offset == RD_DOORBELL_KERNEL_SIZE &&
              rdna3_drm_bo_lookup(f, gc.out.handle)->size == 8192);
        u.in.op = AMDGPU_USERQ_OP_CREATE;
        u.in.flags = 0;
        u.in.ip_type = AMDGPU_HW_IP_COMPUTE;
        u.in.queue_va = 0x100000;
        u.in.queue_size = 4096;
        u.in.rptr_va = 0x200000;
        u.in.wptr_va = 0x201000;
        u.in.doorbell_handle = gc.out.handle;
        u.in.mqd = (uint64_t)(uintptr_t)&mc;
        u.in.mqd_size = sizeof(mc);
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_USERQ), u) == -RD_EINVAL);
        {
            struct drm_gem_close cl = { gc.out.handle, 0 };
            CHECK(IOCTL(f, RD_NR_GEM_CLOSE, cl) == 0);
        }
        gc.in.bo_size = 4096;
        gc.in.domains = AMDGPU_GEM_DOMAIN_DOORBELL;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_GEM_CREATE), gc) == 0 &&
              rdna3_drm_bo_lookup(f, gc.out.handle)->vram_offset == RD_DOORBELL_KERNEL_SIZE);   /* reused */
        rdna3_drm_device_set_userq(&dev, 0, 0);
    }

    /* a client that exits with a job still on the ring (card run 50: Chess
     * freed its VM under a running job, which faulted and hung the GFX
     * ring): amdgpu_flush / amdgpu_vm_fini, the close waits for the job
     * before the mappings and the VM go, and takes no new ioctls meanwhile */
    {
        static rdna3_drm_file fz;
        union drm_amdgpu_gem_create gz;
        struct drm_amdgpu_gem_va vz;
        int only0 = h_only, hang0 = h_hang, r32 = h_ring32, live0, unm0;
        uint64_t pend;
        h_only = h_hang = h_ring32 = 0;
        ring_done = ring_next;
        memset(&g_uf, 0, sizeof(g_uf));
        g_bl = NULL; g_dep = NULL; g_shadow = NULL; g_bl_handle = 0;
        dev.ops.submit = h_submit;
        rdna3_drm_device_set_ring_ready(&dev, AMDGPU_HW_IP_DMA, 1);
        CHECK(rdna3_drm_file_open(&dev, &fz) == 0);
        memset(&gz, 0, sizeof(gz));
        gz.in.bo_size = 65536;
        gz.in.domains = AMDGPU_GEM_DOMAIN_GTT;
        CHECK(IOCTL(&fz, RD_NR_AMDGPU(DRM_AMDGPU_GEM_CREATE), gz) == 0);
        memset(&vz, 0, sizeof(vz));
        vz.handle = gz.out.handle; vz.operation = AMDGPU_VA_OP_MAP; vz.va_address = 0x400000;
        vz.map_size = 65536;
        CHECK(IOCTL(&fz, RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA), vz) == 0);
        CHECK(cs_submit(&fz, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == 0);
        pend = ring_next;
        CHECK(ring_done < pend);
        live0 = vm_live;
        unm0 = vm_unmaps;
        probe_vm_live = probe_vm_unmaps = -1;
        probe_file = &fz;
        h_during_wait = probe_close_wait;
        rdna3_drm_file_close(&fz);
        /* waited with everything in place, and another thread's CS got no
         * further than the door */
        CHECK(probe_vm_live == live0 && probe_vm_unmaps == unm0 && probe_ioctl == -RD_ENODEV);
        CHECK(ring_done >= pend && vm_live == live0 - 1 && vm_unmaps == unm0 + 1);
        /* a job that never finishes, not even after the OS's job-timeout
         * recovery (h_hang): the VM is left alone, not freed under it */
        CHECK(rdna3_drm_file_open(&dev, &fz) == 0);
        CHECK(cs_submit(&fz, 0x400000, 64, AMDGPU_HW_IP_DMA, 0, 0, NULL) == 0);
        live0 = vm_live;
        h_hang = 1;
        rdna3_drm_file_close(&fz);
        h_hang = 0;
        CHECK(vm_live == live0);   /* leaked, as TTM keeps BOs with pending fences */
        vm_live--;                 /* (released by hand for the totals below) */
        ring_done = ring_next;
        h_only = only0;
        h_hang = hang0;
        h_ring32 = r32;
        dev.ops.submit = NULL;
    }

    rdna3_drm_file_close(f);
    CHECK(live_bos == 0 && vm_live == 0);
    /* a closed file stays dead: no ioctl gets through (one that waited for
     * the OS lock while it closed); the tests below open it again */
    {
        struct drm_amdgpu_info dead;
        uint32_t dv = 0;
        memset(&dead, 0, sizeof(dead));
        dead.return_pointer = (uint64_t)(uintptr_t)&dv;
        dead.return_size = 4;
        dead.query = AMDGPU_INFO_ACCEL_WORKING;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), dead) == -RD_ENODEV);
        rdna3_drm_file_close(f);   /* a second close is a no-op */
    }
    CHECK(rdna3_drm_file_open(&dev, f) == 0);

    /* the video IPs (VCN 4.0 / JPEG 4.0): without the block (rdna3vcn=0)
     * the answers stay what they were; once the kext reports it up they
     * follow amdgpu_kms.c (the CS side runs in sim_navi32) */
    {
        static rdna3_drm_device vd;
        static rdna3_drm_file vf;
        struct drm_amdgpu_info in;
        struct drm_amdgpu_info_hw_ip ip;
        struct drm_amdgpu_info_video_caps caps;
        uint32_t n, t;
        rdna3_drm_device_init(&vd, &ops, &disc, 0x747E, 0xC8, 0x0, 16ull << 30, 256ull << 20, 8ull << 30);
        CHECK(rdna3_drm_file_open(&vd, &vf) == 0);
        memset(&in, 0, sizeof(in));
        for (t = AMDGPU_HW_IP_VCN_DEC; t <= AMDGPU_HW_IP_VCN_JPEG; t++) {
            in.query = AMDGPU_INFO_HW_IP_INFO;
            in.query_hw_ip.type = t;
            in.return_pointer = (uint64_t)(uintptr_t)&ip;
            in.return_size = sizeof(ip);
            memset(&ip, 0xff, sizeof(ip));
            CHECK(IOCTL(&vf, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0 && ip.available_rings == 0 &&
                  ip.hw_ip_version_major == 0 && ip.ib_start_alignment == 32);
            in.query = AMDGPU_INFO_HW_IP_COUNT;
            in.return_pointer = (uint64_t)(uintptr_t)&n;
            in.return_size = 4;
            CHECK(IOCTL(&vf, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0 && n == 0);
        }
        in.query = AMDGPU_INFO_VIDEO_CAPS;
        in.video_cap.type = AMDGPU_INFO_VIDEO_CAPS_ENCODE;
        in.return_pointer = (uint64_t)(uintptr_t)&caps;
        in.return_size = sizeof(caps);
        CHECK(IOCTL(&vf, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0 &&
              !caps.codec_info[AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_MPEG4_AVC].valid);
        CHECK(vd.accel_working == 0);
        rdna3_drm_device_set_vcn(&vd, AMD_IP_VERSION(4, 0, 0), 2, 0, 1, 0);   /* VCN0 up, JPEG not */
        CHECK(IOCTL(&vf, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0 &&
              caps.codec_info[AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_AV1].valid &&
              caps.codec_info[AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_HEVC].max_height == 4352);
        in.query = AMDGPU_INFO_HW_IP_INFO;
        in.query_hw_ip.type = AMDGPU_HW_IP_VCN_ENC;
        in.return_pointer = (uint64_t)(uintptr_t)&ip;
        in.return_size = sizeof(ip);
        CHECK(IOCTL(&vf, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0 && ip.available_rings == 1 &&
              ip.hw_ip_version_major == 4 && ip.ip_discovery_version == 0x040000 && ip.ib_size_alignment == 4);
        in.query_hw_ip.type = AMDGPU_HW_IP_VCN_JPEG;
        CHECK(IOCTL(&vf, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0 && ip.available_rings == 0 &&
              ip.hw_ip_version_major == 4 && ip.ib_size_alignment == 64);
        in.query = AMDGPU_INFO_HW_IP_COUNT;
        in.return_pointer = (uint64_t)(uintptr_t)&n;
        in.return_size = 4;
        in.query_hw_ip.type = AMDGPU_HW_IP_VCN_DEC;
        CHECK(IOCTL(&vf, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0 && n == 2);
        in.query_hw_ip.type = AMDGPU_HW_IP_VCN_JPEG;
        CHECK(IOCTL(&vf, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0 && n == 1);
        CHECK(vd.accel_working == 1 && vd.ready_rings[AMDGPU_HW_IP_VCN_ENC] == 1 &&
              vd.ready_rings[AMDGPU_HW_IP_VCN_JPEG] == 0);
        rdna3_drm_file_close(&vf);
        rdna3_drm_device_fini(&vd);
    }
    /* AMDGPU_INFO_READ_MMR_REG: amdgpu_info_ioctl + soc21_read_register */
    {
        struct drm_amdgpu_info in;
        uint32_t v[4];
        dev.gc_base0 = 0x1260;
        dev.gb_addr_config = 0x545;
        dev.ops.read_reg = h_read_reg;
        memset(&in, 0, sizeof(in));
        in.query = AMDGPU_INFO_READ_MMR_REG;
        in.return_pointer = (uint64_t)(uintptr_t)v;
        in.return_size = sizeof(v);
        in.read_mmr_reg.dword_offset = 0x1260 + 0x0da4;   /* GRBM_STATUS */
        in.read_mmr_reg.count = 1;
        in.read_mmr_reg.instance = 0xffffffff;
        memset(v, 0, sizeof(v));
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0 && v[0] == 0xA0000000u + 0x1260 + 0x0da4);
        in.read_mmr_reg.dword_offset = 0x1260 + 0x0dae;   /* GRBM_STATUS_SE2, SE3 */
        in.read_mmr_reg.count = 2;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0 && v[1] == 0xA0000000u + 0x1260 + 0x0daf);
        in.read_mmr_reg.dword_offset = 0x1260 + 0x0e24;   /* CP_CPC_STATUS .. CP_CPF_STALLED_STAT1 */
        in.read_mmr_reg.count = 4;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0 && v[3] == 0xA0000000u + 0x1260 + 0x0e27);
        in.read_mmr_reg.dword_offset = 0x1260 + 0x0625;   /* SDMA1_STATUS_REG */
        in.read_mmr_reg.count = 1;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0);
        in.read_mmr_reg.dword_offset = 0x1260 + 0x13de;   /* GB_ADDR_CONFIG from gfx.config */
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0 && v[0] == 0x545);
        in.read_mmr_reg.dword_offset = 0x1260 + 0x0da7;   /* not in the list */
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == -RD_EFAULT);
        in.read_mmr_reg.dword_offset = 0x1260 + 0x0da4;
        in.read_mmr_reg.count = 129;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == -RD_EINVAL);
        in.read_mmr_reg.count = 1;
        in.read_mmr_reg.instance = 4;                      /* SE 4 does not exist */
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == -RD_EINVAL);
        in.read_mmr_reg.instance = (2u << 8) | 1;           /* SH 2 does not exist */
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == -RD_EINVAL);
        in.read_mmr_reg.instance = (1u << 8) | 3;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0);
        in.read_mmr_reg.count = 0;                          /* nothing read, nothing copied */
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0);
        in.read_mmr_reg.count = 2;                          /* copies min(size, count * 4) */
        in.return_size = 4;
        v[1] = 0x12345678;
        CHECK(IOCTL(f, RD_NR_AMDGPU(DRM_AMDGPU_INFO), in) == 0 && v[1] == 0x12345678);
    }
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_uapi: all checks passed\n");
    return 0;
}
