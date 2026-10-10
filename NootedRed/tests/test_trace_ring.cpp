// A-22 早期 trace 内存环形缓冲（`nred::TraceRing`）离线单元测试
//
// 验收要求（任务卡 A-22）：
//   ① 未就绪/非法输入 ⇒ 丢弃（n<=0、nullptr）；
//   ② 就绪 ⇒ 入缓冲（written 累加）；
//   ③ 溢出（超过 kLineMax）⇒ 覆盖最旧、overflow 累加；
//   ④ 刷盘取行：按序（最旧→最新）、长度正确、reset 后清空。
//
// 编译运行（分析机）：
//   make -f src/NootedRed/tests/Makefile test

#include <NRedTraceRing.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>

static void test_invalid_input_dropped() {
    printf("▶ test_invalid_input_dropped\n");
    nred::TraceRing r;
    // n <= 0 或 nullptr ⇒ 丢弃
    assert(r.push("hello", 0) == false && "n==0 must drop");
    assert(r.push("hello", -1) == false && "n<0 must drop");
    assert(r.push(nullptr, 5) == false && "nullptr must drop");
    assert(r.dropped == 3 && "dropped must be 3");
    assert(r.written == 0 && "written must be 0");
    assert(r.count == 0);
    printf("  PASS: invalid input dropped=%u\n", r.dropped);
}

static void test_push_and_get() {
    printf("▶ test_push_and_get\n");
    nred::TraceRing r;
    const char* l0 = "tmrInit: loadToc ok tmr_size=0x4000000\n";
    const char* l1 = "tmrLoad: submit tmr_mc=0x1BC00000 tmr_size=0x4000000\n";
    assert(r.push(l0, static_cast<int>(strlen(l0))) == true);
    assert(r.push(l1, static_cast<int>(strlen(l1))) == true);
    assert(r.count == 2 && "count must be 2");
    assert(r.written == 2 && "written must be 2");
    // 按序取（最旧→最新）
    const char* got = nullptr;
    uint32_t len = r.get(0, &got);
    assert(len == strlen(l0) - 1 && "len excludes newline");   // 不含换行
    assert(got != nullptr && memcmp(got, l0, len) == 0 && "line0 mismatch");
    len = r.get(1, &got);
    assert(len == strlen(l1) - 1);
    assert(got != nullptr && memcmp(got, l1, len) == 0 && "line1 mismatch");
    // 越界 ⇒ 0
    assert(r.get(2, &got) == 0 && "out-of-range must be 0");
    printf("  PASS: push+get ordered, count=%u written=%u\n", r.count, r.written);
}

static void test_overflow_overwrites_oldest() {
    printf("▶ test_overflow_overwrites_oldest\n");
    nred::TraceRing r;
    const uint32_t cap = nred::TraceRing::kLineMax;
    // 填满 + 再推 3 行 ⇒ 溢出 3 次，count 保持 cap
    for (uint32_t i = 0; i < cap + 3; ++i) {
        char b[32];
        const int n = snprintf(b, sizeof(b), "line-%03u\n", i);
        r.push(b, n);
    }
    assert(r.count == cap && "count must stay at cap");
    assert(r.overflow == 3 && "overflow must be 3");
    assert(r.written == cap + 3 && "written must accumulate all pushes");
    // 最旧应为 line-003（被覆盖 3 行后）：最旧 = (0+3) % cap
    const char* got = nullptr;
    const uint32_t len = r.get(0, &got);
    assert(len == 8 && "len of 'line-003' is 8");
    assert(got != nullptr && memcmp(got, "line-003", 8) == 0 && "oldest must be line-003");
    printf("  PASS: overflow=%u count=%u oldest=line-003\n", r.overflow, r.count);
}

static void test_reset() {
    printf("▶ test_reset\n");
    nred::TraceRing r;
    const char* l = "x\n";
    r.push(l, 2);
    assert(r.count == 1);
    r.reset();
    assert(r.count == 0 && "count must be 0 after reset");
    assert(r.head == 0 && "head must be 0 after reset");
    printf("  PASS: reset clears count/head\n");
}

int main() {
    printf("=== A-22 trace ring offline tests ===\n");
    test_invalid_input_dropped();
    test_push_and_get();
    test_overflow_overwrites_oldest();
    test_reset();
    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}
