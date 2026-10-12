/*
 * dcn32_dp.h - DisplayPort SST over the DIG of DCN 3.2: sink capabilities,
 * link settings, 8b/10b link training with fallback, and the DP stream
 * side, ported from amdgpu Display Core (Linux v6.12):
 *
 *   link_dp_capability.c   dp_retrieve_lttpr_cap, retrieve_link_cap (subset),
 *                          dp_get_max_link_cap, link_decide_link_settings /
 *                          decide_dp_link_settings, decide_fallback_link_setting,
 *                          dp_verify_link_cap
 *   link_validation.c      dp_link_bandwidth_kbps
 *   dc_dsc.c               dc_bandwidth_in_kbps_from_timing
 *   link_dp_training.c     perform_link_training_with_retries,
 *                          dp_perform_link_training, the DPCD / lane helpers
 *   link_dp_training_8b_10b.c  dp_perform_8b_10b_link_training and its
 *                          clock recovery / channel equalization sequences
 *   link_dp_phy.c          dp_enable_link_phy, dp_disable_link_phy,
 *                          dp_set_hw_lane_settings, dpcd_write_rx_power_ctrl
 *   link_hwss_dio.c        the DIO link hwss (enable / disable link output,
 *                          test pattern, lane settings, stream encoder setup)
 *   dcn10_link_encoder.c   training pattern / passthrough registers, the
 *                          VBIOS TransmitterControl ENABLE / DISABLE /
 *                          SETUP_VSEMPH (sent to the DMCUB, as for TMDS)
 *   dcn10/dcn20/dcn32 stream encoders  MSA, blank, unblank (M/N, FIFOs)
 *   dce_clock_source.c     dcn31_program_pix_clk for DP: the DP DTO
 *
 * Scope: SST, 8b/10b (RBR, HBR, HBR2, HBR3), RGB 8 bpc. FEC, DSC and MST
 * are off unless the link's policies turn them on (fec_policy / dsc_policy
 * with rdna3dsc=1, mst with rdna3mst=1):
 *
 *   FEC  link_dp_phy.c dp_set_fec_ready / dp_set_fec_enable, link_dp_capability.c
 *        dp_is_fec_supported / dp_should_enable_fec, dcn20_link_encoder.c
 *        enc2_fec_set_ready / enc2_fec_set_enable / enc2_fec_is_active, and
 *        dp_link_bandwidth_kbps's FEC efficiency (link_validation.c)
 *   DSC  retrieve_link_cap's DSC / branch decoder caps, get_active_converter_info
 *        (subset), and link_dpms.c link_set_dsc_on_stream, link_set_dsc_pps_packet,
 *        link_set_dsc_enable, dp_set_dsc_on_rx (dm_helpers_dp_write_dsc_enable,
 *        SST), in link_set_dpms_on / _off; the DSC block itself is hw/dcn32_dsc.c
 *   MST  link_set_dpms_on / _off's MST branches (enable_link_dp_mst,
 *        allocate_mst_payload, deallocate_mst_payload, disable_link_dp): hw/dcn32_dp_mst.c,
 *        the topology manager hw/dp_mst.c
 *
 * UHBR / 128b/132b (the link's uhbr_policy, rdna3uhbr=1): an SST stream
 * whose link settings land on UHBR10 / 13.5 / 20 runs through the HPO
 * encoders (hw/dcn32_hpo.c) instead of the DIG:
 *
 *   link_dp_capability.c   the 128b/132b caps (DP_128B132B_SUPPORTED_LINK_RATES,
 *                          retrieve_cable_id, dpcd_write_cable_id_to_dprx,
 *                          get_cable_max_link_rate, the LTTPR's 128b/132b
 *                          rates), the UHBR rates in dp_get_max_link_cap,
 *                          increase / reduce_link_rate,
 *                          decide_fallback_link_setting_max_bw_policy,
 *                          link_dp_get_encoding_format, dp_is_128b_132b_signal
 *   link_validation.c      dp_link_bandwidth_kbps (128b/132b: 96.71 %)
 *   dc_dsc.c               apply_128b_132b_stream_overhead (hw/dcn32_dsc.c)
 *   link_dp_training_128b_132b.c  dp_perform_128b_132b_link_training, the
 *                          channel EQ / CDS done sequences, the training
 *                          settings, dp_decide_128b_132b_lttpr_mode
 *   link_dp_training.c     the 128b/132b branches (TX FFE presets, the DPCD
 *                          patterns, the intra-hop wait on exit,
 *                          configure_lttpr_mode_non_transparent)
 *   link_hwss_hpo_dp.c     the HPO link hwss (link output, test pattern, FFE,
 *                          stream encoder setup / attribute, the VC payload,
 *                          throttled VCP size, hblank min symbol width)
 *   link_dpms.c            update_sst_payload,
 *                          write_128b_132b_sst_payload_allocation_table,
 *                          link_calculate_sst_avg_time_slots_per_mtp,
 *                          poll_for_allocation_change_trigger, get_lane_status;
 *                          the 128b/132b order of link_set_dpms_on / _off
 *   dcn20/dce110 hwseq     dcn20_enable_stream_timing's DTBCLK DTO,
 *                          dcn20_enable_stream / dce110_disable_stream's
 *                          DPSTREAMCLK and SYMCLK32_SE, the HPO blank /
 *                          unblank, optc3_set_out_mux
 *
 * Without uhbr_policy (DC's debug.disable_uhbr) dp_get_max_link_cap stays at
 * HBR3 whatever the sink and the VBIOS encoder caps say, as before. The VBIOS
 * encoder caps record decides which UHBR rates the board allows (no record:
 * link_enc_feature, no UHBR). MST over 128b/132b is not ported.
 *
 * ODM combine (l->odm_opp_cnt 2, hw/dcn32_odm.c with rdna3odm=1): the
 * stream's DSC on every segment's engine (dcn32_update_dsc_on_stream's
 * split), the pixel rate divider (K2 / 2) and the DP DTO at the requested
 * pixel clock / segments, dio_se_pix_per_cycle 2 (dcn32_unblank_stream).
 * An LTTPR is trained transparent on 8b/10b (dp_decide_8b_10b_lttpr_mode
 * with an LTTPR-aware VBIOS); on 128b/132b DC puts it in non-transparent
 * mode and trains the DPRX end to end (the LTTPRs train themselves).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCN32_DP_H
#define RDNA3_DCN32_DP_H

#include "dcn32.h"
#include "dcaux.h"
#include "dmub32.h"
#include "edid.h"
#include "dcn32_dsc.h"
#include "dcn32_hdmi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* DPCD addresses (drm_dp.h, dpcd_defs.h) */
#define RD_DPCD_REV                     0x00000
#define RD_DPCD_MAX_LINK_RATE           0x00001
#define RD_DPCD_MAX_LANE_COUNT          0x00002
#define RD_DPCD_MAX_DOWNSPREAD          0x00003
#define RD_DPCD_DOWNSTREAMPORT_PRESENT  0x00005
#define RD_DPCD_MAIN_LINK_CHANNEL_CODING 0x00006
#define RD_DPCD_DOWN_STREAM_PORT_COUNT  0x00007
#define RD_DPCD_TRAINING_AUX_RD_INTERVAL 0x0000e
#define RD_DPCD_FEC_CAPABILITY          0x00090
#define RD_DPCD_DOWNSTREAM_PORT_0       0x00080
#define RD_DPCD_MSTM_CAP                0x00021
#define RD_DPCD_MSTM_CTRL               0x00111
#define RD_DPCD_LINK_BW_SET             0x00100
#define RD_DPCD_LANE_COUNT_SET          0x00101
#define RD_DPCD_TRAINING_PATTERN_SET    0x00102
#define RD_DPCD_TRAINING_LANE0_SET      0x00103
#define RD_DPCD_DOWNSPREAD_CTRL         0x00107
#define RD_DPCD_MAIN_LINK_CHANNEL_CODING_SET 0x00108
#define RD_DPCD_SINK_COUNT              0x00200
#define RD_DPCD_LANE0_1_STATUS          0x00202
#define RD_DPCD_LANE_ALIGN_STATUS_UPDATED 0x00204
#define RD_DPCD_ADJUST_REQUEST_LANE0_1  0x00206
#define RD_DPCD_SINK_OUI                0x00400
#define RD_DPCD_SINK_HW_REVISION_START  0x00409
#define RD_DPCD_SOURCE_OUI              0x00300
#define RD_DPCD_SOURCE_MINIMUM_HBLANK_SUPPORTED 0x00340
#define RD_DPCD_SOURCE_SINK_CAP         0x00317
#define RD_DPCD_EDP_GENERAL_CAP_2       0x00703
#define RD_DPCD_BRANCH_OUI              0x00500
#define RD_DPCD_BRANCH_REVISION_START   0x00509
#define RD_DPCD_DFP_CAPABILITY_EXTENSION_SUPPORT 0x000a3
#define RD_DPCD_PCON_HDMI_POST_FRL_STATUS 0x03036
#define RD_DPCD_DPRX_FEATURE_ENUMERATION_LIST_CONT_1 0x02214
#define RD_DPCD_SET_POWER               0x00600
#define RD_DPCD_DP13_DPCD_REV           0x02200
#define RD_DPCD_DPRX_FEATURE_ENUMERATION_LIST 0x02210
#define RD_DPCD_LTTPR_REV               0xf0000   /* DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV */
#define RD_DPCD_PHY_REPEATER_CNT        0xf0002
#define RD_DPCD_PHY_REPEATER_MODE       0xf0003
#define RD_DPCD_PHY_REPEATER_EXTENDED_WAIT_TIMEOUT 0xf0005
#define RD_DPCD_TOTAL_LTTPR_CNT         0xf000a
#define RD_DPCD_TRAINING_PATTERN_SET_PHY_REPEATER1 0xf0010
#define RD_DPCD_TRAINING_LANE0_SET_PHY_REPEATER1 0xf0011
#define RD_DPCD_TRAINING_AUX_RD_INTERVAL_PHY_REPEATER1 0xf0020
#define RD_DPCD_LANE0_1_STATUS_PHY_REPEATER1 0xf0030
#define RD_DPCD_ADJUST_REQUEST_LANE0_1_PHY_REPEATER1 0xf0033
#define RD_DPCD_REPEATER_CONFIGURATION_AND_STATUS_SIZE 0x50
/* DP 2.0 (drm_dp.h, dc_dp_types.h) */
#define RD_DPCD_SINK_VIDEO_FALLBACK_FORMATS 0x00020
#define RD_DPCD_FEC_CAPABILITY_1        0x00091
#define RD_DPCD_CABLE_ATTRIBUTES_UPDATED_BY_DPTX 0x00110
#define RD_DPCD_PAYLOAD_ALLOCATE_SET        0x1c0
#define RD_DPCD_PAYLOAD_ALLOCATE_START_TIME_SLOT 0x001c1
#define RD_DPCD_PAYLOAD_ALLOCATE_TIME_SLOT_COUNT 0x001c2
#define RD_DPCD_SINK_STATUS             0x00205
#define RD_DPCD_PAYLOAD_TABLE_UPDATE_STATUS 0x2c0
#define RD_DPCD_128B132B_SUPPORTED_LINK_RATES 0x02215
#define RD_DPCD_128B132B_TRAINING_AUX_RD_INTERVAL 0x02216
#define RD_DPCD_CABLE_ATTRIBUTES_UPDATED_BY_DPRX 0x02217
#define RD_DPCD_MAX_UNCOMPRESSED_PIXEL_RATE_CAP 0x0221c
/* DP_MAIN_LINK_CHANNEL_CODING */
#define RD_DP_CAP_ANSI_8B10B            (1u << 0)
#define RD_DP_CAP_ANSI_128B132B         (1u << 1)
/* DP_128B132B_SUPPORTED_LINK_RATES, the LTTPR's 128b/132b rates */
#define RD_DP_UHBR10                    (1u << 0)
#define RD_DP_UHBR20                    (1u << 1)
#define RD_DP_UHBR13_5                  (1u << 2)
/* union dp_cable_id: UHBR10_20_CAPABILITY 2 bits, UHBR13_5 1, CABLE_TYPE 3 */
#define RD_DP_CABLE_UHBR10_20(c)        ((c) & 3u)
#define RD_DP_CABLE_UHBR13_5(c)         (((c) >> 2) & 1u)
#define RD_DP_CABLE_TYPE(c)             (((c) >> 3) & 7u)
/* DP_SINK_STATUS */
#define RD_DP_INTRA_HOP_AUX_REPLY_INDICATION (1u << 3)
/* DP_LANE_ALIGN_STATUS_UPDATED, 128b/132b */
#define RD_DP_128B132B_DPRX_EQ_INTERLANE_ALIGN_DONE  (1u << 2)
#define RD_DP_128B132B_DPRX_CDS_INTERLANE_ALIGN_DONE (1u << 3)
#define RD_DP_128B132B_LT_FAILED        (1u << 4)
/* union payload_table_update_status */
#define RD_DP_VC_PAYLOAD_TABLE_UPDATED  (1u << 0)
#define RD_DP_ACT_HANDLED               (1u << 1)

#define RD_DP_SET_POWER_D0              0x1
#define RD_DP_SET_POWER_D3              0x2
#define RD_DP_PHY_REPEATER_MODE_TRANSPARENT     0x55
#define RD_DP_PHY_REPEATER_MODE_NON_TRANSPARENT 0xaa
/* DP_MAX_LANE_COUNT / DP_LANE_COUNT_SET bits */
#define RD_DP_LANE_COUNT_MASK           0x1f
#define RD_DP_POST_LT_ADJ_REQ           (1u << 5)
#define RD_DP_TPS3_SUPPORTED            (1u << 6)
#define RD_DP_ENHANCED_FRAME            (1u << 7)
/* DP_MAX_DOWNSPREAD */
#define RD_DP_MAX_DOWNSPREAD_0_5        (1u << 0)
#define RD_DP_TPS4_SUPPORTED            (1u << 7)
/* DP_TRAINING_PATTERN_SET: enum dpcd_training_patterns + SCRAMBLING_DISABLE */
#define RD_DPCD_TRAINING_PATTERN_VIDEOIDLE 0
#define RD_DPCD_TRAINING_PATTERN_1      1
#define RD_DPCD_TRAINING_PATTERN_2      2
#define RD_DPCD_TRAINING_PATTERN_3      3
#define RD_DPCD_TRAINING_PATTERN_4      7
#define RD_DP_SCRAMBLING_DISABLE        (1u << 5)
/* union dpcd_training_lane: VS 0-1, MAX_SWING_REACHED 2, PE 3-4, MAX_PE_REACHED 5 */
#define RD_DP_LANE_VS(x)                ((x) & 3u)
#define RD_DP_LANE_PE(x)                (((x) >> 3) & 3u)
/* union lane_status (one nibble per lane) */
#define RD_DP_LANE_CR_DONE              (1u << 0)
#define RD_DP_LANE_CHANNEL_EQ_DONE      (1u << 1)
#define RD_DP_LANE_SYMBOL_LOCKED        (1u << 2)
#define RD_DP_INTERLANE_ALIGN_DONE      (1u << 0)
#define RD_DP_POST_LT_ADJ_REQ_IN_PROGRESS (1u << 1)

/* enum dc_link_rate, 8b/10b part (DPCD LINK_BW codes, x 0.27 Gbps / lane) */
#define RD_DP_LINK_RATE_UNKNOWN 0x00
#define RD_DP_LINK_RATE_LOW     0x06   /* RBR  1.62 Gbps */
#define RD_DP_LINK_RATE_RATE_2  0x08
#define RD_DP_LINK_RATE_RATE_3  0x09
#define RD_DP_LINK_RATE_HIGH    0x0a   /* HBR  2.70 Gbps */
#define RD_DP_LINK_RATE_RBR2    0x0c
#define RD_DP_LINK_RATE_RATE_6  0x10
#define RD_DP_LINK_RATE_HIGH2   0x14   /* HBR2 5.40 Gbps */
#define RD_DP_LINK_RATE_RATE_8  0x19
#define RD_DP_LINK_RATE_HIGH3   0x1e   /* HBR3 8.10 Gbps */
/* the 128b/132b part, in 10 Mbps per lane */
#define RD_DP_LINK_RATE_UHBR10  1000   /* UHBR10   10.0 Gbps */
#define RD_DP_LINK_RATE_UHBR13_5 1350  /* UHBR13.5 13.5 Gbps */
#define RD_DP_LINK_RATE_UHBR20  2000   /* UHBR20   20.0 Gbps */
/* enum dp_link_encoding (what DP_MAIN_LINK_CHANNEL_CODING_SET takes) */
enum { RD_DP_UNKNOWN_ENCODING = 0, RD_DP_8b_10b_ENCODING = 1, RD_DP_128b_132b_ENCODING = 2 };
/* enum dc_link_spread */
#define RD_DP_LINK_SPREAD_DISABLED       0x00
#define RD_DP_LINK_SPREAD_05_DOWNSPREAD_30KHZ 0x10
/* enum dc_voltage_swing / dc_pre_emphasis */
#define RD_DP_VOLTAGE_SWING_MAX_LEVEL   3
#define RD_DP_PRE_EMPHASIS_MAX_LEVEL    3
#define RD_DP_LANE_COUNT_DP_MAX         4

/* link.h / link_dpms.c / link_detection.c */
#define RD_DP_LINK_TRAINING_ATTEMPTS        4
#define RD_DP_LINK_TRAINING_MAX_RETRY_COUNT 5
#define RD_DP_LINK_TRAINING_MAX_CR_RETRY    100
#define RD_DP_LINK_TRAINING_MAX_VERIFY_RETRY 2
#define RD_DP_LINK_TRAINING_RETRY_DELAY     50   /* ms */
#define RD_DP_LINK_RATE_REF_FREQ_IN_KHZ     27000

/* enum dc_dp_training_pattern */
enum {
    RD_DP_TRAINING_PATTERN_SEQUENCE_1 = 0,
    RD_DP_TRAINING_PATTERN_SEQUENCE_2,
    RD_DP_TRAINING_PATTERN_SEQUENCE_3,
    RD_DP_TRAINING_PATTERN_SEQUENCE_4,
    RD_DP_TRAINING_PATTERN_VIDEOIDLE,
    RD_DP_128b_132b_TPS1,
    RD_DP_128b_132b_TPS2,
    RD_DP_128b_132b_TPS2_CDS,
};
/* enum dpcd_training_patterns, 128b/132b */
#define RD_DPCD_128b_132b_TPS1          1
#define RD_DPCD_128b_132b_TPS2          2
#define RD_DPCD_128b_132b_TPS2_CDS      3

/* enum link_training_result */
enum {
    RD_LT_SUCCESS = 0,
    RD_LT_CR_FAIL_LANE0,
    RD_LT_CR_FAIL_LANE1,
    RD_LT_CR_FAIL_LANE23,
    RD_LT_EQ_FAIL_CR,
    RD_LT_EQ_FAIL_CR_PARTIAL,
    RD_LT_EQ_FAIL_EQ,
    RD_LT_LQA_FAIL,
    RD_LT_LINK_LOSS,
    RD_LT_ABORT,
    RD_LT_128b_132b_LT_FAILED,
    RD_LT_128b_132b_MAX_LOOP_COUNT_REACHED,
    RD_LT_128b_132b_CHANNEL_EQ_DONE_TIMEOUT,
    RD_LT_128b_132b_CDS_DONE_TIMEOUT,
};

/* enum lttpr_mode */
enum { RD_LTTPR_MODE_NON_LTTPR = 0, RD_LTTPR_MODE_TRANSPARENT, RD_LTTPR_MODE_NON_TRANSPARENT };

/* enum dc_link_fec_state */
enum { RD_FEC_NOT_READY = 0, RD_FEC_READY, RD_FEC_ENABLED };

/* enum display_dongle_type (the ones get_active_converter_info sets) */
enum { RD_DONGLE_NONE = 0, RD_DONGLE_DP_VGA, RD_DONGLE_DP_DVI, RD_DONGLE_DP_HDMI };

/* struct dc_dongle_caps: a DP-to-HDMI converter's detailed caps (DPCD
 * 0x080-0x083, DP_PCON_HDMI_POST_FRL_STATUS) and the DFP capability
 * extension (DPCD 0x0a3-0x0ae, DP 2.0) */
typedef struct rdna3_dp_dongle_caps {
    int      dongle_type, extended_cap_valid;
    int      is_dp_hdmi_s3d_converter, is_dp_hdmi_ycbcr422_pass_through, is_dp_hdmi_ycbcr420_pass_through;
    int      is_dp_hdmi_ycbcr422_converter, is_dp_hdmi_ycbcr420_converter;
    uint32_t dp_hdmi_max_bpc, dp_hdmi_max_pixel_clk_in_khz, dp_hdmi_frl_max_link_bw_in_kbps;
    struct {
        int      supported;
        uint32_t max_pixel_rate_in_mps, max_video_h_active_width, max_video_v_active_height;
        uint8_t  encoding_format_caps;   /* bit 0 RGB, 1 YCbCr444, 2 422, 3 420 */
        uint8_t  rgb_color_depth_caps, ycbcr444_color_depth_caps, ycbcr422_color_depth_caps,
                 ycbcr420_color_depth_caps;   /* bit 0 6 bpc, 1 8, 2 10, 3 12, 4 16 */
    } dfp_cap_ext;
} rdna3_dp_dongle_caps;

/* LINK_AUX_DEFAULT_LTTPR_TIMEOUT_PERIOD / LINK_AUX_DEFAULT_TIMEOUT_PERIOD (us) */
#define RD_LINK_AUX_DEFAULT_LTTPR_TIMEOUT_PERIOD 3200
#define RD_LINK_AUX_DEFAULT_TIMEOUT_PERIOD       552

/* struct link_mst_stream_allocation_table (DIO: the stream encoder is the
 * DIG front end; at most 4 streams, DP_MSE_SAT0..1) */
#define RD_MST_MAX_STREAMS 4
typedef struct rdna3_mst_alloc_table {
    uint32_t stream_count;
    struct { int fe; uint32_t vcp_id, slot_count; } a[RD_MST_MAX_STREAMS];
} rdna3_mst_alloc_table;

struct rdna3_mst_mgr;
struct rdna3_audio_stream;

/* struct dc_link_settings (no link rate set: eDP only) */
typedef struct rdna3_dp_link_settings {
    uint32_t lane_count;      /* 1, 2, 4; 0 = unknown */
    uint32_t link_rate;       /* RD_DP_LINK_RATE_* */
    uint32_t link_spread;     /* RD_DP_LINK_SPREAD_* */
} rdna3_dp_link_settings;

/* struct dc_lane_settings (ffe_preset: union dc_dp_ffe_preset, 128b/132b) */
typedef struct rdna3_dp_lane_settings {
    uint8_t voltage_swing, pre_emphasis, post_cursor2;
    uint8_t ffe_preset;
} rdna3_dp_lane_settings;

/* struct dc_lttpr_caps */
typedef struct rdna3_dp_lttpr_caps {
    uint8_t revision, max_link_rate, phy_repeater_cnt, max_lane_count;
    uint8_t mode, max_ext_timeout, main_link_channel_coding, supported_128b_132b_rates;
    uint8_t aux_rd_interval[7];     /* per repeater (non-transparent 8b/10b), & 0x7f */
} rdna3_dp_lttpr_caps;

/* struct dpcd_caps, the part this port uses */
typedef struct rdna3_dp_caps {
    uint8_t  rx[16];                /* receiver capability field (0x000.., or 0x2200.. when extended) */
    uint8_t  dpcd_rev;
    uint8_t  max_ln_count;          /* raw DP_MAX_LANE_COUNT */
    uint8_t  max_down_spread;       /* raw DP_MAX_DOWNSPREAD */
    uint8_t  channel_coding_cap;
    uint8_t  sink_count;
    uint8_t  dprx_feature;
    uint8_t  fec_cap;
    uint8_t  ext_receiver_cap_field_present;
    uint8_t  allow_invalid_msa_timing_param;
    uint32_t sink_dev_id;           /* IEEE OUI */
    rdna3_dp_lttpr_caps lttpr;
    /* DPCD 1.4+: DSC basic caps (0x060..0x06f), DSC branch decoder caps
     * (0x0a0..0x0a2, converters only), the branch / dongle type */
    uint8_t  dsc_basic[RD_DSC_DPCD_BASIC], dsc_branch[RD_DSC_DPCD_BRANCH];
    int      is_branch_dev, dongle_type;
    uint8_t  mstm_cap;              /* DP_MSTM_CAP (0x021), DPCD 1.2+ */
    rdna3_dp_dongle_caps dongle_caps;
    /* read_dp_device_vendor_id, the sink id string and revisions */
    uint32_t branch_dev_id;
    uint8_t  branch_dev_name[6], branch_hw_revision, branch_fw_revision[2];
    uint8_t  sink_dev_id_str[6], sink_hw_revision, sink_fw_revision[2];
    uint8_t  adaptive_sync_caps;    /* DP_DPRX_FEATURE_ENUMERATION_LIST_CONT_1 */
    int      panel_luminance_control;   /* eDP general cap 2 (dpcd_read_sink_ext_caps) */
    /* DP 2.0: the 128b/132b link rates (RD_DP_UHBR*), the sink's video
     * fallback formats, FEC capability 1, the max uncompressed pixel rate,
     * the cable attributes (union dp_cable_id) */
    uint8_t  dp_128b_132b_supported_link_rates, fallback_formats, fec_cap1;
    uint8_t  max_uncompressed_pixel_rate_cap[2];
    uint8_t  cable_id;
} rdna3_dp_caps;

/* struct encoder_info_frame, the DP part (dcn30_update_info_frame) */
typedef struct rdna3_dp_info_frames {
    rdna3_info_packet vsc, spd, hdrsmd, adaptive_sync;
    int      adaptive_sync_line_num_valid;   /* sdp_line_num */
    uint32_t adaptive_sync_line_num;
} rdna3_dp_info_frames;

/* struct link_training_settings (8b/10b) */
typedef struct rdna3_dp_lt_settings {
    rdna3_dp_link_settings link_settings;
    rdna3_dp_lane_settings hw_lane_settings[RD_DP_LANE_COUNT_DP_MAX];
    uint8_t  dpcd_lane_settings[RD_DP_LANE_COUNT_DP_MAX];   /* union dpcd_training_lane */
    uint32_t cr_pattern_time, eq_pattern_time;              /* us */
    uint32_t pattern_for_cr, pattern_for_eq;
    /* 128b/132b: the EQ wait / loop limits, the CDS pattern and its waits */
    uint32_t eq_wait_time_limit, eq_loop_count_limit;
    uint32_t pattern_for_cds, cds_pattern_time, cds_wait_time_limit;
    int      enhanced_framing, should_set_fec_ready;
    int      disallow_per_lane_settings, always_match_dpcd_with_hw_lane_settings;
    int      lttpr_mode;
} rdna3_dp_lt_settings;

/* struct dc_link + the pipe resources one DP SST stream uses */
typedef struct rdna3_dp_link {
    rdna3_dev  *d;
    rdna3_dmub *dm;                 /* the DMCUB that runs the VBIOS transmitter commands */
    uint32_t aux;                   /* AUX engine (= DDC line of the connector) */
    uint32_t be;                    /* link encoder: DIG back end = transmitter (UNIPHY n) */
    uint32_t fe;                    /* stream encoder: DIG front end */
    uint32_t otg;                   /* timing generator feeding the stream encoder */
    int      hpd;                   /* HPD n, 0-based; -1 none */
    uint8_t  connector_id, encoder_id;
    uint32_t enc_caps;              /* VBIOS encoder caps (RD_ATOM_ENC_CAP_*): link encoder features */
    uint32_t dprefclk_khz;          /* DP DTO reference (dcn32_clk_read) */
    int      lttpr_aware;           /* dc->caps.vbios_lttpr_aware && extended_aux_timeout_support */
    int      vbios_lttpr_enable;    /* dc->caps.vbios_lttpr_enable: dce_info display_caps
                                     * DCE_INFO_CAPS_LTTPR_SUPPORT_ENABLE (vbios_dce_info_caps);
                                     * 8b/10b trains LTTPRs non-transparent, hop by hop */
    uint16_t chip_id;               /* PCI device id: dpcd_set_source_specific_data (asic_id.chip_id) */
    int      keep_receiver_powered; /* wa_flags.dp_keep_receiver_powered (dp_wa_power_up_0010FA) */
    uint8_t  dpcd_sink_ext_caps;    /* DP_SOURCE_SINK_CAP (dpcd_read_sink_ext_caps) */
    int      ignore_msa_timing_param;   /* stream->ignore_msa_timing_param: FreeSync only, 0 */
    uint32_t pix_per_cycle;         /* dio_se_pix_per_cycle: 1, 2; 0 = take the boot state's */
    int      caps_valid, verified;
    rdna3_dp_caps dpcd_caps;
    rdna3_dp_link_settings reported_link_cap, max_link_cap, verified_link_cap, cur_link_settings;
    rdna3_dp_lane_settings cur_lane_setting[RD_DP_LANE_COUNT_DP_MAX];
    /* what the last training did (log, tests, the kext's report) */
    int      last_result;
    uint32_t lt_runs, lt_fallbacks;
    /* FEC / DSC (rdna3dsc=1): fec_policy = DC's dp_should_enable_fec (FEC on
     * a capable sink, trained FEC-ready, enabled after training), dsc_policy
     * = apply_dsc_policy_for_stream (DSC when the mode needs more than the
     * link carries); the stream's timing.flags.DSC / dsc_cfg; the DSC
     * engine (n = pipe n, dcn20_acquire_dsc) and the OPP it reads */
    int      fec_policy, dsc_policy;
    int      fec_state;             /* RD_FEC_* */
    int      stream_dsc;
    rdna3_dsc_config dsc_cfg;
    uint32_t dsc_inst, opp;
    uint8_t  pps[RD_DSC_PPS_BYTES]; /* stream->dsc_packed_pps */
    /* MST (rdna3mst=1): the stream is SIGNAL_TYPE_DISPLAY_PORT_MST to output
     * port 'mst_port' of 'mgr'; link_active (link_status.link_active) and
     * the stream allocation table of the link (DC's mst_stream_alloc_table) */
    int      mst;
    struct rdna3_mst_mgr *mgr;
    int      mst_port;
    int      link_active;
    rdna3_mst_alloc_table mst_alloc;
    uint32_t mst_pbn, mst_slots, mst_vcp_x, mst_vcp_y;   /* the last payload (log, tests) */
    int      mst_act;               /* the last ACT handled */
    /* UHBR (rdna3uhbr=1): uhbr_policy = !debug.disable_uhbr. The HPO
     * resources of the link: stream encoder hpo_se (0..3, VPG 6 + n) and
     * link encoder hpo_le (0..1); hpo_active = dp_is_128b_132b_signal for
     * the running stream; the DTBCLK reference (get_dtb_ref_clk_frequency,
     * kHz); cable_id_written (dprx_states); what the SST payload got */
    int      uhbr_policy;
    uint32_t hpo_se, hpo_le;
    int      hpo_active;
    uint32_t dtbclk_khz;
    int      cable_id_written;
    uint32_t sst_slots, sst_vcp_x, sst_vcp_y, hblank_min_symbol_width;
    int      sst_act;
    /* ODM combine of the stream (hw/dcn32_odm.c, rdna3odm=1: odm_policy =
     * the "combine as needed" ODM policy, 0 = dm_odm_combine_policy_none):
     * odm_opp_cnt segments (0 / 1: none), the OPP and the DSC engine of each
     * segment (the first are l->opp and l->dsc_inst) */
    int      odm_policy;
    uint32_t odm_opp_cnt;
    uint32_t odm_opp[4], odm_dsc[4];
    /* the boot state's pixel mode while a 128b/132b or ODM stream changes
     * it (dio_se_pix_per_cycle, the K1 / K2 pixel rate divider): put back
     * for an 8b/10b single-pipe stream, whose divider this driver keeps */
    int      boot_saved;
    uint32_t boot_ppc, boot_k1, boot_k2;
    /* the stream's timing while enable_link_dp trains (the fallback's
     * bandwidth check in the setting's encoding:
     * perform_link_training_with_retries); NULL otherwise */
    const rdna3_mode *lt_mode;
    /* rdna3audio: the stream's audio (hw/dcn32_audio.h): set up with the
     * stream (DIO or HPO as the link setting decides), on after the
     * unblank (dce110_enable_audio_stream), off before the blank of
     * link_set_dpms_off and the endpoint with the back end; NULL none */
    struct rdna3_audio_stream *audio;
} rdna3_dp_link;

/* Fill the static part of 'l' (no hardware access). */
void dcn32_dp_link_init(rdna3_dp_link *l, rdna3_dev *d, rdna3_dmub *dm, uint32_t aux, uint32_t be, uint32_t fe,
                        uint32_t otg, int hpd, uint8_t connector_id, uint8_t encoder_id, uint32_t enc_caps,
                        uint32_t dprefclk_khz);

/* dc_bandwidth_in_kbps_from_timing: RGB 4:4:4 at 'bpc' bits per component */
uint32_t dcn32_dp_stream_kbps(uint32_t pixel_khz, uint32_t bpc);
/* link_dp_get_encoding_format: RD_DP_*_ENCODING of a link setting */
uint32_t dcn32_dp_encoding(const rdna3_dp_link_settings *s);
/* dc_link_get_highest_encoding_format: RD_DC_LINK_ENCODING_* of the link
 * (the verified link cap's encoding; HDMI TMDS behind a DP-HDMI converter) */
uint32_t dcn32_dp_highest_encoding(const rdna3_dp_link *l);
/* mst_decide_link_encoding_format: RD_DP_*_ENCODING of the setting MST
 * streams use (the verified link cap) */
uint32_t dcn32_dp_mst_decide_link_encoding_format(const rdna3_dp_link *l);
/* dp_decide_lttpr_mode: RD_LTTPR_MODE_* the setting's encoding trains in */
int dcn32_dp_decide_lttpr_mode(const rdna3_dp_link *l, const rdna3_dp_link_settings *s);
/* dp_get_max_link_enc_cap: the DIO link encoder's limits (VBIOS encoder
 * caps record); 0 when 'max_link_enc_cap' is NULL */
int dcn32_dp_get_max_link_enc_cap(const rdna3_dp_link *l, rdna3_dp_link_settings *max_link_enc_cap);
/* dp_is_128b_132b_signal for the link's running (or next) stream */
int  dcn32_dp_is_128b_132b_signal(const rdna3_dp_link *l);
/* dp_link_bandwidth_kbps (8b/10b, FEC off) */
uint32_t dcn32_dp_link_kbps_settings(const rdna3_dp_link_settings *s);

/* dp_link_bandwidth_kbps for this link: FEC efficiency when
 * dp_should_enable_fec (the link's FEC policy and a FEC-capable sink) */
uint32_t dcn32_dp_link_bw_kbps(const rdna3_dp_link *l, const rdna3_dp_link_settings *s);
int  dcn32_dp_should_enable_fec(const rdna3_dp_link *l);
/* dc_link_get_link_cap: the verified link cap (the max one until verified) */
const rdna3_dp_link_settings *dcn32_dp_link_cap(const rdna3_dp_link *l);

/* DSC (rdna3dsc=1): the dc_crtc_timing of a mode; update_dsc_caps (1 when
 * the sink decodes DSC); apply_dsc_policy_for_stream (1 with *cfg when the
 * mode needs DSC on this link); dc_bandwidth_in_kbps_from_timing of a mode
 * with (cfg) or without (NULL) DSC */
void dcn32_dp_dsc_timing(const rdna3_mode *m, uint32_t bpc, rdna3_dsc_timing *t);
int  dcn32_dp_sink_dsc_caps(const rdna3_dp_link *l, rdna3_dsc_dec_caps *caps);
int  dcn32_dp_decide_dsc(const rdna3_dp_link *l, const rdna3_mode *m, uint32_t bpc, rdna3_dsc_config *cfg);
/* the same for an ODM stream of 'opp_cnt' segments: the slices divisible
 * by the segments (dc_dsc_config_options.dsc_force_odm_hslice_override) */
int  dcn32_dp_decide_dsc_odm(const rdna3_dp_link *l, const rdna3_mode *m, uint32_t bpc, uint32_t opp_cnt,
                             rdna3_dsc_config *cfg);
uint32_t dcn32_dp_mode_kbps(const rdna3_mode *m, uint32_t bpc, const rdna3_dsc_config *cfg);
/* the same in 'link_encoding' (RD_DC_LINK_ENCODING_*: the 128b/132b overhead) */
uint32_t dcn32_dp_mode_kbps_enc(const rdna3_mode *m, uint32_t bpc, const rdna3_dsc_config *cfg, uint32_t link_encoding);
/* what link_decide_link_settings asks of the link for mode 'm' (8 bpc): in
 * the link's highest encoding (dcn32_dp_highest_encoding) */
uint32_t dcn32_dp_stream_req_kbps(const rdna3_dp_link *l, const rdna3_mode *m, const rdna3_dsc_config *cfg);

/* retrieve_link_cap (subset: LTTPR caps, wake-up, receiver caps, extended
 * caps, DPRX features, sink count / OUI, FEC cap) and dp_get_max_link_cap.
 * DPCD reads, plus the writes retrieve_link_cap does (LTTPR transparent
 * mode and its count echo when an LTTPR answers). 0 or RDNA3_ENODEV (no
 * sink on the AUX channel) / RDNA3_EIO. */
int  dcn32_dp_read_caps(rdna3_dp_link *l);
/* is_dp_active_dongle / is_dp_branch_device */
int  dcn32_dp_is_active_dongle(const rdna3_dp_link *l);
int  dcn32_dp_is_branch_device(const rdna3_dp_link *l);
/* link_validate_mode_timing for an RGB 8 bpc stream of mode 'm' on the
 * link (dp_active_dongle_validate_timing: a DP-to-HDMI converter's TMDS /
 * FRL limits, the DFP capability extension; no passive dongle on a DP
 * connector). 1 = the converter carries it; the link bandwidth stays
 * with dcn32_dp_decide_link_settings (dp_validate_mode_timing). */
int  dcn32_dp_validate_mode_timing(const rdna3_dp_link *l, const rdna3_mode *m);

/* enc3_stream_encoder_update_dp_info_packets: VSC (GSP 0, 1), SPD (2), HDR
 * static metadata (3), adaptive sync (5) through the stream encoder's VPG,
 * their DP_SEC_CNTL enables and the master enable */
void dcn32_enc_update_dp_info_packets(rdna3_dp_link *l, const rdna3_dp_info_frames *f);
/* enc3_stream_encoder_update_dp_info_packets_sdp_line_num (adaptive sync) */
void dcn32_enc_update_dp_info_packets_sdp_line_num(rdna3_dp_link *l, const rdna3_dp_info_frames *f);
/* enc1_stream_encoder_dp_get_pixel_format: the DP stream encoder's pixel
 * encoding (RD_PIXEL_ENCODING_*, 0 undefined) and component depth
 * (RD_COLOR_DEPTH_*). 0 for a NULL argument. */
int  dcn32_enc_dp_get_pixel_format(rdna3_dp_link *l, uint32_t *encoding, uint32_t *depth);



/* link_decide_link_settings (decide_dp_link_settings) for 'req_kbps' over
 * the verified link cap (the max link cap until a training verified one).
 * 0 with *out set, RDNA3_EINVAL when no setting carries it. */
int  dcn32_dp_decide_link_settings(const rdna3_dp_link *l, uint32_t req_kbps, rdna3_dp_link_settings *out);

/* decide_fallback_link_setting: 1 when *cur was lowered, 0 at the end */
int  dcn32_dp_fallback_link_setting(const rdna3_dp_link *l, rdna3_dp_link_settings *max,
                                    rdna3_dp_link_settings *cur, int training_result);

/* dp_verify_link_cap_with_retries: train at the max link cap, falling
 * back until one holds, record it as the verified link cap; the PHY is
 * left off. 1 when a setting trained. */
int  dcn32_dp_verify_link_cap(rdna3_dp_link *l, int attempts);

/* dp_perform_link_training at 'set' (PHY already enabled): RD_LT_* */
int  dcn32_dp_perform_link_training(rdna3_dp_link *l, const rdna3_dp_link_settings *set, int skip_video_pattern);

/* perform_link_training_with_retries: PHY on, train, PHY off on failure,
 * retry (with decide_fallback_link_setting when 'do_fallback', stopping
 * when the link no longer carries 'req_kbps'). 1 when trained; the
 * settings that trained are in l->cur_link_settings. */
int  dcn32_dp_link_training_with_retries(rdna3_dp_link *l, const rdna3_dp_link_settings *set, int skip_video_pattern,
                                         int attempts, int do_fallback, uint32_t req_kbps);

/* dp_enable_link_phy / dp_disable_link_phy */
void dcn32_dp_enable_link_phy(rdna3_dp_link *l, const rdna3_dp_link_settings *set);
void dcn32_dp_disable_link_phy(rdna3_dp_link *l);

/* dcn10_link_encoder_hw_init: TransmitterControl INIT on the link's
 * transmitter (dcn32_init_hw after a power loss; the AUX engine is set up
 * by each transaction, dcaux.c). 0 or the DMCUB error. */
int  dcn32_dp_link_encoder_hw_init(rdna3_dp_link *l);

/* dcn31_program_pix_clk for a DP signal: the DP DTO of OTG 'otg' at
 * 'pixel_khz' (the requested pixel clock: divided by the ODM segments),
 * PIPE0_DTO_SRC_SEL 1 (8b/10b) or 2 (128b/132b: l->hpo_active) */
void dcn32_dp_program_pix_clk(rdna3_dp_link *l, uint32_t pixel_khz);

/* The stream resources DC's validation hands the pipe for mode 'm' (the
 * link settings link_decide_link_settings picks; with a 128b/132b setting
 * the HPO stream / link encoders: l->hpo_active). 0, or RDNA3_EINVAL when
 * no setting carries the mode (l->stream_dsc / dsc_cfg set by the caller). */
int  dcn32_dp_acquire_stream(rdna3_dp_link *l, const rdna3_mode *m);
/* dcn20_enable_stream_timing's clock part for the stream acquired: the
 * pixel rate divider (dcn32_calculate_dccg_k1_k2_values, for a 128b/132b
 * or an ODM stream; an 8b/10b single pipe keeps what the boot set), the DP
 * DTO (dcn31_program_pix_clk at the requested clock: / ODM segments), and
 * for 128b/132b DTBCLK_P from DTBCLK0 and the DTBCLK DTO at the pixel
 * clock / 4 from l->dtbclk_khz */
void dcn32_dp_enable_stream_timing(rdna3_dp_link *l, const rdna3_mode *m);

/* link_set_dpms_off (dce110_blank_stream, deallocate_mst_payload for MST
 * or update_sst_payload for 128b/132b, disable_link_dp, dce110_disable_stream
 * (reset_dio_stream_encoder, or the HPO encoders, SYMCLK32, DPSTREAMCLK and
 * the DTBCLK DTO off), the stream's DSC off on every ODM segment:
 * link_set_dsc_enable; a 128b/132b OTG back to the DIO) */
void dcn32_dp_link_set_dpms_off(rdna3_dp_link *l);

/* link_set_dpms_on with mode 'm' (the OTG already runs it), DSC when
 * l->stream_dsc (l->dsc_cfg; set by the caller), MST to l->mst_port when
 * l->mst, ODM segments l->odm_opp_cnt: link_decide_link_settings (in the
 * link's highest encoding); a 128b/132b setting takes the HPO encoders
 * (optc3_set_out_mux HPO, setup_hpo_dp_stream_attribute, dcn20_enable_stream's
 * DTBCLK DTO / DPSTREAMCLK / SYMCLK32 / setup_hpo_dp_stream_encoder, the
 * PPS through the HPO stream encoder, update_sst_payload, the HPO unblank),
 * an 8b/10b one the DIO (dcn10_link_encoder_setup, setup_dio_stream_attribute,
 * the stream encoder setup, allocate_mst_payload for MST,
 * dcn32_unblank_stream: two pixels per cycle with ODM); between them
 * link_set_dsc_enable, enable_link_dp / enable_link_dp_mst (training with
 * retries; with 'do_fallback' the training falls back too; FEC on 8b/10b),
 * dp_set_dsc_on_rx + link_set_dsc_pps_packet. 0, RDNA3_EINVAL (no link
 * setting carries the mode; MST over a 128b/132b link), RDNA3_EIO (training
 * failed: an SST stream is still enabled as DC does, the caller decides; an
 * MST stream is not; or the MST payload failed). */
int  dcn32_dp_link_set_dpms_on(rdna3_dp_link *l, const rdna3_mode *m, int do_fallback);

/* the link status bits (DPCD 0x202..0x204) still hold: dp_check_link_loss_status */
int  dcn32_dp_link_ok(rdna3_dp_link *l);

/* a link rate's name (log, reports) */
const char *dcn32_dp_rate_name(uint32_t link_rate);

#ifdef __cplusplus
}
#endif
#endif
