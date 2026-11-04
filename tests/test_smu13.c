/* Production SMU C over an independent MMIO/SMN/VRAM model, not a real GPU.
 * Linux v6.12 smu_v13_0_7_check_fw_status / smu_v13_0_set_smu_mailbox_registers.
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "smu13.h"
#include "vcn40.h"
#include "dcn32_pstate.h"

/* The other translation unit asserts 13.0.7 independently. */
typedef char abi0_metrics_size[(sizeof(SmuMetricsExternal_t) == 392) ? 1 : -1];
typedef char abi0_serial_low[(offsetof(SmuMetrics_t, PublicSerialNumberLower) == 236) ? 1 : -1];
typedef char abi0_serial_high[(offsetof(SmuMetrics_t, PublicSerialNumberUpper) == 240) ? 1 : -1];

/* Independent Linux addresses: catch a wrong production register constant. */
enum {
    MP1_BASE = 0x1000, SMN_INDEX = 0x2000, SMN_DATA = 0x2001,
    RESP = MP1_BASE + 0x29a,
    DAL_MSG = 0x1628a, DAL_ARG = 0x16273, DAL_RESP = 0x16274,
    MMIO_DWORDS = 0x20000
};
#define FLAGS_0 0x03b10024u
#define FLAGS_7 0x03b10028u

typedef struct model {
    uint32_t reg[MMIO_DWORDS], index, flags0, flags7, mp1_base;
    uint32_t reads, writes, flag_writes, smn_reads, last_smn_read, vram_writes, waits;
    uint32_t msgs[256], params[256], nmsg, response, if_version;
    uint32_t dal_msgs[16], dal_params[16], ndal;
    char log[32768];
    size_t log_len;
    uint8_t vram[0x40000];
    uint8_t combo[4804], metric_bytes[392], uploaded[3972], rom[2048];
    uint64_t driver_mc, fb_start, enabled, allowed;
    uint32_t uploads, pm_model, fail_msg, fail_param, dpm_count, bad_clock_value;
} model;

static void pm_message(model *m, uint32_t msg, uint32_t param);

static void delay(void *ctx, uint32_t us)
{
    model *m = ctx;
    m->waits += us;
}

static void barrier(void *ctx) { (void)ctx; }

static void log_msg(void *ctx, int level, const char *fmt, va_list ap)
{
    model *m = ctx;
    size_t left = sizeof(m->log) - m->log_len;
    int n;
    (void)level;
    n = vsnprintf(m->log + m->log_len, left, fmt, ap);
    assert(n >= 0 && (size_t)n + 1 < left);
    m->log_len += (size_t)n;
    m->log[m->log_len++] = '\n';
    m->log[m->log_len] = '\0';
}

static uint32_t mmio_read(void *ctx, uint64_t off)
{
    model *m = ctx;
    uint32_t reg = (uint32_t)(off / 4);
    assert(off % 4 == 0 && off / 4 < MMIO_DWORDS);
    m->reads++;
    if (reg == SMN_INDEX)
        return m->index;
    if (reg == SMN_DATA) {
        m->smn_reads++;
        m->last_smn_read = m->index;
        assert(m->index == FLAGS_0 || m->index == FLAGS_7);
        return m->index == FLAGS_0 ? m->flags0 : m->flags7;
    }
    return m->reg[reg];
}

static void mmio_write(void *ctx, uint64_t off, uint32_t value)
{
    model *m = ctx;
    uint32_t reg = (uint32_t)(off / 4);
    assert(off % 4 == 0 && off / 4 < MMIO_DWORDS);
    m->writes++;
    if (reg == SMN_INDEX) {
        m->index = value;
        return;
    }
    if (reg == SMN_DATA) {
        assert(m->index == FLAGS_0 || m->index == FLAGS_7);
        m->flag_writes++;
        if (m->index == FLAGS_0) m->flags0 = value;
        else m->flags7 = value;
        return;
    }
    m->reg[reg] = value;
    if (reg == m->mp1_base + 0x282) {
        assert(m->nmsg < sizeof(m->msgs) / sizeof(m->msgs[0]));
        m->msgs[m->nmsg] = value;
        m->params[m->nmsg++] = m->reg[m->mp1_base + 0x292];
        if (value == 1) m->reg[m->mp1_base + 0x292]++;
        if (value == 2) m->reg[m->mp1_base + 0x292] = 0x00526100;
        if (value == 3) m->reg[m->mp1_base + 0x292] = m->if_version;
        m->reg[m->mp1_base + 0x29a] = m->response;
        if (m->pm_model) pm_message(m, value, m->params[m->nmsg - 1]);
    }
    if (reg == DAL_MSG) {
        uint32_t arg = m->reg[DAL_ARG];
        assert(m->ndal < sizeof(m->dal_msgs) / sizeof(m->dal_msgs[0]));
        m->dal_msgs[m->ndal] = value;
        m->dal_params[m->ndal++] = arg;
        if (value == 2) m->reg[DAL_ARG] = 0x00526100;
        if (value == 0xb) m->reg[DAL_ARG] = (arg & 0xff) == 0xff ? 2 : (arg & 0xff) ? 1200 : 100;
        if (value == 9) m->reg[DAL_ARG] = (arg & 0xffff) * 1000;
        if (value == 0x15) m->reg[DAL_ARG] = 0x3f;
        m->reg[DAL_RESP] = 1;
    }
}

static void vram_written(void *ctx, uint64_t off, uint64_t len)
{
    model *m = ctx;
    (void)off;
    (void)len;
    m->vram_writes++;
}

static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { w16(p, (uint16_t)v); w16(p + 2, (uint16_t)(v >> 16)); }

static int fixture_mp1(rdna3_dev *d, model *m, uint8_t rev, uint8_t mp1_bases, uint32_t mp1_base)
{
    amd_discovery disc;
    uint8_t bin[256] = {0};
    size_t p = 144;
    uint32_t i, k;
    int ret;
    rdna3_os os = { .ctx = m, .udelay = delay, .msleep = delay, .vlog = log_msg,
        .wmb = barrier, .mmio_rd = mmio_read, .mmio_wr = mmio_write,
        .vram_written = vram_written };
    memset(m, 0, sizeof(*m));
    memset(&disc, 0, sizeof(disc));
    /* Same discovery-to-device path as sim_navi32, no implicit Navi 32 IP. */
#define IP(hw, ma, re, base0) do { amd_ip_entry *e = &disc.ips[disc.num_ips++]; \
    e->hw_id = hw; e->major = ma; e->revision = re; \
    e->num_base = e->num_base_kept = 2; e->base[0] = base0; } while (0)
    IP(AMD_HWID_GC, 11, rev == 0 ? 0 : rev == 7 ? 2 : 3, 0x3000);
    IP(AMD_HWID_MP0, 13, 10, MP1_BASE);
    IP(AMD_HWID_NBIF, 4, 0, SMN_INDEX);
    disc.ips[2].base[1] = SMN_INDEX;
    IP(AMD_HWID_MP1, 13, rev, MP1_BASE);
    IP(AMD_HWID_THM, 13, 0, 0x6000);
    IP(AMD_HWID_DMU, 3, rev == 7 ? 1 : 0, 0x7000);
    disc.ips[5].minor = 2;
#undef IP
    disc.ips[3].num_base = disc.ips[3].num_base_kept = mp1_bases;
    disc.ips[3].base[0] = mp1_base;
    /* Same v1 binary / v3 IP table layout as test_discovery: real parser,
     * including num_base=0, before the production discovery-to-device boundary. */
    w32(bin, AMD_DISC_BINARY_SIGNATURE);
    w16(bin + 4, 1);
    w32(bin + 60, AMD_DISC_TABLE_SIGNATURE);
    w16(bin + 64, 3);
    w16(bin + 72, 1);
    w16(bin + 76, 140);
    w16(bin + 142, (uint16_t)disc.num_ips);
    for (i = 0; i < disc.num_ips; i++) {
        const amd_ip_entry *e = &disc.ips[i];
        w16(bin + p, e->hw_id);
        bin[p + 3] = e->num_base;
        bin[p + 4] = e->major;
        bin[p + 5] = e->minor;
        bin[p + 6] = e->revision;
        p += 8;
        for (k = 0; k < e->num_base; k++, p += 4)
            w32(bin + p, e->base[k]);
    }
    w16(bin + 66, (uint16_t)(p - 60));
    w16(bin + 12, 60);
    w16(bin + 14, amd_disc_checksum(bin + 60, p - 60));
    w16(bin + 16, (uint16_t)(p - 60));
    w16(bin + 10, (uint16_t)p);
    w16(bin + 8, amd_disc_checksum(bin + 10, p - 10));
    assert(amd_discovery_parse(bin, p, &disc) == AMD_DISC_OK);
    assert(disc.ips[3].num_base_kept == mp1_bases);
    rdna3_dev_init(d, &os, MMIO_DWORDS * 4);
    ret = rdna3_dev_set_discovery(d, &disc);
    d->usec_timeout = 4;
    d->vram_cpu = m->vram;
    d->vram_cpu_len = sizeof(m->vram);
    d->vram_size = sizeof(m->vram);
    rdna3_vram_alloc_init(d, 8, sizeof(m->vram));
    m->mp1_base = mp1_base;
    m->reg[mp1_base + 0x29a] = m->response = 1;
    m->reg[DAL_RESP] = 1;
    m->if_version = rev == 7 ? 0x35 : 0x3d;
    return ret;
}

static void fixture(rdna3_dev *d, model *m, uint8_t rev)
{
    assert(fixture_mp1(d, m, rev, 1, MP1_BASE) == RDNA3_OK);
}

static void test_status_and_hello(uint8_t rev)
{
    rdna3_dev d;
    model m;
    uint32_t fw = 0, ifv = 0;
    fixture(&d, &m, rev);
    m.flags0 = rev == 7 ? 0 : 1;
    m.flags7 = rev == 7 ? 1 : 0;
    /* hwInit calls hello directly, before the optional (unsupported on
     * 13.0.7) PM init: the status read must be part of that entry point. */
    assert(smu13_hello(&d, &fw, &ifv) == RDNA3_OK);
    assert(m.smn_reads == (rev == 7 ? 1u : 0u));
    assert(smu13_check_fw_status(&d) == RDNA3_OK);
    assert(m.smn_reads == (rev == 7 ? 2u : 1u));
    assert(m.last_smn_read == (rev == 7 ? FLAGS_7 : FLAGS_0) && !m.flag_writes);
    assert(fw == 0x00526100 && ifv == (rev == 7 ? 0x35u : 0x3du));
    assert(m.nmsg == 3 && m.msgs[0] == 1 && m.msgs[1] == 2 && m.msgs[2] == 3);
    assert(m.params[0] == 0x1234 && m.params[1] == 0 && m.params[2] == 0);
    assert(strstr(m.log, "differs") == NULL);
    if (rev == 7)
        assert(strstr(m.log, "mailbox ready; PM requires private IF 0x35 tables") != NULL);
    m.if_version = 0;
    assert(smu13_hello(&d, &fw, &ifv) == RDNA3_OK); /* Linux warns, not a new ABI port. */
    assert(strstr(m.log, rev == 7 ? "header's 0x35" : "header's 0x3d") != NULL);
    m.flags0 = m.flags7 = 0;
    assert(smu13_check_fw_status(&d) == RDNA3_EIO && m.flag_writes == 0);
    assert(strstr(m.log, rev == 7 ? "flags at 0x3b10028" : "flags at 0x3b10024") != NULL);
}

static void test_hello_status_failure(void)
{
    rdna3_dev d;
    model m;
    uint32_t fw = 0xa5a5, ifv = 0x5a5a;
    fixture(&d, &m, 7);
    m.flags0 = 1; /* the old flags and both mailbox responses look ready */
    assert(smu13_hello(&d, &fw, &ifv) == RDNA3_EIO);
    assert(fw == 0xa5a5 && ifv == 0x5a5a);
    assert(m.smn_reads == 1 && m.last_smn_read == FLAGS_7);
    assert(m.writes == 1 && !m.flag_writes); /* only the SMN index write */
    assert(!m.nmsg && !m.ndal && !m.waits && !m.vram_writes);
    assert(strstr(m.log, "flags at 0x3b10028 = 0x0, PMFW not running") != NULL);
    assert(strstr(m.log, "mailbox ready") == NULL && strstr(m.log, "SMU: PMFW") == NULL);
}

static void test_mailbox_errors(uint8_t rev)
{
    rdna3_dev d;
    model m;
    const uint32_t rejected[] = { 0xfd, 0xfc, 0xfe, 0xff };
    unsigned i;
    fixture(&d, &m, rev);
    m.reg[RESP] = 0;
    assert(smu13_send_msg(&d, 1, 0, NULL) == RDNA3_ETIME);
    assert(m.writes == 0 && m.nmsg == 0 && m.waits == d.usec_timeout);
    assert(strstr(m.log, "not ready") != NULL);
    fixture(&d, &m, rev);
    m.response = 0;
    assert(smu13_send_msg(&d, 1, 0, NULL) == RDNA3_ETIME);
    assert(m.writes == 3 && m.nmsg == 1 && m.waits == d.usec_timeout);
    assert(strstr(m.log, "timed out") != NULL);
    for (i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        fixture(&d, &m, rev);
        m.response = rejected[i];
        assert(smu13_send_msg(&d, 1, 0, NULL) == (rejected[i] == 0xfe ? RDNA3_EINVAL : RDNA3_EIO));
        assert(m.writes == 3 && m.nmsg == 1 && strstr(m.log, "-> resp") != NULL);
    }
}

static void test_pm_isolation(int unknown)
{
    rdna3_dev d;
    rdna3_smu s;
    rdna3_pmctl_req rq;
    rdna3_pmctl_reply rp;
    rdna3_uclk_pin u;
    model m;
    uint32_t v = 0, msg, exact, scpm;
    int error = unknown ? RDNA3_ENODEV : RDNA3_EOPNOTSUPP;
    int sent = -1;
    fixture(&d, &m, 7);
    if (unknown == 1) d.ipver[HWIP_MP1].present = 0;
    if (unknown == 2) d.ipver[HWIP_MP1].major = 14;
    if (unknown == 3) d.ipver[HWIP_MP1].minor = 1;
    if (unknown == 4) d.ipver[HWIP_MP1].rev = 8;
    if (unknown == 5) d.ipver[HWIP_MP1].num_base = 0;
    if (unknown) {
        assert(smu13_check_fw_status(&d) == error);
        assert(smu13_hello(&d, &v, &v) == error);
    }
    for (exact = 0; exact < 2; exact++) {
        d.amdgpu_exact = (int)exact;
        for (scpm = 0; scpm < 2; scpm++) {
            memset(&s, 0xa5, sizeof(s));
            assert(smu13_pm_init(&d, &s, NULL, ~0ull, 3, 6, (int)scpm) == error);
            assert(!s.driver_table && !s.tool_table && !s.enabled && !s.fw_status_ok);
            assert(d.alloc_next == 8 && d.nallocs == 0);
        }
    }
    s.driver_table = 8;
    s.baco_state = RD_SMU_BACO_STATE_EXIT;
    s.irq_enabled = 1; /* even stale state must not reach THM or flag clearing */
    m.vram[8] = 0x5a;
    assert(smu13_table_write(&d, &s, TABLE_PPTABLE, 0, &v, sizeof(v)) == error);
    assert(smu13_table_read(&d, &s, TABLE_SMU_METRICS, 0, &v, sizeof(v)) == error);
    assert(smu13_read_metrics(&d, &s) == error && !s.metrics_valid);
    assert(smu13_pm_query(&d, &s, NULL, 1) == error);
    assert(smu13_mode1_reset(&d) == error);
    assert(smu13_pm_fini(&d, &s) == error);
    assert(smu13_set_mp1_state(&d, &s, RD_PP_MP1_STATE_UNLOAD) == error);
    assert(smu13_disable_pmfw_state(&d) == error);
    assert(smu13_notify_display_change(&d, 0) == error);
    assert(smu13_set_vcn_enable(&d, 2, 0, 1) == error);
    assert(smu13_set_jpeg_enable(&d, 1) == error);
    /* Includes transfer, allowed-mask, feature-enable, DALNotPresent and unload IDs. */
    for (msg = 4; msg <= 0x60; msg++)
        assert(smu13_send_msg(&d, msg, 0, NULL) == error);
    memset(&rq, 0, sizeof(rq));
    rq.abi = RDNA3_PMCTL_ABI;
    for (rq.op = 0; rq.op < RD_PMCTL_OP_COUNT; rq.op++) {
        assert(smu13_pmctl(&d, &s, &rq, &rp, ~0u) == error);
        assert(rp.err == error && !rp.info.pm_up);
    }
    /* hwInit ignores the unsupported pmInit result, then the default
     * displayUclkPinInit path reaches this independent DAL mailbox. */
    assert(dcn32_uclk_pin_init(&d, &u, 0) == RDNA3_ENODEV);
    assert(!u.ready && u.last_err == error);
    assert(dcn32_uclk_pin_update(&d, &u, 2, 0, 0, &sent) == RDNA3_ENODEV && !sent);
    /* Even a stale ready object cannot bypass the shared mailbox boundary. */
    u.ready = 1;
    u.min_mhz = 100;
    u.max_mhz = 1200;
    assert(dcn32_uclk_pin_update(&d, &u, 2, 0, 0, &sent) == error);
    assert(dcn32_smu_set_hard_min(&d, RD_PPCLK_UCLK, 1200, 0x526100, NULL) == error);
    assert(dcn32_smu_num_displays(&d, 2) == error);
    assert(!m.ndal && strstr(m.log, "DAL SMU: unsupported MP1 discovery") != NULL);
    assert(m.reads == 0 && m.writes == 0 && m.nmsg == 0 && m.waits == 0);
    assert(m.flag_writes == 0 && m.vram_writes == 0 && m.vram[8] == 0x5a);
    assert(strstr(m.log, "unsupported MP1 discovery") != NULL);
}

static void test_mp1_segment(uint8_t rev)
{
    rdna3_dev d;
    model m;
    uint32_t fw = 0, ifv = 0;
    assert(fixture_mp1(&d, &m, rev, 0, MP1_BASE) == RDNA3_ENODEV);
    assert(d.ipver[HWIP_MP1].present && !d.ipver[HWIP_MP1].num_base);
    assert(strstr(m.log, "lacks register segment 0") != NULL);
    m.reg[0x29a] = 1; /* the wrong absolute address would look ready */
    assert(smu13_send_msg(&d, 1, 0, NULL) == RDNA3_ENODEV);
    assert(smu13_hello(&d, &fw, &ifv) == RDNA3_ENODEV);
    assert(smu13_check_fw_status(&d) == RDNA3_ENODEV);
    assert(dcn32_smu_send(&d, 2, 0, NULL) == RDNA3_ENODEV);
    assert(!m.reads && !m.writes && !m.nmsg && !m.ndal && !m.waits);
    /* A present segment at address zero is distinct from no segment. */
    assert(fixture_mp1(&d, &m, rev, 1, 0) == RDNA3_OK);
    assert(d.ipver[HWIP_MP1].num_base == 1 && !d.reg_base[HWIP_MP1][0][0]);
    m.flags0 = m.flags7 = 1;
    assert(smu13_hello(&d, &fw, &ifv) == RDNA3_OK && m.nmsg == 3);
    assert(fw == 0x00526100 && ifv == (rev == 7 ? 0x35u : 0x3du));
}

static void test_dal_navi32(uint8_t rev)
{
    rdna3_dev d;
    rdna3_uclk_pin u;
    model m;
    int sent = 0;
    fixture(&d, &m, rev);
    assert(dcn32_uclk_pin_init(&d, &u, 0) == RDNA3_OK);
    assert(u.ready && u.min_mhz == 100 && u.max_mhz == 1200 && u.pmfw == 0x526100);
    assert(m.ndal == 4 && m.dal_msgs[0] == 2 && m.dal_msgs[1] == 0xb &&
           m.dal_msgs[2] == 0xb && m.dal_msgs[3] == 0xb);
    assert(m.dal_params[1] == ((2u << 16) | 0xff) && m.dal_params[2] == (2u << 16) &&
           m.dal_params[3] == ((2u << 16) | 1));
    assert(dcn32_uclk_pin_update(&d, &u, 2, 0, 0, &sent) == RDNA3_OK && sent);
    assert(u.pins == 1 && u.hard_min_mhz == 1200 && m.ndal == 6);
    assert(m.dal_msgs[4] == 9 && m.dal_params[4] == ((2u << 16) | 1200));
    assert(m.dal_msgs[5] == 0x15); /* production delay variant, same guarded sender */
    assert(dcn32_uclk_pin_update(&d, &u, 1, 0, 0, &sent) == RDNA3_OK && sent);
    assert(u.releases == 1 && u.hard_min_mhz == 100 && m.ndal == 8);
    assert(m.dal_msgs[6] == 9 && m.dal_params[6] == ((2u << 16) | 100));
    assert(m.dal_msgs[7] == 0x15 && !m.waits && m.writes == 24);
}

static void test_navi32_unload(void)
{
    rdna3_dev d;
    rdna3_smu s;
    model m;
    fixture(&d, &m, 10);
    memset(&s, 0, sizeof(s));
    m.flags0 = 1;
    m.flags7 = 0x12345678;
    assert(smu13_set_mp1_state(&d, &s, RD_PP_MP1_STATE_SHUTDOWN) == RDNA3_OK && !m.writes);
    assert(smu13_set_mp1_state(&d, &s, RD_PP_MP1_STATE_UNLOAD) == RDNA3_OK);
    assert(m.nmsg == 1 && m.msgs[0] == 0x2e && m.params[0] == 0x55 && m.flag_writes == 0);
    s.baco_state = RD_SMU_BACO_STATE_EXIT;
    assert(smu13_set_mp1_state(&d, &s, RD_PP_MP1_STATE_UNLOAD) == RDNA3_OK);
    assert(m.nmsg == 2 && m.msgs[1] == 0x2e && m.params[1] == 0x55);
    assert(m.flag_writes == 1 && !m.flags0 && m.flags7 == 0x12345678 && m.last_smn_read == FLAGS_0);
    assert(smu13_send_msg(&d, PPSMC_MSG_SetAllowedFeaturesMaskLow, 0x55, NULL) == RDNA3_OK);
    assert(m.nmsg == 3 && m.msgs[2] == 4 && m.params[2] == 0x55);
}

#include "sim_smu13_0_7.inc"

int main(void)
{
    const uint8_t revisions[] = { 0, 7, 10 };
    unsigned i;
    for (i = 0; i < sizeof(revisions) / sizeof(revisions[0]); i++) {
        test_status_and_hello(revisions[i]);
        test_mailbox_errors(revisions[i]);
        test_mp1_segment(revisions[i]);
    }
    test_hello_status_failure();
    test_navi33_pm();
    for (i = 1; i <= 5; i++)
        test_pm_isolation((int)i);
    test_dal_navi32(0);
    test_dal_navi32(10);
    test_navi32_unload();
    puts("smu13: 13.0.7 PM bootstrap/telemetry, ABI and DAL model; Navi32 boundaries ok (no hardware)");
    return 0;
}
