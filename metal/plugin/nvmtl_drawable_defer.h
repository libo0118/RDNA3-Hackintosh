/*
 * nvmtl_drawable_defer.h - which copy-outs of a drawable's IOSurface a command buffer's seal leaves out (DRIVER-PLAN N69).
 *
 * A game writes its CAMetalDrawable in several command buffers per frame; only the copy of the private image into the IOSurface that
 * the PRESENTING command buffer takes is ever read (WindowServer reads the surface after the present). Plain C, so
 * tests/test_drawable_defer.c checks the decision exactly as NVMTLEventSync.m uses it.
 */
#ifndef NVMTL_DRAWABLE_DEFER_H
#define NVMTL_DRAWABLE_DEFER_H

/* 1: leave this texture's copy-out out of the seal's surface set (its writes stay in the private image; the GPU queue orders them before the
 * copy of the command buffer that presents it). `on` is NVMTL_DRAWABLE_DEFER, `presented_once` the root texture was ever handed to a present
 * call, `presented_here` this command buffer presents it (any of its drawables, not only the last). */
static inline int nvmtl_drawable_defer(int on, int presented_once, int presented_here)
{
    return on && presented_once && !presented_here;
}

#endif
