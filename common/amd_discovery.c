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

/* amdgpu_discovery_calculate_checksum */
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
    if (rd32(bin) != AMD_DISC_BINARY_SIGNATURE)   /* amdgpu_discovery_verify_binary_signature */
        return AMD_DISC_E_BIN_SIGNATURE;

    out->bin_version_major = rd16(bin + 4);
    out->bin_version_minor = rd16(bin + 6);
    bin_size = rd16(bin + 10);
    out->bin_size = bin_size;

    if (bin_size < BHDR_V1_TABLES || bin_size > len)
        return AMD_DISC_E_TRUNCATED;
    /* amdgpu_discovery_verify_checksum */
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

    /* amdgpu_discovery_init: the GC, harvest and VCN info tables, when
     * present, must carry their id / signature and checksum (the MALL
     * table's check is disabled there: "if (0 && offset)") */
    {
        size_t t = tables + TABLE_INFO_SIZE * AMD_DISC_TABLE_GC, o;
        if (in_bounds(bin_size, t, TABLE_INFO_SIZE) && (o = rd16(bin + t)) != 0) {
            if (!in_bounds(bin_size, o, 12) || rd32(bin + o) != AMD_DISC_GC_TABLE_ID ||
                !in_bounds(bin_size, o, rd32(bin + o + 8)) ||
                amd_disc_checksum(bin + o, rd32(bin + o + 8)) != rd16(bin + t + 2))
                return AMD_DISC_E_GC_TABLE;
        }
        t = tables + TABLE_INFO_SIZE * AMD_DISC_TABLE_HARVEST_INFO;
        if (in_bounds(bin_size, t, TABLE_INFO_SIZE) && (o = rd16(bin + t)) != 0) {
            if (!in_bounds(bin_size, o, AMD_DISC_HARVEST_TABLE_SIZE) ||
                rd32(bin + o) != AMD_DISC_HARVEST_SIGNATURE ||
                amd_disc_checksum(bin + o, AMD_DISC_HARVEST_TABLE_SIZE) != rd16(bin + t + 2))
                return AMD_DISC_E_HARVEST_TABLE;
        }
        t = tables + TABLE_INFO_SIZE * AMD_DISC_TABLE_VCN_INFO;
        if (in_bounds(bin_size, t, TABLE_INFO_SIZE) && (o = rd16(bin + t)) != 0) {
            if (!in_bounds(bin_size, o, 12) || rd32(bin + o) != AMD_DISC_VCN_INFO_TABLE_ID ||
                !in_bounds(bin_size, o, rd32(bin + o + 8)) ||
                amd_disc_checksum(bin + o, rd32(bin + o + 8)) != rd16(bin + t + 2))
                return AMD_DISC_E_VCN_TABLE;
        }
    }

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

            /* amdgpu_discovery_validate_ip: such an entry is skipped */
            if (ip[2] >= AMD_DISC_HWIP_MAX_INSTANCE || rd16(ip) >= AMD_DISC_HW_ID_MAX) {
                out->num_ips_invalid++;
            } else if (out->num_ips < AMD_DISC_MAX_IPS) {
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
                /* amdgpu_discovery_reg_base_init: VCN revision bits
                 * [7:6] are the en/decode capability, not the revision */
                if (e->hw_id == AMD_HWID_VCN) {
                    e->vcn_config = e->revision & 0xc0;
                    e->revision &= (uint8_t)~0xc0;
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

/* table_list[idx].offset, 0 when absent or outside the binary */
static size_t table_off(const uint8_t *bin, size_t len, int idx)
{
    size_t tables, t;
    if (!in_bounds(len, 0, BHDR_V1_TABLES))
        return 0;
    tables = rd16(bin + 4) == 2 ? BHDR_V2_TABLES : BHDR_V1_TABLES;
    t = tables + TABLE_INFO_SIZE * (size_t)idx;
    if (!in_bounds(len, t, TABLE_INFO_SIZE))
        return 0;
    return rd16(bin + t);
}

#define IPV(maj, min, rev) (((uint32_t)(maj) << 16) | ((uint32_t)(min) << 8) | (uint32_t)(rev))

static uint32_t ip_version_of(const amd_discovery *d, uint16_t hw_id, uint8_t inst)
{
    const amd_ip_entry *e = amd_discovery_find(d, hw_id, inst);
    return e ? IPV(e->major, e->minor, e->revision) : 0;
}

/* amdgpu_discovery_read_harvest_bit_per_ip: variant 1 marks a harvested
 * VCN / DMU (discovery tables of the Navi 1x boards amdgpu lists) */
static void harvest_bits_per_ip(const amd_discovery *d, amd_disc_soc *s, uint32_t *vcn_harvest_count)
{
    uint32_t i;
    for (i = 0; i < d->num_ips; i++) {
        const amd_ip_entry *e = &d->ips[i];
        if (e->variant != 1)
            continue;
        if (e->hw_id == AMD_HWID_VCN) {
            (*vcn_harvest_count)++;
            if (e->instance == 0) {
                s->vcn_harvest_config |= AMD_DISC_VCN_HARVEST_VCN0;
                s->vcn_inst_mask &= ~AMD_DISC_VCN_HARVEST_VCN0;
                s->jpeg_inst_mask &= ~AMD_DISC_VCN_HARVEST_VCN0;
            } else {
                s->vcn_harvest_config |= AMD_DISC_VCN_HARVEST_VCN1;
                s->vcn_inst_mask &= ~AMD_DISC_VCN_HARVEST_VCN1;
                s->jpeg_inst_mask &= ~AMD_DISC_VCN_HARVEST_VCN1;
            }
        } else if (e->hw_id == AMD_HWID_DMU) {
            s->harvest_ip_mask |= AMD_DISC_HARVEST_IP_DMU;
        }
    }
}

/* amdgpu_discovery_read_from_harvest_table */
static void harvest_from_table(const uint8_t *bin, size_t len, amd_disc_soc *s,
                               uint32_t *vcn_harvest_count, uint32_t *umc_harvest_count)
{
    size_t o = table_off(bin, len, AMD_DISC_TABLE_HARVEST_INFO);
    uint32_t umc_harvest_config = 0, i;
    if (!o || !in_bounds(len, o, AMD_DISC_HARVEST_TABLE_SIZE))
        return;   /* "invalid harvest table offset" */
    for (i = 0; i < 32; i++) {
        uint16_t hw = rd16(bin + o + 8 + 4 * i);
        uint8_t inst = bin[o + 8 + 4 * i + 2];
        uint32_t bit = inst < 32 ? 1u << inst : 0;
        if (hw == 0)
            break;
        switch (hw) {
        case AMD_HWID_VCN:
            (*vcn_harvest_count)++;
            s->vcn_harvest_config |= bit;
            s->jpeg_harvest_config |= bit;
            s->vcn_inst_mask &= ~bit;
            s->jpeg_inst_mask &= ~bit;
            break;
        case AMD_HWID_DMU:
            s->harvest_ip_mask |= AMD_DISC_HARVEST_IP_DMU;
            break;
        case AMD_HWID_UMC:
            umc_harvest_config |= bit;
            (*umc_harvest_count)++;
            break;
        case AMD_HWID_GC:
            s->xcc_mask &= ~bit;
            break;
        case AMD_HWID_SDMA0:
            s->sdma_mask &= ~bit;
            break;
        default:
            break;
        }
    }
    /* adev->umc.node_inst_num = the UMC count of reg_base_init */
    s->umc_active_mask = (s->num_umc >= 32 ? 0xffffffffu : (1u << s->num_umc) - 1) & ~umc_harvest_config;
}

amd_disc_status amd_discovery_soc_config(const uint8_t *bin, size_t len, const amd_discovery *d,
                                         uint16_t pci_device, uint8_t pci_revision,
                                         amd_disc_soc *s)
{
    uint32_t i, gc, vcn_harvest_count = 0, umc_harvest_count = 0;
    size_t o;

    if (!bin || !d || !s)
        return AMD_DISC_E_ARGS;
    zero(s, sizeof(*s));

    /* amdgpu_discovery_reg_base_init: instance counts and masks */
    for (i = 0; i < d->num_ips; i++) {
        const amd_ip_entry *e = &d->ips[i];
        uint32_t bit = e->instance < 32 ? 1u << e->instance : 0;
        if (e->hw_id == AMD_HWID_VCN && s->num_vcn_inst < AMD_DISC_MAX_VCN_INSTANCES) {
            s->vcn_config[s->num_vcn_inst] = e->vcn_config;
            s->num_vcn_inst++;
            s->vcn_inst_mask |= bit;
            s->jpeg_inst_mask |= bit;
        }
        if ((e->hw_id == AMD_HWID_SDMA0 || e->hw_id == AMD_HWID_SDMA1 || e->hw_id == AMD_HWID_SDMA2 ||
             e->hw_id == AMD_HWID_SDMA3) && s->num_sdma < AMD_DISC_MAX_SDMA_INSTANCES) {
            s->num_sdma++;
            s->sdma_mask |= bit;
        }
        if (e->hw_id == AMD_HWID_UMC)
            s->num_umc++;
        if (e->hw_id == AMD_HWID_GC)
            s->xcc_mask |= bit;
    }
    s->umc_active_mask = s->num_umc >= 32 ? 0xffffffffu : (1u << s->num_umc) - 1;

    /* amdgpu_discovery_harvest_ip */
    gc = ip_version_of(d, AMD_HWID_GC, 0);
    if (gc < IPV(10, 2, 0) && gc != IPV(9, 4, 3) && gc != IPV(9, 4, 4)) {
        if ((pci_device == 0x731E && (pci_revision == 0xC6 || pci_revision == 0xC7)) ||
            (pci_device == 0x7340 && pci_revision == 0xC9) ||
            (pci_device == 0x7360 && pci_revision == 0xC7)) {
            harvest_bits_per_ip(d, s, &vcn_harvest_count);
            s->harvest_from_ip_bits = 1;
        }
    } else {
        harvest_from_table(bin, len, s, &vcn_harvest_count, &umc_harvest_count);
    }
    /* amdgpu_discovery_harvest_config_quirk: Navy Flounder boards with a
     * bad VCN harvest entry */
    if (ip_version_of(d, AMD_HWID_VCN, 1) == IPV(3, 0, 1) && gc == IPV(10, 3, 2)) {
        switch (pci_revision) {
        case 0xC1: case 0xC2: case 0xC3: case 0xC5: case 0xC7: case 0xCF: case 0xDF:
            s->vcn_harvest_config |= AMD_DISC_VCN_HARVEST_VCN1;
            s->vcn_inst_mask &= ~AMD_DISC_VCN_HARVEST_VCN1;
            break;
        default:
            break;
        }
    }
    if (vcn_harvest_count == s->num_vcn_inst)
        s->harvest_ip_mask |= AMD_DISC_HARVEST_IP_VCN | AMD_DISC_HARVEST_IP_JPEG;
    if (umc_harvest_count < s->num_umc)
        s->num_umc -= umc_harvest_count;

    /* amdgpu_discovery_get_mall_info: mall_info_header (id, major, minor,
     * size) then v1 { mall_size_per_m, m_s_present, m_half_use } or
     * v2 { mall_size_per_umc } */
    o = table_off(bin, len, AMD_DISC_TABLE_MALL_INFO);
    if (o && in_bounds(len, o, 24)) {
        uint32_t per = rd32(bin + o + 12), u;
        switch (rd16(bin + o + 4)) {
        case 1: {
            uint32_t present = rd32(bin + o + 16), half = rd32(bin + o + 20);
            uint64_t size = 0;
            for (u = 0; u < s->num_umc && u < 32; u++) {
                if (present & (1u << u))
                    size += (uint64_t)per * 2;
                else if (half & (1u << u))
                    size += per / 2;
                else
                    size += per;
            }
            s->mall_size = size;
            s->m_half_use = half;
            break;
        }
        case 2:
            s->mall_size = (uint64_t)per * s->num_umc;
            break;
        default:   /* "Unhandled MALL info table" */
            break;
        }
    }

    /* amdgpu_discovery_get_vcn_info: vcn_info_v1_0 { header, num_of_instances,
     * instance_info[4] { instance_num, fuse_data, reserved[2] } } */
    o = table_off(bin, len, AMD_DISC_TABLE_VCN_INFO);
    if (o && s->num_vcn_inst <= 4 && in_bounds(len, o, 16 + 16 * 4) && rd16(bin + o + 4) == 1)
        for (i = 0; i < s->num_vcn_inst; i++)
            s->vcn_codec_disable_mask[i] = rd32(bin + o + 16 + 16 * i + 4);
    return AMD_DISC_OK;
}

uint8_t amd_discovery_get_harvest_info(const amd_disc_soc *s, uint16_t hw_id, uint8_t inst)
{
    uint32_t bit = inst < 32 ? 1u << inst : 0;
    switch (hw_id) {
    case AMD_HWID_VCN:
        return (bit & s->vcn_inst_mask) == 0;
    case AMD_HWID_DMU:
        return (s->harvest_ip_mask & AMD_DISC_HARVEST_IP_DMU) ? 1 : 0;
    case AMD_HWID_GC:
        return (bit & s->xcc_mask) == 0;
    case AMD_HWID_SDMA0:
        return (bit & s->sdma_mask) == 0;
    default:   /* UMC: "TODO: It needs another parsing" */
        return 0;
    }
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
    case AMD_DISC_E_GC_TABLE:        return "bad GC table id or checksum";
    case AMD_DISC_E_HARVEST_TABLE:   return "bad harvest table signature or checksum";
    case AMD_DISC_E_VCN_TABLE:       return "bad VCN info table id or checksum";
    }
    return "unknown";
}
