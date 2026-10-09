# AMD firmware for Navi 32

Stage 2 (bring-up) needs microcode from
[linux-firmware](https://gitlab.com/kernel-firmware/linux-firmware), directory `amdgpu/`.
The list for the RX 7800 XT (GC 11.0.3), confirmed in linux-firmware's `WHENCE` and in
the amdgpu driver's `MODULE_FIRMWARE()`:

| Block (IP) | Files |
|---|---|
| PSP (MP0 13.0.7) | `psp_13_0_7_sos.bin`, `psp_13_0_7_ta.bin` |
| SMU (MP1 13.0.7) | `smu_13_0_7.bin` |
| GFX (GC 11.0.3) | `gc_11_0_3_imu.bin`, `gc_11_0_3_pfp.bin`, `gc_11_0_3_me.bin`, `gc_11_0_3_mec.bin`, `gc_11_0_3_rlc.bin`, `gc_11_0_3_mes_2.bin`, `gc_11_0_3_mes1.bin` |
| SDMA 6.0.3 | `sdma_6_0_3.bin` |
| Display (DCN 3.2.1) | `dcn_3_2_1_dmcub.bin` |
| Video (VCN 4.0.4, optional) | `vcn_4_0_4.bin` |

Before use, confirm the IP versions on your card: `amd_discovery_dump` prints
the expected file prefixes based on the discovery table.

## License (`LICENSE.amdgpu`)

Key terms (full text: `LICENSES/LICENSE.amdgpu` in linux-firmware):

* redistribution is **allowed**, free of charge, **in binary form only**;
* the copyright notice, the license text and the disclaimer must be included;
* **no reverse engineering, decompilation or disassembly of the firmware.**

Consequences for the project:

1. The blobs are **not** committed to this repo (`.gitignore` blocks `firmware/*.bin`).
   The repo contains only MIT code.
2. Binary releases may include the firmware in `RDNA3Accel.kext/Contents/Resources/`
   together with `LICENSE.amdgpu`. This complies with the license, but its
   text must be included in the package.
3. The kext loads the firmware from its own resources (`OSKextRequestResource`), because macOS
   has no equivalent of `request_firmware()`.
4. We do not analyze the contents of the blobs. We take the interface to them (headers, load
   order, PSP registers) exclusively from the open amdgpu code.
   Reverse engineering in this project applies only to macOS interfaces
   (Stage 6), never to AMD firmware.
