/*
 * dcn32_freesync.h - FreeSync / VRR (adaptive sync) for DCN 3.2, ported
 * from Linux amdgpu (v6.12, MIT):
 *
 *   drm_edid.c        drm_get_monitor_range, get_monitor_range,
 *                     drm_for_each_detailed_block, cea_for_each_detailed_block,
 *                     is_display_descriptor (DRM's monitor_range, which
 *                     amdgpu_dm reads)
 *   drm_modes.c       drm_mode_vrefresh
 *   amdgpu_dm.c       amdgpu_dm_update_freesync_caps, parse_edid_displayid_vrr,
 *                     parse_hdmi_amd_vsdb, parse_edid_cea, parse_edid_cea_dmub,
 *                     dm_edid_parser_send_cea, get_freesync_config_for_crtc,
 *                     update_stream_irq_parameters, update_freesync_state_on_stream,
 *                     is_dc_timing_adjust_needed, amdgpu_dm_crtc_vrr_active
 *   freesync.c        mod_freesync_calc_nominal_field_rate,
 *                     mod_freesync_calc_v_total_from_refresh,
 *                     calc_duration_in_us_from_refresh_in_uhz,
 *                     vrr_settings_require_update, mod_freesync_build_vrr_params,
 *                     mod_freesync_build_vrr_infopacket, build_vrr_infopacket_v1,
 *                     build_vrr_infopacket_v2, build_vrr_infopacket_v3,
 *                     build_vrr_infopacket_header_v1, build_vrr_infopacket_header_v2,
 *                     build_vrr_infopacket_header_v3, build_vrr_infopacket_data_v1,
 *                     build_vrr_infopacket_data_v3, build_vrr_infopacket_checksum,
 *                     build_vrr_infopacket_sdp_v1_3, mod_freesync_get_freesync_enabled
 *   info_packet.c     mod_build_adaptive_sync_infopacket,
 *                     mod_build_adaptive_sync_infopacket_v1
 *   dc_resource.c     set_spd_info_packet, set_adaptive_sync_info_packet,
 *                     adaptive_sync_override_dp_info_packets_sdp_line_num
 *   dc.c              dc_stream_adjust_vmin_vmax
 *   dcn10_hwseq.c     dcn10_set_drr (DCN 3.2's hwss.set_drr)
 *
 * What amdgpu does for a FreeSync stream, without its per-frame parts:
 * the sink's refresh range (the EDID range descriptor / DRM's
 * monitor_range for DP with MSA_TIMING_PAR_IGNORED, the AMD VSDB through
 * the DMCUB's EDID CEA parser for HDMI and for a whitelisted DP-HDMI PCON),
 * the stream's VRR state (ACTIVE_VARIABLE when enabled, else INACTIVE),
 * the v_total range (vmin from the max refresh, vmax from the min), the
 * FreeSync SPD infopacket (and the adaptive sync SDP v1 of a PCON), DRR on
 * the OTG (optc32_set_drr + the static screen trigger) and the manual
 * trigger a flip fires (commit_planes_for_stream). Not ported: below the
 * range compensation (BTR: mod_freesync_handle_preflip / _handle_v_update,
 * which run per flip and per vblank interrupt), the fixed-refresh ramp,
 * FreeSync video modes (VRR_STATE_ACTIVE_FIXED from userspace), the
 * long-vtotal path (set_long_vtotal), FreeSync 2 HDR transfer functions
 * (app_tf is TRANSFER_FUNC_UNKNOWN as amdgpu_dm passes it), eDP replay and
 * the DP adaptive sync SDP v2 (amdgpu_dm never builds it for DCN 3.2), DRM's
 * VTB extension blocks.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCN32_FREESYNC_H
#define RDNA3_DCN32_FREESYNC_H

#include "rdna3_hw.h"
#include "edid.h"
#include "dmub32.h"
#include "dcn32_hdmi.h"     /* rdna3_info_packet, RD_SIGNAL_* */
#include "dcn32_dp_mst.h"   /* RD_ADAPTIVE_SYNC_TYPE_*, RD_FREESYNC_TYPE_* */

#ifdef __cplusplus
extern "C" {
#endif

/* dc->caps.max_v_total of DCN 3.2 (dcn32_resource_construct) */
#define RD_DCN32_MAX_V_TOTAL ((1u << 15) - 1)
/* freesync.c MIN_REFRESH_RANGE, amdgpu_dm's "max - min > 10" */
#define RD_FREESYNC_MIN_REFRESH_RANGE 10
/* dcn10_set_drr: OTG_TRIG_A (bit 11) as the static screen event, 2 frames */
#define RD_DRR_EVENT_TRIGGERS 0x800
#define RD_DRR_NUM_FRAMES     2
/* DMUB_CMD__EDID_CEA and its reply types (dmub_cmd.h) */
#define RD_DMUB_CMD_EDID_CEA            79
#define RD_DMUB_EDID_CEA_DATA_CHUNK     8
#define RD_DMUB_EDID_CEA_AMD_VSDB       1
#define RD_DMUB_EDID_CEA_ACK            2
/* struct dmub_rb_cmd_edid_cea: payload bytes after the header (the
 * 16-byte input of the packed union) */
#define RD_DMUB_EDID_CEA_PAYLOAD        16
/* HDMI_AMD_VENDOR_SPECIFIC_DATA_BLOCK_IEEE_REGISTRATION_ID */
#define RD_AMD_VSDB_IEEE_ID 0x00001A

/* where the range came from (the log) */
enum {
    RD_VRR_FROM_NONE = 0,
    RD_VRR_FROM_MONITOR_RANGE,      /* DP, EDID 1.4 continuous frequency: DRM's monitor_range */
    RD_VRR_FROM_RANGE_DESCRIPTOR,   /* DP, EDID 1.2+: amdgpu_dm's own range descriptor loop */
    RD_VRR_FROM_AMD_VSDB,           /* HDMI: the AMD VSDB (DMCUB EDID CEA parser) */
    RD_VRR_FROM_PCON_VSDB,          /* a whitelisted DP-HDMI PCON: the AMD VSDB */
};

/* enum mod_vrr_state */
enum {
    RD_VRR_STATE_UNSUPPORTED = 0,
    RD_VRR_STATE_DISABLED,
    RD_VRR_STATE_INACTIVE,
    RD_VRR_STATE_ACTIVE_VARIABLE,
    RD_VRR_STATE_ACTIVE_FIXED,
};

/* enum vrr_packet_type */
enum {
    RD_PACKET_TYPE_VRR = 0,
    RD_PACKET_TYPE_FS_V1,
    RD_PACKET_TYPE_FS_V2,
    RD_PACKET_TYPE_FS_V3,
    RD_PACKET_TYPE_VTEM,
};

/* struct amdgpu_hdmi_vsdb_info, the FreeSync fields */
typedef struct rdna3_vsdb_info {
    uint32_t amd_vsdb_version;
    int      freesync_supported;
    uint32_t min_refresh_rate_hz, max_refresh_rate_hz;
} rdna3_vsdb_info;

/* what amdgpu_dm_update_freesync_caps leaves in the connector:
 * display_info.monitor_range, amdgpu_dm_connector's min/max_vfreq, as_type,
 * pack_sdp_v1_3 and vsdb_info, dm_con_state->freesync_capable */
typedef struct rdna3_freesync_caps {
    uint32_t monitor_min_vfreq, monitor_max_vfreq;
    uint32_t min_vfreq, max_vfreq;
    int      freesync_capable;
    int      as_type;
    int      pack_sdp_v1_3;
    rdna3_vsdb_info vsdb_info;
    uint32_t from;                  /* RD_VRR_FROM_* */
    int      hdmi_vsdb;             /* parse_hdmi_amd_vsdb's result: extension index, -1 none,
                                     * RD_AMD_VSDB_PARSER_FAILED */
} rdna3_freesync_caps;

/* struct mod_freesync_config */
typedef struct rdna3_freesync_config {
    uint32_t state;                 /* RD_VRR_STATE_* */
    int      vsif_supported, ramping, btr;
    uint32_t min_refresh_in_uhz, max_refresh_in_uhz, fixed_refresh_in_uhz;
} rdna3_freesync_config;

/* struct dc_crtc_timing_adjust */
typedef struct rdna3_crtc_timing_adjust {
    uint32_t v_total_min, v_total_max, v_total_mid, v_total_mid_frame_num;
    int      allow_otg_v_count_halt;
} rdna3_crtc_timing_adjust;

/* struct mod_vrr_params (without the flip interval work-around) */
typedef struct rdna3_vrr_params {
    int      supported, send_info_frame;
    uint32_t state;
    uint32_t min_refresh_in_uhz, max_duration_in_us;
    uint32_t max_refresh_in_uhz, min_duration_in_us;
    uint32_t fixed_refresh_in_uhz;
    rdna3_crtc_timing_adjust adjust;
    struct {
        int      fixed_active, ramping_active;
        uint32_t target_refresh_in_uhz;
    } fixed;
    struct {
        int      btr_enabled, btr_active;
        uint32_t mid_point_in_us, inserted_duration_in_us, frames_to_insert, frame_counter, margin_in_us;
    } btr;
} rdna3_vrr_params;

/* the parts of struct dc_stream_state the FreeSync code reads and sets */
typedef struct rdna3_vrr_stream {
    uint32_t pix_clk_100hz, h_total, v_total;
    uint32_t v_addressable, v_front_porch;
    uint32_t signal;                /* RD_SIGNAL_* */
    int      freesync_on_desktop;   /* never set by amdgpu_dm: 0 */
    /* set from the state */
    int      ignore_msa_timing_param, allow_freesync;
    rdna3_info_packet vrr_infopacket, adaptive_sync_infopacket;
} rdna3_vrr_stream;

/* the FreeSync state of one CRTC (dm_crtc_state + dm_irq_params) */
typedef struct rdna3_freesync_crtc {
    int      vrr_supported;
    rdna3_freesync_config config;
    rdna3_vrr_params vrr;
    uint32_t packet_type;           /* RD_PACKET_TYPE_* */
} rdna3_freesync_crtc;

/* ---- the sink's range ---- */

/* drm_get_monitor_range: EDID 1.4 with continuous frequency, a "range
 * limits only" display range descriptor in the base block or a CTA
 * extension's detailed timing area; *min / *max stay as they were without */
void dcn32_drm_get_monitor_range(const uint8_t *edid, uint32_t len, uint32_t *min_vfreq, uint32_t *max_vfreq);
/* parse_edid_displayid_vrr: the dynamic video timing range block of a
 * DisplayID extension (eDP panels) */
void dcn32_parse_edid_displayid_vrr(const uint8_t *edid, uint32_t len, uint32_t *min_vfreq, uint32_t *max_vfreq);
/* parse_hdmi_amd_vsdb: the first CTA extension handed to the DMCUB's EDID
 * CEA parser (DMUB_CMD__EDID_CEA, 8 bytes per command: parse_edid_cea_dmub,
 * dm_edid_parser_send_cea). The extension index, -1 (no CTA block, no
 * DMCUB, no FreeSync AMD VSDB) or RD_AMD_VSDB_PARSER_FAILED (a command
 * the firmware did not run or answered with an unknown result). */
#define RD_AMD_VSDB_PARSER_FAILED (-2)
int  dcn32_parse_hdmi_amd_vsdb(rdna3_dmub *dm, const uint8_t *edid, uint32_t len, rdna3_vsdb_info *v);
/* amdgpu_dm_update_freesync_caps for a sink of 'signal' (RD_SIGNAL_*):
 * 'allow_invalid_msa_timing_param' = the link's DPCD bit (DP_MSA_TIMING_PAR_IGNORED,
 * 0 without a link), 'as_type' = dm_get_adaptive_sync_support_type
 * (RD_ADAPTIVE_SYNC_TYPE_NONE without a link), 'dm' the DMCUB for the CEA
 * parser (may be NULL: no HDMI FreeSync then). 'edid' NULL: no caps. */
void dcn32_update_freesync_caps(rdna3_freesync_caps *c, const uint8_t *edid, uint32_t len, uint32_t signal,
                                int allow_invalid_msa_timing_param, int as_type, rdna3_dmub *dm);

/* ---- the stream ---- */

/* drm_mode_vrefresh (Hz, rounded) */
uint32_t dcn32_drm_mode_vrefresh(const rdna3_mode *m);
/* the stream fields of mode 'm' on a sink of 'signal' */
void dcn32_vrr_stream_from_mode(rdna3_vrr_stream *s, const rdna3_mode *m, uint32_t signal);
/* mod_freesync_calc_nominal_field_rate (uHz) */
uint64_t dcn32_freesync_calc_nominal_field_rate(const rdna3_vrr_stream *s);
/* mod_freesync_calc_v_total_from_refresh */
uint32_t dcn32_freesync_calc_v_total_from_refresh(const rdna3_vrr_stream *s, uint32_t refresh_in_uhz);
/* mod_freesync_build_vrr_params with dc->caps.max_v_total of DCN 3.2;
 * 'vrr' holds the previous parameters (zero the first time) */
void dcn32_freesync_build_vrr_params(const rdna3_vrr_stream *s, const rdna3_freesync_config *in_config,
                                     rdna3_vrr_params *vrr);
/* mod_freesync_build_vrr_infopacket with TRANSFER_FUNC_UNKNOWN: the SPD
 * infopacket of the FreeSync state ('p' zeroed by the caller, as amdgpu_dm
 * does; untouched when !vrr->send_info_frame) */
void dcn32_freesync_build_vrr_infopacket(const rdna3_vrr_stream *s, const rdna3_vrr_params *vrr, uint32_t packet_type,
                                         rdna3_info_packet *p, int pack_sdp_v1_3);
/* mod_build_adaptive_sync_infopacket: v1 (header only) for a whitelisted
 * PCON (and eDP), nothing for the others */
void dcn32_build_adaptive_sync_infopacket(int as_type, rdna3_info_packet *p);
/* adaptive_sync_override_dp_info_packets_sdp_line_num: the line the
 * adaptive sync SDP goes out on, from the pipe's VSTARTUP_START */
void dcn32_adaptive_sync_sdp_line_num(const rdna3_vrr_stream *s, uint32_t vstartup_start, int *valid, uint32_t *line);

/* get_freesync_config_for_crtc + update_stream_irq_parameters +
 * update_freesync_state_on_stream (no surface: no BTR) for 'vrr_enabled'
 * (the CRTC's vrr_enabled property, here the boot argument): the stream's
 * ignore_msa_timing_param, vrr / adaptive sync infopackets and
 * allow_freesync, the CRTC's config and vrr params. 'crtc' keeps the
 * previous parameters (zero the first time). */
void dcn32_freesync_update_stream(rdna3_freesync_crtc *crtc, rdna3_vrr_stream *s, const rdna3_freesync_caps *caps,
                                  const rdna3_mode *m, int vrr_enabled);
/* amdgpu_dm_crtc_vrr_active */
int  dcn32_freesync_vrr_active(const rdna3_freesync_crtc *crtc);
/* is_dc_timing_adjust_needed */
int  dcn32_freesync_timing_adjust_needed(const rdna3_freesync_crtc *old_crtc, const rdna3_freesync_crtc *new_crtc);

/* ---- DRR ---- */

/* dcn10_set_drr for one OTG: optc32_set_drr, the static screen trigger
 * (OTG_TRIG_A, 2 frames) when vmin and vmax are set */
void dcn32_set_drr(rdna3_dev *d, uint32_t otg, const rdna3_crtc_timing_adjust *adjust);
/* dc_stream_adjust_vmin_vmax for the stream on 'otg': 1 when programmed,
 * 0 when a v_total above DCN 3.2's maximum would need set_long_vtotal
 * (not ported) */
int  dcn32_stream_adjust_vmin_vmax(rdna3_dev *d, uint32_t otg, const rdna3_crtc_timing_adjust *adjust);

#ifdef __cplusplus
}
#endif
#endif
