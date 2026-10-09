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
| `hw/dcn32.c` | sprzętowy kursor 256×256 ARGB (HUBP + DPP, jak `hubp2_cursor_set_*` / `dpp2_set_cursor_attributes`); szuka zapalonych głowic (HUBP bez blank, OTG włączony), czyta format, tiling, DCC, viewport, pitch, adres MC, timing OTG (jak `optc1_get_hw_timing`); flip (`hubp2_program_surface_flip_and_addr`, zmiana pitch pod `SURFACE_UPDATE_LOCK`), flip pending, licznik klatek, linia, vblank, CRC OTG |
| `kext/src/RDNA3Display.cpp` | na `rdna3lvl>=2` czyta głowice (**tylko odczyt**), mierzy odświeżanie z licznika klatek, odpowiada na wywołania framebuffera; alokatory VRAM zaczynają się nad buforem GOP (4K = 32 MiB) |
| `fb/rdna3-fb.cpp` | `RDNA3Framebuffer`: jeden tryb (ten z firmware), apertura BAR0, VBL z licznika klatek OTG, flipy dla akceleratora; bez rdzenia albo bez użytecznej głowicy działa jak zwykły framebuffer na buforze konsoli z bootloadera (`PE_Video`) |

## Bramki

| Boot-arg | Efekt |
|---|---|
| brak `rdna3fb=1` | `RDNA3Framebuffer` się nie uruchamia; ekran obsługuje IONDRVFramebuffer Apple |
| `rdna3fb=1` | nasz framebuffer bierze kategorię `IOFramebuffer` (wynik 30000 > 20000 IONDRV); gdy `start()` się nie uda, IOKit wraca do IONDRV |
| `rdna3lvl=2` (albo więcej) | rdzeń zna głowice: tryb DCN (flipy, prawdziwy VBL) zamiast trybu konsoli |
| `rdna3cursor=0` | kursor programowy zamiast sprzętowego kursora DCN |
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

## DP AUX, HPD, EDID (pierwszy krok Display Core)

`hw/dcaux.c` (port `dce_aux.c`: arbiter, `AUX_SW_DATA` FIFO, `AUX_SW_GO`,
status, odpowiedź) daje natywny odczyt/zapis DPCD, I2C-over-AUX i EDID
(128 B + rozszerzenia, segment E-DDC), `dcaux_hpd_sense` czyta
`DC_HPD_INT_STATUS.DC_HPD_SENSE`. Przypisanie silnik AUX ↔ złącze ↔ OTG
jest w tablicy obiektów VBIOS (jeszcze nie parsowanej), więc kext sonduje
wszystkie 5 silników przy starcie (`RDNA3,Connectors`,
`RDNA3,Connector<n>,{AUX,HPD,DPCD,EDID,Monitor,Modes}`), a `hw/edid.c`
wyciąga z EDID nazwę, tryby szczegółowe (DTD, w tym CTA-861), standardowe
i ustalone. `RDNA3Framebuffer` odpowiada `hasDDCConnect`/`getDDCBlock`
(`rdna3DisplayEdid`: głowica 0 = pierwszy podłączony sink z EDID), więc
macOS buduje `IODisplayEDID` i pokazuje nazwę monitora. Modesetting z tych
trybów wymaga jeszcze: interpretera ATOM (tablice `TransmitterControl`,
`SetPixelClock`), treningu łącza DP i programowania OTG/HUBP pod nowy tryb.

## Tablica obiektów VBIOS i interpreter AtomBIOS (krok 2)

`vbios_display_paths` (port `bios_parser2`: `get_bios_object`,
`get_hpd_record`, `get_gpio_i2c_info`) czyta `displayobjectinfo` v1.4/v1.5
i `gpio_pin_lut`: złącze (DP/HDMI/USB-C), enkoder (UNIPHY A..G), linia DDC
(= silnik AUX), pin HPD, możliwości enkodera (HBR3, UHBR, MST) i limit
łącza. Kext publikuje `RDNA3,DisplayPath<n>` i opisuje złącza AUX ich typem.
Który OTG prowadzi ścieżkę, nie ma w VBIOS: Display Core przydziela potoki
przy modesecie.

`hw/atom.c` to port `atom.c` z amdgpu: wszystkie 126 opkodów, metody IIO,
okno FB (scratch), zagnieżdżone `CALLTABLE` (limit 32), wykrywanie pętli
(20 s zleconych opóźnień) plus budżet opkodów. W odróżnieniu od Linuksa nie
alokuje pamięci (workspace z puli w kontekście) i sprawdza granice obrazu
przy każdym odczycie bajtkodu. Kext tworzy kontekst i publikuje wersje
tablic `DIGxEncoderControl`, `DIG1TransmitterControl`, `SetPixelClock`,
`EnableDispPowerGating`, `SetDCEClock` (`RDNA3,AtomCommandTables`), ale
niczego jeszcze nie wykonuje. Symulator ma mini-asembler bajtkodu i testuje
każdą grupę opkodów, IIO, FB, przerwanie pętli, dostęp poza obraz i
rekurencję.

Uwaga: na DCN 3.2 Linux nie wykonuje tych tablic w sterowniku
(`dcn32 debug.dmub_command_table = true`). `command_table2` wysyła je do
DMCUB jako komendy `DMUB_CMD__VBIOS`, a firmware DMCUB wykonuje je z kopii
VBIOS w oknie CW3. Interpreter jest więc ścieżką zapasową i narzędziem
diagnostycznym, a główną drogą jest DMCUB (niżej).

## DMCUB (krok 3)

`hw/dmub32.c` to port `dmub_srv.c` + `dmub_dcn32.c` oraz
`dm_dmub_sw_init`/`dm_dmub_hw_init`:

* odczyt nagłówka `dcmub_firmware_header_v1_0` i bloku metadanych
  `dmub_fw_meta` (za 256- lub 512-bajtową stopką PSP), rozkład 12 okien
  (inst const, stos, BSS, VBIOS, mailbox, trace, stan FW, scratch, IB,
  shared state, LSDMA, cursor offload) w VRAM, wyrównanie jak w Linuksie;
* reset: `GPINT STOP_FW` do działającego firmware (zostawionego przez
  VBIOS), czekanie na `0xDEADDEAD` w SCRATCH7 i `PWAIT_MODE`, soft reset,
  wyłączenie okien CW2-CW7, wyzerowanie skrzynek;
* obraz instrukcji ładuje PSP (już w planie `LOAD_IP_FW`), więc CW0/CW1
  zostają jego; ścieżka backdoor (kopiowanie obrazu do CW0 z adresem
  fizycznym FB) też jest;
* okna CW3 (VBIOS), CW4 (inbox1 + outbox1), CW5 + region 5 (trace /
  outbox0), CW6 (stan FW), opcje startu, `reset_release`, czekanie na
  `DAL_FW | MAILBOX_RDY`;
* ring inbox1 (64-bajtowe komendy, wrap, pełny ring, odczyt odpowiedzi ze
  slotu), `QUERY_FEATURE_CAPS` jako pierwsza wymiana, `DMUB_CMD__VBIOS`
  z `DIG1TransmitterControl` v1.7.

W kexcie to poziom 5 za flagą `rdna3dmub=1` (domyślnie wyłączone, bo
DMCUB obsługuje wyświetlacz uruchomiony przez GOP i reset trzeba najpierw
sprawdzić na karcie). Wynik: `RDNA3,DMUB`, `RDNA3,DMUBResult`,
`RDNA3,DMUBVersion`, `RDNA3,DMUBBootStatus`, `RDNA3,DMUBCaps`.
Symulator modeluje firmware: sprawdza okna, kopię VBIOS i skrzynkę przed
zgłoszeniem gotowości, odpowiada na STOP_FW, wykonuje komendy z ringu;
testy obejmują prawdziwy `dcn_3_2_1_dmcub.bin` (wersja 0x07003300).

## Ograniczenia (do zrobienia)

* jedna głowica (head 0), jeden tryb; brak EDID (AUX/I2C DCN), brak zmiany
  rozdzielczości i odświeżania, brak gammy;
* przerwania VBLANK z IH (teraz odpytywanie licznika klatek);
* pełny DC (modesetting) później, osobny duży etap.
