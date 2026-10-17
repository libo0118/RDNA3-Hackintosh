#!/bin/bash
# The per-frame trace (card run 2026-10-17b, common/rdna3_trace.h) through
# tools/rdna3_trace.py and tools/triage_reports.py: a synthetic trace per
# hypothesis (tests/trace_fixture.py) must be named as that hypothesis -
# H-B2 (a back that never receives a frame), H-COPY (the buffer on the
# panel rewritten in place), H-FREE (VRAM freed while scanned), a copy
# forward error (CRITICAL) - the A/B side by side must judge mode 1, and the
# coverage self-check must FAIL "trace incomplete" for a missing HUBP
# address (Q6), events lost in the A/B window (Q8) and a report of a kernel
# with the trace but no trace in it; the mode 1 counters' verdict and
# CoreDisplay's "copy forward failed" (CRITICAL) too.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf -- "$T"' EXIT
fail() { echo "test_triage_trace: FAIL $*"; exit 1; }
has() { grep -qF -- "$2" <<<"$1" || fail "$3: '$2' not in: $1"; }
hasnt() { ! grep -qF -- "$2" <<<"$1" || fail "$3: '$2' unexpected in: $1"; }
dec() { python3 -I "$ROOT/tests/trace_fixture.py" "$1" "$T/$1.bin" && python3 "$ROOT/tools/rdna3_trace.py" "$T/$1.bin"; }

out=$(dec b2)
has "$out" "FAIL     H-B2: head 0: the back 0x8b8f0000 was named by 40 swaps and read EMPTY at 40" b2
has "$out" "WindowServer's frames land in 0x8b100000" b2
has "$out" "PASS     Q3: 40 empty backs, 40 delayed re-samples" b2
hasnt "$out" "H-FREE" b2
hasnt "$out" "CF-ERROR" b2

out=$(dec copy)
has "$out" "FAIL     H-COPY: mode 0: 39 of 39 flipped buffers were rewritten" copy
hasnt "$out" "H-B2" copy

out=$(dec free)
has "$out" "WARN     H-FREE: 1 buffer(s) freed while a head scanned them" free
hasnt "$out" "FAIL     H-COPY" free

out=$(dec cferr)
has "$out" "CRITICAL CF-ERROR: 1 copy forward(s) answered an error" cferr
hasnt "$out" "H-COPY" cferr

out=$(dec ab)
has "$out" "PASS     A/B verdict: the copy (mode 1) showed no black frame" ab
has "$out" "mode 1 (A: mode 1)" ab
has "$out" "mode 0 (B: mode 0)" ab
has "$out" "PASS     Q9: 1 command submissions" ab
has "$out" "PASS     Q8: 2 stream reads, 0 events lost" ab

out=$(dec clean)
hasnt "$out" "FAIL" clean
hasnt "$out" "WARN" clean

out=$(dec nopanel)
has "$out" "FAIL     trace incomplete: Q6" nopanel
has "$out" "a core without rdna3DisplayScanAddr: 40" nopanel

# ECC review round 1: what makes a report come back empty or wrong is loud
out=$(dec v1)
has "$out" "PASS     A/B verdict" v1                      # version 1 headers still read
has "$out" "PASS     Q8: 2 stream reads" v1
out=$(dec version)
has "$out" "trace version 9 at byte 0 (this decoder reads 1-2)" version
has "$out" "FAIL     trace incomplete: Q1-Q8" version
out=$(dec truncated)
has "$out" "FAIL     trace incomplete: Q8" truncated
has "$out" "TRUNCATED: 124 of 126 record(s)" truncated
out=$(dec corrupt)
has "$out" "no R3TR header at byte 10096 (after 1 blob(s))" corrupt
has "$out" "FAIL     trace incomplete: Q8" corrupt
out=$(dec gap)
has "$out" "63 event(s) between two reads MISSING from the file" gap
out=$(dec inactive)
has "$out" "FAIL     A/B inconclusive: phase 2 had 0 swaps on head 0 (need 20)" inactive
hasnt "$out" "A/B verdict" inactive
out=$(dec mode0never)
has "$out" "FAIL     A/B inconclusive: mode 0 never applied in phase 2 (its swaps ran in mode 1: 40)" mode0never
hasnt "$out" "PASS     A/B verdict" mode0never
out=$(dec nomem)
has "$out" "7 event(s) NOT recorded (no memory for the accelerator's ring)" nomem
has "$out" "FAIL     trace incomplete: Q8" nomem
out=$(dec short)
has "$out" "FAIL     A/B inconclusive: phase 1 had 10 swaps on head 0 (need 20)" short
# ECC review round 2
out=$(dec nojoin)
has "$out" "WARN     join: head 0 back 0x8b100000 (40 swaps, 40 with pixels) has NO IOSurface id" nojoin
out=$(dec bootlost)   # the first stream read is the boot before the pump: its 'lost' is no loss of the run
has "$out" "PASS     Q8: 2 stream reads, 0 events lost between them; the boot before the pump: 500 events" bootlost
out=$(dec copyblack)
has "$out" "FAIL     COPY-BLACK: 10 copy(ies) of a back WITH pixels left the scanout buffer without any" copyblack
out=$(dec gpucap)
has "$out" "WARN     GPU: 1 GPU wait(s) ran out the 10 s cap" gpucap
out=$(dec mc)
hasnt "$out" "WARN     PANEL" mc                            # MC addresses are not compared with BAR ones
has "$out" "no VRAM base yet (MC addresses): 10" mc
out=$(python3 "$ROOT/tools/rdna3_trace.py" "$T/ab.bin" --timeline 5)
has "$out" "08:00:00." wall-clock                         # the timeline in the logs' clock (UTC)
# garbage never crashes the decoder: a text file where the binary was expected
printf 'debug.rdna3accel_trace: Format:S,rd3_trace Length:4096\n' > "$T/text.bin"
out=$(python3 "$ROOT/tools/rdna3_trace.py" "$T/text.bin")
has "$out" "NOT a trace (no R3TR header; it starts b'debug.rdna3accel_trace: ', 55 bytes" text   # the real first bytes
head -c 3000 /dev/urandom > "$T/random.bin"
python3 "$ROOT/tools/rdna3_trace.py" "$T/random.bin" >/dev/null || fail "random bytes crashed the decoder"
# a kernel log dump missing lines
out=$(python3 -I - "$ROOT/tools" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import rdna3_trace as r
t = r.Trace()
log = ("RDNA3Accelerator: trace: events 1..3 (asked) -- format: tr ...\n"
       "RDNA3Accelerator: tr 0 1 100 1 0 1 d 1a 3 5543 0 0 100 87040000 8b100000 8b100000 0 0\n"
       "RDNA3Accelerator: trace: dump done, 3 line(s), 0 event(s) already overwritten\n")
print(t.check_dumps(log), t.problems[0][2])
PY
)
has "$out" "2 the kernel log: 2 'tr' line(s) of 1 dump(s) missing" dumps
out=$(dec lost)
has "$out" "FAIL     trace incomplete: Q8 (2 stream reads, 25 events lost" lost

# a serial log alone: the kernel's "tr" lines (RD3_TRACE_LINE_FMT) decode to the same events
out=$(python3 -I - "$ROOT/tools" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import rdna3_trace as r
line = ("RDNA3Accelerator: tr 0 7 123456789 1 0 1 d 1a 3 5543 0 0 100 87040000 8b100000 8b100000 "
        "0 1716151413121110\n")
t = r.Trace()
print(t.add_lines("noise\n" + line + line))   # the same event twice: once
t.finish()
e = t.events[0]
print(len(t.events), e.name, e.seq, e.t, e.head, e.arg, hex(e.flags), hex(e.pb), e.bb.hex(), (e.v1 >> 4) & 0xf)
PY
)
has "$out" "1 SWAP 7 123456789 0 1 0xd 0x8b100000 1011121314151617 4" tr-lines

# the same through triage: a report whose kernel runs these sources
HASH=$(cd "$ROOT" && python3 -I -c 'import sys; sys.path.insert(0, "tools"); import triage_reports as t; print(t.fb_source_hash(t.ACCEL_HASH_FILES))')
mk() {   # mk <name> <trace kind or -> <tests counters or NOTESTS> [coredisplay line] [extra: stream|notread|cut]
    mkdir -p "$T/$1"
    [ "$2" = - ] || python3 -I "$ROOT/tests/trace_fixture.py" "$2" "$T/$1/trace.bin"
    python3 -I - "$T/$1" "$2" "$HASH" "$3" "${4:-}" "${5:-}" <<'PY'
import io, os, sys, tarfile
d, kind, h, counters, cd, extra = sys.argv[1:7]
members = {
    "01-boot-args.txt": "boot-args rdna3lvl=8 rdna3fb=1 rdna3accel=1 rdna3agdc=3\n",
    "03-kexts.txt": "RDNA3Accel\n",
    "11-ioreg-props.txt": "",
    "20-log-rdna3.txt": "RDNA3Accelerator: build %s\nRDNA3Accelerator: IOGraphicsAccelerator2::start -> 1\n" % h,
    "24-log-coredisplay.txt": cd + "\n",
}
base = os.path.join(d, "rdna3-report-x-20261017-120000")
with tarfile.open(base + ".tar.gz", "w:gz") as t:
    for n, body in members.items():
        data = body.encode()
        ti = tarfile.TarInfo("rdna3-report/" + n)
        ti.size = len(data)
        t.addfile(ti, io.BytesIO(data))
    def add(name, data):
        ti = tarfile.TarInfo("rdna3-report/" + name)
        ti.size = len(data)
        t.addfile(ti, io.BytesIO(data))
    if kind != "-":
        data = open(os.path.join(d, "trace.bin"), "rb").read()
        if extra in ("stream", "cut"):
            add("36-stream-trace-stream.bin", data)
            add("36-stream-trace-stream.bin.size", b"%d\n" % (len(data) + (500 if extra == "cut" else 0)))
        else:
            add("36-frame-trace.bin", data)
    if extra.startswith("plugin"):   # the Metal plugin's trace of WindowServer (plugin-join / -wrap / -old / -none)
        pk = extra[len("plugin-"):]
        if pk != "none":
            add("31-plugin-trace/WindowServer-158.trace", open(os.path.join(d, "plugin.trace"), "rb").read())
        add("31-plugin-trace/info.txt", (b"boottime 1792310000\n" + (b"== plugin frame traces collected: 0 (rejected: 1)\n"
                                         b"REJECT -rw-r----- 1 uid 501 size 4198400 WindowServer-9.trace: named WindowServer "
                                         b"but owned by uid 501\n" if pk == "none" else
                                         b"== plugin frame traces collected: 1 (rejected: 0)\n")))
    ft = "debug.rdna3accel_trace: 1000 bytes in 36-frame-trace.bin\n"
    if extra == "notread":
        ft = "debug.rdna3accel_trace: NOT read (not root, or a kext before card run 2026-10-17b)\n"
    add("36-frame-trace.txt", ft.encode())
if counters != "NOTESTS":
    dump = "".join("debug.rdna3accel_%s: %s\n" % tuple(l.split()) for l in counters.strip().splitlines()
                   if len(l.split()) == 2 and l.split()[0] != "AB")
    txt = "== A3 display counters after M6\n" + dump
    if "M5" in counters.split():
        txt = "M5: WindowServer LOADED the plugin; it is on the allow list\n" + txt
    if "LOOKS" in counters.split():   # rdna3_drminfo --scanout looks: ab1 (mode 1) left pipe 0 black, ab0 (mode 0) did not
        look = ("== rdna3_drminfo --scanout (look %s)\n2 pipe(s) lit\n"
                "pipe 0: LIT otg 0 2560x1440 pitch 10240 fmt 10 sw 0 dcc 0 address 0x1 in use 0x1\n"
                "  picture 320x180: %d%% not black, mean rgb 1 1 1, 0 bytes changed in 2 s\n"
                "pipe 1: LIT otg 1 1920x1080 pitch 7680 fmt 10 sw 0 dcc 0 address 0x2 in use 0x2\n"
                "  picture 320x180: 41%% not black, mean rgb 9 9 9, 10 bytes changed in 2 s\n")
        txt = look % ("ab1", 1) + look % ("ab0", 44) + txt
    if "LOOKSBAD" in counters.split():   # pipe 0 lit but its picture not readable in ab1; ab0 readable
        txt = ("== rdna3_drminfo --scanout (look ab1)\n1 pipe(s) lit\n"
               "pipe 0: LIT otg 0 2560x1440 pitch 10240 fmt 10 sw 0 dcc 1 address 0x1 in use 0x1\n"
               "  picture: not readable (0 of 180 rows; tiled, DCC or outside the BAR)\n"
               "== rdna3_drminfo --scanout (look ab0)\n1 pipe(s) lit\n"
               "pipe 0: LIT otg 0 2560x1440 pitch 10240 fmt 10 sw 0 dcc 0 address 0x1 in use 0x1\n"
               "  picture 320x180: 50% not black, mean rgb 9 9 9, 10 bytes changed in 2 s\n") + txt
    if "LOOKSMISS" in counters.split():
        txt = "== swap A/B phase 1: swap mode 1 (12:00:00)\n" + txt
    if "ABDIAG" in counters.split():   # autoreport's A/B diagnostics in the tests file
        txt = ("== swap A/B phase 1: swap mode 1 (12:00:00)\n"
               "== swap A/B: activity: Activity Monitor is NOT running - the phases may see no swaps (inconclusive)\n"
               "upload of trace-stream.bin FAILED (kept for the next boot)\n"
               "== plugin trace snapshot ab1: 0 WindowServer trace(s); 1 rejected\n"
               "== swap A/B: debug.rdna3accel_swap_mode is 1 after setting 0 (NOT APPLIED)\n"
               "== swap A/B done: swap mode 1 (exit 1)\n") + txt
    if "AB" in counters.split():   # the A/B's markers and two counter dumps (head 0: 30 swaps in the window)
        txt = ("debug.rdna3accel_swaps_h0: 100\n== swap A/B phase 1: swap mode 1\n== swap A/B phase 2: swap mode 0\n"
               "== swap A/B counters after mode 0\ndebug.rdna3accel_swaps_h0: %d\n== swap A/B phase 3\n" %
               (100 + int(counters.split("AB")[1].split()[0]))) + txt
    open(base + "-tests.txt", "w").write(txt)
PY
}
run() { python3 "$ROOT/tools/triage_reports.py" "$T/$1" 2>&1 || true; }

mk b2 b2 "swaps_h0 80
cf_calls 1
swap_mode 0"
out=$(run b2)
has "$out" "FAIL frame trace: H-B2: head 0: the back 0x8b8f0000" triage-b2
has "$out" "frame trace timeline" triage-b2

mk none - "swaps_h0 80
swap_mode 1"
out=$(run none)
has "$out" "FAIL frame trace: trace incomplete: Q1-Q11" triage-none

mk cd clean "swaps_h0 40
swap_mode 1
swaps_copy_h0 40
swaps_copyempty_h0 3
swaps_copyblack_h0 10
swaps_copyfail_h0 0" "2026-10-17 13:20:15.056 E  WindowServer[158:29f] [com.apple.CoreDisplay:default] [ERROR] - copy_forward_region: copy forward failed (0xe00002bc) ... disabling."
out=$(run cd)
has "$out" "CRITICAL copy forward: CoreDisplay disabled copy forward at 2026-10-17 13:20:15.056 (0xe00002bc)" triage-cd
has "$out" "the family failed before calling it" triage-cd
has "$out" "FAIL swap copy: head 0: 40 backs copied" triage-cd
has "$out" "10 black frames" triage-cd

# ECC review round 1 through triage
# no tests file: the A/B judged from the trace, said so
mk notests ab NOTESTS "" stream
out=$(run notests)
has "$out" "no tests file next to this report" triage-notests
has "$out" "PASS frame trace: A/B verdict" triage-notests
# the accelerator current, an autoreport run, but no A/B in the trace
mk noab clean "swaps_h0 40
swap_mode 1" "" stream
out=$(run noab)
has "$out" "FAIL frame trace: A/B did not run" triage-noab
# collect.sh could not read the trace sysctls
mk notread clean "swaps_h0 40" "" notread
out=$(run notread)
has "$out" "debug.rdna3accel_trace NOT read by collect.sh" triage-notread
# a stream shorter than the size autoreport.sh wrote next to it
mk cut ab "swaps_h0 80" "" cut
out=$(run cut)
has "$out" "a cut-off upload or copy" triage-cut
# the counters saw more swaps over the A/B than the trace holds
mk window ab "AB 500
swaps_h0 600" "" stream
out=$(run window)
has "$out" "FEWER than the counters: head 0: 80 in the trace, 500 counted over the A/B" triage-window
# garbage in the archive: a FAIL naming it, never a traceback
printf 'not a trace at all' > "$T/garbage.bin"
mk garbage - "swaps_h0 1"
python3 -I - "$T/garbage" "$T/garbage.bin" <<'PY'
import io, sys, tarfile
base = sys.argv[1] + "/rdna3-report-x-20261017-120000.tar.gz"
with tarfile.open(base) as t:
    members = [(m, t.extractfile(m).read()) for m in t.getmembers() if m.isfile()]
with tarfile.open(base, "w:gz") as t:
    for m, data in members:
        t.addfile(m, io.BytesIO(data))
    data = open(sys.argv[2], "rb").read()
    ti = tarfile.TarInfo("rdna3-report/36-frame-trace.bin")
    ti.size = len(data)
    t.addfile(ti, io.BytesIO(data))
PY
out=$(run garbage)
hasnt "$out" "Traceback" triage-garbage
has "$out" "NOT a trace (no R3TR header" triage-garbage

# ECC review round 3: a lit pipe whose picture is not readable is not a PASS; a missing A/B look is said;
# autoreport's A/B diagnostics reach the SUMMARY
mk looksbad ab "swaps_h0 80
LOOKSBAD" "" stream
out=$(run looksbad)
has "$out" "A/B screens            WARN NOT JUDGED: no lit pipe with a readable picture in both looks; pipe 0 picture NOT READABLE in look ab1 (not judged)" triage-looksbad
has "$out" "pipe 0 picture NOT READABLE (not judged)" triage-looksbad
has "$out" "A/B main display       WARN pipe 0" triage-looksbad
mk looksmiss ab "swaps_h0 80
LOOKSMISS" "" stream
out=$(run looksmiss)
has "$out" "WARN look ab1 and ab0 missing although the A/B ran" triage-looksmiss
mk abdiag ab "swaps_h0 80
ABDIAG" "" stream
run abdiag > "$T/abdiag.txt"
out=$(cat "$T/abdiag.txt")
has "$out" "swap A/B               FAIL exit 1; debug.rdna3accel_swap_mode is 1 after setting 0 (NOT APPLIED); Activity Monitor NOT running; 1 upload(s) FAILED (trace-stream.bin); plugin snapshot ab1: 0 trace(s), 1 rejected" triage-abdiag
python3 -I -B "$ROOT/tools/result_checklist.py" --no-color --width 220 "$T/abdiag.txt" | grep -qE "^FAIL +swap A/B " || fail "a failed A/B is not a FAIL in the checklist"

# the plugin's trace joined with the kernel's: each back's IOSurface and what the plugin did to it
python3 -I "$ROOT/tests/trace_fixture.py" plugin-join "$T/pj.trace"
mkp() {   # mkp <name> <trace kind> <plugin kind> <counters>
    mkdir -p "$T/$1"
    python3 -I "$ROOT/tests/trace_fixture.py" "plugin-$3" "$T/$1/plugin.trace"
    mk "$1" "$2" "$4" "" "plugin-$3"
}
mkp join ab join "swaps_h0 80
M5"
out=$(run join)
has "$out" "join: head 0 back 0x8b100000 = IOSurface 101 (80 swaps, 40 in mode 1 / 40 in mode 0 of the A/B, 80 with pixels, 0 EMPTY): plugin DRAWN 40, published 40 (dirty 40, copy-out 0)" triage-join
has "$out" "plugin trace WindowServer[158] (opened 2026-10-18 08:00:00 UTC)" triage-join
# ECC review round 3: the archive's final copy wrapped (only surface 103), the A/B snapshot next to the report
# still holds 101: merged, the join keeps the plugin's verdict
mkp merge ab late "swaps_h0 80
M5"
python3 -I "$ROOT/tests/trace_fixture.py" plugin-join "$T/merge/rdna3-report-x-20261017-120000-plugin-ab1-WindowServer-158.trace"
out=$(run merge)
has "$out" "IOSurface 101 (80 swaps, 40 in mode 1 / 40 in mode 0 of the A/B, 80 with pixels, 0 EMPTY): plugin DRAWN 40" triage-merge
has "$out" "(2 copies merged)" triage-merge
# backs with IOSurface ids but no plugin trace at all (and no M5 line): MISSING, the join has no verdict
mk nojoinplugin ab "swaps_h0 80" "" stream
out=$(run nojoinplugin)
has "$out" "WARN plugin trace MISSING (collect_plugin_trace.sh did not run (no 31-plugin-trace/ and no 37-plugin-trace-collect.txt)) - the join lines below carry no plugin verdict" triage-nojoinplugin
# a back the plugin never saw although its trace covers the A/B, with empty swaps: WARN
python3 -I "$ROOT/tests/trace_fixture.py" b2 "$T/b2j.bin"
out=$(python3 -I - "$ROOT/tools" "$T/b2j.bin" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import rdna3_trace as r
t = r.load([sys.argv[2]])
for lv, x in r.join_lines(t, {101: "DRAWN 40"}, True):
    print(lv, x)
PY
)
has "$out" "WARN join: head 0 back 0x8b8f0000 = IOSurface 102 (40 swaps, 0 with pixels, 40 EMPTY): plugin NEVER saw this IOSurface although its trace covers the A/B" join-never
has "$out" "INFO join: head 0 back 0x8b100000 = IOSurface 101" join-never
# a wrapped plugin ring that does not cover the A/B
mkp wrap ab wrap "swaps_h0 80
M5"
out=$(run wrap)
has "$out" "main ring WRAPPED (5000 events written" triage-wrap
has "$out" "is NOT covered" triage-wrap
# an old boot's trace in the directory: not judged
mkp old ab old "swaps_h0 80
M5"
out=$(run old)
has "$out" "OLD - opened 2025-10-18" triage-old
has "$out" "plugin trace MISSING" triage-old
# WindowServer loaded the plugin, no trace: MISSING with the collector's reason, the REJECT line
mkp none ab none "swaps_h0 80
M5"
out=$(run none)
has "$out" "WARN plugin trace MISSING (plugin frame traces collected: 0 (rejected: 1)) although WindowServer loaded the plugin" triage-none-plugin
has "$out" "plugin trace collector: REJECT -rw-r----- 1 uid 501" triage-none-plugin
hasnt "$out" "$T" triage-none-plugin                     # no archive path in the MISSING line
# the panels: a black pipe in the A/B's mode 1 look, CoreDisplay's copy forward switch-off in the SUMMARY
mk looks ab "swaps_h0 80
LOOKS" "2026-10-17 13:20:15.056 E  WindowServer[158:29f] [com.apple.CoreDisplay:default] [ERROR] - copy_forward_region: copy forward failed (0xe00002bc) ... disabling." stream
run looks > "$T/looks-sum.txt"
sum=$(cat "$T/looks-sum.txt")
has "$sum" "A/B screens" triage-looks
has "$sum" "FAIL mode 1 left pipe(s) 0 BLACK (pipe 0: mode 1 1%, mode 0 44%" triage-looks
has "$sum" "BLACK pipe(s) 0" triage-looks
has "$sum" "CRITICAL CoreDisplay disabled copy forward" triage-looks
python3 -I -B "$ROOT/tools/result_checklist.py" --no-color --width 200 "$T/looks-sum.txt" > "$T/looks-chk.txt" 2>&1 || true
grep -qE "^FAIL +copy forward +CRITICAL|^FAIL +copy forward" "$T/looks-chk.txt" || { cat "$T/looks-chk.txt"; fail "CRITICAL is not a FAIL in the checklist"; }
grep -qE "^FAIL +A/B screens" "$T/looks-chk.txt" || fail "a black A/B look is not a FAIL in the checklist"

echo "test_triage_trace: all checks passed"
