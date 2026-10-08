# RDNA3-Hackintosh

Otwartoźródłowy sterownik macOS (x86_64, Hackintosh) dla kart AMD RDNA3,
zaczynając od **Radeon RX 7800 XT (Navi 32, PCI `1002:747E`)**.

> **Status: Etap 1 (rozpoznanie karty).** Sterownik jeszcze niczego nie
> wyświetla ani nie akceleruje. Pełna ścieżka do Metal to projekt na
> miesiące–lata, patrz [docs/ROADMAP.md](docs/ROADMAP.md).

## Co jest w repo

| Ścieżka | Co to jest |
|---|---|
| `kext/` | `RDNA3Accel.kext`: dopasowanie PCI, mapowanie BAR5 (MMIO), bezpieczne odczyty, odczyt i dekodowanie tablicy IP discovery z VRAM |
| `common/amd_discovery.[ch]` | Przenośny parser tablicy *IP discovery* (C99, bez libc, sprawdza wszystkie granice). Ten sam kod działa w kexcie i na hoście |
| `tests/` | Testy jednostkowe parsera i fuzzing mutacyjny pod ASan/UBSan |
| `tools/amd_discovery_dump.c` | Dekoder blobu discovery na hoście: wersje IP, bazy rejestrów, potrzebne pliki firmware |
| `tools/linux/` | Zrzut danych referencyjnych z tej samej karty pod Linuksem/amdgpu |
| `tools/macos/` | Wyciąga blob discovery opublikowany przez kext z `ioreg` |
| `docs/` | Plan, przygotowanie stanowiska, instrukcje testów, licencje firmware |

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
