/*
 * amd_discovery.c - see amd_discovery.h
 *
 * SPDX-License-Identifier: MIT
 */
#include "amd_discovery.h"

/* binary_header (v0/v1): sig u32, ver_major u16, ver_minor u16,
 * checksum u16, size u16, table_info[6] at +12.
 * binary_header_v2: same first 12 bytes, num_tables u16, pad u16,
 * table_info[] at +16. table_info = { offset, checksum, size, pad } u16. */
#define BHDR_CHECKSUM_END   10u  /* checksum covers bytes after this */
#define BHDR_V1_TABLES      12u
#define BHDR_V2_NUM_TABLES  12u
#define BHDR_V2_TABLES      16u
#define TABLE_INFO_SIZE     8u

/* ip_discovery_header: sig u32, version u16, size u16, id u32,
 * num_dies u16, die_info[16] {die_id u16, die_offset u16},
 * then 2 bytes (v4: bit0 of byte 0 = base_addr_64_bit). */
#define IHDR_VERSION        4u
#define IHDR_SIZE_FIELD     6u
#define IHDR_NUM_DIES       12u
#define IHDR_DIE_INFO       14u
#define IHDR_FLAGS          (IHDR_DIE_INFO + 4u * 16u) /* 78 */
#define IHDR_TOTAL          (IHDR_FLAGS + 2u)          /* 80 */

#define DHDR_SIZE           4u  /* die_id u16, num_ips u16 */
#define IP_FIXED_SIZE       8u  /* hw_id u16, inst, nbase, maj, min, rev, bits */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* true when [off, off+n) lies inside a buffer of |len| bytes */
static int in_bounds(size_t len, size_t off, size_t n)
{
    return off <= len && n <= len - off;
}

uint16_t amd_disc_checksum(const uint8_t *p, size_t n)
{
    uint16_t sum = 0;
    size_t i;
    for (i = 0; i < n; i++)
        sum = (uint16_t)(sum + p[i]);
    return sum;
}

static void zero(void *p, size_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    while (n--)
        *b++ = 0;
}

amd_disc_status amd_discovery_parse(const uint8_t *bin, size_t len,
                                    amd_discovery *out)
{
    size_t tables, tbl_off, ihdr_off, ihdr_len;
    uint16_t bin_size, ipd_ver;
    unsigned i;

    if (!bin || !out)
        return AMD_DISC_E_ARGS;
    zero(out, sizeof(*out));

    /* ---- binary header ---- */
    if (!in_bounds(len, 0, BHDR_V1_TABLES))
        return AMD_DISC_E_TRUNCATED;
    if (rd32(bin) != AMD_DISC_BINARY_SIGNATURE)
        return AMD_DISC_E_BIN_SIGNATURE;

    out->bin_version_major = rd16(bin + 4);
    out->bin_version_minor = rd16(bin + 6);
    bin_size = rd16(bin + 10);
    out->bin_size = bin_size;

    if (bin_size < BHDR_V1_TABLES || bin_size > len)
        return AMD_DISC_E_TRUNCATED;
    if (amd_disc_checksum(bin + BHDR_CHECKSUM_END, bin_size - BHDR_CHECKSUM_END)
        != rd16(bin + 8))
        return AMD_DISC_E_BIN_CHECKSUM;

    switch (out->bin_version_major) {
    case 0:
    case 1:
        tables = BHDR_V1_TABLES;
        break;
    case 2:
        if (!in_bounds(bin_size, 0, BHDR_V2_TABLES))
            return AMD_DISC_E_TRUNCATED;
        if (rd16(bin + BHDR_V2_NUM_TABLES) <= AMD_DISC_TABLE_IP_DISCOVERY)
            return AMD_DISC_E_TRUNCATED;
        tables = BHDR_V2_TABLES;
        break;
    default:
        return AMD_DISC_E_BIN_VERSION;
    }

    tbl_off = tables + TABLE_INFO_SIZE * AMD_DISC_TABLE_IP_DISCOVERY;
    if (!in_bounds(bin_size, tbl_off, TABLE_INFO_SIZE))
        return AMD_DISC_E_TRUNCATED;

    /* ---- IP discovery table ---- */
    ihdr_off = rd16(bin + tbl_off);
    if (!in_bounds(bin_size, ihdr_off, IHDR_TOTAL))
        return AMD_DISC_E_TRUNCATED;

    const uint8_t *ih = bin + ihdr_off;
    if (rd32(ih) != AMD_DISC_TABLE_SIGNATURE)
        return AMD_DISC_E_TABLE_SIGNATURE;

    ihdr_len = rd16(ih + IHDR_SIZE_FIELD);
    if (!in_bounds(bin_size, ihdr_off, ihdr_len))
        return AMD_DISC_E_TRUNCATED;
    if (amd_disc_checksum(ih, ihdr_len) != rd16(bin + tbl_off + 2))
        return AMD_DISC_E_TABLE_CHECKSUM;

    ipd_ver = rd16(ih + IHDR_VERSION);
    out->ipd_version = ipd_ver;
    out->num_dies = rd16(ih + IHDR_NUM_DIES);
    out->base_addr_64_bit = (ipd_ver >= 4) ? (ih[IHDR_FLAGS] & 1u) : 0;

    if (out->num_dies == 0 || out->num_dies > AMD_DISC_MAX_DIES)
        return AMD_DISC_E_BAD_DIE;

    const size_t base_sz = out->base_addr_64_bit ? 8u : 4u;

    /* ---- GC info table (optional, only version 1.x decoded) ---- */
    {
        size_t gt = tables + TABLE_INFO_SIZE * AMD_DISC_TABLE_GC;
        size_t go;
        if (in_bounds(bin_size, gt, TABLE_INFO_SIZE) && (go = rd16(bin + gt)) != 0 &&
            in_bounds(bin_size, go, 12 + 19 * 4) && rd32(bin + go) == AMD_DISC_GC_TABLE_ID &&
            rd16(bin + go + 4) == 1) {
            amd_gc_info *g = &out->gc;
            uint32_t *f = &g->num_se;
            unsigned n, k;
            g->version_major = 1;
            g->version_minor = rd16(bin + go + 6);
            n = g->version_minor >= 2 ? 30 : g->version_minor >= 1 ? 22 : 19;
            for (k = 0; k < n && in_bounds(bin_size, go + 12 + 4 * k, 4); k++)
                f[k] = rd32(bin + go + 12 + 4 * k);
            out->has_gc_info = 1;
        }
    }

    for (i = 0; i < out->num_dies; i++) {
        size_t die_off = rd16(ih + IHDR_DIE_INFO + 4u * i + 2u);
        size_t ip_off;
        unsigned j, num_ips;

        if (!in_bounds(bin_size, die_off, DHDR_SIZE))
            return AMD_DISC_E_BAD_DIE;
        num_ips = rd16(bin + die_off + 2);
        ip_off = die_off + DHDR_SIZE;

        for (j = 0; j < num_ips; j++) {
            const uint8_t *ip;
            uint8_t nbase;
            unsigned k;

            if (!in_bounds(bin_size, ip_off, IP_FIXED_SIZE))
                return AMD_DISC_E_TRUNCATED;
            ip = bin + ip_off;
            nbase = ip[3];
            if (!in_bounds(bin_size, ip_off + IP_FIXED_SIZE, base_sz * nbase))
                return AMD_DISC_E_TRUNCATED;

            if (out->num_ips < AMD_DISC_MAX_IPS) {
                amd_ip_entry *e = &out->ips[out->num_ips++];
                e->hw_id    = rd16(ip);
                e->die      = (uint8_t)i;
                e->instance = ip[2];
                e->num_base = nbase;
                e->major    = ip[4];
                e->minor    = ip[5];
                e->revision = ip[6];
                if (ipd_ver >= 3) {
                    e->sub_revision = ip[7] & 0x0f;
                    e->variant      = ip[7] >> 4;
                } else {
                    e->harvest      = ip[7] & 0x0f;
                }
                e->num_base_kept = nbase < AMD_DISC_MAX_BASES ? nbase
                                                              : AMD_DISC_MAX_BASES;
                for (k = 0; k < e->num_base_kept; k++) {
                    const uint8_t *b = ip + IP_FIXED_SIZE + base_sz * k;
                    /* 64-bit: amdgpu keeps lower 32 bits, top 2 cleared */
                    e->base[k] = out->base_addr_64_bit ? (rd32(b) & 0x3FFFFFFFu)
                                                       : rd32(b);
                }
            } else {
                out->num_ips_dropped++;
            }
            ip_off += IP_FIXED_SIZE + base_sz * nbase;
        }
    }
    return AMD_DISC_OK;
}

const amd_ip_entry *amd_discovery_find(const amd_discovery *d,
                                       uint16_t hw_id, uint8_t instance)
{
    uint32_t i;
    if (!d)
        return NULL;
    for (i = 0; i < d->num_ips; i++)
        if (d->ips[i].hw_id == hw_id && d->ips[i].instance == instance)
            return &d->ips[i];
    return NULL;
}

const char *amd_hwid_name(uint16_t hw_id)
{
    switch (hw_id) {
    case 1:   return "MP1";
    case 2:   return "MP2";
    case 3:   return "THM";
    case 4:   return "SMUIO";
    case 5:   return "FUSE";
    case 6:   return "CLKA";
    case 10:  return "PWR";
    case 11:  return "GC";
    case 12:  return "UVD/VCN";
    case 13:  return "AUDIO_AZ";
    case 14:  return "ACP";
    case 15:  return "DCI";
    case 16:  return "DCO";
    case 17:  return "XDMA";
    case 18:  return "DCEAZ";
    case 19:  return "SDPMUX";
    case 20:  return "NTB";
    case 21:  return "VPE";
    case 22:  return "UMSCH";
    case 24:  return "IOHC";
    case 28:  return "L2IMU";
    case 32:  return "VCE";
    case 34:  return "MMHUB";
    case 35:  return "ATHUB";
    case 36:  return "DBGU_NBIO";
    case 37:  return "DFX";
    case 38:  return "DBGU0";
    case 39:  return "DBGU1";
    case 40:  return "OSSSYS";
    case 41:  return "HDP";
    case 42:  return "SDMA0";
    case 43:  return "SDMA1";
    case 44:  return "ISP";
    case 45:  return "DBGU_IO";
    case 46:  return "DF";
    case 47:  return "CLKB";
    case 48:  return "FCH";
    case 49:  return "DFX_DAP";
    case 50:  return "L1IMU_PCIE";
    case 51:  return "L1IMU_NBIF";
    case 52:  return "L1IMU_IOAGR";
    case 66:  return "WAFLC";
    case 67:  return "FCH_USB_PD";
    case 68:  return "SDMA2";
    case 69:  return "SDMA3";
    case 70:  return "PCIE";
    case 80:  return "PCS";
    case 89:  return "DDCL";
    case 90:  return "SST";
    case 91:  return "LSDMA";
    case 100: return "IOAGR";
    case 108: return "NBIF";
    case 124: return "IOAPIC";
    case 128: return "SYSTEMHUB";
    case 144: return "NTBCCP";
    case 150: return "UMC";
    case 168: return "SATA";
    case 170: return "USB";
    case 176: return "CCXSEC";
    case 200: return "XGMI";
    case 216: return "XGBE";
    case 255: return "MP0";
    case 271: return "DMU";
    case 272: return "DIO";
    case 274: return "DAZ";
    case 294: return "ATU";
    default:  return NULL;
    }
}

const char *amd_disc_status_str(amd_disc_status s)
{
    switch (s) {
    case AMD_DISC_OK:                return "ok";
    case AMD_DISC_E_ARGS:            return "bad arguments";
    case AMD_DISC_E_TRUNCATED:       return "truncated / out of bounds";
    case AMD_DISC_E_BIN_SIGNATURE:   return "bad binary signature";
    case AMD_DISC_E_BIN_VERSION:     return "unsupported binary header version";
    case AMD_DISC_E_BIN_CHECKSUM:    return "binary checksum mismatch";
    case AMD_DISC_E_TABLE_SIGNATURE: return "bad IP table signature";
    case AMD_DISC_E_TABLE_CHECKSUM:  return "IP table checksum mismatch";
    case AMD_DISC_E_BAD_DIE:         return "bad die list";
    }
    return "unknown";
}
