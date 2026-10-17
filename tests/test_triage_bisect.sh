#!/bin/bash
# tools/triage_reports.py + tools/triage_bisect.py on synthetic reports of the
# display bisect (card run 2026-10-17d, `rdna3vm step bisect`): a fake report
# with a 39-bisect.txt, a tests file and the looks' PPMs.
#  - breaks at one switch: fine with everything off, broken at the defaults and
#    from step 6 on (the last of the 'on' steps since the swap mode 1 became a
#    control step, 2026-10-17d) -> the table, the owner's first n, the first black look and
#    "BREAKS AT step 6"
#  - still broken with every switch off -> "not one of these switches"
#  - nothing looked broken -> INFO, no FAIL
#  - a picture that stood still between a step's looks (it moved in the others)
#  - missing data is a loud WARN: no look, no counters, a step never run, the
#    run cut, no end line, a switch that did not take, the defaults not the
#    compiled ones; a bisect run without bisect.txt is a FAIL; the uploaded
#    <report>-bisect.txt works as well as the archive's member
#  - the SUMMARY rows and the swap A/B is not expected in a bisect run
#  - bisect-agdc (rdna3bisect=4, card run 2026-10-17f): the steps are not
#    cumulative, so the verdict names the steps that LOOKED RIGHT (and the
#    broken ones, by swap_mode) instead of "breaks at"; a second table has the
#    per-step swap counters (deltas from the step's own "-- before" block:
#    copies, holds, iop flips, rewritten in place, torn) and the FRONT SURFACE
#    live-writer lines cut out of the kernel log per step (the serial log by
#    the trace_dump marker lines, the archive's log by its clock); missing log,
#    unplaceable steps and the sampler's 40-sample cap are loud WARNs
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
    (6, "on", "stale-back hold and swap mode 1: the defaults again", "1 1 1 1 1 0 1 0 0"),
    (7, "extra", "CONTROL mode 0", "1 1 1 1 0 0 1 0 0"), (8, "extra", "CONTROL trace_scan", "1 1 1 1 1 1 1 0 0"),
    (9, "extra", "CONTROL mode 1 landed", "1 1 1 1 1 0 1 1 0"), (10, "extra", "CONTROL mode 2", "1 1 1 1 2 0 1 0 0"),
    (11, "extra", "CONTROL a3ws", "1 1 1 1 1 0 1 0 1"), (12, "final", "the defaults again", "1 1 1 1 1 0 1 0 0")]
DEF = "1 1 1 1 1 0 1 0 0"   # swap_mode 1 (the copy) again since the write generations


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
            "console_user: tester", "autologin: none", "originals: " + " ".join("%s=%s" % kv for kv in zip(KNOBS, (("0 1 1 1 0 0 1 0 0" if nonstd else DEF)).split()))]
    for n, kind, name, vals in PLAN:
        head.append("plan: %d|%02d:%02d|%s|%s|x" % (n, n * 30 // 60, n * 30 % 60, kind, name))
    lines = list(head)
    prev = "0 1 1 1 0 0 1 0 0" if nonstd else DEF   # what the boot left
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


KNOBS4 = KNOBS + ["iop", "iop_flip", "flipbuf_present", "swap_gen"]
BASE4 = "1 1 1 1 1 0 1 0 0 1 0 0 1"
PLAN4 = [  # n, kind, name, values (the whole combination every time)
    (0, "ref", "reference: the defaults of the agdc profile", BASE4),
    (1, "var", "swap_mode 0 (the flip), swap_gen 1", "1 1 1 1 0 0 1 0 0 1 0 0 1"),
    (2, "var", "swap_mode 1, swap_gen 0", "1 1 1 1 1 0 1 0 0 1 0 0 0"),
    (3, "var", "swap_mode 0, swap_gen 0", "1 1 1 1 0 0 1 0 0 1 0 0 0"),
    (4, "var", "swap_mode 1, flipbuf_present 1", "1 1 1 1 1 0 1 0 0 1 0 1 1"),
    (5, "var", "swap_mode 0, flipbuf_present 1", "1 1 1 1 0 0 1 0 0 1 0 1 1"),
    (6, "var", "swap_mode 2 (every back copied)", "1 1 1 1 2 0 1 0 0 1 0 0 1"),
    (7, "var", "swap_mode 1, iop 0", "1 1 1 1 1 0 1 0 0 0 0 0 1"),
    (8, "var", "swap_mode 1, a3 0", "0 1 1 1 1 0 1 0 0 1 0 0 1"),
    (9, "var", "swap_mode 1, swap_latch 0", "1 1 0 1 1 0 1 0 0 1 0 0 1"),
    (10, "var", "swap_mode 1, swap_stale 0", "1 1 1 1 1 0 0 0 0 1 0 0 1"),
    (11, "var", "swap_mode 0, iop_flip toggled", "1 1 1 1 0 0 1 0 0 1 1 0 1"),
    (12, "final", "the defaults again", BASE4)]
FRONT_LINE = ("RDNA3Accelerator: FRONT SURFACE sample %d: sum 0x%08x %s nonzero 100/200 px(100,100)=0x1 px(960,540)=0x2 "
              "px(1800,1000)=0x3 1920x1080 pitch 7680")
MOVED = "*** MOVED -- A LIVE WRITER IS PAINTING THE SCANNED-OUT SURFACE ***"
FROZEN = "(UNCHANGED since last sample -- frozen, not proof of a picture)"


def ev0(n):
    return 10000 + 5000 * n


def build4(right=(), tty=True, nobefore=False):
    """bisect.txt of a bisect-agdc run: step 0 and the steps not in `right` look broken (pipe 1 black, the owner says n);
    the counters of step n are 1000*n cumulative at its start and +300 swaps h0 / +150 h1 / +10*n copies / +2*n holds /
    +5*n rewritten in place / +7 iop flips by its second look; 40 swaps pass between a step's look and the next start"""
    head = ["# rdna3 bisect v1", "variant: 4 (bisect-agdc)", "hold_s: 30", "mode: " + ("tty" if tty else "timer"), "started: 12:00:00",
            "console_user: tester", "autologin: none", "originals: " + " ".join("%s=%s" % kv for kv in zip(KNOBS4, BASE4.split()))]
    for n, kind, name, vals in PLAN4:
        head.append("plan: %d|%02d:%02d|%s|%s|x" % (n, n * 30 // 60, n * 30 % 60, kind, name))
    lines = list(head)
    for n, kind, name, vals in PLAN4:
        bad = n not in right
        base = 1000 * n
        chg = ", ".join("%s %s->%s" % (k, a, b) for k, a, b in zip(KNOBS4, BASE4.split(), vals.split()) if a != b) or "nothing"
        o = ["== step %d" % n, "kind: " + kind, "name: " + name, "changed: " + chg]
        if not nobefore:
            o += ["-- before", "debug.rdna3accel_swaps_h0: %d" % base, "debug.rdna3accel_swaps_h1: %d" % (base // 2),
                  "debug.rdna3accel_swaps_copy_h0: %d" % (base // 10), "debug.rdna3accel_swaps_stalehold_h0: 0",
                  "debug.rdna3accel_swaps_inplace_h0: %d" % (base // 20), "debug.rdna3accel_swaps_torn_h0: 0",
                  "debug.rdna3accel_iop_flips: 0", "debug.rdna3accel_iop_ok: %d" % base]
        o += ["stream_bytes_start: %d" % (n * 1000), "trace_events_start: %d" % ev0(n),
              "set: (nothing)", "readback: " + " ".join("debug.rdna3accel_%s=%s" % kv for kv in zip(KNOBS4, vals.split())),
              "at: 12:%02d:00" % n, "offset: 00:%02d" % (n * 30)]
        pcts = [45, 1 if bad else 30]
        mv = [9000, 0]
        o += look(n, "bisect%da" % n, pcts, mv) + look(n, "bisect%d" % n, pcts, mv)
        o.append("looks: bisect%da bisect%d" % (n, n))
        for p, pct in enumerate(pcts):
            for nm in ("bisect%da" % n, "bisect%d" % n):
                ppm("%s-scanout%s-pipe%d.ppm" % (stem, nm, p), (0, 0, 0) if pct <= 3 else (90, 90, 90),
                    noise=0 if (p == 1 or nm.endswith("a")) else 40)
        o += ["-- counters", "debug.rdna3accel_swaps_h0: %d" % (base + 300), "debug.rdna3accel_swaps_h1: %d" % (base // 2 + 150),
              "debug.rdna3accel_swaps_copy_h0: %d" % (base // 10 + 10 * n), "debug.rdna3accel_swaps_stalehold_h0: %d" % (2 * n),
              "debug.rdna3accel_swaps_inplace_h0: %d" % (base // 20 + 5 * n), "debug.rdna3accel_swaps_torn_h0: 0",
              "debug.rdna3accel_iop_flips: 7", "debug.rdna3accel_iop_ok: %d" % (base + 50)]
        o += ["stream_bytes: %d" % (n * 1000 + 900), "trace_events: %d" % (ev0(n) + 4000)]
        o.append("answer: " + (("n" if bad else "y") if tty else "-"))
        o.append("== end step %d" % n)
        lines += o
    lines += ["restored: " + BASE4, "finished: 12:07:00", "== end"]
    return "\n".join(lines) + "\n"


def serial4(front, cap_before=0, after=0, markers=True):
    """a serial log: per step its trace_dump marker line, then front[n] = (live, samples) FRONT SURFACE lines; the dump at the
    end of the run, and `after` more live writer lines behind it; cap_before sampler lines before the first marker"""
    out = ["RDNA3Accelerator: boot noise"]
    k = 0
    for i in range(cap_before):
        k += 1
        out.append(FRONT_LINE % (k, k, FROZEN))
    for n, kind, name, vals in PLAN4:
        if markers:
            out.append("RDNA3Accelerator: trace: events %d..%d (asked by sysctl debug.rdna3accel_trace_dump)" % (ev0(n) + 3, ev0(n) + 3))
        out.append("RDNA3Accelerator: tr 0 %d 1 2 0 0 0 0 0 0 0 0 0 0 0 0 0 0" % (ev0(n) + 3))
        live, smp = front.get(n, (0, 0))
        for j in range(smp):
            k += 1
            out.append(FRONT_LINE % (k, k, MOVED if j < live else FROZEN))
    end = ev0(len(PLAN4))   # after the last step
    out.append("RDNA3Accelerator: trace: events %d..%d (asked by sysctl debug.rdna3accel_trace_dump)" % (end - 599, end))
    for j in range(after):
        k += 1
        out.append(FRONT_LINE % (k, k, MOVED))
    return "\n".join(out) + "\n"


def clocklog4(front):
    """the archive's log show: the lines carry their date and time, no marker lines"""
    out = ["2026-10-17 11:59:00.100 Df  kernel[0:1] (RDNA3Accelerator) RDNA3Accelerator: boot noise"]
    for n, kind, name, vals in PLAN4:
        live, smp = front.get(n, (0, 0))
        for j in range(smp):
            out.append("2026-10-17 12:%02d:%02d.500 Df  kernel[0:1] (RDNA3Accelerator) " % (n, 5 + j) + FRONT_LINE % (j + 1, j + 1, MOVED if j < live else FROZEN))
    out.append("2026-10-17 12:20:00.000 Df  kernel[0:1] (RDNA3Accelerator) RDNA3Accelerator: after the run")
    return "\n".join(out) + "\n"


def write(text, archive=True, upload=False, runtests=True, args="rdna3bisect=1 rdna3vmauto=1", log="RDNA3: hello\n", serial=None):
    members = {"01-boot-args.txt": "boot-args\t%s\n" % args, "03-kexts.txt": "RDNA3Accel\n", "11-ioreg-props.txt": "",
               "20-log-rdna3.txt": log}
    if serial is not None:
        open(os.path.join(d, "serial-20261017-120000.log"), "w").write(serial)
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
elif scen.startswith("agdc"):
    A = "rdna3bisect=4 rdna3vmauto=1"
    FR = {0: (2, 3), 1: (0, 1), 6: (1, 2)}      # step: (live writer lines, sampler lines)
    if scen == "agdc":              # the reference and most steps broken; 3 and 4 look right; the serial log has the markers
        write(build4(right=(3, 4)), args=A, serial=serial4(FR, after=4))
    elif scen == "agdcclock":       # the archive's log show only: no markers, the lines' clock
        write(build4(right=(3, 4)), args=A, log=clocklog4(FR))
    elif scen == "agdccap":         # the sampler's 40 samples were used up before step 0
        write(build4(right=(3, 4)), args=A, serial=serial4({}, cap_before=40))
    elif scen == "agdcnolog":       # no log with a marker or a clock
        write(build4(right=(3, 4)), args=A)
    elif scen == "agdcnone":        # nothing looked right
        write(build4(right=()), args=A, serial=serial4(FR))
    elif scen == "agdcgood":        # nothing broke
        write(build4(right=tuple(range(13)), tty=False), args=A, serial=serial4({}))
    elif scen == "agdcrefok":       # the reference looked right, step 2 broken
        write(build4(right=tuple(n for n in range(13) if n != 2)), args=A, serial=serial4({}))
    elif scen == "agdcflaky":       # step 12 has the switches of step 0 and looks different
        write(build4(right=(12,)), args=A, serial=serial4({}))
    elif scen == "agdcnobefore":    # an older autoreport: no "-- before" block, the deltas come from the step before
        write(build4(right=(3,), nobefore=True), args=A, serial=serial4({}))
elif scen == "plain":    # an ordinary run: no bisect at all
    write("", archive=False, args="rdna3lvl=8 rdna3fb=1")
PY
}
run() { python3 -I "$ROOT/tools/triage_reports.py" "$1" 2>&1 || true; }

# ---- breaks at step 6 (the defaults broken, everything off fine)
mk "$T/a" breaks
o=$(run "$T/a")
grep -q 'display bisect (tools/triage_bisect.py' <<<"$o" || fail "no bisect table: $o"
grep -qE '^ +6 on' <<<"$o" || fail "step 6 not in the table: $o"
grep -qE '^ +0 ref .* n +45% +1% ' <<<"$o" || fail "step 0 row (owner n, pipe 1 1%): $(grep -E '^ +0 ' <<<"$o")"
grep -qE '^ +1 off .* y +45% +30% ' <<<"$o" || fail "step 1 row: $(grep -E '^ +1 ' <<<"$o")"
grep -qE '^ +2 on +a3 0->1 +y ' <<<"$o" || fail "step 2 row: $(grep -E '^ +2 ' <<<"$o")"
grep -qE '^ +6 on .* n +45% +1% .*<-- the owner marked it n; pipe 1 black' <<<"$o" || fail "step 6 should be flagged: $(grep -E '^ +6 ' <<<"$o")"
grep -q 'FAIL BISECT: BREAKS AT step 6 (swap_mode 0->1, swap_stale' <<<"$o" || fail "verdict: $(grep BISECT <<<"$o")"
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
grep -q 'swap_mode 0 (default 1)' <<<"$o" || fail "swap_mode 0 is not the default since the write generations: $(grep 'compiled' <<<"$o")"
mk "$T/m" missing
o=$(run "$T/m")
grep -q 'FAIL bisect run (rdna3bisect in the boot-args) but no bisect.txt' <<<"$o" || fail "missing bisect.txt: $o"
grep -q 'RESULT: something failed' <<<"$o" || fail "missing bisect.txt should fail the run"
mk "$T/p" plain
o=$(run "$T/p")
! grep -qi 'bisect' <<<"$o" || fail "an ordinary run mentions the bisect: $(grep -i bisect <<<"$o")"

# ---- bisect-agdc: the steps that looked right, the counters of each step, the FRONT SURFACE lines per step
mk "$T/g1" agdc
o=$(run "$T/g1")
grep -q 'display bisect (tools/triage_bisect.py' <<<"$o" || fail "agdc: no bisect table: $o"
[ "$(grep -cE '^ +[0-9]+ (ref|var|final) ' <<<"$o")" = 13 ] || fail "agdc: not 13 rows in the table: $(grep -E '^ +[0-9]+ (ref|var|final)' <<<"$o")"
grep -qE '^ +0 ref .* n +45% +1% ' <<<"$o" || fail "agdc: step 0 row: $(grep -E '^ +0 ref' <<<"$o")"
grep -qE '^ +3 var .*swap_mode 1->0, swap_gen 1->0 +y +45% +30% ' <<<"$o" || fail "agdc: step 3 row (looks right): $(grep -E '^ +3 var' <<<"$o")"
grep -qE '^ +5 var .*swap_mode 1->0, flipbuf_present 0->1 +n ' <<<"$o" || fail "agdc: step 5 row (changed against the boot, not the step before): $(grep -E '^ +5 var' <<<"$o")"
grep -q 'FAIL BISECT-AGDC: the reference (step 0, the agdc profile.s defaults) looked BROKEN' <<<"$o" || fail "agdc: verdict: $(grep BISECT <<<"$o")"
grep -q 'the steps that looked RIGHT: step 3 (swap_mode 0, swap_gen 0); step 4 (swap_mode 1, flipbuf_present 1)' <<<"$o" || fail "agdc: the right steps: $(grep 'looked RIGHT' <<<"$o")"
grep -q 'INFO BISECT-AGDC by swap_mode: mode 0: right \[3\], broken \[1, 5, 11\]; mode 1: right \[4\], broken \[0, 2, 7, 8, 9, 10, 12\]; mode 2: right none, broken \[6\]' <<<"$o" ||
    fail "agdc: by swap_mode: $(grep 'by swap_mode' <<<"$o")"
! grep -q 'BREAKS AT' <<<"$o" || fail "agdc: a cumulative verdict for a plan that is not cumulative"
grep -q 'bisect  *FAIL BISECT-AGDC: the reference' <<<"$o" || fail "agdc: SUMMARY row: $(sed -n '/^SUMMARY/,$p' <<<"$o" | grep -i bisect)"
# the counters: from the step's own "-- before" block (swaps h0 +300, not +340 from the step before), per step
grep -q 'swap counters per step' <<<"$o" || fail "agdc: no counter table"
grep -qE '^ +3 +0/1/1/1/1/0/0/0 +300 +150 +30 +6 +7 +15 +0 +50 ' <<<"$o" || fail "agdc: step 3 counters: $(grep -E '^ +3 +[0-9]/' <<<"$o")"
grep -qE '^ +10 +1/0/1/1/1/0/0/1 +300 +150 +100 +20 +7 +50 ' <<<"$o" || fail "agdc: step 10 counters: $(grep -E '^ +10 +[0-9]/' <<<"$o")"
grep -q 'the switch column: swap_mode/stale/iop/a3/lat/flip/fbp/gen' <<<"$o" || fail "agdc: the switch column is not explained"
# the trace slice of each step
grep -qE '^ +3 .* 25000\.\.29000 \(stream bytes 3000\.\.3900\)' <<<"$o" || fail "agdc: step 3 trace slice: $(grep -E '^ +3 +[0-9]/' <<<"$o")"
# FRONT SURFACE per step from the serial log, cut by the markers; the lines after the end dump are no step's
grep -qE '^ +0 +1/1/1/1/1/0/0/1 .* +2/3 ' <<<"$o" || fail "agdc: step 0 front: $(grep -E '^ +0 +[0-9]/' <<<"$o")"
grep -qE '^ +1 +[0-9/]+ .* +0/1 ' <<<"$o" || fail "agdc: step 1 front: $(grep -E '^ +1 +[0-9]/' <<<"$o")"
grep -qE '^ +6 +[0-9/]+ .* +1/2 ' <<<"$o" || fail "agdc: step 6 front: $(grep -E '^ +6 +[0-9]/' <<<"$o")"
grep -qE '^ +12 +[0-9/]+ .* +0/0 ' <<<"$o" || fail "agdc: step 12 front (the lines after the run's last dump are not its): $(grep -E '^ +12 +[0-9]/' <<<"$o")"
grep -q 'INFO FRONT SURFACE lines per step: serial-20261017-120000.log, cut by the trace_dump marker lines' <<<"$o" || fail "agdc: the log used is not said: $(grep 'FRONT SURFACE' <<<"$o")"
! grep -q 'FRONT SURFACE sample line' <<<"$o" || fail "agdc: a cap warning where the steps have samples: $(grep 'FRONT SURFACE' <<<"$o")"

# the archive's log show, no marker lines: cut by its clock
mk "$T/g2" agdcclock
o=$(run "$T/g2")
grep -q 'INFO FRONT SURFACE lines per step: 20-log-rdna3.txt, cut by the log.s clock' <<<"$o" || fail "agdc clock: $(grep 'FRONT SURFACE' <<<"$o")"
grep -qE '^ +0 +[0-9/]+ .* +2/3 ' <<<"$o" || fail "agdc clock: step 0 front: $(grep -E '^ +0 +[0-9]/' <<<"$o")"
grep -qE '^ +6 +[0-9/]+ .* +1/2 ' <<<"$o" || fail "agdc clock: step 6 front: $(grep -E '^ +6 +[0-9]/' <<<"$o")"

# the sampler's cap: samples exist, none inside a step
mk "$T/g3" agdccap
o=$(run "$T/g3")
grep -q 'WARN the log has 40 FRONT SURFACE sample line(s), none inside a step: the kext samples the front surface only 40 times per boot' <<<"$o" || fail "agdc cap: $(grep 'FRONT SURFACE' <<<"$o")"

# a log that cannot place a step
mk "$T/g4" agdcnolog
o=$(run "$T/g4")
grep -q 'WARN 20-log-rdna3.txt: could not place any step in the log' <<<"$o" || fail "agdc no log: $(grep 'FRONT SURFACE\|could not place' <<<"$o")"
grep -qE '^ +0 +[0-9/]+ .* +\? ' <<<"$o" || fail "agdc no log: the front column should say ?: $(grep -E '^ +0 +[0-9]/' <<<"$o")"

# no step looked right
mk "$T/g5" agdcnone
o=$(run "$T/g5")
grep -q 'FAIL BISECT-AGDC: the reference (step 0) looked BROKEN .* and NO step looked right' <<<"$o" || fail "agdc none right: $(grep BISECT <<<"$o")"

# nothing broke (timer mode: the looks decide)
mk "$T/g6" agdcgood
o=$(run "$T/g6")
grep -q 'INFO BISECT-AGDC: no step looked broken' <<<"$o" || fail "agdc good: $(grep BISECT <<<"$o")"
! grep -qE 'FAIL (BISECT|the first)' <<<"$o" || fail "agdc good run failed: $(grep FAIL <<<"$o" | head -3)"

# the reference looked right, one step broke
mk "$T/g7" agdcrefok
o=$(run "$T/g7")
grep -q 'WARN BISECT-AGDC: the reference (step 0) looked RIGHT .*these steps looked BROKEN: step 2 (swap_mode 1, swap_gen 0)' <<<"$o" || fail "agdc ref ok: $(grep BISECT <<<"$o")"

# step 12 = step 0 but not alike
mk "$T/g8" agdcflaky
o=$(run "$T/g8")
grep -q 'WARN BISECT-AGDC: step 12 has the same switches as step 0 but looked right there, broken here' <<<"$o" || fail "agdc flaky: $(grep BISECT <<<"$o")"

# an older autoreport without the "-- before" blocks: the deltas come from the step before (1000 swaps, the whole step before's span, not 300)
mk "$T/g9" agdcnobefore
o=$(run "$T/g9")
grep -qE '^ +3 +0/1/1/1/1/0/0/0 +1000 +500 ' <<<"$o" || fail "agdc no before: $(grep -E '^ +3 +[0-9]/' <<<"$o")"

# the boot-args of the run reach the group
python3 -I - "$ROOT" <<'PY' || fail "triage_reports.bisect_run_expected(rdna3bisect=4)"
import sys
sys.path.insert(0, sys.argv[1] + "/tools")
import triage_reports as t
assert t.bisect_run_expected("x", "", "rdna3bisect=4 rdna3vmauto=1")
assert t.bisect_run_expected("x", "", "rdna3bisect=1")
assert not t.bisect_run_expected("x", "", "rdna3bisect=3")
PY

echo "test_triage_bisect: ok"
