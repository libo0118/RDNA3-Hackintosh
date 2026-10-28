#!/usr/bin/env python3
"""What a frame costs on its way to the panel (DRIVER-PLAN N61): triage rows `present cost` and `IOP copy`.

Sources:
  * the Metal plugin's `vk: present cost: SUMMARY outs= cpu= dma= ...` (metal/plugin/nvmtl_present_cost.h), the newest
    line per process, from the plugin / CoreDisplay logs: every copy-out of a private image into its IOSurface (the CPU
    path: staging submit + wait + CPU copy, split; the GPU path: copied in the command buffer, the publish only moves
    the seeds), why the GPU path was not taken, surface-in uploads;
  * the kernel's `debug.rdna3accel_iop_copy_*` / `iop_gate_us*` (common/rdna3_iop_copy.h) in 29-surface-vram.txt: the
    display pipe's CPU copy of every presented surface into the scanout, and the time its gates (the N59 stale wait,
    the VRAM page-off wait) held the transactions.

    present_rows(plugin_text, surfvram, iop_on) -> [(LEVEL, name, text)]
"""
import re

SLOW_MS = 8.0   # an average copy-out or IOP copy above this costs a 60 Hz frame half its time


def _num(d, k):
    try:
        return int(d.get(k, "0") or 0)
    except ValueError:
        return 0


def plugin_summaries(plugin_text):
    """{'Proc[pid]': {key: value}} from the newest `vk: present cost: SUMMARY` per process"""
    last = {}
    for m in re.finditer(r"([A-Za-z][\w .-]*?)\[(\d+):[0-9a-f]+\].*?NVMTL: vk: present cost: SUMMARY (.*)$", plugin_text or "", re.M):
        name = re.sub(r"^(Df|Db|Dc|I|In|E|Er|Ef|F|Fa|A|De|Default|Info|Debug|Error|Fault)\s+", "", m.group(1).strip())
        last["%s[%s]" % (name, m.group(2))] = dict(kv.split("=", 1) for kv in m.group(3).split() if "=" in kv)
    return last


def kernel_counters(surfvram):
    """{short name: int} of debug.rdna3accel_iop_copy* / iop_gate_us* / iop_ok*"""
    out = {}
    for m in re.finditer(r"^debug\.rdna3accel_(iop_copy\w*|iop_gate_us\w*|iop_ok\w*): (-?\d+)\s*$", surfvram or "", re.M):
        out[m.group(1)] = int(m.group(2))
    return out


def iop_text(c):
    """the kernel's copy cost as a phrase, or None without the N61 counters"""
    n = c.get("iop_copy_n")
    if n is None:
        return None
    if not n:
        return "no IOP copy"
    us = c.get("iop_copy_us", 0)
    heads = []
    for h in range(4):
        hn = c.get("iop_copy_n_h%d" % h, 0)
        if hn:
            heads.append("head %d: %d, avg %.2f ms, gates avg %.2f ms" % (
                h, hn, c.get("iop_copy_us_h%d" % h, 0) / 1000.0 / hn, c.get("iop_gate_us_h%d" % h, 0) / 1000.0 / hn))
    return ("%d IOP copies, avg %.2f ms (%d MB/s, %d of them 8 bytes per access), longest %.2f ms, %d MB in all; gates before "
            "the copies avg %.2f ms%s" % (
                n, us / 1000.0 / n, c.get("iop_copy_bytes", 0) // us if us else 0, c.get("iop_copy_64", 0),
                c.get("iop_copy_max_us", 0) / 1000.0, c.get("iop_copy_bytes", 0) >> 20, c.get("iop_gate_us", 0) / 1000.0 / n,
                " (" + "; ".join(heads) + ")" if heads else ""))


def present_rows(plugin_text, surfvram, iop_on=True):
    out = []
    c = kernel_counters(surfvram)
    iop = iop_text(c)
    if iop is None:
        if iop_on and "iop_ok" in c:
            out.append(("WARN", "IOP copy", "no debug.rdna3accel_iop_copy_* counters in 29-surface-vram.txt (a kernel before DRIVER-PLAN N61, "
                        "or a collector that does not grep them): what the display pipe's copy into the scanout costs is UNKNOWN"))
    else:
        n = c.get("iop_copy_n", 0)
        avg = c.get("iop_copy_us", 0) / 1000.0 / n if n else 0.0
        out.append(("WARN" if avg > SLOW_MS else "INFO", "IOP copy", iop + (": the copy alone takes over %.0f ms a frame" % SLOW_MS if avg > SLOW_MS else "")))
    iop_avg = "%.2f ms" % (c.get("iop_copy_us", 0) / 1000.0 / c["iop_copy_n"]) if c.get("iop_copy_n") else "unknown"
    sums = plugin_summaries(plugin_text)
    if not sums:
        if re.search(r"NVMTL: vk: handoff: SUMMARY", plugin_text or ""):
            out.append(("INFO", "present cost", "no `vk: present cost: SUMMARY` line although the plugin logs hand-off summaries: a plugin "
                        "before DRIVER-PLAN N61, or no IOSurface was copied out / uploaded"))
        return out
    for proc, d in sorted(sums.items()):
        outs, cpu, dma = _num(d, "outs"), _num(d, "cpu"), _num(d, "dma")
        ins = _num(d, "ins")
        whys = ", ".join("%s %d" % (k[4:], _num(d, k)) for k in ("why_off", "why_first", "why_shape", "why_ro", "why_import", "why_moved", "why_record") if _num(d, k))
        path = "GPU" if dma and not cpu else "CPU" if cpu and not dma else "GPU %d / CPU %d" % (dma, cpu) if outs else "none"
        text = ("%s: copy-out avg %.2f ms (path %s; NVMTL_SURFOUT_DMA %s), IOP copy avg %s, surface-in avg %.2f ms | "
                "%d copy-outs (%d MB), longest %.2f ms, %d slow (>= 16 ms); CPU path avg %.2f ms = lock %.2f + staging submit/wait %.2f + "
                "CPU copy %.2f ms (%s MB/s); GPU path publish avg %.3f ms; GPU path not taken: %s; %d surface(s) imported, %d import(s) refused; "
                "%d surface-in upload(s), %s MB, longest %.2f ms" % (
                    proc, _num(d, "out_us_avg") / 1000.0, path, "on" if d.get("dma_mode") == "1" else "OFF", iop_avg, _num(d, "in_us_avg") / 1000.0,
                    outs, _num(d, "out_mb"), _num(d, "out_us_max") / 1000.0, _num(d, "slow"),
                    _num(d, "cpu_us_avg") / 1000.0, _num(d, "lock_us_avg") / 1000.0, _num(d, "gpu_us_avg") / 1000.0, _num(d, "memcpy_us_avg") / 1000.0,
                    d.get("memcpy_mbps", "?"), _num(d, "dma_us_avg") / 1000.0, whys or "-", _num(d, "imports"), _num(d, "import_fail"),
                    ins, d.get("in_mb", "?"), _num(d, "in_us_max") / 1000.0))
        lvl = "INFO"
        if outs and _num(d, "out_us_avg") / 1000.0 > SLOW_MS:
            lvl = "WARN"
            text += ": the copy-outs cost more than %.0f ms each on average" % SLOW_MS
        if d.get("dma_mode") == "1" and cpu and not dma and proc.startswith("WindowServer"):
            lvl = "WARN"
            text += ": the GPU path is on but WindowServer used none (the reasons above, the `present cost (N61):` lines say more)"
        if _num(d, "import_fail") or _num(d, "why_record"):
            lvl = "WARN"
        out.append((lvl, "present cost", text))
    return out
