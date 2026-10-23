/*
 * dcirq.c - see dcirq.h. Port of to_dal_irq_source_dcn32 and the
 * vblank / vupdate_no_lock / pflip entries of irq_source_info_dcn32
 * (irq_service_dcn32.c), dal_irq_service_set / _set_generic / _ack_generic
 * (irq_service.c), dc_interrupt_set / dc_interrupt_ack (dc.c).
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcirq.h"
#include "gen/dcn_3_2_0_irq.h"

#define OTG_STRIDE  (regOTG1_OTG_GLOBAL_SYNC_STATUS - regOTG0_OTG_GLOBAL_SYNC_STATUS)
#define HUBP_STRIDE (regHUBPREQ1_DCSURF_SURFACE_FLIP_INTERRUPT - regHUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT)

/* every instance is at the same stride (irq_source_info_dcn32 names each
 * one; the table here computes them) */
typedef char rd_dcirq_otg_stride[(regOTG3_OTG_GLOBAL_SYNC_STATUS - regOTG2_OTG_GLOBAL_SYNC_STATUS == OTG_STRIDE &&
                                  regOTG2_OTG_GLOBAL_SYNC_STATUS - regOTG1_OTG_GLOBAL_SYNC_STATUS == OTG_STRIDE) ? 1 : -1];
typedef char rd_dcirq_hubp_stride[(regHUBPREQ3_DCSURF_SURFACE_FLIP_INTERRUPT -
                                   regHUBPREQ2_DCSURF_SURFACE_FLIP_INTERRUPT == HUBP_STRIDE &&
                                   regHUBPREQ2_DCSURF_SURFACE_FLIP_INTERRUPT -
                                   regHUBPREQ1_DCSURF_SURFACE_FLIP_INTERRUPT == HUBP_STRIDE) ? 1 : -1];

/* struct irq_source_info: enable_reg / enable_mask, ack_reg / ack_mask
 * (IRQ_REG_ENTRY: the enable value is the mask, the ack value is the mask) */
typedef struct dcirq_info {
    uint32_t reg;           /* enable_reg == ack_reg for all three kinds */
    uint32_t enable_mask;
    uint32_t ack_mask;
    uint32_t status_mask;   /* the INT_STATUS bit (for dcirq_pending) */
} dcirq_info;

static int find_irq_source_info(rdna3_dev *d, rdna3_dc_irq_source s, dcirq_info *in)
{
    if (!d->ipver[HWIP_DCE].present)
        return RDNA3_ENODEV;
    if (s.inst >= RD_DCIRQ_INSTS)
        return RDNA3_EINVAL;   /* dummy_irq_entry() in irq_source_info_dcn32 */
    switch (s.kind) {
    case RD_DCIRQ_VBLANK:      /* vblank_int_entry */
        in->reg = rdna3_reg_offset(d, HWIP_DCE, 0, regOTG0_OTG_GLOBAL_SYNC_STATUS_BASE_IDX,
                                   regOTG0_OTG_GLOBAL_SYNC_STATUS) + (uint32_t)OTG_STRIDE * s.inst;
        in->enable_mask = OTG0_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_INT_EN_MASK;
        in->ack_mask = OTG0_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_EVENT_CLEAR_MASK;
        in->status_mask = OTG0_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_INT_STATUS_MASK;
        return RDNA3_OK;
    case RD_DCIRQ_VUPDATE:     /* vupdate_no_lock_int_entry */
        in->reg = rdna3_reg_offset(d, HWIP_DCE, 0, regOTG0_OTG_GLOBAL_SYNC_STATUS_BASE_IDX,
                                   regOTG0_OTG_GLOBAL_SYNC_STATUS) + (uint32_t)OTG_STRIDE * s.inst;
        in->enable_mask = OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_INT_EN_MASK;
        in->ack_mask = OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_EVENT_CLEAR_MASK;
        in->status_mask = OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_INT_STATUS_MASK;
        return RDNA3_OK;
    case RD_DCIRQ_PFLIP:       /* pflip_int_entry */
        in->reg = rdna3_reg_offset(d, HWIP_DCE, 0, regHUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT_BASE_IDX,
                                   regHUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT) + (uint32_t)HUBP_STRIDE * s.inst;
        in->enable_mask = HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_INT_MASK_MASK;
        in->ack_mask = HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_CLEAR_MASK;
        in->status_mask = HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_INT_STATUS_MASK;
        return RDNA3_OK;
    default:
        return RDNA3_EINVAL;   /* HPD: dcaux.c; the rest: not ported */
    }
}

int dcirq_to_irq_source(uint32_t client_id, uint32_t src_id, uint32_t ext_id, rdna3_dc_irq_source *s)
{
    rdna3_dc_irq_source r = { RD_DCIRQ_NONE, 0 };
    /* dcn10_register_irq_handlers: every DC source on SOC15_IH_CLIENTID_DCE */
    if (client_id == RD_IH_CLIENT_DCE_DCIRQ) {
        if (src_id >= RD_IH_SRC_DC_D1_OTG_VSTARTUP && src_id < RD_IH_SRC_DC_D1_OTG_VSTARTUP + RD_DCIRQ_SRC_SPAN) {
            r.kind = RD_DCIRQ_VBLANK;
            r.inst = src_id - RD_IH_SRC_DC_D1_OTG_VSTARTUP;
        } else if (src_id >= RD_IH_SRC_HUBP0_FLIP_INTERRUPT &&
                   src_id < RD_IH_SRC_HUBP0_FLIP_INTERRUPT + RD_DCIRQ_SRC_SPAN) {
            r.kind = RD_DCIRQ_PFLIP;
            r.inst = src_id - RD_IH_SRC_HUBP0_FLIP_INTERRUPT;
        } else if (src_id >= RD_IH_SRC_OTG0_V_UPDATE_NO_LOCK &&
                   src_id < RD_IH_SRC_OTG0_V_UPDATE_NO_LOCK + RD_DCIRQ_SRC_SPAN) {
            r.kind = RD_DCIRQ_VUPDATE;
            r.inst = src_id - RD_IH_SRC_OTG0_V_UPDATE_NO_LOCK;
        } else if (src_id == RD_IH_SRC_DC_HPD1_INT_DCN && ext_id < 2 * RD_DCIRQ_SRC_SPAN) {
            /* DCN_1_0__CTXID__DC_HPDn_INT = n - 1, DC_HPDn_RX_INT = n + 5 */
            r.kind = ext_id < RD_DCIRQ_SRC_SPAN ? RD_DCIRQ_HPD : RD_DCIRQ_HPDRX;
            r.inst = ext_id % RD_DCIRQ_SRC_SPAN;
        }
    }
    if (s)
        *s = r;
    return r.kind != RD_DCIRQ_NONE;
}

int dcirq_ack(rdna3_dev *d, rdna3_dc_irq_source s)
{
    dcirq_info in;
    uint32_t v;
    int r = find_irq_source_info(d, s, &in);
    if (r)
        return r;
    /* dal_irq_service_ack_generic: value = (value & ~ack_mask) | ack_value */
    v = rdna3_rreg(d, in.reg);
    if (v == ~0u)
        return RDNA3_ENODEV;
    rdna3_wreg(d, in.reg, (v & ~in.ack_mask) | in.ack_mask);
    return RDNA3_OK;
}

int dcirq_set(rdna3_dev *d, rdna3_dc_irq_source s, int enable)
{
    dcirq_info in;
    uint32_t v;
    int r = find_irq_source_info(d, s, &in);
    if (r)
        return r;
    /* dal_irq_service_set: dal_irq_service_ack first (no .set / .ack of
     * their own for these three: the generic ones) */
    r = dcirq_ack(d, s);
    if (r)
        return r;
    /* dal_irq_service_set_generic: enable_value[enable ? 0 : 1] =
     * { mask, ~mask } under enable_mask */
    v = rdna3_rreg(d, in.reg);
    if (v == ~0u)
        return RDNA3_ENODEV;
    v = (v & ~in.enable_mask) | ((enable ? in.enable_mask : ~in.enable_mask) & in.enable_mask);
    rdna3_wreg(d, in.reg, v);
    return RDNA3_OK;
}

int dcirq_enabled(rdna3_dev *d, rdna3_dc_irq_source s)
{
    dcirq_info in;
    uint32_t v;
    if (find_irq_source_info(d, s, &in))
        return -1;
    v = rdna3_rreg(d, in.reg);
    if (v == ~0u)
        return -1;
    return (v & in.enable_mask) ? 1 : 0;
}

int dcirq_pending(rdna3_dev *d, rdna3_dc_irq_source s)
{
    dcirq_info in;
    uint32_t v;
    if (find_irq_source_info(d, s, &in))
        return -1;
    v = rdna3_rreg(d, in.reg);
    if (v == ~0u)
        return -1;
    return (v & in.status_mask) ? 1 : 0;
}

const char *dcirq_kind_name(uint32_t kind)
{
    switch (kind) {
    case RD_DCIRQ_VBLANK:  return "VBLANK";
    case RD_DCIRQ_VUPDATE: return "VUPDATE";
    case RD_DCIRQ_PFLIP:   return "PFLIP";
    case RD_DCIRQ_HPD:     return "HPD";
    case RD_DCIRQ_HPDRX:   return "HPDRX";
    default:               return "INVALID";
    }
}
