/*
 * From the NullMoth NVIDIA driver for macOS (kexts/NVRMAGDC/AppleGraphicsDeviceControl_re.h)
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Apple's AppleGraphicsDeviceControl base class (AppleGraphicsControl.kext's
 * AppleGraphicsDeviceControl plugin), reconstructed: no SDK header exists.
 * RDNA3AGDC checks the running class's size against this one before it
 * makes an object (rdna3-agdc.cpp).
 */

#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
struct AGDCClientState_t;
struct IOExternalMethodArguments;
class AppleGraphicsDeviceControl : public IOService {
    OSDeclareDefaultStructors(AppleGraphicsDeviceControl)
    uint8_t _agdc_private[0x110 - sizeof(IOService)];
public:
    virtual bool     start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void     stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual IOReturn message(UInt32 type, IOService *provider, void *argument = 0) APPLE_KEXT_OVERRIDE;
    virtual bool     terminate(IOOptionBits options = 0) APPLE_KEXT_OVERRIDE;
    virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler) APPLE_KEXT_OVERRIDE;
    virtual IOReturn vendor_doDeviceAttribute(unsigned int cmd, unsigned long *in, unsigned long inSize,
                                              unsigned long *out, unsigned long *outSize, IOExternalMethodArguments *args) = 0;
    virtual IOReturn vendor_doDeviceAttribute(unsigned int cmd, unsigned long *in, unsigned long inSize,
                                              unsigned long *out, unsigned long *outSize, AGDCClientState_t *state);
    virtual void _RESERVEDAppleGraphicsDeviceControl0(); virtual void _RESERVEDAppleGraphicsDeviceControl1();
    virtual void _RESERVEDAppleGraphicsDeviceControl2(); virtual void _RESERVEDAppleGraphicsDeviceControl3();
    virtual void _RESERVEDAppleGraphicsDeviceControl4(); virtual void _RESERVEDAppleGraphicsDeviceControl5();
    virtual void _RESERVEDAppleGraphicsDeviceControl6(); virtual void _RESERVEDAppleGraphicsDeviceControl7();
};
static_assert(sizeof(AppleGraphicsDeviceControl) == 0x110, "AppleGraphicsDeviceControl is 0x110 bytes in 15.7.9");
