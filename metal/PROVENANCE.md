# Provenance of metal/

Imported from **NullMoth NVIDIA Driver for macOS**
<https://github.com/nullmoth/nvidia-macos-driver>, commit
`b9a5a2b35c6475b1df0e7438916df25ad5c6d1d8` (2026-10-08).

| Directory | Upstream path | Upstream licence |
|---|---|---|
| `metal/plugin/` | `plugin/` | PolyForm Noncommercial 1.0.0 (`LICENSE.PolyForm-Noncommercial-1.0.0`), © NullMoth Systems |
| `metal/translator/` | `translator/` | LGPL-3.0-or-later (`translator/LICENSE`, `translator/COPYING`), based on metal2vulkan © Anees Iqbal |

Use in this repository is under the permission the NullMoth author gave this
project (see `/NOTICE`): **AMD GPUs only**, without interfering with the
NullMoth NVIDIA driver. The translator additionally stays LGPL-3.0-or-later:
it is built as a separate dynamic library and its source is shipped here.

The first commit that adds this directory contains the files byte-for-byte
as upstream; every AMD adaptation is a later, separate commit so the diff
against upstream stays reviewable (`git log -- metal/`).

## Synced with upstream `cd8e2bb4` (2026-10-13)

The upstream changes to `plugin/` and `translator/` since `b9a5a2b` (about
40 commits: translator fixes for OpenCL, Blender, CoreImage and stage
outputs, WebGL in Chromium, the Unity command-buffer lock-up, surface
upload copies, texture swizzle, the surface-writer map lock, the sandbox
`sysconf` fallback) applied as one diff. Kept as ours where they meet the
AMD adaptation: the compile-time vendor (`rdna3mtl_config.h`) instead of
upstream's run-time NVK/RADV/ANV switch, whose AMD path is an app-only GPU
beside another one's WindowServer (`nvmtl_vk_vendor` here is fixed to AMD
and never headless), the NVIDIA video codecs compiled out, no ANGLE
`disableMetalOnNvidia` override, our allow list.

