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
| VBIOS przez okno ROM SMUIO, tablice ATOM (firmwareinfo, smu_info, smc_dpm_info, vram_usagebyfirmware) | `hw/vbios.c` | `amdgpu_bios.c`, `atom.c`, `amdgpu_atomfirmware.c` | sim (syntetyczny obraz) |
| Odporność: dump stanu (GRBM/CP/SDMA), zabicie fal VMID (`SQ_CMD`), soft reset GFX (`GRBM_SOFT_RESET` + cp_resume), reset mode-1 przez debug mailbox PMFW + ponowny bring-up od poziomu 4, VM fault z IH (adres, VMID, status L2) → kontekst „guilty”, flagi `AMDGPU_CTX_OP_QUERY_STATE2`, `-ECANCELED` | `hw/gfx11.c`, `hw/gmc11.c`, `hw/smu13.c`, `kext/src/RDNA3Bringup.cpp` | `gfx_v11_0_soft_reset`, `gfx_v10_0_ring_soft_recovery`, `smu_v13_0_7_mode1_reset`, `gmc_v11_0_process_interrupt`, `amdgpu_job_timedout`, `amdgpu_ctx_query2` | sim (zawieszenie CP, fault IB, mode-1) |
| SMU 13.0.7 PM: tablice driver/tool w VRAM, combo PPTable z PMFW + BoardTable z VBIOS, `TransferTableDram2Smu`, `RunDcBtc`, `SetAllowedFeaturesMask`, `OverridePcieParameters`, `EnableAllSmuFeatures`, tablice DPM, limit PPT, workload, metryki (`SMU_TABLE_SMU_METRICS`) | `hw/smu13.c` | `amdgpu_smu.c` (`smu_smc_hw_setup`), `smu_v13_0.c`, `smu_v13_0_7_ppt.c` | sim (model PMFW: tablice, cechy, poziomy DPM) |
| Uruchomienie w kexcie (wątek, firmware z Resources) | `kext/src/RDNA3Bringup.cpp` | `amdgpu_device_init` (fragment) | CI build |

## Uruchomienie na sprzęcie

1. Zbuduj kext i dołóż firmware (nie trafia do gita, patrz `docs/FIRMWARE.md`):

   ```sh
   make kext
   make -C kext firmware FW_DIR=/ścieżka/do/linux-firmware/amdgpu
   ```

2. boot-args: `rdna3lvl=4` (PSP + firmware), `rdna3lvl=5` (+ SMU; domyślnie
   `rdna3pm=1` czyta VBIOS i uruchamia zarządzanie energią PMFW — zestaw
   „bezpieczny” bez DPM pamięci/fabric/DCN, bo bez Display Core nikt nie mówi
   PMFW, kiedy przełączenie p-state nie zepsuje skanowania; `rdna3pm=0`
   zostawia zegary rozruchowe, `rdna3pm=2` pełny zestaw amdgpu,
   `rdna3pmmask=<hex>` maska cech wprost; wyniki: `RDNA3,PM`,
   `RDNA3,PMResult`, `RDNA3,PMFeaturesRunning`, `RDNA3,PMGfxClkMaxMHz`,
   `RDNA3,PMPowerLimitW`, `RDNA3,VBIOSPartNumber`, a po bring-upie metryki
   `RDNA3,PMGfxClkMHz`, `RDNA3,PMTempHotspotC`, `RDNA3,PMPowerW`,
   `RDNA3,PMFanRPM`) lub
   `rdna3lvl=6` (+ GMC: GART w VRAM, MMHUB, flush TLB; `RDNA3,GMCResult`;
   + ring IH 6.0, `RDNA3,IHResult`, `RDNA3,IHEarlyIVs`) lub `rdna3lvl=7`
   (+ SDMA0: ring, test ringu, kopia 64 KiB VRAM→VRAM sprawdzona przez CPU;
   `RDNA3,SDMAResult`, `RDNA3,SDMACopyMismatches`). **Poziom 7 to pierwszy,
   na którym GPU sam wykonuje polecenia.** `rdna3lvl=8` dodaje GFX 11:
   sprawdzenie autoloadu RLC, adresy startowe RS64, GFXHUB, golden registers,
   SH_MEM, CSB + SRM, ring GFX0 z clear state i testem `SCRATCH_REG0`
   (`RDNA3,GFXResult`). Od poziomu 6 kext mapuje BAR2 i włącza aperturę
   doorbelli (`RDNA3,Doorbells`); wtedy SDMA0 i GFX0 piszą wptr przez
   doorbell, a na poziomie 8 startują KIQ (MEC1 pipe 1) i pierwsza kolejka
   compute KCQ0 (pipe 0) mapowana przez `MAP_QUEUES` (`RDNA3,KIQResult`,
   `RDNA3,KCQResult`; CS na `AMDGPU_HW_IP_COMPUTE`). `rdna3doorbell=0`
   zostawia rejestry wptr i pomija compute. `rdna3msi=1` (od poziomu 6) rejestruje MSI karty
   w IOKit: ring IH jest opróżniany z przerwania, a oczekiwanie na fence
   śpi do wybudzenia przez handler (zapas 5 ms na zgubione przerwanie).
   Poziom 7 sprawdza pierwsze przerwanie trap z SDMA (`RDNA3,MSIVerified`);
   gdy nie dojdzie w 200 ms, sterownik wraca do odpytywania. Liczniki:
   `RDNA3,IRQs`, `RDNA3,IRQIVs`, `RDNA3,IRQFenceGFX`, `RDNA3,IRQFenceSDMA`.
3. Wyniki w IORegistry (`ioreg -l -r -c RDNA3Accel`): `RDNA3,Stage2`,
   `RDNA3,PSPResult`, `RDNA3,SOSVersion`, `RDNA3,SMUVersion`, `RDNA3,FBStartMC`.
   Log: `log show --last boot --predicate 'eventMessage CONTAINS "RDNA3:"'`.

Oczekiwane na RX 7800 XT: `sOS running, version 0x00330024` (zależnie od
wersji linux-firmware), 29× `LOAD_IP_FW ... ok`, `RLC autoload started`,
`SMU: PMFW 82.97.0` (lub podobnie).

## Zawieszenia, reset, błędy stron

`fenceWait` daje każdemu fence'owi 10 s. Potem (`amdgpu_job_timedout`):
zrzut stanu do logu (`GFX timeout: GRBM_STATUS ...`), VM ostatniego joba na tym
ringu zostaje „guilty” (jego `CS` kończą się `-ECANCELED`, `AMDGPU_CTX_OP_QUERY_STATE2`
zwraca `GUILTY|RESET`), `SQ_CMD` zabija fale tego VMID i czeka 100 ms na
fence. Gdy to nie pomaga: fence'y wszystkich ringów są dopełniane, GFX
przechodzi soft reset (`gfx_v11_0_soft_reset` + ring 0, KIQ, KCQ od nowa),
a gdy i to zawiedzie (albo zawiesił się SDMA) — reset mode-1 przez PMFW i
`hwInit()` od poziomu 4 (PSP, firmware, SMU, GMC, IH, SDMA, GFX). Tablice
stron VM zostają (VRAM przeżywa mode-1; słowo kontrolne `RDN3` to sprawdza,
inaczej klienci dostają `VRAMLOST`). Błędy stron GPUVM (klient VMC/GFX,
źródło 0) są dekodowane z IV (`gmc11_decode_vm_fault`: hub, VMID, adres,
`L2_PROTECTION_FAULT_STATUS`) i też oznaczają VM jako guilty. Liczniki w
IORegistry: `RDNA3,VMFaults`, `RDNA3,LastFaultAddr`, `RDNA3,SoftRecoveries`,
`RDNA3,Resets`, `RDNA3,GPUResets`, `RDNA3,VRAMLost`.

## Decyzje i ryzyka (do sprawdzenia na sprzęcie)

* **Wszystkie bufory PSP w VRAM** (zapis przez BAR0 + HDP flush). amdgpu
  wspiera to (`debug_use_vram_fw_buf`), dzięki czemu PSP startuje bez GART.
* **Okno alokacji 16 MiB–512 MiB VRAM**, w części widocznej przez BAR0.
  Pierwsze 16 MiB pomijamy (framebuffer przed-OS), górę VRAM też (discovery,
  rezerwacje IFWI). amdgpu czyta dokładne rezerwacje z tablic ATOM
  (`vram_usagebyfirmware`); `vbios_fw_vram_reservation()` już je czyta,
  alokator jeszcze z nich nie korzysta.
* **PM bez Display Core**: amdgpu włącza DPM UCLK/FCLK/DCN, bo jego DC
  programuje watermarki i mówi PMFW (przez DMCUB), kiedy p-state może się
  przełączyć. My zostawiamy skanowanie z GOP nietknięte, więc domyślnie te
  cechy są wyłączone (`RD_SMU_FEATURES_SAFE`), a GFXOFF nigdy — nikt nie
  wysyła `AllowGfxOff`. Pełny zestaw (`rdna3pm=2`) to test na własne ryzyko:
  możliwe migotanie/underflow obrazu.
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
   obsługa przepełnienia; odpytywany albo z MSI (`rdna3msi=1`,
   `RPTR_REARM`, fence'y GFX EOP / SDMA trap → `ih60_iv_fence`). Zostaje:
   ring 1 (page faulty), doorbell rptr, VM fault → zabicie kontekstu.
3. 🟡 **GFX 11** (`hw/gfx11.c`): ring GFX0 przez rejestry `CP_RB0_*` (jak
   amdgpu z `amdgpu_mes=0`), clear state, test ringu, joby w kolejności
   `amdgpu_ib_schedule` (VM flush, ACQUIRE_MEM, HDP, ramki, fence); KIQ i
   KCQ0 na MEC (MQD w VRAM, `kiq_init_register`, `SET_RESOURCES` +
   `MAP_QUEUES`, `UNMAP_QUEUES` przy zamknięciu); CS na
   `AMDGPU_HW_IP_GFX`/`COMPUTE` od `rdna3lvl=8`. Zostaje: MES, więcej
   kolejek compute, priorytety.
4. 🟡 **SDMA 6.0** (`hw/sdma60.c`): ring QUEUE0 w VRAM, test ringu, kopia,
   fill, fence, IB (`INDIRECT`), TRAP po fence przy MSI; wptr przez
   doorbell (`S2A_DOORBELL_ENTRY_2` przez RSMU) albo rejestr. Zostaje: SDMA1.
5. Wtedy `AMDGPU_CS` w `uapi/` dostaje prawdziwą implementację, a tryb
   `rdna3enum=1` przestaje być potrzebny.
