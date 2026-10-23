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
# card run 2026-10-17c: every swap of the empty back skipped while the other back had nothing new: no frame lost
has "$out" "WARN     H-B2: head 0: the back 0x8b8f0000 was named by 40 swaps and read EMPTY at 40" b2
has "$out" "WindowServer's frames land in 0x8b100000" b2
has "$out" "but no frame lost: all 40 of its swaps were held/skipped" b2
has "$out" "PASS     Q3: 40 empty backs, 40 delayed re-samples" b2
hasnt "$out" "H-FREE" b2
hasnt "$out" "CF-ERROR" b2

# card run 2026-10-18b: mode 0 swaps to the buffer already on the panel, not flipped again (SWAP target 4)
python3 -I "$ROOT/tests/trace_fixture.py" samebuf "$T/samebuf.bin"
out=$(python3 "$ROOT/tools/rdna3_trace.py" "$T/samebuf.bin" --timeline 400)
has "$out" "mode 0 flip: back 0x8b100000 (pixels, 0708090a0b0c0d0e) front 0x87040000 (empty) -> panel 0x8b100000 target same buffer, not flipped copy 0 us wait 0 us" samebuf
has "$out" "-> panel 0x8b100000 target - copy 0 us wait 14800 us" samebuf
has "$out" "39 same buffer (not flipped), waits 1 (avg 14800 us)" samebuf

# mode 1 held the empty back while the other back had a new frame: frames held back (FAIL), counted in the A/B line
out=$(dec b2lost)
has "$out" "FAIL     H-B2: head 0: the back 0x8b8f0000 was named by 40 swaps and read EMPTY at 40" b2lost
has "$out" "40 held/skipped, 40 of them with a new frame in another buffer, 0 shown black" b2lost
has "$out" "40 held/skipped (40 with a new frame elsewhere)" b2lost
# debug.rdna3accel_swap_landed=0: the same holds, the buffer found but not presented - FAIL names it
python3 -I "$ROOT/tests/trace_fixture.py" b2would "$T/b2would.bin"
out=$(python3 "$ROOT/tools/rdna3_trace.py" "$T/b2would.bin" --timeline 400)
has "$out" "FAIL     H-B2: head 0: the back 0x8b8f0000" b2would
has "$out" "the landed buffer found but NOT presented at 40 (debug.rdna3accel_swap_landed=0" b2would
has "$out" "hold: back 0x8b8f0000 (empty, 0000000000000000) front 0x87040000 (empty) would present (swap_landed 0) the landed buffer 0x8b100000" b2would
has "$out" "40 landed buffers NOT presented (swap_landed 0)" b2would
# mode 1 presented the landed buffer: H-B2 handled (INFO), the timeline names the buffer
python3 -I "$ROOT/tests/trace_fixture.py" b2landed "$T/b2landed.bin"
out=$(python3 "$ROOT/tools/rdna3_trace.py" "$T/b2landed.bin" --timeline 400)
has "$out" "INFO     H-B2: head 0: the back 0x8b8f0000 was named by 40 swaps" b2landed
has "$out" "handled: landed buffer presented 40 times" b2landed
has "$out" "copy: back 0x8b8f0000 (empty, 0000000000000000) front 0x87040000 (empty) presented the landed buffer 0x8b100000" b2landed
has "$out" "80 shown (40 landed),    0 held/skipped" b2landed
hasnt "$out" "FAIL     H-B2" b2landed
has "$out" "0 BLACK frames" b2landed

# card run 2026-10-17c: head 1's second swap of a frame named a back WindowServer did not redraw. The old
# kext copied its old frame over every new one (H-STALE FAIL, A-B-A on the scanout); the stale-back hold keeps
# the newer frame (INFO), the timeline names the swap that showed it
python3 -I "$ROOT/tests/trace_fixture.py" stale "$T/stale.bin"
out=$(python3 "$ROOT/tools/rdna3_trace.py" "$T/stale.bin" --timeline 400)
has "$out" "FAIL     H-STALE: head 1: 61 mode 1/2 copies decoded, 30 stale copied, 0 held, 30 A-B-A on the scanout samples - stale backs COPIED (0x8b8f0000 30x; first swap #3, shown before at #1)" stale
has "$out" "no stale hold in the trace: a kext before the stale-back hold" stale
has "$out" "WARN     A-B-A: head 1: 30 A-B-A patterns on the scanout samples" stale
has "$out" "its PASS does not cover this" stale
hasnt "$out" "H-STALE: head 0" stale
hasnt "$out" "COPY-BLACK" stale
python3 -I "$ROOT/tests/trace_fixture.py" stalehold "$T/stalehold.bin"
out=$(python3 "$ROOT/tools/rdna3_trace.py" "$T/stalehold.bin" --timeline 400)
has "$out" "INFO     H-STALE: head 1: 31 mode 1/2 copies decoded, 0 stale copied, 30 held, 0 A-B-A on the scanout samples" stalehold
has "$out" "hold: back 0x8b8f0000 (pixels, 0708090a0b0c0d0e) front 0x87040000 (empty) held: stale back (shown at #1)" stalehold
has "$out" "30 held/skipped (0 with a new frame elsewhere), 30 stale backs held" stalehold
hasnt "$out" "FAIL     H-STALE" stalehold
hasnt "$out" "A-B-A: head" stalehold
hasnt "$out" "may look frozen" stalehold
# some held, some copied: FAIL names the copies, the kext has the hold
python3 -I "$ROOT/tests/trace_fixture.py" stalemixed "$T/stalemixed.bin"
out=$(python3 "$ROOT/tools/rdna3_trace.py" "$T/stalemixed.bin")
has "$out" "FAIL     H-STALE: head 1: 46 mode 1/2 copies decoded, 15 stale copied, 15 held" stalemixed
has "$out" "the kext has the hold (its holds are in the trace)" stalemixed
hasnt "$out" "a kext before the stale-back hold" stalemixed
# swap_stale=0: the copies were on purpose (SWAP v1 bit 24): WARN, the timeline says so
python3 -I "$ROOT/tests/trace_fixture.py" stalewould "$T/stalewould.bin"
out=$(python3 "$ROOT/tools/rdna3_trace.py" "$T/stalewould.bin" --timeline 400)
has "$out" "WARN     H-STALE: head 1: 61 mode 1/2 copies decoded, 30 stale copied, 0 held, 30 A-B-A on the scanout samples - copied on purpose (debug.rdna3accel_swap_stale=0)" stalewould
has "$out" "stale: would hold (swap_stale 0)" stalewould
hasnt "$out" "FAIL     H-STALE" stalewould
# the 20 ms window: 25 ms after B1's copy is not stale, 19 ms is
python3 -I "$ROOT/tests/trace_fixture.py" stale20ms "$T/stale20ms.bin"
out=$(python3 "$ROOT/tools/rdna3_trace.py" "$T/stale20ms.bin")
has "$out" "15 stale copied, 0 held" stale20ms
has "$out" "first swap #33, shown before at #31" stale20ms
# the kernel's resets (GONE, a swap mode change, D reallocated) forget the records: nothing stale
python3 -I "$ROOT/tests/trace_fixture.py" stalereset "$T/stalereset.bin"
out=$(python3 "$ROOT/tools/rdna3_trace.py" "$T/stalereset.bin")
has "$out" "INFO     H-STALE: head 1: 61 mode 1/2 copies decoded, 0 stale copied, 0 held" stalereset
# a held back while another buffer had a new frame, or after the CPU wrote it: a real frame held (FAIL)
python3 -I "$ROOT/tests/trace_fixture.py" stalereal "$T/stalereal.bin"
out=$(python3 "$ROOT/tools/rdna3_trace.py" "$T/stalereal.bin")
has "$out" "FAIL     H-STALE: head 1: held a REAL frame at 30 stale hold(s) (first swap #3: a new frame in 0x8b100000 at that swap)" stalereal
python3 -I - "$ROOT" "$T/stalereal.bin" <<'PY'
import sys
sys.path.insert(0, sys.argv[1] + "/tools")
import rdna3_trace as r
d = r.stale_backs(r.load([sys.argv[2]]))[1]
whys = [w for _, w in d["real"]]
assert sum("aperture write" in w for w in whys) == 15 and sum("a new frame" in w for w in whys) == 15, whys
PY
# more than two stale holds per copy on a head: the panel may look frozen (WARN)
python3 -I - "$ROOT" "$T/frozen.bin" <<'PY'
import sys
sys.path.insert(0, sys.argv[1] + "/tests")
sys.path.insert(0, sys.argv[1] + "/tools")
import trace_fixture as f
import rdna3_trace as r
w = f.W()
f.cf(w, 1, f.B2)
f.swap(w, 1, 1, 4, f.B2, True, head=1, pc=f.D, flags=96, target=2, bandk=200)
f.cf(w, 1, f.B1)
f.swap(w, 2, 1, 4, f.B1, True, head=1, pc=f.HOME, flags=96, target=1, bandk=3)
for n in range(3, 9):
    f.swap(w, n, 1, 5, f.B2, True, head=1, pc=f.HOME, stale=1)
open(sys.argv[2], "wb").write(w.blob())
out = r.h_stale(r.load([sys.argv[2]]))
assert any(lv == "WARN" and "6 stale holds against 2 copies" in t for lv, n, t in out), out
PY
# every other fixture's backs are drawn or empty: never a stale FAIL
out=$(dec b2landed)
hasnt "$out" "FAIL     H-STALE" b2landed-stale
has "$out" "INFO     H-STALE: head 0:" b2landed-stale

out=$(dec copy)
has "$out" "WARN     H-COPY: mode 0: 39 of 39 flipped buffers were rewritten" copy   # a WARN since 2026-10-17d (mode 0 is the default)
hasnt "$out" "FAIL     H-COPY" copy
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
    "01-boot-args.txt": "boot-args rdna3lvl=8 rdna3fb=1 rdna3accel=1 rdna3agdc=3 %s\n" % os.environ.get("MK_ARGS", ""),
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
has "$out" "WARN frame trace: H-B2: head 0: the back 0x8b8f0000" triage-b2
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
# card run 2026-10-17c: the landed buffer's counter (swaps_copylanded_h<N>): H-B2 handled, in the checklist too
mk landed clean "swaps_h0 40
swap_mode 1
swaps_copy_h0 40
swaps_copylanded_h0 12
swaps_copyempty_h0 3"
out=$(run landed)
has "$out" "40 backs copied" triage-landed
has "$out" "12 of them the landed buffer instead of an empty / stale back, 3 empty backs kept off a picture" triage-landed
has "$out" "INFO swap copy: head 0: H-B2 handled: landed buffer presented 12 times" triage-landed
has "$out" "swap copy              INFO head 0: H-B2 handled: landed buffer presented 12 times" triage-landed-summary
mk would clean "swaps_h0 40
swap_mode 1
swap_landed 0
swaps_copy_h0 40
swaps_copylanded_would_h0 7
swaps_copyempty_h0 7"
out=$(run would)
has "$out" "WARN swap copy: head 0: the landed buffer WOULD have been presented 7 times (swap_landed 0" triage-would
# card run 2026-10-17c: the stale back - the counters (swaps_stalehold_h<N>, the reasons) and the trace's
# verdict, in the report's lines and the SUMMARY
mk stalehold stalehold "swaps_h1 31
swap_mode 1
swap_landed 0
swap_stale 1
swaps_copy_h1 31
swaps_copylanded_h1 0
swaps_stalehold_h1 30
swaps_stalecheck_norecord_h1 1
swaps_stalecheck_sameshown_h1 2
swaps_staleevict_h1 0
swaps_stalehold_miss_h1 4
swaps_stalenocf_h1 5"
out=$(run stalehold)
has "$out" "0 of them the landed buffer instead of an empty / stale back, 0 empty backs kept off a picture, 30 stale backs held (not redrawn since shown)" triage-stalehold
has "$out" "INFO swap copy: head 1: H-STALE handled: 30 stale backs held" triage-stalehold
has "$out" "INFO swap copy: head 1: stale back (swap_stale 1): kernel: 30 held, 0 would (swap_stale 0); not held: norecord 1, changed 0, cf 0, sameshown 2, nopanel 0, nohead 0; 0 evicted (8-slot table), 4 second-grid misses, 5 changed without copy forward" triage-stalehold
has "$out" "swap copy              INFO head 1: H-STALE handled: 30 stale backs held" triage-stalehold-summary
has "$out" "frame trace            INFO H-STALE: head 1: 31 mode 1/2 copies decoded, 0 stale copied, 30 held" triage-stalehold-summary
has "$out" "; kernel: 30 held" triage-stalehold-kern
# the stale hold's counters are all there (the write generations' missing ones are gennone's test, ECC review round 1)
hasnt "$out" "swap copy: counter missing" triage-stalehold
hasnt "$out" "swap copy: head 1: counter missing" triage-stalehold
hasnt "$out" "stale holds, the trace holds" triage-stalehold
# the kext has the hold (counters) but copied some: the reasons named, never "a kext before the hold"; the
# counter and the trace's holds apart (20 vs 15, nothing lost): WARN
mk stalemixed stalemixed "swaps_h1 61
swap_mode 1
swap_landed 0
swaps_copy_h1 46
swaps_copylanded_h1 0
swaps_stalehold_h1 20
swaps_staleevict_h1 15"
out=$(run stalemixed)
has "$out" "FAIL frame trace: H-STALE: head 1: 46 mode 1/2 copies decoded, 15 stale copied, 15 held" triage-stalemixed
hasnt "$out" "a kext before the stale-back hold" triage-stalemixed
has "$out" "WARN frame trace: H-STALE: head 1: the kernel counted 20 stale holds, the trace holds 15 (0 events lost)" triage-stalemixed
python3 -I - "$ROOT" "$T/stalemixed.bin" <<'PY'
import sys
sys.path.insert(0, sys.argv[1] + "/tools")
import rdna3_trace as r
c = {"swaps_stalehold_h1": 15, "swaps_staleevict_h1": 15, "swaps_stalecheck_norecord_h1": 15}
lines = [t for lv, n, t in r.h_stale(r.load([sys.argv[2]]), {"counters": c}) if n == "H-STALE"]
assert any("15 evicted (8-slot table)" in t and "norecord 15" in t for t in lines), lines
assert not any("the kernel counted" in t for t in lines), lines
PY
mk stale stale "swaps_h1 61
swap_mode 1
swaps_copy_h1 61"
out=$(run stale)
has "$out" "FAIL frame trace: H-STALE: head 1: 61 mode 1/2 copies decoded, 30 stale copied" triage-stale
has "$out" "frame trace            FAIL H-STALE: head 1: 61 mode 1/2 copies decoded, 30 stale copied" triage-stale-summary
has "$out" "frame trace            WARN A-B-A: head 1: 30 A-B-A patterns" triage-stale-summary
# an older kext: the counters missing are said, never printed as zeros
has "$out" "WARN swap copy: counter missing: swap_landed (kext older than 2026-10-17c)" triage-stale-old
has "$out" "WARN swap copy: head 1: counter missing: swaps_stalehold_h1, swaps_copylanded_h1 (kext older than 2026-10-17c)" triage-stale-old
has "$out" "61 backs copied (0 MiB, avg 0 us, max 0 us), ? of them the landed buffer" triage-stale-old
has "$out" "? stale backs held" triage-stale-old
# the hold counter without swaps_copy (a partial dump): WARN; only stale holds on a head still get its lines
mk stalenocopy - "swaps_h1 31
swap_mode 1
swap_landed 0
swaps_stalehold_h1 30
swaps_copy_h0 10
swaps_copylanded_h0 0
swaps_stalehold_h0 0"
out=$(run stalenocopy)
has "$out" "WARN swap copy: head 1: counter missing: swaps_copy_h1 although swaps_stalehold_h1 is 30" triage-stalenocopy
# the counters without a trace: H-STALE not judged, said so
has "$out" "WARN frame trace: H-STALE not judged: no trace in the report (swaps_stalehold_h<N> counters only)" triage-stalenotrace
mk staleonly - "swaps_h1 31
swap_mode 1
swap_landed 1
swaps_copy_h1 0
swaps_copylanded_h1 0
swaps_stalehold_h1 30"
out=$(run staleonly)
has "$out" "INFO swap copy: head 1: H-STALE handled: 30 stale backs held" triage-staleonly

# ECC review round 1 through triage
# no tests file: the A/B judged from the trace, said so
mk notests ab NOTESTS "" stream
out=$(run notests)
has "$out" "no tests file next to this report" triage-notests
has "$out" "PASS frame trace: A/B verdict" triage-notests
# the accelerator current, an autoreport run, but no A/B in the trace
MK_ARGS="rdna3swapab=1" mk noab clean "swaps_h0 40
swap_mode 1" "" stream
out=$(run noab)
has "$out" "FAIL frame trace: A/B did not run" triage-noab
# card run 2026-10-17d: the A/B is opt-in; a normal step (no rdna3swapab=1) is not failed for not having one
mk noabasked clean "swaps_h0 40
swap_mode 0" "" stream
out=$(run noabasked)
hasnt "$out" "A/B did not run" triage-noabasked
has "$out" "INFO frame trace: A/B not asked" triage-noabasked
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

# card run 2026-10-17d: the write generations - a back WindowServer never wrote held each frame (INFO), a held
# back the trace saw written (FAIL), a copy forward burst with no swap and the flip-buffer / surface hooks
out=$(dec gen)
has "$out" "INFO     H-GEN: head 0: 60 swaps judged by the write generations: 30 presented (newer 30, first after a reset 0, changed with no write seen 0, aged 0, after an eviction 0), 30 held (the panel's 0, older 0, never written 30), 0 the surface's newer buffer instead" gen
hasnt "$out" "FAIL     H-GEN" gen
hasnt "$out" "may look frozen" gen
python3 -I "$ROOT/tests/trace_fixture.py" gen "$T/gen.bin"
out=$(python3 "$ROOT/tools/rdna3_trace.py" "$T/gen.bin" --timeline 400)
has "$out" "hold: back 0x8b8f0000 (pixels, a4a5a6a7a8a9aaab) front 0x87040000 (empty) gen 0: never written HELD: not written since the head's frame (head's generation 60)" gen-timeline
has "$out" "copy: back 0x8b100000 (pixels, 9d9e9fa0a1a2a3a4) front 0x87040000 (empty) gen 60: newer" gen-timeline
# ECC review round 1: a held back whose band bytes changed since its presentation (FAIL)
out=$(dec genchanged)
has "$out" "FAIL     H-GEN: head 0: held a CHANGED back 30 time(s) (first swap #2, back 0x8b8f0000)" genchanged
out=$(dec genreal)
has "$out" "FAIL     H-GEN: head 0: 15 hold(s) of a back the trace shows WRITTEN since the head's last frame (first swap #4, back 0x8b8f0000)" genreal
python3 -I "$ROOT/tests/trace_fixture.py" flippath "$T/flippath.bin"
out=$(python3 "$ROOT/tools/rdna3_trace.py" "$T/flippath.bin" --timeline 400)
has "$out" "H-FLIPPATH: head 0: 6 of 40 copy forwards with no swap of their surface within 50 ms, 1 burst(s) of 3 or more (the longest: 5 copy forwards" flippath
has "$out" "H-FLIPPATH: head 0: flip-buffer calls submitFlipBuffer 3; traced 3" flippath
has "$out" "H-FLIPPATH: surface hooks traced: pickPresentType 1, isBackBufferReady 2; isBackBufferReady said NOT ready 1 time(s)" flippath
has "$out" "FLIPBUF h0 submitFlipBuffer(1) #1 res 0x8b100000 / 0x87040000 -> traced panel 0x8d000000" flippath
has "$out" "HOOK h0 isBackBufferReady(1, 0) -> 0x1 (call 1)" flippath
# 17d-like bursts with no flip-buffer call at all: WARN (the frames go by yet another path)
python3 -I - "$ROOT" "$T/burstonly.bin" <<'PY'
import struct, sys
sys.path.insert(0, sys.argv[1] + "/tests")
import trace_fixture as f
w = f.W()
f.gen_run(w, "flippath")
w.ev = [e for e in w.ev if struct.unpack_from(f.EV, e)[2] != 23]
open(sys.argv[2], "wb").write(w.blob())
PY
out=$(python3 "$ROOT/tools/rdna3_trace.py" "$T/burstonly.bin")
has "$out" "WARN     H-FLIPPATH: head 0: 5 of 39 copy forwards with no swap of their surface within 50 ms, 1 burst(s) of 3 or more" burstonly
has "$out" "and NO flip-buffer call in the trace" burstonly
# the counters through triage: the write generations' verdict, a missing counter is loud
mk gencnt gen "swaps_h0 60
swap_mode 1
swap_gen 1
flipbuf_present 0
swaps_gen_aperture 30
swaps_gen_cf 30
swaps_gen_evicted 0
swaps_gen_freed 0
swaps_gen_resets 0
swaps_genpresent_h0 30
swaps_genhold_h0 30
swaps_genhold_would_h0 0
swaps_genfirst_h0 1
swaps_gencontent_h0 2
swaps_genunwritten_h0 30
cf_gennewer_h0 0
flipbuf_h0 0
flipbuf_scanout_h0 0
flipbuf_presented_h0 0
flipbuf_held_h0 0
flipbuf_failed_h0 0
swaps_gen_apcached 5
swaps_gen_cfnostamp 1
swaps_gen_judged_h0 60
swaps_gen_aged_h0 0
swaps_gen_evictpresent_h0 0
swaps_gen_landed_h0 0
swaps_gennotjudged_h0 0
swaps_gen_resets_h0 0
flipbuf_busy_h0 0
flipbuf_unlocked_h0 0
flipbuf_emptyheld_h0 0
flipbuf_nohead 0
flipbuf_nores 0
flipbuf_intoff 0
flipbuf_badptr 0
hook_intr 0
hook_intoff 0
hook_pickpresent 4
hook_didsubmitswap 0
hook_backready 9
hook_backready_no 1
hook_submitupdate 0" "" stream
out=$(run gencnt)
has "$out" "INFO swap gen: write generations: swap_gen 1, flipbuf_present 0, swap mode 1; generations handed out by page-on 30 (+5 by its cached descriptor), by copy forward 30 (1 targets kept theirs" triage-gencnt
hasnt "$out" "counter missing" triage-gencnt
hasnt "$out" "do not add up" triage-gencnt
hasnt "$out" "hook wiring unproven" triage-gencnt
has "$out" "WARN swap gen: head 0: 2 back(s) changed with NO write the accelerator saw" triage-gencnt
has "$out" "INFO swap gen: flip-present hooks: hook_pickpresent 4, hook_didsubmitswap 0, hook_backready 9, hook_backready_no 1" triage-gencnt
has "$out" "H-GEN: head 0: 60 swaps judged" triage-gencnt
hasnt "$out" "counter missing: swaps_genhold_h0" triage-gencnt
# ECC review round 1: swap_gen 1 but no swap judged on a head (the rule inert): FAIL in the trace and the counters
mk geninert clean "swaps_h0 40
swap_mode 1
swap_gen 1
swaps_genpresent_h0 0
swaps_genhold_h0 0" "" stream
out=$(run geninert)
has "$out" "FAIL frame trace: H-GEN: head 0: 40 swaps in mode 0/1 with swap_gen 1, NONE judged" triage-geninert
has "$out" "FAIL swap gen: head 0: the write generations judged NONE of 40 swaps" triage-geninert
mk gennocnt gen "swaps_h0 60
swap_mode 1
swap_gen 1
swaps_genpresent_h0 30" "" stream
out=$(run gennocnt)
has "$out" "WARN swap gen: head 0: counter missing: swaps_genhold_h0" triage-gennocnt
has "$out" "counter missing: swaps_genhold_h0 (the collector's grep" triage-gennocnt
# ECC review round 1 (H1): a kext of these sources whose dump has none of the write generations' counters
mk gennone gen "swaps_h0 60
swap_mode 1" "" stream
out=$(run gennone)
has "$out" "WARN swap gen: counter missing: swap_gen and the write generations' counters" triage-gennone
# ... and the same dump from an older kext says nothing
cp -r "$T/gennone" "$T/gennoneold"
python3 -I - "$T/gennoneold" <<'PY'
import glob, io, sys, tarfile
for p in glob.glob(sys.argv[1] + "/*.tar.gz"):
    with tarfile.open(p) as t:
        ms = [(m, t.extractfile(m).read() if m.isfile() else None) for m in t.getmembers()]
    with tarfile.open(p, "w:gz") as t:
        for m, data in ms:
            if m.name.endswith("20-log-rdna3.txt"):
                data = b"RDNA3Accelerator: build 0123456789\n"
                m.size = len(data)
            t.addfile(m, io.BytesIO(data) if data is not None else None)
PY
out=$(run gennoneold)
hasnt "$out" "swap gen: counter missing" triage-gennoneold
# ECC review round 1: the judged swaps against their outcomes, the first-after-reset share, interrupt-level hooks,
# the flip-buffer calls against their outcomes, the hooks never firing
mk genrecon gen "swaps_h0 60
swap_mode 1
swap_gen 1
flipbuf_present 1
swaps_genpresent_h0 20
swaps_genhold_h0 10
swaps_genhold_would_h0 0
swaps_gen_landed_h0 0
swaps_gen_judged_h0 50
swaps_genfirst_h0 10
flipbuf_h0 10
flipbuf_presented_h0 3
flipbuf_held_h0 2
hook_intr 3
hook_intr_flipbuf 3
hook_pickpresent 0" "" stream
out=$(run genrecon)
has "$out" "WARN swap gen: head 0: the write generations' outcomes do not add up: 50 judged, but presented 20 + held 10" triage-genrecon
has "$out" "WARN swap gen: head 0: 10 of 20 generation presents were the first after a reset (> 30%)" triage-genrecon
has "$out" "WARN swap gen: flip-present hooks called at INTERRUPT LEVEL 3 time(s) (flipbuf 3)" triage-genrecon
has "$out" "WARN swap gen: flip-buffer calls do not add up: 10 calls on our heads" triage-genrecon
mk genunproven gen "swaps_h0 60
swap_mode 1
swap_gen 1
flipbuf_present 0
swaps_genpresent_h0 60
swaps_gen_judged_h0 60
flipbuf_h0 0
flipbuf_scanout_h0 0
hook_pickpresent 0
hook_didsubmitswap 0
hook_backready 0
hook_submitupdate 0
hook_intr 0" "" stream
out=$(run genunproven)
has "$out" "WARN swap gen: hook wiring unproven: 60 swaps and NOT ONE call of any flip-present hook" triage-genunproven

# ECC review round 1 (M5): --all lists the first kernel-log lines of the write generations and the hooks
out=$(python3 -I - "$ROOT/tools" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import triage_reports as t
log = "\n".join(["RDNA3Accelerator: flip-present hooks installed: RD3DisplayPipe submitFlipBuffer vtable+2496"] +
                 ["swap #%d: head 0: the back (a1) 0x8b8f0000 held: x" % i for i in range(30)] +
                 ["write generations: head 0 reset (a mode change)", "something else"])
for l in t.gen_log_lines(log):
    print(l)
print(t.gen_log_lines(""))
PY
)
has "$out" "flip-present hooks installed: RD3DisplayPipe submitFlipBuffer vtable+2496" triage-genlog
has "$out" "write generations: head 0 reset" triage-genlog
has "$out" "swap #11: head 0" triage-genlog
hasnt "$out" "swap #12: head 0" triage-genlog
hasnt "$out" "something else" triage-genlog
has "$out" "(none: a kext without the write generations" triage-genlog

# DRIVER-PLAN N38: the present queue's PRESENT events (H-PRESENT)
python3 -I "$ROOT/tests/trace_fixture.py" present "$T/present.bin"
out=$(python3 "$ROOT/tools/rdna3_trace.py" "$T/present.bin" --timeline 400)
has "$out" "INFO     H-PRESENT: head 0: 60 present(s) (swap 60): 20 programmed at once, 40 queued (20 replaced: newest wins, 0 Busy kept), 0 joined; 40 latch(es) seen (avg 15000 us, max 16000 us), 20 programmed at a latch; 1 writer wait(s) (max 900 us)" present
has "$out" "PRESENT h0 swap #3 0x87040000 replaced (newest wins) (replaced #2) -> in flight 0x8b100000, queued 0x87040000" present
has "$out" "PRESENT h0 a copy forward into 0x8b100000 waited 900 us for the flip away from it" present
hasnt "$out" "FAIL     H-PRESENT" present
hasnt "$out" "WARN     H-PRESENT" present
python3 -I "$ROOT/tests/trace_fixture.py" presentbad "$T/presentbad.bin"
out=$(python3 "$ROOT/tools/rdna3_trace.py" "$T/presentbad.bin" --timeline 400)
has "$out" "FAIL     H-PRESENT: head 0: 1 completion(s) BEFORE the latch was seen (1 not latched in time, 0 unreadable; first: present #99 at 0x8b8f0000" presentbad
has "$out" "FAIL     H-PRESENT: head 0: 2 frame(s) LOST with no newer one (1 dropped: buffer freed, 1 refused when programmed from the queue; first: present #100 at 0x8b100000" presentbad
has "$out" "INFO     H-PRESENT: head 0: 1 queued present(s) dropped at a display mode change" presentbad
has "$out" "WARN     H-PRESENT: head 0: 1 writer wait(s) ran out" presentbad
has "$out" "TIMEOUT: completed EARLY 40000 us" presentbad
has "$out" "DROPPED (at a mode change)" presentbad

echo "test_triage_trace: all checks passed"
