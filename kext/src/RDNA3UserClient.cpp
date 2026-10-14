/*
 * RDNA3UserClient.cpp - see RDNA3UserClient.hpp.
 *
 * SPDX-License-Identifier: MIT
 */
#include "RDNA3UserClient.hpp"
#include "RDNA3Accel.hpp"

#include <IOKit/IOLib.h>

#define super IOUserClient
OSDefineMetaClassAndStructors(RDNA3UserClient, IOUserClient)
OSDefineMetaClassAndStructors(IOAccelRDNA3Bridge, RDNA3UserClient)

bool RDNA3UserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
    if (!super::initWithTask(owningTask, securityID, type))
        return false;
    fTask = owningTask;
    return true;
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
    if (fAccel->drmOpen(fFile) != 0) {
        IOFree(fFile, sizeof(*fFile));
        fFile = nullptr;
        return false;
    }
    fFile->os = fTask;   /* GEM_USERPTR wires pages of this task */
    fOpen = true;
    return true;
}

IOReturn RDNA3UserClient::clientClose()
{
    if (fOpen) {
        fAccel->drmClose(fFile);
        fOpen = false;
    }
    if (fFile) {
        IOFree(fFile, sizeof(*fFile));
        fFile = nullptr;
    }
    if (!isInactive())
        terminate();
    return kIOReturnSuccess;
}

void RDNA3UserClient::stop(IOService *provider)
{
    if (fOpen) {
        fAccel->drmClose(fFile);
        fOpen = false;
    }
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
    if (!fOpen)
        return kIOReturnNotOpen;
    uint64_t off = (uint64_t)type << 12;
    IOMemoryDescriptor *md = fAccel->drmMemoryForOffset(fFile, off, options);
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
