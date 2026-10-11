/*
 * dcn32_audio.c - see dcn32_audio.h. Ported from Linux amdgpu Display Core
 * (v6.12): dce/dce_audio.c, dcn30/dcn30_afmt.c, dcn31/dcn31_apg.c,
 * dio/dcn10/dcn10_stream_encoder.c, dio/dcn30/dcn30_dio_stream_encoder.c,
 * hpo/dcn31/dcn31_hpo_dp_stream_encoder.c, link/hwss/link_hwss_dio.c,
 * link/hwss/link_hwss_hpo_dp.c, hwss/dce110/dce110_hwseq.c,
 * core/dc_resource.c, core/dc_stream.c, clk_mgr/dcn32 (the PME
 * workaround), MIT, Copyright Advanced Micro Devices, Inc. Every function
 * names the one it ports; REG_UPDATE / REG_SET become read-modify-writes.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_audio.h"
#include "dcn32_clk.h"
#include "dcn32_hpo.h"
#include "fixpt31_32.h"
#include "gen/dcn_3_2_0_audio.h"
#include "gen/dcn_3_2_0_hdmi.h"
#include "gen/dcn_3_2_0_hpo.h"

#define FLD(reg, field, x) (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK)
#define MSK(reg, field) ((uint32_t)reg##__##field##_MASK)
#define GETF(v, reg, field) (((v) & (uint32_t)reg##__##field##_MASK) >> reg##__##field##__SHIFT)

/* one DIG front end's AFMT / HDMI / DP_SEC registers every 0x100 */
#define FE_S  (regDIG1_AFMT_CNTL - regDIG0_AFMT_CNTL)
/* the azalia endpoints: AZF0ENDPOINTn, 6 registers apart */
#define AZ_S  (regAZF0ENDPOINT1_AZALIA_F0_CODEC_ENDPOINT_INDEX - regAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX)
/* the HPO stream encoders and their APGs */
#define HPO_S (regDP_STREAM_ENC1_DP_STREAM_ENC_CLOCK_CONTROL - regDP_STREAM_ENC0_DP_STREAM_ENC_CLOCK_CONTROL)
#define APG_S (regAPG1_APG_CONTROL - regAPG0_APG_CONTROL)

/* register 'reg' of instance n with stride s */
#define A(d, reg, n, s) (rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + (uint32_t)(s) * (n))

/* DCN32 resource: the DCCG audio DTO select values */
#define RD_DCCG_AUDIO_DTO_SEL_DTO0          0
#define RD_DCCG_AUDIO_DTO_SEL_DTO1          1
#define RD_DCCG_AUDIO_DTO_SEL_NO_AUDIO_DTO  3
#define RD_DCCG_AUDIO_DTO_SEL_DTBCLK        4
/* dcn32_smu13_driver_if / dalsmc.h */
#define RD_DALSMC_MSG_BacoAudioD3PME        0x11

/* REG_UPDATE_n: one read-modify-write of the fields in 'mask' */
static void upd(rdna3_dev *d, uint32_t a, uint32_t mask, uint32_t v)
{
    rdna3_wreg(d, a, (rdna3_rreg(d, a) & ~mask) | (v & mask));
}

/* ==================== dc_stream.c / dm_helpers ==================== */

/* dc_stream_construct: the audio_info from edid_caps (dm_helpers_parse_edid_caps:
 * manufacturer_id = mfg_id[0] | mfg_id[1] << 8, product_id = the product
 * code, the monitor name, the SADs, speaker_flags) */
void dcn32_audio_info_from_edid(const rdna3_edid *e, rdna3_audio_info *ai)
{
    uint32_t i, mfg;
    for (i = 0; i < sizeof(*ai); i++)
        ((uint8_t *)ai)[i] = 0;
    if (!e)
        return;
    /* the EDID's big-endian PNP word, read back as DRM's mfg_id[0..1] */
    mfg = ((uint32_t)(e->manufacturer[0] - '@') & 0x1f) << 10 | ((uint32_t)(e->manufacturer[1] - '@') & 0x1f) << 5 |
          ((uint32_t)(e->manufacturer[2] - '@') & 0x1f);
    ai->manufacture_id = (mfg >> 8) | (mfg & 0xff) << 8;
    ai->product_id = e->product;
    for (i = 0; i < RD_AUDIO_DISPLAY_NAME_SIZE && i < sizeof(e->name) && e->name[i]; i++)
        ai->display_name[i] = (uint8_t)e->name[i];
    for (i = 0; i < e->audio_mode_count && i < RD_EDID_MAX_AUDIO; i++) {
        ai->modes[i].channel_count = e->audio_modes[i].channel_count;
        ai->modes[i].format_code = e->audio_modes[i].format_code;
        ai->modes[i].sample_rates = e->audio_modes[i].sample_rate;
        ai->modes[i].sample_size = e->audio_modes[i].sample_size;
    }
    ai->mode_count = i;
    /* no latency in dm_helpers_parse_edid_caps' caps */
    ai->audio_latency = ai->video_latency = 0;
    ai->flags = e->speaker_flags;
    /* "WindowDM has implemented, other DMs need Unhardcode port_id" */
    ai->port_id[0] = 0x5558859e;
    ai->port_id[1] = 0xd989449;
}

/* ==================== dcn10_stream_encoder.c: the ACR table ==================== */

/* audio_clock_info_table, audio_clock_info_table_36bpc, _48bpc: pixel
 * clock (10 kHz), N / CTS for 32, 44.1, 48 kHz (HDMI 1.4 tables 7-1..7-3) */
static const rdna3_audio_clock_info audio_clock_info_table[16] = {
    { 2517, 4576, 28125, 7007, 31250, 6864, 28125 },
    { 2518, 4576, 28125, 7007, 31250, 6864, 28125 },
    { 2520, 4096, 25200, 6272, 28000, 6144, 25200 },
    { 2700, 4096, 27000, 6272, 30000, 6144, 27000 },
    { 2702, 4096, 27027, 6272, 30030, 6144, 27027 },
    { 2703, 4096, 27027, 6272, 30030, 6144, 27027 },
    { 5400, 4096, 54000, 6272, 60000, 6144, 54000 },
    { 5405, 4096, 54054, 6272, 60060, 6144, 54054 },
    { 7417, 11648, 210937, 17836, 234375, 11648, 140625 },
    { 7425, 4096, 74250, 6272, 82500, 6144, 74250 },
    { 14835, 11648, 421875, 8918, 234375, 5824, 140625 },
    { 14850, 4096, 148500, 6272, 165000, 6144, 148500 },
    { 29670, 5824, 421875, 4459, 234375, 5824, 281250 },
    { 29700, 3072, 222750, 4704, 247500, 5120, 247500 },
    { 59340, 5824, 843750, 8918, 937500, 5824, 562500 },
    { 59400, 3072, 445500, 9408, 990000, 6144, 594000 },
};

static const rdna3_audio_clock_info audio_clock_info_table_36bpc[14] = {
    { 2517, 9152, 84375, 7007, 48875, 9152, 56250 },
    { 2518, 9152, 84375, 7007, 48875, 9152, 56250 },
    { 2520, 4096, 37800, 6272, 42000, 6144, 37800 },
    { 2700, 4096, 40500, 6272, 45000, 6144, 40500 },
    { 2702, 8192, 81081, 6272, 45045, 8192, 54054 },
    { 2703, 8192, 81081, 6272, 45045, 8192, 54054 },
    { 5400, 4096, 81000, 6272, 90000, 6144, 81000 },
    { 5405, 4096, 81081, 6272, 90090, 6144, 81081 },
    { 7417, 11648, 316406, 17836, 351562, 11648, 210937 },
    { 7425, 4096, 111375, 6272, 123750, 6144, 111375 },
    { 14835, 11648, 632812, 17836, 703125, 11648, 421875 },
    { 14850, 4096, 222750, 6272, 247500, 6144, 222750 },
    { 29670, 5824, 632812, 8918, 703125, 5824, 421875 },
    { 29700, 4096, 445500, 4704, 371250, 5120, 371250 },
};

static const rdna3_audio_clock_info audio_clock_info_table_48bpc[14] = {
    { 2517, 4576, 56250, 7007, 62500, 6864, 56250 },
    { 2518, 4576, 56250, 7007, 62500, 6864, 56250 },
    { 2520, 4096, 50400, 6272, 56000, 6144, 50400 },
    { 2700, 4096, 54000, 6272, 60000, 6144, 54000 },
    { 2702, 4096, 54054, 6267, 60060, 8192, 54054 },
    { 2703, 4096, 54054, 6272, 60060, 8192, 54054 },
    { 5400, 4096, 108000, 6272, 120000, 6144, 108000 },
    { 5405, 4096, 108108, 6272, 120120, 6144, 108108 },
    { 7417, 11648, 421875, 17836, 468750, 11648, 281250 },
    { 7425, 4096, 148500, 6272, 165000, 6144, 148500 },
    { 14835, 11648, 843750, 8918, 468750, 11648, 281250 },
    { 14850, 4096, 297000, 6272, 330000, 6144, 297000 },
    { 29670, 5824, 843750, 4459, 468750, 5824, 562500 },
    { 29700, 3072, 445500, 4704, 495000, 5120, 495000 },
};

/* get_audio_clock_info */
void dcn32_get_audio_clock_info(uint32_t color_depth, uint32_t crtc_pixel_clock_100hz,
                                uint32_t actual_pixel_clock_100hz, rdna3_audio_clock_info *out)
{
    const rdna3_audio_clock_info *clock_info;
    uint32_t index, audio_array_size, crtc_pixel_clock_in_10khz = crtc_pixel_clock_100hz / 100;

    switch (color_depth) {
    case RD_COLOR_DEPTH_161616:
        clock_info = audio_clock_info_table_48bpc;
        audio_array_size = sizeof(audio_clock_info_table_48bpc) / sizeof(audio_clock_info_table_48bpc[0]);
        break;
    case RD_COLOR_DEPTH_121212:
        clock_info = audio_clock_info_table_36bpc;
        audio_array_size = sizeof(audio_clock_info_table_36bpc) / sizeof(audio_clock_info_table_36bpc[0]);
        break;
    default:
        clock_info = audio_clock_info_table;
        audio_array_size = sizeof(audio_clock_info_table) / sizeof(audio_clock_info_table[0]);
        break;
    }
    /* search for the exact pixel clock in the table */
    for (index = 0; index < audio_array_size; index++) {
        if (clock_info[index].pixel_clock_in_10khz > crtc_pixel_clock_in_10khz)
            break;   /* not match */
        if (clock_info[index].pixel_clock_in_10khz == crtc_pixel_clock_in_10khz) {
            *out = clock_info[index];
            return;
        }
    }
    /* not found: the HDMI spec's "Other" pixel clock entry */
    if (actual_pixel_clock_100hz == 0)
        actual_pixel_clock_100hz = crtc_pixel_clock_100hz;
    out->pixel_clock_in_10khz = actual_pixel_clock_100hz / 100;
    out->cts_32khz = actual_pixel_clock_100hz / 10;
    out->cts_44khz = actual_pixel_clock_100hz / 10;
    out->cts_48khz = actual_pixel_clock_100hz / 10;
    out->n_32khz = 4096;
    out->n_44khz = 6272;
    out->n_48khz = 6144;
}

/* speakers_to_channels (dcn30_afmt.c, the same as dcn10's): union
 * audio_cea_channels FL 0, FR 1, LFE 2, FC 3, RL_RC 4, RR 5, RC_RLC_FLC 6,
 * RRC_FRC 7 */
uint32_t dcn32_audio_speakers_to_channels(uint32_t sf)
{
    uint32_t ch = 0;
#define B(x) (((sf) & (x)) ? 1u : 0u)
    /* these are one to one */
    ch |= B(RD_SPK_FL_FR) << 0 | B(RD_SPK_FL_FR) << 1 | B(RD_SPK_LFE) << 2 | B(RD_SPK_FC) << 3;
    /* if Rear Left and Right exist move RC speaker to channel 7, otherwise to channel 5 */
    if (sf & RD_SPK_RL_RR) {
        ch |= 1u << 4 | 1u << 5;
        ch = (ch & ~(1u << 6)) | B(RD_SPK_RC) << 6;
    } else {
        ch = (ch & ~(1u << 4)) | B(RD_SPK_RC) << 4;
    }
    /* FRONT Left Right Center and REAR Left Right Center are exclusive */
    if (sf & RD_SPK_FLC_FRC) {
        ch = (ch & ~(3u << 6)) | 1u << 6 | 1u << 7;
    } else {
        ch = (ch & ~(3u << 6)) | B(RD_SPK_RLC_RRC) << 6 | B(RD_SPK_RLC_RRC) << 7;
    }
#undef B
    return ch;
}

/* ==================== dce_audio.c ==================== */

/* write_indirect_azalia_reg / read_indirect_azalia_reg */
void dcn32_aud_write(rdna3_dev *d, uint32_t inst, uint32_t index, uint32_t v)
{
    rdna3_wreg(d, A(d, regAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX, inst, AZ_S),
               FLD(AZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX, AZALIA_ENDPOINT_REG_INDEX, index));
    rdna3_wreg(d, A(d, regAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_DATA, inst, AZ_S), v);
}

uint32_t dcn32_aud_read(rdna3_dev *d, uint32_t inst, uint32_t index)
{
    rdna3_wreg(d, A(d, regAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX, inst, AZ_S),
               FLD(AZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX, AZALIA_ENDPOINT_REG_INDEX, index));
    return rdna3_rreg(d, A(d, regAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_DATA, inst, AZ_S));
}

/* is_audio_format_supported: the mode of 'code' with the most channels */
static int is_audio_format_supported(const rdna3_audio_info *info, uint32_t code, uint32_t *format_index)
{
    uint32_t index, max_channel_index = 0;
    int found = 0;
    if (!info)
        return 0;
    for (index = 0; index < info->mode_count; index++) {
        if (info->modes[index].format_code != code)
            continue;
        if (found) {
            /* format has multiple entries: the one with the most channels */
            if (info->modes[index].channel_count > info->modes[max_channel_index].channel_count)
                max_channel_index = index;
        } else {
            found = 1;
            max_channel_index = index;
        }
    }
    if (found && format_index)
        *format_index = max_channel_index;
    return found;
}

/* check_audio_bandwidth_hdmi: HDMI 1.3 table 7.5 and the audio packets
 * the horizontal blank holds */
static uint32_t check_audio_bandwidth_hdmi(const rdna3_audio_crtc_info *c, uint32_t channel_count, uint32_t rates)
{
    uint32_t samples, h_blank;
    int lim48 = 0, lim88 = 0, lim96 = 0, lim174 = 0;
    if (!c)
        return rates;
    /* for two channels: whatever the sink supports */
    if (channel_count > 2) {
        if (c->requested_pixel_clock_100hz <= 270000 && c->v_active <= 576 && !c->interlaced &&
            !(c->pixel_repetition == 2 || c->pixel_repetition == 4))
            lim48 = 1;
        else if (c->requested_pixel_clock_100hz <= 270000 && c->v_active <= 576 && c->interlaced &&
                 c->pixel_repetition == 2)
            lim88 = 1;
        else if (c->requested_pixel_clock_100hz <= 540000 && c->v_active <= 576 && !c->interlaced)
            lim174 = 1;
    }
    /* the audio bandwidth of the 8 ch layout (layout 1) */
    h_blank = c->h_total - c->h_active;
    if (c->pixel_repetition)
        h_blank *= c->pixel_repetition;
    h_blank -= 58;   /* HDMI 1.3 table 7.5 */
    h_blank -= 16;   /* control period */
    samples = h_blank * 10;
    samples /= 32;   /* audio packets (x 10) per line */
    samples *= c->v_active;
    samples *= c->refresh_rate;   /* samples x 10 per second */
    samples /= 10;
    /* deep color: the link runs faster than the pixel rate */
    switch (c->color_depth) {
    case RD_COLOR_DEPTH_888: samples *= 4; break;
    case RD_COLOR_DEPTH_101010: samples *= 5; break;
    case RD_COLOR_DEPTH_121212: samples *= 6; break;
    default: samples *= 4; break;
    }
    samples /= 4;
    if (samples < 88200)
        lim48 = 1;
    else if (samples < 96000)
        lim88 = 1;
    else if (samples < 176400)
        lim96 = 1;
    else if (samples < 192000)
        lim174 = 1;
    if (lim174)
        rates &= ~RD_AUDIO_RATE_192;
    if (lim96)
        rates &= ~(RD_AUDIO_RATE_192 | RD_AUDIO_RATE_176_4);
    if (lim88)
        rates &= ~(RD_AUDIO_RATE_192 | RD_AUDIO_RATE_176_4 | RD_AUDIO_RATE_96);
    if (lim48)
        rates &= ~(RD_AUDIO_RATE_192 | RD_AUDIO_RATE_176_4 | RD_AUDIO_RATE_96 | RD_AUDIO_RATE_88_2);
    return rates;
}

/* get_link_symbol_clk_freq_mhz */
static rd_fixpt get_link_symbol_clk_freq_mhz(uint32_t link_rate)
{
    switch (link_rate) {
    case RD_DP_LINK_RATE_LOW: return dc_fixpt_from_int(162);
    case RD_DP_LINK_RATE_HIGH: return dc_fixpt_from_int(270);
    case RD_DP_LINK_RATE_HIGH2: return dc_fixpt_from_int(540);
    case RD_DP_LINK_RATE_HIGH3: return dc_fixpt_from_int(810);
    case RD_DP_LINK_RATE_UHBR10: return dc_fixpt_from_fraction(3125, 10);
    case RD_DP_LINK_RATE_UHBR13_5: return dc_fixpt_from_fraction(421875, 1000);
    case RD_DP_LINK_RATE_UHBR20: return dc_fixpt_from_int(625);
    default: return dc_fixpt_from_int(0);   /* unexpected */
    }
}

/* struct dp_audio_layout_config, get_audio_layout_config: L-PCM, one
 * (8 ch sized) layout per SDP */
typedef struct { uint32_t layouts_per_sample_denom, symbols_per_layout, max_layouts_per_audio_sdp; } dp_audio_layout;

static void get_audio_layout_config(uint32_t channel_count, uint32_t encoding, dp_audio_layout *o)
{
    uint32_t sym = encoding == RD_DP_8b_10b_ENCODING ? 40 : encoding == RD_DP_128b_132b_ENCODING ? 10 : 0;
    if (!sym)
        return;
    if (channel_count == 2) {
        o->layouts_per_sample_denom = 4;
        o->symbols_per_layout = sym;
        o->max_layouts_per_audio_sdp = 1;
    } else if (channel_count == 8 || channel_count == 6) {
        o->layouts_per_sample_denom = 1;
        o->symbols_per_layout = sym;
        o->max_layouts_per_audio_sdp = 1;
    }
}

/* get_av_stream_map_lane_count */
static uint32_t get_av_stream_map_lane_count(uint32_t encoding, uint32_t lane_count, int is_mst)
{
    if (encoding == RD_DP_8b_10b_ENCODING)
        return is_mst ? 4 : lane_count;
    if (encoding == RD_DP_128b_132b_ENCODING)
        return 4;
    return 0;
}

/* get_audio_sdp_overhead */
static uint32_t get_audio_sdp_overhead(uint32_t encoding, uint32_t lane_count, int is_mst)
{
    if (encoding == RD_DP_8b_10b_ENCODING)
        return is_mst ? 16 /* 4 * 2 + 8 */ : lane_count * 2 + 8;
    if (encoding == RD_DP_128b_132b_ENCODING)
        return 10;   /* 4 x 2.5 */
    return 0;
}

/* calculate_required_audio_bw_in_symbols */
static uint32_t calculate_required_audio_bw_in_symbols(const rdna3_audio_crtc_info *c, const dp_audio_layout *lc,
                                                       uint32_t sample_rate_hz, uint32_t av_lanes, uint32_t sdp_overhead)
{
    /* DP spec recommends a 1.05 to 1.1 safety margin against sample under-run */
    rd_fixpt margin = dc_fixpt_from_fraction(110, 100);
    rd_fixpt line_khz = dc_fixpt_from_fraction(c->requested_pixel_clock_100hz, (int64_t)c->h_total * 10);
    rd_fixpt samples_per_line, layouts_per_line, sym_per_sdp, rem;
    uint32_t num_sdp, req;

    if (!av_lanes)
        return 0;
    samples_per_line = dc_fixpt_from_fraction(sample_rate_hz, 1000);
    samples_per_line = dc_fixpt_div(samples_per_line, line_khz);
    layouts_per_line = dc_fixpt_div_int(samples_per_line, lc->layouts_per_sample_denom);
    num_sdp = (uint32_t)dc_fixpt_floor(dc_fixpt_div_int(layouts_per_line, lc->max_layouts_per_audio_sdp));
    sym_per_sdp = dc_fixpt_from_int(lc->max_layouts_per_audio_sdp * lc->symbols_per_layout);
    sym_per_sdp = dc_fixpt_add_int(sym_per_sdp, sdp_overhead);
    sym_per_sdp = dc_fixpt_mul(sym_per_sdp, margin);
    req = num_sdp;
    req *= (((uint32_t)dc_fixpt_ceil(sym_per_sdp) + av_lanes) / av_lanes) * av_lanes;
    if (num_sdp != (uint32_t)dc_fixpt_ceil(dc_fixpt_div_int(layouts_per_line, lc->max_layouts_per_audio_sdp))) {
        rem = dc_fixpt_sub_int(layouts_per_line, num_sdp * lc->max_layouts_per_audio_sdp);
        rem = dc_fixpt_mul_int(rem, lc->symbols_per_layout);
        rem = dc_fixpt_add_int(rem, sdp_overhead);
        rem = dc_fixpt_mul(rem, margin);
        req += (((uint32_t)dc_fixpt_ceil(rem) + av_lanes) / av_lanes) * av_lanes;
    }
    return req;
}

/* calculate_available_hblank_bw_in_symbols (8b/10b MST, 128b/132b SST / MST) */
static uint32_t calculate_available_hblank_bw_in_symbols(const rdna3_audio_crtc_info *c,
                                                         const rdna3_audio_dp_link_info *dp)
{
    uint64_t hblank = c->h_total - c->h_active;
    rd_fixpt hblank_ms = dc_fixpt_from_fraction((int64_t)hblank * 10, c->requested_pixel_clock_100hz);
    rd_fixpt lsclk_mhz = get_link_symbol_clk_freq_mhz(dp->link_rate);
    rd_fixpt bpp, peak_kbps, link_kbps, frac, count;
    uint32_t avail;

    if (c->dsc_bits_per_pixel) {
        bpp = dc_fixpt_from_fraction(c->dsc_bits_per_pixel, 16);
    } else {
        switch (c->color_depth) {
        case RD_COLOR_DEPTH_666: bpp = dc_fixpt_from_int(6); break;
        case RD_COLOR_DEPTH_888: bpp = dc_fixpt_from_int(8); break;
        case RD_COLOR_DEPTH_101010: bpp = dc_fixpt_from_int(10); break;
        case RD_COLOR_DEPTH_121212: bpp = dc_fixpt_from_int(12); break;
        default: bpp = dc_fixpt_from_int(8); break;
        }
        bpp = dc_fixpt_mul_int(bpp, 3);
        if (c->pixel_encoding == RD_PIXEL_ENCODING_YCBCR422) {
            bpp = dc_fixpt_div_int(bpp, 3);
            bpp = dc_fixpt_mul_int(bpp, 2);
        } else if (c->pixel_encoding == RD_PIXEL_ENCODING_YCBCR420) {
            bpp = dc_fixpt_div_int(bpp, 2);
        }
    }
    /* the main link overhead is in the audio side of the budget */
    peak_kbps = dc_fixpt_from_fraction(c->requested_pixel_clock_100hz, 10);
    peak_kbps = dc_fixpt_mul(peak_kbps, bpp);
    link_kbps = dc_fixpt_from_int(dp->link_bandwidth_kbps);
    frac = dc_fixpt_div(peak_kbps, link_kbps);
    count = dc_fixpt_mul_int(hblank_ms, 1000);
    count = dc_fixpt_mul(count, lsclk_mhz);
    count = dc_fixpt_mul(count, frac);
    avail = (uint32_t)dc_fixpt_floor(count);
    avail *= dp->lane_count;
    avail -= c->dsc_num_slices * 4;   /* EOC overhead */
    if (avail < dp->hblank_min_symbol_width)
        avail = 4 * dp->hblank_min_symbol_width;   /* each symbol takes 4 frames */
    if (avail < 12)
        avail = 0;
    else
        avail -= 12;   /* main link overhead */
    return avail;
}

/* check_audio_bandwidth_dp */
static uint32_t check_audio_bandwidth_dp(const rdna3_audio_crtc_info *c, const rdna3_audio_dp_link_info *dp,
                                         uint32_t channel_count, uint32_t rates)
{
    static const struct { uint32_t hz, bit; } r[] = {
        { 192000, RD_AUDIO_RATE_192 }, { 176400, RD_AUDIO_RATE_176_4 }, { 96000, RD_AUDIO_RATE_96 },
        { 88200, RD_AUDIO_RATE_88_2 }, { 48000, RD_AUDIO_RATE_48 }, { 44100, RD_AUDIO_RATE_44_1 },
        { 32000, RD_AUDIO_RATE_32 },
    };
    dp_audio_layout lc = { 0, 0, 0 };
    uint32_t avail, lanes, ovh, i;
    /* "TODO: Add validation for SST 8b/10 case" */
    if (!c || !dp || (!dp->is_mst && dp->encoding == RD_DP_8b_10b_ENCODING))
        return rates;
    avail = calculate_available_hblank_bw_in_symbols(c, dp);
    lanes = get_av_stream_map_lane_count(dp->encoding, dp->lane_count, dp->is_mst);
    ovh = get_audio_sdp_overhead(dp->encoding, dp->lane_count, dp->is_mst);
    get_audio_layout_config(channel_count, dp->encoding, &lc);
    if (!lc.max_layouts_per_audio_sdp || !lc.symbols_per_layout || !lc.layouts_per_sample_denom)
        return rates;
    for (i = 0; i < sizeof(r) / sizeof(r[0]); i++)
        if (avail < calculate_required_audio_bw_in_symbols(c, &lc, r[i].hz, lanes, ovh))
            rates &= ~r[i].bit;
    return rates;
}

/* check_audio_bandwidth */
uint32_t dcn32_audio_check_bandwidth(const rdna3_audio_crtc_info *crtc, const rdna3_audio_dp_link_info *dp,
                                     uint32_t channel_count, uint32_t signal, uint32_t sample_rates)
{
    switch (signal) {
    case RD_SIGNAL_HDMI_TYPE_A:
        return check_audio_bandwidth_hdmi(crtc, channel_count, sample_rates);
    case RD_SIGNAL_DISPLAY_PORT:
    case RD_SIGNAL_DISPLAY_PORT_MST:
        return check_audio_bandwidth_dp(crtc, dp, channel_count, sample_rates);
    default:
        return sample_rates;
    }
}

/* set_high_bit_rate_capable: expose / hide the HBR capability */
static void set_high_bit_rate_capable(rdna3_dev *d, uint32_t inst, int capable)
{
    uint32_t v = dcn32_aud_read(d, inst, RD_AZ_RESPONSE_HBR);
    v = (v & ~RD_AZ_HBR_CAPABLE) | (capable ? RD_AZ_HBR_CAPABLE : 0);
    dcn32_aud_write(d, inst, RD_AZ_RESPONSE_HBR, v);
}

/* set_video_latency (ms / 2 + 1; out of range: left) */
static void set_video_latency(rdna3_dev *d, uint32_t inst, int latency_in_ms)
{
    uint32_t v;
    if (latency_in_ms < 0 || latency_in_ms > 255)
        return;
    v = dcn32_aud_read(d, inst, RD_AZ_RESPONSE_LIPSYNC);
    v = (v & ~0xffu) | (uint32_t)latency_in_ms;   /* VIDEO_LIPSYNC */
    dcn32_aud_write(d, inst, RD_AZ_RESPONSE_LIPSYNC, v);
}

/* set_audio_latency (clamped to 0..255) */
static void set_audio_latency(rdna3_dev *d, uint32_t inst, int latency_in_ms)
{
    uint32_t v;
    if (latency_in_ms < 0)
        latency_in_ms = 0;
    if (latency_in_ms > 255)
        latency_in_ms = 255;
    v = dcn32_aud_read(d, inst, RD_AZ_RESPONSE_LIPSYNC);
    v = (v & ~0xff00u) | (uint32_t)latency_in_ms << 8;   /* AUDIO_LIPSYNC */
    dcn32_aud_write(d, inst, RD_AZ_RESPONSE_LIPSYNC, v);
}

/* dce_aud_az_enable */
void dcn32_aud_az_enable(rdna3_dev *d, uint32_t inst)
{
    uint32_t v = dcn32_aud_read(d, inst, RD_AZ_HOT_PLUG_CONTROL);
    v |= RD_AZ_HPC_CLOCK_GATING_DISABLE | RD_AZ_HPC_AUDIO_ENABLED;
    dcn32_aud_write(d, inst, RD_AZ_HOT_PLUG_CONTROL, v);
    v &= ~RD_AZ_HPC_CLOCK_GATING_DISABLE;
    dcn32_aud_write(d, inst, RD_AZ_HOT_PLUG_CONTROL, v);
    RDBG(d, "audio: endpoint %u enabled: 0x%x", inst, v);
}

/* dce_aud_az_disable_hbr_audio */
void dcn32_aud_az_disable_hbr_audio(rdna3_dev *d, uint32_t inst)
{
    set_high_bit_rate_capable(d, inst, 0);
}

/* dce_aud_az_disable */
void dcn32_aud_az_disable(rdna3_dev *d, uint32_t inst)
{
    uint32_t v = dcn32_aud_read(d, inst, RD_AZ_HOT_PLUG_CONTROL);
    v |= RD_AZ_HPC_CLOCK_GATING_DISABLE;
    dcn32_aud_write(d, inst, RD_AZ_HOT_PLUG_CONTROL, v);
    v &= ~RD_AZ_HPC_AUDIO_ENABLED;
    dcn32_aud_write(d, inst, RD_AZ_HOT_PLUG_CONTROL, v);
    v &= ~RD_AZ_HPC_CLOCK_GATING_DISABLE;
    dcn32_aud_write(d, inst, RD_AZ_HOT_PLUG_CONTROL, v);
    RDBG(d, "audio: endpoint %u disabled: 0x%x", inst, dcn32_aud_read(d, inst, RD_AZ_HOT_PLUG_CONTROL));
}

/* dce_aud_az_configure: what the codec reports for the pin (the ELD) */
void dcn32_aud_az_configure(rdna3_dev *d, uint32_t inst, uint32_t signal, const rdna3_audio_crtc_info *crtc,
                            const rdna3_audio_info *info, const rdna3_audio_dp_link_info *dp)
{
    uint32_t speakers = info->flags & 0x7fu, v, field, format_index, index, code, strlen = 0, i;
    int is_ac3_supported = 0;

    /* SIGNAL_TYPE_VIRTUAL never reaches this port */
    v = dcn32_aud_read(d, inst, RD_AZ_HOT_PLUG_CONTROL);
    v |= RD_AZ_HPC_CLOCK_GATING_DISABLE;
    dcn32_aud_write(d, inst, RD_AZ_HOT_PLUG_CONTROL, v);

    /* speaker allocation, LFE_PLAYBACK_LEVEL 0, the connection type */
    v = dcn32_aud_read(d, inst, RD_AZ_CHANNEL_SPEAKER);
    v = (v & ~RD_AZ_CS_SPEAKER_ALLOCATION) | speakers;
    v &= ~(RD_AZ_CS_LFE_PLAYBACK_LEVEL | RD_AZ_CS_HDMI_CONNECTION | RD_AZ_CS_DP_CONNECTION);
    field = (v & RD_AZ_CS_EXTRA_CONNECTION_INFO) >> 18;
    field &= ~0x1u;
    v = (v & ~RD_AZ_CS_EXTRA_CONNECTION_INFO) | (field << 18 & RD_AZ_CS_EXTRA_CONNECTION_INFO);
    switch (signal) {
    case RD_SIGNAL_HDMI_TYPE_A:
        v |= RD_AZ_CS_HDMI_CONNECTION;
        break;
    case RD_SIGNAL_DISPLAY_PORT:
    case RD_SIGNAL_DISPLAY_PORT_MST:
        v |= RD_AZ_CS_DP_CONNECTION;
        break;
    default:
        break;
    }
    dcn32_aud_write(d, inst, RD_AZ_CHANNEL_SPEAKER, v);

    /* ACP data: supports AI */
    v = dcn32_aud_read(d, inst, RD_AZ_ACP_DATA);
    v = (v & ~RD_AZ_ACP_SUPPORTS_AI) | ((info->flags & RD_SPK_SUPPORT_AI) ? RD_AZ_ACP_SUPPORTS_AI : 0);
    dcn32_aud_write(d, inst, RD_AZ_ACP_DATA, v);

    /* the audio descriptors: every format code but 1-bit audio and DST */
    for (format_index = 0; format_index < RD_AUDIO_FORMAT_CODE_COUNT; format_index++) {
        code = RD_AUDIO_FORMAT_CODE_LINEARPCM + format_index;
        if (code == RD_AUDIO_FORMAT_CODE_1BITAUDIO || code == RD_AUDIO_FORMAT_CODE_DST)
            continue;
        v = 0;
        if (is_audio_format_supported(info, code, &index)) {
            const rdna3_audio_mode *m = &info->modes[index];
            uint32_t rates = m->sample_rates, byte2 = m->sample_size, channel_count = m->channel_count;
            switch (code) {
            case RD_AUDIO_FORMAT_CODE_LINEARPCM:
                rates = dcn32_audio_check_bandwidth(crtc, dp, channel_count, signal, rates);
                byte2 = m->sample_size;
                v |= (rates & 0xffu) << 24;   /* SUPPORTED_FREQUENCIES_STEREO */
                break;
            case RD_AUDIO_FORMAT_CODE_AC3:
                is_ac3_supported = 1;
                break;
            case RD_AUDIO_FORMAT_CODE_DOLBYDIGITALPLUS:
            case RD_AUDIO_FORMAT_CODE_DTS_HD:
            case RD_AUDIO_FORMAT_CODE_MAT_MLP:
            case RD_AUDIO_FORMAT_CODE_DST:
            case RD_AUDIO_FORMAT_CODE_WMAPRO:
                byte2 = m->sample_size;   /* vendor_specific */
                break;
            default:
                break;
            }
            /* MAX_CHANNELS, SUPPORTED_FREQUENCIES, DESCRIPTOR_BYTE_2 */
            v |= (channel_count - 1) & 0x7u;
            v |= (rates & 0xffu) << 8;
            v |= (byte2 & 0xffu) << 16;
        }
        dcn32_aud_write(d, inst, RD_AZ_AUDIO_DESCRIPTOR0 + format_index, v);
    }
    /* "todo: this reg global. why program global register?" */
    if (is_ac3_supported)
        rdna3_wreg(d, A(d, regAZALIA_F0_CODEC_FUNCTION_PARAMETER_STREAM_FORMATS, 0, 0), 0x05);

    /* 192 kHz 8 ch (the HBR requirement) */
    set_high_bit_rate_capable(d, inst,
                              (dcn32_audio_check_bandwidth(crtc, dp, 8, signal, RD_AUDIO_RATE_192) & RD_AUDIO_RATE_192)
                                  != 0);
    /* audio and video lip sync */
    set_video_latency(d, inst, (int)info->video_latency);
    set_audio_latency(d, inst, (int)info->audio_latency);

    /* SINK_INFO0: manufacturer, product */
    dcn32_aud_write(d, inst, RD_AZ_SINK_INFO0, (info->manufacture_id & 0xffffu) | (info->product_id & 0xffffu) << 16);
    /* SINK_INFO1: the display name's length (at most 18) */
    while (info->display_name[strlen++] != '\0') {
        if (strlen >= RD_AUDIO_HW_DISPLAY_NAME_SIZE)
            break;
    }
    dcn32_aud_write(d, inst, RD_AZ_SINK_INFO1, strlen & 0xffu);
    RDBG(d, "audio: endpoint %u configured: name length %u", inst, strlen);
    /* the port id: PORT_ID0 = display index, PORT_ID1 = 16-bit BDF */
    dcn32_aud_write(d, inst, RD_AZ_SINK_INFO2, info->port_id[0]);
    dcn32_aud_write(d, inst, RD_AZ_SINK_INFO3, info->port_id[1]);
    /* the 18 char monitor string, 4 per register */
    for (i = 0; i < 5; i++) {
        uint32_t k, w = 0;
        for (k = 0; k < 4 && i * 4 + k < RD_AUDIO_HW_DISPLAY_NAME_SIZE; k++)
            w |= (uint32_t)info->display_name[i * 4 + k] << (8 * k);
        dcn32_aud_write(d, inst, RD_AZ_SINK_INFO4 + i, w);
    }
    v = dcn32_aud_read(d, inst, RD_AZ_HOT_PLUG_CONTROL);
    v &= ~RD_AZ_HPC_CLOCK_GATING_DISABLE;
    dcn32_aud_write(d, inst, RD_AZ_HOT_PLUG_CONTROL, v);
}

/* get_azalia_clock_info_hdmi: 24 MHz in 100 Hz over the pixel clock */
static void get_azalia_clock_info_hdmi(uint32_t actual_pixel_clock_100hz, uint32_t *phase, uint32_t *module)
{
    *phase = 24 * 10000;
    *module = actual_pixel_clock_100hz;
}

/* get_azalia_clock_info_dp: 24 MHz over the DP DTO source (DPREFCLK) */
static void get_azalia_clock_info_dp(const rdna3_audio_pll_info *pll, uint32_t *phase, uint32_t *module)
{
    *phase = 24 * 10000;
    *module = pll->audio_dto_source_clock_in_khz * 10;
}

/* dce_aud_wall_dto_setup */
void dcn32_aud_wall_dto_setup(rdna3_dev *d, uint32_t signal, const rdna3_audio_crtc_info *crtc,
                              const rdna3_audio_pll_info *pll)
{
    uint32_t phase = 0, module = 0, src = A(d, regDCCG_AUDIO_DTO_SOURCE, 0, 0);
    /* dc_is_hdmi_tmds_signal */
    if (signal == RD_SIGNAL_HDMI_TYPE_A) {
        /* DTO0: 24 MHz, 128 x Fs from the HDMI pixel clock; the source and
         * the select first ("otherwise there will be no HDMI audio at boot") */
        get_azalia_clock_info_hdmi(crtc->calculated_pixel_clock_100hz, &phase, &module);
        upd(d, src, MSK(DCCG_AUDIO_DTO_SOURCE, DCCG_AUDIO_DTO0_SOURCE_SEL) | MSK(DCCG_AUDIO_DTO_SOURCE, DCCG_AUDIO_DTO_SEL),
            FLD(DCCG_AUDIO_DTO_SOURCE, DCCG_AUDIO_DTO0_SOURCE_SEL, pll->dto_source - RD_DTO_SOURCE_ID0) |
                FLD(DCCG_AUDIO_DTO_SOURCE, DCCG_AUDIO_DTO_SEL, RD_DCCG_AUDIO_DTO_SEL_DTO0));
        upd(d, A(d, regDCCG_AUDIO_DTO0_MODULE, 0, 0), MSK(DCCG_AUDIO_DTO0_MODULE, DCCG_AUDIO_DTO0_MODULE), module);
        upd(d, A(d, regDCCG_AUDIO_DTO0_PHASE, 0, 0), MSK(DCCG_AUDIO_DTO0_PHASE, DCCG_AUDIO_DTO0_PHASE), phase);
    } else {
        /* DTO1: 24 MHz, 512 x Fs / 128 x Fs; the default, and DP's */
        get_azalia_clock_info_dp(pll, &phase, &module);
        upd(d, src, MSK(DCCG_AUDIO_DTO_SOURCE, DCCG_AUDIO_DTO_SEL),
            FLD(DCCG_AUDIO_DTO_SOURCE, DCCG_AUDIO_DTO_SEL, RD_DCCG_AUDIO_DTO_SEL_DTO1));
        upd(d, A(d, regDCCG_AUDIO_DTO1_MODULE, 0, 0), MSK(DCCG_AUDIO_DTO1_MODULE, DCCG_AUDIO_DTO1_MODULE), module);
        upd(d, A(d, regDCCG_AUDIO_DTO1_PHASE, 0, 0), MSK(DCCG_AUDIO_DTO1_PHASE, DCCG_AUDIO_DTO1_PHASE), phase);
        upd(d, src, MSK(DCCG_AUDIO_DTO_SOURCE, DCCG_AUDIO_DTO2_USE_512FBR_DTO),
            FLD(DCCG_AUDIO_DTO_SOURCE, DCCG_AUDIO_DTO2_USE_512FBR_DTO, 1));
    }
    RDBG(d, "audio: wall DTO %s: module %u phase %u", signal == RD_SIGNAL_HDMI_TYPE_A ? "0" : "1", module, phase);
}

/* dce_aud_endpoint_valid: PORT_CONNECTIVITY 1 = no physical connection */
int dcn32_aud_endpoint_valid(rdna3_dev *d, uint32_t inst)
{
    uint32_t v = dcn32_aud_read(d, inst, RD_AZ_RESPONSE_CONFIGURATION_DEFAULT);
    return (v >> 30) != 1;
}

/* dce_aud_hw_init: once, on endpoint 0: rates R5..R7 (32, 44.1, 48 kHz),
 * the CLKSTOP / EPSS keep-alive bits */
void dcn32_aud_hw_init(rdna3_dev *d, uint32_t inst)
{
    uint32_t v;
    if (inst != 0)
        return;
    v = dcn32_aud_read(d, inst, RD_AZ_HOT_PLUG_CONTROL);
    v |= RD_AZ_HPC_CLOCK_GATING_DISABLE;
    dcn32_aud_write(d, inst, RD_AZ_HOT_PLUG_CONTROL, v);
    upd(d, A(d, regAZALIA_F0_CODEC_FUNCTION_PARAMETER_SUPPORTED_SIZE_RATES, 0, 0),
        MSK(AZALIA_F0_CODEC_FUNCTION_PARAMETER_SUPPORTED_SIZE_RATES, AUDIO_RATE_CAPABILITIES),
        FLD(AZALIA_F0_CODEC_FUNCTION_PARAMETER_SUPPORTED_SIZE_RATES, AUDIO_RATE_CAPABILITIES, 0x70));
    upd(d, A(d, regAZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES, 0, 0),
        MSK(AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES, CLKSTOP) |
            MSK(AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES, EPSS),
        FLD(AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES, CLKSTOP, 1) |
            FLD(AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES, EPSS, 1));
    v &= ~RD_AZ_HPC_CLOCK_GATING_DISABLE;
    dcn32_aud_write(d, inst, RD_AZ_HOT_PLUG_CONTROL, v);
}

/* ==================== dc_resource.c: the endpoints ==================== */

void dcn32_audio_pool_init(rdna3_dev *d, rdna3_audio_pool *p)
{
    uint32_t i;
    for (i = 0; i < sizeof(*p); i++)
        ((uint8_t *)p)[i] = 0;
    /* read_dce_straps (dcn32_resource.c): DC_PINSTRAPS_AUDIO */
    p->straps = GETF(rdna3_rreg(d, A(d, regDC_PINSTRAPS, 0, 0)), DC_PINSTRAPS, DC_PINSTRAPS_AUDIO);
    /* update_num_audio (hdmi_disable 0, audio_stream_number 0: multi stream) */
    p->dp_audio = 1;
    p->hdmi_audio_native = p->hdmi_audio_on_dongle = (p->straps & 0x2) ? 1 : 0;
    /* resource_construct: the pins up to the first without a connection */
    for (i = 0; i < RD_AUDIO_MAX_ENDPOINTS; i++) {
        if (!dcn32_aud_endpoint_valid(d, i))
            break;
        p->count++;
    }
    /* dcn32_init_hw: hw_init on every pin (endpoint 0 programs the codec) */
    for (i = 0; i < p->count; i++)
        dcn32_aud_hw_init(d, i);
    RLOG(d, "audio: %u azalia endpoints, straps 0x%x (HDMI audio %s)", p->count, p->straps,
         p->hdmi_audio_native ? "native" : "off: HDMI connectors run as DVI");
}

/* find_first_free_audio + update_audio_usage */
int dcn32_audio_acquire(rdna3_audio_pool *p, uint32_t id, const uint8_t *stream_enc_acquired)
{
    uint32_t i;
    int found = -1;
    for (i = 0; i < p->count && found < 0; i++) {
        /* enough endpoints: the one matching the stream encoder */
        if (!p->acquired[i] && stream_enc_acquired && stream_enc_acquired[i]) {
            if (id != i)
                continue;
            found = (int)i;
        }
    }
    /* the engine id's own endpoint */
    if (found < 0 && id < p->count && !p->acquired[id])
        found = (int)id;
    /* first come, first served */
    for (i = 0; found < 0 && i < p->count; i++)
        if (!p->acquired[i])
            found = (int)i;
    if (found >= 0)
        p->acquired[found] = 1;
    return found;
}

void dcn32_audio_release(rdna3_audio_pool *p, int inst)
{
    if (inst >= 0 && (uint32_t)inst < RD_AUDIO_MAX_ENDPOINTS)
        p->acquired[inst] = 0;
}

int dcn32_audio_wanted(uint32_t signal, const rdna3_audio_info *info)
{
    /* dc_is_audio_capable_signal: DP, DP MST, HDMI (eDP has no connector here) */
    if (signal != RD_SIGNAL_DISPLAY_PORT && signal != RD_SIGNAL_DISPLAY_PORT_MST && signal != RD_SIGNAL_HDMI_TYPE_A)
        return 0;
    return info && info->mode_count && info->flags;
}

/* ==================== dcn30_afmt.c ==================== */

#define AFA(d, reg, fe) (rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + FE_S * (fe))

/* afmt3_setup_hdmi_audio (no afmt_poweron on DCN 3.2) */
static void afmt3_setup_hdmi_audio(rdna3_dev *d, uint32_t fe)
{
    upd(d, AFA(d, regAFMT0_AFMT_AUDIO_PACKET_CONTROL, fe), MSK(AFMT0_AFMT_AUDIO_PACKET_CONTROL, AFMT_60958_CS_UPDATE),
        FLD(AFMT0_AFMT_AUDIO_PACKET_CONTROL, AFMT_60958_CS_UPDATE, 1));
    upd(d, AFA(d, regAFMT0_AFMT_AUDIO_PACKET_CONTROL2, fe),
        MSK(AFMT0_AFMT_AUDIO_PACKET_CONTROL2, AFMT_AUDIO_LAYOUT_OVRD) |
            MSK(AFMT0_AFMT_AUDIO_PACKET_CONTROL2, AFMT_60958_OSF_OVRD), 0);
    upd(d, AFA(d, regAFMT0_AFMT_60958_0, fe),
        MSK(AFMT0_AFMT_60958_0, AFMT_60958_CS_CHANNEL_NUMBER_L) | MSK(AFMT0_AFMT_60958_0, AFMT_60958_CS_CLOCK_ACCURACY),
        FLD(AFMT0_AFMT_60958_0, AFMT_60958_CS_CHANNEL_NUMBER_L, 1));
    upd(d, AFA(d, regAFMT0_AFMT_60958_1, fe), MSK(AFMT0_AFMT_60958_1, AFMT_60958_CS_CHANNEL_NUMBER_R),
        FLD(AFMT0_AFMT_60958_1, AFMT_60958_CS_CHANNEL_NUMBER_R, 2));
    upd(d, AFA(d, regAFMT0_AFMT_60958_2, fe),
        MSK(AFMT0_AFMT_60958_2, AFMT_60958_CS_CHANNEL_NUMBER_2) | MSK(AFMT0_AFMT_60958_2, AFMT_60958_CS_CHANNEL_NUMBER_3) |
            MSK(AFMT0_AFMT_60958_2, AFMT_60958_CS_CHANNEL_NUMBER_4) |
            MSK(AFMT0_AFMT_60958_2, AFMT_60958_CS_CHANNEL_NUMBER_5) |
            MSK(AFMT0_AFMT_60958_2, AFMT_60958_CS_CHANNEL_NUMBER_6) |
            MSK(AFMT0_AFMT_60958_2, AFMT_60958_CS_CHANNEL_NUMBER_7),
        FLD(AFMT0_AFMT_60958_2, AFMT_60958_CS_CHANNEL_NUMBER_2, 3) | FLD(AFMT0_AFMT_60958_2, AFMT_60958_CS_CHANNEL_NUMBER_3, 4) |
            FLD(AFMT0_AFMT_60958_2, AFMT_60958_CS_CHANNEL_NUMBER_4, 5) |
            FLD(AFMT0_AFMT_60958_2, AFMT_60958_CS_CHANNEL_NUMBER_5, 6) |
            FLD(AFMT0_AFMT_60958_2, AFMT_60958_CS_CHANNEL_NUMBER_6, 7) |
            FLD(AFMT0_AFMT_60958_2, AFMT_60958_CS_CHANNEL_NUMBER_7, 8));
}

/* afmt3_se_audio_setup: the azalia stream -> DIG mapping, the channels */
static void afmt3_se_audio_setup(rdna3_dev *d, uint32_t fe, uint32_t az_inst, const rdna3_audio_info *info)
{
    uint32_t channels;
    if (!info)
        return;   /* "This should not happen" */
    channels = dcn32_audio_speakers_to_channels(info->flags);
    rdna3_wreg(d, AFA(d, regAFMT0_AFMT_AUDIO_SRC_CONTROL, fe),
               FLD(AFMT0_AFMT_AUDIO_SRC_CONTROL, AFMT_AUDIO_SRC_SELECT, az_inst));
    upd(d, AFA(d, regAFMT0_AFMT_AUDIO_PACKET_CONTROL2, fe), MSK(AFMT0_AFMT_AUDIO_PACKET_CONTROL2, AFMT_AUDIO_CHANNEL_ENABLE),
        FLD(AFMT0_AFMT_AUDIO_PACKET_CONTROL2, AFMT_AUDIO_CHANNEL_ENABLE, channels));
    /* no afmt_poweron: the forced memory power off goes */
    upd(d, AFA(d, regAFMT0_AFMT_MEM_PWR, fe), MSK(AFMT0_AFMT_MEM_PWR, AFMT_MEM_PWR_FORCE), 0);
}

/* afmt3_audio_mute_control: AFMT_AUDIO_SAMPLE_SEND */
static void afmt3_audio_mute_control(rdna3_dev *d, uint32_t fe, int mute)
{
    upd(d, AFA(d, regAFMT0_AFMT_AUDIO_PACKET_CONTROL, fe), MSK(AFMT0_AFMT_AUDIO_PACKET_CONTROL, AFMT_AUDIO_SAMPLE_SEND),
        FLD(AFMT0_AFMT_AUDIO_PACKET_CONTROL, AFMT_AUDIO_SAMPLE_SEND, mute ? 0 : 1));
}

/* afmt3_audio_info_immediate_update */
static void afmt3_audio_info_immediate_update(rdna3_dev *d, uint32_t fe)
{
    upd(d, AFA(d, regAFMT0_AFMT_INFOFRAME_CONTROL0, fe), MSK(AFMT0_AFMT_INFOFRAME_CONTROL0, AFMT_AUDIO_INFO_UPDATE),
        FLD(AFMT0_AFMT_INFOFRAME_CONTROL0, AFMT_AUDIO_INFO_UPDATE, 1));
}

/* afmt3_setup_dp_audio */
static void afmt3_setup_dp_audio(rdna3_dev *d, uint32_t fe)
{
    upd(d, AFA(d, regAFMT0_AFMT_AUDIO_PACKET_CONTROL, fe), MSK(AFMT0_AFMT_AUDIO_PACKET_CONTROL, AFMT_60958_CS_UPDATE),
        FLD(AFMT0_AFMT_AUDIO_PACKET_CONTROL, AFMT_60958_CS_UPDATE, 1));
    upd(d, AFA(d, regAFMT0_AFMT_AUDIO_PACKET_CONTROL2, fe),
        MSK(AFMT0_AFMT_AUDIO_PACKET_CONTROL2, AFMT_AUDIO_LAYOUT_OVRD) |
            MSK(AFMT0_AFMT_AUDIO_PACKET_CONTROL2, AFMT_60958_OSF_OVRD), 0);
    afmt3_audio_info_immediate_update(d, fe);
    upd(d, AFA(d, regAFMT0_AFMT_60958_0, fe), MSK(AFMT0_AFMT_60958_0, AFMT_60958_CS_CLOCK_ACCURACY), 0);
}

/* ==================== the DIO stream encoder (dcn10 / dcn30) ==================== */

/* enc1_se_enable_audio_clock (the clock-on wait is a TODO in DC) */
static void enc1_se_enable_audio_clock(rdna3_dev *d, uint32_t fe, int enable)
{
    upd(d, AFA(d, regDIG0_AFMT_CNTL, fe), MSK(DIG0_AFMT_CNTL, AFMT_AUDIO_CLOCK_EN),
        FLD(DIG0_AFMT_CNTL, AFMT_AUDIO_CLOCK_EN, enable ? 1 : 0));
}

/* enc3_se_setup_hdmi_audio: the AFMT, the audio packet delay, ACR auto
 * send with N / CTS for 32 / 44.1 / 48 kHz (the HDA driver picks the rate;
 * HDMI_ACR_N_MULTIPLE stays) */
static void enc3_se_setup_hdmi_audio(rdna3_dev *d, uint32_t fe, const rdna3_audio_crtc_info *crtc)
{
    rdna3_audio_clock_info ci = { 0, 0, 0, 0, 0, 0, 0 };
    afmt3_setup_hdmi_audio(d, fe);
    upd(d, AFA(d, regDIG0_HDMI_AUDIO_PACKET_CONTROL, fe), MSK(DIG0_HDMI_AUDIO_PACKET_CONTROL, HDMI_AUDIO_DELAY_EN),
        FLD(DIG0_HDMI_AUDIO_PACKET_CONTROL, HDMI_AUDIO_DELAY_EN, 1));
    upd(d, AFA(d, regDIG0_HDMI_ACR_PACKET_CONTROL, fe),
        MSK(DIG0_HDMI_ACR_PACKET_CONTROL, HDMI_ACR_AUTO_SEND) | MSK(DIG0_HDMI_ACR_PACKET_CONTROL, HDMI_ACR_SOURCE) |
            MSK(DIG0_HDMI_ACR_PACKET_CONTROL, HDMI_ACR_AUDIO_PRIORITY),
        FLD(DIG0_HDMI_ACR_PACKET_CONTROL, HDMI_ACR_AUTO_SEND, 1));
    dcn32_get_audio_clock_info(crtc->color_depth, crtc->requested_pixel_clock_100hz, crtc->calculated_pixel_clock_100hz,
                               &ci);
    RDBG(d, "audio: DIG %u ACR for %u x 100 Hz: N %u/%u/%u CTS %u/%u/%u", fe, crtc->requested_pixel_clock_100hz,
         ci.n_32khz, ci.n_44khz, ci.n_48khz, ci.cts_32khz, ci.cts_44khz, ci.cts_48khz);
    upd(d, AFA(d, regDIG0_HDMI_ACR_32_0, fe), MSK(DIG0_HDMI_ACR_32_0, HDMI_ACR_CTS_32),
        FLD(DIG0_HDMI_ACR_32_0, HDMI_ACR_CTS_32, ci.cts_32khz));
    upd(d, AFA(d, regDIG0_HDMI_ACR_32_1, fe), MSK(DIG0_HDMI_ACR_32_1, HDMI_ACR_N_32),
        FLD(DIG0_HDMI_ACR_32_1, HDMI_ACR_N_32, ci.n_32khz));
    upd(d, AFA(d, regDIG0_HDMI_ACR_44_0, fe), MSK(DIG0_HDMI_ACR_44_0, HDMI_ACR_CTS_44),
        FLD(DIG0_HDMI_ACR_44_0, HDMI_ACR_CTS_44, ci.cts_44khz));
    upd(d, AFA(d, regDIG0_HDMI_ACR_44_1, fe), MSK(DIG0_HDMI_ACR_44_1, HDMI_ACR_N_44),
        FLD(DIG0_HDMI_ACR_44_1, HDMI_ACR_N_44, ci.n_44khz));
    upd(d, AFA(d, regDIG0_HDMI_ACR_48_0, fe), MSK(DIG0_HDMI_ACR_48_0, HDMI_ACR_CTS_48),
        FLD(DIG0_HDMI_ACR_48_0, HDMI_ACR_CTS_48, ci.cts_48khz));
    upd(d, AFA(d, regDIG0_HDMI_ACR_48_1, fe), MSK(DIG0_HDMI_ACR_48_1, HDMI_ACR_N_48),
        FLD(DIG0_HDMI_ACR_48_1, HDMI_ACR_N_48, ci.n_48khz));
}

/* enc3_se_hdmi_audio_setup */
void dcn32_enc_hdmi_audio_setup(rdna3_dev *d, uint32_t fe, uint32_t az_inst, const rdna3_audio_info *info,
                                const rdna3_audio_crtc_info *crtc)
{
    enc1_se_enable_audio_clock(d, fe, 1);
    enc3_se_setup_hdmi_audio(d, fe, crtc);
    afmt3_se_audio_setup(d, fe, az_inst, info);
}

/* enc1_se_hdmi_audio_disable (no afmt_powerdown on DCN 3.2) */
void dcn32_enc_hdmi_audio_disable(rdna3_dev *d, uint32_t fe)
{
    enc1_se_enable_audio_clock(d, fe, 0);
}

/* enc3_se_dp_audio_setup */
void dcn32_enc_dp_audio_setup(rdna3_dev *d, uint32_t fe, uint32_t az_inst, const rdna3_audio_info *info)
{
    afmt3_se_audio_setup(d, fe, az_inst, info);
}

/* DP_SEC_AUD_N__DP_SEC_AUD_N__DEFAULT, DP_SEC_TIMESTAMP_MODE__AUTO_CALC */
#define RD_DP_SEC_AUD_N_DEFAULT 0x8000
#define RD_DP_SEC_TIMESTAMP_MODE_AUTO_CALC 1

/* enc3_se_setup_dp_audio: ATP, auto-calculated timestamps, the AFMT */
static void enc3_se_setup_dp_audio(rdna3_dev *d, uint32_t fe)
{
    rdna3_wreg(d, AFA(d, regDP0_DP_SEC_AUD_N, fe), FLD(DP0_DP_SEC_AUD_N, DP_SEC_AUD_N, RD_DP_SEC_AUD_N_DEFAULT));
    rdna3_wreg(d, AFA(d, regDP0_DP_SEC_TIMESTAMP, fe),
               FLD(DP0_DP_SEC_TIMESTAMP, DP_SEC_TIMESTAMP_MODE, RD_DP_SEC_TIMESTAMP_MODE_AUTO_CALC));
    afmt3_setup_dp_audio(d, fe);
}

/* enc1_se_enable_dp_audio: ASP, then ATP / AIP, the master enable last */
static void enc1_se_enable_dp_audio(rdna3_dev *d, uint32_t fe)
{
    uint32_t a = AFA(d, regDP0_DP_SEC_CNTL, fe);
    upd(d, a, MSK(DP0_DP_SEC_CNTL, DP_SEC_ASP_ENABLE), FLD(DP0_DP_SEC_CNTL, DP_SEC_ASP_ENABLE, 1));
    upd(d, a, MSK(DP0_DP_SEC_CNTL, DP_SEC_ATP_ENABLE) | MSK(DP0_DP_SEC_CNTL, DP_SEC_AIP_ENABLE),
        FLD(DP0_DP_SEC_CNTL, DP_SEC_ATP_ENABLE, 1) | FLD(DP0_DP_SEC_CNTL, DP_SEC_AIP_ENABLE, 1));
    upd(d, a, MSK(DP0_DP_SEC_CNTL, DP_SEC_STREAM_ENABLE), FLD(DP0_DP_SEC_CNTL, DP_SEC_STREAM_ENABLE, 1));
}

/* enc3_se_dp_audio_enable */
void dcn32_enc_dp_audio_enable(rdna3_dev *d, uint32_t fe)
{
    enc1_se_enable_audio_clock(d, fe, 1);
    enc3_se_setup_dp_audio(d, fe);
    enc1_se_enable_dp_audio(d, fe);
}

/* enc1_se_disable_dp_audio: the audio SDPs off; the master enable stays
 * when the infoframes still use the register */
static void enc1_se_disable_dp_audio(rdna3_dev *d, uint32_t fe)
{
    uint32_t a = AFA(d, regDP0_DP_SEC_CNTL, fe);
    upd(d, a,
        MSK(DP0_DP_SEC_CNTL, DP_SEC_ASP_ENABLE) | MSK(DP0_DP_SEC_CNTL, DP_SEC_ATP_ENABLE) |
            MSK(DP0_DP_SEC_CNTL, DP_SEC_AIP_ENABLE) | MSK(DP0_DP_SEC_CNTL, DP_SEC_ACM_ENABLE) |
            MSK(DP0_DP_SEC_CNTL, DP_SEC_STREAM_ENABLE), 0);
    if (rdna3_rreg(d, a) != 0)
        upd(d, a, MSK(DP0_DP_SEC_CNTL, DP_SEC_STREAM_ENABLE), FLD(DP0_DP_SEC_CNTL, DP_SEC_STREAM_ENABLE, 1));
}

/* enc1_se_dp_audio_disable */
void dcn32_enc_dp_audio_disable(rdna3_dev *d, uint32_t fe)
{
    enc1_se_disable_dp_audio(d, fe);
    enc1_se_enable_audio_clock(d, fe, 0);
}

/* enc3_audio_mute_control */
void dcn32_enc_audio_mute_control(rdna3_dev *d, uint32_t fe, int mute)
{
    afmt3_audio_mute_control(d, fe, mute);
}

/* ==================== the HPO stream encoder (dcn31) and its APG ==================== */

#define HA(d, reg, se) (rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + HPO_S * (se))
#define GA(d, reg, se) (rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + APG_S * (se))

/* apg31_enable: reset, then the APG on */
static void apg31_enable(rdna3_dev *d, uint32_t inst)
{
    uint32_t a = GA(d, regAPG0_APG_CONTROL, inst);
    upd(d, a, MSK(APG0_APG_CONTROL, APG_RESET), FLD(APG0_APG_CONTROL, APG_RESET, 1));
    if (rdna3_wait_reg(d, a, MSK(APG0_APG_CONTROL, APG_RESET_DONE), MSK(APG0_APG_CONTROL, APG_RESET_DONE), 0, 10))
        RDBG(d, "audio: APG %u reset not done", inst);
    upd(d, a, MSK(APG0_APG_CONTROL, APG_RESET), 0);
    if (rdna3_wait_reg(d, a, 0, MSK(APG0_APG_CONTROL, APG_RESET_DONE), 0, 10))
        RDBG(d, "audio: APG %u reset done stays", inst);
    upd(d, GA(d, regAPG0_APG_CONTROL2, inst), MSK(APG0_APG_CONTROL2, APG_ENABLE), FLD(APG0_APG_CONTROL2, APG_ENABLE, 1));
}

/* apg31_disable */
static void apg31_disable(rdna3_dev *d, uint32_t inst)
{
    upd(d, GA(d, regAPG0_APG_CONTROL2, inst), MSK(APG0_APG_CONTROL2, APG_ENABLE), 0);
}

/* apg31_se_audio_setup: DP's one audio stream (id 0), every channel pair
 * on, the forced memory power off goes */
static void apg31_se_audio_setup(rdna3_dev *d, uint32_t inst, const rdna3_audio_info *info)
{
    if (!info)
        return;
    upd(d, GA(d, regAPG0_APG_CONTROL2, inst), MSK(APG0_APG_CONTROL2, APG_DP_AUDIO_STREAM_ID), 0);
    upd(d, GA(d, regAPG0_APG_DBG_GEN_CONTROL, inst), MSK(APG0_APG_DBG_GEN_CONTROL, APG_DBG_AUDIO_CHANNEL_ENABLE),
        FLD(APG0_APG_DBG_GEN_CONTROL, APG_DBG_AUDIO_CHANNEL_ENABLE, 0xff));
    upd(d, GA(d, regAPG0_APG_MEM_PWR, inst), MSK(APG0_APG_MEM_PWR, APG_MEM_PWR_FORCE), 0);
}

/* hpo_dp_is_gsp_enabled: GSP 0, 2, 3, 11 sent every frame */
static int hpo_dp_is_gsp_enabled(rdna3_dev *d, uint32_t se)
{
    const uint32_t m = DP_SYM32_ENC0_DP_SYM32_ENC_SDP_GSP_CONTROL0__GSP_VIDEO_CONTINUOUS_TRANSMISSION_ENABLE_MASK;
    return (rdna3_rreg(d, HA(d, regDP_SYM32_ENC0_DP_SYM32_ENC_SDP_GSP_CONTROL0, se)) & m) ||
           (rdna3_rreg(d, HA(d, regDP_SYM32_ENC0_DP_SYM32_ENC_SDP_GSP_CONTROL2, se)) & m) ||
           (rdna3_rreg(d, HA(d, regDP_SYM32_ENC0_DP_SYM32_ENC_SDP_GSP_CONTROL3, se)) & m) ||
           (rdna3_rreg(d, HA(d, regDP_SYM32_ENC0_DP_SYM32_ENC_SDP_GSP_CONTROL11, se)) & m);
}

/* dcn31_hpo_dp_stream_enc_audio_setup: the audio input mux, the APG (APG n
 * serves HPO stream encoder n) */
void dcn32_hpo_se_audio_setup(rdna3_dev *d, uint32_t se, uint32_t az_inst, const rdna3_audio_info *info)
{
    upd(d, HA(d, regDP_STREAM_ENC0_DP_STREAM_ENC_AUDIO_CONTROL, se),
        MSK(DP_STREAM_ENC0_DP_STREAM_ENC_AUDIO_CONTROL, DP_STREAM_ENC_INPUT_MUX_AUDIO_STREAM_SOURCE_SEL),
        FLD(DP_STREAM_ENC0_DP_STREAM_ENC_AUDIO_CONTROL, DP_STREAM_ENC_INPUT_MUX_AUDIO_STREAM_SOURCE_SEL, az_inst));
    apg31_se_audio_setup(d, se, info);
}

#define SDPA(field) MSK(DP_SYM32_ENC0_DP_SYM32_ENC_SDP_AUDIO_CONTROL0, field)

/* dcn31_hpo_dp_stream_enc_audio_enable */
void dcn32_hpo_se_audio_enable(rdna3_dev *d, uint32_t se)
{
    uint32_t a = HA(d, regDP_SYM32_ENC0_DP_SYM32_ENC_SDP_AUDIO_CONTROL0, se);
    upd(d, a, SDPA(ASP_ENABLE), SDPA(ASP_ENABLE));
    upd(d, a, SDPA(ATP_ENABLE) | SDPA(AIP_ENABLE), SDPA(ATP_ENABLE) | SDPA(AIP_ENABLE));
    upd(d, HA(d, regDP_SYM32_ENC0_DP_SYM32_ENC_SDP_CONTROL, se),
        MSK(DP_SYM32_ENC0_DP_SYM32_ENC_SDP_CONTROL, SDP_STREAM_ENABLE),
        MSK(DP_SYM32_ENC0_DP_SYM32_ENC_SDP_CONTROL, SDP_STREAM_ENABLE));
    apg31_enable(d, se);
}

/* dcn31_hpo_dp_stream_enc_audio_disable: the SDP stream stays for GSPs */
void dcn32_hpo_se_audio_disable(rdna3_dev *d, uint32_t se)
{
    upd(d, HA(d, regDP_SYM32_ENC0_DP_SYM32_ENC_SDP_AUDIO_CONTROL0, se),
        SDPA(ASP_ENABLE) | SDPA(ATP_ENABLE) | SDPA(AIP_ENABLE) | SDPA(ACM_ENABLE), 0);
    if (!hpo_dp_is_gsp_enabled(d, se))
        upd(d, HA(d, regDP_SYM32_ENC0_DP_SYM32_ENC_SDP_CONTROL, se),
            MSK(DP_SYM32_ENC0_DP_SYM32_ENC_SDP_CONTROL, SDP_STREAM_ENABLE), 0);
    apg31_disable(d, se);
}

/* ==================== link hwss (DIO / HPO) ==================== */

static int is_dp(uint32_t signal)
{
    return signal == RD_SIGNAL_DISPLAY_PORT || signal == RD_SIGNAL_DISPLAY_PORT_MST;
}

/* setup_dio_audio_output / setup_hpo_dp_audio_output */
static void setup_audio_output(rdna3_audio_stream *s)
{
    if (s->hpo) {
        /* setup_hpo_dp_audio_output */
        dcn32_hpo_se_audio_setup(s->d, s->hpo_se, (uint32_t)s->az_inst, &s->info);
    } else if (is_dp(s->signal)) {
        /* setup_dio_audio_output, DP */
        dcn32_enc_dp_audio_setup(s->d, s->fe, (uint32_t)s->az_inst, &s->info);
    } else {
        /* setup_dio_audio_output, HDMI */
        dcn32_enc_hdmi_audio_setup(s->d, s->fe, (uint32_t)s->az_inst, &s->info, &s->out.crtc_info);
    }
}

/* enable_dio_audio_packet / enable_hpo_dp_audio_packet */
static void enable_audio_packet(rdna3_audio_stream *s)
{
    if (s->hpo) {
        /* enable_hpo_dp_audio_packet */
        dcn32_hpo_se_audio_enable(s->d, s->hpo_se);
        return;
    }
    /* enable_dio_audio_packet */
    if (is_dp(s->signal))
        dcn32_enc_dp_audio_enable(s->d, s->fe);
    dcn32_enc_audio_mute_control(s->d, s->fe, 0);
}

/* disable_dio_audio_packet / disable_hpo_dp_audio_packet */
static void disable_audio_packet(rdna3_audio_stream *s)
{
    if (s->hpo) {
        /* disable_hpo_dp_audio_packet */
        if (s->az_inst >= 0)
            dcn32_hpo_se_audio_disable(s->d, s->hpo_se);
        return;
    }
    /* disable_dio_audio_packet */
    dcn32_enc_audio_mute_control(s->d, s->fe, 1);
    if (s->az_inst >= 0) {
        if (is_dp(s->signal))
            dcn32_enc_dp_audio_disable(s->d, s->fe);
        else
            dcn32_enc_hdmi_audio_disable(s->d, s->fe);
    }
}

/* ==================== dce110_hwseq.c ==================== */

/* translate_to_dto_source: CONTROLLER_ID_D0 + OTG -> DTO_SOURCE_ID0 + OTG */
static uint32_t translate_to_dto_source(uint32_t otg)
{
    return otg <= 5 ? RD_DTO_SOURCE_ID0 + otg : RD_DTO_SOURCE_UNKNOWN;
}

/* populate_audio_dp_link_info */
static void populate_audio_dp_link_info(rdna3_audio_dp_link_info *li, uint32_t signal, const rdna3_mode *m,
                                        const rdna3_crtc_format *f, const rdna3_dsc_config *dsc,
                                        const rdna3_dp_link_settings *ls, uint32_t link_bw_kbps, int fec_supported)
{
    rd_fixpt bw;
    li->encoding = dcn32_dp_encoding(ls);
    li->is_mst = signal == RD_SIGNAL_DISPLAY_PORT_MST;
    li->lane_count = ls->lane_count;
    li->link_rate = ls->link_rate;
    bw = dc_fixpt_from_int(link_bw_kbps);
    /* the most pessimistic values: no FEC or SSC in the video stream */
    if (li->encoding == RD_DP_8b_10b_ENCODING && fec_supported)
        bw = dc_fixpt_mul(bw, dc_fixpt_from_fraction(100, 97));   /* DATA_EFFICIENCY_8b_10b_FEC_EFFICIENCY_x100 */
    else if (li->encoding == RD_DP_128b_132b_ENCODING)
        bw = dc_fixpt_mul(bw, dc_fixpt_from_fraction(10000, 9975));   /* 99.75 % SSC overhead */
    li->link_bandwidth_kbps = (uint32_t)dc_fixpt_floor(bw);
    /* hblank_min_symbol_width = floor(h_blank * bpp / 128) for 128b/132b */
    if (li->encoding == RD_DP_128b_132b_ENCODING) {
        uint32_t h_blank = m->h_total - m->h_active, bpp;
        if (dsc) {
            bpp = dsc->bits_per_pixel;
        } else {
            bpp = 16;
            switch (f->color_depth) {
            case RD_COLOR_DEPTH_666: bpp *= 18; break;
            case RD_COLOR_DEPTH_888: bpp *= 24; break;
            case RD_COLOR_DEPTH_101010: bpp *= 30; break;
            case RD_COLOR_DEPTH_121212: bpp *= 36; break;
            default: bpp = 0; break;
            }
            if (f->pixel_encoding == RD_PIXEL_ENCODING_YCBCR422)
                bpp = bpp * 2 / 3;
            else if (f->pixel_encoding == RD_PIXEL_ENCODING_YCBCR420)
                bpp /= 2;
        }
        li->hblank_min_symbol_width =
            (uint32_t)dc_fixpt_floor(dc_fixpt_div(dc_fixpt_from_int((int64_t)h_blank * bpp), dc_fixpt_from_int(128 / 16)));
    } else {
        li->hblank_min_symbol_width = 0;
    }
}

/* build_audio_output */
void dcn32_audio_build_output(rdna3_audio_stream *s, const rdna3_mode *m, const rdna3_crtc_format *f,
                              uint32_t requested_pix_khz, const rdna3_dsc_config *dsc,
                              const rdna3_dp_link_settings *ls, uint32_t link_bw_kbps, int fec_supported)
{
    rdna3_audio_output *o = &s->out;
    rdna3_audio_crtc_info *c = &o->crtc_info;
    uint32_t i;
    for (i = 0; i < sizeof(*o); i++)
        ((uint8_t *)o)[i] = 0;
    o->engine_id = s->hpo ? 12 + s->hpo_se /* ENGINE_ID_HPO_DP_0 */ : s->fe;
    o->signal = s->signal;
    /* the active part: audio goes in the real blank (no borders here) */
    c->h_total = m->h_total;
    c->h_active = m->h_active;
    c->v_active = m->v_active;
    c->pixel_repetition = 1;
    c->interlaced = m->interlaced ? 1 : 0;
    c->refresh_rate = (uint32_t)((uint64_t)m->pixel_khz * 1000 / ((uint64_t)m->h_total * m->v_total));
    c->color_depth = f->color_depth;
    c->requested_pixel_clock_100hz = requested_pix_khz * 10;
    c->calculated_pixel_clock_100hz = requested_pix_khz * 10;
    c->pixel_encoding = f->pixel_encoding;
    c->dsc_bits_per_pixel = dsc ? dsc->bits_per_pixel : 0;
    c->dsc_num_slices = dsc ? dsc->num_slices_h : 0;
    /* HDMI: the ACR at the deep color ratio; 4:2:0 halves */
    if (s->signal == RD_SIGNAL_HDMI_TYPE_A && c->requested_pixel_clock_100hz == m->pixel_khz * 10 &&
        f->pixel_encoding == RD_PIXEL_ENCODING_YCBCR420) {
        c->requested_pixel_clock_100hz /= 2;
        c->calculated_pixel_clock_100hz = requested_pix_khz * 10 / 2;
    }
    if (is_dp(s->signal))
        o->pll_info.audio_dto_source_clock_in_khz = s->dprefclk_khz;
    o->pll_info.feed_back_divider = 0;   /* pll_settings.feedback_divider: no PLL divider on DCN */
    o->pll_info.dto_source = translate_to_dto_source(s->otg);
    o->pll_info.ss_enabled = 1;          /* "hard code to enable for now" */
    o->pll_info.ss_percentage = 0;
    if (is_dp(s->signal) && ls)
        populate_audio_dp_link_info(&o->dp_link_info, s->signal, m, f, dsc, ls, link_bw_kbps, fec_supported);
}

/* dce110_setup_audio_dto: one stream's view of the topology loop (an HDMI
 * stream with audio takes DTO0, else a DP stream DTO1) */
void dcn32_audio_setup_dto(rdna3_audio_stream *s)
{
    if (s->az_inst < 0)
        return;
    if (s->signal == RD_SIGNAL_HDMI_TYPE_A) {
        /* dccg31_set_audio_dtbclk_dto with dto_params {0}: the DTBCLK audio DTO off */
        dcn32_dccg_set_audio_dtbclk_dto(s->d, 0, 0);
        dcn32_aud_wall_dto_setup(s->d, s->signal, &s->out.crtc_info, &s->out.pll_info);
    } else if (is_dp(s->signal)) {
        dcn32_aud_wall_dto_setup(s->d, s->signal, &s->out.crtc_info, &s->out.pll_info);
    }
}

void dcn32_audio_setup_output(rdna3_audio_stream *s)
{
    if (s->az_inst < 0)
        return;
    setup_audio_output(s);
    dcn32_aud_az_configure(s->d, (uint32_t)s->az_inst, s->signal, &s->out.crtc_info, &s->info, &s->out.dp_link_info);
    /* dc->config.disable_hbr_audio_dp2 (DCN 3.2) on a 128b/132b stream */
    if (s->hpo)
        dcn32_aud_az_disable_hbr_audio(s->d, (uint32_t)s->az_inst);
}

/* dcn32_enable_pme_wa -> dcn32_smu_set_pme_workaround: DALSMC_MSG_BacoAudioD3PME
 * ("apply the PME w/a in order to wake AZ from D3") */
static void pme_workaround(rdna3_audio_stream *s)
{
    int r;
    if (!s->pme_wa)
        return;
    r = dcn32_smu_send(s->d, RD_DALSMC_MSG_BacoAudioD3PME, 0, &s->last_pme);
    if (r)
        RERR(s->d, "audio: PMFW PME workaround (BacoAudioD3PME) failed (%d)", r);
}

/* dce110_enable_audio_stream */
void dcn32_audio_enable_stream(rdna3_audio_stream *s)
{
    if (s->az_inst < 0 || s->enabled)
        return;
    dcn32_aud_az_enable(s->d, (uint32_t)s->az_inst);
    /* num_audio >= 1 (it starts at 1): the PME workaround every time */
    pme_workaround(s);
    enable_audio_packet(s);
    s->enabled = 1;
    RLOG(s->d, "audio: endpoint %d on %s %u (%s), %u modes, speakers 0x%02x", s->az_inst, s->hpo ? "HPO" : "DIG",
         s->hpo ? s->hpo_se : s->fe, s->signal == RD_SIGNAL_HDMI_TYPE_A ? "HDMI" : "DP", s->info.mode_count,
         s->info.flags & 0xffu);
}

/* dce110_disable_audio_stream */
void dcn32_audio_disable_stream(rdna3_audio_stream *s)
{
    if (s->az_inst >= 0 && !s->enabled)
        return;
    disable_audio_packet(s);
    if (s->az_inst >= 0) {
        s->enabled = 0;
        pme_workaround(s);
    }
}

void dcn32_audio_reset_back_end(rdna3_audio_stream *s)
{
    if (s->az_inst >= 0)
        dcn32_aud_az_disable(s->d, (uint32_t)s->az_inst);
}

void dcn32_audio_attach(rdna3_audio_stream *s)
{
    if (s->az_inst < 0)
        return;
    if (s->signal == RD_SIGNAL_HDMI_TYPE_A && !s->hpo) {
        /* enc32_stream_encoder_hdmi_set_stream_attribute, "following belongs
         * to audio": the audio infoframe every frame on line 2 */
        uint32_t a = AFA(s->d, regDIG0_HDMI_INFOFRAME_CONTROL0, s->fe);
        upd(s->d, a, MSK(DIG0_HDMI_INFOFRAME_CONTROL0, HDMI_AUDIO_INFO_SEND),
            FLD(DIG0_HDMI_INFOFRAME_CONTROL0, HDMI_AUDIO_INFO_SEND, 1));
        afmt3_audio_info_immediate_update(s->d, s->fe);
        upd(s->d, AFA(s->d, regDIG0_HDMI_INFOFRAME_CONTROL1, s->fe), MSK(DIG0_HDMI_INFOFRAME_CONTROL1, HDMI_AUDIO_INFO_LINE),
            FLD(DIG0_HDMI_INFOFRAME_CONTROL1, HDMI_AUDIO_INFO_LINE, 2));
    }
    s->enabled = 0;
    dcn32_audio_setup_dto(s);
    dcn32_audio_setup_output(s);
    dcn32_audio_enable_stream(s);
}
