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

  * N68: `debug.rdna3accel_iop_txn_*` / `iop_vbl_*` / `iop_latch*` (same file, or a bare-metal stick report's
    sysctl.txt): every transaction's time on WindowServer's thread per head, where in the scan its copy ended (two
    copies in one refresh, refreshes with none, direct copies ending mid-scan), both heads inside at once, and the
    latched copy (debug.rdna3accel_iop_latch): frames flipped at the latch, its fallbacks by reason, its VRAM.

    present_rows(plugin_text, surfvram, iop_on) -> [(LEVEL, name, text)]   (the `ws frame` rows included)
    iop_latch_rows(counters) -> [(LEVEL, 'IOP timing' | 'IOP latch', text)]
    python3 tools/triage_present_cost.py SYSCTL_DUMP [PLUGIN_LOG]   (prints the rows; the Pi session's sysctl.txt)
    ws_frame_rows(plugin_text) -> [(LEVEL, 'ws frame', text)]
    drawable_rows(plugin_text) -> [(LEVEL, 'drawable', text)]   (N73: nextDrawable wait, present latency, WindowServer's surface textures)
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
    for m in re.finditer(r"^debug\.rdna3accel_(iop_copy\w*|iop_gate_us\w*|iop_ok\w*|iop_flip_fmt|iop_txn\w*|iop_vbl\w*|"
                         r"iop_latch\w*|iop_stale_wait_ms): (-?\d+)\s*$",
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


def _q(c, k):
    """a counter as text, '?' when the dump lacks it"""
    return str(c[k]) if k in c else "?"


def iop_format_rows(c):
    """bare metal 2026-10-31 (N65): `IOP formats` - the presented surfaces that were not BGRA (l10r converted by the
    CPU copy, anything else refused, the last refused fourcc); no row while every frame was BGRA or for a kernel
    without the counters; a companion counter missing is '?' and a WARN (ECC review of 48fe3ad)"""
    seen, l10r, other = c.get("iop_copy_fmt_l10r_seen", 0), c.get("iop_copy_fmt_l10r", 0), c.get("iop_copy_fmt_other", 0)
    if not l10r and not other and not seen:
        return []
    need = ("iop_copy_fmt_l10r", "iop_copy_fmt_l10r_seen", "iop_copy_fmt_other", "iop_copy_fmt_other_last",
            "iop_copy_sdma_fb_format", "iop_flip_fmt")
    missing = [k for k in need if k not in c]
    last = c.get("iop_copy_fmt_other_last", 0)
    last_txt = ("0x%08x ('%s')" % (last, "".join(chr((last >> s_) & 0xff) if 32 <= ((last >> s_) & 0xff) < 127 else "."
                                                  for s_ in (24, 16, 8, 0)))) if last else "none"
    text = ("%s l10r (ARGB2101010) frame(s) converted into the 8-bit scanout by the CPU of %s seen (not by SDMA: "
            "fallbacks `format` %s; zero-copy flips refused for the format %s); %s frame(s) of another format refused "
            "(the last: %s)" % (_q(c, "iop_copy_fmt_l10r"), _q(c, "iop_copy_fmt_l10r_seen"),
                                _q(c, "iop_copy_sdma_fb_format"), _q(c, "iop_flip_fmt"), _q(c, "iop_copy_fmt_other"),
                                last_txt))
    why = []
    if other:
        why.append("the `iop: surface 0x.. format 0x........ is not BGRA or l10r` lines name the format; those frames "
                   "never reached the panel")
    if seen > l10r:
        why.append("%d l10r frame(s) seen but not converted (refused later: geometry, map, scanout - the `iop:` lines)" %
                   (seen - l10r))
    if missing:
        why.append("counter(s) MISSING from the dump: %s (not 0: the collector's grep or an older kext)" % ", ".join(missing))
    if why:
        return [("WARN", "IOP formats", text + ": " + "; ".join(why))]
    return [("INFO", "IOP formats", text + "; WindowServer presents 10-bit surfaces: the DCN 10-bit scanout (N66) would "
             "remove the conversion")]


SAME_WARN, SKIP_WARN = 0.02, 0.05   # N68: of the placed copies, two in one refresh / refreshes repeated
VBL_PLACED_MIN = 0.9                # N68 K2: fewer copies placed than this share of the head's copies: data missing
LATCH_FB_WARN = 0.01                # N68: the latched path's frames that took the direct copy


def iop_latch_rows(c):
    """N68: `IOP timing` (K1 / K2 / K4 per head) and `IOP latch` (the latched copy); no row for a kernel before N68
    unless it presented (then a WARN: the counters are missing)"""
    heads = [h for h in range(4) if c.get("iop_txn_n_h%d" % h, 0)]
    if "iop_txn_n_h0" not in c:
        if c.get("iop_ok", 0):
            return [("WARN", "IOP timing", "no debug.rdna3accel_iop_txn_* counters although IOP frames were presented "
                     "(iop_ok %d): a kernel before DRIVER-PLAN N68, or a collector that does not grep iop_txn / iop_vbl / "
                     "iop_latch - the transactions' timing is UNKNOWN" % c.get("iop_ok", 0))]
        return []
    out = []
    for h in heads:
        n = c["iop_txn_n_h%d" % h]
        g = lambda k: c.get("%s_h%d" % (k, h), 0)   # noqa: E731
        vn = g("iop_vbl_n")
        bins = "/".join(str(g("iop_vbl_bin%d" % b)) for b in range(4)) + " blank %d" % g("iop_vbl_blank")
        text = ("head %d: %d transaction(s), avg %.2f ms, longest %.2f ms, %d over 8 ms (the `iop txn:` lines split "
                "them), N59 stale wait %.2f ms a transaction (stale_wait_ms %s); %d copies placed in the scan: %d in the "
                "SAME refresh as the one before, %d refresh(es) with NO copy, %d direct copies ending mid-scan (torn); "
                "scan quarters %s" % (
                    h, n, g("iop_txn_us") / 1000.0 / n, g("iop_txn_max_us") / 1000.0, g("iop_txn_slow"),
                    g("iop_txn_stale_us") / 1000.0 / n, _q(c, "iop_stale_wait_ms"), vn, g("iop_vbl_same"),
                    g("iop_vbl_skip"), g("iop_vbl_mid"), bins))
        why = []
        copies = g("iop_copy_n") or n   # the head's copies (CPU + SDMA); a kernel without them: its transactions
        if not vn:
            why.append("NO copy was placed in the scan (iop_vbl_n 0): K2 is missing - the head state unreadable "
                       "(iop_vbl_unread, the `iop vbl:` lines) or the counters not collected")
        elif vn < VBL_PLACED_MIN * copies:
            why.append("only %d of %d copies placed in the scan: K2 is partial (iop_vbl_unread)" % (vn, copies))
        if g("iop_txn_slow"):
            why.append("transactions held WindowServer's thread over 8 ms")
        if vn and g("iop_vbl_same") > SAME_WARN * vn:
            why.append("copies land two in one refresh (one never shown): uneven steps")
        if vn and g("iop_vbl_skip") > SKIP_WARN * vn:
            why.append("refreshes get no new frame (a repeated frame): uneven steps")
        out.append(("WARN" if why else "INFO", "IOP timing", text + (": " + "; ".join(why) if why else "")))
    ov, ovs, unread = c.get("iop_txn_overlap", 0), c.get("iop_txn_overlap_same", 0), c.get("iop_vbl_unread", 0)
    if ov or ovs or unread:
        out.append(("WARN" if unread else "INFO", "IOP timing",
                    "%d transaction(s) entered while another head's was inside, %d while one of the same head was; %d "
                    "copies whose head state could not be read%s" % (
                        ov, ovs, unread, " (the `iop vbl:` lines say why: K2 misses them)" if unread else "")))
    gaps = []
    if "iop_stale_wait_ms" not in c:
        gaps.append("debug.rdna3accel_iop_stale_wait_ms is MISSING from the dump (the N59 switch during the run unknown)")
    txn_all = sum(c.get("iop_txn_n_h%d" % h, 0) for h in range(4))
    if txn_all < c.get("iop_ok", 0):
        gaps.append("%d transactions timed but %d presented (iop_ok): counters from two kernels, or a head beyond 4"
                    % (txn_all, c.get("iop_ok", 0)))
    if gaps:
        out.append(("WARN", "IOP timing", "; ".join(gaps)))
    if "iop_latch" not in c:
        out.append(("WARN", "IOP latch", "no debug.rdna3accel_iop_latch in the dump although the N68 timing counters are "
                    "there: the collector's grep misses it"))
        return out
    on = c.get("iop_latch", 0)
    fbs = ", ".join("%s %d" % (k, c.get("iop_latch_fb_" + k, 0)) for k in
                    ("queue", "vram", "resize", "busy", "refused", "geom") if c.get("iop_latch_fb_" + k, 0))
    per = "; ".join("head %d: %d latched (%d into D, %d waited avg %.2f ms), %d direct instead" % (
        h, c.get("iop_latch_n_h%d" % h, 0), c.get("iop_latch_d_h%d" % h, 0), c.get("iop_latch_wait_h%d" % h, 0),
        c.get("iop_latch_wait_us_h%d" % h, 0) / 1000.0 / max(1, c.get("iop_latch_wait_h%d" % h, 0)),
        c.get("iop_latch_fb_h%d" % h, 0)) for h in heads)
    latched = sum(c.get("iop_latch_n_h%d" % h, 0) for h in range(4))
    text = ("debug.rdna3accel_iop_latch %s; %s; fallbacks: %s; second buffers: %d MiB held, %d allocated, %d freed, "
            "%d refused, %d free(s) put off" % (
                "ON (copied frames flipped at the latch)" if on else "off (copied into the live scanout)", per or "-",
                fbs or "none", c.get("iop_latch_vram_bytes", 0) >> 20, c.get("iop_latch_vram_allocs", 0),
                c.get("iop_latch_vram_frees", 0), c.get("iop_latch_vram_refused", 0), c.get("iop_latch_freewait", 0)))
    why = []
    fb_all = sum(c.get("iop_latch_fb_" + k, 0) for k in ("queue", "vram", "resize", "busy", "refused", "geom"))
    if on and fb_all > LATCH_FB_WARN * max(1, latched + fb_all):
        why.append("%d of %d frames of the latched path took the direct copy (over %.0f %%: the reasons above, the "
                   "`iop latch: ... copied into the LIVE scanout instead` lines)" % (fb_all, latched + fb_all,
                                                                                     LATCH_FB_WARN * 100))
    if not on and latched:
        why.append("it reads 0 but %d frame(s) were latched: switched off during the run (the counters mix both "
                   "phases; the `iop latch (N68): ... -> 0` line says when)" % latched)
    if c.get("iop_latch_vram_free_failed", 0):
        why.append("%d second buffer(s) the core refused to free (that VRAM stays allocated)" % c["iop_latch_vram_free_failed"])
    if on and not latched and sum(c.get("iop_vbl_n_h%d" % h, 0) for h in heads):
        why.append("it is ON but no frame was latched (the fallback reasons)")
    if c.get("iop_latch_vram_refused", 0):
        why.append("the visible VRAM heap refused the second buffer (`iop latch: ... no VRAM` lines; the latch is on by "
                   "default since the bare-metal round 3: with a 256 MiB BAR there is no room, so the frames fell back "
                   "to the direct copy - a bigger BAR (ResizeAppleGpuBars) or rdna3ioplatch=0)")
    if c.get("iop_latch_fb_refused", 0) or c.get("iop_latch_fb_geom", 0):
        why.append("flips refused or buffers of the wrong shape (`iop latch:` lines)")
    if not on and c.get("iop_latch_vram_bytes", 0):
        why.append("off, but a second buffer is still held (the panel may still scan it: iop_latch_freewait)")
    out.append(("WARN" if why else "INFO", "IOP latch", text + (": " + "; ".join(why) if why else "")))
    return out


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


DL_WAIT_PCT = 30.0   # an app in nextDrawable more than this share of its wall is held by the drawables' way back (N73)
DL_THRASH = 0.5      # WindowServer with the keep ring on: fewer reuses than this share of its IOSurface textures = the ring misses
DL_THRASH_MIN = 100  # ... counted from this many textures on


def dl_lines(plugin_text):
    """{'Proc[pid]': [{key: value}, ...]} every `vk: drawable: SUMMARY` per process in log order (DRIVER-PLAN N73)"""
    out = {}
    for m in re.finditer(r"([A-Za-z][\w .-]*?)\[(\d+):[0-9a-f]+\].*?NVMTL: vk: drawable: SUMMARY (.*)$", plugin_text or "", re.M):
        name = re.sub(r"^(Df|Db|Dc|I|In|E|Er|Ef|F|Fa|A|De|Default|Info|Debug|Error|Fault)\s+", "", m.group(1).strip())
        out.setdefault("%s[%s]" % (name, m.group(2)), []).append(dict(kv.split("=", 1) for kv in m.group(3).split() if "=" in kv))
    return out


def dl_summaries(plugin_text):
    """{'Proc[pid]': {key: value}} from the newest `vk: drawable: SUMMARY` per process"""
    return {k: v[-1] for k, v in dl_lines(plugin_text).items()}


def _dl_interval_pct(prev, d):
    """the nextDrawable share of the interval between two SUMMARY lines (next_ms / wall_ms), None when it cannot be taken"""
    if not prev or "next_ms" not in d or "wall_ms" not in d or "next_ms" not in prev or "wall_ms" not in prev:
        return None
    dw = _num(d, "wall_ms") - _num(prev, "wall_ms")
    return 100.0 * (_num(d, "next_ms") - _num(prev, "next_ms")) / dw if dw > 0 else None


def drawable_rows(plugin_text):
    """N73: `drawable: nextDrawable P % of the wall ... commit -> present ... away ... | surface textures ...` per process"""
    lines = dl_lines(plugin_text)
    if not lines:
        if re.search(r"NVMTL: vk: present cost: SUMMARY", plugin_text or ""):
            return [("INFO", "drawable", "no `vk: drawable: SUMMARY` line although the plugin logs present-cost summaries: a plugin before "
                     "DRIVER-PLAN N73 (the apps' nextDrawable wait and WindowServer's IOSurface images are UNKNOWN)")]
        return []
    out = []
    for proc, seq in sorted(lines.items(), key=lambda kv: (not kv[0].startswith("WindowServer"), kv[0])):
        d = seq[-1]
        ws = proc.startswith("WindowServer")
        nn, an, sn, sa, pn = _num(d, "next_n"), _num(d, "away_n"), _num(d, "st_new"), _num(d, "st_alias"), _num(d, "pres_n")
        bad_pct = False
        try:
            pct = float(d.get("next_pct", "0"))
        except ValueError:
            pct, bad_pct = 0.0, True
        ipct = _dl_interval_pct(seq[-2] if len(seq) > 1 else None, d)
        ms = lambda k: _num(d, k) / 1000.0  # noqa: E731
        parts, warn = [], []
        if nn:
            parts.append("nextDrawable %.1f %% of the wall%s (%d call(s), avg %.2f ms, longest %.2f ms, %d over 1 ms, %d nil)" % (
                pct, (", %.1f %% in the last interval" % ipct) if ipct is not None else "", nn, ms("next_us_avg"), ms("next_us_max"),
                _num(d, "next_slow"), _num(d, "next_nil")))
        elif pn and not ws:
            if d.get("trace") == "1":
                parts.append("nextDrawable hooked but never called (the app presents another way)")
            else:
                parts.append("nextDrawable NOT timed (trace=0: NVMTL_DRAWABLE_TRACE=0, or -[CAMetalLayer nextDrawable] was never found: the "
                             "`drawable (N73):` lines)")
                warn.append("the app presents but its nextDrawable wait is UNKNOWN")
        if bad_pct:
            warn.append("next_pct %r could not be read" % d.get("next_pct"))
        if pn:
            parts.append("commit -> present avg %.2f ms (longest %.2f, %d present(s))" % (ms("pres_us_avg"), ms("pres_us_max"), pn))
        if an or _num(d, "away_unknown"):
            parts.append("present -> back from nextDrawable avg %.2f ms (longest %.2f, %d timed, %d unknown)" % (
                ms("away_us_avg"), ms("away_us_max"), an, _num(d, "away_unknown")))
        keep = d.get("keep", "0")
        if sn or sa:
            parts.append("IOSurface textures: %d new image(s) (avg %.3f ms to make), %d wrapper(s) reused a live image, wrapper life avg %.2f ms "
                         "(longest %.2f, %d); keep ring %s (%d kept, %d let go)" % (
                             sn, ms("st_new_us_avg"), sa, ms("st_life_us_avg"), ms("st_life_us_max"), _num(d, "st_life_n"),
                             keep if keep != "0" else "OFF", _num(d, "st_kept"), _num(d, "st_evict")))
            if ws and keep != "0" and sn + sa >= DL_THRASH_MIN and sa < DL_THRASH * (sn + sa):
                warn.append("the keep ring misses: only %d of %d IOSurface textures reused an image (more surfaces in rotation than the "
                            "%s images it keeps, or they age out; NVMTL_SURFTEX_KEEP)" % (sa, sn + sa, keep))
        if not parts and not warn:
            continue
        text = "%s: %s" % (proc, "; ".join(parts))
        wait = ipct if ipct is not None else pct
        if nn >= 100 and wait > DL_WAIT_PCT:
            warn.append("the app waits for its drawables over %.0f %% of the time - the cap is the drawables' way back "
                        "(compare commit -> present, ours in the app, with present -> back, WindowServer's hold; WindowServer's own row "
                        "says what its frames cost)" % DL_WAIT_PCT)
        if warn:
            text += ": " + "; ".join(warn)
        out.append(("WARN" if warn else "INFO", "drawable", text))
    return out


def present_rows(plugin_text, surfvram, iop_on=True):
    return _present_rows(plugin_text, surfvram, iop_on) + ws_frame_rows(plugin_text) + drawable_rows(plugin_text)


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
    out.extend(iop_latch_rows(c))
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


def main(argv):
    """the rows of a sysctl dump (and a plugin log) - for a bare-metal stick report's sysctl.txt"""
    if len(argv) < 2:
        print("usage: triage_present_cost.py SYSCTL_DUMP [PLUGIN_LOG]")
        return 2
    with open(argv[1], errors="replace") as f:
        sysctls = f.read()
    plugin = ""
    if len(argv) > 2:
        with open(argv[2], errors="replace") as f:
            plugin = f.read()
    for lvl, name, text in present_rows(plugin, sysctls, True):
        print("%s %-14s %s" % (lvl, name, text))
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main(sys.argv))
