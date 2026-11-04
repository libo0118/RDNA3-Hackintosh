#!/usr/bin/env python3
"""Execute production GFX suspend and stop's hub cleanup at partial bring-up."""
import os
import pathlib
import re
import shlex
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
source = (root / "kext/src/RDNA3Bringup.cpp").read_text(encoding="utf-8")
start = source.index("void RDNA3Bringup::gfxSuspend()")
suspend = source[start:source.index("void RDNA3Bringup::vmFlush(", start)]
start = source.index("void RDNA3Bringup::stopAndFree()")
stop = source[start:source.index("void RDNA3Bringup::publish(", start)]
stop_guard, = re.findall(
    r"    if \([^\n]*\)\n        gfxhub11_gart_disable\([^;\n]*\);", stop)

harness = r'''
#include <assert.h>
#include <initializer_list>
#include <vector>
#include "gfx11.h"
enum Action { PrivReg, PrivInst, BadOp, Kgq, Mec, Cp, Hub };
static std::vector<Action> actions;
static void expect(std::initializer_list<Action> expected) {
    assert(actions == std::vector<Action>(expected));
    actions.clear();
}
void gfx11_set_priv_reg_fault_state(rdna3_dev *d, int enable) {
    assert(d && !enable); actions.push_back(PrivReg);
}
void gfx11_set_priv_inst_fault_state(rdna3_dev *d, int enable) {
    assert(d && !enable); actions.push_back(PrivInst);
}
void gfx11_set_bad_op_fault_state(rdna3_dev *d, int enable) {
    assert(d && !enable); actions.push_back(BadOp);
}
void gfx11_mec_stop(rdna3_dev *d, rdna3_mec_ring *kiq,
                    rdna3_mec_ring *kcq, rdna3_mes *mes) {
    assert(d && kiq); (void)kcq; (void)mes; actions.push_back(Mec);
}
void gfx11_cp_gfx_enable(rdna3_dev *d, int enable) {
    assert(d && !enable); actions.push_back(Cp);
}
// sim_gfxhub11.inc checks this state transition with the real C and both MMIO layouts.
void gfxhub11_gart_disable(rdna3_dev *d, rdna3_gmc *g) {
    assert(d && g && g->gfxhub_enabled);
    actions.push_back(Hub);
    g->gfxhub_enabled = 0;
}
struct RDNA3Bringup {
    rdna3_dev *fDev = nullptr;
    rdna3_gmc *fGmc = nullptr;
    rdna3_gfx_ring *fGfx = nullptr;
    rdna3_mec_ring *fKiq = nullptr, *fKcq = nullptr;
    rdna3_mes *fMes = nullptr;
    bool fAmdgpuExact = false;
    void gfxSuspend();
    void stopHub();
    void kgqStop() {
        assert(fDev && fGfx && fGfx->async && fGfx->ready && fKiq && fKiq->rb.ready);
        actions.push_back(Kgq);
    }
};
'''
main = r'''
int main() {
    rdna3_dev dev{};
    rdna3_gmc gmc{};
    rdna3_gfx_ring gfx{};
    rdna3_mec_ring kiq{}, kcq{};
    rdna3_mes mes{};
    RDNA3Bringup bringup;
    bringup.fDev = &dev;
    bringup.fGmc = &gmc;
    bringup.fAmdgpuExact = true;

    // Level 7, SDMA failure after hub enable, and level 8 GFX allocation failure.
    assert(!bringup.fGfx);
    gmc.gfxhub_enabled = 1;
    bringup.stopHub();
    expect({Hub});
    assert(!gmc.gfxhub_enabled);
    bringup.stopHub();
    expect({});
    gmc.gfxhub_enabled = 1;
    bringup.gfxSuspend();
    expect({Hub});
    assert(!gmc.gfxhub_enabled);
    bringup.gfxSuspend();
    bringup.stopHub();
    expect({});

    // The non-exact stop policy and uninitialized-device guards do not change.
    bringup.fAmdgpuExact = false;
    gmc.gfxhub_enabled = 1;
    bringup.stopHub();
    expect({});
    assert(gmc.gfxhub_enabled);
    bringup.fAmdgpuExact = true;
    bringup.fDev = nullptr;
    bringup.gfxSuspend();
    bringup.stopHub();
    expect({});
    assert(gmc.gfxhub_enabled);
    bringup.fDev = &dev;
    bringup.fGmc = nullptr;
    bringup.gfxSuspend();
    bringup.stopHub();
    expect({});
    bringup.fGmc = &gmc;

    // A ready async ring still stops in the original order, before the hub.
    bringup.fGfx = &gfx;
    bringup.fKiq = &kiq;
    bringup.fKcq = &kcq;
    bringup.fMes = &mes;
    gfx.async = gfx.ready = kiq.rb.ready = 1;
    bringup.gfxSuspend();
    expect({PrivReg, PrivInst, BadOp, Kgq, Mec, Cp, Hub});
    assert(!gfx.ready && !gmc.gfxhub_enabled);
    bringup.gfxSuspend();
    bringup.stopHub();
    expect({PrivReg, PrivInst, BadOp, Mec, Cp});

    // A partially initialized GFX ring needs no KIQ to turn its hub off.
    bringup.fKiq = bringup.fKcq = nullptr;
    bringup.fMes = nullptr;
    gmc.gfxhub_enabled = 1;
    bringup.gfxSuspend();
    expect({PrivReg, PrivInst, BadOp, Cp, Hub});
    assert(!gfx.ready && !gmc.gfxhub_enabled);
    gfx.async = 0;
    gfx.ready = 1;
    bringup.gfxSuspend();
    expect({PrivReg, PrivInst, BadOp, Cp});
    assert(!gfx.ready);

    // An unready KIQ must not unmap the KGQ; an absent device touches nothing.
    bringup.fKiq = &kiq;
    gfx.async = gfx.ready = 1;
    kiq.rb.ready = 0;
    bringup.gfxSuspend();
    expect({PrivReg, PrivInst, BadOp, Mec, Cp});
    assert(!gfx.ready);
    bringup.fDev = nullptr;
    gfx.ready = gmc.gfxhub_enabled = 1;
    bringup.gfxSuspend();
    bringup.stopHub();
    expect({});
    assert(gfx.ready && gmc.gfxhub_enabled);
    bringup.fDev = &dev;
    bringup.stopHub();
    expect({Hub});
    assert(!gmc.gfxhub_enabled);
}
'''
cpp_source = (harness + suspend + "\nvoid RDNA3Bringup::stopHub() {\n" +
              stop_guard + "\n}\n" + main)

if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="rdna3-gfxhub-lifecycle-") as directory:
        path = pathlib.Path(directory)
        cpp, binary = path / "test.cpp", path / "test"
        cpp.write_text(cpp_source, encoding="utf-8")
        subprocess.run(shlex.split(os.environ.get("CXX", "clang++")) +
                       ["-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(root / "hw"), "-I" + str(root / "common"),
                        str(cpp), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("test_gfxhub_lifecycle: ok")
