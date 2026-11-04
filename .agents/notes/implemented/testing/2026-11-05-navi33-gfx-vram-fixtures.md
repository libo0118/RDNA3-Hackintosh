# Agent Note: 用独立合成输入覆盖 GC 11.0.2 GFX 与 VRAM_INFO 3.0

Status: implemented

## Problem

现有 GFX topology 与初始化回归明确运行 GC 11.0.3，不能证明 GC 11.0.2 discovery 会到达同一生产 autoload、RS64 与 golden 路径。已有 VRAM_INFO 3.0 只有 16-channel / 256-bit 的短表，缺少 8-channel / 128-bit、两个不同 vendor record、完整表边界和未知输出的独立输入。

本次没有确认上述 GFX 路径需要新的生产差异。不能把 `gc_11_0_3` 注释或头文件名当作错误，也不能用私有 ROM、产品名称或 `dram_size_per_ch` 反推硬件容量。

## Decision

只在现有 `tests/sim_navi32.c` 增加两个无条件运行的测试，不改生产 GFX / VBIOS parser，不增加新 `.inc`、Makefile prerequisite 或通用模型框架。保留全部原 GC 11.0.3 和 VRAM_INFO 2.x 回归。

### GC 11.0.2 GFX

- 复用 `make_discovery`，但将实际传入 `rdna3_dev_set_discovery` 的 GC revision 改为 2，并设置独立的合成 segment 0 / 1 / 2 基址。其他 IP 沿用模型；这不是本机完整 Navi33 discovery。
- 用得到的 GC IP 调用真实 `amd_fw_set_for_gc`，明确要求 Navi33 文件集及三个 `gc_11_0_2_{pfp,me,mec}.bin` 名称。独立合成 v2.0 header 的非零高、低起始地址经过生产 `gfx11_rs64_from_fw`；不读取任何实际固件或 ROM。
- 用真实 `gfx11_sw_init_topo` 断言 1 ME、2 MEC、每 MEC 4 pipes × 4 queues、默认 ring bitmap 和与 GC 11.0.3 相同的 FIFO 参数。
- 复用 `sim_gfxhub11.inc` 的 MMIO / ACK transport，仅记录 16 次 banked RS64 PC 写入。预期地址、ME/pipe selector、32-bit packed PC、高位与 TCP bit29 均采用固定上游数值，不从被测生成宏自证。
- 真实 `gfx11_hw_init` 分别覆盖 CP busy / autoload complete、CP idle / autoload incomplete 的 timeout，再覆盖两者满足后的成功。timeout 不进入 RS64、GFXHUB 或 golden；成功逐一核对 PFP / ME 的两个 pipe、MEC 的四个 pipe、GC2 GFXHUB context / 页表地址及保留其他位的 TCP_CNTL 写入。
- 使用生产 async CP resume 分支，成功返回后仍要求 `ready == 0`，因为本 fixture 不模拟后续 KIQ map / ring test。模型的 256 MiB 地址空间不是本卡容量。

### VRAM_INFO 3.0

- 独立构造一个合法 ATOM ROM 及 4238-byte、revision 3.0 的 VRAM_INFO 表，通过真实 `vbios_parse` → `vbios_data_table` → `vbios_vram_info`。字段为 memory type `0x70`、8 channels、channel_width 4、enable `0xff`、两个 68-byte module record（起始偏移 64 / 132）；两个 vendor 为 `0x31` / `0x46`，不是实卡数据。
- 断言结果为 GDDR6、128-bit、首 record 的低四位 vendor 1。module_id 0 / 1 / 9 均使用首 record，与 Linux 3.0 语义一致，不套用 2.x 的模块遍历。改变 channel_width、channel_enable、dram_size_per_ch 后仍为 128-bit，不能将这些字段误当容量或 2.x 宽度公式。
- 缺失表要求 `RDNA3_OK` 且完整重置输出为 UNKNOWN / 0 / 0。位于 ROM 范围内但声明长度为 4 / 64 / 67 的短表要求 `RDNA3_EINVAL`；3.1 和未知 4.0 同样要求 `EINVAL`，不放宽成任意失败或成功。
- 另一区分已有接口语义：声明完整长度但越过 ROM 一字节的表，被 `vbios_data_table` 拒绝为 `NULL`，所以 `vbios_vram_info` 依既有缺失表契约返回 `OK` 和 UNKNOWN / 0 / 0。这不是合法 3.0 解码，也没有将该路径伪装成 `EINVAL`。

根代理同时收紧 DEV_INFO 的真实 metadata 边界。`sim_navi32.c` 内两个既有 Navi32 UAPI fixture 在各自 `rdna3_drm_device_init` 后显式发布模型已知的 GDDR6 / 256-bit / 64 MiB MALL，不依赖被移除的生产全局默认值；它们不是 GC2 metadata 证据。

### 一手依据

只读核对固定 Linux v6.12 文本，不执行下载内容：

- [`gfx_v11_0_sw_init` 的 ME/MEC topology](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c#L1545-L1555) 和 [`gfx_v11_0_gpu_early_init` 的 FIFO](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c#L1056-L1074)。
- [`gfx_v11_0_config_gfx_rs64`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c#L2764-L2846)、[autoload 条件](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c#L2849-L2882)、[golden settings](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c#L245-L247) 及[共用分派](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c#L430-L447)。
- [GC 11.0.0 offset](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/include/asic_reg/gc/gc_11_0_0_offset.h) 和 [mask](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/include/asic_reg/gc/gc_11_0_0_sh_mask.h)。
- [`amdgpu_atomfirmware_get_vram_info` 的 3.0 分支](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/amdgpu_atomfirmware.c#L421-L439) 和 [`atom_vram_module_v3_0` / header 布局](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/include/atomfirmware.h#L3261-L3299)。

### Testing

针对性执行入口为 `make t.sim_navi32`。本次 Windows 环境仅发现 Python / Node，没有 clang / clang++ / gcc / cc / make；新增 C 断言、ASan/UBSan 和 native macOS 编译尚未运行，必须由根代理汇合后的 GitHub CI 执行。源头数值、词法平衡、差异和 Note 结构检查不替代 C 执行证据；本篇不声称 red/green 或硬件通过。

## Alternatives considered

- **只扩展原 topology 的 expected 文本，或把 GC3 测试改名为 Navi33**：改动最少，却没有真正改变 discovery、固件选择与生产初始化输入，因此增加独立 GC2 fixture，保留原 GC3 回归。
- **新建完整 Navi33 寄存器 / command processor 模型**：能够覆盖更多 ring 行为，但会复制已有 transport 并扩大未验证假设；本次只复用 GFXHUB transport 和一个窄 PC 写记录器。
- **只修改调用者的 metadata，跳过真实 VRAM parser**：容易验证导出值，却抓不到 3.0 module 选择、字段偏移、长度与缺失表语义，因此以独立 ATOM ROM 走生产 parser。

## Consequences

收益是现有生产共用路径获得明确的 GC2 / 128-bit 合成回归；代价是测试维护固定数值 oracle，未来上游布局或接口语义变化时必须独立重新核对，而非替换成生产宏让测试自证。

新建前检索 proposed / implemented / rejected 的 GFX、VRAM_INFO、Navi33 活跃 Note：[固件基础](../feature/2026-11-04-navi33-firmware-foundation.md)、[GFXHUB](../feature/2026-11-05-navi33-gfxhub.md) 和 [UAPI revision](../bug-fix/2026-11-05-navi33-uapi-revision.md) 部分重叠，保留其生产分派、清理与未知输入边界；[SMU PM bootstrap](../feature/2026-11-04-navi33-smu-pm-bootstrap.md) 的独立字节 fixture 方法继续复用，其 PM / Atom 板表决定不改变。其余命中属于 macOS 编译 / enum 验证，与本测试决定无关；没有需要吸收、删除或归档的提案。旧 Note 的反向链接由根代理汇合处理。

不改 PCI 7480 匹配、EFI、系统、真实 ROM 或硬件通过记录。合成输入与 CI 绿色只能证明模型 / 编译，不证明 RX 7600 XT 的物理显存容量、macOS 15 初始化、出图、DP、Metal、功耗或恢复正确。
