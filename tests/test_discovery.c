/*
 * Host unit tests for common/amd_discovery.c.
 *
 * Builds synthetic discovery binaries that mimic a Navi 32 layout
 * (binary header v1 + IP table v3/32-bit, and header v2 + IP table v4/64-bit),
 * checks decoding, then checks that every corruption is rejected and that
 * random mutations never read out of bounds (run under ASan/UBSan).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amd_discovery.h"

static int failures;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
    failures++; } } while (0)

static void w16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }
static void w32(uint8_t *p, uint32_t v) { w16(p, v & 0xffff); w16(p + 2, v >> 16); }
static void w64(uint8_t *p, uint64_t v) { w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32)); }

struct fake_ip {
    uint16_t hw_id; uint8_t inst, maj, min, rev, bits;
    uint8_t nbase; uint64_t base[4];
};

/* A plausible subset of Navi 32 (versions as handled by amdgpu for GC 11.0.3). */
static const struct fake_ip navi32[] = {
    { AMD_HWID_GC,    0, 11, 0, 3, 0x00, 2, { 0x1260, 0xA000 } },
    { AMD_HWID_MP0,   0, 13, 0, 7, 0x00, 2, { 0x16000, 0x243FC00 } },
    { AMD_HWID_MP1,   0, 13, 0, 7, 0x00, 2, { 0x16000, 0x243FC00 } },
    { AMD_HWID_SDMA0, 0,  6, 0, 3, 0x00, 1, { 0x1260 } },
    { AMD_HWID_SDMA0, 1,  6, 0, 3, 0x00, 1, { 0x1860 } },
    { AMD_HWID_NBIF,  0,  4, 3, 0, 0x00, 3, { 0x0, 0x14, 0xD20 } },
    { AMD_HWID_DMU,   0,  3, 2, 1, 0x21, 3, { 0x12, 0xC0, 0x34C0 } },
    { AMD_HWID_VCN,   0,  4, 0, 4, 0x00, 1, { 0x7800 } },
};
#define NIPS (sizeof(navi32) / sizeof(navi32[0]))

/* Returns total size. hdr_v: 1 or 2. ipd_v: 3 or 4 (4 => 64-bit bases). */
static size_t build(uint8_t *b, size_t cap, int hdr_v, int ipd_v)
{
    const int b64 = (ipd_v >= 4);
    const size_t tables = (hdr_v == 2) ? 16 : 12;
    const size_t ntab = 6;
    size_t ihdr = tables + 8 * ntab;
    size_t die = ihdr + 80;
    size_t p = die + 4;
    size_t i, k;

    memset(b, 0, cap);
    w32(b, AMD_DISC_BINARY_SIGNATURE);
    w16(b + 4, (uint16_t)hdr_v);
    w16(b + 6, 0);
    if (hdr_v == 2)
        w16(b + 12, (uint16_t)ntab);

    /* die header */
    w16(b + die, 0);
    w16(b + die + 2, (uint16_t)NIPS);
    for (i = 0; i < NIPS; i++) {
        const struct fake_ip *f = &navi32[i];
        w16(b + p, f->hw_id);
        b[p + 2] = f->inst; b[p + 3] = f->nbase;
        b[p + 4] = f->maj;  b[p + 5] = f->min; b[p + 6] = f->rev;
        b[p + 7] = f->bits;
        p += 8;
        for (k = 0; k < f->nbase; k++) {
            if (b64) { w64(b + p, f->base[k] | 0xC000000000000000ull | 0x40000000ull); p += 8; }
            else     { w32(b + p, (uint32_t)f->base[k]); p += 4; }
        }
    }

    /* IP discovery header; size covers header + die data, like real HW */
    w32(b + ihdr, AMD_DISC_TABLE_SIGNATURE);
    w16(b + ihdr + 4, (uint16_t)ipd_v);
    w16(b + ihdr + 6, (uint16_t)(p - ihdr));
    w16(b + ihdr + 12, 1);              /* num_dies */
    w16(b + ihdr + 14, 0);              /* die_id */
    w16(b + ihdr + 16, (uint16_t)die);  /* die_offset */
    if (b64)
        b[ihdr + 78] = 1;

    /* table_list[IP_DISCOVERY] */
    w16(b + tables, (uint16_t)ihdr);
    w16(b + tables + 2, amd_disc_checksum(b + ihdr, p - ihdr));
    w16(b + tables + 4, (uint16_t)(p - ihdr));

    w16(b + 10, (uint16_t)p);
    w16(b + 8, amd_disc_checksum(b + 10, p - 10));
    return p;
}

/* append an optional table (GC / harvest / VCN info) of 'len' bytes and
 * fix table_list[idx], bin_size and the binary checksum; returns the size */
static size_t add_table(uint8_t *b, size_t n, int idx, const uint8_t *t, size_t len)
{
    const size_t tables = (b[4] == 2) ? 16 : 12;
    memcpy(b + n, t, len);
    w16(b + tables + 8 * (size_t)idx, (uint16_t)n);
    w16(b + tables + 8 * (size_t)idx + 2, amd_disc_checksum(t, len));
    w16(b + tables + 8 * (size_t)idx + 4, (uint16_t)len);
    n += len;
    w16(b + 10, (uint16_t)n);
    w16(b + 8, amd_disc_checksum(b + 10, n - 10));
    return n;
}

static void fix_bin_checksum(uint8_t *b, size_t n)
{
    w16(b + 8, amd_disc_checksum(b + 10, n - 10));
}

static void check_navi32(const amd_discovery *d, int b64)
{
    const amd_ip_entry *gc = amd_discovery_find(d, AMD_HWID_GC, 0);
    const amd_ip_entry *sdma1 = amd_discovery_find(d, AMD_HWID_SDMA0, 1);
    const amd_ip_entry *dmu = amd_discovery_find(d, AMD_HWID_DMU, 0);

    CHECK(d->num_ips == NIPS);
    CHECK(d->num_dies == 1);
    CHECK(d->base_addr_64_bit == b64);
    CHECK(gc && gc->major == 11 && gc->minor == 0 && gc->revision == 3);
    CHECK(gc && gc->num_base_kept == 2 && gc->base[0] == 0x1260 && gc->base[1] == 0xA000);
    CHECK(sdma1 && sdma1->base[0] == 0x1860);
    CHECK(dmu && dmu->major == 3 && dmu->minor == 2 && dmu->revision == 1);
    /* bits 0x21: sub_revision = 1, variant = 2 (v3+ only) */
    CHECK(dmu && dmu->sub_revision == 1 && dmu->variant == 2);
    CHECK(amd_discovery_find(d, AMD_HWID_UMC, 0) == NULL);
}

int main(void)
{
    static uint8_t b[1u << 16];
    static amd_discovery d;
    size_t n, i;
    int hv, iv;

    /* --- good blobs --- */
    for (hv = 1; hv <= 2; hv++)
        for (iv = 3; iv <= 4; iv++) {
            n = build(b, sizeof(b), hv, iv);
            CHECK(amd_discovery_parse(b, n, &d) == AMD_DISC_OK);
            check_navi32(&d, iv >= 4);
            /* trailing garbage after bin_size is ignored (VRAM read is 10 KiB) */
            b[n] = 0xAA;
            CHECK(amd_discovery_parse(b, n + 64, &d) == AMD_DISC_OK);
        }

    /* --- rejections --- */
    n = build(b, sizeof(b), 1, 3);
    CHECK(amd_discovery_parse(NULL, n, &d) == AMD_DISC_E_ARGS);
    CHECK(amd_discovery_parse(b, 4, &d) == AMD_DISC_E_TRUNCATED);
    CHECK(amd_discovery_parse(b, n - 1, &d) == AMD_DISC_E_TRUNCATED);

    b[0] ^= 1;
    CHECK(amd_discovery_parse(b, n, &d) == AMD_DISC_E_BIN_SIGNATURE);
    b[0] ^= 1;

    b[n - 1] ^= 0x10; /* inside IP data: binary checksum catches it first */
    CHECK(amd_discovery_parse(b, n, &d) == AMD_DISC_E_BIN_CHECKSUM);
    b[n - 1] ^= 0x10;

    /* all-ones = what a dead / D3cold device returns on MMIO and VRAM reads */
    memset(b, 0xFF, n);
    CHECK(amd_discovery_parse(b, n, &d) == AMD_DISC_E_BIN_SIGNATURE);
    memset(b, 0, n);
    CHECK(amd_discovery_parse(b, n, &d) == AMD_DISC_E_BIN_SIGNATURE);

    /* table checksum wrong but binary checksum fixed up */
    n = build(b, sizeof(b), 1, 3);
    b[12 + 2] ^= 1;
    w16(b + 8, amd_disc_checksum(b + 10, n - 10));
    CHECK(amd_discovery_parse(b, n, &d) == AMD_DISC_E_TABLE_CHECKSUM);

    /* amdgpu_discovery_init: the GC (id + checksum over its size), harvest
     * (signature + checksum over sizeof(struct harvest_table)) and VCN info
     * (id + checksum over size_bytes) tables; MALL is not checked */
    {
        uint8_t gc[132], hvt[AMD_DISC_HARVEST_TABLE_SIZE], vcn[96], mall[48];
        size_t n0, n1, o;
        memset(gc, 0, sizeof(gc)); memset(hvt, 0, sizeof(hvt)); memset(vcn, 0, sizeof(vcn));
        memset(mall, 0xEE, sizeof(mall));
        w32(gc, AMD_DISC_GC_TABLE_ID); w16(gc + 4, 1); w16(gc + 6, 2); w32(gc + 8, sizeof(gc));
        w32(gc + 12, 3);                                   /* num_se */
        w32(hvt, AMD_DISC_HARVEST_SIGNATURE); w32(hvt + 4, 1);
        w32(vcn, AMD_DISC_VCN_INFO_TABLE_ID); w16(vcn + 4, 1); w32(vcn + 8, sizeof(vcn));
        n = build(b, sizeof(b), 2, 4);
        n = add_table(b, n, AMD_DISC_TABLE_GC, gc, sizeof(gc));
        n = add_table(b, n, AMD_DISC_TABLE_HARVEST_INFO, hvt, sizeof(hvt));
        n = add_table(b, n, AMD_DISC_TABLE_VCN_INFO, vcn, sizeof(vcn));
        n0 = n;
        n = add_table(b, n, AMD_DISC_TABLE_MALL_INFO, mall, sizeof(mall));   /* bad id: ignored */
        CHECK(amd_discovery_parse(b, n, &d) == AMD_DISC_OK && d.has_gc_info && d.gc.num_se == 3);
        /* a wrong checksum in each table, the binary checksum fixed up */
        o = (size_t)(b[16 + 8 * AMD_DISC_TABLE_GC] | (b[16 + 8 * AMD_DISC_TABLE_GC + 1] << 8));
        b[o + 20] ^= 1; fix_bin_checksum(b, n);
        CHECK(amd_discovery_parse(b, n, &d) == AMD_DISC_E_GC_TABLE);
        b[o + 20] ^= 1; b[o] ^= 1; fix_bin_checksum(b, n);       /* wrong table id */
        CHECK(amd_discovery_parse(b, n, &d) == AMD_DISC_E_GC_TABLE);
        b[o] ^= 1; fix_bin_checksum(b, n);
        o = (size_t)(b[16 + 8 * AMD_DISC_TABLE_HARVEST_INFO] | (b[16 + 8 * AMD_DISC_TABLE_HARVEST_INFO + 1] << 8));
        b[o + 100] ^= 4; fix_bin_checksum(b, n);
        CHECK(amd_discovery_parse(b, n, &d) == AMD_DISC_E_HARVEST_TABLE);
        b[o + 100] ^= 4; fix_bin_checksum(b, n);
        o = (size_t)(b[16 + 8 * AMD_DISC_TABLE_VCN_INFO] | (b[16 + 8 * AMD_DISC_TABLE_VCN_INFO + 1] << 8));
        b[o + 50] ^= 2; fix_bin_checksum(b, n);
        CHECK(amd_discovery_parse(b, n, &d) == AMD_DISC_E_VCN_TABLE);
        b[o + 50] ^= 2; fix_bin_checksum(b, n);
        CHECK(amd_discovery_parse(b, n, &d) == AMD_DISC_OK);
        (void)n0;
        /* amdgpu_discovery_validate_ip: an instance number or hw id out of
         * range drops that IP only */
        n1 = build(b, sizeof(b), 1, 3);
        b[12 + 48 + 80 + 4 + 2] = AMD_DISC_HWIP_MAX_INSTANCE;   /* first IP's instance */
        w16(b + 12 + 2, amd_disc_checksum(b + 12 + 48, n1 - 12 - 48));
        fix_bin_checksum(b, n1);
        CHECK(amd_discovery_parse(b, n1, &d) == AMD_DISC_OK && d.num_ips_invalid == 1 &&
              d.num_ips == NIPS - 1);
    }

    /* --- mutation fuzz: must never crash or read OOB (ASan) --- */
    srand(1234);
    for (i = 0; i < 200000; i++) {
        size_t len = build(b, sizeof(b), 1 + (int)(i & 1), 3 + (int)((i >> 1) & 1));
        int m, nm = 1 + rand() % 8;
        for (m = 0; m < nm; m++)
            b[rand() % len] = (uint8_t)rand();
        if (i & 4) { /* keep checksums valid so deeper code is reached */
            size_t tbl = (b[4] == 2) ? 16 : 12;
            size_t ih = (size_t)(b[tbl] | (b[tbl + 1] << 8));
            size_t isz;
            if (ih + 8 <= len) {
                isz = (size_t)(b[ih + 6] | (b[ih + 7] << 8));
                if (ih + isz <= len) {
                    uint16_t c = amd_disc_checksum(b + ih, isz);
                    b[tbl + 2] = c & 0xff; b[tbl + 3] = c >> 8;
                }
            }
            {
                size_t bs = (size_t)(b[10] | (b[11] << 8));
                if (bs >= 12 && bs <= len) {
                    uint16_t c = amd_disc_checksum(b + 10, bs - 10);
                    b[8] = c & 0xff; b[9] = c >> 8;
                }
            }
        }
        (void)amd_discovery_parse(b, len, &d);
    }

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_discovery: all checks passed\n");
    return 0;
}
