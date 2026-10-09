/*
 * dcn32_mode.h - the computational half of a DCN 3.2 modeset, no register
 * writes: OTG timing values for an EDID mode (optc1_program_timing),
 * the DP 8b/10b link bandwidth check (dc_link_bandwidth_kbps vs.
 * dc_bandwidth_in_kbps_from_timing), a mode filter for one connector and
 * matching the mode the GOP left running against the EDID list.
 *
 * Applying a mode also needs DML (watermarks, DLG/TTU, vstartup), the
 * pixel clock (DCCG DTO), the stream encoder's MSA and the HUBP/DPP sizes;
 * that waits for the card. What is here is what can be checked without it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCN32_MODE_H
#define RDNA3_DCN32_MODE_H

#include "dcn32.h"
#include "edid.h"

#ifdef __cplusplus
extern "C" {
#endif

/* register field values optc1_program_timing writes */
typedef struct rdna3_otg_timing {
    uint32_t h_total;                     /* OTG_H_TOTAL = h_total - 1 */
    uint32_t h_sync_start, h_sync_end;    /* OTG_H_SYNC_A: 0, sync width */
    uint32_t h_blank_start, h_blank_end;  /* OTG_H_BLANK_START_END */
    uint32_t h_sync_pol;                  /* OTG_H_SYNC_A_POL: 1 = negative */
    uint32_t v_total;                     /* OTG_V_TOTAL (and V_TOTAL_MIN/MAX) = v_total - 1 */
    uint32_t v_sync_start, v_sync_end;
    uint32_t v_blank_start, v_blank_end;
    uint32_t v_sync_pol;
    uint32_t interlace;                   /* OTG_INTERLACE_ENABLE */
    uint32_t start_point;                 /* OTG_START_POINT_CNTL: 1 for DP */
    uint32_t field_num;                   /* OTG_FIELD_NUMBER_CNTL */
} rdna3_otg_timing;

/* optc1_program_timing (with apply_front_porch_workaround) for 'm' on a DP
 * (dp != 0) or TMDS sink. RDNA3_EINVAL when the mode has no detailed
 * timing (standard / established entries carry only size and rate). */
int dcn32_otg_timing(const rdna3_mode *m, int dp, rdna3_otg_timing *t);

/* DP 8b/10b payload bandwidth in kbps for a DPCD link rate code (0x06 RBR,
 * 0x0a HBR, 0x14 HBR2, 0x1e HBR3) and lane count; 'downspread' takes the
 * 0.5 % SSC margin off. */
uint64_t dcn32_dp_link_kbps(uint32_t link_rate_code, uint32_t lanes, int downspread);

/* what a mode needs at 'bpp' bits per pixel */
uint64_t dcn32_mode_kbps(const rdna3_mode *m, uint32_t bpp);

/* why a mode is refused (0 = usable) */
enum {
    RD_MODE_OK = 0,
    RD_MODE_NO_TIMING = 1,      /* no detailed timing */
    RD_MODE_INTERLACED = 2,     /* not offered on DP here */
    RD_MODE_LINK_BW = 3,        /* more than the link carries at 24 bpp */
    RD_MODE_PIXEL_CLOCK = 4,    /* beyond one pipe without ODM combine */
    RD_MODE_SIZE = 5,
};
/* Conservative caps chosen by this driver for one OTG/DPP pipe without ODM
 * combine, until DML decides per mode; not numbers taken from DC. */
#define RD_DCN32_MAX_PCLK_KHZ  1200000u
#define RD_DCN32_MAX_H_ACTIVE  5120u

/* dc_validate_stream + dp_validate_mode_timing for one DP sink */
int dcn32_mode_check(const rdna3_mode *m, uint32_t link_rate_code, uint32_t lanes, int downspread);

/* The EDID mode the head is running (same active size and totals, sync
 * widths and front porches; pixel clock within 0.5 %), or -1. 'pclk_khz'
 * is the head's measured or programmed pixel clock, 0 to skip that test. */
int dcn32_match_head_mode(const rdna3_dcn_head *h, uint32_t pclk_khz, const rdna3_edid *e);

#ifdef __cplusplus
}
#endif
#endif
