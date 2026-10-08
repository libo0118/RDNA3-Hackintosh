# Etap 0: przygotowanie stanowiska

Zanim cokolwiek załadujesz na 7800 XT, zbuduj stanowisko, na którym błąd
sterownika nie zostawia Cię bez ekranu i bez logów.

## 1. Współpraca zamiast pisania wszystkiego od zera

Projekt NVIDIA to **NullMoth NVIDIA Driver**:
<https://github.com/nullmoth/nvidia-macos-driver> (v1.1.0, Sequoia 15.7/15.8,
RTX 5060). Pełna analiza: [NULLMOTH-ANALYSIS.md](NULLMOTH-ANALYSIS.md). Najważniejsze:

* Ich wtyczka Metal ładuje sterownik Vulkan przez `dlopen()` i korzysta głównie ze
  standardowego Vulkana, więc **nad RADV może działać po wyłączeniu hooków NVK**.
* Tłumacz AIR→SPIR-V jest na LGPL i nadaje się do użycia bez zmian.
* Kernel↔userspace robią przez **emulację linuksowych ioctl na IOKit**. U nas to
  oznacza: kext implementuje UAPI amdgpu, a RADV działa z istniejącym winsysem.
* Wtyczka i kexty są na **PolyForm Noncommercial**, więc nie wolno ich wkleić do
  naszego repo MIT bez zgody autora.

Do zrobienia ręcznie (wymaga Twojego konta): wejdź na Discorda NullMoth (link nie
jest podany w README; jest wspomniany przy zgłaszaniu błędów w aplikacji 1401)
i napisz do autora. Szablon:

> Cześć, piszę otwarty sterownik macOS dla RDNA3 (RX 7800 XT):
> https://github.com/voten641/rdna3-hackintosh. Kext rozpoznaje już kartę i czyta
> tablicę IP discovery. Planuję zrobić jak Wy: kext emulujący linuksowe UAPI
> (u mnie amdgpu) przez IOConnectCallMethod, a nad nim RADV z Mesy. Wasza wtyczka
> Metal ładuje ICD przez dlopen, więc mogłaby działać nad RADV.
> 1) Czy rozważylibyście tryb „generic Vulkan” wtyczki (hooki NVK opcjonalne)?
> 2) Czy możliwa jest licencja pozwalająca łączyć wtyczkę z kodem MIT, albo
>    wspólna, neutralna część wtyczki?
> 3) Jakiego minimalnego zestawu cech Vulkana wymaga wtyczka? Sprawdzę je
>    wcześniej na RADV pod Linuksem.
> Chętnie oddam z powrotem poprawki do wtyczki i tłumacza.

## 2. Sprzęt

| Rola | Karta | Po co |
|---|---|---|
| Wyświetlanie | wspierana natywnie (np. RX 6600 / RX 580) | macOS ma działający obraz niezależnie od naszego kextu |
| Test | RX 7800 XT | żadnych monitorów podpiętych na początku etapów 1–2 |

* W BIOS/UEFI ustaw kartę wyświetlającą jako główną (primary PCIe slot / „Primary
  Display”), włącz *Above 4G decoding*. ReBAR może zostać włączony, ale kext na
  etapie 1 nie korzysta z BAR0.
* **Nie** podszywaj 7800 XT pod inny device-id w `DeviceProperties` OpenCore i nie
  ładuj dla niej WhateverGreen-owych fake-ID, bo wtedy dopasuje się do niej sterownik
  Apple zamiast naszego.
* Najlepiej mieć też dysk z Linuksem na tym samym komputerze: amdgpu na tej samej
  karcie jest referencją, z którą porównujemy każdy odczyt
  (`tools/linux/dump_amdgpu_reference.sh`).

## 3. System i debugowanie

* **macOS Sequoia 15.x** (jak projekt NVIDIA). Zanotuj dokładny build (`sw_vers`),
  bo Kernel Debug Kit musi mu odpowiadać co do joty.
* **SIP wyłączony**: w OpenCore `NVRAM > Add > 7C436110-AB2A-4BBB-A880-FE41995C9F82 >
  csr-active-config = EF0F0000` (0xFEF) i `SecureBootModel = Disabled`.
* **boot-args** na czas developmentu:

  ```
  -v keepsyms=1 debug=0x144 rdna3lvl=0
  ```

  `debug=0x144` = NMI z przycisku zasilania (0x4) + ARP dla debuggera KDP (0x40) +
  log paniki na ekranie (0x100). `keepsyms=1` daje symbole w backtrace'ach.
* **Kernel Debug Kit** dla dokładnie tego builda z developer.apple.com: symbole
  jądra dla LLDB (`lldb /Library/Developer/KDKs/<KDK>/System/Library/Kernels/kernel`).
* **LLDB przez sieć (KDP) i zrzuty paniki (netdump)**:

  ```
  debug=0x144 kdp_match_name=en0                      # LLDB: kdp-remote <IP>
  debug=0x144 kdp_match_name=en0 _panicd_ip=<IP>      # core dump do kdumpd
  ```

  Uwaga: KDP działa tylko z kartami sieciowymi, których sterownik Apple wspiera
  tryb pollingu. Popularne sterowniki hackintoshowe (Realtek, kexty Intel z
  repozytoriów społeczności) zwykle **nie działają** z KDP. Sprawdź to na samym
  początku; alternatywą jest karta USB-Ethernet obsługiwana przez Apple lub
  PCIe Intel I210/I225 ze sterownikiem Apple.
* Logi bez debuggera: `log show --last boot --predicate 'sender == "RDNA3Accel"'`
  albo `sudo dmesg | grep RDNA3`.

## 4. Narzędzia budowania

* Mac z Xcode (pełny Xcode, nie same CLT, żeby mieć `Kernel.framework` w SDK).
* Kext budujemy przez `make kext`; to samo robi CI na `macos-15`, więc artefakt
  z GitHub Actions też nadaje się do testów.
* Linux wystarczy do pracy nad kodem przenośnym (`common/`, testy, narzędzia).

## 5. Lista kontrolna przed pierwszym załadowaniem kextu

- [ ] Druga karta wyświetla obraz, 7800 XT bez monitora
- [ ] `ioreg -l | grep -i 747e` pokazuje urządzenie PCI 7800 XT bez sterownika
- [ ] SIP wyłączony (`csrutil status`)
- [ ] boot-args jak wyżej, działa wejście w picker OpenCore z drugą konfiguracją
      **bez** kextu (ratunek)
- [ ] Zrzut referencyjny z Linuksa zrobiony (`tools/linux/dump_amdgpu_reference.sh`)
