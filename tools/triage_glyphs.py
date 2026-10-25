"""Triage rows of DRIVER-PLAN N50 (card run 2026-10-25e: broken glyphs in Terminal / Maps, tile-shaped blocks of wrong colour in Chess).
The plugin gained a set of A/B switches and counters for those artefacts; this module reads their lines from the plugin logs
(26-log-metal-plugin.txt = the other processes, 24-log-coredisplay.txt = WindowServer; lines `... Name[pid:tid] (RDNA3MTLDriver) NVMTL: msg`,
process names may contain blanks: "Activity Monitor"), so that a report says which profile it was taken with and what the counters saw.
Kept in a module of its own so that tools/triage_reports.py only gains one call. docs/AFTER-TESTS.md "Rendering artefacts: the A/B ladder"
explains every line. Also per-rung summaries of the ladder (`parse_ladder` + `ladder_rows`, tools/vm/macos/autoreport.sh ladder_run: 41-ladder.txt, 41-ladder-<rung>.log)."""
import glob
import os
import re

_LINE = re.compile(r"^\S+ \S+ +\S+ +(.+?)\[(\d+):([0-9a-f]+)\] \(RDNA3MTLDriver\) NVMTL: (.*)$")
_LINE_OLD = re.compile(r"\b([^\s\[\]]+)\[(\d+):([0-9a-f]+)\] \(RDNA3MTLDriver\) NVMTL: (.*)$")
_Q = r'(unset|"(?:[^"\\]|\\.)*")'
_RADV = re.compile(r"^vk: RADV_DEBUG=" + _Q + r" RADV_PERFTEST=" + _Q + r" \(NVMTL_RADV_DEBUG=" + _Q + r", NVMTL_RADV_PERFTEST=" + _Q + r"\)")
_RADV_REJECT = re.compile(r"^vk: (NVMTL_RADV_\w+)=" + _Q + r" REJECTED: (.*?) \(only RADV's")
_PARANOID = re.compile(r"^conf: NVMTL_PARANOID=(\S*) - the diagnostic profile is ON: (\d+) switch\(es\) set by it \((.*?)\)(?:, already set and kept: (.*?))?; ")
_PARANOID_OFF = re.compile(r"^conf: NVMTL_PARANOID=(\S*) is not 1")
_CONF_SET = re.compile(r"^conf: set (.*)$")
_CONF_SKIP = re.compile(r"^conf: (line \d+ skipped: .*|\.\.\. and \d+ more line\(s\) skipped)$")
_SWITCHES = re.compile(r"^vk: ab switches(?: \[\d+\])?: (.*)$")
_SWITCHES_NONE = re.compile(r"^vk: ab switches: none set")
_TOKEN = re.compile(r'(NVMTL_[A-Z0-9_]+)=("(?:[^"\\]|\\.)*")')
_SUM_AB = re.compile(r"^vk: ab: SUMMARY (.*)$")
_SUM_MG = re.compile(r"^managed: SUMMARY (.*)$")
_SUM_SIN = re.compile(r"^vk: surfin: SUMMARY (.*)$")
_KV = re.compile(r"(\w+)=([\d,]+)")
_WIT = re.compile(r"^vk: keep witness (\d+) landed only (\d+) us AFTER the flush returned")
_POOL_OFF = re.compile(r"^vk: pool reuse OFF")
_MG_ON = re.compile(r"^managed: (?:shadow check ON|upload snapshot ON)")
_DRIVER = re.compile(r"^vk: driver opened directly")
_LAYOUT_NAMES = ("UNDEFINED", "GENERAL", "COLOR", "DS", "DSRO", "SRO", "SRC", "DST", "PREINIT", "other")


def _lines(text):
    for raw in (text or "").splitlines():
        m = _LINE.match(raw) or _LINE_OLD.search(raw)
        if m:
            yield m.group(1), int(m.group(2)), m.group(3), m.group(4)


def _unq(v):
    return v[1:-1].replace('\\"', '"').replace("\\\\", "\\") if v.startswith('"') else None


def _seq(kv):
    try:
        return int(kv.get("seq", 0))
    except (TypeError, ValueError):
        return 0


def _kv(text):
    out = {}
    for k, v in _KV.findall(text):
        out[k] = [int(x) for x in v.split(",")] if "," in v else int(v)
    return out


# the switches NVMTL_PARANOID=1 sets (metal/plugin/NVMTLObjects.m, tests/test_triage_glyphs.py compares the two lists)
PARANOID_KEYS = ("NVMTL_RADV_DEBUG", "NVMTL_ASYNC_COMMIT", "NVMTL_GSUB_INFLIGHT", "NVMTL_PRESUBMIT_PROLOGUE", "NVMTL_NO_POOL_REUSE", "NVMTL_NO_BUFREUSE",
                 "NVMTL_NO_BUFTEX_RECYCLE", "NVMTL_NO_HEAPTEX_RECYCLE", "NVMTL_NO_HEAPTEX_PLACE", "NVMTL_NO_DIRTYONLY", "NVMTL_ENC78_OFF", "NVMTL_G79_OFF",
                 "NVMTL_NO_RES2", "NVMTL_NO_PURGE_RECLAIM", "NVMTL_NO_SURFIN_CACHE")


def ab_switch_keys(text):
    """{(process, pid): set of NVMTL_ keys} of the processes that logged `vk: ab switches` (an empty set for `none set`), from the plugin log lines in `text`"""
    out = {}
    for proc, pid, tid, msg in _lines(text):
        if _SWITCHES_NONE.match(msg):
            out.setdefault((proc, pid), set())
            continue
        m = _SWITCHES.match(msg)
        if m:
            out.setdefault((proc, pid), set()).update(k for k, v in _TOKEN.findall(m.group(1)))
    return out


def n50_plugin_rows(text=""):
    """[(level, name, text)]. WARN: a debug / diagnostic profile or switch was on (the run does not show the default plugin), a flag was refused or a conf
    line skipped, the fence returned before the GPU's last write was visible, the CPU rewrote a managed buffer's range while its copy was in flight, a
    check or snapshot that did nothing, a plugin without the lines. INFO: the counters of what the load passes declare versus what the images rest in."""
    rows = []
    procs = {}                 # (process, pid) -> True: every process that logged anything of ours
    radv = {}                  # key -> (RADV_DEBUG, RADV_PERFTEST) as the process had them (None = unset)
    rejects = []
    paranoid, paranoid_off = {}, []
    conf_set, conf_skip = {}, []
    switches, switch_procs, switch_none = {}, set(), set()
    ab, mg, sin = {}, {}, {}   # key -> newest SUMMARY dict
    wit_lines = {}             # key -> (lines, worst) when there is no SUMMARY (an older plugin)
    pool_off = set()
    mg_on = set()
    driver_up = set()
    for proc, pid, tid, msg in _lines(text):
        key = (proc, pid)
        if _DRIVER.match(msg):
            driver_up.add(key)
        m = _RADV.match(msg)
        if m:
            radv[key] = (_unq(m.group(1)), _unq(m.group(2))); continue
        m = _RADV_REJECT.match(msg)
        if m:
            rejects.append("%s: %s = %s" % (proc, m.group(1), _unq(m.group(2)))[:160] + " - " + m.group(3)); continue
        m = _PARANOID.match(msg)
        if m:
            paranoid[key] = (int(m.group(2)), m.group(3), m.group(4) or ""); continue
        m = _PARANOID_OFF.match(msg)
        if m:
            paranoid_off.append("%s (NVMTL_PARANOID=%s)" % (proc, m.group(1))); continue
        m = _CONF_SET.match(msg)
        if m:
            conf_set.setdefault(m.group(1), set()).add(proc); continue
        m = _CONF_SKIP.match(msg)
        if m:
            conf_skip.append("%s: %s" % (proc, m.group(1))); continue
        if _SWITCHES_NONE.match(msg):
            switch_none.add(key); switch_procs.add(key); continue
        m = _SWITCHES.match(msg)
        if m:
            switch_procs.add(key)
            for k, v in _TOKEN.findall(m.group(1)):
                switches.setdefault((k, _unq(v)), set()).add(proc)
            continue
        m = _SUM_AB.match(msg)
        if m:
            ab[key] = _kv(m.group(1)); continue
        m = _SUM_MG.match(msg)
        if m:
            mg[key] = _kv(m.group(1)); continue
        m = _SUM_SIN.match(msg)
        if m:
            kv = _kv(m.group(1))
            # a timer tick and the exit line can reach the log out of order: keep the highest seq
            if key not in sin or _seq(kv) >= _seq(sin[key]):
                sin[key] = kv
            continue
        m = _WIT.match(msg)
        if m:
            old = wit_lines.get(key, (0, 0)); wit_lines[key] = (old[0] + 1, max(old[1], int(m.group(2)))); continue
        if _POOL_OFF.match(msg):
            pool_off.add(proc)
        elif _MG_ON.match(msg):
            mg_on.add(key)

    names = lambda keys: ", ".join(sorted({k[0] if isinstance(k, tuple) else k for k in keys}))
    # the RADV profile: every process named, set or not; an empty value is not a profile
    if radv:
        on = {k: v for k, v in radv.items() if (v[0] or v[1])}
        off = {k for k in radv if k not in on}
        if on:
            rows.append(("WARN", "radv debug", "RADV_DEBUG / RADV_PERFTEST were SET in %s (%s): this run does not show the default driver; a picture that is right with it and "
                         "wrong without it is compression metadata (docs/AFTER-TESTS.md, Rendering artefacts)%s" % (
                             names(on), "; ".join(sorted({"%s: RADV_DEBUG=%s RADV_PERFTEST=%s" % (k[0], v[0] or "unset", v[1] or "unset") for k, v in on.items()}))[:240],
                             ("; unset in " + names(off)) if off else "")))
        else:
            rows.append(("INFO", "radv debug", "RADV_DEBUG / RADV_PERFTEST unset in %s: the default driver" % names(off)))
    for k in sorted(driver_up - set(radv)):
        rows.append(("WARN", "radv debug", "%s opened the driver but logged no RADV flags: the plugin is older than DRIVER-PLAN N50, NVMTL_RADV_DEBUG would have done nothing" % k[0]))
    for r in rejects[:4]:
        rows.append(("WARN", "radv debug", "%s: the A/B did not run" % r))
    # what the conf file did
    for msg, procs_ in sorted(conf_set.items()):
        rows.append(("INFO", "plugin conf", "applied from rdna3mtl.conf / the profile in %s: %s" % (names(procs_), msg[:300])))
    for s in sorted(set(conf_skip))[:6]:
        rows.append(("WARN", "plugin conf", "a conf line was NOT applied - %s" % s))
    if paranoid:
        rows.append(("WARN", "paranoid profile", "NVMTL_PARANOID was on in %s: %s; this run does not show the default plugin" % (
            names(paranoid), "; ".join(sorted({"%s: %d set by it (%s)%s" % (k[0], v[0], v[1], (", kept: " + v[2]) if v[2] else "") for k, v in paranoid.items()}))[:300])))
    for p_ in paranoid_off[:3]:
        rows.append(("WARN", "paranoid profile", "%s: the value is not 1, the profile is OFF" % p_))
    # every switch that is SET in a process
    if switches:
        by = {}
        for (k, v), ps in sorted(switches.items()):
            by.setdefault(k, []).append("%s in %s" % (v, ", ".join(sorted(ps))))
        rows.append(("WARN", "plugin switch", "set in the environment of the plugin's processes: %s - this run does not show the default plugin" % "; ".join(
            "%s=%s" % (k, " / ".join(v)) for k, v in sorted(by.items()))[:700]))
    elif switch_none:
        rows.append(("INFO", "plugin switch", "no NVMTL_ switch set in %s" % names(switch_none)))
    # processes that told which switches they have and never printed a SUMMARY: nothing about their counters is known
    nosum = sorted({k[0] for k in switch_procs if k not in ab and k not in mg})
    if nosum:
        rows.append(("INFO", "ab summary", "%s logged `vk: ab switches` but no `vk: ab: SUMMARY` / `managed: SUMMARY` line: the counters are UNKNOWN there (the process ended before its first "
                     "10 s tick, or ran no LOAD pass and no witnessed submit); the load pass and fence rows say nothing about it" % ", ".join(nosum)))
    old_plugin = sorted({k[0] for k in driver_up if k not in switch_procs})
    if old_plugin:
        rows.append(("WARN", "plugin switch", "%s logged no `vk: ab switches` line (an older plugin): which switches were on is not in the report" % ", ".join(old_plugin)))
    # the counters of this file's load passes and the fence
    for key, d in sorted(ab.items()):
        for kind in ("colour", "depth"):
            allc, bad = d.get("load_%s_all" % kind, 0), d.get("load_%s_mismatch" % kind, 0)
            if not allc:
                continue
            vec = d.get("%s_by_layout" % kind, [0] * 10)
            parts = ["%s %d" % (nm, c) for nm, c in zip(_LAYOUT_NAMES, vec) if c]
            bar, inrp = d.get("load_%s_barriers" % kind, 0), d.get("load_%s_inrp" % kind, 0)
            rows.append(("INFO", "load pass layouts", "%s %s: %d load pass(es), %d began on an image that did not rest in the declared layout (%s)%s%s" % (
                key[0], kind, allc, bad, ", ".join(parts) or "none", ("; %d transitioned (NVMTL_LOAD_BARRIER)" % bar) if bar else "",
                ("; %d seen with a render pass already open (not counted)" % inrp) if inrp else "")))
        early, ok, worst, blind = d.get("wit_early", 0), d.get("wit_ok", 0), d.get("wit_worst_us", 0), d.get("wit_blind", 0)
        if early:
            rows.append(("WARN", "fence early", "%s: the fence returned before the GPU's last write was visible to the CPU %d time(s) in %d witnessed submits, worst %d us late: a CPU read of "
                         "GPU output right after a fence wait can see stale bytes (the plugin waits for its completion witness; anything that does not is exposed)" % (
                             key[0], early, ok + early, worst)))
        elif key[0] in pool_off:
            rows.append(("INFO", "fence early", "%s: pool reuse is OFF there (NVMTL_NO_POOL_REUSE), so no completion witness was recorded: 'no early fence' is NOT evidence" % key[0]))
        elif ok:
            rows.append(("INFO", "fence early", "%s: %d witnessed submits, no early fence%s" % (key[0], ok, ("; %d submits without a witness" % blind) if blind else "")))
    for key, (n, worst) in sorted(wit_lines.items()):
        if key not in ab:
            rows.append(("WARN", "fence early", "%s: the fence returned before the GPU's last write was visible (%d logged line(s), worst %d us late; no counter line: an older plugin)" % (key[0], n, worst)))
    for p_ in sorted(pool_off):
        if not any(k[0] == p_ for k in ab):
            rows.append(("INFO", "fence early", "%s: pool reuse is OFF there, so no completion witness was recorded" % p_))
    # the managed upload check / snapshot
    for key, d in sorted(mg.items()):
        chk, snap = d.get("check", 0), d.get("snapshot", 0)
        if not (chk or snap):
            continue
        ce, cv, cs = d.get("changed_exact", 0), d.get("changed_env", 0), d.get("check_skipped", 0)
        sync, up = d.get("sync_uploads", 0), d.get("uploads", 0)
        if ce:
            rows.append(("WARN", "managed shadow", "%s: the CPU rewrote the shadow of a managed buffer %d time(s) under a single didModifyRange (and %d merged ranges changed) while the GPU copy was in flight, of %d uploads "
                         "checked: the GPU may have read a mix of two states (random strokes in a glyph cell)%s" % (key[0], ce, cv, d.get("checked", 0), " - a snapshot was on: the GPU saw the state of the flush" if snap else "")))
        elif chk and d.get("checked", 0):
            rows.append(("INFO", "managed shadow", "%s: %d managed upload(s) checked, none changed under a single didModifyRange%s" % (key[0], d.get("checked", 0), ("; %d merged ranges changed (legitimate gaps)" % cv) if cv else "")))
        if sync:
            rows.append(("WARN", "managed shadow", "%s: %d of %d managed uploads were a synchronous submit, where NVMTL_MANAGED_CHECK / NVMTL_MANAGED_SNAPSHOT are INERT (the pre-submit prologue is off, full or failed): "
                         "the result says nothing about those" % (key[0], sync, up)))
        bad = []
        if cs: bad.append("%d upload(s) not checked (no shadow, or a range past the buffer)" % cs)
        if d.get("snap_failed", 0): bad.append("%d snapshot allocation(s) failed" % d["snap_failed"])
        if d.get("snap_skipped_big", 0): bad.append("%d range(s) over 1 MiB read the shadow itself" % d["snap_skipped_big"])
        if d.get("snap_skipped_noshadow", 0): bad.append("%d range(s) without a shadow" % d["snap_skipped_noshadow"])
        if snap and up and not d.get("snapped", 0): bad.append("the snapshot is on and NO upload was snapshotted")
        if bad:
            rows.append(("WARN", "managed shadow", "%s: %s" % (key[0], "; ".join(bad))))
        elif snap and d.get("snapped", 0):
            rows.append(("INFO", "managed shadow", "%s: %d upload(s) read a private snapshot" % (key[0], d["snapped"])))
    # N51: the surface-in cache (docs/AFTER-TESTS.md "Surface-in cost and the cache")
    for key, d in sorted(sin.items()):
        calls, fast, br = d.get("calls", 0), d.get("fast", 0), d.get("bufcopy", 0)
        miss = d.get("miss_unsynced", 0) + d.get("miss_seed", 0) + d.get("miss_gen", 0)
        refresh_us = d.get("bufcopy_up_us", 0) + d.get("bufcopy_skip_us", 0) + d.get("bufcopy_noshadow_us", 0)
        tot_us = d.get("sin_us_total", 0)
        share = ("%.0f %% of the %.1f ms spent in surface-in calls" % (100.0 * refresh_us / tot_us, tot_us / 1000.0)) if tot_us else "no surface-in time recorded"
        refused = d.get("cimg_refused_size", 0) + d.get("cimg_refused_form", 0) + d.get("cimg_refused_full", 0)
        txt = ("%s (SUMMARY #%d after %.0f s): %d surface-in call(s) in %.1f ms: %d answered by the probe (%.1f ms), %d missed (%d never synced, %d seed moved, %d generation moved) and took the locked path "
               "(%d uploads, %d shared-VRAM skips, %d pending-write skips, %d overwrite skips, %d failures), %d buffer-texture repeat(s) in one command buffer; "
               "%d buffer-texture refresh(es) = %.1f ms = %s: %d uploaded (%.1f ms), %d skipped unchanged (%.1f ms), %d without a shadow (%.1f ms), %d failed, %d dropped by a writer of the image, "
               "%d skips refused by the image's layout; shadows %.1f KB, %d refused; copy images %d new, %d reused, %d parked, %d evicted, %d refused (size %d, shape %d, pool full %d), pool %s" % (
                   key[0], d.get("seq", 0), d.get("t_ms", 0) / 1000.0, calls, tot_us / 1000.0, fast, d.get("fast_us", 0) / 1000.0, miss, d.get("miss_unsynced", 0), d.get("miss_seed", 0), d.get("miss_gen", 0),
                   d.get("locked_upload", 0), d.get("locked_skip_vram", 0), d.get("locked_skip_pending", 0), d.get("locked_skip_overwrite", 0), d.get("locked_fail", 0), d.get("bufcopy_dedup", 0),
                   br, refresh_us / 1000.0, share, d.get("bufcopy_uploaded", 0), d.get("bufcopy_up_us", 0) / 1000.0, d.get("bufcopy_skipped", 0), d.get("bufcopy_skip_us", 0) / 1000.0,
                   d.get("bufcopy_noshadow", 0), d.get("bufcopy_noshadow_us", 0) / 1000.0, d.get("bufcopy_failed", 0), d.get("bufcopy_forgotten", 0), d.get("bufcopy_skip_refused_layout", 0),
                   d.get("shadow_bytes", 0) / 1024.0, d.get("shadow_refused", 0), d.get("cimg_new", 0), d.get("cimg_recycled", 0), d.get("cimg_parked", 0), d.get("cimg_evicted", 0), refused,
                   d.get("cimg_refused_size", 0), d.get("cimg_refused_form", 0), d.get("cimg_refused_full", 0), "ON" if d.get("cimg_on", 1) else "OFF"))
        vchk, vbad = d.get("verify_checked", 0), d.get("verify_mismatch", 0)
        if vbad:
            rows.append(("FAIL", "surface-in cache", "%s: NVMTL_SURFIN_VERIFY found %d STALE image(s) in %d verified skips: the shadow claimed contents the image did not hold. %s" % (key[0], vbad, vchk, txt)))
        elif not d.get("cache", 1):
            rows.append(("WARN", "surface-in cache", "%s: NVMTL_NO_SURFIN_CACHE is set, this run does not show the default plugin. %s" % (key[0], txt)))
        elif d.get("bufcopy_failed", 0):
            rows.append(("WARN", "surface-in cache", "%s: %d buffer-texture refresh(es) FAILED (the image keeps its old pixels, the next refresh uploads again). %s" % (key[0], d["bufcopy_failed"], txt)))
        elif br and d.get("bufcopy_noshadow", 0) * 2 > br:
            rows.append(("WARN", "surface-in cache", "%s: most buffer-texture refreshes had no shadow (images over 1 MiB or the 32 MiB budget: shadow_refused=%d), so they are uploaded every time. %s" % (
                key[0], d.get("shadow_refused", 0), txt)))
        elif d.get("verify", 0) and not vchk:
            rows.append(("WARN", "surface-in cache", "%s: NVMTL_SURFIN_VERIFY=%d is on and no skip was verified (no refresh was skipped). %s" % (key[0], d["verify"], txt)))
        else:
            rows.append(("INFO", "surface-in cache", txt + ("; VERIFY: %d skip(s) read back, 0 stale" % vchk if vchk else "")))
    for key, d in sorted(sin.items()):
        rs = d.get("rsample_by_layout")
        if isinstance(rs, list) and len(rs) == 6 and sum(rs):
            names = ("UNDEFINED", "GENERAL", "SHADER_READ_ONLY", "TRANSFER_SRC", "TRANSFER_DST", "other")
            lie = sum(rs) - rs[2]
            rows.append(("INFO", "sampled layout", "%s: %d render-encoder sample(s) of a texture, %d (%.0f %%) with the image resting in a layout other than the SHADER_READ_ONLY_OPTIMAL the descriptor declares "
                         "and no transition recorded (%s). The host probe (tests/vk_meta_probe.c LIE=1/2, stock RADV 26.2.5, DCC on and off) shows no wrong pixel for it; the plugin's own Mesa is unchecked: "
                         "a non-zero count here with the artefacts is the lead to follow" % (key[0], sum(rs), lie, 100.0 * lie / sum(rs), ", ".join("%s %d" % (n, c) for n, c in zip(names, rs) if c))))
    for key in sorted(switch_procs - set(sin)):
        lvl = "WARN" if key[0] == "WindowServer" else "INFO"
        rows.append((lvl, "surface-in cache", "%s logged `vk: ab switches` but no `vk: surfin: SUMMARY`%s" % (key[0], " - WindowServer is never quit gracefully and the plugin prints the totals from a 10 s timer: an older plugin, or no surface-in call at all" if lvl == "WARN" else " (an older plugin, or no surface-in call)")))
    for key in sorted(mg_on - set(mg)):
        rows.append(("INFO", "managed shadow", "%s: the shadow check / snapshot was on and no managed upload was seen (no didModifyRange upload in that process)" % key[0]))
    return rows


# the ladder: tools/vm/macos/autoreport.sh ladder_run writes ladder.txt (one block per rung: what it ran with, the looks, the answer) and <rung>.log
# (the plugin log slice of the rung's processes); collect.sh packs them as 41-ladder.txt and 41-ladder-<rung>.log. The rung list is the same as
# autoreport.sh's LADDER_RUNGS / LADDER_WS_RUNGS (tests/test_ladder_step.sh compares them)
LADDER_RUNGS = ("baseline", "radv", "sync", "managed", "alignguard", "paranoid", "loadbarrier")
LADDER_WS_RUNGS = ("radv-ws", "paranoid-ws")
# variant 3 (`rdna3vm step ladder-paranoid`): the knobs of NVMTL_PARANOID's composition (the same list in autoreport.sh's LADDER_PARANOID_RUNGS; the test compares)
LADDER_PARANOID_RUNGS = ("baseline", "paranoid", "paranoid-minus-radv-sync", "paranoid-a", "paranoid-b", "gsub-inflight", "no-prologue", "no-pool-reuse", "no-bufreuse",
                         "no-buftex-recycle", "no-heaptex-recycle", "no-heaptex-place", "no-dirtyonly", "enc78-off", "g79-off", "no-res2", "no-purge-reclaim", "no-surfin-cache")
# rung -> the knobs it sets (the halves partition PARANOID_KEYS; tests/test_ladder_step.sh compares these with autoreport.sh's table)
PARANOID_A = ("NVMTL_RADV_DEBUG", "NVMTL_NO_POOL_REUSE", "NVMTL_NO_BUFREUSE", "NVMTL_NO_BUFTEX_RECYCLE", "NVMTL_NO_HEAPTEX_RECYCLE", "NVMTL_NO_HEAPTEX_PLACE",
              "NVMTL_NO_RES2", "NVMTL_NO_PURGE_RECLAIM", "NVMTL_NO_SURFIN_CACHE")
PARANOID_B = ("NVMTL_ASYNC_COMMIT", "NVMTL_GSUB_INFLIGHT", "NVMTL_PRESUBMIT_PROLOGUE", "NVMTL_NO_DIRTYONLY", "NVMTL_ENC78_OFF", "NVMTL_G79_OFF")
# the rung that overrides two of the profile's switches by giving them an explicit value: the profile must then NOT have set them itself
MINUS_RUNG = "paranoid-minus-radv-sync"
MINUS_KEYS = ("NVMTL_RADV_DEBUG", "NVMTL_ASYNC_COMMIT")


def ab_summary_keys(text):
    """the (process, pid) of the processes that printed a `vk: ab: SUMMARY` or `managed: SUMMARY` line"""
    return {(proc, pid) for proc, pid, tid, msg in _lines(text) if _SUM_AB.match(msg) or _SUM_MG.match(msg)}


def ladder_log_name(rung):
    """the archive member of a rung's log slice"""
    return "41-ladder-%s.log" % rung


def parse_ladder(text, logs=None):
    """ladder.txt (+ {rung: log text}) -> (variant, {rung: dict for ladder_rows}, [problems]). A rung block is `== rung NAME` ... `== end rung NAME`; a block
    without its end line is a rung that was cut off (it is kept and marked "cut")."""
    logs = logs or {}
    variant, rungs, cur, problems, ended = 1, {}, None, [], False
    for raw in (text or "").splitlines():
        line = raw.rstrip()
        m = re.match(r"^variant: (\d+)", line)
        if m and cur is None:
            variant = int(m.group(1)); continue
        m = re.match(r"^== rung (\S+?)(?::.*)?$", line)
        if m:
            cur = rungs.setdefault(m.group(1), {"conf": [], "scope": "?", "looks": [], "verdict": None, "cut": True, "warn": [], "name": m.group(1)})
            continue
        m = re.match(r"^== end rung (\S+)", line)
        if m:
            if m.group(1) in rungs:
                rungs[m.group(1)]["cut"] = False
            cur = None; continue
        if line == "== end":
            ended = True; continue
        if cur is None:
            continue
        m = re.match(r"^(scope|conf|looks|answer|pids|log|env|set): ?(.*)$", line)
        if m:
            k, v = m.group(1), m.group(2).strip()
            if k == "scope": cur["scope"] = v
            elif k == "conf": cur["conf"] = [] if v.startswith("(nothing)") else v.split()
            elif k == "looks": cur["looks"] = v.split()
            elif k == "answer": cur["verdict"] = v if v in ("y", "n") else None
            elif k == "pids": cur["pids"] = v
            elif k == "log": cur["logline"] = v
            continue
        if line.startswith("quit: still running after"):
            cur["quit_failed"] = True   # the graceful quit (System Events) did not end the apps: TERM, the plugin's exit SUMMARY is lost
            continue
        if line.startswith("WARN:") or line.startswith("failed:") or line.startswith("NOT TAKEN:"):
            cur["warn"].append(line[:200])
    for name, r in rungs.items():
        r["log"] = logs.get(name, "")
    if not ended:
        problems.append("ladder.txt has no closing '== end' line: the run was cut short or is still going")
    return variant, rungs, problems


def _minus_rung_problems(log):
    """paranoid-minus-radv-sync must be the profile WITHOUT its RADV flags and async commit: the plugin's `conf: NVMTL_PARANOID=1 - ... set by it (...)` line must
    not name them, and RADV_DEBUG must have stayed unset (an explicit empty NVMTL_RADV_DEBUG that `open --env` dropped would put the flags back)"""
    out, seen, radv_on = [], False, set()
    for proc, pid, tid, msg in _lines(log):
        m = _PARANOID.match(msg)
        if m:
            seen = True
            by = {t.split("=", 1)[0] for t in (m.group(3) or "").split() if "=" in t}
            bad = sorted(k for k in MINUS_KEYS if k in by)
            if bad:
                out.append("%s: the profile set %s itself, the explicit override did not reach the process (this rung is the whole profile)" % (proc, ", ".join(bad)))
            continue
        m = _RADV.match(msg)
        if m and (_unq(m.group(1)) or _unq(m.group(2))):
            radv_on.add(proc)
    if not seen:
        out.append("no `conf: NVMTL_PARANOID=...` line in the slice: the profile was not on")
    if radv_on:
        out.append("RADV flags were set in %s although this rung must not have them" % ", ".join(sorted(radv_on)))
    return out


_PARK = re.compile(r"^buffer park \((?:park|reuse)\): (\d+) parked, (\d+) reused")
_HEAPTEX = re.compile(r"^heap texture(?: config:| \()")
_BUFTEX = re.compile(r"^buffer texture(?: \((?:new|recycled)\)|: dres R3)")
_RES2 = re.compile(r"^residency \(res2\): (?:evict-batch|waited)")
_RES1 = re.compile(r"^residency \(res1\): the wall is")
_GSUB = re.compile(r"^commit: G-SUB (\d+) parts submitted ahead, peak (\d+) in flight")


def knob_evidence(log, proc="Terminal"):
    """{rung: (exercised, text)} for the single-knob rungs of the PARANOID ladder, from the plugin log of a rung that ran WITHOUT the knob (the baseline): did the
    code the knob switches off run in `proc` at all? A knob whose code never ran in the process cannot change its picture (False); None = the log cannot tell."""
    park = heap = buftex = res2 = res1 = 0
    peak = 0
    mg, sin = {}, {}
    for p, pid, tid, msg in _lines(log):
        if p != proc:
            continue
        m = _PARK.match(msg)
        if m:
            park = max(park, int(m.group(1)) + int(m.group(2)))
        heap += bool(_HEAPTEX.match(msg)); buftex += bool(_BUFTEX.match(msg)); res2 += bool(_RES2.match(msg)); res1 += bool(_RES1.match(msg))
        m = _GSUB.match(msg)
        if m:
            peak = max(peak, int(m.group(2)))
        m = _SUM_MG.match(msg)
        if m:
            mg = _kv(m.group(1))
        m = _SUM_SIN.match(msg)
        if m:
            sin = _kv(m.group(1))
    seen = bool(mg or sin or peak or heap or buftex or park)   # a Terminal log at all
    if not seen:
        return {}

    def ev(flag, yes, no):
        return (flag, yes if flag else no)
    up = mg.get("prologue_uploads", 0)
    out = {
        "no-prologue": ev(up > 0, "%d managed upload(s) rode a submit's prologue" % up, "no managed upload rode a prologue"),
        "gsub-inflight": ev(peak > 1, "up to %d parts were in flight" % peak, "at most 1 part was ever in flight, so a cap of 1 changes nothing"),
        "no-bufreuse": ev(park > 0, "%d buffer(s) parked or reused" % park, "no buffer was ever parked or reused (no `buffer park (...)` line)"),
        "no-heaptex-place": ev(heap > 0, "a heap texture was made", "no texture was ever made from a heap (no `heap texture` line)"),
        "no-heaptex-recycle": ev(heap > 0, "a heap texture was made", "no texture was ever made from a heap (no `heap texture` line)"),
        "no-buftex-recycle": ev(buftex > 0 or sin.get("cimg_new", 0) + sin.get("cimg_recycled", 0) > 0, "a buffer-backed texture was made",
                                "no texture was ever made over a buffer (no `buffer texture (new|recycled)` line)"),
        "no-res2": ev(res2 > 0, "the pager moved textures", "the pager never ran (no `evict-batch` line)"),
        "no-purge-reclaim": ev(res1 > 0, "textures were purged under VRAM pressure", "no purge under VRAM pressure ever happened (no `the wall is` line)"),
        "no-surfin-cache": ev(sin.get("bufcopy", 0) + sin.get("cimg_new", 0) > 0 or sin.get("miss_seed", 0) + sin.get("miss_gen", 0) > 0,
                              "the cache decided something", "no buffer texture was refreshed and no cached decision was ever missed: the cache only answered the probe"),
    }
    return out


def glyph_verdicts(rungs, prefix):
    """{rung: {"verdict", "text"}} of the automatic glyph check (tools/glyph_verdict.py) on the rung's hires look(s): `<prefix>-scanout<look>-pipe<N>.ppm`, prefix
    = the report's path without .tar.gz. A rung with no look file is left out; ImportError / a bad file is a `none` with the reason, never an exception."""
    try:
        import glyph_verdict as gv
    except ImportError:
        return {n: {"verdict": "none", "text": "tools/glyph_verdict.py is missing next to triage_glyphs.py: the glyphs were NOT checked"} for n in rungs}
    out = {}
    for name, r in rungs.items():
        files = []
        for look in r.get("looks") or []:
            if look.endswith("h"):
                files += sorted(glob.glob(glob.escape(prefix) + "-scanout" + glob.escape(look) + "-pipe*.ppm"))
        if not files:
            continue
        try:
            out[name] = gv.verdict_files(files)
        except Exception as e:  # noqa: BLE001  (one bad picture must not take the triage down)
            out[name] = {"verdict": "none", "text": "the glyph check FAILED (%s: %s)" % (type(e).__name__, e)}
    return out


def ladder_rows(rungs, variant=1, problems=(), glyphs=None):
    """rungs: {rung name: {"conf": [lines], "scope": "apps"|"windowserver", "log": plugin log slice, "looks": [file names], "verdict": "y"|"n"|None}}.
    One row per rung: what it set, which processes show it in the log (the plugin's own `vk: ab switches` line), the load pass / fence / managed counters
    of the slice, and the owner's verdict when one was given. [(level, name, text)]."""
    rows = [("WARN", "ladder", p) for p in problems]
    names = LADDER_PARANOID_RUNGS if variant == 3 else LADDER_RUNGS + (LADDER_WS_RUNGS if variant == 2 else ())
    glyphs = glyphs or {}
    evid = knob_evidence((rungs.get("baseline") or {}).get("log", "")) if variant == 3 else {}
    for name in names:
        r = rungs.get(name)
        if r is None:
            rows.append(("WARN", "ladder " + name, "the rung did not run or left nothing in the report"))
            continue
        sub = n50_plugin_rows(r.get("log", ""))
        warn = [t for lvl, nm, t in sub if lvl == "WARN" and nm in ("fence early", "managed shadow", "plugin conf")]
        looks = r.get("looks") or []
        v = r.get("verdict")
        # the switches the rung asked for against the keys the processes' own `vk: ab switches` lines show: a missing key = it did not reach that process,
        # an extra one = not the default plugin (a leftover conf line in the baseline, or a rung that set more than it should)
        want = {c.split("=", 1)[0] for c in (r.get("conf") or [])}
        allowed = set(want) | (set(PARANOID_KEYS) | {"NVMTL_PARANOID"} if "NVMTL_PARANOID" in want else set())
        procs = ab_switch_keys(r.get("log", ""))
        miss = sorted({"%s: %s" % (k[0], ", ".join(sorted(want - ks))) for k, ks in procs.items() if want - ks})
        stray = sorted({"%s: %s" % (k[0], ", ".join(sorted(ks - allowed))) for k, ks in procs.items() if ks - allowed})
        reached = not want or (bool(procs) and not miss)
        problems_ = []
        if want and not procs:
            problems_.append("the plugin log slice has no `vk: ab switches` line, so the rung did not reach the plugin")
        elif miss:
            problems_.append("the rung did not reach the plugin (a switch it set is missing from `vk: ab switches` of %s)" % "; ".join(miss))
        if stray:
            problems_.append("not default: %s set in %s although the rung did not ask for it" % ("; ".join(stray), "the process" if len(stray) == 1 else "those processes"))
        have_sum = ab_summary_keys(r.get("log", ""))
        nosum = sorted({k[0] for k in procs if k not in have_sum})
        if nosum:
            problems_.append("no SUMMARY line from %s: the counters are unknown there" % ", ".join(nosum))
        txt = "set %s (%s); %d look(s); %s%s" % (", ".join(r.get("conf") or []) or "nothing", r.get("scope", "?"), len(looks),
                                                 ("verdict " + {"y": "PICTURE RIGHT", "n": "picture broken"}.get(v, str(v))) if v else "no verdict (the owner was not asked or did not answer)",
                                                 ("; counters: " + " | ".join(warn)[:200]) if warn else "")
        gl = glyphs.get(name)
        if gl:
            txt += "; glyphs (automatic, tools/glyph_verdict.py): " + gl["text"]
        ev = evid.get(name)
        if ev:
            txt += "; in the baseline Terminal " + ev[1] + ("" if ev[0] else " - this rung CANNOT differ from the baseline")
        extra = problems_ + list(r.get("warn") or [])
        if name == MINUS_RUNG:
            extra += _minus_rung_problems(r.get("log", ""))
        if warn:
            extra.append("a counter or conf WARN in the slice")
        if r.get("cut"):
            extra.append("the rung has no end line (cut off)")
        if r.get("log") == "" and r.get("conf"):
            extra.append("no plugin log slice for this rung")
        if extra:
            txt += "; " + "; ".join(extra)[:240]
        rows.append(("INFO" if not extra else "WARN", "ladder " + name, txt))
    nq = [n for n in names if (rungs.get(n) or {}).get("quit_failed")]
    if nq:
        rows.append(("INFO", "ladder quit", "%d of %d rung(s): the apps did not quit when asked through System Events and were TERMed after 8 s (macOS may be waiting for the one-time Automation "
                     "prompt for osascript, or the Terminal asks to terminate its running process); the plugin's exit SUMMARY is lost there, its 10 s counters are not" % (len(nq), len(names))))
    # the automatic glyph verdict: which rung made the text clean
    gv_ = {n: glyphs[n]["verdict"] for n in names if n in glyphs}
    if gv_ and all(v == "none" for v in gv_.values()):
        rows.append(("WARN", "ladder glyphs", "the automatic glyph check found no tick lines in any rung's look (the text.command window was not on a screen, or the looks are missing): %s" % next(iter(glyphs.values()))["text"]))
    elif gv_:
        clean = [n for n in names if gv_.get(n) == "clean"]
        if gv_.get("baseline") == "broken" and clean:
            rows.append(("INFO", "ladder glyphs", "the baseline glyphs were BROKEN and CLEAN with: %s (tools/glyph_verdict.py: identical tick lines are drawn identically)" % ", ".join(clean)))
        elif gv_.get("baseline") == "broken":
            tail = ""
            if variant == 3 and gv_.get("paranoid") != "clean":
                tail = ("; the positive control (paranoid, every knob of the profile) is NOT clean either, so none of the knobs of this ladder is the cure and the single-knob "
                        "rungs say nothing - the cause is outside what NVMTL_PARANOID changes (docs/AFTER-TESTS.md)")
            rows.append(("INFO", "ladder glyphs", "the baseline glyphs were BROKEN and no rung made them clean%s" % tail))
        elif gv_.get("baseline") == "clean":
            rows.append(("INFO", "ladder glyphs", "the baseline glyphs were CLEAN in this run: the artefact did not show, the other rungs prove nothing"))
        else:
            rows.append(("INFO", "ladder glyphs", "the baseline's glyphs could not be judged (%s)" % (gv_.get("baseline") or "no look")))
    if variant == 3 and evid:
        dead = [n for n in LADDER_PARANOID_RUNGS if n in evid and not evid[n][0]]
        if dead:
            rows.append(("INFO", "ladder knobs", "rungs whose switch has no code to switch off in the baseline Terminal (they cannot differ from the baseline): %s" % ", ".join(dead)))
    # the verdict of the whole ladder: which rung made the picture right
    ans = {n: rungs[n].get("verdict") for n in names if n in rungs}
    if ans.get("baseline") == "n":
        fixed = [n for n, v in ans.items() if v == "y"]
        rows.append(("INFO", "ladder", ("the baseline picture was broken and the picture was RIGHT with: %s - that switch is the discriminator (docs/AFTER-TESTS.md)" % ", ".join(fixed)) if fixed
                     else "the baseline picture was broken and no rung made it right"))
    elif ans.get("baseline") == "y":
        rows.append(("INFO", "ladder", "the baseline picture was right in this run: the artefact did not show, the other rungs prove nothing"))
    elif ans:
        rows.append(("INFO", "ladder", "no owner verdicts (a timer run, or no answer): compare the rungs' looks (41-ladder.txt names them, <report>-scanoutladder-<rung>h-pipe*.ppm)"))
    return rows
