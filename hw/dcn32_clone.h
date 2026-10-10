/*
 * dcn32_clone.h - light a second TMDS monitor with the picture of a head the
 * GOP already lit, without a full Display Core: every per-instance register
 * the lit pipe carries (HUBP, DPP, MPCC, OPP/FMT, ODM, OTG, DIG front end)
 * is copied to a free pipe, the routing registers are set for the new
 * instances, and the PHY side - pixel clock, encoder stream setup,
 * transmitter enable - goes through the VBIOS commands the DMCUB executes
 * (the RX 7800 XT VBIOS has no ATOM tables for them, see AFTER-TESTS).
 *
 * The order follows amdgpu's enable path for one stream:
 *   dcn10_power_on_plane_resources / dcn32_hubp_pg_control  (pipe power)
 *   dcn32_program_det_size                                  (DET segments)
 *   dcn31_program_pix_clk -> set_pixel_clock_v7             (PHY PLL, crtc)
 *   dccg2_update_dpp_dto, dccg32_set_pixel_rate_div         (DCCG copies)
 *   optc1_program_timing, optc32_set_odm_bypass             (OTG/ODM copies)
 *   dpp/opp/mpc copies, mpc1_insert_plane                   (MPC tree)
 *   enc1_dig_connect_to_otg, enc1_stream_encoder_hdmi_set_stream_attribute
 *     -> encoder_control_digx_v1_5 STREAM_SETUP
 *   dcn10_link_encoder_setup + connect_dig_be_to_fe         (DIG_BE_CNTL)
 *   dcn10_link_encoder_enable_tmds_output -> transmitter_control_v1_7
 *   optc32_enable_crtc, hubp2_set_blank(false)
 * The surface is the source head's: both monitors show the same picture
 * (mirror) until the framebuffer gives the clone its own surface with
 * dcn32_flip.
 *
 * TMDS only (DIG_MODE DVI or HDMI on both ends, 8 bpc, no infoframes);
 * DP needs link training first.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCN32_CLONE_H
#define RDNA3_DCN32_CLONE_H

#include "dcn32.h"
#include "dcn32_clk.h"
#include "dmub32.h"
#include "vbios.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rdna3_dcn_clone {
    /* source: a lit head (dcn32_read_state) */
    uint32_t src_pipe, src_otg, src_opp, src_mpcc;
    int      src_fe, src_be;
    uint32_t dig_mode;            /* DIG_MODE of the source: 2 DVI, 3 HDMI */
    /* destination */
    uint32_t dst_pipe, dst_otg, dst_opp, dst_mpcc;
    uint32_t dst_fe, dst_be;      /* DIG front end, back end = transmitter */
    uint32_t pll;                 /* COMBO PHY PLL n (ATOM_COMBOPHY_PLL0 + n) */
    int      hpd;                 /* HPD n, 0-based; -1 none */
    uint8_t  encoder_id;          /* ATOM encoder object id (low byte) */
    uint8_t  connector_id;        /* ATOM connector object id (low byte) */
    uint32_t pclk_khz;            /* pixel clock of the mode */
    /* DET segments (64 KiB) before / after the split */
    uint32_t det_src, det_dst, det_src_new, det_dst_new;
    rdna3_dcn_clocks clk;         /* display clocks the GOP left (dcn32_clk_read) */
    uint32_t displays;            /* lit heads with the clone (not sent: see enable) */
    /* set by the caller after the plan when nothing else uses the PMFW
     * mailbox: after each DMCUB VBIOS command a TestMessage checks the PMFW
     * still answers, and the clone stops at the first one that broke it
     * (the first card runs lost the PMFW right after the clone) */
    int      smu_probe;
} rdna3_dcn_clone;

/* Pick free instances for showing lit head 'head' of 'dc' on the connector
 * 'dst' (a VBIOS display path) at 'pclk_khz'. Reads only. RDNA3_EINVAL for
 * a DP source or destination or a bad head, RDNA3_EBUSY when no pipe, OTG
 * or DIG is free, the destination transmitter is already lit, or DISPCLK /
 * DPPCLK are below the pixel rate (dcn32_clk_fits). */
int  dcn32_clone_plan(rdna3_dev *d, const rdna3_dcn *dc, uint32_t head,
                      const rdna3_display_path *dst, uint32_t pclk_khz, rdna3_dcn_clone *c);

/* Program the plan; 'dm' is the running DMCUB (dmub32_hw_init). Returns
 * 0, or the first error (power gate, DET or DMCUB command) after undoing
 * what it can. Logs every step. The PMFW is not told the display count
 * (see dcn32_clone_enable: NumOfDisplays wedged its mailbox on the card). */
int  dcn32_clone_enable(rdna3_dev *d, rdna3_dmub *dm, const rdna3_dcn_clone *c);

/* Is the clone producing the picture? Over 'wait_ms' its OTG frame
 * counter must move, and the OTG CRC (optc1_configure_crc, whole frame)
 * of the clone must equal the boot head's: same surface, same timing,
 * same colour pipeline. A clone that passes but stays black on the
 * monitor points at the PHY / PLL side. Returns 0 when both hold, or when
 * the OTG runs and the CRCs all read zero (no CRC: not compared). */
typedef struct rdna3_clone_check {
    uint32_t frames_src, frames_dst;          /* frames counted over the wait */
    uint32_t crc_src[3], crc_dst[3];          /* R/Cr, G/Y, B/Cb */
    int      running, crc_match;
    int      crc_valid;                       /* any CRC word non-zero */
} rdna3_clone_check;
int  dcn32_clone_verify(rdna3_dev *d, const rdna3_dcn_clone *c, uint32_t wait_ms, rdna3_clone_check *k);

/* Undo: blank the HUBP, stop the OTG, transmitter DISABLE, take the MPCC
 * out of the tree, give the DET segments back. */
int  dcn32_clone_disable(rdna3_dev *d, rdna3_dmub *dm, const rdna3_dcn_clone *c);

#ifdef __cplusplus
}
#endif
#endif
