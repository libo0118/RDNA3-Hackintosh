#!/usr/bin/env python3
"""The triage's 'VRAM aperture' row (DRIVER-PLAN N62): how big BAR0 was, how much of it the core kext mapped into the kernel
and how big the CPU-visible VRAM heap became, from the core's lines (kext/src/RDNA3Drm.cpp drmInit, RDNA3Bringup::mapVram):

  RDNA3: VRAM aperture: BAR0 1024 MiB at phys 0x80000000, mapped 1024 MiB (kernel window, rdna3vramwin cap 1024 MiB), visible heap 912 MiB (15200 MiB beyond it)
  RDNA3: VRAM aperture: NOT MAPPED: BAR0 0 MiB, mapped 0 MiB, visible heap 0 MiB: BAR0 has no IODeviceMemory from IOPCIFamily (...): <why>
  RDNA3: cannot map BAR0 (VRAM aperture): <why>

Card run 2026-10-28c (BAR0 1 GiB at 0x900000000 in the VM): both maps failed, the bring-up stopped and WindowServer never reached
the GPU; the kext of that run logged no reason. Used by tools/triage_reports.py; tests/test_triage_bar.py."""
import re

ROW = "VRAM aperture"
OK_RE = re.compile(r"VRAM aperture: BAR0 (\d+) MiB at phys (0x[0-9a-f]+), mapped (\d+) MiB \(([^)]*)\), visible heap (\d+) MiB"
                   r"(?: \((\d+) MiB beyond it\))?")
NOT_RE = re.compile(r"VRAM aperture: NOT MAPPED: BAR0 (\d+) MiB, mapped 0 MiB, visible heap 0 MiB: (.*)$", re.M)
BRINGUP_RE = re.compile(r"cannot map BAR0 \(VRAM aperture\)(?:: (.*))?$", re.M)
OLD_DRM_RE = re.compile(r"DRM/amdgpu UAPI ready: VRAM (\d+) MiB \((\d+) MiB visible\)")


def bar_rows(*texts):
    """[(level, ROW, text)] from the kernel log(s): the archive's 20-log-rdna3 and / or a serial log (duplicates are fine,
    the newest line of each kind wins). INFO with the window, WARN when the window is smaller than BAR0 because bigger maps
    failed, FAIL when nothing was mapped (the reason the kext logged), [] when the log has no core DRM init at all."""
    log = "\n".join(t or "" for t in texts)
    oks = OK_RE.findall(log)
    nots = NOT_RE.findall(log)
    bring = [m.group(1) for m in BRINGUP_RE.finditer(log)]
    out = []
    if nots:
        bar, why = nots[-1]
        out.append(("FAIL", ROW, "BAR0 %s MiB, mapped 0 MiB, visible heap 0 MiB - NOT MAPPED: %s; nothing of VRAM is CPU-visible, "
                                 "the bring-up stops at its own map (docs/TESTING-VM.html 'A bigger BAR0')" % (bar, why.strip()[:300])))
    elif oks:
        bar, phys, mapped, how, vis, beyond = oks[-1]
        bar_i, mapped_i = int(bar), int(mapped)
        text = "BAR0 %s MiB, mapped %s MiB, visible heap %s MiB (BAR0 at phys %s%s)" % (
            bar, mapped, vis, phys, ", %s MiB of VRAM beyond it" % beyond if beyond else "")
        if mapped_i < bar_i and "failed to map" in how:
            out.append(("WARN", ROW, text + " - the kernel window is smaller than BAR0 because bigger windows failed to map "
                                            "(the map KPI gives no IOReturn; rdna3vramwin=<MiB> sets the first try)"))
        elif mapped_i < bar_i:
            out.append(("INFO", ROW, text + " - the window is rdna3vramwin's cap, the rest of BAR0 stays user-mappable only"))
        else:
            out.append(("INFO", ROW, text))
    else:
        old = OLD_DRM_RE.findall(log)
        if old:
            vis = int(old[-1][1])
            if vis == 0:
                out.append(("FAIL", ROW, "BAR0 not mapped (0 MiB visible) by a kext before N62, which logs no reason: in a VM a BAR0 "
                                         "above 4 GiB is the known cause (card run 2026-10-28c)"))
            else:
                out.append(("INFO", ROW, "%d MiB of BAR0 mapped (a kext before N62: no 'VRAM aperture:' line)" % vis))
    for why in bring[-1:]:
        if not nots or (why and why.strip() not in nots[-1][1]):
            out.append(("FAIL", ROW, "the bring-up could not map BAR0%s - Stage 2 stopped (no GPU for WindowServer)" % (
                ": " + why.strip()[:300] if why else " (a kext before N62: no reason)")))
    return out
