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
 *   amdgpu_dm_mst_types.c  dm_mst_get_pbn_divider, dm_dp_mst_is_port_support_mode
 *                          (no DSC), kbps_from_pbn
 *   amdgpu_dm.c            dm_encoder_helper_atomic_check's PBN / time slots
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

/* enable_mst_on_sink: DP_MST_EN in DP_MSTM_CTRL */
void dcn32_dp_mst_enable_on_sink(rdna3_dp_link *l, int enable);

/* dm_mst_get_pbn_divider: dc_link_bandwidth_kbps of the link cap / (8 x 1000 x 54) */
uint32_t dcn32_dp_mst_pbn_divider(const rdna3_dp_link *l);

/* the mode check of an MST stream to l->mst_port: dm_dp_mst_is_port_support_mode
 * (the stream within the root link and the port's full PBN), then the PBN
 * (drm_dp_calc_pbn_mode, 8 bpc RGB) and time slots dm_encoder_helper_atomic_check
 * asks for (drm_dp_atomic_find_time_slots, within the 63 of 8b/10b).
 * 0 with *pbn / *slots, or RDNA3_EINVAL. Reads only (the payload state of
 * the port is not kept). */
int  dcn32_dp_mst_mode_fits(rdna3_dp_link *l, uint32_t stream_kbps, uint32_t pixel_khz, uint32_t *pbn,
                            uint32_t *slots);

/* allocate_mst_payload for the stream of mode 'm' needing 'stream_kbps':
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
