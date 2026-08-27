// VBIOSSMC Message Definitions (DCN314 Display Clock)
//
// 来源：Linux amdgpu dcn314_smu.c:64-94（已审查确认：Verifier 3/3 PASS）
// 用于 780M（DCN 3.1.4）显示时钟适配，与 PPSMC（计算侧）是两套独立消息体系
// 邮箱寄存器在 Regs/SMU.hpp：MP1_SMN_C2PMSG_67/83/91
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.

#pragma once
#include <IOKit/IOTypes.h>

// 状态/结果常量（dcn314_smu.c:89-94）
constexpr UInt32 VBIOSSMC_Status_BUSY              = 0x0;
constexpr UInt32 VBIOSSMC_Result_OK                = 0x1;
constexpr UInt32 VBIOSSMC_Result_Failed            = 0xFF;
constexpr UInt32 VBIOSSMC_Result_UnknownCmd        = 0xFE;
constexpr UInt32 VBIOSSMC_Result_CmdRejectedPrereq = 0xFD;
constexpr UInt32 VBIOSSMC_Result_CmdRejectedBusy   = 0xFC;

// 消息号常量（23 个，0x1-0x17，dcn314_smu.c:64-87）
constexpr UInt32 VBIOSSMC_MSG_TestMessage                  = 0x1;
constexpr UInt32 VBIOSSMC_MSG_GetSmuVersion                = 0x2;
constexpr UInt32 VBIOSSMC_MSG_PowerUpGfx                   = 0x3;
constexpr UInt32 VBIOSSMC_MSG_SetDispclkFreq               = 0x4;
constexpr UInt32 VBIOSSMC_MSG_SetDprefclkFreq              = 0x5;
constexpr UInt32 VBIOSSMC_MSG_SetDppclkFreq                = 0x6;
constexpr UInt32 VBIOSSMC_MSG_SetHardMinDcfclkByFreq       = 0x7;
constexpr UInt32 VBIOSSMC_MSG_SetMinDeepSleepDcfclk        = 0x8;
constexpr UInt32 VBIOSSMC_MSG_SetPhyclkVoltageByFreq       = 0x9;
constexpr UInt32 VBIOSSMC_MSG_GetFclkFrequency             = 0xA;
constexpr UInt32 VBIOSSMC_MSG_SetDisplayCount              = 0xB;
constexpr UInt32 VBIOSSMC_MSG_EnableTmdp48MHzRefclkPwrDown = 0xC;
constexpr UInt32 VBIOSSMC_MSG_UpdatePmeRestore             = 0xD;
constexpr UInt32 VBIOSSMC_MSG_SetVbiosDramAddrHigh         = 0xE;
constexpr UInt32 VBIOSSMC_MSG_SetVbiosDramAddrLow          = 0xF;
constexpr UInt32 VBIOSSMC_MSG_TransferTableSmu2Dram        = 0x10;
constexpr UInt32 VBIOSSMC_MSG_TransferTableDram2Smu        = 0x11;
constexpr UInt32 VBIOSSMC_MSG_SetDisplayIdleOptimizations  = 0x12;
constexpr UInt32 VBIOSSMC_MSG_GetDprefclkFreq              = 0x13;
constexpr UInt32 VBIOSSMC_MSG_GetDtbclkFreq                = 0x14;
constexpr UInt32 VBIOSSMC_MSG_AllowZstatesEntry            = 0x15;
constexpr UInt32 VBIOSSMC_MSG_DisallowZstatesEntry         = 0x16;
constexpr UInt32 VBIOSSMC_MSG_SetDtbClk                    = 0x17;
constexpr UInt32 VBIOSSMC_MSG_Message_Count                = 0x18;
