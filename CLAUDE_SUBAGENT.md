# How agents work in this repository

Read this before anything else. Your brief from the manager only says
what to do in this task (files, goal, evidence, what done looks like);
everything that is the same for every task is here. Where the brief and
this file disagree, the brief wins. `CLAUDE.md` is the manager's file;
this one is yours.

## The project in one paragraph

A macOS driver for the AMD RX 7800 XT (Navi 32, `1002:747E`), ported from
Linux amdgpu: the kexts (`kext/`, `fb/`, `accel/`; portable hardware code
in `hw/`, `common/`, `uapi/`), the Metal plugin WindowServer and apps run
on (`metal/plugin/`, Metal -> Vulkan -> RADV -> our DRM), libdrm_xnu and
tools. The owner tests on the real card in a VM and sends reports
(`reports/<date>/`, never committed); `docs/AFTER-TESTS.md` explains how
they are read. Nobody can run the hardware but the owner, so every card
run is expensive: work that needs one must be right the first time.

The owner's macOS 26 VM (OSX-KVM, reached with `ssh -i ~/.ssh/claude_tahoe
-p 10022 shrek@localhost`) is for read-only probes and plugin builds in
`/tmp`, and only while the owner has it running. **Never start, stop or
restart it yourself** (no `boot-macOS*.sh`, no QEMU monitor commands): it
boots the same disk (`mac_hdd_ng.img`) as the card VM `rdna3vm` runs, and
two machines on one disk corrupt it. If ssh does not answer, say so in
your report and let the manager ask the owner; build on CI instead.

## Find code through the graph first

Before grepping or reading files, ask the code graph (it saves most of
the tokens an agent spends finding the right place):

```sh
G=/home/user/RDNA3-Hackintosh/graphify-out/graph.json
graphify query "<question>" --budget 1500 --graph $G
graphify explain "<symbol>" --graph $G       # a node and its neighbours
graphify path "<A>" "<B>" --graph $G         # how A reaches B
graphify affected "<symbol>" --graph $G      # what depends on it
```

Read raw files only for the lines the graph located. The graph describes
`stage1-kext`, not edits made in your worktree. Do not run
`graphify update` from a worktree (it rewrites the shared graph).

## Where you work

- Only in the worktree your brief names (`.claude/worktrees/<task>`, branch
  `quick/<task>`). Never edit the main checkout or another agent's
  worktree; never push; never touch `main` or `stage1-kext`.
- Commit on your branch when done (more commits are fine; never amend a
  commit you did not make in this task).
- Never `git stash` / `git stash pop` (the stash is shared by every
  worktree); use a WIP commit instead. No `git reset --hard`, no force
  operations, no deleting branches.
- Files the brief says another agent owns are off limits: put your part
  in new files and say in your report how to wire them in.

## Commits

- Everything committed is English. Summary line in the repository's style
  (`Card run 2026-10-17b: ...`, `rdna3vm: ...`, `graphify: ...`), no
  `feat:`/`fix:` prefixes, `[skip ci]` at the end of the summary line.
- The body says why. When reviewers' findings are fixed, say the ECC
  review round in the body.
- End with exactly these trailers (no model names anywhere else):
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01TUagumeQUVJp3cJSLhgzB1
  ```

## Code rules

- **Kext** (`kext/`, `fb/`, `accel/`): XNU kernel C++ (IOKit, libkern).
  No STL, no exceptions, no `new`/`delete` outside OSObject; IOMalloc /
  IOFree, OSObject retain/release, IOLock / IOSimpleLock, OSAddAtomic.
  Never sleep (IOSleep, IOLock, SDMA waits) with a spinlock held.
- **`hw/`, `common/`, `uapi/`**: portable C99. Put pure decision logic in
  `common/*.h` with a host test in `tests/` (pattern:
  `common/rdna3_swap_front.h` + `tests/test_swap_front.c`).
- **Port, don't invent**: a hardware behaviour follows a named amdgpu
  function, named in a comment; reproduce hardware fixes in
  `tests/sim_navi32.c` first.
- **Metal plugin** (ObjC/C): it runs inside WindowServer, a crash there
  is a dead desktop. It builds only on CI's macOS runner (`full: true`);
  you cannot compile it here, so mirror existing patterns and check types
  by hand (a clang syntax check against the macOS SDK helps).
- ECC's modern-C++, RAII-through-STL, GoogleTest, CMake, clang-format
  and 80 % coverage rules do not apply here.

## Logging: a failure without a log line is a bug

The driver is debugged only from the report of a card run. Every failure
and every decision that can change the picture gets a counter (a
`debug.rdna3accel_*` / `debug.rdna3_*` sysctl, collected by
`tools/macos/collect.sh` and `tools/vm/macos/autoreport.sh`) or a log
line, with the reason and the values that tell hypotheses apart. Rate
limit logs (first N, then every 1024th), but never so that the first
occurrence or the cause disappears. New counters must reach the report
(check the collectors' grep and the file names) and `tools/triage_reports.py`
must print them: missing data is a loud WARN/FAIL, never silence.

## Before you say done

1. `make test` passes in your worktree.
2. Kext changes: `MACOS_SDK=/tmp/claude-0/-home-user-RDNA3-Hackintosh/23ea88ca-2e98-5b1d-81ac-506363fb8ea8/scratchpad/MacOSX11.3.sdk tools/kext_check.sh`
   prints `14 files ok` (if that SDK path is gone, say so).
3. Driver code changed: `python3 tools/progress.py` (CI checks
   `docs/status/`).
4. A new accel source or header: keep the hash lists in sync
   (`accel/Makefile`, `ACCEL_HASH_FILES` in `tools/triage_reports.py`,
   `tests/test_accel_hash_parity.py`).
5. A new test is wired into the Makefile's `test` target.
6. Docs: `docs/AFTER-TESTS.md` (symptom -> counters -> action) and
   `docs/DRIVER-PLAN.md` (what stays open) when behaviour changed.

Never dispatch CI, never use GitHub-hosted runners, never commit
`reports/`, firmware or anything with serial numbers or EDIDs.

## Your final message (the manager only sees this)

- Commits (hash, branch), test results (what you ran, pass/fail).
- What you found, marked **certain from code** (with file:line) or
  **needs the card**.
- What you changed: files, functions, new sysctls/boot-args with
  defaults, new counters and log lines exactly as they will appear,
  triage verdicts.
- Risks and what you could not do or check. Never end on "waiting for".

## Reviewers

When your brief makes you a reviewer: read-only (no edits, commits or
pushes). Verify every finding in the code before reporting it; trace a
realistic path from a real caller. Report each as CRITICAL / HIGH /
MEDIUM / LOW with file:line, the concrete failure scenario and a fix,
and say "no confirmed findings" when there are none. Run `make test` in
the worktree you review when it helps.
