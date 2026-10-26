#!/usr/bin/env python3
"""The app survey (`rdna3vm step apps`, tools/vm/macos/autoreport.sh APPS block, boot-arg rdna3ladder=5): a row per app.

Reads 41-ladder.txt (variant 5: one `== rung <app>` block per app: what was opened, the pids, whether it was still alive, the plugin and WindowServer state,
the crash reports, the looks) and the app's log slices: 41-ladder-<app>.log (the plugin's NVMTL lines of its processes), 41-ladder-<app>-sys.log (the app's
own Metal / GPU / error lines, WindowServer's and the kernel's RDNA3 lines of the same minutes) and 41-ladder-<app>-crash.log (the summary of a crash report).

apps_rows(ladder_txt, rungs, problems, glyphs, logs) -> [(level, name, text)]; `rungs` / `problems` come from triage_glyphs.parse_ladder (the looks, the
hi-res looks' names), `glyphs` is the automatic glyph verdict of the Terminal rung (the only app whose text is known: the tick lines), `logs` is
{member name without 41-ladder- and .log: text}. One row `app <name>`: `ran` / `crashed` / `exited by itself` / `did not open` / `was already running`,
the stages, the plugin, the WindowServer pid check, the plugin's draw counters, and the lines of the log that name a Metal / NVMTL error, a GPU restart or
a VM fault. Whether an app DREW is the screenshots' verdict (`<report>-scanoutapps-<app>[-<stage>]h-pipe<N>`): the rows say what the logs cannot hide.
"""
import re

# the same list as tools/vm/macos/autoreport.sh APPS_RUNGS (tests/test_apps_step.sh compares them)
APPS_RUNGS = ("chess", "terminal", "textedit", "calculator", "notes", "safari", "preview", "maps", "settings", "appstore", "photos", "imagecapture",
              "quicktime", "mission-control", "launchpad")
APPS_TITLE = {"chess": "Chess", "terminal": "Terminal", "textedit": "TextEdit", "calculator": "Calculator", "notes": "Notes", "safari": "Safari",
              "preview": "Preview", "maps": "Maps", "settings": "System Settings", "appstore": "App Store", "photos": "Photos",
              "imagecapture": "Image Capture", "quicktime": "QuickTime Player", "mission-control": "Mission Control", "launchpad": "Launchpad / Apps"}

# what in a log slice is worth a row (case-insensitive): the Metal plugin's own errors, a GPU restart / hang, a VM fault
_NOTABLE = (
    ("Metal/NVMTL error", re.compile(r"(NVMTL|NullMoth|MTL\w*|Metal\w*)\b.*\b(error|errors|fail|failed|failure|unsupported|not supported|unimplemented|abort|assert|timeout|invalid)\b", re.I)),
    ("GPU restart/hang", re.compile(r"\b(gpu|ring|accelerator|rdna3\w*)\b.*\b(restart\w*|reset|hang|hung|timed out|timeout|stall\w*)\b", re.I)),
    ("VM fault", re.compile(r"(vm[ _]fault|page fault|gcvm|vm_l2|vmc\b|protection fault)", re.I)),
)
# generic error words of the app's own lines: counted, never a WARN on their own (a normal app logs "error" in many harmless lines)
_GENERIC = re.compile(r"\b(error|fault|crash|assert)", re.I)
_CENSUS = re.compile(r"census: SUMMARY (.*)$")
_KV = re.compile(r"(\w+)=(\d+)")


def _blocks(text):
    """{rung: [lines]} of a ladder.txt: the lines of each `== rung X` ... `== end rung X` block, in order"""
    out, cur = {}, None
    for raw in (text or "").splitlines():
        line = raw.rstrip()
        m = re.match(r"^== rung (\S+?)(?::.*)?$", line)
        if m:
            cur = out.setdefault(m.group(1), [])
            continue
        if re.match(r"^== end rung ", line) or line == "== end":
            cur = None
            continue
        if cur is not None:
            cur.append(line)
    return out


def _field(lines, key):
    """the value of the last `key: value` line, or None"""
    val = None
    for l in lines:
        if l.startswith(key + ": "):
            val = l[len(key) + 2:].strip()
    return val


def scan_notables(*texts):
    """{category: (count, first matching line)} over the log slices"""
    found = {}
    for text in texts:
        for l in (text or "").splitlines():
            for cat, rx in _NOTABLE:
                if rx.search(l):
                    n, first = found.get(cat, (0, None))
                    found[cat] = (n + 1, first or l.strip())
                    break
    return found


def generic_errors(text):
    """(count, first line) of the lines of a slice that carry a generic error word and are not already notable"""
    n, first = 0, None
    for l in (text or "").splitlines():
        if _GENERIC.search(l) and not any(rx.search(l) for _c, rx in _NOTABLE):
            n += 1
            first = first or l.strip()
    return n, first


def draw_counters(log):
    """(draws, presents, pids) of the newest `census: SUMMARY` of each process of the plugin log (DRIVER-PLAN N50's per-process census), summed"""
    last = {}
    for l in (log or "").splitlines():
        m = re.search(r"\[(\d+):[0-9a-f]+\]", l)
        c = _CENSUS.search(l)
        if m and c:
            last[m.group(1)] = {k: int(v) for k, v in _KV.findall(c.group(1))}
    if not last:
        return None
    return sum(v.get("draws", 0) for v in last.values()), sum(v.get("present", 0) for v in last.values()), sorted(last)


def _short(s, n=150):
    s = re.sub(r"^\S+ \S+ +\S+ +", "", s)   # the date, the time and the log level of a compact line
    return s if len(s) <= n else s[:n - 3] + "..."


def app_row(name, lines, rung, glyph, logs):
    """one app's (level, text)"""
    result = _field(lines, "result") or "(no result line: the rung was cut off)"
    plugin = _field(lines, "plugin") or ""
    stages = _field(lines, "stages") or "?"
    pids = _field(lines, "pids") or ""
    alive = _field(lines, "alive") or ""
    ws_b, ws_a = _field(lines, "ws_before"), _field(lines, "ws_after")
    notes, level = [], "INFO"
    bad = result.startswith(("crashed", "exited")) or "WindowServer restarted" in result or "WindowServer / Dock crashed" in result or "cut off" in result
    if bad:
        level = "FAIL"
    elif result.startswith(("did not open", "was already")):
        level = "WARN"
    text = result
    if stages != "main":
        text += "; stages " + stages
    if pids:
        text += "; pids " + pids
    if name != "mission-control" and plugin:
        if plugin.startswith("not checked"):
            pass
        elif plugin.startswith("NOT loaded"):
            notes.append("the Metal plugin is NOT loaded in any process of the app (lsof; a sandboxed helper may hide it: DRIVER-PLAN N1)")
            level = "WARN" if level == "INFO" else level
        else:
            text += "; plugin loaded in " + plugin
    if ws_b is not None and ws_a is not None and ws_b != ws_a:
        notes.append("WindowServer pid %s -> %s" % (ws_b or "none", ws_a or "none"))
    crashes = [l[len("crash: "):] for l in lines if l.startswith("crash: ") and l != "crash: none"]
    if crashes:
        text += "; crash report(s): " + ", ".join(crashes)[:300]
    died = [l[len("died: "):] for l in lines if l.startswith("died: ")]
    if died:
        text += "; " + "; ".join(died)[:200]
    # the plugin's counters of the app's processes, and what the logs complain about
    plog, syslog, crashlog = logs.get(name, ""), logs.get(name + "-sys", ""), logs.get(name + "-crash", "")
    dc = draw_counters(plog)
    if dc:
        text += "; plugin census: %d draws, %d presents (%d process(es))" % (dc[0], dc[1], len(dc[2]))
    elif name != "mission-control" and not result.startswith(("did not open", "was already")):
        nl = len([l for l in plog.splitlines() if l.strip()])
        notes.append("no `census: SUMMARY` from the plugin in the app's processes (%d NVMTL line(s)): it drew nothing through the plugin, or ended before the 10 s tick" % nl)
        level = "WARN" if level == "INFO" else level
    notable = scan_notables(plog, syslog)
    for cat, (n, first) in sorted(notable.items()):
        notes.append("%s: %d line(s), first `%s`" % (cat, n, _short(first)))
        level = "WARN" if level == "INFO" else level
    gn, gfirst = generic_errors(syslog)
    if gn:
        text += "; %d other error/fault line(s) in the slice (first `%s`)" % (gn, _short(gfirst, 110))
    if crashlog.strip():
        reason = next((l for l in crashlog.splitlines() if l.startswith(("exception:", "termination:"))), "")
        text += "; crash summary: " + (reason[:200] or "see 41-ladder-%s-crash.log" % name)
    if glyph:
        text += "; glyphs (automatic, tools/glyph_verdict.py): " + glyph["text"]
        if glyph["verdict"] == "broken":
            level = "WARN" if level == "INFO" else level
    for l in lines:
        if l.startswith(("WARN:", "failed:", "NOT TAKEN:")):
            notes.append(l[:200])
            level = "WARN" if level == "INFO" else level
    if rung and rung.get("quit_survived"):
        notes.append(rung["quit_survived"])
        level = "WARN" if level == "INFO" else level
    looks = (rung or {}).get("looks") or []
    text += "; %d look(s)%s" % (len(looks), (": " + " ".join(looks)) if looks else " (NONE: the screenshots are missing)")
    if notes:
        text += "; " + "; ".join(notes)[:900]
    return level, text


def apps_rows(ladder_txt, rungs, problems=(), glyphs=None, logs=None):
    """[(level, name, text)] of the app survey: a row per app in APPS_RUNGS order, the problems of the run, one summary row"""
    rows = [("WARN", "apps", p) for p in problems]
    blocks = _blocks(ladder_txt)
    glyphs = glyphs or {}
    logs = logs or {}
    count = {"ran": 0, "crashed": 0, "exited": 0, "did not open": 0, "other": 0, "missing": 0, "ws": 0}
    for name in APPS_RUNGS:
        title = APPS_TITLE.get(name, name)
        lines = blocks.get(name)
        if lines is None:
            rows.append(("WARN", "app " + name, "%s: the app did not run or left nothing in the report (the survey was cut short, or an older autoreport.sh)" % title))
            count["missing"] += 1
            continue
        lvl, text = app_row(name, lines, rungs.get(name), glyphs.get(name), logs)
        res = _field(lines, "result") or ""
        key = next((k for k in ("ran", "crashed", "exited", "did not open") if res.startswith(k)), "other")
        count[key] += 1
        count["ws"] += 1 if "WindowServer restarted" in res or "WindowServer / Dock crashed" in res else 0
        rows.append((lvl, "app " + name, "%s: %s" % (title, text)))
    for l in (ladder_txt or "").splitlines():   # lines between the rungs that say why the survey stopped
        if l.startswith(("ABORT:", "login: NOBODY")) and ("WARN", "apps", l[:200]) not in rows:
            rows.append(("WARN", "apps", l[:200]))
    rows.append(("INFO", "apps survey", "%d apps: %d ran, %d crashed, %d exited by themselves, %d did not open, %d other, %d missing; WindowServer restarted or crashed under %d; the rows say what the logs show, "
                 "whether an app DREW is for the screenshots (<report>-scanoutapps-<app>[-<stage>]h-pipe<N>.png in the result file)" % (
                     len(APPS_RUNGS), count["ran"], count["crashed"], count["exited"], count["did not open"], count["other"], count["missing"], count["ws"])))
    return rows
