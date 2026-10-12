/*
 * dcn32_state.h - display state that survives events which wipe it, and
 * monitor standby on any TMDS head.
 *
 * The port model is amdgpu's suspend / resume of Display Core:
 *   dm_suspend      -> dm->cached_state = the atomic state, then
 *                      amdgpu_dm_commit_zero_streams + dc_set_power_state(D3)
 *   dm_resume       -> dc_set_power_state(D0) (dcn32_init_hw: DCCG, HUBBUB,
 *                      link encoders), then the cached state committed again
 *                      (dc_commit_streams -> dcn20_enable_stream_timing,
 *                      link_set_dpms_on, the plane programming)
 * Without Display Core there is no atomic state to replay, so the "cached
 * state" is what the hardware itself ran: every per-instance register of a
 * lit head's pipe (the lists of gen/dcn_3_2_0_clone.h: HUBP, DPP, MPCC,
 * OPP/FMT, ODM, OTG, DIG front end), the DIG back end's TMDS setup, the
 * routing (MPC tree, ODM, DIG FE/BE), the head's DCCG part, its DET
 * segments, the cursor, the gamma mode, and what the VBIOS commands need
 * (encoder / connector ids, HPD, pixel clock). The global part is the
 * DCHUBBUB system context (hubbub3_init_dchub_sys_ctx), the arbiter /
 * watermark registers and the DCCG globals dccg2_init / dcn32_init_hw set.
 *
 * dcn32_state_restore programs a head from its snapshot in the order of
 * dcn32_modeset_apply (and dcn32_clone_enable for a pipe that lost its
 * power and routing):
 *   dcn32_hubp_pg_control, dcn32_program_det_size        (pipe power, DET)
 *   dccg copies, dcn31_program_pix_clk -> set_pixel_clock_v7
 *   optc1_program_timing + the OTG/ODM/OPP/DPP/HUBP/MPCC registers
 *   enc1_dig_connect_to_otg, encoder_control_digx_v1_5 STREAM_SETUP
 *   DIG_BE_CNTL, TransmitterControl INIT (dcn10_link_encoder_hw_init, only
 *   when the back end lost its setup, as init_hw does after a power loss)
 *   optc32_enable_crtc, TransmitterControl ENABLE, hubp2_set_blank(false)
 *
 * A DP SST head on one pipe is restored with its rdna3_dp_link (the
 * caller's, which knows the sink): the DIG and the PHY are the link's
 * (dcn32_dp_link_set_dpms_off for the TransmitterControl DISABLE, the
 * stream resources and dcn20_enable_stream_timing's clock part -
 * dcn32_dp_acquire_stream, dcn32_dp_enable_stream_timing, DTBCLK again
 * for a 128b/132b link - for the TMDS pixel clock, enc1_dig_connect_to_otg
 * for the HDMI stream setup, and after optc32_enable_crtc
 * dcn32_dp_link_set_dpms_on, which trains the link again, for the
 * TransmitterControl ENABLE), as dc_commit_streams does on dm_resume. The
 * stream's DSC configuration is kept in the snapshot while the link is
 * down (link_set_dpms_off forgets it).
 *
 * dcn32_head_link is link_set_dpms_off / link_set_dpms_on for a TMDS head:
 * hubp2_set_blank + dcn10_link_encoder_disable_output (TransmitterControl
 * DISABLE, skipped when the DIG is already off) and back with
 * dcn10_link_encoder_enable_tmds_output (TransmitterControl ENABLE) +
 * unblank; the OTG and the PHY PLL keep running, as on the clone
 * (dcn32_clone_link). A DP head goes through its link the same way
 * (dcn32_dp_link_set_dpms_off / _on: trained again).
 *
 * Not covered (see docs/DRIVER-PLAN.md, D4): the gamma LUT RAM (only the
 * mode is kept; after a power loss a RAM LUT goes to bypass and the restore
 * logs it, the caller loads its table again), VPG infoframe contents
 * (infoframes off, as the clone and the mode set leave them), DP MST heads
 * and ODM combined heads (the second pipe has no snapshot): those are
 * refused, the caller keeps them blank.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCN32_STATE_H
#define RDNA3_DCN32_STATE_H

#include "dcn32.h"
#include "dcn32_clk.h"
#include "dmub32.h"
#include "dcn32_dp.h"
#include "vbios.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RD_DCN_STATE_REGS    400   /* per-instance registers of one head */
#define RD_DCN_STATE_GLOBALS 96

typedef struct rdna3_dcn_head_state {
    int      valid;
    int      tmds;                    /* DIG mode DVI or HDMI: restore / link possible */
    int      dp;                      /* DIG mode DP SST, one pipe: restore / link with its rdna3_dp_link */
    uint32_t pipe, otg, opp, mpcc;
    uint32_t fe, be, dig_mode;        /* DIG front end, back end (= transmitter), DIG_MODE */
    int      hpd;                     /* HPD n, 0-based; -1 none */
    uint8_t  encoder_id, connector_id;
    uint32_t pclk_khz;
    rdna3_dcn_head geo;               /* the rdna3_dcn_head entry as saved */
    uint32_t cursor_size;             /* rdna3_dcn cursor bookkeeping */
    int      cursor_on;
    /* routing and enables, programmed explicitly */
    uint32_t hubp_cntl, otg_control, vtg_control, odm_select;
    uint32_t mpcc_top, mpcc_bot, mpcc_opp, mpc_out_mux;
    uint32_t dig_fe_cntl, dig_be_cntl;
    uint32_t addr_lo, addr_hi;
    uint32_t det;                     /* DCHUBBUB_DET<pipe>_CTRL */
    uint32_t pg_status;               /* DOMAIN<pipe>_PG_STATUS (0 = powered) */
    /* DCCG of the OTG / pipe (dccg_copy of the clone) */
    uint32_t pixel_rate_cntl, phypll_rate_cntl, dp_dto_phase, dp_dto_modulo;
    uint32_t pixel_rate_k, dppclk_dto, dppclk_dto_ctrl;
    uint32_t ogam_control;            /* MPCC_OGAM<mpcc>_MPCC_OGAM_CONTROL */
    /* a DP head whose link this code took down: the stream's DSC as
     * link_set_dpms_off found it (kept across snapshots of the same pipe) */
    int      dp_held, dp_dsc;
    rdna3_dsc_config dp_dsc_cfg;
    uint32_t nregs;
    uint32_t reg[RD_DCN_STATE_REGS];  /* the lists, in order (dcn32_state.c) */
} rdna3_dcn_head_state;

typedef struct rdna3_dcn_state {
    rdna3_dcn_head_state head[RD_DCN_MAX_PIPES];   /* by rdna3_dcn head index */
    rdna3_dcn_clocks clk;             /* the display clocks at the save */
    uint32_t nglobal;
    uint32_t global[RD_DCN_STATE_GLOBALS];
    uint32_t saves, restores;
} rdna3_dcn_state;

/* Snapshot head 'head' of 'dc' (lit now: dcn32_read_state). 'path' is its
 * VBIOS display path (encoder / connector ids, HPD; NULL: none, then no
 * restore or link control), 'pclk_khz' its pixel clock (0: from the DLG
 * ratio REF_FREQ_TO_PIX_FREQ at the 100 MHz DCHUB reference). A head whose
 * DIG reads off (monitor in standby) keeps the DIG ids of its last
 * snapshot. Also takes the global part. Reads only. */
int  dcn32_state_save(rdna3_dev *d, const rdna3_dcn *dc, uint32_t head, const rdna3_display_path *path,
                      uint32_t pclk_khz, rdna3_dcn_state *st);
/* forget head 'head' (it went away: the clone disabled, ...) */
void dcn32_state_drop(rdna3_dcn_state *st, uint32_t head);

/* What is left of a saved head now. */
typedef struct rdna3_dcn_state_check {
    int      powered;                 /* pipe power domain on */
    int      otg_on;                  /* OTG_MASTER_EN */
    int      blanked;                 /* HUBP_BLANK_EN */
    int      dig_on;                  /* DIG back end enabled (link up) */
    int      timing_ok;               /* OTG totals as saved */
    int      sys_ok;                  /* DCHUBBUB system aperture as saved */
    int      routed;                  /* HUBP -> OTG, MPC tree, DIG FE/BE as saved */
    uint32_t ndiff;                   /* per-instance registers that differ */
    uint32_t frames;                  /* OTG frames counted over the wait */
    int      lost;                    /* needs dcn32_state_restore */
} rdna3_dcn_state_check;
/* Compare head 'head' with its snapshot; with 'wait_ms' the OTG frame
 * counter is read twice. 1 = the head lost its state, 0 = it is there,
 * RDNA3_EINVAL = no snapshot. Reads only. */
int  dcn32_state_check(rdna3_dev *d, const rdna3_dcn_state *st, uint32_t head, uint32_t wait_ms,
                       rdna3_dcn_state_check *k);

/* Put the global part back: the registers that differ from the snapshot
 * (after a reset or a power loss; a no-op when nothing was lost).
 * Returns how many were written. */
int  dcn32_state_restore_global(rdna3_dev *d, const rdna3_dcn_state *st);

/* Program head 'head' from its snapshot (dm: the running DMCUB). 'blank':
 * leave the HUBP blanked (display sleep), 'link_off': leave the
 * transmitter off (monitor standby). 'dp': the head's DP link (a DP SST
 * head; NULL for TMDS). 'dc' gets the head entry back. RDNA3_EINVAL: no
 * snapshot, neither TMDS nor a DP head with its link (MST, ODM: the caller
 * keeps it blank), or no DP link setting carries the mode any more;
 * RDNA3_ENODEV: no DMCUB, RDNA3_EBUSY: the display clocks no longer carry
 * the pixel rate, RDNA3_EIO: the DP link did not train (the stream is on,
 * as DC leaves it), else the first failed VBIOS command / power step. */
int  dcn32_state_restore(rdna3_dev *d, rdna3_dmub *dm, rdna3_dcn *dc, rdna3_dcn_state *st, uint32_t head,
                         int blank, int link_off, rdna3_dp_link *dp);

/* Monitor standby on head 'head' (0: link down, 1: back up) from its
 * snapshot ('dp': a DP head's link, trained again on the way up).
 * RDNA3_EINVAL for a head without one or neither TMDS nor DP with its link. */
int  dcn32_head_link(rdna3_dev *d, rdna3_dmub *dm, rdna3_dcn *dc, rdna3_dcn_state *st, uint32_t head,
                     int on, rdna3_dp_link *dp);

/* amdgpu_dm_commit_zero_streams for one head before a sleep: blank,
 * TransmitterControl DISABLE (a DP head: dcn32_dp_link_set_dpms_off on
 * 'dp'), optc32_disable_crtc. The snapshot must be taken first. */
int  dcn32_state_head_off(rdna3_dev *d, rdna3_dmub *dm, rdna3_dcn *dc, rdna3_dcn_state *st, uint32_t head,
                          rdna3_dp_link *dp);

#ifdef __cplusplus
}
#endif
#endif
