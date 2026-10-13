/*
 * test_agdc_k5.c - RDNA3AGDC's display-topology answers (agdc/rdna3_agdc_k5.h)
 * for a 1920x1080@60 head: the timing words a framebuffer publishes, the
 * kAGDCLinkConfig and plane-scaler answers built from them, and the refusals.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "../agdc/rdna3_agdc_k5.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint32_t rd32(const uint8_t *e, unsigned off)
{
    return (uint32_t)e[off] | (uint32_t)e[off + 1] << 8 | (uint32_t)e[off + 2] << 16 | (uint32_t)e[off + 3] << 24;
}

int main(void)
{
    uint32_t t[2][RD3K5_TIMING_WORDS];
    struct rd3k5_head h[2];
    static struct rd3k5_link_config lin, lout;
    static struct rd3k5_fb_ext_cap cin, cout;
    int why = 0;
    int32_t idx;

    /* CEA 1920x1080@60: 148.5 MHz, h 88/44/148, v 4/5/36, both syncs positive */
    CHECK(rd3k5_timing_from(t[0], 148500000ull, 1920, 280, 88, 44, 1080, 45, 4, 5, 60000, 1, 1) == 0);
    CHECK(t[0][RD3K5_T_TAG] == RD3K5_TIMING_TAG);
    CHECK(t[0][RD3K5_T_HSS] == 2008 && t[0][RD3K5_T_HSE] == 2052 && t[0][RD3K5_T_HTOT] == 2200);
    CHECK(t[0][RD3K5_T_VSS] == 1084 && t[0][RD3K5_T_VSE] == 1089 && t[0][RD3K5_T_VTOT] == 1125);
    CHECK(rd3k5_timing_bad(t[0], sizeof t[0]) == 0);
    /* a sync pulse past the blanking, no pixel clock */
    CHECK(rd3k5_timing_from(t[1], 148500000ull, 1920, 280, 250, 44, 1080, 45, 4, 5, 60000, 1, 1) == 2);
    CHECK(rd3k5_timing_from(t[1], 0, 1920, 280, 88, 44, 1080, 45, 4, 5, 60000, 1, 1) == 1);
    CHECK(rd3k5_timing_bad(t[1], sizeof t[1]) == 2);   /* zeroed: no tag */
    CHECK(rd3k5_timing_bad(t[0], 12) == 1);

    h[0].t = t[0]; h[0].tBytes = sizeof t[0];
    h[1].t = NULL; h[1].tBytes = 0;

    /* kAGDCLinkConfig for index 0 */
    memset(&lin, 0, sizeof lin);
    lin.id = 0;
    CHECK(rd3k5_answer(RD3K5_CMD_LINKCONFIG, &lin, sizeof lin, &lout, sizeof lout, h, 2, &why, &idx) == RD3K5_R_LINK);
    CHECK(idx == 0 && lout.id == 0 && lout.port == 1 && lout.state == 1 && lout.groupID == -1);
    CHECK(lout.timing.pixelClock == 148500000ull && lout.timing.hActive == 1920 && lout.timing.hBlanking == 280);
    CHECK(lout.timing.hSyncOffset == 88 && lout.timing.hSyncPulseWidth == 44);
    CHECK(lout.timing.vActive == 1080 && lout.timing.vBlanking == 45 && lout.timing.vSyncOffset == 4);
    CHECK(lout.timing.vSyncPulseWidth == 5 && lout.timing.hSyncConfig == 1 && lout.timing.numLinks == 1);
    /* in place, as IOPresentment may pass it */
    memset(&lout, 0, sizeof lout);
    CHECK(rd3k5_answer(RD3K5_CMD_LINKCONFIG, &lout, sizeof lout, &lout, sizeof lout, h, 2, &why, &idx) == RD3K5_R_LINK);
    CHECK(lout.timing.hActive == 1920);
    /* index 1 has no timing (console head), index 2 does not exist, wrong size */
    lin.id = 1;
    CHECK(rd3k5_answer(RD3K5_CMD_LINKCONFIG, &lin, sizeof lin, &lout, sizeof lout, h, 2, &why, &idx) == RD3K5_R_NOTFOUND);
    CHECK(why == 0x31 && idx == 1);
    lin.id = 2;
    CHECK(rd3k5_answer(RD3K5_CMD_LINKCONFIG, &lin, sizeof lin, &lout, sizeof lout, h, 2, &why, &idx) == RD3K5_R_BADARG);
    CHECK(why == 0x11);
    CHECK(rd3k5_answer(RD3K5_CMD_LINKCONFIG, &lin, sizeof lin - 1, &lout, sizeof lout, h, 2, &why, &idx) == RD3K5_R_BADARG);

    /* the plane scaler of index 0: limits = the raster */
    memset(&cin, 0, sizeof cin);
    cin.fbIndex = 0;
    cin.type = RD3K5_CAP_PLANESCALER;
    CHECK(rd3k5_answer(RD3K5_CMD_FBCAPEX, &cin, sizeof cin, &cout, sizeof cout, h, 2, &why, &idx) == RD3K5_R_CAPS);
    CHECK(cout.count == 1 && cout.type == RD3K5_CAP_PLANESCALER);
    CHECK(rd32(cout.entry[0], 0x024) == 1080 && rd32(cout.entry[0], 0x028) == 1920);
    CHECK(rd32(cout.entry[0], 0x1b4) == 1080 && rd32(cout.entry[0], 0x1b8) == 1920);
    /* another capability type is not ours to answer; another command neither */
    cin.type = 0x11;
    CHECK(rd3k5_answer(RD3K5_CMD_FBCAPEX, &cin, sizeof cin, &cout, sizeof cout, h, 2, &why, &idx) == RD3K5_R_UNSUPPORTED);
    CHECK(rd3k5_answer(0x922, &cin, sizeof cin, &cout, sizeof cout, h, 2, &why, &idx) == RD3K5_R_UNSUPPORTED);

    if (fails) {
        printf("test_agdc_k5: %d failure(s)\n", fails);
        return 1;
    }
    printf("test_agdc_k5: all passed\n");
    return 0;
}
