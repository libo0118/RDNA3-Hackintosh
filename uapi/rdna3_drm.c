/*
 * rdna3_drm.c - see rdna3_drm.h. Behaviour follows amdgpu_kms.c,
 * amdgpu_gem.c, amdgpu_ctx.c and drm_syncobj.c (MIT / GPL-2.0 OR MIT)
 * where it matters to userspace; the implementation is our own.
 *
 * SPDX-License-Identifier: MIT
 */
#include "rdna3_drm.h"
#include "amd_fw.h"

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
#define GMC_HOLE_MASK       0x0000ffffffffffffull   /* AMDGPU_GMC_HOLE_MASK */
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
    di->mall_size = 64ull << 20;                 /* 64 MiB Infinity Cache (rdna3amdgpu=1: the MALL table) */
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
        dev->gc_ip_version = AMD_IP_VERSION(ip->major, ip->minor, ip->revision);
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

void rdna3_drm_device_set_tcc_disabled(rdna3_drm_device *dev, uint64_t mask)
{
    /* amdgpu_info_ioctl: dev_info->tcc_disabled_mask = adev->gfx.config.tcc_disabled_mask */
    dev->dev_info.tcc_disabled_mask = mask;
}

void rdna3_drm_device_set_asic_info(rdna3_drm_device *dev, uint32_t gpu_counter_freq_khz,
                                    uint32_t vram_type, uint32_t vram_bit_width, uint64_t mall_size)
{
    struct drm_amdgpu_info_device *di = &dev->dev_info;
    if (gpu_counter_freq_khz)
        di->gpu_counter_freq = gpu_counter_freq_khz;
    if (vram_type)
        di->vram_type = vram_type;
    if (vram_bit_width)
        di->vram_bit_width = vram_bit_width;
    if (mall_size)
        di->mall_size = mall_size;
}

void rdna3_drm_device_set_userq(rdna3_drm_device *dev, uint32_t ip_mask, uint64_t doorbell_size)
{
    dev->userq_ip_mask = ip_mask;
    /* DEV_INFO: dev_info->userq_ip_mask = amdgpu_userq_get_supported_ip_mask() */
    dev->dev_info.userq_ip_mask = ip_mask;
    if (doorbell_size > (uint64_t)RD_MAX_DOORBELL_PAGES * 4096)
        doorbell_size = (uint64_t)RD_MAX_DOORBELL_PAGES * 4096;
    dev->doorbell_size = ip_mask ? doorbell_size : 0;
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
    if (!++dev->next_file_id)
        ++dev->next_file_id;
    f->id = dev->next_file_id;
    f->next_file = dev->files;
    dev->files = f;
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

static void doorbell_pages_set(rdna3_drm_device *dev, uint64_t first, uint64_t n, int on)
{
    uint64_t i;
    for (i = first; i < first + n && i < RD_MAX_DOORBELL_PAGES; i++) {
        if (on)
            dev->doorbell_used[i / 8] |= (uint8_t)(1u << (i % 8));
        else
            dev->doorbell_used[i / 8] &= (uint8_t)~(1u << (i % 8));
    }
}


/* amdgpu_vram_mgr_vis_size: the part of a VRAM BO inside the CPU-visible
 * window [0, vis_vram_size), what AMDGPU_INFO_VIS_VRAM_USAGE and the
 * cpu_accessible_vram heap report (RADV's budget for its host-visible VRAM
 * heap; it stayed 0 here, so the heap always looked empty) */
static uint64_t bo_vis_bytes(const rdna3_drm_device *dev, const rdna3_bo *bo)
{
    uint64_t end;
    if (bo->placement != AMDGPU_GEM_DOMAIN_VRAM || bo->vram_offset >= dev->vis_vram_size)
        return 0;
    end = bo->vram_offset + bo->size;
    return (end < dev->vis_vram_size ? end : dev->vis_vram_size) - bo->vram_offset;
}

static void bo_put_dev(rdna3_drm_device *dev, rdna3_bo *bo)
{
    if (--bo->refcnt)
        return;
    if (bo->placement == AMDGPU_GEM_DOMAIN_DOORBELL) {   /* pages of the doorbell BAR */
        doorbell_pages_set(dev, bo->vram_offset / 4096, bo->size / 4096, 0);
        dev->ops.free(dev->ops.ctx, bo, sizeof(*bo));
        return;
    }
    if (bo_onchip(bo)) {   /* no backing: the range goes back */
        uint32_t total;
        onchip_set(onchip_map(dev, bo->placement, &total), (uint32_t)bo->vram_offset,
                   (uint32_t)bo->size, 0);
        dev->ops.free(dev->ops.ctx, bo, sizeof(*bo));
        return;
    }
    if (bo->foreign) {
        /* the accelerator's VRAM: never counted in vram_usage */
    } else if (bo->placement == AMDGPU_GEM_DOMAIN_VRAM) {
        dev->vram_usage -= bo->size;
        dev->vis_vram_usage -= bo_vis_bytes(dev, bo);
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

void rdna3_drm_bo_get(rdna3_bo *bo)
{
    bo->refcnt++;
}

void rdna3_drm_bo_put(rdna3_drm_device *dev, rdna3_bo *bo)
{
    bo_put_dev(dev, bo);
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
static void userq_mgr_fini(struct rdna3_drm_file *f);
static void seq64_unmap(struct rdna3_drm_file *f);
static void seq64_fini(rdna3_drm_device *dev);
static int ctx_vram_lost(const struct rdna3_drm_file *f, uint32_t ctx_id);

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
    /* the fence drivers the files left (parked references), then seq64 */
    if (dev->fence_drv)
        dev->ops.free(dev->ops.ctx, dev->fence_drv, sizeof(*dev->fence_drv) * RD_MAX_FENCE_DRV);
    if (dev->uq_deferred)
        dev->ops.free(dev->ops.ctx, dev->uq_deferred, sizeof(*dev->uq_deferred) * RD_USERQ_DEFERRED);
    dev->fence_drv = NULL;
    dev->uq_deferred = NULL;
    seq64_fini(dev);
    for (i = 0; i < RD_MAX_EXPORTS; i++)
        if (dev->exports[i].bo) {
            bo_put_dev(dev, dev->exports[i].bo);
            zero(&dev->exports[i], sizeof(dev->exports[i]));
        }
}

static void va_release(rdna3_drm_file *f, rdna3_va *m)
{
    rdna3_drm_ops *ops = &f->dev->ops;
    if (f->vm && ops->vm_unmap && !(m->bo && m->bo->placement == AMDGPU_GEM_DOMAIN_DOORBELL))
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

/* amdgpu_flush (amdgpu_ctx_mgr_entity_flush) and amdgpu_vm_fini
 * (dma_fence_wait on the VM's last fences; TTM frees the page tables and
 * BOs only once the jobs' fences in their reservations signalled): the
 * file's jobs finish before its mappings, VM and BOs go. Card run 50: Chess
 * exited with a job on the GFX ring, its VM was freed under it, the job
 * faulted on the unmapped page tables and the ring hung for everybody.
 * The wait is the OS's fence_wait without a deadline: its job timeout
 * recovers a ring that never gets there (amdgpu_job_timedout). */
static int fence_wait_abs(rdna3_drm_device *dev, const rdna3_fence *fe, uint64_t abs_ns);

static int file_wait_idle(rdna3_drm_file *f)
{
    rdna3_drm_device *dev = f->dev;
    rdna3_fence fe;
    uint32_t ip, ring;
    int idle = 1;
    for (ip = 0; ip < AMDGPU_HW_IP_NUM; ip++)
        for (ring = 0; ring < 4; ring++) {
            if (!f->last_seq[ip][ring])
                continue;
            zero(&fe, sizeof(fe));
            fe.ip = ip;
            fe.ring = ring;
            fe.seq = f->last_seq[ip][ring];
            if (fence_wait_abs(dev, &fe, UINT64_MAX) && !fence_done(dev, &fe))
                idle = 0;   /* not even the recovery got it through */
            f->last_seq[ip][ring] = 0;
        }
    return idle;
}

void rdna3_drm_file_close(rdna3_drm_file *f)
{
    rdna3_drm_ops *ops = &f->dev->ops;
    rdna3_drm_file **pp;
    uint32_t i;
    int idle;
    if (f->closing)
        return;   /* a second close of the same file (clientClose and stop) */
    f->closing = 1;
    idle = file_wait_idle(f);
    for (pp = &f->dev->files; *pp; pp = &(*pp)->next_file)
        if (*pp == f) {
            *pp = f->next_file;
            break;
        }
    f->next_file = NULL;
    f->id = 0;
    userq_mgr_fini(f);   /* amdgpu_driver_postclose_kms: amdgpu_userq_mgr_fini */
    uf_reap(f->dev, 0);
    /* what outlives the file counts against nobody (a new file may get its
     * address) */
    for (i = 0; f->dev->fence_drv && i < RD_MAX_FENCE_DRV; i++)
        if (f->dev->fence_drv[i].owner == f)
            f->dev->fence_drv[i].owner = NULL;
    for (i = 0; f->dev->uq_deferred && i < RD_USERQ_DEFERRED; i++)
        if (f->dev->uq_deferred[i].file == f)
            f->dev->uq_deferred[i].file = NULL;
    for (i = 0; i < RD_MAX_BO_LISTS; i++)
        bo_list_release(f, i);
    /* !idle: a job of this file still runs after the job timeout's
     * recovery. Its mappings, page tables and BOs stay (amdgpu: TTM frees
     * them only once the fences in their reservations signal, which these
     * never do): leaked rather than freed under the job; the tables that
     * only name them go */
    if (f->vas && idle)
        for (i = 0; i < f->nvas; i++)
            if (f->vas[i].handle)
                va_release(f, &f->vas[i]);
    if (idle)
        seq64_unmap(f);
    if (f->vm && ops->vm_destroy && idle)
        ops->vm_destroy(ops->ctx, f->vm);
    f->vm = NULL;
    if (f->bos) {
        for (i = 0; i < RD_MAX_HANDLES && idle; i++)
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
    /* closing stays set: an ioctl or mmap that waited for the OS lock meets
     * a dead file (rdna3_drm_file_open zeroes it again) */
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
    uint32_t i, free_slot = RD_MAX_EXPORTS, mine = 0;
    if (!bo || bo_onchip(bo))
        return -RD_EINVAL;
    /* a surface's VRAM is reached through RD_NR_XNU_SURFACE_BO, which asks
     * the IOSurface rules of each importer; a token would skip them */
    if (bo->userptr || bo->foreign)
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
        if (dev->exports[i].bo && dev->exports[i].owner == f)
            mine++;
    }
    if (free_slot == RD_MAX_EXPORTS || mine >= RD_MAX_EXPORTS_PER_FILE)
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

static rdna3_userq_fence_drv *fdrv_find(rdna3_drm_device *dev, uint32_t ctx);
static int userq_fence_done(rdna3_drm_device *dev, const rdna3_fence *fe);
static int userq_fence_wait(rdna3_drm_device *dev, const rdna3_fence *fe, uint64_t abs_ns);

static int fence_done(rdna3_drm_device *dev, const rdna3_fence *fe)
{
    if (fe->ip == RD_USERQ_FENCE_IP)
        return userq_fence_done(dev, fe);
    /* a kernel ring compares its 32-bit fence modulo 2^32: a handle half
     * the space behind the last submission passed long ago */
    if (fe->ip < AMDGPU_HW_IP_NUM && fe->ring < 4 && fe->seq <= dev->last_seq[fe->ip][fe->ring] &&
        dev->last_seq[fe->ip][fe->ring] - fe->seq >= (1ull << 31))
        return 1;
    if (!dev->ops.fence_signaled)
        return 1;
    return dev->ops.fence_signaled(dev->ops.ctx, fe->ip, fe->ring, fe->seq);
}

static int fence_wait_abs(rdna3_drm_device *dev, const rdna3_fence *fe, uint64_t abs_ns)
{
    if (fe->ip == RD_USERQ_FENCE_IP)
        return userq_fence_wait(dev, fe, abs_ns);
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

/* how long wait-any slices before one full wait (the OS's hang cap, 10 s) */
#define RD_WAIT_SLICE_LIMIT_NS (10ull * 1000 * 1000 * 1000)

/* drm_syncobj_array_wait (SYNCOBJ_WAIT / TIMELINE_WAIT). WAIT_ALL: every
 * syncobj, in turn. Without it: the first one ready, its index in
 * first_signaled (vkWaitForFences with waitAll = false). A syncobj with no
 * fence (or a timeline point not there yet) is -EINVAL unless
 * WAIT_FOR_SUBMIT, as in DRM; with it the wait runs out instead, since no
 * other client can submit while this one holds the device. PR #2 review:
 * the flags were ignored and every wait was a wait-all. */
static int syncobj_wait(rdna3_drm_file *f, uint64_t handles_ptr, uint32_t count, uint64_t points_ptr,
                        uint32_t flags, uint64_t abs_ns, uint32_t *first)
{
    rdna3_drm_device *dev = f->dev;
    int waited = 0;
    uint64_t t0 = dev->ops.now_ns ? dev->ops.now_ns(dev->ops.ctx) : 0;
    *first = 0;
    if (flags & ~(uint32_t)(DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
                            DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE))
        return -RD_EINVAL;
    if (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL)
        return syncobj_for_each(f, handles_ptr, count, points_ptr, 2, abs_ns);
    for (;;) {
        rdna3_fence fe = { 0, 0, 0 }, fk = { 0, 0, 0 };
        uint32_t i, h, pending = 0, fi = 0, fki = 0, kernel = 0;
        uint64_t pt = 0, now, until;
        for (i = 0; i < count; i++) {
            rdna3_syncobj *s;
            if (dev->ops.copyin(dev->ops.ctx, &h, handles_ptr + 4ull * i, 4))
                return -RD_EFAULT;
            if (points_ptr && dev->ops.copyin(dev->ops.ctx, &pt, points_ptr + 8ull * i, 8))
                return -RD_EFAULT;
            s = sobj(f, h);
            if (!s)
                return -RD_ENOENT;
            if (sobj_signaled(dev, s) && (!points_ptr || s->point >= pt)) {
                *first = i;
                return 0;
            }
            if (s->has_fence) {
                if (!pending) {
                    fe = s->fence;
                    fi = i;
                }
                if (!kernel && s->fence.ip != RD_USERQ_FENCE_IP) {
                    fk = s->fence;
                    fki = i;
                    kernel = 1;
                }
                pending++;
            } else if (!(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT)) {
                return -RD_EINVAL;
            }
        }
        if (!abs_ns || !pending || waited)
            return -RD_ETIME;
        /* one fence at a time: with several pending, short slices so a
         * later one that finishes first is seen; the fence by value, the
         * syncobjs looked up again after the wait (the OS lock may drop).
         * The OS caps each wait call at 10 s and recovers a hang there; a
         * 1 ms slice never gets that far, so after 10 s of slices the wait
         * goes on the first pending fence with the whole deadline (PR #2
         * follow-up review: a hung GPU was never recovered) */
        until = abs_ns;
        if (pending > 1 && dev->ops.now_ns) {
            now = dev->ops.now_ns(dev->ops.ctx);
            if (now - t0 < RD_WAIT_SLICE_LIMIT_NS && abs_ns > now && abs_ns - now > 1000000)
                until = now + 1000000;
        }
        if (until == abs_ns) {
            waited = 1;
            /* the full wait goes on a kernel ring's fence when there is
             * one: the OS recovers a hung kernel ring at its cap, a user
             * queue's wait only stops there (PR #2 review) */
            if (kernel) {
                fe = fk;
                fi = fki;
            }
        }
        /* that fence signalled: done, even if its syncobj went away while
         * the lock was dropped (DRM holds the fence, not the syncobj) */
        if (!fence_wait_abs(dev, &fe, until)) {
            *first = fi;
            return 0;
        }
    }
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
    if (ip == AMDGPU_HW_IP_VCN_ENC && ring < RD_VCN_MAX_INST && (dev->vcn_inst_ready & (1u << ring)))
        return 1;   /* the VCN instance a context's VCN_ENC entity runs on */
    return ip < AMDGPU_HW_IP_NUM && ring < 4 && (dev->ready_rings[ip] & (1u << ring));
}

/* amdgpu_ctx_get_fence: a sequence number that was never submitted is
 * -EINVAL, not something to wait for (it would only end at the hang cap,
 * with a GPU reset) */
/* the kernel rings count their fences in 32 bits (amdgpu_fence_emit's
 * ring->fence_drv.sync_seq), the handles userspace gets never wrap (the
 * scheduler entity's 64-bit sequence): the ring's answer, widened past
 * the last one this ring gave */
static uint64_t seq_extend(const rdna3_drm_device *dev, uint32_t ip, uint32_t ring, uint64_t seq)
{
    uint64_t last = dev->last_seq[ip][ring];
    return last + (uint32_t)((uint32_t)seq - (uint32_t)last);
}

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
    if (x->seq && x->ip == RD_USERQ_FENCE_IP) {
        /* a user queue fence: a context this device made, a value its
         * queue signaled; a context that is gone has signaled */
        rdna3_userq_fence_drv *fd = fdrv_find(f->dev, x->ring);
        if (!x->ring || x->ring > f->dev->next_fence_ctx || (fd && x->seq > fd->last_seq))
            return -RD_EINVAL;
        if (!fd) {
            s->signaled = 1;
            s->has_fence = 0;
            return 0;
        }
    } else if (x->seq && (!ring_ok(f->dev, x->ip, x->ring) ||
                          !seq_submitted(f->dev, x->ip, x->ring, x->seq))) {
        return -RD_EINVAL;
    }
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

/* ------------------------- VCN 4.0 / JPEG 4.0 ------------------------- */

static rdna3_va *va_lookup(rdna3_drm_file *f, uint64_t addr);

/* soc15d.h / jpeg_v2_0.h: the JRBC packets jpeg_v2_dec_ring_parse_cs allows */
#define RD_PACKETJ_TYPE0          0
#define RD_PACKETJ_TYPE3          3
#define RD_PACKETJ_TYPE6          6
#define RD_PACKETJ_CONDITION_CHECK0 0
#define RD_PACKETJ_CONDITION_CHECK3 3
#define RD_CP_PACKETJ_NOP         0x60000000u
#define RD_JPEG_REG_RANGE_START   0x4000
#define RD_JPEG_REG_RANGE_END     0x41c2
/* vcn_v4_0.c */
#define RD_RDECODE_MSG_CREATE                   0x00000000
#define RD_RDECODE_MESSAGE_CREATE               0x00000001
#define RD_RADEON_VCN_ENGINE_TYPE_ENCODE        0x00000002
#define RD_RADEON_VCN_ENGINE_TYPE_DECODE        0x00000003
#define RD_RADEON_VCN_ENGINE_INFO               0x30000001
#define RD_RADEON_VCN_ENGINE_INFO_MAX_OFFSET    16
#define RD_RENCODE_ENCODE_STANDARD_AV1          2
#define RD_RENCODE_IB_PARAM_SESSION_INIT        0x00000003
#define RD_RENCODE_IB_PARAM_SESSION_INIT_MAX_OFFSET 64
#define RD_AMDGPU_VCN_HARVEST_VCN0              (1u << 0)

void rdna3_drm_device_set_vcn(rdna3_drm_device *dev, uint32_t ip_version, uint32_t num_inst,
                              uint32_t harvest_config, uint32_t inst_ready, int jpeg_ready)
{
    dev->vcn_ip_version = ip_version;
    dev->vcn_num_inst = num_inst > RD_VCN_MAX_INST ? RD_VCN_MAX_INST : num_inst;
    dev->vcn_harvest = harvest_config;
    dev->vcn_inst_ready = inst_ready & ((1u << dev->vcn_num_inst) - 1) & ~harvest_config;
    dev->jpeg_ready = jpeg_ready ? 1 : 0;
    /* the IP blocks vcn_v4_0_ip_block / jpeg_v4_0_ip_block: 4.0 */
    dev->ip_ver_major[AMDGPU_HW_IP_VCN_DEC] = dev->ip_ver_major[AMDGPU_HW_IP_VCN_ENC] = 4;
    dev->ip_ver_minor[AMDGPU_HW_IP_VCN_DEC] = dev->ip_ver_minor[AMDGPU_HW_IP_VCN_ENC] = 0;
    dev->ip_ver_major[AMDGPU_HW_IP_VCN_JPEG] = 4;
    dev->ip_ver_minor[AMDGPU_HW_IP_VCN_JPEG] = 0;
    /* one VCN_ENC ring for userspace (amdgpu_ctx_num_entities), whichever
     * instance runs it; the JPEG ring */
    if (dev->vcn_inst_ready)
        rdna3_drm_device_set_ring_ready(dev, AMDGPU_HW_IP_VCN_ENC, 1);
    if (dev->jpeg_ready)
        rdna3_drm_device_set_ring_ready(dev, AMDGPU_HW_IP_VCN_JPEG, 1);
}

static int vcn_up(const rdna3_drm_device *dev, uint32_t ip)
{
    return dev->vcn_num_inst &&
           (ip == AMDGPU_HW_IP_VCN_DEC || ip == AMDGPU_HW_IP_VCN_ENC || ip == AMDGPU_HW_IP_VCN_JPEG);
}

/* the hardware ring a (context, ip, ring) fence lives on: a VCN_ENC
 * context's entity runs on one VCN instance (amdgpu_ctx_init_entity) */
static uint32_t entity_ring(const rdna3_drm_file *f, uint32_t ctx, uint32_t ip, uint32_t ring)
{
    if (ip == AMDGPU_HW_IP_VCN_ENC && ring == 0 && ctx < RD_MAX_CTX && f->vcn_entity[ctx])
        return f->vcn_entity[ctx] - 1u;
    return ring;
}

/* drm_sched_pick_best over the VCN_ENC schedulers (amdgpu_ctx_init_entity
 * pins a video entity to one): the lowest score wins, VCN0 starts with
 * one (vcn_v4_0_sw_init: "scheduled after other instances"); a job still
 * running on an instance counts as its load */
static uint32_t vcn_pick_best(rdna3_drm_file *f)
{
    rdna3_drm_device *dev = f->dev;
    uint32_t i, best = 0, best_score = ~0u;
    for (i = 0; i < dev->vcn_num_inst; i++) {
        uint32_t score;
        rdna3_fence fe;
        if (!(dev->vcn_inst_ready & (1u << i)))
            continue;
        fe.ip = AMDGPU_HW_IP_VCN_ENC;
        fe.ring = i;
        fe.seq = dev->last_seq[AMDGPU_HW_IP_VCN_ENC][i];
        score = (i == 0 ? 1u : 0u) + ((fe.seq && !fence_done(dev, &fe)) ? 1u : 0u);
        if (score < best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
}

/* Ported from vcn_v4_0_limit_sched: the create message must be in the
 * first IB of the entity, which then moves to VCN0 for good */
static int vcn_v4_0_limit_sched(rdna3_drm_file *f, uint32_t ctx, uint32_t *inst)
{
    if (ctx < RD_MAX_CTX && f->vcn_entity_jobs[ctx])
        return -RD_EINVAL;
    /* if VCN0 is harvested, we can't support AV1 */
    if (f->dev->vcn_harvest & RD_AMDGPU_VCN_HARVEST_VCN0)
        return -RD_EINVAL;
    *inst = 0;   /* drm_sched_entity_modify_sched(VCN_ENC prio 0 scheds, 1) */
    if (ctx < RD_MAX_CTX)
        f->vcn_entity[ctx] = 1;
    return 0;
}

/* amdgpu_cs_find_mapping + amdgpu_bo_kmap: the CPU view of [addr, addr + 4)
 * and where its mapping ends (VA); NULL when unmapped or not CPU visible */
static const uint8_t *cs_map(rdna3_drm_file *f, uint64_t addr, uint64_t *start, uint64_t *end)
{
    rdna3_drm_ops *ops = &f->dev->ops;
    rdna3_va *m = va_lookup(f, addr);
    uint8_t *p;
    if (!m || !ops->bo_kmap)
        return NULL;
    p = (uint8_t *)ops->bo_kmap(ops->ctx, m->bo);
    if (!p)
        return NULL;
    *start = m->va & 0x0000ffffffffffffull;
    *end = *start + m->size;
    return p + m->offset;
}

/* Ported from vcn_v4_0_dec_msg: a decode create message that names a codec
 * other than H264 / HEVC / VP9 (create[0] 0x7 / 0x10 / 0x11) needs VCN0 */
static int vcn_v4_0_dec_msg(rdna3_drm_file *f, uint32_t ctx, uint32_t *inst, uint64_t addr)
{
    uint64_t start, end;
    const uint8_t *ptr;
    uint32_t msg[6], num_buffers, i;
    int r = 0;

    addr &= 0x0000ffffffffffffull;   /* AMDGPU_GMC_HOLE_MASK */
    ptr = cs_map(f, addr, &start, &end);
    if (!ptr)
        return -RD_EINVAL;           /* Can't find BO for addr */
    if (addr & 0x7)
        return -RD_EINVAL;           /* VCN messages must be 8 byte aligned */
    if (end - addr < sizeof(msg))
        return -RD_EINVAL;
    copy(msg, ptr + (addr - start), sizeof(msg));
    /* Check length */
    if (msg[1] > end - addr)
        return -RD_EINVAL;
    if (msg[3] != RD_RDECODE_MSG_CREATE)
        return 0;
    num_buffers = msg[2];
    for (i = 0; i < num_buffers; ++i) {
        uint64_t at = addr + 24 + 16ull * i;
        uint32_t ent[4], offset, size, create0;
        if (at + 16 > end)
            return -RD_EINVAL;
        copy(ent, ptr + (at - start), sizeof(ent));
        if (ent[0] != RD_RDECODE_MESSAGE_CREATE)
            continue;
        offset = ent[1];
        size = ent[2];
        if (offset + size > end)
            return -RD_EINVAL;
        if (addr + offset + 4 > end)
            return -RD_EINVAL;
        copy(&create0, ptr + (addr + offset - start), 4);
        /* H264, HEVC and VP9 can run on any instance */
        if (create0 == 0x7 || create0 == 0x10 || create0 == 0x11)
            continue;
        r = vcn_v4_0_limit_sched(f, ctx, inst);
        if (r)
            return r;
    }
    return r;
}

/* Ported from vcn_v4_0_enc_find_ib_param: the offset of package 'id' within
 * the first max_offset dwords, -1 if absent */
static int vcn_v4_0_enc_find_ib_param(const uint32_t *ib, uint32_t length_dw, uint32_t id, int max_offset)
{
    uint32_t i;
    for (i = 0; i < length_dw && (int)i < max_offset && ib[i] >= 8; i += ib[i] / 4) {
        if (i + 1 < length_dw && ib[i + 1] == id)
            return (int)i;
    }
    return -1;
}

/* Ported from vcn_v4_0_ring_patch_cs_in_place: on VCN1, AV1 encode sessions
 * and decode sessions of other codecs than H264 / HEVC / VP9 go to VCN0 */
static int vcn_v4_0_ring_patch_cs_in_place(rdna3_drm_file *f, uint32_t ctx, uint32_t *inst, const uint32_t *ib,
                                           uint32_t length_dw)
{
    int idx;
    uint32_t val;
    /* The first instance can decode anything */
    if (!*inst)
        return 0;
    /* RADEON_VCN_ENGINE_INFO is at the top of ib block */
    idx = vcn_v4_0_enc_find_ib_param(ib, length_dw, RD_RADEON_VCN_ENGINE_INFO, RD_RADEON_VCN_ENGINE_INFO_MAX_OFFSET);
    if (idx < 0)   /* engine info is missing */
        return 0;
    if ((uint32_t)idx + 2 >= length_dw)
        return 0;
    val = ib[idx + 2];   /* RADEON_VCN_ENGINE_TYPE */
    if (val == RD_RADEON_VCN_ENGINE_TYPE_DECODE) {
        uint64_t addr;
        if ((uint32_t)idx + 9 > length_dw)
            return 0;
        /* struct amdgpu_vcn_decode_buffer at idx + 6 */
        if (!(ib[idx + 6] & 0x1))
            return 0;
        addr = ((uint64_t)ib[idx + 7]) << 32 | ib[idx + 8];
        return vcn_v4_0_dec_msg(f, ctx, inst, addr);
    } else if (val == RD_RADEON_VCN_ENGINE_TYPE_ENCODE) {
        idx = vcn_v4_0_enc_find_ib_param(ib, length_dw, RD_RENCODE_IB_PARAM_SESSION_INIT,
                                         RD_RENCODE_IB_PARAM_SESSION_INIT_MAX_OFFSET);
        if (idx >= 0 && (uint32_t)idx + 2 < length_dw && ib[idx + 2] == RD_RENCODE_ENCODE_STANDARD_AV1)
            return vcn_v4_0_limit_sched(f, ctx, inst);
    }
    return 0;
}

/* Ported from jpeg_v2_dec_ring_parse_cs: JPEG register writes and NOPs only */
static int jpeg_v2_dec_ring_parse_cs(const uint32_t *ib, uint32_t length_dw)
{
    uint32_t i, reg, res, cond, type;
    for (i = 0; i < length_dw; i += 2) {
        reg = ib[i] & 0x3FFFF;
        res = (ib[i] >> 18) & 0x3F;
        cond = (ib[i] >> 24) & 0xF;
        type = (ib[i] >> 28) & 0xF;
        if (res)   /* only support 0 at the moment */
            return -RD_EINVAL;
        switch (type) {
        case RD_PACKETJ_TYPE0:
            if (cond != RD_PACKETJ_CONDITION_CHECK0 || reg < RD_JPEG_REG_RANGE_START || reg > RD_JPEG_REG_RANGE_END)
                return -RD_EINVAL;
            break;
        case RD_PACKETJ_TYPE3:
            if (cond != RD_PACKETJ_CONDITION_CHECK3 || reg < RD_JPEG_REG_RANGE_START || reg > RD_JPEG_REG_RANGE_END)
                return -RD_EINVAL;
            break;
        case RD_PACKETJ_TYPE6:
            if (ib[i] == RD_CP_PACKETJ_NOP)
                continue;
            return -RD_EINVAL;
        default:
            return -RD_EINVAL;   /* Unknown packet type */
        }
    }
    return 0;
}

/* Ported from amdgpu_cs_patch_ibs for the video rings: the IB must sit in
 * one mapping of the VM; VCN_ENC (patch_cs_in_place) may move the entity
 * to VCN0, VCN_JPEG (parse_cs) runs a checked copy ('copies' gets the
 * zalloc'ed buffers, freed by the caller) */
static int cs_patch_ibs(rdna3_drm_file *f, uint32_t ctx, uint32_t ip, uint32_t *inst, rdna3_ib *ibs,
                        uint32_t nibs, uint32_t **copies)
{
    rdna3_drm_ops *ops = &f->dev->ops;
    uint32_t i;
    int r;
    for (i = 0; i < nibs; i++) {
        uint64_t va = ibs[i].va & 0x0000ffffffffffffull, start, end;
        const uint8_t *kptr = cs_map(f, va, &start, &end);
        uint64_t bytes = 4ull * ibs[i].length_dw;
        rdna3_va *m = va_lookup(f, va);
        if (!m)
            return -RD_EINVAL;   /* IB va_start is invalid */
        if (va + bytes > (m->va & 0x0000ffffffffffffull) + m->size)
            return -RD_EINVAL;   /* IB va_start+ib_bytes is invalid */
        if (ip == AMDGPU_HW_IP_VCN_JPEG) {
            if (!kptr)
                return -RD_EINVAL;
            copies[i] = (uint32_t *)ops->zalloc(ops->ctx, (size_t)bytes);
            if (!copies[i])
                return -RD_ENOMEM;
            copy(copies[i], kptr + (va - start), (size_t)bytes);
            r = jpeg_v2_dec_ring_parse_cs(copies[i], ibs[i].length_dw);
            if (r)
                return r;
            ibs[i].kdata = copies[i];
        } else if (*inst) {
            /* the in-place check reads the user's IB */
            uint32_t n = ibs[i].length_dw < 128 ? ibs[i].length_dw : 128, head[128];
            if (!kptr)
                return -RD_EINVAL;
            copy(head, kptr + (va - start), 4ull * n);
            r = vcn_v4_0_ring_patch_cs_in_place(f, ctx, inst, head, n);
            if (r)
                return r;
        }
    }
    return 0;
}

/* soc21.c: vcn_4_0_0_video_codecs_{encode,decode}_array_vcn{0,1}
 * (codec_info_build: type, width, height, pixels = width * height, level) */
typedef struct { uint32_t type, w, h, level; } rd_codec;
static const rd_codec vcn_4_0_0_video_codecs_encode_array_vcn0[] = {
    { AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_MPEG4_AVC, 4096, 4096, 0 },
    { AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_HEVC, 8192, 4352, 0 },
    { AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_AV1, 8192, 4352, 0 },
};
static const rd_codec vcn_4_0_0_video_codecs_encode_array_vcn1[] = {
    { AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_MPEG4_AVC, 4096, 4096, 0 },
    { AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_HEVC, 8192, 4352, 0 },
};
static const rd_codec vcn_4_0_0_video_codecs_decode_array_vcn0[] = {
    { AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_MPEG4_AVC, 4096, 4096, 52 },
    { AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_HEVC, 8192, 4352, 186 },
    { AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_JPEG, 16384, 16384, 0 },
    { AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_VP9, 8192, 4352, 0 },
    { AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_AV1, 8192, 4352, 0 },
};
static const rd_codec vcn_4_0_0_video_codecs_decode_array_vcn1[] = {
    { AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_MPEG4_AVC, 4096, 4096, 52 },
    { AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_HEVC, 8192, 4352, 186 },
    { AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_JPEG, 16384, 16384, 0 },
    { AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_VP9, 8192, 4352, 0 },
};
#define RD_N(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

/* Ported from soc21_query_video_codecs (VCN 4.0.0 / 4.0.2 / 4.0.4 / 4.0.5,
 * not a VF): VCN1's tables when VCN0 is harvested */
static int soc21_query_video_codecs(const rdna3_drm_device *dev, int encode, const rd_codec **codecs,
                                    uint32_t *count)
{
    uint32_t i, harvested = 0;
    for (i = 0; i < 8; i++)
        harvested += (dev->vcn_harvest >> i) & 1;
    if (dev->vcn_num_inst == harvested)
        return -RD_EINVAL;
    switch (dev->vcn_ip_version) {
    case AMD_IP_VERSION(4, 0, 0):
    case AMD_IP_VERSION(4, 0, 2):
    case AMD_IP_VERSION(4, 0, 4):
    case AMD_IP_VERSION(4, 0, 5):
        if (dev->vcn_harvest & RD_AMDGPU_VCN_HARVEST_VCN0) {
            *codecs = encode ? vcn_4_0_0_video_codecs_encode_array_vcn1 : vcn_4_0_0_video_codecs_decode_array_vcn1;
            *count = encode ? RD_N(vcn_4_0_0_video_codecs_encode_array_vcn1)
                            : RD_N(vcn_4_0_0_video_codecs_decode_array_vcn1);
        } else {
            *codecs = encode ? vcn_4_0_0_video_codecs_encode_array_vcn0 : vcn_4_0_0_video_codecs_decode_array_vcn0;
            *count = encode ? RD_N(vcn_4_0_0_video_codecs_encode_array_vcn0)
                            : RD_N(vcn_4_0_0_video_codecs_decode_array_vcn0);
        }
        return 0;
    default:
        return -RD_EINVAL;
    }
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
    uint32_t *copies[RD_MAX_CS_IBS];    /* VCN_JPEG: the checked IB copies */
    uint32_t ctx = cs->in.ctx_id, hw_ring;
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
    /* amdgpu_cs_pass1: ctx->generation != the job's generation - a context
     * made before VRAM was lost submits nothing more; a new one does */
    if (ctx_vram_lost(f, ctx))
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
            if (!ring_ok(dev, ib.ip_type, ib.ring) || (ib.ip_type == AMDGPU_HW_IP_VCN_ENC && ib.ring))
                return dev->enumeration_only ? -RD_ENODEV : -RD_EINVAL;  /* faked ring */
            if (ib.flags & (AMDGPU_IB_FLAG_PREAMBLE | AMDGPU_IB_FLAG_CE))
                continue;            /* no CE on GFX11; preambles can be dropped */
            ip = ib.ip_type;
            ring = ib.ring;
            ibs[nibs].va = ib.va_start;
            ibs[nibs].length_dw = ib.ib_bytes / 4;
            ibs[nibs].flags = ib.flags;
            ibs[nibs].kdata = NULL;
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
                fe.ring = entity_ring(f, dep.ctx_id, dep.ip_type, dep.ring);
                fe.seq = dep_seq(dev, dep.ip_type, fe.ring, dep.handle);
                /* amdgpu_ctx_get_fence: a sequence number never submitted
                 * is -EINVAL; waited on, it would run into the hang cap and
                 * a reset that hits other clients (PR #2 review) */
                if (!seq_submitted(dev, fe.ip, fe.ring, fe.seq))
                    return -RD_EINVAL;
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
        case AMDGPU_CHUNK_ID_CP_GFX_SHADOW: {
            /* amdgpu_cs_p2_shadow: only the flags are checked; the VAs
             * (0 = no shadowing) go to the job as they are, the last
             * chunk wins. A short chunk reads as zeros. */
            struct drm_amdgpu_cs_chunk_cp_gfx_shadow sh;
            zero(&sh, sizeof(sh));
            if (ops->copyin(ops->ctx, &sh, ch.chunk_data,
                            MIN((uint64_t)ch.length_dw * 4, sizeof(sh))))
                return -RD_EFAULT;
            if (sh.flags & ~(uint64_t)AMDGPU_CS_CHUNK_CP_GFX_SHADOW_FLAGS_INIT_SHADOW)
                return -RD_EINVAL;
            job.shadow_va = sh.shadow_va;
            job.csa_va = sh.csa_va;
            job.gds_va = sh.gds_va;
            job.init_shadow = (sh.flags & AMDGPU_CS_CHUNK_CP_GFX_SHADOW_FLAGS_INIT_SHADOW) ? 1 : 0;
            break;
        }
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

    /* the video rings: the context's VCN_ENC entity picks its instance on
     * first use (amdgpu_ctx_init_entity), amdgpu_cs_patch_ibs checks the
     * IBs (and may move the entity to VCN0), JPEG runs checked copies */
    hw_ring = ring;
    if (ip == AMDGPU_HW_IP_VCN_ENC || ip == AMDGPU_HW_IP_VCN_JPEG) {
        zero(copies, sizeof(copies));
        if (ip == AMDGPU_HW_IP_VCN_ENC) {
            if (ctx < RD_MAX_CTX && !f->vcn_entity[ctx])
                f->vcn_entity[ctx] = (uint8_t)(vcn_pick_best(f) + 1);
            hw_ring = entity_ring(f, ctx, ip, ring);
        }
        r = cs_patch_ibs(f, ctx, ip, &hw_ring, ibs, nibs, copies);
        if (!r) {
            job.seq_base = dev->last_seq[ip][hw_ring];
            r = ops->submit(ops->ctx, f->vm, ip, hw_ring, ibs, nibs, &job, &seq);
        }
        for (i = 0; i < nibs; i++)
            if (copies[i])
                ops->free(ops->ctx, copies[i], 4ull * ibs[i].length_dw);
        if (r)
            return r;
        if (ip == AMDGPU_HW_IP_VCN_ENC && ctx < RD_MAX_CTX)
            f->vcn_entity_jobs[ctx]++;
        ring = hw_ring;
    } else {
        job.seq_base = dev->last_seq[ip][ring];
        r = ops->submit(ops->ctx, f->vm, ip, ring, ibs, nibs, &job, &seq);
        if (r)
            return r;
    }
    seq = seq_extend(dev, ip, ring, seq);
    dev->last_seq[ip][ring] = seq;
    f->last_seq[ip][ring] = seq;
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
    fe.ring = entity_ring(f, w->in.ctx_id, w->in.ip_type, w->in.ring);
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
    rdna3_fence fe0 = { 0, 0, 0 };
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
        fe.ring = entity_ring(f, af.ctx_id, af.ip_type, af.ring);
        fe.seq = af.seq_no;
        if (!seq_submitted(dev, fe.ip, fe.ring, fe.seq))
            return -RD_EINVAL;
        if (!i)
            fe0 = fe;
        if (fence_done(dev, &fe) || (all && !fence_wait_abs(dev, &fe, timeout))) {
            done++;
            if (first == ~0u)
                first = i;
            if (!all)
                break;
        }
    }
    if (!all && !done) {
        /* wait for the first one; good enough for a single ring. The fence
         * the loop validated, not a second read of user memory */
        rdna3_fence fe = fe0;
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
    /* amdgpu_cs_get_fence -> amdgpu_ctx_get_fence: never submitted = -EINVAL */
    {
        uint32_t ring = entity_ring(f, h->in.fence.ctx_id, h->in.fence.ip_type, h->in.fence.ring);
        if (!seq_submitted(f->dev, h->in.fence.ip_type, ring,
                           dep_seq(f->dev, h->in.fence.ip_type, ring, h->in.fence.seq_no)))
            return -RD_EINVAL;
    }
    zero(&c, sizeof(c));
    r = syncobj_create(f, &c);
    if (r)
        return r;
    s = sobj(f, c.handle);
    s->fence.ip = h->in.fence.ip_type;
    s->fence.ring = entity_ring(f, h->in.fence.ctx_id, h->in.fence.ip_type, h->in.fence.ring);
    s->fence.seq = dep_seq(f->dev, s->fence.ip, s->fence.ring, h->in.fence.seq_no);
    s->has_fence = s->fence.seq != 0;
    s->signaled = !s->has_fence;
    zero(&h->out, sizeof(h->out));
    h->out.handle = c.handle;
    return 0;
}

/* --------------------------- amdgpu GEM --------------------------- */

static int gem_create_onchip(rdna3_drm_file *f, union drm_amdgpu_gem_create *a);

/* amdgpu_gem_create_ioctl for AMDGPU_GEM_DOMAIN_DOORBELL: pages of the
 * doorbell BAR past the kernel's doorbells (the TTM doorbell manager), for
 * the user queues' doorbells. Without user queues there are none. */
static int gem_create_doorbell(rdna3_drm_file *f, union drm_amdgpu_gem_create *a)
{
    rdna3_drm_device *dev = f->dev;
    uint64_t pages = (a->in.bo_size + 4095) / 4096, total = dev->doorbell_size / 4096, p, i;
    rdna3_bo *bo;
    uint32_t handle = 0;
    int r;
    if (!dev->doorbell_size)
        return -RD_EOPNOTSUPP;
    if (total > RD_MAX_DOORBELL_PAGES)
        total = RD_MAX_DOORBELL_PAGES;
    for (p = RD_DOORBELL_KERNEL_SIZE / 4096; p + pages <= total; p++) {
        for (i = 0; i < pages && !(dev->doorbell_used[(p + i) / 8] & (1u << ((p + i) % 8))); i++)
            ;
        if (i == pages)
            break;
        p += i;   /* past the busy page */
    }
    if (p + pages > total)
        return -RD_ENOMEM;
    bo = (rdna3_bo *)dev->ops.zalloc(dev->ops.ctx, sizeof(*bo));
    if (!bo)
        return -RD_ENOMEM;
    doorbell_pages_set(dev, p, pages, 1);
    bo->refcnt = 1;
    bo->size = pages * 4096;
    bo->alignment = 4096;
    bo->domains = AMDGPU_GEM_DOMAIN_DOORBELL;
    bo->placement = AMDGPU_GEM_DOMAIN_DOORBELL;
    bo->flags = a->in.domain_flags;
    bo->vram_offset = p * 4096;   /* byte offset in the doorbell BAR */
    bo->mmap_offset = dev->next_mmap_offset;
    dev->next_mmap_offset += bo->size + 4096;
    r = handle_alloc(f, bo, &handle);
    if (r) {
        bo_put(f, bo);
        return r;
    }
    zero(a, sizeof(*a));
    a->out.handle = handle;
    return 0;
}

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
        return gem_create_doorbell(f, a);
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
    /* amdgpu_gem_create_ioctl: every VRAM allocation is handed out cleared */
    if (dom & AMDGPU_GEM_DOMAIN_VRAM)
        bo->flags |= AMDGPU_GEM_CREATE_VRAM_CLEARED;
    /* prefer VRAM if asked; the OS layer may fall back to GTT */
    bo->placement = (dom & AMDGPU_GEM_DOMAIN_VRAM) ? AMDGPU_GEM_DOMAIN_VRAM
                                                   : AMDGPU_GEM_DOMAIN_GTT;
    r = dev->ops.bo_alloc(dev->ops.ctx, bo);
    /* amdgpu_gem_create_ioctl's retries: without CPU_ACCESS_REQUIRED (TTM
     * moves the BO into the visible window on a CPU fault), then with GTT
     * added to a VRAM-only request. Nothing migrates a BO here, so the first
     * would leave it where the CPU cannot reach it: straight to GTT, which
     * the CPU reaches (card run 51: RADV's descriptor pool, VRAM with CPU
     * access, found the visible VRAM full) */
    if (r && bo->placement == AMDGPU_GEM_DOMAIN_VRAM &&
        dev->gtt_usage + bo->size <= dev->gtt_size) {   /* no unbounded wired memory */
        bo->placement = AMDGPU_GEM_DOMAIN_GTT;
        bo->vram_offset = 0;
        r = dev->ops.bo_alloc(dev->ops.ctx, bo);
        if (!r && !(dom & AMDGPU_GEM_DOMAIN_GTT))
            bo->domains = dom | AMDGPU_GEM_DOMAIN_GTT;
    }
    if (r) {
        dev->ops.free(dev->ops.ctx, bo, sizeof(*bo));
        return r;
    }
    bo->mmap_offset = dev->next_mmap_offset;
    dev->next_mmap_offset += bo->size + 4096;
    if (bo->placement == AMDGPU_GEM_DOMAIN_VRAM) {
        dev->vram_usage += bo->size;
        dev->vis_vram_usage += bo_vis_bytes(dev, bo);
    } else {
        dev->gtt_usage += bo->size;
    }

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
    bo->userptr_ro = (a->flags & AMDGPU_GEM_USERPTR_READONLY) ? 1 : 0;
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

/* the accelerator VRAM a file pins through surface BOs (they cannot be
 * exported, so only the importing file has handles on them) */
static uint64_t file_foreign_bytes(const rdna3_drm_file *f)
{
    uint64_t n = 0;
    uint32_t i;
    for (i = 0; i < RD_MAX_HANDLES; i++)
        if (f->bos[i] && f->bos[i]->foreign)
            n += f->bos[i]->size;
    return n;
}

/* RD_NR_XNU_SURFACE_BO: a handle on an IOSurface's VRAM, or its dirty mark */
static int surface_bo(rdna3_drm_file *f, struct rd_xnu_surface_bo *s)
{
    rdna3_drm_device *dev = f->dev;
    rdna3_bo *bo;
    uint32_t handle = 0;
    int r;

    if (s->plane > RD_XNU_SURFACE_MAX_PLANE)   /* IOSurface planes; never handed on to the OS unchecked */
        return -RD_EINVAL;
    if (s->op == RD_XNU_SURFACE_BO_DIRTY) {
        if (!dev->ops.surface_dirty)
            return -RD_ENOSYS;
        return dev->ops.surface_dirty(dev->ops.ctx, f->os, s->surface_id, s->plane);
    }
    if (s->op != RD_XNU_SURFACE_BO_IMPORT || !s->surface_id)
        return -RD_EINVAL;
    if (!dev->ops.surface_bo)
        return -RD_ENOSYS;
    bo = (rdna3_bo *)dev->ops.zalloc(dev->ops.ctx, sizeof(*bo));
    if (!bo)
        return -RD_ENOMEM;
    bo->refcnt = 1;
    r = dev->ops.surface_bo(dev->ops.ctx, f->os, bo, s->surface_id, s->plane);
    if (r) {
        dev->ops.free(dev->ops.ctx, bo, sizeof(*bo));
        return r;
    }
    /* a VRAM BO RADV may map and place in its VA like any other; WindowServer
     * and the display read the same bytes */
    bo->foreign = 1;
    bo->alignment = 4096;
    bo->domains = AMDGPU_GEM_DOMAIN_VRAM;
    bo->placement = AMDGPU_GEM_DOMAIN_VRAM;
    bo->flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
    if (!bo->size || (bo->size & 4095) || (bo->vram_offset & 4095) ||
        bo->vram_offset + bo->size > dev->vram_size || bo->vram_offset + bo->size < bo->vram_offset) {
        bo_put(f, bo);   /* the OS's hold goes back through bo_free */
        return -RD_EINVAL;
    }
    if (s->size > bo->size) {   /* the GPU would address past the surface */
        bo_put(f, bo);
        return -RD_ENOSPC;
    }
    /* a BO keeps the surface's VRAM after the surface is gone: one client
     * may pin at most half of the CPU-visible VRAM the accelerator draws
     * from (ECC security review: a loop of imports starved WindowServer) */
    if (file_foreign_bytes(f) + bo->size > RD_SURFACE_BO_FILE_MAX(dev)) {
        bo_put(f, bo);
        return -RD_ENOSPC;
    }
    bo->mmap_offset = dev->next_mmap_offset;
    dev->next_mmap_offset += bo->size + 4096;
    r = handle_alloc(f, bo, &handle);
    if (r) {
        bo_put(f, bo);
        return r;
    }
    s->handle = handle;
    s->size = bo->size;
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

/* one mapping into the GPU page tables (amdgpu_vm_bo_update); a doorbell
 * BO is only recorded: the GPU never reaches doorbells through a VM, the
 * client writes them through its CPU mapping */
static int va_map_hw(rdna3_drm_file *f, rdna3_bo *bo, uint64_t va, uint64_t offset, uint64_t size, uint64_t flags)
{
    rdna3_drm_ops *ops = &f->dev->ops;
    uint32_t vflags = (uint32_t)flags;
    if (!f->vm || !ops->vm_map || bo->placement == AMDGPU_GEM_DOMAIN_DOORBELL)
        return 0;
    if (bo->userptr_ro)   /* a read-only userptr is never writable */
        vflags &= ~(uint32_t)AMDGPU_VM_PAGE_WRITEABLE;
    return ops->vm_map(ops->ctx, f->vm, bo, va, offset, size, vflags);
}

static int gem_va(rdna3_drm_file *f, struct drm_amdgpu_gem_va *va)
{
    uint32_t i;
    /* mappings are recorded always and written into the GPU page tables
     * when the OS layer provides vm_map (hw/gpuvm.c behind it) */
    if (va->va_address < VA_RESERVED_BOTTOM || (va->va_address & 4095) || (va->map_size & 4095) ||
        (va->offset_in_bo & 4095) || va->va_address + va->map_size < va->va_address)
        return -RD_EINVAL;
    /* amdgpu_gem_va_ioctl: nothing in the GMC hole; the high half is the
     * low one sign-extended; nothing past max_pfn less the reserved top */
    if (va->va_address >= GMC_HOLE_START && va->va_address < GMC_HOLE_END)
        return -RD_EINVAL;
    va->va_address &= GMC_HOLE_MASK;
    if (va->va_address + va->map_size > VM_SIZE)
        return -RD_EINVAL;
    switch (va->operation) {
    case AMDGPU_VA_OP_MAP:
    case AMDGPU_VA_OP_REPLACE: {
        rdna3_bo *bo = rdna3_drm_bo_lookup(f, va->handle);
        if (!bo)
            return -RD_ENOENT;
        /* amdgpu_vm_verify_parameters / amdgpu_vm_bo_map: no u64 wrap of
         * offset + size past the BO */
        if (!va->map_size || va->offset_in_bo > bo->size || va->map_size > bo->size - va->offset_in_bo ||
            bo_onchip(bo))
            return -RD_EINVAL;
        /* REPLACE clears what it overlaps (amdgpu_vm_bo_clear_mappings);
         * MAP refuses it (amdgpu_vm_bo_map: "conflict with") */
        for (i = 0; i < f->nvas; i++) {
            rdna3_va *m = &f->vas[i];
            if (!m->handle || !(m->va < va->va_address + va->map_size && va->va_address < m->va + m->size))
                continue;
            if (va->operation != AMDGPU_VA_OP_REPLACE)
                return -RD_EINVAL;
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
        {
            int r = va_map_hw(f, bo, va->va_address, va->offset_in_bo, va->map_size, va->flags);
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

/* amdgpu_gem_wait_idle_ioctl: the BO's reservation fences, until the
 * absolute timeout. No BO here carries its own fences, so it waits for
 * what every kernel ring and every user queue on the device last
 * submitted: never "idle" while a job may still use it, at the cost of
 * waiting on other work too. Status 1: still busy at the timeout. */
static int gem_wait_idle(rdna3_drm_file *f, union drm_amdgpu_gem_wait_idle *w)
{
    rdna3_drm_device *dev = f->dev;
    rdna3_bo *bo = rdna3_drm_bo_lookup(f, w->in.handle);
    uint64_t timeout = w->in.timeout;
    rdna3_fence *fe;
    rdna3_drm_file *g;
    uint32_t domain, n = 0, cap = AMDGPU_HW_IP_NUM * 4, ip, ring, i;
    int busy = 0;
    if (!bo)
        return -RD_ENOENT;
    domain = bo->placement;   /* the waits drop the OS lock: the BO may go */
    for (g = dev->files; g; g = g->next_file)
        if (g->userqs)
            cap += RD_MAX_USERQ;
    fe = (rdna3_fence *)dev->ops.zalloc(dev->ops.ctx, sizeof(*fe) * cap);
    if (!fe)
        return -RD_ENOMEM;
    for (ip = 0; ip < AMDGPU_HW_IP_NUM; ip++)
        for (ring = 0; ring < 4; ring++) {
            rdna3_fence k;
            k.ip = ip;
            k.ring = ring;
            k.seq = dev->last_seq[ip][ring];
            if (k.seq && !fence_done(dev, &k))
                fe[n++] = k;
        }
    for (g = dev->files; g; g = g->next_file)
        for (i = 0; g->userqs && i < RD_MAX_USERQ && n < cap; i++)
            if (g->userqs[i].used && g->userqs[i].last_fence.ip && !fence_done(dev, &g->userqs[i].last_fence))
                fe[n++] = g->userqs[i].last_fence;
    /* a job that ended in an error (a recovered hang) no longer uses the
     * BO: like a signaled fence with an error, it is not "busy" */
    for (i = 0; i < n && !busy; i++)
        busy = fence_wait_abs(dev, &fe[i], timeout) == -RD_ETIME;
    dev->ops.free(dev->ops.ctx, fe, sizeof(*fe) * cap);
    zero(&w->out, sizeof(w->out));
    w->out.status = busy ? 1 : 0;
    w->out.domain = domain;
    return 0;
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

/* amdgpu_ctx_priority_is_valid: the five levels; UNSET is not one */
static int ctx_prio_valid(int32_t p)
{
    return p == AMDGPU_CTX_PRIORITY_VERY_LOW || p == AMDGPU_CTX_PRIORITY_LOW ||
           p == AMDGPU_CTX_PRIORITY_NORMAL || p == AMDGPU_CTX_PRIORITY_HIGH ||
           p == AMDGPU_CTX_PRIORITY_VERY_HIGH;
}

/* drm_is_current_master || capable(CAP_SYS_NICE), as the OS sees it */
static int caller_privileged(const rdna3_drm_device *dev)
{
    return dev->ops.privileged && dev->ops.privileged(dev->ops.ctx);
}

/* amdgpu_ctx_priority_permit: NORMAL and below for everyone */
static int ctx_prio_permit(const rdna3_drm_device *dev, int32_t p)
{
    if (p <= AMDGPU_CTX_PRIORITY_NORMAL || caller_privileged(dev))
        return 0;
    return -RD_EACCES;
}

int32_t rdna3_drm_ctx_priority(const rdna3_drm_file *f, uint32_t ctx_id)
{
    if (!ctx_id || ctx_id >= RD_MAX_CTX || !f->ctx_used[ctx_id])
        return AMDGPU_CTX_PRIORITY_UNSET;
    return f->ctx_override_prio[ctx_id] == AMDGPU_CTX_PRIORITY_UNSET ? f->ctx_init_prio[ctx_id]
                                                                     : f->ctx_override_prio[ctx_id];
}

/* a live context made before the last VRAM loss: ctx->generation !=
 * amdgpu_vm_generation (its high half is adev->vram_lost_counter) */
static int ctx_vram_lost(const rdna3_drm_file *f, uint32_t ctx_id)
{
    return ctx_id && ctx_id < RD_MAX_CTX && f->ctx_used[ctx_id] &&
           f->ctx_vram_lost[ctx_id] != f->dev->vram_lost_counter;
}

/* Ported from amdgpu_inc_vram_lost (amdgpu_device_gpu_recover ->
 * amdgpu_device_reinit_after_reset, once amdgpu_device_check_vram_lost
 * found the reset magic gone) */
void rdna3_drm_device_vram_lost(rdna3_drm_device *dev)
{
    dev->vram_lost_counter++;
}

static int ctx_ioctl(rdna3_drm_file *f, union drm_amdgpu_ctx *c)
{
    uint32_t id = c->in.ctx_id, i;
    int32_t prio = c->in.priority;
    int r;
    /* amdgpu_ctx_ioctl: garbage in the priority field means NORMAL, for
     * backwards compatibility */
    if (!ctx_prio_valid(prio))
        prio = AMDGPU_CTX_PRIORITY_NORMAL;
    switch (c->in.op) {
    case AMDGPU_CTX_OP_ALLOC_CTX:
        /* amdgpu_ctx_alloc -> amdgpu_ctx_init: above NORMAL needs privilege */
        if ((r = ctx_prio_permit(f->dev, prio)) != 0)
            return r;
        for (i = 1; i < RD_MAX_CTX; i++)
            if (!f->ctx_used[i]) {
                f->ctx_used[i] = 1;
                f->ctx_init_prio[i] = (int16_t)prio;
                f->ctx_override_prio[i] = AMDGPU_CTX_PRIORITY_UNSET;
                /* ctx->generation = amdgpu_vm_generation: the VRAM loss
                 * count the context was made under */
                f->ctx_vram_lost[i] = f->dev->vram_lost_counter;
                f->vcn_entity[i] = 0;
                f->vcn_entity_jobs[i] = 0;
                zero(c, sizeof(*c));
                c->out.alloc.ctx_id = i;
                return 0;
            }
        return -RD_ENOMEM;
    case AMDGPU_CTX_OP_FREE_CTX:
        if (!id || id >= RD_MAX_CTX || !f->ctx_used[id])
            return -RD_EINVAL;
        f->ctx_used[id] = 0;
        f->vcn_entity[id] = 0;
        f->vcn_entity_jobs[id] = 0;
        return 0;
    case AMDGPU_CTX_OP_QUERY_STATE:
    case AMDGPU_CTX_OP_QUERY_STATE2: {
        /* amdgpu_ctx_query / amdgpu_ctx_query2: the reset and guilty
         * state lives with the VM here (one VM per file, one VMID per VM);
         * VRAMLOST is the context's own: ctx->generation !=
         * amdgpu_vm_generation, i.e. VRAM was lost after the context was
         * made (rdna3_drm_device_vram_lost) */
        uint32_t fl = 0, op = c->in.op;
        if (!id || id >= RD_MAX_CTX || !f->ctx_used[id])
            return -RD_EINVAL;
        if (f->dev->ops.vm_query && f->vm)
            fl = f->dev->ops.vm_query(f->dev->ops.ctx, f->vm) & ~(uint32_t)AMDGPU_CTX_QUERY2_FLAGS_VRAMLOST;
        if (ctx_vram_lost(f, id))
            fl |= AMDGPU_CTX_QUERY2_FLAGS_VRAMLOST;
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

/* ------------------------------ sched ------------------------------ */

static rdna3_drm_file *file_by_id(rdna3_drm_device *dev, uint32_t id)
{
    rdna3_drm_file *t;
    for (t = dev->files; id && t; t = t->next_file)
        if (t->id == id)
            return t;
    return NULL;
}

/* amdgpu_sched_ioctl (DRM_MASTER in amdgpu_ioctls_kms): the op, then the
 * priority, then the target file (here by RD_NR_XNU_FILE_ID id: the fd
 * libdrm_xnu translated). PROCESS overrides every context of that file
 * (amdgpu_sched_process_priority_override), CONTEXT one of them
 * (amdgpu_sched_context_priority_override); both through
 * amdgpu_ctx_priority_override, which keeps init_priority. */
static int sched_ioctl(rdna3_drm_file *f, union drm_amdgpu_sched *a)
{
    uint32_t op = a->in.op, cid = a->in.ctx_id, i;
    int32_t prio = a->in.priority;
    rdna3_drm_file *t;
    if (!caller_privileged(f->dev))   /* drm_ioctl_permit: not the DRM master */
        return -RD_EACCES;
    if (op != AMDGPU_SCHED_OP_PROCESS_PRIORITY_OVERRIDE && op != AMDGPU_SCHED_OP_CONTEXT_PRIORITY_OVERRIDE)
        return -RD_EINVAL;
    if (!ctx_prio_valid(prio))
        return -RD_EINVAL;
    t = file_by_id(f->dev, (uint32_t)a->in.fd);
    if (!t)                           /* fd_empty / amdgpu_file_to_fpriv */
        return -RD_EINVAL;
    if (op == AMDGPU_SCHED_OP_PROCESS_PRIORITY_OVERRIDE) {
        for (i = 1; i < RD_MAX_CTX; i++)
            if (t->ctx_used[i])
                t->ctx_override_prio[i] = (int16_t)prio;
        return 0;
    }
    if (!cid || cid >= RD_MAX_CTX || !t->ctx_used[cid])   /* amdgpu_ctx_get */
        return -RD_EINVAL;
    t->ctx_override_prio[cid] = (int16_t)prio;
    return 0;
}

/* amdgpu_proc_options_ioctl: the one option is KFD's SIGBUS delay for
 * poison / RAS events (amdgpu_amdkfd_set_sigbus_delay); there is no KFD
 * here, so it answers like amdgpu built without CONFIG_HSA_AMD */
static int proc_options_ioctl(struct drm_amdgpu_proc_options *o)
{
    if (o->op == AMDGPU_PROC_OPTIONS_OP_KFD_SIGBUS_DELAY)
        return -RD_EOPNOTSUPP;
    return -RD_EINVAL;
}

/* --------------------------- user queues --------------------------- */
/* amdgpu_userq.c, amdgpu_userq_fence.c, mes_userqueue.c and amdgpu_seq64.c
 * (Linux 6.16) on this driver's objects. What amdgpu does with TTM, dma_fence
 * and drm_exec is done in place here: everything is resident (no eviction
 * fences, no restore worker), kernel objects are CPU-visible VRAM BOs, a
 * fence is a value (see rdna3_userq in rdna3_drm.h), and implicit sync
 * through BO reservations does not exist in this driver (the BO lists of
 * USERQ_SIGNAL / USERQ_WAIT are looked up, nothing is attached to them). */

#define VM_MAX_PFN      (1ull << 36)            /* adev->vm_manager.max_pfn, 48-bit VM */
#define USERQ_PROC_CTX_SZ 4096u                 /* AMDGPU_USERQ_PROC_CTX_SZ */
#define USERQ_GANG_CTX_SZ 4096u                 /* AMDGPU_USERQ_GANG_CTX_SZ */
#define FENCE_STUB_IP   0xffffffffu             /* dma_fence_get_stub(): signaled */
#define BO_INVALID_OFFSET 0x7fffffffffffffffull /* AMDGPU_BO_INVALID_OFFSET (LONG_MAX) */

static rdna3_userq_fence_drv *fdrv_find(rdna3_drm_device *dev, uint32_t ctx)
{
    uint32_t i;
    if (!dev->fence_drv || !ctx)
        return NULL;
    for (i = 0; i < RD_MAX_FENCE_DRV; i++)
        if (dev->fence_drv[i].refcnt && dev->fence_drv[i].context == ctx)
            return &dev->fence_drv[i];
    return NULL;
}

/* the mapping of this file's VM that holds 'addr' (amdgpu_vm_bo_lookup_mapping) */
static rdna3_va *va_lookup(rdna3_drm_file *f, uint64_t addr)
{
    uint32_t i;
    addr &= GMC_HOLE_MASK;
    for (i = 0; i < f->nvas; i++) {
        rdna3_va *m = &f->vas[i];
        uint64_t start = m->va & GMC_HOLE_MASK;
        if (m->handle && m->bo && addr >= start && addr - start < m->size)
            return m;
    }
    return NULL;
}

/* ---- seq64 ---- */

/* Ported from amdgpu_seq64_init: the slots' BO, created with the first
 * fence driver (amdgpu: at device init, 2 MiB in GTT) */
static int seq64_init(rdna3_drm_device *dev)
{
    rdna3_drm_ops *ops = &dev->ops;
    rdna3_bo *bo;
    void *cpu;
    uint64_t mc = 0;
    int r;
    if (dev->seq64_bo)
        return 0;
    if (!ops->bo_kmap || !ops->bo_gpu_addr)
        return -RD_ENODEV;
    bo = (rdna3_bo *)ops->zalloc(ops->ctx, sizeof(*bo));
    if (!bo)
        return -RD_ENOMEM;
    bo->refcnt = 1;
    bo->size = RD_SEQ64_SIZE;
    bo->alignment = 4096;
    bo->domains = bo->placement = AMDGPU_GEM_DOMAIN_VRAM;
    bo->flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED | AMDGPU_GEM_CREATE_VRAM_CONTIGUOUS;
    r = ops->bo_alloc(ops->ctx, bo);
    if (r) {
        ops->free(ops->ctx, bo, sizeof(*bo));
        return r;
    }
    dev->vram_usage += bo->size;
    dev->vis_vram_usage += bo_vis_bytes(dev, bo);
    cpu = ops->bo_kmap(ops->ctx, bo);
    r = ops->bo_gpu_addr(ops->ctx, bo, &mc);
    if (!cpu || r) {
        bo_put_dev(dev, bo);
        return r ? r : -RD_ENOMEM;
    }
    zero(cpu, RD_SEQ64_SIZE);
    zero(dev->seq64_used, sizeof(dev->seq64_used));
    dev->seq64_bo = bo;
    dev->seq64_cpu = (volatile uint64_t *)cpu;
    dev->seq64_gpu_addr = mc;
    return 0;
}

/* Ported from amdgpu_seq64_fini */
static void seq64_fini(rdna3_drm_device *dev)
{
    if (dev->seq64_bo)
        bo_put_dev(dev, dev->seq64_bo);
    dev->seq64_bo = NULL;
    dev->seq64_cpu = NULL;
}

/* Ported from amdgpu_seq64_alloc */
static int seq64_alloc(rdna3_drm_device *dev, uint64_t *va, uint64_t *gpu_addr,
                       volatile uint64_t **cpu_addr)
{
    uint32_t bit;
    for (bit = 0; bit < RD_SEQ64_SLOTS; bit++)
        if (!(dev->seq64_used[bit / 8] & (1u << (bit % 8))))
            break;
    if (bit >= RD_SEQ64_SLOTS)
        return -RD_ENOSPC;
    dev->seq64_used[bit / 8] |= (uint8_t)(1u << (bit % 8));
    *va = bit * 8ull + RD_SEQ64_VA;
    *gpu_addr = bit * 8ull + dev->seq64_gpu_addr;
    *cpu_addr = dev->seq64_cpu + bit;
    return 0;
}

/* Ported from amdgpu_seq64_free */
static void seq64_free(rdna3_drm_device *dev, uint64_t va)
{
    uint64_t bit = (va - RD_SEQ64_VA) / 8;
    if (bit < RD_SEQ64_SLOTS)
        dev->seq64_used[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}

/* Ported from amdgpu_seq64_map: read-only, uncached, at the reserved VA */
static int seq64_map(rdna3_drm_file *f)
{
    rdna3_drm_ops *ops = &f->dev->ops;
    int r;
    if (f->seq64_mapped)
        return 0;
    if (!f->dev->seq64_bo)
        return -RD_EINVAL;
    if (f->vm && ops->vm_map) {
        r = ops->vm_map(ops->ctx, f->vm, f->dev->seq64_bo, RD_SEQ64_VA & GMC_HOLE_MASK, 0,
                        RD_SEQ64_SIZE, AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_MTYPE_UC);
        if (r)
            return r;
    }
    f->seq64_mapped = 1;
    return 0;
}

/* Ported from amdgpu_seq64_unmap */
static void seq64_unmap(rdna3_drm_file *f)
{
    rdna3_drm_ops *ops = &f->dev->ops;
    if (!f->seq64_mapped)
        return;
    if (f->vm && ops->vm_unmap)
        ops->vm_unmap(ops->ctx, f->vm, RD_SEQ64_VA & GMC_HOLE_MASK, RD_SEQ64_SIZE);
    f->seq64_mapped = 0;
}

/* The OS lost VRAM and with it every page table, and gave each live VM a
 * new, empty root: every client's mappings go into the tables again (what
 * amdgpu gets from evicting the page tables to GTT before the sleep and
 * amdgpu_vm_validate putting them back). The BO contents in VRAM stay lost;
 * clients see that as VRAMLOST. Returns how many mappings failed. */
int rdna3_drm_device_vm_replay(rdna3_drm_device *dev)
{
    rdna3_drm_ops *ops = &dev->ops;
    rdna3_drm_file *f;
    uint32_t i;
    int failed = 0;
    for (f = dev->files; f; f = f->next_file) {
        if (!f->vm)
            continue;
        for (i = 0; f->vas && i < f->nvas; i++) {
            rdna3_va *m = &f->vas[i];
            if (m->handle && m->bo && va_map_hw(f, m->bo, m->va, m->offset, m->size, m->flags))
                failed++;
        }
        if (f->seq64_mapped && dev->seq64_bo && ops->vm_map &&
            ops->vm_map(ops->ctx, f->vm, dev->seq64_bo, RD_SEQ64_VA & GMC_HOLE_MASK, 0, RD_SEQ64_SIZE,
                        AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_MTYPE_UC))
            failed++;
    }
    return failed;
}

static void vram_bo_visit(rdna3_drm_device *dev, rdna3_bo *bo, void (*fn)(void *, rdna3_bo *), void *ctx)
{
    if (!bo || bo->placement != AMDGPU_GEM_DOMAIN_VRAM || bo->visit == dev->visit_gen)
        return;
    bo->visit = dev->visit_gen;
    fn(ctx, bo);
}

void rdna3_drm_device_for_each_vram_bo(rdna3_drm_device *dev, void (*fn)(void *ctx, rdna3_bo *bo), void *ctx)
{
    rdna3_drm_file *f;
    uint32_t i;
    if (!++dev->visit_gen)
        ++dev->visit_gen;   /* 0 is what a new BO holds */
    for (f = dev->files; f; f = f->next_file)
        for (i = 0; f->bos && i < RD_MAX_HANDLES; i++)
            vram_bo_visit(dev, f->bos[i], fn, ctx);
    for (i = 0; i < RD_MAX_EXPORTS; i++)
        vram_bo_visit(dev, dev->exports[i].bo, fn, ctx);
    vram_bo_visit(dev, dev->seq64_bo, fn, ctx);
    /* the user queues' kernel objects: MQD, firmware context, wptr page */
    for (f = dev->files; f; f = f->next_file)
        for (i = 0; f->userqs && i < RD_MAX_USERQ; i++)
            if (f->userqs[i].used) {
                vram_bo_visit(dev, f->userqs[i].mqd.obj, fn, ctx);
                vram_bo_visit(dev, f->userqs[i].fw_obj.obj, fn, ctx);
                vram_bo_visit(dev, f->userqs[i].wptr_obj.obj, fn, ctx);
            }
}

/* ---- fence drivers ---- */

static int userq_tables(rdna3_drm_device *dev)
{
    rdna3_drm_ops *ops = &dev->ops;
    if (!dev->fence_drv)
        dev->fence_drv = (rdna3_userq_fence_drv *)ops->zalloc(ops->ctx,
                                                              sizeof(*dev->fence_drv) * RD_MAX_FENCE_DRV);
    if (!dev->uq_deferred)
        dev->uq_deferred = ops->zalloc(ops->ctx, sizeof(*dev->uq_deferred) * RD_USERQ_DEFERRED);
    return dev->fence_drv && dev->uq_deferred ? 0 : -RD_ENOMEM;
}

/* Ported from amdgpu_userq_fence_read */
static uint64_t userq_fence_read(const rdna3_userq_fence_drv *fd)
{
    return *fd->cpu_addr;
}

static void userq_fence_driver_put(rdna3_drm_device *dev, uint32_t ctx);

/* Ported from amdgpu_userq_fence_driver_get */
static void userq_fence_driver_get(rdna3_userq_fence_drv *fd)
{
    fd->refcnt++;
}

/* Ported from amdgpu_userq_fence_driver_process: the fences up to the rptr
 * have signaled; the fence drivers their queue waited on are let go
 * (amdgpu: from the EOP interrupt; here whenever a fence is looked at) */
static void userq_fence_driver_process(rdna3_drm_device *dev, rdna3_userq_fence_drv *fd)
{
    uint64_t rptr;
    uint32_t i, ctx;
    if (!fd || !dev->uq_deferred)
        return;
    rptr = userq_fence_read(fd);
    ctx = fd->context;
    for (i = 0; i < RD_USERQ_DEFERRED; i++)
        if (dev->uq_deferred[i].owner == ctx && dev->uq_deferred[i].seq <= rptr) {
            uint32_t ref = dev->uq_deferred[i].ref;
            zero(&dev->uq_deferred[i], sizeof(dev->uq_deferred[i]));
            userq_fence_driver_put(dev, ref);
        }
}

/* Ported from amdgpu_userq_fence_driver_destroy: the driver's pending
 * fences signal (-ECANCELED), what they held goes, the slot is free again */
static void userq_fence_driver_destroy(rdna3_drm_device *dev, rdna3_userq_fence_drv *fd)
{
    uint32_t ctx = fd->context, i;
    uint64_t va = fd->va;
    zero(fd, sizeof(*fd));
    seq64_free(dev, va);
    for (i = 0; dev->uq_deferred && i < RD_USERQ_DEFERRED; i++)
        if (dev->uq_deferred[i].owner == ctx) {
            uint32_t ref = dev->uq_deferred[i].ref;
            zero(&dev->uq_deferred[i], sizeof(dev->uq_deferred[i]));
            userq_fence_driver_put(dev, ref);
        }
}

/* Ported from amdgpu_userq_fence_driver_put */
static void userq_fence_driver_put(rdna3_drm_device *dev, uint32_t ctx)
{
    rdna3_userq_fence_drv *fd = fdrv_find(dev, ctx);
    if (fd && !--fd->refcnt)
        userq_fence_driver_destroy(dev, fd);
}

/* Ported from amdgpu_userq_fence_driver_alloc: a seq64 slot, a fence
 * context, the doorbell as its key (adev->userq_xa) */
static int userq_fence_driver_alloc(rdna3_drm_file *f, rdna3_userq *q)
{
    rdna3_drm_device *dev = f->dev;
    rdna3_userq_fence_drv *fd = NULL;
    uint32_t i, mine = 0;
    int r = userq_tables(dev);
    if (!r)
        r = seq64_init(dev);
    if (r)
        return r;
    for (i = 0; i < RD_MAX_FENCE_DRV; i++) {
        if (!dev->fence_drv[i].refcnt) {
            if (!fd)
                fd = &dev->fence_drv[i];
        } else if (dev->fence_drv[i].owner == f) {
            mine++;
        }
    }
    if (!fd || mine >= RD_FENCE_DRV_PER_FILE)
        return -RD_ENOMEM;
    r = seq64_alloc(dev, &fd->va, &fd->gpu_addr, &fd->cpu_addr);
    if (r)
        return r;
    *fd->cpu_addr = 0;
    fd->refcnt = 1;
    if (!++dev->next_fence_ctx)
        ++dev->next_fence_ctx;
    fd->context = dev->next_fence_ctx;
    fd->doorbell_index = q->doorbell_index;
    fd->last_seq = 0;
    fd->owner = f;
    q->fence_ctx = fd->context;
    return 0;
}

/* Ported from amdgpu_userq_walk_and_drop_fence_drv */
static void userq_walk_and_drop_fence_drv(rdna3_drm_device *dev, rdna3_userq *q)
{
    uint32_t i, n = q->nxa;
    q->nxa = 0;
    for (i = 0; i < n; i++)
        userq_fence_driver_put(dev, q->fence_drv_xa[i]);
}

/* Ported from amdgpu_userq_fence_driver_free */
static void userq_fence_driver_free(rdna3_drm_device *dev, rdna3_userq *q)
{
    userq_walk_and_drop_fence_drv(dev, q);
    /* Drop the fence_drv reference held by user queue */
    userq_fence_driver_put(dev, q->fence_ctx);
    q->fence_ctx = 0;
}

/* Ported from amdgpu_userq_fence_signaled: rptr >= wptr. A context that is
 * gone signaled when its driver died. */
static int userq_fence_done(rdna3_drm_device *dev, const rdna3_fence *fe)
{
    rdna3_userq_fence_drv *fd = fdrv_find(dev, fe->ring);
    if (!fd)
        return 1;
    if (userq_fence_read(fd) < fe->seq)
        return 0;
    userq_fence_driver_process(dev, fd);
    return 1;
}

struct userq_wait_arg {
    rdna3_drm_device *dev;
    rdna3_fence fe;
};

static int userq_wait_done(void *arg)
{
    struct userq_wait_arg *a = (struct userq_wait_arg *)arg;
    return userq_fence_done(a->dev, &a->fe);
}

/* dma_fence_wait on a user queue fence: the OS sleeps until the slot gets
 * there (EOP interrupt) or the deadline passes */
static int userq_fence_wait(rdna3_drm_device *dev, const rdna3_fence *fe, uint64_t abs_ns)
{
    struct userq_wait_arg a;
    if (userq_fence_done(dev, fe))
        return 0;
    if (!abs_ns || !dev->ops.userq_wait)
        return -RD_ETIME;
    a.dev = dev;
    a.fe = *fe;
    return dev->ops.userq_wait(dev->ops.ctx, userq_wait_done, &a, abs_ns);
}

/* Ported from amdgpu_userq_fence_create: a fence for 'seq' on the queue's
 * driver. The drivers the queue waited on (its fence_drv_xa) stay
 * referenced until this fence signals (fence_drv_array). */
static int userq_fence_create(rdna3_drm_device *dev, rdna3_userq *q, uint64_t seq, rdna3_fence *out)
{
    rdna3_userq_fence_drv *fd = fdrv_find(dev, q->fence_ctx);
    uint32_t i, j, kept = 0, mine = 0;
    if (!fd)
        return -RD_EINVAL;
    out->ip = RD_USERQ_FENCE_IP;
    out->ring = fd->context;
    out->seq = seq;
    if (seq > fd->last_seq)
        fd->last_seq = seq;
    if (!q->nxa)
        return 0;
    if (userq_fence_read(fd) >= seq) {   /* the hardware already processed the job */
        userq_walk_and_drop_fence_drv(dev, q);
        return 0;
    }
    for (j = 0; j < RD_USERQ_DEFERRED; j++)
        if (dev->uq_deferred[j].owner && dev->uq_deferred[j].file == fd->owner)
            mine++;
    for (i = 0, j = 0; i < q->nxa; i++) {
        while (j < RD_USERQ_DEFERRED && dev->uq_deferred[j].owner)
            j++;
        /* no room, or this client's share is used (amdgpu: no array): it
         * stays in the xa */
        if (j == RD_USERQ_DEFERRED || mine >= RD_USERQ_DEFERRED_PER_FILE) {
            q->fence_drv_xa[kept++] = q->fence_drv_xa[i];
            continue;
        }
        dev->uq_deferred[j].owner = fd->context;
        dev->uq_deferred[j].ref = q->fence_drv_xa[i];
        dev->uq_deferred[j].seq = seq;
        dev->uq_deferred[j].file = fd->owner;
        mine++;
    }
    q->nxa = kept;
    return 0;
}

/* ---- queue objects ---- */

/* Ported from amdgpu_userq_find (a queue being built or torn down is out of reach) */
static rdna3_userq *userq_find(rdna3_drm_file *f, uint32_t qid)
{
    if (!f->userqs || !qid || qid >= RD_MAX_USERQ || f->userqs[qid - 1].used != 1)
        return NULL;
    return &f->userqs[qid - 1];
}

/* Ported from amdgpu_userq_get_supported_ip_mask */
static uint32_t userq_get_supported_ip_mask(const rdna3_drm_device *dev)
{
    return dev->userq_ip_mask;
}

/* Ported from amdgpu_userq_create_object: a zeroed kernel BO the GPU reads
 * in VMID 0 (amdgpu: GTT bound to GART; here CPU-visible VRAM, where this
 * driver keeps the kernel queues' MQDs too) */
static int userq_create_object(rdna3_drm_device *dev, rdna3_userq_obj *o, uint32_t size)
{
    rdna3_drm_ops *ops = &dev->ops;
    rdna3_bo *bo = (rdna3_bo *)ops->zalloc(ops->ctx, sizeof(*bo));
    int r;
    if (!bo)
        return -RD_ENOMEM;
    bo->refcnt = 1;
    bo->size = ((uint64_t)size + 4095) & ~4095ull;
    bo->alignment = 4096;
    bo->domains = bo->placement = AMDGPU_GEM_DOMAIN_VRAM;
    bo->flags = AMDGPU_GEM_CREATE_VRAM_CONTIGUOUS | AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
    r = ops->bo_alloc(ops->ctx, bo);
    if (r) {
        ops->free(ops->ctx, bo, sizeof(*bo));
        return r;
    }
    dev->vram_usage += bo->size;
    dev->vis_vram_usage += bo_vis_bytes(dev, bo);
    o->obj = bo;
    o->cpu_ptr = ops->bo_kmap(ops->ctx, bo);
    r = ops->bo_gpu_addr(ops->ctx, bo, &o->gpu_addr);
    if (!o->cpu_ptr || r) {
        bo_put_dev(dev, bo);
        zero(o, sizeof(*o));
        return r ? r : -RD_ENOMEM;
    }
    zero(o->cpu_ptr, (size_t)bo->size);
    return 0;
}

/* Ported from amdgpu_userq_destroy_object */
static void userq_destroy_object(rdna3_drm_device *dev, rdna3_userq_obj *o)
{
    if (o->obj)
        bo_put_dev(dev, o->obj);
    zero(o, sizeof(*o));
}

/* Ported from amdgpu_doorbell_index_on_bar: dword index of doorbell
 * 'doorbell_index' (of 'db_size' bytes) of a doorbell BO */
static uint32_t doorbell_index_on_bar(const rdna3_bo *db_bo, uint32_t doorbell_index, uint32_t db_size)
{
    return (uint32_t)(db_bo->vram_offset / 4) + doorbell_index * ((db_size + 3) / 4);
}

/* Ported from amdgpu_userq_get_doorbell_index: the queue holds its
 * doorbell BO (amdgpu pins it in the doorbell domain, which only a doorbell
 * BO passes). Unlike amdgpu 6.16 the doorbell has to lie inside the BO, or
 * a client could ring another's queues. */
static int userq_get_doorbell_index(rdna3_drm_file *f, rdna3_userq *q, uint32_t handle,
                                    uint32_t offset, uint32_t *index)
{
    rdna3_bo *bo = rdna3_drm_bo_lookup(f, handle);
    uint32_t db_size;
    if (!bo || bo->placement != AMDGPU_GEM_DOMAIN_DOORBELL)
        return -RD_EINVAL;
    switch (q->queue_type) {
    case AMDGPU_HW_IP_GFX:
    case AMDGPU_HW_IP_COMPUTE:
    case AMDGPU_HW_IP_DMA:
        db_size = 8;
        break;
    default:                 /* VCN_ENC / VPE: not on this card's user queues */
        return -RD_EINVAL;
    }
    if ((uint64_t)offset * db_size + db_size > bo->size)
        return -RD_EINVAL;
    bo->refcnt++;
    q->db_obj = bo;
    *index = doorbell_index_on_bar(bo, offset, db_size);
    return 0;
}

/* Ported from amdgpu_userq_priority_permit: HIGH needs CAP_SYS_NICE or the
 * DRM master (the OS's stand-in, ops.privileged) */
static int userq_priority_permit(rdna3_drm_device *dev, int32_t priority)
{
    if (priority < AMDGPU_USERQ_CREATE_FLAGS_QUEUE_PRIORITY_HIGH)
        return 0;
    if (caller_privileged(dev))
        return 0;
    return -RD_EACCES;
}

/* Ported from amdgpu_userq_input_va_validate (Linux 6.18): [addr, addr +
 * expected_size) lies in one mapping of the VM, counted in GPU pages */
static int userq_input_va_validate(rdna3_drm_file *f, uint64_t addr, uint64_t expected_size)
{
    uint64_t user_addr = (addr & GMC_HOLE_MASK) >> 12, size = expected_size >> 12, start, last;
    rdna3_va *m = va_lookup(f, addr);
    if (!m)
        return -RD_EINVAL;
    start = (m->va & GMC_HOLE_MASK) >> 12;
    last = start + m->size / 4096 - 1;
    if (user_addr >= start && last - user_addr + 1 >= size)
        return 0;
    return -RD_EINVAL;
}

/* ---- mes_userqueue.c ---- */

/* Ported from convert_to_mes_priority */
static uint32_t convert_to_mes_priority(int32_t priority)
{
    switch (priority) {
    case AMDGPU_USERQ_CREATE_FLAGS_QUEUE_PRIORITY_NORMAL_LOW:
    default:
        return AMD_MES_PRIORITY_LEVEL_NORMAL;
    case AMDGPU_USERQ_CREATE_FLAGS_QUEUE_PRIORITY_LOW:
        return AMD_MES_PRIORITY_LEVEL_LOW;
    case AMDGPU_USERQ_CREATE_FLAGS_QUEUE_PRIORITY_NORMAL_HIGH:
        return AMD_MES_PRIORITY_LEVEL_MEDIUM;
    case AMDGPU_USERQ_CREATE_FLAGS_QUEUE_PRIORITY_HIGH:
        return AMD_MES_PRIORITY_LEVEL_HIGH;
    }
}

/* Ported from mes_userq_map: the MES takes the queue (ADD_QUEUE) */
static int mes_userq_map(rdna3_drm_file *f, rdna3_userq *q)
{
    rdna3_drm_ops *ops = &f->dev->ops;
    amd_mes_add_queue in;
    zero(&in, sizeof(in));
    in.process_va_start = 0;
    in.process_va_end = VM_MAX_PFN - 1;
    /* set process quantum to 10 ms and gang quantum to 1 ms as default */
    in.process_quantum = 100000;
    in.gang_quantum = 10000;
    in.paging = 0;
    in.process_context_addr = q->fw_obj.gpu_addr;
    in.gang_context_addr = q->fw_obj.gpu_addr + USERQ_PROC_CTX_SZ;
    in.inprocess_gang_priority = AMD_MES_PRIORITY_LEVEL_NORMAL;
    in.gang_global_priority_level = convert_to_mes_priority(q->priority);
    in.queue_type = q->queue_type;
    in.mqd_addr = q->mqd.gpu_addr;
    in.wptr_addr = q->prop.wptr_gpu_addr;
    in.queue_size = q->prop.queue_size >> 2;
    in.doorbell_offset = q->prop.doorbell_index;
    in.wptr_mc_addr = q->wptr_obj.gpu_addr;
    /* process_id (vm->pasid) and page_table_base_addr (amdgpu_gmc_pd_addr)
     * belong to the VM: the OS fills them */
    return ops->userq_add_hw_queue(ops->ctx, f->vm, &in);
}

/* Ported from mes_userq_unmap: the MES lets go of it (REMOVE_QUEUE) */
static int mes_userq_unmap(rdna3_drm_file *f, rdna3_userq *q)
{
    rdna3_drm_ops *ops = &f->dev->ops;
    amd_mes_remove_queue in;
    zero(&in, sizeof(in));
    in.doorbell_offset = q->doorbell_index;
    in.gang_context_addr = q->fw_obj.gpu_addr + USERQ_PROC_CTX_SZ;
    return ops->userq_remove_hw_queue(ops->ctx, &in);
}

/* Ported from mes_userq_create_ctx_space: a page each of process and gang
 * context for the firmware */
static int mes_userq_create_ctx_space(rdna3_drm_device *dev, rdna3_userq *q)
{
    return userq_create_object(dev, &q->fw_obj, USERQ_PROC_CTX_SZ + USERQ_GANG_CTX_SZ);
}

/* Ported from mes_userq_map_gtt_bo_to_gart (the reference amdgpu takes is
 * the queue's here, dropped with the MQD) */
static int mes_userq_map_gtt_bo_to_gart(rdna3_drm_device *dev, rdna3_bo *bo, uint64_t *mc)
{
    int r;
    if (bo->placement != AMDGPU_GEM_DOMAIN_VRAM && bo->placement != AMDGPU_GEM_DOMAIN_GTT)
        return -RD_EINVAL;
    /* GART binds the page writable: never a client's read-only pages (as
     * va_map_hw keeps them read-only; ECC security review) */
    if (bo->userptr_ro)
        return -RD_EPERM;
    r = dev->ops.bo_gpu_addr(dev->ops.ctx, bo, mc);
    if (r)
        return r;
    bo->refcnt++;
    return 0;
}

/* Ported from mes_userq_create_wptr_mapping: the firmware polls the wptr
 * through GART, so its BO (one page at most) gets bound there. As in
 * amdgpu, the MES sees the BO's first qword. */
static int mes_userq_create_wptr_mapping(rdna3_drm_file *f, rdna3_userq *q, uint64_t wptr)
{
    rdna3_va *m = va_lookup(f, wptr & GMC_HOLE_MASK);
    int r;
    if (!m)
        return -RD_EINVAL;
    if (m->bo->size > 4096)
        return -RD_EINVAL;   /* "Requested GART mapping for wptr bo larger than one page" */
    r = mes_userq_map_gtt_bo_to_gart(f->dev, m->bo, &q->wptr_obj.gpu_addr);
    if (r)
        return r;
    q->wptr_obj.obj = m->bo;
    return 0;
}

/* Ported from mes_userq_mqd_create */
static int mes_userq_mqd_create(rdna3_drm_file *f, const struct drm_amdgpu_userq_in *in, rdna3_userq *q)
{
    rdna3_drm_device *dev = f->dev;
    rdna3_drm_ops *ops = &dev->ops;
    amd_mqd_prop *p = &q->prop;
    rdna3_userq_fence_drv *fd = fdrv_find(dev, q->fence_ctx);
    uint32_t mqd_size = q->queue_type == AMDGPU_HW_IP_DMA ? AMD_V11_SDMA_MQD_SIZE
                      : q->queue_type == AMDGPU_HW_IP_GFX ? AMD_V11_GFX_MQD_SIZE
                                                          : AMD_V11_COMPUTE_MQD_SIZE;
    int r;

    if (!in->wptr_va || !in->rptr_va || !in->queue_va || in->queue_size == 0 || !fd)
        return -RD_EINVAL;   /* "Invalid MQD parameters for userqueue" */
    r = userq_create_object(dev, &q->mqd, mqd_size);
    if (r)
        return r;
    /* Initialize the MQD BO with user given values */
    zero(p, sizeof(*p));
    p->wptr_gpu_addr = in->wptr_va;
    p->rptr_gpu_addr = in->rptr_va;
    p->queue_size = (uint32_t)in->queue_size;
    p->hqd_base_gpu_addr = in->queue_va;
    p->mqd_gpu_addr = q->mqd.gpu_addr;
    p->use_doorbell = 1;
    p->doorbell_index = q->doorbell_index;
    p->fence_address = fd->gpu_addr;

    if (q->queue_type == AMDGPU_HW_IP_COMPUTE) {
        struct drm_amdgpu_userq_mqd_compute_gfx11 c;
        if (in->mqd_size != sizeof(c)) {
            r = -RD_EINVAL;   /* "Invalid compute IP MQD size" */
            goto free_mqd;
        }
        if (ops->copyin(ops->ctx, &c, in->mqd, sizeof(c))) {
            r = -RD_ENOMEM;   /* memdup_user failed */
            goto free_mqd;
        }
        /* mes_userq_mqd_create (6.18): the client's VAs must be mapped
         * (PR #2 review: a bad one could hang the MES and every queue) */
        if (userq_input_va_validate(f, c.eop_va, 4096)) {
            r = -RD_EINVAL;
            goto free_mqd;
        }
        p->eop_gpu_addr = c.eop_va;
        p->hqd_pipe_priority = AMD_GFX_PIPE_PRIO_NORMAL;
        p->hqd_queue_priority = AMD_GFX_QUEUE_PRIORITY_MINIMUM;
        p->hqd_active = 0;
        p->tmz_queue = (in->flags & AMDGPU_USERQ_CREATE_FLAGS_QUEUE_SECURE) ? 1 : 0;
    } else if (q->queue_type == AMDGPU_HW_IP_GFX) {
        struct drm_amdgpu_userq_mqd_gfx11 g;
        if (in->mqd_size != sizeof(g) || !in->mqd) {
            r = -RD_EINVAL;   /* "Invalid GFX MQD" */
            goto free_mqd;
        }
        if (ops->copyin(ops->ctx, &g, in->mqd, sizeof(g))) {
            r = -RD_ENOMEM;
            goto free_mqd;
        }
        if (userq_input_va_validate(f, g.shadow_va, AMD_GFX11_SHADOW_SIZE) ||
            userq_input_va_validate(f, g.csa_va, AMD_GFX11_FWWORKAREA_SIZE)) {
            r = -RD_EINVAL;
            goto free_mqd;
        }
        p->shadow_addr = g.shadow_va;
        p->csa_addr = g.csa_va;
        p->tmz_queue = (in->flags & AMDGPU_USERQ_CREATE_FLAGS_QUEUE_SECURE) ? 1 : 0;
    } else if (q->queue_type == AMDGPU_HW_IP_DMA) {
        struct drm_amdgpu_userq_mqd_sdma_gfx11 s;
        if (in->mqd_size != sizeof(s) || !in->mqd) {
            r = -RD_EINVAL;   /* "Invalid SDMA MQD" */
            goto free_mqd;
        }
        if (ops->copyin(ops->ctx, &s, in->mqd, sizeof(s))) {
            r = -RD_ENOMEM;
            goto free_mqd;
        }
        if (userq_input_va_validate(f, s.csa_va, AMD_GFX11_FWWORKAREA_SIZE)) {
            r = -RD_EINVAL;
            goto free_mqd;
        }
        p->csa_addr = s.csa_va;
    }

    r = ops->userq_mqd_init(ops->ctx, q->queue_type, q->mqd.cpu_ptr, p);
    if (r)
        goto free_mqd;
    /* Create BO for FW operations */
    r = mes_userq_create_ctx_space(dev, q);
    if (r)
        goto free_mqd;
    /* FW expects WPTR BOs to be mapped into GART */
    r = mes_userq_create_wptr_mapping(f, q, p->wptr_gpu_addr);
    if (r)
        goto free_ctx;
    return 0;

free_ctx:
    userq_destroy_object(dev, &q->fw_obj);
free_mqd:
    userq_destroy_object(dev, &q->mqd);
    return r;
}

/* Ported from mes_userq_mqd_destroy */
static void mes_userq_mqd_destroy(rdna3_drm_device *dev, rdna3_userq *q)
{
    userq_destroy_object(dev, &q->fw_obj);
    userq_destroy_object(dev, &q->mqd);
    if (q->wptr_obj.obj)
        bo_put_dev(dev, q->wptr_obj.obj);
    zero(&q->wptr_obj, sizeof(q->wptr_obj));
}

/* ---- amdgpu_userq.c ---- */

/* Ported from amdgpu_userq_unmap_helper */
static int userq_unmap_helper(rdna3_drm_file *f, rdna3_userq *q)
{
    int r = 0;
    if (q->state == RD_USERQ_STATE_MAPPED) {
        r = mes_userq_unmap(f, q);
        q->state = r ? RD_USERQ_STATE_HUNG : RD_USERQ_STATE_UNMAPPED;
    }
    return r;
}

/* Ported from amdgpu_userq_map_helper */
static int userq_map_helper(rdna3_drm_file *f, rdna3_userq *q)
{
    int r = 0;
    if (q->state == RD_USERQ_STATE_UNMAPPED) {
        r = mes_userq_map(f, q);
        q->state = r ? RD_USERQ_STATE_HUNG : RD_USERQ_STATE_MAPPED;
    }
    return r;
}

/* Ported from amdgpu_userq_wait_for_last_fence: up to 100 ms for the last
 * signaled work (the OS lock may be dropped meanwhile; the caller has taken
 * the queue out of reach) */
static void userq_wait_for_last_fence(rdna3_drm_device *dev, const rdna3_userq *q)
{
    rdna3_fence fe = q->last_fence;
    if (fe.ip != RD_USERQ_FENCE_IP || userq_fence_done(dev, &fe) || !dev->ops.now_ns)
        return;
    userq_fence_wait(dev, &fe, dev->ops.now_ns(dev->ops.ctx) + 100ull * 1000 * 1000);
}

/* Ported from amdgpu_userq_cleanup (it also lets go of the doorbell BO,
 * which amdgpu's file-close path never drops) */
static void userq_cleanup(rdna3_drm_file *f, rdna3_userq *q)
{
    mes_userq_mqd_destroy(f->dev, q);
    userq_fence_driver_free(f->dev, q);
    if (q->db_obj)
        bo_put_dev(f->dev, q->db_obj);
    zero(q, sizeof(*q));
}

/* Ported from amdgpu_userq_fence_driver_force_completion: the slot reads as
 * the last fence made on the queue, every waiter goes on */
static void userq_fence_force_completion(rdna3_drm_device *dev, rdna3_userq *q)
{
    rdna3_userq_fence_drv *fd = fdrv_find(dev, q->fence_ctx);
    if (!fd || !fd->cpu_addr)
        return;
    *fd->cpu_addr = fd->last_seq;
    userq_fence_driver_process(dev, fd);
}

/* Ported from amdgpu_userq_suspend (S3; S0ix would preempt instead):
 * every queue off the scheduler before the sleep. Returns the failures. */
int rdna3_drm_device_userq_suspend(rdna3_drm_device *dev)
{
    rdna3_drm_file *f;
    uint32_t i;
    int failed = 0;
    for (f = dev->files; f; f = f->next_file)
        for (i = 0; f->userqs && i < RD_MAX_USERQ; i++)
            if (f->userqs[i].used == 1 && userq_unmap_helper(f, &f->userqs[i]))
                failed++;
    return failed;
}

/* Ported from amdgpu_userq_active: the queues of one process mapped on the
 * scheduler now */
int rdna3_drm_file_userq_active(const rdna3_drm_file *f)
{
    uint32_t i;
    int n = 0;
    for (i = 0; f && f->userqs && i < RD_MAX_USERQ; i++)
        n += f->userqs[i].used == 1 && f->userqs[i].state == RD_USERQ_STATE_MAPPED;
    return n;
}

/* amdgpu_userq_active over every process */
int rdna3_drm_device_userq_active(const rdna3_drm_device *dev)
{
    const rdna3_drm_file *f;
    int n = 0;
    for (f = dev->files; f; f = f->next_file)
        n += rdna3_drm_file_userq_active(f);
    return n;
}

/* Ported from amdgpu_userq_resume: the queues back on the scheduler (their
 * MQDs and rings came back with the evicted VRAM) */
int rdna3_drm_device_userq_resume(rdna3_drm_device *dev)
{
    rdna3_drm_file *f;
    uint32_t i;
    int failed = 0;
    for (f = dev->files; f; f = f->next_file)
        for (i = 0; f->userqs && i < RD_MAX_USERQ; i++)
            if (f->userqs[i].used == 1 && userq_map_helper(f, &f->userqs[i]))
                failed++;
    return failed;
}

/* Ported from amdgpu_userq_pre_reset + amdgpu_userq_post_reset, after the
 * fact: the GPU reset restarted the MES scheduler, which forgot its queues
 * (no REMOVE_QUEUE reaches it any more). Every mapped queue is hung, its
 * fences complete; without a VRAM loss the queues go back on the
 * scheduler. Returns the failures. */
int rdna3_drm_device_userq_reset(rdna3_drm_device *dev, int vram_lost)
{
    rdna3_drm_file *f;
    uint32_t i;
    int failed = 0;
    for (f = dev->files; f; f = f->next_file)
        for (i = 0; f->userqs && i < RD_MAX_USERQ; i++) {
            rdna3_userq *q = &f->userqs[i];
            if (q->used != 1 || q->state != RD_USERQ_STATE_MAPPED)
                continue;
            q->state = RD_USERQ_STATE_HUNG;
            userq_fence_force_completion(dev, q);
            if (vram_lost)
                continue;
            if (mes_userq_map(f, q))
                failed++;
            else
                q->state = RD_USERQ_STATE_MAPPED;
        }
    return failed;
}

/* Ported from amdgpu_userq_mgr_init: the queue table, with the first queue */
static int userq_mgr_init(rdna3_drm_file *f)
{
    if (!f->userqs)
        f->userqs = (rdna3_userq *)f->dev->ops.zalloc(f->dev->ops.ctx, sizeof(rdna3_userq) * RD_MAX_USERQ);
    return f->userqs ? 0 : -RD_ENOMEM;
}

/* Ported from amdgpu_userq_mgr_fini: every queue of a closing file. The
 * 100 ms wait for the last fence is left out: it would drop the OS lock in
 * the middle of the file's teardown; REMOVE_QUEUE preempts the queue anyway. */
static void userq_mgr_fini(rdna3_drm_file *f)
{
    uint32_t i;
    if (!f->userqs)
        return;
    for (i = 0; i < RD_MAX_USERQ; i++) {
        rdna3_userq *q = &f->userqs[i];
        if (!q->used)
            continue;
        q->used = 2;   /* out of reach of userq_find */
        userq_wait_for_last_fence(f->dev, q);   /* amdgpu_userq_mgr_fini */
        userq_unmap_helper(f, q);
        userq_cleanup(f, q);
    }
    f->dev->ops.free(f->dev->ops.ctx, f->userqs, sizeof(rdna3_userq) * RD_MAX_USERQ);
    f->userqs = NULL;
}

/* Ported from amdgpu_userq_destroy */
static int userq_destroy(rdna3_drm_file *f, uint32_t queue_id)
{
    rdna3_userq *q = userq_find(f, queue_id);
    int r;
    if (!q)
        return -RD_EINVAL;   /* "Invalid queue id to destroy" */
    q->used = 2;             /* the wait may drop the OS lock: nobody else finds it */
    userq_wait_for_last_fence(f->dev, q);
    r = userq_unmap_helper(f, q);
    userq_cleanup(f, q);     /* drops the doorbell BO after the unmap, not before */
    return r;
}

/* Ported from amdgpu_userq_create */
static int userq_create(rdna3_drm_file *f, union drm_amdgpu_userq *args)
{
    rdna3_drm_device *dev = f->dev;
    rdna3_drm_ops *ops = &dev->ops;
    rdna3_userq *q = NULL;
    uint32_t qid, index = 0;
    int32_t priority = (int32_t)((args->in.flags & AMDGPU_USERQ_CREATE_FLAGS_QUEUE_PRIORITY_MASK) >>
                                 AMDGPU_USERQ_CREATE_FLAGS_QUEUE_PRIORITY_SHIFT);
    int r;

    r = userq_priority_permit(dev, priority);
    if (r)
        return r;
    /* uq_funcs = adev->userq_funcs[ip_type] */
    if (!(userq_get_supported_ip_mask(dev) & (1u << args->in.ip_type)) || !ops->userq_mqd_init ||
        !ops->userq_add_hw_queue || !ops->userq_remove_hw_queue || !ops->bo_kmap || !ops->bo_gpu_addr)
        return -RD_EINVAL;   /* "Usermode queue is not supported for this IP" */
    r = userq_mgr_init(f);
    if (r)
        return r;
    /* Validate the userq virtual address (Linux 6.18) */
    if (userq_input_va_validate(f, args->in.queue_va, args->in.queue_size) ||
        userq_input_va_validate(f, args->in.rptr_va, 4096) ||
        userq_input_va_validate(f, args->in.wptr_va, 4096))
        return -RD_EINVAL;
    /* idr_alloc(1, AMDGPU_MAX_USERQ_COUNT): taken before anything is built,
     * the slot stays out of reach until the queue is complete */
    for (qid = 1; qid < RD_MAX_USERQ && f->userqs[qid - 1].used; qid++)
        ;
    if (qid == RD_MAX_USERQ)
        return -RD_ENOMEM;   /* "Failed to allocate a queue id" */
    q = &f->userqs[qid - 1];
    zero(q, sizeof(*q));
    q->used = 2;
    q->doorbell_handle = args->in.doorbell_handle;
    q->queue_type = args->in.ip_type;
    q->priority = priority;
    q->state = RD_USERQ_STATE_UNMAPPED;

    /* Convert relative doorbell offset into absolute doorbell index */
    r = userq_get_doorbell_index(f, q, args->in.doorbell_handle, args->in.doorbell_offset, &index);
    if (r)
        goto fail;           /* "Failed to get doorbell for queue" */
    q->doorbell_index = index;
    r = userq_fence_driver_alloc(f, q);
    if (r)
        goto fail_db;        /* "Failed to alloc fence driver" */
    r = mes_userq_mqd_create(f, &args->in, q);
    if (r)
        goto fail_fence;     /* "Failed to create Queue" */
    /* the fence VAs the client waits on live in its VM (amdgpu maps seq64
     * into every VM at open) */
    r = seq64_map(f);
    if (r)
        goto fail_mqd;
    r = userq_map_helper(f, q);
    if (r)
        goto fail_mqd;       /* "Failed to map Queue" */
    q->used = 1;
    zero(&args->out, sizeof(args->out));
    args->out.queue_id = qid;
    return 0;

fail_mqd:
    mes_userq_mqd_destroy(dev, q);
fail_fence:
    userq_fence_driver_free(dev, q);
fail_db:
    bo_put_dev(dev, q->db_obj);
fail:
    zero(q, sizeof(*q));
    return r;
}

/* Ported from amdgpu_userq_ioctl, with amdgpu_userq_input_args_validate
 * (Linux 6.18) in front of it */
static int userq_ioctl(rdna3_drm_file *f, union drm_amdgpu_userq *args)
{
    const struct drm_amdgpu_userq_in *in = &args->in;
    switch (in->op) {
    case AMDGPU_USERQ_OP_CREATE:
        if (in->flags & ~(uint32_t)(AMDGPU_USERQ_CREATE_FLAGS_QUEUE_PRIORITY_MASK |
                                    AMDGPU_USERQ_CREATE_FLAGS_QUEUE_SECURE))
            return -RD_EINVAL;
        /* Usermode queues are only supported for GFX, compute and SDMA */
        if (in->ip_type != AMDGPU_HW_IP_GFX && in->ip_type != AMDGPU_HW_IP_DMA &&
            in->ip_type != AMDGPU_HW_IP_COMPUTE)
            return -RD_EINVAL;
        /* secure queues need TMZ, which this driver does not enable
         * (amdgpu_is_tmz), except on GFX / compute */
        if ((in->flags & AMDGPU_USERQ_CREATE_FLAGS_QUEUE_SECURE) && in->ip_type != AMDGPU_HW_IP_GFX &&
            in->ip_type != AMDGPU_HW_IP_COMPUTE)
            return -RD_EINVAL;
        if (in->queue_va == BO_INVALID_OFFSET || in->queue_va == 0 || in->queue_size == 0)
            return -RD_EINVAL;
        if (!in->wptr_va || !in->rptr_va)
            return -RD_EINVAL;
        return userq_create(f, args);
    case AMDGPU_USERQ_OP_FREE:
        if (in->ip_type || in->doorbell_handle || in->doorbell_offset || in->flags || in->queue_va ||
            in->queue_size || in->rptr_va || in->wptr_va || in->mqd || in->mqd_size)
            return -RD_EINVAL;
        return userq_destroy(f, in->queue_id);
    default:
        return -RD_EINVAL;
    }
}

/* ---- amdgpu_userq_fence.c: the ioctls ---- */

/* Ported from amdgpu_userq_fence_read_wptr: the wptr the client wrote,
 * read like amdgpu does from the first qword of the BO the wptr VA
 * belongs to */
static int userq_fence_read_wptr(rdna3_drm_file *f, rdna3_userq *q, uint64_t *wptr)
{
    rdna3_drm_ops *ops = &f->dev->ops;
    rdna3_va *m = va_lookup(f, q->prop.wptr_gpu_addr);
    const uint8_t *p;
    if (!m)
        return -RD_EINVAL;   /* "Failed to lookup amdgpu_bo_va_mapping" */
    p = (const uint8_t *)ops->bo_kmap(ops->ctx, m->bo);
    if (!p)
        return -RD_EINVAL;   /* "Failed mapping the userqueue wptr bo" */
    copy(wptr, p, 8);
    return 0;
}

/* the BO handles of a signal / wait: every one has to exist */
static int userq_bo_handles(rdna3_drm_file *f, uint64_t ptr, uint32_t n)
{
    rdna3_drm_ops *ops = &f->dev->ops;
    uint32_t i, h;
    for (i = 0; i < n; i++) {
        if (ops->copyin(ops->ctx, &h, ptr + 4ull * i, 4))
            return -RD_EFAULT;
        if (!rdna3_drm_bo_lookup(f, h))
            return -RD_ENOENT;
    }
    return 0;
}

/* Ported from amdgpu_userq_signal_ioctl: a fence for the queue's current
 * wptr goes into the syncobjs (drm_syncobj_replace_fence) */
static int userq_signal_ioctl(rdna3_drm_file *f, struct drm_amdgpu_userq_signal *a)
{
    rdna3_drm_device *dev = f->dev;
    rdna3_drm_ops *ops = &dev->ops;
    uint32_t i, h, n = a->num_syncobj_handles;
    rdna3_userq *q;
    rdna3_fence fe;
    uint64_t wptr = 0;
    int r;

    for (i = 0; i < n; i++) {
        if (ops->copyin(ops->ctx, &h, a->syncobj_handles + 4ull * i, 4))
            return -RD_EFAULT;
        if (!sobj(f, h))
            return -RD_ENOENT;
    }
    r = userq_bo_handles(f, a->bo_read_handles, a->num_bo_read_handles);
    if (!r)
        r = userq_bo_handles(f, a->bo_write_handles, a->num_bo_write_handles);
    if (r)
        return r;
    /* Retrieve the user queue */
    q = userq_find(f, a->queue_id);
    if (!q)
        return -RD_ENOENT;
    r = userq_fence_read_wptr(f, q, &wptr);
    if (r)
        return r;
    /* Create a new fence */
    r = userq_fence_create(dev, q, wptr, &fe);
    if (r)
        return r;
    q->last_fence = fe;
    /* Add the created fence to syncobj/BO's */
    for (i = 0; i < n; i++) {
        rdna3_syncobj *s;
        if (ops->copyin(ops->ctx, &h, a->syncobj_handles + 4ull * i, 4))
            return -RD_EFAULT;
        s = sobj(f, h);
        if (!s)
            continue;
        s->signaled = 0;
        s->has_fence = 1;
        s->fence = fe;
    }
    return 0;
}

/* drm_syncobj_find_fence(..., DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT): the
 * syncobj's fence for 'point', a signaled stub once it passed; -ENOENT
 * without the syncobj. Nothing submitted for it yet is -ETIME at once:
 * Linux waits for a submission, which nothing can make while this ioctl
 * runs. */
static int userq_find_fence(rdna3_drm_file *f, uint32_t h, uint64_t point, rdna3_fence *fe)
{
    rdna3_syncobj *s = sobj(f, h);
    if (!s)
        return -RD_ENOENT;
    if (point && s->point < point)
        return -RD_ETIME;
    if (sobj_signaled(f->dev, s)) {
        fe->ip = FENCE_STUB_IP;
        fe->ring = 0;
        fe->seq = 0;
        return 0;
    }
    if (!s->has_fence)
        return -RD_ETIME;
    *fe = s->fence;
    return 0;
}

/* the syncobj fences of a wait in amdgpu's order (timeline points, then
 * syncobjs): counted when 'out' is NULL, else stored (at most 'max') */
static int userq_wait_fences(rdna3_drm_file *f, const struct drm_amdgpu_userq_wait *w, rdna3_fence *out,
                             uint32_t max, uint32_t *count)
{
    rdna3_drm_ops *ops = &f->dev->ops;
    uint32_t i, h, pt, n = 0;
    rdna3_fence fe;
    int r;
    for (i = 0; i < w->num_syncobj_timeline_handles; i++) {
        /* amdgpu reads the points as 32-bit values */
        if (ops->copyin(ops->ctx, &h, w->syncobj_timeline_handles + 4ull * i, 4) ||
            ops->copyin(ops->ctx, &pt, w->syncobj_timeline_points + 4ull * i, 4))
            return -RD_EFAULT;
        r = userq_find_fence(f, h, pt, &fe);
        if (r)
            return r;
        if (out && n >= max)
            return -RD_EINVAL;
        if (out)
            out[n] = fe;
        n++;
    }
    for (i = 0; i < w->num_syncobj_handles; i++) {
        if (ops->copyin(ops->ctx, &h, w->syncobj_handles + 4ull * i, 4))
            return -RD_EFAULT;
        r = userq_find_fence(f, h, 0, &fe);
        if (r)
            return r;
        if (out && n >= max)
            return -RD_EINVAL;
        if (out)
            out[n] = fe;
        n++;
    }
    *count = n;
    return 0;
}

/* dma_fence_dedup_array: one fence per context, the latest */
static uint32_t fence_dedup(rdna3_fence *fe, uint32_t n)
{
    uint32_t i, j, k = 0;
    for (i = 0; i < n; i++) {
        for (j = 0; j < k; j++)
            if (fe[j].ip == fe[i].ip && fe[j].ring == fe[i].ring) {
                if (fe[i].seq > fe[j].seq)
                    fe[j].seq = fe[i].seq;
                break;
            }
        if (j == k)
            fe[k++] = fe[i];
    }
    return k;
}

/* Ported from amdgpu_userq_wait_ioctl: num_fences 0 counts the fences
 * behind the syncobjs; otherwise kernel ring fences are waited for here and
 * each user queue fence comes back as (va, value) for the waiting queue to
 * wait on, its fence driver referenced by that queue */
static int userq_wait_ioctl(rdna3_drm_file *f, struct drm_amdgpu_userq_wait *w)
{
    rdna3_drm_device *dev = f->dev;
    rdna3_drm_ops *ops = &dev->ops;
    struct drm_amdgpu_userq_fence_info info;
    rdna3_fence *fences;
    rdna3_userq *waitq;
    uint32_t i, n = 0, cnt = 0, alloc;
    int r;

    r = userq_bo_handles(f, w->bo_read_handles, w->num_bo_read_handles);
    if (!r)
        r = userq_bo_handles(f, w->bo_write_handles, w->num_bo_write_handles);
    if (!r)
        r = userq_wait_fences(f, w, NULL, 0, &n);
    if (r)
        return r;
    if (!w->num_fences) {
        /* the BOs carry no fences here: only the syncobjs count */
        w->num_fences = (uint16_t)n;
        return 0;
    }
    if (n > w->num_fences)
        return -RD_EINVAL;
    if (!n) {
        w->num_fences = 0;
        return userq_find(f, w->waitq_id) ? 0 : -RD_EINVAL;
    }
    alloc = n;
    fences = (rdna3_fence *)ops->zalloc(ops->ctx, sizeof(*fences) * alloc);
    if (!fences)
        return -RD_ENOMEM;
    r = userq_wait_fences(f, w, fences, n, &n);
    if (r)
        goto out;
    /* Keep only the latest fences to reduce the number of values given
     * back to userspace */
    n = fence_dedup(fences, n);
    if (!userq_find(f, w->waitq_id)) {
        r = -RD_EINVAL;
        goto out;
    }
    for (i = 0; i < n; i++) {
        rdna3_userq_fence_drv *fd;
        if (fences[i].ip == FENCE_STUB_IP)
            continue;
        if (fences[i].ip != RD_USERQ_FENCE_IP) {
            /* Just waiting on other driver fences should be good for now
             * (the OS lock may be dropped: the queue is looked up again) */
            r = fence_wait_abs(dev, &fences[i], ~0ull);
            if (r)
                goto out;
            continue;
        }
        fd = fdrv_find(dev, fences[i].ring);
        if (!fd || userq_fence_read(fd) >= fences[i].seq)
            continue;    /* signaled (or its driver is gone): nothing to wait on */
        waitq = userq_find(f, w->waitq_id);
        if (!waitq) {
            r = -RD_EINVAL;
            goto out;
        }
        /* the queue keeps the fence driver until its next fence signals */
        if (waitq->nxa == RD_USERQ_WAIT_REFS) {
            r = -RD_EBUSY;   /* xa_alloc: no free index */
            goto out;
        }
        userq_fence_driver_get(fd);
        waitq->fence_drv_xa[waitq->nxa++] = fd->context;
        /* Store drm syncobj's gpu va address and value */
        info.va = fd->va;
        info.value = fences[i].seq;
        if (ops->copyout(ops->ctx, w->out_fences + sizeof(info) * cnt, &info, sizeof(info))) {
            r = -RD_EFAULT;
            goto out;
        }
        cnt++;
    }
    w->num_fences = (uint16_t)cnt;
    r = 0;
out:
    ops->free(ops->ctx, fences, sizeof(*fences) * alloc);
    return r;
}

/* Ported from amdgpu_userq_metadata_info_gfx (amdgpu_kms.c) with the sizes
 * of gfx_v11_0_get_gfx_shadow_info_nocheck */
static void userq_metadata_info_gfx(struct drm_amdgpu_info_uq_metadata_gfx *meta)
{
    meta->shadow_size = AMD_GFX11_SHADOW_SIZE;
    meta->shadow_alignment = AMD_GFX11_SHADOW_ALIGNMENT;
    meta->csa_size = AMD_GFX11_FWWORKAREA_SIZE;
    meta->csa_alignment = AMD_GFX11_FWWORKAREA_ALIGNMENT;
}

/* ------------------------------ info ------------------------------ */

/* enum amdgpu_ring_type */
enum {
    RD_RING_TYPE_GFX = 0, RD_RING_TYPE_COMPUTE, RD_RING_TYPE_SDMA, RD_RING_TYPE_UVD, RD_RING_TYPE_VCE,
    RD_RING_TYPE_UVD_ENC = 6, RD_RING_TYPE_VCN_DEC, RD_RING_TYPE_VCN_ENC, RD_RING_TYPE_VCN_JPEG,
    RD_RING_TYPE_VPE = 10
};

/* amdgpu_hw_ip_to_ring_type */
static int amdgpu_hw_ip_to_ring_type(uint32_t hw_ip)
{
    switch (hw_ip) {
    case AMDGPU_HW_IP_GFX: return RD_RING_TYPE_GFX;
    case AMDGPU_HW_IP_COMPUTE: return RD_RING_TYPE_COMPUTE;
    case AMDGPU_HW_IP_DMA: return RD_RING_TYPE_SDMA;
    case AMDGPU_HW_IP_UVD: return RD_RING_TYPE_UVD;
    case AMDGPU_HW_IP_VCE: return RD_RING_TYPE_VCE;
    case AMDGPU_HW_IP_UVD_ENC: return RD_RING_TYPE_UVD_ENC;
    case AMDGPU_HW_IP_VCN_DEC: return RD_RING_TYPE_VCN_DEC;
    case AMDGPU_HW_IP_VCN_ENC: return RD_RING_TYPE_VCN_ENC;
    case AMDGPU_HW_IP_VCN_JPEG: return RD_RING_TYPE_VCN_JPEG;
    case AMDGPU_HW_IP_VPE: return RD_RING_TYPE_VPE;
    default: return -1;
    }
}

/* amdgpu_ring_max_ibs: the IBs one submission may carry on a ring of 'type' */
static uint32_t amdgpu_ring_max_ibs(int type)
{
    switch (type) {
    case RD_RING_TYPE_GFX:
        return 192;   /* "Need to keep at least 192 on GFX7+ for old radv." */
    case RD_RING_TYPE_COMPUTE:
        return 125;
    case RD_RING_TYPE_VCN_JPEG:
        return 16;
    default:
        return 49;
    }
}

static int info_out(rdna3_drm_file *f, struct drm_amdgpu_info *in, const void *p, size_t n)
{
    size_t len = MIN((size_t)in->return_size, n);
    if (!in->return_pointer || !len)
        return -RD_EINVAL;
    return f->dev->ops.copyout(f->dev->ops.ctx, in->return_pointer, p, len) ? -RD_EFAULT : 0;
}

/* soc21_allowed_read_registers (gc_11_0_0_offset.h; the SDMA0 / SDMA1 bases
 * are GC's on SOC21): GRBM_STATUS, GRBM_STATUS2, GRBM_STATUS_SE0..3,
 * SDMA0 / SDMA1_STATUS_REG, CP_STAT, CP_STALLED_STAT1..3, CP_CPF_BUSY_STAT,
 * CP_CPF_STALLED_STAT1, CP_CPF_STATUS, CP_CPC_BUSY_STAT,
 * CP_CPC_STALLED_STAT1, CP_CPC_STATUS, GB_ADDR_CONFIG; none GRBM indexed */
static const uint32_t soc21_allowed_read_registers[] = {
    0x0da4, 0x0da2, 0x0da5, 0x0da6, 0x0dae, 0x0daf, 0x0025, 0x0625, 0x0f40, 0x0f3d, 0x0f3e, 0x0f3c,
    0x0e28, 0x0e29, 0x0e27, 0x0e25, 0x0e26, 0x0e24, 0x13de,
};
#define SOC21_REG_GB_ADDR_CONFIG 0x13de

/* Ported from soc21_get_register_value (not GRBM indexed): GB_ADDR_CONFIG
 * from gfx.config when known */
static uint32_t soc21_get_register_value(rdna3_drm_device *dev, uint32_t reg_offset)
{
    if (reg_offset == dev->gc_base0 + SOC21_REG_GB_ADDR_CONFIG && dev->gb_addr_config)
        return dev->gb_addr_config;
    return dev->ops.read_reg ? dev->ops.read_reg(dev->ops.ctx, reg_offset) : 0;
}

/* Ported from soc21_read_register ('se_num' / 'sh_num' only matter for
 * GRBM-indexed entries, of which the list has none) */
static int soc21_read_register(rdna3_drm_device *dev, uint32_t se_num, uint32_t sh_num,
                               uint32_t reg_offset, uint32_t *value)
{
    uint32_t i;
    (void)se_num;
    (void)sh_num;
    *value = 0;
    if (!dev->gc_base0)   /* !adev->reg_offset[GC_HWIP][0] */
        return -RD_EINVAL;
    for (i = 0; i < sizeof(soc21_allowed_read_registers) / sizeof(soc21_allowed_read_registers[0]); i++) {
        if (reg_offset != dev->gc_base0 + soc21_allowed_read_registers[i])
            continue;
        *value = soc21_get_register_value(dev, reg_offset);
        return 0;
    }
    return -RD_EINVAL;
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
        if (vcn_up(dev, t)) {
            /* amdgpu_hw_ip_info: VCN_DEC has no ring on VCN 4 (the unified
             * ring is VCN_ENC's), one entity per IP (amdgpu_ctx_num_entities),
             * ip_discovery_version from UVD_HWIP for VCN and JPEG alike */
            uint32_t n = t == AMDGPU_HW_IP_VCN_ENC ? (dev->vcn_inst_ready ? 1 : 0)
                       : t == AMDGPU_HW_IP_VCN_JPEG ? dev->jpeg_ready : 0;
            ip.hw_ip_version_major = dev->ip_ver_major[t];
            ip.hw_ip_version_minor = dev->ip_ver_minor[t];
            ip.ip_discovery_version = dev->vcn_ip_version;
            ip.capabilities_flags = 0;
            ip.available_rings = (1u << n) - 1;
            ip.ib_start_alignment = 256;
            ip.ib_size_alignment = t == AMDGPU_HW_IP_VCN_ENC ? 4 : 64;
            return info_out(f, in, &ip, sizeof(ip));
        }
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
        if (vcn_up(dev, t))   /* amdgpu_info_ioctl: num_vcn_inst; JPEG: num_jpeg_inst * num_jpeg_rings */
            n = t == AMDGPU_HW_IP_VCN_JPEG ? 1 : dev->vcn_num_inst;
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
        /* amdgpu_firmware_info: MEC index 1 is MEC2 (none on GFX11, 0),
         * SDMA by instance (two on Navi 32, one firmware) */
        if ((t == AMDGPU_INFO_FW_GFX_MEC || t == AMDGPU_INFO_FW_SDMA) && in->query_fw.index > 1)
            return -RD_EINVAL;
        if (t == AMDGPU_INFO_FW_TA) {
            /* by TA_FW_TYPE_PSP_*: XGMI 2, RAS 3, HDCP 4, DTM 5, RAP 6,
             * SECUREDISPLAY 7, from the PSP contexts' bin_desc (0 for a TA
             * the file lacks); feature_version is never set for them */
            uint32_t i = in->query_fw.index;
            if (i < 2 || i > 7)
                return -RD_EINVAL;
            fw.ver = dev->ta_version[i];
            return info_out(f, in, &fw, sizeof(fw));
        }
        if (t < 32 && !(t == AMDGPU_INFO_FW_GFX_MEC && in->query_fw.index == 1)) {
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
    case AMDGPU_INFO_DEV_INFO: {
        /* amdgpu_info_ioctl -> amdgpu_gfx_get_gfx_shadow_info ->
         * gfx_v11_0_get_gfx_shadow_info: the shadow and CSA sizes only with
         * adev->gfx.cp_gfx_shadow (gfx_v11_0_check_fw_cp_gfx_shadow; not a
         * VF here, so never on this card), else they stay 0 */
        struct drm_amdgpu_info_device di;
        copy(&di, &dev->dev_info, sizeof(di));
        if (amd_gfx11_check_fw_cp_gfx_shadow(dev->gc_ip_version,
                                             dev->fw_version[AMDGPU_INFO_FW_GFX_ME],
                                             dev->fw_version[AMDGPU_INFO_FW_GFX_PFP],
                                             dev->fw_version[AMDGPU_INFO_FW_GFX_MEC],
                                             (int)dev->sriov_vf)) {
            di.shadow_size = RD_MQD_SHADOW_BASE_SIZE;
            di.shadow_alignment = RD_MQD_SHADOW_BASE_ALIGNMENT;
            di.csa_size = RD_MQD_FWWORKAREA_SIZE;
            di.csa_alignment = RD_MQD_FWWORKAREA_ALIGNMENT;
        } else {
            di.shadow_size = di.shadow_alignment = di.csa_size = di.csa_alignment = 0;
        }
        return info_out(f, in, &di, sizeof(di));
    }
    case AMDGPU_INFO_VRAM_LOST_COUNTER:
        /* amdgpu_info_ioctl: atomic_read(&adev->vram_lost_counter), 4 bytes */
        return info_out(f, in, &dev->vram_lost_counter, 4);
    case AMDGPU_INFO_NUM_EVICTIONS:
    case AMDGPU_INFO_NUM_VRAM_CPU_PAGE_FAULTS:
    case AMDGPU_INFO_NUM_BYTES_MOVED: {
        /* adev->num_evictions / num_vram_cpu_page_faults / num_bytes_moved
         * count TTM moves and CPU faults on VRAM BOs; nothing here moves a
         * BO (SET_PLACEMENT migration is not there yet) or faults one in,
         * and the sleep's VRAM copy (RDNA3Evict.cpp) works on ranges, not
         * BOs: 0 */
        uint64_t z = 0;
        return info_out(f, in, &z, 8);
    }
    case AMDGPU_INFO_MAX_IBS: {
        uint32_t max_ibs[AMDGPU_HW_IP_NUM];
        uint32_t i;
        /* amdgpu_ring_max_ibs of each IP's ring type, within what the CS
         * ioctl takes (more than RD_MAX_CS_IBS would be refused, so RADV
         * splits by this) */
        for (i = 0; i < AMDGPU_HW_IP_NUM; i++) {
            max_ibs[i] = amdgpu_ring_max_ibs(amdgpu_hw_ip_to_ring_type(i));
            if (max_ibs[i] > RD_MAX_CS_IBS)
                max_ibs[i] = RD_MAX_CS_IBS;
        }
        return info_out(f, in, max_ibs, sizeof(max_ibs));
    }
    case AMDGPU_INFO_VIDEO_CAPS: {
        struct drm_amdgpu_info_video_caps caps;
        const rd_codec *codecs;
        uint32_t i, count;
        zero(&caps, sizeof(caps));   /* without the VCN block: nothing valid */
        if (!dev->vcn_num_inst)
            return info_out(f, in, &caps, sizeof(caps));
        /* amdgpu_info_ioctl: soc21_query_video_codecs by type */
        if (in->video_cap.type != AMDGPU_INFO_VIDEO_CAPS_DECODE &&
            in->video_cap.type != AMDGPU_INFO_VIDEO_CAPS_ENCODE)
            return -RD_EINVAL;
        if (soc21_query_video_codecs(dev, in->video_cap.type == AMDGPU_INFO_VIDEO_CAPS_ENCODE, &codecs, &count))
            return -RD_EINVAL;
        for (i = 0; i < count; i++) {
            uint32_t idx = codecs[i].type;
            if (idx >= AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_COUNT)
                continue;
            caps.codec_info[idx].valid = 1;
            caps.codec_info[idx].max_width = codecs[i].w;
            caps.codec_info[idx].max_height = codecs[i].h;
            caps.codec_info[idx].max_pixels_per_frame = codecs[i].w * codecs[i].h;
            caps.codec_info[idx].max_level = codecs[i].level;
        }
        return info_out(f, in, &caps, sizeof(caps));
    }
    case AMDGPU_INFO_READ_MMR_REG: {
        /* amdgpu_info_ioctl: SE / SH indices (all bits = broadcast), at most
         * 128 registers, an offset outside soc21_allowed_read_registers is
         * -EFAULT, min(return_size, count * 4) bytes copied out */
        uint32_t se_num = (in->read_mmr_reg.instance >> AMDGPU_INFO_MMR_SE_INDEX_SHIFT) &
                          AMDGPU_INFO_MMR_SE_INDEX_MASK;
        uint32_t sh_num = (in->read_mmr_reg.instance >> AMDGPU_INFO_MMR_SH_INDEX_SHIFT) &
                          AMDGPU_INFO_MMR_SH_INDEX_MASK;
        uint32_t n = in->read_mmr_reg.count, regs[128], i;
        size_t len;
        if (se_num == AMDGPU_INFO_MMR_SE_INDEX_MASK)
            se_num = 0xffffffff;
        else if (se_num >= 4)            /* AMDGPU_GFX_MAX_SE */
            return -RD_EINVAL;
        if (sh_num == AMDGPU_INFO_MMR_SH_INDEX_MASK)
            sh_num = 0xffffffff;
        else if (sh_num >= 2)            /* AMDGPU_GFX_MAX_SH_PER_SE */
            return -RD_EINVAL;
        if (n > 128)
            return -RD_EINVAL;
        for (i = 0; i < n; i++)
            if (soc21_read_register(dev, se_num, sh_num, in->read_mmr_reg.dword_offset + i, &regs[i]))
                return -RD_EFAULT;   /* "unallowed offset" */
        len = MIN((size_t)in->return_size, (size_t)n * 4);
        if (!len)
            return 0;
        return dev->ops.copyout(dev->ops.ctx, in->return_pointer, regs, len) ? -RD_EFAULT : 0;
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
    case AMDGPU_INFO_NUM_HANDLES:
        /* amdgpu_info_ioctl: UVD handles are counted before Polaris only,
         * later ASICs have unlimited ones (-ENODATA); other IPs -EINVAL */
        return in->query_hw_ip.type == AMDGPU_HW_IP_UVD ? -RD_ENODATA : -RD_EINVAL;
    case AMDGPU_INFO_UQ_FW_AREAS: {
        /* amdgpu_info_ioctl: the firmware areas a user queue of that IP
         * needs (shadow and CSA), GFX only */
        struct drm_amdgpu_info_uq_metadata meta;
        zero(&meta, sizeof(meta));
        switch (in->query_hw_ip.type) {
        case AMDGPU_HW_IP_GFX:
            userq_metadata_info_gfx(&meta.gfx);
            return info_out(f, in, &meta, sizeof(meta));
        default:
            return -RD_EINVAL;
        }
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
    case AMDGPU_INFO_VBIOS:   /* amdgpu_info_ioctl: size, image (from offset), strings */
        switch (in->vbios_info.type) {
        case AMDGPU_INFO_VBIOS_SIZE: {
            uint32_t n = dev->vbios_len;
            return info_out(f, in, &n, 4);
        }
        case AMDGPU_INFO_VBIOS_IMAGE:
            if (!dev->vbios || in->vbios_info.offset >= dev->vbios_len)
                return -RD_EINVAL;
            return info_out(f, in, dev->vbios + in->vbios_info.offset, dev->vbios_len - in->vbios_info.offset);
        case AMDGPU_INFO_VBIOS_INFO:
            if (!dev->vbios)
                return -RD_EINVAL;
            return info_out(f, in, &dev->vbios_info, sizeof(dev->vbios_info));
        default:
            return -RD_EINVAL;
        }
    default:
        return -RD_EINVAL;
    }
}

/* ------------------------------ dispatch ------------------------------ */

#define NEED(type) do { if (size < sizeof(type)) return -RD_EINVAL; } while (0)

static int drm_ioctl_dispatch(rdna3_drm_file *f, uint32_t nr, void *data, uint32_t size)
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
        return syncobj_wait(f, w->handles, w->count_handles, 0, w->flags,
                            w->timeout_nsec > 0 ? (uint64_t)w->timeout_nsec : 0, &w->first_signaled);
    }
    case RD_NR_SYNCOBJ_TIMELINE_WAIT: {
        struct drm_syncobj_timeline_wait *w = (struct drm_syncobj_timeline_wait *)data;
        NEED(struct drm_syncobj_timeline_wait);
        return syncobj_wait(f, w->handles, w->count_handles, w->points, w->flags,
                            w->timeout_nsec > 0 ? (uint64_t)w->timeout_nsec : 0, &w->first_signaled);
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
    case RD_NR_AMDGPU(DRM_AMDGPU_GEM_WAIT_IDLE): NEED(union drm_amdgpu_gem_wait_idle);
        return gem_wait_idle(f, (union drm_amdgpu_gem_wait_idle *)data);
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
    case RD_NR_AMDGPU(DRM_AMDGPU_SCHED): NEED(union drm_amdgpu_sched);
        return sched_ioctl(f, (union drm_amdgpu_sched *)data);
    case RD_NR_AMDGPU(DRM_AMDGPU_PROC_OPTIONS): NEED(struct drm_amdgpu_proc_options);
        return proc_options_ioctl((struct drm_amdgpu_proc_options *)data);
    case RD_NR_AMDGPU(DRM_AMDGPU_USERQ): NEED(union drm_amdgpu_userq);
        return userq_ioctl(f, (union drm_amdgpu_userq *)data);
    case RD_NR_AMDGPU(DRM_AMDGPU_USERQ_SIGNAL): NEED(struct drm_amdgpu_userq_signal);
        return userq_signal_ioctl(f, (struct drm_amdgpu_userq_signal *)data);
    case RD_NR_AMDGPU(DRM_AMDGPU_USERQ_WAIT): NEED(struct drm_amdgpu_userq_wait);
        return userq_wait_ioctl(f, (struct drm_amdgpu_userq_wait *)data);
    case RD_NR_XNU_SURFACE_BO: NEED(struct rd_xnu_surface_bo);
        return surface_bo(f, (struct rd_xnu_surface_bo *)data);
    case RD_NR_XNU_FILE_ID: NEED(struct rd_xnu_file_id);
        ((struct rd_xnu_file_id *)data)->id = f->id;
        ((struct rd_xnu_file_id *)data)->pad = 0;
        return 0;
    case RD_NR_XNU_SCANOUT: NEED(struct rd_xnu_scanout);
        if (!f->dev->ops.scanout)
            return -RD_EOPNOTSUPP;
        return f->dev->ops.scanout(f->dev->ops.ctx, (struct rd_xnu_scanout *)data);
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
    case RD_NR_XNU_SCANOUT: return "XNU_SCANOUT";
    case RD_NR_XNU_FILE_ID: return "XNU_FILE_ID";
    case RD_NR_XNU_SURFACE_BO: return "XNU_SURFACE_BO";
    case RD_NR_AMDGPU(DRM_AMDGPU_SCHED): return "AMDGPU_SCHED";
    case RD_NR_AMDGPU(DRM_AMDGPU_PROC_OPTIONS): return "AMDGPU_PROC_OPTIONS";
    case RD_NR_AMDGPU(DRM_AMDGPU_GEM_USERPTR): return "AMDGPU_GEM_USERPTR";
    case RD_NR_AMDGPU(DRM_AMDGPU_GEM_CREATE): return "AMDGPU_GEM_CREATE";
    case RD_NR_AMDGPU(DRM_AMDGPU_GEM_MMAP): return "AMDGPU_GEM_MMAP";
    case RD_NR_AMDGPU(DRM_AMDGPU_CTX): return "AMDGPU_CTX";
    case RD_NR_AMDGPU(DRM_AMDGPU_CS): return "AMDGPU_CS";
    case RD_NR_AMDGPU(DRM_AMDGPU_INFO): return "AMDGPU_INFO";
    case RD_NR_AMDGPU(DRM_AMDGPU_GEM_VA): return "AMDGPU_GEM_VA";
    case RD_NR_AMDGPU(DRM_AMDGPU_WAIT_CS): return "AMDGPU_WAIT_CS";
    case RD_NR_AMDGPU(DRM_AMDGPU_USERQ): return "AMDGPU_USERQ";
    case RD_NR_AMDGPU(DRM_AMDGPU_USERQ_SIGNAL): return "AMDGPU_USERQ_SIGNAL";
    case RD_NR_AMDGPU(DRM_AMDGPU_USERQ_WAIT): return "AMDGPU_USERQ_WAIT";
    default: return "?";
    }
}

/* drm_ioctl: a client built against an older UAPI passes a shorter struct
 * (e.g. drm_amdgpu_gem_op before num_entries, 16 bytes instead of 24); the
 * kernel copies min(user, kernel) bytes in, zero-fills the rest and copies
 * the same length back out. Do the same, so an older libdrm/Mesa works and
 * a newer one does not lose its fields. */
int rdna3_drm_ioctl(rdna3_drm_file *f, uint32_t nr, void *data, uint32_t size)
{
    uint64_t ext[64];   /* 512 bytes: larger than every DRM/amdgpu ioctl struct */
    int r;
    if (f->closing)
        return -RD_ENODEV;
    if (size < 8 || size >= sizeof(ext))
        return drm_ioctl_dispatch(f, nr, data, size);
    uint32_t i;
    zero(ext, sizeof(ext));
    for (i = 0; i < size; i++)
        ((uint8_t *)ext)[i] = ((const uint8_t *)data)[i];
    r = drm_ioctl_dispatch(f, nr, ext, sizeof(ext));
    for (i = 0; i < size; i++)
        ((uint8_t *)data)[i] = ((const uint8_t *)ext)[i];
    return r;
}
