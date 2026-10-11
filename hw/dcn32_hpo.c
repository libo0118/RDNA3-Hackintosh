/*
 * dcn32_hpo.c - see dcn32_hpo.h. Every function names the amdgpu one it
 * ports (Linux v6.12, MIT); REG_UPDATE / REG_SET / REG_WAIT become the
 * read-modify-writes and polls below.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_hpo.h"
#include "dcn32_hwseq.h"
#include "gen/dcn_3_2_0_hpo.h"

#define GET(v, reg, field) (((v) & (uint32_t)reg##__##field##_MASK) >> reg##__##field##__SHIFT)
#define SET(v, reg, field, x) \
    (((v) & ~(uint32_t)reg##__##field##_MASK) | \
     (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK))
#define FIELD(reg, field, x) (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK)

/* one stride for the HPO stream and link encoders and their VPGs */
#define HPO_S  (regDP_STREAM_ENC1_DP_STREAM_ENC_CLOCK_CONTROL - regDP_STREAM_ENC0_DP_STREAM_ENC_CLOCK_CONTROL)
#define PRC_S  (regOTG1_PIXEL_RATE_CNTL - regOTG0_PIXEL_RATE_CNTL)
#define VPG_HPO0 6u   /* VPG[6] -> HPO_DP[0] (dcn32_hpo_dp_stream_encoder_create) */

static uint32_t rd(rdna3_dev *d, uint32_t seg, uint32_t off)
{
    return rdna3_rreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, (int)seg, off));
}

static void wr(rdna3_dev *d, uint32_t seg, uint32_t off, uint32_t v)
{
    rdna3_wreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, (int)seg, off), v);
}

static uint32_t addr(rdna3_dev *d, uint32_t seg, uint32_t off)
{
    return rdna3_reg_offset(d, HWIP_DCE, 0, (int)seg, off);
}

/* instance n of register R (the name without its "reg" prefix; n x HPO_S /
 * PRC_S / 1 from instance 0). Two levels so an R that is itself a macro
 * (SE0 / SY0) expands before the pasting. */
#define RN(d, R, n, s)       RN_(d, R, n, s)
#define RN_(d, R, n, s)      rd((d), reg##R##_BASE_IDX, (uint32_t)(reg##R) + (uint32_t)(s) * (n))
#define WN(d, R, n, s, v)    WN_(d, R, n, s, v)
#define WN_(d, R, n, s, v)   wr((d), reg##R##_BASE_IDX, (uint32_t)(reg##R) + (uint32_t)(s) * (n), (v))
#define AN(d, R, n, s)       AN_(d, R, n, s)
#define AN_(d, R, n, s)      addr((d), reg##R##_BASE_IDX, (uint32_t)(reg##R) + (uint32_t)(s) * (n))
/* REG_UPDATE(reg, field, x) on instance n */
#define UPD(d, R, n, s, field, x)  UPD_(d, R, n, s, field, x)
#define UPD_(d, R, n, s, field, x) WN_(d, R, n, s, SET(RN_(d, R, n, s), R, field, x))
/* REG_WAIT(reg, field, x, ...) on instance n, 'us' microseconds at most */
#define WAIT(d, R, n, s, field, x, us)  WAIT_(d, R, n, s, field, x, us)
#define WAIT_(d, R, n, s, field, x, us) \
    rdna3_wait_reg((d), AN_(d, R, n, s), FIELD(R, field, x), (uint32_t)R##__##field##_MASK, 0, (us))
/* GET / SET / FIELD of an R that is a macro (SE0 / SY0) */
#define GETX(v, R, field)    GET(v, R, field)
#define SETX(v, R, field, x) SET(v, R, field, x)
#define FIELDX(R, field, x)  FIELD(R, field, x)

/* ==================== HPO DP stream encoder ==================== */

#define SE0(r) DP_STREAM_ENC0_DP_STREAM_ENC_##r
#define SY0(r) DP_SYM32_ENC0_DP_SYM32_ENC_##r

/* dcn31_hpo_dp_stream_enc_enable_stream */
void dcn32_hpo_se_enable_stream(rdna3_dev *d, uint32_t se)
{
    /* every clock of the DP_STREAM_ENC on */
    UPD(d, DP_STREAM_ENC0_DP_STREAM_ENC_CLOCK_CONTROL, se, HPO_S, DP_STREAM_ENC_CLOCK_EN, 1);
    /* the DP_SYM32_ENC logic through reset: assert, wait, release, wait */
    UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_CONTROL, se, HPO_S, DP_SYM32_ENC_RESET, 1);
    WAIT(d, DP_SYM32_ENC0_DP_SYM32_ENC_CONTROL, se, HPO_S, DP_SYM32_ENC_RESET_DONE, 1, 10);
    UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_CONTROL, se, HPO_S, DP_SYM32_ENC_RESET, 0);
    WAIT(d, DP_SYM32_ENC0_DP_SYM32_ENC_CONTROL, se, HPO_S, DP_SYM32_ENC_RESET_DONE, 0, 10);
    /* idle pattern generation on */
    UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_CONTROL, se, HPO_S, DP_SYM32_ENC_ENABLE, 1);
}

/* dcn31_hpo_dp_stream_enc_dp_unblank: 'stream_source' = the OTG */
void dcn32_hpo_se_dp_unblank(rdna3_dev *d, uint32_t se, uint32_t stream_source)
{
    UPD(d, DP_STREAM_ENC0_DP_STREAM_ENC_INPUT_MUX_CONTROL, se, HPO_S,
        DP_STREAM_ENC_INPUT_MUX_PIXEL_STREAM_SOURCE_SEL, stream_source);
    /* video transmission in the main framer */
    UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_VID_STREAM_CONTROL, se, HPO_S, VID_STREAM_ENABLE, 1);
    /* the pixel to symbol FIFO reset, then on */
    UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_VID_FIFO_CONTROL, se, HPO_S, PIXEL_TO_SYMBOL_FIFO_RESET, 1);
    WAIT(d, DP_SYM32_ENC0_DP_SYM32_ENC_VID_FIFO_CONTROL, se, HPO_S, PIXEL_TO_SYMBOL_FIFO_RESET_DONE, 1, 10);
    UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_VID_FIFO_CONTROL, se, HPO_S, PIXEL_TO_SYMBOL_FIFO_RESET, 0);
    WAIT(d, DP_SYM32_ENC0_DP_SYM32_ENC_VID_FIFO_CONTROL, se, HPO_S, PIXEL_TO_SYMBOL_FIFO_RESET_DONE, 0, 10);
    UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_VID_FIFO_CONTROL, se, HPO_S, PIXEL_TO_SYMBOL_FIFO_ENABLE, 1);
    /* the clock ramp adjuster FIFO reset, then on (after the debug CRC) */
    UPD(d, DP_STREAM_ENC0_DP_STREAM_ENC_CLOCK_RAMP_ADJUSTER_FIFO_STATUS_CONTROL0, se, HPO_S, FIFO_RESET, 1);
    WAIT(d, DP_STREAM_ENC0_DP_STREAM_ENC_CLOCK_RAMP_ADJUSTER_FIFO_STATUS_CONTROL0, se, HPO_S, FIFO_RESET_DONE, 1, 10);
    UPD(d, DP_STREAM_ENC0_DP_STREAM_ENC_CLOCK_RAMP_ADJUSTER_FIFO_STATUS_CONTROL0, se, HPO_S, FIFO_RESET, 0);
    WAIT(d, DP_STREAM_ENC0_DP_STREAM_ENC_CLOCK_RAMP_ADJUSTER_FIFO_STATUS_CONTROL0, se, HPO_S, FIFO_RESET_DONE, 0, 10);
    {
        uint32_t v = RN(d, DP_SYM32_ENC0_DP_SYM32_ENC_VID_CRC_CONTROL, se, HPO_S);
        v = SET(v, DP_SYM32_ENC0_DP_SYM32_ENC_VID_CRC_CONTROL, CRC_ENABLE, 1);
        v = SET(v, DP_SYM32_ENC0_DP_SYM32_ENC_VID_CRC_CONTROL, CRC_CONT_MODE_ENABLE, 1);
        WN(d, DP_SYM32_ENC0_DP_SYM32_ENC_VID_CRC_CONTROL, se, HPO_S, v);
    }
    UPD(d, DP_STREAM_ENC0_DP_STREAM_ENC_CLOCK_RAMP_ADJUSTER_FIFO_STATUS_CONTROL0, se, HPO_S, FIFO_ENABLE, 1);
}

/* dcn31_hpo_dp_stream_enc_dp_blank */
void dcn32_hpo_se_dp_blank(rdna3_dev *d, uint32_t se)
{
    UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_VID_STREAM_CONTROL, se, HPO_S, VID_STREAM_ENABLE, 0);
    /* until the next vblank: 10 us x 5000, a 24 Hz frame and some */
    if (WAIT(d, DP_SYM32_ENC0_DP_SYM32_ENC_VID_STREAM_CONTROL, se, HPO_S, VID_STREAM_STATUS, 0, 50000))
        RERR(d, "hpo: stream encoder %u: video stream status stays on after the blank", se);
    UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_SDP_CONTROL, se, HPO_S, SDP_STREAM_ENABLE, 0);
    UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_VID_FIFO_CONTROL, se, HPO_S, PIXEL_TO_SYMBOL_FIFO_ENABLE, 0);
    UPD(d, DP_STREAM_ENC0_DP_STREAM_ENC_CLOCK_RAMP_ADJUSTER_FIFO_STATUS_CONTROL0, se, HPO_S, FIFO_ENABLE, 0);
}

/* dcn31_hpo_dp_stream_enc_disable */
void dcn32_hpo_se_disable(rdna3_dev *d, uint32_t se)
{
    UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_CONTROL, se, HPO_S, DP_SYM32_ENC_ENABLE, 0);
    UPD(d, DP_STREAM_ENC0_DP_STREAM_ENC_CLOCK_CONTROL, se, HPO_S, DP_STREAM_ENC_CLOCK_EN, 0);
}

/* enum dp2_pixel_encoding, enum dp2_uncompressed_component_depth */
enum { DP2_RGB_YCBCR444 = 0, DP2_YCBCR422, DP2_YCBCR420, DP2_Y_ONLY };
enum { DP2_DEPTH_6BPC = 0, DP2_DEPTH_8BPC, DP2_DEPTH_10BPC, DP2_DEPTH_12BPC };

static void msa(rdna3_dev *d, uint32_t se, uint32_t n, uint32_t l0, uint32_t l1, uint32_t l2, uint32_t l3)
{
    wr(d, regDP_SYM32_ENC0_DP_SYM32_ENC_VID_MSA0_BASE_IDX,
       (uint32_t)regDP_SYM32_ENC0_DP_SYM32_ENC_VID_MSA0 + n + HPO_S * se,
       (l0 & 0xff) | (l1 & 0xff) << 8 | (l2 & 0xff) << 16 | (l3 & 0xff) << 24);
}

/* dcn31_hpo_dp_stream_enc_set_stream_attribute (COLOR_SPACE_SRGB, the MSA
 * per DP 2.0 2.7.4.1: 9 words, one byte per lane) */
void dcn32_hpo_se_set_stream_attribute(rdna3_dev *d, uint32_t se, const rdna3_mode *m, uint32_t encoding,
                                       uint32_t depth, int compressed, int double_buffer_en)
{
    uint32_t pixel_encoding, component_depth, h_active_start, v_active_start, h_blank, h_back_porch, h_width,
             v_height, v;
    uint64_t v_freq;
    uint8_t misc0 = 0, misc1 = 0, hsp, vsp;

    /* MISC0[0] 0: asynchronous clocks; MISC1[0] 0: no interlace */
    /* double buffer the MSA / pixel format only for a running stream */
    UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_VID_MSA_DOUBLE_BUFFER_CONTROL, se, HPO_S, MSA_DOUBLE_BUFFER_ENABLE,
        double_buffer_en ? 1 : 0);
    UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_VID_PIXEL_FORMAT_DOUBLE_BUFFER_CONTROL, se, HPO_S,
        PIXEL_FORMAT_DOUBLE_BUFFER_ENABLE, double_buffer_en ? 1 : 0);
    switch (encoding) {
    case RD_PIXEL_ENCODING_YCBCR422:
        pixel_encoding = DP2_YCBCR422;
        misc0 |= 0x2;
        break;
    case RD_PIXEL_ENCODING_YCBCR444:
        pixel_encoding = DP2_RGB_YCBCR444;
        misc0 |= 0x4;
        break;
    case RD_PIXEL_ENCODING_YCBCR420:
        pixel_encoding = DP2_YCBCR420;
        misc1 |= 0x40;
        break;
    case RD_PIXEL_ENCODING_RGB:
    default:
        pixel_encoding = DP2_RGB_YCBCR444;
        break;
    }
    /* no VSC SDP for the colorimetry */
    misc1 &= (uint8_t)~0x40;
    switch (depth) {
    case RD_COLOR_DEPTH_666:
        component_depth = DP2_DEPTH_6BPC;
        break;
    case RD_COLOR_DEPTH_888:
        component_depth = DP2_DEPTH_8BPC;
        misc0 |= 0x20;
        break;
    case RD_COLOR_DEPTH_101010:
        component_depth = DP2_DEPTH_10BPC;
        misc0 |= 0x40;
        break;
    case RD_COLOR_DEPTH_121212:
        component_depth = DP2_DEPTH_12BPC;
        misc0 |= 0x60;
        break;
    default:
        component_depth = DP2_DEPTH_6BPC;
        break;
    }
    v = RN(d, DP_SYM32_ENC0_DP_SYM32_ENC_VID_PIXEL_FORMAT, se, HPO_S);
    v = SET(v, DP_SYM32_ENC0_DP_SYM32_ENC_VID_PIXEL_FORMAT, PIXEL_ENCODING_TYPE, compressed ? 1 : 0);
    v = SET(v, DP_SYM32_ENC0_DP_SYM32_ENC_VID_PIXEL_FORMAT, UNCOMPRESSED_PIXEL_ENCODING, pixel_encoding);
    v = SET(v, DP_SYM32_ENC0_DP_SYM32_ENC_VID_PIXEL_FORMAT, UNCOMPRESSED_COMPONENT_DEPTH, component_depth);
    WN(d, DP_SYM32_ENC0_DP_SYM32_ENC_VID_PIXEL_FORMAT, se, HPO_S, v);
    /* COLOR_SPACE_SRGB: MISC1[7] = 0 */
    misc1 &= (uint8_t)~0x80;
    /* from the VESA timing; h_active_start from the leading edge of sync */
    h_blank = m->h_total - m->h_active;
    h_back_porch = h_blank - m->h_front - m->h_sync;
    h_active_start = m->h_sync + h_back_porch;
    v_active_start = m->v_total - m->v_active - m->v_front;
    h_width = m->h_active;
    v_height = m->v_active;
    hsp = m->h_sync_pos ? 0 : 0x80;
    vsp = m->v_sync_pos ? 0 : 0x80;
    v_freq = (uint64_t)m->pixel_khz * 10 * 100;   /* pix_clk_100hz x 100 */
    msa(d, se, 0, 0, 0, 0, (uint32_t)(v_freq >> 40));
    msa(d, se, 1, 0, 0, 0, (uint32_t)(v_freq >> 32) & 0xff);
    msa(d, se, 2, 0, 0, 0, (uint32_t)(v_freq >> 24) & 0xff);
    msa(d, se, 3, m->h_total >> 8, h_active_start >> 8, h_width >> 8, (uint32_t)(v_freq >> 16) & 0xff);
    msa(d, se, 4, m->h_total & 0xff, h_active_start & 0xff, h_width & 0xff, (uint32_t)(v_freq >> 8) & 0xff);
    msa(d, se, 5, m->v_total >> 8, v_active_start >> 8, v_height >> 8, (uint32_t)v_freq & 0xff);
    msa(d, se, 6, m->v_total & 0xff, v_active_start & 0xff, v_height & 0xff, misc0);
    msa(d, se, 7, hsp | (m->h_sync >> 8), vsp | (m->v_sync >> 8), 0, misc1);
    msa(d, se, 8, m->h_sync & 0xff, m->v_sync & 0xff, 0, 0);
}

/* vpg31 / vpg3_update_generic_info_packet on VPG 6 + se */
static void vpg_packet(rdna3_dev *d, uint32_t se, uint32_t index, const rdna3_info_packet *p, int immediate)
{
    uint8_t hb[4];
    hb[0] = p->hb0;
    hb[1] = p->hb1;
    hb[2] = p->hb2;
    hb[3] = p->hb3;
    dcn32_vpg_update_generic_info_packet(d, VPG_HPO0 + se, index, hb, p->sb, immediate);
}

/* dcn31_hpo_dp_stream_enc_update_dp_info_packets_sdp_line_num */
void dcn32_hpo_se_update_dp_info_packets_sdp_line_num(rdna3_dev *d, uint32_t se, const rdna3_dp_info_frames *f)
{
    if (f->adaptive_sync.valid && f->adaptive_sync_line_num_valid) {
        /* 00: REFER_TO_DP_SOF, 01: REFER_TO_OTG_SOF */
        UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_SDP_GSP_CONTROL5, se, HPO_S, GSP_SOF_REFERENCE, 1);
        UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_SDP_GSP_CONTROL5, se, HPO_S, GSP_TRANSMISSION_LINE_NUMBER,
            f->adaptive_sync_line_num);
    }
}

/* dcn31_hpo_dp_stream_enc_update_dp_info_packets */
void dcn32_hpo_se_update_dp_info_packets(rdna3_dev *d, uint32_t se, const rdna3_dp_info_frames *f)
{
    uint32_t dmdata_packet_enabled;
    if (f->vsc.valid)
        vpg_packet(d, se, 0, &f->vsc, 1);
    if (f->spd.valid)
        vpg_packet(d, se, 2, &f->spd, 1);
    if (f->hdrsmd.valid)
        vpg_packet(d, se, 3, &f->hdrsmd, 1);
    /* packet 4 is for immediate SDP messages */
    if (f->adaptive_sync.valid)
        vpg_packet(d, se, 5, &f->adaptive_sync, 1);
    /* transmission from the next frame */
    UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_SDP_GSP_CONTROL0, se, HPO_S, GSP_VIDEO_CONTINUOUS_TRANSMISSION_ENABLE,
        f->vsc.valid ? 1 : 0);
    UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_SDP_GSP_CONTROL2, se, HPO_S, GSP_VIDEO_CONTINUOUS_TRANSMISSION_ENABLE,
        f->spd.valid ? 1 : 0);
    UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_SDP_GSP_CONTROL3, se, HPO_S, GSP_VIDEO_CONTINUOUS_TRANSMISSION_ENABLE,
        f->hdrsmd.valid ? 1 : 0);
    UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_SDP_GSP_CONTROL5, se, HPO_S, GSP_VIDEO_CONTINUOUS_TRANSMISSION_ENABLE,
        f->adaptive_sync.valid ? 1 : 0);
    /* dynamic metadata transmission (read, as DC does, not used) */
    dmdata_packet_enabled = GET(RN(d, DP_SYM32_ENC0_DP_SYM32_ENC_SDP_METADATA_PACKET_CONTROL, se, HPO_S),
                                DP_SYM32_ENC0_DP_SYM32_ENC_SDP_METADATA_PACKET_CONTROL, METADATA_PACKET_ENABLE);
    (void)dmdata_packet_enabled;
    /* the secondary data path on */
    UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_SDP_CONTROL, se, HPO_S, SDP_STREAM_ENABLE, 1);
}

/* dcn31_hpo_dp_stream_enc_stop_dp_info_packets */
void dcn32_hpo_se_stop_dp_info_packets(rdna3_dev *d, uint32_t se)
{
    uint32_t a;
    UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_SDP_GSP_CONTROL0, se, HPO_S, GSP_VIDEO_CONTINUOUS_TRANSMISSION_ENABLE, 0);
    UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_SDP_GSP_CONTROL2, se, HPO_S, GSP_VIDEO_CONTINUOUS_TRANSMISSION_ENABLE, 0);
    UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_SDP_GSP_CONTROL3, se, HPO_S, GSP_VIDEO_CONTINUOUS_TRANSMISSION_ENABLE, 0);
    /* the secondary data path off unless audio still uses it */
    a = RN(d, DP_SYM32_ENC0_DP_SYM32_ENC_SDP_AUDIO_CONTROL0, se, HPO_S);
    if (!(a & (DP_SYM32_ENC0_DP_SYM32_ENC_SDP_AUDIO_CONTROL0__ASP_ENABLE_MASK |
               DP_SYM32_ENC0_DP_SYM32_ENC_SDP_AUDIO_CONTROL0__ATP_ENABLE_MASK |
               DP_SYM32_ENC0_DP_SYM32_ENC_SDP_AUDIO_CONTROL0__AIP_ENABLE_MASK |
               DP_SYM32_ENC0_DP_SYM32_ENC_SDP_AUDIO_CONTROL0__ACM_ENABLE_MASK)))
        UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_SDP_CONTROL, se, HPO_S, SDP_STREAM_ENABLE, 0);
}

/* dcn31_hpo_dp_stream_enc_set_dsc_pps_info_packet */
void dcn32_hpo_se_set_dsc_pps_info_packet(rdna3_dev *d, uint32_t se, int enable, const uint8_t *pps,
                                          int immediate_update)
{
    if (enable && pps) {
        static const uint8_t hb[4] = { 0, 0x10, 127, 0 };   /* DC_DP_INFOFRAME_TYPE_PPS, 128 bytes - 1 */
        uint32_t i, v;
        /* the PPS packet size (128 bytes) */
        UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_SDP_GSP_CONTROL11, se, HPO_S, GSP_PAYLOAD_SIZE, 3);
        for (i = 0; i < 4; i++)
            dcn32_vpg_update_generic_info_packet(d, VPG_HPO0 + se, 11 + i, hb, pps + 32 * i, immediate_update);
        /* VBID[6] after the PPS line */
        UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_SDP_GSP_CONTROL11, se, HPO_S, GSP_TRANSMISSION_LINE_NUMBER, 2);
        v = RN(d, DP_SYM32_ENC0_DP_SYM32_ENC_VID_VBID_CONTROL, se, HPO_S);
        v = SET(v, DP_SYM32_ENC0_DP_SYM32_ENC_VID_VBID_CONTROL, VBID_6_COMPRESSEDSTREAM_FLAG_SOF_REFERENCE, 0);
        v = SET(v, DP_SYM32_ENC0_DP_SYM32_ENC_VID_VBID_CONTROL, VBID_6_COMPRESSEDSTREAM_FLAG_LINE_NUMBER, 3);
        WN(d, DP_SYM32_ENC0_DP_SYM32_ENC_VID_VBID_CONTROL, se, HPO_S, v);
        /* sent at that line every frame */
        UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_SDP_GSP_CONTROL11, se, HPO_S, GSP_VIDEO_CONTINUOUS_TRANSMISSION_ENABLE, 1);
        UPD(d, DP_SYM32_ENC0_DP_SYM32_ENC_SDP_CONTROL, se, HPO_S, SDP_STREAM_ENABLE, 1);
    } else {
        uint32_t v = RN(d, DP_SYM32_ENC0_DP_SYM32_ENC_SDP_GSP_CONTROL11, se, HPO_S);
        v = SET(v, DP_SYM32_ENC0_DP_SYM32_ENC_SDP_GSP_CONTROL11, GSP_VIDEO_CONTINUOUS_TRANSMISSION_ENABLE, 0);
        v = SET(v, DP_SYM32_ENC0_DP_SYM32_ENC_SDP_GSP_CONTROL11, GSP_PAYLOAD_SIZE, 0);
        WN(d, DP_SYM32_ENC0_DP_SYM32_ENC_SDP_GSP_CONTROL11, se, HPO_S, v);
    }
}

/* dcn31_hpo_dp_stream_enc_map_stream_to_link: DP_STREAM_MAPPER_CONTROL<se> */
void dcn32_hpo_se_map_stream_to_link(rdna3_dev *d, uint32_t se, uint32_t le)
{
    if (se >= RD_HPO_DP_STREAM_ENCS || le >= RD_HPO_DP_LINK_ENCS)
        return;
    UPD(d, DP_STREAM_MAPPER_CONTROL0, se, 1, DP_STREAM_LINK_TARGET, le);
}

/* dcn31_set_hblank_min_symbol_width */
void dcn32_hpo_se_set_hblank_min_symbol_width(rdna3_dev *d, uint32_t se, uint32_t width)
{
    WN(d, DP_SYM32_ENC0_DP_SYM32_ENC_HBLANK_CONTROL, se, HPO_S,
       FIELD(DP_SYM32_ENC0_DP_SYM32_ENC_HBLANK_CONTROL, HBLANK_MINIMUM_SYMBOL_WIDTH, width));
}

/* dcn31_hpo_dp_stream_enc_read_state */
void dcn32_hpo_se_read_state(rdna3_dev *d, uint32_t se, rdna3_hpo_se_state *s)
{
    uint32_t v;
    s->stream_enc_enabled = GET(RN(d, DP_SYM32_ENC0_DP_SYM32_ENC_CONTROL, se, HPO_S),
                                DP_SYM32_ENC0_DP_SYM32_ENC_CONTROL, DP_SYM32_ENC_ENABLE);
    s->vid_stream_enabled = GET(RN(d, DP_SYM32_ENC0_DP_SYM32_ENC_VID_STREAM_CONTROL, se, HPO_S),
                                DP_SYM32_ENC0_DP_SYM32_ENC_VID_STREAM_CONTROL, VID_STREAM_ENABLE);
    s->otg_inst = GET(RN(d, DP_STREAM_ENC0_DP_STREAM_ENC_INPUT_MUX_CONTROL, se, HPO_S),
                      DP_STREAM_ENC0_DP_STREAM_ENC_INPUT_MUX_CONTROL, DP_STREAM_ENC_INPUT_MUX_PIXEL_STREAM_SOURCE_SEL);
    v = RN(d, DP_SYM32_ENC0_DP_SYM32_ENC_VID_PIXEL_FORMAT, se, HPO_S);
    s->compressed_format = GET(v, DP_SYM32_ENC0_DP_SYM32_ENC_VID_PIXEL_FORMAT, PIXEL_ENCODING_TYPE);
    s->pixel_encoding = GET(v, DP_SYM32_ENC0_DP_SYM32_ENC_VID_PIXEL_FORMAT, UNCOMPRESSED_PIXEL_ENCODING);
    s->component_depth = GET(v, DP_SYM32_ENC0_DP_SYM32_ENC_VID_PIXEL_FORMAT, UNCOMPRESSED_COMPONENT_DEPTH);
    s->sdp_enabled = GET(RN(d, DP_SYM32_ENC0_DP_SYM32_ENC_SDP_CONTROL, se, HPO_S),
                         DP_SYM32_ENC0_DP_SYM32_ENC_SDP_CONTROL, SDP_STREAM_ENABLE);
    s->mapped_to_link_enc = se < RD_HPO_DP_STREAM_ENCS
                                ? GET(RN(d, DP_STREAM_MAPPER_CONTROL0, se, 1), DP_STREAM_MAPPER_CONTROL0,
                                      DP_STREAM_LINK_TARGET)
                                : 0;
}

/* ==================== HPO DP link encoder ==================== */

#define LC DP_DPHY_SYM320_DP_DPHY_SYM32_CONTROL

/* dcn31_hpo_dp_link_enc_enable */
void dcn32_hpo_le_enable(rdna3_dev *d, uint32_t le, uint32_t lanes)
{
    uint32_t dp_link_enabled = GET(RN(d, DP_DPHY_SYM320_DP_DPHY_SYM32_STATUS, le, HPO_S),
                                   DP_DPHY_SYM320_DP_DPHY_SYM32_STATUS, STATUS), v;
    /* the clocks first */
    UPD(d, DP_LINK_ENC0_DP_LINK_ENC_CLOCK_CONTROL, le, HPO_S, DP_LINK_ENC_CLOCK_EN, 1);
    /* the DPHY reset only from disabled to enabled */
    if (!dp_link_enabled) {
        UPD(d, DP_DPHY_SYM320_DP_DPHY_SYM32_CONTROL, le, HPO_S, DPHY_RESET, 1);
        UPD(d, DP_DPHY_SYM320_DP_DPHY_SYM32_CONTROL, le, HPO_S, DPHY_RESET, 0);
    }
    v = RN(d, LC, le, HPO_S);
    v = SET(v, DP_DPHY_SYM320_DP_DPHY_SYM32_CONTROL, DPHY_ENABLE, 1);
    v = SET(v, DP_DPHY_SYM320_DP_DPHY_SYM32_CONTROL, PRECODER_ENABLE, 1);
    v = SET(v, DP_DPHY_SYM320_DP_DPHY_SYM32_CONTROL, NUM_LANES, lanes == 1 ? 0 : lanes == 2 ? 1 : 3);
    WN(d, LC, le, HPO_S, v);
}

/* dcn31_hpo_dp_link_enc_disable */
void dcn32_hpo_le_disable(rdna3_dev *d, uint32_t le)
{
    UPD(d, DP_DPHY_SYM320_DP_DPHY_SYM32_CONTROL, le, HPO_S, DPHY_ENABLE, 0);
    /* the clock last */
    UPD(d, DP_LINK_ENC0_DP_LINK_ENC_CLOCK_CONTROL, le, HPO_S, DP_LINK_ENC_CLOCK_EN, 0);
}

static void tp_config(rdna3_dev *d, uint32_t le, uint32_t select, int prbs, uint32_t prbs_sel)
{
    uint32_t v = RN(d, DP_DPHY_SYM320_DP_DPHY_SYM32_TP_CONFIG, le, HPO_S);
    if (prbs) {
        /* REG_UPDATE_4 of the PRBS kind, then of the select */
        v = SET(v, DP_DPHY_SYM320_DP_DPHY_SYM32_TP_CONFIG, TP_PRBS_SEL0, prbs_sel);
        v = SET(v, DP_DPHY_SYM320_DP_DPHY_SYM32_TP_CONFIG, TP_PRBS_SEL1, prbs_sel);
        v = SET(v, DP_DPHY_SYM320_DP_DPHY_SYM32_TP_CONFIG, TP_PRBS_SEL2, prbs_sel);
        v = SET(v, DP_DPHY_SYM320_DP_DPHY_SYM32_TP_CONFIG, TP_PRBS_SEL3, prbs_sel);
        WN(d, DP_DPHY_SYM320_DP_DPHY_SYM32_TP_CONFIG, le, HPO_S, v);
    }
    v = SET(v, DP_DPHY_SYM320_DP_DPHY_SYM32_TP_CONFIG, TP_SELECT0, select);
    v = SET(v, DP_DPHY_SYM320_DP_DPHY_SYM32_TP_CONFIG, TP_SELECT1, select);
    v = SET(v, DP_DPHY_SYM320_DP_DPHY_SYM32_TP_CONFIG, TP_SELECT2, select);
    v = SET(v, DP_DPHY_SYM320_DP_DPHY_SYM32_TP_CONFIG, TP_SELECT3, select);
    WN(d, DP_DPHY_SYM320_DP_DPHY_SYM32_TP_CONFIG, le, HPO_S, v);
}

/* dcn31_hpo_dp_link_enc_set_link_test_pattern */
void dcn32_hpo_le_set_link_test_pattern(rdna3_dev *d, uint32_t le, uint32_t pattern, const uint8_t *custom)
{
    uint32_t mode = RD_DP2_TEST_PATTERN, i;
    switch (pattern) {
    case RD_HPO_TP_VIDEO_MODE:
        mode = RD_DP2_LINK_ACTIVE;
        break;
    case RD_HPO_TP_128b_132b_TPS1_TRAINING_MODE:
        mode = RD_DP2_LINK_TRAINING_TPS1;
        break;
    case RD_HPO_TP_128b_132b_TPS2_TRAINING_MODE:
        mode = RD_DP2_LINK_TRAINING_TPS2;
        break;
    case RD_HPO_TP_128b_132b_TPS1:
        tp_config(d, le, RD_DP_DPHY_TP_SELECT_TPS1, 0, 0);
        break;
    case RD_HPO_TP_128b_132b_TPS2:
        tp_config(d, le, RD_DP_DPHY_TP_SELECT_TPS2, 0, 0);
        break;
    case RD_HPO_TP_PRBS7:
    case RD_HPO_TP_PRBS9:
    case RD_HPO_TP_PRBS11:
    case RD_HPO_TP_PRBS15:
    case RD_HPO_TP_PRBS23:
    case RD_HPO_TP_PRBS31:
        tp_config(d, le, RD_DP_DPHY_TP_SELECT_PRBS, 1, pattern - RD_HPO_TP_PRBS7);
        break;
    case RD_HPO_TP_264BIT_CUSTOM:
        if (!custom)
            return;
        for (i = 0; i < 11; i++)
            wr(d, regDP_DPHY_SYM320_DP_DPHY_SYM32_TP_CUSTOM0_BASE_IDX,
               (uint32_t)regDP_DPHY_SYM320_DP_DPHY_SYM32_TP_CUSTOM0 + i + HPO_S * le,
               FIELD(DP_DPHY_SYM320_DP_DPHY_SYM32_TP_CUSTOM0, TP_CUSTOM,
                     (uint32_t)custom[3 * i + 2] << 16 | (uint32_t)custom[3 * i + 1] << 8 | custom[3 * i]));
        tp_config(d, le, RD_DP_DPHY_TP_SELECT_CUSTOM, 0, 0);
        break;
    case RD_HPO_TP_SQUARE:
        WN(d, DP_DPHY_SYM320_DP_DPHY_SYM32_TP_SQ_PULSE, le, HPO_S,
           FIELD(DP_DPHY_SYM320_DP_DPHY_SYM32_TP_SQ_PULSE, TP_SQ_PULSE_WIDTH, custom ? custom[0] : 0));
        tp_config(d, le, RD_DP_DPHY_TP_SELECT_SQUARE, 0, 0);
        break;
    default:
        return;
    }
    UPD(d, DP_DPHY_SYM320_DP_DPHY_SYM32_CONTROL, le, HPO_S, MODE, mode);
}

/* fill_stream_allocation_row_info (HPO): the HPO stream encoder of the row */
static void hpo_fill_stream_allocation_row_info(const rdna3_mst_alloc_table *t, uint32_t row, uint32_t *src,
                                                uint32_t *slots)
{
    if (row < t->stream_count && t->a[row].fe >= 0) {
        *src = (uint32_t)t->a[row].fe;
        *slots = t->a[row].slot_count;
    } else {
        *src = 0;
        *slots = 0;
    }
}

/* dcn31_hpo_dp_link_enc_update_stream_allocation_table: the VC payload
 * table on the TX side, then SAT_UPDATE 1 (update with the ACT); the
 * update takes 109..868 us */
void dcn32_hpo_le_update_stream_allocation_table(rdna3_dev *d, uint32_t le, const rdna3_mst_alloc_table *t)
{
    uint32_t row, src, slots, v;
    for (row = 0; row < 4; row++) {
        hpo_fill_stream_allocation_row_info(t, row, &src, &slots);
        v = RN(d, DP_DPHY_SYM320_DP_DPHY_SYM32_SAT_VC0, le * HPO_S + row, 1);
        v = SET(v, DP_DPHY_SYM320_DP_DPHY_SYM32_SAT_VC0, SAT_STREAM_SOURCE, src);
        v = SET(v, DP_DPHY_SYM320_DP_DPHY_SYM32_SAT_VC0, SAT_SLOT_COUNT, slots);
        WN(d, DP_DPHY_SYM320_DP_DPHY_SYM32_SAT_VC0, le * HPO_S + row, 1, v);
    }
    /* 0 no action, 1 update with the trigger, 2 without */
    UPD(d, DP_DPHY_SYM320_DP_DPHY_SYM32_SAT_UPDATE, le, HPO_S, SAT_UPDATE, 1);
    if (WAIT(d, DP_DPHY_SYM320_DP_DPHY_SYM32_STATUS, le, HPO_S, SAT_UPDATE_PENDING, 0, 100 * 200))
        RERR(d, "hpo: link encoder %u: SAT_UPDATE_PENDING stays set", le);
}

/* dcn31_hpo_dp_link_enc_set_throttled_vcp_size: X.Y with 25 fraction bits */
void dcn32_hpo_le_set_throttled_vcp_size(rdna3_dev *d, uint32_t le, uint32_t se, rd_fixpt avg_time_slots_per_mtp,
                                         uint32_t *xo, uint32_t *yo)
{
    uint32_t x = (uint32_t)dc_fixpt_floor(avg_time_slots_per_mtp);
    uint32_t y = (uint32_t)dc_fixpt_ceil(dc_fixpt_shl(dc_fixpt_sub_int(avg_time_slots_per_mtp, x), 25));
    /* y rounding up to an integer carries into x */
    if (y >> 25) {
        x += 1;
        y = 0;
    }
    if (se < 4)
        WN(d, DP_DPHY_SYM320_DP_DPHY_SYM32_VC_RATE_CNTL0, le * HPO_S + se, 1,
           FIELD(DP_DPHY_SYM320_DP_DPHY_SYM32_VC_RATE_CNTL0, STREAM_VC_RATE_X, x) |
               FIELD(DP_DPHY_SYM320_DP_DPHY_SYM32_VC_RATE_CNTL0, STREAM_VC_RATE_Y, y));
    if (xo)
        *xo = x;
    if (yo)
        *yo = y;
    /* 116..903 ns on the link */
    WAIT(d, DP_DPHY_SYM320_DP_DPHY_SYM32_STATUS, le, HPO_S, RATE_UPDATE_PENDING, 0, 10);
}

/* dcn31_hpo_dp_link_enc_read_state */
void dcn32_hpo_le_read_state(rdna3_dev *d, uint32_t le, rdna3_hpo_le_state *s)
{
    uint32_t v, i;
    s->link_enc_enabled = GET(RN(d, DP_DPHY_SYM320_DP_DPHY_SYM32_STATUS, le, HPO_S),
                              DP_DPHY_SYM320_DP_DPHY_SYM32_STATUS, STATUS);
    v = RN(d, LC, le, HPO_S);
    s->lane_count = GET(v, DP_DPHY_SYM320_DP_DPHY_SYM32_CONTROL, NUM_LANES);
    s->link_mode = GET(v, DP_DPHY_SYM320_DP_DPHY_SYM32_CONTROL, MODE);
    for (i = 0; i < 4; i++) {
        v = RN(d, DP_DPHY_SYM320_DP_DPHY_SYM32_SAT_VC0, le * HPO_S + i, 1);
        s->stream_src[i] = GET(v, DP_DPHY_SYM320_DP_DPHY_SYM32_SAT_VC0, SAT_STREAM_SOURCE);
        s->slot_count[i] = GET(v, DP_DPHY_SYM320_DP_DPHY_SYM32_SAT_VC0, SAT_SLOT_COUNT);
        v = RN(d, DP_DPHY_SYM320_DP_DPHY_SYM32_VC_RATE_CNTL0, le * HPO_S + i, 1);
        s->vc_rate_x[i] = GET(v, DP_DPHY_SYM320_DP_DPHY_SYM32_VC_RATE_CNTL0, STREAM_VC_RATE_X);
        s->vc_rate_y[i] = GET(v, DP_DPHY_SYM320_DP_DPHY_SYM32_VC_RATE_CNTL0, STREAM_VC_RATE_Y);
    }
}

/* link_transmitter_control -> transmitter_control_v1_7: HPO_instance is
 * hpo_engine_id - ENGINE_ID_HPO_DP_0 for a DP signal, with DC's uint8_t
 * arithmetic for the commands that leave hpo_engine_id 0 (ENGINE_ID_DIGA) */
#define RD_ENGINE_ID_HPO_DP_0 12
static int hpo_transmitter_control(rdna3_dmub *dm, uint8_t action, uint32_t transmitter, int hpd, uint8_t mode_laneset,
                                   uint32_t lanes, uint32_t pixel_clock_khz, int hpo_engine_id)
{
    rdna3_dig_tx_v1_7 t = { 0 };
    if (!dm || !dm->hw_init)
        return RDNA3_ENODEV;
    t.phyid = (uint8_t)transmitter;
    t.action = action;
    t.digmode = mode_laneset;
    t.lanenum = (uint8_t)lanes;
    t.hpdsel = (uint8_t)(hpd >= 0 ? hpd + 1 : 0);
    t.digfe_sel = 0;   /* dig_encoder_sel_to_atom(ENGINE_ID_UNKNOWN) */
    t.connobj_id = 0;
    t.hpo_instance = (uint8_t)((uint8_t)hpo_engine_id - RD_ENGINE_ID_HPO_DP_0);
    t.symclk_10khz = pixel_clock_khz / 10;
    return dmub32_transmitter_control(dm, &t);
}

/* ATOM_TRANSMITTER_DIGMODE_V6_DP_MST: what signal_type_to_atom_dig_mode
 * gives SIGNAL_TYPE_DISPLAY_PORT_MST */
#define RD_ATOM_DIGMODE_DP_MST 5

/* dcn31_hpo_dp_link_enc_enable_dp_output: TransmitterControl ENABLE as an
 * MST signal on the HPO engine, the link rate x 1000 as the symbol clock */
int dcn32_hpo_le_enable_dp_output(rdna3_dmub *dm, uint32_t le, uint32_t transmitter, int hpd, uint32_t lanes,
                                  uint32_t link_rate)
{
    int r = hpo_transmitter_control(dm, RD_ATOM_TX_ENABLE, transmitter, hpd, RD_ATOM_DIGMODE_DP_MST, lanes,
                                    link_rate * 1000, (int)le + RD_ENGINE_ID_HPO_DP_0);
    if (r && dm && dm->d)
        RERR(dm->d, "hpo: link encoder %u: TransmitterControl ENABLE on UNIPHY %u failed (%d)", le, transmitter, r);
    return r;
}

/* dcn31_hpo_dp_link_enc_disable_output: TransmitterControl DISABLE for the
 * stream's signal ('digmode'), then dcn31_hpo_dp_link_enc_disable */
int dcn32_hpo_le_disable_output(rdna3_dmub *dm, rdna3_dev *d, uint32_t le, uint32_t transmitter, int hpd,
                                uint8_t digmode)
{
    int r = hpo_transmitter_control(dm, RD_ATOM_TX_DISABLE, transmitter, hpd, digmode, 0, 0, 0);
    if (r) {
        RERR(d, "hpo: link encoder %u: TransmitterControl DISABLE on UNIPHY %u failed (%d)", le, transmitter, r);
        return r;
    }
    dcn32_hpo_le_disable(d, le);
    return RDNA3_OK;
}

/* dcn31_hpo_dp_link_enc_set_ffe: SET_VOLTAGE_AND_PREEMPASIS with the FFE
 * preset as the lane settings (DC leaves hpd_sel 0 = HPD_SOURCEID1) */
int dcn32_hpo_le_set_ffe(rdna3_dmub *dm, uint32_t le, uint32_t transmitter, uint32_t lanes, uint32_t link_rate,
                         uint8_t ffe_preset)
{
    (void)le;
    return hpo_transmitter_control(dm, RD_ATOM_TX_SETUP_VSEMPH, transmitter, 0, ffe_preset, lanes, link_rate * 1000,
                                   0);
}

/* ==================== DCCG ==================== */

/* dccg32_get_pixel_rate_div */
void dcn32_dccg_get_pixel_rate_div(rdna3_dev *d, uint32_t otg, uint32_t *k1, uint32_t *k2)
{
    uint32_t v = RN(d, OTG_PIXEL_RATE_DIV, 0, 0);
    *k1 = *k2 = RD_PIXEL_RATE_DIV_NA;
    if (otg >= 4)
        return;
    /* OTGn_PIXEL_RATE_DIVK1 (1 bit) and DIVK2 (2 bits), 3 bits per OTG */
    *k1 = (v >> (3 * otg)) & 1u;
    *k2 = (v >> (3 * otg + 1)) & 3u;
}

/* dccg32_set_pixel_rate_div */
void dcn32_dccg_set_pixel_rate_div(rdna3_dev *d, uint32_t otg, uint32_t k1, uint32_t k2)
{
    uint32_t cur_k1, cur_k2, v;
    /* 0xF does not fit the 1 / 2 bit fields */
    if (k1 == RD_PIXEL_RATE_DIV_NA || k2 == RD_PIXEL_RATE_DIV_NA || otg >= 4)
        return;
    dcn32_dccg_get_pixel_rate_div(d, otg, &cur_k1, &cur_k2);
    if (k1 == cur_k1 && k2 == cur_k2)
        return;
    v = RN(d, OTG_PIXEL_RATE_DIV, 0, 0);
    v &= ~(7u << (3 * otg));
    v |= ((k1 & 1u) | (k2 & 3u) << 1) << (3 * otg);
    WN(d, OTG_PIXEL_RATE_DIV, 0, 0, v);
}

/* dccg32_set_dtbclk_p_src: DTBCLK_P<otg> from DTBCLK0 (2) / DPREFCLK (0), off for REFCLK */
void dcn32_dccg_set_dtbclk_p_src(rdna3_dev *d, uint32_t src, uint32_t otg)
{
    uint32_t p_src_sel = src == RD_STREAMCLK_DTBCLK0 ? 2 : 0, v;
    /* DTBCLK_P<n>_SRC_SEL (2 bits) and _EN, 3 bits per OTG */
    const uint32_t en = DTBCLK_P_CNTL__DTBCLK_P0_EN_MASK << (3 * otg);
    const uint32_t sel = DTBCLK_P_CNTL__DTBCLK_P0_SRC_SEL_MASK << (3 * otg);
    if (otg >= 4)
        return;
    v = RN(d, DTBCLK_P_CNTL, 0, 0);
    if (src == RD_STREAMCLK_REFCLK)
        v &= ~en;
    else
        v = (v & ~(en | sel)) | (p_src_sel << (3 * otg)) | en;
    WN(d, DTBCLK_P_CNTL, 0, 0, v);
}

/* dccg32_set_dtbclk_dto: the pixel valid for the OTG -> HPO path, the
 * DTO output at pixel rate / 4 */
void dcn32_dccg_set_dtbclk_dto(rdna3_dev *d, uint32_t otg, uint32_t pixclk_khz, uint32_t ref_dtbclk_khz, int is_hdmi)
{
    uint32_t req_dtbclk_khz = pixclk_khz / 4, v;
    if (otg >= 4)
        return;
    if (ref_dtbclk_khz && req_dtbclk_khz) {
        /* phase / modulo = dtbclk / dtbclk ref */
        WN(d, DTBCLK_DTO0_MODULO, otg, 1, ref_dtbclk_khz * 1000);
        WN(d, DTBCLK_DTO0_PHASE, otg, 1, req_dtbclk_khz * 1000);
        UPD(d, OTG0_PIXEL_RATE_CNTL, otg, PRC_S, DTBCLK_DTO0_ENABLE, 1);
        if (WAIT(d, OTG0_PIXEL_RATE_CNTL, otg, PRC_S, DTBCLKDTO0_ENABLE_STATUS, 1, 100))
            RERR(d, "dccg: OTG %u: DTBCLK DTO not enabled", otg);
        /* OTG_PIXEL_RATE_DIV: DIVK1 and DIVK2 by 1 */
        dcn32_dccg_set_pixel_rate_div(d, otg, RD_PIXEL_RATE_DIV_BY_1, RD_PIXEL_RATE_DIV_BY_1);
        /* the DTO source select only after the DTO is on */
        UPD(d, OTG0_PIXEL_RATE_CNTL, otg, PRC_S, PIPE0_DTO_SRC_SEL, 2);
    } else {
        v = RN(d, OTG0_PIXEL_RATE_CNTL, otg, PRC_S);
        v = SET(v, OTG0_PIXEL_RATE_CNTL, DTBCLK_DTO0_ENABLE, 0);
        v = SET(v, OTG0_PIXEL_RATE_CNTL, PIPE0_DTO_SRC_SEL, is_hdmi ? 0 : 1);
        WN(d, OTG0_PIXEL_RATE_CNTL, otg, PRC_S, v);
        WN(d, DTBCLK_DTO0_MODULO, otg, 1, 0);
        WN(d, DTBCLK_DTO0_PHASE, otg, 1, 0);
    }
}

/* dccg32_set_dpstreamclk: DTBCLK_P from DTBCLK0 (always), DPSTREAMCLK<hpo>
 * on (off for REFCLK) with the OTG as its source */
void dcn32_dccg_set_dpstreamclk(rdna3_dev *d, uint32_t src, uint32_t otg, uint32_t hpo_se)
{
    uint32_t v;
    dcn32_dccg_set_dtbclk_p_src(d, RD_STREAMCLK_DTBCLK0, otg);
    if (hpo_se >= 4)
        return;
    v = RN(d, DPSTREAMCLK_CNTL, 0, 0);
    v &= ~((DPSTREAMCLK_CNTL__DPSTREAMCLK0_EN_MASK | DPSTREAMCLK_CNTL__DPSTREAMCLK0_SRC_SEL_MASK) << (4 * hpo_se));
    v |= ((src == RD_STREAMCLK_REFCLK ? 0u : DPSTREAMCLK_CNTL__DPSTREAMCLK0_EN_MASK) |
          ((otg << DPSTREAMCLK_CNTL__DPSTREAMCLK0_SRC_SEL__SHIFT) & DPSTREAMCLK_CNTL__DPSTREAMCLK0_SRC_SEL_MASK))
         << (4 * hpo_se);
    WN(d, DPSTREAMCLK_CNTL, 0, 0, v);
}

/* dccg31_enable_symclk32_se: SYMCLK32_SE<hpo> from PHYD32CLK<phy> */
void dcn32_dccg_enable_symclk32_se(rdna3_dev *d, uint32_t hpo_se, uint32_t phyd32clk)
{
    uint32_t v;
    if (hpo_se >= 4)
        return;
    v = RN(d, SYMCLK32_SE_CNTL, 0, 0);
    v &= ~((SYMCLK32_SE_CNTL__SYMCLK32_SE0_SRC_SEL_MASK | SYMCLK32_SE_CNTL__SYMCLK32_SE0_EN_MASK) << (4 * hpo_se));
    v |= (((phyd32clk << SYMCLK32_SE_CNTL__SYMCLK32_SE0_SRC_SEL__SHIFT) & SYMCLK32_SE_CNTL__SYMCLK32_SE0_SRC_SEL_MASK) |
          SYMCLK32_SE_CNTL__SYMCLK32_SE0_EN_MASK) << (4 * hpo_se);
    WN(d, SYMCLK32_SE_CNTL, 0, 0, v);
}

/* dccg31_disable_symclk32_se: back to REFCLK */
void dcn32_dccg_disable_symclk32_se(rdna3_dev *d, uint32_t hpo_se)
{
    uint32_t v;
    if (hpo_se >= 4)
        return;
    v = RN(d, SYMCLK32_SE_CNTL, 0, 0);
    v &= ~((SYMCLK32_SE_CNTL__SYMCLK32_SE0_SRC_SEL_MASK | SYMCLK32_SE_CNTL__SYMCLK32_SE0_EN_MASK) << (4 * hpo_se));
    WN(d, SYMCLK32_SE_CNTL, 0, 0, v);
}
