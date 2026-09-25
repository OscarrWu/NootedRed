// 验收测试：DCN 3.1.4 ODM 配置序列生成器
//
// 对应 docs/ROADMAP.md 第六步（子步骤 ① ODM 配置）的离线判据：
//   ① bypass / combine 两种拓扑的 op 序列（地址、op 类型、掩码、移位、值、顺序）
//   ② ODM 内存实例掩码的三档分支（照 Linux dcn314_optc.c:55-79）
//   ③ regUpdate 的执行语义 = 读-改-写（与真值记录的形态一致：1 读 1 写）
//
// 编译运行（分析机）：
//   make -f src/NootedRed/DisplaySeq/Makefile test
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.

#include "Dcn314OdmSeq.hpp"
#include "RegOp.hpp"
#include "RegSink.hpp"

#include <stdio.h>
#include <stdlib.h>

using namespace display;
using namespace display::dcn314_odm;

static constexpr uint32_t kSeg2 = 0x34C0u;  // DCN SE2 基址
static constexpr uint32_t kSeg3 = 0x9000u;  // DCN SE3 基址

static constexpr uint32_t kDss  = kSeg2 + 0x1ACB;  // OPTC_DATA_SOURCE_SELECT（OTG0）
static constexpr uint32_t kWctl = kSeg2 + 0x1ACE;  // OPTC_WIDTH_CONTROL（OTG0）
static constexpr uint32_t kMcfg = kSeg2 + 0x1AD0;  // OPTC_MEMORY_CONFIG（OTG0）
static constexpr uint32_t kHtc  = kSeg2 + 0x1B2E;  // OTG_H_TIMING_CNTL（OTG0）
static constexpr uint32_t kMux0 = kSeg3 + 0x0580;  // MPC_OUT0_MUX（BASE_IDX=3！）
static constexpr uint32_t kMux1 = kSeg3 + 0x0584;  // MPC_OUT1_MUX

static int g_fail = 0;

static void fail(const char* label, const char* what, unsigned long got, unsigned long want)
{
    fprintf(stderr, "  [FAIL] %s: %s got=0x%lX want=0x%lX\n", label, what, got, want);
    g_fail += 1;
}

static void expectSize(const RegSeq& seq, size_t want, const char* label)
{
    if (seq.size() != want) {
        fprintf(stderr, "  [FAIL] %s: op 数 got=%zu want=%zu\n", label, seq.size(), want);
        g_fail += 1;
    }
}

static void expectKind(const RegSeq& seq, size_t i, RegOp::Kind kind, const char* label)
{
    if (i >= seq.size()) { return; }
    if (seq[i].kind != kind) { fail(label, "op kind", static_cast<unsigned long>(seq[i].kind), static_cast<unsigned long>(kind)); }
}

static void expectAddr(const RegSeq& seq, size_t i, uint32_t addr, const char* label)
{
    if (i >= seq.size()) { return; }
    if (seq[i].addr != addr) { fail(label, "addr", seq[i].addr, addr); }
}

static void expectValue(const RegSeq& seq, size_t i, uint32_t value, const char* label)
{
    if (i >= seq.size()) { return; }
    if (seq[i].value != value) { fail(label, "value", seq[i].value, value); }
}

static void expectMaskShift(const RegSeq& seq, size_t i, uint32_t mask, uint8_t shift, const char* label)
{
    if (i >= seq.size()) { return; }
    if (seq[i].mask != mask) { fail(label, "mask", seq[i].mask, mask); }
    if (seq[i].shift != shift) { fail(label, "shift", seq[i].shift, shift); }
}

// ── 记录型 sink：把实际发生的读/写按序记下，用于验证 regUpdate 的执行语义 ──
class RecordingSink final : public RegSink {
public:
    RegValue read(const RegAddr addr) override
    {
        reads[readCount] = addr;
        readValues[readCount] = preset;
        readCount += 1;
        return preset;
    }
    void write(const RegAddr addr, const RegValue val) override
    {
        writes[writeCount].addr = addr;
        writes[writeCount].value = val;
        writeCount += 1;
    }
    void delayMicroseconds(uint32_t) override {}

    struct W { RegAddr addr; RegValue value; };
    static constexpr size_t kMax = 64;
    RegAddr  reads[kMax]{};
    RegValue readValues[kMax]{};
    W        writes[kMax]{};
    size_t   readCount{0};
    size_t   writeCount{0};
    RegValue preset{0};
};

// ═══════════════════════════════════════════════════════════════════════════
// ① bypass（oppCnt = 1）：与真值实测序列逐条对齐
//   真值（Linux，OTG0）：Write(MEMORY_CONFIG)=0x0、Write(DATA_SOURCE_SELECT)=0xFFF00000、
//                        Update(OTG_H_TIMING_CNTL, DIV_MODE, 0)
// ═══════════════════════════════════════════════════════════════════════════
static void test_bypass_sequence()
{
    RegOp  buf[32]{};
    RegSeq seq(buf, 32);
    const uint32_t oppInst[1] = {0};
    generateUpdateOdmFull(seq, kSeg2, kSeg3, /*otgInst=*/0, oppInst, /*oppCnt=*/1, /*sliceWidth=*/0,
                          /*twoPixelsPerContainer=*/false);

    // 4 段：Write(dss) + Update(htc) + Write(mcfg) + Update(mux)
    expectSize(seq, 4, "bypass");

    expectKind(seq, 0, RegOp::Kind::Write, "bypass[0]");
    expectAddr(seq, 0, kDss, "bypass[0]");
    expectValue(seq, 0, 0xFFF00000u, "bypass[0]");  // 真值实测值

    expectKind(seq, 1, RegOp::Kind::Update, "bypass[1]");
    expectAddr(seq, 1, kHtc, "bypass[1]");
    expectMaskShift(seq, 1, 0x3u, 0, "bypass[1]");
    expectValue(seq, 1, 0u, "bypass[1]");  // h_div = NO_DIV（真值里该字段读=写=0x30000，即字段值 0）

    expectKind(seq, 2, RegOp::Kind::Write, "bypass[2]");
    expectAddr(seq, 2, kMcfg, "bypass[2]");
    expectValue(seq, 2, 0u, "bypass[2]");

    expectKind(seq, 3, RegOp::Kind::Update, "bypass[3]");
    expectAddr(seq, 3, kMux0, "bypass[3]");                    // ⚠️ SEG3 基址（不是 SEG2）
    expectMaskShift(seq, 3, 0x300u, 0, "bypass[3]");
    expectValue(seq, 3, 0x100u, "bypass[3]");                  // DISABLE=1(bits8) | RATE=0
    printf("  [PASS] bypass：4 个 op（Write dss=0xFFF00000 / Update htc / Write mcfg=0 / Update mux）\n");
}

// ═══════════════════════════════════════════════════════════════════════════
// ② combine（oppCnt = 2）：Linux dcn314_optc.c:50-104 的顺序
// ═══════════════════════════════════════════════════════════════════════════
static void test_combine_sequence()
{
    RegOp  buf[32]{};
    RegSeq seq(buf, 32);
    const uint32_t oppInst[2] = {0, 1};
    // sliceWidth=1920 → h_active=3840 → odm_mem_count=(3840+2047)/2048=2 → 「<=2」档
    generateUpdateOdmFull(seq, kSeg2, kSeg3, /*otgInst=*/0, oppInst, /*oppCnt=*/2, /*sliceWidth=*/1920,
                          /*twoPixelsPerContainer=*/false);

    // Write(mcfg) + Write(dss) + Update(wctl) + Update(htc) + Update(mux0) + Update(mux1)
    expectSize(seq, 6, "combine");

    expectKind(seq, 0, RegOp::Kind::Write, "combine[0]");
    expectAddr(seq, 0, kMcfg, "combine[0]");
    expectValue(seq, 0, 0x5u, "combine[0]");  // (1<<(0*2)) | (1<<(1*2)) = 0x5

    expectKind(seq, 1, RegOp::Kind::Write, "combine[1]");
    expectAddr(seq, 1, kDss, "combine[1]");
    expectValue(seq, 1, (1u << 0) | (0u << 16) | (1u << 20), "combine[1]");  // NUM=1, SEG0=0, SEG1=1

    expectKind(seq, 2, RegOp::Kind::Update, "combine[2]");
    expectAddr(seq, 2, kWctl, "combine[2]");
    expectMaskShift(seq, 2, 0x1FFFu, 0, "combine[2]");
    expectValue(seq, 2, 1920u, "combine[2]");

    expectKind(seq, 3, RegOp::Kind::Update, "combine[3]");
    expectAddr(seq, 3, kHtc, "combine[3]");
    expectValue(seq, 3, 1u, "combine[3]");  // opp_cnt - 1

    expectKind(seq, 4, RegOp::Kind::Update, "combine[4]");
    expectAddr(seq, 4, kMux0, "combine[4]");
    expectKind(seq, 5, RegOp::Kind::Update, "combine[5]");
    expectAddr(seq, 5, kMux1, "combine[5]");
    printf("  [PASS] combine(oppCnt=2)：6 个 op（含 2 个 OPP 各一次 out-rate-control 读改写）\n");
}

// ═══════════════════════════════════════════════════════════════════════════
// ③ 内存掩码三档（照 Linux dcn314_optc.c:55-79 逐字）
// ═══════════════════════════════════════════════════════════════════════════
static void test_memory_mask()
{
    const uint32_t ab[2] = {0, 1};
    const uint32_t ab4[4] = {0, 1, 2, 3};

    // 每档都按 Linux 公式手算（h_active = sliceWidth*oppCnt；count = (h_active+2047)/2048）：
    //   oppCnt=2：count=2 → 1<<(0*2) | 1<<(1*2) = 0x5；count=3 → 3<<0 | 3<<2 = 0xF；count=8 → 0x77
    //   oppCnt=4：count=2 → 0x3；count=4 → 0xF；count=8 → 0x3F
    const uint32_t m2 = computeOdmMemoryMask(ab, 2, 1920);    // h_active=3840  → count=2
    const uint32_t m4 = computeOdmMemoryMask(ab, 2, 3000);    // h_active=6000  → count=3（<=4 档）
    const uint32_t m8 = computeOdmMemoryMask(ab, 2, 8192);    // h_active=16384 → count=8
    const uint32_t m4p = computeOdmMemoryMask(ab4, 4, 1000);  // h_active=4000  → count=2
    const uint32_t m4p2 = computeOdmMemoryMask(ab4, 4, 2000); // h_active=8000  → count=4
    const uint32_t m4p3 = computeOdmMemoryMask(ab4, 4, 4096); // h_active=16384 → count=8

    if (m2 != 0x5u) { fail("mask", "oppCnt=2,count=2", m2, 0x5u); }
    if (m4 != 0xFu) { fail("mask", "oppCnt=2,count=3", m4, 0xFu); }
    if (m8 != 0x77u) { fail("mask", "oppCnt=2,count=8", m8, 0x77u); }
    if (m4p != 0x3u) { fail("mask", "oppCnt=4,count=2", m4p, 0x3u); }
    if (m4p2 != 0xFu) { fail("mask", "oppCnt=4,count=4", m4p2, 0xFu); }
    if (m4p3 != 0x3Fu) { fail("mask", "oppCnt=4,count=8", m4p3, 0x3Fu); }
    printf("  [PASS] 内存掩码六档（oppCnt=2: 0x5/0xF/0x77；oppCnt=4: 0x3/0xF/0x3F）\n");
}

// ═══════════════════════════════════════════════════════════════════════════
// ④ regUpdate 的执行语义 = 读-改-写（真值形态：1 读 1 写）
// ═══════════════════════════════════════════════════════════════════════════
static void test_update_op_execution()
{
    RegOp  buf[8]{};
    RegSeq seq(buf, 8);
    seq.push(regUpdate(kMux0, 0x300u, 0, 0x100u, "t"));

    RecordingSink sink;
    sink.preset = 0x00004000u;  // 模拟真值里该寄存器的原值
    const size_t done = sink.executeAll(seq);

    if (done != 1) { fail("update-exec", "executeAll 返回", done, 1); }
    if (sink.readCount != 1) { fail("update-exec", "读次数", sink.readCount, 1); }
    if (sink.writeCount != 1) { fail("update-exec", "写次数", sink.writeCount, 1); }
    if (sink.reads[0] != kMux0) { fail("update-exec", "读地址", sink.reads[0], kMux0); }
    if (sink.writes[0].addr != kMux0) { fail("update-exec", "写地址", sink.writes[0].addr, kMux0); }
    // (0x4000 & ~0x300) | 0x100 = 0x4100 —— 与真值里该寄存器的第一对写值一致
    if (sink.writes[0].value != 0x00004100u) { fail("update-exec", "写值", sink.writes[0].value, 0x00004100u); }
    printf("  [PASS] regUpdate 语义：1 读 1 写，0x4000 → 0x4100（与真值实测一致）\n");
}

int main()
{
    printf("验收测试：DCN 3.1.4 ODM 序列生成器（第六步 · 子步骤 ①）\n");
    printf("── op 序列 / 内存掩码 / 执行语义 ──\n");
    test_bypass_sequence();
    test_combine_sequence();
    test_memory_mask();
    test_update_op_execution();

    if (g_fail != 0) {
        printf("失败 %d 项。\n", g_fail);
        return 1;
    }
    printf("全部通过。\n");
    return 0;
}
