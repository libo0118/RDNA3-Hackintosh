/*
 * amd_fw.h - AMD firmware container parsing and the PSP load plan.
 *
 * Mirrors what amdgpu does at sw_init for an RDNA3 dGPU with
 * load_type == AMDGPU_FW_LOAD_PSP:
 *
 *   - psp_init_sos_microcode():  split psp_<ver>_sos.bin into the pieces
 *     the PSP bootloader wants (KDB, SPL, SYS_DRV, SOC/INTF/DBG/RAS drv,
 *     SOS, TOC);
 *   - amdgpu_ucode_init_single_fw() + the per-IP init_microcode code:
 *     cut every other firmware file (PFP/ME/MEC/RLC/IMU/MES/SDMA/SMU/
 *     DMCUB/VCN) into "ucodes", each loaded by one PSP LOAD_IP_FW
 *     command, in ascending AMDGPU_UCODE_ID order (same as amdgpu's
 *     psp_load_non_psp_fw() loop).
 *
 * Everything is bounds-checked; the result references the caller's
 * buffers by offset, it does not copy. Pure C99, no libc.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AMD_FW_H
#define AMD_FW_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum amd_fw_status {
    AMD_FW_OK = 0,
    AMD_FW_E_ARGS,
    AMD_FW_E_TRUNCATED,
    AMD_FW_E_VERSION,     /* unsupported header version */
    AMD_FW_E_MISSING,     /* required blob not provided */
    AMD_FW_E_TOO_MANY,
} amd_fw_status;

/* ---------------- PSP SOS container ---------------- */

/* PSP_FW_TYPE_* from amdgpu_ucode.h */
enum {
    AMD_PSP_BIN_SOS = 1, AMD_PSP_BIN_SYS_DRV, AMD_PSP_BIN_KDB, AMD_PSP_BIN_TOC,
    AMD_PSP_BIN_SPL, AMD_PSP_BIN_RL, AMD_PSP_BIN_SOC_DRV, AMD_PSP_BIN_INTF_DRV,
    AMD_PSP_BIN_DBG_DRV, AMD_PSP_BIN_RAS_DRV, AMD_PSP_BIN_IPKEYMGR_DRV,
    AMD_PSP_BIN_SPDM_DRV, AMD_PSP_BIN_MAX
};

typedef struct amd_psp_bin {
    uint32_t offset;      /* from start of the file, 0 size => absent */
    uint32_t size;
    uint32_t fw_version;
} amd_psp_bin;

typedef struct amd_psp_sos {
    uint32_t    ucode_version;
    amd_psp_bin bin[AMD_PSP_BIN_MAX];   /* indexed by AMD_PSP_BIN_* */
} amd_psp_sos;

amd_fw_status amd_psp_sos_parse(const uint8_t *fw, size_t len, amd_psp_sos *out);

/* ---------------- PSP TA container (psp_<ver>_ta.bin) ---------------- */

/* TA_FW_TYPE_* (enum ta_fw_type, amdgpu_ucode.h); also the sub-index of
 * AMDGPU_INFO_FW_TA */
enum {
    AMD_TA_ASD = 1, AMD_TA_XGMI, AMD_TA_RAS, AMD_TA_HDCP, AMD_TA_DTM, AMD_TA_RAP,
    AMD_TA_SECUREDISPLAY, AMD_TA_XGMI_AUX, AMD_TA_MAX
};

/* the trusted applications of the TA file as amdgpu's psp contexts keep
 * them after psp_init_ta_microcode -> parse_ta_v2_microcode ->
 * parse_ta_bin_descriptor: by TA type, offset from the start of the file,
 * size 0 = not in the file. On MP0 other than 13.0.6 an XGMI_AUX entry
 * lands in the XGMI slot (is_ta_fw_applicable), so bin[AMD_TA_XGMI_AUX]
 * stays empty. */
typedef struct amd_psp_ta {
    uint32_t    ucode_version;
    amd_psp_bin bin[AMD_TA_MAX];
} amd_psp_ta;

amd_fw_status amd_psp_ta_parse(const uint8_t *fw, size_t len, amd_psp_ta *out);

/* AMDGPU_INFO_FW_ASD and AMDGPU_INFO_FW_TA (amdgpu_firmware_info): ASD's
 * version goes into ver[] and feature[] (parse_ta_bin_descriptor sets the
 * ASD feature to its fw_version); ta_ver[] gets each TA's version by
 * AMD_TA_* (their feature versions stay 0: a v2 TA file carries none).
 * A TA the file lacks reads 0, as amdgpu's zeroed context does. */
void amd_fw_ta_versions(const amd_psp_ta *ta, uint32_t ver[32], uint32_t feature[32],
                        uint32_t ta_ver[AMD_TA_MAX]);

/* PSP bootloader commands (enum psp_bootloader_cmd, amdgpu_psp.h) in the
 * order psp_hw_start() issues them. SOS goes last and has its own
 * handshake (C2PMSG_81 sign of life). */
typedef struct amd_psp_bl_step {
    uint8_t  bin;        /* AMD_PSP_BIN_* that must be present for the step */
    uint8_t  src;        /* AMD_PSP_BIN_* whose bytes are copied (see SPL) */
    uint32_t bl_cmd;     /* written to C2PMSG_35 */
    const char *name;
} amd_psp_bl_step;
extern const amd_psp_bl_step amd_psp_bl_sequence[];
extern const unsigned amd_psp_bl_sequence_len;

/* ---------------- non-PSP firmware → ucode list ---------------- */

enum amd_fw_blob {
    AMD_BLOB_SMC = 0,     /* smu_13_0_10.bin       */
    AMD_BLOB_SDMA,        /* sdma_6_0_3.bin       */
    AMD_BLOB_PFP,         /* gc_11_0_3_pfp.bin    */
    AMD_BLOB_ME,          /* gc_11_0_3_me.bin     */
    AMD_BLOB_MEC,         /* gc_11_0_3_mec.bin    */
    AMD_BLOB_RLC,         /* gc_11_0_3_rlc.bin    */
    AMD_BLOB_IMU,         /* gc_11_0_3_imu.bin    */
    AMD_BLOB_MES,         /* gc_11_0_3_mes_2.bin  (scheduler pipe) */
    AMD_BLOB_MES1,        /* gc_11_0_3_mes1.bin   (KIQ pipe, optional) */
    AMD_BLOB_DMCUB,       /* dcn_3_2_0_dmcub.bin  */
    AMD_BLOB_VCN,         /* vcn_4_0_0.bin        (optional) */
    AMD_BLOB_COUNT
};

typedef struct amd_fw_blob_ref {
    const uint8_t *data;   /* NULL = not provided */
    size_t         len;
} amd_fw_blob_ref;

/* AMDGPU_UCODE_ID_* values we use (must match amdgpu_ucode.h order) */
enum {
    AMD_UCODE_SDMA_UCODE_TH0 = 9,
    AMD_UCODE_SDMA_UCODE_TH1 = 10,
    AMD_UCODE_CP_RS64_PFP = 15,
    AMD_UCODE_CP_RS64_ME,
    AMD_UCODE_CP_RS64_MEC,
    AMD_UCODE_CP_RS64_PFP_P0_STACK,
    AMD_UCODE_CP_RS64_PFP_P1_STACK,
    AMD_UCODE_CP_RS64_ME_P0_STACK,
    AMD_UCODE_CP_RS64_ME_P1_STACK,
    AMD_UCODE_CP_RS64_MEC_P0_STACK,
    AMD_UCODE_CP_RS64_MEC_P1_STACK,
    AMD_UCODE_CP_RS64_MEC_P2_STACK,
    AMD_UCODE_CP_RS64_MEC_P3_STACK,   /* 25 */
    AMD_UCODE_CP_MES = 30,
    AMD_UCODE_CP_MES_DATA,
    AMD_UCODE_CP_MES1,
    AMD_UCODE_CP_MES1_DATA,
    AMD_UCODE_IMU_I,
    AMD_UCODE_IMU_D,
    AMD_UCODE_GLOBAL_TAP_DELAYS,
    AMD_UCODE_SE0_TAP_DELAYS,
    AMD_UCODE_SE1_TAP_DELAYS,
    AMD_UCODE_SE2_TAP_DELAYS,
    AMD_UCODE_SE3_TAP_DELAYS,
    AMD_UCODE_RLC_RESTORE_LIST_CNTL,
    AMD_UCODE_RLC_RESTORE_LIST_GPM_MEM,
    AMD_UCODE_RLC_RESTORE_LIST_SRM_MEM,
    AMD_UCODE_RLC_IRAM,
    AMD_UCODE_RLC_DRAM,
    AMD_UCODE_RLC_IRAM_1,
    AMD_UCODE_RLC_DRAM_1,
    AMD_UCODE_RLC_P,
    AMD_UCODE_RLC_V,
    AMD_UCODE_RLC_G,                  /* 50 */
    AMD_UCODE_STORAGE,
    AMD_UCODE_SMC,                    /* 52 */
    AMD_UCODE_VCN = 57,
    AMD_UCODE_DMCUB = 63,
};

typedef struct amd_ucode {
    uint16_t ucode_id;   /* AMD_UCODE_* (AMDGPU_UCODE_ID_*) */
    uint16_t psp_type;   /* enum psp_gfx_fw_type (psp_gfx_if.h) */
    uint8_t  blob;       /* enum amd_fw_blob */
    uint32_t offset;     /* payload offset inside the blob */
    uint32_t size;       /* payload size in bytes */
    const char *name;
} amd_ucode;

#define AMD_UCODE_MAX 48

typedef struct amd_ucode_plan {
    uint32_t  count;
    amd_ucode ucode[AMD_UCODE_MAX];
    uint32_t  total_aligned;   /* sum of ALIGN(size, 4096): fw buffer size */
} amd_ucode_plan;

/* Build the ordered LOAD_IP_FW list. SMC is included (amdgpu loads it
 * from psp_hw_start() before TMR setup because Navi3x has
 * pmfw_centralized_cstate_management); the caller decides when. */
amd_fw_status amd_ucode_plan_build(const amd_fw_blob_ref blobs[AMD_BLOB_COUNT],
                                   amd_ucode_plan *plan);

const amd_ucode *amd_ucode_plan_find(const amd_ucode_plan *plan, uint16_t ucode_id);

const char *amd_fw_status_str(amd_fw_status s);

/* linux-firmware file names for Navi 32 (GC 11.0.3), index by enum amd_fw_blob */
extern const char *const amd_navi32_fw_names[AMD_BLOB_COUNT];
extern const char *const amd_navi32_sos_name;
extern const char *const amd_navi32_ta_name;   /* optional: only versions come from it */

/* AMDGPU_INFO_FW_VERSION (amdgpu_firmware_info): version and feature of
 * each firmware, by AMDGPU_INFO_FW_* index (< 32), from the blobs' headers
 * the way amdgpu's init_microcode functions keep them; sos may be NULL.
 * Entries without a source stay 0. Runtime versions (SMC from the PMFW,
 * MES from CP_MES_GP3_LO) are the caller's. */
void amd_fw_versions(const amd_fw_blob_ref blobs[AMD_BLOB_COUNT], const amd_psp_sos *sos,
                     uint32_t ver[32], uint32_t feature[32]);

#ifdef __cplusplus
}
#endif
#endif /* AMD_FW_H */
