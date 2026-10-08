/*
 * amd_discovery_dump - decode an AMD IP discovery binary on the host.
 *
 * Get a real blob from a Linux box with the card on amdgpu:
 *     sudo cat /sys/kernel/debug/dri/<N>/amdgpu_discovery > navi32.bin
 * or from our kext (Stage 1) via `ioreg`, see docs/STAGE1.md.
 *
 * Usage: amd_discovery_dump <file>
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include "amd_discovery.h"

/* amdgpu names firmware "<block>_<maj>_<min>_<rev>*.bin"
 * (amdgpu_ucode_ip_version_decode). Print the expected prefix so the
 * person doing bring-up knows which linux-firmware files to fetch. */
static const char *fw_block(uint16_t hw_id)
{
    switch (hw_id) {
    case AMD_HWID_GC:    return "gc";
    case AMD_HWID_MP0:   return "psp";
    case AMD_HWID_MP1:   return "smu";
    case AMD_HWID_SDMA0: return "sdma";
    case AMD_HWID_VCN:   return "vcn";
    case AMD_HWID_DMU:   return "dcn (dmcub)";
    default:             return NULL;
    }
}

int main(int argc, char **argv)
{
    static amd_discovery d;
    static uint8_t buf[1u << 16];
    FILE *f;
    size_t len;
    amd_disc_status st;
    uint32_t i;
    unsigned k;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <discovery.bin>\n", argv[0]);
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    len = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    st = amd_discovery_parse(buf, len, &d);
    if (st != AMD_DISC_OK) {
        fprintf(stderr, "parse failed: %s\n", amd_disc_status_str(st));
        return 1;
    }

    printf("binary v%u.%u, %u bytes; IP table v%u, %u die(s), %s bases\n",
           d.bin_version_major, d.bin_version_minor, d.bin_size,
           d.ipd_version, d.num_dies, d.base_addr_64_bit ? "64-bit" : "32-bit");
    printf("%-4s %-12s %4s %4s  %-10s  %s\n",
           "die", "ip", "hwid", "inst", "version", "bases (dwords)");
    for (i = 0; i < d.num_ips; i++) {
        const amd_ip_entry *e = &d.ips[i];
        const char *n = amd_hwid_name(e->hw_id);
        char ver[24];
        snprintf(ver, sizeof(ver), "%u.%u.%u", e->major, e->minor, e->revision);
        printf("%-4u %-12s %4u %4u  %-10s ", e->die, n ? n : "?",
               e->hw_id, e->instance, ver);
        for (k = 0; k < e->num_base_kept; k++)
            printf(" 0x%08x", e->base[k]);
        if (e->num_base > e->num_base_kept)
            printf(" (+%u more)", e->num_base - e->num_base_kept);
        printf("\n");
    }
    if (d.num_ips_dropped)
        printf("warning: %u entries dropped (AMD_DISC_MAX_IPS)\n",
               d.num_ips_dropped);

    printf("\nExpected linux-firmware prefixes (amdgpu/<prefix>*.bin):\n");
    for (i = 0; i < d.num_ips; i++) {
        const amd_ip_entry *e = &d.ips[i];
        const char *b = fw_block(e->hw_id);
        if (!b || e->instance != 0)
            continue;
        printf("  %-12s -> %s_%u_%u_%u\n", amd_hwid_name(e->hw_id), b,
               e->major, e->minor, e->revision);
    }
    return 0;
}
