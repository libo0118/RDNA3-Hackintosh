# Test w maszynie wirtualnej (Linux + jedna karta RX 7800 XT)

Instrukcja dla komputera z Linuksem, w którym **jedyną kartą graficzną jest
RX 7800 XT** (bez iGPU i bez drugiej karty). macOS działa w maszynie
wirtualnej QEMU, a karta zostaje do niej przekazana (passthrough). Wszystko
robi jeden skrypt: `tools/vm/rdna3vm`.

Jak to wygląda w praktyce:

1. Wpisujesz `rdna3vm step 2`. Pulpit Linuksa znika, bo Linux oddaje kartę.
2. Na monitorze pojawia się macOS. Uruchamia się z naszym sterownikiem na
   poziomie z danego kroku.
3. Po około 2 minutach macOS sam zbiera raport, wysyła go do Linuksa i się
   wyłącza. Wraca pulpit Linuksa.
4. Raport leży w `~/rdna3-vm/reports`. Wpisujesz następny krok.

**Czas:** przygotowanie raz, około 1–1,5 godziny (prawie cały ten czas to
instalacja macOS). Potem każdy krok zajmuje około 3 minut.

> Na czas kroku Linux nie ma obrazu, a klawiatura i mysz należą do macOS.
> Zapisz wcześniej otwarte rzeczy. Nie utkniesz bez obrazu, patrz
> [Awaryjne wyjście](#awaryjne-wyjście): **oba Ctrl naraz, potem 3× Esc**.

---

## Krok A. BIOS i Linux (raz, 10 min)

**BIOS płyty głównej:**
- włączone: **SVM** (AMD) albo **VT-x** (Intel);
- włączone: **IOMMU** (AMD-Vi) albo **VT-d** (Intel);
- włączone: **Above 4G Decoding**;
- wyłączony: **CSM**.

Resizable BAR może zostać włączony.

**Parametr jądra** `iommu=pt` (na Intelu dodatkowo `intel_iommu=on`):

| Dystrybucja | Polecenie |
|---|---|
| Ubuntu / Debian / Mint | w `/etc/default/grub` dopisz do `GRUB_CMDLINE_LINUX_DEFAULT` tekst `iommu=pt`, potem `sudo update-grub` |
| Fedora | `sudo grubby --update-kernel=ALL --args="iommu=pt"` |
| Arch (GRUB) | jak Ubuntu, potem `sudo grub-mkconfig -o /boot/grub/grub.cfg` |

Potem zrestartuj komputer.

**Pakiety:**

| Dystrybucja | Polecenie |
|---|---|
| Ubuntu / Debian | `sudo apt install qemu-system-x86 qemu-utils python3 git dmg2img zstd psmisc` |
| Fedora | `sudo dnf install qemu-kvm qemu-img python3 git dmg2img zstd psmisc` |
| Arch | `sudo pacman -S qemu-full python git zstd psmisc`; `dmg2img` jest w AUR |

Na koniec dodaj się do grupy `kvm`: `sudo usermod -aG kvm $USER`. Potem
wyloguj się i zaloguj ponownie.

**Sprawdzenie:**

```sh
git clone -b stage1-kext https://github.com/Voten641/RDNA3-Hackintosh ~/RDNA3-Hackintosh
~/RDNA3-Hackintosh/tools/vm/rdna3vm check
```

Każda linia ma mieć `OK`. Skrypt pokazuje też, co trafi do VM: kartę (z jej
funkcją audio HDMI) oraz klawiaturę i mysz USB.

Dla wygody ustaw alias. Później wystarczy pisać `rdna3vm`:

```sh
echo "alias rdna3vm=~/RDNA3-Hackintosh/tools/vm/rdna3vm" >> ~/.bashrc && . ~/.bashrc
```

## Krok B. Pobranie macOS (raz, 10 min)

Używamy gotowego projektu [OSX-KVM](https://github.com/kholia/OSX-KVM):

```sh
git clone --depth 1 --recursive https://github.com/kholia/OSX-KVM.git ~/OSX-KVM
cd ~/OSX-KVM
./fetch-macOS-v2.py                      # wybierz Sequoia (macOS 15)
dmg2img -i BaseSystem.dmg BaseSystem.img
```

## Krok C. Paczka ze sterownikiem (raz i po każdej nowej wersji)

1. Otwórz [GitHub → Actions](https://github.com/Voten641/RDNA3-Hackintosh/actions).
   Wejdź w najnowszy zielony przebieg **build** na gałęzi `stage1-kext` i
   pobierz artefakt **RDNA3Accel-kext** (plik `.zip`).
2. Uruchom:

```sh
rdna3vm prepare --osx-kvm ~/OSX-KVM --artifact ~/Downloads/RDNA3Accel-kext.zip
```

Skrypt robi kolejno:
- kopię OpenCore z wyłączonym SIP (wymagane dla niepodpisanego kextu);
- paczkę dla macOS: kexty z firmware wziętym z `/lib/firmware/amdgpu` twojego
  Linuksa;
- zrzut VBIOS karty;
- dane referencyjne z amdgpu (`reports/linux-reference`), z którymi
  porównujemy wyniki.

O hasło `sudo` pyta kilka razy.

## Krok D. Instalacja macOS w oknie (raz, ~1 h)

Ten krok działa **bez przekazywania karty**: macOS otwiera się w zwykłym
oknie na pulpicie Linuksa.

```sh
rdna3vm window
```

1. W menu OpenCore wybierz **macOS Base System**.
2. **Narzędzie dyskowe** → wybierz *QEMU HARDDISK* (ok. 128 GB) → **Wymaż**:
   nazwa `macOS`, format **APFS**. Zamknij Narzędzie dyskowe.
3. **Zainstaluj ponownie macOS Sequoia** → wybierz dysk `macOS`. Instalator
   sam kilka razy restartuje VM. Jeśli zobaczysz menu OpenCore, wybierz
   wpis instalatora (*macOS Installer*), a na koniec po prostu `macOS`.
4. Załóż konto. Apple ID, lokalizację i inne opcje możesz pominąć.
5. W macOS otwórz **Terminal** (Cmd+Spacja, wpisz „Terminal”) i wklej:

```sh
curl -s http://10.0.2.2:8000/setup.sh | sudo sh
```

To instaluje sterownik i automatyczne raporty. Gdy skończy, wyłącz VM
poleceniem `sudo shutdown -h now`.

Jeśli macOS pokaże „Rozszerzenie systemowe zablokowane”: wejdź w Ustawienia
systemowe → Prywatność i ochrona → **Zezwól**, uruchom ponownie i wyłącz VM.

## Krok E. Testy na karcie (każdy około 3 min)

Wykonuj kroki **po kolei**. Następny uruchamiasz dopiero, gdy poprzedni
wrócił z raportem:

```sh
rdna3vm step 0
```

Pulpit zgaśnie, na monitorze pojawi się OpenCore, a potem macOS z białym
tekstem na czarnym tle (tryb verbose). Po około 2 minutach od pojawienia się
pulpitu macOS sam się wyłączy i wróci pulpit Linuksa. Potem uruchom kolejne
kroki:

| Krok | Co testuje | Na co patrzysz na ekranie |
|---|---|---|
| `0` | kext się ładuje, nic nie robi | macOS startuje normalnie |
| `1` | odczyt rejestrów, rozmiar VRAM | j.w. |
| `2` | tablica IP discovery, VBIOS, monitor (EDID) | j.w. |
| `3` | rejestry bloku GC, liczba CU | j.w. |
| `4` | PSP: ładowanie firmware | j.w.; pierwszy krok, który zapisuje do karty |
| `5` | SMU: zegary, wentylator, temperatura | wentylator może zmienić obroty |
| `5b` | DMCUB (firmware wyświetlania) | **ekran może mrugnąć albo zgasnąć** |
| `6` | pamięć GPU (GART), przerwania | j.w. |
| `6b` | przerwania MSI | j.w. |
| `7` | SDMA: pierwsze polecenia wykonane przez GPU (+ test DMA) | j.w. |
| `8` | GFX: ring grafiki (+ test GFX) | j.w. |
| `9` | nasz framebuffer zamiast obrazu z UEFI | obraz ma zostać bez zmian; **może zgasnąć** |
| `10` | akcelerator (WindowServer) | ryzykowny; najpierw wyślij raporty z 0–9 |

Jeśli chcesz popatrzeć dłużej, dopisz `--stay`, na przykład `rdna3vm step 9 --stay`.
VM nie wyłączy się wtedy sama. Zamknij macOS normalnie (Apple → Wyłącz)
albo przyciskiem zasilania. Raport wyśle się mimo to.

Wartości, jakich oczekujemy w każdym kroku, opisuje [TESTING-HW](TESTING-HW.md).
Nie musisz ich sprawdzać, wystarczy przysłać raporty.

## Awaryjne wyjście

Każdy z poniższych sposobów zamyka **tylko VM**. Skrypt oddaje wtedy kartę,
klawiaturę i mysz Linuksowi i przywraca pulpit:

1. **Oba Ctrl naraz, potem 3× Esc** (w ciągu 3 sekund). Oba Ctrl zabierają
   klawiaturę macOS i oddają ją Linuksowi, a 3× Esc zamyka VM. Działa także
   przy zawieszonym macOS i przy kernel panic, bo klawiaturę czyta Linux,
   nie macOS.
2. **Krótkie wciśnięcie przycisku zasilania komputera.** Na czas VM Linux
   nie wyłącza się od niego, tylko zamyka VM.
3. **Nic nie robisz:** automatyczny krok sam zamyka VM po 8 minutach
   (zwykle kończy się po około 3). Z `--stay` limit wynosi 30 minut. Oba
   limity zmienisz w `~/rdna3-vm/config` (`AUTO_TIMEOUT_MIN`, `TIMEOUT_MIN`).

Skrypt oddaje kartę Linuksowi także wtedy, gdy QEMU sam się wysypie.
Przywracanie jest podpięte pod zakończenie skryptu, a nie pod macOS.

Ostatnia deska ratunku, gdy po zamknięciu VM pulpit nie wraca: karta nie
dała się zresetować (znany kłopot RDNA3 z passthrough). Wtedy krótko
wciśnij przycisk zasilania. VM już nie działa, więc Linux wyłączy się
normalnie. Po włączeniu komputera wszystko działa. Gdyby nawet to nie
zadziałało, przytrzymaj przycisk 5 sekund. Karcie to nie szkodzi.

## Gdy coś pójdzie nie tak

| Objaw | Co zrobić |
|---|---|
| czarny ekran albo zawieszenie dłużej niż 5 min, kernel panic | [awaryjne wyjście](#awaryjne-wyjście) (oba Ctrl, 3× Esc). Przyślij raporty i **nie idź dalej** |
| po zamknięciu VM pulpit Linuksa nie wraca | krótko przycisk zasilania: Linux wyłączy się normalnie (patrz wyżej) |
| `step` kończy się od razu, pulpit nie znika | przeczytaj `~/rdna3-vm/reports/host-*.log` (najnowszy) i przyślij go |
| macOS nie wysłał raportu | uruchom ten sam krok z `--stay` i w Terminalu macOS wykonaj `sudo sh "/Library/Application Support/RDNA3VM/autoreport.sh"` |
| klawiatura nie działa w macOS | VM uruchomiła się bez przechwytu: przyślij `host-*.log`; wyjście przyciskiem zasilania |

Komputer bez menedżera logowania (start do konsoli) też działa. Skrypt sam
zatrzymuje `display-manager` i potem go wznawia.

## Co mi wysłać

```sh
rdna3vm reports
```

Z każdego kroku przyślij pliki z `~/rdna3-vm/reports`:
- `rdna3-report-*.tar.gz` (raport z macOS);
- `*-tests.txt` (wyniki testów DMA i GFX, od kroku 7);
- `serial-*.log` (log jądra macOS; zostaje także po zawieszeniu i panice);
- `host-*.log` (co robił skrypt na Linuksie).

Przy pierwszej wysyłce dołącz też katalog `linux-reference`.

## Nowa wersja sterownika

1. Pobierz nowy artefakt (krok C, punkt 1).
2. Uruchom `rdna3vm prepare --artifact ~/Downloads/RDNA3Accel-kext.zip`.
3. Uruchom `rdna3vm window` i w Terminalu macOS wklej to samo polecenie
   `curl ... | sudo sh` co w kroku D. Potem `sudo shutdown -h now`.
4. Wróć do kroku E.

## Jak to działa (dla ciekawych)

- `rdna3vm step N` zapisuje boot-args kroku do `config.plist` OpenCore (kopia
  w `~/rdna3-vm/OpenCore-rdna3.img`). Potem jako usługa systemd (`rdna3vm-*`,
  działa także po zgaszeniu pulpitu):
  - zatrzymuje pulpit i odłącza konsolę;
  - przepina kartę z `amdgpu` na `vfio-pci`;
  - uruchamia QEMU z kartą, klawiaturą i myszą USB;
  - po wyłączeniu VM oddaje wszystko Linuksowi.
- Klawiaturę i mysz QEMU czyta z Linuksa (`input-linux`, przełącznik
  „oba Ctrl”) i podaje macOS jako urządzenia USB. Linux sam pilnuje 3× Esc
  i przycisku zasilania (`vmtool.py stopkeys`, `systemd-inhibit`). Po VM
  przycisk znów działa zwyczajnie.
- macOS widzi Linuksa pod adresem `10.0.2.2`. Pod tym adresem
  `tools/vm/vmtool.py serve` udostępnia paczkę i przyjmuje raporty (tylko
  lokalnie, `127.0.0.1:8000`).
- W macOS demon `org.rdna3.vmreport` zbiera raport 90 s po starcie, czyli
  `tools/macos/collect.sh` plus testy `rdna3_drminfo`. Wysyła go i, jeśli host
  o to poprosił (`rdna3vmauto=1`), wyłącza system.
- Log jądra idzie na port szeregowy VM (`serial=3`) do `serial-*.log`, a
  `debug=0x100` zatrzymuje macOS na panice zamiast restartu.
