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
_SUM_CEN = re.compile(r"^census: SUMMARY (.*)$")
_SUM_SIN2 = re.compile(r"^vk: surfin: SUMMARY2 (.*)$")
_SUM_FBF = re.compile(r"^vk: fbfetch: SUMMARY (.*)$")
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


def who(key):
    """`Terminal[907]`: the process and its pid (a rung can have two instances of one app)"""
    return "%s[%d]" % (key[0], key[1])


def _ud_rows(mg):
    """rows for the managed buffers' undeclared-write detector (NVMTL_MANAGED_CHECK) and the widened uploads (NVMTL_MANAGED_WHOLE / NVMTL_MANAGED_PAGES)"""
    rows = []
    for key, d in sorted(mg.items()):
        if d.get("whole") or d.get("pages"):
            decl, up = d.get("decl_bytes", 0), d.get("up_bytes", 0)
            rows.append(("WARN", "managed upload", "%s: NVMTL_MANAGED_%s is on, this run does not show the default plugin: %d flush range(s) declared %d byte(s), %d byte(s) were uploaded (%.1fx), %d of "
                         "%d uploads widened%s" % (who(key), "WHOLE" if d.get("whole") else "PAGES", d.get("uploads", 0), decl, up, (up / decl) if decl else 0.0, d.get("widened_uploads", 0), d.get("uploads", 0),
                                                   ("; %d buffer(s) the app wrote WITHOUT any didModifyRange were uploaded whole at a submit (NVMTL_MANAGED_WHOLE)" % d["ud_whole_marks"]) if d.get("ud_whole_marks") else "")))
        if not (d.get("check") or d.get("ud_scans") or d.get("ud_idle_scans")):
            continue
        scans, idle = d.get("ud_scans", 0), d.get("ud_idle_scans", 0)
        hits, ub, late = d.get("ud_hits", 0), d.get("ud_bytes", 0), d.get("ud_late_bytes", 0)
        tail = []
        if d.get("ud_nomirror"): tail.append("%d buffer(s) had no mirror (over 8 MiB, past the 64 MiB of mirrors, or a shadow shorter than the buffer)" % d["ud_nomirror"])
        managed, mirrored = d.get("ud_managed", 0), d.get("ud_mirrors", 0)
        unwatched = max(managed - mirrored - d.get("ud_nomirror", 0), 0)
        if managed and unwatched:
            tail.append("%d of the %d managed buffers made in this process were never watched (never read through `contents`, never declared a range: nothing could be compared)" % (unwatched, managed))
        blind = managed > 0 and unwatched * 3 >= managed
        if d.get("ud_gpu_off"): tail.append("%d buffer(s) the GPU writes back (synchronize) were left out" % d["ud_gpu_off"])
        if "ud_scans" not in d:
            rows.append(("WARN", "undeclared writes", "%s: NVMTL_MANAGED_CHECK is on and the managed SUMMARY has no ud_* counters: an older plugin, the detector did not run" % who(key)))
        elif not (scans or idle):
            rows.append(("INFO", "undeclared writes", "%s: the detector is on and no managed buffer was scanned (none was declared or read through `contents`)%s" % (who(key), ("; " + "; ".join(tail)) if tail else "")))
        elif hits:
            real = max(ub - late, 0)
            rows.append(("WARN", "undeclared writes", "%s: the CPU wrote managed-buffer bytes it did NOT declare with didModifyRange: %d scan(s) of %d (+%d idle) found %d changed byte(s) in %d buffer(s) "
                         "[%d of them declared at the NEXT flush (a race, not a missing declaration), %d never], %d hit(s) in a page the declared range touches (a page-granular copy would have carried them); "
                         "a byte-exact upload leaves them stale in VRAM - hypothesis A of the glyph artefacts: the picture right with NVMTL_MANAGED_WHOLE / NVMTL_MANAGED_PAGES confirms it%s" % (
                             who(key), hits, scans, idle, ub, d.get("ud_buffers", 0), late, real, d.get("ud_samepage_hits", 0), ("; " + "; ".join(tail)) if tail else "")))
        else:
            rows.append(("INFO", "undeclared writes", "%s: %d flush scan(s) + %d idle scan(s) of %d mirrored buffer(s) (%.1f KB): no byte of the WATCHED buffers was written outside a declared range - hypothesis A "
                         "(undeclared CPU writes) is %s%s" % (who(key), scans, idle, mirrored, d.get("ud_mirror_bytes", 0) / 1024.0,
                                                              "NOT excluded: a large share of the managed buffers was not watched" if blind else "not supported for them", ("; " + "; ".join(tail)) if tail else "")))
    return rows


def _same_image_rows(ab):
    rows = []
    for key, d in sorted(ab.items()):
        n = d.get("same_image_copies", 0)
        if not n:
            continue
        ov, gen, tmp, bad, unsafe = d.get("same_image_overlap", 0), d.get("same_image_general", 0), d.get("same_image_tmp", 0), d.get("same_image_tmpfail", 0), d.get("same_image_unsafe", 0)
        txt = ("%s: %d copy(ies) with one image on both sides (%d disjoint, recorded GENERAL -> GENERAL; %d overlapping, %d of them through a scratch image). Before DRIVER-PLAN N50 the layouts were wrong and an "
               "overlapping copy ran in place, which RADV 26.2.5 gets wrong in tile-shaped regions (tests/vk_same_image_probe.c)" % (who(key), n, gen, ov, tmp))
        if bad or unsafe:
            rows.append(("WARN", "same-image copy", txt + "; %d overlapping copy(ies) could NOT use the scratch image (%d scratch allocation(s) failed, %d of a shape it does not cover: 3D, depth, multisample, compressed) "
                         "and ran in place (undefined)" % (bad + unsafe, bad, unsafe)))
        else:
            rows.append(("INFO", "same-image copy", txt))
    return rows


_CEN_LETTERS = {"u": "surface-in uploads (family VRAM)", "U": "surface-in uploads (private image)", "s": "surface-in skips", "!": "surface-in failures", "P": "render passes", "c": "cleared passes",
                "D": "draws", "B": "buffer_image_blit draws", "F": "framebuffer-fetch draws", "E": "blit-encoder copies", "R": "replaceRegion writes", "X": "compute writes", "T": "presents",
                "O": "GPU writes published as a dirty mark", "C": "GPU writes copied out", "?": "publish failures"}


def _census_rows(cen):
    """one INFO row per process that printed `census: SUMMARY`: what touched its IOSurface render targets and how (metal/plugin/nvmtl_surf_census.h)"""
    rows = []
    for key, (d, top) in sorted(cen.items()):
        if not d:
            continue
        txt = ("%s (SUMMARY #%d after %.0f s): %d surface(s)%s; surface-in uploads %d into family VRAM + %d into private images (%d MB), %d skipped, %d failed; render passes %d (%d cleared), draws %d "
               "(%d buffer_image_blit, %d framebuffer fetch); blit-encoder copies %d, replaceRegion %d, compute writes %d, presents %d; GPU writes published: %d dirty mark(s), %d copied out, %d failed" % (
                   who(key), d.get("seq", 0), d.get("t_ms", 0) / 1000.0, d.get("surfaces", 0), (" (+%d event(s) on surfaces past the 128-slot table)" % d["overflow"]) if d.get("overflow") else "",
                   d.get("sin_up_vram", 0), d.get("sin_up_priv", 0), d.get("sin_up_mb", 0), d.get("sin_skip", 0), d.get("sin_fail", 0), d.get("pass", 0), d.get("pass_clear", 0), d.get("draws", 0),
                   d.get("draw_blit", 0), d.get("draw_fbf", 0), d.get("blitenc", 0), d.get("replace", 0), d.get("compute", 0), d.get("present", 0), d.get("out_dirty", 0), d.get("out_copy", 0), d.get("out_fail", 0)))
        if top:
            txt += "; busiest surfaces (id:WxH:V=family VRAM|P=private:counters): " + top[:300]
        lvl = "WARN" if d.get("sin_fail") or d.get("out_fail") else "INFO"
        rows.append((lvl, "surface census", txt))
    return rows


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
    ab, mg, sin, cen = {}, {}, {}, {}   # key -> newest SUMMARY dict (cen: (dict, the top= list))
    sin2 = {}                  # key -> {seq: the SUMMARY2 line's numbers}
    fbf = {}                   # key -> newest `vk: fbfetch: SUMMARY`
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
        m = _SUM_SIN2.match(msg)
        if m:
            kv = _kv(m.group(1))
            sin2.setdefault(key, {})[_seq(kv)] = kv   # the second half of a surfin SUMMARY (same seq as its first line)
            continue
        m = _SUM_CEN.match(msg)
        if m:
            body, _sep, top = m.group(1).partition(" top=")   # (top= holds "id:WxH:V:u1,P2;..." - not key=value numbers)
            kv = _kv(body)
            if key not in cen or _seq(kv) >= _seq(cen[key][0]):
                cen[key] = (kv, top.strip())
            continue
        m = _SUM_FBF.match(msg)
        if m:
            fbf[key] = _kv(m.group(1)); continue
        m = _WIT.match(msg)
        if m:
            old = wit_lines.get(key, (0, 0)); wit_lines[key] = (old[0] + 1, max(old[1], int(m.group(2)))); continue
        if _POOL_OFF.match(msg):
            pool_off.add(proc)
        elif _MG_ON.match(msg):
            mg_on.add(key)

    for key, d in sin.items():   # a surfin SUMMARY is two lines (nvlog cuts at 1023 characters): the second one of the same seq completes the first
        d.update(sin2.get(key, {}).get(_seq(d), {}))
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
    nocen = sorted({who(k) for k in switch_procs if k not in cen})
    if nocen:
        rows.append(("INFO", "census summary", "%s logged `vk: ab switches` but no `census: SUMMARY`: the surface census is UNKNOWN there (no IOSurface render target was touched, or the process ended before the "
                     "first 10 s tick, or a plugin older than the census)" % ", ".join(nocen)))
    nofbf = sorted({who(k) for k in switch_procs if k not in fbf})
    if nofbf:
        rows.append(("INFO", "fbfetch summary", "%s logged `vk: ab switches` but no `vk: fbfetch: SUMMARY`: the framebuffer-fetch counters are UNKNOWN there (no pipeline read the framebuffer, or the process ended too early)" % ", ".join(nofbf)))
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
                who(key), kind, allc, bad, ", ".join(parts) or "none", ("; %d transitioned (NVMTL_LOAD_BARRIER)" % bar) if bar else "",
                ("; %d seen with a render pass already open (not counted)" % inrp) if inrp else "")))
        early, ok, worst, blind = d.get("wit_early", 0), d.get("wit_ok", 0), d.get("wit_worst_us", 0), d.get("wit_blind", 0)
        if early:
            rows.append(("WARN", "fence early", "%s: the fence returned before the GPU's last write was visible to the CPU %d time(s) in %d witnessed submits, worst %d us late: a CPU read of "
                         "GPU output right after a fence wait can see stale bytes (the plugin waits for its completion witness; anything that does not is exposed)" % (
                             who(key), early, ok + early, worst)))
        elif key[0] in pool_off:
            rows.append(("INFO", "fence early", "%s: pool reuse is OFF there (NVMTL_NO_POOL_REUSE), so no completion witness was recorded: 'no early fence' is NOT evidence" % who(key)))
        elif ok:
            rows.append(("INFO", "fence early", "%s: %d witnessed submits, no early fence%s" % (who(key), ok, ("; %d submits without a witness" % blind) if blind else "")))
    for key, (n, worst) in sorted(wit_lines.items()):
        if key not in ab:
            rows.append(("WARN", "fence early", "%s: the fence returned before the GPU's last write was visible (%d logged line(s), worst %d us late; no counter line: an older plugin)" % (who(key), n, worst)))
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
                         "checked: the GPU may have read a mix of two states (random strokes in a glyph cell)%s" % (who(key), ce, cv, d.get("checked", 0), " - a snapshot was on: the GPU saw the state of the flush" if snap else "")))
        elif chk and d.get("checked", 0):
            rows.append(("INFO", "managed shadow", "%s: %d managed upload(s) checked, none changed under a single didModifyRange%s" % (who(key), d.get("checked", 0), ("; %d merged ranges changed (legitimate gaps)" % cv) if cv else "")))
        if sync:
            rows.append(("WARN", "managed shadow", "%s: %d of %d managed uploads were a synchronous submit, where NVMTL_MANAGED_CHECK / NVMTL_MANAGED_SNAPSHOT are INERT (the pre-submit prologue is off, full or failed): "
                         "the result says nothing about those" % (who(key), sync, up)))
        bad = []
        if cs: bad.append("%d upload(s) not checked (no shadow, or a range past the buffer)" % cs)
        if d.get("snap_failed", 0): bad.append("%d snapshot allocation(s) failed" % d["snap_failed"])
        if d.get("snap_skipped_big", 0): bad.append("%d range(s) over 1 MiB read the shadow itself" % d["snap_skipped_big"])
        if d.get("snap_skipped_noshadow", 0): bad.append("%d range(s) without a shadow" % d["snap_skipped_noshadow"])
        if snap and up and not d.get("snapped", 0): bad.append("the snapshot is on and NO upload was snapshotted")
        if bad:
            rows.append(("WARN", "managed shadow", "%s: %s" % (who(key), "; ".join(bad))))
        elif snap and d.get("snapped", 0):
            rows.append(("INFO", "managed shadow", "%s: %d upload(s) read a private snapshot" % (who(key), d["snapped"])))
    # N50: the undeclared-write detector, the widened uploads (NVMTL_MANAGED_WHOLE / PAGES), same-image copies and the surface census
    rows.extend(_ud_rows(mg))
    rows.extend(_same_image_rows(ab))
    rows.extend(_census_rows(cen))
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
                   who(key), d.get("seq", 0), d.get("t_ms", 0) / 1000.0, calls, tot_us / 1000.0, fast, d.get("fast_us", 0) / 1000.0, miss, d.get("miss_unsynced", 0), d.get("miss_seed", 0), d.get("miss_gen", 0),
                   d.get("locked_upload", 0), d.get("locked_skip_vram", 0), d.get("locked_skip_pending", 0), d.get("locked_skip_overwrite", 0), d.get("locked_fail", 0), d.get("bufcopy_dedup", 0),
                   br, refresh_us / 1000.0, share, d.get("bufcopy_uploaded", 0), d.get("bufcopy_up_us", 0) / 1000.0, d.get("bufcopy_skipped", 0), d.get("bufcopy_skip_us", 0) / 1000.0,
                   d.get("bufcopy_noshadow", 0), d.get("bufcopy_noshadow_us", 0) / 1000.0, d.get("bufcopy_failed", 0), d.get("bufcopy_forgotten", 0), d.get("bufcopy_skip_refused_layout", 0),
                   d.get("shadow_bytes", 0) / 1024.0, d.get("shadow_refused", 0), d.get("cimg_new", 0), d.get("cimg_recycled", 0), d.get("cimg_parked", 0), d.get("cimg_evicted", 0), refused,
                   d.get("cimg_refused_size", 0), d.get("cimg_refused_form", 0), d.get("cimg_refused_full", 0), "ON" if d.get("cimg_on", 1) else "OFF"))
        vchk, vbad = d.get("verify_checked", 0), d.get("verify_mismatch", 0)
        if vbad:
            rows.append(("FAIL", "surface-in cache", "%s: NVMTL_SURFIN_VERIFY found %d STALE image(s) in %d verified skips: the shadow claimed contents the image did not hold. %s" % (who(key), vbad, vchk, txt)))
        elif not d.get("cache", 1):
            rows.append(("WARN", "surface-in cache", "%s: NVMTL_NO_SURFIN_CACHE is set, this run does not show the default plugin. %s" % (who(key), txt)))
        elif d.get("bufcopy_failed", 0):
            rows.append(("WARN", "surface-in cache", "%s: %d buffer-texture refresh(es) FAILED (the image keeps its old pixels, the next refresh uploads again). %s" % (who(key), d["bufcopy_failed"], txt)))
        elif br and d.get("bufcopy_noshadow", 0) * 2 > br:
            rows.append(("WARN", "surface-in cache", "%s: most buffer-texture refreshes had no shadow (images over 1 MiB or the 32 MiB budget: shadow_refused=%d), so they are uploaded every time. %s" % (
                who(key), d.get("shadow_refused", 0), txt)))
        elif d.get("verify", 0) and not vchk:
            rows.append(("WARN", "surface-in cache", "%s: NVMTL_SURFIN_VERIFY=%d is on and no skip was verified (no refresh was skipped). %s" % (who(key), d["verify"], txt)))
        else:
            rows.append(("INFO", "surface-in cache", txt + ("; VERIFY: %d skip(s) read back, 0 stale" % vchk if vchk else "")))
    for key, d in sorted(sin.items()):
        rs = d.get("rsample_by_layout")
        if isinstance(rs, list) and len(rs) == 6 and sum(rs):
            names = ("UNDEFINED", "GENERAL", "SHADER_READ_ONLY", "TRANSFER_SRC", "TRANSFER_DST", "other")
            lie = sum(rs) - rs[2]
            rows.append(("INFO", "sampled layout", "%s: %d render-encoder sample(s) of a texture, %d (%.0f %%) with the image resting in a layout other than the SHADER_READ_ONLY_OPTIMAL the descriptor declares "
                         "and no transition recorded (%s). The host probe (tests/vk_meta_probe.c LIE=1/2, stock RADV 26.2.5, DCC on and off) shows no wrong pixel for it; the plugin's own Mesa is unchecked: "
                         "a non-zero count here with the artefacts is the lead to follow" % (who(key), sum(rs), lie, 100.0 * lie / sum(rs), ", ".join("%s %d" % (n, c) for n, c in zip(names, rs) if c))))
    for key in sorted(switch_procs - set(sin)):
        lvl = "WARN" if key[0] == "WindowServer" else "INFO"
        rows.append((lvl, "surface-in cache", "%s logged `vk: ab switches` but no `vk: surfin: SUMMARY`%s" % (who(key), " - WindowServer is never quit gracefully and the plugin prints the totals from a 10 s timer: an older plugin, or no surface-in call at all" if lvl == "WARN" else " (an older plugin, or no surface-in call)")))
    for key in sorted(mg_on - set(mg)):
        rows.append(("INFO", "managed shadow", "%s: the shadow check / snapshot was on and no managed upload was seen (no didModifyRange upload in that process)" % who(key)))
    return rows


# the ladder: tools/vm/macos/autoreport.sh ladder_run writes ladder.txt (one block per rung: what it ran with, the looks, the answer) and <rung>.log
# (the plugin log slice of the rung's processes); collect.sh packs them as 41-ladder.txt and 41-ladder-<rung>.log. The rung list is the same as
# autoreport.sh's LADDER_RUNGS / LADDER_WS_RUNGS (tests/test_ladder_step.sh compares them)
LADDER_RUNGS = ("baseline", "radv", "sync", "managed", "alignguard", "paranoid", "loadbarrier")
LADDER_WS_RUNGS = ("radv-ws", "paranoid-ws")
# variant 3 (`rdna3vm step ladder-glyphs`): the glyph hunt (the same list in autoreport.sh's LADDER_GLYPHS_RUNGS; the test compares). The knob of each rung (tests compare these
# with autoreport.sh's table too); `paranoid` is the NEGATIVE control (card run 2026-10-25f: it does not clean the text)
LADDER_GLYPHS_RUNGS = ("baseline", "managed-whole", "managed-check", "no-surface-share", "no-surface-pageoff", "fbfetch-zero", "managed-pages", "managed-eager-zero", "no-texbuf",
                       "no-bindless", "dontcare-clears", "skip-empty-0", "paranoid")
GLYPHS_CONTROL = "paranoid"
# what a rung that came out CLEAN (with the baseline BROKEN) points at
GLYPHS_MEANING = {
    "managed-whole": "the app writes managed-buffer bytes outside its didModifyRange and the GPU needs them (hypothesis A); look at the `undeclared writes` row and run managed-pages",
    "managed-pages": "a page-granular copy is enough: the undeclared bytes sit in the pages the declared ranges touch (hypothesis A, Apple copies whole pages)",
    "no-surface-share": "the window surface shared with WindowServer in family VRAM is where the text breaks (hypothesis C): the private copy path is right",
    "no-surface-pageoff": "the VRAM-newest / page-off protocol is where it breaks (hypothesis C): a plain copy-out is right",
    "fbfetch-zero": "the framebuffer-fetch draw is where the strokes come from (a fetch of zero draws clean glyphs)",
    "managed-eager-zero": "the creation-time clear of managed buffers matters (the old memset + upload is right)",
    "no-texbuf": "the texel-buffer views (the window as a buffer texture) are the cause",
    "no-bindless": "the bindless descriptor path is the cause",
    "dontcare-clears": "a don't-care load action must clear",
    "skip-empty-0": "an encoder with no draws must still be submitted",
    "managed-check": "the detector changed the timing (it hashes and mirrors the shadow at the flush): a race between the CPU and the upload; run managed-whole next",
}


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
        if line.startswith(("ABORTED", "push:")):   # (written wherever the run was: inside a rung or between two)
            if line[:200] not in problems:
                problems.append(line[:200])
            continue
        if cur is None:
            # lines outside a rung that the report must show: a run that was ABORTED by a signal, a warning before the first rung, ppm files that did not reach the host
            if line.startswith(("ABORTED", "WARN:", "push:")) and line[:200] not in problems:
                problems.append(line[:200])
            continue
        m = re.match(r"^-- look (\S+)", line)
        if m:
            cur["_look"] = m.group(1); continue
        m = re.match(r"^exit: (-?\d+)$", line)
        if m:
            if int(m.group(1)) != 0:
                cur["warn"].append("the scanout look %s exited %s (the picture may be missing or partial)" % (cur.get("_look", "?"), m.group(1)))
            continue
        if line.startswith("log: ") and "truncated" in line:
            cur["warn"].append("the plugin log slice was TRUNCATED (" + line[5:120] + ")")
        m = re.match(r"^(scope|conf|looks|answer|pids|log|env|set): ?(.*)$", line)
        if m:
            k, v = m.group(1), m.group(2).strip()
            if k == "scope": cur["scope"] = v
            elif k == "conf": cur["conf"] = [] if v.startswith("(nothing)") else v.split()
            elif k == "looks": cur["looks"] = v.split()
            elif k == "answer": cur["verdict"] = v if v in ("y", "n") else None
            elif k == "pids":
                cur["pids"] = v
                tm = re.search(r"Terminal=(\d+)", v)
                cur["terminal_pid"] = int(tm.group(1)) if tm else None
            elif k == "log": cur["logline"] = v
            continue
        if line.startswith("quit: still running after"):
            cur["quit_failed"] = True   # (runs before the glyph hunt) the graceful quit (System Events) did not end the apps: TERM, the plugin's exit SUMMARY is lost
            continue
        m = re.match(r"^quit: pid (\d+) ended by SIG(INT|TERM|KILL) after (\d+)s", line)
        if m:
            cur.setdefault("quit_ends", []).append((m.group(2), int(m.group(3))))   # how each instance ended (SIGINT, then SIGTERM, then SIGKILL; no System Events)
            continue
        if line.startswith("quit: pid(s) ") and "SURVIVED" in line:
            cur["quit_survived"] = line[:120]
            continue
        if line.startswith("WARN:") or line.startswith("failed:") or line.startswith("NOT TAKEN:"):
            cur["warn"].append(line[:200])
    for name, r in rungs.items():
        r["log"] = logs.get(name, "")
    if not ended:
        problems.append("ladder.txt has no closing '== end' line: the run was cut short or is still going")
    return variant, rungs, problems


_STATE_LINES = {   # the plugin's effective-state lines (one per process start): what a knob really did
    "share": re.compile(r"^item 7: IOSurface textures (live in the surface's family VRAM|private per process)"),
    "pageoff": re.compile(r"^after a GPU write, IOSurface VRAM is (marked newest|copied out eagerly)"),
    "zero": re.compile(r"^zero-fill: managed buffers (clear their VRAM copy|memset their CPU copy)"),
    "texbuf": re.compile(r"^vk: par3 VK_EXT_mutable_descriptor_type \w+.*texture buffers (ON|OFF)"),
    "bindless": re.compile(r"^vk: descriptor indexing .*bindless textures (ON|OFF)"),
    "skip": re.compile(r"^commit: empty submission parts (are NOT submitted|are submitted as before)"),
}
_SKIPPED = re.compile(r"^commit: (\d+) empty submission parts NOT submitted")


def terminal_state(log, pid=None):
    """what the plugin log says about the rung's Terminal (the instance with `pid`, else any process called Terminal): the effective-state lines, the newest SUMMARY of
    managed / vk: ab / fbfetch / census, whether the NVMTL_PARANOID profile was on, how many empty submission parts were skipped, and whether it logged anything at all"""
    st = {"seen": False, "paranoid": False, "skipped": 0}
    for proc, p_, tid, msg in _lines(log):
        if (p_ != pid) if pid else (proc != "Terminal"):
            continue
        st["seen"] = True
        for k, rx in _STATE_LINES.items():
            m = rx.match(msg)
            if m:
                st[k] = m.group(1)
        m = _SKIPPED.match(msg)
        if m:
            st["skipped"] = max(st["skipped"], int(m.group(1)))
        if _PARANOID.match(msg):
            st["paranoid"] = True
        for key, rx in (("mg", _SUM_MG), ("ab", _SUM_AB), ("fbf", _SUM_FBF)):
            m = rx.match(msg)
            if m:
                st[key] = _kv(m.group(1))
        m = _SUM_CEN.match(msg)
        if m:
            st["cen"] = _kv(m.group(1).partition(" top=")[0])
    return st


def rung_engaged(name, st, bst):
    """(True | False | None, why): did the rung's knob change anything in the Terminal? st = terminal_state of the rung, bst = of the baseline. A rung whose key reached the
    plugin can still be a no-op (the feature was already off, the app never used it): comparing the plugin's effective-state lines with the baseline's, or reading a counter
    that only the knob moves, tells. None = the logs cannot tell (the line or counter is missing)."""
    def flip(key, was, now, what):
        a, b = bst.get(key), st.get(key)
        if a is None or b is None:
            return None, "no `%s` line in %s: whether the knob changed anything is unknown" % (what, "the baseline's Terminal log" if a is None else "this rung's Terminal log")
        if a == was and b == now:
            return True, "%s: baseline '%s', this rung '%s'" % (what, a, b)
        if a != was:
            return False, "%s was already '%s' in the baseline: the knob changes nothing" % (what, a)
        return False, "%s is still '%s' in this rung: the knob did not take" % (what, b)
    mg, ab, fbf = st.get("mg") or {}, st.get("ab") or {}, st.get("fbf") or {}
    if name in ("managed-whole", "managed-pages"):
        if not mg:
            return None, "no `managed: SUMMARY` from Terminal"
        flag = "whole" if name == "managed-whole" else "pages"
        if not mg.get(flag):
            return False, "the %s flag is 0 in the managed SUMMARY: the knob did not reach the code" % flag
        if mg.get("widened_uploads", 0) > 0 or mg.get("ud_whole_marks", 0) > 0:
            return True, "%d of %d uploads widened%s" % (mg.get("widened_uploads", 0), mg.get("uploads", 0), (", %d unreported buffers uploaded whole" % mg["ud_whole_marks"]) if mg.get("ud_whole_marks") else "")
        return False, "no upload was widened (%d managed uploads): every declared range already covered what the knob adds, or the app uploads nothing managed" % mg.get("uploads", 0)
    if name == "managed-check":
        if not mg:
            return None, "no `managed: SUMMARY` from Terminal"
        n = mg.get("ud_scans", 0) + mg.get("ud_idle_scans", 0)
        return (True, "%d scans" % n) if n else (False, "the detector scanned nothing (no managed buffer was declared or read through `contents`)")
    if name == "no-surface-share":
        return flip("share", "live in the surface's family VRAM", "private per process", "`item 7: IOSurface textures ...`")
    if name == "no-surface-pageoff":
        return flip("pageoff", "marked newest", "copied out eagerly", "`after a GPU write, IOSurface VRAM is ...`")
    if name == "managed-eager-zero":
        return flip("zero", "clear their VRAM copy", "memset their CPU copy", "`zero-fill: managed buffers ...`")
    if name == "no-texbuf":
        return flip("texbuf", "ON", "OFF", "`vk: par3 ... texture buffers ...`")
    if name == "no-bindless":
        return flip("bindless", "ON", "OFF", "`vk: descriptor indexing ... bindless textures ...`")
    if name == "skip-empty-0":
        if st.get("skip") is None or "skip" not in st:
            return None, "no `commit: empty submission parts ...` line in this rung's Terminal log"
        if st["skip"] != "are submitted as before":
            return False, "the plugin still skips empty parts: the knob did not take"
        if bst.get("skipped", 0) > 0:
            return True, "the baseline skipped %d empty submission part(s), this rung submits them" % bst["skipped"]
        return False, "the baseline's Terminal never had an empty submission part to skip: the knob changes nothing"
    if name == "fbfetch-zero":
        if not fbf:
            return None, "no `vk: fbfetch: SUMMARY` from Terminal"
        return (True, "zr_forced=%d pipeline(s) read zero" % fbf["zr_forced"]) if fbf.get("zr_forced", 0) > 0 else (False, "zr_forced=0: no pipeline of the Terminal reads the framebuffer, the knob changes nothing")
    if name == "dontcare-clears":
        if "dontcare_seen" not in ab:
            return None, "no dontcare_* counter in the Terminal's `vk: ab: SUMMARY` (an older plugin, or no summary)"
        return (True, "%d don't-care attachment(s) cleared" % ab.get("dontcare_cleared", 0)) if ab.get("dontcare_cleared", 0) > 0 else (False, "no render pass attachment had a don't-care load action (dontcare_seen=%d): the knob changes nothing" % ab["dontcare_seen"])
    if name == "paranoid":
        return (True, "the NVMTL_PARANOID profile line is in the log") if st.get("paranoid") else (False, "no `conf: NVMTL_PARANOID=1 - the diagnostic profile is ON` line: the profile was not on")
    return None, ""


def glyph_verdicts(rungs, prefix):
    """{rung: {"verdict", "text"}} of the automatic glyph check (tools/glyph_verdict.py) on the rung's hires look(s): `<prefix>-scanout<look>-pipe<N>.ppm`, prefix
    = the report's path without .tar.gz; the `h2` look of the glyph hunt is the second look that identifies the live window. A rung with no look file is left out; ImportError / a bad file is a `none` with the reason, never an exception."""
    try:
        import glyph_verdict as gv
    except ImportError:
        return {n: {"verdict": "none", "text": "tools/glyph_verdict.py is missing next to triage_glyphs.py: the glyphs were NOT checked"} for n in rungs}
    out = {}
    for name, r in rungs.items():
        files, later = [], []
        looks_ = r.get("looks") or []
        if not any(l.endswith("h") for l in looks_):
            continue   # no hires look was taken for this rung (an older run): nothing to judge, and nothing was promised
        if prefix is None:
            out[name] = {"verdict": "none", "text": "the glyphs were NOT judged: this triage was not given the report as <name>.tar.gz, so the looks that travel next to the archive (...-scanoutladder-<rung>h-pipe*.ppm) cannot be found"}
            continue
        for look in looks_:
            if look.endswith("h"):
                files += sorted(glob.glob(glob.escape(prefix) + "-scanout" + glob.escape(look) + "-pipe*.ppm"))
            elif look.endswith("h2"):
                later += sorted(glob.glob(glob.escape(prefix) + "-scanout" + glob.escape(look) + "-pipe*.ppm"))
        if not files:
            out[name] = {"verdict": "none", "text": "the glyphs were NOT judged: no hires look file next to the report (%s-scanout%s-pipe*.ppm): they are uploaded beside the archive and were not sent, or not kept with it" % (os.path.basename(prefix), [l for l in looks_ if l.endswith("h")][0])}
            continue
        try:
            # a rung with a second look (the glyph hunt) is judged on the window that changed between the two, its own live text
            out[name] = gv.verdict_files(files, later=later) if any(l.endswith("h2") for l in looks_) else gv.verdict_files(files)
        except Exception as e:  # noqa: BLE001  (one bad picture must not take the triage down)
            out[name] = {"verdict": "none", "text": "the glyph check FAILED (%s: %s)" % (type(e).__name__, e)}
    return out


def ladder_rows(rungs, variant=1, problems=(), glyphs=None):
    """rungs: {rung name: {"conf": [lines], "scope": "apps"|"windowserver", "log": plugin log slice, "looks": [file names], "verdict": "y"|"n"|None}}.
    One row per rung: what it set, which processes show it in the log (the plugin's own `vk: ab switches` line), the load pass / fence / managed counters
    of the slice, and the owner's verdict when one was given. [(level, name, text)]."""
    rows = [("WARN", "ladder", p) for p in problems]
    names = LADDER_GLYPHS_RUNGS if variant == 3 else LADDER_RUNGS + (LADDER_WS_RUNGS if variant == 2 else ())
    glyphs = glyphs or {}
    engaged = {}   # rung -> (True | False | None, why) for the glyph hunt (variant 3)
    bst = terminal_state((rungs.get("baseline") or {}).get("log", ""), (rungs.get("baseline") or {}).get("terminal_pid")) if variant == 3 else {}
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
        # EVERY rung, the baseline too: the Terminal this rung started must have logged in the slice (a slice of other processes' lines proves nothing about the Terminal)
        tpid = r.get("terminal_pid")
        if "pids" in r:
            if tpid is None:
                problems_.append("Terminal did not start in this rung (pids: %s): the rung proves nothing" % r["pids"].strip()[:60])
            elif not any(k[1] == tpid for k in procs):
                problems_.append("Terminal pid %d logged nothing (no `vk: ab switches` line) in the slice, the rung proves nothing" % tpid)
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
        for want_ in ("undeclared writes", "managed upload", "same-image copy", "surface census", "fence early"):   # the rung's own numbers for the Terminal: the rows a baseline comparison needs
            for lvl_, nm_, t_ in sub:
                if nm_ == want_ and t_.startswith("Terminal"):
                    txt += "; %s: %s" % (nm_, t_[:260]); break
        extra = problems_ + list(r.get("warn") or [])
        tst = terminal_state(r.get("log", ""), tpid) if variant == 3 or (tpid and "pids" in r) else {}
        if "pids" in r and tpid and tst.get("seen") and "cen" not in tst:
            extra.append("no `census: SUMMARY` from the Terminal: its surface census is unknown (an older plugin, or no IOSurface target was drawn into)")
        if variant == 3 and name != "baseline":
            eng = rung_engaged(name, tst, bst) if tst.get("seen") and bst.get("seen") else (None, "the Terminal's log is missing in this rung or in the baseline")
            engaged[name] = eng
            if eng[0] is True:
                txt += "; knob engaged (%s)" % eng[1]
            elif eng[0] is False:
                extra.append("KNOB NOT ENGAGED: %s - this rung's picture says nothing about the knob" % eng[1])
            else:
                extra.append("engagement could not be shown: %s" % eng[1])
        if warn:
            extra.append("a counter or conf WARN in the slice")
        if r.get("cut"):
            extra.append("the rung has no end line (cut off)")
        if not (r.get("log") or "").strip():
            extra.append("no plugin log slice for this rung")
        if name in glyphs and glyphs[name]["verdict"] == "none" and "NOT judged" in glyphs[name]["text"]:
            extra.append(glyphs[name]["text"])
        elif any(l.endswith("h") for l in looks) and name not in glyphs:
            extra.append("the glyphs were not judged (no verdict was given to this rung although it has a hires look)")
        if extra:
            txt += "; " + "; ".join(extra)[:700]
        rows.append(("INFO" if not extra else "WARN", "ladder " + name, txt))
    nq = [n for n in names if (rungs.get(n) or {}).get("quit_failed")]
    if nq:
        rows.append(("INFO", "ladder quit", "%d of %d rung(s) (an older run): the apps did not quit when asked through System Events and were TERMed after 8 s (macOS may be waiting for the one-time Automation "
                     "prompt for osascript, or the Terminal asks to terminate its running process); the plugin's exit SUMMARY is lost there, its 10 s counters are not" % (len(nq), len(names))))
    ends = [e for n in names for e in (rungs.get(n) or {}).get("quit_ends", [])]
    surv = [n for n in names if (rungs.get(n) or {}).get("quit_survived")]
    if ends or surv:
        by = {}
        for sig, secs in ends:
            by.setdefault(sig, []).append(secs)
        txt = "; ".join("%d instance(s) ended by SIG%s (after %s s)" % (len(v), k, "/".join(str(x) for x in sorted(set(v)))) for k, v in sorted(by.items(), key=lambda kv: ("INT", "TERM", "KILL").index(kv[0])))
        rows.append(("WARN" if surv else "INFO", "ladder quit", "%s%s; a signal ends a GUI process without its exit handlers, so the plugin's exit SUMMARY is lost: the counters are the last 10 s tick of each process" % (
            txt, ("; STILL ALIVE after SIGKILL in: " + ", ".join(surv)) if surv else "")))
    # the automatic glyph verdict: which rung made the text clean
    gv_ = {n: glyphs[n]["verdict"] for n in names if n in glyphs}
    if gv_ and all(v == "none" for v in gv_.values()):
        rows.append(("WARN", "ladder glyphs", "the automatic glyph check found no tick lines in any rung's look (the text.command window was not on a screen, or the looks are missing): %s" % next(iter(glyphs.values()))["text"]))
    elif gv_:
        clean = [n for n in names if gv_.get(n) == "clean"]
        if variant == 3:   # a rung whose knob was not engaged is the baseline again: its picture is no evidence about the knob
            clean = [n for n in clean if n == "baseline" or (engaged.get(n) or (None,))[0] is True]
        if gv_.get("baseline") == "broken" and clean:
            rows.append(("INFO", "ladder glyphs", "the baseline glyphs were BROKEN and CLEAN with: %s (tools/glyph_verdict.py: identical tick lines are drawn identically)" % ", ".join(clean)))
        elif gv_.get("baseline") == "broken":
            tail = ""
            if variant == 3:
                eng_ok = [n for n in names if n not in ("baseline", GLYPHS_CONTROL) and (engaged.get(n) or (None,))[0] is True]
                eng_no = [n for n in names if n not in ("baseline", GLYPHS_CONTROL) and n not in eng_ok]
                tail = ("; no ENGAGED knob of this glyph hunt is the cure (engaged: %s)" % (", ".join(eng_ok) or "none"))
                if eng_no:
                    tail += ("; %d rung(s) did NOT engage their knob or could not show it (%s): they exclude nothing" % (len(eng_no), ", ".join(eng_no)))
                tail += ("; read the `undeclared writes`, `surface census`, `same-image copy` and `fence early` rows of the baseline rung for what is left (docs/AFTER-TESTS.md)")
            rows.append(("INFO", "ladder glyphs", "the baseline glyphs were BROKEN and no rung made them clean%s" % tail))
        elif gv_.get("baseline") == "clean":
            rows.append(("INFO", "ladder glyphs", "the baseline glyphs were CLEAN in this run: the artefact did not show, the other rungs prove nothing"))
        else:
            rows.append(("INFO", "ladder glyphs", "the baseline's glyphs could not be judged (%s)" % (gv_.get("baseline") or "no look")))
    if variant == 3 and gv_:
        if gv_.get(GLYPHS_CONTROL) == "clean":
            rows.append(("WARN", "ladder glyphs control", "the negative control (%s) came out CLEAN here, though card run 2026-10-25f says it does not clean Terminal's text: either the live-window "
                         "identification picked another window, or the artefact is intermittent - the other rungs of this run are not comparable" % GLYPHS_CONTROL))
        if gv_.get("baseline") == "broken":
            fixers = [n for n in names if gv_.get(n) == "clean" and n != GLYPHS_CONTROL and n in GLYPHS_MEANING]
            for n in fixers:
                if (engaged.get(n) or (None,))[0] is True:
                    rows.append(("INFO", "ladder glyphs " + n, "CLEAN while the baseline was BROKEN: " + GLYPHS_MEANING[n]))
                else:
                    rows.append(("WARN", "ladder glyphs " + n, "CLEAN while the baseline was BROKEN, but the knob was NOT shown to be engaged (%s): the text is intermittent, not cured by this knob" % (engaged.get(n) or (None, "unknown"))[1]))
        unjudged = [n for n in names if n in glyphs and gv_.get(n) == "none"]
        if unjudged and len(unjudged) < len(gv_):
            rows.append(("INFO", "ladder glyphs none", "no live tick window was found in the look of: %s (the Terminal did not come up on a screen in that rung, or the screen did not change between the two looks)" % ", ".join(unjudged)))
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
