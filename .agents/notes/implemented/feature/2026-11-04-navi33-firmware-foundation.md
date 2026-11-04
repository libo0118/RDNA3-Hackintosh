# Agent Note: Navi 33 固件准备不等于 RX 7600 XT 硬件支持

Status: implemented

## Problem

上游 `stage1-kext` 的基准为 `464d781d3bf4171d15d724d90cb1abc32608e2dd`，来源是 [Voten641/RDNA3-Hackintosh](https://forgejo.voten.me/Voten641/RDNA3-Hackintosh)。硬件名称表已经识别 `1002:7480`、revision `C0` 为 RX 7600 XT，但 bring-up 的 SOS、TA 与所有其他固件始终使用 Navi 32 文件名。只增加 PCI 匹配会把未经移植和实卡验证的初始化路径直接暴露给用户。

## Decision

本阶段只提供可在 host / CI 上验证的固件基础，不扩大 IOKit 匹配：

- `amd_fw_set_for_gc` 根据 discovery 中的 GC `11.0.3` / `11.0.2` 选择 Navi 32 / Navi 33 文件集；未知 GC 返回空指针，bring-up 在复位和固件加载前停止，不默认为 Navi 32。
- bring-up 的 SOS、TA、ucode 文件及名称诊断使用同一个文件集。Navi 32 的文件名、默认 CLI 行为与默认打包文件集保持不变。
- `package/firmware.sh --chip navi33`、`tools/ci/fetch_fw.sh DEST navi33`、`amd_fw_dump DIR navi33` 和 `make -C kext firmware FW_CHIP=navi33 FW_DIR=DIR` 显式准备或检查 Navi 33 文件；这些命令不启用硬件匹配。安装器仍默认 Navi 32，不提供 RX 7600 XT 安装流程。
- dump 对存在的 TA 执行 common header 校验和 `amd_psp_ta_parse`；TA 损坏即失败。缺失 TA 延续运行时的可选语义，但日志明确写出缺失。CI 下载步骤要求全部 13 个文件的校验通过，不能用可选语义代替完整文件集。
- `kext/Info.plist` 继续只匹配 `747e`，不包含 `7480`。不改寄存器、VRAM 大小、拓扑、VBIOS、系统保护或 Metal 行为。

### 一手依据与验证范围

- Linux v6.12 的 [gfx_v11_0.c](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c)、[imu_v11_0.c](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/imu_v11_0.c)、[mes_v11_0.c](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/mes_v11_0.c) 列出 GC `11.0.2` 的固件（包括 scheduler 的 `_mes_2` 和 KIQ 的 `_mes1`）。[psp_v13_0.c](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/psp_v13_0.c)、[sdma_v6_0.c](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c)、[smu_v13_0.c](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0.c) 提供相应 IP 的命名依据。
- 文件及 SHA-256 固定到官方 [linux-firmware 20260916](https://gitlab.com/kernel-firmware/linux-firmware/-/tree/20260916/amdgpu)，文件名由该版本 [WHENCE](https://gitlab.com/kernel-firmware/linux-firmware/-/blob/20260916/WHENCE) 核对。新增的 13 个文件从官方 raw 下载后逐一核对 common header 大小并计算 SHA-256；blob 不进入 Git 或本阶段 CI 上传的制品。
- [LLVM AMDGPUUsage 的处理器表](https://llvm.org/docs/AMDGPUUsage.html#processors) 将 RX 7600 XT 列为 `gfx1102`，是编译器目标的佐证，不是本机 IP discovery 或 macOS 初始化成功的证据。本机尚无 Linux 导出的 discovery，不能用离线文件表反推所有实际 IP 版本。
- 上游 `hw/smu13.c` 明确面向 SMU `13.0.10`，显示路径使用 `dcn32` 与其生成寄存器头；本阶段不声称这些代码已适配 Navi 33，也不将 Navi 32 simulator 的结果称为 RX 7600 XT 实卡结果。

### Testing

`tests/test_fw.c` 执行两个 GC 分支、完整 Navi 33 文件名和未知 GC 拒绝测试。`tests/test_fw_dump.py` 启动真实 dump 二进制，覆盖默认 / 显式两个文件集以及有效、损坏和缺失 TA。打包测试检查两个文件列表、固件复制与未知 chip 拒绝。针对性命令为：

```sh
make t.test_fw t.test_fw_dump t.test_fetch_fw t.test_make_pkg
make tools
tools/ci/fetch_fw.sh build/fw33 navi33
test -f build/fw33/.complete
build/host/amd_fw_dump build/fw33 navi33
```

本地 Windows 已执行 shell 语法检查、模拟下载失败测试及真实 Navi 33 文件复制 / 检查；该环境没有 C 编译器或 make，C 测试、真实 dump 和 macOS kext 编译由 CI 执行，不能提前记为通过。`python -X utf8 tools/progress.py` 保持上游状态指标不变。

## Alternatives considered

- **直接增加 `7480` 的 IOKit 匹配**：修改最少，也能让驱动绑定本卡；但绑定后会进入未经 Navi 33 验证的 SMU、显示及图形初始化。因缺少对应移植和实卡证据而不采用。
- **从每个 IP 版本动态拼接全部文件名**：可避免维护每芯片文件表；但 MES 变体、不同 IP 的命名规则及文件可用性需要明确依据。本阶段只维护官方版本中已核对的两个固定文件集，不声称支持任意 GC / IP 组合。
- **完全保留硬编码的 Navi 32 文件集**：风险最低，但无法通过同一 host 工具和打包入口验证本次 Navi 33 固件准备，故只保留为默认行为，不作为唯一文件集。

## Consequences

收益是将固件选择从营销名称与硬编码调用中分离，并获得无需加载驱动的回归入口。代价是需要同步维护 C 文件集与脚本清单；CI 的真实文件解析和测试负责发现缺项，不替代硬件验证。新建前检查本仓库没有既有活跃 Agent Note，因此没有需要吸收、互链或废弃的决定。

GitHub hosted 验证的触发、依赖和制品边界见[CI 决定](../process/2026-11-04-github-hosted-validation.md)。

后续 [SMU 状态与基础 mailbox 子阶段](2026-11-04-navi33-smu-status-mailbox.md) 分派 MP1 状态地址和接口提示，并隔离未移植的 13.0.7 PM ABI；它不取代这里的固件准备边界，也不启用硬件匹配。

[私有 SMU PM bootstrap 子阶段](2026-11-04-navi33-smu-pm-bootstrap.md) 继续增加真正的 13.0.7 表交换、DPM 与基础读取，不把固件选择或 CI 绿色当作 RX 7600 XT 的硬件完成；本篇文件集与未绑定决定仍保留。

[GFXHUB 子阶段](2026-11-05-navi33-gfxhub.md) 按真实 GC discovery 选择 3.0 / 3.0.3 的寄存器，覆盖早期 IRQ、页表与全部 flush 消费者；不取代本篇固件选择与未绑定边界，也不证明其余图形、显示或实卡路径完成。

[UAPI revision 修复](../bug-fix/2026-11-05-navi33-uapi-revision.md) 同样按实际 GC discovery 与 NBIO strap 生成用户态身份，未知 GC 的 DEV_INFO 明确失败，不改变本篇未绑定决定。

[ASIC 元数据发布](../bug-fix/2026-11-05-navi33-asic-metadata.md)复用早期真实 VBIOS / discovery，去除 Navi32 元数据假默认；[GC2 / VRAM 合成回归](../testing/2026-11-05-navi33-gfx-vram-fixtures.md)覆盖本文件集选择后的初始化与 3.0 解析，不取代未绑定或实机验证条件。

升级到可绑定 RX 7600 XT 的条件是：先取得本机 Linux IP discovery / VBIOS 参考，在对应 amdgpu 函数基础上核对并移植初始化差异，再单独评审受控实卡测试方案。本阶段的 CI 只能证明离线测试或编译，不证明裸机 Sequoia 15 出图、DP 热插拔、硬件提交完成、Metal 或功耗 / 复位正确。
