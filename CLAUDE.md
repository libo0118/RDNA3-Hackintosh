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
