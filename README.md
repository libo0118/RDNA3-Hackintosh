# RDNA3-Hackintosh

Otwartoźródłowy sterownik macOS (x86_64, Hackintosh) dla kart AMD RDNA3,
zaczynając od **Radeon RX 7800 XT (Navi 32, PCI `1002:747E`)**.

> **Status:** kod etapów 1, 2 (PSP/firmware/SMU), 4, 5 i 6 jest napisany i
> buduje się w CI, ale **nic nie było jeszcze uruchomione na prawdziwej karcie**.
> Sterownik niczego nie wyświetla ani nie akceleruje. Stan każdego etapu:
> [docs/ROADMAP.md](docs/ROADMAP.md).

## Co jest w repo

| Ścieżka | Co to jest |
|---|---|
| `kext/` | `RDNA3Accel.kext`: dopasowanie PCI, mapowanie BAR5 (MMIO), bezpieczne odczyty, odczyt i dekodowanie tablicy IP discovery z VRAM |
| `common/amd_discovery.[ch]` | Przenośny parser tablicy *IP discovery* (C99, bez libc, sprawdza wszystkie granice). Ten sam kod działa w kexcie i na hoście |
| `tests/` | Testy jednostkowe parsera i fuzzing mutacyjny pod ASan/UBSan |
| `tools/amd_discovery_dump.c` | Dekoder blobu discovery na hoście: wersje IP, bazy rejestrów, potrzebne pliki firmware |
| `tools/linux/` | Zrzut danych referencyjnych z tej samej karty pod Linuksem/amdgpu |
| `tools/macos/` | Wyciąga blob discovery opublikowany przez kext z `ioreg` |
| `hw/` | Przenośny bring-up (PSP 13.0.x, SMU 13.0.x, rejestry, VRAM), testowany na symulatorze `tests/sim_navi32.c` |
| `uapi/` | Linuksowe UAPI amdgpu niezależne od OS (używane przez kext i testy) |
| `userspace/` | `libdrm_xnu` (rdzeń libdrm nad IOKit) + build libdrm_amdgpu; `mesa/` = build RADV na macOS i łatki |
| `metal/` | Wtyczka Metal i tłumacz AIR→SPIR-V z NullMoth, przystosowane do AMD |
| `third_party/` | Nagłówki AMD/DRM (MIT) i libdrm_amdgpu (MIT) |
| `docs/` | Plan, przygotowanie stanowiska, instrukcje testów, licencje firmware, [analiza sterownika NullMoth (NVIDIA)](docs/NULLMOTH-ANALYSIS.md) |

## Szybki start

Na dowolnym systemie (Linux/macOS):

```sh
make test     # testy parsera + fuzzing (ASan/UBSan)
make tools    # build/host/amd_discovery_dump
```

Na Macu z Xcode (kext budujemy tylko tam):

```sh
make kext     # -> build/RDNA3Accel.kext (x86_64)
```

CI (GitHub Actions) buduje kext na `macos-15` i dołącza go jako artefakt.

Ładowanie kextu na maszynie testowej i poziomy bezpieczeństwa (`rdna3lvl=`)
opisuje [docs/STAGE1.md](docs/STAGE1.md). Najpierw przeczytaj
[docs/STAGE0.md](docs/STAGE0.md): **bez drugiej karty do obrazu nie testuj.**

## Zasady projektu

* **amdgpu (Linux) jest źródłem prawdy** dla sprzętu. Każdy rejestr w kodzie ma
  odnośnik do pliku nagłówkowego amdgpu, z którego pochodzi. amdgpu jest na
  licencji MIT, więc port jest zgodny licencyjnie.
* **Najpierw tylko odczyt.** Każdy etap dodaje zapisy do sprzętu dopiero gdy
  odczyty zgadzają się z referencją z Linuksa (`tools/linux/`).
* **Dane ze sprzętu są niezaufane.** Parsery sprawdzają granice i są fuzzowane;
  zły blob nie może wywalić jądra.
* **Firmware AMD nie trafia do repo.** Patrz [docs/FIRMWARE.md](docs/FIRMWARE.md).

## Licencja

MIT, patrz [LICENSE](LICENSE). Firmware AMD ma własną licencję (`LICENSE.amdgpu`
z linux-firmware) i nie jest tu dystrybuowany.
