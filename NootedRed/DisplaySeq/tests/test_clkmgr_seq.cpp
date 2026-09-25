// 第五步任务 B 验收测试：DCN314 时钟主流程翻译
//
// 对应 tmp/第五步翻译规格.md §5.4 验收判据：
//   1. 每个函数有测试，断言：消息号序列与顺序、参数值、状态机字段迁移（next->pwrState 等）、
//      shouldSetClock 真值表
//   2. 一条测试专门断言"下发顺序"：dppClockLowered = true / false 两种输入下
//      dispclk 与 dppclk 的相对顺序与 Linux 一致
//   3. 一条测试断言：DMCUB 相关消息（若有）不在序列中
//   4. make -f src/NootedRed/DisplaySeq/Makefile test 全绿
//
// 编译运行（分析机）：
//   make -f src/NootedRed/DisplaySeq/Makefile test
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.

#include "Dcn314ClkMgrSeq.hpp"
#include "VBIOSSMC.hpp"

#include <cassert>
#include <cstdio>

using namespace display;

// ── 测试用的常量（Phoenix 的 VBIOSSMC 邮箱，BASE_IDX=0 / SEG0）──
// 值取自 src/NootedRed/Regs/SMU.hpp，此处复写以独立校验。
static constexpr RegAddr kMailbox67 = 0x283;
static constexpr RegAddr kMailbox83 = 0x293;
static constexpr RegAddr kMailbox91 = 0x29B;
static constexpr dcn314_clk::Mailbox kMb = {kMailbox67, kMailbox83, kMailbox91};

// 从序列中抽取"消息事务"：每 6 个 op 为一笔
// （等闲 → 清响应 → 写参数 → 写消息触发 → 等完成 → 读回参数），
// 同时逐项校验事务形状与顺序（Linux dcn314_smu.c L118-163）。
struct MsgTx { uint32_t msgId; uint32_t param; };

static size_t collectTx(const RegSeq& seq, MsgTx* out, size_t cap) {
    assert(seq.size() % 6 == 0);
    size_t n = 0;
    for (size_t i = 0; i + 5 < seq.size(); i += 6) {
        assert(seq[i].kind == RegOp::Kind::Poll && seq[i].addr == kMailbox91);       // wait_idle_before
        assert(seq[i + 1].kind == RegOp::Kind::Write && seq[i + 1].addr == kMailbox91);
        assert(seq[i + 1].value == VBIOSSMC_Status_BUSY);                            // clear_response
        assert(seq[i + 2].kind == RegOp::Kind::Write && seq[i + 2].addr == kMailbox83); // write_param
        assert(seq[i + 3].kind == RegOp::Kind::Write && seq[i + 3].addr == kMailbox67); // trigger_msg
        assert(seq[i + 4].kind == RegOp::Kind::Poll && seq[i + 4].addr == kMailbox91);  // wait_complete
        assert(seq[i + 5].kind == RegOp::Kind::Read && seq[i + 5].addr == kMailbox83);  // read_back_param
        if (n < cap)
            out[n++] = {seq[i + 3].value, seq[i + 2].value};
    }
    return n;
}

// ── 1. shouldSetClock 真值表 ──────────────────────────────────────────────
// Linux: ((safe_to_lower && calc_clk < cur_clk) || calc_clk > cur_clk)
//        （inc/hw/clk_mgr_internal.h L561-564）
static void test_should_set_clock_truth_table() {
    assert(dcn314_clk::shouldSetClock(false, 500, 400) == true);   // calc > cur
    assert(dcn314_clk::shouldSetClock(false, 400, 500) == false);
    assert(dcn314_clk::shouldSetClock(false, 500, 500) == false);
    assert(dcn314_clk::shouldSetClock(true, 400, 500) == true);    // safe_to_lower && calc < cur
    assert(dcn314_clk::shouldSetClock(true, 500, 400) == true);    // calc > cur
    assert(dcn314_clk::shouldSetClock(true, 500, 500) == false);
    assert(dcn314_clk::shouldSetClock(true, 400, 400) == false);
    std::puts("  [PASS] 1 shouldSetClock 真值表");
}

// ── 2. safe_to_lower = true 全流程 ────────────────────────────────────────
// 覆盖：zstate 下发（≠DISALLOW 且 ≠cur）、dtbclk 关闭、dcfclk/deep-sleep/dispclk 上调、
//       dppclk 下调（dppClockLowered=true）、钳位 100MHz。
// Linux dcn314_update_clocks L229-325 的顺序：
//   zstate(L232) → dtbclk(L237) → [pwr 因 displayCount=1 跳过] → dcfclk(L277)
//   → deep-sleep(L283) → [dppclk 仅记录 lowered 不下发] → dispclk(L307) → dppclk(L317)
static void test_update_clocks_safe_to_lower_full() {
    RegOp buf[64];
    RegSeq seq(buf, 64);

    const dcn314_clk::ClkMgrState cur = {600000, 400000, 300000, 300000,
                                         dcn314_clk::ZSTATE_UNKNOWN, dcn314_clk::PWR_UNKNOWN, true};
    const dcn314_clk::TargetClocks tgt = {700000, 450000, 100000, 400000,
                                          dcn314_clk::ZSTATE_ALLOWED, false};
    const dcn314_clk::ClkMgrConsts cst = {0, 1};   // minDispClkKhz=0, activeDisplayCount=1
    dcn314_clk::ClkMgrState next;

    const bool any = dcn314_clk::generateUpdateClocks(seq, kMb, cur, tgt, cst, true, &next);
    assert(any);
    assert(!seq.overflowed());
    assert(seq.size() == 36);   // 6 笔事务 × 6 op

    MsgTx tx[16];
    const size_t n = collectTx(seq, tx, 16);
    assert(n == 6);

    // 消息号顺序与参数（单位 MHz，经 khz_to_mhz_ceil）
    assert(tx[0].msgId == VBIOSSMC_MSG_AllowZstatesEntry);               // zstate (L232)
    assert(tx[0].param == 0x700);                                        // ALLOW: (1<<10)|(1<<9)|(1<<8)
    assert(tx[1].msgId == VBIOSSMC_MSG_SetDtbClk);                       // dtbclk 关 (L237)
    assert(tx[1].param == 0);
    assert(tx[2].msgId == VBIOSSMC_MSG_SetHardMinDcfclkByFreq);          // dcfclk (L277)
    assert(tx[2].param == 700);                                          // ceil(700000/1000)
    assert(tx[3].msgId == VBIOSSMC_MSG_SetMinDeepSleepDcfclk);           // deep sleep (L283)
    assert(tx[3].param == 450);
    assert(tx[4].msgId == VBIOSSMC_MSG_SetDispclkFreq);                  // dispclk (L307)
    assert(tx[4].param == 400);
    assert(tx[5].msgId == VBIOSSMC_MSG_SetDppclkFreq);                   // dppclk (L317, lowered)
    assert(tx[5].param == 100);                                          // 钳位后 100000 kHz

    // next 状态迁移（Linux 记账字段 L233/238/276/282/293/308）
    assert(next.zstateSupport == dcn314_clk::ZSTATE_ALLOWED);
    assert(next.dtbclkEn == false);
    assert(next.dcfclkKhz == 700000);
    assert(next.dcfclkDeepSleepKhz == 450000);
    assert(next.dppclkKhz == 100000);        // 钳位后的值（Linux L293 存的是钳位值）
    assert(next.dispclkKhz == 400000);
    assert(next.pwrState == dcn314_clk::PWR_UNKNOWN);   // displayCount=1 → 不进 LOW_POWER

    std::puts("  [PASS] 2 update_clocks safe_to_lower 全流程（消息号/参数/next 状态）");
}

// ── 3. safe_to_lower = false 全流程 ───────────────────────────────────────
// 覆盖：zstate 下发 DISALLOW、dtbclk 开启、idle optimization=0（mission mode）、
//       dppclk 上调（dppClockLowered=false → else 分支 L321）。
// Linux L253-325 的顺序：
//   zstate(L256) → dtbclk(L261) → pwr/idle(L269) → [dcfclk/deep 因 calc<cur 不下发]
//   → [dispclk 因 calc<cur 不下发] → dppclk(L321)
static void test_update_clocks_not_safe_to_lower() {
    RegOp buf[64];
    RegSeq seq(buf, 64);

    const dcn314_clk::ClkMgrState cur = {700000, 450000, 100000, 400000,
                                         dcn314_clk::ZSTATE_ALLOWED, dcn314_clk::PWR_LOW_POWER, false};
    const dcn314_clk::TargetClocks tgt = {600000, 400000, 300000, 300000,
                                          dcn314_clk::ZSTATE_DISALLOW, true};
    const dcn314_clk::ClkMgrConsts cst = {0, 1};
    dcn314_clk::ClkMgrState next;

    const bool any = dcn314_clk::generateUpdateClocks(seq, kMb, cur, tgt, cst, false, &next);
    assert(any);
    assert(!seq.overflowed());
    assert(seq.size() == 24);   // 4 笔事务

    MsgTx tx[16];
    const size_t n = collectTx(seq, tx, 16);
    assert(n == 4);

    assert(tx[0].msgId == VBIOSSMC_MSG_AllowZstatesEntry);      // zstate DISALLOW (L256)
    assert(tx[0].param == 0);
    assert(tx[1].msgId == VBIOSSMC_MSG_SetDtbClk);              // dtbclk 开 (L261)
    assert(tx[1].param == 1);
    assert(tx[2].msgId == VBIOSSMC_MSG_SetDisplayIdleOptimizations);  // idle=0 mission (L269)
    assert(tx[2].param == 0);
    assert(tx[3].msgId == VBIOSSMC_MSG_SetDppclkFreq);          // dppclk 上调 (L321)
    assert(tx[3].param == 300);

    assert(next.zstateSupport == dcn314_clk::ZSTATE_DISALLOW);
    assert(next.dtbclkEn == true);
    assert(next.pwrState == dcn314_clk::PWR_MISSION_MODE);
    assert(next.dcfclkKhz == 700000);        // safe_to_lower=false, calc<cur → 不下发不变
    assert(next.dcfclkDeepSleepKhz == 450000);
    assert(next.dppclkKhz == 300000);
    assert(next.dispclkKhz == 400000);

    std::puts("  [PASS] 3 update_clocks !safe_to_lower 全流程");
}

// ── 4. 低功耗 idle optimization 位域 ──────────────────────────────────────
// Linux L244-247：union display_idle_optimization_u idle_info = {0};
//   df_request_disabled=1 (bit0), phy_ref_clk_off=1 (bit1), s0i2_rdy=1 (bit2)
//   位域定义见 dcn314_smu.h L81-86 → data = 0x7
static void test_update_clocks_low_power_idle_opt() {
    RegOp buf[64];
    RegSeq seq(buf, 64);

    const dcn314_clk::ClkMgrState cur = {700000, 450000, 100000, 400000,
                                         dcn314_clk::ZSTATE_ALLOWED, dcn314_clk::PWR_MISSION_MODE, false};
    const dcn314_clk::TargetClocks tgt = {700000, 450000, 100000, 400000,
                                          dcn314_clk::ZSTATE_ALLOWED, false};
    const dcn314_clk::ClkMgrConsts cst = {0, 0};   // activeDisplayCount = 0
    dcn314_clk::ClkMgrState next;

    const bool any = dcn314_clk::generateUpdateClocks(seq, kMb, cur, tgt, cst, true, &next);
    assert(any);
    assert(seq.size() == 6);   // 仅 1 笔

    MsgTx tx[4];
    const size_t n = collectTx(seq, tx, 4);
    assert(n == 1);
    assert(tx[0].msgId == VBIOSSMC_MSG_SetDisplayIdleOptimizations);
    assert(tx[0].param == 0x7);   // df_request_disabled | phy_ref_clk_off | s0i2_rdy
    assert(next.pwrState == dcn314_clk::PWR_LOW_POWER);

    std::puts("  [PASS] 4 低功耗 idle optimization 位域（0x7）");
}

// ── 5. dppclk / dispclk 相对顺序（dppClockLowered 真/假）──────────────────
// Linux 顺序：dispclk 在下发块内 L307，dppclk 在 L317（lowered）或 L321（else）。
// 两种情况都必须是 dispclk 先、dppclk 后。
static void test_dppclk_dispclk_order() {
    // 场景 1：dppClockLowered = true（cur.dppclk > clamped tgt.dppclk）
    {
        RegOp buf[64];
        RegSeq seq(buf, 64);
        const dcn314_clk::ClkMgrState cur = {600000, 400000, 300000, 300000,
                                             dcn314_clk::ZSTATE_UNKNOWN, dcn314_clk::PWR_UNKNOWN, false};
        const dcn314_clk::TargetClocks tgt = {600000, 400000, 100000, 400000,
                                              dcn314_clk::ZSTATE_UNKNOWN, false};
        const dcn314_clk::ClkMgrConsts cst = {0, 1};
        dcn314_clk::ClkMgrState next;
        dcn314_clk::generateUpdateClocks(seq, kMb, cur, tgt, cst, true, &next);

        MsgTx tx[8];
        const size_t n = collectTx(seq, tx, 8);
        assert(n == 2);   // dispclk + dppclk（dcfclk/deep 不变，pwr 因 displayCount=1 跳过）
        assert(tx[0].msgId == VBIOSSMC_MSG_SetDispclkFreq);   // L307 先
        assert(tx[1].msgId == VBIOSSMC_MSG_SetDppclkFreq);    // L317 后（lowered）
        assert(next.dppclkKhz == 100000);
    }
    // 场景 2：dppClockLowered = false（cur.dppclk <= clamped tgt.dppclk）
    {
        RegOp buf[64];
        RegSeq seq(buf, 64);
        const dcn314_clk::ClkMgrState cur = {600000, 400000, 100000, 300000,
                                             dcn314_clk::ZSTATE_UNKNOWN, dcn314_clk::PWR_UNKNOWN, false};
        const dcn314_clk::TargetClocks tgt = {600000, 400000, 300000, 400000,
                                              dcn314_clk::ZSTATE_UNKNOWN, false};
        const dcn314_clk::ClkMgrConsts cst = {0, 1};
        dcn314_clk::ClkMgrState next;
        dcn314_clk::generateUpdateClocks(seq, kMb, cur, tgt, cst, true, &next);

        MsgTx tx[8];
        const size_t n = collectTx(seq, tx, 8);
        assert(n == 2);
        assert(tx[0].msgId == VBIOSSMC_MSG_SetDispclkFreq);   // L307 先
        assert(tx[1].msgId == VBIOSSMC_MSG_SetDppclkFreq);    // L321 后（updateDppclk）
        assert(next.dppclkKhz == 300000);
    }
    // 场景 3：dppClockLowered=false、仅 dispclk 变（updateDispclk=true）→ dppclk 也随之 (L321)
    {
        RegOp buf[64];
        RegSeq seq(buf, 64);
        const dcn314_clk::ClkMgrState cur = {600000, 400000, 300000, 300000,
                                             dcn314_clk::ZSTATE_UNKNOWN, dcn314_clk::PWR_UNKNOWN, false};
        const dcn314_clk::TargetClocks tgt = {600000, 400000, 300000, 400000,
                                              dcn314_clk::ZSTATE_UNKNOWN, false};
        const dcn314_clk::ClkMgrConsts cst = {0, 1};
        dcn314_clk::ClkMgrState next;
        dcn314_clk::generateUpdateClocks(seq, kMb, cur, tgt, cst, true, &next);

        MsgTx tx[8];
        const size_t n = collectTx(seq, tx, 8);
        assert(n == 2);
        assert(tx[0].msgId == VBIOSSMC_MSG_SetDispclkFreq);
        assert(tx[1].msgId == VBIOSSMC_MSG_SetDppclkFreq);   // updateDispclk → L321
    }
    std::puts("  [PASS] 5 dppclk/dispclk 相对顺序（lowered 真/假）");
}

// ── 6. 序列中不含 DMCUB 通知类操作 ────────────────────────────────────────
// Linux L327-337 的 DMUB_CMD__CLK_MGR_NOTIFY_CLOCKS 已按项目定论跳过
// （docs/DMCUB决策备忘录.md：苹果 DMCUB 通道对 780M 不可用）。
// 断言：任意 op 的地址都落在 VBIOSSMC 邮箱三地址之一，且总长是 6 的倍数。
static void test_no_dmub_ops() {
    RegOp buf[128];
    RegSeq seq(buf, 128);

    const dcn314_clk::ClkMgrState cur = {600000, 400000, 300000, 300000,
                                         dcn314_clk::ZSTATE_UNKNOWN, dcn314_clk::PWR_UNKNOWN, true};
    const dcn314_clk::TargetClocks tgt = {700000, 450000, 100000, 400000,
                                          dcn314_clk::ZSTATE_ALLOWED, false};
    const dcn314_clk::ClkMgrConsts cst = {0, 1};
    dcn314_clk::ClkMgrState next;
    dcn314_clk::generateUpdateClocks(seq, kMb, cur, tgt, cst, true, &next);
    dcn314_clk::generateUpdateClocks(seq, kMb, next, tgt, cst, false, &next);

    assert(!seq.overflowed());
    assert(seq.size() % 6 == 0);
    for (size_t i = 0; i < seq.size(); ++i) {
        const RegOp& op = seq[i];
        // VBIOSSMC 事务只碰这三个邮箱寄存器（Linux dcn314_smu.c 的 C2PMSG_67/83/91）
        const bool isMailbox = (op.addr == kMailbox67 || op.addr == kMailbox83 || op.addr == kMailbox91);
        assert(isMailbox);
    }
    std::puts("  [PASS] 6 序列不含 DMCUB 通知类操作");
}

// ── 7. zstate 两个分支 ─────────────────────────────────────────────────────
// safe_to_lower=true：tgt ≠ DISALLOW 且 ≠ cur → 下发 tgt（Linux L230-234）
// safe_to_lower=false：tgt == DISALLOW 且 ≠ cur → 下发 DISALLOW（Linux L254-258）
// 相等时不重复下发（Linux 的记账比较）。
static void test_zstate_branches() {
    // 分支 1：safe_to_lower=true
    {
        RegOp buf[64];
        RegSeq seq(buf, 64);
        const dcn314_clk::ClkMgrState cur = {0, 0, 100000, 0,
                                             dcn314_clk::ZSTATE_DISALLOW, dcn314_clk::PWR_UNKNOWN, false};
        const dcn314_clk::TargetClocks tgt = {0, 0, 100000, 0,
                                              dcn314_clk::ZSTATE_ALLOWED, false};
        const dcn314_clk::ClkMgrConsts cst = {0, 1};
        dcn314_clk::ClkMgrState next;
        dcn314_clk::generateUpdateClocks(seq, kMb, cur, tgt, cst, true, &next);

        MsgTx tx[2];
        const size_t n = collectTx(seq, tx, 2);
        assert(n == 1);
        assert(tx[0].msgId == VBIOSSMC_MSG_AllowZstatesEntry);
        assert(tx[0].param == 0x700);   // ALLOW: (1<<10)|(1<<9)|(1<<8)（dcn314_smu.c L350）
        assert(next.zstateSupport == dcn314_clk::ZSTATE_ALLOWED);
    }
    // 分支 2：safe_to_lower=false
    {
        RegOp buf[64];
        RegSeq seq(buf, 64);
        const dcn314_clk::ClkMgrState cur = {0, 0, 100000, 0,
                                             dcn314_clk::ZSTATE_ALLOWED, dcn314_clk::PWR_MISSION_MODE, false};
        const dcn314_clk::TargetClocks tgt = {0, 0, 100000, 0,
                                              dcn314_clk::ZSTATE_DISALLOW, false};
        const dcn314_clk::ClkMgrConsts cst = {0, 1};
        dcn314_clk::ClkMgrState next;
        dcn314_clk::generateUpdateClocks(seq, kMb, cur, tgt, cst, false, &next);

        MsgTx tx[2];
        const size_t n = collectTx(seq, tx, 2);
        assert(n == 1);
        assert(tx[0].msgId == VBIOSSMC_MSG_AllowZstatesEntry);
        assert(tx[0].param == 0);   // DISALLOW（dcn314_smu.c L355）
        assert(next.zstateSupport == dcn314_clk::ZSTATE_DISALLOW);
    }
    // 不变时不重复下发
    {
        RegOp buf[64];
        RegSeq seq(buf, 64);
        const dcn314_clk::ClkMgrState cur = {0, 0, 100000, 0,
                                             dcn314_clk::ZSTATE_ALLOWED, dcn314_clk::PWR_LOW_POWER, false};
        const dcn314_clk::TargetClocks tgt = {0, 0, 100000, 0,
                                              dcn314_clk::ZSTATE_ALLOWED, false};
        const dcn314_clk::ClkMgrConsts cst = {0, 1};
        dcn314_clk::ClkMgrState next;
        const bool any = dcn314_clk::generateUpdateClocks(seq, kMb, cur, tgt, cst, true, &next);
        assert(!any);
        assert(seq.size() == 0);
    }
    std::puts("  [PASS] 7 zstate 两个分支");
}

// ── 8. minDispClkKhz 钳位 ──────────────────────────────────────────────────
// Linux L303-305：dc->debug.min_disp_clk_khz > 0 且请求 < 下限 → 用下限。
// 注意 L308 记账用的是**原始请求值**（非钳位值）。
static void test_min_disp_clk_clamp() {
    RegOp buf[64];
    RegSeq seq(buf, 64);

    const dcn314_clk::ClkMgrState cur = {0, 0, 100000, 200000,
                                         dcn314_clk::ZSTATE_DISALLOW, dcn314_clk::PWR_UNKNOWN, false};
    const dcn314_clk::TargetClocks tgt = {0, 0, 100000, 150000,
                                          dcn314_clk::ZSTATE_ALLOWED, false};
    const dcn314_clk::ClkMgrConsts cst = {300000, 1};   // minDispClkKhz=300000
    dcn314_clk::ClkMgrState next;

    dcn314_clk::generateUpdateClocks(seq, kMb, cur, tgt, cst, true, &next);

    MsgTx tx[8];
    const size_t n = collectTx(seq, tx, 8);
    assert(n == 3);   // zstate + dispclk + dppclk（updateDispclk → L321）
    assert(tx[0].msgId == VBIOSSMC_MSG_AllowZstatesEntry);
    assert(tx[1].msgId == VBIOSSMC_MSG_SetDispclkFreq);
    assert(tx[1].param == 300);                     // 150000 被钳到 300000 → 300 MHz
    assert(tx[2].msgId == VBIOSSMC_MSG_SetDppclkFreq);
    assert(next.dispclkKhz == 150000);              // 记账用原始请求值（L308）

    std::puts("  [PASS] 8 minDispClkKhz 钳位");
}

// ── 9. 读寄存器序列 ────────────────────────────────────────────────────────
// Linux：dcn314_is_spll_ssc_enabled（L177-185）读 CLK6_0_CLK6_spll_field_8=0x464b（L98）；
//        dcn314_read_ss_info_from_lut（L779-795）读 CLK1_CLK2_BYPASS_CNTL=0x029c（L90）。
static void test_read_helpers() {
    RegOp buf[4];
    RegSeq seq(buf, 4);
    dcn314_clk::generateReadSpllSscEnabled(seq, 0x464b);
    assert(seq.size() == 1);
    assert(seq[0].kind == RegOp::Kind::Read);
    assert(seq[0].addr == 0x464b);

    RegSeq seq2(buf, 4);
    dcn314_clk::generateReadSsInfoClockSource(seq2, 0x029c);
    assert(seq2.size() == 1);
    assert(seq2[0].kind == RegOp::Kind::Read);
    assert(seq2[0].addr == 0x029c);

    std::puts("  [PASS] 9 读寄存器序列（spll_ssc / clock_source）");
}

// ── 10. ss_info_table 查表 ─────────────────────────────────────────────────
// Linux dcn314_clk_mgr.c L492-495：
//   .ss_divider = 1000, .ss_percentage = {0, 0, 375, 375, 375}
static void test_ss_info_table() {
    assert(dcn314_clk::kSsDivider == 1000);
    assert(dcn314_clk::ssPercentageFor(0) == 0);
    assert(dcn314_clk::ssPercentageFor(1) == 0);
    assert(dcn314_clk::ssPercentageFor(2) == 375);
    assert(dcn314_clk::ssPercentageFor(3) == 375);
    assert(dcn314_clk::ssPercentageFor(4) == 375);
    assert(dcn314_clk::ssPercentageFor(5) == 0);   // 越界（Linux 有 ARRAY_SIZE 守卫 L787）
    std::puts("  [PASS] 10 ss_info_table 查表");
}

// ── 11. adjustDpRefFreqForSs ───────────────────────────────────────────────
// Linux dce_adjust_dp_ref_freq_for_ss（dce100/dce_clk_mgr.c L100-113）：
//   adjusted = floor(dp_ref_clk_khz * (1 - ss_pct/(ss_div*200)))，fixed31_32。
// 用 64 位整数复现，结果应与定点一致。
static void test_adjust_dp_ref_freq_for_ss() {
    // ss 关闭 / 除数为 0 → 原值（Linux L102 条件不成立）
    assert(dcn314_clk::adjustDpRefFreqForSs(594000, 0, 1000) == 594000);
    assert(dcn314_clk::adjustDpRefFreqForSs(594000, 375, 0) == 594000);
    // 375/1000/200：594000 * (1 - 0.001875) = 592886.25 → floor 592886
    assert(dcn314_clk::adjustDpRefFreqForSs(594000, 375, 1000) == 592886);
    // 整除情形：1000000 * 0.998125 = 998125.0
    assert(dcn314_clk::adjustDpRefFreqForSs(1000000, 375, 1000) == 998125);
    std::puts("  [PASS] 11 adjustDpRefFreqForSs（整数复现 fixed31_32）");
}

// ── 12. initClocksState ────────────────────────────────────────────────────
// Linux dcn314_init_clocks（L187-206）：memset 清零 → pwr/zstate 置 UNKNOWN →
//   spll_ssc 开启时 dp_dto_source = adjust(..., L201-203)，否则 = dprefclk（L204-205）。
static void test_init_clocks_state() {
    uint32_t dto = 0;
    const dcn314_clk::ClkMgrState s1 = dcn314_clk::initClocksState(0, false, 594000, 375, 1000, &dto);
    assert(dto == 594000);   // ssc 关闭 → 原值
    assert(s1.pwrState == dcn314_clk::PWR_UNKNOWN);
    assert(s1.zstateSupport == dcn314_clk::ZSTATE_UNKNOWN);
    assert(s1.dcfclkKhz == 0);            // memset 清零
    assert(s1.dcfclkDeepSleepKhz == 0);
    assert(s1.dppclkKhz == 0);
    assert(s1.dispclkKhz == 0);
    assert(s1.dtbclkEn == false);

    uint32_t dto2 = 0;
    dcn314_clk::initClocksState(0, true, 594000, 375, 1000, &dto2);
    assert(dto2 == 592886);   // ssc 开启 → adjustDpRefFreqForSs

    // next 指针可为 nullptr
    const dcn314_clk::ClkMgrState s3 = dcn314_clk::initClocksState(0, false, 594000, 375, 1000, nullptr);
    assert(s3.pwrState == dcn314_clk::PWR_UNKNOWN);

    std::puts("  [PASS] 12 initClocksState");
}

int main() {
    std::puts("── 第五步任务 B 测试：DCN314 时钟主流程 ──");
    test_should_set_clock_truth_table();
    test_update_clocks_safe_to_lower_full();
    test_update_clocks_not_safe_to_lower();
    test_update_clocks_low_power_idle_opt();
    test_dppclk_dispclk_order();
    test_no_dmub_ops();
    test_zstate_branches();
    test_min_disp_clk_clamp();
    test_read_helpers();
    test_ss_info_table();
    test_adjust_dp_ref_freq_for_ss();
    test_init_clocks_state();
    std::puts("全部通过。");
    return 0;
}
