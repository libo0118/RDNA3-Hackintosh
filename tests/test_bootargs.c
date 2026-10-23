/*
 * test_bootargs.c - common/rdna3_bootargs.h on the host: the default
 * profile and rd3_gamma_on() (the output gamma, on by default since card
 * run 56, off only with rdna3gamma=0), with a stubbed boot-arg reader.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include "rdna3_bootargs.h"

const struct test_bootarg *test_bootargs;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    static const struct test_bootarg none[] = { { NULL, 0 } };
    static const struct test_bootarg off[] = { { "rdna3gamma", 0 }, { NULL, 0 } };
    static const struct test_bootarg on[] = { { "rdna3gamma", 1 }, { NULL, 0 } };
    static const struct test_bootarg noprofile[] = { { "rdna3profile", 0 }, { NULL, 0 } };
    static const struct test_bootarg ladder[] = { { "rdna3lvl", 8 }, { "rdna3msi", 1 }, { NULL, 0 } };
    static const struct test_bootarg ladder_off[] = { { "rdna3lvl", 8 }, { "rdna3gamma", 0 }, { NULL, 0 } };
    static const struct test_bootarg noprofile_off[] = { { "rdna3profile", 0 }, { "rdna3gamma", 0 }, { NULL, 0 } };

    /* no boot-args: the profile; gamma on */
    test_bootargs = none;
    CHECK(rd3_profile_on());
    CHECK(rd3_arg("rdna3fb", 0) == 1 && rd3_arg("rdna3lvl", 0) == 8);
    CHECK(rd3_gamma_on());
    /* rdna3gamma=0 turns it off, =1 keeps it on */
    test_bootargs = off;
    CHECK(!rd3_gamma_on());
    CHECK(rd3_arg("rdna3fb", 0) == 1);   /* nothing else changes */
    test_bootargs = on;
    CHECK(rd3_gamma_on());
    /* without the profile (rdna3profile=0, or a ladder step's rdna3lvl) gamma stays on */
    test_bootargs = noprofile;
    CHECK(!rd3_profile_on());
    CHECK(rd3_arg("rdna3fb", 0) == 0);
    CHECK(rd3_gamma_on());
    test_bootargs = ladder;
    CHECK(!rd3_profile_on());
    CHECK(rd3_gamma_on());
    test_bootargs = ladder_off;
    CHECK(!rd3_gamma_on());
    test_bootargs = noprofile_off;
    CHECK(!rd3_gamma_on());

    /* card run 58: WindowServer's display framebuffers stay out of A3 unless
     * rdna3a3display=1, with or without the profile */
    {
        static const struct test_bootarg a3on[] = { { "rdna3a3display", 1 }, { NULL, 0 } };
        static const struct test_bootarg a3off[] = { { "rdna3a3display", 0 }, { NULL, 0 } };
        static const struct test_bootarg a3ladder[] = { { "rdna3lvl", 8 }, { "rdna3a3display", 1 }, { NULL, 0 } };
        test_bootargs = none;
        CHECK(!rd3_a3_display_on());
        test_bootargs = ladder;
        CHECK(!rd3_a3_display_on());
        test_bootargs = a3on;
        CHECK(rd3_a3_display_on());
        test_bootargs = a3off;
        CHECK(!rd3_a3_display_on());
        test_bootargs = a3ladder;
        CHECK(rd3_a3_display_on());
    }
    /* DRIVER-PLAN N42: the vblank / pflip interrupt path on unless rdna3vblirq=0, with or without the profile */
    {
        static const struct test_bootarg virqoff[] = { { "rdna3vblirq", 0 }, { NULL, 0 } };
        static const struct test_bootarg virqladder[] = { { "rdna3lvl", 8 }, { "rdna3vblirq", 0 }, { NULL, 0 } };
        test_bootargs = none;
        CHECK(rd3_vblirq_on());
        test_bootargs = ladder;
        CHECK(rd3_vblirq_on());
        test_bootargs = virqoff;
        CHECK(!rd3_vblirq_on() && rd3_gamma_on());
        test_bootargs = virqladder;
        CHECK(!rd3_vblirq_on());
    }
    /* card run 58 (2026-10-15d): WindowServer's inputs out unless rdna3a3ws=1; A3 on unless rdna3a3=0 */
    {
        static const struct test_bootarg wson[] = { { "rdna3a3ws", 1 }, { NULL, 0 } };
        static const struct test_bootarg a3off[] = { { "rdna3a3", 0 }, { NULL, 0 } };
        test_bootargs = none;
        CHECK(!rd3_a3_ws_on() && rd3_a3_on());
        test_bootargs = ladder;
        CHECK(!rd3_a3_ws_on() && rd3_a3_on());
        test_bootargs = wson;
        CHECK(rd3_a3_ws_on() && rd3_a3_on() && !rd3_a3_display_on());
        test_bootargs = a3off;
        CHECK(!rd3_a3_on() && !rd3_a3_ws_on());
    }

    if (fails) {
        printf("test_bootargs: %d check(s) failed\n", fails);
        return 1;
    }
    printf("test_bootargs: all checks passed\n");
    return 0;
}
