// Vega iGPU SMU Message Definitions
//
// Copyright © 2024-2025 ChefKiss. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once
#include <IOKit/IOTypes.h>

constexpr UInt32 MP0_SMN_C2PMSG_58                     = 0x7A;
constexpr UInt32 MP0_SMN_C2PMSG_91                     = 0x9B;
constexpr UInt32 MP0_SMN_C2PMSG_100                    = 0xA4;
constexpr UInt32 MP1_SMN_C2PMSG_66                     = 0x282;
constexpr UInt32 MP1_SMN_C2PMSG_82                     = 0x292;
constexpr UInt32 MP1_SMN_C2PMSG_90                     = 0x29A;
// VBIOSSMC 显示时钟邮箱（dcn314，已审查确认：mp_13_0_5_offset.h）
constexpr UInt32 MP1_SMN_C2PMSG_67                     = 0x283;
constexpr UInt32 MP1_SMN_C2PMSG_83                     = 0x293;
constexpr UInt32 MP1_SMN_C2PMSG_91                     = 0x29B;
constexpr UInt32 MP1_SMN_FPS_CNT                       = 0x2C4;
constexpr UInt32 MP1_FIRMWARE_FLAGS                    = 0x3010024;
constexpr UInt32 MP1_FIRMWARE_FLAGS_INTERRUPTS_ENABLED = 0x1;
