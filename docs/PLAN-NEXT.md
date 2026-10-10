# Plan: what is left, where and how (2026-10-10)

Starting point: commit `1b38786` on `stage1-kext`. Ladder steps 0-8 passed
on the card in the VM (`docs/status/hw-verified.txt`). Step 9 runs (our
framebuffer takes over the GOP head at 1920x1080, hardware cursor, EDID of
both monitors over the DDC) but macOS still names the monitor "Unknown".

Rules unchanged: port named amdgpu functions (MIT), reproduce the hardware
behaviour in `tests/sim_navi32.c` first, `make test` before every push,
commits with `[skip ci]`, one manual `build.yml` run (`full: "false"`) per
batch, only when a card test needs the build.

Legend: **[sim]** can be finished and verified in the simulator now;
**[card]** needs a run on the card to confirm; **[big]** several sessions.

## Status after the night of 2026-10-10 (simulator only, nothing card-tested)

| Item | Commit | What the card has to show |
|---|---|---|
| A2 head -> connector | `2440cea` | `RDNA3,Head0,Route` names the lit connector |
| A4 refresh read-back | `264dce3` | `display: head 0 ... refresh` near 60 Hz |
| A3 DMCUB windows logged | `c9f4315` | the three `DMUB state` lines (step 5b) |
| E1/E2 triage + docs | `7d95ce8` | - |
| B1-B4 clone onto a 2nd TMDS monitor | `bf056d5` | step 9b (`rdna3dmub=1 rdna3mirror=1`), needs 5b |
| B7 HPD interrupts | `ab2665b` | unplug/plug: `display: HPDn ...` (with `rdna3msi=1`) |
| C1 MES scheduler, C2 teardown order | `98702fd` | step 8c (`rdna3mes=1`): `RDNA3,MESResult = 0` |
| CI fix (MES test state across the two sim runs) | `d9adaa7` | - |
| B6 extended desktop on the clone | `cb15c8b` | step 9c (`rdna3mirror=2`): two displays in System Settings |
| D2 PRIME + fence fds | `e5e2b21` | - (host tests) |
| A1 EDID fetched at framebuffer start | `ba761b9` | step 9: monitor name instead of "Unknown" |
| clone check (frame counter + OTG CRC) | `a54bc5b` | `RDNA3,MirrorCheck` running, same CRC |
| PerformanceStatistics from the SMU | `c3db924` | `ioreg` shows temperature, fan, clocks |
| `rdna3vm queue`, `package/oc_install.sh` | `3127016`, `8786936` | - |
| B5 subset: DCN clock read-back, DAL mailbox | this batch | `RDNA3,DCNClocks`; step 9b log `DAL SMU: 2 display(s)` |
| `rdna3vm step 8c / 9b / 9c` | `3f743e5` | - |

Still open: A1 (monitor name), A3 (DMCUB on the card), B5 (bandwidth /
watermarks for real modesets), B6 (one framebuffer per head, mode
change: step 9c), C3, C4, D.

## A. Open findings from the card (first)

1. **macOS monitor name ("Unknown")** [card] - `fb/rdna3-fb.cpp`.
   `1b38786` logs what IOGraphics asks (HLDDC sense, `hasDDCConnect`) and
   what the core answers (`displayEdid`), and retries the EDID fetch. Next
   run decides between: IOGraphics never asks (then set
   `kConnectionSupportsHLDDCSense` from `start()`, publish `IODisplayEDID`
   on the `IODisplayConnect` ourselves, or answer `getDDCBlock` with the
   right block numbering) and the core refusing (fix `displayEdid`).
2. **Head -> connector routing** [sim] - `hw/dcn32.c`, `kext/src/RDNA3Display.cpp`.
   Today head 0 takes "the EDID that contains its mode". Port the read-back
   amdgpu uses for seamless boot: `optc1_get_otg_active_size` /
   `dcn10_get_dig_frontend` (`DIG_FE_CNTL.DIG_SOURCE_SELECT` = OTG),
   `dcn10_get_dig_mode`, `DIG_BE_CNTL.DIG_FE_SOURCE_SELECT`, and the
   transmitter (UNIPHY A-G) -> VBIOS path -> DDC line. Publish
   `RDNA3,Head<n>,Connector`. Simulator: GOP state with OTG1 -> DIG2 ->
   UNIPHYB.
3. **DMCUB (step 5b)** [card] - `hw/dmub32.c`. CW0/CW1 are zero when we
   start the PSP-loaded DMCUB. Pending test `rdna3lvl=5 rdna3dmub=1
   rdna3pm=0`. If still zero: compare with `dm_dmub_hw_init` +
   `dmub_srv_hw_init` for `DMUB_WINDOW_0_INST_CONST` when
   `load_type == PSP` (amdgpu passes the inst_const region anyway and
   programs CW0 itself only when `!psp_loaded`?) - re-read
   `dmub_dcn32_backdoor_load` vs `dmub_dcn32_setup_windows`, try the
   backdoor load (`rdna3dmub=2`) as the fallback already in the code.
4. **Refresh rate read-back** [sim] - one run printed 63.741 Hz for a
   1080p60 head. `optc1_get_hw_timing` + DCCG `OTG_PIXEL_RATE`/DTO read:
   check `dcn32_head_refresh` against `dccg32_get_pixel_rate_div`
   (`OTG_PIXEL_RATE_DIV`, `DTBCLK` vs `DPREFCLK`).

## B. Display: our own mode setting (light the second monitor) [big]

Goal: HDMI/TMDS SST on DCN 3.2 (both monitors here are TMDS; DP later),
second head, mode change from macOS. Plan B of the old plan: a mini-DC,
not the whole `amd/display/dc`.

B1. **Register read-back and model** [sim] - generate DIG FE/BE, HDMI
    stream encoder, DCCG (`OTG_PIXEL_RATE_CNTL`, `DPPCLK_DTO`,
    `PHYASYMCLK`), OPP/MPC (`MPCC_*`, `OPP_PIPE_CONTROL`) headers with
    `tools/gen_regs.py`; simulator gets a DCN pipe model (HUBP -> DPP ->
    MPC -> OPP -> OTG -> DIG -> PHY) that knows what "lit" means.
B2. **Timing + scanout on a free pipe** [sim] - `optc1_program_timing`
    (done in `dcn32_mode.c` as values) applied: `optc32_enable_crtc`,
    `optc1_program_global_sync`, `hubp2_program_surface_config` /
    `hubp1_program_pixel_format`, `dpp` bypass, MPCC tree for one plane,
    `opp_program_fmt` (8 bpc, no dither). Same order as
    `dcn20_enable_stream_timing` + `dcn20_program_pipe`.
B3. **Pixel clock** [sim] - DCN 3.2 drives the OTG pixel rate through
    DCCG DTO (`dccg32_set_dtbclk_dto` / `dccg2_set_dpp_clk`), the PHY
    symbol clock through the VBIOS `SetPixelClock` (v7) - run it with our
    ATOM interpreter (`hw/atom.c`), or through DMCUB
    (`dmub_cmd` `SET_PIXEL_CLOCK`) once 5b works.
B4. **Encoder + transmitter** [sim, card] - `DIGxEncoderControl` (v5,
    `ENCODER_CMD_STREAM_SETUP` HDMI) and `DIG1TransmitterControl` (v1.7,
    `TRANSMITTER_CONTROL_ENABLE`) through the ATOM interpreter (amdgpu's
    `bios_parser2` command tables), `enc32_stream_encoder_hdmi_set_stream_attribute`
    (AVI infoframe off at first), `dcn32_unblank_stream`.
B5. **Bandwidth** [sim] - minimum: request DISPCLK/DPPCLK from the SMU
    (`smu_v13_0_set_hard_min_clk`), fixed watermarks/DLG from a 1080p60
    table (DML later); `dcn32_prepare_bandwidth` subset.
B6. **Framebuffer** [card] - `RDNA3Framebuffer`: one instance per head,
    `getDisplayModeCount`/`getDisplayModes` from the EDID's usable modes,
    `setDisplayMode` -> core modeset call (`RD3K_DISPLAY_FN_MODESET`),
    `IOFBConnectInterrupt` on HPD.
B7. **HPD interrupts** [sim] - DCN HPD IH sources (`DC_HPD1_INT`..),
    `dce110_hpd_irq` enable/ack, re-probe EDID on change, notify the fb.

Ladder: new step **9b** = second monitor lit by us, **9c** = mode change.

## C. GPU core follow-ups

C1. **MES scheduler pipe** [sim] - `mes_v11_0_hw_init`: map the MES ring
    (pipe 0) through the KIQ, `SET_HW_RESOURCES`, `QUERY_SCHED_STATUS`;
    `enable_legacy_queue_map` when the MES firmware >= 0x47 (log the
    version from `CP_MES_GP3_LO`). Needed for user queues and queue resets.
C2. **Teardown/reset on MES KIQ** [sim] - soft reset re-runs
    `mes_v11_0_kiq_hw_init`; check `gfx11_mec_stop` order (KGQ unmap,
    KCQ unmap, MES disable) against `gfx_v11_0_hw_fini`.
C3. **SDMA soft reset, more KCQs, GFX pipe 1** [sim] - small.
C4. **Clock gating / GFXOFF** [card] - only after display is stable.

## D. Acceleration path (step 10)

D1. **Step 10 smoke test** [card] - `rdna3accel=1`: the accelerator
    attaches, WindowServer still composes; collect `ioreg -l` of
    `IOAccelerator`, `IOSurface` usage.
D2. **PRIME / IOSurface sharing** [sim] - BO export/import between the
    accelerator and libdrm_xnu (`AMDGPU_GEM_OP`, dma-buf-like handles).
D3. **Metal plugin** [big] - after D1.

## E. Tooling and docs (cheap, keep current)

E1. `tools/triage_reports.py`: step 9 checks (`RDNA3FB` EDID line,
    `EDIDSource`, `Head0EdidMode`), the serial-log interleaving (read
    `20-log-rdna3.txt` first).
E2. `docs/TESTING-VM.md` / `TESTING-HW.md`: step 9 expectations (monitor
    name, DDC EDID), steps 9b/9c.
E3. `docs/AFTER-TESTS.md` rows for every new symptom.

## Order for the next sessions

A2 -> A4 -> E1/E2 -> B1 -> B2 -> B3 -> B4 -> B7 -> C1 -> C2, with A1/A3
whenever a card report arrives. Each item: simulator test, `make test`,
`tools/progress.py`, commit `[skip ci]`, Notion card updated.
