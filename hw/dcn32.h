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
 * Register offsets are DCN 3.2.1 (Navi 32/33); HUBP n = HUBP0 + n * 0xdc,
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
} rdna3_dcn_head;

typedef struct rdna3_dcn {
    int      present;           /* DCN found in discovery */
    uint32_t nheads;
    rdna3_dcn_head head[RD_DCN_MAX_PIPES];
    uint64_t requested[RD_DCN_MAX_PIPES];   /* last address written per pipe */
} rdna3_dcn;

/* Scan the four pipes for lit ones: HUBP not blanked, OTG master enabled.
 * Returns the number of heads found (0 when the display is off or DCN is
 * not in discovery). Reads only. */
int  dcn32_read_state(rdna3_dev *d, rdna3_dcn *dc);

/* A head CPU drawing can take over: 32 bpp linear, no DCC, surface in VRAM. */
int  dcn32_head_usable(rdna3_dev *d, const rdna3_dcn_head *h);

/* Flip head 'i' to the surface at MC address 'mc' with 'pitch_px' pixels
 * per row (same width/height as the mode). 'immediate' = no vsync wait
 * (SURFACE_FLIP_TYPE). Pitch changes go under SURFACE_UPDATE_LOCK. */
int  dcn32_flip(rdna3_dev *d, rdna3_dcn *dc, uint32_t i, uint64_t mc, uint32_t pitch_px,
                int immediate);
/* hubp2_is_flip_pending: 1 while the last flip of head 'i' has not latched */
int  dcn32_flip_pending(rdna3_dev *d, rdna3_dcn *dc, uint32_t i);

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
