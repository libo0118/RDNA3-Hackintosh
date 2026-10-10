/*
 * rdna3_bootargs.h - boot-arg values with the driver's default profile,
 * shared by RDNA3Accel, RDNA3Framebuffer and RDNA3Accelerator (kernel only).
 *
 * Without any rdna3lvl boot-arg the driver runs the profile verified on the
 * card (card runs 20-22, step big2): every bring-up level, MSI, DMCUB,
 * the framebuffer with the accelerator's display pipe, the second monitor
 * extended, PMFW DPM, the idle GFXCLK cap, display sleep and the mode list.
 * A boot-arg always wins over the profile. The test ladder sets rdna3lvl,
 * so its steps keep their explicit, minimal settings; rdna3profile=1 adds
 * the profile's defaults to such a step, rdna3profile=0 drops them.
 * -rdna3off still turns everything off.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_BOOTARGS_H
#define RDNA3_BOOTARGS_H

#include <stdint.h>
#include <string.h>
#include <pexpert/pexpert.h>

static inline int rd3_profile_on(void)
{
    uint32_t p = 0, lvl = 0;
    if (PE_parse_boot_argn("rdna3profile", &p, sizeof(p)))
        return p != 0;
    return !PE_parse_boot_argn("rdna3lvl", &lvl, sizeof(lvl));
}

/* the value of boot-arg 'name': given -> as given; else the profile's
 * value when the profile is on and has one; else 'dflt' */
static inline uint32_t rd3_arg(const char *name, uint32_t dflt)
{
    static const struct { const char *name; uint32_t v; } profile[] = {
        { "rdna3lvl", 8 },      { "rdna3msi", 1 },     { "rdna3dmub", 1 },     { "rdna3fb", 1 },
        { "rdna3mirror", 2 },   { "rdna3accel", 1 },   { "rdna3accelfb", 4 },  { "rdna3acceliop", 1 },
        { "rdna3pm", 2 },       { "rdna3idlecap", 1 }, { "rdna3dpms", 1 },     { "rdna3modes", 1 },
    };
    uint32_t v = 0, i;
    if (PE_parse_boot_argn(name, &v, sizeof(v)))
        return v;
    if (!rd3_profile_on())
        return dflt;
    for (i = 0; i < sizeof(profile) / sizeof(profile[0]); i++)
        if (!strcmp(profile[i].name, name))
            return profile[i].v;
    return dflt;
}

#endif
