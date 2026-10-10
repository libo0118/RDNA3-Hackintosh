/*
 * dcn32_modeset.h - a new timing on a head the GOP lit (TMDS: DVI or HDMI),
 * without Display Core: the stream is taken down and brought back up the
 * way amdgpu's dc_commit_streams does for a timing change on one stream,
 * with the PHY side through the VBIOS commands the DMCUB executes (as the
 * clone, dcn32_clone.h):
 *
 *   hubp2_set_blank                                    (scanout off)
 *   dcn10_link_encoder_disable_output -> TransmitterControl DISABLE
 *   optc32_disable_crtc                                (OTG + VTG off, OTG_BUSY)
 *   dcn31_program_pix_clk -> set_pixel_clock_v7        (PHY PLL at the new clock)
 *   optc1_program_timing, optc1_program_global_sync, optc1_set_vtg_params
 *   hubp2_program_deadline                             (DLG/TTU for the timing)
 *   dpp1_dscl_set_recout / MPC size, hubp2_program_size (plane = active area)
 *   encoder_control_digx_v1_5 STREAM_SETUP             (DIG front end)
 *   optc32_enable_crtc, TransmitterControl ENABLE, unblank
 *
 * The OTG global sync follows DML 3.2 (dml32 VStartup = MaxVStartupLines =
 * vblank - 1; VUpdateOffset = ceil(htotal / 4); VUpdateWidth and
 * VReadyOffset are fixed times, so the GOP's values scale with the pixel
 * clock; MIN_DST_Y_NEXT_START from TSetup). Of the DLG/TTU registers
 * dml32_rq_dlg_get_dlg_reg derives, the ones that follow from the timing
 * are computed exactly (ref_freq_to_pix_freq, refcyc_per_htotal,
 * refcyc_h_blank_end, dlg_vblank_end, qos_level_high_wm); the bandwidth
 * ones (line / request delivery, prefetch) are the GOP's values rescaled
 * to the new pixel time and width - no full DML. That is a bet a card run
 * checks (underflow, CRC, picture); dcn32_modeset_apply puts the old timing
 * back when the new one does not run.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCN32_MODESET_H
#define RDNA3_DCN32_MODESET_H

#include "dcn32.h"
#include "dcn32_clk.h"
#include "dcn32_mode.h"
#include "dmub32.h"
#include "vbios.h"

#ifdef __cplusplus
extern "C" {
#endif

/* everything one timing needs, as register values */
typedef struct rdna3_modeset_regs {
    uint32_t pclk_khz;
    uint32_t h_active, v_active;
    /* OTG */
    uint32_t h_total, h_blank, h_sync, h_sync_cntl, v_total, v_blank, v_sync, v_sync_cntl;
    uint32_t vstartup, vupdate, vready, vtg;            /* VSTARTUP/VUPDATE/VREADY_PARAM, VTG0_CONTROL */
    /* HUBP DLG/TTU */
    uint32_t blank0, blank1, dst_dim, after_scaler, prefetch, line_pre, line, ref_to_pix;
    uint32_t qos_wm, global_ttu, surf0_cntl0, surf0_cntl1;
    /* plane */
    uint32_t viewport_dim, recout_size, mpc_size, oppbuf;
} rdna3_modeset_regs;

typedef struct rdna3_dcn_modeset {
    uint32_t head, pipe, otg, opp;
    uint32_t fe, be, dig_mode;
    int      hpd;
    uint8_t  encoder_id, connector_id;
    rdna3_dcn_clocks clk;
    rdna3_modeset_regs cur, want;     /* running now (read back), the target */
    /* dcn32_modeset_plan_clk: what apply raises first (0 = enough) */
    uint32_t raise_dispclk_khz, raise_dppclk_khz, pmfw;
} rdna3_dcn_modeset;

/* why a mode is refused for a mode set (0 = can try) */
enum {
    RD_MODESET_OK = 0,
    RD_MODESET_NOT_TMDS = 1,     /* DP head: the link would need training */
    RD_MODESET_NO_TIMING = 2,
    RD_MODESET_INTERLACED = 3,
    RD_MODESET_CLOCKS = 4,       /* DISPCLK / DPPCLK below the pixel rate */
    RD_MODESET_TMDS_CLOCK = 5,   /* above 340 MHz (HDMI 1.4 TMDS, no scrambling) */
    RD_MODESET_SIZE = 6,
    RD_MODESET_BAD_HEAD = 7,
};

/* Read head 'head' of 'dc' (running at 'cur_pclk_khz', from the EDID match
 * or the VBIOS; 0 = unknown) and compute the registers for mode 'm'. 'path'
 * is the head's VBIOS display path (encoder / connector ids, HPD). Reads
 * only. Returns RD_MODESET_*. */
int  dcn32_modeset_plan(rdna3_dev *d, const rdna3_dcn *dc, uint32_t head, const rdna3_display_path *path,
                        uint32_t cur_pclk_khz, const rdna3_mode *m, rdna3_dcn_modeset *ms);
/* The same, and with 'lim' (the PMFW's display clock limits,
 * dcn32_clk_limits_read; NULL or no limits = the plan above) a mode faster
 * than DISPCLK / DPPCLK run now is planned with the clocks DML 3.2 asks
 * for it (dcn32_required_dispclk / _dppclk: pixel clock + downspread +
 * ramping margin, DFS granularity) when they fit under the limits;
 * dcn32_modeset_apply raises them (dcn32_clk_raise) before it touches the
 * stream. RD_MODESET_CLOCKS when they do not fit. */
int  dcn32_modeset_plan_clk(rdna3_dev *d, const rdna3_dcn *dc, uint32_t head, const rdna3_display_path *path,
                            uint32_t cur_pclk_khz, const rdna3_mode *m, const rdna3_dcn_clk_limits *lim,
                            rdna3_dcn_modeset *ms);

/* Program the plan with the surface at MC 'mc' (pitch_px pixels per row,
 * the new active size). After 'settle_ms' the OTG must count frames and
 * the HUBP report no underflow, else the old timing (and 'old_mc' /
 * 'old_pitch_px') goes back and RDNA3_EIO is returned. 'dc' and 'sc' are
 * updated to the new timing on success (the scaler's native size too).
 * A plan with clocks to raise raises them first (they stay up when the old
 * timing comes back); RDNA3_EBUSY, stream untouched, when the PMFW does
 * not set them. */
int  dcn32_modeset_apply(rdna3_dev *d, rdna3_dmub *dm, rdna3_dcn *dc, rdna3_dcn_scale *sc,
                         const rdna3_dcn_modeset *ms, uint64_t mc, uint32_t pitch_px,
                         uint64_t old_mc, uint32_t old_pitch_px, uint32_t settle_ms);

#ifdef __cplusplus
}
#endif
#endif
