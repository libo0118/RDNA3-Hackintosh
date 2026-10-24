/*
 * cfstr_peek: read CFSTRs and static byte flags of the CoreDisplay image that
 * is loaded in THIS process, by their unslid addresses (the ones dyld_info
 * prints for the dyld shared cache), and the matching CFPreferences values.
 * Read-only: it writes nothing but stdout.
 *
 *   cc -O0 -o cfstr_peek cfstr_peek.c -framework CoreFoundation
 *   cfstr_peek s:0x7ff8405067b8 d:0x7ff8404fd578 b:0x7ff843bac76d ...
 *
 * Modes (hex, unslid address in the CoreDisplay image):
 *   s:ADDR  a CFStringRef (a CFSTR constant): print its UTF-8 text
 *   d:ADDR  like s:, and the string is the preferences domain for the
 *           CFPreferencesCopyValue pass below
 *   b:ADDR  one byte (a static bool flag)
 *   w:ADDR  one 32-bit little-endian word (e.g. the gpuVendors mask)
 *
 * Every address is checked against the LC_SEGMENT_64 ranges of the CoreDisplay
 * image (plus the slide) before it is dereferenced; a SIGSEGV / SIGBUS inside
 * a dereference is caught and reported, not fatal.
 *
 * Addresses of one macOS build only (24H32); the build (kern.osversion) is
 * printed first and a different one is flagged.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define EXPECTED_BUILD "24H32"
#define MAX_SEGS 32
#define CFSTR_STRUCT_SIZE 32 /* isa, flags, str*, length */

static const char *kLibPaths[] = {
    "/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay",
    "/System/Library/Frameworks/CoreDisplay.framework/Versions/A/CoreDisplay",
    NULL,
};

struct seg {
    char name[17];
    uintptr_t lo; /* vmaddr + slide */
    uintptr_t hi;
};

static struct seg g_segs[MAX_SEGS];
static int g_nsegs;
static intptr_t g_slide;

static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_guarded;

static void on_fault(int sig)
{
    if (g_guarded) {
        g_guarded = 0;
        siglongjmp(g_jmp, sig);
    }
    _exit(128 + sig);
}

static void install_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_fault;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

static int in_image(uintptr_t p, size_t len)
{
    int i;
    for (i = 0; i < g_nsegs; i++) {
        if (p >= g_segs[i].lo && p < g_segs[i].hi && len <= g_segs[i].hi - p)
            return 1;
    }
    return 0;
}

/* The CoreDisplay image: index, slide, segment ranges. Returns 0 on success. */
static int find_image(void)
{
    uint32_t n = _dyld_image_count(), i;
    for (i = 0; i < n; i++) {
        const char *nm = _dyld_get_image_name(i);
        const struct mach_header_64 *mh;
        const struct load_command *lc;
        uint32_t k;
        if (!nm || !strstr(nm, "CoreDisplay.framework"))
            continue;
        mh = (const struct mach_header_64 *)_dyld_get_image_header(i);
        if (!mh || mh->magic != MH_MAGIC_64)
            continue;
        g_slide = _dyld_get_image_vmaddr_slide(i);
        printf("image: [%u] %s\n", i, nm);
        printf("slide: 0x%lx\n", (unsigned long)g_slide);
        lc = (const struct load_command *)(mh + 1);
        for (k = 0; k < mh->ncmds; k++) {
            if (lc->cmd == LC_SEGMENT_64 && g_nsegs < MAX_SEGS) {
                const struct segment_command_64 *sc = (const struct segment_command_64 *)lc;
                struct seg *s = &g_segs[g_nsegs++];
                memcpy(s->name, sc->segname, 16);
                s->name[16] = 0;
                s->lo = (uintptr_t)(sc->vmaddr + (uint64_t)g_slide);
                s->hi = s->lo + (uintptr_t)sc->vmsize;
                /* __PAGEZERO is not mapped */
                if (sc->vmaddr == 0 || sc->initprot == 0)
                    g_nsegs--;
                else
                    printf("segment %-16s 0x%lx..0x%lx\n", s->name, (unsigned long)s->lo,
                           (unsigned long)s->hi);
            }
            lc = (const struct load_command *)((const char *)lc + lc->cmdsize);
        }
        return g_nsegs ? 0 : -1;
    }
    return -1;
}

/* Resolve a CFSTR at the unslid address; returns the CFStringRef if it is a string. */
static CFStringRef peek_string(unsigned long long addr)
{
    uintptr_t p = (uintptr_t)(addr + (unsigned long long)g_slide);
    char buf[256];
    int ok = 0, isstr = 0;
    volatile unsigned long long w[4] = {0, 0, 0, 0};
    int sig;

    if (!in_image(p, CFSTR_STRUCT_SIZE)) {
        printf("s:0x%llx -> outside CoreDisplay\n", addr);
        return NULL;
    }
    buf[0] = 0;
    g_guarded = 1;
    sig = sigsetjmp(g_jmp, 1);
    if (sig == 0) {
        const unsigned long long *raw = (const unsigned long long *)p;
        CFStringRef s = (CFStringRef)p;
        w[0] = raw[0];
        w[1] = raw[1];
        w[2] = raw[2];
        w[3] = raw[3];
        if (CFGetTypeID(s) == CFStringGetTypeID()) {
            isstr = 1;
            ok = CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8);
        }
        g_guarded = 0;
    } else {
        printf("s:0x%llx -> fault (signal %d) while reading\n", addr, sig);
        return NULL;
    }
    if (!isstr) {
        printf("s:0x%llx -> not a string (raw words %016llx %016llx %016llx %016llx)\n", addr,
               w[0], w[1], w[2], w[3]);
        return NULL;
    }
    if (!ok) {
        printf("s:0x%llx -> a string that does not fit 256 UTF-8 bytes\n", addr);
        return (CFStringRef)p;
    }
    printf("s:0x%llx -> \"%s\"\n", addr, buf);
    return (CFStringRef)p;
}

static void peek_byte(unsigned long long addr)
{
    uintptr_t p = (uintptr_t)(addr + (unsigned long long)g_slide);
    volatile unsigned char v = 0;
    int sig;

    if (!in_image(p, 1)) {
        printf("b:0x%llx -> outside CoreDisplay\n", addr);
        return;
    }
    g_guarded = 1;
    sig = sigsetjmp(g_jmp, 1);
    if (sig == 0) {
        v = *(const volatile unsigned char *)p;
        g_guarded = 0;
    } else {
        printf("b:0x%llx -> fault (signal %d) while reading\n", addr, sig);
        return;
    }
    printf("b:0x%llx -> 0x%02x (%u)\n", addr, (unsigned)v, (unsigned)v);
}

static void peek_word(unsigned long long addr)
{
    uintptr_t p = (uintptr_t)(addr + (unsigned long long)g_slide);
    volatile uint32_t v = 0;
    int sig;

    if (!in_image(p, 4)) {
        printf("w:0x%llx -> outside CoreDisplay\n", addr);
        return;
    }
    g_guarded = 1;
    sig = sigsetjmp(g_jmp, 1);
    if (sig == 0) {
        memcpy((void *)&v, (const void *)p, 4);
        g_guarded = 0;
    } else {
        printf("w:0x%llx -> fault (signal %d) while reading\n", addr, sig);
        return;
    }
    printf("w:0x%llx -> 0x%08x (%u)\n", addr, (unsigned)v, (unsigned)v);
}

static void print_value(CFTypeRef v)
{
    char buf[512];
    CFStringRef d = CFCopyDescription(v);
    if (d && CFStringGetCString(d, buf, sizeof buf, kCFStringEncodingUTF8))
        printf("%s", buf);
    else
        printf("(value does not print)");
    if (d)
        CFRelease(d);
}

static void prefs_pass(CFStringRef domain)
{
    static const char *keys[] = {"useIOP", "useIOP_onIG", "useIOP_onAMD", "useIOP_onNVIDIA",
                                 "useIOP_onAPV", NULL};
    CFStringRef users[2] = {kCFPreferencesAnyUser, kCFPreferencesCurrentUser};
    const char *uname[2] = {"AnyUser", "CurrentUser"};
    CFStringRef hosts[2] = {kCFPreferencesAnyHost, kCFPreferencesCurrentHost};
    const char *hname[2] = {"AnyHost", "CurrentHost"};
    int u, h, k;

    for (u = 0; u < 2; u++) {
        for (h = 0; h < 2; h++) {
            for (k = 0; keys[k]; k++) {
                CFStringRef key = CFStringCreateWithCString(NULL, keys[k], kCFStringEncodingUTF8);
                CFPropertyListRef v = key ? CFPreferencesCopyValue(key, domain, users[u], hosts[h]) : NULL;
                printf("pref %-11s %-11s %-16s = ", uname[u], hname[h], keys[k]);
                if (v) {
                    print_value(v);
                    CFRelease(v);
                } else {
                    printf("unset");
                }
                printf("\n");
                if (key)
                    CFRelease(key);
            }
        }
    }
}

int main(int argc, char **argv)
{
    char build[64] = "";
    size_t len = sizeof build;
    CFStringRef domain = NULL;
    void *h = NULL;
    int i;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (sysctlbyname("kern.osversion", build, &len, NULL, 0) != 0)
        strcpy(build, "unknown");
    printf("macOS build: %s\n", build);
    if (strcmp(build, EXPECTED_BUILD) != 0)
        printf("WARNING: the addresses are for build %s only, this is %s\n", EXPECTED_BUILD, build);

    for (i = 0; kLibPaths[i] && !h; i++) {
        h = dlopen(kLibPaths[i], RTLD_LAZY);
        printf("dlopen %s: %s\n", kLibPaths[i], h ? "ok" : dlerror());
    }
    if (!h)
        printf("dlopen failed everywhere, looking for an already loaded CoreDisplay\n");
    if (find_image() != 0) {
        printf("CoreDisplay image not found in this process\n");
        return 1;
    }
    install_handlers();

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        unsigned long long addr;
        char *end = NULL;
        if (a[0] && a[1] == ':' && (a[0] == 's' || a[0] == 'd' || a[0] == 'b' || a[0] == 'w')) {
            addr = strtoull(a + 2, &end, 16);
            if (end == a + 2 || *end) {
                printf("%s -> not a hex address\n", a);
                continue;
            }
            if (a[0] == 'b') {
                peek_byte(addr);
            } else if (a[0] == 'w') {
                peek_word(addr);
            } else {
                CFStringRef s = peek_string(addr);
                if (a[0] == 'd' && s)
                    domain = s;
            }
        } else {
            printf("%s -> unknown mode (use s:ADDR, d:ADDR, b:ADDR or w:ADDR)\n", a);
        }
    }

    if (domain) {
        char buf[256];
        if (CFStringGetCString(domain, buf, sizeof buf, kCFStringEncodingUTF8))
            printf("preferences domain: \"%s\"\n", buf);
        prefs_pass(domain);
    } else {
        printf("preferences: no domain string resolved (pass d:ADDR), skipped\n");
    }
    return 0;
}
