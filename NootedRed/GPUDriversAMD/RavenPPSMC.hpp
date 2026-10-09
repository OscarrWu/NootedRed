// Raven PowerPlay System Management Controller
//
// Copyright © 2024-2025 ChefKiss. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once
#include <IOKit/IOTypes.h>

// ⚠️ 注记（T12）：当前无任何 `.cpp` `#include` 本文件（仅 pbxproj 登记为 Headers）——死文件，
// 语义与 RenoirPPSMC.hpp 几乎一致（仅差 PowerGateAtHub）。禁止新增 include；如需清理另案处理。
// 若未来使用，请先核对 kb/逆向事实与结论/乙线R1-PPSMC消息表对照与0x02语义.md 的同值不同义/越界对照表。

constexpr UInt32 PPSMC_MSG_GetSmuVersion       = 0x2;
constexpr UInt32 PPSMC_MSG_PowerUpGfx          = 0x6;
constexpr UInt32 PPSMC_MSG_PowerUpSdma         = 0xE;
constexpr UInt32 PPSMC_MSG_DeviceDriverReset   = 0x1E;
constexpr UInt32 PPSMC_MSG_SoftReset           = 0x2E;
constexpr UInt32 PPSMC_MSG_PowerGateMmHub      = 0x35;
constexpr UInt32 PPSMC_MSG_ForceGfxContentSave = 0x39;
