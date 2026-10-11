/*
 * sim_display_dc.inc - included by sim_navi32.c before sim_wr: the model of
 * the Display Core pieces ported for stage 4 item K (the OTG's scan
 * position / status / vertical interrupt / triggers, the DMCUB's GPINT
 * acks and inbox0) and their tests (test_dc_k, called from run()).
 *
 * The model only acts while a test turns it on (SK.otg_model,
 * SK.dmub_model): the older tests keep the plain register file.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_otg.h"
#include "gen/dcn_3_2_0_otg.h"
#include "gen/dcn_3_2_0_dmub_diag.h"
#include "dcn32_color.h"
#include "gen/dcn_3_2_0_color.h"

#define ODMR_K(reg, n) (DCN_B2 + reg + (regODM1_OPTC_INPUT_CLOCK_CONTROL - regODM0_OPTC_INPUT_CLOCK_CONTROL) * (n))
#define SK_FRAME_US 16667u    /* the modelled OTGs run at 60 Hz */

static struct {
    int otg_model, dmub_model;
    int gpint_hang;              /* the firmware does not ack GPINT commands */
    unsigned gpints, manual_trigs, inbox0_cmds, dmuif_sets;
    uint32_t inbox0_last;
} SK;

/* the line / pixel OTG o scans now (60 Hz frames of its V_TOTAL lines) */
static void sk_pos(uint32_t o, uint32_t *line, uint32_t *pix)
{
    uint64_t t = S.now_us % SK_FRAME_US;
    uint32_t vt = (S.reg[OTG(regOTG0_OTG_V_TOTAL, o)] & OTG0_OTG_V_TOTAL__OTG_V_TOTAL_MASK) + 1;
    uint32_t ht = (S.reg[OTG(regOTG0_OTG_H_TOTAL, o)] & OTG0_OTG_H_TOTAL__OTG_H_TOTAL_MASK) + 1;
    uint64_t lt = t * vt;
    *line = (uint32_t)(lt / SK_FRAME_US);
    *pix = (uint32_t)((lt % SK_FRAME_US) * ht / SK_FRAME_US);
}

static int sk_on(uint32_t o)
{
    return (S.reg[OTG(regOTG0_OTG_CONTROL, o)] & OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK) != 0;
}

static int sk_in_vblank(uint32_t o, uint32_t line)
{
    uint32_t vb = S.reg[OTG(regOTG0_OTG_V_BLANK_START_END, o)];
    uint32_t start = (vb & OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_START_MASK) >>
                     OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_START__SHIFT;
    uint32_t end = (vb & OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_END_MASK) >> OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_END__SHIFT;
    return line >= start || line < end;
}

/* the vertical interrupt 0 status latches while the counter is in range */
static void sk_vline0(uint32_t o)
{
    uint32_t pos = S.reg[OTG(regOTG0_OTG_VERTICAL_INTERRUPT0_POSITION, o)], line, pix;
    uint32_t start = (pos & OTG0_OTG_VERTICAL_INTERRUPT0_POSITION__OTG_VERTICAL_INTERRUPT0_LINE_START_MASK) >>
                     OTG0_OTG_VERTICAL_INTERRUPT0_POSITION__OTG_VERTICAL_INTERRUPT0_LINE_START__SHIFT;
    uint32_t end = (pos & OTG0_OTG_VERTICAL_INTERRUPT0_POSITION__OTG_VERTICAL_INTERRUPT0_LINE_END_MASK) >>
                   OTG0_OTG_VERTICAL_INTERRUPT0_POSITION__OTG_VERTICAL_INTERRUPT0_LINE_END__SHIFT;
    if (!sk_on(o))
        return;
    sk_pos(o, &line, &pix);
    if (line >= start && line <= end)
        S.reg[OTG(regOTG0_OTG_VERTICAL_INTERRUPT0_CONTROL, o)] |= OTG0_OTG_VERTICAL_INTERRUPT0_CONTROL__OTG_VERTICAL_INTERRUPT0_STATUS_MASK;
}

static int sk_rd(uint32_t dw, uint32_t *out)
{
    uint32_t o;
    if (SK.otg_model)
        for (o = 0; o < RD_DCN_MAX_PIPES; o++) {
            uint32_t line, pix;
            if (dw == OTG(regOTG0_OTG_STATUS_POSITION, o)) {
                S.now_us += 1;   /* a register read takes time */
                if (!sk_on(o)) {
                    *out = 0;
                    return 1;
                }
                sk_pos(o, &line, &pix);
                *out = (line << OTG0_OTG_STATUS_POSITION__OTG_VERT_COUNT__SHIFT) |
                       (pix << OTG0_OTG_STATUS_POSITION__OTG_HORZ_COUNT__SHIFT);
                return 1;
            }
            if (dw == OTG(regOTG0_OTG_STATUS, o)) {
                S.now_us += 1;
                sk_pos(o, &line, &pix);
                *out = !sk_on(o) ? 0 : sk_in_vblank(o, line) ? OTG0_OTG_STATUS__OTG_V_BLANK_MASK
                                                              : OTG0_OTG_STATUS__OTG_V_ACTIVE_DISP_MASK;
                return 1;
            }
            if (dw == OTG(regOTG0_OTG_VERTICAL_INTERRUPT0_CONTROL, o)) {
                S.now_us += 1;
                sk_vline0(o);
                *out = S.reg[dw];
                return 1;
            }
            if (dw == OTG(regOTG0_OTG_MASTER_UPDATE_LOCK, o)) {
                uint32_t v = S.reg[dw] & ~OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK;
                if (v & OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK)
                    v |= OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK;
                *out = v;
                return 1;
            }
            if (dw == OTG(regOTG0_OTG_CLOCK_CONTROL, o)) {
                uint32_t v = S.reg[dw] & ~OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_ON_MASK;
                if (v & OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_EN_MASK)
                    v |= OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_ON_MASK;
                *out = v;
                return 1;
            }
            if (dw == ODMR_K(regODM0_OPTC_INPUT_CLOCK_CONTROL, o)) {
                uint32_t v = S.reg[dw] & ~ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_ON_MASK;
                if (v & ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_EN_MASK)
                    v |= ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_ON_MASK;
                *out = v;
                return 1;
            }
        }
    return 0;
}

static int sk_wr(uint32_t dw, uint32_t v)
{
    uint32_t o;
    /* the MPCC gamut remap: the selected set becomes the current one */
    for (o = 0; o < RD_DCN_MAX_PIPES; o++)
        if (dw == DCN_B3 + regMPCC_OGAM0_MPCC_GAMUT_REMAP_MODE +
                      (regMPCC_OGAM1_MPCC_GAMUT_REMAP_MODE - regMPCC_OGAM0_MPCC_GAMUT_REMAP_MODE) * o) {
            uint32_t m = v & MPCC_OGAM0_MPCC_GAMUT_REMAP_MODE__MPCC_GAMUT_REMAP_MODE_MASK;
            S.reg[dw] = m | (m << MPCC_OGAM0_MPCC_GAMUT_REMAP_MODE__MPCC_GAMUT_REMAP_MODE_CURRENT__SHIFT);
            return 1;
        }
    if (SK.otg_model)
        for (o = 0; o < RD_DCN_MAX_PIPES; o++) {
            if (dw == OTG(regOTG0_OTG_VERTICAL_INTERRUPT0_CONTROL, o)) {
                uint32_t keep = v & ~OTG0_OTG_VERTICAL_INTERRUPT0_CONTROL__OTG_VERTICAL_INTERRUPT0_CLEAR_MASK;
                keep &= ~OTG0_OTG_VERTICAL_INTERRUPT0_CONTROL__OTG_VERTICAL_INTERRUPT0_STATUS_MASK;
                if (!(v & OTG0_OTG_VERTICAL_INTERRUPT0_CONTROL__OTG_VERTICAL_INTERRUPT0_CLEAR_MASK))
                    keep |= S.reg[dw] & OTG0_OTG_VERTICAL_INTERRUPT0_CONTROL__OTG_VERTICAL_INTERRUPT0_STATUS_MASK;
                S.reg[dw] = keep;
                return 1;
            }
            if (dw == ODMR_K(regODM0_OPTC_INPUT_GLOBAL_CONTROL, o)) {
                uint32_t keep = v & ~ODM0_OPTC_INPUT_GLOBAL_CONTROL__OPTC_UNDERFLOW_CLEAR_MASK;
                keep &= ~ODM0_OPTC_INPUT_GLOBAL_CONTROL__OPTC_UNDERFLOW_OCCURRED_STATUS_MASK;
                if (!(v & ODM0_OPTC_INPUT_GLOBAL_CONTROL__OPTC_UNDERFLOW_CLEAR_MASK))
                    keep |= S.reg[dw] & ODM0_OPTC_INPUT_GLOBAL_CONTROL__OPTC_UNDERFLOW_OCCURRED_STATUS_MASK;
                S.reg[dw] = keep;
                return 1;
            }
            if (dw == OTG(regOTG0_OTG_FORCE_COUNT_NOW_CNTL, o) &&
                (v & OTG0_OTG_FORCE_COUNT_NOW_CNTL__OTG_FORCE_COUNT_NOW_CLEAR_MASK)) {
                S.reg[dw] = v & ~(OTG0_OTG_FORCE_COUNT_NOW_CNTL__OTG_FORCE_COUNT_NOW_CLEAR_MASK |
                                  OTG0_OTG_FORCE_COUNT_NOW_CNTL__OTG_FORCE_COUNT_NOW_OCCURRED_MASK);
                return 1;
            }
            if (dw == OTG(regOTG0_OTG_VERT_SYNC_CONTROL, o) &&
                (v & OTG0_OTG_VERT_SYNC_CONTROL__OTG_FORCE_VSYNC_NEXT_LINE_CLEAR_MASK)) {
                S.reg[dw] = v & ~(OTG0_OTG_VERT_SYNC_CONTROL__OTG_FORCE_VSYNC_NEXT_LINE_CLEAR_MASK |
                                  OTG0_OTG_VERT_SYNC_CONTROL__OTG_FORCE_VSYNC_NEXT_LINE_OCCURRED_MASK);
                return 1;
            }
            if (dw == OTG(regOTG0_OTG_TRIGA_MANUAL_TRIG, o)) {
                SK.manual_trigs++;
                S.reg[dw] = 0;   /* a strobe */
                return 1;
            }
        }
    if (SK.dmub_model) {
        if (dw == DMR(regDMCUB_GPINT_DATAIN1)) {
            SK.gpints++;
            S.reg[dw] = v;
            if (!S.dmub.running || !(v >> 28) || SK.gpint_hang)
                return 1;
            if (((v >> 16) & 0xfff) == RD_DMUB_GPINT_STOP_FW) {
                S.dmub.running = 0;
                S.dmub.stops++;
                S.reg[DMR(regDMCUB_SCRATCH7)] = RD_DMUB_GPINT_STOP_FW_RESPONSE;
                S.reg[DMR(regDMCUB_CNTL)] |= DMCUB_CNTL__DMCUB_PWAIT_MODE_STATUS_MASK;
            } else if (((v >> 16) & 0xfff) == RD_DMUB_GPINT_GET_FW_VERSION) {
                S.reg[DMR(regDMCUB_SCRATCH7)] = 0x07001234u;
                S.reg[DMR(regDMCUB_GPINT_DATAOUT)] = 0x07001234u;
            }
            S.reg[dw] = v & 0x0fffffffu;   /* the ack: status cleared */
            return 1;
        }
        if (dw == DMR(regDMCUB_INBOX0_WPTR)) {
            S.reg[dw] = v;
            if (S.dmub.running && !SK.gpint_hang) {
                SK.inbox0_cmds++;
                SK.inbox0_last = v;
                S.reg[DMR(regDMCUB_SCRATCH17)] = 1;
            }
            return 1;
        }
        if (dw == DMR(regMMHUBBUB_SOFT_RESET) && (v & MMHUBBUB_SOFT_RESET__DMUIF_SOFT_RESET_MASK) &&
            !(S.reg[dw] & MMHUBBUB_SOFT_RESET__DMUIF_SOFT_RESET_MASK))
            SK.dmuif_sets++;
    }
    return 0;
}

/* ---- tests ---- */

static void test_dc_otg(rdna3_dev *dev)
{
    const uint32_t o = 3;
    rdna3_crtc_position p;
    rdna3_mode m;
    rdna3_crtc_format f;
    uint32_t w = 0, h = 0, vbs, vbe, hp, vp, opp[4] = { 0, 1, 2, 3 }, k;
    rdna3_drr_params drr;

    memset(&SK, 0, sizeof(SK));
    SK.otg_model = 1;
    /* OTG 3: 1920x1080, htotal 2200, vtotal 1125, blank 1080..0 (V_BLANK_END 0 + active from 0) */
    S.reg[OTG(regOTG0_OTG_H_TOTAL, o)] = 2199;
    S.reg[OTG(regOTG0_OTG_V_TOTAL, o)] = 1124;
    S.reg[OTG(regOTG0_OTG_H_BLANK_START_END, o)] = 2000u | 80u << OTG0_OTG_H_BLANK_START_END__OTG_H_BLANK_END__SHIFT;
    S.reg[OTG(regOTG0_OTG_V_BLANK_START_END, o)] = 1121u | 41u << OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_END__SHIFT;
    S.reg[OTG(regOTG0_OTG_CONTROL, o)] = 0;

    /* off: no active size, the counter does not move */
    CHECK(!dcn32_otg_get_active_size(dev, o, &w, &h));
    CHECK(!dcn32_otg_is_counter_moving(dev, o));
    S.reg[OTG(regOTG0_OTG_CONTROL, o)] = OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK;
    CHECK(dcn32_otg_get_active_size(dev, o, &w, &h) && w == 1920 && h == 1080);
    CHECK(dcn32_otg_is_counter_moving(dev, o));
    dcn32_otg_get_position(dev, o, &p);
    CHECK(p.vertical_count < 1125 && p.horizontal_count < 2200);
    dcn32_otg_get_scanoutpos(dev, o, &vbs, &vbe, &hp, &vp);
    CHECK(vbs == 1121 && vbe == 41 && vp < 1125);

    /* optc1_wait_for_state: vblank, then active, in order */
    S.now_us = (S.now_us / SK_FRAME_US + 1) * SK_FRAME_US + 100;   /* line ~6: in the blank */
    CHECK(dcn32_otg_wait_for_state(dev, o, RD_CRTC_STATE_VBLANK) == RDNA3_OK);
    CHECK(dcn32_otg_wait_for_state(dev, o, RD_CRTC_STATE_VACTIVE) == RDNA3_OK);
    dcn32_otg_get_position(dev, o, &p);
    CHECK(p.vertical_count >= 41 && p.vertical_count < 1121);
    S.reg[OTG(regOTG0_OTG_CONTROL, o)] = 0;
    CHECK(dcn32_otg_wait_for_state(dev, o, RD_CRTC_STATE_VACTIVE) == RDNA3_ETIME);   /* off: 100 ms */
    S.reg[OTG(regOTG0_OTG_CONTROL, o)] = OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK;

    /* vertical interrupt 0 on lines 500..510: latched within a frame, cleared */
    dcn32_otg_setup_vertical_interrupt0(dev, o, 500, 510);
    CHECK(S.reg[OTG(regOTG0_OTG_VERTICAL_INTERRUPT0_POSITION, o)] == (500u | 510u << 16));
    dcn32_otg_vline0_clear(dev, o);
    S.now_us = (S.now_us / SK_FRAME_US + 1) * SK_FRAME_US;   /* line 0 */
    CHECK(!dcn32_otg_vline0_status(dev, o));
    for (k = 0; k < 2000 && !dcn32_otg_vline0_status(dev, o); k++)
        S.now_us += 10;
    CHECK(dcn32_otg_vline0_status(dev, o));
    dcn32_otg_get_position(dev, o, &p);
    CHECK(p.vertical_count >= 500 && p.vertical_count <= 512);
    dcn32_otg_vline0_clear(dev, o);
    S.now_us = (S.now_us / SK_FRAME_US + 1) * SK_FRAME_US;
    CHECK(!dcn32_otg_vline0_status(dev, o));
    dcn32_otg_setup_vertical_interrupt1(dev, o, 7);
    dcn32_otg_setup_vertical_interrupt2(dev, o, 9);
    CHECK(S.reg[OTG(regOTG0_OTG_VERTICAL_INTERRUPT1_POSITION, o)] == 7 &&
          S.reg[OTG(regOTG0_OTG_VERTICAL_INTERRUPT2_POSITION, o)] == 9);

    /* optc1_validate_timing: CEA 1080p60 passes; short blanks, syncs, interlace do not */
    memset(&m, 0, sizeof(m));
    m.h_active = 1920; m.v_active = 1080; m.h_total = 2200; m.v_total = 1125;
    m.h_front = 88; m.h_sync = 44; m.v_front = 4; m.v_sync = 5; m.pixel_khz = 148500;
    CHECK(dcn32_otg_validate_timing(&m));
    m.h_total = 1920 + 31;
    CHECK(!dcn32_otg_validate_timing(&m));
    m.h_total = 2200; m.v_total = 1082;
    CHECK(!dcn32_otg_validate_timing(&m));
    m.v_total = 1125; m.h_sync = 3;
    CHECK(!dcn32_otg_validate_timing(&m));
    m.h_sync = 44; m.interlaced = 1;
    CHECK(!dcn32_otg_validate_timing(&m));
    m.interlaced = 0; m.h_total = 32769;
    CHECK(!dcn32_otg_validate_timing(&m));

    memset(&f, 0, sizeof(f));
    f.pixel_encoding = RD_PIXEL_ENCODING_RGB;
    CHECK(!dcn32_otg_two_pixels_per_container(&f));
    f.pixel_encoding = RD_PIXEL_ENCODING_YCBCR420;
    CHECK(dcn32_otg_two_pixels_per_container(&f));
    f.pixel_encoding = RD_PIXEL_ENCODING_YCBCR422; f.dsc = 1;
    CHECK(dcn32_otg_two_pixels_per_container(&f));
    f.dsc_ycbcr422_simple = 1;
    CHECK(!dcn32_otg_two_pixels_per_container(&f));

    /* optc3_tg_init: DRR timing double buffered at frame start, underflow cleared */
    S.reg[ODMR_K(regODM0_OPTC_INPUT_GLOBAL_CONTROL, o)] = ODM0_OPTC_INPUT_GLOBAL_CONTROL__OPTC_UNDERFLOW_OCCURRED_STATUS_MASK;
    CHECK(dcn32_otg_underflow_occurred(dev, o));
    dcn32_otg_tg_init(dev, o);
    CHECK(!dcn32_otg_underflow_occurred(dev, o));
    CHECK(((S.reg[OTG(regOTG0_OTG_DOUBLE_BUFFER_CONTROL, o)] & OTG0_OTG_DOUBLE_BUFFER_CONTROL__OTG_DRR_TIMING_DBUF_UPDATE_MODE_MASK) >>
           OTG0_OTG_DOUBLE_BUFFER_CONTROL__OTG_DRR_TIMING_DBUF_UPDATE_MODE__SHIFT) == 2);

    /* optc1_enable_optc_clock: on waits for CLK_ON; off clears a pending underflow first */
    dcn32_otg_enable_optc_clock(dev, o, 1);
    CHECK((S.reg[OTG(regOTG0_OTG_CLOCK_CONTROL, o)] & OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_EN_MASK) &&
          (S.reg[ODMR_K(regODM0_OPTC_INPUT_CLOCK_CONTROL, o)] & ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_GATE_DIS_MASK));
    S.reg[ODMR_K(regODM0_OPTC_INPUT_GLOBAL_CONTROL, o)] |= ODM0_OPTC_INPUT_GLOBAL_CONTROL__OPTC_UNDERFLOW_OCCURRED_STATUS_MASK;
    dcn32_otg_enable_optc_clock(dev, o, 0);
    CHECK(!dcn32_otg_underflow_occurred(dev, o) &&
          !(S.reg[OTG(regOTG0_OTG_CLOCK_CONTROL, o)] & OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_EN_MASK) &&
          !(S.reg[ODMR_K(regODM0_OPTC_INPUT_CLOCK_CONTROL, o)] & ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_EN_MASK));
    dcn32_otg_enable_optc_clock(dev, o, 1);

    /* static screen: 8-bit frame count, bit 8 of the event mask dropped */
    dcn32_otg_set_static_screen_control(dev, o, 0x181, 300);
    CHECK(S.reg[OTG(regOTG0_OTG_STATIC_SCREEN_CONTROL, o)] ==
          (0x81u | 0xffu << OTG0_OTG_STATIC_SCREEN_CONTROL__OTG_STATIC_SCREEN_FRAME_COUNT__SHIFT));

    /* timing sync: trigger on OTG 1's vsync (rising edge with positive
     * vsync), reset seen, cleared */
    S.reg[OTG(regOTG0_OTG_V_SYNC_A_CNTL, o)] = 0;
    dcn32_otg_enable_reset_trigger(dev, o, 1);
    k = S.reg[OTG(regOTG0_OTG_TRIGA_CNTL, o)];
    CHECK((k & OTG0_OTG_TRIGA_CNTL__OTG_TRIGA_SOURCE_SELECT_MASK) >> OTG0_OTG_TRIGA_CNTL__OTG_TRIGA_SOURCE_SELECT__SHIFT == 20 &&
          (k & OTG0_OTG_TRIGA_CNTL__OTG_TRIGA_SOURCE_PIPE_SELECT_MASK) >> OTG0_OTG_TRIGA_CNTL__OTG_TRIGA_SOURCE_PIPE_SELECT__SHIFT == 1 &&
          (k & OTG0_OTG_TRIGA_CNTL__OTG_TRIGA_RISING_EDGE_DETECT_CNTL_MASK) &&
          !(k & OTG0_OTG_TRIGA_CNTL__OTG_TRIGA_FALLING_EDGE_DETECT_CNTL_MASK));
    CHECK(S.reg[OTG(regOTG0_OTG_FORCE_COUNT_NOW_CNTL, o)] == 2u);
    S.reg[OTG(regOTG0_OTG_V_SYNC_A_CNTL, o)] = OTG0_OTG_V_SYNC_A_CNTL__OTG_V_SYNC_A_POL_MASK;
    dcn32_otg_enable_reset_trigger(dev, o, 1);
    CHECK(S.reg[OTG(regOTG0_OTG_TRIGA_CNTL, o)] & OTG0_OTG_TRIGA_CNTL__OTG_TRIGA_FALLING_EDGE_DETECT_CNTL_MASK);
    CHECK(!dcn32_otg_did_triggered_reset_occur(dev, o));
    S.reg[OTG(regOTG0_OTG_FORCE_COUNT_NOW_CNTL, o)] |= OTG0_OTG_FORCE_COUNT_NOW_CNTL__OTG_FORCE_COUNT_NOW_OCCURRED_MASK;
    CHECK(dcn32_otg_did_triggered_reset_occur(dev, o));
    dcn32_otg_disable_reset_trigger(dev, o);
    CHECK(!dcn32_otg_did_triggered_reset_occur(dev, o) && S.reg[OTG(regOTG0_OTG_TRIGA_CNTL, o)] == 0);
    dcn32_otg_enable_crtc_reset(dev, o, 2, RD_CRTC_EVENT_VSYNC_FALLING, RD_TRIGGER_DELAY_NEXT_LINE);
    CHECK((S.reg[OTG(regOTG0_OTG_TRIGA_CNTL, o)] & OTG0_OTG_TRIGA_CNTL__OTG_TRIGA_FALLING_EDGE_DETECT_CNTL_MASK) &&
          (S.reg[OTG(regOTG0_OTG_VERT_SYNC_CONTROL, o)] & OTG0_OTG_VERT_SYNC_CONTROL__OTG_AUTO_FORCE_VSYNC_MODE_MASK));
    S.reg[OTG(regOTG0_OTG_VERT_SYNC_CONTROL, o)] |= OTG0_OTG_VERT_SYNC_CONTROL__OTG_FORCE_VSYNC_NEXT_LINE_OCCURRED_MASK;
    CHECK(dcn32_otg_did_triggered_reset_occur(dev, o));
    dcn32_otg_disable_reset_trigger(dev, o);
    CHECK(!dcn32_otg_did_triggered_reset_occur(dev, o));

    /* DRR: v_total 1125..1350, mid 1200 for 2 frames, manual trigger on TRIGA */
    memset(&drr, 0, sizeof(drr));
    drr.vertical_total_min = 1125; drr.vertical_total_max = 1350;
    drr.vertical_total_mid = 1200; drr.vertical_total_mid_frame_num = 2;
    dcn32_otg_set_drr(dev, o, &drr);
    CHECK(S.reg[OTG(regOTG0_OTG_V_TOTAL_MIN, o)] == 1124 && S.reg[OTG(regOTG0_OTG_V_TOTAL_MAX, o)] == 1349 &&
          S.reg[OTG(regOTG0_OTG_V_TOTAL_MID, o)] == 1199);
    k = S.reg[OTG(regOTG0_OTG_V_TOTAL_CONTROL, o)];
    CHECK((k & OTG0_OTG_V_TOTAL_CONTROL__OTG_V_TOTAL_MIN_SEL_MASK) && (k & OTG0_OTG_V_TOTAL_CONTROL__OTG_V_TOTAL_MAX_SEL_MASK) &&
          (k & OTG0_OTG_V_TOTAL_CONTROL__OTG_VTOTAL_MID_REPLACING_MAX_EN_MASK) &&
          ((k & OTG0_OTG_V_TOTAL_CONTROL__OTG_VTOTAL_MID_FRAME_NUM_MASK) >> OTG0_OTG_V_TOTAL_CONTROL__OTG_VTOTAL_MID_FRAME_NUM__SHIFT) == 2 &&
          ((k & OTG0_OTG_V_TOTAL_CONTROL__OTG_SET_V_TOTAL_MIN_MASK_MASK) >> OTG0_OTG_V_TOTAL_CONTROL__OTG_SET_V_TOTAL_MIN_MASK__SHIFT) == 2 &&
          !(k & OTG0_OTG_V_TOTAL_CONTROL__OTG_FORCE_LOCK_ON_EVENT_MASK));
    dcn32_otg_program_manual_trigger(dev, o);
    CHECK(SK.manual_trigs == 1);
    dcn32_otg_set_drr_trigger_window(dev, o, 10, 20);
    dcn32_otg_set_vtotal_change_limit(dev, o, 0x80);
    CHECK(S.reg[OTG(regOTG0_OTG_DRR_TRIGGER_WINDOW, o)] == (10u | 20u << 16) &&
          S.reg[OTG(regOTG0_OTG_DRR_V_TOTAL_CHANGE, o)] == 0x80);
    /* DRR off (no params): the trigger stays, min / max untouched */
    dcn32_otg_set_drr(dev, o, NULL);
    CHECK(S.reg[OTG(regOTG0_OTG_V_TOTAL_MAX, o)] == 1349);

    /* locks: optc3_lock waits for the status, unlock drops it; triple
     * buffer keeps VUPDATE out; double-buffered lock before the h blank */
    dcn32_otg_lock(dev, o, 1);
    CHECK((S.reg[OTG(regOTG0_OTG_MASTER_UPDATE_LOCK, o)] & 1) &&
          ((S.reg[OTG(regOTG0_OTG_GLOBAL_CONTROL2, o)] & OTG0_OTG_GLOBAL_CONTROL2__OTG_MASTER_UPDATE_LOCK_SEL_MASK) >>
           OTG0_OTG_GLOBAL_CONTROL2__OTG_MASTER_UPDATE_LOCK_SEL__SHIFT) == o);
    dcn32_otg_lock(dev, o, 0);
    CHECK(!(S.reg[OTG(regOTG0_OTG_MASTER_UPDATE_LOCK, o)] & 1));
    dcn32_otg_triplebuffer_lock(dev, o, 1);
    CHECK(S.reg[OTG(regOTG0_OTG_VUPDATE_KEEPOUT, o)] & OTG0_OTG_VUPDATE_KEEPOUT__OTG_MASTER_UPDATE_LOCK_VUPDATE_KEEPOUT_EN_MASK);
    dcn32_otg_triplebuffer_lock(dev, o, 0);
    CHECK(S.reg[OTG(regOTG0_OTG_VUPDATE_KEEPOUT, o)] == 0 && !(S.reg[OTG(regOTG0_OTG_MASTER_UPDATE_LOCK, o)] & 1));
    dcn32_otg_lock_doublebuffer(dev, o, 1);
    k = S.reg[OTG(regOTG0_OTG_GLOBAL_CONTROL0, o)];
    CHECK((k & OTG0_OTG_GLOBAL_CONTROL0__MASTER_UPDATE_LOCK_DB_EN_MASK) &&
          (k & OTG0_OTG_GLOBAL_CONTROL0__MASTER_UPDATE_LOCK_DB_START_X_MASK) == 1799u &&
          ((k & OTG0_OTG_GLOBAL_CONTROL0__MASTER_UPDATE_LOCK_DB_END_X_MASK) >> OTG0_OTG_GLOBAL_CONTROL0__MASTER_UPDATE_LOCK_DB_END_X__SHIFT) == 1820u);
    k = S.reg[OTG(regOTG0_OTG_GLOBAL_CONTROL1, o)];
    CHECK((k & OTG0_OTG_GLOBAL_CONTROL1__MASTER_UPDATE_LOCK_DB_START_Y_MASK) == 1120u &&
          ((k & OTG0_OTG_GLOBAL_CONTROL1__MASTER_UPDATE_LOCK_DB_END_Y_MASK) >> OTG0_OTG_GLOBAL_CONTROL1__MASTER_UPDATE_LOCK_DB_END_Y__SHIFT) == 1121u);
    CHECK((S.reg[OTG(regOTG0_OTG_GLOBAL_CONTROL2, o)] & OTG0_OTG_GLOBAL_CONTROL2__GLOBAL_UPDATE_LOCK_EN_MASK) &&
          S.reg[OTG(regOTG0_OTG_VUPDATE_KEEPOUT, o)] ==
              (100u << OTG0_OTG_VUPDATE_KEEPOUT__MASTER_UPDATE_LOCK_VUPDATE_KEEPOUT_END_OFFSET__SHIFT |
               OTG0_OTG_VUPDATE_KEEPOUT__OTG_MASTER_UPDATE_LOCK_VUPDATE_KEEPOUT_EN_MASK));
    dcn32_otg_lock_doublebuffer(dev, o, 0);
    CHECK(!(S.reg[OTG(regOTG0_OTG_GLOBAL_CONTROL0, o)] & OTG0_OTG_GLOBAL_CONTROL0__MASTER_UPDATE_LOCK_DB_EN_MASK) &&
          !(S.reg[OTG(regOTG0_OTG_GLOBAL_CONTROL2, o)] & OTG0_OTG_GLOBAL_CONTROL2__GLOBAL_UPDATE_LOCK_EN_MASK) &&
          S.reg[OTG(regOTG0_OTG_GLOBAL_CONTROL1, o)] == 0);

    /* ODM: 2 OPPs for a 3840-wide mode (2 memories), 4 for 7680; the
     * segments read back; bypass reads 1 */
    S.reg[ODMR_K(regODM0_OPTC_DATA_SOURCE_SELECT, o)] = 0;
    CHECK(dcn32_otg_odm_combine_segments(dev, o) == 1);
    opp[0] = 2; opp[1] = 3;
    CHECK(dcn32_otg_set_odm_combine(dev, o, opp, 2, 1920) == RDNA3_OK);
    CHECK(S.reg[ODMR_K(regODM0_OPTC_MEMORY_CONFIG, o)] == (0x1u << 4 | 0x1u << 6) &&
          dcn32_otg_odm_combine_segments(dev, o) == 2 &&
          (S.reg[ODMR_K(regODM0_OPTC_WIDTH_CONTROL, o)] & ODM0_OPTC_WIDTH_CONTROL__OPTC_SEGMENT_WIDTH_MASK) == 1920 &&
          (S.reg[OTG(regOTG0_OTG_H_TIMING_CNTL, o)] & OTG0_OTG_H_TIMING_CNTL__OTG_H_TIMING_DIV_MODE_MASK) >>
                  OTG0_OTG_H_TIMING_CNTL__OTG_H_TIMING_DIV_MODE__SHIFT == 1);
    CHECK(dcn32_otg_set_odm_combine(dev, o, opp, 2, 3840) == RDNA3_OK &&
          S.reg[ODMR_K(regODM0_OPTC_MEMORY_CONFIG, o)] == (0x3u << 4 | 0x3u << 6));
    opp[0] = 0; opp[1] = 1; opp[2] = 2; opp[3] = 3;
    CHECK(dcn32_otg_set_odm_combine(dev, o, opp, 4, 1920) == RDNA3_OK &&
          S.reg[ODMR_K(regODM0_OPTC_MEMORY_CONFIG, o)] == 0xf && dcn32_otg_odm_combine_segments(dev, o) == 4);
    CHECK(dcn32_otg_set_odm_combine(dev, o, opp, 3, 1920) == RDNA3_EINVAL);
    S.reg[ODMR_K(regODM0_OPTC_DATA_SOURCE_SELECT, o)] = 2;
    CHECK(dcn32_otg_odm_combine_segments(dev, o) == -1);
    S.reg[ODMR_K(regODM0_OPTC_DATA_SOURCE_SELECT, o)] = 0;
    dcn32_otg_set_h_timing_div_manual_mode(dev, o, 1);
    CHECK(S.reg[OTG(regOTG0_OTG_H_TIMING_CNTL, o)] & OTG0_OTG_H_TIMING_CNTL__OTG_H_TIMING_DIV_MODE_MANUAL_MASK);
    dcn32_otg_set_h_timing_div_manual_mode(dev, o, 0);
    CHECK(!(S.reg[OTG(regOTG0_OTG_H_TIMING_CNTL, o)] & OTG0_OTG_H_TIMING_CNTL__OTG_H_TIMING_DIV_MODE_MANUAL_MASK));
    S.reg[ODMR_K(regODM0_OPTC_INPUT_GLOBAL_CONTROL, o)] = ODM0_OPTC_INPUT_GLOBAL_CONTROL__OPTC_DOUBLE_BUFFER_PENDING_MASK;
    CHECK(dcn32_otg_double_buffer_pending(dev, o));
    S.reg[ODMR_K(regODM0_OPTC_INPUT_GLOBAL_CONTROL, o)] = 0;
    CHECK(!dcn32_otg_double_buffer_pending(dev, o));
    dcn32_otg_set_out_mux(dev, o, 1);
    CHECK((S.reg[OTG(regOTG0_OTG_CONTROL, o)] & OTG0_OTG_CONTROL__OTG_OUT_MUX_MASK) >> OTG0_OTG_CONTROL__OTG_OUT_MUX__SHIFT == 1 &&
          sk_on(o));
    dcn32_otg_set_out_mux(dev, o, 0);

    /* the "otg" ladder step on the lit OTG: everything read, the vertical
     * interrupt 0 latched mid-frame and its registers put back */
    {
        rdna3_otg_check c;
        S.reg[OTG(regOTG0_OTG_VERTICAL_INTERRUPT0_POSITION, o)] = 0x12345;
        S.reg[OTG(regOTG0_OTG_VERTICAL_INTERRUPT0_CONTROL, o)] = 0;
        CHECK(dcn32_otg_check(dev, o, &c) == RDNA3_OK);
        CHECK(c.moving && c.width == 1920 && c.height == 1080 && c.vblank_seen && c.vactive_seen &&
              c.vline0_seen && c.vline0_line == 41 + 540 && c.vline0_at >= 581 && c.vline0_at <= 584 &&
              !c.underflow && !c.db_pending && c.odm_segments == 1);
        CHECK(S.reg[OTG(regOTG0_OTG_VERTICAL_INTERRUPT0_POSITION, o)] == 0x12345 &&
              S.reg[OTG(regOTG0_OTG_VERTICAL_INTERRUPT0_CONTROL, o)] == 0);
        S.reg[OTG(regOTG0_OTG_CONTROL, o)] = 0;
        CHECK(dcn32_otg_check(dev, o, &c) == RDNA3_ENODEV);
    }

    /* leave OTG 3 as found: off, its registers cleared */
    for (k = regOTG0_OTG_H_TOTAL; k < regOTG1_OTG_CONTROL - regOTG0_OTG_CONTROL + regOTG0_OTG_H_TOTAL; k++)
        S.reg[DCN_B2 + k + (regOTG1_OTG_CONTROL - regOTG0_OTG_CONTROL) * o] = 0;
    S.reg[ODMR_K(regODM0_OPTC_MEMORY_CONFIG, o)] = 0;
    S.reg[ODMR_K(regODM0_OPTC_WIDTH_CONTROL, o)] = 0;
    S.reg[ODMR_K(regODM0_OPTC_INPUT_CLOCK_CONTROL, o)] = 0;
    SK.otg_model = 0;
}

static void test_dc_dmub(rdna3_dev *dev, rdna3_dmub *dm)
{
    unsigned stops0 = S.dmub.stops, boots0 = S.dmub.boots, cmds0;
    rdna3_dmub_diag g;
    rdna3_dmub_trace_entry e;
    uint32_t base, k;

    memset(&SK, 0, sizeof(SK));
    SK.dmub_model = 1;
    CHECK(dm->hw_init && S.dmub.running && dm->power_state == RD_DMUB_POWER_STATE_D0);

    /* GPINT: GET_FW_VERSION acked, its answer in SCRATCH7 and DATAOUT
     * (reading DATAOUT acks the GPINT interrupt and clears it) */
    CHECK(dmub32_send_gpint(dm, RD_DMUB_GPINT_GET_FW_VERSION, 0, 1000) == RDNA3_OK);
    CHECK(dmub32_gpint_response(dm) == 0x07001234u);
    CHECK(dmub32_gpint_dataout(dm) == 0x07001234u && S.reg[DMR(regDMCUB_GPINT_DATAOUT)] == 0 &&
          (S.reg[DMR(regDMCUB_INTERRUPT_ENABLE)] & DMCUB_INTERRUPT_ENABLE__DMCUB_GPINT_IH_INT_EN_MASK) &&
          !(S.reg[DMR(regDMCUB_INTERRUPT_ACK)] & DMCUB_INTERRUPT_ACK__DMCUB_GPINT_IH_INT_ACK_MASK));
    SK.gpint_hang = 1;
    CHECK(dmub32_send_gpint(dm, RD_DMUB_GPINT_GET_FW_VERSION, 0, 50) == RDNA3_ETIME);
    SK.gpint_hang = 0;

    /* inbox0: the SubVP HW lock word, acked in SCRATCH17 */
    CHECK(dmub32_inbox0_hw_lock_word(1) == (1u | 1u << 23) && dmub32_inbox0_hw_lock_word(0) == (1u | 1u << 24));
    CHECK(dmub32_hw_lock_inbox0(dm, 1) == RDNA3_OK && SK.inbox0_cmds == 1 && SK.inbox0_last == (1u | 1u << 23));
    CHECK(dmub32_hw_lock_inbox0(dm, 0) == RDNA3_OK && SK.inbox0_last == (1u | 1u << 24));
    SK.gpint_hang = 1;   /* a hung firmware does not ack */
    CHECK(dmub32_hw_lock_inbox0(dm, 1) == RDNA3_ETIME);
    SK.gpint_hang = 0;

    /* the trace ring: two entries the firmware logged, then nothing */
    base = (uint32_t)(dm->fb_off + dm->region[RD_DMUB_WIN_TRACEBUFF].base) + RD_DMUB_TRACE_ENTRY_OFFSET;
    dm->outbox0_rptr = 0;
    S.reg[DMR(regDMCUB_OUTBOX0_WPTR)] = 0;
    CHECK(!dmub32_trace_read(dm, &e));
    for (k = 0; k < 8; k++) {
        uint32_t v = 0x100 + k;
        memcpy(S.vram + base + 4 * k, &v, 4);
    }
    S.reg[DMR(regDMCUB_OUTBOX0_WPTR)] = 32;
    CHECK(dmub32_trace_read(dm, &e) && e.trace_code == 0x100 && e.tick_count == 0x101 && e.param1 == 0x103);
    CHECK(dmub32_trace_read(dm, &e) && e.trace_code == 0x104 && e.param0 == 0x106);
    CHECK(!dmub32_trace_read(dm, &e) && dm->outbox0_rptr == 32);
    S.reg[DMR(regDMCUB_OUTBOX0_WPTR)] = 0;
    dm->outbox0_rptr = 0;

    /* the diagnostic data */
    S.reg[DMR(regDMCUB_SCRATCH16)] = 0x1616;
    S.reg[DMR(regDMCUB_INBOX0_SIZE)] = 0x40;
    CHECK(dmub32_get_diagnostic_data(dm, &g) && g.is_dmcub_enabled && !g.is_dmcub_soft_reset &&
          g.scratch[16] == 0x1616 && g.scratch[0] == dmub32_fw_boot_status(dev) && g.inbox0_size == 0x40 &&
          g.dmcub_version == dm->fw_version && g.is_cw6_enabled);
    dmub32_log_diagnostic(dm, "test");
    (void)dmub32_current_time(dev);

    /* D3: nothing goes to the firmware; D0 again */
    cmds0 = S.dmub.cmds;
    dmub32_set_power_state(dm, RD_DMUB_POWER_STATE_D3);
    CHECK(dmub32_query_caps(dm) == RDNA3_EBUSY && S.dmub.cmds == cmds0);
    dmub32_set_power_state(dm, RD_DMUB_POWER_STATE_D0);
    CHECK(dmub32_query_caps(dm) == RDNA3_OK && S.dmub.cmds == cmds0 + 1);

    /* rdna3dmubreset=1: amdgpu's dmub_dcn32_reset. The firmware acks
     * STOP_FW and stops; DMUIF goes through reset and is released; the
     * core boots again and answers */
    dm->amd_reset = 1;
    CHECK(dmub32_hw_init(dm, S.vbios, VBIOS_LEN, 1, 100000) == RDNA3_OK);
    CHECK(dm->reset_acked && dm->reset_stopped && SK.dmuif_sets >= 1 &&
          !(S.reg[DMR(regMMHUBBUB_SOFT_RESET)] & MMHUBBUB_SOFT_RESET__DMUIF_SOFT_RESET_MASK) &&
          S.reg[DMR(regDMCUB_GPINT_DATAIN1)] == 0 && S.dmub.running);
    CHECK(dmub32_query_caps(dm) == RDNA3_OK);
    /* a hung firmware: no ack, no answer within 30 reads: reset forced,
     * the time spent is the 60 reads, not the 1 s of the verified path */
    {
        uint64_t t0 = S.now_us;
        S.reg[DMR(regDMCUB_SCRATCH7)] = 0;   /* the running firmware's own use of it */
        SK.gpint_hang = 1;
        dmub32_hw_reset(dm);
        CHECK(!dm->reset_acked && !dm->reset_stopped &&
              (S.reg[DMR(regDMCUB_CNTL2)] & DMCUB_CNTL2__DMCUB_SOFT_RESET_MASK) &&
              !(S.reg[DMR(regDMCUB_CNTL)] & DMCUB_CNTL__DMCUB_ENABLE_MASK) &&
              (S.reg[DMR(regMMHUBBUB_SOFT_RESET)] & MMHUBBUB_SOFT_RESET__DMUIF_SOFT_RESET_MASK));
        CHECK(S.now_us - t0 < 1000);
        SK.gpint_hang = 0;
    }
    CHECK(dmub32_hw_init(dm, S.vbios, VBIOS_LEN, 1, 100000) == RDNA3_OK && S.dmub.running &&
          !(S.reg[DMR(regMMHUBBUB_SOFT_RESET)] & MMHUBBUB_SOFT_RESET__DMUIF_SOFT_RESET_MASK));
    /* back to the verified sequence; DMUIF stays untouched by it */
    dm->amd_reset = 0;
    k = SK.dmuif_sets;
    CHECK(dmub32_hw_init(dm, S.vbios, VBIOS_LEN, 1, 100000) == RDNA3_OK && SK.dmuif_sets == k);
    CHECK(dmub32_query_caps(dm) == RDNA3_OK);
    S.dmub.stops = stops0;
    S.dmub.boots = boots0;
    SK.dmub_model = 0;
}

#define GAMUT_K(reg, n) (DCN_B3 + reg + (regMPCC_OGAM1_MPCC_GAMUT_REMAP_MODE - regMPCC_OGAM0_MPCC_GAMUT_REMAP_MODE) * (n))
#define OCSC_K(reg, n)  (DCN_B3 + reg + (regMPC_OUT1_CSC_MODE - regMPC_OUT0_CSC_MODE) * (n))
#define MUX_K(reg, n)   (DCN_B3 + reg + (regMPC_OUT1_MUX - regMPC_OUT0_MUX) * (n))
#define MPCC_K(reg, n)  (DCN_B3 + reg + (regMPCC1_MPCC_TOP_SEL - regMPCC0_MPCC_TOP_SEL) * (n))

static void test_dc_color(rdna3_dev *dev)
{
    rd_fixpt m[12];
    uint16_t r[12];
    rdna3_denorm_clamp cl;
    const uint16_t *t;
    uint32_t k, save[64], n = 0;
    /* the registers touched, saved and put back at the end */
    const uint32_t regs[] = {
        OCSC_K(regMPC_OUT0_CSC_MODE, 2), MUX_K(regMPC_OUT0_DENORM_CONTROL, 2), MUX_K(regMPC_OUT0_DENORM_CLAMP_G_Y, 2),
        MUX_K(regMPC_OUT0_DENORM_CLAMP_B_CB, 2), GAMUT_K(regMPCC_OGAM0_MPCC_GAMUT_REMAP_MODE, 3),
        MPCC_K(regMPCC0_MPCC_TOP_SEL, 3), MPCC_K(regMPCC0_MPCC_BOT_SEL, 3), MPCC_K(regMPCC0_MPCC_OPP_ID, 3),
        MPCC_K(regMPCC0_MPCC_UPDATE_LOCK_SEL, 3), MUX_K(regMPC_OUT0_MUX, 3), MUX_K(regMPC_OUT0_MUX, 1),
        DCN_B3 + regMPC_OUT_CSC_COEF_FORMAT,
    };
    for (k = 0; k < sizeof(regs) / sizeof(regs[0]); k++)
        save[n++] = S.reg[regs[k]];

    /* find_color_matrix: the 709 full-range table; none past the end */
    t = dcn32_find_color_matrix(RD_COLOR_SPACE_YCBCR709_TYPE);
    CHECK(t && t[0] == 0xE04 && t[1] == 0xF345 && t[11] == 0x1004);
    CHECK(!dcn32_find_color_matrix(99));

    /* mpc3_set_ocsc_default on OPP 2, set A: the six coefficient registers */
    S.reg[DCN_B3 + regMPC_OUT_CSC_COEF_FORMAT] = 0xf;
    CHECK(dcn32_mpc_set_ocsc_default(dev, 2, RD_COLOR_SPACE_YCBCR709_TYPE, RD_MPC_OUTPUT_CSC_COEF_A) == RDNA3_OK);
    CHECK(S.reg[DCN_B3 + regMPC_OUT_CSC_COEF_FORMAT] == 0 && S.reg[OCSC_K(regMPC_OUT0_CSC_MODE, 2)] == 1);
    CHECK(S.reg[OCSC_K(regMPC_OUT0_CSC_C11_C12_A, 2)] == (0xE04u | 0xF345u << 16) &&
          S.reg[OCSC_K(regMPC_OUT0_CSC_C33_C34_A, 2)] == (0xE04u | 0x1004u << 16));
    /* set B with limited-range RGB through mpc3_set_output_csc */
    CHECK(dcn32_mpc_set_output_csc(dev, 2, dcn32_find_color_matrix(RD_COLOR_SPACE_RGB_LIMITED_TYPE),
                                   RD_MPC_OUTPUT_CSC_COEF_B) == RDNA3_OK);
    CHECK(S.reg[OCSC_K(regMPC_OUT0_CSC_MODE, 2)] == 2 && S.reg[OCSC_K(regMPC_OUT0_CSC_C11_C12_B, 2)] == 0x1B67 &&
          S.reg[OCSC_K(regMPC_OUT0_CSC_C33_C34_B, 2)] == (0x1B67u | 0x201u << 16));
    CHECK(dcn32_mpc_set_output_csc(dev, 2, NULL, RD_MPC_OUTPUT_CSC_COEF_A) == RDNA3_EINVAL);
    CHECK(dcn32_mpc_set_output_csc(dev, 2, NULL, RD_MPC_OUTPUT_CSC_DISABLE) == RDNA3_OK &&
          S.reg[OCSC_K(regMPC_OUT0_CSC_MODE, 2)] == 0);

    /* mpc3_set_denorm: 10 bpc = mode 4; 16 bpc = bypass; the clamps */
    dcn32_mpc_set_denorm(dev, 2, RD_COLOR_DEPTH_101010);
    CHECK(((S.reg[MUX_K(regMPC_OUT0_DENORM_CONTROL, 2)] & MPC_OUT0_DENORM_CONTROL__MPC_OUT_DENORM_MODE_MASK) >>
           MPC_OUT0_DENORM_CONTROL__MPC_OUT_DENORM_MODE__SHIFT) == 4);
    dcn32_mpc_set_denorm(dev, 2, RD_COLOR_DEPTH_161616);
    CHECK(!(S.reg[MUX_K(regMPC_OUT0_DENORM_CONTROL, 2)] & MPC_OUT0_DENORM_CONTROL__MPC_OUT_DENORM_MODE_MASK));
    memset(&cl, 0, sizeof(cl));
    cl.clamp_max_r_cr = 0x3c0; cl.clamp_min_r_cr = 0x40; cl.clamp_max_g_y = 0x3ac; cl.clamp_min_g_y = 0x40;
    cl.clamp_max_b_cb = 0x3c0; cl.clamp_min_b_cb = 0x40;
    dcn32_mpc_set_denorm_clamp(dev, 2, &cl);
    k = S.reg[MUX_K(regMPC_OUT0_DENORM_CLAMP_G_Y, 2)];
    CHECK(((k & MPC_OUT0_DENORM_CLAMP_G_Y__MPC_OUT_DENORM_CLAMP_MAX_G_Y_MASK) >>
           MPC_OUT0_DENORM_CLAMP_G_Y__MPC_OUT_DENORM_CLAMP_MAX_G_Y__SHIFT) == 0x3ac &&
          ((k & MPC_OUT0_DENORM_CLAMP_G_Y__MPC_OUT_DENORM_CLAMP_MIN_G_Y_MASK) >>
           MPC_OUT0_DENORM_CLAMP_G_Y__MPC_OUT_DENORM_CLAMP_MIN_G_Y__SHIFT) == 0x40);

    /* convert_float_matrix: 1.0 = 0x2000, -1.0 = 0xE000 (S2.13 two's
     * complement with the sign bit), 3.5 clamped to 3.0, 0.5 = 0x1000 */
    memset(m, 0, sizeof(m));
    m[0] = dc_fixpt_from_int(1);
    m[1] = dc_fixpt_from_int(-1);
    m[2] = dc_fixpt_from_fraction(7, 2);
    m[3] = dc_fixpt_from_fraction(1, 2);
    m[4] = dc_fixpt_from_fraction(-7, 2);
    dcn32_convert_float_matrix(r, m, 12);
    CHECK(r[0] == 0x2000 && r[1] == 0xE000 && r[2] == 0x6000 && r[3] == 0x1000 && r[4] == 0xA000 && r[5] == 0);

    /* mpc3_set_gamut_remap on MPCC 3: identity into set A (after bypass),
     * the next one into set B, then A again, bypass */
    memset(m, 0, sizeof(m));
    m[0] = m[5] = m[10] = dc_fixpt_from_int(1);
    S.reg[GAMUT_K(regMPCC_OGAM0_MPCC_GAMUT_REMAP_MODE, 3)] = 0;
    dcn32_mpc_set_gamut_remap(dev, 3, m);
    CHECK((S.reg[GAMUT_K(regMPCC_OGAM0_MPCC_GAMUT_REMAP_MODE, 3)] & 3) == 1 &&
          S.reg[GAMUT_K(regMPCC_OGAM0_MPC_GAMUT_REMAP_C11_C12_A, 3)] == 0x2000 &&
          S.reg[GAMUT_K(regMPCC_OGAM0_MPC_GAMUT_REMAP_C11_C12_A, 3) + 2] == (0x2000u << 16) &&
          S.reg[GAMUT_K(regMPCC_OGAM0_MPC_GAMUT_REMAP_C33_C34_A, 3)] == 0x2000);
    m[0] = dc_fixpt_from_fraction(1, 2);
    dcn32_mpc_set_gamut_remap(dev, 3, m);
    CHECK((S.reg[GAMUT_K(regMPCC_OGAM0_MPCC_GAMUT_REMAP_MODE, 3)] & 3) == 2 &&
          S.reg[GAMUT_K(regMPCC_OGAM0_MPC_GAMUT_REMAP_C11_C12_B, 3)] == 0x1000);
    dcn32_mpc_set_gamut_remap(dev, 3, m);
    CHECK((S.reg[GAMUT_K(regMPCC_OGAM0_MPCC_GAMUT_REMAP_MODE, 3)] & 3) == 1);
    dcn32_mpc_set_gamut_remap(dev, 3, NULL);
    CHECK((S.reg[GAMUT_K(regMPCC_OGAM0_MPCC_GAMUT_REMAP_MODE, 3)] & 3) == 0);

    /* mpc3_mpc_init_single_inst on MPCC 3 routed to OPP 1: out of the
     * tree, OPP 1's output mux off, OPP 3's rate control off */
    S.reg[MPCC_K(regMPCC0_MPCC_TOP_SEL, 3)] = 3;
    S.reg[MPCC_K(regMPCC0_MPCC_BOT_SEL, 3)] = 0xf;
    S.reg[MPCC_K(regMPCC0_MPCC_OPP_ID, 3)] = 1;
    S.reg[MUX_K(regMPC_OUT0_MUX, 1)] = 3;
    S.reg[MUX_K(regMPC_OUT0_MUX, 3)] = 0xf | 5u << MPC_OUT0_MUX__MPC_OUT_FLOW_CONTROL_COUNT__SHIFT;
    dcn32_mpc_init_single_inst(dev, 3);
    CHECK(S.reg[MPCC_K(regMPCC0_MPCC_TOP_SEL, 3)] == 0xf && S.reg[MPCC_K(regMPCC0_MPCC_OPP_ID, 3)] == 0xf &&
          S.reg[MPCC_K(regMPCC0_MPCC_UPDATE_LOCK_SEL, 3)] == 0xf &&
          (S.reg[MUX_K(regMPC_OUT0_MUX, 1)] & MPC_OUT0_MUX__MPC_OUT_MUX_MASK) == 0xf &&
          (S.reg[MUX_K(regMPC_OUT0_MUX, 3)] & MPC_OUT0_MUX__MPC_OUT_RATE_CONTROL_DISABLE_MASK) &&
          !(S.reg[MUX_K(regMPC_OUT0_MUX, 3)] & MPC_OUT0_MUX__MPC_OUT_FLOW_CONTROL_COUNT_MASK));

    n = 0;
    for (k = 0; k < sizeof(regs) / sizeof(regs[0]); k++)
        S.reg[regs[k]] = save[n++];
}

static void test_dc_k(rdna3_dev *dev, rdna3_dmub *dm)
{
    test_dc_color(dev);

    test_dc_otg(dev);
    test_dc_dmub(dev, dm);
    printf("sim_navi32: display core K tests ran (%u GPINTs, %u inbox0 commands)\n", SK.gpints, SK.inbox0_cmds);
}
