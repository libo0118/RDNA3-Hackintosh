/*
 * Host tests for common/amd_fw.c.
 *   test_fw              synthetic headers + mutation fuzz (ASan/UBSan)
 *   test_fw <fw-dir>     additionally checks real linux-firmware Navi 32 files
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amd_fw.h"
#include "amdgpu_ucode_hdr.h"
#include "psp_gfx_if.h"

static int failures;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
    failures++; } } while (0)

static void w32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void w16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

static void common(uint8_t *b, uint32_t total, uint16_t maj, uint16_t min,
                   uint32_t usize)
{
    w32(b + 0, total);
    w32(b + 4, 256);
    w16(b + 8, maj);
    w16(b + 10, min);
    w32(b + 16, 0x1234);
    w32(b + 20, usize);
    w32(b + 24, 256);
}

/* --------------------------- synthetic --------------------------- */

static size_t make_sos_v2(uint8_t *b)
{
    struct { uint32_t type, off, size; } bins[] = {
        { 2, 0, 1000 }, { 1, 1000, 2000 }, { 4, 3000, 100 }, { 3, 3100, 200 }, { 5, 3300, 50 },
    };
    size_t total = 256 + 3350;
    unsigned i;
    memset(b, 0, total);
    common(b, (uint32_t)total, 2, 0, 3350);
    w32(b + offsetof(struct psp_firmware_header_v2_0, psp_fw_bin_count), 5);
    for (i = 0; i < 5; i++) {
        uint8_t *d = b + offsetof(struct psp_firmware_header_v2_0, psp_fw_bin) + 16 * i;
        w32(d + 0, bins[i].type);
        w32(d + 4, 0x100 + i);
        w32(d + 8, bins[i].off);
        w32(d + 12, bins[i].size);
    }
    return total;
}

static size_t make_rs64(uint8_t *b)
{
    memset(b, 0, 4096);
    common(b, 4096, 2, 0, 3000);
    w32(b + offsetof(struct gfx_firmware_header_v2_0, ucode_size_bytes), 1000);
    w32(b + offsetof(struct gfx_firmware_header_v2_0, data_size_bytes), 500);
    w32(b + offsetof(struct gfx_firmware_header_v2_0, data_offset_bytes), 2000);
    return 4096;
}

static size_t make_simple(uint8_t *b, uint16_t maj, uint16_t min)
{
    memset(b, 0, 4096);
    common(b, 4096, maj, min, 1024);
    return 4096;
}

static void synthetic(void)
{
    static uint8_t sos[8192], pfp[4096], me[4096], mec[4096], rlc[4096], imu[4096],
        mes[4096], sdma[4096], smc[4096];
    static amd_ucode_plan plan;
    amd_fw_blob_ref blobs[AMD_BLOB_COUNT];
    amd_psp_sos s;
    size_t n;

    n = make_sos_v2(sos);
    CHECK(amd_psp_sos_parse(sos, n, &s) == AMD_FW_OK);
    CHECK(s.bin[AMD_PSP_BIN_SYS_DRV].offset == 256 && s.bin[AMD_PSP_BIN_SYS_DRV].size == 1000);
    CHECK(s.bin[AMD_PSP_BIN_SOS].offset == 256 + 1000 && s.bin[AMD_PSP_BIN_SOS].size == 2000);
    CHECK(s.bin[AMD_PSP_BIN_KDB].size == 200 && s.bin[AMD_PSP_BIN_SPL].size == 50);
    CHECK(amd_psp_sos_parse(sos, 100, &s) == AMD_FW_E_TRUNCATED);
    w32(sos + offsetof(struct psp_firmware_header_v2_0, psp_fw_bin) + 12, 0xFFFFFF00u);
    CHECK(amd_psp_sos_parse(sos, n, &s) == AMD_FW_E_TRUNCATED);

    memset(blobs, 0, sizeof(blobs));
    make_rs64(pfp); make_rs64(me); make_rs64(mec);
    make_simple(rlc, 2, 0);
    make_simple(imu, 1, 0);
    w32(imu + offsetof(struct imu_firmware_header_v1_0, imu_iram_ucode_size_bytes), 100);
    w32(imu + offsetof(struct imu_firmware_header_v1_0, imu_dram_ucode_size_bytes), 200);
    make_simple(mes, 1, 0);
    w32(mes + offsetof(struct mes_firmware_header_v1_0, mes_ucode_size_bytes), 300);
    w32(mes + offsetof(struct mes_firmware_header_v1_0, mes_ucode_offset_bytes), 256);
    w32(mes + offsetof(struct mes_firmware_header_v1_0, mes_ucode_data_size_bytes), 400);
    w32(mes + offsetof(struct mes_firmware_header_v1_0, mes_ucode_data_offset_bytes), 1024);
    make_simple(sdma, 2, 0);
    w32(sdma + offsetof(struct sdma_firmware_header_v2_0, ctx_ucode_size_bytes), 128);
    w32(sdma + offsetof(struct sdma_firmware_header_v2_0, ctl_ucode_offset), 512);
    w32(sdma + offsetof(struct sdma_firmware_header_v2_0, ctl_ucode_size_bytes), 64);
    make_simple(smc, 2, 1);

#define SET(i, buf) do { blobs[i].data = buf; blobs[i].len = sizeof(buf); } while (0)
    SET(AMD_BLOB_PFP, pfp); SET(AMD_BLOB_ME, me); SET(AMD_BLOB_MEC, mec);
    SET(AMD_BLOB_RLC, rlc); SET(AMD_BLOB_IMU, imu); SET(AMD_BLOB_MES, mes);
    SET(AMD_BLOB_SDMA, sdma); SET(AMD_BLOB_SMC, smc);

    CHECK(amd_ucode_plan_build(blobs, &plan) == AMD_FW_OK);
    /* 2 sdma + 3 pfp + 3 me + 5 mec + 2 mes + 2 imu + rlc_g + smc */
    CHECK(plan.count == 19);
    {
        uint32_t i;
        for (i = 1; i < plan.count; i++)
            CHECK(plan.ucode[i - 1].ucode_id < plan.ucode[i].ucode_id);
    }
    {
        const amd_ucode *u = amd_ucode_plan_find(&plan, AMD_UCODE_IMU_D);
        CHECK(u && u->offset == 256 + 100 && u->size == 200 && u->psp_type == GFX_FW_TYPE_IMU_D);
        u = amd_ucode_plan_find(&plan, AMD_UCODE_CP_RS64_MEC_P3_STACK);
        CHECK(u && u->offset == 2000 && u->size == 500 &&
              u->psp_type == GFX_FW_TYPE_RS64_MEC_P3_STACK);
        u = amd_ucode_plan_find(&plan, AMD_UCODE_SDMA_UCODE_TH1);
        CHECK(u && u->offset == 512 && u->size == 64);
    }

    blobs[AMD_BLOB_RLC].data = NULL;
    CHECK(amd_ucode_plan_build(blobs, &plan) == AMD_FW_E_MISSING);
    SET(AMD_BLOB_RLC, rlc);

    /* fuzz: mutate headers, never crash / never read OOB */
    {
        static uint8_t work[4096];
        unsigned it, k;
        srand(7);
        for (it = 0; it < 100000; it++) {
            uint8_t *src = (it & 1) ? mec : imu;
            memcpy(work, src, sizeof(work));
            for (k = 0; k < 1 + (unsigned)(rand() % 6); k++)
                work[rand() % 128] = (uint8_t)rand();
            if (it & 1) blobs[AMD_BLOB_MEC].data = work; else blobs[AMD_BLOB_IMU].data = work;
            blobs[(it & 1) ? AMD_BLOB_MEC : AMD_BLOB_IMU].len = (size_t)(rand() % 4097);
            (void)amd_ucode_plan_build(blobs, &plan);
            (void)amd_psp_sos_parse(work, blobs[(it & 1) ? AMD_BLOB_MEC : AMD_BLOB_IMU].len, &s);
            SET(AMD_BLOB_MEC, mec); SET(AMD_BLOB_IMU, imu);
        }
    }
}

/* PSP TA container (parse_ta_v2_microcode / parse_ta_bin_descriptor) */
/* gfx_v11_0_check_fw_cp_gfx_shadow: GC 11.0.0/2/3, ME 1505, PFP 1600,
 * MEC 512, SR-IOV only */
static void shadow_check(void)
{
    const uint32_t gc1103 = AMD_IP_VERSION(11, 0, 3);
    CHECK(amd_gfx11_check_fw_cp_gfx_shadow(gc1103, 1505, 1600, 512, 1) == 1);
    CHECK(amd_gfx11_check_fw_cp_gfx_shadow(AMD_IP_VERSION(11, 0, 0), 1505, 1600, 512, 1) == 1);
    CHECK(amd_gfx11_check_fw_cp_gfx_shadow(AMD_IP_VERSION(11, 0, 2), 1505, 1600, 512, 1) == 1);
    CHECK(amd_gfx11_check_fw_cp_gfx_shadow(gc1103, 1505, 1600, 512, 0) == 0);   /* bare metal */
    CHECK(amd_gfx11_check_fw_cp_gfx_shadow(gc1103, 1504, 1600, 512, 1) == 0);
    CHECK(amd_gfx11_check_fw_cp_gfx_shadow(gc1103, 1505, 1599, 512, 1) == 0);
    CHECK(amd_gfx11_check_fw_cp_gfx_shadow(gc1103, 1505, 1600, 511, 1) == 0);
    CHECK(amd_gfx11_check_fw_cp_gfx_shadow(AMD_IP_VERSION(11, 0, 1), 9999, 9999, 9999, 1) == 0);
    CHECK(amd_gfx11_check_fw_cp_gfx_shadow(AMD_IP_VERSION(11, 5, 0), 9999, 9999, 9999, 1) == 0);
}

static void ta_synthetic(void)
{
    static uint8_t b[256 + 4000], work[256 + 4000];
    struct { uint32_t type, ver, off, size; } bins[] = {
        { 1, 0x21000118, 0, 2000 },      /* ASD */
        { 8, 0x00000a14, 2000, 100 },    /* XGMI_AUX: the XGMI slot off MP0 13.0.6 */
        { 4, 0x17000053, 2100, 900 },    /* HDCP */
        { 9, 0x1, 3000, 100 },           /* unknown type: skipped */
        { 3, 0x1b32000d, 3100, 900 },    /* RAS */
    };
    uint32_t ver[32], feat[32], ta[AMD_TA_MAX];
    amd_psp_ta t;
    unsigned i, it;
    memset(b, 0, sizeof(b));
    common(b, sizeof(b), 2, 0, 4000);
    w32(b + offsetof(struct ta_firmware_header_v2_0, ta_fw_bin_count), 5);
    for (i = 0; i < 5; i++) {
        uint8_t *d = b + offsetof(struct ta_firmware_header_v2_0, ta_fw_bin) + 16 * i;
        w32(d + 0, bins[i].type);
        w32(d + 4, bins[i].ver);
        w32(d + 8, bins[i].off);
        w32(d + 12, bins[i].size);
    }
    CHECK(amd_psp_ta_parse(b, sizeof(b), &t) == AMD_FW_OK);
    CHECK(t.bin[AMD_TA_ASD].offset == 256 && t.bin[AMD_TA_ASD].size == 2000);
    CHECK(t.bin[AMD_TA_XGMI].fw_version == 0xa14 && t.bin[AMD_TA_XGMI_AUX].size == 0);
    CHECK(t.bin[AMD_TA_RAS].offset == 256 + 3100 && t.bin[AMD_TA_DTM].size == 0);
    memset(ver, 0xee, sizeof(ver));
    memset(feat, 0xee, sizeof(feat));
    amd_fw_ta_versions(&t, ver, feat, ta);
    CHECK(ver[0x0d] == 0x21000118 && feat[0x0d] == 0x21000118);   /* AMDGPU_INFO_FW_ASD */
    CHECK(ta[AMD_TA_HDCP] == 0x17000053 && ta[AMD_TA_RAS] == 0x1b32000d && ta[AMD_TA_DTM] == 0 &&
          ta[AMD_TA_SECUREDISPLAY] == 0 && ta[AMD_TA_XGMI] == 0xa14);
    CHECK(ver[0x04] == 0xeeeeeeeeu);   /* other entries untouched */

    /* a descriptor past the end, too many descriptors, a v1 header */
    memcpy(work, b, sizeof(b));
    w32(work + offsetof(struct ta_firmware_header_v2_0, ta_fw_bin) + 16 * 4 + 12, 901);
    CHECK(amd_psp_ta_parse(work, sizeof(work), &t) == AMD_FW_E_TRUNCATED);
    memcpy(work, b, sizeof(b));
    w32(work + offsetof(struct ta_firmware_header_v2_0, ta_fw_bin_count), 13);
    CHECK(amd_psp_ta_parse(work, sizeof(work), &t) == AMD_FW_E_TOO_MANY);
    memcpy(work, b, sizeof(b));
    w16(work + 8, 1);
    CHECK(amd_psp_ta_parse(work, sizeof(work), &t) == AMD_FW_E_VERSION);
    /* fuzz: never read out of bounds */
    srand(11);
    for (it = 0; it < 50000; it++) {
        size_t len = (size_t)(rand() % (int)sizeof(work));
        memcpy(work, b, sizeof(b));
        for (i = 0; i < 1 + (unsigned)(rand() % 6); i++)
            work[rand() % 128] = (uint8_t)rand();
        if (amd_psp_ta_parse(work, len, &t) == AMD_FW_OK)
            for (i = 1; i < AMD_TA_MAX; i++)
                CHECK((uint64_t)t.bin[i].offset + t.bin[i].size <= len);
    }
}

/* ------------------------------ real ------------------------------ */

static uint8_t *slurp(const char *dir, const char *name, size_t *len)
{
    char path[1024];
    FILE *f;
    uint8_t *buf;
    long n;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (!(f = fopen(path, "rb")))
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static void real(const char *dir)
{
    static amd_ucode_plan plan;
    amd_fw_blob_ref blobs[AMD_BLOB_COUNT];
    amd_psp_sos s;
    size_t n = 0;
    uint8_t *sos = slurp(dir, amd_navi32_sos_name, &n);
    unsigned i;

    CHECK(sos != NULL);
    if (!sos)
        return;
    CHECK(amd_psp_sos_parse(sos, n, &s) == AMD_FW_OK);
    CHECK(s.bin[AMD_PSP_BIN_SOS].size && s.bin[AMD_PSP_BIN_SYS_DRV].size &&
          s.bin[AMD_PSP_BIN_KDB].size && s.bin[AMD_PSP_BIN_TOC].size);
    for (i = 1; i < AMD_PSP_BIN_MAX; i++)
        CHECK(s.bin[i].size <= 0x100000); /* psp_copy_fw() limit: 1 MiB */

    memset(blobs, 0, sizeof(blobs));
    for (i = 0; i < AMD_BLOB_COUNT; i++) {
        blobs[i].data = slurp(dir, amd_navi32_fw_names[i], &blobs[i].len);
        /* amdgpu_ucode_validate accepts every file of the set */
        if (blobs[i].data)
            CHECK(amd_ucode_validate(blobs[i].data, blobs[i].len) == AMD_FW_OK);
    }
    CHECK(amd_ucode_validate(sos, n) == AMD_FW_OK);
    CHECK(amd_ucode_hdr_version(blobs[AMD_BLOB_PFP].data, blobs[AMD_BLOB_PFP].len, 2, 0));   /* rs64_enable */
    CHECK(amd_ucode_plan_build(blobs, &plan) == AMD_FW_OK);
    CHECK(amd_ucode_plan_find(&plan, AMD_UCODE_RLC_G) != NULL);
    CHECK(amd_ucode_plan_find(&plan, AMD_UCODE_SMC) != NULL);
    CHECK(amd_ucode_plan_find(&plan, AMD_UCODE_IMU_I) != NULL);
    printf("test_fw: real firmware in %s: %u ucodes, %u KiB\n", dir, plan.count,
           plan.total_aligned / 1024);
    {
        /* AMDGPU_INFO_FW_VERSION as amdgpu reports it for these files */
        uint32_t ver[32], feat[32];
        amd_fw_versions(blobs, &s, ver, feat);
        CHECK(ver[0x04] && ver[0x05] && ver[0x08] && ver[0x07] && ver[0x0b]);   /* ME PFP MEC RLC SDMA */
        CHECK(ver[0x0c] && ver[0x1b] && ver[0x14]);   /* SOS IMU DMCUB; MES: 0 in the header, runtime */
        CHECK(ver[0x06] == 0 && ver[0x01] == 0);                                /* no CE, no VCE */
        printf("test_fw: versions ME 0x%x/%u PFP 0x%x/%u MEC 0x%x/%u RLC 0x%x/%u SDMA 0x%x/%u MES 0x%x/%u "
               "SOS 0x%x IMU 0x%x DMCUB 0x%x\n", ver[4], feat[4], ver[5], feat[5], ver[8], feat[8], ver[7],
               feat[7], ver[0xb], feat[0xb], ver[0x1a], feat[0x1a], ver[0xc], ver[0x1b], ver[0x14]);
        /* gfx_v11_0_check_fw_cp_gfx_shadow on these files: new enough
         * for CP gfx shadowing, which amdgpu still enables on a VF only */
        CHECK(amd_gfx11_check_fw_cp_gfx_shadow(AMD_IP_VERSION(11, 0, 3), ver[4], ver[5], ver[8], 1) == 1);
        CHECK(amd_gfx11_check_fw_cp_gfx_shadow(AMD_IP_VERSION(11, 0, 3), ver[4], ver[5], ver[8], 0) == 0);
    }
    for (i = 0; i < AMD_BLOB_COUNT; i++)
        free((void *)blobs[i].data);
    free(sos);

    /* psp_13_0_10_ta.bin: what amdgpu_firmware_info answers for
     * AMDGPU_INFO_FW_ASD and AMDGPU_INFO_FW_TA on this card */
    {
        amd_psp_ta t;
        uint32_t ver[32] = { 0 }, feat[32] = { 0 }, ta[AMD_TA_MAX];
        uint8_t *tf = slurp(dir, amd_navi32_ta_name, &n);
        if (!tf) {
            printf("test_fw: no %s in %s, TA versions not checked\n", amd_navi32_ta_name, dir);
            return;
        }
        CHECK(amd_psp_ta_parse(tf, n, &t) == AMD_FW_OK);
        amd_fw_ta_versions(&t, ver, feat, ta);
        /* the file carries ASD, HDCP, DTM and RAS (linux-firmware 20260916:
         * 0x21000118, 0x17000053, 0x1200001f, 0x1b32000d); no XGMI, RAP or
         * secure display TA, for which amdgpu reports 0 */
        CHECK(ver[0x0d] && feat[0x0d] == ver[0x0d] && ta[AMD_TA_ASD] == ver[0x0d]);
        CHECK(ta[AMD_TA_HDCP] && ta[AMD_TA_DTM] && ta[AMD_TA_RAS]);
        CHECK(ta[AMD_TA_XGMI] == 0 && ta[AMD_TA_RAP] == 0 && ta[AMD_TA_SECUREDISPLAY] == 0);
        for (i = 1; i < AMD_TA_MAX; i++)
            CHECK(t.bin[i].size <= 0x100000);
        printf("test_fw: TA ASD 0x%08x HDCP 0x%08x DTM 0x%08x RAS 0x%08x\n", ta[AMD_TA_ASD],
               ta[AMD_TA_HDCP], ta[AMD_TA_DTM], ta[AMD_TA_RAS]);
        free(tf);
    }
}

/* amdgpu_ucode_validate / _hdr_version / _ip_version_decode / _name */
static void ucode_helpers(void)
{
    uint8_t h[64];
    char pre[32];
    memset(h, 0, sizeof(h));
    h[0] = sizeof(h);            /* size_bytes */
    h[8] = 2; h[10] = 0;         /* header 2.0 */
    CHECK(amd_ucode_validate(h, sizeof(h)) == AMD_FW_OK);
    CHECK(amd_ucode_validate(h, sizeof(h) + 4) != AMD_FW_OK);
    CHECK(amd_ucode_validate(h, 2) != AMD_FW_OK && amd_ucode_validate(NULL, 8) == AMD_FW_E_ARGS);
    CHECK(amd_ucode_hdr_version(h, sizeof(h), 2, 0) && !amd_ucode_hdr_version(h, sizeof(h), 2, 1));
    h[10] = 1;
    CHECK(!amd_ucode_hdr_version(h, sizeof(h), 2, 0) && !amd_ucode_hdr_version(h, 4, 2, 1));
    /* the Navi 32 file names start with the decoded prefixes */
    CHECK(amd_ucode_ip_version_decode(AMD_UCODE_BLOCK_GC, 11, 0, 3, pre, sizeof(pre)) == 9 &&
          strcmp(pre, "gc_11_0_3") == 0 && strncmp(amd_navi32_fw_names[AMD_BLOB_PFP], pre, 9) == 0);
    CHECK(amd_ucode_ip_version_decode(AMD_UCODE_BLOCK_SDMA, 6, 0, 3, pre, sizeof(pre)) > 0 &&
          strcmp(amd_navi32_fw_names[AMD_BLOB_SDMA], "sdma_6_0_3.bin") == 0 && strcmp(pre, "sdma_6_0_3") == 0);
    CHECK(amd_ucode_ip_version_decode(AMD_UCODE_BLOCK_MP0, 13, 0, 10, pre, sizeof(pre)) == 11 &&
          strncmp(amd_navi32_sos_name, pre, 11) == 0);
    CHECK(amd_ucode_ip_version_decode(AMD_UCODE_BLOCK_MP1, 13, 0, 10, pre, sizeof(pre)) > 0 &&
          strcmp(pre, "smu_13_0_10") == 0);
    CHECK(amd_ucode_ip_version_decode(AMD_UCODE_BLOCK_VCN, 4, 0, 0, pre, sizeof(pre)) > 0 &&
          strcmp(pre, "vcn_4_0_0") == 0);
    CHECK(amd_ucode_ip_version_decode(99, 1, 0, 0, pre, sizeof(pre)) == -1);
    CHECK(amd_ucode_ip_version_decode(AMD_UCODE_BLOCK_MP0, 13, 0, 10, pre, 6) == 11 && strcmp(pre, "psp_1") == 0);
    CHECK(strcmp(amd_ucode_name(AMD_UCODE_SDMA_UCODE_TH0), "SDMA_CTX") == 0 &&
          strcmp(amd_ucode_name(AMD_UCODE_CP_MES1), "CP_MES_KIQ") == 0 &&
          strcmp(amd_ucode_name(AMD_UCODE_RLC_G), "RLC_G") == 0 &&
          strcmp(amd_ucode_name(AMD_UCODE_SMC), "SMC") == 0 && strcmp(amd_ucode_name(AMD_UCODE_DMCUB), "DMCUB") == 0 &&
          strcmp(amd_ucode_name(AMD_UCODE_CP_RS64_MEC_P3_STACK), "RS64_MEC_P3_STACK") == 0 &&
          strcmp(amd_ucode_name(500), "UNKNOWN UCODE") == 0);
}

int main(int argc, char **argv)
{
    ucode_helpers();
    synthetic();
    shadow_check();
    ta_synthetic();
    if (argc > 1 && argv[1][0])
        real(argv[1]);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_fw: all checks passed\n");
    return 0;
}
