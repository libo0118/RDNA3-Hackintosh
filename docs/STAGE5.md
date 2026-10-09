# Etap 5: Vulkan (RADV na macOS)

RADV z Mesy budujemy na macOS **z niezmienionym winsysem amdgpu**:
`libdrm_amdgpu` (oryginał, `third_party/libdrm`) + nasz rdzeń libdrm
(`userspace/libdrm_xnu`), który zamiast `/dev/dri` rozmawia z kextem.

```sh
userspace/mesa/build_radv.sh build/mesa     # na Macu z Xcode, meson, ninja, glslang, mako
# wynik: build/mesa/install/lib/libvulkan_radeon.dylib + radeon_icd.x86_64.json
```

`build_radv.sh` przypina commit Mesy (`MESA_COMMIT`, lustro GitHub),
nakłada `userspace/mesa/patches/*.patch` i buduje
`-Dvulkan-drivers=amd -Dllvm=disabled` (ACO, bez LLVM). Job CI `radv-macos`
(runner Intel) robi to samo; jego podsumowanie błędów to lista zadań portu.

## Łatki

| Łatka | Po co |
|---|---|
| `0001-radv-open-render-node-through-drm_xnu` | RADV otwiera i zamyka węzeł renderujący przez `drm_xnu` zamiast `open(2)` |
| `0002-addrlib-apple-branches-are-for-the-kernel-driver` | gałęzie `__APPLE__` w addrlib są dla sterownika jądra Apple (nagłówek pod NDA, `__umoddi3`, `IOPanic`); w userspace ich nie chcemy |
| `0003-radv-darwin-weak-entrypoints-via-dynamic-lookup` | słabe, niezdefiniowane symbole `wsi_*` i opcjonalne entrypointy: ld64 na macOS wymaga `-undefined dynamic_lookup` |
| `0004-darwin-use-libdrm_xnu-as-libdrm` | na macOS `dep_libdrm` = nasze libdrm: `HAVE_LIBDRM`, `vk_drm_syncobj.c`, `u_sync_provider.c` i `wsi_common_drm.c` budują się jak na Linuksie (bez tego `vk_drm_syncobj_*` nie istniały i dlopen by się nie udał) |

`libvulkan_radeon.dylib` buduje się i linkuje w CI. Po linkowaniu skrypt
czyta `nm -m`: symbol „dynamically looked up” nie ma właściciela. Słabe
(`weak.txt`) dostają NULL przy ładowaniu (opcjonalne entrypointy); niesłabe
(`missing.txt`) sprawiłyby, że dlopen się nie uda, więc ta lista musi być
pusta. SPIRV-Tools są wyłączone (`-Dspirv-tools=disabled`): tylko dla
zrzutów debug, a ich symbole nie trafiały do linkowania.

## vulkaninfo bez karty

Job `radv-macos` po zbudowaniu RADV podmienia `libdrm.2.dylib` na
`libdrm_loopback.dylib` (`make -C userspace loopback`: ten sam rdzeń libdrm na
`uapi/rdna3_drm.c` w procesie, backend loopback z programowym SDMA) i uruchamia
`vulkaninfo --summary` (loader i narzędzia z brew). To end-to-end test UAPI
bez kextu: `radv_physical_device` pyta o wszystko, czego potrzebuje
(DEV_INFO, HW_IP_INFO, FW_VERSION, MEMORY, SENSOR, GPUVM_FAULT, syncobj).
Log w artefakcie `radv-macos-logs/vulkaninfo.log`.

## Dalej

1. `vulkaninfo` na Macu z kextem na
   `rdna3lvl=2 rdna3enum=1`.
2. WSI: na macOS nie ma KMS; obrazy do wyświetlenia idą przez IOSurface,
   a wtyczka Metal (Etap 6) jest właścicielem prezentacji.
3. Synchronizacja: `vk_drm_syncobj` na naszych syncobj; prawdziwe fence'y
   przyjdą razem z CS (Etap 2 → ringi).
4. Testy compute (CTS `dEQP-VK.compute.*`) po pierwszym działającym CS.
