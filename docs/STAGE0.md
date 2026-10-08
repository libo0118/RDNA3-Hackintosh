# Etap 0: przygotowanie stanowiska

Zanim cokolwiek załadujesz na 7800 XT, zbuduj stanowisko, na którym błąd
sterownika nie zostawia Cię bez ekranu i bez logów.

## 1. Współpraca zamiast pisania wszystkiego od zera

Warstwa Metal-nad-Vulkanem i tłumacz shaderów nie zależą od producenta karty.
Stan na październik 2026 (nie udało mi się zweryfikować nazwy ani repozytorium):

* na r/hackintosh pojawiły się wpisy o hobbystycznym sterowniku NVIDIA
  (RTX 3070 Ti, obraz działa, akceleracja czeka na rozruch GSP) i o tłumaczeniu
  Metal→Vulkan, które według komentujących jest częściowo otwarte, ale we
  wczesnej alfie.
* Gotowe projekty działające w odwrotną stronę (Vulkan nad Metalem: MoltenVK,
  KosmicKrisp) **nie** rozwiązują naszego problemu, ale ich kod pokazuje, jak
  mapują się pojęcia obu API.

Do zrobienia ręcznie (wymaga Twojego konta, nie da się tego zautomatyzować):

1. Znajdź repo i Discorda projektu NVIDIA z wątku na r/hackintosh.
2. Napisz do autora. Szablon:

   > Cześć, piszę otwarty sterownik macOS dla RDNA3 (RX 7800 XT):
   > https://github.com/voten641/rdna3-hackintosh. Mam kext, który rozpoznaje
   > kartę i czyta tablicę IP discovery. Twoja warstwa Metal→Vulkan i tłumacz
   > shaderów są niezależne od producenta, a ja i tak muszę napisać backend
   > winsys RADV pod własny kext. Czy jesteś otwarty na wspólny interfejs
   > kext↔userspace (alokacja BO, submit, fence'y, IOSurface), żeby oba
   > sterowniki mogły używać tej samej warstwy Metal? Chętnie dostosuję się
   > do Twojej licencji i struktury repo.

3. Ustalcie licencję. Ten projekt jest na MIT (jak amdgpu i Mesa). Jeśli ich kod
   jest na GPL, warstwy muszą zostać rozdzielone procesowo lub licencyjnie.

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
