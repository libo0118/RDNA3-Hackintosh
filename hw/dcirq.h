/*
 * dcirq.h - the Display Core interrupt sources of DCN 3.2 that drive the
 * vblank and the page-flip completion (DRIVER-PLAN N42), ported from Linux
 * amdgpu (MIT):
 *
 *   irq/dcn32/irq_service_dcn32.c  to_dal_irq_source_dcn32 (IH src_id ->
 *                                  DC_IRQ_SOURCE_*), irq_source_info_dcn32
 *                                  (vblank_int_entry, vupdate_no_lock_int_entry,
 *                                  pflip_int_entry: the enable / ack bits)
 *   irq/irq_service.c              dal_irq_service_set (ack, then
 *                                  dal_irq_service_set_generic),
 *                                  dal_irq_service_ack_generic
 *   irq/irq_service.c              dal_irq_service_ack, dal_irq_service_to_irq_source
 *   core/dc.c                      dc_interrupt_set, dc_interrupt_ack,
 *                                  dc_interrupt_to_irq_source
 *   amdgpu_dm/amdgpu_dm.c          dcn10_register_irq_handlers: the src_ids
 *                                  registered on SOC15_IH_CLIENTID_DCE
 *   include/ivsrcid/dcn/irqsrcs_dcn_1_0.h  the src_id numbers
 *
 * DC_IRQ_SOURCE_VBLANKn is the OTG's VSTARTUP event (OTG_GLOBAL_SYNC_STATUS
 * VSTARTUP_INT_EN / VSTARTUP_EVENT_CLEAR), VUPDATEn its VUPDATE_NO_LOCK
 * event (same register), PFLIPn the HUBP's flip interrupt
 * (HUBPREQ DCSURF_SURFACE_FLIP_INTERRUPT SURFACE_FLIP_INT_MASK /
 * SURFACE_FLIP_CLEAR). The DCN 3.2 table has instances 0..3 of each (OTG0..3,
 * HUBP0..3); amdgpu indexes PFLIPn by the CRTC's otg_inst, the register is
 * the HUBP's (pipe n drives OTG n in its resource map) - here the caller
 * names the HUBP (the head's pipe) for a PFLIP source and the OTG for the
 * other two.
 *
 * The HPD sources share the table (DC_HPD1_INT with the pin in the context
 * id); their enable / ack stay in dcaux.c (dchpd_irq_set / dchpd_irq_ack,
 * hpd_ack's polarity flip), dcirq_to_irq_source only decodes them.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCIRQ_H
#define RDNA3_DCIRQ_H

#include "rdna3_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* irqsrcs_dcn_1_0.h, on IH client SOC15_IH_CLIENTID_DCE */
#define RD_IH_CLIENT_DCE_DCIRQ           0x04   /* = RD_IH_CLIENT_DCE (dcaux.h) */
#define RD_IH_SRC_DC_D1_OTG_VSTARTUP     0x3C   /* DCN_1_0__SRCID__DC_D1_OTG_VSTARTUP .. D6 = 0x41 */
#define RD_IH_SRC_HUBP0_FLIP_INTERRUPT   0x4F   /* DCN_1_0__SRCID__HUBP0_FLIP_INTERRUPT .. HUBP5 = 0x54 */
#define RD_IH_SRC_OTG0_V_UPDATE_NO_LOCK  0x57   /* DCN_1_0__SRCID__OTG0_IHC_V_UPDATE_NO_LOCK_INTERRUPT .. OTG5 = 0x5C */
#define RD_IH_SRC_DC_HPD1_INT_DCN        9      /* DCN_1_0__SRCID__DC_HPD1_INT (every HPD / HPD RX) */
#define RD_DCIRQ_SRC_SPAN                6      /* D1..D6, HUBP0..5, OTG0..5 in the src_id table */
#define RD_DCIRQ_INSTS                   4      /* instances irq_source_info_dcn32 programs (0..3) */

/* the DC_IRQ_SOURCE_* kinds this driver uses (irq_types.h enum dc_irq_source) */
enum {
    RD_DCIRQ_NONE    = 0,   /* DC_IRQ_SOURCE_INVALID */
    RD_DCIRQ_VBLANK  = 1,   /* DC_IRQ_SOURCE_VBLANK1 + n: OTG n VSTARTUP */
    RD_DCIRQ_VUPDATE = 2,   /* DC_IRQ_SOURCE_VUPDATE1 + n: OTG n VUPDATE_NO_LOCK */
    RD_DCIRQ_PFLIP   = 3,   /* DC_IRQ_SOURCE_PFLIP1 + n: HUBP n flip */
    RD_DCIRQ_HPD     = 4,   /* DC_IRQ_SOURCE_HPD1 + n */
    RD_DCIRQ_HPDRX   = 5,   /* DC_IRQ_SOURCE_HPD1RX + n (DP short pulse) */
};
typedef struct rdna3_dc_irq_source {
    uint32_t kind;          /* RD_DCIRQ_* */
    uint32_t inst;          /* OTG (VBLANK, VUPDATE), HUBP (PFLIP) or HPD pin */
} rdna3_dc_irq_source;

/* dc_interrupt_to_irq_source -> to_dal_irq_source_dcn32 for an IV of
 * client 'client_id': 1 and *s filled for a DCN source this driver knows
 * (VBLANK1..6, VUPDATE1..6, PFLIP1..6, HPD1..6, HPD1RX..6RX), 0 otherwise
 * (*s = NONE). Instances beyond RD_DCIRQ_INSTS decode (the IH can raise
 * them) but dcirq_set / dcirq_ack refuse them, as the dummy entries of
 * irq_source_info_dcn32 do. */
int  dcirq_to_irq_source(uint32_t client_id, uint32_t src_id, uint32_t ext_id, rdna3_dc_irq_source *s);

/* dc_interrupt_set -> dal_irq_service_set: the source acked first, then its
 * enable bit set or cleared (dal_irq_service_set_generic). VBLANK / VUPDATE
 * / PFLIP of instances 0..3 only (RDNA3_EINVAL otherwise, RDNA3_ENODEV
 * without DCN). */
int  dcirq_set(rdna3_dev *d, rdna3_dc_irq_source s, int enable);
/* dc_interrupt_ack -> dal_irq_service_ack_generic: the CLEAR bit written */
int  dcirq_ack(rdna3_dev *d, rdna3_dc_irq_source s);
/* the source's enable bit (1 / 0), -1 for a source dcirq_set refuses or an
 * unreadable register (all-ones: the device gone or in D3) */
int  dcirq_enabled(rdna3_dev *d, rdna3_dc_irq_source s);
/* the source's INT_STATUS (1 = raised and not acked), -1 as above */
int  dcirq_pending(rdna3_dev *d, rdna3_dc_irq_source s);

const char *dcirq_kind_name(uint32_t kind);

#ifdef __cplusplus
}
#endif
#endif
