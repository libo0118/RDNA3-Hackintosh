#!/bin/bash
# tools/vm/rdna3vm: 'update' and 'cts' fetch builds and artifacts from the
# owner's Forgejo (CI_FORGE=forgejo, the default) and still from GitHub
# (CI_FORGE=github). The functions are cut out of the script and run with a
# stubbed curl that serves canned Forgejo API JSON (shapes recorded from
# https://forgejo.voten.me, Forgejo 17 = gitea 1.22 API, 2026-10-25):
#  - runs: one 'status' field (running/success/failure/cancelled), the run
#    number is index_in_repo (id is the API key), 'limit' is ignored on page 1
#  - jobs: a bare list without steps; a running job's steps come from
#    actions/jobs/ID
#  - artifacts: a bare list (name, run_id, expired, size_in_bytes); the zip is
#    actions/artifacts/ID/zip
# Checks: ci_cfg precedence, ci_setup, the Forgejo -> GitHub-shape
# normaliser, the live jobs lines, cmd__update end to end (newest success,
# waiting for the build of HEAD, a failed build of HEAD, a build that is not
# there), the download + zip check + artifact-run marker, cmd_cts, the token
# only as a header (never in argv, never in the output), and the old GitHub
# path through a stubbed gh_api.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf -- "$T"' EXIT
fail() { echo "test_forgejo_update: FAIL $*"; [ -f "$T/out.txt" ] && tail -n 30 "$T/out.txt"; exit 1; }
RV=$ROOT/tools/vm/rdna3vm

FUNCS="ci_cfg ci_setup forgejo_token forgejo_api ci_api ci_norm ci_runs ci_run ci_jobs ci_artifacts
build_jobs_lines wait_build commit_unbuilt cmd__update cmd_cts"
for f in $FUNCS; do sed -n "/^$f() {/,/^}/p" "$RV"; done > "$T/fn.sh"
[ "$(grep -c '^[a-z_]*() ' "$T/fn.sh")" = 15 ] || fail "functions not found: $(grep -o '^[a-z_]*()' "$T/fn.sh" | tr '\n' ' ')"

# ---- a git checkout on stage1-kext (HEAD's sha is what the API lists)
export HOME=$T/home WORK=$T/work ROOT=$T/repo
mkdir -p "$HOME" "$WORK" "$ROOT"
git -C "$ROOT" init -q -b stage1-kext
git -C "$ROOT" remote add origin https://github.com/Owner/Repo.git
G="git -C $ROOT -c user.name=t -c user.email=t@t"
echo a > "$ROOT/a"; $G add a; $G commit -q -m one
echo b > "$ROOT/b"; $G add b; $G commit -q -m two
HEAD=$($G rev-parse HEAD)
OLD=$($G rev-parse HEAD~1)
export HEAD OLD
TOKEN=tok-SECRET-0123456789
export TOKEN

# ---- canned API answers (Forgejo shapes)
A=$T/api; mkdir -p "$A"
run() {  # run ID NO SHA STATUS
    printf '{"id":%s,"title":"build","workflow_id":"build.yml","index_in_repo":%s,"prettyref":"stage1-kext","commit_sha":"%s","status":"%s","html_url":"https://forgejo.voten.me/Voten641/RDNA3-Hackintosh/actions/runs/%s"}' "$1" "$2" "$3" "$4" "$2"
}
write_runs() {  # the list as Forgejo gives it, newest first, page 1 not limited
    printf '{"workflow_runs":[%s],"total_count":%s}' "$(IFS=,; echo "$*")" "$#"
}
zipof() {  # zipof OUT NAME CONTENT: a zip with one file, as an uploaded artifact
    python3 -I -c 'import sys, zipfile
z = zipfile.ZipFile(sys.argv[1], "w"); z.writestr(sys.argv[2], sys.argv[3]); z.close()' "$@"
}
art() {  # art ID NAME RUN EXPIRED
    printf '{"id":%s,"name":"%s","run_id":%s,"size_in_bytes":1234,"expired":%s,"created_at":"2026-10-25T09:00:00Z","archive_download_url":"x"}' "$1" "$2" "$3" "$4"
}
zipof "$A/zip_50.zip" kext.txt "kext of run 30"
zipof "$A/zip_52.zip" kext.txt "kext of run 38"
zipof "$A/zip_49.zip" kext.txt "an expired artifact"
printf 'not a zip' > "$A/zip_53.zip"
mkdir -p "$T/cts/cts"; echo "cts 1.3.7" > "$T/cts/cts/VERSION"
tar -czf "$T/rdna3-cts.tar.gz" -C "$T/cts" cts
python3 -I -c 'import sys, zipfile
z = zipfile.ZipFile(sys.argv[1], "w"); z.write(sys.argv[2], "rdna3-cts.tar.gz"); z.close()' "$A/zip_60.zip" "$T/rdna3-cts.tar.gz"

# run 30 (#5) succeeded on OLD; run 38 (#6) is the build of HEAD
RUN_OK=$(run 30 5 "$OLD" success)
RUN_HEAD_RUN=$(run 38 6 "$HEAD" running)
RUN_HEAD_FAIL=$(run 38 6 "$HEAD" failure)
RUN_HEAD_OK=$(run 38 6 "$HEAD" success)
RUN_OLD_FAIL=$(run 20 3 "$OLD" failure)
echo "$RUN_OK" > "$A/run_30.json"
echo "$RUN_HEAD_RUN" > "$A/run_38.json"
cat > "$A/jobs_38.json" <<'J'
[{"id":156,"run_id":38,"name":"changes","status":"success","steps":null},
 {"id":158,"run_id":38,"name":"kext","status":"running","steps":null},
 {"id":159,"run_id":38,"name":"radv-macos","status":"skipped","steps":null},
 {"id":160,"run_id":38,"name":"host-tests","status":"waiting","steps":null}]
J
cat > "$A/job_158.json" <<'J'
{"id":158,"run_id":38,"name":"kext","status":"running","steps":[
 {"number":0,"name":"Set up job","status":"success","started":"2026-10-25T08:41:14Z","stopped":"2026-10-25T08:41:15Z"},
 {"number":1,"name":"Build RDNA3Accel.kext","status":"running","started":"2026-10-25T08:41:17Z","stopped":null},
 {"number":2,"name":"upload","status":"waiting","started":null,"stopped":null}]}
J
# the second look at the jobs: the kext job finished (its artifact is up)
sed 's/"name":"kext","status":"running"/"name":"kext","status":"success"/' "$A/jobs_38.json" > "$A/jobs_38.1.json"
echo "[$(art 50 RDNA3Accel-kext 30 false),$(art 51 other 30 false)]" > "$A/runart_30.json"
echo "[$(art 52 RDNA3Accel-kext 38 false)]" > "$A/runart_38.json"
echo "[$(art 53 RDNA3Accel-kext 20 false)]" > "$A/runart_20.json"
echo "[$(art 49 RDNA3Accel-kext 30 true),$(art 54 RDNA3Accel-kext 30 true)]" > "$A/runart_expired.json"
# rdna3-cts: the newest one is expired, the next is fine
echo "[$(art 61 rdna3-cts 40 true),$(art 60 rdna3-cts 38 false)]" > "$A/allart_cts.json"
echo "[]" > "$A/empty.json"

# ---- the curl stub: serves $A by URL, records argv, checks nothing leaks
: > "$T/curl.log"
curl() {
    local out="" url="" cfg="" hdr=""
    while [ $# -gt 0 ]; do
        case $1 in
            -o) out=$2; shift 2 ;;
            -K) cfg=$2; shift 2 ;;
            -H) hdr=$2; shift 2 ;;
            -*) shift ;;
            *) url=$1; shift ;;
        esac
    done
    echo "$url" >> "$T/curl.log"
    [ -z "$cfg" ] || { cat "$cfg" >> "$T/curl.hdr"; }
    case $url in
        *nowhere.example*) echo "curl: (6) Could not resolve host" >&2; return 6 ;;
        "$CI_URL/api/v1/repos/$CI_REPO/"*) ;;
        *) echo "curl stub: unexpected URL $url" >&2; return 22 ;;
    esac
    local p=${url#"$CI_URL/api/v1/repos/$CI_REPO/"} f="" n
    case $p in
        actions/runs\?*)
            case $p in
                *"ref=refs/heads/stage1-kext"*) ;;
                *) echo "curl stub: runs without the branch ref: $p" >&2; return 22 ;;
            esac
            case $p in *"workflow_id=build.yml"*) ;; *) echo "curl stub: no workflow_id: $p" >&2; return 22 ;; esac
            case $p in *"status=success"*) f=runs_success ;; *) f=runs_all ;; esac ;;
        actions/runs/*/artifacts*) n=${p#actions/runs/}; n=${n%%/*}; f=runart_$n ;;
        actions/runs/*/jobs*) n=${p#actions/runs/}; n=${n%%/*}; f=jobs_$n ;;
        actions/runs/*) n=${p#actions/runs/}; f=run_$n ;;
        actions/jobs/*) f=job_${p#actions/jobs/} ;;
        actions/artifacts\?name=rdna3-cts*) f=allart_cts ;;
        actions/artifacts/*/zip) n=${p#actions/artifacts/}; n=${n%/zip}; f=zip_$n ;;
        *) echo "curl stub: unknown path $p" >&2; return 22 ;;
    esac
    # the k-th request of the same path gets FILE.k.json when it exists
    local key; key=$(printf '%s' "$p" | tr -c 'a-zA-Z0-9' _)
    local k=$(( $(cat "$T/cnt.$key" 2>/dev/null || echo 0) + 1 )); echo "$k" > "$T/cnt.$key"
    case $f in
        zip_*) [ -f "$A/$f.zip" ] || return 22; cp "$A/$f.zip" "${out:-/dev/stdout}"; return 0 ;;
    esac
    [ -f "$A/$f.$((k - 1)).json" ] && f=$f.$((k - 1))
    if [ ! -f "$A/$f.json" ]; then echo "curl stub: 404 $p" >&2; return 22; fi
    if [ -n "$out" ]; then cp "$A/$f.json" "$out"; else cat "$A/$f.json"; fi
}
sleep() { :; }
say()  { printf '== %s\n' "$*"; }
ok()   { printf '  OK  %s\n' "$*"; }
warn() { printf '  WARN %s\n' "$*"; }
die()  { printf 'DIE: %s\n' "$*" >&2; exit 1; }
CONF=$T/config
SELF=$RV
VMTOOL="python3 $(dirname "$RV")/vmtool.py"
cmd_prepare() { echo "prepare $*" >> "$T/prepared"; }
. "$T/fn.sh"
unset FORGEJO_TOKEN GITHUB_TOKEN CI_FORGE FORGEJO_URL FORGEJO_REPO 2>/dev/null || true
reset_api() { rm -f "$T"/cnt.* "$T/prepared" "$T/curl.hdr"; : > "$T/curl.log"; rm -f "$WORK/artifact-run" "$HOME/Downloads/RDNA3Accel-kext.zip"; }

# ---- ci_cfg: environment > config file > default; a commented key is nothing
printf '# comment\n#FORGEJO_URL=https://commented.example\nFORGEJO_URL="https://cfg.example/"\nFORGEJO_REPO=cfg/repo\n' > "$CONF"
[ "$(ci_cfg CI_FORGE forgejo)" = forgejo ] || fail "ci_cfg: default"
[ "$(ci_cfg FORGEJO_URL x)" = "https://cfg.example/" ] || fail "ci_cfg: config value (quotes stripped): $(ci_cfg FORGEJO_URL x)"
[ "$(FORGEJO_REPO=env/repo ci_cfg FORGEJO_REPO x)" = env/repo ] || fail "ci_cfg: the environment must win"
rm -f "$CONF"

# ---- ci_setup: Forgejo by default, GitHub on request, nothing else
ci_setup
[ "$CI_FORGE $CI_URL $CI_REPO" = "forgejo https://forgejo.voten.me Voten641/RDNA3-Hackintosh" ] || fail "ci_setup default: $CI_FORGE $CI_URL $CI_REPO"
[ "$CI_PAGE" = https://forgejo.voten.me/Voten641/RDNA3-Hackintosh/actions ] || fail "ci_setup: CI_PAGE $CI_PAGE"
( CI_FORGE=github; ci_setup; [ "$CI_REPO $CI_NAME" = "Owner/Repo GitHub Actions" ] ) || fail "ci_setup github: slug of origin"
( FORGEJO_URL=https://other.example/ FORGEJO_REPO=a/b ci_setup; [ "$CI_URL $CI_REPO" = "https://other.example a/b" ] ) || fail "ci_setup: FORGEJO_URL / FORGEJO_REPO"
( CI_FORGE=gitlab ci_setup ) > "$T/out.txt" 2>&1 && fail "ci_setup: an unknown CI_FORGE was accepted"
grep -q "CI_FORGE is 'gitlab'" "$T/out.txt" || fail "ci_setup: no message for an unknown CI_FORGE"

# ---- the token: environment, ~/.config file, .env; sent as a header, not in argv
[ -z "$(forgejo_token)" ] || fail "forgejo_token: a token out of nowhere"
echo "FORGEJO_TOKEN=\"from-env-file\"" > "$ROOT/.env"
[ "$(forgejo_token)" = from-env-file ] || fail "forgejo_token: .env"
mkdir -p "$HOME/.config/rdna3vm"; echo "from-config-file" > "$HOME/.config/rdna3vm/forgejo-token"
[ "$(forgejo_token)" = from-config-file ] || fail "forgejo_token: ~/.config/rdna3vm/forgejo-token before .env"
[ "$(FORGEJO_TOKEN=from-env forgejo_token)" = from-env ] || fail "forgejo_token: the environment must win"
rm -f "$ROOT/.env" "$HOME/.config/rdna3vm/forgejo-token"

# ---- the normaliser, on the recorded shapes
reset_api
write_runs "$RUN_HEAD_RUN" "$(run 36 5 "$OLD" failure)" "$(run 35 4 "$OLD" cancelled)" "$(run 30 3 "$OLD" waiting)" "$RUN_OK" > "$A/runs_all.json"
write_runs "$RUN_OK" > "$A/runs_success.json"
norm=$(ci_runs stage1-kext 3 | python3 -I -c '
import json, sys
r = json.load(sys.stdin)["workflow_runs"]
print(len(r), *[(x["run_number"], x["status"], x["conclusion"]) for x in r])')
[ "$norm" = "3 (6, 'in_progress', None) (5, 'completed', 'failure') (4, 'completed', 'cancelled')" ] || fail "ci_runs normalisation / limit: $norm"
norm=$(ci_runs stage1-kext 10 | python3 -I -c '
import json, sys
print([(x["run_number"], x["status"]) for x in json.load(sys.stdin)["workflow_runs"]][3:])')
[ "$norm" = "[(3, 'queued'), (5, 'completed')]" ] || fail "ci_runs: waiting is queued: $norm"
norm=$(ci_runs stage1-kext 1 success | python3 -I -c '
import json, sys
r = json.load(sys.stdin)["workflow_runs"]
print(r[0]["id"], r[0]["run_number"], r[0]["head_sha"] == sys.argv[1], r[0]["conclusion"])' "$OLD")
[ "$norm" = "30 5 True success" ] || fail "ci_runs success: $norm"
grep -q 'status=success' "$T/curl.log" || fail "ci_runs success: the status filter was not sent"
# no success at all: an empty list, not an error
echo '{"workflow_runs":[],"total_count":0}' > "$A/runs_success.json"
[ "$(ci_runs stage1-kext 1 success)" = '{"workflow_runs": []}' ] || fail "ci_runs: empty answer"
write_runs "$RUN_OK" > "$A/runs_success.json"

# jobs: the running one gets its current step and start time
reset_api
jl=$(ci_jobs 38 | build_jobs_lines)
case $jl in *"kext"*"Build RDNA3Accel.kext"*) ;; *) fail "jobs lines: the running job shows no step: $jl" ;; esac
case $jl in *"changes"*) ;; *) fail "jobs lines: changes missing" ;; esac
[ "$(printf '%s\n' "$jl" | grep -c .)" = 4 ] || fail "jobs lines: expected 4 lines: $jl"
printf '%s\n' "$jl" | grep -q 'radv-macos' || fail "jobs lines: skipped job missing"
printf '%s\n' "$jl" | grep -q 'wait' || fail "jobs lines: the waiting job should say wait"
ci_jobs 38 | python3 -I -c '
import json, sys
j = {x["name"]: x for x in json.load(sys.stdin)["jobs"]}
assert j["kext"]["status"] == "completed" and j["kext"]["conclusion"] == "success", j["kext"]
' || fail "jobs: the kext job of the second look should be completed/success"
grep -q 'actions/jobs/158' "$T/curl.log" || fail "jobs: the running job's steps were not fetched"
grep -q 'actions/jobs/156' "$T/curl.log" && fail "jobs: a finished job was fetched for its steps"

# artifacts
a=$(ci_artifacts RDNA3Accel-kext 30 | python3 -I -c '
import json, sys
print([(x["id"], x["workflow_run"]["id"], x["expired"]) for x in json.load(sys.stdin)["artifacts"] if x["name"] == "RDNA3Accel-kext"])')
[ "$a" = "[(50, 30, False)]" ] || fail "ci_artifacts of a run: $a"
a=$(ci_artifacts rdna3-cts | python3 -I -c '
import json, sys
print([(x["id"], x["workflow_run"]["id"], x["expired"]) for x in json.load(sys.stdin)["artifacts"]])')
[ "$a" = "[(61, 40, True), (60, 38, False)]" ] || fail "ci_artifacts by name (newest first, expired kept for the caller): $a"

# ---- cmd__update, end to end

# 1. the build of HEAD is the newest success: it is downloaded
write_runs "$RUN_HEAD_OK" "$RUN_OK" > "$A/runs_all.json"
write_runs "$RUN_HEAD_OK" "$RUN_OK" > "$A/runs_success.json"
reset_api
FORGEJO_TOKEN=$TOKEN cmd__update > "$T/out.txt" 2>&1 || fail "update 1 failed"
grep -q 'build #6' "$T/out.txt" || fail "update 1: build #6 (run 38) not chosen"
[ "$(cat "$WORK/artifact-run")" = 38 ] || fail "update 1: artifact-run is $(cat "$WORK/artifact-run" 2>/dev/null)"
[ -f "$HOME/Downloads/RDNA3Accel-kext.zip" ] && [ ! -e "$HOME/Downloads/RDNA3Accel-kext.zip.part" ] || fail "update 1: zip missing or .part left"
python3 -I -c 'import sys, zipfile; z = zipfile.ZipFile(sys.argv[1]); assert z.read("kext.txt") == b"kext of run 38"' "$HOME/Downloads/RDNA3Accel-kext.zip" || fail "update 1: the zip is not run 38's artifact"
grep -q "prepare --artifact $HOME/Downloads/RDNA3Accel-kext.zip" "$T/prepared" || fail "update 1: prepare not called with the zip"
grep -q 'actions/artifacts/52/zip' "$T/curl.log" || fail "update 1: not the artifact 52 zip endpoint"
# the token: only as a header in the curl config, never in argv, never printed
grep -q "Authorization: token $TOKEN" "$T/curl.hdr" || fail "update 1: no Authorization header from the token"
grep -q "$TOKEN" "$T/curl.log" && fail "update 1: the token is in a URL"
grep -q "$TOKEN" "$T/out.txt" && fail "update 1: the token was printed"
# a second run: already downloaded, nothing fetched again
: > "$T/curl.log"
FORGEJO_TOKEN=$TOKEN cmd__update > "$T/out.txt" 2>&1 || fail "update 1b failed"
grep -q 'already downloaded' "$T/out.txt" || fail "update 1b: not 'already downloaded'"
grep -q 'artifacts/52/zip' "$T/curl.log" && fail "update 1b: downloaded again"
# no token at all: the public repository works anonymously
reset_api
cmd__update > "$T/out.txt" 2>&1 || fail "update 1c (anonymous) failed"
[ ! -s "$T/curl.hdr" ] || fail "update 1c: a header without a token"

# 2. the build of HEAD is still running: wait for the kext job, download its artifact
write_runs "$RUN_HEAD_RUN" "$RUN_OK" > "$A/runs_all.json"
write_runs "$RUN_OK" > "$A/runs_success.json"
reset_api
cmd__update > "$T/out.txt" 2>&1 || fail "update 2 failed"
grep -q 'still running: waiting for it' "$T/out.txt" || fail "update 2: did not wait for the build of HEAD"
grep -q 'the kext job finished' "$T/out.txt" || fail "update 2: did not stop at the finished kext job"
grep -q 'Build RDNA3Accel.kext' "$T/out.txt" || fail "update 2: the running step was not shown"
[ "$(cat "$WORK/artifact-run")" = 38 ] || fail "update 2: artifact-run $(cat "$WORK/artifact-run" 2>/dev/null)"
python3 -I -c 'import sys, zipfile; assert zipfile.ZipFile(sys.argv[1]).read("kext.txt") == b"kext of run 38"' "$HOME/Downloads/RDNA3Accel-kext.zip" || fail "update 2: wrong zip"
# --no-wait: stops with a message instead
reset_api
( cmd__update --no-wait ) > "$T/out.txt" 2>&1 && fail "update 2b: --no-wait went on"
grep -q "still running: run 'rdna3vm update' again" "$T/out.txt" || fail "update 2b: no message"

# 3. the build of HEAD failed: the last good build is used
write_runs "$RUN_HEAD_FAIL" "$RUN_OK" > "$A/runs_all.json"
reset_api
cmd__update > "$T/out.txt" 2>&1 || fail "update 3 failed"
grep -q 'failed: using the last good build #5' "$T/out.txt" || fail "update 3: no fallback message"
[ "$(cat "$WORK/artifact-run")" = 30 ] || fail "update 3: artifact-run $(cat "$WORK/artifact-run" 2>/dev/null)"
python3 -I -c 'import sys, zipfile; assert zipfile.ZipFile(sys.argv[1]).read("kext.txt") == b"kext of run 30"' "$HOME/Downloads/RDNA3Accel-kext.zip" || fail "update 3: wrong zip"

# 4. no success at all, and a success without the artifact / with a bad zip
echo '{"workflow_runs":[],"total_count":0}' > "$A/runs_success.json"
reset_api
( cmd__update ) > "$T/out.txt" 2>&1 && fail "update 4: no success must fail"
grep -q 'no successful build.yml run on stage1-kext' "$T/out.txt" || fail "update 4: message"
write_runs "$(run 20 3 "$HEAD" success)" > "$A/runs_success.json"
write_runs "$(run 20 3 "$HEAD" success)" > "$A/runs_all.json"
echo '[]' > "$A/runart_20.json"
reset_api
( cmd__update ) > "$T/out.txt" 2>&1 && fail "update 4b: a run without the artifact must fail"
grep -q 'build #3 has no RDNA3Accel-kext artifact' "$T/out.txt" || fail "update 4b: message"
echo "[$(art 49 RDNA3Accel-kext 20 true)]" > "$A/runart_20.json"
reset_api
( cmd__update ) > "$T/out.txt" 2>&1 && fail "update 4c: an expired artifact must not be used"
grep -q 'build #3 has no RDNA3Accel-kext artifact' "$T/out.txt" || fail "update 4c: an expired artifact must read as no artifact"
echo "[$(art 53 RDNA3Accel-kext 20 false)]" > "$A/runart_20.json"
reset_api
( cmd__update ) > "$T/out.txt" 2>&1 && fail "update 4d: a bad zip must fail"
grep -q 'not a valid zip' "$T/out.txt" || fail "update 4d: message"
[ ! -e "$HOME/Downloads/RDNA3Accel-kext.zip" ] || fail "update 4d: the bad zip replaced the good one"

# 5. the API is unreachable: a message that says where to look
reset_api
( FORGEJO_URL=https://nowhere.example cmd__update ) > "$T/out.txt" 2>&1 && fail "update 5: an unreachable forge must fail"
grep -q 'cannot list the workflow runs of Voten641/RDNA3-Hackintosh on Forgejo Actions' "$T/out.txt" || fail "update 5: message"

# ---- cmd_cts: the newest unexpired rdna3-cts artifact
reset_api
mkdir -p "$WORK/share"
cmd_cts > "$T/out.txt" 2>&1 || fail "cts failed"
[ -f "$WORK/share/rdna3-cts.tar.gz" ] && [ -s "$WORK/share/rdna3-cts.sha256" ] || fail "cts: tarball / sha256 not written"
grep -q 'cts 1.3.7' "$T/out.txt" || fail "cts: VERSION not printed"
grep -q 'artifacts/60/zip' "$T/curl.log" || fail "cts: not the unexpired artifact 60 (61 is expired)"
echo "[$(art 61 rdna3-cts 40 true)]" > "$A/allart_cts.json"
reset_api
( cmd_cts ) > "$T/out.txt" 2>&1 && fail "cts: only an expired artifact must fail"
grep -q 'no rdna3-cts artifact (https://forgejo.voten.me/Voten641/RDNA3-Hackintosh/actions' "$T/out.txt" || fail "cts: message names the Forgejo page"

# ---- CI_FORGE=github: the old path (gh_api stubbed) still works
GH_LOG=$T/gh.log; : > "$GH_LOG"
gh_api() {
    echo "$1" >> "$GH_LOG"
    case $1 in
        repos/Owner/Repo/actions/workflows/build.yml/runs\?branch=stage1-kext\&status=success*)
            printf '{"workflow_runs":[{"id":777,"run_number":9,"head_sha":"%s","status":"completed","conclusion":"success"}]}' "$HEAD" ;;
        repos/Owner/Repo/actions/workflows/build.yml/runs*)
            printf '{"workflow_runs":[{"id":777,"run_number":9,"head_sha":"%s","status":"completed","conclusion":"success"}]}' "$HEAD" ;;
        repos/Owner/Repo/actions/runs/777/artifacts)
            echo '{"artifacts":[{"id":5,"name":"RDNA3Accel-kext","expired":false,"workflow_run":{"id":777}}]}' ;;
        repos/Owner/Repo/actions/artifacts/5/zip) cp "$A/zip_52.zip" "$2" ;;
        *) echo "gh stub: $1" >&2; return 1 ;;
    esac
}
reset_api
CI_FORGE=github cmd__update > "$T/out.txt" 2>&1 || fail "update github failed"
grep -q 'GitHub Actions' "$T/out.txt" || fail "update github: not named"
grep -q 'build #9' "$T/out.txt" || fail "update github: build #9"
[ "$(cat "$WORK/artifact-run")" = 777 ] || fail "update github: artifact-run"
[ ! -s "$T/curl.log" ] || fail "update github: curl was called (the Forgejo path leaked)"

echo "test_forgejo_update: ok"
