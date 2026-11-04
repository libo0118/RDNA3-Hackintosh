#!/usr/bin/env python3
"""Run shared ASIC publishing, real parsers/UAPI, and early-init/enum guards."""
import os
import pathlib
import re
import shlex
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
bringup = (root / "kext/src/RDNA3Bringup.cpp").read_text(encoding="utf-8")
drm = (root / "kext/src/RDNA3Drm.cpp").read_text(encoding="utf-8")
accel = (root / "kext/src/RDNA3Accel.cpp").read_text(encoding="utf-8")
parser = (root / "hw/vbios.c").read_text(encoding="utf-8")


def method(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2] + "\n"


start = bringup.index("    /* the VBIOS: boot-up clocks and the PMFW BoardTable */")
vbios = bringup[start:bringup.index("    /* smu_smc_hw_setup's pcie_gen", start)]
pm_guard, = re.findall(r"        if \(fPmMode\)\n            pmInit\(\);", bringup)
# Keep the actual initialization/publication order and the whole enum guard.
# Heap allocation and MMIO mapping before this span are OS-specific, not metadata logic.
start = drm.index("    rdna3_drm_device_init(fDrm,")
drm_init = drm[start:drm.index('    LOG("DRM/amdgpu UAPI ready:', start)]
shared = method(drm, "void RDNA3Accel::drmSetAsicInfo(") + method(drm, "void RDNA3Accel::drmPublishVbiosMetadata(")
start_method = method(accel, "bool RDNA3Accel::start(")
display = (root / "kext/src/RDNA3Display.cpp").read_text(encoding="utf-8")
start = display.index("    fDispRom = (uint8_t *)IOMalloc(", display.index("void RDNA3Accel::displayReadVbios()"))
display_allocations = display[start:display.index("    ret = vbios_read(", start)]
parsers = (parser[parser.index("static uint16_t rd16("):parser.index("static int locate(")] +
           method(parser, "const uint8_t *vbios_data_table(") +
           method(parser, "int vbios_clock_info(") +
           method(parser, "static uint32_t atom_vram_type(") +
           method(parser, "int vbios_vram_info("))

harness = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>
#include "vbios.h"
#include "smu13_0_7.h"
#include "gfx11_info.h"
#include "rdna3_drm.h"
#define bzero(p, n) memset(p, 0, n)
static std::vector<std::string> logs, events;
static void note(const char *format, ...) {
    char text[512];
    va_list args;
    va_start(args, format); vsnprintf(text, sizeof(text), format, args); va_end(args);
    logs.emplace_back(text);
}
#define BLOG(...) note(__VA_ARGS__)
#define LOG(...) note(__VA_ARGS__)
static bool logged(const char *text) {
    for (const auto &line : logs) if (line.find(text) != std::string::npos) return true;
    return false;
}
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++) p[i] = v >> (8 * i);
}
// Only invented tables, never a captured ROM. The legacy table has four distinct modules.
struct Image {
    uint8_t data[1024]{};
    rdna3_vbios vb{};
    Image(bool legacy = false) {
        vb.data = data; vb.len = sizeof(data); vb.data_table = 0x20;
        put16(data + 0x24 + 2 * RD_ATOM_DT_FIRMWAREINFO, 0x100);
        put16(data + 0x100, 88); data[0x102] = 3; data[0x103] = 4;
        put32(data + 0x118, 0x100); // scratch base, module selected by a real rreg call
        put16(data + 0x24 + 2 * RD_ATOM_DT_GFX_INFO, 0x180);
        put16(data + 0x180, 44); data[0x182] = 3; put32(data + 0x1a8, 2700);
        put16(data + 0x24 + 2 * RD_ATOM_DT_VRAM_INFO, 0x200);
        if (legacy) {
            put16(data + 0x200, 24 + 4 * 32); data[0x202] = 2; data[0x203] = 4;
            data[0x214] = 4;
            for (unsigned i = 0; i < 4; i++) {
                uint8_t *m = data + 0x218 + 32 * i;
                put16(m + 20, 32); m[23] = i ? 0x70 : 0x50;
                m[24] = i ? 8 : 16; m[25] = 4; m[28] = 6;
            }
        } else {
            put16(data + 0x200, 68); data[0x202] = 3;
            data[0x218] = 0x70; data[0x219] = 8; data[0x243] = 6;
        }
    }
};
static Image *read_image;
static int read_result, boot_result, legacy_boot, navi33_boot, scratch_reads, cu_reads, tcc_reads;
static uint32_t selected_module = 3, exact, enum_only, pm_mode;
int vbios_read(rdna3_dev *d, rdna3_vbios *v, uint8_t *rom, uint32_t cap) {
    assert(d && read_image && cap == RD_VBIOS_MAX_LEN);
    *v = read_image->vb; memcpy(rom, v->data, v->len); v->data = rom; return read_result;
}
void vbios_strings(const rdna3_vbios *v, rdna3_vbios_strings *s) {
    assert(v); memset(s, 0, sizeof(*s));
}
int vbios_bootup_values(rdna3_dev *d, const rdna3_vbios *v, rdna3_vbios_boot *b) {
    assert(d && v); legacy_boot++;
    memset(b, 0, sizeof(*b));
    b->firmware_capability = RD_ATOM_FW_CAP_2STAGE_BIST_TRAINING | RD_ATOM_FW_CAP_DYNAMIC_BOOT_CFG;
    return boot_result;
}
int smu13_0_7_bootup_values(rdna3_dev *d, const rdna3_vbios *v, rdna3_vbios_boot *b) {
    navi33_boot++; return vbios_bootup_values(d, v, b);
}
uint32_t rdna3_rreg(rdna3_dev *d, uint32_t reg) {
    assert(d && reg == 0x104); scratch_reads++; return selected_module << 16;
}
int gfx11_get_cu_info(rdna3_dev *d, rdna3_gfx_cu_info *ci) {
    assert(d); cu_reads++; ci->cu_active = 32; ci->rb_active = 15; return RDNA3_OK;
}
uint32_t gfx11_get_tcc_info(rdna3_dev *d) { assert(d); tcc_reads++; return 0x12; }
static bool PE_parse_boot_argn(const char *name, void *value, int size) {
    assert(size == 4);
    if (!strcmp(name, "rdna3amdgpu")) *(uint32_t *)value = exact;
    else if (!strcmp(name, "rdna3enum")) *(uint32_t *)value = enum_only;
    else return false;
    return true;
}
static int copyout(void *, uint64_t address, const void *data, size_t size) {
    memcpy((void *)(uintptr_t)address, data, size); return 0;
}
static rdna3_drm_ops test_ops() { rdna3_drm_ops ops{}; ops.copyout = copyout; return ops; }
static int query(rdna3_drm_device *dev, drm_amdgpu_info_device *out) {
    rdna3_drm_file file{}; file.dev = dev;
    drm_amdgpu_info in{};
    in.query = AMDGPU_INFO_DEV_INFO; in.return_pointer = (uintptr_t)out; in.return_size = sizeof(*out);
    return rdna3_drm_ioctl(&file, RD_NR_AMDGPU(DRM_AMDGPU_INFO), &in, sizeof(in));
}
static void rejected(rdna3_drm_device *dev) {
    drm_amdgpu_info_device out, untouched;
    memset(&untouched, 0xa5, sizeof(untouched)); out = untouched;
    assert(query(dev, &out) == -RD_ENODEV && !memcmp(&out, &untouched, sizeof(out)));
}
struct IOService {
    bool start(IOService *) { return true; }
    void joinPMtree(IOService *) {}
};
struct IOPCIDevice : IOService {
    void retain() {}
    uint16_t configRead16(int) { return 0x7480; }
};
struct IOPMPowerState { unsigned long a, b, c, d, e, f, g, h, i, j, k, l; };
struct IOPMrootDomain {
    uint64_t createPMAssertion(int, int, IOService *, const char *) { return 1; }
};
static IOPMrootDomain *getPMRootDomain() { return nullptr; }
enum { kIOPCIConfigDeviceID, kIOPMDriverAssertionPreventSystemIdleSleepBit,
       kIOPMDriverAssertionLevelOn, kIOPMPowerStateVersion1, kIOPMPowerOn, kIOReturnSuccess };
#define KEXT_COMMIT "test"
#define KEXT_VERSION "test"
#define KEXT_SRC_HASH "test"
#define OSDynamicCast(type, value) static_cast<type *>(value)
#define super IOService
static unsigned allocation_number = 0, fail_allocation = 0;
static void *IOMalloc(size_t size) {
    if (!fail_allocation) return calloc(1, size);
    if (++allocation_number == fail_allocation) return nullptr;
    void *data = malloc(size);
    assert(data); memset(data, 0xa5, size); return data;
}
static void IOFree(void *data, size_t) { free(data); }
static uint32_t rd3_arg(const char *name, uint32_t) {
    if (!strcmp(name, "rdna3lvl")) return 8;
    if (!strcmp(name, "rdna3pm")) return pm_mode;
    return 0;
}
struct RDNA3Bringup;
struct RDNA3Accel : IOService {
    rdna3_drm_device state{};
    rdna3_drm_device *fDrm = &state;
    void *fDrmLock = this, *fMmioMap = nullptr;
    IOPCIDevice *fPci = nullptr;
    rdna3_dev device{};
    amd_discovery discovery{};
    Image *cached = nullptr;
    rdna3_vbios *fDispVbios = nullptr;
    uint8_t *fDispRom = nullptr;
    rdna3_display_path *fPaths = nullptr;
    void readDisplayVbiosAllocations();
    rdna3_dev *fDispDev = nullptr;
    bool registers = true, display_ok = true, fDiscSocOk = false, fPmJoined = false;
    amd_disc_soc fDiscSoc{};
    uint32_t fLevel = 0, images = 0, fFwVer[32]{}, fFwFeat[32]{}, fTaVer[RD_TA_TYPES]{};
    uint8_t *fVbiosCopy = nullptr, *fMmio = nullptr;
    uint32_t fVbiosCopyLen = 0, fMmioLen = 0;
    drm_amdgpu_info_vbios fVbiosInfo{};
    uint64_t fVramSize = 16ull << 30, fSleepAssertion = 0;
    RDNA3Bringup *fBringup = nullptr;
    int first_query = -1;
    uint32_t first_ready = 0;
    std::map<std::string, uint64_t> values;
    std::map<std::string, std::string> strings;
    RDNA3Accel() {
        discovery.has_gc_info = 1; discovery.gc.num_se = 2;
        discovery.gc.num_sa_per_se = 2; discovery.gc.num_wgp0_per_sa = 4;
        discovery.num_ips = 1; discovery.ips[0].hw_id = AMD_HWID_GC;
        discovery.ips[0].major = 11; discovery.ips[0].revision = 2;
        rdna3_drm_ops ops = test_ops();
        rdna3_drm_device_init(fDrm, &ops, &discovery, 0x7480, 0xc0, 0, fVramSize, 256ull << 20, 8ull << 30);
    }
    void setNumber(const char *key, uint64_t value, unsigned = 64) { values[key] = value; }
    void setProperty(const char *key, uint64_t value, int) { values[key] = value; }
    void setProperty(const char *key, const char *value) { strings[key] = value; }
    void drmSetAsicInfo(uint32_t, uint32_t, uint32_t, uint64_t);
    void drmPublishVbiosMetadata(rdna3_dev *, const rdna3_vbios *);
    void drmSetVbios(const uint8_t *image, uint32_t len, const drm_amdgpu_info_vbios *info) {
        assert(image && len && info); images++;
    }
    bool start(IOService *);
    bool drmInit(const amd_discovery *);
    bool displayInit(const amd_discovery *) {
        events.emplace_back("display"); fDispVbios = cached ? &cached->vb : nullptr;
        fDispDev = registers ? &device : nullptr; return display_ok;
    }
    bool platformInit() {
        events.emplace_back("platform"); drm_amdgpu_info_device out{};
        first_query = query(fDrm, &out); first_ready = fDrm->accel_working; return true;
    }
    bool readDiscovery(amd_discovery *disc) { *disc = discovery; return true; }
    void readConfigSpace() {}
    bool isInD0() { return true; }
    bool mapMmio() { return true; }
    bool waitForIfwi() { return true; }
    bool readVramSize() { return true; }
    void readGcStatus(const amd_discovery *) {}
    void statsStart() {}
    void PMinit() {}
    int registerPowerDriver(IOService *, IOPMPowerState *, unsigned) { return kIOReturnSuccess; }
    void registerService() { events.emplace_back("registered"); }
};
struct RD3DrmScope { explicit RD3DrmScope(RDNA3Accel *) {} };
struct RDNA3Bringup {
    RDNA3Accel *fOwner = nullptr;
    rdna3_dev *fDev = nullptr;
    bool fAmdgpuExact = false;
    uint32_t fPmMode = 0;
    rdna3_vbios fVbios{};
    uint8_t rom[1024]{};
    uint8_t *fRom = rom;
    bool init(RDNA3Accel *owner, IOPCIDevice *, void *, uint32_t, const amd_discovery *, uint64_t) {
        fOwner = owner; fDev = &owner->device; fAmdgpuExact = exact; fPmMode = rd3_arg("rdna3pm", 0); return true;
    }
    void start() { events.emplace_back("async"); } // scheduled, not yet executed
    void stopAndFree() {}
    void publish(const char *key, uint64_t value) { fOwner->setNumber(key, value); }
    void publishString(const char *key, const char *value) { fOwner->setProperty(key, value); }
    void pmInit();
    void runPm();
};
'''

main = r'''
int main() {
    Image global, legacy(true); read_image = &global;
    IOPCIDevice pci;
    // Exercise the real allocation/early-return prefix with poisoned successful allocations.
    for (unsigned fail : {1u, 2u, 3u, 4u}) {
        RDNA3Accel owner; allocation_number = 0; fail_allocation = fail;
        owner.readDisplayVbiosAllocations();
        assert(allocation_number == 3);
        if (owner.fDispVbios) {
            const uint8_t empty[sizeof(rdna3_vbios)] = {};
            assert(!memcmp(owner.fDispVbios, empty, sizeof(empty)));
        }
        owner.drmPublishVbiosMetadata(&owner.device, owner.fDispVbios);
        rejected(owner.fDrm);
        free(owner.fDispRom); free(owner.fDispVbios); free(owner.fPaths);
    }
    fail_allocation = 0;
    // Full production start(), its real DRM initialization span and the real enum guard:
    // the first consumer sees metadata before any asynchronous PM, even with rdna3pm=0.
    for (uint32_t profile : {0u, 1u}) for (uint32_t pm : {0u, 1u})
        for (uint32_t enumeration : {0u, 1u}) {
            exact = profile; pm_mode = pm; enum_only = enumeration;
            RDNA3Accel owner; owner.cached = &global;
            events.clear(); legacy_boot = navi33_boot = 0;
            assert(owner.start(&pci));
            assert((events == std::vector<std::string>{"display", "drm", "platform", "async", "registered"}));
            assert(owner.first_query == 0 && owner.first_ready == enumeration);
            assert(owner.state.dev_info.gpu_counter_freq == 27000 && owner.state.dev_info.vram_bit_width == 128);
            assert(!legacy_boot && !navi33_boot);
            owner.fBringup->runPm();
            assert(owner.images == pm && legacy_boot == (pm && profile));
            delete owner.fBringup;
        }
    // Real early initialization refuses only synthetic readiness, not later hardware readiness.
    enum_only = 1;
    for (int failure : {0, 1, 2, 3, 4, 5, 6}) {
        Image bad;
        if (failure == 1) bad.data[0x218] = 0;
        if (failure == 2) put32(bad.data + 0x1a8, 0);
        if (failure == 4) bad.vb.data_table = 0;
        if (failure == 5) bad.data[0x182] = 1;
        if (failure == 6) bad.data[0x219] = 0;
        RDNA3Accel owner; owner.cached = failure == 0 ? nullptr : &bad;
        if (failure == 3) { owner.discovery.ips[0].revision = 3; owner.discovery.gc.num_se = 3; }
        logs.clear(); assert(owner.start(&pci));
        assert(owner.first_query == -RD_ENODEV && !owner.first_ready && !owner.state.enumeration_only);
        assert(logged("rdna3enum=1: refused: incomplete ASIC metadata"));
        rejected(owner.fDrm);
        owner.drmSetAsicInfo(0, 0, 0, 64ull << 20);
        owner.drmPublishVbiosMetadata(&owner.device, &global.vb);
        drm_amdgpu_info_device out{};
        assert(query(owner.fDrm, &out) == 0 && !owner.state.accel_working && !owner.state.enumeration_only);
        rdna3_drm_device_set_ring_ready(owner.fDrm, AMDGPU_HW_IP_GFX, 1);
        assert(owner.state.accel_working); // unchanged hardware-ready policy
        delete owner.fBringup;
    }
    enum_only = 0;
    // Both exact modes and both boot parsers reuse the same real publishing method.
    for (uint8_t revision : {10, 7}) for (bool profile : {false, true}) {
        RDNA3Accel owner; owner.device.ipver[HWIP_MP1].rev = revision;
        RDNA3Bringup b; b.fOwner = &owner; b.fDev = &owner.device; b.fAmdgpuExact = profile;
        read_image = &legacy; read_result = boot_result = RDNA3_OK;
        logs.clear(); legacy_boot = navi33_boot = scratch_reads = 0; b.pmInit();
        assert(scratch_reads == 1 && owner.images == 1);
        assert(owner.state.dev_info.vram_type == AMDGPU_VRAM_TYPE_GDDR6 && owner.state.dev_info.vram_bit_width == 128);
        assert(owner.values.at("RDNA3,VRAMVendor") == 6);
        assert(owner.strings.at("RDNA3,VBIOSMetadata") == "current");
        assert(legacy_boot == profile && navi33_boot == (profile && revision == 7));
        assert(logged("two-stage memory training") == profile && logged("dynamic boot config") == profile);
        read_result = RDNA3_EINVAL; logs.clear(); b.pmInit();
        assert(owner.images == 1 && owner.state.dev_info.vram_bit_width == 128);
        assert(owner.strings.at("RDNA3,VBIOSMetadata").find("read failed") != std::string::npos);
        assert(logged("prior or unknown DEV_INFO metadata retained"));
        read_result = RDNA3_OK; boot_result = RDNA3_EINVAL; scratch_reads = 0;
        b.pmInit();
        assert(scratch_reads == 1 && owner.strings.at("RDNA3,VBIOSMetadata") == "current");
    }
    read_result = RDNA3_OK;
    // Boot parsing can succeed without a scratch address: do not choose legacy module 0.
    for (bool v3 : {false, true}) {
        Image image(!v3); put32(image.data + 0x118, 0); read_image = &image;
        RDNA3Accel owner; RDNA3Bringup b;
        b.fOwner = &owner; b.fDev = &owner.device; b.fAmdgpuExact = true;
        boot_result = RDNA3_OK; legacy_boot = scratch_reads = 0; logs.clear(); b.pmInit();
        assert(legacy_boot == 1 && scratch_reads == 0 && logged("BIOS scratch module selection unavailable"));
        if (v3) {
            drm_amdgpu_info_device out{};
            assert(query(owner.fDrm, &out) == 0 && out.vram_bit_width == 128);
        } else {
            assert(!owner.state.dev_info.vram_type && !owner.state.dev_info.vram_bit_width);
            assert(!owner.values.count("RDNA3,VRAMType")); rejected(owner.fDrm);
            put32(image.data + 0x118, 0x100);
            owner.drmPublishVbiosMetadata(&owner.device, &image.vb);
            put32(image.data + 0x118, 0);
            owner.drmPublishVbiosMetadata(&owner.device, &image.vb);
            drm_amdgpu_info_device out{};
            assert(query(owner.fDrm, &out) == 0 && out.vram_bit_width == 128);
            assert(owner.strings.at("RDNA3,VBIOSMetadata").find("incomplete") != std::string::npos);
        }
    }
    // A module 0 actually read from scratch remains legitimate, unlike an assumed 0.
    {
        RDNA3Accel owner; selected_module = 0;
        owner.drmPublishVbiosMetadata(&owner.device, &legacy.vb);
        drm_amdgpu_info_device out{};
        assert(query(owner.fDrm, &out) == 0 && out.vram_type == AMDGPU_VRAM_TYPE_GDDR5 && out.vram_bit_width == 256);
        selected_module = 3;
    }
    // Invalid/absent scratch metadata and register view likewise cannot select an old module.
    for (int failure : {0, 1, 2}) {
        Image image(true);
        if (failure == 0) put16(image.data + 0x100, 24);
        if (failure == 1) image.data[0x102] = 2;
        RDNA3Accel owner; scratch_reads = 0;
        owner.drmPublishVbiosMetadata(failure == 2 ? nullptr : &owner.device, &image.vb);
        assert(!scratch_reads && !owner.state.dev_info.vram_type); rejected(owner.fDrm);
    }
    // Clock parser failure and successful parsing with a zero reference both reject DEV_INFO.
    for (bool parser_error : {false, true}) {
        Image image;
        if (parser_error) image.data[0x182] = 1;
        else put32(image.data + 0x1a8, 0);
        RDNA3Accel owner; owner.drmPublishVbiosMetadata(&owner.device, &image.vb);
        assert(owner.state.dev_info.vram_bit_width == 128 && !owner.state.dev_info.gpu_counter_freq);
        rejected(owner.fDrm);
        owner.drmPublishVbiosMetadata(&owner.device, &global.vb);
        drm_amdgpu_info_device out{};
        assert(query(owner.fDrm, &out) == 0 && out.gpu_counter_freq == 27000);
        owner.drmPublishVbiosMetadata(&owner.device, &image.vb);
        assert(query(owner.fDrm, &out) == 0 && out.gpu_counter_freq == 27000);
        assert(owner.strings.at("RDNA3,VBIOSMetadata").find("incomplete") != std::string::npos);
        put32(global.data + 0x1a8, 10000);
        owner.drmPublishVbiosMetadata(&owner.device, &global.vb);
        assert(query(owner.fDrm, &out) == 0 && out.gpu_counter_freq == 100000);
        put32(global.data + 0x1a8, 2700);
        owner.drmPublishVbiosMetadata(nullptr, nullptr);
        assert(query(owner.fDrm, &out) == 0 && out.gpu_counter_freq == 100000);
    }
    // A failed VRAM parse cannot overwrite the last known type/width either.
    {
        Image bad; bad.data[0x203] = 1;
        RDNA3Accel owner; owner.drmPublishVbiosMetadata(&owner.device, &bad.vb); rejected(owner.fDrm);
        owner.drmPublishVbiosMetadata(&owner.device, &global.vb);
        owner.drmPublishVbiosMetadata(&owner.device, &bad.vb);
        drm_amdgpu_info_device out{};
        assert(query(owner.fDrm, &out) == 0 && out.vram_bit_width == 128);
    }
    // MALL is published independently of level/register/GC/exact; TCC keeps its real guard.
    for (uint32_t level : {0u, 3u}) for (bool gc : {false, true}) for (bool registers : {false, true})
        for (uint32_t profile : {0u, 1u}) {
            RDNA3Accel owner; owner.cached = &global; owner.fDispVbios = &global.vb;
            owner.fLevel = level; owner.fDispDev = registers ? &owner.device : nullptr;
            owner.fDiscSocOk = true; owner.fDiscSoc.mall_size = 48ull << 20;
            owner.discovery.has_gc_info = gc; exact = profile; cu_reads = tcc_reads = 0;
            owner.fPci = &pci; assert(owner.drmInit(&owner.discovery));
            assert(owner.state.dev_info.mall_size == (48ull << 20) && owner.state.dev_info.vram_bit_width == 128);
            const bool harvest = level >= 3 && gc && registers;
            assert(cu_reads == harvest && tcc_reads == (harvest && profile));
            assert(owner.state.dev_info.tcc_disabled_mask == (harvest && profile ? 0x12 : 0));
        }
    for (bool discovery : {false, true}) {
        RDNA3Accel owner; owner.fDiscSocOk = discovery; owner.fPci = &pci;
        logs.clear(); assert(owner.drmInit(&owner.discovery));
        assert(!owner.state.dev_info.mall_size && logged("MALL size unknown"));
    }
}
'''

cpp_source = (harness + parsers + shared +
              "\nvoid RDNA3Accel::readDisplayVbiosAllocations() {\n" + display_allocations + "}\n" +
              "\nbool RDNA3Accel::drmInit(const amd_discovery *disc) {\n"
              '    events.emplace_back("drm");\n'
              "    rdna3_drm_ops ops = test_ops();\n"
              "    uint8_t rev = 0xc0; uint32_t revId = 0; uint64_t visible = 256ull << 20, gtt = 8ull << 30;\n" +
              drm_init + "    return true;\n}\n" + start_method +
              "\nvoid RDNA3Bringup::pmInit() {\n    int ret; const rdna3_vbios *vb = nullptr;\n" + vbios +
              "    assert(vb == (ret == RDNA3_OK ? &fVbios : nullptr));\n}\n" +
              "\nvoid RDNA3Bringup::runPm() {\n" + pm_guard + "\n}\n" + main)

if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="rdna3-asic-metadata-") as directory:
        path = pathlib.Path(directory)
        cpp, binary = path / "test.cpp", path / "test"
        cpp.write_text(cpp_source, encoding="utf-8")
        includes = ["-I" + str(root / d) for d in ("hw", "common", "uapi", "third_party/amd")]
        sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-sanitize-recover=all"]
        objects = []
        for source in ("uapi/rdna3_drm.c", "common/amd_discovery.c", "common/amd_fw.c", "common/range_alloc.c"):
            obj = path / (pathlib.Path(source).stem + ".o")
            subprocess.run(shlex.split(os.environ.get("CC", "clang")) +
                           ["-std=c99", "-Wall", "-Wextra", "-Werror", *sanitize, *includes,
                            "-c", str(root / source), "-o", str(obj)], check=True)
            objects.append(str(obj))
        subprocess.run(shlex.split(os.environ.get("CXX", "clang++")) +
                       ["-std=c++17", "-Wall", "-Wextra", "-Werror", *sanitize, *includes,
                        str(cpp), *objects, "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("test_asic_metadata: ok")
