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
#include "dcn32_hdmi.h"
#include "gen/dcn_3_2_0_hdmi.h"
#include "dcn32_hwseq.h"
#include "gen/dcn_3_2_0_hwseq.h"
#include "gen/dcn_3_2_0_dpsec.h"


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

/* ---- HDMI: EDID caps, infoframes, link encoder validation, the HDMI
 * stream of a TMDS mode set (rdna3hdmi) ---- */

#define DIGK(reg, fe) (DCN_B2 + (reg) + (regDIG1_HDMI_CONTROL - regDIG0_HDMI_CONTROL) * (fe))

/* a synthetic HDMI 2.0 sink: 1080p DTD; CTA block with VICs 16, 97, 95,
 * one LPCM SAD, a speaker allocation, the HDMI VSDB (300 MHz) and the
 * HDMI Forum VSDB (600 MHz, SCDC, LTE_340Mcsc_Scramble) */
static void hdmi_test_edid(uint8_t *b, uint16_t mfg, uint16_t product)
{
    static const uint8_t hdr[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    static const uint8_t dtd_1080p[18] = { 0x02, 0x3A, 0x80, 0x18, 0x71, 0x38, 0x2D, 0x40, 0x58,
                                           0x2C, 0x45, 0x00, 0xC4, 0x8E, 0x21, 0x00, 0x00, 0x1E };
    static const uint8_t cta[] = {
        0x43, 16, 97, 95,                                  /* video: VIC 16, 97, 95 */
        0x23, 0x09, 0x07, 0x07,                            /* audio: LPCM 2 ch, 32-48 kHz, 16-24 bit */
        0x83, 0x01, 0x00, 0x00,                            /* speaker allocation: FL/FR */
        0x67, 0x03, 0x0C, 0x00, 0x10, 0x00, 0x00, 0x3C,    /* HDMI VSDB: 1.0.0.0, 300 MHz */
        0x67, 0xD8, 0x5D, 0xC4, 0x01, 0x78, 0x88, 0x00,    /* HF-VSDB: 600 MHz, SCDC, LTE scramble */
    };
    uint32_t i, s;
    memset(b, 0, 256);
    memcpy(b, hdr, 8);
    b[8] = (uint8_t)(mfg >> 8);
    b[9] = (uint8_t)mfg;
    b[10] = (uint8_t)product;
    b[11] = (uint8_t)(product >> 8);
    b[18] = 1;
    b[19] = 4;
    b[20] = 0x80;
    b[24] = 0x02;
    for (i = 38; i < 54; i++)
        b[i] = 0x01;
    memcpy(b + 54, dtd_1080p, 18);
    b[75] = 0x10;   /* dummy descriptors */
    b[93] = 0x10;
    b[111] = 0x10;
    b[126] = 1;
    for (i = 0, s = 0; i < 127; i++)
        s += b[i];
    b[127] = (uint8_t)(0x100 - (s & 0xff));
    b[128] = 0x02;
    b[129] = 0x03;
    b[130] = (uint8_t)(4 + sizeof(cta));
    b[131] = 0x70;
    memcpy(b + 132, cta, sizeof(cta));
    for (i = 128, s = 0; i < 255; i++)
        s += b[i];
    b[255] = (uint8_t)(0x100 - (s & 0xff));
}

static uint8_t hdmi_sum(const rdna3_info_packet *p, uint32_t len)
{
    uint32_t i, s = p->hb0 + p->hb1 + p->hb2;
    for (i = 0; i <= len; i++)
        s += p->sb[i];
    return (uint8_t)s;
}

/* the AVI header / PB1-4 the VPG holds in generic packet 0 of front end 'fe' */
static int hdmi_vpg_avi(uint32_t fe, uint32_t vic, uint32_t m)
{
    uint32_t h = S.vpg[fe].mem[0], pb = S.vpg[fe].mem[1];
    return h == (0x82u | 2u << 8 | 13u << 16) && ((pb >> 8) & 0xff) == 0x1e &&
           ((pb >> 16) & 0xff) == (0x08u | m << 4) && (pb >> 24) == 0x80 && (S.vpg[fe].mem[2] & 0xff) == vic;
}

static void test_dc_hdmi(rdna3_dev *dev, rdna3_dmub *dm, rdna3_dcn *dc2, rdna3_dcn_scale *sc,
                         const rdna3_display_path *own)
{
    static rdna3_edid e;
    static rdna3_dcn_modeset ms;
    static rdna3_hdmi_info_frames f;
    uint8_t edid[256];
    const rdna3_mode *m1080 = NULL, *m4k60 = NULL, *m4k30 = NULL;
    rdna3_crtc_format rgb = { RD_PIXEL_ENCODING_RGB, RD_COLOR_DEPTH_888, 0, 0 }, fm;
    rdna3_link_enc_features ft;
    rdna3_hdmi_cfg hc;
    rdna3_mode up;
    uint32_t i, fe, ctl;
    const int sink0 = S.i2c_sink;
    static uint32_t dig_save[0x38];   /* the front end's AFMT .. AFMT_CNTL registers, restored at the end */

    /* dm_helpers_parse_edid_caps with DRM's CTA parsing */
    hdmi_test_edid(edid, 0x4D2D, 0x1234);   /* "SIM" */
    CHECK(edid_parse(edid, 256, &e) == 0 && e.hdmi && e.scdc_present && e.lte_340_scramble &&
          e.max_tmds_clk_mhz == 600 && e.audio_mode_count == 1 && e.audio_modes[0].format_code == 1 &&
          e.audio_modes[0].channel_count == 2 && e.audio_modes[0].sample_rate == 7 && e.speaker_flags == 1 &&
          e.panel_id == RD_EDID_PANEL_ID('S', 'I', 'M', 0x1234) && !e.quirk_disable_fams);
    /* apply_edid_quirks */
    hdmi_test_edid(edid, 0x4C2D, 0x0E5E);   /* SAM 0x0E5E */
    CHECK(edid_parse(edid, 256, &e) == 0 && e.quirk_disable_fams && !e.quirk_remove_sink_ext_caps);
    for (i = 0; i < e.nmodes; i++) {
        const rdna3_mode *m = &e.mode[i];
        if (m->h_active == 1920 && m->v_active == 1080 && m->origin == RD_MODE_FROM_DTD)
            m1080 = m;
        if (m->h_active == 3840 && m->pixel_khz == 594000)
            m4k60 = m;
        if (m->h_active == 3840 && m->pixel_khz == 297000 && m->h_total == 4400)
            m4k30 = m;
    }
    CHECK(m1080 && m4k60 && m4k30 && m4k60->origin == RD_MODE_FROM_CTA);
    if (!m1080 || !m4k60 || !m4k30)
        return;
    CHECK(edid_match_vic(m1080) == 16 && edid_match_vic(m4k60) == 97 && edid_match_vic(m4k30) == 95);

    /* resource_build_info_frame: AVI v2 with VIC, bars, checksum */
    dcn32_hdmi_build_info_frames(&f, m1080, &rgb, 1);
    CHECK(f.avi.valid && f.avi.hb0 == 0x82 && f.avi.hb1 == 2 && f.avi.hb2 == 13 && f.avi.sb[1] == 0x1e &&
          f.avi.sb[2] == 0x08 && f.avi.sb[3] == 0x80 && f.avi.sb[4] == 16 && f.avi.sb[5] == 0 &&
          f.avi.sb[8] == (1126 & 0xff) && f.avi.sb[9] == (1126 >> 8) && f.avi.sb[12] == (2201 & 0xff) &&
          f.avi.sb[13] == (2201 >> 8) && hdmi_sum(&f.avi, 13) == 0 && !f.vendor.valid && !f.spd.valid);
    /* a CTA mode: its picture aspect (16:9) */
    dcn32_hdmi_build_info_frames(&f, m4k60, &rgb, 1);
    CHECK(f.avi.sb[2] == 0x28 && f.avi.sb[4] == 97 && !f.vendor.valid);
    /* an HDMI VIC mode on an HDMI sink: AVI VIC 0, the HDMI VIC in the VSIF */
    dcn32_hdmi_build_info_frames(&f, m4k30, &rgb, 1);
    CHECK(f.avi.sb[4] == 0 && f.vendor.valid && f.vendor.hb0 == 0x81 && f.vendor.hb1 == 1 && f.vendor.hb2 == 5 &&
          f.vendor.sb[1] == 0x03 && f.vendor.sb[2] == 0x0c && f.vendor.sb[3] == 0 && f.vendor.sb[4] == 0x20 &&
          f.vendor.sb[5] == 1 && hdmi_sum(&f.vendor, 5) == 0);
    dcn32_hdmi_build_info_frames(&f, m4k30, &rgb, 0);
    CHECK(f.avi.sb[4] == 95 && !f.vendor.valid && hdmi_sum(&f.avi, 13) == 0);
    /* YCbCr 4:4:4 above 27.03 MHz: Y = 2, BT.709 */
    fm = rgb;
    fm.pixel_encoding = RD_PIXEL_ENCODING_YCBCR444;
    dcn32_hdmi_build_info_frames(&f, m1080, &fm, 1);
    CHECK(f.avi.sb[1] == (0x40 | 0x1e) && f.avi.sb[2] == (0x80 | 0x08) && hdmi_sum(&f.avi, 13) == 0);

    /* get_tmds_output_pixel_clock_100hz, dcn10_link_encoder_validate_output_with_stream */
    fm = rgb;
    fm.color_depth = RD_COLOR_DEPTH_101010;
    CHECK(dcn32_tmds_output_pixel_clock_100hz(5940000, &fm) == 7425000);
    fm.pixel_encoding = RD_PIXEL_ENCODING_YCBCR420;
    CHECK(dcn32_tmds_output_pixel_clock_100hz(5940000, &fm) == 3712500);
    dcn32_link_enc_features(RD_ATOM_ENC_CAP_HDMI6G, &ft);
    CHECK(ft.max_hdmi_pixel_clock == 600000 && ft.max_hdmi_deep_color == RD_COLOR_DEPTH_121212 && ft.hdmi_6gb_en);
    CHECK(dcn32_link_enc_validate_output(&ft, RD_SIGNAL_HDMI_TYPE_A, RD_SIGNAL_HDMI_TYPE_A, &rgb, 594000, 594000, 600));
    CHECK(!dcn32_link_enc_validate_output(&ft, RD_SIGNAL_HDMI_TYPE_A, RD_SIGNAL_HDMI_TYPE_A, &rgb, 594000, 594000, 300));
    CHECK(!dcn32_link_enc_validate_output(&ft, RD_SIGNAL_HDMI_TYPE_A, RD_SIGNAL_HDMI_TYPE_A, &rgb, 600001, 600001, 0));
    CHECK(!dcn32_link_enc_validate_output(&ft, RD_SIGNAL_HDMI_TYPE_A, RD_SIGNAL_HDMI_TYPE_A, &rgb, 20000, 20000, 0));
    fm = rgb;
    fm.color_depth = RD_COLOR_DEPTH_161616;
    CHECK(!dcn32_link_enc_validate_output(&ft, RD_SIGNAL_HDMI_TYPE_A, RD_SIGNAL_HDMI_TYPE_A, &fm, 148500, 297000, 0));
    dcn32_link_enc_features(0, &ft);   /* no HDMI 6G record: below 300 MHz */
    CHECK(!dcn32_link_enc_validate_output(&ft, RD_SIGNAL_HDMI_TYPE_A, RD_SIGNAL_HDMI_TYPE_A, &rgb, 300000, 300000, 0) &&
          dcn32_link_enc_validate_output(&ft, RD_SIGNAL_HDMI_TYPE_A, RD_SIGNAL_HDMI_TYPE_A, &rgb, 297000, 297000, 0));
    CHECK(dcn32_link_enc_validate_output(&ft, RD_SIGNAL_DVI_SINGLE_LINK, RD_SIGNAL_DVI_SINGLE_LINK, &rgb, 165000, 0, 0) &&
          !dcn32_link_enc_validate_output(&ft, RD_SIGNAL_DVI_SINGLE_LINK, RD_SIGNAL_DVI_SINGLE_LINK, &rgb, 165001, 0, 0) &&
          dcn32_link_enc_validate_output(&ft, RD_SIGNAL_DVI_DUAL_LINK, RD_SIGNAL_DVI_DUAL_LINK, &rgb, 330000, 0, 0));
    fm = rgb;
    fm.color_depth = RD_COLOR_DEPTH_101010;
    CHECK(!dcn32_link_enc_validate_output(&ft, RD_SIGNAL_DVI_SINGLE_LINK, RD_SIGNAL_DVI_SINGLE_LINK, &fm, 100000, 0, 0));
    fm.pixel_encoding = RD_PIXEL_ENCODING_YCBCR420;
    CHECK(dcn32_link_enc_validate_output(&ft, RD_SIGNAL_DISPLAY_PORT, RD_SIGNAL_DISPLAY_PORT, &fm, 594000, 0, 0));

    /* the plan on the lit HDMI head (DIGC, OTG 2, pipe 1, at 1440p) */
    fe = (uint32_t)dc2->head[0].dig_fe;
    for (i = 0; i < 0x38; i++)
        dig_save[i] = S.reg[DIGK(regAFMT0_AFMT_INFOFRAME_CONTROL0 + i, fe)];
    memset(&hc, 0, sizeof(hc));
    hc.flags = RD_HDMI_STREAM;
    hc.ddc = 0;
    hc.enc_caps = RD_ATOM_ENC_CAP_HDMI6G;
    hc.hdmi_sink = 1;
    hc.lte_340_scramble = 1;
    hc.max_tmds_clk_mhz = 600;
    CHECK(dcn32_modeset_plan_hdmi(dev, dc2, 0, own, &hc, 241500, m4k60, NULL, &ms) == RD_MODESET_TMDS_CLOCK);
    CHECK(dcn32_modeset_plan_hdmi(dev, dc2, 0, own, NULL, 241500, m4k60, NULL, &ms) == RD_MODESET_TMDS_CLOCK);
    hc.flags = RD_HDMI_STREAM | RD_HDMI_20;
    hc.enc_caps = 0;
    CHECK(dcn32_modeset_plan_hdmi(dev, dc2, 0, own, &hc, 241500, m4k60, NULL, &ms) == RD_MODESET_TMDS_CLOCK);
    hc.enc_caps = RD_ATOM_ENC_CAP_HDMI6G;
    hc.max_tmds_clk_mhz = 300;
    CHECK(dcn32_modeset_plan_hdmi(dev, dc2, 0, own, &hc, 241500, m4k60, NULL, &ms) == RD_MODESET_TMDS_CLOCK);
    hc.max_tmds_clk_mhz = 600;
    CHECK(dcn32_modeset_plan_hdmi(dev, dc2, 0, own, &hc, 241500, m4k60, NULL, &ms) == RD_MODESET_OK &&
          ms.hdmi.flags == (RD_HDMI_STREAM | RD_HDMI_20) && ms.tmds_want.pixel_khz == 594000 &&
          ms.tmds_cur.h_active == 2560 && ms.tmds_cur.pixel_khz == 241500);
    /* a DVI head (DIG_MODE 2) under the configuration: single link, 165 MHz */
    dc2->head[0].dig_mode = 2;
    CHECK(dcn32_modeset_plan_hdmi(dev, dc2, 0, own, &hc, 241500, m1080, NULL, &ms) == RD_MODESET_OK);
    dc2->head[0].dig_mode = 3;

    /* the mode set to 1080p: DC's HDMI stream, SCDC on DDC1 (the sink's
     * SCDC version 1: the source version is written; LTE scramble) */
    S.i2c_sink = 0;
    S.scdc_on = 1;
    memset(S.scdc, 0, sizeof(S.scdc));
    S.scdc[0x01] = 1;
    S.scdc_writes = 0;
    memset(S.vpg, 0, sizeof(S.vpg));
    SK.otg_model = 1;
    S.dcn_frames = 1;
    S.dmub.nseq = 0;
    S.reg[DIGK(regDIG0_HDMI_GC, fe)] = DIG0_HDMI_GC__HDMI_GC_AVMUTE_MASK;   /* left muted: AV mute goes off */
    hc.flags = RD_HDMI_STREAM;
    CHECK(dcn32_modeset_plan_hdmi(dev, dc2, 0, own, &hc, 241500, m1080, NULL, &ms) == RD_MODESET_OK);
    CHECK(dcn32_modeset_apply(dev, dm, dc2, sc, &ms, FB_START + 0x8000000, 1920, FB_START, 2560, 50) == RDNA3_OK);
    CHECK(S.dmub.nseq == 4 && dc2->head[0].width == 1920);
    ctl = S.reg[DIGK(regDIG0_HDMI_CONTROL, fe)];
    CHECK((ctl & DIG0_HDMI_CONTROL__HDMI_PACKET_GEN_VERSION_MASK) && (ctl & DIG0_HDMI_CONTROL__HDMI_KEEPOUT_MODE_MASK) &&
          (ctl & DIG0_HDMI_CONTROL__HDMI_NO_EXTRA_NULL_PACKET_FILLED_MASK) &&
          (ctl & DIG0_HDMI_CONTROL__HDMI_DATA_SCRAMBLE_EN_MASK) && !(ctl & DIG0_HDMI_CONTROL__HDMI_CLOCK_CHANNEL_RATE_MASK) &&
          !(ctl & DIG0_HDMI_CONTROL__HDMI_DEEP_COLOR_ENABLE_MASK) && !(ctl & DIG0_HDMI_CONTROL__HDMI_DEEP_COLOR_DEPTH_MASK));
    CHECK(S.scdc[0x02] == 1 && S.scdc[0x20] == 1 && S.scdc_writes == 4);
    CHECK((S.reg[DIGK(regDIG0_HDMI_VBI_PACKET_CONTROL, fe)] &
           (DIG0_HDMI_VBI_PACKET_CONTROL__HDMI_GC_SEND_MASK | DIG0_HDMI_VBI_PACKET_CONTROL__HDMI_GC_CONT_MASK |
            DIG0_HDMI_VBI_PACKET_CONTROL__HDMI_NULL_SEND_MASK | DIG0_HDMI_VBI_PACKET_CONTROL__HDMI_ACP_SEND_MASK)) ==
          (DIG0_HDMI_VBI_PACKET_CONTROL__HDMI_GC_SEND_MASK | DIG0_HDMI_VBI_PACKET_CONTROL__HDMI_GC_CONT_MASK |
           DIG0_HDMI_VBI_PACKET_CONTROL__HDMI_NULL_SEND_MASK));
    CHECK((S.reg[DIGK(regDIG0_HDMI_INFOFRAME_CONTROL0, fe)] & DIG0_HDMI_INFOFRAME_CONTROL0__HDMI_AUDIO_INFO_SEND_MASK) &&
          ((S.reg[DIGK(regDIG0_HDMI_INFOFRAME_CONTROL1, fe)] & DIG0_HDMI_INFOFRAME_CONTROL1__HDMI_AUDIO_INFO_LINE_MASK) >>
           DIG0_HDMI_INFOFRAME_CONTROL1__HDMI_AUDIO_INFO_LINE__SHIFT) == 2 &&
          (S.reg[DCN_B2 + regAFMT0_AFMT_INFOFRAME_CONTROL0 + (regAFMT1_AFMT_INFOFRAME_CONTROL0 - regAFMT0_AFMT_INFOFRAME_CONTROL0) * fe] &
           AFMT0_AFMT_INFOFRAME_CONTROL0__AFMT_AUDIO_INFO_UPDATE_MASK));
    /* the AVI infoframe in generic packet 0, every frame on line 2; no vendor frame */
    CHECK((S.reg[DIGK(regDIG0_HDMI_GENERIC_PACKET_CONTROL0, fe)] & 0xff) == 0x03 &&
          (S.reg[DIGK(regDIG0_HDMI_GENERIC_PACKET_CONTROL1, fe)] & 0xffff) == 2 &&
          (S.reg[DIGK(regDIG0_HDMI_DB_CONTROL, fe)] & DIG0_HDMI_DB_CONTROL__HDMI_DB_DISABLE_MASK) &&
          (S.reg[DIGK(regDIG0_AFMT_CNTL, fe)] & DIG0_AFMT_CNTL__AFMT_AUDIO_CLOCK_EN_MASK) && hdmi_vpg_avi(fe, 16, 0) &&
          !(S.reg[DIGK(regDIG0_HDMI_GC, fe)] & DIG0_HDMI_GC__HDMI_GC_AVMUTE_MASK));

    /* 4K60 at 594 MHz (rdna3hdmi bit 1): scrambler, 1/4 clock channel,
     * TMDS_CONFIG 3; the CTA mode's 16:9 in the AVI frame */
    hc.flags = RD_HDMI_STREAM | RD_HDMI_20;
    S.dmub.nseq = 0;
    CHECK(dcn32_modeset_plan_hdmi(dev, dc2, 0, own, &hc, 148500, m4k60, NULL, &ms) == RD_MODESET_OK);
    CHECK(dcn32_modeset_apply(dev, dm, dc2, sc, &ms, FB_START + 0x8000000, 3840, FB_START, 1920, 50) == RDNA3_OK);
    ctl = S.reg[DIGK(regDIG0_HDMI_CONTROL, fe)];
    CHECK(S.dmub.nseq == 4 && dc2->head[0].width == 3840 && (ctl & DIG0_HDMI_CONTROL__HDMI_DATA_SCRAMBLE_EN_MASK) &&
          (ctl & DIG0_HDMI_CONTROL__HDMI_CLOCK_CHANNEL_RATE_MASK) && S.scdc[0x20] == 3 && S.scdc[0x21] == 1 &&
          hdmi_vpg_avi(fe, 97, 2));

    /* a mode that does not run: the running one back, scrambling with it */
    up = *m1080;
    S.dcn_frames = 0;
    CHECK(dcn32_modeset_plan_hdmi(dev, dc2, 0, own, &hc, 594000, &up, NULL, &ms) == RD_MODESET_OK);
    CHECK(dcn32_modeset_apply(dev, dm, dc2, sc, &ms, FB_START, 1920, FB_START + 0x8000000, 3840, 50) == RDNA3_EIO);
    ctl = S.reg[DIGK(regDIG0_HDMI_CONTROL, fe)];
    CHECK(dc2->head[0].width == 3840 && (ctl & DIG0_HDMI_CONTROL__HDMI_CLOCK_CHANNEL_RATE_MASK) && S.scdc[0x20] == 3 &&
          hdmi_vpg_avi(fe, 97, 0));
    S.dcn_frames = 1;

    /* back to the GOP's 1440p as before the test, without the HDMI stream:
     * the old path zeroes the packet controls */
    memset(&up, 0, sizeof(up));
    up.h_active = 2560; up.v_active = 1440; up.h_total = 2720; up.v_total = 1481;
    up.h_front = 48; up.h_sync = 32; up.v_front = 3; up.v_sync = 5; up.pixel_khz = 241500;
    up.h_sync_pos = up.v_sync_pos = 1;
    hc.flags = RD_HDMI_STREAM;
    CHECK(dcn32_modeset_plan_hdmi(dev, dc2, 0, own, &hc, 594000, &up, NULL, &ms) == RD_MODESET_OK);
    /* with rdna3dcseq: apply_symclk_on_tx_off_wa (the running 594 MHz
     * again after TX DISABLE, DISABLE again once the OTG stopped), then the
     * DIO FIFO resync (DENTIST WDIVIDER = RDIVIDER) */
    ms.dc_seq = 1;
    S.dmub.nseq = 0;
    S.reg[DCN_B1 + regDENTIST_DISPCLK_CNTL] = 0x18u | 0x1au << 8;
    CHECK(dcn32_modeset_apply(dev, dm, dc2, sc, &ms, FB_START, 2560, FB_START + 0x8000000, 3840, 50) == RDNA3_OK &&
          dc2->head[0].width == 2560 && S.scdc[0x20] == 1 &&
          !(S.reg[DIGK(regDIG0_HDMI_CONTROL, fe)] & DIG0_HDMI_CONTROL__HDMI_CLOCK_CHANNEL_RATE_MASK));
    CHECK(S.dmub.nseq == 6 && S.dmub.seq[0] == RD_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL &&
          S.dmub.seq_tx_action[0] == RD_ATOM_TX_DISABLE && S.dmub.seq[1] == RD_DMUB_VBIOS_SET_PIXEL_CLOCK &&
          S.dmub.seq[2] == RD_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL && S.dmub.seq_tx_action[2] == RD_ATOM_TX_DISABLE &&
          S.dmub.seq[3] == RD_DMUB_VBIOS_SET_PIXEL_CLOCK && S.dmub.seq[4] == RD_DMUB_VBIOS_DIGX_ENCODER_CONTROL &&
          S.dmub.seq[5] == RD_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL && S.dmub.seq_tx_action[5] == RD_ATOM_TX_ENABLE);
    CHECK((S.reg[DCN_B1 + regDENTIST_DISPCLK_CNTL] & DENTIST_DISPCLK_CNTL__DENTIST_DISPCLK_WDIVIDER_MASK) == 0x1a);
    S.reg[DCN_B1 + regDENTIST_DISPCLK_CNTL] = 0;
    dcn32_enc_stop_hdmi_info_packets(dev, fe);
    dcn32_enc_reset_hdmi_stream_attribute(dev, fe);
    CHECK(S.reg[DIGK(regDIG0_HDMI_GENERIC_PACKET_CONTROL0, fe)] == 0 &&
          !(S.reg[DIGK(regDIG0_HDMI_CONTROL, fe)] & DIG0_HDMI_CONTROL__HDMI_DATA_SCRAMBLE_EN_MASK));
    for (i = 0; i < 0x38; i++)
        S.reg[DIGK(regAFMT0_AFMT_INFOFRAME_CONTROL0 + i, fe)] = dig_save[i];
    S.i2c_sink = sink0;
    S.scdc_on = 0;
    SK.otg_model = 0;
    S.dcn_frames = 0;
    printf("sim_navi32: HDMI stream tests ran (%u SCDC writes)\n", S.scdc_writes);
}

/* ---- DP: the source specific data, AUX timeouts, the vendor ids, a
 * DP-to-HDMI converter's caps and mode validation, the DSC disconnect wait ---- */
static void test_dc_dp(rdna3_dev *dev, rdna3_dmub *dm)
{
    static rdna3_dp_link l;
    static uint8_t dpcd_save[sizeof(S.dpcd)];
    const uint32_t rx1 = S.reg[AUXR(regDP_AUX0_AUX_DPHY_RX_CONTROL1, 0)];
    rdna3_mode m1080, m4k;
    uint32_t v;

    memcpy(dpcd_save, S.dpcd, sizeof(S.dpcd));
    /* dce_aux_configure_timeout: 3.2 ms = 100 x 32 us, 552 us = 69 x 8 us */
    CHECK(dcaux_configure_timeout(dev, 0, RD_LINK_AUX_DEFAULT_LTTPR_TIMEOUT_PERIOD) ==
          ((rx1 & DP_AUX0_AUX_DPHY_RX_CONTROL1__AUX_RX_TIMEOUT_LEN_MASK) >> DP_AUX0_AUX_DPHY_RX_CONTROL1__AUX_RX_TIMEOUT_LEN__SHIFT) *
              (8u << ((rx1 & DP_AUX0_AUX_DPHY_RX_CONTROL1__AUX_RX_TIMEOUT_LEN_MUL_MASK) >>
                      DP_AUX0_AUX_DPHY_RX_CONTROL1__AUX_RX_TIMEOUT_LEN_MUL__SHIFT)));
    v = S.reg[AUXR(regDP_AUX0_AUX_DPHY_RX_CONTROL1, 0)];
    CHECK(((v & DP_AUX0_AUX_DPHY_RX_CONTROL1__AUX_RX_TIMEOUT_LEN_MASK) >> DP_AUX0_AUX_DPHY_RX_CONTROL1__AUX_RX_TIMEOUT_LEN__SHIFT) == 100 &&
          ((v & DP_AUX0_AUX_DPHY_RX_CONTROL1__AUX_RX_TIMEOUT_LEN_MUL_MASK) >> DP_AUX0_AUX_DPHY_RX_CONTROL1__AUX_RX_TIMEOUT_LEN_MUL__SHIFT) == 2);
    CHECK(dcaux_configure_timeout(dev, 0, 5000) == 3200 &&
          ((S.reg[AUXR(regDP_AUX0_AUX_DPHY_RX_CONTROL1, 0)] & DP_AUX0_AUX_DPHY_RX_CONTROL1__AUX_RX_TIMEOUT_LEN_MASK) >>
           DP_AUX0_AUX_DPHY_RX_CONTROL1__AUX_RX_TIMEOUT_LEN__SHIFT) == 79);   /* ceil(5000 / 64) */

    /* retrieve_link_cap on the sim's DP sink: AMD's source OUI and device
     * id, DCN 3.2's minimum h blank, the vendor ids, then the 552 us timeout */
    S.dpcd[0x500] = 0x00; S.dpcd[0x501] = 0x1c; S.dpcd[0x502] = 0xf8;   /* synthetic branch OUI */
    memcpy(&S.dpcd[0x503], "SIMBR1", 6);
    memcpy(&S.dpcd[0x403], "SIMSNK", 6);
    S.dpcd[0x409] = 0x21; S.dpcd[0x40a] = 3; S.dpcd[0x40b] = 7;
    dcn32_dp_link_init(&l, dev, dm, 0, 1, 2, 2, 0, 0x13, 0x1e, 0, 0);
    l.chip_id = 0x747e;
    CHECK(dcn32_dp_read_caps(&l) == RDNA3_OK && l.caps_valid);
    CHECK(S.dpcd[0x300] == 0x00 && S.dpcd[0x301] == 0x00 && S.dpcd[0x302] == 0x1a && S.dpcd[0x303] == 0x7e &&
          S.dpcd[0x304] == 0x74 && S.dpcd[0x305] == 0 && S.dpcd[0x309] == 26 && S.dpcd[0x340] == 80);
    CHECK(l.dpcd_caps.branch_dev_id == 0x001cf8 && !memcmp(l.dpcd_caps.branch_dev_name, "SIMBR1", 6) &&
          !memcmp(l.dpcd_caps.sink_dev_id_str, "SIMSNK", 6) && l.dpcd_caps.sink_hw_revision == 0x21 &&
          l.dpcd_caps.sink_fw_revision[0] == 3 && l.dpcd_caps.sink_fw_revision[1] == 7);
    v = S.reg[AUXR(regDP_AUX0_AUX_DPHY_RX_CONTROL1, 0)];
    CHECK(((v & DP_AUX0_AUX_DPHY_RX_CONTROL1__AUX_RX_TIMEOUT_LEN_MASK) >> DP_AUX0_AUX_DPHY_RX_CONTROL1__AUX_RX_TIMEOUT_LEN__SHIFT) == 69 &&
          !(v & DP_AUX0_AUX_DPHY_RX_CONTROL1__AUX_RX_TIMEOUT_LEN_MUL_MASK));
    CHECK(!dcn32_dp_is_branch_device(&l) && !dcn32_dp_is_active_dongle(&l) && !l.keep_receiver_powered);
    /* dpcd_read_sink_ext_caps */
    CHECK(l.dpcd_sink_ext_caps == S.dpcd[0x317] && !l.dpcd_caps.panel_luminance_control);

    /* a DP-to-HDMI converter: 300 MHz TMDS, 10 bpc, 4:2:x pass-through */
    S.dpcd[0x005] = 0x01 | 2u << 1 | 0x10;    /* port present, DVI/HDMI/DP++, detailed caps */
    S.dpcd[0x080] = 3;                        /* HDMI */
    S.dpcd[0x081] = 120;                      /* x 2.5 MHz */
    S.dpcd[0x082] = 0x01;                     /* 10 bpc, no FRL */
    S.dpcd[0x083] = 0x06;
    CHECK(dcn32_dp_read_caps(&l) == RDNA3_OK && dcn32_dp_is_branch_device(&l) && dcn32_dp_is_active_dongle(&l) &&
          l.dpcd_caps.dongle_type == RD_DONGLE_DP_HDMI && l.dpcd_caps.dongle_caps.extended_cap_valid &&
          l.dpcd_caps.dongle_caps.dp_hdmi_max_pixel_clk_in_khz == 300000 && l.dpcd_caps.dongle_caps.dp_hdmi_max_bpc == 10 &&
          l.dpcd_caps.dongle_caps.is_dp_hdmi_ycbcr422_pass_through && l.dpcd_caps.dongle_caps.is_dp_hdmi_ycbcr420_pass_through);
    CHECK(edid_dmt(1920, 1080, 60, &m1080) == 0);
    memset(&m4k, 0, sizeof(m4k));
    m4k.h_active = 3840; m4k.v_active = 2160; m4k.h_total = 4400; m4k.v_total = 2250;
    m4k.h_front = 176; m4k.h_sync = 88; m4k.v_front = 8; m4k.v_sync = 10; m4k.pixel_khz = 594000;
    CHECK(dcn32_dp_validate_mode_timing(&l, &m1080) && !dcn32_dp_validate_mode_timing(&l, &m4k));
    /* the DFP capability extension caps the active size */
    S.dpcd[0x081] = 240;                      /* 600 MHz: the TMDS limit off */
    S.dpcd[0x0a3] = 1;
    S.dpcd[0x0a4] = 0x58; S.dpcd[0x0a5] = 0x02;   /* 600 Mpix/s */
    S.dpcd[0x0a6] = 0x80; S.dpcd[0x0a7] = 0x07;   /* 1920 wide */
    S.dpcd[0x0a8] = 0x38; S.dpcd[0x0a9] = 0x04;   /* 1080 high */
    S.dpcd[0x0aa] = 0x01; S.dpcd[0x0ab] = 0x02;   /* RGB, 8 bpc */
    CHECK(dcn32_dp_read_caps(&l) == RDNA3_OK && l.dpcd_caps.dongle_caps.dfp_cap_ext.supported &&
          l.dpcd_caps.dongle_caps.dfp_cap_ext.max_video_h_active_width == 1920 &&
          dcn32_dp_validate_mode_timing(&l, &m1080) && !dcn32_dp_validate_mode_timing(&l, &m4k));
    S.dpcd[0x0ab] = 0x04;                     /* 10 bpc only: RGB 8 bpc refused */
    CHECK(dcn32_dp_read_caps(&l) == RDNA3_OK && !dcn32_dp_validate_mode_timing(&l, &m1080));
    /* a 0010FA VGA converter keeps its receiver powered */
    S.dpcd[0x005] = 0x01 | 1u << 1;
    S.dpcd[0x080] = 1;
    S.dpcd[0x500] = 0x00; S.dpcd[0x501] = 0x10; S.dpcd[0x502] = 0xfa;
    CHECK(dcn32_dp_read_caps(&l) == RDNA3_OK && l.dpcd_caps.dongle_type == RD_DONGLE_DP_VGA && l.keep_receiver_powered);
    memcpy(S.dpcd, dpcd_save, sizeof(S.dpcd));
    S.reg[AUXR(regDP_AUX0_AUX_DPHY_RX_CONTROL1, 0)] = rx1;
    dcaux_configure_timeout(dev, 0, 0);   /* the software poll back to its default */
    S.reg[AUXR(regDP_AUX0_AUX_DPHY_RX_CONTROL1, 0)] = rx1;

    /* the DP secondary data packets: a VSC SDP through VPG packets 0 and 1,
     * GSP0 and the master enable on; stopped again; the pixel format read */
    {
        static rdna3_dp_info_frames f;
        const uint32_t sec = S.reg[DPR(regDP0_DP_SEC_CNTL, 2)], pf = S.reg[DPR(regDP0_DP_PIXEL_FORMAT, 2)];
        uint32_t enc = 9, depth = 9;
        memset(&f, 0, sizeof(f));
        memset(S.vpg, 0, sizeof(S.vpg));
        f.vsc.valid = 1;
        f.vsc.hb1 = 0x07; f.vsc.hb2 = 0x05; f.vsc.hb3 = 0x13;   /* VSC rev 5 */
        f.vsc.sb[16] = 0x11;
        f.adaptive_sync_line_num_valid = 1;
        f.adaptive_sync_line_num = 7;
        dcn32_dp_link_init(&l, dev, dm, 0, 1, 2, 2, 0, 0x13, 0x1e, 0, 0);
        S.reg[DPR(regDP0_DP_SEC_CNTL, 2)] = 0;
        dcn32_enc_update_dp_info_packets_sdp_line_num(&l, &f);   /* no adaptive sync packet: nothing */
        CHECK((S.reg[DPR(regDP0_DP_SEC_CNTL5, 2)] & DP0_DP_SEC_CNTL5__DP_SEC_GSP5_LINE_NUM_MASK) != 7);
        dcn32_enc_update_dp_info_packets(&l, &f);
        CHECK((S.reg[DPR(regDP0_DP_SEC_CNTL, 2)] & (DP0_DP_SEC_CNTL__DP_SEC_GSP0_ENABLE_MASK | DP0_DP_SEC_CNTL__DP_SEC_STREAM_ENABLE_MASK)) ==
                  (DP0_DP_SEC_CNTL__DP_SEC_GSP0_ENABLE_MASK | DP0_DP_SEC_CNTL__DP_SEC_STREAM_ENABLE_MASK) &&
              !(S.reg[DPR(regDP0_DP_SEC_CNTL, 2)] & DP0_DP_SEC_CNTL__DP_SEC_GSP2_ENABLE_MASK) &&
              S.vpg[2].mem[0] == (0x07u << 8 | 0x05u << 16 | 0x13u << 24) && S.vpg[2].mem[9] == S.vpg[2].mem[0] &&
              (S.vpg[2].mem[5] & 0xff) == 0x11 && S.vpg[2].writes == 3 * 9);
        f.vsc.valid = 0;
        dcn32_enc_update_dp_info_packets(&l, &f);
        CHECK(S.reg[DPR(regDP0_DP_SEC_CNTL, 2)] == DP0_DP_SEC_CNTL__DP_SEC_STREAM_ENABLE_MASK);
        S.reg[DPR(regDP0_DP_PIXEL_FORMAT, 2)] = 2u | 2u << DP0_DP_PIXEL_FORMAT__DP_COMPONENT_DEPTH__SHIFT;
        CHECK(dcn32_enc_dp_get_pixel_format(&l, &enc, &depth) && enc == RD_PIXEL_ENCODING_YCBCR444 &&
              depth == RD_COLOR_DEPTH_101010 && !dcn32_enc_dp_get_pixel_format(&l, NULL, &depth));
        S.reg[DPR(regDP0_DP_SEC_CNTL, 2)] = sec;
        S.reg[DPR(regDP0_DP_PIXEL_FORMAT, 2)] = pf;
        memset(S.vpg, 0, sizeof(S.vpg));
    }

    /* dsc2_wait_disconnect_pending_clear */
    v = S.reg[DCN_B2 + regDSCRM0_DSCRM_DSC_FORWARD_CONFIG + 1];
    S.reg[DCN_B2 + regDSCRM0_DSCRM_DSC_FORWARD_CONFIG + 1] = v & ~DSCRM0_DSCRM_DSC_FORWARD_CONFIG__DSCRM_DSC_DOUBLE_BUFFER_REG_UPDATE_PENDING_MASK;
    CHECK(dcn32_dsc_wait_disconnect_pending_clear(dev, 1) == RDNA3_OK);
    S.reg[DCN_B2 + regDSCRM0_DSCRM_DSC_FORWARD_CONFIG + 1] |= DSCRM0_DSCRM_DSC_FORWARD_CONFIG__DSCRM_DSC_DOUBLE_BUFFER_REG_UPDATE_PENDING_MASK;
    CHECK(dcn32_dsc_wait_disconnect_pending_clear(dev, 1) == RDNA3_ETIME);
    S.reg[DCN_B2 + regDSCRM0_DSCRM_DSC_FORWARD_CONFIG + 1] = v;
}

/* ---- dcn32_hwseq.c ---- */
static void test_dc_hwseq(rdna3_dev *dev)
{
    rdna3_mode m;
    rdna3_dc_clocks a, b;
    uint32_t k1, k2, i, save[10], status[8];
    static const uint32_t pg[8] = { regDOMAIN0_PG_CONFIG, regDOMAIN1_PG_CONFIG, regDOMAIN2_PG_CONFIG, regDOMAIN3_PG_CONFIG,
                                    regDOMAIN16_PG_CONFIG, regDOMAIN17_PG_CONFIG, regDOMAIN18_PG_CONFIG, regDOMAIN19_PG_CONFIG };

    /* dcn32_calculate_dccg_k1_k2_values */
    CHECK(edid_dmt(1920, 1080, 60, &m) == 0);
    dcn32_calculate_dccg_k1_k2_values(RD_K12_SIGNAL_TMDS, &m, 0, 0, 1, &k1, &k2);
    CHECK(k1 == RD_PIXEL_RATE_DIV_BY_1 && k2 == RD_PIXEL_RATE_DIV_BY_4);
    dcn32_calculate_dccg_k1_k2_values(RD_K12_SIGNAL_TMDS, &m, 1, 0, 1, &k1, &k2);
    CHECK(k2 == RD_PIXEL_RATE_DIV_BY_2);
    /* DP: the DIG pixel rate div policy for an even h timing */
    CHECK(dcn32_is_dp_dig_pixel_rate_div_policy(RD_K12_SIGNAL_DP, &m) &&
          !dcn32_is_dp_dig_pixel_rate_div_policy(RD_K12_SIGNAL_TMDS, &m));
    dcn32_calculate_dccg_k1_k2_values(RD_K12_SIGNAL_DP, &m, 0, 0, 1, &k1, &k2);
    CHECK(k1 == RD_PIXEL_RATE_DIV_BY_1 && k2 == RD_PIXEL_RATE_DIV_BY_2);
    m.h_sync = 45;
    dcn32_calculate_dccg_k1_k2_values(RD_K12_SIGNAL_DP, &m, 0, 0, 1, &k1, &k2);
    CHECK(k2 == RD_PIXEL_RATE_DIV_BY_4 && !dcn32_is_dp_dig_pixel_rate_div_policy(RD_K12_SIGNAL_DP, &m));
    CHECK(dcn32_calculate_dccg_k1_k2_values(RD_K12_SIGNAL_DP, &m, 0, 0, 2, &k1, &k2) == 2 && k2 == RD_PIXEL_RATE_DIV_BY_2);
    dcn32_calculate_dccg_k1_k2_values(RD_K12_SIGNAL_DP, &m, 0, 1, 1, &k1, &k2);
    CHECK(k2 == RD_PIXEL_RATE_DIV_BY_2);
    dcn32_calculate_dccg_k1_k2_values(RD_K12_SIGNAL_DP_128B_132B, &m, 0, 0, 1, &k1, &k2);
    CHECK(k1 == RD_PIXEL_RATE_DIV_BY_1 && k2 == RD_PIXEL_RATE_DIV_BY_1);

    /* dcn32_get_dispclk_from_dentist, dccg32_trigger_dio_fifo_resync */
    save[0] = S.reg[DCN_B1 + regDENTIST_DISPCLK_CNTL];
    S.reg[DCN_B1 + regDENTIST_DISPCLK_CNTL] = 0x18;          /* divider 6.0 */
    CHECK(dcn32_get_dispclk_from_dentist(dev, 4300000) == 4u * 4300000 / 24);
    CHECK(dcn32_dccg_trigger_dio_fifo_resync(dev) == 0 && S.reg[DCN_B1 + regDENTIST_DISPCLK_CNTL] == 0x18);
    S.reg[DCN_B1 + regDENTIST_DISPCLK_CNTL] = 0x18 | 0x20u << 8;
    CHECK(dcn32_resync_fifo_dccg_dio(dev) == 0x20 && S.reg[DCN_B1 + regDENTIST_DISPCLK_CNTL] == (0x20 | 0x20u << 8));
    S.reg[DCN_B1 + regDENTIST_DISPCLK_CNTL] = save[0];

    /* dcn32_enable_power_gating_plane inside an IP request */
    for (i = 0; i < 8; i++) {
        save[i] = S.reg[DCN_B2 + pg[i]];
        status[i] = S.reg[DCN_B2 + pg[i] + 1];   /* the model's PG status follows the gate */
        S.reg[DCN_B2 + pg[i]] = DOMAIN0_PG_CONFIG__DOMAIN_POWER_FORCEON_MASK | DOMAIN0_PG_CONFIG__DOMAIN_POWER_GATE_MASK;
    }
    save[8] = S.reg[DCN_B2 + regDC_IP_REQUEST_CNTL];
    S.reg[DCN_B2 + regDC_IP_REQUEST_CNTL] = 0;
    dcn32_enable_power_gating_plane(dev, 1);
    for (i = 0; i < 8; i++)
        CHECK(S.reg[DCN_B2 + pg[i]] == DOMAIN0_PG_CONFIG__DOMAIN_POWER_GATE_MASK);
    CHECK(S.reg[DCN_B2 + regDC_IP_REQUEST_CNTL] == 0);
    dcn32_enable_power_gating_plane(dev, 0);
    for (i = 0; i < 8; i++) {
        CHECK(S.reg[DCN_B2 + pg[i]] & DOMAIN0_PG_CONFIG__DOMAIN_POWER_FORCEON_MASK);
        S.reg[DCN_B2 + pg[i]] = save[i];
        S.reg[DCN_B2 + pg[i] + 1] = status[i];
    }
    S.reg[DCN_B2 + regDC_IP_REQUEST_CNTL] = save[8];

    /* hubp32_init */
    save[9] = S.reg[DCN_B2 + regHUBP1_HUBPREQ_DEBUG_DB];
    dcn32_hubp_init(dev, 1);
    CHECK(S.reg[DCN_B2 + regHUBP1_HUBPREQ_DEBUG_DB] == 0x100);
    S.reg[DCN_B2 + regHUBP1_HUBPREQ_DEBUG_DB] = save[9];

    /* hubp32_cursor_set_attributes' MALL selection: over 16 KiB */
    save[9] = S.reg[DCN_B2 + regHUBP1_DCHUBP_MALL_CONFIG];
    S.reg[DCN_B2 + regHUBP1_DCHUBP_MALL_CONFIG] = 1;   /* USE_MALL_SEL kept */
    CHECK(dcn32_hubp32_cursor_mall(dev, 1, 256, 256, RD_DCN_CURSOR_MODE_PREMULT) == 1 &&
          S.reg[DCN_B2 + regHUBP1_DCHUBP_MALL_CONFIG] == (1u | HUBP0_DCHUBP_MALL_CONFIG__USE_MALL_FOR_CURSOR_MASK));
    CHECK(dcn32_hubp32_cursor_mall(dev, 1, 64, 64, RD_DCN_CURSOR_MODE_PREMULT) == 0 &&
          S.reg[DCN_B2 + regHUBP1_DCHUBP_MALL_CONFIG] == 1u);
    CHECK(dcn32_hubp32_cursor_mall(dev, 1, 33, 64, RD_DCN_CURSOR_MODE_PREMULT) == 0 &&   /* 64 x 64 x 4 */
          dcn32_hubp32_cursor_mall(dev, 1, 128, 128, 0) == 0 &&                            /* mono: 8 KiB */
          dcn32_hubp32_cursor_mall(dev, 1, 256, 256, 0) == 1 &&                            /* mono: 32 KiB */
          dcn32_hubp32_cursor_mall(dev, 1, 32, 32, 4) == 0);                               /* 64 x 32 x 8 */
    S.reg[DCN_B2 + regHUBP1_DCHUBP_MALL_CONFIG] = save[9];

    /* dcn32_are_clock_states_equal */
    memset(&a, 0, sizeof(a));
    a.dispclk_khz = 716000; a.dppclk_khz = 358000; a.p_state_change_support = 1;
    b = a;
    CHECK(dcn32_are_clock_states_equal(&a, &b));
    b.fclk_p_state_change_support = 1;
    CHECK(!dcn32_are_clock_states_equal(&a, &b));
}

static void test_dc_k(rdna3_dev *dev, rdna3_dmub *dm)
{
    test_dc_hwseq(dev);
    test_dc_dp(dev, dm);
    test_dc_color(dev);

    test_dc_otg(dev);
    test_dc_dmub(dev, dm);
    printf("sim_navi32: display core K tests ran (%u GPINTs, %u inbox0 commands)\n", SK.gpints, SK.inbox0_cmds);
}
