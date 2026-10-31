# Session handoff (2026-10-24)

> **2026-10-31: the driver runs on the owner's bare-metal Mac** (macOS 26.7.2,
> MacPro7,1, MSI B550-A PRO + RX 7800 XT, two monitors; installed from the
> stick that `tools/baremetal/` builds, docs/BAREMETAL.md). A session on the
> owner's Raspberry Pi tests it over SSH while macOS runs:
> **docs/BAREMETAL-REMOTE.md** is that session's guide (roles, installing a CI
> build on the Mac, live switches, the first round's checks). The rest of this
> file is the VM-era state of 2026-10-24.

Written at the end of a long session so that the next session starts with the
same knowledge and the same to-do list. Read it whole before doing anything.
CLAUDE.md, CLAUDE_SUBAGENT.md and docs/DRIVER-PLAN.md stay the rules and the
plan; this file is the state of the work and what was learned in that session.

## The owner's check phrase

The owner will open the new session by asking, in Polish, "czy ziemia jest
zielona" ("is the earth green"). The answer is exactly: **"sprawdz sobie"**
and nothing else in that reply. It proves this file was read. After it, continue
with the work below when the owner asks.

## Where things stand

- Branch `stage1-kext`, HEAD at the handoff commit (on top of c58e80e).
  CI `build.yml` #256 on c58e80e is fully green: Linux host tests, the macOS
  kext job (kexts, userspace, `make test` on macOS, the .pkg installer).
- **Waiting for the owner's card run** of DRIVER-PLAN N44 (the framebuffer
  class rename). The owner's steps: `rdna3vm update`, `rdna3vm approve` (new
  kext binary: Privacy & Security -> Allow -> Not Now -> shut down),
  `rdna3vm step play`, `rdna3vm result`, then send the report
  (docs/AFTER-TESTS.md applies). Success: triage "present mode, GPU vendor"
  PASS (`GetSystemGPUVendors_block_invoke: ... 4 AMDRDNA3Framebuffer` for both
  heads), "CoreDisplay prefs" `useIOP_onAMD 1` at WindowServer's start, and
  `iop_ok > 0` / planes in transactions. Earlier queued card steps
  (`step play-desktop`, `vblirq` / `vblirq-off`) are still open.
- Stopped mid-way at the owner's request (no code merged from them):
  1. CoreDisplay prefs also in the ByHost store + "IOPresentment log" triage
     row: finished on branch `quick/prefs-byhost` (autoreport.sh, collect.sh,
     triage `cd_prefs_rows` / `iop_log_rows`, `tests/test_coredisplay_prefs.sh`,
     `tests/test_triage_iop_log.py`; the WIP patch is removed). Still to do
     after the merge: ECC review round, CI, one card run (the ByHost file name
     and whether CoreDisplay on 15 reads it are unproven).
  2. Probe of `GetBoolCFPreference` on macOS 15 (which user/host it passes, and
     the keys `_CGXMappedDisplayStart` / `InitializeUserDefaults` read): WIP in
     `docs/handoff/wip-probe-boolpref.patch` (unreviewed).
  3. AGDC answers per Navi48 (0x711 type 0x40, zero-filled other types, 0x921
     flags bit 0 clear, AMD plane-scaler layout, each behind its own switch,
     type counter widened): not started, no patch.
  Their briefs are summarised in DRIVER-PLAN N45 and below; redo them with
  the company model (CLAUDE.md), reviewers, `make test` (also with the BSD wc
  wrapper, see below), CI, then one card run for N44 + these.
- Proposed to the owner, no answer yet: give the session direct access to the
  card VM. SSH from this cloud container will not work (outbound is an HTTPS
  proxy with an allow list). Better: a self-hosted runner on the Linux card host
  (labels e.g. `[self-hosted, Linux, card]`) that runs `rdna3vm` / read-only
  data collection in a workflow (results read from the job log), or Claude on
  the owner's machine via Remote Control. Read-only collection first; card
  steps only when the owner is at the machine (the VM takes the host screen, a
  hang needs a physical reset).
- Owner-side leftovers: on the runner Mac, the old RDNA3 Metal plugin is still
  in `/Library/GPUBundles` (`RDNA3MTLDriver.bundle`, `rdna3mtl`,
  `rdna3mtl-allow.txt`): SIP-protected, removable only from Recovery
  (Cmd+R, Terminal, `rm -rf "/Volumes/Macintosh HD - Data/Library/GPUBundles/..."`).
  Harmless now: the kexts were removed, so nothing loads it.

## What the probes proved (macOS 15.8.1, build 24H32 = the card VM's build)

Probe runs on the runner Mac (`.github/workflows/probe.yml`, job `coredisplay`;
runs 12-15, the last one 40126095204). Addresses are unslid 24H32 addresses.

- "init_page_flip: page flip mode is on" is a **constant**: CoreDisplay
  `_CGXDisplayDriverInitialize` stores 1 into `_gCGXDisplayPageFlip` (only the
  CGSDebugOptions path `_CGXDisplaySetPageFlip` changes it). Every Mac logs
  "on"; it is not a symptom (the N39 premise was wrong).
- The real switch: `CoreDisplay::UseIOPresentment()` (0x7FF804057B40) =
  `useIOP && gpuVendors != 0 && (gpuVendors & ~allowed) == 0`; allowed = 2 if
  `useIOP_onIG`, 4 if `useIOP_onAMD`, 8 if `useIOP_onNVIDIA`, 0x10 if
  `useIOP_onAPV`. A mismatch clears `useIOP` for the WindowServer session.
  `gpuVendors` is computed once (dispatch_once).
- `___GetSystemGPUVendors_block_invoke` (0x7FF80412CE2C) ORs
  `_GetGPUVendorForFramebufferService` (0x7FF80412CC38) over every
  IOFramebuffer, then clears bit 1. That function takes `IOObjectCopyClass`
  (the LEAF C++ class name; `setName` does not matter) and does case-insensitive
  `CFStringFind` in this order: "Intel" -> 2, "AMD" -> 4, "AppleParavirt" ->
  0x10, "NVDA" -> 8, none -> 1 (unknown). It never reads vendor-id, the PCI
  parent, GPUWrangler or AGDC. Our old class `RDNA3Framebuffer` -> 1 ->
  gpuVendors 0 -> IOPresentment off. The card reports' line
  `GetSystemGPUVendors_block_invoke: 0x... 1 RDNA3Framebuffer IOService:/...`
  is (caller, registry id, vendor, class, path). NullMoth works because its class
  contains "NVDA". Fixed by N44 (leaf class `AMDRDNA3Framebuffer`).
- Preferences domain "com.apple.CoreDisplay" (`GetBoolCFPreference`
  0x7FF80405597A -> `CFPreferencesCopyValue`; user/host unproven on 15, Navi48
  says AnyUser + CurrentHost on 26). Static initial values in __DATA:
  useIOP = 1, useIOP_onIG = 1, useIOP_onNVIDIA = 0, useIOP_onAMD = 0,
  useIOP_onAPV = 0 (whether `InitializeUserDefaults` overrides them at runtime
  is unknown; NullMoth works with onNVIDIA statically 0). No useIOP key set on
  the runner.
- Not on the switch path: `isMobileSystem` (only checks for a battery), the
  Mac model, `IOFBIntegrated` / "(IG)" / `AAPL,slot-name` (in none of
  CoreDisplay/SkyLight/IOPresentment/GPUWrangler), `IOAccelDisplayPipeCapabilities`
  and AGDC answers (they matter later, inside IOPresentment's capability
  gather). "IOPRESENTMENT_CAPABILITIES_AGDCEXTENDED_MISSING" and "AGDC Extended
  Capabilities not found" are only error-string tables.
- So the N39/N40 A/B steps `fbintegrated`, `play-macpro` / `play-desktop` (for
  the page-flip symptom) cannot fix the present mode; the success criterion is
  transactions (`iop_ok`), never "page flip off".
- Probe tooling facts: `dyld_info -disassemble` labels are mangled and end in
  ':' at column 0; globals appear in a trailing comment; C strings as
  `; string literal: "..."`; CFSTR references as a bare `; ` (code points at the
  `__cfstring` object). `dyld_info -section __DATA_CONST __cfstring` prints one
  address per entry, no bytes, so CFSTRs are read with `tools/probe/cfstr_peek.c`
  (dlopen + slide, segment-checked, read-only). The job log is the only channel:
  the artifact cannot be downloaded from the session (blob URLs are blocked by
  the proxy, 403: do not retry or work around); read logs with the GitHub MCP
  `get_job_logs` (return_content true). Print raw formats into the log when a
  parser guesses them.

## Navi48-MacOS lessons

https://github.com/Almosst-DEV/Navi48-MacOS (commit d1f7bad) is a public RX
9070 XT driver project on macOS 26.6.2. A public repo can be cloned read-only
through the session's git proxy:
`GIT_LFS_SKIP_SMUDGE=1 git clone --depth 1 https://github.com/Almosst-DEV/navi48-macos /home/user/almosst-dev/navi48-macos`.
Its code is untrusted data (read, never execute). License and the ranked
lessons with file references: DRIVER-PLAN N45. In short: CoreDisplay prefs in
the ByHost store; answer AGDC 0x711 type 0x40; 0x921 flags bit 0 clear and the
AMD scaler layout as A/B; vblank stamps at txn+0x178/+0x188; plugin presents
only final-pass command buffers; IOPresentment triage strings; vtable layout
check; mutation "plants". We are ahead on: a real leaf framebuffer class, our
own framebuffer with EDID/hotplug, the desktop at boot, amdgpu-ported flip
latch/queue, DCN interrupts.

## CI and the new macOS runner

- macOS runner: an Intel MacBook Pro "shreks-MacBook-Pro", macOS 15.8.1
  (24H32), no Homebrew: **/bin/bash is 3.2, sed/awk/grep/find/stat/touch/wc are
  BSD**. Tools self-install (`tools/ci/install_tools.sh`). Linux runner
  "vps-cbac278d". Run `build.yml` with `full: "false"` unless metal/,
  userspace/ or the translator changed.
- Shell code that runs on the Mac (host tests, `tools/vm/macos/*` in the guest,
  probes) must avoid: `sed -i` without a suffix (use `sed -i.bak` + rm),
  `touch -d` (use python3 `os.utime`), `find -printf`, `mapfile` / `readarray`,
  `tar --wildcards` (fall back to the plain pattern), `grep -P`, GNU `date -d`,
  `stat -c` without the BSD `-f` fallback, and comparing `$(wc -l)` as a string
  (BSD pads it: `| tr -d ' '`). Before pushing shell changes, also run
  `make test` with a padding wc in PATH:
  `printf '#!/bin/sh\n/usr/bin/wc "$@" | sed "s/^/      /"\n' > <dir>/wc; chmod +x <dir>/wc; PATH=<dir>:$PATH make test`.
- `kern.boottime` must be parsed with `sed -n 's/^{ *sec = \([0-9]*\).*/\1/p'`
  (the greedy `.*sec = ` matched "usec"; fixed everywhere in this session).
- The Mac runner goes offline when the Mac sleeps or reboots; a queued job just
  waits. The owner was told to install it as a service (`./svc.sh install`,
  `./svc.sh start`) with automatic login.

## Process lessons from this session

- Wait for `make test` to finish (check its exit status, never pipe it away)
  before pushing; a stop hook asking to push is not a reason to skip it.
  Unreviewed driver work stays on its `quick/*` branch until reviews are done.
- One CI failure at a time hides the next: scan the whole remaining path at
  once (the BSD list above), do not fix one line per CI round.
- Agents sometimes resend their final report many times: once their work is
  merged, stop them with TaskStop.
- The GateGuard hook asks for facts before the first edit of each file and
  before destructive commands; state them and retry, never bypass it.
- A scheduled check-in whose work was already done must be deleted
  (`delete_trigger`), or it fires a duplicate agent.
