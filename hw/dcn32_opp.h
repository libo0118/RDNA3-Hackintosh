/*
 * dcn32_opp.h - the output pixel formats of DCN 3.2: the OPP's formatter
 * (FMT) and display pattern generator (DPG), and the output format
 * amdgpu_dm chooses for an HDMI stream, ported from Linux amdgpu Display
 * Core / amdgpu_dm (v6.12, MIT):
 *
 *   opp/dcn10/dcn10_opp.c   opp1_program_fmt, opp1_program_bit_depth_reduction
 *                           (opp1_set_truncation, opp1_set_spatial_dither;
 *                           temporal dither is a TODO in DC),
 *                           opp1_program_clamping_and_pixel_encoding
 *                           (opp1_set_clamping, opp1_set_pixel_encoding),
 *                           opp1_set_dyn_expansion
 *   opp/dcn20/dcn20_opp.c   opp2_set_disp_pattern_generator,
 *                           opp2_program_dpg_dimensions, opp2_dpg_set_blank_color,
 *                           opp2_dpg_is_blanked, opp2_dpg_is_pending
 *   core/dc_resource.c      resource_build_bit_depth_reduction_params
 *   resource/dcn20          build_clamping_params (build_pipe_hw_param)
 *   core/dc_hw_sequencer.c  color_space_to_black_color, get_color_space_type
 *   hwss/dcn20              dcn20_blank_pixel_data, dcn20_program_output_csc
 *   dce/dce_clock_source.c  dce112_program_pixel_clk_resync (the PHY PLL's
 *                           deep color ratio), dcn3_get_pix_clk_dividers'
 *                           HDMI deep color clock
 *   amdgpu_dm/amdgpu_dm.c   fill_stream_properties_from_drm_display_mode's
 *                           pixel encoding, convert_color_depth_from_display_info,
 *                           adjust_colour_depth_from_display_info,
 *                           get_output_color_space; create_validate_stream_for_sink's
 *                           lower-bpc retry
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCN32_OPP_H
#define RDNA3_DCN32_OPP_H

#include "rdna3_hw.h"
#include "edid.h"
#include "dcn32_hdmi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* struct bit_depth_reduction_params (flags) */
typedef struct rdna3_bit_depth_params {
    uint8_t truncate_enabled, truncate_depth, truncate_mode;
    uint8_t spatial_dither_enabled, spatial_dither_depth, spatial_dither_mode;
    uint8_t rgb_random, frame_random, highpass_random;
    uint8_t frame_modulation_enabled, frame_modulation_depth;
    uint32_t r_seed_value, g_seed_value, b_seed_value;
    uint32_t pixel_encoding;
} rdna3_bit_depth_params;

/* enum clamping_range */
enum { RD_CLAMPING_FULL_RANGE = 0, RD_CLAMPING_LIMITED_RANGE_8BPC, RD_CLAMPING_LIMITED_RANGE_10BPC,
       RD_CLAMPING_LIMITED_RANGE_12BPC, RD_CLAMPING_LIMITED_RANGE_PROGRAMMABLE };
/* struct clamping_and_pixel_encoding_params */
typedef struct rdna3_clamping_params {
    uint32_t pixel_encoding, clamping_level, c_depth;
} rdna3_clamping_params;

/* enum dc_dither_option (the ones resource_build_bit_depth_reduction_params tells apart) */
enum {
    RD_DITHER_OPTION_DEFAULT = 0, RD_DITHER_OPTION_DISABLE, RD_DITHER_OPTION_FM6, RD_DITHER_OPTION_FM8,
    RD_DITHER_OPTION_FM10, RD_DITHER_OPTION_SPATIAL6_FRAME_RANDOM, RD_DITHER_OPTION_SPATIAL8_FRAME_RANDOM,
    RD_DITHER_OPTION_SPATIAL10_FRAME_RANDOM, RD_DITHER_OPTION_SPATIAL6, RD_DITHER_OPTION_SPATIAL8,
    RD_DITHER_OPTION_SPATIAL10, RD_DITHER_OPTION_TRUN6, RD_DITHER_OPTION_TRUN8, RD_DITHER_OPTION_TRUN10,
    RD_DITHER_OPTION_TRUN10_SPATIAL8, RD_DITHER_OPTION_TRUN10_SPATIAL6, RD_DITHER_OPTION_TRUN10_FM8,
    RD_DITHER_OPTION_TRUN10_FM6, RD_DITHER_OPTION_TRUN10_SPATIAL8_FM6, RD_DITHER_OPTION_SPATIAL10_FM8,
    RD_DITHER_OPTION_SPATIAL10_FM6, RD_DITHER_OPTION_TRUN8_SPATIAL6, RD_DITHER_OPTION_TRUN8_FM6,
    RD_DITHER_OPTION_SPATIAL8_FM6, RD_DITHER_OPTION_MAX = RD_DITHER_OPTION_SPATIAL8_FM6, RD_DITHER_OPTION_INVALID
};

/* resource_build_bit_depth_reduction_params for a stream of 'color_depth'
 * and 'pixel_encoding' (dither_option DEFAULT: spatial 8 for 8 bpc, the
 * 10-bit truncation for 10 bpc) */
void dcn32_build_bit_depth_params(uint32_t dither_option, uint32_t color_depth, uint32_t pixel_encoding,
                                  rdna3_bit_depth_params *p);
/* build_clamping_params */
void dcn32_build_clamping_params(uint32_t color_depth, uint32_t pixel_encoding, rdna3_clamping_params *c);

/* ---- the OPP (instance 'opp') ---- */
void dcn32_opp_program_bit_depth_reduction(rdna3_dev *d, uint32_t opp, const rdna3_bit_depth_params *p);
void dcn32_opp_program_clamping_and_pixel_encoding(rdna3_dev *d, uint32_t opp, const rdna3_clamping_params *c);
void dcn32_opp_program_fmt(rdna3_dev *d, uint32_t opp, const rdna3_bit_depth_params *p, const rdna3_clamping_params *c);
/* opp1_set_dyn_expansion (dyn_expansion AUTO; the color space argument
 * DC passes is always YCbCr 601 and unused) */
void dcn32_opp_set_dyn_expansion(rdna3_dev *d, uint32_t opp, uint32_t color_depth, uint32_t signal);

/* struct tg_color (10-bit components) */
typedef struct rdna3_tg_color {
    uint16_t color_r_cr, color_g_y, color_b_cb;
} rdna3_tg_color;
/* enum controller_dp_test_pattern / controller_dp_color_space (the DPG's) */
enum {
    RD_CONTROLLER_DP_TEST_PATTERN_D102 = 0, RD_CONTROLLER_DP_TEST_PATTERN_SYMBOLERROR, RD_CONTROLLER_DP_TEST_PATTERN_PRBS7,
    RD_CONTROLLER_DP_TEST_PATTERN_COLORSQUARES, RD_CONTROLLER_DP_TEST_PATTERN_VERTICALBARS,
    RD_CONTROLLER_DP_TEST_PATTERN_HORIZONTALBARS, RD_CONTROLLER_DP_TEST_PATTERN_COLORRAMP,
    RD_CONTROLLER_DP_TEST_PATTERN_VIDEOMODE, RD_CONTROLLER_DP_TEST_PATTERN_RESERVED_8,
    RD_CONTROLLER_DP_TEST_PATTERN_RESERVED_9, RD_CONTROLLER_DP_TEST_PATTERN_RESERVED_A,
    RD_CONTROLLER_DP_TEST_PATTERN_COLORSQUARES_CEA, RD_CONTROLLER_DP_TEST_PATTERN_SOLID_COLOR,
};
enum { RD_CONTROLLER_DP_COLOR_SPACE_RGB = 0, RD_CONTROLLER_DP_COLOR_SPACE_YCBCR601, RD_CONTROLLER_DP_COLOR_SPACE_YCBCR709,
       RD_CONTROLLER_DP_COLOR_SPACE_UDEFINED };
/* enum test_pattern_mode (DPG_MODE) */
enum { RD_TEST_PATTERN_MODE_COLORSQUARES_RGB = 0, RD_TEST_PATTERN_MODE_COLORSQUARES_YCBCR601,
       RD_TEST_PATTERN_MODE_COLORSQUARES_YCBCR709, RD_TEST_PATTERN_MODE_VERTICALBARS,
       RD_TEST_PATTERN_MODE_HORIZONTALBARS, RD_TEST_PATTERN_MODE_SINGLERAMP_RGB, RD_TEST_PATTERN_MODE_DUALRAMP_RGB };

void dcn32_opp_set_disp_pattern_generator(rdna3_dev *d, uint32_t opp, uint32_t test_pattern, uint32_t color_space,
                                          uint32_t color_depth, const rdna3_tg_color *solid_color, uint32_t width,
                                          uint32_t height, uint32_t offset);
void dcn32_opp_program_dpg_dimensions(rdna3_dev *d, uint32_t opp, uint32_t width, uint32_t height);
void dcn32_opp_dpg_set_blank_color(rdna3_dev *d, uint32_t opp, const rdna3_tg_color *color);
int  dcn32_opp_dpg_is_blanked(rdna3_dev *d, uint32_t opp);
int  dcn32_opp_dpg_is_pending(rdna3_dev *d, uint32_t opp);

/* color_space_to_black_color (RD_CS_*) */
void dcn32_color_space_to_black_color(uint32_t color_space, rdna3_tg_color *black);
/* dcn20_blank_pixel_data for a single-OPP stream: blank = the DPG's solid
 * black of the color space, unblank = video */
void dcn32_blank_pixel_data(rdna3_dev *d, uint32_t opp, int blank, uint32_t color_space, uint32_t color_depth,
                            uint32_t width, uint32_t height);
/* dcn20_wait_for_blank_complete: up to 100 ms for the DPG's update, then
 * opp2_dpg_is_blanked */
int  dcn32_wait_for_blank_complete(rdna3_dev *d, uint32_t opp);
/* get_color_space_type: RD_CS_* -> RD_COLOR_SPACE_*_TYPE (dcn32_color.h) */
uint32_t dcn32_color_space_type(uint32_t color_space);
/* dcn20_program_output_csc with no adjustment matrix: the MPC's output CSC
 * for the color space (mpc3_set_ocsc_default, coefficient set A) and
 * mpc3_set_denorm to the stream's depth */
void dcn32_program_output_csc(rdna3_dev *d, uint32_t opp, uint32_t color_space, uint32_t color_depth);

/* dce112_program_pixel_clk_resync: PHYPLL<pll>_PIXCLK_RESYNC_CNTL's deep
 * color ratio for an HDMI signal */
void dcn32_program_pixel_clk_resync(rdna3_dev *d, uint32_t pll, uint32_t signal, uint32_t color_depth, int ycbcr420);
/* dcn3_get_pix_clk_dividers: the PHY PLL clock of an HDMI stream (the
 * pixel clock x 1.25 / 1.5 / 2 for 10 / 12 / 16 bpc) */
uint32_t dcn32_hdmi_pll_clk_khz(uint32_t pixel_khz, uint32_t color_depth);
/* get_pixel_clock_parameters' color depth (4:2:2 runs the PLL at 8 bpc) */
uint32_t dcn32_pix_clk_color_depth(const rdna3_crtc_format *f);

/* ---- amdgpu_dm's output format for an HDMI / DVI stream ---- */
/* the drm_display_info parts it reads (from the EDID: color_formats, bpc,
 * max_tmds_clock in kHz) */
typedef struct rdna3_sink_color_caps {
    uint8_t  color_formats, bpc;
    uint32_t max_tmds_clock_khz;
} rdna3_sink_color_caps;
void dcn32_sink_color_caps(const rdna3_edid *e, rdna3_sink_color_caps *c);
/* convert_color_depth_from_display_info (not 4:2:0): the EDID's bpc (8 when
 * not given), capped at 'requested_bpc' rounded down to even */
uint32_t dcn32_convert_color_depth(uint32_t edid_bpc, uint32_t requested_bpc);
/* adjust_colour_depth_from_display_info: the deepest color at or below
 * f->color_depth whose TMDS clock fits the sink's max TMDS clock (kHz); 0
 * when none does (the depth is then left) */
int  dcn32_adjust_colour_depth(rdna3_crtc_format *f, uint32_t pixel_khz, uint32_t max_tmds_clock_khz);
/* get_output_color_space with DRM_MODE_COLORIMETRY_DEFAULT */
uint32_t dcn32_output_color_space(const rdna3_crtc_format *f, uint32_t pixel_khz);
/* fill_stream_properties_from_drm_display_mode for 'signal': YCbCr 4:4:4
 * on an HDMI sink that has it, else RGB; the depth from 'requested_bpc'
 * (the connector's max bpc: 16 unless userspace lowers it) and the HDMI
 * TMDS adjustment. 'force_yuv422' is this port's own switch (amdgpu v6.12
 * has only force_yuv420_output, set by debugfs): YCbCr 4:2:2 on a sink
 * that lists it, in the place force_yuv420_output takes in the chain.
 * (4:2:0-only modes need the EDID's YCbCr 4:2:0 maps, which this port
 * does not read: none is 4:2:0.) Returns the color space (RD_CS_*). */
uint32_t dcn32_output_format(const rdna3_sink_color_caps *caps, const rdna3_mode *m, uint32_t signal, uint32_t requested_bpc,
                             int force_yuv422, rdna3_crtc_format *f);

#ifdef __cplusplus
}
#endif
#endif
