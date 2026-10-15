/* Which processes get the Metal plugin's device: the allow list policy of
 * metal/plugin/rdna3mtl_allow_policy.h (apps mode, '-' / '!' / '@' lines, the
 * boot-arg kill switch). SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include "rdna3mtl_allow_policy.h"

static int failures;
#define HOME "/Users/kevin"
#define APP(p) rdna3mtl_is_user_app((p), HOME)
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static const char *DEFAULT_LIST =
    "# comment\n"
    "mtlprobe\n"
    "nvmtltest\n"
    "vulkaninfo\n"
    "@WindowServer\n"
    "-Safari\n"
    "%apps\n";

static int decide(const char *list, const char *name, const char *path, int armed, int pipe_up,
                  int apps_off, rdna3mtl_allow_mode *mode, char *why, size_t n)
{
    FILE *f = tmpfile();
    rdna3mtl_allow_ctx c = { name, path, HOME, armed, pipe_up, apps_off };
    int r;
    if (!f) { perror("tmpfile"); failures++; return -1; }
    fputs(list, f);
    r = rdna3mtl_allow_decide(f, &c, why, n, mode);
    fclose(f);
    return r;
}

static int ok(const char *list, const char *name, const char *path, int armed, int pipe_up, int apps_off)
{
    rdna3mtl_allow_mode m; char why[300];
    return decide(list, name, path, armed, pipe_up, apps_off, &m, why, sizeof why);
}

static void test_is_user_app(void)
{
    CHECK(APP("/System/Applications/Chess.app/Contents/MacOS/Chess"));
    CHECK(APP("/System/Applications/Maps.app/Contents/MacOS/Maps"));
    CHECK(APP("/System/Applications/System Settings.app/Contents/MacOS/System Settings"));
    CHECK(APP("/Applications/Utilities/Terminal.app/Contents/MacOS/Terminal"));
    CHECK(APP("/Applications/Foo.app/Contents/MacOS/Foo"));
    CHECK(APP("/Users/kevin/Applications/Foo.app/Contents/MacOS/Foo"));
    /* the session's agents, helpers and tools are not applications */
    CHECK(!APP("/System/Library/CoreServices/Dock.app/Contents/MacOS/Dock"));
    CHECK(!APP("/System/Library/CoreServices/Finder.app/Contents/MacOS/Finder"));
    CHECK(!APP("/System/Library/CoreServices/SystemUIServer.app/Contents/MacOS/SystemUIServer"));
    CHECK(!APP("/System/Library/Frameworks/WebKit.framework/Versions/A/XPCServices/"
                                "com.apple.WebKit.GPU.xpc/Contents/MacOS/com.apple.WebKit.GPU"));
    CHECK(!APP("/Applications/Google Chrome.app/Contents/Frameworks/Google Chrome Framework.framework/"
                                "Helpers/Google Chrome Helper (GPU).app/Contents/MacOS/Google Chrome Helper (GPU)"));
    CHECK(!APP("/Applications/Foo.app/Contents/XPCServices/Bar.xpc/Contents/MacOS/Bar"));
    CHECK(!APP("/Applications/Foo.app/Contents/Resources/tool"));
    CHECK(!APP("/Applications/Foo.app/Contents/MacOS/"));
    CHECK(!APP("/Applications/Foo.app/Contents/MacOS/sub/Foo"));
    CHECK(!APP("/Applications/Foo.app.bak/Contents/MacOS/Foo"));
    CHECK(!APP("/Applications/.app/Contents/MacOS/Foo"));
    CHECK(!APP("/Applications/tool"));
    CHECK(!APP("/usr/bin/vulkaninfo"));
    CHECK(!APP("/tmp/Foo.app/Contents/MacOS/Foo"));
    CHECK(!APP("/Users//Applications/Foo.app/Contents/MacOS/Foo"));
    /* only the running user's own Applications folder */
    CHECK(!APP("/Users/other/Applications/Foo.app/Contents/MacOS/Foo"));
    CHECK(!APP("/Users/Shared/Applications/Foo.app/Contents/MacOS/Foo"));
    CHECK(!APP("/Users/kevinx/Applications/Foo.app/Contents/MacOS/Foo"));
    CHECK(!APP("/Users/kevin/Applications"));
    CHECK(!rdna3mtl_is_user_app("/Users/kevin/Applications/Foo.app/Contents/MacOS/Foo", NULL));
    CHECK(!rdna3mtl_is_user_app("/Users/kevin/Applications/Foo.app/Contents/MacOS/Foo", "/Users/other"));
    /* homes outside /Users, nested homes and /Users/Shared are never trusted */
    CHECK(!rdna3mtl_is_user_app("/var/root/Applications/Foo.app/Contents/MacOS/Foo", "/var/root"));
    CHECK(!rdna3mtl_is_user_app("/Volumes/Data/kevin/Applications/Foo.app/Contents/MacOS/Foo", "/Volumes/Data/kevin"));
    CHECK(!rdna3mtl_is_user_app("/Users/kevin/x/Applications/Foo.app/Contents/MacOS/Foo", "/Users/kevin/x"));
    CHECK(!rdna3mtl_is_user_app("/Users/Shared/Applications/Foo.app/Contents/MacOS/Foo", "/Users/Shared"));
    CHECK(rdna3mtl_is_user_app("/Users/other/Applications/Foo.app/Contents/MacOS/Foo", "/Users/other"));
    /* translocated apps run from a random mount */
    CHECK(!APP("/private/var/folders/ab/cd/AppTranslocation/1234/d/Foo.app/Contents/MacOS/Foo"));
    /* an app nested in another app is refused (the first .app/ decides) */
    CHECK(!APP("/Applications/Xcode.app/Contents/Developer/Applications/Simulator.app/Contents/MacOS/Simulator"));
    CHECK(!APP("/Applications/Foo.app/Contents/Applications/Bar.app/Contents/MacOS/Bar"));
    /* any file directly in Contents/MacOS counts: Info.plist is not read */
    CHECK(APP("/Applications/Foo.app/Contents/MacOS/Foo-helper"));
    CHECK(!APP("/Users/kevin/Documents/Foo.app/Contents/MacOS/Foo"));
    CHECK(!APP("/Users/kevin"));
    CHECK(!APP(""));
    CHECK(!APP(NULL));
}

static void test_bootarg(void)
{
    CHECK(rdna3mtl_bootarg_is("rdna3mtlapps=0", "rdna3mtlapps", "0"));
    CHECK(rdna3mtl_bootarg_is("-v keepsyms=1 rdna3mtlapps=0 amfi=0x80", "rdna3mtlapps", "0"));
    CHECK(rdna3mtl_bootarg_is("amfi=0x80  rdna3mtlapps=0\n", "rdna3mtlapps", "0"));
    CHECK(!rdna3mtl_bootarg_is("rdna3mtlapps=1", "rdna3mtlapps", "0"));
    CHECK(!rdna3mtl_bootarg_is("rdna3mtlapps=01", "rdna3mtlapps", "0"));
    CHECK(!rdna3mtl_bootarg_is("xrdna3mtlapps=0", "rdna3mtlapps", "0"));
    CHECK(!rdna3mtl_bootarg_is("rdna3mtlapps", "rdna3mtlapps", "0"));
    CHECK(!rdna3mtl_bootarg_is("", "rdna3mtlapps", "0"));
    CHECK(!rdna3mtl_bootarg_is(NULL, "rdna3mtlapps", "0"));
}

static void test_apps_mode(void)
{
    const char *chess = "/System/Applications/Chess.app/Contents/MacOS/Chess";
    rdna3mtl_allow_mode m; char why[300];

    /* card runs 48-56: armed, AGDC display pipe up */
    CHECK(decide(DEFAULT_LIST, "Chess", chess, 1, 1, 0, &m, why, sizeof why) == 1);
    CHECK(m == RDNA3MTL_MODE_APPS && strstr(why, "apps mode"));
    /* an app with no pipe or not armed is refused, and says why */
    CHECK(decide(DEFAULT_LIST, "Chess", chess, 1, 0, 0, &m, why, sizeof why) == 0);
    CHECK(strstr(why, "AGDC display pipe"));
    CHECK(decide(DEFAULT_LIST, "Chess", chess, 0, 1, 0, &m, why, sizeof why) == 0);
    CHECK(strstr(why, "armed"));
    /* the session's agents and helpers never get it from apps mode */
    CHECK(decide(DEFAULT_LIST, "Dock", "/System/Library/CoreServices/Dock.app/Contents/MacOS/Dock", 1, 1, 0,
                 &m, why, sizeof why) == 0);
    CHECK(strstr(why, "Contents/MacOS"));
    CHECK(!ok(DEFAULT_LIST, "com.apple.WebKit.GPU",
              "/System/Library/Frameworks/WebKit.framework/Versions/A/XPCServices/com.apple.WebKit.GPU.xpc/Contents/MacOS/com.apple.WebKit.GPU",
              1, 1, 0));
    CHECK(!ok(DEFAULT_LIST, "Chess", NULL, 1, 1, 0));
    /* the deny list wins, wherever it stands in the file */
    CHECK(!ok(DEFAULT_LIST, "Safari", "/Applications/Safari.app/Contents/MacOS/Safari", 1, 1, 0));
    CHECK(!ok("%apps\n-Safari\n", "Safari", "/Applications/Safari.app/Contents/MacOS/Safari", 1, 1, 0));
    CHECK(!ok("Safari\n-Safari\n", "Safari", "/Applications/Safari.app/Contents/MacOS/Safari", 1, 1, 0));
    CHECK(!ok("  -  Safari  \n%apps\n", "Safari", "/Applications/Safari.app/Contents/MacOS/Safari", 1, 1, 0));
    CHECK(ok("-Safari\n%apps\n", "Preview", "/System/Applications/Preview.app/Contents/MacOS/Preview", 1, 1, 0));
    /* names with spaces, trailing blanks and CRLF line ends */
    CHECK(ok("-Other\r\n%apps  \r\n", "System Settings",
             "/System/Applications/System Settings.app/Contents/MacOS/System Settings", 1, 1, 0));
    CHECK(!ok("-System Settings  \r\n%apps\r\n", "System Settings",
              "/System/Applications/System Settings.app/Contents/MacOS/System Settings", 1, 1, 0));
}

static void test_kill_switch(void)
{
    const char *chess = "/System/Applications/Chess.app/Contents/MacOS/Chess";
    rdna3mtl_allow_mode m; char why[300];
    /* the boot-arg ends apps mode and leaves the explicit names alone */
    CHECK(decide(DEFAULT_LIST, "Chess", chess, 1, 1, 1, &m, why, sizeof why) == 0);
    CHECK(m == RDNA3MTL_MODE_APPS_OFF && strstr(why, "rdna3mtlapps=0"));
    CHECK(ok(DEFAULT_LIST, "mtlprobe", "/usr/local/bin/mtlprobe", 1, 1, 1));
    CHECK(ok(DEFAULT_LIST, "WindowServer", "/System/Library/PrivateFrameworks/SkyLight.framework/Resources/WindowServer", 1, 1, 1));
    /* without the line, the same list is the old allow list */
    CHECK(decide("mtlprobe\n@WindowServer\n", "Chess", chess, 1, 1, 0, &m, why, sizeof why) == 0);
    CHECK(m == RDNA3MTL_MODE_LIST);
    CHECK(!ok("", "Chess", chess, 1, 1, 0));
    CHECK(!ok("# only a comment\n\n", "Chess", chess, 1, 1, 0));
}

static void test_old_lines(void)
{
    const char *ws = "/System/Library/PrivateFrameworks/SkyLight.framework/Resources/WindowServer";
    rdna3mtl_allow_mode m; char why[300];
    /* WindowServer: only with the display pipe up, exactly as before apps mode */
    CHECK(ok(DEFAULT_LIST, "WindowServer", ws, 0, 1, 0));
    CHECK(ok(DEFAULT_LIST, "WindowServer", ws, 1, 1, 0));
    CHECK(decide(DEFAULT_LIST, "WindowServer", ws, 1, 0, 0, &m, why, sizeof why) == 0);
    CHECK(strstr(why, "AGDC display pipe"));
    CHECK(decide(DEFAULT_LIST, "WindowServer", ws, 1, 1, 0, &m, why, sizeof why) == 1);
    CHECK(strstr(why, "allow-listed: WindowServer"));
    /* '!name' needs the armed accelerator, '*' is everybody */
    CHECK(!ok("!Foo\n", "Foo", NULL, 0, 1, 0));
    CHECK(ok("!Foo\n", "Foo", NULL, 1, 0, 0));
    CHECK(ok("*\n", "anything", NULL, 0, 0, 0));
    CHECK(!ok("-bad\n*\n", "bad", NULL, 0, 0, 0));
    CHECK(ok("-bad\n*\n", "good", NULL, 0, 0, 0));
    /* a failing '@' line ends the search for that process, as before */
    CHECK(!ok("@Foo\nFoo\n", "Foo", NULL, 1, 0, 0));
    /* a "%apps" that does not apply lets a later explicit name decide */
    CHECK(ok("%apps\nDock\n", "Dock", "/System/Library/CoreServices/Dock.app/Contents/MacOS/Dock", 1, 1, 0));
    CHECK(!ok("", NULL, NULL, 1, 1, 0));
    CHECK(!ok("*\n", "", NULL, 1, 1, 0));
}


/* '-' matches the executable's file name too, not only argv[0] */
static void test_renamed_argv0(void)
{
    const char *safari = "/Applications/Safari.app/Contents/MacOS/Safari";
    rdna3mtl_allow_mode m; char why[300];
    CHECK(decide(DEFAULT_LIST, "NotSafari", safari, 1, 1, 0, &m, why, sizeof why) == 0);
    CHECK(strstr(why, "denied by the list ('-Safari')"));
    CHECK(!ok("*\n-Safari\n", "x", safari, 0, 0, 0));
    CHECK(ok("*\n-Safari\n", "x", "/Applications/Other.app/Contents/MacOS/Other", 0, 0, 0));
    CHECK(ok("*\n-Safari\n", "x", NULL, 0, 0, 0));
    CHECK(ok("*\n-Safari\n", "x", "/", 0, 0, 0));
}

/* a line over the buffer is ignored whole: its tail must not become an entry */
static int decide_text(const char *text, const char *name, char *why, size_t n)
{
    rdna3mtl_allow_ctx c = { name, "/tmp/x", HOME, 1, 1, 0 };
    rdna3mtl_allow_mode m;
    FILE *f = tmpfile();
    int r;
    if (!f) { perror("tmpfile"); failures++; return -1; }
    fputs(text, f);
    r = rdna3mtl_allow_decide(f, &c, why, n, &m);
    fclose(f);
    return r;
}

static void test_long_lines(void)
{
    static char text[1200];
    char why[500];
    size_t n;

    /* exactly 255 characters is still a line (with and without a newline) */
    memset(text, '#', 255); text[255] = 0;
    strcat(text, "\n*\n");
    CHECK(decide_text(text, "Evil", why, sizeof why) == 1);
    CHECK(!strstr(why, "over 255"));
    memset(text, '#', 255); text[255] = 0;
    CHECK(decide_text(text, "Evil", why, sizeof why) == 0);
    CHECK(!strstr(why, "over 255"));
    memset(text, '#', 255); text[255] = 0;
    strcat(text, "\nEvil");
    CHECK(decide_text(text, "Evil", why, sizeof why) == 1);
    CHECK(!strstr(why, "over 255"));

    /* 260 characters: the tail "-Evil" is not a deny entry, the next line still counts */
    memset(text, 'x', 255); text[255] = 0;
    strcat(text, "-Evil\n*\n");
    CHECK(decide_text(text, "Evil", why, sizeof why) == 1);
    CHECK(strstr(why, "[1 list line(s) over 255 characters ignored]"));

    /* ... and a tail "*" is not an allow entry */
    memset(text, 'x', 255); text[255] = 0;
    strcat(text, "*\n");
    CHECK(decide_text(text, "Evil", why, sizeof why) == 0);
    CHECK(strstr(why, "[1 list line(s) over 255 characters ignored]"));

    /* a line of 5000 characters, then a normal one; two long lines count twice */
    {
        static char big[12000], longline[5001];
        memset(longline, 'y', 5000); longline[5000] = 0;
        (void)n;
        snprintf(big, sizeof big, "%s\n#c\n%s\nEvil\n", longline, longline);
        CHECK(decide_text(big, "Evil", why, sizeof why) == 1);
        CHECK(strstr(why, "[2 list line(s) over 255 characters ignored]"));
    }
}

/* NVMTL_ENABLE and the test tools bypass the list, the deny lines, the boot-arg */
static void test_bypass(void)
{
    rdna3mtl_allow_mode m; char why[300];
    FILE *f = tmpfile();
    rdna3mtl_allow_ctx tool = { "mtlprobe", "/usr/local/bin/mtlprobe", HOME, 0, 0, 1 };
    rdna3mtl_allow_ctx other = { "Foo", "/Applications/Foo.app/Contents/MacOS/Foo", HOME, 1, 1, 1 };
    fputs("-mtlprobe\n-nvmtltest\n-nvmtlrender\n-Foo\n", f);
    CHECK(rdna3mtl_process_allowed(f, &tool, 0, why, sizeof why, &m) == 1);
    CHECK(strstr(why, "built-in test tool"));
    tool.name = "nvmtltest"; CHECK(rdna3mtl_process_allowed(f, &tool, 0, why, sizeof why, &m) == 1);
    tool.name = "nvmtlrender"; CHECK(rdna3mtl_process_allowed(f, &tool, 0, why, sizeof why, &m) == 1);
    CHECK(rdna3mtl_process_allowed(NULL, &tool, 0, why, sizeof why, &m) == 1);   /* no list readable */
    /* anything else is refused by its '-' line, and by an unreadable list ... */
    CHECK(rdna3mtl_process_allowed(f, &other, 0, why, sizeof why, &m) == 0);
    CHECK(strstr(why, "denied"));
    CHECK(rdna3mtl_process_allowed(NULL, &other, 0, why, sizeof why, &m) == 0);
    CHECK(strstr(why, "no allow-list readable"));
    /* ... unless NVMTL_ENABLE is set */
    CHECK(rdna3mtl_process_allowed(f, &other, 1, why, sizeof why, &m) == 1);
    CHECK(strstr(why, "NVMTL_ENABLE"));
    CHECK(rdna3mtl_process_allowed(NULL, &other, 1, why, sizeof why, &m) == 1);
    fclose(f);
    /* the same names are refused by the decision itself: the bypass is the caller's */
    f = tmpfile(); fputs("-mtlprobe\n", f);
    CHECK(rdna3mtl_allow_decide(f, &tool, why, sizeof why, &m) == 0);
    fclose(f);
}

/* the list the package installs */
static void test_shipped_list(const char *file)
{
    static char list[8192];
    FILE *f = fopen(file, "r");
    size_t n;
    if (!f) { fprintf(stderr, "cannot open %s\n", file); failures++; return; }
    n = fread(list, 1, sizeof list - 1, f);
    fclose(f);
    list[n] = 0;
    CHECK(ok(list, "Chess", "/System/Applications/Chess.app/Contents/MacOS/Chess", 1, 1, 0));
    CHECK(ok(list, "Maps", "/System/Applications/Maps.app/Contents/MacOS/Maps", 1, 1, 0));
    CHECK(!ok(list, "Chess", "/System/Applications/Chess.app/Contents/MacOS/Chess", 1, 1, 1));
    CHECK(!ok(list, "Chess", "/System/Applications/Chess.app/Contents/MacOS/Chess", 0, 0, 0));
    CHECK(!ok(list, "Safari", "/Applications/Safari.app/Contents/MacOS/Safari", 1, 1, 0));
    CHECK(!ok(list, "Dock", "/System/Library/CoreServices/Dock.app/Contents/MacOS/Dock", 1, 1, 0));
    CHECK(ok(list, "WindowServer", "/System/Library/PrivateFrameworks/SkyLight.framework/Resources/WindowServer", 0, 1, 0));
    CHECK(ok(list, "WindowServer", "/System/Library/PrivateFrameworks/SkyLight.framework/Resources/WindowServer", 1, 1, 1));
    CHECK(!ok(list, "WindowServer", "/System/Library/PrivateFrameworks/SkyLight.framework/Resources/WindowServer", 1, 0, 0));
    CHECK(ok(list, "vulkaninfo", "/usr/local/bin/vulkaninfo", 0, 0, 0));
}

int main(int argc, char **argv)
{
    test_is_user_app();
    test_bootarg();
    test_apps_mode();
    test_kill_switch();
    test_old_lines();
    test_renamed_argv0();
    test_long_lines();
    test_bypass();
    if (argc > 1) test_shipped_list(argv[1]);
    if (failures) { fprintf(stderr, "test_mtl_allow: %d failure(s)\n", failures); return 1; }
    puts("test_mtl_allow: ok");
    return 0;
}
