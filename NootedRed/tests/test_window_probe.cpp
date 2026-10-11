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

// ── A-45：P13（bit18 = `initializeTtl` 返回值的忠实位）与其它哨兵**同权** ────────────
static void test_p13_is_first_false_candidate() {
    printf("▶ test_p13_is_first_false_candidate\n");
    // 数组序（A-30 程序序）：P12 < P13 < P14
    assert(nred::SentryId::P12_530 < nred::SentryId::P13_bit18 && "P13 must follow P12");
    assert(nred::SentryId::P13_bit18 < nred::SentryId::P14_2F8 && "P13 must precede P14");
    nred::SentryVal vals[nred::SentryId::SentryCount];
    for (uint32_t i = 0; i < nred::SentryId::SentryCount; ++i) {
        vals[i].id    = i;
        vals[i].value = 1;
        vals[i].valid = 1;
    }
    // P13 为 0（= 该条未通过；A-42 已证 bit18 忠实）、其后各项非 0 ⇒ firstFalse **必须报 P13**
    //  （若把 A-41 的排除逻辑加回，本断言即失败——见报告 A4 的"改回即失败"演示。）
    vals[nred::SentryId::P13_bit18].value = 0;
    const nred::WindowResult r = nred::findFirstFalseSentry(vals, nred::SentryId::SentryCount);
    assert(r.firstFalseId == nred::SentryId::P13_bit18 && "P13 (bit18) must be firstFalse");
    assert(r.firstFalseValid == 1 && "P13 读数有效 ⇒ firstFalseValid=1");
    assert(r.readCount == nred::SentryId::SentryCount && "all readable");
    // 顺序性对照：更早的 P12 也为 0 ⇒ 报 P12（而不是 P13）
    vals[nred::SentryId::P12_530].value = 0;
    const nred::WindowResult r2 = nred::findFirstFalseSentry(vals, nred::SentryId::SentryCount);
    assert(r2.firstFalseId == nred::SentryId::P12_530 && "earlier false must win");
    printf("  PASS: P13（bit18）同权参与 firstFalse，顺序性正确\n");
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

// ── A-44 用例：P5 合成判据（`+0x338` ∧ `+0x340` 均非 0）─────────────────────────────
static void test_p5_sentry_verdict() {
    printf("▶ test_p5_sentry_verdict\n");
    assert(nred::p5ServicesVerdict(1, 1) == 1 && "both non-zero ⇒ P5 passed");
    assert(nred::p5ServicesVerdict(0x1000, 0x2000) == 1 && "both non-zero（任意非零值）⇒ 1");
    assert(nred::p5ServicesVerdict(0, 0) == 0 && "both zero ⇒ P5 failed");
    assert(nred::p5ServicesVerdict(0, 1) == 0 && "0x338==0 ⇒ P5 failed");
    assert(nred::p5ServicesVerdict(1, 0) == 0 && "0x340==0 ⇒ P5 failed");
    printf("  PASS: P5 合成判据真值表\n");
}

// ── A-44 对照（能失败）：P5 必须早于 P6，否则 P5 失败会被误报为 P6 ──────────────────
static void test_p5_precedes_p6_in_scan() {
    printf("▶ test_p5_precedes_p6_in_scan\n");
    assert(nred::SentryId::P5_Services < nred::SentryId::P6_370 && "P5 must precede P6");
    // 场景：P5 失败（合成判据 0）且 P5 未通过 ⇒ P6 未到达（0x370 == 0）⇒ firstFalse 必须是 P5
    nred::SentryVal vals[nred::SentryId::SentryCount];
    for (uint32_t i = 0; i < nred::SentryId::SentryCount; ++i) {
        vals[i].id    = i;
        vals[i].value = 1;
        vals[i].valid = 1;
    }
    vals[nred::SentryId::P5_Services].value = 0;
    vals[nred::SentryId::P5_bit13].value    = 0;
    vals[nred::SentryId::P6_370].value      = 0;
    const nred::WindowResult r = nred::findFirstFalseSentry(vals, nred::SentryId::SentryCount);
    assert(r.firstFalseId == nred::SentryId::P5_Services
           && "P5 failure must be reported as P5 (not P6)");
    assert(r.firstFalseValid == 1 && "P5 读数有效 ⇒ firstFalseValid=1");
    printf("  PASS: P5 失败归属 P5（数组序早于 P6）\n");
}

// ── A-47 用例：P5 入口只读采样的纯逻辑（期望串比较 / 类型分支与 sentinel / 上限截断 / 有界复制）──
static void test_p5_sampling_pure_logic() {
    printf("▶ test_p5_sampling_pure_logic\n");
    // 期望串（A-46 核实字面值；A-46 登记：不得与 `…HWServicesVega` 混用）
    assert(nred::p5StrEq(nred::p5MatchCategory(), "AMDRadeonX5000HWServices") == 1 && "literal match");
    assert(nred::p5StrEq(nred::p5MatchCategory(), "AMDRadeonX5000HWServicesVega") == 0 && "not the same");
    assert(nred::p5StrEq(nullptr, "x") == 0 && "nullptr ⇒ 不等");
    assert(nred::p5StrEq("abc", "abd") == 0 && "differ");
    assert(nred::p5StrEq("abc", "abcd") == 0 && "长度不同 ⇒ 不等");
    assert(nred::p5StrEq("", "") == 1 && "两个空串 ⇒ 等");
    // `IOMatchCategory` 类型码：0=缺失／1=OSString／2=其它（sentinel）
    assert(nred::p5PropTypeCode(1, 0) == 0 && "missing ⇒ 0");
    assert(nred::p5PropTypeCode(0, 1) == 1 && "OSString ⇒ 1");
    assert(nred::p5PropTypeCode(0, 0) == 2 && "其它类型 ⇒ 2（sentinel）");
    // sentinel 语义：只有 type==1 才允许继续取 C 串（= 不再对未知类型调用取值方法）
    assert(nred::p5ShouldReadCString(1) == 1 && "type=1 ⇒ 取 C 串");
    assert(nred::p5ShouldReadCString(0) == 0 && "type=0 ⇒ 不取");
    assert(nred::p5ShouldReadCString(2) == 0 && "type=2（sentinel）⇒ 不取");
    // client 明细上限截断（≤8；超出仅计数）
    assert(nred::p5DetailCap() == 8 && "明细上限 8");
    for (uint32_t i = 0; i < 8; ++i) { assert(nred::p5DetailWanted(i) == 1 && "上限内 ⇒ 出明细"); }
    for (uint32_t i = 8; i < 12; ++i) { assert(nred::p5DetailWanted(i) == 0 && "超上限 ⇒ 不出明细"); }
    // 有界复制：截断到 cap-1、必 NUL 结尾、不越界；nullptr ⇒ 空串
    char buf[8];
    memset(buf, 0x7F, sizeof(buf));
    nred::p5StrCopyBounded(buf, sizeof(buf), "0123456789abcdef");
    assert(memcmp(buf, "0123456", sizeof(buf)) == 0 && "截断 + NUL 结尾");
    nred::p5StrCopyBounded(buf, sizeof(buf), nullptr);
    assert(buf[0] == '\0' && "nullptr ⇒ 空串");
    printf("  PASS: P5 采样纯逻辑（期望串/类型分支/sentinel/上限/有界复制）\n");
}

// ── A-51 用例（B15-R 崩点回归）：非对象指针**不得**进入类型判定路径 ────────────────────
static void test_p5_typecheck_gate() {
    printf("▶ test_p5_typecheck_gate\n");
    // B15-R 实测：`vptr[0x710]()` 返回的是 `const char*`（Apple kext `__cstring`），旧实现把它当对象
    //  送进 `safeMetaCast` ⇒ GP trap（RDI=0xffffff7f910f849a、RAX="AMDRadeo"）。该来源必须被拒。
    const uint64_t kWantStrAddr = 0xffffff7f910f849aULL;   // B15-R 的 RDI（期望串地址，非对象）
    assert(nred::p5TypeCheckAllowed(nred::P5SrcUnknown, kWantStrAddr) == 0
           && "字符串/字面量指针 ⇒ 禁入类型判定");
    assert(nred::p5TypeCheckAllowed(nred::P5SrcUnknown, 0xffffff800caa24c8ULL) == 0
           && "来源不明的内核指针 ⇒ 一律禁入");
    // IOKit 明确返回 + 落在内核区间 ⇒ 准入
    assert(nred::p5TypeCheckAllowed(nred::P5SrcIokitReturn, kWantStrAddr) == 1
           && "IOKit 返回 + 内核区间 ⇒ 准入");
    assert(nred::p5TypeCheckAllowed(nred::P5SrcIokitReturn, 0xffffff7f80000000ULL) == 1 && "阈值边界（含）");
    // IOKit 返回但指针非法（空/低地址/阈值下界）⇒ 仍禁入
    assert(nred::p5TypeCheckAllowed(nred::P5SrcIokitReturn, 0) == 0 && "空指针 ⇒ 禁入");
    assert(nred::p5TypeCheckAllowed(nred::P5SrcIokitReturn, 0x00007ffb5a98ce80ULL) == 0 && "低地址 ⇒ 禁入");
    assert(nred::p5TypeCheckAllowed(nred::P5SrcIokitReturn, 0xffffff7f7fffffffULL) == 0 && "阈值下界 ⇒ 禁入");
    printf("  PASS: 类型判定准入门（字符串地址被拒、IOKit 返回准入、阈值边界正确）\n");
}

// ── A-55 用例（G2 盲区回归）：两串不同时，命中判定必须用 getter 串 ──────────────────
static void test_p5_want_source_two_strings() {
    printf("▶ test_p5_want_source_two_strings\n");
    // 来源标记：getter 串非空 ⇒ 1（判定用 getter 串＝Apple 的等价条件）；否则 0（回退字面值）
    assert(nred::p5WantSource("AMDRadeonX5000HWServices") == 1 && "非空 ⇒ 用 getter 串");
    assert(nred::p5WantSource("AMDRadeonX5000HWServicesVega") == 1 && "两串不同的场景 ⇒ 仍用 getter 串");
    assert(nred::p5WantSource("") == 0 && "空串 ⇒ 回退字面值");
    assert(nred::p5WantSource(nullptr) == 0 && "nullptr ⇒ 回退字面值");
    // 机械论证（G2 盲区消除）：设 getter 串＝A、字面值＝B（A≠B）、某 client 的 iomc＝A
    const char* const getterStr = "AMDRadeonX5000HWServicesVega";   // 假设 getter 返回 A
    const char* const clientStr = "AMDRadeonX5000HWServicesVega";   // client 的属性＝A
    assert(nred::p5WantSource(getterStr) == 1 && "A 非空 ⇒ 判定用 A");
    assert(nred::p5StrEq(clientStr, getterStr) == 1 && "Apple 会接受（A==A）");
    assert(nred::p5StrEq(clientStr, nred::p5MatchCategory()) == 0
           && "但 A ≠ A-46 字面值 ⇒ 旧实现（只用字面值判定）会漏掉该命中");
    printf("  PASS: 判定用 getter 串；两串不同时不再漏命中\n");
}

// ── A-55 用例（G1）：命中对象 vptr 与 HWServicesAbstract 静态槽的数值比对 ─────────────
static void test_p5_abstract_vt_compare() {
    printf("▶ test_p5_abstract_vt_compare\n");
    assert(nred::p5HwsAbstractVt() == 0x4D61CF8ULL && "kc 绝对值");
    assert(nred::p5HwsAbstractVtZvm() == nred::p5HwsAbstractVt() - 0x4B37000ULL && "归零 VM 换算自洽");
    assert(nred::p5HwsAbstractVtZvm() == 0x22ACF8ULL && "0x4D61CF8 − 0x4B37000");
    // 换算与判定（只做数值比较）
    const uint64_t slide = 0xffffff800fc00000ULL;   // 示例 slide（非 0）
    assert(nred::p5VtZvm(slide + 0x22ACF8ULL, slide) == 0x22ACF8ULL && "运行时 → 归零 VM");
    assert(nred::p5IsAbstractVt(nred::p5VtZvm(slide + 0x22ACF8ULL, slide)) == 1 && "抽象基类 vtable ⇒ 1");
    assert(nred::p5IsAbstractVt(0) == 0 && "vptr 未读到（0）⇒ 0（不得误报）");
    assert(nred::p5IsAbstractVt(0x123456ULL) == 0 && "其它 vtable ⇒ 0");
    assert(nred::p5VtZvm(0, slide) == 0 && "vptr=0 ⇒ 0");
    assert(nred::p5VtZvm(slide + 0x22ACF8ULL, 0) == 0 && "slide=0（未取到）⇒ 0");
    printf("  PASS: 抽象 vtable 数值比对（含边界）\n");
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
    test_p13_is_first_false_candidate();
    test_mask_base_is_accel_not_self();
    test_p5_sentry_verdict();
    test_p5_precedes_p6_in_scan();
    test_p5_sampling_pure_logic();
    test_p5_typecheck_gate();
    test_p5_want_source_two_strings();
    test_p5_abstract_vt_compare();
    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}
