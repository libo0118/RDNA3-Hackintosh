#!/usr/bin/env python3
"""result_checklist.py - every test of a card run on its own line.

    result_checklist.py [--no-color] [--width N] 00-triage.txt

Reads the output of tools/triage_reports.py (the 00-triage.txt that
'rdna3vm result' writes) and prints the "SUMMARY of the newest report"
section as a checklist: one line per row, a verdict first,

    PASS | FAIL | WARN | INFO | NOT RUN   <name>   <what the triage said>

then the counts and the triage's RESULT line. The verdict of a row is
decided by the rules in classify() below, from the text the triage prints
(it has no machine-readable form). A row no rule knows is shown as INFO
with its text, never dropped. Python 3, standard library only. This is a
display tool: the exit status is 0 whatever the rows say (a missing or
unreadable file is reported on stdout).
"""
import re
import shutil
import sys

PASS, FAIL, WARN, INFO, NOT_RUN = "PASS", "FAIL", "WARN", "INFO", "NOT RUN"
ORDER = (PASS, FAIL, WARN, INFO, NOT_RUN)
COLOR = {PASS: "32", FAIL: "1;31", WARN: "33", INFO: "90", NOT_RUN: "90"}
NAME_COL = 22          # triage_reports.py prints "  %-22s %s"

def split_row(line):
    """(name, text) of a SUMMARY row: '  %-22s %s' (sub-items carry two more
    leading spaces in the name); a name longer than the column ends at the
    next space"""
    body = line[2:]
    if len(body) > NAME_COL and body[NAME_COL] == " ":
        return body[:NAME_COL].rstrip(), body[NAME_COL + 1:].strip()
    sp = body.find(" ", NAME_COL)
    if sp < 0:
        return body.rstrip(), ""
    return body[:sp].rstrip(), body[sp + 1:].strip()


def _lvl(word):
    return {"PASS": PASS, "FAIL": FAIL, "WARN": WARN, "INFO": INFO, "CRITICAL": FAIL}[word]


def classify(name, text):
    """(verdict, text) of one SUMMARY row. The rules, in this order:

    1. the text starts with PASS / FAIL / WARN / INFO, also behind an
       upper-case label ("GAMMA: FAIL", "SURFXPROC: PASS"), or says
       "-> PASS" ("magenta pixels: ... -> PASS: ...");
    2. the text's own counts: "N verified, M wrong", "N of M apps ran",
       "N switch(es) tried, K failed", "N rounds, K failed", "pass N, fail K";
       an app's "running ..., plugin ..., crash report: ..." line;
    3. ALL PASSED / PASSED -> PASS; FAILED / CRASHED / NOT STARTED /
       FROZEN / NO PRESENT CALLBACK -> FAIL; NOTHING TESTED / not run /
       NOT MEASURED / not judged / no present path -> NOT RUN;
    4. the 'screens ...' looks (lit pipes, UNDERFLOW, SAME SURFACE,
       NO TX LINES, TX problems, a sleep look), M5 / M6 / TX watch / VM faults;
    5. everything else (boot-args, platform, power, counters, flip latch
       details ...): a keyword (FAILED, UNDERFLOW, NOT FINISHED ...) decides,
       else INFO."""
    n, t = name.strip().lower(), text
    low = t.lower()

    # 1. an explicit verdict in the text
    m = re.match(r"^(?:[A-Z][A-Z0-9 ]*: )?(PASS|FAIL|CRITICAL|WARN|INFO)\b", t)
    if m:
        if t.startswith(m.group(1) + " "):   # a bare "WARN head 0: ..." says it in the verdict column
            t = t[len(m.group(1)) + 1:]
        return _lvl(m.group(1)), t
    m = re.search(r"-> (PASS|FAIL|WARN)\b", t)
    if m:
        return _lvl(m.group(1)), t
    # an upper-case label ("METAL: ", "MODES: ") is dropped, a verdict word before the colon is not
    body = re.sub(r"^(?!(?:ALL PASSED|PASSED|FAILED|CRASHED|NOT STARTED|NOTHING TESTED)\b)[A-Z][A-Z0-9 ]*: ", "", t)

    # 2. counts
    m = re.match(r"^(\d+) verified, (\d+) wrong", t)
    if m:
        if int(m.group(2)) > 0:
            return FAIL, t
        return (PASS if int(m.group(1)) > 0 else NOT_RUN), t   # nothing verified: no DMA/GFX step ran
    m = re.match(r"^(\d+) of (\d+) apps ran", t)
    if m:
        return (PASS if m.group(1) == m.group(2) else FAIL), t
    m = re.match(r"^running (yes|NO), plugin (loaded|NOT loaded), crash report: (.*)$", t)
    if m:
        ok = m.group(1) == "yes" and m.group(2) == "loaded" and m.group(3).strip() == "none"
        return (PASS if ok else FAIL), t
    m = re.match(r"^(\d+) (?:switch\(es\) tried|rounds), (\d+) failed", body)
    if m:
        if int(m.group(2)) > 0:
            return FAIL, t
        return (PASS if int(m.group(1)) > 0 else NOT_RUN), t
    m = re.match(r"^pass (\d+), fail (\d+)(?:, not supported \d+ of \d+ cases; (\d+) group)?", body)
    if m:      # Vulkan CTS total or group
        if int(m.group(2)) > 0:
            return FAIL, t
        return (FAIL if int(m.group(3) or 0) > 0 else PASS), t   # a group without totals hung or died
    if name.strip() == "failed case":
        return FAIL, t

    # 3. keywords at the start (behind a label)
    if re.match(r"^(ALL )?PASSED\b", body):
        return PASS, t
    if re.match(r"^(FAILED|CRASHED|NOT STARTED)\b", body) or "NO PRESENT CALLBACK" in t or "FROZEN" in t \
            or "no verdict" in low or re.match(r"^no totals", body):
        return FAIL, t
    if re.match(r"^(NOTHING TESTED|not run)\b", body) or "NOT MEASURED" in t or "not judged" in low \
            or "no present path" in low or t == "?":
        return NOT_RUN, t
    if "UNJUDGED" in t:
        return WARN, t

    # 4. specific rows
    if n.startswith("screens "):
        return _screens(n, t)
    if n.startswith("m6 screens"):
        if "NOT FINISHED" in t or t.startswith("no pictures"):
            return WARN, t
        if re.search(r"unchanged(?! \(not main\))", t):
            return WARN, t
        return (PASS if "changed" in t else INFO), t
    if n.startswith("m5 windowserver"):
        if "did not load" in t:
            return FAIL, t
        return (WARN if "not on the allow list" in t else PASS), t
    if n.startswith("v3 vulkan wsi"):
        return (PASS if re.search(r"-> 0\b", t) else WARN), t
    if n.startswith("tx watch"):
        if re.match(r"^(NEVER TICKED|NO TICK|off)\b", t):
            return NOT_RUN, t
        return (WARN if "with problems" in t else INFO), t
    if n.startswith("vm faults"):
        return WARN, t
    if n.startswith("display sleep") and "(kernel log): NONE" in t:
        return WARN, t
    if n.startswith("r4 macos") and "not running" in t:
        return FAIL, t
    if n.startswith("m4 on the screen") and t.endswith("no picture"):
        return NOT_RUN, t
    if n.startswith("boot-args"):
        return INFO, t

    # 5. descriptive rows and the rest: a keyword decides, else INFO
    if re.search(r"\b(FAILED|CRASHED|CRITICAL)\b|\bFAIL\b", t):
        return FAIL, t
    if re.search(r"\bWARN\b|UNDERFLOW|NOT FINISHED|SAME SURFACE", t):
        return WARN, t
    return INFO, t


def _screens(n, t):
    """one 'screens <look>' row: '<k> lit[, <j> blanked][, UNDERFLOW]
    [, SAME SURFACE on two pipes][, TX: <problems>][, NO TX LINES]'"""
    if t.startswith("NOT READ"):
        return NOT_RUN, t
    m = re.match(r"^(\d+) lit(?:, (\d+) blanked)?", t)
    if not m:
        return INFO, t
    lit, blanked = int(m.group(1)), int(m.group(2) or 0)
    if lit == 0:
        # the sleep look expects dark pipes (rdna3dpms=1); any other look
        # with no lit pipe is a dark or "no signal" screen
        return (PASS if "sleep" in n and blanked > 0 else FAIL), t
    if re.search(r"dark|NO SIGNAL", t):
        return FAIL, t
    if "sleep" in n:     # lit pipes in the sleep look: nothing blanked is a failed display sleep
        return (FAIL if blanked == 0 else WARN), t
    if "wake" in n and blanked > 0:
        return FAIL, t   # a pipe is still blanked after the wake
    if re.search(r"BLACK pipe", t):   # a lit pipe whose picture is (nearly) all black (ECC review round 2)
        return FAIL, t
    if re.search(r"UNDERFLOW|SAME SURFACE|NO TX LINES|TX: |NOT READABLE", t):
        return WARN, t
    return PASS, t


def _fail_name(rest):
    """(name, text) of an indented triage line 'FAIL <name padded> <text>';
    the name when it is set off by two spaces or ends in a colon, else the
    whole line as text"""
    m = re.match(r"^(\S+(?: \S+){0,3}?)\s{2,}(\S.*)$", rest)
    if m:
        return "triage: " + m.group(1), m.group(2)
    m = re.match(r"^([A-Za-z][A-Za-z0-9/ ]{0,30}?): (.*)$", rest)
    if m:
        return "triage: " + m.group(1), m.group(2)
    return "triage", rest


def parse(lines):
    """(rows, result line, note). rows = [(name, text, forced)]: the SUMMARY
    section's rows (forced None) and every indented 'FAIL ...' detail line
    outside it (forced FAIL: stale builds, VM faults, WindowServer starts,
    planes in transactions, display sleep ... are printed only there). Without
    a SUMMARY (no report from macOS arrived) the triage's '!!' complaints,
    their continuation lines joined, are the rows."""
    rows, result, note, in_sum = [], "", "", False
    forced = []
    for line in lines:
        s = line.rstrip("\n")
        if s.startswith("RESULT:"):
            result = s
            in_sum = False
        elif s.startswith("SUMMARY of the newest report"):
            in_sum = True
        elif in_sum and s.startswith("====="):
            in_sum = False
        elif in_sum and s.startswith("  ") and s.strip():
            rows.append(split_row(s) + (None,))
        elif not in_sum:
            m = re.match(r"^\s+(?:FAIL|CRITICAL)\s+(\S.*)$", s)
            if m:
                forced.append(_fail_name(m.group(1).strip()) + (FAIL,))
    if not rows:
        note = "no SUMMARY in the triage (no report from macOS arrived): the triage's own complaints"
        i = 0
        while i < len(lines):
            s = lines[i].rstrip("\n")
            if s.lstrip().startswith("!!"):
                msg = s.lstrip()[2:].strip()
                j = i + 1       # the message goes on in indented lines (up to 3)
                while j < len(lines) and j - i <= 3 and re.match(r"^\s+\S", lines[j]) and \
                        not re.match(r"^\s*(!!|FAIL\b)", lines[j]):
                    msg += " " + lines[j].strip()
                    j += 1
                rows.append(("triage", msg, FAIL))
                i = j
            else:
                i += 1
    return forced + rows, result, note


def render(rows, result, note, color, width):
    out = []
    if note:
        out.append(note)
    namew = min(max([len(r[0]) for r in rows] or [0]), 32)
    counts = dict.fromkeys(ORDER, 0)
    failed = []
    for nm, tx, forced in rows:
        if forced:
            lv = forced
        else:
            lv, tx = classify(nm, tx)
        counts[lv] += 1
        if lv == FAIL and nm.strip() not in failed:
            failed.append(nm.strip())
        tag = "%-7s" % lv
        if color:
            tag = "\033[%sm%s\033[0m" % (COLOR[lv], tag)
        room = max(width - 7 - 1 - namew - 2, 20)
        if len(tx) > room:
            tx = tx[:room - 3] + "..."
        out.append("%s %-*s  %s" % (tag, namew, nm, tx))
    out.append("")
    out.append(", ".join("%d %s" % (counts[k], k) for k in ORDER))
    if failed:
        out.append("FAIL: " + ", ".join(failed))
    if result:
        out.append(result)
        if "failed" in result and not counts[FAIL]:
            out.append("RESULT says failed but no FAIL row found: read 00-triage.txt")
    return "\n".join(out)


def main(argv):
    color = sys.stdout.isatty()
    width = shutil.get_terminal_size((120, 24)).columns if color else 120
    path = None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--no-color":
            color = False
        elif a == "--width" and i + 1 < len(argv):
            i += 1
            try:
                width = int(argv[i])
            except ValueError:
                print("result_checklist: --width needs a number")
                return 0
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            path = a
        i += 1
    if not path:
        print(__doc__)
        return 0
    try:
        with open(path, errors="replace") as f:
            lines = f.read().splitlines()
    except OSError as e:
        print("result_checklist: cannot read %s: %s" % (path, e))
        return 0
    rows, result, note = parse(lines)
    if not rows and not result:
        print("result_checklist: no SUMMARY and no RESULT line in %s" % path)
        return 0
    print(render(rows, result, note, color, width))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
