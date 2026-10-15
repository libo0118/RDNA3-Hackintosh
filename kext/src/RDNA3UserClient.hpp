/*
 * RDNA3UserClient - the kernel end of the libdrm shim.
 *
 * Userspace opens the RDNA3Accel service (IOServiceOpen, type 0) once per
 * "DRM fd". Where the sandbox refuses that class (WindowServer: "IOUC
 * RDNA3UserClient failed MACF", card run 47), it opens RDNA3Accelerator
 * with type RD3K_UC_TYPE_BASE instead and gets an IOAccelRDNA3Bridge: the
 * same client under a name WindowServer's sandbox lets through, attached
 * to one of the accelerator's carriers (NullMoth's IOAccelNVRMBridge).
 * Selectors:
 *   0 kSelIoctl   scalar in [nr], struct in/out = ioctl payload,
 *                 scalar out [errno]; nr is the DRM ioctl number byte
 *                 (DRM_COMMAND_BASE + DRM_AMDGPU_* for amdgpu ioctls).
 *   1 kSelPm      power management (common/rdna3_pmctl.h): struct in =
 *                 rdna3_pmctl_req, struct out = rdna3_pmctl_reply, scalar
 *                 out [errno]. Writes are gated by the boot-args
 *                 rdna3pmctl / rdna3baco and need an administrator.
 * mmap:  IOConnectMapMemory64(conn, type = mmap_offset >> 12, ...) where
 *        mmap_offset comes from DRM_AMDGPU_GEM_MMAP.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_USERCLIENT_HPP
#define RDNA3_USERCLIENT_HPP

#include <IOKit/IOUserClient.h>

extern "C" {
#include "rdna3_drm.h"
#include "rdna3_pmctl.h"
}

class RDNA3Accel;

class RDNA3UserClient : public IOUserClient {
    OSDeclareDefaultStructors(RDNA3UserClient)

public:
    enum { kSelIoctl = 0, kSelPm = RDNA3_PMCTL_SELECTOR, kSelCount };
    enum { kMaxPayload = 4096 };

    virtual bool initWithTask(task_t owningTask, void *securityID, UInt32 type) override;
    virtual void free() override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual IOReturn clientClose() override;
    void closeFile();
    virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                    IOExternalMethodDispatch *dispatch, OSObject *target,
                                    void *reference) override;
    virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
                                         IOMemoryDescriptor **memory) override;
    /* the device when the provider is not RDNA3Accel (a bridge on a carrier) */
    void setOwner(RDNA3Accel *owner) { fOwner = owner; }

private:
    IOReturn        pmMethod(IOExternalMethodArguments *args);
    IOReturn        ioctlMethod(IOExternalMethodArguments *args);
    bool            useBegin();
    void            useEnd();
    IOLock         *fUseLock = nullptr;
    uint32_t        fUsers = 0;    /* ioctls / maps in progress, under fUseLock */
    RDNA3Accel     *fAccel = nullptr;
    RDNA3Accel     *fOwner = nullptr;
    task_t          fTask = nullptr;
    rdna3_drm_file *fFile = nullptr;
    volatile UInt32 fOpen = 0;     /* 1 while the DRM file is open; closeFile clears it once */
};

/* RDNA3UserClient handed out by the accelerator (RD3K_FN_NEW_CLIENT) */
class IOAccelRDNA3Bridge : public RDNA3UserClient {
    OSDeclareDefaultStructors(IOAccelRDNA3Bridge)
};

#endif
