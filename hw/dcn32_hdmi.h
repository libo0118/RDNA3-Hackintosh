/*
 * dcn32_hdmi.h - the HDMI / DVI side of the DCN 3.2 DIG stream encoder and
 * of the link, ported from Linux amdgpu Display Core (v6.12) as DC runs it
 * for a TMDS stream:
 *
 *   dcn32_dio_stream_encoder.c  enc32_stream_encoder_hdmi_set_stream_attribute
 *                               (HDMI_CONTROL: packet generation, deep color,
 *                               the scrambler and the 1/4 clock channel rate
 *                               above 340 MHz; GC / NULL packets, the audio
 *                               info frame line), enc32_stream_encoder_dvi_set_stream_attribute
 *   dcn10_stream_encoder.c      enc1_stream_encoder_set_stream_attribute_helper,
 *                               enc1_stream_encoder_set_avmute,
 *                               enc1_reset_hdmi_stream_attribute
 *   dcn30_dio_stream_encoder.c  enc3_stream_encoder_update_hdmi_info_packets,
 *                               enc3_update_hdmi_info_packet,
 *                               enc3_stream_encoder_stop_hdmi_info_packets
 *                               (infoframes through the VPG's generic packets)
 *   dc_resource.c               set_avi_info_frame (the AVI infoframe)
 *   link_ddc.c                  write_scdc_data / read_scdc_data (SCDC over
 *                               the DDC: scrambling, TMDS bit clock ratio)
 *   link_validation.c           get_tmds_output_pixel_clock_100hz
 *   dcn10_link_encoder.c        dcn10_link_encoder_validate_output_with_stream
 *                               and its DVI / HDMI / DP checks
 *   dcn30_hwseq.c               dcn30_set_avmute (two frames for the mute)
 *
 * The VBIOS half of the stream setup (encoder_control: ATOM_ENCODER_CMD
 * STREAM_SETUP through the DMCUB) stays with the caller (hw/dcn32_modeset.c).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCN32_HDMI_H
#define RDNA3_DCN32_HDMI_H

#include "rdna3_hw.h"
#include "edid.h"
#include "dcn32_otg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* struct dc_info_packet */
typedef struct rdna3_info_packet {
    int      valid;
    uint8_t  hb0, hb1, hb2, hb3;
    uint8_t  sb[32];
} rdna3_info_packet;

/* struct encoder_info_frame (the HDMI part) */
typedef struct rdna3_hdmi_info_frames {
    rdna3_info_packet avi, vendor, gamut, spd, hdrsmd, hfvsif, vtem;
} rdna3_hdmi_info_frames;

/* enum dc_color_space, the ones set_avi_info_frame tells apart */
enum {
    RD_CS_SRGB = 0, RD_CS_SRGB_LIMITED, RD_CS_YCBCR601, RD_CS_YCBCR709, RD_CS_YCBCR601_LIMITED,
    RD_CS_YCBCR709_LIMITED, RD_CS_2020_RGB_FULLRANGE, RD_CS_2020_RGB_LIMITEDRANGE, RD_CS_2020_YCBCR,
    RD_CS_ADOBERGB,
};
/* enum dc_aspect_ratio */
enum { RD_ASPECT_RATIO_NO_DATA = 0, RD_ASPECT_RATIO_4_3, RD_ASPECT_RATIO_16_9, RD_ASPECT_RATIO_64_27,
       RD_ASPECT_RATIO_256_135 };

#define RD_HDMI_INFOFRAME_TYPE_AVI   0x82
#define RD_HDMI_AVI_INFOFRAME_SIZE   13
/* HDMI_CLOCK_CHANNEL_RATE_MORE_340M (kHz): the scrambler and the 1/4 rate */
#define RD_HDMI_CLOCK_CHANNEL_RATE_MORE_340M 340000
/* TMDS limits (dc_hw_types.h) and DCN 3.2's link encoder caps */
#define RD_TMDS_MIN_PIXEL_CLOCK      25000
#define RD_TMDS_MAX_PIXEL_CLOCK      165000
#define RD_DCN32_MAX_HDMI_PIXEL_CLOCK 600000

/* the picture aspect of a CTA VIC (drm's edid_cea_modes picture_aspect):
 * VICs 1, 2, 17 are 4:3, the other progressive VICs this port knows 16:9 */
uint32_t dcn32_hdmi_vic_aspect(uint32_t vic);

/* set_avi_info_frame: AVI InfoFrame v2 (v3 for VIC >= 128) for a stream of
 * pixel encoding 'f->pixel_encoding' in 'color_space' (RD_CS_*), VIC
 * 'vic' (0: none), picture 'aspect', with the bar info of 'm' (no
 * borders); 'qs_bit' = the sink selects RGB quantization (full / limited
 * signalled), else the default range */
void dcn32_hdmi_build_avi(rdna3_info_packet *p, const rdna3_crtc_format *f, uint32_t color_space, uint32_t vic,
                          uint32_t aspect, int qs_bit, const rdna3_mode *m);

/* drm_hdmi_vendor_infoframe_from_display_mode's HDMI VIC (drm_match_hdmi_mode)
 * for an HDMI sink: the 4K CTA VICs 95, 94, 93, 98 are HDMI VICs 1-4, and
 * their AVI VIC is 0 (drm_mode_cea_vic) */
uint32_t dcn32_hdmi_vic_from_cea(uint32_t vic);
/* mod_build_hf_vsif_infopacket without 3D: the HDMI vendor infoframe that
 * carries an HDMI VIC (h_total >= 3840, v_total >= 2160), else !valid */
void dcn32_hdmi_build_vsif(rdna3_info_packet *p, uint32_t hdmi_vic, uint32_t h_total, uint32_t v_total);
/* what resource_build_info_frame gives an HDMI stream of mode 'm' in
 * amdgpu_dm's defaults (no borders, no 3D, sRGB, content type "no data",
 * qs_bit 0): VIC (drm_match_cea_mode) with the picture aspect of a CTA
 * mode, AVI, the vendor infoframe of an HDMI VIC; no SPD (FreeSync off),
 * no HDR metadata. 'hdmi_sink': the sink has an HDMI VSDB. */
void dcn32_hdmi_build_info_frames(rdna3_hdmi_info_frames *f, const rdna3_mode *m, const rdna3_crtc_format *fmt,
                                  int hdmi_sink);

/* enc3_update_hdmi_info_packet: packet 'index' (0..14) through VPG 'fe'
 * (one VPG per DIG front end) and its HDMI_GENERICn controls: sent every
 * frame on line 2, or off when !valid */
void dcn32_enc_update_hdmi_info_packet(rdna3_dev *d, uint32_t fe, uint32_t index, const rdna3_info_packet *p);
/* enc3_stream_encoder_update_hdmi_info_packets: HDMI_DB_DISABLE, the AFMT
 * clock, then AVI (0), HF-VSIF (5), gamut (2), vendor (1), SPD (3), HDR
 * static metadata (4), VTEM (6) */
void dcn32_enc_update_hdmi_info_packets(rdna3_dev *d, uint32_t fe, const rdna3_hdmi_info_frames *f);
/* enc3_stream_encoder_stop_hdmi_info_packets: generic packets 0-14 off */
void dcn32_enc_stop_hdmi_info_packets(rdna3_dev *d, uint32_t fe);

/* enc32_stream_encoder_hdmi_set_stream_attribute after the VBIOS
 * STREAM_SETUP: TMDS pixel encoding, HDMI_CONTROL (deep color, the
 * scrambler + 1/4 clock channel at >= 340 MHz or with lte_340_scramble),
 * GC / NULL packets every frame, ACP off, the audio info frame on line 2,
 * AV mute off */
void dcn32_enc_hdmi_set_stream_attribute(rdna3_dev *d, uint32_t fe, const rdna3_crtc_format *f,
                                         uint32_t actual_pix_clk_khz, int lte_340_scramble);
/* enc32_stream_encoder_dvi_set_stream_attribute after the VBIOS STREAM_SETUP */
void dcn32_enc_dvi_set_stream_attribute(rdna3_dev *d, uint32_t fe, const rdna3_crtc_format *f);
/* enc1_stream_encoder_set_avmute; with 'otg' >= 0 and enable, dcn30_set_avmute's
 * wait of two frames for the mute to go out */
void dcn32_enc_set_avmute(rdna3_dev *d, uint32_t fe, int enable, int otg);
/* enc1_reset_hdmi_stream_attribute: HDMI 1.4 packets, no deep color, no scrambling */
void dcn32_enc_reset_hdmi_stream_attribute(rdna3_dev *d, uint32_t fe);
/* HDMI_CONTROL as programmed (the scrambler / clock ratio bits for the log) */
uint32_t dcn32_enc_hdmi_control(rdna3_dev *d, uint32_t fe);

/* write_scdc_data on DDC line 'ddc': the source version when the sink
 * reports SCDC version 1, then TMDS_CONFIG = 3 (scrambling + 1/40 bit
 * clock ratio) above 340 MHz, 1 with lte_340_scramble, else 0. Returns the
 * first I2C error (DC ignores them; the caller logs). */
#define RD_HDMI_SCDC_ADDRESS        0x54
#define RD_HDMI_SCDC_SINK_VERSION   0x01
#define RD_HDMI_SCDC_SOURCE_VERSION 0x02
#define RD_HDMI_SCDC_TMDS_CONFIG    0x20
#define RD_HDMI_SCDC_SCRAMBLER_STATUS 0x21
#define RD_HDMI_SCDC_STATUS_FLAGS   0x40
int  dcn32_hdmi_write_scdc(rdna3_dev *d, uint32_t ddc, uint32_t pix_clk_khz, int lte_340_scramble);
/* read_scdc_data: TMDS_CONFIG, and with scrambling on the scrambler status
 * and the status flags (clock detected, channels locked) */
typedef struct rdna3_scdc_state {
    uint8_t tmds_config, scrambler_status, status_flags[2];
} rdna3_scdc_state;
int  dcn32_hdmi_read_scdc(rdna3_dev *d, uint32_t ddc, rdna3_scdc_state *s);

/* get_tmds_output_pixel_clock_100hz: the TMDS character rate of a stream
 * (4:2:0 halves, 4:2:2 2/3, deep color x bpc / 8) */
uint32_t dcn32_tmds_output_pixel_clock_100hz(uint32_t pix_clk_100hz, const rdna3_crtc_format *f);

/* enum signal_type, the ones the link encoder validates */
enum { RD_SIGNAL_DVI_SINGLE_LINK = 1, RD_SIGNAL_DVI_DUAL_LINK, RD_SIGNAL_HDMI_TYPE_A, RD_SIGNAL_DISPLAY_PORT,
       RD_SIGNAL_DISPLAY_PORT_MST };
/* the link encoder features DCN 3.2's constructor sets (max HDMI
 * 600 MHz, 12 bpc, 4:2:0 on HDMI and DP; HDMI_6GB_EN from the VBIOS) */
typedef struct rdna3_link_enc_features {
    uint32_t max_hdmi_pixel_clock;   /* kHz */
    uint32_t max_hdmi_deep_color;    /* RD_COLOR_DEPTH_* */
    int      hdmi_ycbcr420_supported, dp_ycbcr420_supported, hdmi_6gb_en;
} rdna3_link_enc_features;
void dcn32_link_enc_features(uint32_t vbios_encoder_caps, rdna3_link_enc_features *ft);
/* dcn10_link_encoder_validate_output_with_stream: 1 when the encoder can
 * drive the stream. 'phy_pix_clk_khz' is the TMDS rate for HDMI;
 * 'edid_max_tmds_clk_mhz' the sink's limit (0 none) */
int  dcn32_link_enc_validate_output(const rdna3_link_enc_features *ft, uint32_t signal, uint32_t connector_signal,
                                    const rdna3_crtc_format *f, uint32_t pix_clk_khz, uint32_t phy_pix_clk_khz,
                                    uint32_t edid_max_tmds_clk_mhz);

#ifdef __cplusplus
}
#endif
#endif
