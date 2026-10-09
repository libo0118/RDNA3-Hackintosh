# Etap 3: wyświetlanie (DCN 3.2)

Pełny port Display Core z amdgpu (modesetting, trening łącza, DMCUB) to ponad
100 tys. linii. Pierwszy krok jest dużo mniejszy: **przejmujemy tryb, który
ustawiło VBIOS / UEFI GOP**. Karta już wyświetla obraz z liniowego bufora w VRAM;
sterownik tylko odczytuje, gdzie ten bufor jest i jak wygląda timing, a potem
przełącza (flip) wyświetlanie na inne bufory w VRAM, które przygotował
akcelerator. Łącze, zegary i routing OPP/MPC zostają takie, jak zostawił firmware.

```
GOP / VBIOS ──▶ DCN 3.2: HUBP (adres, pitch, format) ─▶ OTG (timing) ─▶ monitor
                    ▲  odczyt + flip                       ▲ licznik klatek, CRC
          hw/dcn32.c (MIT) ◀── RDNA3Accel.kext: rdna3DisplayInfo/Flip/Status/Crc
                                     ▲  (kext/src/RDNA3Display.cpp)
                         RDNA3Framebuffer.kext (fb/, IOFramebuffer, z NVRMFB)
                                     ▲
                    WindowServer / RDNA3Accelerator (flipy powierzchni)
```

## Części

| Plik | Co robi |
|---|---|
| `hw/dcn32.c` | szuka zapalonych głowic (HUBP bez blank, OTG włączony), czyta format, tiling, DCC, viewport, pitch, adres MC, timing OTG (jak `optc1_get_hw_timing`); flip (`hubp2_program_surface_flip_and_addr`, zmiana pitch pod `SURFACE_UPDATE_LOCK`), flip pending, licznik klatek, linia, vblank, CRC OTG |
| `kext/src/RDNA3Display.cpp` | na `rdna3lvl>=2` czyta głowice (**tylko odczyt**), mierzy odświeżanie z licznika klatek, odpowiada na wywołania framebuffera; alokatory VRAM zaczynają się nad buforem GOP (4K = 32 MiB) |
| `fb/rdna3-fb.cpp` | `RDNA3Framebuffer`: jeden tryb (ten z firmware), apertura BAR0, VBL z licznika klatek OTG, flipy dla akceleratora; bez rdzenia albo bez użytecznej głowicy działa jak zwykły framebuffer na buforze konsoli z bootloadera (`PE_Video`) |

## Bramki

| Boot-arg | Efekt |
|---|---|
| brak `rdna3fb=1` | `RDNA3Framebuffer` się nie uruchamia; ekran obsługuje IONDRVFramebuffer Apple |
| `rdna3fb=1` | nasz framebuffer bierze kategorię `IOFramebuffer` (wynik 30000 > 20000 IONDRV); gdy `start()` się nie uda, IOKit wraca do IONDRV |
| `rdna3lvl=2` (albo więcej) | rdzeń zna głowice: tryb DCN (flipy, prawdziwy VBL) zamiast trybu konsoli |
| `-rdna3off` | wyłącza wszystko |

Zapisy do rejestrów DCN zdarzają się tylko przy flipie albo przy włączeniu CRC,
czyli dopiero gdy akcelerator zacznie wyświetlać swoje powierzchnie.

## Testy z jedną kartą (bez iGPU)

macOS bez sterownika dla 7800 XT startuje na buforze GOP (IONDRVFramebuffer):
bez akceleracji, jedna rozdzielczość, jeden monitor. To wystarczy.

1. BIOS: CSM wyłączony (czysty UEFI GOP). Jeśli są problemy z Resizable BAR:
   wyłączyć go albo `ResizeAppleGpuBars = -1` w OpenCore. **Nie** blokować
   `IONDRVSupport` w tym etapie.
2. W macOS włączyć Logowanie zdalne (SSH) i łączyć się z innego urządzenia.
3. Kexty instalować (`package/install.sh`), ale **pierwsze testy robić przez
   ręczne ładowanie**, nie przy starcie: zawieszenie naprawia twardy reset.
4. Kolejność:
   1. `rdna3lvl=2`: tylko odczyty. W `ioreg -l -w0 | grep RDNA3` powinny być
      `RDNA3,DisplayHeads`, `RDNA3,DisplayReserveEnd`; w logu
      `display: head 0 …x… pitch … usable`.
   2. `rdna3lvl=2 rdna3fb=1`: nasz framebuffer zamiast IONDRV. Oczekiwane:
      pulpit wygląda tak samo, w logu `RDNA3-fb: DCN mode: …`. Jeśli ekran
      zgaśnie: SSH i logi, potem restart bez `rdna3fb=1`.
   3. Dopiero potem akcelerator (`rdna3accel=1`, Etap 7) i flipy.
5. Logi: `log show --last 10m --predicate 'eventMessage CONTAINS "RDNA3"'`.

## Ograniczenia (do zrobienia)

* jedna głowica (head 0), jeden tryb; brak EDID (AUX/I2C DCN), brak zmiany
  rozdzielczości i odświeżania, brak sprzętowego kursora (macOS rysuje
  kursor programowo), brak gammy;
* przerwania VBLANK z IH (teraz odpytywanie licznika klatek);
* pełny DC (modesetting) później, osobny duży etap.
