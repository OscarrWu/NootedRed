// PSP 13.0.4 GPCOM 环验收测试（用户态纯逻辑）
//
// 覆盖：
//   ① 正常路径：帧提交 + fence 等待
//   ② 超时路径：fence 永不满足 ⇒ 必须返回失败且不死循环
//   ③ 帧构造验证：cmdBufCopy → 读取验证
//
// 编译运行：
//   make -f src/NootedRed/FwBringup/tests/Makefile test
//
// 依据：docs/子任务/乙线自建固件层作战计划.md T5 验收判据

#include "FwBringup/Psp13Ring.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace fw;
using namespace display;

// ── 测试用 RegSink 替身 ──
struct MockSink final : public RegSink {
    enum OpKind { Write, Read, Delay };
    struct RecordedOp {
        OpKind   kind;
        RegAddr  addr;
        RegValue value;
    };
    std::vector<RecordedOp> ops;

    // 预置读值（地址 → 值映射）
    struct ReadStub { RegAddr addr; RegValue value; };
    std::vector<ReadStub> stubs;

    void reset() { ops.clear(); stubs.clear(); }

    void addStub(RegAddr a, RegValue v) { stubs.push_back({a, v}); }

    // RegSink 接口
    RegValue read(RegAddr addr) override {
        RegValue val = 0;
        for (auto& s : stubs) {
            if (s.addr == addr) { val = s.value; break; }
        }
        ops.push_back({Read, addr, val});
        return val;
    }

    void write(RegAddr addr, RegValue val) override {
        ops.push_back({Write, addr, val});
    }

    void delayMicroseconds(uint32_t) override {
        ops.push_back({Delay, 0, 0});
    }
};

// ── 静态环缓冲（固定大小，无动态分配） ──
static uint8_t  s_ring_buf[kRingSizeBytes];
static uint8_t  s_cmd_buf[kCmdBufSize];
static uint32_t s_fence_val;

static RingState s_rs;

static void resetStaticBufs() {
    std::memset(s_ring_buf, 0, sizeof(s_ring_buf));
    std::memset(s_cmd_buf, 0, sizeof(s_cmd_buf));
    s_fence_val = 0;
    ringInit(&s_rs, s_ring_buf, s_cmd_buf, &s_fence_val);
}

// ── 辅助断言 ──
static void assertOp(const MockSink::RecordedOp& op,
                     MockSink::OpKind kind, RegAddr addr, RegValue val) {
    assert(op.kind == kind);
    assert(op.addr == addr);
    assert(op.value == val);
}

// ════════════════════════════════════════════════════════════════════
// 测试 1：正常路径 —— ringSubmitFrame + ringWaitForFence
// ════════════════════════════════════════════════════════════════════
static void testNormalSubmit() {
    printf("[test 1] Normal submit + fence wait... ");
    MockSink sink;
    resetStaticBufs();

    // 预置 wptr = 0 的读 stub
    sink.addStub(kC2PMSG67, 0);

    const uint64_t cmd_mc = 0x10000000ULL;
    const uint64_t fence_mc = 0x20000000ULL;

    // 提交帧
    int ret = ringSubmitFrame(&s_rs, sink, cmd_mc, fence_mc);
    assert(ret == 0);

    // 检查寄存器操作序列：
    //   1. read C2PMSG67 → 0
    //   2. write C2PMSG67 → (0 + RbFrameSizeDw) % RingSizeDw
    assert(sink.ops.size() == 2);
    assertOp(sink.ops[0], MockSink::Read,  kC2PMSG67, 0);

    const uint32_t expected_wptr = kRbFrameSizeDw; // 16 dwords
    assertOp(sink.ops[1], MockSink::Write, kC2PMSG67, expected_wptr);

    // 验证帧内容
    RbFrame* frame = reinterpret_cast<RbFrame*>(s_ring_buf);
    assert(frame->cmd_buf_addr_lo == 0x10000000);
    assert(frame->cmd_buf_addr_hi == 0);
    assert(frame->fence_addr_lo   == 0x20000000);
    assert(frame->fence_addr_hi   == 0);
    assert(frame->fence_value     == 1); // first fence

    // fence 完成
    s_fence_val = 1;
    ret = ringWaitForFence(&s_rs, sink, 1);
    assert(ret == 0);

    printf("PASS\n");
}

// ════════════════════════════════════════════════════════════════════
// 测试 2：超时路径 —— fence 永不满足 ⇒ 必须返回失败且不死循环
// ════════════════════════════════════════════════════════════════════
static void testFenceTimeout() {
    printf("[test 2] Fence timeout... ");
    MockSink sink;
    resetStaticBufs();

    // fence 永不置为 42
    // ringWaitForFence 有 kFenceTimeout 上限，不会死循环
    int ret = ringWaitForFence(&s_rs, sink, 42);
    assert(ret == -1); // 必须返回超时

    printf("PASS (timeout=%d iterations)\n", kFenceTimeout);
}

// ════════════════════════════════════════════════════════════════════
// 测试 3：帧构造 —— cmdBufCopy + cmdBufGet 验证
// ════════════════════════════════════════════════════════════════════
static void testCmdBufCopy() {
    printf("[test 3] Command buffer copy and verify... ");
    resetStaticBufs();

    GfxCmdResp cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.buf_size    = kCmdBufSize;
    cmd.buf_version = 1;
    cmd.cmd_id      = GFX_CMD_ID_LOAD_TOC;
    cmd.cmd_payload[0] = 0x12345678;
    cmd.cmd_payload[1] = 0x9ABCDEF0;
    cmd.cmd_payload[2] = 0x100;

    cmdBufCopy(&s_rs, &cmd);

    const GfxCmdResp* readback = cmdBufGet(&s_rs);
    assert(readback->buf_size    == kCmdBufSize);
    assert(readback->buf_version == 1);
    assert(readback->cmd_id      == GFX_CMD_ID_LOAD_TOC);
    assert(readback->cmd_payload[0] == 0x12345678);
    assert(readback->cmd_payload[1] == 0x9ABCDEF0);
    assert(readback->cmd_payload[2] == 0x100);
    assert(readback->resp_status    == 0);
    assert(readback->resp_tmr_size  == 0);

    printf("PASS\n");
}

// ════════════════════════════════════════════════════════════════════
// 测试 4：waitReg 超时
// ════════════════════════════════════════════════════════════════════
static void testWaitRegTimeout() {
    printf("[test 4] waitReg timeout (sink never satisfies)... ");
    MockSink sink;
    // 不含 stub：所有 read 返回 0，C2PMSG35 的 bit31 永远不置位
    int ret = waitReg(sink, kC2PMSG35, kBlReadyFlag, kBlReadyFlag, false);
    assert(ret == -1); // must timeout

    printf("PASS (timeout=%d us iterations)\n", kRegPollUs);
}

// ════════════════════════════════════════════════════════════════════
// 主函数
// ════════════════════════════════════════════════════════════════════
int main() {
    testNormalSubmit();
    testFenceTimeout();
    testCmdBufCopy();
    testWaitRegTimeout();
    printf("\n所有 PSP13 环测试通过。\n");
    return 0;
}
