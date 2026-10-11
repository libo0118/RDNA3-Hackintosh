/*
 * dcn32_hdmi.c - see dcn32_hdmi.h. Ported from Linux amdgpu Display Core
 * (dio/dcn32/dcn32_dio_stream_encoder.c, dio/dcn30/dcn30_dio_stream_encoder.c,
 * dio/dcn10/dcn10_stream_encoder.c, dio/dcn10/dcn10_link_encoder.c,
 * dcn30/dcn30_afmt.c, core/dc_resource.c, link/protocols/link_ddc.c,
 * link/link_validation.c, modules/info_packet/info_packet.c, v6.12), MIT,
 * Copyright Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_hdmi.h"
#include "dcn32_dsc.h"
#include "dcaux.h"
#include "vbios.h"
#include "gen/dcn_3_2_0_hdmi.h"

#define DIG_S  (regDIG1_HDMI_CONTROL - regDIG0_HDMI_CONTROL)                          /* 0x100 */
#define AFMT_S (regAFMT1_AFMT_INFOFRAME_CONTROL0 - regAFMT0_AFMT_INFOFRAME_CONTROL0)    /* 0x100 */

#define FLD(reg, field, x) (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK)
#define MSK(reg, field) ((uint32_t)reg##__##field##_MASK)
/* paste reg##_BASE_IDX in the outermost macro, before 'reg' can expand */
#define AD(d, reg, n) (rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + DIG_S * (n))
#define AA(d, reg, n) (rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + AFMT_S * (n))

/* REG_UPDATE_n: one read-modify-write of the fields in 'mask' */
static void upd(rdna3_dev *d, uint32_t a, uint32_t mask, uint32_t v)
{
    rdna3_wreg(d, a, (rdna3_rreg(d, a) & ~mask) | (v & mask));
}

/* ---- dc_resource.c / info_packet.c: the packets ---- */

uint32_t dcn32_hdmi_vic_aspect(uint32_t vic)
{
    switch (vic) {
    case 1: case 2: case 17:
        return RD_ASPECT_RATIO_4_3;
    case 3: case 4: case 16: case 18: case 19: case 31: case 32: case 33: case 34: case 63: case 64:
    case 93: case 94: case 95: case 96: case 97:
        return RD_ASPECT_RATIO_16_9;
    default:
        return RD_ASPECT_RATIO_NO_DATA;
    }
}

uint32_t dcn32_hdmi_vic_from_cea(uint32_t vic)
{
    switch (vic) {
    case 95: return 1;   /* 3840x2160@30 */
    case 94: return 2;   /* 3840x2160@25 */
    case 93: return 3;   /* 3840x2160@24 */
    case 98: return 4;   /* 4096x2160@24 */
    default: return 0;
    }
}

static void clear_packet(rdna3_info_packet *p)
{
    uint32_t i;
    for (i = 0; i < sizeof(*p); i++)
        ((uint8_t *)p)[i] = 0;
}

/* set_avi_info_frame (union hdmi_info_packet's bit layout: PB1 = Y A0 B S,
 * PB2 = C M R, PB3 = ITC EC Q SC, PB4 = VIC, PB5 = YQ CN PR, PB6-13 bars) */
void dcn32_hdmi_build_avi(rdna3_info_packet *p, const rdna3_crtc_format *f, uint32_t color_space, uint32_t vic,
                          uint32_t aspect, int qs_bit, const rdna3_mode *m)
{
    uint32_t y, c = 0, ec = 0, mm = 0, q, i, version = 2, length = RD_HDMI_AVI_INFOFRAME_SIZE;
    uint32_t bar_top, bar_bottom, bar_left, bar_right;
    uint8_t sum;

    clear_packet(p);
    switch (f->pixel_encoding) {
    case RD_PIXEL_ENCODING_YCBCR422: y = 1; break;
    case RD_PIXEL_ENCODING_YCBCR444: y = 2; break;
    case RD_PIXEL_ENCODING_YCBCR420: y = 3; break;
    default: y = 0; break;
    }
    /* C0, C1 : Colorimetry (EC: 6 = BT.2020 RGB/YCbCr, 4 = opRGB; C 3 = extended) */
    switch (color_space) {
    case RD_CS_YCBCR709:
    case RD_CS_YCBCR709_LIMITED:
        c = 2;
        break;
    case RD_CS_YCBCR601:
    case RD_CS_YCBCR601_LIMITED:
        c = 1;
        break;
    case RD_CS_2020_RGB_FULLRANGE:
    case RD_CS_2020_RGB_LIMITEDRANGE:
    case RD_CS_2020_YCBCR:
        ec = 6;
        c = 3;
        break;
    case RD_CS_ADOBERGB:
        ec = 4;
        c = 3;
        break;
    default:
        c = 0;
        break;
    }
    if (aspect == RD_ASPECT_RATIO_4_3 || aspect == RD_ASPECT_RATIO_16_9)
        mm = aspect;
    /* Q0, Q1: full 2, limited 1, default 0 */
    if (qs_bit == 1) {
        if (color_space == RD_CS_SRGB || color_space == RD_CS_2020_RGB_FULLRANGE)
            q = 2;
        else if (color_space == RD_CS_SRGB_LIMITED || color_space == RD_CS_2020_RGB_LIMITEDRANGE)
            q = 1;
        else
            q = 0;
    } else {
        q = 0;
    }
    /* VIC >= 128: AVI InfoFrame version 3 */
    if (vic >= 128)
        version = 3;
    /* A0 = 1 (active format valid), B = 3 (both bars valid), S = 2
     * (underscan), R = 8 (same as picture), SC = 0 (uniform), ITC = 1 with
     * CN = 0 (content type "no data" / graphics), YQ = 0 (limited), PR = 0 */
    p->sb[1] = (uint8_t)(y << 5 | 1u << 4 | 3u << 2 | 2u);
    p->sb[2] = (uint8_t)(c << 6 | mm << 4 | 8u);
    p->sb[3] = (uint8_t)(1u << 7 | ec << 4 | q << 2 | 0u);
    p->sb[4] = (uint8_t)vic;
    p->sb[5] = 0;
    /* bars: no borders on this driver's timings */
    bar_top = 0;
    bar_bottom = m->v_total - 0 + 1;
    bar_left = 0;
    bar_right = m->h_total - 0 + 1;
    p->sb[6] = (uint8_t)bar_top;
    p->sb[7] = (uint8_t)(bar_top >> 8);
    p->sb[8] = (uint8_t)bar_bottom;
    p->sb[9] = (uint8_t)(bar_bottom >> 8);
    p->sb[10] = (uint8_t)bar_left;
    p->sb[11] = (uint8_t)(bar_left >> 8);
    p->sb[12] = (uint8_t)bar_right;
    p->sb[13] = (uint8_t)(bar_right >> 8);
    /* ACE0-ACE3 = 0 */
    p->hb0 = RD_HDMI_INFOFRAME_TYPE_AVI;
    p->hb1 = (uint8_t)version;
    p->hb2 = (uint8_t)length;
    sum = (uint8_t)(RD_HDMI_INFOFRAME_TYPE_AVI + length + version);
    for (i = 1; i <= length; i++)
        sum = (uint8_t)(sum + p->sb[i]);
    p->sb[0] = (uint8_t)(0x100 - sum);
    p->valid = 1;
}

void dcn32_hdmi_build_vsif(rdna3_info_packet *p, uint32_t hdmi_vic, uint32_t h_total, uint32_t v_total)
{
    uint32_t length = 5, i;
    uint8_t sum = 0;

    clear_packet(p);
    if (!(hdmi_vic != 0 && h_total >= 3840 && v_total >= 2160))
        return;
    /* IEEE OUI 00-0C-03, HDMI_Video_Format = 1 (extended resolution) */
    p->sb[1] = 0x03;
    p->sb[2] = 0x0C;
    p->sb[3] = 0x00;
    p->sb[4] = 1u << 5;
    p->sb[5] = (uint8_t)hdmi_vic;
    p->hb0 = 0x81;   /* HDMI_INFOFRAME_TYPE_VENDOR */
    p->hb1 = 0x01;
    p->hb2 = (uint8_t)length;
    sum = (uint8_t)(p->hb0 + p->hb1 + p->hb2);
    for (i = 1; i <= length; i++)
        sum = (uint8_t)(sum + p->sb[i]);
    p->sb[0] = (uint8_t)(0x100 - sum);
    p->valid = 1;
}

void dcn32_hdmi_build_info_frames(rdna3_hdmi_info_frames *f, const rdna3_mode *m, const rdna3_crtc_format *fmt,
                                  int hdmi_sink)
{
    uint32_t i, vic, hdmi_vic = 0, aspect;
    for (i = 0; i < sizeof(*f); i++)
        ((uint8_t *)f)[i] = 0;
    /* fill_stream_properties_from_drm_display_mode: drm's AVI VIC (0 for an
     * HDMI VIC mode on an HDMI sink) and the vendor infoframe's HDMI VIC */
    vic = edid_match_vic(m);
    if (hdmi_sink)
        hdmi_vic = dcn32_hdmi_vic_from_cea(vic);
    if (hdmi_vic)
        vic = 0;
    /* get_aspect_ratio: the picture aspect DRM gives the modes of the CTA
     * video data block; other modes none */
    aspect = m->origin == RD_MODE_FROM_CTA ? dcn32_hdmi_vic_aspect(edid_match_vic(m)) : RD_ASPECT_RATIO_NO_DATA;
    /* get_output_color_space with DRM_MODE_COLORIMETRY_DEFAULT */
    dcn32_hdmi_build_avi(&f->avi, fmt, fmt->pixel_encoding == RD_PIXEL_ENCODING_RGB ? RD_CS_SRGB
                                       : m->pixel_khz > 27030 ? RD_CS_YCBCR709 : RD_CS_YCBCR601,
                         vic, aspect, 0, m);
    /* set_vendor_info_packet: stream->vsp_infopacket */
    dcn32_hdmi_build_vsif(&f->vendor, hdmi_vic, m->h_total, m->v_total);
}

/* ---- dcn30_dio_stream_encoder.c: the infoframes through the VPG ---- */

/* HDMI_GENERICn_LINE: two packets per register, the even one in the low half */
static const uint32_t generic_line_reg[15] = {
    regDIG0_HDMI_GENERIC_PACKET_CONTROL1, regDIG0_HDMI_GENERIC_PACKET_CONTROL1,
    regDIG0_HDMI_GENERIC_PACKET_CONTROL2, regDIG0_HDMI_GENERIC_PACKET_CONTROL2,
    regDIG0_HDMI_GENERIC_PACKET_CONTROL3, regDIG0_HDMI_GENERIC_PACKET_CONTROL3,
    regDIG0_HDMI_GENERIC_PACKET_CONTROL4, regDIG0_HDMI_GENERIC_PACKET_CONTROL4,
    regDIG0_HDMI_GENERIC_PACKET_CONTROL7, regDIG0_HDMI_GENERIC_PACKET_CONTROL7,
    regDIG0_HDMI_GENERIC_PACKET_CONTROL8, regDIG0_HDMI_GENERIC_PACKET_CONTROL8,
    regDIG0_HDMI_GENERIC_PACKET_CONTROL9, regDIG0_HDMI_GENERIC_PACKET_CONTROL9,
    regDIG0_HDMI_GENERIC_PACKET_CONTROL10,
};

static uint32_t dig_addr(rdna3_dev *d, uint32_t off, uint32_t fe)
{
    /* all of the DIG front end's HDMI registers sit in segment 2 */
    return rdna3_reg_offset(d, HWIP_DCE, 0, regDIG0_HDMI_CONTROL_BASE_IDX, off) + DIG_S * fe;
}

/* enc3_update_hdmi_info_packet */
void dcn32_enc_update_hdmi_info_packet(rdna3_dev *d, uint32_t fe, uint32_t index, const rdna3_info_packet *p)
{
    uint32_t cont, send, line, ctl, sh, lmask;

    if (index > 14)
        return;   /* invalid HW packet index */
    if (p->valid) {
        uint8_t hb[4];
        hb[0] = p->hb0;
        hb[1] = p->hb1;
        hb[2] = p->hb2;
        hb[3] = p->hb3;
        dcn32_vpg_update_generic_info_packet(d, fe, index, hb, p->sb, 1);
        /* packet transmission begins on the next frame, every frame, line 2 */
        cont = 1;
        send = 1;
        line = 2;
    } else {
        cont = 0;
        send = 0;
        line = 0;
    }
    /* HDMI_GENERICn_SEND / _CONT: packets 0-7 in CONTROL0, 8-14 in CONTROL6 */
    ctl = index < 8 ? regDIG0_HDMI_GENERIC_PACKET_CONTROL0 : regDIG0_HDMI_GENERIC_PACKET_CONTROL6;
    sh = 4 * (index & 7);
    upd(d, dig_addr(d, ctl, fe), 3u << sh, (send | cont << 1) << sh);
    lmask = (index & 1) ? 0xffff0000u : 0x0000ffffu;
    upd(d, dig_addr(d, generic_line_reg[index], fe), lmask, (index & 1) ? line << 16 : line);
}

/* enc3_stream_encoder_update_hdmi_info_packets */
void dcn32_enc_update_hdmi_info_packets(rdna3_dev *d, uint32_t fe, const rdna3_hdmi_info_frames *f)
{
    /* for bring up, disable dp double TODO */
    upd(d, AD(d, regDIG0_HDMI_DB_CONTROL, fe), MSK(DIG0_HDMI_DB_CONTROL, HDMI_DB_DISABLE),
        FLD(DIG0_HDMI_DB_CONTROL, HDMI_DB_DISABLE, 1));
    upd(d, AD(d, regDIG0_AFMT_CNTL, fe), MSK(DIG0_AFMT_CNTL, AFMT_AUDIO_CLOCK_EN),
        FLD(DIG0_AFMT_CNTL, AFMT_AUDIO_CLOCK_EN, 1));
    /* always add mandatory packets first followed by optional ones */
    dcn32_enc_update_hdmi_info_packet(d, fe, 0, &f->avi);
    dcn32_enc_update_hdmi_info_packet(d, fe, 5, &f->hfvsif);
    dcn32_enc_update_hdmi_info_packet(d, fe, 2, &f->gamut);
    dcn32_enc_update_hdmi_info_packet(d, fe, 1, &f->vendor);
    dcn32_enc_update_hdmi_info_packet(d, fe, 3, &f->spd);
    dcn32_enc_update_hdmi_info_packet(d, fe, 4, &f->hdrsmd);
    dcn32_enc_update_hdmi_info_packet(d, fe, 6, &f->vtem);
}

/* enc3_stream_encoder_stop_hdmi_info_packets: REG_SET of 0 two packets at a
 * time, then packet 14's line */
void dcn32_enc_stop_hdmi_info_packets(rdna3_dev *d, uint32_t fe)
{
    rdna3_wreg(d, AD(d, regDIG0_HDMI_GENERIC_PACKET_CONTROL0, fe), 0);
    rdna3_wreg(d, AD(d, regDIG0_HDMI_GENERIC_PACKET_CONTROL1, fe), 0);
    rdna3_wreg(d, AD(d, regDIG0_HDMI_GENERIC_PACKET_CONTROL0, fe), 0);
    rdna3_wreg(d, AD(d, regDIG0_HDMI_GENERIC_PACKET_CONTROL2, fe), 0);
    rdna3_wreg(d, AD(d, regDIG0_HDMI_GENERIC_PACKET_CONTROL0, fe), 0);
    rdna3_wreg(d, AD(d, regDIG0_HDMI_GENERIC_PACKET_CONTROL3, fe), 0);
    rdna3_wreg(d, AD(d, regDIG0_HDMI_GENERIC_PACKET_CONTROL0, fe), 0);
    rdna3_wreg(d, AD(d, regDIG0_HDMI_GENERIC_PACKET_CONTROL4, fe), 0);
    rdna3_wreg(d, AD(d, regDIG0_HDMI_GENERIC_PACKET_CONTROL6, fe), 0);
    rdna3_wreg(d, AD(d, regDIG0_HDMI_GENERIC_PACKET_CONTROL7, fe), 0);
    rdna3_wreg(d, AD(d, regDIG0_HDMI_GENERIC_PACKET_CONTROL6, fe), 0);
    rdna3_wreg(d, AD(d, regDIG0_HDMI_GENERIC_PACKET_CONTROL8, fe), 0);
    rdna3_wreg(d, AD(d, regDIG0_HDMI_GENERIC_PACKET_CONTROL6, fe), 0);
    rdna3_wreg(d, AD(d, regDIG0_HDMI_GENERIC_PACKET_CONTROL9, fe), 0);
    rdna3_wreg(d, AD(d, regDIG0_HDMI_GENERIC_PACKET_CONTROL6, fe), 0);
    upd(d, AD(d, regDIG0_HDMI_GENERIC_PACKET_CONTROL10, fe),
        MSK(DIG0_HDMI_GENERIC_PACKET_CONTROL10, HDMI_GENERIC14_LINE), 0);
}

/* ---- dcn10 / dcn32 stream encoder: the HDMI engine ---- */

/* enc1_stream_encoder_set_stream_attribute_helper */
static void set_stream_attribute_helper(rdna3_dev *d, uint32_t fe, const rdna3_crtc_format *f)
{
    upd(d, AD(d, regDIG0_DIG_FE_CNTL, fe), MSK(DIG0_DIG_FE_CNTL, TMDS_PIXEL_ENCODING),
        FLD(DIG0_DIG_FE_CNTL, TMDS_PIXEL_ENCODING, f->pixel_encoding == RD_PIXEL_ENCODING_YCBCR422 ? 1 : 0));
    upd(d, AD(d, regDIG0_DIG_FE_CNTL, fe), MSK(DIG0_DIG_FE_CNTL, TMDS_COLOR_FORMAT),
        FLD(DIG0_DIG_FE_CNTL, TMDS_COLOR_FORMAT, 0));
}

#define HC(field, x) FLD(DIG0_HDMI_CONTROL, field, x)
#define HM(field) MSK(DIG0_HDMI_CONTROL, field)

/* enc32_stream_encoder_hdmi_set_stream_attribute after the VBIOS encoder
 * control (avoid_vbios_exec_table is off on DCN 3.2) */
void dcn32_enc_hdmi_set_stream_attribute(rdna3_dev *d, uint32_t fe, const rdna3_crtc_format *f,
                                         uint32_t actual_pix_clk_khz, int lte_340_scramble)
{
    uint32_t a = AD(d, regDIG0_HDMI_CONTROL, fe), v;

    /* configure pixel encoding */
    set_stream_attribute_helper(d, fe, f);
    /* setup HDMI engine */
    upd(d, a,
        HM(HDMI_PACKET_GEN_VERSION) | HM(HDMI_KEEPOUT_MODE) | HM(HDMI_DEEP_COLOR_ENABLE) | HM(HDMI_DATA_SCRAMBLE_EN) |
            HM(HDMI_NO_EXTRA_NULL_PACKET_FILLED) | HM(HDMI_CLOCK_CHANNEL_RATE),
        HC(HDMI_PACKET_GEN_VERSION, 1) | HC(HDMI_KEEPOUT_MODE, 1) | HC(HDMI_DEEP_COLOR_ENABLE, 0) |
            HC(HDMI_DATA_SCRAMBLE_EN, 0) | HC(HDMI_NO_EXTRA_NULL_PACKET_FILLED, 1) | HC(HDMI_CLOCK_CHANNEL_RATE, 0));
    /* configure color depth */
    switch (f->color_depth) {
    case RD_COLOR_DEPTH_888:
        upd(d, a, HM(HDMI_DEEP_COLOR_DEPTH), HC(HDMI_DEEP_COLOR_DEPTH, 0));
        break;
    case RD_COLOR_DEPTH_101010:
        v = f->pixel_encoding == RD_PIXEL_ENCODING_YCBCR422 ? 0 : 1;
        upd(d, a, HM(HDMI_DEEP_COLOR_DEPTH) | HM(HDMI_DEEP_COLOR_ENABLE),
            HC(HDMI_DEEP_COLOR_DEPTH, 1) | HC(HDMI_DEEP_COLOR_ENABLE, v));
        break;
    case RD_COLOR_DEPTH_121212:
        v = f->pixel_encoding == RD_PIXEL_ENCODING_YCBCR422 ? 0 : 1;
        upd(d, a, HM(HDMI_DEEP_COLOR_DEPTH) | HM(HDMI_DEEP_COLOR_ENABLE),
            HC(HDMI_DEEP_COLOR_DEPTH, 2) | HC(HDMI_DEEP_COLOR_ENABLE, v));
        break;
    case RD_COLOR_DEPTH_161616:
        upd(d, a, HM(HDMI_DEEP_COLOR_DEPTH) | HM(HDMI_DEEP_COLOR_ENABLE),
            HC(HDMI_DEEP_COLOR_DEPTH, 3) | HC(HDMI_DEEP_COLOR_ENABLE, 1));
        break;
    default:
        break;
    }
    if (actual_pix_clk_khz >= RD_HDMI_CLOCK_CHANNEL_RATE_MORE_340M) {
        /* the HDMI data scrambler; clock channel at 1/4 of the character rate */
        upd(d, a, HM(HDMI_DATA_SCRAMBLE_EN) | HM(HDMI_CLOCK_CHANNEL_RATE),
            HC(HDMI_DATA_SCRAMBLE_EN, 1) | HC(HDMI_CLOCK_CHANNEL_RATE, 1));
    } else if (lte_340_scramble) {
        /* scrambled with the clock channel at the character rate */
        upd(d, a, HM(HDMI_DATA_SCRAMBLE_EN) | HM(HDMI_CLOCK_CHANNEL_RATE),
            HC(HDMI_DATA_SCRAMBLE_EN, 1) | HC(HDMI_CLOCK_CHANNEL_RATE, 0));
    }
    /* General Control packet on every frame */
    upd(d, AD(d, regDIG0_HDMI_VBI_PACKET_CONTROL, fe),
        MSK(DIG0_HDMI_VBI_PACKET_CONTROL, HDMI_GC_CONT) | MSK(DIG0_HDMI_VBI_PACKET_CONTROL, HDMI_GC_SEND) |
            MSK(DIG0_HDMI_VBI_PACKET_CONTROL, HDMI_NULL_SEND),
        FLD(DIG0_HDMI_VBI_PACKET_CONTROL, HDMI_GC_CONT, 1) | FLD(DIG0_HDMI_VBI_PACKET_CONTROL, HDMI_GC_SEND, 1) |
            FLD(DIG0_HDMI_VBI_PACKET_CONTROL, HDMI_NULL_SEND, 1));
    /* no Audio Content Protection packet */
    upd(d, AD(d, regDIG0_HDMI_VBI_PACKET_CONTROL, fe), MSK(DIG0_HDMI_VBI_PACKET_CONTROL, HDMI_ACP_SEND), 0);
    /* the Audio InfoFrame packet */
    upd(d, AD(d, regDIG0_HDMI_INFOFRAME_CONTROL0, fe), MSK(DIG0_HDMI_INFOFRAME_CONTROL0, HDMI_AUDIO_INFO_SEND),
        FLD(DIG0_HDMI_INFOFRAME_CONTROL0, HDMI_AUDIO_INFO_SEND, 1));
    /* afmt3_audio_info_immediate_update: the double-buffered AUDIO_INFO now */
    upd(d, AA(d, regAFMT0_AFMT_INFOFRAME_CONTROL0, fe), MSK(AFMT0_AFMT_INFOFRAME_CONTROL0, AFMT_AUDIO_INFO_UPDATE),
        FLD(AFMT0_AFMT_INFOFRAME_CONTROL0, AFMT_AUDIO_INFO_UPDATE, 1));
    /* the line of the Audio InfoFrame: VBI_LINE_0 + 2 */
    upd(d, AD(d, regDIG0_HDMI_INFOFRAME_CONTROL1, fe), MSK(DIG0_HDMI_INFOFRAME_CONTROL1, HDMI_AUDIO_INFO_LINE),
        FLD(DIG0_HDMI_INFOFRAME_CONTROL1, HDMI_AUDIO_INFO_LINE, 2));
    /* HDMI GC AVMUTE off */
    upd(d, AD(d, regDIG0_HDMI_GC, fe), MSK(DIG0_HDMI_GC, HDMI_GC_AVMUTE), 0);
}

/* enc32_stream_encoder_dvi_set_stream_attribute after the VBIOS encoder control */
void dcn32_enc_dvi_set_stream_attribute(rdna3_dev *d, uint32_t fe, const rdna3_crtc_format *f)
{
    set_stream_attribute_helper(d, fe, f);
}

/* enc1_stream_encoder_set_avmute; dcn30_set_avmute's wait for the mute */
void dcn32_enc_set_avmute(rdna3_dev *d, uint32_t fe, int enable, int otg)
{
    upd(d, AD(d, regDIG0_HDMI_GC, fe), MSK(DIG0_HDMI_GC, HDMI_GC_AVMUTE), FLD(DIG0_HDMI_GC, HDMI_GC_AVMUTE, enable ? 1 : 0));
    /* wait for two frames to make sure AV mute is sent out */
    if (enable && otg >= 0 && dcn32_otg_is_enabled(d, (uint32_t)otg)) {
        dcn32_otg_wait_for_state(d, (uint32_t)otg, RD_CRTC_STATE_VACTIVE);
        dcn32_otg_wait_for_state(d, (uint32_t)otg, RD_CRTC_STATE_VBLANK);
        dcn32_otg_wait_for_state(d, (uint32_t)otg, RD_CRTC_STATE_VACTIVE);
        dcn32_otg_wait_for_state(d, (uint32_t)otg, RD_CRTC_STATE_VBLANK);
        dcn32_otg_wait_for_state(d, (uint32_t)otg, RD_CRTC_STATE_VACTIVE);
    }
}

/* enc1_reset_hdmi_stream_attribute */
void dcn32_enc_reset_hdmi_stream_attribute(rdna3_dev *d, uint32_t fe)
{
    upd(d, AD(d, regDIG0_HDMI_CONTROL, fe),
        HM(HDMI_PACKET_GEN_VERSION) | HM(HDMI_KEEPOUT_MODE) | HM(HDMI_DEEP_COLOR_ENABLE) | HM(HDMI_DATA_SCRAMBLE_EN) |
            HM(HDMI_CLOCK_CHANNEL_RATE),
        HC(HDMI_PACKET_GEN_VERSION, 1) | HC(HDMI_KEEPOUT_MODE, 1));
}

uint32_t dcn32_enc_hdmi_control(rdna3_dev *d, uint32_t fe)
{
    return rdna3_rreg(d, AD(d, regDIG0_HDMI_CONTROL, fe));
}

/* ---- link_ddc.c: SCDC ---- */

/* link_query_ddc_data: the offset (and a second byte to write), then the read */
static int scdc_query(rdna3_dev *d, uint32_t ddc, uint8_t *wbuf, uint32_t wlen, uint8_t *rbuf, uint32_t rlen)
{
    rd_i2c_payload p[2];
    uint32_t n = 0;
    p[n].addr7 = RD_HDMI_SCDC_ADDRESS;
    p[n].write = 1;
    p[n].data = wbuf;
    p[n].len = wlen;
    n++;
    if (rbuf && rlen) {
        p[n].addr7 = RD_HDMI_SCDC_ADDRESS;
        p[n].write = 0;
        p[n].data = rbuf;
        p[n].len = rlen;
        n++;
    }
    return dcddc_i2c_cmd(d, ddc, p, n);
}

/* write_scdc_data (panel_patch.skip_scdc_overwrite is never set by amdgpu_dm) */
int dcn32_hdmi_write_scdc(rdna3_dev *d, uint32_t ddc, uint32_t pix_clk_khz, int lte_340_scramble)
{
    int over_340_mhz = pix_clk_khz > 340000 ? 1 : 0;
    uint8_t offset = RD_HDMI_SCDC_SINK_VERSION, sink_version = 0, wb[2];
    int ret, r;

    ret = scdc_query(d, ddc, &offset, 1, &sink_version, 1);
    if (sink_version == 1) {
        /* Source Version = 1 */
        wb[0] = RD_HDMI_SCDC_SOURCE_VERSION;
        wb[1] = 1;
        r = scdc_query(d, ddc, wb, 2, NULL, 0);
        if (r && !ret)
            ret = r;
    }
    wb[0] = RD_HDMI_SCDC_TMDS_CONFIG;
    wb[1] = over_340_mhz ? 3 : lte_340_scramble ? 1 : 0;
    r = scdc_query(d, ddc, wb, 2, NULL, 0);
    if (r && !ret)
        ret = r;
    return ret;
}

/* read_scdc_data (DC reads and drops the status; it is returned here) */
int dcn32_hdmi_read_scdc(rdna3_dev *d, uint32_t ddc, rdna3_scdc_state *s)
{
    uint8_t offset = RD_HDMI_SCDC_TMDS_CONFIG;
    int ret, r;
    s->tmds_config = s->scrambler_status = s->status_flags[0] = s->status_flags[1] = 0;
    ret = scdc_query(d, ddc, &offset, 1, &s->tmds_config, 1);
    if (s->tmds_config & 0x1) {
        offset = RD_HDMI_SCDC_SCRAMBLER_STATUS;
        r = scdc_query(d, ddc, &offset, 1, &s->scrambler_status, 1);
        if (r && !ret)
            ret = r;
        offset = RD_HDMI_SCDC_STATUS_FLAGS;
        r = scdc_query(d, ddc, &offset, 1, s->status_flags, 2);
        if (r && !ret)
            ret = r;
    }
    return ret;
}

/* ---- link_validation.c / dcn10_link_encoder.c ---- */

/* get_tmds_output_pixel_clock_100hz */
uint32_t dcn32_tmds_output_pixel_clock_100hz(uint32_t pix_clk_100hz, const rdna3_crtc_format *f)
{
    uint32_t pxl_clk = pix_clk_100hz;
    if (f->pixel_encoding == RD_PIXEL_ENCODING_YCBCR420)
        pxl_clk /= 2;
    else if (f->pixel_encoding == RD_PIXEL_ENCODING_YCBCR422)
        pxl_clk = pxl_clk * 2 / 3;
    if (f->color_depth == RD_COLOR_DEPTH_101010)
        pxl_clk = pxl_clk * 10 / 8;
    else if (f->color_depth == RD_COLOR_DEPTH_121212)
        pxl_clk = pxl_clk * 12 / 8;
    return pxl_clk;
}

/* DCN 3.2's link_enc_feature (dcn32_resource.c) with the VBIOS encoder
 * caps the link encoder's constructor reads (HDMI_6GB_EN) */
void dcn32_link_enc_features(uint32_t vbios_encoder_caps, rdna3_link_enc_features *ft)
{
    ft->max_hdmi_pixel_clock = RD_DCN32_MAX_HDMI_PIXEL_CLOCK;
    ft->max_hdmi_deep_color = RD_COLOR_DEPTH_121212;
    ft->hdmi_ycbcr420_supported = 1;
    ft->dp_ycbcr420_supported = 1;
    ft->hdmi_6gb_en = (vbios_encoder_caps & RD_ATOM_ENC_CAP_HDMI6G) ? 1 : 0;
}

/* dcn10_link_encoder_validate_dvi_output */
static int validate_dvi_output(const rdna3_link_enc_features *ft, uint32_t connector_signal, uint32_t signal,
                               const rdna3_crtc_format *f, uint32_t pix_clk_100hz)
{
    uint32_t max_pixel_clock = RD_TMDS_MAX_PIXEL_CLOCK;
    if (signal == RD_SIGNAL_DVI_DUAL_LINK)
        max_pixel_clock *= 2;
    /* HDMI downgraded to DVI: not capped when the DDI is not DVI */
    if (connector_signal != RD_SIGNAL_DVI_DUAL_LINK && connector_signal != RD_SIGNAL_DVI_SINGLE_LINK)
        max_pixel_clock = ft->max_hdmi_pixel_clock;
    /* DVI only supports RGB pixel encoding */
    if (f->pixel_encoding != RD_PIXEL_ENCODING_RGB)
        return 0;
    /* DVI through the adapter of an HDMI connector */
    if ((connector_signal == RD_SIGNAL_DVI_SINGLE_LINK || connector_signal == RD_SIGNAL_HDMI_TYPE_A) &&
        signal != RD_SIGNAL_HDMI_TYPE_A && pix_clk_100hz > RD_TMDS_MAX_PIXEL_CLOCK * 10)
        return 0;
    if (pix_clk_100hz < RD_TMDS_MIN_PIXEL_CLOCK * 10)
        return 0;
    if (pix_clk_100hz > max_pixel_clock * 10)
        return 0;
    /* DVI supports 6/8bpp single-link and 10/16bpp dual-link */
    switch (f->color_depth) {
    case RD_COLOR_DEPTH_666:
    case RD_COLOR_DEPTH_888:
        break;
    case RD_COLOR_DEPTH_101010:
    case RD_COLOR_DEPTH_161616:
        if (signal != RD_SIGNAL_DVI_DUAL_LINK)
            return 0;
        break;
    default:
        return 0;
    }
    return 1;
}

/* dcn10_link_encoder_validate_hdmi_output (dc->debug.hdmi20_disable off) */
static int validate_hdmi_output(const rdna3_link_enc_features *ft, const rdna3_crtc_format *f,
                                uint32_t edid_max_tmds_clk_mhz, uint32_t adjusted_pix_clk_100hz)
{
    /* the pixel clock against the EDID's max TMDS clock */
    if (edid_max_tmds_clk_mhz != 0 && adjusted_pix_clk_100hz > edid_max_tmds_clk_mhz * 10000)
        return 0;
    if (ft->max_hdmi_deep_color < f->color_depth)
        return 0;
    if (f->color_depth < RD_COLOR_DEPTH_888)
        return 0;
    if (adjusted_pix_clk_100hz < RD_TMDS_MIN_PIXEL_CLOCK * 10)
        return 0;
    if (adjusted_pix_clk_100hz == 0 || adjusted_pix_clk_100hz > ft->max_hdmi_pixel_clock * 10)
        return 0;
    if (!ft->hdmi_ycbcr420_supported && f->pixel_encoding == RD_PIXEL_ENCODING_YCBCR420)
        return 0;
    if (!ft->hdmi_6gb_en && adjusted_pix_clk_100hz >= 3000000)
        return 0;
    return 1;
}

/* dcn10_link_encoder_validate_dp_output */
static int validate_dp_output(const rdna3_link_enc_features *ft, const rdna3_crtc_format *f)
{
    if (f->pixel_encoding == RD_PIXEL_ENCODING_YCBCR420 && !ft->dp_ycbcr420_supported)
        return 0;
    return 1;
}

/* dcn10_link_encoder_validate_output_with_stream */
int dcn32_link_enc_validate_output(const rdna3_link_enc_features *ft, uint32_t signal, uint32_t connector_signal,
                                   const rdna3_crtc_format *f, uint32_t pix_clk_khz, uint32_t phy_pix_clk_khz,
                                   uint32_t edid_max_tmds_clk_mhz)
{
    switch (signal) {
    case RD_SIGNAL_DVI_SINGLE_LINK:
    case RD_SIGNAL_DVI_DUAL_LINK:
        return validate_dvi_output(ft, connector_signal, signal, f, pix_clk_khz * 10);
    case RD_SIGNAL_HDMI_TYPE_A:
        return validate_hdmi_output(ft, f, edid_max_tmds_clk_mhz, phy_pix_clk_khz * 10);
    case RD_SIGNAL_DISPLAY_PORT:
    case RD_SIGNAL_DISPLAY_PORT_MST:
        return validate_dp_output(ft, f);
    default:
        return 0;
    }
}
