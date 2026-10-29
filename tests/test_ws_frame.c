/* tests/test_ws_frame.c - the per-frame cost counters of metal/plugin/nvmtl_ws_frame.h (DRIVER-PLAN N64) */
#include <stdio.h>
#include <string.h>
#include "nvmtl_ws_frame.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static int has(const char *body, const char *kv) { return strstr(body, kv) != NULL; }

int main(void)
{
    char body[1024];
    nvmtl_wf_stats s; memset(&s, 0, sizeof s);
    /* nothing counted: no line */
    CHECK(!nvmtl_wf_format(&s, 0, body, sizeof body));
    CHECK(nvmtl_wf_events(&s) == 0);
    /* a command buffer without a render pass is not a frame */
    { nvmtl_wf_cb cb = { 0, 0, 0, 0, 0 };
      CHECK(!nvmtl_wf_is_frame(&cb) && !nvmtl_wf_is_frame(NULL));
      CHECK(!nvmtl_wf_frame(&s, &cb, 5000, 1, 1000, 0, 1000000000ull)); CHECK(s.frames == 0); }
    /* two frames in one second, one in the next; the second has no GPU time */
    const uint64_t t = 7ull * 1000000000ull;
    { nvmtl_wf_cb cb = { 3, 20, 18, 18, 0 };
      CHECK(nvmtl_wf_frame(&s, &cb, 4000000, 1, 2000000, 100000, t + 1)); }
    { nvmtl_wf_cb cb = { 1, 4, 2, 0, 2 };
      CHECK(nvmtl_wf_frame(&s, &cb, 2000000, 0, 999999999, 300000, t + 500000000ull)); }
    { nvmtl_wf_cb cb = { 2, 6, 0, 0, 0 };
      CHECK(nvmtl_wf_frame(&s, &cb, 6000000, 1, 4000000, 200000, t + 1000000000ull)); }
    CHECK(s.frames == 3 && s.passes == 6 && s.draws == 30 && s.fbf_draws == 20 && s.bar_img == 18 && s.bar_mem == 2);
    CHECK(s.gpu_n == 2 && s.gpu_ns == 6000000 && s.gpu_max_ns == 4000000);
    CHECK(s.rec_ns == 12000000 && s.rec_max_ns == 6000000);
    CHECK(s.active_s == 2 && s.fps_peak == 2);
    nvmtl_wf_upload(&s, 900 * 1024, 2300000);
    nvmtl_wf_upload(&s, 100 * 1024, 700000);
    CHECK(nvmtl_wf_events(&s) == 5);
    CHECK(nvmtl_wf_format(&s, 0, body, sizeof body));
    CHECK(has(body, "frames=3 ") && has(body, "active_s=2 ") && has(body, "fps_peak=2 ") && has(body, "passes=6 ") && has(body, "draws=30 "));
    CHECK(has(body, "fbf_draws=20 ") && has(body, "bar_img=18 ") && has(body, "bar_mem=2 "));
    CHECK(has(body, "rec_us_avg=4000 ") && has(body, "rec_us_max=6000 "));
    CHECK(has(body, "gpu_n=2 ") && has(body, "gpu_us_avg=3000 ") && has(body, "gpu_us_max=4000 "));
    CHECK(has(body, "wait_us_avg=200 "));
    CHECK(has(body, "ups=2 ") && has(body, "up_kb=1000 ") && has(body, "up_us_avg=1500 ") && has(body, "membar=0"));
    CHECK(nvmtl_wf_format(&s, 1, body, sizeof body) && has(body, "membar=1"));
    /* uploads alone still print (a process that uploads but never draws) */
    { nvmtl_wf_stats u; memset(&u, 0, sizeof u); nvmtl_wf_upload(&u, 4096, 1000);
      CHECK(nvmtl_wf_format(&u, 0, body, sizeof body) && has(body, "frames=0 ") && has(body, "gpu_us_avg=0 ") && has(body, "ups=1 ")); }
    /* a tiny buffer truncates, never overflows */
    { char small[16]; memset(small, 'x', sizeof small);
      CHECK(nvmtl_wf_format(&s, 0, small, sizeof small) && small[15] == 0); }
    /* GPU ticks: wrap at the valid bits, the period scales, a bad period is 0 */
    CHECK(nvmtl_wf_ticks_ns(100, 200, 64, 10.0) == 1000);
    CHECK(nvmtl_wf_ticks_ns((1ull << 48) - 10, 10, 48, 1.0) == 20);
    CHECK(nvmtl_wf_ticks_ns(0, 100, 0, 1.0) == 100);
    CHECK(nvmtl_wf_ticks_ns(0, 100, 64, 0.0) == 0 && nvmtl_wf_ticks_ns(0, 100, 64, -1.0) == 0 && nvmtl_wf_ticks_ns(0, 100, 64, 2e6) == 0);
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("test_ws_frame: ok");
    return 0;
}
