/*
 * dcn32_otg.h - the timing generator (OTG / OPTC) functions of DCN 3.2 that
 * the scanout takeover, the mode set and the clone did not need, ported
 * from Linux amdgpu Display Core (v6.12) as dcn32_tg_funcs wires them:
 *
 *   dcn10_optc.c  optc1_setup_vertical_interrupt0/1/2, optc1_validate_timing,
 *                 optc1_is_two_pixels_per_container, optc1_get_otg_active_size,
 *                 optc1_get_position, optc1_is_counter_moving,
 *                 optc1_get_crtc_scanoutpos, optc1_wait_for_state,
 *                 optc1_enable_optc_clock, optc1_set_static_screen_control,
 *                 optc1_enable_reset_trigger, optc1_enable_crtc_reset,
 *                 optc1_did_triggered_reset_occur, optc1_disable_reset_trigger,
 *                 optc1_is_optc_underflow_occurred, optc1_clear_optc_underflow,
 *                 optc1_set_vtotal_min_max, optc1_unlock
 *   dcn20_optc.c  optc2_setup_manual_trigger, optc2_program_manual_trigger,
 *                 optc2_triplebuffer_unlock
 *   dcn30_optc.c  optc3_tg_init (optc3_set_timing_double_buffer),
 *                 optc3_lock, optc3_triplebuffer_lock,
 *                 optc3_lock_doublebuffer_enable / _disable,
 *                 optc3_set_drr_trigger_window, optc3_set_vtotal_change_limit,
 *                 optc3_set_vtotal_min_max, optc3_set_out_mux
 *   dcn32_optc.c  optc32_set_drr (optc32_setup_manual_trigger),
 *                 optc32_get_double_buffer_pending,
 *                 optc32_get_odm_combine_segments,
 *                 optc32_set_h_timing_div_manual_mode, optc32_set_odm_combine
 *
 * DRR without FW-assisted MCLK switching (no FAMS in this port): the
 * register paths of optc3_set_vtotal_min_max / optc32_setup_manual_trigger,
 * not the DMCUB commands.
 *
 * Instances: OTG n at regOTG0_* + n * (OTG1 - OTG0), ODM / OPTC input n at
 * regODM0_* + n * (ODM1 - ODM0).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCN32_OTG_H
#define RDNA3_DCN32_OTG_H

#include "rdna3_hw.h"
#include "edid.h"
#include "dcn32_dsc.h"   /* RD_PIXEL_ENCODING_* (enum dc_pixel_encoding) */

#ifdef __cplusplus
extern "C" {
#endif

/* enum dc_color_depth (dc_hw_types.h) */
enum {
    RD_COLOR_DEPTH_UNDEFINED = 0,
    RD_COLOR_DEPTH_666,
    RD_COLOR_DEPTH_888,
    RD_COLOR_DEPTH_101010,
    RD_COLOR_DEPTH_121212,
    RD_COLOR_DEPTH_141414,
    RD_COLOR_DEPTH_161616,
    RD_COLOR_DEPTH_999,
    RD_COLOR_DEPTH_111111,
};

/* the part of struct dc_crtc_timing the stream checks look at */
typedef struct rdna3_crtc_format {
    uint32_t pixel_encoding;        /* RD_PIXEL_ENCODING_* */
    uint32_t color_depth;           /* RD_COLOR_DEPTH_* */
    int      dsc;                   /* timing.flags.DSC */
    int      dsc_ycbcr422_simple;   /* dsc_cfg.ycbcr422_simple */
} rdna3_crtc_format;

/* the limits DCN 3.2's timing generator init sets (optc1_validate_timing) */
#define RD_OPTC_MIN_H_BLANK            32
#define RD_OPTC_MIN_V_BLANK            3
#define RD_OPTC_MIN_V_BLANK_INTERLACE  5
#define RD_OPTC_MIN_H_SYNC_WIDTH       4
#define RD_OPTC_MIN_V_SYNC_WIDTH       1

/* optc1_setup_vertical_interrupt0/1/2: the line range of the vertical
 * interrupt (0: start..end; 1, 2: start line) */
void dcn32_otg_setup_vertical_interrupt0(rdna3_dev *d, uint32_t otg, uint32_t start_line, uint32_t end_line);
void dcn32_otg_setup_vertical_interrupt1(rdna3_dev *d, uint32_t otg, uint32_t start_line);
void dcn32_otg_setup_vertical_interrupt2(rdna3_dev *d, uint32_t otg, uint32_t start_line);
/* OTG_VERTICAL_INTERRUPT0_CONTROL: the status latched when the counter is
 * in the range (what irq_service_dcn32's vline0 source reads), and its clear */
int  dcn32_otg_vline0_status(rdna3_dev *d, uint32_t otg);
void dcn32_otg_vline0_clear(rdna3_dev *d, uint32_t otg);

/* optc1_validate_timing for DCN 3.2's limits: 1 = the OTG can run it */
int  dcn32_otg_validate_timing(const rdna3_mode *m);
/* optc1_is_two_pixels_per_container (and the stream encoder's
 * is_two_pixels_per_containter): 4:2:0, or DSC 4:2:2 that is not simple */
int  dcn32_otg_two_pixels_per_container(const rdna3_crtc_format *f);

/* struct crtc_position */
typedef struct rdna3_crtc_position {
    uint32_t horizontal_count, vertical_count, nominal_vcount;
} rdna3_crtc_position;
/* optc1_get_position */
void dcn32_otg_get_position(rdna3_dev *d, uint32_t otg, rdna3_crtc_position *p);
/* optc1_is_counter_moving: two reads of the position differ */
int  dcn32_otg_is_counter_moving(rdna3_dev *d, uint32_t otg);
/* optc1_get_crtc_scanoutpos */
void dcn32_otg_get_scanoutpos(rdna3_dev *d, uint32_t otg, uint32_t *v_blank_start, uint32_t *v_blank_end,
                              uint32_t *h_position, uint32_t *v_position);
/* optc1_get_otg_active_size: 0 when the OTG is off */
int  dcn32_otg_get_active_size(rdna3_dev *d, uint32_t otg, uint32_t *width, uint32_t *height);
/* optc1_is_tg_enabled: OTG_MASTER_EN */
int  dcn32_otg_is_enabled(rdna3_dev *d, uint32_t otg);
/* optc1_wait_for_state: OTG_V_BLANK or OTG_V_ACTIVE_DISP within 100 ms */
enum { RD_CRTC_STATE_VBLANK = 0, RD_CRTC_STATE_VACTIVE };
int  dcn32_otg_wait_for_state(rdna3_dev *d, uint32_t otg, int state);

/* optc1_enable_optc_clock */
void dcn32_otg_enable_optc_clock(rdna3_dev *d, uint32_t otg, int enable);
/* optc1_set_static_screen_control (event mask, frames <= 255) */
#define RD_STATIC_SCREEN_EVENT_MASK_RANGETIMING_DOUBLE_BUFFER_UPDATE_EN 0x100
void dcn32_otg_set_static_screen_control(rdna3_dev *d, uint32_t otg, uint32_t event_triggers, uint32_t num_frames);

/* timing synchronization (enable_timing_synchronization / reset trigger) */
enum { RD_CRTC_EVENT_VSYNC_RISING = 0, RD_CRTC_EVENT_VSYNC_FALLING };
enum { RD_TRIGGER_DELAY_NEXT_PIXEL = 0, RD_TRIGGER_DELAY_NEXT_LINE };
void dcn32_otg_enable_reset_trigger(rdna3_dev *d, uint32_t otg, uint32_t source_otg);
void dcn32_otg_enable_crtc_reset(rdna3_dev *d, uint32_t otg, uint32_t source_otg, int event, int delay);
int  dcn32_otg_did_triggered_reset_occur(rdna3_dev *d, uint32_t otg);
void dcn32_otg_disable_reset_trigger(rdna3_dev *d, uint32_t otg);

/* optc1_is_optc_underflow_occurred / optc1_clear_optc_underflow */
int  dcn32_otg_underflow_occurred(rdna3_dev *d, uint32_t otg);
void dcn32_otg_clear_underflow(rdna3_dev *d, uint32_t otg);

/* optc3_tg_init: DRR timing double buffering at frame start, underflow cleared */
void dcn32_otg_tg_init(rdna3_dev *d, uint32_t otg);
/* optc3_lock / optc1_unlock and optc3_triplebuffer_lock /
 * optc2_triplebuffer_unlock */
void dcn32_otg_lock(rdna3_dev *d, uint32_t otg, int lock);
void dcn32_otg_triplebuffer_lock(rdna3_dev *d, uint32_t otg, int lock);
/* optc3_lock_doublebuffer_enable / _disable */
void dcn32_otg_lock_doublebuffer(rdna3_dev *d, uint32_t otg, int enable);

/* DRR: optc32_set_drr (v_total min / max / mid; 0 = DRR off with the
 * manual trigger still set up), optc3_set_vtotal_min_max,
 * optc3_set_drr_trigger_window, optc3_set_vtotal_change_limit, and the
 * manual trigger (optc2_program_manual_trigger) */
typedef struct rdna3_drr_params {
    uint32_t vertical_total_min, vertical_total_max;
    uint32_t vertical_total_mid, vertical_total_mid_frame_num;
} rdna3_drr_params;
void dcn32_otg_set_drr(rdna3_dev *d, uint32_t otg, const rdna3_drr_params *p);
void dcn32_otg_set_vtotal_min_max(rdna3_dev *d, uint32_t otg, uint32_t vtotal_min, uint32_t vtotal_max);
void dcn32_otg_set_drr_trigger_window(rdna3_dev *d, uint32_t otg, uint32_t start, uint32_t end);
void dcn32_otg_set_vtotal_change_limit(rdna3_dev *d, uint32_t otg, uint32_t limit);
void dcn32_otg_setup_manual_trigger(rdna3_dev *d, uint32_t otg);
void dcn32_otg_program_manual_trigger(rdna3_dev *d, uint32_t otg);

/* optc3_set_out_mux (enum otg_out_mux_dest: 0 DIO, 1 HPO DP) */
void dcn32_otg_set_out_mux(rdna3_dev *d, uint32_t otg, uint32_t dest);
/* optc32_get_double_buffer_pending: OPTC_DOUBLE_BUFFER_PENDING */
int  dcn32_otg_double_buffer_pending(rdna3_dev *d, uint32_t otg);
/* optc32_get_odm_combine_segments: 1, 2, 4 or -1 */
int  dcn32_otg_odm_combine_segments(rdna3_dev *d, uint32_t otg);
/* optc32_set_h_timing_div_manual_mode */
void dcn32_otg_set_h_timing_div_manual_mode(rdna3_dev *d, uint32_t otg, int manual);
/* optc32_set_odm_combine: 2 or 4 OPPs, each 'segment_width' pixels wide */
int  dcn32_otg_set_odm_combine(rdna3_dev *d, uint32_t otg, const uint32_t *opp_id, uint32_t opp_cnt,
                               uint32_t segment_width);

/* The ladder step "otg" (boot-arg rdna3otg=1): what the functions above
 * read on a lit head, without changing what it shows. The vertical
 * interrupt 0 is aimed at the middle of the active area (its interrupt
 * stays disabled) and put back afterwards. */
typedef struct rdna3_otg_check {
    int      moving;                  /* optc1_is_counter_moving */
    uint32_t width, height;           /* optc1_get_otg_active_size (0 x 0: OTG off) */
    uint32_t v_blank_start, v_blank_end, h_position, v_position, nominal_vcount;
    int      vblank_seen, vactive_seen;   /* optc1_wait_for_state */
    int      vline0_seen;             /* the vertical interrupt 0 status latched */
    uint32_t vline0_line, vline0_at;  /* the line it was aimed at, the line read when it latched */
    int      underflow;               /* optc1_is_optc_underflow_occurred */
    int      db_pending;              /* optc32_get_double_buffer_pending */
    int      odm_segments;            /* optc32_get_odm_combine_segments */
} rdna3_otg_check;
/* 0, or RDNA3_ENODEV when the OTG is off */
int  dcn32_otg_check(rdna3_dev *d, uint32_t otg, rdna3_otg_check *c);

#ifdef __cplusplus
}
#endif
#endif
