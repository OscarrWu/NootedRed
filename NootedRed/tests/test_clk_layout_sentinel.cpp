// A-27（甲）线读数：哨兵判定（`fw::clkLayoutSentinel`）离线单元测试
//
// 覆盖（A-15 方案 A 的双向自证设计）：
//   ① 门控解析——本卡门控在 kext 侧（`checkKernelArgument("-NRedClkLayoutReadout")`），
//      离线验证**纯逻辑**哨兵本身（门控假的机械自证见报告：不调用 ⇒ 零输出）。
//   ② 零读路径：`createCalls == 0` ⇒ sentinel=1（not-called，函数未被调用）。
//   ③ 已调用但空：`createCalls > 0 && clkMgr == 0` ⇒ sentinel=2（called-but-null）。
//   ④ 可读：`createCalls > 0 && clkMgr != 0` ⇒ sentinel=0（readable）。
//
// ⚠️ 关键纪律：sentinel=1（未被调用）**绝不可**当作"读数为 0/失败"——
//   这是 A-15 诊断出的核心混淆（"函数未执行" vs "执行了但读不到"）。
//
// 编译运行（分析机）：
//   make -f src/NootedRed/tests/Makefile test

#define FW_CLK_LAYOUT_NO_KEXT   // 只编译纯逻辑部分（排除 kext 接线）
#include <FwBringup/NRedClkLayoutReadout.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>

static void test_sentinel_not_called() {
    printf("▶ test_sentinel_not_called\n");
    // dc_clk_mgr_create 从未被调用 ⇒ sentinel=1
    assert(fw::clkLayoutSentinel(0, 0) == 1 && "calls==0 ⇒ not-called");
    assert(fw::clkLayoutSentinel(0, 0x1234) == 1 && "calls==0 ⇒ not-called even if ptr set");
    assert(strcmp(fw::clkLayoutSentinelText(1), "not-called") == 0);
    printf("  PASS: sentinel=not-called\n");
}

static void test_sentinel_called_but_null() {
    printf("▶ test_sentinel_called_but_null\n");
    // 已调用但返回空 ⇒ sentinel=2（已调用，但 clk_mgr 为空）
    assert(fw::clkLayoutSentinel(1, 0) == 2 && "called but clk_mgr==0 ⇒ called-but-null");
    assert(fw::clkLayoutSentinel(5, 0) == 2 && "多次调用仍为空 ⇒ called-but-null");
    assert(strcmp(fw::clkLayoutSentinelText(2), "called-but-null") == 0);
    printf("  PASS: sentinel=called-but-null\n");
}

static void test_sentinel_readable() {
    printf("▶ test_sentinel_readable\n");
    // 已调用且 clk_mgr 非空 ⇒ sentinel=0（可读，读数可信）
    assert(fw::clkLayoutSentinel(1, 0xffffff8000000000ULL) == 0 && "readable");
    assert(strcmp(fw::clkLayoutSentinelText(0), "readable") == 0);
    printf("  PASS: sentinel=readable\n");
}

static void test_sentinel_unknown_code() {
    printf("▶ test_sentinel_unknown_code\n");
    // 非法哨兵码 ⇒ 文案 "?"，且不得被当作 readable
    assert(strcmp(fw::clkLayoutSentinelText(99), "?") == 0 && "unknown ⇒ ?");
    printf("  PASS: unknown sentinel text\n");
}

// 双向自证（机械）：门控假 ⇒ 零输出。此处以"不调用即无输出"为逻辑等价验证：
//   runClkLayoutReadout 是纯函数，仅在被调用时产生读数 ⇒ 不调用 ⇒ 零读数。
static void test_zero_read_path() {
    printf("▶ test_zero_read_path\n");
    // 门控假路径下不会调用 runClkLayoutReadout；此处验证其"可被调用但无副作用"的纯逻辑性质：
    //  以全 0 输入调用 ⇒ 各 valid 均为 0（零读），且不崩溃。
    const fw::ClkLayoutReadout r = fw::runClkLayoutReadout(0, 0, 0, 0, 0, nullptr, nullptr, nullptr);
    assert(r.bwValid == 0 && r.ppValid == 0 && r.ctxValid == 0 && "all-zero input ⇒ all invalid");
    assert(r.bwNonZeroDwords == 0 && "no bw dwords");
    assert(r.overlayCalls == 0);
    printf("  PASS: zero-read path (no crash, all invalid)\n");
}

int main() {
    printf("=== A-27 clk layout sentinel offline tests ===\n");
    test_sentinel_not_called();
    test_sentinel_called_but_null();
    test_sentinel_readable();
    test_sentinel_unknown_code();
    test_zero_read_path();
    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}
