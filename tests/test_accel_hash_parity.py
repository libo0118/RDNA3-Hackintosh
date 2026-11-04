"""accel/Makefile's ACCEL_SRC_HASH inputs and tools/triage_reports.py's
ACCEL_HASH_FILES must be the same files in the same order: triage computes
the accelerator's build id from them (card run 58 added two common/
headers to both). Run: python3 -I tests/test_accel_hash_parity.py"""
import os
import posixpath
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import triage_reports  # noqa: E402

mk = open(os.path.join(ROOT, "accel", "Makefile")).read()
m = re.search(r"^ACCEL_SRC_HASH\s*:=\s*\$\(shell cat (.*?) \|", mk, re.M)
if not m:
    sys.exit("test_accel_hash_parity: FAIL no ACCEL_SRC_HASH := $(shell cat ... | ...) in accel/Makefile")
make_files = tuple(posixpath.normpath(posixpath.join("accel", f)) for f in m.group(1).split())
if make_files != tuple(triage_reports.ACCEL_HASH_FILES):
    sys.exit("test_accel_hash_parity: FAIL accel/Makefile hashes %s, triage %s" %
             (make_files, triage_reports.ACCEL_HASH_FILES))
if "accel/iofam/iofam_types.h" not in make_files:
    sys.exit("test_accel_hash_parity: FAIL enum declarations are missing from the accelerator build id")
for f in make_files:
    if not os.path.exists(os.path.join(ROOT, f)):
        sys.exit("test_accel_hash_parity: FAIL %s does not exist" % f)
print("test_accel_hash_parity: all checks passed (%d files)" % len(make_files))
