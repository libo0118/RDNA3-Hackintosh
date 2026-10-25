/* Metal framebuffer fetch decisions (metal/plugin/nvmtl_fbfetch_plan.h): which pipelines are served, which attachments can be inputs,
 * the render-pass variant key, the pInputAttachments array, the restart rule.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include "nvmtl_fbfetch_plan.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
    unsigned why; uint32_t sv, zr;
    nvmtl_fbf_in in = { .read_mask = 1, .ncol = 1, .cfmt_present = 1, .cfmt_int = 0, .samples = 1, .layout_ok = 1 };
#define PLAN() nvmtl_fbf_plan(&in, &sv, &zr, &why)
    PLAN(); CHECK(sv == 1 && zr == 0 && why == NVMTL_FBF_OK);
    /* nothing read: nothing to serve or zero, no reason */
    in.read_mask = 0; PLAN(); CHECK(sv == 0 && zr == 0 && why == NVMTL_FBF_OK); in.read_mask = 1;
    /* MRT: reads of slots 0 and 2 */
    in.ncol = 3; in.cfmt_present = 7; in.read_mask = 5; PLAN(); CHECK(sv == 5 && zr == 0 && why == NVMTL_FBF_OK);
    /* the card run's Terminal: the module reads colour(0..2) but the pipeline has ONE colour attachment: slot 0 served, 1 and 2 zeroed */
    in.ncol = 1; in.cfmt_present = 1; in.read_mask = 7; PLAN(); CHECK(sv == 1 && zr == 6 && why == NVMTL_FBF_NO_ATTACHMENT);
    /* a hole in the pipeline's formats; beyond ncol; beyond 8 */
    in.ncol = 3; in.cfmt_present = 5; in.read_mask = 3; PLAN(); CHECK(sv == 1 && zr == 2 && why == NVMTL_FBF_NO_ATTACHMENT);
    in.cfmt_present = 7; in.ncol = 2; in.read_mask = 4; PLAN(); CHECK(sv == 0 && zr == 4 && why == NVMTL_FBF_NO_ATTACHMENT);
    in.ncol = 3; in.read_mask = 1u << 9; PLAN(); CHECK(sv == 0 && zr == 0);                      /* bit 9 is not a binding of the band: ignored */
    /* an integer slot is zeroed, the float one is served */
    in.read_mask = 3; in.cfmt_int = 2; PLAN(); CHECK(sv == 1 && zr == 2 && why == NVMTL_FBF_INTEGER_FORMAT);
    in.cfmt_int = 0;
    /* global reasons zero every read: multisample, no layout, forced zero, tessellation */
    in.read_mask = 3; in.samples = 4; PLAN(); CHECK(sv == 0 && zr == 3 && why == NVMTL_FBF_MULTISAMPLE); in.samples = 1;
    in.layout_ok = 0; PLAN(); CHECK(sv == 0 && zr == 3 && why == NVMTL_FBF_NO_LAYOUT); in.layout_ok = 1;
    in.forced_zero = 1; PLAN(); CHECK(sv == 0 && zr == 3 && why == NVMTL_FBF_FORCED_ZERO); in.forced_zero = 0;
    in.tess = 1; PLAN(); CHECK(sv == 0 && zr == 3 && why == NVMTL_FBF_TESS); in.tess = 0;
    in.read_mask = 1; nvmtl_fbf_plan(&in, NULL, NULL, NULL); PLAN(); CHECK(sv == 1);
    for (unsigned r = 0; r < NVMTL_FBF_REASONS; r++) CHECK(nvmtl_fbf_reason_text(r)[0]);
    CHECK(!strcmp(nvmtl_fbf_reason_text(999), "?"));

    /* attachment facts */
    CHECK(nvmtl_fbf_attachment_reason(1, 1, 2, 1) == NVMTL_FBF_OK);
    CHECK(nvmtl_fbf_attachment_reason(1, 0, 3, 1) == NVMTL_FBF_OK);              /* a slice of a 2D array: a 2D view */
    CHECK(nvmtl_fbf_attachment_reason(4, 1, 4, 1) == NVMTL_FBF_MULTISAMPLE);
    CHECK(nvmtl_fbf_attachment_reason(1, 6, 3, 1) == NVMTL_FBF_LAYERED);
    CHECK(nvmtl_fbf_attachment_reason(1, 1, 7, 1) == NVMTL_FBF_VOLUME);
    CHECK(nvmtl_fbf_attachment_reason(1, 1, 2, 0) == NVMTL_FBF_NO_USAGE);

    /* variant key */
    CHECK(nvmtl_fbf_rp_key(0, 0) == 0 && nvmtl_fbf_rp_key(3, 0) == 3);           /* no input attachment: the plain key (legacy slots keep working) */
    CHECK(nvmtl_fbf_rp_key(0x3ff, 0x81) == (0x3ffu | (0x81u << 16)));
    CHECK(nvmtl_fbf_key_load(nvmtl_fbf_rp_key(0x3ff, 0x81)) == 0x3ff && nvmtl_fbf_key_fbin(nvmtl_fbf_rp_key(0x3ff, 0x81)) == 0x81);
    CHECK(nvmtl_fbf_rp_key(0, 1) != nvmtl_fbf_rp_key(0, 2) && nvmtl_fbf_rp_key(1, 1) != nvmtl_fbf_rp_key(1, 0));
    CHECK(nvmtl_fbf_rp_key(0x10000u | 5u, 0) == 5u);                              /* load bits never leak into the fbin field */

    /* pInputAttachments: slot 1 has no attachment (index UNUSED = ~0) */
    { const uint32_t U = ~0u, att[4] = { 0, U, 1, 2 }; uint32_t out[8];
      CHECK(nvmtl_fbf_input_refs(0, 4, att, U, out) == 0 && out[0] == U && out[2] == U);
      CHECK(nvmtl_fbf_input_refs(1, 4, att, U, out) == 1 && out[0] == 0 && out[1] == U);
      CHECK(nvmtl_fbf_input_refs(0xf, 4, att, U, out) == 4 && out[0] == 0 && out[1] == U && out[2] == 1 && out[3] == 2);
      CHECK(nvmtl_fbf_input_refs(2, 4, att, U, out) == 0 && out[1] == U);             /* bit set for a slot without attachment: nothing */
      CHECK(nvmtl_fbf_input_refs(0x100, 4, att, U, out) == 0);                       /* bit beyond ncol ignored */
      CHECK(nvmtl_fbf_input_refs(4, 4, att, U, out) == 3 && out[2] == 1); }

    /* restart rule and the pass's mask */
    CHECK(nvmtl_fbf_missing(0, 1) == 1 && nvmtl_fbf_missing(1, 1) == 0 && nvmtl_fbf_missing(3, 1) == 0 && nvmtl_fbf_missing(1, 3) == 2 && nvmtl_fbf_missing(5, 0) == 0);
    CHECK(nvmtl_fbf_pass_fbin(0, 1, 1, 1) == 1 && nvmtl_fbf_pass_fbin(2, 1, 3, 1) == 3 && nvmtl_fbf_pass_fbin(2, 1, 1, 1) == 1);
    CHECK(nvmtl_fbf_pass_fbin(3, 3, 3, 4) == 0);                                  /* multisample: never */
    CHECK(nvmtl_fbf_pass_fbin(0x1ff, 0, 0x1ff, 1) == 0xff);
    /* a restart never ping-pongs: the union covers both pipelines */
    { uint32_t fbin = 1; const uint32_t a = 1, b = 2;
      CHECK(nvmtl_fbf_missing(fbin, b) == 2); fbin = nvmtl_fbf_pass_fbin(fbin, b, 3, 1);
      CHECK(!nvmtl_fbf_missing(fbin, a) && !nvmtl_fbf_missing(fbin, b)); }
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("test_fbfetch_plan: ok");
    return 0;
}
