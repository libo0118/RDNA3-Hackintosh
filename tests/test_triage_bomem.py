#!/usr/bin/env python3
"""tools/triage_reports.py's BO mapping rows (DRIVER-PLAN N47 (b)):
- bomem_rows: the 2026-10-25 kext's RDNA3BoMemory::doMap / doUnmap line (a
  fixed string: that kext no longer exists in the tree) as FAIL rows, and the
  current kext's 'failed in xnu' (FAIL) and 'CPU map of offset ... refused'
  (WARN) lines, built from kext/src/RDNA3Drm.cpp's own LOG format strings, so a
  change there that triage no longer reads fails here;
- bomap_counter_lines: debug.rdna3_vram's bomap_* counters, whose names must
  all be in RDNA3Platform.cpp's vramReport format."""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import triage_reports as tr  # noqa: E402

fails = []
src = open(os.path.join(ROOT, "kext", "src", "RDNA3Drm.cpp")).read()


def log_format(start, follow):
    """the concatenated string literals of the LOG( whose format starts with 'start', up to 'follow'"""
    m = re.search(r'LOG\(("' + re.escape(start) + r'.*?)\s*,\s*' + follow, src, re.S)
    if not m:
        fails.append("no LOG format '%s' in kext/src/RDNA3Drm.cpp" % start)
        return ""
    return "RDNA3: " + "".join(re.findall(r'"((?:\\.|[^"\\])*)"', m.group(1)))


def fill(fmt, *vals):
    return "kernel: " + re.sub(r"%(llx|llu|u|x|d|s)", "{}", fmt).format(*vals)


old = "RDNA3: GEM: RDNA3BoMemory::%s called (%d such): kIOReturnUnsupported, this xnu maps a sub-range " \
      "other than through its parent (BO 0x%llx+%llu KiB, options 0x%x, length 0x%llx; DRIVER-PLAN N47)"
r = tr.bomem_rows("\n".join(["kernel: RDNA3: something else", fill(old, "doMap", 1, "0x100000", 64, "0x4", "0x10000"),
                             fill(old, "doMap", 8, "100000", 64, "4", "10000"),
                             fill(old, "doUnmap", 1, "100000", 64, "4", "10000")]))
if [(x[0], x[2].split(" called")[0]) for x in r] != [("FAIL", "RDNA3BoMemory::doMap"),
                                                     ("FAIL", "RDNA3BoMemory::doUnmap")]:
    fails.append("old calls: %r" % (r,))
elif "at least 8" not in r[0][2]:
    fails.append("the highest count: %r" % (r,))

ffmt = log_format("GEM: CPU %s of a %llu KiB %s BO at 0x%llx failed in xnu", "what")
nfmt = log_format("GEM: CPU map of offset 0x%llx refused: %s (pid %d, %u such)%s", r"\(unsigned long long\)off")
rfmt = log_format("GEM: CPU map of offset 0x%llx refused: %s (BO %llu KiB", r"\(unsigned long long\)off")
dfmt = log_format("GEM: CPU map of offset 0x%llx: no descriptor for the", r"\(unsigned long long\)off")
lfmt = log_format("GEM: a CPU mapping of a %llu KiB BO at 0x%llx went after the DRM teardown",
                  r"\(unsigned long long\)\(size")
if ffmt and nfmt and rfmt and dfmt and lfmt:
    log = "\n".join([   # values of %llx / %x without their 0x: the format has it
        fill(ffmt, "map", 64, "VRAM", "2000000", "e00002bc", 501, 1),
        fill(ffmt, "map", 64, "VRAM", "2000000", "e00002bc", 501, 3),
        fill(ffmt, "unmap", 4, "doorbell", "3000", "e00002c2", 502, 1),
        fill(nfmt, "0", "no BO of this file at that offset", 77, 5, "; offset 0 is what ..."),
        fill(nfmt, "100003000", "no BO of this file at that offset", 77, 6, ""),
        fill(rfmt, "100001000", "a userptr BO (amdgpu_gem_object_mmap: -EPERM)", 0, "0", "2", "0", 256, 2048,
             78, 2),
        fill(dfmt, "100005000", 64, "VRAM", "8000000", "the BAR has no physical segment at the BO's offset", "0", 0,
             79, 1),
        fill(lfmt, 64, "8000000", 3),
    ])
    rows = tr.bomem_rows(log)
    got = [(lv, t.split(" (")[0]) for lv, _, t in rows]
    want = [("FAIL", "xnu refused a CPU map of a VRAM BO at least 3 time(s)"),
            ("FAIL", "xnu refused a CPU unmap of a doorbell BO at least 1 time(s)"),
            ("WARN", "map / unmap requests for offset 0"),
            ("WARN", "map / unmap requests for offsets that name no BO of the file"),
            ("WARN", "a CPU map refused: a userptr BO"),
            ("FAIL", "no mapping descriptor for a VRAM BO, at least 1 time(s): the BAR has no physical segment at "
                     "the BO's offset"),
            ("WARN", "at least 3 CPU mapping(s) went after the DRM teardown")]
    if got != want:
        fails.append("new lines: %r" % (rows,))
if tr.bomem_rows("kernel: RDNA3: something else") or tr.bomem_rows("") or tr.bomem_rows(None):
    fails.append("no call: rows")
tsrc = open(os.path.join(ROOT, "tools", "triage_reports.py")).read()
if not re.search(r"klog = kernel_text\(p, d\)\n(.*\n){0,8}.*bomem_rows\(klog\)", tsrc):
    fails.append("bomem_rows is not fed kernel_text(p, d)")
if "out.extend(bomap_counter_lines(c))" not in tsrc:
    fails.append("vram_heap_lines does not print bomap_counter_lines")

# the counters: names in the kext's report, parsed from a dump, verdicts
psrc = open(os.path.join(ROOT, "kext", "src", "RDNA3Platform.cpp")).read()
for k in tr.BOMAP_KEYS:
    if not re.search(r"\b%s=%%" % k, psrc):
        fails.append("vramReport has no '%s=' (RDNA3Platform.cpp)" % k)
c = tr.vram_heap_counters("debug.rdna3_vram: visible_kib=1 refused=0 bomap_live=3 bomap_live_kib=192 bomap_made=9 "
                          "bomap_reused=4 bomap_refused=0 bomap_quota=0 bomap_nodesc=0 bomap_nobo=0 bomap_fail=0 "
                          "kernel_refused_kib=0")
lines = tr.bomap_counter_lines(c)
if [lv for lv, _ in lines] != ["INFO"] or "3 descriptor(s) live holding 192 KiB" not in lines[0][1]:
    fails.append("clean counters: %r" % (lines,))
c.update(bomap_fail=2, bomap_nobo=7, bomap_refused=3, bomap_quota=2, bomap_nodesc=1)
if [lv for lv, _ in tr.bomap_counter_lines(c)] != ["INFO", "FAIL", "WARN", "FAIL", "WARN"] or \
        "2 by the uid's quota" not in tr.bomap_counter_lines(c)[-1][1]:
    fails.append("bad counters: %r" % (tr.bomap_counter_lines(c),))
old_dump = tr.vram_heap_counters("debug.rdna3_vram: visible_kib=1 refused=0 kernel_refused_kib=0")
if [lv for lv, _ in tr.bomap_counter_lines(old_dump)] != ["WARN"]:
    fails.append("an old kext's dump: %r" % (tr.bomap_counter_lines(old_dump),))
if tr.bomap_counter_lines(None):
    fails.append("no dump: rows (vram_heap_lines reports the missing dump itself)")

if fails:
    for f in fails:
        print("test_triage_bomem: FAIL " + f)
    sys.exit(1)
print("test_triage_bomem: ok")
