/*
 * dcn32_gamma.h - output gamma of a head: the 1D table macOS hands
 * IOFramebuffer::setGammaTable, as the MPCC output gamma (OGAM) PWL that
 * amdgpu programs on DCN 3.2 for a stream's out_transfer_func:
 *
 *   dcn32_set_output_transfer_func -> mpc3_set_output_gamma
 *   cm3_helper_translate_curve_to_hw_format    (SDR distribution)
 *   cm3_helper_convert_to_custom_float          (convert_to_custom_float_format:
 *                                                build_custom_float, setup_custom_float)
 *   mpc3_get_ogam_current, mpc3_power_on_ogam_lut, mpc3_configure_ogam_lut,
 *   mpc3_program_luta / mpc3_program_lutb -> cm_helper_program_gamcor_xfer_func,
 *   mpc3_program_ogam_pwl (is_rgb_equal)
 *
 * The curve: the SDR distribution of cm3_helper_translate_curve_to_hw_format,
 * 11 regions from 2^-10 to 2^1 with 8, 9 x 16 and 2 segments = 154 points
 * plus the end point (hw_points_num 155). Point j of region e sits at
 * x = 2^e * (1 + j / 2^seg) (the colour module's coordinates_x), the end
 * point at x = 2.
 *
 * What x is: the GOP leaves the DPP without degamma or gamut remap (CM in
 * bypass), so the OGAM input is the encoded pixel value in [0, 1] and the
 * macOS table applies to it directly: y(x) = the table interpolated
 * linearly at x (as apply_lut_1d does for a 1D ramp), x >= 1 -> the last
 * entry. Below 2^-10 the hardware follows the start slope y(2^-10) / 2^-10
 * from 0 (amdgpu leaves START_BASE / OFFSET alone), so a table whose
 * first entry is not 0 still maps pixel value 0 to 0.
 *
 * All arithmetic is dc_fixpt (signed 31.32 in an int64), no floating point.
 * Writes happen under the OTG master update lock (optc3_lock, as
 * dcn20_pipe_control_lock brackets a stream update), so MODE/SELECT land in
 * one frame; the LUT RAM not in use is the one written (A/B ping-pong).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCN32_GAMMA_H
#define RDNA3_DCN32_GAMMA_H

#include "dcn32.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RD_GAMMA_HW_POINTS   155u    /* hw_points_num of the SDR distribution */
#define RD_GAMMA_REGIONS     17u     /* MPCC_OGAM_RAMx_REGION_0_1 .. _32_33 */
#define RD_GAMMA_MAX_ENTRIES 1024u   /* per channel of a table */

/* mpc3_get_ogam_current (enum dc_lut_mode) */
#define RD_GAMMA_LUT_BYPASS  0u
#define RD_GAMMA_LUT_RAM_A   1u
#define RD_GAMMA_LUT_RAM_B   2u

/* a table in hardware form; channel order R, G, B */
typedef struct rdna3_gamma_pwl {
    /* corner points, custom float as the registers take them */
    uint32_t start_x[3];       /* EXP_REGION_START = 2^-10 (6e12m) */
    uint32_t start_slope[3];   /* EXP_REGION_START_SLOPE = y0 / x0 (6e12m) */
    uint32_t end_base[3];      /* EXP_REGION_END_BASE = y at 2^1 (6e12m) */
    uint32_t end_x[3];         /* EXP_REGION_END = 2^1 (6e10m) */
    uint32_t end_slope[3];     /* EXP_REGION_END_SLOPE = 0 (6e10m) */
    uint32_t region[RD_GAMMA_REGIONS];      /* REGION_0_1 .. REGION_32_33 values */
    uint32_t lut[3][RD_GAMMA_HW_POINTS];    /* the PWL points (6e12m) */
    uint32_t num;              /* points written: hw_points_num */
    int      rgb_equal;        /* is_rgb_equal: one pass with WRITE_COLOR_MASK 7 */
    int      linear;           /* __is_lut_linear (+-1): the identity, bypass does the same */
} rdna3_gamma_pwl;

/* 'channels' (1 = the same table for R, G and B, or 3 = R, G, B one after
 * the other) tables of 'count' (2..RD_GAMMA_MAX_ENTRIES) entries,
 * 0..65535 each -> *p. RDNA3_EINVAL for a bad shape. */
int  dcn32_gamma_build(const uint16_t *data, uint32_t channels, uint32_t count, rdna3_gamma_pwl *p);

/* The MPCCs that feed OTG 'otg' (as hw/dcn32_clone.c walks it: ODM
 * OPTC_DATA_SOURCE_SELECT -> OPP -> MPC_OUT<opp>_MUX -> MPCC; two with ODM
 * combine). Returns how many (1 or 2), or a negative RDNA3_E* code. */
int  dcn32_gamma_otg_mpccs(rdna3_dev *d, uint32_t otg, uint32_t mpcc[2]);

/* mpc3_set_output_gamma on MPCC 'mpcc' under the lock of OTG 'otg' (no
 * lock when otg >= RD_DCN_MAX_PIPES). p == NULL: bypass (MPCC_OGAM_MODE 0).
 * RDNA3_ETIME when the OGAM memory does not power up (MODE/SELECT are put
 * back, nothing changes on screen). */
int  dcn32_gamma_apply(rdna3_dev *d, uint32_t otg, uint32_t mpcc, const rdna3_gamma_pwl *p);

/* Head 'head' of 'dc': every MPCC of its OTG. Returns 0 or the first error. */
int  dcn32_gamma_set_head(rdna3_dev *d, const rdna3_dcn *dc, uint32_t head, const rdna3_gamma_pwl *p);

typedef struct rdna3_gamma_state {
    uint32_t mode;             /* MPCC_OGAM_MODE: 0 bypass, 2 RAM LUT */
    uint32_t select;           /* MPCC_OGAM_SELECT: 0 RAM A, 1 RAM B */
    uint32_t current;          /* mpc3_get_ogam_current: RD_GAMMA_LUT_* (what is latched) */
    uint32_t mem_pwr_state;    /* MPCC_OGAM_MEM_PWR_STATE: 0 = powered */
    /* the DPP in front (DPP n feeds MPCC n): CM_GAMCOR_MODE_CURRENT, 0 =
     * no degamma, which the curve mapping above assumes */
    uint32_t dpp_degamma;
} rdna3_gamma_state;
int  dcn32_gamma_state(rdna3_dev *d, uint32_t mpcc, rdna3_gamma_state *s);

#ifdef __cplusplus
}
#endif
#endif
