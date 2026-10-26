/* Host test of metal/plugin/nvmtl_capture.h (DRIVER-PLAN N55, NVMTL_CAPTURE_DRAW): the switch's grammar, which matching draws are taken,
 * the byte ranges kept, and the container format. With a path argument it also writes a sample container that
 * tools/capture_decode.py --check must read back (the Makefile runs both), so the C writer and the Python reader cannot drift.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#define _POSIX_C_SOURCE 200809L   /* openat, fstatat, mkdtemp, symlink under -std=c99 */
#define _DARWIN_C_SOURCE          /* macOS: O_NOFOLLOW and mkdtemp are hidden by a strict _POSIX_C_SOURCE */
#include "nvmtl_capture.h"
#include <assert.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
int main(int argc, char **argv)
{
    nvmtl_cap_spec s; char why[96];
    CHECK(nvmtl_cap_parse("buffer_image_blit:200", &s, why, sizeof why) == 0 && !strcmp(s.shader, "buffer_image_blit") && s.first == 200 && s.count == 1 && s.every == 1 && !s.proc[0]);
    CHECK(nvmtl_cap_parse("fixed_frag_lpf_cpf:5:8:23@Terminal", &s, why, sizeof why) == 0 && s.first == 5 && s.count == 8 && s.every == 23 && !strcmp(s.proc, "Terminal"));
    CHECK(nvmtl_cap_parse("blit:1:32", &s, why, sizeof why) == 0 && s.count == 32);
    const char *bad[] = { "", "blit", ":3", "blit:", "blit:0", "blit:1:0", "blit:1:33", "blit:1:2:0", "blit:1:2:3:4", "blit:x", "blit:1:", "blit:1@",
                          "blit:1@a@b", "blit::2", "blit:99999999999", "blit:-1" };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) { int r = nvmtl_cap_parse(bad[i], &s, why, sizeof why); if (r == 0) printf("FAIL accepted \"%s\"\n", bad[i]); fails += r == 0; CHECK(r != 0 || why[0]); }
    nvmtl_cap_parse("blit:10:3:5", &s, why, sizeof why);
    CHECK(nvmtl_cap_hit(&s, 9) == -1 && nvmtl_cap_hit(&s, 10) == 0 && nvmtl_cap_hit(&s, 11) == -1 && nvmtl_cap_hit(&s, 15) == 1 && nvmtl_cap_hit(&s, 20) == 2 && nvmtl_cap_hit(&s, 25) == -1);
    CHECK(nvmtl_cap_name_match(&s, "buffer_image_blit_vert_lpf", "x") && nvmtl_cap_name_match(&s, NULL, "buffer_image_blit_frag_lpf") && !nvmtl_cap_name_match(&s, "fixed_vert", "fixed_frag"));
    CHECK(nvmtl_cap_proc_match(&s, "WindowServer"));
    nvmtl_cap_parse("blit:1@Terminal", &s, why, sizeof why);
    CHECK(nvmtl_cap_proc_match(&s, "Terminal") && !nvmtl_cap_proc_match(&s, "WindowServer") && !nvmtl_cap_proc_match(&s, NULL));
    size_t lo, hi;
    CHECK(nvmtl_cap_range(262144, 5000, 4u << 20, &lo, &hi) && lo == 0 && hi == 262144);
    CHECK(nvmtl_cap_range(10u << 20, 1000, 4u << 20, &lo, &hi) && lo == 1000 && hi == 1000 + (4u << 20));
    CHECK(nvmtl_cap_range(5u << 20, (5u << 20) - 10, 4u << 20, &lo, &hi) && lo == (5u << 20) - 10 && hi == 5u << 20);
    CHECK(!nvmtl_cap_range(10u << 20, 10u << 20, 4u << 20, &lo, &hi) && !nvmtl_cap_range(0, 0, 16, &lo, &hi));
    nvmtl_capbuf b; CHECK(nvmtl_cap_init(&b, 4096) == 0 && b.n == 16 && !memcmp(b.p, "NVMTLCP1", 8));
    uint8_t data[3000]; for (unsigned i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 7);
    CHECK(nvmtl_cap_text(&b, "META", "meta.txt", "process=test\n") == 0);
    CHECK(nvmtl_cap_bytes(&b, "v1.cpu_draw", 1234, data, 1000) == 0);
    CHECK(nvmtl_cap_bytes(&b, "f9.cpu_draw", 0, data, 3000) == 1);     /* past the 4 KiB limit: a DROP record */
    CHECK(b.dropped == 1 && b.recs == 3 && b.n <= 4096);
    char nm[64]; nvmtl_cap_safe_name("Terminal", nm, sizeof nm); CHECK(!strcmp(nm, "Terminal"));
    nvmtl_cap_safe_name("a/b c\n../x", nm, sizeof nm); CHECK(!strcmp(nm, "a_b_c_.._x"));
    nvmtl_cap_safe_name("", nm, sizeof nm); CHECK(!strcmp(nm, "unknown"));
    { char longn[300]; memset(longn, 'x', sizeof longn - 1); longn[sizeof longn - 1] = 0; nvmtl_cap_safe_name(longn, nm, sizeof nm); CHECK(strlen(nm) == 48); }
    {   /* the directory and the file: a stale file of ours is replaced, a symlinked directory or file is refused */
        char tmpl[] = "/tmp/test_capture.XXXXXX"; char *d = mkdtemp(tmpl); CHECK(d != NULL);
        char dir[128], lnk[160], other[160]; snprintf(dir, sizeof dir, "%s/cap", d); snprintf(lnk, sizeof lnk, "%s/link", d); snprintf(other, sizeof other, "%s/other", d);
        int dfd = nvmtl_cap_open_dir(dir); CHECK(dfd >= 0);
        struct stat st; CHECK(stat(dir, &st) == 0 && (st.st_mode & 07777) == 01777);
        int fd = nvmtl_cap_open_file(dfd, "capture-x-1-0.bin"); CHECK(fd >= 0);
        size_t done = 0; CHECK(nvmtl_cap_write_all(fd, (const uint8_t *)"old!", 4, &done) == 0 && done == 4); close(fd);
        fd = nvmtl_cap_open_file(dfd, "capture-x-1-0.bin"); CHECK(fd >= 0);            /* the stale file of an earlier boot: replaced */
        if (fd >= 0) { CHECK(fstat(fd, &st) == 0 && st.st_size == 0); close(fd); }
        fd = open(other, O_CREAT | O_WRONLY, 0600); if (fd >= 0) close(fd);
        CHECK(symlinkat(other, dfd, "capture-x-1-1.bin") == 0);
        CHECK(nvmtl_cap_open_file(dfd, "capture-x-1-1.bin") < 0 && errno == EEXIST);   /* a planted symlink: refused, not followed */
        char hard[160]; snprintf(hard, sizeof hard, "%s/capture-x-1-2.bin", dir); CHECK(link(other, hard) == 0);
        CHECK(nvmtl_cap_open_file(dfd, "capture-x-1-2.bin") < 0);                     /* two links: not ours to replace */
        close(dfd);
        CHECK(symlink(dir, lnk) == 0);
        CHECK(nvmtl_cap_open_dir(lnk) < 0);                                            /* a symlinked directory: refused */
        unlinkat(AT_FDCWD, hard, 0); snprintf(hard, sizeof hard, "%s/capture-x-1-1.bin", dir); unlink(hard);
        snprintf(hard, sizeof hard, "%s/capture-x-1-0.bin", dir); unlink(hard); unlink(lnk); unlink(other); rmdir(dir); rmdir(d);
    }
    if (argc > 1) { FILE *f = fopen(argv[1], "wb"); CHECK(f && fwrite(b.p, 1, b.n, f) == b.n); if (f) fclose(f); }
    nvmtl_cap_free(&b);
    printf(fails ? "test_capture: %d FAILED\n" : "test_capture: ok\n", fails);
    return fails != 0;
}
