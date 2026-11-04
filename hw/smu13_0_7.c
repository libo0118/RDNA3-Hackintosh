/* SMU 13.0.7 PM table/metrics port from Linux v6.12 smu_v13_0_7_ppt.c
 * and smu_cmn.c (MIT). The 13.0.0 ABI is never visible in this unit. */
#include "smu13_0_7.h"
#include "gen/smu13_driver_if_v13_0_7.h"
#include "gen/smu_v13_0_7_ppsmc.h"
#include <stddef.h>

/* atomfirmware.h's common header; the powerplay header below is verbatim. */
struct atom_common_table_header {
    uint16_t structuresize;
    uint8_t format_revision, content_revision;
};
#include "gen/smu_v13_0_7_pptable.h"

/* Host and kext compile these assertions against the real vendor types. */
#define ABI_ASSERT(name, expr) typedef char abi7_##name[(expr) ? 1 : -1]
ABI_ASSERT(driver_if, SMU13_0_7_DRIVER_IF_VERSION == RD_SMU_13_0_7_IF_VERSION);
ABI_ASSERT(pptable_version, PPTABLE_VERSION == 0x27);
ABI_ASSERT(sku_size, sizeof(SkuTable_t) == 3484);
ABI_ASSERT(board_size, sizeof(BoardTable_t) == 488);
ABI_ASSERT(board_prefix_size, offsetof(BoardTable_t, MmHubPadding) == 456);
ABI_ASSERT(pp_size, sizeof(PPTable_t) == 3972);
ABI_ASSERT(board_offset, offsetof(PPTable_t, BoardTable) == 3484);
ABI_ASSERT(sku_temp, offsetof(SkuTable_t, TemperatureLimit) == 50);
ABI_ASSERT(sku_pcie, offsetof(SkuTable_t, PcieGenSpeed) == 1232 &&
    offsetof(SkuTable_t, PcieLaneCount) == 1235 && offsetof(SkuTable_t, LclkFreq) == 1238);
ABI_ASSERT(sku_fan, offsetof(SkuTable_t, FanMaximumRpm) == 1330 &&
    offsetof(SkuTable_t, FanAbnormalTempLimitOffset) == 1412);
ABI_ASSERT(sku_clocks, offsetof(SkuTable_t, DriverReportedClocks) == 2780);
ABI_ASSERT(sku_power, offsetof(SkuTable_t, MsgLimits) == 2808);
ABI_ASSERT(board_maco, offsetof(BoardTable_t, HsrEnabled) == 188 &&
    offsetof(BoardTable_t, VddqOffEnabled) == 189);
ABI_ASSERT(combo_offset, offsetof(struct smu_13_0_7_powerplay_table, smc_pptable) == 832);
ABI_ASSERT(combo_size, sizeof(struct smu_13_0_7_powerplay_table) == 4804);
ABI_ASSERT(combo_table_size_offset, offsetof(struct smu_13_0_7_powerplay_table, table_size) == 6);
ABI_ASSERT(combo_caps_offset, offsetof(struct smu_13_0_7_powerplay_table, platform_caps) == 18);
ABI_ASSERT(combo_shutdown_offset, offsetof(struct smu_13_0_7_powerplay_table, software_shutdown_temp) == 29);
ABI_ASSERT(metrics_size, sizeof(SmuMetricsExternal_t) == 392);
ABI_ASSERT(metrics_core_size, sizeof(SmuMetrics_t) == 240);
ABI_ASSERT(metrics_serial_low, offsetof(SmuMetrics_t, PublicSerialNumberLower) == 232);
ABI_ASSERT(metrics_serial_high, offsetof(SmuMetrics_t, PublicSerialNumberUpper) == 236);
ABI_ASSERT(metrics_power, offsetof(SmuMetrics_t, AverageSocketPower) == 116);
ABI_ASSERT(metrics_clocks, offsetof(SmuMetrics_t, CurrClock) == 0 &&
    offsetof(SmuMetrics_t, AverageGfxclkFrequencyPreDs) == 54 &&
    offsetof(SmuMetrics_t, AverageMemclkFrequencyPostDs) == 64);
ABI_ASSERT(metrics_activity, offsetof(SmuMetrics_t, AverageGfxActivity) == 104 &&
    offsetof(SmuMetrics_t, AverageUclkActivity) == 106);
ABI_ASSERT(metrics_energy, offsetof(SmuMetrics_t, EnergyAccumulator) == 112);
ABI_ASSERT(metrics_temp, offsetof(SmuMetrics_t, AvgTemperature) == 120);
ABI_ASSERT(metrics_fan, offsetof(SmuMetrics_t, AvgFanRpm) == 152);
ABI_ASSERT(metrics_throttle, offsetof(SmuMetrics_t, ThrottlingPercentage) == 154);
ABI_ASSERT(table_ids, TABLE_PPTABLE == 0 && TABLE_COMBO_PPTABLE == 1 && TABLE_WATERMARKS == 2 &&
    TABLE_AVFS_PSM_DEBUG == 3 && TABLE_PMSTATUSLOG == 4 && TABLE_SMU_METRICS == 5 &&
    TABLE_DRIVER_SMU_CONFIG == 6 && TABLE_ACTIVITY_MONITOR_COEFF == 7 && TABLE_OVERDRIVE == 8 &&
    TABLE_I2C_COMMANDS == 9 && TABLE_DRIVER_INFO == 10 && TABLE_ECCINFO == 11 && TABLE_WIFIBAND == 12);
ABI_ASSERT(clock_ids, PPCLK_GFXCLK == 0 && PPCLK_SOCCLK == 1 && PPCLK_UCLK == 2 &&
    PPCLK_FCLK == 3 && PPCLK_DCLK_0 == 4 && PPCLK_VCLK_0 == 5 && PPCLK_DCLK_1 == 6 &&
    PPCLK_VCLK_1 == 7 && PPCLK_DISPCLK == 8 && PPCLK_DPPCLK == 9 && PPCLK_DPREFCLK == 10 &&
    PPCLK_DCFCLK == 11 && PPCLK_DTBCLK == 12 && PPCLK_COUNT == 13);
ABI_ASSERT(feature_ids, FEATURE_DPM_GFXCLK_BIT == 1 && FEATURE_DPM_UCLK_BIT == 3 &&
    FEATURE_DPM_FCLK_BIT == 4 && FEATURE_DPM_SOCCLK_BIT == 5 && FEATURE_DPM_MP0CLK_BIT == 6 &&
    FEATURE_DPM_LINK_BIT == 7 && FEATURE_DPM_DCN_BIT == 8 && FEATURE_MM_DPM_BIT == 21 &&
    FEATURE_THROTTLERS_BIT == 24 && FEATURE_ACDC_BIT == 27 && FEATURE_FAN_CONTROL_BIT == 30 &&
    FEATURE_GFX_IMU_BIT == 37 && FEATURE_SPARE_51_BIT == 51);
ABI_ASSERT(temp_ids, TEMP_EDGE == 0 && TEMP_HOTSPOT == 1 && TEMP_MEM == 4);
ABI_ASSERT(power_sources, POWER_SOURCE_AC == 0 && POWER_SOURCE_DC == 1);
ABI_ASSERT(irq_contexts, IH_INTERRUPT_ID_TO_DRIVER == 0xfe && IH_INTERRUPT_CONTEXT_ID_AC == 3 &&
    IH_INTERRUPT_CONTEXT_ID_DC == 4 && IH_INTERRUPT_CONTEXT_ID_THERMAL_THROTTLING == 7 &&
    IH_INTERRUPT_CONTEXT_ID_FAN_ABNORMAL == 8 && IH_INTERRUPT_CONTEXT_ID_FAN_RECOVERY == 9);

static void copy_bytes(void *dst, const void *src, size_t size)
{
    uint8_t *out = dst;
    const uint8_t *in = src;
    while (size--) *out++ = *in++;
}

static int check_mp1_7(rdna3_dev *d)
{
    const rdna3_ipver *ip = &d->ipver[HWIP_MP1];
    if (ip->present && ip->num_base && ip->major == 13 && ip->minor == 0 && ip->rev == 7)
        return RDNA3_OK;
    RERR(d, "SMU 13.0.7: private ABI requires discovered MP1 13.0.7 segment 0");
    return RDNA3_ENODEV;
}

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int smu13_0_7_bootup_values(rdna3_dev *d, const rdna3_vbios *vbios, rdna3_vbios_boot *b)
{
    uint16_t size = 0, needed;
    uint8_t frev = 0, crev = 0;
    const uint8_t *t;
    unsigned i;
    int ret = check_mp1_7(d);
    if (ret) return ret;
    for (i = 0; i < sizeof(*b); i++) ((uint8_t *)b)[i] = 0;
    t = vbios_data_table(vbios, RD_ATOM_DT_FIRMWAREINFO, &size, &frev, &crev);
    /* smu_v13_0_get_vbios_bootup_values explicitly maps revision 0/1/2 to
     * the 3.1 leading fields. Unknown later layouts are not defaulted to 3.4. */
    if (!t || frev != 3 || crev > 5) {
        RERR(d, "SMU 13.0.7: unsupported firmwareinfo %u.%u", frev, crev);
        return RDNA3_EOPNOTSUPP;
    }
    needed = crev >= 4 ? 88 : crev == 3 ? 64 : 38;
    if (size < needed) {
        RERR(d, "SMU 13.0.7: firmwareinfo %u.%u has %u bytes, needs %u", frev, crev, size, needed);
        return RDNA3_EINVAL;
    }
    if (crev <= 4) return vbios_bootup_values(d, vbios, b);
    /* atom_firmware_info_v3_5 names +8/+12 and +28/+32 RESERVED.
     * Never turn those bytes into clocks/voltages. DPM may make them unnecessary. */
    b->fw_frev = frev; b->fw_crev = crev;
    b->firmware_revision = read32(t + 4);
    b->firmware_capability = read32(t + 16);
    b->cooling_id = t[37];
    b->pp_table_id = read32(t + 60);
    b->fw_reserved_kb = read32(t + 84);
    t = vbios_data_table(vbios, RD_ATOM_DT_SMU_INFO, &size, &frev, &crev);
    if (t && frev == 3 && crev == 6 && size >= 184) {
        b->socclk = read32(t + 40); b->fclk = read32(t + 72);
        b->vclk = read32(t + 176); b->dclk = read32(t + 180);
    } else if (t && frev == 4 && crev == 0 && size >= 204) {
        b->socclk = read32(t + 40); b->dcefclk = read32(t + 56); b->fclk = read32(t + 72);
        b->dclk = read32(t + 196); b->vclk = read32(t + 200);
    }
    RLOG(d, "SMU 13.0.7: firmwareinfo 3.5 has no GFX/UCLK boot values; those clocks require DPM");
    return RDNA3_OK;
}

static int table_range(rdna3_dev *d, uint64_t off, uint32_t size)
{
    if (!d->vram_cpu || !off || off > d->vram_cpu_len || size > d->vram_cpu_len - off) {
        RERR(d, "SMU 13.0.7: driver table outside CPU VRAM (mapped %d, length 0x%llx, offset 0x%llx, size %u)",
             !!d->vram_cpu, (unsigned long long)d->vram_cpu_len, (unsigned long long)off, size);
        return RDNA3_EINVAL;
    }
    return RDNA3_OK;
}

static int table_read(rdna3_dev *d, uint64_t off, uint32_t id, void *dst, uint32_t size)
{
    /* The wire message carries no length. COMBO may DMA the full Linux
     * MP0_MP1_DATA_REGION_SIZE_COMBOPPTABLE allocation, not just our struct. */
    int ret = table_range(d, off, id == TABLE_COMBO_PPTABLE ? 0x4000u : size);
    if (!ret) ret = smu13_send_msg(d, PPSMC_MSG_TransferTableSmu2Dram, id, NULL);
    if (ret) return ret;
    rdna3_hdp_invalidate(d);
    rdna3_vram_read(d, off, dst, size);
    return RDNA3_OK;
}

int smu13_0_7_setup_pptable(rdna3_dev *d, rdna3_smu_13_0_7 *p, uint64_t driver_table,
                           const rdna3_vbios *vbios, uint32_t if_version, int scpm)
{
    struct smu_13_0_7_powerplay_table table;
    PPTable_t *pp = &table.smc_pptable;
    const SkuTable_t *sku = &pp->SkuTable;
    const uint8_t *board;
    uint16_t size = 0;
    uint8_t frev = 0, crev = 0;
    unsigned i;
    int ret = check_mp1_7(d);
    if (ret) return ret;

    if (if_version != SMU13_0_7_DRIVER_IF_VERSION) {
        RERR(d, "SMU 13.0.7: PM ABI requires driver IF 0x%x, got 0x%x",
             SMU13_0_7_DRIVER_IF_VERSION, if_version);
        return RDNA3_EOPNOTSUPP;
    }
    /* smu_v13_0_7_get_pptable_from_pmfw: also read the raw combo with SCPM. */
    ret = table_read(d, driver_table, TABLE_COMBO_PPTABLE, &table, sizeof(table));
    if (ret) return ret;
    if (table.header.structuresize < offsetof(struct smu_13_0_7_powerplay_table, smc_pptable) ||
        table.header.structuresize > 0x4000 ||
        table.table_size != offsetof(struct smu_13_0_7_powerplay_table, smc_pptable)) {
        RERR(d, "SMU 13.0.7: invalid combo header size %u / driver portion %u (expected %u)",
             table.header.structuresize, table.table_size,
             (unsigned)offsetof(struct smu_13_0_7_powerplay_table, smc_pptable));
        return RDNA3_EINVAL;
    }
    /* Board/SKU Version are opaque identifiers, NOT PPTABLE_VERSION. Linux
     * selects this layout by MP1, not an invented VBIOS revision whitelist. */
    p->board_table_appended = 0;
    if (!scpm) {
        const size_t header = sizeof(struct atom_common_table_header);
        const size_t board_data = offsetof(BoardTable_t, MmHubPadding);
        if (!vbios) {
            RERR(d, "SMU 13.0.7: non-SCPM bootstrap needs VBIOS smc_dpm_info");
            return RDNA3_ENOENT;
        }
        board = vbios_data_table(vbios, RD_ATOM_DT_SMC_DPM_INFO, &size, &frev, &crev);
        if (!board || (size < header + sizeof(BoardTable_t) &&
            !(frev == 5 && crev == 0 && size >= header + board_data))) {
            RERR(d, "SMU 13.0.7: smc_dpm_info %u.%u has %u bytes, needs %u (Atom 5.0 prefix %u)",
                 frev, crev, size, (unsigned)(header + sizeof(BoardTable_t)),
                 (unsigned)(header + board_data));
            return board ? RDNA3_EINVAL : RDNA3_ENOENT;
        }
        /* Observed Atom 5.0 contains the whole board prefix, not SMU-internal MMHUB padding. */
        copy_bytes(&pp->BoardTable, board + header, board_data);
        p->board_table_appended = 1;
        RLOG(d, "SMU 13.0.7: opaque BoardTable 0x%x from VBIOS smc_dpm_info %u.%u",
             pp->BoardTable.Version, frev, crev);
    }
    /* smu_cmn_write_pptable: SCPM forbids driver uploads and allowed-mask writes. */
    if (!scpm) {
        rdna3_vram_write(d, driver_table, pp, sizeof(*pp));
        rdna3_hdp_flush(d);
        ret = smu13_send_msg(d, PPSMC_MSG_TransferTableDram2Smu, TABLE_PPTABLE, NULL);
        if (ret) return ret;
    }
    p->pp_frev = table.header.format_revision;
    p->pp_crev = table.header.content_revision;
    p->thermal_controller = table.thermal_controller_type;
    p->shutdown_temp = table.software_shutdown_temp;
    p->platform_caps = table.platform_caps;
    p->sku_version = sku->Version;
    p->board_version = pp->BoardTable.Version;
    p->maco_support = (table.platform_caps & SMU_13_0_7_PP_PLATFORM_CAP_BACO) &&
                     (table.platform_caps & SMU_13_0_7_PP_PLATFORM_CAP_MACO) &&
                     (pp->BoardTable.HsrEnabled || pp->BoardTable.VddqOffEnabled);
    p->temp_edge = sku->TemperatureLimit[TEMP_EDGE];
    p->temp_hotspot = sku->TemperatureLimit[TEMP_HOTSPOT];
    p->temp_mem = sku->TemperatureLimit[TEMP_MEM];
    p->fan_max_rpm = sku->FanMaximumRpm;
    p->fan_temp_offset = sku->FanAbnormalTempLimitOffset;
    p->base_clock_ac = sku->DriverReportedClocks.BaseClockAc;
    p->game_clock_ac = sku->DriverReportedClocks.GameClockAc;
    p->ppt_ac = sku->SocketPowerLimitAc[PPT_THROTTLER_PPT0];
    p->ppt_message_limit_ac = sku->MsgLimits.Power[PPT_THROTTLER_PPT0][POWER_SOURCE_AC];
    p->ppt_message_limit_dc = sku->MsgLimits.Power[PPT_THROTTLER_PPT0][POWER_SOURCE_DC];
    for (i = 0; i < NUM_LINK_LEVELS; i++) {
        p->pcie_gen[i] = sku->PcieGenSpeed[i];
        p->pcie_lane[i] = sku->PcieLaneCount[i];
        p->lclk[i] = sku->LclkFreq[i];
    }
    RLOG(d, "SMU 13.0.7: combo %u.%u, SKU 0x%x, board 0x%x, SCPM %d, AC rated PPT %u W",
         p->pp_frev, p->pp_crev, p->sku_version, p->board_version, scpm, p->ppt_ac);
    return RDNA3_OK;
}

/* smu_v13_0_7_get_allowed_feature_mask, from zero (amd_shared.h ppfeaturemask). */
uint64_t smu13_0_7_allowed_features(uint32_t pp_feature, int athub_mmhub_pg)
{
#define F(bit) (1ull << FEATURE_##bit##_BIT)
    uint64_t m = F(FW_DATA_READ) | F(DPM_SOCCLK) | F(DS_LCLK) | F(DPM_MP0CLK) | F(MM_DPM) |
        F(DS_VCN) | F(DS_FCLK) | F(DF_CSTATE) | F(THROTTLERS) | F(VR0HOT) | F(FW_CTF) |
        F(FAN_CONTROL) | F(DS_SOCCLK) | F(GFXCLK_SPREAD_SPECTRUM) | F(MEM_TEMP_READ) |
        F(FW_DSTATE) | F(SOC_MPCLK_DS) | F(BACO_MPCLK_DS) | F(GFX_PCC_DFLL) | F(SOC_CG) | F(BACO);
    if (pp_feature & 0x1u) m |= F(DPM_GFXCLK) | F(GFX_IMU) | F(DPM_GFX_POWER_OPTIMIZER);
    if (pp_feature & 0x8000u) m |= F(GFXOFF);
    if (pp_feature & 0x2u) m |= F(DPM_UCLK) | F(DPM_FCLK) | F(VMEMP_SCALING) | F(VDDIO_MEM_SCALING);
    if (pp_feature & 0x4u) m |= F(DPM_LINK);
    if (pp_feature & 0x8u) m |= F(DS_GFXCLK);
    if (pp_feature & 0x100u) m |= F(GFX_ULV);
    if (pp_feature & 0x2000u) m |= F(DPM_DCN);
    if (athub_mmhub_pg) m |= F(ATHUB_MMHUB_PG);
    return m;
#undef F
}

/* Only audited bootstrap/common controls; old table-backed tuning stays closed. */
int smu13_0_7_message_supported(uint32_t msg)
{
    switch (msg) {
    case PPSMC_MSG_TestMessage: case PPSMC_MSG_GetSmuVersion: case PPSMC_MSG_GetDriverIfVersion:
    case PPSMC_MSG_SetAllowedFeaturesMaskLow: case PPSMC_MSG_SetAllowedFeaturesMaskHigh:
    case PPSMC_MSG_EnableAllSmuFeatures:
    case PPSMC_MSG_GetRunningSmuFeaturesLow: case PPSMC_MSG_GetRunningSmuFeaturesHigh:
    case PPSMC_MSG_SetDriverDramAddrHigh: case PPSMC_MSG_SetDriverDramAddrLow:
    case PPSMC_MSG_SetToolsDramAddrHigh: case PPSMC_MSG_SetToolsDramAddrLow:
    case PPSMC_MSG_TransferTableSmu2Dram: case PPSMC_MSG_TransferTableDram2Smu:
    case PPSMC_MSG_SetSoftMinByFreq: case PPSMC_MSG_SetSoftMaxByFreq:
    case PPSMC_MSG_GetDpmFreqByIndex: case PPSMC_MSG_OverridePcieParameters:
    case PPSMC_MSG_SetWorkloadMask: case PPSMC_MSG_GetPptLimit:
    case PPSMC_MSG_AllowGfxOff: case PPSMC_MSG_DisallowGfxOff:
    case PPSMC_MSG_PowerUpVcn: case PPSMC_MSG_PowerDownVcn:
    case PPSMC_MSG_PowerUpJpeg: case PPSMC_MSG_PowerDownJpeg:
    case PPSMC_MSG_NotifyPowerSource: case PPSMC_MSG_ReenableAcDcInterrupt:
    case PPSMC_MSG_AllowIHHostInterrupt:
        return 1;
    default: return 0;
    }
}

/* smu_v13_0_7_get_gpu_metrics / get_smu_metrics_data. Serial is not exposed:
 * 13.0.7 registers no get_unique_id callback, unlike 13.0.0. */
int smu13_0_7_read_metrics(rdna3_dev *d, rdna3_smu_13_0_7 *p, uint64_t driver_table)
{
    SmuMetricsExternal_t ext;
    const SmuMetrics_t *m = &ext.SmuMetrics;
    rdna3_gpu_metrics *g = &p->gpu_metrics;
    rdna3_pm_detail *o = &p->detail;
    uint32_t i;
    int ret = check_mp1_7(d);
    if (!ret) ret = table_read(d, driver_table, TABLE_SMU_METRICS, &ext, sizeof(ext));
    if (ret) return ret;
    for (i = 0; i < sizeof(*g); i++) ((uint8_t *)g)[i] = 0;
    for (i = 0; i < PPCLK_COUNT; i++) p->clock_mhz[i] = m->CurrClock[i];
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
    g->average_gfxclk_frequency = m->AverageGfxActivity <= 15 ? m->AverageGfxclkFrequencyPostDs
        : m->AverageGfxclkFrequencyPreDs;
    g->average_uclk_frequency = m->AverageUclkActivity <= 15 ? m->AverageMemclkFrequencyPostDs
        : m->AverageMemclkFrequencyPreDs;
    g->average_vclk0_frequency = m->AverageVclk0Frequency;
    g->average_dclk0_frequency = m->AverageDclk0Frequency;
    g->average_vclk1_frequency = m->AverageVclk1Frequency;
    g->average_dclk1_frequency = m->AverageDclk1Frequency;
    g->current_gfxclk = m->CurrClock[PPCLK_GFXCLK];
    g->current_socclk = m->CurrClock[PPCLK_SOCCLK];
    g->current_uclk = m->CurrClock[PPCLK_UCLK];
    g->current_vclk0 = m->CurrClock[PPCLK_VCLK_0];
    g->current_dclk0 = m->CurrClock[PPCLK_DCLK_0];
    g->current_vclk1 = m->CurrClock[PPCLK_VCLK_1];
    g->current_dclk1 = m->CurrClock[PPCLK_DCLK_1];
    for (i = 0; i < THROTTLER_COUNT; i++)
        if (m->ThrottlingPercentage[i]) g->throttle_status |= 1u << i;
    g->current_fan_speed = m->AvgFanRpm;
    g->pcie_link_width = m->PcieWidth;
    g->pcie_link_speed = m->PcieRate == 2 ? 50 : m->PcieRate == 3 ? 80 : m->PcieRate == 4 ? 160 : 25;
    g->voltage_gfx = m->AvgVoltage[SVI_PLANE_GFX];
    g->voltage_soc = m->AvgVoltage[SVI_PLANE_SOC];
    g->voltage_mem = m->AvgVoltage[SVI_PLANE_VMEMP];
    o->gfx_activity = m->AverageGfxActivity;
    o->uclk_activity = m->AverageUclkActivity;
    o->pcie_busy = m->PCIeBusy;
    o->gfxclk_target = m->AverageGfxclkFrequencyTarget;
    o->gfxclk_pre_ds = m->AverageGfxclkFrequencyPreDs;
    o->gfxclk_post_ds = m->AverageGfxclkFrequencyPostDs;
    o->memclk_post_ds = m->AverageMemclkFrequencyPostDs;
    o->fclk_post_ds = m->AverageFclkFrequencyPostDs;
    o->socclk = m->CurrClock[PPCLK_SOCCLK];
    o->dcefclk = m->CurrClock[PPCLK_DCFCLK];
    o->socket_w = m->AverageSocketPower;
    o->board_w = m->AverageTotalBoardPower;
    o->vddgfx_mv = m->AvgVoltage[SVI_PLANE_GFX];
    p->fan_pwm = (uint32_t)m->AvgFanPwm * 255 / 100;
    if (p->fan_pwm > 255) p->fan_pwm = 255;
    return RDNA3_OK;
}
