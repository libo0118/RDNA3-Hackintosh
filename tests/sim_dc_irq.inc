/*
 * sim_dc_irq.inc - included by sim_navi32.c before sim_wr: the model of the
 * DCN interrupt sources DRIVER-PLAN N42 ports (hw/dcirq.c: the OTG VSTARTUP
 * = DC_IRQ_SOURCE_VBLANKn and VUPDATE_NO_LOCK = VUPDATEn events of
 * OTG_GLOBAL_SYNC_STATUS, the HUBP flip interrupt = PFLIPn of
 * DCSURF_SURFACE_FLIP_INTERRUPT) and their test (test_dc_irq, called from
 * run() while the IH ring is up).
 *
 * The hardware as the register spec has it: an event sets its OCCURRED
 * status; with its enable set, the INT_STATUS rises and the IH gets one IV
 * (client DCE, src_id from irqsrcs_dcn_1_0.h) - a level source: no second
 * IV until the CLEAR strobe drops INT_STATUS (dal_irq_service_ack_generic).
 * The CLEAR bits read 0; OCCURRED / INT_STATUS are read-only.
 *
 * The model only acts while the test turns it on (SQ.model): the older
 * tests keep the plain register file.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcirq.h"
#include "gen/dcn_3_2_0_irq.h"
#include "rdna3_vbl_irq.h"

#define SQ_GSS(o)   OTG(regOTG0_OTG_GLOBAL_SYNC_STATUS, o)
#define SQ_FLIPI(p) HUBP(regHUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT, p)
#define SQ_FRAME_US 16667u

static struct {
    int model;
    int broken_level;           /* a broken source: an IV at every event, latched or not (the storm) */
    unsigned ivs;               /* DCN IVs the model raised */
} SQ;

/* OTG o starts a frame: VSTARTUP, then (later in the frame) VUPDATE */
static void sq_otg_event(uint32_t o)
{
    uint32_t *r = &S.reg[SQ_GSS(o)];
    if (!SQ.model || !(S.reg[OTG(regOTG0_OTG_CONTROL, o)] & OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK))
        return;
    *r |= OTG0_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_EVENT_OCCURRED_MASK |
          OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_EVENT_OCCURRED_MASK;
    if ((*r & OTG0_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_INT_EN_MASK) &&
        (SQ.broken_level || !(*r & OTG0_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_INT_STATUS_MASK))) {
        *r |= OTG0_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_INT_STATUS_MASK;
        SQ.ivs++;
        ih_push_iv(RD_IH_CLIENT_DCE, RD_IH_SRC_DC_D1_OTG_VSTARTUP + o, 0, 0, 0, 0, 0);
    }
    if ((*r & OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_INT_EN_MASK) &&
        (SQ.broken_level || !(*r & OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_INT_STATUS_MASK))) {
        *r |= OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_INT_STATUS_MASK;
        SQ.ivs++;
        ih_push_iv(RD_IH_CLIENT_DCE, RD_IH_SRC_OTG0_V_UPDATE_NO_LOCK + o, 0, 0, 0, 0, 0);
    }
}

/* HUBP p's flip latched (the new surface is scanned) */
static void sq_flip_event(uint32_t p)
{
    uint32_t *r = &S.reg[SQ_FLIPI(p)];
    if (!SQ.model)
        return;
    *r |= HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_OCCURRED_MASK;
    if ((*r & HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_INT_MASK_MASK) &&
        !(*r & HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_INT_STATUS_MASK)) {
        *r |= HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_INT_STATUS_MASK;
        SQ.ivs++;
        ih_push_iv(RD_IH_CLIENT_DCE, RD_IH_SRC_HUBP0_FLIP_INTERRUPT + p, 0, 0, 0, 0, 0);
    }
}

/* one frame of OTG o: the counter moves, the flips pending on its pipes
 * latch (hubp2_is_flip_pending goes 0), then the OTG's events */
static void sq_frame(uint32_t o)
{
    uint32_t p;
    S.now_us += SQ_FRAME_US;
    if (!(S.reg[OTG(regOTG0_OTG_CONTROL, o)] & OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK))
        return;
    S.reg[OTG(regOTG0_OTG_STATUS_FRAME_COUNT, o)] =
        (S.reg[OTG(regOTG0_OTG_STATUS_FRAME_COUNT, o)] + 1) & OTG0_OTG_STATUS_FRAME_COUNT__OTG_FRAME_COUNT_MASK;
    for (p = 0; p < RD_DCN_MAX_PIPES; p++) {
        uint32_t *fc = &S.reg[HUBP(regHUBPREQ0_DCSURF_FLIP_CONTROL, p)];
        uint32_t cntl = S.reg[HUBP(regHUBP0_DCHUBP_CNTL, p)];
        if (((cntl & HUBP0_DCHUBP_CNTL__HUBP_VTG_SEL_MASK) >> HUBP0_DCHUBP_CNTL__HUBP_VTG_SEL__SHIFT) != o ||
            !(*fc & HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK))
            continue;
        *fc &= ~HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK;
        S.reg[HUBP(regHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE, p)] =
            S.reg[HUBP(regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, p)];
        sq_flip_event(p);
    }
    sq_otg_event(o);
}

/* the registers' write side: CLEAR strobes, read-only status kept */
static int sq_wr(uint32_t dw, uint32_t v)
{
    uint32_t n;
    if (!SQ.model)
        return 0;
    for (n = 0; n < RD_DCN_MAX_PIPES; n++) {
        if (dw == SQ_GSS(n)) {
            const uint32_t ro = OTG0_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_EVENT_OCCURRED_MASK |
                                OTG0_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_INT_STATUS_MASK |
                                OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_EVENT_OCCURRED_MASK |
                                OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_INT_STATUS_MASK |
                                OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_STATUS_MASK |
                                OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_EVENT_OCCURRED_MASK |
                                OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_INT_STATUS_MASK |
                                OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_STATUS_MASK |
                                OTG0_OTG_GLOBAL_SYNC_STATUS__VREADY_EVENT_OCCURRED_MASK |
                                OTG0_OTG_GLOBAL_SYNC_STATUS__VREADY_INT_STATUS_MASK;
            const uint32_t strobe = OTG0_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_EVENT_CLEAR_MASK |
                                    OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_EVENT_CLEAR_MASK |
                                    OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_EVENT_CLEAR_MASK |
                                    OTG0_OTG_GLOBAL_SYNC_STATUS__VREADY_EVENT_CLEAR_MASK;
            uint32_t st = S.reg[dw] & ro;
            if (v & OTG0_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_EVENT_CLEAR_MASK)
                st &= ~(OTG0_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_EVENT_OCCURRED_MASK |
                        OTG0_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_INT_STATUS_MASK);
            if (v & OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_EVENT_CLEAR_MASK)
                st &= ~(OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_EVENT_OCCURRED_MASK |
                        OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_INT_STATUS_MASK);
            S.reg[dw] = st | (v & ~ro & ~strobe);
            return 1;
        }
        if (dw == SQ_FLIPI(n)) {
            const uint32_t ro = HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_OCCURRED_MASK |
                                HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_INT_STATUS_MASK |
                                HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_AWAY_OCCURRED_MASK |
                                HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_AWAY_INT_STATUS_MASK;
            const uint32_t strobe = HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_CLEAR_MASK |
                                    HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_AWAY_CLEAR_MASK;
            uint32_t st = S.reg[dw] & ro;
            if (v & HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_CLEAR_MASK)
                st &= ~(HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_OCCURRED_MASK |
                        HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_INT_STATUS_MASK);
            S.reg[dw] = st | (v & ~ro & ~strobe);
            return 1;
        }
    }
    return 0;
}

/* the core's handler as kext/src/RDNA3DisplayIrq.cpp has it, modelled on
 * amdgpu_dm_irq_handler: dc_interrupt_to_irq_source, dc_interrupt_ack,
 * then the high-irq work (dm_crtc_high_irq / dm_pflip_high_irq: here a
 * count per instance) and the storm guard */
struct sq_seen {
    rdna3_dev *d;
    int no_ack;                           /* the handler "forgets" the ack */
    unsigned vbl[RD_DCIRQ_INSTS], vup[RD_DCIRQ_INSTS], pfl[RD_DCIRQ_INSTS], other, storms, hpd;
    struct rd3_virq_storm st[3][RD_DCIRQ_INSTS];
};
static void sq_iv_cb(void *ctx, const rdna3_iv *iv)
{
    struct sq_seen *s = (struct sq_seen *)ctx;
    rdna3_dc_irq_source src;
    if (!dcirq_to_irq_source(iv->client_id, iv->src_id, iv->src_data[0], &src) || src.inst >= RD_DCIRQ_INSTS) {
        s->other++;
        return;
    }
    if (src.kind == RD_DCIRQ_HPD || src.kind == RD_DCIRQ_HPDRX) {
        s->hpd++;
        return;
    }
    if (!s->no_ack)
        CHECK(dcirq_ack(s->d, src) == RDNA3_OK);
    if (src.kind == RD_DCIRQ_VBLANK)
        s->vbl[src.inst]++;
    else if (src.kind == RD_DCIRQ_VUPDATE)
        s->vup[src.inst]++;
    else
        s->pfl[src.inst]++;
    if (rd3_virq_storm_note(&s->st[src.kind - 1][src.inst], S.now_us * 1000ull)) {
        s->storms++;
        CHECK(dcirq_set(s->d, src, 0) == RDNA3_OK);
    }
}

static void test_dc_irq(rdna3_dev *dev, rdna3_ih *ih)
{
    /* what the test touches, put back at the end */
    uint32_t save_otg[RD_DCN_MAX_PIPES], save_hubp[RD_DCN_MAX_PIPES], save_fc[RD_DCN_MAX_PIPES];
    uint32_t save_frame[RD_DCN_MAX_PIPES], save_addr[RD_DCN_MAX_PIPES], save_inuse[RD_DCN_MAX_PIPES];
    const int hold0 = S.dcn_hold;
    const unsigned flips0 = S.dcn_flips;
    struct sq_seen s;
    rdna3_dc_irq_source src;
    const rdna3_dc_irq_source vbl0 = { RD_DCIRQ_VBLANK, 0 }, vbl2 = { RD_DCIRQ_VBLANK, 2 };
    const rdna3_dc_irq_source pfl0 = { RD_DCIRQ_PFLIP, 0 }, pfl1 = { RD_DCIRQ_PFLIP, 1 };
    const rdna3_dc_irq_source vup0 = { RD_DCIRQ_VUPDATE, 0 };
    uint32_t i;

    for (i = 0; i < RD_DCN_MAX_PIPES; i++) {
        save_otg[i] = S.reg[OTG(regOTG0_OTG_CONTROL, i)];
        save_hubp[i] = S.reg[HUBP(regHUBP0_DCHUBP_CNTL, i)];
        save_fc[i] = S.reg[HUBP(regHUBPREQ0_DCSURF_FLIP_CONTROL, i)];
        save_frame[i] = S.reg[OTG(regOTG0_OTG_STATUS_FRAME_COUNT, i)];
        save_addr[i] = S.reg[HUBP(regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, i)];
        save_inuse[i] = S.reg[HUBP(regHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE, i)];
    }
    memset(&s, 0, sizeof(s));
    s.d = dev;
    ih60_process(dev, ih, NULL, NULL);

    /* the decode: to_dal_irq_source_dcn32 on client DCE only */
    CHECK(dcirq_to_irq_source(RD_IH_CLIENT_DCE, 0x3C, 0, &src) && src.kind == RD_DCIRQ_VBLANK && src.inst == 0);
    CHECK(dcirq_to_irq_source(RD_IH_CLIENT_DCE, 0x41, 0, &src) && src.kind == RD_DCIRQ_VBLANK && src.inst == 5);
    CHECK(dcirq_to_irq_source(RD_IH_CLIENT_DCE, 0x4F, 0, &src) && src.kind == RD_DCIRQ_PFLIP && src.inst == 0);
    CHECK(dcirq_to_irq_source(RD_IH_CLIENT_DCE, 0x52, 0, &src) && src.kind == RD_DCIRQ_PFLIP && src.inst == 3);
    CHECK(dcirq_to_irq_source(RD_IH_CLIENT_DCE, 0x57, 0, &src) && src.kind == RD_DCIRQ_VUPDATE && src.inst == 0);
    CHECK(dcirq_to_irq_source(RD_IH_CLIENT_DCE, 0x5C, 0, &src) && src.kind == RD_DCIRQ_VUPDATE && src.inst == 5);
    CHECK(dcirq_to_irq_source(RD_IH_CLIENT_DCE, 9, 4, &src) && src.kind == RD_DCIRQ_HPD && src.inst == 4);
    CHECK(dcirq_to_irq_source(RD_IH_CLIENT_DCE, 9, 7, &src) && src.kind == RD_DCIRQ_HPDRX && src.inst == 1);
    CHECK(!dcirq_to_irq_source(RD_IH_CLIENT_DCE, 9, 12, &src) && src.kind == RD_DCIRQ_NONE);
    CHECK(!dcirq_to_irq_source(RD_IH_CLIENT_DCE, 0x2A, 0, &src));   /* DPP6 perfmon: not ours */
    CHECK(!dcirq_to_irq_source(RD_IH_CLIENT_GFX, 0x3C, 0, &src));
    CHECK(!dcirq_to_irq_source(RD_IH_CLIENT_DCE, 0x55, 0, &src));
    /* the HPD source of dcaux.c is the same as irq_service_dcn32's */
    CHECK(RD_IH_SRC_DC_HPD1_INT == RD_IH_SRC_DC_HPD1_INT_DCN);
    /* the dummy entries and the HPD ones (dcaux.c) are refused */
    src.kind = RD_DCIRQ_VBLANK;
    src.inst = 4;
    CHECK(dcirq_set(dev, src, 1) == RDNA3_EINVAL && dcirq_enabled(dev, src) == -1);
    src.kind = RD_DCIRQ_HPD;
    src.inst = 0;
    CHECK(dcirq_set(dev, src, 1) == RDNA3_EINVAL && dcirq_ack(dev, src) == RDNA3_EINVAL);

    /* two heads: HUBP 0 on OTG 0, HUBP 1 on OTG 2 */
    SQ.model = 1;
    S.reg[OTG(regOTG0_OTG_CONTROL, 0)] |= OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK;
    S.reg[OTG(regOTG0_OTG_CONTROL, 2)] |= OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK;
    S.reg[OTG(regOTG0_OTG_CONTROL, 1)] &= ~OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK;
    S.reg[HUBP(regHUBP0_DCHUBP_CNTL, 0)] = 0u << HUBP0_DCHUBP_CNTL__HUBP_VTG_SEL__SHIFT;
    S.reg[HUBP(regHUBP0_DCHUBP_CNTL, 1)] = 2u << HUBP0_DCHUBP_CNTL__HUBP_VTG_SEL__SHIFT;
    S.reg[SQ_GSS(0)] = S.reg[SQ_GSS(2)] = 0;
    S.reg[SQ_FLIPI(0)] = S.reg[SQ_FLIPI(1)] = 0;

    /* disabled sources are silent: the events latch their status only */
    sq_frame(0);
    sq_frame(2);
    CHECK(ih60_process(dev, ih, sq_iv_cb, &s) == 0 && SQ.ivs == 0);
    CHECK(S.reg[SQ_GSS(0)] & OTG0_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_EVENT_OCCURRED_MASK);

    /* dc_interrupt_set: acked first (the old event raises nothing), then enabled */
    CHECK(dcirq_set(dev, vbl0, 1) == RDNA3_OK && dcirq_set(dev, vbl2, 1) == RDNA3_OK);
    CHECK(dcirq_set(dev, pfl0, 1) == RDNA3_OK && dcirq_set(dev, pfl1, 1) == RDNA3_OK);
    CHECK(dcirq_enabled(dev, vbl0) == 1 && dcirq_enabled(dev, vbl2) == 1 && dcirq_enabled(dev, pfl1) == 1);
    CHECK(dcirq_enabled(dev, vup0) == 0 && dcirq_pending(dev, vbl0) == 0);
    CHECK(!(S.reg[SQ_GSS(0)] & OTG0_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_EVENT_OCCURRED_MASK));
    CHECK(!(S.reg[SQ_GSS(0)] & OTG0_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_EVENT_CLEAR_MASK));   /* a strobe */
    CHECK(ih60_process(dev, ih, sq_iv_cb, &s) == 0);

    /* a vblank per frame per head, both heads independent */
    for (i = 0; i < 5; i++) {
        sq_frame(0);
        sq_frame(2);
        CHECK(ih60_process(dev, ih, sq_iv_cb, &s) == 2);
    }
    for (i = 0; i < 3; i++) {
        sq_frame(0);
        CHECK(ih60_process(dev, ih, sq_iv_cb, &s) == 1);
    }
    CHECK(s.vbl[0] == 8 && s.vbl[2] == 5 && s.vbl[1] == 0 && s.other == 0 && s.pfl[0] == 0);
    CHECK(dcirq_pending(dev, vbl0) == 0);   /* acked */

    /* the ack re-arms: without it the level source stays latched, one IV */
    s.no_ack = 1;
    sq_frame(0);
    sq_frame(0);
    sq_frame(0);
    CHECK(ih60_process(dev, ih, sq_iv_cb, &s) == 1 && s.vbl[0] == 9 && dcirq_pending(dev, vbl0) == 1);
    CHECK(dcirq_ack(dev, vbl0) == RDNA3_OK && dcirq_pending(dev, vbl0) == 0);
    s.no_ack = 0;
    sq_frame(0);
    CHECK(ih60_process(dev, ih, sq_iv_cb, &s) == 1 && s.vbl[0] == 10);

    /* the page flip: programmed (held until the vblank), no IV while pending;
     * the vblank latches it and the HUBP's flip interrupt comes with it */
    S.dcn_hold = 1;
    rdna3_wreg(dev, HUBP(regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, 1), 0x00400000u);
    CHECK(S.reg[HUBP(regHUBPREQ0_DCSURF_FLIP_CONTROL, 1)] & HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK);
    CHECK(ih60_process(dev, ih, sq_iv_cb, &s) == 0 && s.pfl[1] == 0);
    sq_frame(0);   /* the other head's vblank does not latch it */
    CHECK(ih60_process(dev, ih, sq_iv_cb, &s) == 1 && s.pfl[1] == 0 && s.vbl[0] == 11);
    sq_frame(2);
    CHECK(ih60_process(dev, ih, sq_iv_cb, &s) == 2 && s.pfl[1] == 1 && s.vbl[2] == 6);
    CHECK(!(S.reg[HUBP(regHUBPREQ0_DCSURF_FLIP_CONTROL, 1)] & HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK));
    CHECK(S.reg[HUBP(regHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE, 1)] == 0x00400000u);
    CHECK(dcirq_pending(dev, pfl1) == 0);
    /* no flip, no flip interrupt */
    sq_frame(2);
    CHECK(ih60_process(dev, ih, sq_iv_cb, &s) == 1 && s.pfl[1] == 1 && s.vbl[2] == 7);
    /* a flip that latches at once (no hold) raises it at once */
    S.dcn_hold = 0;
    rdna3_wreg(dev, HUBP(regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, 0), 0x00800000u);
    CHECK(ih60_process(dev, ih, sq_iv_cb, &s) == 1 && s.pfl[0] == 1 && s.pfl[1] == 1);

    /* VUPDATE_NO_LOCK on request */
    CHECK(dcirq_set(dev, vup0, 1) == RDNA3_OK);
    sq_frame(0);
    CHECK(ih60_process(dev, ih, sq_iv_cb, &s) == 2 && s.vup[0] == 1 && s.vbl[0] == 12);
    CHECK(dcirq_set(dev, vup0, 0) == RDNA3_OK);

    /* disabled: silent, the other head goes on */
    CHECK(dcirq_set(dev, vbl2, 0) == RDNA3_OK && dcirq_enabled(dev, vbl2) == 0);
    sq_frame(2);
    sq_frame(0);
    CHECK(ih60_process(dev, ih, sq_iv_cb, &s) == 1 && s.vbl[2] == 7 && s.vbl[0] == 13);

    /* ECC review (the storm guard's clock is the handler's): the handler
     * stalls for 100 frames. A working source raises once and then waits
     * for its ack (the INT_STATUS latch), so the ring holds ONE IV of it:
     * drained, it is no storm; the guard notes the quiet stretch, nothing
     * is counted as a drained backlog, the frames after it count normally */
    {
        const unsigned v2 = s.vbl[2], storms0 = s.storms;
        CHECK(dcirq_set(dev, vbl2, 1) == RDNA3_OK);
        ih60_process(dev, ih, sq_iv_cb, &s);
        sq_frame(2);
        CHECK(ih60_process(dev, ih, sq_iv_cb, &s) == 1);
        for (i = 0; i < 100; i++)
            sq_frame(2);   /* 1.67 s, nobody drains the ring */
        CHECK(ih60_process(dev, ih, sq_iv_cb, &s) == 1 && s.vbl[2] == v2 + 2);
        CHECK(s.st[0][2].drain_end_ns != 0 && s.st[0][2].drained == 0 && s.storms == storms0);
        for (i = 0; i < 5; i++) {
            sq_frame(2);
            CHECK(ih60_process(dev, ih, sq_iv_cb, &s) == 1);
        }
        CHECK(s.vbl[2] == v2 + 7 && s.st[0][2].drained == 0 && s.st[0][2].drains == 0 && s.storms == storms0);
        CHECK(dcirq_set(dev, vbl2, 0) == RDNA3_OK);
        ih60_process(dev, ih, NULL, NULL);
    }

    /* a storm (a source that raises without waiting for the ack): cut at
     * RD3_VIRQ_STORM_MAX within the window, the source disabled, then silent */
    {
        const unsigned ivs0 = SQ.ivs, before = s.vbl[0];
        SQ.broken_level = 1;
        for (i = 0; i < 400; i++) {
            sq_otg_event(0);   /* no time passes: the events come back to back */
            ih60_process(dev, ih, sq_iv_cb, &s);
        }
        CHECK(s.storms == 1 && dcirq_enabled(dev, vbl0) == 0);
        /* the stall above left head 0 quiet: RD3_VIRQ_DRAIN_MAX more pass first */
        CHECK(s.vbl[0] - before <= RD3_VIRQ_STORM_MAX + 1 + RD3_VIRQ_DRAIN_MAX &&
              SQ.ivs - ivs0 <= RD3_VIRQ_STORM_MAX + 1 + RD3_VIRQ_DRAIN_MAX);
        CHECK(s.st[0][0].drained == RD3_VIRQ_DRAIN_MAX && s.st[0][0].drains == 1);
        SQ.broken_level = 0;
    }

    /* everything off; the model and the registers as they were */
    CHECK(dcirq_set(dev, pfl0, 0) == RDNA3_OK && dcirq_set(dev, pfl1, 0) == RDNA3_OK);
    ih60_process(dev, ih, NULL, NULL);
    SQ.model = 0;
    S.dcn_hold = hold0;
    S.dcn_flips = flips0;
    for (i = 0; i < RD_DCN_MAX_PIPES; i++) {
        S.reg[OTG(regOTG0_OTG_CONTROL, i)] = save_otg[i];
        S.reg[HUBP(regHUBP0_DCHUBP_CNTL, i)] = save_hubp[i];
        S.reg[HUBP(regHUBPREQ0_DCSURF_FLIP_CONTROL, i)] = save_fc[i];
        S.reg[OTG(regOTG0_OTG_STATUS_FRAME_COUNT, i)] = save_frame[i];
        S.reg[HUBP(regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, i)] = save_addr[i];
        S.reg[HUBP(regHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE, i)] = save_inuse[i];
        S.reg[SQ_GSS(i)] = 0;
        S.reg[SQ_FLIPI(i)] = 0;
    }
    printf("sim_navi32: DCN vblank / vupdate / pflip interrupts: %u vblank, %u pflip IVs handled, %u storm cut\n",
           s.vbl[0] + s.vbl[2], s.pfl[0] + s.pfl[1], s.storms);
}
