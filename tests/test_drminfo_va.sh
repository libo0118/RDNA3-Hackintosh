#!/bin/sh
# tests/test_drminfo_va.sh BUILD_DIR - two jobs in one process each get their own VA (card run 2026-10-12):
# rdna3_drminfo on the loopback must not report GEM_VA. (No `set -e`: a failing run of the tool itself is not
# what is checked here, as before; a GEM_VA line is.)
B=${1:-build/host}
! "$B/rdna3_drminfo_host" --dma-test --gfx-test 2>&1 | grep -q 'GEM_VA'
