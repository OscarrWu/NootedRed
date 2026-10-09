// SMU 发送有界准入（超时-L1）白名单判定离线断言
//
// 依据：tmp/lead/下一步技术裁定-2026-10-09.md §2.2、§4 T1 验收 A3
// 验收要求：≥6 组，含正例（0x01/0x02/0x03/GetSmuVersion → 允许）、
//   负例（0x0D、0x10、0xFF → 拒绝）、对照组（未定义 ID 必须拒绝）。
//
// 编译运行（分析机）：
//   make -f src/NootedRed/tests/Makefile test
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.

#include <HWLibsSmuGate.hpp>

#include <cassert>
#include <cstdio>

using namespace NRedSmuGate;

// ── 白名单 ID（去重后）──
static constexpr UInt32 kTestMessage          = 0x01;  // PhoenixPPSMC::PPSMC_MSG_TestMessage
static constexpr UInt32 kGetPmfwVersion       = 0x02;  // PhoenixPPSMC::PPSMC_MSG_GetPmfwVersion
static constexpr UInt32 kGetDriverIfVersion   = 0x03;  // PhoenixPPSMC::PPSMC_MSG_GetDriverIfVersion
static constexpr UInt32 kGetSmuVersion        = 0x02;  // PPSMC_MSG_GetSmuVersion (Renoir/Raven，与 GetPmfwVersion 同 ID)

// ── 状态写类 / 默认拒绝集（任务卡 §2.2）──
static constexpr UInt32 kSetDriverDramAddrHigh = 0x0D;
static constexpr UInt32 kTransferTableDram2Smu = 0x10;
static constexpr UInt32 kEnableGfxImu          = 0x16;
static constexpr UInt32 kPowerUpVcn            = 0x07;
static constexpr UInt32 kPowerUpJpeg           = 0x22;
static constexpr UInt32 kPowerUpSdma           = 0x0E;  // Renoir PPSMC_MSG_PowerUpSdma
static constexpr UInt32 kPowerUpGfx            = 0x06;  // Renoir PPSMC_MSG_PowerUpGfx
static constexpr UInt32 kGfxDeviceDriverReset  = 0x11;  // Phoenix PPSMC_MSG_GfxDeviceDriverReset
static constexpr UInt32 kDeviceDriverReset     = 0x1E;  // Renoir PPSMC_MSG_DeviceDriverReset
static constexpr UInt32 kForceGfxContentSave   = 0x39;  // Renoir PPSMC_MSG_ForceGfxContentSave
static constexpr UInt32 kPowerGateMmHub        = 0x35;  // Renoir PPSMC_MSG_PowerGateMmHub
static constexpr UInt32 kPowerGateAtHub        = 0x3D;  // Renoir PPSMC_MSG_PowerGateAtHub

// ── 未定义/对照组 ID──
static constexpr UInt32 kUndefinedFF           = 0xFF;
static constexpr UInt32 kGetGfxclkFrequency    = 0x17;  // Phoenix PPSMC_MSG_GetGfxclkFrequency
static constexpr UInt32 kGetEnabledSmuFeatures = 0x12;  // Phoenix PPSMC_MSG_GetEnabledSmuFeatures
static constexpr UInt32 kAllowGfxOff           = 0x19;  // Phoenix PPSMC_MSG_AllowGfxOff
static constexpr UInt32 kDisallowGfxOff        = 0x1A;  // Phoenix PPSMC_MSG_DisallowGfxOff
static constexpr UInt32 kUndefined99           = 0x99;

static void test_whitelist_allows() {
    // 正例：白名单内的消息必须允许
    assert(smuMsgAllowedByTimeoutL1(kTestMessage)        == true);
    assert(smuMsgAllowedByTimeoutL1(kGetPmfwVersion)     == true);
    assert(smuMsgAllowedByTimeoutL1(kGetDriverIfVersion) == true);
    assert(smuMsgAllowedByTimeoutL1(kGetSmuVersion)      == true);  // 同 ID 0x02
    std::puts("  [PASS] 白名单消息全部允许");
}

static void test_status_write_rejected() {
    // 负例：状态写类消息必须拒绝（任务卡默认拒绝集）
    assert(smuMsgAllowedByTimeoutL1(kSetDriverDramAddrHigh) == false);
    assert(smuMsgAllowedByTimeoutL1(kTransferTableDram2Smu) == false);
    assert(smuMsgAllowedByTimeoutL1(kEnableGfxImu)          == false);
    assert(smuMsgAllowedByTimeoutL1(kPowerUpVcn)            == false);
    assert(smuMsgAllowedByTimeoutL1(kPowerUpJpeg)           == false);
    assert(smuMsgAllowedByTimeoutL1(kPowerUpSdma)           == false);
    assert(smuMsgAllowedByTimeoutL1(kPowerUpGfx)            == false);
    assert(smuMsgAllowedByTimeoutL1(kGfxDeviceDriverReset)  == false);
    assert(smuMsgAllowedByTimeoutL1(kDeviceDriverReset)     == false);
    assert(smuMsgAllowedByTimeoutL1(kForceGfxContentSave)   == false);
    assert(smuMsgAllowedByTimeoutL1(kPowerGateMmHub)        == false);
    assert(smuMsgAllowedByTimeoutL1(kPowerGateAtHub)        == false);
    std::puts("  [PASS] 状态写类/默认拒绝集消息全部拒绝");
}

static void test_undefined_ids_rejected() {
    // 对照组：未定义/非白名单 ID 必须拒绝
    assert(smuMsgAllowedByTimeoutL1(kUndefinedFF)           == false);
    assert(smuMsgAllowedByTimeoutL1(kGetGfxclkFrequency)    == false);
    assert(smuMsgAllowedByTimeoutL1(kGetEnabledSmuFeatures) == false);
    assert(smuMsgAllowedByTimeoutL1(kAllowGfxOff)           == false);
    assert(smuMsgAllowedByTimeoutL1(kDisallowGfxOff)        == false);
    assert(smuMsgAllowedByTimeoutL1(kUndefined99)           == false);
    // 边界：0x00 也非白名单
    assert(smuMsgAllowedByTimeoutL1(0x00)                   == false);
    // 边界：最大值
    assert(smuMsgAllowedByTimeoutL1(0xFFFFFFFF)             == false);
    std::puts("  [PASS] 未定义/边界 ID 全部拒绝");
}

static void test_idempotent() {
    // 幂等性：多次调用同一 ID 结果一致
    for (int i = 0; i < 10; ++i) {
        assert(smuMsgAllowedByTimeoutL1(kTestMessage)        == true);
        assert(smuMsgAllowedByTimeoutL1(kSetDriverDramAddrHigh) == false);
        assert(smuMsgAllowedByTimeoutL1(kUndefinedFF)        == false);
    }
    std::puts("  [PASS] 判定函数幂等");
}

int main() {
    std::puts("超时-L1（有界准入）白名单判定离线断言");
    test_whitelist_allows();
    test_status_write_rejected();
    test_undefined_ids_rejected();
    test_idempotent();
    std::puts("全部通过。");
    return 0;
}