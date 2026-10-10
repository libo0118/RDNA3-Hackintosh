/*
 * mmhub302.c - MMHUB VMID0 GART and contexts, ported from Linux amdgpu
 * mmhub_v3_0_2.c, plus the MMHUB half of gmc_v11_0.c. Navi 32 is MMHUB
 * 3.0.0 (mmhub_v3_0.c): the same sequence on bare metal, other register
 * offsets, so it uses the generated mmhub_3_0_0 header
 * (gart_enable, MMIO TLB flush with the invalidation semaphore). MIT.
 *
 * SDMA, IH, VCN, DCN and the PSP/SMU clients reach memory through MMHUB,
 * so this is what gmc_v11_0_gart_enable() turns on; GFXHUB (gmc11.c)
 * follows in gfx_v11_0_hw_init().
 *
 * SPDX-License-Identifier: MIT
 */
#include "gmc11.h"
#include "gen/mmhub_3_0_0.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* soc15_common.h REG_SET_FIELD */
#define REG_FIELD_SHIFT(reg, field) reg##__##field##__SHIFT
#define REG_FIELD_MASK(reg, field)  reg##__##field##_MASK
#define REG_SET_FIELD(orig, reg, field, val) \
    (((orig) & ~(uint32_t)REG_FIELD_MASK(reg, field)) | \
     ((uint32_t)REG_FIELD_MASK(reg, field) & ((uint32_t)(val) << REG_FIELD_SHIFT(reg, field))))

#define RMM(d, reg) \
    rdna3_rreg((d), rdna3_reg_offset((d), HWIP_MMHUB, 0, reg##_BASE_IDX, reg))
#define WMM(d, reg, v) \
    rdna3_wreg((d), rdna3_reg_offset((d), HWIP_MMHUB, 0, reg##_BASE_IDX, reg), (v))
#define RMM_OFF(d, reg, o) \
    rdna3_rreg((d), rdna3_reg_offset((d), HWIP_MMHUB, 0, reg##_BASE_IDX, reg) + (o))
#define WMM_OFF(d, reg, o, v) \
    rdna3_wreg((d), rdna3_reg_offset((d), HWIP_MMHUB, 0, reg##_BASE_IDX, reg) + (o), (v))

/* mmhub_v3_0_init(): distances */
#define CTX_DISTANCE      (regMMVM_CONTEXT1_CNTL - regMMVM_CONTEXT0_CNTL)
#define CTX_ADDR_DISTANCE (regMMVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32 - \
                           regMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32)
#define ENG_DISTANCE      (regMMVM_INVALIDATE_ENG1_REQ - regMMVM_INVALIDATE_ENG0_REQ)
#define ENG_ADDR_DISTANCE (regMMVM_INVALIDATE_ENG1_ADDR_RANGE_LO32 - \
                           regMMVM_INVALIDATE_ENG0_ADDR_RANGE_LO32)

/* mmhub_v3_0.c */
#define regMMVM_L2_CNTL3_DEFAULT 0x80100007
#define regMMVM_L2_CNTL4_DEFAULT 0x000000c1
#define regMMVM_L2_CNTL5_DEFAULT 0x00003fe0

#define MTYPE_UC 3   /* navi10_enum.h */

/* mmhub_v3_0_setup_vm_pt_regs */
void mmhub302_setup_vm_pt_regs(rdna3_dev *d, uint32_t vmid, uint64_t pd)
{
    WMM_OFF(d, regMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32, CTX_ADDR_DISTANCE * vmid, (uint32_t)pd);
    WMM_OFF(d, regMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32, CTX_ADDR_DISTANCE * vmid,
            (uint32_t)(pd >> 32));
}

/* mmhub_v3_0_init_gart_aperture_regs */
static void init_gart_aperture_regs(rdna3_dev *d, rdna3_gmc *g)
{
    uint64_t pt_base = rdna3_vram_pa(d, g->gart_table) | RD_PTE_VALID;

    mmhub302_setup_vm_pt_regs(d, 0, pt_base);
    WMM(d, regMMVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32, (uint32_t)(g->gart_start >> 12));
    WMM(d, regMMVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32, (uint32_t)(g->gart_start >> 44));
    WMM(d, regMMVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32, (uint32_t)(g->gart_end >> 12));
    WMM(d, regMMVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32, (uint32_t)(g->gart_end >> 44));
}

/* mmhub_v3_0_init_system_aperture_regs */
static void init_system_aperture_regs(rdna3_dev *d, rdna3_gmc *g)
{
    uint64_t value;
    uint32_t tmp;

    WMM(d, regMMMC_VM_AGP_BASE, 0);
    WMM(d, regMMMC_VM_AGP_BOT, (uint32_t)(g->agp_start >> 24));
    WMM(d, regMMMC_VM_AGP_TOP, (uint32_t)(g->agp_end >> 24));

    WMM(d, regMMMC_VM_SYSTEM_APERTURE_LOW_ADDR, (uint32_t)(MIN(d->fb_start, g->agp_start) >> 18));
    WMM(d, regMMMC_VM_SYSTEM_APERTURE_HIGH_ADDR, (uint32_t)(MAX(d->fb_end, g->agp_end) >> 18));

    value = rdna3_vram_pa(d, g->scratch);
    WMM(d, regMMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB, (uint32_t)(value >> 12));
    WMM(d, regMMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB, (uint32_t)(value >> 44));

    WMM(d, regMMVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32, (uint32_t)(g->dummy_page_pa >> 12));
    WMM(d, regMMVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32, (uint32_t)(g->dummy_page_pa >> 44));

    tmp = RMM(d, regMMVM_L2_PROTECTION_FAULT_CNTL2);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL2, ACTIVE_PAGE_MIGRATION_PTE_READ_RETRY, 1);
    WMM(d, regMMVM_L2_PROTECTION_FAULT_CNTL2, tmp);
}

/* mmhub_v3_0_init_tlb_regs */
static void init_tlb_regs(rdna3_dev *d)
{
    uint32_t tmp = RMM(d, regMMMC_VM_MX_L1_TLB_CNTL);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL, ENABLE_L1_TLB, 1);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL, SYSTEM_ACCESS_MODE, 3);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL, ENABLE_ADVANCED_DRIVER_MODEL, 1);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL, SYSTEM_APERTURE_UNMAPPED_ACCESS, 0);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL, ECO_BITS, 0);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL, MTYPE, MTYPE_UC);
    WMM(d, regMMMC_VM_MX_L1_TLB_CNTL, tmp);
}

/* mmhub_v3_0_init_cache_regs */
static void init_cache_regs(rdna3_dev *d)
{
    uint32_t tmp = RMM(d, regMMVM_L2_CNTL);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, ENABLE_L2_CACHE, 1);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, ENABLE_L2_FRAGMENT_PROCESSING, 0);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY, 1);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, L2_PDE0_CACHE_TAG_GENERATION_MODE, 0);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, PDE_FAULT_CLASSIFICATION, 0);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, CONTEXT1_IDENTITY_ACCESS_MODE, 1);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, IDENTITY_MODE_FRAGMENT_SIZE, 0);
    WMM(d, regMMVM_L2_CNTL, tmp);

    tmp = RMM(d, regMMVM_L2_CNTL2);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL2, INVALIDATE_ALL_L1_TLBS, 1);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL2, INVALIDATE_L2_CACHE, 1);
    WMM(d, regMMVM_L2_CNTL2, tmp);

    tmp = regMMVM_L2_CNTL3_DEFAULT;          /* translate_further == false */
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL3, BANK_SELECT, 9);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL3, L2_CACHE_BIGK_FRAGMENT_SIZE, 6);
    WMM(d, regMMVM_L2_CNTL3, tmp);

    tmp = regMMVM_L2_CNTL4_DEFAULT;
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL4, VMC_TAP_PDE_REQUEST_PHYSICAL, 0);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL4, VMC_TAP_PTE_REQUEST_PHYSICAL, 0);
    WMM(d, regMMVM_L2_CNTL4, tmp);

    tmp = regMMVM_L2_CNTL5_DEFAULT;
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL5, L2_CACHE_SMALLK_FRAGMENT_SIZE, 0);
    WMM(d, regMMVM_L2_CNTL5, tmp);
}

/* mmhub_v3_0_enable_system_domain */
static void enable_system_domain(rdna3_dev *d)
{
    uint32_t tmp = RMM(d, regMMVM_CONTEXT0_CNTL);
    tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT0_CNTL, ENABLE_CONTEXT, 1);
    tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT0_CNTL, PAGE_TABLE_DEPTH, 0);
    tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT0_CNTL, RETRY_PERMISSION_OR_INVALID_PAGE_FAULT, 0);
    WMM(d, regMMVM_CONTEXT0_CNTL, tmp);
}

/* mmhub_v3_0_disable_identity_aperture */
static void disable_identity_aperture(rdna3_dev *d)
{
    WMM(d, regMMVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_LO32, 0xFFFFFFFF);
    WMM(d, regMMVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_HI32, 0x0000000F);
    WMM(d, regMMVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_LO32, 0);
    WMM(d, regMMVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_HI32, 0);
    WMM(d, regMMVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_LO32, 0);
    WMM(d, regMMVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_HI32, 0);
}

/* mmhub_v3_0_setup_vmid_config */
static void setup_vmid_config(rdna3_dev *d, rdna3_gmc *g)
{
    uint32_t i, tmp = 0;
    for (i = 0; i <= 14; i++) {
        tmp = RMM_OFF(d, regMMVM_CONTEXT1_CNTL, i * CTX_DISTANCE);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL, ENABLE_CONTEXT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL, PAGE_TABLE_DEPTH, g->num_level);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL, RANGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL, DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL, PDE0_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL, VALID_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL, READ_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL, WRITE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL, EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL, PAGE_TABLE_BLOCK_SIZE, g->block_size - 9);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL, RETRY_PERMISSION_OR_INVALID_PAGE_FAULT, 0);
        WMM_OFF(d, regMMVM_CONTEXT1_CNTL, i * CTX_DISTANCE, tmp);
        WMM_OFF(d, regMMVM_CONTEXT1_PAGE_TABLE_START_ADDR_LO32, i * CTX_ADDR_DISTANCE, 0);
        WMM_OFF(d, regMMVM_CONTEXT1_PAGE_TABLE_START_ADDR_HI32, i * CTX_ADDR_DISTANCE, 0);
        WMM_OFF(d, regMMVM_CONTEXT1_PAGE_TABLE_END_ADDR_LO32, i * CTX_ADDR_DISTANCE,
                (uint32_t)(g->max_pfn - 1));
        WMM_OFF(d, regMMVM_CONTEXT1_PAGE_TABLE_END_ADDR_HI32, i * CTX_ADDR_DISTANCE,
                (uint32_t)((g->max_pfn - 1) >> 32));
    }
    g->mm_vm_cntx_cntl = tmp;
}

/* mmhub_v3_0_program_invalidation */
static void program_invalidation(rdna3_dev *d)
{
    uint32_t i;
    for (i = 0; i < 18; ++i) {
        WMM_OFF(d, regMMVM_INVALIDATE_ENG0_ADDR_RANGE_LO32, i * ENG_ADDR_DISTANCE, 0xffffffff);
        WMM_OFF(d, regMMVM_INVALIDATE_ENG0_ADDR_RANGE_HI32, i * ENG_ADDR_DISTANCE, 0x1f);
    }
}

/* mmhub_v3_0_gart_enable */
int mmhub302_gart_enable(rdna3_dev *d, rdna3_gmc *g)
{
    init_gart_aperture_regs(d, g);
    init_system_aperture_regs(d, g);
    init_tlb_regs(d);
    init_cache_regs(d);
    enable_system_domain(d);
    disable_identity_aperture(d);
    setup_vmid_config(d, g);
    program_invalidation(d);
    g->mmhub_enabled = 1;
    return RDNA3_OK;
}

/* mmhub_v3_0_gart_disable */
void mmhub302_gart_disable(rdna3_dev *d)
{
    uint32_t i, tmp;
    for (i = 0; i < 16; i++)
        WMM_OFF(d, regMMVM_CONTEXT0_CNTL, i * CTX_DISTANCE, 0);
    tmp = RMM(d, regMMMC_VM_MX_L1_TLB_CNTL);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL, ENABLE_L1_TLB, 0);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL, ENABLE_ADVANCED_DRIVER_MODEL, 0);
    WMM(d, regMMMC_VM_MX_L1_TLB_CNTL, tmp);
    tmp = RMM(d, regMMVM_L2_CNTL);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, ENABLE_L2_CACHE, 0);
    WMM(d, regMMVM_L2_CNTL, tmp);
    WMM(d, regMMVM_L2_CNTL3, 0);
}

/* mmhub_v3_0_set_fault_enable_default */
void mmhub302_set_fault_enable_default(rdna3_dev *d, int value)
{
    uint32_t tmp = RMM(d, regMMVM_L2_PROTECTION_FAULT_CNTL);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL, RANGE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL, PDE0_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL, PDE1_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL, PDE2_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                        TRANSLATE_FURTHER_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL, NACK_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL, DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL, VALID_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL, READ_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL, WRITE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL, EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    if (!value) {
        tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL, CRASH_ON_NO_RETRY_FAULT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL, CRASH_ON_RETRY_FAULT, 1);
    }
    WMM(d, regMMVM_L2_PROTECTION_FAULT_CNTL, tmp);
}

/* gmc_v11_0_flush_gpu_tlb() for AMDGPU_MMHUB0, MMIO path (before KIQ/MES):
 * semaphore acquire, request, ack, semaphore release, then the MMHUB
 * private invalidation through L2_BANK_SELECT_RESERVED_CID2 bit 25. */
int mmhub302_flush_tlb(rdna3_dev *d, uint32_t vmid, uint32_t flush_type)
{
    const uint32_t eng = 17;
    uint32_t req = 0, i, tmp = 0;
    int ok = 0;

    /* mmhub_v3_0_get_invalidate_req */
    req = REG_SET_FIELD(req, MMVM_INVALIDATE_ENG0_REQ, PER_VMID_INVALIDATE_REQ, 1u << vmid);
    req = REG_SET_FIELD(req, MMVM_INVALIDATE_ENG0_REQ, FLUSH_TYPE, flush_type);
    req = REG_SET_FIELD(req, MMVM_INVALIDATE_ENG0_REQ, INVALIDATE_L2_PTES, 1);
    req = REG_SET_FIELD(req, MMVM_INVALIDATE_ENG0_REQ, INVALIDATE_L2_PDE0, 1);
    req = REG_SET_FIELD(req, MMVM_INVALIDATE_ENG0_REQ, INVALIDATE_L2_PDE1, 1);
    req = REG_SET_FIELD(req, MMVM_INVALIDATE_ENG0_REQ, INVALIDATE_L2_PDE2, 1);
    req = REG_SET_FIELD(req, MMVM_INVALIDATE_ENG0_REQ, INVALIDATE_L1_PTES, 1);
    req = REG_SET_FIELD(req, MMVM_INVALIDATE_ENG0_REQ, CLEAR_PROTECTION_FAULT_STATUS_ADDR, 0);

    rdna3_hdp_flush(d);

    for (i = 0; i < d->usec_timeout; i++) {
        /* a read of 1 means the semaphore was acquired */
        if (RMM_OFF(d, regMMVM_INVALIDATE_ENG0_SEM, ENG_DISTANCE * eng) & 0x1)
            break;
        d->os.udelay(d->os.ctx, 1);
    }
    if (i >= d->usec_timeout)
        RERR(d, "GMC: MMHUB invalidation semaphore timeout");

    WMM_OFF(d, regMMVM_INVALIDATE_ENG0_REQ, ENG_DISTANCE * eng, req);
    for (i = 0; i < d->usec_timeout; i++) {
        tmp = RMM_OFF(d, regMMVM_INVALIDATE_ENG0_ACK, ENG_DISTANCE * eng);
        if (tmp & (1u << vmid)) {
            ok = 1;
            break;
        }
        d->os.udelay(d->os.ctx, 1);
    }
    WMM_OFF(d, regMMVM_INVALIDATE_ENG0_SEM, ENG_DISTANCE * eng, 0);

    tmp = RMM(d, regMMVM_L2_BANK_SELECT_RESERVED_CID2);
    tmp |= 1u << 25;   /* RSERVED_CACHE_PRIVATE_INVALIDATION */
    WMM(d, regMMVM_L2_BANK_SELECT_RESERVED_CID2, tmp);
    (void)RMM(d, regMMVM_L2_BANK_SELECT_RESERVED_CID2);

    if (!ok) {
        RERR(d, "GMC: MMHUB TLB flush timeout (vmid %u)", vmid);
        return RDNA3_ETIME;
    }
    return RDNA3_OK;
}

/* mmhub_v3_0_get_fb_location */
uint64_t mmhub302_get_fb_location(rdna3_dev *d)
{
    uint64_t base = RMM(d, regMMMC_VM_FB_LOCATION_BASE);
    return (base & MMMC_VM_FB_LOCATION_BASE__FB_BASE_MASK) << 24;
}

/* mmhub_v3_0_get_mc_fb_offset */
uint64_t mmhub302_get_mc_fb_offset(rdna3_dev *d)
{
    return (uint64_t)RMM(d, regMMMC_VM_FB_OFFSET) << 24;
}

/* gmc_v11_0_gart_enable(): MMHUB, HDP flush, fault defaults, flush VMID0 */
int gmc11_gart_enable(rdna3_dev *d, rdna3_gmc *g)
{
    int r = mmhub302_gart_enable(d, g);
    if (r)
        return r;
    rdna3_hdp_flush(d);
    /* amdgpu_vm_fault_stop default (AMDGPU_VM_FAULT_STOP_NEVER) -> true */
    mmhub302_set_fault_enable_default(d, 1);
    r = mmhub302_flush_tlb(d, 0, 0);
    if (r == RDNA3_OK)
        RLOG(d, "GMC: PCIE GART of %lluM enabled on MMHUB (table at VRAM 0x%llx)",
             (unsigned long long)(g->gart_size >> 20), (unsigned long long)g->gart_table);
    return r;
}

/* ------------------------- page faults ------------------------- */

#define MM_FAULT_IRQ_MASK \
    (MMVM_CONTEXT1_CNTL__RANGE_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK | \
     MMVM_CONTEXT1_CNTL__DUMMY_PAGE_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK | \
     MMVM_CONTEXT1_CNTL__PDE0_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK | \
     MMVM_CONTEXT1_CNTL__VALID_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK | \
     MMVM_CONTEXT1_CNTL__READ_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK | \
     MMVM_CONTEXT1_CNTL__WRITE_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK | \
     MMVM_CONTEXT1_CNTL__EXECUTE_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK)

uint32_t mmhub302_fault_status(rdna3_dev *d, int clear)
{
    uint32_t st = RMM(d, regMMVM_L2_PROTECTION_FAULT_STATUS);
    if (clear)
        WMM(d, regMMVM_L2_PROTECTION_FAULT_CNTL,
            (RMM(d, regMMVM_L2_PROTECTION_FAULT_CNTL) & ~1u) | 1u);
    return st;
}

void mmhub302_set_vm_fault_interrupts(rdna3_dev *d, int enable)
{
    uint32_t i, tmp;
    for (i = 0; i < 16; i++) {
        tmp = RMM_OFF(d, regMMVM_CONTEXT0_CNTL, i * CTX_DISTANCE);
        if (enable)
            tmp |= MM_FAULT_IRQ_MASK;
        else
            tmp &= ~MM_FAULT_IRQ_MASK;
        WMM_OFF(d, regMMVM_CONTEXT0_CNTL, i * CTX_DISTANCE, tmp);
    }
}
