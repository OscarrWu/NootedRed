// A-20 早期落盘安全前置检查（`nred::TraceSafe`）离线单元测试
//
// 验收要求（任务卡 A-20）：
//   ① 未就绪（rootvnode == nullptr）⇒ 跳过落盘（计数累加，返回 false）；
//   ② 就绪（rootvnode != nullptr）⇒ 可写（返回 true，written 累加）；
//   ③ n <= 0 ⇒ 跳过（无论就绪与否）；
//   ④ 丢弃/写入计数准确（事后可知丢了多少行）。
//
// 编译运行（分析机）：
//   make -f src/NootedRed/tests/Makefile test

#include <NRedTraceSafe.hpp>

#include <cassert>
#include <cstdio>

static void test_not_ready_skips() {
    printf("▶ test_not_ready_skips\n");
    nred::TraceSafe s;
    // 根 FS 未就绪 ⇒ 跳过、不重试
    assert(s.shouldWrite(false, 10) == false && "must skip when rootvnode not ready");
    assert(s.dropped == 1 && "dropped must be 1");
    assert(s.written == 0 && "written must be 0");
    // 连续多次未就绪 ⇒ 丢弃计数累加
    assert(s.shouldWrite(false, 10) == false);
    assert(s.shouldWrite(false, 20) == false);
    assert(s.dropped == 3 && "dropped must accumulate to 3");
    assert(s.written == 0);
    printf("  PASS: skip when not ready, dropped=%u\n", s.dropped);
}

static void test_ready_writes() {
    printf("▶ test_ready_writes\n");
    nred::TraceSafe s;
    // 根 FS 就绪 ⇒ 可写
    assert(s.shouldWrite(true, 10) == true && "must allow write when ready");
    assert(s.written == 1 && "written must be 1");
    assert(s.dropped == 0);
    assert(s.shouldWrite(true, 20) == true);
    assert(s.written == 2 && "written must accumulate to 2");
    assert(s.dropped == 0);
    printf("  PASS: write when ready, written=%u\n", s.written);
}

static void test_n_le_zero_skips() {
    printf("▶ test_n_le_zero_skips\n");
    nred::TraceSafe s;
    // n <= 0 ⇒ 跳过（无论就绪与否）
    assert(s.shouldWrite(true, 0) == false && "n==0 must skip");
    assert(s.shouldWrite(true, -1) == false && "n<0 must skip");
    assert(s.dropped == 2 && "dropped must be 2");
    assert(s.written == 0);
    printf("  PASS: skip when n<=0, dropped=%u\n", s.dropped);
}

static void test_mixed_counts() {
    printf("▶ test_mixed_counts\n");
    nred::TraceSafe s;
    // 混合：先未就绪（丢 2），后就绪（写 3）
    s.shouldWrite(false, 10);
    s.shouldWrite(false, 10);
    s.shouldWrite(true, 10);
    s.shouldWrite(true, 10);
    s.shouldWrite(true, 10);
    assert(s.dropped == 2 && "dropped must be 2");
    assert(s.written == 3 && "written must be 3");
    printf("  PASS: mixed counts dropped=%u written=%u\n", s.dropped, s.written);
}

int main() {
    printf("=== A-20 trace safe offline tests ===\n");
    test_not_ready_skips();
    test_ready_writes();
    test_n_le_zero_skips();
    test_mixed_counts();
    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}
