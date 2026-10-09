# Etap 4: interfejs jądro–użytkownik (UAPI amdgpu)

Kext nie ma własnego ABI: implementuje **linuksowe UAPI amdgpu** (struktury
z `amdgpu_drm.h`/`drm.h`), a userspace woła je dokładnie tak jak na Linuksie.

```
RADV / libdrm_amdgpu (bez zmian)
   │  drmIoctl / mmap
libdrm.dylib = xf86drm_xnu.c + drm_xnu.c      (userspace/libdrm_xnu)
   │  IOConnectCallMethod(sel 0: nr, payload) / IOConnectMapMemory64
RDNA3UserClient → RDNA3Accel::drmIoctl         (kext/src)
   │
uapi/rdna3_drm.c  (niezależne od OS, testowane na hoście)
```

## Status ioctl

| ioctl | Stan |
|---|---|
| `DRM_IOCTL_VERSION`, `GET_CAP`, `SET_CLIENT_CAP` | ✅ (`amdgpu` 3.61) |
| `GEM_CLOSE` | ✅ |
| `SYNCOBJ_CREATE/DESTROY/RESET/SIGNAL/WAIT/TIMELINE_*/QUERY` | ✅ syncobj niesie fence CS i sygnalizuje się, gdy GPU go minie |
| `SYNCOBJ_TRANSFER` | ✅ kopia fence'a/punktu (snapshot, syncobj są per plik) |
| `SYNCOBJ_HANDLE_TO_FD/FD_TO_HANDLE` | ✅ w `libdrm_xnu` (`drm_xnu.c`): kext nie ma deskryptorów, więc eksport to fd `/dev/null` w tablicy procesu; sync_file = snapshot przez `TRANSFER`, opaque = ten sam handle. Tylko w obrębie procesu (RADV, `VK_KHR_external_semaphore_fd`); między procesami ⬜ |
| `PRIME_*` | ⬜ `-ENOSYS` (wymaga IOSurface/eksportu, Etap 7) |
| `AMDGPU_GEM_CREATE` | ✅ GTT: `IOBufferMemoryDescriptor`; VRAM: sterta w VRAM (od 512 MiB), mapowalna przez BAR0 |
| `AMDGPU_GEM_MMAP` + `mmap` | ✅ przez `clientMemoryForType` |
| `AMDGPU_GEM_VA` | ✅ MAP/UNMAP/CLEAR/REPLACE; na `rdna3lvl>=6` zapis prawdziwych tablic stron (`hw/gpuvm.c`), mapowanie trzyma referencję BO, `GEM_CLOSE` zdejmuje mapowania jak amdgpu |
| `AMDGPU_GEM_METADATA/OP/WAIT_IDLE`, `AMDGPU_VM`, `AMDGPU_CTX` | ✅ |
| `AMDGPU_INFO` | ✅ ACCEL_WORKING, DEV_INFO (z tabeli GC discovery), HW_IP_INFO/COUNT, FW_VERSION, VRAM_GTT, MEMORY, *_USAGE, READ_MMR_REG (biała lista soc21), MAX_IBS, VIDEO_CAPS, SENSOR (z metryk SMU: zegary, temperatura, obciążenie, moc, VDDGFX, pstate), GPUVM_FAULT (ostatni błąd stron VM klienta) |
| `AMDGPU_CS` | ✅ chunki IB / DEPENDENCIES / SYNCOBJ_IN/OUT / TIMELINE / BO_HANDLES; ring DMA prawdziwy od `rdna3lvl=7` (SDMA0, IB w VMID pliku), GFX/COMPUTE tylko ogłaszane (`rdna3enum`) |
| `WAIT_CS`, `WAIT_FENCES`, `FENCE_TO_HANDLE` (syncobj), `BO_LIST` | ✅ prawdziwe fence'y SDMA; termin jako CLOCK_MONOTONIC (`mach_continuous_time`), limit 10 s |
| `GEM_USERPTR` | ✅ `IOMemoryDescriptor::withAddressRange` nad taskiem klienta, `prepare()` na czas życia BO, strony przez `IODMACommand`; bez `mmap` (`-EPERM` jak amdgpu); `amdgpu_create_bo_from_user_mem` w teście loopback |
| (wszystkie) | errno w numeracji Linuksa z UAPI; `drm_xnu` tłumaczy na macOS (`ETIME` 62→92, `ENOSYS` 38→78, `EOPNOTSUPP` 95→102), bo RADV porównuje z `-ETIME`/`-ECANCELED` hosta |
| `AMDGPU_CTX_OP_QUERY_STATE/STATE2` | ✅ RESET / VRAMLOST / GUILTY z resetów i błędów stron (`vm_query`), guilty → `CS` = `-ECANCELED` |
| `AMDGPU_CS` chunk `FENCE` (user fence) | 🟡 przyjmowany i pomijany: RADV nie czyta `fence_map`, używa syncobj i `WAIT_CS` |

## Tryb enumeracji (`rdna3enum=1`)

`libdrm_amdgpu` odmawia inicjalizacji, gdy `ACCEL_WORKING == 0`. Boot-arg
`rdna3enum=1` zgłasza `accel_working` i po jednym ringu GFX/COMPUTE/DMA, więc
`vulkaninfo` i ścieżki alokacji pamięci działają przed Etapem 2. Każdy `CS`
dalej zwraca `-ENODEV`.

## Testy

* `tests/test_uapi.c`: ioctl na poziomie struktur.
* `tests/test_libdrm_amdgpu.c`: **niezmienione libdrm_amdgpu** przez
  `drm_xnu` z backendem loopback (UAPI w tym samym procesie). Obejmuje
  `amdgpu_device_initialize` (z jego `F_DUPFD`), `amdgpu_query_gpu_info`,
  BO + mmap + zapis/odczyt, VA, konteksty, syncobj, zamknięcie oryginalnego
  fd jak w RADV.
* `tests/test_libdrm_amdgpu.c` wysyła też **CS przez niezmienione libdrm_amdgpu**
  (`amdgpu_cs_submit_raw2` + `amdgpu_cs_query_fence_status`) na programowy
  SDMA backendu loopback, który interpretuje pakiety WRITE/COPY/FILL/FENCE.
* Na Macu: `build/userspace/rdna3_drminfo` (kext na `rdna3lvl>=2`);
  `rdna3_drminfo --dma-test` (`rdna3lvl=7`) wysyła IB SDMA z BO w VRAM i
  sprawdza, co zapisało GPU: pierwsze zadanie z userspace na karcie.

## Do sprawdzenia na sprzęcie

* `RDNA3,GBAddrConfig` w ioreg (`rdna3lvl>=3`): wartość zastępcza `0x545`
  to tylko placeholder; RADV liczy z niej swizzle tekstur.
* Harvesting CU/RB: od `rdna3lvl=3` kext czyta maski jak
  `gfx_v11_0_get_cu_info`/`setup_rb` (`CC/GC_USER_SA_UNIT_DISABLE`,
  `CC/GC_USER_RB_BACKEND_DISABLE`, `CC/GC_USER_SHADER_ARRAY_CONFIG` na SE/SA
  wybranym przez `GRBM_GFX_INDEX`) i podmienia nimi domyślne „wszystko
  aktywne” w `DEV_INFO` (`RDNA3,ActiveCUs`, `RDNA3,ActiveRBMask`). RX 7800 XT
  powinien pokazać 60 CU; inna liczba znaczy, że trzeba porównać z Linuksem.
