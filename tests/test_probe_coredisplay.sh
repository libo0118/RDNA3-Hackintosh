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
    -section)
      case "$*" in
        *__cfstring*) cat <<'CFS'
7FF80405B000  0000000000000000 00000000000007C8 00007FF8040A0000 0000000000000009
7FF80405B020  0000000000000000 00000000000007C8 00007FF8040A0010 0000000000000007
7FF80405B040  0000000000000000 00000000000007C8 00007FF8040A0030 000000000000000e
CFS
        ;;
        *__const*) cat <<'CST'
7FF804059000  0000000000000000 0000000000000000 00007FF804058100 0000000000000000
CST
        ;;
        *) printf '0x00001234  init_page_flip: page flip mode is %%s\n0x00001300  isMobile=%%s\n0x00001400  AAPL,slot-name\n0x00001500  TransactionsSupported\n0x00001600  displayPipeIsAvailable\n0x00001700  IOPRESENTMENT_CAPABILITIES_AGDCEXTENDED_MISSING\n0x00001800  DisplayPipeSupported\n0x00001900  AGDC Extended Capabilities not found\n0x00001b00  ioPresentmentFindAGDCMatch\n0x00007FF8040A0000  vendor-id\n0x00007FF8040A0010  IOClass\n0x00007FF8040A0030  useIOPOverride\n' ;;
      esac
      exit 0 ;;
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
__ZN11CoreDisplay16UseIOPresentmentEv:
  0x4000  push rbp
  0x4004  call __ZN11CoreDisplay20DisplayPipeAvailableEv
  0x4008  call _CFPreferencesGetAppBooleanValue
  0x400c  lea rdi, [rip + 0x10]   ## 0x1a00 "IOPresentmentSwitch"
  0x4010  call _isMobileSystem
  0x4014  jmp __ZN11CoreDisplay15BigHelperEv
__ZN11CoreDisplay20DisplayPipeAvailableEv:
  0x4100  lea rdi, [rip + 0x20]   ## 0x1600 "displayPipeIsAvailable"
  0x4104  call _pipe_probe_leaf
  0x4108  ret
_pipe_probe_leaf:
  0x4200  mov eax, 1
  0x4204  ret
_isMobileSystem:
  0x4500  lea rdi, [rip + 0x30]   ## "hw.model"
  0x4504  call _sysctlbyname_wrapper
_sysctlbyname_wrapper:
  0x4600  ret
__ZN11CoreDisplay18InitializeDisplayPipesEv:
  0x5000  lea rax, [rip + 0x40]   ## 0x00001700
  0x5004  call _pipe_probe_leaf
_decoy_longer_address:
  0x5100  lea rax, [rip + 0x50]   ## 0x17000
-[IOPresentmentDisplayPipe copyCapabilities]:
  0x6000  lea rdi, [rip + 0x60]   ## 0x1800 "DisplayPipeSupported"
  0x6004  call _ioPresentmentFindAGDCMatch
_ioPresentmentFindAGDCMatch:
  0x6100  lea rdi, [rip + 0x70]   ## "AGDC Extended Capabilities not found"
  0x6104  ret
EOF
# a callee of UseIOPresentment that is longer than the 62-line "small" limit: its own callee must not be listed
{ echo "__ZN11CoreDisplay15BigHelperEv:"; echo "  0x4300  call _big_leaf_never_shown"
  i=0; while [ $i -lt 90 ]; do echo "  0x43$i  nop"; i=$((i + 1)); done
  echo "  0x4308  ret"; echo "_big_leaf_never_shown:"; echo "  0x4400  ret"; } >> "$T/fake.dis"
# probe run 10, in the real dyld_info format: mangled labels at column 0, indented instructions with
# an address, globals in a "; name" comment, a CFSTR reference as a bare "; " (or no comment at all)
cat >> "$T/fake.dis" <<'REAL'
__ZL19GetSystemGPUVendorsv:
    7FF804058000 pushq %rbp
    7FF804058001 cmpq $-1, __ZZL19GetSystemGPUVendorsvE4pred(%rip) ; __ZZL19GetSystemGPUVendorsvE4pred
    7FF804058008 leaq 0xff1(%rip), %rsi ; ___block_literal_global.197.5218
    7FF80405800F callq _dispatch_once
    7FF804058014 retq
___ZL19GetSystemGPUVendorsv_block_invoke:
    7FF804058200 pushq %rbp
    7FF804058201 callq _classify_vendor
    7FF804058206 retq
_classify_vendor:
    7FF804058300 leaq 0x10(%rip), %rax ; string literal: "IOPresentment"
    7FF804058307 retq
_gpu_class_blk:
    7FF804058100 pushq %rbp
    7FF804058101 leaq 0x2ef8(%rip), %rdi
    7FF804058108 callq _IORegistryEntrySearchCFProperty
    7FF80405810D leaq 0x2f0c(%rip), %rsi ; 
    7FF804058114 callq _vendor_leaf
    7FF804058119 retq
_vendor_leaf:
    7FF804058400 retq
__ZN11CoreDisplay9InitFlagsEv:
    7FF80405C000 leaq -0xfc7(%rip), %rdi
    7FF80405C007 callq _CFPreferencesGetAppBooleanValue
    7FF80405C00C movb %al, __ZN11CoreDisplayL6useIOPE(%rip) ; __ZN11CoreDisplayL6useIOPE
    7FF80405C012 movb %al, __ZN11CoreDisplayL12useIOP_onAMDE(%rip) ; __ZN11CoreDisplayL12useIOP_onAMDE
    7FF80405C018 callq _read_default_helper
    7FF80405C01D leaq 0x5000(%rip), %rax
    7FF80405C024 retq
_read_default_helper:
    7FF80405C100 lea rsi, [rip - 0x10e7]
    7FF80405C107 retq
__ZN24IOPresentmentDisplayPipe18_IOPresentmentInitEP15__IOPresentment:
    7FF80405D000 retq
REAL
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
# ---- the switch (50-..54- files): CoreDisplay::UseIOPresentment and what feeds it
has() { grep -q -- "$2" "$O/$1" || fail "$1: lacks [$2]: $(head -5 "$O/$1")"; }
hasnt() { ! grep -q -- "$2" "$O/$1" || fail "$1: must not contain [$2]"; }
has 50-UseIOPresentment.s '^__ZN11CoreDisplay16UseIOPresentmentEv:$'
hasnt 50-UseIOPresentment.s '_unrelated'
hasnt 50-UseIOPresentment.s '^_init_page_flip:$'
# callees, one level: the local ones (call and tail jmp), not the external ones
has 50-UseIOPresentment-callees.s '^== __ZN11CoreDisplay20DisplayPipeAvailableEv$'
has 50-UseIOPresentment-callees.s '^== _isMobileSystem$'
has 50-UseIOPresentment-callees.s '^== __ZN11CoreDisplay15BigHelperEv$'
hasnt 50-UseIOPresentment-callees.s 'truncated at'
hasnt 50-UseIOPresentment-callees.s '^== _CFPreferencesGetAppBooleanValue$'
hasnt 50-UseIOPresentment-callees.s '^== __ZN11CoreDisplay16UseIOPresentmentEv$'
# second level: only the callees of small callees
has 50-UseIOPresentment-callees2.s '^== _pipe_probe_leaf$'
has 50-UseIOPresentment-callees2.s '^== _sysctlbyname_wrapper$'
hasnt 50-UseIOPresentment-callees2.s '_big_leaf_never_shown'
hasnt 50-UseIOPresentment-callees2.s '^== __ZN11CoreDisplay20DisplayPipeAvailableEv$'
# strings, calls and reads of the switch
has 50-UseIOPresentment-keys.txt '"IOPresentmentSwitch"'
has 50-UseIOPresentment-keys.txt '^_CFPreferencesGetAppBooleanValue$'
has 50-UseIOPresentment-keys.txt '^_pipe_probe_leaf$'
# the users of the capability strings, by the text of the string and by its address (also zero-padded)
has 51-CoreDisplay-str-displayPipeIsAvailable.s '^__ZN11CoreDisplay20DisplayPipeAvailableEv:$'
hasnt 51-CoreDisplay-str-displayPipeIsAvailable.s '^_unrelated:$'
has 51-CoreDisplay-str-IOPRESENTMENT_CAPABILITIES_AGDCEXTENDED_MISSING.s '^__ZN11CoreDisplay18InitializeDisplayPipesEv:$'
hasnt 51-CoreDisplay-str-IOPRESENTMENT_CAPABILITIES_AGDCEXTENDED_MISSING.s '_decoy_longer_address'
has 52-CoreDisplay-InitializeDisplayPipes.s '^__ZN11CoreDisplay18InitializeDisplayPipesEv:$'
# a symbol that is not in the disassembly: a "not found" file, not a failure
has 52-CoreDisplay-DisplayPipeForService.s '^not found: CoreDisplay::DisplayPipeForService'
has 53-IOPresentment-Init.s '^__ZN24IOPresentmentDisplayPipe18_IOPresentmentInitEP15__IOPresentment:$'
# the IOPresentment capability path (an ObjC method label counts as a function boundary)
has 53-IOPresentment-CopyCapabilities.s '^-\[IOPresentmentDisplayPipe copyCapabilities\]:$'
hasnt 53-IOPresentment-CopyCapabilities.s '^_ioPresentmentFindAGDCMatch:$'
# a string, not a function: found by its text (the call) and its address in the cstring table
has 53-IOPresentment-str-ioPresentmentFindAGDCMatch.s '^-\[IOPresentmentDisplayPipe copyCapabilities\]:$'
has 53-IOPresentment-str-DisplayPipeSupported.s '^-\[IOPresentmentDisplayPipe copyCapabilities\]:$'
has 53-IOPresentment-str-TransactionsSupported.s '^not found: no function of IOPresentment.dis uses "TransactionsSupported"'
has 53-IOPresentment-str-AGDC_Extended_Capabilities_not_found.s '^_ioPresentmentFindAGDCMatch:$'
# isMobileSystem, whole, and its callee
has 54-isMobileSystem.s '^_isMobileSystem:$'
hasnt 54-isMobileSystem.s '^__ZN11CoreDisplay16UseIOPresentmentEv:$'
has 54-isMobileSystem-callees.s '^== _sysctlbyname_wrapper$'
has 43-fn-CoreDisplay-isMobile.s '^__ZN11CoreDisplay16UseIOPresentmentEv:$'
# ---- probe run 10: GetSystemGPUVendors, the useIOP flags, the CFSTR resolver
has 55-GetSystemGPUVendors.s '^__ZL19GetSystemGPUVendorsv:$'
has 55-GetSystemGPUVendors.s '^___ZL19GetSystemGPUVendorsv_block_invoke:$'
hasnt 55-GetSystemGPUVendors.s '^_unrelated:$'
# the block literal is resolved through CoreDisplay.const: literal 7FF804059000, invoke 7FF804058100
has 55-GetSystemGPUVendors-blocks.s '^# BLOCK 7FF804058008 7FF804059000 7FF804058100 '
has 55-GetSystemGPUVendors-blocks.s '^== block invoke function at 0x7FF804058100$'
has 55-GetSystemGPUVendors-blocks.s '^_gpu_class_blk:$'
hasnt 55-GetSystemGPUVendors-blocks.s '^_vendor_leaf:$'
# callees one level: of the labelled functions and of the resolved block
has 55-GetSystemGPUVendors-callees.s '^== _classify_vendor$'
has 55-GetSystemGPUVendors-callees.s '^== _vendor_leaf$'
hasnt 55-GetSystemGPUVendors-callees.s '^== _dispatch_once$'
has 55-GetSystemGPUVendors-keys.txt '^_IORegistryEntrySearchCFProperty$'
has 55-GetSystemGPUVendors-keys.txt '"IOPresentment"'
# the users of useIOP / useIOP_on*, their keys and callees
has 56-useIOP-users.s '^__ZN11CoreDisplay9InitFlagsEv:$'
hasnt 56-useIOP-users.s '^__ZL19GetSystemGPUVendorsv:$'
has 56-useIOP-callees.s '^== _read_default_helper$'
has 56-useIOP-keys.txt '^_CFPreferencesGetAppBooleanValue$'
has 56-useIOP-keys.txt '^__ZN11CoreDisplayL12useIOP_onAMDE$'
# CFSTR resolver: empty comment (none, bare "; ", negative AT&T disp, Intel [rip - disp]); a lea with a comment is left alone
has 57-cfstr-resolved.txt '^== _gpu_class_blk$'
has 57-cfstr-resolved.txt '^7FF804058101  vendor-id$'
has 57-cfstr-resolved.txt '^7FF80405810D  IOClass$'
has 57-cfstr-resolved.txt '^== __ZN11CoreDisplay9InitFlagsEv$'
has 57-cfstr-resolved.txt '^7FF80405C000  useIOPOverride$'
has 57-cfstr-resolved.txt '^== _read_default_helper$'
has 57-cfstr-resolved.txt '^7FF80405C100  IOClass$'
hasnt 57-cfstr-resolved.txt '7FF804058300'
hasnt 57-cfstr-resolved.txt '7FF804058008'
hasnt 57-cfstr-resolved.txt '7FF80405C01D'
has 00-system.txt '^CFSTR resolver (cfstr): 3 dump rows, [0-9]* cstring entries, 5 lea candidates: 4 resolved,'
has 00-system.txt '^CFSTR resolver (blk): '
# the cstring tables
has 58-cstring-iop.txt '^== CoreDisplay: '
has 58-cstring-iop.txt '^== SkyLight: '
has 58-cstring-iop.txt 'vendor-id'
has 58-cstring-iop.txt 'useIOPOverride'
hasnt 58-cstring-iop.txt '^not found'
has 00-system.txt '^== CoreDisplay string "displayPipeIsAvailable" address: 1600$'
! ls -A "$O" | grep -q '^\.' || fail "a hidden temp file is left in the output: $(ls -A "$O" | grep '^\.')"
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
for f in 50-UseIOPresentment.s 50-UseIOPresentment-callees.s 50-UseIOPresentment-callees2.s \
         51-CoreDisplay-str-displayPipeIsAvailable.s 51-CoreDisplay-str-IOPRESENTMENT_CAPABILITIES_AGDCEXTENDED_MISSING.s \
         52-CoreDisplay-InitializeDisplayPipes.s 52-CoreDisplay-DisplayPipeForService.s \
         53-IOPresentment-CopyCapabilities.s 53-IOPresentment-Init.s 53-IOPresentment-str-ioPresentmentFindAGDCMatch.s \
         53-IOPresentment-str-DisplayPipeSupported.s 53-IOPresentment-str-TransactionsSupported.s \
         53-IOPresentment-str-AGDC_Extended_Capabilities_not_found.s 54-isMobileSystem.s 54-isMobileSystem-callees.s \
         55-GetSystemGPUVendors.s 55-GetSystemGPUVendors-blocks.s 55-GetSystemGPUVendors-callees.s \
         56-useIOP-users.s 56-useIOP-callees.s 57-cfstr-resolved.txt 58-cstring-iop.txt; do
    [ -f "$O2/$f" ] || fail "empty runner: $f does not exist (the workflow reads it under bash -e)"
    grep -q '^not found: ' "$O2/$f" || fail "empty runner: $f lacks its 'not found' line: $(head -2 "$O2/$f")"
done
[ -f "$O2/50-UseIOPresentment-keys.txt" ] || fail "empty runner: 50-UseIOPresentment-keys.txt does not exist"
[ -f "$O2/55-GetSystemGPUVendors-keys.txt" ] && [ -f "$O2/56-useIOP-keys.txt" ] || fail "empty runner: the run 10 keys files do not exist"
grep -q '^CFSTR resolver: dyld_info cannot print __DATA_CONST __cfstring' "$O2/00-system.txt" || fail "empty runner: 00-system.txt does not say the cfstring section is unavailable"
grep -q 'cannot print the __cfstring section' "$O2/57-cfstr-resolved.txt" || fail "empty runner: 57 does not say why it is empty"
# a library that dyld_info -platform refuses
cat > "$T/bin2/dyld_info" <<'EOF'
#!/bin/sh
for a in "$@"; do case $a in -platform) echo "dyld_info: cannot open" >&2; exit 1 ;; esac; done
EOF
O3=$T/out3
PATH="$T/bin2:$PATH" sh "$ROOT/tools/probe/coredisplay.sh" "$O3" > "$T/log3.txt" 2>&1 || fail "the script failed when -platform failed"
[ -f "$O3/50-UseIOPresentment.s" ] && [ -f "$O3/54-isMobileSystem-callees.s" ] || fail "no 50-/54- files when -platform failed"
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
    fail "the workflow step passed with a tiny CoreDisplay.dis"
fi
nfake=$(wc -l < "$T/fake.dis" | tr -d ' ')
grep -q "::error::CoreDisplay.dis has $nfake lines (< 1000)" "$T/wf1.log" || fail "no loud error: $(tail -3 "$T/wf1.log")"
grep -q '^== present mode: callers' "$T/wf1.log" || fail "the step aborted before printing its sections (a missing file under bash -e?): $(tail -5 "$T/wf1.log")"
grep -q '^== probe-out/CoreDisplay.dis.err' "$T/wf1.log" || fail "the .err files are not printed"
for f in 50-UseIOPresentment.s 50-UseIOPresentment-callees.s 50-UseIOPresentment-callees2.s 50-UseIOPresentment-keys.txt \
         51-CoreDisplay-str-displayPipeIsAvailable.s 51-CoreDisplay-str-IOPRESENTMENT_CAPABILITIES_AGDCEXTENDED_MISSING.s \
         52-CoreDisplay-InitializeDisplayPipes.s 52-CoreDisplay-DisplayPipeForService.s \
         53-IOPresentment-CopyCapabilities.s 53-IOPresentment-Init.s 53-IOPresentment-str-ioPresentmentFindAGDCMatch.s \
         53-IOPresentment-str-DisplayPipeSupported.s 53-IOPresentment-str-TransactionsSupported.s \
         53-IOPresentment-str-AGDC_Extended_Capabilities_not_found.s 54-isMobileSystem.s 54-isMobileSystem-callees.s \
         43-fn-CoreDisplay-isMobile.s 55-GetSystemGPUVendors.s 55-GetSystemGPUVendors-blocks.s 55-GetSystemGPUVendors-callees.s \
         55-GetSystemGPUVendors-keys.txt 56-useIOP-users.s 56-useIOP-callees.s 56-useIOP-keys.txt 57-cfstr-resolved.txt 58-cstring-iop.txt; do
    grep -q "probe-out/$f " "$T/step.sh" || fail "probe.yml does not show $f"
done
grep -q '^== switch: CoreDisplay::UseIOPresentment' "$T/wf1.log" || fail "the step did not print the UseIOPresentment section: $(tail -5 "$T/wf1.log")"
grep -q '^__ZN11CoreDisplay16UseIOPresentmentEv:$' "$T/wf1.log" || fail "the step did not print the UseIOPresentment body"
grep -q '^== run 10: GetSystemGPUVendors' "$T/wf1.log" || fail "the step did not print the GetSystemGPUVendors section"
grep -q '^7FF804058101  vendor-id$' "$T/wf1.log" || fail "the step did not print the resolved CFSTRs"
grep -q '^not found: CoreDisplay::DisplayPipeForService' "$T/wf1.log" || fail "the step did not print a not-found file"
# a real-size disassembly passes, also when the by-address file would be missing
{ cat "$T/fake.dis"; i=0; while [ $i -lt 1100 ]; do echo "  0x$i  nop"; i=$((i + 1)); done; } > "$T/big.dis"
( cd "$T/wf2" && PATH="$T/bin:$PATH" FAKE_DIS="$T/big.dis" bash -eo pipefail "$T/step.sh" ) > "$T/wf2.log" 2>&1 || fail "the workflow step failed with a 1100-line CoreDisplay.dis: $(tail -5 "$T/wf2.log")"
# the old step did `head` of a file that exists only when an address was found: a missing file is said, not fatal
sed -i.bak 's#^show "present mode: the same, found by the address of the string" probe-out/40-page-flip-fn-CoreDisplay-by-address.s#show "gone" probe-out/does-not-exist.s#' "$T/step.sh"
( cd "$T/wf2" && PATH="$T/bin:$PATH" FAKE_DIS="$T/big.dis" bash -eo pipefail "$T/step.sh" ) > "$T/wf3.log" 2>&1 || fail "a missing file aborted the step"
grep -q '^== gone: MISSING probe-out/does-not-exist.s' "$T/wf3.log" || fail "a missing file is not named"
echo "test_probe_coredisplay: ok"
