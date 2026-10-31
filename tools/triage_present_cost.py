#!/usr/bin/env python3
"""What a frame costs on its way to the panel (DRIVER-PLAN N61): triage rows `present cost` and `IOP copy`.

Sources:
  * the Metal plugin's `vk: present cost: SUMMARY outs= cpu= dma= ...` (metal/plugin/nvmtl_present_cost.h), the newest
    line per process, from the plugin / CoreDisplay logs: every copy-out of a private image into its IOSurface (the CPU
    path: staging submit + wait + CPU copy, split; the GPU path: copied in the command buffer, the publish only moves
    the seeds), why the GPU path was not taken, surface-in uploads;
  * the kernel's `debug.rdna3accel_iop_copy_*` / `iop_gate_us*` (common/rdna3_iop_copy.h) in 29-surface-vram.txt: the
    display pipe's CPU copy of every presented surface into the scanout, and the time its gates (the N59 stale wait,
    the VRAM page-off wait) held the transactions;
  * N63: `debug.rdna3accel_iop_copy_sdma_*` (same file): the same copy by SDMA0 from the surface's pages bound to the
    GART, its fallbacks to the CPU by reason, and `iop_copy_sdma_state` (1 on, 0 off by the switch, 2 off after
    failures, 3 a core without rdna3IopSdma).

  * N64: the plugin's `vk: ws frame: SUMMARY frames= ... rec_us_avg= gpu_us_avg= ...` (metal/plugin/nvmtl_ws_frame.h), the newest
    line per process: what one frame (a command buffer that began a render pass) costs to record and on the GPU, its draws,
    framebuffer-fetch draws and the barriers before them (image / old memory barrier), the surface-in uploads.

    present_rows(plugin_text, surfvram, iop_on) -> [(LEVEL, name, text)]   (the `ws frame` rows included)
    ws_frame_rows(plugin_text) -> [(LEVEL, 'ws frame', text)]
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
    """{short name: int} of debug.rdna3accel_iop_copy* / iop_gate_us* / iop_ok* / iop_flip_fmt"""
    out = {}
    for m in re.finditer(r"^debug\.rdna3accel_(iop_copy\w*|iop_gate_us\w*|iop_ok\w*|iop_flip_fmt): (-?\d+)\s*$",
                         surfvram or "", re.M):
        out[m.group(1)] = int(m.group(2))
    return out


SDMA_FB = ("core", "pages", "bind", "copy", "verify", "off", "scanout", "format")   # common/rdna3_iop_copy.h enum rd3_iop_sdma_fb
SDMA_STATE = {0: "off (debug.rdna3accel_iop_sdma=0 / rdna3iopsdma=0)", 1: "on", 2: "switched OFF after 3 failures in a row",
              3: "the core does not answer rdna3IopSdma (a core older than N63)"}


def iop_counts(c):
    """(copies, us) of both paths together: the CPU's iop_copy_n / _us and N63's iop_copy_sdma_n / _us"""
    return c.get("iop_copy_n", 0) + c.get("iop_copy_sdma_n", 0), c.get("iop_copy_us", 0) + c.get("iop_copy_sdma_us", 0)


def iop_path(c):
    """which path the IOP copies took: SDMA, CPU, `SDMA n / CPU m`, or None (no copy)"""
    cpu, sdma = c.get("iop_copy_n", 0), c.get("iop_copy_sdma_n", 0)
    if not cpu and not sdma:
        return None
    return "SDMA" if sdma and not cpu else "CPU" if cpu and not sdma else "SDMA %d / CPU %d" % (sdma, cpu)


def sdma_text(c):
    """N63: the SDMA path as a phrase, or None for a kernel before it"""
    if "iop_copy_sdma_n" not in c:
        return None
    n, us = c.get("iop_copy_sdma_n", 0), c.get("iop_copy_sdma_us", 0)
    st = c.get("iop_copy_sdma_state")
    fbs = ", ".join("%s %d" % (k, c.get("iop_copy_sdma_fb_" + k, 0)) for k in SDMA_FB if c.get("iop_copy_sdma_fb_" + k, 0))
    binds = c.get("iop_copy_sdma_binds", 0)
    return ("SDMA path %s: %d copies%s; %d bind(s)%s, %d unbind(s), %d range(s) bound; %d checked by read-back; "
            "fallbacks to the CPU %d%s" % (
                SDMA_STATE.get(st, "state %s" % st) if st is not None else "state unknown", n,
                (", avg %.2f ms (core %.2f ms), longest %.2f ms, %d MB/s" % (
                    us / 1000.0 / n, c.get("iop_copy_sdma_core_us", 0) / 1000.0 / n, c.get("iop_copy_sdma_max_us", 0) / 1000.0,
                    c.get("iop_copy_sdma_bytes", 0) // us if us else 0)) if n else "",
                binds, " (avg %.2f ms)" % (c.get("iop_copy_sdma_bind_us", 0) / 1000.0 / binds) if binds else "",
                c.get("iop_copy_sdma_unbinds", 0), c.get("iop_copy_sdma_ranges", 0), c.get("iop_copy_sdma_verified", 0),
                c.get("iop_copy_sdma_fb", 0), " (" + fbs + ")" if fbs else ""))


def iop_text(c):
    """the kernel's copy cost as a phrase, or None without the N61 counters"""
    n = c.get("iop_copy_n")
    if n is None:
        return None
    sd = sdma_text(c)
    tot, tot_us = iop_counts(c)
    if not tot:
        return "no IOP copy" + ("; " + sd if sd else "")
    us = c.get("iop_copy_us", 0)
    heads = []
    for h in range(4):
        hn = c.get("iop_copy_n_h%d" % h, 0)
        if hn:
            heads.append("head %d: %d, avg %.2f ms, gates avg %.2f ms" % (
                h, hn, c.get("iop_copy_us_h%d" % h, 0) / 1000.0 / hn, c.get("iop_gate_us_h%d" % h, 0) / 1000.0 / hn))
    cpu = ("CPU path: %d copies, avg %.2f ms (%d MB/s, %d of them 8 bytes per access), longest %.2f ms, %d MB in all" % (
        n, us / 1000.0 / n, c.get("iop_copy_bytes", 0) // us if us else 0, c.get("iop_copy_64", 0),
        c.get("iop_copy_max_us", 0) / 1000.0, c.get("iop_copy_bytes", 0) >> 20)) if n else "CPU path: no copy"
    return ("%d IOP copies (path %s), avg %.2f ms; %s%s; gates before the copies avg %.2f ms%s" % (
                tot, iop_path(c), tot_us / 1000.0 / tot, cpu, "; " + sd if sd else "", c.get("iop_gate_us", 0) / 1000.0 / tot,
                " (" + "; ".join(heads) + ")" if heads else ""))


def iop_format_rows(c):
    """bare metal 2026-10-31: `IOP formats` - the presented surfaces that were not BGRA (l10r converted by the CPU copy,
    anything else refused); no row while every frame was BGRA or for a kernel without the counters"""
    l10r, other = c.get("iop_copy_fmt_l10r", 0), c.get("iop_copy_fmt_other", 0)
    if not l10r and not other:
        return []
    text = ("%d l10r (ARGB2101010) frame(s) converted to the 8-bit scanout by the CPU (not by SDMA: fallbacks `format` %d; "
            "zero-copy flips refused for the format %d); %d frame(s) of another format refused" % (
                l10r, c.get("iop_copy_sdma_fb_format", 0), c.get("iop_flip_fmt", 0), other))
    if other:
        return [("WARN", "IOP formats", text + ": the `iop: surface 0x.. format 0x........ is not BGRA or l10r` lines name the "
                 "format; those frames never reached the panel")]
    return [("INFO", "IOP formats", text + "; WindowServer presents 10-bit surfaces: the DCN 10-bit scanout (N66) would "
             "remove the conversion")]


WS_HALF_FRAME_MS = 8.0   # one command buffer over this (record or GPU) takes half of a 60 Hz frame; WindowServer commits ~2 a frame


def ws_summaries(plugin_text):
    """{'Proc[pid]': {key: value}} from the newest `vk: ws frame: SUMMARY` per process"""
    last = {}
    for m in re.finditer(r"([A-Za-z][\w .-]*?)\[(\d+):[0-9a-f]+\].*?NVMTL: vk: ws frame: SUMMARY (.*)$", plugin_text or "", re.M):
        name = re.sub(r"^(Df|Db|Dc|I|In|E|Er|Ef|F|Fa|A|De|Default|Info|Debug|Error|Fault)\s+", "", m.group(1).strip())
        last["%s[%s]" % (name, m.group(2))] = dict(kv.split("=", 1) for kv in m.group(3).split() if "=" in kv)
    return last


def ws_frame_rows(plugin_text):
    """N64: `ws frame: avg X ms (encode E, GPU G, fbfetch barriers B/frame, uploads U MB/frame)` per process, WindowServer first"""
    sums = ws_summaries(plugin_text)
    if not sums:
        if re.search(r"NVMTL: vk: present cost: SUMMARY", plugin_text or ""):
            return [("INFO", "ws frame", "no `vk: ws frame: SUMMARY` line although the plugin logs present-cost summaries: a plugin before "
                     "DRIVER-PLAN N64 (what a WindowServer frame costs is UNKNOWN)")]
        return []
    out = []
    for proc, d in sorted(sums.items(), key=lambda kv: (not kv[0].startswith("WindowServer"), kv[0])):
        fr, gn, ups = _num(d, "frames"), _num(d, "gpu_n"), _num(d, "ups")
        if not fr and not ups:
            continue
        rec, gpu = _num(d, "rec_us_avg") / 1000.0, _num(d, "gpu_us_avg") / 1000.0
        bi, bm = _num(d, "bar_img"), _num(d, "bar_mem")
        per = (lambda k: _num(d, k) / float(fr)) if fr else (lambda k: 0.0)
        up_mb = _num(d, "up_kb") / 1024.0 / fr if fr else 0.0
        act = _num(d, "active_s")
        text = ("%s: ws frame: avg %s ms (encode %.2f, GPU %s, fbfetch barriers %.1f/frame, uploads %.2f MB/frame) | "
                "%d frame(s) (command buffers that drew) in %d active second(s) = %.1f/s, busiest second %d; %.1f pass(es), %.1f draw(s), "
                "%.1f framebuffer-fetch draw(s) a frame; barriers: image %d, OLD memory %d (NVMTL_FBF_MEMBAR %s); encode longest %.2f ms, "
                "GPU longest %s, fence wait avg %.2f ms; %d surface-in upload(s) (%.1f a frame, avg %.2f ms, %d MB in all)" % (
                    proc, ("%.2f" % (rec + gpu)) if gn else ("%.2f + GPU ?" % rec), rec, ("%.2f" % gpu) if gn else "UNKNOWN",
                    (bi + bm) / float(fr) if fr else 0.0, up_mb,
                    fr, act, fr / float(act) if act else 0.0, _num(d, "fps_peak"), per("passes"), per("draws"), per("fbf_draws"),
                    bi, bm, "ON (the old barrier)" if d.get("membar") == "1" else "off", _num(d, "rec_us_max") / 1000.0,
                    ("%.2f ms (%d of %d timed)" % (_num(d, "gpu_us_max") / 1000.0, gn, fr)) if gn else "-",
                    _num(d, "wait_us_avg") / 1000.0, ups, ups / float(fr) if fr else 0.0, _num(d, "up_us_avg") / 1000.0, _num(d, "up_kb") >> 10))
        lvl, why = "INFO", []
        if fr and not gn:
            why.append("no GPU timestamp came back (calibrated timestamps missing?): the GPU share is UNKNOWN")
        if rec > WS_HALF_FRAME_MS:
            why.append("encoding alone takes over %.0f ms a command buffer" % WS_HALF_FRAME_MS)
        if gn and gpu > WS_HALF_FRAME_MS:
            why.append("the GPU alone takes over %.0f ms a command buffer" % WS_HALF_FRAME_MS)
        if bm and d.get("membar") != "1":
            why.append("%d draw(s) got the OLD memory barrier without NVMTL_FBF_MEMBAR (an attachment without an image: the `got the OLD global barrier` line)" % bm)
        if why:
            lvl = "WARN" if proc.startswith("WindowServer") else "INFO"
            text += ": " + "; ".join(why)
        out.append((lvl, "ws frame", text))
    return out


def present_rows(plugin_text, surfvram, iop_on=True):
    return _present_rows(plugin_text, surfvram, iop_on) + ws_frame_rows(plugin_text)


def _present_rows(plugin_text, surfvram, iop_on=True):
    out = []
    c = kernel_counters(surfvram)
    iop = iop_text(c)
    if iop is None:
        if iop_on and "iop_ok" in c:
            out.append(("WARN", "IOP copy", "no debug.rdna3accel_iop_copy_* counters in 29-surface-vram.txt (a kernel before DRIVER-PLAN N61, "
                        "or a collector that does not grep them): what the display pipe's copy into the scanout costs is UNKNOWN"))
    else:
        n, us = iop_counts(c)
        avg = us / 1000.0 / n if n else 0.0
        lvl, why = ("WARN", ": the copy alone takes over %.0f ms a frame" % SLOW_MS) if avg > SLOW_MS else ("INFO", "")
        st = c.get("iop_copy_sdma_state")
        if st == 2:
            lvl, why = "WARN", why + ": the SDMA path switched itself OFF (the `iop sdma (N63): switched OFF` and `iop: sdma -> CPU` lines say why)"
        elif c.get("iop_copy_sdma_fb_verify", 0) or c.get("iop_copy_sdma_fb_copy", 0):
            lvl, why = "WARN", why + ": SDMA copies failed or read back wrong (the `iop: sdma -> CPU` lines)"
        elif (st == 1 and c.get("iop_copy_n", 0) and not c.get("iop_copy_sdma_n", 0)
              and c.get("iop_copy_sdma_fb_format", 0) < c.get("iop_copy_n", 0)):
            lvl, why = "WARN", why + ": the SDMA path is on but every copy went to the CPU (the fallback reasons above)"
        out.append((lvl, "IOP copy", iop + why))
    out.extend(iop_format_rows(c))
    n_all, us_all = iop_counts(c)
    iop_avg = ("%.2f ms (%s)" % (us_all / 1000.0 / n_all, iop_path(c))) if n_all else "unknown"
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
