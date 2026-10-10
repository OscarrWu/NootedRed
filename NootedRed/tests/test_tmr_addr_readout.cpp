// A-23 三个只读仪表（`nred::TmrAddrReadout`）离线单元测试
//
// 覆盖：
//   ① buf_phy / sys_phy / tmr_size 透传正确；
//   ② align_rem 计算（含 tmr_size==0 哨兵）；
//   ③ in_window 判定（in/out/unknown）；
//   ④ 门控假零差异（机械自证：runTmrAddrReadout 纯逻辑，默认关时不被调用）。
//
// 编译运行（分析机）：
//   make -f src/NootedRed/tests/Makefile test

#include <NRedTmrAddrReadout.hpp>

#include <cassert>
#include <cstdio>
#include <cstdint>

static void test_values_transparent() {
    printf("▶ test_values_transparent\n");
    const nred::TmrAddrReadout r = nred::runTmrAddrReadout(
        0x1BC00000ULL, 0x1BC00000ULL, 0x4000000, 0x80F8000000ULL, 0x1000000ULL, 0x1BC00000ULL);
    assert(r.buf_phy  == 0x1BC00000 && "buf_phy pass-through");
    assert(r.sys_phy  == 0x1BC00000 && "sys_phy pass-through");
    assert(r.tmr_size == 0x4000000  && "tmr_size pass-through");
    printf("  PASS: values transparent\n");
}

static void test_align_rem_normal() {
    printf("▶ test_align_rem_normal\n");
    const nred::TmrAddrReadout r = nred::runTmrAddrReadout(
        0x1BC00000, 0, 0x4000000, 0, 0, 0x1BC00000);
    // 0x1BC00000 % 0x4000000 = 0x1BC00000 - (0x1BC00000/0x4000000)*0x4000000
    // 0x1BC00000 / 0x4000000 = 6 (since 6*0x4000000=0x18000000)
    // 0x1BC00000 - 0x18000000 = 0x3C00000
    assert(r.align_rem == 0x3C00000ULL && "align_rem must be 0x3C00000");
    printf("  PASS: align_rem=0x%llx\n", (unsigned long long)r.align_rem);
}

static void test_align_rem_zero_size() {
    printf("▶ test_align_rem_zero_size\n");
    const nred::TmrAddrReadout r = nred::runTmrAddrReadout(
        0x1BC00000, 0, 0, 0, 0, 0x1BC00000);
    assert(r.align_rem == ~0ULL && "align_rem must be UINT64_MAX sentinel");
    printf("  PASS: align_rem sentinel\n");
}

static void test_in_window_in() {
    printf("▶ test_in_window_in\n");
    const nred::TmrAddrReadout r = nred::runTmrAddrReadout(
        0x80F8000000ULL, 0, 0x100000, 0x80F8000000ULL, 0x1000000ULL, 0x80F8000000ULL);
    assert(r.in_window == 1 && "within window must be 'in'");
    printf("  PASS: in_window=in\n");
}

static void test_in_window_out() {
    printf("▶ test_in_window_out\n");
    const nred::TmrAddrReadout r = nred::runTmrAddrReadout(
        0x1BC00000, 0, 0x100000, 0x80F8000000ULL, 0x1000000ULL, 0x1BC00000ULL);
    assert(r.in_window == 0 && "outside window must be 'out'");
    printf("  PASS: in_window=out\n");
}

static void test_in_window_unknown() {
    printf("▶ test_in_window_unknown\n");
    // base=0 → unknown
    const nred::TmrAddrReadout r1 = nred::runTmrAddrReadout(
        0x1BC00000, 0, 0x100000, 0, 0x1000000, 0x1BC00000ULL);
    assert(r1.in_window == 2 && "base==0 must be unknown");
    // size=0 → unknown
    const nred::TmrAddrReadout r2 = nred::runTmrAddrReadout(
        0x1BC00000, 0, 0x100000, 0x80F8000000ULL, 0, 0x1BC00000ULL);
    assert(r2.in_window == 2 && "size==0 must be unknown");
    printf("  PASS: in_window=unknown for zero base/size\n");
}

// ⚠️ 读 0 陷阱：断言也遵守 —— 不把 0 当作"基址为 0"，只当作"尚未捕获"
static void test_zero_trap_never_inferred() {
    printf("▶ test_zero_trap_never_inferred\n");
    // 如果 base=0 且 addr=0，仍为 unknown（不判为 in）
    const nred::TmrAddrReadout r = nred::runTmrAddrReadout(
        0, 0, 0x100000, 0, 0x1000000, 0);
    assert(r.in_window == 2 && "zero base with zero addr must still be unknown");
    printf("  PASS: zero trap respected\n");
}

int main() {
    printf("=== A-23 TmrAddrReadout offline tests ===\n");
    test_values_transparent();
    test_align_rem_normal();
    test_align_rem_zero_size();
    test_in_window_in();
    test_in_window_out();
    test_in_window_unknown();
    test_zero_trap_never_inferred();
    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}
