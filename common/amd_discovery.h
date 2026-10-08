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
    uint8_t  num_base;      /* as reported by HW */
    uint8_t  num_base_kept; /* min(num_base, AMD_DISC_MAX_BASES) */
    /* Register segment bases, in DWORDs (like amdgpu's reg_offset[]).
     * For 64-bit tables these are collapsed exactly like amdgpu does:
     * lower_32_bits(x) & 0x3FFFFFFF. */
    uint32_t base[AMD_DISC_MAX_BASES];
} amd_ip_entry;

typedef struct amd_discovery {
    uint16_t bin_version_major, bin_version_minor;
    uint16_t bin_size;
    uint16_t ipd_version;       /* ip_discovery_header.version */
    uint16_t num_dies;
    uint8_t  base_addr_64_bit;
    uint32_t num_ips;           /* entries stored in ips[] */
    uint32_t num_ips_dropped;   /* entries that did not fit */
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
} amd_disc_status;

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
