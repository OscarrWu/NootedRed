// SMU13 邮箱握手原语验收测试（用户态纯逻辑）
//
// 覆盖：
//   ① 正常路径（响应 OK = 0x01）
//   ② 超时路径（sink 永远返回 0 ⇒ 必须返回 NoResponse 且不能死循环）
//   ③ 失败码路径（响应 0xFF）
//   ④ 断言实际产生的寄存器读写顺序与值（清响应→写参数→写命令→轮询）
//
// 编译运行：
//   make -f src/NootedRed/FwBringup/tests/Makefile test
//
// 依据：docs/子任务/乙线自建固件层作战计划.md T3 验收判据

#include "FwBringup/Smu13Mailbox.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace fw;
using namespace display;

// ── 测试用 RegSink 替身（记录操作序列 + 预置读值） ──
// 参照 DisplaySeq/RegSinkUser.hpp 的做法，但不受 final 类限制。
struct MockSink final : public RegSink {
    enum OpKind { Write, Read, Delay };
    struct RecordedOp {
        OpKind   kind;
        RegAddr  addr;
        RegValue value;
    };
    std::vector<RecordedOp> ops;

    struct Preset {
        RegAddr addr;
        RegValue value;
    };
    Preset presets[32];
    std::size_t presetCount = 0;

    void presetRead(RegAddr addr, RegValue val) {
        assert(presetCount < 32);
        presets[presetCount++] = {addr, val};
    }

    RegValue lookupPreset(RegAddr addr) const {
        for (std::size_t i = 0; i < presetCount; ++i) {
            if (presets[i].addr == addr) return presets[i].value;
        }
        return 0;
    }

    // RegSink 接口（纯虚）
    RegValue read(RegAddr addr) override {
        ops.push_back({Read, addr, 0});
        RegValue v = lookupPreset(addr);
        // 父类 execute() 在 Poll 循环中调用我们的 read()，然后将返回值赋给 lastValue_
        return v;
    }

    void write(RegAddr addr, RegValue val) override {
        ops.push_back({Write, addr, val});
    }

    void delayMicroseconds(uint32_t us) override {
        ops.push_back({Delay, 0, us});
    }

    void clear() { ops.clear(); presetCount = 0; }
};

// ── 断言辅助 ──
static void assertWrite(const MockSink& sink, std::size_t idx,
                        RegAddr expectedAddr, RegValue expectedVal,
                        const char* label) {
    assert(idx < sink.ops.size());
    assert(sink.ops[idx].kind == MockSink::Write);
    assert(sink.ops[idx].addr == expectedAddr);
    assert(sink.ops[idx].value == expectedVal);
    (void)label;
}

static void assertRead(const MockSink& sink, std::size_t idx,
                       RegAddr expectedAddr, const char* label) {
    assert(idx < sink.ops.size());
    assert(sink.ops[idx].kind == MockSink::Read);
    assert(sink.ops[idx].addr == expectedAddr);
    (void)label;
}

// 统计对某地址的 Read 操作数（来自 Poll 循环）
static std::size_t countReads(const MockSink& sink, RegAddr addr) {
    std::size_t n = 0;
    for (const auto& op : sink.ops) {
        if (op.kind == MockSink::Read && op.addr == addr) ++n;
    }
    return n;
}

// ═══════════════════════════════════════════════════════════════════════════
// 测试用例
// ═══════════════════════════════════════════════════════════════════════════

// ① 正常路径：响应 OK (0x01)
static void test_normal_ok() {
    MockSink sink;
    sink.presetRead(kSmu13RegResp, kSmuRespOk);

    uint32_t rawResp = 0;
    SmuResult res = send(sink, PhoenixPPSMC::PPSMC_MSG_TestMessage, 0x1234, &rawResp);

    assert(res == SmuResult::Ok);
    assert(rawResp == kSmuRespOk);

    // 操作序列：Write(resp=0) → Write(arg=0x1234) → Write(msg=0x01) → Read(resp 若干次)
    assert(sink.ops.size() >= 4);
    assertWrite(sink, 0, kSmu13RegResp, 0, "clear response");
    assertWrite(sink, 1, kSmu13RegArg,  0x1234, "write param");
    assertWrite(sink, 2, kSmu13RegMsg,  PhoenixPPSMC::PPSMC_MSG_TestMessage, "write msg");

    // 第 3 次起是 Poll 循环产生的 Read（首次 read 即返回 kSmuRespOk → 退出）
    assertRead(sink, 3, kSmu13RegResp, "poll read #1");

    // 确认只有 1 次 poll read（因为立即命中）
    assert(countReads(sink, kSmu13RegResp) == 1);

    puts("test_normal_ok: PASS");
}

// ② 超时路径：sink 永远返回 0 ⇒ 必须返回 NoResponse 且不能死循环
static void test_timeout_no_response() {
    MockSink sink;
    // 不预置任何值 ⇒ read() 返回 0
    // 设置小超时（10 次 × 1µs）
    sink.setPollLimits(10, 1);

    uint32_t rawResp = 0;
    SmuResult res = send(sink, PhoenixPPSMC::PPSMC_MSG_GetPmfwVersion, 0, &rawResp,
                         10, 1);  // 显式传小超时

    assert(res == SmuResult::NoResponse);
    assert(rawResp == 0);

    // 前 3 次是写
    assertWrite(sink, 0, kSmu13RegResp, 0, "clear response");
    assertWrite(sink, 1, kSmu13RegArg,  0, "write param");
    assertWrite(sink, 2, kSmu13RegMsg,  PhoenixPPSMC::PPSMC_MSG_GetPmfwVersion, "write msg");

    // Poll 循环应恰好 10 次 Read（每次返回 0，直到超时）
    assert(countReads(sink, kSmu13RegResp) == 10);

    puts("test_timeout_no_response: PASS");
}

// ③ 失败码路径：响应 0xFF (Failed)
static void test_failed_response() {
    MockSink sink;
    sink.presetRead(kSmu13RegResp, kSmuRespFailed);

    uint32_t rawResp = 0;
    SmuResult res = send(sink, PhoenixPPSMC::PPSMC_MSG_GetDriverIfVersion, 0, &rawResp);

    assert(res == SmuResult::Failed);
    assert(rawResp == kSmuRespFailed);

    assertWrite(sink, 0, kSmu13RegResp, 0, "clear response");
    assertWrite(sink, 1, kSmu13RegArg,  0, "write param");
    assertWrite(sink, 2, kSmu13RegMsg,  PhoenixPPSMC::PPSMC_MSG_GetDriverIfVersion, "write msg");
    assert(countReads(sink, kSmu13RegResp) >= 1);

    puts("test_failed_response: PASS");
}

// ④ 分离式调用：sendOnly + waitOnly
static void test_split_send_wait() {
    MockSink sink;
    sink.presetRead(kSmu13RegResp, kSmuRespOk);

    bool sent = sendOnly(sink, PhoenixPPSMC::PPSMC_MSG_TestMessage, 0xABCD);
    assert(sent);

    uint32_t rawResp = 0;
    SmuResult res = waitOnly(sink, &rawResp);

    assert(res == SmuResult::Ok);
    assert(rawResp == kSmuRespOk);

    // 发送阶段 3 次 Write
    assert(sink.ops.size() >= 3);
    assertWrite(sink, 0, kSmu13RegResp, 0, "clear response");
    assertWrite(sink, 1, kSmu13RegArg,  0xABCD, "write param");
    assertWrite(sink, 2, kSmu13RegMsg,  PhoenixPPSMC::PPSMC_MSG_TestMessage, "write msg");

    // 等待阶段有 Poll read
    assert(countReads(sink, kSmu13RegResp) >= 1);

    puts("test_split_send_wait: PASS");
}

// ⑤ 便利封装：sendTestMessage / sendGetSmuVersion / sendGetDriverIfVersion
static void test_convenience_wrappers() {
    {
        MockSink sink;
        sink.presetRead(kSmu13RegResp, kSmuRespOk);
        uint32_t rawResp = 0;
        SmuResult r1 = sendTestMessage(sink, &rawResp);
        assert(r1 == SmuResult::Ok);
        assert(rawResp == kSmuRespOk);
        assertWrite(sink, 2, kSmu13RegMsg, PhoenixPPSMC::PPSMC_MSG_TestMessage, "TestMessage ID");
    }
    {
        MockSink sink;
        sink.presetRead(kSmu13RegResp, kSmuRespOk);
        uint32_t rawResp = 0;
        SmuResult r2 = sendGetSmuVersion(sink, &rawResp);
        assert(r2 == SmuResult::Ok);
        assertWrite(sink, 2, kSmu13RegMsg, PhoenixPPSMC::PPSMC_MSG_GetPmfwVersion, "GetSmuVersion ID");
    }
    {
        MockSink sink;
        sink.presetRead(kSmu13RegResp, kSmuRespOk);
        uint32_t rawResp = 0;
        SmuResult r3 = sendGetDriverIfVersion(sink, &rawResp);
        assert(r3 == SmuResult::Ok);
        assertWrite(sink, 2, kSmu13RegMsg, PhoenixPPSMC::PPSMC_MSG_GetDriverIfVersion, "GetDriverIfVersion ID");
    }

    puts("test_convenience_wrappers: PASS");
}

// ⑥ 其他错误码：UnknownCmd (0xFE), RejectedPrereq (0xFD), RejectedBusy (0xFC)
static void test_other_error_codes() {
    struct Case { uint32_t resp; SmuResult expected; const char* name; };
    Case cases[] = {
        {kSmuRespUnknownCmd,     SmuResult::UnknownCommand,   "UnknownCmd"},
        {kSmuRespRejectedPrereq, SmuResult::RejectedPrereq,   "RejectedPrereq"},
        {kSmuRespRejectedBusy,   SmuResult::RejectedBusy,     "RejectedBusy"},
    };

    for (const auto& c : cases) {
        MockSink sink;
        sink.presetRead(kSmu13RegResp, c.resp);

        uint32_t rawResp = 0;
        SmuResult res = send(sink, PhoenixPPSMC::PPSMC_MSG_TestMessage, 0, &rawResp);

        assert(res == c.expected);
        assert(rawResp == c.resp);
    }

    puts("test_other_error_codes: PASS");
}

// ═══════════════════════════════════════════════════════════════════════════
// main
// ═══════════════════════════════════════════════════════════════════════════

int main() {
    test_normal_ok();
    test_timeout_no_response();
    test_failed_response();
    test_split_send_wait();
    test_convenience_wrappers();
    test_other_error_codes();

    puts("\n=== ALL TESTS PASSED ===");
    return 0;
}