/*
 * rdna3_drm.c - see rdna3_drm.h. Behaviour follows amdgpu_kms.c,
 * amdgpu_gem.c, amdgpu_ctx.c and drm_syncobj.c (MIT / GPL-2.0 OR MIT)
 * where it matters to userspace; the implementation is our own.
 *
 * SPDX-License-Identifier: MIT
 */
#include "rdna3_drm.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static void zero(void *p, size_t n)
{
    volatile uint8_t *q = (volatile uint8_t *)p;
    while (n--)
        *q++ = 0;
}

static void copy(void *d, const void *s, size_t n)
{
    uint8_t *dd = (uint8_t *)d;
    const uint8_t *ss = (const uint8_t *)s;
    while (n--)
        *dd++ = *ss++;
}

static size_t slen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

/* ------------------------------ device ------------------------------ */

/* 48-bit GPUVM (gmc_v11_0: amdgpu_vm_adjust_size(..., 48), max_level 3) */
#define VA_RESERVED_BOTTOM  (1ull << 16)
#define VA_RESERVED_TOP     ((1ull << 16) + (2ull << 20) + (2ull << 20))
#define GMC_HOLE_START      0x0000800000000000ull
#define GMC_HOLE_END        0xffff800000000000ull
#define VM_SIZE             ((1ull << 48) - VA_RESERVED_TOP)

void rdna3_drm_device_init(rdna3_drm_device *dev, const rdna3_drm_ops *ops,
                           const amd_discovery *disc, uint16_t pci_device,
                           uint8_t pci_revision, uint32_t rev_id,
                           uint64_t vram_size, uint64_t vis_vram_size,
                           uint64_t gtt_size)
{
    struct drm_amdgpu_info_device *di = &dev->dev_info;
    const amd_gc_info *gc = &disc->gc;
    const amd_ip_entry *ip;
    uint32_t se, sa, cu_per_sa, i, j;

    zero(dev, sizeof(*dev));
    dev->ops = *ops;
    dev->vram_size = vram_size;
    dev->vis_vram_size = vis_vram_size;
    dev->gtt_size = gtt_size;
    dev->next_mmap_offset = 1ull << 32;

    di->device_id = pci_device;
    di->chip_rev = rev_id;
    di->external_rev = rev_id + 0x20;     /* soc21.c, GC 11.0.3 */
    di->pci_rev = pci_revision;
    di->family = AMDGPU_FAMILY_GC_11_0_0;

    se = disc->has_gc_info ? gc->num_se : 3;
    sa = disc->has_gc_info ? gc->num_sa_per_se : 2;
    cu_per_sa = disc->has_gc_info ? 2 * (gc->num_wgp0_per_sa + gc->num_wgp1_per_sa) : 10;
    di->num_shader_engines = se;
    di->num_shader_arrays_per_engine = sa;
    di->num_cu_per_sh = cu_per_sa;
    di->gpu_counter_freq = 100000;        /* soc21_get_xclk(): 100 MHz, in kHz */
    /* placeholders until the SMU's DPM tables or the VBIOS boot clocks
     * arrive (rdna3_drm_device_set_clocks) */
    di->max_engine_clock = di->min_engine_clock = 2430000;
    di->max_memory_clock = di->min_memory_clock = 2438000;

    /* all CUs/RBs active until the OS reads the harvesting registers
     * (rdna3_drm_device_set_cu_info). gfx_v11_0_get_cu_info fills no
     * always-on CU masks, so cu_ao_* stay 0 as on Linux. */
    di->cu_active_number = 0;
    for (i = 0; i < se && i < 4; i++)
        for (j = 0; j < sa && j < 4; j++) {
            di->cu_bitmap[i][j] = (cu_per_sa >= 32) ? 0xffffffffu : ((1u << cu_per_sa) - 1);
            di->cu_active_number += cu_per_sa;
        }
    di->cu_ao_mask = 0;
    di->num_rb_pipes = (disc->has_gc_info ? gc->num_rb_per_se : 4) * se;
    di->enabled_rb_pipes_mask = (di->num_rb_pipes >= 32) ? 0xffffffffu
                                                         : ((1u << di->num_rb_pipes) - 1);
    di->num_hw_gfx_contexts = 8;
    di->ids_flags = AMDGPU_IDS_FLAGS_GANG_SUBMIT;

    di->virtual_address_offset = VA_RESERVED_BOTTOM;
    di->virtual_address_max = MIN(VM_SIZE, GMC_HOLE_START);
    di->high_va_offset = GMC_HOLE_END;
    di->high_va_max = GMC_HOLE_END | VM_SIZE;
    di->virtual_address_alignment = 4096;
    di->pte_fragment_size = (1u << 9) * 4096;   /* fragment_size 9 */
    di->gart_page_size = 4096;

    di->vram_type = AMDGPU_VRAM_TYPE_GDDR6;
    di->vram_bit_width = 256;                    /* Navi 32: 4 MCDs x 64 bit */
    if (disc->has_gc_info) {
        di->gc_double_offchip_lds_buf = gc->double_offchip_lds_buffer;
        di->wave_front_size = gc->wave_size;
        di->num_shader_visible_vgprs = gc->num_gprs;
        di->num_tcc_blocks = gc->num_gl2c;
        di->gs_vgt_table_depth = gc->gs_table_depth;
        di->gs_prim_buffer_depth = gc->gsprim_buff_depth;
        di->max_gs_waves_per_vgt = gc->num_max_gs_thds;
        di->tcp_cache_size = gc->tcp_l1_size;
        di->num_sqc_per_wgp = gc->num_sqc_per_wgp;
        di->sqc_data_cache_size = gc->l1_data_cache_size_per_sqc;
        di->sqc_inst_cache_size = gc->l1_instruction_cache_size_per_sqc;
        di->gl1c_cache_size = gc->gl1c_size_per_instance * gc->gl1c_per_sa;
        di->gl2c_cache_size = gc->gl2c_per_gpu;
    }
    di->mall_size = 64ull << 20;                 /* 64 MiB Infinity Cache; TODO MALL table */
    di->pcie_gen = 4;
    di->pcie_num_lanes = 16;

    /* engines: none are usable until Stage 2 brings up the rings */
    dev->accel_working = 0;
    ip = amd_discovery_find(disc, AMD_HWID_GC, 0);
    if (ip) {
        dev->gc_base0 = ip->num_base_kept ? ip->base[0] : 0;
        dev->ip_ver_major[AMDGPU_HW_IP_GFX] = ip->major;
        dev->ip_ver_minor[AMDGPU_HW_IP_GFX] = ip->minor;
        dev->ip_ver_major[AMDGPU_HW_IP_COMPUTE] = ip->major;
        dev->ip_ver_minor[AMDGPU_HW_IP_COMPUTE] = ip->minor;
    }
    ip = amd_discovery_find(disc, AMD_HWID_SDMA0, 0);
    if (ip) {
        dev->ip_ver_major[AMDGPU_HW_IP_DMA] = ip->major;
        dev->ip_ver_minor[AMDGPU_HW_IP_DMA] = ip->minor;
    }
}

void rdna3_drm_device_set_enumeration_only(rdna3_drm_device *dev, int on)
{
    dev->enumeration_only = on;
    dev->accel_working = on ? 1 : 0;
    dev->available_rings[AMDGPU_HW_IP_GFX] = on ? 1 : 0;
    dev->available_rings[AMDGPU_HW_IP_COMPUTE] = on ? 1 : 0;
    dev->available_rings[AMDGPU_HW_IP_DMA] = on ? 1 : 0;
}

void rdna3_drm_device_set_clocks(rdna3_drm_device *dev, uint32_t min_sclk_khz, uint32_t max_sclk_khz,
                                 uint32_t min_mclk_khz, uint32_t max_mclk_khz)
{
    struct drm_amdgpu_info_device *di = &dev->dev_info;
    if (min_sclk_khz) di->min_engine_clock = min_sclk_khz;
    if (max_sclk_khz) di->max_engine_clock = max_sclk_khz;
    if (min_mclk_khz) di->min_memory_clock = min_mclk_khz;
    if (max_mclk_khz) di->max_memory_clock = max_mclk_khz;
}

void rdna3_drm_device_set_cu_info(rdna3_drm_device *dev, const uint32_t bitmap[4][4],
                                  uint32_t cu_active, uint32_t rb_mask)
{
    struct drm_amdgpu_info_device *di = &dev->dev_info;
    uint32_t i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            di->cu_bitmap[i][j] = bitmap[i][j];
    di->cu_active_number = cu_active;
    /* amdgpu_kms: enabled_rb_pipes_mask = backend_enable_mask; num_rb_pipes
     * stays max_backends_per_se * max_shader_engines */
    di->enabled_rb_pipes_mask = rb_mask;
}

void rdna3_drm_device_set_ring_ready(rdna3_drm_device *dev, uint32_t ip, uint32_t mask)
{
    if (ip >= AMDGPU_HW_IP_NUM)
        return;
    dev->ready_rings[ip] |= mask;
    dev->available_rings[ip] |= mask;
    dev->accel_working = 1;
}

/* ------------------------------ file ------------------------------ */

int rdna3_drm_file_open(rdna3_drm_device *dev, rdna3_drm_file *f)
{
    zero(f, sizeof(*f));
    f->dev = dev;
    f->bos = (rdna3_bo **)dev->ops.zalloc(dev->ops.ctx, sizeof(rdna3_bo *) * RD_MAX_HANDLES);
    f->syncobjs = (rdna3_syncobj *)dev->ops.zalloc(dev->ops.ctx,
                                                   sizeof(rdna3_syncobj) * RD_MAX_SYNCOBJ);
    f->vas = (rdna3_va *)dev->ops.zalloc(dev->ops.ctx, sizeof(rdna3_va) * RD_MAX_VA);
    if (!f->bos || !f->syncobjs || !f->vas) {
        rdna3_drm_file_close(f);
        return -RD_ENOMEM;
    }
    if (dev->ops.vm_create) {
        f->vm = dev->ops.vm_create(dev->ops.ctx);
        if (!f->vm) {
            rdna3_drm_file_close(f);
            return -RD_ENOMEM;
        }
    }
    return 0;
}

/* ---- GDS / GWS / OA: on-chip ranges (amdgpu_ttm_init_on_chip) ---- */

static uint64_t *onchip_map(rdna3_drm_device *dev, uint32_t dom, uint32_t *total)
{
    if (dom == AMDGPU_GEM_DOMAIN_GDS) { *total = RD_GDS_SIZE; return dev->gds_map; }
    if (dom == AMDGPU_GEM_DOMAIN_GWS) { *total = RD_GWS_SIZE; return dev->gws_map; }
    if (dom == AMDGPU_GEM_DOMAIN_OA)  { *total = RD_OA_SIZE;  return dev->oa_map; }
    return NULL;
}

static int onchip_bit(const uint64_t *m, uint32_t i)
{
    return (int)((m[i / 64] >> (i % 64)) & 1);
}

static void onchip_set(uint64_t *m, uint32_t base, uint32_t n, int on)
{
    uint32_t i;
    for (i = base; i < base + n; i++) {
        if (on)
            m[i / 64] |= 1ull << (i % 64);
        else
            m[i / 64] &= ~(1ull << (i % 64));
    }
}

/* first fit, 'align' units apart */
static int onchip_alloc(uint64_t *m, uint32_t total, uint32_t n, uint32_t align, uint32_t *base)
{
    uint32_t b = 0, i;
    while (b + n <= total) {
        for (i = 0; i < n && !onchip_bit(m, b + i); i++)
            ;
        if (i == n) {
            onchip_set(m, b, n, 1);
            *base = b;
            return 0;
        }
        b = (b + i + 1 + align - 1) / align * align;   /* past the busy unit */
    }
    return -RD_ENOMEM;
}

static int bo_onchip(const rdna3_bo *bo)
{
    return (bo->placement & RD_ONCHIP_DOMAINS) != 0;
}

static void bo_put_dev(rdna3_drm_device *dev, rdna3_bo *bo)
{
    if (--bo->refcnt)
        return;
    if (bo_onchip(bo)) {   /* no backing: the range goes back */
        uint32_t total;
        onchip_set(onchip_map(dev, bo->placement, &total), (uint32_t)bo->vram_offset,
                   (uint32_t)bo->size, 0);
        dev->ops.free(dev->ops.ctx, bo, sizeof(*bo));
        return;
    }
    if (bo->placement == AMDGPU_GEM_DOMAIN_VRAM) {
        dev->vram_usage -= bo->size;
    } else {
        dev->gtt_usage -= bo->size;
    }
    dev->ops.bo_free(dev->ops.ctx, bo);
    dev->ops.free(dev->ops.ctx, bo, sizeof(*bo));
}

static void bo_put(rdna3_drm_file *f, rdna3_bo *bo)
{
    bo_put_dev(f->dev, bo);
}

static void bo_list_release(rdna3_drm_file *f, uint32_t i)
{
    if (f->bo_lists[i].gds)
        bo_put(f, f->bo_lists[i].gds);
    if (f->bo_lists[i].gws)
        bo_put(f, f->bo_lists[i].gws);
    if (f->bo_lists[i].oa)
        bo_put(f, f->bo_lists[i].oa);
    zero(&f->bo_lists[i], sizeof(f->bo_lists[i]));
}

static int fence_done(rdna3_drm_device *dev, const rdna3_fence *fe);

static void exports_gc(rdna3_drm_device *dev, const struct rdna3_drm_file *closing);

/* user fence BOs whose job finished can go (amdgpu_job_free drops uf_bo) */
static void uf_reap(rdna3_drm_device *dev, int all)
{
    uint32_t i, dropped = 0;
    for (i = 0; i < RD_MAX_UF_PENDING; i++)
        if (dev->uf_pending[i].bo && (all || fence_done(dev, &dev->uf_pending[i].fence))) {
            bo_put_dev(dev, dev->uf_pending[i].bo);
            dev->uf_pending[i].bo = NULL;
            dropped++;
        }
    if (dropped && !all)
        exports_gc(dev, NULL);   /* that may have been an export's last other holder */
}

void rdna3_drm_device_fini(rdna3_drm_device *dev)
{
    uint32_t i;
    uf_reap(dev, 1);
    for (i = 0; i < RD_MAX_EXPORTS; i++)
        if (dev->exports[i].bo) {
            bo_put_dev(dev, dev->exports[i].bo);
            zero(&dev->exports[i], sizeof(dev->exports[i]));
        }
}

static void va_release(rdna3_drm_file *f, rdna3_va *m)
{
    rdna3_drm_ops *ops = &f->dev->ops;
    if (f->vm && ops->vm_unmap)
        ops->vm_unmap(ops->ctx, f->vm, m->va, m->size);
    if (m->bo)
        bo_put(f, m->bo);
    m->bo = NULL;
    m->handle = 0;
}

/* exports nobody can reach any more: the exporter's file is gone (or
 * 'closing' is going) and no file holds the BO, only the export itself */
static void exports_gc(rdna3_drm_device *dev, const struct rdna3_drm_file *closing)
{
    uint32_t i;
    for (i = 0; i < RD_MAX_EXPORTS; i++) {
        if (!dev->exports[i].bo)
            continue;
        if (closing && dev->exports[i].owner == closing)
            dev->exports[i].owner = NULL;
        if (!dev->exports[i].owner && dev->exports[i].bo->refcnt == 1) {
            bo_put_dev(dev, dev->exports[i].bo);
            zero(&dev->exports[i], sizeof(dev->exports[i]));
        }
    }
}

void rdna3_drm_file_close(rdna3_drm_file *f)
{
    rdna3_drm_ops *ops = &f->dev->ops;
    uint32_t i;
    uf_reap(f->dev, 0);
    for (i = 0; i < RD_MAX_BO_LISTS; i++)
        bo_list_release(f, i);
    if (f->vas)
        for (i = 0; i < f->nvas; i++)
            if (f->vas[i].handle)
                va_release(f, &f->vas[i]);
    if (f->vm && ops->vm_destroy)
        ops->vm_destroy(ops->ctx, f->vm);
    f->vm = NULL;
    if (f->bos) {
        for (i = 0; i < RD_MAX_HANDLES; i++)
            if (f->bos[i])
                bo_put(f, f->bos[i]);
        ops->free(ops->ctx, f->bos, sizeof(rdna3_bo *) * RD_MAX_HANDLES);
    }
    if (f->syncobjs)
        ops->free(ops->ctx, f->syncobjs, sizeof(rdna3_syncobj) * RD_MAX_SYNCOBJ);
    if (f->vas)
        ops->free(ops->ctx, f->vas, sizeof(rdna3_va) * RD_MAX_VA);
    f->bos = NULL;
    f->syncobjs = NULL;
    f->vas = NULL;
    exports_gc(f->dev, f);
}

rdna3_bo *rdna3_drm_bo_lookup(rdna3_drm_file *f, uint32_t handle)
{
    if (handle == 0 || handle > RD_MAX_HANDLES)
        return NULL;
    return f->bos[handle - 1];
}

rdna3_bo *rdna3_drm_bo_by_mmap_offset(rdna3_drm_file *f, uint64_t off)
{
    uint32_t i;
    for (i = 0; i < RD_MAX_HANDLES; i++)
        if (f->bos[i] && !bo_onchip(f->bos[i]) && f->bos[i]->mmap_offset == off)
            return f->bos[i];
    return NULL;
}

static int handle_alloc(rdna3_drm_file *f, rdna3_bo *bo, uint32_t *handle)
{
    uint32_t i;
    for (i = 0; i < RD_MAX_HANDLES; i++)
        if (!f->bos[i]) {
            f->bos[i] = bo;
            *handle = i + 1;
            return 0;
        }
    return -RD_ENOSPC;
}

/* ------------------------------ core ------------------------------ */

static int put_string(rdna3_drm_file *f, uint64_t uptr, __kernel_size_t *len, const char *s)
{
    size_t n = slen(s);
    size_t avail = (size_t)*len;
    *len = n;
    if (uptr && avail)
        return f->dev->ops.copyout(f->dev->ops.ctx, uptr, s, MIN(avail, n));
    return 0;
}

static int drm_version(rdna3_drm_file *f, struct drm_version *v)
{
    int r;
    v->version_major = RD_DRM_MAJOR;
    v->version_minor = RD_DRM_MINOR;
    v->version_patchlevel = RD_DRM_PATCH;
    r = put_string(f, (uint64_t)(uintptr_t)v->name, &v->name_len, "amdgpu");
    if (!r)
        r = put_string(f, (uint64_t)(uintptr_t)v->date, &v->date_len, "20150101");
    if (!r)
        r = put_string(f, (uint64_t)(uintptr_t)v->desc, &v->desc_len, "AMD GPU (RDNA3-Hackintosh)");
    return r ? -RD_EFAULT : 0;
}

static int drm_get_cap(struct drm_get_cap *c)
{
    switch (c->capability) {
    case DRM_CAP_PRIME:              c->value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT; return 0;
    case DRM_CAP_TIMESTAMP_MONOTONIC: c->value = 1; return 0;
    case DRM_CAP_SYNCOBJ:            c->value = 1; return 0;
    case DRM_CAP_SYNCOBJ_TIMELINE:   c->value = 1; return 0;
    case DRM_CAP_DUMB_BUFFER:        c->value = 0; return 0;
    default:                         c->value = 0; return -RD_EINVAL;
    }
}

static int gem_close(rdna3_drm_file *f, struct drm_gem_close *c)
{
    rdna3_bo *bo = rdna3_drm_bo_lookup(f, c->handle);
    uint32_t i;
    if (!bo)
        return -RD_EINVAL;
    f->bos[c->handle - 1] = NULL;
    for (i = 0; i < f->nvas; i++)
        if (f->vas[i].handle == c->handle)
            va_release(f, &f->vas[i]);   /* amdgpu_gem_object_close drops the bo_va */
    bo_put(f, bo);
    exports_gc(f->dev, NULL);
    return 0;
}

/* ----------------------- PRIME (BO sharing) ----------------------- */

static uint64_t export_token(rdna3_drm_device *dev)
{
    uint64_t t = 0, z;
    uint32_t i;
    for (;;) {
        if (dev->ops.random64) {
            t = dev->ops.random64(dev->ops.ctx);
        } else {   /* splitmix64 of a counter and the clock */
            z = ++dev->export_ctr + (dev->ops.gpu_clock ? dev->ops.gpu_clock(dev->ops.ctx) : 0) +
                0x9E3779B97F4A7C15ull;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            t = z ^ (z >> 31);
        }
        if (!t)
            continue;
        for (i = 0; i < RD_MAX_EXPORTS; i++)
            if (dev->exports[i].bo && dev->exports[i].token == t)
                break;
        if (i == RD_MAX_EXPORTS)
            return t;
    }
}

/* amdgpu_gem_prime_export, without the dma-buf */
static int bo_export(rdna3_drm_file *f, struct rd_xnu_bo_share *x)
{
    rdna3_drm_device *dev = f->dev;
    rdna3_bo *bo = rdna3_drm_bo_lookup(f, x->handle);
    uint32_t i, free_slot = RD_MAX_EXPORTS;
    if (!bo || bo_onchip(bo))
        return -RD_EINVAL;
    if (bo->userptr)
        return -RD_EPERM;
    for (i = 0; i < RD_MAX_EXPORTS; i++) {
        if (dev->exports[i].bo == bo) {
            x->token = dev->exports[i].token;
            x->size = bo->size;
            if (!dev->exports[i].owner)
                dev->exports[i].owner = f;
            return 0;
        }
        if (!dev->exports[i].bo && free_slot == RD_MAX_EXPORTS)
            free_slot = i;
    }
    if (free_slot == RD_MAX_EXPORTS)
        return -RD_ENOSPC;
    dev->exports[free_slot].token = export_token(dev);
    dev->exports[free_slot].bo = bo;
    dev->exports[free_slot].owner = f;
    bo->refcnt++;
    x->token = dev->exports[free_slot].token;
    x->size = bo->size;
    return 0;
}

/* drm_gem_prime_fd_to_handle: the handle this file already has, or a new one */
static int bo_import(rdna3_drm_file *f, struct rd_xnu_bo_share *x)
{
    rdna3_drm_device *dev = f->dev;
    rdna3_bo *bo = NULL;
    uint32_t i;
    int r;
    for (i = 0; i < RD_MAX_EXPORTS && !bo; i++)
        if (dev->exports[i].bo && dev->exports[i].token == x->token)
            bo = dev->exports[i].bo;
    if (!x->token || !bo)
        return -RD_ENOENT;
    x->size = bo->size;
    for (i = 0; i < RD_MAX_HANDLES; i++)
        if (f->bos[i] == bo) {
            x->handle = i + 1;
            return 0;
        }
    r = handle_alloc(f, bo, &x->handle);
    if (r)
        return r;
    bo->refcnt++;
    return 0;
}

/* --------------------------- syncobj --------------------------- */
/* A syncobj is signaled outright, or carries the fence of the CS that
 * signals it (SYNCOBJ_OUT / FENCE_TO_HANDLE) and is signaled once that
 * fence passes. Timeline points are tracked as a single value. */

static int fence_done(rdna3_drm_device *dev, const rdna3_fence *fe)
{
    if (!dev->ops.fence_signaled)
        return 1;
    return dev->ops.fence_signaled(dev->ops.ctx, fe->ip, fe->ring, fe->seq);
}

static int fence_wait_abs(rdna3_drm_device *dev, const rdna3_fence *fe, uint64_t abs_ns)
{
    if (fence_done(dev, fe))
        return 0;
    if (!abs_ns || !dev->ops.fence_wait)
        return -RD_ETIME;
    return dev->ops.fence_wait(dev->ops.ctx, fe->ip, fe->ring, fe->seq, abs_ns);
}

static int sobj_signaled(rdna3_drm_device *dev, rdna3_syncobj *s)
{
    if (!s->signaled && s->has_fence && fence_done(dev, &s->fence)) {
        s->signaled = 1;
        s->has_fence = 0;
    }
    return s->signaled;
}

static rdna3_syncobj *sobj(rdna3_drm_file *f, uint32_t h)
{
    if (!h || h > RD_MAX_SYNCOBJ || !f->syncobjs[h - 1].used)
        return NULL;
    return &f->syncobjs[h - 1];
}

static int syncobj_create(rdna3_drm_file *f, struct drm_syncobj_create *c)
{
    uint32_t i;
    for (i = 0; i < RD_MAX_SYNCOBJ; i++)
        if (!f->syncobjs[i].used) {
            f->syncobjs[i].used = 1;
            f->syncobjs[i].signaled = (c->flags & DRM_SYNCOBJ_CREATE_SIGNALED) ? 1 : 0;
            f->syncobjs[i].point = 0;
            c->handle = i + 1;
            return 0;
        }
    return -RD_ENOSPC;
}

static int syncobj_destroy(rdna3_drm_file *f, struct drm_syncobj_destroy *d)
{
    rdna3_syncobj *s = sobj(f, d->handle);
    if (!s)
        return -RD_EINVAL;
    zero(s, sizeof(*s));
    return 0;
}

static int syncobj_for_each(rdna3_drm_file *f, uint64_t handles_ptr, uint32_t count,
                            uint64_t points_ptr, int op, uint64_t abs_ns)
{
    uint32_t i, h;
    uint64_t pt = 0;
    for (i = 0; i < count; i++) {
        rdna3_syncobj *s;
        if (f->dev->ops.copyin(f->dev->ops.ctx, &h, handles_ptr + 4ull * i, 4))
            return -RD_EFAULT;
        if (points_ptr && f->dev->ops.copyin(f->dev->ops.ctx, &pt, points_ptr + 8ull * i, 8))
            return -RD_EFAULT;
        s = sobj(f, h);
        if (!s)
            return -RD_ENOENT;
        switch (op) {
        case 0: s->signaled = 0; s->has_fence = 0; break;    /* reset */
        case 1: s->signaled = 1; s->has_fence = 0; if (pt > s->point) s->point = pt; break;
        case 2: /* wait (all) */
            if (sobj_signaled(f->dev, s) && (!points_ptr || s->point >= pt))
                break;
            if (s->has_fence) {
                /* the fence by value: ops.fence_wait may drop the OS lock,
                 * and another thread can reset, replace or destroy the
                 * syncobj meanwhile */
                rdna3_fence fe = s->fence;
                int r = fence_wait_abs(f->dev, &fe, abs_ns);
                if (r)
                    return r;
                s = sobj(f, h);
                if (s && s->has_fence && s->fence.ip == fe.ip && s->fence.ring == fe.ring &&
                    s->fence.seq == fe.seq) {
                    s->signaled = 1;
                    s->has_fence = 0;
                }
                break;
            }
            return -RD_ETIME;
        case 3: /* query */
            pt = sobj_signaled(f->dev, s) ? s->point : 0;
            if (f->dev->ops.copyout(f->dev->ops.ctx, points_ptr + 8ull * i, &pt, 8))
                return -RD_EFAULT;
            break;
        }
    }
    return 0;
}

/* drm_syncobj_transfer_ioctl: copy the fence of (src, src_point) into
 * (dst, dst_point). Syncobjs are per file here, so this is a snapshot, not
 * a shared object: what a sync_file export/import pair does on Linux. */
static int syncobj_transfer(rdna3_drm_file *f, struct drm_syncobj_transfer *t)
{
    rdna3_syncobj *src = sobj(f, t->src_handle), *dst = sobj(f, t->dst_handle);
    if (!src || !dst)
        return -RD_ENOENT;
    if (t->flags & ~DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT)
        return -RD_EINVAL;
    if (t->src_point && src->point < t->src_point)
        return -RD_EINVAL;   /* that point was never submitted */
    dst->signaled = sobj_signaled(f->dev, src) ? 1 : src->signaled;
    dst->has_fence = src->has_fence && !dst->signaled;
    dst->fence = src->fence;
    if (t->dst_point)
        dst->point = t->dst_point;
    return 0;
}

/* ------------------------------ CS ------------------------------ */

static int ring_ok(rdna3_drm_device *dev, uint32_t ip, uint32_t ring)
{
    return ip < AMDGPU_HW_IP_NUM && ring < 4 && (dev->ready_rings[ip] & (1u << ring));
}

/* amdgpu_ctx_get_fence: a sequence number that was never submitted is
 * -EINVAL, not something to wait for (it would only end at the hang cap,
 * with a GPU reset) */
static int seq_submitted(const rdna3_drm_device *dev, uint32_t ip, uint32_t ring, uint64_t seq)
{
    return ip < AMDGPU_HW_IP_NUM && ring < 4 && seq <= dev->last_seq[ip][ring];
}

/* RD_NR_XNU_SYNCOBJ_FENCE: the syncobj's state by value, for libdrm_xnu's
 * cross-process sync_file. SET validates like WAIT_CS: only a fence that was
 * submitted on this device may be installed, a forged future one would
 * block its waiters until the hang cap and a GPU reset. */
static int syncobj_fence(rdna3_drm_file *f, struct rd_xnu_syncobj_fence *x)
{
    rdna3_syncobj *s = sobj(f, x->handle);
    if (!s || x->pad)
        return -RD_EINVAL;
    if (x->op == RD_XNU_FENCE_GET) {
        x->signaled = sobj_signaled(f->dev, s) ? 1 : 0;
        x->ip = s->has_fence ? s->fence.ip : 0;
        x->ring = s->has_fence ? s->fence.ring : 0;
        x->seq = s->has_fence ? s->fence.seq : 0;
        return 0;
    }
    if (x->op != RD_XNU_FENCE_SET)
        return -RD_EINVAL;
    if (x->signaled) {
        s->signaled = 1;
        s->has_fence = 0;
        return 0;
    }
    if (x->seq && (!ring_ok(f->dev, x->ip, x->ring) ||
                   !seq_submitted(f->dev, x->ip, x->ring, x->seq)))
        return -RD_EINVAL;
    s->signaled = 0;
    s->has_fence = x->seq != 0;
    s->fence.ip = x->ip;
    s->fence.ring = x->ring;
    s->fence.seq = x->seq;
    return 0;
}

/* DEPENDENCIES / WAIT_CS handles: ~0 = latest on that ring, 0 = none */
static uint64_t dep_seq(rdna3_drm_device *dev, uint32_t ip, uint32_t ring, uint64_t h)
{
    if (h == ~0ull)
        return dev->last_seq[ip][ring];
    return h;
}

/* the on-chip BOs of a BO list, as handles (amdgpu_bo_list_create: the
 * last one of each kind wins); every handle must exist */
static int bo_list_scan(rdna3_drm_file *f, uint64_t ptr, uint32_t n, uint32_t esz, uint32_t h[3])
{
    rdna3_drm_ops *ops = &f->dev->ops;
    uint32_t i;
    h[0] = h[1] = h[2] = 0;
    if (n > RD_MAX_HANDLES || (n && esz < sizeof(uint32_t)))
        return -RD_EINVAL;
    for (i = 0; i < n; i++) {
        struct drm_amdgpu_bo_list_entry e;
        rdna3_bo *bo;
        zero(&e, sizeof(e));
        if (ops->copyin(ops->ctx, &e, ptr + (uint64_t)esz * i, MIN(esz, sizeof(e))))
            return -RD_EFAULT;
        bo = rdna3_drm_bo_lookup(f, e.bo_handle);
        if (!bo)
            return -RD_ENOENT;
        if (bo->placement == AMDGPU_GEM_DOMAIN_GDS)
            h[0] = e.bo_handle;
        else if (bo->placement == AMDGPU_GEM_DOMAIN_GWS)
            h[1] = e.bo_handle;
        else if (bo->placement == AMDGPU_GEM_DOMAIN_OA)
            h[2] = e.bo_handle;
    }
    return 0;
}

static int bo_list_find(rdna3_drm_file *f, uint32_t handle)
{
    int i;
    for (i = 0; handle && i < RD_MAX_BO_LISTS; i++)
        if (f->bo_lists[i].handle == handle)
            return i;
    return -1;
}

/* AMDGPU_BO_LIST: every BO is resident anyway; a list keeps references
 * to its GDS / GWS / OA BOs for the jobs that name it */
static int bo_list_ioctl(rdna3_drm_file *f, union drm_amdgpu_bo_list *l)
{
    uint32_t op = l->in.operation, h[3], handle;
    int i, r;
    if (op == AMDGPU_BO_LIST_OP_DESTROY) {
        i = bo_list_find(f, l->in.list_handle);
        if (i >= 0)
            bo_list_release(f, (uint32_t)i);
        return 0;
    }
    if (op != AMDGPU_BO_LIST_OP_CREATE && op != AMDGPU_BO_LIST_OP_UPDATE)
        return -RD_EINVAL;
    r = bo_list_scan(f, l->in.bo_info_ptr, l->in.bo_number, l->in.bo_info_size, h);
    if (r)
        return r;
    if (op == AMDGPU_BO_LIST_OP_UPDATE) {
        handle = l->in.list_handle;
        i = bo_list_find(f, handle);
        if (i < 0)
            return -RD_ENOENT;
        bo_list_release(f, (uint32_t)i);
    } else {
        for (i = 0; i < RD_MAX_BO_LISTS && f->bo_lists[i].handle; i++)
            ;
        if (i == RD_MAX_BO_LISTS)
            return -RD_ENOMEM;
        if (!++f->next_bo_list)
            ++f->next_bo_list;
        handle = f->next_bo_list;
    }
    f->bo_lists[i].handle = handle;
    if ((f->bo_lists[i].gds = rdna3_drm_bo_lookup(f, h[0])) != NULL)
        f->bo_lists[i].gds->refcnt++;
    if ((f->bo_lists[i].gws = rdna3_drm_bo_lookup(f, h[1])) != NULL)
        f->bo_lists[i].gws->refcnt++;
    if ((f->bo_lists[i].oa = rdna3_drm_bo_lookup(f, h[2])) != NULL)
        f->bo_lists[i].oa->refcnt++;
    zero(&l->out, sizeof(l->out));
    l->out.list_handle = handle;
    return 0;
}

static int uf_bo_ok(const rdna3_bo *bo, uint64_t offset)
{
    return bo && bo->size == 4096 && !bo->userptr && offset <= 4096 - 8 && !(offset & 7);
}

static int cs_ioctl(rdna3_drm_file *f, union drm_amdgpu_cs *cs)
{
    rdna3_drm_device *dev = f->dev;
    rdna3_drm_ops *ops = &dev->ops;
    rdna3_ib ibs[RD_MAX_CS_IBS];
    uint32_t nibs = 0, ip = 0, ring = 0, i, j;
    /* syncobjs to signal once queued: handle + timeline point */
    uint32_t out_h[16];
    uint64_t out_pt[16];
    uint32_t nout = 0;
    uint64_t seq = 0;
    rdna3_cs_job job;
    uint64_t uf_offset = 0;
    uint32_t uf_handle = 0, uf_slot = RD_MAX_UF_PENDING;
    uint32_t onchip[3] = { 0, 0, 0 };   /* BO_HANDLES chunk: GDS / GWS / OA handles */
    int have_bo_handles = 0;
    int r;

    if (!ops->submit)
        return -RD_ENODEV;
    uf_reap(dev, 0);
    zero(&job, sizeof(job));
    if (!cs->in.num_chunks || cs->in.num_chunks > 64)
        return -RD_EINVAL;
    if (cs->in.bo_list_handle && bo_list_find(f, cs->in.bo_list_handle) < 0)
        return -RD_ENOENT;   /* amdgpu_bo_list_get */
    /* amdgpu_cs_submit: a guilty context never runs again */
    if (ops->vm_query && f->vm && (ops->vm_query(ops->ctx, f->vm) & AMDGPU_CTX_QUERY2_FLAGS_GUILTY))
        return -RD_ECANCELED;

    for (i = 0; i < cs->in.num_chunks; i++) {
        uint64_t cptr;
        struct drm_amdgpu_cs_chunk ch;
        if (ops->copyin(ops->ctx, &cptr, cs->in.chunks + 8ull * i, 8) ||
            ops->copyin(ops->ctx, &ch, cptr, sizeof(ch)))
            return -RD_EFAULT;
        switch (ch.chunk_id) {
        case AMDGPU_CHUNK_ID_IB: {
            struct drm_amdgpu_cs_chunk_ib ib;
            zero(&ib, sizeof(ib));
            if (ops->copyin(ops->ctx, &ib, ch.chunk_data,
                            MIN((uint64_t)ch.length_dw * 4, sizeof(ib))))
                return -RD_EFAULT;
            if (nibs == RD_MAX_CS_IBS || (ib.ib_bytes & 3) || !ib.ib_bytes || ib.ip_instance)
                return -RD_EINVAL;
            if (nibs && (ib.ip_type != ip || ib.ring != ring))
                return -RD_EINVAL;   /* one ring per submission, like amdgpu */
            if (!ring_ok(dev, ib.ip_type, ib.ring))
                return dev->enumeration_only ? -RD_ENODEV : -RD_EINVAL;  /* faked ring */
            if (ib.flags & (AMDGPU_IB_FLAG_PREAMBLE | AMDGPU_IB_FLAG_CE))
                continue;            /* no CE on GFX11; preambles can be dropped */
            ip = ib.ip_type;
            ring = ib.ring;
            ibs[nibs].va = ib.va_start;
            ibs[nibs].length_dw = ib.ib_bytes / 4;
            ibs[nibs].flags = ib.flags;
            nibs++;
            break;
        }
        case AMDGPU_CHUNK_ID_DEPENDENCIES:
        case AMDGPU_CHUNK_ID_SCHEDULED_DEPENDENCIES: {
            uint32_t n = ch.length_dw * 4 / sizeof(struct drm_amdgpu_cs_chunk_dep);
            for (j = 0; j < n; j++) {
                struct drm_amdgpu_cs_chunk_dep dep;
                rdna3_fence fe;
                if (ops->copyin(ops->ctx, &dep, ch.chunk_data + sizeof(dep) * j, sizeof(dep)))
                    return -RD_EFAULT;
                if (!dep.handle || !ring_ok(dev, dep.ip_type, dep.ring))
                    continue;
                fe.ip = dep.ip_type;
                fe.ring = dep.ring;
                fe.seq = dep_seq(dev, dep.ip_type, dep.ring, dep.handle);
                /* same ring: in-order execution already covers it */
                if (ch.chunk_id == AMDGPU_CHUNK_ID_DEPENDENCIES && !fence_done(dev, &fe) &&
                    (r = fence_wait_abs(dev, &fe, ~0ull)) != 0)
                    return r;
            }
            break;
        }
        case AMDGPU_CHUNK_ID_SYNCOBJ_IN:
        case AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_WAIT: {
            int tl = ch.chunk_id == AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_WAIT;
            uint32_t sz = tl ? sizeof(struct drm_amdgpu_cs_chunk_syncobj)
                             : sizeof(struct drm_amdgpu_cs_chunk_sem);
            uint32_t n = ch.length_dw * 4 / sz;
            for (j = 0; j < n; j++) {
                struct drm_amdgpu_cs_chunk_syncobj so;
                rdna3_syncobj *s;
                zero(&so, sizeof(so));
                if (ops->copyin(ops->ctx, &so, ch.chunk_data + (uint64_t)sz * j, sz))
                    return -RD_EFAULT;
                s = sobj(f, so.handle);
                if (!s)
                    return -RD_EINVAL;
                if (!sobj_signaled(dev, s) && s->has_fence) {
                    r = fence_wait_abs(dev, &s->fence, ~0ull);
                    if (r)
                        return r;
                }
            }
            break;
        }
        case AMDGPU_CHUNK_ID_SYNCOBJ_OUT:
        case AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_SIGNAL: {
            int tl = ch.chunk_id == AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_SIGNAL;
            uint32_t sz = tl ? sizeof(struct drm_amdgpu_cs_chunk_syncobj)
                             : sizeof(struct drm_amdgpu_cs_chunk_sem);
            uint32_t n = ch.length_dw * 4 / sz;
            for (j = 0; j < n; j++) {
                struct drm_amdgpu_cs_chunk_syncobj so;
                zero(&so, sizeof(so));
                if (nout == 16)
                    return -RD_EINVAL;
                if (ops->copyin(ops->ctx, &so, ch.chunk_data + (uint64_t)sz * j, sz))
                    return -RD_EFAULT;
                if (!sobj(f, so.handle))
                    return -RD_EINVAL;
                out_h[nout] = so.handle;
                out_pt[nout] = tl ? so.point : 0;
                nout++;
            }
            break;
        }
        case AMDGPU_CHUNK_ID_FENCE: {
            /* amdgpu_cs_p1_user_fence: one page, not userptr, room for 8
             * bytes; 8-byte aligned too, since the GPU writes a qword */
            struct drm_amdgpu_cs_chunk_fence cf;
            zero(&cf, sizeof(cf));
            if (uf_handle || ops->copyin(ops->ctx, &cf, ch.chunk_data,
                                         MIN((uint64_t)ch.length_dw * 4, sizeof(cf))))
                return uf_handle ? -RD_EINVAL : -RD_EFAULT;
            if (!uf_bo_ok(rdna3_drm_bo_lookup(f, cf.handle), cf.offset))
                return -RD_EINVAL;
            uf_handle = cf.handle;   /* looked up again after the waits */
            uf_offset = cf.offset;
            break;
        }
        case AMDGPU_CHUNK_ID_BO_HANDLES: {
            /* residency: everything is resident; only the on-chip BOs
             * matter. A single list per job, like amdgpu */
            struct drm_amdgpu_bo_list_in bl;
            zero(&bl, sizeof(bl));
            if (have_bo_handles || cs->in.bo_list_handle)
                return -RD_EINVAL;
            if (ops->copyin(ops->ctx, &bl, ch.chunk_data,
                            MIN((uint64_t)ch.length_dw * 4, sizeof(bl))))
                return -RD_EFAULT;
            r = bo_list_scan(f, bl.bo_info_ptr, bl.bo_number, bl.bo_info_size, onchip);
            if (r)
                return r;
            have_bo_handles = 1;
            break;
        }
        case AMDGPU_CHUNK_ID_CP_GFX_SHADOW:
            break;
        default:
            return -RD_EINVAL;
        }
    }
    if (!nibs)
        return -RD_EINVAL;
    if (uf_handle) {
        /* a slot to hold the BO until the GPU wrote it; all busy: the
         * oldest job of the table is waited for */
        for (i = 0; i < RD_MAX_UF_PENDING && dev->uf_pending[i].bo; i++)
            ;
        if (i == RD_MAX_UF_PENDING) {
            r = fence_wait_abs(dev, &dev->uf_pending[0].fence, ~0ull);
            if (r)
                return r;
            uf_reap(dev, 0);
            for (i = 0; i < RD_MAX_UF_PENDING && dev->uf_pending[i].bo; i++)
                ;
            if (i == RD_MAX_UF_PENDING)
                return -RD_EBUSY;
        }
        uf_slot = i;
        /* the waits above may have let a GEM_CLOSE in */
        job.uf_bo = rdna3_drm_bo_lookup(f, uf_handle);
        job.uf_offset = uf_offset;
        if (!uf_bo_ok(job.uf_bo, uf_offset))
            return -RD_EINVAL;
    }
    /* GDS / GWS / OA partitions (amdgpu_job_set_resources), looked up
     * after the waits as well */
    {
        rdna3_bo *gds = NULL, *gws = NULL, *oa = NULL;
        if (cs->in.bo_list_handle) {
            int li = bo_list_find(f, cs->in.bo_list_handle);
            if (li < 0)
                return -RD_ENOENT;
            gds = f->bo_lists[li].gds;
            gws = f->bo_lists[li].gws;
            oa = f->bo_lists[li].oa;
        } else {
            gds = rdna3_drm_bo_lookup(f, onchip[0]);
            gws = rdna3_drm_bo_lookup(f, onchip[1]);
            oa = rdna3_drm_bo_lookup(f, onchip[2]);
            if ((onchip[0] && !gds) || (onchip[1] && !gws) || (onchip[2] && !oa))
                return -RD_ENOENT;
        }
        if (gds) {
            job.gds_base = (uint32_t)gds->vram_offset;
            job.gds_size = (uint32_t)gds->size;
        }
        if (gws) {
            job.gws_base = (uint32_t)gws->vram_offset;
            job.gws_size = (uint32_t)gws->size;
        }
        if (oa) {
            job.oa_base = (uint32_t)oa->vram_offset;
            job.oa_size = (uint32_t)oa->size;
        }
    }

    r = ops->submit(ops->ctx, f->vm, ip, ring, ibs, nibs, &job, &seq);
    if (r)
        return r;
    dev->last_seq[ip][ring] = seq;
    if (job.uf_bo) {
        job.uf_bo->refcnt++;
        dev->uf_pending[uf_slot].bo = job.uf_bo;
        dev->uf_pending[uf_slot].fence.ip = ip;
        dev->uf_pending[uf_slot].fence.ring = ring;
        dev->uf_pending[uf_slot].fence.seq = seq;
    }
    for (i = 0; i < nout; i++) {
        /* looked up again: a dependency wait drops the OS lock, and a
         * SYNCOBJ_DESTROY may have run meanwhile */
        rdna3_syncobj *s = sobj(f, out_h[i]);
        if (!s)
            continue;
        s->signaled = 0;
        s->has_fence = 1;
        s->fence.ip = ip;
        s->fence.ring = ring;
        s->fence.seq = seq;
        if (out_pt[i] > s->point)
            s->point = out_pt[i];
    }
    cs->out.handle = seq;
    return 0;
}

static int wait_cs(rdna3_drm_file *f, union drm_amdgpu_wait_cs *w)
{
    rdna3_drm_device *dev = f->dev;
    rdna3_fence fe;
    uint64_t timeout = w->in.timeout;
    if (!ring_ok(dev, w->in.ip_type, w->in.ring))
        return -RD_EINVAL;
    fe.ip = w->in.ip_type;
    fe.ring = w->in.ring;
    fe.seq = dep_seq(dev, fe.ip, fe.ring, w->in.handle);
    zero(&w->out, sizeof(w->out));
    if (!fe.seq)
        return 0;
    if (!seq_submitted(dev, fe.ip, fe.ring, fe.seq))
        return -RD_EINVAL;
    w->out.status = fence_wait_abs(dev, &fe, timeout) ? 1 : 0;
    return 0;
}

static int wait_fences(rdna3_drm_file *f, union drm_amdgpu_wait_fences *w)
{
    rdna3_drm_device *dev = f->dev;
    uint32_t i, n = w->in.fence_count, all = w->in.wait_all, first = ~0u, done = 0;
    uint64_t timeout = w->in.timeout_ns;
    if (!n || n > 1024)
        return -RD_EINVAL;
    for (i = 0; i < n; i++) {
        struct drm_amdgpu_fence af;
        rdna3_fence fe;
        if (dev->ops.copyin(dev->ops.ctx, &af, w->in.fences + sizeof(af) * i, sizeof(af)))
            return -RD_EFAULT;
        if (!ring_ok(dev, af.ip_type, af.ring))
            return -RD_EINVAL;
        fe.ip = af.ip_type;
        fe.ring = af.ring;
        fe.seq = af.seq_no;
        if (!seq_submitted(dev, fe.ip, fe.ring, fe.seq))
            return -RD_EINVAL;
        if (fence_done(dev, &fe) || (all && !fence_wait_abs(dev, &fe, timeout))) {
            done++;
            if (first == ~0u)
                first = i;
            if (!all)
                break;
        }
    }
    if (!all && !done) {
        /* wait for the first one; good enough for a single ring */
        struct drm_amdgpu_fence af;
        rdna3_fence fe;
        if (dev->ops.copyin(dev->ops.ctx, &af, w->in.fences, sizeof(af)))
            return -RD_EFAULT;
        fe.ip = af.ip_type;
        fe.ring = af.ring;
        fe.seq = af.seq_no;
        if (!fence_wait_abs(dev, &fe, timeout)) {
            done = 1;
            first = 0;
        }
    }
    zero(&w->out, sizeof(w->out));
    w->out.status = all ? (done == n) : (done > 0);
    w->out.first_signaled = first == ~0u ? 0 : first;
    return 0;
}

static int fence_to_handle(rdna3_drm_file *f, union drm_amdgpu_fence_to_handle *h)
{
    struct drm_syncobj_create c;
    rdna3_syncobj *s;
    int r;
    if (h->in.what != AMDGPU_FENCE_TO_HANDLE_GET_SYNCOBJ)
        return -RD_EOPNOTSUPP;   /* FDs need PRIME-style export (Stage 4b) */
    if (!ring_ok(f->dev, h->in.fence.ip_type, h->in.fence.ring))
        return -RD_EINVAL;
    zero(&c, sizeof(c));
    r = syncobj_create(f, &c);
    if (r)
        return r;
    s = sobj(f, c.handle);
    s->fence.ip = h->in.fence.ip_type;
    s->fence.ring = h->in.fence.ring;
    s->fence.seq = dep_seq(f->dev, s->fence.ip, s->fence.ring, h->in.fence.seq_no);
    s->has_fence = s->fence.seq != 0;
    s->signaled = !s->has_fence;
    zero(&h->out, sizeof(h->out));
    h->out.handle = c.handle;
    return 0;
}

/* --------------------------- amdgpu GEM --------------------------- */

static int gem_create_onchip(rdna3_drm_file *f, union drm_amdgpu_gem_create *a);

static int gem_create(rdna3_drm_file *f, union drm_amdgpu_gem_create *a)
{
    rdna3_drm_device *dev = f->dev;
    uint64_t size = a->in.bo_size;
    uint32_t dom = (uint32_t)a->in.domains;
    rdna3_bo *bo;
    uint32_t handle = 0;
    int r;

    if (!size || size > (dev->vram_size + dev->gtt_size))
        return -RD_EINVAL;
    if (dom & ~(uint32_t)AMDGPU_GEM_DOMAIN_MASK)
        return -RD_EINVAL;
    if (dom & AMDGPU_GEM_DOMAIN_DOORBELL)
        return -RD_EOPNOTSUPP; /* TODO(stage4): doorbell BOs (user queues) */
    if (dom & RD_ONCHIP_DOMAINS)
        return gem_create_onchip(f, a);

    bo = (rdna3_bo *)dev->ops.zalloc(dev->ops.ctx, sizeof(*bo));
    if (!bo)
        return -RD_ENOMEM;
    bo->refcnt = 1;
    bo->size = (size + 4095) & ~4095ull;
    bo->alignment = MAX(a->in.alignment, 4096ull);
    bo->domains = dom;
    bo->flags = a->in.domain_flags;
    /* prefer VRAM if asked; the OS layer may fall back to GTT */
    bo->placement = (dom & AMDGPU_GEM_DOMAIN_VRAM) ? AMDGPU_GEM_DOMAIN_VRAM
                                                   : AMDGPU_GEM_DOMAIN_GTT;
    r = dev->ops.bo_alloc(dev->ops.ctx, bo);
    if (r && bo->placement == AMDGPU_GEM_DOMAIN_VRAM && (dom & AMDGPU_GEM_DOMAIN_GTT)) {
        bo->placement = AMDGPU_GEM_DOMAIN_GTT;
        r = dev->ops.bo_alloc(dev->ops.ctx, bo);
    }
    if (r) {
        dev->ops.free(dev->ops.ctx, bo, sizeof(*bo));
        return r;
    }
    bo->mmap_offset = dev->next_mmap_offset;
    dev->next_mmap_offset += bo->size + 4096;
    if (bo->placement == AMDGPU_GEM_DOMAIN_VRAM)
        dev->vram_usage += bo->size;
    else
        dev->gtt_usage += bo->size;

    r = handle_alloc(f, bo, &handle);
    if (r) {
        bo_put(f, bo);
        return r;
    }
    zero(a, sizeof(*a));
    a->out.handle = handle;
    return 0;
}

/* amdgpu_gem_create_ioctl for GDS / GWS / OA: sizes and alignment are in
 * units (bytes / entries / counters), never CPU-visible, never in a VM;
 * a job gets them through its BO list */
static int gem_create_onchip(rdna3_drm_file *f, union drm_amdgpu_gem_create *a)
{
    rdna3_drm_device *dev = f->dev;
    uint32_t dom = (uint32_t)a->in.domains, total = 0, base = 0, handle = 0;
    uint64_t *map = onchip_map(dev, dom, &total);   /* exactly one of them */
    uint64_t align = a->in.alignment ? a->in.alignment : 1;
    rdna3_bo *bo;
    int r;
    if (!map || a->in.bo_size > total || align > total ||
        (a->in.domain_flags & AMDGPU_GEM_CREATE_VM_ALWAYS_VALID))
        return -RD_EINVAL;   /* "GDS bo cannot be per-vm-bo" */
    r = onchip_alloc(map, total, (uint32_t)a->in.bo_size, (uint32_t)align, &base);
    if (r)
        return r;
    bo = (rdna3_bo *)dev->ops.zalloc(dev->ops.ctx, sizeof(*bo));
    if (!bo) {
        onchip_set(map, base, (uint32_t)a->in.bo_size, 0);
        return -RD_ENOMEM;
    }
    bo->refcnt = 1;
    bo->size = a->in.bo_size;
    bo->alignment = align;
    bo->domains = dom;
    bo->placement = dom;
    bo->flags = a->in.domain_flags | AMDGPU_GEM_CREATE_NO_CPU_ACCESS;
    bo->vram_offset = base;
    r = handle_alloc(f, bo, &handle);
    if (r) {
        bo_put(f, bo);
        return r;
    }
    zero(a, sizeof(*a));
    a->out.handle = handle;
    return 0;
}

/* amdgpu_gem_userptr_ioctl: a BO over the client's own pages */
static int gem_userptr(rdna3_drm_file *f, struct drm_amdgpu_gem_userptr *a)
{
    rdna3_drm_device *dev = f->dev;
    rdna3_bo *bo;
    uint32_t handle = 0;
    int r;

    if (a->flags & ~(uint32_t)(AMDGPU_GEM_USERPTR_READONLY | AMDGPU_GEM_USERPTR_ANONONLY |
                               AMDGPU_GEM_USERPTR_VALIDATE | AMDGPU_GEM_USERPTR_REGISTER))
        return -RD_EINVAL;
    /* reject unknown flags and no-op combinations like amdgpu */
    if (!(a->flags & AMDGPU_GEM_USERPTR_READONLY) && !(a->flags & AMDGPU_GEM_USERPTR_REGISTER))
        return -RD_EACCES;
    if (!a->size || (a->addr & 4095) || (a->size & 4095) || a->size > dev->gtt_size)
        return -RD_EINVAL;
    if (!dev->ops.bo_userptr)
        return -RD_ENOSYS;

    bo = (rdna3_bo *)dev->ops.zalloc(dev->ops.ctx, sizeof(*bo));
    if (!bo)
        return -RD_ENOMEM;
    bo->refcnt = 1;
    bo->size = a->size;
    bo->alignment = 4096;
    bo->domains = AMDGPU_GEM_DOMAIN_GTT;
    bo->placement = AMDGPU_GEM_DOMAIN_GTT;
    bo->flags = (a->flags & AMDGPU_GEM_USERPTR_READONLY) ? AMDGPU_GEM_CREATE_NO_CPU_ACCESS : 0;
    bo->userptr = 1;
    bo->user_addr = a->addr;
    r = dev->ops.bo_userptr(dev->ops.ctx, f->os, bo, a->addr, a->flags);
    if (r) {
        dev->ops.free(dev->ops.ctx, bo, sizeof(*bo));
        return r;
    }
    dev->gtt_usage += bo->size;
    r = handle_alloc(f, bo, &handle);
    if (r) {
        bo_put(f, bo);
        return r;
    }
    a->handle = handle;
    return 0;
}

static int gem_mmap(rdna3_drm_file *f, union drm_amdgpu_gem_mmap *m)
{
    rdna3_bo *bo = rdna3_drm_bo_lookup(f, m->in.handle);
    if (!bo)
        return -RD_ENOENT;
    if ((bo->flags & AMDGPU_GEM_CREATE_NO_CPU_ACCESS) || bo->userptr)
        return -RD_EPERM;   /* amdgpu: userptr BOs are never mmap()ed */
    m->out.addr_ptr = bo->mmap_offset;
    return 0;
}

static int gem_va(rdna3_drm_file *f, struct drm_amdgpu_gem_va *va)
{
    rdna3_drm_ops *ops = &f->dev->ops;
    uint32_t i;
    /* mappings are recorded always and written into the GPU page tables
     * when the OS layer provides vm_map (hw/gpuvm.c behind it) */
    if (va->va_address < VA_RESERVED_BOTTOM || (va->va_address & 4095) || (va->map_size & 4095) ||
        (va->offset_in_bo & 4095))
        return -RD_EINVAL;
    switch (va->operation) {
    case AMDGPU_VA_OP_MAP:
    case AMDGPU_VA_OP_REPLACE: {
        rdna3_bo *bo = rdna3_drm_bo_lookup(f, va->handle);
        if (!bo)
            return -RD_ENOENT;
        if (!va->map_size || va->offset_in_bo + va->map_size > bo->size || bo_onchip(bo))
            return -RD_EINVAL;
        if (va->operation == AMDGPU_VA_OP_REPLACE)
            for (i = 0; i < f->nvas; i++) {
                rdna3_va *m = &f->vas[i];
                if (m->handle && m->va < va->va_address + va->map_size &&
                    va->va_address < m->va + m->size)
                    va_release(f, m);
            }
        for (i = 0; i < f->nvas; i++)
            if (!f->vas[i].handle)
                break;
        if (i == f->nvas) {
            if (f->nvas >= RD_MAX_VA)
                return -RD_ENOSPC;
            f->nvas++;
        }
        if (f->vm && ops->vm_map) {
            int r = ops->vm_map(ops->ctx, f->vm, bo, va->va_address, va->offset_in_bo,
                                va->map_size, va->flags);
            if (r)
                return r;
        }
        bo->refcnt++;
        f->vas[i].bo = bo;
        f->vas[i].handle = va->handle;
        f->vas[i].va = va->va_address;
        f->vas[i].offset = va->offset_in_bo;
        f->vas[i].size = va->map_size;
        f->vas[i].flags = va->flags;
        return 0;
    }
    case AMDGPU_VA_OP_UNMAP:
    case AMDGPU_VA_OP_CLEAR:
        for (i = 0; i < f->nvas; i++) {
            rdna3_va *m = &f->vas[i];
            if (!m->handle)
                continue;
            if (va->operation == AMDGPU_VA_OP_UNMAP ? m->va == va->va_address
                                                    : (m->va < va->va_address + va->map_size &&
                                                       va->va_address < m->va + m->size))
                va_release(f, m);
        }
        return 0;
    default:
        return -RD_EINVAL;
    }
}

static int gem_metadata(rdna3_drm_file *f, struct drm_amdgpu_gem_metadata *m)
{
    rdna3_bo *bo = rdna3_drm_bo_lookup(f, m->handle);
    if (!bo)
        return -RD_ENOENT;
    if (m->op == AMDGPU_GEM_METADATA_OP_SET_METADATA) {
        if (m->data.data_size_bytes > sizeof(m->data.data) ||
            m->data.data_size_bytes > sizeof(bo->metadata))
            return -RD_EINVAL;
        bo->tiling_info = m->data.tiling_info;
        bo->metadata_size = m->data.data_size_bytes;
        copy(bo->metadata, m->data.data, bo->metadata_size);
        return 0;
    }
    if (m->op == AMDGPU_GEM_METADATA_OP_GET_METADATA) {
        m->data.tiling_info = bo->tiling_info;
        m->data.data_size_bytes = bo->metadata_size;
        copy(m->data.data, bo->metadata, bo->metadata_size);
        return 0;
    }
    return -RD_EINVAL;
}

static int gem_op(rdna3_drm_file *f, struct drm_amdgpu_gem_op *o)
{
    rdna3_bo *bo = rdna3_drm_bo_lookup(f, o->handle);
    if (!bo)
        return -RD_ENOENT;
    switch (o->op) {
    case AMDGPU_GEM_OP_GET_GEM_CREATE_INFO: {
        struct drm_amdgpu_gem_create_in info;
        zero(&info, sizeof(info));
        info.bo_size = bo->size;
        info.alignment = bo->alignment;
        info.domains = bo->domains;
        info.domain_flags = bo->flags;
        return f->dev->ops.copyout(f->dev->ops.ctx, o->value, &info, sizeof(info))
                   ? -RD_EFAULT : 0;
    }
    case AMDGPU_GEM_OP_SET_PLACEMENT:
        if (bo_onchip(bo))
            return -RD_EINVAL;   /* the BO lists find them by domain */
        bo->domains = (uint32_t)o->value; /* migration: TODO */
        return 0;
    default:
        return -RD_EINVAL;
    }
}

/* ------------------------------ ctx ------------------------------ */

static int ctx_ioctl(rdna3_drm_file *f, union drm_amdgpu_ctx *c)
{
    uint32_t id = c->in.ctx_id, i;
    switch (c->in.op) {
    case AMDGPU_CTX_OP_ALLOC_CTX:
        for (i = 1; i < RD_MAX_CTX; i++)
            if (!f->ctx_used[i]) {
                f->ctx_used[i] = 1;
                zero(c, sizeof(*c));
                c->out.alloc.ctx_id = i;
                return 0;
            }
        return -RD_ENOMEM;
    case AMDGPU_CTX_OP_FREE_CTX:
        if (!id || id >= RD_MAX_CTX || !f->ctx_used[id])
            return -RD_EINVAL;
        f->ctx_used[id] = 0;
        return 0;
    case AMDGPU_CTX_OP_QUERY_STATE:
    case AMDGPU_CTX_OP_QUERY_STATE2: {
        /* amdgpu_ctx_query / amdgpu_ctx_query2: the reset state lives
         * with the VM here (one VM per file, one VMID per VM) */
        uint32_t fl = 0, op = c->in.op;
        if (!id || id >= RD_MAX_CTX || !f->ctx_used[id])
            return -RD_EINVAL;
        if (f->dev->ops.vm_query && f->vm)
            fl = f->dev->ops.vm_query(f->dev->ops.ctx, f->vm);
        zero(c, sizeof(*c));
        if (op == AMDGPU_CTX_OP_QUERY_STATE2) {
            c->out.state.flags = fl;
        } else {
            c->out.state.reset_status = (fl & AMDGPU_CTX_QUERY2_FLAGS_GUILTY) ? AMDGPU_CTX_GUILTY_RESET
                                      : (fl & AMDGPU_CTX_QUERY2_FLAGS_RESET) ? AMDGPU_CTX_INNOCENT_RESET
                                      : AMDGPU_CTX_NO_RESET;
        }
        return 0;
    }
    case AMDGPU_CTX_OP_GET_STABLE_PSTATE:
        zero(c, sizeof(*c));
        c->out.pstate.flags = AMDGPU_CTX_STABLE_PSTATE_NONE;
        return 0;
    case AMDGPU_CTX_OP_SET_STABLE_PSTATE:
        return 0;
    default:
        return -RD_EINVAL;
    }
}

/* ------------------------------ info ------------------------------ */

static int info_out(rdna3_drm_file *f, struct drm_amdgpu_info *in, const void *p, size_t n)
{
    size_t len = MIN((size_t)in->return_size, n);
    if (!in->return_pointer || !len)
        return -RD_EINVAL;
    return f->dev->ops.copyout(f->dev->ops.ctx, in->return_pointer, p, len) ? -RD_EFAULT : 0;
}

static int info_ioctl(rdna3_drm_file *f, struct drm_amdgpu_info *in)
{
    rdna3_drm_device *dev = f->dev;
    switch (in->query) {
    case AMDGPU_INFO_ACCEL_WORKING:
        return info_out(f, in, &dev->accel_working, 4);
    case AMDGPU_INFO_HW_IP_INFO: {
        struct drm_amdgpu_info_hw_ip ip;
        uint32_t t = in->query_hw_ip.type;
        if (t >= AMDGPU_HW_IP_NUM || in->query_hw_ip.ip_instance)
            return -RD_EINVAL;
        zero(&ip, sizeof(ip));
        ip.hw_ip_version_major = dev->ip_ver_major[t];
        ip.hw_ip_version_minor = dev->ip_ver_minor[t];
        ip.ib_start_alignment = 32;
        ip.ib_size_alignment = (t == AMDGPU_HW_IP_DMA) ? 4 : 32;
        ip.available_rings = dev->available_rings[t];
        ip.ip_discovery_version = (dev->ip_ver_major[t] << 16) | (dev->ip_ver_minor[t] << 8);
        return info_out(f, in, &ip, sizeof(ip));
    }
    case AMDGPU_INFO_HW_IP_COUNT: {
        uint32_t t = in->query_hw_ip.type, n;
        if (t >= AMDGPU_HW_IP_NUM)
            return -RD_EINVAL;
        n = dev->available_rings[t] ? 1 : 0;
        return info_out(f, in, &n, 4);
    }
    case AMDGPU_INFO_TIMESTAMP: {
        uint64_t ts = dev->ops.gpu_clock ? dev->ops.gpu_clock(dev->ops.ctx) : 0;
        return info_out(f, in, &ts, 8);
    }
    case AMDGPU_INFO_FW_VERSION: {
        struct drm_amdgpu_info_firmware fw;
        uint32_t t = in->query_fw.fw_type;
        zero(&fw, sizeof(fw));
        if (t < 32) {
            fw.ver = dev->fw_version[t];
            fw.feature = dev->fw_feature[t];
        }
        return info_out(f, in, &fw, sizeof(fw));
    }
    case AMDGPU_INFO_VRAM_USAGE:
        return info_out(f, in, &dev->vram_usage, 8);
    case AMDGPU_INFO_VIS_VRAM_USAGE:
        return info_out(f, in, &dev->vis_vram_usage, 8);
    case AMDGPU_INFO_GTT_USAGE:
        return info_out(f, in, &dev->gtt_usage, 8);
    case AMDGPU_INFO_VRAM_GTT: {
        struct drm_amdgpu_info_vram_gtt vg;
        vg.vram_size = dev->vram_size;
        vg.vram_cpu_accessible_size = dev->vis_vram_size;
        vg.gtt_size = dev->gtt_size;
        return info_out(f, in, &vg, sizeof(vg));
    }
    case AMDGPU_INFO_MEMORY: {
        struct drm_amdgpu_memory_info mi;
        zero(&mi, sizeof(mi));
        mi.vram.total_heap_size = dev->vram_size;
        mi.vram.usable_heap_size = dev->vram_size - (64ull << 20);
        mi.vram.heap_usage = dev->vram_usage;
        mi.vram.max_allocation = mi.vram.usable_heap_size * 3 / 4;
        mi.cpu_accessible_vram.total_heap_size = dev->vis_vram_size;
        mi.cpu_accessible_vram.usable_heap_size = dev->vis_vram_size;
        mi.cpu_accessible_vram.heap_usage = dev->vis_vram_usage;
        mi.cpu_accessible_vram.max_allocation = dev->vis_vram_size * 3 / 4;
        mi.gtt.total_heap_size = dev->gtt_size;
        mi.gtt.usable_heap_size = dev->gtt_size;
        mi.gtt.heap_usage = dev->gtt_usage;
        mi.gtt.max_allocation = dev->gtt_size * 3 / 4;
        return info_out(f, in, &mi, sizeof(mi));
    }
    case AMDGPU_INFO_DEV_INFO:
        return info_out(f, in, &dev->dev_info, sizeof(dev->dev_info));
    case AMDGPU_INFO_VRAM_LOST_COUNTER:
    case AMDGPU_INFO_NUM_EVICTIONS:
    case AMDGPU_INFO_NUM_VRAM_CPU_PAGE_FAULTS:
    case AMDGPU_INFO_NUM_BYTES_MOVED: {
        uint64_t z = 0;
        return info_out(f, in, &z, 8);
    }
    case AMDGPU_INFO_MAX_IBS: {
        uint32_t max_ibs[AMDGPU_HW_IP_NUM];
        uint32_t i;
        for (i = 0; i < AMDGPU_HW_IP_NUM; i++)
            max_ibs[i] = 16;
        return info_out(f, in, max_ibs, sizeof(max_ibs));
    }
    case AMDGPU_INFO_VIDEO_CAPS: {
        struct drm_amdgpu_info_video_caps caps;
        zero(&caps, sizeof(caps));   /* no VCN yet */
        return info_out(f, in, &caps, sizeof(caps));
    }
    case AMDGPU_INFO_READ_MMR_REG: {
        /* soc21_allowed_read_registers (GC 11.0.3 offsets, gc_11_0_3_offset.h) */
        static const uint32_t allowed[] = {
            0x0da4 /* GRBM_STATUS */, 0x0da2 /* GRBM_STATUS2 */, 0x0da5, 0x0da6, 0x0da7,
            0x0da8 /* GRBM_STATUS_SE0..3 */, 0x0f40 /* CP_STAT */, 0x13de /* GB_ADDR_CONFIG */,
        };
        uint32_t n = in->read_mmr_reg.count, vals[64], i, k;
        if (!n || n > 64 || (size_t)in->return_size < n * 4ull)
            return -RD_EINVAL;
        for (i = 0; i < n; i++) {
            uint32_t reg = in->read_mmr_reg.dword_offset + i, rel = reg - dev->gc_base0;
            int ok = 0;
            for (k = 0; k < sizeof(allowed) / sizeof(allowed[0]); k++)
                ok |= rel == allowed[k];
            if (!ok)
                return -RD_EINVAL;
            if (rel == 0x13de)
                vals[i] = dev->gb_addr_config;
            else
                vals[i] = dev->ops.read_reg ? dev->ops.read_reg(dev->ops.ctx, reg) : 0;
        }
        return info_out(f, in, vals, n * 4);
    }
    case AMDGPU_INFO_SENSOR: {
        /* amdgpu_dpm_read_sensor through the OS (SMU metrics) */
        uint32_t v = 0;
        if (!dev->ops.sensor || dev->ops.sensor(dev->ops.ctx, in->sensor_info.type, &v))
            return -RD_EINVAL;
        return info_out(f, in, &v, 4);
    }
    case AMDGPU_INFO_GPUVM_FAULT: {
        /* amdgpu_vm_update_fault_cache: the last fault of this client's VM */
        struct drm_amdgpu_info_gpuvm_fault gf;
        uint64_t addr = 0;
        uint32_t status = 0, hub = 0;
        zero(&gf, sizeof(gf));
        if (dev->ops.vm_fault_info && f->vm &&
            !dev->ops.vm_fault_info(dev->ops.ctx, f->vm, &addr, &status, &hub)) {
            gf.addr = addr;
            gf.status = status;
            gf.vmhub = hub;
        }
        return info_out(f, in, &gf, sizeof(gf));
    }
    case AMDGPU_INFO_GDS_CONFIG: {   /* amdgpu_kms.c: the compute fields only */
        struct drm_amdgpu_info_gds g;
        zero(&g, sizeof(g));
        g.compute_partition_size = RD_GDS_SIZE;
        g.gds_total_size = RD_GDS_SIZE;
        g.gws_per_compute_partition = RD_GWS_SIZE;
        g.oa_per_compute_partition = RD_OA_SIZE;
        return info_out(f, in, &g, sizeof(g));
    }
    case AMDGPU_INFO_VBIOS:
    default:
        return -RD_EINVAL;
    }
}

/* ------------------------------ dispatch ------------------------------ */

#define NEED(type) do { if (size < sizeof(type)) return -RD_EINVAL; } while (0)

int rdna3_drm_ioctl(rdna3_drm_file *f, uint32_t nr, void *data, uint32_t size)
{
    switch (nr) {
    /* ---- DRM core ---- */
    case RD_NR_VERSION:        NEED(struct drm_version); return drm_version(f, (struct drm_version *)data);
    case RD_NR_GET_CAP:        NEED(struct drm_get_cap); return drm_get_cap((struct drm_get_cap *)data);
    case RD_NR_SET_CLIENT_CAP: {
        struct drm_set_client_cap *c = (struct drm_set_client_cap *)data;
        NEED(struct drm_set_client_cap);
        if (c->capability < 64)
            f->client_caps |= (c->value ? 1ull : 0) << c->capability;
        return 0;
    }
    case RD_NR_GEM_CLOSE:      NEED(struct drm_gem_close); return gem_close(f, (struct drm_gem_close *)data);
    case RD_NR_SYNCOBJ_CREATE: NEED(struct drm_syncobj_create);
        return syncobj_create(f, (struct drm_syncobj_create *)data);
    case RD_NR_SYNCOBJ_DESTROY: NEED(struct drm_syncobj_destroy);
        return syncobj_destroy(f, (struct drm_syncobj_destroy *)data);
    case RD_NR_SYNCOBJ_RESET: {
        struct drm_syncobj_array *a = (struct drm_syncobj_array *)data;
        NEED(struct drm_syncobj_array);
        return syncobj_for_each(f, a->handles, a->count_handles, 0, 0, 0);
    }
    case RD_NR_SYNCOBJ_SIGNAL: {
        struct drm_syncobj_array *a = (struct drm_syncobj_array *)data;
        NEED(struct drm_syncobj_array);
        return syncobj_for_each(f, a->handles, a->count_handles, 0, 1, 0);
    }
    case RD_NR_SYNCOBJ_TIMELINE_SIGNAL: {
        struct drm_syncobj_timeline_array *a = (struct drm_syncobj_timeline_array *)data;
        NEED(struct drm_syncobj_timeline_array);
        return syncobj_for_each(f, a->handles, a->count_handles, a->points, 1, 0);
    }
    case RD_NR_SYNCOBJ_QUERY: {
        struct drm_syncobj_timeline_array *a = (struct drm_syncobj_timeline_array *)data;
        NEED(struct drm_syncobj_timeline_array);
        return syncobj_for_each(f, a->handles, a->count_handles, a->points, 3, 0);
    }
    case RD_NR_SYNCOBJ_WAIT: {
        struct drm_syncobj_wait *w = (struct drm_syncobj_wait *)data;
        NEED(struct drm_syncobj_wait);
        w->first_signaled = 0;
        return syncobj_for_each(f, w->handles, w->count_handles, 0, 2, w->timeout_nsec > 0 ? (uint64_t)w->timeout_nsec : 0);
    }
    case RD_NR_SYNCOBJ_TIMELINE_WAIT: {
        struct drm_syncobj_timeline_wait *w = (struct drm_syncobj_timeline_wait *)data;
        NEED(struct drm_syncobj_timeline_wait);
        w->first_signaled = 0;
        return syncobj_for_each(f, w->handles, w->count_handles, w->points, 2, w->timeout_nsec > 0 ? (uint64_t)w->timeout_nsec : 0);
    }

    /* ---- amdgpu ---- */
    case RD_NR_AMDGPU(DRM_AMDGPU_GEM_CREATE): NEED(union drm_amdgpu_gem_create);
        return gem_create(f, (union drm_amdgpu_gem_create *)data);
    case RD_NR_AMDGPU(DRM_AMDGPU_GEM_MMAP): NEED(union drm_amdgpu_gem_mmap);
        return gem_mmap(f, (union drm_amdgpu_gem_mmap *)data);
    case RD_NR_AMDGPU(DRM_AMDGPU_CTX): NEED(union drm_amdgpu_ctx);
        return ctx_ioctl(f, (union drm_amdgpu_ctx *)data);
    case RD_NR_AMDGPU(DRM_AMDGPU_INFO): NEED(struct drm_amdgpu_info);
        return info_ioctl(f, (struct drm_amdgpu_info *)data);
    case RD_NR_AMDGPU(DRM_AMDGPU_GEM_METADATA): NEED(struct drm_amdgpu_gem_metadata);
        return gem_metadata(f, (struct drm_amdgpu_gem_metadata *)data);
    case RD_NR_AMDGPU(DRM_AMDGPU_GEM_WAIT_IDLE): {
        union drm_amdgpu_gem_wait_idle *w = (union drm_amdgpu_gem_wait_idle *)data;
        rdna3_bo *bo;
        NEED(union drm_amdgpu_gem_wait_idle);
        bo = rdna3_drm_bo_lookup(f, w->in.handle);
        if (!bo)
            return -RD_ENOENT;
        w->out.status = 0;   /* idle: nothing runs on the GPU yet */
        w->out.domain = bo->placement;
        return 0;
    }
    case RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA): NEED(struct drm_amdgpu_gem_va);
        return gem_va(f, (struct drm_amdgpu_gem_va *)data);
    case RD_NR_AMDGPU(DRM_AMDGPU_GEM_OP): NEED(struct drm_amdgpu_gem_op);
        return gem_op(f, (struct drm_amdgpu_gem_op *)data);
    case RD_NR_AMDGPU(DRM_AMDGPU_VM): {
        union drm_amdgpu_vm *v = (union drm_amdgpu_vm *)data;
        NEED(union drm_amdgpu_vm);
        if (v->in.op != AMDGPU_VM_OP_RESERVE_VMID && v->in.op != AMDGPU_VM_OP_UNRESERVE_VMID)
            return -RD_EINVAL;
        zero(v, sizeof(*v));
        return 0;
    }
    case RD_NR_AMDGPU(DRM_AMDGPU_WAIT_CS): NEED(union drm_amdgpu_wait_cs);
        if (!f->dev->ops.submit ||
            !ring_ok(f->dev, ((union drm_amdgpu_wait_cs *)data)->in.ip_type,
                     ((union drm_amdgpu_wait_cs *)data)->in.ring)) {
            /* faked ring (rdna3enum) or no submission: nothing ever runs */
            zero(&((union drm_amdgpu_wait_cs *)data)->out, sizeof(struct drm_amdgpu_wait_cs_out));
            return 0;
        }
        return wait_cs(f, (union drm_amdgpu_wait_cs *)data);
    case RD_NR_AMDGPU(DRM_AMDGPU_CS): NEED(union drm_amdgpu_cs);
        return cs_ioctl(f, (union drm_amdgpu_cs *)data);
    case RD_NR_AMDGPU(DRM_AMDGPU_WAIT_FENCES): NEED(union drm_amdgpu_wait_fences);
        if (!f->dev->ops.submit)
            return -RD_ENODEV;
        return wait_fences(f, (union drm_amdgpu_wait_fences *)data);
    case RD_NR_AMDGPU(DRM_AMDGPU_FENCE_TO_HANDLE): NEED(union drm_amdgpu_fence_to_handle);
        if (!f->dev->ops.submit)
            return -RD_ENODEV;
        return fence_to_handle(f, (union drm_amdgpu_fence_to_handle *)data);
    case RD_NR_AMDGPU(DRM_AMDGPU_BO_LIST): {
        NEED(union drm_amdgpu_bo_list);
        return bo_list_ioctl(f, (union drm_amdgpu_bo_list *)data);
    }
    case RD_NR_AMDGPU(DRM_AMDGPU_GEM_USERPTR): NEED(struct drm_amdgpu_gem_userptr);
        return gem_userptr(f, (struct drm_amdgpu_gem_userptr *)data);
    case RD_NR_SYNCOBJ_TRANSFER: NEED(struct drm_syncobj_transfer);
        return syncobj_transfer(f, (struct drm_syncobj_transfer *)data);
    case RD_NR_XNU_BO_EXPORT: NEED(struct rd_xnu_bo_share);
        return bo_export(f, (struct rd_xnu_bo_share *)data);
    case RD_NR_XNU_BO_IMPORT: NEED(struct rd_xnu_bo_share);
        return bo_import(f, (struct rd_xnu_bo_share *)data);
    case RD_NR_XNU_SYNCOBJ_FENCE: NEED(struct rd_xnu_syncobj_fence);
        return syncobj_fence(f, (struct rd_xnu_syncobj_fence *)data);
    case RD_NR_PRIME_HANDLE_TO_FD:               /* libdrm_xnu: RD_NR_XNU_BO_EXPORT/IMPORT */
    case RD_NR_PRIME_FD_TO_HANDLE:
    case RD_NR_SYNCOBJ_HANDLE_TO_FD:             /* fds are the libdrm_xnu layer's (drm_xnu.c) */
    case RD_NR_SYNCOBJ_FD_TO_HANDLE:
        return -RD_ENOSYS;
    default:
        return -RD_EINVAL;
    }
}

const char *rdna3_drm_ioctl_name(uint32_t nr)
{
    switch (nr) {
    case RD_NR_VERSION: return "VERSION";
    case RD_NR_GET_CAP: return "GET_CAP";
    case RD_NR_SET_CLIENT_CAP: return "SET_CLIENT_CAP";
    case RD_NR_GEM_CLOSE: return "GEM_CLOSE";
    case RD_NR_SYNCOBJ_CREATE: return "SYNCOBJ_CREATE";
    case RD_NR_SYNCOBJ_DESTROY: return "SYNCOBJ_DESTROY";
    case RD_NR_SYNCOBJ_WAIT: return "SYNCOBJ_WAIT";
    case RD_NR_SYNCOBJ_RESET: return "SYNCOBJ_RESET";
    case RD_NR_SYNCOBJ_SIGNAL: return "SYNCOBJ_SIGNAL";
    case RD_NR_SYNCOBJ_TRANSFER: return "SYNCOBJ_TRANSFER";
    case RD_NR_XNU_SYNCOBJ_FENCE: return "XNU_SYNCOBJ_FENCE";
    case RD_NR_XNU_BO_EXPORT: return "XNU_BO_EXPORT";
    case RD_NR_XNU_BO_IMPORT: return "XNU_BO_IMPORT";
    case RD_NR_AMDGPU(DRM_AMDGPU_GEM_USERPTR): return "AMDGPU_GEM_USERPTR";
    case RD_NR_AMDGPU(DRM_AMDGPU_GEM_CREATE): return "AMDGPU_GEM_CREATE";
    case RD_NR_AMDGPU(DRM_AMDGPU_GEM_MMAP): return "AMDGPU_GEM_MMAP";
    case RD_NR_AMDGPU(DRM_AMDGPU_CTX): return "AMDGPU_CTX";
    case RD_NR_AMDGPU(DRM_AMDGPU_CS): return "AMDGPU_CS";
    case RD_NR_AMDGPU(DRM_AMDGPU_INFO): return "AMDGPU_INFO";
    case RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA): return "AMDGPU_GEM_VA";
    case RD_NR_AMDGPU(DRM_AMDGPU_WAIT_CS): return "AMDGPU_WAIT_CS";
    default: return "?";
    }
}
