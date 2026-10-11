/*
 * dcn32_dsc.h - Display Stream Compression on DCN 3.2 (DP SST), ported from
 * amdgpu Display Core (Linux v6.12, MIT) and the DRM DSC helper:
 *
 *   dsc/dc_dsc.c            dc_dsc_parse_dsc_dpcd (+ its DPCD field decoders),
 *                           dc_dsc_compute_bandwidth_range, dc_dsc_compute_config,
 *                           setup_dsc_config, decide_dsc_bandwidth_range,
 *                           decide_dsc_target_bpp_x16, get_dsc_enc_caps,
 *                           intersect_dsc_caps, the slice-count helpers,
 *                           dc_dsc_get_policy_for_timing, dc_dsc_stream_bandwidth_in_kbps,
 *                           dc_dsc_stream_bandwidth_overhead_in_kbps,
 *                           dc_bandwidth_in_kbps_from_timing (DSC branch),
 *                           dc_dsc_get_default_config_option
 *   dsc/dcn20/dcn20_dsc.c   dsc2_get_enc_caps (DCN 3.2 creates the DCN 2.0
 *                           DSC encoder), dsc_prepare_config,
 *                           dsc_init_reg_values, dsc_update_from_dsc_parameters,
 *                           dsc2_get_packed_pps, dsc2_set_config,
 *                           dsc_write_to_registers, dsc2_enable, dsc2_disable,
 *                           dsc2_disconnect, dsc2_read_state
 *   dsc/rc_calc.c, dml/dsc/rc_calc_fpu.c, dml/dsc/qp_tables.h
 *                           calc_rc_params, _do_calc_rc_params, get_qp_set,
 *                           get_ofs_set (exact rational arithmetic instead of
 *                           DC's float, see dcn32_dsc.c)
 *   dsc/rc_calc_dpi.c       dscc_compute_dsc_parameters, copy_pps_fields, copy_rc_to_cfg
 *   drm_dsc_helper.c        drm_dsc_compute_rc_parameters, drm_dsc_pps_payload_pack
 *   hwss/dcn32/dcn32_hwseq.c  dcn32_dsc_pg_control, dcn32_dsc_pg_status
 *   optc/dcn30, dcn20       optc3_set_dsc_config (optc2_set_dsc_config + V_SYNC_MODE)
 *   dio/dcn32, dcn30        enc32_dp_set_dsc_config, enc3_dp_set_dsc_pps_info_packet
 *   dcn30/dcn30_vpg.c       vpg3_update_generic_info_packet
 *
 * With ODM combine (hw/dcn32_odm.c) each segment's DSC engine gets its
 * share (dcn32_update_dsc_on_stream: the picture and the slices divided by
 * the segments, is_odm: ICH reset at the end of every line); the PPS is the
 * whole picture's. DCN
 * 3.2 has no DSCCLK DTO in dccg32_funcs (set_dto_dscclk is NULL): DSCCLK is
 * DISPCLK / 3, so dml32's DSCCLK check (pixel rate / 3 within DISPCLK / 3
 * less the downspread) is the DISPCLK check the mode set already makes.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCN32_DSC_H
#define RDNA3_DCN32_DSC_H

#include "rdna3_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RD_DSC_ENGINES        4           /* res_cap num_dsc */
#define RD_DSC_PPS_BYTES      128
#define RD_DSC_NUM_BUF_RANGES 15
#define RD_DSC_DPCD_BASIC     16          /* DP_DSC_SUPPORT 0x060 .. 0x06f */
#define RD_DSC_DPCD_BRANCH    3           /* DP_DSC_BRANCH_OVERALL_THROUGHPUT_0 0x0a0 .. */

/* DPCD (drm_dp.h) */
#define RD_DPCD_DSC_SUPPORT               0x060
#define RD_DPCD_DSC_BRANCH_OVERALL_THROUGHPUT_0 0x0a0
#define RD_DPCD_FEC_CONFIGURATION         0x120
#define RD_DPCD_DSC_ENABLE                0x160
#define RD_DP_FEC_CAPABLE                 (1u << 0)
#define RD_DP_FEC_READY                   (1u << 0)
#define RD_DP_DSC_DECOMPRESSION_IS_SUPPORTED (1u << 0)

/* enum dc_pixel_encoding (the ones DSC knows) */
enum { RD_PIXEL_ENCODING_RGB = 1, RD_PIXEL_ENCODING_YCBCR422, RD_PIXEL_ENCODING_YCBCR444, RD_PIXEL_ENCODING_YCBCR420 };

/* enum optc_dsc_mode */
enum { RD_OPTC_DSC_DISABLED = 0, RD_OPTC_DSC_ENABLED_444, RD_OPTC_DSC_ENABLED_NATIVE_SUBSAMPLED };

/* enum dc_link_encoding_format */
enum { RD_DC_LINK_ENCODING_UNSPECIFIED = 0, RD_DC_LINK_ENCODING_DP_8b_10b, RD_DC_LINK_ENCODING_DP_128b_132b,
       RD_DC_LINK_ENCODING_HDMI_TMDS, RD_DC_LINK_ENCODING_HDMI_FRL };

/* enum dsc_pixel_format */
enum { RD_DSC_PIXFMT_RGB = 0, RD_DSC_PIXFMT_YCBCR444, RD_DSC_PIXFMT_SIMPLE_YCBCR422, RD_DSC_PIXFMT_NATIVE_YCBCR422,
       RD_DSC_PIXFMT_NATIVE_YCBCR420, RD_DSC_PIXFMT_UNKNOWN };

/* union dsc_slice_caps1 / dsc_slice_caps2 (DPCD bits) */
#define RD_DSC_SINK_SLICES_1   (1u << 0)
#define RD_DSC_SINK_SLICES_2   (1u << 1)
#define RD_DSC_SINK_SLICES_4   (1u << 3)
#define RD_DSC_SINK_SLICES_8   (1u << 5)
#define RD_DSC_SINK_SLICES_12  (1u << 7)
#define RD_DSC_SINK_SLICES_16  (1u << 0)   /* slice_caps2 */
/* union dsc_enc_slice_caps */
#define RD_DSC_ENC_SLICES_1    (1u << 0)
#define RD_DSC_ENC_SLICES_2    (1u << 1)
#define RD_DSC_ENC_SLICES_3    (1u << 2)
#define RD_DSC_ENC_SLICES_4    (1u << 3)
#define RD_DSC_ENC_SLICES_8    (1u << 4)
#define RD_DSC_ENC_SLICES_12   (1u << 5)
#define RD_DSC_ENC_SLICES_16   (1u << 6)
/* union dsc_color_formats */
#define RD_DSC_FMT_RGB          (1u << 0)
#define RD_DSC_FMT_YCBCR_444    (1u << 1)
#define RD_DSC_FMT_SIMPLE_422   (1u << 2)
#define RD_DSC_FMT_NATIVE_422   (1u << 3)
#define RD_DSC_FMT_NATIVE_420   (1u << 4)
/* union dsc_color_depth */
#define RD_DSC_DEPTH_8_BPC      (1u << 1)
#define RD_DSC_DEPTH_10_BPC     (1u << 2)
#define RD_DSC_DEPTH_12_BPC     (1u << 3)

/* struct dsc_dec_dpcd_caps */
typedef struct rdna3_dsc_dec_caps {
    int      is_dsc_supported;
    uint8_t  dsc_version;
    int32_t  rc_buffer_size;
    uint8_t  slice_caps1, slice_caps2;
    int32_t  lb_bit_depth;
    int      is_block_pred_supported;
    int32_t  edp_max_bits_per_pixel;
    uint8_t  color_formats, color_depth;
    int32_t  throughput_mode_0_mps, throughput_mode_1_mps;
    int32_t  max_slice_width;
    uint32_t bpp_increment_div;
    uint32_t branch_overall_throughput_0_mps, branch_overall_throughput_1_mps, branch_max_line_width;
    int      is_dp;
} rdna3_dsc_dec_caps;

/* struct dsc_enc_caps */
typedef struct rdna3_dsc_enc_caps {
    uint8_t  dsc_version;
    uint8_t  slice_caps;
    int32_t  lb_bit_depth;
    int      is_block_pred_supported;
    uint8_t  color_formats, color_depth;
    int32_t  max_total_throughput_mps;
    int32_t  max_slice_width;
    uint32_t bpp_increment_div;
    uint32_t edp_sink_max_bits_per_pixel;
    int      is_dp;
} rdna3_dsc_enc_caps;

/* struct dc_dsc_config */
typedef struct rdna3_dsc_config {
    uint32_t num_slices_h, num_slices_v;
    uint32_t bits_per_pixel;          /* x16 */
    int      block_pred_enable;
    uint32_t linebuf_depth;
    uint32_t version_minor;
    int      ycbcr422_simple;
    int      is_dp;
} rdna3_dsc_config;

/* struct dc_dsc_bw_range */
typedef struct rdna3_dsc_bw_range {
    uint32_t min_kbps, min_target_bpp_x16, max_kbps, max_target_bpp_x16, stream_kbps;
} rdna3_dsc_bw_range;

/* struct dc_dsc_policy */
typedef struct rdna3_dsc_policy {
    int      use_min_slices_h;
    int      max_slices_h;
    int      min_slice_height;
    uint32_t max_target_bpp, min_target_bpp;
    int      enable_dsc_when_not_needed;
} rdna3_dsc_policy;

/* struct dc_dsc_config_options */
typedef struct rdna3_dsc_options {
    uint32_t dsc_min_slice_height_override;
    uint32_t max_target_bpp_limit_override_x16;
    uint32_t slice_height_granularity;
    uint32_t dsc_force_odm_hslice_override;
    int      force_dsc_when_not_needed;
} rdna3_dsc_options;

/* the parts of struct dc_crtc_timing DSC reads (no borders) */
typedef struct rdna3_dsc_timing {
    uint32_t h_addressable, v_addressable, h_total, v_total;
    uint32_t pix_clk_100hz;
    uint32_t bpc;                     /* display_color_depth: 6, 8, 10, 12, 14, 16 */
    uint32_t pixel_encoding;          /* RD_PIXEL_ENCODING_* */
    int      y_only;
    uint32_t dsc_fixed_bits_per_pixel_x16;
    int      dsc;                     /* flags.DSC */
    rdna3_dsc_config dsc_cfg;
    /* the link encoding dc_bandwidth_in_kbps_from_timing is asked for
     * (RD_DC_LINK_ENCODING_*; 0 = unspecified, as 8b/10b) */
    uint32_t link_encoding;
} rdna3_dsc_timing;

/* struct drm_dsc_config (field widths as DRM's) */
typedef struct rdna3_dsc_pps {
    uint8_t  line_buf_depth, bits_per_component;
    int      convert_rgb;
    uint16_t slice_width, slice_height;
    int      simple_422;
    uint16_t pic_width, pic_height;
    uint8_t  rc_tgt_offset_high, rc_tgt_offset_low;
    uint16_t bits_per_pixel;          /* x16 */
    uint8_t  rc_edge_factor, rc_quant_incr_limit1, rc_quant_incr_limit0;
    uint16_t initial_xmit_delay, initial_dec_delay;
    int      block_pred_enable;
    uint8_t  first_line_bpg_offset;
    uint16_t initial_offset;
    uint16_t rc_buf_thresh[RD_DSC_NUM_BUF_RANGES - 1];
    struct { uint8_t range_min_qp, range_max_qp, range_bpg_offset; } rc_range_params[RD_DSC_NUM_BUF_RANGES];
    uint16_t rc_model_size;
    uint8_t  flatness_min_qp, flatness_max_qp;
    uint8_t  initial_scale_value;
    uint16_t scale_decrement_interval, scale_increment_interval;
    uint16_t nfl_bpg_offset, slice_bpg_offset;
    uint16_t final_offset;
    int      vbr_enable;
    uint8_t  mux_word_size;
    uint16_t slice_chunk_size;
    uint16_t rc_bits;
    uint8_t  dsc_version_minor, dsc_version_major;
    int      native_422, native_420;
    uint8_t  second_line_bpg_offset;
    uint16_t nsl_bpg_offset, second_line_offset_adj;
} rdna3_dsc_pps;

/* struct dsc_reg_values (the non-PPS fields DC uses) */
typedef struct rdna3_dsc_reg_values {
    uint32_t pixel_format, num_slices_h, num_slices_v;
    uint32_t alternate_ich_encoding_en, ich_reset_at_eol, bpp_x32, rc_buffer_model_size;
    uint32_t dsc_clock_enable, dsc_clock_gating_disable, underflow_recovery_en, underflow_occurred_int_en;
    uint32_t underflow_occurred_status, dsc_dbg_en, rc_buffer_model_overflow_int_en[4];
    rdna3_dsc_pps pps;
} rdna3_dsc_reg_values;

/* struct dsc_config */
typedef struct rdna3_dsc_hw_config {
    uint32_t pic_width, pic_height;
    uint32_t pixel_encoding;
    uint32_t bpc;
    int      is_odm;
    rdna3_dsc_config dc_dsc_cfg;
} rdna3_dsc_hw_config;

/* struct dsc_optc_config */
typedef struct rdna3_dsc_optc_config {
    uint32_t slice_width;
    uint32_t bytes_per_pixel;         /* u3.28 */
    int      is_pixel_format_444;
} rdna3_dsc_optc_config;

/* struct dcn_dsc_state */
typedef struct rdna3_dsc_state {
    uint32_t dsc_clock_en, dsc_slice_width, dsc_bits_per_pixel, dsc_slice_height, dsc_pic_width;
    uint32_t dsc_pic_height, dsc_slice_bpg_offset, dsc_chunk_size, dsc_fw_en, dsc_opp_source;
} rdna3_dsc_state;

/* ---- policy (dc_dsc.c) ---- */

/* dc_dsc_parse_dsc_dpcd: 'basic' = DPCD 0x060..0x06f, 'branch' = 0x0a0..0x0a2
 * or NULL (no branch device). 1 when DSC is supported and every field decodes. */
int  dcn32_dsc_parse_dsc_dpcd(const uint8_t *basic, const uint8_t *branch, rdna3_dsc_dec_caps *caps);
/* dc_dsc_get_default_config_option */
void dcn32_dsc_default_options(rdna3_dsc_options *o);
/* dc_dsc_compute_config: 1 with *cfg set when DSC can (and per policy must)
 * carry 'timing' in 'target_kbps' (0: no target bpp, the max slices etc.) */
int  dcn32_dsc_compute_config(const rdna3_dsc_dec_caps *sink, const rdna3_dsc_options *o, uint32_t target_kbps,
                              const rdna3_dsc_timing *timing, rdna3_dsc_config *cfg);
/* dc_dsc_compute_bandwidth_range */
int  dcn32_dsc_compute_bandwidth_range(uint32_t min_bpp_x16, uint32_t max_bpp_x16, const rdna3_dsc_dec_caps *sink,
                                       const rdna3_dsc_timing *timing, rdna3_dsc_bw_range *range);
/* dc_dsc_stream_bandwidth_in_kbps / dc_dsc_stream_bandwidth_overhead_in_kbps */
uint32_t dcn32_dsc_stream_kbps(const rdna3_dsc_timing *timing, uint32_t bpp_x16, uint32_t num_slices_h, int is_dp);
uint32_t dcn32_dsc_overhead_kbps(const rdna3_dsc_timing *timing, uint32_t num_slices_h, int is_dp);
/* dc_bandwidth_in_kbps_from_timing in t->link_encoding (128b/132b: the
 * apply_128b_132b_stream_overhead padding); with flags.DSC the compressed rate */
uint32_t dcn32_dsc_timing_kbps(const rdna3_dsc_timing *timing);
/* dsc2_get_enc_caps */
void dcn32_dsc_get_enc_caps(rdna3_dsc_enc_caps *caps, uint32_t pixel_clock_100hz);
/* dc_dsc_get_policy_for_timing */
void dcn32_dsc_get_policy(const rdna3_dsc_timing *timing, uint32_t max_target_bpp_limit_override_x16,
                          rdna3_dsc_policy *policy);

/* ---- PPS (dcn20_dsc.c, rc_calc*.c, drm_dsc_helper.c) ---- */

/* dsc_prepare_config: the register values and the OPTC side of 'cfg'.
 * 1, or 0 for parameters DSC cannot take. */
int  dcn32_dsc_prepare_config(const rdna3_dsc_hw_config *cfg, rdna3_dsc_reg_values *rv, rdna3_dsc_optc_config *optc);
/* dsc2_get_packed_pps: the 128-byte PPS (drm_dsc_pps_payload_pack) */
int  dcn32_dsc_get_packed_pps(const rdna3_dsc_hw_config *cfg, uint8_t *pps);
/* drm_dsc_pps_payload_pack alone */
void dcn32_dsc_pps_payload_pack(uint8_t *pps, const rdna3_dsc_pps *cfg);

/* ---- hardware ---- */

/* dcn32_dsc_pg_control / dcn32_dsc_pg_status (DOMAIN16..19); 0 or RDNA3_ETIME */
int  dcn32_dsc_pg_control(rdna3_dev *d, uint32_t inst, int power_on);
int  dcn32_dsc_pg_status(rdna3_dev *d, uint32_t inst);
/* dsc2_set_config: dsc_prepare_config + dsc_write_to_registers. 0 or RDNA3_EINVAL. */
int  dcn32_dsc_set_config(rdna3_dev *d, uint32_t inst, const rdna3_dsc_hw_config *cfg, rdna3_dsc_optc_config *optc);
void dcn32_dsc_enable(rdna3_dev *d, uint32_t inst, uint32_t opp_pipe);     /* dsc2_enable */
void dcn32_dsc_disable(rdna3_dev *d, uint32_t inst);                       /* dsc2_disable */
void dcn32_dsc_disconnect(rdna3_dev *d, uint32_t inst);                    /* dsc2_disconnect */
void dcn32_dsc_read_state(rdna3_dev *d, uint32_t inst, rdna3_dsc_state *s);/* dsc2_read_state */
/* dsc2_wait_disconnect_pending_clear (DC: post_unlock_reset_opp, before
 * dsc_disable on a seamless update) */
int  dcn32_dsc_wait_disconnect_pending_clear(rdna3_dev *d, uint32_t inst);
/* optc3_set_dsc_config on OTG 'otg' */
void dcn32_optc_set_dsc_config(rdna3_dev *d, uint32_t otg, uint32_t dsc_mode, uint32_t bytes_per_pixel,
                               uint32_t slice_width);
/* enc32_dp_set_dsc_config on DIG front end 'fe' */
void dcn32_enc_dp_set_dsc_config(rdna3_dev *d, uint32_t fe, uint32_t dsc_mode);
/* vpg3_update_generic_info_packet for the other packets of a VPG (the
 * HDMI infoframes): header hb[0..3] and sb[0..31] into generic packet
 * 'index', taken at once or at the next frame */
void dcn32_vpg_update_generic_info_packet(rdna3_dev *d, uint32_t vpg, uint32_t index, const uint8_t *hb,
                                          const uint8_t *sb, int immediate_update);
/* enc3_dp_set_dsc_pps_info_packet (VPG n = DIG front end n); pps NULL with !enable */
void dcn32_enc_dp_set_dsc_pps_info_packet(rdna3_dev *d, uint32_t fe, int enable, const uint8_t *pps,
                                          int immediate_update);

#ifdef __cplusplus
}
#endif
#endif
