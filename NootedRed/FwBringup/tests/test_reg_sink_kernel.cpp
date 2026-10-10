// RegSinkKernel 离线单元测试（用户态，mock 回调）
//
// 覆盖：
//   ① 正常读/写（验证调用序列与返回值）
//   ② 回读刷写确实发生（断言调用顺序：写索引→回读索引→读数据→清HI）
//   ③ 高地址时 HI 被写且被清（模拟 addr >> 32 != 0 的分支）
//   ④ 超时/错误路径（索引回读不匹配、HI 回读不匹配、数据回读不匹配、重试耗尽）
//
// 编译运行：
//   make -f src/NootedRed/FwBringup/tests/Makefile test
//
// 依据：docs/子任务/乙线SMN安全访问调查.md §3.3/§3.4

#include "FwBringup/RegSinkKernel.hpp"
#include "DisplaySeq/RegOp.hpp"
#include "FwBringup/RegAddr.hpp"
#include <vector>
#include <cassert>
#include <cstdint>
#include <iostream>

using namespace fw;
using namespace display;

// ── 测试基础设施 ──

struct CallRecord {
    enum Type { Read, Write } type;
    uint32_t offset;
    uint32_t value;  // Write 时的值，Read 时忽略
};

struct MockContext {
    std::vector<CallRecord> calls;
    std::vector<uint32_t> readReturns;  // 预置的读返回值队列
    size_t readIndex = 0;
    bool lockCalled = false;
    bool unlockCalled = false;
    uint32_t lastError = 0;

    // 预置读返回值（按调用顺序消费）
    void pushReadReturn(uint32_t v) { readReturns.push_back(v); }

    uint32_t mmioRead(uint32_t offset) {
        calls.push_back({CallRecord::Read, offset, 0});
        if (readIndex < readReturns.size()) {
            return readReturns[readIndex++];
        }
        return 0xFFFFFFFF;  // 默认返回全 1（模拟总线错误）
    }

    void mmioWrite(uint32_t offset, uint32_t value) {
        calls.push_back({CallRecord::Write, offset, value});
    }

    void lock() { lockCalled = true; }
    void unlock() { unlockCalled = true; }

    void reset() {
        calls.clear();
        readReturns.clear();
        readIndex = 0;
        lockCalled = false;
        unlockCalled = false;
    }

    // 断言调用序列匹配
    void assertCalls(const std::vector<CallRecord>& expected, const char* testName) {
        if (calls.size() != expected.size()) {
            std::cerr << "[" << testName << "] FAIL: call count mismatch. got=" << calls.size()
                      << " expected=" << expected.size() << "\n";
            for (size_t i = 0; i < calls.size(); ++i) {
                std::cerr << "  [" << i << "] " << (calls[i].type == CallRecord::Read ? "R" : "W")
                          << " off=0x" << std::hex << calls[i].offset << std::dec
                          << " val=0x" << std::hex << calls[i].value << std::dec << "\n";
            }
            assert(false);
        }
        for (size_t i = 0; i < calls.size(); ++i) {
            if (calls[i].type != expected[i].type ||
                calls[i].offset != expected[i].offset ||
                (calls[i].type == CallRecord::Write && calls[i].value != expected[i].value)) {
                std::cerr << "[" << testName << "] FAIL: call[" << i << "] mismatch\n";
                std::cerr << "  got:      " << (calls[i].type == CallRecord::Read ? "R" : "W")
                          << " off=0x" << std::hex << calls[i].offset << std::dec
                          << " val=0x" << std::hex << calls[i].value << std::dec << "\n";
                std::cerr << "  expected: " << (expected[i].type == CallRecord::Read ? "R" : "W")
                          << " off=0x" << std::hex << expected[i].offset << std::dec
                          << " val=0x" << std::hex << expected[i].value << std::dec << "\n";
                assert(false);
            }
        }
        std::cout << "[" << testName << "] PASS: call sequence matches (" << calls.size() << " calls)\n";
    }

    void assertLockUnlock(const char* testName) {
        assert(lockCalled && "lock not called");
        assert(unlockCalled && "unlock not called");
        std::cout << "[" << testName << "] PASS: lock/unlock called\n";
    }
};

// ── 静态回调桥接函数（零分配：普通函数指针 + void* 上下文）──

static uint32_t mockReadReg(void* p, uint32_t offset) {
    return static_cast<MockContext*>(p)->mmioRead(offset);
}

static void mockWriteReg(void* p, uint32_t offset, uint32_t value) {
    static_cast<MockContext*>(p)->mmioWrite(offset, value);
}

static void mockLock(void* p) {
    static_cast<MockContext*>(p)->lock();
}

static void mockUnlock(void* p) {
    static_cast<MockContext*>(p)->unlock();
}

// 构造 SmnCallbacks（delayUs 留 nullptr，maxRetries 可指定）
static SmnCallbacks makeCallbacks(MockContext* ctx, uint32_t maxRetries = 3) {
    SmnCallbacks cb{};
    cb.readReg = &mockReadReg;
    cb.writeReg = &mockWriteReg;
    cb.lock = &mockLock;
    cb.unlock = &mockUnlock;
    cb.delayUs = nullptr;
    cb.ctx = ctx;
    cb.maxRetries = maxRetries;
    return cb;
}

// RegSeq 需要外部缓冲区
static RegOp g_seqBuffer[256];

// ── 测试用例 ──

// ① 正常读：地址 < 4GB（无 HI），单次成功。RegSinkKernel 默认 SEG0：
//    read(0x91) → 字节地址 (kMpSeg0Base + 0x91) * 4 = 0x00058244
static void test_normal_read() {
    MockContext ctx;
    ctx.pushReadReturn(0x00058244);  // 回读 PCIE_INDEX2（= 写入的 SEG0 字节地址）
    ctx.pushReadReturn(0xDEADBEEF);  // 读 PCIE_DATA2

    RegSinkKernel sink(makeCallbacks(&ctx));

    uint32_t val = sink.read(0x91);

    assert(val == 0xDEADBEEF);
    assert(sink.lastError() == SmnAccessError::None);

    ctx.assertCalls({
        {CallRecord::Write, kPcieIndex2Offset, 0x00058244},
        {CallRecord::Read,  kPcieIndex2Offset, 0},
        {CallRecord::Read,  kPcieData2Offset,  0},
    }, "test_normal_read");
    ctx.assertLockUnlock("test_normal_read");
}

// ② 正常写：地址 < 4GB（无 HI），单次成功
static void test_normal_write() {
    MockContext ctx;
    ctx.pushReadReturn(0x00058244);  // 回读 PCIE_INDEX2（= 写入的 SEG0 字节地址）
    ctx.pushReadReturn(0xCAFEBABE);  // 回读 PCIE_DATA2（写后 flush）

    RegSinkKernel sink(makeCallbacks(&ctx));

    sink.write(0x91, 0xCAFEBABE);

    assert(sink.lastError() == SmnAccessError::None);
    assert(sink.lastValue() == 0xCAFEBABE);

    ctx.assertCalls({
        {CallRecord::Write, kPcieIndex2Offset, 0x00058244},
        {CallRecord::Read,  kPcieIndex2Offset, 0},
        {CallRecord::Write, kPcieData2Offset,  0xCAFEBABE},
        {CallRecord::Read,  kPcieData2Offset,  0},
    }, "test_normal_write");
    ctx.assertLockUnlock("test_normal_write");
}

// ③ 高地址（需要 HI）：addr >> 32 != 0，验证 HI 写入与清零
static void test_high_address_hi_written_and_cleared() {
    MockContext ctx;
    const uint64_t highAddr = 0x1090FF244ull;

    ctx.pushReadReturn(0x090FF244);  // 回读 INDEX2
    ctx.pushReadReturn(0x01);        // 回读 INDEX_HI
    ctx.pushReadReturn(0x12345678);  // 读 DATA2
    ctx.pushReadReturn(0x00);        // 回读 INDEX_HI 清零确认

    RegSinkKernel sink(makeCallbacks(&ctx));

    auto result = sink.read64(highAddr);

    assert(*result == 0x12345678);
    assert(result.error == SmnAccessError::None);

    ctx.assertCalls({
        {CallRecord::Write, kPcieIndex2Offset,  0x090FF244},
        {CallRecord::Read,  kPcieIndex2Offset,  0},
        {CallRecord::Write, kPcieIndexHiOffset, 0x01},
        {CallRecord::Read,  kPcieIndexHiOffset, 0},
        {CallRecord::Read,  kPcieData2Offset,   0},
        {CallRecord::Write, kPcieIndexHiOffset, 0x00},
        {CallRecord::Read,  kPcieIndexHiOffset, 0},
    }, "test_high_address_hi_written_and_cleared");
    ctx.assertLockUnlock("test_high_address_hi_written_and_cleared");
}

// ④ 错误路径：索引回读不匹配 → 重试 → 最终超时
static void test_index_readback_mismatch_retry_exhaust() {
    MockContext ctx;
    for (int i = 0; i < 4; ++i) {
        ctx.pushReadReturn(0x00000000);  // 回读 INDEX2 返回 0 ≠ 写入值
        ctx.pushReadReturn(0xDEADBEEF);  // DATA2（不会被读到）
    }

    RegSinkKernel sink(makeCallbacks(&ctx));
    uint32_t val = sink.read(0x91);

    assert(val == 0xFFFFFFFF);
    assert(sink.lastError() == SmnAccessError::IndexWriteFail);  // 重试耗尽返回最后一个错误

    // 验证共尝试 4 次（每次：W INDEX2 → R INDEX2）
    assert(ctx.calls.size() == 4 * 2);  // 4 attempts × 2 mmio calls each
    assert(ctx.lockCalled && ctx.unlockCalled);
    std::cout << "[test_index_readback_mismatch_retry_exhaust] PASS: retries exhausted, IndexWriteFail error\n";
}

// ⑤ 错误路径：HI 回读不匹配（单次尝试，maxRetries=0）
static void test_hi_readback_mismatch() {
    MockContext ctx;
    const uint64_t highAddr = 0x1090FF244ull;

    ctx.pushReadReturn(0x090FF244);  // 回读 INDEX2 OK
    ctx.pushReadReturn(0xFF);        // 回读 INDEX_HI 不匹配
    ctx.pushReadReturn(0x12345678);  // DATA2
    ctx.pushReadReturn(0x00);        // 清零确认

    RegSinkKernel sink(makeCallbacks(&ctx, 0));  // maxRetries=0 → 仅尝试 1 次，直接返回 HiWriteFail

    auto result = sink.read64(highAddr);

    assert(*result == 0xFFFFFFFF);
    assert(result.error == SmnAccessError::HiWriteFail);
    std::cout << "[test_hi_readback_mismatch] PASS: HiWriteFail error\n";
}

// ⑥ 错误路径：写数据后回读不匹配（单次尝试）
static void test_data_write_readback_mismatch() {
    MockContext ctx;

    ctx.pushReadReturn(0x090FF244);  // 回读 INDEX2 OK
    ctx.pushReadReturn(0x00000000);  // 回读 DATA2 不匹配

    RegSinkKernel sink(makeCallbacks(&ctx, 0));  // maxRetries=0

    auto err = sink.write64(fw::smnAddr(0x91), 0xCAFEBABE);

    assert(err == SmnAccessError::DataWriteFail);
    std::cout << "[test_data_write_readback_mismatch] PASS: DataWriteFail error\n";
}

// ⑦ 错误路径：清 HI 后回读不为 0（单次尝试）
static void test_hi_clear_readback_nonzero() {
    MockContext ctx;
    const uint64_t highAddr = 0x1090FF244ull;

    ctx.pushReadReturn(0x090FF244);  // 回读 INDEX2
    ctx.pushReadReturn(0x01);        // 回读 INDEX_HI
    ctx.pushReadReturn(0x12345678);  // 读 DATA2
    ctx.pushReadReturn(0x01);        // 回读 INDEX_HI 清零确认 → 失败

    RegSinkKernel sink(makeCallbacks(&ctx, 0));  // maxRetries=0

    auto result = sink.read64(highAddr);

    assert(*result == 0xFFFFFFFF);
    assert(result.error == SmnAccessError::HiClearFail);
    std::cout << "[test_hi_clear_readback_nonzero] PASS: HiClearFail error\n";
}

// ⑧ 通过 RegSink::execute 验证 Poll 语义（超时返回 false）
static void test_poll_timeout() {
    MockContext ctx;
    for (int i = 0; i < 5; ++i) {
        ctx.pushReadReturn(0x00058244);  // 回读 INDEX2 匹配（SEG0 字节地址）
        ctx.pushReadReturn(0x00000000);  // 读 DATA2 返回 0（busy）
    }

    RegSinkKernel sink(makeCallbacks(&ctx));
    sink.setPollLimits(5, 0);

    RegOp pollOp = regPollUntilNot(0x91, 0x0, "poll_test");
    bool ok = sink.execute(pollOp);

    assert(!ok);  // 超时返回 false
    // Poll 超时不设置 lastError（最后一次 read 成功返回 0），仅靠返回值判断
    assert(sink.lastError() == SmnAccessError::None);
    std::cout << "[test_poll_timeout] PASS: poll timeout returns false\n";
}

// ⑨ 通过 RegSink::execute 验证 Poll 成功（读到非 busy 值）
static void test_poll_success() {
    MockContext ctx;
    for (int i = 0; i < 2; ++i) {
        ctx.pushReadReturn(0x00058244);
        ctx.pushReadReturn(0x00000000);
    }
        ctx.pushReadReturn(0x00058244);
    ctx.pushReadReturn(0x00000001);

    RegSinkKernel sink(makeCallbacks(&ctx));
    sink.setPollLimits(10, 0);

    RegOp pollOp = regPollUntilNot(0x91, 0x0, "poll_test");
    bool ok = sink.execute(pollOp);

    assert(ok);
    assert(sink.lastValue() == 0x01);
    assert(sink.lastError() == SmnAccessError::None);
    std::cout << "[test_poll_success] PASS: poll succeeds on ready value\n";
}

// ⑩ 验证 RegSink::executeAll 顺序执行
static void test_execute_all_sequence() {
    MockContext ctx;
        ctx.pushReadReturn(0x00058244);
    ctx.pushReadReturn(0xCAFEBABE);
        ctx.pushReadReturn(0x00058244);
    ctx.pushReadReturn(0xDEADBEEF);
        ctx.pushReadReturn(0x00058244);
    ctx.pushReadReturn(0x00000000);
        ctx.pushReadReturn(0x00058244);
    ctx.pushReadReturn(0x00000001);

    RegSinkKernel sink(makeCallbacks(&ctx));
    sink.setPollLimits(10, 0);

    RegSeq seq(g_seqBuffer, 256);
    seq.push(regWrite(0x91, 0xCAFEBABE, "write"));
    seq.push(regRead(0x91, "read"));
    seq.push(regPollUntilNot(0x91, 0x0, "poll"));

    size_t executed = sink.executeAll(seq);

    assert(executed == 3);
    assert(sink.lastError() == SmnAccessError::None);
    std::cout << "[test_execute_all_sequence] PASS: executeAll runs full sequence\n";
}

// ── main ──

int main() {
    std::cout << "=== RegSinkKernel 离线单元测试 ===\n\n";

    test_normal_read();
    test_normal_write();
    test_high_address_hi_written_and_cleared();
    test_index_readback_mismatch_retry_exhaust();
    test_hi_readback_mismatch();
    test_data_write_readback_mismatch();
    test_hi_clear_readback_nonzero();
    test_poll_timeout();
    test_poll_success();
    test_execute_all_sequence();

    std::cout << "\n=== 所有测试通过 ===\n";
    return 0;
}
