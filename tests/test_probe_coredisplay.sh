#!/bin/bash
# tools/probe/coredisplay.sh's present-mode part (card run 2026-10-17d: WindowServer
# logs "init_page_flip: page flip mode is on" and never uses IOPresentment
# transactions), with a stub dyld_info and a fake disassembly: the keyword strings
# of CoreDisplay / SkyLight / IOPresentment / GPUWrangler, the function that logs
# the mode (found by its string and by the string's address), its callers and their
# callers, and the strings / called functions / preference reads of those. The
# script stays read-only: it writes into its output directory only.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf -- "$T"' EXIT
fail() { echo "test_probe_coredisplay: FAIL $*"; exit 1; }
sh -n "$ROOT/tools/probe/coredisplay.sh" || fail "sh -n"

mkdir -p "$T/bin"
cat > "$T/bin/dyld_info" <<'EOF'
#!/bin/sh
# stub: arguments as coredisplay.sh passes them
for a in "$@"; do
  case $a in
    -platform) echo "platform: macOS"; exit 0 ;;
    -help) echo "dyld_info help"; exit 0 ;;
    -disassemble) cat "$FAKE_DIS"; exit 0 ;;
    -section) printf '0x00001234  init_page_flip: page flip mode is %%s\n0x00001300  isMobile=%%s\n0x00001400  AAPL,slot-name\n0x00001500  TransactionsSupported\n'; exit 0 ;;
    -exports) echo "_CoreDisplay_PageFlipSetup"; exit 0 ;;
  esac
done
EOF
chmod +x "$T/bin/dyld_info"
cat > "$T/fake.dis" <<'EOF'
_init_page_flip:
  0x1000  push rbp
  0x1004  lea rdi, [rip + 0x2000]   ## 0x1234 "init_page_flip: page flip mode is %s"
  0x1008  lea rsi, __CFConstantStringClassReference "AllowPageFlip"
  0x100c  call _CFPreferencesGetAppBooleanValue
  0x1010  call _os_log_impl
_setup_display:
  0x2000  call _init_page_flip
  0x2004  call _CGXGetBooleanForKey
_top_level:
  0x2100  call _setup_display
_unrelated:
  0x3000  ret
EOF
O=$T/out
PATH="$T/bin:$PATH" FAKE_DIS="$T/fake.dis" sh "$ROOT/tools/probe/coredisplay.sh" "$O" > "$T/log.txt" 2>&1 || fail "the script failed: $(tail -5 "$T/log.txt")"

for lib in CoreDisplay SkyLight IOPresentment GPUWrangler; do
    grep -q "^== $lib: " "$O/30-keyword-strings.txt" || fail "no keyword section for $lib"
done
grep -q 'init_page_flip: page flip mode is' "$O/30-keyword-strings.txt" || fail "30: the mode string"
grep -q 'AAPL,slot-name' "$O/30-keyword-strings.txt" || fail "30: AAPL,slot-name"
grep -q 'TransactionsSupported' "$O/30-keyword-strings.txt" || fail "30: TransactionsSupported"
grep -q '_CoreDisplay_PageFlipSetup' "$O/30-keyword-strings.txt" || fail "30: the exports"
for kw in init_page_flip 'page flip mode' MPAccessSurfaceForDisplayDevice isBackBuffered IOFBIntegrated '(IG)' isMobile built-in 'AAPL,slot-name' TransactionsSupported DisplayPipeSupported IOAccelDisplayPipeCapabilities vendorID; do
    # the keyword list the script greps for (escaped parentheses in the regex)
    k=$(printf '%s' "$kw" | sed 's/(/\\(/; s/)/\\)/')
    grep -qF -- "$k" "$ROOT/tools/probe/coredisplay.sh" || fail "keyword [$kw] is not in the script"
done
grep -q '^_init_page_flip:$' "$O/40-page-flip-fn-CoreDisplay.s" || fail "40: the function by its string"
grep -q '^_init_page_flip:$' "$O/40-page-flip-fn-CoreDisplay-by-address.s" || fail "40: the function by the address of the string"
! grep -q '_unrelated' "$O/40-page-flip-fn-CoreDisplay.s" || fail "40: an unrelated function"
grep -q '^_init_page_flip:$' "$O/41-page-flip-function-names.txt" || fail "41: function names"
grep -q '== callers of _init_page_flip: in CoreDisplay.dis' "$O/41-page-flip-callers.s" && grep -q '^_setup_display:$' "$O/41-page-flip-callers.s" || fail "41: callers"
grep -q '^_setup_display:$' "$O/41-page-flip-caller-names.txt" || fail "41: caller names"
grep -q '^_top_level:$' "$O/41-page-flip-callers2.s" || fail "41: the callers' callers"
grep -q '"AllowPageFlip"' "$O/42-page-flip-keys.txt" || fail "42: the CFSTR key"
grep -q '^_CFPreferencesGetAppBooleanValue$' "$O/42-page-flip-keys.txt" || fail "42: a called function"
grep -q '^_CGXGetBooleanForKey$' "$O/42-page-flip-keys.txt" || fail "42: the callers' reads"
grep -q 'CFPreferencesGetAppBooleanValue' "$O/42-page-flip-keys.txt" || fail "42: the preference read line"
grep -q '^## the callers. callers' "$O/42-page-flip-keys.txt" || fail "42: no section for the callers' callers"
ls "$O"/43-fn-CoreDisplay-isMobile.s "$O"/43-fn-GPUWrangler-IOFBIntegrated___IG__.s >/dev/null 2>&1 || fail "43: the integrated / mobile function files: $(ls "$O" | grep '^43')"
[ ! -e "$O/.callers.tmp" ] || fail "a temp file is left in the output"
# read-only on the runner: nothing but the output directory is written
! grep -nE '(^|[^>])>[^>&]*(/System|/Library|/usr|/private|/etc)|\bsudo\b|defaults +write|nvram +[A-Za-z]|rm +-rf? +[^"$ ]' "$ROOT/tools/probe/coredisplay.sh" ||
    fail "the script writes or deletes outside its output directory"
# ---- ECC review of 501eae9: a library whose disassembly or strings are empty is said, per library
! grep -q '^FAILED' "$O/00-system.txt" || fail "FAILED line although every library produced output: $(grep '^FAILED' "$O/00-system.txt")"
[ -f "$O/40-page-flip-fn-SkyLight-by-address.s" ] && [ -f "$O/40-page-flip-fn-CoreDisplay-by-address.s" ] || fail "40-*-by-address.s must exist unconditionally (the workflow reads them under bash -e)"
mkdir -p "$T/bin2"
cat > "$T/bin2/dyld_info" <<'EOF'
#!/bin/sh
# stub for a runner where dyld_info has nothing to say about the libraries (or fails)
for a in "$@"; do
  case $a in
    -platform) echo "platform: macOS"; exit 0 ;;
    -help) echo "dyld_info help"; exit 0 ;;
    -disassemble) echo "dyld_info: no such image in the shared cache" >&2; exit 1 ;;
    -section) echo "dyld_info: no section" >&2; exit 1 ;;
    -exports) exit 0 ;;
  esac
done
EOF
chmod +x "$T/bin2/dyld_info"
O2=$T/out2
PATH="$T/bin2:$PATH" sh "$ROOT/tools/probe/coredisplay.sh" "$O2" > "$T/log2.txt" 2>&1 || fail "the script must not fail itself on empty output: $(tail -3 "$T/log2.txt")"
for lib in CoreDisplay SkyLight IOPresentment GPUWrangler; do
    grep -q "^FAILED: $lib .dis/.cstring empty ($lib.dis), see $lib.dis.err\$" "$O2/00-system.txt" || fail "no FAILED line for $lib .dis: $(grep FAILED "$O2/00-system.txt")"
    grep -q "^FAILED: $lib .dis/.cstring empty ($lib.cstring), see $lib.dis.err\$" "$O2/00-system.txt" || fail "no FAILED line for $lib .cstring"
    grep -q 'dyld_info: no such image' "$O2/$lib.dis.err" || fail "$lib.dis.err lacks dyld_info's complaint"
done
[ -f "$O2/40-page-flip-fn-CoreDisplay-by-address.s" ] || fail "by-address file missing when nothing was found"
# a library that dyld_info -platform refuses
cat > "$T/bin2/dyld_info" <<'EOF'
#!/bin/sh
for a in "$@"; do case $a in -platform) echo "dyld_info: cannot open" >&2; exit 1 ;; esac; done
EOF
O3=$T/out3
PATH="$T/bin2:$PATH" sh "$ROOT/tools/probe/coredisplay.sh" "$O3" > "$T/log3.txt" 2>&1 || fail "the script failed when -platform failed"
grep -q '^FAILED: CoreDisplay dyld_info -platform failed' "$O3/00-system.txt" || fail "no FAILED line for -platform: $(grep FAILED "$O3/00-system.txt")"

# ---- the workflow step, run under bash -e like Actions does: it prints the .err files and
# 00-system.txt, survives missing files, and fails loudly when CoreDisplay.dis is tiny
WF=$ROOT/.github/workflows/probe.yml
grep -q 'if: always()' "$WF" || fail "probe.yml: the artifact upload must run after a failure"
python3 -I - "$WF" "$T/step.sh" <<'PY'
import sys
t = open(sys.argv[1]).read()
job = t.split("  coredisplay:")[1].split("  iofam:")[0]
body = job.split("run: |\n", 1)[1].split("      - uses: actions/upload-artifact", 1)[0]
open(sys.argv[2], "w").write("".join(l[10:] + "\n" for l in body.splitlines()))
PY
grep -q '^sh tools/probe/coredisplay.sh probe-out$' "$T/step.sh" || fail "probe.yml: could not extract the coredisplay step"
sed -i.bak "s#^sh tools/probe/coredisplay.sh probe-out#sh $ROOT/tools/probe/coredisplay.sh probe-out#" "$T/step.sh"
mkdir -p "$T/wf1" "$T/wf2"
# a tiny disassembly: the step fails loudly, after printing what there is
if ( cd "$T/wf1" && PATH="$T/bin:$PATH" FAKE_DIS="$T/fake.dis" bash -eo pipefail "$T/step.sh" ) > "$T/wf1.log" 2>&1; then
    fail "the workflow step passed with a 13-line CoreDisplay.dis"
fi
grep -q '::error::CoreDisplay.dis has 13 lines (< 1000)' "$T/wf1.log" || fail "no loud error: $(tail -3 "$T/wf1.log")"
grep -q '^== present mode: callers' "$T/wf1.log" || fail "the step aborted before printing its sections (a missing file under bash -e?): $(tail -5 "$T/wf1.log")"
grep -q '^== probe-out/CoreDisplay.dis.err' "$T/wf1.log" || fail "the .err files are not printed"
# a real-size disassembly passes, also when the by-address file would be missing
{ cat "$T/fake.dis"; i=0; while [ $i -lt 1100 ]; do echo "  0x$i  nop"; i=$((i + 1)); done; } > "$T/big.dis"
( cd "$T/wf2" && PATH="$T/bin:$PATH" FAKE_DIS="$T/big.dis" bash -eo pipefail "$T/step.sh" ) > "$T/wf2.log" 2>&1 || fail "the workflow step failed with a 1100-line CoreDisplay.dis: $(tail -5 "$T/wf2.log")"
# the old step did `head` of a file that exists only when an address was found: a missing file is said, not fatal
sed -i.bak 's#^show "present mode: the same, found by the address of the string" probe-out/40-page-flip-fn-CoreDisplay-by-address.s#show "gone" probe-out/does-not-exist.s#' "$T/step.sh"
( cd "$T/wf2" && PATH="$T/bin:$PATH" FAKE_DIS="$T/big.dis" bash -eo pipefail "$T/step.sh" ) > "$T/wf3.log" 2>&1 || fail "a missing file aborted the step"
grep -q '^== gone: MISSING probe-out/does-not-exist.s' "$T/wf3.log" || fail "a missing file is not named"
echo "test_probe_coredisplay: ok"
