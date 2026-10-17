#!/usr/bin/env python3
"""
triage_bisect.py - the per-step table of the display bisect (card run
2026-10-17d): `rdna3vm step bisect` / `bisect-plugin` / `bisect-agdc`,
tools/vm/macos/autoreport.sh bisect_run, docs/TESTING-VM.md "Display bisect".

    python3 tools/triage_bisect.py bisect.txt [report-prefix]

The run turns every new feature that has a runtime switch off, then on again
one at a time (oldest first), 30 s each, while the owner looks at both
monitors. bisect.txt (the archive's 39-bisect.txt, or <report>-bisect.txt
uploaded after every step) has one block per step: what was switched (and
read back), the owner's answer (y / n / Enter with a tty, "-" on the timer),
two scanout looks (bisect<N>a about 8 s in, bisect<N> about 20 s in; the
<report>-scanoutbisect<N>[a]-pipe<P>.ppm pictures) and the counters.

analyse() turns that into a table (step, what changed, the owner's answer,
pipe 0/1 % not black, whether the picture moved between the step's looks,
the swap / stale-hold counter deltas) and findings. Missing data is a WARN,
never silence. It names the first step the owner marked n, the first whose
looks went black, the first whose looks stood still, and what the run says
about the bug (see verdict()).

`bisect-agdc` (variant 4, card run 2026-10-17f) is the same bisect on the agdc
profile (WindowServer on our Metal plugin). Its steps are not cumulative (each
sets its whole combination), so there is no "breaks at step N" verdict: the
table names the steps that LOOKED RIGHT (the owner's answers; the looks'
black % and movement) and the ones that looked broken, by swap_mode, and a
second table has the per-step swap counters (swaps, copies, holds, iop flips,
buffers rewritten in place, torn) and the FRONT SURFACE sampler's lines
("*** MOVED -- A LIVE WRITER IS PAINTING THE SCANNED-OUT SURFACE ***") cut out
of the kernel log per step (analyse(text, prefix, logs): logs = [(name, text)],
the archive's 20-log-rdna3.txt and the serial log; cut by the one-event
"trace: events W..W (asked by sysctl ...)" line every step writes into the
kernel log at its start, else by the log's own clock).

Python 3, standard library only; imported by tools/triage_reports.py.
"""
import os
import re
import sys

# a lit pipe with at most this much of its picture not black is a black screen
# (the same line as triage_reports.BLACK_PCT)
BLACK_PCT = 3
# a pipe whose two looks of one step differ in less than this fraction of the
# pixels stood still (a clock tick in the menu bar is ~0.02 %, Activity
# Monitor's graphs on the main display far more)
MOVED_FRACTION = 0.003
# the values the switches have without any boot-arg (accel/rdna3-accel.cpp; swap_mode 1, the copy, again since the
# write generations of card run 2026-10-17d - it was 0 from 2026-10-17d to 2026-10-17g)
DEFAULTS = {"a3": "1", "flicker": "1", "swap_latch": "1", "swap_torn": "1", "swap_mode": "1",
            "trace_scan": "0", "swap_stale": "1", "swap_landed": "0", "a3ws": "0"}
MAIN_KINDS = ("ref", "off", "on", "final", "var")
# plan 4 (bisect-agdc) also switches these (the agdc profile's own defaults: iop on, iop_flip off; the write
# generations on, the flip-buffer present off: card run 2026-10-17d)
DEFAULTS_AGDC = dict(DEFAULTS, iop="1", iop_flip="0", flipbuf_present="0", swap_gen="1")
# the agdc table's switch column, in this order
AGDC_SWITCHES = ("swap_mode", "swap_stale", "iop", "a3", "swap_latch", "iop_flip", "flipbuf_present", "swap_gen")
# the kext's FRONT SURFACE sampler stops after this many samples per boot (accel/rdna3-accel.cpp rd3AccelSampleFront)
FRONT_SAMPLE_CAP = 40
# a step's marker line in the kernel log names the event count W a few events after trace_events_start
MARK_TOL = 300

_KEY = re.compile(r"^([a-z_0-9]+): ?(.*)$")
_PIPE = re.compile(r"^pipe (\d+): (?:power-gated )?(\w+)")
_PIC = re.compile(r"^  picture (\d+)x(\d+): (\d+)% not black, mean rgb \d+ \d+ \d+, (\d+) bytes changed")
_CNT = re.compile(r"^(debug\.\S+): (-?\d+)\s*$")
_MARK = re.compile(r"trace: events (\d+)\.\.(\d+) \(asked by sysctl")
_STAMP = re.compile(r"^\d{4}-\d{2}-\d{2} (\d{2}):(\d{2}):(\d{2})")


def parse(text):
    """{"head": {key: value}, "plan": [(n, mmss, kind, name, changed)], "aborts": [..], "ended": bool,
    "steps": [step]}; a step is {"n", "kind", "name", "changed", "set", "readback" {oid: value},
    "failed", "answer", "at", "offset", "looks" {name: {"pipes" {p: {"state", "pct", "bytes", "px"}},
    "exit", "not_taken", "lit"}}, "counters" {oid: int}, "ended": bool, ...}"""
    out = {"head": {}, "plan": [], "aborts": [], "ended": False, "steps": [], "warns": []}
    cur = None
    sect = None
    for raw in text.splitlines():
        line = raw.rstrip()
        if not line:
            continue
        m = re.match(r"^== step (\d+)\s*$", line)
        if m:
            cur = {"n": int(m.group(1)), "looks": {}, "counters": {}, "readback": {}, "ended": False}
            out["steps"].append(cur)
            sect = None
            continue
        if line.startswith("== end step"):
            if cur:
                cur["ended"] = True
            cur, sect = None, None
            continue
        if line == "== end":
            out["ended"] = True
            cur, sect = None, None
            continue
        if line.startswith("ABORT:"):
            out["aborts"].append(line[6:].strip())
            continue
        if line.startswith("WARN:"):
            out["warns"].append(line[5:].strip())
            continue
        if cur is None:
            m = _KEY.match(line)
            if m and m.group(1) == "plan":
                f = m.group(2).split("|")
                if len(f) >= 5 and f[0].strip().isdigit():
                    out["plan"].append((int(f[0]), f[1], f[2], f[3], "|".join(f[4:])))
            elif m:
                out["head"][m.group(1)] = m.group(2)
            continue
        m = re.match(r"^-- look (\S+)", line)
        if m:
            sect = ("look", m.group(1))
            cur["looks"][m.group(1)] = {"pipes": {}, "exit": None, "not_taken": None, "lit": None}
            continue
        if line.startswith("-- counters"):
            sect = ("counters",)
            continue
        if line.startswith("-- before"):
            sect = ("before",)
            cur.setdefault("before", {})
            continue
        if sect and sect[0] == "look":
            lk = cur["looks"][sect[1]]
            m = _KEY.match(line)
            if m and m.group(1) == "exit":
                lk["exit"] = m.group(2)
                continue
            if line.startswith("NOT TAKEN"):
                lk["not_taken"] = line
                continue
            m = _PIPE.match(line)
            if m:
                lk["cur"] = m.group(1)
                lk["pipes"][m.group(1)] = {"state": m.group(2), "pct": None, "bytes": None, "px": None}
                continue
            if re.match(r"^pipe (\d+): not read", line):
                lk["pipes"][re.match(r"^pipe (\d+)", line).group(1)] = {"state": "asleep", "pct": None,
                                                                         "bytes": None, "px": None}
                continue
            m = _PIC.match(line)
            if m and lk.get("cur") in lk["pipes"]:
                p = lk["pipes"][lk["cur"]]
                p["pct"], p["bytes"], p["px"] = int(m.group(3)), int(m.group(4)), int(m.group(1)) * int(m.group(2))
                continue
            m = re.match(r"^(\d+) pipe\(s\) lit", line)
            if m:
                lk["lit"] = int(m.group(1))
            continue
        if sect and sect[0] in ("counters", "before"):
            m = _CNT.match(line)
            if m:
                cur["counters" if sect[0] == "counters" else "before"][m.group(1)] = int(m.group(2))
                continue
        m = _KEY.match(line)
        if m:
            sect = None
            k, v = m.group(1), m.group(2)
            if k == "readback":
                for oid, val in re.findall(r"(\S+?)=(\S+)", v):
                    cur["readback"][oid] = val
                cur["readback_text"] = v
            else:
                cur[k] = v
    return out


def read_ppm(path):
    """(w, h, rgb bytes) of a binary PPM, or None"""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return None
    m = re.match(rb"P6\s+(?:#[^\n]*\s+)*(\d+)\s+(\d+)\s+(\d+)\s", data)
    if not m:
        return None
    w, h = int(m.group(1)), int(m.group(2))
    px = data[m.end():m.end() + w * h * 3]
    return (w, h, px) if len(px) == w * h * 3 else None


def ppm_not_black_pct(img):
    w, h, px = img
    n = sum(1 for i in range(0, w * h * 3, 3) if (px[i] | px[i + 1] | px[i + 2]) > 8)
    return n * 100 // (w * h)


def ppm_changed_fraction(a, b):
    """the fraction of pixels that differ between two pictures, None when their sizes differ"""
    if a[0] != b[0] or a[1] != b[1]:
        return None
    w, h, pa = a
    pb = b[2]
    row, n = w * 3, 0
    for y in range(h):
        ra, rb = pa[y * row:(y + 1) * row], pb[y * row:(y + 1) * row]
        if ra != rb:
            n += sum(1 for x in range(0, row, 3) if ra[x:x + 3] != rb[x:x + 3])
    return n / float(w * h)


def _delta(cur, prev, pattern):
    """the sum over the counters matching pattern in cur minus prev; None when either step has none of them"""
    rx = re.compile(pattern)
    a = [v for k, v in cur.items() if rx.match(k)]
    b = [v for k, v in prev.items() if rx.match(k)] if prev is not None else []
    if not a or not b:
        return None
    return sum(a) - sum(b)


def _fmt(v):
    return "?" if v is None else str(v)


def step_metrics(step, prev, prefix):
    """{"pct": {pipe: % or None}, "moved": {pipe: True/False/None}, "lit": [pipes], "problems": [text], "deltas": {...}}
    of one step: the pictures' numbers from the look's text (the PPMs when the text has none), movement between
    the step's two looks (the PPMs, else the looks' own 'bytes changed in 2 s')"""
    n = step["n"]
    main, early = step["looks"].get("bisect%d" % n), step["looks"].get("bisect%da" % n)
    met = {"pct": {}, "moved": {}, "lit": [], "problems": [], "deltas": {}, "state": {}}
    if not main or main.get("not_taken"):
        met["problems"].append("no look (%s)" % ((main or {}).get("not_taken") or "bisect.txt has none for it"))
        return met
    if main.get("exit") not in (None, "0"):
        met["problems"].append("the look ended with exit %s" % main["exit"])
    for p, info in sorted(main["pipes"].items()):
        met["state"][p] = info["state"]
        if info["state"] != "LIT":
            continue
        met["lit"].append(p)
        ppm = read_ppm("%s-scanoutbisect%d-pipe%s.ppm" % (prefix, n, p)) if prefix else None
        ppm_a = read_ppm("%s-scanoutbisect%da-pipe%s.ppm" % (prefix, n, p)) if prefix else None
        pct = info["pct"]
        if pct is None and ppm:
            pct = ppm_not_black_pct(ppm)
        met["pct"][p] = pct
        if pct is None:
            met["problems"].append("pipe %s lit, its picture not readable (tiled, DCC or outside the BAR) and no PPM" % p)
        fracs = []
        if ppm and ppm_a:
            f = ppm_changed_fraction(ppm_a, ppm)
            if f is not None:
                fracs.append(f)
        for lk in (main, early):
            q = (lk or {}).get("pipes", {}).get(p)
            if q and q.get("bytes") is not None and q.get("px"):
                fracs.append(q["bytes"] / float(q["px"] * 3))
        if fracs:
            met["moved"][p] = max(fracs) >= MOVED_FRACTION
        else:
            met["moved"][p] = None
            met["problems"].append("pipe %s: nothing says whether its picture moved (no PPMs, no 'bytes changed')" % p)
    if not early or early.get("not_taken"):
        met["problems"].append("the early look (bisect%da) is missing: movement judged from the main look only" % n)
    if not met["lit"] and not any(i["state"] == "asleep" for i in main["pipes"].values()):
        met["problems"].append("no pipe lit in the look (the card showed nothing?)")
    # the counters at the start of this step when the run took them (autoreport's "-- before"), else the step before's
    c, pc = step["counters"], (step.get("before") or (prev["counters"] if prev else None))
    if not c:
        met["problems"].append("no counters")
    met["deltas"] = {
        "copy": _delta(c, pc, r"^debug\.rdna3accel_swaps_copy_h\d$"),
        "inplace": _delta(c, pc, r"^debug\.rdna3accel_swaps_inplace_h\d$"),
        "torn": _delta(c, pc, r"^debug\.rdna3accel_swaps_torn_h\d$"),
        "flips": _delta(c, pc, r"^debug\.rdna3accel_iop_flips$"),
        "iopok": _delta(c, pc, r"^debug\.rdna3accel_iop_ok$"),
        "sw0": _delta(c, pc, r"^debug\.rdna3accel_swaps_h0$"),
        "sw1": _delta(c, pc, r"^debug\.rdna3accel_swaps_h1$"),
        "hold": _delta(c, pc, r"^debug\.rdna3accel_swaps_stalehold_h\d$"),
        "would": _delta(c, pc, r"^debug\.rdna3accel_swaps_stalehold_would_h\d$"),
        "empty": _delta(c, pc, r"^debug\.rdna3accel_swaps_copyempty_h\d$"),
        # card run 2026-10-17d: the write generations' holds, the family's flip-buffer calls and their presents
        "genhold": _delta(c, pc, r"^debug\.rdna3accel_swaps_genhold_h\d$"),
        "flipbuf": _delta(c, pc, r"^debug\.rdna3accel_flipbuf(?:_scanout)?_h\d$"),
        "fbpres": _delta(c, pc, r"^debug\.rdna3accel_flipbuf_presented_h\d$"),
    }
    return met


def judge(steps, mets):
    """{step n: {"bad": bool, "why": [..], "known": bool}}: bad = the owner said n, a lit pipe is black, or a pipe
    that moved in some other step stood still in this one"""
    can_move = set()
    for n, m in mets.items():
        for p, mv in m["moved"].items():
            if mv:
                can_move.add(p)
    res = {}
    for s in steps:
        n = s["n"]
        m = mets[n]
        why = []
        known = False
        ans = s.get("answer", "-")
        if ans in ("y", "n"):
            known = True
        if ans == "n":
            why.append("the owner marked it n")
        for p, pct in sorted(m["pct"].items()):
            if pct is not None:
                known = True
                if pct <= BLACK_PCT:
                    why.append("pipe %s black (%d%% not black)" % (p, pct))
        for p, mv in sorted(m["moved"].items()):
            if mv is False and p in can_move and (m["pct"].get(p) is None or m["pct"][p] > BLACK_PCT):
                why.append("pipe %s stood still between the step's looks (it moved in other steps)" % p)
        res[n] = {"bad": bool(why), "why": why, "known": known}
    return res


def label(s):
    """what a step is, in a sentence: the reference by its name, the others by what they switched"""
    return s.get("name", "?") if s.get("kind") == "ref" else "%s [%s]" % (s.get("changed", "?"), s.get("name", "?"))


def _short(t, n):
    return t if len(t) <= n else t[:n - 1] + "~"


def _int(v):
    try:
        return int(str(v).strip())
    except (TypeError, ValueError):
        return None


def _secs(hms):
    m = re.match(r"^(\d+):(\d+):(\d+)$", (hms or "").strip())
    return int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) if m else None


def cut_log(lines, steps, hold):
    """({step n: (first line, end line)}, method): where each step's lines are in a kernel log. 'markers': the
    one-event 'trace: events W..W (asked by sysctl' line each step writes at its start, W a few events after the
    step's trace_events_start, in step order (the serial log has no clock); else 'clock': the lines' own
    'YYYY-MM-DD HH:MM:SS' (log show) against the steps' 'at:' times. ({}, None) when neither places a step."""
    marks = []
    allmarks = []
    for i, l in enumerate(lines):
        m = _MARK.search(l)
        if m:
            allmarks.append(i)
            if m.group(1) == m.group(2):
                marks.append((i, int(m.group(1))))
    start, j = {}, 0
    for st in steps:
        e0 = _int(st.get("trace_events_start"))
        if e0 is None:
            continue
        for k in range(j, len(marks)):
            if e0 <= marks[k][1] <= e0 + MARK_TOL:
                start[st["n"]] = marks[k][0]
                j = k + 1
                break
    if start:
        ordered = sorted(start.items())
        cuts = {}
        for idx, (n, a) in enumerate(ordered):
            if idx + 1 < len(ordered):
                z = ordered[idx + 1][1]
            else:   # the last step ends at the next dump of any size (autoreport's end-of-run one), else the log's end
                z = next((x for x in allmarks if x > a), len(lines))
            cuts[n] = (a, z)
        return cuts, "markers"
    # the log's own clock
    stamps, cur = [], None
    for l in lines:
        m = _STAMP.match(l)
        if m:
            cur = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))
        stamps.append(cur)
    if all(x is None for x in stamps):
        return {}, None
    ats = [(st["n"], _secs(st.get("at"))) for st in steps]
    seen = [t for t in stamps if t is not None]
    first, last = min(seen), max(seen)
    cuts = {}
    for idx, (n, a) in enumerate(ats):
        if a is None:
            continue
        z = ats[idx + 1][1] if idx + 1 < len(ats) and ats[idx + 1][1] is not None else a + (hold or 30)
        sel = [i for i, t in enumerate(stamps) if t is not None and a <= t < z]
        if sel:
            cuts[n] = (sel[0], sel[-1] + 1)
        elif first <= a and z <= last:   # the log covers the step's time and has no line in it: an empty window
            cuts[n] = (0, 0)
    return cuts, ("clock" if cuts else None)


def front_by_step(steps, hold, logs):
    """({step n: (lines with 'A LIVE WRITER', FRONT SURFACE sample lines)}, [(level, text)]) from the kernel logs
    ([(name, text)]); the log that places most steps is used"""
    notes = []
    if not logs:
        notes.append(("WARN", "no kernel log given (the archive's 20-log-rdna3.txt, a serial-*.log next to the report): the "
                              "FRONT SURFACE column is empty"))
        return {}, notes
    best = None
    for name, text in logs:
        lines = text.splitlines()
        cuts, how = cut_log(lines, steps, hold)
        tot = sum(1 for l in lines if "FRONT SURFACE sample" in l)
        key = (len(cuts), how == "markers", tot)
        if best is None or key > best[0]:
            best = (key, name, lines, cuts, how, tot)
    _, name, lines, cuts, how, tot = best
    if not cuts:
        notes.append(("WARN", "%s: could not place any step in the log (no 'trace: events W..W (asked by sysctl' marker near the "
                              "steps' trace_events_start, no clock in its lines): the FRONT SURFACE column is empty" % name))
        return {}, notes
    out = {}
    for n, (a, z) in cuts.items():
        seg = lines[a:z]
        out[n] = (sum(1 for l in seg if "A LIVE WRITER IS PAINTING" in l), sum(1 for l in seg if "FRONT SURFACE sample" in l))
    unplaced = [st["n"] for st in steps if st["n"] not in cuts]
    notes.append(("WARN" if unplaced else "INFO", "FRONT SURFACE lines per step: %s, cut by %s%s" % (
        name, "the trace_dump marker lines" if how == "markers" else "the log's clock",
        "; step(s) %s not found in it" % ", ".join(map(str, unplaced)) if unplaced else "")))
    inside = sum(v[1] for v in out.values())
    if inside == 0:
        if tot:
            notes.append(("WARN", "the log has %d FRONT SURFACE sample line(s), none inside a step: the kext samples the front "
                                  "surface only %d times per boot (rd3AccelSampleFront) and used them up before the bisect, so "
                                  "a live writer cannot show in the log in this run - the per-step 'inplace' / 'torn' counters "
                                  "are the evidence" % (tot, FRONT_SAMPLE_CAP)))
        else:
            notes.append(("WARN", "no FRONT SURFACE sample line in %s at all (the sampler runs only with the flip path and "
                                  "only %d times per boot): no live-writer evidence from the log" % (name, FRONT_SAMPLE_CAP)))
    return out, notes


def analyse(text, prefix=None, logs=None):
    """{"table": [lines], "findings": [(level, text)], "rows": [(name, text)] for the SUMMARY, "steps": n};
    logs = [(name, kernel log text)] for the agdc variant's FRONT SURFACE column"""
    b = parse(text)
    agdc = b["head"].get("variant", "").startswith("4")
    steps = b["steps"]
    f = []
    table = []
    if not steps:
        f.append(("FAIL", "bisect.txt has no step block%s" % ("; " + "; ".join(b["aborts"]) if b["aborts"] else "")))
        return {"table": [], "findings": f, "rows": [("bisect", "FAIL no step ran%s" % (
            ": " + b["aborts"][0] if b["aborts"] else ""))], "steps": 0}
    prev = None
    mets = {}
    for s in steps:
        mets[s["n"]] = step_metrics(s, prev, prefix)
        prev = s
    verdicts = judge(steps, mets)
    by_n = dict((s["n"], s) for s in steps)
    head = b["head"]
    table.append("bisect: variant %s, hold %s s, mode %s, started %s, %d step(s) in the file%s" % (
        head.get("variant", "?"), head.get("hold_s", "?"), head.get("mode", "?"), head.get("started", "?"), len(steps),
        "" if b["ended"] else " (NO END LINE: the run was cut)"))
    table.append("%4s %-6s %-44s %-5s %5s %5s %-11s %s" % (
        "step", "kind", "what changed", "owner", "pipe0", "pipe1", "moved",
        ("deltas in the step" if agdc else "deltas since the step before") + ": swaps h0/h1, held, would-hold, empty backs"))
    for s in steps:
        n = s["n"]
        m, v = mets[n], verdicts[n]
        pcts = [("%d%%" % m["pct"][p]) if m["pct"].get(p) is not None else ("?" if p in m["lit"] else
                (m["state"].get(p, "off") if p in m["state"] else "-")) for p in ("0", "1")]
        moved = "/".join(("move" if m["moved"].get(p) else "still" if m["moved"].get(p) is False else "?") if p in m["lit"]
                         else "-" for p in ("0", "1"))
        d = m["deltas"]
        ans = s.get("answer", "?")
        table.append("%4d %-6s %-44s %-5s %5s %5s %-11s %s/%s, %s, %s, %s%s" % (
            n, s.get("kind", "?"), _short("(the defaults as the boot left them)" if s.get("kind") == "ref" else s.get("changed", s.get("name", "?")), 44), ans, pcts[0], pcts[1], moved,
            _fmt(d.get("sw0")), _fmt(d.get("sw1")), _fmt(d.get("hold")), _fmt(d.get("would")), _fmt(d.get("empty")),
            "   <-- " + "; ".join(v["why"]) if v["bad"] else ""))
    front = {}
    if agdc:
        front, fnotes = front_by_step(steps, _int(head.get("hold_s")), logs)
        f.extend(fnotes)
        table.append("swap counters per step (from the step's start to its second look, ~20 s in; 'front' = FRONT SURFACE "
                     "live-writer lines / sampler lines in the kernel log during the step):")
        table.append("(the switch column: swap_mode/stale/iop/a3/lat/flip/fbp/gen = %s; gen hold = the write "
                     "generations' holds, fbuf = the family's flip-buffer calls / of them presented)" % ", ".join(AGDC_SWITCHES))
        table.append("%4s %-30s %6s %6s %6s %6s %6s %6s %6s %6s %8s %9s %-9s %s" % (
            "step", "swap_mode/stale/iop/a3/lat/flip/fbp/gen", "sw h0", "sw h1", "copy", "hold", "flips", "inplace", "torn",
            "iop_ok", "gen hold", "fbuf", "front", "trace events"))
        for s in steps:
            n = s["n"]
            d, rb = mets[n]["deltas"], s["readback"]
            sw = "/".join(rb.get("debug.rdna3accel_" + k, "-") for k in AGDC_SWITCHES)
            fr = ("%d/%d" % front[n]) if n in front else "?"
            e0, e1 = _int(s.get("trace_events_start")), _int(s.get("trace_events"))
            table.append("%4d %-30s %6s %6s %6s %6s %6s %6s %6s %6s %8s %9s %-9s %s" % (
                n, sw, _fmt(d.get("sw0")), _fmt(d.get("sw1")), _fmt(d.get("copy")), _fmt(d.get("hold")), _fmt(d.get("flips")),
                _fmt(d.get("inplace")), _fmt(d.get("torn")), _fmt(d.get("iopok")), _fmt(d.get("genhold")),
                "%s/%s" % (_fmt(d.get("flipbuf")), _fmt(d.get("fbpres"))), fr,
                ("%d..%d (stream bytes %s..%s)" % (e0, e1, s.get("stream_bytes_start", "?"), s.get("stream_bytes", "?"))
                 if e0 is not None and e1 is not None else "?")))
    # --- missing data: loud
    for s in steps:
        n = s["n"]
        for pr in mets[n]["problems"]:
            f.append(("WARN", "step %d: %s" % (n, pr)))
        if not s.get("ended"):
            f.append(("WARN", "step %d has no end line (the run was cut inside it)" % n))
        if s.get("failed"):
            f.append(("FAIL", "step %d: a switch did not take: %s" % (n, s["failed"].strip())))
    planned = [p[0] for p in b["plan"]]
    missing = [n for n in planned if n not in by_n]
    if not planned:
        f.append(("WARN", "bisect.txt has no timetable (plan: lines)"))
    if missing:
        f.append(("WARN", "step(s) %s never ran (the run was cut or stopped: %s)" % (
            ", ".join(map(str, missing)), "; ".join(b["aborts"]) or "no ABORT line, the VM's time limit?")))
    for a in b["aborts"]:
        f.append(("FAIL", "bisect aborted: " + a))
    for w in b["warns"]:
        f.append(("WARN", "bisect: " + w))
    if head.get("absent"):
        f.append(("INFO", "this kernel has no sysctl for: %s (those switches were skipped)" % head["absent"].strip()))
    if head.get("mode") == "timer":
        f.append(("INFO", "no owner answers (timer mode, no tty): the verdict rests on the looks"))
    elif not any(s.get("answer") in ("y", "n") for s in steps):
        f.append(("WARN", "tty mode, but the owner answered no step (y / n): the verdict rests on the looks"))
    if head.get("variant", "").startswith("2"):
        f.append(("INFO", "autologin: %s; console user at the start: %s" % (head.get("autologin", "?"), head.get("console_user", "?"))))
    # the switches at step 0 against the compiled defaults
    s0 = by_n.get(0)
    if s0 and s0["readback"]:
        odd = ["%s %s (default %s)" % (k, s0["readback"].get("debug.rdna3accel_" + k), dv)
               for k, dv in sorted((DEFAULTS_AGDC if agdc else DEFAULTS).items())
               if s0["readback"].get("debug.rdna3accel_" + k) not in (None, "-", dv)]
        if odd:
            f.append(("WARN", "step 0 is not the compiled defaults: %s (a boot-arg set it?); the 'on' steps return to what the boot left" %
                      ", ".join(odd)))
    # --- the owner's n, the looks' black / still
    firstn = next((s for s in steps if s.get("answer") == "n" and s.get("kind") in MAIN_KINDS), None)
    anyn = next((s for s in steps if s.get("answer") == "n"), None)
    pick = firstn or anyn
    if pick:
        f.append(("FAIL", "the first step the owner marked n: step %d (%s)%s" % (
            pick["n"], label(pick), " - the reference, the bug as it is; %s below says where it comes from" % ("BISECT-AGDC" if agdc else "BISECT")
            if pick["n"] == 0 else "")))
    for what, key in (("black", "black"), ("stood still", "still")):
        for s in steps:
            if s.get("kind") not in MAIN_KINDS:
                continue
            w = [x for x in verdicts[s["n"]]["why"] if key in x]
            if w:
                f.append(("FAIL" if key == "black" else "WARN", "the first step whose looks %s: step %d (%s): %s%s" % (
                    what, s["n"], label(s), "; ".join(w),
                    " - the reference, the bug as it is" if s["n"] == 0 else "")))
                break
    f.extend(verdict_agdc(steps, verdicts, by_n) if agdc else verdict(steps, verdicts, by_n))
    for s in steps:
        if s.get("kind") == "extra" and verdicts[s["n"]]["bad"]:
            f.append(("INFO", "control step %d (%s) looked bad: expected, these are the off-by-default states: %s" % (
                s["n"], s.get("name", ""), "; ".join(verdicts[s["n"]]["why"]))))
    cut = not b["ended"] or bool(missing)
    rows = [("bisect", "%s %d step(s) in the file%s, mode %s" % ("WARN" if cut else "INFO", len(steps),
                                                               " (UNFINISHED)" if cut else "", head.get("mode", "?")))]
    for lv, t in f:
        if lv in ("FAIL", "WARN") and (t.startswith(("the first step", "BISECT", "bisect aborted", "step ")) or "never ran" in t):
            rows.append(("bisect", "%s %s" % (lv, _short(t, 300))))
        elif lv == "INFO" and t.startswith("BISECT"):
            rows.append(("bisect", "INFO " + _short(t, 300)))
    return {"table": table, "findings": f, "rows": rows, "steps": len(steps)}


def verdict(steps, verdicts, by_n):
    """[(level, text)]: what the run says (ref = step 0 at the defaults, off = step 1 with every switch off, then
    the 'on' steps oldest first, the final step back at the defaults)"""
    out = []

    def st(n):
        return verdicts[n] if n in verdicts else None

    ref, off = st(0), st(1)
    ons = [s for s in steps if s.get("kind") == "on"]
    if ref is None or not ref["known"]:
        out.append(("WARN", "BISECT: the reference (step 0) has no readable look and no answer: nothing to compare the others with"))
        return out
    if off is None or not off["known"]:
        out.append(("WARN", "BISECT: step 1 (every switch off) has no readable look and no answer: the run cannot say whether a switch matters"))
    firston = next((s for s in ons if verdicts[s["n"]]["bad"]), None)
    if ref["bad"] and off is not None and off["known"] and off["bad"]:
        out.append(("FAIL", "BISECT: still broken with every switchable feature off (step 1: %s): not one of these switches - "
                            "look at the boot-args (rdna3gamma, rdna3visreserve ...), the copy forward, kext code without a switch, "
                            "the Metal plugin (step bisect-plugin) or the hardware (docs/TESTING-VM.md 'Display bisect')" %
                            "; ".join(off["why"])))
    elif ref["bad"] and off is not None and off["known"]:
        if firston:
            s = firston
            out.append(("FAIL", "BISECT: BREAKS AT step %d (%s): fine with every switch off (step 1), broken at the defaults "
                                "(step 0); step %d is the first that switches a feature back on and breaks [%s] - the steps are "
                                "cumulative, so the earlier switches are on too" % (
                                    s["n"], label(s), s["n"], "; ".join(verdicts[s["n"]]["why"]))))
        else:
            out.append(("WARN", "BISECT: the defaults (step 0) looked broken and every switch off (step 1) did not, but no 'on' step "
                                "broke: the bug is intermittent or needs the whole set - compare the final step"))
    elif not ref["bad"] and off is not None and off["known"] and off["bad"]:
        out.append(("WARN", "BISECT: the defaults (step 0) looked fine but every switch off (step 1) broke: the old behaviour is the "
                            "broken one, or the bug is intermittent"))
    else:
        if firston:
            out.append(("WARN", "BISECT: step %d (%s) looked broken though step 0 and step 1 did not: intermittent, or an order "
                                "effect (the switches are cumulative)" % (firston["n"], firston.get("name", ""))))
        else:
            out.append(("INFO", "BISECT: no step looked broken - the bug did not show in this run (or needs real apps, or only "
                                "with the screens as the owner uses them)"))
    # a later step with the switches of step 0 (the 'on' steps end there, the final step returns there)
    r0 = by_n.get(0)
    for s in steps:
        if s["n"] == 0 or s.get("kind") not in ("on", "final") or not r0 or not r0["readback"]:
            continue
        if r0["readback"] == s["readback"] and verdicts[0]["known"] and verdicts[s["n"]]["known"] \
                and verdicts[0]["bad"] != verdicts[s["n"]]["bad"]:
            out.append(("WARN", "BISECT: step %d has the same switches as step 0 but looked %s there, %s here: flaky, or the "
                                "state before decides" % (s["n"], "broken" if verdicts[s["n"]]["bad"] else "fine",
                                                         "broken" if verdicts[0]["bad"] else "fine")))
    return out


def _vary(s):
    """what a step of the agdc plan sets, short"""
    if s.get("kind") == "ref":
        return "the defaults"
    name = s.get("name", "?")
    return name if len(name) <= 70 else name[:69] + "~"


def verdict_agdc(steps, verdicts, by_n):
    """[(level, text)] for `bisect-agdc` (plan 4): the steps are not cumulative, so no 'breaks at'; it names the steps
    that looked RIGHT and the ones that looked broken, and the same by swap_mode"""
    out = []
    ref = verdicts.get(0)
    judged = [s for s in steps if verdicts[s["n"]]["known"]]
    right = [s for s in judged if not verdicts[s["n"]]["bad"]]
    wrong = [s for s in judged if verdicts[s["n"]]["bad"]]
    unknown = [s for s in steps if not verdicts[s["n"]]["known"]]

    def lst(ss, why=False):
        return "; ".join("step %d (%s)%s" % (s["n"], _vary(s), " [%s]" % "; ".join(verdicts[s["n"]]["why"]) if why else "")
                         for s in ss) or "none"
    if ref is None or not ref["known"]:
        out.append(("WARN", "BISECT-AGDC: the reference (step 0) has no readable look and no answer: nothing to compare the others with"))
    if ref is not None and ref["known"] and ref["bad"]:
        rr = [s for s in right if s["n"] != 0]
        if rr:
            out.append(("FAIL", "BISECT-AGDC: the reference (step 0, the agdc profile's defaults) looked BROKEN [%s]; the steps that "
                                "looked RIGHT: %s" % ("; ".join(ref["why"]), lst(rr))))
        else:
            out.append(("FAIL", "BISECT-AGDC: the reference (step 0) looked BROKEN [%s] and NO step looked right: none of swap_mode "
                                "0/1/2, swap_gen, flipbuf_present, swap_stale, iop, a3, swap_latch, iop_flip repairs it on the agdc profile (or the answers are "
                                "missing: see the WARN lines) - the bug is elsewhere (WindowServer's own draw path, the plugin: "
                                "step bisect-plugin, the copy forward, the hardware)" % "; ".join(ref["why"])))
        out.append(("INFO", "BISECT-AGDC: looked broken: %s" % lst([s for s in wrong if s["n"] != 0])))
    elif ref is not None and ref["known"]:
        if wrong:
            out.append(("WARN", "BISECT-AGDC: the reference (step 0) looked RIGHT - the bug did not show at the defaults - but these "
                                "steps looked BROKEN: %s" % lst(wrong, True)))
        else:
            out.append(("INFO", "BISECT-AGDC: no step looked broken - the bug did not show in this run (or needs real apps, or only "
                                "with the screens as the owner uses them)"))
    if unknown:
        out.append(("WARN", "BISECT-AGDC: not judged (no readable look and no answer): %s" % lst(unknown)))
    # by swap_mode: the question the plan was made for
    groups = {}
    for s in steps:
        mode = s["readback"].get("debug.rdna3accel_swap_mode")
        if mode is None:
            continue
        g = groups.setdefault(mode, {"right": [], "wrong": [], "?": []})
        v = verdicts[s["n"]]
        g["?" if not v["known"] else "wrong" if v["bad"] else "right"].append(s["n"])
    if groups:
        out.append(("INFO", "BISECT-AGDC by swap_mode: " + "; ".join(
            "mode %s: right %s, broken %s%s" % (m, g["right"] or "none", g["wrong"] or "none", ", not judged %s" % g["?"] if g["?"] else "")
            for m, g in sorted(groups.items()))))
    # the same switches as step 0 must look the same (an order effect: the iop steps switch the pipe capabilities at runtime)
    r0 = by_n.get(0)
    for s in steps:
        if s["n"] == 0 or s.get("kind") != "final" or not r0 or not r0["readback"]:
            continue
        if r0["readback"] == s["readback"] and verdicts[0]["known"] and verdicts[s["n"]]["known"] \
                and verdicts[0]["bad"] != verdicts[s["n"]]["bad"]:
            out.append(("WARN", "BISECT-AGDC: step %d has the same switches as step 0 but looked %s there, %s here: flaky, or an "
                                "order effect (iop 0 in steps 4 and 5 changes what WindowServer was told about the pipes at runtime)" % (
                                    s["n"], "broken" if verdicts[s["n"]]["bad"] else "right",
                                    "broken" if verdicts[0]["bad"] else "right")))
    return out


def locate(report_path):
    """the bisect.txt text of a report: the archive's 39-bisect.txt or the <report>-bisect.txt the VM uploaded
    after every step - the longer one; "" when there is none"""
    texts = []
    base = report_path[:-len(".tar.gz")] if report_path.endswith(".tar.gz") else report_path
    try:
        with open(base + "-bisect.txt", errors="replace") as f:
            texts.append(f.read())
    except OSError:
        pass
    if report_path.endswith(".tar.gz") and os.path.exists(report_path):
        import tarfile
        try:
            with tarfile.open(report_path) as tar:
                for m in tar.getmembers():
                    if m.isfile() and m.name.endswith("bisect.txt"):
                        texts.append(tar.extractfile(m).read().decode("utf-8", "replace"))
        except (OSError, tarfile.TarError):
            pass
    return max(texts, key=len) if texts else ""


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    text = open(sys.argv[1], errors="replace").read()
    prefix = sys.argv[2] if len(sys.argv) > 2 else None
    r = analyse(text, prefix)
    for l in r["table"]:
        print(l)
    for lv, t in r["findings"]:
        print("%s %s" % (lv, t))
    sys.exit(1 if any(lv == "FAIL" for lv, _ in r["findings"]) else 0)


if __name__ == "__main__":
    main()
