/*
 * smu13.c - see smu13.h. Mailbox mirrors smu_cmn_send_smc_msg_with_param():
 *   wait resp != 0 -> resp = 0 -> arg -> msg -> wait resp != 0 -> read arg.
 * Power management mirrors smu_hw_init -> smu_smc_hw_setup for SMU 13.0.10
 * (smu_v13_0_0_ppt.c).
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
    if (*if_version != SMU13_0_0_DRIVER_IF_VERSION)
        RLOG(d, "SMU: driver interface 0x%x differs from the header's 0x%x "
             "(amdgpu goes on with a warning too)", *if_version, SMU13_0_0_DRIVER_IF_VERSION);
    return RDNA3_OK;
}

int smu13_check_fw_status(rdna3_dev *d)
{
    uint32_t flags = rdna3_smn_rreg(d, RD_SMN_MP1_FIRMWARE_FLAGS);
    if (flags & MP1_FIRMWARE_FLAGS__INTERRUPTS_ENABLED_MASK)
        return RDNA3_OK;
    RERR(d, "SMU: MP1 firmware flags 0x%x, PMFW not running", flags);
    return RDNA3_EIO;
}

/* ---- tables ---- */

static int xfer(rdna3_dev *d, uint32_t table_id, uint32_t arg, int to_smu)
{
    /* smu_cmn_update_table: param = (argument << 16) | table_id */
    return smu13_send_msg(d, to_smu ? PPSMC_MSG_TransferTableDram2Smu
                                    : PPSMC_MSG_TransferTableSmu2Dram,
                          ((arg & 0xffff) << 16) | (table_id & 0xffff), NULL);
}

int smu13_table_read(rdna3_dev *d, rdna3_smu *s, uint32_t table_id, uint32_t arg, void *dst,
                     uint32_t size)
{
    int ret;
    if (!s->driver_table || size > RD_SMU_DRIVER_TABLE_SIZE)
        return RDNA3_EINVAL;
    ret = xfer(d, table_id, arg, 0);
    if (ret)
        return ret;
    rdna3_hdp_invalidate(d);
    rdna3_vram_read(d, s->driver_table, dst, size);
    return RDNA3_OK;
}

int smu13_table_write(rdna3_dev *d, rdna3_smu *s, uint32_t table_id, uint32_t arg,
                      const void *src, uint32_t size)
{
    if (!s->driver_table || size > RD_SMU_DRIVER_TABLE_SIZE)
        return RDNA3_EINVAL;
    rdna3_vram_write(d, s->driver_table, src, size);
    /* flush the HDP so the PMFW sees what the CPU wrote (smu_cmn_update_table) */
    rdna3_hdp_flush(d);
    return xfer(d, table_id, arg, 1);
}

/* ---- setup pieces (smu_v13_0.c / smu_v13_0_0_ppt.c) ---- */

static int set_table_locations(rdna3_dev *d, rdna3_smu *s)
{
    uint64_t mc = rdna3_vram_mc(d, s->driver_table);
    int ret;
    /* smu_v13_0_set_driver_table_location */
    ret = smu13_send_msg(d, PPSMC_MSG_SetDriverDramAddrHigh, (uint32_t)(mc >> 32), NULL);
    if (!ret)
        ret = smu13_send_msg(d, PPSMC_MSG_SetDriverDramAddrLow, (uint32_t)mc, NULL);
    if (ret)
        return ret;
    /* smu_v13_0_set_tool_table_location (PMSTATUSLOG) */
    mc = rdna3_vram_mc(d, s->tool_table);
    ret = smu13_send_msg(d, PPSMC_MSG_SetToolsDramAddrHigh, (uint32_t)(mc >> 32), NULL);
    if (!ret)
        ret = smu13_send_msg(d, PPSMC_MSG_SetToolsDramAddrLow, (uint32_t)mc, NULL);
    return ret;
    /* smu_v13_0_notify_memory_pool_location: no DramLog pool (size 0) */
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* smu_v13_0_0_setup_pptable: combo table from the PMFW, driver portion
 * parsed (check_powerplay_table), PPTable_t stored, BoardTable from the
 * VBIOS appended unless SCPM is on. */
static int setup_pptable(rdna3_dev *d, rdna3_smu *s, const rdna3_vbios *vbios)
{
    uint8_t hdr[RD_SMU_PP_SMC_PPTABLE_OFF];
    int ret;

    /* smu_v13_0_0_get_pptable_from_pmfw -> smu_cmn_get_combo_pptable */
    ret = xfer(d, TABLE_COMBO_PPTABLE, 0, 0);
    if (ret) {
        RERR(d, "SMU: TransferTableSmu2Dram(COMBO_PPTABLE) failed");
        return ret;
    }
    rdna3_hdp_invalidate(d);
    rdna3_vram_read(d, s->driver_table, hdr, sizeof(hdr));
    rdna3_vram_read(d, s->driver_table + RD_SMU_PP_SMC_PPTABLE_OFF, &s->pptable,
                    sizeof(s->pptable));

    s->pp_frev = hdr[2];
    s->pp_crev = hdr[3];
    s->platform_caps = rd32(hdr + RD_SMU_PP_PLATFORM_CAPS_OFF);
    s->thermal_controller = hdr[RD_SMU_PP_THERMAL_CTRL_OFF];
    s->software_shutdown_temp = rd16(hdr + RD_SMU_PP_SHUTDOWN_TEMP_OFF);
    /* amdgpu never checks the format (smu_v13_0_0_check_powerplay_table):
     * PMFW 80.92 on the RX 7800 XT reports 20.0 for the same layout */
    if (s->pp_frev != RD_SMU_PP_TABLE_FORMAT_REVISION)
        RLOG(d, "SMU: powerplay table format %u.%u (header documents %u.x), used as is",
             s->pp_frev, s->pp_crev, RD_SMU_PP_TABLE_FORMAT_REVISION);
    if (rd16(hdr) < RD_SMU_PP_SMC_PPTABLE_OFF) {
        RERR(d, "SMU: powerplay table too short (%u bytes)", rd16(hdr));
        return RDNA3_EINVAL;
    }

    /* smu_v13_0_0_append_powerplay_table: BoardTable_t from smc_dpm_info
     * (atom_smc_dpm_info_table_13_0_0); with SCPM the PSP did it */
    s->board_table_appended = 0;
    if (s->scpm) {
        RLOG(d, "SMU: SCPM: the PSP provided the PPTable (BoardTable version 0x%x)",
             s->pptable.BoardTable.Version);
    } else if (vbios) {
        uint16_t size = 0;
        uint8_t frev = 0, crev = 0;
        const uint8_t *t = vbios_data_table(vbios, RD_ATOM_DT_SMC_DPM_INFO, &size, &frev, &crev);
        if (!t) {
            RERR(d, "SMU: VBIOS has no smc_dpm_info table");
            return RDNA3_ENOENT;
        }
        if (size < 4 + sizeof(BoardTable_t)) {
            RERR(d, "SMU: smc_dpm_info %u.%u is %u bytes, BoardTable needs %u", frev, crev, size,
                 (unsigned)(4 + sizeof(BoardTable_t)));
            return RDNA3_EINVAL;
        }
        {
            uint8_t *dst = (uint8_t *)&s->pptable.BoardTable;
            unsigned i;
            for (i = 0; i < sizeof(BoardTable_t); i++)
                dst[i] = t[4 + i];
        }
        s->board_table_appended = 1;
        RLOG(d, "SMU: BoardTable version 0x%x from VBIOS smc_dpm_info %u.%u",
             s->pptable.BoardTable.Version, frev, crev);
    } else {
        RLOG(d, "SMU: no VBIOS, keeping the PMFW's BoardTable (version 0x%x)",
             s->pptable.BoardTable.Version);
    }
    RLOG(d, "SMU: PPTable SKU version 0x%x, platform caps 0x%x, PPT0 %u W AC / %u W DC, "
         "limits edge %u hotspot %u mem %u C, fan max %u rpm",
         s->pptable.SkuTable.Version, s->platform_caps,
         s->pptable.SkuTable.SocketPowerLimitAc[PPT_THROTTLER_PPT0],
         s->pptable.SkuTable.SocketPowerLimitDc[PPT_THROTTLER_PPT0],
         s->pptable.SkuTable.TemperatureLimit[TEMP_EDGE],
         s->pptable.SkuTable.TemperatureLimit[TEMP_HOTSPOT],
         s->pptable.SkuTable.TemperatureLimit[TEMP_MEM], s->pptable.SkuTable.FanMaximumRpm);
    return RDNA3_OK;
}

/* smu_v13_0_set_allowed_mask */
static int set_allowed_mask(rdna3_dev *d, uint64_t allowed)
{
    int ret = smu13_send_msg(d, PPSMC_MSG_SetAllowedFeaturesMaskHigh, (uint32_t)(allowed >> 32),
                             NULL);
    if (ret)
        return ret;
    return smu13_send_msg(d, PPSMC_MSG_SetAllowedFeaturesMaskLow, (uint32_t)allowed, NULL);
}

/* smu_cmn_get_enabled_mask (GetRunningSmuFeaturesLow/High variant) */
static int get_enabled_mask(rdna3_dev *d, uint64_t *mask)
{
    uint32_t lo = 0, hi = 0;
    int ret = smu13_send_msg(d, PPSMC_MSG_GetRunningSmuFeaturesHigh, 0, &hi);
    if (ret)
        return ret;
    ret = smu13_send_msg(d, PPSMC_MSG_GetRunningSmuFeaturesLow, 0, &lo);
    if (ret)
        return ret;
    *mask = ((uint64_t)hi << 32) | lo;
    return RDNA3_OK;
}

/* smu_v13_0_0_update_pcie_parameters with PP_PCIE_DPM on: clamp the
 * table's levels to what the link supports */
static int update_pcie_parameters(rdna3_dev *d, rdna3_smu *s)
{
    const SkuTable_t *sku = &s->pptable.SkuTable;
    uint32_t i, n = 0;
    int ret = RDNA3_OK;
    for (i = 0; i < NUM_LINK_LEVELS; i++) {
        uint32_t gen = sku->PcieGenSpeed[i], lane = sku->PcieLaneCount[i];
        if (!gen && !lane && !sku->LclkFreq[i])
            continue;
        if (gen > s->pcie_gen_cap || lane > s->pcie_width_cap) {
            uint32_t arg;
            if (gen > s->pcie_gen_cap) gen = s->pcie_gen_cap;
            if (lane > s->pcie_width_cap) lane = s->pcie_width_cap;
            arg = (n << 16) | (gen << 8) | lane;
            ret = smu13_send_msg(d, PPSMC_MSG_OverridePcieParameters, arg, NULL);
            if (ret)
                return ret;
        }
        n++;
    }
    s->pcie_levels = n;
    return ret;
}

int smu13_get_dpm_table(rdna3_dev *d, uint32_t ppclk, rdna3_smu_dpm *t)
{
    uint32_t v = 0, i;
    int ret;
    t->count = 0;
    t->fine_grained = 0;
    /* smu_v13_0_get_dpm_level_count: level 0xff returns the count,
     * bit 31 = fine grained (smu_v13_0_get_fine_grained_status) */
    ret = smu13_send_msg(d, PPSMC_MSG_GetDpmFreqByIndex, (ppclk << 16) | 0xff, &v);
    if (ret)
        return ret;
    t->fine_grained = (v >> 31) & 1;
    v &= 0x7fffffff;
    if (v > RD_SMU_DPM_MAX_LEVELS)
        v = RD_SMU_DPM_MAX_LEVELS;
    t->count = v;
    for (i = 0; i < t->count; i++) {
        ret = smu13_send_msg(d, PPSMC_MSG_GetDpmFreqByIndex, (ppclk << 16) | i, &v);
        if (ret)
            return ret;
        t->level[i] = v & 0x7fffffff;
    }
    return RDNA3_OK;
}

int smu13_set_soft_freq_range(rdna3_dev *d, const rdna3_smu *s, uint32_t ppclk, uint32_t min_mhz,
                              uint32_t max_mhz)
{
    int ret = RDNA3_OK;
    (void)s;
    if (max_mhz) {
        ret = smu13_send_msg(d, PPSMC_MSG_SetSoftMaxByFreq, (ppclk << 16) | (max_mhz & 0xffff),
                             NULL);
        if (ret)
            return ret;
    }
    if (min_mhz)
        ret = smu13_send_msg(d, PPSMC_MSG_SetSoftMinByFreq, (ppclk << 16) | (min_mhz & 0xffff),
                             NULL);
    return ret;
}

int smu13_get_power_limit(rdna3_dev *d, rdna3_smu *s, uint32_t *watts)
{
    int ret;
    if (!(s->enabled & RD_SMU_FEATURE(FEATURE_THROTTLERS_BIT)))
        return RDNA3_EINVAL;   /* SMU_FEATURE_PPT_BIT maps to THROTTLERS */
    ret = smu13_send_msg(d, PPSMC_MSG_GetPptLimit, POWER_SOURCE_AC << 16, watts);
    if (!ret)
        s->ppt_limit_w = *watts;
    return ret;
}

int smu13_set_power_limit(rdna3_dev *d, rdna3_smu *s, uint32_t watts)
{
    const SkuTable_t *sku = &s->pptable.SkuTable;
    uint32_t pp = sku->SocketPowerLimitAc[PPT_THROTTLER_PPT0];
    uint32_t msg = sku->MsgLimits.Power[PPT_THROTTLER_PPT0][POWER_SOURCE_AC];
    uint32_t hi = pp > msg ? pp : msg, lo = pp < msg ? pp : msg;
    if (!(s->enabled & RD_SMU_FEATURE(FEATURE_THROTTLERS_BIT)))
        return RDNA3_EINVAL;
    /* smu_v13_0_0_init_ppt_limits: [min(pp,msg), max(pp,msg)] without overdrive */
    if (watts < lo || watts > hi) {
        RERR(d, "SMU: power limit %u W outside %u..%u W", watts, lo, hi);
        return RDNA3_EINVAL;
    }
    return smu13_send_msg(d, PPSMC_MSG_SetPptLimit, watts, NULL);
}

int smu13_set_workload(rdna3_dev *d, uint32_t workload_bit)
{
    return smu13_send_msg(d, PPSMC_MSG_SetWorkloadMask, 1u << workload_bit, NULL);
}

int smu13_gfx_off(rdna3_dev *d, int allow)
{
    return smu13_send_msg(d, allow ? PPSMC_MSG_AllowGfxOff : PPSMC_MSG_DisallowGfxOff, 0, NULL);
}

/* SetHardMinByFreq to the table's top level: the clock stays there */
static int pin_to_top(rdna3_dev *d, uint32_t ppclk, const rdna3_smu_dpm *t, const char *name)
{
    uint32_t mhz;
    if (!t->count)
        return RDNA3_OK;
    mhz = t->level[t->count - 1];
    RLOG(d, "SMU: %s pinned at %u MHz (no p-state switch under the firmware's scanout)", name, mhz);
    return smu13_send_msg(d, PPSMC_MSG_SetHardMinByFreq, (ppclk << 16) | (mhz & 0xffff), NULL);
}

int smu13_pm_init(rdna3_dev *d, rdna3_smu *s, const rdna3_vbios *vbios, uint64_t allowed,
                  uint32_t pcie_gen_cap, uint32_t pcie_width_cap, int scpm)
{
    int ret;
    unsigned i;

    for (i = 0; i < sizeof(*s); i++)
        ((uint8_t *)s)[i] = 0;
    s->pcie_gen_cap = pcie_gen_cap;
    s->pcie_width_cap = pcie_width_cap;
    s->scpm = scpm;

    /* smu_start_smc_engine (PSP loaded the PMFW) -> check_fw_status, versions */
    ret = smu13_check_fw_status(d);
    if (ret)
        return ret;
    s->fw_status_ok = 1;
    ret = smu13_hello(d, &s->version, &s->if_version);
    if (ret)
        return ret;

    /* smu_init_fb_allocations: one driver table sized for the largest
     * exchange, the tool (PMSTATUSLOG) table */
    s->driver_table = rdna3_vram_alloc(d, RD_SMU_DRIVER_TABLE_SIZE, 4096, "smu-driver-table");
    s->tool_table = rdna3_vram_alloc(d, RD_SMU_TOOL_SIZE, 4096, "smu-tool-table");
    if (s->driver_table == UINT64_MAX || s->tool_table == UINT64_MAX) {
        RERR(d, "SMU: no VRAM for the driver/tool tables");
        return RDNA3_ENOMEM;
    }
    rdna3_vram_fill(d, s->driver_table, 0, RD_SMU_DRIVER_TABLE_SIZE);
    rdna3_vram_fill(d, s->tool_table, 0, RD_SMU_TOOL_SIZE);
    rdna3_hdp_flush(d);

    /* smu_v13_0_get_vbios_bootup_values (what runs before DPM is enabled) */
    if (vbios) {
        ret = vbios_bootup_values(d, vbios, &s->boot);
        if (ret)
            return ret;
    }

    /* smu_smc_hw_setup. init_display_count is a no-op on 13.0.x (no
     * NumOfDisplays message); DALNotPresent is only sent by DC. */
    ret = set_table_locations(d, s);
    if (ret) {
        RERR(d, "SMU: SetDriverDramAddr/SetToolsDramAddr failed");
        return ret;
    }
    ret = setup_pptable(d, s, vbios);
    if (ret)
        return ret;
    /* smu_write_pptable -> smu_cmn_write_pptable; with SCPM "PSP is
     * responsible for the PPTable transferring" */
    if (!s->scpm) {
        ret = smu13_table_write(d, s, TABLE_PPTABLE, 0, &s->pptable, sizeof(s->pptable));
        if (ret) {
            RERR(d, "SMU: TransferTableDram2Smu(PPTABLE) failed");
            return ret;
        }
    }
    /* smu_v13_0_run_btc */
    ret = smu13_send_msg(d, PPSMC_MSG_RunDcBtc, 0, NULL);
    if (ret)
        return ret;
    /* smu_feature_set_allowed_mask: "not needed and permitted" with SCPM */
    s->allowed = allowed;
    if (!s->scpm) {
        ret = set_allowed_mask(d, allowed);
        if (ret)
            return ret;
    }
    /* smu_update_pcie_parameters */
    ret = update_pcie_parameters(d, s);
    if (ret) {
        RERR(d, "SMU: OverridePcieParameters failed");
        return ret;
    }
    /* smu_system_features_control(true) */
    ret = smu13_send_msg(d, PPSMC_MSG_EnableAllSmuFeatures, 0, NULL);
    if (ret) {
        RERR(d, "SMU: EnableAllSmuFeatures failed");
        return ret;
    }
    ret = get_enabled_mask(d, &s->enabled);
    if (ret)
        return ret;
    s->dpm_running = (s->enabled & RD_SMU_FEATURES_DPM) != 0;
    RLOG(d, "SMU: features allowed 0x%016llx running 0x%016llx, dpm %s",
         (unsigned long long)s->allowed, (unsigned long long)s->enabled,
         s->dpm_running ? "running" : "disabled");

    /* smu_v13_0_0_set_default_dpm_table for the clocks we report */
    if (s->enabled & RD_SMU_FEATURE(FEATURE_DPM_GFXCLK_BIT)) {
        ret = smu13_get_dpm_table(d, PPCLK_GFXCLK, &s->gfx);
        if (ret)
            return ret;
        /* clamp the top level to GameClockAc like amdgpu */
        if (s->gfx.count && s->pptable.SkuTable.DriverReportedClocks.GameClockAc &&
            s->gfx.level[s->gfx.count - 1] > s->pptable.SkuTable.DriverReportedClocks.GameClockAc)
            s->gfx.level[s->gfx.count - 1] = s->pptable.SkuTable.DriverReportedClocks.GameClockAc;
    } else {
        s->gfx.count = 1;
        s->gfx.level[0] = s->boot.gfxclk / 100;
    }
    if (s->enabled & RD_SMU_FEATURE(FEATURE_DPM_UCLK_BIT)) {
        ret = smu13_get_dpm_table(d, PPCLK_UCLK, &s->uclk);
        if (ret)
            return ret;
    } else {
        s->uclk.count = 1;
        s->uclk.level[0] = s->boot.uclk / 100;
    }
    if (s->enabled & RD_SMU_FEATURE(FEATURE_DPM_SOCCLK_BIT)) {
        ret = smu13_get_dpm_table(d, PPCLK_SOCCLK, &s->soc);
        if (ret)
            return ret;
    } else {
        s->soc.count = 1;
        s->soc.level[0] = s->boot.socclk / 100;
    }
    if (s->enabled & RD_SMU_FEATURE(FEATURE_DPM_FCLK_BIT)) {
        ret = smu13_get_dpm_table(d, PPCLK_FCLK, &s->fclk);
        if (ret)
            return ret;
    } else {
        s->fclk.count = 1;
        s->fclk.level[0] = s->boot.fclk / 100;
    }
    RLOG(d, "SMU: gfxclk %u..%u MHz (%u levels%s), uclk %u..%u MHz (%u), socclk %u..%u MHz (%u)",
         s->gfx.level[0], s->gfx.level[s->gfx.count ? s->gfx.count - 1 : 0], s->gfx.count,
         s->gfx.fine_grained ? ", fine grained" : "",
         s->uclk.level[0], s->uclk.level[s->uclk.count ? s->uclk.count - 1 : 0], s->uclk.count,
         s->soc.level[0], s->soc.level[s->soc.count ? s->soc.count - 1 : 0], s->soc.count);

    /* the PMFW runs UCLK/FCLK DPM although 'allowed' keeps them off
     * (SCPM ignores the allowed mask): hold them at the top level */
    if ((s->enabled & ~allowed & RD_SMU_FEATURE(FEATURE_DPM_UCLK_BIT)) && s->uclk.count) {
        ret = pin_to_top(d, PPCLK_UCLK, &s->uclk, "UCLK");
        if (ret)
            return ret;
    }
    if ((s->enabled & ~allowed & RD_SMU_FEATURE(FEATURE_DPM_FCLK_BIT)) && s->fclk.count) {
        ret = pin_to_top(d, PPCLK_FCLK, &s->fclk, "FCLK");
        if (ret)
            return ret;
    }

    /* smu_late_init bits that matter: the PPT limit and the default
     * workload (smu_v13_0_0_set_power_profile_mode -> SetWorkloadMask) */
    if (s->enabled & RD_SMU_FEATURE(FEATURE_THROTTLERS_BIT)) {
        uint32_t w = 0;
        if (!smu13_get_power_limit(d, s, &w))
            RLOG(d, "SMU: PPT limit %u W", w);
    }
    ret = smu13_set_workload(d, WORKLOAD_PPLIB_DEFAULT_BIT);
    if (ret)
        return ret;
    return RDNA3_OK;
}

int smu13_mode1_reset(rdna3_dev *d)
{
    /* __smu_cmn_send_debug_msg: param, message, response = 0; the PMFW
     * takes the whole chip through reset, so nothing answers */
    rdna3_wreg(d, SOC15_OFF(d, MP1, 0, regMP1_SMN_C2PMSG_53), 0);
    rdna3_wreg(d, SOC15_OFF(d, MP1, 0, regMP1_SMN_C2PMSG_75), RD_DEBUGSMC_MSG_Mode1Reset);
    rdna3_wreg(d, SOC15_OFF(d, MP1, 0, regMP1_SMN_C2PMSG_54), 0);
    d->os.msleep(d->os.ctx, RD_SMU_MODE1_RESET_WAIT_MS);
    return RDNA3_OK;
}

int smu13_pm_fini(rdna3_dev *d, rdna3_smu *s)
{
    (void)s;
    return smu13_send_msg(d, PPSMC_MSG_PrepareMp1ForUnload, 0, NULL);
}

/* ---- metrics ---- */

int smu13_read_metrics(rdna3_dev *d, rdna3_smu *s)
{
    int ret = smu13_table_read(d, s, TABLE_SMU_METRICS, 0, &s->metrics, sizeof(s->metrics));
    s->metrics_valid = ret == RDNA3_OK;
    return ret;
}

#define M(s) (&(s)->metrics.SmuMetrics)
uint32_t smu13_metric_gfxclk_mhz(const rdna3_smu *s) { return M(s)->AverageGfxclkFrequencyPreDs; }
uint32_t smu13_metric_uclk_mhz(const rdna3_smu *s)   { return M(s)->CurrClock[PPCLK_UCLK]; }
uint32_t smu13_metric_temp_edge_c(const rdna3_smu *s)    { return M(s)->AvgTemperature[TEMP_EDGE]; }
uint32_t smu13_metric_temp_hotspot_c(const rdna3_smu *s) { return M(s)->AvgTemperature[TEMP_HOTSPOT]; }
uint32_t smu13_metric_temp_mem_c(const rdna3_smu *s)     { return M(s)->AvgTemperature[TEMP_MEM]; }
uint32_t smu13_metric_power_w(const rdna3_smu *s)        { return M(s)->AverageSocketPower; }
uint32_t smu13_metric_gfx_activity(const rdna3_smu *s)   { return M(s)->AverageGfxActivity; }
uint32_t smu13_metric_fan_rpm(const rdna3_smu *s)        { return M(s)->AvgFanRpm; }
uint32_t smu13_metric_vddgfx_mv(const rdna3_smu *s)      { return M(s)->AvgVoltage[SVI_PLANE_GFX]; }
void smu13_metric_detail(const rdna3_smu *s, rdna3_pm_detail *o)
{
    o->gfx_activity = M(s)->AverageGfxActivity;
    o->uclk_activity = M(s)->AverageUclkActivity;
    o->pcie_busy = M(s)->PCIeBusy;
    o->gfxclk_target = M(s)->AverageGfxclkFrequencyTarget;
    o->gfxclk_pre_ds = M(s)->AverageGfxclkFrequencyPreDs;
    o->gfxclk_post_ds = M(s)->AverageGfxclkFrequencyPostDs;
    o->memclk_post_ds = M(s)->AverageMemclkFrequencyPostDs;
    o->fclk_post_ds = M(s)->AverageFclkFrequencyPostDs;
    o->socclk = M(s)->CurrClock[PPCLK_SOCCLK];
    o->dcefclk = M(s)->CurrClock[PPCLK_DCFCLK];
    o->socket_w = M(s)->AverageSocketPower;
    o->board_w = M(s)->AverageTotalBoardPower;
    o->vddgfx_mv = M(s)->AvgVoltage[SVI_PLANE_GFX];
}

uint32_t smu13_metric_throttlers(const rdna3_smu *s)
{
    /* smu_v13_0_get_throttler_status */
    uint32_t st = 0, i;
    for (i = 0; i < THROTTLER_COUNT; i++)
        if (M(s)->ThrottlingPercentage[i])
            st |= 1u << i;
    return st;
}
