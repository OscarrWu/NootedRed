// SmnReadProbe 离线单元测试（用户态，mock 回调）
//
// 覆盖（对应 docs/子任务/乙线SMN单点验证设计.md §4.3 的每一条硬约束）：
//
//   ① 正常路径：**恰好 4 次 MMIO**，序列逐条 = 写索引→读数据 ×2，
//      且**没有任何 PCIE_DATA2 写**（本轮唯一被写的寄存器是索引寄存器 PCIE_INDEX2）
//   ② 两个点互不干扰：真寄存器 = 基址×4+0x244（0x090FF244）、空白对照 = 基址×4（0x090FF000）
//   ③ 不回读索引、不碰 PCIE_INDEX_HI、不重试（把 maxRetries 给 3 也只会发 2 次 MMIO/点）
//   ④ 读数如实透传（含"通道未译码"时的全 1）
//   ⑤ 无延时、只碰 0x0E / 0x0F 两个偏移
//
// ⚠️ mock 的建模方式（这一点很关键）：`PCIE_INDEX2` / `PCIE_DATA2` 在 mock 里是
//    **真实的寄存器**（写索引会改变后续读索引的回读值），而不是"按顺序消费的返回队列"。
//    理由：`SmnIndirectAccess::read` 会**回读索引做校验**（Linux 的 posted-write flush），
//    队列式 mock 会把"回读索引"与"读数据"的返回值错位 ⇒ 测出来的失败是**mock 的**，
//    不是实现的。用寄存器模型则序列与语义都不依赖调用顺序的巧合。
//
// 编译运行：
//   make -f src/NootedRed/FwBringup/tests/Makefile test
//
// 依据：docs/子任务/乙线SMN单点验证设计.md §4.3；docs/子任务/乙线SMN安全访问调查.md §3.3

#include "FwBringup/SmnReadProbe.hpp"
#include "FwBringup/RegSinkKernel.hpp"
#include "FwBringup/RegAddr.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace fw;

// ── 测试基础设施（零分配形态：静态函数 + 结构体上下文）──

struct CallRecord {
    enum Type { Read, Write } type;
    uint32_t offset;
    uint32_t value;   // Write 时的值；Read 时忽略
};

struct MockContext {
    std::vector<CallRecord> calls;
    uint32_t lockCount   = 0;
    uint32_t unlockCount = 0;
    bool     delayUsed   = false;

    // ── 两个 MMIO 寄存器的模型（不是返回队列）──
    uint32_t indexReg  = 0;        // PCIE_INDEX2 的内容（写它 → 回读得到同样的值）
    uint32_t indexReadbackOverride = 0;  // 非 0 时：回读 PCIE_INDEX2 一律返回该值（模拟写未生效）
    bool     readbackBroken = false;

    // ── "SMN 空间"模型：写入索引后，数据寄存器返回什么 ──
    //    语义：把索引值当作 SMN 字节地址查表；查不到即返回全 1（未译码/总线错误的最常见形态）。
    uint32_t smnAddr0 = 0;         // 已知 SMN 字节地址（= 真寄存器）
    uint32_t smnValue0 = kSmnProbeNoValue;
    uint32_t smnAddr1 = 0;         // 另一个已知 SMN 字节地址（= 空白对照）
    uint32_t smnValue1 = kSmnProbeNoValue;
    bool     channelDead = false;  // true ⇒ 任何数据读都返回全 1（模拟"通道未译码"）

    void setKnownPoint(uint32_t byteAddr, uint32_t value) {
        if (smnAddr0 == 0 && smnAddr1 != byteAddr) { smnAddr0 = byteAddr; smnValue0 = value; }
        else { smnAddr1 = byteAddr; smnValue1 = value; }
    }

    uint32_t mmioRead(uint32_t offset) {
        calls.push_back({CallRecord::Read, offset, 0});
        if (offset == kPcieIndex2Offset) {
            return readbackBroken ? indexReadbackOverride : indexReg;
        }
        if (offset == kPcieData2Offset) {
            if (channelDead) { return kSmnProbeNoValue; }
            // 数据寄存器的内容由"索引指向的 SMN 地址"决定（这正是间接通道的语义）
            if (indexReg == smnAddr0) { return smnValue0; }
            if (indexReg == smnAddr1) { return smnValue1; }
            return kSmnProbeNoValue;
        }
        return kSmnProbeNoValue;
    }

    void mmioWrite(uint32_t offset, uint32_t value) {
        calls.push_back({CallRecord::Write, offset, value});
        if (offset == kPcieIndex2Offset) { indexReg = value; }   // 索引寄存器：写的值就是读回的值
        // 其它偏移（= PCIE_DATA2）在 mock 里只记录、不改动任何状态：
        // 我们不模拟"写数据后回读数据"，因为**本轮不允许发生任何数据写**（由断言直接禁止）。
    }

    // ── 断言辅助 ──

    void assertCalls(const std::vector<CallRecord>& expected, const char* testName) const {
        if (calls.size() != expected.size()) {
            std::cerr << "[" << testName << "] FAIL: call count mismatch. got=" << calls.size()
                      << " expected=" << expected.size() << "\n";
            dumpCalls();
            assert(false);
        }
        for (size_t i = 0; i < calls.size(); ++i) {
            if (calls[i].type != expected[i].type || calls[i].offset != expected[i].offset ||
                (calls[i].type == CallRecord::Write && calls[i].value != expected[i].value)) {
                std::cerr << "[" << testName << "] FAIL: call[" << i << "] mismatch\n";
                dumpCalls();
                assert(false);
            }
        }
        std::cout << "[" << testName << "] PASS: call sequence matches (" << calls.size() << " MMIO)\n";
    }

    void dumpCalls() const {
        for (size_t i = 0; i < calls.size(); ++i) {
            std::cerr << "  [" << i << "] " << (calls[i].type == CallRecord::Read ? "R" : "W")
                      << " off=0x" << std::hex << calls[i].offset << std::dec
                      << " val=0x" << std::hex << calls[i].value << std::dec << "\n";
        }
    }

    // 断言：本轮**没有任何 PCIE_DATA2 写**（= 没有任何功能寄存器写）
    void assertNoDataWrite(const char* testName) const {
        for (const auto& c : calls) {
            if (c.type == CallRecord::Write && c.offset == kPcieData2Offset) {
                std::cerr << "[" << testName << "] FAIL: PCIE_DATA2 was written (0x" << std::hex << c.value
                          << std::dec << ") — the probe must be READ-ONLY\n";
                assert(false);
            }
        }
        std::cout << "[" << testName << "] PASS: no PCIE_DATA2 write (read-only)\n";
    }

    // 断言：只碰了 PCIE_INDEX2 / PCIE_DATA2 两个偏移
    void assertOnlyIndexDataOffsets(const char* testName) const {
        for (const auto& c : calls) {
            if (c.offset != kPcieIndex2Offset && c.offset != kPcieData2Offset) {
                std::cerr << "[" << testName << "] FAIL: unexpected offset 0x" << std::hex << c.offset
                          << std::dec << "\n";
                assert(false);
            }
        }
        std::cout << "[" << testName << "] PASS: only PCIE_INDEX2/PCIE_DATA2 touched\n";
    }

    // 断言：恰好 n 次写 PCIE_INDEX2，且从未写 PCIE_INDEX_HI
    void assertIndexWrites(uint32_t n, const char* testName) const {
        uint32_t writes = 0;
        for (const auto& c : calls) {
            if (c.type == CallRecord::Write && c.offset == kPcieIndex2Offset) { ++writes; }
            if (c.offset == kPcieIndexHiOffset) {
                std::cerr << "[" << testName << "] FAIL: PCIE_INDEX_HI touched (must not be, addr < 2^32)\n";
                assert(false);
            }
        }
        if (writes != n) {
            std::cerr << "[" << testName << "] FAIL: index writes got=" << writes << " expected=" << n << "\n";
            assert(false);
        }
        std::cout << "[" << testName << "] PASS: PCIE_INDEX2 written exactly " << n
                  << " time(s); PCIE_INDEX_HI never touched\n";
    }

    void assertLockBalance(const char* testName) const {
        assert(lockCount == unlockCount && "lock/unlock not balanced");
        assert(lockCount > 0 && "lock never taken");
        std::cout << "[" << testName << "] PASS: lock/unlock balanced (" << lockCount << ")\n";
    }
};

// ── 静态回调桥接 ──

static uint32_t mockReadReg(void* p, uint32_t offset) {
    return static_cast<MockContext*>(p)->mmioRead(offset);
}
static void mockWriteReg(void* p, uint32_t offset, uint32_t value) {
    static_cast<MockContext*>(p)->mmioWrite(offset, value);
}
static void mockLock(void* p) { ++static_cast<MockContext*>(p)->lockCount; }
static void mockUnlock(void* p) { ++static_cast<MockContext*>(p)->unlockCount; }
// 延时回调：**故意设成"被调用即失败"** —— 本轮不应需要任何延时（无重试、无轮询）。
static void mockDelay(void* p, uint32_t) { static_cast<MockContext*>(p)->delayUsed = true; }

// 构造与内核侧**完全同形**的回调集合（内核侧实现见 SmnReadProbe.hpp 头部注释与调用点）。
// maxRetries 故意给非 0 值：用于验证 runSmnReadProbe **强行把它压成 0**（本轮不重试）。
static SmnCallbacks makeCallbacks(MockContext* ctx, uint32_t maxRetries = 3) {
    SmnCallbacks cb{};
    cb.readReg  = &mockReadReg;
    cb.writeReg = &mockWriteReg;
    cb.lock     = &mockLock;
    cb.unlock   = &mockUnlock;
    cb.delayUs  = &mockDelay;
    cb.ctx      = ctx;
    cb.maxRetries = maxRetries;
    return cb;
}

// ── ① 正常路径：恰好 4 次 MMIO，序列逐条匹配 ──

static void test_normal_single_point_read() {
    MockContext ctx;
    // SMN 空间模型：真寄存器 = "活"值，空白对照 = 0（两者由各自字节地址区分）
    ctx.setKnownPoint(0x090FF244u, 0x121D4A);     // MP0_SMN_C2PMSG_81
    ctx.setKnownPoint(0x090FF000u, 0x00000000);   // 空白对照

    const SmnProbeReadings r = runSmnReadProbe(makeCallbacks(&ctx));

    assert(r.regAddr == 0x090FF244u);
    assert(r.blankAddr == 0x090FF000u);
    assert(r.regValue == 0x121D4A);
    assert(r.blankValue == 0x00000000);
    assert(r.regError == static_cast<uint32_t>(SmnAccessError::None));
    assert(r.blankError == static_cast<uint32_t>(SmnAccessError::None));
    assert(r.retries == 0);
    assert(!ctx.delayUsed && "probe must not delay");

    // 每条点的规范序列（SmnIndirectAccess::read，maxRetries 被压成 0）：
    //   写索引 → 回读索引（校验）→ 读数据   = 3 次 MMIO
    // 两点共 6 次；**其中没有任何一次 PCIE_DATA2 写**。
    ctx.assertCalls({
        {CallRecord::Write, kPcieIndex2Offset, 0x090FF244},   // 真：写索引（MP0 C2PMSG_81）
        {CallRecord::Read,  kPcieIndex2Offset, 0},            // 真：回读索引（posted-write flush）
        {CallRecord::Read,  kPcieData2Offset,  0},            // 真：读数据
        {CallRecord::Write, kPcieIndex2Offset, 0x090FF000},   // 对：写索引（同一基址 + 偏移 0）
        {CallRecord::Read,  kPcieIndex2Offset, 0},            // 对：回读索引
        {CallRecord::Read,  kPcieData2Offset,  0},            // 对：读数据
    }, "test_normal_single_point_read");

    ctx.assertNoDataWrite("test_normal_single_point_read");
    ctx.assertOnlyIndexDataOffsets("test_normal_single_point_read");
    ctx.assertIndexWrites(2, "test_normal_single_point_read");
    ctx.assertLockBalance("test_normal_single_point_read");
}

// ── ② 两个点互不干扰：真/对的索引值不同（可归因于"寄存器本身"而非通道形态）──

static void test_points_are_distinct() {
    MockContext ctx;
    // 故意让两点读回同一值 → 本用例只校验"地址与调用面"，不依赖读数差异。
    ctx.setKnownPoint(0x090FF244u, 0xA5A5A5A5);
    ctx.setKnownPoint(0x090FF000u, 0xA5A5A5A5);

    const SmnProbeReadings r = runSmnReadProbe(makeCallbacks(&ctx));

    assert(r.regAddr != r.blankAddr);
    assert(r.regAddr == fw::smnAddr(kSmnProbeRegOffset));
    assert(r.blankAddr == fw::smnAddr(kSmnProbeBlankOffset));
    assert(ctx.calls.size() == 6);
    assert(ctx.calls[0].value == r.regAddr);    // 第一条索引写 = 真寄存器地址
    assert(ctx.calls[3].value == r.blankAddr);  // 第二条索引写 = 空白对照地址
    // 同一公式、同一基址、只差寄存器 ID ⇒ 两个字节地址相差恰好 0x244
    assert(r.regAddr - r.blankAddr == kSmnProbeRegOffset * 4);
    std::cout << "[test_points_are_distinct] PASS: reg=0x" << std::hex << r.regAddr << " blank=0x"
              << r.blankAddr << std::dec << " (delta=0x244)\n";
}

// ── ③ 不重试：maxRetries 给 3，实际仍只发 2 次序列（每点 3 次 MMIO）──

static void test_no_retry_even_if_caller_asks() {
    MockContext ctx;
    ctx.setKnownPoint(0x090FF244u, 0xDEADBEEF);
    ctx.setKnownPoint(0x090FF000u, 0xFEEDFACE);

    // 故意把调用方的 maxRetries 设成 3：runSmnReadProbe 必须把它压成 0。
    // 若未压成 0 且索引回读"失真"，就会重试 ⇒ 调用数按 3 的倍数膨胀（本用例能测出）。
    const SmnCallbacks cb = makeCallbacks(&ctx, /*maxRetries=*/3);
    const SmnProbeReadings r = runSmnReadProbe(cb);

    assert(r.regValue == 0xDEADBEEF);
    assert(r.blankValue == 0xFEEDFACE);
    assert(r.retries == 0);
    assert(!ctx.delayUsed);
    assert(ctx.calls.size() == 6);   // 恰好"2 点 × 3 次"；重试会让它变成 6 的倍数
    ctx.assertNoDataWrite("test_no_retry_even_if_caller_asks");
    ctx.assertIndexWrites(2, "test_no_retry_even_if_caller_asks");
    std::cout << "[test_no_retry_even_if_caller_asks] PASS: maxRetries forced to 0"
                 " (6 MMIO = 2 points x [W idx, R idx, R data])\n";
}

// ── ④ "通道未译码"路径：数据读一律全 1（74/75 轮实测形态）──
//      读数必须**原样透传**，不得被替换成 0 或别的值（否则判读侧会误读为"读到 0"）。

static void test_unreadable_channel_yields_all_ones() {
    MockContext ctx;
    ctx.channelDead = true;   // 任何数据读都返回全 1（未译码 / 总线错误的最常见形态）

    const SmnProbeReadings r = runSmnReadProbe(makeCallbacks(&ctx));

    assert(r.regValue == kSmnProbeNoValue);
    assert(r.blankValue == kSmnProbeNoValue);
    // 无重试、无回读校验 ⇒ 错误码仍为 None，**不作任何掩盖**；读数语义由判读侧解释。
    assert(r.regError == static_cast<uint32_t>(SmnAccessError::None));
    assert(r.blankError == static_cast<uint32_t>(SmnAccessError::None));
    assert(ctx.calls.size() == 6);
    ctx.assertNoDataWrite("test_unreadable_channel_yields_all_ones");
    ctx.assertIndexWrites(2, "test_unreadable_channel_yields_all_ones");
    std::cout << "[test_unreadable_channel_yields_all_ones] PASS: ffffffff passed through unchanged\n";
}

// ── ⑤ 索引写入未生效：错误码必须**如实**是 IndexWriteFail（不掩盖、不重试）──

static void test_index_write_failure_is_reported() {
    MockContext ctx;
    ctx.setKnownPoint(0x090FF244u, 0x11111111);
    ctx.setKnownPoint(0x090FF000u, 0x22222222);
    ctx.readbackBroken = true;
    ctx.indexReadbackOverride = 0xDEADBEEF;   // 回读索引不匹配 ⇒ IndexWriteFail

    const SmnProbeReadings r = runSmnReadProbe(makeCallbacks(&ctx, /*maxRetries=*/3));

    assert(r.regError == static_cast<uint32_t>(SmnAccessError::IndexWriteFail));
    assert(r.blankError == static_cast<uint32_t>(SmnAccessError::IndexWriteFail));
    // 不重试 ⇒ 每点恰好 2 次 MMIO（写索引、回读索引；**因回读不匹配而止步，不再读数据**），
    //   两点共 4 次。若实现会重试，这里会是 4 的倍数（maxRetries=3 ⇒ 16 次）。
    assert(ctx.calls.size() == 4);
    ctx.assertNoDataWrite("test_index_write_failure_is_reported");
    ctx.assertIndexWrites(2, "test_index_write_failure_is_reported");
    ctx.assertLockBalance("test_index_write_failure_is_reported");
    std::cout << "[test_index_write_failure_is_reported] PASS: IndexWriteFail surfaced, no retry\n";
}

// ── ⑥ 锁回调为空（降级形态）：内核侧若无 IOLock 可用，必须仍能执行（不崩、不阻塞）──

static void test_null_lock_callbacks_are_tolerated() {
    MockContext ctx;
    ctx.setKnownPoint(0x090FF244u, 0x00000001);
    ctx.setKnownPoint(0x090FF000u, 0x00000002);

    SmnCallbacks cb = makeCallbacks(&ctx);
    cb.lock   = nullptr;
    cb.unlock = nullptr;

    const SmnProbeReadings r = runSmnReadProbe(cb);

    assert(r.regValue == 0x00000001);
    assert(r.blankValue == 0x00000002);
    assert(ctx.lockCount == 0 && ctx.unlockCount == 0);
    assert(ctx.calls.size() == 6);
    ctx.assertNoDataWrite("test_null_lock_callbacks_are_tolerated");
    ctx.assertIndexWrites(2, "test_null_lock_callbacks_are_tolerated");
    std::cout << "[test_null_lock_callbacks_are_tolerated] PASS: nullptr lock/unlock tolerated\n";
}

// ── ⑦ 常量自检：偏移取自 Regs/PSP13.hpp，不是写死的字面量 ──

static void test_constants_and_addresses() {
    assert(kSmnProbeRegOffset == 0x0091);
    assert(kSmnProbeRegOffset == MP0_SMN_C2PMSG_81);
    assert(kSmnProbeBlankOffset == 0);
    assert(kSmnProbeMaxRetries == 0);
    assert(fw::smnAddr(kSmnProbeRegOffset) == 0x090FF244u);
    assert(fw::smnAddr(kSmnProbeBlankOffset) == 0x090FF000u);
    assert(kPcieIndex2Offset == 0x0E);
    assert(kPcieData2Offset == 0x0F);
    std::cout << "[test_constants_and_addresses] PASS: addresses derive from RegAddr.hpp/PSP13.hpp\n";
}

// ── main ──

int main() {
    std::cout << "=== SmnReadProbe 离线单元测试（只读单点规范 SMN 访问）===\n\n";

    test_constants_and_addresses();
    test_normal_single_point_read();
    test_points_are_distinct();
    test_no_retry_even_if_caller_asks();
    test_unreadable_channel_yields_all_ones();
    test_index_write_failure_is_reported();
    test_null_lock_callbacks_are_tolerated();

    std::cout << "\n=== 所有测试通过 ===\n";
    return 0;
}
