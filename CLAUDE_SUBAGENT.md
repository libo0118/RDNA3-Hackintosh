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
(`reports/<date>/`, never committed); `docs/AFTER-TESTS.html` explains how
they are read. Nobody can run the hardware but the owner, so every card
run is expensive: work that needs one must be right the first time.

The owner's macOS 26 VM (OSX-KVM, reached with `ssh -i ~/.ssh/claude_tahoe
-p 10022 shrek@localhost`) is for read-only probes and plugin builds in
`/tmp`, and only while the owner has it running. **Never start, stop or
restart it yourself** (no `boot-macOS*.sh`, no QEMU monitor commands): it
boots the same disk (`mac_hdd_ng.img`) as the card VM `rdna3vm` runs, and
two machines on one disk corrupt it. If ssh does not answer, say so in
your report and let the manager ask the owner; build on CI instead.

## Know where things are: docs/CODEMAP.html

Read docs/CODEMAP.html before anything else in the code: the directories,
each subsystem's key files and functions, where the logs, counters,
report members and triage rows live, the tests and CI jobs, and a "where
to change X" table. Go straight to the files it names; search only for
what it does not cover. Whoever adds a subsystem, a file family, a report
member or a counter family updates it in the same commit (the row format
is in "The working docs are HTML" below).

## The working docs are HTML: the exact row formats

`docs/DRIVER-PLAN.html`, `docs/AFTER-TESTS.html` and `docs/CODEMAP.html` are
machine-read and edited every day; edit them as HTML, by hand, in these
shapes (never convert them back to Markdown). In text, `<`, `>` and `&` are
written `&lt;`, `&gt;`, `&amp;` (a placeholder is `&lt;date&gt;`, not
`<date>`); a path, command, counter or log line goes in `<code>...</code>`,
a multi-line command in `<pre><code>...</code></pre>`, keeping every byte.
Tools: `python3 tools/docrows.py next` (the next free N), `snippet N [SIZE]`
(an empty row), `fix` (re-syncs the badges), `check` (the same check as
`make test`: tags balanced, ids unique, links and #anchors between the
docs resolve, N rows well formed).

**DRIVER-PLAN, a new N row** (append it as the last `<section>` inside
`<div class="rows" id="n-rows">`; the number is `docrows.py next`):

```html
<section class="row" id="n77" data-n="77" data-status="open" data-size="M">
<div class="rowhead"><a class="rid" href="#n77">N77</a> <span class="badge st-open gen">open</span></div>
<p class="rowtext">What is missing: the evidence, the cause, what was done, what stays open.</p>
<dl class="rowmeta"><dt class="gen">Where it shows</dt><dd>accel, kext</dd><dt class="gen">Size</dt><dd>M</dd><dt class="gen">Status</dt><dd>open</dd></dl>
</section>
```

`data-status` is the one field that counts: `open`, `card` (done, the card run
is open), `partial`, `done`, `superseded` or `unknown`; when it changes, set
the `<dd>` Status text too and run `python3 tools/docrows.py fix` (the badge
follows). To update a row, edit its `<p class="rowtext">` / Status in place
(find it with `grep -n 'id="n68"' docs/DRIVER-PLAN.html`); a reference to
another row is `<a href="#n68">N68</a>`. The work-item tables (P, S, V, A,
M, R) and the "Done" tables are ordinary tables: a row is
`<tr id="p6"><td>P6</td><td>Item</td><td>amdgpu source</td><td>S</td><td>1 boot</td></tr>`
(lower-case id; the "Done" tables have no ids).

**AFTER-TESTS**: a symptom row is
`<tr><td>symptom</td><td>likely cause</td><td>action</td></tr>` appended to
the `<tbody>` of the step's table (every table has a stable id, e.g.
`id="step-4-t1"`; sections: `<h3 id="slug">`, the slug being the title
lower-cased with everything but letters, digits, `_`, `-` and spaces removed
and spaces turned into `-`, plus an alias `<span id="step-4"></span>` /
`<span id="card-run-60"></span>` as the first child for steps and card runs).
A new section is an `<h3 id="slug">` with its table or list under it and an
entry in the `<nav class="toc">` at the top (the section's `<li><a
href="#slug">`); a new card run is `<h3 id="slug"><span
id="card-run-NN"></span>Card run NN (...)</h3>`. Other docs link to these
ids (`AFTER-TESTS.html#the-whole-ladder-in-one-card-run-rdna3vm-step-ladder`):
never rename a heading that is linked, `grep -rn` the old id first.

**CODEMAP**: one table per area; a new file or family is a row
`<tr><td><code>path/file.cpp</code></td><td>what it holds</td><td><code>entry points</code></td></tr>`
in the table of its area (the columns are the table's header), and the
"Last updated" line at the top gets the date.

## Find code through the graph first

Before grepping or reading files, ask the code graph (it saves most of
the tokens an agent spends finding the right place):

```sh
G=/mnt/data/Programowanie/RDNA3-Hackintosh/graphify-out/graph.json
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
6. Docs: `docs/AFTER-TESTS.html` (symptom -> counters -> action) and
   `docs/DRIVER-PLAN.html` (what stays open) when behaviour changed, in
   the row formats below; then `python3 tools/docrows.py check` (it runs
   in `make test` as `test_docs_html`) prints `docs: ok`.

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
