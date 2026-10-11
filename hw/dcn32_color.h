/*
 * dcn32_color.h - the MPC output color pieces of DCN 3.2 besides the
 * output gamma (hw/dcn32_gamma.c), ported from Linux amdgpu Display Core
 * (v6.12) as dcn32_mpc_funcs wires them:
 *
 *   dcn30_mpc.c        mpc3_set_gamut_remap (program_gamut_remap: the MPCC
 *                      gamut remap after blending, coefficient sets A / B
 *                      used in turn), mpc3_set_output_csc, mpc3_set_ocsc_default,
 *                      mpc3_set_denorm, mpc3_set_denorm_clamp,
 *                      mpc3_mpc_init_single_inst (+ dcn10_mpc.c
 *                      mpc1_mpc_init_single_inst)
 *   dcn10_cm_common.c  cm_helper_program_color_matrices
 *   basics/conversion.c convert_float_matrix, fixed_point_to_int_frac
 *   core/dc_hw_sequencer.c find_color_matrix (output_csc_matrix[])
 *
 * The output CSC turns the blended RGB into YCbCr or limited-range RGB for
 * the OPP; denorm sets the bit depth the OPP formatter receives.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCN32_COLOR_H
#define RDNA3_DCN32_COLOR_H

#include "rdna3_hw.h"
#include "fixpt31_32.h"
#include "dcn32_otg.h"   /* RD_COLOR_DEPTH_* */

#ifdef __cplusplus
extern "C" {
#endif

/* enum dc_color_space_type (the output CSC tables) */
enum {
    RD_COLOR_SPACE_RGB_TYPE = 0,
    RD_COLOR_SPACE_RGB_LIMITED_TYPE,
    RD_COLOR_SPACE_YCBCR601_TYPE,
    RD_COLOR_SPACE_YCBCR709_TYPE,
    RD_COLOR_SPACE_YCBCR601_LIMITED_TYPE,
    RD_COLOR_SPACE_YCBCR709_LIMITED_TYPE,
    RD_COLOR_SPACE_YCBCR2020_TYPE,
    RD_COLOR_SPACE_YCBCR709_BLACK_TYPE,
};

/* enum mpc_output_csc_mode */
enum { RD_MPC_OUTPUT_CSC_DISABLE = 0, RD_MPC_OUTPUT_CSC_COEF_A, RD_MPC_OUTPUT_CSC_COEF_B };
/* enum mpcc_gamut_remap_mode_select */
enum { RD_GAMUT_REMAP_BYPASS = 0, RD_GAMUT_REMAP_COEFF, RD_GAMUT_REMAP_COMA_COEFF };

/* find_color_matrix: the 12 S2.13 coefficients of a color space type, or NULL */
const uint16_t *dcn32_find_color_matrix(uint32_t color_space_type);
/* convert_float_matrix: 31.32 fixed point to S2.13 (clamped to
 * [-4, 4 - 2^-13], sign-magnitude like fixed_point_to_int_frac) */
void dcn32_convert_float_matrix(uint16_t *matrix, const rd_fixpt *flt, uint32_t n);

/* mpc3_set_output_csc (regval NULL only with CSC_DISABLE) */
int  dcn32_mpc_set_output_csc(rdna3_dev *d, uint32_t opp, const uint16_t *regval, uint32_t ocsc_mode);
/* mpc3_set_ocsc_default */
int  dcn32_mpc_set_ocsc_default(rdna3_dev *d, uint32_t opp, uint32_t color_space_type, uint32_t ocsc_mode);
/* mpc3_set_denorm: the depth the OPP gets (RD_COLOR_DEPTH_*; others bypass) */
void dcn32_mpc_set_denorm(rdna3_dev *d, uint32_t opp, uint32_t color_depth);
/* struct mpc_denorm_clamp; mpc3_set_denorm_clamp */
typedef struct rdna3_denorm_clamp {
    uint32_t clamp_max_r_cr, clamp_min_r_cr, clamp_max_g_y, clamp_min_g_y, clamp_max_b_cb, clamp_min_b_cb;
} rdna3_denorm_clamp;
void dcn32_mpc_set_denorm_clamp(rdna3_dev *d, uint32_t opp, const rdna3_denorm_clamp *c);
/* mpc3_set_gamut_remap: 'matrix' NULL = bypass (GRAPHICS_GAMUT_ADJUST_TYPE
 * not SW), else 12 coefficients (3 x 4, row major) programmed into the
 * set not in use, which then becomes current */
void dcn32_mpc_set_gamut_remap(rdna3_dev *d, uint32_t mpcc, const rd_fixpt *matrix);
/* mpc3_mpc_init_single_inst: the MPCC out of every tree, its OPP's output
 * mux off, the OPP's rate control off */
void dcn32_mpc_init_single_inst(rdna3_dev *d, uint32_t mpcc);

#ifdef __cplusplus
}
#endif
#endif
