/* The decisions behind the plugin's A/B switches (metal/plugin/nvmtl_ab_switch.h, DRIVER-PLAN N49): which RADV flag lists reach the environment of
 * WindowServer, how they join, which layout slot a LOAD pass is counted under and when it needs a barrier, which managed ranges the snapshot covers.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include "nvmtl_ab_switch.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
    /* the flag lists the card run uses are taken */
    CHECK(nvmtl_radv_flag_ok("nodcc"));
    CHECK(nvmtl_radv_flag_ok("nodcc,nohiz,nofmask,nofastclears"));
    CHECK(nvmtl_radv_flag_ok("nodcc,nohiz,nofmask,nofastclears,zerovram,syncshaders"));
    CHECK(nvmtl_radv_flag_ok("a_b,c9"));
    /* nothing that could reach a shell, a path or a second variable */
    CHECK(!nvmtl_radv_flag_ok(NULL));
    CHECK(!nvmtl_radv_flag_ok(""));
    CHECK(!nvmtl_radv_flag_ok("NODCC"));          /* case: RADV reads lowercase names */
    CHECK(!nvmtl_radv_flag_ok("nodcc nohiz"));
    CHECK(!nvmtl_radv_flag_ok("nodcc;rm"));
    CHECK(!nvmtl_radv_flag_ok("nodcc=1"));
    CHECK(!nvmtl_radv_flag_ok("-nodcc"));
    CHECK(!nvmtl_radv_flag_ok("a\nb"));
    CHECK(!nvmtl_radv_flag_ok("../x"));
    /* the length cap: 200 characters pass, 201 do not */
    { char v[300]; memset(v, 'a', sizeof v); v[NVMTL_RADV_FLAGS_MAX] = 0; CHECK(nvmtl_radv_flag_ok(v)); v[NVMTL_RADV_FLAGS_MAX] = 'a'; v[NVMTL_RADV_FLAGS_MAX + 1] = 0; CHECK(!nvmtl_radv_flag_ok(v)); }

    /* joining: the value that was there stays first */
    { char o[64];
      CHECK(nvmtl_radv_flag_join(o, sizeof o, NULL, "nodcc") == 0 && !strcmp(o, "nodcc"));
      CHECK(nvmtl_radv_flag_join(o, sizeof o, "", "nodcc") == 0 && !strcmp(o, "nodcc"));
      CHECK(nvmtl_radv_flag_join(o, sizeof o, "syncshaders", "nodcc,nohiz") == 0 && !strcmp(o, "syncshaders,nodcc,nohiz"));
      CHECK(nvmtl_radv_flag_join(o, 8, "syncshaders", "nodcc") == -1 && !o[0]);   /* does not fit: empty, never a cut-off flag */
      CHECK(nvmtl_radv_flag_join(o, 6, NULL, "nodcc") == 0 && !strcmp(o, "nodcc"));   /* exactly fits */
      CHECK(nvmtl_radv_flag_join(o, 5, NULL, "nodcc") == -1); }

    /* layout slots: the VkImageLayout numbers the census line names, everything else is "other" */
    for (uint32_t l = 0; l < 9; l++) CHECK(nvmtl_load_layout_slot(l) == l);
    CHECK(nvmtl_load_layout_slot(9) == 9);
    CHECK(nvmtl_load_layout_slot(1000001002u) == 9);   /* VK_IMAGE_LAYOUT_PRESENT_SRC_KHR and friends */
    CHECK(nvmtl_load_layout_slot(0xffffffffu) == 9);

    /* the barrier: TRANSFER_SRC (6) is the colour declaration, DEPTH_STENCIL_ATTACHMENT (3) the depth one */
    CHECK(!nvmtl_load_needs_barrier(6, 6, 1));
    CHECK(nvmtl_load_needs_barrier(5, 6, 1));       /* sampled, then drawn into again: the Chess log's "layout 5" */
    CHECK(nvmtl_load_needs_barrier(0, 6, 1));       /* never written */
    CHECK(nvmtl_load_needs_barrier(1, 6, 1));       /* GENERAL after a compute write */
    CHECK(nvmtl_load_needs_barrier(5, 3, 1));       /* a depth texture sampled by the depth resolve pass, then loaded as depth */
    CHECK(!nvmtl_load_needs_barrier(3, 3, 1));
    CHECK(!nvmtl_load_needs_barrier(5, 6, 0));      /* the knob off: counted only */
    CHECK(!nvmtl_load_needs_barrier(0, 6, 0));

    /* the snapshot covers up to 1 MiB, never an empty range, never when the knob is off */
    CHECK(nvmtl_snap_wanted(1, 1));
    CHECK(nvmtl_snap_wanted(262144, 1));            /* Terminal's upload buffers */
    CHECK(nvmtl_snap_wanted(NVMTL_SNAP_MAX_BYTES, 1));
    CHECK(!nvmtl_snap_wanted(NVMTL_SNAP_MAX_BYTES + 1, 1));
    CHECK(!nvmtl_snap_wanted(0, 1));
    CHECK(!nvmtl_snap_wanted(4096, 0));

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("test_ab_switch: ok\n");
    return 0;
}
