/*
 * RDNA3UserClient.cpp - see RDNA3UserClient.hpp.
 *
 * SPDX-License-Identifier: MIT
 */
#include "RDNA3UserClient.hpp"
#include "RDNA3Accel.hpp"

#include <IOKit/IOLib.h>

/* XNU's IOKitKeysPrivate.h, not in the SDK; IOUserClient::clientHasPrivilege
 * compares the name: the task's audit session is the console session */
#ifndef kIOClientPrivilegeConsoleSession
#define kIOClientPrivilegeConsoleSession "consolesession"
#endif

#define super IOUserClient
OSDefineMetaClassAndStructors(RDNA3UserClient, IOUserClient)
OSDefineMetaClassAndStructors(IOAccelRDNA3Bridge, RDNA3UserClient)

bool RDNA3UserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
    if (!super::initWithTask(owningTask, securityID, type))
        return false;
    fTask = owningTask;
    fUseLock = IOLockAlloc();
    return fUseLock != nullptr;
}

void RDNA3UserClient::free()
{
    if (fUseLock)
        IOLockFree(fUseLock);
    fUseLock = nullptr;
    super::free();
}

/* the DRM file for one call: only from the task that opened the client
 * (its IOSurfaces and pages are what the file stands for; a connection
 * port handed to another process must not act as the opener), and not
 * while closeFile tears it down */
bool RDNA3UserClient::useBegin()
{
    if (current_task() != fTask)
        return false;
    IOLockLock(fUseLock);
    bool ok = fOpen != 0;
    if (ok)
        fUsers++;
    IOLockUnlock(fUseLock);
    return ok;
}

void RDNA3UserClient::useEnd()
{
    IOLockLock(fUseLock);
    if (--fUsers == 0)
        IOLockWakeup(fUseLock, &fUsers, false);
    IOLockUnlock(fUseLock);
}

bool RDNA3UserClient::start(IOService *provider)
{
    if (!super::start(provider))
        return false;
    fAccel = fOwner ? fOwner : OSDynamicCast(RDNA3Accel, provider);
    if (!fAccel || !fAccel->drmAvailable())
        return false;
    fFile = (rdna3_drm_file *)IOMalloc(sizeof(*fFile));
    if (!fFile)
        return false;
    /* the console user gets the larger VMID share (rdna3_vmid_uid_cap) */
    bool console = clientHasPrivilege(fTask, kIOClientPrivilegeConsoleSession) == kIOReturnSuccess;
    if (fAccel->drmOpen(fFile, console) != 0) {
        IOFree(fFile, sizeof(*fFile));
        fFile = nullptr;
        return false;
    }
    fFile->os = fTask;   /* GEM_USERPTR wires pages of this task */
    fOpen = 1;
    return true;
}

/* clientClose and stop (the termination thread) may both get here; the
 * close's fence wait drops the DRM lock, so only the one that clears fOpen
 * closes and frees the file (ECC review of card run 50's close wait) */
void RDNA3UserClient::closeFile()
{
    if (!OSCompareAndSwap(1, 0, &fOpen))
        return;
    /* an ioctl or map of this client still running (ECC security review:
     * it read fFile while another thread freed it) finishes first */
    IOLockLock(fUseLock);
    while (fUsers)
        IOLockSleep(fUseLock, &fUsers, THREAD_UNINT);
    IOLockUnlock(fUseLock);
    fAccel->drmClose(fFile);
    IOFree(fFile, sizeof(*fFile));
    fFile = nullptr;
}

IOReturn RDNA3UserClient::clientClose()
{
    closeFile();
    if (!isInactive())
        terminate();
    return kIOReturnSuccess;
}

void RDNA3UserClient::stop(IOService *provider)
{
    closeFile();
    super::stop(provider);
}

IOReturn RDNA3UserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                         IOExternalMethodDispatch *, OSObject *, void *)
{
    if (selector == kSelPm && fOpen)
        return pmMethod(args);
    if (selector != kSelIoctl || !fOpen)
        return kIOReturnBadArgument;
    if (args->scalarInputCount < 1 || args->scalarOutputCount < 1)
        return kIOReturnBadArgument;
    if (!useBegin())
        return current_task() != fTask ? kIOReturnNotPrivileged : kIOReturnNotOpen;
    IOReturn r = ioctlMethod(args);
    useEnd();
    return r;
}

IOReturn RDNA3UserClient::ioctlMethod(IOExternalMethodArguments *args)
{

    uint32_t nr = (uint32_t)args->scalarInput[0];
    uint32_t inSize = args->structureInputSize;
    uint32_t outSize = args->structureOutputSize;
    if (args->structureInputDescriptor || args->structureOutputDescriptor ||
        inSize > kMaxPayload || outSize > kMaxPayload)
        return kIOReturnBadArgument;

    /* kernel stacks are 16 KiB: keep the payload off the stack */
    uint8_t *buf = (uint8_t *)IOMalloc(kMaxPayload);
    if (!buf)
        return kIOReturnNoMemory;
    uint32_t size = inSize > outSize ? inSize : outSize;
    bzero(buf, kMaxPayload);
    if (inSize)
        memcpy(buf, args->structureInput, inSize);

    int ret = fAccel->drmIoctl(fFile, nr, buf, size);

    if (outSize)
        memcpy(args->structureOutput, buf, outSize);
    IOFree(buf, kMaxPayload);
    args->scalarOutput[0] = (uint64_t)(ret < 0 ? -ret : 0);
    args->scalarOutputCount = 1;
    return kIOReturnSuccess;
}

IOReturn RDNA3UserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
                                              IOMemoryDescriptor **memory)
{
    if (!useBegin())
        return current_task() != fTask ? kIOReturnNotPrivileged : kIOReturnNotOpen;
    uint64_t off = (uint64_t)type << 12;
    IOMemoryDescriptor *md = fAccel->drmMemoryForOffset(fFile, off, options);
    useEnd();
    if (!md)
        return kIOReturnNotFound;
    *memory = md;   /* retained; IOUserClient releases it */
    return kIOReturnSuccess;
}

/* kSelPm: one rdna3_pmctl_req in, one rdna3_pmctl_reply out. Reads are for
 * everyone who may open the device; the request carries whether this
 * caller is an administrator (the writes need one). */
IOReturn RDNA3UserClient::pmMethod(IOExternalMethodArguments *args)
{
    rdna3_pmctl_req rq;
    if (args->structureInputDescriptor || args->structureOutputDescriptor ||
        args->structureInputSize != sizeof(rq) ||
        args->structureOutputSize < sizeof(rdna3_pmctl_reply) || args->scalarOutputCount < 1)
        return kIOReturnBadArgument;
    memcpy(&rq, args->structureInput, sizeof(rq));
    rdna3_pmctl_reply *rp = (rdna3_pmctl_reply *)IOMalloc(sizeof(*rp));
    if (!rp)
        return kIOReturnNoMemory;
    bzero(rp, sizeof(*rp));
    bool admin = clientHasPrivilege(fTask, kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
    int ret = fAccel->pmCtl(&rq, rp, admin);
    memcpy(args->structureOutput, rp, sizeof(*rp));
    args->structureOutputSize = sizeof(*rp);
    IOFree(rp, sizeof(*rp));
    args->scalarOutput[0] = (uint64_t)(ret < 0 ? -ret : 0);
    args->scalarOutputCount = 1;
    return kIOReturnSuccess;
}
