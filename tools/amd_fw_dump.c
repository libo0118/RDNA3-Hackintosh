/*
 * amd_fw_dump - show the PSP load plan amdgpu would use for a Navi 32,
 * computed by common/amd_fw.c from a directory of linux-firmware files.
 *
 * Usage: amd_fw_dump <dir-with-amdgpu-firmware>
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amd_fw.h"

static uint8_t *slurp(const char *dir, const char *name, size_t *len)
{
    char path[1024];
    FILE *f;
    uint8_t *buf;
    long n;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(n > 0 ? (size_t)n : 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

int main(int argc, char **argv)
{
    static amd_ucode_plan plan;
    amd_fw_blob_ref blobs[AMD_BLOB_COUNT];
    amd_psp_sos sos;
    amd_fw_status st;
    size_t len = 0;
    uint8_t *sosbuf;
    unsigned i;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <firmware-dir>\n", argv[0]);
        return 2;
    }

    sosbuf = slurp(argv[1], amd_navi32_sos_name, &len);
    if (!sosbuf) {
        fprintf(stderr, "missing %s\n", amd_navi32_sos_name);
        return 1;
    }
    st = amd_psp_sos_parse(sosbuf, len, &sos);
    printf("%s: %s, ucode 0x%08x\n", amd_navi32_sos_name, amd_fw_status_str(st),
           sos.ucode_version);
    if (st != AMD_FW_OK)
        return 1;
    printf("PSP bootloader sequence:\n");
    for (i = 0; i < amd_psp_bl_sequence_len; i++) {
        const amd_psp_bl_step *s = &amd_psp_bl_sequence[i];
        const amd_psp_bin *b = &sos.bin[s->bin];
        if (!b->size) {
            printf("  %-9s (absent, skipped)\n", s->name);
            continue;
        }
        printf("  %-9s cmd 0x%08x  copies bin %u: off 0x%06x size %6u ver 0x%08x\n",
               s->name, s->bl_cmd, s->src, sos.bin[s->src].offset,
               sos.bin[s->src].size, sos.bin[s->src].fw_version);
    }
    if (sos.bin[AMD_PSP_BIN_TOC].size)
        printf("  TOC present (%u bytes): used by LOAD_TOC to size the TMR\n",
               sos.bin[AMD_PSP_BIN_TOC].size);

    memset(blobs, 0, sizeof(blobs));
    for (i = 0; i < AMD_BLOB_COUNT; i++) {
        blobs[i].data = slurp(argv[1], amd_navi32_fw_names[i], &blobs[i].len);
        if (!blobs[i].data)
            printf("  (not found: %s)\n", amd_navi32_fw_names[i]);
    }
    st = amd_ucode_plan_build(blobs, &plan);
    printf("\nLOAD_IP_FW plan: %s, %u ucodes, fw buffer %u KiB\n",
           amd_fw_status_str(st), plan.count, plan.total_aligned / 1024);
    if (st != AMD_FW_OK)
        return 1;
    for (i = 0; i < plan.count; i++) {
        const amd_ucode *u = &plan.ucode[i];
        printf("  id %2u  psp type %2u  %-24s %-22s off 0x%06x size %7u\n",
               u->ucode_id, u->psp_type, u->name, amd_navi32_fw_names[u->blob],
               u->offset, u->size);
    }
    for (i = 0; i < AMD_BLOB_COUNT; i++)
        free((void *)blobs[i].data);
    free(sosbuf);
    return 0;
}
