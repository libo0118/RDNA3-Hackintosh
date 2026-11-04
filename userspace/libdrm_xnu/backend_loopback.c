/*
 * backend_loopback.c - drm_xnu transport that runs uapi/rdna3_drm.c in the
 * same process, with malloc'd buffer objects. Lets the real libdrm_amdgpu
 * (and later RADV's winsys) be exercised on any host, without a GPU.
 *
 * SPDX-License-Identifier: MIT
 */
#include "drm_xnu_backend.h"
#include "rdna3_drm.h"
#include "rdna3_bo_map.h"
#include "rdna3_strip.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static rdna3_drm_device g_dev;
static int g_dev_ready;

static void *lb_zalloc(void *c, size_t n) { (void)c; return calloc(1, n); }
static void lb_free(void *c, void *p, size_t n) { (void)c; (void)n; free(p); }
static int lb_bo_alloc(void *c, rdna3_bo *bo)
{
    (void)c;
    bo->backing = mmap(NULL, (size_t)bo->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return bo->backing == MAP_FAILED ? (bo->backing = NULL, -RD_ENOMEM) : 0;
}
static void lb_bo_free(void *c, rdna3_bo *bo)
{
    (void)c;
    if (!bo->userptr && !bo->foreign)   /* a userptr BO's pages are the client's */
        munmap(bo->backing, (size_t)bo->size);
}
/* RD_NR_XNU_SURFACE_BO: "IOSurface" 0x51 is 64 KiB every import shares
 * (the accelerator's VRAM on the card); others do not exist */
#define LB_SURFACE_ID   0x51u
#define LB_SURFACE_SIZE (64u << 10)
static void *lb_surface;
uint32_t lb_surface_dirty_count, lb_surface_try_count, lb_surface_mark_count[2];
/* DRIVER-PLAN N71 (c): every import of 0x51 is over allocation #5; "IOSurface"
 * 0x53 is the same memory imported READ-ONLY, shared by pid 4242 (what
 * WindowServer gets for an application's shared drawable) */
#define LB_SURFACE_RO_ID 0x53u
#define LB_SURFACE_GEN   5u
/* 0x100..0x1ff: writable aliases of 0x51 (the share table's tests need many surfaces) */
#define LB_SURFACE_ALIAS(id) ((id) >= 0x100u && (id) < 0x200u)
uint32_t lb_surface_share_count, lb_surface_share_gen;
static int lb_surface_share(void *c, void *os, uint32_t id, uint32_t plane, uint64_t gen)
{
    (void)c; (void)os;
    if ((id != LB_SURFACE_ID && !LB_SURFACE_ALIAS(id)) || plane)
        return -RD_ENOENT;
    if (gen != LB_SURFACE_GEN)
        return -RD_EPERM;
    lb_surface_share_count++;
    lb_surface_share_gen = (uint32_t)gen;
    return 0;
}
static int lb_surface_bo(void *c, void *os, rdna3_bo *bo, uint32_t id, uint32_t plane, uint32_t flags)
{
    (void)c; (void)os;
    if (id == LB_SURFACE_RO_ID && !(flags & RD_SURFACE_BO_ACCEPT_RO))
        return -RD_EPERM;   /* the kernel's old refusal of WindowServer's import */
    if ((id != LB_SURFACE_ID && id != LB_SURFACE_RO_ID && !LB_SURFACE_ALIAS(id)) || plane)
        return -RD_ENOENT;
    bo->surf_gen = LB_SURFACE_GEN;
    if (id == LB_SURFACE_RO_ID) {
        bo->gpu_ro = 1;
        bo->surf_pid = 4242;
    }
    if (!lb_surface) {
        void *p = mmap(NULL, LB_SURFACE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED)
            return -RD_ENOMEM;
        lb_surface = p;
    }
    bo->backing = lb_surface;
    bo->vram_offset = 1u << 20;
    bo->size = LB_SURFACE_SIZE;
    return 0;
}
static int lb_surface_dirty(void *c, void *os, uint32_t id, uint32_t plane)
{
    (void)c; (void)os;
    if (id != LB_SURFACE_ID)
        return -RD_ENOENT;
    if (plane & RD_XNU_SURFACE_PLANE_GPU_MARK) {   /* N41: a pending (1) / ready (0) mark */
        lb_surface_mark_count[plane & 1u]++;
        return 0;
    }
    if (plane & RD_XNU_SURFACE_PLANE_TRYLOCK) {
        plane &= ~RD_XNU_SURFACE_PLANE_TRYLOCK;
        lb_surface_try_count++;
    }
    if (plane)
        return -RD_ENOENT;
    lb_surface_dirty_count++;
    return 0;
}
/* GEM_USERPTR: in one process the client's pages are already ours */
static int lb_bo_userptr(void *c, void *os, rdna3_bo *bo, uint64_t uaddr, uint32_t flags)
{
    (void)c; (void)os; (void)flags;
    bo->backing = (void *)(uintptr_t)uaddr;
    return 0;
}
/* AMDGPU_INFO_SENSOR: what a quiet card reports (RADV's winsys asks for
 * temperature and clocks at creation) */
/* RD_NR_XNU_SCANOUT: pipe 0 "scans" a 1920x1080 test picture (blue-green
 * gradient, a white box in the middle) so rdna3_drminfo --scanout and the
 * report's picture path run on the host */
/* the test picture's pixel (x, y) of an out_w x out_h look */
static void lb_scanout_px(uint32_t x, uint32_t y, uint32_t out_w, uint32_t out_h, uint8_t *o)
{
    /* the white box covers the same part of the picture at any width */
    uint32_t bx = x * RD_XNU_SCANOUT_W / out_w, by = y * 180 / out_h;
    int box = bx >= 140 && bx < 180 && by >= 70 && by < 110;
    o[0] = box ? 255 : 0;
    o[1] = box ? 255 : (uint8_t)(y * 255 / out_h);
    o[2] = box ? 255 : (uint8_t)(x * 255 / out_w);
}

/* RDNA3_LB_STRIP=1 (tests/test_frame_order.sh, DRIVER-PLAN N59): the picture also holds the app survey's
 * frame-order strips (common/rdna3_strip.h), a css one at row 100 and a canvas one at row 200, both at x 300;
 * their counter advances every 5th scanout call and every 40th frame shows the frame three back */
static void lb_strip_overlay(struct rd_xnu_scanout *s)
{
    static unsigned calls;
    static uint8_t strip[RD3_STRIP_W * 3];
    if (!getenv("RDNA3_LB_STRIP"))
        return;
    const unsigned frame = calls++ / 5, shown = frame % 40 == 39 ? frame - 3 : frame;
    for (uint32_t r = 0; r < s->nrows; r++) {
        const uint32_t y = s->row0 + r;
        int layer = y >= 100 && y < 100 + RD3_STRIP_H ? RD3_SL_CSS : y >= 200 && y < 200 + RD3_STRIP_H ? RD3_SL_CANVAS : -1;
        if (layer < 0)
            continue;
        rd3_strip_paint(strip, layer, layer == RD3_SL_CSS ? shown % RD3_STRIP_CSS_MOD : shown % RD3_STRIP_MOD);
        for (uint32_t x = 0; x < s->ncols; x++) {
            const uint32_t sx = s->col0 + x;
            if (sx >= 300 && sx < 300 + RD3_STRIP_W)
                memcpy(s->rgb + (r * s->ncols + x) * 3, strip + (sx - 300) * 3, 3);
        }
    }
}

static int lb_scanout(void *c, struct rd_xnu_scanout *s)
{
    uint32_t r, x, per_call, out_w = RD_XNU_SCANOUT_W;
    (void)c;
    if (s->pipe > 3)
        return -RD_EINVAL;
    if (s->pipe != 0) {
        s->powered = 0;
        return 0;
    }
    s->powered = s->lit = 1;
    s->format = 8;
    s->width = s->pitch_px = 1920;
    s->height = 1080;
    s->address = s->inuse = 0x8000000000ull;
    s->det_segs = 18;
    s->dispclk_khz = s->dppclk_khz = 165384;
    /* the kext's RDNA3Accel::drmScanout: RD_XNU_SCANOUT_NATIVE copies the
     * surface's own pixels, one rd_xnu_scanout_native_seg() per call;
     * RDNA3_LB_SCANOUT_V1=1 plays a kext from before it (ignores the flag,
     * abi stays 0) for the host test of rdna3_drminfo's fallback */
    if ((s->flags & RD_XNU_SCANOUT_NATIVE) && !getenv("RDNA3_LB_SCANOUT_V1")) {
        s->out_w = s->width;
        s->out_h = s->height;
        s->abi = RD_XNU_SCANOUT_ABI;
        rd_xnu_scanout_native_seg(s->width, s->height, s->row0, s->col0, &s->nrows, &s->ncols);
        for (r = 0; r < s->nrows; r++)
            for (x = 0; x < s->ncols; x++)
                lb_scanout_px(s->col0 + x, s->row0 + r, s->out_w, s->out_h, s->rgb + (r * s->ncols + x) * 3);
        lb_strip_overlay(s);
        return 0;
    }
    /* RD_XNU_SCANOUT_HIRES samples RD_XNU_SCANOUT_HIRES_W wide (at most
     * the surface), as many rows per call as rgb[] holds */
    if ((s->flags & RD_XNU_SCANOUT_HIRES) && s->width > RD_XNU_SCANOUT_W)
        out_w = s->width < RD_XNU_SCANOUT_HIRES_W ? s->width : RD_XNU_SCANOUT_HIRES_W;
    per_call = (uint32_t)(sizeof(s->rgb) / ((size_t)out_w * 3));
    s->out_w = out_w;
    s->out_h = 1080 * out_w / 1920;
    s->nrows = s->row0 < s->out_h ? s->out_h - s->row0 : 0;
    if (s->nrows > per_call)
        s->nrows = per_call;
    for (r = 0; r < s->nrows; r++)
        for (x = 0; x < s->out_w; x++)
            lb_scanout_px(x, s->row0 + r, s->out_w, s->out_h, s->rgb + (r * s->out_w + x) * 3);
    return 0;
}

/* RD_NR_XNU_TXSTATE: pipe 0's head as a clean HDMI output (what
 * rdna3_drminfo --scanout prints on the host) */
static int lb_txstate(void *c, struct rd_xnu_txstate *t)
{
    (void)c;
    if (t->pipe > 3)
        return -RD_EINVAL;
    t->valid = t->pipe == 0;
    t->problems = t->changes = 0;
    snprintf(t->text, sizeof(t->text), "%s",
             t->valid ? "head 0: OTG0 -> DIGD -> UNIPHYD\nproblems: none\nOTG0: OTG_CONTROL 0x10201 V_TOTAL 0x464\n"
                        "watch: 3 tick(s), 0 change(s), 0 busy skip(s)\nsince the start: unchanged\n"
                      : "");
    return 0;
}

static int lb_sensor(void *c, uint32_t type, uint32_t *v)
{
    (void)c;
    switch (type) {
    case AMDGPU_INFO_SENSOR_GFX_SCLK: *v = 500; return 0;
    case AMDGPU_INFO_SENSOR_GFX_MCLK: *v = 96; return 0;
    case AMDGPU_INFO_SENSOR_GPU_TEMP: *v = 35000; return 0;
    case AMDGPU_INFO_SENSOR_GPU_LOAD: *v = 0; return 0;
    case AMDGPU_INFO_SENSOR_GPU_AVG_POWER: *v = 12; return 0;
    case AMDGPU_INFO_SENSOR_VDDGFX: *v = 650; return 0;
    case AMDGPU_INFO_SENSOR_STABLE_PSTATE_GFX_SCLK:
    case AMDGPU_INFO_SENSOR_PEAK_PSTATE_GFX_SCLK: *v = 2124; return 0;
    case AMDGPU_INFO_SENSOR_STABLE_PSTATE_GFX_MCLK:
    case AMDGPU_INFO_SENSOR_PEAK_PSTATE_GFX_MCLK: *v = 1218; return 0;
    default: return -RD_EINVAL;
    }
}
/* DRM_AMDGPU_SCHED / high context priorities: root, as the kext */
static int lb_privileged(void *c) { (void)c; return geteuid() == 0; }
static int lb_copyin(void *c, void *d, uint64_t u, size_t n) { (void)c; memcpy(d, (void *)(uintptr_t)u, n); return 0; }
static int lb_copyout(void *c, uint64_t u, const void *s, size_t n) { (void)c; memcpy((void *)(uintptr_t)u, s, n); return 0; }

/* ---- a software DMA ring: page-table-free VMs and an SDMA 6.0 packet
 * interpreter, so libdrm_amdgpu's CS path runs end to end on any host ---- */
#define LB_MAX_MAPS 1024
struct lb_map { uint64_t va, size; uint8_t *host; };
struct lb_vm { struct lb_map m[LB_MAX_MAPS]; unsigned n; };
static uint64_t g_seq;

static void *lb_vm_create(void *c) { (void)c; return calloc(1, sizeof(struct lb_vm)); }
static void lb_vm_destroy(void *c, void *vm) { (void)c; free(vm); }
static int lb_vm_map(void *c, void *p, rdna3_bo *bo, uint64_t va, uint64_t off, uint64_t size,
                     uint32_t flags)
{
    struct lb_vm *vm = (struct lb_vm *)p;
    unsigned i;
    (void)c; (void)flags;
    for (i = 0; i < vm->n && vm->m[i].size; i++)
        ;
    if (i == LB_MAX_MAPS)
        return -RD_ENOMEM;
    vm->m[i].va = va;
    vm->m[i].size = size;
    vm->m[i].host = (uint8_t *)bo->backing + off;
    if (i == vm->n)
        vm->n++;
    return 0;
}
static void lb_vm_unmap(void *c, void *p, uint64_t va, uint64_t size)
{
    struct lb_vm *vm = (struct lb_vm *)p;
    unsigned i;
    (void)c;
    for (i = 0; i < vm->n; i++)
        if (vm->m[i].size && vm->m[i].va == va && vm->m[i].size == size)
            vm->m[i].size = 0;
}
/* the GPU's 48-bit view: GEM_VA keeps the mappings with the hole masked
 * off (AMDGPU_GMC_HOLE_MASK), an IB or a packet may name the canonical
 * high-half address of the same page */
static uint8_t *lb_va(struct lb_vm *vm, uint64_t va, uint64_t len)
{
    unsigned i;
    va &= 0x0000ffffffffffffull;
    for (i = 0; i < vm->n; i++)
        if (vm->m[i].size && va >= vm->m[i].va && va + len <= vm->m[i].va + vm->m[i].size)
            return vm->m[i].host + (va - vm->m[i].va);
    return NULL;
}
static int lb_run_ib(struct lb_vm *vm, const uint32_t *ib, uint32_t ndw)
{
    uint32_t p = 0;
    while (p < ndw) {
        uint32_t h = ib[p], op = h & 0xff, sub = (h >> 8) & 0xff;
        if (op == 0) {                                       /* NOP */
            p += 1 + ((h >> 16) & 0x3fff);
        } else if (op == 2 && sub == 0 && p + 4 <= ndw) {    /* WRITE_LINEAR */
            uint64_t va = ib[p + 1] | ((uint64_t)ib[p + 2] << 32);
            uint32_t cnt = (ib[p + 3] & 0xfffff) + 1;
            uint8_t *dst = lb_va(vm, va, 4ull * cnt);
            if (!dst || p + 4 + cnt > ndw)
                return -RD_EFAULT;
            memcpy(dst, &ib[p + 4], 4ull * cnt);
            p += 4 + cnt;
        } else if (op == 1 && sub == 0 && p + 7 <= ndw) {    /* COPY_LINEAR */
            uint32_t len = ib[p + 1] + 1;
            uint64_t s = ib[p + 3] | ((uint64_t)ib[p + 4] << 32);
            uint64_t d = ib[p + 5] | ((uint64_t)ib[p + 6] << 32);
            uint8_t *ps = lb_va(vm, s, len), *pd = lb_va(vm, d, len);
            if (!ps || !pd)
                return -RD_EFAULT;
            memmove(pd, ps, len);
            p += 7;
        } else if (op == 11 && p + 5 <= ndw) {               /* CONST_FILL */
            uint64_t d = ib[p + 1] | ((uint64_t)ib[p + 2] << 32);
            uint32_t v = ib[p + 3], len = ib[p + 4] + 1, i;
            uint8_t *pd = lb_va(vm, d, len);
            if (!pd)
                return -RD_EFAULT;
            for (i = 0; i + 4 <= len; i += 4)
                memcpy(pd + i, &v, 4);
            p += 5;
        } else if (op == 5 && p + 4 <= ndw) {                /* FENCE */
            uint64_t va = ib[p + 1] | ((uint64_t)ib[p + 2] << 32);
            uint8_t *dst = lb_va(vm, va, 4);
            if (!dst)
                return -RD_EFAULT;
            memcpy(dst, &ib[p + 3], 4);
            p += 4;
        } else {
            return -RD_EINVAL;
        }
    }
    return 0;
}
/* GFX/compute jobs are not executed, but their PM4 framing is: every
 * packet must fit in its IB, and every INDIRECT_BUFFER (RADV chains its
 * command streams) must point at mapped memory of this VM, walked to a
 * small depth. Catches broken packets and missing VA mappings on the host. */
static int lb_pm4_walk(struct lb_vm *vm, uint64_t va, uint32_t ndw, int depth, uint32_t *npkt)
{
    const uint32_t *ib = (const uint32_t *)lb_va(vm, va, 4ull * ndw);
    uint32_t p = 0;
    if (!ib)
        return -RD_EFAULT;
    if (depth > 8)
        return 0;
    while (p < ndw) {
        uint32_t h = ib[p], type = h >> 30, len;
        if (type == 2 || h == 0xffff1000u) {               /* filler, 1-dword NOP (PKT3_NOP_PAD) */
            p++;
            continue;
        }
        len = ((h >> 16) & 0x3fff) + 2;                    /* type 0 / 3 */
        if (type == 1 || p + len > ndw) {
            fprintf(stderr, "loopback: PM4 dword %u of %u at 0x%llx: header 0x%08x\n", p, ndw,
                    (unsigned long long)va, h);
            if (getenv("RDNA3_LB_TRACE")) {
                uint32_t q;
                for (q = 0; q < ndw; q++)
                    fprintf(stderr, "  [%3u] 0x%08x\n", q, ib[q]);
            }
            return -RD_EINVAL;
        }
        if (type == 3 && ((h >> 8) & 0xff) == 0x3f && len >= 4) {   /* INDIRECT_BUFFER */
            uint64_t cva = (ib[p + 1] & ~3u) | ((uint64_t)(ib[p + 2] & 0xffff) << 32);
            uint32_t cdw = ib[p + 3] & 0xfffff;
            int r;
            r = cdw ? lb_pm4_walk(vm, cva, cdw, depth + 1, npkt) : 0;
            if (r)
                return r;
        }
        (*npkt)++;
        p += len;
    }
    return 0;
}

static int lb_submit(void *c, void *p, uint32_t ip, uint32_t ring, const rdna3_ib *ibs, uint32_t n,
                     const rdna3_cs_job *job, uint64_t *seq)
{
    struct lb_vm *vm = (struct lb_vm *)p;
    uint32_t i;
    (void)c;
    /* RDNA3_LB_GFX_NOP=1: GFX/compute jobs are accepted and complete without
     * running (there is no shader core here), so a client's whole CS path -
     * chunks, BO lists, syncobjs, fences - can be exercised on the host */
    if ((ip == AMDGPU_HW_IP_GFX || ip == AMDGPU_HW_IP_COMPUTE) && !ring && getenv("RDNA3_LB_GFX_NOP")) {
        uint32_t npkt = 0;
        for (i = 0; i < n; i++) {
            int r = lb_pm4_walk(vm, ibs[i].va, ibs[i].length_dw, 0, &npkt);
            if (r) {
                fprintf(stderr, "loopback: %s IB %u at 0x%llx (%u dw): bad PM4 framing or unmapped VA (%d)\n",
                        ip == AMDGPU_HW_IP_GFX ? "GFX" : "compute", i, (unsigned long long)ibs[i].va,
                        ibs[i].length_dw, r);
                return r;
            }
        }
        if (getenv("RDNA3_LB_TRACE"))
            fprintf(stderr, "loopback: %s job, %u IB(s), %u PM4 packets\n",
                    ip == AMDGPU_HW_IP_GFX ? "GFX" : "compute", n, npkt);
        *seq = ++g_seq;
        if (job->uf_bo)
            memcpy((uint8_t *)job->uf_bo->backing + job->uf_offset, seq, 8);
        return 0;
    }
    if (ip != AMDGPU_HW_IP_DMA || ring)
        return -RD_EINVAL;
    for (i = 0; i < n; i++) {
        const uint32_t *ib = (const uint32_t *)lb_va(vm, ibs[i].va, 4ull * ibs[i].length_dw);
        int r;
        if (!ib)
            return -RD_EFAULT;
        r = lb_run_ib(vm, ib, ibs[i].length_dw);
        if (r)
            return r;
    }
    *seq = ++g_seq;   /* runs synchronously: done as soon as it is queued */
    if (job->uf_bo)   /* the user fence write the GPU does after the job */
        memcpy((uint8_t *)job->uf_bo->backing + job->uf_offset, seq, 8);
    return 0;
}
static int lb_signaled(void *c, uint32_t ip, uint32_t ring, uint64_t seq)
{
    (void)c; (void)ip; (void)ring;
    return seq <= g_seq;
}
static int lb_wait(void *c, uint32_t ip, uint32_t ring, uint64_t seq, uint64_t abs_ns)
{
    (void)abs_ns;
    return lb_signaled(c, ip, ring, seq) ? 0 : -RD_ETIME;
}

static uint64_t lb_random64(void *c)
{
    uint64_t v = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    (void)c;
    if (f) {
        if (fread(&v, sizeof(v), 1, f) != 1)
            v = 0;
        fclose(f);
    }
    return v;
}

static uint64_t lb_now(void *c)
{
    struct timespec ts;
    (void)c;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void dev_init(void)
{
    rdna3_drm_ops ops = { NULL, lb_zalloc, lb_free, lb_bo_alloc, lb_bo_free, lb_copyin, lb_copyout, NULL, NULL,
                          lb_vm_create, lb_vm_destroy, lb_vm_map, lb_vm_unmap,
                          lb_submit, lb_signaled, lb_wait, NULL,
                          lb_bo_userptr, lb_sensor, NULL, lb_random64, lb_scanout, lb_privileged,
                          NULL, NULL, NULL, NULL, NULL, NULL, NULL, lb_surface_bo, lb_surface_dirty, NULL,
                          lb_txstate, NULL, NULL, NULL };
    amd_discovery disc;
    if (g_dev_ready)
        return;
    ops.now_ns = lb_now;   /* wait-any slices its waits by it */
    ops.surface_share = lb_surface_share;   /* N71 (c) */
    memset(&disc, 0, sizeof(disc));
    disc.has_gc_info = 1;
    disc.gc.num_se = 3; disc.gc.num_sa_per_se = 2; disc.gc.num_wgp0_per_sa = 5;
    disc.gc.num_rb_per_se = 4; disc.gc.wave_size = 32; disc.gc.num_gl2c = 16;
    disc.gc.num_gprs = 1536; disc.gc.gl2c_per_gpu = 4u << 20;
    disc.num_ips = 2;
    disc.ips[0].hw_id = AMD_HWID_GC; disc.ips[0].major = 11; disc.ips[0].revision = 3;
    disc.ips[1].hw_id = AMD_HWID_SDMA0; disc.ips[1].major = 6; disc.ips[1].revision = 3;
    rdna3_drm_device_init(&g_dev, &ops, &disc, 0x747E, 0xC8, 0, 16ull << 30, 256ull << 20, 8ull << 30);
    /* Explicit synthetic Navi32 fixture, never a production-device default. */
    rdna3_drm_device_set_asic_info(&g_dev, 100000, AMDGPU_VRAM_TYPE_GDDR6, 256, 64ull << 20);
    rdna3_drm_device_set_enumeration_only(&g_dev, 1);
    rdna3_drm_device_set_ring_ready(&g_dev, AMDGPU_HW_IP_DMA, 1);   /* software SDMA */
    if (getenv("RDNA3_LB_GFX_NOP")) {   /* GFX/compute jobs accepted, not run (lb_submit) */
        rdna3_drm_device_set_ring_ready(&g_dev, AMDGPU_HW_IP_GFX, 1);
        rdna3_drm_device_set_ring_ready(&g_dev, AMDGPU_HW_IP_COMPUTE, 1);
    }
    /* placeholder until read from a real Navi 32 (RDNA3,GBAddrConfig in ioreg) */
    g_dev.gb_addr_config = 0x00000545;
    g_dev.gc_base0 = 0x1260;
    g_dev_ready = 1;
}

static uintptr_t lb_open(void)
{
    rdna3_drm_file *f;
    dev_init();
    f = calloc(1, sizeof(*f));
    if (!f || rdna3_drm_file_open(&g_dev, f)) {
        free(f);
        errno = ENOMEM;
        return 0;
    }
    return (uintptr_t)f;
}

static void lb_close(uintptr_t c)
{
    rdna3_drm_file_close((rdna3_drm_file *)c);
    free((void *)c);
}

static int lb_ioctl(uintptr_t c, uint32_t nr, void *arg, uint32_t size)
{
    int r = rdna3_drm_ioctl((rdna3_drm_file *)c, nr, arg, size);
    return r < 0 ? -r : 0;
}

/* the kernel's checks (common/rdna3_bo_map.h: no userptr, no BO without
 * CPU access; no BAR limits here) */
static void *lb_map(uintptr_t c, uint64_t off, size_t len)
{
    rdna3_bo *bo = rdna3_drm_bo_by_mmap_offset((rdna3_drm_file *)c, off);
    enum rd3_bomap_kind k = rd3_bomap_classify(bo, UINT64_MAX, UINT64_MAX);
    if (!bo || rd3_bomap_refused(k) || len > bo->size) {
        errno = EINVAL;
        return NULL;
    }
    return bo->backing;   /* the BO's own pages: CPU view == "GPU" view */
}

/* as the kext: the offset must still name the mapped BO of this file
 * (IOConnectUnmapMemory64 finds the mapping through it), so a caller that
 * passes the wrong one, or closes the BO first, fails here too */
uint32_t lb_unmap_refused;   /* host tests: every unmap must find its BO */
static int lb_unmap(uintptr_t c, void *a, size_t l, uint64_t off)
{
    rdna3_bo *bo = rdna3_drm_bo_by_mmap_offset((rdna3_drm_file *)c, off);
    (void)l;
    if (!bo || bo->backing != a) {
        lb_unmap_refused++;
        fprintf(stderr, "loopback: unmap of %p with offset 0x%llx names no mapped BO of this file\n", a,
                (unsigned long long)off);
        return -1;
    }
    return 0;
}

static int lb_pci_info(uint16_t *vendor, uint16_t *device, uint16_t *subvendor,
                       uint16_t *subdevice, uint8_t *revision, uint16_t *domain,
                       uint8_t *bus, uint8_t *dev, uint8_t *func)
{
    *vendor = 0x1002; *device = 0x747E; *subvendor = 0x1002; *subdevice = 0x0E3B;
    *revision = 0xC8; *domain = 0; *bus = 3; *dev = 0; *func = 0;
    return 0;
}

const drm_xnu_backend drm_xnu_backend_default = {
    lb_open, lb_close, lb_ioctl, lb_map, lb_unmap, lb_pci_info,
};
