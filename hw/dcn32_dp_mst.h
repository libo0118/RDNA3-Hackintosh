/*
 * dcn32_dp_mst.h - the Display Core side of DP MST on DCN 3.2 (one stream
 * on the head's DIG front end), ported from amdgpu (Linux v6.12, MIT):
 *
 *   link_dpms.c            enable_mst_on_sink, allocate_mst_payload,
 *                          deallocate_mst_payload, update_mst_stream_alloc_table,
 *                          remove_stream_from_alloc_table, get_pbn_per_slot,
 *                          get_pbn_from_bw_in_kbps, get_pbn_from_timing, log_vcp_x_y
 *   link_hwss_dio.c        update_dio_stream_allocation_table, set_dio_throttled_vcp_size
 *   dcn10_link_encoder.c   dcn10_link_encoder_update_mst_stream_allocation_table,
 *                          fill_stream_allocation_row_info
 *   dcn10_stream_encoder.c enc1_stream_encoder_set_throttled_vcp_size
 *   amdgpu_dm_helpers.c    dm_helpers_dp_mst_write_payload_allocation_table,
 *                          fill_dc_mst_payload_table_from_drm, dm_helpers_construct_old_payload,
 *                          dm_helpers_dp_mst_poll_for_allocation_change_trigger,
 *                          dm_helpers_dp_mst_send_payload_allocation,
 *                          dm_helpers_dp_mst_update_mst_mgr_for_deallocation
 *                          link_increase_mst_payload, link_reduce_mst_payload
 *   amdgpu_dm_mst_types.c  dm_mst_get_pbn_divider, dm_dp_mst_is_port_support_mode,
 *                          kbps_from_pbn, needs_dsc_aux_workaround,
 *                          is_synaptics_cascaded_panamera, validate_dsc_caps_on_connector,
 *                          retrieve_downstream_port_device, get_fec_overhead_multiplier,
 *                          kbps_to_peak_pbn, set_dsc_configs_from_fairness_vars,
 *                          bpp_x16_from_pbn, increase_dsc_bpp, try_disable_dsc,
 *                          compute_mst_dsc_configs_for_link, is_dsc_need_re_compute,
 *                          compute_mst_dsc_configs_for_state,
 *                          pre_compute_mst_dsc_configs_for_state, is_link_to_dschub,
 *                          is_dsc_precompute_needed, pre_validate_dsc,
 *                          is_dsc_common_config_possible, dp_get_link_current_set_bw
 *   amdgpu_dm_helpers.c    dm_helpers_dp_write_dsc_enable (MST),
 *                          write_dsc_enable_synaptics_non_virtual_dpcd_mst,
 *                          execute_synaptics_rc_command, apply_synaptics_fifo_reset_wa,
 *                          dm_get_adaptive_sync_support_type, dm_is_freesync_pcon_whitelist
 *   amdgpu_dm.c            dm_encoder_helper_atomic_check's PBN / time slots,
 *                          dm_update_mst_vcpi_slots_for_dsc
 *   dc.c                   commit_planes_do_stream_update's dsc_config / mst_bw_update
 *
 * The topology (sideband messages, payload ID table, ACT) is hw/dp_mst.c.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCN32_DP_MST_H
#define RDNA3_DCN32_DP_MST_H

#include "dcn32_dp.h"
#include "dp_mst.h"

#ifdef __cplusplus
extern "C" {
#endif

/* a stream of an atomic commit on an MST link: struct dc_stream_state
 * (its link, its connector = dm_stream_context, the signal, the timing with
 * flags.DSC / dsc_cfg and dsc_cfg.mst_pbn), its CRTC's new state and the
 * dm_connector_state's pbn / vcpi_slots */
typedef struct rdna3_mst_stream {
    rdna3_dp_link *link;
    const rdna3_mst_connector *aconnector;
    int      is_mst;
    rdna3_dsc_timing timing;
    uint32_t mst_pbn;
    int      enable, active, mode_changed, active_changed, connectors_changed;
    int      pbn, vcpi_slots;
} rdna3_mst_stream;

/* struct dc_state: its streams */
typedef struct rdna3_mst_dc_state {
    uint32_t stream_count;
    rdna3_mst_stream *streams[RD_DCN_MAX_PIPES];
} rdna3_mst_dc_state;

/* struct dsc_mst_fairness_vars */
typedef struct rdna3_mst_fairness_vars {
    int pbn, dsc_enabled, bpp_x16;
    const rdna3_mst_connector *aconnector;
} rdna3_mst_fairness_vars;

/* the MST parts of struct drm_atomic_state: dc->current_state and the
 * topology state of each manager the commit touches (one MST hub per DP
 * connector, a commit of this driver touches one; two kept the kernel
 * stack small) */
#define RD_MST_MAX_TOPO 2
typedef struct rdna3_mst_atomic_state {
    const rdna3_mst_dc_state *current;
    uint32_t ntopo;
    struct {
        rdna3_mst_mgr *mgr;
        rdna3_mst_atomic st;
    } topo[RD_MST_MAX_TOPO];
} rdna3_mst_atomic_state;

/* enum adaptive_sync_type (the ones amdgpu_dm sets) */
enum { RD_ADAPTIVE_SYNC_TYPE_NONE = 0, RD_ADAPTIVE_SYNC_TYPE_DP, RD_FREESYNC_TYPE_PCON_IN_WHITELIST,
       RD_FREESYNC_TYPE_PCON_NOT_IN_WHITELIST, RD_ADAPTIVE_SYNC_TYPE_EDP };

/* enable_mst_on_sink: DP_MST_EN in DP_MSTM_CTRL */
void dcn32_dp_mst_enable_on_sink(rdna3_dp_link *l, int enable);

/* dm_mst_get_pbn_divider: dc_link_bandwidth_kbps of the link cap / (8 x 1000 x 54) */
uint32_t dcn32_dp_mst_pbn_divider(const rdna3_dp_link *l);

/* the mode check and atomic check of an MST stream of mode 'm' (8 bpc RGB)
 * to l->mst_port, as amdgpu_dm makes them for a commit that changes this
 * stream alone: create_validate_stream_for_sink's dm_dp_mst_is_port_support_mode
 * (DSC when the end-to-end bandwidth does not carry it), then
 * dm_encoder_helper_atomic_check's PBN (drm_dp_calc_pbn_mode) and time
 * slots, pre_validate_dsc, compute_mst_dsc_configs_for_state (the DSC
 * fairness on a DSC hub), dm_update_mst_vcpi_slots_for_dsc and
 * drm_dp_mst_atomic_check. 0 with *pbn / *slots, *dsc / *cfg (the stream's
 * flags.DSC / dsc_cfg) and *mode_changed (0: the stream runs this timing
 * and DSC config already), or an error. The topology is not changed. */
int  dcn32_dp_mst_mode_fits(rdna3_dp_link *l, const rdna3_mode *m, uint32_t *pbn, uint32_t *slots, int *dsc,
                            rdna3_dsc_config *cfg, int *mode_changed);

/* needs_dsc_aux_workaround: a Synaptics hub (DPCD 1.2 / 1.4) with two sinks or more */
int  dcn32_dp_mst_needs_dsc_aux_workaround(const rdna3_dp_link *l);
/* dm_dp_mst_get_modes' DSC part for output port 'port' of l->mgr:
 * validate_dsc_caps_on_connector and retrieve_downstream_port_device into
 * *aconnector (no DSC: dsc_aux RD_MST_AUX_NONE, the caps zero) */
void dcn32_dp_mst_detect_dsc(rdna3_dp_link *l, rdna3_mst_connector *aconnector, int port);
/* dm_dp_mst_is_port_support_mode: 0 when 'timing' (8 bpc, link_encoding
 * set here) fits port aconnector->port, with timing->dsc / dsc_cfg when it
 * needs DSC for it; RDNA3_EINVAL when it does not fit */
int  dcn32_dp_mst_is_port_support_mode(rdna3_dp_link *l, const rdna3_mst_connector *aconnector,
                                       rdna3_dsc_timing *timing);
/* kbps_to_peak_pbn with the link's get_fec_overhead_multiplier */
int  dcn32_dp_mst_kbps_to_peak_pbn(const rdna3_dp_link *l, uint32_t kbps);
/* drm_dp_mst_atomic_check over every topology of the commit */
int  dcn32_dp_mst_atomic_check(const rdna3_mst_atomic_state *state);
/* pre_validate_dsc: the DSC configs the commit would get computed on a
 * copy of the current state; a stream whose timing (DSC included) stays
 * gets mode_changed 0. 0 or an error */
int  dcn32_dp_mst_pre_validate_dsc(rdna3_mst_atomic_state *state, rdna3_mst_dc_state *new_state,
                                   rdna3_mst_fairness_vars *vars);
/* compute_mst_dsc_configs_for_state: the DSC fairness on each DSC hub
 * whose streams change; each stream's timing.dsc / dsc_cfg / mst_pbn and
 * vars[] (a stream per entry, in link order). 0 or an error */
int  dcn32_dp_mst_compute_dsc_configs_for_state(rdna3_mst_atomic_state *state, rdna3_mst_dc_state *dc_state,
                                                rdna3_mst_fairness_vars *vars);
/* dm_helpers_dp_write_dsc_enable, MST: DP_DSC_ENABLE through the
 * connector's DSC aux (and passthrough aux), the Synaptics workarounds of
 * a hub without virtual DPCD. 1 when written */
int  dcn32_dp_mst_write_dsc_enable(rdna3_dp_link *l, int enable);
/* dm_get_adaptive_sync_support_type: RD_FREESYNC_TYPE_PCON_IN_WHITELIST
 * for a DP-HDMI PCON with adaptive sync SDPs, MSA ignore and a whitelisted
 * branch id, else RD_ADAPTIVE_SYNC_TYPE_NONE */
int  dcn32_dp_adaptive_sync_support_type(const rdna3_dp_link *l);
/* link_increase_mst_payload / link_reduce_mst_payload: the running MST
 * stream's payload resized to the PBN the commit gave it (l->mst_req_pbn)
 * and its VC rate to 'bw_in_kbps'. 0 or an error */
int  dcn32_dp_mst_increase_payload(rdna3_dp_link *l, uint32_t bw_in_kbps);
int  dcn32_dp_mst_reduce_payload(rdna3_dp_link *l, uint32_t bw_in_kbps);
/* the stream update of a running MST DSC stream whose timing stays and
 * whose DSC config changes (dc_update_stream: copy_stream_update_to_stream
 * then commit_planes_do_stream_update's update_dsc_config and
 * mst_bw_update): the new 'cfg' and the PBN 'pbn' without a mode set.
 * 0, or an error with the stream as it was */
int  dcn32_dp_mst_update_stream_dsc(rdna3_dp_link *l, const rdna3_mode *m, const rdna3_dsc_config *cfg, uint32_t pbn);

/* allocate_mst_payload for the stream of mode 'm' needing 'stream_kbps'
 * (its PBN l->mst_req_pbn, or drm_dp_calc_pbn_mode at 8 bpc when 0):
 * the time slots, drm_dp_add_payload_part1 (DPCD 0x1c0..0x1c2), the
 * stream allocation table into DP_MSE_SAT* with the ACT, the ACT handled
 * (0x2c0), ALLOCATE_PAYLOAD, the VC rate (DP_MSE_RATE X.Y). 0 or error. */
int  dcn32_dp_mst_allocate_payload(rdna3_dp_link *l, const rdna3_mode *m, uint32_t stream_kbps);
/* deallocate_mst_payload */
int  dcn32_dp_mst_deallocate_payload(rdna3_dp_link *l);

#ifdef __cplusplus
}
#endif
#endif
