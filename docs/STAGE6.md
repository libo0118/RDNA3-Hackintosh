# Etap 6: Metal

Wtyczkę Metal i tłumacza AIR→SPIR-V przenieśliśmy z projektu NullMoth
(`metal/`, pochodzenie i licencje w `metal/PROVENANCE.md`, zgoda autora
na użycie dla AMD w `NOTICE`).

```
Metal app → RDNA3MTLDriver.bundle (metal/plugin) → librdna3mtl_translate.dylib (AIR→SPIR-V)
          → libvulkan_radeon.dylib (RADV, Etap 5) → libdrm_xnu → RDNA3Accel.kext
```

```sh
make -C metal plugin        # RDNA3MTLDriver.bundle (pobiera Vulkan-Headers)
make -C metal translator    # librdna3mtl_translate.dylib (cargo)
```

## Co zmieniliśmy względem NullMoth

* `metal/plugin/rdna3mtl_config.h`: wszystkie ścieżki i nazwy AMD w jednym
  miejscu (`/Library/GPUBundles/rdna3mtl/`: RADV, loader, tłumacz, konfiguracja;
  `rdna3mtl-allow.txt`; sysctl `debug.rdna3accelfb`). Nic nie koliduje z
  instalacją NullMoth.
* Kod wtyczki zmieniony tylko w miejscach ze ścieżkami. Elementy wyłącznie
  NVIDIA (kompilator `NVIDIAShared`, hook SASS w NVK, NVDEC/NVENC) i tak mają
  czyste ścieżki zapasowe, gdy ich pliki nie istnieją.
* Tłumacz bez zmian (LGPL-3.0+), budowany jako osobny dylib pod nową nazwą.

## Co jeszcze potrzebne, żeby Metal zobaczył kartę

1. **Akcelerator (IOAccelerator)**: usługa z `MetalPluginName` /
   `MetalPluginClassName`, przez którą `MTLCopyAllDevices` znajduje wtyczkę,
   a WindowServer kompozytuje. NullMoth ma ją w `kexts/NVRM/accel`
   (IOAcceleratorFamily2 z odtworzonymi nagłówkami). Port dla AMD to osobny
   kext ładowany z AuxKC (Etap 7).
2. RADV z działającym CS (Etap 2 + 5).
3. Allow-lista procesów (`metal/rdna3mtl-allow.txt`) zaczyna się wąsko:
   `mtlprobe`, `nvmtltest`, `vulkaninfo`.

## Uwagi

* Testy jednostkowe tłumacza wymagają `validation/fixtures`, których NullMoth
  nie publikuje. Sama biblioteka buduje się na Linuksie i macOS (CI).
* Nazwy klas Obj-C (`NVMTLDevice` itd.) zostały jak upstream, żeby diff był
  mały. Kolizja możliwa tylko, gdyby oba bundle trafiły do jednego procesu.
