/*
 * sim_navi32.c - behavioural model of the Navi 32 registers that Stage 2
 * bring-up touches, plus an end-to-end test of hw/psp13.c + hw/smu13.c.
 *
 * The model follows the protocol as amdgpu drives it (not AMD internals):
 *   - PSP bootloader: C2PMSG_35 bit31 = ready; a write of a command loads
 *     the bytes at MC(C2PMSG_36 << 20) and sets bit31 again. SOSDRV makes
 *     the sOS alive (C2PMSG_81 != 0) and ready for ring creation
 *     (C2PMSG_64 = 0x80000000).
 *   - sOS: C2PMSG_64 = KM<<16 creates the ring at C2PMSG_69/70; each
 *     C2PMSG_67 (wptr) write consumes one psp_gfx_rb_frame, executes the
 *     command found at its MC address and writes the fence.
 *   - PMFW (MP1) answers mailbox messages once LOAD_IP_FW(SMU) happened.
 * It checks the ordering rules amdgpu relies on and that the CPU flushed
 * HDP before every doorbell-like register write.
 *
 *   sim_navi32 [linux-firmware/amdgpu dir]
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "psp13.h"
#include "smu13.h"
#include "gmc11.h"
#include "gen/gc_11_0_3_gmc.h"
#include "navi3x_regs.h"
#include "psp_gfx_if.h"
#include "amdgpu_ucode_hdr.h"

static int failures;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
    failures++; } } while (0)

/* ----------------------------- model ----------------------------- */

#define MMIO_LEN   (1u << 20)
#define VRAM_SIZE  (16ull << 30)       /* what MEMSIZE reports */
#define BAR_LEN    (64u << 20)         /* CPU-visible window we model */
#define FB_START   0x8000000000ull     /* MC base like real dGPUs */

#define MP0_BASE   0x16000u
#define MP1_BASE   0x16000u
#define GC_BASE0   0x1260u
#define NBIO_B0    0x0u
#define NBIO_B1    0x14u
#define NBIO_B2    0xD20u

typedef struct sim {
    uint32_t reg[MMIO_LEN / 4];
    uint8_t *vram;
    int verbose;

    /* bootloader log */
    uint32_t bl_cmds[16];
    unsigned nbl;
    int sos_alive;

    /* sOS */
    uint64_t ring_mc;
    uint32_t rptr_dw;
    uint32_t ip_fw_types[64];
    unsigned nipfw;
    int tmr_setup, toc_loaded, autoload, autoload_after_rlc_g, smu_loaded;
    int tmr_after_smu;
    int pending_writes;   /* CPU VRAM writes not yet flushed via HDP */
    int unflushed_doorbells;
    uint32_t smu_msgs;
    /* virtual time: the sOS needs ~50 ms to boot after SOSDRV, so that
     * psp_v13_0_bootloader_load_sos()'s "wait for C2PMSG_81 to change"
     * (read after a 20 ms delay) sees the transition like on hardware */
    uint64_t now_us;
    uint64_t sos_boot_at;
    unsigned tlb_flushes;
} sim;

static sim S;

static uint32_t *R(uint32_t dw) { return &S.reg[dw]; }
#define MP0(n) (MP0_BASE + regMP0_SMN_C2PMSG_##n)
#define MP1(n) (MP1_BASE + regMP1_SMN_C2PMSG_##n)

static uint8_t *mc_ptr(uint64_t mc, uint64_t len)
{
    if (mc < FB_START || mc - FB_START + len > BAR_LEN)
        return NULL;
    return S.vram + (mc - FB_START);
}

static void psp_exec(uint64_t cmd_mc, uint64_t fence_mc, uint32_t fence_val)
{
    struct psp_gfx_cmd_resp *cmd = (struct psp_gfx_cmd_resp *)mc_ptr(cmd_mc, sizeof(*cmd));
    uint32_t *fence = (uint32_t *)mc_ptr(fence_mc, 4);
    if (!cmd || !fence) {
        fprintf(stderr, "sim: frame points outside VRAM\n");
        failures++;
        return;
    }
    memset(&cmd->resp, 0, sizeof(cmd->resp));
    switch (cmd->cmd_id) {
    case GFX_CMD_ID_LOAD_TOC:
        S.toc_loaded = 1;
        cmd->resp.tmr_size = 0x600000;
        break;
    case GFX_CMD_ID_SETUP_TMR:
        S.tmr_setup = 1;
        S.tmr_after_smu = S.smu_loaded;
        if (!cmd->cmd.cmd_setup_tmr.bitfield.virt_phy_addr || cmd->cmd.cmd_setup_tmr.buf_size != 0x600000)
            cmd->resp.status = 0xFFFF0001;
        break;
    case GFX_CMD_ID_LOAD_IP_FW: {
        uint32_t t = cmd->cmd.cmd_load_ip_fw.fw_type;
        uint64_t mc = cmd->cmd.cmd_load_ip_fw.fw_phy_addr_lo |
                      ((uint64_t)cmd->cmd.cmd_load_ip_fw.fw_phy_addr_hi << 32);
        if (!mc_ptr(mc, cmd->cmd.cmd_load_ip_fw.fw_size) || (mc & 4095))
            cmd->resp.status = 0xFFFF0002;
        if (S.nipfw < 64)
            S.ip_fw_types[S.nipfw++] = t;
        if (t == GFX_FW_TYPE_SMU) {
            S.smu_loaded = 1;
            *R(MP1(90)) = 1; /* PMFW up, mailbox idle */
        }
        break;
    }
    case GFX_CMD_ID_AUTOLOAD_RLC:
        S.autoload = 1;
        S.autoload_after_rlc_g = S.nipfw && S.ip_fw_types[S.nipfw - 1] == GFX_FW_TYPE_RLC_G;
        break;
    default:
        cmd->resp.status = 0x100; /* PSP_ERR_UNKNOWN_COMMAND */
    }
    *fence = fence_val;
}

static void sim_wr(void *ctx, uint64_t off, uint32_t v)
{
    uint32_t dw = (uint32_t)(off / 4);
    (void)ctx;
    if (off >= MMIO_LEN)
        return;

    /* HDP flush through the remap hole */
    if (off == RDNA3_MMIO_REG_HOLE_OFFSET + KFD_MMIO_REMAP_HDP_MEM_FLUSH_CNTL) {
        S.pending_writes = 0;
        return;
    }

    if (dw == MP0(35)) {
        uint64_t mc = (uint64_t)*R(MP0(36)) << 20;
        if (!(*R(MP0(35)) & 0x80000000u)) {
            fprintf(stderr, "sim: bootloader command while busy\n");
            failures++;
        }
        if (S.pending_writes)
            S.unflushed_doorbells++;
        if (!mc_ptr(mc, 4096)) {
            fprintf(stderr, "sim: bootloader buffer MC 0x%llx outside VRAM\n",
                    (unsigned long long)mc);
            failures++;
        }
        if (S.nbl < 16)
            S.bl_cmds[S.nbl++] = v;
        if (v == 0x20000u) { /* SOSDRV: boots asynchronously */
            S.sos_boot_at = S.now_us + 50000;
            *R(MP0(35)) = 0; /* bootloader busy, then gone */
        } else {
            *R(MP0(35)) = 0x80000000u;
        }
        return;
    }
    if (dw == MP0(64)) {
        if (v == (2u << 16)) {
            S.ring_mc = *R(MP0(69)) | ((uint64_t)*R(MP0(70)) << 32);
            if (*R(MP0(71)) != PSP_RING_SIZE || !mc_ptr(S.ring_mc, PSP_RING_SIZE))
                *R(dw) = 0x80000001u; /* status error */
            else
                *R(dw) = 0x80000000u;
        } else if (v == GFX_CTRL_CMD_ID_DESTROY_RINGS) {
            *R(dw) = 0x80000000u;
        }
        return;
    }
    if (dw == MP0(67)) {
        /* doorbell: consume frames from rptr to new wptr */
        if (S.pending_writes)
            S.unflushed_doorbells++;
        while (S.rptr_dw != v) {
            struct psp_gfx_rb_frame *f =
                (struct psp_gfx_rb_frame *)mc_ptr(S.ring_mc + S.rptr_dw * 4u, sizeof(*f));
            if (!f)
                break;
            psp_exec(f->cmd_buf_addr_lo | ((uint64_t)f->cmd_buf_addr_hi << 32),
                     f->fence_addr_lo | ((uint64_t)f->fence_addr_hi << 32), f->fence_value);
            S.rptr_dw = (S.rptr_dw + 16) % (PSP_RING_SIZE / 4);
        }
        *R(dw) = v;
        return;
    }
    /* GFXHUB invalidation engine 17: acknowledge the requested VMIDs */
    {
        uint32_t eng_dist = regGCVM_INVALIDATE_ENG1_REQ - regGCVM_INVALIDATE_ENG0_REQ;
        if (dw == GC_BASE0 + regGCVM_INVALIDATE_ENG0_REQ + 17 * eng_dist) {
            S.reg[GC_BASE0 + regGCVM_INVALIDATE_ENG0_ACK + 17 * eng_dist] = v & 0xffff;
            S.tlb_flushes++;
            if (S.pending_writes)
                S.unflushed_doorbells++;
            *R(dw) = v;
            return;
        }
    }
    if (dw == MP1(66)) {
        uint32_t arg = *R(MP1(82));
        S.smu_msgs++;
        switch (v) {
        case PPSMC_MSG_TestMessage:      *R(MP1(82)) = arg + 1; break;
        case PPSMC_MSG_GetSmuVersion:    *R(MP1(82)) = 0x00526100; break;
        case PPSMC_MSG_GetDriverIfVersion: *R(MP1(82)) = 0x35; break;
        default: *R(MP1(90)) = PPSMC_Result_UnknownCmd; return;
        }
        *R(MP1(90)) = PPSMC_Result_OK;
        return;
    }
    *R(dw) = v;
}

static void sim_tick(void)
{
    if (S.sos_boot_at && S.now_us >= S.sos_boot_at) {
        S.sos_boot_at = 0;
        S.sos_alive = 1;
        *R(MP0(81)) = 0x1;
        *R(MP0(58)) = 0x00330024;
        *R(MP0(64)) = 0x80000000u;
    }
}

static uint32_t sim_rd(void *ctx, uint64_t off)
{
    (void)ctx;
    sim_tick();
    if (off >= MMIO_LEN)
        return ~0u;
    return S.reg[off / 4];
}

static void sim_udelay(void *c, uint32_t us) { (void)c; S.now_us += us; }
static void sim_msleep(void *c, uint32_t ms) { (void)c; S.now_us += 1000ull * ms; }
static void sim_wmb(void *c) { (void)c; }
static void sim_vlog(void *c, int lvl, const char *fmt, va_list ap)
{
    (void)c;
    if (!S.verbose && lvl > RDNA3_LOG_ERR)
        return;
    fputs(lvl == RDNA3_LOG_ERR ? "  [err] " : "  ", stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
}

static void sim_vram_written(void *c, uint64_t off, uint64_t len)
{
    (void)c; (void)off; (void)len;
    S.pending_writes = 1;
}

/* ------------------------- fixtures ------------------------- */

static void make_discovery(amd_discovery *disc)
{
    memset(disc, 0, sizeof(*disc));
#define IP(hw, ma, mi, re, b0, b1, b2) do { amd_ip_entry *e = &disc->ips[disc->num_ips++]; \
    e->hw_id = hw; e->major = ma; e->minor = mi; e->revision = re; \
    e->num_base = e->num_base_kept = 3; e->base[0] = b0; e->base[1] = b1; e->base[2] = b2; } while (0)
    IP(AMD_HWID_GC, 11, 0, 3, GC_BASE0, 0xA000, 0x1C000);
    IP(AMD_HWID_MP0, 13, 0, 7, MP0_BASE, 0, 0);
    IP(AMD_HWID_MP1, 13, 0, 7, MP1_BASE, 0, 0);
    IP(AMD_HWID_NBIF, 4, 3, 0, NBIO_B0, NBIO_B1, NBIO_B2);
    IP(AMD_HWID_HDP, 6, 0, 0, 0xF20, 0, 0);
    disc->num_dies = 1;
}

static uint8_t *slurp(const char *dir, const char *name, size_t *len)
{
    char path[1024];
    FILE *f;
    uint8_t *b;
    long n;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (!(f = fopen(path, "rb")))
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

/* minimal synthetic firmware: SOS v2 (sys, sos, kdb, toc) + one blob per IP */
static uint8_t syn_sos[16384], syn_blob[AMD_BLOB_COUNT][8192];

static void w32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static void synth(amd_fw_blob_ref blobs[AMD_BLOB_COUNT], const uint8_t **sos, size_t *sos_len)
{
    unsigned i;
    uint32_t types[4][3] = { { 2, 0, 1024 }, { 1, 1024, 2048 }, { 3, 3072, 512 }, { 4, 3584, 256 } };
    memset(syn_sos, 0xA5, sizeof(syn_sos));
    w32(syn_sos + 0, 256 + 4096); w32(syn_sos + 4, 256);
    syn_sos[8] = 2; syn_sos[9] = 0; syn_sos[10] = 0; syn_sos[11] = 0;
    w32(syn_sos + 24, 256);
    w32(syn_sos + 32, 4);
    for (i = 0; i < 4; i++) {
        w32(syn_sos + 36 + 16 * i, types[i][0]);
        w32(syn_sos + 40 + 16 * i, 1);
        w32(syn_sos + 44 + 16 * i, types[i][1]);
        w32(syn_sos + 48 + 16 * i, types[i][2]);
    }
    *sos = syn_sos;
    *sos_len = 256 + 4096;

    for (i = 0; i < AMD_BLOB_COUNT; i++) {
        uint8_t *b = syn_blob[i];
        memset(b, (int)(0x10 + i), 8192);
        memset(b, 0, 256);
        w32(b + 0, 8192); w32(b + 4, 256);
        b[8] = 1;
        w32(b + 20, 1024); w32(b + 24, 256);
        blobs[i].data = b;
        blobs[i].len = 8192;
    }
    /* header versions the plan builder insists on */
    syn_blob[AMD_BLOB_SDMA][8] = 2;
    w32(syn_blob[AMD_BLOB_SDMA] + offsetof(struct sdma_firmware_header_v2_0, ctx_ucode_size_bytes), 512);
    w32(syn_blob[AMD_BLOB_SDMA] + offsetof(struct sdma_firmware_header_v2_0, ctl_ucode_offset), 2048);
    w32(syn_blob[AMD_BLOB_SDMA] + offsetof(struct sdma_firmware_header_v2_0, ctl_ucode_size_bytes), 512);
    for (i = AMD_BLOB_PFP; i <= AMD_BLOB_MEC; i++) {
        syn_blob[i][8] = 2;
        w32(syn_blob[i] + offsetof(struct gfx_firmware_header_v2_0, ucode_size_bytes), 1024);
        w32(syn_blob[i] + offsetof(struct gfx_firmware_header_v2_0, data_size_bytes), 512);
        w32(syn_blob[i] + offsetof(struct gfx_firmware_header_v2_0, data_offset_bytes), 4096);
    }
    syn_blob[AMD_BLOB_RLC][8] = 2;
    w32(syn_blob[AMD_BLOB_IMU] + offsetof(struct imu_firmware_header_v1_0, imu_iram_ucode_size_bytes), 512);
    w32(syn_blob[AMD_BLOB_IMU] + offsetof(struct imu_firmware_header_v1_0, imu_dram_ucode_size_bytes), 512);
    for (i = AMD_BLOB_MES; i <= AMD_BLOB_MES1; i++) {
        w32(syn_blob[i] + offsetof(struct mes_firmware_header_v1_0, mes_ucode_size_bytes), 1024);
        w32(syn_blob[i] + offsetof(struct mes_firmware_header_v1_0, mes_ucode_offset_bytes), 256);
        w32(syn_blob[i] + offsetof(struct mes_firmware_header_v1_0, mes_ucode_data_size_bytes), 1024);
        w32(syn_blob[i] + offsetof(struct mes_firmware_header_v1_0, mes_ucode_data_offset_bytes), 2048);
    }
    w32(syn_blob[AMD_BLOB_DMCUB] + offsetof(struct dmcub_firmware_header_v1_0, inst_const_bytes), 1024);
}

/* ---------------------------- test ---------------------------- */

static void run(const char *fwdir)
{
    static amd_ucode_plan plan;
    static rdna3_psp psp;
    static rdna3_dev dev;
    amd_fw_blob_ref blobs[AMD_BLOB_COUNT];
    amd_discovery disc;
    rdna3_os os = { NULL, sim_udelay, sim_msleep, sim_vlog, sim_wmb, sim_rd, sim_wr,
                     sim_vram_written };
    const uint8_t *sos = NULL;
    size_t sos_len = 0;
    uint32_t smu_ver = 0, ifv = 0;
    unsigned i;
    int ret;

    memset(&S, 0, sizeof(S));
    S.verbose = getenv("SIM_VERBOSE") != NULL;
    S.vram = calloc(1, BAR_LEN);
    *R(MP0(35)) = 0x80000000u;  /* bootloader ready */
    *R(MP0(33)) = 0x80000000u;  /* IFWI done */
    *R(mmRCC_CONFIG_MEMSIZE) = (uint32_t)(VRAM_SIZE >> 20);
    *R(GC_BASE0 + regGCMC_VM_FB_LOCATION_BASE) = (uint32_t)(FB_START >> 24);
    *R(GC_BASE0 + regGCMC_VM_FB_LOCATION_TOP) = (uint32_t)((FB_START + VRAM_SIZE - 1) >> 24);
    *R(GC_BASE0 + regGCMC_VM_FB_OFFSET) = 0;

    memset(blobs, 0, sizeof(blobs));
    if (fwdir) {
        sos = slurp(fwdir, amd_navi32_sos_name, &sos_len);
        for (i = 0; i < AMD_BLOB_COUNT; i++)
            blobs[i].data = slurp(fwdir, amd_navi32_fw_names[i], &blobs[i].len);
        CHECK(sos != NULL);
        if (!sos)
            return;
    } else {
        synth(blobs, &sos, &sos_len);
    }
    CHECK(amd_ucode_plan_build(blobs, &plan) == AMD_FW_OK);

    rdna3_dev_init(&dev, &os, MMIO_LEN);
    dev.usec_timeout = 100000; /* amdgpu default; virtual time is free */
    dev.vram_size = VRAM_SIZE;
    dev.vram_cpu = S.vram;
    dev.vram_cpu_len = BAR_LEN;
    make_discovery(&disc);
    CHECK(rdna3_dev_set_discovery(&dev, &disc) == RDNA3_OK);
    CHECK(rdna3_read_fb_location(&dev) == RDNA3_OK);
    CHECK(dev.fb_start == FB_START);
    rdna3_nbio_remap_hdp(&dev);
    CHECK(S.reg[NBIO_B2 + regBIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL] == RDNA3_MMIO_REG_HOLE_OFFSET);
    rdna3_vram_alloc_init(&dev, 16u << 20, BAR_LEN);

    CHECK(psp13_init(&psp, &dev, sos, sos_len) == RDNA3_OK);
    ret = psp13_hw_init(&psp, &plan, blobs);
    CHECK(ret == RDNA3_OK);

    /* bootloader order mirrors psp_hw_start(): KDB, SPL(kdb), SYS, SOS */
    CHECK(S.nbl >= 3);
    CHECK(S.bl_cmds[0] == 0x80000u);
    CHECK(S.bl_cmds[S.nbl - 1] == 0x20000u);
    CHECK(S.sos_alive && psp.sos_fw_version == 0x00330024);
    CHECK(S.toc_loaded && S.tmr_setup && S.tmr_after_smu);
    CHECK(S.nipfw == plan.count);
    CHECK(S.ip_fw_types[0] == GFX_FW_TYPE_SMU);
    CHECK(S.autoload && S.autoload_after_rlc_g);
    CHECK(S.unflushed_doorbells == 0);

    CHECK(smu13_hello(&dev, &smu_ver, &ifv) == RDNA3_OK);
    CHECK(smu_ver == 0x00526100 && ifv == 0x35);
    CHECK(smu13_send_msg(&dev, 0x77, 0, NULL) != RDNA3_OK);

    /* GMC: GART in VRAM, GFXHUB VMID0 + contexts, TLB flush */
    {
        static rdna3_gmc gmc;
        uint64_t pte = 0;
        CHECK(gmc11_init(&dev, &gmc, 0) == RDNA3_OK);
        CHECK(gmc.gart_start == 0 && gmc.gart_size == (512ull << 20));
        CHECK(gfxhub303_gart_enable(&dev, &gmc) == RDNA3_OK);
        gfxhub303_set_fault_enable_default(&dev, 1);
        CHECK(S.reg[GC_BASE0 + regGCVM_CONTEXT0_CNTL] & GCVM_CONTEXT0_CNTL__ENABLE_CONTEXT_MASK);
        CHECK(S.reg[GC_BASE0 + regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32] ==
              (uint32_t)(rdna3_vram_pa(&dev, gmc.gart_table) | 1));
        CHECK(S.reg[GC_BASE0 + regGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32] ==
              (uint32_t)(gmc.gart_end >> 12));
        CHECK((S.reg[GC_BASE0 + regGCVM_CONTEXT1_CNTL] & GCVM_CONTEXT1_CNTL__PAGE_TABLE_DEPTH_MASK) >>
              GCVM_CONTEXT1_CNTL__PAGE_TABLE_DEPTH__SHIFT == 3);
        CHECK(S.reg[GC_BASE0 + regGCVM_L2_CNTL] & GCVM_L2_CNTL__ENABLE_L2_CACHE_MASK);
        CHECK(gmc11_gart_map(&dev, &gmc, 0x10000, 0x123456000ull, 2,
                             RD_PTE_VALID | RD_PTE_SYSTEM | RD_PTE_SNOOPED | RD_PTE_READABLE |
                             RD_PTE_WRITEABLE) == RDNA3_OK);
        memcpy(&pte, S.vram + gmc.gart_table + (0x10000 / 4096 + 1) * 8, 8);
        CHECK((pte & 0x0000FFFFFFFFF000ull) == 0x123457000ull && (pte & RD_PTE_VALID));
        CHECK(gfxhub303_flush_tlb(&dev, 0, 0) == RDNA3_OK && S.tlb_flushes == 1);
        CHECK(gmc11_gart_map(&dev, &gmc, gmc.gart_size, 0, 1, RD_PTE_VALID) == RDNA3_EINVAL);
        CHECK(S.unflushed_doorbells == 0);
    }

    printf("sim_navi32: %s firmware, %u bootloader cmds, %u LOAD_IP_FW, TMR %u KiB, "
           "VRAM used %llu KiB\n", fwdir ? "real" : "synthetic", S.nbl, S.nipfw,
           psp.tmr_size / 1024, (unsigned long long)((dev.alloc_next - dev.alloc_lo) / 1024));

    /* failure path: bootloader never ready -> clean timeout */
    {
        static rdna3_psp p2;
        memset(&S.reg, 0, sizeof(S.reg));
        rdna3_vram_alloc_init(&dev, 16u << 20, BAR_LEN);
        CHECK(psp13_init(&p2, &dev, sos, sos_len) == RDNA3_OK);
        CHECK(psp13_bootloader_load_all(&p2) == RDNA3_ETIME);
    }

    if (fwdir) {
        for (i = 0; i < AMD_BLOB_COUNT; i++)
            free((void *)blobs[i].data);
        free((void *)sos);
    }
    free(S.vram);
}

int main(int argc, char **argv)
{
    run(NULL);
    if (argc > 1 && argv[1][0])
        run(argv[1]);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("sim_navi32: all checks passed\n");
    return 0;
}
