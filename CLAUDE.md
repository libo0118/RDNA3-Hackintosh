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
