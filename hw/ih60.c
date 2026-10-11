/*
 * ih60.c - see ih60.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "ih60.h"
#include "gen/osssys_6_0_0.h"
#include "gen/nbio_4_3_0_ih.h"
#include "doorbell.h"

#define REG_FIELD_SHIFT(reg, field) reg##__##field##__SHIFT
#define REG_FIELD_MASK(reg, field)  reg##__##field##_MASK
#define REG_SET_FIELD(orig, reg, field, val) \
    (((orig) & ~(uint32_t)REG_FIELD_MASK(reg, field)) | \
     ((uint32_t)REG_FIELD_MASK(reg, field) & ((uint32_t)(val) << REG_FIELD_SHIFT(reg, field))))
#define REG_GET_FIELD(v, reg, field) \
    (((v) & (uint32_t)REG_FIELD_MASK(reg, field)) >> REG_FIELD_SHIFT(reg, field))

#define ROSS(d, reg) \
    rdna3_rreg((d), rdna3_reg_offset((d), HWIP_OSSSYS, 0, reg##_BASE_IDX, reg))
#define WOSS(d, reg, v) \
    rdna3_wreg((d), rdna3_reg_offset((d), HWIP_OSSSYS, 0, reg##_BASE_IDX, reg), (v))
#define RNBIO(d, reg) \
    rdna3_rreg((d), rdna3_reg_offset((d), HWIP_NBIO, 0, reg##_BASE_IDX, reg))
#define WNBIO(d, reg, v) \
    rdna3_wreg((d), rdna3_reg_offset((d), HWIP_NBIO, 0, reg##_BASE_IDX, reg), (v))

static uint32_t order_base_2(uint32_t n)
{
    uint32_t r = 0;
    while ((1u << r) < n)
        r++;
    return r;
}

/* ih_v6_0_toggle_ring_interrupts(ring 0); with ring 0 the only ring this is
 * also ih_v6_0_toggle_interrupts */
static void toggle(rdna3_dev *d, rdna3_ih *ih, int enable)
{
    uint32_t tmp = ROSS(d, regIH_RB_CNTL);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, RB_ENABLE, enable ? 1 : 0);
    if (enable) {
        /* clear a stale overflow: 0, then 1, then 0 again below */
        tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 0);
        WOSS(d, regIH_RB_CNTL, tmp);
        tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 1);
        WOSS(d, regIH_RB_CNTL, tmp);
        tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 0);
    }
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, ENABLE_INTR, enable ? 1 : 0);
    WOSS(d, regIH_RB_CNTL, tmp);
    if (!enable) {
        WOSS(d, regIH_RB_RPTR, 0);
        WOSS(d, regIH_RB_WPTR, 0);
        ih->rptr = 0;
    }
    ih->enabled = enable;
}

/* force_update_wptr_for_self_int (ring 1 threshold field is harmless with
 * ring 1 unconfigured; amdgpu writes it unconditionally) */
static void force_update_wptr_for_self_int(rdna3_dev *d, uint32_t threshold, uint32_t timeout,
                                           int enabled)
{
    uint32_t ih_cntl = ROSS(d, regIH_CNTL2);
    uint32_t ih_rb_cntl = ROSS(d, regIH_RB_CNTL_RING1);
    ih_cntl = REG_SET_FIELD(ih_cntl, IH_CNTL2, SELF_IV_FORCE_WPTR_UPDATE_TIMEOUT, timeout);
    ih_cntl = REG_SET_FIELD(ih_cntl, IH_CNTL2, SELF_IV_FORCE_WPTR_UPDATE_ENABLE, enabled);
    ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL_RING1, RB_USED_INT_THRESHOLD, threshold);
    WOSS(d, regIH_RB_CNTL_RING1, ih_rb_cntl);
    WOSS(d, regIH_CNTL2, ih_cntl);
}

/* ih_v6_0_hw_init: amdgpu_ih_ring_init + ih_v6_0_irq_init */
int ih60_init(rdna3_dev *d, rdna3_ih *ih, uint32_t ring_size, uint64_t dummy_page_pa, int msi)
{
    uint64_t ring_mc, wb_mc;
    uint32_t tmp, zero[2] = { 0, 0 };

    if (!d->ipver[HWIP_OSSSYS].present || d->ipver[HWIP_OSSSYS].major != 6 ||
        d->ipver[HWIP_OSSSYS].minor != 0) {
        RERR(d, "IH: OSSSYS %u.%u.%u is not IH 6.0", d->ipver[HWIP_OSSSYS].major,
             d->ipver[HWIP_OSSSYS].minor, d->ipver[HWIP_OSSSYS].rev);
        return RDNA3_ENODEV;
    }
    if (!ring_size)
        ring_size = RD_IH_RING_SIZE;
    if (ring_size & (ring_size - 1))
        return RDNA3_EINVAL;

    /* amdgpu_ih_ring_init */
    ih->ring_size = ring_size;
    ih->ptr_mask = ring_size - 1;
    ih->rptr = 0;
    ih->overflows = 0;
    ih->processed = 0;
    ih->msi = msi;
    ih->ring = rdna3_vram_alloc(d, ring_size, 4096, "ih-ring");
    ih->wptr_wb = rdna3_vram_alloc(d, 4096, 4096, "ih-wptr");
    if (ih->ring == UINT64_MAX || ih->wptr_wb == UINT64_MAX)
        return RDNA3_ENOMEM;
    rdna3_vram_fill(d, ih->ring, 0, ring_size);
    rdna3_vram_write(d, ih->wptr_wb, zero, sizeof(zero));
    rdna3_hdp_flush(d);
    ring_mc = rdna3_vram_mc(d, ih->ring);
    wb_mc = rdna3_vram_mc(d, ih->wptr_wb);

    /* ih_v6_0_irq_init */
    toggle(d, ih, 0);

    /* nbio_v4_3_ih_control */
    WNBIO(d, regBIF_BX0_INTERRUPT_CNTL2, (uint32_t)(dummy_page_pa >> 8));
    tmp = RNBIO(d, regBIF_BX0_INTERRUPT_CNTL);
    tmp = REG_SET_FIELD(tmp, BIF_BX0_INTERRUPT_CNTL, IH_DUMMY_RD_OVERRIDE, 0);
    /* "=1 if ring is in non-cacheable memory, e.g., vram": ours is */
    tmp = REG_SET_FIELD(tmp, BIF_BX0_INTERRUPT_CNTL, IH_REQ_NONSNOOP_EN, 1);
    WNBIO(d, regBIF_BX0_INTERRUPT_CNTL, tmp);

    /* ih_v6_0_enable_ring(ring 0) */
    WOSS(d, regIH_RB_BASE, (uint32_t)(ring_mc >> 8));
    WOSS(d, regIH_RB_BASE_HI, (uint32_t)((ring_mc >> 40) & 0xff));
    tmp = ROSS(d, regIH_RB_CNTL);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, MC_SPACE, 4);          /* !use_bus_addr */
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 1);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, WPTR_OVERFLOW_ENABLE, 1);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, RB_SIZE, order_base_2(ring_size / 4));
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, WPTR_WRITEBACK_ENABLE, 1);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, MC_SNOOP, 0);          /* VRAM: nothing to snoop */
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, MC_RO, 0);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, MC_VMID, 0);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, RPTR_REARM, msi ? 1 : 0);   /* !!adev->irq.msi_enabled */
    WOSS(d, regIH_RB_CNTL, tmp);
    WOSS(d, regIH_RB_WPTR_ADDR_LO, (uint32_t)wb_mc);
    WOSS(d, regIH_RB_WPTR_ADDR_HI, (uint32_t)(wb_mc >> 32) & 0xFFFF);
    WOSS(d, regIH_RB_WPTR, 0);
    WOSS(d, regIH_RB_RPTR, 0);
    WOSS(d, regIH_DOORBELL_RPTR, REG_SET_FIELD(0u, IH_DOORBELL_RPTR, ENABLE, 0));
    /* nbio ih_doorbell_range(use_doorbell = false) would zero RANGE_SIZE of
     * S2A_DOORBELL_ENTRY_1_CTRL, a register outside the MMIO BAR (indirect
     * PCIE access); left at its reset value until doorbells are used. */

    tmp = ROSS(d, regIH_STORM_CLIENT_LIST_CNTL);
    tmp = REG_SET_FIELD(tmp, IH_STORM_CLIENT_LIST_CNTL, CLIENT18_IS_STORM_CLIENT, 1);
    WOSS(d, regIH_STORM_CLIENT_LIST_CNTL, tmp);
    tmp = ROSS(d, regIH_INT_FLOOD_CNTL);
    tmp = REG_SET_FIELD(tmp, IH_INT_FLOOD_CNTL, FLOOD_CNTL_ENABLE, 1);
    WOSS(d, regIH_INT_FLOOD_CNTL, tmp);
    tmp = ROSS(d, regIH_MSI_STORM_CTRL);
    tmp = REG_SET_FIELD(tmp, IH_MSI_STORM_CTRL, DELAY, 3);
    WOSS(d, regIH_MSI_STORM_CTRL, tmp);

    toggle(d, ih, 1);
    force_update_wptr_for_self_int(d, 0, 8, 1);

    RLOG(d, "IH: ring %u KiB at MC 0x%llx, wptr writeback at MC 0x%llx (%s)",
         ring_size / 1024, (unsigned long long)ring_mc, (unsigned long long)wb_mc,
         msi ? "MSI" : "polled");
    return RDNA3_OK;
}

static void ih_v6_0_toggle_interrupts(rdna3_dev *d, rdna3_ih *ih, int enable);

/* ih_v6_0_hw_fini -> ih_v6_0_irq_disable */
void ih60_disable(rdna3_dev *d, rdna3_ih *ih)
{
    force_update_wptr_for_self_int(d, 0, 8, 0);
    if (ih->sys)
        ih_v6_0_toggle_interrupts(d, ih, 0);
    else
        toggle(d, ih, 0);
    /* Wait and acknowledge irq */
    d->os.udelay(d->os.ctx, 1000);
}

void ih60_decode_iv(const uint32_t dw[8], rdna3_iv *iv)
{
    iv->client_id = dw[0] & 0xff;
    iv->src_id = (dw[0] >> 8) & 0xff;
    iv->ring_id = (dw[0] >> 16) & 0xff;
    iv->vmid = (dw[0] >> 24) & 0xf;
    iv->vmid_src = dw[0] >> 31;
    iv->timestamp = dw[1] | ((uint64_t)(dw[2] & 0xffff) << 32);
    iv->timestamp_src = dw[2] >> 31;
    iv->pasid = dw[3] & 0xffff;
    iv->node_id = (dw[3] >> 16) & 0xff;
    iv->src_data[0] = dw[4];
    iv->src_data[1] = dw[5];
    iv->src_data[2] = dw[6];
    iv->src_data[3] = dw[7];
}

/* ih_v6_0_get_wptr */
static uint32_t get_wptr(rdna3_dev *d, rdna3_ih *ih)
{
    uint32_t wptr, tmp;

    rdna3_vram_read(d, ih->wptr_wb, &wptr, 4);
    if (!REG_GET_FIELD(wptr, IH_RB_WPTR, RB_OVERFLOW))
        return wptr & ih->ptr_mask;
    wptr = ROSS(d, regIH_RB_WPTR);
    if (!REG_GET_FIELD(wptr, IH_RB_WPTR, RB_OVERFLOW))
        return wptr & ih->ptr_mask;
    wptr = REG_SET_FIELD(wptr, IH_RB_WPTR, RB_OVERFLOW, 0);

    /* overflow: skip to the oldest entry not yet overwritten */
    tmp = (wptr + 32) & ih->ptr_mask;
    RERR(d, "IH: ring overflow (wptr 0x%08x, rptr 0x%08x -> 0x%08x)", wptr, ih->rptr, tmp);
    ih->rptr = tmp;
    ih->overflows++;
    tmp = ROSS(d, regIH_RB_CNTL);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 1);
    WOSS(d, regIH_RB_CNTL, tmp);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 0);
    WOSS(d, regIH_RB_CNTL, tmp);
    return wptr & ih->ptr_mask;
}

/* amdgpu_ih_process (without the restart-on-new-wptr loop's reschedule) */
static uint32_t ih60_process_amdgpu(rdna3_dev *d, rdna3_ih *ih, rdna3_iv_cb cb, void *ctx);

uint32_t ih60_process(rdna3_dev *d, rdna3_ih *ih, rdna3_iv_cb cb, void *ctx)
{
    uint32_t wptr, n = 0, dw[8], rounds = 0;
    rdna3_iv iv;

    if (!ih->enabled)
        return 0;
    if (ih->sys)
        return ih60_process_amdgpu(d, ih, cb, ctx);
    wptr = get_wptr(d, ih);
    /* amdgpu restarts while wptr keeps moving; bound it so a stuck
     * overflow flag cannot hold the caller forever */
    while (ih->rptr != wptr && rounds++ < 16) {
        while (ih->rptr != wptr) {
            rdna3_vram_read(d, ih->ring + ih->rptr, dw, sizeof(dw));
            ih60_decode_iv(dw, &iv);
            ih->rptr = (ih->rptr + RD_IH_IV_SIZE) & ih->ptr_mask;
            if (cb)
                cb(ctx, &iv);
            n++;
        }
        WOSS(d, regIH_RB_RPTR, ih->rptr);   /* ih_v6_0_set_rptr, no doorbell */
        wptr = get_wptr(d, ih);
    }
    ih->processed += n;
    return n;
}

uint32_t ih60_iv_fence(const rdna3_iv *iv)
{
    /* gfx_v11_0_eop_irq: ring_id = queue << 4 | me << 2 | pipe */
    if (iv->client_id == RD_IH_CLIENT_GRBM_CP && iv->src_id == RD_IH_SRC_CP_EOP) {
        if ((iv->ring_id & 0x7f) == 0)
            return RD_IH_FENCE_GFX;
        if ((iv->ring_id & 0x7f) == 0x04)
            return RD_IH_FENCE_COMPUTE0;
        return 0;
    }
    /* sdma_v6_0_process_trap_irq: ring_id = instance << 4 | queue */
    if (iv->client_id == RD_IH_CLIENT_GFX && iv->src_id == RD_IH_SRC_SDMA_TRAP &&
        (iv->ring_id & 0xff) == 0)
        return RD_IH_FENCE_SDMA0;
    return 0;
}

/* the user queue halves of gfx_v11_0_eop_irq and sdma_v6_0_process_fence_irq
 * (adev->enable_mes && doorbell_offset): the doorbell that names the queue's
 * fence driver; the SDMA reports it in SDMA0_QUEUE0_DOORBELL_OFFSET units */
uint32_t ih60_iv_userq_doorbell(const rdna3_iv *iv)
{
    if (iv->client_id == RD_IH_CLIENT_GRBM_CP && iv->src_id == RD_IH_SRC_CP_EOP)
        return iv->src_data[0];
    if (iv->client_id == RD_IH_CLIENT_GFX && iv->src_id == RD_IH_SRC_SDMA_FENCE)
        return iv->src_data[0] >> 2;   /* SDMA0_QUEUE0_DOORBELL_OFFSET__OFFSET__SHIFT */
    return 0;
}

/* ======================================================================
 * rdna3amdgpu=1: ih_v6_0 as amdgpu runs it on a dGPU (ring 0 + ring 1 in
 * GTT, MC_SNOOP, rptr doorbells, ring 1 page-fault redirection)
 * ====================================================================== */

/* osssys_6_0_0_offset.h: ring 1 (all BASE_IDX 0) */
#define regIH_RB_BASE_RING1        0x008d
#define regIH_RB_BASE_HI_RING1     0x008e
#define regIH_RB_RPTR_RING1        0x008f
#define regIH_RB_WPTR_RING1        0x0090
#define regIH_DOORBELL_RPTR_RING1  0x0093
#define OSSOFF(d, reg) rdna3_reg_offset((d), HWIP_OSSSYS, 0, 0, (reg))

typedef struct ih_regs {
    uint32_t rb_base, rb_base_hi, rb_cntl, rb_wptr, rb_rptr, doorbell_rptr;
    uint32_t rb_wptr_addr_lo, rb_wptr_addr_hi;
} ih_regs;

/* Ported from ih_v6_0_init_register_offset */
static void ih_v6_0_init_register_offset(rdna3_dev *d, const rdna3_ih *ih, ih_regs *r)
{
    if (!ih->ring_id) {
        r->rb_base = OSSOFF(d, regIH_RB_BASE);
        r->rb_base_hi = OSSOFF(d, regIH_RB_BASE_HI);
        r->rb_cntl = OSSOFF(d, regIH_RB_CNTL);
        r->rb_wptr = OSSOFF(d, regIH_RB_WPTR);
        r->rb_rptr = OSSOFF(d, regIH_RB_RPTR);
        r->doorbell_rptr = OSSOFF(d, regIH_DOORBELL_RPTR);
        r->rb_wptr_addr_lo = OSSOFF(d, regIH_RB_WPTR_ADDR_LO);
        r->rb_wptr_addr_hi = OSSOFF(d, regIH_RB_WPTR_ADDR_HI);
    } else {
        r->rb_base = OSSOFF(d, regIH_RB_BASE_RING1);
        r->rb_base_hi = OSSOFF(d, regIH_RB_BASE_HI_RING1);
        r->rb_cntl = OSSOFF(d, regIH_RB_CNTL_RING1);
        r->rb_wptr = OSSOFF(d, regIH_RB_WPTR_RING1);
        r->rb_rptr = OSSOFF(d, regIH_RB_RPTR_RING1);
        r->doorbell_rptr = OSSOFF(d, regIH_DOORBELL_RPTR_RING1);
        r->rb_wptr_addr_lo = r->rb_wptr_addr_hi = 0;
    }
}

/* the wptr / rptr shadows after the ring (amdgpu_ih_ring_init) */
#define WPTR_SHADOW(ih) ((ih)->sys[(ih)->ring_size / 4])
#define RPTR_SHADOW(ih) ((ih)->sys[(ih)->ring_size / 4 + 1])

/* Ported from ih_v6_0_toggle_ring_interrupts */
static void ih_v6_0_toggle_ring_interrupts(rdna3_dev *d, rdna3_ih *ih, int enable)
{
    ih_regs r;
    uint32_t tmp;
    ih_v6_0_init_register_offset(d, ih, &r);
    tmp = rdna3_rreg(d, r.rb_cntl);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, RB_ENABLE, enable ? 1 : 0);
    if (enable) {
        /* Unset the CLEAR_OVERFLOW bit to make sure the next step is
         * switching the bit from 0 to 1 */
        tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 0);
        rdna3_wreg(d, r.rb_cntl, tmp);
        /* Clear RB_OVERFLOW bit */
        tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 1);
        rdna3_wreg(d, r.rb_cntl, tmp);
        /* Unset the CLEAR_OVERFLOW bit immediately so new overflows can be
         * detected. */
        tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 0);
    }
    /* enable_intr field is only valid in ring0 */
    if (!ih->ring_id)
        tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, ENABLE_INTR, enable ? 1 : 0);
    rdna3_wreg(d, r.rb_cntl, tmp);
    if (enable) {
        ih->enabled = 1;
    } else {
        /* set rptr, wptr to 0 */
        rdna3_wreg(d, r.rb_rptr, 0);
        rdna3_wreg(d, r.rb_wptr, 0);
        ih->enabled = 0;
        ih->rptr = 0;
    }
}

/* Ported from ih_v6_0_toggle_interrupts */
static void ih_v6_0_toggle_interrupts(rdna3_dev *d, rdna3_ih *ih, int enable)
{
    ih_v6_0_toggle_ring_interrupts(d, ih, enable);
    if (ih->ih1 && ih->ih1->ring_size)
        ih_v6_0_toggle_ring_interrupts(d, ih->ih1, enable);
}

/* Ported from ih_v6_0_rb_cntl (use_bus_addr false: MC_SPACE 4) */
static uint32_t ih_v6_0_rb_cntl(const rdna3_ih *ih, uint32_t ih_rb_cntl)
{
    uint32_t rb_bufsz = order_base_2(ih->ring_size / 4);
    ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL, MC_SPACE, 4);
    ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 1);
    ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL, WPTR_OVERFLOW_ENABLE, 1);
    ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL, RB_SIZE, rb_bufsz);
    /* Ring Buffer write pointer writeback. If enabled, IH_RB_WPTR register
     * value is written to memory */
    ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL, WPTR_WRITEBACK_ENABLE, 1);
    ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL, MC_SNOOP, 1);
    ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL, MC_RO, 0);
    ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL, MC_VMID, 0);
    return ih_rb_cntl;
}

/* Ported from ih_v6_0_doorbell_rptr */
static uint32_t ih_v6_0_doorbell_rptr(const rdna3_ih *ih)
{
    uint32_t v = 0;
    if (ih->use_doorbell) {
        v = REG_SET_FIELD(v, IH_DOORBELL_RPTR, OFFSET, ih->doorbell_index);
        v = REG_SET_FIELD(v, IH_DOORBELL_RPTR, ENABLE, 1);
    } else {
        v = REG_SET_FIELD(v, IH_DOORBELL_RPTR, ENABLE, 0);
    }
    return v;
}

/* Ported from ih_v6_0_enable_ring */
static void ih_v6_0_enable_ring(rdna3_dev *d, rdna3_ih *ih)
{
    ih_regs r;
    uint32_t tmp;
    uint64_t wptr_addr = ih->gpu_addr + ih->ring_size;
    ih_v6_0_init_register_offset(d, ih, &r);
    /* Ring Buffer base. [39:8] of 40-bit address of the beginning of the ring buffer */
    rdna3_wreg(d, r.rb_base, (uint32_t)(ih->gpu_addr >> 8));
    rdna3_wreg(d, r.rb_base_hi, (uint32_t)((ih->gpu_addr >> 40) & 0xff));
    tmp = rdna3_rreg(d, r.rb_cntl);
    tmp = ih_v6_0_rb_cntl(ih, tmp);
    if (!ih->ring_id)
        tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, RPTR_REARM, ih->msi ? 1 : 0);
    if (ih->ring_id == 1) {
        tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, WPTR_OVERFLOW_ENABLE, 0);
        tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, RB_FULL_DRAIN_ENABLE, 1);
    }
    rdna3_wreg(d, r.rb_cntl, tmp);
    if (!ih->ring_id) {
        /* set the ih ring 0 writeback address whether it's enabled or not */
        rdna3_wreg(d, r.rb_wptr_addr_lo, (uint32_t)wptr_addr);
        rdna3_wreg(d, r.rb_wptr_addr_hi, (uint32_t)(wptr_addr >> 32) & 0xFFFF);
    }
    /* set rptr, wptr to 0 */
    rdna3_wreg(d, r.rb_wptr, 0);
    rdna3_wreg(d, r.rb_rptr, 0);
    rdna3_wreg(d, r.doorbell_rptr, ih_v6_0_doorbell_rptr(ih));
}

/* Ported from amdgpu_ih_ring_init (use_bus_addr false: the GTT ring) */
static void amdgpu_ih_ring_init(rdna3_ih *ih, volatile void *sys, uint64_t mc, uint32_t ring_size,
                                uint32_t ring_id, int msi)
{
    uint32_t i;
    ring_size = 4u << order_base_2(ring_size / 4);   /* Align ring size */
    ih->ring_size = ring_size;
    ih->ptr_mask = ring_size - 1;
    ih->rptr = 0;
    ih->sys = (volatile uint32_t *)sys;
    ih->gpu_addr = mc;
    ih->ring_id = ring_id;
    ih->msi = msi;
    ih->overflows = 0;
    ih->processed = 0;
    ih->ring = UINT64_MAX;
    ih->wptr_wb = UINT64_MAX;
    for (i = 0; i < ring_size / 4 + 2; i++)
        ih->sys[i] = 0;
}

int ih60_init_amdgpu(rdna3_dev *d, rdna3_ih *ih, rdna3_ih *ih1, volatile void *sys0, uint64_t mc0,
                     volatile void *sys1, uint64_t mc1, uint32_t ring_size, uint64_t dummy_page_pa,
                     int msi)
{
    uint32_t tmp;
    if (!d->ipver[HWIP_OSSSYS].present || d->ipver[HWIP_OSSSYS].major != 6 ||
        d->ipver[HWIP_OSSSYS].minor != 0)
        return RDNA3_ENODEV;
    if (!sys0 || !ring_size)
        return RDNA3_EINVAL;

    /* ih_v6_0_sw_init */
    amdgpu_ih_ring_init(ih, sys0, mc0, ring_size, 0, msi);
    ih->use_doorbell = d->doorbell_ok;
    ih->doorbell_index = RD_DB_INDEX(RD_DB_IH);
    ih->ih1 = NULL;
    ih->self_irqs = 0;
    if (ih1 && sys1) {   /* !(adev->flags & AMD_IS_APU) */
        amdgpu_ih_ring_init(ih1, sys1, mc1, ring_size, 1, msi);
        ih1->use_doorbell = d->doorbell_ok;
        ih1->doorbell_index = RD_DB_INDEX(RD_DB_IH + 1);
        ih1->ih1 = NULL;
        ih->ih1 = ih1;
    }

    /* ih_v6_0_irq_init: disable irqs */
    ih_v6_0_toggle_interrupts(d, ih, 0);
    /* nbio_v4_3_ih_control: the ring is in cacheable (snooped) memory */
    WNBIO(d, regBIF_BX0_INTERRUPT_CNTL2, (uint32_t)(dummy_page_pa >> 8));
    tmp = RNBIO(d, regBIF_BX0_INTERRUPT_CNTL);
    tmp = REG_SET_FIELD(tmp, BIF_BX0_INTERRUPT_CNTL, IH_DUMMY_RD_OVERRIDE, 0);
    tmp = REG_SET_FIELD(tmp, BIF_BX0_INTERRUPT_CNTL, IH_REQ_NONSNOOP_EN, 0);
    WNBIO(d, regBIF_BX0_INTERRUPT_CNTL, tmp);

    ih_v6_0_enable_ring(d, ih);
    if (ih->ih1)
        ih_v6_0_enable_ring(d, ih->ih1);
    /* update doorbell range for ih ring 0 */
    nbio43_ih_doorbell_range(d, ih->use_doorbell, ih->doorbell_index);

    tmp = ROSS(d, regIH_STORM_CLIENT_LIST_CNTL);
    tmp = REG_SET_FIELD(tmp, IH_STORM_CLIENT_LIST_CNTL, CLIENT18_IS_STORM_CLIENT, 1);
    WOSS(d, regIH_STORM_CLIENT_LIST_CNTL, tmp);
    tmp = ROSS(d, regIH_INT_FLOOD_CNTL);
    tmp = REG_SET_FIELD(tmp, IH_INT_FLOOD_CNTL, FLOOD_CNTL_ENABLE, 1);
    WOSS(d, regIH_INT_FLOOD_CNTL, tmp);
    tmp = ROSS(d, regIH_MSI_STORM_CTRL);
    tmp = REG_SET_FIELD(tmp, IH_MSI_STORM_CTRL, DELAY, 3);
    WOSS(d, regIH_MSI_STORM_CTRL, tmp);

    /* Redirect the interrupts to IH RB1 for dGPU */
    if (ih->ih1) {
        tmp = ROSS(d, regIH_RING1_CLIENT_CFG_INDEX);
        tmp = REG_SET_FIELD(tmp, IH_RING1_CLIENT_CFG_INDEX, INDEX, 0);
        WOSS(d, regIH_RING1_CLIENT_CFG_INDEX, tmp);
        tmp = ROSS(d, regIH_RING1_CLIENT_CFG_DATA);
        tmp = REG_SET_FIELD(tmp, IH_RING1_CLIENT_CFG_DATA, CLIENT_ID, 0xa);
        tmp = REG_SET_FIELD(tmp, IH_RING1_CLIENT_CFG_DATA, SOURCE_ID, 0x0);
        tmp = REG_SET_FIELD(tmp, IH_RING1_CLIENT_CFG_DATA, SOURCE_ID_MATCH_ENABLE, 0x1);
        WOSS(d, regIH_RING1_CLIENT_CFG_DATA, tmp);
    }

    /* enable interrupts */
    ih_v6_0_toggle_interrupts(d, ih, 1);
    /* enable wptr force update for self int */
    force_update_wptr_for_self_int(d, 0, 8, 1);
    RLOG(d, "IH: ring 0%s in system memory at MC 0x%llx, %u KiB, snooped, rptr by %s",
         ih->ih1 ? " + 1" : "", (unsigned long long)mc0, ih->ring_size / 1024,
         ih->use_doorbell ? "doorbell" : "register");
    return RDNA3_OK;
}

/* Ported from ih_v6_0_get_wptr */
static uint32_t ih_v6_0_get_wptr(rdna3_dev *d, rdna3_ih *ih)
{
    ih_regs r;
    uint32_t wptr, tmp;
    wptr = WPTR_SHADOW(ih);
    if (!REG_GET_FIELD(wptr, IH_RB_WPTR, RB_OVERFLOW))
        goto out;
    ih_v6_0_init_register_offset(d, ih, &r);
    wptr = rdna3_rreg(d, r.rb_wptr);
    if (!REG_GET_FIELD(wptr, IH_RB_WPTR, RB_OVERFLOW))
        goto out;
    wptr = REG_SET_FIELD(wptr, IH_RB_WPTR, RB_OVERFLOW, 0);
    /* When a ring buffer overflow happen start parsing interrupt from the
     * last not overwritten vector (wptr + 32). */
    tmp = (wptr + 32) & ih->ptr_mask;
    RERR(d, "IH ring buffer overflow (0x%08X, 0x%08X, 0x%08X)", wptr, ih->rptr, tmp);
    ih->rptr = tmp;
    ih->overflows++;
    tmp = rdna3_rreg(d, r.rb_cntl);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 1);
    rdna3_wreg(d, r.rb_cntl, tmp);
    /* Unset the CLEAR_OVERFLOW bit immediately so new overflows can be
     * detected. */
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 0);
    rdna3_wreg(d, r.rb_cntl, tmp);
out:
    return wptr & ih->ptr_mask;
}

/* Ported from ih_v6_0_set_rptr */
static void ih_v6_0_set_rptr(rdna3_dev *d, rdna3_ih *ih)
{
    if (ih->use_doorbell) {
        RPTR_SHADOW(ih) = ih->rptr;
        rdna3_doorbell_wr32(d, ih->doorbell_index, ih->rptr);
    } else {
        ih_regs r;
        ih_v6_0_init_register_offset(d, ih, &r);
        rdna3_wreg(d, r.rb_rptr, ih->rptr);
    }
}

/* Ported from ih_v6_0_self_irq: ring 1's wptr arrives in an IV on ring 0 */
static int ih_v6_0_self_irq(rdna3_ih *ih, const rdna3_iv *entry)
{
    switch (entry->ring_id) {
    case 1:
        if (ih->ih1) {
            WPTR_SHADOW(ih->ih1) = entry->src_data[0];
            ih->self_irqs++;
            return 1;   /* schedule_work(&adev->irq.ih1_work) */
        }
        break;
    default:
        break;
    }
    return 0;
}

/* amdgpu_ih_process for one ring; '*ring1' is set when a self IV moved
 * ring 1's wptr */
static uint32_t process_ring(rdna3_dev *d, rdna3_ih *ih, rdna3_iv_cb cb, void *ctx, int *ring1)
{
    uint32_t wptr, n = 0, dw[8], i, rounds = 0;
    rdna3_iv iv;
    if (!ih->enabled)
        return 0;
    wptr = ih_v6_0_get_wptr(d, ih);
    while (ih->rptr != wptr && rounds++ < 16) {
        while (ih->rptr != wptr) {
            for (i = 0; i < 8; i++)
                dw[i] = ih->sys[ih->rptr / 4 + i];
            ih60_decode_iv(dw, &iv);
            ih->rptr = (ih->rptr + RD_IH_IV_SIZE) & ih->ptr_mask;
            n++;
            /* amdgpu_irq_dispatch: SOC21_IH_CLIENTID_IH source 0 is the self IV */
            if (iv.client_id == RD_IH_CLIENT_IH && iv.src_id == 0 && ring1 && ih_v6_0_self_irq(ih, &iv)) {
                *ring1 = 1;
                continue;
            }
            if (cb)
                cb(ctx, &iv);
        }
        ih_v6_0_set_rptr(d, ih);
        wptr = ih_v6_0_get_wptr(d, ih);
    }
    ih->processed += n;
    return n;
}

static uint32_t ih60_process_amdgpu(rdna3_dev *d, rdna3_ih *ih, rdna3_iv_cb cb, void *ctx)
{
    int ring1 = 0;
    uint32_t n = process_ring(d, ih, cb, ctx, &ring1);
    /* amdgpu_irq_handle_ih1 */
    if (ih->ih1 && (ring1 || ih_v6_0_get_wptr(d, ih->ih1) != ih->ih1->rptr))
        n += process_ring(d, ih->ih1, cb, ctx, NULL);
    return n;
}
