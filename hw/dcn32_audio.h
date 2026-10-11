/*
 * dcn32_audio.h - the display side of HDMI / DisplayPort audio on DCN 3.2,
 * ported from Linux amdgpu Display Core (v6.12, MIT):
 *
 *   dce/dce_audio.c        the azalia endpoints (AZALIA_F0 "pins"):
 *                          dce_aud_hw_init, dce_aud_endpoint_valid,
 *                          dce_aud_az_enable / _disable / _disable_hbr_audio,
 *                          dce_aud_az_configure (speaker allocation, the
 *                          short audio descriptors, lip sync, the sink
 *                          info = the ELD the codec answers with),
 *                          check_audio_bandwidth (HDMI table 7.5, DP SDP
 *                          budget), dce_aud_wall_dto_setup (the DCCG
 *                          audio DTO: DTO0 from an HDMI pixel clock, DTO1
 *                          from DPREFCLK)
 *   dcn30/dcn30_afmt.c     afmt3_setup_hdmi_audio, afmt3_se_audio_setup,
 *                          afmt3_audio_mute_control, afmt3_setup_dp_audio,
 *                          afmt3_audio_info_immediate_update
 *   dio/dcn30 + dcn10      enc3_se_hdmi_audio_setup (HDMI ACR: N / CTS from
 *                          get_audio_clock_info), enc3_se_dp_audio_setup /
 *                          _enable, enc3_audio_mute_control,
 *                          enc1_se_enable_audio_clock, enc1_se_enable_dp_audio,
 *                          enc1_se_dp_audio_disable, enc1_se_hdmi_audio_disable
 *   hpo/dcn31 + dcn31_apg.c dcn31_hpo_dp_stream_enc_audio_setup / _enable /
 *                          _disable (the 128b/132b audio SDPs through the APG)
 *   link/hwss              setup_dio_audio_output, enable / disable_dio_audio_packet,
 *                          setup_hpo_dp_audio_output, enable / disable_hpo_dp_audio_packet
 *   hwss/dce110            build_audio_output, populate_audio_dp_link_info,
 *                          dce110_setup_audio_dto, dce110_enable_audio_stream,
 *                          dce110_disable_audio_stream
 *   core/dc_resource.c     update_num_audio, the endpoint count of
 *                          resource_construct, find_first_free_audio,
 *                          update_audio_usage
 *   core/dc_stream.c       dc_stream_construct's audio_info (from the EDID caps)
 *
 * The HD Audio codec itself (the azalia verbs, the streams) belongs to the
 * macOS HDA driver; this side fills the endpoint registers the codec
 * reports (ELD, connection type, rates), clocks the audio DTO and turns the
 * audio packets of the stream encoder on.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCN32_AUDIO_H
#define RDNA3_DCN32_AUDIO_H

#include "rdna3_hw.h"
#include "edid.h"
#include "dcn32_hdmi.h"   /* RD_SIGNAL_*, rdna3_crtc_format */
#include "dcn32_dp.h"     /* RD_DP_*_ENCODING, link rates */

#ifdef __cplusplus
extern "C" {
#endif

/* DCN 3.2's audio pins (dcn32_resource.c num_audio) */
#define RD_AUDIO_MAX_ENDPOINTS 5
/* AUDIO_INFO_DISPLAY_NAME_SIZE_IN_CHARS, MAX_HW_AUDIO_INFO_DISPLAY_NAME_SIZE_IN_CHARS */
#define RD_AUDIO_DISPLAY_NAME_SIZE 20
#define RD_AUDIO_HW_DISPLAY_NAME_SIZE 18
/* AUDIO_FORMAT_CODE_* (CTA-861 short audio descriptor codes) */
enum {
    RD_AUDIO_FORMAT_CODE_LINEARPCM = 1, RD_AUDIO_FORMAT_CODE_AC3, RD_AUDIO_FORMAT_CODE_MPEG1,
    RD_AUDIO_FORMAT_CODE_MP3, RD_AUDIO_FORMAT_CODE_MPEG2, RD_AUDIO_FORMAT_CODE_AAC, RD_AUDIO_FORMAT_CODE_DTS,
    RD_AUDIO_FORMAT_CODE_ATRAC, RD_AUDIO_FORMAT_CODE_1BITAUDIO, RD_AUDIO_FORMAT_CODE_DOLBYDIGITALPLUS,
    RD_AUDIO_FORMAT_CODE_DTS_HD, RD_AUDIO_FORMAT_CODE_MAT_MLP, RD_AUDIO_FORMAT_CODE_DST,
    RD_AUDIO_FORMAT_CODE_WMAPRO, RD_AUDIO_FORMAT_CODE_LAST,
};
#define RD_AUDIO_FORMAT_CODE_COUNT (RD_AUDIO_FORMAT_CODE_LAST - RD_AUDIO_FORMAT_CODE_LINEARPCM)
/* union audio_sample_rates */
#define RD_AUDIO_RATE_32    (1u << 0)
#define RD_AUDIO_RATE_44_1  (1u << 1)
#define RD_AUDIO_RATE_48    (1u << 2)
#define RD_AUDIO_RATE_88_2  (1u << 3)
#define RD_AUDIO_RATE_96    (1u << 4)
#define RD_AUDIO_RATE_176_4 (1u << 5)
#define RD_AUDIO_RATE_192   (1u << 6)
/* struct audio_speaker_flags (= the CTA speaker allocation byte) */
#define RD_SPK_FL_FR   (1u << 0)
#define RD_SPK_LFE     (1u << 1)
#define RD_SPK_FC      (1u << 2)
#define RD_SPK_RL_RR   (1u << 3)
#define RD_SPK_RC      (1u << 4)
#define RD_SPK_FLC_FRC (1u << 5)
#define RD_SPK_RLC_RRC (1u << 6)
#define RD_SPK_SUPPORT_AI (1u << 7)

/* struct audio_mode */
typedef struct rdna3_audio_mode {
    uint8_t format_code;     /* RD_AUDIO_FORMAT_CODE_* */
    uint8_t channel_count;
    uint8_t sample_rates;    /* RD_AUDIO_RATE_* */
    uint8_t sample_size;     /* LPCM sample size / max bit rate / vendor specific: SAD byte 3 */
} rdna3_audio_mode;

/* struct audio_info */
typedef struct rdna3_audio_info {
    uint32_t flags;          /* audio_info_flags.all: speaker flags, ALLSPEAKERS = bits 6:0, SUPPORT_AI = bit 7 */
    uint32_t video_latency, audio_latency;
    uint8_t  display_name[RD_AUDIO_DISPLAY_NAME_SIZE];
    uint32_t manufacture_id, product_id;
    uint32_t port_id[2];
    uint32_t mode_count;
    rdna3_audio_mode modes[RD_EDID_MAX_AUDIO];
} rdna3_audio_info;

/* struct audio_crtc_info */
typedef struct rdna3_audio_crtc_info {
    uint32_t h_total, h_active, v_active, pixel_repetition;
    uint32_t requested_pixel_clock_100hz, calculated_pixel_clock_100hz;
    uint32_t refresh_rate;
    uint32_t color_depth;     /* RD_COLOR_DEPTH_* */
    uint32_t pixel_encoding;  /* RD_PIXEL_ENCODING_* */
    int      interlaced;
    uint32_t dsc_bits_per_pixel, dsc_num_slices;
} rdna3_audio_crtc_info;

/* struct audio_dp_link_info */
typedef struct rdna3_audio_dp_link_info {
    uint32_t link_bandwidth_kbps, hblank_min_symbol_width;
    uint32_t encoding;        /* RD_DP_*_ENCODING */
    uint32_t link_rate;       /* RD_DP_LINK_RATE_* */
    uint32_t lane_count;
    int      is_mst;
} rdna3_audio_dp_link_info;

/* enum audio_dto_source: DTO_SOURCE_ID0 + OTG */
enum { RD_DTO_SOURCE_UNKNOWN = 0, RD_DTO_SOURCE_ID0 };

/* struct audio_pll_info */
typedef struct rdna3_audio_pll_info {
    uint32_t audio_dto_source_clock_in_khz, feed_back_divider;
    uint32_t dto_source;      /* RD_DTO_SOURCE_ID0 + n */
    int      ss_enabled;
    uint32_t ss_percentage;
} rdna3_audio_pll_info;

/* struct audio_output */
typedef struct rdna3_audio_output {
    uint32_t engine_id, signal;
    rdna3_audio_crtc_info crtc_info;
    rdna3_audio_pll_info pll_info;
    rdna3_audio_dp_link_info dp_link_info;
} rdna3_audio_output;

/* struct audio_clock_info: the HDMI ACR N / CTS of one pixel clock */
typedef struct rdna3_audio_clock_info {
    uint32_t pixel_clock_in_10khz;
    uint32_t n_32khz, cts_32khz, n_44khz, cts_44khz, n_48khz, cts_48khz;
} rdna3_audio_clock_info;

/* ---- the pure parts (no hardware) ---- */

/* dc_stream_construct's audio_info from the sink's EDID caps (the
 * manufacturer / product ids, the monitor name, the short audio
 * descriptors, the speaker allocation; amdgpu sets no container id: the
 * fixed port id of dc_stream_construct) */
void dcn32_audio_info_from_edid(const rdna3_edid *e, rdna3_audio_info *ai);
/* get_audio_clock_info */
void dcn32_get_audio_clock_info(uint32_t color_depth, uint32_t crtc_pixel_clock_100hz,
                                uint32_t actual_pixel_clock_100hz, rdna3_audio_clock_info *out);
/* speakers_to_channels (afmt3 / enc1): the AFMT_AUDIO_CHANNEL_ENABLE bits */
uint32_t dcn32_audio_speakers_to_channels(uint32_t speaker_flags);
/* check_audio_bandwidth: the rates of 'sample_rates' (RD_AUDIO_RATE_*) the
 * timing carries for 'channel_count' channels on 'signal' */
uint32_t dcn32_audio_check_bandwidth(const rdna3_audio_crtc_info *crtc, const rdna3_audio_dp_link_info *dp,
                                     uint32_t channel_count, uint32_t signal, uint32_t sample_rates);

/* ---- the azalia endpoints (dce_audio.c), endpoint n = AZF0ENDPOINTn ---- */

/* read / write an endpoint register (AZALIA_F0_CODEC_ENDPOINT_INDEX / _DATA) */
uint32_t dcn32_aud_read(rdna3_dev *d, uint32_t inst, uint32_t index);
void dcn32_aud_write(rdna3_dev *d, uint32_t inst, uint32_t index, uint32_t v);
int  dcn32_aud_endpoint_valid(rdna3_dev *d, uint32_t inst);
void dcn32_aud_hw_init(rdna3_dev *d, uint32_t inst);
void dcn32_aud_az_enable(rdna3_dev *d, uint32_t inst);
void dcn32_aud_az_disable(rdna3_dev *d, uint32_t inst);
void dcn32_aud_az_disable_hbr_audio(rdna3_dev *d, uint32_t inst);
void dcn32_aud_az_configure(rdna3_dev *d, uint32_t inst, uint32_t signal, const rdna3_audio_crtc_info *crtc,
                            const rdna3_audio_info *info, const rdna3_audio_dp_link_info *dp);
void dcn32_aud_wall_dto_setup(rdna3_dev *d, uint32_t signal, const rdna3_audio_crtc_info *crtc,
                              const rdna3_audio_pll_info *pll);

/* the endpoint indices (ixAZALIA_F0_CODEC_PIN_CONTROL_*, dce_11_0_d.h) */
#define RD_AZ_CHANNEL_SPEAKER         0x25
#define RD_AZ_ACP_DATA                0x27
#define RD_AZ_AUDIO_DESCRIPTOR0       0x28
#define RD_AZ_RESPONSE_LIPSYNC        0x37
#define RD_AZ_RESPONSE_HBR            0x38
#define RD_AZ_SINK_INFO0              0x3a
#define RD_AZ_SINK_INFO1              0x3b
#define RD_AZ_SINK_INFO2              0x3c
#define RD_AZ_SINK_INFO3              0x3d
#define RD_AZ_SINK_INFO4              0x3e
#define RD_AZ_SINK_INFO8              0x42
#define RD_AZ_HOT_PLUG_CONTROL        0x54
#define RD_AZ_RESPONSE_CONFIGURATION_DEFAULT 0x56
/* their fields (dce_11_0_sh_mask.h) */
#define RD_AZ_HPC_CLOCK_GATING_DISABLE 0x00000001u
#define RD_AZ_HPC_AUDIO_ENABLED        0x80000000u
#define RD_AZ_CS_SPEAKER_ALLOCATION    0x0000007fu
#define RD_AZ_CS_HDMI_CONNECTION       0x00010000u
#define RD_AZ_CS_DP_CONNECTION         0x00020000u
#define RD_AZ_CS_EXTRA_CONNECTION_INFO 0x00fc0000u
#define RD_AZ_CS_LFE_PLAYBACK_LEVEL    0x03000000u
#define RD_AZ_ACP_SUPPORTS_AI          0x00000040u
#define RD_AZ_HBR_CAPABLE              0x00000001u

/* ---- the pool of endpoints (resource_construct, update_num_audio) ---- */
typedef struct rdna3_audio_pool {
    uint32_t count;                  /* endpoints until the first with PORT_CONNECTIVITY 1 */
    uint32_t straps;                 /* DC_PINSTRAPS_AUDIO */
    int      hdmi_audio_native, hdmi_audio_on_dongle, dp_audio;
    uint8_t  acquired[RD_AUDIO_MAX_ENDPOINTS];   /* res_ctx.is_audio_acquired */
} rdna3_audio_pool;
/* read_dce_straps + update_num_audio + the endpoint count, then
 * dcn32_init_hw's audio hw_init loop (only endpoint 0 programs anything) */
void dcn32_audio_pool_init(rdna3_dev *d, rdna3_audio_pool *p);
/* find_first_free_audio for stream encoder 'engine_id' (the DIG front end;
 * the HPO stream encoders' ids lie beyond the endpoints) and
 * update_audio_usage; -1 when none is left */
int  dcn32_audio_acquire(rdna3_audio_pool *p, uint32_t engine_id, const uint8_t *stream_enc_acquired);
void dcn32_audio_release(rdna3_audio_pool *p, int inst);

/* ---- one stream's audio (the pipe_ctx's stream_res.audio) ---- */
typedef struct rdna3_audio_stream {
    rdna3_dev *d;
    uint32_t signal;          /* RD_SIGNAL_HDMI_TYPE_A / DISPLAY_PORT / DISPLAY_PORT_MST */
    uint32_t fe;              /* the DIG stream encoder (its AFMT, DP_SEC) */
    int      hpo;             /* a 128b/132b stream: the HPO stream encoder hpo_se */
    uint32_t hpo_se;
    uint32_t otg;
    int      az_inst;         /* the endpoint (stream_res.audio->inst), -1 none */
    int      enabled;         /* stream_res.audio->enabled */
    int      pme_wa;          /* clk_mgr enable_pme_wa: DALSMC BacoAudioD3PME on enable / disable */
    uint32_t dprefclk_khz;    /* clk_mgr get_dp_ref_clk_frequency (the DP DTO1 source) */
    rdna3_audio_info info;    /* stream->audio_info */
    rdna3_audio_output out;   /* build_audio_output of the running timing */
    uint32_t last_pme;        /* the PMFW's answer to the last PME message (log) */
} rdna3_audio_stream;

/* dc_resource's audio rule: an audio capable signal and an audio_info with
 * modes and speaker flags */
int  dcn32_audio_wanted(uint32_t signal, const rdna3_audio_info *info);

/* build_audio_output (+ translate_to_dto_source) for mode 'm' in format
 * 'f' with the requested pixel clock (kHz) of the pixel clock params;
 * 'dsc' the stream's DSC config or NULL. A DP stream also gets
 * populate_audio_dp_link_info: the link setting 'ls', its bandwidth
 * (dc_link_bandwidth_kbps: FEC included when the link enables it) and
 * whether the sink supports FEC (dp_is_fec_supported) */
void dcn32_audio_build_output(rdna3_audio_stream *s, const rdna3_mode *m, const rdna3_crtc_format *f,
                              uint32_t requested_pix_khz, const rdna3_dsc_config *dsc,
                              const rdna3_dp_link_settings *ls, uint32_t link_bw_kbps, int fec_supported);
/* dce110_setup_audio_dto for this stream (HDMI: dccg31_set_audio_dtbclk_dto
 * off, DTO0 from the pixel clock; DP: DTO1 from DPREFCLK) */
void dcn32_audio_setup_dto(rdna3_audio_stream *s);
/* the audio part of dce110_apply_single_controller_ctx_to_hw: the link
 * hwss setup_audio_output (DIO or HPO), az_configure, and on a 128b/132b
 * stream az_disable_hbr_audio (DCN 3.2 sets config.disable_hbr_audio_dp2) */
void dcn32_audio_setup_output(rdna3_audio_stream *s);
/* dce110_enable_audio_stream / dce110_disable_audio_stream */
void dcn32_audio_enable_stream(rdna3_audio_stream *s);
void dcn32_audio_disable_stream(rdna3_audio_stream *s);
/* dcn20_reset_back_end_for_pipe's az_disable of the stream's endpoint */
void dcn32_audio_reset_back_end(rdna3_audio_stream *s);
/* the whole sequence DC runs for a stream that comes up (setup_dto,
 * setup_output, enable_stream), for a stream the GOP already lit (the
 * seamless boot case: DC still sets the audio up); on HDMI also the audio
 * infoframe lines of enc32_stream_encoder_hdmi_set_stream_attribute */
void dcn32_audio_attach(rdna3_audio_stream *s);

/* ---- the stream encoder halves (exported for the tests) ---- */
void dcn32_enc_hdmi_audio_setup(rdna3_dev *d, uint32_t fe, uint32_t az_inst, const rdna3_audio_info *info,
                                const rdna3_audio_crtc_info *crtc);
void dcn32_enc_hdmi_audio_disable(rdna3_dev *d, uint32_t fe);
void dcn32_enc_dp_audio_setup(rdna3_dev *d, uint32_t fe, uint32_t az_inst, const rdna3_audio_info *info);
void dcn32_enc_dp_audio_enable(rdna3_dev *d, uint32_t fe);
void dcn32_enc_dp_audio_disable(rdna3_dev *d, uint32_t fe);
void dcn32_enc_audio_mute_control(rdna3_dev *d, uint32_t fe, int mute);
void dcn32_hpo_se_audio_setup(rdna3_dev *d, uint32_t se, uint32_t az_inst, const rdna3_audio_info *info);
void dcn32_hpo_se_audio_enable(rdna3_dev *d, uint32_t se);
void dcn32_hpo_se_audio_disable(rdna3_dev *d, uint32_t se);

#ifdef __cplusplus
}
#endif
#endif
