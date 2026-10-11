/*
 * dcn32_opp.c - see dcn32_opp.h. Ported from Linux amdgpu Display Core
 * (opp/dcn10/dcn10_opp.c, opp/dcn20/dcn20_opp.c, core/dc_resource.c,
 * resource/dcn20/dcn20_resource.c, core/dc_hw_sequencer.c,
 * hwss/dcn20/dcn20_hwseq.c, dce/dce_clock_source.c) and amdgpu_dm
 * (amdgpu_dm.c), v6.12, MIT, Copyright Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_opp.h"
#include "dcn32_color.h"
#include "gen/dcn_3_2_0_fmt.h"

#define FLD(reg, field, x) (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK)
#define MSK(reg, field) ((uint32_t)reg##__##field##_MASK)
#define GETF(v, reg, field) (((v) & (uint32_t)reg##__##field##_MASK) >> reg##__##field##__SHIFT)

/* FMT and DPG of OPP n */
#define FMT_S (regFMT1_FMT_CONTROL - regFMT0_FMT_CONTROL)
#define DPG_S (regDPG1_DPG_CONTROL - regDPG0_DPG_CONTROL)
#define FA(d, reg, n) (rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + FMT_S * (n))
#define DA(d, reg, n) (rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + DPG_S * (n))
#define F(field, x) FLD(FMT0_FMT_BIT_DEPTH_CONTROL, field, x)
#define FM(field) MSK(FMT0_FMT_BIT_DEPTH_CONTROL, field)

static void upd(rdna3_dev *d, uint32_t a, uint32_t mask, uint32_t v)
{
    rdna3_wreg(d, a, (rdna3_rreg(d, a) & ~mask) | (v & mask));
}

/* ==================== dc_resource.c / dcn20_resource.c ==================== */

/* resource_build_bit_depth_reduction_params */
void dcn32_build_bit_depth_params(uint32_t option, uint32_t color_depth, uint32_t pixel_encoding,
                                  rdna3_bit_depth_params *p)
{
    uint32_t i;
    for (i = 0; i < sizeof(*p); i++)
        ((uint8_t *)p)[i] = 0;
    if (option == RD_DITHER_OPTION_DEFAULT) {
        switch (color_depth) {
        case RD_COLOR_DEPTH_666: option = RD_DITHER_OPTION_SPATIAL6; break;
        case RD_COLOR_DEPTH_888: option = RD_DITHER_OPTION_SPATIAL8; break;
        case RD_COLOR_DEPTH_101010: option = RD_DITHER_OPTION_TRUN10; break;
        default: option = RD_DITHER_OPTION_DISABLE; break;
        }
    }
    if (option == RD_DITHER_OPTION_DISABLE)
        return;
    if (option == RD_DITHER_OPTION_TRUN6) {
        p->truncate_enabled = 1;
        p->truncate_depth = 0;
    } else if (option == RD_DITHER_OPTION_TRUN8 || option == RD_DITHER_OPTION_TRUN8_SPATIAL6 ||
               option == RD_DITHER_OPTION_TRUN8_FM6) {
        p->truncate_enabled = 1;
        p->truncate_depth = 1;
    } else if (option == RD_DITHER_OPTION_TRUN10 || option == RD_DITHER_OPTION_TRUN10_SPATIAL6 ||
               option == RD_DITHER_OPTION_TRUN10_SPATIAL8 || option == RD_DITHER_OPTION_TRUN10_FM8 ||
               option == RD_DITHER_OPTION_TRUN10_FM6 || option == RD_DITHER_OPTION_TRUN10_SPATIAL8_FM6) {
        p->truncate_enabled = 1;
        p->truncate_depth = 2;
        if (option == RD_DITHER_OPTION_TRUN10)
            p->truncate_mode = 1;
    }
    /* the formatter reduces 4 bits at most: 12 -> 6 truncates to 10 with rounding first */
    if (option == RD_DITHER_OPTION_SPATIAL6_FRAME_RANDOM || option == RD_DITHER_OPTION_SPATIAL6 ||
        option == RD_DITHER_OPTION_FM6) {
        p->truncate_enabled = 1;
        p->truncate_depth = 2;
        p->truncate_mode = 1;
    }
    /* spatial dither (modes 1-3 never used) */
    if (option == RD_DITHER_OPTION_SPATIAL6_FRAME_RANDOM || option == RD_DITHER_OPTION_SPATIAL6 ||
        option == RD_DITHER_OPTION_TRUN10_SPATIAL6 || option == RD_DITHER_OPTION_TRUN8_SPATIAL6) {
        p->spatial_dither_enabled = 1;
        p->spatial_dither_depth = 0;
        p->highpass_random = 1;
        p->rgb_random = pixel_encoding == RD_PIXEL_ENCODING_RGB ? 1 : 0;
    } else if (option == RD_DITHER_OPTION_SPATIAL8_FRAME_RANDOM || option == RD_DITHER_OPTION_SPATIAL8 ||
               option == RD_DITHER_OPTION_SPATIAL8_FM6 || option == RD_DITHER_OPTION_TRUN10_SPATIAL8 ||
               option == RD_DITHER_OPTION_TRUN10_SPATIAL8_FM6) {
        p->spatial_dither_enabled = 1;
        p->spatial_dither_depth = 1;
        p->highpass_random = 1;
        p->rgb_random = pixel_encoding == RD_PIXEL_ENCODING_RGB ? 1 : 0;
    } else if (option == RD_DITHER_OPTION_SPATIAL10_FRAME_RANDOM || option == RD_DITHER_OPTION_SPATIAL10 ||
               option == RD_DITHER_OPTION_SPATIAL10_FM8 || option == RD_DITHER_OPTION_SPATIAL10_FM6) {
        p->spatial_dither_enabled = 1;
        p->spatial_dither_depth = 2;
        p->highpass_random = 1;
        p->rgb_random = pixel_encoding == RD_PIXEL_ENCODING_RGB ? 1 : 0;
    }
    if (option == RD_DITHER_OPTION_SPATIAL6 || option == RD_DITHER_OPTION_SPATIAL8 || option == RD_DITHER_OPTION_SPATIAL10)
        p->frame_random = 0;
    else
        p->frame_random = 1;
    /* temporal dither (frame modulation) */
    if (option == RD_DITHER_OPTION_FM6 || option == RD_DITHER_OPTION_SPATIAL8_FM6 ||
        option == RD_DITHER_OPTION_SPATIAL10_FM6 || option == RD_DITHER_OPTION_TRUN10_FM6 ||
        option == RD_DITHER_OPTION_TRUN8_FM6 || option == RD_DITHER_OPTION_TRUN10_SPATIAL8_FM6) {
        p->frame_modulation_enabled = 1;
        p->frame_modulation_depth = 0;
    } else if (option == RD_DITHER_OPTION_FM8 || option == RD_DITHER_OPTION_SPATIAL10_FM8 ||
               option == RD_DITHER_OPTION_TRUN10_FM8) {
        p->frame_modulation_enabled = 1;
        p->frame_modulation_depth = 1;
    } else if (option == RD_DITHER_OPTION_FM10) {
        p->frame_modulation_enabled = 1;
        p->frame_modulation_depth = 2;
    }
    p->pixel_encoding = pixel_encoding;
}

/* build_clamping_params */
void dcn32_build_clamping_params(uint32_t color_depth, uint32_t pixel_encoding, rdna3_clamping_params *c)
{
    c->clamping_level = RD_CLAMPING_FULL_RANGE;
    c->c_depth = color_depth;
    c->pixel_encoding = pixel_encoding;
}

/* ==================== dcn10_opp.c ==================== */

/* opp1_set_truncation: depth 0 = 18 bpp, 1 = 24 bpp, 2 = 30 bpp */
static void opp1_set_truncation(rdna3_dev *d, uint32_t opp, const rdna3_bit_depth_params *p)
{
    upd(d, FA(d, regFMT0_FMT_BIT_DEPTH_CONTROL, opp), FM(FMT_TRUNCATE_EN) | FM(FMT_TRUNCATE_DEPTH) | FM(FMT_TRUNCATE_MODE),
        F(FMT_TRUNCATE_EN, p->truncate_enabled) | F(FMT_TRUNCATE_DEPTH, p->truncate_depth) |
            F(FMT_TRUNCATE_MODE, p->truncate_mode));
}

/* opp1_set_spatial_dither */
static void opp1_set_spatial_dither(rdna3_dev *d, uint32_t opp, const rdna3_bit_depth_params *p)
{
    const uint32_t ctl = FA(d, regFMT0_FMT_CONTROL, opp), bdc = FA(d, regFMT0_FMT_BIT_DEPTH_CONTROL, opp);
    const uint32_t cm = MSK(FMT0_FMT_CONTROL, FMT_SPATIAL_DITHER_FRAME_COUNTER_MAX) |
                        MSK(FMT0_FMT_CONTROL, FMT_SPATIAL_DITHER_FRAME_COUNTER_BIT_SWAP);
    /* spatial (random) dithering off first */
    upd(d, bdc,
        FM(FMT_SPATIAL_DITHER_EN) | FM(FMT_SPATIAL_DITHER_MODE) | FM(FMT_SPATIAL_DITHER_DEPTH) |
            FM(FMT_TEMPORAL_DITHER_EN) | FM(FMT_HIGHPASS_RANDOM_ENABLE) | FM(FMT_FRAME_RANDOM_ENABLE) |
            FM(FMT_RGB_RANDOM_ENABLE), 0);
    /* FRAME_COUNTER_MAX only with frame random */
    if (p->frame_random == 1) {
        if (p->spatial_dither_depth == 0 || p->spatial_dither_depth == 1)
            upd(d, ctl, cm, FLD(FMT0_FMT_CONTROL, FMT_SPATIAL_DITHER_FRAME_COUNTER_MAX, 15) |
                                FLD(FMT0_FMT_CONTROL, FMT_SPATIAL_DITHER_FRAME_COUNTER_BIT_SWAP, 2));
        else if (p->spatial_dither_depth == 2)
            upd(d, ctl, cm, FLD(FMT0_FMT_CONTROL, FMT_SPATIAL_DITHER_FRAME_COUNTER_MAX, 3) |
                                FLD(FMT0_FMT_CONTROL, FMT_SPATIAL_DITHER_FRAME_COUNTER_BIT_SWAP, 1));
        else
            return;
    } else {
        upd(d, ctl, cm, 0);
    }
    /* the seeds of the R, G, B random values (REG_SET: the offsets 0) */
    rdna3_wreg(d, FA(d, regFMT0_FMT_DITHER_RAND_R_SEED, opp), FLD(FMT0_FMT_DITHER_RAND_R_SEED, FMT_RAND_R_SEED, p->r_seed_value));
    rdna3_wreg(d, FA(d, regFMT0_FMT_DITHER_RAND_G_SEED, opp), FLD(FMT0_FMT_DITHER_RAND_G_SEED, FMT_RAND_G_SEED, p->g_seed_value));
    rdna3_wreg(d, FA(d, regFMT0_FMT_DITHER_RAND_B_SEED, opp), FLD(FMT0_FMT_DITHER_RAND_B_SEED, FMT_RAND_B_SEED, p->b_seed_value));
    upd(d, bdc,
        FM(FMT_SPATIAL_DITHER_EN) | FM(FMT_SPATIAL_DITHER_MODE) | FM(FMT_SPATIAL_DITHER_DEPTH) |
            FM(FMT_HIGHPASS_RANDOM_ENABLE) | FM(FMT_FRAME_RANDOM_ENABLE) | FM(FMT_RGB_RANDOM_ENABLE),
        F(FMT_SPATIAL_DITHER_EN, p->spatial_dither_enabled) | F(FMT_SPATIAL_DITHER_MODE, p->spatial_dither_mode) |
            F(FMT_SPATIAL_DITHER_DEPTH, p->spatial_dither_depth) | F(FMT_HIGHPASS_RANDOM_ENABLE, p->highpass_random) |
            F(FMT_FRAME_RANDOM_ENABLE, p->frame_random) | F(FMT_RGB_RANDOM_ENABLE, p->rgb_random));
}

/* opp1_program_bit_depth_reduction (set_temporal_dither is a TODO in DC) */
void dcn32_opp_program_bit_depth_reduction(rdna3_dev *d, uint32_t opp, const rdna3_bit_depth_params *p)
{
    opp1_set_truncation(d, opp, p);
    opp1_set_spatial_dither(d, opp, p);
}

/* opp1_set_pixel_encoding: 0 = RGB / YCbCr 4:4:4, 1 = 4:2:2, 2 = 4:2:0
 * (force_chroma_subsampling_1tap off) */
static void opp1_set_pixel_encoding(rdna3_dev *d, uint32_t opp, const rdna3_clamping_params *c)
{
    const uint32_t a = FA(d, regFMT0_FMT_CONTROL, opp);
    const uint32_t m = MSK(FMT0_FMT_CONTROL, FMT_PIXEL_ENCODING) | MSK(FMT0_FMT_CONTROL, FMT_SUBSAMPLING_MODE) |
                       MSK(FMT0_FMT_CONTROL, FMT_CBCR_BIT_REDUCTION_BYPASS);
    switch (c->pixel_encoding) {
    case RD_PIXEL_ENCODING_RGB:
    case RD_PIXEL_ENCODING_YCBCR444:
        upd(d, a, m, 0);
        upd(d, a, MSK(FMT0_FMT_CONTROL, FMT_PIXEL_ENCODING), 0);
        break;
    case RD_PIXEL_ENCODING_YCBCR422:
        upd(d, a, m, FLD(FMT0_FMT_CONTROL, FMT_PIXEL_ENCODING, 1) | FLD(FMT0_FMT_CONTROL, FMT_SUBSAMPLING_MODE, 2));
        break;
    case RD_PIXEL_ENCODING_YCBCR420:
        upd(d, a, m, FLD(FMT0_FMT_CONTROL, FMT_PIXEL_ENCODING, 2) | FLD(FMT0_FMT_CONTROL, FMT_SUBSAMPLING_MODE, 2) |
                         FLD(FMT0_FMT_CONTROL, FMT_CBCR_BIT_REDUCTION_BYPASS, 1));
        break;
    default:
        break;
    }
}

/* opp1_set_clamping: full range clamps nothing; limited ranges per bpc */
static void opp1_set_clamping(rdna3_dev *d, uint32_t opp, const rdna3_clamping_params *c)
{
    const uint32_t a = FA(d, regFMT0_FMT_CLAMP_CNTL, opp);
    const uint32_t m = MSK(FMT0_FMT_CLAMP_CNTL, FMT_CLAMP_DATA_EN) | MSK(FMT0_FMT_CLAMP_CNTL, FMT_CLAMP_COLOR_FORMAT);
    upd(d, a, m, 0);
    switch (c->clamping_level) {
    case RD_CLAMPING_FULL_RANGE:
        upd(d, a, m, FLD(FMT0_FMT_CLAMP_CNTL, FMT_CLAMP_DATA_EN, 1));
        break;
    case RD_CLAMPING_LIMITED_RANGE_8BPC:
        upd(d, a, m, FLD(FMT0_FMT_CLAMP_CNTL, FMT_CLAMP_DATA_EN, 1) | FLD(FMT0_FMT_CLAMP_CNTL, FMT_CLAMP_COLOR_FORMAT, 1));
        break;
    case RD_CLAMPING_LIMITED_RANGE_10BPC:
        upd(d, a, m, FLD(FMT0_FMT_CLAMP_CNTL, FMT_CLAMP_DATA_EN, 1) | FLD(FMT0_FMT_CLAMP_CNTL, FMT_CLAMP_COLOR_FORMAT, 2));
        break;
    case RD_CLAMPING_LIMITED_RANGE_12BPC:
        upd(d, a, m, FLD(FMT0_FMT_CLAMP_CNTL, FMT_CLAMP_DATA_EN, 1) | FLD(FMT0_FMT_CLAMP_CNTL, FMT_CLAMP_COLOR_FORMAT, 3));
        break;
    default:   /* CLAMPING_LIMITED_RANGE_PROGRAMMABLE: a TODO in DC */
        break;
    }
}

/* opp1_set_dyn_expansion: 8 -> 12 bit (mode 1), 10 / 12 -> 12 bit (mode 0)
 * for HDMI and DP */
void dcn32_opp_set_dyn_expansion(rdna3_dev *d, uint32_t opp, uint32_t color_depth, uint32_t signal)
{
    const uint32_t a = FA(d, regFMT0_FMT_DYNAMIC_EXP_CNTL, opp);
    const uint32_t m = MSK(FMT0_FMT_DYNAMIC_EXP_CNTL, FMT_DYNAMIC_EXP_EN) | MSK(FMT0_FMT_DYNAMIC_EXP_CNTL, FMT_DYNAMIC_EXP_MODE);
    upd(d, a, m, 0);
    /* opp->dyn_expansion: DYN_EXPANSION_AUTO (dc_stream_set_dyn_expansion is never called) */
    if (signal != RD_SIGNAL_HDMI_TYPE_A && signal != RD_SIGNAL_DISPLAY_PORT && signal != RD_SIGNAL_DISPLAY_PORT_MST)
        return;
    switch (color_depth) {
    case RD_COLOR_DEPTH_888:
        upd(d, a, m, FLD(FMT0_FMT_DYNAMIC_EXP_CNTL, FMT_DYNAMIC_EXP_EN, 1) | FLD(FMT0_FMT_DYNAMIC_EXP_CNTL, FMT_DYNAMIC_EXP_MODE, 1));
        break;
    case RD_COLOR_DEPTH_101010:
    case RD_COLOR_DEPTH_121212:
        upd(d, a, m, FLD(FMT0_FMT_DYNAMIC_EXP_CNTL, FMT_DYNAMIC_EXP_EN, 1));
        break;
    default:
        break;
    }
}

/* opp1_program_clamping_and_pixel_encoding */
void dcn32_opp_program_clamping_and_pixel_encoding(rdna3_dev *d, uint32_t opp, const rdna3_clamping_params *c)
{
    opp1_set_clamping(d, opp, c);
    opp1_set_pixel_encoding(d, opp, c);
}

/* opp1_program_fmt: the 4:2:0 memory powered, dithering after the source
 * select, then clamping and the encoding */
void dcn32_opp_program_fmt(rdna3_dev *d, uint32_t opp, const rdna3_bit_depth_params *p, const rdna3_clamping_params *c)
{
    if (c->pixel_encoding == RD_PIXEL_ENCODING_YCBCR420)
        upd(d, FA(d, regFMT0_FMT_MAP420_MEMORY_CONTROL, opp), MSK(FMT0_FMT_MAP420_MEMORY_CONTROL, FMT_MAP420MEM_PWR_FORCE), 0);
    dcn32_opp_program_bit_depth_reduction(d, opp, p);
    dcn32_opp_program_clamping_and_pixel_encoding(d, opp, c);
}

/* ==================== dcn20_opp.c: the display pattern generator ==================== */

#define DPGC(field, x) FLD(DPG0_DPG_CONTROL, field, x)
#define DPGM(field) MSK(DPG0_DPG_CONTROL, field)

/* enum test_pattern_color_format, enum test_pattern_dyn_range */
enum { TP_BPC_6 = 0, TP_BPC_8, TP_BPC_10, TP_BPC_12 };
enum { TP_DYN_RANGE_VESA = 0, TP_DYN_RANGE_CEA };

static void dpg_colours(rdna3_dev *d, uint32_t opp, uint32_t c1r, uint32_t c0r, uint32_t c1g, uint32_t c0g, uint32_t c1b,
                        uint32_t c0b)
{
    rdna3_wreg(d, DA(d, regDPG0_DPG_COLOUR_R_CR, opp),
               FLD(DPG0_DPG_COLOUR_R_CR, DPG_COLOUR1_R_CR, c1r) | FLD(DPG0_DPG_COLOUR_R_CR, DPG_COLOUR0_R_CR, c0r));
    rdna3_wreg(d, DA(d, regDPG0_DPG_COLOUR_G_Y, opp),
               FLD(DPG0_DPG_COLOUR_G_Y, DPG_COLOUR1_G_Y, c1g) | FLD(DPG0_DPG_COLOUR_G_Y, DPG_COLOUR0_G_Y, c0g));
    rdna3_wreg(d, DA(d, regDPG0_DPG_COLOUR_B_CB, opp),
               FLD(DPG0_DPG_COLOUR_B_CB, DPG_COLOUR1_B_CB, c1b) | FLD(DPG0_DPG_COLOUR_B_CB, DPG_COLOUR0_B_CB, c0b));
}

/* opp2_dpg_set_blank_color: 16-bit MSB aligned, bits 3:0 hardwired to 0 */
void dcn32_opp_dpg_set_blank_color(rdna3_dev *d, uint32_t opp, const rdna3_tg_color *c)
{
    rdna3_wreg(d, DA(d, regDPG0_DPG_COLOUR_B_CB, opp),
               FLD(DPG0_DPG_COLOUR_B_CB, DPG_COLOUR1_B_CB, (uint32_t)c->color_b_cb << 6) |
                   FLD(DPG0_DPG_COLOUR_B_CB, DPG_COLOUR0_B_CB, (uint32_t)c->color_b_cb << 6));
    rdna3_wreg(d, DA(d, regDPG0_DPG_COLOUR_G_Y, opp),
               FLD(DPG0_DPG_COLOUR_G_Y, DPG_COLOUR1_G_Y, (uint32_t)c->color_g_y << 6) |
                   FLD(DPG0_DPG_COLOUR_G_Y, DPG_COLOUR0_G_Y, (uint32_t)c->color_g_y << 6));
    rdna3_wreg(d, DA(d, regDPG0_DPG_COLOUR_R_CR, opp),
               FLD(DPG0_DPG_COLOUR_R_CR, DPG_COLOUR1_R_CR, (uint32_t)c->color_r_cr << 6) |
                   FLD(DPG0_DPG_COLOUR_R_CR, DPG_COLOUR0_R_CR, (uint32_t)c->color_r_cr << 6));
}

/* opp2_program_dpg_dimensions */
void dcn32_opp_program_dpg_dimensions(rdna3_dev *d, uint32_t opp, uint32_t width, uint32_t height)
{
    rdna3_wreg(d, DA(d, regDPG0_DPG_DIMENSIONS, opp),
               FLD(DPG0_DPG_DIMENSIONS, DPG_ACTIVE_WIDTH, width) | FLD(DPG0_DPG_DIMENSIONS, DPG_ACTIVE_HEIGHT, height));
}

/* opp2_set_disp_pattern_generator */
void dcn32_opp_set_disp_pattern_generator(rdna3_dev *d, uint32_t opp, uint32_t test_pattern, uint32_t color_space,
                                          uint32_t color_depth, const rdna3_tg_color *solid_color, uint32_t width,
                                          uint32_t height, uint32_t offset)
{
    /* the color ramp generator mixes 16-bit color; two bar colors: white, black */
    static const uint16_t src_color[6] = { 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000 };
    const uint32_t src_bpc = 16, ctl = DA(d, regDPG0_DPG_CONTROL, opp);
    uint32_t bit_depth, dst_bpc, index, mode, dyn_range, inc_base;
    uint16_t dst_color[6];

    switch (color_depth) {
    case RD_COLOR_DEPTH_666: bit_depth = TP_BPC_6; break;
    case RD_COLOR_DEPTH_888: bit_depth = TP_BPC_8; break;
    case RD_COLOR_DEPTH_101010: bit_depth = TP_BPC_10; break;
    case RD_COLOR_DEPTH_121212: bit_depth = TP_BPC_12; break;
    default: bit_depth = TP_BPC_8; break;
    }
    /* the DPG dimensions and offset */
    dcn32_opp_program_dpg_dimensions(d, opp, width, height);
    rdna3_wreg(d, DA(d, regDPG0_DPG_OFFSET_SEGMENT, opp), FLD(DPG0_DPG_OFFSET_SEGMENT, DPG_X_OFFSET, offset));

    switch (test_pattern) {
    case RD_CONTROLLER_DP_TEST_PATTERN_COLORSQUARES:
    case RD_CONTROLLER_DP_TEST_PATTERN_COLORSQUARES_CEA:
        dyn_range = test_pattern == RD_CONTROLLER_DP_TEST_PATTERN_COLORSQUARES_CEA ? TP_DYN_RANGE_CEA : TP_DYN_RANGE_VESA;
        mode = color_space == RD_CONTROLLER_DP_COLOR_SPACE_YCBCR601   ? RD_TEST_PATTERN_MODE_COLORSQUARES_YCBCR601
             : color_space == RD_CONTROLLER_DP_COLOR_SPACE_YCBCR709 ? RD_TEST_PATTERN_MODE_COLORSQUARES_YCBCR709
                                                                    : RD_TEST_PATTERN_MODE_COLORSQUARES_RGB;
        upd(d, ctl, DPGM(DPG_EN) | DPGM(DPG_MODE) | DPGM(DPG_DYNAMIC_RANGE) | DPGM(DPG_BIT_DEPTH) | DPGM(DPG_VRES) | DPGM(DPG_HRES),
            DPGC(DPG_EN, 1) | DPGC(DPG_MODE, mode) | DPGC(DPG_DYNAMIC_RANGE, dyn_range) | DPGC(DPG_BIT_DEPTH, bit_depth) |
                DPGC(DPG_VRES, 6) | DPGC(DPG_HRES, 6));
        break;
    case RD_CONTROLLER_DP_TEST_PATTERN_VERTICALBARS:
    case RD_CONTROLLER_DP_TEST_PATTERN_HORIZONTALBARS:
        mode = test_pattern == RD_CONTROLLER_DP_TEST_PATTERN_VERTICALBARS ? RD_TEST_PATTERN_MODE_VERTICALBARS
                                                                           : RD_TEST_PATTERN_MODE_HORIZONTALBARS;
        dst_bpc = bit_depth == TP_BPC_6 ? 6 : bit_depth == TP_BPC_10 ? 10 : 8;
        /* to the color format, then 16-bit MSB aligned */
        for (index = 0; index < 6; index++) {
            dst_color[index] = (uint16_t)(src_color[index] >> (src_bpc - dst_bpc));
            dst_color[index] = (uint16_t)(dst_color[index] << (16 - dst_bpc));
        }
        dpg_colours(d, opp, dst_color[0], dst_color[3], dst_color[1], dst_color[4], dst_color[2], dst_color[5]);
        upd(d, ctl, DPGM(DPG_EN) | DPGM(DPG_MODE) | DPGM(DPG_DYNAMIC_RANGE) | DPGM(DPG_BIT_DEPTH) | DPGM(DPG_VRES) | DPGM(DPG_HRES),
            DPGC(DPG_EN, 1) | DPGC(DPG_MODE, mode) | DPGC(DPG_BIT_DEPTH, bit_depth));
        break;
    case RD_CONTROLLER_DP_TEST_PATTERN_COLORRAMP:
        mode = bit_depth == TP_BPC_10 ? RD_TEST_PATTERN_MODE_DUALRAMP_RGB : RD_TEST_PATTERN_MODE_SINGLERAMP_RGB;
        dst_bpc = bit_depth == TP_BPC_6 ? 6 : bit_depth == TP_BPC_10 ? 10 : 8;
        /* one gradation of the first ramp in 16-bit color */
        inc_base = src_bpc - dst_bpc;
        switch (bit_depth) {
        case TP_BPC_6:
            rdna3_wreg(d, DA(d, regDPG0_DPG_RAMP_CONTROL, opp), FLD(DPG0_DPG_RAMP_CONTROL, DPG_INC0, inc_base));
            upd(d, ctl, DPGM(DPG_VRES) | DPGM(DPG_HRES), DPGC(DPG_VRES, 6) | DPGC(DPG_HRES, 6));
            break;
        case TP_BPC_8:
            rdna3_wreg(d, DA(d, regDPG0_DPG_RAMP_CONTROL, opp), FLD(DPG0_DPG_RAMP_CONTROL, DPG_INC0, inc_base));
            upd(d, ctl, DPGM(DPG_VRES) | DPGM(DPG_HRES), DPGC(DPG_VRES, 6) | DPGC(DPG_HRES, 8));
            break;
        case TP_BPC_10:
            rdna3_wreg(d, DA(d, regDPG0_DPG_RAMP_CONTROL, opp),
                       FLD(DPG0_DPG_RAMP_CONTROL, DPG_RAMP0_OFFSET, 384u << 6) | FLD(DPG0_DPG_RAMP_CONTROL, DPG_INC0, inc_base) |
                           FLD(DPG0_DPG_RAMP_CONTROL, DPG_INC1, inc_base + 2));
            upd(d, ctl, DPGM(DPG_VRES) | DPGM(DPG_HRES), DPGC(DPG_VRES, 5) | DPGC(DPG_HRES, 8));
            break;
        default:
            break;
        }
        upd(d, ctl, DPGM(DPG_EN) | DPGM(DPG_MODE) | DPGM(DPG_DYNAMIC_RANGE) | DPGM(DPG_BIT_DEPTH),
            DPGC(DPG_EN, 1) | DPGC(DPG_MODE, mode) | DPGC(DPG_BIT_DEPTH, bit_depth));
        break;
    case RD_CONTROLLER_DP_TEST_PATTERN_VIDEOMODE:
        rdna3_wreg(d, ctl, 0);
        rdna3_wreg(d, DA(d, regDPG0_DPG_COLOUR_R_CR, opp), 0);
        rdna3_wreg(d, DA(d, regDPG0_DPG_COLOUR_G_Y, opp), 0);
        rdna3_wreg(d, DA(d, regDPG0_DPG_COLOUR_B_CB, opp), 0);
        rdna3_wreg(d, DA(d, regDPG0_DPG_RAMP_CONTROL, opp), 0);
        break;
    case RD_CONTROLLER_DP_TEST_PATTERN_SOLID_COLOR:
        dcn32_opp_dpg_set_blank_color(d, opp, solid_color);
        upd(d, ctl, DPGM(DPG_EN) | DPGM(DPG_MODE), DPGC(DPG_EN, 1) | DPGC(DPG_MODE, RD_TEST_PATTERN_MODE_HORIZONTALBARS));
        dcn32_opp_program_dpg_dimensions(d, opp, width, height);
        break;
    default:
        break;
    }
}

/* opp2_dpg_is_blanked: on, no double-buffered update pending */
int dcn32_opp_dpg_is_blanked(rdna3_dev *d, uint32_t opp)
{
    uint32_t en = GETF(rdna3_rreg(d, DA(d, regDPG0_DPG_CONTROL, opp)), DPG0_DPG_CONTROL, DPG_EN);
    uint32_t pend = GETF(rdna3_rreg(d, DA(d, regDPG0_DPG_STATUS, opp)), DPG0_DPG_STATUS, DPG_DOUBLE_BUFFER_PENDING);
    return en == 1 && pend == 0;
}

/* opp2_dpg_is_pending */
int dcn32_opp_dpg_is_pending(rdna3_dev *d, uint32_t opp)
{
    uint32_t en = GETF(rdna3_rreg(d, DA(d, regDPG0_DPG_CONTROL, opp)), DPG0_DPG_CONTROL, DPG_EN);
    uint32_t pend = GETF(rdna3_rreg(d, DA(d, regDPG0_DPG_STATUS, opp)), DPG0_DPG_STATUS, DPG_DOUBLE_BUFFER_PENDING);
    return en == 1 && pend == 1;
}

/* ==================== dc_hw_sequencer.c / dcn20_hwseq.c ==================== */

/* color_space_to_black_color (black_color_format[]: RGB full 0, RGB limited
 * 0x40, YUV_CV 0x1f4 / 0x40 / 0x1f4) */
void dcn32_color_space_to_black_color(uint32_t color_space, rdna3_tg_color *black)
{
    switch (color_space) {
    case RD_CS_YCBCR601:
    case RD_CS_YCBCR709:
    case RD_CS_YCBCR601_LIMITED:
    case RD_CS_YCBCR709_LIMITED:
    case RD_CS_2020_YCBCR:
        black->color_r_cr = 0x1f4;
        black->color_g_y = 0x40;
        black->color_b_cb = 0x1f4;
        break;
    case RD_CS_SRGB_LIMITED:
        black->color_r_cr = black->color_g_y = black->color_b_cb = 0x40;
        break;
    default:   /* sRGB black (full range) */
        black->color_r_cr = black->color_g_y = black->color_b_cb = 0;
        break;
    }
}

/* dcn20_blank_pixel_data (one OPP, no visual confirm, no ABM) */
void dcn32_blank_pixel_data(rdna3_dev *d, uint32_t opp, int blank, uint32_t color_space, uint32_t color_depth,
                            uint32_t width, uint32_t height)
{
    rdna3_tg_color black;
    dcn32_color_space_to_black_color(color_space, &black);
    dcn32_opp_set_disp_pattern_generator(d, opp,
                                         blank ? RD_CONTROLLER_DP_TEST_PATTERN_SOLID_COLOR
                                               : RD_CONTROLLER_DP_TEST_PATTERN_VIDEOMODE,
                                         RD_CONTROLLER_DP_COLOR_SPACE_UDEFINED, color_depth, &black, width, height, 0);
}

/* dcn20_wait_for_blank_complete: 1000 x 100 us for the update, then blanked? */
int dcn32_wait_for_blank_complete(rdna3_dev *d, uint32_t opp)
{
    int counter;
    for (counter = 0; counter < 1000; counter++) {
        if (!dcn32_opp_dpg_is_pending(d, opp))
            break;
        d->os.udelay(d->os.ctx, 100);
    }
    if (counter == 1000) {
        RERR(d, "opp %u: failed to blank crtc", opp);
        return 0;
    }
    return dcn32_opp_dpg_is_blanked(d, opp);
}

/* get_color_space_type */
uint32_t dcn32_color_space_type(uint32_t cs)
{
    switch (cs) {
    case RD_CS_SRGB:
    case RD_CS_2020_RGB_FULLRANGE:
    case RD_CS_ADOBERGB:
        return RD_COLOR_SPACE_RGB_TYPE;
    case RD_CS_SRGB_LIMITED:
    case RD_CS_2020_RGB_LIMITEDRANGE:
        return RD_COLOR_SPACE_RGB_LIMITED_TYPE;
    case RD_CS_YCBCR601:
        return RD_COLOR_SPACE_YCBCR601_TYPE;
    case RD_CS_YCBCR709:
        return RD_COLOR_SPACE_YCBCR709_TYPE;
    case RD_CS_YCBCR601_LIMITED:
        return RD_COLOR_SPACE_YCBCR601_LIMITED_TYPE;
    case RD_CS_YCBCR709_LIMITED:
        return RD_COLOR_SPACE_YCBCR709_LIMITED_TYPE;
    case RD_CS_2020_YCBCR:
        return RD_COLOR_SPACE_YCBCR2020_TYPE;
    default:
        return RD_COLOR_SPACE_RGB_TYPE;
    }
}

/* dcn20_program_output_csc (csc_color_matrix.enable_adjustment off:
 * set_ocsc_default) and the depth the OPP gets (mpc3_set_denorm) */
void dcn32_program_output_csc(rdna3_dev *d, uint32_t opp, uint32_t color_space, uint32_t color_depth)
{
    dcn32_mpc_set_ocsc_default(d, opp, dcn32_color_space_type(color_space), RD_MPC_OUTPUT_CSC_COEF_A);
    dcn32_mpc_set_denorm(d, opp, color_depth);
}

/* ==================== dce_clock_source.c ==================== */

static const uint32_t pixclk_resync_reg[5] = {
    regPHYPLLA_PIXCLK_RESYNC_CNTL, regPHYPLLB_PIXCLK_RESYNC_CNTL, regPHYPLLA_PIXCLK_RESYNC_CNTL + 2,
    regPHYPLLA_PIXCLK_RESYNC_CNTL + 3, regPHYPLLE_PIXCLK_RESYNC_CNTL,
};

/* dce112_program_pixel_clk_resync: 24 bit 1:1, 30 bit 5:4, 36 bit 3:2,
 * 48 bit 2:1 (no PHYPLLA_PIXCLK_DOUBLE_RATE_ENABLE field on DCN 3.2) */
void dcn32_program_pixel_clk_resync(rdna3_dev *d, uint32_t pll, uint32_t signal, uint32_t color_depth, int ycbcr420)
{
    uint32_t deep_color_cntl = 0, a;
    (void)ycbcr420;
    if (pll >= 5)
        return;
    if (signal == RD_SIGNAL_HDMI_TYPE_A) {
        switch (color_depth) {
        case RD_COLOR_DEPTH_101010: deep_color_cntl = 1; break;
        case RD_COLOR_DEPTH_121212: deep_color_cntl = 2; break;
        case RD_COLOR_DEPTH_161616: deep_color_cntl = 3; break;
        default: deep_color_cntl = 0; break;
        }
    }
    a = rdna3_reg_offset(d, HWIP_DCE, 0, regPHYPLLA_PIXCLK_RESYNC_CNTL_BASE_IDX, pixclk_resync_reg[pll]);
    upd(d, a, MSK(PHYPLLA_PIXCLK_RESYNC_CNTL, PHYPLLA_DCCG_DEEP_COLOR_CNTL),
        FLD(PHYPLLA_PIXCLK_RESYNC_CNTL, PHYPLLA_DCCG_DEEP_COLOR_CNTL, deep_color_cntl));
}

/* dcn3_get_pix_clk_dividers, HDMI */
uint32_t dcn32_hdmi_pll_clk_khz(uint32_t pixel_khz, uint32_t color_depth)
{
    uint64_t c = pixel_khz;
    switch (color_depth) {
    case RD_COLOR_DEPTH_101010: c = (c * 5) >> 2; break;
    case RD_COLOR_DEPTH_121212: c = (c * 6) >> 2; break;
    case RD_COLOR_DEPTH_161616: c = c * 2; break;
    default: break;
    }
    return (uint32_t)c;
}

uint32_t dcn32_pix_clk_color_depth(const rdna3_crtc_format *f)
{
    return f->pixel_encoding == RD_PIXEL_ENCODING_YCBCR422 ? RD_COLOR_DEPTH_888 : f->color_depth;
}

/* ==================== amdgpu_dm.c ==================== */

void dcn32_sink_color_caps(const rdna3_edid *e, rdna3_sink_color_caps *c)
{
    c->color_formats = e ? e->color_formats : 0;
    c->bpc = e ? e->bpc : 0;
    c->max_tmds_clock_khz = e ? e->max_tmds_clk_mhz * 1000 : 0;
}

/* convert_color_depth_from_display_info (not 4:2:0) */
uint32_t dcn32_convert_color_depth(uint32_t edid_bpc, uint32_t requested_bpc)
{
    uint32_t bpc = edid_bpc ? edid_bpc : 8;   /* 8 bpc by default */
    if (requested_bpc > 0) {
        /* capped at the requested value, rounded down to even */
        if (requested_bpc < bpc)
            bpc = requested_bpc;
        bpc = bpc - (bpc & 1);
    }
    switch (bpc) {
    case 0: return RD_COLOR_DEPTH_888;   /* "Temporary Work around" for 0 */
    case 6: return RD_COLOR_DEPTH_666;
    case 8: return RD_COLOR_DEPTH_888;
    case 10: return RD_COLOR_DEPTH_101010;
    case 12: return RD_COLOR_DEPTH_121212;
    case 14: return RD_COLOR_DEPTH_141414;
    case 16: return RD_COLOR_DEPTH_161616;
    default: return RD_COLOR_DEPTH_UNDEFINED;
    }
}

/* adjust_colour_depth_from_display_info */
int dcn32_adjust_colour_depth(rdna3_crtc_format *f, uint32_t pixel_khz, uint32_t max_tmds_clock_khz)
{
    uint32_t depth = f->color_depth, clk;
    do {
        clk = pixel_khz;
        if (f->pixel_encoding == RD_PIXEL_ENCODING_YCBCR420)
            clk /= 2;
        /* the HDMI TMDS clock of the depth */
        switch (depth) {
        case RD_COLOR_DEPTH_888: break;
        case RD_COLOR_DEPTH_101010: clk = clk * 30 / 24; break;
        case RD_COLOR_DEPTH_121212: clk = clk * 36 / 24; break;
        case RD_COLOR_DEPTH_161616: clk = clk * 48 / 24; break;
        default: return 0;   /* the only depths valid for HDMI */
        }
        if (clk <= max_tmds_clock_khz) {
            f->color_depth = depth;
            return 1;
        }
    } while (--depth > RD_COLOR_DEPTH_666);
    return 0;
}

/* get_output_color_space, DRM_MODE_COLORIMETRY_DEFAULT (no Y-only):
 * 27.03 MHz separates SDTV (BT.601) from HDTV (BT.709) */
uint32_t dcn32_output_color_space(const rdna3_crtc_format *f, uint32_t pixel_khz)
{
    if (f->pixel_encoding == RD_PIXEL_ENCODING_RGB)
        return RD_CS_SRGB;
    return pixel_khz * 10 > 270300 ? RD_CS_YCBCR709 : RD_CS_YCBCR601;
}

/* fill_stream_properties_from_drm_display_mode: the pixel encoding and the
 * depth for 'signal' */
uint32_t dcn32_output_format(const rdna3_sink_color_caps *caps, const rdna3_mode *m, uint32_t signal,
                             uint32_t requested_bpc, int force_yuv422, rdna3_crtc_format *f)
{
    f->dsc = 0;
    f->dsc_ycbcr422_simple = 0;
    if ((caps->color_formats & RD_COLOR_FORMAT_YCBCR422) && force_yuv422)
        f->pixel_encoding = RD_PIXEL_ENCODING_YCBCR422;
    else if ((caps->color_formats & RD_COLOR_FORMAT_YCBCR444) && signal == RD_SIGNAL_HDMI_TYPE_A)
        f->pixel_encoding = RD_PIXEL_ENCODING_YCBCR444;
    else
        f->pixel_encoding = RD_PIXEL_ENCODING_RGB;
    f->color_depth = dcn32_convert_color_depth(caps->bpc, requested_bpc);
    /* HDMI: the deepest color the sink's TMDS clock carries (no 4:2:0 fallback here) */
    if (signal == RD_SIGNAL_HDMI_TYPE_A)
        dcn32_adjust_colour_depth(f, m->pixel_khz, caps->max_tmds_clock_khz);
    return dcn32_output_color_space(f, m->pixel_khz);
}
