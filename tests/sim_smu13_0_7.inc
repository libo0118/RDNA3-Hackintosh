/* Linux v6.12 ABI byte fixture, independent from the private vendor C types.
 * smu13_driver_if_v13_0_7.h: PP 3972 = SKU 3484 + board 488;
 * smu_v13_0_7_pptable.h: driver portion 832, combo 4804, metrics 392.
 * These are model inputs, not a captured RX 7600 XT or a hardware result. */

static void pm_message(model *m, uint32_t msg, uint32_t param)
{
    uint32_t *arg = &m->reg[m->mp1_base + 0x292];
    uint64_t off;
    static const uint16_t clocks[13][2] = {
        {600, 3200}, {400, 1200}, {100, 1200}, {500, 1800}, {200, 900}, {300, 1000},
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {300, 900}, {0, 0}
    };
    if (msg == m->fail_msg && (param == m->fail_param || m->fail_param == UINT32_MAX)) {
        m->reg[m->mp1_base + 0x29a] = 0xfd;
        return;
    }
    switch (msg) {
    case 0xe: m->driver_mc = (uint64_t)param << 32; break;
    case 0xf: m->driver_mc |= param; break;
    case 0x5: m->allowed = (uint64_t)param << 32; break;
    case 0x4: m->allowed |= param; break;
    case 0xd: *arg = (uint32_t)(m->enabled >> 32); break;
    case 0xc: *arg = (uint32_t)m->enabled; break;
    case 0x12:
        assert(m->driver_mc >= m->fb_start);
        off = m->driver_mc - m->fb_start;
        assert(off + 0x4000 <= sizeof(m->vram));
        if (param == 1) {
            memset(m->vram + off, 0xc3, 0x4000); /* legal full COMBO DMA */
            memcpy(m->vram + off, m->combo, sizeof(m->combo));
        }
        else if (param == 5) memcpy(m->vram + off, m->metric_bytes, sizeof(m->metric_bytes));
        else assert(0 && "unexpected 13.0.7 table read");
        break;
    case 0x13:
        assert(param == 0 && m->driver_mc >= m->fb_start);
        off = m->driver_mc - m->fb_start;
        assert(off + sizeof(m->uploaded) <= sizeof(m->vram));
        memcpy(m->uploaded, m->vram + off, sizeof(m->uploaded));
        m->uploads++;
        break;
    case 0x1f:
        assert((param >> 16) < 6 || (param >> 16) == 11);
        if ((param & 0xffff) == 0xff)
            *arg = m->dpm_count | ((param >> 16) == 0 ? 0x80000000u : 0);
        else {
            assert((param & 0xffff) < 2);
            *arg = clocks[param >> 16][param & 0xffff];
            if (m->bad_clock_value && param == 0) *arg = m->bad_clock_value;
        }
        break;
    case 0x33: assert(param == 0 || param == 0x10000); *arg = param ? 77 : 123; break;
    default: break;
    }
}

static void pm_fixture(rdna3_dev *d, model *m, rdna3_vbios *v)
{
    uint8_t *b, *sku, *fw;
    unsigned i;
    fixture(d, m, 7);
    m->flags7 = 1;
    m->pm_model = 1;
    m->dpm_count = 2;
    m->fb_start = d->fb_start = 0x120000000ull;
    m->enabled = (1ull << 0) | (1ull << 1) | (1ull << 3) | (1ull << 4) | (1ull << 5) | (1ull << 8) |
        (1ull << 21) | (1ull << 24) | (1ull << 30) | (1ull << 37);
    /* Atom common header is driver-portion length, not DMA byte count. */
    w16(m->combo, 832); m->combo[2] = 15; m->combo[4] = 2;
    w16(m->combo + 6, 832);
    w32(m->combo + 18, 0x19); /* POWERPLAY | BACO | MACO */
    m->combo[22] = 28;
    w16(m->combo + 29, 110);
    sku = m->combo + 832;
    w32(sku, 0x12345678); /* SKU identity, deliberately NOT PPTABLE_VERSION. */
    w32(sku + 4, 1u << 30);
    w16(sku + 16, 150);   /* SocketPowerLimitAc[0] */
    w16(sku + 24, 100);   /* SocketPowerLimitDc[0] */
    w16(sku + 50, 95); w16(sku + 52, 105); w16(sku + 58, 100);
    sku[1232] = 0; sku[1233] = 2; sku[1234] = 3;
    sku[1235] = 1; sku[1236] = 4; sku[1237] = 6;
    w16(sku + 1238, 100); w16(sku + 1240, 400); w16(sku + 1242, 600);
    w16(sku + 1330, 3000); w16(sku + 1412, (uint16_t)-5);
    w16(sku + 2780, 2100); w16(sku + 2782, 2800);
    w16(sku + 2808, 170);
    w16(sku + 2810, 110);
    w32(sku + 3484, 0x87654321); /* PMFW board, replaced only without SCPM. */
    sku[3484 + 189] = 1;

    /* A real parser consumes an in-memory Atom ROM, not hand-filled offsets. */
    b = m->rom;
    b[0] = 0x55; b[1] = 0xaa; b[2] = 4;
    memcpy(b + 0x30, " 761295520", 10);
    w16(b + 0x48, 0x80);
    w16(b + 0x80, 0x24); b[0x82] = 2; b[0x83] = 2;
    memcpy(b + 0x84, "ATOM", 4);
    w16(b + 0xa0, 0xc0);
    w16(b + 0xc0, 4 + 2 * RD_ATOM_DT_COUNT); b[0xc2] = 2; b[0xc3] = 1;
    w16(b + 0xc4 + 2 * RD_ATOM_DT_FIRMWAREINFO, 0x180);
    w16(b + 0xc4 + 2 * RD_ATOM_DT_SMU_INFO, 0x200);
    w16(b + 0xc4 + 2 * RD_ATOM_DT_SMC_DPM_INFO, 0x400);
    fw = b + 0x180;
    w16(fw, 108); fw[2] = 3; fw[3] = 4;
    w32(fw + 8, 50000); w32(fw + 12, 10000);
    w16(b + 0x200, 204); b[0x202] = 4; b[0x203] = 0;
    w32(b + 0x200 + 40, 40000); w32(b + 0x200 + 72, 50000);
    w32(b + 0x200 + 56, 35000);
    w32(b + 0x200 + 196, 20000); w32(b + 0x200 + 200, 30000);
    w16(b + 0x400, 492);
    /* No upstream revision whitelist; keep arbitrary common-header revisions. */
    b[0x402] = 9; b[0x403] = 17;
    for (i = 0; i < 488; i++) b[0x404 + i] = (uint8_t)(i * 7);
    w32(b + 0x404, 0); /* Zero is a valid opaque board identity. */
    b[0x404 + 176] = 0xa6; /* 13.0.7 FclkSpreadPadding, not GfxclkSpreadEnable. */
    b[0x404 + 188] = 1; b[0x404 + 189] = 0;
    assert(vbios_parse(d, v, b, sizeof(m->rom)) == RDNA3_OK);

    /* Independent metrics offsets, including the four-byte serial shift. */
    for (i = 0; i < 13; i++) w32(m->metric_bytes + 4 * i, 1000 + i);
    w16(m->metric_bytes + 52, 2300); w16(m->metric_bytes + 54, 2200);
    w16(m->metric_bytes + 56, 300); w16(m->metric_bytes + 60, 700);
    w16(m->metric_bytes + 62, 1200); w16(m->metric_bytes + 64, 100);
    w16(m->metric_bytes + 66, 800); w16(m->metric_bytes + 68, 750);
    w16(m->metric_bytes + 70, 650); w16(m->metric_bytes + 72, 600);
    w16(m->metric_bytes + 74, 17); w16(m->metric_bytes + 84, 950);
    w16(m->metric_bytes + 86, 900); w16(m->metric_bytes + 88, 850);
    w16(m->metric_bytes + 104, 67); w16(m->metric_bytes + 106, 12);
    w16(m->metric_bytes + 108, 30); w16(m->metric_bytes + 110, 40);
    w32(m->metric_bytes + 112, 0x87654321);
    w16(m->metric_bytes + 116, 87); w16(m->metric_bytes + 118, 101);
    w16(m->metric_bytes + 120, 48); w16(m->metric_bytes + 122, 64);
    w16(m->metric_bytes + 128, 56);
    m->metric_bytes[148] = 4; m->metric_bytes[149] = 8;
    m->metric_bytes[150] = 37; w16(m->metric_bytes + 152, 1456);
    m->metric_bytes[154 + 1] = 7; m->metric_bytes[154 + 16] = 11;
    w32(m->metric_bytes + 232, 0x11223344);
    w32(m->metric_bytes + 236, 0x55667788);
    w32(m->metric_bytes + 240, 0xdeadbeef); /* legacy high would misread spare */
}

static uint32_t msg_index(const model *m, uint32_t msg, uint32_t param)
{
    uint32_t i;
    for (i = 0; i < m->nmsg; i++)
        if (m->msgs[i] == msg && m->params[i] == param) return i;
    return UINT32_MAX;
}

static void test_pm7_success(int scpm, int exact)
{
    rdna3_dev d;
    rdna3_smu s;
    rdna3_vbios v;
    rdna3_uclk_pin pin;
    rdna3_pmctl_req rq;
    rdna3_pmctl_reply rp;
    rdna3_gpu_metrics g;
    rdna3_temp_range tr;
    rdna3_pm_detail detail;
    model m;
    uint32_t i, n, writes, got = 0;
    int sent = 0;
    const uint32_t prefix[] = {1, 2, 3, 0xe, 0xf, 0x10, 0x11, 0x12};
    pm_fixture(&d, &m, &v);
    d.amdgpu_exact = exact;
    assert(smu13_pm_init(&d, &s, &v, ~0ull, 2, 4, scpm) == RDNA3_OK);
    assert(s.initialized && s.fw_status_ok && s.mp1_rev == 7 && s.dpm_running);
    assert(s.allowed == 0xa3a471fe7fffull && !(s.allowed >> 49));
    assert(s.driver_table == 4096 && s.tool_table == 20480);
    assert(m.driver_mc == d.fb_start + s.driver_table);
    for (i = 0; i < sizeof(prefix) / sizeof(prefix[0]); i++) assert(m.msgs[i] == prefix[i]);
    assert(m.params[3] == 1 && m.params[4] == 0x20001000);
    assert(m.params[5] == 1 && m.params[6] == 0x20005000);
    assert(m.params[7] == 1 && m.uploads == (scpm ? 0u : 1u));
    assert(s.board_table_appended == !scpm);
    if (!scpm) {
        assert(!memcmp(m.uploaded, m.combo + 832, 3484));
        assert(!memcmp(m.uploaded + 3484, m.rom + 0x404, 488));
        assert(s.navi33.board_version == 0 && s.navi33.maco_support);
        assert(msg_index(&m, 0x13, 0) < msg_index(&m, 0x5, (uint32_t)(s.allowed >> 32)));
        assert(msg_index(&m, 0x5, (uint32_t)(s.allowed >> 32)) < msg_index(&m, 0x4, (uint32_t)s.allowed));
        assert(m.allowed == s.allowed);
    } else {
        assert(s.navi33.board_version == 0x87654321 && !m.allowed);
        for (i = 0; i < m.nmsg; i++) assert(m.msgs[i] != 4 && m.msgs[i] != 5 && m.msgs[i] != 0x13);
    }
    assert(s.gfx.count == 2 && s.gfx.fine_grained && s.gfx.level[1] == 2800);
    assert(s.uclk.level[1] == 1200 && s.soc.level[1] == 1200 && s.fclk.level[1] == 1800);
    assert(s.vdclk_valid && s.vclk.level[1] == 1000 && s.dclk.level[1] == 900);
    assert(s.dcef.count == 2 && s.dcef.level[1] == 900);
    assert(s.pcie_levels == 3 && s.pcie_gen[2] == 2 && s.pcie_lane[2] == 4);
    assert(msg_index(&m, 0x6, 0) < msg_index(&m, 0x1f, 0xff));
    assert(msg_index(&m, 0x1f, (1u << 16) | 0xff) < msg_index(&m, 0x1f, 0xff));
    assert(msg_index(&m, 0x1f, (5u << 16) | 1) < msg_index(&m, 0x20, (2u << 16) | (2u << 8) | 4));
    assert(msg_index(&m, 0x1f, (11u << 16) | 1) < msg_index(&m, 0x20, (2u << 16) | (2u << 8) | 4));
    for (i = 0; i < m.nmsg; i++)
        assert(m.msgs[i] != 0x36 && m.msgs[i] != 0x4e && m.msgs[i] != 0x1b && m.msgs[i] != 7);
    assert(s.irq_enabled && s.software_shutdown_temp == 110 && !m.flag_writes);

    /* Poison every legacy table: no 13.0.7 reader may use those bytes. */
    memset(&s.pptable, 0xa5, sizeof(s.pptable));
    memset(&s.metrics, 0x5a, sizeof(s.metrics));
    assert(smu13_pm_query(&d, &s, &v, 1) == RDNA3_OK);
    assert(s.ac_power == -1 && s.current_power_limit == UINT32_MAX && s.metrics_valid);
    assert(msg_index(&m, 0x33, 0) == UINT32_MAX && msg_index(&m, 0x35, 0) == UINT32_MAX);
    assert(smu13_irq_process(&d, &s, RD_IH_CLIENT_MP1, 0xfe, RD_SMU_IH_CTX_AC) == RD_SMU_IRQ_NONE);
    assert(smu13_pm_query(&d, &s, &v, 1) == RDNA3_OK);
    assert(!s.od_enabled && !s.od_valid && !s.unique_id && !s.baco_platform);
    assert(s.current_power_limit == 123 && s.power_limit_max == 170);
    assert(smu13_metric_gfxclk_mhz(&s) == 2200 && smu13_metric_uclk_mhz(&s) == 1002);
    assert(smu13_metric_temp_edge_c(&s) == 48 && smu13_metric_temp_hotspot_c(&s) == 64);
    assert(smu13_metric_temp_mem_c(&s) == 56 && smu13_metric_power_w(&s) == 87);
    assert(smu13_metric_fan_rpm(&s) == 1456 && smu13_metric_vddgfx_mv(&s) == 950);
    assert(smu13_metric_throttlers(&s) == ((1u << 1) | (1u << 16)));
    smu13_metric_detail(&s, &detail);
    assert(detail.board_w == 101 && detail.pcie_busy == 17 && detail.dcefclk == 1011);
    assert(smu13_get_thermal_temperature_range(&s, &tr) == RDNA3_OK);
    assert(tr.max == 95000 && tr.hotspot_crit_max == 105000 && tr.mem_crit_max == 100000);
    assert(tr.software_shutdown_temp == 110 && tr.software_shutdown_temp_offset == -5);
    assert(smu13_fan_max_rpm(&s) == 3000 && !smu13_od_feature_mask(&s));
    assert(smu13_get_gpu_metrics(&d, &s, &g) == RDNA3_OK);
    assert(g.energy_accumulator == 0x87654321 && g.average_socket_power == 87);
    assert(g.average_gfxclk_frequency == 2200 && g.average_uclk_frequency == 100);
    assert(g.current_gfxclk == 1000 && g.current_vclk0 == 1005 && g.average_vclk0_frequency == 800);
    assert(g.pcie_link_width == 8 && g.pcie_link_speed == 160);
    assert(smu13_get_current_clk_freq_by_table(&d, &s, PPCLK_FCLK, &got) == RDNA3_OK && got == 1003);
    assert(smu13_get_current_clk_freq_by_table(&d, &s, PPCLK_VCLK_0, &got) == RDNA3_OK && got == 1005);
    w16(m.metric_bytes + 104, 1);
    assert(smu13_get_gpu_metrics(&d, &s, &g) == RDNA3_OK && g.average_gfxclk_frequency == 300);
    assert(smu13_get_current_clk_freq_by_table(&d, &s, PPCLK_GFXCLK, &got) == RDNA3_OK && got == 2200);
    n = m.nmsg;
    smu13_get_unique_id(&d, &s);
    assert(m.nmsg == n && s.unique_id == 0); /* no old serial callback */
    memset(&rq, 0, sizeof(rq)); rq.abi = RDNA3_PMCTL_ABI;
    rq.op = RD_PMCTL_INFO;
    assert(smu13_pmctl(&d, &s, &rq, &rp, ~0u) == RDNA3_OK && rp.info.pm_up);
    assert(rp.info.fan_max_rpm == 3000 && rp.info.metrics.temperature_hotspot == 64 && !rp.info.unique_id);
    assert(rp.info.gfxclk_max == 2800 && rp.info.thermal.software_shutdown_temp == 110);
    n = m.nmsg; writes = m.vram_writes;
    for (rq.op = 1; rq.op < RD_PMCTL_OP_COUNT; rq.op++)
        assert(smu13_pmctl(&d, &s, &rq, &rp, ~0u) == RDNA3_EOPNOTSUPP && !rp.info.pm_up);
    assert(m.nmsg == n && m.vram_writes == writes);
    assert(smu13_table_write(&d, &s, TABLE_PPTABLE, 0, &s.pptable, sizeof(s.pptable)) == RDNA3_EOPNOTSUPP);
    assert(smu13_table_read(&d, &s, TABLE_SMU_METRICS, 0, &s.metrics, sizeof(s.metrics)) == RDNA3_EOPNOTSUPP);
    assert(smu13_set_fan_speed_pwm(&d, &s, 255) == RDNA3_EOPNOTSUPP);
    assert(smu13_set_power_limit(&d, &s, 150) == RDNA3_EOPNOTSUPP);
    assert(smu13_force_performance_level(&d, &s, RD_DPM_FORCED_LEVEL_HIGH, NULL, NULL) == RDNA3_EOPNOTSUPP);
    assert(m.nmsg == n && m.vram_writes == writes);

    /* hwInit's actual post-PM default path: independent DAL UCLK pin/release. */
    assert(dcn32_uclk_pin_init(&d, &pin, 0) == RDNA3_OK);
    assert(dcn32_uclk_pin_update(&d, &pin, 2, 0, 0, &sent) == RDNA3_OK && sent);
    assert(dcn32_uclk_pin_update(&d, &pin, 1, 0, 0, &sent) == RDNA3_OK && sent);
    assert(m.ndal == 8 && m.dal_msgs[4] == 9 && m.dal_msgs[5] == 0x15);
    assert(dcn32_smu_send_fclk_pstate_message(&d, 1) == RDNA3_EOPNOTSUPP && m.ndal == 8);
    assert(smu13_set_vcn_enable(&d, 1, 0, 1) == RDNA3_OK);
    assert(smu13_set_jpeg_enable(&d, 1) == RDNA3_OK);

    n = m.nmsg;
    m.fail_msg = 0x12; m.fail_param = 5;
    rq.op = RD_PMCTL_INFO;
    assert(smu13_pmctl(&d, &s, &rq, &rp, ~0u) == RDNA3_EIO && !rp.info.pm_up && !s.metrics_valid);
    m.fail_msg = 0;
    s.driver_table = sizeof(m.vram) - 391;
    assert(smu13_read_metrics(&d, &s) == RDNA3_EINVAL && m.nmsg == n + 3);
    s.driver_table = 4096;
    assert(smu13_read_metrics(&d, &s) == RDNA3_OK);
    assert(smu13_is_mode1_reset_supported(&s));
    n = m.nmsg;
    assert(smu13_mode1_reset(&d) == RDNA3_OK && m.nmsg == n && !m.flag_writes && m.flags7 == 1);
    assert(smu13_pm_fini(&d, &s) == RDNA3_OK && !s.initialized && !s.irq_enabled);
    assert(m.nmsg == n && !m.flag_writes && m.flags7 == 1);
    assert(smu13_pmctl(&d, &s, &rq, &rp, ~0u) == RDNA3_ENODEV && !rp.info.pm_up);
}

static void test_pm7_bad_input(void)
{
    rdna3_dev d;
    rdna3_smu s;
    rdna3_vbios v;
    model m;
    unsigned i;
    for (i = 0; i < 9; i++) {
        int ret;
        pm_fixture(&d, &m, &v);
        switch (i) {
        case 0: m.if_version = 0x3d; break;
        case 1: w16(m.combo, 831); break;
        case 2: w16(m.combo + 6, 828); break;
        case 3: w16(m.rom + 0x400, 491); break;
        case 4: w16(m.rom + 0x400, 2048); break;
        case 5: w16(m.rom + 0x180, 37); break;
        case 6: break; /* missing VBIOS, never silently reuse PMFW board */
        case 7: m.rom[0x182] = 4; break; /* unknown firmwareinfo format */
        case 8: m.rom[0x183] = 6; break; /* no speculative later-revision cast */
        }
        ret = smu13_pm_init(&d, &s, i == 6 ? NULL : &v, ~0ull, 3, 6, 0);
        assert(ret != RDNA3_OK && !s.initialized && !m.uploads && !m.allowed && !m.flag_writes);
        assert(msg_index(&m, 0x6, 0) == UINT32_MAX);
        if (!i) assert(ret == RDNA3_EOPNOTSUPP && m.nmsg == 3 && !m.vram_writes && !s.driver_table);
    }
    pm_fixture(&d, &m, &v);
    assert(smu13_pm_init(&d, &s, NULL, ~0ull, 3, 6, 1) == RDNA3_OK && !m.uploads);
    for (i = 0; i < 2; i++) {
        pm_fixture(&d, &m, &v);
        m.dpm_count = i ? 17 : 0;
        assert(smu13_pm_init(&d, &s, &v, ~0ull, 3, 6, 0) == RDNA3_EINVAL && !s.initialized);
    }
    pm_fixture(&d, &m, &v); d.ipver[HWIP_THM].num_base = 0;
    assert(smu13_pm_init(&d, &s, &v, ~0ull, 3, 6, 0) == RDNA3_ENODEV && !m.writes);
    pm_fixture(&d, &m, &v);
    assert(smu13_pm_init(&d, &s, &v, ~0ull, UINT32_MAX, 6, 0) == RDNA3_ENODEV && !m.writes);
    pm_fixture(&d, &m, &v); d.alloc_hi = 4096;
    assert(smu13_pm_init(&d, &s, &v, ~0ull, 3, 6, 0) == RDNA3_ENOMEM && !m.vram_writes && !s.initialized);
    pm_fixture(&d, &m, &v); m.bad_clock_value = 0x10000;
    assert(smu13_pm_init(&d, &s, &v, ~0ull, 3, 6, 0) == RDNA3_EINVAL && !s.initialized);
    pm_fixture(&d, &m, &v); m.bad_clock_value = 4000;
    assert(smu13_pm_init(&d, &s, &v, ~0ull, 3, 6, 0) == RDNA3_EINVAL && !s.initialized);
}

static void test_boot_values_35(void)
{
    rdna3_dev d;
    rdna3_smu s;
    rdna3_vbios v;
    rdna3_vbios_boot boot;
    model m;
    uint8_t *fw;
    pm_fixture(&d, &m, &v);
    /* Full atom_firmware_info_v3_5 (172 bytes), away from smu_info/board. */
    fw = m.rom + 0x600;
    memcpy(fw, m.rom + 0x180, 108);
    w16(m.rom + 0xc4 + 2 * RD_ATOM_DT_FIRMWAREINFO, 0x600);
    w16(fw, 172); fw[3] = 5;
    w32(fw + 8, 0xdeadbeef); w32(fw + 12, 0xcafebabe); /* reserved, not clocks */
    w32(fw + 28, 0x12345678); w32(fw + 32, 0x87654321); /* reserved, not voltages */
    assert(smu13_0_7_bootup_values(&d, &v, &boot) == RDNA3_OK);
    assert(!boot.gfxclk && !boot.uclk && !boot.vddc && !boot.vdd_gfx);
    assert(boot.socclk == 40000 && boot.fclk == 50000 && boot.vclk == 30000);
    assert(smu13_pm_init(&d, &s, &v, RD_SMU_FEATURES_SAFE, 3, 6, 0) == RDNA3_EOPNOTSUPP);
    assert(!m.uploads && msg_index(&m, 0x13, 0) == UINT32_MAX); /* a required fallback is unknown */
    /* Independent live DPM enumeration does not need those reserved fields. */
    m.nmsg = 0; m.allowed = 0;
    assert(smu13_pm_init(&d, &s, &v, ~0ull, 3, 6, 0) == RDNA3_OK);
    assert(s.gfx.level[0] == 600 && s.uclk.level[0] == 100 && !s.boot.gfxclk && !s.boot.uclk);
}

static void test_pm7_failure_order(void)
{
    const uint32_t fail[][2] = {
        {0xe, 1}, {0x12, 1}, {0x13, 0}, {5, UINT32_MAX}, {4, UINT32_MAX}, {6, 0},
        {0xd, 0}, {0x1f, (2u << 16) | 0xff}, {0x20, UINT32_MAX}, {0x24, 1}
    };
    rdna3_dev d;
    rdna3_smu s;
    rdna3_vbios v;
    model m;
    unsigned i;
    for (i = 0; i < sizeof(fail) / sizeof(fail[0]); i++) {
        pm_fixture(&d, &m, &v);
        m.fail_msg = fail[i][0]; m.fail_param = fail[i][1];
        assert(smu13_pm_init(&d, &s, &v, ~0ull, 3, 6, 0) == RDNA3_EIO);
        assert(!s.initialized && !s.irq_enabled && !m.flag_writes && strstr(m.log, "-> resp 0xfd") != NULL);
    }
    pm_fixture(&d, &m, &v);
    w32(m.combo + 18, 0x1d); /* GPIO-controlled DC with an enabled ACDC feature */
    m.enabled |= 1ull << 27;
    m.fail_msg = 0x4c; m.fail_param = 0;
    assert(smu13_pm_init(&d, &s, &v, ~0ull, 3, 6, 1) == RDNA3_EIO);
    assert(!s.initialized && !s.irq_enabled && !m.flag_writes);
    pm_fixture(&d, &m, &v);
    assert(smu13_pm_init(&d, &s, &v, ~0ull, 3, 6, 0) == RDNA3_OK);
    assert(smu13_irq_process(&d, &s, RD_IH_CLIENT_MP1, 0xfe, RD_SMU_IH_CTX_AC) == RD_SMU_IRQ_NONE);
    m.fail_msg = 0x33; m.fail_param = 0;
    assert(smu13_pm_query(&d, &s, &v, 0) == RDNA3_EIO); /* observed source, real query failure */
}

static void test_pm7_dal_gate(void)
{
    rdna3_dev d;
    model m;
    fixture(&d, &m, 7);
    assert(d.ipver[HWIP_GC].rev == 2);
    assert(dcn32_smu_set_hard_min(&d, RD_PPCLK_UCLK, 1200, 0x524dff, NULL) == RDNA3_OK);
    assert(m.ndal == 1); /* below Navi33's gate, even above Navi32's gate */
    assert(dcn32_smu_set_hard_min(&d, RD_PPCLK_UCLK, 1200, 0x524e00, NULL) == RDNA3_OK);
    assert(m.ndal == 3 && m.dal_msgs[2] == 0x15);
    fixture(&d, &m, 7);
    m.reg[DAL_RESP] = 0;
    assert(dcn32_smu_send(&d, 2, 0, NULL) == RDNA3_ETIME && !m.ndal && !m.writes);
}

static void test_pm7_external_contract(void)
{
    rdna3_dev d;
    rdna3_smu s;
    rdna3_smu_13_0_7 p;
    rdna3_vbios v;
    rdna3_pmctl_req rq;
    rdna3_pmctl_reply rp;
    model m;
    uint64_t off;
    unsigned i;
    const uint32_t caps[] = {0x11, 0x9, 0x19};
    pm_fixture(&d, &m, &v);
    memset(&p, 0, sizeof(p));
    off = sizeof(m.vram) - sizeof(m.combo);
    assert(smu13_0_7_setup_pptable(&d, &p, off, &v, 0x35, 1) == RDNA3_EINVAL);
    assert(!m.nmsg && !m.writes && !m.vram_writes); /* 4804 fits, 0x4000 does not */
    d.vram_cpu = NULL;
    assert(smu13_0_7_setup_pptable(&d, &p, 4096, &v, 0x35, 1) == RDNA3_EINVAL && !m.nmsg);
    d.vram_cpu = m.vram;
    off = sizeof(m.vram) - 0x4000;
    m.driver_mc = m.fb_start + off;
    assert(smu13_0_7_setup_pptable(&d, &p, off, &v, 0x35, 1) == RDNA3_OK);
    assert(m.nmsg == 1 && m.msgs[0] == 0x12 && !m.uploads);
    for (i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        pm_fixture(&d, &m, &v);
        w32(m.combo + 18, caps[i]);
        assert(smu13_pm_init(&d, &s, &v, ~0ull, 3, 6, 0) == RDNA3_OK);
        assert(s.navi33.maco_support == (caps[i] == 0x19));
    }
    pm_fixture(&d, &m, &v);
    m.rom[0x404 + 188] = m.rom[0x404 + 189] = 0;
    assert(smu13_pm_init(&d, &s, &v, ~0ull, 3, 6, 0) == RDNA3_OK && !s.navi33.maco_support);
    for (i = 0; i < 2; i++) {
        pm_fixture(&d, &m, &v);
        if (!i) m.enabled &= ~(1ull << 30);
        /* Deliberately contradict FeaturesToRun: it cannot prove fan topology. */
        w32(m.combo + 832 + 4, i ? 0 : 1u << 30);
        assert(smu13_pm_init(&d, &s, &v, ~0ull, 3, 6, 0) == RDNA3_OK);
        assert(smu13_pm_query(&d, &s, &v, 0) == RDNA3_OK);
        memset(&rq, 0, sizeof(rq)); rq.abi = RDNA3_PMCTL_ABI; rq.op = RD_PMCTL_INFO;
        assert(smu13_pmctl(&d, &s, &rq, &rp, ~0u) == RDNA3_OK);
        assert(rp.info.no_fan == !i && rp.info.fan_rpm == 1456);
        assert(rp.info.fan_mode == (i ? RD_FAN_CTRL_AUTO : UINT32_MAX));
        assert(smu13_get_fan_control_mode(&d, &s) == rp.info.fan_mode);
        assert(smu13_set_fan_control_mode(&d, &s, RD_FAN_CTRL_MANUAL) == RDNA3_EOPNOTSUPP);
    }
    for (i = 0; i < 2; i++) {
        pm_fixture(&d, &m, &v);
        assert(smu13_pm_init(&d, &s, &v, RD_SMU_FEATURES_SAFE, 3, 6, (int)i) == RDNA3_OK);
        assert(s.allowed == (0xa3a471fe7fffull & RD_SMU_FEATURES_SAFE));
        assert(s.allowed & (1ull << 30));
        assert(msg_index(&m, 0x1b, (2u << 16) | 1200) == UINT32_MAX);
        assert(msg_index(&m, 0x1b, (3u << 16) | 1800) == UINT32_MAX);
    }
    for (i = 0; i < 2; i++) {
        uint64_t allowed = i ? ~0ull & ~(1ull << 24) : ~0ull;
        pm_fixture(&d, &m, &v);
        m.enabled &= ~(1ull << 24);
        assert(smu13_pm_init(&d, &s, &v, allowed, 3, 6, 0) == RDNA3_OK);
        assert(s.ppt_limit_w == UINT32_MAX && s.current_power_limit == UINT32_MAX);
        assert(smu13_irq_process(&d, &s, RD_IH_CLIENT_MP1, 0xfe, RD_SMU_IH_CTX_AC) == RD_SMU_IRQ_NONE);
        assert(smu13_pm_query(&d, &s, &v, 0) == RDNA3_OK && s.metrics_valid);
        memset(&rq, 0, sizeof(rq)); rq.abi = RDNA3_PMCTL_ABI; rq.op = RD_PMCTL_INFO;
        assert(smu13_pmctl(&d, &s, &rq, &rp, ~0u) == RDNA3_OK && rp.info.pm_up);
        assert(rp.info.power_current == UINT32_MAX && rp.info.power_default == UINT32_MAX);
        assert(rp.info.power_min == UINT32_MAX && rp.info.power_max == UINT32_MAX);
        assert(rp.info.metrics.average_socket_power == 87 && smu13_metric_temp_hotspot_c(&s) == 64);
        assert(msg_index(&m, 0x33, 0) == UINT32_MAX);
        assert(smu13_get_power_limit(&d, &s, &rp.info.power_current) == RDNA3_EOPNOTSUPP);
    }
    pm_fixture(&d, &m, &v);
    assert(smu13_pm_init(&d, &s, &v, ~0ull, 3, 6, 0) == RDNA3_OK);
    assert(smu13_irq_process(&d, &s, RD_IH_CLIENT_MP1, 0xfe, RD_SMU_IH_CTX_DC) == RD_SMU_IRQ_NONE);
    assert(smu13_pm_query(&d, &s, &v, 0) == RDNA3_OK);
    assert(s.current_power_limit == 77 && s.power_limit_max == 110);
    assert(msg_index(&m, 0x33, 0x10000) != UINT32_MAX && msg_index(&m, 0x35, 1) == UINT32_MAX);
}

static void test_metrics_cache(uint8_t rev)
{
    rdna3_dev d;
    rdna3_smu s;
    rdna3_vbios v;
    rdna3_pmctl_req rq;
    rdna3_pmctl_reply rp;
    model m;
    uint64_t stamp = 1, generation;
    uint32_t n;
    int ret;
    if (rev == 7) {
        pm_fixture(&d, &m, &v);
        assert(smu13_pm_init(&d, &s, &v, ~0ull, 3, 6, 0) == RDNA3_OK);
    } else {
        fixture(&d, &m, rev);
        memset(&s, 0, sizeof(s));
        s.mp1_rev = rev; s.driver_table = 4096;
        m.pm_model = 1; m.driver_mc = 4096;
        /* The common metrics prefix has identical v6.12 offsets in both ABIs. */
        w16(m.metric_bytes + 122, 64);
    }
    assert(smu13_read_metrics_cached(&d, &s, 10, 0, &stamp) == RDNA3_OK && stamp == 10);
    generation = s.metrics_generation; n = m.nmsg;
    assert(smu13_read_metrics_cached(&d, &s, 11, 1, &stamp) == RDNA3_OK && m.nmsg == n);
    memset(&rq, 0, sizeof(rq)); rq.abi = RDNA3_PMCTL_ABI; rq.op = RD_PMCTL_INFO;
    m.fail_msg = 0x12; m.fail_param = 5;
    ret = smu13_pmctl(&d, &s, &rq, &rp, ~0u);
    if (rev == 7) assert(ret == RDNA3_EIO && !rp.info.pm_up);
    assert(!s.metrics_valid && s.metrics_generation == generation);
    stamp = smu13_metrics_after_control(&s, generation, ret, 12);
    assert(!stamp);
    /* stats and sensor both call this production cache reader. A recent
     * timestamp cannot hide the failed INFO transfer or expose old bytes. */
    stamp = 12; n = m.nmsg;
    assert(smu13_read_metrics_cached(&d, &s, 13, 1, &stamp) == RDNA3_EIO && !stamp && m.nmsg == n + 1);
    stamp = 13;
    assert(smu13_read_metrics_cached(&d, &s, 14, 1, &stamp) == RDNA3_EIO && !stamp);
    ret = smu13_pmctl(&d, &s, &rq, &rp, ~0u);
    if (rev == 7) assert(ret == RDNA3_EIO && !rp.info.pm_up);
    m.fail_msg = 0; w16(m.metric_bytes + 122, 81);
    assert(smu13_read_metrics_cached(&d, &s, 15, 0, &stamp) == RDNA3_OK && stamp == 15);
    assert(smu13_metric_temp_hotspot_c(&s) == 81);
    generation = s.metrics_generation; n = m.nmsg;
    assert(smu13_read_metrics_cached(&d, &s, 16, 1, &stamp) == RDNA3_OK && m.nmsg == n);
    assert(smu13_metrics_after_control(&s, generation, RDNA3_OK, 17) == 0); /* no read */
    ret = smu13_pmctl(&d, &s, &rq, &rp, ~0u);
    assert(ret == RDNA3_OK && s.metrics_valid && s.metrics_generation > generation);
    assert(smu13_metrics_after_control(&s, generation, ret, 18) == 18);
    assert(smu13_metrics_after_control(&s, generation, RDNA3_EIO, 19) == 0);
}

static void test_clock_registers(void)
{
    const uint8_t revisions[] = {0, 7, 10};
    rdna3_dev d;
    rdna3_dcn_clocks clocks, before;
    model m;
    unsigned i, j;
    for (i = 0; i < sizeof(revisions) / sizeof(revisions[0]); i++) {
        uint32_t right = revisions[i] == 7 ? 0x16e64 : 0x16e69;
        uint32_t wrong = revisions[i] == 7 ? 0x16e69 : 0x16e64;
        fixture(&d, &m, revisions[i]);
        m.reg[0x16e37] = 30; /* PLL 30 * 100 MHz, same in both Linux groups. */
        for (j = 0; j < 5; j++) {
            m.reg[right + 3 * j] = 0x10 + 4 * j;
            m.reg[wrong + 3 * j] = 0x7f; /* wrong group cannot carry the requested mode */
        }
        assert(dcn32_clk_read(&d, &clocks) == RDNA3_OK && m.reads == 6);
        assert(clocks.vco_khz == 3000000 && clocks.dispclk_khz == 750000);
        assert(clocks.dppclk_khz == 600000 && clocks.dprefclk_khz == 500000);
        assert(clocks.dcfclk_khz == 428571 && clocks.dtbclk_khz == 375000);
        assert(dcn32_clk_fits(&clocks, 500000) == RDNA3_OK);
        assert(dcn32_clk_raise(&d, 0x526100, 0, 500000, 500000, &clocks) == RDNA3_OK);
        assert(!m.ndal && !m.writes); /* sufficient clocks: no spurious hard-min/DTO writes */
    }
    for (i = 0; i < 4; i++) {
        fixture(&d, &m, 7);
        if (i == 0) d.ipver[HWIP_GC].present = 0;
        if (i == 1) d.ipver[HWIP_GC].major = 12;
        if (i == 2) d.ipver[HWIP_GC].minor = 1;
        if (i == 3) d.ipver[HWIP_GC].rev = 9;
        memset(&clocks, 0xa5, sizeof(clocks)); before = clocks;
        assert(dcn32_clk_read(&d, &clocks) == RDNA3_ENODEV && !memcmp(&clocks, &before, sizeof(clocks)));
        assert(dcn32_clk_raise(&d, 0x526100, 0, 500000, 500000, &clocks) == RDNA3_ENODEV);
        assert(!m.reads && !m.writes && !m.ndal && strstr(m.log, "unsupported GC discovery") != NULL);
    }
    fixture(&d, &m, 7);
    m.reg[0x16e37] = 30;
    m.reg[0x16e64] = 0x7f; m.reg[0x16e67] = 0x10;
    m.reg[0x16e69] = 0x10; /* the wrong 320 register would bypass the divider-127 refusal */
    assert(dcn32_clk_raise(&d, 0x526100, 0, 500000, 500000, NULL) == RDNA3_EBUSY);
    assert(!m.ndal && !m.writes);
}

static void test_navi33_pm(void)
{
    int scpm, exact;
    assert(smu13_0_7_allowed_features(0, 0) == 0xa38471f47061ull);
    assert(smu13_0_7_allowed_features(0xfff7bfffu, 0) == 0xa3a471fe7fffull);
    assert((smu13_0_7_allowed_features(0, 1) ^ smu13_0_7_allowed_features(0, 0)) == (1ull << 48));
    for (scpm = 0; scpm < 2; scpm++)
        for (exact = 0; exact < 2; exact++) test_pm7_success(scpm, exact);
    test_pm7_bad_input();
    test_boot_values_35();
    test_pm7_failure_order();
    test_pm7_dal_gate();
    test_pm7_external_contract();
    test_metrics_cache(7);
    test_metrics_cache(10);
    test_clock_registers();
}
