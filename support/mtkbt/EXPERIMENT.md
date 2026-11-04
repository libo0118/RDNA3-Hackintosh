# MtkBt 2c7c:7009 受保护实验候选

**不是已验证驱动或自动安装包。** 此产物只证明对应 CI 执行了 host 策略测试、编译与离线结构检查；不能证明本机 USB / ROM 相符、macOS 15 能启用蓝牙、配对、音频或睡眠唤醒。签名为 ad-hoc，不是 Apple 开发者签名或公证，也不改变 SIP、AMFI 或其他系统保护。

## 固定来源与改动

- 作者源码：[WTLMtkBluetooth 0.2.4](https://github.com/watchthelight-labs/WTLMtkBluetooth/tree/377ff232be3ec62ecd6548878edf769226a398cb)，commit `377ff232be3ec62ecd6548878edf769226a398cb`，tree `1bb07cd42ca4f64e46d9c3aef43e6f035c37f562`。
- 固定 [codeload tar.gz](https://codeload.github.com/watchthelight-labs/WTLMtkBluetooth/tar.gz/377ff232be3ec62ecd6548878edf769226a398cb) SHA-256：`3254bc7f5b964201aa7b253bb06a209af970eac2da5a59c67e99b727190749e4`。哈希不一致即停止，不跟随 tag / main 更新。
- 同目录 `guarded-7009.patch` 原样应用到该基线：只新增真实 `idVendor=0x2c7c`、`idProduct=0x7009`、configuration 1、interface 0 的 personality；不伪装成其他 USB ID。
- 芯片仍必须为 MT7925；FW version 读取成功且低字节为零，才可使用唯一内置 `_1_1`；FW flavor 读取失败也停止。不修改固件内容、已知寄存器值或上游下载 / reset 序列。
- 缺少实际 interrupt IN / bulk IN / bulk OUT、interrupt pipe / buffer 或首次 interrupt read 提交失败时拒绝 session，post-open 失败会关闭接口并释放 pipe。未通过身份门禁时也不进入 shutdown FUNC_CTRL 0。

[Linux 上游精确 ID 提交](https://github.com/torvalds/linux/commit/7ed1d46c6bc2848469f8b0cefc43eef2c5d23536) 是身份依据，不是本机端点实测；同 ID 样本不能代替本机 ROM 数据。端点地址来自当前接口描述符，不硬编码为样本的 `81/82/02`。

## CI 与离线复现

独立 `.github/workflows/mtkbt.yml` **仅 `workflow_dispatch`，无 inputs**；不会因 push / PR 自动执行，也不修改 GPU workflow。需在已含该 workflow 的分支上手动运行，例如：

```sh
gh workflow run mtkbt.yml --ref <reviewed-branch>
```

Runner：GitHub hosted `macos-15-intel`，固定 `DEVELOPER_DIR=/Applications/Xcode_16.4.app/Contents/Developer` 且校验 `Xcode 16.4`，20 分钟上限，只授 `contents: read`。checkout 与 upload-artifact 均固定完整官方 commit SHA，不使用 secrets 或 self-hosted runner。源码、host executable 和构建输出都在 `RUNNER_TEMP`；不运行安装器、不安装、不加载 kext、不访问本机设备。

对固定源码，在应用 patch 前保留原始 `Info.plist`，然后：

```sh
git apply --check /path/to/guarded-7009.patch
git apply /path/to/guarded-7009.patch
clang++ -std=c++17 -Wall -Wextra -Werror -pedantic tests/startup-policy.cpp -o /tmp/mtkbt-startup-policy
/tmp/mtkbt-startup-policy
python3 -I tests/check-plist.py /path/to/original-Info.plist Info.plist
bash tools/build-mac.sh
```

上游 build 脚本经审阅，只编译且会删除源码目录内 `out/MtkBt.kext`；仅在隔离临时源码使用，不指向 EFI 或系统目录。产物携带 `LICENSE`、`NOTICE.md`、完整 `LICENSES/`、补丁、此范围说明、构建 / host 测试记录与哈希。离线检查包括 ad-hoc 签名验证、仅 `x86_64`、Mach-O `KEXTBUNDLE`、无动态 dylib 依赖 / 用户态 C++ runtime 依赖，以及原有匹配项和 KPI 依赖的完整比较。

host 测试直接调用内核共享的边界谓词，以模拟下载计数验证拒绝条件；不模拟 IOKit USB 传输或并发清理。CI 没有本机设备，不能给出硬件安全或可用性结论。

## Sequoia 15 依赖与后续注入边界

以下只记录作者 / 官方要求，**本 workflow 不下载、安装或注入这些依赖，也不写 NVRAM**：

1. `MtkBt.kext` 本体只依赖 `com.apple.iokit.IOUSBHostFamily=1.2`、`com.apple.kpi.iokit=14.0`、`com.apple.kpi.libkern=14.0`、`com.apple.kpi.mach=14.0`，**不依赖 Lilu**。
2. 作者要求后续由 `bluetoothd` 接管时另配 **BlueToolFixup + Lilu**。2026-11-04 官方最新正式发布是 [BrcmPatchRAM / BlueToolFixup 2.7.2](https://github.com/acidanthera/BrcmPatchRAM/releases/tag/2.7.2) 与 [Lilu 1.7.2](https://github.com/acidanthera/Lilu/releases/tag/1.7.2)。[BlueToolFixup 官方 README](https://github.com/acidanthera/BrcmPatchRAM/blob/aef376bb1e7e27cabc61da7edfb1a88fac7f7acc/README.md#bluetoolfixupkext) 写 Lilu **1.5.4+**；其 plist 的链接声明是 `as.vit9696.Lilu=1.4.7`，不是建议把运行依赖降到 1.4.7。不要为本候选增加 Broadcom 固件包或 `BrcmBluetoothInjector`。
3. [作者安装说明](https://github.com/watchthelight-labs/WTLMtkBluetooth/blob/db331dfb48b06328a5642d87b95a711632e6e691/README.md#installation) 与 [BlueToolFixup 2.7.0 changelog](https://github.com/acidanthera/BrcmPatchRAM/blob/aef376bb1e7e27cabc61da7edfb1a88fac7f7acc/Changelog.md) 要求在 OpenCore `NVRAM > Add` / `Delete` 下的公共 namespace `7C436110-AB2A-4BBB-A880-FE41995C9F82` 设置：

   | 键 | Add 类型和值 | Delete |
   |---|---|---|
   | `bluetoothExternalDongleFailed` | Data：`00`，1 个零字节 | 同名字符串 |
   | `bluetoothInternalControllerInfo` | Data：`0000000000000000000000000000`，**14 个零字节** | 同名字符串 |

   不用 16 字节替代 14 字节，不用 boot-arg / 保护降级绕开配置。两个键会影响整个蓝牙栈；后续如获准修改，须先备份原配置和值并提供回滚。
4. OpenCore 加载次序为 `Lilu.kext` → `BlueToolFixup.kext` → `MtkBt.kext`；MtkBt 的 `ExecutablePath=Contents/MacOS/MtkBt`、`PlistPath=Contents/Info.plist`。作者示例 `MinKernel=21.0.0` 不是本机支持证明；本目标仅 Sequoia 15，整合者应按实际系统范围限制，不扩大为其他 macOS 已验证。
5. 作者单列的 `WTLLegionBtFix` 针对 **Tahoe 26**，不作为 Sequoia 15 依赖。不要增加 Tahoe-only 补丁、不放宽本候选保护、不照抄其他机器的 USB 端口或 `_UPC` 补丁。

CI + 独立源码审阅通过以后，仍由整合者决定是否放入可回滚的实验启动介质。届时需核验本机 USB 映射 / configuration / endpoints、芯片 / FW 寄存器及 macOS 冷启动、重启、配对。错误应从 `MtkBtLastError` / `MtkBtLog` 读取；`ready` 仅是 loader 自报，不等于配对成功。未知 ROM 被拒绝是预期结果，不是理由去关掉门禁。
