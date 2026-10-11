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
 * A DP SST head (DIG_MODE 0) goes through the same path when the caller
 * hands a DP link (dcn32_modeset_plan_dp, hw/dcn32_dp.h, boot-arg
 * rdna3dplink=1 in the kext): the link and the stream are taken down as
 * link_set_dpms_off does, the pixel clock is the DP DTO
 * (dcn31_program_pix_clk) instead of the PHY PLL, the link cap is verified
 * once (dp_verify_link_cap: training with fallback), and link_set_dpms_on
 * brings the link back: link settings for the mode, training, MSA,
 * stream encoder, unblank. Without a link a DP head stays refused.
 * The link may carry the stream with DSC (dp->dsc_policy: the DSC engine,
 * the PPS and DSC in the sink go with the link, hw/dcn32_dsc.h) or as an
 * MST stream (dp->mst: DIG_MODE 5, the VC payload and the ACT,
 * hw/dcn32_dp_mst.h).
 *
 * A TMDS head planned with an HDMI configuration (dcn32_modeset_plan_hdmi,
 * boot-arg rdna3hdmi in the kext) gets the rest of what DC does for an
 * HDMI / DVI stream (hw/dcn32_hdmi.h): link_set_dpms_off's AV mute, the
 * SCDC back to HDMI 1.4 rates, dce110_disable_stream's info packets off
 * and HDMI attribute reset; then enc32_stream_encoder_hdmi_set_stream_attribute
 * after the STREAM_SETUP, write_scdc_data before and read_scdc_data after
 * the transmitter, the AVI / vendor infoframes (dcn30_update_info_frame),
 * AV mute off. With RD_HDMI_20 the mode may need TMDS character rates
 * above 340 MHz (scrambling, the 1/4 clock channel): validated as
 * dcn10_link_encoder_validate_output_with_stream does (600 MHz, the
 * VBIOS's HDMI 6G cap, the sink's EDID max TMDS clock).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCN32_MODESET_H
#define RDNA3_DCN32_MODESET_H

#include "dcn32.h"
#include "dcn32_clk.h"
#include "dcn32_mode.h"
#include "dcn32_dp.h"
#include "dmub32.h"
#include "dcn32_hdmi.h"
#include "vbios.h"

#ifdef __cplusplus
extern "C" {
#endif

/* the HDMI half of DC's TMDS stream (rdna3hdmi) */
#define RD_HDMI_STREAM 0x1   /* stream attributes, infoframes, AV mute, SCDC: as DC */
#define RD_HDMI_20     0x2   /* TMDS character rates above 340 MHz (HDMI 2.0 scrambling) */
typedef struct rdna3_hdmi_cfg {
    uint32_t flags;              /* RD_HDMI_* */
    int32_t  ddc;                /* the connector's DDC line (SCDC), -1 none */
    uint32_t enc_caps;           /* VBIOS encoder caps (RD_ATOM_ENC_CAP_HDMI6G: HDMI_6GB_EN) */
    uint8_t  hdmi_sink;          /* the EDID has an HDMI VSDB (edid_caps.edid_hdmi) */
    uint8_t  lte_340_scramble;   /* HF-VSDB: timing.flags.LTE_340MCSC_SCRAMBLE */
    uint32_t max_tmds_clk_mhz;   /* the EDID's max TMDS clock, 0 unknown */
} rdna3_hdmi_cfg;

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
    /* DP SST head (dcn32_modeset_plan_dp): its link, the running and the
     * new timing as modes (the MSA and the link settings come from them) */
    rdna3_dp_link *dp;
    rdna3_mode dp_cur, dp_want;
    /* DP: the stream's DSC (rdna3dsc: apply_dsc_policy_for_stream) for the
     * new and the running timing; MST: the new stream's PBN and time slots */
    int      dp_want_dsc, dp_cur_dsc;
    rdna3_dsc_config dp_want_dsc_cfg, dp_cur_dsc_cfg;
    uint32_t dp_mst_pbn, dp_mst_slots;
    /* TMDS with an HDMI configuration (dcn32_modeset_plan_hdmi): it, and
     * the new and the running timing as modes (the AVI infoframe) */
    rdna3_hdmi_cfg hdmi;
    rdna3_mode tmds_cur, tmds_want;
} rdna3_dcn_modeset;

/* why a mode is refused for a mode set (0 = can try) */
enum {
    RD_MODESET_OK = 0,
    RD_MODESET_NOT_TMDS = 1,     /* neither DVI / HDMI nor (with a DP link) DP SST */
    RD_MODESET_NO_TIMING = 2,
    RD_MODESET_INTERLACED = 3,
    RD_MODESET_CLOCKS = 4,       /* DISPCLK / DPPCLK below the pixel rate */
    RD_MODESET_TMDS_CLOCK = 5,   /* above 340 MHz (HDMI 1.4 TMDS, no scrambling), or what the
                                  * link encoder refuses (dcn32_modeset_plan_hdmi) */
    RD_MODESET_SIZE = 6,
    RD_MODESET_BAD_HEAD = 7,
    RD_MODESET_LINK_BW = 8,      /* DP: no link setting of the (verified) link cap carries it */
    RD_MODESET_DP_LINK = 9,      /* DP: no sink capabilities read, or the link is not this head's */
    RD_MODESET_DONGLE = 10,      /* DP: beyond the active converter (DP-to-HDMI TMDS / FRL, DFP caps) */
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
/* The same for a head that may be DP SST: with 'dp' (dcn32_dp_link_init
 * for this head's AUX engine, DIG back / front end and OTG, then
 * dcn32_dp_read_caps) a DIG_MODE 0 head is planned too: the mode must fit
 * a link setting of the link cap (verified, else the max one), and a head
 * whose stream encoder runs two pixels per cycle needs even horizontal
 * timings. With dp->dsc_policy (rdna3dsc) a mode the link cannot carry
 * gets DSC when the sink decodes it (dcn32_dp_decide_dsc); with dp->mst
 * (rdna3mst: dp->mgr, dp->mst_port) the head is an MST stream (DIG_MODE 5,
 * or a DIG_MODE 0 head taken over): the mode must fit the port's PBN and
 * the 63 time slots. 'dp' NULL = dcn32_modeset_plan_clk. */
int  dcn32_modeset_plan_dp(rdna3_dev *d, const rdna3_dcn *dc, uint32_t head, const rdna3_display_path *path,
                           rdna3_dp_link *dp, uint32_t cur_pclk_khz, const rdna3_mode *m,
                           const rdna3_dcn_clk_limits *lim, rdna3_dcn_modeset *ms);

/* The same for a TMDS head with the HDMI configuration 'hc' (NULL or no
 * RD_HDMI_STREAM = dcn32_modeset_plan_clk): the TMDS rate is validated as
 * dcn10_link_encoder_validate_output_with_stream does it (RGB 8 bpc; above
 * 340 MHz only with RD_HDMI_20), and dcn32_modeset_apply runs DC's HDMI
 * stream setup (see above). */
int  dcn32_modeset_plan_hdmi(rdna3_dev *d, const rdna3_dcn *dc, uint32_t head, const rdna3_display_path *path,
                             const rdna3_hdmi_cfg *hc, uint32_t cur_pclk_khz, const rdna3_mode *m,
                             const rdna3_dcn_clk_limits *lim, rdna3_dcn_modeset *ms);

/* The running timing of head 'h' at 'pclk_khz' as a mode
 (the one
 * dcn32_modeset_apply puts back when the new one does not run). */
void dcn32_modeset_head_mode(const rdna3_dcn_head *h, uint32_t pclk_khz, rdna3_mode *m);

/* Program the plan with the surface at MC 'mc' (pitch_px pixels per row,
 * DP: the link is trained in it; a failed training counts as a mode that
 * does not run: the old timing comes back, trained again;
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
