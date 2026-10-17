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
    /* ODM combine 2:1 (hw/dcn32_odm.c): odm_cnt 2 when HUBP 'odm_pipe' on
     * the same OTG scans the right part from viewport x 'odm_x'; the flips,
     * the blank and the cursor go to both pipes. 0 / 1: one pipe. */
    uint32_t odm_cnt, odm_pipe, odm_x;
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
    uint32_t underflow;         /* HUBP_UNDERFLOW_STATUS: the pipe ran out of data (sticky) */
    uint32_t otg, format, sw_mode, dcc;
    uint32_t width, height, pitch_px;
    uint32_t flip_pending;      /* SURFACE_FLIP_PENDING */
    uint64_t address;           /* PRIMARY_SURFACE_ADDRESS (requested) */
    uint64_t inuse;             /* SURFACE_EARLIEST_INUSE (being scanned) */
    /* what decides whether the pipe keeps up (underflow at scaled modes,
     * card run 20): its DET and the shared compressed buffer (64 KiB
     * segments, current), the clocks (global DISPCLK, this pipe's DPPCLK
     * through its DTO), the scaler (DSCL_MODE, LB_MEMORY_CTRL,
     * DSCL_MEM_PWR_CTRL) and the DLG it was given (DST_AFTER_SCALER,
     * REFCYC_PER_REQ_DELIVERY, REFCYC_PER_LINE_DELIVERY_L) */
    uint32_t det_segs, compbuf_segs;
    uint32_t dispclk_khz, dppclk_khz;
    uint32_t dscl_mode, lb_memory_ctrl, dscl_mem_pwr;
    uint32_t dlg_after_scaler, dlg_req_delivery, dlg_line_delivery;
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

/* Scaled modes on a lit head: the monitor keeps its timing, the DPP
 * scaler stretches a smaller surface over the active area. The registers
 * the GOP left are saved on the first scaled source and put back when the
 * native size comes back. */
#define RD_SCALE_REGS 22
#define RD_SCALE_BW_REGS 8
typedef struct rdna3_dcn_scale {
    int      saved[RD_DCN_MAX_PIPES];
    uint32_t native_w[RD_DCN_MAX_PIPES], native_h[RD_DCN_MAX_PIPES];
    uint32_t reg[RD_DCN_MAX_PIPES][RD_SCALE_REGS];
    /* rdna3scaledlg=1: a scaled source also gets the line buffer and the
     * DLG/TTU a scaled plane needs (dcn32_set_source); the GOP's values
     * are saved alongside and come back with the native size */
    uint32_t dlg;
    int      bw_saved[RD_DCN_MAX_PIPES];
    uint32_t bw_reg[RD_DCN_MAX_PIPES][RD_SCALE_BW_REGS];
} rdna3_dcn_scale;

/* Show the w x h surface at MC address 'mc' (pitch_px pixels per row) on
 * head i, upscaled to the head's native active size: HUBP viewport/pitch/
 * address + dpp1_dscl_set_scaler_manual_scale (444 RGB, 2-tap hardcoded
 * coefficients, ratio src/dst, init (ratio + taps + 1) / 2), all under the
 * OTG master update lock (optc3_lock / optc1_unlock) so they latch in one
 * frame. w x h == native restores the saved bypass state. Upscaling only,
 * down to 1/4 of the native size.
 * With sc->dlg the scaled plane gets what amdgpu programs for it besides
 * the ratios (the GOP's DLG/TTU were computed for a 1:1 plane):
 * dpp1_dscl_set_lb with dpp1_dscl_find_lb_memory_config /
 * dscl32_calc_lb_num_partitions (LB_MEMORY_CTRL, LB_DATA_FORMAT), and of
 * dml32_rq_dlg_get_dlg_reg the fields that depend on the scale:
 * refcyc_per_req_delivery(_pre)_l x 1 / HRatio (fewer 256-byte requests per
 * line, dml32_CalculatePixelDeliveryTimes), refcyc_per_line_delivery
 * (_pre)_l x the 64-pixel rounding of the narrower swath, and
 * refcyc_x_after_scaler + (DPPCLKDelaySCL - DPPCLKDelaySCLLBOnly) DPPCLK
 * cycles when the GOP bypassed the scaler (dml32_CalculatePrefetchSchedule
 * DSTXAfterScaler). VInitPreFill, the prefetch source lines and so
 * VRATIO_PREFETCH / DST_Y_PREFETCH do not change for an upscale with 2
 * taps (floor((vratio + 3) / 2) = 1 as for 1:1). */
int  dcn32_set_source(rdna3_dev *d, rdna3_dcn *dc, rdna3_dcn_scale *sc, uint32_t i, uint64_t mc,
                      uint32_t w, uint32_t h, uint32_t pitch_px);

/* hubp2_clear_underflow: HUBP_UNDERFLOW_CLEAR on pipe 'pipe' (the status
 * is sticky: the report clears it to see whether it comes back) */
int  dcn32_clear_underflow(rdna3_dev *d, uint32_t pipe);

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
/* hubp2_read_state_common's surface addresses of head 'i''s pipe: what
 * the HUBP scans (DCSURF_SURFACE_EARLIEST_INUSE) and what was last
 * requested (DCSURF_PRIMARY_SURFACE_ADDRESS), MC addresses; -RDNA3_EINVAL
 * for no such head (card run 2026-10-17b: the accelerator's trace records
 * what each panel really scanned at every swap) */
int  dcn32_scan_addr(rdna3_dev *d, rdna3_dcn *dc, uint32_t i, uint64_t *inuse, uint64_t *requested);
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
/* optc1_configure_crc on OTG 'otg' as dc_stream_configure_crc sets it up:
 * windows A and B = the whole active area (read from the OTG), union of
 * both, continuous. Without the windows the CRC stays 0 (card runs 9b-17). */
void dcn32_otg_crc_config(rdna3_dev *d, uint32_t otg, int enable);
/* R/Cr, G/Y, B/Cb of the last frame; returns RDNA3_ENODEV when CRC is off */
int  dcn32_crc_read(rdna3_dev *d, const rdna3_dcn *dc, uint32_t i, uint32_t *r, uint32_t *g,
                    uint32_t *b);

#ifdef __cplusplus
}
#endif
#endif
