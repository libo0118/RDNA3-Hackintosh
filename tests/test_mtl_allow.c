/* Which processes get the Metal plugin's device: the allow list policy of
 * metal/plugin/rdna3mtl_allow_policy.h (apps mode, '-' / '!' / '@' lines, the
 * boot-arg kill switch). SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include "rdna3mtl_allow_policy.h"

static int failures;
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
    rdna3mtl_allow_ctx c = { name, path, armed, pipe_up, apps_off };
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
    CHECK(rdna3mtl_is_user_app("/System/Applications/Chess.app/Contents/MacOS/Chess"));
    CHECK(rdna3mtl_is_user_app("/System/Applications/Maps.app/Contents/MacOS/Maps"));
    CHECK(rdna3mtl_is_user_app("/System/Applications/System Settings.app/Contents/MacOS/System Settings"));
    CHECK(rdna3mtl_is_user_app("/Applications/Utilities/Terminal.app/Contents/MacOS/Terminal"));
    CHECK(rdna3mtl_is_user_app("/Applications/Foo.app/Contents/MacOS/Foo"));
    CHECK(rdna3mtl_is_user_app("/Users/kevin/Applications/Foo.app/Contents/MacOS/Foo"));
    /* the session's agents, helpers and tools are not applications */
    CHECK(!rdna3mtl_is_user_app("/System/Library/CoreServices/Dock.app/Contents/MacOS/Dock"));
    CHECK(!rdna3mtl_is_user_app("/System/Library/CoreServices/Finder.app/Contents/MacOS/Finder"));
    CHECK(!rdna3mtl_is_user_app("/System/Library/CoreServices/SystemUIServer.app/Contents/MacOS/SystemUIServer"));
    CHECK(!rdna3mtl_is_user_app("/System/Library/Frameworks/WebKit.framework/Versions/A/XPCServices/"
                                "com.apple.WebKit.GPU.xpc/Contents/MacOS/com.apple.WebKit.GPU"));
    CHECK(!rdna3mtl_is_user_app("/Applications/Google Chrome.app/Contents/Frameworks/Google Chrome Framework.framework/"
                                "Helpers/Google Chrome Helper (GPU).app/Contents/MacOS/Google Chrome Helper (GPU)"));
    CHECK(!rdna3mtl_is_user_app("/Applications/Foo.app/Contents/XPCServices/Bar.xpc/Contents/MacOS/Bar"));
    CHECK(!rdna3mtl_is_user_app("/Applications/Foo.app/Contents/Resources/tool"));
    CHECK(!rdna3mtl_is_user_app("/Applications/Foo.app/Contents/MacOS/"));
    CHECK(!rdna3mtl_is_user_app("/Applications/Foo.app/Contents/MacOS/sub/Foo"));
    CHECK(!rdna3mtl_is_user_app("/Applications/Foo.app.bak/Contents/MacOS/Foo"));
    CHECK(!rdna3mtl_is_user_app("/Applications/.app/Contents/MacOS/Foo"));
    CHECK(!rdna3mtl_is_user_app("/Applications/tool"));
    CHECK(!rdna3mtl_is_user_app("/usr/bin/vulkaninfo"));
    CHECK(!rdna3mtl_is_user_app("/tmp/Foo.app/Contents/MacOS/Foo"));
    CHECK(!rdna3mtl_is_user_app("/Users//Applications/Foo.app/Contents/MacOS/Foo"));
    CHECK(!rdna3mtl_is_user_app("/Users/kevin/Documents/Foo.app/Contents/MacOS/Foo"));
    CHECK(!rdna3mtl_is_user_app("/Users/kevin"));
    CHECK(!rdna3mtl_is_user_app(""));
    CHECK(!rdna3mtl_is_user_app(NULL));
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
    CHECK(strstr(why, "main executable"));
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
    if (argc > 1) test_shipped_list(argv[1]);
    if (failures) { fprintf(stderr, "test_mtl_allow: %d failure(s)\n", failures); return 1; }
    puts("test_mtl_allow: ok");
    return 0;
}
