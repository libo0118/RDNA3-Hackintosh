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
    for (i = 0; i < AMD_BLOB_COUNT; i++)
        blobs[i].data = slurp(dir, amd_navi32_fw_names[i], &blobs[i].len);
    CHECK(amd_ucode_plan_build(blobs, &plan) == AMD_FW_OK);
    CHECK(amd_ucode_plan_find(&plan, AMD_UCODE_RLC_G) != NULL);
    CHECK(amd_ucode_plan_find(&plan, AMD_UCODE_SMC) != NULL);
    CHECK(amd_ucode_plan_find(&plan, AMD_UCODE_IMU_I) != NULL);
    printf("test_fw: real firmware in %s: %u ucodes, %u KiB\n", dir, plan.count,
           plan.total_aligned / 1024);
    for (i = 0; i < AMD_BLOB_COUNT; i++)
        free((void *)blobs[i].data);
    free(sos);
}

int main(int argc, char **argv)
{
    synthetic();
    if (argc > 1 && argv[1][0])
        real(argv[1]);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_fw: all checks passed\n");
    return 0;
}
