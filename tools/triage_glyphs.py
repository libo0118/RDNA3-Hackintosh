"""Triage rows of DRIVER-PLAN N49 (card run 2026-10-25e: broken glyphs in Terminal / Maps, tile-shaped blocks of wrong colour in Chess).
The plugin gained a set of A/B switches and counters for those artefacts; this module reads their lines from the plugin logs
(26-log-metal-plugin.txt = the other processes, 24-log-coredisplay.txt = WindowServer; lines `... Name[pid:tid] (RDNA3MTLDriver) NVMTL: msg`),
so that a report says which profile it was taken with and what the counters saw. Kept in a module of its own so that tools/triage_reports.py
only gains one call. docs/AFTER-TESTS.md "Rendering artefacts: the A/B ladder" explains every line."""
import re

_LINE = re.compile(r"\b([^\s\[\]]+)\[(\d+):([0-9a-f]+)\] \(RDNA3MTLDriver\) NVMTL: (.*)$")
_RADV = re.compile(r"^vk: RADV_DEBUG=(\S*) RADV_PERFTEST=(\S*) \(NVMTL_RADV_DEBUG=(\S*), NVMTL_RADV_PERFTEST=(\S*)\)")
_RADV_REJECT = re.compile(r"^vk: (NVMTL_RADV_\w+)=(\S*) REJECTED")
_PARANOID = re.compile(r"^conf: NVMTL_PARANOID=1 - the diagnostic profile is ON \((\d+) switch")
_LOADPASS = re.compile(r"^vk: load pass (colour|depth) of an image resting in layout (\d+), the pass declares (\d+) \[(\d+) such pass\(es\) so far; per layout "
                       r"\(UNDEFINED GENERAL COLOR DS DSRO SRO SRC DST PREINIT other\): ((?:\d+ ?){10})\]")
_LOADBAR = re.compile(r"^vk: load passes: the declared initial layout is (MADE TRUE|not enforced)")
_WIT = re.compile(r"^vk: keep witness (\d+) landed only (\d+) us AFTER the flush returned(?: - .*?\[(\d+) time\(s\) so far in (\d+) witnessed submit\(s\), worst (\d+) us\])?")
_MGCHG = re.compile(r"^managed: the CPU shadow of a (\d+)-byte buffer CHANGED .*?\((one didModifyRange|several didModifyRange).*?\[(\d+) exact, (\d+) envelope of several marks, in (\d+) uploads checked\]")
_MGSUM = re.compile(r"^managed: shadow check: (\d+) upload\(s\) checked, (\d+) changed under a single didModifyRange, (\d+) merged ranges changed")
_MGON = re.compile(r"^managed: shadow check ON")
_SNAP = re.compile(r"^managed: upload snapshot ON")
_SYNC = re.compile(r"^commit: synchronous \(NVMTL_ASYNC_COMMIT=0\)")
_DRIVER = re.compile(r"^vk: driver opened directly")
_LAYOUT_NAMES = ("UNDEFINED", "GENERAL", "COLOR", "DS", "DSRO", "SRO", "SRC", "DST", "PREINIT", "other")


def _lines(text):
    for raw in (text or "").splitlines():
        m = _LINE.search(raw)
        if m:
            yield m.group(1), int(m.group(2)), m.group(3), m.group(4)


def n49_plugin_rows(text=""):
    """[(level, name, text)]. WARN: a debug / diagnostic profile was on (the run does not show the default plugin), the fence returned before the GPU's
    last write was visible, the CPU rewrote a managed buffer's range while its copy was in flight, a RADV flag was rejected, a plugin without the lines.
    INFO: the counters of what the load passes declare versus what the images rest in."""
    rows = []
    radv = {}                   # process -> (RADV_DEBUG, RADV_PERFTEST) of its newest line
    rejects = []
    paranoid = {}               # process -> switches
    loadpass = {}               # (process, kind) -> (count, [10 per-layout counts])
    loadbar = set()
    wit = {}                    # process -> (times, submits, worst_us, first_us)
    mg_changed = {}             # process -> (exact, envelope, checked)
    mg_sum = {}                 # process -> (checked, exact, envelope)
    mg_on = set()
    switches = {}               # label -> processes: the diagnostic switches (besides RADV_DEBUG and PARANOID) that were on
    driver_up = set()
    for proc, pid, tid, msg in _lines(text):
        key = (proc, pid)
        if _DRIVER.match(msg):
            driver_up.add(key)
        m = _RADV.match(msg)
        if m:
            radv[key] = (m.group(1), m.group(2)); continue
        m = _RADV_REJECT.match(msg)
        if m:
            rejects.append("%s %s=%s" % (proc, m.group(1), m.group(2))); continue
        m = _PARANOID.match(msg)
        if m:
            paranoid[key] = int(m.group(1)); continue
        m = _LOADPASS.match(msg)
        if m:
            vec = [int(x) for x in m.group(5).split()]
            k = (key, m.group(1))
            if k not in loadpass or int(m.group(4)) >= loadpass[k][0]:
                loadpass[k] = (int(m.group(4)), vec)
            continue
        m = _LOADBAR.match(msg)
        if m:
            if m.group(1) == "MADE TRUE":
                loadbar.add(key)
            continue
        m = _WIT.match(msg)
        if m:
            n = int(m.group(3)) if m.group(3) else 1
            sub = int(m.group(4)) if m.group(4) else 0
            worst = int(m.group(5)) if m.group(5) else int(m.group(2))
            old = wit.get(key)
            if not old or n >= old[0]:
                wit[key] = (n, sub, max(worst, old[2] if old else 0), old[3] if old else int(m.group(2)))
            continue
        m = _MGCHG.match(msg)
        if m:
            mg_changed[key] = (int(m.group(3)), int(m.group(4)), int(m.group(5))); continue
        m = _MGSUM.match(msg)
        if m:
            mg_sum[key] = (int(m.group(1)), int(m.group(2)), int(m.group(3))); continue
        if _MGON.match(msg):
            mg_on.add(key)
        elif _SNAP.match(msg):
            switches.setdefault("NVMTL_MANAGED_SNAPSHOT=1", set()).add(proc)
        elif _SYNC.match(msg):
            switches.setdefault("NVMTL_ASYNC_COMMIT=0", set()).add(proc)

    names = lambda keys: ", ".join(sorted({k[0] for k in keys}))
    # the RADV profile
    if radv:
        on = {k: v for k, v in radv.items() if v[0] != "unset" or v[1] != "unset"}
        if on:
            rows.append(("WARN", "radv debug", "RADV_DEBUG / RADV_PERFTEST were SET in %s (%s): this run does not show the default driver; a picture that is right with it and wrong "
                         "without it is compression metadata (docs/AFTER-TESTS.md, Rendering artefacts)" % (
                             names(on), "; ".join(sorted({"%s: RADV_DEBUG=%s RADV_PERFTEST=%s" % (k[0], v[0], v[1]) for k, v in on.items()}))[:200])))
        else:
            rows.append(("INFO", "radv debug", "RADV_DEBUG / RADV_PERFTEST unset in %d process(es): the default driver" % len(radv)))
    elif driver_up:
        rows.append(("WARN", "radv debug", "%d process(es) opened the driver but none logged its RADV flags: the plugin is older than DRIVER-PLAN N49, "
                     "NVMTL_RADV_DEBUG would have done nothing" % len(driver_up)))
    for r in rejects[:4]:
        rows.append(("WARN", "radv debug", "%s was REJECTED (only lowercase letters, digits, '_' and ',' are taken): the A/B did not run" % r))
    if paranoid:
        rows.append(("WARN", "paranoid profile", "NVMTL_PARANOID=1 was on in %s (%d switches: no async commit, no recycling or reuse, no barrier folding, no residency paging, "
                     "RADV metadata off): this run does not show the default plugin" % (names(paranoid), max(paranoid.values()))))
    for label in sorted(switches):
        rows.append(("WARN", "plugin switch", "%s was on in %s: this run does not show the default plugin" % (label, ", ".join(sorted(switches[label])))))
    for key in sorted(loadbar):
        rows.append(("WARN", "plugin switch", "NVMTL_LOAD_BARRIER=1 was on in %s: this run does not show the default plugin" % key[0]))
    # what the load passes declare versus the layouts the images rest in
    for (key, kind), (n, vec) in sorted(loadpass.items(), key=lambda kv: (kv[0][0][0], kv[0][1])):
        parts = ["%s %d" % (nm, c) for nm, c in zip(_LAYOUT_NAMES, vec) if c]
        rows.append(("INFO", "load pass layouts", "%s %s: %d load pass(es) began on an image that did not rest in the declared layout (all load passes by tracked layout: %s)%s" % (
            key[0], kind, n, ", ".join(parts), "; NVMTL_LOAD_BARRIER was on, they were transitioned" if key in loadbar else "")))
    # the fence that returns before the data is there
    for key, (n, sub, worst, first) in sorted(wit.items()):
        rows.append(("WARN", "fence early", "%s: the fence returned before the GPU's last write was visible to the CPU %d time(s)%s, worst %d us late: a CPU read of GPU output "
                     "right after a fence wait can see stale bytes (the plugin waits for its completion witness; anything that does not is exposed)" % (
                         key[0], n, (" in %d witnessed submits" % sub) if sub else "", worst)))
    # a managed upload whose source was rewritten while the GPU copy was in flight
    for key, (ex, env, checked) in sorted(mg_changed.items()):
        rows.append(("WARN", "managed shadow", "%s: the CPU rewrote the shadow of a managed buffer %d time(s) under a single didModifyRange (and %d merged ranges changed) while the GPU "
                     "copy was in flight, of %d uploads checked: the GPU may have read a mix of two states (random strokes in a glyph cell)" % (key[0], ex, env, checked)))
    for key, (checked, ex, env) in sorted(mg_sum.items()):
        if key not in mg_changed and not ex and not env:
            rows.append(("INFO", "managed shadow", "%s: %d managed upload(s) checked, none changed between the flush and the GPU's completion" % (key[0], checked)))
    for key in sorted(mg_on - set(mg_sum) - set(mg_changed)):
        rows.append(("INFO", "managed shadow", "%s: the shadow check was on and no managed upload was checked (no didModifyRange upload in that process)" % key[0]))
    return rows
