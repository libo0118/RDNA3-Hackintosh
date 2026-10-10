# Stage 5: Vulkan (RADV on macOS)

We build Mesa's RADV on macOS **with the amdgpu winsys unchanged**:
`libdrm_amdgpu` (the original, `third_party/libdrm`) + our libdrm core
(`userspace/libdrm_xnu`), which talks to the kext instead of `/dev/dri`.

```sh
userspace/mesa/build_radv.sh build/mesa     # on a Mac with Xcode, meson, ninja, glslang, mako
# output: build/mesa/install/lib/libvulkan_radeon.dylib + radeon_icd.x86_64.json
```

`build_radv.sh` pins a Mesa commit (`MESA_COMMIT`, GitHub mirror),
applies `userspace/mesa/patches/*.patch` and builds with
`-Dvulkan-drivers=amd -Dllvm=disabled` (ACO, no LLVM). The CI job `radv-macos`
(Intel runner) does the same; its error summary is the port's task list.

## Patches

| Patch | Purpose |
|---|---|
| `0001-radv-open-render-node-through-drm_xnu` | RADV opens and closes the render node through `drm_xnu` instead of `open(2)` |
| `0002-addrlib-apple-branches-are-for-the-kernel-driver` | the `__APPLE__` branches in addrlib are for Apple's kernel driver (NDA header, `__umoddi3`, `IOPanic`); we do not want them in userspace |
| `0003-radv-darwin-weak-entrypoints-via-dynamic-lookup` | weak, undefined `wsi_*` symbols and optional entrypoints: ld64 on macOS requires `-undefined dynamic_lookup` |
| `0004-darwin-use-libdrm_xnu-as-libdrm` | on macOS `dep_libdrm` = our libdrm: `HAVE_LIBDRM`, `vk_drm_syncobj.c`, `u_sync_provider.c` and `wsi_common_drm.c` build as on Linux (without this, `vk_drm_syncobj_*` did not exist and dlopen would fail) |
| `0005-os_drm-ioctls-through-libdrm-on-drm_xnu` | `util/os_drm.h` (used by `ac_linux_drm.c` for every amdgpu query, CS, BO and VA call) calls `ioctl(2)` on the fd directly, bypassing libdrm; a drm_xnu fd is no kernel file, so RADV failed `ac_drm_query_gpu_info` and found no device (found with `tools/radv_host.sh`). On drm_xnu it goes through `drmIoctl` |

`libvulkan_radeon.dylib` builds and links in CI. After linking, the script
reads `nm -m`: a "dynamically looked up" symbol has no owner. Weak ones
(`weak.txt`) get NULL at load time (optional entrypoints); non-weak ones
(`missing.txt`) would make dlopen fail, so this list must be
empty. SPIRV-Tools are disabled (`-Dspirv-tools=disabled`): they are only for
debug dumps, and their symbols were not making it into the link.

## vulkaninfo without a card

After building RADV, the `radv-macos` job swaps `libdrm.2.dylib` for
`libdrm_loopback.dylib` (`make -C userspace loopback`: the same libdrm core on
`uapi/rdna3_drm.c` in-process, loopback backend with software SDMA) and runs
`vulkaninfo --summary` (loader and tools from brew). This is an end-to-end UAPI test
without the kext: `radv_physical_device` queries everything it needs
(DEV_INFO, HW_IP_INFO, FW_VERSION, MEMORY, SENSOR, GPUVM_FAULT, syncobj).
The log is in the artifact `radv-macos-logs/vulkaninfo.log`.

## RADV on a Linux host (no GPU, no CI)

`tools/radv_host.sh` builds the same Mesa commit with the same patches on
Linux against libdrm_xnu with the loopback backend (`-DRADV_DRM_XNU` takes
the patches' macOS branches: render node through `drm_xnu`, no `/dev/dri`
inode to `stat`). It then runs `userspace/rdna3_vktest --loopback`: instance,
physical device, device creation, and fill + copy on the transfer queue
(the loopback executes SDMA only). Everything RADV asks of the UAPI at
startup is exercised here first.

## rdna3_vktest

`userspace/rdna3_vktest.c` loads the ICD directly (the VM has no Vulkan
loader): device creation, then per queue family a 64 KiB fill + copy
(RADV's compute meta shaders on the general/compute queues: a real
ACO-compiled dispatch) and a 256-byte one (CP DMA), checked on the CPU. The
`radv-macos` CI job builds it next to RADV; `rdna3vm prepare --radv` puts
both in the VM bundle and the report runs it from step 8 on.

## Next

1. `rdna3_vktest` on the card (step 8 with `prepare --radv`).
2. WSI: macOS has no KMS; images for display go through IOSurface,
   and the Metal plugin (Stage 6) owns presentation.
3. Synchronization: `vk_drm_syncobj` on our syncobjs; real fences
   will come together with CS (Stage 2 → rings).
4. Compute tests (CTS `dEQP-VK.compute.*`) after the first working CS.
