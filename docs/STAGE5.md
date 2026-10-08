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

`libvulkan_radeon.dylib` buduje się i linkuje w CI. Po linkowaniu skrypt
zapisuje `missing.txt`: symbole, które naprawdę trzeba jeszcze dostarczyć
(bez opcjonalnych entrypointów i eksportów libSystem).

## Dalej

1. `vulkaninfo` na Macu z kextem na
   `rdna3lvl=2 rdna3enum=1`.
2. WSI: na macOS nie ma KMS; obrazy do wyświetlenia idą przez IOSurface,
   a wtyczka Metal (Etap 6) jest właścicielem prezentacji.
3. Synchronizacja: `vk_drm_syncobj` na naszych syncobj; prawdziwe fence'y
   przyjdą razem z CS (Etap 2 → ringi).
4. Testy compute (CTS `dEQP-VK.compute.*`) po pierwszym działającym CS.
