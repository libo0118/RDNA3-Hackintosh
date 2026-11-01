/* tests/test_drawable_life.c - the drawable-life counters of metal/plugin/nvmtl_drawable_life.h (DRIVER-PLAN N73) */
#include <stdio.h>
#include <string.h>
#include "nvmtl_drawable_life.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static int has(const char *body, const char *kv) { return strstr(body, kv) != NULL; }

int main(void)
{
    char body[1024];
    nvmtl_dl_stats s; memset(&s, 0, sizeof s);
    CHECK(!nvmtl_dl_format(&s, 0, 0, body, sizeof body));

    /* the keep ring's size: WindowServer by default, others off, numbers clamped, junk off */
    CHECK(nvmtl_keep_size(NULL, 1) == NVMTL_KEEP_WS_DEFAULT && nvmtl_keep_size(NULL, 0) == 0);
    CHECK(nvmtl_keep_size("0", 1) == 0 && nvmtl_keep_size("4", 0) == 4 && nvmtl_keep_size("999", 1) == NVMTL_KEEP_MAX);
    CHECK(nvmtl_keep_size("off", 1) == 0 && nvmtl_keep_size("", 1) == 0 && nvmtl_keep_size("-1", 1) == 0);

    /* nextDrawable: 3 calls, one slow, one nil; the share of the wall from the first start to the last end */
    const uint64_t ms = 1000000ull, T = 50 * ms;
    nvmtl_dl_next(&s, T, T + 2 * ms, 1);             /* 2 ms: slow */
    nvmtl_dl_next(&s, T + 5 * ms, T + 5 * ms + 500000, 1);
    nvmtl_dl_next(&s, T + 9 * ms, T + 10 * ms, 0);   /* exactly 1 ms: not slow, nil */
    CHECK(s.next_n == 3 && s.next_ns == 3500000 && s.next_max_ns == 2 * ms && s.next_slow == 1 && s.next_nil == 1);
    CHECK(s.t_first == T && s.t_end == T + 10 * ms);
    nvmtl_dl_next(&s, T + 20 * ms, T + 19 * ms, 1);  /* a clock that went back: 0, never a wrap */
    CHECK(s.next_ns == 3500000 && s.next_n == 4);

    /* present latency: commit -> present; no commit time or a time before it is not counted */
    nvmtl_dl_pres(&s, T, T + 3 * ms); nvmtl_dl_pres(&s, 0, T); nvmtl_dl_pres(&s, T + 5, T);
    CHECK(s.pres_n == 1 && s.pres_ns == 3 * ms && s.pres_max_ns == 3 * ms);

    /* the per-surface table: present, back (away), back again without a present (unknown), id 0 ignored */
    nvmtl_dl_present(&s, 7, T); nvmtl_dl_present(&s, 8, T + ms); nvmtl_dl_present(&s, 7, T + 2 * ms);   /* 7 again: its time moves */
    nvmtl_dl_present(&s, 0, T);
    uint64_t away = 0;
    CHECK(nvmtl_dl_back(&s, 7, T + 6 * ms, &away) && away == 4 * ms);
    CHECK(!nvmtl_dl_back(&s, 7, T + 7 * ms, &away) && s.away_unknown == 1);
    CHECK(nvmtl_dl_back(&s, 8, T + 9 * ms, NULL) && s.away_n == 2 && s.away_ns == 12 * ms && s.away_max_ns == 8 * ms);
    CHECK(!nvmtl_dl_back(&s, 0, T, &away) && s.away_unknown == 2);
    /* a full table forgets the oldest */
    { nvmtl_dl_stats f; memset(&f, 0, sizeof f);
      for (unsigned i = 0; i < NVMTL_DL_SLOTS; i++) nvmtl_dl_present(&f, 100 + i, 1000 + i);
      nvmtl_dl_present(&f, 999, 5000);                       /* evicts id 100 (t 1000) */
      CHECK(!nvmtl_dl_back(&f, 100, 6000, NULL));
      CHECK(nvmtl_dl_back(&f, 101, 6000, &away) && away == 6000 - 1001);
      CHECK(nvmtl_dl_back(&f, 999, 6000, &away) && away == 1000);
      nvmtl_dl_present(&f, 555, 7000);                       /* goes into a freed slot, evicts nobody */
      CHECK(nvmtl_dl_back(&f, 102, 8000, &away) && away == 8000 - 1002); }

    /* surface textures */
    nvmtl_dl_st_new(&s, 400000); nvmtl_dl_st_new(&s, 200000); nvmtl_dl_st_alias(&s);
    nvmtl_dl_st_life(&s, T, T + 9 * ms); nvmtl_dl_st_life(&s, 0, T); nvmtl_dl_st_life(&s, T + 1, T);
    CHECK(s.st_new == 2 && s.st_new_ns == 600000 && s.st_alias == 1 && s.st_life_n == 1 && s.st_life_max_ns == 9 * ms);

    /* the keep ring's victim: an entry unused for over 2 s first, else the least recently used when full, else none */
    { const uint64_t t[4] = { 10 * ms, 5 * ms, 20 * ms, 15 * ms };
      CHECK(nvmtl_keep_victim(t, 4, 8, 30 * ms) == -1);          /* room, nothing old */
      CHECK(nvmtl_keep_victim(t, 4, 4, 30 * ms) == 1);           /* full: the LRU */
      CHECK(nvmtl_keep_victim(t, 4, 8, 5 * ms + NVMTL_KEEP_AGE_NS + 1) == 1);   /* the old one, though there is room */
      CHECK(nvmtl_keep_victim(t, 4, 0, 30 * ms) == 1);           /* off: every entry goes */
      CHECK(nvmtl_keep_victim(t, 0, 0, 30 * ms) == -1); }

    s.st_kept = 3; s.st_evict = 1;
    CHECK(nvmtl_dl_format(&s, 8, 1, body, sizeof body));
    CHECK(has(body, "next_n=4 ") && has(body, "next_us_avg=875 ") && has(body, "next_us_max=2000 ") && has(body, "next_slow=1 "));
    CHECK(has(body, "next_nil=1 ") && has(body, "next_pct=18.4 ") && has(body, "next_ms=3 ") && has(body, "wall_ms=19 "));   /* 3.5 ms of the 19 ms from the first start to the latest end */
    CHECK(has(body, "pres_n=1 ") && has(body, "pres_us_avg=3000 ") && has(body, "pres_us_max=3000 "));
    CHECK(has(body, "away_n=2 ") && has(body, "away_us_avg=6000 ") && has(body, "away_us_max=8000 ") && has(body, "away_unknown=2 "));
    CHECK(has(body, "st_new=2 ") && has(body, "st_new_us_avg=300 ") && has(body, "st_alias=1 ") && has(body, "st_kept=3 ") && has(body, "st_evict=1 "));
    CHECK(has(body, "st_life_n=1 ") && has(body, "st_life_us_avg=9000 ") && has(body, "st_life_us_max=9000 ") && has(body, "keep=8 ") && has(body, "trace=1"));
    { char small[16]; memset(small, 'x', sizeof small);
      CHECK(nvmtl_dl_format(&s, 0, 0, small, sizeof small) && small[15] == 0); }
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("test_drawable_life: ok");
    return 0;
}
