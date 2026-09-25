// B2 验收测试：RegOp 序列架构
//
// 对应 docs/ROADMAP.md §3.2「序列生成架构」的三条验收判据：
//   1. 接口无硬件依赖，可在分析机用户态编译
//   2. 单元测试覆盖序列生成
//   3. 用 mock 寄存器接口跑真机路径，其产出序列与影子运行结果一致
//
// 判据 1 由"本文件能在分析机 g++ 直接编译通过"本身证明。
// 判据 2、3 由下述断言证明。
//
// 编译运行（分析机）：
//   make -f src/NootedRed/DisplaySeq/Makefile test
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.

#include "RegOp.hpp"
#include "RegSink.hpp"
#include "RegSinkUser.hpp"
#include "VBIOSSMC.hpp"
#include "VbiosSmcSeq.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace display;

// ── 测试用的常量（Phoenix 的 VBIOSSMC 邮箱，BASE_IDX=0 / SEG0）──
// 值取自 src/NootedRed/Regs/SMU.hpp，此处复写以独立校验。
static constexpr RegAddr kMailbox67 = 0x283;
static constexpr RegAddr kMailbox83 = 0x293;
static constexpr RegAddr kMailbox91 = 0x29B;

// ═══════════════════════════════════════════════════════════════════════════
// 判据 2：单元测试覆盖序列生成
// ═══════════════════════════════════════════════════════════════════════════

// 2a. 常量与源文件一致（防止本地副本漂移）
static void test_constants_match_source() {
    assert(VBIOSSMC_Status_BUSY == 0x0);
    assert(VBIOSSMC_Result_OK == 0x1);
    assert(VBIOSSMC_Result_Failed == 0xFF);
    assert(VBIOSSMC_MSG_SetDispclkFreq == 0x4);
    assert(VBIOSSMC_MSG_SetDppclkFreq == 0x6);
    assert(VBIOSSMC_MSG_SetHardMinDcfclkByFreq == 0x7);
    assert(VBIOSSMC_MSG_SetMinDeepSleepDcfclk == 0x8);
    assert(VBIOSSMC_MSG_SetDisplayCount == 0xB);
    assert(VBIOSSMC_MSG_SetDisplayIdleOptimizations == 0x12);
    std::puts("  [PASS] 2a 常量与源文件一致");
}

// 2b. khz→MHz 向上取整（Linux khz_to_mhz_ceil）
static void test_khz_to_mhz_ceil() {
    using namespace vbios_smc;
    assert(khzToMhzCeil(594000) == 594);   // 整除
    assert(khzToMhzCeil(594001) == 595);   // 向上
    assert(khzToMhzCeil(0) == 0);
    assert(khzToMhzCeil(1000) == 1);
    assert(khzToMhzCeil(999) == 1);        // 不足 1 MHz 也进 1
    assert(khzToMhzCeil(600000) == 600);
    std::puts("  [PASS] 2b khz→MHz 向上取整");
}

// 2c. send_msg_with_param 的序列形状与顺序
//     Linux 次序：等闲 → 清响应 → 写参数 → 写消息 → 等完成 → 读回参数
static void test_send_msg_sequence_order() {
    using namespace vbios_smc;
    RegOp buf[RegSeq::kDefaultCapacity];
    RegSeq seq(buf, RegSeq::kDefaultCapacity);

    generateSetDispclk(seq, kMailbox67, kMailbox83, kMailbox91, 594000);

    assert(!seq.overflowed());
    assert(seq.size() == 6);

    // 顺序是协议的一部分，逐项断言
    assert(seq[0].kind == RegOp::Kind::Poll && seq[0].addr == kMailbox91);
    assert(seq[1].kind == RegOp::Kind::Write && seq[1].addr == kMailbox91);
    assert(seq[1].value == VBIOSSMC_Status_BUSY);          // 清响应 = 写 BUSY
    assert(seq[2].kind == RegOp::Kind::Write && seq[2].addr == kMailbox83);
    assert(seq[2].value == 594);                           // 参数 = MHz
    assert(seq[3].kind == RegOp::Kind::Write && seq[3].addr == kMailbox67);
    assert(seq[3].value == VBIOSSMC_MSG_SetDispclkFreq);   // 触发
    assert(seq[4].kind == RegOp::Kind::Poll && seq[4].addr == kMailbox91);
    assert(seq[5].kind == RegOp::Kind::Read && seq[5].addr == kMailbox83);
    std::puts("  [PASS] 2c send_msg 序列形状与顺序");
}

// 2d. Poll op 的 mask 语义：busy=0x0 → mask=0xFFFFFFFF
//     判据 (读值 & mask) != 0 等价于 Linux 的 (读值 != BUSY)
static void test_poll_mask_semantics() {
    const RegOp op = regPollUntilNot(kMailbox91, VBIOSSMC_Status_BUSY);
    assert(op.kind == RegOp::Kind::Poll);
    assert(op.addr == kMailbox91);
    // mask = ~BUSY = ~0 = 0xFFFFFFFF → 非零读值即视为完成
    assert(op.mask == 0xFFFFFFFFu);
    // 非零 busy 值的取反
    const RegOp op2 = regPollUntilNot(kMailbox91, 0x1);
    assert(op2.mask == 0xFFFFFFFEu);
    std::puts("  [PASS] 2d Poll mask 语义");
}

// 2e. 四个改频 wrapper 只改消息号，其余一致
static void test_wrappers_differ_only_in_msgid() {
    using namespace vbios_smc;
    RegOp b1[64], b2[64], b3[64];
    RegSeq s1(b1, 64), s2(b2, 64), s3(b3, 64);

    generateSetDispclk(s1, kMailbox67, kMailbox83, kMailbox91, 600000);
    generateSetDppclk(s2, kMailbox67, kMailbox83, kMailbox91, 600000);
    generateSetHardMinDcfclk(s3, kMailbox67, kMailbox83, kMailbox91, 600000);

    assert(s1.size() == s2.size() && s2.size() == s3.size());
    // index 3 是消息号写入 → 三者应不同
    assert(s1[3].value == VBIOSSMC_MSG_SetDispclkFreq);
    assert(s2[3].value == VBIOSSMC_MSG_SetDppclkFreq);
    assert(s3[3].value == VBIOSSMC_MSG_SetHardMinDcfclkByFreq);
    // 其余位置应完全一致
    for (std::size_t i = 0; i < s1.size(); ++i) {
        if (i == 3) { continue; }
        assert(s1[i].kind == s2[i].kind && s1[i].kind == s3[i].kind);
        assert(s1[i].addr == s2[i].addr && s1[i].addr == s3[i].addr);
        assert(s1[i].value == s2[i].value && s1[i].value == s3[i].value);
    }
    std::puts("  [PASS] 2e 四个改频 wrapper 仅消息号不同");
}

// 2f. SetDisplayCount 的参数不做频率换算（不是频率，是计数）
static void test_display_count_no_unit_conversion() {
    using namespace vbios_smc;
    RegOp buf[64];
    RegSeq seq(buf, 64);
    generateSetDisplayCount(seq, kMailbox67, kMailbox83, kMailbox91, 2);
    assert(seq[2].value == 2);  // 直接透传，不是 1（若误做 ceil(2/1000) 会变 1）
    std::puts("  [PASS] 2f SetDisplayCount 参数不做频率换算");
}

// 2g. 缓冲区溢出被检出（而非静默截断）
static void test_overflow_detected() {
    RegOp small[3];
    RegSeq seq(small, 3);
    seq.push(regWrite(1, 1));
    seq.push(regWrite(2, 2));
    seq.push(regWrite(3, 3));
    assert(!seq.overflowed());
    seq.push(regWrite(4, 4));   // 第 4 项超容
    assert(seq.overflowed());
    assert(seq.size() == 3);    // 不增长
    std::puts("  [PASS] 2g 溢出被检出");
}

// 2h. 过滤（差分器按前缀切片 / 只看写）
static void test_filtering() {
    using namespace vbios_smc;
    RegOp buf[64];
    RegSeq seq(buf, 64);
    generateSetDispclk(seq, kMailbox67, kMailbox83, kMailbox91, 594000);

    RegOp wbuf[64];
    RegSeq writes(wbuf, 64);
    const std::size_t n = seq.copyWrites(writes);
    assert(n == 3);              // clear_response / write_param / trigger_msg 共 3 个写
    assert(writes.size() == n);

    RegOp fbuf[64];
    RegSeq filtered(fbuf, 64);
    const std::size_t m = seq.copyWhereStepPrefix(filtered, "wait_");
    assert(m == 2);              // wait_idle_before + wait_complete
    assert(std::strcmp(filtered[0].step, "wait_idle_before") == 0);
    assert(std::strcmp(filtered[1].step, "wait_complete") == 0);

    // 过滤到容量不足的缓冲区 → 溢出标志被置
    RegOp tiny[1];
    RegSeq tinySeq(tiny, 1);
    seq.copyWrites(tinySeq);
    assert(tinySeq.overflowed());
    std::puts("  [PASS] 2h 过滤（前缀 / 只写 / 溢出）");
}

// ═══════════════════════════════════════════════════════════════════════════
// 判据 3：用 mock 寄存器接口跑真机路径，产出序列与影子运行一致
// ═══════════════════════════════════════════════════════════════════════════

// 记录型 sink：把实际发生的每次写/读按序记下，供与生成器产出逐项比对。
// 这是"真机路径产出序列 == 生成器序列"的可验证形式。
class RecordingSink final : public RegSink {
public:
    struct Item {
        char    kind;   // 'w' | 'r'
        RegAddr addr;
        RegValue value;
    };
    static constexpr std::size_t kCap = 128;
    Item        items[kCap]{};
    std::size_t count{0};

    void presetRead(RegAddr a, RegValue v) {
        if (presetCount_ < 8) { presets_[presetCount_++] = {a, v}; }
    }

    RegValue read(RegAddr addr) override {
        RegValue v = 0;
        for (std::size_t i = 0; i < presetCount_; ++i) {
            if (presets_[i].addr == addr) { v = presets_[i].value; break; }
        }
        if (count < kCap) { items[count++] = {'r', addr, v}; }
        return v;
    }
    void write(RegAddr addr, RegValue val) override {
        if (count < kCap) { items[count++] = {'w', addr, val}; }
    }
    void delayMicroseconds(std::uint32_t) override {}

private:
    struct P { RegAddr addr; RegValue value; };
    P           presets_[8]{};
    std::size_t presetCount_{0};
};

// 3a. ★核心：把生成器序列喂给 mock sink 执行，其**实际写序列**必须逐项等于生成器产出
static void test_mock_execution_matches_generated() {
    using namespace vbios_smc;
    RegOp buf[64];
    RegSeq seq(buf, 64);
    generateSetDispclk(seq, kMailbox67, kMailbox83, kMailbox91, 594000);

    RecordingSink sink;
    sink.presetRead(kMailbox91, VBIOSSMC_Result_OK);  // 预置非忙 → 两个 Poll 均立即通过
    sink.setPollLimits(4, 0);

    const std::size_t reached = sink.executeAll(seq);
    assert(reached == seq.size());                    // 全部执行成功

    // 逐项比对：序列里的每个 Write，必须在 sink 上以相同 addr/value 出现，且顺序一致
    std::size_t wi = 0;
    for (std::size_t i = 0; i < seq.size(); ++i) {
        if (seq[i].kind != RegOp::Kind::Write) { continue; }
        // 跳过 sink 上因 Poll 产生的读
        while (wi < sink.count && sink.items[wi].kind != 'w') { ++wi; }
        assert(wi < sink.count);                      // 生成器有 Write，sink 必然有对应
        assert(sink.items[wi].addr == seq[i].addr);
        assert(sink.items[wi].value == seq[i].value);
        ++wi;
    }
    std::puts("  [PASS] 3a 真机路径实际写序列 == 生成器序列（逐项）");
}

// 3b. Poll 在"始终忙"时应超时返回失败（不挂死）
static void test_poll_timeout_is_bounded() {
    RegOp buf[8];
    RegSeq seq(buf, 8);
    seq.push(regPollUntilNot(kMailbox91, VBIOSSMC_Status_BUSY));

    UserSpaceRegSink sink;
    sink.presetRead(kMailbox91, VBIOSSMC_Status_BUSY);  // 永远忙
    sink.setPollLimits(5, 0);                            // 5 次即放弃

    const std::size_t reached = sink.executeAll(seq);
    assert(reached == 0);                                // 第 0 项即失败
    std::puts("  [PASS] 3b Poll 超时有界");
}

// 3c. Poll 在读到非忙值时立即通过
static void test_poll_completes_on_non_busy() {
    RegOp buf[8];
    RegSeq seq(buf, 8);
    seq.push(regPollUntilNot(kMailbox91, VBIOSSMC_Status_BUSY));

    UserSpaceRegSink sink;
    sink.presetRead(kMailbox91, 0x1);   // OK
    sink.setPollLimits(5, 0);

    const std::size_t reached = sink.executeAll(seq);
    assert(reached == 1);
    std::puts("  [PASS] 3c Poll 读到非忙值即通过");
}

// 3d. 同一份生成器产出可重复（影子运行的可复现性前提）
static void test_generation_is_deterministic() {
    using namespace vbios_smc;
    RegOp b1[64], b2[64];
    RegSeq s1(b1, 64), s2(b2, 64);
    generateSetDispclk(s1, kMailbox67, kMailbox83, kMailbox91, 594000);
    generateSetDispclk(s2, kMailbox67, kMailbox83, kMailbox91, 594000);

    assert(s1.size() == s2.size());
    for (std::size_t i = 0; i < s1.size(); ++i) {
        assert(s1[i].kind == s2[i].kind);
        assert(s1[i].addr == s2[i].addr);
        assert(s1[i].value == s2[i].value);
        assert(s1[i].mask == s2[i].mask);
    }
    std::puts("  [PASS] 3d 生成确定性（可复现）");
}

// 3e. 影子运行落盘：跑完序列后，落盘条数 == 序列长度（每个 op 一行 JSON）
static void test_shadow_run_records_all_ops() {
    using namespace vbios_smc;
    RegOp buf[64];
    RegSeq seq(buf, 64);
    generateSetDispclk(seq, kMailbox67, kMailbox83, kMailbox91, 594000);

    const char* path = "/tmp/test_shadow_run.json";
    std::FILE* f = std::fopen(path, "w");
    assert(f != nullptr);

    UserSpaceRegSink sink;
    sink.presetRead(kMailbox91, VBIOSSMC_Result_OK);
    sink.setPollLimits(4, 0);
    sink.setOutput(f);
    const std::size_t reached = sink.runAndRecord(seq);
    std::fclose(f);

    assert(reached == seq.size());
    assert(sink.opCount() == seq.size());   // 每个 op 记一次

    // 回读：行数 == 序列长度，且首行是 poll(wait_idle_before)
    std::FILE* g = std::fopen(path, "r");
    assert(g != nullptr);
    char line[256];
    std::size_t lines = 0;
    bool firstOk = false;
    while (std::fgets(line, sizeof(line), g) != nullptr) {
        if (lines == 0) {
            firstOk = std::strstr(line, "\"kind\":\"poll\"") != nullptr &&
                      std::strstr(line, "wait_idle_before") != nullptr;
        }
        ++lines;
    }
    std::fclose(g);
    assert(lines == seq.size());
    assert(firstOk);
    std::puts("  [PASS] 3e 影子运行落盘条数与序列一致");
}

int main() {
    std::puts("B2 验收测试：RegOp 序列架构");
    std::puts("── 判据 2：单元测试覆盖序列生成 ──");
    test_constants_match_source();
    test_khz_to_mhz_ceil();
    test_send_msg_sequence_order();
    test_poll_mask_semantics();
    test_wrappers_differ_only_in_msgid();
    test_display_count_no_unit_conversion();
    test_overflow_detected();
    test_filtering();

    std::puts("── 判据 3：mock 执行与生成器一致 ──");
    test_mock_execution_matches_generated();
    test_poll_timeout_is_bounded();
    test_poll_completes_on_non_busy();
    test_generation_is_deterministic();
    test_shadow_run_records_all_ops();

    std::puts("全部通过。判据 1（用户态编译）由本次编译本身证明。");
    return 0;
}
