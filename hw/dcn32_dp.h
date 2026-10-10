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
 * Scope: SST, 8b/10b (RBR, HBR, HBR2, HBR3), RGB 8 bpc, no DSC, no FEC (the
 * sink's FEC capability is read but FEC stays off: DC's FEC policy is not
 * ported), no MST. UHBR / 128b/132b (the HPO encoder, DTBCLK, DPSTREAMCLK)
 * is declared not supported and left out: dp_get_max_link_cap never goes
 * above HBR3. An LTTPR found in the chain is trained in transparent mode
 * (dp_decide_8b_10b_lttpr_mode with an LTTPR-aware VBIOS that does not force
 * non-transparent); per-repeater (non-transparent) training is left out.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCN32_DP_H
#define RDNA3_DCN32_DP_H

#include "dcn32.h"
#include "dcaux.h"
#include "dmub32.h"
#include "edid.h"

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
#define RD_DPCD_SET_POWER               0x00600
#define RD_DPCD_DP13_DPCD_REV           0x02200
#define RD_DPCD_DPRX_FEATURE_ENUMERATION_LIST 0x02210
#define RD_DPCD_LTTPR_REV               0xf0000   /* DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV */
#define RD_DPCD_PHY_REPEATER_CNT        0xf0002
#define RD_DPCD_PHY_REPEATER_MODE       0xf0003
#define RD_DPCD_PHY_REPEATER_EXTENDED_WAIT_TIMEOUT 0xf0005
#define RD_DPCD_TOTAL_LTTPR_CNT         0xf000a

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

/* enum dc_dp_training_pattern (8b/10b) */
enum {
    RD_DP_TRAINING_PATTERN_SEQUENCE_1 = 0,
    RD_DP_TRAINING_PATTERN_SEQUENCE_2,
    RD_DP_TRAINING_PATTERN_SEQUENCE_3,
    RD_DP_TRAINING_PATTERN_SEQUENCE_4,
    RD_DP_TRAINING_PATTERN_VIDEOIDLE,
};

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
};

/* enum lttpr_mode */
enum { RD_LTTPR_MODE_NON_LTTPR = 0, RD_LTTPR_MODE_TRANSPARENT, RD_LTTPR_MODE_NON_TRANSPARENT };

/* struct dc_link_settings (no link rate set: eDP only) */
typedef struct rdna3_dp_link_settings {
    uint32_t lane_count;      /* 1, 2, 4; 0 = unknown */
    uint32_t link_rate;       /* RD_DP_LINK_RATE_* */
    uint32_t link_spread;     /* RD_DP_LINK_SPREAD_* */
} rdna3_dp_link_settings;

/* struct dc_lane_settings */
typedef struct rdna3_dp_lane_settings {
    uint8_t voltage_swing, pre_emphasis, post_cursor2;
} rdna3_dp_lane_settings;

/* struct dc_lttpr_caps (8b/10b part) */
typedef struct rdna3_dp_lttpr_caps {
    uint8_t revision, max_link_rate, phy_repeater_cnt, max_lane_count;
    uint8_t mode, max_ext_timeout, main_link_channel_coding, supported_128b_132b_rates;
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
} rdna3_dp_caps;

/* struct link_training_settings (8b/10b) */
typedef struct rdna3_dp_lt_settings {
    rdna3_dp_link_settings link_settings;
    rdna3_dp_lane_settings hw_lane_settings[RD_DP_LANE_COUNT_DP_MAX];
    uint8_t  dpcd_lane_settings[RD_DP_LANE_COUNT_DP_MAX];   /* union dpcd_training_lane */
    uint32_t cr_pattern_time, eq_pattern_time;              /* us */
    uint32_t pattern_for_cr, pattern_for_eq;
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
    uint32_t pix_per_cycle;         /* dio_se_pix_per_cycle: 1, 2; 0 = take the boot state's */
    int      caps_valid, verified;
    rdna3_dp_caps dpcd_caps;
    rdna3_dp_link_settings reported_link_cap, max_link_cap, verified_link_cap, cur_link_settings;
    rdna3_dp_lane_settings cur_lane_setting[RD_DP_LANE_COUNT_DP_MAX];
    /* what the last training did (log, tests, the kext's report) */
    int      last_result;
    uint32_t lt_runs, lt_fallbacks;
} rdna3_dp_link;

/* Fill the static part of 'l' (no hardware access). */
void dcn32_dp_link_init(rdna3_dp_link *l, rdna3_dev *d, rdna3_dmub *dm, uint32_t aux, uint32_t be, uint32_t fe,
                        uint32_t otg, int hpd, uint8_t connector_id, uint8_t encoder_id, uint32_t enc_caps,
                        uint32_t dprefclk_khz);

/* dc_bandwidth_in_kbps_from_timing: RGB 4:4:4 at 'bpc' bits per component */
uint32_t dcn32_dp_stream_kbps(uint32_t pixel_khz, uint32_t bpc);
/* dp_link_bandwidth_kbps (8b/10b, FEC off) */
uint32_t dcn32_dp_link_kbps_settings(const rdna3_dp_link_settings *s);

/* retrieve_link_cap (subset: LTTPR caps, wake-up, receiver caps, extended
 * caps, DPRX features, sink count / OUI, FEC cap) and dp_get_max_link_cap.
 * DPCD reads, plus the writes retrieve_link_cap does (LTTPR transparent
 * mode and its count echo when an LTTPR answers). 0 or RDNA3_ENODEV (no
 * sink on the AUX channel) / RDNA3_EIO. */
int  dcn32_dp_read_caps(rdna3_dp_link *l);

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

/* dcn31_program_pix_clk for a DP signal: the DP DTO of OTG 'otg' at 'pixel_khz' */
void dcn32_dp_program_pix_clk(rdna3_dp_link *l, uint32_t pixel_khz);

/* link_set_dpms_off for DP SST (dcn20_blank_stream, disable_link_dp,
 * dcn20_disable_stream's reset_dio_stream_encoder) */
void dcn32_dp_link_set_dpms_off(rdna3_dp_link *l);

/* link_set_dpms_on for DP SST with mode 'm' (the OTG already runs it):
 * dcn10_link_encoder_setup, setup_dio_stream_attribute (MSA),
 * enable_link_dp (link_decide_link_settings + training with retries; with
 * 'do_fallback' the training falls back too), dcn20_enable_stream's
 * stream encoder setup, dcn32_unblank_stream. 0, RDNA3_EINVAL (no link
 * setting carries the mode), RDNA3_EIO (training failed: the stream is
 * still enabled as DC does, the caller decides). */
int  dcn32_dp_link_set_dpms_on(rdna3_dp_link *l, const rdna3_mode *m, int do_fallback);

/* the link status bits (DPCD 0x202..0x204) still hold: dp_check_link_loss_status */
int  dcn32_dp_link_ok(rdna3_dp_link *l);

#ifdef __cplusplus
}
#endif
#endif
