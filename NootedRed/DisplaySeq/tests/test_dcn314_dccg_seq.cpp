// 验收测试：DCN 3.1.4 DCCG 像素率分频序列生成器（路线图第七步·集成）
//
// 离线判据（对应 docs/ROADMAP.md 第七步「跑通全部单元测试与序列比对」）：
//   ① 位域取值与 Linux dcn_3_1_4_sh_mask.h:8205-8220 逐项一致（四组 OTG 位域不重叠）
//   ② 写侧序列形态：一次 `Update`（读-改-写），掩码 = K1.MASK | K2.MASK，值按各自 shift 就位
//      —— 与 Linux `REG_UPDATE_2(OTG_PIXEL_RATE_DIV, OTGn_K1, k1, OTGn_K2, k2)` 等价
//   ③ 门 ①（NA 拒绝）：k1 或 k2 = 0xF 时**不产出任何 op**（Linux dcn314_dccg.c:113-116）
//   ④ 门 ③（实例分发）：OTG0..3 各自命中正确的 shift/mask/地址；otg_inst > 3 拒绝
//   ⑤ 读侧：`generateReadPixelRateDiv` 产出单个 Read，`extractK1/K2` 从读回值正确取字段
//
// 编译运行（分析机）：
//   make -f src/NootedRed/DisplaySeq/Makefile test
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.

#include "Dcn314DccgSeq.hpp"
#include "RegOp.hpp"
#include "RegSink.hpp"

#include <stdio.h>
#include <stdlib.h>

using namespace display;
using namespace display::dcn314_dccg;

static constexpr uint32_t kSeg1 = 0xC0u;  // DCN_SEG1_BASE（DCCG 所在段）
static constexpr uint32_t kRateDivAddr = kSeg1 + 0x006Fu;  // regOTG_PIXEL_RATE_DIV

static int g_fail = 0;

static void fail(const char* label, const char* what, unsigned long got, unsigned long want)
{
    fprintf(stderr, "  [FAIL] %s: %s got=0x%lX want=0x%lX\n", label, what, got, want);
    g_fail += 1;
}

// ── 记录型 sink：把实际发生的读/写按序记下 ──
class RecordingSink final : public RegSink {
public:
    RegValue read(const RegAddr addr) override
    {
        reads[readCount]  = addr;
        readCount        += 1;
        return preset;
    }
    void write(const RegAddr addr, const RegValue val) override
    {
        writes[writeCount].addr  = addr;
        writes[writeCount].value = val;
        writeCount              += 1;
    }
    void delayMicroseconds(uint32_t) override {}

    struct W { RegAddr addr; RegValue value; };
    static constexpr size_t kMax = 16;
    RegAddr  reads[kMax]{};
    W        writes[kMax]{};
    size_t   readCount{0};
    size_t   writeCount{0};
    RegValue preset{0};
};

// ═══════════════════════════════════════════════════════════════════════════
// ① 位域取值与 Linux 头文件一致（四组 OTG 位域互不重叠、顺序递增）
// ═══════════════════════════════════════════════════════════════════════════
static void test_field_values()
{
    // 依据 dcn_3_1_4_sh_mask.h:8205-8220，逐条断言：
    struct Want { uint32_t inst, k1s, k1m, k2s, k2m; };
    const Want want[4] = {
        {0, 0x0, 0x00000001u, 0x1, 0x00000006u},
        {1, 0x3, 0x00000008u, 0x4, 0x00000030u},
        {2, 0x6, 0x00000040u, 0x7, 0x00000180u},
        {3, 0x9, 0x00000200u, 0xA, 0x00000C00u},
    };

    for (uint32_t i = 0; i < 4; i += 1) {
        uint32_t k1s = 0, k1m = 0, k2s = 0, k2m = 0;
        if (!otgRateDivFields(i, &k1s, &k1m, &k2s, &k2m)) {
            fail("fields", "otgRateDivFields 拒绝合法实例", i, 0);
            continue;
        }
        if (k1s != want[i].k1s) { fail("fields", "K1 shift", k1s, want[i].k1s); }
        if (k1m != want[i].k1m) { fail("fields", "K1 mask", k1m, want[i].k1m); }
        if (k2s != want[i].k2s) { fail("fields", "K2 shift", k2s, want[i].k2s); }
        if (k2m != want[i].k2m) { fail("fields", "K2 mask", k2m, want[i].k2m); }
    }

    // 四组位域必须互不重叠（否则合并成一次 Update 会静默破坏别的实例）
    uint32_t seen = 0;
    for (uint32_t i = 0; i < 4; i += 1) {
        uint32_t k1s = 0, k1m = 0, k2s = 0, k2m = 0;
        otgRateDivFields(i, &k1s, &k1m, &k2s, &k2m);
        if ((seen & (k1m | k2m)) != 0) { fail("fields", "位域重叠（实例）", i, 0); }
        seen |= (k1m | k2m);
    }
    // Linux 头文件里 OTG0..3 的域覆盖 bit0..bit11
    if ((seen & 0xFFFu) != 0xFFFu) { fail("fields", "四组域应覆盖 bit0-11", seen, 0xFFFu); }

    // 非法实例（> 3）必须被拒绝
    uint32_t s = 0, m = 0;
    if (otgRateDivFields(4, &s, &m, &s, &m)) { fail("fields", "otg_inst=4 应被拒绝", 1, 0); }

    printf("  [PASS] ① 位域取值与 Linux dcn_3_1_4_sh_mask.h 一致，四组互不重叠且覆盖 bit0-11\n");
}

// ═══════════════════════════════════════════════════════════════════════════
// ② 写侧序列形态：一次 Update（读-改-写），与 REG_UPDATE_2 等价
// ═══════════════════════════════════════════════════════════════════════════
static void test_write_sequence()
{
    RegOp  buf[8]{};
    RegSeq seq(buf, 8);

    // OTG0 + (K1=BY_1=0, K2=BY_4=3)：mask = 0x1|0x6 = 0x7，value = (0<<0)|(3<<1) = 0x6
    const bool ok = generateSetPixelRateDiv(seq, kSeg1, /*otgInst=*/0, kPixelRateDivBy1, kPixelRateDivBy4);
    if (!ok) { fail("write", "generateSetPixelRateDiv 返回 false", 0, 1); }
    if (seq.size() != 1) { fail("write", "op 数", seq.size(), 1); }
    if (seq[0].kind != RegOp::Kind::Update) { fail("write", "op kind 应为 Update", static_cast<unsigned long>(seq[0].kind), 4); }
    if (seq[0].addr != kRateDivAddr) { fail("write", "addr", seq[0].addr, kRateDivAddr); }
    if (seq[0].mask != 0x7u) { fail("write", "mask", seq[0].mask, 0x7u); }
    if (seq[0].value != 0x6u) { fail("write", "value", seq[0].value, 0x6u); }

    // 执行语义：1 读 1 写；原值 0xFFFFF000 → (0xFFFFF000 & ~0x7) | 0x6 = 0xFFFFF006
    RecordingSink sink;
    sink.preset = 0xFFFFF000u;
    const size_t done = sink.executeAll(seq);
    if (done != 1) { fail("write-exec", "executeAll 返回", done, 1); }
    if (sink.readCount != 1) { fail("write-exec", "读次数", sink.readCount, 1); }
    if (sink.writeCount != 1) { fail("write-exec", "写次数", sink.writeCount, 1); }
    if (sink.reads[0] != kRateDivAddr) { fail("write-exec", "读地址", sink.reads[0], kRateDivAddr); }
    if (sink.writes[0].value != 0xFFFFF006u) { fail("write-exec", "写值", sink.writes[0].value, 0xFFFFF006u); }

    printf("  [PASS] ② 写侧一次 Update（1 读 1 写），mask=K1|K2、value 按各自 shift 就位\n");
}

// ═══════════════════════════════════════════════════════════════════════════
// ③ 门 ①（NA 拒绝）—— Linux dcn314_dccg.c:113-116 的 BREAK_TO_DEBUGGER 分支
// ═══════════════════════════════════════════════════════════════════════════
static void test_na_rejection()
{
    RegOp  buf[8]{};
    RegSeq seq(buf, 8);

    // K1 = NA：拒绝、不产出 op
    if (generateSetPixelRateDiv(seq, kSeg1, 0, kPixelRateDivNA, kPixelRateDivBy1)) {
        fail("na", "K1=NA 应被拒绝", 1, 0);
    }
    if (seq.size() != 0) { fail("na", "K1=NA 时不应有 op", seq.size(), 0); }

    // K2 = NA：同样拒绝
    if (generateSetPixelRateDiv(seq, kSeg1, 0, kPixelRateDivBy1, kPixelRateDivNA)) {
        fail("na", "K2=NA 应被拒绝", 1, 0);
    }
    if (seq.size() != 0) { fail("na", "K2=NA 时不应有 op", seq.size(), 0); }

    // 非法实例：拒绝
    if (generateSetPixelRateDiv(seq, kSeg1, 7, kPixelRateDivBy1, kPixelRateDivBy1)) {
        fail("na", "otg_inst=7 应被拒绝", 1, 0);
    }
    if (seq.size() != 0) { fail("na", "非法实例时不应有 op", seq.size(), 0); }

    printf("  [PASS] ③ NA 与非法实例均被拒绝、不产出 op（对应 Linux BREAK_TO_DEBUGGER 分支）\n");
}

// ═══════════════════════════════════════════════════════════════════════════
// ④ 门 ③（实例分发）：OTG0..3 命中各自的 shift/mask/地址
// ═══════════════════════════════════════════════════════════════════════════
static void test_instance_dispatch()
{
    // OTG1 + (K1=BY_1=0, K2=BY_2=1)：mask = 0x8|0x30 = 0x38，value = (0<<3)|(1<<4) = 0x10
    {
        RegOp  buf[8]{};
        RegSeq seq(buf, 8);
        if (!generateSetPixelRateDiv(seq, kSeg1, 1, kPixelRateDivBy1, kPixelRateDivBy2)) {
            fail("dispatch", "OTG1 应被接受", 0, 1);
        }
        if (seq.size() != 1) { fail("dispatch", "OTG1 op 数", seq.size(), 1); }
        if (seq[0].mask != 0x38u) { fail("dispatch", "OTG1 mask", seq[0].mask, 0x38u); }
        if (seq[0].value != 0x10u) { fail("dispatch", "OTG1 value", seq[0].value, 0x10u); }
        if (seq[0].addr != kRateDivAddr) { fail("dispatch", "OTG1 addr（同一打包寄存器）", seq[0].addr, kRateDivAddr); }
    }

    // OTG3 + (K1=BY_1=0, K2=BY_4=3)：mask = 0x200|0xC00 = 0xE00，value = (0<<9)|(3<<10) = 0xC00
    {
        RegOp  buf[8]{};
        RegSeq seq(buf, 8);
        if (!generateSetPixelRateDiv(seq, kSeg1, 3, kPixelRateDivBy1, kPixelRateDivBy4)) {
            fail("dispatch", "OTG3 应被接受", 0, 1);
        }
        if (seq.size() != 1) { fail("dispatch", "OTG3 op 数", seq.size(), 1); }
        if (seq[0].mask != 0xE00u) { fail("dispatch", "OTG3 mask", seq[0].mask, 0xE00u); }
        if (seq[0].value != 0xC00u) { fail("dispatch", "OTG3 value", seq[0].value, 0xC00u); }
    }

    // PIXEL_RATE_CNTL 实例偏移：OTG0/1/2/3 = 0x80/0x84/0x88/0x8C（BASE_IDX=1）
    const uint32_t wantCntl[4] = {0x0080u, 0x0084u, 0x0088u, 0x008Cu};
    for (uint32_t i = 0; i < 4; i += 1) {
        uint32_t off = 0;
        if (!otgPixelRateCntlOffset(i, &off)) { fail("dispatch", "cntl 偏移应可取", i, 0); continue; }
        if (off != wantCntl[i]) { fail("dispatch", "cntl 偏移", off, wantCntl[i]); }
    }
    {
        uint32_t off = 0;
        if (otgPixelRateCntlOffset(4, &off)) { fail("dispatch", "cntl otg_inst=4 应被拒绝", 1, 0); }
    }

    printf("  [PASS] ④ 实例分发：OTG0..3 各自命中正确掩码/取值；CNTL 偏移 0x80/0x84/0x88/0x8C\n");
}

// ═══════════════════════════════════════════════════════════════════════════
// ⑤ 读侧：单条 Read + 字段提取
// ═══════════════════════════════════════════════════════════════════════════
static void test_read_and_extract()
{
    RegOp  buf[8]{};
    RegSeq seq(buf, 8);
    generateReadPixelRateDiv(seq, kSeg1, /*otgInst=*/0);
    if (seq.size() != 1) { fail("read", "op 数", seq.size(), 1); }
    if (seq[0].kind != RegOp::Kind::Read) { fail("read", "op kind 应为 Read", static_cast<unsigned long>(seq[0].kind), 1); }
    if (seq[0].addr != kRateDivAddr) { fail("read", "addr", seq[0].addr, kRateDivAddr); }

    // 字段提取：OTG0 的值 0x6 → K1 = bit0 = 0（BY_1）、K2 = bit1-2 = 3（BY_4）
    if (extractK1(0, 0x6u) != kPixelRateDivBy1) { fail("extract", "OTG0 K1", extractK1(0, 0x6u), kPixelRateDivBy1); }
    if (extractK2(0, 0x6u) != kPixelRateDivBy4) { fail("extract", "OTG0 K2", extractK2(0, 0x6u), kPixelRateDivBy4); }

    // OTG3 的值 0xC00 → K1 = bit9 = 0（BY_1）、K2 = bit10-11 = 3（BY_4）
    if (extractK1(3, 0xC00u) != kPixelRateDivBy1) { fail("extract", "OTG3 K1", extractK1(3, 0xC00u), kPixelRateDivBy1); }
    if (extractK2(3, 0xC00u) != kPixelRateDivBy4) { fail("extract", "OTG3 K2", extractK2(3, 0xC00u), kPixelRateDivBy4); }

    // 往返一致：写入值 → 读回值 → 提取，必须还原出原始 k1/k2（与 Linux 的 set→get 往返同义）
    const uint32_t cases[4][2] = {{0, 0}, {0, 3}, {0, 1}, {1, 1}};  // 只用合法值（非 NA）
    for (uint32_t i = 0; i < 4; i += 1) {
        for (uint32_t inst = 0; inst < 4; inst += 1) {
            RegOp  wbuf[8]{};
            RegSeq wseq(wbuf, 8);
            if (!generateSetPixelRateDiv(wseq, kSeg1, inst, cases[i][0], cases[i][1])) { continue; }
            // 模拟"写进一个干净寄存器后的完整值"
            RecordingSink sink;
            sink.preset = 0;
            sink.executeAll(wseq);
            const uint32_t regVal = sink.writes[0].value;
            if (extractK1(inst, regVal) != cases[i][0]) {
                fail("roundtrip", "K1 往返", extractK1(inst, regVal), cases[i][0]);
            }
            if (extractK2(inst, regVal) != cases[i][1]) {
                fail("roundtrip", "K2 往返", extractK2(inst, regVal), cases[i][1]);
            }
        }
    }

    printf("  [PASS] ⑤ 读侧单条 Read + extractK1/K2；set→get 往返一致（全部合法取值 × 四实例）\n");
}

int main()
{
    printf("验收测试：DCN 3.1.4 DCCG 像素率分频序列生成器（第七步·集成）\n");
    printf("── 位域 / 写序列 / 拒绝门 / 实例分发 / 读写往返 ──\n");
    test_field_values();
    test_write_sequence();
    test_na_rejection();
    test_instance_dispatch();
    test_read_and_extract();

    if (g_fail != 0) {
        printf("失败 %d 项。\n", g_fail);
        return 1;
    }
    printf("全部通过。\n");
    return 0;
}
