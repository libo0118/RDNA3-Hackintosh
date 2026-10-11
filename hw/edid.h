/*
 * edid.h - the parts of EDID 1.3/1.4 (+ CTA-861 extension) a mode list
 * needs: manufacturer, product, name, detailed timing descriptors, standard
 * timings, established timings. Written from the VESA E-EDID spec; no
 * kernel code involved.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_EDID_H
#define RDNA3_EDID_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RD_EDID_MAX_MODES 40

typedef struct rdna3_mode {
    uint32_t h_active, v_active;
    uint32_t h_total, v_total;
    uint32_t h_front, h_sync, v_front, v_sync;   /* front porch and sync widths */
    uint32_t pixel_khz;                           /* 0 for standard/established timings */
    uint32_t refresh_mhz;                         /* milli-Hz */
    uint8_t  h_sync_pos, v_sync_pos, interlaced, preferred;
    uint8_t  origin;                              /* RD_MODE_FROM_* */
} rdna3_mode;

/* where a mode's timing came from */
#define RD_MODE_FROM_DTD 0      /* detailed timing descriptor */
#define RD_MODE_FROM_DMT 1      /* VESA DMT table (standard / established entry) */
#define RD_MODE_FROM_CVT 2      /* CVT reduced blanking (standard entry not in DMT) */
#define RD_MODE_FROM_CTA 3      /* CTA-861 VIC */

typedef struct rdna3_edid {
    char     manufacturer[4];       /* PNP id, e.g. "DEL" */
    uint16_t product;
    uint32_t serial;
    uint8_t  week, year;            /* year - 1990 */
    uint8_t  version, revision;
    uint8_t  digital;
    uint32_t width_cm, height_cm;
    char     name[14];              /* monitor name descriptor */
    uint32_t nmodes;
    rdna3_mode mode[RD_EDID_MAX_MODES];
    uint32_t extensions;            /* blocks after the base block */
    /* amdgpu_dm's dm_helpers_parse_edid_caps (struct dc_edid_caps) with
     * DRM's CTA parsing (drm_parse_hdmi_vsdb_video, drm_parse_hdmi_forum_scds,
     * drm_edid_to_sad, drm_edid_to_speaker_allocation): */
    uint8_t  hdmi;                  /* an HDMI VSDB (IEEE OUI 00-0C-03): edid_hdmi */
    uint8_t  scdc_present;          /* HF-VSDB: SCDC_Present */
    uint8_t  lte_340_scramble;      /* HF-VSDB: LTE_340Mcsc_Scramble */
    uint32_t max_tmds_clk_mhz;      /* HDMI VSDB Max_TMDS_Clock, HF-VSDB Max_TMDS_Character_Rate; 0 unknown */
    uint32_t audio_mode_count;      /* short audio descriptors (<= RD_EDID_MAX_AUDIO) */
    struct {
        uint8_t format_code, channel_count, sample_rate, sample_size;
    } audio_modes[16];
    uint8_t  speaker_flags;         /* speaker allocation, DEFAULT_SPEAKER_LOCATION (5) without one */
    uint32_t panel_id;              /* edid_extract_panel_id */
    /* DRM's update_display_info colour part (drm_parse_cea_ext,
     * drm_parse_hdmi_deep_color_info, the EDID 1.4 input / feature bits),
     * what amdgpu_dm's output format choice reads (rdna3fmt):
     * display_info.color_formats (RD_COLOR_FORMAT_*), display_info.bpc
     * (0 = not given), the HDMI VSDB's RGB 4:4:4 deep color modes
     * (edid_hdmi_rgb444_dc_modes: RD_EDID_HDMI_DC_*) and whether they
     * hold for YCbCr 4:4:4 too (DC_Y444) */
    uint8_t  color_formats, bpc, hdmi_dc_modes, hdmi_dc_y444;
    uint8_t  cta_ext3, hdmi_dc_bpc;  /* parse state: the CTA byte 3, the VSDB's bpc */
    /* apply_edid_quirks (panel_patch) */
    uint8_t  quirk_disable_fams, quirk_remove_sink_ext_caps, quirk_disable_colorimetry;
} rdna3_edid;

#define RD_EDID_MAX_AUDIO 16        /* DC_MAX_AUDIO_DESC_COUNT */
/* DRM_COLOR_FORMAT_* */
#define RD_COLOR_FORMAT_RGB444   (1u << 0)
#define RD_COLOR_FORMAT_YCBCR444 (1u << 1)
#define RD_COLOR_FORMAT_YCBCR422 (1u << 2)
#define RD_COLOR_FORMAT_YCBCR420 (1u << 3)
/* DRM_EDID_HDMI_DC_*: the HDMI VSDB's byte 6 */
#define RD_EDID_HDMI_DC_48 (1u << 6)
#define RD_EDID_HDMI_DC_36 (1u << 5)
#define RD_EDID_HDMI_DC_30 (1u << 4)
#define RD_EDID_HDMI_DC_Y444 (1u << 3)
#define RD_EDID_DEFAULT_SPEAKER_LOCATION 5

/* drm_edid_encode_panel_id ('A'..'Z' x 3, product code) */
#define RD_EDID_PANEL_ID(a, b, c, product) \
    ((((uint32_t)(a) - '@') & 0x1f) << 26 | (((uint32_t)(b) - '@') & 0x1f) << 21 | \
     (((uint32_t)(c) - '@') & 0x1f) << 16 | ((uint32_t)(product) & 0xffff))

/* drm_match_cea_mode for the progressive CTA-861 timings this parser
 * knows: the VIC of 'm' (same active size, totals, syncs, pixel clock
 * within 0.5 %, either the 60 Hz or the 59.94 Hz variant), 0 = none */
uint32_t edid_match_vic(const rdna3_mode *m);

/* Validate the header and checksums, fill 'out'. Returns 0, or -1 for a bad
 * header / checksum / size. 'len' is a multiple of 128 (base + extensions). */
int edid_parse(const uint8_t *data, uint32_t len, rdna3_edid *out);

/* drm_cvt_mode with reduced blanking (CVT 1.1 RB v1), non-interlaced,
 * no margins: the timing of a standard entry DMT does not list */
int edid_cvt_rb(uint32_t h, uint32_t v, uint32_t hz, rdna3_mode *m);

/* drm_mode_find_dmt: the VESA DMT timing of h x v at hz, 0 when found */
int edid_dmt(uint32_t h, uint32_t v, uint32_t hz, rdna3_mode *m);

/* checksum of one 128-byte block (the bytes sum to 0 mod 256 when valid) */
int edid_block_valid(const uint8_t *block);

#ifdef __cplusplus
}
#endif
#endif
