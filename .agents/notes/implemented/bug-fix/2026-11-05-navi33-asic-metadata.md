# Agent Note: ASIC 元数据来自实际 VBIOS / discovery，不受 exact 硬件控制开关隔离

Status: implemented

## Problem

原 `RDNA3Bringup::pmInit` 读取 VBIOS 成功后，xclk、显存类型和位宽的解析 / 发布整个处于 `fAmdgpuExact` 内；默认 `rdna3amdgpu=0` 因而永远保留 UAPI 的 GDDR6 / 256 bit 假值。MALL discovery 的发布又同时依赖 level 3、GC harvesting 和 exact，未读取时仍声称 Navi32 的 64 MiB。仅移出 exact 还不够：scratch 地址为零而 boot parser 成功时会误选旧表 module 0；未知 xclk 会被 Mesa 当成错误时间基准；异步 PM 晚于 enum / 用户态初次枚举，`rdna3pm=0` 更永远不提供该数据。

已有本地只读核对记录指出，与 live BDF / VID / DID 匹配的 VBIOS 缓存含 VRAM_INFO 3.0（`0x20a0`、4238 字节），`memory_type=0x70`、`channel_num=8`、`channel_width=4`、enable `0xff`。现有生产 parser 与 Linux 的 3.0 布局一致，位宽是 `8 * 16 = 128 bit`，不是 Navi32 的 256 bit。本次不重新读取、上传或提交原始 ROM；fixture 只含人工合成数据。

两个模块的 `dram_size_per_ch=1024` 没有已确认的容量单位，本次不据此推导 16 GB。Windows dedicated 8176 MB 也不证明物理显存容量；DEV_INFO 元数据与 `MEM_INFO` 的容量来源不能混用。

## Decision

- 初始 xclk、VRAM type / width、MALL 使用 ABI 已有的 UNKNOWN / 0，表示尚未取得，不表示物理容量为 0 或没有 cache；删除生产全局 Navi32 常量。不改 wire layout、版本、boot-arg、PCI link lane 或其他未核对字段。
- `RDNA3Accel::drmPublishVbiosMetadata` 是早期初始化与 PM 刷新的共享只读发布入口，不依赖 exact 或 PM。`displayInit` 已读取的 `fDispVbios` / `fDispDev` 在 `drmInit` 同步解析发布，先于 enum ready、`platformInit` 和异步 bring-up；`pmInit` 后续读到有效 VBIOS 仍复用同一入口，不改变完整 GPU / 电源异步顺序。display 缓存对象只要分配成功就先清零，再判断其余分配失败，避免新早期消费者读到未初始化指针。
- module 只在已验证布局 / 长度的 FirmwareInfo 给出非零 scratch 地址、且实际读取 BIOS scratch 4 后才算取得；boot parser 成功不能证明选择成功。无地址 / 无 register view 时使用 `UINT32_MAX` 表示不可选，旧表不调用 parser，不猜 module 0；VRAM_INFO 3.0 的全局 type / width 仍独立解析。boot parser 只供 exact 下原有 memory-training / dynamic-boot-config 诊断，不新增训练、GECC / RAS 或其他控制。
- 读取失败不更新 DEV_INFO；clock / VRAM parser 失败或缺少有效字段也不把其输出作为新数据。内部 `rdna3_drm_device_set_asic_info` 保持 0 = 保留既有值，因此 MALL-only 更新不清空已知 VRAM。重置 / resume 的失败显式记录 `RDNA3,VBIOSMetadata=unavailable...` 或 `incomplete...`，日志说明保留的是 prior / unknown，不冒充本次成功。有效本次读取标为 `current`。
- 实际 discovery MALL 在 `drmInit` 直接发布，不依赖 level、寄存器 view、GC harvesting 或 exact。未知 / 零 size 的日志明确写出 unknown，不猜本卡 32 MiB。TCC 的原 level / GC / exact MMIO 门槛不变。
- 在现有 `AMDGPU_INFO_DEV_INFO` 输出边界拒绝缺失 xclk / type / width，及 GC 11.0.3、零 MALL、`num_shader_engines != 1` 的组合，返回既有 `-RD_ENODEV` 且不写输出。GC 11.0.2 的零 MALL 仍合法，GC 11.0.3 单 SE 的合法零也不收窄；不向 setter、file-open 或其他查询叠加守卫，不用 enumeration-only 绕过边界。
- `rdna3_drm_device_has_asic_info` 统一 DEV_INFO 与 enum 的同一完整性条件。`rdna3enum=1` 在早期数据不完整时明确记录 `refused: incomplete ASIC metadata`，不设置合成 `accel_working` / rings；后续 metadata 成功不会自动补发 enum ready。默认非 enum 的真实 ring-ready 策略不变。
- loopback 显式用真实 setter 构造其合成 Navi32 GDDR6 / 256 bit / 64 MiB / 100 MHz fixture，不再依赖生产默认值。revision 用例同样提供明确的合成 metadata，保持测试责任互相独立。

### 消费者与一手依据

- 固定 Linux v6.12 [`amdgpu_atomfirmware_get_vram_info`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/amdgpu_atomfirmware.c#L294-L528) 的 VRAM_INFO 3.0 使用 `channel_num * 16`；[`soc21_get_xclk`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/soc21.c#L232-L235) 返回实际 SPLL reference。
- 仓库 `third_party/amd/amdgpu_drm.h` 定义 `AMDGPU_VRAM_TYPE_UNKNOWN=0`；vendored libdrm `amdgpu_gpu_info.c` 原样复制 type / width / xclk，不替生产值兜底。
- 只读核对 `userspace/mesa/build_radv.sh` 固定的 [Mesa `ac_gpu_info.c`](https://github.com/chaotic-cx/mesa-mirror/blob/d381c361aec812e9d772291dd1da9b8e1b234a74/src/amd/common/ac_gpu_info.c)：575–576 复制 type / width，2599–2623 的 UNKNOWN 返回 0 ops/clock，1659 将零 width 转成零带宽。因此未知不能当作已识别显存继续发布。603–622 对 Navi33 已有 firmware MALL=0 的官方推算路径；其他 GFX11 直接采用传入 MALL，1737–1740 的零 cache 分支断言单 SE。只拒绝已确认不能消费的组合，不修改 Mesa、不伪造 cache、不改变 MEM_INFO。
- 同一固定版本 `ac_gpu_info.c:1228–1231` 对零 `gpu_counter_freq` 警告 timestamps 错误并替成 1 kHz；[`radv_physical_device.c:1912`](https://github.com/chaotic-cx/mesa-mirror/blob/d381c361aec812e9d772291dd1da9b8e1b234a74/src/amd/vulkan/radv_physical_device.c#L1912) 再据此计算 `timestampPeriod`。因此不能仅有 VRAM 就放行未知 xclk。
- `metal/plugin/nvmtl_vk.c:853,927–933,1221–1225` 在初始化前置 `g_state=-1`，枚举失败返回，且 `pthread_once` 同进程只执行一次。后续 PM 补齐不会让 Metal 自行重试；本次只消除 metadata / enum 的早期窗口，不改 Metal / Mesa 或声称所有启动时序已解决。

### Testing

`tests/test_uapi.c` 经真实 `rdna3_drm_ioctl` 检查初始未知 / 不完整数据拒绝及完整输出缓冲区不变；128 / 256 bit、GDDR6 / HBM 更新，MALL-only 与全零调用保留已知 VRAM；两个 GC 的合法零 / 缺 xclk / 缺 VRAM / 多 SE 缺 MALL 边界，以及 enumeration-only 不能伪装完整 metadata。原 revision 与 shadow / CSA 回归保留。`tests/test_libdrm_amdgpu.c` 经真实 libdrm 核对 loopback 明确给出的合成数据；simulator 两处既有 DRM fixture 同样明确提供 100000 kHz，不靠生产默认值。

`tests/test_asic_metadata.py` 提取完整生产共享发布方法、真实 VBIOS clock / VRAM / table parser、完整 `RDNA3Accel::start`、包含同步发布与完整 enum guard 的 DRM 初始化段、PM 读取块与真实 `fPmMode` guard，并链接完整生产 UAPI / common C。仅 OS / MMIO / 读取 / boot 诊断输入替身化，不为测试新增生产 wrapper。合成四-module 旧表在 scratch=0、boot=OK 时不能发布 module 0；3.0 无 scratch 仍能发布。clock 失败 / reference=0 配有效 VRAM 经过真实 DEV_INFO 查询拒绝且输出不变；后续有效数据、旧 known 缓存、两种 exact / PM / enum 的首个消费者、失败不假 ready、MALL 与 TCC / RAS 门槛均有 ASan / UBSan 运行断言。另提取生产 display VBIOS 分配 / 早退前缀，用有毒字节填充成功分配，依次模拟三个分配失败及全成功，要求可见缓存始终已初始化，并经真实发布入口拒绝空数据。GC2 / GFX 寄存器模拟覆盖仍由既有 simulator 任务负责。

针对性入口为 `make t.test_uapi t.test_libdrm_amdgpu t.test_asic_metadata`。本地 Windows 的 Python 语法及全部生产源码提取检查通过；环境没有 clang / gcc / make，harness 不能启动 C 编译器。因此未声称新增 C / C++ 断言已执行、ASan / UBSan 或 native kext 已通过；完整 host、macOS native 与 cross 验证由根代理汇合后执行。

## Alternatives considered

- **只把常量换成 Navi33 的 128 bit / 32 MiB / 16 GB**：改动最少，却将另一张卡的假值变成默认值，且本次没有容量和 MALL 的实测依据，不采用。
- **只移除 exact 门槛，失败仍用 Navi32 默认**：能修成功路径，但读 / 解析失败继续输出假硬件身份；改为初始未知并在真正的 DEV_INFO 消费边界明确拒绝。
- **将 setter 的 0 改成清空**：可以直接重置未知字段，却会让现有 MALL-only 调用擦掉已知 VRAM。ASIC 元数据在同一设备恢复期间仍可保留最后一次成功值，只需明确标记本次结果，不扩大内部接口。
- **为绕过 Mesa 断言填假 cache 或改 Mesa**：前者继续撒谎，后者超出此驱动发布路径修复范围；采用既有 errno 拒绝无法安全消费的组合，并保留官方 Navi33 特殊分支。
- **继续等待异步 PM 或修改 Metal 重试**：前者保留首次枚举失败且 `rdna3pm=0` 永远缺数据，后者扩大到用户态生命周期兼容层；复用 display 已缓存的真实数据同步发布即可修复本次窗口。

## Consequences

收益是默认 profile 与 exact 均只发布真实读取值，且可读的 display VBIOS 不再等待 PM 才可供首次枚举；未知失败不伪装成 Navi32。代价是 VBIOS 不可读 / 不完整的 DEV_INFO 明确失败，enum 诊断 profile 不再假报 ready。同进程 Metal 初始化失败没有自动重试，本次不改变这一限制；完整 GPU / ring 的其他异步时序仍待单独验证，不能保证 Metal 启动成功。零 MALL 的物理含义仍未知，不等同于无 cache；日志和本篇说明该 ABI 表达限制。

新建前检索全部活跃 Note：[UAPI revision 修复](2026-11-05-navi33-uapi-revision.md)、[固件基础](../feature/2026-11-04-navi33-firmware-foundation.md)和 [GFXHUB](../feature/2026-11-05-navi33-gfxhub.md)部分重叠，保留各自 discovery / 未绑定决定；SMU / 板表属于其他边界，不吸收或改写。[GitHub CI](../process/2026-11-04-github-hosted-validation.md)及 [macOS 15 编译契约](../process/2026-11-05-macos15-cross-native-contract.md)的证据边界不变。没有需要删除、归档或废弃的提案；旧 Note 反链由根代理汇合补齐。

不修改 PCI 7480 匹配、EFI、系统、原始 ROM 或硬件通过记录。RX 7600 XT / macOS 15 尚未实机启动；离线测试和任何 CI 绿色均不能证明出图、DP、Metal、功耗或恢复正确。
