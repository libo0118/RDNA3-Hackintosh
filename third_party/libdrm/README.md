# libdrm subset (MIT)

From libdrm 2.4.134 (`libdrm_2.4.134.orig.tar.xz`, Ubuntu archive mirror of
<https://gitlab.freedesktop.org/mesa/drm>). Kept unmodified:

* `amdgpu/` - libdrm_amdgpu, compiled as-is for macOS by `userspace/Makefile`
* `xf86drm.h`, `include/drm/*` - the public libdrm API/UAPI headers Mesa builds against
* `data/amdgpu.ids` - marketing names

The core of libdrm (`xf86drm.c`) is **not** used: it is Linux/BSD specific
(sysfs, device nodes). `userspace/libdrm_xnu/xf86drm_xnu.c` implements the
subset of that API that libdrm_amdgpu and Mesa RADV need, on top of the
RDNA3Accel IOUserClient.
