/* The decisions behind the plugin's A/B switches (metal/plugin/nvmtl_ab_switch.h, DRIVER-PLAN N50): which RADV flag lists reach the environment of
 * WindowServer, how they join, how a value is quoted in the log, which layout slot a LOAD pass is counted under and when it needs a barrier, whether a
 * depth attachment is LOADed, which managed ranges the snapshot covers, and the list of switches a process logs at its start.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include "nvmtl_ab_switch.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
static int ok(const char *v, int perftest) { char w[96]; return nvmtl_radv_flag_check(v, perftest, w, sizeof w) == 0; }
static const char *fake_env(const char *k)
{
    static const char *const t[][2] = { { "NVMTL_RADV_DEBUG", "nodcc,nohiz" }, { "NVMTL_ASYNC_COMMIT", "0" }, { "NVMTL_NO_POOL_REUSE", "1" },
                                        { "NVMTL_MANAGED_CHECK", " 0" }, { "NVMTL_G79_OFF", "a\"b\\c\x01" }, { 0, 0 } };
    for (int i = 0; t[i][0]; i++) if (!strcmp(t[i][0], k)) return t[i][1];
    return NULL;
}

int main(void)
{
    /* DRIVER-PLAN N57: NVMTL_ATTR_RING_BIG_PAGE -> RADV_ATTR_RING_BIG_PAGE: only "0" sets it, unset / "1" leave upstream, the rest is refused */
    { char w[96];
      CHECK(!strcmp(nvmtl_attr_ring_value("0", w, sizeof w) ? nvmtl_attr_ring_value("0", w, sizeof w) : "", "0") && !w[0]);
      CHECK(!nvmtl_attr_ring_value(NULL, w, sizeof w) && !w[0]);
      CHECK(!nvmtl_attr_ring_value("1", w, sizeof w) && !w[0]);
      CHECK(!nvmtl_attr_ring_value("00", w, sizeof w) && w[0]);
      CHECK(!nvmtl_attr_ring_value("", w, sizeof w) && w[0]);
      CHECK(!nvmtl_attr_ring_value("off", w, sizeof w) && strstr(w, "only 0"));
      int listed = 0;
      for (int i = 0; nvmtl_ab_knobs[i]; i++) listed |= !strcmp(nvmtl_ab_knobs[i], "NVMTL_ATTR_RING_BIG_PAGE");
      CHECK(listed); }   /* `vk: ab switches` shows it: the triage's `plugin switch` row and the ladder's rung check */
    /* the flag lists the card run uses are taken */
    CHECK(ok("nodcc", 0));
    CHECK(ok("nodcc,nohiz,nofmask,nofastclears", 0));
    CHECK(ok("nodcc,nohiz,nofmask,nofastclears,zerovram,syncshaders", 0));
    CHECK(ok("dccmsaa,nosam", 1));
    /* a typo or a flag of the other variable is refused with the name in the reason */
    { char w[96];
      CHECK(nvmtl_radv_flag_check("nofastclear", 0, w, sizeof w) == -1 && strstr(w, "'nofastclear'"));
      CHECK(nvmtl_radv_flag_check("nodcc,nohizz", 0, w, sizeof w) == -1 && strstr(w, "'nohizz'"));
      CHECK(nvmtl_radv_flag_check("dccmsaa", 0, w, sizeof w) == -1);       /* a RADV_PERFTEST name in RADV_DEBUG */
      CHECK(nvmtl_radv_flag_check("nodcc", 1, w, sizeof w) == -1);
      CHECK(nvmtl_radv_flag_check("nodcc,", 0, w, sizeof w) == -1 && strstr(w, "trailing"));
      CHECK(nvmtl_radv_flag_check("nodcc,,nohiz", 0, w, sizeof w) == -1);
      CHECK(nvmtl_radv_flag_check(",nodcc", 0, w, sizeof w) == -1); }
    /* nothing that could reach a shell, a path or a second variable; the reason names the character */
    { char w[96];
      CHECK(nvmtl_radv_flag_check(NULL, 0, w, sizeof w) == -1);
      CHECK(nvmtl_radv_flag_check("", 0, w, sizeof w) == -1 && strstr(w, "empty"));
      CHECK(nvmtl_radv_flag_check("nodcc nohiz", 0, w, sizeof w) == -1 && strstr(w, "0x20") && strstr(w, "position 5"));
      CHECK(nvmtl_radv_flag_check("NODCC", 0, w, sizeof w) == -1 && strstr(w, "0x4e"));
      CHECK(!ok("nodcc;rm", 0)); CHECK(!ok("nodcc=1", 0)); CHECK(!ok("-nodcc", 0)); CHECK(!ok("a\nb", 0)); CHECK(!ok("../x", 0)); }
    /* the length cap: a list of 200 characters passes the character test, 201 do not (known names, joined, to make it a legal list) */
    { char v[400]; v[0] = 0; while (strlen(v) < 210) strcat(v, v[0] ? ",nodcc" : "nodcc"); char w[96];
      CHECK(strlen(v) > NVMTL_RADV_FLAGS_MAX && nvmtl_radv_flag_check(v, 0, w, sizeof w) == -1 && strstr(w, "longer")); }

    /* contains: every flag of add is a flag of old (whole names) */
    CHECK(nvmtl_radv_flag_contains("nodcc,nohiz,nofmask", "nohiz,nodcc"));
    CHECK(!nvmtl_radv_flag_contains("nodcc,nohiz", "nofmask"));
    CHECK(!nvmtl_radv_flag_contains("nodcc", "nodcc,nohiz"));
    CHECK(!nvmtl_radv_flag_contains("nodisplaydcc", "nodcc"));       /* a prefix is not the flag */
    CHECK(!nvmtl_radv_flag_contains(NULL, "nodcc")); CHECK(!nvmtl_radv_flag_contains("", "nodcc"));
    /* joining: the value that was there stays first, and is not repeated */
    { char o[64];
      CHECK(nvmtl_radv_flag_join(o, sizeof o, NULL, "nodcc") == 0 && !strcmp(o, "nodcc"));
      CHECK(nvmtl_radv_flag_join(o, sizeof o, "", "nodcc") == 0 && !strcmp(o, "nodcc"));
      CHECK(nvmtl_radv_flag_join(o, sizeof o, "syncshaders", "nodcc,nohiz") == 0 && !strcmp(o, "syncshaders,nodcc,nohiz"));
      CHECK(nvmtl_radv_flag_join(o, sizeof o, "nodcc,nohiz", "nodcc") == 0 && !strcmp(o, "nodcc,nohiz"));   /* twice: unchanged, never a,b,a,b */
      CHECK(nvmtl_radv_flag_join(o, 8, "syncshaders", "nodcc") == -1 && !o[0]);   /* does not fit: empty, never a cut-off flag */
      CHECK(nvmtl_radv_flag_join(o, 6, NULL, "nodcc") == 0 && !strcmp(o, "nodcc"));   /* exactly fits */
      CHECK(nvmtl_radv_flag_join(o, 5, NULL, "nodcc") == -1); }

    /* quoting for the log */
    { char q[64];
      nvmtl_ab_quote(q, sizeof q, NULL); CHECK(!strcmp(q, "unset"));
      nvmtl_ab_quote(q, sizeof q, ""); CHECK(!strcmp(q, "\"\""));
      nvmtl_ab_quote(q, sizeof q, "nodcc nohiz"); CHECK(!strcmp(q, "\"nodcc nohiz\""));
      nvmtl_ab_quote(q, sizeof q, "a\"b\\c\x01"); CHECK(!strcmp(q, "\"a\\\"b\\\\c\\x01\""));
      nvmtl_ab_quote(q, 8, "abcdefghijkl"); CHECK(strlen(q) < 8); }

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

    /* a depth attachment is LOADed when the pass declares DEPTH_STENCIL_ATTACHMENT_OPTIMAL: depth LOAD, or stencil LOAD when the format has stencil */
    CHECK(nvmtl_load_depth_declared(2u, 0));        /* depth load, no stencil */
    CHECK(nvmtl_load_depth_declared(2u, 1));
    CHECK(nvmtl_load_depth_declared(4u, 1));        /* stencil load alone, a D32S8 format */
    CHECK(!nvmtl_load_depth_declared(4u, 0));       /* the stencil bit means nothing without stencil: begin_render sets it along with depth, and a clear-only pass has it alone */
    CHECK(!nvmtl_load_depth_declared(0u, 1));
    CHECK(!nvmtl_load_depth_declared(1u, 1));       /* colour load only */
    CHECK(nvmtl_load_depth_declared(6u, 0));

    /* the snapshot covers up to 1 MiB, never an empty range, never when the knob is off */
    CHECK(nvmtl_snap_wanted(1, 1));
    CHECK(nvmtl_snap_wanted(262144, 1));            /* Terminal's upload buffers */
    CHECK(nvmtl_snap_wanted(NVMTL_SNAP_MAX_BYTES, 1));
    CHECK(!nvmtl_snap_wanted(NVMTL_SNAP_MAX_BYTES + 1, 1));
    CHECK(!nvmtl_snap_wanted(0, 1));
    CHECK(!nvmtl_snap_wanted(4096, 0));

    /* the switch list: only knobs that are set, values quoted ("0" and " 0" are different and both visible), chunks that fit */
    { char c[400]; size_t next = 0; unsigned n = nvmtl_ab_switch_chunk(c, sizeof c, &next, fake_env);
      CHECK(n == 5);
      CHECK(strstr(c, "NVMTL_RADV_DEBUG=\"nodcc,nohiz\"") && strstr(c, "NVMTL_ASYNC_COMMIT=\"0\"") && strstr(c, "NVMTL_NO_POOL_REUSE=\"1\""));
      CHECK(strstr(c, "NVMTL_MANAGED_CHECK=\" 0\""));
      CHECK(strstr(c, "NVMTL_G79_OFF=\"a\\\"b\\\\c\\x01\""));
      CHECK(!strstr(c, "NVMTL_PARANOID"));
      CHECK(nvmtl_ab_switch_chunk(c, sizeof c, &next, fake_env) == 0);   /* nothing more */ }
    { char c[70]; size_t next = 0; unsigned total = 0, chunks = 0, n;      /* a small buffer: several chunks, none cut, all five tokens seen */
      while ((n = nvmtl_ab_switch_chunk(c, sizeof c, &next, fake_env)) != 0) { total += n; chunks++; CHECK(strlen(c) < sizeof c); CHECK(chunks < 10); }
      CHECK(total == 5 && chunks >= 3); }

    /* NVMTL_MANAGED_WHOLE / PAGES: the range a flush uploads */
    { size_t a, b;
      nvmtl_managed_widen(262144, 100, 164, 0, 0, &a, &b); CHECK(a == 100 && b == 164);                 /* byte exact, as before */
      nvmtl_managed_widen(262144, 100, 164, 0, 1, &a, &b); CHECK(a == 0 && b == 4096);                  /* the page */
      nvmtl_managed_widen(262144, 4096, 8192, 0, 1, &a, &b); CHECK(a == 4096 && b == 8192);             /* already whole pages */
      nvmtl_managed_widen(262144, 4095, 4097, 0, 1, &a, &b); CHECK(a == 0 && b == 8192);                /* a range that straddles two */
      nvmtl_managed_widen(10000, 9000, 9900, 0, 1, &a, &b); CHECK(a == 8192 && b == 10000);             /* clipped to the buffer */
      nvmtl_managed_widen(262144, 100, 164, 1, 1, &a, &b); CHECK(a == 0 && b == 262144);               /* WHOLE wins */
      nvmtl_managed_widen(262144, 100, 999999, 0, 0, &a, &b); CHECK(a == 100 && b == 262144);          /* a range past the end is cut */ }
    /* the undeclared-write detector */
    { static uint8_t prev[16384], cur[16384]; nvmtl_undecl u;
      memset(prev, 0, sizeof prev); memcpy(cur, prev, sizeof cur);
      cur[5000] = 7; cur[5001] = 7;                                         /* declared: [4096, 6000) */
      CHECK(!nvmtl_undeclared_scan(cur, prev, sizeof cur, 4096, 6000, &u) && u.bytes == 0);
      cur[6100] = 1;                                                         /* 101 bytes after the range, in the next page */
      CHECK(nvmtl_undeclared_scan(cur, prev, sizeof cur, 4096, 6000, &u) && u.bytes == 1 && u.first == 6100 && u.last == 6100 && u.nearest == 101 && u.samepage);
      cur[100] = 1; cur[16000] = 1;                                          /* far away, other pages */
      CHECK(nvmtl_undeclared_scan(cur, prev, sizeof cur, 4096, 6000, &u) && u.bytes == 3 && u.first == 100 && u.last == 16000 && u.nearest == 101);
      memcpy(prev, cur, sizeof cur); cur[6000] = 9;                          /* the byte right after the range: distance 1, same page */
      CHECK(nvmtl_undeclared_scan(cur, prev, sizeof cur, 4096, 6000, &u) && u.bytes == 1 && u.nearest == 1 && u.samepage);
      memcpy(prev, cur, sizeof cur); cur[12000] = 3;
      CHECK(nvmtl_undeclared_scan(cur, prev, sizeof cur, 4096, 6000, &u) && u.bytes == 1 && !u.samepage && u.nearest == 6001);
      CHECK(!nvmtl_undeclared_scan(cur, cur, sizeof cur, 0, sizeof cur, &u));      /* the whole buffer declared: nothing is outside */ }

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("test_ab_switch: ok\n");
    return 0;
}
