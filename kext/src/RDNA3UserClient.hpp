/*
 * RDNA3UserClient - the kernel end of the libdrm shim.
 *
 * Userspace opens the RDNA3Accel service (IOServiceOpen, type 0) once per
 * "DRM fd". Selectors:
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
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual IOReturn clientClose() override;
    virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                    IOExternalMethodDispatch *dispatch, OSObject *target,
                                    void *reference) override;
    virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
                                         IOMemoryDescriptor **memory) override;

private:
    IOReturn        pmMethod(IOExternalMethodArguments *args);
    RDNA3Accel     *fAccel = nullptr;
    task_t          fTask = nullptr;
    rdna3_drm_file *fFile = nullptr;
    bool            fOpen = false;
};

#endif
