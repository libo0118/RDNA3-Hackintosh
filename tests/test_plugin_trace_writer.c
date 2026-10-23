/*
 * Host half of tests/test_plugin_trace.sh: drives metal/plugin/nvmtl_trace.h (the very code the Metal plugin
 * writes its frame trace with) and leaves trace files for tools/plugin_trace.py to decode.
 *
 *   test_plugin_trace_writer scenario   a WindowServer-like run: two back buffers per head, one always cleared
 *                                       to transparent black and copied out empty, one rendered and marked dirty
 *   test_plugin_trace_writer wrap       more events than the ring holds (NVMTL_TRACE_RING=256)
 *   test_plugin_trace_writer parity     the band hash against the kernel's (common/rdna3_a3_display.h) formula
 *   test_plugin_trace_writer off        NVMTL_TRACE=0: nothing may be created
 *   test_plugin_trace_writer dir PATH   nvt_open_dir: a new shared dir is 1777, a symlink is refused, a foreign owner too
 *   test_plugin_trace_writer append DIR nvt_open_append: refuses a symlink, a FIFO, a hard link, (as root) a foreign file
 *   test_plugin_trace_writer squat      a symlink and a foreign file sit on the trace's name: another name is taken
 *   test_plugin_trace_writer prune DIR  nvt_prune_fd: dead-pid traces over the limit go, WindowServer's and live ones stay
 * The trace goes to $NVMTL_TRACE_DIR as $NVMTL_TRACE_NAME-<pid>.trace.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdarg.h>
#include <assert.h>
#include <stddef.h>
#include "nvmtl_trace.h"
#include "rdna3_a3_display.h"

void nvlog(const char *fmt, ...)
{
    va_list a; va_start(a, fmt);
    fputs("nvlog: ", stderr); vfprintf(stderr, fmt, a); fputc('\n', stderr);
    va_end(a);
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "test_plugin_trace_writer: FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* the layout tools/plugin_trace.py reads */
typedef char off_abs[offsetof(nvt_rec, abs) == 0 ? 1 : -1];
typedef char off_seq[offsetof(nvt_rec, seq) == 8 ? 1 : -1];
typedef char off_kind[offsetof(nvt_rec, kind) == 16 ? 1 : -1];
typedef char off_surf[offsetof(nvt_rec, surf) == 20 ? 1 : -1];
typedef char off_rc[offsetof(nvt_rec, rc) == 24 ? 1 : -1];
typedef char off_bytes[offsetof(nvt_rec, bytes) == 56 ? 1 : -1];
typedef char off_id[offsetof(nvt_rec, id) == 64 ? 1 : -1];
typedef char off_u[offsetof(nvt_rec, u) == 80 ? 1 : -1];
typedef char off_hdr[sizeof(nvt_hdr) == 176 ? 1 : -1];

enum { W = 2560, H = 1440 };
static const float TRANSPARENT[4] = { 0.f, 0.f, 0.f, 0.f };
static const float WHITE[4] = { 1.f, 1.f, 1.f, 1.f };

static uint32_t g_pix[(size_t)W * H];
static void fill(uint32_t argb) { for (size_t i = 0; i < (size_t)W * H; i++) g_pix[i] = argb; }

static void frame(uint32_t cb, uint32_t surf, int empty, int refused, int publish, uint32_t seed)
{
    const uint32_t pass = nvt_next_id();
    const uint64_t t0 = nvt_now();
    nvt_ev_pass_begin(cb, pass, 0, 1, surf, 0, W, H, 80, 0, empty ? 2u : 1u, 1u, 0u, empty ? TRANSPARENT : WHITE);
    nvt_ev_pass_end(cb, pass, empty ? NVT_PASS_CLEAR_ONLY : NVT_PASS_DRAWN, 0, surf, W, H, empty ? 0u : 6u, empty ? TRANSPARENT : WHITE, t0);
    nvt_ev_cb_commit(cb, 1, 1, 1);
    nvt_ev_cb_dirty(cb, surf, 0, W, H, 80, NVT_F_VRAM);
    nvt_set_cb(cb);
    if (publish) {
        if (refused) {
            nvt_kreq_dirty(surf, 0, NVT_KD_REFUSED, -1, 19, 12);
            uint32_t bands[NVT_BANDS];
            fill(0);   /* what the readback of a cleared image holds */
            nvt_band_hash(g_pix, (size_t)W * 4, W, H, bands);
            nvt_ev_surf(NVT_K_SURF_OUT, NVT_OUT_COPY_AFTER_REFUSE, surf, 0, W, H, 80, W * 4u, NVT_F_VRAM, 19, seed, seed + 1, t0,
                        (uint64_t)W * H * 4u, bands);
        } else {
            nvt_kreq_dirty(surf, 0, NVT_KD_ACCEPTED, 0, 0, 9);
            nvt_ev_surf(NVT_K_SURF_OUT, NVT_OUT_DIRTY, surf, 0, W, H, 80, W * 4u, NVT_F_VRAM, 0, seed, seed, t0, 0, NULL);
        }
    }
    nvt_set_cb(0);
    nvt_ev_cb_done(cb, 0, 4, t0, 1);
}

static int scenario(void)
{
    CHECK(nvt_on());
    nvt_ev_map(101, 0, W, H, 80, W * 4u, 1, 0, 0);
    nvt_ev_map(102, 0, W, H, 80, W * 4u, 0, NVT_WHY_PITCH, 0);
    nvt_kreq_probe(1, -1, ENOENT);
    for (unsigned f = 0; f < 8; f++) {
        const uint32_t cb = nvt_next_id();
        if (f % 2 == 0) frame(cb, 101, 0, 0, 1, 40 + f);     /* surface 101: rendered, marked dirty        */
        else            frame(cb, 102, 1, 1, 1, 50 + f);     /* surface 102: cleared, dirty refused, empty */
    }
    /* surface 107: the pass drew, the dirty mark was refused, and the copy-out came back empty: the drawing was lost */
    { const uint32_t cb = nvt_next_id(); frame(cb, 107, 0, 1, 1, 60); }
    /* a command buffer that dirtied surface 103 and never published it */
    { const uint32_t cb = nvt_next_id(); frame(cb, 103, 0, 0, 0, 7); }
    /* surface 102's last publish was the empty copy-out, and its next surface-in is skipped as "VRAM current" */
    nvt_ev_surf(NVT_K_SURF_IN, NVT_IN_SKIP_VRAM, 102, 0, W, H, 80, W * 4u, NVT_F_VRAM, 0, 58, 58, nvt_now(), 0, NULL);
    /* a surface-in skipped on a tag this process never wrote, then one that uploaded */
    nvt_ev_surf(NVT_K_SURF_IN, NVT_IN_SKIP_VRAM, 105, 0, 640, 480, 80, 2560, NVT_F_VRAM, 0, 9, 9, nvt_now(), 0, NULL);
    { uint32_t bands[NVT_BANDS]; fill(0x00ff8040u); nvt_band_hash(g_pix, (size_t)W * 4, W, H, bands);
      nvt_ev_surf(NVT_K_SURF_IN, NVT_IN_UPLOAD, 106, 0, W, H, 80, W * 4u, NVT_F_RO_LOCK, 0, 3, 3, nvt_now(), (uint64_t)W * H * 4u, bands); }
    nvt_count(NVT_C_IN_SYNCED, 5);
    /* the Vulkan boundary */
    nvt_vk_submit(1, "nvmtl_vk_image_read", NVT_SUB_OK, 0, 150, 1, 0);
    nvt_vk_wait(1, "nvmtl_vk_image_read", NVT_WAIT_OK, 0, 900, 0, 5000);
    nvt_vk_submit(2, "surface upload", NVT_SUB_FAILED, -4, 20, 1, 0);
    nvt_vk_disable(2, "surface upload", NVT_DIS_SUBMIT, -4);
    nvt_vk_submit(3, "nvmtl_vk_image_read", NVT_SUB_OK, 0, 60, 1, 0);
    nvt_vk_wait(3, "nvmtl_vk_image_read", NVT_WAIT_TIMEOUT, 2, 5001000, 5001, 5000);
    nvt_alloc_fail("vkAllocateMemory(image)", -2);
    nvt_alloc_fail("newBufferWithBytesNoCopy: zero length -> nil", 0);
    /* a plugin older than DRIVER-PLAN N43: RADV refused a linear buffer texture with a pitch that is not a multiple of 256 B */
    nvt_alloc_fail("vkCreateImage(buffer texture)", -1000158000);
    nvt_alloc_fail("vkCreateImage(buffer texture)", -1000158000);
    nvt_alloc_fail("vkCreateImage(buffer texture)", -1000158000);
    nvt_kreq_import(104, 0, 4096u * 4u, NVT_KI_ALLOC_REFUSED, -2, 12, 300);
    return 0;
}

static int wrap(void)
{
    CHECK(nvt_on());
    for (unsigned i = 0; i < 1000; i++) nvt_ev_cb_commit(i + 1, 0, 0, 0);
    for (unsigned i = 0; i < 700; i++) nvt_vk_submit(i + 1, "x", NVT_SUB_OK, 0, 1, 1, 0);
    return 0;
}

static int parity(void)
{
    /* the kernel's loop, accel/rdna3-accel.cpp rd3AccelBandHash, over the shared row helper */
    static const unsigned dims[][3] = { { 2560, 1440, 2560 * 4 }, { 1920, 1080, 1920 * 4 + 64 }, { 100, 20, 512 }, { 64, 32, 256 },
                                        { 3840, 2160, 3840 * 4 }, { 47, 33, 192 }, { 1280, 31, 5120 } };
    for (unsigned d = 0; d < sizeof dims / sizeof dims[0]; d++) {
        const unsigned w = dims[d][0], h = dims[d][1], pitch = dims[d][2];
        uint32_t *px = (uint32_t *)malloc((size_t)pitch * h);
        CHECK(px);
        unsigned long s = 12345u + d;
        for (size_t i = 0; i < (size_t)pitch * h / 4; i++) { s = s * 6364136223846793005ul + 1442695040888963407ul; px[i] = (uint32_t)(s >> 24); }
        uint32_t want[NVT_BANDS], got[NVT_BANDS];
        for (unsigned b = 0; b < NVT_BANDS; b++) {
            unsigned rows[4];
            const unsigned nr = rd3_flicker_band_rows(b, NVT_BANDS, h, b ? 2u : 4u, rows);
            uint32_t v = 2166136261u;
            for (unsigned i = 0; i < nr; i++)
                for (unsigned c = 0; c < NVT_COLS; c++) {
                    const unsigned x = (w * (2 * c + 1)) / (2 * NVT_COLS);
                    v = (v ^ (px[(unsigned long)rows[i] * (pitch / 4) + x] & 0xffffffu)) * 16777619u;
                }
            want[b] = v;
        }
        nvt_band_hash(px, pitch, w, h, got);
        for (unsigned b = 0; b < NVT_BANDS; b++) {
            if (want[b] != got[b]) { fprintf(stderr, "parity: %ux%u band %u: kernel %08x plugin %08x\n", w, h, b, want[b], got[b]); return 1; }
        }
        free(px);
    }
    puts("parity ok");
    return 0;
}

static int squat(void)
{
    /* the directory is world writable: someone plants the trace's name before the process starts */
    const char *dir = getenv("NVMTL_TRACE_DIR");
    char p[512], victim[512];
    CHECK(dir);
    snprintf(victim, sizeof victim, "%s/victim.txt", dir);
    snprintf(p, sizeof p, "%s/%s-%u.trace", dir, getenv("NVMTL_TRACE_NAME"), (unsigned)getpid());
    { int fd = open(victim, O_WRONLY | O_CREAT | O_TRUNC, 0600); CHECK(fd >= 0); CHECK(write(fd, "keep", 4) == 4); close(fd); }
    CHECK(mkdir(dir, 0700) == 0 || errno == EEXIST);
    CHECK(symlink(victim, p) == 0);
    CHECK(nvt_on());
    CHECK(strstr(g_nvt.path, ".1-") != NULL);   /* the next name */
    nvt_ev_cb_commit(1, 0, 0, 0);
    { char buf[8] = { 0 }; int fd = open(victim, O_RDONLY); CHECK(fd >= 0); CHECK(read(fd, buf, 8) == 4); close(fd); CHECK(memcmp(buf, "keep", 4) == 0); }
    return 0;
}

static int dirs(const char *base)
{
    char d[512], l[512];
    struct stat st;
    CHECK(mkdir(base, 0700) == 0 || errno == EEXIST);
    snprintf(d, sizeof d, "%s/shared", base);
    int fd = nvt_open_dir(d, 1);
    CHECK(fd >= 0);
    CHECK(fstat(fd, &st) == 0 && (st.st_mode & 07777) == 01777);   /* made by this call: sticky and world writable */
    close(fd);
    fd = nvt_open_dir(d, 1);   /* it exists now: opened, not chmod'ed again */
    CHECK(fd >= 0);
    close(fd);
    snprintf(d, sizeof d, "%s/private", base);
    fd = nvt_open_dir(d, 0);
    CHECK(fd >= 0 && fstat(fd, &st) == 0 && (st.st_mode & 07777) == 0700);   /* a named directory is never made public */
    close(fd);
    snprintf(l, sizeof l, "%s/link", base);
    CHECK(symlink(d, l) == 0);
    errno = 0;
    CHECK(nvt_open_dir(l, 1) < 0 && (errno == ELOOP || errno == ENOTDIR || errno == EMLINK));   /* never follow a link */
    CHECK(fstatat(AT_FDCWD, d, &st, 0) == 0 && (st.st_mode & 07777) == 0700);
    if (geteuid() == 0) {   /* root never writes into a directory somebody else owns */
        snprintf(d, sizeof d, "%s/foreign", base);
        CHECK(mkdir(d, 0755) == 0 && chown(d, 1234, 1234) == 0);
        errno = 0;
        CHECK(nvt_open_dir(d, 1) < 0 && errno == EPERM);
        snprintf(d, sizeof d, "%s/ws", base);
        CHECK(mkdir(d, 0755) == 0 && chown(d, NVT_UID_WINDOWSERVER, 1234) == 0);
        errno = 0;
        CHECK(nvt_open_dir(d, 1) < 0 && errno == EPERM);
    }
    puts("dirs ok");
    return 0;
}

static void touch_old(const char *dir, const char *name, long age)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    int fd = open(p, O_WRONLY | O_CREAT, 0600);
    CHECK(fd >= 0);
    close(fd);
    struct timespec ts[2] = { { time(NULL) - age, 0 }, { time(NULL) - age, 0 } };
    CHECK(utimensat(AT_FDCWD, p, ts, 0) == 0);
}
static int exists(const char *dir, const char *name)
{
    char p[512];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return lstat(p, &st) == 0;
}
static int prune(const char *dir)
{
    char n[64];
    CHECK(mkdir(dir, 0700) == 0);
    /* 80 dead apps (pids no process has), the newest ones a minute old; dead WindowServer traces; a live one (ours);
     * one that merely looks like a trace */
    for (int i = 0; i < 80; i++) { snprintf(n, sizeof n, "App-%d.trace", 4100000 + i); touch_old(dir, n, 60 + 10 * (80 - i)); }
    touch_old(dir, "WindowServer-4200000.trace", 7200);
    touch_old(dir, "WindowServer.1-4200001.trace", 4 * 86400);   /* a dead one older than 3 days goes by age */
    snprintf(n, sizeof n, "Live-%u.trace", (unsigned)getpid()); touch_old(dir, n, 4 * 86400);
    touch_old(dir, "notes.txt", 10 * 86400);
    const int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    CHECK(dfd >= 0);
    nvt_prune_fd(dfd, "keepme-1.trace");
    close(dfd);
    unsigned left = 0;
    for (int i = 0; i < 80; i++) { snprintf(n, sizeof n, "App-%d.trace", 4100000 + i); left += (unsigned)exists(dir, n); }
    CHECK(left == NVT_KEEP_FILES - 2);   /* 64 allowed: the two WindowServer files count in the total, 62 apps stay */
    CHECK(!exists(dir, "App-4100000.trace") && exists(dir, "App-4100079.trace"));   /* the oldest went */
    CHECK(exists(dir, "WindowServer-4200000.trace"));    /* never for the count */
    CHECK(!exists(dir, "WindowServer.1-4200001.trace"));  /* by age */
    snprintf(n, sizeof n, "Live-%u.trace", (unsigned)getpid());
    CHECK(exists(dir, n) && exists(dir, "notes.txt"));
    puts("prune ok");
    return 0;
}

static int append(const char *dir)
{
    char p[512], q[512];
    struct stat st;
    CHECK(mkdir(dir, 0700) == 0);
    snprintf(p, sizeof p, "%s/log", dir);
    int fd = nvt_open_append(p, 0644);
    CHECK(fd >= 0);
    CHECK(!(fcntl(fd, F_GETFL) & O_NONBLOCK));   /* the log writes block like any file's */
    CHECK(write(fd, "a", 1) == 1);
    close(fd);
    fd = nvt_open_append(p, 0644);   /* an existing plain file of ours */
    CHECK(fd >= 0);
    close(fd);
    snprintf(q, sizeof q, "%s/link", dir);
    CHECK(symlink(p, q) == 0);
    errno = 0;
    CHECK(nvt_open_append(q, 0644) < 0 && errno == ELOOP);
    snprintf(q, sizeof q, "%s/fifo", dir);
    CHECK(mkfifo(q, 0600) == 0);
    errno = 0;
    CHECK(nvt_open_append(q, 0644) < 0 && (errno == ENXIO || errno == EPERM));   /* never blocks, never opens it */
    snprintf(q, sizeof q, "%s/hard", dir);
    CHECK(link(p, q) == 0);
    errno = 0;
    CHECK(nvt_open_append(q, 0644) < 0 && errno == EPERM);
    CHECK(nvt_open_append(p, 0644) < 0 && errno == EPERM);   /* the original has two links now */
    if (geteuid() == 0) {
        snprintf(q, sizeof q, "%s/foreign", dir);
        fd = open(q, O_WRONLY | O_CREAT, 0644);
        CHECK(fd >= 0);
        close(fd);
        CHECK(chown(q, 1234, 1234) == 0);
        errno = 0;
        CHECK(nvt_open_append(q, 0644) < 0 && errno == EPERM);   /* root does not append to a user's file */
    }
    CHECK(stat(dir, &st) == 0);
    puts("append ok");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "append") && argc > 2) return append(argv[2]);
    if (!strcmp(argv[1], "squat")) return squat();
    if (!strcmp(argv[1], "dir") && argc > 2) return dirs(argv[2]);
    if (!strcmp(argv[1], "prune") && argc > 2) return prune(argv[2]);
    if (!strcmp(argv[1], "scenario")) return scenario();
    if (!strcmp(argv[1], "wrap")) return wrap();
    if (!strcmp(argv[1], "parity")) return parity();
    if (!strcmp(argv[1], "off")) { CHECK(!nvt_on()); nvt_ev_cb_commit(1, 0, 0, 0); CHECK(!nvt_on()); return 0; }
    return 2;
}
