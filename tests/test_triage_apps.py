#!/usr/bin/env python3
"""tools/triage_apps.py (the app survey, `rdna3vm step apps`) on made-up 41-ladder.txt blocks: an app that ran clean, one that crashed with a report, one
that did not open, one the owner already had open, a WindowServer that restarted, notable log lines, the Terminal's glyph verdict, an empty report."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import triage_apps as a  # noqa: E402
import triage_glyphs as tg  # noqa: E402

FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("FAIL: " + what)


def block(name, result="ran", app=None, stages="main", pids=None, plugin="App(1000)", ws=("90", "90"), extra=(), looks=None):
    title = app or name
    looks = looks if looks is not None else ["apps-%sa" % name, "apps-%sh" % name]
    out = ["== rung " + name, "scope: apps", "conf: (nothing)", "app: " + title, "stages: " + stages, "started: 23:00:00", "ws_before: " + ws[0],
           "pids: %s=%s" % (title.replace(" ", "_"), pids or "1000"), "-- look %s (23:00:20)" % looks[0], "exit: 0"]
    out += ["looks: " + " ".join(looks), "alive: 1000 (of: 1000 )", "helpers: none", "plugin: " + plugin, "ws_after: " + ws[1]]
    out += list(extra)
    out += ["result: " + result, "== end rung " + name]
    return "\n".join(out)


def census(pid, draws, present):
    return "2026-10-26 23:00:11.000 Df  App[%d:1f] (RDNA3MTLDriver) NVMTL: census: SUMMARY seq=1 pass=10 draws=%d present=%d" % (pid, draws, present)


def rows_for(blocks, logs=None, glyphs=None, problems=()):
    text = "# rdna3 ladder v1\nvariant: 5 (apps)\n" + "\n".join(blocks) + "\nfinished: 23:30:00\n== end\n"
    v, rungs, probs = tg.parse_ladder(text, logs or {})
    check(v == 5, "parse_ladder reads variant 5")
    return {n: (lv, t) for lv, n, t in a.apps_rows(text, rungs, list(probs) + list(problems), glyphs, logs or {})}


ALL_OK = [block(n) for n in a.APPS_RUNGS]
LOGS_OK = {n: census(1000, 9, 4) for n in a.APPS_RUNGS}

# a clean survey: every row INFO, the summary counts them, the title is the app's own name
d = rows_for(ALL_OK, LOGS_OK)
check(set(d) == {"app " + n for n in a.APPS_RUNGS} | {"apps survey"}, "a row per app and the summary: %r" % sorted(d))
check(all(v[0] == "INFO" for v in d.values()), "a clean survey has INFO rows only: %r" % {k: v for k, v in d.items() if v[0] != "INFO"})
check(d["app settings"][1].startswith("System Settings: ran") and "15 ran, 0 crashed" in d["apps survey"][1], "titles and counts: %r" % ((d["app settings"], d["apps survey"]),))
check("plugin census: 9 draws, 4 presents" in d["app chess"][1], "the plugin's draw counters: " + d["app chess"][1])
check("apps-chessa apps-chessh" in d["app chess"][1], "the look names are in the row")

# crashed (report), exited by itself, did not open, the owner's own instance, no plugin, a missing app
bl = [block(n) for n in a.APPS_RUNGS if n not in ("preview", "maps", "notes", "calculator", "photos", "appstore")]
bl += [block("preview", "crashed (report: Preview-2026-10-26-230000.ips)", extra=["died: pid 1000 was gone after stage pdf (23:00:30)", "crash: Preview-2026-10-26-230000.ips (this app)"]),
       block("maps", "exited by itself (1000 after stage main) with no crash report", extra=["died: pid 1000 was gone after stage main (23:00:30)", "crash: none"]),
       block("notes", "did not open", extra=["WARN: Notes did not start within 20 s of the open (open exit 1)"]),
       block("calculator", "was already running (the owner's instance)", plugin="not checked (no process started by this rung)"),
       block("photos", plugin="NOT loaded in any process of the app (lsof)")]
logs = dict(LOGS_OK, **{"preview-crash": "== Preview-2026-10-26-230000.ips\nexception: {\"type\":\"EXC_BAD_ACCESS\"}\ncrashed thread 0:\n  libsystem  x + 1\n"})
d = rows_for(bl, logs)
check(d["app preview"][0] == "FAIL" and "crashed" in d["app preview"][1] and "EXC_BAD_ACCESS" in d["app preview"][1] and "Preview-2026-10-26-230000.ips" in d["app preview"][1], "crash row: %r" % (d["app preview"],))
check(d["app maps"][0] == "FAIL" and "exited by itself" in d["app maps"][1], "exit row: %r" % (d["app maps"],))
check(d["app notes"][0] == "WARN" and "did not open" in d["app notes"][1] and "did not start within" in d["app notes"][1], "did-not-open row: %r" % (d["app notes"],))
check(d["app calculator"][0] == "WARN" and "no `census" not in d["app calculator"][1] and "plugin loaded in" not in d["app calculator"][1], "the owner's instance is not judged on the plugin: %r" % (d["app calculator"],))
check(d["app photos"][0] == "WARN" and "NOT loaded" in d["app photos"][1], "plugin not loaded: %r" % (d["app photos"],))
check(d["app appstore"][0] == "WARN" and "did not run" in d["app appstore"][1], "a missing block: %r" % (d["app appstore"],))
check("1 crashed, 1 exited by themselves, 1 did not open" in d["apps survey"][1] and "6 other" not in d["apps survey"][1], "counts: %r" % (d["apps survey"],))

# WindowServer restarted under an app, and the survey stopped there
bl = [block("chess", "ran; WindowServer restarted", ws=("90", "91"), extra=["WARN: WindowServer restarted during this app (pid 90 -> 91): the desktop session was rebuilt"])]
d = rows_for(bl + ["login: NOBODY logged in 0s after the WindowServer restart: the rungs after this one did not run", "ABORT: the survey stops at chess (nobody is logged in): the apps after it did not run"], {"chess": census(1000, 1, 1)})
check(d["app chess"][0] == "FAIL" and "WindowServer pid 90 -> 91" in d["app chess"][1], "WindowServer restart row: %r" % (d["app chess"],))
check(d["app terminal"][0] == "WARN" and "did not run" in d["app terminal"][1], "the apps after the stop are missing rows")
check("WindowServer restarted or crashed under 1" in d["apps survey"][1] and "14 missing" in d["apps survey"][1], "the summary: %r" % (d["apps survey"],))

# notable log lines: a Metal error, a GPU restart, a VM fault; generic error words are counted but are no WARN on their own
sysl = "\n".join([
    "2026-10-26 23:00:13.000 E   Safari[1000:1f] (Metal) MTLDevice error: invalid pixel format",
    "2026-10-26 23:00:14.000 E   kernel[0:0] RDNA3Accel: gfx ring timeout, GPU reset",
    "2026-10-26 23:00:15.000 E   kernel[0:0] RDNA3Accel: VM fault at 0x1000",
    "2026-10-26 23:00:16.000 E   WindowServer[90:1] some harmless error: file not found",
])
d = rows_for([block("safari", stages="text canvas webgl", looks=["apps-safari-texta", "apps-safari-texth"])], {"safari": census(1000, 3, 3), "safari-sys": sysl})
t = d["app safari"][1]
check(d["app safari"][0] == "WARN" and "Metal/NVMTL error: 1 line(s)" in t and "GPU restart/hang: 1 line(s)" in t and "VM fault: 1 line(s)" in t, "notable categories: " + t)
check("1 other error/fault line(s)" in t and "stages text canvas webgl" in t, "generic count and stages: " + t)
check(sorted(a.scan_notables(sysl)) == ["GPU restart/hang", "Metal/NVMTL error", "VM fault"], "scan_notables: %r" % (a.scan_notables(sysl),))
check(a.draw_counters("") is None and a.draw_counters(census(5, 2, 1) + "\n" + census(5, 7, 3) + "\n" + census(6, 1, 1)) == (8, 4, ["5", "6"]), "draw_counters keeps the newest census per process")

# no census at all: said, never silent
d = rows_for([block("maps")], {"maps": "2026-10-26 23:00:01.000 Df  App[1000:1f] (RDNA3MTLDriver) NVMTL: vk: ab switches: none set (x)"})
check(d["app maps"][0] == "WARN" and "no `census: SUMMARY`" in d["app maps"][1], "no census: " + d["app maps"][1])

# the Terminal's glyph verdict reaches its row; broken text is a WARN
gl = {"terminal": {"verdict": "broken", "text": "BROKEN: 3 of 20 tick lines have no twin"}}
d = rows_for([block("terminal", looks=["apps-terminala", "apps-terminalh", "apps-terminalh2"])], {"terminal": census(1000, 3, 3)}, gl)
check(d["app terminal"][0] == "WARN" and "glyphs (automatic, tools/glyph_verdict.py): BROKEN" in d["app terminal"][1] and "apps-terminalh2" in d["app terminal"][1], "glyph row: " + d["app terminal"][1])

# problems of the run (outside any app) are rows; a run with no apps at all is all warnings, not an exception
d = rows_for([], {}, None, ["ABORTED by SIGTERM at rung safari (23:10:00): the rungs after it did not run"])
check(d["apps"][0] == "WARN" and "ABORTED" in d["apps"][1] and len([k for k in d if k.startswith("app ") and d[k][0] == "WARN"]) == 15, "empty run: %r" % (sorted(d),))

# the app list is the autoreport.sh one (tests/test_apps_step.sh compares the stage list too)
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "vm", "macos", "autoreport.sh")).read()
check('APPS_RUNGS="%s"' % " ".join(a.APPS_RUNGS) in src, "APPS_RUNGS of autoreport.sh = triage_apps.APPS_RUNGS")
check(set(a.APPS_TITLE) == set(a.APPS_RUNGS), "a title for every app")

# the whole triage on a synthetic report (tools/triage_reports.py): the app rows are printed, only the Terminal is sent to the glyph check
import io, subprocess, tarfile, tempfile  # noqa: E402,E401
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
td = tempfile.mkdtemp()
prefix = os.path.join(td, "rdna3-report-test-1")
ladder = "# rdna3 ladder v1\nvariant: 5 (apps)\n" + "\n".join(
    block(n, looks=["apps-%sa" % n, "apps-%sh" % n] + (["apps-%sh2" % n] if n == "terminal" else [])) for n in a.APPS_RUNGS) + "\n== end\n"
files = {"01-boot-args.txt": "rdna3lvl=8 rdna3accel=1 rdna3ladder=5\n", "03-kexts.txt": "RDNA3Accelerator 1.0 loaded\n", "41-ladder.txt": ladder,
         "41-ladder-chess.log": census(1000, 3, 2) + "\n", "41-ladder-chess-sys.log": "2026-10-26 23:00:15.000 E   kernel[0:0] RDNA3Accel: VM fault at 0x1000\n"}
with tarfile.open(prefix + ".tar.gz", "w:gz") as t:
    for n, body in files.items():
        data = body.encode()
        ti = tarfile.TarInfo("rdna3-report-AbC/" + n)
        ti.size = len(data)
        t.addfile(ti, io.BytesIO(data))
open(prefix + "-tests.txt", "w").write("== tests\n")
p = subprocess.run([sys.executable, "-I", "-B", os.path.join(ROOT, "tools", "triage_reports.py"), td], capture_output=True, text=True, env=dict(os.environ, RDNA3_NO_PICTURES="1"))
out = p.stdout
check("app chess" in out and "app mission-control" in out and "apps survey" in out, "triage_reports prints the app rows: %r" % (out[-800:], ))
check("VM fault: 1 line(s)" in out and "plugin census: 3 draws, 2 presents" in out, "triage_reports: the chess row reads its two log slices")
check(out.count("glyphs (automatic") == 1 and "app terminal" in out, "triage_reports: only the Terminal is sent to the glyph check (%d verdicts)" % out.count("glyphs (automatic"))

if FAILS:
    print("%d check(s) failed" % len(FAILS))
    sys.exit(1)
print("test_triage_apps: ok")
