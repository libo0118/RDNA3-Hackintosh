# Notes for AI agents working in this repository

- **Session agent (the manager), before anything else: read
  [docs/HANDOFF.html](docs/HANDOFF.html)** — the state of the work, what the last
  session learned and left to do, and the owner's check phrase.
- macOS driver for AMD RX 7800 XT (Navi 32), ported from Linux amdgpu.
  Working branch: `stage1-kext`.
- Talk to the user in Polish; everything committed is English only.
- CI runs only on the owner's Forgejo (https://forgejo.voten.me, repo
  `Voten641/RDNA3-Hackintosh`; the owner's decision, 2026-10-25), with its
  two runners, labels `linux` and `macOS` (both x86_64, jobs on the host).
  Pushes go to GitHub and Forgejo; CI is dispatched on Forgejo only
  (`POST /api/v1/repos/Voten641/RDNA3-Hackintosh/actions/workflows/build.yml/dispatches`
  with the token from `FORGEJO_TOKEN` in the git-ignored `.env`; never write
  the token anywhere). Use the runners freely, as often as is useful:
  dispatch `build.yml` after every push of code (the macOS jobs are the only
  real Xcode build and `kextlibs` symbol check (`kext`, its artifact ready in about 2 minutes)
  and the macOS run of the host tests (`macos-tests`, after it)),
  and use them for any extra check or experiment. Never use hosted runners:
  every job stays on `runs-on: linux` / `runs-on: macOS`. Run `build.yml`
  with `full: "false"` (kext + host tests: all `rdna3vm update` takes);
  `full: true` only when `metal/`, `userspace/` or the translator changed.
- Hardware changes are ports of named amdgpu functions and are reproduced in
  `tests/sim_navi32.c` first; `make test` must pass before pushing.
- Kext code can be type-checked on Linux without CI:
  `MACOS_SDK=<MacOSX11.3.sdk> tools/kext_check.sh` (SDK from
  github.com/phracker/MacOSX-SDKs releases); run it before pushing kext changes.
- **When the user sends hardware-test reports** (`rdna3-report-*.tar.gz`,
  `serial-*.log`, `host-*.log`, `*-tests.txt`): follow
  [docs/AFTER-TESTS.md](docs/AFTER-TESTS.md) step by step.
- Test procedures the user runs: `docs/TESTING-VM.html`, `docs/TESTING-HW.html`.
- README status (`docs/status/`): run `python3 tools/progress.py` before every
  commit that changes driver code (CI checks it). A ladder step that passed
  on the card goes into `docs/status/hw-verified.txt`, then regenerate.
  New amdgpu source files to track: `tools/progress/sources.txt`, then
  `python3 tools/progress/update_index.py`.

## Documentation format

The owner's decision (2026-11-01): documentation is written as `.html`, not
`.md`. Markdown only has headings, bold and bullets; HTML can also carry
tables, CSS, SVG charts, code, live interactions, workflows, spatial data and
real images. Every new document is a self-contained `.html` file (inline CSS
and SVG, no external fetches, readable from a local checkout), and a document
that is rewritten moves to `.html`. The exceptions stay Markdown: this
`CLAUDE.md` (Claude Code reads it only as Markdown), `CLAUDE_SUBAGENT.md` (the
agents' rules) and a short `README.md` (GitHub and Forgejo render only that on
the repository page), which carries excerpts of the full HTML status and links
to it. All other docs are converted to proper HTML. When a `.md` file becomes `.html`,
update in the same commit every tool, test and link that reads or names it
(`tools/progress.py`, the triage scripts, `CLAUDE_SUBAGENT.md`, other docs).
Every HTML doc uses the style and header layout of
[docs/doc-template.html](docs/doc-template.html) (copy its `<style>` block
verbatim) and is listed in `docs/index.html`. Brief every agent that writes
docs accordingly.

## How we work: a company

The owner is the CEO: they say what they want done or fixed. The session
agent is the manager: it does not do the work itself, it runs the team.
The team is the specialised agents available to the session (the ECC
roster, `/ecc:ecc-guide` lists them, plus the built-in ones), each for its
own job. For every request the manager:

1. Breaks it into tasks (TaskCreate) and picks the right employee for each:
   planning (`ecc:planner`, `ecc:architect`, `ecc:code-architect`),
   finding things (`Explore`, `ecc:code-explorer`), implementing
   (`general-purpose` with a precise brief, `ecc:tdd-guide` for host-test
   work, `ecc:build-error-resolver` / `ecc:cpp-build-resolver` when CI is
   red), docs (`ecc:doc-updater`).
2. Briefs each one completely but briefly: every brief starts with "Read
   /mnt/data/Programowanie/RDNA3-Hackintosh/CLAUDE_SUBAGENT.md first" (the rules every
   agent follows: graph first, worktree, commits and trailers, kernel
   and plugin code rules, logging, checks before done, the final report,
   reviewers) and then holds only what this task needs: the worktree,
   files, goal, evidence, files other agents own, what done looks like.
   Runs independent work in parallel and in the background, and owns
   collecting the results. A rule that applies to every agent goes into
   CLAUDE_SUBAGENT.md, not into briefs.
3. Hands every finished piece to other employees before it ships: review
   and bug hunting (`ecc:cpp-reviewer` / `ecc:code-reviewer`,
   `ecc:security-reviewer`, `ecc:silent-failure-hunter`), testing
   (`ecc:pr-test-analyzer`, then `make test`, kext_check, CI). Findings go
   back to an implementer; the loop ends when the reviewers have nothing
   confirmed left.
4. Checks the result itself (it is accountable for it), commits, pushes,
   dispatches CI, and reports to the CEO in Polish: what was done, by whom,
   what was found and fixed, what is still open.

How many employees at once (the CEO's rule: bigger tasks, fewer agents,
so the Claude usage limits last). Count only agents running at the same
time; an `opus` agent counts as two:

| What is running | Most at once |
|---|---|
| Read-only search / inventory / log gathering (`haiku`) | 4 |
| Reviews after a change (read-only reviewers) | 4 |
| Small implementation tasks (size S) | 3 |
| Medium implementation tasks (size M) | 2 |
| Large tasks (L / XL), or any kernel work on `opus` | 1, plus at most one read-only helper |
| Anything mixed | 4 units in total (opus = 2 units) |

Queue the rest (TaskCreate) and start the next one when an agent
finishes. Prefer one agent that does a whole task well over several that
each redo the same reading.

Which model each employee runs on (the CEO's choice; pass it as the Agent
tool's `model` on every delegation, it overrides the agent's own default):

| Work | Model |
|---|---|
| Kernel code (kext, hw, uapi, fb, agdc), the accelerator, security review, architecture and planning | `opus` |
| Other implementation (plugin, libdrm_xnu, tools, tests, docs), code review, test analysis, build fixes | `sonnet` |
| Searching the code, gathering logs and reports, simple lookups | `haiku` |

Small, mechanical steps (a one-line fix, git, CI dispatch, reading a log)
the manager may do directly; anything larger goes to the team. Two
employees never edit the same files at the same time: give each one its
own worktree. Do NOT use the Agent tool's `isolation: "worktree"` — it
branches from origin/main, which is far behind `stage1-kext` (it put five
agents 88 commits back on 2026-10-15). Instead create them from the tip:
`git worktree add .claude/worktrees/<task> -b quick/<task> stage1-kext`
and give it the code graph:
`ln -s "$PWD/graphify-out" .claude/worktrees/<task>/graphify-out`
(`.claude/worktrees/` is git-ignored), tell the agent to work only there
and commit on that branch without pushing, then merge the branch into
`stage1-kext` and remove the worktree.

## ECC

The project uses ECC (https://github.com/affaan-m/ECC):
`.claude/settings.json` enables its Claude Code plugin `ecc@ecc` (agents,
skills, commands, hooks) for every session in this repository, and
`.claude/rules/ecc/` holds its `common` and `cpp` rules.

**Use ECC whenever it can help (the owner's standing request).** After
every code change, before or right after the push, run the ECC agents that
fit, in parallel and in the background: `ecc:code-reviewer` or
`ecc:cpp-reviewer` (kext, accel, uapi, hw, libdrm_xnu), `ecc:security-reviewer`
(ioctls, user input, memory shared between processes),
`ecc:silent-failure-hunter` (the driver is debugged only from the card's
report logs, so a failure without a log line is a bug) and
`ecc:pr-test-analyzer` (what `make test` could cover). Fix what they
confirm, add the tests they find missing, record what stays open in
docs/DRIVER-PLAN.md, and say in the commit that ECC reviewed it. Use
ECC's planning agents for larger features and its build resolvers when
CI is red. Use its other agents and
skills where they fit (planning, code review, security review, TDD on the
host tests). Where ECC and this file disagree, this file wins:

- Commits keep this repository's style: a plain summary line ("Card run
  50: ...", "rdna3vm: ..."), a body that explains why, the session's
  trailers. No `feat:`/`fix:` prefixes.
- The kext is XNU kernel code (IOKit, libkern C++): no STL, no exceptions,
  no `std::unique_ptr`/`std::vector`, no `new`/`delete` outside OSObject;
  IOMalloc/IOFree and OSObject retain/release are the norm there. `hw/` and
  `uapi/` are portable C99. The ECC C++ rules on modern C++, RAII through
  the standard library, GoogleTest/CMake and clang-format do not apply.
- Tests are `make test` (simulator, UAPI, host tools) and the card runs;
  the 80% coverage and E2E rules do not apply. Hardware fixes still follow
  "port, don't invent" (docs/AFTER-TESTS.md) rather than ECC's research
  step.
- CI is only the owner's Forgejo runners above.

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

Rules:
- When graphify-out/graph.json exists, run `graphify query "<question>"` before your first grep, search or source-file read for a codebase question, and put this rule in every subagent prompt that explores code. Read raw files only for lines the graph or an earlier read already located. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
- A committed graphify-out/graph.json is rewritten whole on every build, so a large diff to it in a pull request is expected. Do not flag it as churn or suggest skipping the graph rebuild or its hooks.

In this repository (the owner's rule: every agent uses the graph, to save tokens):
- graphify-out/ is not committed. `.claude/hooks/graphify-session.sh` (SessionStart) installs the pinned CLI and builds or updates the graph of the checkout (AST only, about 30 s); the PreToolUse hooks (`graphify hook-guard`) hold back the first raw file read of a session until one `graphify query` ran. A session without the CLI or the graph simply reads files.
- Every agent brief that involves code includes: "Use the code graph before grepping or reading files: `graphify query \"<question>\" --budget 1500`, `graphify explain \"<symbol>\"`, `graphify path \"<A>\" \"<B>\"`, `graphify affected \"<symbol>\"` (add `--graph <path>/graphify-out/graph.json` outside the repo root); read raw files only for the lines the graph located."
- Worktrees link the main checkout's graph (see "How we work"); it describes `stage1-kext`, not the agent's own edits. Agents in a worktree do NOT run `graphify update` (it would rewrite the shared graph with their branch).
- After merging a branch into `stage1-kext`, the manager runs `graphify update .` in the main checkout.
