/*
 * backend_loopback.c - drm_xnu transport that runs uapi/rdna3_drm.c in the
 * same process, with malloc'd buffer objects. Lets the real libdrm_amdgpu
 * (and later RADV's winsys) be exercised on any host, without a GPU.
 *
 * SPDX-License-Identifier: MIT
 */
#include "drm_xnu_backend.h"
#include "rdna3_drm.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

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
static void lb_bo_free(void *c, rdna3_bo *bo) { (void)c; munmap(bo->backing, (size_t)bo->size); }
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
static uint8_t *lb_va(struct lb_vm *vm, uint64_t va, uint64_t len)
{
    unsigned i;
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
static int lb_submit(void *c, void *p, uint32_t ip, uint32_t ring, const rdna3_ib *ibs, uint32_t n,
                     uint64_t *seq)
{
    struct lb_vm *vm = (struct lb_vm *)p;
    uint32_t i;
    (void)c;
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

static void dev_init(void)
{
    rdna3_drm_ops ops = { NULL, lb_zalloc, lb_free, lb_bo_alloc, lb_bo_free, lb_copyin, lb_copyout, NULL, NULL,
                          lb_vm_create, lb_vm_destroy, lb_vm_map, lb_vm_unmap,
                          lb_submit, lb_signaled, lb_wait, NULL };
    amd_discovery disc;
    if (g_dev_ready)
        return;
    memset(&disc, 0, sizeof(disc));
    disc.has_gc_info = 1;
    disc.gc.num_se = 3; disc.gc.num_sa_per_se = 2; disc.gc.num_wgp0_per_sa = 5;
    disc.gc.num_rb_per_se = 4; disc.gc.wave_size = 32; disc.gc.num_gl2c = 16;
    disc.gc.num_gprs = 1536; disc.gc.gl2c_per_gpu = 4u << 20;
    disc.num_ips = 2;
    disc.ips[0].hw_id = AMD_HWID_GC; disc.ips[0].major = 11; disc.ips[0].revision = 3;
    disc.ips[1].hw_id = AMD_HWID_SDMA0; disc.ips[1].major = 6; disc.ips[1].revision = 3;
    rdna3_drm_device_init(&g_dev, &ops, &disc, 0x747E, 0xC8, 0, 16ull << 30, 256ull << 20, 8ull << 30);
    rdna3_drm_device_set_enumeration_only(&g_dev, 1);
    rdna3_drm_device_set_ring_ready(&g_dev, AMDGPU_HW_IP_DMA, 1);   /* software SDMA */
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

static void *lb_map(uintptr_t c, uint64_t off, size_t len)
{
    rdna3_bo *bo = rdna3_drm_bo_by_mmap_offset((rdna3_drm_file *)c, off);
    if (!bo || len > bo->size) {
        errno = EINVAL;
        return NULL;
    }
    return bo->backing;   /* the BO's own pages: CPU view == "GPU" view */
}

static int lb_unmap(uintptr_t c, void *a, size_t l) { (void)c; (void)a; (void)l; return 0; }

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
