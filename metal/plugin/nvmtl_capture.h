/*
 * RDNA3 Metal plugin - draw capture (DRIVER-PLAN N55), the parts a host test can run.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * NVMTL_CAPTURE_DRAW=<shader>:<first>[:<count>[:<every>]][@<process>]   (off when unset; rdna3mtl.conf or the environment)
 *   <shader>   a substring of the vertex or the fragment function name (buffer_image_blit, fixed_frag_lpf_cpf, ...)
 *   <first>    the 1-based ordinal, among this process's draws whose pipeline matches, of the first draw to capture
 *   <count>    how many draws to capture (default 1, at most NVMTL_CAP_MAX_DRAWS)
 *   <every>    the stride between captured ordinals (default 1: consecutive matching draws)
 *   @<process> only in the process of that exact name (getprogname); without it every process that matches captures
 * Each captured draw becomes ONE file /var/tmp/rdna3-nvmtl/capture-<process>-<pid>-<k>.bin, written when its command buffer
 * completed: the pipeline's SPIR-V (as the translator gave it), the AIR and the function constants (JSON, the fc cache's own
 * key), the draw's arguments, the render target, scissor and viewport, the address table as the draw sees it, and for every
 * bound buffer its bytes three times: the CPU copy when the draw was encoded, the CPU copy when the GPU was done, and the
 * GPU's own copy (VRAM of a managed buffer, read back after the command buffer completed). tools/capture_decode.py unpacks
 * and judges it. The container is the format below; a record that would pass the per-draw byte cap is replaced by a DROP
 * record (name and size), never silently left out.
 *
 * File: "NVMTLCP1" u32 version (1) u32 0, then records: char tag[4], u32 name_len, u64 data_len, name, data (little endian).
 */
#ifndef NVMTL_CAPTURE_H
#define NVMTL_CAPTURE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#define NVMTL_CAP_MAX_DRAWS     32u
#define NVMTL_CAP_DRAW_BYTES    (48u << 20)    /* one draw's file */
#define NVMTL_CAP_PROC_BYTES    (160u << 20)   /* everything one process captures */
#define NVMTL_CAP_BUF_BYTES     (4u << 20)     /* one buffer binding, per snapshot */
#define NVMTL_CAP_VERSION       1u

typedef struct {
    char shader[96];
    char proc[64];
    uint32_t first, count, every;
} nvmtl_cap_spec;

/* 0 = parsed; -1 = malformed, `why` says what (the switch is then OFF, and the plugin logs it) */
static inline int nvmtl_cap_parse(const char *s, nvmtl_cap_spec *o, char *why, size_t whyn)
{
#define NVMTL_CAP_FAIL(m) do { if (why && whyn) snprintf(why, whyn, "%s", (m)); return -1; } while (0)
    if (!o) return -1;
    memset(o, 0, sizeof *o);
    if (!s || !*s) NVMTL_CAP_FAIL("empty");
    const char *at = strchr(s, '@');
    const size_t body = at ? (size_t)(at - s) : strlen(s);
    if (at) {
        const size_t pn = strlen(at + 1);
        if (!pn) NVMTL_CAP_FAIL("'@' without a process name");
        if (pn >= sizeof o->proc) NVMTL_CAP_FAIL("process name longer than 63 characters");
        if (strchr(at + 1, '@') || strchr(at + 1, ':')) NVMTL_CAP_FAIL("a second '@' or a ':' in the process name");
        memcpy(o->proc, at + 1, pn); o->proc[pn] = 0;
    }
    const char *c = memchr(s, ':', body);
    if (!c) NVMTL_CAP_FAIL("no ':<first>' after the shader name");
    const size_t sn = (size_t)(c - s);
    if (!sn) NVMTL_CAP_FAIL("empty shader name");
    if (sn >= sizeof o->shader) NVMTL_CAP_FAIL("shader name longer than 95 characters");
    memcpy(o->shader, s, sn); o->shader[sn] = 0;
    uint32_t v[3] = { 0, 1, 1 }; unsigned nv = 0;
    const char *p = c + 1, *end = s + body;
    while (p <= end && nv < 3) {
        if (p == end || *p == ':') NVMTL_CAP_FAIL("an empty number");
        unsigned long long x = 0; const char *q = p;
        while (q < end && *q >= '0' && *q <= '9') { x = x * 10 + (unsigned long long)(*q - '0'); if (x > 0xffffffffull) NVMTL_CAP_FAIL("a number over 2^32"); q++; }
        if (q < end && *q != ':') NVMTL_CAP_FAIL("not a decimal number");
        v[nv++] = (uint32_t)x;
        if (q == end) { p = end + 1; break; }
        p = q + 1;
        if (p > end) NVMTL_CAP_FAIL("a trailing ':'");
    }
    if (p <= end) NVMTL_CAP_FAIL("more than three numbers");
    if (!v[0]) NVMTL_CAP_FAIL("<first> must be 1 or more (ordinals start at 1)");
    if (!v[1] || v[1] > NVMTL_CAP_MAX_DRAWS) NVMTL_CAP_FAIL("<count> must be 1..32");
    if (!v[2]) NVMTL_CAP_FAIL("<every> must be 1 or more");
    o->first = v[0]; o->count = v[1]; o->every = v[2];
    return 0;
#undef NVMTL_CAP_FAIL
}

/* the capture index (0-based) the 1-based matching ordinal takes, or -1 */
static inline int nvmtl_cap_hit(const nvmtl_cap_spec *s, uint64_t ord)
{
    if (!s || !s->first || ord < s->first) return -1;
    const uint64_t d = ord - s->first;
    if (d % s->every) return -1;
    const uint64_t k = d / s->every;
    return k < s->count ? (int)k : -1;
}

static inline int nvmtl_cap_name_match(const nvmtl_cap_spec *s, const char *vname, const char *fname)
{
    if (!s || !s->shader[0]) return 0;
    return (vname && strstr(vname, s->shader)) || (fname && strstr(fname, s->shader));
}

static inline int nvmtl_cap_proc_match(const nvmtl_cap_spec *s, const char *prog)
{
    if (!s) return 0;
    if (!s->proc[0]) return 1;
    return prog && !strcmp(prog, s->proc);
}

/* the bytes of a binding to keep: the whole buffer when it is at most `cap` bytes (Core Animation binds sub-ranges of 256 KB
   pools and its offsets inside them matter), else `cap` bytes from the bound offset. 0 = nothing to take. */
static inline int nvmtl_cap_range(size_t len, size_t off, size_t cap, size_t *lo, size_t *hi)
{
    if (!lo || !hi || !len || !cap) return 0;
    if (len <= cap) { *lo = 0; *hi = len; return 1; }
    if (off >= len) return 0;
    *lo = off; *hi = len - off > cap ? off + cap : len;
    return 1;
}

typedef struct { uint8_t *p; size_t n, cap, limit; unsigned recs, dropped; int oom; } nvmtl_capbuf;

static inline int nvmtl_cap_grow(nvmtl_capbuf *b, size_t add)
{
    if (b->n + add <= b->cap) return 0;
    size_t nc = b->cap ? b->cap : 4096;
    while (nc < b->n + add) nc *= 2;
    uint8_t *q = (uint8_t *)realloc(b->p, nc);
    if (!q) { b->oom = 1; return -1; }
    b->p = q; b->cap = nc; return 0;
}
static inline void nvmtl_cap_raw(nvmtl_capbuf *b, const void *d, size_t n) { if (n) memcpy(b->p + b->n, d, n); b->n += n; }
static inline void nvmtl_cap_u32(uint8_t *o, uint32_t v) { for (int i = 0; i < 4; i++) o[i] = (uint8_t)(v >> (8 * i)); }
static inline void nvmtl_cap_u64(uint8_t *o, uint64_t v) { for (int i = 0; i < 8; i++) o[i] = (uint8_t)(v >> (8 * i)); }

static inline int nvmtl_cap_init(nvmtl_capbuf *b, size_t limit)
{
    memset(b, 0, sizeof *b); b->limit = limit;
    if (nvmtl_cap_grow(b, 16)) return -1;
    uint8_t h[16]; memcpy(h, "NVMTLCP1", 8); nvmtl_cap_u32(h + 8, NVMTL_CAP_VERSION); nvmtl_cap_u32(h + 12, 0);
    nvmtl_cap_raw(b, h, 16); return 0;
}
static inline void nvmtl_cap_free(nvmtl_capbuf *b) { free(b->p); memset(b, 0, sizeof *b); }

/* one record whose data is a || b (b may be NULL): a DROP record (data: the u64 size refused) when it would pass the limit */
static inline int nvmtl_cap_rec2(nvmtl_capbuf *b, const char tag[4], const char *name, const void *a, size_t an, const void *c, size_t cn)
{
    if (!b || !b->p) return -1;
    const size_t nl = name ? strlen(name) : 0, dn = an + cn;
    if (b->n + 16 + nl + dn > b->limit) {
        if (b->n + 16 + nl + 8 > b->limit || nvmtl_cap_grow(b, 16 + nl + 8)) { b->dropped++; return -1; }
        uint8_t h[16], sz[8]; memcpy(h, "DROP", 4); nvmtl_cap_u32(h + 4, (uint32_t)nl); nvmtl_cap_u64(h + 8, 8); nvmtl_cap_u64(sz, (uint64_t)dn);
        nvmtl_cap_raw(b, h, 16); nvmtl_cap_raw(b, name, nl); nvmtl_cap_raw(b, sz, 8); b->recs++; b->dropped++;
        return 1;
    }
    if (nvmtl_cap_grow(b, 16 + nl + dn)) { b->dropped++; return -1; }
    uint8_t h[16]; memcpy(h, tag, 4); nvmtl_cap_u32(h + 4, (uint32_t)nl); nvmtl_cap_u64(h + 8, (uint64_t)dn);
    nvmtl_cap_raw(b, h, 16); nvmtl_cap_raw(b, name, nl); nvmtl_cap_raw(b, a, an); nvmtl_cap_raw(b, c, cn); b->recs++;
    return 0;
}
static inline int nvmtl_cap_rec(nvmtl_capbuf *b, const char tag[4], const char *name, const void *d, size_t n) { return nvmtl_cap_rec2(b, tag, name, d, n, NULL, 0); }
static inline int nvmtl_cap_text(nvmtl_capbuf *b, const char tag[4], const char *name, const char *s) { return nvmtl_cap_rec(b, tag, name, s, s ? strlen(s) : 0); }
/* a buffer snapshot: the data starts with the u64 offset of the first byte inside the buffer */
static inline int nvmtl_cap_bytes(nvmtl_capbuf *b, const char *name, uint64_t lo, const void *d, size_t n)
{
    uint8_t o[8]; nvmtl_cap_u64(o, lo);
    return nvmtl_cap_rec2(b, "BUF ", name, o, 8, d, n);
}

/* the process name in a file name: [A-Za-z0-9._ -] kept, everything else '_', at most n-1 characters, "unknown" when empty
   (as nvmtl_trace.h names the traces; a long name cannot push the pid and index out of the path) */
static inline void nvmtl_cap_safe_name(const char *pn, char *out, size_t n)
{
    size_t k = 0;
    if (!out || !n) return;
    for (; pn && pn[k] && k + 1 < n && k < 48; k++) {
        const char c = pn[k];
        out[k] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_') ? c : '_';
    }
    out[k] = 0;
    if (!k) snprintf(out, n, "unknown");
}

/* the shared capture directory, opened once per write without following a symlink (nvmtl_trace.h nvt_open_dir): created
   1777 like the trace directory only when this call made it (fchmod through the fd); its owner must be root, ourselves or
   _windowserver (88) - a root process takes only root's. -1 with errno set otherwise. */
static inline int nvmtl_cap_open_dir(const char *dir)
{
    int created = 0;
    if (mkdir(dir, 0777) == 0) created = 1;
    else if (errno != EEXIST) return -1;
    const int fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) { close(fd); errno = ENOTDIR; return -1; }
    const uid_t me = geteuid();
    if (me == 0 ? st.st_uid != 0 : (st.st_uid != 0 && st.st_uid != me && st.st_uid != 88)) { close(fd); errno = EPERM; return -1; }
    if (created) (void)fchmod(fd, 01777);
    return fd;
}

/* create `file` in `dfd` exclusively, never through a symlink; a stale file of ours (regular, our uid, one link: the same pid
   after a reboot, /var/tmp persists) is replaced once, anything else is refused (EEXIST). Mode 0640. */
static inline int nvmtl_cap_open_file(int dfd, const char *file)
{
    for (int again = 0; again < 2; again++) {
        const int fd = openat(dfd, file, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0640);
        if (fd >= 0) { (void)fchmod(fd, 0640); return fd; }
        if (errno != EEXIST) return -1;
        struct stat st;
        if (again || fstatat(dfd, file, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1
            || unlinkat(dfd, file, 0) != 0) { errno = EEXIST; return -1; }
    }
    errno = EEXIST;
    return -1;
}

/* all of it, EINTR retried; 0 = written, else -1 with the errno of the failing write (EIO for a zero-length write) */
static inline int nvmtl_cap_write_all(int fd, const uint8_t *p, size_t n, size_t *done)
{
    size_t d = 0;
    while (d < n) {
        const ssize_t w = write(fd, p + d, n - d);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { if (w == 0) errno = EIO; if (done) *done = d; return -1; }
        d += (size_t)w;
    }
    if (done) *done = d;
    return 0;
}
#endif
