#!/bin/bash
# tools/triage_reports.py's "VISIBLE VRAM" lines (card run 2026-10-17c: the
# CPU-visible heap filled with GEM BOs, WindowServer's framebuffers were
# refused) on synthetic reports: the accelerator refused (FAIL), only GEM BOs
# fell back to GTT (WARN), a lost range (FAIL), a clean run (PASS), the dump
# only in the archive's 38-vram-heap.txt, the dump missing on a kext that
# has the heap lines (WARN, loud), and a kext before the report with
# framebuffer refusals in the log (WARN).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf -- "$T"' EXIT
fail() { echo "test_triage_vram: FAIL $*"; exit 1; }

# the core's line, as vramReport() prints it: the given owner/refusal fields over zeros
vline() {   # vline key=value ...
    python3 -I - "$@" <<'PY'
import sys
keys = ["visible_kib=147456", "reserve_kib=73728", "reserve_arg=-1", "refused=0", "refused_accel=0", "refused_gem=0",
        "refused_hi=0", "reserve_blocked=0", "hi_full=0", "hi_clear_fail=0", "lost_kib=0", "free_fail=0", "free_fail_kib=0",
        "retired_n=0", "retired_kib=0", "refused_other=0", "gtt_fail=0", "gtt_fail_kib=0"]
for p in ("acc", "gem", "hi"):
    keys += ["free_%s_kib=0" % p, "largest_%s_kib=0" % p, "nfree_%s=1" % p, "full_skips_%s=0" % p]
for o in ("accel", "fb_surface", "fb_cursor", "swap_d", "gem", "gem_hi", "kernel"):
    keys += ["%s_kib=0" % o, "%s_n=0" % o, "%s_peak_kib=0" % o, "%s_refused=0" % o, "%s_refused_kib=0" % o]
d = dict(k.split("=") for k in keys)
for a in sys.argv[1:]:
    k, v = a.split("=")
    d[k] = v
print(" ".join("%s=%s" % kv for kv in d.items()))
PY
}

mk() {   # mk <dir/name> <tests-file dump line or ""> <38-vram-heap.txt content or ""> <kernel log>
    mkdir -p "$T/$(dirname "$1")"
    python3 -I - "$T/$1" "$2" "$3" "$4" <<'PY'
import io, sys, tarfile
path, dump, arc, log = sys.argv[1:5]
members = {
    "01-boot-args.txt": "boot-args rdna3lvl=8 rdna3fb=1 rdna3accel=1 rdna3agdc=3\n",
    "03-kexts.txt": "RDNA3Accel\n",
    "11-ioreg-props.txt": "",
    "20-log-rdna3.txt": log.replace("\\n", "\n") + "\n",
    "24-log-coredisplay.txt": "",
}
if arc:
    members["38-vram-heap.txt"] = arc.replace("\\n", "\n") + "\n"
with tarfile.open(path + ".tar.gz", "w:gz") as t:
    for n, body in members.items():
        data = body.encode()
        ti = tarfile.TarInfo("rdna3-report/" + n)
        ti.size = len(data)
        t.addfile(ti, io.BytesIO(data))
if dump:
    open(path + "-tests.txt", "w").write("== A3 display counters after M6\n" + dump + "\n")
PY
}
run() { python3 "$ROOT/tools/triage_reports.py" "$T/$1" 2>&1 || true; }
has() { grep -qF -- "$2" <<<"$1" || fail "$3: '$2' not in: $1"; }
hasnt() { ! grep -qF -- "$2" <<<"$1" || fail "$3: '$2' unexpected in: $1"; }
LOG="RDNA3: DRM/amdgpu UAPI ready: VRAM 16368 MiB (256 MiB visible), user heap 144 MiB visible + 15856 MiB beyond BAR0; of the visible part 96 MiB are the accelerator's reserve (default)"

# the accelerator refused: the display
mk acc/rdna3-report-acc "debug.rdna3_vram: $(vline refused=6 refused_accel=4 refused_gem=2 accel_kib=73728 accel_n=9 accel_peak_kib=73728 accel_refused=4 accel_refused_kib=32400 gem_kib=71000 gem_n=3 gem_peak_kib=71000 gem_refused=2 free_gem_kib=1024 largest_gem_kib=512 reserve_blocked=2)" "" "$LOG"
o=$(run acc)
hasnt "$o" Traceback acc
has "$o" 'FAIL VISIBLE VRAM: VISIBLE VRAM exhausted: 6 refusals (4 of the accelerator' acc
has "$o" 'owners accel 73728 KiB x9 (peak 73728 KiB, refused 4), gem 71000 KiB x3 (peak 71000 KiB, refused 2)' acc
has "$o" '2 GEM request(s) were held off by the reserve' acc
grep -q 'VISIBLE VRAM  *FAIL VISIBLE VRAM exhausted: 6 refusals' <<<"$o" || fail "acc: the summary line: $o"
grep -q '^RESULT: something failed' <<<"$o" || fail "acc: the result: $o"

# only GEM BOs: they fall back to GTT, the display is fine
mk gem/rdna3-report-gem "debug.rdna3_vram: $(vline refused=3 refused_gem=3 gem_kib=60000 gem_n=2 gem_peak_kib=70000 gem_refused=3 accel_kib=30000 accel_n=4 accel_peak_kib=30000)" "" "$LOG"
o=$(run gem)
has "$o" 'WARN VISIBLE VRAM: VISIBLE VRAM exhausted: 3 refusals (0 of the accelerator' gem
hasnt "$o" 'FAIL VISIBLE VRAM' gem

# clean
mk ok/rdna3-report-ok "debug.rdna3_vram: $(vline accel_kib=30000 accel_n=4 accel_peak_kib=30000)" "" "$LOG"
o=$(run ok)
has "$o" 'PASS VISIBLE VRAM: no refusals (0): the accelerator'"'"'s reserve is 73728 KiB of the 147456 KiB visible heap; held at the end: accel 30000 KiB x4 (peak 30000 KiB)' ok

# a lost range and a full free list, a BO that went to GTT beyond BAR0
mk lost/rdna3-report-lost "debug.rdna3_vram: $(vline lost_kib=15872000 nfree_hi=4096 full_skips_hi=3 refused=1 refused_hi=1 hi_full=1 free_hi_kib=252472 largest_hi_kib=60 gem_hi_refused=1)" "" "$LOG"
o=$(run lost)
has "$o" 'FAIL VISIBLE VRAM: the heap lost 15872000 KiB: a free list was full when a range came back' lost
has "$o" 'WARN VISIBLE VRAM: 3 allocation attempt(s) skipped a free range only because the free list was full' lost
has "$o" 'WARN VISIBLE VRAM: 1 BO(s) without CPU access found no room beyond BAR0 (free 252472 KiB, largest 60 KiB' lost

# only in the archive's 38-vram-heap.txt (collect.sh without autoreport)
mk arc/rdna3-report-arc "" "== sysctl debug.rdna3_vram (the end of the run)\ndebug.rdna3_vram: $(vline refused=1 refused_accel=1 accel_refused=1 accel_kib=1 accel_n=1 accel_peak_kib=1)" "$LOG"
o=$(run arc)
has "$o" 'FAIL VISIBLE VRAM: VISIBLE VRAM exhausted: 1 refusals (1 of the accelerator' arc

# the kext has the heap lines but no dump reached the report: loud
mk missing/rdna3-report-missing "" "" "$LOG\nRDNA3: VRAM REFUSED #1: accelerator allocation, 8100 KiB by accel"
o=$(run missing)
has "$o" 'WARN VISIBLE VRAM: not measured: debug.rdna3_vram is in neither the tests file nor 38-vram-heap.txt' missing
has "$o" "(1 'VRAM REFUSED' line(s)" missing
# collect.sh ran and its file holds no dump: loud whatever the log says (here a log without any heap line)
mk nodump/rdna3-report-nodump "" "(no debug.rdna3_vram sysctl: kext older than card run 2026-10-17c, or not loaded)" "RDNA3: hello"
o=$(run nodump)
has "$o" 'WARN VISIBLE VRAM: not measured: 38-vram-heap.txt has no debug.rdna3_vram dump - (no debug.rdna3_vram sysctl: kext older' nodump

# the log has more refusals than the dump says; the log's own refusal lines pass through
mk stale/rdna3-report-stale "debug.rdna3_vram: $(vline refused=1 refused_gem=1 gem_refused=1)" "" "$LOG\nRDNA3: VRAM REFUSED #5: GEM BO the CPU may map, 36992 KiB by gem; held: gem 1 KiB x1 (peak 1 KiB, refused 1); alignment 2048 KiB\nRDNA3: VRAM REFUSED #6: accelerator allocation, 8100 KiB by accel; held: nothing"
o=$(run stale)
has "$o" 'WARN VISIBLE VRAM: log shows 6 refusals, newest dump says 1' stale
has "$o" 'INFO VISIBLE VRAM: log: VRAM REFUSED #5: GEM BO the CPU may map, 36992 KiB by gem' stale
has "$o" 'INFO VISIBLE VRAM: log: VRAM REFUSED #6: accelerator allocation, 8100 KiB by accel' stale

# a dump cut before its last key, a heap of 0 KiB
mk cut/rdna3-report-cut "debug.rdna3_vram: $(vline | sed 's/ kernel_n=.*//')" "" "$LOG"
o=$(run cut)
has "$o" 'WARN VISIBLE VRAM: dump truncated: debug.rdna3_vram ends before kernel_refused_kib' cut
mk zero/rdna3-report-zero "debug.rdna3_vram: $(vline visible_kib=0 reserve_kib=0)" "" "$LOG"
o=$(run zero)
has "$o" 'WARN VISIBLE VRAM: visible_kib=0: the core reports no CPU-visible heap' zero

# a failed free, a retired range, a refused kernel BO, a GTT fallback that failed, an early refusal
mk free/rdna3-report-free "debug.rdna3_vram: $(vline free_fail=2 free_fail_kib=64 retired_n=3 retired_kib=192 refused=1 kernel_refused=1 refused_other=4 gtt_fail=5 gtt_fail_kib=40 kernel_peak_kib=64)" "" "$LOG"
o=$(run free)
has "$o" 'FAIL VISIBLE VRAM: 2 free(s) of 64 KiB were refused by the heap' free
has "$o" 'WARN VISIBLE VRAM: 3 range(s), 192 KiB, retired for good' free
has "$o" 'FAIL VISIBLE VRAM: 1 kernel-internal BO(s) (seq64, user queue objects) refused' free
has "$o" 'WARN VISIBLE VRAM: 5 client BO(s), 40 KiB, found no VRAM and no GTT either' free
has "$o" 'WARN VISIBLE VRAM: 4 allocation(s) refused before the heaps were asked' free
hasnt "$o" 'went to GTT' free

# a kext before the report: the framebuffer refusals still say something
mk old/rdna3-report-old "" "" "RDNA3: DRM/amdgpu UAPI ready: VRAM 16368 MiB (256 MiB visible), user heap 144 MiB visible + 15856 MiB\nRDNA3Accelerator: RD3VidMemory::allocPhysical(<ptr>): 8294400 bytes REFUSED by the framebuffer (0xe00002db)"
o=$(run old)
has "$o" 'WARN VISIBLE VRAM: 1 accelerator VRAM allocation(s) refused by the framebuffer, no debug.rdna3_vram' old

# a kext before the report and nothing refused: no line
mk none/rdna3-report-none "" "" "RDNA3: DRM/amdgpu UAPI ready: VRAM 16368 MiB (256 MiB visible), user heap 144 MiB visible + 15856 MiB"
o=$(run none)
hasnt "$o" 'VISIBLE VRAM' none
echo "test_triage_vram: all checks passed"
