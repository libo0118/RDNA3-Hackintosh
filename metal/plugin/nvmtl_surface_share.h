/*
 * nvmtl_surface_share.h - whether an application's IOSurface textures live in the surface's family VRAM (item 7) while
 * the kernel keeps WindowServer's inputs out of it (DRIVER-PLAN N71).
 *
 * With BAR0 at 1 GiB the visible heap has room, so an app's CAMetalLayer drawables became family-VRAM surfaces; the
 * kernel refuses WindowServer that import (`kept out of A3: a WindowServer input`, debug.rdna3accel_a3ws 0, card run 58),
 * so WindowServer read every frame through the BAR with the CPU (~4.5 ms a 1080p frame, the game 123-145 fps instead of
 * ~215). Plain C, so tests/test_surface_share.c checks the decision exactly as nvmtl_vk.c uses it.
 */
#ifndef NVMTL_SURFACE_SHARE_H
#define NVMTL_SURFACE_SHARE_H

/* 1: this process's IOSurface textures stay private (copies) although the import works. is_ws: the process is
 * WindowServer; a3ws: debug.rdna3accel_a3ws (-1 when the sysctl is absent: an older accelerator, no policy to follow);
 * forced: NVMTL_SURFACE_SHARE=1 (share regardless). */
static inline int nvmtl_surface_share_kept_private(int is_ws, int a3ws, int forced)
{
    return !forced && !is_ws && a3ws == 0;
}

#endif
