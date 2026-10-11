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
#include "gen/thm_13_0_2_fan.h"
#include "soc21_cg.h"

/* smu_v13_0_set_smu_mailbox_registers / smu_v13_0_0_set_smu_mailbox_registers
 * (the debug mailbox 53/75/54 is in smu13_mode1_reset) */
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

/* smu_v13_0_check_fw_version (after the TestMessage): an if_version
 * mismatch only warns */
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
    /* smu_v13_0_0_store_powerplay_table: the smc_pptable part */
    rdna3_vram_read(d, s->driver_table + RD_SMU_PP_SMC_PPTABLE_OFF, &s->pptable,
                    sizeof(s->pptable));

    s->pp_frev = hdr[2];
    s->pp_crev = hdr[3];
    s->platform_caps = rd32(hdr + RD_SMU_PP_PLATFORM_CAPS_OFF);
    s->thermal_controller = hdr[RD_SMU_PP_THERMAL_CTRL_OFF];
    s->software_shutdown_temp = rd16(hdr + RD_SMU_PP_SHUTDOWN_TEMP_OFF);
    /* overdrive_table.max/min[SMU_13_0_0_ODSETTING_POWERPERCENTAGE]
     * (smu_v13_0_0_get_power_limit reads them from the driver portion) */
    s->od_ppt_pct_max = (int32_t)rd32(hdr + RD_SMU_PP_OD_MAX_OFF +
                                      4 * RD_SMU_13_0_0_ODSETTING_POWERPERCENTAGE);
    s->od_ppt_pct_min = (int32_t)rd32(hdr + RD_SMU_PP_OD_MIN_OFF +
                                      4 * RD_SMU_13_0_0_ODSETTING_POWERPERCENTAGE);
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
        /* smu_v13_0_get_dpm_freq_by_index */
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

/* smu_v13_0_get_current_power_limit (a desktop card: always AC) */
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

/* smu_v13_0_set_power_limit, within the limits of smu_v13_0_0_init_ppt_limits */
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

/* SetHardMinByFreq to the table's top level: the clock stays there
 * (smu_v13_0_set_hard_freq_limited_range with min = top, max = 0) */
static int pin_to_top(rdna3_dev *d, uint32_t ppclk, const rdna3_smu_dpm *t, const char *name)
{
    uint32_t mhz;
    if (!t->count)
        return RDNA3_OK;
    mhz = t->level[t->count - 1];
    RLOG(d, "SMU: %s pinned at %u MHz (no p-state switch under the firmware's scanout)", name, mhz);
    return smu13_send_msg(d, PPSMC_MSG_SetHardMinByFreq, (ppclk << 16) | (mhz & 0xffff), NULL);
}

static void pcie_table_init(rdna3_smu *s);
static int vdclk_tables(rdna3_dev *d, rdna3_smu *s);

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
    if (d->amdgpu_exact && allowed == RD_SMU_FEATURES_AMDGPU)
        allowed = smu13_get_allowed_feature_mask(s->version, RD_PP_FEATURE_MASK_DEFAULT, 0);
    s->allowed = allowed;
    if (!s->scpm) {
        ret = set_allowed_mask(d, allowed);
        if (ret)
            return ret;
    }
    /* smu_update_pcie_parameters; amdgpu itself sends it after the DPM
     * tables (below, rdna3amdgpu=1) */
    if (!d->amdgpu_exact) {
        ret = update_pcie_parameters(d, s);
        if (ret) {
            RERR(d, "SMU: OverridePcieParameters failed");
            return ret;
        }
    }
    /* smu_system_features_control(true) -> smu_v13_0_0_system_features_control
     * -> smu_v13_0_system_features_control */
    ret = smu13_send_msg(d, PPSMC_MSG_EnableAllSmuFeatures, 0, NULL);
    if (ret) {
        RERR(d, "SMU: EnableAllSmuFeatures failed");
        return ret;
    }
    ret = get_enabled_mask(d, &s->enabled);
    if (ret)
        return ret;
    s->dpm_running = (s->enabled & RD_SMU_FEATURES_DPM) != 0;   /* smu_v13_0_0_is_dpm_running */
    /* smu_smc_hw_setup: feature->supported = the enabled mask right now */
    s->supported = s->enabled;
    s->baco_state = RD_SMU_BACO_STATE_NONE;
    s->df_cstate = ~0u;
    s->power_profile_mode = RD_PP_SMC_POWER_PROFILE_BOOTUP_DEFAULT;
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

    if (d->amdgpu_exact) {
        /* the rest of smu_v13_0_0_set_default_dpm_table (VCLK / DCLK, the
         * pcie_table), then smu_smc_hw_setup goes on: update_pcie_parameters,
         * enable_thermal_alert, notify_display_change */
        ret = vdclk_tables(d, s);
        if (ret)
            return ret;
        pcie_table_init(s);
        ret = smu13_update_pcie_parameters(d, s, s->pcie_gen_cap, s->pcie_width_cap);
        if (ret) {
            RERR(d, "SMU: OverridePcieParameters failed");
            return ret;
        }
        /* smu_v13_0_0_check_powerplay_table (setup_pptable) */
        s->dc_controlled_by_gpio = (s->platform_caps & RD_SMU_PP_PLATFORM_CAP_HARDWAREDC) != 0;
        ret = smu13_enable_thermal_alert(d, s);
        if (ret) {
            RERR(d, "SMU: Failed to enable thermal alert!");
            return ret;
        }
        ret = smu13_notify_display_change(d, d->amdgpu_dc);
        if (ret) {
            RERR(d, "SMU: Failed to notify display change!");
            return ret;
        }
    }

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
    s->workload_mask = 1u << WORKLOAD_PPLIB_DEFAULT_BIT;
    return RDNA3_OK;
}

int smu13_mode1_reset(rdna3_dev *d)
{
    /* smu_v13_0_0_set_mode1_reset_param: param 0, the RAS fatal error
     * flag (1 << 16) needs RAS, which this card does not have.
     * __smu_cmn_send_debug_msg: param, message, response = 0; the PMFW
     * takes the whole chip through reset, so nothing answers */
    rdna3_wreg(d, SOC15_OFF(d, MP1, 0, regMP1_SMN_C2PMSG_53), 0);
    rdna3_wreg(d, SOC15_OFF(d, MP1, 0, regMP1_SMN_C2PMSG_75), RD_DEBUGSMC_MSG_Mode1Reset);
    rdna3_wreg(d, SOC15_OFF(d, MP1, 0, regMP1_SMN_C2PMSG_54), 0);
    d->os.msleep(d->os.ctx, RD_SMU_MODE1_RESET_WAIT_MS);
    return RDNA3_OK;
}

int smu13_pm_fini(rdna3_dev *d, rdna3_smu *s)
{
    if (d->amdgpu_exact) {
        /* smu_hw_fini -> smu_smc_hw_cleanup's disable_thermal_alert, then
         * smu_reset_mp1_state: UNLOAD only on a 13.0.10 without display
         * hardware, which the RX 7800 XT is not */
        if (s->irq_enabled)
            smu13_disable_thermal_alert(d, s);
        return RDNA3_OK;
    }
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
/* smu_v13_0_0_get_fan_speed_rpm: METRICS_CURR_FANSPEED */
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

/* ======================================================================
 * PM control: fan, power limit, overdrive, clocks / profiles, BACO.
 * Ports of smu_v13_0.c and smu_v13_0_0_ppt.c (Linux v6.12; the zero RPM
 * overdrive controls follow v6.14). Every entry point keeps amdgpu's
 * checks and order of messages.
 * ====================================================================== */

static void bytes_copy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d8 = (uint8_t *)dst;
    const uint8_t *s8 = (const uint8_t *)src;
    while (n--)
        *d8++ = *s8++;
}

static int bytes_cmp(const void *a, const void *b, uint32_t n)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x < *y ? -1 : 1;
    return 0;
}

static void bytes_zero(void *p, uint32_t n)
{
    uint8_t *b = (uint8_t *)p;
    while (n--)
        *b++ = 0;
}

#define RD_FIELD_GET(v, R, F)    (((v) & R##__##F##_MASK) >> R##__##F##__SHIFT)
#define RD_FIELD_SET(v, R, F, x) (((v) & ~(uint32_t)R##__##F##_MASK) | \
                                  (((uint32_t)(x) << R##__##F##__SHIFT) & (uint32_t)R##__##F##_MASK))
#define THM_REG(d, reg)          rdna3_reg_offset((d), HWIP_THM, 0, reg##_BASE_IDX, reg)

/* smu_cmn_feature_is_supported: the running set smu_smc_hw_setup saw */
static int feature_supported(const rdna3_smu *s, int bit)
{
    return (int)((s->supported >> bit) & 1);
}

/* smu_cmn_feature_is_enabled: asks the PMFW (GetRunningSmuFeatures*) */
static int feature_enabled(rdna3_dev *d, rdna3_smu *s, int bit)
{
    uint64_t m = 0;
    if (get_enabled_mask(d, &m))
        return 0;
    s->enabled = m;
    return (int)((m >> bit) & 1);
}

/* smu_cmn_feature_set_enabled -> smu_cmn_feature_update_enable_state */
static int feature_set_enabled(rdna3_dev *d, rdna3_smu *s, int bit, int enable)
{
    uint64_t mask = 1ull << bit;
    int ret = smu13_send_msg(d, enable ? PPSMC_MSG_EnableSmuFeaturesLow : PPSMC_MSG_DisableSmuFeaturesLow,
                             (uint32_t)mask, NULL);
    if (ret)
        return ret;
    ret = smu13_send_msg(d, enable ? PPSMC_MSG_EnableSmuFeaturesHigh : PPSMC_MSG_DisableSmuFeaturesHigh,
                         (uint32_t)(mask >> 32), NULL);
    if (!ret)
        s->enabled = enable ? (s->enabled | mask) : (s->enabled & ~mask);
    return ret;
}

/* smu_cmn_clk_dpm_is_enabled for the clocks with a DPM table here */
static int clk_dpm_feature(uint32_t ppclk)
{
    switch (ppclk) {
    case PPCLK_GFXCLK: return FEATURE_DPM_GFXCLK_BIT;
    case PPCLK_UCLK:   return FEATURE_DPM_UCLK_BIT;
    case PPCLK_SOCCLK: return FEATURE_DPM_SOCCLK_BIT;
    case PPCLK_FCLK:   return FEATURE_DPM_FCLK_BIT;
    default:           return -1;
    }
}

static const rdna3_smu_dpm *dpm_table_of(const rdna3_smu *s, uint32_t ppclk)
{
    switch (ppclk) {
    case PPCLK_GFXCLK: return &s->gfx;
    case PPCLK_UCLK:   return &s->uclk;
    case PPCLK_SOCCLK: return &s->soc;
    case PPCLK_FCLK:   return &s->fclk;
    default:           return NULL;
    }
}

/* ---- late init: what smu_late_init reads back ---- */

int smu13_pm_query(rdna3_dev *d, rdna3_smu *s, const rdna3_vbios *vbios, int od_wanted)
{
    const SkuTable_t *sku = &s->pptable.SkuTable;
    uint32_t cur = 0, def = 0, max = 0, min = 0;

    /* Ported from smu_v13_0_0_check_powerplay_table: the bits the setup
     * did not take (platform caps, OD limits, fan) */
    s->dc_controlled_by_gpio = (s->platform_caps & RD_SMU_PP_PLATFORM_CAP_HARDWAREDC) != 0;
    s->baco_platform = s->maco_support = 0;
    if (s->platform_caps & RD_SMU_PP_PLATFORM_CAP_BACO) {
        s->baco_platform = 1;
        if (s->platform_caps & RD_SMU_PP_PLATFORM_CAP_MACO)
            s->maco_support = 1;
    }
    /* smu_set_funcs: od_enabled from ppfeaturemask PP_OVERDRIVE_MASK */
    s->od_enabled = od_wanted != 0;
    if (!sku->OverDriveLimitsMin.FeatureCtrlMask || !sku->OverDriveLimitsBasicMax.FeatureCtrlMask)
        s->od_enabled = 0;
    s->no_fan = !(sku->FeaturesToRun[0] & (1u << FEATURE_FAN_CONTROL_BIT));

    /* amdgpu_atomfirmware_scratch_regs_init: bios_scratch_reg_startaddr */
    s->bios_scratch_reg = 0;
    if (vbios) {
        uint16_t size = 0;
        const uint8_t *t = vbios_data_table(vbios, RD_ATOM_DT_FIRMWAREINFO, &size, NULL, NULL);
        if (t && size >= 0x1c)
            s->bios_scratch_reg = rd32(t + 0x18);
    }

    smu13_populate_umd_state_clk(s);
    /* smu_late_init -> smu_set_ac_dc (rdna3amdgpu=1): a desktop runs on
     * AC; nothing when the board's GPIO controls AC/DC */
    if (d->amdgpu_exact && !s->dc_controlled_by_gpio) {
        if (smu13_set_power_source(d, 0) == RDNA3_OK)
            s->ac_power = 1;
        else
            RERR(d, "SMU: Failed to switch to AC mode!");
    }
    /* smu_get_asic_power_limits */
    if (smu13_get_power_limits(d, s, &cur, &def, &max, &min) == RDNA3_OK) {
        s->current_power_limit = cur;
        s->power_limit_default = def;
        s->power_limit_max = max;
        s->power_limit_min = min;
    }
    smu13_get_unique_id(d, s);
    RLOG(d, "SMU: power limit %u W (%u..%u W), OD %s (features 0x%x, PPT -%d..+%d%%), BACO%s%s, %s",
         s->current_power_limit, s->power_limit_min, s->power_limit_max,
         s->od_enabled ? "on" : "off", sku->OverDriveLimitsBasicMax.FeatureCtrlMask,
         s->od_ppt_pct_min, s->od_ppt_pct_max, s->baco_platform ? "" : " not supported",
         s->maco_support ? " + MACO" : "", s->no_fan ? "no fan" : "fan control");
    return RDNA3_OK;
}

/* ---- fan ---- */

/* Ported from smu_v13_0_get_fan_control_mode */
uint32_t smu13_get_fan_control_mode(rdna3_dev *d, rdna3_smu *s)
{
    if (!feature_enabled(d, s, FEATURE_FAN_CONTROL_BIT))
        return RD_FAN_CTRL_MANUAL;
    else
        return RD_FAN_CTRL_AUTO;
}

/* Ported from smu_v13_0_auto_fan_control */
static int auto_fan_control(rdna3_dev *d, rdna3_smu *s, int auto_fan)
{
    int ret;
    if (!feature_supported(s, FEATURE_FAN_CONTROL_BIT))
        return RDNA3_OK;
    ret = feature_set_enabled(d, s, FEATURE_FAN_CONTROL_BIT, auto_fan);
    if (ret)
        RERR(d, "SMU: %s smc FAN CONTROL feature failed", auto_fan ? "Start" : "Stop");
    return ret;
}

/* Ported from smu_v13_0_set_fan_static_mode */
static int set_fan_static_mode(rdna3_dev *d, uint32_t mode)
{
    rdna3_wreg(d, THM_REG(d, regCG_FDO_CTRL2),
               RD_FIELD_SET(rdna3_rreg(d, THM_REG(d, regCG_FDO_CTRL2)), CG_FDO_CTRL2, TMIN, 0));
    rdna3_wreg(d, THM_REG(d, regCG_FDO_CTRL2),
               RD_FIELD_SET(rdna3_rreg(d, THM_REG(d, regCG_FDO_CTRL2)), CG_FDO_CTRL2, FDO_PWM_MODE, mode));
    return RDNA3_OK;
}

/* Ported from smu_v13_0_set_fan_speed_pwm */
int smu13_set_fan_speed_pwm(rdna3_dev *d, rdna3_smu *s, uint32_t speed)
{
    uint32_t duty100, duty;
    uint64_t tmp64;

    if (!d->ipver[HWIP_THM].present)
        return RDNA3_ENODEV;   /* no THM block in the discovery table */
    if (speed > 255)
        speed = 255;
    if (auto_fan_control(d, s, 0))
        return RDNA3_EINVAL;
    duty100 = RD_FIELD_GET(rdna3_rreg(d, THM_REG(d, regCG_FDO_CTRL1)), CG_FDO_CTRL1, FMAX_DUTY100);
    if (!duty100)
        return RDNA3_EINVAL;
    tmp64 = (uint64_t)speed * duty100;
    duty = (uint32_t)(tmp64 / 255);
    rdna3_wreg(d, THM_REG(d, regCG_FDO_CTRL0),
               RD_FIELD_SET(rdna3_rreg(d, THM_REG(d, regCG_FDO_CTRL0)), CG_FDO_CTRL0, FDO_STATIC_DUTY, duty));
    return set_fan_static_mode(d, RD_FDO_PWM_MODE_STATIC);
}

/* Ported from smu_v13_0_set_fan_control_mode */
int smu13_set_fan_control_mode(rdna3_dev *d, rdna3_smu *s, uint32_t mode)
{
    int ret = RDNA3_OK;
    switch (mode) {
    case RD_FAN_CTRL_NONE:
        ret = smu13_set_fan_speed_pwm(d, s, 255);
        break;
    case RD_FAN_CTRL_MANUAL:
        ret = auto_fan_control(d, s, 0);
        break;
    case RD_FAN_CTRL_AUTO:
        ret = auto_fan_control(d, s, 1);
        break;
    default:
        break;
    }
    if (ret) {
        RERR(d, "SMU: set fan control mode %u failed", mode);
        return RDNA3_EINVAL;
    }
    return ret;
}

/* Ported from smu_v13_0_set_fan_speed_rpm */
int smu13_set_fan_speed_rpm(rdna3_dev *d, rdna3_smu *s, uint32_t speed)
{
    uint32_t crystal_clock_freq = 2500, tach_period;
    int ret;

    if (!speed)
        return RDNA3_EINVAL;
    if (!d->ipver[HWIP_THM].present)
        return RDNA3_ENODEV;
    ret = auto_fan_control(d, s, 0);
    if (ret)
        return ret;
    tach_period = 60 * crystal_clock_freq * 10000 / (8 * speed);
    rdna3_wreg(d, THM_REG(d, regCG_TACH_CTRL),
               RD_FIELD_SET(rdna3_rreg(d, THM_REG(d, regCG_TACH_CTRL)), CG_TACH_CTRL, TARGET_PERIOD,
                            tach_period));
    return set_fan_static_mode(d, RD_FDO_PWM_MODE_STATIC_RPM);
}

/* Ported from smu_v13_0_0_get_fan_speed_pwm: the PMFW's percent -> 0..255 */
int smu13_get_fan_speed_pwm(rdna3_dev *d, rdna3_smu *s, uint32_t *speed)
{
    int ret;
    if (!speed)
        return RDNA3_EINVAL;
    ret = smu13_read_metrics(d, s);     /* METRICS_CURR_FANPWM */
    if (ret) {
        RERR(d, "SMU: Failed to get fan speed(PWM)");
        return ret;
    }
    *speed = (uint32_t)s->metrics.SmuMetrics.AvgFanPwm * 255 / 100;
    if (*speed > 255)
        *speed = 255;
    return RDNA3_OK;
}

/* Ported from smu_v13_0_0_get_fan_speed_rpm (METRICS_CURR_FANSPEED) */
int smu13_get_fan_speed_rpm(rdna3_dev *d, rdna3_smu *s, uint32_t *speed)
{
    int ret;
    if (!speed)
        return RDNA3_EINVAL;
    ret = smu13_read_metrics(d, s);
    if (!ret)
        *speed = s->metrics.SmuMetrics.AvgFanRpm;
    return ret;
}

/* ---- power limit ---- */

/* Ported from smu_v13_0_0_get_power_limit (a desktop card: AC) */
int smu13_get_power_limits(rdna3_dev *d, rdna3_smu *s, uint32_t *current_w, uint32_t *default_w,
                           uint32_t *max_w, uint32_t *min_w)
{
    const SkuTable_t *sku = &s->pptable.SkuTable;
    uint32_t power_limit = 0, od_percent_upper = 0, od_percent_lower = 0;
    uint32_t msg_limit = sku->MsgLimits.Power[PPT_THROTTLER_PPT0][POWER_SOURCE_AC];

    /* smu_v13_0_get_current_power_limit */
    if (smu13_get_power_limit(d, s, &power_limit))
        power_limit = sku->SocketPowerLimitAc[PPT_THROTTLER_PPT0];
    if (current_w)
        *current_w = power_limit;
    if (default_w)
        *default_w = power_limit;

    if (s->od_enabled && smu13_is_od_feature_supported(s, PP_OD_FEATURE_PPT_BIT)) {
        od_percent_upper = (uint32_t)s->od_ppt_pct_max;
        od_percent_lower = (uint32_t)s->od_ppt_pct_min;
    } else if (smu13_is_od_feature_supported(s, PP_OD_FEATURE_PPT_BIT)) {
        od_percent_upper = 0;
        od_percent_lower = (uint32_t)s->od_ppt_pct_min;
    }
    if (max_w)
        *max_w = msg_limit * (100 + od_percent_upper) / 100;
    if (min_w)
        *min_w = power_limit * (100 - od_percent_lower) / 100;
    return RDNA3_OK;
}

/* smu_v13_0_set_power_limit (SMU_DEFAULT_PPT_LIMIT) */
static int v13_set_power_limit(rdna3_dev *d, rdna3_smu *s, uint32_t limit)
{
    int ret;
    if (!(s->enabled & RD_SMU_FEATURE(FEATURE_THROTTLERS_BIT))) {
        RERR(d, "SMU: Setting new power limit is not supported");
        return RDNA3_EOPNOTSUPP;
    }
    ret = smu13_send_msg(d, PPSMC_MSG_SetPptLimit, limit, NULL);
    if (ret) {
        RERR(d, "SMU: Set power limit Failed");
        return ret;
    }
    s->current_power_limit = limit;
    return RDNA3_OK;
}

/* smu_set_power_limit's range check, then Ported from
 * smu_v13_0_0_set_power_limit: above the message limit the PPT goes to it
 * and the rest is the OD table's Ppt percentage */
int smu13_set_ppt_limit(rdna3_dev *d, rdna3_smu *s, uint32_t limit)
{
    const SkuTable_t *sku = &s->pptable.SkuTable;
    uint32_t msg_limit = sku->MsgLimits.Power[PPT_THROTTLER_PPT0][POWER_SOURCE_AC];
    int ret;

    if (limit > s->power_limit_max || limit < s->power_limit_min) {
        RERR(d, "SMU: New power limit (%u) is out of range [%u,%u]", limit, s->power_limit_min,
             s->power_limit_max);
        return RDNA3_EINVAL;
    }
    if (!limit)
        limit = s->current_power_limit;

    if (limit <= msg_limit) {
        if (s->current_power_limit > msg_limit) {
            if (!s->od_valid)
                return RDNA3_EOPNOTSUPP;
            s->od.OverDriveTable.Ppt = 0;
            s->od.OverDriveTable.FeatureCtrlMask |= 1u << PP_OD_FEATURE_PPT_BIT;
            ret = smu13_upload_overdrive_table(d, s, &s->od);
            if (ret) {
                RERR(d, "SMU: Failed to upload overdrive table");
                return ret;
            }
        }
        return v13_set_power_limit(d, s, limit);
    } else if (s->od_enabled) {
        if (!s->od_valid)
            return RDNA3_EOPNOTSUPP;
        ret = v13_set_power_limit(d, s, msg_limit);
        if (ret)
            return ret;
        s->od.OverDriveTable.Ppt = (int16_t)((limit * 100) / msg_limit - 100);
        s->od.OverDriveTable.FeatureCtrlMask |= 1u << PP_OD_FEATURE_PPT_BIT;
        ret = smu13_upload_overdrive_table(d, s, &s->od);
        if (ret) {
            RERR(d, "SMU: Failed to upload overdrive table");
            return ret;
        }
        s->current_power_limit = limit;
    } else {
        return RDNA3_EINVAL;
    }
    return RDNA3_OK;
}

/* ---- overdrive ---- */

/* Ported from smu_v13_0_0_is_od_feature_supported */
int smu13_is_od_feature_supported(const rdna3_smu *s, int od_feature_bit)
{
    return (s->pptable.SkuTable.OverDriveLimitsBasicMax.FeatureCtrlMask & (1u << od_feature_bit)) != 0;
}

/* Ported from smu_v13_0_0_get_od_setting_limits (+ v6.14's zero RPM) */
void smu13_get_od_setting_limits(const rdna3_smu *s, int od_setting, int32_t *min, int32_t *max)
{
    const OverDriveLimits_t *up = &s->pptable.SkuTable.OverDriveLimitsBasicMax;
    const OverDriveLimits_t *lo = &s->pptable.SkuTable.OverDriveLimitsMin;
    int32_t od_min_setting, od_max_setting;

    switch (od_setting) {
    case RD_OD_SET_GFXCLK_FMIN:
        od_min_setting = lo->GfxclkFmin;
        od_max_setting = up->GfxclkFmin;
        break;
    case RD_OD_SET_GFXCLK_FMAX:
        od_min_setting = lo->GfxclkFmax;
        od_max_setting = up->GfxclkFmax;
        break;
    case RD_OD_SET_UCLK_FMIN:
        od_min_setting = lo->UclkFmin;
        od_max_setting = up->UclkFmin;
        break;
    case RD_OD_SET_UCLK_FMAX:
        od_min_setting = lo->UclkFmax;
        od_max_setting = up->UclkFmax;
        break;
    case RD_OD_SET_GFX_VF_CURVE:
        od_min_setting = lo->VoltageOffsetPerZoneBoundary;
        od_max_setting = up->VoltageOffsetPerZoneBoundary;
        break;
    case RD_OD_SET_FAN_CURVE_TEMP:
        od_min_setting = lo->FanLinearTempPoints;
        od_max_setting = up->FanLinearTempPoints;
        break;
    case RD_OD_SET_FAN_CURVE_PWM:
        od_min_setting = lo->FanLinearPwmPoints;
        od_max_setting = up->FanLinearPwmPoints;
        break;
    case RD_OD_SET_FAN_ACOUSTIC_LIMIT:
        od_min_setting = lo->AcousticLimitRpmThreshold;
        od_max_setting = up->AcousticLimitRpmThreshold;
        break;
    case RD_OD_SET_FAN_ACOUSTIC_TARGET:
        od_min_setting = lo->AcousticTargetRpmThreshold;
        od_max_setting = up->AcousticTargetRpmThreshold;
        break;
    case RD_OD_SET_FAN_TARGET_TEMPERATURE:
        od_min_setting = lo->FanTargetTemperature;
        od_max_setting = up->FanTargetTemperature;
        break;
    case RD_OD_SET_FAN_MINIMUM_PWM:
        od_min_setting = lo->FanMinimumPwm;
        od_max_setting = up->FanMinimumPwm;
        break;
    case RD_OD_SET_FAN_ZERO_RPM_ENABLE:
        od_min_setting = lo->FanZeroRpmEnable;
        od_max_setting = up->FanZeroRpmEnable;
        break;
    case RD_OD_SET_FAN_ZERO_RPM_STOP_TEMP:
        od_min_setting = lo->FanZeroRpmStopTemp;
        od_max_setting = up->FanZeroRpmStopTemp;
        break;
    default:
        od_min_setting = od_max_setting = 0x7fffffff;   /* INT_MAX */
        break;
    }
    if (min)
        *min = od_min_setting;
    if (max)
        *max = od_max_setting;
}

/* Ported from smu_v13_0_0_get_overdrive_table */
int smu13_get_overdrive_table(rdna3_dev *d, rdna3_smu *s, OverDriveTableExternal_t *t)
{
    int ret = smu13_table_read(d, s, TABLE_OVERDRIVE, 0, t, sizeof(*t));
    if (ret)
        RERR(d, "SMU: Failed to get overdrive table");
    return ret;
}

/* Ported from smu_v13_0_0_upload_overdrive_table */
int smu13_upload_overdrive_table(rdna3_dev *d, rdna3_smu *s, OverDriveTableExternal_t *t)
{
    int ret = smu13_table_write(d, s, TABLE_OVERDRIVE, 0, t, sizeof(*t));
    if (ret)
        RERR(d, "SMU: Failed to upload overdrive table");
    return ret;
}

/* Ported from smu_v13_0_0_set_supported_od_feature_mask (+ v6.14's zero RPM) */
static void set_supported_od_feature_mask(rdna3_smu *s)
{
    if (smu13_is_od_feature_supported(s, PP_OD_FEATURE_FAN_CURVE_BIT))
        s->od_feature_mask |= RD_OD_OPS_SUPPORT_FAN_CURVE_RETRIEVE | RD_OD_OPS_SUPPORT_FAN_CURVE_SET |
            RD_OD_OPS_SUPPORT_ACOUSTIC_LIMIT_THRESHOLD_RETRIEVE |
            RD_OD_OPS_SUPPORT_ACOUSTIC_LIMIT_THRESHOLD_SET |
            RD_OD_OPS_SUPPORT_ACOUSTIC_TARGET_THRESHOLD_RETRIEVE |
            RD_OD_OPS_SUPPORT_ACOUSTIC_TARGET_THRESHOLD_SET |
            RD_OD_OPS_SUPPORT_FAN_TARGET_TEMPERATURE_RETRIEVE |
            RD_OD_OPS_SUPPORT_FAN_TARGET_TEMPERATURE_SET |
            RD_OD_OPS_SUPPORT_FAN_MINIMUM_PWM_RETRIEVE | RD_OD_OPS_SUPPORT_FAN_MINIMUM_PWM_SET |
            RD_OD_OPS_SUPPORT_FAN_ZERO_RPM_ENABLE_RETRIEVE | RD_OD_OPS_SUPPORT_FAN_ZERO_RPM_ENABLE_SET |
            RD_OD_OPS_SUPPORT_FAN_ZERO_RPM_STOP_TEMP_RETRIEVE |
            RD_OD_OPS_SUPPORT_FAN_ZERO_RPM_STOP_TEMP_SET;
}

/* Ported from smu_v13_0_0_set_default_od_settings. 'in_suspend': a resume
 * (or re-init after a reset) keeps the user's values in od_user. */
int smu13_set_default_od_settings(rdna3_dev *d, rdna3_smu *s, int in_suspend)
{
    OverDriveTableExternal_t bak;
    OverDriveTable_t *u = &s->od_user.OverDriveTable;
    int ret, i;

    ret = smu13_get_overdrive_table(d, s, &s->od_boot);
    if (ret)
        return ret;
    RDBG(d, "SMU: OD: Gfxclk: (%d, %d) Uclk: (%d, %d)", s->od_boot.OverDriveTable.GfxclkFmin,
         s->od_boot.OverDriveTable.GfxclkFmax, s->od_boot.OverDriveTable.UclkFmin,
         s->od_boot.OverDriveTable.UclkFmax);
    bytes_copy(&s->od, &s->od_boot, sizeof(s->od));

    if (!in_suspend) {
        bytes_copy(&s->od_user, &s->od_boot, sizeof(s->od_user));
        s->user_od = 0;
    } else if (s->user_od) {
        bytes_copy(&bak, &s->od_user, sizeof(bak));
        bytes_copy(&s->od_user, &s->od_boot, sizeof(s->od_user));
        u->GfxclkFmin = bak.OverDriveTable.GfxclkFmin;
        u->GfxclkFmax = bak.OverDriveTable.GfxclkFmax;
        u->UclkFmin = bak.OverDriveTable.UclkFmin;
        u->UclkFmax = bak.OverDriveTable.UclkFmax;
        for (i = 0; i < PP_NUM_OD_VF_CURVE_POINTS; i++)
            u->VoltageOffsetPerZoneBoundary[i] = bak.OverDriveTable.VoltageOffsetPerZoneBoundary[i];
        for (i = 0; i < NUM_OD_FAN_MAX_POINTS - 1; i++) {
            u->FanLinearTempPoints[i] = bak.OverDriveTable.FanLinearTempPoints[i];
            u->FanLinearPwmPoints[i] = bak.OverDriveTable.FanLinearPwmPoints[i];
        }
        u->AcousticLimitRpmThreshold = bak.OverDriveTable.AcousticLimitRpmThreshold;
        u->AcousticTargetRpmThreshold = bak.OverDriveTable.AcousticTargetRpmThreshold;
        u->FanTargetTemperature = bak.OverDriveTable.FanTargetTemperature;
        u->FanMinimumPwm = bak.OverDriveTable.FanMinimumPwm;
        u->FanZeroRpmEnable = bak.OverDriveTable.FanZeroRpmEnable;
        u->FanZeroRpmStopTemp = bak.OverDriveTable.FanZeroRpmStopTemp;
    }
    set_supported_od_feature_mask(s);
    s->od_valid = 1;
    return RDNA3_OK;
}

/* Ported from smu_v13_0_0_restore_user_od_settings */
int smu13_restore_user_od_settings(rdna3_dev *d, rdna3_smu *s)
{
    int res;
    s->od_user.OverDriveTable.FeatureCtrlMask = (1u << PP_OD_FEATURE_GFXCLK_BIT) |
        (1u << PP_OD_FEATURE_UCLK_BIT) | (1u << PP_OD_FEATURE_GFX_VF_CURVE_BIT) |
        (1u << PP_OD_FEATURE_FAN_CURVE_BIT);
    res = smu13_upload_overdrive_table(d, s, &s->od_user);
    s->od_user.OverDriveTable.FeatureCtrlMask = 0;
    if (res == 0)
        bytes_copy(&s->od, &s->od_user, sizeof(s->od));
    return res;
}

/* Ported from smu_v13_0_0_od_restore_table_single (+ v6.14's zero RPM) */
static int od_restore_table_single(rdna3_dev *d, rdna3_smu *s, long input)
{
    OverDriveTable_t *od = &s->od.OverDriveTable;
    const OverDriveTable_t *boot = &s->od_boot.OverDriveTable;
    int i;

    switch (input) {
    case RD_PP_OD_EDIT_FAN_CURVE:
        for (i = 0; i < NUM_OD_FAN_MAX_POINTS; i++) {
            od->FanLinearTempPoints[i] = boot->FanLinearTempPoints[i];
            od->FanLinearPwmPoints[i] = boot->FanLinearPwmPoints[i];
        }
        od->FanMode = FAN_MODE_AUTO;
        od->FeatureCtrlMask |= 1u << PP_OD_FEATURE_FAN_CURVE_BIT;
        break;
    case RD_PP_OD_EDIT_ACOUSTIC_LIMIT:
        od->AcousticLimitRpmThreshold = boot->AcousticLimitRpmThreshold;
        od->FanMode = FAN_MODE_AUTO;
        od->FeatureCtrlMask |= 1u << PP_OD_FEATURE_FAN_CURVE_BIT;
        break;
    case RD_PP_OD_EDIT_ACOUSTIC_TARGET:
        od->AcousticTargetRpmThreshold = boot->AcousticTargetRpmThreshold;
        od->FanMode = FAN_MODE_AUTO;
        od->FeatureCtrlMask |= 1u << PP_OD_FEATURE_FAN_CURVE_BIT;
        break;
    case RD_PP_OD_EDIT_FAN_TARGET_TEMPERATURE:
        od->FanTargetTemperature = boot->FanTargetTemperature;
        od->FanMode = FAN_MODE_AUTO;
        od->FeatureCtrlMask |= 1u << PP_OD_FEATURE_FAN_CURVE_BIT;
        break;
    case RD_PP_OD_EDIT_FAN_MINIMUM_PWM:
        od->FanMinimumPwm = boot->FanMinimumPwm;
        od->FanMode = FAN_MODE_AUTO;
        od->FeatureCtrlMask |= 1u << PP_OD_FEATURE_FAN_CURVE_BIT;
        break;
    case RD_PP_OD_EDIT_FAN_ZERO_RPM_ENABLE:
        od->FanZeroRpmEnable = boot->FanZeroRpmEnable;
        od->FeatureCtrlMask |= 1u << PP_OD_FEATURE_ZERO_FAN_BIT;
        break;
    case RD_PP_OD_EDIT_FAN_ZERO_RPM_STOP_TEMP:
        od->FanZeroRpmStopTemp = boot->FanZeroRpmStopTemp;
        od->FeatureCtrlMask |= 1u << PP_OD_FEATURE_ZERO_FAN_BIT;
        break;
    default:
        RLOG(d, "SMU: Invalid table index: %ld", input);
        return RDNA3_EINVAL;
    }
    return RDNA3_OK;
}

/* one value of an OD setting within its limits */
static int od_in_range(rdna3_dev *d, const rdna3_smu *s, int setting, long v, const char *what)
{
    int32_t minimum, maximum;
    smu13_get_od_setting_limits(s, setting, &minimum, &maximum);
    if (v < minimum || v > maximum) {
        RLOG(d, "SMU: %s (%ld) must be within [%d, %d]", what, v, minimum, maximum);
        return 0;
    }
    return 1;
}

/* the single-value fan settings (one shape in amdgpu) */
static int od_edit_fan_value(rdna3_dev *d, rdna3_smu *s, int setting, long v, const char *what)
{
    OverDriveTable_t *od = &s->od.OverDriveTable;
    if (!smu13_is_od_feature_supported(s, PP_OD_FEATURE_FAN_CURVE_BIT)) {
        RLOG(d, "SMU: Fan curve setting not supported");
        return RDNA3_EOPNOTSUPP;
    }
    if (!od_in_range(d, s, setting, v, what))
        return RDNA3_EINVAL;
    switch (setting) {
    case RD_OD_SET_FAN_ACOUSTIC_LIMIT:     od->AcousticLimitRpmThreshold = (uint16_t)v; break;
    case RD_OD_SET_FAN_ACOUSTIC_TARGET:    od->AcousticTargetRpmThreshold = (uint16_t)v; break;
    case RD_OD_SET_FAN_TARGET_TEMPERATURE: od->FanTargetTemperature = (uint16_t)v; break;
    case RD_OD_SET_FAN_MINIMUM_PWM:        od->FanMinimumPwm = (uint16_t)v; break;
    default: return RDNA3_EINVAL;
    }
    od->FanMode = FAN_MODE_AUTO;
    od->FeatureCtrlMask |= 1u << PP_OD_FEATURE_FAN_CURVE_BIT;
    return RDNA3_OK;
}

/* Ported from smu_v13_0_0_od_edit_dpm_table (+ v6.14's zero RPM) */
int smu13_od_edit_dpm_table(rdna3_dev *d, rdna3_smu *s, uint32_t type, const long *input,
                            uint32_t size)
{
    OverDriveTable_t *od = &s->od.OverDriveTable;
    /* offsetof(OverDriveTable_t, VoltageOffsetPerZoneBoundary) */
    const uint32_t offset_of_voltageoffset = 4;
    uint32_t feature_ctrlmask, i;
    int ret = RDNA3_OK;

    if (!s->od_valid)
        return RDNA3_EOPNOTSUPP;   /* the tables were never read */

    switch (type) {
    case RD_PP_OD_EDIT_SCLK_VDDC_TABLE:
        if (!smu13_is_od_feature_supported(s, PP_OD_FEATURE_GFXCLK_BIT)) {
            RLOG(d, "SMU: GFXCLK_LIMITS setting not supported");
            return RDNA3_EOPNOTSUPP;
        }
        for (i = 0; i < size; i += 2) {
            if (i + 2 > size) {
                RLOG(d, "SMU: invalid number of input parameters %u", size);
                return RDNA3_EINVAL;
            }
            switch (input[i]) {
            case 0:
                if (!od_in_range(d, s, RD_OD_SET_GFXCLK_FMIN, input[i + 1], "GfxclkFmin"))
                    return RDNA3_EINVAL;
                od->GfxclkFmin = (int16_t)input[i + 1];
                od->FeatureCtrlMask |= 1u << PP_OD_FEATURE_GFXCLK_BIT;
                break;
            case 1:
                if (!od_in_range(d, s, RD_OD_SET_GFXCLK_FMAX, input[i + 1], "GfxclkFmax"))
                    return RDNA3_EINVAL;
                od->GfxclkFmax = (int16_t)input[i + 1];
                od->FeatureCtrlMask |= 1u << PP_OD_FEATURE_GFXCLK_BIT;
                break;
            default:
                RLOG(d, "SMU: Invalid SCLK_VDDC_TABLE index: %ld (0:min, 1:max)", input[i]);
                return RDNA3_EINVAL;
            }
        }
        if (od->GfxclkFmin > od->GfxclkFmax) {
            RERR(d, "SMU: Invalid setting: GfxclkFmin(%u) is bigger than GfxclkFmax(%u)",
                 (uint32_t)od->GfxclkFmin, (uint32_t)od->GfxclkFmax);
            return RDNA3_EINVAL;
        }
        break;

    case RD_PP_OD_EDIT_MCLK_VDDC_TABLE:
        if (!smu13_is_od_feature_supported(s, PP_OD_FEATURE_UCLK_BIT)) {
            RLOG(d, "SMU: UCLK_LIMITS setting not supported");
            return RDNA3_EOPNOTSUPP;
        }
        for (i = 0; i < size; i += 2) {
            if (i + 2 > size) {
                RLOG(d, "SMU: invalid number of input parameters %u", size);
                return RDNA3_EINVAL;
            }
            switch (input[i]) {
            case 0:
                if (!od_in_range(d, s, RD_OD_SET_UCLK_FMIN, input[i + 1], "UclkFmin"))
                    return RDNA3_EINVAL;
                od->UclkFmin = (uint16_t)input[i + 1];
                od->FeatureCtrlMask |= 1u << PP_OD_FEATURE_UCLK_BIT;
                break;
            case 1:
                if (!od_in_range(d, s, RD_OD_SET_UCLK_FMAX, input[i + 1], "UclkFmax"))
                    return RDNA3_EINVAL;
                od->UclkFmax = (uint16_t)input[i + 1];
                od->FeatureCtrlMask |= 1u << PP_OD_FEATURE_UCLK_BIT;
                break;
            default:
                RLOG(d, "SMU: Invalid MCLK_VDDC_TABLE index: %ld (0:min, 1:max)", input[i]);
                return RDNA3_EINVAL;
            }
        }
        if (od->UclkFmin > od->UclkFmax) {
            RERR(d, "SMU: Invalid setting: UclkFmin(%u) is bigger than UclkFmax(%u)",
                 (uint32_t)od->UclkFmin, (uint32_t)od->UclkFmax);
            return RDNA3_EINVAL;
        }
        break;

    case RD_PP_OD_EDIT_VDDGFX_OFFSET:
        if (!smu13_is_od_feature_supported(s, PP_OD_FEATURE_GFX_VF_CURVE_BIT)) {
            RLOG(d, "SMU: Gfx offset setting not supported");
            return RDNA3_EOPNOTSUPP;
        }
        if (!od_in_range(d, s, RD_OD_SET_GFX_VF_CURVE, input[0], "Voltage offset"))
            return RDNA3_EINVAL;
        for (i = 0; i < PP_NUM_OD_VF_CURVE_POINTS; i++)
            od->VoltageOffsetPerZoneBoundary[i] = (int16_t)input[0];
        od->FeatureCtrlMask |= 1u << PP_OD_FEATURE_GFX_VF_CURVE_BIT;
        break;

    case RD_PP_OD_EDIT_FAN_CURVE:
        if (!smu13_is_od_feature_supported(s, PP_OD_FEATURE_FAN_CURVE_BIT)) {
            RLOG(d, "SMU: Fan curve setting not supported");
            return RDNA3_EOPNOTSUPP;
        }
        if (input[0] >= NUM_OD_FAN_MAX_POINTS - 1 || input[0] < 0)
            return RDNA3_EINVAL;
        if (!od_in_range(d, s, RD_OD_SET_FAN_CURVE_TEMP, input[1], "Fan curve temp setting") ||
            !od_in_range(d, s, RD_OD_SET_FAN_CURVE_PWM, input[2], "Fan curve pwm setting"))
            return RDNA3_EINVAL;
        od->FanLinearTempPoints[input[0]] = (uint8_t)input[1];
        od->FanLinearPwmPoints[input[0]] = (uint8_t)input[2];
        od->FanMode = FAN_MODE_MANUAL_LINEAR;
        od->FeatureCtrlMask |= 1u << PP_OD_FEATURE_FAN_CURVE_BIT;
        break;

    case RD_PP_OD_EDIT_ACOUSTIC_LIMIT:
        return od_edit_fan_value(d, s, RD_OD_SET_FAN_ACOUSTIC_LIMIT, input[0],
                                 "acoustic limit threshold setting");
    case RD_PP_OD_EDIT_ACOUSTIC_TARGET:
        return od_edit_fan_value(d, s, RD_OD_SET_FAN_ACOUSTIC_TARGET, input[0],
                                 "acoustic target threshold setting");
    case RD_PP_OD_EDIT_FAN_TARGET_TEMPERATURE:
        return od_edit_fan_value(d, s, RD_OD_SET_FAN_TARGET_TEMPERATURE, input[0],
                                 "fan target temperature setting");
    case RD_PP_OD_EDIT_FAN_MINIMUM_PWM:
        return od_edit_fan_value(d, s, RD_OD_SET_FAN_MINIMUM_PWM, input[0],
                                 "fan minimum pwm setting");

    case RD_PP_OD_EDIT_FAN_ZERO_RPM_ENABLE:
    case RD_PP_OD_EDIT_FAN_ZERO_RPM_STOP_TEMP:
        if (!smu13_is_od_feature_supported(s, PP_OD_FEATURE_ZERO_FAN_BIT)) {
            RLOG(d, "SMU: Zero RPM setting not supported");
            return RDNA3_EOPNOTSUPP;
        }
        if (type == RD_PP_OD_EDIT_FAN_ZERO_RPM_ENABLE) {
            if (!od_in_range(d, s, RD_OD_SET_FAN_ZERO_RPM_ENABLE, input[0], "zero RPM enable setting"))
                return RDNA3_EINVAL;
            od->FanZeroRpmEnable = (uint8_t)input[0];
        } else {
            if (!od_in_range(d, s, RD_OD_SET_FAN_ZERO_RPM_STOP_TEMP, input[0],
                             "zero RPM stop temperature setting"))
                return RDNA3_EINVAL;
            od->FanZeroRpmStopTemp = (uint8_t)input[0];
        }
        od->FeatureCtrlMask |= 1u << PP_OD_FEATURE_ZERO_FAN_BIT;
        break;

    case RD_PP_OD_RESTORE_DEFAULT_TABLE:
        if (size == 1) {
            ret = od_restore_table_single(d, s, input[0]);
            if (ret)
                return ret;
        } else {
            feature_ctrlmask = od->FeatureCtrlMask;
            bytes_copy(&s->od, &s->od_boot, sizeof(s->od));
            od->FeatureCtrlMask = feature_ctrlmask;
        }
        /* fall through */
    case RD_PP_OD_COMMIT_DPM_TABLE:
        /* FeatureCtrlMask tells the PMFW what this one operation is
         * about; it is not part of the user's settings, so not cached */
        if (bytes_cmp((uint8_t *)&s->od + offset_of_voltageoffset,
                      (uint8_t *)&s->od_user + offset_of_voltageoffset,
                      (uint32_t)sizeof(OverDriveTableExternal_t) - offset_of_voltageoffset)) {
            RDBG(d, "SMU: OD: Gfxclk: (%d, %d) Uclk: (%d, %d)", od->GfxclkFmin, od->GfxclkFmax,
                 od->UclkFmin, od->UclkFmax);
            ret = smu13_upload_overdrive_table(d, s, &s->od);
            if (ret) {
                RERR(d, "SMU: Failed to upload overdrive table");
                return ret;
            }
            od->FeatureCtrlMask = 0;
            bytes_copy((uint8_t *)&s->od_user + offset_of_voltageoffset,
                       (uint8_t *)&s->od + offset_of_voltageoffset,
                       (uint32_t)sizeof(OverDriveTableExternal_t) - offset_of_voltageoffset);
            s->user_od = bytes_cmp(&s->od_user, &s->od_boot, sizeof(OverDriveTableExternal_t)) != 0;
        }
        break;

    default:
        return RDNA3_EOPNOTSUPP;   /* amdgpu: -ENOSYS */
    }
    return ret;
}

/* ---- clocks / profiles ---- */

/* Ported from smu_v13_0_0_get_dpm_ultimate_freq (the DPM tables' ends) */
int smu13_get_dpm_ultimate_freq(const rdna3_smu *s, uint32_t ppclk, uint32_t *min, uint32_t *max)
{
    const rdna3_smu_dpm *t = dpm_table_of(s, ppclk);
    if (!t)
        return RDNA3_EINVAL;   /* "Unsupported clock type!" (no VCLK/DCLK table here) */
    if (min)
        *min = t->count ? t->level[0] : 0;
    if (max)
        *max = t->count ? t->level[t->count - 1] : 0;
    return RDNA3_OK;
}

/* Ported from smu_v13_0_0_force_clk_levels -> smu_v13_0_set_soft_freq_limited_range */
int smu13_force_clk_levels(rdna3_dev *d, rdna3_smu *s, uint32_t ppclk, uint32_t mask)
{
    const rdna3_smu_dpm *t = dpm_table_of(s, ppclk);
    uint32_t soft_min_level = 0, soft_max_level = 0, min_freq, max_freq, i;
    int bit;

    if (mask) {
        for (i = 0; i < 32; i++)
            if (mask & (1u << i)) { soft_min_level = i; break; }      /* ffs - 1 */
        for (i = 32; i-- > 0;)
            if (mask & (1u << i)) { soft_max_level = i; break; }      /* fls - 1 */
    }
    if (!t)
        return RDNA3_OK;      /* DCEFCLK / PCIE / others: nothing */
    if (t->fine_grained) {
        /* There is only 2 levels for fine grained DPM */
        soft_max_level = soft_max_level >= 1 ? 1 : 0;
        soft_min_level = soft_min_level >= 1 ? 1 : 0;
    } else if (soft_max_level >= t->count || soft_min_level >= t->count) {
        return RDNA3_EINVAL;
    }
    min_freq = t->level[soft_min_level];
    max_freq = t->level[soft_max_level];
    /* smu_v13_0_set_soft_freq_limited_range: nothing when the clock's DPM is off */
    bit = clk_dpm_feature(ppclk);
    if (bit < 0 || !feature_enabled(d, s, bit))
        return RDNA3_OK;
    return smu13_set_soft_freq_range(d, s, ppclk, min_freq, max_freq);
}

/* Ported from smu_v13_0_0_get_current_clk_freq_by_table (through
 * smu_v13_0_0_get_smu_metrics_data's members) */
int smu13_get_current_clk_freq_by_table(rdna3_dev *d, rdna3_smu *s, uint32_t ppclk, uint32_t *value)
{
    const SmuMetrics_t *m = &s->metrics.SmuMetrics;
    int ret = smu13_read_metrics(d, s);
    if (ret)
        return ret;
    switch (ppclk) {
    case PPCLK_GFXCLK:   /* METRICS_AVERAGE_GFXCLK */
        *value = m->AverageGfxActivity <= RD_SMU_13_0_0_BUSY_THRESHOLD ? m->AverageGfxclkFrequencyPostDs
                                                                       : m->AverageGfxclkFrequencyPreDs;
        break;
    case PPCLK_UCLK:   *value = m->CurrClock[PPCLK_UCLK]; break;     /* METRICS_CURR_UCLK */
    case PPCLK_FCLK:   *value = m->CurrClock[PPCLK_FCLK]; break;
    case PPCLK_SOCCLK: *value = m->CurrClock[PPCLK_SOCCLK]; break;
    case PPCLK_VCLK_0: *value = m->AverageVclk0Frequency; break;     /* METRICS_AVERAGE_VCLK */
    case PPCLK_DCLK_0: *value = m->AverageDclk0Frequency; break;
    case PPCLK_VCLK_1: *value = m->AverageVclk1Frequency; break;
    case PPCLK_DCLK_1: *value = m->AverageDclk1Frequency; break;
    case PPCLK_DCFCLK: *value = m->CurrClock[PPCLK_DCFCLK]; break;   /* METRICS_CURR_DCEFCLK */
    default:
        return RDNA3_EINVAL;
    }
    return RDNA3_OK;
}

/* Ported from smu_v13_0_0_populate_umd_state_clk */
void smu13_populate_umd_state_clk(rdna3_smu *s)
{
    const DriverReportedClocks_t *dc = &s->pptable.SkuTable.DriverReportedClocks;
    uint32_t gmin = 0, gmax = 0, umin = 0, umax = 0, smin = 0, smax = 0, fmin = 0, fmax = 0;

    smu13_get_dpm_ultimate_freq(s, PPCLK_GFXCLK, &gmin, &gmax);
    smu13_get_dpm_ultimate_freq(s, PPCLK_UCLK, &umin, &umax);
    smu13_get_dpm_ultimate_freq(s, PPCLK_SOCCLK, &smin, &smax);
    smu13_get_dpm_ultimate_freq(s, PPCLK_FCLK, &fmin, &fmax);

    s->pstate_gfx.min = gmin;
    s->pstate_gfx.peak = (dc->GameClockAc && dc->GameClockAc < gmax) ? dc->GameClockAc : gmax;
    s->pstate_uclk.min = umin;
    s->pstate_uclk.peak = umax;
    s->pstate_soc.min = smin;
    s->pstate_soc.peak = smax;
    s->pstate_fclk.min = fmin;
    s->pstate_fclk.peak = fmax;

    s->pstate_gfx.standard = (dc->BaseClockAc && dc->BaseClockAc < gmax) ? dc->BaseClockAc : gmax;
    s->pstate_uclk.standard = umax;
    s->pstate_soc.standard = smin;
    s->pstate_fclk.standard = fmin;
}

/* smu_v13_0_0_workload_map: PP_SMC_POWER_PROFILE_* -> WORKLOAD_PPLIB_*_BIT */
static int workload_of_profile(uint32_t profile)
{
    static const int8_t map[RD_PP_SMC_POWER_PROFILE_COUNT] = {
        WORKLOAD_PPLIB_DEFAULT_BIT, WORKLOAD_PPLIB_FULL_SCREEN_3D_BIT, WORKLOAD_PPLIB_POWER_SAVING_BIT,
        WORKLOAD_PPLIB_VIDEO_BIT, WORKLOAD_PPLIB_VR_BIT, WORKLOAD_PPLIB_COMPUTE_BIT,
        WORKLOAD_PPLIB_CUSTOM_BIT, WORKLOAD_PPLIB_WINDOW_3D_BIT, -1, -1,
    };
    return profile < RD_PP_SMC_POWER_PROFILE_COUNT ? map[profile] : -1;
}

/* Ported from smu_v13_0_0_get_power_profile_mode: one workload's
 * activity monitor coefficients (the GFXCLK and FCLK rows) */
int smu13_get_power_profile_coeff(rdna3_dev *d, rdna3_smu *s, uint32_t workload_bit,
                                  rdna3_pmctl_coeff *out)
{
    DpmActivityMonitorCoeffIntExternal_t ext;
    const DpmActivityMonitorCoeffInt_t *a = &ext.DpmActivityMonitorCoeffInt;
    int ret;

    if (workload_bit >= WORKLOAD_PPLIB_COUNT)
        return RDNA3_EINVAL;
    ret = smu13_table_read(d, s, TABLE_ACTIVITY_MONITOR_COEFF, workload_bit, &ext, sizeof(ext));
    if (ret) {
        RERR(d, "SMU: Failed to get activity monitor");
        return ret;
    }
    out->workload = workload_bit;
    out->gfx[0] = a->Gfx_FPS;
    out->gfx[1] = a->Gfx_MinActiveFreqType;
    out->gfx[2] = a->Gfx_MinActiveFreq;
    out->gfx[3] = a->Gfx_BoosterFreqType;
    out->gfx[4] = a->Gfx_BoosterFreq;
    out->gfx[5] = (int32_t)a->Gfx_PD_Data_limit_c;
    out->gfx[6] = (int32_t)a->Gfx_PD_Data_error_coeff;
    out->gfx[7] = (int32_t)a->Gfx_PD_Data_error_rate_coeff;
    out->fclk[0] = a->Fclk_FPS;
    out->fclk[1] = a->Fclk_MinActiveFreqType;
    out->fclk[2] = a->Fclk_MinActiveFreq;
    out->fclk[3] = a->Fclk_BoosterFreqType;
    out->fclk[4] = a->Fclk_BoosterFreq;
    out->fclk[5] = (int32_t)a->Fclk_PD_Data_limit_c;
    out->fclk[6] = (int32_t)a->Fclk_PD_Data_error_coeff;
    out->fclk[7] = (int32_t)a->Fclk_PD_Data_error_rate_coeff;
    return RDNA3_OK;
}

/* Ported from smu_v13_0_0_set_power_profile_mode (v6.12) */
int smu13_set_power_profile_mode(rdna3_dev *d, rdna3_smu *s, const long *input, uint32_t size)
{
    DpmActivityMonitorCoeffIntExternal_t ext;
    DpmActivityMonitorCoeffInt_t *a = &ext.DpmActivityMonitorCoeffInt;
    const rdna3_ipver *mp1 = &d->ipver[HWIP_MP1];
    uint32_t workload_mask, selected_workload_mask;
    int workload_type, ret;

    s->power_profile_mode = (uint32_t)input[size];
    if (s->power_profile_mode >= RD_PP_SMC_POWER_PROFILE_COUNT) {
        RERR(d, "SMU: Invalid power profile mode %u", s->power_profile_mode);
        return RDNA3_EINVAL;
    }

    if (s->power_profile_mode == RD_PP_SMC_POWER_PROFILE_CUSTOM) {
        if (size != 9)
            return RDNA3_EINVAL;
        ret = smu13_table_read(d, s, TABLE_ACTIVITY_MONITOR_COEFF, WORKLOAD_PPLIB_CUSTOM_BIT, &ext,
                               sizeof(ext));
        if (ret) {
            RERR(d, "SMU: Failed to get activity monitor");
            return ret;
        }
        switch (input[0]) {
        case 0: /* Gfxclk */
            a->Gfx_FPS = (uint8_t)input[1];
            a->Gfx_MinActiveFreqType = (uint8_t)input[2];
            a->Gfx_MinActiveFreq = (uint16_t)input[3];
            a->Gfx_BoosterFreqType = (uint8_t)input[4];
            a->Gfx_BoosterFreq = (uint16_t)input[5];
            a->Gfx_PD_Data_limit_c = (uint32_t)input[6];
            a->Gfx_PD_Data_error_coeff = (uint32_t)input[7];
            a->Gfx_PD_Data_error_rate_coeff = (uint32_t)input[8];
            break;
        case 1: /* Fclk */
            a->Fclk_FPS = (uint8_t)input[1];
            a->Fclk_MinActiveFreqType = (uint8_t)input[2];
            a->Fclk_MinActiveFreq = (uint16_t)input[3];
            a->Fclk_BoosterFreqType = (uint8_t)input[4];
            a->Fclk_BoosterFreq = (uint16_t)input[5];
            a->Fclk_PD_Data_limit_c = (uint32_t)input[6];
            a->Fclk_PD_Data_error_coeff = (uint32_t)input[7];
            a->Fclk_PD_Data_error_rate_coeff = (uint32_t)input[8];
            break;
        default:
            return RDNA3_EINVAL;
        }
        ret = smu13_table_write(d, s, TABLE_ACTIVITY_MONITOR_COEFF, WORKLOAD_PPLIB_CUSTOM_BIT, &ext,
                                sizeof(ext));
        if (ret) {
            RERR(d, "SMU: Failed to set activity monitor");
            return ret;
        }
    }

    workload_type = workload_of_profile(s->power_profile_mode);
    if (workload_type < 0)
        return RDNA3_EINVAL;
    selected_workload_mask = workload_mask = 1u << workload_type;

    /* "Add optimizations for SMU13.0.0/10. Reuse the power saving profile" */
    if ((mp1->major == 13 && mp1->minor == 0 && mp1->rev == 0 &&
         (s->version == 0x004e6601 || s->version >= 0x004e7300)) ||
        (mp1->major == 13 && mp1->minor == 0 && mp1->rev == 10 &&
         s->version >= RD_SMU_13_0_10_PROFILE_POWERSAVING_FW))
        workload_mask |= 1u << WORKLOAD_PPLIB_POWER_SAVING_BIT;

    ret = smu13_send_msg(d, PPSMC_MSG_SetWorkloadMask, workload_mask, NULL);
    if (!ret)
        s->workload_mask = selected_workload_mask;
    return ret;
}

/* Ported from smu_v13_0_0_get_thermal_temperature_range (smu13_thermal_policy[0]
 * with the PPTable's limits) */
int smu13_get_thermal_temperature_range(const rdna3_smu *s, rdna3_temp_range *range)
{
    const SkuTable_t *sku = &s->pptable.SkuTable;
    if (!range)
        return RDNA3_EINVAL;
    range->min = -273150;
    range->hotspot_min = -273150;
    range->mem_min = -273150;
    range->max = sku->TemperatureLimit[TEMP_EDGE] * 1000;
    range->edge_emergency_max = (sku->TemperatureLimit[TEMP_EDGE] + RD_CTF_OFFSET_EDGE) * 1000;
    range->hotspot_crit_max = sku->TemperatureLimit[TEMP_HOTSPOT] * 1000;
    range->hotspot_emergency_max = (sku->TemperatureLimit[TEMP_HOTSPOT] + RD_CTF_OFFSET_HOTSPOT) * 1000;
    range->mem_crit_max = sku->TemperatureLimit[TEMP_MEM] * 1000;
    range->mem_emergency_max = (sku->TemperatureLimit[TEMP_MEM] + RD_CTF_OFFSET_MEM) * 1000;
    range->software_shutdown_temp = s->software_shutdown_temp;
    range->software_shutdown_temp_offset = sku->FanAbnormalTempLimitOffset;
    return RDNA3_OK;
}

/* Ported from smu_v13_0_0_get_unique_id */
void smu13_get_unique_id(rdna3_dev *d, rdna3_smu *s)
{
    uint32_t upper32 = 0, lower32 = 0;
    if (smu13_read_metrics(d, s) == RDNA3_OK) {
        upper32 = s->metrics.SmuMetrics.PublicSerialNumberUpper;
        lower32 = s->metrics.SmuMetrics.PublicSerialNumberLower;
    }
    s->unique_id = ((uint64_t)upper32 << 32) | lower32;
}

/* smu_v13_0_0_throttler_map through smu_cmn_get_indep_throttler_status
 * (bits the map does not list land on 0, as there) */
uint64_t smu13_indep_throttler_status(uint32_t dep_status)
{
    static const uint8_t map[THROTTLER_COUNT] = {
        [THROTTLER_PPT0_BIT] = 0,            /* SMU_THROTTLER_PPT0_BIT */
        [THROTTLER_PPT1_BIT] = 1,
        [THROTTLER_PPT2_BIT] = 2,
        [THROTTLER_PPT3_BIT] = 3,
        [THROTTLER_TDC_GFX_BIT] = 16,        /* SMU_THROTTLER_TDC_GFX_BIT */
        [THROTTLER_TDC_SOC_BIT] = 17,
        [THROTTLER_TEMP_EDGE_BIT] = 35,      /* SMU_THROTTLER_TEMP_EDGE_BIT */
        [THROTTLER_TEMP_HOTSPOT_BIT] = 36,
        [THROTTLER_TEMP_MEM_BIT] = 34,
        [THROTTLER_TEMP_VR_GFX_BIT] = 38,
        [THROTTLER_TEMP_VR_SOC_BIT] = 39,
        [THROTTLER_TEMP_VR_MEM0_BIT] = 40,
        [THROTTLER_TEMP_VR_MEM1_BIT] = 41,
        [THROTTLER_TEMP_LIQUID0_BIT] = 42,
        [THROTTLER_TEMP_LIQUID1_BIT] = 43,
        [THROTTLER_GFX_APCC_PLUS_BIT] = 23,  /* SMU_THROTTLER_APCC_BIT */
        [THROTTLER_FIT_BIT] = 57,            /* SMU_THROTTLER_FIT_BIT */
    };
    uint64_t indep = 0;
    uint32_t bit;
    for (bit = 0; bit < THROTTLER_COUNT; bit++)
        if (dep_status & (1u << bit))
            indep |= 1ull << map[bit];
    return indep;
}

/* pcie_gen_to_speed (amdgpu_smu.c), 0.1 GT/s */
static uint32_t pcie_gen_to_speed(uint32_t gen)
{
    switch (gen) {
    case 0: case 1: return 25;
    case 2: return 50;
    case 3: return 80;
    case 4: return 160;
    default: return 320;
    }
}

/* Ported from smu_v13_0_0_get_gpu_metrics (gpu_metrics_v1_3's fields) */
int smu13_get_gpu_metrics(rdna3_dev *d, rdna3_smu *s, rdna3_gpu_metrics *g)
{
    const SmuMetrics_t *m = &s->metrics.SmuMetrics;
    int ret = smu13_read_metrics(d, s);
    if (ret)
        return ret;
    bytes_zero(g, sizeof(*g));
    g->temperature_edge = m->AvgTemperature[TEMP_EDGE];
    g->temperature_hotspot = m->AvgTemperature[TEMP_HOTSPOT];
    g->temperature_mem = m->AvgTemperature[TEMP_MEM];
    g->temperature_vrgfx = m->AvgTemperature[TEMP_VR_GFX];
    g->temperature_vrsoc = m->AvgTemperature[TEMP_VR_SOC];
    g->temperature_vrmem = m->AvgTemperature[TEMP_VR_MEM0] > m->AvgTemperature[TEMP_VR_MEM1]
                               ? m->AvgTemperature[TEMP_VR_MEM0] : m->AvgTemperature[TEMP_VR_MEM1];
    g->average_gfx_activity = m->AverageGfxActivity;
    g->average_umc_activity = m->AverageUclkActivity;
    g->average_mm_activity = m->Vcn0ActivityPercentage > m->Vcn1ActivityPercentage
                                 ? m->Vcn0ActivityPercentage : m->Vcn1ActivityPercentage;
    g->average_socket_power = m->AverageSocketPower;
    g->energy_accumulator = m->EnergyAccumulator;
    g->average_gfxclk_frequency = m->AverageGfxActivity <= RD_SMU_13_0_0_BUSY_THRESHOLD
                                      ? m->AverageGfxclkFrequencyPostDs : m->AverageGfxclkFrequencyPreDs;
    g->average_uclk_frequency = m->AverageUclkActivity <= RD_SMU_13_0_0_BUSY_THRESHOLD
                                    ? m->AverageMemclkFrequencyPostDs : m->AverageMemclkFrequencyPreDs;
    g->average_vclk0_frequency = m->AverageVclk0Frequency;
    g->average_dclk0_frequency = m->AverageDclk0Frequency;
    g->average_vclk1_frequency = m->AverageVclk1Frequency;
    g->average_dclk1_frequency = m->AverageDclk1Frequency;
    g->current_gfxclk = g->average_gfxclk_frequency;
    g->current_socclk = m->CurrClock[PPCLK_SOCCLK];
    g->current_uclk = m->CurrClock[PPCLK_UCLK];
    g->current_vclk0 = m->CurrClock[PPCLK_VCLK_0];
    g->current_dclk0 = m->CurrClock[PPCLK_DCLK_0];
    g->current_vclk1 = m->CurrClock[PPCLK_VCLK_1];
    g->current_dclk1 = m->CurrClock[PPCLK_DCLK_1];
    g->throttle_status = smu13_metric_throttlers(s);   /* smu_v13_0_get_throttler_status */
    g->indep_throttle_status = smu13_indep_throttler_status(g->throttle_status);
    g->current_fan_speed = m->AvgFanRpm;
    g->pcie_link_width = m->PcieWidth;
    if ((uint32_t)(m->PcieRate - 1) > 3)   /* (PcieRate - 1) > LINK_SPEED_MAX */
        g->pcie_link_speed = pcie_gen_to_speed(1);
    else
        g->pcie_link_speed = pcie_gen_to_speed(m->PcieRate);
    g->voltage_gfx = m->AvgVoltage[SVI_PLANE_GFX];
    g->voltage_soc = m->AvgVoltage[SVI_PLANE_SOC];
    g->voltage_mem = m->AvgVoltage[SVI_PLANE_VMEMP];
    return RDNA3_OK;
}

/* Ported from smu_v13_0_0_set_df_cstate (DFCstateControl) */
int smu13_set_df_cstate(rdna3_dev *d, rdna3_smu *s, uint32_t state)
{
    int ret = smu13_send_msg(d, PPSMC_MSG_SetExternalClientDfCstateAllow, state, NULL);
    if (!ret)
        s->df_cstate = state;
    return ret;
}

/* ---- BACO ---- */

/* Ported from smu_v13_0_baco_get_state */
int smu13_baco_get_state(const rdna3_smu *s)
{
    return s->baco_state;
}

/* Ported from smu_v13_0_baco_set_armd3_sequence */
int smu13_baco_set_armd3_sequence(rdna3_dev *d, rdna3_smu *s, uint32_t seq)
{
    int ret = smu13_send_msg(d, PPSMC_MSG_ArmD3, seq, NULL);
    if (ret)
        return ret;
    if (seq == RD_BACO_SEQ_BAMACO || seq == RD_BACO_SEQ_BACO)
        s->baco_state = RD_SMU_BACO_STATE_ENTER;
    else
        s->baco_state = RD_SMU_BACO_STATE_EXIT;
    return RDNA3_OK;
}

/* Ported from smu_v13_0_baco_set_state. 'seq' is BACO_SEQ_BAMACO for
 * amdgpu's rpm_mode AMDGPU_RUNPM_BAMACO, else BACO_SEQ_BACO. */
int smu13_baco_set_state(rdna3_dev *d, rdna3_smu *s, int state, uint32_t seq)
{
    int ret;
    if (smu13_baco_get_state(s) == state)
        return RDNA3_OK;
    if (state == RD_SMU_BACO_STATE_ENTER) {
        ret = smu13_send_msg(d, PPSMC_MSG_EnterBaco, seq, NULL);
    } else {
        ret = smu13_send_msg(d, PPSMC_MSG_ExitBaco, 0, NULL);
        if (ret)
            return ret;
        /* clear vbios scratch 6 and 7 for coming asic reinit */
        if (s->bios_scratch_reg) {
            rdna3_wreg(d, s->bios_scratch_reg + 6, 0);
            rdna3_wreg(d, s->bios_scratch_reg + 7, 0);
        }
    }
    if (!ret)
        s->baco_state = state;
    return ret;
}

/* Ported from smu_v13_0_get_bamaco_support */
int smu13_get_bamaco_support(rdna3_dev *d, rdna3_smu *s)
{
    int bamaco_support = 0;
    if (!s->baco_platform)
        return 0;
    if (s->maco_support)
        bamaco_support |= RD_MACO_SUPPORT;
    /* return true if ASIC is in BACO state already */
    if (smu13_baco_get_state(s) == RD_SMU_BACO_STATE_ENTER)
        return bamaco_support | RD_BACO_SUPPORT;
    if (feature_supported(s, FEATURE_BACO_BIT) && !feature_enabled(d, s, FEATURE_BACO_BIT))
        return 0;
    return bamaco_support | RD_BACO_SUPPORT;
}

/* Ported from smu_v13_0_baco_enter */
int smu13_baco_enter(rdna3_dev *d, rdna3_smu *s, int runpm_audio, uint32_t seq)
{
    int ret;
    if (runpm_audio)
        return smu13_baco_set_armd3_sequence(d, s, seq);
    ret = smu13_baco_set_state(d, s, RD_SMU_BACO_STATE_ENTER, seq);
    if (!ret)
        d->os.msleep(d->os.ctx, 10);   /* usleep_range(10000, 11000) */
    return ret;
}

/* Ported from smu_v13_0_baco_exit */
int smu13_baco_exit(rdna3_dev *d, rdna3_smu *s, int runpm_audio)
{
    if (runpm_audio) {
        /* Wait for PMFW handling for the Dstate change */
        d->os.msleep(d->os.ctx, 10);
        return smu13_baco_set_armd3_sequence(d, s, RD_BACO_SEQ_ULPS);
    }
    return smu13_baco_set_state(d, s, RD_SMU_BACO_STATE_EXIT, 0);
}

/* ---- userspace requests (common/rdna3_pmctl.h) ---- */

void smu13_pmctl_info(rdna3_dev *d, rdna3_smu *s, uint32_t gates, rdna3_pmctl_info *o)
{
    const OverDriveTable_t *od = &s->od.OverDriveTable;
    uint32_t i, v = 0;
    int32_t lo, hi;

    bytes_zero(o, sizeof(*o));
    o->gates = gates;
    o->pm_up = 1;
    o->fan_mode = smu13_get_fan_control_mode(d, s);    /* refreshes s->enabled */
    o->features_enabled = s->enabled;
    if (smu13_get_fan_speed_pwm(d, s, &v) == RDNA3_OK)
        o->fan_pwm = v;
    o->fan_rpm = s->metrics.SmuMetrics.AvgFanRpm;
    o->fan_max_rpm = s->pptable.SkuTable.FanMaximumRpm;
    o->no_fan = (uint32_t)s->no_fan;
    smu13_get_power_limits(d, s, &o->power_current, NULL, NULL, NULL);
    o->power_default = s->power_limit_default;
    o->power_min = s->power_limit_min;
    o->power_max = s->power_limit_max;
    o->od_enabled = (uint32_t)s->od_enabled;
    o->od_valid = (uint32_t)s->od_valid;
    o->od_feature_ctrl = s->pptable.SkuTable.OverDriveLimitsBasicMax.FeatureCtrlMask;
    o->od_ops = s->od_feature_mask;
    o->user_od = (uint32_t)s->user_od;
    o->od_ppt_pct_min = s->od_ppt_pct_min;
    o->od_ppt_pct_max = s->od_ppt_pct_max;
    for (i = 0; i < RD_OD_SET_COUNT; i++) {
        smu13_get_od_setting_limits(s, (int)i, &lo, &hi);
        o->od_lim_min[i] = lo;
        o->od_lim_max[i] = hi;
    }
    if (s->od_valid) {
        o->od_gfxclk_min = od->GfxclkFmin;
        o->od_gfxclk_max = od->GfxclkFmax;
        o->od_uclk_min = od->UclkFmin;
        o->od_uclk_max = od->UclkFmax;
        o->od_vddgfx_offset = od->VoltageOffsetPerZoneBoundary[0];
        o->od_ppt = od->Ppt;
        o->od_fan_mode = od->FanMode;
        for (i = 0; i < NUM_OD_FAN_MAX_POINTS; i++) {
            o->od_fan_curve_temp[i] = od->FanLinearTempPoints[i];
            o->od_fan_curve_pwm[i] = od->FanLinearPwmPoints[i];
        }
        o->od_acoustic_limit = od->AcousticLimitRpmThreshold;
        o->od_acoustic_target = od->AcousticTargetRpmThreshold;
        o->od_fan_target_temp = od->FanTargetTemperature;
        o->od_fan_min_pwm = od->FanMinimumPwm;
        o->od_zero_rpm_enable = od->FanZeroRpmEnable;
        o->od_zero_rpm_stop_temp = od->FanZeroRpmStopTemp;
    }
    smu13_get_dpm_ultimate_freq(s, PPCLK_GFXCLK, &o->gfxclk_min, &o->gfxclk_max);
    smu13_get_dpm_ultimate_freq(s, PPCLK_UCLK, &o->uclk_min, &o->uclk_max);
    smu13_get_dpm_ultimate_freq(s, PPCLK_SOCCLK, &o->socclk_min, &o->socclk_max);
    smu13_get_dpm_ultimate_freq(s, PPCLK_FCLK, &o->fclk_min, &o->fclk_max);
    o->gfxclk_standard = s->pstate_gfx.standard;
    o->gfxclk_peak = s->pstate_gfx.peak;
    o->uclk_standard = s->pstate_uclk.standard;
    o->uclk_peak = s->pstate_uclk.peak;
    o->gfxclk_levels = s->gfx.count;
    o->uclk_levels = s->uclk.count;
    o->socclk_levels = s->soc.count;
    o->fclk_levels = s->fclk.count;
    for (i = 0; i < 16; i++) {
        o->gfxclk_lv[i] = i < s->gfx.count ? s->gfx.level[i] : 0;
        o->uclk_lv[i] = i < s->uclk.count ? s->uclk.level[i] : 0;
        o->socclk_lv[i] = i < s->soc.count ? s->soc.level[i] : 0;
        o->fclk_lv[i] = i < s->fclk.count ? s->fclk.level[i] : 0;
    }
    smu13_get_current_clk_freq_by_table(d, s, PPCLK_GFXCLK, &o->gfxclk_cur);
    smu13_get_current_clk_freq_by_table(d, s, PPCLK_UCLK, &o->uclk_cur);
    smu13_get_current_clk_freq_by_table(d, s, PPCLK_SOCCLK, &o->socclk_cur);
    smu13_get_current_clk_freq_by_table(d, s, PPCLK_FCLK, &o->fclk_cur);
    o->power_profile_mode = s->power_profile_mode;
    o->workload_mask = s->workload_mask;
    smu13_get_thermal_temperature_range(s, &o->thermal);
    o->unique_id = s->unique_id;
    o->baco_support = (uint32_t)smu13_get_bamaco_support(d, s);
    o->baco_state = (uint32_t)s->baco_state;
    o->df_cstate = s->df_cstate;
    smu13_get_gpu_metrics(d, s, &o->metrics);
}

static uint32_t ppclk_of(int32_t pp)
{
    switch (pp) {
    case RD_PP_SCLK:   return PPCLK_GFXCLK;
    case RD_PP_MCLK:   return PPCLK_UCLK;
    case RD_PP_SOCCLK: return PPCLK_SOCCLK;
    case RD_PP_FCLK:   return PPCLK_FCLK;
    default:           return PPCLK_COUNT;
    }
}

/* the inputs an OD command reads (amdgpu's sysfs parser guarantees them) */
static uint32_t od_inputs_needed(long type)
{
    switch (type) {
    case RD_PP_OD_EDIT_FAN_CURVE:
        return 3;
    case RD_PP_OD_EDIT_VDDGFX_OFFSET:
    case RD_PP_OD_EDIT_ACOUSTIC_LIMIT:
    case RD_PP_OD_EDIT_ACOUSTIC_TARGET:
    case RD_PP_OD_EDIT_FAN_TARGET_TEMPERATURE:
    case RD_PP_OD_EDIT_FAN_MINIMUM_PWM:
    case RD_PP_OD_EDIT_FAN_ZERO_RPM_ENABLE:
    case RD_PP_OD_EDIT_FAN_ZERO_RPM_STOP_TEMP:
        return 1;
    default:
        return 0;
    }
}

int smu13_pmctl(rdna3_dev *d, rdna3_smu *s, const rdna3_pmctl_req *rq, rdna3_pmctl_reply *rp,
                uint32_t gates)
{
    long in[RD_PMCTL_MAX_ARGS + 1], p[RD_PMCTL_MAX_ARGS + 1];
    uint32_t n = rq->nargs, i;
    int ret = RDNA3_OK;

    bytes_zero(rp, sizeof(*rp));
    rp->op = rq->op;
    if (rq->abi != RDNA3_PMCTL_ABI || n > RD_PMCTL_MAX_ARGS || rq->op >= RD_PMCTL_OP_COUNT) {
        rp->err = RDNA3_EINVAL;
        return rp->err;
    }
    for (i = 0; i < RD_PMCTL_MAX_ARGS + 1; i++)
        in[i] = i < n ? rq->a[i] : 0;

    /* every request but INFO sends something new to the PMFW: it needs
     * rdna3pmctl=1 and an administrator; overdrive needs rdna3pmctl=2 */
    if (rq->op != RD_PMCTL_INFO) {
        if (!(gates & RD_PMCTL_GATE_WRITE)) {
            ret = RDNA3_EOPNOTSUPP;
            goto out;
        }
        if (!(gates & RD_PMCTL_GATE_ADMIN)) {
            ret = RDNA3_EPERM;
            goto out;
        }
    }
    if (rq->op == RD_PMCTL_OD_EDIT && !(gates & RD_PMCTL_GATE_OD)) {
        ret = RDNA3_EOPNOTSUPP;   /* amdgpu: no pp_od_clk_voltage without od_enabled */
        goto out;
    }

    switch (rq->op) {
    case RD_PMCTL_INFO:
        break;
    case RD_PMCTL_FAN_MODE:
        if (n < 1) { ret = RDNA3_EINVAL; break; }
        ret = smu13_set_fan_control_mode(d, s, (uint32_t)in[0]);
        break;
    case RD_PMCTL_FAN_PWM:
    case RD_PMCTL_FAN_RPM:
        /* amdgpu_hwmon_set_pwm1 / set_fan1_target: manual mode first */
        if (n < 1 || in[0] < 0) { ret = RDNA3_EINVAL; break; }
        if (smu13_get_fan_control_mode(d, s) != RD_FAN_CTRL_MANUAL) {
            RLOG(d, "SMU: manual fan speed control should be enabled first");
            ret = RDNA3_EINVAL;
            break;
        }
        ret = rq->op == RD_PMCTL_FAN_PWM ? smu13_set_fan_speed_pwm(d, s, (uint32_t)in[0])
                                         : smu13_set_fan_speed_rpm(d, s, (uint32_t)in[0]);
        break;
    case RD_PMCTL_POWER_LIMIT:
        if (n < 1 || in[0] < 0) { ret = RDNA3_EINVAL; break; }
        ret = smu13_set_ppt_limit(d, s, (uint32_t)in[0]);
        break;
    case RD_PMCTL_OD_EDIT:
        if (n < 1 || n - 1 < od_inputs_needed(in[0])) { ret = RDNA3_EINVAL; break; }
        ret = smu13_od_edit_dpm_table(d, s, (uint32_t)in[0], in + 1, n - 1);
        break;
    case RD_PMCTL_PROFILE:
        /* amdgpu_set_pp_power_profile_mode: parameter[size] = the mode */
        if (n < 1 || in[0] < 0) { ret = RDNA3_EINVAL; break; }
        for (i = 0; i + 1 < n; i++)
            p[i] = in[1 + i];
        p[n - 1] = in[0];
        ret = smu13_set_power_profile_mode(d, s, p, n - 1);
        break;
    case RD_PMCTL_PROFILE_COEFF:
        if (n < 1 || in[0] < 0) { ret = RDNA3_EINVAL; break; }
        ret = smu13_get_power_profile_coeff(d, s, (uint32_t)in[0], &rp->coeff);
        break;
    case RD_PMCTL_FORCE_LEVELS:
        if (n < 2 || ppclk_of(rq->a[0]) == PPCLK_COUNT) { ret = RDNA3_EINVAL; break; }
        ret = smu13_force_clk_levels(d, s, ppclk_of(rq->a[0]), (uint32_t)rq->a[1]);
        break;
    case RD_PMCTL_DF_CSTATE:
        if (n < 1 || in[0] < 0 || in[0] > 1) { ret = RDNA3_EINVAL; break; }
        ret = smu13_set_df_cstate(d, s, (uint32_t)in[0]);
        break;
    case RD_PMCTL_PERF_LEVEL:
        /* amdgpu_set_power_dpm_force_performance_level */
        if (n < 1 || in[0] <= 0) { ret = RDNA3_EINVAL; break; }
        ret = smu13_force_performance_level(d, s, (uint32_t)in[0], soc21_update_umd_stable_pstate, d);
        break;
    default:   /* RD_PMCTL_BACO_CYCLE: the OS (re-init after the exit) */
        ret = RDNA3_EOPNOTSUPP;
        break;
    }
out:
    rp->err = ret;
    smu13_pmctl_info(d, s, gates, &rp->info);
    return ret;
}

/* ---- VCN / JPEG power (declared in vcn40.h, used by its start/stop) ---- */

/* Ported from smu_v13_0_set_vcn_enable: PowerUpVcn / PowerDownVcn for every
 * instance that is not harvested, the instance in the upper half */
int smu13_set_vcn_enable(rdna3_dev *d, uint32_t num_inst, uint32_t harvest_config, int enable)
{
    uint32_t i;
    int ret = RDNA3_OK;
    for (i = 0; i < num_inst; i++) {
        if (harvest_config & (1u << i))
            continue;
        ret = smu13_send_msg(d, enable ? PPSMC_MSG_PowerUpVcn : PPSMC_MSG_PowerDownVcn, i << 16, NULL);
        if (ret)
            return ret;
    }
    return ret;
}

/* Ported from smu_v13_0_set_jpeg_enable */
int smu13_set_jpeg_enable(rdna3_dev *d, int enable)
{
    return smu13_send_msg(d, enable ? PPSMC_MSG_PowerUpJpeg : PPSMC_MSG_PowerDownJpeg, 0, NULL);
}

/* ======================================================================
 * MP1 state, allowed features, PCIe levels, performance levels and the
 * thermal / MP1 interrupts (smu_v13_0.c, smu_v13_0_0_ppt.c, amdgpu_smu.c)
 * ====================================================================== */

/* thm_13_0_2_offset.h / _sh_mask.h, mp_13_0_0_offset.h / _sh_mask.h */
#define regTHM_THERMAL_INT_ENA                        0x000a
#define regTHM_THERMAL_INT_ENA_BASE_IDX               0
#define regTHM_THERMAL_INT_CTRL                       0x000b
#define regTHM_THERMAL_INT_CTRL_BASE_IDX              0
#define THM_THERMAL_INT_ENA__THERM_INTH_CLR__SHIFT    0x3
#define THM_THERMAL_INT_ENA__THERM_INTL_CLR__SHIFT    0x4
#define THM_THERMAL_INT_ENA__THERM_TRIGGER_CLR__SHIFT 0x5
#define THM_THERMAL_INT_CTRL__DIG_THERM_INTH__SHIFT   0x0
#define THM_THERMAL_INT_CTRL__DIG_THERM_INTH_MASK     0x000000FFu
#define THM_THERMAL_INT_CTRL__DIG_THERM_INTL__SHIFT   0x8
#define THM_THERMAL_INT_CTRL__DIG_THERM_INTL_MASK     0x0000FF00u
#define THM_THERMAL_INT_CTRL__THERM_INTH_MASK__SHIFT  0x18
#define THM_THERMAL_INT_CTRL__THERM_INTH_MASK_MASK    0x01000000u
#define THM_THERMAL_INT_CTRL__THERM_INTL_MASK__SHIFT  0x19
#define THM_THERMAL_INT_CTRL__THERM_INTL_MASK_MASK    0x02000000u
#define THM_THERMAL_INT_CTRL__THERM_TRIGGER_MASK_MASK 0x04000000u
#define THM_THERMAL_INT_CTRL__THERM_IH_HW_ENA__SHIFT  0x1c
#define THM_THERMAL_INT_CTRL__THERM_IH_HW_ENA_MASK    0x10000000u
#define THM_THERMAL_INT_CTRL__MAX_IH_CREDIT__SHIFT    0x1d
#define THM_THERMAL_INT_CTRL__MAX_IH_CREDIT_MASK      0xE0000000u
#define regMP1_SMN_IH_SW_INT                          0x02c2
#define regMP1_SMN_IH_SW_INT_BASE_IDX                 0
#define regMP1_SMN_IH_SW_INT_CTRL                     0x02c3
#define regMP1_SMN_IH_SW_INT_CTRL_BASE_IDX            0
#define MP1_SMN_IH_SW_INT__ID__SHIFT                  0x0
#define MP1_SMN_IH_SW_INT__ID_MASK                    0x000000FFu
#define MP1_SMN_IH_SW_INT__VALID__SHIFT               0x8
#define MP1_SMN_IH_SW_INT__VALID_MASK                 0x00000100u
#define MP1_SMN_IH_SW_INT_CTRL__INT_MASK__SHIFT       0x0
#define MP1_SMN_IH_SW_INT_CTRL__INT_MASK_MASK         0x00000001u
#define MP1_SMN_IH_SW_INT_CTRL__INT_ACK__SHIFT        0x8
#define MP1_SMN_IH_SW_INT_CTRL__INT_ACK_MASK          0x00000100u
#define MP1_REG(d, reg) rdna3_reg_offset((d), HWIP_MP1, 0, reg##_BASE_IDX, reg)
#define SMU_THERMAL_MINIMUM_ALERT_TEMP 0
#define SMU_THERMAL_MAXIMUM_ALERT_TEMP 255

int smu13_disable_pmfw_state(rdna3_dev *d)
{
    rdna3_smn_wreg(d, RD_SMN_MP1_FIRMWARE_FLAGS, 0);
    return rdna3_smn_rreg(d, RD_SMN_MP1_FIRMWARE_FLAGS) == 0 ? RDNA3_OK : RDNA3_EINVAL;
}

int smu13_set_mp1_state(rdna3_dev *d, rdna3_smu *s, int mp1_state)
{
    int ret;
    switch (mp1_state) {
    case RD_PP_MP1_STATE_UNLOAD:
        ret = smu13_send_msg(d, PPSMC_MSG_PrepareMp1ForUnload, 0x55, NULL);
        if (!ret && s && s->baco_state == RD_SMU_BACO_STATE_EXIT)
            ret = smu13_disable_pmfw_state(d);
        break;
    default:   /* Ignore others */
        ret = RDNA3_OK;
    }
    return ret;
}

uint64_t smu13_get_allowed_feature_mask(uint32_t smc_fw_version, uint32_t pp_feature,
                                        int athub_mmhub_pg)
{
    uint64_t m = ~0ull;
    if (!(pp_feature & RD_PP_SCLK_DPM_MASK))
        m &= ~(RD_SMU_FEATURE(FEATURE_DPM_GFXCLK_BIT) | RD_SMU_FEATURE(FEATURE_GFX_IMU_BIT));
    if (!athub_mmhub_pg)
        m &= ~RD_SMU_FEATURE(FEATURE_ATHUB_MMHUB_PG_BIT);
    if (!(pp_feature & RD_PP_SOCCLK_DPM_MASK))
        m &= ~RD_SMU_FEATURE(FEATURE_DPM_SOCCLK_BIT);
    /* PMFW 78.58 contains a critical fix for gfxoff feature */
    if (smc_fw_version < 0x004e3a00u || !(pp_feature & RD_PP_GFXOFF_MASK))
        m &= ~RD_SMU_FEATURE(FEATURE_GFXOFF_BIT);
    if (!(pp_feature & RD_PP_MCLK_DPM_MASK))
        m &= ~(RD_SMU_FEATURE(FEATURE_DPM_UCLK_BIT) | RD_SMU_FEATURE(FEATURE_VMEMP_SCALING_BIT) |
               RD_SMU_FEATURE(FEATURE_VDDIO_MEM_SCALING_BIT));
    if (!(pp_feature & RD_PP_SCLK_DEEP_SLEEP_MASK))
        m &= ~RD_SMU_FEATURE(FEATURE_DS_GFXCLK_BIT);
    if (!(pp_feature & RD_PP_PCIE_DPM_MASK))
        m &= ~(RD_SMU_FEATURE(FEATURE_DPM_LINK_BIT) | RD_SMU_FEATURE(FEATURE_DS_LCLK_BIT));
    if (!(pp_feature & RD_PP_ULV_MASK))
        m &= ~RD_SMU_FEATURE(FEATURE_GFX_ULV_BIT);
    return m;
}

int smu13_is_mode1_reset_supported(const rdna3_smu *s)
{
    /* PMFW support is available since 78.41 */
    return s->version >= 0x004e2900u;
}

/* Ported from smu_v13_0_set_power_source (CMN2ASIC_MAPPING_PWR: AC 0, DC 1) */
int smu13_set_power_source(rdna3_dev *d, int dc)
{
    return smu13_send_msg(d, PPSMC_MSG_NotifyPowerSource, dc ? POWER_SOURCE_DC : POWER_SOURCE_AC, NULL);
}

int smu13_notify_display_change(rdna3_dev *d, int dc_present)
{
    if (!dc_present)
        return smu13_send_msg(d, PPSMC_MSG_DALNotPresent, 0, NULL);
    return RDNA3_OK;
}

/* smu_v13_0_0_set_default_dpm_table's pcie_table: the SkuTable's link
 * levels without the all-zero ones */
static void pcie_table_init(rdna3_smu *s)
{
    const SkuTable_t *sku = &s->pptable.SkuTable;
    uint32_t i, n = 0;
    for (i = 0; i < NUM_LINK_LEVELS; i++) {
        if (!sku->PcieGenSpeed[i] && !sku->PcieLaneCount[i] && !sku->LclkFreq[i])
            continue;
        s->pcie_gen[n] = sku->PcieGenSpeed[i];
        s->pcie_lane[n] = sku->PcieLaneCount[i];
        n++;
    }
    s->pcie_levels = n;
}

int smu13_update_pcie_parameters(rdna3_dev *d, rdna3_smu *s, uint32_t pcie_gen_cap,
                                 uint32_t pcie_width_cap)
{
    uint32_t i, n = s->pcie_levels;
    int ret;
    for (i = 0; i < n; i++) {
        if (s->pcie_gen[i] > pcie_gen_cap)
            s->pcie_gen[i] = pcie_gen_cap;
        if (s->pcie_lane[i] > pcie_width_cap)
            s->pcie_lane[i] = pcie_width_cap;
    }
    for (i = 0; i < n; i++) {
        uint32_t arg = (i << 16) | (s->pcie_gen[i] << 8) | s->pcie_lane[i];
        ret = smu13_send_msg(d, PPSMC_MSG_OverridePcieParameters, arg, NULL);
        if (ret)
            return ret;
    }
    return RDNA3_OK;
}

/* the deep sleep features the 13.0.0 feature map has (no DS_MP0CLK /
 * DS_MP1CLK there) */
/* Ported from smu_v13_0_deep_sleep_control */
int smu13_deep_sleep_control(rdna3_dev *d, rdna3_smu *s, int enable)
{
    static const int ds[] = { FEATURE_DS_GFXCLK_BIT, FEATURE_DS_UCLK_BIT, FEATURE_DS_FCLK_BIT,
                              FEATURE_DS_SOCCLK_BIT, FEATURE_DS_LCLK_BIT, FEATURE_DS_VCN_BIT };
    unsigned i;
    int ret = RDNA3_OK;
    for (i = 0; i < sizeof(ds) / sizeof(ds[0]); i++) {
        if (!feature_supported(s, ds[i]))
            continue;
        ret = feature_set_enabled(d, s, ds[i], enable);
        if (ret) {
            RERR(d, "SMU: Failed to %s deep sleep feature %d", enable ? "enable" : "disable", ds[i]);
            return ret;
        }
    }
    return ret;
}

/* Ported from smu_v13_0_gfx_ulv_control */
int smu13_gfx_ulv_control(rdna3_dev *d, rdna3_smu *s, int enable)
{
    if (feature_supported(s, FEATURE_GFX_ULV_BIT))
        return feature_set_enabled(d, s, FEATURE_GFX_ULV_BIT, enable);
    return RDNA3_OK;
}

/* Ported from smu_v13_0_gpo_control */
int smu13_gpo_control(rdna3_dev *d, int enable)
{
    int ret = smu13_send_msg(d, PPSMC_MSG_SetGpoAllow, enable ? 1 : 0, NULL);
    if (ret)
        RERR(d, "SMU: SetGpoAllow %d failed!", enable);
    return ret;
}

/* smu_v13_0_set_soft_freq_limited_range: nothing for a clock whose DPM is
 * off (smu_cmn_clk_dpm_is_enabled) */
static int soft_range(rdna3_dev *d, rdna3_smu *s, uint32_t ppclk, int bit, uint32_t min, uint32_t max)
{
    if (!feature_enabled(d, s, bit))
        return RDNA3_OK;
    return smu13_set_soft_freq_range(d, s, ppclk, min, max);
}

/* smu_v13_0_0_set_default_dpm_table's VCLK / DCLK tables (MM_DPM) and the
 * values smu_v13_0_0_populate_umd_state_clk gives their pstates */
static int vdclk_tables(rdna3_dev *d, rdna3_smu *s)
{
    int ret;
    if (s->vdclk_valid)
        return RDNA3_OK;
    if (feature_enabled(d, s, FEATURE_MM_DPM_BIT)) {
        ret = smu13_get_dpm_table(d, PPCLK_VCLK_0, &s->vclk);
        if (!ret)
            ret = smu13_get_dpm_table(d, PPCLK_DCLK_0, &s->dclk);
        if (ret)
            return ret;
    } else {
        s->vclk.count = s->dclk.count = 1;
        s->vclk.level[0] = s->boot.vclk / 100;
        s->dclk.level[0] = s->boot.dclk / 100;
    }
    s->pstate_vclk.min = s->vclk.level[0];
    s->pstate_vclk.peak = s->vclk.level[s->vclk.count ? s->vclk.count - 1 : 0];
    s->pstate_vclk.standard = s->vclk.level[0];
    s->pstate_dclk.min = s->dclk.level[0];
    s->pstate_dclk.peak = s->dclk.level[s->dclk.count ? s->dclk.count - 1 : 0];
    s->pstate_dclk.standard = s->dclk.level[0];
    s->vdclk_valid = 1;
    return RDNA3_OK;
}

static uint32_t tmin(const rdna3_smu_dpm *t) { return t->count ? t->level[0] : 0; }
static uint32_t tmax(const rdna3_smu_dpm *t) { return t->count ? t->level[t->count - 1] : 0; }

/* two VCN instances, none harvested on Navi 32 (vcn_unified_0 / _1) */
#define RD_SMU_NUM_VCN_INST 2

int smu13_set_performance_level(rdna3_dev *d, rdna3_smu *s, uint32_t level)
{
    uint32_t smin = 0, smax = 0, mmin = 0, mmax = 0, socmin = 0, socmax = 0;
    uint32_t vmin = 0, vmax = 0, dmin = 0, dmax = 0, fmin = 0, fmax = 0, i;
    int ret = vdclk_tables(d, s);
    if (ret)
        return ret;
    smu13_populate_umd_state_clk(s);
    switch (level) {
    case RD_DPM_FORCED_LEVEL_HIGH:
        smin = smax = tmax(&s->gfx); mmin = mmax = tmax(&s->uclk); socmin = socmax = tmax(&s->soc);
        vmin = vmax = tmax(&s->vclk); dmin = dmax = tmax(&s->dclk); fmin = fmax = tmax(&s->fclk);
        break;
    case RD_DPM_FORCED_LEVEL_LOW:
        smin = smax = tmin(&s->gfx); mmin = mmax = tmin(&s->uclk); socmin = socmax = tmin(&s->soc);
        vmin = vmax = tmin(&s->vclk); dmin = dmax = tmin(&s->dclk); fmin = fmax = tmin(&s->fclk);
        break;
    case RD_DPM_FORCED_LEVEL_AUTO:
        smin = tmin(&s->gfx); smax = tmax(&s->gfx); mmin = tmin(&s->uclk); mmax = tmax(&s->uclk);
        socmin = tmin(&s->soc); socmax = tmax(&s->soc); vmin = tmin(&s->vclk); vmax = tmax(&s->vclk);
        dmin = tmin(&s->dclk); dmax = tmax(&s->dclk); fmin = tmin(&s->fclk); fmax = tmax(&s->fclk);
        break;
    case RD_DPM_FORCED_LEVEL_PROFILE_STANDARD:
        smin = smax = s->pstate_gfx.standard; mmin = mmax = s->pstate_uclk.standard;
        socmin = socmax = s->pstate_soc.standard; vmin = vmax = s->pstate_vclk.standard;
        dmin = dmax = s->pstate_dclk.standard; fmin = fmax = s->pstate_fclk.standard;
        break;
    case RD_DPM_FORCED_LEVEL_PROFILE_MIN_SCLK:
        smin = smax = s->pstate_gfx.min;
        break;
    case RD_DPM_FORCED_LEVEL_PROFILE_MIN_MCLK:
        mmin = mmax = s->pstate_uclk.min;
        break;
    case RD_DPM_FORCED_LEVEL_PROFILE_PEAK:
        smin = smax = s->pstate_gfx.peak; mmin = mmax = s->pstate_uclk.peak;
        socmin = socmax = s->pstate_soc.peak; vmin = vmax = s->pstate_vclk.peak;
        dmin = dmax = s->pstate_dclk.peak; fmin = fmax = s->pstate_fclk.peak;
        break;
    case RD_DPM_FORCED_LEVEL_MANUAL:
    case RD_DPM_FORCED_LEVEL_PROFILE_EXIT:
        return RDNA3_OK;
    default:
        RERR(d, "SMU: Invalid performance level %u", level);
        return RDNA3_EINVAL;
    }
    if (smin && smax && (ret = soft_range(d, s, PPCLK_GFXCLK, FEATURE_DPM_GFXCLK_BIT, smin, smax)))
        return ret;
    if (mmin && mmax && (ret = soft_range(d, s, PPCLK_UCLK, FEATURE_DPM_UCLK_BIT, mmin, mmax)))
        return ret;
    if (socmin && socmax && (ret = soft_range(d, s, PPCLK_SOCCLK, FEATURE_DPM_SOCCLK_BIT, socmin, socmax)))
        return ret;
    for (i = 0; vmin && vmax && i < RD_SMU_NUM_VCN_INST; i++)
        if ((ret = soft_range(d, s, i ? PPCLK_VCLK_1 : PPCLK_VCLK_0, FEATURE_MM_DPM_BIT, vmin, vmax)))
            return ret;
    for (i = 0; dmin && dmax && i < RD_SMU_NUM_VCN_INST; i++)
        if ((ret = soft_range(d, s, i ? PPCLK_DCLK_1 : PPCLK_DCLK_0, FEATURE_MM_DPM_BIT, dmin, dmax)))
            return ret;
    if (fmin && fmax && (ret = soft_range(d, s, PPCLK_FCLK, FEATURE_DPM_FCLK_BIT, fmin, fmax)))
        return ret;
    return RDNA3_OK;
}

#define RD_UMD_PROFILE_MASK (RD_DPM_FORCED_LEVEL_PROFILE_STANDARD | RD_DPM_FORCED_LEVEL_PROFILE_MIN_SCLK | \
                             RD_DPM_FORCED_LEVEL_PROFILE_MIN_MCLK | RD_DPM_FORCED_LEVEL_PROFILE_PEAK)

/* Ported from smu_enable_umd_pstate */
static void smu_enable_umd_pstate(rdna3_dev *d, rdna3_smu *s, uint32_t *level,
                                  rdna3_umd_pstate_fn umd_pstate, void *ctx)
{
    if (!(s->dpm_level & RD_UMD_PROFILE_MASK)) {
        /* enter umd pstate, save current level, disable gfx cg */
        if (*level & RD_UMD_PROFILE_MASK) {
            s->saved_dpm_level = s->dpm_level;
            smu13_gpo_control(d, 0);
            smu13_gfx_ulv_control(d, s, 0);
            smu13_deep_sleep_control(d, s, 0);
            if (umd_pstate)
                umd_pstate(ctx, 1);
        }
    } else if (!(*level & RD_UMD_PROFILE_MASK)) {
        /* exit umd pstate, restore level, enable gfx cg */
        if (*level == RD_DPM_FORCED_LEVEL_PROFILE_EXIT)
            *level = s->saved_dpm_level;
        if (umd_pstate)
            umd_pstate(ctx, 0);
        smu13_deep_sleep_control(d, s, 1);
        smu13_gfx_ulv_control(d, s, 1);
        smu13_gpo_control(d, 1);
    }
}

/* Ported from smu_adjust_power_state_dynamic (no display config / clock
 * adjust callbacks on 13.0.0); workload_setting[i] is profile i */
static int smu_adjust_power_state_dynamic(rdna3_dev *d, rdna3_smu *s, uint32_t level)
{
    int ret = RDNA3_OK, index = 0;
    if (s->dpm_level != level) {
        ret = smu13_set_performance_level(d, s, level);
        if (ret) {
            RERR(d, "SMU: Failed to set performance level!");
            return ret;
        }
        s->dpm_level = level;   /* update the saved copy */
    }
    if (s->dpm_level != RD_DPM_FORCED_LEVEL_MANUAL &&
        s->dpm_level != RD_DPM_FORCED_LEVEL_PERF_DETERMINISM) {
        uint32_t m = s->workload_mask;
        long workload;
        while (m) {             /* fls */
            index++;
            m >>= 1;
        }
        index = index > 0 && index <= 7 ? index - 1 : 0;   /* WORKLOAD_POLICY_MAX */
        workload = index;
        if (s->power_profile_mode != (uint32_t)workload)
            smu13_set_power_profile_mode(d, s, &workload, 0);   /* smu_bump_power_profile_mode */
    }
    return ret;
}

/* Ported from smu_force_performance_level */
int smu13_force_performance_level(rdna3_dev *d, rdna3_smu *s, uint32_t level,
                                  rdna3_umd_pstate_fn umd_pstate, void *ctx)
{
    if (!s->dpm_running)
        return RDNA3_EOPNOTSUPP;
    if (!s->dpm_level)
        s->dpm_level = RD_DPM_FORCED_LEVEL_AUTO;   /* smu_dpm_ctx->dpm_level after smu_sw_init */
    smu_enable_umd_pstate(d, s, &level, umd_pstate, ctx);
    /* smu_handle_task(AMD_PP_TASK_READJUST_POWER_STATE) */
    return smu_adjust_power_state_dynamic(d, s, level);
}

int smu13_set_irq_state(rdna3_dev *d, rdna3_smu *s, int enable)
{
    uint32_t val, low, high;
    if (!enable) {
        val = rdna3_rreg(d, THM_REG(d, regTHM_THERMAL_INT_CTRL));
        val = RD_FIELD_SET(val, THM_THERMAL_INT_CTRL, THERM_INTH_MASK, 1);
        val = RD_FIELD_SET(val, THM_THERMAL_INT_CTRL, THERM_INTL_MASK, 1);
        rdna3_wreg(d, THM_REG(d, regTHM_THERMAL_INT_CTRL), val);
        rdna3_wreg(d, THM_REG(d, regTHM_THERMAL_INT_ENA), 0);
        val = rdna3_rreg(d, MP1_REG(d, regMP1_SMN_IH_SW_INT_CTRL));
        val = RD_FIELD_SET(val, MP1_SMN_IH_SW_INT_CTRL, INT_MASK, 1);
        rdna3_wreg(d, MP1_REG(d, regMP1_SMN_IH_SW_INT_CTRL), val);
        s->irq_enabled = 0;
        return RDNA3_OK;
    }
    /* smu->thermal_range: min_temp from smu_v13_0_0_get_thermal_temperature_range
     * (-273150 m°C) becomes SMU_THERMAL_MINIMUM_ALERT_TEMP, max is the
     * software shutdown temperature */
    low = SMU_THERMAL_MINIMUM_ALERT_TEMP;
    high = s->software_shutdown_temp < SMU_THERMAL_MAXIMUM_ALERT_TEMP ? s->software_shutdown_temp
                                                                     : SMU_THERMAL_MAXIMUM_ALERT_TEMP;
    val = rdna3_rreg(d, THM_REG(d, regTHM_THERMAL_INT_CTRL));
    val = RD_FIELD_SET(val, THM_THERMAL_INT_CTRL, MAX_IH_CREDIT, 5);
    val = RD_FIELD_SET(val, THM_THERMAL_INT_CTRL, THERM_IH_HW_ENA, 1);
    val = RD_FIELD_SET(val, THM_THERMAL_INT_CTRL, THERM_INTH_MASK, 0);
    val = RD_FIELD_SET(val, THM_THERMAL_INT_CTRL, THERM_INTL_MASK, 0);
    val = RD_FIELD_SET(val, THM_THERMAL_INT_CTRL, DIG_THERM_INTH, high & 0xff);
    val = RD_FIELD_SET(val, THM_THERMAL_INT_CTRL, DIG_THERM_INTL, low & 0xff);
    val &= ~THM_THERMAL_INT_CTRL__THERM_TRIGGER_MASK_MASK;
    rdna3_wreg(d, THM_REG(d, regTHM_THERMAL_INT_CTRL), val);
    val = (1u << THM_THERMAL_INT_ENA__THERM_INTH_CLR__SHIFT) |
          (1u << THM_THERMAL_INT_ENA__THERM_INTL_CLR__SHIFT) |
          (1u << THM_THERMAL_INT_ENA__THERM_TRIGGER_CLR__SHIFT);
    rdna3_wreg(d, THM_REG(d, regTHM_THERMAL_INT_ENA), val);
    val = rdna3_rreg(d, MP1_REG(d, regMP1_SMN_IH_SW_INT));
    val = RD_FIELD_SET(val, MP1_SMN_IH_SW_INT, ID, 0xFE);
    val = RD_FIELD_SET(val, MP1_SMN_IH_SW_INT, VALID, 0);
    rdna3_wreg(d, MP1_REG(d, regMP1_SMN_IH_SW_INT), val);
    val = rdna3_rreg(d, MP1_REG(d, regMP1_SMN_IH_SW_INT_CTRL));
    val = RD_FIELD_SET(val, MP1_SMN_IH_SW_INT_CTRL, INT_MASK, 0);
    rdna3_wreg(d, MP1_REG(d, regMP1_SMN_IH_SW_INT_CTRL), val);
    s->irq_enabled = 1;
    return RDNA3_OK;
}

static int smu_v13_0_allow_ih_interrupt(rdna3_dev *d)
{
    return smu13_send_msg(d, PPSMC_MSG_AllowIHHostInterrupt, 0, NULL);
}

static int smu_v13_0_process_pending_interrupt(rdna3_dev *d, rdna3_smu *s)
{
    if (s->dc_controlled_by_gpio && feature_enabled(d, s, FEATURE_ACDC_BIT))
        return smu_v13_0_allow_ih_interrupt(d);
    return RDNA3_OK;
}

/* amdgpu_irq_get(irq_source) -> smu_v13_0_set_irq_state(ENABLE) */
int smu13_enable_thermal_alert(rdna3_dev *d, rdna3_smu *s)
{
    int ret = smu13_set_irq_state(d, s, 1);
    if (ret)
        return ret;
    return smu_v13_0_process_pending_interrupt(d, s);
}

/* Ported from smu_v13_0_disable_thermal_alert */
int smu13_disable_thermal_alert(rdna3_dev *d, rdna3_smu *s)
{
    return smu13_set_irq_state(d, s, 0);
}

int smu13_iv_is_smu(uint32_t client_id, uint32_t src_id)
{
    return (client_id == RD_IH_CLIENT_THM &&
            (src_id == RD_THM_SRCID_DIG_THERM_L2H || src_id == RD_THM_SRCID_DIG_THERM_H2L)) ||
           (client_id == RD_IH_CLIENT_ROM_SMUIO && src_id == RD_SMUIO_SRCID_GPIO19) ||
           (client_id == RD_IH_CLIENT_MP1 && src_id == RD_SMU_IH_INTERRUPT_ID_TO_DRIVER);
}

static int smu_v13_0_ack_ac_dc_interrupt(rdna3_dev *d)
{
    return smu13_send_msg(d, PPSMC_MSG_ReenableAcDcInterrupt, 0, NULL);
}

/* the soft CTF limit written back to DIG_THERM_INTH (FAN_ABNORMAL /
 * FAN_RECOVERY) */
static void thm_set_high(rdna3_dev *d, uint32_t high)
{
    uint32_t data = rdna3_rreg(d, THM_REG(d, regTHM_THERMAL_INT_CTRL));
    data = RD_FIELD_SET(data, THM_THERMAL_INT_CTRL, DIG_THERM_INTH, high & 0xff);
    data &= ~THM_THERMAL_INT_CTRL__THERM_TRIGGER_MASK_MASK;
    rdna3_wreg(d, THM_REG(d, regTHM_THERMAL_INT_CTRL), data);
}

int smu13_irq_process(rdna3_dev *d, rdna3_smu *s, uint32_t client_id, uint32_t src_id,
                      uint32_t ctxid)
{
    uint32_t data;
    int32_t high;
    if (client_id == RD_IH_CLIENT_THM) {
        switch (src_id) {
        case RD_THM_SRCID_DIG_THERM_L2H:
            return RD_SMU_IRQ_SWCTF;   /* schedule_delayed_work(swctf_delayed_work, 50 ms) */
        case RD_THM_SRCID_DIG_THERM_H2L:
            RERR(d, "ERROR: GPU under temperature range detected");
            break;
        default:
            RERR(d, "ERROR: GPU under temperature range unknown src id (%u)", src_id);
            break;
        }
        return RD_SMU_IRQ_UNDER;
    }
    if (client_id == RD_IH_CLIENT_ROM_SMUIO) {
        RERR(d, "ERROR: GPU HW Critical Temperature Fault(aka CTF) detected!");
        /* SW CTF just occurred; try to orderly shut the system down */
        RERR(d, "ERROR: System is going to shutdown due to GPU HW CTF!");
        return RD_SMU_IRQ_HWCTF;
    }
    if (client_id != RD_IH_CLIENT_MP1 || src_id != RD_SMU_IH_INTERRUPT_ID_TO_DRIVER)
        return RD_SMU_IRQ_NONE;
    /* ACK SMUToHost interrupt */
    data = rdna3_rreg(d, MP1_REG(d, regMP1_SMN_IH_SW_INT_CTRL));
    data = RD_FIELD_SET(data, MP1_SMN_IH_SW_INT_CTRL, INT_ACK, 1);
    rdna3_wreg(d, MP1_REG(d, regMP1_SMN_IH_SW_INT_CTRL), data);
    switch (ctxid) {
    case RD_SMU_IH_CTX_AC:
        RDBG(d, "SMU: Switched to AC mode!");
        smu_v13_0_ack_ac_dc_interrupt(d);
        s->ac_power = 1;
        break;
    case RD_SMU_IH_CTX_DC:
        RDBG(d, "SMU: Switched to DC mode!");
        smu_v13_0_ack_ac_dc_interrupt(d);
        s->ac_power = 0;
        break;
    case RD_SMU_IH_CTX_THERMAL_THROTTLING:
        /* the throttler status is logged from the metrics by the OS */
        s->throttle_int_counter++;
        return RD_SMU_IRQ_THROTTLE;
    case RD_SMU_IH_CTX_FAN_ABNORMAL:
        high = (int32_t)s->software_shutdown_temp + s->pptable.SkuTable.FanAbnormalTempLimitOffset;
        if (high > SMU_THERMAL_MAXIMUM_ALERT_TEMP)
            high = SMU_THERMAL_MAXIMUM_ALERT_TEMP;
        RERR(d, "Reduce soft CTF limit to %d (by an offset %d)", high,
             (int)s->pptable.SkuTable.FanAbnormalTempLimitOffset);
        thm_set_high(d, (uint32_t)high);
        break;
    case RD_SMU_IH_CTX_FAN_RECOVERY:
        high = s->software_shutdown_temp < SMU_THERMAL_MAXIMUM_ALERT_TEMP ? s->software_shutdown_temp
                                                                         : SMU_THERMAL_MAXIMUM_ALERT_TEMP;
        RERR(d, "Recover soft CTF limit to %d", high);
        thm_set_high(d, (uint32_t)high);
        break;
    default:
        RDBG(d, "SMU: Unhandled context id %u from client:%u!", ctxid, client_id);
        break;
    }
    return RD_SMU_IRQ_NONE;
}

/* Ported from smu_swctf_delayed_work_handler (the hotspot through
 * smu_v13_0_0_read_sensor AMDGPU_PP_SENSOR_HOTSPOT_TEMP, the limit
 * smu->thermal_range.software_shutdown_temp) */
int smu13_swctf_check(rdna3_dev *d, rdna3_smu *s, uint32_t *hotspot_c)
{
    uint32_t t = 0;
    int ok = smu13_read_metrics(d, s) == RDNA3_OK;
    if (ok)
        t = smu13_metric_temp_hotspot_c(s);
    if (hotspot_c)
        *hotspot_c = t;
    if (ok && t < s->software_shutdown_temp)
        return 0;
    RERR(d, "ERROR: GPU over temperature range(SW CTF) detected!");
    RERR(d, "ERROR: System is going to shutdown due to GPU SW CTF!");
    return 1;
}
