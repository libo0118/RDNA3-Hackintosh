/*
 * dcn32_subvp.h - Sub-viewport (SubVP) MCLK switching on DCN 3.2 (boot-arg
 * rdna3subvp=1, with rdna3mall=1): for a stream whose DET and vblank do
 * not hide the 400 us UCLK switch (high refresh rates, two unsynchronised
 * heads), a free pipe scans the top lines of the same surface into the
 * MALL ahead of time on a "phantom" OTG with a short timing; the DMCUB
 * firmware then runs the switch while the main pipe reads those lines from
 * the MALL. Ported from Linux amdgpu Display Core (v6.12, MIT):
 *
 *   dml/dcn32/dcn32_fpu.c          dcn32_full_validate_bw_helper's SubVP part,
 *                                  dcn32_enough_pipes_for_subvp,
 *                                  dcn32_assign_subvp_pipe,
 *                                  dcn32_set_phantom_stream_timing,
 *                                  subvp_validate_static_schedulability,
 *                                  subvp_subvp_admissable, subvp_subvp_schedulable,
 *                                  subvp_vblank_schedulable, assign_subvp_index,
 *                                  dcn32_helper_populate_phantom_dlg_params,
 *                                  dcn32_allow_subvp_high_refresh_rate,
 *                                  dcn32_allow_subvp_with_active_margin
 *   resource/dcn32/                dcn32_subvp_in_use, dcn32_subvp_vblank_admissable,
 *                                  dcn32_is_center_timing, dcn32_check_native_scaling_for_res
 *   hwss/dcn32/dcn32_hwseq.c       dcn32_enable_phantom_streams,
 *                                  dcn32_disable_phantom_streams, dcn32_blank_phantom,
 *                                  dcn32_init_blank, dcn32_commit_subvp_config,
 *                                  dcn32_subvp_pipe_control_lock(_fast),
 *                                  dcn32_update_phantom_vp_position,
 *                                  dcn32_apply_update_flags_for_phantom,
 *                                  is_subvp_phantom_topology_transition_seamless,
 *                                  dcn32_is_pipe_topology_transition_seamless,
 *                                  dcn32_program_mall_pipe_config (FORCE_ONE_ROW_FOR_FRAME),
 *                                  dcn32_set_input_transfer_func, dcn32_set_mcm_luts,
 *                                  dcn32_set_mpc_shaper_3dlut (the phantom plane's color:
 *                                  predefined degamma, no LUTs)
 *   dpp/dcn30/dcn30_dpp.c          dpp3_set_pre_degam, dpp3_program_gamcor_lut (bypass)
 *   mpc/dcn32/dcn32_mpc.c          mpc32_program_1dlut / _shaper / _3dlut (bypass)
 *   hubp/dcn32/dcn32_hubp.c        hubp32_phantom_hubp_post_enable,
 *                                  hubp32_prepare_subvp_buffering
 *   optc/dcn32/dcn32_optc.c        optc32_phantom_crtc_post_enable,
 *                                  optc32_disable_phantom_otg
 *   opp/dcn20/dcn20_opp.c          opp2_set_disp_pattern_generator (solid
 *                                  colour), opp2_dpg_set_blank_color,
 *                                  opp2_dpg_is_blanked, opp2_dpg_is_pending,
 *                                  opp2_program_dpg_dimensions
 *   hwss/dcn20/dcn20_hwseq.c       dcn20_wait_for_blank_complete
 *   dc_dmub_srv.c                  dc_dmub_setup_subvp_dmub_command,
 *                                  populate_subvp_cmd_pipe_info,
 *                                  populate_subvp_cmd_vblank_pipe_info,
 *                                  update_subvp_prefetch_end_to_mall_start
 *   dmub/src/dmub_dcn32.c          dmub_dcn32_save_surf_addr (dmub_srv_subvp_save_surf_addr)
 *   dce/dmub_hw_lock_mgr.c         dmub_hw_lock_mgr_inbox0_cmd (hw/dmub32.c)
 *
 * The phantom pipe is brought up from the main pipe's registers the way
 * hw/dcn32_odm.c brings up an ODM pipe (power, DPP DTO, HUBP / DPP / MPCC
 * / OPP copies, its own MPC tree), on its own OTG (OTG = pipe index, as
 * DC's free pipes) with the phantom timing and the DP DTO as its pixel
 * clock (dcn31_program_pix_clk for SIGNAL_TYPE_VIRTUAL), DET 0, MALL_SEL
 * 1, blanked. Its DLG / TTU are the main pipe's with the phantom's
 * vertical timing (same pixel clock and line: the horizontal terms do not
 * change; DML's vertical ones - vstartup, vblank end, the next start - are
 * recomputed for the phantom), not a full dml32_rq_dlg_get_dlg_reg.
 *
 * Not taken: SubVP + DRR (no DRR stream here), a SubVP main on an ODM or
 * scaled ("center timing") head, SubVP while the arbiter's p-state allow
 * is forced (the mirror's rdna3pstate). The phantom's prefetch (DLG
 * VBLANK / prefetch registers) is the main's: its VSTARTUP window is at
 * least as long (back porch = the main's VSTARTUP), so the main's
 * schedule fits; DC recomputes it with DML.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCN32_SUBVP_H
#define RDNA3_DCN32_SUBVP_H

#include "dcn32_pstate.h"

#ifdef __cplusplus
extern "C" {
#endif

/* dc->caps of DCN 3.2 (dcn32_resource_construct) */
#define RD_SUBVP_FW_PROCESSING_DELAY_US       15
#define RD_SUBVP_PREFETCH_END_TO_MALL_START_US 15
#define RD_SUBVP_SWATH_HEIGHT_MARGIN_LINES    16
#define RD_SUBVP_PSTATE_ALLOW_WIDTH_US        20
#define RD_SUBVP_VERTICAL_INT_MARGIN_US       30
#define RD_DCN3_2_MAX_SUBVP_PIXEL_RATE_MHZ    1800
#define RD_SUBVP_MAIN_SPLIT_NONE              0xF
/* enum mclk_switch_mode */
#define RD_MCLK_SWITCH_SUBVP  2
#define RD_MCLK_SWITCH_VBLANK 3

/* one phantom stream (the phantom of ctx->plane[main]) */
typedef struct rdna3_subvp_phantom {
    uint32_t main, phantom;            /* plane indices in the ctx */
    uint32_t main_pipe, main_otg, main_opp;
    uint32_t pipe;                     /* phantom pipe = OTG = OPP = MPCC */
    uint32_t hactive;
    uint32_t vactive, vfront, vsync, vbp, vtotal;   /* the phantom timing */
    uint32_t vp_y;                     /* the main viewport's y start (dcn32_update_phantom_vp_position) */
    uint32_t subvp_index;              /* assign_subvp_index */
} rdna3_subvp_phantom;

typedef struct rdna3_subvp_state {
    uint32_t n;
    rdna3_subvp_phantom ph[2];
    uint8_t  cmd[RD_DMUB_RB_CMD_SIZE]; /* the last DMUB_CMD__HANDLE_SUBVP_CMD */
    int      enabled;                  /* phantoms programmed */
    uint32_t locks;                    /* DMUB HW lock commands sent */
} rdna3_subvp_state;

/* dcn32_subvp_in_use */
int  dcn32_subvp_in_use(const rdna3_pstate_ctx *ctx);
/* dcn32_check_native_scaling_for_res / dcn32_is_center_timing for a plane */
int  dcn32_check_native_scaling_for_res(const rdna3_pstate_plane *p, uint32_t w, uint32_t h);
int  dcn32_is_center_timing(const rdna3_pstate_plane *p);
/* dcn32_allow_subvp_high_refresh_rate: 120..175 Hz, 3840x2160 / 3440x1440 /
 * 2560x1440 / 1920x1080 unscaled, every stream at least 120 Hz */
int  dcn32_allow_subvp_high_refresh_rate(const rdna3_pstate_ctx *ctx, const rdna3_pstate_plane *p);
/* dcn32_allow_subvp_with_active_margin: 55..65 Hz at 2560x1440 / 1920x1080 */
int  dcn32_allow_subvp_with_active_margin(const rdna3_pstate_plane *p);
/* the pipes no plane (nor its ODM pipe, nor a head in 'used_mask') uses */
uint32_t dcn32_subvp_free_pipes(const rdna3_pstate_ctx *ctx, uint32_t used_mask);

/* dcn32_full_validate_bw_helper's SubVP part on a validated 'ctx' whose
 * p-state switch is not supported (or runs at a higher UCLK than state 0):
 * phantoms for the planes dcn32_assign_subvp_pipe picks while there are
 * free pipes (not in 'used_mask'), validated again, kept when statically
 * schedulable and on a lower UCLK. 'gecc' is the DMCUB's gecc_enable
 * (SubVP off). Returns 1 with 'ctx' extended (phantom planes appended,
 * mains marked) and 'sv' filled, else 0 with 'ctx' as it was. */
int  dcn32_subvp_validate(const rdna3_dcn32_soc *soc, const rdna3_dcn32_bw *bw, rdna3_pstate_ctx *ctx,
                          uint32_t used_mask, int gecc, rdna3_subvp_state *sv);

/* dc_dmub_setup_subvp_dmub_command: the 64-byte DMUB_CMD__FW_ASSISTED_MCLK_SWITCH
 * / HANDLE_SUBVP_CMD for 'ctx' ('enable' 0: the header only, SubVP off) */
void dcn32_subvp_build_cmd(const rdna3_pstate_ctx *ctx, const rdna3_subvp_state *sv, uint32_t refclk_mhz, int enable,
                           uint8_t cmd[RD_DMUB_RB_CMD_SIZE]);
/* dcn32_commit_subvp_config: the command to the DMCUB */
int  dcn32_commit_subvp_config(rdna3_dmub *dm, const rdna3_pstate_ctx *ctx, rdna3_subvp_state *sv, uint32_t refclk_mhz);
/* dcn32_subvp_pipe_control_lock: the DMCUB HW lock (inbox0) around a full
 * update while SubVP runs or ran; dcn32_subvp_pipe_control_lock_fast:
 * around an immediate flip of a SubVP pipe */
int  dcn32_subvp_pipe_control_lock(rdna3_dmub *dm, rdna3_subvp_state *sv, int subvp_in_use, int subvp_prev_use,
                                   int lock);
int  dcn32_subvp_pipe_control_lock_fast(rdna3_dmub *dm, rdna3_subvp_state *sv, int subvp_immediate_flip, int lock);
/* dmub_dcn32_save_surf_addr: the main pipe's new surface for the DMCUB
 * (SCRATCH9/11 or 12/13 for index 0, 18/19 or 20/22 for index 1, the
 * select toggling in SCRATCH15 / 23) */
void dmub_dcn32_save_surf_addr(rdna3_dev *d, uint64_t addr, uint64_t meta_addr, uint32_t subvp_index);

/* OPP display pattern generator (opp2_*) and the blank paths using it */
void dcn32_opp_dpg_blank(rdna3_dev *d, uint32_t opp, uint32_t width, uint32_t height);
void dcn32_opp_dpg_off(rdna3_dev *d, uint32_t opp);
int  dcn32_subvp_dpg_is_blanked(rdna3_dev *d, uint32_t opp);
/* dcn20_wait_for_blank_complete: until no DPG update pends (1000 x 100 us) */
int  dcn32_subvp_wait_for_blank_complete(rdna3_dev *d, uint32_t opp);
/* dcn32_init_blank: black from the DPG of the OPP(s) OTG 'otg' reads, at
 * its active size (half each with two segments) */
int  dcn32_init_blank(rdna3_dev *d, uint32_t otg);
/* dcn32_blank_phantom */
int  dcn32_blank_phantom(rdna3_dev *d, uint32_t otg, uint32_t width, uint32_t height);

/* the plane's input color as DC programs it for every plane (the phantom's
 * too): enum dc_transfer_func_predefined */
enum { RD_TF_SRGB = 0, RD_TF_BT709, RD_TF_PQ, RD_TF_LINEAR, RD_TF_UNITY, RD_TF_HLG, RD_TF_HLG12, RD_TF_GAMMA22,
       RD_TF_GAMMA24, RD_TF_GAMMA26 };
/* the predefined transfer function a CNVC_CFG0_PRE_DEGAM value selects */
uint32_t dcn32_pre_degam_tf(uint32_t pre_degam);
/* dcn32_set_input_transfer_func for a plane with a predefined (or no)
 * input transfer function: dpp3_set_pre_degam, no GAMCOR curve, then
 * dcn32_set_mcm_luts */
int  dcn32_set_input_transfer_func(rdna3_dev *d, uint32_t dpp, uint32_t mpcc, uint32_t tf);
/* dcn32_set_mcm_luts without blend / shaper / 3D LUTs: the MPCC's MCM 1D
 * LUT, shaper and 3D LUT in bypass */
int  dcn32_set_mcm_luts(rdna3_dev *d, uint32_t mpcc);
/* dcn32_set_mpc_shaper_3dlut: programs only a stream's initialized 3D
 * LUT (with its shaper), which macOS never gives: 0, nothing written */
int  dcn32_set_mpc_shaper_3dlut(rdna3_dev *d, uint32_t mpcc, int lut3d_initialized);

/* HUBP / OTG pieces */
void hubp32_phantom_hubp_post_enable(rdna3_dev *d, uint32_t pipe);
void hubp32_prepare_subvp_buffering(rdna3_dev *d, uint32_t pipe, int enable);
void optc32_phantom_crtc_post_enable(rdna3_dev *d, uint32_t otg);
void optc32_disable_phantom_otg(rdna3_dev *d, uint32_t otg);

/* dcn32_apply_update_flags_for_phantom: what a phantom pipe programs
 * (every step of a full enable) */
#define RD_PIPE_UPD_ENABLE        0x001
#define RD_PIPE_UPD_MPCC          0x002
#define RD_PIPE_UPD_DPPCLK        0x004
#define RD_PIPE_UPD_HUBP_INTERDEP 0x008
#define RD_PIPE_UPD_HUBP_RQ_DLG   0x010
#define RD_PIPE_UPD_GAMUT_REMAP   0x020
#define RD_PIPE_UPD_SCALER        0x040
#define RD_PIPE_UPD_VIEWPORT      0x080
#define RD_PIPE_UPD_DET_SIZE      0x100
#define RD_PIPE_UPD_ODM           0x200
#define RD_PIPE_UPD_GLOBAL_SYNC   0x400
uint32_t dcn32_apply_update_flags_for_phantom(int otg_master);
/* dcn32_update_phantom_vp_position: the phantom's viewport follows the
 * main plane's position (y start) */
void dcn32_update_phantom_vp_position(rdna3_dev *d, rdna3_subvp_phantom *ph, uint32_t main_vp_y);

/* dcn32_enable_phantom_streams + the phantom front end: every phantom of
 * 'sv' brought up (see the header comment), then
 * dcn32_program_mall_pipe_config's FORCE_ONE_ROW_FOR_FRAME / CURSOR_REQ_MODE
 * on the mains. 'dprefclk_khz' is the DP DTO reference. */
int  dcn32_enable_phantom_streams(rdna3_dev *d, const rdna3_pstate_ctx *ctx, rdna3_subvp_state *sv,
                                  uint32_t dprefclk_khz);
/* dcn32_disable_phantom_streams + the phantom planes off, SubVP buffering
 * off on the mains */
void dcn32_disable_phantom_streams(rdna3_dev *d, rdna3_subvp_state *sv);

/* is_subvp_phantom_topology_transition_seamless /
 * dcn32_is_pipe_topology_transition_seamless over two configurations:
 * every pipe free in either, or the same OTG master stream (the same head
 * on the same pipe and OTG), or a phantom of the same main */
int  is_subvp_phantom_topology_transition_seamless(const rdna3_pstate_ctx *cur, const rdna3_pstate_ctx *next,
                                                   uint32_t cur_k, uint32_t next_k);
int  dcn32_is_pipe_topology_transition_seamless(const rdna3_pstate_ctx *cur, const rdna3_pstate_ctx *next);

/* The update in DC's order around the DMCUB: HW lock (while SubVP runs
 * or ran), dcn32_power_prepare, the old phantoms off / the new ones up
 * (kept when the same), dcn32_commit_subvp_config, unlock after the
 * SubVP mains reached vblank, dcn32_power_optimize. 'cur' is what runs
 * and becomes 'next'. */
int  dcn32_subvp_commit(rdna3_dev *d, rdna3_dmub *dm, rdna3_dcn32_power *pw, rdna3_pstate_ctx *ctx,
                        rdna3_subvp_state *cur, rdna3_subvp_state *next, uint32_t dprefclk_khz);
/* the subvp_index of the SubVP main on 'main_pipe' (for a flip:
 * dmub_dcn32_save_surf_addr), or -1 */
int  dcn32_subvp_index_of(const rdna3_subvp_state *sv, uint32_t main_pipe);

#ifdef __cplusplus
}
#endif
#endif
