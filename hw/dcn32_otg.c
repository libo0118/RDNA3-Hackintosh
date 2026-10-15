/*
 * dcn32_otg.c - see dcn32_otg.h. Ported from Linux amdgpu Display Core
 * (optc/dcn10/dcn10_optc.c, optc/dcn20/dcn20_optc.c, optc/dcn30/dcn30_optc.c,
 * optc/dcn32/dcn32_optc.c), MIT, Copyright Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_otg.h"
#include "gen/dcn_3_2_0_otg.h"

#define OTG_STRIDE (regOTG1_OTG_CONTROL - regOTG0_OTG_CONTROL)
#define ODM_STRIDE (regODM1_OPTC_INPUT_CLOCK_CONTROL - regODM0_OPTC_INPUT_CLOCK_CONTROL)

#define GET(v, reg, field) (((v) & (uint32_t)reg##__##field##_MASK) >> reg##__##field##__SHIFT)
#define FLD(reg, field, x) (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK)
#define SET(v, reg, field, x) (((v) & ~(uint32_t)reg##__##field##_MASK) | FLD(reg, field, x))

/* paste reg##_BASE_IDX in the outermost macro, before 'reg' can expand */
#define AO(d, reg, n) (rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + OTG_STRIDE * (n))
#define AM(d, reg, n) (rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + ODM_STRIDE * (n))
#define RO(d, reg, n) rdna3_rreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + OTG_STRIDE * (n))
#define WO(d, reg, n, v) \
    rdna3_wreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + OTG_STRIDE * (n), (v))
#define RM(d, reg, n) rdna3_rreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + ODM_STRIDE * (n))
#define WM(d, reg, n, v) \
    rdna3_wreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + ODM_STRIDE * (n), (v))
/* REG_UPDATE of one field; 'name' without the reg prefix */
#define UO(d, name, n, field, x) WO(d, reg##name, n, SET(RO(d, reg##name, n), name, field, x))
#define UM(d, name, n, field, x) WM(d, reg##name, n, SET(RM(d, reg##name, n), name, field, x))

/* ---- dcn10_optc.c ---- */

/* optc1_setup_vertical_interrupt0 */
void dcn32_otg_setup_vertical_interrupt0(rdna3_dev *d, uint32_t otg, uint32_t start_line, uint32_t end_line)
{
    WO(d, regOTG0_OTG_VERTICAL_INTERRUPT0_POSITION, otg,
       FLD(OTG0_OTG_VERTICAL_INTERRUPT0_POSITION, OTG_VERTICAL_INTERRUPT0_LINE_START, start_line) |
       FLD(OTG0_OTG_VERTICAL_INTERRUPT0_POSITION, OTG_VERTICAL_INTERRUPT0_LINE_END, end_line));
}

/* optc1_setup_vertical_interrupt1 */
void dcn32_otg_setup_vertical_interrupt1(rdna3_dev *d, uint32_t otg, uint32_t start_line)
{
    WO(d, regOTG0_OTG_VERTICAL_INTERRUPT1_POSITION, otg,
       FLD(OTG0_OTG_VERTICAL_INTERRUPT1_POSITION, OTG_VERTICAL_INTERRUPT1_LINE_START, start_line));
}

/* optc1_setup_vertical_interrupt2 */
void dcn32_otg_setup_vertical_interrupt2(rdna3_dev *d, uint32_t otg, uint32_t start_line)
{
    WO(d, regOTG0_OTG_VERTICAL_INTERRUPT2_POSITION, otg,
       FLD(OTG0_OTG_VERTICAL_INTERRUPT2_POSITION, OTG_VERTICAL_INTERRUPT2_LINE_START, start_line));
}

int dcn32_otg_vline0_status(rdna3_dev *d, uint32_t otg)
{
    return (int)GET(RO(d, regOTG0_OTG_VERTICAL_INTERRUPT0_CONTROL, otg), OTG0_OTG_VERTICAL_INTERRUPT0_CONTROL,
                    OTG_VERTICAL_INTERRUPT0_STATUS);
}

void dcn32_otg_vline0_clear(rdna3_dev *d, uint32_t otg)
{
    UO(d, OTG0_OTG_VERTICAL_INTERRUPT0_CONTROL, otg, OTG_VERTICAL_INTERRUPT0_CLEAR, 1);
}

/* optc1_validate_timing with the limits of DCN 3.2's timing generator: no
 * borders, no 3D, interlace refused ("temporarily blocking") */
int dcn32_otg_validate_timing(const rdna3_mode *m)
{
    const uint32_t max_h_total = (OTG0_OTG_H_TOTAL__OTG_H_TOTAL_MASK >> OTG0_OTG_H_TOTAL__OTG_H_TOTAL__SHIFT) + 1;
    const uint32_t max_v_total = (OTG0_OTG_V_TOTAL__OTG_V_TOTAL_MASK >> OTG0_OTG_V_TOTAL__OTG_V_TOTAL__SHIFT) + 1;
    uint32_t v_blank, h_blank, min_v_blank;

    if (!m || m->h_total < m->h_active || m->v_total < m->v_active)
        return 0;
    v_blank = m->v_total - m->v_active;
    h_blank = m->h_total - m->h_active;
    if (m->interlaced)
        return 0;
    if (m->h_total > max_h_total || m->v_total > max_v_total)
        return 0;
    if (h_blank < RD_OPTC_MIN_H_BLANK)
        return 0;
    if (m->h_sync < RD_OPTC_MIN_H_SYNC_WIDTH || m->v_sync < RD_OPTC_MIN_V_SYNC_WIDTH)
        return 0;
    min_v_blank = m->interlaced ? RD_OPTC_MIN_V_BLANK_INTERLACE : RD_OPTC_MIN_V_BLANK;
    return v_blank >= min_v_blank;
}

/* optc1_is_two_pixels_per_container */
int dcn32_otg_two_pixels_per_container(const rdna3_crtc_format *f)
{
    int two_pix = f->pixel_encoding == RD_PIXEL_ENCODING_YCBCR420;
    two_pix = two_pix || (f->dsc && f->pixel_encoding == RD_PIXEL_ENCODING_YCBCR422 && !f->dsc_ycbcr422_simple);
    return two_pix;
}

/* optc1_get_position */
void dcn32_otg_get_position(rdna3_dev *d, uint32_t otg, rdna3_crtc_position *p)
{
    uint32_t v = RO(d, regOTG0_OTG_STATUS_POSITION, otg);
    p->horizontal_count = GET(v, OTG0_OTG_STATUS_POSITION, OTG_HORZ_COUNT);
    p->vertical_count = GET(v, OTG0_OTG_STATUS_POSITION, OTG_VERT_COUNT);
    p->nominal_vcount = GET(RO(d, regOTG0_OTG_NOM_VERT_POSITION, otg), OTG0_OTG_NOM_VERT_POSITION, OTG_VERT_COUNT_NOM);
}

/* optc1_is_counter_moving */
int dcn32_otg_is_counter_moving(rdna3_dev *d, uint32_t otg)
{
    rdna3_crtc_position p1, p2;
    dcn32_otg_get_position(d, otg, &p1);
    dcn32_otg_get_position(d, otg, &p2);
    return !(p1.horizontal_count == p2.horizontal_count && p1.vertical_count == p2.vertical_count);
}

/* optc1_get_crtc_scanoutpos */
void dcn32_otg_get_scanoutpos(rdna3_dev *d, uint32_t otg, uint32_t *v_blank_start, uint32_t *v_blank_end,
                              uint32_t *h_position, uint32_t *v_position)
{
    rdna3_crtc_position p;
    uint32_t v = RO(d, regOTG0_OTG_V_BLANK_START_END, otg);
    *v_blank_start = GET(v, OTG0_OTG_V_BLANK_START_END, OTG_V_BLANK_START);
    *v_blank_end = GET(v, OTG0_OTG_V_BLANK_START_END, OTG_V_BLANK_END);
    dcn32_otg_get_position(d, otg, &p);
    *h_position = p.horizontal_count;
    *v_position = p.vertical_count;
}

/* optc1_get_otg_active_size */
int dcn32_otg_get_active_size(rdna3_dev *d, uint32_t otg, uint32_t *width, uint32_t *height)
{
    uint32_t v, h;
    if (!GET(RO(d, regOTG0_OTG_CONTROL, otg), OTG0_OTG_CONTROL, OTG_MASTER_EN))
        return 0;
    v = RO(d, regOTG0_OTG_V_BLANK_START_END, otg);
    h = RO(d, regOTG0_OTG_H_BLANK_START_END, otg);
    *width = GET(h, OTG0_OTG_H_BLANK_START_END, OTG_H_BLANK_START) - GET(h, OTG0_OTG_H_BLANK_START_END, OTG_H_BLANK_END);
    *height = GET(v, OTG0_OTG_V_BLANK_START_END, OTG_V_BLANK_START) - GET(v, OTG0_OTG_V_BLANK_START_END, OTG_V_BLANK_END);
    return 1;
}

/* optc1_is_tg_enabled */
int dcn32_otg_is_enabled(rdna3_dev *d, uint32_t otg)
{
    return GET(RO(d, regOTG0_OTG_CONTROL, otg), OTG0_OTG_CONTROL, OTG_MASTER_EN) == 1;
}

/* optc1_wait_for_state: REG_WAIT(OTG_STATUS, ..., 1, 1 us, 100000) */
int dcn32_otg_wait_for_state(rdna3_dev *d, uint32_t otg, int state)
{
    uint32_t mask = state == RD_CRTC_STATE_VBLANK ? OTG0_OTG_STATUS__OTG_V_BLANK_MASK
                                                  : OTG0_OTG_STATUS__OTG_V_ACTIVE_DISP_MASK;
    return rdna3_wait_reg(d, AO(d, regOTG0_OTG_STATUS, otg), mask, mask, 0, 100000);
}

/* optc1_is_optc_underflow_occurred */
int dcn32_otg_underflow_occurred(rdna3_dev *d, uint32_t otg)
{
    return GET(RM(d, regODM0_OPTC_INPUT_GLOBAL_CONTROL, otg), ODM0_OPTC_INPUT_GLOBAL_CONTROL,
               OPTC_UNDERFLOW_OCCURRED_STATUS) == 1;
}

/* optc1_clear_optc_underflow */
void dcn32_otg_clear_underflow(rdna3_dev *d, uint32_t otg)
{
    UM(d, ODM0_OPTC_INPUT_GLOBAL_CONTROL, otg, OPTC_UNDERFLOW_CLEAR, 1);
}

/* optc1_enable_optc_clock */
void dcn32_otg_enable_optc_clock(rdna3_dev *d, uint32_t otg, int enable)
{
    uint32_t v;
    if (enable) {
        v = RM(d, regODM0_OPTC_INPUT_CLOCK_CONTROL, otg);
        v = SET(v, ODM0_OPTC_INPUT_CLOCK_CONTROL, OPTC_INPUT_CLK_EN, 1);
        v = SET(v, ODM0_OPTC_INPUT_CLOCK_CONTROL, OPTC_INPUT_CLK_GATE_DIS, 1);
        WM(d, regODM0_OPTC_INPUT_CLOCK_CONTROL, otg, v);
        rdna3_wait_reg(d, AM(d, regODM0_OPTC_INPUT_CLOCK_CONTROL, otg),
                       ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_ON_MASK,
                       ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_ON_MASK, 0, 1000);
        v = RO(d, regOTG0_OTG_CLOCK_CONTROL, otg);
        v = SET(v, OTG0_OTG_CLOCK_CONTROL, OTG_CLOCK_EN, 1);
        v = SET(v, OTG0_OTG_CLOCK_CONTROL, OTG_CLOCK_GATE_DIS, 1);
        WO(d, regOTG0_OTG_CLOCK_CONTROL, otg, v);
        rdna3_wait_reg(d, AO(d, regOTG0_OTG_CLOCK_CONTROL, otg), OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_ON_MASK,
                       OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_ON_MASK, 0, 1000);
        return;
    }
    /* the last chance to clear the underflow: with the clock off it stays */
    if (dcn32_otg_underflow_occurred(d, otg))
        dcn32_otg_clear_underflow(d, otg);
    v = RO(d, regOTG0_OTG_CLOCK_CONTROL, otg);
    v = SET(v, OTG0_OTG_CLOCK_CONTROL, OTG_CLOCK_GATE_DIS, 0);
    v = SET(v, OTG0_OTG_CLOCK_CONTROL, OTG_CLOCK_EN, 0);
    WO(d, regOTG0_OTG_CLOCK_CONTROL, otg, v);
    v = RM(d, regODM0_OPTC_INPUT_CLOCK_CONTROL, otg);
    v = SET(v, ODM0_OPTC_INPUT_CLOCK_CONTROL, OPTC_INPUT_CLK_GATE_DIS, 0);
    v = SET(v, ODM0_OPTC_INPUT_CLOCK_CONTROL, OPTC_INPUT_CLK_EN, 0);
    WM(d, regODM0_OPTC_INPUT_CLOCK_CONTROL, otg, v);
}

/* optc1_set_static_screen_control */
void dcn32_otg_set_static_screen_control(rdna3_dev *d, uint32_t otg, uint32_t event_triggers, uint32_t num_frames)
{
    if (num_frames > 0xff)   /* an 8-bit field */
        num_frames = 0xff;
    /* bit 8 no longer applies (PSR on RV) */
    event_triggers &= ~(uint32_t)RD_STATIC_SCREEN_EVENT_MASK_RANGETIMING_DOUBLE_BUFFER_UPDATE_EN;
    WO(d, regOTG0_OTG_STATIC_SCREEN_CONTROL, otg,
       FLD(OTG0_OTG_STATIC_SCREEN_CONTROL, OTG_STATIC_SCREEN_EVENT_MASK, event_triggers) |
       FLD(OTG0_OTG_STATIC_SCREEN_CONTROL, OTG_STATIC_SCREEN_FRAME_COUNT, num_frames));
}

/* the vsync of the source OTG (OTG_TRIGA_SOURCE_SELECT 20 + PIPE_SELECT) */
#define TRIGA_SRC_VSYNC  20
#define TRIGA_SRC_MANUAL 21

/* optc1_enable_reset_trigger */
void dcn32_otg_enable_reset_trigger(rdna3_dev *d, uint32_t otg, uint32_t source_otg)
{
    uint32_t falling = GET(RO(d, regOTG0_OTG_V_SYNC_A_CNTL, otg), OTG0_OTG_V_SYNC_A_CNTL, OTG_V_SYNC_A_POL);
    uint32_t v = FLD(OTG0_OTG_TRIGA_CNTL, OTG_TRIGA_SOURCE_SELECT, TRIGA_SRC_VSYNC) |
                 FLD(OTG0_OTG_TRIGA_CNTL, OTG_TRIGA_SOURCE_PIPE_SELECT, source_otg);
    if (falling)
        v |= FLD(OTG0_OTG_TRIGA_CNTL, OTG_TRIGA_FALLING_EDGE_DETECT_CNTL, 1);
    else
        v |= FLD(OTG0_OTG_TRIGA_CNTL, OTG_TRIGA_RISING_EDGE_DETECT_CNTL, 1);
    WO(d, regOTG0_OTG_TRIGA_CNTL, otg, v);
    /* force H / V count to H_TOTAL / V_TOTAL (progressive) */
    WO(d, regOTG0_OTG_FORCE_COUNT_NOW_CNTL, otg, FLD(OTG0_OTG_FORCE_COUNT_NOW_CNTL, OTG_FORCE_COUNT_NOW_MODE, 2));
}

/* optc1_enable_crtc_reset */
void dcn32_otg_enable_crtc_reset(rdna3_dev *d, uint32_t otg, uint32_t source_otg, int event, int delay)
{
    WO(d, regOTG0_OTG_TRIGA_CNTL, otg,
       FLD(OTG0_OTG_TRIGA_CNTL, OTG_TRIGA_SOURCE_SELECT, TRIGA_SRC_VSYNC) |
       FLD(OTG0_OTG_TRIGA_CNTL, OTG_TRIGA_SOURCE_PIPE_SELECT, source_otg) |
       FLD(OTG0_OTG_TRIGA_CNTL, OTG_TRIGA_RISING_EDGE_DETECT_CNTL, event == RD_CRTC_EVENT_VSYNC_RISING) |
       FLD(OTG0_OTG_TRIGA_CNTL, OTG_TRIGA_FALLING_EDGE_DETECT_CNTL, event == RD_CRTC_EVENT_VSYNC_FALLING));
    if (delay == RD_TRIGGER_DELAY_NEXT_LINE)
        WO(d, regOTG0_OTG_VERT_SYNC_CONTROL, otg, FLD(OTG0_OTG_VERT_SYNC_CONTROL, OTG_AUTO_FORCE_VSYNC_MODE, 1));
    else
        WO(d, regOTG0_OTG_FORCE_COUNT_NOW_CNTL, otg, FLD(OTG0_OTG_FORCE_COUNT_NOW_CNTL, OTG_FORCE_COUNT_NOW_MODE, 2));
}

/* optc1_did_triggered_reset_occur */
int dcn32_otg_did_triggered_reset_occur(rdna3_dev *d, uint32_t otg)
{
    uint32_t force = GET(RO(d, regOTG0_OTG_FORCE_COUNT_NOW_CNTL, otg), OTG0_OTG_FORCE_COUNT_NOW_CNTL,
                         OTG_FORCE_COUNT_NOW_OCCURRED);
    uint32_t vsync = GET(RO(d, regOTG0_OTG_VERT_SYNC_CONTROL, otg), OTG0_OTG_VERT_SYNC_CONTROL,
                         OTG_FORCE_VSYNC_NEXT_LINE_OCCURRED);
    return vsync != 0 || force != 0;
}

/* optc1_disable_reset_trigger */
void dcn32_otg_disable_reset_trigger(rdna3_dev *d, uint32_t otg)
{
    WO(d, regOTG0_OTG_TRIGA_CNTL, otg, 0);
    WO(d, regOTG0_OTG_FORCE_COUNT_NOW_CNTL, otg, FLD(OTG0_OTG_FORCE_COUNT_NOW_CNTL, OTG_FORCE_COUNT_NOW_CLEAR, 1));
    WO(d, regOTG0_OTG_VERT_SYNC_CONTROL, otg, FLD(OTG0_OTG_VERT_SYNC_CONTROL, OTG_FORCE_VSYNC_NEXT_LINE_CLEAR, 1));
}

/* optc1_set_vtotal_min_max */
static void optc1_set_vtotal_min_max(rdna3_dev *d, uint32_t otg, uint32_t vtotal_min, uint32_t vtotal_max)
{
    WO(d, regOTG0_OTG_V_TOTAL_MAX, otg, FLD(OTG0_OTG_V_TOTAL_MAX, OTG_V_TOTAL_MAX, vtotal_max));
    WO(d, regOTG0_OTG_V_TOTAL_MIN, otg, FLD(OTG0_OTG_V_TOTAL_MIN, OTG_V_TOTAL_MIN, vtotal_min));
}

/* ---- dcn20_optc.c / dcn30_optc.c ---- */

/* optc3_tg_init: optc3_set_timing_double_buffer(true) (DRR timing
 * double-buffered, update mode 2 = start of frame) + optc1_clear_optc_underflow */
void dcn32_otg_tg_init(rdna3_dev *d, uint32_t otg)
{
    UO(d, OTG0_OTG_DOUBLE_BUFFER_CONTROL, otg, OTG_DRR_TIMING_DBUF_UPDATE_MODE, 2);
    dcn32_otg_clear_underflow(d, otg);
}

/* optc3_lock / optc1_unlock: REG_WAIT(UPDATE_LOCK_STATUS, 1, 1 us, 10) */
void dcn32_otg_lock(rdna3_dev *d, uint32_t otg, int lock)
{
    if (!lock) {
        WO(d, regOTG0_OTG_MASTER_UPDATE_LOCK, otg, 0);
        return;
    }
    UO(d, OTG0_OTG_GLOBAL_CONTROL2, otg, OTG_MASTER_UPDATE_LOCK_SEL, otg);
    WO(d, regOTG0_OTG_MASTER_UPDATE_LOCK, otg, FLD(OTG0_OTG_MASTER_UPDATE_LOCK, OTG_MASTER_UPDATE_LOCK, 1));
    rdna3_wait_reg(d, AO(d, regOTG0_OTG_MASTER_UPDATE_LOCK, otg), OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK,
                   OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK, 0, 10);
}

/* optc3_triplebuffer_lock / optc2_triplebuffer_unlock */
void dcn32_otg_triplebuffer_lock(rdna3_dev *d, uint32_t otg, int lock)
{
    if (!lock) {
        WO(d, regOTG0_OTG_MASTER_UPDATE_LOCK, otg, 0);
        WO(d, regOTG0_OTG_VUPDATE_KEEPOUT, otg, 0);
        return;
    }
    UO(d, OTG0_OTG_GLOBAL_CONTROL2, otg, OTG_MASTER_UPDATE_LOCK_SEL, otg);
    WO(d, regOTG0_OTG_VUPDATE_KEEPOUT, otg,
       FLD(OTG0_OTG_VUPDATE_KEEPOUT, OTG_MASTER_UPDATE_LOCK_VUPDATE_KEEPOUT_EN, 1));
    WO(d, regOTG0_OTG_MASTER_UPDATE_LOCK, otg, FLD(OTG0_OTG_MASTER_UPDATE_LOCK, OTG_MASTER_UPDATE_LOCK, 1));
    rdna3_wait_reg(d, AO(d, regOTG0_OTG_MASTER_UPDATE_LOCK, otg), OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK,
                   OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK, 0, 10);
}

/* optc3_lock_doublebuffer_enable / optc3_lock_doublebuffer_disable */
void dcn32_otg_lock_doublebuffer(rdna3_dev *d, uint32_t otg, int enable)
{
    uint32_t v;
    if (enable) {
        uint32_t vb = RO(d, regOTG0_OTG_V_BLANK_START_END, otg), hb = RO(d, regOTG0_OTG_H_BLANK_START_END, otg);
        uint32_t v_blank_start = GET(vb, OTG0_OTG_V_BLANK_START_END, OTG_V_BLANK_START);
        uint32_t h_blank_start = GET(hb, OTG0_OTG_H_BLANK_START_END, OTG_H_BLANK_START);

        v = RO(d, regOTG0_OTG_GLOBAL_CONTROL1, otg);
        v = SET(v, OTG0_OTG_GLOBAL_CONTROL1, MASTER_UPDATE_LOCK_DB_START_Y, v_blank_start - 1);
        v = SET(v, OTG0_OTG_GLOBAL_CONTROL1, MASTER_UPDATE_LOCK_DB_END_Y, v_blank_start);
        WO(d, regOTG0_OTG_GLOBAL_CONTROL1, otg, v);
        v = RO(d, regOTG0_OTG_GLOBAL_CONTROL4, otg);
        v = SET(v, OTG0_OTG_GLOBAL_CONTROL4, DIG_UPDATE_POSITION_X, h_blank_start - 180 - 1);
        v = SET(v, OTG0_OTG_GLOBAL_CONTROL4, DIG_UPDATE_POSITION_Y, v_blank_start - 1);
        WO(d, regOTG0_OTG_GLOBAL_CONTROL4, otg, v);
        v = RO(d, regOTG0_OTG_GLOBAL_CONTROL0, otg);
        v = SET(v, OTG0_OTG_GLOBAL_CONTROL0, MASTER_UPDATE_LOCK_DB_START_X, h_blank_start - 200 - 1);
        v = SET(v, OTG0_OTG_GLOBAL_CONTROL0, MASTER_UPDATE_LOCK_DB_END_X, h_blank_start - 180);
        v = SET(v, OTG0_OTG_GLOBAL_CONTROL0, MASTER_UPDATE_LOCK_DB_EN, 1);
        WO(d, regOTG0_OTG_GLOBAL_CONTROL0, otg, v);
        UO(d, OTG0_OTG_GLOBAL_CONTROL2, otg, GLOBAL_UPDATE_LOCK_EN, 1);
        WO(d, regOTG0_OTG_VUPDATE_KEEPOUT, otg,
           FLD(OTG0_OTG_VUPDATE_KEEPOUT, MASTER_UPDATE_LOCK_VUPDATE_KEEPOUT_START_OFFSET, 0) |
           FLD(OTG0_OTG_VUPDATE_KEEPOUT, MASTER_UPDATE_LOCK_VUPDATE_KEEPOUT_END_OFFSET, 100) |
           FLD(OTG0_OTG_VUPDATE_KEEPOUT, OTG_MASTER_UPDATE_LOCK_VUPDATE_KEEPOUT_EN, 1));
        return;
    }
    v = RO(d, regOTG0_OTG_GLOBAL_CONTROL0, otg);
    v = SET(v, OTG0_OTG_GLOBAL_CONTROL0, MASTER_UPDATE_LOCK_DB_START_X, 0);
    v = SET(v, OTG0_OTG_GLOBAL_CONTROL0, MASTER_UPDATE_LOCK_DB_END_X, 0);
    WO(d, regOTG0_OTG_GLOBAL_CONTROL0, otg, v);
    v = RO(d, regOTG0_OTG_GLOBAL_CONTROL1, otg);
    v = SET(v, OTG0_OTG_GLOBAL_CONTROL1, MASTER_UPDATE_LOCK_DB_START_Y, 0);
    v = SET(v, OTG0_OTG_GLOBAL_CONTROL1, MASTER_UPDATE_LOCK_DB_END_Y, 0);
    WO(d, regOTG0_OTG_GLOBAL_CONTROL1, otg, v);
    UO(d, OTG0_OTG_GLOBAL_CONTROL2, otg, GLOBAL_UPDATE_LOCK_EN, 0);
    UO(d, OTG0_OTG_GLOBAL_CONTROL0, otg, MASTER_UPDATE_LOCK_DB_EN, 0);
}

/* optc3_set_drr_trigger_window */
void dcn32_otg_set_drr_trigger_window(rdna3_dev *d, uint32_t otg, uint32_t start, uint32_t end)
{
    WO(d, regOTG0_OTG_DRR_TRIGGER_WINDOW, otg,
       FLD(OTG0_OTG_DRR_TRIGGER_WINDOW, OTG_DRR_TRIGGER_WINDOW_START_X, start) |
       FLD(OTG0_OTG_DRR_TRIGGER_WINDOW, OTG_DRR_TRIGGER_WINDOW_END_X, end));
}

/* optc3_set_vtotal_change_limit */
void dcn32_otg_set_vtotal_change_limit(rdna3_dev *d, uint32_t otg, uint32_t limit)
{
    WO(d, regOTG0_OTG_DRR_V_TOTAL_CHANGE, otg, FLD(OTG0_OTG_DRR_V_TOTAL_CHANGE, OTG_DRR_V_TOTAL_CHANGE_LIMIT, limit));
}

/* optc3_set_vtotal_min_max without FAMS (dc->caps.dmub_caps.mclk_sw off):
 * optc1_set_vtotal_min_max */
void dcn32_otg_set_vtotal_min_max(rdna3_dev *d, uint32_t otg, uint32_t vtotal_min, uint32_t vtotal_max)
{
    optc1_set_vtotal_min_max(d, otg, vtotal_min, vtotal_max);
}

/* optc32_setup_manual_trigger (no FAMS) = the V_TOTAL_CONTROL part of
 * optc2_setup_manual_trigger: MIN/MAX_SEL, TRIGA as the event that ends
 * the extended vertical blank */
void dcn32_otg_setup_manual_trigger(rdna3_dev *d, uint32_t otg)
{
    uint32_t v = RO(d, regOTG0_OTG_V_TOTAL_CONTROL, otg);
    v = SET(v, OTG0_OTG_V_TOTAL_CONTROL, OTG_V_TOTAL_MIN_SEL, 1);
    v = SET(v, OTG0_OTG_V_TOTAL_CONTROL, OTG_V_TOTAL_MAX_SEL, 1);
    v = SET(v, OTG0_OTG_V_TOTAL_CONTROL, OTG_FORCE_LOCK_ON_EVENT, 0);
    v = SET(v, OTG0_OTG_V_TOTAL_CONTROL, OTG_SET_V_TOTAL_MIN_MASK, 1u << 1);   /* TRIGA */
    WO(d, regOTG0_OTG_V_TOTAL_CONTROL, otg, v);
}

/* optc2_program_manual_trigger */
void dcn32_otg_program_manual_trigger(rdna3_dev *d, uint32_t otg)
{
    WO(d, regOTG0_OTG_TRIGA_MANUAL_TRIG, otg, FLD(OTG0_OTG_TRIGA_MANUAL_TRIG, OTG_TRIGA_MANUAL_TRIG, 1));
}

/* optc32_set_drr (v6.12 and v6.16 alike). Unlike DCN 3.1's optc31_set_drr
 * it has no "DRR off" branch: without a vmin / vmax it leaves MIN_SEL /
 * MAX_SEL and V_TOTAL_MIN / MAX as they are and arms the manual trigger
 * all the same (the mode set's optc1_set_vtotal_min_max(v_total, v_total)
 * keeps such a stream at its nominal timing). */
void dcn32_otg_set_drr(rdna3_dev *d, uint32_t otg, const rdna3_drr_params *p)
{
    if (p && p->vertical_total_max > 0 && p->vertical_total_min > 0) {
        if (p->vertical_total_mid != 0) {
            uint32_t v;
            WO(d, regOTG0_OTG_V_TOTAL_MID, otg, FLD(OTG0_OTG_V_TOTAL_MID, OTG_V_TOTAL_MID, p->vertical_total_mid - 1));
            v = RO(d, regOTG0_OTG_V_TOTAL_CONTROL, otg);
            v = SET(v, OTG0_OTG_V_TOTAL_CONTROL, OTG_VTOTAL_MID_REPLACING_MAX_EN, 1);
            v = SET(v, OTG0_OTG_V_TOTAL_CONTROL, OTG_VTOTAL_MID_FRAME_NUM, (uint8_t)p->vertical_total_mid_frame_num);
            WO(d, regOTG0_OTG_V_TOTAL_CONTROL, otg, v);
        }
        dcn32_otg_set_vtotal_min_max(d, otg, p->vertical_total_min - 1, p->vertical_total_max - 1);
    }
    dcn32_otg_setup_manual_trigger(d, otg);
}

/* optc3_set_out_mux */
void dcn32_otg_set_out_mux(rdna3_dev *d, uint32_t otg, uint32_t dest)
{
    UO(d, OTG0_OTG_CONTROL, otg, OTG_OUT_MUX, dest);
}

/* ---- dcn32_optc.c ---- */

/* optc32_get_double_buffer_pending */
int dcn32_otg_double_buffer_pending(rdna3_dev *d, uint32_t otg)
{
    return GET(RM(d, regODM0_OPTC_INPUT_GLOBAL_CONTROL, otg), ODM0_OPTC_INPUT_GLOBAL_CONTROL,
               OPTC_DOUBLE_BUFFER_PENDING) == 1;
}

/* optc32_get_odm_combine_segments */
int dcn32_otg_odm_combine_segments(rdna3_dev *d, uint32_t otg)
{
    switch (GET(RM(d, regODM0_OPTC_DATA_SOURCE_SELECT, otg), ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_NUM_OF_INPUT_SEGMENT)) {
    case 0: return 1;
    case 1: return 2;
    case 3: return 4;
    default: return -1;   /* 2 is reserved */
    }
}

/* optc32_set_h_timing_div_manual_mode */
void dcn32_otg_set_h_timing_div_manual_mode(rdna3_dev *d, uint32_t otg, int manual)
{
    UO(d, OTG0_OTG_H_TIMING_CNTL, otg, OTG_H_TIMING_DIV_MODE_MANUAL, manual ? 1 : 0);
}

/* optc32_set_odm_combine */
int dcn32_otg_set_odm_combine(rdna3_dev *d, uint32_t otg, const uint32_t *opp_id, uint32_t opp_cnt,
                              uint32_t segment_width)
{
    uint32_t memory_mask, h_active = segment_width * opp_cnt, v;
    /* each memory instance is 2048 x (32 x 2) bits: half a line of 4096 */
    uint32_t odm_mem_count = (h_active + 2047) / 2048;

    if (opp_cnt != 2 && opp_cnt != 4)
        return RDNA3_EINVAL;
    /* <= 4k: 2 memories + 2 pipes; <= 8k: 4 + 2; <= 12k: 6 + 4 */
    if (opp_cnt == 4)
        memory_mask = odm_mem_count <= 2 ? 0x3 : odm_mem_count <= 4 ? 0xf : 0x3f;
    else if (odm_mem_count <= 2)
        memory_mask = 0x1u << (opp_id[0] * 2) | 0x1u << (opp_id[1] * 2);
    else if (odm_mem_count <= 4)
        memory_mask = 0x3u << (opp_id[0] * 2) | 0x3u << (opp_id[1] * 2);
    else
        memory_mask = 0x77;
    WM(d, regODM0_OPTC_MEMORY_CONFIG, otg, FLD(ODM0_OPTC_MEMORY_CONFIG, OPTC_MEM_SEL, memory_mask));
    if (opp_cnt == 2)
        WM(d, regODM0_OPTC_DATA_SOURCE_SELECT, otg,
           FLD(ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_NUM_OF_INPUT_SEGMENT, 1) |
           FLD(ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG0_SRC_SEL, opp_id[0]) |
           FLD(ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG1_SRC_SEL, opp_id[1]));
    else
        WM(d, regODM0_OPTC_DATA_SOURCE_SELECT, otg,
           FLD(ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_NUM_OF_INPUT_SEGMENT, 3) |
           FLD(ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG0_SRC_SEL, opp_id[0]) |
           FLD(ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG1_SRC_SEL, opp_id[1]) |
           FLD(ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG2_SRC_SEL, opp_id[2]) |
           FLD(ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG3_SRC_SEL, opp_id[3]));
    UM(d, ODM0_OPTC_WIDTH_CONTROL, otg, OPTC_SEGMENT_WIDTH, segment_width);
    v = RO(d, regOTG0_OTG_H_TIMING_CNTL, otg);
    WO(d, regOTG0_OTG_H_TIMING_CNTL, otg, SET(v, OTG0_OTG_H_TIMING_CNTL, OTG_H_TIMING_DIV_MODE, opp_cnt - 1));
    return RDNA3_OK;
}

/* optc32_set_odm_bypass */
void dcn32_otg_set_odm_bypass(rdna3_dev *d, uint32_t otg, int two_pix)
{
    uint32_t v;
    WM(d, regODM0_OPTC_DATA_SOURCE_SELECT, otg,
       FLD(ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_NUM_OF_INPUT_SEGMENT, 0) |
       FLD(ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG0_SRC_SEL, otg) |
       FLD(ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG1_SRC_SEL, 0xf) |
       FLD(ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG2_SRC_SEL, 0xf) |
       FLD(ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG3_SRC_SEL, 0xf));
    /* enum h_timing_div_mode: H_TIMING_NO_DIV 0, H_TIMING_DIV_BY2 1 */
    v = RO(d, regOTG0_OTG_H_TIMING_CNTL, otg);
    WO(d, regOTG0_OTG_H_TIMING_CNTL, otg, SET(v, OTG0_OTG_H_TIMING_CNTL, OTG_H_TIMING_DIV_MODE, two_pix ? 1 : 0));
    WM(d, regODM0_OPTC_MEMORY_CONFIG, otg, FLD(ODM0_OPTC_MEMORY_CONFIG, OPTC_MEM_SEL, 0));
}

/* ---- the "otg" ladder step ---- */

int dcn32_otg_check(rdna3_dev *d, uint32_t otg, rdna3_otg_check *c)
{
    uint32_t pos0, ctl0, i, line;
    rdna3_crtc_position p;

    for (i = 0; i < sizeof(*c); i++)
        ((uint8_t *)c)[i] = 0;
    if (!dcn32_otg_get_active_size(d, otg, &c->width, &c->height))
        return RDNA3_ENODEV;
    c->moving = dcn32_otg_is_counter_moving(d, otg);
    dcn32_otg_get_scanoutpos(d, otg, &c->v_blank_start, &c->v_blank_end, &c->h_position, &c->v_position);
    dcn32_otg_get_position(d, otg, &p);
    c->nominal_vcount = p.nominal_vcount;
    c->underflow = dcn32_otg_underflow_occurred(d, otg);
    c->db_pending = dcn32_otg_double_buffer_pending(d, otg);
    c->odm_segments = dcn32_otg_odm_combine_segments(d, otg);
    c->vblank_seen = dcn32_otg_wait_for_state(d, otg, RD_CRTC_STATE_VBLANK) == RDNA3_OK;
    c->vactive_seen = dcn32_otg_wait_for_state(d, otg, RD_CRTC_STATE_VACTIVE) == RDNA3_OK;

    /* vertical interrupt 0 on three lines in the middle of the active
     * area: its status must latch within two frames (60 ms polled) */
    pos0 = RO(d, regOTG0_OTG_VERTICAL_INTERRUPT0_POSITION, otg);
    ctl0 = RO(d, regOTG0_OTG_VERTICAL_INTERRUPT0_CONTROL, otg);
    line = c->v_blank_end + c->height / 2;
    c->vline0_line = line;
    dcn32_otg_setup_vertical_interrupt0(d, otg, line, line + 2);
    dcn32_otg_vline0_clear(d, otg);
    for (i = 0; i < 1200; i++) {
        if (dcn32_otg_vline0_status(d, otg)) {
            dcn32_otg_get_position(d, otg, &p);
            c->vline0_seen = 1;
            c->vline0_at = p.vertical_count;
            break;
        }
        d->os.udelay(d->os.ctx, 50);
    }
    WO(d, regOTG0_OTG_VERTICAL_INTERRUPT0_POSITION, otg, pos0);
    dcn32_otg_vline0_clear(d, otg);
    WO(d, regOTG0_OTG_VERTICAL_INTERRUPT0_CONTROL, otg,
       ctl0 & ~(OTG0_OTG_VERTICAL_INTERRUPT0_CONTROL__OTG_VERTICAL_INTERRUPT0_CLEAR_MASK |
                OTG0_OTG_VERTICAL_INTERRUPT0_CONTROL__OTG_VERTICAL_INTERRUPT0_STATUS_MASK |
                OTG0_OTG_VERTICAL_INTERRUPT0_CONTROL__OTG_VERTICAL_INTERRUPT0_INT_STATUS_MASK |
                OTG0_OTG_VERTICAL_INTERRUPT0_CONTROL__OTG_VINTE_STATUS_MASK));
    RLOG(d, "otg %u: %ux%u, counter %s, v %u (blank %u..%u, nominal %u), vblank %d vactive %d, "
         "vline0 at %u %s (read %u), underflow %d, double buffer pending %d, ODM segments %d", otg, c->width,
         c->height, c->moving ? "moving" : "STOPPED", c->v_position, c->v_blank_start, c->v_blank_end,
         c->nominal_vcount, c->vblank_seen, c->vactive_seen, line, c->vline0_seen ? "latched" : "NOT latched",
         c->vline0_at, c->underflow, c->db_pending, c->odm_segments);
    return RDNA3_OK;
}
