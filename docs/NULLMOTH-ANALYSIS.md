# Analiza: NullMoth NVIDIA driver i co z niego wynika dla RDNA3

Źródło: <https://github.com/nullmoth/nvidia-macos-driver> (commit `b9a5a2b`,
8.10.2026, wersja 1.1.0). Ten dokument opisuje architekturę własnymi słowami.
**Nie kopiujemy stamtąd kodu**: patrz „Licencje” niżej.

## Architektura NullMoth

```
Metal app ─► NVMTLDriver.bundle ─► translator (AIR → SPIR-V) ─► NVK (Mesa) ─► kexty ─► GPU
```

| Warstwa | Ich implementacja | Licencja | Odpowiednik u nas |
|---|---|---|---|
| Wtyczka Metal | `plugin/` (Obj-C, ~7 tys. linii samego `nvmtl_vk.c`) implementuje `MTLDevice` itd. nad Vulkanem | PolyForm Noncommercial 1.0.0 | do uzgodnienia z autorem (patrz niżej) |
| Tłumacz shaderów | `translator/` (Rust, oparty na metal2vulkan): Apple AIR → SPIR-V | LGPL-3.0-or-later | ten sam, bez zmian: SPIR-V jest niezależny od producenta |
| Vulkan | NVK z Mesa + łatka `nvk/nvk-macos.patch` (~12 tys. linii) | MIT (Mesa) | **RADV** z Mesa + nasza łatka |
| Kernel | port otwartych modułów NVIDIA r610 z warstwą OS `os-xnu*.cpp` | PolyForm NC / MIT+GPL (NVIDIA) | port amdgpu + `linux_compat/` (Etap 2) |
| Framebuffer | `NVRMFB.kext` (`IOFramebuffer` nad NVKMS) | PolyForm NC | DCN 3.2.1 + `IOFramebuffer` (Etap 3) |
| Akcelerator | `NVAccel` (`IOAccelerator`, IOAcceleratorFamily2) | PolyForm NC | Etap 4/7 |
| Polityka wyświetlania | `NVRMAGDC.kext` (AppleGraphicsDeviceControl) | PolyForm NC | Etap 7 |

## Najważniejsze ustalenia techniczne

### 1. Wtyczka Metal ładuje dowolny sterownik Vulkan

`nvmtl_vk.c` otwiera ICD przez `dlopen()` (ścieżka z `NVMTL_ICD_LIB` lub
`nvk_icd.json`) i woła `vk_icdGetInstanceProcAddr`. Rdzeń korzysta ze
standardowego Vulkana: `VK_KHR_external_memory_fd`, `VK_EXT_image_drm_format_modifier`,
`VK_EXT_mutable_descriptor_type`, `VK_EXT_memory_budget`, ray query, mesh shadery.
RADV wspiera to wszystko na RDNA3.

Są jednak fragmenty przywiązane do NVK: hooki do NVK (szybka ścieżka „SASS
dispatch”, `nvk_blend_state`), obejścia ograniczeń NVK oraz `NVIDIAShared.bundle`
(NVVM). Trzeba je wyłączyć lub zastąpić dla RADV.

### 2. Kernel↔userspace: emulacja linuksowych ioctl przez IOKit

To najważniejszy pomysł do przejęcia. NVK nie dostał nowego backendu: istniejący
backend `nvkmd/nvrm` (z portu na Haiku) woła `ioctl()` dokładnie jak na Linuksie,
a cienka warstwa `nvrm_xnu.c` zamienia to na IOKit:

| Linux | macOS (NullMoth) |
|---|---|
| `open("/dev/nvidia0")` | `IOServiceOpen()` na usłudze kextu; jako „fd” zwracany jest deskryptor `/dev/null`, a fd↔`io_connect_t` trzymane w tabeli |
| `ioctl(fd, cmd, arg)` | `IOConnectCallMethod(conn, SEL_ESC, {cmd}, arg, size → arg)` |
| `mmap(fd, …)` | `IOConnectMapMemory64()` |
| czekanie na zdarzenia | osobny selektor z timeoutem |

**Konsekwencja dla nas:** Etap 4 nie powinien wymyślać własnego ABI. Kext implementuje
**linuksowe UAPI amdgpu** (`include/uapi/drm/amdgpu_drm.h`, MIT):
`DRM_IOCTL_AMDGPU_INFO`, `GEM_CREATE`, `GEM_MMAP`, `GEM_VA`, `CTX`, `CS`,
`WAIT_CS`, `BO_LIST`, `SYNCOBJ_*`. Wtedy RADV z istniejącym winsysem amdgpu
działa prawie bez zmian; podmieniamy tylko warstwę wywołań systemowych.

W Mesie wszystkie ioctl RADV przechodzą przez jedną warstwę (`src/amd/common/ac_linux_drm.c`,
`ac_drm_*`, dodana na potrzeby virtio native context). *Nie mogłem tego potwierdzić z tego
środowiska (proxy blokuje gitlab.freedesktop.org); sprawdź w aktualnej Mesie.* Jeśli tak jest,
łatka RADV ogranicza się do tego pliku + WSI/IOSurface.

### 3. Jak Metal znajduje wtyczkę

Usługa `IOAccelerator` publikuje w IORegistry `MetalPluginName`
(ścieżka do bundla w `/Library/GPUBundles/`), `MetalPluginClassName`
(klasa `MTLDevice`) i `IOGLBundleName = AppleMetalOpenGLRenderer` (OpenGL nad
Metalem). U nas analogicznie: `RDNA3MTLDriver.bundle` + własne `IOAccelerator`.

### 4. Lekcje z OpenCore i ładowania kextów (Sequoia)

* Kexty wyświetlania linkują z IOGraphicsFamily / IOAcceleratorFamily2, które istnieją
  tylko w kolekcjach jądra Apple, więc **od Etapu 3 kexty muszą się ładować z Auxiliary
  Kernel Collection** (`/Library/Extensions` + `kmutil`), a nie przez wstrzyknięcie
  OpenCore. Nasz kext z Etapu 1 (tylko IOPCIFamily) może jeszcze być wstrzykiwany.
* `csr-active-config = 430A0000` (0x0A43) przetestowane z niepodpisanymi kextami.
* `Kernel > Block: com.apple.iokit.IONDRVSupport (Exclude)`, inaczej framebuffer
  firmware zajmuje indeks wyświetlacza 0.
* `amfi_get_out_of_my_way=0x1 amfi=0x80`, żeby WindowServer załadował niepodpisany bundle.
* Instalator macOS potrzebuje odwrotnych ustawień BAR (mały BAR, bez blokady IONDRV).
* Mają awaryjny boot-arg `-nvoff` i narzędzie EFI, które go ustawia. U nas jest `-rdna3off`.

### 5. Gdzie AMD jest trudniejsze niż NVIDIA

NVIDIA (Turing+) ma **GSP**: firmware na karcie robi większość inicjalizacji, a
otwarte moduły r610 są projektowane z myślą o przenośności między systemami
(`os-interface`). amdgpu nie ma takiej warstwy: inicjalizacja (PSP, SMU, GMC, IH,
GFX/MES) dzieje się w sterowniku hosta i jest pisana pod API Linuksa (DRM, TTM,
`dma_fence`). Nasz Etap 2 jest więc większy niż ich odpowiednik.

## Licencje: co wolno

| Komponent NullMoth | Licencja | Czy możemy użyć w repo MIT? |
|---|---|---|
| `translator/` | LGPL-3.0+ | **tak**, jako osobna biblioteka dynamiczna (wymogi LGPL: źródła + możliwość podmiany) |
| `nvk/` (łatka na Mesę) | MIT | tak, jako wzór dla łatki RADV |
| `plugin/`, `kexts/`, `build/`, `package/` | PolyForm Noncommercial 1.0.0 | **nie** w repo MIT. Możliwe: (a) zgoda autora na inną licencję dla wspólnej części, (b) osobny moduł na PolyForm NC, co uczyni całość niekomercyjną, (c) wspólny, neutralny projekt wtyczki prowadzony razem |

Kod amdgpu (MIT) i nasz kext pozostają na MIT niezależnie od wyboru.

## Proponowane pytania do autora (Discord NullMoth)

1. Czy wtyczka (`plugin/`) mogłaby mieć tryb „generic Vulkan”, w którym hooki NVK są
   opcjonalne, żeby dało się ją uruchomić nad RADV?
2. Czy zgodzisz się na licencję pozwalającą łączyć wtyczkę z kodem MIT, albo na
   wydzielenie wspólnej części vendor-neutral?
3. Jaki jest minimalny zestaw rozszerzeń / cech Vulkana, którego wtyczka wymaga
   (żeby sprawdzić to wcześnie na RADV pod Linuksem)?
4. Jakie zachowania IOAccelerator/IOSurface, których WindowServer oczekuje, były
   najtrudniejsze do odtworzenia?
