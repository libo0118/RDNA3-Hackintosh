# Stage 4: kernel–user interface (amdgpu UAPI)

The kext has no ABI of its own: it implements the **Linux amdgpu UAPI** (structures
from `amdgpu_drm.h`/`drm.h`), and userspace calls it exactly as it does on Linux.

```
RADV / libdrm_amdgpu (unchanged)
   │  drmIoctl / mmap
libdrm.dylib = xf86drm_xnu.c + drm_xnu.c      (userspace/libdrm_xnu)
   │  IOConnectCallMethod(sel 0: nr, payload) / IOConnectMapMemory64
RDNA3UserClient → RDNA3Accel::drmIoctl         (kext/src)
   │
uapi/rdna3_drm.c  (OS-independent, tested on the host)
```

## ioctl status

| ioctl | Status |
|---|---|
| `DRM_IOCTL_VERSION`, `GET_CAP`, `SET_CLIENT_CAP` | ✅ (`amdgpu` 3.61) |
| `GEM_CLOSE` | ✅ |
| `SYNCOBJ_CREATE/DESTROY/RESET/SIGNAL/WAIT/TIMELINE_*/QUERY` | ✅ a syncobj carries the CS fence and signals once the GPU passes it |
| `SYNCOBJ_TRANSFER` | ✅ copy of the fence/point (snapshot; syncobjs are per file) |
| `SYNCOBJ_HANDLE_TO_FD/FD_TO_HANDLE` | ✅ in `libdrm_xnu` (`drm_xnu.c`): the kext has no descriptors. **sync_file** = an unlinked temporary file holding the fence by value (`ip, ring, seq` or "signaled"), read/written through the private `RD_NR_XNU_SYNCOBJ_FENCE` (0xF0). It works across processes (fork, SCM_RIGHTS, XPC). The kext only accepts a fence that was actually submitted (`seq ≤ last_seq`), so a forged file cannot force a wait until the timeout and a GPU reset. **opaque** = an fd for `/dev/null` in the process table, same handle (within the process only). Test: `test_uapi` (GET/SET, rejection), `test_libdrm_amdgpu` (second connection + child after `fork()`) |
| `PRIME_*` | ⬜ `-ENOSYS` (needs IOSurface/export, Stage 7) |
| `AMDGPU_GEM_CREATE` | ✅ GTT: `IOBufferMemoryDescriptor`; VRAM: a heap in VRAM (from 512 MiB), mappable through BAR0 |
| `AMDGPU_GEM_MMAP` + `mmap` | ✅ through `clientMemoryForType` |
| `AMDGPU_GEM_VA` | ✅ MAP/UNMAP/CLEAR/REPLACE; on `rdna3lvl>=6` it writes real page tables (`hw/gpuvm.c`), a mapping holds a BO reference, `GEM_CLOSE` removes mappings like amdgpu |
| `AMDGPU_GEM_METADATA/OP/WAIT_IDLE`, `AMDGPU_VM`, `AMDGPU_CTX` | ✅ |
| `AMDGPU_INFO` | ✅ ACCEL_WORKING, DEV_INFO (from the GC discovery table), HW_IP_INFO/COUNT, FW_VERSION, VRAM_GTT, MEMORY, *_USAGE, READ_MMR_REG (soc21 allow list), MAX_IBS, VIDEO_CAPS, SENSOR (from SMU metrics: clocks, temperature, load, power, VDDGFX, pstate), GPUVM_FAULT (the client's last VM page fault) |
| `AMDGPU_CS` | ✅ chunks IB / DEPENDENCIES / SYNCOBJ_IN/OUT / TIMELINE / BO_HANDLES; the DMA ring is real from `rdna3lvl=7` (SDMA0, IB in the file's VMID), GFX/COMPUTE are only advertised (`rdna3enum`) |
| `WAIT_CS`, `WAIT_FENCES`, `FENCE_TO_HANDLE` (syncobj), `BO_LIST` | ✅ real SDMA fences; deadline as CLOCK_MONOTONIC (`mach_continuous_time`), 10 s limit |
| `GEM_USERPTR` | ✅ `IOMemoryDescriptor::withAddressRange` over the client task, `prepare()` for the BO's lifetime, pages through `IODMACommand`; no `mmap` (`-EPERM` like amdgpu); `amdgpu_create_bo_from_user_mem` in the loopback test |
| (all) | errno in Linux numbering from the UAPI; `drm_xnu` translates to macOS (`ETIME` 62→92, `ENOSYS` 38→78, `EOPNOTSUPP` 95→102), because RADV compares against the host's `-ETIME`/`-ECANCELED` |
| `AMDGPU_CTX_OP_QUERY_STATE/STATE2` | ✅ RESET / VRAMLOST / GUILTY from resets and page faults (`vm_query`), guilty → `CS` = `-ECANCELED` |
| Video IPs (`rdna3vcn=1`) | ✅ VCN 4.0 + JPEG 4.0 (`hw/vcn40.c`): `HW_IP_INFO` / `HW_IP_COUNT` for `VCN_DEC` (no ring on VCN 4: the unified ring is `VCN_ENC`'s, count 2), `VCN_ENC` (one ring, count 2), `VCN_JPEG` (one ring) as amdgpu_kms.c; `VIDEO_CAPS` decode / encode from soc21_query_video_codecs; `CS` to `VCN_ENC` (a context's entity is pinned to one instance like amdgpu_ctx_init_entity, VCN1 first; `vcn_v4_0_ring_patch_cs_in_place` moves AV1 encode and non-H264/HEVC/VP9 decode sessions to VCN0) and `VCN_JPEG` (the IB is checked by `jpeg_v2_dec_ring_parse_cs` and a kernel copy runs). Without the boot-arg the video IPs answer as before (no rings, zero caps). `rdna3vcn=2` (item I): the same rings with VCN dynamic power gating as amdgpu runs it on this card (DPG SRAM image through the PSP, `=3` through the LMA); VCN / JPEG are suspended (VCPU BOs copied to system memory) and resumed across a system sleep and a GPU reset |
| `AMDGPU_CS` chunk `FENCE` (user fence) | ✅ like `amdgpu_cs_p1_user_fence`: a single-page BO, not userptr, offset ≤ 4088 (and 8-byte aligned). The ring writes the 64-bit job number just before the ring fence (`amdgpu_ib_schedule`): SDMA with two FENCE packets, GFX/compute with `RELEASE_MEM` DATA_SEL 2. Address in VMID0: VRAM = MC address, GTT = one of 64 GART slots below the transfer window (remapped only once its last job has passed). The BO is held by a reference until the fence passes (`uf_pending`, released at the next CS / when the file is closed), so a `GEM_CLOSE` in the meantime does not free pages the GPU is writing to |
| BOs in `GDS` / `GWS` / `OA` | ✅ on-chip ranges (`gfx_v11_0_set_gds_init`: 4 KiB GDS, 64 GWS, 16 OA), size and alignment in units; `NO_CPU_ACCESS` (`mmap` = `-EPERM`), no `GEM_VA`, `VM_ALWAYS_VALID` = `-EINVAL`. `INFO_GDS_CONFIG` as in amdgpu 6.12. A job gets the partitions from the BO list (chunk `BO_HANDLES` or `bo_list_handle`, not both); the kext switches them from the ring (`gfx_v11_0_ring_emit_gds_switch`) only when the VMID has a different one; always after a reset. `gfx_v11_0_init_gds_vmid` at GC start. Current Mesa does not use GDS on GFX11; this is for UAPI compatibility |
| `BO_LIST` | ✅ CREATE/UPDATE/DESTROY; everything is resident anyway, so a list only holds references to its own GDS/GWS/OA BOs (up to 256 lists per file); unknown BO = `-ENOENT` |

## Enumeration mode (`rdna3enum=1`)

`libdrm_amdgpu` refuses to initialize when `ACCEL_WORKING == 0`. The boot-arg
`rdna3enum=1` reports `accel_working` and one GFX/COMPUTE/DMA ring each, so
`vulkaninfo` and the memory allocation paths work before Stage 2. Every `CS`
still returns `-ENODEV`.

## Tests

* `tests/test_uapi.c`: ioctls at the structure level.
* `tests/test_libdrm_amdgpu.c`: **unmodified libdrm_amdgpu** through
  `drm_xnu` with the loopback backend (UAPI in the same process). Covers
  `amdgpu_device_initialize` (with its `F_DUPFD`), `amdgpu_query_gpu_info`,
  BO + mmap + write/read, VA, contexts, syncobj, closing the original
  fd as RADV does.
* `tests/test_libdrm_amdgpu.c` also submits a **CS through unmodified libdrm_amdgpu**
  (`amdgpu_cs_submit_raw2` + `amdgpu_cs_query_fence_status`) to the loopback
  backend's software SDMA, which interprets WRITE/COPY/FILL/FENCE packets.
* On a Mac: `build/userspace/rdna3_drminfo` (kext at `rdna3lvl>=2`);
  `rdna3_drminfo --dma-test` (`rdna3lvl=7`) submits an SDMA IB with a BO in VRAM and
  checks what the GPU wrote: the first job from userspace on the card.

## To check on hardware

* `RDNA3,GBAddrConfig` in ioreg (`rdna3lvl>=3`): the fallback value `0x545`
  is only a placeholder; RADV computes texture swizzle from it.
* CU/RB harvesting: from `rdna3lvl=3` the kext reads the masks like
  `gfx_v11_0_get_cu_info`/`setup_rb` (`CC/GC_USER_SA_UNIT_DISABLE`,
  `CC/GC_USER_RB_BACKEND_DISABLE`, `CC/GC_USER_SHADER_ARRAY_CONFIG` on the SE/SA
  selected through `GRBM_GFX_INDEX`) and uses them to replace the default "all
  active" in `DEV_INFO` (`RDNA3,ActiveCUs`, `RDNA3,ActiveRBMask`). The RX 7800 XT
  should show 60 CUs; any other number means it must be compared with Linux.
