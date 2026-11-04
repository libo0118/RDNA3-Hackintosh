# Agent Note: Navi 33 使用私有 SMU 13.0.7 PM ABI，而不是解除旧表保护

Status: implemented

## Problem

[状态与 mailbox 子阶段](2026-11-04-navi33-smu-status-mailbox.md) 修复 flags 地址和 MP1 segment 边界，但刻意拒绝全部 PM 与独立 DAL，不能作为 RX 7600 XT 的最终适配。旧 `rdna3_smu` 嵌入 13.0.0 的 PPTable、metrics、OD，bring-up 又直接读取这些表字段；仅删 guard 会把原先不可达的旧 ABI 解释器暴露给 13.0.7。

## Decision

`hw/smu13_0_7.c` 单独包含固定 Linux v6.12 的三份 vendor header，保留 MIT 原文和来源；它不 include 13.0.0 header。`smu13_0_7.h` 仅跨越已解码的中立值，`smu13.c` 以实际 discovery 的 MP1 分派。Navi32 的 vendor 布局、已有功能及默认次序保留，不能用 Navi32 模型推断本卡拓扑。

- 13.0.7 仍在 hello 前读取 `0x03b10028`。PM 仅接受本实现确知的 driver IF `0x35`；hello 本身延续 Linux 的 warning 语义，不能以一次版本查询成功证明表兼容。PM 需要已发现的 THM segment 0 与真实 PCIe caps，不使用“Gen4 x16”的默认值代替缺失输入。
- bootstrap 按 `smu_smc_hw_setup` 及实际注册的 13.0.7 回调执行：设置 driver/tools 地址，读取 combo，校验 header 范围与 driver-portion 偏移；非 SCPM 从有效 Atom ROM 的 `smc_dpm_info` common header 后复制完整 BoardTable 并上传 PPTable；SCPM 不拼板表、不上传、不写 allowed mask。IF/长度错误在表上传前返回，缺 VBIOS 的非 SCPM 不能静默写回 PMFW 的板表。
- Board/SKU Version 是每板/SKU 的 opaque 标识，不是 `PPTABLE_VERSION 0x27`。Linux 未定义 `smc_dpm_info` revision 白名单，也未把注释中 “For PLUM_BONITO” 的外层示例值作为全体 Navi33 门槛，因此不虚构这些约束。ROM/Atom parser 保证表范围，再检查完整 BoardTable 的长度，原样复制所有 opaque 字节。
- allowed features 从零按 `smu_v13_0_7_get_allowed_feature_mask` 构造，再应用调用者要求的功能限制；非 SCPM 高 32 位先发、低位后发。其后使能、读取 enabled/DPM clocks、按 PCIe caps 更新链路、配置 thermal alert 和默认 workload。13.0.7 没有 `run_btc` / `notify_display_change` 回调，因此不因消息 ID 存在就发 RunDcBtc 或 DALNotPresent；也不套用 Navi32 的 SCPM UCLK/FCLK 顶频策略。
- 基础频率、温度、功耗、活动率、风扇转速、只读 INFO 使用真实 13.0.7 metrics 布局与单位；13.0.0 metrics/PPTable 在模型中被 poison 后仍应读到正确数据。13.0.7 没有 unique-ID 回调，保留 serial 偏移断言但不暴露旧 unique-ID 输出。
- COMBO 消息不携带长度，发出前校验完整 `0x4000` 交换区，CPU 仍只复制 ABI struct 的 4804 字节。MACO 元数据必须同时满足 BACO cap、MACO cap 及 HSR/VddqOff 条件。FAN_CONTROL 运行位仅表示 firmware 控制可用性，不从 SKU FeaturesToRun 猜“板上无风扇”；INFO 的既有 `no_fan` 消费者显示的是 “no fan control”，未观察到的模式为 `UINT32_MAX`，不能误报手动模式或 full-speed。
- 原始 13.0.0 表 API 对 13.0.7 保持拒绝，避免已有 OD / 风扇 / profile helper 穿透。用户的手动风扇、功率限制写、OD、BACO、强制性能级等不属于本阶段必要启动功能，外部 pmctl 明确只支持 INFO；读取失败的 INFO 返回错误，不制造 `pm_up=1`。
- DCN320/321 共享独立 DAL mailbox 的固定寄存器，普通和 delay 发送共用同一 transport；GC 11.0.2 的 hard-min status 门槛为 `0x524e00`，不是 Navi32 的 `0x503900`。DAL IF `0x18` 不与 driver IF `0x35` 混用。DCN321 的 FCLK 通知从真实调用处跳过；这不表示其余 clock-generator/display 寄存器已经适配。
- `dcn32_clk_read` 按实际 GC 11.0.2 使用 Linux 321 的 DFS 起始寄存器 `0x16e64`，11.0.0/3 使用 320 的 `0x16e69`，同用 PLL `0x16e37` / stride 3 / 原 VCO 算法。其返回值从 void 改为 int，未知 GC 在任何 clock MMIO 前失败；modeset、state、clone、scaler、pstate 与 kext 报告消费错误，不能拿错误读数判断是否 raise。divider-127 判断也使用选中的组。
- stats / sensor 共用生产 C 的 `smu13_read_metrics_cached`，必须同时有有效数据与未过期 timestamp 才用缓存。成功 DMA 才递增 generation；pmctl 只有整体成功且真实刷新 metrics 才推进 timestamp，失败或未刷新会失效缓存。此修复同时覆盖 13.0.7 与 Navi32，不把失败 INFO 后 100 ms 内的旧值 / 初始零冒充新读数。
- 13.0.7 的初始 AC/DC 来源为未知，不发送无真实 OS 输入的 NotifyPowerSource(AC)。SKU 的 AC/DC 额定值不是当前供电状态；当前 PPT 限制以既有整数域的 `UINT32_MAX` 表达不可用，CLI 显示 unavailable；IORegistry 的未知功率上限移除数值属性并发布相应 Available=0，不把 UINT32_MAX 展示为瓦数，合法零值仍保留。THROTTLERS 未运行同样只使限制不可用，不阻断实际平均功率、温度、频率 metrics 或 INFO。现有真正 FW AC/DC 中断能确定固件上下文，再按对应 source 读取限制；macOS 电源 provider 与完整 OS late-init 策略仍未接入，不加假回调或新开关。Navi32 原有 exact 模式行为保留。
- boot 读取区分已证实的 firmwareinfo 布局：v6.12 明确将 content 0/1/2 按 3.1 leading fields 读取，3/4 使用各自已定义前缀；只要求真正读取字段的最低 38/64/88 字节，不拒绝合法尾部。已知 3.5 的 +8/+12 与电压字段是 RESERVED，不解释成 boot clocks / voltages，只解码已有定义的 metadata 与 smu_info 时钟。需要 GFX/UCLK boot fallback 的非 SCPM 请求在 PPTable 上传前明确不支持；独立、实际运行的 DPM 枚举不因未使用的 boot 字段被阻断。3.5 的可靠 boot-clock fallback 来源仍未实现，未知更晚 revision 不猜兼容；Navi32 共享 parser 不改。
- 13.0.7 mode1 reset 使用 Linux 共用 debug mailbox；正常 fini/shutdown 只停 thermal alert，PMFW 自行管理 DPM，不发 DisableAll / 盲目 unload，也不清 firmware flags。晚期启动失败清理已开启的 thermal alert；不发缺乏上游依据的固件回滚命令。

### 一手依据

固定 Linux v6.12：[13.0.7 表及回调](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0_7_ppt.c)、[共用初始化/cleanup](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/pm/swsmu/amdgpu_smu.c)、[共用 SMU 13 mailbox/thermal/reset](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0.c)、[Atom firmwareinfo 定义](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/include/atomfirmware.h#L478-L644)、[DCN32 DAL transport](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/display/dc/clk_mgr/dcn32/dcn32_clk_mgr_smu_msg.c)、[DCN321 分支](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/display/dc/clk_mgr/dcn32/dcn32_clk_mgr.c)。新增 vendor 结构的 sizeof/offsetof、表 ID、clock/feature bits 由生产 C 编译断言约束；本机未执行 C 编译，断言结果待 CI，不能用 Python 结构计算替代。

### Testing

`tests/test_smu13.c` 与 `sim_smu13_0_7.inc` 链接生产 C；IP discovery 和 Atom ROM 经过真实 parser，字节 fixture 使用 v6.12 的独立数字偏移，不含实卡数据。覆盖 SCPM/非 SCPM、normal/exact、opaque 零板号、板表复制、完整 COMBO DMA 区 / 空 CPU 映射、IF/尺寸/ROM/显存不足、逐阶段拒绝、feature/clock 映射、metrics sentinel、DAL UCLK pin/release 与门槛、320/321 错组哨兵与“clocks 已足够”决策、divider-127、手动控制隔离、reset/fini、MACO/FAN 条件、失败 INFO 后立即 stats/sensor 共用缓存消费、THROTTLERS disabled 及真实 FW AC/DC 上下文切换。原 `sim_navi32` 是完整 Navi32 回归入口。`test_pm_property.py` 提取生产 C++ 的功率属性发布方法，以内存 owner 执行已知→未知→零→已知转换，不把源码 grep 断言当行为测试；它不模拟真实 IOKit 生命周期。

```sh
make t.test_smu13 t.test_discovery t.sim_navi32 t.test_docs_html
python -X utf8 tools/progress.py --check
```

Windows 本地没有 C compiler / make：没有声称新增 C、native kext 或实卡通过。Python/docrows/progress 与 Note 门禁独立执行；真正 C/ASan/UBSan、完整回归和 macOS native 编译必须由根代理的 [GitHub hosted CI](../process/2026-11-04-github-hosted-validation.md) 执行，结果另据真实日志记录。

`tools/progress/sources.txt` 增加 13.0.7 一手文件。全量 `update_index.py` 的联网刷新实际遇到 `WinError 10054`，因此仅以该脚本原有 `functions()` 和已固定的本地 Linux v6.12 原文添加 42 行新索引，其余索引不动，再真实运行 `progress.py`。新增 13.0.7 分组的硬件通过数保持 0，未修改 `hw-verified.txt`。

## Alternatives considered

- **将 vendor0 与 vendor7 名称混在同一 header，或用 vendor0 struct 解释两代数据**：改动行少，但宏/类型冲突，且 metrics serial 差四字节，不能用整体大小恰好相同证明语义相同。采用私有翻译单元和中立摘要。
- **仅删拒绝、保留 Navi32 的全 1 allowed mask 与自定义初始化**：复用最多，但会允许 13.0.7 保留位，误发未注册回调及 SCPM 顶频写；因此只共享 Linux 已命名的共同算法，差异显式分派。
- **对所有板表强制 `Version == 0x27` 或猜一个 VBIOS revision**：看似更严格，但把 opaque 板级标识误当 ABI 版本，会永久拒绝真实输入。采用实际 MP1/已知 IF、完整表范围和真实字段约束，不添加没有资料依据的等值校验。
- **建立覆盖未来版本的 ops 框架或同时移植所有 OD/手动调节**：接口更灵活，但启动不需要、会扩大验证面。本阶段仅完成必要 bootstrap/读取与真实 DAL 路径，未移植调节明确不支持。
- **沿用“桌面卡总是 AC”的常量**：能直接复用旧 late-init，但 Linux 的 current PPT 按真实 `adev->pm.ac_power` 选 source，卡名 / 无电池 / SKU 都不能代替该输入。本阶段保持固件策略，缺输入时限制不可用，独立实际功率读取仍工作。

## Consequences

后续本机 VFCT 实际输入的 Atom 5.0/460 字节板表修复见[非 MMHUB 前缀决定](../bug-fix/2026-11-05-smc-board-prefix-padding.md)。它修正这里最初要求完整 wire BoardTable 的边界，其余 PM ABI 与分派决定保留。

[GFXHUB 子阶段](2026-11-05-navi33-gfxhub.md) 补齐 GC 11.0.2 的 3.0 寄存器与专用 CP_DEBUG 步骤，覆盖 IRQ / 页表 / flush；它与本篇 PM ABI 决定并行，不改变 MMHUB、显示和实卡验证仍未完成的边界。

收益是 13.0.7 不再只是被拒绝的安全子集：已有真正的版本专用表交换、特性/DPM 初始化和基础遥测生产实现。代价是增加私有 ABI 文件及中立摘要维护，外部表 ABI 变化必须重新审查，启动后失败时 firmware 可能已启用部分功能；不能以未经 Linux 证明的写命令“撤销”。

本次审计三篇活跃 Note：[固件准备](2026-11-04-navi33-firmware-foundation.md) 与本篇部分重叠，保留固件选择和未绑定边界；[状态/mailbox](2026-11-04-navi33-smu-status-mailbox.md) 的部分拒绝边界由本篇取代，保留其状态/segment 原因并双向链接；[CI 决定](../process/2026-11-04-github-hosted-validation.md) 验证边界不变。无完全吸收或应删除的提案。

没有 E 盘、Windows/BIOS/固件写入、硬件加载或 PCI 7480 绑定。缺少本卡 Linux discovery / VBIOS / SCPM / PMFW 返回及其余 DCN321/图形 bring-up 移植，硬件指标保持未实测。模型与 CI 均不能证明 RX 7600 XT 在 macOS 15 出图、DP、Metal、功耗或恢复正确，也不把本子阶段称作显卡完成。
