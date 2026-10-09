/*
 * edid.h - the parts of EDID 1.3/1.4 (+ CTA-861 extension) a mode list
 * needs: manufacturer, product, name, detailed timing descriptors, standard
 * timings, established timings. Written from the VESA E-EDID spec; no
 * kernel code involved.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_EDID_H
#define RDNA3_EDID_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RD_EDID_MAX_MODES 32

typedef struct rdna3_mode {
    uint32_t h_active, v_active;
    uint32_t h_total, v_total;
    uint32_t h_front, h_sync, v_front, v_sync;   /* front porch and sync widths */
    uint32_t pixel_khz;                           /* 0 for standard/established timings */
    uint32_t refresh_mhz;                         /* milli-Hz */
    uint8_t  h_sync_pos, v_sync_pos, interlaced, preferred;
} rdna3_mode;

typedef struct rdna3_edid {
    char     manufacturer[4];       /* PNP id, e.g. "DEL" */
    uint16_t product;
    uint32_t serial;
    uint8_t  week, year;            /* year - 1990 */
    uint8_t  version, revision;
    uint8_t  digital;
    uint32_t width_cm, height_cm;
    char     name[14];              /* monitor name descriptor */
    uint32_t nmodes;
    rdna3_mode mode[RD_EDID_MAX_MODES];
    uint32_t extensions;            /* blocks after the base block */
} rdna3_edid;

/* Validate the header and checksums, fill 'out'. Returns 0, or -1 for a bad
 * header / checksum / size. 'len' is a multiple of 128 (base + extensions). */
int edid_parse(const uint8_t *data, uint32_t len, rdna3_edid *out);

/* checksum of one 128-byte block (the bytes sum to 0 mod 256 when valid) */
int edid_block_valid(const uint8_t *block);

#ifdef __cplusplus
}
#endif
#endif
