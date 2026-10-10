/*
 * vbios.c - see vbios.h. amdgpu_soc15_read_bios_from_rom + check_atom_bios +
 * amdgpu_atom_parse_data_header, ported from Linux amdgpu (MIT).
 *
 * SPDX-License-Identifier: MIT
 */
#include "vbios.h"
#include "navi3x_regs.h"

#define ATOM_SIGNATURE        " 761295520"
#define ATOM_SIGNATURE_OFF    0x30
#define ATOM_ROM_TABLE_PTR    0x48   /* -> atom_rom_header_v2_2 */
#define ATOM_ROM_DATA_PTR     0x20   /* rom header: masterdatatable_offset */
#define ATOM_ROM_MSG_PTR      0x10   /* rom header: vbios_bootupmessageoffset */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int locate(rdna3_dev *d, rdna3_vbios *v)
{
    const uint8_t *b = v->data;
    uint16_t hdr, dt;
    unsigned i;

    if (v->len < 0x4a || b[0] != 0x55 || b[1] != 0xAA) {
        RERR(d, "VBIOS: no 55AA ROM signature (%02x %02x)", b[0], b[1]);
        return RDNA3_ENODEV;
    }
    hdr = rd16(b + ATOM_ROM_TABLE_PTR);
    if (!hdr || (uint32_t)hdr + 0x24 > v->len) {
        RERR(d, "VBIOS: bad ROM header offset 0x%x", hdr);
        return RDNA3_ENODEV;
    }
    /* check_atom_bios: "ATOM" (or byte-swapped "MOTA") after the common header */
    if (!((b[hdr + 4] == 'A' && b[hdr + 5] == 'T' && b[hdr + 6] == 'O' && b[hdr + 7] == 'M') ||
          (b[hdr + 4] == 'M' && b[hdr + 5] == 'O' && b[hdr + 6] == 'T' && b[hdr + 7] == 'A'))) {
        RERR(d, "VBIOS: not an ATOM BIOS");
        return RDNA3_ENODEV;
    }
    v->rom_header = hdr;
    dt = rd16(b + hdr + ATOM_ROM_DATA_PTR);
    if (!dt || (uint32_t)dt + 4 + 2 * RD_ATOM_DT_COUNT > v->len) {
        RERR(d, "VBIOS: bad data table directory offset 0x%x", dt);
        return RDNA3_ENODEV;
    }
    v->data_table = dt;

    /* the boot-up message is the human readable part number string */
    v->part_number[0] = 0;
    {
        uint16_t msg = rd16(b + hdr + ATOM_ROM_MSG_PTR);
        if (msg && msg < v->len) {
            const uint8_t *s = b + msg;
            /* skip leading control / whitespace */
            while ((uint32_t)(s - b) < v->len && (*s == '\r' || *s == '\n' || *s == ' '))
                s++;
            for (i = 0; i + 1 < sizeof(v->part_number) && (uint32_t)(s - b) < v->len; i++) {
                uint8_t c = s[i];
                if (c < 0x20 || c > 0x7e)
                    break;
                v->part_number[i] = (char)c;
            }
            v->part_number[i] = 0;
        }
    }
    return RDNA3_OK;
}

int vbios_parse(rdna3_dev *d, rdna3_vbios *v, uint8_t *buf, uint32_t len)
{
    v->data = buf;
    v->cap = len;
    v->len = len;
    v->rom_offset = 0;
    return locate(d, v);
}

int vbios_read(rdna3_dev *d, rdna3_vbios *v, uint8_t *buf, uint32_t cap)
{
    /* smuio_v13_0_6_get_rom_index_offset, smuio_v13_0_6_get_rom_data_offset */
    uint32_t idx = SOC15_OFF(d, SMUIO, 0, regROM_INDEX);
    uint32_t dat = SOC15_OFF(d, SMUIO, 0, regROM_DATA);
    uint32_t i, dwords, len;
    uint32_t hdr[16];

    v->data = buf;
    v->cap = cap;
    v->len = 0;
    if (!d->ipver[HWIP_SMUIO].present) {
        RERR(d, "VBIOS: no SMUIO block in discovery");
        return RDNA3_ENODEV;
    }
    /* nbio_v4_3_get_rom_offset: strap selects which 128 KiB of the ROM */
    v->rom_offset = (RREG32_SOC15(d, NBIO, 0, regREGS_ROM_OFFSET_CTRL) &
                     REGS_ROM_OFFSET_CTRL__ROM_OFFSET_MASK) << 17;

    /* amdgpu_read_bios_from_rom: header first, then the whole image */
    rdna3_wreg(d, idx, v->rom_offset);
    for (i = 0; i < 16; i++)
        hdr[i] = rdna3_rreg(d, dat);
    {
        const uint8_t *h = (const uint8_t *)hdr;
        if (h[0] != 0x55 || h[1] != 0xAA) {
            RERR(d, "VBIOS: ROM window holds %02x %02x, not a BIOS image", h[0], h[1]);
            return RDNA3_ENODEV;
        }
        if (h[ATOM_SIGNATURE_OFF] != ' ' || h[ATOM_SIGNATURE_OFF + 1] != '7' ||
            h[ATOM_SIGNATURE_OFF + 2] != '6' || h[ATOM_SIGNATURE_OFF + 9] != '0') {
            RERR(d, "VBIOS: ATI signature missing at 0x30");
            return RDNA3_ENODEV;
        }
        len = (uint32_t)h[2] << 9;
    }
    if (!len || len > cap) {
        RERR(d, "VBIOS: image is %u bytes, buffer holds %u", len, cap);
        return RDNA3_ENOMEM;
    }
    dwords = (len + 3) / 4;
    rdna3_wreg(d, idx, v->rom_offset);
    for (i = 0; i < dwords; i++) {
        uint32_t w = rdna3_rreg(d, dat);
        buf[4 * i] = (uint8_t)w;
        buf[4 * i + 1] = (uint8_t)(w >> 8);
        buf[4 * i + 2] = (uint8_t)(w >> 16);
        buf[4 * i + 3] = (uint8_t)(w >> 24);
    }
    v->len = len;
    if (locate(d, v))
        return RDNA3_ENODEV;
    RLOG(d, "VBIOS: %u KiB ATOM image at ROM offset 0x%x, \"%s\"", len / 1024, v->rom_offset,
         v->part_number);
    return RDNA3_OK;
}

const uint8_t *vbios_data_table(const rdna3_vbios *v, unsigned index, uint16_t *size,
                                uint8_t *frev, uint8_t *crev)
{
    uint16_t off;
    const uint8_t *t;
    if (!v->data || !v->data_table || index >= RD_ATOM_DT_COUNT)
        return NULL;
    /* amdgpu_atom_parse_data_header: directory = common header + uint16 offsets */
    off = rd16(v->data + v->data_table + 4 + 2 * index);
    if (!off || (uint32_t)off + 4 > v->len)
        return NULL;
    t = v->data + off;
    if ((uint32_t)off + rd16(t) > v->len)
        return NULL;
    if (size) *size = rd16(t);
    if (frev) *frev = t[2];
    if (crev) *crev = t[3];
    return t;
}

int vbios_bootup_values(rdna3_dev *d, const rdna3_vbios *v, rdna3_vbios_boot *b)
{
    uint16_t size;
    uint8_t frev, crev;
    const uint8_t *t;
    unsigned i;

    for (i = 0; i < sizeof(*b); i++)
        ((uint8_t *)b)[i] = 0;
    t = vbios_data_table(v, RD_ATOM_DT_FIRMWAREINFO, &size, &frev, &crev);
    if (!t) {
        RERR(d, "VBIOS: no firmwareinfo table");
        return RDNA3_ENOENT;
    }
    if (frev != 3) {
        RERR(d, "VBIOS: firmwareinfo %u.%u not understood", frev, crev);
        return RDNA3_EINVAL;
    }
    /* atom_firmware_info_v3_1 .. v3_4 share the leading fields */
    b->fw_frev = frev;
    b->fw_crev = crev;
    b->firmware_revision = rd32(t + 4);
    b->gfxclk = rd32(t + 8);
    b->uclk = rd32(t + 12);
    b->firmware_capability = rd32(t + 16);   /* amdgpu_atomfirmware_query_firmware_capability */
    b->vddc = rd16(t + 28);
    b->vddci = rd16(t + 30);
    b->mvddc = rd16(t + 32);
    b->vdd_gfx = rd16(t + 34);
    b->cooling_id = t[37];
    if (crev >= 3 && size >= 64)
        b->pp_table_id = rd32(t + 60);
    if (crev >= 4 && size >= 88)
        b->fw_reserved_kb = rd32(t + 84);

    t = vbios_data_table(v, RD_ATOM_DT_SMU_INFO, &size, &frev, &crev);
    if (t) {
        if (frev == 3 && crev == 6 && size >= 184) {
            b->socclk = rd32(t + 40);
            b->fclk = rd32(t + 72);
            b->vclk = rd32(t + 176);
            b->dclk = rd32(t + 180);
        } else if (frev == 4 && crev == 0 && size >= 204) {
            b->socclk = rd32(t + 40);
            b->dcefclk = rd32(t + 56);
            b->fclk = rd32(t + 72);
            b->dclk = rd32(t + 196);
            b->vclk = rd32(t + 200);
        } else if (!(frev == 3 && crev == 1)) {
            RLOG(d, "VBIOS: smu_info %u.%u not handled", frev, crev);
        }
    }
    RLOG(d, "VBIOS: firmwareinfo 3.%u rev 0x%x, boot gfxclk %u MHz uclk %u MHz socclk %u MHz "
         "fclk %u MHz, vddc %u mV, pptable id %u",
         crev, b->firmware_revision, b->gfxclk / 100, b->uclk / 100, b->socclk / 100,
         b->fclk / 100, b->vddc, b->pp_table_id);
    return RDNA3_OK;
}

int vbios_fw_vram_reservation(const rdna3_vbios *v, uint64_t *start, uint64_t *size)
{
    uint16_t sz;
    uint8_t frev, crev;
    const uint8_t *t = vbios_data_table(v, RD_ATOM_DT_VRAM_USAGEBYFIRMWARE, &sz, &frev, &crev);
    *start = 0;
    *size = 0;
    if (!t || frev != 2 || sz < 12)
        return RDNA3_ENOENT;
    /* v2.1: start_address_in_kb, used_by_firmware_in_kb; v2.2 has the same
     * leading layout (fw_region_start_address_in_kb) */
    *start = (uint64_t)rd32(t + 4) << 10;
    *size = (uint64_t)rd16(t + 8) << 10;
    return RDNA3_OK;
}

uint64_t vbios_fw_reserved_fb_size(const rdna3_vbios *v)
{
    uint16_t sz;
    uint8_t frev, crev;
    const uint8_t *t = vbios_data_table(v, RD_ATOM_DT_FIRMWAREINFO, &sz, &frev, &crev);
    /* atom_firmware_info_v3_4 / v3_5: fw_reserved_size_in_kb at 84 */
    if (!t || frev != 3 || (crev != 4 && crev != 5) || sz < 88)
        return 0;
    return (uint64_t)rd32(t + 84) << 10;
}

/* ---- display object table: bios_parser2 get_bios_object / get_*_record ---- */

#define ATOM_RECORD_END     0xFF
#define ATOM_REC_I2C        1
#define ATOM_REC_HPD_INT    2
#define ATOM_REC_CONN_CAP   3
#define ATOM_REC_CONN_SPEED 4
#define ATOM_REC_ENC_CAP    20
#define ATOM_REC_DISP_CAPS  23
#define BIOS_MAX_NUM_RECORD 20

/* DCN 3.2 GPIO registers as the GPIO_PIN_LUT names them: absolute dword
 * offsets (DCN_BASE__INST0_SEG2 0x34C0 + reg), as hw_translate_dcn32.c */
#define DCN32_SEG2            0x34C0u
#define DCN32_GPIO_DDC1_A     (DCN32_SEG2 + 0x28d1u)   /* DDCn_A every 4 dwords */
#define DCN32_GPIO_HPD_A      (DCN32_SEG2 + 0x28f5u)

static const uint8_t *gpio_pin(const rdna3_vbios *v, uint8_t gpio_id, int match_i2c)
{
    /* atom_gpio_pin_lut_v2_1: header + atom_gpio_pin_assignment[] (8 bytes) */
    uint16_t size;
    uint8_t frev, crev;
    const uint8_t *t = vbios_data_table(v, RD_ATOM_DT_GPIO_PIN_LUT, &size, &frev, &crev);
    unsigned i, n;
    if (!t || crev != 1 || size < 4 + 8)
        return NULL;
    n = (size - 4u) / 8u;
    for (i = 0; i < n; i++) {
        const uint8_t *p = t + 4 + 8 * i;
        uint8_t id = p[6];
        if (match_i2c ? ((id & 0x80) == (gpio_id & 0x80) && (id & 0x70) == (gpio_id & 0x70) &&
                         (id & 0x0f) == (gpio_id & 0x0f))
                      : id == gpio_id)
            return p;
    }
    return NULL;
}

static int hpd_from_pin(const rdna3_vbios *v, uint8_t pin_id)
{
    static const uint8_t shifts[6] = { 0x00, 0x08, 0x10, 0x18, 0x1a, 0x1c };
    const uint8_t *p = gpio_pin(v, pin_id, 0);
    unsigned i;
    if (!p || rd32(p) != DCN32_GPIO_HPD_A)
        return -1;
    for (i = 0; i < 6; i++)
        if (p[4] == shifts[i])
            return (int)i;
    return -1;
}

static int ddc_from_i2c(const rdna3_vbios *v, uint8_t i2c_id)
{
    const uint8_t *p = gpio_pin(v, i2c_id, 1);
    if (p) {
        uint32_t reg = rd32(p);
        if (reg >= DCN32_GPIO_DDC1_A && reg < DCN32_GPIO_DDC1_A + 5 * 4 &&
            (reg - DCN32_GPIO_DDC1_A) % 4 == 0)
            return (int)((reg - DCN32_GPIO_DDC1_A) / 4);
    }
    /* no LUT entry: the hardware-assisted I2C line number is the DDC line */
    if ((i2c_id & 0x80) && (i2c_id & 0x0f) < 5)
        return i2c_id & 0x0f;
    return -1;
}

static void walk_records(const rdna3_vbios *v, uint32_t off, rdna3_display_path *p)
{
    unsigned i;
    for (i = 0; i < BIOS_MAX_NUM_RECORD; i++) {
        const uint8_t *r;
        uint8_t type, sz;
        if (!off || off + 2 > v->len)
            return;
        r = v->data + off;
        type = r[0];
        sz = r[1];
        if (type == ATOM_RECORD_END || !sz || off + sz > v->len)
            return;
        switch (type) {
        case ATOM_REC_I2C:
            if (sz >= 4) {
                p->i2c_id = r[2];
                p->ddc_line = (int8_t)ddc_from_i2c(v, r[2]);
            }
            break;
        case ATOM_REC_HPD_INT:
            if (sz >= 4) {
                p->hpd_pin_id = r[2];
                p->hpd_active = r[3];
                p->hpd = (int8_t)hpd_from_pin(v, r[2]);
            }
            break;
        case ATOM_REC_CONN_CAP:
            if (sz >= 4)
                p->connector_caps = rd16(r + 2);
            break;
        case ATOM_REC_CONN_SPEED:
            if (sz >= 6)
                p->max_link_mhz = rd32(r + 2);
            break;
        case ATOM_REC_ENC_CAP:
            if (sz >= 6)
                p->encoder_caps = rd32(r + 2);
            break;
        case ATOM_REC_DISP_CAPS:
            if (sz >= 6)
                p->connector_caps = rd32(r + 2);
            break;
        default:
            break;
        }
        off += sz;
    }
}

static int8_t transmitter_of(uint16_t enc)
{
    unsigned id = RD_ATOM_OBJ_ID(enc), en = RD_ATOM_OBJ_ENUM(enc);
    int base;
    if (RD_ATOM_OBJ_TYPE(enc) != RD_ATOM_OBJ_TYPE_ENCODER || en < 1 || en > 2)
        return -1;
    switch (id) {
    case RD_ATOM_ENC_UNIPHY:  base = 0; break;
    case RD_ATOM_ENC_UNIPHY1: base = 2; break;
    case RD_ATOM_ENC_UNIPHY2: base = 4; break;
    case RD_ATOM_ENC_UNIPHY3: base = 6; if (en != 1) return -1; break;
    default: return -1;
    }
    return (int8_t)(base + (int)en - 1);
}

int vbios_display_paths(const rdna3_vbios *v, rdna3_display_path *out, unsigned max)
{
    uint16_t size;
    uint8_t frev, crev;
    const uint8_t *t = vbios_data_table(v, RD_ATOM_DT_DISPLAYOBJECTINFO, &size, &frev, &crev);
    uint32_t tbl;
    unsigned i, n, cnt = 0;

    if (!t)
        return 0;
    if (frev != 1 || (crev != 4 && crev != 5) || size < 8)
        return RDNA3_EINVAL;
    tbl = (uint32_t)(t - v->data);
    n = t[6];
    if (8u + 16u * n > size)
        return RDNA3_EINVAL;
    for (i = 0; i < n && cnt < max; i++) {
        const uint8_t *e = t + 8 + 16 * i;
        uint16_t disp = rd16(e), drec = rd16(e + 2), enc = rd16(e + 4);
        rdna3_display_path *p = &out[cnt];
        unsigned k;
        /* a generic object (GLSync, MXM) has no encoder: skip, as DC does */
        if (!enc || RD_ATOM_OBJ_TYPE(disp) != RD_ATOM_OBJ_TYPE_CONNECTOR)
            continue;
        for (k = 0; k < sizeof(*p); k++)
            ((uint8_t *)p)[k] = 0;
        p->connector_objid = disp;
        p->encoder_objid = enc;
        p->connector = (uint8_t)RD_ATOM_OBJ_ID(disp);
        p->connector_enum = (uint8_t)RD_ATOM_OBJ_ENUM(disp);
        p->transmitter = transmitter_of(enc);
        p->ddc_line = -1;
        p->hpd = -1;
        if (crev == 4) {
            /* atom_display_object_path_v2: ..., encoder_recordoffset at 8,
             * device_tag at 12 */
            p->device_tag = rd16(e + 12);
            walk_records(v, drec ? tbl + drec : 0, p);
            if (rd16(e + 8))
                walk_records(v, tbl + rd16(e + 8), p);
        } else {
            /* atom_display_object_path_v3: device_tag at 12, no encoder
             * records (bios_parser2: "not available in v1_5") */
            p->device_tag = rd16(e + 12);
            walk_records(v, drec ? tbl + drec : 0, p);
        }
        cnt++;
    }
    return (int)cnt;
}

const char *vbios_connector_name(uint8_t c)
{
    switch (c) {
    case RD_ATOM_CONN_DVI_D:       return "DVI-D";
    case RD_ATOM_CONN_DUAL_DVI_D:  return "DVI-D DL";
    case RD_ATOM_CONN_HDMI_A:      return "HDMI";
    case RD_ATOM_CONN_DISPLAYPORT: return "DP";
    case RD_ATOM_CONN_EDP:         return "eDP";
    case RD_ATOM_CONN_USBC:        return "USB-C";
    default:                       return "?";
    }
}

/* ---- identification strings (atom.c, Linux v6.12) ---- */

#define OFF_STRINGS_NUMBER 0x2f   /* OFFSET_TO_GET_ATOMBIOS_NUMBER_OF_STRINGS */
#define OFF_STRINGS_START  0x6e   /* OFFSET_TO_GET_ATOMBIOS_STRING_START */
#define OFF_PART_NUMBER    0x80   /* OFFSET_TO_VBIOS_PART_NUMBER */
#define OFF_DATE           0x50   /* OFFSET_TO_VBIOS_DATE */
#define OFF_ROM_HEADER_PTR 0x48   /* OFFSET_TO_ATOM_ROM_HEADER_POINTER */

/* atom_find_str_in_rom: first offset in [start, end] where str begins */
static int find_str(const rdna3_vbios *v, const char *str, uint32_t start, uint32_t end)
{
    uint32_t n = 0, i;
    while (str[n])
        n++;
    for (; start <= end && start + n <= v->len; start++) {
        for (i = 0; i < n && v->data[start + i] == (uint8_t)str[i]; i++)
            ;
        if (i == n)
            return (int)start;
    }
    return -1;
}

void vbios_strings(const rdna3_vbios *v, rdna3_vbios_strings *s)
{
    const uint8_t *p = v->data;
    uint32_t len = v->len, i, off, num;
    int at;
    for (i = 0; i < sizeof(*s); i++)
        ((uint8_t *)s)[i] = 0;
    if (!p || len < 0x100)
        return;
    num = p[OFF_STRINGS_NUMBER];
    off = rd16(p + OFF_STRINGS_START);
    /* atom_get_vbios_name: past the num strings (P/N, ASIC, bus, memory)
     * and their CR LF, trailing spaces cut */
    if (num && off < len) {
        uint32_t c = off, k;
        for (k = 0; k < num && c < len; k++) {
            while (c < len && p[c])
                c++;
            c++;
        }
        c += 2;
        for (k = 0; k < sizeof(s->name) - 1 && c + k < len && p[c + k]; k++)
            s->name[k] = (char)p[c + k];
        while (k && s->name[k - 1] == ' ')
            s->name[--k] = 0;
    } else {
        for (i = 0; i < 7; i++)
            s->name[i] = "--N/A--"[i];
    }
    /* atom_get_vbios_pn: the first string, else the fixed offset, else
     * after "ATOMBIOS"; printable characters only */
    {
        uint32_t c = num ? off : OFF_PART_NUMBER, k;
        if (c >= len || !p[c]) {
            at = find_str(v, "ATOMBIOS", 3, 1024);
            c = at < 0 ? len : (uint32_t)at + 8;
        }
        if (c < len && !p[c])
            c++;
        for (k = 0; k < 43 && k < sizeof(s->pn) - 1 && c + k < len && p[c + k] >= ' ' && p[c + k] <= 'z'; k++)
            s->pn[k] = (char)p[c + k];
    }
    /* atom_get_vbios_date: "20YY/MM/DD HH:MM" from "MM/DD/YY HH:MM" */
    {
        const uint8_t *d = p + OFF_DATE;
        const uint8_t idx[] = { 6, 7, 0, 1, 3, 4, 9, 10, 11, 12, 13 };
        char out[17] = "20xx/xx/xx xxxxx";
        static const uint8_t pos[] = { 2, 3, 5, 6, 8, 9, 11, 12, 13, 14, 15 };
        for (i = 0; i < sizeof(idx); i++)
            out[pos[i]] = (char)d[idx[i]];
        for (i = 0; i < 17; i++)
            s->date[i] = out[i];
    }
    /* atom_get_vbios_version: after the "ATOMBIOSBK-AMD VER" anchor between
     * the strings and the ROM header (3..1024 if those are not set) */
    {
        uint32_t start = off, end = rd16(p + OFF_ROM_HEADER_PTR);
        if (end <= start) {
            start = 3;
            end = 1024;
        }
        at = find_str(v, "ATOMBIOSBK-AMD", start, end);
        if (at >= 0)
            for (i = 0; i < sizeof(s->ver_str) - 1 && (uint32_t)at + 18 + i < len; i++)
                s->ver_str[i] = (char)p[at + 18 + i];
    }
    /* ctx->version = atom_firmware_info's revision */
    {
        uint16_t size;
        uint8_t frev, crev;
        const uint8_t *t = vbios_data_table(v, RD_ATOM_DT_FIRMWAREINFO, &size, &frev, &crev);
        if (t && size >= 8)
            s->version = rd32(t + 4);
    }
}
