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

static void dev_init(void)
{
    rdna3_drm_ops ops = { NULL, lb_zalloc, lb_free, lb_bo_alloc, lb_bo_free, lb_copyin, lb_copyout, NULL, NULL,
                          NULL, NULL, NULL, NULL };
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
