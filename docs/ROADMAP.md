# Plan

Legenda: ✅ zrobione · 🟡 w toku · ⬜ nie zaczęte

| Etap | Cel | Status |
|---|---|---|
| 0 | Stanowisko testowe, kontakt z projektem NVIDIA | 🟡 dokumentacja gotowa ([STAGE0](STAGE0.md)), projekt NVIDIA przeanalizowany ([NULLMOTH-ANALYSIS](NULLMOTH-ANALYSIS.md)); sprzęt i kontakt po Twojej stronie |
| 1 | Kext rozpoznaje kartę, mapuje BAR, czyta rejestry i discovery | 🟡 kod + testy gotowe ([STAGE1](STAGE1.md)); czeka na test na sprzęcie |
| 2 | Bring-up | 🟡 PSP + firmware + SMU, GMC (GART, MMHUB, GFXHUB), IH 6.0, GPUVM, SDMA 6.0, ring GFX 11 — w symulatorze ([STAGE2](STAGE2.md)); poziomy `rdna3lvl` 4–8; doorbelle, KIQ + KCQ0, MSI (`rdna3msi=1`), SMU PM (PPTable, DPM GFXCLK/SOCCLK, throttlery, wentylator, metryki; `rdna3pm`), zawieszenia → soft recovery / soft reset GFX / reset mode-1, VM fault → guilty ctx; MES, BACO/sleep do zrobienia |
| 3 | Wyświetlanie: DCN 3.2.1, IOFramebuffer | 🟡 przejęcie trybu z GOP, flipy, VBL z licznika klatek, CRC, DP AUX/HPD/EDID (nazwa monitora, lista trybów) w symulatorze; `RDNA3Framebuffer.kext` z NVRMFB ([STAGE3](STAGE3.md)); modesetting ⬜ |
| 4 | IOUserClient implementujący **UAPI amdgpu** | 🟡 CS na ringach DMA/GFX/COMPUTE, fence'y, syncobj (+TRANSFER, fd w procesie), GPUVM, USERPTR, SENSOR, GPUVM_FAULT, stan resetu ctx ([STAGE4](STAGE4.md)); niezmienione libdrm_amdgpu wysyła CS end-to-end na hoście; PRIME/IOSurface do zrobienia |
| 5 | Vulkan: RADV z istniejącym winsysem amdgpu | 🟡 `libvulkan_radeon.dylib` buduje się i linkuje na macOS w CI z 3 łatkami ([STAGE5](STAGE5.md)) |
| 6 | Metal: wtyczka + tłumacz z NullMoth | 🟡 zaimportowane, przystosowane do AMD, budują się w CI ([STAGE6](STAGE6.md)) |
| 7 | Integracja: WindowServer, CA, PM, sleep | 🟡 akcelerator IOAcceleratorFamily2 przeniesiony z NullMoth, buduje się w CI ([STAGE7](STAGE7.md)); PM/sleep ⬜ |
| 8 | Pakowanie, OpenCore, wydania | 🟡 `package/install.sh` / `uninstall.sh`; OpenCore i wydania ⬜ |

## Etap 1: warunek ukończenia

* kext ładuje się na Sequoi i dopasowuje do 7800 XT na wszystkich poziomach 0–3
  bez zawieszenia;
* `amd_discovery_dump` z blobu macOS == z blobu Linuksa (patrz STAGE1).

## Etap 2: kolejność prac (port z amdgpu)

Kolejność odpowiada `amdgpu_device_ip_init()` dla SOC21. Każdy krok najpierw
**czyta** stan, porównuje z Linuksem, dopiero potem **pisze**.

1. **Warstwa rejestrów**: tablica `reg_offset[hwip][inst][seg]` z discovery,
   makra jak `SOC15_REG_OFFSET`/`RREG32_SOC15`, dostęp SMN (pośredni przez NBIO 4.3).
2. **Firmware**: ładowanie z zasobów kextu, parsowanie nagłówków `amdgpu_firmware_header`
   (`amdgpu_ucode.h`, MIT).
3. **NBIO 4.3 / HDP 6.x**: doorbelle, flush HDP.
4. **GMC 11 + MMHUB 3.0.x / GFXHUB 11** (dokładne wersje z discovery): rozkład VRAM, aperture, tablice stron GPUVM
   (VMID0 dla jądra), bufor ochronny (dummy page).
5. **PSP 13.0.7**: rozruch SOS, ring PSP, ładowanie TMR, a potem za pomocą PSP firmware GFX,
   SDMA, SMU, DMCUB (front-door loading, jak amdgpu dla RDNA3).
6. **SMU 13.0.7**: ✅ `smu_smc_hw_setup` (PPTable z PMFW + BoardTable z VBIOS,
   cechy, DPM, PPT, workload, metryki); bez overdrive; DPM pamięci/DCN czeka
   na Display Core.
7. **IH 6.0.x (OSSSYS)**: ring przerwań, MSI w IOKit (`IOInterruptEventSource` z MSI).
8. **GFX 11 / MES**: RLC, IMU, MES (na RDNA3 kolejki zarządza MES), ring KIQ;
   **SDMA 6.0**: pierwszy ring.
9. **Test**: `WRITE_DATA` na ringu GFX/SDMA do bufora w VRAM i odczyt przez
   `MM_INDEX`, potem fence + przerwanie.

Sprzęt jest MIT-owy w amdgpu, ale kod jest pisany pod linuksowe API (DRM, TTM,
`dma_fence`, kref). Plan: cienka warstwa zgodności (`linux_compat/`), tylko tyle,
ile potrzebują przeportowane pliki, a nie emulacja całego jądra Linuksa.

## Etapy 3–8: znane decyzje i ryzyka

* **DC (display core)** jest w dużej mierze niezależny od systemu operacyjnego
  (`dc/` + `dmub/`), więc port przez warstwę `dm_services`, wzorem amdgpu_dm,
  jest realny. IOFramebuffer jest przestarzały, ale wciąż działa na x86 w Sequoi.
* **Etap 4: decyzja po analizie NullMoth.** Nie projektujemy własnego ABI.
  IOUserClient przyjmuje linuksowe ioctl amdgpu (`amdgpu_drm.h`, MIT) przez
  `IOConnectCallMethod`, mmap idzie przez `IOConnectMapMemory64`, a w userspace
  cienka warstwa podmienia `open/ioctl/mmap`. Ten sam układ działa u NullMoth dla NVK.
  Zysk: RADV i jego testy działają bez przepisywania winsysu, a każdy ioctl
  można porównać 1:1 z Linuksem.
* **Etap 5** zaczyna się od `vulkaninfo` i testów compute, które na Linuksie
  przechodzą na tej samej karcie: ten sam strumień ioctl, więc różnice wskazują
  błąd w kexcie.
* **Etap 6**: NullMoth pokazał już, jak Metal znajduje wtyczkę (`MetalPluginName`,
  `MetalPluginClassName` na usłudze IOAccelerator) i jak wygląda `MTLDevice` nad Vulkanem.
  Ich wtyczka jest na PolyForm Noncommercial, więc albo wspólny projekt z autorem, albo
  własna implementacja. Reverse engineering prywatnych interfejsów Apple
  (bundle AMD, IOAccelerator) jest potrzebny w mniejszym zakresie niż zakładał plan.
  Niepewność prawna: interoperacyjność zwykle przemawia za RE interfejsów, ale
  to nie jest porada prawna.
* **Horyzont**: macOS 26 jest ostatnim wydaniem na x86_64, więc użyteczność
  projektu kończy się wraz z jego wsparciem.
