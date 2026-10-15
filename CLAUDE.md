# Notes for AI agents working in this repository

- macOS driver for AMD RX 7800 XT (Navi 32), ported from Linux amdgpu.
  Working branch: `stage1-kext`.
- Talk to the user in Polish; everything committed is English only.
- CI runs on the owner's self-hosted runners (`[self-hosted, Linux]`,
  `[self-hosted, macOS]`, both x86_64). Use them freely, as often as is
  useful: dispatch `build.yml` after every push of code (the macOS job is
  the only real Xcode build, `kextlibs` symbol check and macOS run of the
  host tests), and use them for any extra check or experiment. Never use
  GitHub-hosted runners (`ubuntu-latest`, `macos-latest`, ...): every job
  must stay on `runs-on: [self-hosted, ...]`. Run `build.yml` with
  `full: "false"` (kext + host tests: all `rdna3vm update` takes);
  `full: true` only when `metal/`, `userspace/` or the translator changed.
- Hardware changes are ports of named amdgpu functions and are reproduced in
  `tests/sim_navi32.c` first; `make test` must pass before pushing.
- Kext code can be type-checked on Linux without CI:
  `MACOS_SDK=<MacOSX11.3.sdk> tools/kext_check.sh` (SDK from
  github.com/phracker/MacOSX-SDKs releases); run it before pushing kext changes.
- **When the user sends hardware-test reports** (`rdna3-report-*.tar.gz`,
  `serial-*.log`, `host-*.log`, `*-tests.txt`): follow
  [docs/AFTER-TESTS.md](docs/AFTER-TESTS.md) step by step.
- Test procedures the user runs: `docs/TESTING-VM.md`, `docs/TESTING-HW.md`.
- README status (`docs/status/`): run `python3 tools/progress.py` before every
  commit that changes driver code (CI checks it). A ladder step that passed
  on the card goes into `docs/status/hw-verified.txt`, then regenerate.
  New amdgpu source files to track: `tools/progress/sources.txt`, then
  `python3 tools/progress/update_index.py`.

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
2. Briefs each one completely (they start with no context: files, the
   rules of this file, what done looks like), runs independent work in
   parallel and in the background, and owns collecting the results.
3. Hands every finished piece to other employees before it ships: review
   and bug hunting (`ecc:cpp-reviewer` / `ecc:code-reviewer`,
   `ecc:security-reviewer`, `ecc:silent-failure-hunter`), testing
   (`ecc:pr-test-analyzer`, then `make test`, kext_check, CI). Findings go
   back to an implementer; the loop ends when the reviewers have nothing
   confirmed left.
4. Checks the result itself (it is accountable for it), commits, pushes,
   dispatches CI, and reports to the CEO in Polish: what was done, by whom,
   what was found and fixed, what is still open.

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
- CI is only the self-hosted runners above.
