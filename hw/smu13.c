/*
 * smu13.c - see smu13.h. Mirrors the classic smu_cmn_send_smc_msg_with_param():
 *   wait resp != 0 -> resp = 0 -> arg -> msg -> wait resp != 0 -> read arg.
 *
 * SPDX-License-Identifier: MIT
 */
#include "smu13.h"
#include "navi3x_regs.h"

#define MSG(d)  SOC15_OFF(d, MP1, 0, regMP1_SMN_C2PMSG_66)
#define ARG(d)  SOC15_OFF(d, MP1, 0, regMP1_SMN_C2PMSG_82)
#define RESP(d) SOC15_OFF(d, MP1, 0, regMP1_SMN_C2PMSG_90)

static int poll_resp(rdna3_dev *d, uint32_t *resp)
{
    uint32_t i;
    for (i = 0; i < d->usec_timeout; i++) {
        *resp = rdna3_rreg(d, RESP(d));
        if (*resp)
            return RDNA3_OK;
        d->os.udelay(d->os.ctx, 1);
    }
    return RDNA3_ETIME;
}

static int decode(uint32_t resp)
{
    switch (resp) {
    case PPSMC_Result_OK:               return RDNA3_OK;
    case PPSMC_Result_UnknownCmd:       return RDNA3_EINVAL;
    case PPSMC_Result_CmdRejectedPrereq:
    case PPSMC_Result_CmdRejectedBusy:  return RDNA3_EIO;
    default:                            return RDNA3_EIO;
    }
}

int smu13_send_msg(rdna3_dev *d, uint32_t msg, uint32_t param, uint32_t *out)
{
    uint32_t resp;
    int ret = poll_resp(d, &resp);
    if (ret) {
        RERR(d, "SMU: not ready for msg 0x%x (resp 0)", msg);
        return ret;
    }
    rdna3_wreg(d, RESP(d), 0);
    rdna3_wreg(d, ARG(d), param);
    rdna3_wreg(d, MSG(d), msg);
    ret = poll_resp(d, &resp);
    if (ret) {
        RERR(d, "SMU: msg 0x%x param 0x%x timed out", msg, param);
        return ret;
    }
    if (out)
        *out = rdna3_rreg(d, ARG(d));
    ret = decode(resp);
    if (ret)
        RERR(d, "SMU: msg 0x%x param 0x%x -> resp 0x%x", msg, param, resp);
    return ret;
}

int smu13_hello(rdna3_dev *d, uint32_t *smu_version, uint32_t *if_version)
{
    uint32_t v = 0;
    int ret = smu13_send_msg(d, PPSMC_MSG_TestMessage, 0x1234, &v);
    if (ret)
        return ret;
    ret = smu13_send_msg(d, PPSMC_MSG_GetSmuVersion, 0, smu_version);
    if (ret)
        return ret;
    ret = smu13_send_msg(d, PPSMC_MSG_GetDriverIfVersion, 0, if_version);
    if (ret)
        return ret;
    RLOG(d, "SMU: PMFW %u.%u.%u, driver interface 0x%x",
         (*smu_version >> 16) & 0xff, (*smu_version >> 8) & 0xff, *smu_version & 0xff,
         *if_version);
    return RDNA3_OK;
}
