# Agent Note: macOS 15 Intel accelerator 的隔离枚举符号名探针

Status: implemented

## Problem

[原生 run 41880017271 / job 126749743304](https://github.com/libo0118/RDNA3-Hackintosh/actions/runs/41880017271/job/126749743304) 的真实环境是 macOS 15.7.9 / build 24G830、Xcode 16.4 / SDK 15.5。accelerator 的 914 个导入只有 907 个得到 provider；七个未解析名称覆盖 `IOAccelSurface` 的四个 shape / lock 方法、`IOAccelGLContext2::processSwap` 和两个 legacy display 类的 `setStereo`，对应参数仍编码成 `j`。

`accel/iofam/iofam_types.h` 的四个类型目前是 `unsigned int` typedef。`gen_tahoe_fwd.py` 关于 macOS 26 命名枚举的注释不能证明 macOS 15 的实际签名；直接修改生产头或搬入 Tahoe 虚表槽位会把名称假设误当 ABI 证据。

## Decision

独立 `.github/workflows/macos15-accel-symbol-probe.yml` 只接受手动 `workflow_dispatch`，使用 `macos-15-intel`，固定 Xcode 16.4 build 16F6 / SDK 15.5，并在运行时检查原生 x86_64、macOS 15 和 Kernel headers。`toolchain.log` 分别记录真实 `uname -r` 与 `sysctl -n kern.osrelease`，要求两者相同且为 Darwin 24。条件不符即失败，不改用 ARM 或其他 SDK。权限仅 `contents: read`，沿用仓库已有 SHA 固定的 checkout / upload-artifact。

- 先在 checkout 构建正常 accelerator，对实际 x86_64 executable 执行 `nm -u`，并运行真实 `kmutil libraries -p ... --undef-symbols`。
- 用 `git archive "$GITHUB_SHA"` 将同一提交的全部公开源码解压到 `RUNNER_TEMP` 内 `mktemp -d` 独占目录，不读取或复制本机私有数据。工具拒绝 checkout、临时根目录、符号链接头文件和发生漂移的头。
- 仅在该副本把四个原文各精确出现一次的 typedef 替换为 `enum Name : unsigned int;`，不定义枚举值，不改变任何 vtable 声明或槽位，保存可审阅的公开源 patch。生产头保持不变。
- 在副本重建 accelerator，再采集真实 x86_64 `nm` 和当前 Mac `kmutil` 输出。共享 `kmutil_kpi_guard.py` 的 `parse_nm_undef` / `parse_kmutil` / `check`，不新增 Mach-O parser、第三方依赖或替代 guard。
- 正常构建必须当场仍有恰好七个指定未解析导入；同时其他 KPI 策略必须通过。两次 imports 集合的移除项必须精确为原七个，新增项必须精确为这四个命名枚举产生的七个候选名称；不能只数七个，不能把旧清单当当前 provider 证明。
- 候选运行无 `warn_unresolved` 的 strict guard，包括现有最新 XNU 清单与声明库策略。该旁载清单的 `xnu-12377...` 等标签只表示仓库策略版本，不能当作 host kernel 版本；当前 provider 来自本机真实 `kmutil`，host 版本来自前述系统命令。JSON 记录实际候选名称、provider、剩余 unresolved 和 guard 返回码；仍未解析或其他 guard 拒绝明确为负面结果且非零。名称假设或差异本身不成立也失败，不回退到原 typedef。
- 制品只显式选择日志、小型 `result.json`、公开 `candidate-header.patch`、`SOURCE.txt` / `SCOPE.txt`。不上传正常或候选 kext、Apple 二进制、固件、秘密或原始 ROM，不执行安装、加载、KC 创建或 USB / EFI 操作。

### Testing

本地入口为 `python -I -B tools/ci/test_macos15_accel_symbol_probe.py`，覆盖四处精确替换、源码副本边界、checkout 不变、漂移 / 重复声明拒绝、真实 baseline 集合要求、差异两侧精确检查，以及共享 guard 的正面 / 候选未解析 / 正常构建额外违规和对应 JSON。fixture 的 provider 是合成输入，不是 Apple 真实导出证据；手动 workflow 自身也执行这组 selftests，既有 regular host CI 也持续运行这五项测试。

Windows 已实际运行上述 5 个单元测试及既有 `tests/test_kmutil_kpi_guard.py`，均通过；两个新 Python 文件通过 AST 语法检查。PyYAML 解析与手动触发、只读权限、Intel runner、固定工具链路径、action SHA、制品白名单断言通过，6 个 `run` block 全部通过 Git Bash `bash -n`；三项 Note 门禁通过。实际 Mac 编译及原生 provider 探针必须在后续推送后由主代理运行，本次实现不提前宣称运行成功。

## Alternatives considered

- 直接改生产 typedef：差异最短，也可能修正名称，但尚无 macOS 15 当前 provider 证据，且会改变 ABI 假设，所以先用不可部署的临时候选量测。
- 直接复用 Tahoe forwarder / 虚表：已有生成逻辑，能够回答 macOS 26 的问题，但那不是 macOS 15 vtable 证据，本探针只改四个类型声明。
- 复用 ARM 交叉编译或旧 unresolved manifest 作通过依据：无需等待 Intel runner，但无法观测当前 x86_64 provider，故保留现有跨编译契约不动，另设手动原生实验。

## Consequences

收益是用同提交、同工具链对照实验回答“七个候选符号名是否由当前声明库导出”，而不污染生产源码或发布候选。代价是额外一次正常和一次临时 accelerator 构建，并严格依赖可用的 Intel runner 与固定工具链；名称或既有未解析集合改变时必须重新审查探针，不能自动扩大允许范围。

即使探针成功，也只证明当前符号名与 provider 匹配，不证明 ABI、父类 vtable、KC 链接、签名加载、macOS 启动、DP、Metal、RADV 或 RX 7600 XT 实卡。候选不得部署至用户 U 盘或 production，生产修改仍须独立评审与验证。

新建时检索活跃 Note：与[GitHub CI 决定](2026-11-04-github-hosted-validation.md)及[固定原生交叉编译契约](2026-11-05-macos15-cross-native-contract.md)部分重叠，保留其已有验证边界，本篇只增加原生名称假设实验；命中的 Navi33 SMU / GFXHUB / UAPI 笔记不属于本流程决定，不改、不吸收、不归档。
