// Phoenix (SMU 13.0.4) PPSMC message IDs
// Source: linux smu_v13_0_4_ppsmc.h
#pragma once
#include <Base.h>

namespace PhoenixPPSMC {
constexpr UInt32 PPSMC_MSG_TestMessage               = 0x01;
constexpr UInt32 PPSMC_MSG_GetPmfwVersion            = 0x02;
constexpr UInt32 PPSMC_MSG_GetDriverIfVersion        = 0x03;
constexpr UInt32 PPSMC_MSG_PowerDownVcn              = 0x06;
constexpr UInt32 PPSMC_MSG_PowerUpVcn                = 0x07;
constexpr UInt32 PPSMC_MSG_SetHardMinVcn             = 0x08;
constexpr UInt32 PPSMC_MSG_SetSoftMinGfxclk          = 0x09;
constexpr UInt32 PPSMC_MSG_PrepareMp1ForUnload       = 0x0C;
constexpr UInt32 PPSMC_MSG_SetDriverDramAddrHigh     = 0x0D;
constexpr UInt32 PPSMC_MSG_SetDriverDramAddrLow      = 0x0E;
constexpr UInt32 PPSMC_MSG_TransferTableSmu2Dram     = 0x0F;
constexpr UInt32 PPSMC_MSG_TransferTableDram2Smu     = 0x10;
constexpr UInt32 PPSMC_MSG_GfxDeviceDriverReset      = 0x11;
constexpr UInt32 PPSMC_MSG_GetEnabledSmuFeatures     = 0x12;
constexpr UInt32 PPSMC_MSG_EnableGfxImu              = 0x16;
constexpr UInt32 PPSMC_MSG_GetGfxclkFrequency        = 0x17;
constexpr UInt32 PPSMC_MSG_AllowGfxOff               = 0x19;
constexpr UInt32 PPSMC_MSG_DisallowGfxOff            = 0x1A;
constexpr UInt32 PPSMC_MSG_SetSoftMaxGfxClk          = 0x1B;
constexpr UInt32 PPSMC_MSG_SetHardMinGfxClk          = 0x1C;
constexpr UInt32 PPSMC_MSG_PowerDownJpeg             = 0x21;
constexpr UInt32 PPSMC_MSG_PowerUpJpeg               = 0x22;
}
