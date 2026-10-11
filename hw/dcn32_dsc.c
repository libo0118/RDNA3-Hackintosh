/*
 * dcn32_dsc.c - see dcn32_dsc.h. Display Stream Compression for DCN 3.2,
 * ported from amdgpu Display Core (Linux v6.12, MIT) and the DRM DSC
 * helper (MIT); every function names the one it ports.
 *
 * DC computes the rate control parameters in floating point
 * (rc_calc_fpu.c under DC_FP_START). The kext builds without floating
 * point, so _do_calc_rc_params here works on exact rationals: the target
 * bpp is a multiple of 1/16 (1/32 for native 4:2:x), every comparison is
 * exact, and dsc_roundf (round half away from zero) is evaluated on the
 * exact value. For every bpp DSC allows (6.0 .. 63.9375) this gives the
 * values the double arithmetic of DC gives (checked for the get_ofs_set
 * interpolations, the only ones with inexact constants such as 8 / 3.0).
 * DC's fixed point (fixed31_32) is fixpt31_32.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_dsc.h"
#include "fixpt31_32.h"
#include "gen/dsc_qp_tables.h"
#include "gen/dcn_3_2_0_dsc.h"
#include "gen/dcn_3_2_0_dp.h"
#include "gen/dcn_3_2_0_pipe.h"
#include "gen/dcn_3_2_1.h"
#include "gen/dcn_3_2_0_hpo.h"

#define GET(v, reg, field) (((v) & (uint32_t)reg##__##field##_MASK) >> reg##__##field##__SHIFT)
#define FIELD(reg, field, x) (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK)
#define SET(v, reg, field, x) (((v) & ~(uint32_t)reg##__##field##_MASK) | FIELD(reg, field, x))

#define RI(d, reg, n, stride) rd((d), reg##_BASE_IDX, (uint32_t)(reg) + (uint32_t)(stride) * (n))
#define WI(d, reg, n, stride, v) wr((d), reg##_BASE_IDX, (uint32_t)(reg) + (uint32_t)(stride) * (n), (v))
#define AI(d, reg, n, stride) \
    rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, (uint32_t)(reg) + (uint32_t)(stride) * (n))
#define UPDATE(d, reg, n, stride, regf, field, x) \
    upd((d), reg##_BASE_IDX, (uint32_t)(reg) + (uint32_t)(stride) * (n), \
        (uint32_t)regf##__##field##_MASK, regf##__##field##__SHIFT, (uint32_t)(x))

#define DSC_S   (regDSC_TOP1_DSC_TOP_CONTROL - regDSC_TOP0_DSC_TOP_CONTROL)              /* 0x5c */
#define DSCRM_S (regDSCRM1_DSCRM_DSC_FORWARD_CONFIG - regDSCRM0_DSCRM_DSC_FORWARD_CONFIG)/* 1 */
#define PG_S    (regDOMAIN17_PG_CONFIG - regDOMAIN16_PG_CONFIG)                          /* 2 */
#define ODM_S   (regODM1_OPTC_DATA_FORMAT_CONTROL - regODM0_OPTC_DATA_FORMAT_CONTROL)    /* 0x10 */
#define OTG_S   0x80u
#define DP_S    (regDP1_DP_LINK_CNTL - regDP0_DP_LINK_CNTL)                              /* 0x100 */
#define AFMT_S  (regDIG1_AFMT_CNTL - regDIG0_AFMT_CNTL)                                  /* 0x100 */
#define VPG_S   (regVPG1_VPG_GENERIC_PACKET_ACCESS_CTRL - regVPG0_VPG_GENERIC_PACKET_ACCESS_CTRL)

static uint32_t rd(rdna3_dev *d, uint32_t seg, uint32_t off)
{
    return rdna3_rreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, (int)seg, off));
}

static void wr(rdna3_dev *d, uint32_t seg, uint32_t off, uint32_t v)
{
    rdna3_wreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, (int)seg, off), v);
}

static void upd(rdna3_dev *d, uint32_t seg, uint32_t off, uint32_t mask, uint32_t shift, uint32_t x)
{
    uint32_t v = rd(d, seg, off);
    wr(d, seg, off, (v & ~mask) | ((x << shift) & mask));
}

static void zero(void *p, uint32_t n)
{
    uint8_t *b = (uint8_t *)p;
    while (n--)
        *b++ = 0;
}

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* ==================================================================== */
/* policy: dc_dsc.c                                                     */
/* ==================================================================== */

/* default DSC policy target bitrate limit is 16bpp */
static const uint32_t dsc_policy_max_target_bpp_limit = 16;
/* default DSC policy enables DSC only when needed */
static const int dsc_policy_enable_dsc_when_not_needed = 0;
static const int dsc_policy_disable_dsc_stream_overhead = 0;

/* dsc_buff_block_size_from_dpcd */
static int dsc_buff_block_size_from_dpcd(int dpcd_buff_block_size, int *buff_block_size)
{
    switch (dpcd_buff_block_size) {
    case 0: *buff_block_size = 1024; break;        /* DP_DSC_RC_BUF_BLK_SIZE_1 */
    case 1: *buff_block_size = 4 * 1024; break;
    case 2: *buff_block_size = 16 * 1024; break;
    case 3: *buff_block_size = 64 * 1024; break;
    default: return 0;
    }
    return 1;
}

/* dsc_line_buff_depth_from_dpcd */
static int dsc_line_buff_depth_from_dpcd(int dpcd_line_buff_bit_depth, int32_t *line_buff_bit_depth)
{
    if (0 <= dpcd_line_buff_bit_depth && dpcd_line_buff_bit_depth <= 7)
        *line_buff_bit_depth = dpcd_line_buff_bit_depth + 9;
    else if (dpcd_line_buff_bit_depth == 8)
        *line_buff_bit_depth = 8;
    else
        return 0;
    return 1;
}

/* dsc_throughput_from_dpcd */
static int dsc_throughput_from_dpcd(int dpcd_throughput, int32_t *throughput)
{
    static const int32_t t[16] = { 0, 340, 400, 450, 500, 550, 600, 650, 700, 750, 800, 850, 900, 950, 1000, 170 };
    if (dpcd_throughput < 0 || dpcd_throughput > 15)
        return 0;
    *throughput = t[dpcd_throughput];
    return 1;
}

/* dsc_bpp_increment_div_from_dpcd */
static int dsc_bpp_increment_div_from_dpcd(uint8_t bpp_increment_dpcd, uint32_t *bpp_increment_div)
{
    switch (bpp_increment_dpcd & 0x7) {
    case 0: *bpp_increment_div = 16; break;
    case 1: *bpp_increment_div = 8; break;
    case 2: *bpp_increment_div = 4; break;
    case 3: *bpp_increment_div = 2; break;
    case 4: *bpp_increment_div = 1; break;
    default: return 0;
    }
    return 1;
}

/* dc_dsc_parse_dsc_dpcd (no dc->debug.dsc_bpp_increment_div override) */
int dcn32_dsc_parse_dsc_dpcd(const uint8_t *b, const uint8_t *br, rdna3_dsc_dec_caps *c)
{
    int buff_block_size, dpcd_throughput;
    uint32_t v;

    zero(c, sizeof(*c));
    if (!b)
        return 0;
    c->is_dsc_supported = (b[0x0] & RD_DP_DSC_DECOMPRESSION_IS_SUPPORTED) != 0;   /* DP_DSC_SUPPORT */
    if (!c->is_dsc_supported)
        return 0;
    c->dsc_version = b[0x1];                                                          /* DP_DSC_REV */
    if (!dsc_buff_block_size_from_dpcd(b[0x2] & 0x03, &buff_block_size))            /* DP_DSC_RC_BUF_BLK_SIZE */
        return 0;
    c->rc_buffer_size = (b[0x3] + 1) * buff_block_size;                              /* DP_DSC_RC_BUF_SIZE */
    c->slice_caps1 = b[0x4];                                                          /* DP_DSC_SLICE_CAP_1 */
    if (!dsc_line_buff_depth_from_dpcd(b[0x5], &c->lb_bit_depth))                   /* DP_DSC_LINE_BUF_BIT_DEPTH */
        return 0;
    c->is_block_pred_supported = (b[0x6] & 1) != 0;                                  /* DP_DSC_BLK_PREDICTION_SUPPORT */
    c->edp_max_bits_per_pixel = b[0x7] | b[0x8] << 8;                                /* DP_DSC_MAX_BITS_PER_PIXEL_LOW/HI */
    c->color_formats = b[0x9];                                                        /* DP_DSC_DEC_COLOR_FORMAT_CAP */
    c->color_depth = b[0xa];                                                          /* DP_DSC_DEC_COLOR_DEPTH_CAP */
    dpcd_throughput = b[0xb];                                                         /* DP_DSC_PEAK_THROUGHPUT */
    if (!dsc_throughput_from_dpcd(dpcd_throughput & 0xf, &c->throughput_mode_0_mps))
        return 0;
    c->throughput_mode_0_mps += (b[0x2] >> 3) * 2;                                    /* granular delta */
    if (!dsc_throughput_from_dpcd((dpcd_throughput & 0xf0) >> 4, &c->throughput_mode_1_mps))
        return 0;
    c->max_slice_width = b[0xc] * 320;                                                /* DP_DSC_MAX_SLICE_WIDTH */
    c->slice_caps2 = b[0xd];                                                          /* DP_DSC_SLICE_CAP_2 */
    if (!dsc_bpp_increment_div_from_dpcd(b[0xf], &c->bpp_increment_div))             /* DP_DSC_BITS_PER_PIXEL_INC */
        return 0;
    /* extended caps: NULL for a device that is not a branch */
    if (!br) {
        c->branch_overall_throughput_0_mps = 0;
        c->branch_overall_throughput_1_mps = 0;
        c->branch_max_line_width = 0;
        return 1;
    }
    v = br[0];
    c->branch_overall_throughput_0_mps = v == 0 ? 0 : v == 1 ? 680 : v * 50 + 600;
    v = br[1];
    c->branch_overall_throughput_1_mps = v == 0 ? 0 : v == 1 ? 680 : v * 50 + 600;
    c->branch_max_line_width = br[2] * 320u;
    c->is_dp = 1;
    return 1;
}

/* dc_dsc_get_default_config_option (no debug overrides) */
void dcn32_dsc_default_options(rdna3_dsc_options *o)
{
    zero(o, sizeof(*o));
    o->dsc_min_slice_height_override = 0;
    o->dsc_force_odm_hslice_override = 0;     /* dc->debug.force_odm_combine */
    o->max_target_bpp_limit_override_x16 = 0;
    o->slice_height_granularity = 1;
    o->force_dsc_when_not_needed = 0;
}

/* dsc2_get_enc_caps (DCN 3.2 creates a dcn20 DSC: dcn32_dsc_create) */
#define DCN20_MAX_PIXEL_CLOCK_Mhz   1188
#define DCN20_MAX_DISPLAY_CLOCK_Mhz 1200
void dcn32_dsc_get_enc_caps(rdna3_dsc_enc_caps *e, uint32_t pixel_clock_100hz)
{
    e->dsc_version = 0x21;   /* v1.2, in the DP spec's reverse order */
    e->slice_caps = RD_DSC_ENC_SLICES_1 | RD_DSC_ENC_SLICES_2 | RD_DSC_ENC_SLICES_3 | RD_DSC_ENC_SLICES_4;
    e->lb_bit_depth = 13;
    e->is_block_pred_supported = 1;
    e->color_formats = RD_DSC_FMT_RGB | RD_DSC_FMT_YCBCR_444 | RD_DSC_FMT_SIMPLE_422 | RD_DSC_FMT_NATIVE_420;
    e->color_depth = RD_DSC_DEPTH_8_BPC | RD_DSC_DEPTH_10_BPC | RD_DSC_DEPTH_12_BPC;
    e->max_total_throughput_mps = DCN20_MAX_DISPLAY_CLOCK_Mhz;
    /* above one pipe's pixel rate: two engines, at least two slices */
    if (pixel_clock_100hz >= DCN20_MAX_PIXEL_CLOCK_Mhz * 10000u) {
        e->slice_caps = (uint8_t)((e->slice_caps & ~RD_DSC_ENC_SLICES_1) | RD_DSC_ENC_SLICES_8);
        e->max_total_throughput_mps = DCN20_MAX_DISPLAY_CLOCK_Mhz * 2;
    }
    /* four engines (ODM 4:1) */
    if (pixel_clock_100hz > DCN20_MAX_PIXEL_CLOCK_Mhz * 10000u * 2) {
        e->slice_caps = (uint8_t)(e->slice_caps | RD_DSC_ENC_SLICES_12 | RD_DSC_ENC_SLICES_16);
        e->max_total_throughput_mps = DCN20_MAX_DISPLAY_CLOCK_Mhz * 4;
    }
    e->max_slice_width = 5184;
    e->bpp_increment_div = 16;
}

/* get_dsc_enc_caps (dc->debug.disable_dsc / native422_support off) */
static void get_dsc_enc_caps(rdna3_dsc_enc_caps *e, uint32_t pixel_clock_100hz)
{
    zero(e, sizeof(*e));
    dcn32_dsc_get_enc_caps(e, pixel_clock_100hz);
}

/* intersect_dsc_caps */
static int intersect_dsc_caps(const rdna3_dsc_dec_caps *s, const rdna3_dsc_enc_caps *e, uint32_t pixel_encoding,
                              rdna3_dsc_enc_caps *c)
{
    int32_t max_slices, total_sink_throughput;
    zero(c, sizeof(*c));
    c->dsc_version = MIN(s->dsc_version, e->dsc_version);
    if (!c->dsc_version)
        return 0;
    if ((s->slice_caps1 & RD_DSC_SINK_SLICES_1) && (e->slice_caps & RD_DSC_ENC_SLICES_1))
        c->slice_caps |= RD_DSC_ENC_SLICES_1;
    if ((s->slice_caps1 & RD_DSC_SINK_SLICES_2) && (e->slice_caps & RD_DSC_ENC_SLICES_2))
        c->slice_caps |= RD_DSC_ENC_SLICES_2;
    if ((s->slice_caps1 & RD_DSC_SINK_SLICES_4) && (e->slice_caps & RD_DSC_ENC_SLICES_4))
        c->slice_caps |= RD_DSC_ENC_SLICES_4;
    if ((s->slice_caps1 & RD_DSC_SINK_SLICES_8) && (e->slice_caps & RD_DSC_ENC_SLICES_8))
        c->slice_caps |= RD_DSC_ENC_SLICES_8;
    if ((s->slice_caps1 & RD_DSC_SINK_SLICES_12) && (e->slice_caps & RD_DSC_ENC_SLICES_12))
        c->slice_caps |= RD_DSC_ENC_SLICES_12;
    if ((s->slice_caps2 & RD_DSC_SINK_SLICES_16) && (e->slice_caps & RD_DSC_ENC_SLICES_16))
        c->slice_caps |= RD_DSC_ENC_SLICES_16;
    if (!c->slice_caps)
        return 0;
    c->lb_bit_depth = MIN(s->lb_bit_depth, e->lb_bit_depth);
    if (!c->lb_bit_depth)
        return 0;
    c->is_block_pred_supported = s->is_block_pred_supported && e->is_block_pred_supported;
    c->color_formats = (uint8_t)(s->color_formats & e->color_formats);
    if (!c->color_formats)
        return 0;
    c->color_depth = (uint8_t)(s->color_depth & e->color_depth);
    if (!c->color_depth)
        return 0;
    max_slices = 0;
    if (c->slice_caps & RD_DSC_ENC_SLICES_1)
        max_slices = 1;
    if (c->slice_caps & RD_DSC_ENC_SLICES_2)
        max_slices = 2;
    if (c->slice_caps & RD_DSC_ENC_SLICES_4)
        max_slices = 4;
    total_sink_throughput = max_slices * s->throughput_mode_0_mps;
    if (pixel_encoding == RD_PIXEL_ENCODING_YCBCR422 || pixel_encoding == RD_PIXEL_ENCODING_YCBCR420)
        total_sink_throughput = max_slices * s->throughput_mode_1_mps;
    c->max_total_throughput_mps = MIN(total_sink_throughput, e->max_total_throughput_mps);
    c->max_slice_width = MIN(s->max_slice_width, e->max_slice_width);
    if (!c->max_slice_width)
        return 0;
    c->bpp_increment_div = MIN(s->bpp_increment_div, e->bpp_increment_div);
    if (pixel_encoding == RD_PIXEL_ENCODING_YCBCR422 || pixel_encoding == RD_PIXEL_ENCODING_YCBCR420)
        c->bpp_increment_div = MIN(c->bpp_increment_div, 8u);
    c->edp_sink_max_bits_per_pixel = (uint32_t)s->edp_max_bits_per_pixel;
    c->is_dp = s->is_dp;
    return 1;
}

/* dsc_div_by_10_round_up */
static uint32_t dsc_div_by_10_round_up(uint32_t value)
{
    return (value + 9) / 10;
}

/* dc_dsc_stream_bandwidth_overhead_in_kbps */
uint32_t dcn32_dsc_overhead_kbps(const rdna3_dsc_timing *t, uint32_t num_slices_h, int is_dp)
{
    rd_fixpt max_dsc_overhead, refresh_rate;
    if (dsc_policy_disable_dsc_stream_overhead || !is_dp || !t->h_total || !t->v_total)
        return 0;
    refresh_rate = dc_fixpt_from_int(t->pix_clk_100hz);
    refresh_rate = dc_fixpt_div_int(refresh_rate, t->h_total);
    refresh_rate = dc_fixpt_div_int(refresh_rate, t->v_total);
    refresh_rate = dc_fixpt_mul_int(refresh_rate, 100);
    max_dsc_overhead = dc_fixpt_from_int(num_slices_h);
    max_dsc_overhead = dc_fixpt_mul_int(max_dsc_overhead, t->v_total);
    max_dsc_overhead = dc_fixpt_mul_int(max_dsc_overhead, 256);
    max_dsc_overhead = dc_fixpt_div_int(max_dsc_overhead, 1000);
    max_dsc_overhead = dc_fixpt_mul(max_dsc_overhead, refresh_rate);
    return (uint32_t)dc_fixpt_ceil(max_dsc_overhead);
}

/* dc_dsc_stream_bandwidth_in_kbps */
uint32_t dcn32_dsc_stream_kbps(const rdna3_dsc_timing *t, uint32_t bpp_x16, uint32_t num_slices_h, int is_dp)
{
    uint32_t overhead_in_kbps = dcn32_dsc_overhead_kbps(t, num_slices_h, is_dp);
    rd_fixpt bpp = dc_fixpt_from_fraction(bpp_x16, 16);
    rd_fixpt bw = dc_fixpt_from_fraction(t->pix_clk_100hz, 10);
    bw = dc_fixpt_mul(bw, bpp);
    bw = dc_fixpt_add_int(bw, overhead_in_kbps);
    return (uint32_t)dc_fixpt_ceil(bw);
}

/* apply_128b_132b_stream_overhead: an uncompressed 128b/132b stream pads
 * each line to whole 4-lane 32-bit symbols:
 * Symbols_per_HActive = HActive x bpp / (4 lanes x 32 bit), overhead =
 * ceil(Symbols_per_HActive) / Symbols_per_HActive (disable_128b_132b_stream_overhead
 * is false) */
static uint32_t apply_128b_132b_stream_overhead(const rdna3_dsc_timing *t, uint32_t kbps)
{
    uint32_t total_kbps = kbps;
    if (!t->dsc) {
        rd_fixpt bpp, overhead_factor;
        bpp = dc_fixpt_from_int(kbps);
        bpp = dc_fixpt_div_int(bpp, t->pix_clk_100hz / 10);
        overhead_factor = dc_fixpt_from_int(t->h_addressable);
        overhead_factor = dc_fixpt_mul(overhead_factor, bpp);
        overhead_factor = dc_fixpt_div_int(overhead_factor, 128);
        overhead_factor = dc_fixpt_div(dc_fixpt_from_int(dc_fixpt_ceil(overhead_factor)), overhead_factor);
        total_kbps = (uint32_t)dc_fixpt_ceil(dc_fixpt_mul_int(overhead_factor, total_kbps));
    }
    return total_kbps;
}

/* dc_bandwidth_in_kbps_from_timing (no HDMI FRL: the FRL bandwidth of a
 * VIC-less timing is not used by this port) */
uint32_t dcn32_dsc_timing_kbps(const rdna3_dsc_timing *t)
{
    uint32_t bits_per_channel, kbps;
    if (t->dsc)
        return dcn32_dsc_stream_kbps(t, t->dsc_cfg.bits_per_pixel, t->dsc_cfg.num_slices_h, t->dsc_cfg.is_dp);
    switch (t->bpc) {
    case 6: case 8: case 10: case 12: case 14: case 16: bits_per_channel = t->bpc; break;
    default: bits_per_channel = 8; break;
    }
    kbps = t->pix_clk_100hz / 10;
    kbps *= bits_per_channel;
    if (!t->y_only) {
        /* only Y-only reduces the bandwidth by 1/3 compared to RGB */
        kbps *= 3;
        if (t->pixel_encoding == RD_PIXEL_ENCODING_YCBCR420)
            kbps /= 2;
        else if (t->pixel_encoding == RD_PIXEL_ENCODING_YCBCR422)
            kbps = kbps * 2 / 3;
    }
    if (t->link_encoding == RD_DC_LINK_ENCODING_DP_128b_132b)
        kbps = apply_128b_132b_stream_overhead(t, kbps);
    return kbps;
}

/* compute_bpp_x16_from_target_bandwidth */
static uint32_t compute_bpp_x16_from_target_bandwidth(uint32_t bandwidth_in_kbps, const rdna3_dsc_timing *t,
                                                      uint32_t num_slices_h, uint32_t bpp_increment_div, int is_dp)
{
    uint32_t overhead_in_kbps = dcn32_dsc_overhead_kbps(t, num_slices_h, is_dp);
    rd_fixpt eff = dc_fixpt_from_int(bandwidth_in_kbps);
    rd_fixpt bpp_x16;
    eff = dc_fixpt_sub_int(eff, overhead_in_kbps);
    bpp_x16 = dc_fixpt_mul_int(eff, 10);
    bpp_x16 = dc_fixpt_div_int(bpp_x16, t->pix_clk_100hz);
    bpp_x16 = dc_fixpt_from_int(dc_fixpt_floor(dc_fixpt_mul_int(bpp_x16, bpp_increment_div)));
    bpp_x16 = dc_fixpt_div_int(bpp_x16, bpp_increment_div);
    bpp_x16 = dc_fixpt_mul_int(bpp_x16, 16);
    return (uint32_t)dc_fixpt_floor(bpp_x16);
}

/* decide_dsc_bandwidth_range */
static int decide_dsc_bandwidth_range(uint32_t min_bpp_x16, uint32_t max_bpp_x16, uint32_t num_slices_h,
                                      const rdna3_dsc_enc_caps *c, const rdna3_dsc_timing *t,
                                      rdna3_dsc_bw_range *range)
{
    uint32_t preferred_bpp_x16 = t->dsc_fixed_bits_per_pixel_x16;
    zero(range, sizeof(*range));
    if (preferred_bpp_x16) {
        if (preferred_bpp_x16 <= max_bpp_x16 && preferred_bpp_x16 >= min_bpp_x16) {
            range->max_target_bpp_x16 = preferred_bpp_x16;
            range->min_target_bpp_x16 = preferred_bpp_x16;
        }
    } else if (c->edp_sink_max_bits_per_pixel) {
        /* the bpp limit of an eDP sink */
        range->max_target_bpp_x16 = MIN(c->edp_sink_max_bits_per_pixel, max_bpp_x16);
        range->min_target_bpp_x16 = min_bpp_x16;
    } else {
        range->max_target_bpp_x16 = max_bpp_x16;
        range->min_target_bpp_x16 = min_bpp_x16;
    }
    if (range->max_target_bpp_x16 >= range->min_target_bpp_x16 && range->min_target_bpp_x16 > 0) {
        rdna3_dsc_timing nodsc = *t;
        nodsc.dsc = 0;
        range->stream_kbps = dcn32_dsc_timing_kbps(&nodsc);   /* native stream bandwidth */
        range->max_kbps = dcn32_dsc_stream_kbps(t, range->max_target_bpp_x16, num_slices_h, c->is_dp);
        range->min_kbps = dcn32_dsc_stream_kbps(t, range->min_target_bpp_x16, num_slices_h, c->is_dp);
    }
    return range->max_kbps >= range->min_kbps && range->min_kbps > 0;
}

/* dc_dsc_get_policy_for_timing */
void dcn32_dsc_get_policy(const rdna3_dsc_timing *t, uint32_t max_target_bpp_limit_override_x16, rdna3_dsc_policy *p)
{
    uint32_t bpc;
    p->min_target_bpp = 0;
    p->max_target_bpp = 0;
    p->use_min_slices_h = 1;     /* the fewest slices that fit the pixel clock */
    p->max_slices_h = 0;
    p->min_slice_height = 108;   /* VESA DSC spreadsheet user guide */
    p->enable_dsc_when_not_needed = 0;
    switch (t->bpc) {
    case 8: case 10: case 12: bpc = t->bpc; break;
    default: return;
    }
    switch (t->pixel_encoding) {
    case RD_PIXEL_ENCODING_RGB:
    case RD_PIXEL_ENCODING_YCBCR444:
    case RD_PIXEL_ENCODING_YCBCR422:   /* no native 4:2:2 assumed */
        p->min_target_bpp = 8;
        p->max_target_bpp = 3 * bpc;
        break;
    case RD_PIXEL_ENCODING_YCBCR420:
        p->min_target_bpp = 6;
        p->max_target_bpp = bpc * 3 / 2;
        break;
    default:
        return;
    }
    if (p->max_target_bpp > dsc_policy_max_target_bpp_limit)
        p->max_target_bpp = dsc_policy_max_target_bpp_limit;
    if (max_target_bpp_limit_override_x16 && p->max_target_bpp > max_target_bpp_limit_override_x16 / 16)
        p->max_target_bpp = max_target_bpp_limit_override_x16 / 16;
    p->enable_dsc_when_not_needed = dsc_policy_enable_dsc_when_not_needed;
}

/* decide_dsc_target_bpp_x16 */
static int decide_dsc_target_bpp_x16(const rdna3_dsc_policy *policy, const rdna3_dsc_options *o,
                                     const rdna3_dsc_enc_caps *c, uint32_t target_bandwidth_kbps,
                                     const rdna3_dsc_timing *t, uint32_t num_slices_h, uint32_t *target_bpp_x16)
{
    rdna3_dsc_bw_range range;
    *target_bpp_x16 = 0;
    if (decide_dsc_bandwidth_range(policy->min_target_bpp * 16, policy->max_target_bpp * 16, num_slices_h, c, t,
                                   &range)) {
        if (target_bandwidth_kbps >= range.stream_kbps) {
            if (policy->enable_dsc_when_not_needed || o->force_dsc_when_not_needed)
                *target_bpp_x16 = range.max_target_bpp_x16;   /* max bpp although DSC is not needed */
        } else if (target_bandwidth_kbps >= range.max_kbps) {
            *target_bpp_x16 = range.max_target_bpp_x16;       /* the max target bpp allowed */
        } else if (target_bandwidth_kbps >= range.min_kbps) {
            /* the target bpp that takes the entire target bandwidth */
            *target_bpp_x16 = compute_bpp_x16_from_target_bandwidth(target_bandwidth_kbps, t, num_slices_h,
                                                                    c->bpp_increment_div, c->is_dp);
        }
    }
    return *target_bpp_x16 != 0;
}

/* get_available_dsc_slices */
static int get_available_dsc_slices(uint8_t slice_caps, int *available_slices)
{
    int idx = 0;
    if (slice_caps & RD_DSC_ENC_SLICES_1) available_slices[idx++] = 1;
    if (slice_caps & RD_DSC_ENC_SLICES_2) available_slices[idx++] = 2;
    if (slice_caps & RD_DSC_ENC_SLICES_4) available_slices[idx++] = 4;
    if (slice_caps & RD_DSC_ENC_SLICES_8) available_slices[idx++] = 8;
    if (slice_caps & RD_DSC_ENC_SLICES_12) available_slices[idx++] = 12;
    if (slice_caps & RD_DSC_ENC_SLICES_16) available_slices[idx++] = 16;
    return idx;
}

/* get_max_dsc_slices */
static int get_max_dsc_slices(uint8_t slice_caps)
{
    int available_slices[6], end_idx = get_available_dsc_slices(slice_caps, available_slices);
    return end_idx > 0 ? available_slices[end_idx - 1] : 0;
}

/* inc_num_slices: the next bigger available count, or one more */
static int inc_num_slices(uint8_t slice_caps, int num_slices)
{
    int available_slices[6], end_idx, i, n = num_slices;
    end_idx = get_available_dsc_slices(slice_caps, available_slices);
    if (end_idx == 0)
        return n + 1;
    for (i = 0; i < end_idx; i++)
        if (n < available_slices[i]) {
            n = available_slices[i];
            break;
        }
    if (n == num_slices)
        n++;
    return n;
}

/* dec_num_slices: the next smaller available count, or one less (stops at 0) */
static int dec_num_slices(uint8_t slice_caps, int num_slices)
{
    int available_slices[6], end_idx, i, n = num_slices;
    end_idx = get_available_dsc_slices(slice_caps, available_slices);
    if (end_idx == 0 && n > 0)
        return n + 1;   /* sic: DC's increment when no slice count is available */
    for (i = end_idx - 1; i >= 0; i--)
        if (n > available_slices[i]) {
            n = available_slices[i];
            break;
        }
    if (n == num_slices) {
        n--;
        if (n < 0)
            n = 0;
    }
    return n;
}

/* fit_num_slices_up: the equal or next bigger available count */
static int fit_num_slices_up(uint8_t slice_caps, int num_slices)
{
    int available_slices[6], end_idx, i, n = num_slices;
    end_idx = get_available_dsc_slices(slice_caps, available_slices);
    if (end_idx == 0)
        return n + 1;
    for (i = 0; i < end_idx; i++)
        if (n <= available_slices[i]) {
            n = available_slices[i];
            break;
        }
    return n;
}

/* setup_dsc_config */
static int setup_dsc_config(const rdna3_dsc_dec_caps *sink, const rdna3_dsc_enc_caps *enc, uint32_t target_bandwidth_kbps,
                            const rdna3_dsc_timing *t, const rdna3_dsc_options *o, rdna3_dsc_config *cfg)
{
    rdna3_dsc_enc_caps cc;
    rdna3_dsc_policy policy;
    int max_slices_h = 0, min_slices_h = 0, num_slices_h = 0, pic_width, slice_width, pic_height, slice_height;
    int sink_per_slice_throughput_mps = 0, possible = 0;
    uint32_t branch_max_throughput_mps = 0, target_bpp = 0;

    zero(cfg, sizeof(*cfg));
    dcn32_dsc_get_policy(t, o->max_target_bpp_limit_override_x16, &policy);
    pic_width = (int)t->h_addressable;
    pic_height = (int)t->v_addressable;
    if (!sink->is_dsc_supported)
        goto done;
    if (sink->branch_max_line_width && (int)sink->branch_max_line_width < pic_width)
        goto done;
    /* the decoder's caps intersected with the encoder's */
    possible = intersect_dsc_caps(sink, enc, t->pixel_encoding, &cc);
    if (!possible)
        goto done;
    /* colour format (and the throughput that goes with it) */
    cfg->ycbcr422_simple = 0;
    switch (t->pixel_encoding) {
    case RD_PIXEL_ENCODING_RGB:
        possible = (cc.color_formats & RD_DSC_FMT_RGB) != 0;
        sink_per_slice_throughput_mps = sink->throughput_mode_0_mps;
        branch_max_throughput_mps = sink->branch_overall_throughput_0_mps;
        break;
    case RD_PIXEL_ENCODING_YCBCR444:
        possible = (cc.color_formats & RD_DSC_FMT_YCBCR_444) != 0;
        sink_per_slice_throughput_mps = sink->throughput_mode_0_mps;
        branch_max_throughput_mps = sink->branch_overall_throughput_0_mps;
        break;
    case RD_PIXEL_ENCODING_YCBCR422:
        possible = (cc.color_formats & RD_DSC_FMT_NATIVE_422) != 0;
        sink_per_slice_throughput_mps = sink->throughput_mode_1_mps;
        branch_max_throughput_mps = sink->branch_overall_throughput_1_mps;
        if (!possible) {
            possible = (cc.color_formats & RD_DSC_FMT_SIMPLE_422) != 0;
            cfg->ycbcr422_simple = possible;
            sink_per_slice_throughput_mps = sink->throughput_mode_0_mps;
        }
        break;
    case RD_PIXEL_ENCODING_YCBCR420:
        possible = (cc.color_formats & RD_DSC_FMT_NATIVE_420) != 0;
        sink_per_slice_throughput_mps = sink->throughput_mode_1_mps;
        branch_max_throughput_mps = sink->branch_overall_throughput_1_mps;
        break;
    default:
        possible = 0;
    }
    /* the branch's maximum throughput */
    if (branch_max_throughput_mps && dsc_div_by_10_round_up(t->pix_clk_100hz) > branch_max_throughput_mps * 1000)
        possible = 0;
    if (!possible)
        goto done;
    /* colour depth */
    switch (t->bpc) {
    case 8: possible = (cc.color_depth & RD_DSC_DEPTH_8_BPC) != 0; break;
    case 10: possible = (cc.color_depth & RD_DSC_DEPTH_10_BPC) != 0; break;
    case 12: possible = (cc.color_depth & RD_DSC_DEPTH_12_BPC) != 0; break;
    default: possible = 0;
    }
    if (!possible)
        goto done;
    /* slice width (slices per line) */
    max_slices_h = get_max_dsc_slices(cc.slice_caps);
    while (max_slices_h > 0) {
        if (pic_width % max_slices_h == 0)
            break;
        max_slices_h = dec_num_slices(cc.slice_caps, max_slices_h);
    }
    possible = cc.max_slice_width > 0;
    if (!possible)
        goto done;
    min_slices_h = pic_width / cc.max_slice_width;
    if (pic_width % cc.max_slice_width)
        min_slices_h++;
    min_slices_h = fit_num_slices_up(cc.slice_caps, min_slices_h);
    while (min_slices_h <= max_slices_h) {
        int pix_clk_per_slice_khz = (int)dsc_div_by_10_round_up(t->pix_clk_100hz) / min_slices_h;
        if (pix_clk_per_slice_khz <= sink_per_slice_throughput_mps * 1000)
            break;
        min_slices_h = inc_num_slices(cc.slice_caps, min_slices_h);
    }
    possible = min_slices_h <= max_slices_h;
    if (min_slices_h && pic_width % min_slices_h != 0)
        min_slices_h = 0;
    if (min_slices_h == 0 && max_slices_h == 0)
        possible = 0;
    if (!possible)
        goto done;
    if (policy.use_min_slices_h) {
        if (min_slices_h > 0)
            num_slices_h = min_slices_h;
        else if (max_slices_h > 0)   /* max slices when min slices does not work out */
            num_slices_h = policy.max_slices_h ? MIN(policy.max_slices_h, max_slices_h) : max_slices_h;
        else
            possible = 0;
    } else {
        if (max_slices_h > 0)
            num_slices_h = policy.max_slices_h ? MIN(policy.max_slices_h, max_slices_h) : max_slices_h;
        else if (min_slices_h > 0)
            num_slices_h = min_slices_h;
        else
            possible = 0;
    }
    /* a forced ODM needs slices divisible by the ODM segments */
    switch (o->dsc_force_odm_hslice_override) {
    case 2:
        if (num_slices_h < 2)
            num_slices_h = fit_num_slices_up(cc.slice_caps, 2);
        break;
    case 3:
        num_slices_h = (cc.slice_caps & RD_DSC_ENC_SLICES_12) ? 12 : 0;
        break;
    case 4:
        if (num_slices_h < 4)
            num_slices_h = fit_num_slices_up(cc.slice_caps, 4);
        break;
    default:
        break;
    }
    if (num_slices_h == 0)
        possible = 0;
    if (!possible)
        goto done;
    cfg->num_slices_h = (uint32_t)num_slices_h;
    slice_width = pic_width / num_slices_h;
    possible = slice_width <= cc.max_slice_width;
    if (!possible)
        goto done;
    /* slice height: the policy's, then the first the picture height divides */
    if (o->dsc_min_slice_height_override == 0)
        slice_height = MIN(policy.min_slice_height, pic_height);
    else
        slice_height = MIN((int)o->dsc_min_slice_height_override, pic_height);
    while (slice_height < pic_height &&
           (pic_height % slice_height != 0 || slice_height % (int)o->slice_height_granularity != 0 ||
            (t->pixel_encoding == RD_PIXEL_ENCODING_YCBCR420 && slice_height % 2 != 0)))
        slice_height++;
    if (t->pixel_encoding == RD_PIXEL_ENCODING_YCBCR420)
        possible = slice_height % 2 == 0;
    if (!possible)
        goto done;
    if (slice_height > 0) {
        cfg->num_slices_v = (uint32_t)(pic_height / slice_height);
    } else {
        possible = 0;
        goto done;
    }
    if (target_bandwidth_kbps > 0) {
        possible = decide_dsc_target_bpp_x16(&policy, o, &cc, target_bandwidth_kbps, t, (uint32_t)num_slices_h,
                                             &target_bpp);
        cfg->bits_per_pixel = target_bpp;
    }
    if (!possible)
        goto done;
    cfg->block_pred_enable = cc.is_block_pred_supported;
    cfg->linebuf_depth = (uint32_t)cc.lb_bit_depth;
    cfg->version_minor = (cc.dsc_version & 0xf0u) >> 4;
    cfg->is_dp = sink->is_dp;
done:
    if (!possible)
        zero(cfg, sizeof(*cfg));
    return possible;
}

/* dc_dsc_compute_config */
int dcn32_dsc_compute_config(const rdna3_dsc_dec_caps *sink, const rdna3_dsc_options *o, uint32_t target_kbps,
                             const rdna3_dsc_timing *t, rdna3_dsc_config *cfg)
{
    rdna3_dsc_enc_caps enc;
    rdna3_dsc_options def;
    if (!o) {
        dcn32_dsc_default_options(&def);
        o = &def;
    }
    get_dsc_enc_caps(&enc, t->pix_clk_100hz);
    return setup_dsc_config(sink, &enc, target_kbps, t, o, cfg);
}

/* dc_dsc_compute_bandwidth_range */
int dcn32_dsc_compute_bandwidth_range(uint32_t min_bpp_x16, uint32_t max_bpp_x16, const rdna3_dsc_dec_caps *sink,
                                      const rdna3_dsc_timing *t, rdna3_dsc_bw_range *range)
{
    rdna3_dsc_enc_caps enc, common;
    rdna3_dsc_config config;
    rdna3_dsc_options options;
    int possible;
    zero(&options, sizeof(options));
    options.dsc_min_slice_height_override = 0;
    options.max_target_bpp_limit_override_x16 = max_bpp_x16;
    options.slice_height_granularity = 1;
    get_dsc_enc_caps(&enc, t->pix_clk_100hz);
    possible = intersect_dsc_caps(sink, &enc, t->pixel_encoding, &common);
    if (possible)
        possible = setup_dsc_config(sink, &enc, 0, t, &options, &config);
    if (possible)
        possible = decide_dsc_bandwidth_range(min_bpp_x16, max_bpp_x16, config.num_slices_h, &common, t, range);
    return possible;
}

/* ==================================================================== */
/* rate control: rc_calc.c, rc_calc_fpu.c (exact), rc_calc_dpi.c, DRM  */
/* ==================================================================== */

/* struct rc_params, enum colour_mode */
#define QP_SET_SIZE 15
typedef struct rc_params {
    int rc_quant_incr_limit0, rc_quant_incr_limit1, initial_fullness_offset, initial_xmit_delay;
    int first_line_bpg_offset, second_line_bpg_offset, flatness_min_qp, flatness_max_qp, flatness_det_thresh;
    int qp_min[QP_SET_SIZE], qp_max[QP_SET_SIZE], ofs[QP_SET_SIZE];
    int rc_model_size, rc_edge_factor, rc_tgt_offset_hi, rc_tgt_offset_lo;
    int rc_buf_thresh[QP_SET_SIZE - 1];
} rc_params;
enum { CM_RGB, CM_444, CM_422, CM_420 };
enum { DAL_MM_MIN = 0, DAL_MM_MAX = 1 };

/* median3 */
static int median3(int a, int b, int c)
{
    int t;
    if (a > b) { t = a; a = b; b = t; }
    if (b > c) { t = b; b = c; c = t; }
    if (a > b) { t = b; b = c; c = t; }   /* sic: DC swaps b and c here */
    return b;
}

/* dsc_roundf on the exact value p / q (q > 0): half away from zero */
static int dsc_roundf(int64_t p, int64_t q)
{
    if (p >= 0)
        return (int)((2 * p + q) / (2 * q));
    return -(int)((q - 2 * p) / (2 * q));
}

/* get_qp_set: bpp = bpp32 / 32 */
static void get_qp_set(int *qps, int cm, int bpc, int max_min, int bpp32)
{
    const rd_dsc_qp_entry *table = 0;
    int table_size = 0, mode = (cm == CM_444 || cm == CM_RGB) ? 444 : (cm == CM_422 ? 422 : 420), index, i;
#define QPT(m, b, mm) \
    if (mode == m && bpc == b && max_min == DAL_MM_##mm) { \
        table = rd_qp_table_##m##_##b##bpc_##mm; \
        table_size = (int)(sizeof(rd_qp_table_##m##_##b##bpc_##mm) / sizeof(rd_qp_table_##m##_##b##bpc_##mm[0])); \
    }
#define DAL_MM_max DAL_MM_MAX
#define DAL_MM_min DAL_MM_MIN
    QPT(444, 8, max) QPT(444, 8, min) QPT(444, 10, max) QPT(444, 10, min) QPT(444, 12, max) QPT(444, 12, min)
    QPT(422, 8, max) QPT(422, 8, min) QPT(422, 10, max) QPT(422, 10, min) QPT(422, 12, max) QPT(422, 12, min)
    QPT(420, 8, max) QPT(420, 8, min) QPT(420, 10, max) QPT(420, 10, min) QPT(420, 12, max) QPT(420, 12, min)
#undef QPT
    if (!table)
        return;
    /* index = (bpp - table[0].bpp) * 2, truncated */
    index = (bpp32 - 16 * table[0].bpp_x2) / 16;
    if (index < 0 || index >= table_size)
        return;   /* bigger than the table: DC logs and keeps the set */
    for (i = 0; i < QP_SET_SIZE; i++)
        qps[i] = table[index].qps[i];
}

/* get_ofs_set; 'R(base, num, den)' = dsc_roundf((bpp - base) * (num / den)) */
static void get_ofs_set(int *ofs, int cm, int bpp32)
{
    int *p = ofs;
#define B(x) ((x) * 32)
#define R(base, num, den) dsc_roundf((int64_t)(bpp32 - B(base)) * (num), 32 * (int64_t)(den))
    if (cm == CM_444 || cm == CM_RGB) {
        *p++ = bpp32 <= B(6) ? 0 : (bpp32 >= B(8) && bpp32 <= B(12)) ? 2 : bpp32 >= B(15) ? 10
             : (bpp32 > B(6) && bpp32 < B(8)) ? 0 + R(6, 2, 2) : 2 + R(12, 8, 3);
        *p++ = bpp32 <= B(6) ? -2 : (bpp32 >= B(8) && bpp32 <= B(12)) ? 0 : bpp32 >= B(15) ? 8
             : (bpp32 > B(6) && bpp32 < B(8)) ? -2 + R(6, 2, 2) : 0 + R(12, 8, 3);
        *p++ = bpp32 <= B(6) ? -2 : (bpp32 >= B(8) && bpp32 <= B(12)) ? 0 : bpp32 >= B(15) ? 6
             : (bpp32 > B(6) && bpp32 < B(8)) ? -2 + R(6, 2, 2) : 0 + R(12, 6, 3);
        *p++ = bpp32 <= B(6) ? -4 : (bpp32 >= B(8) && bpp32 <= B(12)) ? -2 : bpp32 >= B(15) ? 4
             : (bpp32 > B(6) && bpp32 < B(8)) ? -4 + R(6, 2, 2) : -2 + R(12, 6, 3);
        *p++ = bpp32 <= B(6) ? -6 : (bpp32 >= B(8) && bpp32 <= B(12)) ? -4 : bpp32 >= B(15) ? 2
             : (bpp32 > B(6) && bpp32 < B(8)) ? -6 + R(6, 2, 2) : -4 + R(12, 6, 3);
        *p++ = bpp32 <= B(12) ? -6 : bpp32 >= B(15) ? 0 : -6 + R(12, 6, 3);
        *p++ = bpp32 <= B(12) ? -8 : bpp32 >= B(15) ? -2 : -8 + R(12, 6, 3);
        *p++ = bpp32 <= B(12) ? -8 : bpp32 >= B(15) ? -4 : -8 + R(12, 4, 3);
        *p++ = bpp32 <= B(12) ? -8 : bpp32 >= B(15) ? -6 : -8 + R(12, 2, 3);
        *p++ = bpp32 <= B(12) ? -10 : bpp32 >= B(15) ? -8 : -10 + R(12, 2, 3);
        *p++ = -10;
        *p++ = bpp32 <= B(6) ? -12 : bpp32 >= B(8) ? -10 : -12 + R(6, 2, 2);
        *p++ = -12;
        *p++ = -12;
        *p++ = -12;
    } else if (cm == CM_422) {
        *p++ = bpp32 <= B(8) ? 2 : bpp32 >= B(10) ? 10 : 2 + R(8, 8, 2);
        *p++ = bpp32 <= B(8) ? 0 : bpp32 >= B(10) ? 8 : 0 + R(8, 8, 2);
        *p++ = bpp32 <= B(8) ? 0 : bpp32 >= B(10) ? 6 : 0 + R(8, 6, 2);
        *p++ = bpp32 <= B(8) ? -2 : bpp32 >= B(10) ? 4 : -2 + R(8, 6, 2);
        *p++ = bpp32 <= B(8) ? -4 : bpp32 >= B(10) ? 2 : -4 + R(8, 6, 2);
        *p++ = bpp32 <= B(8) ? -6 : bpp32 >= B(10) ? 0 : -6 + R(8, 6, 2);
        *p++ = bpp32 <= B(8) ? -8 : bpp32 >= B(10) ? -2 : -8 + R(8, 6, 2);
        *p++ = bpp32 <= B(8) ? -8 : bpp32 >= B(10) ? -4 : -8 + R(8, 4, 2);
        *p++ = bpp32 <= B(8) ? -8 : bpp32 >= B(10) ? -6 : -8 + R(8, 2, 2);
        *p++ = bpp32 <= B(8) ? -10 : bpp32 >= B(10) ? -8 : -10 + R(8, 2, 2);
        *p++ = -10;
        *p++ = bpp32 <= B(6) ? -12 : bpp32 >= B(7) ? -10 : -12 + R(6, 2, 1);
        *p++ = -12;
        *p++ = -12;
        *p++ = -12;
    } else {
        *p++ = bpp32 <= B(6) ? 2 : bpp32 >= B(8) ? 10 : 2 + R(6, 8, 2);
        *p++ = bpp32 <= B(6) ? 0 : bpp32 >= B(8) ? 8 : 0 + R(6, 8, 2);
        *p++ = bpp32 <= B(6) ? 0 : bpp32 >= B(8) ? 6 : 0 + R(6, 6, 2);
        *p++ = bpp32 <= B(6) ? -2 : bpp32 >= B(8) ? 4 : -2 + R(6, 6, 2);
        *p++ = bpp32 <= B(6) ? -4 : bpp32 >= B(8) ? 2 : -4 + R(6, 6, 2);
        *p++ = bpp32 <= B(6) ? -6 : bpp32 >= B(8) ? 0 : -6 + R(6, 6, 2);
        *p++ = bpp32 <= B(6) ? -8 : bpp32 >= B(8) ? -2 : -8 + R(6, 6, 2);
        *p++ = bpp32 <= B(6) ? -8 : bpp32 >= B(8) ? -4 : -8 + R(6, 4, 2);
        *p++ = bpp32 <= B(6) ? -8 : bpp32 >= B(8) ? -6 : -8 + R(6, 2, 2);
        *p++ = bpp32 <= B(6) ? -10 : bpp32 >= B(8) ? -8 : -10 + R(6, 2, 2);
        *p++ = -10;
        *p++ = bpp32 <= B(4) ? -12 : bpp32 >= B(5) ? -10 : -12 + R(4, 2, 1);
        *p++ = -12;
        *p++ = -12;
        *p++ = -12;
    }
#undef R
#undef B
}

/* _do_calc_rc_params (exact: bpp = bpp32 / 32, see the file comment) */
static void _do_calc_rc_params(rc_params *rc, int cm, int bpc, uint32_t drm_bpp, int is_navite_422_or_420,
                               int slice_width, int slice_height, int minor_version)
{
    int bpp32 = (int)drm_bpp * 2, bg32, padding_pixels, i, factor, x;
    if (is_navite_422_or_420)
        bpp32 /= 2;   /* native 4:2:x: bits_per_pixel is twice the target bpp */
    rc->rc_quant_incr_limit0 = (bpc == 8 ? 11 : bpc == 10 ? 15 : 19) - ((minor_version == 1 && cm == CM_444) ? 1 : 0);
    rc->rc_quant_incr_limit1 = rc->rc_quant_incr_limit0;
    bg32 = (cm == CM_444 || cm == CM_RGB) ? bpp32 : bpp32 * 2;   /* bpp_group */
    x = MIN(34, slice_height - 8);
    switch (cm) {
    case CM_420:
        rc->initial_fullness_offset = bpp32 >= 6 * 32 ? 2048 : bpp32 <= 4 * 32 ? 6144
            : (bpp32 > 4 * 32 && bpp32 <= 5 * 32) ? 6144 - dsc_roundf((int64_t)(bpp32 - 128) * 512, 32)
            : 5632 - dsc_roundf((int64_t)(bpp32 - 160) * 3584, 32);
        rc->first_line_bpg_offset = median3(0, 12 + (9 * x) / 100, (3 * bpc * 3 * 32 - 3 * bg32) / 32);
        rc->second_line_bpg_offset = median3(0, 12, (3 * bpc * 3 * 32 - 3 * bg32) / 32);
        break;
    case CM_422:
        rc->initial_fullness_offset = bpp32 >= 8 * 32 ? 2048 : bpp32 <= 7 * 32 ? 5632
            : 5632 - dsc_roundf((int64_t)(bpp32 - 224) * 3584, 32);
        rc->first_line_bpg_offset = median3(0, 12 + (9 * x) / 100, (3 * bpc * 4 * 32 - 3 * bg32) / 32);
        rc->second_line_bpg_offset = 0;
        break;
    default:   /* CM_444, CM_RGB */
        rc->initial_fullness_offset = bpp32 >= 12 * 32 ? 2048 : bpp32 <= 8 * 32 ? 6144
            : (bpp32 > 8 * 32 && bpp32 <= 10 * 32) ? 6144 - dsc_roundf((int64_t)(bpp32 - 256) * 256, 32)
            : 5632 - dsc_roundf((int64_t)(bpp32 - 320) * 1792, 32);
        rc->first_line_bpg_offset = median3(0, 12 + (9 * x) / 100,
                                            ((3 * bpc + (cm == CM_444 ? 0 : 2)) * 3 * 32 - 3 * bg32) / 32);
        rc->second_line_bpg_offset = 0;
        break;
    }
    factor = (cm == CM_444 || cm == CM_RGB) ? 1 : 2;
    /* dsc_roundf(8192.0 / 2.0 / bpp / factor) */
    rc->initial_xmit_delay = dsc_roundf(4096 * 32, (int64_t)bpp32 * factor);
    if (cm == CM_422 || cm == CM_420)
        slice_width /= 2;
    padding_pixels = (slice_width % 3) != 0 ? (3 - (slice_width % 3)) * (rc->initial_xmit_delay / slice_width) : 0;
    if (3 * bg32 >= 32 * (((rc->initial_xmit_delay + 2) / 3) * (3 + (cm == CM_422)))) {
        if ((rc->initial_xmit_delay + padding_pixels) % 3 == 1)
            rc->initial_xmit_delay++;
    }
    rc->flatness_min_qp = (bpc == 8 ? 3 : bpc == 10 ? 7 : 11) - ((minor_version == 1 && cm == CM_444) ? 1 : 0);
    rc->flatness_max_qp = (bpc == 8 ? 12 : bpc == 10 ? 16 : 20) - ((minor_version == 1 && cm == CM_444) ? 1 : 0);
    rc->flatness_det_thresh = 2 << (bpc - 8);
    get_qp_set(rc->qp_min, cm, bpc, DAL_MM_MIN, bpp32);
    get_qp_set(rc->qp_max, cm, bpc, DAL_MM_MAX, bpp32);
    if (cm == CM_444 && minor_version == 1) {
        for (i = 0; i < QP_SET_SIZE; ++i) {
            rc->qp_min[i] = rc->qp_min[i] > 0 ? rc->qp_min[i] - 1 : 0;
            rc->qp_max[i] = rc->qp_max[i] > 0 ? rc->qp_max[i] - 1 : 0;
        }
    }
    get_ofs_set(rc->ofs, cm, bpp32);
    /* fixed parameters */
    rc->rc_model_size = 8192;
    rc->rc_edge_factor = 6;
    rc->rc_tgt_offset_hi = 3;
    rc->rc_tgt_offset_lo = 3;
    {
        static const int th[QP_SET_SIZE - 1] = { 896, 1792, 2688, 3584, 4480, 5376, 6272,
                                                 6720, 7168, 7616, 7744, 7872, 8000, 8064 };
        for (i = 0; i < QP_SET_SIZE - 1; i++)
            rc->rc_buf_thresh[i] = th[i];
    }
}

/* calc_rc_params */
static void calc_rc_params(rc_params *rc, const rdna3_dsc_pps *pps)
{
    int mode = pps->convert_rgb ? CM_RGB : pps->simple_422 ? CM_444 : pps->native_422 ? CM_422
             : pps->native_420 ? CM_420 : CM_444;
    int bpc = pps->bits_per_component == 8 ? 8 : pps->bits_per_component == 10 ? 10 : 12;
    zero(rc, sizeof(*rc));
    _do_calc_rc_params(rc, mode, bpc, pps->bits_per_pixel, pps->native_422 || pps->native_420, pps->slice_width,
                       pps->slice_height, pps->dsc_version_minor);
}

#define DIV_ROUND_UP(a, b) (((a) + (b) - 1) / (b))

/* drm_dsc_compute_rc_parameters */
static int drm_dsc_compute_rc_parameters(rdna3_dsc_pps *c)
{
    unsigned long groups_per_line, groups_total, num_extra_mux_bits, slice_bits, hrd_delay, final_scale, rbs_min;
    if (c->native_420 || c->native_422) {
        groups_per_line = DIV_ROUND_UP((unsigned long)(c->slice_width / 2), 3ul);   /* DSC_RC_PIXELS_PER_GROUP */
        c->slice_chunk_size = (uint16_t)DIV_ROUND_UP((unsigned long)(c->slice_width / 2) * c->bits_per_pixel, 8ul * 16);
    } else {
        groups_per_line = DIV_ROUND_UP((unsigned long)c->slice_width, 3ul);
        c->slice_chunk_size = (uint16_t)DIV_ROUND_UP((unsigned long)c->slice_width * c->bits_per_pixel, 8ul * 16);
    }
    if (c->convert_rgb)
        num_extra_mux_bits = 3ul * (c->mux_word_size + (4ul * c->bits_per_component + 4) - 2);
    else if (c->native_422)
        num_extra_mux_bits = 4ul * c->mux_word_size + (4ul * c->bits_per_component + 4) +
                             3ul * (4ul * c->bits_per_component) - 2;
    else
        num_extra_mux_bits = 3ul * c->mux_word_size + (4ul * c->bits_per_component + 4) +
                             2ul * (4ul * c->bits_per_component) - 2;
    /* bits in one slice */
    slice_bits = 8ul * c->slice_chunk_size * c->slice_height;
    while (num_extra_mux_bits > 0 && ((slice_bits - num_extra_mux_bits) % c->mux_word_size))
        num_extra_mux_bits--;
    if (groups_per_line < (unsigned long)c->initial_scale_value - 8)
        c->initial_scale_value = (uint8_t)(groups_per_line + 8);
    /* scale_decrement_interval, DSC spec 1.11 */
    if (c->initial_scale_value > 8)
        c->scale_decrement_interval = (uint16_t)(groups_per_line / (c->initial_scale_value - 8u));
    else
        c->scale_decrement_interval = 4095;   /* DSC_SCALE_DECREMENT_INTERVAL_MAX */
    c->final_offset = (uint16_t)(c->rc_model_size - (c->initial_xmit_delay * c->bits_per_pixel + 8u) / 16u +
                                 num_extra_mux_bits);
    if (c->final_offset >= c->rc_model_size)
        return RDNA3_EINVAL;   /* -ERANGE: FinalOfs >= RcModelSize for this InitialXmitDelay */
    final_scale = (c->rc_model_size * 8ul) / (c->rc_model_size - c->final_offset);
    if (c->slice_height > 1)
        /* 16-bit value with 11 fractional bits */
        c->nfl_bpg_offset = (uint16_t)DIV_ROUND_UP((unsigned long)c->first_line_bpg_offset << 11,
                                                   (unsigned long)(c->slice_height - 1));
    else
        c->nfl_bpg_offset = 0;
    groups_total = groups_per_line * c->slice_height;
    c->slice_bpg_offset = (uint16_t)DIV_ROUND_UP(
        ((unsigned long)c->rc_model_size - c->initial_offset + num_extra_mux_bits) << 11, groups_total);
    if (final_scale > 9)
        c->scale_increment_interval = (uint16_t)(((unsigned long)c->final_offset * (1ul << 11)) /
                                                 (((unsigned long)c->nfl_bpg_offset + c->slice_bpg_offset) *
                                                  (final_scale - 9)));
    else
        c->scale_increment_interval = 0;   /* no scale increment at the end of the slice */
    rbs_min = c->rc_model_size - c->initial_offset +
              DIV_ROUND_UP((unsigned long)c->initial_xmit_delay * c->bits_per_pixel, 16ul) +
              groups_per_line * c->first_line_bpg_offset;
    hrd_delay = DIV_ROUND_UP(rbs_min * 16, (unsigned long)c->bits_per_pixel);
    c->rc_bits = (uint16_t)((hrd_delay * c->bits_per_pixel) / 16);
    c->initial_dec_delay = (uint16_t)(hrd_delay - c->initial_xmit_delay);
    return RDNA3_OK;
}

/* copy_pps_fields */
static void copy_pps_fields(rdna3_dsc_pps *to, const rdna3_dsc_pps *from)
{
    uint32_t i;
    to->line_buf_depth = from->line_buf_depth;
    to->bits_per_component = from->bits_per_component;
    to->convert_rgb = from->convert_rgb;
    to->slice_width = from->slice_width;
    to->slice_height = from->slice_height;
    to->simple_422 = from->simple_422;
    to->native_422 = from->native_422;
    to->native_420 = from->native_420;
    to->pic_width = from->pic_width;
    to->pic_height = from->pic_height;
    to->rc_tgt_offset_high = from->rc_tgt_offset_high;
    to->rc_tgt_offset_low = from->rc_tgt_offset_low;
    to->bits_per_pixel = from->bits_per_pixel;
    to->rc_edge_factor = from->rc_edge_factor;
    to->rc_quant_incr_limit1 = from->rc_quant_incr_limit1;
    to->rc_quant_incr_limit0 = from->rc_quant_incr_limit0;
    to->initial_xmit_delay = from->initial_xmit_delay;
    to->initial_dec_delay = from->initial_dec_delay;
    to->block_pred_enable = from->block_pred_enable;
    to->first_line_bpg_offset = from->first_line_bpg_offset;
    to->second_line_bpg_offset = from->second_line_bpg_offset;
    to->initial_offset = from->initial_offset;
    for (i = 0; i < RD_DSC_NUM_BUF_RANGES - 1; i++)
        to->rc_buf_thresh[i] = from->rc_buf_thresh[i];
    for (i = 0; i < RD_DSC_NUM_BUF_RANGES; i++)
        to->rc_range_params[i] = from->rc_range_params[i];
    to->rc_model_size = from->rc_model_size;
    to->flatness_min_qp = from->flatness_min_qp;
    to->flatness_max_qp = from->flatness_max_qp;
    to->initial_scale_value = from->initial_scale_value;
    to->scale_decrement_interval = from->scale_decrement_interval;
    to->scale_increment_interval = from->scale_increment_interval;
    to->nfl_bpg_offset = from->nfl_bpg_offset;
    to->nsl_bpg_offset = from->nsl_bpg_offset;
    to->slice_bpg_offset = from->slice_bpg_offset;
    to->final_offset = from->final_offset;
    to->vbr_enable = from->vbr_enable;
    to->slice_chunk_size = from->slice_chunk_size;
    to->second_line_offset_adj = from->second_line_offset_adj;
    to->dsc_version_minor = from->dsc_version_minor;
}

/* copy_rc_to_cfg */
static void copy_rc_to_cfg(rdna3_dsc_pps *c, const rc_params *rc)
{
    int i;
    c->rc_quant_incr_limit0 = (uint8_t)rc->rc_quant_incr_limit0;
    c->rc_quant_incr_limit1 = (uint8_t)rc->rc_quant_incr_limit1;
    c->initial_offset = (uint16_t)rc->initial_fullness_offset;
    c->initial_xmit_delay = (uint16_t)rc->initial_xmit_delay;
    c->first_line_bpg_offset = (uint8_t)rc->first_line_bpg_offset;
    c->second_line_bpg_offset = (uint8_t)rc->second_line_bpg_offset;
    c->flatness_min_qp = (uint8_t)rc->flatness_min_qp;
    c->flatness_max_qp = (uint8_t)rc->flatness_max_qp;
    for (i = 0; i < QP_SET_SIZE; ++i) {
        c->rc_range_params[i].range_min_qp = (uint8_t)rc->qp_min[i];
        c->rc_range_params[i].range_max_qp = (uint8_t)rc->qp_max[i];
        /* the 8-bit signed value truncated to a 6-bit signed one */
        c->rc_range_params[i].range_bpg_offset = (uint8_t)(0x3f & rc->ofs[i]);
    }
    c->rc_model_size = (uint16_t)rc->rc_model_size;
    c->rc_edge_factor = (uint8_t)rc->rc_edge_factor;
    c->rc_tgt_offset_high = (uint8_t)rc->rc_tgt_offset_hi;
    c->rc_tgt_offset_low = (uint8_t)rc->rc_tgt_offset_lo;
    for (i = 0; i < QP_SET_SIZE - 1; ++i)
        c->rc_buf_thresh[i] = (uint16_t)rc->rc_buf_thresh[i];
}

/* struct dsc_parameters */
typedef struct dsc_parameters {
    rdna3_dsc_pps pps;
    uint32_t bytes_per_pixel, rc_buffer_model_size;
} dsc_parameters;

/* dscc_compute_dsc_parameters */
static int dscc_compute_dsc_parameters(const rdna3_dsc_pps *pps, const rc_params *rc, dsc_parameters *p)
{
    rdna3_dsc_pps cfg;
    int ret;
    zero(&cfg, sizeof(cfg));
    p->pps = *pps;
    p->pps.initial_scale_value = (uint8_t)(8 * rc->rc_model_size / (rc->rc_model_size - rc->initial_fullness_offset));
    copy_pps_fields(&cfg, &p->pps);
    copy_rc_to_cfg(&cfg, rc);
    cfg.mux_word_size = p->pps.bits_per_component <= 10 ? 48 : 64;
    ret = drm_dsc_compute_rc_parameters(&cfg);
    /* round up */
    p->bytes_per_pixel = (uint32_t)(((uint64_t)cfg.slice_chunk_size * 0x10000000 + (cfg.slice_width - 1u)) /
                                    cfg.slice_width);
    copy_pps_fields(&p->pps, &cfg);
    p->rc_buffer_model_size = cfg.rc_bits;
    return ret;
}

/* dsc_init_reg_values */
static void dsc_init_reg_values(rdna3_dsc_reg_values *r)
{
    zero(r, sizeof(*r));
    r->dsc_clock_enable = 1;
    r->pps.dsc_version_minor = 2;
    r->pps.dsc_version_major = 1;
    r->pps.line_buf_depth = 9;
    r->pps.bits_per_component = 8;
    r->pps.block_pred_enable = 1;
    r->pps.initial_xmit_delay = 170;
    r->pps.initial_offset = 6144;
    r->pps.flatness_min_qp = 3;
    r->pps.flatness_max_qp = 12;
    r->pps.rc_model_size = 8192;
    r->pps.rc_edge_factor = 6;
    r->pps.rc_quant_incr_limit0 = 11;
    r->pps.rc_quant_incr_limit1 = 11;
    r->pps.rc_tgt_offset_low = 3;
    r->pps.rc_tgt_offset_high = 3;
}

/* dsc_update_from_dsc_parameters: the computed PPS, thresholds shifted for the registers */
static void dsc_update_from_dsc_parameters(rdna3_dsc_reg_values *r, const dsc_parameters *p)
{
    uint32_t i;
    r->pps = p->pps;
    for (i = 0; i < RD_DSC_NUM_BUF_RANGES - 1; i++)
        r->pps.rc_buf_thresh[i] = (uint16_t)(r->pps.rc_buf_thresh[i] >> 6);
    r->rc_buffer_model_size = p->rc_buffer_model_size;
}

/* dsc_dc_pixel_encoding_to_dsc_pixel_format */
static uint32_t dsc_dc_pixel_encoding_to_dsc_pixel_format(uint32_t enc, int is_ycbcr422_simple)
{
    switch (enc) {
    case RD_PIXEL_ENCODING_RGB: return RD_DSC_PIXFMT_RGB;
    case RD_PIXEL_ENCODING_YCBCR422:
        return is_ycbcr422_simple ? RD_DSC_PIXFMT_SIMPLE_YCBCR422 : RD_DSC_PIXFMT_NATIVE_YCBCR422;
    case RD_PIXEL_ENCODING_YCBCR444: return RD_DSC_PIXFMT_YCBCR444;
    case RD_PIXEL_ENCODING_YCBCR420: return RD_DSC_PIXFMT_NATIVE_YCBCR420;
    default: return RD_DSC_PIXFMT_UNKNOWN;
    }
}

/* dsc_dc_color_depth_to_dsc_bits_per_comp (DSC_BPC_UNKNOWN = 13) */
static uint32_t dsc_dc_color_depth_to_dsc_bits_per_comp(uint32_t bpc)
{
    return (bpc == 8 || bpc == 10 || bpc == 12) ? bpc : 13;
}

int dcn32_dsc_prepare_config(const rdna3_dsc_hw_config *cfg, rdna3_dsc_reg_values *r, rdna3_dsc_optc_config *optc)
{
    const rdna3_dsc_config *dc = &cfg->dc_dsc_cfg;
    dsc_parameters params;
    rc_params rc;

    /* dsc_prepare_config: validate the input */
    if (!dc->num_slices_v || !dc->num_slices_h || !(dc->version_minor == 1 || dc->version_minor == 2) ||
        !cfg->pic_width || !cfg->pic_height ||
        !((dc->version_minor == 1 && 8 <= dc->linebuf_depth && dc->linebuf_depth <= 13) ||
          (dc->version_minor == 2 && ((8 <= dc->linebuf_depth && dc->linebuf_depth <= 15) || dc->linebuf_depth == 0))) ||
        !(96 <= dc->bits_per_pixel && dc->bits_per_pixel <= 0x3ff))
        return 0;   /* invalid parameters */
    dsc_init_reg_values(r);
    r->pixel_format = dsc_dc_pixel_encoding_to_dsc_pixel_format(cfg->pixel_encoding, dc->ycbcr422_simple);
    r->num_slices_h = dc->num_slices_h;
    r->num_slices_v = dc->num_slices_v;
    r->pps.dsc_version_minor = (uint8_t)dc->version_minor;
    r->pps.pic_width = (uint16_t)cfg->pic_width;
    r->pps.pic_height = (uint16_t)cfg->pic_height;
    r->pps.bits_per_component = (uint8_t)dsc_dc_color_depth_to_dsc_bits_per_comp(cfg->bpc);
    r->pps.block_pred_enable = dc->block_pred_enable;
    r->pps.line_buf_depth = (uint8_t)dc->linebuf_depth;
    r->alternate_ich_encoding_en = r->pps.dsc_version_minor == 1 ? 0 : 1;
    r->ich_reset_at_eol = (cfg->is_odm || r->num_slices_h > 1) ? 0xF : 0;
    r->pps.slice_width = (uint16_t)(cfg->pic_width / dc->num_slices_h);
    r->pps.slice_height = (uint16_t)(cfg->pic_height / dc->num_slices_v);
    if (r->pps.slice_height * dc->num_slices_v != cfg->pic_height)
        return 0;   /* picture height not divisible by num_slices_v */
    r->bpp_x32 = dc->bits_per_pixel << 1;
    if (r->pixel_format == RD_DSC_PIXFMT_NATIVE_YCBCR420 || r->pixel_format == RD_DSC_PIXFMT_NATIVE_YCBCR422)
        r->pps.bits_per_pixel = (uint16_t)r->bpp_x32;
    else
        r->pps.bits_per_pixel = (uint16_t)(r->bpp_x32 >> 1);
    r->pps.convert_rgb = r->pixel_format == RD_DSC_PIXFMT_RGB;
    r->pps.native_422 = r->pixel_format == RD_DSC_PIXFMT_NATIVE_YCBCR422;
    r->pps.native_420 = r->pixel_format == RD_DSC_PIXFMT_NATIVE_YCBCR420;
    r->pps.simple_422 = r->pixel_format == RD_DSC_PIXFMT_SIMPLE_YCBCR422;
    calc_rc_params(&rc, &r->pps);
    if (dscc_compute_dsc_parameters(&r->pps, &rc, &params))
        return 0;   /* DSC config failed */
    dsc_update_from_dsc_parameters(r, &params);
    optc->bytes_per_pixel = params.bytes_per_pixel;
    optc->slice_width = r->pps.slice_width;
    optc->is_pixel_format_444 = r->pixel_format == RD_DSC_PIXFMT_RGB || r->pixel_format == RD_DSC_PIXFMT_YCBCR444 ||
                                r->pixel_format == RD_DSC_PIXFMT_SIMPLE_YCBCR422;
    return 1;
}

static void put_be16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* drm_dsc_pps_payload_pack (struct drm_dsc_picture_parameter_set byte layout) */
void dcn32_dsc_pps_payload_pack(uint8_t *p, const rdna3_dsc_pps *c)
{
    uint32_t i;
    zero(p, RD_DSC_PPS_BYTES);
    p[0] = (uint8_t)(c->dsc_version_minor | c->dsc_version_major << 4);       /* DSC_PPS_VERSION_MAJOR_SHIFT */
    p[3] = (uint8_t)(c->line_buf_depth | c->bits_per_component << 4);         /* DSC_PPS_BPC_SHIFT */
    p[4] = (uint8_t)(((c->bits_per_pixel & 0x300) >> 8) | (c->vbr_enable ? 1u << 2 : 0) |
                     (c->simple_422 ? 1u << 3 : 0) | (c->convert_rgb ? 1u << 4 : 0) |
                     (c->block_pred_enable ? 1u << 5 : 0));
    p[5] = (uint8_t)(c->bits_per_pixel & 0xff);
    put_be16(p + 6, c->pic_height);
    put_be16(p + 8, c->pic_width);
    put_be16(p + 10, c->slice_height);
    put_be16(p + 12, c->slice_width);
    put_be16(p + 14, c->slice_chunk_size);
    p[16] = (uint8_t)((c->initial_xmit_delay & 0x300) >> 8);
    p[17] = (uint8_t)(c->initial_xmit_delay & 0xff);
    put_be16(p + 18, c->initial_dec_delay);
    p[21] = c->initial_scale_value;
    put_be16(p + 22, c->scale_increment_interval);
    p[24] = (uint8_t)((c->scale_decrement_interval & 0xf00) >> 8);
    p[25] = (uint8_t)(c->scale_decrement_interval & 0xff);
    p[27] = c->first_line_bpg_offset;
    put_be16(p + 28, c->nfl_bpg_offset);
    put_be16(p + 30, c->slice_bpg_offset);
    put_be16(p + 32, c->initial_offset);
    put_be16(p + 34, c->final_offset);
    p[36] = c->flatness_min_qp;
    p[37] = c->flatness_max_qp;
    put_be16(p + 38, c->rc_model_size);
    p[40] = 6;                                                 /* DSC_RC_EDGE_FACTOR_CONST */
    p[41] = c->rc_quant_incr_limit0;
    p[42] = c->rc_quant_incr_limit1;
    p[43] = (uint8_t)(3 | 3 << 4);                             /* DSC_RC_TGT_OFFSET_LO/HI_CONST */
    for (i = 0; i < RD_DSC_NUM_BUF_RANGES - 1; i++)
        p[44 + i] = (uint8_t)c->rc_buf_thresh[i];
    for (i = 0; i < RD_DSC_NUM_BUF_RANGES; i++)
        put_be16(p + 58 + 2 * i, (uint32_t)c->rc_range_params[i].range_min_qp << 11 |
                                 (uint32_t)c->rc_range_params[i].range_max_qp << 6 |
                                 c->rc_range_params[i].range_bpg_offset);
    p[88] = (uint8_t)((c->native_422 ? 1 : 0) | (c->native_420 ? 1u << 1 : 0));
    p[89] = c->second_line_bpg_offset;
    put_be16(p + 90, c->nsl_bpg_offset);
    put_be16(p + 92, c->second_line_offset_adj);
}

/* dsc2_get_packed_pps */
int dcn32_dsc_get_packed_pps(const rdna3_dsc_hw_config *cfg, uint8_t *pps)
{
    rdna3_dsc_reg_values rv;
    rdna3_dsc_optc_config optc;
    int ok;
    zero(&rv, sizeof(rv));
    zero(&optc, sizeof(optc));
    ok = dcn32_dsc_prepare_config(cfg, &rv, &optc);
    dcn32_dsc_pps_payload_pack(pps, &rv.pps);
    return ok;
}

/* ==================================================================== */
/* hardware                                                             */
/* ==================================================================== */

/* dcn32_dsc_pg_status: DOMAIN16..19 PGFSM status 0 = powered */
int dcn32_dsc_pg_status(rdna3_dev *d, uint32_t inst)
{
    if (inst >= RD_DSC_ENGINES)
        return 0;
    return GET(RI(d, regDOMAIN16_PG_STATUS, inst, PG_S), DOMAIN16_PG_STATUS, DOMAIN_PGFSM_PWR_STATUS) == 0;
}

/* dcn32_dsc_pg_control under DC_IP_REQUEST_CNTL (enable_double_buffered_dsc_pg_support) */
int dcn32_dsc_pg_control(rdna3_dev *d, uint32_t inst, int power_on)
{
    uint32_t power_gate = power_on ? 0 : 1, pwr_status = power_on ? 0 : 2, org, v;
    int r;
    if (inst >= RD_DSC_ENGINES)
        return RDNA3_EINVAL;
    org = GET(RI(d, regDC_IP_REQUEST_CNTL, 0, 0), DC_IP_REQUEST_CNTL, IP_REQUEST_EN);
    if (!org)
        WI(d, regDC_IP_REQUEST_CNTL, 0, 0, FIELD(DC_IP_REQUEST_CNTL, IP_REQUEST_EN, 1));
    v = RI(d, regDOMAIN16_PG_CONFIG, inst, PG_S);
    WI(d, regDOMAIN16_PG_CONFIG, inst, PG_S, SET(v, DOMAIN16_PG_CONFIG, DOMAIN_POWER_GATE, power_gate));
    r = rdna3_wait_reg(d, AI(d, regDOMAIN16_PG_STATUS, inst, PG_S),
                       pwr_status << DOMAIN16_PG_STATUS__DOMAIN_PGFSM_PWR_STATUS__SHIFT,
                       DOMAIN16_PG_STATUS__DOMAIN_PGFSM_PWR_STATUS_MASK, 0, 1000);
    if (!org)
        WI(d, regDC_IP_REQUEST_CNTL, 0, 0, 0);
    if (r)
        RERR(d, "dsc: DSC %u power %s: PGFSM status 0x%x", inst, power_on ? "up" : "down",
             RI(d, regDOMAIN16_PG_STATUS, inst, PG_S));
    return r ? RDNA3_ETIME : RDNA3_OK;
}

/* dsc_write_to_registers */
static void dsc_write_to_registers(rdna3_dev *d, uint32_t n, const rdna3_dsc_reg_values *r)
{
    const rdna3_dsc_pps *p = &r->pps;
    uint32_t temp_int, i;
#define W(reg, v) wr(d, reg##_BASE_IDX, (uint32_t)(reg) + (uint32_t)DSC_S * n, (v))
#define RP(i) p->rc_range_params[i]
    W(regDSC_TOP0_DSC_DEBUG_CONTROL, FIELD(DSC_TOP0_DSC_DEBUG_CONTROL, DSC_DBG_EN, r->dsc_dbg_en));
    /* dsccif */
    W(regDSCCIF0_DSCCIF_CONFIG0,
      FIELD(DSCCIF0_DSCCIF_CONFIG0, INPUT_INTERFACE_UNDERFLOW_RECOVERY_EN, r->underflow_recovery_en) |
      FIELD(DSCCIF0_DSCCIF_CONFIG0, INPUT_INTERFACE_UNDERFLOW_OCCURRED_INT_EN, r->underflow_occurred_int_en) |
      FIELD(DSCCIF0_DSCCIF_CONFIG0, INPUT_INTERFACE_UNDERFLOW_OCCURRED_STATUS, r->underflow_occurred_status) |
      FIELD(DSCCIF0_DSCCIF_CONFIG0, INPUT_PIXEL_FORMAT, r->pixel_format) |
      FIELD(DSCCIF0_DSCCIF_CONFIG0, BITS_PER_COMPONENT, p->bits_per_component));
    W(regDSCCIF0_DSCCIF_CONFIG1, FIELD(DSCCIF0_DSCCIF_CONFIG1, PIC_WIDTH, p->pic_width) |
                                 FIELD(DSCCIF0_DSCCIF_CONFIG1, PIC_HEIGHT, p->pic_height));
    /* dscc (DCN 3.2 has ICH_RESET_AT_END_OF_LINE) */
    W(regDSCC0_DSCC_CONFIG0, FIELD(DSCC0_DSCC_CONFIG0, ICH_RESET_AT_END_OF_LINE, r->ich_reset_at_eol) |
                             FIELD(DSCC0_DSCC_CONFIG0, NUMBER_OF_SLICES_PER_LINE, r->num_slices_h - 1) |
                             FIELD(DSCC0_DSCC_CONFIG0, ALTERNATE_ICH_ENCODING_EN, r->alternate_ich_encoding_en) |
                             FIELD(DSCC0_DSCC_CONFIG0, NUMBER_OF_SLICES_IN_VERTICAL_DIRECTION, r->num_slices_v - 1));
    W(regDSCC0_DSCC_CONFIG1, FIELD(DSCC0_DSCC_CONFIG1, DSCC_RATE_CONTROL_BUFFER_MODEL_SIZE, r->rc_buffer_model_size));
    W(regDSCC0_DSCC_INTERRUPT_CONTROL_STATUS,
      FIELD(DSCC0_DSCC_INTERRUPT_CONTROL_STATUS, DSCC_RATE_CONTROL_BUFFER_MODEL0_OVERFLOW_OCCURRED_INT_EN,
            r->rc_buffer_model_overflow_int_en[0]) |
      FIELD(DSCC0_DSCC_INTERRUPT_CONTROL_STATUS, DSCC_RATE_CONTROL_BUFFER_MODEL1_OVERFLOW_OCCURRED_INT_EN,
            r->rc_buffer_model_overflow_int_en[1]) |
      FIELD(DSCC0_DSCC_INTERRUPT_CONTROL_STATUS, DSCC_RATE_CONTROL_BUFFER_MODEL2_OVERFLOW_OCCURRED_INT_EN,
            r->rc_buffer_model_overflow_int_en[2]) |
      FIELD(DSCC0_DSCC_INTERRUPT_CONTROL_STATUS, DSCC_RATE_CONTROL_BUFFER_MODEL3_OVERFLOW_OCCURRED_INT_EN,
            r->rc_buffer_model_overflow_int_en[3]));
    W(regDSCC0_DSCC_PPS_CONFIG0, FIELD(DSCC0_DSCC_PPS_CONFIG0, DSC_VERSION_MINOR, p->dsc_version_minor) |
                                 FIELD(DSCC0_DSCC_PPS_CONFIG0, LINEBUF_DEPTH, p->line_buf_depth) |
                                 FIELD(DSCC0_DSCC_PPS_CONFIG0, BITS_PER_COMPONENT, p->bits_per_component));
    if (r->pixel_format == RD_DSC_PIXFMT_NATIVE_YCBCR420 || r->pixel_format == RD_DSC_PIXFMT_NATIVE_YCBCR422)
        temp_int = r->bpp_x32;
    else
        temp_int = r->bpp_x32 >> 1;
    W(regDSCC0_DSCC_PPS_CONFIG1, FIELD(DSCC0_DSCC_PPS_CONFIG1, BITS_PER_PIXEL, temp_int) |
                                 FIELD(DSCC0_DSCC_PPS_CONFIG1, SIMPLE_422, r->pixel_format == RD_DSC_PIXFMT_SIMPLE_YCBCR422) |
                                 FIELD(DSCC0_DSCC_PPS_CONFIG1, CONVERT_RGB, r->pixel_format == RD_DSC_PIXFMT_RGB) |
                                 FIELD(DSCC0_DSCC_PPS_CONFIG1, BLOCK_PRED_ENABLE, p->block_pred_enable ? 1 : 0) |
                                 FIELD(DSCC0_DSCC_PPS_CONFIG1, NATIVE_422, r->pixel_format == RD_DSC_PIXFMT_NATIVE_YCBCR422) |
                                 FIELD(DSCC0_DSCC_PPS_CONFIG1, NATIVE_420, r->pixel_format == RD_DSC_PIXFMT_NATIVE_YCBCR420) |
                                 FIELD(DSCC0_DSCC_PPS_CONFIG1, CHUNK_SIZE, p->slice_chunk_size));
    W(regDSCC0_DSCC_PPS_CONFIG2, FIELD(DSCC0_DSCC_PPS_CONFIG2, PIC_WIDTH, p->pic_width) |
                                 FIELD(DSCC0_DSCC_PPS_CONFIG2, PIC_HEIGHT, p->pic_height));
    W(regDSCC0_DSCC_PPS_CONFIG3, FIELD(DSCC0_DSCC_PPS_CONFIG3, SLICE_WIDTH, p->slice_width) |
                                 FIELD(DSCC0_DSCC_PPS_CONFIG3, SLICE_HEIGHT, p->slice_height));
    W(regDSCC0_DSCC_PPS_CONFIG4, FIELD(DSCC0_DSCC_PPS_CONFIG4, INITIAL_XMIT_DELAY, p->initial_xmit_delay));
    W(regDSCC0_DSCC_PPS_CONFIG5, FIELD(DSCC0_DSCC_PPS_CONFIG5, INITIAL_SCALE_VALUE, p->initial_scale_value) |
                                 FIELD(DSCC0_DSCC_PPS_CONFIG5, SCALE_INCREMENT_INTERVAL, p->scale_increment_interval));
    W(regDSCC0_DSCC_PPS_CONFIG6, FIELD(DSCC0_DSCC_PPS_CONFIG6, SCALE_DECREMENT_INTERVAL, p->scale_decrement_interval) |
                                 FIELD(DSCC0_DSCC_PPS_CONFIG6, FIRST_LINE_BPG_OFFSET, p->first_line_bpg_offset) |
                                 FIELD(DSCC0_DSCC_PPS_CONFIG6, SECOND_LINE_BPG_OFFSET, p->second_line_bpg_offset));
    W(regDSCC0_DSCC_PPS_CONFIG7, FIELD(DSCC0_DSCC_PPS_CONFIG7, NFL_BPG_OFFSET, p->nfl_bpg_offset) |
                                 FIELD(DSCC0_DSCC_PPS_CONFIG7, SLICE_BPG_OFFSET, p->slice_bpg_offset));
    W(regDSCC0_DSCC_PPS_CONFIG8, FIELD(DSCC0_DSCC_PPS_CONFIG8, NSL_BPG_OFFSET, p->nsl_bpg_offset) |
                                 FIELD(DSCC0_DSCC_PPS_CONFIG8, SECOND_LINE_OFFSET_ADJ, p->second_line_offset_adj));
    W(regDSCC0_DSCC_PPS_CONFIG9, FIELD(DSCC0_DSCC_PPS_CONFIG9, INITIAL_OFFSET, p->initial_offset) |
                                 FIELD(DSCC0_DSCC_PPS_CONFIG9, FINAL_OFFSET, p->final_offset));
    W(regDSCC0_DSCC_PPS_CONFIG10, FIELD(DSCC0_DSCC_PPS_CONFIG10, FLATNESS_MIN_QP, p->flatness_min_qp) |
                                  FIELD(DSCC0_DSCC_PPS_CONFIG10, FLATNESS_MAX_QP, p->flatness_max_qp) |
                                  FIELD(DSCC0_DSCC_PPS_CONFIG10, RC_MODEL_SIZE, p->rc_model_size));
    W(regDSCC0_DSCC_PPS_CONFIG11, FIELD(DSCC0_DSCC_PPS_CONFIG11, RC_EDGE_FACTOR, p->rc_edge_factor) |
                                  FIELD(DSCC0_DSCC_PPS_CONFIG11, RC_QUANT_INCR_LIMIT0, p->rc_quant_incr_limit0) |
                                  FIELD(DSCC0_DSCC_PPS_CONFIG11, RC_QUANT_INCR_LIMIT1, p->rc_quant_incr_limit1) |
                                  FIELD(DSCC0_DSCC_PPS_CONFIG11, RC_TGT_OFFSET_LO, p->rc_tgt_offset_low) |
                                  FIELD(DSCC0_DSCC_PPS_CONFIG11, RC_TGT_OFFSET_HI, p->rc_tgt_offset_high));
    W(regDSCC0_DSCC_PPS_CONFIG12, FIELD(DSCC0_DSCC_PPS_CONFIG12, RC_BUF_THRESH0, p->rc_buf_thresh[0]) |
                                  FIELD(DSCC0_DSCC_PPS_CONFIG12, RC_BUF_THRESH1, p->rc_buf_thresh[1]) |
                                  FIELD(DSCC0_DSCC_PPS_CONFIG12, RC_BUF_THRESH2, p->rc_buf_thresh[2]) |
                                  FIELD(DSCC0_DSCC_PPS_CONFIG12, RC_BUF_THRESH3, p->rc_buf_thresh[3]));
    W(regDSCC0_DSCC_PPS_CONFIG13, FIELD(DSCC0_DSCC_PPS_CONFIG13, RC_BUF_THRESH4, p->rc_buf_thresh[4]) |
                                  FIELD(DSCC0_DSCC_PPS_CONFIG13, RC_BUF_THRESH5, p->rc_buf_thresh[5]) |
                                  FIELD(DSCC0_DSCC_PPS_CONFIG13, RC_BUF_THRESH6, p->rc_buf_thresh[6]) |
                                  FIELD(DSCC0_DSCC_PPS_CONFIG13, RC_BUF_THRESH7, p->rc_buf_thresh[7]));
    W(regDSCC0_DSCC_PPS_CONFIG14, FIELD(DSCC0_DSCC_PPS_CONFIG14, RC_BUF_THRESH8, p->rc_buf_thresh[8]) |
                                  FIELD(DSCC0_DSCC_PPS_CONFIG14, RC_BUF_THRESH9, p->rc_buf_thresh[9]) |
                                  FIELD(DSCC0_DSCC_PPS_CONFIG14, RC_BUF_THRESH10, p->rc_buf_thresh[10]) |
                                  FIELD(DSCC0_DSCC_PPS_CONFIG14, RC_BUF_THRESH11, p->rc_buf_thresh[11]));
    W(regDSCC0_DSCC_PPS_CONFIG15, FIELD(DSCC0_DSCC_PPS_CONFIG15, RC_BUF_THRESH12, p->rc_buf_thresh[12]) |
                                  FIELD(DSCC0_DSCC_PPS_CONFIG15, RC_BUF_THRESH13, p->rc_buf_thresh[13]) |
                                  FIELD(DSCC0_DSCC_PPS_CONFIG15, RANGE_MIN_QP0, RP(0).range_min_qp) |
                                  FIELD(DSCC0_DSCC_PPS_CONFIG15, RANGE_MAX_QP0, RP(0).range_max_qp) |
                                  FIELD(DSCC0_DSCC_PPS_CONFIG15, RANGE_BPG_OFFSET0, RP(0).range_bpg_offset));
    /* DSCC_PPS_CONFIG16..22: two ranges each (1-2, 3-4, ..., 13-14) */
    for (i = 0; i < 7; i++) {
        uint32_t a = 1 + 2 * i, b = a + 1;
        uint32_t v = FIELD(DSCC0_DSCC_PPS_CONFIG16, RANGE_MIN_QP1, RP(a).range_min_qp) |
                     FIELD(DSCC0_DSCC_PPS_CONFIG16, RANGE_MAX_QP1, RP(a).range_max_qp) |
                     FIELD(DSCC0_DSCC_PPS_CONFIG16, RANGE_BPG_OFFSET1, RP(a).range_bpg_offset) |
                     FIELD(DSCC0_DSCC_PPS_CONFIG16, RANGE_MIN_QP2, RP(b).range_min_qp) |
                     FIELD(DSCC0_DSCC_PPS_CONFIG16, RANGE_MAX_QP2, RP(b).range_max_qp) |
                     FIELD(DSCC0_DSCC_PPS_CONFIG16, RANGE_BPG_OFFSET2, RP(b).range_bpg_offset);
        WI(d, regDSCC0_DSCC_PPS_CONFIG16, n * DSC_S + i, 1, v);
    }
#undef RP
#undef W
}

/* dsc2_set_config */
int dcn32_dsc_set_config(rdna3_dev *d, uint32_t inst, const rdna3_dsc_hw_config *cfg, rdna3_dsc_optc_config *optc)
{
    rdna3_dsc_reg_values rv;
    if (inst >= RD_DSC_ENGINES || !dcn32_dsc_prepare_config(cfg, &rv, optc))
        return RDNA3_EINVAL;
    RLOG(d, "dsc: DSC %u: %ux%u %u bpc, %u x %u slices, %u.%04u bpp, chunk %u, xmit delay %u, dec delay %u",
         inst, cfg->pic_width, cfg->pic_height, cfg->bpc, cfg->dc_dsc_cfg.num_slices_h, cfg->dc_dsc_cfg.num_slices_v,
         cfg->dc_dsc_cfg.bits_per_pixel / 16, (cfg->dc_dsc_cfg.bits_per_pixel % 16) * 10000 / 16,
         rv.pps.slice_chunk_size, rv.pps.initial_xmit_delay, rv.pps.initial_dec_delay);
    dsc_write_to_registers(d, inst, &rv);
    return RDNA3_OK;
}

/* dsc2_enable */
void dcn32_dsc_enable(rdna3_dev *d, uint32_t inst, uint32_t opp_pipe)
{
    uint32_t clk = GET(RI(d, regDSC_TOP0_DSC_TOP_CONTROL, inst, DSC_S), DSC_TOP0_DSC_TOP_CONTROL, DSC_CLOCK_EN);
    uint32_t fw = RI(d, regDSCRM0_DSCRM_DSC_FORWARD_CONFIG, inst, DSCRM_S), v;
    if ((clk || GET(fw, DSCRM0_DSCRM_DSC_FORWARD_CONFIG, DSCRM_DSC_FORWARD_EN)) &&
        GET(fw, DSCRM0_DSCRM_DSC_FORWARD_CONFIG, DSCRM_DSC_OPP_PIPE_SOURCE) != opp_pipe)
        RERR(d, "dsc: DSC %u already enabled at OPP %u", inst,
             GET(fw, DSCRM0_DSCRM_DSC_FORWARD_CONFIG, DSCRM_DSC_OPP_PIPE_SOURCE));
    UPDATE(d, regDSC_TOP0_DSC_TOP_CONTROL, inst, DSC_S, DSC_TOP0_DSC_TOP_CONTROL, DSC_CLOCK_EN, 1);
    v = RI(d, regDSCRM0_DSCRM_DSC_FORWARD_CONFIG, inst, DSCRM_S);
    v = SET(v, DSCRM0_DSCRM_DSC_FORWARD_CONFIG, DSCRM_DSC_FORWARD_EN, 1);
    v = SET(v, DSCRM0_DSCRM_DSC_FORWARD_CONFIG, DSCRM_DSC_OPP_PIPE_SOURCE, opp_pipe);
    WI(d, regDSCRM0_DSCRM_DSC_FORWARD_CONFIG, inst, DSCRM_S, v);
}

/* dsc2_disable */
void dcn32_dsc_disable(rdna3_dev *d, uint32_t inst)
{
    UPDATE(d, regDSCRM0_DSCRM_DSC_FORWARD_CONFIG, inst, DSCRM_S, DSCRM0_DSCRM_DSC_FORWARD_CONFIG, DSCRM_DSC_FORWARD_EN, 0);
    UPDATE(d, regDSC_TOP0_DSC_TOP_CONTROL, inst, DSC_S, DSC_TOP0_DSC_TOP_CONTROL, DSC_CLOCK_EN, 0);
}

/* dsc2_disconnect */
void dcn32_dsc_disconnect(rdna3_dev *d, uint32_t inst)
{
    UPDATE(d, regDSCRM0_DSCRM_DSC_FORWARD_CONFIG, inst, DSCRM_S, DSCRM0_DSCRM_DSC_FORWARD_CONFIG, DSCRM_DSC_FORWARD_EN, 0);
}

/* dsc2_wait_disconnect_pending_clear: the double-buffered disconnect taken
 * (REG_WAIT 2 us x 50000): RDNA3_OK or RDNA3_ETIME */
int dcn32_dsc_wait_disconnect_pending_clear(rdna3_dev *d, uint32_t inst)
{
    return rdna3_wait_reg(d, AI(d, regDSCRM0_DSCRM_DSC_FORWARD_CONFIG, inst, DSCRM_S), 0,
                          DSCRM0_DSCRM_DSC_FORWARD_CONFIG__DSCRM_DSC_DOUBLE_BUFFER_REG_UPDATE_PENDING_MASK, 0, 100000);
}

/* dsc2_read_state */
void dcn32_dsc_read_state(rdna3_dev *d, uint32_t inst, rdna3_dsc_state *s)
{
    uint32_t v;
    s->dsc_clock_en = GET(RI(d, regDSC_TOP0_DSC_TOP_CONTROL, inst, DSC_S), DSC_TOP0_DSC_TOP_CONTROL, DSC_CLOCK_EN);
    v = RI(d, regDSCC0_DSCC_PPS_CONFIG3, inst, DSC_S);
    s->dsc_slice_width = GET(v, DSCC0_DSCC_PPS_CONFIG3, SLICE_WIDTH);
    s->dsc_slice_height = GET(v, DSCC0_DSCC_PPS_CONFIG3, SLICE_HEIGHT);
    v = RI(d, regDSCC0_DSCC_PPS_CONFIG1, inst, DSC_S);
    s->dsc_bits_per_pixel = GET(v, DSCC0_DSCC_PPS_CONFIG1, BITS_PER_PIXEL);
    s->dsc_chunk_size = GET(v, DSCC0_DSCC_PPS_CONFIG1, CHUNK_SIZE);
    v = RI(d, regDSCC0_DSCC_PPS_CONFIG2, inst, DSC_S);
    s->dsc_pic_width = GET(v, DSCC0_DSCC_PPS_CONFIG2, PIC_WIDTH);
    s->dsc_pic_height = GET(v, DSCC0_DSCC_PPS_CONFIG2, PIC_HEIGHT);
    s->dsc_slice_bpg_offset = GET(RI(d, regDSCC0_DSCC_PPS_CONFIG7, inst, DSC_S), DSCC0_DSCC_PPS_CONFIG7, SLICE_BPG_OFFSET);
    v = RI(d, regDSCRM0_DSCRM_DSC_FORWARD_CONFIG, inst, DSCRM_S);
    s->dsc_fw_en = GET(v, DSCRM0_DSCRM_DSC_FORWARD_CONFIG, DSCRM_DSC_FORWARD_EN);
    s->dsc_opp_source = GET(v, DSCRM0_DSCRM_DSC_FORWARD_CONFIG, DSCRM_DSC_OPP_PIPE_SOURCE);
}

/* optc3_set_dsc_config: optc2_set_dsc_config, then V_SYNC_MODE 0 */
void dcn32_optc_set_dsc_config(rdna3_dev *d, uint32_t otg, uint32_t dsc_mode, uint32_t bytes_per_pixel,
                               uint32_t slice_width)
{
    UPDATE(d, regODM0_OPTC_DATA_FORMAT_CONTROL, otg, ODM_S, ODM0_OPTC_DATA_FORMAT_CONTROL, OPTC_DSC_MODE, dsc_mode);
    WI(d, regODM0_OPTC_BYTES_PER_PIXEL, otg, ODM_S, FIELD(ODM0_OPTC_BYTES_PER_PIXEL, OPTC_DSC_BYTES_PER_PIXEL,
                                                          bytes_per_pixel));
    UPDATE(d, regODM0_OPTC_WIDTH_CONTROL, otg, ODM_S, ODM0_OPTC_WIDTH_CONTROL, OPTC_DSC_SLICE_WIDTH, slice_width);
    UPDATE(d, regOTG0_OTG_V_SYNC_A_CNTL, otg, OTG_S, OTG0_OTG_V_SYNC_A_CNTL, OTG_V_SYNC_MODE, 0);
}

/* enc32_dp_set_dsc_config (DP_DSC_BYTES_PER_PIXEL / SLICE_WIDTH are gone on DCN 3.2) */
void dcn32_enc_dp_set_dsc_config(rdna3_dev *d, uint32_t fe, uint32_t dsc_mode)
{
    UPDATE(d, regDP0_DP_DSC_CNTL, fe, DP_S, DP0_DP_DSC_CNTL, DP_DSC_MODE, dsc_mode == RD_OPTC_DSC_DISABLED ? 0 : 1);
}

/* the register distance from VPG 0 to VPG 'vpg': 0..4 the DIG front ends'
 * (one stride), 6..9 the HPO DP stream encoders' (VPG[6 + n] -> HPO_DP[n],
 * dcn32_hpo_dp_stream_encoder_create; their own stride) */
static uint32_t vpg_delta(uint32_t vpg)
{
    if (vpg >= 6)
        return (uint32_t)(regVPG6_VPG_GENERIC_PACKET_ACCESS_CTRL - regVPG0_VPG_GENERIC_PACKET_ACCESS_CTRL) +
               (uint32_t)(regVPG7_VPG_GENERIC_PACKET_ACCESS_CTRL - regVPG6_VPG_GENERIC_PACKET_ACCESS_CTRL) * (vpg - 6);
    return VPG_S * vpg;
}

/* vpg3_update_generic_info_packet: header + 32 payload bytes of packet
 * 'index' into the VPG's generic packet memory (9 dwords per packet) */
static void vpg3_update_generic_info_packet(rdna3_dev *d, uint32_t vpg, uint32_t index, const uint8_t *hb,
                                            const uint8_t *sb, int immediate_update)
{
    uint32_t i, v;
    /* the hardware not reading the GSP memory */
    rdna3_wait_reg(d, AI(d, regVPG0_VPG_GENERIC_STATUS, 1, vpg_delta(vpg)), 0,
                   VPG0_VPG_GENERIC_STATUS__VPG_GENERIC_CONFLICT_OCCURED_MASK, 0, 500);
    UPDATE(d, regVPG0_VPG_GENERIC_STATUS, 1, vpg_delta(vpg), VPG0_VPG_GENERIC_STATUS, VPG_GENERIC_CONFLICT_CLR, 1);
    UPDATE(d, regVPG0_VPG_GENERIC_PACKET_ACCESS_CTRL, 1, vpg_delta(vpg), VPG0_VPG_GENERIC_PACKET_ACCESS_CTRL,
           VPG_GENERIC_DATA_INDEX, index * 9);
    WI(d, regVPG0_VPG_GENERIC_PACKET_DATA, 1, vpg_delta(vpg),
       (uint32_t)hb[0] | (uint32_t)hb[1] << 8 | (uint32_t)hb[2] << 16 | (uint32_t)hb[3] << 24);
    for (i = 0; i < 8; i++)
        WI(d, regVPG0_VPG_GENERIC_PACKET_DATA, 1, vpg_delta(vpg),
           (uint32_t)sb[4 * i] | (uint32_t)sb[4 * i + 1] << 8 | (uint32_t)sb[4 * i + 2] << 16 |
           (uint32_t)sb[4 * i + 3] << 24);
    /* the double-buffered packet taken at once or at the next frame */
    if (immediate_update) {
        v = RI(d, regVPG0_VPG_GSP_IMMEDIATE_UPDATE_CTRL, 1, vpg_delta(vpg));
        WI(d, regVPG0_VPG_GSP_IMMEDIATE_UPDATE_CTRL, 1, vpg_delta(vpg), v | (1u << index));   /* VPG_GENERICn_IMMEDIATE_UPDATE */
    } else {
        v = RI(d, regVPG0_VPG_GSP_FRAME_UPDATE_CTRL, 1, vpg_delta(vpg));
        WI(d, regVPG0_VPG_GSP_FRAME_UPDATE_CTRL, 1, vpg_delta(vpg), v | (1u << index));       /* VPG_GENERICn_FRAME_UPDATE */
    }
}

void dcn32_vpg_update_generic_info_packet(rdna3_dev *d, uint32_t vpg, uint32_t index, const uint8_t *hb,
                                          const uint8_t *sb, int immediate_update)
{
    vpg3_update_generic_info_packet(d, vpg, index, hb, sb, immediate_update);
}

/* enc3_dp_set_dsc_pps_info_packet: the PPS in GSP 11..14 (4 x 32 bytes),
 * sent every frame at line 2, VBID[6] at line 3 */
void dcn32_enc_dp_set_dsc_pps_info_packet(rdna3_dev *d, uint32_t fe, int enable, const uint8_t *pps,
                                          int immediate_update)
{
    if (enable && pps) {
        static const uint8_t hb[4] = { 0, 0x10, 127, 0 };   /* DC_DP_INFOFRAME_TYPE_PPS, 128 bytes - 1 */
        uint32_t i, v;
        /* PPS packet size (128 bytes) */
        UPDATE(d, regDP0_DP_SEC_CNTL2, fe, DP_S, DP0_DP_SEC_CNTL2, DP_SEC_GSP11_PPS, 1);
        /* the clock on before the AFMT block */
        UPDATE(d, regDIG0_AFMT_CNTL, fe, AFMT_S, DIG0_AFMT_CNTL, AFMT_AUDIO_CLOCK_EN, 1);
        for (i = 0; i < 4; i++)
            vpg3_update_generic_info_packet(d, fe, 11 + i, hb, pps + 32 * i, immediate_update);
        /* VBID[6] after the PPS line */
        UPDATE(d, regDP0_DP_GSP11_CNTL, fe, DP_S, DP0_DP_GSP11_CNTL, DP_SEC_GSP11_LINE_NUM, 2);
        v = RI(d, regDP0_DP_MSA_VBID_MISC, fe, DP_S);
        v = SET(v, DP0_DP_MSA_VBID_MISC, DP_VBID6_LINE_REFERENCE, 0);
        v = SET(v, DP0_DP_MSA_VBID_MISC, DP_VBID6_LINE_NUM, 3);
        WI(d, regDP0_DP_MSA_VBID_MISC, fe, DP_S, v);
        /* sent on every frame */
        UPDATE(d, regDP0_DP_GSP11_CNTL, fe, DP_S, DP0_DP_GSP11_CNTL, DP_SEC_GSP11_ENABLE, 1);
        UPDATE(d, regDP0_DP_SEC_CNTL, fe, DP_S, DP0_DP_SEC_CNTL, DP_SEC_STREAM_ENABLE, 1);
    } else {
        UPDATE(d, regDP0_DP_GSP11_CNTL, fe, DP_S, DP0_DP_GSP11_CNTL, DP_SEC_GSP11_ENABLE, 0);
        UPDATE(d, regDP0_DP_SEC_CNTL2, fe, DP_S, DP0_DP_SEC_CNTL2, DP_SEC_GSP11_PPS, 0);
    }
}
