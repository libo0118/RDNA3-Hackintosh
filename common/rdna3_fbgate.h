/*
 * rdna3_fbgate.h - the framebuffers' IOGraphics controller gates the
 * accelerator closes before it calls the family's display machine itself
 * (card run 2026-10-26j, DRIVER-PLAN N58). Plain C, no kernel headers: the
 * accelerator uses it and tests/test_fbgate.c checks it on the host.
 *
 * The hang (kernel stackshot of reports/2026-10-26j, macOS 26.4.1, every
 * boot with -cpu host, rarely on Skylake-Client): head 1 published after
 * the arm, so rd3AccelBootGates (a thread call) registered it and
 * delivered the mode change itself:
 *   thread call  triggerFromSysctl -> registerFramebufferLocked ->
 *                deliverModeChange -> IOAccelLegacyDisplayMachine::
 *                display_mode_did_change, which takes the ACCELERATOR LOCK
 *                (IOGraphicsAccelerator2 +0x88), then for every pipe
 *                IOAccelLegacyDisplayPipe::displayModeDidChange ->
 *                IOFramebuffer::getAttributeExt -> the CONTROLLER GATE
 *                (IOGraphicsWorkLoop::timedCloseGate) - blocked;
 *   AppleGPUWrangler  IOFramebuffer::message, inside the CONTROLLER GATE ->
 *                probeAccelerator -> IOGraphicsAccelerator2::requestProbe ->
 *                the ACCELERATOR LOCK - blocked.
 * IOGraphics' own order is gate, then accelerator lock: the family's
 * display machine normally runs from IOAccelDisplayPipe::
 * display_change_handler, an IOFramebuffer notification IOGraphics
 * delivers inside the controller gate, and probeAccelerator is called
 * inside it too. The thread call took them the other way round.
 *
 * The fix keeps IOGraphics' order: the thread (or sysctl writer) closes
 * the controller gate of every published framebuffer (IOFramebuffer::
 * getWorkLoop() is the controller's IOGraphicsControllerWorkLoop, the gate
 * getAttributeExt and message close; the gate is recursive for its owner,
 * so the family's own closes inside it pass) before found_framebuffer /
 * display_mode_will_change / display_mode_did_change / start, which take
 * the accelerator lock. Heads that share a controller share its gate: each
 * workloop is closed once, in head order (the only place that holds more
 * than one, always under fRegLock, so the order is the same everywhere),
 * and opened in the reverse order.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_FBGATE_H
#define RDNA3_FBGATE_H

#define RD3_FBGATE_MAX 8u

struct rd3_fbgate_plan {
    unsigned n;                      /* gates to close */
    unsigned head[RD3_FBGATE_MAX];   /* close order: the first head of each workloop, ascending */
    unsigned gated;                  /* bit i: head i's framebuffer is inside a gate of the plan */
    unsigned noWl;                   /* bit i: head i published but has no controller workloop: NOT gated */
};

/* fb[i]: head i's published framebuffer or NULL; wl[i]: its controller
 * workloop (IOFramebuffer::getWorkLoop) or NULL. Heads past RD3_FBGATE_MAX
 * are not looked at (the accelerator has kRd3MaxFB <= RD3_FBGATE_MAX). */
static inline void rd3_fbgate_plan(const void *const fb[], const void *const wl[], unsigned heads,
                                   struct rd3_fbgate_plan *p)
{
    p->n = 0;
    p->gated = 0;
    p->noWl = 0;
    if (heads > RD3_FBGATE_MAX)
        heads = RD3_FBGATE_MAX;
    for (unsigned i = 0; i < heads; i++) {
        if (!fb[i])
            continue;
        if (!wl[i]) {
            p->noWl |= 1u << i;
            continue;
        }
        unsigned k;
        for (k = 0; k < p->n; k++)
            if (wl[p->head[k]] == wl[i])
                break;
        if (k == p->n)
            p->head[p->n++] = i;
        p->gated |= 1u << i;
    }
}

#endif /* RDNA3_FBGATE_H */
