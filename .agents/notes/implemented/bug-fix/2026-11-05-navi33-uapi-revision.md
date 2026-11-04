# Agent Note: Navi33 UAPI revision 由 GC discovery 与 NBIO strap 决定

Status: implemented

## Problem

`rdna3_drm_device_init` 已分别接收 PCI revision 与 NBIO strap `rev_id`，却把所有设备的 `external_rev` 都设置为 `rev_id + 0x20`。实际 GC 11.0.2 因此得到 Navi32 的 revision 区间；缺失或未知 GC 也能通过 `AMDGPU_INFO_DEV_INFO` 导出未经确认的身份。PCI `C0` 不是 NBIO chip revision，商品名称和 PCI device ID 也不能替代 IP discovery。

## Decision

初始化按已发现的完整 `gc_ip_version` 计算并保存身份：GC 11.0.2 为 NBIO `rev_id + 0x10`，GC 11.0.3 保留 `rev_id + 0x20`。`chip_rev` 原样保留 NBIO strap，`pci_rev` 原样保留 PCI config revision；不从营销名称、PCI `7480` 或显存容量推断版本。

缺失或未知 GC 不填充假 `external_rev`。已有 `AMDGPU_INFO_DEV_INFO` ioctl 边界只接受这两个 GC，在任何身份输出复制前返回 `-RD_ENODEV`，并保持调用者的输出缓冲区不变。沿用 `RDNA3UserClient` 的 errno 输出、libdrm_xnu 的 errno 转换和既有用户工具的 `perror("AMDGPU_INFO")`，不新增日志回调、计数器或错误协议。

`void rdna3_drm_device_init` 及全部 caller 契约保持不变，不在每个内部 setter、file-open 或非身份查询重复加守卫。kext 的 NBIO 实读逻辑不变，仅同步说明两种 GC offset 的注释。

### 一手依据

只读核对固定 Linux v6.12 的 [`soc21_common_early_init`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/soc21.c#L580-L629) 与 [GC 11.0.3 分支](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/soc21.c#L656-L670)。[`amdgpu_device_get_rev_id`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c#L1156-L1159) 调用 NBIO 的 `get_rev_id`；[`nbio_v4_3_get_rev_id`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/nbio_v4_3.c#L40-L48) 从 strap 寄存器取字段，不读取 PCI revision。

### Verification

`tests/test_uapi.c` 保留原 Navi32 `external_rev == 0x20` 枚举及 shadow/CSA 回归，新增十组初始化后经生产 `rdna3_drm_ioctl` → `AMDGPU_INFO_DEV_INFO` 的场景：两个 GC offset、零/非零 NBIO strap、PCI revision 独立变化、故意不一致的 PCI product 与 GC，以及缺失 GC、11.0.0、11.0.4、11.1.2、12.0.2。断言已保存的身份、成功输出、失败 errno、失败时完整缓冲区未写入及 file/VM 清理。

本地 Windows 未找到 clang、gcc 或 make，因此新增 C 回归尚未执行；没有声称已观察 red/green、ASan/UBSan 或 native kext 编译通过。根代理汇合后须运行 `make t.test_uapi`、完整 sanitizers/host 回归与 macOS native CI。Note 门禁与 `git diff --check` 仅是结构和差异校验，不替代这些执行证据。

## Alternatives considered

- 按 `7480` 或 PCI `C0` 选择 offset：表面改动少，但两者不表示 GC IP 和 NBIO strap，无法保持三种身份字段独立，不采用。
- 保留未知 GC 的 Navi32 默认值：兼容旧枚举最省事，但向 Mesa 导出假身份；因此只在真实身份查询边界明确拒绝，不新增未请求的 Navi31 支持。
- 将初始化改为返回 int 并扩散全部 caller：可以更早拒绝，但本任务只需阻止无依据的用户态身份，已有 ioctl 错误通道足够，不扩展内部契约。

## Consequences

收益是两种已支持 GC 的 stored/query revision 一致，未知输入不能冒充 Navi32；代价是依赖缺失或未知 discovery 的旧调用者现在得到明确的 `ENODEV`，而不是错误枚举成功。本次只修 revision 身份，不声称其余 DEV_INFO 拓扑、显存属性或完整 Navi33 图形 bring-up 已适配。

新建前检索全部活跃 Note：[固件基础](../feature/2026-11-04-navi33-firmware-foundation.md)与本篇部分重叠，均坚持 discovery 选择与未知输入不回退。SMU 状态、PM bootstrap、Atom 板表及 PSP readiness 属于不同边界，保留原决定，不吸收、归档或改写；[GitHub CI](../process/2026-11-04-github-hosted-validation.md)及 [macOS 15 验证契约](../process/2026-11-05-macos15-cross-native-contract.md)的证据限制保持不变。没有需要废弃的提案。

不改 `7480` IOKit 匹配、EFI、系统配置、固件或硬件通过记录。尚未实机启动，离线回归与 CI 均不能证明 RX 7600 XT 在 macOS 15 的初始化、出图、DP、Metal、功耗或恢复成功。
