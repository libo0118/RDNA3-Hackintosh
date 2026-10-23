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
echo "test_probe_coredisplay: ok"
