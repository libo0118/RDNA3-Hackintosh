/*
 * smu13.h - SMU 13.0.x (MP1) message mailbox, ported from Linux amdgpu
 * smu_cmn.c / smu_v13_0.c (MIT). Mailbox: C2PMSG_66 = message,
 * C2PMSG_82 = argument, C2PMSG_90 = response.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_SMU13_H
#define RDNA3_SMU13_H

#include "rdna3_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* smu_v13_0_7_ppsmc.h */
#define PPSMC_Result_OK                 0x1
#define PPSMC_Result_Failed             0xFF
#define PPSMC_Result_UnknownCmd         0xFE
#define PPSMC_Result_CmdRejectedPrereq  0xFD
#define PPSMC_Result_CmdRejectedBusy    0xFC
#define PPSMC_MSG_TestMessage           0x1
#define PPSMC_MSG_GetSmuVersion         0x2
#define PPSMC_MSG_GetDriverIfVersion    0x3

int smu13_send_msg(rdna3_dev *d, uint32_t msg, uint32_t param, uint32_t *out);
/* Waits for PMFW to be up (response register non-zero), then
 * TestMessage + GetSmuVersion + GetDriverIfVersion. */
int smu13_hello(rdna3_dev *d, uint32_t *smu_version, uint32_t *if_version);

#ifdef __cplusplus
}
#endif
#endif
