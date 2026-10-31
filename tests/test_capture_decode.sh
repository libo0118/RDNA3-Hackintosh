#!/bin/sh
# tests/test_capture_decode.sh BUILD_DIR - the C capture writer and the Python decoder must agree: test_capture
# writes a sample, tools/capture_decode.py --check reads it back (they cannot drift apart). The sample is
# private to this test.
set -eu
B=${1:-build/host}
f=$B/capture_sample.bin
set -x
"$B/test_capture" "$f"
python3 tools/capture_decode.py --check "$f"
