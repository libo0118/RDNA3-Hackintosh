/* DRIVER-PLAN N71: an app's IOSurface textures stay private while the kernel keeps WindowServer's inputs out of family VRAM */
#include <stdio.h>
#include "nvmtl_surface_share.h"

static int fails;
static void check(int got, int want, const char *what)
{
    if (got != want) { printf("FAIL %s: %d, want %d\n", what, got, want); fails++; }
}

int main(void)
{
    check(nvmtl_surface_share_kept_private(0, 0, 0), 1, "app, a3ws 0: private");
    check(nvmtl_surface_share_kept_private(0, 1, 0), 0, "app, a3ws 1: shared");
    check(nvmtl_surface_share_kept_private(0, -1, 0), 0, "app, no sysctl (older accelerator): shared as before");
    check(nvmtl_surface_share_kept_private(1, 0, 0), 0, "WindowServer: its own policy, unchanged");
    check(nvmtl_surface_share_kept_private(0, 0, 1), 0, "NVMTL_SURFACE_SHARE=1 forces sharing");
    check(nvmtl_surface_share_kept_private(1, 1, 1), 0, "WindowServer, forced: shared");
    printf("test_surface_share: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
