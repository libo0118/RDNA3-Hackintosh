/*
 * ih60.c - see ih60.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "ih60.h"
#include "gen/osssys_6_0_0.h"
#include "gen/nbio_4_3_0_ih.h"

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

/* ih_v6_0_hw_fini -> ih_v6_0_irq_disable */
void ih60_disable(rdna3_dev *d, rdna3_ih *ih)
{
    force_update_wptr_for_self_int(d, 0, 8, 0);
    toggle(d, ih, 0);
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
uint32_t ih60_process(rdna3_dev *d, rdna3_ih *ih, rdna3_iv_cb cb, void *ctx)
{
    uint32_t wptr, n = 0, dw[8], rounds = 0;
    rdna3_iv iv;

    if (!ih->enabled)
        return 0;
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
