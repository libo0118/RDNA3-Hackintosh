# Agent Note: macOS 15 ARM 交叉编译只继承固定 Intel 名称级契约

Status: implemented

## Problem

GitHub `macos-15-intel` 曾因 hosted runner 未领取任务失败，补充 cross 流程因此保留；现在生产枚举修正已取得真实 Intel 成功证据，固定契约需要随之刷新，而不是继续继承七个未解析名称。用户需要继续验证 RX 7600 XT / Navi33 / macOS 15 x86_64 源码，但不能把更容易获得的 ARM 主机当成 x86_64 内核环境，或为通过编译删掉目标功能。原生 `kmutil_kpi_guard.py` 本来就是静态符号名与 provider 检查，不创建 KC，也不检查父类 vtable、二进制 ABI 或实际加载。

## Decision

独立的 `.github/workflows/macos15-cross.yml` 只接受 `workflow_dispatch`，不修改、替换或取消既有 `build.yml` 的 Intel 验证；既有 Ubuntu host 步骤另运行这组 Python selftests，使检查器自身也随正常 push 持续测试，不依赖手动交叉编译任务被触发。它使用标准 `macos-15` ARM runner，固定 `DEVELOPER_DIR=/Applications/Xcode_16.4.app/Contents/Developer`，要求 host 为 ARM/macOS 15、Xcode 16.4 build 16F6、SDK 精确为 15.5；条件不满足就失败，不自动改标签或 SDK。

官方 [runner 标签文档](https://docs.github.com/en/actions/reference/runners/github-hosted-runners) 将 `macos-15` 列为 arm64；[ARM 镜像软件表](https://github.com/actions/runner-images/blob/main/images/macos/macos-15-arm64-Readme.md) 在核验时为 image `20261028.0489.1`，明确列出 Xcode 16.4 / SDK 15.5。实际 workflow 再检查工具链，避免文档或镜像漂移被忽略。

- 四个 kext 显式使用 x86_64 / macOS 11.0 deployment / SDK 15.5，与原生构建参数相同；用户工具是 x86_64 / macOS 15.0 deployment。只复用不会检查 ARM 系统导出的 metaclass 检查；不运行 `kmutil`、`kextlibs` 或 AGDC 的 host export 报告。
- `macos15_native_contract.py` 必须将当前每个 bundle 的全部 undefined symbols 与同 bundle 固定基线比较，允许子集，任何新增 import 都硬失败。失败表示未被这个证据覆盖，不表示该符号一定不兼容；更新基线需要真正的 Intel 验证与明确审查，CI 不自动捕获新 allowlist。
- manifest 保留完整 Info 字典。身份、版本、全部 `OSBundleLibraries`、personalities、匹配、provider、`OSBundleRequired` 和其他元数据必须完全相同，包括值类型。仅 `accel` / `fb` / `agdc` 的 Makefile 明确插入的顶层 `RDNA3SourceHash` 是非绑定 buildstamp，允许其有效的 10 位十六进制值变化，并在日志记录基线值与当前值；不忽略任何其他字段，也不对 core 作此豁免。
- 同时从真实 `_kmod_info` 定义的文件映射读取名称、版本、info_version，逐项保持基线身份，不能只相信 plist。当前 undefined symbols 还必须重新通过现有 `load_xnu_lists()` / `xnu_verdicts()` 的最新仓库导出策略；旧基线不能绕过以后更新的清单。
- 四个 bundle 的基线一律要求 imports 等于 resolved、无 WARN 或 unresolved，scope 均为 `native-name-level-guard`。新构建只称“固定原生名称级 guard 契约未扩大”，绝不称当前系统全量绑定、ABI 或硬件通过；不保留 accelerator 的旧报告特判或基线兼容路径。
- 只使用 `contents: read`。基线已离线捕获并提交，CI 不需要 `actions: read` 或 GitHub token 下载旧制品。沿用既有固定 SHA 的 checkout / upload-artifact，制品名称明确标记 `CROSSCOMPILED-NOT-HARDWARE-VERIFIED`，附日志、许可证、来源与 libdrm 的原文许可材料；不下载或打包 GPU firmware，不安装或加载 kext。

### 不可变原生基线

来源是正式生产 [run 41896280472 / job 126806539895](https://github.com/libo0118/RDNA3-Hackintosh/actions/runs/41896280472/job/126806539895)，提交 `fce68dbc2cb932a75cf5fd989c22cac4900910ff`，不是隔离候选。GitHub API 的 run、Intel job 和对应 guard steps 均为成功，真实日志为 macOS `15.7.9` / build `24G830`、Xcode `16.4` / `16F6`、SDK `15.5`。

| 真实制品 | artifact ID | GitHub API digest（下载后逐字节核对） |
| --- | --- | --- |
| `experimental-macos15-x86_64-NOT-HARDWARE-VERIFIED` | `13356645862` | `sha256:683cea32997d54fd3aced5cab577def0e91c97039cc64d7d4f0b41bd99060d36` |
| `experimental-macos15-compile-logs` | `13354609881` | `sha256:5ffd537ee5e6c5edb48902b5226f1bb31f17e13c6860b6393a0ea09bfde5376d` |

`capture` 只接受上述固定 run / job / commit / artifact ID / digest。成功步骤必须包含真实 job 的 `Require macOS 15 Intel and the macOS 15 SDK`、`Strict KPI guards (core, framebuffer and AGDC)` 和 `Strict accelerator KPI guard (names and providers only)`；对应 ZIP 仍使用 `kpi-strict.log` / `kpi-accelerator-report.log`，日志路径不代表放宽 accelerator。捕获前检查每个 ZIP 成员的 CRC、原始及标准化路径、重复项和链接；TAR 同样检查路径和成员类型，不向工作区直接解压。manifest 额外保留 TAR、原 job 日志、每个 executable / Info 的 SHA256，完整导入集合、Info 和实际 Mach-O 身份，不手写 symbol list 或“预期都通过”。

Windows 无 `nm`，捕获器使用 bounds-checked stdlib Mach-O64 parser，读取 `LC_SYMTAB` 中非 STAB、`N_UNDF`、`n_value == 0` 的符号。core 的完整集合与原 `kext.log` 的真实 `nm -u` 输出完全相同；四个数量均与同一 job 的 guard 日志交叉核验。Mac 新构建还强制要求 `xcrun nm -arch x86_64 -u` 与 parser 的集合相同。

| bundle | 实际 parser / 原 native import 数 | 原 kmutil resolved 数 | 可继承范围 |
| --- | --- | --- | --- |
| core | 484 | 484 | 严格名称级 guard |
| framebuffer | 398 | 398 | 严格名称级 guard |
| AGDC | 351 | 351 | 严格名称级 guard |
| accelerator | 914 | 914 | 严格名称级 guard |

本次 core `kextlibs` 仍找不到全部 484 个符号，原脚本明确未将它计作验证；严格结果来自后续真实 `kmutil` guard，不能把这个跳过改称成功。本次真实 accelerator 的 `kmod_info.version` 仍为 `0.1`，Info 的 `CFBundleVersion` 为 `0.1.0`；本工具如实捕获这两个既有值，不改 driver 或凭格式相近宣称二者相同。

历史差异来源保留为 [旧 run 41824849834 / job 126556846479](https://github.com/libo0118/RDNA3-Hackintosh/actions/runs/41824849834/job/126556846479)，提交 `4712d9141572e2d5d88ea57e13d32e51bfc9408a`；旧 manifest SHA256 为 `52abe8bccc5c3c3f126090dfc68d71d73359038885d0cbdb12bff77f805e9f3b`，accelerator 为 914 imports / 907 resolved，七个 unresolved 使用 typedef 编码的名称。新旧真实集合比较恰好删除七个旧名称、加入七个命名枚举名称，其他三个 bundle 的 import 集合不变；除已限定的 accelerator SourceHash 从 `3fd305652c` 变为 `60fcb24544` 外，完整 Info 契约及实际 Mach-O / kmod 身份均不变。差异依据是[生产枚举修复](../bug-fix/2026-11-05-macos15-accel-enum-types.md)后的真实制品，不从隔离探针复制允许集合。

四个基线 `MH_KEXT_BUNDLE` 均没有 `LC_BUILD_VERSION` / `LC_VERSION_MIN_MACOSX`。因此不能声称从 kext 读到了 SDK 15.5；它们保持相同版本命令状态，并由实际 SDK 断言与显式 `SDK` 编译参数约束。五个用户工具则必须在实际 Mach-O 版本命令中记录 macOS 15.0 deployment / SDK 15.5。

### Testing

- 生产 run 的 host 日志为 `153 passed, 0 failed, 6 skipped, 159 tests`，另有当时的 17 项 contract selftests；这证明 `fce68dbc` 的生产源码，不代替刷新后的 cross 执行。
- Windows 已实际执行 `python -I -B tools/ci/test_macos15_native_contract.py`，20 个测试通过，保留截断 / bad load command / 字符串边界、架构 / SDK 目标、实际 kmod 身份、子集、新 import、完整 Info 类型与内容变化、限定 SourceHash 豁免、最新 XNU 策略、nm 不一致，以及 artifact digest / CRC / 路径 / symlink 拒绝。新增实际 `capture` 路径的无警告全解析、数量不符、错误 provenance、缺失或失败 strict step 拒绝；四模块均拒绝旧报告 scope、未完全 resolved 或非空 unresolved。
- 从保存的真实六份 API / artifact 输入生成 manifest，再独立重复两次 `capture`，三者逐字节相同；SHA256 为 `867fa9715caae05f37a43f59cb83407322dd36c1da5a5e02bbf25f4be92d7261`。`run.json` / `job.json` / `artifacts.json`、`native-job.log` 和两个 ZIP 的原始证据保持不动；本轮仅离线读取，重新核对固定 artifact digest，不进行认证或网络下载。
- 对经摘要和路径核验后置于独占临时目录的真实生产四个 kext 与五个用户工具运行 `python -B tools/ci/macos15_native_contract.py check <baseline-build>`，全部通过；重复 capture、check 日志及验证摘要保留。这是原生制品的离线交叉核验，不是 Windows 编译，也没有在 Windows 执行 Mac `nm`。
- 两个修改的 Python 文件通过 AST 检查，既有 `tests/test_kmutil_kpi_guard.py` / `tests/test_kext_kpi_versions.py` 均通过；workflow 经已安装的用户 site PyYAML 解析（`-I` 会屏蔽它），手动触发、只读权限、runner、action SHA 和未调用 ARM KPI 解析断言通过，6 个 `run` block 全部通过 Git Bash `bash -n`。三项 Note 门禁均实际通过。
- 复现捕获：准备上述六份 API / artifact 文件后执行 `python3 tools/ci/macos15_native_contract.py capture <evidence-dir> tools/ci/data/macos15-native-contract.json`。workflow 继续执行 selftests、真实交叉编译和 `check build --nm`；刷新后的版本需由主代理提交后实际 dispatch，本地没有 Xcode，不提前声称这个版本已交叉编译成功。

## Alternatives considered

- 直接把现有 Intel job 改成 ARM：最少配置、容易获得 runner，但会暗中替换原验证目标，故只增加独立手动任务，保留 Intel。
- 在 ARM 上继续调用 `kmutil` / AGDC host exports：最接近原命令，但 provider 是 ARM 系统环境，不能当 x86_64 绑定证据，故使用固定 Intel 证据的名称级契约，遇到新增 import 就停止。
- 只锁 import，不锁 Info / binary kmod 身份：manifest 更短，但库版本、匹配或模块身份变化也会影响加载，不能靠同名符号证明安全，故保留完整元数据与 actual identity。生成的 SourceHash 不属于绑定元数据，只作明确且有日志的唯一豁免。

## Consequences

收益是在原生 runner 不可用时仍获得真实的 x86_64 编译证据，并以严格增量门槛阻止未验证的导入和绑定元数据变化。代价是固定契约保守拒绝可能本来可用的新 import，也不能证明当前 Mac 上的实际 provider、vtable / ABI、KC 创建、签名加载或 RX 7600 XT 硬件正确；新依赖必须回到真正 Intel 目标验证，不能自动放宽。

本篇与[既有 GitHub CI 决定](2026-11-04-github-hosted-validation.md)部分重叠，只增加补充手动证据，其 native / accelerator / 硬件边界仍保留。新建时检索所有活跃 Note；固件基础、SMU 状态、SMU PM 和 PSP ready 笔记只引用相同 CI 边界，与本篇流程决定无冲突，保持不动，无需吸收、归档或拒绝旧提案。

[隔离枚举符号探针](2026-11-05-macos15-accel-symbol-probe.md) 保留早期在真实 Intel 主机量测七个未解析方法名称假设的依据，其一次性入口已退役；它不是当前生产基线，也不把名称匹配当 ABI 或硬件完成。

[生产枚举修复](../bug-fix/2026-11-05-macos15-accel-enum-types.md)的 `fce68dbc` 已取得上述真实生产 native strict 证据；本篇按相同固定证据流程刷新唯一基线，不改变补充 cross 决定，不增加多版本框架。以后的新增 import 仍须真实生产 Intel 验证与明确审查，不能从候选、探针或摘要自动扩展。
