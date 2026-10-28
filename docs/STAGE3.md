# Stage 3: display (DCN 3.2)

A full port of Display Core from amdgpu (modesetting, link training, DMCUB) is over
100k lines. The first step is much smaller: **we take over the mode that
the VBIOS / UEFI GOP set**. The card already displays a picture from a linear buffer in VRAM;
the driver only reads where that buffer is and what the timing looks like, and then
switches (flips) the display to other buffers in VRAM that the
accelerator prepared. The link, clocks and OPP/MPC routing stay as the firmware left them.

```
GOP / VBIOS ──▶ DCN 3.2: HUBP (address, pitch, format) ─▶ OTG (timing) ─▶ monitor
                    ▲  read + flip                         ▲ frame counter, CRC
          hw/dcn32.c (MIT) ◀── RDNA3Accel.kext: rdna3DisplayInfo/Flip/Status/Crc
                                     ▲  (kext/src/RDNA3Display.cpp)
                         RDNA3Framebuffer.kext (fb/, IOFramebuffer, from NVRMFB)
                                     ▲
                    WindowServer / RDNA3Accelerator (surface flips)
```

## Parts

| File | What it does |
|---|---|
| `hw/dcn32.c` | 256×256 ARGB hardware cursor (HUBP + DPP, like `hubp2_cursor_set_*` / `dpp2_set_cursor_attributes`); finds lit heads (HUBP not blanked, OTG enabled), reads format, tiling, DCC, viewport, pitch, MC address, OTG timing (like `optc1_get_hw_timing`); flip (`hubp2_program_surface_flip_and_addr`, pitch change under `SURFACE_UPDATE_LOCK`), flip pending, frame counter, line, vblank, OTG CRC |
| `kext/src/RDNA3Display.cpp` | at `rdna3lvl>=2` reads the heads (**read only**), measures the refresh rate from the frame counter, answers framebuffer calls; the VRAM allocators start above the GOP buffer (4K = 32 MiB) |
| `fb/rdna3-fb.cpp` | `RDNA3Framebuffer`: one mode (the one from firmware), BAR0 aperture, VBL from the OTG frame counter, flips for the accelerator; without the core or without a usable head it works as a plain framebuffer on the bootloader console buffer (`PE_Video`) |

## Gates

| Boot-arg | Effect |
|---|---|
| no `rdna3fb=1` | `RDNA3Framebuffer` does not start; Apple's IONDRVFramebuffer drives the screen |
| `rdna3fb=1` | our framebuffer takes the `IOFramebuffer` category (score 30000 > 20000 IONDRV); if `start()` fails, IOKit falls back to IONDRV |
| `rdna3lvl=2` (or higher) | the core knows the heads: DCN mode (flips, real VBL) instead of console mode |
| `rdna3cursor=0` | software cursor instead of the DCN hardware cursor |
| `-rdna3off` | disables everything |

Writes to DCN registers happen only on a flip or when CRC is enabled,
that is, only once the accelerator starts displaying its own surfaces.

## Tests with a single card (no iGPU)

macOS without a driver for the 7800 XT boots on the GOP buffer (IONDRVFramebuffer):
no acceleration, one resolution, one monitor. That is enough.

1. BIOS: CSM disabled (pure UEFI GOP). With Resizable BAR on, set
   `ResizeAppleGpuBars = 10` (1 GiB) or `8` in OpenCore, or turn it off: macOS
   takes no BAR above 1 GiB (DRIVER-PLAN N62). **Do not** block
   `IONDRVSupport` in this stage.
2. In macOS enable Remote Login (SSH) and connect from another device.
3. Install the kexts (`package/install.sh`), but **do the first tests by
   loading them manually**, not at boot: a hard reset fixes a hang.
4. Order:
   1. `rdna3lvl=2`: reads only. `ioreg -l -w0 | grep RDNA3` should show
      `RDNA3,DisplayHeads`, `RDNA3,DisplayReserveEnd`; in the log
      `display: head 0 …x… pitch … usable`.
   2. `rdna3lvl=2 rdna3fb=1`: our framebuffer instead of IONDRV. Expected:
      the desktop looks the same, in the log `RDNA3-fb: DCN mode: …`. If the screen
      goes dark: SSH and logs, then reboot without `rdna3fb=1`.
   3. Only then the accelerator (`rdna3accel=1`, Stage 7) and flips.
5. Logs: `log show --last 10m --predicate 'eventMessage CONTAINS "RDNA3"'`.

## DP AUX, HPD, EDID (first step of Display Core)

`hw/dcaux.c` (port of `dce_aux.c`: arbiter, `AUX_SW_DATA` FIFO, `AUX_SW_GO`,
status, reply) provides native DPCD read/write, I2C-over-AUX and EDID
(128 B + extensions, E-DDC segment), `dcaux_hpd_sense` reads
`DC_HPD_INT_STATUS.DC_HPD_SENSE`. The AUX engine ↔ connector ↔ OTG assignment
is in the VBIOS object table (not parsed yet), so the kext probes
all 5 engines at startup (`RDNA3,Connectors`,
`RDNA3,Connector<n>,{AUX,HPD,DPCD,EDID,Monitor,Modes}`), and `hw/edid.c`
extracts from the EDID the name, the detailed modes (DTD, including CTA-861), the standard
and the established ones. `RDNA3Framebuffer` answers `hasDDCConnect`/`getDDCBlock`
(`rdna3DisplayEdid`: head 0 = first connected sink with an EDID), so
macOS builds `IODisplayEDID` and shows the monitor name. Modesetting from these
modes still needs: an ATOM interpreter (the `TransmitterControl`,
`SetPixelClock` tables), DP link training and OTG/HUBP programming for the new mode.

## VBIOS object table and AtomBIOS interpreter (step 2)

`vbios_display_paths` (port of `bios_parser2`: `get_bios_object`,
`get_hpd_record`, `get_gpio_i2c_info`) reads `displayobjectinfo` v1.4/v1.5
and `gpio_pin_lut`: connector (DP/HDMI/USB-C), encoder (UNIPHY A..G), DDC line
(= AUX engine), HPD pin, encoder capabilities (HBR3, UHBR, MST) and link
limit. The kext publishes `RDNA3,DisplayPath<n>` and labels the AUX connectors with their type.
Which OTG drives a path is not in the VBIOS: Display Core assigns pipes
at modeset.

`hw/atom.c` is a port of `atom.c` from amdgpu: all 126 opcodes, IIO methods,
FB window (scratch), nested `CALLTABLE` (limit 32), loop detection
(20 s of requested delays) plus an opcode budget. Unlike Linux, it does not
allocate memory (workspace from a pool in the context) and checks the image bounds
on every bytecode read. The kext creates a context and publishes the versions of the
`DIGxEncoderControl`, `DIG1TransmitterControl`, `SetPixelClock`,
`EnableDispPowerGating`, `SetDCEClock` tables (`RDNA3,AtomCommandTables`), but
does not execute anything yet. The simulator has a mini bytecode assembler and tests
every opcode group, IIO, FB, loop abort, out-of-image access and
recursion.

Note: on DCN 3.2 Linux does not execute these tables in the driver
(`dcn32 debug.dmub_command_table = true`). `command_table2` sends them to
DMCUB as `DMUB_CMD__VBIOS` commands, and the DMCUB firmware executes them from a copy
of the VBIOS in the CW3 window. So the interpreter is a fallback path and a
diagnostic tool, and the main path is DMCUB (below).

## DMCUB (step 3)

`hw/dmub32.c` is a port of `dmub_srv.c` + `dmub_dcn32.c` and
`dm_dmub_sw_init`/`dm_dmub_hw_init`:

* reads the `dcmub_firmware_header_v1_0` header and the
  `dmub_fw_meta` metadata block (behind the 256- or 512-byte PSP footer), lays out 12 windows
  (inst const, stack, BSS, VBIOS, mailbox, trace, FW state, scratch, IB,
  shared state, LSDMA, cursor offload) in VRAM, with alignment as in Linux;
* reset: `GPINT STOP_FW` to the running firmware (left by the
  VBIOS), waiting for `0xDEADDEAD` in SCRATCH7 and `PWAIT_MODE`, soft reset,
  disabling the CW2-CW7 windows, clearing the mailboxes;
* the instruction image is loaded by PSP (already in the `LOAD_IP_FW` plan), so CW0/CW1
  stay PSP's; the backdoor path (copying the image to CW0 with the physical
  FB address) is there too;
* windows CW3 (VBIOS), CW4 (inbox1 + outbox1), CW5 + region 5 (trace /
  outbox0), CW6 (FW state), boot options, `reset_release`, waiting for
  `DAL_FW | MAILBOX_RDY`;
* inbox1 ring (64-byte commands, wrap, full ring, reading the reply from the
  slot), `QUERY_FEATURE_CAPS` as the first exchange, `DMUB_CMD__VBIOS`
  with `DIG1TransmitterControl` v1.7.

In the kext this is level 5 behind the `rdna3dmub=1` flag (off by default, because
DMCUB drives the display brought up by GOP and the reset must first be
checked on the card). Result: `RDNA3,DMUB`, `RDNA3,DMUBResult`,
`RDNA3,DMUBVersion`, `RDNA3,DMUBBootStatus`, `RDNA3,DMUBCaps`.
The simulator models the firmware: it checks the windows, the VBIOS copy and the mailbox before
reporting ready, answers STOP_FW, executes commands from the ring;
the tests cover the real `dcn_3_2_1_dmcub.bin` (version 0x07003300).

## Mode calculations (step 4a)

`hw/dcn32_mode.c` computes what can be checked without the card: OTG field values
for a mode from the EDID exactly like `optc1_program_timing` (with the front porch
correction), DP 8b/10b link bandwidth from the rate code and the lane count (with
a 0.5 % margin for downspread), a mode filter (no detailed timing,
interlace, link too slow at 24 bpp, conservative single-pipe limits without
ODM) and detection of which EDID mode the GOP drives. The simulator checks that
the values computed for the 1440p mode from the EDID are exactly the ones the GOP has in the
OTG. The kext publishes `RDNA3,Head0EdidMode` and `RDNA3,Connector<n>,ModesUsable`,
and the log describes each mode with the reason it was rejected.

## DisplayPort SST mode set (step 5)

`hw/dcn32_dp.c` ports the DP SST / 8b/10b part of Display Core so a DP head
can be re-timed by the driver, as the TMDS heads already are
(`hw/dcn32_modeset.c`): `retrieve_link_cap` (subset) with
`dp_retrieve_lttpr_cap` and `wake_up_aux_channel`, `dp_get_max_link_cap`
(sink caps, the VBIOS encoder caps, an LTTPR's limits),
`link_decide_link_settings` / `decide_dp_link_settings` over
`dp_link_bandwidth_kbps` and `dc_bandwidth_in_kbps_from_timing`,
`decide_fallback_link_setting`, `dp_verify_link_cap_with_retries`,
`perform_link_training_with_retries`, `dp_perform_link_training`,
`dp_perform_8b_10b_link_training` with its clock recovery and channel
equalization sequences (TPS1, then TPS2/3/4 as sink and encoder allow),
`perform_post_lt_adj_req_sequence`, `dp_transition_to_video_idle`. The
hardware side is the DCN 3.2 one of DC: the DIO link hwss, the dcn10 link
encoder (training pattern registers, `enc1_configure_encoder`, and the VBIOS
TransmitterControl ENABLE / DISABLE / SETUP_VSEMPH, sent to the DMCUB as for
TMDS), the stream encoder (`enc1_stream_encoder_dp_set_stream_attribute`
MSA, `enc1_stream_encoder_dp_blank`, `enc32_stream_encoder_dp_unblank` M/N
and FIFOs) and `dcn31_program_pix_clk`'s DP branch (the DP DTO from
DPREFCLK). The mode set takes the link down as `link_set_dpms_off` does,
programs the timing, verifies the link cap once, and brings the link up as
`link_set_dpms_on`; a training that fails puts the old timing back.

Not ported: UHBR / 128b/132b (HPO encoder, DTBCLK), non-transparent LTTPR
training (an LTTPR is put in transparent mode), eDP, the source OUI write
(FEC, DSC and MST: step 6 below). The stream encoder keeps the pixels per cycle the GOP
chose (DC's `enable_dp_dig_pixel_rate_div_policy` would choose two on DCN
3.2), and the DCCG pixel rate divider stays as the GOP set it.

Gate: `rdna3dplink=1` (default off, not in the default profile) with
`rdna3modeset=1`; step `dplink` of the ladder. The simulator has a DP sink
with a link layer: it locks clock recovery after a set number of
voltage-swing requests, channel EQ after pre-emphasis requests, and fails
clock recovery at HBR3 to exercise the fallback.

## DSC, FEC and DP MST (step 6)

On top of the DP SST mode set, two optional parts, each behind its own
boot-arg and both needing `rdna3dplink=1`:

**FEC and DSC** (`rdna3dsc=1`, ladder step `dsc`). `hw/dcn32_dsc.c` ports
DC's DSC policy (`dc_dsc_parse_dsc_dpcd`, `dc_dsc_compute_config`,
`setup_dsc_config`, `decide_dsc_target_bpp_x16`, `dsc2_get_enc_caps`,
`dc_dsc_compute_bandwidth_range`), the PPS (`dsc2_get_packed_pps`:
`dscc_compute_dsc_parameters`, `calc_rc_params` with DC's QP tables,
`drm_dsc_compute_rc_parameters`, `drm_dsc_pps_payload_pack`; DC's floating
point rate control is computed on exact rationals, equal to DC's double
results for every bpp) and the hardware (`dsc2_set_config` /
`dsc_write_to_registers`, `dsc2_enable` / `_disable` / `_disconnect`,
`dcn32_dsc_pg_control` for DOMAIN16..19, `optc3_set_dsc_config`,
`enc32_dp_set_dsc_config`, `enc3_dp_set_dsc_pps_info_packet` through VPG
generic packets 11..14). `hw/dcn32_dp.c` reads the sink's FEC and DSC caps
(DPCD 0x090, 0x060..0x06f, branch 0x0a0..0x0a2), follows
`dp_should_enable_fec` (FEC ready before the training, FEC on after it, 97 %
link efficiency), `apply_dsc_policy_for_stream` (DSC when the mode does not
fit the link uncompressed) and `link_set_dsc_enable` / `dp_set_dsc_on_rx` /
`link_set_dsc_pps_packet` in `link_set_dpms_on` / `_off`. One DSC engine per
stream (the pipe's own, `dcn20_acquire_dsc`); DCN 3.2 has no DSCCLK DTO
(DSCCLK = DISPCLK / 3). Not ported: ODM combine (modes wider than 4096, so
no 8K), DSC over MST, YCbCr 4:2:2 / 4:2:0 DSC, eDP DSC.

**MST** (`rdna3mst=1`, ladder step `mst`). `hw/dp_mst.c` ports the DRM MST
helper parts a single stream needs: the sideband message layer (header CRC4,
body CRC8, 48-byte chunks to DOWN_REQ, replies polled through the ESI
registers), LINK_ADDRESS, ENUM_PATH_RESOURCES, CLEAR_PAYLOAD_ID_TABLE,
ALLOCATE_PAYLOAD, REMOTE_DPCD_READ and REMOTE_I2C_READ (the sinks' EDIDs),
the VC payload ID table (DPCD 0x1c0..0x1c2, 0x2c0), the ACT check, PBN and
time slots (`drm_dp_calc_pbn_mode`, `drm_dp_atomic_find_time_slots`).
`hw/dcn32_dp_mst.c` is the DC side: `allocate_mst_payload` /
`deallocate_mst_payload`, the stream allocation table and the DIG's
DP_MSE_SAT rows with the ACT trigger, DP_MSE_RATE (`slot X.Y`), DIG_MODE 5
and TransmitterControl in DP MST mode. The kext takes an SST head with an
MST capable sink over at the next mode switch: MST on, the topology probed,
every sink's EDID read (`RDNA3,Head<n>,MSTPorts`), the head's stream sent
to one sink (`rdna3mstport=N`, else the first); the modes offered are then
that sink's. When the first MST stream does not run, the head goes back to
SST. Not ported: more than one stream (a second head on the same link
needs a second DIG front end on the same back end, a second payload and
SAT row, and a pipe the clone path brings up for it), UP requests
(connection status notifications: the topology is read once), DSC over MST,
128b/132b MST.

The simulator models a DSC + FEC sink (the PPS the source sends is checked
against an independent double precision reference for 3840x2160@144 10 bpc
on HBR3 x4, 7680x4320@60 and a 12 bpp 1080p config against the VESA
values) and an MST hub with two sinks (synthetic EDIDs), which checks
every sideband CRC, answers the messages, keeps the payload table and
applies it at the ACT only when the source's SAT rows match it.

## Limitations (to do)

* one head (head 0), one mode; no EDID (DCN AUX/I2C), no change of
  resolution or refresh rate, no gamma;
* VBLANK interrupts from IH (now polling the frame counter);
* full DC (modesetting) later, a separate large stage.
