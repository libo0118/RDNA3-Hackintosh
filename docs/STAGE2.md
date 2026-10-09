# Etap 2: bring-up sprzętu

Stan: **PSP + firmware + SMU + GMC (GART, MMHUB 3.0.2, GFXHUB 3.0.3)
+ IH 6.0 + SDMA 6.0 napisane i przetestowane w symulatorze**; GFX/MES
jeszcze nie.

## Co jest zrobione

| Część | Plik | Odpowiednik w amdgpu | Test |
|---|---|---|---|
| Rejestry z IP discovery, MMIO + SMN, HDP flush | `hw/rdna3_hw.c` | `amdgpu_discovery_reg_base_init`, `amdgpu_device_rreg`, `nbio_v4_3`, `amdgpu_hdp_generic_flush` | `tests/sim_navi32.c` |
| Lokalizacja FB (MC adres VRAM) | `hw/rdna3_hw.c` | `gfxhub_v3_0_3_get_fb_location` | sim |
| Firmware: podział SOS, plan LOAD_IP_FW | `common/amd_fw.c` | `psp_init_sos_microcode`, `amdgpu_ucode_init_single_fw` | `tests/test_fw.c` + prawdziwe pliki z linux-firmware w CI |
| PSP 13.0.7: bootloader (KDB/SPL/SYS/SOS), ring KM, TOC/TMR, LOAD_IP_FW (29 ucode), RLC autoload | `hw/psp13.c` | `psp_v13_0.c`, `amdgpu_psp.c` | sim (syntetyczny i prawdziwy firmware) |
| SMU 13.0.7: mailbox, TestMessage/GetSmuVersion/GetDriverIfVersion | `hw/smu13.c` | `smu_cmn.c` | sim |
| Uruchomienie w kexcie (wątek, firmware z Resources) | `kext/src/RDNA3Bringup.cpp` | `amdgpu_device_init` (fragment) | CI build |

## Uruchomienie na sprzęcie

1. Zbuduj kext i dołóż firmware (nie trafia do gita, patrz `docs/FIRMWARE.md`):

   ```sh
   make kext
   make -C kext firmware FW_DIR=/ścieżka/do/linux-firmware/amdgpu
   ```

2. boot-args: `rdna3lvl=4` (PSP + firmware), `rdna3lvl=5` (+ SMU) lub
   `rdna3lvl=6` (+ GMC: GART w VRAM, MMHUB, flush TLB; `RDNA3,GMCResult`;
   + ring IH 6.0, `RDNA3,IHResult`, `RDNA3,IHEarlyIVs`) lub `rdna3lvl=7`
   (+ SDMA0: ring, test ringu, kopia 64 KiB VRAM→VRAM sprawdzona przez CPU;
   `RDNA3,SDMAResult`, `RDNA3,SDMACopyMismatches`). **Poziom 7 to pierwszy,
   na którym GPU sam wykonuje polecenia.** `rdna3lvl=8` dodaje GFX 11:
   sprawdzenie autoloadu RLC, adresy startowe RS64, GFXHUB, golden registers,
   SH_MEM, CSB + SRM, ring GFX0 z clear state i testem `SCRATCH_REG0`
   (`RDNA3,GFXResult`).
3. Wyniki w IORegistry (`ioreg -l -r -c RDNA3Accel`): `RDNA3,Stage2`,
   `RDNA3,PSPResult`, `RDNA3,SOSVersion`, `RDNA3,SMUVersion`, `RDNA3,FBStartMC`.
   Log: `log show --last boot --predicate 'eventMessage CONTAINS "RDNA3:"'`.

Oczekiwane na RX 7800 XT: `sOS running, version 0x00330024` (zależnie od
wersji linux-firmware), 29× `LOAD_IP_FW ... ok`, `RLC autoload started`,
`SMU: PMFW 82.97.0` (lub podobnie).

## Decyzje i ryzyka (do sprawdzenia na sprzęcie)

* **Wszystkie bufory PSP w VRAM** (zapis przez BAR0 + HDP flush). amdgpu
  wspiera to (`debug_use_vram_fw_buf`), dzięki czemu PSP startuje bez GART.
* **Okno alokacji 16 MiB–512 MiB VRAM**, w części widocznej przez BAR0.
  Pierwsze 16 MiB pomijamy (framebuffer przed-OS), górę VRAM też (discovery,
  rezerwacje IFWI). amdgpu czyta dokładne rezerwacje z tablic ATOM
  (`vram_usagebyfirmware`). To jeszcze nie jest zaimplementowane.
* **Bez długiego treningu DRAM** (`psp_v13_0_memory_training`). IFWI trenuje
  pamięć przy włączeniu zasilania. Jeśli wystąpią błędy pamięci, to pierwszy
  podejrzany.
* **SPL ładuje bajty KDB**: tak robi `psp_v13_0_bootloader_load_spl` w Linuksie.
  Zostawione 1:1.
* **sOS startuje >20 ms**: Linux czeka 20 ms i dopiero potem obserwuje zmianę
  C2PMSG_81. Symulator modeluje to z wirtualnym zegarem.
* **MES1 (KIQ)**: ładujemy, jeśli plik jest obecny. amdgpu dla GC 11 może go
  pomijać zależnie od `enable_mes_kiq`.

## Dalej (kolejność jak w `amdgpu_device_ip_init`)

1. ✅ **GMC 11 / MMHUB 3.0.2 / GFXHUB 3.0.3**: GART w VRAM, VMID0 i
   konteksty 1–15, flush TLB (MMHUB z semaforem i prywatną inwalidacją).
   ✅ Tablice stron per-VM (`hw/gpuvm.c`): PDB2→PDB1→PDB0→PTB w VRAM z puli
   16 MiB, VMID 1–15 na otwarty plik DRM, `GEM_VA` zapisuje PTE (VRAM: adres
   fizyczny VRAM, GTT: strony systemowe + SNOOPED), flush TLB po każdej
   zmianie. Działa od `rdna3lvl=6`; niżej mapowania są tylko zapamiętywane.
2. 🟡 **IH 6.0** (`hw/ih60.c`): ring 0 w VRAM, writeback wptr, dekodowanie IV,
   obsługa przepełnienia; odpytywany. Zostaje: MSI w IOKit
   (`registerInterrupt`), ring 1 (page faulty), doorbell rptr.
3. 🟡 **GFX 11** (`hw/gfx11.c`): ring GFX0 przez rejestry `CP_RB0_*` (jak
   amdgpu z `amdgpu_mes=0`), clear state, test ringu, IB + fence
   `RELEASE_MEM`; CS na `AMDGPU_HW_IP_GFX` od `rdna3lvl=8`. Zostaje: VM flush /
   ACQUIRE_MEM wokół jobów, KIQ + kolejki compute (MEC) albo MES, doorbelle.
4. 🟡 **SDMA 6.0** (`hw/sdma60.c`): ring QUEUE0 w VRAM, test ringu, kopia,
   fill, fence; bez doorbelli (wptr przez rejestr + slot poll). Zostaje:
   IB (`INDIRECT`), przerwanie TRAP przez IH, SDMA1, doorbelle.
5. Wtedy `AMDGPU_CS` w `uapi/` dostaje prawdziwą implementację, a tryb
   `rdna3enum=1` przestaje być potrzebny.
