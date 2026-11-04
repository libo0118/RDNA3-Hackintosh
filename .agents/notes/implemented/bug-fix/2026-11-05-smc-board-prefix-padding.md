# Agent Note: Atom 板表前缀不包含 SMU 内部 MMHUB padding

Status: implemented

## Problem

本机通过标准 ACPI API 得到与实时 BDF/VID/DID 匹配的 VFCT VBIOS 缓存，并核对 PCIR、ATOM、subsystem 和 BIOS part。其 `smc_dpm_info` 为 5.0、460 字节；common header 后只有 456 字节，下一张表紧接其后。按完整 `sizeof(BoardTable_t)=488` 要求会误拒这个真实输入，盲拷 488 则会读到下一张表。

## Decision

运行时仍由实际 MP1 和已支持的 driver IF 选择 13.0.7 ABI，不按营销名称或显存假设选布局。固定 Linux v6.12 vendor header 声明的 `MmHubPadding` 位于 456，后 32 字节明确为 SMU 内部使用且不要修改；所有非 MMHUB 字段都位于该前缀中。

原完整表输入保持可接受；对本机已观察的 Atom 5.0，接受包含完整 456 字节前缀的输入。只复制该完整前缀，保留 COMBO 中原有的 SMU 内部 padding，不复制邻表，也不以零值伪造板级字段。其它缺少完整数据的未知布局仍明确失败，SCPM 继续完全跳过板表拼接。

依据：[Linux 13.0.7 按所选 BoardTable 包装复制](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0_7_ppt.c#L406-L435)、[BoardTable padding 声明](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu13_driver_if_v13_0_7.h#L1354-L1363)。这不是声称找到命名的 `atom_smc_dpm_info_v5_0` 完整官方声明；目前没有该声明，保留此证据边界。

## Alternatives considered

- 继续要求 492 字节：实现最简单，但真实的 460 字节输入被永久拒绝。
- 不看 Atom 声明长度而拷贝完整 488 字节：贴近 Linux 当前的盲拷，但会把相邻表内容放进内部 padding，不采用。
- 将缺失的整个尾部清零：不读取邻表，但会改写 vendor 明示属于 SMU 内部的字节，不如保留原 COMBO。

## Verification

生产 C 编译断言约束 prefix offset；模拟输入包括完整表和已观察的 5.0 短表、截断功能字段、未知短格式，并在邻区放毒值。目标短表用例还通过生产 `vbios_parse` 消费 ATOM 2.3/44 的已用前缀；本地真实 ROM 已直接核对 master-data 指针仍位于 `+0x20`，但不声称解析 2.3 新增字段。上传必须保留完整前缀和原 COMBO padding；GFX boot 字段为零但独立 GFX DPM 正常时仍允许初始化，固定 UCLK 使用其真实 boot 字段。真正执行结果由 GitHub CI 记录，不预先标为通过。

## Consequences

此修复使[PM bootstrap](../feature/2026-11-04-navi33-smu-pm-bootstrap.md)能够接收已核验的目标表形状，但不证明 PMFW 已接受它、SCPM 状态或硬件初始化。原始 VFCT/ROM 只保留本地，不进入公共仓库或 CI；公共测试使用独立合成字节，不包含本机 ROM。
