# Etap 1: kext rozpoznający kartę

`RDNA3Accel.kext` dopasowuje się do `1002:747E` (Navi 32) i, stopniowo, czyta
coraz więcej ze sprzętu. Każdy poziom włączasz osobno boot-argiem `rdna3lvl=`,
więc jeśli któryś zawiesi system, wiesz dokładnie który.

| `rdna3lvl` | Co robi | Ryzyko |
|---|---|---|
| `0` (domyślnie) | dopasowanie PCI, odczyt przestrzeni konfiguracyjnej (ID, rewizja, link PCIe, stan zasilania). Nie dotyka BAR-ów | brak |
| `1` | + sprawdza czy karta jest w D0, włącza dekodowanie pamięci, mapuje **BAR5 (MMIO)**, czeka (max 2 s) aż firmware PSP zgłosi gotowość (`MP0_SMN_C2PMSG_33` bit 31), czyta rozmiar VRAM (`RCC_CONFIG_MEMSIZE`) | niskie: to dokładnie pierwsze odczyty amdgpu |
| `2` | + czyta 10 KiB tablicy IP discovery z końca VRAM przez okno `MM_INDEX`/`MM_DATA`, weryfikuje sumy kontrolne, dekoduje i publikuje w IORegistry | niskie: jedyne zapisy to `MM_INDEX`/`MM_INDEX_HI` |
| `3` | + czyta `GRBM_STATUS` z bloku GC pod adresem z discovery | niskie, pierwszy rejestr adresowany przez discovery |

`-rdna3off` całkowicie wyłącza kext (probe kończy się porażką).

Kext **nigdy** nie włącza bus masteringu (brak DMA) i nie ładuje firmware.

## Budowanie

```sh
make kext            # na Macu; wynik: build/RDNA3Accel.kext
make -C kext check   # plutil, symbole kmod_info i metaklasy, kextlibs
```

## Ładowanie

Zalecane: wstrzyknięcie przez OpenCore (działa niezależnie od zabezpieczeń kextów
w Sequoi):

1. Skopiuj `RDNA3Accel.kext` do `EFI/OC/Kexts/`.
2. W `config.plist > Kernel > Add` dodaj wpis:
   `BundlePath = RDNA3Accel.kext`, `ExecutablePath = Contents/MacOS/RDNA3Accel`,
   `PlistPath = Contents/Info.plist`, `Enabled = true`, `Arch = x86_64`,
   `MinKernel = 24.0.0` (Sequoia).
3. Zostaw drugi wpis w pickerze OpenCore / drugą kopię EFI z `Enabled = false`
   jako drogę ratunkową.

Alternatywnie, przy wyłączonym SIP: `sudo kmutil load -p build/RDNA3Accel.kext`.

## Co sprawdzić po starcie

```sh
log show --last boot --predicate 'eventMessage CONTAINS "RDNA3:"'
ioreg -l -w0 -r -c RDNA3Accel
tools/macos/ioreg_discovery.sh          # (rdna3lvl>=2) zapis + dekodowanie blobu
```

Oczekiwane na `rdna3lvl=2` dla RX 7800 XT:

* `RDNA3,VRAMSizeMB = 16384`
* `IP discovery ... : ok`
* w tabeli: GC **11.0.3**, wraz z MP0/MP1 (PSP/SMU), SDMA, DMU (DCN), VCN, NBIF.
  Dokładne wersje porównaj z Linuksem; `amd_discovery_dump` podpowiada też, jakie
  pliki firmware będą potrzebne w etapie 2.

### Porównanie z Linuksem (warunek ukończenia etapu)

```sh
# Linux, ta sama karta:
sudo tools/linux/dump_amdgpu_reference.sh ref/
build/host/amd_discovery_dump ref/discovery.bin > linux.txt
# macOS:
tools/macos/ioreg_discovery.sh mac.bin && build/host/amd_discovery_dump mac.bin > mac.txt
diff linux.txt mac.txt     # oczekiwane: brak różnic
```

## Gdy coś pójdzie źle

| Objaw | Prawdopodobna przyczyna |
|---|---|
| kext się nie ładuje, brak logów | inny sterownik wygrał dopasowanie (fake-ID w OpenCore?) albo zły `Arch`/`MinKernel` |
| `device not in D0` | karta uśpiona przez firmware; dodaj ją do `DeviceProperties` z `built-in`, sprawdź ustawienia ASPM/ASPM L1 w BIOS |
| `MMIO reads all-ones` | BAR5 nieprzypisany (włącz Above 4G), karta w D3cold, albo zły slot |
| `IFWI NOT ready` | karta nie została zainicjalizowana przez firmware; sprawdź czy działa pod Linuksem/Windows |
| `RCC_CONFIG_MEMSIZE = 0` | VRAM nie wytrenowany; tablica discovery w pamięci systemowej (nietypowe dla dGPU) |
| `no discovery signature` | inny układ VRAM niż zakładamy; porównaj `DRIVER_SCRATCH_0..2` z Linuksem |
| zawieszenie przy `rdna3lvl=N` | zostań na `N-1`, zgłoś issue z logiem z poprzedniego poziomu |

## Źródła offsetów

Wszystkie stałe w `kext/src/RDNA3Regs.h` i `common/amd_discovery.*` zostały
sprawdzone z Linuksem (gałąź master, październik 2026):
`amdgpu_discovery.c`, `include/discovery.h`, `include/soc15_hw_ip.h`,
`asic_reg/gc/gc_11_0_3_offset.h`, `asic_reg/nbio/nbio_4_3_0_offset.h`,
`amdgpu_device.c` (BAR5 = MMIO), `amdgpu_device_mm_access()` (sekwencja
`MM_INDEX`/`MM_DATA`).
