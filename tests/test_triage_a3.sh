#!/bin/bash
# tools/triage_reports.py's A3 line (IOSurface VRAM) on synthetic reports:
# none, shared with kernel BOs, shared without any (a WARN), refusals.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf -- "$T"' EXIT
fail() { echo "test_triage_a3: FAIL $*"; exit 1; }

mk() {   # mk <name> <29-surface-vram.txt contents or "-" for none>
    python3 -I - "$T/$1" "$2" <<'PY'
import io, sys, tarfile
path, sv = sys.argv[1], sys.argv[2]
members = {
    "01-boot-args.txt": "boot-args rdna3lvl=8 rdna3fb=1 rdna3accel=1 rdna3agdc=2\n",
    "03-kexts.txt": "RDNA3Accel\n",
    "11-ioreg-props.txt": "",
    "20-log-rdna3.txt": "RDNA3: hello\n",
    "24-log-coredisplay.txt": "",
}
if sv != "-":
    members["29-surface-vram.txt"] = sv.replace("\\n", "\n")
with tarfile.open(path + ".tar.gz", "w:gz") as t:
    for n, body in members.items():
        data = body.encode()
        ti = tarfile.TarInfo("rdna3-report/" + n)
        ti.size = len(data)
        t.addfile(ti, io.BytesIO(data))
PY
}
mkdir -p "$T/r1" "$T/r2" "$T/r3" "$T/r4"
LIVE="NVMTL: item 7: IOSurface textures live in the surface's family VRAM (one per surface, every process)"
mk r1/rdna3-report-none -
mk r2/rdna3-report-ok "== 20-log-rdna3.txt\\nRDNA3: GEM: IOSurface 12 plane 0 -> 8100 KiB of the accelerator's VRAM at 0x1000000 (A3, import 1)\\n== 26-log-metal-plugin.txt\\n$LIVE"
mk r3/rdna3-report-nobo "== 26-log-metal-plugin.txt\\n$LIVE"
mk r4/rdna3-report-refused "== 20-log-rdna3.txt\\nRDNA3: GEM: IOSurface 9 plane 0 import refused: no such surface for this process (status 0x80000003, call 0, 1 of this cause)\\n== 26-log-metal-plugin.txt\\n$LIVE\\nNVMTL: item 7: surface 9 plane 0: family VRAM import refused (VkResult -1000072003, kernel errno 2 No such file or directory, 8294400 B) - private image + copies (1 so far)"

run() { python3 "$ROOT/tools/triage_reports.py" "$T/$1" 2>&1 || true; }
o=$(run r1); grep -q 'INFO IOSurface VRAM (A3)  *no 29-surface-vram.txt' <<<"$o" || fail "r1: $o"
o=$(run r2); grep -q 'INFO IOSurface VRAM (A3)  *1 process(es) shared, 0 private; 1 kernel BO' <<<"$o" || fail "r2: $o"
o=$(run r3); grep -q 'WARN IOSurface VRAM (A3)  *1 process(es) shared, 0 private; 0 kernel BO' <<<"$o" || fail "r3: $o"
o=$(run r4)
grep -q 'WARN IOSurface VRAM (A3)' <<<"$o" || fail "r4 state: $o"
grep -q 'kernel refused: no such surface for this process' <<<"$o" || fail "r4 kernel: $o"
grep -q 'plugin import refused: VkResult -1000072003, kernel errno 2' <<<"$o" || fail "r4 plugin: $o"
! grep -q Traceback <<<"$(run r1)$(run r2)$(run r3)$(run r4)" || fail "a traceback"

# card run 58: WindowServer's display framebuffers kept out of A3 (the
# policy, not a warning), the refusal that never fired, the accelerator's
# verdict when they were let in, the counters, and a main screen frozen
# through the M6 looks (judged on the main pipe only, when every app ran)
mk58() {   # mk58 <name> <20-log-rdna3.txt> <29-surface-vram.txt> <m6 pictures> [extra tests text]
    # m6 pictures: differ | same (pipe 0 frozen) | same1 (pipe 1 frozen) | tick (pipe 0: one pixel) | two | none
    python3 -I - "$T/$1" "$2" "$3" "$4" "${5:-}" <<'PY'
import io, sys, tarfile
path, log, sv, m6, extra = sys.argv[1:6]
members = {
    "01-boot-args.txt": "boot-args rdna3lvl=8 rdna3fb=1 rdna3accel=1 rdna3agdc=2\n",
    "03-kexts.txt": "RDNA3Accel\n",
    "11-ioreg-props.txt": '    "RDNA3,Head0,Route" = "OTG0 -> DIGD -> UNIPHYD (mode 3) -> HDMI-4 DDC1"\n',
    "20-log-rdna3.txt": log.replace("\\n", "\n"),
    "24-log-coredisplay.txt": "",
    "29-surface-vram.txt": sv.replace("\\n", "\n"),
}
with tarfile.open(path + ".tar.gz", "w:gz") as t:
    for n, body in members.items():
        data = body.encode()
        ti = tarfile.TarInfo("rdna3-report/" + n)
        ti.size = len(data)
        t.addfile(ti, io.BytesIO(data))
open(path + "-tests.txt", "w").write(
    "== rdna3_drminfo --scanout-hires (look m6Chess)\npipe 0: LIT otg 0 1920x1080\npipe 1: LIT otg 1 1920x1080\n"
    "2 pipe(s) lit\n== probe: Metal apps (M6)\nM6: 2 of 2 apps ran on the plugin without a crash\n== M6 end\n" +
    extra.replace("\\n", "\n"))
looks = ("Chess", "Maps", "Settings", "desktop")[:2 if m6 == "two" else 4]
if m6 != "none":
    for i, look in enumerate(looks):
        for pipe in (0, 1):
            frozen = (pipe == 0 and m6 in ("same", "tick")) or (pipe == 1 and m6 == "same1")
            px = bytearray([40 if frozen else 40 + 50 * i] * (32 * 32 * 3))
            if m6 == "tick" and pipe == 0:
                px[0:3] = bytes([i * 60] * 3)   # the menu bar clock: one pixel of 1024
            with open("%s-scanoutm6%s-pipe%d.ppm" % (path, look, pipe), "wb") as f:
                f.write(b"P6\n32 32\n255\n" + bytes(px))
PY
}
for r in r5 r6 r7 r8 r9 r10 r11 r12 r13 r14 r15 r16 r17 r18 r19 r20 r21 r22 r23 r24 r25 r26 r27 r28; do mkdir -p "$T/$r"; done
KEPTBOOT="RDNA3Accelerator: A3: WindowServer's display framebuffers are kept out of A3: WindowServer composites them through copies the legacy swap pages on (card run 58)"
SCAN="RDNA3Accelerator: RD3DisplayPipe::initFramebufferResource(head 0, index 0, res 0x1) -> vidmem 0x2 over the LIVE SCANOUT phys 0x80000000 1920 x 1080 pitch 7680 (8294400 bytes)"
KEPT="RDNA3Accelerator: surfgem: surface 1 plane 0 refused: WindowServer's display framebuffer of head mask 0x3 (1920x1080): the legacy swaps scan out the legacy surface's own VRAM, never a BO over this one (card run 58) -- copies instead; rdna3a3display=1 lets it in (status 0x80000007, 1 of this cause; ok 0, refused 1)"
PLUGREF="NVMTL: item 7: surface 1 plane 0: family VRAM import refused (VkResult -1000072003, kernel errno 1 Operation not permitted, 8294400 B) - private image + copies (1 so far)"
OTHERPERM="NVMTL: item 7: surface 77 plane 0: family VRAM import refused (VkResult -1000072003, kernel errno 1 Operation not permitted, 65536 B) - private image + copies (2 so far)"
GEM23="RDNA3: GEM: IOSurface 23 plane 0 -> 772 KiB of the accelerator's VRAM at 0xd0f0000 (A3, import 1)"
WSBO="NVMTL: item 7: surface 2 1920x1080 pitch 7680 -> the family's VRAM (vram 1, private 0)"
A3OK="RDNA3Accelerator: a3 display: head 0: 120 legacy swaps, it scans out phys 0x8b100000 (1920x1080); WindowServer's display framebuffers are kept out of A3, composited through copies (4 refused, 9 other WindowServer surfaces, 0 unchecked)"
A3NEVER="RDNA3Accelerator: a3 display: head 0: 8 legacy swaps, it scans out phys 0x8b100000 (1920x1080); WindowServer's display framebuffers are kept out of A3, composited through copies (0 refused, 9 other WindowServer surfaces, 0 unchecked) *** the refusal never fired ***"
A3BAD="RDNA3Accelerator: a3 display: head 0: 120 legacy swaps, 0 scanned out an A3 BO; it shows phys 0x8b100000, WindowServer drew display surface 1 at phys 0x8d0f0000 last (2 BO(s) that may be this head's, e.g. 0x8d0f0000) -- *** WINDOWSERVER DRAWS WHERE THE PANEL DOES NOT READ (card run 58) ***"
mk58 r5/rdna3-report-kept "RDNA3: hello\\n$KEPTBOOT\\n$SCAN\\n$KEPT\\n$A3OK" "== 20-log-rdna3.txt\\n$KEPT\\n$GEM23\\n== 26-log-metal-plugin.txt\\n$LIVE\\n$PLUGREF" differ
mk58 r6/rdna3-report-apart "RDNA3: hello\\nRDNA3Accelerator: A3: WindowServer's display framebuffers MAY become BOs over their own VRAM\\n$A3BAD" "== 26-log-metal-plugin.txt\\n$LIVE" none
mk58 r7/rdna3-report-frozen "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" same
mk58 r8/rdna3-report-moving "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" differ
mk58 r9/rdna3-report-missed "RDNA3: hello\\n$KEPTBOOT\\n$SCAN" "== 20-log-rdna3.txt\\n$GEM23\\n== 24-log-coredisplay.txt\\n$LIVE\\n$WSBO" none
mk58 r10/rdna3-report-never "RDNA3: hello\\n$KEPTBOOT\\n$A3NEVER" "== 26-log-metal-plugin.txt\\n$LIVE" none
mk58 r11/rdna3-report-quiet "RDNA3: hello\\n$KEPTBOOT\\n$SCAN" "== 20-log-rdna3.txt\\n$GEM23\\n== 26-log-metal-plugin.txt\\n$LIVE" none
mk58 r12/rdna3-report-norun "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" same "M6 Chess: running NO, plugin NOT loaded, crash report: none\\n"
mk58 r13/rdna3-report-side "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" same1
mk58 r14/rdna3-report-tick "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" tick
mk58 r15/rdna3-report-two "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" two
mk58 r16/rdna3-report-ctr "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" differ "debug.rdna3accel_a3display: 1\\ndebug.rdna3accel_a3display_imported: 4\\ndebug.rdna3accel_swaps_h0: 500\\ndebug.rdna3accel_swaps_a3_h0: 0\\n"
mk58 r17/rdna3-report-oldk "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" differ "(no rdna3accel_a3display sysctl: kext older than card run 58)\\n"
mk58 r18/rdna3-report-perm "RDNA3: hello\\n$KEPTBOOT\\n$SCAN\\n$KEPT" "== 20-log-rdna3.txt\\n$KEPT\\n$GEM23\\n== 26-log-metal-plugin.txt\\n$LIVE\\n$PLUGREF\\n$OTHERPERM" differ
o=$(run r5)
grep -q 'INFO IOSurface VRAM (A3)' <<<"$o" || fail "r5: display refusals are the policy, not a WARN: $o"
grep -q 'INFO A3 display  *WindowServer.s display framebuffers kept out of A3: 1 surface(s) refused' <<<"$o" || fail "r5 A3 display: $o"
grep -q 'a3 display: head 0: 120 legacy swaps' <<<"$o" || fail "r5 verdict line: $o"
! grep -qE 'FAIL (A3 display|M6 screen)' <<<"$o" || fail "r5 no A3 display / M6 screen FAIL: $o"
o=$(run r6)
grep -q 'FAIL A3 display  *WindowServer draws display surfaces into A3 BOs no legacy swap scans out' <<<"$o" || fail "r6: $o"
grep -q 'DOES NOT READ' <<<"$o" || fail "r6 the line: $o"
o=$(run r7)
grep -q 'FAIL M6 screen: pipe 0 (main display, by [^)]*) showed the same picture in all 4 M6 looks (Chess, Maps, Settings, desktop' <<<"$o" || fail "r7: $o"
grep -q 'PASS M6 screen: pipe 1 changed' <<<"$o" || fail "r7 pipe 1 changed: $o"
grep -q 'M6 screens  *pipe 0 (main) FROZEN in all 4 looks' <<<"$o" || fail "r7 summary: $o"
o=$(run r8)
! grep -q 'FAIL M6 screen' <<<"$o" || fail "r8: $o"
grep -q "M6 screens  *pipe 0 changed" <<<"$o" || fail "r8 summary: $o"
o=$(run r9)
grep -q 'FAIL A3 display  *the refusal never fired: 1 WindowServer surface(s) of a scanout size became BOs (2)' <<<"$o" || fail "r9: $o"
o=$(run r10)
grep -q 'FAIL A3 display  *the refusal never fired' <<<"$o" || fail "r10: $o"
o=$(run r11)
grep -q 'INFO A3 display  *kept-out mode, but no display framebuffer import seen' <<<"$o" || fail "r11: $o"
o=$(run r12)
grep -q 'INFO M6 screen: pipe 0 (main display, by [^)]*) did not change in the M6 looks, not judged: Chess did not run' <<<"$o" || fail "r12: $o"
! grep -q 'FAIL M6 screen' <<<"$o" || fail "r12 no FAIL: $o"
o=$(run r13)
grep -q 'INFO M6 screen: pipe 1 did not change in the M6 looks .*not the main display' <<<"$o" || fail "r13: $o"
! grep -q 'FAIL M6 screen' <<<"$o" || fail "r13 no FAIL: $o"
o=$(run r14)
grep -q 'FAIL M6 screen: pipe 0 (main display, by [^)]*) showed the same picture' <<<"$o" || fail "r14: one clock pixel must not clear a frozen pipe: $o"
o=$(run r15)
grep -q 'INFO M6 screen: pipe 0 (main display, by [^)]*): 2 M6 look(s), not judged' <<<"$o" || fail "r15: $o"
o=$(run r16)
grep -q 'FAIL A3 display: 4 display surface(s) imported (rdna3a3display=1) and no legacy swap ever scanned one out' <<<"$o" || fail "r16: $o"
grep -q 'A3 display counters  *refused ?, imported 4' <<<"$o" || fail "r16 summary: $o"
o=$(run r17)
grep -q 'INFO A3 display counters: none (the kext predates card run 58)' <<<"$o" || fail "r17: $o"
o=$(run r18)
grep -q 'WARN IOSurface VRAM (A3)' <<<"$o" || fail "r18: another EPERM refusal stays a warning: $o"
grep -q 'plugin import refused: VkResult -1000072003, kernel errno 1' <<<"$o" || fail "r18 line: $o"
# round 2: the marker of a kernel that printed it with nothing looked at
# (aa913df), 2 heads x 4 display surfaces with the kernel naming only 6 of
# them, the IOP flip path
A3QUIET="RDNA3Accelerator: a3 display: head 0: 8 legacy swaps, it scans out phys 0x8b100000 (1920x1080); WindowServer's display framebuffers are kept out of A3, composited through copies (0 refused, 0 other WindowServer surfaces, 0 unchecked) *** the refusal never fired ***"
mk58 r19/rdna3-report-quietmark "RDNA3: hello\\n$KEPTBOOT\\n$A3QUIET" "== 26-log-metal-plugin.txt\\n$LIVE" none
K6=""; P8=""
for i in 1 2 3 4 5 6; do K6="$K6\\nRDNA3Accelerator: surfgem: surface $i plane 0 refused: WindowServer's display framebuffer of head mask 0x3 (1920x1080): copies instead (status 0x80000007, $i of this cause; ok 0, refused $i)"; done
for i in 1 2 3 4 5 6 7 8; do P8="$P8\\nNVMTL: item 7: surface $i plane 0: family VRAM import refused (VkResult -1000072003, kernel errno 1 Operation not permitted, 8294400 B) - private image + copies ($i so far)"; done
mk58 r20/rdna3-report-eight "RDNA3: hello\\n$KEPTBOOT\\n$SCAN$K6" "== 20-log-rdna3.txt$K6\\n$GEM23\\n== 24-log-coredisplay.txt\\n$LIVE$P8\\n== sysctl (card run 58)\\ndebug.rdna3accel_a3display: 0\\ndebug.rdna3accel_a3display_refused: 8" differ
mk58 r21/rdna3-report-eightnoctr "RDNA3: hello\\n$KEPTBOOT\\n$SCAN$K6" "== 20-log-rdna3.txt$K6\\n$GEM23\\n== 24-log-coredisplay.txt\\n$LIVE$P8" differ
mk58 r22/rdna3-report-iopflip "RDNA3: hello\\n$KEPTBOOT\\n$SCAN\\nRDNA3Accelerator: surfgem: debug.rdna3accel_iop_flip=1 flips IOSurfaces out of their own VRAM: no display surface is kept out of A3" "== 20-log-rdna3.txt\\n$GEM23\\n== 24-log-coredisplay.txt\\n$LIVE\\n$WSBO" none
o=$(run r19)
! grep -q 'FAIL A3 display' <<<"$o" || fail "r19: the marker with nothing looked at is no failure: $o"
grep -q 'INFO A3 display  *kept-out mode, but no display framebuffer import seen' <<<"$o" || fail "r19 INFO: $o"
o=$(run r20)
grep -q 'INFO IOSurface VRAM (A3)' <<<"$o" || fail "r20: 8 display EPERMs with the counter at 8 are the policy: $o"
grep -q 'INFO A3 display  *WindowServer.s display framebuffers kept out of A3: 8 surface(s) refused' <<<"$o" || fail "r20 kept: $o"
o=$(run r21)
grep -q 'WARN IOSurface VRAM (A3)' <<<"$o" || fail "r21: without the counter 2 unnamed EPERMs stay a warning: $o"
o=$(run r22)
! grep -q 'FAIL A3 display' <<<"$o" || fail "r22: the IOP flip path keeps display BOs by design: $o"
grep -q 'INFO A3 display  *debug.rdna3accel_iop_flip=1' <<<"$o" || fail "r22 INFO: $o"
# card run 58 (2026-10-15d): flicker on both screens, WindowServer's inputs
# kept out, the system asleep at the sleep look, gamma that cannot be
# compared on a changing screen, the main display found by its mode switch,
# a run stopped inside M6
WSBOOT="RDNA3Accelerator: A3: WindowServer's inputs are kept out (composited through copies); applications keep A3 for their own and shared surfaces (card run 58, 2026-10-15d)"
WSREF="RDNA3Accelerator: surfgem: surface 33 plane 0 refused: a WindowServer input (1920x24): its writers (GPU, CPU, the plugin's copies) are not ordered with the VRAM WindowServer would sample (card run 58: flicker on both screens) -- copies instead; rdna3a3ws=1 lets it in (status 0x80000008, 1 of this cause; ok 0, refused 1)"
WSPLUG="NVMTL: item 7: surface 33 plane 0: family VRAM import refused (VkResult -1000072003, kernel errno 1 Operation not permitted, 184320 B) - private image + copies (1 so far)"
WSPLUG2="NVMTL: item 7: surface 34 plane 0: family VRAM import refused (VkResult -1000072003, kernel errno 1 Operation not permitted, 8192 B) - private image + copies (2 so far)"
mk58 r23/rdna3-report-flicker "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" differ "debug.rdna3accel_swaps_h0: 765\\ndebug.rdna3accel_swaps_abab_h0: 500\\ndebug.rdna3accel_swaps_h1: 269\\ndebug.rdna3accel_swaps_abab_h1: 3\\n"
WSREF2="${WSREF//surface 33/surface 34}"
mk58 r24/rdna3-report-wskept "RDNA3: hello\\n$KEPTBOOT\\n$WSBOOT\\n$SCAN\\n$WSREF\\n$WSREF2" "== 20-log-rdna3.txt\\n$WSREF\\n$WSREF2\\n$GEM23\\n== 24-log-coredisplay.txt\\n$LIVE\\n$WSPLUG\\n$WSPLUG2\\n== sysctl (card run 58)\\ndebug.rdna3accel_a3ws_refused: 2" differ
mk58 r25/rdna3-report-asleep "RDNA3: hello\\nRDNA3: display: head 0 blanked (display sleep) (0)" "== 26-log-metal-plugin.txt\\n$LIVE" differ "== rdna3_drminfo --scanout (look sleep)\\npipe 0: not read: the display is asleep (system sleep or shutdown)\\npipe 1: not read: the display is asleep (system sleep or shutdown)\\ndisplay asleep: 4 pipe(s) not read, nothing to judge\\n[exit 0]\\n"
mk58 r26/rdna3-report-gamma "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" differ "CRC before NOT stable (the screen changed between the two looks)\\nGAMMA: UNJUDGED - 2 pipe(s) moved with the dark table, but the screen changed between every two looks (CRC before not stable): the restore cannot be compared\\n"
mk58 r27/rdna3-report-maindp "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" same1 "== rdna3_drminfo --scanout (look mode-d0-1600x900)\\npipe 0: LIT otg 0 1920x1080 pitch 1920\\npipe 1: LIT otg 1 1600x900 pitch 1600\\n2 pipe(s) lit\\n"
mk58 r28/rdna3-report-m6cut "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" none
# r28: the run stopped inside M6 (no "M6: n of 2" line)
# portable in-place edit (BSD sed on the macOS runner has no GNU -i)
f="$T/r28/rdna3-report-m6cut-tests.txt"
sed -e '/^M6: /d' -e '/^== M6 end$/d' "$f" > "$f.new" && mv "$f.new" "$f"
o=$(run r23)
# a kernel that does not count the bands, one head over the line: a caret
# cannot be told from a picture (card run 58b review), WARN
grep -q 'WARN flicker: head 0: alternating pictures (candidate flicker): 500 of 765 legacy swaps since boot.*this kernel does not count the bands' <<<"$o" || fail "r23: $o"
grep -q 'PASS flicker: head 1: 3 of 269 legacy swaps' <<<"$o" || fail "r23 head 1: $o"
grep -q 'flicker  *WARN head 0: alternating pictures' <<<"$o" || fail "r23 summary: $o"
! grep -q 'the kernel logged alternating pictures' <<<"$o" || fail "r23: one WARN is enough: $o"
o=$(run r24)
grep -q 'INFO IOSurface VRAM (A3)' <<<"$o" || fail "r24: WindowServer's refused inputs are the policy, not a WARN: $o"
grep -q 'INFO A3 WindowServer  *WindowServer.s inputs kept out of A3: 2 refused' <<<"$o" || fail "r24 WS: $o"
o=$(run r25)
grep -q 'INFO display sleep: not judged by the look (the system was asleep); the kernel blanked head(s) 0 first' <<<"$o" || fail "r25: $o"
! grep -q 'FAIL display sleep' <<<"$o" || fail "r25 no FAIL: $o"
o=$(run r26)
grep -q 'GAMMA: UNJUDGED' <<<"$o" || fail "r26: $o"
grep -q 'FAIL gamma: the screens kept changing (CRC before not stable) and no RDNA3,Head<n>,Gamma record' <<<"$o" || fail "r26: no core record is a FAIL: $o"
o=$(run r27)
grep -q 'FAIL M6 screen: pipe 1 (main display, by display 0.s mode switch to 1600x900) showed the same picture' <<<"$o" || fail "r27: the main display is the pipe that followed display 0's mode switch: $o"
grep -q 'INFO M6 screen: pipe 0 ' <<<"$o" && fail "r27 pipe 0 changed: $o"
o=$(run r28)
grep -q 'M6 screens  *NOT FINISHED: the run stopped inside M6' <<<"$o" || fail "r28: $o"
# round 2 of card run 58b: the flicker test's denominator and dumps, a
# WindowServer input that leaked into A3, gamma judged by the core's record,
# a display that never blanked
for r in r29 r30 r31 r32 r33 r34 r35; do mkdir -p "$T/$r"; done
mk58 r29/rdna3-report-fewswaps "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" differ "== A3 display counters after M6\\ndebug.rdna3accel_swaps_h0: 40\\ndebug.rdna3accel_swaps_hashed_h0: 20\\ndebug.rdna3accel_swaps_abab_h0: 10\\n"
mk58 r30/rdna3-report-delta "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" differ "== display pipe and AGDC counters\\ndebug.rdna3accel_swaps_h0: 1000\\ndebug.rdna3accel_swaps_hashed_h0: 1000\\ndebug.rdna3accel_swaps_abab_h0: 10\\ndebug.rdna3accel_swaps_ababwide_h0: 10\\n== A3 display counters after M6\\ndebug.rdna3accel_swaps_h0: 1100\\ndebug.rdna3accel_swaps_hashed_h0: 1100\\ndebug.rdna3accel_swaps_abab_h0: 90\\ndebug.rdna3accel_swaps_ababwide_h0: 60\\n"
mk58 r31/rdna3-report-skipped "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" differ "== A3 display counters after M6\\ndebug.rdna3accel_swaps_h0: 300\\ndebug.rdna3accel_swaps_hashed_h0: 0\\ndebug.rdna3accel_swaps_hashskip_h0: 300\\ndebug.rdna3accel_swaps_abab_h0: 0\\n"
mk58 r32/rdna3-report-wsleak "RDNA3: hello\\n$KEPTBOOT\\n$WSBOOT\\n$SCAN" "== 20-log-rdna3.txt\\n$GEM23\\n== 24-log-coredisplay.txt\\n$LIVE\\nNVMTL: item 7: surface 2 1920x24 pitch 7680 -> the family's VRAM (vram 1, private 0)" none
mk58 r33/rdna3-report-wscount "RDNA3: hello\\n$KEPTBOOT\\n$WSBOOT\\n$SCAN" "== 20-log-rdna3.txt\\n$GEM23\\n== 24-log-coredisplay.txt\\n$LIVE\\n== sysctl (card run 58)\\ndebug.rdna3accel_a3display_other: 10\\ndebug.rdna3accel_a3ws_refused: 4" none
mk58 r34/rdna3-report-gammarec "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" differ "== gamma property before\\n\"RDNA3,Head0,Gamma\" = \"table 3x256 -> 0: MPCC 0 mode 2 RAM A, DPP degamma 0, 4 calls\"\\n\"RDNA3,Head1,Gamma\" = \"table 3x256 -> 0: MPCC 1 mode 2 RAM A, DPP degamma 0, 4 calls\"\\nCRC before NOT stable (the screen changed between the two looks)\\nGAMMA: UNJUDGED - the screen changed between every two looks (CRC before not stable): neither the dark table nor the restore can be judged from the CRCs of 2 pipe(s)\\n== gamma property after\\n\"RDNA3,Head0,Gamma\" = \"table 3x256 -> 0: MPCC 0 mode 2 RAM B, DPP degamma 0, 8 calls\"\\n\"RDNA3,Head1,Gamma\" = \"table 3x256 -> 0: MPCC 1 mode 2 RAM B, DPP degamma 0, 7 calls\"\\n[exit 0]\\n"
mk58 r35/rdna3-report-noblank "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" differ "== rdna3_drminfo --scanout (look sleep)\\npipe 0: not read: the display is asleep (system sleep or shutdown)\\ndisplay asleep: 4 pipe(s) not read, nothing to judge\\n[exit 0]\\n"
for r in r36; do mkdir -p "$T/$r"; done
mk58 r36/rdna3-report-wsunnamed "RDNA3: hello\\n$KEPTBOOT\\n$WSBOOT\\n$SCAN\\n$WSREF" "== 20-log-rdna3.txt\\n$WSREF\\n$GEM23\\n== 24-log-coredisplay.txt\\n$LIVE\\n$WSPLUG\\n$WSPLUG2\\n== sysctl (card run 58)\\ndebug.rdna3accel_a3ws_refused: 2" differ
o=$(run r36)
grep -q 'WARN IOSurface VRAM (A3)' <<<"$o" || fail "r36: an EPERM of a surface no kernel line named stays a warning: $o"
o=$(run r29)
grep -q 'INFO flicker: head 0: not judged, 10 of 20 hashed swaps' <<<"$o" || fail "r29: fewer than 60 hashed swaps are not judged: $o"
! grep -q 'PASS flicker: head 0' <<<"$o" || fail "r29 no PASS: $o"
o=$(run r30)
grep -q "FAIL flicker: head 0: alternating pictures (candidate flicker): 80 of 100 hashed swaps between 'display pipe and AGDC counters' and 'A3 display counters after M6'" <<<"$o" || fail "r30: the change between the dumps decides: $o"
grep -q 'swaps h0 1100 (A3 0)$' <<<"$o" || fail "r30: one swaps entry per head in the summary: $o"
o=$(run r31)
grep -q 'INFO flicker: head 0: NOT MEASURED .*swaps_hashskip_h0 300' <<<"$o" || fail "r31: $o"
o=$(run r32)
grep -q 'FAIL A3 WindowServer  *WindowServer input leaked into A3: 1 surface(s)' <<<"$o" || fail "r32: $o"
o=$(run r33)
grep -q 'FAIL A3 WindowServer  *WindowServer input leaked into A3: 10 non-display import(s), 4 refused' <<<"$o" || fail "r33: $o"
o=$(run r34)
grep -q 'WARN gamma: open: the screens kept changing .*head 0 +4, head 1 +3' <<<"$o" || fail "r34: $o"
! grep -q 'FAIL gamma' <<<"$o" || fail "r34 no FAIL: $o"
o=$(run r35)
grep -q 'FAIL display sleep: never blanked a head' <<<"$o" || fail "r35: $o"
all=""
# round 3 of card run 58b: the bands that alternated (a caret is one band),
# the dump right before the probes, a display sleep with no kernel log, a
# refusal for another task
for r in r37 r38 r39 r40 r41 r42; do mkdir -p "$T/$r"; done
mk58 r37/rdna3-report-twoheads "RDNA3: hello" "== 26-log-metal-plugin.txt\n$LIVE" differ "debug.rdna3accel_swaps_h0: 765\ndebug.rdna3accel_swaps_abab_h0: 500\ndebug.rdna3accel_swaps_h1: 269\ndebug.rdna3accel_swaps_abab_h1: 100\n"
mk58 r38/rdna3-report-wide "RDNA3: hello" "== 26-log-metal-plugin.txt\n$LIVE" differ "== A3 display counters after M6\ndebug.rdna3accel_swaps_h0: 400\ndebug.rdna3accel_swaps_hashed_h0: 400\ndebug.rdna3accel_swaps_abab_h0: 200\ndebug.rdna3accel_swaps_ababwide_h0: 150\ndebug.rdna3accel_swaps_h1: 400\ndebug.rdna3accel_swaps_hashed_h1: 400\ndebug.rdna3accel_swaps_abab_h1: 0\ndebug.rdna3accel_swaps_ababwide_h1: 0\n"
mk58 r39/rdna3-report-caret "RDNA3: hello\nRDNA3Accelerator: flicker: head 0: swap 120 showed alternating pictures (A B A B in 1 of 8 bands, candidate flicker), 120 of 240 hashed swaps so far, 0 in more than one band (phys 0xb100000, 1920x1080)" "== 26-log-metal-plugin.txt\n$LIVE" differ "== A3 display counters after M6\ndebug.rdna3accel_swaps_h0: 400\ndebug.rdna3accel_swaps_hashed_h0: 400\ndebug.rdna3accel_swaps_abab_h0: 200\ndebug.rdna3accel_swaps_ababwide_h0: 3\n"
mk58 r40/rdna3-report-beforeprobes "RDNA3: hello" "== 26-log-metal-plugin.txt\n$LIVE" differ "== display pipe and AGDC counters\ndebug.rdna3accel_swaps_h0: 500\ndebug.rdna3accel_swaps_hashed_h0: 500\ndebug.rdna3accel_swaps_abab_h0: 0\ndebug.rdna3accel_swaps_ababwide_h0: 0\n== A3 display counters before the probes\ndebug.rdna3accel_flicker: 2\ndebug.rdna3accel_swaps_h0: 1000\ndebug.rdna3accel_swaps_hashed_h0: 1000\ndebug.rdna3accel_swaps_abab_h0: 10\ndebug.rdna3accel_swaps_ababwide_h0: 10\n== probe: platform (R3 bare metal, R4 macOS 26)\n== A3 display counters after M6\ndebug.rdna3accel_swaps_h0: 1100\ndebug.rdna3accel_swaps_hashed_h0: 1100\ndebug.rdna3accel_swaps_abab_h0: 90\ndebug.rdna3accel_swaps_ababwide_h0: 90\n"
mk58 r41/rdna3-report-nolog "" "== 26-log-metal-plugin.txt\n$LIVE" differ "== rdna3_drminfo --scanout (look sleep)\npipe 0: not read: the display is asleep (system sleep or shutdown)\ndisplay asleep: 4 pipe(s) not read, nothing to judge\n[exit 0]\n"
TMREF="RDNA3Accelerator: surfgem: surface 33 plane 0 refused: imported for another task than the caller's (1920x24): its process is unknown and might be WindowServer -- copies instead (status 0x80000008, 1 of this cause; ok 0, refused 1)"
TMREF2="${TMREF//surface 33/surface 34}"
mk58 r42/rdna3-report-taskmis "RDNA3: hello\n$KEPTBOOT\n$WSBOOT\n$SCAN\n$TMREF\n$TMREF2" "== 20-log-rdna3.txt\n$TMREF\n$TMREF2\n$GEM23\n== 24-log-coredisplay.txt\n$LIVE\n$WSPLUG\n$WSPLUG2\n== sysctl (card run 58)\ndebug.rdna3accel_a3display_task_mismatch: 2" differ
o=$(run r37)
grep -q 'FAIL flicker: head 0: alternating pictures .*this kernel does not count the bands; on 2 heads' <<<"$o" || fail "r37: two heads over the line: $o"
grep -q 'FAIL flicker: head 1: alternating pictures .*on 2 heads' <<<"$o" || fail "r37 head 1: $o"
o=$(run r38)
grep -q 'FAIL flicker: head 0: alternating pictures (candidate flicker): 200 of 400 hashed swaps.*150 of them in more than one band' <<<"$o" || fail "r38: more than one band is a picture: $o"
grep -q 'PASS flicker: head 1: no A B A B' <<<"$o" || fail "r38 head 1: $o"
o=$(run r39)
grep -q 'WARN flicker: head 0: alternating pictures .*3 of them in more than one band: one band taking turns on one head' <<<"$o" || fail "r39: a caret is a WARN: $o"
! grep -q 'FAIL flicker' <<<"$o" || fail "r39 no FAIL: $o"
! grep -q 'the kernel logged alternating pictures' <<<"$o" || fail "r39: one WARN is enough: $o"
o=$(run r40)
grep -q "FAIL flicker: head 0: alternating pictures (candidate flicker): 80 of 100 hashed swaps between 'A3 display counters before the probes' and 'A3 display counters after M6'" <<<"$o" || fail "r40: the dump before the probes is the baseline: $o"
o=$(run r41)
grep -q 'INFO display sleep: not judged: the look found the system asleep and there is no kernel log' <<<"$o" || fail "r41: $o"
! grep -q 'FAIL display sleep' <<<"$o" || fail "r41 no FAIL: $o"
o=$(run r42)
grep -q 'INFO IOSurface VRAM (A3)' <<<"$o" || fail "r42: a refusal for another task is the policy: $o"
# card run 58b (2026-10-16): the VM stopped before the report, only the
# serial log - two backs taking turns while copy forward copied nothing
# (an older kernel), and the copy forward counters of a fixed one
for r in r43 r44 r45; do mkdir -p "$T/$r"; done
{
    echo "RDNA3Accelerator: copy forward #1: the front buffer (vm <ptr>) was never shown -- the back buffer already holds the last frame, nothing copied (1 such calls)"
    echo "RDNA3Accelerator: swap #85: head 1 frame phys 0x8c0e0000 -> 0x8b100000 (surface <ptr>, argument is the pipe resource, a1 <ptr> a2 <ptr>)"
    echo "RDNA3Accelerator: swap #113: head 1 frame phys 0x8b100000 -> 0x8b8f0000 (surface <ptr>, surface seen before, a1 <ptr> a2 <ptr>)"
    echo "RDNA3Accelerator: swap #114: head 1 frame phys 0x8b8f0000 -> 0x8b100000 (surface <ptr>, surface seen before, a1 <ptr> a2 <ptr>)"
    echo "RDNA3Accelerator: flicker: head 1: swap 204 showed alternating pictures (A B A B in 6 of 8 bands, candidate flicker), 120 of 204 hashed swaps so far, 53 in more than one band (phys 0x8b100000, 1920x1080)"
    echo "RDNA3Accelerator: flicker: head 0: swap 194 showed alternating pictures (A B A B in 7 of 8 bands, candidate flicker), 8 of 194 hashed swaps so far, 5 in more than one band (phys 0x8c8d0000, 1920x1080)"
} > "$T/r43/serial-20261015-201955.log"
mk58 r44/rdna3-report-cfshown "RDNA3: hello" "== 26-log-metal-plugin.txt\n$LIVE" differ "== A3 display counters after M6\ndebug.rdna3accel_swaps_h1: 400\ndebug.rdna3accel_cf_shown_h1: 380\ndebug.rdna3accel_cf_backlast_h0: 200\ndebug.rdna3accel_cf_src_h1: 0\ndebug.rdna3accel_cf_notshown: 0\ndebug.rdna3accel_cf_shown_failed: 0\ndebug.rdna3accel_swaps_frontskip_h1: 2\n"
mk58 r45/rdna3-report-cfstale "RDNA3: hello" "== 26-log-metal-plugin.txt\n$LIVE" differ "== A3 display counters after M6\ndebug.rdna3accel_swaps_h1: 400\ndebug.rdna3accel_cf_shown_h1: 300\ndebug.rdna3accel_cf_notshown: 7\ndebug.rdna3accel_cf_shown_failed: 3\n"
o=$(run r43)
grep -q 'FAIL swap/flicker  *head 1: alternating pictures (candidate flicker): 120 of 204 hashed swaps A B A B, 53 in more than one band' <<<"$o" || fail "r43: the serial log's flicker lines: $o"
grep -q 'PASS swap/flicker  *head 0: 8 of 194' <<<"$o" || fail "r43 head 0: $o"
grep -q 'WARN swap/flicker  *head(s) 1: two back buffers took turns while copy forward copied nothing' <<<"$o" || fail "r43: two backs, no copy forward: $o"
o=$(run r44)
grep -q 'copy forward  *INFO h0 0 from the shown buffer / 200 not needed / 0 from the front, h1 380 from the shown buffer / 0 not needed / 0 from the front; 0 refused by SDMA, 0 with no head (rects left stale); 2 swap(s) skipped' <<<"$o" || fail "r44: $o"
o=$(run r45)
grep -q 'copy forward  *WARN .*3 refused by SDMA, 7 with no head' <<<"$o" || fail "r45: $o"
for r in r5 r6 r7 r8 r9 r10 r11 r12 r13 r14 r15 r16 r17 r18 r19 r20 r21 r22 r23 r24 r25 r26 r27 r28 r29 r30 r31 r32 r33 r34 r35 r36 r37 r38 r39 r40 r41 r42 r43 r44 r45; do all="$all$(run $r)"; done
! grep -q Traceback <<<"$all" || fail "a traceback (run 58 cases)"
echo "test_triage_a3: all checks passed"
