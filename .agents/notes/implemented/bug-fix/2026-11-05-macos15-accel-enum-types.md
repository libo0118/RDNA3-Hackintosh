# Agent Note: macOS 15 accelerator 四类型保留命名枚举身份

Status: implemented

## Problem

生产 `accel/iofam/iofam_types.h` 原先把 `IOAccelStereoMode`、`eDoSwap`、`eIOAccelSurfaceShapeBits`、`eLockType` 写成 `unsigned int` typedef，令七个继承方法的 C++ 符号名编码成 `j`。原生 macOS 15 构建虽有 914 imports，却只有 907 个得到 provider，不能靠 OpenCore 声称未导出的名称可绑定。

[已完成的隔离探针](../process/2026-11-05-macos15-accel-symbol-probe.md)在同一 b97c2812、同一 Intel / macOS 15.7.9 / Darwin 24.6.0 / Xcode 16.4 / SDK 15.5 上只修改这四条声明，得到恰好七个名称替换、实际 `IOAcceleratorFamily2` provider 和 914/914 strict guard；它回答了名称问题，不是完整 ABI 证明。

## Decision

生产头中的四条声明使用 `enum Name : unsigned int;`，保留命名类型身份与显式 unsigned underlying type，不猜枚举值，不改父类、vtable 声明、槽位或 Tahoe forwarder。四类型的调用面只包含已有 family 方法声明和生成的 Tahoe 同名参数；`gen_stubs.py` / `gen_tahoe_fwd.py` 复用现有签名，无需生成新槽位。

- `accel/Makefile` 的 `ACCEL_SRC_HASH` 与 `tools/triage_reports.py` 的 `ACCEL_HASH_FILES` 在相同位置加入本次实际改变的 `accel/iofam/iofam_types.h`。运行时 buildID 和报告比较由此包含枚举修正；不顺便重构未改动的所有 family headers。
- `tests/test_accel_hash_parity.py` 同时要求两份输入顺序一致和该生产头必须在列表内，避免两边一起遗漏。Makefile 路径使用 POSIX 规范化，使 Windows 不再仅因分隔符不同误报契约失败。
- `tests/test_accel_enum_types.py` 直接 include 真实生产头，仅临时 stub 两个未被声明使用的 Apple platform includes，以 C++17 static assertions 检查四类型均是 enum、underlying type 为 `unsigned int`，size / alignment 与其相同。它接入根 Makefile 的 `PY_TESTS`，不为测试增加生产 wrapper。
- 原生 `build.yml` 的 accelerator guard 移除 `--warn-unresolved`，任何 unresolved import 现在和其他三模块一样失败；保留四模块编译、检查、打包与已有日志路径。guard 只验证名称和声明 provider，不声称 ABI、加载或硬件通过。
- 一次性探针 workflow、工具与五项专用 selftests 从当前 HEAD 退役，regular host CI 不再调用它们；b97c2812 历史与已核验实验制品保留，详见上述探针笔记。

### Testing

针对性入口是 `python -I -B tests/test_accel_enum_types.py`、`python -I -B tests/test_accel_hash_parity.py` 与 `python -I -B tests/test_kmutil_kpi_guard.py`；完整 host 测试和修改后的原生 macOS 15 strict guard 由主代理汇合后执行 GitHub CI。本机 Windows 无 clang++ / g++ / make，不能把 Python 源码检查当 C++ 编译成功，也不能把历史临时候选当本次生产构建成功。

本次 Windows 实际通过 hash parity（21 文件）、既有 KPI guard / KPI versions、accel slots（11 cases）和 accel locks（后者使用 `-X utf8`，避免系统 GBK 解码失败）。三个修改过的 Python 文件通过 AST 检查；新测试的源码/临时 stub/编译参数自检确认 16 个 static assertions 且直接 include 生产头，但没有编译 C++。生产头 diff 与已核验实验 ZIP 内的 `candidate-header.patch` 完全一致；四模块 strict guard 与退役接线的源码断言、14 个 workflow `run` block 的 Git Bash `bash -n` 和三项 Note 门禁通过。直接运行新 C++ 测试因缺少 `clang++` 报 `FileNotFoundError: [WinError 2]`；实现阶段未完成 YAML 解析；根代理汇合后使用现有 PyYAML 实际通过生产 strict guard、Intel target、只读权限、探针退役与新测试接线断言，未安装新依赖。

## Alternatives considered

- 保持 typedef 并继续仅报告 unresolved：不改变生产类型最保守，但已被真实同提交对照实验定位的七个名称仍无法解析，不能作为修复。
- 为 macOS 15 搬入 Tahoe vtable forwarding：已有代码可避开直接命名，但它证明的是另一系统的槽位，不是本次 macOS 15 ABI；四声明修正已经有更直接的原生名称证据，故不扩展。
- 定义推测的枚举值或增加旧 typedef 兼容开关：可方便未来调用者，但本轮没有值语义或旧系统兼容证据，也无实际数值调用者，不虚构额外契约。

## Consequences

收益是正式生产构建能发出已在原生环境测得的七个命名枚举符号，buildID 能区分本轮修正，持续 CI 不再把 unresolved 当通过。代价是严格 guard 会如实阻断未来无法绑定的导入；测试只锁 enum 的类型/存储约束，并不证明 family 的完整调用约定或布局。

生产提交 `fce68dbc2cb932a75cf5fd989c22cac4900910ff` 的 [run 41896280472 / Intel job 126806539895](https://github.com/libo0118/RDNA3-Hackintosh/actions/runs/41896280472/job/126806539895) 已真实成功：core 484/484、accelerator 914/914、framebuffer 398/398、AGDC 351/351 均通过 strict 名称级 guard，host 为 153 passed / 0 failed / 6 skipped（159 tests）。[固定 cross 契约](../process/2026-11-05-macos15-cross-native-contract.md)据这次正式生产制品和 API digest 刷新，四模块统一名称级范围；七个新名称来自真实 Mach-O 捕获，不从历史候选或隔离探针偷偷加入 allowlist，ABI / vtable、KC、加载与硬件仍未证明。

新建前检索 proposed / implemented / rejected：探针笔记部分重叠并双向互链，保留其隔离选择、正面证据和限制；既有 GitHub CI 与 cross-contract 笔记部分重叠但由主代理汇合同步，本轮不改。Navi33 SMU / GFXHUB / UAPI 笔记只共享 macOS 15 验证边界，与本修正无关，保持不动；无须吸收、归档或废弃提案。

不改 EFI、PCI 7480 匹配、系统或原始 ROM。ABI / vtable、KC、签名加载、启动、DP、Metal、RADV 与 RX 7600 XT 实卡均未由本修正证明，不部署实验候选或把 CI 绿色宣称为显卡适配完成。
