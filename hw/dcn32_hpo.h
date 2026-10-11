/*
 * dcn32_hpo.h - the HPO (DisplayPort 2.x, 128b/132b channel coding) blocks
 * of DCN 3.2 and the DCCG clocks they run on, ported from amdgpu Display
 * Core (Linux v6.12, MIT):
 *
 *   hpo/dcn31/dcn31_hpo_dp_stream_encoder.c
 *                       dcn31_hpo_dp_stream_enc_enable_stream, _dp_unblank,
 *                       _dp_blank, _disable, _set_stream_attribute,
 *                       _update_dp_info_packets_sdp_line_num,
 *                       _update_dp_info_packets, _stop_dp_info_packets,
 *                       _set_dsc_pps_info_packet, _map_stream_to_link,
 *                       _read_state, dcn31_set_hblank_min_symbol_width
 *   hpo/dcn31/dcn31_hpo_dp_link_encoder.c (dcn32_hpo_dp_link_encoder_funcs
 *                       takes them all but is_in_alt_mode)
 *                       dcn31_hpo_dp_link_enc_enable, _disable,
 *                       _set_link_test_pattern,
 *                       _update_stream_allocation_table (fill_stream_allocation_row_info),
 *                       _set_throttled_vcp_size, _read_state,
 *                       _enable_dp_output, _disable_output, _set_ffe
 *                       (link_transmitter_control: the VBIOS
 *                       DIG1TransmitterControl through the DMCUB, as on DIO)
 *   dccg/dcn32/dcn32_dccg.c   dccg32_set_dtbclk_dto, dccg32_set_dtbclk_p_src,
 *                       dccg32_set_dpstreamclk, dccg32_set_pixel_rate_div,
 *                       dccg32_get_pixel_rate_div
 *   dccg/dcn31/dcn31_dccg.c   dccg31_enable_symclk32_se (get_phy_mux_symclk),
 *                       dccg31_disable_symclk32_se, dccg31_init (with
 *                       dccg31_set_symclk32_le_root_clock_gating),
 *                       dccg31_set_audio_dtbclk_dto (hw/dcn32_audio.c)
 *                       (root clock optimization is off on DCN 3.2: no gate
 *                       writes)
 *
 * Instances (dcn32_resource.c): 4 HPO DP stream encoders (ENGINE_ID_HPO_DP_0..3,
 * VPG 6..9), 2 HPO DP link encoders; one register stride for both.
 * The HPO link encoder's DPHY is enabled by the VBIOS's TransmitterControl
 * (DC never calls link_enable on DCN 3.2; SYMCLK32_LE is the VBIOS's too).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCN32_HPO_H
#define RDNA3_DCN32_HPO_H

#include "dcn32_dp.h"
#include "fixpt31_32.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RD_HPO_DP_STREAM_ENCS 4
#define RD_HPO_DP_LINK_ENCS   2

/* enum dp2_link_mode (DP_DPHY_SYM32_CONTROL.MODE) */
enum { RD_DP2_LINK_TRAINING_TPS1 = 0, RD_DP2_LINK_TRAINING_TPS2, RD_DP2_LINK_ACTIVE, RD_DP2_TEST_PATTERN };
/* enum dp2_phy_tp_select, enum dp2_phy_tp_prbs */
enum { RD_DP_DPHY_TP_SELECT_TPS1 = 0, RD_DP_DPHY_TP_SELECT_TPS2, RD_DP_DPHY_TP_SELECT_PRBS, RD_DP_DPHY_TP_SELECT_CUSTOM,
       RD_DP_DPHY_TP_SELECT_SQUARE };
enum { RD_DP_DPHY_TP_PRBS7 = 0, RD_DP_DPHY_TP_PRBS9, RD_DP_DPHY_TP_PRBS11, RD_DP_DPHY_TP_PRBS15, RD_DP_DPHY_TP_PRBS23,
       RD_DP_DPHY_TP_PRBS31 };

/* the enum dp_test_pattern values the HPO link encoder takes */
enum {
    RD_HPO_TP_VIDEO_MODE = 0,
    RD_HPO_TP_128b_132b_TPS1_TRAINING_MODE,
    RD_HPO_TP_128b_132b_TPS2_TRAINING_MODE,
    RD_HPO_TP_128b_132b_TPS1,
    RD_HPO_TP_128b_132b_TPS2,
    RD_HPO_TP_PRBS7,
    RD_HPO_TP_PRBS9,
    RD_HPO_TP_PRBS11,
    RD_HPO_TP_PRBS15,
    RD_HPO_TP_PRBS23,
    RD_HPO_TP_PRBS31,
    RD_HPO_TP_264BIT_CUSTOM,
    RD_HPO_TP_SQUARE,
    RD_HPO_TP_UNSUPPORTED,
};

/* enum streamclk_source */
enum { RD_STREAMCLK_REFCLK = 0, RD_STREAMCLK_DTBCLK0, RD_STREAMCLK_DPREFCLK };
/* enum phyd32clk_clock_source: PHYD32CLKA + transmitter (get_phyd32clk_src) */
enum { RD_PHYD32CLKA = 0, RD_PHYD32CLKB, RD_PHYD32CLKC, RD_PHYD32CLKD, RD_PHYD32CLKE };

/* struct hpo_dp_stream_encoder_state */
typedef struct rdna3_hpo_se_state {
    uint32_t stream_enc_enabled, vid_stream_enabled, otg_inst;
    uint32_t pixel_encoding, component_depth, compressed_format;
    uint32_t sdp_enabled, mapped_to_link_enc;
} rdna3_hpo_se_state;

/* struct hpo_dp_link_enc_state */
typedef struct rdna3_hpo_le_state {
    uint32_t link_enc_enabled, link_mode, lane_count;
    uint32_t slot_count[4], stream_src[4], vc_rate_x[4], vc_rate_y[4];
} rdna3_hpo_le_state;

/* ---- HPO DP stream encoder 'se' (0..3) ---- */
void dcn32_hpo_se_enable_stream(rdna3_dev *d, uint32_t se);
void dcn32_hpo_se_dp_unblank(rdna3_dev *d, uint32_t se, uint32_t stream_source);
void dcn32_hpo_se_dp_blank(rdna3_dev *d, uint32_t se);
void dcn32_hpo_se_disable(rdna3_dev *d, uint32_t se);
/* dcn31_hpo_dp_stream_enc_set_stream_attribute: mode 'm' (no borders, not
 * interlaced) as 'encoding' (RD_PIXEL_ENCODING_*) at 'depth'
 * (RD_COLOR_DEPTH_*), sRGB, no VSC SDP for the colorimetry;
 * 'compressed' = timing.flags.DSC */
void dcn32_hpo_se_set_stream_attribute(rdna3_dev *d, uint32_t se, const rdna3_mode *m, uint32_t encoding,
                                       uint32_t depth, int compressed, int double_buffer_en);
void dcn32_hpo_se_update_dp_info_packets_sdp_line_num(rdna3_dev *d, uint32_t se, const rdna3_dp_info_frames *f);
void dcn32_hpo_se_update_dp_info_packets(rdna3_dev *d, uint32_t se, const rdna3_dp_info_frames *f);
void dcn32_hpo_se_stop_dp_info_packets(rdna3_dev *d, uint32_t se);
/* the PPS in GSP 11..14 of VPG 6 + se; pps NULL with !enable */
void dcn32_hpo_se_set_dsc_pps_info_packet(rdna3_dev *d, uint32_t se, int enable, const uint8_t *pps,
                                          int immediate_update);
void dcn32_hpo_se_map_stream_to_link(rdna3_dev *d, uint32_t se, uint32_t le);
void dcn32_hpo_se_set_hblank_min_symbol_width(rdna3_dev *d, uint32_t se, uint32_t width);
void dcn32_hpo_se_read_state(rdna3_dev *d, uint32_t se, rdna3_hpo_se_state *s);

/* ---- HPO DP link encoder 'le' (0..1) ---- */
void dcn32_hpo_le_enable(rdna3_dev *d, uint32_t le, uint32_t lanes);
void dcn32_hpo_le_disable(rdna3_dev *d, uint32_t le);
/* RD_HPO_TP_*; 'custom' = 33 bytes for RD_HPO_TP_264BIT_CUSTOM, byte 0 the
 * pulse width for RD_HPO_TP_SQUARE, else NULL */
void dcn32_hpo_le_set_link_test_pattern(rdna3_dev *d, uint32_t le, uint32_t pattern, const uint8_t *custom);
/* the VC payload table: row n's 'fe' is the HPO stream encoder */
void dcn32_hpo_le_update_stream_allocation_table(rdna3_dev *d, uint32_t le, const rdna3_mst_alloc_table *t);
/* STREAM_VC_RATE X.Y of stream encoder 'se'; *x / *y what was written */
void dcn32_hpo_le_set_throttled_vcp_size(rdna3_dev *d, uint32_t le, uint32_t se, rd_fixpt avg_time_slots_per_mtp,
                                         uint32_t *x, uint32_t *y);
void dcn32_hpo_le_read_state(rdna3_dev *d, uint32_t le, rdna3_hpo_le_state *s);
/* the VBIOS transmitter commands (through 'dm'): ENABLE for 128b/132b
 * (signal DP MST, the HPO instance, link rate x 1000 as the symbol clock),
 * DISABLE then the encoder off, SETUP_VSEMPH with the TX FFE preset */
int  dcn32_hpo_le_enable_dp_output(rdna3_dmub *dm, uint32_t le, uint32_t transmitter, int hpd, uint32_t lanes,
                                   uint32_t link_rate);
int  dcn32_hpo_le_disable_output(rdna3_dmub *dm, rdna3_dev *d, uint32_t le, uint32_t transmitter, int hpd,
                                 uint8_t digmode);
int  dcn32_hpo_le_set_ffe(rdna3_dmub *dm, uint32_t le, uint32_t transmitter, uint32_t lanes, uint32_t link_rate,
                          uint8_t ffe_preset);

/* ---- DCCG (DTBCLK, DPSTREAMCLK, SYMCLK32, pixel rate divider) ---- */
/* dccg32_set_dtbclk_dto: the DTBCLK DTO of OTG 'otg' at pixclk / 4 from
 * the reference DTBCLK (both kHz); 0 / 0 turns it off (then the pipe DTO
 * source goes back to DP (1) or HDMI (0)) */
void dcn32_dccg_set_dtbclk_dto(rdna3_dev *d, uint32_t otg, uint32_t pixclk_khz, uint32_t ref_dtbclk_khz, int is_hdmi);
void dcn32_dccg_set_dtbclk_p_src(rdna3_dev *d, uint32_t src, uint32_t otg);
void dcn32_dccg_set_dpstreamclk(rdna3_dev *d, uint32_t src, uint32_t otg, uint32_t hpo_se);
void dcn32_dccg_enable_symclk32_se(rdna3_dev *d, uint32_t hpo_se, uint32_t phyd32clk);
void dcn32_dccg_disable_symclk32_se(rdna3_dev *d, uint32_t hpo_se);
/* dccg31_init (dccg32_funcs.dccg_init): the HPO stream encoders' SYMCLK32
 * back on REFCLK */
void dcn32_dccg_init(rdna3_dev *d);
/* dccg31_set_audio_dtbclk_dto: the audio DTO from DTBCLK ('req' kHz of the
 * 'ref' kHz reference); 0 / 0 = off (DCCG_AUDIO_DTO_SEL 3, no audio DTO) */
void dcn32_dccg_set_audio_dtbclk_dto(rdna3_dev *d, uint32_t ref_dtbclk_khz, uint32_t req_audio_dtbclk_khz);
/* dccg32_set_pixel_rate_div / _get (RD_PIXEL_RATE_DIV_*, dcn32_hwseq.h) */
void dcn32_dccg_set_pixel_rate_div(rdna3_dev *d, uint32_t otg, uint32_t k1, uint32_t k2);
void dcn32_dccg_get_pixel_rate_div(rdna3_dev *d, uint32_t otg, uint32_t *k1, uint32_t *k2);

#ifdef __cplusplus
}
#endif
#endif
