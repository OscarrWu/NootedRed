// T12 防误用离线断言：Renoir vs Phoenix 的 PPSMC 消息"值→语义"对照
//
// 依据：tmp/lead/T9判读裁定-2026-10-09.md §二（T12 规格）；
//       kb/逆向事实与结论/乙线R1-PPSMC消息表对照与0x02语义.md（T9 定论）
// 要求：Phoenix 可达路径不得使用 Renoir 常量；以"值→语义"表驱动；
//       含 4 条同值不同义 + 3 条越界 + 1 条同值同义（0x02）的例外。
//
// 编译运行（分析机）：
//   make -f src/NootedRed/tests/Makefile test
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.

#include <GPUDriversAMD/PhoenixPPSMC.hpp>
#include <GPUDriversAMD/RenoirPPSMC.hpp>

#include <cassert>
#include <cstdio>

// ── 值→语义对照表（T9 定论；Phoenix 权威：smu_v13_0_4_ppsmc.h，最大已定义 ID 0x30）──
struct RenoirVsPhoenix {
    const char* renoirName;
    UInt32      renoirVal;
    const char* phoenixSemantic;  // Phoenix 同值 ID 的实际语义，或 "ID 不存在 (> 0x30)"
    bool        allowedOnPhoenix; // 0x02 = 同值同义例外（true）；7 条风险 = false
};

static constexpr RenoirVsPhoenix kTable[] = {
    {"PPSMC_MSG_GetSmuVersion",       PPSMC_MSG_GetSmuVersion,       "PPSMC_MSG_GetPmfwVersion（同值同义）", true},
    {"PPSMC_MSG_PowerUpGfx",          PPSMC_MSG_PowerUpGfx,          "PPSMC_MSG_PowerDownVcn（反向语义）",   false},
    {"PPSMC_MSG_PowerUpSdma",         PPSMC_MSG_PowerUpSdma,         "PPSMC_MSG_SetDriverDramAddrLow",       false},
    {"PPSMC_MSG_DeviceDriverReset",   PPSMC_MSG_DeviceDriverReset,   "PPSMC_MSG_SetSoftMaxFclkByFreq",       false},
    {"PPSMC_MSG_SoftReset",           PPSMC_MSG_SoftReset,           "PPSMC_MSG_PowerUpUmsch",               false},
    {"PPSMC_MSG_PowerGateMmHub",      PPSMC_MSG_PowerGateMmHub,      "ID 不存在 (> 0x30)",                   false},
    {"PPSMC_MSG_ForceGfxContentSave", PPSMC_MSG_ForceGfxContentSave, "ID 不存在 (> 0x30)",                   false},
    {"PPSMC_MSG_PowerGateAtHub",      PPSMC_MSG_PowerGateAtHub,      "ID 不存在 (> 0x30)",                   false},
};

// ① 同值同义例外：Phoenix 0x02 = GetPmfwVersion ≡ GetSmuVersion
static void test_0x02_exception() {
    assert(PhoenixPPSMC::PPSMC_MSG_GetPmfwVersion == PPSMC_MSG_GetSmuVersion);
    assert(PPSMC_MSG_GetSmuVersion == 0x02);
    std::puts("  [PASS] 0x02 同值同义例外（GetPmfwVersion ≡ GetSmuVersion）");
}

// ② 同值不同义（4 条，其中 0x06 为反向语义）：固化"值冲突"事实，防 PhoenixPPSMC.hpp 改动造成意外
static void test_same_value_different_semantics() {
    assert(PhoenixPPSMC::PPSMC_MSG_PowerDownVcn == PPSMC_MSG_PowerUpGfx);        // 0x06 = PowerDownVcn
    assert(PhoenixPPSMC::PPSMC_MSG_SetDriverDramAddrLow == PPSMC_MSG_PowerUpSdma);  // 0x0E = SetDriverDramAddrLow
    // 0x1E / 0x2E：Phoenix 表（smu_v13_0_4_ppsmc.h）上分别为 SetSoftMaxFclkByFreq / PowerUpUmsch，
    // 项目 PhoenixPPSMC.hpp 仅收录子集 ⇒ 以"值→语义"表 + 白名单不相交断言覆盖（见 ④），不在此处硬编码。
    std::puts("  [PASS] 同值不同义 0x06/0x0E（Phoenix 侧语义已固化；0x1E/0x2E 由表驱动）");
}

// ③ 越界（3 条）：0x35/0x39/0x3D > Phoenix 最大已定义 ID 0x30（PPSMC_Message_Count = 0x31）
static void test_ids_out_of_range() {
    assert(PPSMC_MSG_PowerGateMmHub > 0x30);
    assert(PPSMC_MSG_ForceGfxContentSave > 0x30);
    assert(PPSMC_MSG_PowerGateAtHub > 0x30);
    std::puts("  [PASS] 3 条越界 ID（0x35/0x39/0x3D > 0x30）");
}

// ④ 白名单不相交：7 条风险 ID 均不在超时-L1 白名单 {0x01,0x02,0x03} 内 ⇒ T1 闸必拒（不达 PMFW）
static void test_whitelist_excludes_renoir_risks() {
    constexpr UInt32 kWhitelist[] = {
        PhoenixPPSMC::PPSMC_MSG_TestMessage,       // 0x01
        PhoenixPPSMC::PPSMC_MSG_GetPmfwVersion,    // 0x02
        PhoenixPPSMC::PPSMC_MSG_GetDriverIfVersion // 0x03
    };
    for (const auto& entry : kTable) {
        if (!entry.allowedOnPhoenix) {
            assert(entry.renoirVal != kWhitelist[0]);
            assert(entry.renoirVal != kWhitelist[1]);
            assert(entry.renoirVal != kWhitelist[2]);
        }
    }
    // 例外复核：0x02 在白名单内（同值同义放行）
    assert(PPSMC_MSG_GetSmuVersion == kWhitelist[1]);
    std::puts("  [PASS] 白名单 {0x01,0x02,0x03} 与 7 条风险 ID 不相交（0x02 例外放行）");
}

// ⑤ 表自检：8 条条目的 allowedOnPhoenix 中恰好 1 条为 true（0x02 例外）、7 条为 false
static void test_table_shape() {
    std::size_t allowed = 0, denied = 0;
    for (const auto& entry : kTable) {
        (entry.allowedOnPhoenix ? allowed : denied) += 1;
    }
    assert(allowed == 1);
    assert(denied == 7);
    assert(static_cast<std::size_t>(kTable[0].renoirVal) == 0x02);
    std::puts("  [PASS] 对照表形态：1 例外 + 7 风险");
}

int main() {
    std::puts("T12 防误用离线断言：Renoir vs Phoenix 值→语义对照");
    test_0x02_exception();
    test_same_value_different_semantics();
    test_ids_out_of_range();
    test_whitelist_excludes_renoir_risks();
    test_table_shape();
    std::puts("全部通过。");
    return 0;
}