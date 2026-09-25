// VBIOSSMC 与邮箱常量 —— 用户态可编译版
//
// 从 src/NootedRed/Regs/{VBIOSSMC,SMU}.hpp 抽取常量，去掉了 IOKit 依赖
// （原文件 include <IOKit/IOTypes.h>，无法在分析机用户态编译）。
//
// ⚠️ 常量值必须与源文件一致。已由 tests/test_vbios_smc_seq.cpp 断言核对。
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once

#include <cstdint>

namespace display {

constexpr std::uint32_t VBIOSSMC_Status_BUSY = 0x0;
constexpr std::uint32_t VBIOSSMC_Result_OK = 0x1;
constexpr std::uint32_t VBIOSSMC_Result_Failed = 0xFF;
constexpr std::uint32_t VBIOSSMC_Result_UnknownCmd = 0xFE;
constexpr std::uint32_t VBIOSSMC_Result_CmdRejectedPrereq = 0xFD;
constexpr std::uint32_t VBIOSSMC_Result_CmdRejectedBusy = 0xFC;
constexpr std::uint32_t VBIOSSMC_MSG_TestMessage = 0x1;
constexpr std::uint32_t VBIOSSMC_MSG_GetSmuVersion = 0x2;
constexpr std::uint32_t VBIOSSMC_MSG_PowerUpGfx = 0x3;
constexpr std::uint32_t VBIOSSMC_MSG_SetDispclkFreq = 0x4;
constexpr std::uint32_t VBIOSSMC_MSG_SetDprefclkFreq = 0x5;
constexpr std::uint32_t VBIOSSMC_MSG_SetDppclkFreq = 0x6;
constexpr std::uint32_t VBIOSSMC_MSG_SetHardMinDcfclkByFreq = 0x7;
constexpr std::uint32_t VBIOSSMC_MSG_SetMinDeepSleepDcfclk = 0x8;
constexpr std::uint32_t VBIOSSMC_MSG_SetPhyclkVoltageByFreq = 0x9;
constexpr std::uint32_t VBIOSSMC_MSG_GetFclkFrequency = 0xA;
constexpr std::uint32_t VBIOSSMC_MSG_SetDisplayCount = 0xB;
constexpr std::uint32_t VBIOSSMC_MSG_EnableTmdp48MHzRefclkPwrDown = 0xC;
constexpr std::uint32_t VBIOSSMC_MSG_UpdatePmeRestore = 0xD;
constexpr std::uint32_t VBIOSSMC_MSG_SetVbiosDramAddrHigh = 0xE;
constexpr std::uint32_t VBIOSSMC_MSG_SetVbiosDramAddrLow = 0xF;
constexpr std::uint32_t VBIOSSMC_MSG_TransferTableSmu2Dram = 0x10;
constexpr std::uint32_t VBIOSSMC_MSG_TransferTableDram2Smu = 0x11;
constexpr std::uint32_t VBIOSSMC_MSG_SetDisplayIdleOptimizations = 0x12;
constexpr std::uint32_t VBIOSSMC_MSG_GetDprefclkFreq = 0x13;
constexpr std::uint32_t VBIOSSMC_MSG_GetDtbclkFreq = 0x14;
constexpr std::uint32_t VBIOSSMC_MSG_AllowZstatesEntry = 0x15;
constexpr std::uint32_t VBIOSSMC_MSG_DisallowZstatesEntry = 0x16;
constexpr std::uint32_t VBIOSSMC_MSG_SetDtbClk = 0x17;
constexpr std::uint32_t VBIOSSMC_MSG_Message_Count = 0x18;
constexpr std::uint32_t MP0_SMN_C2PMSG_58 = 0x7A;
constexpr std::uint32_t MP0_SMN_C2PMSG_91 = 0x9B;
constexpr std::uint32_t MP0_SMN_C2PMSG_100 = 0xA4;
constexpr std::uint32_t MP1_SMN_C2PMSG_66 = 0x282;
constexpr std::uint32_t MP1_SMN_C2PMSG_82 = 0x292;
constexpr std::uint32_t MP1_SMN_C2PMSG_90 = 0x29A;
constexpr std::uint32_t MP1_SMN_C2PMSG_67 = 0x283;
constexpr std::uint32_t MP1_SMN_C2PMSG_83 = 0x293;
constexpr std::uint32_t MP1_SMN_C2PMSG_91 = 0x29B;
constexpr std::uint32_t MP1_SMN_FPS_CNT = 0x2C4;
constexpr std::uint32_t MP1_FIRMWARE_FLAGS = 0x3010024;
constexpr std::uint32_t MP1_FIRMWARE_FLAGS_INTERRUPTS_ENABLED = 0x1;

}  // namespace display
