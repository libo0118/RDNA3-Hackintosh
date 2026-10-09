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
| `SYNCOBJ_CREATE/DESTROY/RESET/SIGNAL/WAIT/TIMELINE_*/QUERY` | ✅ bez prawdziwych fence'ów (sygnał = natychmiast) |
| `SYNCOBJ_HANDLE_TO_FD/FD_TO_HANDLE/TRANSFER`, `PRIME_*` | ⬜ `-ENOSYS` (wymaga IOSurface/eksportu, Etap 4b) |
| `AMDGPU_GEM_CREATE` | ✅ GTT: `IOBufferMemoryDescriptor`; VRAM: sterta w VRAM (od 512 MiB), mapowalna przez BAR0 |
| `AMDGPU_GEM_MMAP` + `mmap` | ✅ przez `clientMemoryForType` |
| `AMDGPU_GEM_VA` | ✅ MAP/UNMAP/CLEAR/REPLACE; na `rdna3lvl>=6` zapis prawdziwych tablic stron (`hw/gpuvm.c`), mapowanie trzyma referencję BO, `GEM_CLOSE` zdejmuje mapowania jak amdgpu |
| `AMDGPU_GEM_METADATA/OP/WAIT_IDLE`, `AMDGPU_VM`, `AMDGPU_CTX` | ✅ |
| `AMDGPU_INFO` | ✅ ACCEL_WORKING, DEV_INFO (z tabeli GC discovery), HW_IP_INFO/COUNT, FW_VERSION, VRAM_GTT, MEMORY, *_USAGE, READ_MMR_REG (biała lista soc21), MAX_IBS, VIDEO_CAPS |
| `AMDGPU_CS`, `BO_LIST`, `WAIT_FENCES`, `FENCE_TO_HANDLE` | ⬜ `-ENODEV` do czasu ringów |
| `GEM_USERPTR` | ⬜ `-ENOSYS` |

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
* Na Macu: `build/userspace/rdna3_drminfo` (kext na `rdna3lvl>=2`).

## Do sprawdzenia na sprzęcie

* `RDNA3,GBAddrConfig` w ioreg (`rdna3lvl>=3`): wartość zastępcza `0x545`
  to tylko placeholder; RADV liczy z niej swizzle tekstur.
* Harvesting CU/RB: `DEV_INFO` zakłada wszystkie 60 CU/12 RB aktywne
  (`CC_GC_SHADER_ARRAY_CONFIG` do odczytania w Etapie 2).
