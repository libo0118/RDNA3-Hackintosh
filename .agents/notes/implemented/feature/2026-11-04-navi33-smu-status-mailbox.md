# Agent Note: Navi 33 SMU 状态与基础 mailbox 不等于 PM 移植完成

Status: implemented

## Problem

[固件准备阶段](2026-11-04-navi33-firmware-foundation.md) 选择 Navi 33 文件名，但 `smu13.c` 仍固定按 13.0.0/10 读取 firmware flags、提示接口版本和处理 PM ABI。未来若直接启用 `7480`，会读错状态地址，还可能将 13.0.0 表、特性掩码与卸载清零逻辑用于 13.0.7。两个状态常量正确不等于 PM 可以安全启动。

## Decision

本阶段只提供经 MP1 discovery 选择的状态读取与三个共同查询，不启用 `7480` IOKit 匹配：

- `rdna3_dev_set_discovery` 将 parser 保留的实例 0 segment 数记录到 `rdna3_ipver.num_base`，已发现 MP1 却缺少 segment 0 时记录 IP 版本并返回 `RDNA3_ENODEV`。segment 存在性由数量决定，不以 base 地址是否为 0 推断。`check_mp1` 同样要求该 segment，只接受实际标记 present 的 MP1 `13.0.0`、`13.0.10` 和 `13.0.7`；未知、缺失或缺段在寄存器访问前停止，不静默默认 Navi 32。所有 fixture 经真实 parser 填充该字段，不保留旧 fixture 的隐式默认。
- `smu13_check_fw_status` 为 `13.0.7` 读 SMN `0x03b10028`，为 `13.0.0/10` 保留 `0x03b10024`。生产 `smu13_hello` 在 `13.0.7` 第一个查询前调用它；flags 未 ready 时不写 driver mailbox，不记录 ready，`hwInit` 的现有返回值分支发布 `failed: SMU`。仅在 PM init 中检查不足以覆盖该路径，因为 13.0.7 PM init 已提前拒绝。`13.0.0/10` 的 hello 与 PM init 原顺序不变。IF 期望分别为 `0x35` / `0x3d`，仍按 Linux 只对版本不一致报警；这不是表兼容性证明。
- `smu13_send_msg` 对 `13.0.7` 只允许 `TestMessage`、`GetSmuVersion`、`GetDriverIfVersion`。这会写 mailbox 的响应、参数和消息寄存器，不是纯读取探针；其他 driver-mailbox PM、VCN/JPEG 与 `DALNotPresent` 控制均返回 `RDNA3_EOPNOTSUPP`。
- 独立 DAL mailbox 不经过 `smu13_send_msg`，且 `hwInit` 在 `pmInit` 拒绝后仍进入默认 `displayUclkPinInit`。`dcn32_clk.c` 的普通与 delay 发送共用 `dal_send`，在轮询或写入绝对 DAL 寄存器之前只接受带 segment 0 的 MP1 `13.0.0/10`；13.0.7 返回 `RDNA3_EOPNOTSUPP`，未知/缺失/缺段返回 `RDNA3_ENODEV`，日志包含 present、segment 数和版本。拒绝 delay 发送也计满状态等待预算，不能产生新的空转循环。Navi 32 的发送顺序、成功返回值与原等待预算保持，不在每个上层调用者重复加守卫。
- PM 初始化在分配、读取或写入表之前拒绝 `13.0.7`。表写入口在 CPU 写 VRAM 之前拒绝；PM 查询与外部 `pmctl` 在 ABI 解码前拒绝，不能在 INFO 伪造 `pm_up=1`。直接 debug reset、卸载及 firmware flags 清零入口同样拒绝。不是给每个内部函数增加重复守卫；实际 kext 的传感器、风扇、BACO、IRQ 等后续入口仍由成功初始化后才置位的 `fPmUp` 隔离。
- `13.0.0/10` 的原 PM、mailbox、状态地址及 BACO 退出后的 flags 清零保持。没有将清零写地址简单改到 `0x03b10028`；本阶段也不声称实现 Linux 13.0.7 的卸载。
- bring-up 成功阶段写为 `PSP + SMU mailbox ready`；`PMResult` 和 `fPmUp` 继续单独表示 PM 结果。没有新增 boot-arg、配置开关、兼容层或硬件通过记录。

### 固定一手依据

以下均固定 Linux v6.12，未使用未发布版本或推测寄存器值：

- [`smu_v13_0_7_ppt.c:71,391–403`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0_7_ppt.c#L391-L403) 定义并读取 `0x03b10028`；同文件 [`89–92`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0_7_ppt.c#L89-L92) 映射三个查询。
- [`smu13_driver_if_v13_0_7.h:28–31`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu13_driver_if_v13_0_7.h#L28-L31) 为 IF `0x35`、PPTABLE `0x27`。这里只引入 IF 提示，不导入或复用该表类型；`hw/gen/smu13_driver_if_v13_0_0.h` 仅供原 PM 路径使用。
- [`smu_v13_0.c:2456–2463`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0.c#L2456-L2463) 按 discovery 的 MP1 base 使用 C2PMSG 66/82/90；[`smu_v13_0_7_ppsmc.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu_v13_0_7_ppsmc.h) 的查询 ID 为 1/2/3，未定义 `DALNotPresent`。
- [`smu_v13_0_7_set_mp1_state:2513–2527`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0_7_ppt.c#L2513-L2527) 只调用共同卸载路径，没有 13.0.10 的额外 flags 清零。完整 PM 还涉及 [`allowed features:265–332`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0_7_ppt.c#L265-L332) 的从零构造算法，而非现有全 1 掩位策略。

### Testing

`tests/test_smu13.c` 的所有 fixture 执行真实 `amd_discovery_parse` → `rdna3_dev_set_discovery`，链接并调用生产硬件 C。独立模型检查：两代 flags 地址；生产 hello 的三个查询及参数、IF 正常/报警；13.0.7 新 flags 为 0、旧 flags 与响应寄存器均为 ready 时仍失败，只有 SMN index 写入而无 mailbox 查询；初始/完成超时及拒绝；未知 MP1 零 MMIO/VRAM 写入；13.0.7 表/feature/VCN/reset/unload 隔离；Navi 32 flags 清零不变。

`test_navi33_bringup_boundary` 按 `hwInit` 的生产入口顺序执行 hello → 被拒绝的 PM init → UCLK pin init/update，断言只有三个基础 driver 查询写入、没有 DAL/表写入。`test_pm_isolation` 也用 stale ready 对象尝试直接 hard-min 控制，确认不能绕过 DAL 边界。`test_mp1_segment` 为三个版本检查“MP1 记录存在但 num_base=0”的合法格式输入被 device 边界拒绝；把错误绝对 `0x29a` 预置 ready 也不能触发一次访问；另一份真实解析 fixture 的 segment 0 地址为 0，必须接受。`test_dal_navi32` 对 `13.0.0/10` 执行 UCLK 初始化、双头 pin 与单头 release，检查 DAL 2/0xb/9/0x15 顺序、参数及 delay 变体。原 `sim_navi32` 已显式发现 MP1 `13.0.10` 和三个 base，无需兼容默认或改写它的 PM fixture。

针对性命令：

```sh
make t.test_smu13 t.test_discovery t.sim_navi32 t.test_docs_html
python -X utf8 tools/progress.py --check
```

Windows 本地未发现 `cc` / `clang` / `gcc` / `make`，因此没有冒称 C 测试或 native kext 编译已执行；需由根代理推送后以 GitHub hosted CI 执行上述 C 测试、完整 `make test` 与 macOS native 编译。文档检查、UTF-8 progress 生成和 Note 门禁是本地独立检查，不替代 C 执行或实卡验证。

## Alternatives considered

- **只改 flags 地址与 IF 常量**：差异最小，也可修正孤立读取；但不能阻止 13.0.0 表、全 1 allowed mask、metrics serial 错位或卸载清零进入 Navi 33，因此不采用。
- **本阶段直接导入完整 13.0.7 ABI 并打开 PM**：能更快接近最终支持；但 PPTable/BoardTable、metrics、allowed mask、DPM、风扇/功耗/OD、display/VCN、BACO/reset 需要逐项移植和 ABI fixture。缺少本卡 discovery/VBIOS，不把未完成的行为伪装成成功。
- **每个内部函数都添加 IP 守卫或建立 ops 框架**：表面上覆盖广，但重复检查掩盖真实入口，且为仅有两个行为集合引入额外结构。采用共享 mailbox、表写、硬件初始化和外部控制边界，加现有 `fPmUp` 生命周期门。
- **只在 `hwInit` 的 UCLK pin 调用处跳过 13.0.7**：diff 最小，可切断默认路径；但 modeset、pstate、audio 等也直接调用独立 DAL sender，而且 delay 变体自己写寄存器。将普通与 delay 的发送合并到同一个边界，而不是依赖某个 caller 没有继续执行。
- **用 `reg_base[MP1][0][0] != 0` 判断 segment 存在**：无需增加内部字段；但无法区分缺段和合法地址 0，故保留 discovery 自带的数量并在 device 边界校验。

## Consequences

收益是得到可测的 13.0.7 状态/查询分派和明确的 PM ABI 隔离；代价是 Navi 33 PM、控制与卸载仍拒绝，不能出图或提供加速。未来完整 PM 移植必须同时替换这些拒绝边界、加入独立 ABI 断言和二进制 fixture，不能仅删除保护。

新建时审计两个活跃 Note：固件准备与本阶段部分重叠，保留并双向互链；[GitHub CI 决定](../process/2026-11-04-github-hosted-validation.md) 与验证边界相关但决定不变，保留，不吸收或归档。没有被本阶段完全取代的提案。

没有触碰设备、U 盘、系统、固件 blob 或 `hw-verified.txt`。缺少本机 IP discovery、VBIOS、SCPM、PMFW 运行数据和实卡日志；CI 绿色也不能证明 RX 7600 XT 的 SMN/BAR 可访问、Sequoia 15 启动、DP、Metal、风扇/功耗或复位正确。
