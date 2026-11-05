#!/usr/bin/env python3
"""Test the exact patched IRQ predicate; no device or driver is loaded."""
import pathlib
import re
import subprocess
import sys
import tempfile


def check_source(source):
    cpp = (source / 'AppleIGC/AppleIGC.cpp').read_text()
    hpp = (source / 'AppleIGC/AppleIGC.hpp').read_text()
    predicate = re.search(r'static bool igcProbeLegacyTypeOK\(IOReturn status, int interruptType\)\n\{[^}]+\}', cpp)
    assert predicate, 'missing actual predicate'
    irq = cpp.split('void AppleIGC::interruptOccurred(', 1)[1].split('void AppleIGC::interruptHandler(', 1)[0]
    assert '++diagIrqCount;' in irq and '++diagNotMineCount;' in irq and '++diagDisabledCount;' in irq
    assert 'setProperty(' not in irq, 'must not allocate properties inside the IRQ callback'
    start = cpp.split('bool AppleIGC::initEventSources(', 1)[1].split('#define kNameLength', 1)[0]
    query = 'probeTypeStatus = provider->getInterruptType(0, &interruptType);'
    guard = 'if (!igcProbeLegacyTypeOK(probeTypeStatus, interruptType))'
    registration = 'IOInterruptEventSource::interruptEventSource(this,&AppleIGC::interruptHandler,provider,interruptIndex)'
    assert start.index(query) < start.index(guard) < start.index(registration)
    assert 'usingMsi = true' not in start and 'int interruptIndex = 0;' in start
    assert 'IGCDiag_Failure' in start and 'goto error1;' in start.split(guard, 1)[1].split(registration, 1)[0]
    assert 'UInt64 diagIrqCount;' in hpp and 'void publishInterruptDiagnostics();' in hpp
    publisher = cpp.split('void AppleIGC::publishInterruptDiagnostics()', 1)[1].split('// corresponds to igb_watchdog_task', 1)[0]
    assert all(name in publisher for name in ['IGCDiag_IrqCount', 'IGCDiag_NotMine', 'IGCDiag_OnDisabled', 'IGCDiag_LastICR'])
    watchdog = cpp.split('void AppleIGC::watchdogTask()', 1)[1].split('void AppleIGC::watchdogHandler(', 1)[0]
    assert watchdog.index('if (test_bit(__IGC_DOWN') < watchdog.index('IGCDiag_PCICommand')
    assert 'configWrite' not in publisher + watchdog, 'diagnostic PCI state must be read-only'
    print('PASS: source guard/order, IRQ counters, allocation and active-device contracts')
    return predicate.group(0)


def main():
    source = pathlib.Path(sys.argv[1])
    predicate = check_source(source)
    header = (source / 'MacKernelSDK/Headers/IOKit/pci/IOPCIDevice.h').read_text()
    assert re.search(r'kIOInterruptTypePCIMessaged\s*=\s*0x00010000', header)
    program = '''#include <cassert>
#include <cstdio>
typedef unsigned int IOReturn;
const IOReturn kIOReturnSuccess = 0;
const int kIOInterruptTypePCIMessaged = 0x00010000;
''' + predicate + '''
int main() {
    assert(igcProbeLegacyTypeOK(0, 0));
    assert(igcProbeLegacyTypeOK(0, 1));
    assert(!igcProbeLegacyTypeOK(0, 0x00010000));
    assert(!igcProbeLegacyTypeOK(0, 0x00010001));
    assert(!igcProbeLegacyTypeOK(0xe00002c2U, 0));
    assert(!igcProbeLegacyTypeOK(0xe00002c2U, 0x00010000));
    std::puts("PASS: 6 exact IRQ predicate cases (not hardware tests)");
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        src = pathlib.Path(tmp) / 'irq-policy.cpp'
        exe = pathlib.Path(tmp) / 'irq-policy'
        src.write_text(program)
        subprocess.run(['clang++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-pedantic', str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
