# Agent Note: GitHub hosted CI 只验证源码和固件解析

Status: implemented

## Problem

用户明确要求将 Forgejo 上游派生到 GitHub，并使用 GitHub CI 检查 RX 7600 XT 的适配基础。上游工作流依赖原作者的 `linux` / `macOS` 自托管 runner，不能直接在新账号执行；它的硬件成绩也不能作为本机的验证结果。

## Decision

本派生仓库使用标准 GitHub hosted runner，不访问原作者的 runner、令牌或硬件。保留上游历史、许可和来源，源代码阶段遵循[固件基础决定](../feature/2026-11-04-navi33-firmware-foundation.md)。

- Ubuntu 执行完整离线 host 测试和工具构建；既有真实固件子用例的跳过会明确记录。
- 独立的 Navi 32 / Navi 33 matrix 下载固定版本、SHA 校验的官方固件，并要求完整文件集和真实解析成功；下载或解析失败不能算作通过。
- macOS 15 Intel / Xcode 16.4 编译四个 x86_64 kext 和必要用户态工具。core、framebuffer、AGDC 和 accelerator 的导入检查均严格失败；accelerator 的旧“只报告”阶段已由[命名枚举修复](../bug-fix/2026-11-05-macos15-accel-enum-types.md)取代。名称/provider 检查仍不冒充 ABI 或可加载验证。
- GPU 验证工作流只读仓库权限，不加载 kext、不访问实卡，不构建或验证 Metal / RADV / CTS。GPU 制品只包含二进制、plist 和日志，不含 GPU 固件；动态库必须以客户端的版本化 install name 一起打包。
- [蓝牙候选](../../../../support/mtkbt/EXPERIMENT.md) 使用独立手动 workflow，先执行共享启动策略测试，再编译与核验内核导入。该作者加载器本来将未修改的 `_1_1` 固件嵌入二进制，因此制品保留全部原许可证和来源，不声称它是无固件制品；未知 ROM 或读取失败必须停止。CI 不安装或加载它，也不证明本机配对可用。
- 无线辅助任务只编译与 U 盘 MtkWlan 0.12.1 相同提交的 `mtkwifi`。源码 ZIP 固定提交与 SHA，直接调用已审阅的编译命令，不运行第三方安装脚本、CLI 或 LaunchDaemon；制品保留许可证和未验证联网的说明。这补齐作者 release 缺少预编译 CLI 的实际缺口，不代表 WPA2 可用。
- 原 `probe.yml` 移为 `.github/fixtures/probe.yml`，继续供已有脚本测试读取，但不再作为依赖原作者 runner 的活动工作流。

## Alternatives considered

- 原样使用上游工作流：最接近作者环境，但新账号没有这些私有 runner，会一直排队，故不采用。
- 只做 YAML 检查：本机可以快速执行，但不能发现 C/C++ 编译和真实固件解析错误，不满足用户要求。
- 给 `7480` 启用驱动并把 CI 绿色视为支持：可以制造容易展示的结果，但 CI 没有该卡，无法证明初始化、DP 或 Metal，因此不采用。

## Consequences

Intel hosted runner 无法领到任务时的独立补充证据，见[交叉编译与固定原生名称级契约](2026-11-05-macos15-cross-native-contract.md)。它保留本原生工作流，不将静态 provider 判定冒充实际 KC 链接、加载或硬件运行。

CI 绿色仅表示列出的离线测试、解析和编译通过，不能证明 Sequoia 裸机启动、签名加载、出图、硬件加速或稳定性。完整图形用户态栈和本机实卡测试仍是后续工作；未启用的 RX 7600 XT 匹配保持未启用。

新建前已审计唯一现有活跃笔记：固件基础笔记与本记录部分重叠，保留并互链，不吸收或废弃其硬件边界。原 USB EFI 和机器身份数据不进入此公开仓库。

[原生枚举符号探针](2026-11-05-macos15-accel-symbol-probe.md) 以不可部署的临时候选核对七个 accelerator 方法名，不取代现有构建，也不声称 ABI、加载或硬件通过。
