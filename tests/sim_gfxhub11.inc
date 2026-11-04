/* Linux v6.12 gfxhub_v3_0 / gfxhub_v3_0_3, independent numeric MMIO fixtures. */
#define GH11_DWORDS 0x20000u
static struct {
    uint32_t reg[GH11_DWORDS], writes[GH11_DWORDS], reads[GH11_DWORDS];
    uint8_t expected[GH11_DWORDS];
    uint32_t base, req, ack, fault, status;
    unsigned io, vram_writes, errors, delayed;
    int no_ack, stale_status;
} gh11;

static uint32_t gh11_rd(void *ctx, uint64_t off)
{
    uint32_t dw = (uint32_t)(off / 4);
    (void)ctx;
    CHECK(dw < GH11_DWORDS);
    gh11.io++;
    if (dw >= GH11_DWORDS) return 0;
    gh11.reads[dw]++;
    if (dw == gh11.base + gh11.status && gh11.stale_status) {
        gh11.stale_status = 0;
        return 0xbad;
    }
    return gh11.reg[dw];
}

static void gh11_wr(void *ctx, uint64_t off, uint32_t value)
{
    uint32_t dw = (uint32_t)(off / 4);
    (void)ctx;
    CHECK(dw < GH11_DWORDS);
    gh11.io++;
    if (dw >= GH11_DWORDS) return;
    gh11.writes[dw]++;
    gh11.reg[dw] = value;
    if (dw == gh11.base + gh11.req + 17 && !gh11.no_ack)
        gh11.reg[gh11.base + gh11.ack + 17] = value & 0xffff;
    if (dw == gh11.base + gh11.fault && (value & 1)) {
        gh11.reg[gh11.base + gh11.status] = 0;
        gh11.reg[dw] &= ~1u;
    }
}

static void gh11_delay(void *ctx, uint32_t us) { (void)ctx; gh11.delayed += us; }
static void gh11_log(void *ctx, int level, const char *fmt, va_list ap)
{
    (void)ctx; (void)fmt; (void)ap;
    if (level == RDNA3_LOG_ERR) gh11.errors++;
}
static void gh11_vram_written(void *ctx, uint64_t off, uint64_t len)
{
    (void)ctx; (void)off; (void)len; gh11.vram_writes++;
}

static void test_gfxhub11_variant(unsigned rev, uint32_t base)
{
    /* The values come from Linux's two offset headers, not the production macros. */
    static const uint32_t enable_regs[2][27] = {
        { 0x16f3, 0x16f4, 0x1713, 0x1714, 0x1733, 0x1734,
          0x167c, 0x167b, 0x167a, 0x167d, 0x167e, 0x15a8, 0x15a9,
          0x15cb, 0x15cc, 0x15c5, 0x167f, 0x15bc, 0x15bd, 0x15be, 0x15d4,
          0x15da, 0x15ce, 0x15cf, 0x15d0, 0x15d1, 0x15d2 },
        { 0x1703, 0x1704, 0x1723, 0x1724, 0x1743, 0x1744,
          0x168c, 0x168b, 0x168a, 0x168d, 0x168e, 0x15a8, 0x15a9,
          0x15cf, 0x15d0, 0x15c9, 0x168f, 0x15c0, 0x15c1, 0x15c2, 0x15d8,
          0x15de, 0x15d2, 0x15d3, 0x15d4, 0x15d5, 0x15d6 }
    };
    unsigned v = rev == 2 ? 0 : 1, i, errors;
    uint32_t ctx = rev == 2 ? 0x1688 : 0x1698;
    uint32_t pt = rev == 2 ? 0x16f3 : 0x1703;
    uint32_t start = rev == 2 ? 0x1713 : 0x1723;
    uint32_t end = rev == 2 ? 0x1733 : 0x1743;
    uint32_t range = rev == 2 ? 0x16cf : 0x16df;
    uint32_t l1 = rev == 2 ? 0x167f : 0x168f;
    uint32_t l2 = rev == 2 ? 0x15bc : 0x15c0;
    uint32_t l23 = rev == 2 ? 0x15be : 0x15c2;
    const uint32_t irq_mask = 0x002aaa00u, saved = 0x80000100u;
    const uint64_t pd = 0x123456789001ull;
    uint64_t alloc;
    uint32_t *packet;
    rdna3_dev dev;
    rdna3_gmc gmc;
    amd_discovery disc;
    rdna3_iv iv;
    rdna3_vm_fault fault;
    rdna3_vm_flush_regs f;
    rdna3_gfx_ring gfx;
    rdna3_sdma_ring sdma;
    rdna3_os os = {0};

    memset(&gh11, 0, sizeof(gh11));
    gh11.base = base;
    gh11.req = rev == 2 ? 0x16ab : 0x16bb;
    gh11.ack = rev == 2 ? 0x16bd : 0x16cd;
    gh11.fault = rev == 2 ? 0x15c4 : 0x15c8;
    gh11.status = rev == 2 ? 0x15c8 : 0x15cc;
    os.udelay = gh11_delay; os.msleep = gh11_delay; os.vlog = gh11_log;
    os.mmio_rd = gh11_rd; os.mmio_wr = gh11_wr; os.vram_written = gh11_vram_written;
    rdna3_dev_init(&dev, &os, GH11_DWORDS * 4u);
    make_discovery(&disc);
    disc.ips[0].revision = (uint8_t)rev;
    disc.ips[0].base[0] = base;
    disc.ips[0].base[1] = 0x6000;
    disc.ips[0].num_base = disc.ips[0].num_base_kept = base ? 3 : 1;
    CHECK(rdna3_dev_set_discovery(&dev, &disc) == RDNA3_OK);
    dev.vram_cpu = calloc(1, 2u << 20);
    CHECK(dev.vram_cpu != NULL);
    if (!dev.vram_cpu) return;
    dev.vram_cpu_len = 2u << 20;
    dev.vram_size = 16ull << 30;
    dev.fb_start = 0x800000000ull;
    dev.fb_end = dev.fb_start + dev.vram_size - 1;
    dev.fb_offset = 0x100000000ull;
    dev.usec_timeout = 4;
    rdna3_vram_alloc_init(&dev, 4096, dev.vram_cpu_len);
    memset(&gmc, 0, sizeof(gmc));
    CHECK(gmc11_init(&dev, &gmc, 0) == RDNA3_OK);

    /* Level 6 enables fault IRQs before gfx11_gfxhub_enable at level 7. */
    for (i = 0; i < 16; i++) gh11.reg[base + ctx + i] = saved;
    gmc11_set_vm_fault_interrupts(&dev, 1);
    for (i = 0; i < 16; i++)
        CHECK(gh11.reg[base + ctx + i] == (saved | irq_mask) && gh11.writes[base + ctx + i] == 1);
    if (rev == 2) CHECK(gh11.writes[base + 0x1698] == 0);

    memset(gh11.writes, 0, sizeof(gh11.writes));
    gh11.reg[base + 0x1e1f] = 0x5a5a1234;
    gh11.reg[base + gh11.fault] = 0x20002002;
    if (rev == 2) {
        gh11.reg[base + 0x1698] = 0xdeadbeef;
        gh11.reg[base + 0x1703] = 0xabcdef01;
        gh11.reg[base + 0x15c0] = 0x76543210;
    }
    CHECK(gfx11_gfxhub_enable(&dev, &gmc) == RDNA3_OK && gmc.gfxhub_enabled);
    CHECK(gh11.reg[base + pt] == (uint32_t)(dev.fb_offset + gmc.gart_table + 1));
    CHECK(gh11.reg[base + pt + 1] == 1);
    CHECK(gh11.reg[base + start] == (uint32_t)(gmc.gart_start >> 12));
    CHECK(gh11.reg[base + start + 1] == (uint32_t)(gmc.gart_start >> 44));
    CHECK(gh11.reg[base + end] == (uint32_t)(gmc.gart_end >> 12));
    CHECK(gh11.reg[base + end + 1] == (uint32_t)(gmc.gart_end >> 44));
    CHECK(gh11.reg[base + ctx] == (saved | irq_mask | 1));
    CHECK(gh11.reg[base + l1] == 0x1859 && gh11.reg[base + l2] == 0x80801);
    CHECK(gh11.reg[base + l23] == 0x80130009);
    CHECK(gh11.reg[base + (rev == 2 ? 0x15d4 : 0x15d8)] == 1);
    CHECK(gh11.reg[base + (rev == 2 ? 0x15da : 0x15de)] == 0x3fe0);
    CHECK(gh11.reg[base + gh11.fault] == 0x20003ffe);
    CHECK(gh11.reg[base + gh11.req + 17] == 0x00f80001);
    CHECK(gh11.reg[base + gh11.ack + 17] == 1);
    CHECK(gh11.reg[base + 0x1e1f] == (rev == 2 ? 0x5a5a9234u : 0x5a5a1234u));
    CHECK(gh11.writes[base + 0x1e1f] == (rev == 2 ? 1u : 0u));
    for (i = 1; i < 16; i++) {
        CHECK(gh11.reg[base + ctx + i] == (saved | irq_mask | 0x555407));
        CHECK(gh11.reg[base + start + 2 * i] == 0 && gh11.reg[base + start + 2 * i + 1] == 0);
        CHECK(gh11.reg[base + end + 2 * i] == 0xffffffff && gh11.reg[base + end + 2 * i + 1] == 0xf);
    }
    for (i = 0; i < 27; i++) gh11.expected[base + enable_regs[v][i]] = 1;
    gh11.expected[base + (rev == 2 ? 0x15d3 : 0x15d7)] = 1;
    gh11.expected[base + gh11.fault] = gh11.expected[base + gh11.req + 17] = 1;
    if (rev == 2) gh11.expected[base + 0x1e1f] = 1;
    gh11.expected[dev.rmmio_remap_offset / 4] = 1;
    for (i = 0; i < 16; i++) {
        gh11.expected[base + ctx + i] = 1;
        if (i) {
            gh11.expected[base + start + 2 * i] = gh11.expected[base + start + 2 * i + 1] = 1;
            gh11.expected[base + end + 2 * i] = gh11.expected[base + end + 2 * i + 1] = 1;
        }
    }
    for (i = 0; i < 18; i++) {
        CHECK(gh11.reg[base + range + 2 * i] == 0xffffffff && gh11.reg[base + range + 2 * i + 1] == 0x1f);
        gh11.expected[base + range + 2 * i] = gh11.expected[base + range + 2 * i + 1] = 1;
    }
    for (i = 0; i < GH11_DWORDS; i++) {
        CHECK((gh11.writes[i] != 0) == (gh11.expected[i] != 0));
    }
    if (rev == 2) {
        CHECK(gh11.reg[base + 0x1698] == 0xdeadbeef && gh11.writes[base + 0x1698] == 0);
        CHECK(gh11.reg[base + 0x1703] == 0xabcdef01 && gh11.writes[base + 0x1703] == 0);
        CHECK(gh11.reg[base + 0x15c0] == 0x76543210 && gh11.writes[base + 0x15c0] == 0);
    }

    gfxhub11_setup_vm_pt_regs(&dev, 7, pd);
    CHECK(gh11.reg[base + pt + 14] == 0x56789001 && gh11.reg[base + pt + 15] == 0x1234);
    gfxhub11_flush_regs(&dev, 7, 12, &f);
    CHECK(f.pt_lo == base + pt + 14 && f.pt_hi == base + pt + 15);
    CHECK(f.req == base + gh11.req + 12 && f.ack == base + gh11.ack + 12 && f.req_val == 0x00f80080);
    CHECK(gfxhub11_flush_tlb(&dev, 7, 2) == RDNA3_OK);
    CHECK(gh11.reg[base + gh11.req + 17] == 0x00fa0080);
    gh11.no_ack = 1; gh11.reg[base + gh11.ack + 17] = 0;
    errors = gh11.errors;
    CHECK(gfxhub11_flush_tlb(&dev, 7, 0) == RDNA3_ETIME && gh11.errors == errors + 1 && gh11.delayed == 4);
    gh11.no_ack = 0;

    alloc = rdna3_vram_alloc(&dev, 4096, 4096, "gfxhub-packets");
    CHECK(alloc != UINT64_MAX);
    if (alloc != UINT64_MAX) {
        packet = (uint32_t *)(void *)(dev.vram_cpu + alloc);
        memset(&gfx, 0, sizeof(gfx));
        gfx.ring = alloc; gfx.buf_mask = 1023; gfx.kind = RD_RING_GFX; gfx.vm_inv_eng = 12;
        gfx11_emit_vm_flush(&dev, &gfx, 7, pd);
        CHECK(gfx.wptr == 19 && packet[0] == 0xc0033700 && packet[5] == 0xc0033700);
        CHECK(packet[2] == base + pt + 14 && packet[4] == 0x56789001);
        CHECK(packet[7] == base + pt + 15 && packet[9] == 0x1234);
        CHECK(packet[10] == 0xc0053c00 && packet[12] == base + gh11.req + 12);
        CHECK(packet[13] == base + gh11.ack + 12 && packet[14] == 0x00f80080 && packet[15] == 0x80);
        memset(&sdma, 0, sizeof(sdma));
        sdma.ring = alloc; sdma.buf_mask = 1023; sdma.vm_inv_eng = 12;
        sdma60_emit_vm_flush(&dev, &sdma, 7, pd);
        CHECK(sdma.wptr == 10 && packet[0] == 0xf000000e && packet[3] == 0xf000000e);
        CHECK(packet[1] == base + pt + 14 && packet[2] == 0x56789001);
        CHECK(packet[4] == base + pt + 15 && packet[5] == 0x1234);
        CHECK(packet[6] == 0x1f0c0408 && packet[7] == 0x00f80080 && packet[8] == 0xffffffff && packet[9] == 0x1f0080);
    }

    memset(&iv, 0, sizeof(iv));
    iv.client_id = RD_IH_CLIENT_GFX; iv.src_id = RD_IH_SRC_VM_FAULT;
    iv.vmid = 7; iv.pasid = 0x123; iv.src_data[0] = 0x12345678; iv.src_data[1] = 0x83;
    gh11.reg[base + gh11.status] = 1 | (5u << 1) | (0xau << 4) | (1u << 8) | (0x123u << 9) | (1u << 18);
    i = gh11.reads[base + gh11.status]; gh11.stale_status = 1;
    gmc11_decode_vm_fault(&dev, &iv, &fault);
    CHECK(fault.hub == 0 && fault.vmid == 7 && fault.pasid == 0x123 && fault.addr == 0x312345678000ull);
    CHECK(fault.cid == 0x123 && fault.write && fault.retry && fault.more_faults && fault.walker_error == 5);
    CHECK(fault.permission_faults == 0xa && fault.mapping_error && gh11.reg[base + gh11.status] == 0);
    CHECK(gh11.reads[base + gh11.status] == i + 2);
    if (rev == 2) CHECK(gh11.writes[base + 0x15c8] == 0);

    gfxhub11_set_fault_enable_default(&dev, 0);
    CHECK(gh11.reg[base + gh11.fault] == 0xe0002002);
    gfxhub11_set_fault_enable_default(&dev, 1);
    CHECK(gh11.reg[base + gh11.fault] == (rev == 2 ? 0xe0003ffeu : 0x20003ffeu));
    CHECK(gh11.reg[base + 0x1e1f] == (rev == 2 ? 0x5a5a9234u : 0x5a5a1234u));
    gmc11_set_vm_fault_interrupts(&dev, 0);
    for (i = 0; i < 16; i++) CHECK(!(gh11.reg[base + ctx + i] & irq_mask));
    gh11.reg[base + 0x15a7] = 0x123;
    CHECK(gfxhub11_get_mc_fb_offset(&dev) == 0x123000000ull);
    CHECK(gmc.gfxhub_enabled == 1);
    gfxhub11_gart_disable(&dev, &gmc);
    CHECK(gmc.gfxhub_enabled == 0);
    for (i = 0; i < 16; i++) CHECK(gh11.reg[base + ctx + i] == 0);
    CHECK(gh11.reg[base + l1] == 0x1818 && gh11.reg[base + l2] == 0x80800 && gh11.reg[base + l23] == 0);
    if (rev == 2) CHECK(gh11.writes[base + 0x1698] == 0 && gh11.writes[base + 0x1703] == 0 && gh11.writes[base + 0x15c0] == 0);
    free((void *)dev.vram_cpu);
}

static void test_gfxhub11(void)
{
    rdna3_dev dev;
    rdna3_gmc gmc;
    amd_discovery disc;
    rdna3_os os = {0};
    unsigned i;
    static const uint8_t versions[][4] = {
        { 11, 0, 2, 0 }, { 11, 0, 3, 0 }, { 12, 0, 2, 1 },
        { 11, 1, 2, 1 }, { 11, 0, 0, 1 }, { 11, 0, 1, 1 },
        { 11, 0, 4, 1 }, { 11, 0, 255, 1 }
    };
    test_gfxhub11_variant(2, 0);
    test_gfxhub11_variant(2, 0x2800);
    test_gfxhub11_variant(3, 0);
    test_gfxhub11_variant(3, 0x2800);
    os.mmio_rd = gh11_rd; os.mmio_wr = gh11_wr; os.vlog = gh11_log;
    os.vram_written = gh11_vram_written;
    for (i = 0; i <= sizeof(versions) / sizeof(versions[0]); i++) {
        int discovery_result = RDNA3_OK;
        memset(&gh11, 0, sizeof(gh11));
        rdna3_dev_init(&dev, &os, GH11_DWORDS * 4u);
        make_discovery(&disc);
        if (i == sizeof(versions) / sizeof(versions[0])) {
            disc.ips[0].hw_id = 0;
            discovery_result = RDNA3_ENODEV;
        } else {
            disc.ips[0].major = versions[i][0]; disc.ips[0].minor = versions[i][1];
            disc.ips[0].revision = versions[i][2];
            disc.ips[0].num_base = disc.ips[0].num_base_kept = versions[i][3];
        }
        CHECK(rdna3_dev_set_discovery(&dev, &disc) == discovery_result);
        gh11.io = gh11.vram_writes = gh11.errors = 0;
        dev.fb_start = 0x800000000ull; dev.fb_end = dev.fb_start + (16ull << 30) - 1;
        memset(&gmc, 0, sizeof(gmc));
        CHECK(gmc11_init(&dev, &gmc, 0) == RDNA3_ENODEV);
        CHECK(gh11.io == 0 && gh11.vram_writes == 0 && gh11.errors == 1 && dev.nallocs == 0);
    }
}
