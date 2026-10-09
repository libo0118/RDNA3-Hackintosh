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
    b->firmware_capability = rd32(t + 16);
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
