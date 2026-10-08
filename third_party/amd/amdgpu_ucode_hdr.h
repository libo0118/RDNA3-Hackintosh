/*
 * Copyright 2012 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */
/*
 * RDNA3-Hackintosh: firmware header structures extracted verbatim from
 * Linux drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h (MIT, AMD). Only the
 * on-disk layouts are kept; the driver-side bookkeeping is not.
 */
#ifndef AMDGPU_UCODE_HDR_H
#define AMDGPU_UCODE_HDR_H

#include <stdint.h>

/* gpu_info_firmware_* structs omitted (APU-only, need DC types). */

struct common_firmware_header {
	uint32_t size_bytes; /* size of the entire header+image(s) in bytes */
	uint32_t header_size_bytes; /* size of just the header in bytes */
	uint16_t header_version_major; /* header version */
	uint16_t header_version_minor; /* header version */
	uint16_t ip_version_major; /* IP version */
	uint16_t ip_version_minor; /* IP version */
	uint32_t ucode_version;
	uint32_t ucode_size_bytes; /* size of ucode in bytes */
	uint32_t ucode_array_offset_bytes; /* payload offset from the start of the header */
	uint32_t crc32;  /* crc32 checksum of the payload */
};

/* version_major=1, version_minor=0 */
struct mc_firmware_header_v1_0 {
	struct common_firmware_header header;
	uint32_t io_debug_size_bytes; /* size of debug array in dwords */
	uint32_t io_debug_array_offset_bytes; /* payload offset from the start of the header */
};

/* version_major=1, version_minor=0 */
struct smc_firmware_header_v1_0 {
	struct common_firmware_header header;
	uint32_t ucode_start_addr;
};

/* version_major=2, version_minor=0 */
struct smc_firmware_header_v2_0 {
	struct smc_firmware_header_v1_0 v1_0;
	uint32_t ppt_offset_bytes; /* soft pptable offset */
	uint32_t ppt_size_bytes; /* soft pptable size */
};

struct smc_soft_pptable_entry {
        uint32_t id;
        uint32_t ppt_offset_bytes;
        uint32_t ppt_size_bytes;
};

/* version_major=2, version_minor=1 */
struct smc_firmware_header_v2_1 {
        struct smc_firmware_header_v1_0 v1_0;
        uint32_t pptable_count;
        uint32_t pptable_entry_offset;
};

struct psp_fw_legacy_bin_desc {
	uint32_t fw_version;
	uint32_t offset_bytes;
	uint32_t size_bytes;
};

/* version_major=1, version_minor=0 */
struct psp_firmware_header_v1_0 {
	struct common_firmware_header header;
	struct psp_fw_legacy_bin_desc sos;
};

/* version_major=1, version_minor=1 */
struct psp_firmware_header_v1_1 {
	struct psp_firmware_header_v1_0 v1_0;
	struct psp_fw_legacy_bin_desc toc;
	struct psp_fw_legacy_bin_desc kdb;
};

/* version_major=1, version_minor=2 */
struct psp_firmware_header_v1_2 {
	struct psp_firmware_header_v1_0 v1_0;
	struct psp_fw_legacy_bin_desc res;
	struct psp_fw_legacy_bin_desc kdb;
};

/* version_major=1, version_minor=3 */
struct psp_firmware_header_v1_3 {
	struct psp_firmware_header_v1_1 v1_1;
	struct psp_fw_legacy_bin_desc spl;
	struct psp_fw_legacy_bin_desc rl;
	struct psp_fw_legacy_bin_desc sys_drv_aux;
	struct psp_fw_legacy_bin_desc sos_aux;
};

struct psp_fw_bin_desc {
	uint32_t fw_type;
	uint32_t fw_version;
	uint32_t offset_bytes;
	uint32_t size_bytes;
};

enum psp_fw_type {
	PSP_FW_TYPE_UNKOWN,
	PSP_FW_TYPE_PSP_SOS,
	PSP_FW_TYPE_PSP_SYS_DRV,
	PSP_FW_TYPE_PSP_KDB,
	PSP_FW_TYPE_PSP_TOC,
	PSP_FW_TYPE_PSP_SPL,
	PSP_FW_TYPE_PSP_RL,
	PSP_FW_TYPE_PSP_SOC_DRV,
	PSP_FW_TYPE_PSP_INTF_DRV,
	PSP_FW_TYPE_PSP_DBG_DRV,
	PSP_FW_TYPE_PSP_RAS_DRV,
	PSP_FW_TYPE_PSP_IPKEYMGR_DRV,
	PSP_FW_TYPE_PSP_SPDM_DRV,
	PSP_FW_TYPE_MAX_INDEX,
};

/* version_major=2, version_minor=0 */
struct psp_firmware_header_v2_0 {
	struct common_firmware_header header;
	uint32_t psp_fw_bin_count;
	struct psp_fw_bin_desc psp_fw_bin[];
};

/* version_major=2, version_minor=1 */
struct psp_firmware_header_v2_1 {
	struct common_firmware_header header;
	uint32_t psp_fw_bin_count;
	uint32_t psp_aux_fw_bin_index;
	struct psp_fw_bin_desc psp_fw_bin[];
};

/* version_major=1, version_minor=0 */
struct ta_firmware_header_v1_0 {
	struct common_firmware_header header;
	struct psp_fw_legacy_bin_desc xgmi;
	struct psp_fw_legacy_bin_desc ras;
	struct psp_fw_legacy_bin_desc hdcp;
	struct psp_fw_legacy_bin_desc dtm;
	struct psp_fw_legacy_bin_desc securedisplay;
};

enum ta_fw_type {
	TA_FW_TYPE_UNKOWN,
	TA_FW_TYPE_PSP_ASD,
	TA_FW_TYPE_PSP_XGMI,
	TA_FW_TYPE_PSP_RAS,
	TA_FW_TYPE_PSP_HDCP,
	TA_FW_TYPE_PSP_DTM,
	TA_FW_TYPE_PSP_RAP,
	TA_FW_TYPE_PSP_SECUREDISPLAY,
	TA_FW_TYPE_PSP_XGMI_AUX,
	TA_FW_TYPE_MAX_INDEX,
};

/* version_major=2, version_minor=0 */
struct ta_firmware_header_v2_0 {
	struct common_firmware_header header;
	uint32_t ta_fw_bin_count;
	struct psp_fw_bin_desc ta_fw_bin[];
};

/* version_major=1, version_minor=0 */
struct gfx_firmware_header_v1_0 {
	struct common_firmware_header header;
	uint32_t ucode_feature_version;
	uint32_t jt_offset; /* jt location */
	uint32_t jt_size;  /* size of jt */
};

/* version_major=2, version_minor=0 */
struct gfx_firmware_header_v2_0 {
	struct common_firmware_header header;
	uint32_t ucode_feature_version;
	uint32_t ucode_size_bytes;
	uint32_t ucode_offset_bytes;
	uint32_t data_size_bytes;
	uint32_t data_offset_bytes;
	uint32_t ucode_start_addr_lo;
	uint32_t ucode_start_addr_hi;
};

/* version_major=1, version_minor=0 */
struct mes_firmware_header_v1_0 {
	struct common_firmware_header header;
	uint32_t mes_ucode_version;
	uint32_t mes_ucode_size_bytes;
	uint32_t mes_ucode_offset_bytes;
	uint32_t mes_ucode_data_version;
	uint32_t mes_ucode_data_size_bytes;
	uint32_t mes_ucode_data_offset_bytes;
	uint32_t mes_uc_start_addr_lo;
	uint32_t mes_uc_start_addr_hi;
	uint32_t mes_data_start_addr_lo;
	uint32_t mes_data_start_addr_hi;
};

/* version_major=1, version_minor=0 */
struct rlc_firmware_header_v1_0 {
	struct common_firmware_header header;
	uint32_t ucode_feature_version;
	uint32_t save_and_restore_offset;
	uint32_t clear_state_descriptor_offset;
	uint32_t avail_scratch_ram_locations;
	uint32_t master_pkt_description_offset;
};

/* version_major=2, version_minor=0 */
struct rlc_firmware_header_v2_0 {
	struct common_firmware_header header;
	uint32_t ucode_feature_version;
	uint32_t jt_offset; /* jt location */
	uint32_t jt_size;  /* size of jt */
	uint32_t save_and_restore_offset;
	uint32_t clear_state_descriptor_offset;
	uint32_t avail_scratch_ram_locations;
	uint32_t reg_restore_list_size;
	uint32_t reg_list_format_start;
	uint32_t reg_list_format_separate_start;
	uint32_t starting_offsets_start;
	uint32_t reg_list_format_size_bytes; /* size of reg list format array in bytes */
	uint32_t reg_list_format_array_offset_bytes; /* payload offset from the start of the header */
	uint32_t reg_list_size_bytes; /* size of reg list array in bytes */
	uint32_t reg_list_array_offset_bytes; /* payload offset from the start of the header */
	uint32_t reg_list_format_separate_size_bytes; /* size of reg list format array in bytes */
	uint32_t reg_list_format_separate_array_offset_bytes; /* payload offset from the start of the header */
	uint32_t reg_list_separate_size_bytes; /* size of reg list array in bytes */
	uint32_t reg_list_separate_array_offset_bytes; /* payload offset from the start of the header */
};

/* version_major=2, version_minor=1 */
struct rlc_firmware_header_v2_1 {
	struct rlc_firmware_header_v2_0 v2_0;
	uint32_t reg_list_format_direct_reg_list_length; /* length of direct reg list format array */
	uint32_t save_restore_list_cntl_ucode_ver;
	uint32_t save_restore_list_cntl_feature_ver;
	uint32_t save_restore_list_cntl_size_bytes;
	uint32_t save_restore_list_cntl_offset_bytes;
	uint32_t save_restore_list_gpm_ucode_ver;
	uint32_t save_restore_list_gpm_feature_ver;
	uint32_t save_restore_list_gpm_size_bytes;
	uint32_t save_restore_list_gpm_offset_bytes;
	uint32_t save_restore_list_srm_ucode_ver;
	uint32_t save_restore_list_srm_feature_ver;
	uint32_t save_restore_list_srm_size_bytes;
	uint32_t save_restore_list_srm_offset_bytes;
};

/* version_major=2, version_minor=2 */
struct rlc_firmware_header_v2_2 {
	struct rlc_firmware_header_v2_1 v2_1;
	uint32_t rlc_iram_ucode_size_bytes;
	uint32_t rlc_iram_ucode_offset_bytes;
	uint32_t rlc_dram_ucode_size_bytes;
	uint32_t rlc_dram_ucode_offset_bytes;
};

/* version_major=2, version_minor=3 */
struct rlc_firmware_header_v2_3 {
    struct rlc_firmware_header_v2_2 v2_2;
    uint32_t rlcp_ucode_version;
    uint32_t rlcp_ucode_feature_version;
    uint32_t rlcp_ucode_size_bytes;
    uint32_t rlcp_ucode_offset_bytes;
    uint32_t rlcv_ucode_version;
    uint32_t rlcv_ucode_feature_version;
    uint32_t rlcv_ucode_size_bytes;
    uint32_t rlcv_ucode_offset_bytes;
};

/* version_major=2, version_minor=4 */
struct rlc_firmware_header_v2_4 {
    struct rlc_firmware_header_v2_3 v2_3;
    uint32_t global_tap_delays_ucode_size_bytes;
    uint32_t global_tap_delays_ucode_offset_bytes;
    uint32_t se0_tap_delays_ucode_size_bytes;
    uint32_t se0_tap_delays_ucode_offset_bytes;
    uint32_t se1_tap_delays_ucode_size_bytes;
    uint32_t se1_tap_delays_ucode_offset_bytes;
    uint32_t se2_tap_delays_ucode_size_bytes;
    uint32_t se2_tap_delays_ucode_offset_bytes;
    uint32_t se3_tap_delays_ucode_size_bytes;
    uint32_t se3_tap_delays_ucode_offset_bytes;
};

/* version_major=2, version_minor=5 */
struct rlc_firmware_header_v2_5 {
	struct rlc_firmware_header_v2_2 v2_2;
	uint32_t rlc_1_iram_ucode_size_bytes;
	uint32_t rlc_1_iram_ucode_offset_bytes;
	uint32_t rlc_1_dram_ucode_size_bytes;
	uint32_t rlc_1_dram_ucode_offset_bytes;
};

/* version_major=1, version_minor=0 */
struct sdma_firmware_header_v1_0 {
	struct common_firmware_header header;
	uint32_t ucode_feature_version;
	uint32_t ucode_change_version;
	uint32_t jt_offset; /* jt location */
	uint32_t jt_size; /* size of jt */
};

/* version_major=1, version_minor=1 */
struct sdma_firmware_header_v1_1 {
	struct sdma_firmware_header_v1_0 v1_0;
	uint32_t digest_size;
};

/* version_major=2, version_minor=0 */
struct sdma_firmware_header_v2_0 {
	struct common_firmware_header header;
	uint32_t ucode_feature_version;
	uint32_t ctx_ucode_size_bytes; /* context thread ucode size */
	uint32_t ctx_jt_offset; /* context thread jt location */
	uint32_t ctx_jt_size; /* context thread size of jt */
	uint32_t ctl_ucode_offset;
	uint32_t ctl_ucode_size_bytes; /* control thread ucode size */
	uint32_t ctl_jt_offset; /* control thread jt location */
	uint32_t ctl_jt_size; /* control thread size of jt */
};

/* version_major=1, version_minor=0 */
struct vpe_firmware_header_v1_0 {
	struct common_firmware_header header;
	uint32_t ucode_feature_version;
	uint32_t ctx_ucode_size_bytes; /* context thread ucode size */
	uint32_t ctx_jt_offset; /* context thread jt location */
	uint32_t ctx_jt_size; /* context thread size of jt */
	uint32_t ctl_ucode_offset;
	uint32_t ctl_ucode_size_bytes; /* control thread ucode size */
	uint32_t ctl_jt_offset; /* control thread jt location */
	uint32_t ctl_jt_size; /* control thread size of jt */
};

/* version_major=1, version_minor=0 */
struct umsch_mm_firmware_header_v1_0 {
	struct common_firmware_header header;
	uint32_t umsch_mm_ucode_version;
	uint32_t umsch_mm_ucode_size_bytes;
	uint32_t umsch_mm_ucode_offset_bytes;
	uint32_t umsch_mm_ucode_data_version;
	uint32_t umsch_mm_ucode_data_size_bytes;
	uint32_t umsch_mm_ucode_data_offset_bytes;
	uint32_t umsch_mm_irq_start_addr_lo;
	uint32_t umsch_mm_irq_start_addr_hi;
	uint32_t umsch_mm_uc_start_addr_lo;
	uint32_t umsch_mm_uc_start_addr_hi;
	uint32_t umsch_mm_data_start_addr_lo;
	uint32_t umsch_mm_data_start_addr_hi;
};

/* version_major=3, version_minor=0 */
struct sdma_firmware_header_v3_0 {
    struct common_firmware_header header;
    uint32_t ucode_feature_version;
    uint32_t ucode_offset_bytes;
    uint32_t ucode_size_bytes;
};



/* gpu info payload
 * version_major=1, version_minor=1 */


/* version_major=1, version_minor=0 */
struct dmcu_firmware_header_v1_0 {
	struct common_firmware_header header;
	uint32_t intv_offset_bytes; /* interrupt vectors offset from end of header, in bytes */
	uint32_t intv_size_bytes;  /* size of interrupt vectors, in bytes */
};

/* version_major=1, version_minor=0 */
struct dmcub_firmware_header_v1_0 {
	struct common_firmware_header header;
	uint32_t inst_const_bytes; /* size of instruction region, in bytes */
	uint32_t bss_data_bytes; /* size of bss/data region, in bytes */
};

/* version_major=1, version_minor=0 */
struct imu_firmware_header_v1_0 {
    struct common_firmware_header header;
    uint32_t imu_iram_ucode_size_bytes;
    uint32_t imu_iram_ucode_offset_bytes;
    uint32_t imu_dram_ucode_size_bytes;
    uint32_t imu_dram_ucode_offset_bytes;
};

/* header is fixed size */

#endif /* AMDGPU_UCODE_HDR_H */
