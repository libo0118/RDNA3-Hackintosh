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
 * And the raise path of dcn32_update_clocks for a mode set that needs more
 * DISPCLK / DPPCLK than the GOP left (the DML 3.2 clock floors, the PMFW's
 * DPM limits, SetHardMinByFreq, the pipe's DPPCLK DTO).
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
/* dcn32_smu_wait_for_dmub_ack_mclk: 1 = the PMFW waits for the DMCUB's ack
 * before an MCLK switch (FW-assisted MCLK switching), 0 = it does not;
 * dcn32_update_clocks sends 0 when no stream uses FAMS */
#define RD_DALSMC_MSG_SetAlwaysWaitDmcubResp 0x14
#define RD_CHECK_HARD_MIN_CLK_DISPCLK    0x1
#define RD_CHECK_HARD_MIN_CLK_DPPCLK     0x2
#define RD_CHECK_HARD_MIN_CLK_DPREFCLK   0x4
#define RD_CHECK_HARD_MIN_CLK_DCFCLK     0x8
#define RD_CHECK_HARD_MIN_CLK_DTBCLK     0x10
#define RD_CHECK_HARD_MIN_CLK_UCLK       0x20
/* PPCLK_e (smu13_driver_if_v13_0_0.h) */
#define RD_PPCLK_DISPCLK 8
#define RD_PPCLK_DPPCLK  9
#define RD_PPCLK_DCFCLK  11
#define RD_PPCLK_DTBCLK  12

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
 * answer, RDNA3_EIO for any answer but OK. The DAL ABI is ported only for
 * discovered MP1 13.0.0/10 with register segment 0; other IPs are refused. */
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
/* the same; '*taken' (may be NULL) 1 when the PMFW answered SetHardMinByFreq
 * OK, so an RDNA3_ETIME with '*taken' 1 is the status wait's: the minimum
 * was accepted but ReturnHardMinStatus did not confirm it within 2 s */
int  dcn32_smu_set_hard_min_ex(rdna3_dev *d, uint32_t ppclk, uint32_t mhz, uint32_t pmfw,
                               uint32_t *got_khz, int *taken);

/* Can the display clocks carry one more pipe at 'pclk_khz' without ODM
 * combine? DISPCLK and DPPCLK at least the pixel rate (DML's single-pipe
 * floor, before the downspread and ramping margins it adds). 0 or
 * RDNA3_EBUSY. */
int  dcn32_clk_fits(const rdna3_dcn_clocks *c, uint32_t pclk_khz);

/* --- raising DISPCLK / DPPCLK for a faster timing (rdna3dispclk=1) --- */
#define RD_DALSMC_MSG_GetDpmFreqByIndex  0xB
/* dcn32_init_clocks: "HW recommends limit of 1950 MHz in display clock" */
#define RD_DCN32_DISPCLK_LIMIT_KHZ 1950000u
/* dml32 (dcn3_2_soc / dcn3_2_ip): dcn_downspread_percent 0.5,
 * dispclk_ramp_margin_percent 1 */
#define RD_DCN32_DOWNSPREAD_PER_MILLE 5
#define RD_DCN32_RAMP_PERCENT 1

/* What the PMFW lets the display clocks reach (dcn32_init_clocks:
 * dcn30_smu_get_smu_version, dcn32_init_single_clock for PPCLK_DISPCLK /
 * PPCLK_DPPCLK through the DAL mailbox, capped at 1950 MHz). */
typedef struct rdna3_dcn_clk_limits {
    uint32_t pmfw;                          /* GetSmuVersion */
    uint32_t dispclk_levels, dppclk_levels;
    uint32_t dispclk_max_khz, dppclk_max_khz;
} rdna3_dcn_clk_limits;
int  dcn32_clk_limits_read(rdna3_dev *d, rdna3_dcn_clk_limits *l);

/* dml32_RoundToDFSGranularity: VCO x 4 / floor(VCO x 4 / clk) (round up)
 * or / ceil(...) (round down) */
uint32_t dcn32_dfs_round(uint32_t vco_khz, uint64_t clk_khz_x1e6, int up);
/* dml32_CalculateRequiredDispclk without ODM: pclk x (1 + downspread) x
 * (1 + ramping margin), to the DFS granularity; without the ramping margin
 * when that does not fit under 'max_khz'. 0 = more than 'max_khz' even so. */
uint32_t dcn32_required_dispclk(uint32_t pclk_khz, uint32_t vco_khz, uint32_t max_khz);
/* dml32_CalculateDPPCLK for one pipe at scale ratio <= 1 (no downscale):
 * pclk x (1 + downspread), to the DFS granularity */
uint32_t dcn32_required_dppclk(uint32_t pclk_khz, uint32_t vco_khz);
/* the DPP clock of pipe 'pipe': the global DPPCLK through its DCCG DTO
 * (DPPCLK<n>_DTO_PARAM phase / modulo when DPPCLK<n>_DTO_ENABLE) */
uint32_t dcn32_pipe_dppclk(rdna3_dev *d, const rdna3_dcn_clocks *c, uint32_t pipe);

/* The raise half of dcn32_update_clocks (prepare_bandwidth, safe_to_lower
 * false), before a timing that needs more: dcn32_update_dppclk_dispclk_freq
 * (to the dentist's divider), SetHardMinByFreq(PPCLK_DPPCLK) when DPPCLK
 * goes up, dcn32_update_clocks_update_dentist = SetHardMinByFreq
 * (PPCLK_DISPCLK), both "floored in MHz", then
 * dcn32_update_clocks_update_dpp_dto -> dccg2_update_dpp_dto for 'pipe'
 * when its DTO would give it less than 'dppclk_khz'. Nothing is lowered.
 * The clocks read back afterwards go to 'now' (may be NULL); RDNA3_EBUSY
 * when the PMFW did not set them. */
int  dcn32_clk_raise(rdna3_dev *d, uint32_t pmfw, uint32_t pipe, uint32_t dispclk_khz, uint32_t dppclk_khz,
                     rdna3_dcn_clocks *now);

/* The DTBCLK a 128b/132b stream runs from (dcn32_update_clocks with
 * dtbclk_en, DTBCLK's DPM table of dcn32_init_single_clock): the PMFW's
 * top DTBCLK level as the hard minimum (SetHardMinByFreq(PPCLK_DTBCLK),
 * MHz rounded up); the PMFW's answer in kHz ("DCCG requires KHz precision
 * for DTBCLK") goes to 'ref_khz' (the level when it answers 0). 0 or the
 * mailbox error. */
int  dcn32_clk_dtbclk_enable(rdna3_dev *d, uint32_t pmfw, uint32_t *ref_khz);

#ifdef __cplusplus
}
#endif
#endif
