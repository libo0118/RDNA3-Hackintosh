/*
 * dcn32_pstate.h - the display power features of DCN 3.2 that let the
 * memory clock (UCLK) and the fabric clock drop, and the display idle, with
 * displays on (boot-arg rdna3mall=1 in the kext; SubVP: hw/dcn32_subvp.h,
 * rdna3subvp=1). Ported from Linux amdgpu Display Core (v6.12, MIT):
 *
 *   clk_mgr/dcn32/dcn32_clk_mgr.c      dcn32_init_clocks (DPM tables over the
 *                                      DAL mailbox), dcn32_get_memclk_states_from_smu,
 *                                      dcn32_build_wm_range_table,
 *                                      dcn32_notify_wm_ranges, dcn32_update_clocks
 *                                      (its p-state half: FCLK switch allow,
 *                                      DCFCLK / deep sleep / UCLK hard
 *                                      minimums, CAB ways for SubVP,
 *                                      SetAlwaysWaitDmcubResp, DTBCLK),
 *                                      dcn32_update_clocks_update_dtb_dto,
 *                                      dcn32_set_hard_max_memclk,
 *                                      dcn32_clock_read_ss_info, dcn32_enable_pme_wa
 *   clk_mgr/dcn32/dcn32_clk_mgr_smu_msg.c  dcn32_smu_send_fclk_pstate_message,
 *                                      dcn32_smu_send_cab_for_uclk_message,
 *                                      dcn32_smu_transfer_wm_table_dram_2_smu,
 *                                      dcn32_smu_set_pme_workaround
 *   clk_mgr/dcn30/dcn30_clk_mgr_smu_msg.c  dcn30_smu_set_dram_addr_high / _low,
 *                                      dcn30_smu_get_dc_mode_max_dpm_freq,
 *                                      dcn30_smu_set_min_deep_sleep_dcef_clk,
 *                                      dcn30_smu_set_hard_max_by_freq
 *   dml/dcn32/dcn32_fpu.c              dcn32_build_wm_range_table_fpu,
 *                                      build_synthetic_soc_states (+ get_optimal_ntuple,
 *                                      calculate_net_bw_in_kbytes_sec),
 *                                      dcn32_calculate_wm_and_dlg_fpu (the four
 *                                      watermark sets, the dummy p-state index),
 *                                      dcn32_calculate_dlg_params (clocks, MALL sizes)
 *   dml/dcn32/display_mode_vba_util_32.c  dml32_CalculateWatermarksMALLUseAndDRAMSpeedChangeSupport,
 *                                      dml32_CalculateUrgentLatency, dml32_CalculateExtraLatency,
 *                                      dml32_CalculateTWait, dml32_get_return_bw_mbps,
 *                                      dml32_CalculateDCFCLKDeepSleep,
 *                                      dml32_CalculateVUpdateAndDynamicMetadataParameters,
 *                                      dml32_CalculatePrefetchSchedule (its
 *                                      dst_y_prefetch_equ / VRatioPrefetch part),
 *                                      dml32_CalculateSurfaceSizeInMall
 *   resource/dcn32/                    dcn32_set_det_allocations,
 *                                      dcn32_determine_det_override,
 *                                      dcn32_calculate_mall_ways_from_bytes,
 *                                      dcn32_calc_num_avail_chans_for_mall,
 *                                      dcn32_helper_calculate_mall_bytes_for_cursor
 *   hubbub/dcn32/dcn32_hubbub.c        hubbub32_program_watermarks (urgent,
 *                                      stutter, p-state, USR retraining),
 *                                      hubbub32_init_watermarks, hubbub32_wm_read_state,
 *                                      hubbub32_force_wm_propagate_to_pipes,
 *                                      hubbub32_force_usr_retraining_allow,
 *                                      hubbub32_get_mall_en, dcn32_program_det_size,
 *                                      dcn32_program_compbuf_size
 *   hubp/dcn32/dcn32_hubp.c            hubp32_update_force_pstate_disallow,
 *                                      hubp32_update_force_cursor_pstate_disallow,
 *                                      hubp32_update_mall_sel
 *   hwss/dcn32/dcn32_hwseq.c           dcn32_initialize_min_clocks, dcn32_prepare_bandwidth
 *                                      (+ dcn20_prepare_bandwidth / dcn20_optimize_bandwidth),
 *                                      dcn32_update_force_pstate, dcn32_update_mall_sel,
 *                                      dcn32_program_mall_pipe_config,
 *                                      dcn32_calculate_cab_allocation,
 *                                      dcn32_check_no_memory_request_for_cab,
 *                                      dcn32_apply_idle_power_optimizations,
 *                                      dcn32_program_outstanding_updates,
 *                                      dcn32_interdependent_update_lock
 *   dc_dmub_srv.c                      dc_dmub_srv_p_state_delegate (FAMS off)
 *   bios/bios_parser2.c                bios_parser_get_vram_info (vram_info v3.0,
 *                                      umc_info v4.0), get_soc_bb_info_v4_5,
 *                                      get_ss_info_v4_5
 *
 * The decision is DML 3.2's, reduced to the configurations this driver
 * scans out: one to four planes, each a linear 32 bpp (or 64 bpp) surface
 * on its own OTG, no DCC, no GPUVM, no rotation, no MPO, no writeback, one
 * pipe per plane (two with ODM). For those the DML terms that do not
 * apply are constants (swath height 1 line, 64x1 read blocks, no meta /
 * PTE rows, vtaps 1 unscaled, 2 scaled); what stays is ported formula by
 * formula. Simplifications (documented at each place): the prefetch
 * schedule checks dst_y_prefetch_equ / VRatioPrefetch at the maximum
 * VStartup and the prefetch bandwidth against the return bandwidth (no
 * immediate-flip bandwidth: macOS flips at vsync), FractionOfUrgentBandwidth
 * from the steady and prefetch read bandwidth, synchronized timings never
 * assumed (the heads run unsynchronised), FPO (FW-based vblank stretch) not
 * reached (it needs a DRR / FreeSync stream, which this driver sets up
 * only with rdna3vrr=1 (hw/dcn32_freesync.c), and FPO is not ported:
 * dcn32_can_support_mclk_switch_using_fw_based_vblank_stretch returns no
 * candidate).
 *
 * All integer arithmetic (no FPU in the kernel): times in ns, clocks in
 * kHz / MHz, bandwidths in MB/s (= bytes per us).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCN32_PSTATE_H
#define RDNA3_DCN32_PSTATE_H

#include "rdna3_hw.h"
#include "dcn32.h"
#include "dcn32_clk.h"
#include "dmub32.h"
#include "vbios.h"

#ifdef __cplusplus
extern "C" {
#endif

/* DAL mailbox messages of DCN 3.2 (clk_mgr/dcn32/dalsmc.h) */
#define RD_DALSMC_MSG_SetDalDramAddrHigh      0x5
#define RD_DALSMC_MSG_SetDalDramAddrLow       0x6
#define RD_DALSMC_MSG_TransferTableDram2Smu   0x8
#define RD_DALSMC_MSG_SetHardMaxByFreq        0xA
#define RD_DALSMC_MSG_GetDcModeMaxDpmFreq     0xC
#define RD_DALSMC_MSG_SetMinDeepSleepDcfclk   0xD
#define RD_DALSMC_MSG_BacoAudioD3PME          0x10
#define RD_DALSMC_MSG_SetFclkSwitchAllow      0x11
#define RD_DALSMC_MSG_SetCabForUclkPstate     0x12
#define RD_FCLK_PSTATE_NOTSUPPORTED           0x00
#define RD_FCLK_PSTATE_SUPPORTED              0x01
#define RD_DAL_TABLE_WATERMARKS               2      /* TABLE_WATERMARKS */
/* PPCLK_e */
#define RD_PPCLK_UCLK    2
#define RD_PPCLK_FCLK    3
#define RD_PPCLK_SOCCLK  1
/* WATERMARKS_FLAGS_e */
#define RD_WATERMARKS_CLOCK_RANGE  0
#define RD_WATERMARKS_DUMMY_PSTATE 1
#define RD_WATERMARKS_MALL         2
/* WatermarksExternal_t: 4 rows of { WmSetting, Flags, Padding[2] },
 * Spare[16], MmHubPadding[8] */
#define RD_WM_TABLE_BYTES (4 * 4 + 16 * 4 + 8 * 4)

enum { RD_WM_A = 0, RD_WM_B, RD_WM_C, RD_WM_D, RD_WM_SET_COUNT };

#define RD_DCN32_DPM_LEVELS 16
#define RD_DCN32_MAX_STATES 40

/* dcn3_2_soc / dcn3_2_ip: what the decision uses */
typedef struct rdna3_dcn32_soc {
    uint32_t dram_clock_change_latency_ns;   /* 400 us, or the VBIOS's max_mclk_chg_lat */
    uint32_t fclk_change_latency_ns;         /* 25 us */
    uint32_t sr_exit_ns, sr_enter_plus_exit_ns;
    uint32_t urgent_latency_ns;              /* urgent_latency_pixel_data_only_us 4 */
    uint32_t urgent_adj_fclk_component_ns;   /* 1 us */
    uint32_t urgent_adj_fclk_ref_mhz;        /* 3000 */
    uint32_t usr_retraining_ns, smn_latency_ns;
    uint32_t round_trip_ping_cycles;         /* 263 */
    uint32_t num_chans, chan_width_bytes;    /* VBIOS vram_info, else 24 x 2 */
    uint32_t mall_avail_chans;               /* dcn32_calc_num_avail_chans_for_mall */
    uint64_t max_cab_bytes;                  /* dc->caps.max_cab_allocation_bytes */
    uint32_t vbios_bb;                       /* the VBIOS dce_info v4.5 overrode the latencies */
    /* dcn32_clock_read_ss_info */
    int      ss_on_dprefclk;
    uint32_t dprefclk_ss_percentage, dprefclk_ss_divider;
} rdna3_dcn32_soc;

/* the defaults, then the VBIOS (bios_parser_get_vram_info,
 * get_soc_bb_info_v4_5, dcn32_clock_read_ss_info) when 'vb' is not NULL */
void dcn32_pstate_soc_init(rdna3_dcn32_soc *soc, const rdna3_vbios *vb);

/* One voltage state of the bounding box (build_synthetic_soc_states) */
typedef struct rdna3_dcn32_state {
    uint32_t dcfclk_mhz, fclk_mhz, dram_mts;
    uint64_t net_bw_kbps;
} rdna3_dcn32_state;

/* clk_bw_params of the clock manager */
typedef struct rdna3_dcn32_bw {
    uint32_t pmfw;
    int      smu_present, dpm_present;
    uint32_t n_dcfclk, n_socclk, n_dtbclk, n_dispclk, n_dppclk, n_memclk, n_fclk;
    uint32_t dcfclk_mhz[RD_DCN32_DPM_LEVELS], socclk_mhz[RD_DCN32_DPM_LEVELS], dtbclk_mhz[RD_DCN32_DPM_LEVELS];
    uint32_t dispclk_mhz[RD_DCN32_DPM_LEVELS], dppclk_mhz[RD_DCN32_DPM_LEVELS];
    uint32_t memclk_mhz[RD_DCN32_DPM_LEVELS], fclk_mhz[RD_DCN32_DPM_LEVELS];
    /* dc_mode_limit (dcn30_smu_get_dc_mode_max_dpm_freq) */
    uint32_t dcmode_dcfclk_mhz, dcmode_memclk_mhz, dcmode_fclk_mhz, dcmode_dispclk_mhz, dcmode_dppclk_mhz;
    uint32_t dcmode_dtbclk_mhz, dcmode_socclk_mhz;
    uint32_t max_memclk_mhz;
    /* wm_table.nv_entries (dcn32_build_wm_range_table) */
    struct {
        int      valid;
        uint32_t pstate_latency_ns, fclk_change_latency_ns, sr_exit_ns, sr_enter_plus_exit_ns;
        uint8_t  wm_type;
        uint16_t min_dcfclk, max_dcfclk, min_uclk, max_uclk;
    } wm[RD_WM_SET_COUNT];
    /* dummy_pstate_table */
    uint32_t dummy_dram_mts[4], dummy_latency_us[4];
    /* the bounding box's voltage states */
    uint32_t nstates;
    rdna3_dcn32_state state[RD_DCN32_MAX_STATES];
} rdna3_dcn32_bw;

/* dcn32_init_clocks + dcn32_get_memclk_states_from_smu (the DPM levels of
 * DCFCLK, SOCCLK, DTBCLK, DISPCLK, DPPCLK, UCLK, FCLK and their DC mode
 * maxima over the DAL mailbox), dcn32_build_wm_range_table, and the soc
 * states of dcn32_update_bw_bounding_box_fpu (build_synthetic_soc_states).
 * RDNA3_ENODEV when the PMFW does not answer GetSmuVersion (smu_present
 * false: DC then does nothing of this). */
int  dcn32_pstate_clk_init(rdna3_dev *d, const rdna3_dcn32_soc *soc, rdna3_dcn32_bw *bw);
/* dcn32_build_wm_range_table_fpu (also used by the init) */
void dcn32_build_wm_range_table(const rdna3_dcn32_soc *soc, rdna3_dcn32_bw *bw);
/* build_synthetic_soc_states: RDNA3_EINVAL without DCFCLK / DISPCLK / DTBCLK maxima */
int  dcn32_build_soc_states(const rdna3_dcn32_soc *soc, rdna3_dcn32_bw *bw);

/* dcn32_notify_wm_ranges: the WatermarksExternal_t at VRAM offset
 * 'table_off' (RD_WM_TABLE_BYTES, MC address d->fb_start + table_off),
 * SetDalDramAddrHigh / Low, TransferTableDram2Smu(TABLE_WATERMARKS) */
int  dcn32_notify_wm_ranges(rdna3_dev *d, const rdna3_dcn32_bw *bw, uint64_t table_off);
/* the 112-byte table dcn32_notify_wm_ranges writes */
void dcn32_wm_range_table(const rdna3_dcn32_bw *bw, uint8_t out[RD_WM_TABLE_BYTES]);

/* the DAL messages of dcn32_clk_mgr_smu_msg.c / dcn30_clk_mgr_smu_msg.c */
int  dcn32_smu_send_fclk_pstate_message(rdna3_dev *d, int enable);
int  dcn32_smu_send_cab_for_uclk_message(rdna3_dev *d, uint32_t num_ways);
int  dcn32_smu_transfer_wm_table_dram_2_smu(rdna3_dev *d);
int  dcn32_smu_set_pme_workaround(rdna3_dev *d);
/* dcn32_enable_pme_wa: what dce110_enable_audio_stream / _disable_audio_stream
 * send around an Azalia endpoint (the audio path calls it) */
int  dcn32_enable_pme_wa(rdna3_dev *d, const rdna3_dcn32_bw *bw);
int  dcn32_smu_set_min_deep_sleep_dcfclk(rdna3_dev *d, uint32_t mhz);
int  dcn32_smu_set_hard_max_by_freq(rdna3_dev *d, uint32_t ppclk, uint32_t mhz);

/* ---- the decision ---- */

/* enum dm_use_mall_for_pstate_change_mode */
enum { RD_MALL_PSTATE_DISABLE = 0, RD_MALL_PSTATE_FULL_FRAME, RD_MALL_PSTATE_SUB_VIEWPORT, RD_MALL_PSTATE_PHANTOM };
/* enum clock_change_support (dm_dram_clock_change_*) */
enum {
    RD_DRAM_CHANGE_VACTIVE = 0, RD_DRAM_CHANGE_VBLANK, RD_DRAM_CHANGE_VBLANK_DRR,
    RD_DRAM_CHANGE_VACTIVE_W_MALL_FULL_FRAME, RD_DRAM_CHANGE_VBLANK_W_MALL_FULL_FRAME,
    RD_DRAM_CHANGE_VACTIVE_W_MALL_SUB_VP, RD_DRAM_CHANGE_VBLANK_W_MALL_SUB_VP,
    RD_DRAM_CHANGE_UNSUPPORTED
};
/* enum dm_fclock_change_support */
enum { RD_FCLK_CHANGE_VACTIVE = 0, RD_FCLK_CHANGE_VBLANK, RD_FCLK_CHANGE_UNSUPPORTED };

/* one plane = one stream on its own OTG (a DML surface) */
typedef struct rdna3_pstate_plane {
    uint32_t pipe, otg, odm_pipe, dpp_count;  /* dpp_count 2 with ODM 2:1 */
    uint32_t pclk_khz;
    uint32_t h_total, v_total, h_active, v_active, v_front, v_sync;
    uint32_t src_w, src_h;                    /* the surface viewport (scaled: smaller) */
    uint32_t bpp;                             /* bytes per pixel: 4 or 8 */
    uint32_t det_kb;                          /* DET of this plane's pipe(s), all pipes */
    uint32_t vstartup;                        /* OTG VSTARTUP_START of the main pipe (lines) */
    uint32_t cursor_bytes;                    /* the cursor image (pitch x height x Bpp), 0 = off */
    int      blanked;                         /* HUBP blanked: the plane fetches nothing */
    uint32_t mall;                            /* RD_MALL_PSTATE_* */
    uint32_t refresh_hz;                      /* rounded up as DC does */
    uint32_t paired;                          /* SubVP: the paired main / phantom plane's index + 1 (0: none) */
} rdna3_pstate_plane;

/* union dcn_watermark_set member: one set, in ns (frac in 1/1000) */
typedef struct rdna3_dcn_wm {
    uint32_t urgent_ns, urgent_latency_ns;
    uint32_t cstate_enter_plus_exit_ns, cstate_exit_ns;
    uint32_t pstate_change_ns, fclk_pstate_change_ns;
    uint32_t usr_retraining_ns;
    uint32_t frac_urg_bw_nom, frac_urg_bw_flip;
} rdna3_dcn_wm;

/* the DML outputs at one state with one set of latencies */
typedef struct rdna3_dml_wm_out {
    uint32_t urgent_latency_ns, extra_latency_ns;
    uint32_t urgent_ns, usr_ns, dram_ns, fclk_ns, sr_exit_ns, sr_enter_exit_ns;
    uint32_t dcfclk_ds_khz;
    uint64_t return_bw;                       /* MB/s */
    uint64_t total_rd_bw;                     /* MB/s, steady + prefetch (max of both) */
    uint32_t prefetch_mode;                   /* 0..3, 4 = no prefetch schedule */
    uint32_t dram_change, fclk_change;
    int      usr_ok;
    int32_t  active_margin_ns[RD_DCN_MAX_PIPES];
    int32_t  fclk_margin_ns[RD_DCN_MAX_PIPES];
    uint32_t subvp_lines[RD_DCN_MAX_PIPES];   /* SubViewportLinesNeededInMALL */
    uint32_t vblank_ok;                       /* bandwidth / prefetch fit */
} rdna3_dml_wm_out;

typedef struct rdna3_pstate_ctx {
    uint32_t nplanes;
    rdna3_pstate_plane plane[RD_DCN_MAX_PIPES];
    uint32_t dispclk_khz, dppclk_khz;         /* the display clocks running (DSTXAfterScaler, TSetup) */
    /* the result */
    uint32_t vlevel;
    rdna3_dml_wm_out dml;                     /* at vlevel with set A's latencies */
    uint32_t dram_change;                     /* RD_DRAM_CHANGE_* (after SubVP / FPO overrides) */
    int      p_state_change_support, fclk_p_state_change_support, fw_based_mclk_switching;
    uint32_t dcfclk_khz, fclk_khz, dramclk_khz, socclk_khz, dcfclk_deep_sleep_khz, ref_dtbclk_khz;
    uint32_t dummy_latency_index;
    rdna3_dcn_wm wm[RD_WM_SET_COUNT];
    uint64_t mall_ss_size_bytes, mall_subvp_size_bytes;
    uint32_t num_ways;                        /* SubVP (dcn32_helper_calculate_num_ways_for_subvp) */
    uint32_t det_segs[RD_DCN_MAX_PIPES];      /* per pipe (dcn32_set_det_allocations) */
    uint32_t compbuf_kb;
    /* SubVP (hw/dcn32_subvp.c fills these when it adds a phantom) */
    int      subvp;
    uint32_t subvp_plane;                     /* the SubVP main plane */
    uint32_t phantom_plane;                   /* the phantom plane (index in plane[]) */
} rdna3_pstate_ctx;

/* the refresh rate DC's SubVP checks use: (pclk + vt x ht - 1) / (vt x ht) */
uint32_t dcn32_refresh_rate_ceil(const rdna3_pstate_plane *p);

/* dcn32_set_det_allocations / dcn32_determine_det_override for linear
 * planes (no unbounded requesting: linear surfaces): one stream 18
 * segments, else 18 / streams per stream, split over its pipes; a phantom
 * pipe gets none. Fills ctx->det_segs and each plane's det_kb, and
 * ctx->compbuf_kb (config_return_buffer 1280 KiB minus the DETs). */
void dcn32_set_det_allocations(rdna3_pstate_ctx *ctx);

/* dml32 at one state 'st' with the latencies of watermark set 'wm' (the
 * TWait / PrefetchMode search included); 'vlevel' only selects the DRAM
 * efficiency (strobe below 2). Returns 1 when the mode is supported there
 * (bandwidth and prefetch). */
int  dcn32_dml_eval(const rdna3_dcn32_soc *soc, const rdna3_pstate_ctx *ctx, const rdna3_dcn32_state *st,
                    uint32_t vlevel, uint32_t dram_latency_ns, uint32_t fclk_latency_ns, uint32_t sr_exit_ns,
                    uint32_t sr_enter_exit_ns, rdna3_dml_wm_out *o);

/* dcn32_internal_validate_bw + dcn32_calculate_wm_and_dlg_fpu +
 * dcn32_calculate_dlg_params for the planes in 'ctx' (DET as given):
 * the voltage state (the lowest that supports the mode, with p-state
 * support required as dram_clock_change_requirement_final asks unless a
 * plane is SubVP), the clocks, the four watermark sets, the MALL sizes.
 * RDNA3_EINVAL when no state supports the mode. */
int  dcn32_pstate_validate(const rdna3_dcn32_soc *soc, const rdna3_dcn32_bw *bw, rdna3_pstate_ctx *ctx);

/* dcn32_calculate_mall_ways_from_bytes */
uint32_t dcn32_calculate_mall_ways_from_bytes(const rdna3_dcn32_soc *soc, uint64_t bytes);
/* dcn32_helper_calculate_mall_bytes_for_cursor (ARGB8888 cursor) */
uint32_t dcn32_helper_calculate_mall_bytes_for_cursor(uint32_t cursor_bytes, int ignore_cursor_buf);
/* dcn32_calculate_cab_allocation: ways for every plane's surface + cursor */
uint32_t dcn32_calculate_cab_allocation(const rdna3_dcn32_soc *soc, const rdna3_pstate_ctx *ctx);
/* dcn32_check_no_memory_request_for_cab: no plane fetches (every head blanked) */
int  dcn32_check_no_memory_request_for_cab(const rdna3_pstate_ctx *ctx);

/* ---- programming ---- */

/* hubbub32_* watermark state: what is programmed (hubbub2->watermarks) */
typedef struct rdna3_hubbub_wm {
    rdna3_dcn_wm set[RD_WM_SET_COUNT];
    uint32_t det_segs[RD_DCN_MAX_PIPES], compbuf_segs, crb_segs;
    /* the USR retraining allow stays forced (the mirror's rdna3pstate=2/3)
     * instead of DC's debug.force_usr_allow false */
    int      usr_force_keep;
} rdna3_hubbub_wm;

/* hubbub32_program_watermarks: the four sets in DCHUBB REFCLK cycles
 * ('refclk_mhz', 100 on DCN 3.2), raising only unless 'safe_to_lower';
 * then hubbub1_allow_self_refresh_control(true) and
 * hubbub32_force_usr_retraining_allow(false) as DC's defaults. Returns 1
 * when a value waits for the safe-to-lower pass (wm_pending). */
int  hubbub32_program_watermarks(rdna3_dev *d, rdna3_hubbub_wm *hw, const rdna3_dcn_wm wm[RD_WM_SET_COUNT],
                                 uint32_t refclk_mhz, int safe_to_lower);
/* hubbub32_init_watermarks: set A copied to B, C, D */
void hubbub32_init_watermarks(rdna3_dev *d);
/* hubbub32_wm_read_state: raw register values per set (data_urgent,
 * sr_enter, sr_exit, dram_clk_change, usr_retrain, fclk_pstate_change) */
typedef struct rdna3_hubbub_wm_regs {
    uint32_t data_urgent, sr_enter, sr_exit, dram_clk_change, usr_retrain, fclk_pstate_change;
} rdna3_hubbub_wm_regs;
void hubbub32_wm_read_state(rdna3_dev *d, rdna3_hubbub_wm_regs sets[RD_WM_SET_COUNT]);
void hubbub32_force_wm_propagate_to_pipes(rdna3_dev *d, const rdna3_hubbub_wm *hw, uint32_t refclk_mhz);
void hubbub32_force_usr_retraining_allow(rdna3_dev *d, int allow);
/* hubbub3_force_pstate_change_control(force, allow) */
void dcn32_hubbub_force_pstate(rdna3_dev *d, int force, int allow);
/* hubbub32_get_mall_en: MALL_IN_USE && MALL_PREFETCH_COMPLETE */
int  hubbub32_get_mall_en(rdna3_dev *d);
/* dcn32_init_crb's read of the CRB split into 'hw' */
void dcn32_hubbub_read_crb(rdna3_dev *d, rdna3_hubbub_wm *hw);
/* dcn32_program_det_size (segments, rounded up from KiB) and
 * dcn32_program_compbuf_size (growing only with 'safe_to_increase', after
 * the DETs settled) */
void dcn32_program_det_size(rdna3_dev *d, rdna3_hubbub_wm *hw, uint32_t pipe, uint32_t det_kb);
int  dcn32_program_compbuf_size(rdna3_dev *d, rdna3_hubbub_wm *hw, uint32_t compbuf_kb, int safe_to_increase);

/* HUBP (dcn32_hubp.c) */
void hubp32_update_force_pstate_disallow(rdna3_dev *d, uint32_t pipe, int disallow);
void hubp32_update_force_cursor_pstate_disallow(rdna3_dev *d, uint32_t pipe, int disallow);
void hubp32_update_mall_sel(rdna3_dev *d, uint32_t pipe, uint32_t mall_sel, int cache_cursor);

/* dcn32_update_mall_sel: MALL_SEL 1 for a phantom pipe, 2 (MALL for the
 * static screen) when the CAB ways fit, else 0; the cursor cached when
 * bigger than its 16 KiB buffer */
void dcn32_update_mall_sel(rdna3_dev *d, const rdna3_dcn32_soc *soc, const rdna3_pstate_ctx *ctx);
/* dcn32_update_force_pstate: SubVP / FPO main pipes force p-state
 * disallow (allow moderated by the DMCUB), everything else not forced */
void dcn32_update_force_pstate(rdna3_dev *d, const rdna3_pstate_ctx *ctx, const rdna3_pstate_ctx *old);

/* The clock manager's state (clk_mgr_base->clks) */
typedef struct rdna3_dcn32_clks {
    uint32_t dcfclk_khz, dcfclk_deep_sleep_khz, dramclk_khz, socclk_khz, ref_dtbclk_khz;
    int      p_state_change_support, prev_p_state_change_support;
    int      fclk_p_state_change_support, fclk_prev_p_state_change_support;
    int      fw_based_mclk_switching;
    uint32_t num_ways, prev_num_ways;
    int      valid;
    uint32_t msgs;                            /* DAL messages sent (for the report) */
} rdna3_dcn32_clks;

/* dcn32_update_clocks, the p-state half (the DISPCLK / DPPCLK half is
 * hw/dcn32_clk.c's raise path; NumOfDisplays is not sent: it wedged the
 * PMFW on this card, see dcn32_smu_num_displays). 'otg_mask' are the OTGs
 * dcn32_update_clocks_update_dtb_dto reaches (DC's context pipes with a
 * timing generator). */
int  dcn32_clk_update_pstate(rdna3_dev *d, const rdna3_dcn32_bw *bw, rdna3_dcn32_clks *clks,
                             const rdna3_pstate_ctx *ctx, int safe_to_lower, uint32_t otg_mask);
/* dcn32_update_clocks_update_dtb_dto: DTBCLK DTO (no stream pixel rate:
 * the DTO off, PIPE_DTO_SRC_SEL to the DP DTO) on every OTG of 'otg_mask' */
void dcn32_update_clocks_update_dtb_dto(rdna3_dev *d, uint32_t otg_mask, uint32_t ref_dtbclk_khz);
/* dcn32_initialize_min_clocks: the lowest DPM levels as the starting
 * point, p-state and FCLK switching supported, DISPCLK from the dentist
 * (no streams in DC's state yet: no DTB DTO) */
int  dcn32_initialize_min_clocks(rdna3_dev *d, const rdna3_dcn32_bw *bw, rdna3_dcn32_clks *clks);
/* dcn32_set_hard_max_memclk: UCLK hard max at the top level */
int  dcn32_set_hard_max_memclk(rdna3_dev *d, const rdna3_dcn32_bw *bw);

/* dc_dmub_srv_p_state_delegate(should_manage_pstate = false): FAMS off */
int  dcn32_dmub_p_state_delegate_off(rdna3_dmub *dm);
/* DMUB_CMD__CAB_FOR_SS with sub type 'sub' (RD_DMUB_CAB_*) and 'ways' */
#define RD_DMUB_CMD_HW_LOCK                 69
#define RD_DMUB_CMD_CAB_FOR_SS              75
#define RD_DMUB_CMD_FW_ASSISTED_MCLK_SWITCH 76
#define RD_DMUB_CAB_NO_IDLE_OPTIMIZATION    0
#define RD_DMUB_CAB_NO_DCN_REQ              1
#define RD_DMUB_CAB_DCN_SS_FIT_IN_CAB       2
#define RD_DMUB_CAB_DCN_SS_NOT_FIT_IN_CAB   3
#define RD_DMUB_FAMS_SETUP_FW_CTRL          0
#define RD_DMUB_FAMS_HANDLE_SUBVP_CMD       2
int  dcn32_dmub_cab_cmd(rdna3_dmub *dm, uint32_t sub, uint32_t ways);

/* dcn32_apply_idle_power_optimizations: enable -> CAB_NO_DCN_REQ when no
 * plane fetches, CAB_DCN_SS_FIT_IN_CAB(ways) when the surfaces fit the
 * cache; disable -> CAB_NO_IDLE_OPTIMIZATION. Returns 1 when the DMCUB
 * was told (DC's 'true'), 0 when the surfaces do not fit (nothing sent). */
int  dcn32_apply_idle_power_optimizations(rdna3_dmub *dm, const rdna3_dcn32_soc *soc, const rdna3_pstate_ctx *ctx,
                                          int enable);

/* The whole bandwidth update for a new configuration, in DC's order:
 * dcn32_prepare_bandwidth (FAMS off, update_clocks(raise), watermarks
 * (raise), compbuf shrink), the DET changes, dcn32_program_mall_pipe_config
 * (MALL_SEL, FORCE_ONE_ROW_FOR_FRAME for SubVP mains),
 * dcn32_update_force_pstate, then dcn20_optimize_bandwidth (watermarks
 * lowered, compbuf grown, update_clocks(lower)). */
typedef struct rdna3_dcn32_power {
    rdna3_dcn32_soc soc;
    rdna3_dcn32_bw  bw;
    rdna3_dcn32_clks clks;
    rdna3_hubbub_wm hubbub;
    rdna3_pstate_ctx cur;                     /* what runs */
    int      have_cur;
    uint64_t wm_table_off;
    int      idle;                            /* idle optimizations applied (CAB) */
    uint32_t refclk_mhz;
    int      keep_force;                      /* the arbiter's p-state allow stays forced (see dcn32_power_init) */
} rdna3_dcn32_power;

/* dcn32_init_hw's part: clocks (dcn32_init_clocks), the watermark range
 * table to the PMFW (dcn32_notify_wm_ranges), the UCLK hard max,
 * hubbub3_force_pstate_change_control(false, false) unless 'keep_force',
 * the CRB read (dcn32_init_crb), dcn32_initialize_min_clocks.
 * 'keep_force' (the mirror forced the arbiter's p-state allow, which DC
 * never does): the PMFW may then switch UCLK at any moment, which only a
 * configuration whose every plane hides the switch in active tolerates;
 * dcn32_power_prepare keeps UCLK pinned for any other. */
int  dcn32_power_init(rdna3_dev *d, rdna3_dcn32_power *pw, const rdna3_vbios *vb, uint64_t wm_table_off,
                      int keep_force);
/* the planes of the lit heads of 'dc' ('pclk_khz' per head; 0 = from the
 * OTG and the refresh unknown: the head is skipped) */
uint32_t dcn32_pstate_planes_from_heads(rdna3_dev *d, const rdna3_dcn *dc, const uint32_t *pclk_khz,
                                        const uint32_t *cursor_bytes, rdna3_pstate_ctx *ctx);
int  dcn32_power_apply(rdna3_dev *d, rdna3_dmub *dm, rdna3_dcn32_power *pw, rdna3_pstate_ctx *ctx);
/* the two halves: dcn32_prepare_bandwidth + the pipe programming
 * (DET, MALL_SEL, force p-state), and dcn20_optimize_bandwidth (which
 * makes 'ctx' the running state) */
int  dcn32_power_prepare(rdna3_dev *d, rdna3_dmub *dm, rdna3_dcn32_power *pw, rdna3_pstate_ctx *ctx);
int  dcn32_power_optimize(rdna3_dev *d, rdna3_dcn32_power *pw, rdna3_pstate_ctx *ctx);

/* dcn32_program_outstanding_updates: the compbuf size of the state */
int  dcn32_program_outstanding_updates(rdna3_dev *d, rdna3_dcn32_power *pw);
/* dcn32_interdependent_update_lock: the OTG master update lock on every
 * enabled OTG master that is not a phantom (optc3_lock / optc1_unlock) */
void dcn32_interdependent_update_lock(rdna3_dev *d, const rdna3_pstate_ctx *ctx, int lock);

/* the name of a RD_DRAM_CHANGE_* (log / report) */
const char *dcn32_dram_change_name(uint32_t c);

#ifdef __cplusplus
}
#endif
#endif
