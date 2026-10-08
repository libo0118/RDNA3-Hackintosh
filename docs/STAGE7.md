# Etap 7: integracja z systemem (akcelerator IOAcceleratorFamily2)

Metal i WindowServer szukają karty przez **IOAccelerator**: usługę klasy
`IOGraphicsAccelerator2` z właściwościami `MetalPluginName` /
`MetalPluginClassName`. Dostarcza ją osobny kext `RDNA3Accelerator.kext`
(`accel/`), przeniesiony z akceleratora NullMoth (zob. `accel/PROVENANCE.md`).

```
Metal.framework ── MetalPluginName ──▶ /Library/GPUBundles/RDNA3MTLDriver.bundle (Etap 6)
      │                                        │ RADV (Etap 5) → libdrm_xnu → RDNA3UserClient
IOAcceleratorFamily2
      │
RDNA3Accelerator.kext (accel/)   ── callPlatformFunction (accel/rdna3k_abi.h) ──▶ RDNA3Accel.kext
      │  VRAM, uchwyt urządzenia, zakresy GPU VA                                  (kext/src/RDNA3Platform.cpp)
      └─ scanout, flip, CRC ──▶ RDNA3Framebuffer (Etap 3, jeszcze nie istnieje)
```

```sh
make -C accel                 # build/RDNA3Accelerator.kext (Sequoia)
make -C accel NM_TAHOE=1      # układ vtable dla macOS 26
make -C accel check           # sprawdza, że nie został żaden metaclass NV*
```

## Bramki (jak w NullMoth)

| Co | Jak |
|---|---|
| start akceleratora | boot-arg `rdna3accel=1` (bez niego `start()` zwraca false: ochrona przed pętlą rozruchu) |
| wyłączenie wszystkiego | `-rdna3off` |
| kolejność | czeka do 30 s, aż `RDNA3Accel` ustawi `rdna3-claimed` na IOPCIDevice (robi to po udanym `drmInit`, czyli `rdna3lvl>=2`) |
| wtyczka Metal | `MetalPluginName` jest ukrywane przy starcie; `sudo sysctl -w debug.rdna3accelfb=1` je przywraca (dopiero po zarejestrowaniu framebuffera) |
| display pipe | `debug.rdna3accelfb=3`, `debug.rdna3accel_iop=1`, jak w NullMoth |

## Interfejs akcelerator ↔ RDNA3Accel

| Funkcja | Stan |
|---|---|
| `rdna3AllocVram` / `rdna3FreeVram` | ✅ ta sama sterta VRAM co GEM, tylko w zakresie BAR0 (kva + phys) |
| `rdna3DeviceHandles` | ✅ cookie urządzenia |
| `rdna3GpuVaAlloc` / `Free` | ✅ zakres 1 TiB od 64 TiB |
| `rdna3GpuVaMap` | 🟡 zapamiętuje tylko zakres; tablice stron po GPUVM (Etap 2) |
| `rdna3NewAccelClient` | ⬜ `kIOReturnUnsupported`; RADV otwiera `RDNA3Accel` bezpośrednio |
| `rdna3ScanoutInfo`, `rdna3FlipToSurface`, `rdna3DisplayCrc` | ⬜ po stronie framebuffera (Etap 3) |
| `rdna3SurfVram` / `rdna3SurfShare` / `rdna3SurfPageoff` | ⬜ współdzielenie IOSurface (Etap 4b) |

## Co dalej

1. Pierwszy test na sprzęcie: `rdna3lvl=2 rdna3enum=1 rdna3accel=1`, druga
   karta na ekranie. Oczekiwane: `ioreg -c RDNA3Accelerator` pokazuje usługę,
   log `IOGraphicsAccelerator2::start -> 1`.
2. `populateAccelConfig`: wartości (flagi, limity tekstur) są z NVIDII.
   RDNA3 obsługuje 16384×16384, więc limity pasują; resztę trzeba porównać
   z tym, czego oczekuje wtyczka.
3. `RDNA3Framebuffer` (Etap 3): bez niego display machine nie ma głowic,
   więc `debug.rdna3accelfb=1` nie ma czego zarejestrować i WindowServer nie
   komponuje na tej karcie.
