/*
 * gmc11.c - see gmc11.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gmc11.h"
#include "gen/gc_11_0_3_gmc.h"

#define FOUR_GB (1ull << 32)
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((uint64_t)(a) - 1))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* soc15_common.h REG_SET_FIELD / REG_GET_FIELD */
#define REG_FIELD_SHIFT(reg, field) reg##__##field##__SHIFT
#define REG_FIELD_MASK(reg, field)  reg##__##field##_MASK
#define REG_SET_FIELD(orig, reg, field, val) \
    (((orig) & ~(uint32_t)REG_FIELD_MASK(reg, field)) | \
     ((uint32_t)REG_FIELD_MASK(reg, field) & ((uint32_t)(val) << REG_FIELD_SHIFT(reg, field))))

/* paste reg##_BASE_IDX at the outermost macro so 'reg' is not expanded first */
#define RGC(d, reg) \
    rdna3_rreg((d), rdna3_reg_offset((d), HWIP_GC, 0, reg##_BASE_IDX, reg))
#define WGC(d, reg, v) \
    rdna3_wreg((d), rdna3_reg_offset((d), HWIP_GC, 0, reg##_BASE_IDX, reg), (v))
#define WGC_OFF(d, reg, o, v) \
    rdna3_wreg((d), rdna3_reg_offset((d), HWIP_GC, 0, reg##_BASE_IDX, reg) + (o), (v))
#define RGC_OFF(d, reg, o) \
    rdna3_rreg((d), rdna3_reg_offset((d), HWIP_GC, 0, reg##_BASE_IDX, reg) + (o))

/* gfxhub_v3_0_3_init(): distances between per-context / per-engine regs */
#define CTX_DISTANCE      (regGCVM_CONTEXT1_CNTL - regGCVM_CONTEXT0_CNTL)
#define CTX_ADDR_DISTANCE (regGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32 - \
                           regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32)
#define ENG_DISTANCE      (regGCVM_INVALIDATE_ENG1_REQ - regGCVM_INVALIDATE_ENG0_REQ)
#define ENG_ADDR_DISTANCE (regGCVM_INVALIDATE_ENG1_ADDR_RANGE_LO32 - \
                           regGCVM_INVALIDATE_ENG0_ADDR_RANGE_LO32)

#define MTYPE_UC 3   /* navi10_enum.h: MTYPE_UC */

/* ------------------------------ layout ------------------------------ */

int gmc11_init(rdna3_dev *d, rdna3_gmc *g, uint64_t gart_size)
{
    uint64_t max_mc = RD_GMC_HOLE_START - 1;   /* min(mc_mask, HOLE_START - 1) */
    uint64_t size_bf, size_af, table_size;

    if (!d->fb_end || d->fb_end <= d->fb_start)
        return RDNA3_ENODEV;

    g->gart_size = gart_size ? gart_size : RD_GART_SIZE_DEFAULT;
    /* amdgpu_gmc_gart_location(BEST_FIT) */
    size_bf = d->fb_start;
    size_af = max_mc + 1 - ALIGN_UP(d->fb_end + 1, FOUR_GB);
    if (g->gart_size > MAX(size_bf, size_af))
        g->gart_size = MAX(size_bf, size_af);
    if ((size_bf >= g->gart_size && size_bf < size_af) || size_af < g->gart_size)
        g->gart_start = 0;
    else
        g->gart_start = max_mc - g->gart_size + 1;
    g->gart_start &= ~(FOUR_GB - 1);
    g->gart_end = g->gart_start + g->gart_size - 1;

    /* amdgpu_gmc_set_agp_default */
    /* AGP aperture unused (amdgpu_gmc_agp_location would place it in a
     * free hole; disabled keeps the system aperture == FB) */
    g->agp_start = 0xFFFFFFFFFFFFull;
    g->agp_end = 0;

    /* amdgpu_vm_adjust_size(adev, 256 * 1024, 9, 3, 48) */
    g->max_pfn = 1ull << (48 - 12);
    g->num_level = 3;
    g->block_size = 9;

    table_size = (g->gart_size / 4096) * 8;
    g->gart_table = rdna3_vram_alloc(d, table_size, 4096, "gart-table");
    g->scratch = rdna3_vram_alloc(d, 4096, 4096, "mem-scratch");
    if (g->gart_table == UINT64_MAX || g->scratch == UINT64_MAX)
        return RDNA3_ENOMEM;
    rdna3_vram_fill(d, g->gart_table, 0, table_size);
    rdna3_vram_fill(d, g->scratch, 0, 4096);
    rdna3_hdp_flush(d);
    /* TODO(os): a real system-memory dummy page; the VRAM scratch page
     * stands in for it until the OS layer provides one */
    g->dummy_page_pa = rdna3_vram_pa(d, g->scratch);

    RLOG(d, "GMC: FB 0x%llx-0x%llx, GART %llu MiB at 0x%llx-0x%llx, table at VRAM 0x%llx",
         (unsigned long long)d->fb_start, (unsigned long long)d->fb_end,
         (unsigned long long)(g->gart_size >> 20), (unsigned long long)g->gart_start,
         (unsigned long long)g->gart_end, (unsigned long long)g->gart_table);
    return RDNA3_OK;
}

int gmc11_gart_map(rdna3_dev *d, rdna3_gmc *g, uint64_t gart_off, uint64_t pa,
                   uint32_t pages, uint64_t flags)
{
    uint64_t first = gart_off / 4096, i;
    if ((gart_off & 4095) || (pa & 4095) || first + pages > g->gart_size / 4096)
        return RDNA3_EINVAL;
    for (i = 0; i < pages; i++) {
        uint64_t pte = ((pa + i * 4096) & 0x0000FFFFFFFFF000ull) | flags;
        rdna3_vram_write(d, g->gart_table + (first + i) * 8, &pte, 8);
    }
    rdna3_hdp_flush(d);
    return RDNA3_OK;
}

void gmc11_gart_unmap(rdna3_dev *d, rdna3_gmc *g, uint64_t gart_off, uint32_t pages)
{
    uint64_t first = gart_off / 4096, i, zero = 0;
    for (i = 0; i < pages && first + i < g->gart_size / 4096; i++)
        rdna3_vram_write(d, g->gart_table + (first + i) * 8, &zero, 8);
    rdna3_hdp_flush(d);
}

/* ---------------------------- GFXHUB 3.0.3 ---------------------------- */

void gfxhub303_setup_vm_pt_regs(rdna3_dev *d, uint32_t vmid, uint64_t pd)
{
    WGC_OFF(d, regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32, CTX_ADDR_DISTANCE * vmid, (uint32_t)pd);
    WGC_OFF(d, regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32, CTX_ADDR_DISTANCE * vmid,
            (uint32_t)(pd >> 32));
}

/* gfxhub_v3_0_3_init_gart_aperture_regs */
static void init_gart_aperture_regs(rdna3_dev *d, rdna3_gmc *g)
{
    /* amdgpu_gmc_pd_addr(gart.bo): VRAM physical address | VALID */
    uint64_t pt_base = rdna3_vram_pa(d, g->gart_table) | RD_PTE_VALID;

    gfxhub303_setup_vm_pt_regs(d, 0, pt_base);
    WGC(d, regGCVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32, (uint32_t)(g->gart_start >> 12));
    WGC(d, regGCVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32, (uint32_t)(g->gart_start >> 44));
    WGC(d, regGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32, (uint32_t)(g->gart_end >> 12));
    WGC(d, regGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32, (uint32_t)(g->gart_end >> 44));
}

/* gfxhub_v3_0_3_init_system_aperture_regs */
static void init_system_aperture_regs(rdna3_dev *d, rdna3_gmc *g)
{
    uint64_t value;
    uint32_t tmp;

    WGC(d, regGCMC_VM_AGP_BASE, 0);
    WGC(d, regGCMC_VM_AGP_BOT, (uint32_t)(g->agp_start >> 24));
    WGC(d, regGCMC_VM_AGP_TOP, (uint32_t)(g->agp_end >> 24));

    WGC(d, regGCMC_VM_SYSTEM_APERTURE_LOW_ADDR, (uint32_t)(MIN(d->fb_start, g->agp_start) >> 18));
    WGC(d, regGCMC_VM_SYSTEM_APERTURE_HIGH_ADDR, (uint32_t)(MAX(d->fb_end, g->agp_end) >> 18));

    /* default page: amdgpu_gmc_vram_mc2pa(mem_scratch) */
    value = rdna3_vram_pa(d, g->scratch);
    WGC(d, regGCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB, (uint32_t)(value >> 12));
    WGC(d, regGCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB, (uint32_t)(value >> 44));

    WGC(d, regGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32, (uint32_t)(g->dummy_page_pa >> 12));
    WGC(d, regGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32, (uint32_t)(g->dummy_page_pa >> 44));

    tmp = RGC(d, regGCVM_L2_PROTECTION_FAULT_CNTL2);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL2, ACTIVE_PAGE_MIGRATION_PTE_READ_RETRY, 1);
    WGC(d, regGCVM_L2_PROTECTION_FAULT_CNTL2, tmp);
}

/* gfxhub_v3_0_3_init_tlb_regs */
static void init_tlb_regs(rdna3_dev *d)
{
    uint32_t tmp = RGC(d, regGCMC_VM_MX_L1_TLB_CNTL);
    tmp = REG_SET_FIELD(tmp, GCMC_VM_MX_L1_TLB_CNTL, ENABLE_L1_TLB, 1);
    tmp = REG_SET_FIELD(tmp, GCMC_VM_MX_L1_TLB_CNTL, SYSTEM_ACCESS_MODE, 3);
    tmp = REG_SET_FIELD(tmp, GCMC_VM_MX_L1_TLB_CNTL, ENABLE_ADVANCED_DRIVER_MODEL, 1);
    tmp = REG_SET_FIELD(tmp, GCMC_VM_MX_L1_TLB_CNTL, SYSTEM_APERTURE_UNMAPPED_ACCESS, 0);
    tmp = REG_SET_FIELD(tmp, GCMC_VM_MX_L1_TLB_CNTL, ECO_BITS, 0);
    tmp = REG_SET_FIELD(tmp, GCMC_VM_MX_L1_TLB_CNTL, MTYPE, MTYPE_UC);
    WGC(d, regGCMC_VM_MX_L1_TLB_CNTL, tmp);
}

/* gfxhub_v3_0_3_init_cache_regs */
static void init_cache_regs(rdna3_dev *d)
{
    uint32_t tmp = RGC(d, regGCVM_L2_CNTL);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL, ENABLE_L2_CACHE, 1);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL, ENABLE_L2_FRAGMENT_PROCESSING, 0);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL, ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY, 1);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL, L2_PDE0_CACHE_TAG_GENERATION_MODE, 0);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL, PDE_FAULT_CLASSIFICATION, 0);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL, CONTEXT1_IDENTITY_ACCESS_MODE, 1);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL, IDENTITY_MODE_FRAGMENT_SIZE, 0);
    WGC(d, regGCVM_L2_CNTL, tmp);

    tmp = RGC(d, regGCVM_L2_CNTL2);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL2, INVALIDATE_ALL_L1_TLBS, 1);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL2, INVALIDATE_L2_CACHE, 1);
    WGC(d, regGCVM_L2_CNTL2, tmp);

    tmp = regGCVM_L2_CNTL3_DEFAULT;          /* translate_further == false */
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL3, BANK_SELECT, 9);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL3, L2_CACHE_BIGK_FRAGMENT_SIZE, 6);
    WGC(d, regGCVM_L2_CNTL3, tmp);

    tmp = regGCVM_L2_CNTL4_DEFAULT;
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL4, VMC_TAP_PDE_REQUEST_PHYSICAL, 0);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL4, VMC_TAP_PTE_REQUEST_PHYSICAL, 0);
    WGC(d, regGCVM_L2_CNTL4, tmp);

    tmp = regGCVM_L2_CNTL5_DEFAULT;
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL5, L2_CACHE_SMALLK_FRAGMENT_SIZE, 0);
    WGC(d, regGCVM_L2_CNTL5, tmp);
}

/* gfxhub_v3_0_3_enable_system_domain */
static void enable_system_domain(rdna3_dev *d)
{
    uint32_t tmp = RGC(d, regGCVM_CONTEXT0_CNTL);
    tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT0_CNTL, ENABLE_CONTEXT, 1);
    tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT0_CNTL, PAGE_TABLE_DEPTH, 0);
    tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT0_CNTL, RETRY_PERMISSION_OR_INVALID_PAGE_FAULT, 0);
    WGC(d, regGCVM_CONTEXT0_CNTL, tmp);
}

/* gfxhub_v3_0_3_disable_identity_aperture */
static void disable_identity_aperture(rdna3_dev *d)
{
    WGC(d, regGCVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_LO32, 0xFFFFFFFF);
    WGC(d, regGCVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_HI32, 0x0000000F);
    WGC(d, regGCVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_LO32, 0);
    WGC(d, regGCVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_HI32, 0);
    WGC(d, regGCVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_LO32, 0);
    WGC(d, regGCVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_HI32, 0);
}

/* gfxhub_v3_0_3_setup_vmid_config */
static void setup_vmid_config(rdna3_dev *d, rdna3_gmc *g)
{
    uint32_t i, tmp = 0;
    for (i = 0; i <= 14; i++) {
        tmp = RGC_OFF(d, regGCVM_CONTEXT1_CNTL, i * CTX_DISTANCE);
        tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT1_CNTL, ENABLE_CONTEXT, 1);
        tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT1_CNTL, PAGE_TABLE_DEPTH, g->num_level);
        tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT1_CNTL, RANGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT1_CNTL, DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT1_CNTL, PDE0_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT1_CNTL, VALID_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT1_CNTL, READ_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT1_CNTL, WRITE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT1_CNTL, EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT1_CNTL, PAGE_TABLE_BLOCK_SIZE, g->block_size - 9);
        /* noretry = 1 on dGPUs: no retry XNACK */
        tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT1_CNTL, RETRY_PERMISSION_OR_INVALID_PAGE_FAULT, 0);
        WGC_OFF(d, regGCVM_CONTEXT1_CNTL, i * CTX_DISTANCE, tmp);
        WGC_OFF(d, regGCVM_CONTEXT1_PAGE_TABLE_START_ADDR_LO32, i * CTX_ADDR_DISTANCE, 0);
        WGC_OFF(d, regGCVM_CONTEXT1_PAGE_TABLE_START_ADDR_HI32, i * CTX_ADDR_DISTANCE, 0);
        WGC_OFF(d, regGCVM_CONTEXT1_PAGE_TABLE_END_ADDR_LO32, i * CTX_ADDR_DISTANCE,
                (uint32_t)(g->max_pfn - 1));
        WGC_OFF(d, regGCVM_CONTEXT1_PAGE_TABLE_END_ADDR_HI32, i * CTX_ADDR_DISTANCE,
                (uint32_t)((g->max_pfn - 1) >> 32));
    }
    g->vm_cntx_cntl = tmp;
}

/* gfxhub_v3_0_3_program_invalidation */
static void program_invalidation(rdna3_dev *d)
{
    uint32_t i;
    for (i = 0; i < 18; ++i) {
        WGC_OFF(d, regGCVM_INVALIDATE_ENG0_ADDR_RANGE_LO32, i * ENG_ADDR_DISTANCE, 0xffffffff);
        WGC_OFF(d, regGCVM_INVALIDATE_ENG0_ADDR_RANGE_HI32, i * ENG_ADDR_DISTANCE, 0x1f);
    }
}

int gfxhub303_gart_enable(rdna3_dev *d, rdna3_gmc *g)
{
    init_gart_aperture_regs(d, g);
    init_system_aperture_regs(d, g);
    init_tlb_regs(d);
    init_cache_regs(d);
    enable_system_domain(d);
    disable_identity_aperture(d);
    setup_vmid_config(d, g);
    program_invalidation(d);
    g->gfxhub_enabled = 1;
    return RDNA3_OK;
}

/* gfxhub_v3_0_3_set_fault_enable_default */
void gfxhub303_set_fault_enable_default(rdna3_dev *d, int value)
{
    uint32_t tmp = RGC(d, regGCVM_L2_PROTECTION_FAULT_CNTL);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, RANGE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, PDE0_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, PDE1_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, PDE2_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL,
                        TRANSLATE_FURTHER_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, NACK_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, VALID_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, READ_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, WRITE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, CRASH_ON_NO_RETRY_FAULT, !value);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, CRASH_ON_RETRY_FAULT, !value);
    WGC(d, regGCVM_L2_PROTECTION_FAULT_CNTL, tmp);
}

/* gfxhub_v3_0_3_get_invalidate_req */
static uint32_t invalidate_req(uint32_t vmid, uint32_t flush_type)
{
    uint32_t req = 0;
    req = REG_SET_FIELD(req, GCVM_INVALIDATE_ENG0_REQ, PER_VMID_INVALIDATE_REQ, 1u << vmid);
    req = REG_SET_FIELD(req, GCVM_INVALIDATE_ENG0_REQ, FLUSH_TYPE, flush_type);
    req = REG_SET_FIELD(req, GCVM_INVALIDATE_ENG0_REQ, INVALIDATE_L2_PTES, 1);
    req = REG_SET_FIELD(req, GCVM_INVALIDATE_ENG0_REQ, INVALIDATE_L2_PDE0, 1);
    req = REG_SET_FIELD(req, GCVM_INVALIDATE_ENG0_REQ, INVALIDATE_L2_PDE1, 1);
    req = REG_SET_FIELD(req, GCVM_INVALIDATE_ENG0_REQ, INVALIDATE_L2_PDE2, 1);
    req = REG_SET_FIELD(req, GCVM_INVALIDATE_ENG0_REQ, INVALIDATE_L1_PTES, 1);
    req = REG_SET_FIELD(req, GCVM_INVALIDATE_ENG0_REQ, CLEAR_PROTECTION_FAULT_STATUS_ADDR, 0);
    return req;
}

void gfxhub303_flush_regs(rdna3_dev *d, uint32_t vmid, uint32_t eng, rdna3_vm_flush_regs *o)
{
    o->pt_lo = rdna3_reg_offset(d, HWIP_GC, 0, regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32_BASE_IDX,
                                regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32) + CTX_ADDR_DISTANCE * vmid;
    o->pt_hi = rdna3_reg_offset(d, HWIP_GC, 0, regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32_BASE_IDX,
                                regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32) + CTX_ADDR_DISTANCE * vmid;
    o->req = rdna3_reg_offset(d, HWIP_GC, 0, regGCVM_INVALIDATE_ENG0_REQ_BASE_IDX,
                              regGCVM_INVALIDATE_ENG0_REQ) + ENG_DISTANCE * eng;
    o->ack = rdna3_reg_offset(d, HWIP_GC, 0, regGCVM_INVALIDATE_ENG0_ACK_BASE_IDX,
                              regGCVM_INVALIDATE_ENG0_ACK) + ENG_DISTANCE * eng;
    o->req_val = invalidate_req(vmid, 0);
}

int gfxhub303_flush_tlb(rdna3_dev *d, uint32_t vmid, uint32_t flush_type)
{
    const uint32_t eng = 17;   /* "Use register 17 for GART" */
    uint32_t req = invalidate_req(vmid, flush_type), i, ack = 0;

    rdna3_hdp_flush(d);
    /* GFXHUB: no invalidation semaphore (gmc_v11_0_use_invalidate_semaphore) */
    WGC_OFF(d, regGCVM_INVALIDATE_ENG0_REQ, ENG_DISTANCE * eng, req);
    for (i = 0; i < d->usec_timeout; i++) {
        ack = RGC_OFF(d, regGCVM_INVALIDATE_ENG0_ACK, ENG_DISTANCE * eng);
        if (ack & (1u << vmid))
            return RDNA3_OK;
        d->os.udelay(d->os.ctx, 1);
    }
    RERR(d, "GMC: GFXHUB TLB flush timeout (vmid %u, ack 0x%x)", vmid, ack);
    return RDNA3_ETIME;
}

/* ------------------------- page faults ------------------------- */

#define GC_FAULT_IRQ_MASK \
    (GCVM_CONTEXT1_CNTL__RANGE_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK | \
     GCVM_CONTEXT1_CNTL__DUMMY_PAGE_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK | \
     GCVM_CONTEXT1_CNTL__PDE0_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK | \
     GCVM_CONTEXT1_CNTL__VALID_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK | \
     GCVM_CONTEXT1_CNTL__READ_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK | \
     GCVM_CONTEXT1_CNTL__WRITE_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK | \
     GCVM_CONTEXT1_CNTL__EXECUTE_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK)

int gmc11_iv_is_vm_fault(const rdna3_iv *iv)
{
    if (iv->src_id != RD_IH_SRC_VM_FAULT)
        return 0;
    return iv->client_id == RD_IH_CLIENT_VMC || iv->client_id == RD_IH_CLIENT_GFX;
}

void gmc11_decode_vm_fault(rdna3_dev *d, const rdna3_iv *iv, rdna3_vm_fault *f)
{
    uint32_t st;
    f->hub = iv->client_id == RD_IH_CLIENT_VMC ? 1 : 0;
    f->vmid = iv->vmid;
    f->pasid = iv->pasid;
    f->client = iv->client_id;
    f->src_id = iv->src_id;
    f->ring_id = iv->ring_id;
    f->addr = ((uint64_t)iv->src_data[0] << 12) | (((uint64_t)iv->src_data[1] & 0xf) << 44);
    f->retry = (iv->src_data[1] & 0x80) != 0;   /* AMDGPU_GMC9_FAULT_SOURCE_DATA_RETRY */
    f->write = (iv->src_data[1] & 0x20) != 0;   /* AMDGPU_GMC9_FAULT_SOURCE_DATA_WRITE */
    if (f->hub) {
        st = mmhub302_fault_status(d, 1);
    } else {
        /* a dummy read first: the fast GRBM interface may hand out a stale value */
        (void)RGC(d, regGCVM_L2_PROTECTION_FAULT_STATUS);
        st = RGC(d, regGCVM_L2_PROTECTION_FAULT_STATUS);
        WGC(d, regGCVM_L2_PROTECTION_FAULT_CNTL,
            (RGC(d, regGCVM_L2_PROTECTION_FAULT_CNTL) & ~1u) | 1u);
    }
    f->status = st;
    f->more_faults = st & 0x1;
    f->walker_error = (st >> 1) & 0x7;
    f->permission_faults = (st >> 4) & 0xf;
    f->mapping_error = (st >> 8) & 0x1;
    f->cid = (st >> 9) & 0x1ff;
    if (st & (1u << 18))
        f->write = 1;
}

void gmc11_log_vm_fault(rdna3_dev *d, const rdna3_vm_fault *f)
{
    RERR(d, "[%s] page fault (src_id:%u ring:%u vmid:%u pasid:%u) at 0x%016llx from client %u",
         f->hub ? "mmhub" : "gfxhub", f->src_id, f->ring_id, f->vmid, f->pasid,
         (unsigned long long)f->addr, f->client);
    if (f->status)
        RERR(d, "  L2_PROTECTION_FAULT_STATUS 0x%08x: UTCL2 client 0x%x, MORE_FAULTS %u, "
             "WALKER_ERROR %u, PERMISSION_FAULTS 0x%x, MAPPING_ERROR %u, RW %s",
             f->status, f->cid, f->more_faults, f->walker_error, f->permission_faults,
             f->mapping_error, f->write ? "write" : "read");
}

void gmc11_set_vm_fault_interrupts(rdna3_dev *d, int enable)
{
    uint32_t i, tmp;
    for (i = 0; i < 16; i++) {
        tmp = RGC_OFF(d, regGCVM_CONTEXT0_CNTL, i * CTX_DISTANCE);
        if (enable)
            tmp |= GC_FAULT_IRQ_MASK;
        else
            tmp &= ~GC_FAULT_IRQ_MASK;
        WGC_OFF(d, regGCVM_CONTEXT0_CNTL, i * CTX_DISTANCE, tmp);
    }
    mmhub302_set_vm_fault_interrupts(d, enable);
}
