/*
 * dcn32_odm.h - ODM combine 2:1 on DCN 3.2: one OTG fed by the OPPs of two
 * pipes, each scanning out one half of the active area, for a mode wider
 * than one pipe takes (above 4096 pixels: the 8K modes) or with a pixel
 * rate one pipe's DISPCLK does not reach. Ported from Linux amdgpu Display
 * Core (v6.12, MIT):
 *
 *   dml/dcn32/display_mode_vba_util_32.c  dml32_CalculateODMMode (the
 *                                    "combine as needed" policy for a DP or
 *                                    HDMI stream), dml32_CalculateRequiredDispclk
 *   dml/dcn32/display_rq_dlg_calc_32.c  dml32_rq_dlg_get_dlg_reg_v2's ODM part
 *                                    (refcyc_h_blank_end per ODM pipe)
 *   hwss/dcn32/dcn32_hwseq.c        get_odm_config, dcn32_update_odm (OTG,
 *                                    OPP part; DSC: hw/dcn32_dp.c's
 *                                    link_set_dsc_on_stream does
 *                                    dcn32_update_dsc_on_stream's segments)
 *   core/dc_resource.c              resource_get_odm_slice_dst_width,
 *                                    resource_get_odm_slice_dst_rect
 *   opp/dcn10, opp/dcn20            opp1_pipe_clock_control,
 *                                    opp2_program_left_edge_extra_pixel,
 *                                    opp2_get_left_edge_extra_pixel_count
 *   resource/dcn32                  dcn32_acquire_idle_pipe_for_head_pipe_in_odm
 *                                    (a free pipe: its HUBP, DPP, MPCC, OPP
 *                                    and DSC share its index)
 *
 * The ODM pipe is brought up from the OTG master pipe's registers the way
 * hw/dcn32_clone.c lights a clone pipe (pipe power, DET split, DPP clock
 * DTO, HUBP / DPP / MPCC / OPP copies, mpc1_insert_plane into its own OPP),
 * then given its half: viewport x = slice width, recout and MPC size the
 * slice, OPPBUF the slice width, refcyc_h_blank_end of the second half.
 *
 * 2:1 only: dml32_CalculateODMMode never picks 4:1 for a DP or HDMI
 * stream. HDMI is never combined (Output != dm_hdmi).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCN32_ODM_H
#define RDNA3_DCN32_ODM_H

#include "dcn32.h"

#ifdef __cplusplus
extern "C" {
#endif

/* dcn3_2_ip.maximum_pixels_per_line_per_dsc_unit */
#define RD_DCN32_MAX_PIXELS_PER_LINE_PER_DSC_UNIT 6016
/* the widest active area one pipe takes (dcn3_2_ip.max_line_buffer_lines
 * aside: the 4096 limit of the single-pipe mode set) */
#define RD_DCN32_MAX_SINGLE_PIPE_WIDTH 4096

/* dml32_CalculateODMMode with the "combine as needed" policy for one stream
 * on 'hdmi' (1) or DP (0): 2 (ODM 2:1) when the stream needs more DISPCLK
 * than the state's 'state_dispclk_khz' without ODM and fits with it, when
 * DSC runs on a line wider than one DSC unit takes, or with 5 to 8 DSC
 * slices; else 1. 'vco_khz' is the DENTIST VCO (DFS granularity). */
uint32_t dcn32_odm_mode(int hdmi, uint32_t h_active, uint32_t pclk_khz, uint32_t state_dispclk_khz,
                        uint32_t vco_khz, int dsc, uint32_t dsc_slices);

/* resource_get_odm_slice_dst_width (RGB: no two-pixel alignment unless
 * 'two_pix') */
uint32_t dcn32_odm_slice_dst_width(uint32_t h_active, uint32_t opp_cnt, int is_last_segment, int two_pix);

/* get_odm_config read back from OTG 'otg' (OPTC_DATA_SOURCE_SELECT): the
 * OPP count (1, 2, 4) and their instances in 'opp_inst' (may be NULL) */
uint32_t dcn32_odm_get_config(rdna3_dev *d, uint32_t otg, uint32_t *opp_inst);

/* opp1_pipe_clock_control */
void dcn32_opp_pipe_clock_control(rdna3_dev *d, uint32_t opp, int enable);
/* opp2_program_left_edge_extra_pixel: 1 extra pixel for a 4:2:2 / 4:2:0
 * segment that is not the primary, else 0 (RD_PIXEL_ENCODING_*) */
void dcn32_opp_program_left_edge_extra_pixel(rdna3_dev *d, uint32_t opp, uint32_t pixel_encoding, int is_primary);

/* dcn32_update_odm's OTG and OPP part for OTG 'otg' with the OPPs
 * 'opp_inst' (opp_cnt 1: optc32_set_odm_bypass): optc32_set_odm_combine at
 * the slice width, then each ODM pipe's OPP clock on and its left edge */
void dcn32_update_odm(rdna3_dev *d, uint32_t otg, const uint32_t *opp_inst, uint32_t opp_cnt, uint32_t h_active,
                      uint32_t pixel_encoding);

/* The second pipe of an ODM 2:1 stream */
typedef struct rdna3_odm_pipe {
    uint32_t main_pipe, main_opp, otg;     /* the OTG master pipe */
    uint32_t pipe, opp, mpcc, dsc;         /* the ODM pipe (one index) */
    uint32_t det_main, det_odm, det_main_new, det_odm_new;
} rdna3_odm_pipe;

/* dcn32_acquire_idle_pipe_for_head_pipe_in_odm: a pipe no head uses whose
 * MPCC is in no tree, whose OPP no OTG reads and no MPC output mux feeds
 * (or the one OTG 'otg' combines already), and the DET split
 * (dcn32_determine_det_override: the stream's segments shared by its two
 * pipes). 0, RDNA3_EBUSY (no pipe or no DET to split). */
int  dcn32_odm_acquire(rdna3_dev *d, const rdna3_dcn *dc, uint32_t main_pipe, uint32_t main_opp, uint32_t otg,
                       rdna3_odm_pipe *p);

/* The ODM pipe for a 'h_active' x 'v_active' timing whose main pipe was
 * just programmed (its HUBP, DPP, DLG and MPC): power, DET, DPP DTO, the
 * HUBP / DPP / MPCC / OPP copies, the second half's viewport, recout, MPC
 * size and OPPBUF width (the main pipe's halved too), refcyc_h_blank_end
 * at 'h_blank_end' + the slice, ref/pix 'ref_to_pix' (2^19 units), the
 * MPC tree into its OPP; HUBP blanked (unblanked with the main pipe). */
int  dcn32_odm_pipe_enable(rdna3_dev *d, const rdna3_odm_pipe *p, uint32_t h_active, uint32_t v_active,
                           uint32_t h_blank_end, uint64_t ref_to_pix);
/* the ODM pipe's HUBP blank as the main pipe's */
void dcn32_odm_pipe_blank(rdna3_dev *d, const rdna3_odm_pipe *p, int blank);
/* the ODM pipe back off: MPC tree, OPP clock, DET back to the main pipe,
 * HUBP / DPP clocks, DPP DTO, power gate */
void dcn32_odm_pipe_disable(rdna3_dev *d, const rdna3_odm_pipe *p);

#ifdef __cplusplus
}
#endif
#endif
