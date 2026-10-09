# Test na sprzęcie: drabinka krok po kroku

Jedna procedura dla hackintosha z RX 7800 XT. Każdy krok dokłada jedną
rzecz, a po każdym zbierasz raport jednym skryptem. Jeśli krok zawiesi
system albo zgasi ekran, wracasz do poprzedniego i przysyłasz raport z
niego oraz opis objawu. Szczegóły modułów są w `STAGE1`–`STAGE7`.

## Przed pierwszym krokiem

1. **Droga powrotu.** W OpenCore zostaw drugi wpis albo drugą kopię EFI, w
   której kexty RDNA3 są wyłączone. Boot-arg `-rdna3off` też wyłącza
   wszystko. Sprawdź raz, że umiesz edytować boot-args w pickerze (spacja,
   potem wpis z boot-args albo `Cmd+V` dla trybu verbose).
2. **Zdalny dostęp.** Ustawienia → Ogólne → Udostępnianie → Logowanie
   zdalne (SSH). Gdy ekran zgaśnie, raport zbierzesz przez SSH z innego
   komputera.
3. **BIOS.** CSM wyłączony, Above 4G Decoding włączony. Przy kłopotach z
   Resizable BAR: wyłącz go albo `ResizeAppleGpuBars = -1` w OpenCore.
4. **Budowanie i instalacja.** Na Macu w katalogu repozytorium:

   ```sh
   make kext && make -C userspace && make tools
   make -C accel && make -C fb          # akcelerator i framebuffer (od kroku 9)
   sudo package/install.sh --fw /ścieżka/do/linux-firmware/amdgpu
   ```

   Firmware nie jest w repozytorium (`docs/FIRMWARE.md`). Instalator
   przebudowuje Auxiliary Kernel Collection; potrzebny jest restart.
5. **Boot-args ustawiasz raz na krok**, na przykład:

   ```sh
   sudo nvram boot-args="-v keepsyms=1 rdna3lvl=2"
   ```

   Zachowaj swoje dotychczasowe boot-args i dopisuj do nich te z tabeli.
6. **Raport po każdym kroku** (po restarcie, ze zwykłego terminala lub SSH):

   ```sh
   tools/macos/collect.sh krok3
   ```

   Skrypt niczego nie zmienia. Tworzy `~/rdna3-report-krok3-<data>.tar.gz`
   z logami, IORegistry, boot-args i ewentualnym raportem paniki.

## Drabinka

| Krok | Boot-args (dopisz) | Co robi | Sprawdź (`11-ioreg-props.txt`, `20-log-rdna3.txt`) | Ryzyko |
|---|---|---|---|---|
| 1 | `rdna3lvl=1` | mapuje BAR5, czeka na PSP, czyta rozmiar VRAM | `RDNA3,VRAMSizeMB = 16384`, brak `MMIO reads all-ones` | niskie |
| 2 | `rdna3lvl=2` | tablica IP discovery, odczyt głowic DCN, VBIOS, ścieżki wyświetlania, DP AUX/EDID | `IP discovery ... ok`, GC 11.0.3; `RDNA3,DisplayHeads = 1`; `RDNA3,DisplayPath0` (np. `DP-1 UNIPHYA DDC1 HPD1`); `RDNA3,Connector0,Monitor` = nazwa monitora; `RDNA3,Head0EdidMode`; `RDNA3,AtomCommandTables` | niskie: zapisy tylko do okien odczytu i rejestrów AUX |
| 3 | `rdna3lvl=3` | pierwszy rejestr GC z discovery | `GRBM_STATUS`, `RDNA3,GBAddrConfig`, `RDNA3,ActiveCUs = 60` | niskie |
| 4 | `rdna3lvl=4` | PSP: bootloader, sOS, TMR, ładowanie całego firmware | `RDNA3,Stage2 = PSP up, firmware loaded`, `sOS running`, 29× `LOAD_IP_FW ... ok`, `RLC autoload started` | średnie: pierwsze zapisy do PSP |
| 5 | `rdna3lvl=5` | SMU: PMFW, VBIOS, PPTable, cechy bezpieczne, metryki | `RDNA3,SMUVersion`, `RDNA3,PM`, `RDNA3,PMFeaturesRunning`, `RDNA3,PMTempHotspotC`, `RDNA3,PMFanRPM` | średnie: zegary i wentylator |
| 5b | `rdna3lvl=5 rdna3dmub=1` | DMCUB: zatrzymanie firmware z VBIOS, okna, ponowny start, skrzynka komend | `RDNA3,DMUB = running, mailbox up`, `RDNA3,DMUBVersion = 0x07003300` (lub zbliżona), `RDNA3,DMUBBootStatus` z bitami 0 i 1 | **ekran może mrugnąć lub zgasnąć**; wtedy SSH, raport, krok bez `rdna3dmub` |
| 6 | `rdna3lvl=6` | GMC (GART, MMHUB), ring IH, doorbelle | `RDNA3,GMCResult = 0`, `RDNA3,IHResult = 0`, `RDNA3,Doorbells` | średnie |
| 6b | `rdna3lvl=6 rdna3msi=1` | przerwania MSI zamiast odpytywania | `RDNA3,IRQs` rośnie | niskie |
| 7 | `rdna3lvl=7 rdna3msi=1` | SDMA0: test ringu, kopia 64 KiB VRAM→VRAM | `RDNA3,SDMAResult = 0`, `RDNA3,SDMACopyMismatches = 0`, `RDNA3,MSIVerified = 1` | średnie: **pierwsze polecenia wykonywane przez GPU** |
| 7b | jak 7 | z terminala: `build/userspace/rdna3_drminfo --dma-test` | `DMA job results verified (WRITE_LINEAR + CONST_FILL)` | średnie |
| 8 | `rdna3lvl=8 rdna3msi=1` | GFX 11: RLC, RS64, ring GFX, KIQ, KCQ | `RDNA3,GFXResult = 0`, `RDNA3,KIQResult = 0`, `RDNA3,KCQResult = 0` | średnie |
| 8b | jak 8 | `build/userspace/rdna3_drminfo --gfx-test` | `GFX job results verified (PM4 WRITE_DATA)` | średnie |
| 9 | jak 8 + `rdna3fb=1` | nasz framebuffer zamiast IONDRV na trybie z GOP | pulpit bez zmian, w logu `RDNA3FB: ... EDID ... from AUX` i nazwa monitora w Ustawieniach → Monitory | **ekran może zgasnąć** |
| 10 | jak 9 + `rdna3accel=1` | akcelerator IOAcceleratorFamily2 | `ioreg -c RDNA3Accelerator`, w logu `populateAccelConfig ... what the FAMILY pre-filled` | wysokie: WindowServer |
| 11 | jak 10 | `vulkaninfo --summary` z RADV z `/Library/GPUBundles/rdna3mtl` | urządzenie `AMD Radeon RX 7800 XT (RADV NAVI32)` | średnie |
| 12 | jak 10, potem `sudo sysctl -w debug.rdna3accelfb=1` | wtyczka Metal dla procesów z listy `rdna3mtl-allow.txt` | `mtlprobe` widzi urządzenie AMD | wysokie |

Zasada: **nie przechodź dalej, dopóki krok nie daje oczekiwanych wartości.**
Gdy wynik jest inny niż w tabeli, przyślij raport. To jest dokładnie ta
informacja, której nie da się uzyskać w symulatorze.

## Gdy coś pójdzie źle

| Objaw | Co zrobić |
|---|---|
| zawieszenie przy starcie na kroku N | restart z boot-args kroku N−1 (picker OpenCore) i raport z tego startu; opisz, na którym etapie logu verbose stanął |
| kernel panic | po restarcie `collect.sh` sam dołącza dwa ostatnie raporty paniki |
| czarny ekran, system żyje | SSH, `tools/macos/collect.sh ekran`, potem `sudo nvram boot-args="..."` bez ostatniej flagi i `sudo reboot` |
| brak jakichkolwiek wpisów `RDNA3` w logu | kext się nie załadował: `03-kexts.txt` i `kmutil showloaded`; sprawdź, czy instalator przebudował kolekcję i czy SIP pozwala na kexty z `/Library/Extensions` |
| `RDNA3,PSPResult` różny od 0 | brakuje firmware w `RDNA3Accel.kext/Contents/Resources` albo to inna wersja niż oczekiwana; przyślij raport |

## Czego nie da się sprawdzić bez karty

Symulator modeluje zachowanie z kodu Linuksa, ale nie wie, jak naprawdę
zachowa się karta. Pierwszy test powie przede wszystkim:

* czy PSP przyjmuje nasz porządek ładowania firmware (krok 4);
* czy PMFW akceptuje BoardTable z VBIOS i bezpieczny zestaw cech (krok 5);
* czy reset DMCUB pod działającym obrazem z GOP jest bezpieczny (krok 5b);
* czy GPU wykonuje nasze pakiety SDMA i PM4 (kroki 7 i 8);
* jak IOAcceleratorFamily2 wypełnia `IOAccelConfig` przed nami (krok 10).

Dalsza praca nad modesetem (DML, zegar piksela, trening DP) zaczyna się po
krokach 2, 5b i 9, bo potrzebuje prawdziwych wartości rejestrów DCN.
