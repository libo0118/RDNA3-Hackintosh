#!/usr/bin/env python3
"""Test the actual pre-start IRQ guard; no device or driver is loaded."""
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

    start = cpp.split('bool AppleIGC::start(IOService *provider) {', 1)[1].split(
        'IOReturn AppleIGC::enable(', 1)[0]
    prefix, after_log = start.split('pr_err("start()\\n");', 1)
    assert re.sub(r'//[^\n]*', '', prefix).strip() == 'u32 i;'
    prelude, after_super = after_log.split('    if (super::start(provider) == false) {', 1)
    query = 'provider->getInterruptType(0, &interruptType)'
    guard = 'if (!igcProbeLegacyTypeOK(probeTypeStatus, interruptType))'
    assert prelude.index(query) < prelude.index(guard) < prelude.index('diagInterruptType = interruptType;')
    assert 'return false;' in prelude.split(guard, 1)[1].split('diagInterruptType = interruptType;', 1)[0]
    assert 'status=0x%x type=0x%x' in prelude
    assert all(name not in prelude for name in ('->retain(', '->open(', 'getWorkLoop(', 'createWorkLoop(', 'igc_probe(', 'setProperty('))
    assert after_super.index('pdev->retain()') < after_super.index('pdev->open(this)') < after_super.index('initEventSources(provider)') < after_super.index('igc_probe()')
    assert 'getInterruptType(' not in after_super

    init = cpp.split('bool AppleIGC::init(OSDictionary *properties)', 1)[1].split('void AppleIGC::igc_remove()', 1)[0]
    assert 'diagInterruptType = 0;' in init and 'int diagInterruptType;' in hpp
    events = cpp.split('bool AppleIGC::initEventSources(', 1)[1].split('#define kNameLength', 1)[0]
    registration = 'IOInterruptEventSource::interruptEventSource(this,&AppleIGC::interruptHandler,provider,interruptIndex)'
    assert 'int interruptIndex = 0;' in events and registration in events
    assert 'usingMsi = false;' in events and 'usingMsi = true' not in events
    assert 'setProperty("IGCDiag_IrqType", diagInterruptType, 32);' in events
    assert 'getInterruptType(' not in events and 'igcProbeLegacyTypeOK(' not in events
    irq = cpp.split('void AppleIGC::interruptOccurred(', 1)[1].split('void AppleIGC::interruptHandler(', 1)[0]
    assert '++diagIrqCount;' in irq and '++diagNotMineCount;' in irq and '++diagDisabledCount;' in irq
    assert 'setProperty(' not in irq, 'must not allocate properties inside the IRQ callback'
    assert 'UInt64 diagIrqCount;' in hpp and 'void publishInterruptDiagnostics();' in hpp
    publisher = cpp.split('void AppleIGC::publishInterruptDiagnostics()', 1)[1].split('// corresponds to igb_watchdog_task', 1)[0]
    assert all(name in publisher for name in ['IGCDiag_IrqCount', 'IGCDiag_NotMine', 'IGCDiag_OnDisabled', 'IGCDiag_LastICR'])
    watchdog = cpp.split('void AppleIGC::watchdogTask()', 1)[1].split('void AppleIGC::watchdogHandler(', 1)[0]
    assert 'publishInterruptDiagnostics();' in watchdog
    down_guard = watchdog.index('if (test_bit(__IGC_DOWN')
    down_return = watchdog.index('return;', down_guard)
    assert all(down_return < watchdog.index(name) for name in ('IGCDiag_PCICommand', 'configRead16(', 'findPCICapability('))
    assert 'configWrite' not in publisher + watchdog, 'diagnostic PCI state must be read-only'
    print('PASS: pre-start guard/order, cached source 0, IRQ counters and active-device contracts')
    return predicate.group(0), prelude


def main():
    source = pathlib.Path(sys.argv[1])
    predicate, prelude = check_source(source)
    header = (source / 'MacKernelSDK/Headers/IOKit/pci/IOPCIDevice.h').read_text()
    assert re.search(r'kIOInterruptTypePCIMessaged\s*=\s*0x00010000', header)
    program = '''#include <cassert>
#include <cstdio>
#include <cstring>
typedef unsigned int IOReturn;
const IOReturn kIOReturnSuccess = 0;
const int kIOInterruptTypePCIMessaged = 0x00010000;
''' + predicate + '''
static int logCount;
static unsigned int loggedStatus, loggedType;
static void pr_err(const char *format, unsigned int status, unsigned int type) {
    assert(std::strstr(format, "status=0x%x type=0x%x"));
    ++logCount;
    loggedStatus = status;
    loggedType = type;
}
struct Provider {
    IOReturn status;
    int type;
    bool writesType;
    int queries, retains, opens;
    IOReturn getInterruptType(int index, int *out) {
        assert(index == 0);
        ++queries;
        if (writesType) *out = type;
        return status;
    }
    void retain() { ++retains; }
    void open() { ++opens; }
};
struct Harness {
    int diagInterruptType;
    int baseStarts, workLoops, probes;
    bool start(Provider *provider) {
''' + prelude + '''
        ++baseStarts;
        ++workLoops;
        provider->retain();
        provider->open();
        ++probes;
        return true;
    }
};
static void run_case(IOReturn status, int type, bool writesType, bool accepted) {
    Provider p = {status, type, writesType, 0, 0, 0};
    Harness h = {-1, 0, 0, 0};
    logCount = 0;
    assert(h.start(&p) == accepted);
    assert(p.queries == 1);
    if (accepted) {
        assert(h.diagInterruptType == type);
        assert(h.baseStarts == 1 && h.workLoops == 1 && h.probes == 1);
        assert(p.retains == 1 && p.opens == 1 && logCount == 0);
    } else {
        assert(h.diagInterruptType == -1);
        assert(h.baseStarts == 0 && h.workLoops == 0 && h.probes == 0);
        assert(p.retains == 0 && p.opens == 0 && logCount == 1);
        assert(loggedStatus == status && loggedType == (unsigned int)(writesType ? type : 0));
    }
}
int main() {
    run_case(0, 0, true, true);
    run_case(0, 1, true, true);
    run_case(0, 0x00010000, true, false);
    run_case(0, 0x00010001, true, false);
    run_case(0xe00002c2U, 0, true, false);
    run_case(0xe00002c2U, 0x00010000, true, false);
    run_case(0xe00002c2U, 0x00010000, false, false);
    std::puts("PASS: 7 actual pre-start cases (not hardware tests)");
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
