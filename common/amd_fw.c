/*
 * amd_fw.c - see amd_fw.h. Field offsets come from the vendored, verbatim
 * AMD header structs (third_party/amd/amdgpu_ucode_hdr.h) via offsetof(),
 * but every read goes through a bounds-checked little-endian accessor.
 *
 * SPDX-License-Identifier: MIT
 */
#include "amd_fw.h"
#include "amdgpu_ucode_hdr.h"
#include "psp_gfx_if.h"

#define OFF(type, field) ((uint32_t)offsetof(struct type, field))

typedef struct blob {
    const uint8_t *p;
    size_t len;
} blob;

static int rd32(const blob *b, uint32_t off, uint32_t *v)
{
    if (!b->p || off > b->len || b->len - off < 4)
        return 0;
    *v = (uint32_t)b->p[off] | ((uint32_t)b->p[off + 1] << 8) |
         ((uint32_t)b->p[off + 2] << 16) | ((uint32_t)b->p[off + 3] << 24);
    return 1;
}

static int rd16(const blob *b, uint32_t off, uint16_t *v)
{
    if (!b->p || off > b->len || b->len - off < 2)
        return 0;
    *v = (uint16_t)(b->p[off] | (b->p[off + 1] << 8));
    return 1;
}

static int range_ok(const blob *b, uint64_t off, uint64_t size)
{
    return off <= b->len && size <= b->len - off;
}

/* common_firmware_header */
typedef struct common_hdr {
    uint32_t size_bytes, header_size_bytes;
    uint16_t hv_major, hv_minor;
    uint32_t ucode_version, ucode_size_bytes, ucode_array_offset_bytes;
} common_hdr;

static amd_fw_status read_common(const blob *b, common_hdr *h)
{
    if (!rd32(b, OFF(common_firmware_header, size_bytes), &h->size_bytes) ||
        !rd32(b, OFF(common_firmware_header, header_size_bytes), &h->header_size_bytes) ||
        !rd16(b, OFF(common_firmware_header, header_version_major), &h->hv_major) ||
        !rd16(b, OFF(common_firmware_header, header_version_minor), &h->hv_minor) ||
        !rd32(b, OFF(common_firmware_header, ucode_version), &h->ucode_version) ||
        !rd32(b, OFF(common_firmware_header, ucode_size_bytes), &h->ucode_size_bytes) ||
        !rd32(b, OFF(common_firmware_header, ucode_array_offset_bytes),
              &h->ucode_array_offset_bytes))
        return AMD_FW_E_TRUNCATED;
    if (h->size_bytes > b->len)
        return AMD_FW_E_TRUNCATED;
    return AMD_FW_OK;
}

static void zero(void *p, size_t n)
{
    volatile uint8_t *q = (volatile uint8_t *)p;
    while (n--)
        *q++ = 0;
}

/* ------------------------------ SOS ------------------------------ */

amd_fw_status amd_psp_sos_parse(const uint8_t *fw, size_t len, amd_psp_sos *out)
{
    blob b = { fw, len };
    common_hdr h;
    amd_fw_status st;
    uint32_t base, count, i;

    if (!fw || !out)
        return AMD_FW_E_ARGS;
    zero(out, sizeof(*out));
    if ((st = read_common(&b, &h)) != AMD_FW_OK)
        return st;
    out->ucode_version = h.ucode_version;
    base = h.ucode_array_offset_bytes;

    if (h.hv_major == 1) {
        /* psp_firmware_header_v1_0: sys drv = [0, sos.offset), sos after it */
        uint32_t sos_off, sos_size, sos_ver;
        if (!rd32(&b, OFF(psp_firmware_header_v1_0, sos.offset_bytes), &sos_off) ||
            !rd32(&b, OFF(psp_firmware_header_v1_0, sos.size_bytes), &sos_size) ||
            !rd32(&b, OFF(psp_firmware_header_v1_0, sos.fw_version), &sos_ver))
            return AMD_FW_E_TRUNCATED;
        out->bin[AMD_PSP_BIN_SYS_DRV] = (amd_psp_bin){ base, sos_off, h.ucode_version };
        out->bin[AMD_PSP_BIN_SOS] = (amd_psp_bin){ base + sos_off, sos_size, sos_ver };
        if (h.hv_minor == 1 || h.hv_minor == 3) {
            uint32_t o, s;
            if (rd32(&b, OFF(psp_firmware_header_v1_1, toc.offset_bytes), &o) &&
                rd32(&b, OFF(psp_firmware_header_v1_1, toc.size_bytes), &s))
                out->bin[AMD_PSP_BIN_TOC] = (amd_psp_bin){ base + o, s, 0 };
            if (rd32(&b, OFF(psp_firmware_header_v1_1, kdb.offset_bytes), &o) &&
                rd32(&b, OFF(psp_firmware_header_v1_1, kdb.size_bytes), &s))
                out->bin[AMD_PSP_BIN_KDB] = (amd_psp_bin){ base + o, s, 0 };
        }
        if (h.hv_minor == 3) {
            uint32_t o, s;
            if (rd32(&b, OFF(psp_firmware_header_v1_3, spl.offset_bytes), &o) &&
                rd32(&b, OFF(psp_firmware_header_v1_3, spl.size_bytes), &s))
                out->bin[AMD_PSP_BIN_SPL] = (amd_psp_bin){ base + o, s, 0 };
        }
    } else if (h.hv_major == 2) {
        /* psp_firmware_header_v2_0/v2_1: array of psp_fw_bin_desc */
        uint32_t descs = (h.hv_minor == 1) ? OFF(psp_firmware_header_v2_1, psp_fw_bin)
                                           : OFF(psp_firmware_header_v2_0, psp_fw_bin);
        if (!rd32(&b, OFF(psp_firmware_header_v2_0, psp_fw_bin_count), &count))
            return AMD_FW_E_TRUNCATED;
        if (count > 64)
            return AMD_FW_E_TOO_MANY;
        for (i = 0; i < count; i++) {
            uint32_t d = descs + i * (uint32_t)sizeof(struct psp_fw_bin_desc);
            uint32_t type, ver, off, size;
            if (!rd32(&b, d + OFF(psp_fw_bin_desc, fw_type), &type) ||
                !rd32(&b, d + OFF(psp_fw_bin_desc, fw_version), &ver) ||
                !rd32(&b, d + OFF(psp_fw_bin_desc, offset_bytes), &off) ||
                !rd32(&b, d + OFF(psp_fw_bin_desc, size_bytes), &size))
                return AMD_FW_E_TRUNCATED;
            if (type == 0 || type >= AMD_PSP_BIN_MAX)
                continue; /* amdgpu warns and skips unknown types */
            out->bin[type] = (amd_psp_bin){ base + off, size, ver };
        }
    } else {
        return AMD_FW_E_VERSION;
    }

    for (i = 1; i < AMD_PSP_BIN_MAX; i++)
        if (out->bin[i].size && !range_ok(&b, out->bin[i].offset, out->bin[i].size))
            return AMD_FW_E_TRUNCATED;
    if (!out->bin[AMD_PSP_BIN_SOS].size || !out->bin[AMD_PSP_BIN_SYS_DRV].size)
        return AMD_FW_E_MISSING;
    return AMD_FW_OK;
}

/* psp_hw_start() order; values from enum psp_bootloader_cmd */
const amd_psp_bl_step amd_psp_bl_sequence[] = {
    { AMD_PSP_BIN_KDB,      AMD_PSP_BIN_KDB,      0x80000u,    "KDB" },
    /* psp_v13_0_bootloader_load_spl() passes &psp->kdb, not &psp->spl.
     * Mirror Linux exactly until proven otherwise on hardware. */
    { AMD_PSP_BIN_SPL,      AMD_PSP_BIN_KDB,      0x10000000u, "SPL" },
    { AMD_PSP_BIN_SYS_DRV,  AMD_PSP_BIN_SYS_DRV,  0x10000u,    "SYS_DRV" },
    { AMD_PSP_BIN_SOC_DRV,  AMD_PSP_BIN_SOC_DRV,  0xB0000u,    "SOC_DRV" },
    { AMD_PSP_BIN_INTF_DRV, AMD_PSP_BIN_INTF_DRV, 0xD0000u,    "INTF_DRV" },
    { AMD_PSP_BIN_DBG_DRV,  AMD_PSP_BIN_DBG_DRV,  0xC0000u,    "DBG_DRV" },
    { AMD_PSP_BIN_RAS_DRV,  AMD_PSP_BIN_RAS_DRV,  0xE0000u,    "RAS_DRV" },
    { AMD_PSP_BIN_SPDM_DRV, AMD_PSP_BIN_SPDM_DRV, 0x20000000u, "SPDM_DRV" },
    { AMD_PSP_BIN_SOS,      AMD_PSP_BIN_SOS,      0x20000u,    "SOS" },
};
const unsigned amd_psp_bl_sequence_len =
    sizeof(amd_psp_bl_sequence) / sizeof(amd_psp_bl_sequence[0]);

/* ---------------------------- ucodes ---------------------------- */

typedef struct builder {
    amd_ucode_plan *plan;
    const amd_fw_blob_ref *blobs;
    amd_fw_status err;
} builder;

static void add(builder *bd, uint16_t id, uint16_t type, uint8_t which,
                uint32_t off, uint32_t size, const char *name)
{
    blob b = { bd->blobs[which].data, bd->blobs[which].len };
    amd_ucode *u;

    if (bd->err != AMD_FW_OK || size == 0)
        return;
    if (!range_ok(&b, off, size)) {
        bd->err = AMD_FW_E_TRUNCATED;
        return;
    }
    if (bd->plan->count >= AMD_UCODE_MAX) {
        bd->err = AMD_FW_E_TOO_MANY;
        return;
    }
    u = &bd->plan->ucode[bd->plan->count++];
    u->ucode_id = id;
    u->psp_type = type;
    u->blob = which;
    u->offset = off;
    u->size = size;
    u->name = name;
    bd->plan->total_aligned += (size + 4095u) & ~4095u;
}

#define R32(b, type, field, var)                                   \
    do {                                                           \
        if (!rd32(&(b), OFF(type, field), &(var))) {               \
            bd->err = AMD_FW_E_TRUNCATED;                          \
            return;                                                \
        }                                                          \
    } while (0)

static int get(builder *bd, uint8_t which, blob *b, common_hdr *h, int required)
{
    amd_fw_status st;
    b->p = bd->blobs[which].data;
    b->len = bd->blobs[which].len;
    if (!b->p) {
        if (required && bd->err == AMD_FW_OK)
            bd->err = AMD_FW_E_MISSING;
        return 0;
    }
    if ((st = read_common(b, h)) != AMD_FW_OK) {
        if (bd->err == AMD_FW_OK)
            bd->err = st;
        return 0;
    }
    return 1;
}

static void do_default(builder *bd, uint8_t which, uint16_t id, uint16_t type,
                       const char *name, int required)
{
    blob b; common_hdr h;
    if (get(bd, which, &b, &h, required))
        add(bd, id, type, which, h.ucode_array_offset_bytes, h.ucode_size_bytes, name);
}

static void do_sdma(builder *bd)
{
    blob b; common_hdr h;
    uint32_t ctx, ctl_off, ctl_size;
    if (!get(bd, AMD_BLOB_SDMA, &b, &h, 1))
        return;
    if (h.hv_major != 2) { /* v3 (RS64 SDMA) is SDMA 7, not Navi3x */
        bd->err = AMD_FW_E_VERSION;
        return;
    }
    R32(b, sdma_firmware_header_v2_0, ctx_ucode_size_bytes, ctx);
    R32(b, sdma_firmware_header_v2_0, ctl_ucode_offset, ctl_off);
    R32(b, sdma_firmware_header_v2_0, ctl_ucode_size_bytes, ctl_size);
    add(bd, AMD_UCODE_SDMA_UCODE_TH0, GFX_FW_TYPE_SDMA_UCODE_TH0, AMD_BLOB_SDMA,
        h.ucode_array_offset_bytes, ctx, "SDMA_TH0");
    add(bd, AMD_UCODE_SDMA_UCODE_TH1, GFX_FW_TYPE_SDMA_UCODE_TH1, AMD_BLOB_SDMA,
        ctl_off, ctl_size, "SDMA_TH1");
}

/* RS64 CP (gfx_firmware_header_v2_0): ucode + N stacks sharing one data blob */
static void do_rs64(builder *bd, uint8_t which, uint16_t id, uint16_t type,
                    uint16_t stack_id0, uint16_t stack_type0, unsigned nstacks,
                    const char *name, const char *const *stack_names)
{
    blob b; common_hdr h;
    uint32_t usize, dsize, doff;
    unsigned i;
    if (!get(bd, which, &b, &h, 1))
        return;
    if (h.hv_major != 2) {
        bd->err = AMD_FW_E_VERSION;
        return;
    }
    R32(b, gfx_firmware_header_v2_0, ucode_size_bytes, usize);
    R32(b, gfx_firmware_header_v2_0, data_size_bytes, dsize);
    R32(b, gfx_firmware_header_v2_0, data_offset_bytes, doff);
    add(bd, id, type, which, h.ucode_array_offset_bytes, usize, name);
    for (i = 0; i < nstacks; i++)
        add(bd, (uint16_t)(stack_id0 + i), (uint16_t)(stack_type0 + i), which,
            doff, dsize, stack_names[i]);
}

static void do_mes(builder *bd, uint8_t which, uint16_t id, uint16_t type,
                   uint16_t data_id, uint16_t data_type, const char *n1,
                   const char *n2, int required)
{
    blob b; common_hdr h;
    uint32_t us, uo, ds, dof;
    if (!get(bd, which, &b, &h, required))
        return;
    R32(b, mes_firmware_header_v1_0, mes_ucode_size_bytes, us);
    R32(b, mes_firmware_header_v1_0, mes_ucode_offset_bytes, uo);
    R32(b, mes_firmware_header_v1_0, mes_ucode_data_size_bytes, ds);
    R32(b, mes_firmware_header_v1_0, mes_ucode_data_offset_bytes, dof);
    add(bd, id, type, which, uo, us, n1);
    add(bd, data_id, data_type, which, dof, ds, n2);
}

static void do_imu(builder *bd)
{
    blob b; common_hdr h;
    uint32_t is, ds;
    if (!get(bd, AMD_BLOB_IMU, &b, &h, 1))
        return;
    R32(b, imu_firmware_header_v1_0, imu_iram_ucode_size_bytes, is);
    R32(b, imu_firmware_header_v1_0, imu_dram_ucode_size_bytes, ds);
    add(bd, AMD_UCODE_IMU_I, GFX_FW_TYPE_IMU_I, AMD_BLOB_IMU,
        h.ucode_array_offset_bytes, is, "IMU_I");
    add(bd, AMD_UCODE_IMU_D, GFX_FW_TYPE_IMU_D, AMD_BLOB_IMU,
        h.ucode_array_offset_bytes + is, ds, "IMU_D");
}

static void do_rlc(builder *bd)
{
    blob b; common_hdr h;
    uint32_t s, o, s2, o2;
    if (!get(bd, AMD_BLOB_RLC, &b, &h, 1))
        return;
    if (h.hv_major != 2) {
        bd->err = AMD_FW_E_VERSION;
        return;
    }
    if (h.hv_minor >= 1) {
        R32(b, rlc_firmware_header_v2_1, save_restore_list_cntl_size_bytes, s);
        R32(b, rlc_firmware_header_v2_1, save_restore_list_cntl_offset_bytes, o);
        add(bd, AMD_UCODE_RLC_RESTORE_LIST_CNTL, GFX_FW_TYPE_RLC_RESTORE_LIST_SRM_CNTL,
            AMD_BLOB_RLC, o, s, "RLC_RESTORE_LIST_CNTL");
        R32(b, rlc_firmware_header_v2_1, save_restore_list_gpm_size_bytes, s);
        R32(b, rlc_firmware_header_v2_1, save_restore_list_gpm_offset_bytes, o);
        add(bd, AMD_UCODE_RLC_RESTORE_LIST_GPM_MEM, GFX_FW_TYPE_RLC_RESTORE_LIST_GPM_MEM,
            AMD_BLOB_RLC, o, s, "RLC_RESTORE_LIST_GPM");
        R32(b, rlc_firmware_header_v2_1, save_restore_list_srm_size_bytes, s);
        R32(b, rlc_firmware_header_v2_1, save_restore_list_srm_offset_bytes, o);
        add(bd, AMD_UCODE_RLC_RESTORE_LIST_SRM_MEM, GFX_FW_TYPE_RLC_RESTORE_LIST_SRM_MEM,
            AMD_BLOB_RLC, o, s, "RLC_RESTORE_LIST_SRM");
    }
    if (h.hv_minor >= 2) {
        R32(b, rlc_firmware_header_v2_2, rlc_iram_ucode_size_bytes, s);
        R32(b, rlc_firmware_header_v2_2, rlc_iram_ucode_offset_bytes, o);
        R32(b, rlc_firmware_header_v2_2, rlc_dram_ucode_size_bytes, s2);
        R32(b, rlc_firmware_header_v2_2, rlc_dram_ucode_offset_bytes, o2);
        add(bd, AMD_UCODE_RLC_IRAM, GFX_FW_TYPE_RLC_IRAM, AMD_BLOB_RLC, o, s, "RLC_IRAM");
        add(bd, AMD_UCODE_RLC_DRAM, GFX_FW_TYPE_RLC_DRAM_BOOT, AMD_BLOB_RLC, o2, s2, "RLC_DRAM");
    }
    if (h.hv_minor == 3 || h.hv_minor == 4) {
        R32(b, rlc_firmware_header_v2_3, rlcp_ucode_size_bytes, s);
        R32(b, rlc_firmware_header_v2_3, rlcp_ucode_offset_bytes, o);
        R32(b, rlc_firmware_header_v2_3, rlcv_ucode_size_bytes, s2);
        R32(b, rlc_firmware_header_v2_3, rlcv_ucode_offset_bytes, o2);
        add(bd, AMD_UCODE_RLC_P, GFX_FW_TYPE_RLC_P, AMD_BLOB_RLC, o, s, "RLC_P");
        add(bd, AMD_UCODE_RLC_V, GFX_FW_TYPE_RLC_V, AMD_BLOB_RLC, o2, s2, "RLC_V");
    }
    /* v2.4 (tap delays) and v2.5 (second RLC core) are not used by GC 11.0.3 */
    add(bd, AMD_UCODE_RLC_G, GFX_FW_TYPE_RLC_G, AMD_BLOB_RLC,
        h.ucode_array_offset_bytes, h.ucode_size_bytes, "RLC_G");
}

static void do_dmcub(builder *bd)
{
    blob b; common_hdr h;
    uint32_t inst;
    if (!get(bd, AMD_BLOB_DMCUB, &b, &h, 0))
        return;
    R32(b, dmcub_firmware_header_v1_0, inst_const_bytes, inst);
    add(bd, AMD_UCODE_DMCUB, GFX_FW_TYPE_DMUB, AMD_BLOB_DMCUB,
        h.ucode_array_offset_bytes, inst, "DMCUB");
}

static const char *const pfp_stacks[] = { "RS64_PFP_P0_STACK", "RS64_PFP_P1_STACK" };
static const char *const me_stacks[]  = { "RS64_ME_P0_STACK", "RS64_ME_P1_STACK" };
static const char *const mec_stacks[] = { "RS64_MEC_P0_STACK", "RS64_MEC_P1_STACK",
                                          "RS64_MEC_P2_STACK", "RS64_MEC_P3_STACK" };

static void sort_by_id(amd_ucode_plan *p)
{
    uint32_t i, j;
    for (i = 1; i < p->count; i++) {
        amd_ucode t = p->ucode[i];
        for (j = i; j > 0 && p->ucode[j - 1].ucode_id > t.ucode_id; j--)
            p->ucode[j] = p->ucode[j - 1];
        p->ucode[j] = t;
    }
}

amd_fw_status amd_ucode_plan_build(const amd_fw_blob_ref blobs[AMD_BLOB_COUNT],
                                   amd_ucode_plan *plan)
{
    builder bd;
    if (!blobs || !plan)
        return AMD_FW_E_ARGS;
    zero(plan, sizeof(*plan));
    bd.plan = plan;
    bd.blobs = blobs;
    bd.err = AMD_FW_OK;

    do_sdma(&bd);
    do_rs64(&bd, AMD_BLOB_PFP, AMD_UCODE_CP_RS64_PFP, GFX_FW_TYPE_RS64_PFP,
            AMD_UCODE_CP_RS64_PFP_P0_STACK, GFX_FW_TYPE_RS64_PFP_P0_STACK, 2,
            "RS64_PFP", pfp_stacks);
    do_rs64(&bd, AMD_BLOB_ME, AMD_UCODE_CP_RS64_ME, GFX_FW_TYPE_RS64_ME,
            AMD_UCODE_CP_RS64_ME_P0_STACK, GFX_FW_TYPE_RS64_ME_P0_STACK, 2,
            "RS64_ME", me_stacks);
    do_rs64(&bd, AMD_BLOB_MEC, AMD_UCODE_CP_RS64_MEC, GFX_FW_TYPE_RS64_MEC,
            AMD_UCODE_CP_RS64_MEC_P0_STACK, GFX_FW_TYPE_RS64_MEC_P0_STACK, 4,
            "RS64_MEC", mec_stacks);
    do_mes(&bd, AMD_BLOB_MES, AMD_UCODE_CP_MES, GFX_FW_TYPE_CP_MES,
           AMD_UCODE_CP_MES_DATA, GFX_FW_TYPE_MES_STACK, "CP_MES", "CP_MES_DATA", 1);
    do_mes(&bd, AMD_BLOB_MES1, AMD_UCODE_CP_MES1, GFX_FW_TYPE_CP_MES_KIQ,
           AMD_UCODE_CP_MES1_DATA, GFX_FW_TYPE_MES_KIQ_STACK, "CP_MES1", "CP_MES1_DATA", 0);
    do_imu(&bd);
    do_rlc(&bd);
    do_default(&bd, AMD_BLOB_SMC, AMD_UCODE_SMC, GFX_FW_TYPE_SMU, "SMC", 1);
    do_default(&bd, AMD_BLOB_VCN, AMD_UCODE_VCN, GFX_FW_TYPE_VCN, "VCN", 0);
    do_dmcub(&bd);

    if (bd.err != AMD_FW_OK)
        return bd.err;
    sort_by_id(plan);
    return AMD_FW_OK;
}

const amd_ucode *amd_ucode_plan_find(const amd_ucode_plan *plan, uint16_t ucode_id)
{
    uint32_t i;
    for (i = 0; plan && i < plan->count; i++)
        if (plan->ucode[i].ucode_id == ucode_id)
            return &plan->ucode[i];
    return NULL;
}

const char *amd_fw_status_str(amd_fw_status s)
{
    switch (s) {
    case AMD_FW_OK:          return "ok";
    case AMD_FW_E_ARGS:      return "bad arguments";
    case AMD_FW_E_TRUNCATED: return "truncated / out of bounds";
    case AMD_FW_E_VERSION:   return "unsupported header version";
    case AMD_FW_E_MISSING:   return "required firmware missing";
    case AMD_FW_E_TOO_MANY:  return "too many entries";
    }
    return "unknown";
}

const char *const amd_navi32_fw_names[AMD_BLOB_COUNT] = {
    [AMD_BLOB_SMC]   = "smu_13_0_10.bin",
    [AMD_BLOB_SDMA]  = "sdma_6_0_3.bin",
    [AMD_BLOB_PFP]   = "gc_11_0_3_pfp.bin",
    [AMD_BLOB_ME]    = "gc_11_0_3_me.bin",
    [AMD_BLOB_MEC]   = "gc_11_0_3_mec.bin",
    [AMD_BLOB_RLC]   = "gc_11_0_3_rlc.bin",
    [AMD_BLOB_IMU]   = "gc_11_0_3_imu.bin",
    [AMD_BLOB_MES]   = "gc_11_0_3_mes_2.bin",
    [AMD_BLOB_MES1]  = "gc_11_0_3_mes1.bin",
    [AMD_BLOB_DMCUB] = "dcn_3_2_0_dmcub.bin",
    [AMD_BLOB_VCN]   = "vcn_4_0_0.bin",
};
const char *const amd_navi32_sos_name = "psp_13_0_10_sos.bin";
