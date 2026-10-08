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
