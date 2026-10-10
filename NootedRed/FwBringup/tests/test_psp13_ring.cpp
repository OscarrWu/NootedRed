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

    // 累计"真实睡眠"的微秒数（T6 D3 判据：每轮必须真睡，不得忙等）
    uint64_t slept_us  = 0;

    void reset() { ops.clear(); stubs.clear(); slept_us = 0; }

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

    void delayMicroseconds(uint32_t us) override {
        ops.push_back({Delay, 0, us});
        slept_us += us;
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

    // ★ D3 断言：fence 已满足 ⇒ 先查后睡，零延时（等 Linux while 结构，不先睡）
    assert(sink.slept_us == 0);

    printf("PASS\n");
}

// ════════════════════════════════════════════════════════════════════
// 测试 2：超时路径 + D3 延时步进
//   fence 永不满足 ⇒ 必须返回失败且不死循环；同时每轮必须**真睡** kFencePollUs，
//   总睡眠落在 Linux `20000 × [60,100]µs` 的实时窗口内（否则真机假超时，D3）。
// ════════════════════════════════════════════════════════════════════
static void testFenceTimeout() {
    printf("[test 2] Fence timeout... ");
    MockSink sink;
    resetStaticBufs();

    // fence 永不置为 42
    // ringWaitForFence 有 kFenceTimeout 轮上限，不会死循环
    int ret = ringWaitForFence(&s_rs, sink, 42);
    assert(ret == -1); // 必须返回超时

    // ★ D3 断言①：轮数 = kFenceTimeout，每轮恰好一次延时
    uint32_t delays = 0;
    for (auto& op : sink.ops) {
        if (op.kind == MockSink::Delay) {
            ++delays;
            assert(op.value == kFencePollUs);  // 每轮 80µs，不得为 0（忙等）
        }
    }
    assert(delays == kFenceTimeout);

    // ★ D3 断言②：总睡眠 = 20000 × 80µs = 1.6s，落在 Linux 的 1.2-2.0s 窗口内
    assert(sink.slept_us == static_cast<uint64_t>(kFenceTimeout) * kFencePollUs);
    assert(sink.slept_us >= 20000ull * 60ull);
    assert(sink.slept_us <= 20000ull * 100ull);

    printf("PASS (rounds=%u, slept=%lluus)\n",
           delays, static_cast<unsigned long long>(sink.slept_us));
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
// 测试 5：smnAddr 等价关系 —— 验证字节地址公式与单一事实源一致
//   依据 RegAddr.hpp：smnAddrWithBase(segBase, off) = (segBase + off) * 4
//   （等价 Linux RREG32_SOC15_EXT，soc15_common.h:201-204；SEG0=0x00016000 / SEG1=0x0243FC00）
//   A-2 追加：双段实现 —— 同一 off 在两段下字节地址差恒为 (SEG1-SEG0)*4。
// ════════════════════════════════════════════════════════════════════
static void testSmnAddrEquivalence() {
    printf("[test 5] smnAddr dual-segment equivalence... ");

    // 公式本身：smnAddrWithBase(segBase, off) == (segBase + off) * 4，对任意 off 成立
    assert(smnAddrWithBase(kMpSeg0Base, 0x51) == (0x00016000u + 0x51) * 4);
    assert(smnAddrWithBase(kMpSeg1Base, 0x51) == (0x0243FC00u + 0x51) * 4);
    assert(smnAddrWithBase(kMpSeg0Base, 0x00) == (0x00016000u + 0x00) * 4);
    assert(smnAddrWithBase(kMpSeg1Base, 0x00) == (0x0243FC00u + 0x00) * 4);
    assert(smnAddrWithBase(kMpSeg0Base, 0x29A) == (0x00016000u + 0x29A) * 4);
    assert(smnAddrWithBase(kMpSeg1Base, 0x29A) == (0x0243FC00u + 0x29A) * 4);

    // 双段差恒定：同一 off 下 byteAddr(SEG1) - byteAddr(SEG0) == (SEG1-SEG0)*4
    assert(smnAddrWithBase(kMpSeg1Base, 0x91) - smnAddrWithBase(kMpSeg0Base, 0x91)
           == (kMpSeg1Base - kMpSeg0Base) * 4);
    assert(smnAddrWithBase(kMpSeg1Base, 0x91) - smnAddrWithBase(kMpSeg0Base, 0x91)
           == 0x090A7000u);

    // 定标寄存器：kC2PMSG* 现为 **dword 偏移**（相对段基址，SEG1 为事实源）；
    //   字节地址 = smnAddrWithBase(段基址, 偏移)
    //   C2PMSG_35 (0x63) → SEG0 字节地址 0x0005824C / SEG1 0x090FF18C
    //   C2PMSG_81 (0x91) → SEG0 字节地址 0x00058244 / SEG1 0x090FF244
    assert(kC2PMSG35 == MP0_SMN_C2PMSG_35);   // 0x63（dword 偏移）
    assert(kC2PMSG35 == 0x63u);
    assert(kC2PMSG81 == MP0_SMN_C2PMSG_81);   // 0x91（dword 偏移）
    assert(smnAddrWithBase(kMpSeg0Base, kC2PMSG81) == 0x00058244u);
    assert(smnAddrWithBase(kMpSeg1Base, kC2PMSG81) == 0x090FF244u);
    assert(smnAddrWithBase(kMpSeg0Base, kC2PMSG35) == 0x0005818Cu);
    assert(smnAddrWithBase(kMpSeg1Base, kC2PMSG35) == 0x090FF18Cu);
    printf("PASS\n");
}

// ════════════════════════════════════════════════════════════════════
// 主函数
// ════════════════════════════════════════════════════════════════════
int main() {
    testNormalSubmit();
    testFenceTimeout();
    testCmdBufCopy();
    testWaitRegTimeout();
    testSmnAddrEquivalence();
    printf("\n所有 PSP13 环测试通过。\n");
    return 0;
}
