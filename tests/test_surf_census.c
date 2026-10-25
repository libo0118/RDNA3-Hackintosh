/* The surface census (metal/plugin/nvmtl_surf_census.h, DRIVER-PLAN N50): the per-process table of what touched the IOSurface-backed render targets and the line the
 * plugin prints from it (`census: SUMMARY`, read by tools/triage_glyphs.py). SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include "nvmtl_surf_census.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
    static nvc_table t; char out[2048];
    CHECK(nvc_format(&t, out, sizeof out, 5) == 0 && out[0] == 0);              /* an empty table says nothing */

    nvc_note(&t, 325, 640, 353, 1, NVC_PASS, 1);
    nvc_note(&t, 325, 640, 353, 1, NVC_PASS, 1);
    nvc_note(&t, 325, 640, 353, 1, NVC_PASS_CLEAR, 1);
    nvc_note(&t, 325, 640, 353, 1, NVC_DRAW, 5); nvc_note(&t, 325, 640, 353, 1, NVC_DRAW_BLIT, 3); nvc_note(&t, 325, 640, 353, 1, NVC_DRAW_FBF, 2);
    nvc_note(&t, 325, 640, 353, 1, NVC_SIN_UP_VRAM, 1); nvc_note(&t, 325, 640, 353, 1, NVC_SIN_BYTES, 640u * 353 * 4);
    nvc_note(&t, 325, 640, 353, 1, NVC_SIN_SKIP, 4);
    nvc_note(&t, 7, 100, 50, 0, NVC_REPLACE, 1);
    nvc_note(&t, 0, 1, 1, 0, NVC_PASS, 1);                                      /* surface id 0 is no surface: ignored */
    nvc_note(&t, 9, 1, 1, 0, 99, 1);                                           /* an unknown kind: ignored */
    CHECK(t.n == 2 && t.tot[NVC_PASS] == 2 && t.tot[NVC_DRAW] == 5 && t.tot[NVC_SIN_SKIP] == 4);
    CHECK(nvc_format(&t, out, sizeof out, 5) > 0);
    CHECK(strstr(out, "surfaces=2 ") && strstr(out, " pass=2 pass_clear=1 draws=5 draw_blit=3 draw_fbf=2 "));
    CHECK(strstr(out, "sin_up_vram=1 sin_up_priv=0 sin_up_mb=0 sin_skip=4 "));   /* 902 KB: under one MB */
    CHECK(strstr(out, "top=325:640x353:V:u1,s4,P2,c1,D5,B3,F2;7:100x50:P:R1"));  /* the busiest first; a skip does not make a surface busy */
    CHECK(nvc_activity(&t.s[0]) == 1 + 2 + 5 && nvc_activity(&t.s[1]) == 1);

    /* a surface seen again keeps one slot and takes the newest size / placement */
    nvc_note(&t, 7, 200, 100, 1, NVC_REPLACE, 1);
    CHECK(t.n == 2 && t.s[1].w == 200 && t.s[1].vram == 1 && t.s[1].c[NVC_REPLACE] == 2);

    /* top=N cuts the list; the line never overruns a small buffer */
    char small[64];
    CHECK(nvc_format(&t, small, sizeof small, 8) < (int)sizeof small && strlen(small) < sizeof small);
    nvc_format(&t, out, sizeof out, 1); CHECK(!strchr(strstr(out, "top="), ';'));

    /* a full table counts the new surface in the totals and in `overflow` only */
    static nvc_table f;
    for (uint32_t id = 1; id <= NVC_MAX + 3; id++) nvc_note(&f, id, 8, 8, 0, NVC_PASS, 1);
    CHECK(f.n == NVC_MAX && f.overflow == 3 && f.tot[NVC_PASS] == NVC_MAX + 3);
    nvc_note(&f, 1, 8, 8, 0, NVC_PASS, 1); CHECK(f.overflow == 3 && f.s[0].c[NVC_PASS] == 2);   /* a known surface still counts */
    CHECK(nvc_format(&f, out, sizeof out, 5) > 0 && strstr(out, "overflow=3 "));

    /* the one-line summary of a busy process fits the plugin's log line (1024 bytes of body, the prefix is short) */
    static nvc_table b;
    for (uint32_t id = 1; id <= 12; id++) for (int k = 0; k < NVC_SIN_BYTES; k++) nvc_note(&b, 100000 + id, 3840, 2160, 1, k, 123456789ull);
    CHECK(nvc_format(&b, out, 760, 5) > 0 && strlen(out) < 760);

    /* a slot is found once and counted into lock-free (a render pass keeps it for its draws); it never moves, a full table gives NULL and the total still counts */
    static nvc_table sl;
    nvc_surf *a = nvc_slot(&sl, 42, 100, 50, 1);
    CHECK(a && a->id == 42 && a->w == 100 && a->vram == 1 && sl.n == 1);
    nvc_add(&sl, a, NVC_DRAW, 3); nvc_add(&sl, a, NVC_DRAW_BLIT, 2);
    for (uint32_t id = 100; id < 100 + 60; id++) nvc_note(&sl, id, 8, 8, 0, NVC_PASS, 1);   /* more slots: the first one must not move */
    CHECK(nvc_slot(&sl, 42, 200, 60, 0) == a && a->w == 200 && a->vram == 0 && a->c[NVC_DRAW] == 3 && a->c[NVC_DRAW_BLIT] == 2 && sl.tot[NVC_DRAW] == 3);
    CHECK(nvc_slot(&sl, 0, 1, 1, 0) == NULL);
    nvc_add(&sl, NULL, NVC_DRAW, 4); CHECK(sl.tot[NVC_DRAW] == 7);
    static nvc_table full;
    for (uint32_t id = 1; id <= NVC_MAX; id++) CHECK(nvc_slot(&full, id, 1, 1, 0) != NULL);
    CHECK(nvc_slot(&full, 9999, 1, 1, 0) == NULL && full.overflow == 1);

    CHECK(nvc_is_blit_pipeline("buffer_image_blit_vert_lpf", "x") && nvc_is_blit_pipeline("x", "buffer_image_blit_frag_lpf"));
    CHECK(!nvc_is_blit_pipeline("fixed_vert", "fixed_frag_lpf_cpf") && !nvc_is_blit_pipeline(NULL, NULL));

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("test_surf_census: ok\n");
    return 0;
}
