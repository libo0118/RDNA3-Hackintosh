/*
 * rdna3mtl_config.h - AMD (RDNA3-Hackintosh) configuration of the Metal
 * plugin imported from NullMoth (see ../PROVENANCE.md).
 *
 * Everything that differs between the NVIDIA and the AMD build is a macro
 * here, so the rest of the plugin stays as close to upstream as possible.
 * Install layout (package/install.sh):
 *   /Library/GPUBundles/RDNA3MTLDriver.bundle      this plugin
 *   /Library/GPUBundles/rdna3mtl/                  RADV, loader, translator
 *   /Library/GPUBundles/rdna3mtl-allow.txt         processes allowed to use it
 * Names never overlap with a NullMoth install.
 */
#ifndef RDNA3MTL_CONFIG_H
#define RDNA3MTL_CONFIG_H

#define RDNA3MTL_DIR            "/Library/GPUBundles/rdna3mtl"
#define RDNA3MTL_ALLOW_LIST     "/Library/GPUBundles/rdna3mtl-allow.txt"
#define RDNA3MTL_ALLOW_LIST_ALT "/Library/Extensions/rdna3mtl-allow.txt"
#define RDNA3MTL_GPUPASS_PROBE  "/Library/GPUBundles/rdna3mtl-gpupass-probe"
#define RDNA3MTL_CONF           RDNA3MTL_DIR "/rdna3mtl.conf"
/* Vulkan driver: Mesa RADV built for macOS against libdrm_xnu (Stage 5) */
#define RDNA3MTL_ICD_JSON       RDNA3MTL_DIR "/radeon_icd.x86_64.json"
#define RDNA3MTL_ICD_DYLIB      RDNA3MTL_DIR "/libvulkan_radeon.dylib"
#define RDNA3MTL_LOADER         RDNA3MTL_DIR "/libvulkan.dylib"
/* AIR -> SPIR-V translator (metal/translator, LGPL), renamed at build */
#define RDNA3MTL_XLATE_NAME     "librdna3mtl_translate.dylib"
#define RDNA3MTL_XLATE_DYLIB    RDNA3MTL_DIR "/" RDNA3MTL_XLATE_NAME
/* sysctl the accelerator kext sets once WindowServer may use it */
#define RDNA3MTL_ARMED_SYSCTL   "debug.rdna3accelfb"

/* What the device reports. RADV names the device "AMD Radeon RX 7800 XT
 * (RADV NAVI32)"; the marketing name is the part before " (RADV ". */
#define RDNA3MTL_VENDOR_NAME    "AMD"
#define RDNA3MTL_FALLBACK_NAME  "AMD Radeon GPU"
#define RDNA3MTL_VK_NAME_CUT    " (RADV "
#define RDNA3MTL_TAG            "RDNA3MTL"

/* NVIDIA-only lanes stay compiled but can never find their pieces on an
 * AMD install: NVIDIAShared (vendor compiler, env-gated anyway), the NVK
 * SASS dispatch hook (falls back to SPIR-V). The NVDEC/NVENC VideoToolbox
 * registrations are off: there is no such hardware, and a hardware codec
 * advertised with rating 1000 must never reach VideoToolbox (VCN 4.0
 * would be the AMD equivalent, not ported). */
#define RDNA3MTL_NV_VIDEO_CODECS 0

#endif
