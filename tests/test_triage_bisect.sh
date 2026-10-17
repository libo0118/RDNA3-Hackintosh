#!/bin/bash
# tools/triage_reports.py + tools/triage_bisect.py on synthetic reports of the
# display bisect (card run 2026-10-17d, `rdna3vm step bisect`): a fake report
# with a 39-bisect.txt, a tests file and the looks' PPMs.
#  - breaks at one switch: fine with everything off, broken at the defaults and
#    from step 6 on -> the table, the owner's first n, the first black look and
#    "BREAKS AT step 6"
#  - still broken with every switch off -> "not one of these switches"
#  - nothing looked broken -> INFO, no FAIL
#  - a picture that stood still between a step's looks (it moved in the others)
#  - missing data is a loud WARN: no look, no counters, a step never run, the
#    run cut, no end line, a switch that did not take, the defaults not the
#    compiled ones; a bisect run without bisect.txt is a FAIL; the uploaded
#    <report>-bisect.txt works as well as the archive's member
#  - the SUMMARY rows and the swap A/B is not expected in a bisect run
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf -- "$T"' EXIT
fail() { echo "test_triage_bisect: FAIL $*"; exit 1; }

# mk <dir> <scenario>: one report in <dir>
mk() {
    python3 -I - "$1" "$2" <<'PY'
import io, os, sys, tarfile

d, scen = sys.argv[1], sys.argv[2]
os.makedirs(d, exist_ok=True)
stem = os.path.join(d, "rdna3-report-test-20261017-120000")
KNOBS = ["a3", "flicker", "swap_latch", "swap_torn", "swap_mode", "trace_scan", "swap_stale", "swap_landed", "a3ws"]
PLAN = [  # n, kind, name, values (None = unchanged)
    (0, "ref", "reference", None), (1, "off", "every new feature off", "0 0 0 0 0 0 0 0 0"),
    (2, "on", "A3", "1 0 0 0 0 0 0 0 0"), (3, "on", "flicker test", "1 1 0 0 0 0 0 0 0"),
    (4, "on", "swap_latch", "1 1 1 0 0 0 0 0 0"), (5, "on", "swap_torn", "1 1 1 1 0 0 0 0 0"),
    (6, "on", "swap mode 1", "1 1 1 1 1 0 0 0 0"), (7, "on", "trace_scan", "1 1 1 1 1 1 0 0 0"),
    (8, "on", "stale-back hold: the defaults again", "1 1 1 1 1 1 1 0 0"),
    (9, "extra", "CONTROL landed", "1 1 1 1 1 1 1 1 0"), (10, "extra", "CONTROL mode 2", "1 1 1 1 2 1 1 0 0"),
    (11, "extra", "CONTROL a3ws", "1 1 1 1 1 1 1 0 1"), (12, "final", "the defaults again", "1 1 1 1 1 1 1 0 0")]
DEF = "1 1 1 1 1 1 1 0 0"


def ppm(path, rgb, w=8, h=8, noise=0):
    px = bytearray(bytes(rgb) * (w * h))
    for i in range(noise):     # noise pixels: a changed region
        px[i * 3:i * 3 + 3] = bytes((255, 255, 255))
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h) + bytes(px))


def look(n, name, pcts, changed, ended=True):
    out = ["-- look %s (12:00:00)" % name]
    for p, pct in enumerate(pcts):
        if pct is None:
            continue
        out.append("pipe %d: LIT otg %d 1920x1080 pitch 1920 fmt 0 sw 0 dcc 0 address 0x1000 in use 0x1000" % (p, p))
        out.append("  picture 320x180: %d%% not black, mean rgb 9 9 9, %d bytes changed in 2 s" % (pct, changed[p]))
    out.append("%d pipe(s) lit" % sum(1 for x in pcts if x is not None))
    out.append("exit: 0")
    return out


def block(n, kind, name, vals, prev, answer, pcts, still=(), nolook=False, nocnt=False, failed=None, noend=False, cnt=0):
    changed = ", ".join("%s %s->%s" % (k, a, b) for k, a, b in zip(KNOBS, (prev or DEF).split(), (vals or prev or DEF).split()) if a != b) or "nothing"
    cur = vals or prev or DEF
    o = ["== step %d" % n, "kind: " + kind, "name: " + name, "changed: " + changed,
         "set: " + (" ".join("debug.rdna3accel_%s=%s" % kv for kv in zip(KNOBS, vals.split())) if vals else "(nothing)"),
         "readback: " + " ".join("debug.rdna3accel_%s=%s" % kv for kv in zip(KNOBS, cur.split()))]
    if failed:
        o.append("failed: " + failed)
    o += ["at: 12:%02d:00" % n, "offset: 00:%02d" % (n * 30)]
    if not nolook:
        mv = [0 if (p in still) else 9000 for p in (0, 1)]
        mv[1] = 0
        o += look(n, "bisect%da" % n, pcts, mv) + look(n, "bisect%d" % n, pcts, mv)
        o.append("looks: bisect%da bisect%d" % (n, n))
        for p, pct in enumerate(pcts):
            if pct is None:
                continue
            for nm, noise in (("bisect%da" % n, 0), ("bisect%d" % n, 0 if p in still or p == 1 else 40)):
                dd = os.path.join(d, "scan"); os.makedirs(dd, exist_ok=True)
                ppm("%s-scanout%s-pipe%d.ppm" % (stem, nm, p), (0, 0, 0) if pct <= 3 else (90, 90, 90), noise=noise)
    if not nocnt:
        o += ["-- counters", "debug.rdna3accel_swaps_h0: %d" % (cnt * 100), "debug.rdna3accel_swaps_h1: %d" % (cnt * 50),
              "debug.rdna3accel_swaps_stalehold_h1: %d" % (cnt * 3 if n >= 8 else 0),
              "debug.rdna3accel_swaps_stalehold_would_h1: 0", "debug.rdna3accel_swaps_copyempty_h0: 2"]
    o += ["stream_bytes: 1000", "trace_events: 10"]
    o.append("answer: " + answer)
    if not noend:
        o.append("== end step %d" % n)
    return o


def build(bad_from=None, ref_bad=True, off_bad=False, still_step=None, tty=True, cut_after=None, drop_look=None,
          drop_counters=None, fail_step=None, nonstd=False):
    head = ["# rdna3 bisect v1", "variant: 1 (bisect)", "hold_s: 30", "mode: " + ("tty" if tty else "timer"), "started: 12:00:00",
            "console_user: tester", "autologin: none", "originals: " + " ".join("%s=%s" % kv for kv in zip(KNOBS, (("0 1 1 1 1 1 1 0 0" if nonstd else DEF)).split()))]
    for n, kind, name, vals in PLAN:
        head.append("plan: %d|%02d:%02d|%s|%s|x" % (n, n * 30 // 60, n * 30 % 60, kind, name))
    lines = list(head)
    prev = "0 1 1 1 1 1 1 0 0" if nonstd else DEF   # what the boot left
    for n, kind, name, vals in PLAN:
        if cut_after is not None and n > cut_after:
            break
        bad = (n == 0 and ref_bad) or (n == 1 and off_bad) or (bad_from is not None and kind in ("on", "final") and n >= bad_from) \
            or (bad_from is not None and n == 0 and ref_bad)
        if kind == "extra":
            bad = False
        pcts = [45, 1 if bad else 30]
        ans = "-"
        if tty:
            ans = "n" if bad else "y"
            if kind == "extra":
                ans = "enter"
        still = (0,) if still_step == n else ()
        lines += block(n, kind, name, vals, prev, ans, pcts, still=still, nolook=(drop_look == n),
                       nocnt=(drop_counters == n), failed=("debug.rdna3accel_swap_latch:set-failed(Operation not permitted)" if fail_step == n else None),
                       noend=(cut_after is not None and n == cut_after), cnt=n + 1)
        prev = vals or prev
    if cut_after is None:
        lines += ["restored: " + DEF, "finished: 12:07:00", "== end"]
    return "\n".join(lines) + "\n"


def write(text, archive=True, upload=False, runtests=True, args="rdna3bisect=1 rdna3vmauto=1"):
    members = {"01-boot-args.txt": "boot-args\t%s\n" % args, "03-kexts.txt": "RDNA3Accel\n", "11-ioreg-props.txt": "",
               "20-log-rdna3.txt": "RDNA3: hello\n"}
    if text and archive:
        members["39-bisect.txt"] = text
    with tarfile.open(stem + ".tar.gz", "w:gz") as t:
        for n, body in members.items():
            data = body.encode()
            ti = tarfile.TarInfo("rdna3-report/" + n)
            ti.size = len(data)
            t.addfile(ti, io.BytesIO(data))
    if upload and text:
        open(stem + "-bisect.txt", "w").write(text)
    if runtests:
        open(stem + "-tests.txt", "w").write("boot-args: %s\n%s[exit 0]\n" % (
            args, "== bisect (the kernel's switches)\n" if "rdna3bisect" in args else ""))


if scen == "breaks":
    write(build(bad_from=6))
elif scen == "alloff":
    write(build(bad_from=None, off_bad=True))
elif scen == "good":
    write(build(ref_bad=False, tty=False))
elif scen == "still":
    write(build(ref_bad=False, still_step=4))
elif scen == "cut":
    write(build(bad_from=6, cut_after=7, drop_look=3, drop_counters=5))
elif scen == "failed":
    write(build(ref_bad=False, fail_step=4, nonstd=True))
elif scen == "upload":
    write(build(bad_from=6), archive=False, upload=True)
elif scen == "missing":
    write("", archive=False)
elif scen == "plain":    # an ordinary run: no bisect at all
    write("", archive=False, args="rdna3lvl=8 rdna3fb=1")
PY
}
run() { python3 -I "$ROOT/tools/triage_reports.py" "$1" 2>&1 || true; }

# ---- breaks at step 6 (the defaults broken, everything off fine)
mk "$T/a" breaks
o=$(run "$T/a")
grep -q 'display bisect (tools/triage_bisect.py' <<<"$o" || fail "no bisect table: $o"
grep -qE '^ +6 on +swap_mode 1->1|^ +6 on' <<<"$o" || fail "step 6 not in the table: $o"
grep -qE '^ +0 ref .* n +45% +1% ' <<<"$o" || fail "step 0 row (owner n, pipe 1 1%): $(grep -E '^ +0 ' <<<"$o")"
grep -qE '^ +1 off .* y +45% +30% ' <<<"$o" || fail "step 1 row: $(grep -E '^ +1 ' <<<"$o")"
grep -qE '^ +2 on +a3 0->1 +y ' <<<"$o" || fail "step 2 row: $(grep -E '^ +2 ' <<<"$o")"
grep -qE '^ +6 on .* n +45% +1% .*<-- the owner marked it n; pipe 1 black' <<<"$o" || fail "step 6 should be flagged: $(grep -E '^ +6 ' <<<"$o")"
grep -q 'FAIL BISECT: BREAKS AT step 6 (swap_mode' <<<"$o" || fail "verdict: $(grep BISECT <<<"$o")"
grep -q 'FAIL the first step the owner marked n: step 0' <<<"$o" || fail "first n: $(grep 'first step' <<<"$o")"
grep -q 'FAIL the first step whose looks black: step 0 .*pipe 1 black' <<<"$o" || fail "first black: $(grep 'first step' <<<"$o")"
grep -qE 'moved|move/still' <<<"$o" || fail "no moved column: $o"
grep -qE '^ +2 on .* 100/50|[0-9]+/[0-9]+, 0, 0, 0' <<<"$o" || fail "no counter deltas: $(grep -E '^ +2 ' <<<"$o")"
grep -q 'bisect  *FAIL BISECT: BREAKS AT step 6' <<<"$o" || fail "SUMMARY row: $(sed -n '/^SUMMARY/,$p' <<<"$o" | grep -i bisect)"
! grep -q 'A/B did not run' <<<"$o" || fail "the A/B is not expected in a bisect run"
grep -q 'RESULT: something failed' <<<"$o" || fail "a broken bisect is a failed run"
# the control steps' looks are fine in this scenario; a bad control is an INFO only
# ---- the uploaded <report>-bisect.txt alone does as well
mk "$T/u" upload
o=$(run "$T/u")
grep -q 'FAIL BISECT: BREAKS AT step 6' <<<"$o" || fail "the uploaded bisect.txt: $o"

# ---- still broken with everything off
mk "$T/b" alloff
o=$(run "$T/b")
grep -q 'FAIL BISECT: still broken with every switchable feature off' <<<"$o" || fail "all-off verdict: $(grep BISECT <<<"$o")"
grep -q 'not one of these switches' <<<"$o" || fail "all-off text"

# ---- nothing broke
mk "$T/g" good
o=$(run "$T/g")
grep -q 'INFO BISECT: no step looked broken' <<<"$o" || fail "good verdict: $(grep BISECT <<<"$o")"
! grep -qE 'FAIL (BISECT|the first)' <<<"$o" || fail "a good run failed: $(grep -E 'FAIL' <<<"$o" | head -3)"
grep -q 'INFO no owner answers (timer mode' <<<"$o" || fail "timer mode not said"

# ---- a picture that stood still in one step although it moved in the others
mk "$T/s" still
o=$(run "$T/s")
grep -q 'WARN the first step whose looks stood still: step 4 .*pipe 0 stood still' <<<"$o" || fail "still: $(grep -i 'still' <<<"$o" | head -3)"

# ---- missing data is loud
mk "$T/c" cut
o=$(run "$T/c")
grep -q 'WARN step 3: no look' <<<"$o" || fail "no look not loud: $o"
grep -q 'WARN step 5: no counters' <<<"$o" || fail "no counters not loud"
grep -q 'WARN step 7 has no end line' <<<"$o" || fail "cut step not said"
grep -qE 'WARN step\(s\) 8, 9, 10, 11, 12 never ran' <<<"$o" || fail "missing steps not said: $(grep 'never ran' <<<"$o")"
grep -q 'bisect  *WARN .*UNFINISHED' <<<"$o" || fail "SUMMARY should say unfinished: $(sed -n '/^SUMMARY/,$p' <<<"$o" | grep -i bisect)"
mk "$T/f" failed
o=$(run "$T/f")
grep -q 'FAIL step 4: a switch did not take: debug.rdna3accel_swap_latch:set-failed' <<<"$o" || fail "failed switch: $(grep 'step 4' <<<"$o")"
grep -q 'WARN step 0 is not the compiled defaults: a3 0 (default 1)' <<<"$o" || fail "non-default originals: $(grep 'compiled' <<<"$o")"
mk "$T/m" missing
o=$(run "$T/m")
grep -q 'FAIL bisect run (rdna3bisect in the boot-args) but no bisect.txt' <<<"$o" || fail "missing bisect.txt: $o"
grep -q 'RESULT: something failed' <<<"$o" || fail "missing bisect.txt should fail the run"
mk "$T/p" plain
o=$(run "$T/p")
! grep -qi 'bisect' <<<"$o" || fail "an ordinary run mentions the bisect: $(grep -i bisect <<<"$o")"

echo "test_triage_bisect: ok"
