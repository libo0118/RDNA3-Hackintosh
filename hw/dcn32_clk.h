/*
 * dcn32_clk.h - the DCN 3.2 clock manager subset a second head needs:
 * read the display clocks the GOP left (dcn32_get_vco_frequency_from_reg +
 * dcn32_dump_clk_registers: DENTIST VCO from CLK1_CLK_PLL_REQ, DISPCLK /
 * DPPCLK / DPREFCLK / DCFCLK / DTBCLK from the CLK1 DFS slice dividers), and
 * the DAL mailbox to the PMFW (dcn32_clk_mgr_smu_msg.c: its own
 * MSG/ARG/RESP registers, apart from the driver mailbox smu13 uses) for
 * NumOfDisplays and SetHardMinByFreq.
 *
 * Navi 32 (GC 11.0.3) takes the CLK1 register set of dcn32_clk_mgr_construct;
 * the addresses are absolute, as in amdgpu.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCN32_CLK_H
#define RDNA3_DCN32_CLK_H

#include "rdna3_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* DAL mailbox (dcn32_clk_mgr_smu_msg.c) */
#define RD_DAL_MSG_REG   0x1628Au
#define RD_DAL_ARG_REG   0x16273u
#define RD_DAL_RESP_REG  0x16274u
#define RD_DALSMC_RESULT_OK              0x1
#define RD_DALSMC_MSG_GetSmuVersion      0x2
#define RD_DALSMC_MSG_SetHardMinByFreq   0x9
#define RD_DALSMC_MSG_NumOfDisplays      0xE
#define RD_DALSMC_MSG_ReturnHardMinStatus 0x15
#define RD_CHECK_HARD_MIN_CLK_DISPCLK    0x1
#define RD_CHECK_HARD_MIN_CLK_DPPCLK     0x2
#define RD_CHECK_HARD_MIN_CLK_DPREFCLK   0x4
#define RD_CHECK_HARD_MIN_CLK_DCFCLK     0x8
/* PPCLK_e (smu13_driver_if_v13_0_0.h) */
#define RD_PPCLK_DISPCLK 8
#define RD_PPCLK_DPPCLK  9
#define RD_PPCLK_DCFCLK  11

/* clock generator registers (dcn32_clk_mgr.c, DCN 3.2 set) */
#define RD_CLK1_CLK_PLL_REQ    0x16E37u
#define RD_CLK1_CLK0_DFS_CNTL  0x16E69u   /* + 3 per slice: DISPCLK, DPPCLK, DPREFCLK, DCFCLK, DTBCLK */
#define RD_CLK1_DFS_STRIDE     3
#define RD_DCN32_DFS_REF_KHZ   100000u    /* clk_mgr->dfs_ref_freq_khz */
#define RD_DCN32_VCO_DEFAULT_KHZ 4300000u /* when the PLL register reads 0 */

typedef struct rdna3_dcn_clocks {
    uint32_t vco_khz;
    uint32_t dispclk_khz, dppclk_khz, dprefclk_khz, dcfclk_khz, dtbclk_khz;
} rdna3_dcn_clocks;

/* dentist_get_divider_from_did: DFS divider ID -> divider x4 */
uint32_t dcn32_dentist_divider(uint32_t did);
/* dcn32_get_vco_frequency_from_reg + dcn32_dump_clk_registers */
void dcn32_clk_read(rdna3_dev *d, rdna3_dcn_clocks *c);

/* dcn32_smu_send_msg_with_param: 0, or RDNA3_ETIME when the PMFW does not
 * answer, RDNA3_EIO for any answer but OK */
int  dcn32_smu_send(rdna3_dev *d, uint32_t msg, uint32_t arg, uint32_t *out);
/* dcn30_smu_set_num_of_displays. NOT used: on the RX 7800 XT (PMFW 80.92,
 * driver interface 0x40) the driver mailbox stopped answering right after
 * it, in a VM where the PMFW never saw amdgpu's DAL setup. */
int  dcn32_smu_num_displays(rdna3_dev *d, uint32_t n);
/* dcn32_smu_set_hard_min_by_freq (+ dcn32_smu_wait_get_hard_min_status
 * when the PMFW has it: 'pmfw' >= 0x503900 on GC 11.0.3). 'got_khz' is the
 * PMFW's answer. */
int  dcn32_smu_set_hard_min(rdna3_dev *d, uint32_t ppclk, uint32_t mhz, uint32_t pmfw,
                            uint32_t *got_khz);

/* Can the display clocks carry one more pipe at 'pclk_khz' without ODM
 * combine? DISPCLK and DPPCLK at least the pixel rate (DML's single-pipe
 * floor, before the downspread and ramping margins it adds). 0 or
 * RDNA3_EBUSY. */
int  dcn32_clk_fits(const rdna3_dcn_clocks *c, uint32_t pclk_khz);

#ifdef __cplusplus
}
#endif
#endif
