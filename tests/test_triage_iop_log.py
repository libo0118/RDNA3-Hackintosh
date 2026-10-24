"""DRIVER-PLAN N45 (6): tools/triage_reports.py's "IOPresentment log" row, from the WindowServer log
(24-log-coredisplay.txt) that collect.sh keeps: the strings IOPresentment / CoreDisplay log when the display
pipe's capabilities (AGDC 0x711 / 0x921 answers) are refused, and the one for a built inline buffer.
 - each failure string: FAIL, with its meaning, the count and the first line;
 - 'AGDC IB successful': PASS; a failure and a success together: WARN (it failed first, then worked), never FAIL;
 - neither: INFO that IOPresentment did not get as far as the capabilities;
 - no log / only collect.sh's "$ cmd" and "[exit N]" wrapper lines: WARN, never silence;
 - 'QueueDepth' present / absent: an INFO row;
 - present_mode_lines carries the rows (a report archive), and collect.sh's filter selects the strings.
Run: python3 -I -B tests/test_triage_iop_log.py"""
import io
import os
import re
import sys
import tarfile
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import triage_reports as tr  # noqa: E402

fails = []


def check(cond, what):
    if not cond:
        fails.append(what)


def levels(rows):
    return [r[0] for r in rows]


P = "2026-10-24 12:00:00.000 Df WindowServer[200:1a] [com.apple.IOPresentment:Capabilities] "
WRAP = "$ sh -c log show ...\n%s[exit 0]\n"
FAILS = (("Invalid end point size", "end point"), ("capabilities with no devices", "DeviceArray"),
         ("device with no planes", "PlaneArray"), ("Failed to get capabilities", "could not be read"))
NOISE = P + "something else entirely\n"

# the constants are the strings of DRIVER-PLAN N45 (6)
check([n for n, _ in tr.IOP_LOG_FAILS] == [n for n, _ in FAILS], "failure strings: %r" % [n for n, _ in tr.IOP_LOG_FAILS])
check(tr.IOP_LOG_OK == "AGDC IB successful", "success string: %r" % tr.IOP_LOG_OK)

# each failure alone: one FAIL naming it, its count and its first line; the INFO about QueueDepth follows
for needle, meaning in FAILS:
    log = WRAP % (NOISE + P + "%s (code 3)\n" % needle + P + "%s again\n" % needle)
    r = tr.iop_log_rows(log)
    f = [x for x in r if x[0] == "FAIL"]
    check(len(f) == 1 and r[0][1] == "IOPresentment log", "%s: one FAIL expected: %r" % (needle, r))
    check(f and needle in f[0][2] and "2 time(s)" in f[0][2] and "(code 3)" in f[0][2] and meaning in f[0][2],
          "%s: FAIL text: %r" % (needle, f))
    check(not [x for x in r if x[0] == "PASS"], "%s: no PASS expected: %r" % (needle, r))
    check(levels(r)[-1] == "INFO" and "QueueDepth" in r[-1][2] and "does not appear" in r[-1][2], "%s: QueueDepth row: %r" % (needle, r))
    check(not [x for x in r if "did not get as far" in x[2]], "%s: the 'not as far' INFO is for a log without any verdict: %r" % (needle, r))

# all four failures: four FAIL rows
r = tr.iop_log_rows(WRAP % "".join(P + n + "\n" for n, _ in FAILS))
check(levels(r).count("FAIL") == 4, "four failures: %r" % r)

# success alone: PASS, no FAIL
r = tr.iop_log_rows(WRAP % (NOISE + P + "AGDC IB successful\n" + P + "AGDC IB successful\n"))
check(levels(r)[:1] == ["PASS"] and "2 time(s)" in r[0][2] and "FAIL" not in levels(r) and "WARN" not in levels(r), "success: %r" % r)

# a failure and a success: WARN for the failure, PASS for the success, no FAIL
r = tr.iop_log_rows(WRAP % (P + "Failed to get capabilities\n" + P + "AGDC IB successful\n"))
check(levels(r)[:2] == ["WARN", "PASS"] and "FAIL" not in levels(r) and "AGDC IB successful" in r[0][2], "failure then success: %r" % r)

# neither: INFO that IOPresentment did not get as far
r = tr.iop_log_rows(WRAP % NOISE)
check(levels(r) == ["INFO", "INFO"] and "did not get as far" in r[0][2] and "1-line" in r[0][2], "neither: %r" % r)

# QueueDepth present: the capabilities came from the display-pipe class
r = tr.iop_log_rows(WRAP % (P + "AGDC IB successful\n" + P + "caps { QueueDepth = 3; }\n"))
check(levels(r) == ["PASS", "INFO"] and "appears 1 time(s)" in r[-1][2] and "display-pipe class" in r[-1][2], "QueueDepth: %r" % r)

# no log at all, an empty one, or the wrapper only: a WARN, never silence
for bad in (None, "", "\n", "$ sh -c log show ...\n[exit 0]\n", "$ sh -c log show ...\n[exit 1]\n"):
    r = tr.iop_log_rows(bad)
    check(len(r) == 1 and r[0][:2] == ("WARN", "IOPresentment log") and "24-log-coredisplay.txt" in r[0][2], "no log %r: %r" % (bad, r))

# a long line is cut in the row
r = tr.iop_log_rows(P + "Invalid end point size " + "x" * 1000 + "\n")
check(len(r[0][2]) < 700, "a long first line is cut: %d" % len(r[0][2]))


def archive(d, members):
    p = os.path.join(d, "rdna3-report-x.tar.gz")
    with tarfile.open(p, "w:gz") as t:
        for n, body in members.items():
            data = body.encode()
            ti = tarfile.TarInfo("rdna3-report/" + n)
            ti.size = len(data)
            t.addfile(ti, io.BytesIO(data))
    return p


# present_mode_lines carries the rows: a failing log, a successful one, no log
with tempfile.TemporaryDirectory() as d:
    for body, want in ((WRAP % (P + "device with no planes\n"), ["FAIL"]), (WRAP % (P + "AGDC IB successful\n"), ["PASS"]),
                       (None, ["WARN"])):
        p = archive(d, {"24-log-coredisplay.txt": body} if body is not None else {"00-system.txt": "x\n"})
        rows = [x for x in tr.present_mode_lines(p, "", "rdna3fb=1", host="") if x[1] == "IOPresentment log"]
        check([x[0] for x in rows][:1] == want, "present_mode_lines %r: %r" % (want, rows))

# collect.sh: the log filter selects every string the row looks for
col = open(os.path.join(ROOT, "tools", "macos", "collect.sh")).read()
line = re.search(r"^run 24-log-coredisplay\.txt .*$", col, re.M)
check(line is not None, "collect.sh has no 24-log-coredisplay.txt line")
for needle in [n for n, _ in tr.IOP_LOG_FAILS]:
    check(line is not None and needle in line.group(0), "collect.sh's 24-log-coredisplay.txt filter lacks '%s'" % needle)
check(line is not None and "agdc" in line.group(0), "collect.sh's filter lost the case-insensitive 'agdc' (AGDC IB successful)")

if fails:
    for f in fails:
        print("test_triage_iop_log: FAIL " + f)
    sys.exit(1)
print("test_triage_iop_log: ok")
