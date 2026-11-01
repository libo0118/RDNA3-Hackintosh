# tests/split_parts.sh - sourced by the tests that `make test` runs in parts (test_bisect_step.sh, test_ladder_step.sh,
# test_apps_step.sh): they cost minutes on the macOS runner and one script was the slowest test of the whole run.
#
# The script sets PARTS="name name ..." and sources this file (after ROOT, before mktemp). Then:
#   tests/test_x.sh            no argument: every part, one after the other, each in its own process (own mktemp dir)
#   tests/test_x.sh --parts    print the part names (the Makefile asks, so the lists cannot drift apart)
#   tests/test_x.sh PART       only that part; `make test` has one target per part (t.test_x.PART) and runs them in parallel
# In a part the script asks `in_part NAME` for each section; a section that is not wanted is skipped, the shared set-up (stubs,
# helper functions) runs in every part. A part shares nothing with another one: its own mktemp dir, nothing left behind.
# `in_part a b` is true if the part is any of the names. (Not called `want`: test_bisect_step.sh has a function of that name.)
# bash 3.2 (macOS), no bash 4 features.
SELF=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
SELF_NAME=$(basename "$0" .sh)
if [ "${1:-}" = --parts ]; then echo $PARTS; exit 0; fi
if [ $# -eq 0 ]; then
    for p in $PARTS; do
        bash "$SELF" "$p" || { echo "$SELF_NAME: FAIL in part $p"; exit 1; }
    done
    echo "$SELF_NAME: ok (all parts: $PARTS)"
    exit 0
fi
PART=$1
case " $PARTS " in
    *" $PART "*) ;;
    *) echo "$SELF_NAME: unknown part '$PART' (parts: $PARTS)" >&2; exit 2 ;;
esac
in_part() {
    local p
    for p in "$@"; do [ "$PART" = "$p" ] && return 0; done
    return 1
}
