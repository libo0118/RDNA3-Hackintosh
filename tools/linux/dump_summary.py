#!/usr/bin/env python3
"""dump_summary.py - the parts of tools/linux/card_dump.sh's archive the
macOS driver is compared with, decoded.

    python3 tools/linux/dump_summary.py rdna3-card-dump.tar.gz [report-dir]

With a report dir (rdna3vm reports) it also puts our last 'PM: RLC_STAT'
line next to the Linux idle values.
"""
import glob
import io
import os
import re
import sys
import tarfile

GPM_BITS = ["RLC_BUSY", "GFX_POWER_STATUS", "GFX_CLOCK_STATUS", "GFX_LS_STATUS",
            "GFX_PIPELINE_POWER_STATUS", "CNTX_IDLE_BEING_PROCESSED", "CNTX_BUSY_BEING_PROCESSED",
            "GFX_IDLE_BEING_PROCESSED", "CMP_BUSY_BEING_PROCESSED", "SAVING_REGISTERS",
            "RESTORING_REGISTERS", "GFX3D_BLOCKS_CHANGING_POWER_STATE",
            "CMP_BLOCKS_CHANGING_POWER_STATE", "STATIC_WGP_POWERING_UP", "STATIC_WGP_POWERING_DOWN",
            "DYN_WGP_POWERING_UP", "DYN_WGP_POWERING_DOWN", "ABORTED_PD_SEQUENCE",
            "CMP_power_status", "GFX_LS_STATUS_3D", "GFX_CLOCK_STATUS_3D", "MGCG_OVERRIDE_STATUS",
            "RLC_EXEC_ROM_CODE", "FGCG_OVERRIDE_STATUS", "PG_ERROR_STATUS"]


def members(path):
    files = {}
    with tarfile.open(path) as t:
        for m in t.getmembers():
            if m.isfile():
                files[m.name.split("/", 1)[1] if "/" in m.name else m.name] = t.extractfile(m).read()
    return files


def regs(text):
    out = {}
    for l in text.splitlines():
        p = l.split()
        if len(p) == 3 and p[2].startswith("0x"):
            out[p[0]] = int(p[2], 16)
    return out


def bits(v, names):
    return ",".join(n for i, n in enumerate(names) if v >> i & 1) or "-"


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    f = members(sys.argv[1])
    txt = lambda n: f.get(n, b"").decode(errors="replace")
    print("== card")
    print("  " + txt("vbios/vbios_version.txt").strip())
    m = re.search(r"LnkSta:[^\n]*", txt("pci/gpu-lspci.txt"))
    print("  " + (m.group(0) if m else "no LnkSta"))
    m = re.search(r"Resizable BAR[\s\S]*?BAR 0: current size: (\S+)", txt("pci/gpu-lspci.txt"))
    print("  BAR0 " + (m.group(1) if m else "?"))

    print("== idle power (Linux)")
    s = txt("pm/idle-samples.txt").splitlines()
    busy = [int(x) for x in re.findall(r"busy=(\d+)", "\n".join(s))]
    if busy:
        print("  gpu_busy_percent over 20 s: min %d avg %.1f max %d" % (min(busy), sum(busy) / len(busy), max(busy)))
    print("  " + txt("pm/gfxoff-residency.txt").strip())
    for l in txt("pm/hwmon.txt").splitlines():
        if re.match(r"(power1_average|power1_input|freq1_input|freq2_input|temp1_input|fan1_input) ", l):
            print("  hwmon " + l)
    pm = txt("pm/sysfs.txt")
    for sec in ("pp_dpm_sclk", "pp_dpm_mclk", "pp_dpm_fclk", "pp_features", "pp_power_profile_mode"):
        m = re.search(r"== %s\n(.*?)(?=\n== |\Z)" % sec, pm, re.S)
        if m:
            body = m.group(1).strip().splitlines()
            if sec == "pp_features":
                body = [l for l in body if "enabled" in l.lower()][:60]
            print("  %s:\n    %s" % (sec, "\n    ".join(body[:40])))

    print("== GC / RLC at Linux idle (GFXOFF held off while read)")
    g = regs(txt("regs/gc_11_0_0.txt"))
    for n in ("GRBM_STATUS", "RLC_STAT", "RLC_GPM_STAT", "RLC_PG_CNTL", "RLC_CGCG_CGLS_CTRL",
              "RLC_CGCG_CGLS_CTRL_3D", "RLC_CGTT_MGCG_OVERRIDE", "RLC_SAFE_MODE", "CP_BUSY_STAT",
              "CP_CPC_BUSY_STAT", "CP_CPF_BUSY_STAT", "CP_RB_WPTR_POLL_CNTL", "RLC_CNTL", "GB_ADDR_CONFIG"):
        if n in g:
            extra = "  [%s]" % bits(g[n], GPM_BITS) if n == "RLC_GPM_STAT" else ""
            print("  %-24s 0x%08x%s" % (n, g[n], extra))
    if len(sys.argv) > 2:
        ours = None
        for p in sorted(glob.glob(os.path.join(sys.argv[2], "**", "*"), recursive=True), key=os.path.getmtime):
            if p.endswith((".log", ".txt")) and os.path.isfile(p):
                for l in open(p, errors="replace"):
                    if "PM: RLC_STAT" in l:
                        ours = l.strip()
        print("  macOS: " + (ours[ours.find("PM:"):] if ours else "no 'PM: RLC_STAT' line in the reports"))

    print("== DC state (DTN log: HUBBUB watermarks, pipes, OTG)")
    dtn = txt("display/amdgpu_dm_dtn_log.txt").splitlines()
    print("  " + "\n  ".join(dtn[:80]) if dtn else "  no DTN log")

    print("== displays")
    for n in sorted(f):
        if n.startswith("display/card") and n.endswith(".txt") and "edid" not in n:
            t = txt(n).splitlines()
            print("  %s: %s %s" % (os.path.basename(n)[:-4], t[0] if t else "", " ".join(t[4:6])))
    print("== vulkan")
    print("  " + "\n  ".join(l for l in txt("vulkan/rdna3_vktest.txt").splitlines()
                              if re.match(r"(PASS|FAIL|device|ALL|FAILED|skipped)", l)))


if __name__ == "__main__":
    main()
