"""Triage rows of DRIVER-PLAN N50 (card run 2026-10-25e: broken glyphs in Terminal / Maps, tile-shaped blocks of wrong colour in Chess).
The plugin gained a set of A/B switches and counters for those artefacts; this module reads their lines from the plugin logs
(26-log-metal-plugin.txt = the other processes, 24-log-coredisplay.txt = WindowServer; lines `... Name[pid:tid] (RDNA3MTLDriver) NVMTL: msg`,
process names may contain blanks: "Activity Monitor"), so that a report says which profile it was taken with and what the counters saw.
Kept in a module of its own so that tools/triage_reports.py only gains one call. docs/AFTER-TESTS.md "Rendering artefacts: the A/B ladder"
explains every line. Also per-rung summaries of the ladder (`parse_ladder` + `ladder_rows`, tools/vm/macos/autoreport.sh ladder_run: 41-ladder.txt, 41-ladder-<rung>.log)."""
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
            sin[key] = _kv(m.group(1)); continue
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
        txt = ("%s: %d surface-in call(s): %d answered by the probe, %d missed (%d never synced, %d seed moved, %d generation moved) and took the locked path (%d uploads, %d shared-VRAM skips); "
               "%d buffer-texture refresh(es): %d uploaded (%.1f ms), %d skipped unchanged (%.1f ms), %d without a shadow; copy images %d new, %d reused, %d parked, %d evicted" % (
                   key[0], calls, fast, miss, d.get("miss_unsynced", 0), d.get("miss_seed", 0), d.get("miss_gen", 0), d.get("locked_upload", 0), d.get("locked_skip_vram", 0),
                   br, d.get("bufcopy_uploaded", 0), d.get("bufcopy_up_us", 0) / 1000.0, d.get("bufcopy_skipped", 0), d.get("bufcopy_skip_us", 0) / 1000.0,
                   d.get("bufcopy_noshadow", 0), d.get("cimg_new", 0), d.get("cimg_recycled", 0), d.get("cimg_parked", 0), d.get("cimg_evicted", 0)))
        if not d.get("cache", 1):
            rows.append(("WARN", "surface-in cache", "%s: NVMTL_NO_SURFIN_CACHE is set, this run does not show the default plugin. %s" % (key[0], txt)))
        elif d.get("bufcopy_failed", 0):
            rows.append(("WARN", "surface-in cache", "%s: %d buffer-texture refresh(es) FAILED (the image keeps its old pixels, the next refresh uploads again). %s" % (key[0], d["bufcopy_failed"], txt)))
        elif br and d.get("bufcopy_noshadow", 0) * 2 > br:
            rows.append(("WARN", "surface-in cache", "%s: most buffer-texture refreshes had no shadow (images over 1 MiB or the 32 MiB budget: shadow_refused=%d), so they are uploaded every time. %s" % (
                key[0], d.get("shadow_refused", 0), txt)))
        else:
            rows.append(("INFO", "surface-in cache", txt))
    for key in sorted(switch_procs - set(sin)):
        rows.append(("INFO", "surface-in cache", "%s logged `vk: ab switches` but no `vk: surfin: SUMMARY` (an older plugin, or no surface-in call)" % key[0]))
    for key in sorted(mg_on - set(mg)):
        rows.append(("INFO", "managed shadow", "%s: the shadow check / snapshot was on and no managed upload was seen (no didModifyRange upload in that process)" % key[0]))
    return rows


# the ladder: tools/vm/macos/autoreport.sh ladder_run writes ladder.txt (one block per rung: what it ran with, the looks, the answer) and <rung>.log
# (the plugin log slice of the rung's processes); collect.sh packs them as 41-ladder.txt and 41-ladder-<rung>.log. The rung list is the same as
# autoreport.sh's LADDER_RUNGS / LADDER_WS_RUNGS (tests/test_ladder_step.sh compares them)
LADDER_RUNGS = ("baseline", "radv", "sync", "managed", "alignguard", "paranoid", "loadbarrier")
LADDER_WS_RUNGS = ("radv-ws", "paranoid-ws")


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
        if line.startswith("WARN:") or line.startswith("failed:") or line.startswith("NOT TAKEN:"):
            cur["warn"].append(line[:200])
    for name, r in rungs.items():
        r["log"] = logs.get(name, "")
    if not ended:
        problems.append("ladder.txt has no closing '== end' line: the run was cut short or is still going")
    return variant, rungs, problems


def ladder_rows(rungs, variant=1, problems=()):
    """rungs: {rung name: {"conf": [lines], "scope": "apps"|"windowserver", "log": plugin log slice, "looks": [file names], "verdict": "y"|"n"|None}}.
    One row per rung: what it set, which processes show it in the log (the plugin's own `vk: ab switches` line), the load pass / fence / managed counters
    of the slice, and the owner's verdict when one was given. [(level, name, text)]."""
    rows = [("WARN", "ladder", p) for p in problems]
    names = LADDER_RUNGS + (LADDER_WS_RUNGS if variant == 2 else ())
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
        extra = problems_ + list(r.get("warn") or [])
        if warn:
            extra.append("a counter or conf WARN in the slice")
        if r.get("cut"):
            extra.append("the rung has no end line (cut off)")
        if r.get("log") == "" and r.get("conf"):
            extra.append("no plugin log slice for this rung")
        if extra:
            txt += "; " + "; ".join(extra)[:240]
        rows.append(("INFO" if not extra else "WARN", "ladder " + name, txt))
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
