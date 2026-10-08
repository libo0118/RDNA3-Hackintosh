# Firmware AMD dla Navi 32

Etap 2 (bring-up) wymaga mikrokodu z
[linux-firmware](https://gitlab.com/kernel-firmware/linux-firmware), katalog `amdgpu/`.
Lista dla RX 7800 XT (GC 11.0.3), potwierdzona w `WHENCE` linux-firmware i w
`MODULE_FIRMWARE()` sterownika amdgpu:

| Blok (IP) | Pliki |
|---|---|
| PSP (MP0 13.0.7) | `psp_13_0_7_sos.bin`, `psp_13_0_7_ta.bin` |
| SMU (MP1 13.0.7) | `smu_13_0_7.bin` |
| GFX (GC 11.0.3) | `gc_11_0_3_imu.bin`, `gc_11_0_3_pfp.bin`, `gc_11_0_3_me.bin`, `gc_11_0_3_mec.bin`, `gc_11_0_3_rlc.bin`, `gc_11_0_3_mes_2.bin`, `gc_11_0_3_mes1.bin` |
| SDMA 6.0.3 | `sdma_6_0_3.bin` |
| Display (DCN 3.2.1) | `dcn_3_2_1_dmcub.bin` |
| Wideo (VCN 4.0.4, opcjonalnie) | `vcn_4_0_4.bin` |

Przed użyciem potwierdź wersje IP na swojej karcie: `amd_discovery_dump` wypisuje
oczekiwane prefiksy plików na podstawie tablicy discovery.

## Licencja (`LICENSE.amdgpu`)

Najważniejsze warunki (pełny tekst: `LICENSES/LICENSE.amdgpu` w linux-firmware):

* redystrybucja **dozwolona**, bez opłat, **tylko w postaci binarnej**;
* trzeba dołączyć notkę copyright, tekst licencji i zastrzeżenia;
* **zakaz reverse engineeringu, dekompilacji i deasemblacji firmware.**

Konsekwencje dla projektu:

1. Bloby **nie** są commitowane do tego repo (`.gitignore` blokuje `firmware/*.bin`).
   Repo zawiera tylko kod MIT.
2. Wydania binarne mogą zawierać firmware w `RDNA3Accel.kext/Contents/Resources/`
   razem z `LICENSE.amdgpu`. To jest zgodne z licencją, ale wymaga dołączenia jej
   tekstu w paczce.
3. Kext ładuje firmware z własnych zasobów (`OSKextRequestResource`), bo macOS nie
   ma odpowiednika `request_firmware()`.
4. Nie analizujemy zawartości blobów. Interfejs do nich (nagłówki, kolejność
   ładowania, rejestry PSP) bierzemy wyłącznie z otwartego kodu amdgpu.
   Reverse engineering w tym projekcie dotyczy wyłącznie interfejsów macOS
   (Etap 6), nigdy firmware AMD.
