/*
 * dcn32.h - DCN 3.2.x scanout takeover: read back what the VBIOS / UEFI GOP
 * left on screen (HUBP surface, OTG timing), flip a head to another VRAM
 * surface, read the frame counter / scan position and the OTG CRC.
 *
 * No mode setting and no DMCUB: the display stays in the mode the firmware
 * trained (link, clocks, OPP/MPC routing untouched), only the HUBP surface
 * address (and pitch) changes - the same registers amdgpu DC programs in
 * hubp2_program_surface_flip_and_addr / hubp2_program_size, read the way
 * hubp2_read_state / optc1_get_crtc_timing do. Full DC comes later.
 *
 * Register offsets: the generated dcn_3_2_1 headers, which match DCN 3.2.0
 * (Navi 32, checked define by define); HUBP n = HUBP0 + n * 0xdc,
 * OTG n = OTG0 + n * 0x80 (segment 2 of the DCN discovery base).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCN32_H
#define RDNA3_DCN32_H

#include "rdna3_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RD_DCN_MAX_PIPES 4

/* SURFACE_PIXEL_FORMAT (hubp2_program_pixel_format) */
#define RD_DCN_FMT_ARGB8888    8    /* 0xAARRGGBB little endian: B at byte 0 */
#define RD_DCN_FMT_ARGB2101010 10
#define RD_DCN_FMT_ABGR2101010 11
#define RD_DCN_FMT_ABGR8888    12

typedef struct rdna3_dcn_head {
    uint32_t pipe;              /* HUBP instance */
    uint32_t otg;               /* OTG instance (HUBP_VTG_SEL) */
    uint32_t format;            /* SURFACE_PIXEL_FORMAT */
    uint32_t sw_mode;           /* 0 = linear */
    uint32_t dcc;               /* PRIMARY_SURFACE_DCC_EN */
    uint32_t width, height;     /* primary viewport */
    uint32_t pitch_px;          /* DCSURF_SURFACE_PITCH + 1 */
    uint64_t address;           /* MC address of the primary surface */
    /* OTG timing (optc1_get_crtc_timing): totals, active, porches, syncs */
    uint32_t h_total, v_total, h_active, v_active;
    uint32_t h_front, h_sync, v_front, v_sync;
    uint32_t h_sync_neg, v_sync_neg;
    /* what drives the OTG (dcn10_get_dig_frontend, enc1_dig_source_otg,
     * dcn10_get_dig_mode): -1 when no enabled DIG back end is fed by it */
    int      dig_fe;            /* stream encoder / DIG front end, 0 = DIGA */
    int      dig_be;            /* DIG back end = transmitter, 0 = UNIPHY A */
    uint32_t dig_mode;          /* DIG_MODE: 0 DP, 1 LVDS, 2 DVI, 3 HDMI, 5 DP MST */
} rdna3_dcn_head;

typedef struct rdna3_dcn {
    int      present;           /* DCN found in discovery */
    uint32_t nheads;
    rdna3_dcn_head head[RD_DCN_MAX_PIPES];
    uint64_t requested[RD_DCN_MAX_PIPES];   /* last address written per pipe */
    uint32_t cursor_size[RD_DCN_MAX_PIPES]; /* 0 = no image programmed */
    int      cursor_on[RD_DCN_MAX_PIPES];
} rdna3_dcn;

/* What one pipe scans out right now, read back from HUBP/OTG (for the
 * report's screen snapshot: proof of what reaches the panel). */
typedef struct rdna3_scanout_info {
    uint32_t pipe;
    uint32_t powered;           /* DOMAIN<pipe> power-up (not power-gated) */
    uint32_t lit;               /* HUBP not blanked and its OTG master enabled */
    uint32_t blanked;           /* HUBP_BLANK_EN (dcn32_set_blank, display sleep) */
    uint32_t otg, format, sw_mode, dcc;
    uint32_t width, height, pitch_px;
    uint32_t flip_pending;      /* SURFACE_FLIP_PENDING */
    uint64_t address;           /* PRIMARY_SURFACE_ADDRESS (requested) */
    uint64_t inuse;             /* SURFACE_EARLIEST_INUSE (being scanned) */
} rdna3_scanout_info;

/* Read pipe 'pipe' (0..3). Reads only; a power-gated pipe returns with
 * powered 0 and no HUBP read. */
int  dcn32_scanout_info(rdna3_dev *d, uint32_t pipe, rdna3_scanout_info *s);

/* Rows [row0, row0 + nrows) of the surface scaled to out_w x out_h
 * (nearest pixel), as RGB888 into rgb (nrows * out_w * 3 bytes). Reads the
 * surface the pipe scans (inuse, else address) through the CPU BAR.
 * Only 32 bpp linear surfaces without DCC; -RDNA3_EINVAL otherwise or when
 * the surface is outside the CPU-visible VRAM. */
int  dcn32_scanout_sample(rdna3_dev *d, const rdna3_scanout_info *s, uint32_t out_w,
                          uint32_t out_h, uint32_t row0, uint32_t nrows, uint8_t *rgb);

/* Display sleep without turning the link off: hubp2_set_blank_regs
 * (wait for HUBP_NO_OUTSTANDING_REQ, then HUBP_BLANK_EN + HUBP_TTU_DISABLE);
 * the MPC then sends its black background. 0 unblanks. */
int  dcn32_set_blank(rdna3_dev *d, rdna3_dcn *dc, uint32_t i, int blank);

/* Scan the four pipes for lit ones: HUBP not blanked, OTG master enabled.
 * Returns the number of heads found (0 when the display is off or DCN is
 * not in discovery). Reads only. */
int  dcn32_read_state(rdna3_dev *d, rdna3_dcn *dc);

/* A head CPU drawing can take over: 32 bpp linear, no DCC, surface in VRAM. */
int  dcn32_head_usable(rdna3_dev *d, const rdna3_dcn_head *h);

/* The DIG back end (transmitter) whose front end sources OTG 'otg': amdgpu's
 * seamless-boot read-back. Returns the back end, or -1; *fe and *mode may be
 * NULL. */
int  dcn32_otg_dig(rdna3_dev *d, uint32_t otg, int *fe, uint32_t *mode);

/* Flip head 'i' to the surface at MC address 'mc' with 'pitch_px' pixels
 * per row (same width/height as the mode). 'immediate' = no vsync wait
 * (SURFACE_FLIP_TYPE). Pitch changes go under SURFACE_UPDATE_LOCK. */
int  dcn32_flip(rdna3_dev *d, rdna3_dcn *dc, uint32_t i, uint64_t mc, uint32_t pitch_px,
                int immediate);
/* hubp2_is_flip_pending: 1 while the last flip of head 'i' has not latched */
int  dcn32_flip_pending(rdna3_dev *d, rdna3_dcn *dc, uint32_t i);

/* Hardware cursor (hubp2_cursor_set_attributes / hubp2_cursor_set_position
 * + dpp2_set_cursor_attributes / dpp1_set_cursor_position): a square
 * ARGB8888 premultiplied-alpha image of 'size' = 64, 128 or 256 pixels
 * (pitch = size) at MC address 'mc' (256 B aligned). DPP instance =
 * pipe instance. */
#define RD_DCN_CURSOR_MODE_PREMULT 2   /* CURSOR_MODE_COLOR_PRE_MULTIPLIED_ALPHA */
int  dcn32_cursor_set_image(rdna3_dev *d, rdna3_dcn *dc, uint32_t i, uint64_t mc, uint32_t size);
/* top-left corner on screen (may be negative: moves into the hot spot) */
int  dcn32_cursor_set_position(rdna3_dev *d, rdna3_dcn *dc, uint32_t i, int32_t x, int32_t y,
                               int visible);

/* OTG frame counter (24 bit) and current scan line */
uint32_t dcn32_frame_count(rdna3_dev *d, const rdna3_dcn *dc, uint32_t i);
uint32_t dcn32_scanline(rdna3_dev *d, const rdna3_dcn *dc, uint32_t i);
int      dcn32_in_vblank(rdna3_dev *d, const rdna3_dcn *dc, uint32_t i);

/* OTG CRC over the whole frame (optc1_configure_crc, continuous) */
void dcn32_crc_enable(rdna3_dev *d, const rdna3_dcn *dc, uint32_t i, int enable);
/* R/Cr, G/Y, B/Cb of the last frame; returns RDNA3_ENODEV when CRC is off */
int  dcn32_crc_read(rdna3_dev *d, const rdna3_dcn *dc, uint32_t i, uint32_t *r, uint32_t *g,
                    uint32_t *b);

#ifdef __cplusplus
}
#endif
#endif
