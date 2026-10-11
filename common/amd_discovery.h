/*
 * amd_discovery.h - portable parser for the AMD "IP discovery" binary.
 *
 * The discovery binary lives in the last 64 KiB of VRAM (put there by the
 * PSP/IFWI at power-on) and describes every IP block of the ASIC: its
 * hardware ID, instance, version (e.g. GC 11.0.3 on Navi 32) and the MMIO
 * register base addresses. All later bring-up code (Stage 2+) addresses
 * registers as  base[hw_ip][inst][seg] + reg_offset,  exactly like
 * amdgpu's SOC15_REG_OFFSET(), so this table is the root of everything.
 *
 * Layout follows drivers/gpu/drm/amd/include/discovery.h (MIT, AMD) and
 * amdgpu_discovery.c in Linux. This file is a clean, bounds-checked
 * re-implementation that does not cast packed structs over the buffer:
 * the data comes from hardware and must never be able to crash the kernel.
 *
 * Pure C99, no libc, no allocation: builds in an XNU kext and in user space.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AMD_DISCOVERY_H
#define AMD_DISCOVERY_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- constants from Linux include/discovery.h / amdgpu_discovery.h --- */
#define AMD_DISC_BINARY_SIGNATURE      0x28211407u
#define AMD_DISC_TABLE_SIGNATURE       0x53445049u /* "IPDS" */
#define AMD_DISC_GC_TABLE_ID           0x4347u
#define AMD_DISC_HARVEST_SIGNATURE     0x56524148u
#define AMD_DISC_VCN_INFO_TABLE_ID     0x004E4356u
#define AMD_DISC_HARVEST_TABLE_SIZE    136u   /* sizeof(struct harvest_table) */
#define AMD_DISC_HWIP_MAX_INSTANCE     44u    /* HWIP_MAX_INSTANCE */
#define AMD_DISC_HW_ID_MAX             300u   /* HW_ID_MAX */
#define AMD_DISC_TMR_OFFSET            (64u << 10) /* from end of VRAM */
#define AMD_DISC_TMR_SIZE              (10u << 10) /* bytes amdgpu reads */
#define AMD_DISC_MAX_DIES              16u

/* table_list[] indices */
enum {
    AMD_DISC_TABLE_IP_DISCOVERY = 0,
    AMD_DISC_TABLE_GC           = 1,
    AMD_DISC_TABLE_HARVEST_INFO = 2,
    AMD_DISC_TABLE_VCN_INFO     = 3,
    AMD_DISC_TABLE_MALL_INFO    = 4,
    AMD_DISC_TABLE_NPS_INFO     = 5,
    AMD_DISC_TOTAL_TABLES       = 6
};

/* Hardware IDs we care about (soc15_hw_ip.h) */
enum {
    AMD_HWID_MP1    = 1,   /* SMU */
    AMD_HWID_THM    = 3,
    AMD_HWID_SMUIO  = 4,
    AMD_HWID_CLKA   = 6,
    AMD_HWID_GC     = 11,  /* graphics/compute */
    AMD_HWID_VCN    = 12,  /* == UVD_HWID */
    AMD_HWID_MMHUB  = 34,
    AMD_HWID_ATHUB  = 35,
    AMD_HWID_OSSSYS = 40,  /* interrupt handler (IH) */
    AMD_HWID_HDP    = 41,
    AMD_HWID_SDMA0  = 42,
    AMD_HWID_SDMA1  = 43,
    AMD_HWID_DF     = 46,
    AMD_HWID_NBIF   = 108, /* NBIO */
    AMD_HWID_UMC    = 150,
    AMD_HWID_MP0    = 255, /* PSP */
    AMD_HWID_DMU    = 271, /* display (DCN) */
};

/* Limits for the flat output array. Navi 3x has well under 100 entries. */
#define AMD_DISC_MAX_IPS      256u
#define AMD_DISC_MAX_BASES    8u

typedef struct amd_ip_entry {
    uint16_t hw_id;
    uint8_t  die;
    uint8_t  instance;
    uint8_t  major, minor, revision;
    uint8_t  variant;       /* table v3+, else 0 */
    uint8_t  sub_revision;  /* table v3+, else 0 */
    uint8_t  harvest;       /* table v1/v2 only, raw nibble */
    uint8_t  vcn_config;    /* VCN: revision bits [7:6] (encode / decode
                             * disabled), cleared from 'revision' as
                             * amdgpu_discovery_reg_base_init does */
    uint8_t  num_base;      /* as reported by HW */
    uint8_t  num_base_kept; /* min(num_base, AMD_DISC_MAX_BASES) */
    /* Register segment bases, in DWORDs (like amdgpu's reg_offset[]).
     * For 64-bit tables these are collapsed exactly like amdgpu does:
     * lower_32_bits(x) & 0x3FFFFFFF. */
    uint32_t base[AMD_DISC_MAX_BASES];
} amd_ip_entry;

/* GC info table (discovery.h gc_info_v1_x), as amdgpu_discovery_get_gfx_info() reads it */
typedef struct amd_gc_info {
    uint16_t version_major, version_minor;
    uint32_t num_se, num_wgp0_per_sa, num_wgp1_per_sa, num_rb_per_se, num_gl2c;
    uint32_t num_gprs, num_max_gs_thds, gs_table_depth, gsprim_buff_depth;
    uint32_t parameter_cache_depth, double_offchip_lds_buffer, wave_size;
    uint32_t max_waves_per_simd, max_scratch_slots_per_cu, lds_size;
    uint32_t num_sc_per_se, num_sa_per_se, num_packer_per_sc, num_gl2a;
    /* v1.1+ */
    uint32_t num_tcp_per_sa, num_sdp_interface, num_tcps;
    /* v1.2+ */
    uint32_t num_tcp_per_wpg, tcp_l1_size, num_sqc_per_wgp;
    uint32_t l1_instruction_cache_size_per_sqc, l1_data_cache_size_per_sqc;
    uint32_t gl1c_per_sa, gl1c_size_per_instance, gl2c_per_gpu;
} amd_gc_info;

typedef struct amd_discovery {
    uint16_t bin_version_major, bin_version_minor;
    uint16_t bin_size;
    uint16_t ipd_version;       /* ip_discovery_header.version */
    uint16_t num_dies;
    uint8_t  base_addr_64_bit;
    uint32_t num_ips;           /* entries stored in ips[] */
    uint32_t num_ips_dropped;   /* entries that did not fit */
    uint32_t num_ips_invalid;   /* skipped by amdgpu_discovery_validate_ip */
    uint8_t  has_gc_info;
    amd_gc_info gc;
    amd_ip_entry ips[AMD_DISC_MAX_IPS];
} amd_discovery;

typedef enum amd_disc_status {
    AMD_DISC_OK = 0,
    AMD_DISC_E_ARGS,
    AMD_DISC_E_TRUNCATED,      /* buffer shorter than a header says */
    AMD_DISC_E_BIN_SIGNATURE,
    AMD_DISC_E_BIN_VERSION,
    AMD_DISC_E_BIN_CHECKSUM,
    AMD_DISC_E_TABLE_SIGNATURE,
    AMD_DISC_E_TABLE_CHECKSUM,
    AMD_DISC_E_BAD_DIE,
    /* amdgpu_discovery_init's checks of the optional tables */
    AMD_DISC_E_GC_TABLE,       /* GC table id or checksum */
    AMD_DISC_E_HARVEST_TABLE,  /* harvest table signature or checksum */
    AMD_DISC_E_VCN_TABLE,      /* VCN info table id or checksum */
} amd_disc_status;

/* What amdgpu derives from the binary beyond the IP list, for one ASIC:
 * amdgpu_discovery_reg_base_init's instance counts and masks,
 * amdgpu_discovery_harvest_ip (the harvest table, or the per-IP harvest
 * bits on the Navi 1x boards that need them, then
 * amdgpu_discovery_harvest_config_quirk), amdgpu_discovery_get_mall_info
 * and amdgpu_discovery_get_vcn_info. */
#define AMD_DISC_MAX_VCN_INSTANCES   4u    /* AMDGPU_MAX_VCN_INSTANCES */
#define AMD_DISC_MAX_SDMA_INSTANCES  16u   /* AMDGPU_MAX_SDMA_INSTANCES */
#define AMD_DISC_VCN_HARVEST_VCN0    (1u << 0)   /* AMDGPU_VCN_HARVEST_VCN0 */
#define AMD_DISC_VCN_HARVEST_VCN1    (1u << 1)
#define AMD_DISC_HARVEST_IP_VCN      0x1u  /* AMD_HARVEST_IP_VCN_MASK */
#define AMD_DISC_HARVEST_IP_JPEG     0x2u
#define AMD_DISC_HARVEST_IP_DMU      0x4u
#define AMD_HWID_SDMA2  68
#define AMD_HWID_SDMA3  69

typedef struct amd_disc_soc {
    uint32_t num_vcn_inst, vcn_inst_mask, jpeg_inst_mask;
    uint8_t  vcn_config[AMD_DISC_MAX_VCN_INSTANCES];
    uint32_t vcn_harvest_config, jpeg_harvest_config;
    uint32_t vcn_codec_disable_mask[AMD_DISC_MAX_VCN_INSTANCES];
    uint32_t num_sdma, sdma_mask;
    uint32_t xcc_mask;
    uint32_t num_umc, umc_active_mask;
    uint32_t harvest_ip_mask;       /* AMD_DISC_HARVEST_IP_* */
    uint64_t mall_size;             /* bytes; 0 without a MALL table */
    uint32_t m_half_use;
    uint8_t  harvest_from_ip_bits;  /* the Navi 1x path was taken */
} amd_disc_soc;

/* 'bin'/'len' the binary amd_discovery_parse accepted into 'd'; the PCI
 * device id and revision select amdgpu's board quirks. Returns
 * AMD_DISC_E_ARGS or AMD_DISC_OK; a MALL or VCN table of an unknown
 * version leaves its fields 0 (amdgpu logs it and goes on). */
amd_disc_status amd_discovery_soc_config(const uint8_t *bin, size_t len, const amd_discovery *d,
                                         uint16_t pci_device, uint8_t pci_revision,
                                         amd_disc_soc *out);
/* amdgpu_discovery_get_harvest_info: 1 when instance 'inst' of 'hw_id' is
 * harvested (VCN, DMU, GC, SDMA0; UMC is "TODO" in amdgpu and reads 0) */
uint8_t amd_discovery_get_harvest_info(const amd_disc_soc *s, uint16_t hw_id, uint8_t inst);

/* Byte-sum checksum used by every discovery table. */
uint16_t amd_disc_checksum(const uint8_t *p, size_t n);

/* Validate and decode a discovery binary of |len| bytes. */
amd_disc_status amd_discovery_parse(const uint8_t *bin, size_t len,
                                    amd_discovery *out);

/* Returns NULL when not present. */
const amd_ip_entry *amd_discovery_find(const amd_discovery *d,
                                       uint16_t hw_id, uint8_t instance);

/* Short IP name ("GC", "MP1", ...) or NULL for unknown IDs. */
const char *amd_hwid_name(uint16_t hw_id);

const char *amd_disc_status_str(amd_disc_status s);

#ifdef __cplusplus
}
#endif

#endif /* AMD_DISCOVERY_H */
