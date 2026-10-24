"""fb/Makefile's FB_SRC_HASH and agdc/Makefile's AGDC_SRC_HASH inputs must be the
same files in the same order as tools/triage_reports.py's FB_HASH_FILES and
AGDC_HASH_FILES: the STALE KEXTS check hashes the checked-out sources the way the
Makefiles do and compares with the build ids the kexts print (like
tests/test_accel_hash_parity.py for the accelerator).
Run: python3 -I tests/test_fb_agdc_hash_parity.py"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import triage_reports  # noqa: E402

CASES = (   # directory, Makefile variable, triage tuple
    ("fb", "FB_SRC_HASH", "FB_HASH_FILES"),
    ("agdc", "AGDC_SRC_HASH", "AGDC_HASH_FILES"),
)
total = 0
for d, var, name in CASES:
    mk = open(os.path.join(ROOT, d, "Makefile")).read()
    m = re.search(r"^%s\s*:=\s*\$\(shell cat (.*?) \|" % var, mk, re.M)
    if not m:
        sys.exit("test_fb_agdc_hash_parity: FAIL no %s := $(shell cat ... | ...) in %s/Makefile" % (var, d))
    make_files = tuple(os.path.normpath(os.path.join(d, f)) for f in m.group(1).split())
    want = tuple(getattr(triage_reports, name))
    if make_files != want:
        sys.exit("test_fb_agdc_hash_parity: FAIL %s/Makefile hashes %s, triage %s = %s" % (d, make_files, name, want))
    for f in make_files:
        if not os.path.exists(os.path.join(ROOT, f)):
            sys.exit("test_fb_agdc_hash_parity: FAIL %s does not exist" % f)
    # every repository file the hashed sources #include "..." (transitively)
    # is hashed too: an edit to a missing header would not change the build id
    seen, todo = set(), [f for f in make_files if f.endswith((".cpp", ".c", ".h"))]
    while todo:
        f = todo.pop()
        if f in seen:
            continue
        seen.add(f)
        for inc in re.findall(r'^\s*#\s*include\s+"([^"]+)"', open(os.path.join(ROOT, f)).read(), re.M):
            g = os.path.normpath(os.path.join(os.path.dirname(f), inc))
            if not os.path.exists(os.path.join(ROOT, g)):
                continue   # an SDK header found through -I, not ours
            if g not in make_files:
                sys.exit("test_fb_agdc_hash_parity: FAIL %s includes %s, which %s/Makefile's %s does not hash" % (f, g, d, var))
            todo.append(g)
    # and the hash is computable from them (a None would silence the stale check)
    if not triage_reports.fb_source_hash(want):
        sys.exit("test_fb_agdc_hash_parity: FAIL fb_source_hash(%s) is None" % name)
    total += len(make_files)
print("test_fb_agdc_hash_parity: all checks passed (%d files)" % total)
