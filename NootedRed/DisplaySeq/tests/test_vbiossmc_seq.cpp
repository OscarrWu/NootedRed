// 验收测试：VBIOSSMC 消息序列生成器（任务 A 新增函数）
//
// 对应 tmp/第五步翻译规格.md §4（任务 A）验收判据：
//   1. §3.2 清单里每一个 generate* 都已实现（在 VbiosSmcSeq.hpp 中）
//   2. 每个函数有测试，断言：消息号、参数值、事务形状、单位换算
//   3. generateSetZstateSupport 分流到 6 种 support 值
//
// 编译运行（分析机）：
//   make -f src/NootedRed/DisplaySeq/Makefile test
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.

#include "RegOp.hpp"
#include "VBIOSSMC.hpp"
#include "VbiosSmcSeq.hpp"

#include <stdint.h>
#include <cstdio>
#include <cstdlib>

using namespace display;

// ── 测试用邮箱地址 ──
static constexpr RegAddr kMb67 = 0x283;
static constexpr RegAddr kMb83 = 0x293;
static constexpr RegAddr kMb91 = 0x29B;

// ── 辅助：生成序列并验证标准 6-op 事务形状 ──
// 检查消息号 (msgId)、参数值 (param)、以及 6 步的 op 种类与地址。
// 返回第 2 步（parameter write）的 value，供调用方进一步检查单位换算。
static RegValue checkStandardTransaction(const RegSeq& seq, uint32_t expectedMsgId,
                                          uint32_t expectedParam,
                                          const char* label) {
    if (seq.size() != 6) {
        std::fprintf(stderr, "  [FAIL] %s: expected 6 ops, got %zu\n", label, seq.size());
        std::exit(1);
    }
    // op[0]: poll(91)
    if (seq[0].kind != RegOp::Kind::Poll || seq[0].addr != kMb91) {
        std::fprintf(stderr, "  [FAIL] %s: op[0] expected poll(0x%x), got kind=%d addr=0x%x\n",
                     label, kMb91, (int)seq[0].kind, seq[0].addr);
        std::exit(1);
    }
    // op[1]: write(91, BUSY) = clear response
    if (seq[1].kind != RegOp::Kind::Write || seq[1].addr != kMb91 || seq[1].value != VBIOSSMC_Status_BUSY) {
        std::fprintf(stderr, "  [FAIL] %s: op[1] expected write(0x%x, BUSY), got kind=%d addr=0x%x val=0x%x\n",
                     label, kMb91, (int)seq[1].kind, seq[1].addr, seq[1].value);
        std::exit(1);
    }
    // op[2]: write(83, param)
    if (seq[2].kind != RegOp::Kind::Write || seq[2].addr != kMb83) {
        std::fprintf(stderr, "  [FAIL] %s: op[2] expected write(0x%x), got kind=%d addr=0x%x\n",
                     label, kMb83, (int)seq[2].kind, seq[2].addr);
        std::exit(1);
    }
    if (seq[2].value != expectedParam) {
        std::fprintf(stderr, "  [FAIL] %s: op[2] param=0x%x (expected 0x%x)\n",
                     label, seq[2].value, expectedParam);
        std::exit(1);
    }
    // op[3]: write(67, msgId)
    if (seq[3].kind != RegOp::Kind::Write || seq[3].addr != kMb67 || seq[3].value != expectedMsgId) {
        std::fprintf(stderr, "  [FAIL] %s: op[3] msgId expected 0x%x, got kind=%d addr=0x%x val=0x%x\n",
                     label, expectedMsgId, (int)seq[3].kind, seq[3].addr, seq[3].value);
        std::exit(1);
    }
    // op[4]: poll(91)
    if (seq[4].kind != RegOp::Kind::Poll || seq[4].addr != kMb91) {
        std::fprintf(stderr, "  [FAIL] %s: op[4] expected poll(0x%x), got kind=%d addr=0x%x\n",
                     label, kMb91, (int)seq[4].kind, seq[4].addr);
        std::exit(1);
    }
    // op[5]: read(83)
    if (seq[5].kind != RegOp::Kind::Read || seq[5].addr != kMb83) {
        std::fprintf(stderr, "  [FAIL] %s: op[5] expected read(0x%x), got kind=%d addr=0x%x\n",
                     label, kMb83, (int)seq[5].kind, seq[5].addr);
        std::exit(1);
    }
    return seq[2].value;
}

// ============================================================================
// 测试用例
// ============================================================================

// generateGetSmuVersion: msg=0x2, param=0
// Linux: dcn314_smu_get_smu_version L166-172
static void test_get_smu_version() {
    vbios_smc::ZStateSupport unused; (void)unused; // ensure enum accessible
    RegOp buf[64];
    RegSeq seq(buf, 64);
    vbios_smc::generateGetSmuVersion(seq, kMb67, kMb83, kMb91);
    checkStandardTransaction(seq, VBIOSSMC_MSG_GetSmuVersion, 0, "GetSmuVersion");
    std::puts("  [PASS] generateGetSmuVersion: msg=0x2 param=0");
}

// generateTestMessage: msg=0x1, param 透传
// Linux: VBIOSSMC_MSG_TestMessage L64
static void test_test_message() {
    RegOp buf[64];
    RegSeq seq(buf, 64);
    vbios_smc::generateTestMessage(seq, kMb67, kMb83, kMb91, 42);
    checkStandardTransaction(seq, VBIOSSMC_MSG_TestMessage, 42, "TestMessage");
    std::puts("  [PASS] generateTestMessage: msg=0x1 param=42");
}

// generatePowerUpGfx: msg=0x3, param=0
// Linux: VBIOSSMC_MSG_PowerUpGfx L66
static void test_power_up_gfx() {
    RegOp buf[64];
    RegSeq seq(buf, 64);
    vbios_smc::generatePowerUpGfx(seq, kMb67, kMb83, kMb91);
    checkStandardTransaction(seq, VBIOSSMC_MSG_PowerUpGfx, 0, "PowerUpGfx");
    std::puts("  [PASS] generatePowerUpGfx: msg=0x3 param=0");
}

// generateEnablePhyRefclkPwrdwn: msg=0xC, param 按 enable 分枝
// Linux: dcn314_smu_enable_phy_refclk_pwrdwn L274-290
static void test_enable_phy_refclk_pwrdwn() {
    // enable=true → idle_info = df_request_disabled|phy_ref_clk_off = 0x3
    {
        RegOp buf[64];
        RegSeq seq(buf, 64);
        vbios_smc::generateEnablePhyRefclkPwrdwn(seq, kMb67, kMb83, kMb91, true);
        checkStandardTransaction(seq, VBIOSSMC_MSG_EnableTmdp48MHzRefclkPwrDown, 0x3, "PhyRefclk_enable");
    }
    // enable=false → idle_info = 0
    {
        RegOp buf[64];
        RegSeq seq(buf, 64);
        vbios_smc::generateEnablePhyRefclkPwrdwn(seq, kMb67, kMb83, kMb91, false);
        checkStandardTransaction(seq, VBIOSSMC_MSG_EnableTmdp48MHzRefclkPwrDown, 0, "PhyRefclk_disable");
    }
    std::puts("  [PASS] generateEnablePhyRefclkPwrdwn: msg=0xC enable→0x3, disable→0");
}

// generateEnablePmeWa: msg=0xD, param=0
// Linux: dcn314_smu_enable_pme_wa L292-301
static void test_enable_pme_wa() {
    RegOp buf[64];
    RegSeq seq(buf, 64);
    vbios_smc::generateEnablePmeWa(seq, kMb67, kMb83, kMb91);
    checkStandardTransaction(seq, VBIOSSMC_MSG_UpdatePmeRestore, 0, "EnablePmeWa");
    std::puts("  [PASS] generateEnablePmeWa: msg=0xD param=0");
}

// generateSetVbiosDramAddrHigh: msg=0xE, param=addrHigh（不换算）
// Linux: dcn314_smu_set_dram_addr_high L303-310
static void test_set_vbios_dram_addr_high() {
    RegOp buf[64];
    RegSeq seq(buf, 64);
    vbios_smc::generateSetVbiosDramAddrHigh(seq, kMb67, kMb83, kMb91, 0x80);
    checkStandardTransaction(seq, VBIOSSMC_MSG_SetVbiosDramAddrHigh, 0x80, "DramAddrHigh");
    std::puts("  [PASS] generateSetVbiosDramAddrHigh: msg=0xE param=0x80 (raw)");
}

// generateSetVbiosDramAddrLow: msg=0xF, param=addrLow（不换算）
// Linux: dcn314_smu_set_dram_addr_low L312-319
static void test_set_vbios_dram_addr_low() {
    RegOp buf[64];
    RegSeq seq(buf, 64);
    vbios_smc::generateSetVbiosDramAddrLow(seq, kMb67, kMb83, kMb91, 0xFFCA4000);
    checkStandardTransaction(seq, VBIOSSMC_MSG_SetVbiosDramAddrLow, 0xFFCA4000, "DramAddrLow");
    std::puts("  [PASS] generateSetVbiosDramAddrLow: msg=0xF param=0xFFCA4000 (raw)");
}

// generateTransferDpmTableSmu2Dram: msg=0x10, param=TABLE_DPMCLOCKS(4)
// Linux: dcn314_smu_transfer_dpm_table_smu_2_dram L321-328
static void test_transfer_dpm_table_smu2dram() {
    RegOp buf[64];
    RegSeq seq(buf, 64);
    vbios_smc::generateTransferDpmTableSmu2Dram(seq, kMb67, kMb83, kMb91);
    checkStandardTransaction(seq, VBIOSSMC_MSG_TransferTableSmu2Dram, 4, "TransferDpmTableSmu2Dram");
    std::puts("  [PASS] generateTransferDpmTableSmu2Dram: msg=0x10 param=4 (TABLE_DPMCLOCKS)");
}

// generateTransferWmTableDram2Smu: msg=0x11, param=TABLE_WATERMARKS(1)
// Linux: dcn314_smu_transfer_wm_table_dram_2_smu L330-337
static void test_transfer_wm_table_dram2smu() {
    RegOp buf[64];
    RegSeq seq(buf, 64);
    vbios_smc::generateTransferWmTableDram2Smu(seq, kMb67, kMb83, kMb91);
    checkStandardTransaction(seq, VBIOSSMC_MSG_TransferTableDram2Smu, 1, "TransferWmTableDram2Smu");
    std::puts("  [PASS] generateTransferWmTableDram2Smu: msg=0x11 param=1 (TABLE_WATERMARKS)");
}

// generateSetZstateSupport: 6 种 support 值 → 对应 msg 与 param
// Linux: dcn314_smu_set_zstate_support L339-386
static void test_zstate_support_all_cases() {
    using namespace vbios_smc;

    // ZSTATE_ALLOW → msg=0x15, param=0x700
    {
        RegOp buf[64];
        RegSeq seq(buf, 64);
        generateSetZstateSupport(seq, kMb67, kMb83, kMb91, ZSTATE_ALLOW);
        checkStandardTransaction(seq, VBIOSSMC_MSG_AllowZstatesEntry, 0x700, "ZState_ALLOW");
    }
    // ZSTATE_DISALLOW → msg=0x15, param=0
    {
        RegOp buf[64];
        RegSeq seq(buf, 64);
        generateSetZstateSupport(seq, kMb67, kMb83, kMb91, ZSTATE_DISALLOW);
        checkStandardTransaction(seq, VBIOSSMC_MSG_AllowZstatesEntry, 0, "ZState_DISALLOW");
    }
    // ZSTATE_ALLOW_Z10_ONLY → msg=0x15, param=0x400
    {
        RegOp buf[64];
        RegSeq seq(buf, 64);
        generateSetZstateSupport(seq, kMb67, kMb83, kMb91, ZSTATE_ALLOW_Z10_ONLY);
        checkStandardTransaction(seq, VBIOSSMC_MSG_AllowZstatesEntry, 0x400, "ZState_Z10_ONLY");
    }
    // ZSTATE_ALLOW_Z8_Z10_ONLY → msg=0x15, param=0x500
    {
        RegOp buf[64];
        RegSeq seq(buf, 64);
        generateSetZstateSupport(seq, kMb67, kMb83, kMb91, ZSTATE_ALLOW_Z8_Z10_ONLY);
        checkStandardTransaction(seq, VBIOSSMC_MSG_AllowZstatesEntry, 0x500, "ZState_Z8_Z10_ONLY");
    }
    // ZSTATE_ALLOW_Z8_ONLY → msg=0x15, param=0x100
    {
        RegOp buf[64];
        RegSeq seq(buf, 64);
        generateSetZstateSupport(seq, kMb67, kMb83, kMb91, ZSTATE_ALLOW_Z8_ONLY);
        checkStandardTransaction(seq, VBIOSSMC_MSG_AllowZstatesEntry, 0x100, "ZState_Z8_ONLY");
    }
    // ZSTATE_UNKNOWN → msg=0x15, param=0
    {
        RegOp buf[64];
        RegSeq seq(buf, 64);
        generateSetZstateSupport(seq, kMb67, kMb83, kMb91, ZSTATE_UNKNOWN);
        checkStandardTransaction(seq, VBIOSSMC_MSG_AllowZstatesEntry, 0, "ZState_UNKNOWN");
    }
    std::puts("  [PASS] generateSetZstateSupport: 6 cases all msg=0x15, params vary");
}

// generateSetDtbClk: msg=0x17, param=enable?1:0
// Linux: dcn314_smu_set_dtbclk L388-398
static void test_set_dtb_clk() {
    // enable=true → param=1
    {
        RegOp buf[64];
        RegSeq seq(buf, 64);
        vbios_smc::generateSetDtbClk(seq, kMb67, kMb83, kMb91, true);
        checkStandardTransaction(seq, VBIOSSMC_MSG_SetDtbClk, 1, "DtbClk_enable");
    }
    // enable=false → param=0
    {
        RegOp buf[64];
        RegSeq seq(buf, 64);
        vbios_smc::generateSetDtbClk(seq, kMb67, kMb83, kMb91, false);
        checkStandardTransaction(seq, VBIOSSMC_MSG_SetDtbClk, 0, "DtbClk_disable");
    }
    std::puts("  [PASS] generateSetDtbClk: msg=0x17 enable→1, disable→0");
}

// ═══════════════════════════════════════════════════════════════════════════
// main
// ═══════════════════════════════════════════════════════════════════════════
int main() {
    std::puts("验收测试：VBIOSSMC 序列生成器（任务 A 新增函数）");
    std::puts("── 消息号/参数值/事务形状 ──");

    test_get_smu_version();
    test_test_message();
    test_power_up_gfx();
    test_enable_phy_refclk_pwrdwn();
    test_enable_pme_wa();
    test_set_vbios_dram_addr_high();
    test_set_vbios_dram_addr_low();
    test_transfer_dpm_table_smu2dram();
    test_transfer_wm_table_dram2smu();
    test_zstate_support_all_cases();
    test_set_dtb_clk();

    std::puts("全部通过。");
    return 0;
}
