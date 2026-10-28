/*
 * test_present_cost.c - metal/plugin/nvmtl_present_cost.h on the host (DRIVER-PLAN N61): the copy-out / surface-in
 * counters, the `vk: present cost: SUMMARY` body, the A/B switch and the geometry of the GPU copy-out.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "nvmtl_present_cost.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static nvmtl_so_geom ws_frame(void)
{
    /* WindowServer's display buffer of card run 2026-10-28: 1920x1080 BGRA, 7680 bytes per row, page-aligned */
    nvmtl_so_geom g = { 0x128d6c000u, 0x128d6c000u, 8294400u, 7680u, 1920u, 1080u, 4u, 1u, 1u, 1u, 1u, 4096u };
    return g;
}

int main(void)
{
    nvmtl_pc_stats s;
    char line[1024];
    memset(&s, 0, sizeof s);
    CHECK(nvmtl_pc_format(&s, 1, line, sizeof line) == 0);
    CHECK(nvmtl_pc_events(&s) == 0);

    /* two CPU copy-outs (1.5 ms and 24 ms: one slow), one GPU publish, two surface-in uploads */
    nvmtl_pc_out(&s, NVMTL_SO_CPU, 8294400u, 1500000u, 100000u, 900000u, 500000u);
    nvmtl_pc_out(&s, NVMTL_SO_CPU, 8294400u, 24000000u, 22000000u, 1100000u, 900000u);
    nvmtl_pc_out(&s, NVMTL_SO_DMA, 8294400u, 60000u, 0, 0, 0);
    nvmtl_pc_why(&s, NVMTL_SO_WHY_FIRST); nvmtl_pc_why(&s, NVMTL_SO_WHY_SHAPE);
    nvmtl_pc_why(&s, 0); nvmtl_pc_why(&s, NVMTL_SO_NWHY);   /* ignored */
    nvmtl_pc_in(&s, 924180u, 5000000u); nvmtl_pc_in(&s, 924180u, 1000000u);
    s.imports = 1;
    CHECK(nvmtl_pc_events(&s) == 5);
    CHECK(nvmtl_pc_format(&s, 1, line, sizeof line) == 1);
    CHECK(!strcmp(line, "outs=3 cpu=2 dma=1 out_mb=23 out_us_avg=8520 out_us_max=24000 cpu_us_avg=12750 cpu_us_max=24000 "
                        "dma_us_avg=60 dma_us_max=60 lock_us_avg=11050 gpu_us_avg=1000 memcpy_us_avg=700 memcpy_mbps=11849 slow=1 "
                        "why_off=0 why_first=1 why_shape=1 why_ro=0 why_import=0 why_moved=0 why_record=0 "
                        "imports=1 import_fail=0 ins=2 in_mb=1 in_us_avg=3000 in_us_max=5000 dma_mode=1"));
    if (strstr(line, "outs=3") == NULL) printf("got: %s\n", line);
    CHECK(nvmtl_pc_mbps(8294400u, 0) == 0 && nvmtl_pc_mbps(1000000u, 1000000u) == 1000);
    CHECK(!strcmp(nvmtl_so_why_name(NVMTL_SO_WHY_RO), "ro") && !strcmp(nvmtl_so_why_name(99), "?"));

    /* the A/B switch: on by default, '0...' off */
    CHECK(nvmtl_so_dma_mode(NULL) && nvmtl_so_dma_mode("") && nvmtl_so_dma_mode("1") && !nvmtl_so_dma_mode("0"));
    /* the log cadence: first 8, then every 1024th */
    CHECK(nvmtl_pc_log_due(0) && nvmtl_pc_log_due(7) && !nvmtl_pc_log_due(8) && nvmtl_pc_log_due(1024) && !nvmtl_pc_log_due(1025));

    /* geometry */
    nvmtl_so_plan_t p;
    nvmtl_so_geom g = ws_frame();
    CHECK(nvmtl_so_plan(&g, &p) == NVMTL_SO_OK && p.import_base == g.base && p.import_len == 8294400u && p.offset == 0 && p.row_bytes == 7680u && !p.bad);
    g.alloc = 8294400u + 100u;           /* rounded up to the import alignment */
    CHECK(nvmtl_so_plan(&g, &p) == NVMTL_SO_OK && p.import_len == 8298496u);
    g = ws_frame(); g.row_bytes = 7744u; g.alloc = 7744u * 1080u;   /* a padded pitch: rows land at the surface's pitch */
    CHECK(nvmtl_so_plan(&g, &p) == NVMTL_SO_OK && p.row_bytes == 7744u);
    g = ws_frame(); g.plane_base += 64u; g.alloc += 4096u;         /* a plane offset inside the allocation */
    CHECK(nvmtl_so_plan(&g, &p) == NVMTL_SO_OK && p.offset == 64u);
    g = ws_frame(); g.plane_base += 2u;
    CHECK(nvmtl_so_plan(&g, &p) == NVMTL_SO_WHY_SHAPE && p.bad && strstr(p.bad, "plane offset"));
    g = ws_frame(); g.base += 16u; g.plane_base += 16u;
    CHECK(nvmtl_so_plan(&g, &p) == NVMTL_SO_WHY_SHAPE && strstr(p.bad, "not aligned"));
    g = ws_frame(); g.row_bytes = 7676u;                                    /* narrower than the image's row */
    CHECK(nvmtl_so_plan(&g, &p) == NVMTL_SO_WHY_SHAPE && strstr(p.bad, "wider"));
    g = ws_frame(); g.row_bytes = 7682u;
    CHECK(nvmtl_so_plan(&g, &p) == NVMTL_SO_WHY_SHAPE && strstr(p.bad, "whole number"));
    g = ws_frame(); g.alloc = 8294400u - 4u;                                 /* last row does not fit */
    CHECK(nvmtl_so_plan(&g, &p) == NVMTL_SO_WHY_SHAPE && strstr(p.bad, "do not fit"));
    g = ws_frame(); g.samples = 4;
    CHECK(nvmtl_so_plan(&g, &p) == NVMTL_SO_WHY_SHAPE && strstr(p.bad, "multisampled"));
    g = ws_frame(); g.bw = 4; g.bh = 4;
    CHECK(nvmtl_so_plan(&g, &p) == NVMTL_SO_WHY_SHAPE && strstr(p.bad, "block"));
    g = ws_frame(); g.bpp = 3;
    CHECK(nvmtl_so_plan(&g, &p) == NVMTL_SO_WHY_SHAPE);
    g = ws_frame(); g.layers = 2;
    CHECK(nvmtl_so_plan(&g, &p) == NVMTL_SO_WHY_SHAPE);
    g = ws_frame(); g.align = 0;
    CHECK(nvmtl_so_plan(&g, &p) == NVMTL_SO_WHY_SHAPE);
    g = ws_frame(); g.base = 0;
    CHECK(nvmtl_so_plan(&g, &p) == NVMTL_SO_WHY_SHAPE && p.import_len == 0);
    g = ws_frame(); g.plane_base = g.base - 4096u;
    CHECK(nvmtl_so_plan(&g, &p) == NVMTL_SO_WHY_SHAPE);
    g = ws_frame(); g.w = 640; g.h = 480; g.row_bytes = 2560u; g.alloc = 2560u * 480u; g.bpp = 4;   /* a small window surface */
    CHECK(nvmtl_so_plan(&g, &p) == NVMTL_SO_OK && p.import_len == 1228800u);

    printf("test_present_cost: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
