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

// ── A-34：位掩码读法（`nred::maskBitAt`）——端到端 + 阴性对照 + 规格一致性 ──────────
//  合成 buffer 内的 `[0x1E88, 0x1E8C)` 4 字节即 `accel+0x1e88` 的条件结果位掩码区。
//  换算规则（`kb/技术草案/水印与25条对齐.md` §5.2）：位 n ⇔ byte `0x1e88+(n>>3)` 的第 `n&7` 位。
//  读法**唯一实现**在 `NRedWindowProbe.hpp`（`X5000.cpp` 快照时直接调用）⇒ 本用例与生产读法同源。
static uint8_t gMaskBuf[0x1E90];

static void test_mask_bit_read_p7_faithful() {
    printf("▶ test_mask_bit_read_p7_faithful\n");
    memset(gMaskBuf, 0, sizeof(gMaskBuf));
    gMaskBuf[0x1E88] = 0xFF;   // byte0 全 1（干扰位）
    gMaskBuf[0x1E89] = 0x40;   // byte 0x1e89 的 bit6 ⇒ 位 14（P7 → bit14）
    assert(nred::maskBitAt(gMaskBuf, 14) == 1 && "bit14 must be byte 0x1e89 bit6");
    printf("  PASS: bit(14)=1 from byte 0x1e89 bit6\n");
}

static void test_mask_bit_read_neg_control() {
    printf("▶ test_mask_bit_read_neg_control\n");
    // 阴性对照（复现 B12 轮的读法缺陷）：byte0=0xFF、byte1=0x00 ⇒ 位 14 必为 0。
    //  若读法退回"dword 读 + maskBit(n)"：`maskBit(14)=1<<6` 落在 dword 上 ⇒ 实取 byte0 的
    //  bit6（=1）⇒ 本断言**失败**。故本用例是"改回 bug 版即失败"的机械演示。
    memset(gMaskBuf, 0, sizeof(gMaskBuf));
    gMaskBuf[0x1E88] = 0xFF;
    gMaskBuf[0x1E89] = 0x00;
    assert(nred::maskBitAt(gMaskBuf, 14) == 0 && "byte 0x1e89==0 ⇒ bit14 must be 0 (bug 版会误报 1)");
    printf("  PASS: bit(14)=0 (阴性对照)\n");
}

static void test_mask_bit_spec_consistency() {
    printf("▶ test_mask_bit_spec_consistency\n");
    // 规格一致性：n=0..31 逐位与 §5.2 的字面规则对照。字面规则在**本用例内独立写出**
    //  （不复用 `maskBit`/`maskByteOff`）⇒ 非"自洽型假测试"。
    memset(gMaskBuf, 0, sizeof(gMaskBuf));
    gMaskBuf[0x1E88] = 0xA6;
    gMaskBuf[0x1E89] = 0x5B;
    gMaskBuf[0x1E8A] = 0xC3;
    gMaskBuf[0x1E8B] = 0x01;
    for (uint32_t n = 0; n < 32; ++n) {
        const uint8_t  b    = gMaskBuf[0x1E88 + (n >> 3)];
        const uint64_t want = (b >> (n & 7u)) & 1u;
        assert(nred::maskBitAt(gMaskBuf, n) == want && "byte-wise read must match §5.2");
    }
    printf("  PASS: n=0..31 全部与 §5.2 换算规则一致\n");
}

// ── A-41 对照 ①：P13（bit18 无忠实位）不得抢占 firstFalse ──────────────────────────
static void test_p13_not_first_false() {
    printf("▶ test_p13_not_first_false\n");
    nred::SentryVal vals[nred::SentryId::SentryCount];
    for (uint32_t i = 0; i < nred::SentryId::SentryCount; ++i) {
        vals[i].id    = i;
        vals[i].value = 1;
        vals[i].valid = 1;
    }
    vals[nred::SentryId::P13_Unreliable].value = 0;   // 唯一为 0 的项，但无忠实位 ⇒ 不得报
    const nred::WindowResult r = nred::findFirstFalseSentry(vals, nred::SentryId::SentryCount);
    assert(r.firstFalseId != nred::SentryId::P13_Unreliable && "P13 must not be firstFalse");
    assert(r.firstFalseId == nred::SentryId::SentryCount && "P13 excluded ⇒ no eligible false");
    assert(r.firstFalseValid == 0 && "no eligible false ⇒ valid 0");
    assert(r.readCount == nred::SentryId::SentryCount && "P13 still counted as read");
    // 反面对照：P13 之后合格的项为 0 ⇒ 必须报那一项（而不是 P13）
    vals[nred::SentryId::P17_bit19].value = 0;
    const nred::WindowResult r2 = nred::findFirstFalseSentry(vals, nred::SentryId::SentryCount);
    assert(r2.firstFalseId == nred::SentryId::P17_bit19 && "next eligible false must win");
    printf("  PASS: P13 不参与 firstFalse（读数仍计入 readCount）\n");
}

// ── A-41 对照 ②：掩码读**加速器**侧（hwInterface 侧同偏移无信号）────────────────
static uint8_t gSelfBuf[0x1E90];    // hwInterface 侧（合成：全 0）
static uint8_t gAccelBuf[0x1E90];   // 加速器侧（合成：带目标位）
static uint64_t gHandlerBuf[4];     // 合成 handler：+0x10 存"加速器指针"

static void test_mask_base_is_accel_not_self() {
    printf("▶ test_mask_base_is_accel_not_self\n");
    memset(gSelfBuf, 0, sizeof(gSelfBuf));
    memset(gAccelBuf, 0, sizeof(gAccelBuf));
    gAccelBuf[0x1E89] = 0x40;                     // 位 14（P7 → bit14）只出现在加速器侧
    gHandlerBuf[0] = 0; gHandlerBuf[1] = 0;
    gHandlerBuf[2] = reinterpret_cast<uint64_t>(gAccelBuf);   // handler+0x10 ≡ accel（A-26 §5.1）
    gHandlerBuf[3] = 0;
    assert(nred::accelFromHandler(reinterpret_cast<uint64_t>(gHandlerBuf))
           == reinterpret_cast<uint64_t>(gAccelBuf) && "handler+0x10 must be the accel");
    uint64_t v = 0;
    assert(nred::maskBitSample(gAccelBuf, 1u, 14, &v) == 1 && v == 1 && "bit must come from accel");
    uint64_t vSelf = 0;
    assert(nred::maskBitSample(gSelfBuf, 1u, 14, &vSelf) == 1 && vSelf == 0
           && "hwInterface side has no signal（旧写法 maskBase=s 即读此处）");
    // 基址非法 ⇒ 不取样（调用方须记 valid=0，不得当"读到 0"）
    uint64_t vBad = 9;
    assert(nred::maskBitSample(gAccelBuf, 0u, 14, &vBad) == 0 && vBad == 9 && "invalid base ⇒ no sample");
    printf("  PASS: 位取样走 accel 侧；基址非法不出数\n");
}

int main() {
    printf("=== A-25 window probe offline tests ===\n");
    test_first_false_ordering();
    test_first_false_none_empty();
    test_unknown_not_counted_as_false();
    test_p5_binary_both_zero();
    test_p5_binary_not_both_zero();
    test_p5_binary_invalid();
    test_mask_bit_read_p7_faithful();
    test_mask_bit_read_neg_control();
    test_mask_bit_spec_consistency();
    test_p13_not_first_false();
    test_mask_base_is_accel_not_self();
    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}
