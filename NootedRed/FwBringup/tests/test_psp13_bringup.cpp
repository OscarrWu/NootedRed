// PSP 13.0.4 Bringup 主流程验收测试（用户态纯逻辑）
//
// 覆盖：
//   ① 正常路径（SOS alive → bootloader 跳过 → ring_create → load_toc → tmr_load）
//   ② bootloader 等待超时（C2PMSG_35 永不就绪）— 必须失败且不死循环
//   ③ ring_create 超时（C2PMSG_64 永不返回响应）
//   ④ LOAD_IP_FW fence 超时
//   ⑤ is_sos_alive 短路分支（7 条 bootloader 全部跳过）
//   ⑥ 失败码解码：响应状态带 TEE 错误码 ⇒ 原语返回解码值
//   ⑦ 寄存器序列导出（影子产物，供 T6 差分）
//
// 编译运行：
//   make -f src/NootedRed/FwBringup/tests/Makefile test
//
// 依据：docs/子任务/乙线自建固件层作战计划.md T5 验收判据

#include "FwBringup/Psp13Bringup.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace fw;
using namespace display;

// ── 静态缓冲 ──
static uint8_t  s_ring_buf[kRingSizeBytes];
static uint8_t  s_cmd_buf[kCmdBufSize];
static uint32_t s_fence_val;
static uint8_t  s_fw_pri[kFwCopyMax];
static uint8_t  s_tmr_buf[0x400000]; // 4MB TMR
// 合成 TOC（内容无关紧要，离线只验证寄存器流；真实解析在 PSP 侧）
static uint8_t  s_toc_data[2560];

static RingState s_rs;

static void resetBufs() {
    std::memset(s_ring_buf, 0, sizeof(s_ring_buf));
    std::memset(s_cmd_buf, 0, sizeof(s_cmd_buf));
    std::memset(s_fw_pri, 0, sizeof(s_fw_pri));
    std::memset(s_tmr_buf, 0, sizeof(s_tmr_buf));
    for (size_t i = 0; i < sizeof(s_toc_data); ++i) s_toc_data[i] = (uint8_t)(i * 7);
    s_fence_val = 0;
    ringInit(&s_rs, s_ring_buf, s_cmd_buf, &s_fence_val);
}

// ── 测试用 RegSink 替身 ──
// 模拟 PSP 行为：
//   - C2PMSG_67（写指针）按真实硬件语义维护：读回最近写入值
//   - 其余地址返回预置 stub
//   - write C2PMSG_67（doorbell）时：可选注入响应状态码到命令缓冲
//     （模拟 PSP 写回响应），若 auto_fence=true 则立即完成当前 fence
struct MockSink final : public RegSink {
    enum OpKind { Write, Read, Delay };
    struct RecordedOp {
        OpKind   kind;
        RegAddr  addr;
        RegValue value;
    };
    std::vector<RecordedOp> ops;
    struct ReadStub { RegAddr addr; RegValue value; };
    std::vector<ReadStub> stubs;

    bool       auto_fence    = false;
    uint32_t   inject_status = 0;  // 0 = 不注入；非 0 = 门铃时写入响应状态
    uint32_t   wptr_reg      = 0;  // PSP 侧写指针状态（C2PMSG_67 读回值）

    void reset() {
        ops.clear(); stubs.clear();
        auto_fence = false; inject_status = 0; wptr_reg = 0;
    }
    void addStub(RegAddr a, RegValue v) { stubs.push_back({a, v}); }

    RegValue read(RegAddr addr) override {
        RegValue val;
        if (addr == kC2PMSG67) {
            // PSP 写指针：返回最近写入的值（真实硬件行为）
            val = wptr_reg;
        } else {
            val = 0;
            for (auto& s : stubs) {
                if (s.addr == addr) { val = s.value; break; }
            }
        }
        ops.push_back({Read, addr, val});
        return val;
    }

    void write(RegAddr addr, RegValue val) override {
        ops.push_back({Write, addr, val});
        if (addr == kC2PMSG67) {
            wptr_reg = val;  // PSP 消费帧并推进写指针
            // 门铃：PSP 取走帧 → 写回响应状态 → 完成当前 fence
            if (inject_status != 0) {
                reinterpret_cast<uint32_t*>(s_cmd_buf)[864 / 4] = inject_status;
            }
            if (auto_fence) s_fence_val = s_rs.fence_value;
        }
    }

    void delayMicroseconds(uint32_t) override {
        // 轮询延迟压缩：不记录（测试需要快速超时）
    }
};

// ── BringupCtx 快速装配 ──
static void initCtx(BringupCtx* ctx, MockSink* sink) {
    ctx->sink       = sink;
    ctx->ring       = &s_rs;
    ctx->fw_pri_buf = s_fw_pri;
    ctx->tmr_buf    = s_tmr_buf;
    ctx->tmr_size   = 0x400000;
    ctx->toc_data   = s_toc_data;
    ctx->toc_size   = sizeof(s_toc_data);
    ctx->last_step  = BringupStep::None;
    ctx->last_error = 0;

    for (auto& c : ctx->bl_comps) {
        c.data   = nullptr;
        c.size   = 0;
        c.bl_cmd = 0;
        c.is_sos = false;
    }
}

static const uint32_t kBlCmds[7] = {
    PSP_BL__LOAD_KEY_DATABASE,
    PSP_BL__LOAD_TOS_SPL_TABLE,
    PSP_BL__LOAD_SYSDRV,
    PSP_BL__LOAD_SOCDRV,
    PSP_BL__LOAD_INTFDRV,
    PSP_BL__LOAD_DBGDRV,
    PSP_BL__LOAD_SOSDRV
};

static void setAllBlComponents(BringupCtx* ctx) {
    static uint8_t dummy_fw[64] = {0};
    for (int i = 0; i < 7; ++i) {
        ctx->bl_comps[i].data   = dummy_fw;
        ctx->bl_comps[i].size   = 64;
        ctx->bl_comps[i].bl_cmd = kBlCmds[i];
        ctx->bl_comps[i].is_sos = (i == 6);
    }
}

// ════════════════════════════════════════════════════════════════════
// 测试 1：正常路径 — SOS alive → BL 跳过 → ring_create → load_toc → tmr_load
// ════════════════════════════════════════════════════════════════════
static void testNormalPath() {
    printf("[test 1] Normal path (SOS alive, ring, toc, tmr)... ");
    MockSink sink;
    resetBufs();

    // isSosAlive: C2PMSG_81 != 0
    sink.addStub(kC2PMSG81, 0xCAFE);
    // ring_create: C2PMSG_64 带 RESP_FLAG
    sink.addStub(kC2PMSG64, kMboxTosRespFlag);
    // ringSubmitFrame 读 wptr: C2PMSG_67 → 0
    // 门铃自动完成 fence（模拟 PSP 消费帧）
    sink.auto_fence = true;

    BringupCtx ctx;
    initCtx(&ctx, &sink);
    setAllBlComponents(&ctx); // 全部有效，验证 isSosAlive 短路

    int ret = bringupRun(&ctx, false, false,
                         0x10000000ULL, 0x20000000ULL,
                         0x30000000ULL, 0x40000000ULL);
    assert(ret == 0);
    assert(ctx.last_step == BringupStep::Done);

    // is_sos_alive 短路：不得写 C2PMSG_35/36
    for (auto& op : sink.ops) {
        if (op.kind == MockSink::Write) {
            assert(op.addr != kC2PMSG35);
            assert(op.addr != kC2PMSG36);
        }
    }

    // 必须包含 load_toc（GFX_CMD_ID_LOAD_TOC 写入 cmd_buf）与 tmr_load：
    // 检查门铃（C2PMSG_67 写）出现 3 次 = LOAD_TOC(1) + 本应在 ringSubmitFrame 的
    // LOAD_TOC 帧 + SETUP_TMR 帧。实际：每次 ringSubmitFrame 写一次 C2PMSG_67，
    // loadToc 1 次 + tmrLoad 1 次 = 2 次。
    uint32_t doorbell_count = 0;
    for (auto& op : sink.ops) {
        if (op.kind == MockSink::Write && op.addr == kC2PMSG67) ++doorbell_count;
    }
    assert(doorbell_count == 2); // LOAD_TOC + SETUP_TMR

    // ★ 回归断言（2026-09-29，由独立复核发现的缺陷固化）：
    //   环帧的 `cmd_buf_addr` 必须是**命令缓冲**地址（本测试传 0x30000000），
    //   **不得**是固件缓冲地址（0x10000000）。
    //   依据：Linux `psp_cmd_submit_buf`（amdgpu_psp.c:738）对**所有**命令一律传
    //   `psp->cmd_buf_mc_addr`；固件地址只出现在 LOAD_TOC 命令的负载里。
    //   原先 `loadToc` 误传固件地址，真机上 PSP 会从 TOC 固件字节处解析命令结构 ⇒ 必失败；
    //   离线 MockSink 不校验帧内容，故当时未暴露。
    for (uint32_t f = 0; f < 2; ++f) {
        const auto* fr = reinterpret_cast<const RbFrame*>(s_ring_buf + f * kRbFrameSize);
        const uint64_t cmdAddr = (static_cast<uint64_t>(fr->cmd_buf_addr_hi) << 32) | fr->cmd_buf_addr_lo;
        const uint64_t fenAddr = (static_cast<uint64_t>(fr->fence_addr_hi) << 32) | fr->fence_addr_lo;
        assert(cmdAddr == 0x30000000ULL);   // 命令缓冲地址，不是 0x10000000（固件缓冲）
        assert(fenAddr == 0x20000000ULL);
    }

    // tmr_size 应由 load_toc 覆盖（inject_status=0 时 resp_tmr_size 为 0，
    // 但因为 MockSink 未注入 tmr_size，保持 0；这里只验证流程未中断）
    printf("PASS (doorbell=%u, ops=%zu)\n", doorbell_count, sink.ops.size());
}

// ════════════════════════════════════════════════════════════════════
// 测试 2：Bootloader 等待超时 — C2PMSG_35 永不就绪
// ════════════════════════════════════════════════════════════════════
static void testBlTimeout() {
    printf("[test 2] Bootloader timeout (C2PMSG_35 never ready)... ");
    MockSink sink;
    resetBufs();

    // C2PMSG_81 = 0（SOS 未存活）→ 走 bootloader 等待
    sink.addStub(kC2PMSG81, 0);
    // C2PMSG_35 永不置 bit31（无 stub = 返回 0）

    BringupCtx ctx;
    initCtx(&ctx, &sink);
    static uint8_t dummy_fw[64] = {0};
    ctx.bl_comps[0].data   = dummy_fw;
    ctx.bl_comps[0].size   = 64;
    ctx.bl_comps[0].bl_cmd = kBlCmds[0];

    int ret = bringupRun(&ctx, false, false,
                         0x10000000ULL, 0x20000000ULL,
                         0x30000000ULL, 0x40000000ULL);
    assert(ret == -1); // bootloader 必须失败
    assert(ctx.last_step == BringupStep::BlLoadKdb);
    assert(ctx.last_error == -1);

    printf("PASS (failed at step=%u)\n", (uint32_t)ctx.last_step);
}

// ════════════════════════════════════════════════════════════════════
// 测试 3：ring_create 超时 — C2PMSG_64 永不返回响应
// ════════════════════════════════════════════════════════════════════
static void testRingCreateTimeout() {
    printf("[test 3] Ring create timeout (C2PMSG_64 never ready)... ");
    MockSink sink;
    resetBufs();

    sink.addStub(kC2PMSG81, 0xCAFE); // SOS alive → 跳过 bootloader
    // C2PMSG_64 永不返回 RESP_FLAG（无 stub = 0）

    BringupCtx ctx;
    initCtx(&ctx, &sink);

    int ret = bringupRun(&ctx, false, false,
                         0x10000000ULL, 0x20000000ULL,
                         0x30000000ULL, 0x40000000ULL);
    assert(ret == -1);
    assert(ctx.last_step == BringupStep::RingCreate);

    printf("PASS (failed at step=%u)\n", (uint32_t)ctx.last_step);
}

// ════════════════════════════════════════════════════════════════════
// 测试 4：LOAD_IP_FW fence 超时 — fence 永不满足
// ════════════════════════════════════════════════════════════════════
static void testLoadIpFwTimeout() {
    printf("[test 4] LOAD_IP_FW fence timeout... ");
    MockSink sink;
    resetBufs();

    // auto_fence = false → 门铃不完成 fence → 超时
    // inject_status = 0

    int ret = executeLoadIpFw(sink, &s_rs,
                              0x50000000ULL,  // fw_mc_addr
                              0x1000,          // fw_size
                              GFX_FW_TYPE_IMU_I,
                              0x30000000ULL,  // cmd_buf_mc_addr
                              0x20000000ULL); // fence_mc_addr
    assert(ret == -1); // fence 超时

    printf("PASS\n");
}

// ════════════════════════════════════════════════════════════════════
// 测试 5：isSosAlive 短路 — 7 条 bootloader 全部跳过
// ════════════════════════════════════════════════════════════════════
static void testIsSosAliveShortCircuit() {
    printf("[test 5] isSosAlive short circuit (all 7 BL valid)... ");
    MockSink sink;
    resetBufs();

    sink.addStub(kC2PMSG81, 0x1);
    sink.addStub(kC2PMSG64, kMboxTosRespFlag);
    sink.auto_fence = true;

    BringupCtx ctx;
    initCtx(&ctx, &sink);
    setAllBlComponents(&ctx);

    int ret = bringupRun(&ctx, false, false,
                         0x10000000ULL, 0x20000000ULL,
                         0x30000000ULL, 0x40000000ULL);
    assert(ret == 0);
    // 没有任何 C2PMSG_35/36 写入
    for (auto& op : sink.ops) {
        if (op.kind == MockSink::Write) {
            assert(op.addr != kC2PMSG35);
            assert(op.addr != kC2PMSG36);
        }
    }

    printf("PASS (ops=%zu, all BL skipped)\n", sink.ops.size());
}

// ════════════════════════════════════════════════════════════════════
// 测试 6：失败码解码 — 响应带 TEE 错误码 ⇒ 原语返回解码值
// ════════════════════════════════════════════════════════════════════
static void testErrorCodeDecode() {
    printf("[test 6] Error code decode (resp status = 0xFFFF0002)... ");
    MockSink sink;
    resetBufs();

    // 门铃时注入 TEE_ERROR_CANCEL；同时完成 fence（模拟 PSP 消费帧并写回响应）
    sink.auto_fence    = true;
    sink.inject_status = kTeeErrorCancel;

    int ret = executeLoadIpFw(sink, &s_rs,
                              0x50000000ULL, 0x1000,
                              GFX_FW_TYPE_IMU_I,
                              0x30000000ULL, 0x20000000ULL);
    assert(ret == (int)kTeeErrorCancel); // 解码出的错误码

    // 成功路径对照：注入 kTeeSuccess
    resetBufs();
    MockSink sink2;
    sink2.auto_fence    = true;
    sink2.inject_status = kTeeSuccess;
    ret = executeLoadIpFw(sink2, &s_rs,
                          0x50000000ULL, 0x1000,
                          GFX_FW_TYPE_IMU_I,
                          0x30000000ULL, 0x20000000ULL);
    assert(ret == 0);

    printf("PASS\n");
}

// ════════════════════════════════════════════════════════════════════
// 测试 7：寄存器序列导出（影子产物）
// ════════════════════════════════════════════════════════════════════
static const char* kTracePath = "kb/sequences/psp13_bringup_trace.txt";

static void testSequenceExport() {
    printf("[test 7] Register sequence export to %s... ", kTracePath);
    MockSink sink;
    resetBufs();

    sink.addStub(kC2PMSG81, 0x1);
    sink.addStub(kC2PMSG64, kMboxTosRespFlag);
    sink.auto_fence = true;

    BringupCtx ctx;
    initCtx(&ctx, &sink);

    int ret = bringupRun(&ctx, false, false,
                         0x10000000ULL, 0x20000000ULL,
                         0x30000000ULL, 0x40000000ULL);
    assert(ret == 0);

    // 导出寄存器序列到文件
    FILE* f = fopen(kTracePath, "w");
    assert(f != nullptr);

    fprintf(f, "# PSP 13.0.4 Bringup register trace (offline shadow)\n");
    fprintf(f, "# Generated by test_psp13_bringup test 7\n");
    fprintf(f, "# Format: op addr value  seq=N\n");
    fprintf(f, "#   op: read|write; addr: SMN byte address = smnAddr(off) = (SEG1+off)*4\n");
    fprintf(f, "#          (SEG1=0x0243FC00, dcn314_smu.c:38-43; smn_base64=0; 详见 RegAddr.hpp)\n");
    fprintf(f, "# Compat: 与 kb/sequences/linux-dcn314-init.json 的 sequence 条目\n");
    fprintf(f, "#          {op, addr(dword), value, seq} 风格可对照（A3: dword 索引）\n");
    fprintf(f, "---\n");

    uint32_t seq = 0;
    for (auto& op : sink.ops) {
        ++seq;
        if (op.kind == MockSink::Read) {
            fprintf(f, "read  0x%08X 0x%08X  seq=%u\n", op.addr, op.value, seq);
        } else if (op.kind == MockSink::Write) {
            fprintf(f, "write 0x%08X 0x%08X  seq=%u\n", op.addr, op.value, seq);
        }
    }
    fclose(f);

    printf("PASS (%u ops written)\n", seq);
}

// ════════════════════════════════════════════════════════════════════
// 主函数
// ════════════════════════════════════════════════════════════════════
int main() {
    testNormalPath();
    testBlTimeout();
    testRingCreateTimeout();
    testLoadIpFwTimeout();
    testIsSosAliveShortCircuit();
    testErrorCodeDecode();
    testSequenceExport();
    printf("\n所有 PSP13 Bringup 测试通过。\n");
    return 0;
}
