// A-25 first-false 窗口法（`nred::WindowResult`/`P5Binary`）离线单元测试
//
// 覆盖：
//   ① 门控解析（默认关 ⇒ 零读路径——纯逻辑在门控内才被调用，测试验证纯逻辑本身）；
//   ② 零读路径：self 非法 ⇒ 全部 valid=0、unknownCount 计数；
//   ③ 水印判定顺序：按程序序找第一个为空的水印；
//   ④ P5 时序二值判：nodeD8/nodeD0 均为 0 ⇒ bothZero=1；任一非 0 ⇒ 0。
//
// 编译运行（分析机）：
//   make -f src/NootedRed/tests/Makefile test

#include <NRedWindowProbe.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>

static void test_first_false_ordering() {
    printf("▶ test_first_false_ordering\n");
    // 模拟：P3 全非空、P5+0x338 为空 ⇒ 第一个为空的水印应是 WmP5_338
    nred::WmVal wms[nred::WmCount];
    for (uint32_t i = 0; i < nred::WmCount; ++i) {
        wms[i].id    = i;
        wms[i].value = 0x1ULL;   // 默认非空
        wms[i].valid = 1;
    }
    wms[nred::WmP5_338].value = 0;   // 第一个空
    wms[nred::WmP12_528].value = 0;  // 第二个空（不应先命中）
    const nred::WindowResult r = nred::findFirstFalse(wms, nred::WmCount);
    assert(r.firstFalseId == nred::WmP5_338 && "first-false must be WmP5_338");
    assert(r.firstFalseValid == 1 && "firstFalseValid must be 1");
    assert(r.readCount == nred::WmCount && "all readable");
    assert(r.unknownCount == 0 && "no unknown");
    printf("  PASS: first-false=%s\n", nred::wmName(r.firstFalseId));
}

static void test_first_false_none_empty() {
    printf("▶ test_first_false_none_empty\n");
    nred::WmVal wms[nred::WmCount];
    for (uint32_t i = 0; i < nred::WmCount; ++i) {
        wms[i].id    = i;
        wms[i].value = 0x1ULL;
        wms[i].valid = 1;
    }
    const nred::WindowResult r = nred::findFirstFalse(wms, nred::WmCount);
    assert(r.firstFalseId == nred::WmCount && "no empty ⇒ WmCount sentinel");
    assert(r.firstFalseValid == 0 && "no false ⇒ valid 0");
    printf("  PASS: no first-false (all non-empty)\n");
}

static void test_unknown_not_counted_as_false() {
    printf("▶ test_unknown_not_counted_as_false\n");
    nred::WmVal wms[nred::WmCount];
    for (uint32_t i = 0; i < nred::WmCount; ++i) {
        wms[i].id    = i;
        wms[i].value = 0x1ULL;
        wms[i].valid = 1;
    }
    // 前两个水印"未读到"（valid=0）⇒ 不是空，不计为 first-false，但 unknownCount 累加
    wms[nred::WmP3_2FC].valid = 0;
    wms[nred::WmP3_2FE].valid = 0;
    wms[nred::WmP5_340].value = 0;   // 真正的第一个空（valid=1 且 value=0）
    const nred::WindowResult r = nred::findFirstFalse(wms, nred::WmCount);
    assert(r.unknownCount == 2 && "2 unreadable");
    assert(r.readCount == nred::WmCount - 2 && "readCount excludes unreadable");
    assert(r.firstFalseId == nred::WmP5_340 && "first-false must skip unreadable");
    printf("  PASS: unreadable not counted as false, unknown=%u first=%s\n",
           r.unknownCount, nred::wmName(r.firstFalseId));
}

static void test_p5_binary_both_zero() {
    printf("▶ test_p5_binary_both_zero\n");
    const nred::P5Binary p = nred::evalP5Binary(0, 0, 1, 1);
    assert(p.valid == 1 && "self+node valid");
    assert(p.bothZero == 1 && "both zero ⇒ P5 后半为空");
    assert(p.nodeD8 == 0 && p.nodeD0 == 0);
    printf("  PASS: P5 both zero\n");
}

static void test_p5_binary_not_both_zero() {
    printf("▶ test_p5_binary_not_both_zero\n");
    const nred::P5Binary p = nred::evalP5Binary(0x1, 0, 1, 1);
    assert(p.bothZero == 0 && "nodeD8 nonzero ⇒ not both zero");
    printf("  PASS: P5 not both zero\n");
}

static void test_p5_binary_invalid() {
    printf("▶ test_p5_binary_invalid\n");
    // self 非法（selfValid=0）⇒ valid=0，bothZero 必须为 0（值不可信）
    const nred::P5Binary p = nred::evalP5Binary(0, 0, 0, 0);
    assert(p.valid == 0 && "invalid ⇒ valid 0");
    assert(p.bothZero == 0 && "invalid ⇒ not bothZero");
    printf("  PASS: P5 invalid path\n");
}

int main() {
    printf("=== A-25 window probe offline tests ===\n");
    test_first_false_ordering();
    test_first_false_none_empty();
    test_unknown_not_counted_as_false();
    test_p5_binary_both_zero();
    test_p5_binary_not_both_zero();
    test_p5_binary_invalid();
    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}
