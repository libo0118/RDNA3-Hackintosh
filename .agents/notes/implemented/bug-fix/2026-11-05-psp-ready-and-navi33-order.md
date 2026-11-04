# Agent Note: PSP readiness 不等同上一次命令 status

Status: implemented

## Problem

Linux v6.12 的 `psp_v13_0_ring_create` 在发送新命令前只等待 C2PMSG64 bit31，而项目还要求低16位为零。已置 readiness、但仍保留旧命令 status 的合法输入会被错误地等到超时。Navi33 的 PMFW-before-TMR 分派已有生产实现，但缺少 exact/default 的独立模型回归。

## Decision

新命令前的 readiness 仅检查 bit31；新 ring-create 命令之后原有 response/status 检查保持不变，不把 prior status 与当前命令结果混为一谈。

现有模拟器保留 MP0 13.0.10 的 TMR-before-PMFW 断言，另以 MP0 13.0.7 运行 exact/default 两种 PSP 传输场景，验证 PMFW 在 TMR 前且只加载一次。模型拒绝 bit31 未置位的寄存器值，接受 readiness 已置位且低位保留旧 status 的输入。

一手依据：[Linux v6.12 ring create](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/psp_v13_0.c#L369-L434)、[PMFW centralized 选择](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c#L95-L121)、[启动顺序](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c#L2355-L2390)。

## Alternatives considered

- 保留额外低位零约束：更严格，但与命名的 Linux readiness 契约不同，没有把旧 status 当新命令失败的依据。
- 把所有 response 等待都改成只检查 bit31：改动更广，会改变现有新命令错误处理，不属于本次修复。

## Consequences

这是共享传输修复和模拟验证，不证明目标 GPU 的真实 ROM、discovery 基址、签名固件接受或硬件启动。测试改变的只是 PSP 版本；其余固件是模拟器传输数据，不能当作整套 Navi33 硬件环境。

实际 C、ASan/UBSan 与 native 验证由 GitHub CI 执行，不预先标为通过。不启用 `7480` 绑定，不写 EFI 或 GPU。[PM bootstrap 决定](../feature/2026-11-04-navi33-smu-pm-bootstrap.md)仍保留。
