# Plan: najtrudniejsze i najważniejsze części (kolejność wykonania)

Stan wyjściowy: commit `d333e25` na `stage1-kext`. Wszystko poniżej da się
pisać i sprawdzać bez karty (symulator `tests/sim_navi32.c` + CI), sprzęt
potwierdzi to dopiero przy pierwszych testach (`docs/STAGE3.md`).

Zasady bez zmian: port z amdgpu (MIT) z nazwą źródłowej funkcji w
komentarzu, każda część z testem w symulatorze, kod NullMoth tylko dla AMD,
commity z `[skip ci]` i jeden przebieg CI na paczkę zmian.

## 1. ✅ Przerwania MSI i fence'y z IH (b1f8132)
* `RDNA3Bringup`: własny `IOWorkLoop`, `IOFilterInterruptEventSource` na
  indeksie MSI z `IOPCIDevice::getInterruptType`, akcja = `ih60_process`
  → `ih60_iv_fence` → `IOLockWakeup`.
* Włączenie źródeł: `sdma60_set_trap_interrupt`, `gfx11_set_eop_interrupt`.
* `fenceWait`: `IOLockSleepDeadline` zamiast pętli, zapas 5 ms na zgubione
  przerwanie, bez trzymania `fDrmLock` w czasie snu.
* Brama `rdna3msi=1`; symulator: model generowania IV dla EOP i TRAP.

## 2. 🟡 Doorbelle + KIQ/KCQ zrobione (bez MES); MES i więcej kolejek później
* NBIO 4.3: apertura doorbelli (`nbio_v4_3_enable_doorbell_aperture`,
  zakresy SDMA/IH/VCN), mapowanie BAR2 w kexcie, `WDOORBELL64`.
* Ring GFX i SDMA przez doorbell (jak amdgpu), wptr w pamięci.
* MES 11 (`mes_v11_0.c`, `amdgpu_mes.c`): ładowanie przez PSP już jest;
  dochodzi ring MES, `SET_HW_RESOURCES`, `ADD_QUEUE`/`REMOVE_QUEUE`,
  MQD dla kolejek gfx/compute/sdma. Alternatywa na start: KIQ + MEC
  (`gfx_v11_0_kiq_*`, `compute_mqd_init`, `MAP_QUEUES`), bez MES.
* UAPI: `AMDGPU_HW_IP_COMPUTE` w `rdna3_drm.c`, osobne ringi na plik.
* Symulator: model KIQ/MES (pakiety MAP_QUEUES, MQD → HQD).

## 3. 🟡 Display Core z amdgpu (największy)
Cel: modesetting, EDID, wiele monitorów, hotplug, zmiana rozdzielczości.
* ✅ Krok 1: DP AUX (DPCD, I2C-over-AUX), HPD sense, EDID + parser trybów
  (`hw/dcaux.c`, `hw/edid.c`), sonda złączy w kexcie, `IODisplayEDID`
  przez framebuffer.
* ✅ Krok 2: interpreter AtomBIOS (`hw/atom.c`), tablica obiektów VBIOS
  (złącze → UNIPHY, DDC/AUX, HPD, możliwości enkodera).
* ✅ Krok 3: serwis DMCUB (`hw/dmub32.c`, `rdna3dmub=1`): reset ze STOP_FW,
  okna, inbox1, QUERY_FEATURE_CAPS, komendy VBIOS przez DMCUB.
* ✅ Krok 4a (obliczenia, bez zapisu rejestrów): `hw/dcn32_mode.c` —
  wartości OTG z trybu EDID (`optc1_program_timing`), przepustowość DP
  8b/10b, filtr trybów, rozpoznanie trybu GOP (`RDNA3,Head0EdidMode`,
  `RDNA3,Connector<n>,ModesUsable`).
  Dalej (wymaga karty): DML (watermarki, DLG/TTU, vstartup), zegar piksela
  (DCCG DTO), MSA enkodera strumienia, rozmiary HUBP/DPP, trening DP,
  `DIGxEncoderControl`/`SetPixelClock` przez DMCUB, modeset.
* Warstwa `dm_services` dla XNU (alokacja, logi, zegary, rejestry, delay)
  i kompilacja `drivers/gpu/drm/amd/display/dc` dla DCN 3.2.1 jako
  biblioteki C w kexcie (MIT). Najpierw `dc_create` + odczyt stanu, potem
  `dc_commit_streams`.
* DMCUB: ładowanie `dcn_3_2_1_dmcub.bin` (`dmub_srv`), bez niego DC na
  DCN 3.x nie działa.
* Link: AUX/I2C (EDID), trening DP, HDMI; HPD przez IH (źródła DCN).
* `RDNA3Framebuffer`: prawdziwa lista trybów z EDID, `setDisplayMode`,
  wiele głowic (jedna instancja na OTG), hotplug przez connect interrupt.
* Ryzyko: rozmiar (~150 tys. linii) i zależności od `drm_*`; plan B to
  własny mini-DC tylko dla DP+HDMI SST na DCN 3.2.

## 4. 🟡 SMU 13.0.7: zarządzanie energią (bring-up zrobiony, sleep później)
* ✅ `hw/vbios.c`: VBIOS przez okno ROM SMUIO, tablice ATOM; `hw/smu13.c`:
  `smu_smc_hw_setup` — PPTable z PMFW (combo) + BoardTable z VBIOS,
  `RunDcBtc`, `SetAllowedFeaturesMask`, `OverridePcieParameters`,
  `EnableAllSmuFeatures`, tablice DPM, limit PPT, workload, metryki
  (`SMU_TABLE_SMU_METRICS`). Kext: poziom 5, `rdna3pm=0/1/2`,
  `rdna3pmmask`; symulator modeluje PMFW (tablice, cechy, poziomy DPM).
* Domyślnie bez DPM UCLK/FCLK/DCN i bez GFXOFF — do czasu Display Core
  (patrz STAGE2 „PM bez Display Core”).
* Później: BACO/sleep, `IOPMPowerState` w kexcie, wybudzanie, przerwania
  termiczne (THM) przez IH.

## 5. ✅ Odporność: wykrywanie zawieszeń i reset GPU
* ✅ Timeout fence'a (10 s) → zrzut `GRBM_STATUS`/`CP_STAT`/`SDMA0_STATUS`
  do logu, VM joba „guilty”, `SQ_CMD` zabija fale (soft recovery), soft
  reset GFX (`gfx_v11_0_soft_reset` + ring 0/KIQ/KCQ od nowa), reset mode-1
  przez PMFW i `hwInit()` od poziomu 4; `VRAMLOST` po słowie kontrolnym.
* ✅ VM fault z IH (VMC/GFX źródło 0): adres, VMID, status L2, kontekst
  guilty, `AMDGPU_CTX_OP_QUERY_STATE2` i `-ECANCELED` jak w amdgpu.
* Zostaje: reset pojedynczej kolejki przez MES (`RESET_QUEUE`), gdy MES
  dojdzie; soft reset SDMA; `drop fDrmLock` w czasie czekania.

## 6. 🟡 UAPI dla RADV na serio
* ✅ Timeline syncobj, `SYNCOBJ_TRANSFER`, syncobj-fd w procesie
  (`drm_xnu`), `AMDGPU_CTX` z priorytetami i stanem resetu, `BO_LIST`,
  chunki CS (user fence zapisywany przez ring, BO GDS/GWS/OA z listy BO
  przełączane per VMID), `AMDGPU_INFO`
  z `SENSOR` (metryki SMU) i `GPUVM_FAULT`, `GEM_USERPTR` przez
  `IOMemoryDescriptor::withAddressRange`.
* ✅ `vulkaninfo` na runnerze macOS bez karty: `libdrm_loopback.dylib`
  w jobie `radv-macos` (uruchomienie `full: true`).
* ✅ syncobj sync_file między procesami (plik z fence po wartości,
  `RD_NR_XNU_SYNCOBJ_FENCE`); opaque fd zostaje w obrębie procesu.
* Zostaje: PRIME/IOSurface (eksport/import BO, wspólne z akceleratorem),
  dEQP compute przez loopback (potrzebuje
  wykonywania shaderów — dopiero na karcie).

## 7. Ścieżka WindowServer / Metal
* Akcelerator: powierzchnie IOSurface w VRAM, kompozycja przez flipy
  (już jest szkielet z NullMoth), `populateAccelConfig` pod RDNA3.
* Wtyczka Metal: przegląd translatora NullMoth pod różnice AMD (formaty,
  limity, tiling RDNA3), uruchomienie prostych przykładów Metal.

## Kolejność
1 → 2 → 4 → 5 → 6 → 3 → 7. DC jest największy, ale bez sprzętu trudno go
zweryfikować, a przejęcie trybu z GOP (`RDNA3Framebuffer`) wystarcza do
pierwszych testów. Każdy punkt kończy się kartą przesuniętą na tablicy
w Notion i wpisem w `docs/ROADMAP.md`.
