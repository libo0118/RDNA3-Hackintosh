/*
 * RDNA3Bringup - Stage 2 driver: runs hw/ (PSP, SMU) on the real GPU.
 *
 * Runs on its own kernel thread because it has to wait for
 * kernelmanagerd to hand over firmware (OSKextRequestResource) and the
 * PSP sequence takes seconds. Levels (boot-arg rdna3lvl=):
 *   4  map BAR0, discovery, FB location, HDP remap, PSP bootloader + sOS,
 *      KM ring, TMR, all IP firmware, RLC autoload
 *   5  + SMU hello (TestMessage / GetSmuVersion / GetDriverIfVersion)
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_BRINGUP_HPP
#define RDNA3_BRINGUP_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <IOKit/pci/IOPCIDevice.h>

extern "C" {
#include "rdna3_hw.h"
#include "psp13.h"
#include "gmc11.h"
#include "amd_fw.h"
}

class RDNA3Accel;

struct RDNA3Firmware {
    uint8_t *data;
    uint32_t len;
};

class RDNA3Bringup {
public:
    bool init(RDNA3Accel *owner, IOPCIDevice *pci, IOMemoryMap *mmio,
              uint32_t level, const amd_discovery *disc, uint64_t vramSize);
    void start();          /* spawns the thread */
    void stopAndFree();    /* waits for the thread, frees everything */

    /* rdna3_os callbacks (static trampolines) */
    static void     osUdelay(void *ctx, uint32_t us);
    static void     osMsleep(void *ctx, uint32_t ms);
    static void     osVlog(void *ctx, int level, const char *fmt, va_list ap);
    static void     osWmb(void *ctx);
    static uint32_t osMmioRd(void *ctx, uint64_t off);
    static void     osMmioWr(void *ctx, uint64_t off, uint32_t v);

private:
    static void threadMain(void *arg, int wait_result);
    void run();
    bool loadFirmware(const char *name, RDNA3Firmware *out);
    static void fwCallback(unsigned int tag, int result, const void *data,
                           uint32_t len, void *ctx);
    bool mapVram();
    void publish(const char *key, uint64_t v);
    void publishString(const char *key, const char *s);

    RDNA3Accel   *fOwner  = nullptr;
    IOPCIDevice  *fPci    = nullptr;
    IOMemoryMap  *fMmio   = nullptr;
    IOMemoryMap  *fVram   = nullptr;
    volatile uint8_t *fMmioBase = nullptr;
    uint32_t      fLevel  = 0;
    amd_discovery *fDisc  = nullptr;   /* owned copy */
    uint64_t      fVramSize = 0;

    rdna3_dev    *fDev    = nullptr;
    rdna3_psp    *fPsp    = nullptr;
    amd_ucode_plan *fPlan = nullptr;
    rdna3_gmc    *fGmc    = nullptr;   /* level 6 */
    RDNA3Firmware fSos    = {};
    RDNA3Firmware fBlobs[AMD_BLOB_COUNT] = {};

    /* firmware request rendezvous */
    IOLock       *fLock   = nullptr;
    bool          fFwDone = false;
    RDNA3Firmware fFwResult = {};

    /* thread state */
    bool          fRunning = false;
    bool          fFinished = false;
    volatile bool fAbort = false;
};

#endif
