# Agent Note: macOS 15 ARM 交叉编译只继承固定 Intel 名称级契约

Status: implemented

## Problem

GitHub `macos-15-intel` 曾因 hosted runner 未领取任务失败，新的 Intel 任务仍排队。用户需要继续验证 RX 7600 XT / Navi33 / macOS 15 x86_64 源码，但不能把更容易获得的 ARM 主机当成 x86_64 内核环境，或为通过编译删掉目标功能。原生 `kmutil_kpi_guard.py` 本来就是静态符号名与 provider 检查，不创建 KC，也不检查父类 vtable、二进制 ABI 或实际加载。

## Decision

独立的 `.github/workflows/macos15-cross.yml` 只接受 `workflow_dispatch`，不修改、替换或取消既有 `build.yml` 的 Intel 验证；既有 Ubuntu host 步骤另运行这组 Python selftests，使检查器自身也随正常 push 持续测试，不依赖手动交叉编译任务被触发。它使用标准 `macos-15` ARM runner，固定 `DEVELOPER_DIR=/Applications/Xcode_16.4.app/Contents/Developer`，要求 host 为 ARM/macOS 15、Xcode 16.4 build 16F6、SDK 精确为 15.5；条件不满足就失败，不自动改标签或 SDK。

官方 [runner 标签文档](https://docs.github.com/en/actions/reference/runners/github-hosted-runners) 将 `macos-15` 列为 arm64；[ARM 镜像软件表](https://github.com/actions/runner-images/blob/main/images/macos/macos-15-arm64-Readme.md) 在核验时为 image `20261028.0489.1`，明确列出 Xcode 16.4 / SDK 15.5。实际 workflow 再检查工具链，避免文档或镜像漂移被忽略。

- 四个 kext 显式使用 x86_64 / macOS 11.0 deployment / SDK 15.5，与原生构建参数相同；用户工具是 x86_64 / macOS 15.0 deployment。只复用不会检查 ARM 系统导出的 metaclass 检查；不运行 `kmutil`、`kextlibs` 或 AGDC 的 host export 报告。
- `macos15_native_contract.py` 必须将当前每个 bundle 的全部 undefined symbols 与同 bundle 固定基线比较，允许子集，任何新增 import 都硬失败。失败表示未被这个证据覆盖，不表示该符号一定不兼容；更新基线需要真正的 Intel 验证与明确审查，CI 不自动捕获新 allowlist。
- manifest 保留完整 Info 字典。身份、版本、全部 `OSBundleLibraries`、personalities、匹配、provider、`OSBundleRequired` 和其他元数据必须完全相同，包括值类型。仅 `accel` / `fb` / `agdc` 的 Makefile 明确插入的顶层 `RDNA3SourceHash` 是非绑定 buildstamp，允许其有效的 10 位十六进制值变化，并在日志记录基线值与当前值；不忽略任何其他字段，也不对 core 作此豁免。
- 同时从真实 `_kmod_info` 定义的文件映射读取名称、版本、info_version，逐项保持基线身份，不能只相信 plist。当前 undefined symbols 还必须重新通过现有 `load_xnu_lists()` / `xnu_verdicts()` 的最新仓库导出策略；旧基线不能绕过以后更新的清单。
- core / fb / agdc 只称“固定原生名称级 guard 契约未扩大”；accelerator 只称“编译和 import 契约未扩大，已有未解析导入仍未验证”，绝不称全量绑定通过。
- 只使用 `contents: read`。基线已离线捕获并提交，CI 不需要 `actions: read` 或 GitHub token 下载旧制品。沿用既有固定 SHA 的 checkout / upload-artifact，制品名称明确标记 `CROSSCOMPILED-NOT-HARDWARE-VERIFIED`，附日志、许可证、来源与 libdrm 的原文许可材料；不下载或打包 GPU firmware，不安装或加载 kext。

### 不可变原生基线

来源是 [run 41824849834 / job 126556846479](https://github.com/libo0118/RDNA3-Hackintosh/actions/runs/41824849834/job/126556846479)，提交 `4712d9141572e2d5d88ea57e13d32e51bfc9408a`。GitHub API 的 run、Intel job 和对应 guard steps 均为成功，真实日志为 macOS `15.7.9` / build `24G830`、Xcode `16.4` / `16F6`、SDK `15.5`。

| 真实制品 | artifact ID | GitHub API digest（下载后逐字节核对） |
| --- | --- | --- |
| `experimental-macos15-x86_64-NOT-HARDWARE-VERIFIED` | `13318597464` | `sha256:f3d6d0fd8b79e47c75608ab16d0b738ef9643bf890dc920834ca8ecbfb0b38e4` |
| `experimental-macos15-compile-logs` | `13318682317` | `sha256:9d1d586b636e2a3b59a748c8cea8d5542784c855ff2360bc513c8b0441993db2` |

`capture` 只接受上述固定 run / job / commit / artifact ID / digest。捕获前检查每个 ZIP 成员的 CRC、原始及标准化路径、重复项和链接；TAR 同样检查路径和成员类型，不向工作区直接解压。manifest 额外保留 TAR、原 job 日志、每个 executable / Info 的 SHA256，完整导入集合、Info 和实际 Mach-O 身份，不手写“预期都通过”。

Windows 无 `nm`，捕获器使用 bounds-checked stdlib Mach-O64 parser，读取 `LC_SYMTAB` 中非 STAB、`N_UNDF`、`n_value == 0` 的符号。core 的完整集合与原 `kext.log` 的真实 `nm -u` 输出完全相同；四个数量均与同一 job 的 guard 日志交叉核验。Mac 新构建还强制要求 `xcrun nm -arch x86_64 -u` 与 parser 的集合相同。

| bundle | 实际 parser / 原 native import 数 | 原 kmutil resolved 数 | 可继承范围 |
| --- | --- | --- | --- |
| core | 484 | 484 | 严格名称级 guard |
| framebuffer | 398 | 398 | 严格名称级 guard |
| AGDC | 351 | 351 | 严格名称级 guard |
| accelerator | 914 | 907 | 编译和报告；7 个 unresolved，完整名单保存在 manifest |

原 core `kextlibs` 曾找不到全部 484 个符号，原脚本明确未将它计作验证；严格结果来自后续真实 `kmutil` guard，不能把这个跳过改称成功。原 accelerator 的实际 `kmod_info.version` 为 `0.1`，Info 的 `CFBundleVersion` 为 `0.1.0`；本工具如实保留这两个既有值，不改 driver 或凭格式相近宣称二者相同。

四个基线 `MH_KEXT_BUNDLE` 均没有 `LC_BUILD_VERSION` / `LC_VERSION_MIN_MACOSX`。因此不能声称从 kext 读到了 SDK 15.5；它们保持相同版本命令状态，并由实际 SDK 断言与显式 `SDK` 编译参数约束。五个用户工具则必须在实际 Mach-O 版本命令中记录 macOS 15.0 deployment / SDK 15.5。

### Testing

- Windows 已实际执行 `python -I -B tools/ci/test_macos15_native_contract.py`，17 个测试通过，覆盖截断 / bad load command / 字符串边界、架构 / SDK 目标、实际 kmod 身份、子集、新 import、完整 Info 类型与内容变化、限定 SourceHash 豁免、accelerator 范围、最新 XNU 策略、nm 不一致，以及 artifact digest / CRC / 路径 / symlink 拒绝。
- 对真实下载证据重新运行 `capture` 得到逐字节相同的 manifest；SHA256 为 `52abe8bccc5c3c3f126090dfc68d71d73359038885d0cbdb12bff77f805e9f3b`。证据目录的 `run.json` / `job.json` / `artifacts.json` 分别来自对应 GitHub API，`native-job.log` 来自 job logs API，两个 ZIP 来自固定 artifact archive API。认证只在进程内获取，对离开 `api.github.com` 的 HTTPS 重定向移除 Authorization，令牌不落盘、不打印。
- 对经摘要和路径核验后置于独占临时目录的原生四个 kext 与五个用户工具运行 `python -B tools/ci/macos15_native_contract.py check <baseline-build>`，全部通过。这是旧二进制的离线交叉核验，不是 Windows 编译，也没有在 Windows 执行 Mac `nm`。
- 既有 `tests/test_kmutil_kpi_guard.py` / `tests/test_kext_kpi_versions.py` 均通过；workflow 经已安装 PyYAML 解析，手动触发、只读权限、runner 和 action SHA 断言通过，6 个 `run` block 全部通过 Git Bash `bash -n`。三项 Note 门禁均实际通过。
- 复现捕获：准备上述六份 API / artifact 文件后执行 `python3 tools/ci/macos15_native_contract.py capture <evidence-dir> tools/ci/data/macos15-native-contract.json`。新 workflow 执行 selftests、真实交叉编译和 `check build --nm`；本地没有 clang / make / Xcode，不提前声称这个新 workflow 已编译成功。

## Alternatives considered

- 直接把现有 Intel job 改成 ARM：最少配置、容易获得 runner，但会暗中替换原验证目标，故只增加独立手动任务，保留 Intel。
- 在 ARM 上继续调用 `kmutil` / AGDC host exports：最接近原命令，但 provider 是 ARM 系统环境，不能当 x86_64 绑定证据，故使用固定 Intel 证据的名称级契约，遇到新增 import 就停止。
- 只锁 import，不锁 Info / binary kmod 身份：manifest 更短，但库版本、匹配或模块身份变化也会影响加载，不能靠同名符号证明安全，故保留完整元数据与 actual identity。生成的 SourceHash 不属于绑定元数据，只作明确且有日志的唯一豁免。

## Consequences

收益是在原生 runner 不可用时仍获得真实的 x86_64 编译证据，并以严格增量门槛阻止未验证的导入和绑定元数据变化。代价是固定契约保守拒绝可能本来可用的新 import，也不能证明当前 Mac 上的实际 provider、vtable / ABI、KC 创建、签名加载或 RX 7600 XT 硬件正确；新依赖必须回到真正 Intel 目标验证，不能自动放宽。

本篇与[既有 GitHub CI 决定](2026-11-04-github-hosted-validation.md)部分重叠，只增加补充手动证据，其 native / accelerator / 硬件边界仍保留。新建时检索所有活跃 Note；固件基础、SMU 状态、SMU PM 和 PSP ready 笔记只引用相同 CI 边界，与本篇流程决定无冲突，保持不动，无需吸收、归档或拒绝旧提案。

[隔离枚举符号探针](2026-11-05-macos15-accel-symbol-probe.md) 另用真实 Intel 主机量测七个未解析方法的名称假设；它不修改这里的基线或扩大固定导入契约，也不把名称匹配当 ABI 或硬件完成。

[生产枚举修复](../bug-fix/2026-11-05-macos15-accel-enum-types.md)使七个名称变更超出当前固定基线；在取得真实生产 native strict 证据前，本契约保持拒绝新增导入，不从隔离探针自动扩展允许集合。后续刷新须固定实际生产 run、job、制品与摘要。
