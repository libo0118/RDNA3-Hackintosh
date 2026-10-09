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
    /* TODO(stage2-smu): real clocks from the SMU DPM tables */
    di->max_engine_clock = di->min_engine_clock = 2430000;
    di->max_memory_clock = di->min_memory_clock = 2438000;

    /* TODO(stage2-gfx): harvesting. Assume all CUs/RBs active until
     * CC_GC_SHADER_ARRAY_CONFIG / CC_RB_BACKEND_DISABLE are read. */
    di->cu_active_number = 0;
    for (i = 0; i < se && i < 4; i++)
        for (j = 0; j < sa && j < 4; j++) {
            di->cu_bitmap[i][j] = (cu_per_sa >= 32) ? 0xffffffffu : ((1u << cu_per_sa) - 1);
            di->cu_ao_bitmap[i][j] = MIN(di->cu_bitmap[i][j], 0x3u);
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
    dev->accel_working = on ? 1 : 0;
    dev->available_rings[AMDGPU_HW_IP_GFX] = on ? 1 : 0;
    dev->available_rings[AMDGPU_HW_IP_COMPUTE] = on ? 1 : 0;
    dev->available_rings[AMDGPU_HW_IP_DMA] = on ? 1 : 0;
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

static void bo_put(rdna3_drm_file *f, rdna3_bo *bo)
{
    rdna3_drm_device *dev = f->dev;
    if (--bo->refcnt)
        return;
    if (bo->placement == AMDGPU_GEM_DOMAIN_VRAM) {
        dev->vram_usage -= bo->size;
    } else {
        dev->gtt_usage -= bo->size;
    }
    dev->ops.bo_free(dev->ops.ctx, bo);
    dev->ops.free(dev->ops.ctx, bo, sizeof(*bo));
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

void rdna3_drm_file_close(rdna3_drm_file *f)
{
    rdna3_drm_ops *ops = &f->dev->ops;
    uint32_t i;
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
        if (f->bos[i] && f->bos[i]->mmap_offset == off)
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
    return 0;
}

/* --------------------------- syncobj --------------------------- */
/* With no GPU execution yet, a syncobj is either signaled or not;
 * CS will attach real fences later (TODO(stage4-cs)). */

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
                            uint64_t points_ptr, int op)
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
        case 0: s->signaled = 0; break;                       /* reset */
        case 1: s->signaled = 1; if (pt > s->point) s->point = pt; break; /* signal */
        case 2: if (!s->signaled && (!points_ptr || s->point < pt)) return -RD_ETIME; break;
        case 3: /* query */
            pt = s->signaled ? s->point : 0;
            if (f->dev->ops.copyout(f->dev->ops.ctx, points_ptr + 8ull * i, &pt, 8))
                return -RD_EFAULT;
            break;
        }
    }
    return 0;
}

/* --------------------------- amdgpu GEM --------------------------- */

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
    if (dom & (AMDGPU_GEM_DOMAIN_GDS | AMDGPU_GEM_DOMAIN_GWS | AMDGPU_GEM_DOMAIN_OA |
               AMDGPU_GEM_DOMAIN_DOORBELL))
        return -RD_EOPNOTSUPP; /* TODO(stage4): GDS/GWS/OA/doorbell BOs */

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

static int gem_mmap(rdna3_drm_file *f, union drm_amdgpu_gem_mmap *m)
{
    rdna3_bo *bo = rdna3_drm_bo_lookup(f, m->in.handle);
    if (!bo)
        return -RD_ENOENT;
    if (bo->flags & AMDGPU_GEM_CREATE_NO_CPU_ACCESS)
        return -RD_EPERM;
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
        if (!va->map_size || va->offset_in_bo + va->map_size > bo->size)
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
    case AMDGPU_CTX_OP_QUERY_STATE2:
        if (!id || id >= RD_MAX_CTX || !f->ctx_used[id])
            return -RD_EINVAL;
        zero(c, sizeof(*c));
        return 0;
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
    case AMDGPU_INFO_SENSOR:
    case AMDGPU_INFO_VBIOS:
    case AMDGPU_INFO_GDS_CONFIG:
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
        return syncobj_for_each(f, a->handles, a->count_handles, 0, 0);
    }
    case RD_NR_SYNCOBJ_SIGNAL: {
        struct drm_syncobj_array *a = (struct drm_syncobj_array *)data;
        NEED(struct drm_syncobj_array);
        return syncobj_for_each(f, a->handles, a->count_handles, 0, 1);
    }
    case RD_NR_SYNCOBJ_TIMELINE_SIGNAL: {
        struct drm_syncobj_timeline_array *a = (struct drm_syncobj_timeline_array *)data;
        NEED(struct drm_syncobj_timeline_array);
        return syncobj_for_each(f, a->handles, a->count_handles, a->points, 1);
    }
    case RD_NR_SYNCOBJ_QUERY: {
        struct drm_syncobj_timeline_array *a = (struct drm_syncobj_timeline_array *)data;
        NEED(struct drm_syncobj_timeline_array);
        return syncobj_for_each(f, a->handles, a->count_handles, a->points, 3);
    }
    case RD_NR_SYNCOBJ_WAIT: {
        struct drm_syncobj_wait *w = (struct drm_syncobj_wait *)data;
        NEED(struct drm_syncobj_wait);
        w->first_signaled = 0;
        return syncobj_for_each(f, w->handles, w->count_handles, 0, 2);
    }
    case RD_NR_SYNCOBJ_TIMELINE_WAIT: {
        struct drm_syncobj_timeline_wait *w = (struct drm_syncobj_timeline_wait *)data;
        NEED(struct drm_syncobj_timeline_wait);
        w->first_signaled = 0;
        return syncobj_for_each(f, w->handles, w->count_handles, w->points, 2);
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
    case RD_NR_AMDGPU(DRM_AMDGPU_WAIT_CS): {
        union drm_amdgpu_wait_cs *w = (union drm_amdgpu_wait_cs *)data;
        NEED(union drm_amdgpu_wait_cs);
        zero(&w->out, sizeof(w->out));
        return 0;
    }
    case RD_NR_AMDGPU(DRM_AMDGPU_CS):        /* TODO(stage2-rings): submit IBs */
    case RD_NR_AMDGPU(DRM_AMDGPU_BO_LIST):
    case RD_NR_AMDGPU(DRM_AMDGPU_WAIT_FENCES):
    case RD_NR_AMDGPU(DRM_AMDGPU_FENCE_TO_HANDLE):
        return -RD_ENODEV;
    case RD_NR_AMDGPU(DRM_AMDGPU_GEM_USERPTR):   /* TODO(stage4): wire memory */
    case RD_NR_PRIME_HANDLE_TO_FD:               /* TODO(stage4): IOSurface sharing */
    case RD_NR_PRIME_FD_TO_HANDLE:
    case RD_NR_SYNCOBJ_HANDLE_TO_FD:
    case RD_NR_SYNCOBJ_FD_TO_HANDLE:
    case RD_NR_SYNCOBJ_TRANSFER:
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
