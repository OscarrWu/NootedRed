// Renoir PowerPlay System Management Controller
//
// Copyright © 2024-2025 ChefKiss. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once
#include <IOKit/IOTypes.h>

// ══════════════════════════════════════════════════════════════════════════
// ⚠️ 防误用警示（T12）：本表是 **Renoir（SMU 11）** 的消息 ID，**不是** Phoenix（SMU 13.0.4）的表！
// 本 fork 目标是 AMD Radeon 780M（Phoenix）。Renoir 与 Phoenix 的 PPSMC 消息 ID **同名不同值 / 同值不同义**，
// 严禁按名字把本表常量用于 Phoenix 路径（按名字误判 = 本项目实际发生过的失效模式，见 T3 卡）。
// 完整对照与出处见：kb/逆向事实与结论/乙线R1-PPSMC消息表对照与0x02语义.md
//
// Renoir 常量在 Phoenix 表（smu_v13_0_4_ppsmc.h，最大已定义 ID 0x30）上的语义对照（T9 定论）：
// | Renoir 符号 | 值 | Phoenix 同值 ID / 存在性 | 判定 |
// |---|---|---|---|
// | PPSMC_MSG_GetSmuVersion       | 0x02 | 0x02 = PPSMC_MSG_GetPmfwVersion | 同值同义（0x02 例外，允许）|
// | PPSMC_MSG_PowerUpGfx          | 0x06 | 0x06 = PPSMC_MSG_PowerDownVcn   | ⚠️ 同值不同义（**反向语义**）|
// | PPSMC_MSG_PowerUpSdma         | 0x0E | 0x0E = PPSMC_MSG_SetDriverDramAddrLow | ⚠️ 同值不同义 |
// | PPSMC_MSG_DeviceDriverReset   | 0x1E | 0x1E = PPSMC_MSG_SetSoftMaxFclkByFreq  | ⚠️ 同值不同义 |
// | PPSMC_MSG_SoftReset           | 0x2E | 0x2E = PPSMC_MSG_PowerUpUmsch           | ⚠️ 同值不同义 |
// | PPSMC_MSG_PowerGateMmHub      | 0x35 | 不存在（> 0x30） | ⚠️ ID 越界 |
// | PPSMC_MSG_ForceGfxContentSave | 0x39 | 不存在（> 0x30） | ⚠️ ID 越界 |
// | PPSMC_MSG_PowerGateAtHub      | 0x3D | 不存在（> 0x30） | ⚠️ ID 越界 |
//
// 运行时缓解：上述 7 个风险 ID 均不在超时-L1 白名单 {0x01, 0x02, 0x03} 内，
// 经 smuSendMessage 必被闸拒绝（不达 PMFW）；但仍严禁把本表常量用于 Phoenix 路径（静态语义误读风险）。
// ══════════════════════════════════════════════════════════════════════════

constexpr UInt32 PPSMC_MSG_GetSmuVersion       = 0x2;
constexpr UInt32 PPSMC_MSG_PowerUpGfx          = 0x6;
constexpr UInt32 PPSMC_MSG_PowerUpSdma         = 0xE;
constexpr UInt32 PPSMC_MSG_DeviceDriverReset   = 0x1E;
constexpr UInt32 PPSMC_MSG_SoftReset           = 0x2E;
constexpr UInt32 PPSMC_MSG_PowerGateMmHub      = 0x35;
constexpr UInt32 PPSMC_MSG_ForceGfxContentSave = 0x39;
constexpr UInt32 PPSMC_MSG_PowerGateAtHub      = 0x3D;
