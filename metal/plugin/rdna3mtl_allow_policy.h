/*
 * rdna3mtl_allow_policy.h - which processes get the RDNA3 Metal device.
 *
 * Plain C with no Objective-C and no system calls, so tests/test_mtl_allow.c
 * checks it on the host exactly as the plugin uses it. The caller
 * (nvmtl_process_is_allowed in NVMTLDevice.m) reads the sysctls and the
 * executable's path; this file only decides.
 *
 * The list (/Library/GPUBundles/rdna3mtl-allow.txt), one entry per line:
 *   name        the process with that name (getprogname())
 *   *           every process
 *   -name       never this process (anywhere in the file, whatever else matches)
 *   !name       only once the accelerator is armed (debug.rdna3accelfb != 0)
 *   @name       only while RDNA3AGDC's object is up (debug.rdna3agdc == 1):
 *               WindowServer, card runs 41 and 45
 *   %apps       apps mode: every application, i.e. a process whose executable
 *               is the main executable of an .app bundle in /Applications,
 *               /System/Applications or ~/Applications, but only in the
 *               configuration card runs 48-56 ran the apps in (armed, AGDC
 *               display pipe up: WindowServer composites on the plugin)
 * Without a "%apps" line the list behaves as it did before apps mode existed.
 * The boot-arg rdna3mtlapps=0 ignores the "%apps" line (the list is
 * unreadable without the desktop, the boot-arg is not).
 */
#ifndef RDNA3MTL_ALLOW_POLICY_H
#define RDNA3MTL_ALLOW_POLICY_H

#include <stdio.h>
#include <string.h>
#include <stddef.h>

typedef struct {
    const char *name;   /* getprogname() */
    const char *path;   /* the executable's absolute path; NULL when unknown */
    int armed;          /* debug.rdna3accelfb != 0 */
    int pipe_up;        /* debug.rdna3agdc == 1 */
    int apps_off;       /* boot-arg rdna3mtlapps=0 */
} rdna3mtl_allow_ctx;

/* how the list was read, for the log */
typedef enum {
    RDNA3MTL_MODE_LIST = 0,       /* no "%apps" line: the explicit list only */
    RDNA3MTL_MODE_APPS = 1,       /* "%apps" line, in force */
    RDNA3MTL_MODE_APPS_OFF = 2,   /* "%apps" line, switched off by the boot-arg */
} rdna3mtl_allow_mode;

static inline const char *rdna3mtl_allow_mode_name(rdna3mtl_allow_mode m)
{
    return m == RDNA3MTL_MODE_APPS ? "apps (every application, minus '-' lines)"
         : m == RDNA3MTL_MODE_APPS_OFF ? "list (the %apps line is switched off by boot-arg rdna3mtlapps=0)"
         : "list (explicit names only)";
}

/* path is the main executable of an application bundle that a user installed
 * or Apple ships as an app:
 *   /Applications/.../Name.app/Contents/MacOS/exe
 *   /System/Applications/.../Name.app/Contents/MacOS/exe
 *   /Users/<user>/Applications/.../Name.app/Contents/MacOS/exe
 * Not matched on purpose: the agents under /System/Library/CoreServices (Dock,
 * Finder, SystemUIServer, loginwindow: they run all session long and each
 * would hold a GPU VM), XPC services and helpers inside bundles (Safari's
 * WebKit.GPU, Chrome's GPU helper), and command-line tools. A helper that
 * should get the device is named in the list instead. */
static inline int rdna3mtl_is_user_app(const char *path)
{
    static const char tail[] = "Contents/MacOS/";
    const char *p, *app, *rest;
    if (!path) return 0;
    if (!strncmp(path, "/Applications/", 14)) p = path + 14;
    else if (!strncmp(path, "/System/Applications/", 21)) p = path + 21;
    else if (!strncmp(path, "/Users/", 7)) {
        p = strchr(path + 7, '/');
        if (!p || p == path + 7 || strncmp(p, "/Applications/", 14)) return 0;
        p += 14;
    } else return 0;
    app = strstr(p, ".app/");
    if (!app || app == p || app[-1] == '/') return 0;
    rest = app + 5;
    if (strncmp(rest, tail, sizeof tail - 1)) return 0;
    rest += sizeof tail - 1;
    return *rest && !strchr(rest, '/');
}

/* args is the kernel's boot-args string; true when it holds key=val as a
 * whole space-separated word */
static inline int rdna3mtl_bootarg_is(const char *args, const char *key, const char *val)
{
    size_t kl = strlen(key), vl = strlen(val);
    const char *p = args;
    while (p && *p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, key, kl) && p[kl] == '=' && !strncmp(p + kl + 1, val, vl)) {
            char e = p[kl + 1 + vl];
            if (!e || e == ' ' || e == '\t' || e == '\n') return 1;
        }
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return 0;
}

/* one trimmed entry of the list, or NULL for a blank line and a comment */
static inline char *rdna3mtl_allow_entry(char *line)
{
    char *nl = strpbrk(line, "\r\n"), *p = line, *e;
    if (nl) *nl = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '#') return NULL;
    e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    return p;
}

/* Decide for one process. f is the open list (rewound here). Returns 1 for a
 * device, 0 for none; why gets the reason for the log, *mode how the list
 * reads. The first entry that matches the process decides, except that a '-'
 * entry for it anywhere wins; a "%apps" entry that does not apply lets the
 * later entries decide. */
static inline int rdna3mtl_allow_decide(FILE *f, const rdna3mtl_allow_ctx *c,
                                        char *why, size_t whylen, rdna3mtl_allow_mode *mode)
{
    char line[256], held[200] = "";
    int apps_line = 0;
    *mode = RDNA3MTL_MODE_LIST;
    if (!c->name || !*c->name) { snprintf(why, whylen, "the process has no name"); return 0; }

    rewind(f);
    while (fgets(line, sizeof line, f)) {
        char *p = rdna3mtl_allow_entry(line);
        if (!p) continue;
        if (!strcmp(p, "%apps")) apps_line = 1;
        if (*p != '-') continue;
        p++; while (*p == ' ' || *p == '\t') p++;
        if (!strcmp(p, c->name)) { snprintf(why, whylen, "denied by the list ('-%s')", c->name); return 0; }
    }
    if (apps_line) *mode = c->apps_off ? RDNA3MTL_MODE_APPS_OFF : RDNA3MTL_MODE_APPS;

    rewind(f);
    while (fgets(line, sizeof line, f)) {
        char *p = rdna3mtl_allow_entry(line);
        int armed_only = 0, pipe_only = 0;
        if (!p || *p == '-') continue;
        if (!strcmp(p, "%apps")) {
            if (c->apps_off)
                snprintf(held, sizeof held, "boot-arg rdna3mtlapps=0 switches the apps mode off");
            else if (!rdna3mtl_is_user_app(c->path))
                snprintf(held, sizeof held, "not the main executable of an application in /Applications (%s)",
                         c->path && *c->path ? c->path : "path unknown");
            else if (!c->armed)
                snprintf(held, sizeof held, "apps mode needs debug.rdna3accelfb armed (reads 0)");
            else if (!c->pipe_up)
                snprintf(held, sizeof held, "apps mode needs the AGDC display pipe (debug.rdna3agdc is not 1)");
            else {
                snprintf(why, whylen, "apps mode: %s is an application (%s)", c->name, c->path);
                return 1;
            }
            continue;
        }
        if (*p == '!') { armed_only = 1; p++; while (*p == ' ' || *p == '\t') p++; }
        else if (*p == '@') { pipe_only = 1; p++; while (*p == ' ' || *p == '\t') p++; }
        if (strcmp(p, "*") && strcmp(p, c->name)) continue;
        if (armed_only && !c->armed) {
            snprintf(why, whylen, "'%s' is armed-only and debug.rdna3accelfb reads 0 (arm it with sysctl -w debug.rdna3accelfb=1)", c->name);
            return 0;
        }
        if (pipe_only && !c->pipe_up) {
            snprintf(why, whylen, "'%s' only with the AGDC display pipe and debug.rdna3agdc is not 1", c->name);
            return 0;
        }
        snprintf(why, whylen, "allow-listed: %s", c->name);
        return 1;
    }
    if (held[0]) snprintf(why, whylen, "not allow-listed (%s)", held);
    else snprintf(why, whylen, "not allow-listed");
    return 0;
}

#endif
