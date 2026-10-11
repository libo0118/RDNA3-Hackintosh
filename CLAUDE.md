# Notes for AI agents working in this repository

- macOS driver for AMD RX 7800 XT (Navi 32), ported from Linux amdgpu.
  Working branch: `stage1-kext`.
- Talk to the user in Polish; everything committed is English only.
- CI runs on the owner's self-hosted runners (`[self-hosted, Linux]`,
  `[self-hosted, macOS]`, both x86_64) and may be used freely for testing:
  manual `build.yml` runs as often as needed, `full: true` included.
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
