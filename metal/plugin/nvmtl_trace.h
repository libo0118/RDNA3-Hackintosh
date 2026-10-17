/*
 * nvmtl_trace.h - per-process binary frame trace of the Metal plugin.
 *
 * Why: WindowServer composites through this plugin (Metal -> Vulkan -> RADV ->
 * the kext's DRM). The plugin's text log is rate limited (first 6, then every
 * 2000th), so on a card run nobody could tell, frame by frame, which IOSurface
 * WindowServer rendered into, whether the plugin copied it out, skipped it or
 * marked it dirty, and whether the kernel refused. This file is the record.
 *
 * What: every process that loads the plugin maps
 *   /var/tmp/rdna3-nvmtl/<process>-<pid>.trace      (fallback /private/tmp/...)
 * and writes fixed-size records into it. No syscall per event: the file is an
 * mmap'd ring, three regions:
 *   main ring  surface in/out, render passes, command buffers, kernel requests,
 *              failures (default 16384 records of 128 bytes)
 *   vk ring    every vkQueueSubmit / fence wait (a separate ring: a flood of
 *              small submits must not push the surface events out)
 *   summary    one record per second of process life (8192 seconds, then the
 *              oldest are overwritten) with counters of everything, so the
 *              long picture survives however fast the rings wrap
 * tools/plugin_trace.py decodes it (the layout below is its contract; keep both
 * in step, tests/test_plugin_trace.sh runs this header against the decoder).
 *
 * Always on, never changes behaviour (it only reads state the plugin already
 * has; it takes no IOSurface lock). Off with NVMTL_TRACE=0 in the environment
 * or as a line "NVMTL_TRACE=0" in /Library/GPUBundles/rdna3mtl/rdna3mtl.conf.
 * Other knobs: NVMTL_TRACE_RING=<records> (main ring size), NVMTL_TRACE_HASH
 * (0 none, 1 hash only memory the plugin has locked anyway (default), 2 also
 * hash unlocked CPU memory of surfaces it skips or marks dirty), NVMTL_TRACE_DIR.
 *
 * Portable C99 plus GCC/clang builtins; compiled into the plugin (one
 * translation unit) and by the host test.
 */
#ifndef NVMTL_TRACE_H
#define NVMTL_TRACE_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#else
#include <sys/syscall.h>
#endif

void nvlog(const char *fmt, ...);

#define NVT_VERSION   1u
#define NVT_REC_SIZE  128u
#define NVT_HDR_SIZE  4096u
#define NVT_RING_DEFAULT 16384u
#define NVT_SUM_SLOTS 8192u
#define NVT_BANDS     8u
#define NVT_COLS      48u

/* record kinds */
enum {
    NVT_K_START = 1,      /* the trace opened                                                            */
    NVT_K_SURF_IN = 2,    /* CPU -> GPU: IOSurface pixels brought into the GPU image (or skipped)         */
    NVT_K_SURF_OUT = 3,   /* GPU -> CPU: a GPU write published (dirty mark, or copy out into the surface) */
    NVT_K_PASS_BEGIN = 4, /* a render pass with an IOSurface colour attachment (one record per attachment)*/
    NVT_K_PASS_END = 5,   /* its endEncoding: what the pass turned out to be                              */
    NVT_K_CB_COMMIT = 6,  /* MTLCommandBuffer commit                                                      */
    NVT_K_CB_DIRTY = 7,   /* one per IOSurface the committed command buffer wrote                         */
    NVT_K_CB_DONE = 8,    /* command buffer finished (completed or errored)                               */
    NVT_K_VK_SUBMIT = 9,  /* vkQueueSubmit (vk ring)                                                      */
    NVT_K_VK_WAIT = 10,   /* fence wait of that submit (vk ring)                                          */
    NVT_K_VK_DISABLE = 11,/* the plugin switched its Vulkan backend off                                   */
    NVT_K_ALLOC_FAIL = 12,/* an allocation / import / Vulkan object creation failed                       */
    NVT_K_KREQ_PROBE = 13,/* "kext surface BOs probe"                                                     */
    NVT_K_KREQ_DIRTY = 14,/* the dirty ioctl for a surface                                                */
    NVT_K_KREQ_IMPORT = 15,/* the kernel's surface BO import (family VRAM) for a surface                  */
    NVT_K_SURF_MAP = 16   /* a surface got its texture backing: family VRAM or a private image            */
};

/* SURF_IN paths */
enum { NVT_IN_UPLOAD = 1, NVT_IN_SKIP_VRAM = 2, NVT_IN_LOCK_FAILED = 3, NVT_IN_UPLOAD_FAILED = 4, NVT_IN_NOT_COPYABLE = 5 };
/* SURF_OUT paths. rc: for OUT_COPY_AFTER_REFUSE the dirty request's errno (negative: -2 dirty path switched off,
 * -3 no Vulkan device), for the failures the IOReturn / -1 */
enum { NVT_OUT_DIRTY = 1, NVT_OUT_COPY_PRIVATE = 2, NVT_OUT_COPY_AFTER_REFUSE = 3, NVT_OUT_LOCK_FAILED = 4,
       NVT_OUT_READBACK_FAILED = 5, NVT_OUT_NOT_COPYABLE = 6 };
/* PASS_END paths */
enum { NVT_PASS_DRAWN = 1, NVT_PASS_CLEAR_ONLY = 2, NVT_PASS_EMPTY = 3, NVT_PASS_EMPTY_FAILED = 4,
       NVT_PASS_LOAD_UNTOUCHED = 5, NVT_PASS_REFUSED = 6, NVT_PASS_CLEAR_FAILED = 7 };
/* VK_SUBMIT paths */
enum { NVT_SUB_OK = 1, NVT_SUB_FAILED = 2, NVT_SUB_EARLY_FAIL = 3, NVT_SUB_NO_BACKEND = 4 };
/* VK_WAIT paths */
enum { NVT_WAIT_OK = 1, NVT_WAIT_TIMEOUT = 2, NVT_WAIT_ERROR = 3, NVT_WAIT_WITNESS = 4 };
/* VK_DISABLE paths */
enum { NVT_DIS_SUBMIT = 1, NVT_DIS_WAIT = 2, NVT_DIS_WITNESS = 3 };
/* KREQ_DIRTY paths */
enum { NVT_KD_ACCEPTED = 1, NVT_KD_REFUSED = 2, NVT_KD_OFF = 3, NVT_KD_NO_DEVICE = 4 };
/* KREQ_IMPORT paths */
enum { NVT_KI_OK = 1, NVT_KI_NO_FD = 2, NVT_KI_ALLOC_REFUSED = 3, NVT_KI_BIND_REFUSED = 4, NVT_KI_SHARE_OFF = 5 };
/* SURF_MAP paths; a0 = why private */
enum { NVT_MAP_VRAM = 1, NVT_MAP_PRIVATE = 2 };
enum { NVT_WHY_PLANAR = 1, NVT_WHY_WIDER = 2, NVT_WHY_TALLER = 3, NVT_WHY_PITCH = 4, NVT_WHY_SHARE_OFF = 5,
       NVT_WHY_IMPORT_FAILED = 6, NVT_WHY_ALIAS_FAILED = 7 };

/* record flags */
#define NVT_F_VRAM          0x01u  /* the surface's texture lives in the shared family VRAM          */
#define NVT_F_RO_LOCK       0x02u  /* the IOSurface lock was read-only (the seed does not move)       */
#define NVT_F_HASHED        0x04u  /* band[] holds the band hash of the CPU memory                    */
#define NVT_F_HASH_UNLOCKED 0x08u  /* ... read without an IOSurface lock (NVMTL_TRACE_HASH=2)        */
#define NVT_F_VIEW          0x10u  /* the pass attachment is a view of the surface texture            */
#define NVT_F_CURRENT       0x20u  /* SURF_MAP: the VRAM was already current at the surface's seed    */

/* the 128-byte record. Everything little endian, 8-byte aligned fields first. */
typedef struct nvt_rec {
    uint64_t abs;       /*   0 mach_absolute_time ticks (header says how to convert)                      */
    uint32_t seq;       /*   8 per-process sequence number, 1-based, shared by both rings; 0 = empty/torn */
    uint32_t tid;       /*  12 thread id                                                                  */
    uint8_t  kind;      /*  16 NVT_K_*                                                                    */
    uint8_t  path;      /*  17 the decision / outcome, per kind                                           */
    uint8_t  plane;     /*  18 IOSurface plane                                                            */
    uint8_t  flags;     /*  19 NVT_F_*                                                                    */
    uint32_t surf;      /*  20 IOSurfaceGetID (0: none)                                                   */
    int32_t  rc;        /*  24 result: IOReturn, errno, VkResult, command buffer error code               */
    uint32_t w, h;      /*  28, 32 surface / image size                                                   */
    uint32_t fmt;       /*  36 MTLPixelFormat (pass attachments, surfaces) or VkFormat                    */
    uint32_t bpr;       /*  40 IOSurface bytesPerRow                                                      */
    uint32_t seed0;     /*  44 IOSurfaceGetSeed before                                                    */
    uint32_t seed1;     /*  48 ... after (the seed the lock / unlock returned)                            */
    uint32_t dur_us;    /*  52 how long the operation took                                                */
    uint64_t bytes;     /*  56 bytes moved                                                                */
    uint32_t id;        /*  64 command buffer id (SURF_*, PASS_*, CB_*) / submit seq (VK_*)               */
    uint32_t a0, a1, a2;/*  68 72 76 per kind, see tools/plugin_trace.py                                  */
    union {             /*  80 .. 127                                                                     */
        struct { uint32_t band[NVT_BANDS]; uint32_t a3, a4, a5, a6; } n;   /* band hash / clear colour(f32 bits) */
        char text[48];                                                       /* VK_*, ALLOC_FAIL: who / what       */
    } u;
} nvt_rec;
typedef char nvt_rec_size_check[sizeof(nvt_rec) == NVT_REC_SIZE ? 1 : -1];

/* summary counters (index into nvt_sum.cnt) */
enum {
    NVT_C_EVENTS, NVT_C_IN_UPLOAD, NVT_C_IN_SKIP, NVT_C_IN_SYNCED, NVT_C_IN_FAIL,
    NVT_C_OUT_COPY, NVT_C_OUT_DIRTY, NVT_C_OUT_REFUSED, NVT_C_OUT_FAIL,
    NVT_C_PASSES, NVT_C_PASSES_SURF, NVT_C_CLEAR_ONLY, NVT_C_CLEAR_ONLY_BIG,
    NVT_C_CB_COMMIT, NVT_C_CB_DONE, NVT_C_CB_ERR,
    NVT_C_VK_SUBMIT, NVT_C_VK_SUBMIT_ERR, NVT_C_VK_WAIT_TIMEOUT, NVT_C_VK_WAIT_ERR, NVT_C_VK_DISABLE,
    NVT_C_ALLOC_FAIL, NVT_C_KREQ, NVT_C_KREQ_ERR, NVT_C_MAP_VRAM, NVT_C_MAP_PRIVATE,
    NVT_C_KB_IN, NVT_C_KB_OUT,
    NVT_NCNT
};
/* one second of process life; cnt[] are counts within that second */
typedef struct nvt_sum {
    uint32_t sec1;      /* second since start + 1; 0 empty, 0xffffffff being reset */
    uint32_t flags;
    uint64_t abs_first; /* abs of the event that opened the second */
    uint32_t cnt[NVT_NCNT];
} nvt_sum;
typedef char nvt_sum_size_check[sizeof(nvt_sum) == NVT_REC_SIZE ? 1 : -1];

typedef struct nvt_hdr {
    char     magic[8];      /*   0 "NVMTLTR1"                                      */
    uint32_t version;       /*   8                                                 */
    uint32_t rec_size;      /*  12                                                 */
    uint32_t nmain, nvk, nsum; /* 16 20 24 records per region                      */
    uint32_t pid;           /*  28                                                 */
    uint64_t off_main, off_vk, off_sum; /* 32 40 48 byte offsets of the regions    */
    uint32_t tb_numer, tb_denom; /* 56 60 ns = ticks * numer / denom               */
    uint64_t start_abs;     /*  64 abs ticks when the trace opened                 */
    uint64_t start_wall_ns; /*  72 CLOCK_REALTIME ns then                          */
    uint64_t widx_main;     /*  80 records ever written to the main ring           */
    uint64_t widx_vk;       /*  88 ... to the vk ring                              */
    uint32_t seq_next;      /*  96 sequence numbers handed out                     */
    uint32_t flags;         /* 100 bit0: band hashing on (mode 1+), bit1: mode 2   */
    uint32_t uid;           /* 104                                                 */
    uint32_t ncnt;          /* 108 counters per summary record                     */
    char     proc[64];      /* 112 process name                                    */
} nvt_hdr;

enum { NVT_RING_MAIN = 0, NVT_RING_VK = 1 };

static struct {
    int state;              /* 0 not opened, 2 opening, 1 on, -1 off */
    nvt_hdr *hdr;
    unsigned char *base;
    size_t size;
    nvt_rec *ring[2];
    uint32_t nring[2];
    nvt_sum *sum;
    uint32_t nsum;
    uint64_t start_abs;
    uint32_t tb_n, tb_d;
    int hash_mode;
    char path[320];
} g_nvt = { 0, NULL, NULL, 0, { NULL, NULL }, { 0, 0 }, NULL, 0, 0, 1, 1, 1, { 0 } };
static __thread uint32_t g_nvt_cb;   /* id of the command buffer the calling thread is publishing for */

static inline uint64_t nvt_now(void)
{
#ifdef __APPLE__
    return mach_absolute_time();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}
static inline uint64_t nvt_ticks_to_ns(uint64_t t) { return t * g_nvt.tb_n / g_nvt.tb_d; }
static inline uint32_t nvt_us_since(uint64_t t0)
{
    const uint64_t now = nvt_now();
    const uint64_t us = now > t0 ? nvt_ticks_to_ns(now - t0) / 1000u : 0u;
    return us > 0xffffffffull ? 0xffffffffu : (uint32_t)us;
}
static inline uint32_t nvt_tid(void)
{
    static __thread uint32_t tid;
    if (!tid) {
#ifdef __APPLE__
        uint64_t t = 0; pthread_threadid_np(NULL, &t); tid = (uint32_t)t;
#else
        tid = (uint32_t)syscall(SYS_gettid);
#endif
        if (!tid) tid = 1;
    }
    return tid;
}
static inline int nvt_hash_mode(void) { return g_nvt.hash_mode; }
static inline void nvt_set_cb(uint32_t id) { g_nvt_cb = id; }
static inline uint32_t nvt_cur_cb(void) { return g_nvt_cb; }
/* ids for command buffers, passes, submits: one counter per process (never 0) */
static inline uint32_t nvt_next_id(void)
{
    static uint32_t ids;
    uint32_t v = __atomic_add_fetch(&ids, 1u, __ATOMIC_RELAXED);
    return v ? v : __atomic_add_fetch(&ids, 1u, __ATOMIC_RELAXED);
}

/* ---- the file ---------------------------------------------------------- */

static void nvt_prune(const char *dir, const char *keep)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct nvt_ent { time_t mt; unsigned long pid; char name[80]; } ent[256];
    unsigned n = 0;
    struct dirent *e;
    while (n < 256 && (e = readdir(d)) != NULL) {
        const size_t l = strlen(e->d_name);
        if (l < 9 || l >= sizeof ent[0].name || strcmp(e->d_name + l - 6, ".trace") != 0 || strcmp(e->d_name, keep) == 0) continue;
        const char *dash = strrchr(e->d_name, '-');
        if (!dash) continue;
        char *end = NULL;
        const unsigned long pid = strtoul(dash + 1, &end, 10);
        if (end == dash + 1 || strcmp(end, ".trace") != 0) continue;
        char p[512];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (lstat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        ent[n].mt = st.st_mtime; ent[n].pid = pid; memcpy(ent[n].name, e->d_name, l + 1); n++;
    }
    closedir(d);
    for (unsigned i = 1; i < n; i++) {   /* oldest first */
        struct nvt_ent t = ent[i]; unsigned j = i;
        while (j && ent[j - 1].mt > t.mt) { ent[j] = ent[j - 1]; j--; }
        ent[j] = t;
    }
    const time_t now = time(NULL);
    unsigned total = n;
    for (unsigned i = 0; i < n; i++) {
        const int dead = kill((pid_t)ent[i].pid, 0) != 0 && errno == ESRCH;
        if (!dead || (total <= 64 && now - ent[i].mt <= 3 * 86400)) continue;
        char p[512];
        snprintf(p, sizeof p, "%s/%s", dir, ent[i].name);
        if (unlink(p) == 0) total--;
    }
}

/* the directory is shared by every user the plugin runs as (root daemons, _windowserver, apps): sticky and world
 * writable like /tmp. Refuse a path that is not a directory of root's or ours (a planted symlink). */
static int nvt_dir_ok(const char *dir)
{
    struct stat st;
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) return -1;
    if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    if (st.st_uid != 0 && st.st_uid != geteuid()) { errno = EPERM; return -1; }
    if (st.st_uid == geteuid()) (void)chmod(dir, 01777);
    return 0;
}
static int nvt_open_file(const char *dir, const char *file, char *path, size_t pn)
{
    snprintf(path, pn, "%s/%s", dir, file);
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0 && errno == EEXIST) {   /* a dead process of that pid left it */
        struct stat st;
        if (lstat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() && unlink(path) == 0)
            fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    }
    if (fd >= 0) (void)fchmod(fd, 0644);
    return fd;
}

static const char *nvt_progname(void)
{
    const char *e = getenv("NVMTL_TRACE_NAME");
    if (e && *e) return e;
#ifdef __APPLE__
    return getprogname() ? getprogname() : "unknown";
#else
    return "host";
#endif
}

static void nvt_atfork_child(void)
{
    /* the child must not write into the parent's file: drop the mapping, open its own on the next event */
    if (g_nvt.state == 1 && g_nvt.base) {
        munmap(g_nvt.base, g_nvt.size);
        g_nvt.base = NULL; g_nvt.hdr = NULL; g_nvt.sum = NULL;
        g_nvt.ring[0] = g_nvt.ring[1] = NULL;
        g_nvt.state = 0;
    }
    g_nvt_cb = 0;
}

static int nvt_open_map(void)
{
    const char *off = getenv("NVMTL_TRACE");
    if (off && off[0] == '0') { nvlog("trace: frame trace switched off (NVMTL_TRACE=0)"); return 0; }
    uint32_t nmain = NVT_RING_DEFAULT;
    const char *rs = getenv("NVMTL_TRACE_RING");
    if (rs && *rs) {
        const unsigned long v = strtoul(rs, NULL, 10);
        nmain = v < 256 ? 256u : v > (1ul << 20) ? (1u << 20) : (uint32_t)v;
    }
    const uint32_t nvk = nmain / 2u > 256u ? nmain / 2u : 256u;
    const char *hs = getenv("NVMTL_TRACE_HASH");
    g_nvt.hash_mode = hs && *hs ? (hs[0] == '0' ? 0 : hs[0] == '2' ? 2 : 1) : 1;

    char name[96], file[160], proc[64];
    const char *pn = nvt_progname();
    size_t k = 0;
    for (; pn[k] && k < sizeof proc - 1; k++) {
        const char c = pn[k];
        proc[k] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == ' ') ? c : '_';
        name[k] = proc[k] == ' ' ? '_' : proc[k];
    }
    proc[k] = 0; name[k] = 0;
    if (!proc[0]) { snprintf(name, sizeof name, "unknown"); snprintf(proc, sizeof proc, "unknown"); }
    snprintf(file, sizeof file, "%s-%u.trace", name, (unsigned)getpid());

    const char *dirs[3] = { getenv("NVMTL_TRACE_DIR"), "/var/tmp/rdna3-nvmtl", "/private/tmp/rdna3-nvmtl" };
    int fd = -1, err = 0;
    const char *used = NULL;
    for (int i = (dirs[0] && *dirs[0]) ? 0 : 1; i < 3; i++) {
        if (nvt_dir_ok(dirs[i]) != 0) { err = errno; continue; }
        fd = nvt_open_file(dirs[i], file, g_nvt.path, sizeof g_nvt.path);
        if (fd >= 0) { used = dirs[i]; break; }
        err = errno;
    }
    if (fd < 0) {
        nvlog("trace: cannot create the frame trace %s (errno %d %s) - no trace for this process", file, err, strerror(err));
        return 0;
    }
    const size_t sz = NVT_HDR_SIZE + ((size_t)nmain + nvk + NVT_SUM_SLOTS) * NVT_REC_SIZE;
    void *m = MAP_FAILED;
    if (ftruncate(fd, (off_t)sz) == 0) m = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    const int e2 = errno;
    close(fd);
    if (m == MAP_FAILED) {
        unlink(g_nvt.path);
        nvlog("trace: cannot map the frame trace %s (errno %d %s) - no trace for this process", g_nvt.path, e2, strerror(e2));
        return 0;
    }
#ifdef __APPLE__
    { mach_timebase_info_data_t tb; if (mach_timebase_info(&tb) == 0 && tb.denom) { g_nvt.tb_n = tb.numer; g_nvt.tb_d = tb.denom; } }
#endif
    nvt_hdr *h = (nvt_hdr *)m;
    memset(h, 0, sizeof *h);
    memcpy(h->magic, "NVMTLTR1", 8);
    h->version = NVT_VERSION; h->rec_size = NVT_REC_SIZE;
    h->nmain = nmain; h->nvk = nvk; h->nsum = NVT_SUM_SLOTS;
    h->pid = (uint32_t)getpid(); h->uid = (uint32_t)geteuid();
    h->off_main = NVT_HDR_SIZE; h->off_vk = NVT_HDR_SIZE + (uint64_t)nmain * NVT_REC_SIZE;
    h->off_sum = h->off_vk + (uint64_t)nvk * NVT_REC_SIZE;
    h->tb_numer = g_nvt.tb_n; h->tb_denom = g_nvt.tb_d;
    h->ncnt = NVT_NCNT;
    h->flags = (g_nvt.hash_mode >= 1 ? 1u : 0u) | (g_nvt.hash_mode >= 2 ? 2u : 0u);
    snprintf(h->proc, sizeof h->proc, "%s", proc);
    struct timespec wall; clock_gettime(CLOCK_REALTIME, &wall);
    h->start_wall_ns = (uint64_t)wall.tv_sec * 1000000000ull + (uint64_t)wall.tv_nsec;
    g_nvt.start_abs = h->start_abs = nvt_now();
    g_nvt.hdr = h; g_nvt.base = (unsigned char *)m; g_nvt.size = sz;
    g_nvt.ring[0] = (nvt_rec *)(void *)(g_nvt.base + h->off_main); g_nvt.nring[0] = nmain;
    g_nvt.ring[1] = (nvt_rec *)(void *)(g_nvt.base + h->off_vk);   g_nvt.nring[1] = nvk;
    g_nvt.sum = (nvt_sum *)(void *)(g_nvt.base + h->off_sum);      g_nvt.nsum = NVT_SUM_SLOTS;
    static int forkHook;
    if (!forkHook) { forkHook = 1; pthread_atfork(NULL, NULL, nvt_atfork_child); }
    nvt_prune(used, strrchr(g_nvt.path, '/') + 1);
    nvlog("trace: frame trace %s (%u + %u records of %u bytes, band hash mode %d); decode with tools/plugin_trace.py",
          g_nvt.path, (unsigned)nmain, (unsigned)nvk, (unsigned)NVT_REC_SIZE, g_nvt.hash_mode);
    return 1;
}

static int nvt_init_slow(void)
{
    int expect = 0;
    if (!__atomic_compare_exchange_n(&g_nvt.state, &expect, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return __atomic_load_n(&g_nvt.state, __ATOMIC_ACQUIRE) == 1;
    const int ok = nvt_open_map();
    __atomic_store_n(&g_nvt.state, ok ? 1 : -1, __ATOMIC_RELEASE);
    return ok;
}
/* 1 when events are recorded. The first call opens the file. */
static inline int nvt_on(void)
{
    const int s = __atomic_load_n(&g_nvt.state, __ATOMIC_ACQUIRE);
    if (__builtin_expect(s == 1, 1)) return 1;
    if (s == 0) return nvt_init_slow();
    return 0;
}

/* ---- counters and records ---------------------------------------------- */

static inline nvt_sum *nvt_sum_for(uint64_t abs)
{
    const uint64_t ns = abs > g_nvt.start_abs ? nvt_ticks_to_ns(abs - g_nvt.start_abs) : 0u;
    const uint32_t sec = (uint32_t)(ns / 1000000000ull);
    nvt_sum *s = &g_nvt.sum[sec % g_nvt.nsum];
    uint32_t cur = __atomic_load_n(&s->sec1, __ATOMIC_ACQUIRE);
    if (cur != sec + 1u && cur != 0xffffffffu) {
        if (__atomic_compare_exchange_n(&s->sec1, &cur, 0xffffffffu, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            memset(s->cnt, 0, sizeof s->cnt);
            s->abs_first = abs;
            __atomic_store_n(&s->sec1, sec + 1u, __ATOMIC_RELEASE);
        }
    }
    return s;   /* a thread that lost the claim adds into the second's slot anyway: a lost count at worst */
}
static inline void nvt_count_at(uint64_t abs, unsigned c, uint32_t n)
{
    if (c < NVT_NCNT) __atomic_fetch_add(&nvt_sum_for(abs)->cnt[c], n, __ATOMIC_RELAXED);
}
static inline void nvt_count(unsigned c, uint32_t n)
{
    if (nvt_on()) nvt_count_at(nvt_now(), c, n);
}

#define NVT_BIG_W 1024u   /* "display sized" for the summary's clear-only counter */
#define NVT_BIG_H 600u
static inline uint32_t nvt_kib(uint64_t b) { const uint64_t k = b >> 10; return k > 0xffffffffull ? 0xffffffffu : (uint32_t)k; }

/* the summary counters one record stands for */
static inline void nvt_count_record(const nvt_rec *r)
{
    const uint64_t t = r->abs;
    nvt_count_at(t, NVT_C_EVENTS, 1);
    switch (r->kind) {
    case NVT_K_SURF_IN:
        if (r->path == NVT_IN_UPLOAD) { nvt_count_at(t, NVT_C_IN_UPLOAD, 1); nvt_count_at(t, NVT_C_KB_IN, nvt_kib(r->bytes)); }
        else if (r->path == NVT_IN_SKIP_VRAM) nvt_count_at(t, NVT_C_IN_SKIP, 1);
        else nvt_count_at(t, NVT_C_IN_FAIL, 1);
        break;
    case NVT_K_SURF_OUT:
        if (r->path == NVT_OUT_DIRTY) nvt_count_at(t, NVT_C_OUT_DIRTY, 1);
        else if (r->path == NVT_OUT_COPY_PRIVATE || r->path == NVT_OUT_COPY_AFTER_REFUSE) {
            nvt_count_at(t, NVT_C_OUT_COPY, 1); nvt_count_at(t, NVT_C_KB_OUT, nvt_kib(r->bytes));
            if (r->path == NVT_OUT_COPY_AFTER_REFUSE) nvt_count_at(t, NVT_C_OUT_REFUSED, 1);
        } else nvt_count_at(t, NVT_C_OUT_FAIL, 1);
        break;
    case NVT_K_PASS_END:
        nvt_count_at(t, NVT_C_PASSES_SURF, 1);
        if (r->path == NVT_PASS_CLEAR_ONLY) {
            nvt_count_at(t, NVT_C_CLEAR_ONLY, 1);
            if (r->w >= NVT_BIG_W && r->h >= NVT_BIG_H) nvt_count_at(t, NVT_C_CLEAR_ONLY_BIG, 1);
        }
        break;
    case NVT_K_CB_COMMIT: nvt_count_at(t, NVT_C_CB_COMMIT, 1); break;
    case NVT_K_CB_DONE:
        nvt_count_at(t, NVT_C_CB_DONE, 1);
        if (r->rc) nvt_count_at(t, NVT_C_CB_ERR, 1);
        break;
    case NVT_K_VK_SUBMIT:
        nvt_count_at(t, NVT_C_VK_SUBMIT, 1);
        if (r->path != NVT_SUB_OK) nvt_count_at(t, NVT_C_VK_SUBMIT_ERR, 1);
        break;
    case NVT_K_VK_WAIT:
        if (r->path == NVT_WAIT_TIMEOUT) nvt_count_at(t, NVT_C_VK_WAIT_TIMEOUT, 1);
        else if (r->path != NVT_WAIT_OK) nvt_count_at(t, NVT_C_VK_WAIT_ERR, 1);
        break;
    case NVT_K_VK_DISABLE: nvt_count_at(t, NVT_C_VK_DISABLE, 1); break;
    case NVT_K_ALLOC_FAIL: nvt_count_at(t, NVT_C_ALLOC_FAIL, 1); break;
    case NVT_K_KREQ_PROBE:
        nvt_count_at(t, NVT_C_KREQ, 1);
        if (r->path == 0) nvt_count_at(t, NVT_C_KREQ_ERR, 1);
        break;
    case NVT_K_KREQ_DIRTY:
        nvt_count_at(t, NVT_C_KREQ, 1);
        if (r->path != NVT_KD_ACCEPTED) nvt_count_at(t, NVT_C_KREQ_ERR, 1);
        break;
    case NVT_K_KREQ_IMPORT:
        nvt_count_at(t, NVT_C_KREQ, 1);
        if (r->path != NVT_KI_OK) nvt_count_at(t, NVT_C_KREQ_ERR, 1);
        break;
    case NVT_K_SURF_MAP:
        nvt_count_at(t, r->path == NVT_MAP_VRAM ? NVT_C_MAP_VRAM : NVT_C_MAP_PRIVATE, 1);
        break;
    default: break;
    }
}

static inline void nvt_rec_init(nvt_rec *r, unsigned kind)
{
    memset(r, 0, sizeof *r);
    r->kind = (uint8_t)kind;
}
static inline void nvt_set_text(nvt_rec *r, const char *s)
{
    if (!s) return;
    size_t n = strlen(s);
    if (n > sizeof r->u.text - 1) n = sizeof r->u.text - 1;
    memset(r->u.text, 0, sizeof r->u.text);
    memcpy(r->u.text, s, n);
}
/* write one record: no syscall, one memcpy into the mapped ring. Fills abs, seq, tid. */
static inline void nvt_emit(nvt_rec *r, int ring)
{
    if (!nvt_on()) return;
    nvt_hdr *h = g_nvt.hdr;
    r->abs = nvt_now();
    r->tid = nvt_tid();
    uint32_t seq = __atomic_add_fetch(&h->seq_next, 1u, __ATOMIC_RELAXED);
    if (!seq) seq = __atomic_add_fetch(&h->seq_next, 1u, __ATOMIC_RELAXED);
    r->seq = 0;
    uint64_t *widx = ring == NVT_RING_VK ? &h->widx_vk : &h->widx_main;
    const uint64_t idx = __atomic_fetch_add(widx, 1u, __ATOMIC_RELAXED);
    nvt_rec *slot = &g_nvt.ring[ring == NVT_RING_VK][idx % g_nvt.nring[ring == NVT_RING_VK]];
    __atomic_store_n(&slot->seq, 0u, __ATOMIC_RELAXED);   /* a reader that lands here mid-write skips the slot */
    memcpy(slot, r, sizeof *slot);
    __atomic_store_n(&slot->seq, seq, __ATOMIC_RELEASE);
    r->seq = seq;
    nvt_count_record(r);
}

/* ---- content hash: the kernel's band hash (accel/rdna3-accel.cpp rd3AccelBandHash) ---- */

/* the rows of band 'band' a flicker hash samples (common/rdna3_a3_display.h rd3_flicker_band_rows) */
static inline unsigned nvt_band_rows(unsigned band, unsigned height, unsigned rows, unsigned *out)
{
    unsigned top, len, i, k = 0;
    if (band >= NVT_BANDS || !height || !rows || !out) return 0;
    if (band == 0) {
        top = 0;
        len = height < 32 ? height : 32;
    } else {
        const unsigned rest = height > 32 ? height - 32 : 0;
        top = 32 + (unsigned)(((uint64_t)rest * (band - 1)) / (NVT_BANDS - 1));
        len = 32 + (unsigned)(((uint64_t)rest * band) / (NVT_BANDS - 1)) - top;
    }
    for (i = 0; i < rows && len; i++) {
        const unsigned y = top + (unsigned)(((uint64_t)len * (2 * i + 1)) / (2 * rows));
        if (y < height) out[k++] = y;
    }
    return k;
}
/* 8 bands of 48 columns x 2 rows (4 in the top band) of a 4-byte-per-pixel picture, FNV-1a over the 24 colour bits.
 * The same value the kernel prints for the buffer in VRAM when the pictures are equal. Reads at most
 * h rows of 'pitch' bytes from 'base'. */
static inline void nvt_band_hash(const void *base, size_t pitch, unsigned w, unsigned h, uint32_t out[NVT_BANDS])
{
    const unsigned char *p = (const unsigned char *)base;
    for (unsigned b = 0; b < NVT_BANDS; b++) {
        unsigned rows[4];
        const unsigned nr = nvt_band_rows(b, h, b ? 2u : 4u, rows);
        uint32_t v = 2166136261u;
        for (unsigned i = 0; i < nr; i++)
            for (unsigned c = 0; c < NVT_COLS; c++) {
                const unsigned x = (unsigned)(((uint64_t)w * (2u * c + 1u)) / (2u * NVT_COLS));
                uint32_t px;
                memcpy(&px, p + (size_t)rows[i] * pitch + (size_t)x * 4u, sizeof px);
                v = (v ^ (px & 0xffffffu)) * 16777619u;
            }
        out[b] = v;
    }
}

/* ---- emitters for the pure C side (nvmtl_vk.c, allocation sites) ------------- */

static inline void nvt_alloc_fail(const char *what, int rc)
{
    if (!nvt_on()) return;
    nvt_rec r; nvt_rec_init(&r, NVT_K_ALLOC_FAIL);
    r.rc = rc; r.id = g_nvt_cb;
    nvt_set_text(&r, what);
    nvt_emit(&r, NVT_RING_MAIN);
}
static inline void nvt_vk_submit(uint32_t seq, const char *who, unsigned path, int rc, uint32_t dur_us, uint32_t ncb, uint32_t npre)
{
    if (!nvt_on()) return;
    nvt_rec r; nvt_rec_init(&r, NVT_K_VK_SUBMIT);
    r.path = (uint8_t)path; r.rc = rc; r.id = seq; r.dur_us = dur_us; r.a0 = ncb; r.a1 = npre;
    nvt_set_text(&r, who);
    nvt_emit(&r, NVT_RING_VK);
}
static inline void nvt_vk_wait(uint32_t seq, const char *who, unsigned path, int rc, uint32_t dur_us, uint32_t waited_ms, uint32_t budget_ms)
{
    if (!nvt_on()) return;
    nvt_rec r; nvt_rec_init(&r, NVT_K_VK_WAIT);
    r.path = (uint8_t)path; r.rc = rc; r.id = seq; r.dur_us = dur_us; r.a0 = waited_ms; r.a1 = budget_ms;
    nvt_set_text(&r, who);
    nvt_emit(&r, NVT_RING_VK);
}
static inline void nvt_vk_disable(uint32_t seq, const char *who, unsigned path, int rc)
{
    if (!nvt_on()) return;
    nvt_rec r; nvt_rec_init(&r, NVT_K_VK_DISABLE);
    r.path = (uint8_t)path; r.rc = rc; r.id = seq;
    nvt_set_text(&r, who);
    nvt_emit(&r, NVT_RING_MAIN);
}
static inline void nvt_kreq_dirty(uint32_t surf, uint32_t plane, unsigned path, int rc, int err, uint32_t dur_us)
{
    if (!nvt_on()) return;
    nvt_rec r; nvt_rec_init(&r, NVT_K_KREQ_DIRTY);
    r.surf = surf; r.plane = (uint8_t)plane; r.path = (uint8_t)path; r.rc = rc; r.a0 = (uint32_t)err; r.dur_us = dur_us; r.id = g_nvt_cb;
    nvt_emit(&r, NVT_RING_MAIN);
}
static inline void nvt_kreq_import(uint32_t surf, uint32_t plane, uint64_t bytes, unsigned path, int vkrc, int kerr, uint32_t dur_us)
{
    if (!nvt_on()) return;
    nvt_rec r; nvt_rec_init(&r, NVT_K_KREQ_IMPORT);
    r.surf = surf; r.plane = (uint8_t)plane; r.path = (uint8_t)path; r.rc = vkrc; r.a0 = (uint32_t)kerr; r.bytes = bytes; r.dur_us = dur_us;
    nvt_emit(&r, NVT_RING_MAIN);
}
static inline void nvt_kreq_probe(unsigned state, int rc, int err)
{
    if (!nvt_on()) return;
    nvt_rec r; nvt_rec_init(&r, NVT_K_KREQ_PROBE);
    r.path = (uint8_t)state; r.rc = rc; r.a0 = (uint32_t)err;
    nvt_emit(&r, NVT_RING_MAIN);
}

/* ---- emitters for the Objective-C side's events (also what the host test drives) ---- */

/* SURF_IN / SURF_OUT. t0: nvt_now() at the start of the operation (0: no duration). bands: the band hash of the CPU
 * memory (NULL: none). flags: NVT_F_VRAM, NVT_F_RO_LOCK, NVT_F_HASH_UNLOCKED. cb: the command buffer being published
 * for comes from nvt_set_cb. */
static inline void nvt_ev_surf(unsigned kind, unsigned path, uint32_t surf, unsigned plane, uint32_t w, uint32_t h, uint32_t fmt,
                               uint32_t bpr, unsigned flags, int rc, uint32_t seed0, uint32_t seed1, uint64_t t0, uint64_t bytes,
                               const uint32_t *bands)
{
    if (!nvt_on()) return;
    nvt_rec r; nvt_rec_init(&r, kind);
    r.path = (uint8_t)path; r.plane = (uint8_t)plane; r.flags = (uint8_t)flags; r.surf = surf; r.rc = rc;
    r.w = w; r.h = h; r.fmt = fmt; r.bpr = bpr; r.seed0 = seed0; r.seed1 = seed1; r.bytes = bytes; r.id = g_nvt_cb;
    r.dur_us = t0 ? nvt_us_since(t0) : 0u;
    if (bands) { memcpy(r.u.n.band, bands, sizeof r.u.n.band); r.flags = (uint8_t)(r.flags | NVT_F_HASHED); }
    nvt_emit(&r, NVT_RING_MAIN);
}
/* one colour attachment of a render pass that writes an IOSurface texture. load/store: MTLLoadAction / MTLStoreAction.
 * extra: bit0 depth attachment present, bit1 resolve texture present. clear: the attachment's clear colour. */
static inline void nvt_ev_pass_begin(uint32_t cb, uint32_t pass, unsigned att, unsigned natt, uint32_t surf, unsigned plane, uint32_t w,
                                     uint32_t h, uint32_t fmt, unsigned flags, uint32_t load, uint32_t store, uint32_t extra,
                                     const float clear[4])
{
    if (!nvt_on()) return;
    nvt_rec r; nvt_rec_init(&r, NVT_K_PASS_BEGIN);
    r.plane = (uint8_t)plane; r.flags = (uint8_t)flags; r.surf = surf; r.w = w; r.h = h; r.fmt = fmt;
    r.id = cb; r.a0 = pass; r.a1 = att; r.a2 = natt;
    r.u.n.a3 = load; r.u.n.a4 = store; r.u.n.a5 = extra;
    if (clear) memcpy(r.u.n.band, clear, 4 * sizeof(float));
    nvt_emit(&r, NVT_RING_MAIN);
}
/* the pass ended. sets: descriptor sets the pass added to the command buffer (about two per draw; ~0 for no draws).
 * clear: the colour a clear-only pass cleared to. t0: nvt_now() at its begin. */
static inline void nvt_ev_pass_end(uint32_t cb, uint32_t pass, unsigned path, int rc, uint32_t surf, uint32_t w, uint32_t h,
                                   uint32_t sets, const float clear[4], uint64_t t0)
{
    if (!nvt_on()) return;
    nvt_rec r; nvt_rec_init(&r, NVT_K_PASS_END);
    r.path = (uint8_t)path; r.rc = rc; r.surf = surf; r.w = w; r.h = h; r.id = cb; r.a0 = pass; r.a1 = sets;
    r.dur_us = t0 ? nvt_us_since(t0) : 0u;
    if (clear) memcpy(r.u.n.band, clear, 4 * sizeof(float));
    nvt_emit(&r, NVT_RING_MAIN);
}
static inline void nvt_ev_cb_commit(uint32_t cb, uint32_t nenc, uint32_t npresent, uint32_t ndirty)
{
    if (!nvt_on()) return;
    nvt_rec r; nvt_rec_init(&r, NVT_K_CB_COMMIT);
    r.id = cb; r.a0 = nenc; r.a1 = npresent; r.a2 = ndirty;
    nvt_emit(&r, NVT_RING_MAIN);
}
static inline void nvt_ev_cb_dirty(uint32_t cb, uint32_t surf, unsigned plane, uint32_t w, uint32_t h, uint32_t fmt, unsigned flags)
{
    if (!nvt_on()) return;
    nvt_rec r; nvt_rec_init(&r, NVT_K_CB_DIRTY);
    r.id = cb; r.surf = surf; r.plane = (uint8_t)plane; r.w = w; r.h = h; r.fmt = fmt; r.flags = (uint8_t)flags;
    nvt_emit(&r, NVT_RING_MAIN);
}
/* rc: the NSError code (0 = no error); status: MTLCommandBufferStatus; t_commit: nvt_now() at commit */
static inline void nvt_ev_cb_done(uint32_t cb, int rc, uint32_t status, uint64_t t_commit, uint32_t nparts)
{
    if (!nvt_on()) return;
    nvt_rec r; nvt_rec_init(&r, NVT_K_CB_DONE);
    r.id = cb; r.rc = rc; r.path = rc ? 2u : 1u; r.a0 = status; r.a1 = nparts;
    r.dur_us = t_commit ? nvt_us_since(t_commit) : 0u;
    nvt_emit(&r, NVT_RING_MAIN);
}
/* a surface got its texture backing. why: NVT_WHY_* when private. */
static inline void nvt_ev_map(uint32_t surf, unsigned plane, uint32_t w, uint32_t h, uint32_t fmt, uint32_t bpr, int vram, unsigned why,
                              int current)
{
    if (!nvt_on()) return;
    nvt_rec r; nvt_rec_init(&r, NVT_K_SURF_MAP);
    r.path = (uint8_t)(vram ? NVT_MAP_VRAM : NVT_MAP_PRIVATE); r.plane = (uint8_t)plane; r.surf = surf; r.w = w; r.h = h; r.fmt = fmt; r.bpr = bpr;
    r.flags = (uint8_t)((vram ? NVT_F_VRAM : 0u) | (current ? NVT_F_CURRENT : 0u)); r.a0 = why;
    nvt_emit(&r, NVT_RING_MAIN);
}

#endif /* NVMTL_TRACE_H */
