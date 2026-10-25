"""Triage rows of DRIVER-PLAN N50 (card run 2026-10-25e: broken glyphs in Terminal / Maps, tile-shaped blocks of wrong colour in Chess).
The plugin gained a set of A/B switches and counters for those artefacts; this module reads their lines from the plugin logs
(26-log-metal-plugin.txt = the other processes, 24-log-coredisplay.txt = WindowServer; lines `... Name[pid:tid] (RDNA3MTLDriver) NVMTL: msg`,
process names may contain blanks: "Activity Monitor"), so that a report says which profile it was taken with and what the counters saw.
Kept in a module of its own so that tools/triage_reports.py only gains one call. docs/AFTER-TESTS.md "Rendering artefacts: the A/B ladder"
explains every line."""
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
    ab, mg = {}, {}            # key -> newest SUMMARY dict
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
    for key in sorted(mg_on - set(mg)):
        rows.append(("INFO", "managed shadow", "%s: the shadow check / snapshot was on and no managed upload was seen (no didModifyRange upload in that process)" % key[0]))
    return rows

