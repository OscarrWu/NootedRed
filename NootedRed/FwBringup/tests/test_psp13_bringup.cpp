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

using namespace display;
using namespace fw;

// ── 静态缓冲 ──
static uint8_t  s_ring_buf[kRingSizeBytes];
static uint8_t  s_cmd_buf[kCmdBufSize];
static uint32_t s_fence_val;
static fw::RingState s_rs;
static uint8_t  s_fw_pri[kFwCopyMax];
static uint8_t  s_tmr_buf[0x400000]; // 4MB TMR
// 合成 TOC：P0 修正后 loadToc 会解析 common_firmware_header 并只送 payload 切片，
// 因此夹具必须是**有效头**（不再允许纯填充字节）。头用实测值：
//   psp_13_0_4_toc.bin（2560 B）：header v1.0、ucode_array_offset_bytes=256、
//   ucode_size_bytes=2304 ⇒ payload=[256,2560).
static uint8_t  s_toc_data[2560];
static void resetBufs() {
    std::memset(s_ring_buf, 0, sizeof(s_ring_buf));
    std::memset(s_cmd_buf, 0, sizeof(s_cmd_buf));
    std::memset(s_fw_pri, 0, sizeof(s_fw_pri));
    std::memset(s_tmr_buf, 0, sizeof(s_tmr_buf));
    std::memset(s_toc_data, 0, sizeof(s_toc_data));

    // 有效 common_firmware_header（32 B，等 amdgpu_ucode_psp.h:25-38）+ payload
    // 布局：size_bytes(0) hdr_size(4) ver_major(8) ver_minor(10) ip_major(12)
    //       ip_minor(14) ucode_version(16) ucode_size(20) ucode_off(24) crc(28)
    auto put32 = [](uint32_t off, uint32_t v) {
        s_toc_data[off + 0] = static_cast<uint8_t>(v);
        s_toc_data[off + 1] = static_cast<uint8_t>(v >> 8);
        s_toc_data[off + 2] = static_cast<uint8_t>(v >> 16);
        s_toc_data[off + 3] = static_cast<uint8_t>(v >> 24);
    };
    auto put16 = [](uint32_t off, uint16_t v) {
        s_toc_data[off + 0] = static_cast<uint8_t>(v);
        s_toc_data[off + 1] = static_cast<uint8_t>(v >> 8);
    };
    put32(0,  sizeof(s_toc_data));   // size_bytes = 2560
    put32(4,  32);                   // header_size_bytes（本测试只用 common 头）
    put16(8,  1);                    // header_version_major = 1
    put16(10, 0);                    // header_version_minor
    put16(12, 13);                   // ip_version_major
    put16(14, 0);                    // ip_version_minor
    put32(16, 0xB);                  // ucode_version
    put32(20, 2304);                 // ucode_size_bytes（实测值）
    put32(24, 256);                  // ucode_array_offset_bytes（实测值）
    put32(28, 0x12B73C3F);           // crc32（实测值）

    // payload 区 [256,2560) 填 (0x80 | i&0x7F)：
    //   ① 与头字节（size_bytes 低位为 0x00）区分开 ⇒ 若 loadToc 退回"送整文件"
    //      语义，测试 8 的 fw_pri 逐字节断言立刻失败（非"改软"）；
    //   ② payload 内部字节互不相同 ⇒ 能识别出偏移错位。
    for (size_t i = 256; i < sizeof(s_toc_data); ++i) {
        s_toc_data[i] = static_cast<uint8_t>(0x80u | (i & 0x7Fu));
    }
    s_fence_val = 0;
    fw::ringInit(&s_rs, s_ring_buf, s_cmd_buf, &s_fence_val);
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
        if (addr == fw::kC2PMSG67) {
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
        if (addr == fw::kC2PMSG67) {
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
static void initCtx(fw::BringupCtx* ctx, MockSink* sink) {
    ctx->sink       = sink;
    ctx->ring       = &s_rs;
    ctx->fw_pri_buf = s_fw_pri;
    ctx->tmr_buf    = s_tmr_buf;
    ctx->tmr_size   = 0x400000;
    ctx->toc_data   = s_toc_data;
    ctx->toc_size   = sizeof(s_toc_data);
    ctx->last_step  = fw::BringupStep::None;
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

static void setAllBlComponents(fw::BringupCtx* ctx) {
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
    sink.addStub(fw::kC2PMSG81, 0xCAFE);
    // ring_create: C2PMSG_64 带 RESP_FLAG
    sink.addStub(fw::kC2PMSG64, kMboxTosRespFlag);
    // ringSubmitFrame 读 wptr: C2PMSG_67 → 0
    // 门铃自动完成 fence（模拟 PSP 消费帧）
    sink.auto_fence = true;

    fw::BringupCtx ctx;
    initCtx(&ctx, &sink);
    setAllBlComponents(&ctx); // 全部有效，验证 isSosAlive 短路

    int ret = fw::bringupRun(&ctx, false, false,
                         0x10000000ULL, 0x20000000ULL,
                         0x30000000ULL, 0x40000000ULL);
    assert(ret == 0);
    assert(ctx.last_step == fw::BringupStep::Done);

    // is_sos_alive 短路：不得写 C2PMSG_35/36
    for (auto& op : sink.ops) {
        if (op.kind == MockSink::Write) {
            assert(op.addr != fw::kC2PMSG35);
            assert(op.addr != fw::kC2PMSG36);
        }
    }

    // 必须包含 load_toc（GFX_CMD_ID_LOAD_TOC 写入 cmd_buf）与 tmr_load：
    // 检查门铃（C2PMSG_67 写）出现 3 次 = LOAD_TOC(1) + 本应在 ringSubmitFrame 的
    // LOAD_TOC 帧 + SETUP_TMR 帧。实际：每次 ringSubmitFrame 写一次 C2PMSG_67，
    // loadToc 1 次 + tmrLoad 1 次 = 2 次。
    uint32_t doorbell_count = 0;
    for (auto& op : sink.ops) {
        if (op.kind == MockSink::Write && op.addr == fw::kC2PMSG67) ++doorbell_count;
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
        const auto* fr = reinterpret_cast<const fw::RbFrame*>(s_ring_buf + f * kRbFrameSize);
        const uint64_t cmdAddr = (static_cast<uint64_t>(fr->cmd_buf_addr_hi) << 32) | fr->cmd_buf_addr_lo;
        const uint64_t fenAddr = (static_cast<uint64_t>(fr->fence_addr_hi) << 32) | fr->fence_addr_lo;
        assert(cmdAddr == 0x30000000ULL);   // 命令缓冲地址，不是 0x10000000（固件缓冲）
        assert(fenAddr == 0x20000000ULL);
    }

    // ★ P0 回归断言（2026-09-30，loadToc payload 切片修正固化）：
    //   fw_pri_buf 只含 payload 切片（等 Linux psp_init_toc_microcode:
    //   toc.start_addr = data + ucode_array_offset_bytes；amdgpu_psp.c:4006-4007）
    //   ⇒ fw_pri[0..2304) 必须等于 s_toc_data[256..2560)，且 fw_pri[0] **不是**
    //   文件头字节（Linux 的 fw_pri 偏移 0 是 payload 起始，不是 header 起始）。
    //   帧侧断言（cmd_id / toc_size=2304）在新增的测试 8 中直接调 loadToc 验证。
    for (uint32_t i = 0; i < 2304; ++i) {
        assert(s_fw_pri[i] == s_toc_data[256 + i]);
    }
    // 头字节不得泄漏进 fw_pri（布局差异：Linux fw_pri 偏移 0 是 payload 起始，不是 header 起始）。
    // 注：s_toc_data[0]==0 且 s_toc_data[256]==0 时此断言会误报，已由上方循环完整覆盖。

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
    sink.addStub(fw::kC2PMSG81, 0);
    // C2PMSG_35 永不置 bit31（无 stub = 返回 0）

    fw::BringupCtx ctx;
    initCtx(&ctx, &sink);
    static uint8_t dummy_fw[64] = {0};
    ctx.bl_comps[0].data   = dummy_fw;
    ctx.bl_comps[0].size   = 64;
    ctx.bl_comps[0].bl_cmd = kBlCmds[0];

    int ret = fw::bringupRun(&ctx, false, false,
                         0x10000000ULL, 0x20000000ULL,
                         0x30000000ULL, 0x40000000ULL);
    assert(ret == -1); // bootloader 必须失败
    assert(ctx.last_step == fw::BringupStep::BlLoadKdb);
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

    sink.addStub(fw::kC2PMSG81, 0xCAFE); // SOS alive → 跳过 bootloader
    // C2PMSG_64 永不返回 RESP_FLAG（无 stub = 0）

    fw::BringupCtx ctx;
    initCtx(&ctx, &sink);

    int ret = fw::bringupRun(&ctx, false, false,
                         0x10000000ULL, 0x20000000ULL,
                         0x30000000ULL, 0x40000000ULL);
    assert(ret == -1);
    assert(ctx.last_step == fw::BringupStep::RingCreate);

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

    int ret = fw::executeLoadIpFw(sink, &s_rs,
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

    sink.addStub(fw::kC2PMSG81, 0x1);
    sink.addStub(fw::kC2PMSG64, kMboxTosRespFlag);
    sink.auto_fence = true;

    fw::BringupCtx ctx;
    initCtx(&ctx, &sink);
    setAllBlComponents(&ctx);

    int ret = fw::bringupRun(&ctx, false, false,
                         0x10000000ULL, 0x20000000ULL,
                         0x30000000ULL, 0x40000000ULL);
    assert(ret == 0);
    // 没有任何 C2PMSG_35/36 写入
    for (auto& op : sink.ops) {
        if (op.kind == MockSink::Write) {
            assert(op.addr != fw::kC2PMSG35);
            assert(op.addr != fw::kC2PMSG36);
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

    int ret = fw::executeLoadIpFw(sink, &s_rs,
                              0x50000000ULL, 0x1000,
                              GFX_FW_TYPE_IMU_I,
                              0x30000000ULL, 0x20000000ULL);
    assert(ret == (int)kTeeErrorCancel); // 解码出的错误码

    // 成功路径对照：注入 kTeeSuccess
    resetBufs();
    MockSink sink2;
    sink2.auto_fence    = true;
    sink2.inject_status = kTeeSuccess;
    ret = fw::executeLoadIpFw(sink2, &s_rs,
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

    sink.addStub(fw::kC2PMSG81, 0x1);
    sink.addStub(fw::kC2PMSG64, kMboxTosRespFlag);
    sink.auto_fence = true;

    fw::BringupCtx ctx;
    initCtx(&ctx, &sink);

    int ret = fw::bringupRun(&ctx, false, false,
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
// 测试 8：loadToc 送 payload 切片（P0-1）
//   依据：psp_init_toc_microcode（amdgpu_psp.c:3986-4008）→
//          psp_load_toc（amdgpu_psp.c:855-878）
//   实测：psp_13_0_4_toc.bin 头 v1.0 / ucode_off=256 / ucode_size=2304
// ════════════════════════════════════════════════════════════════════
static void testLoadTocPayloadSlice() {
    printf("[test 8] loadToc sends payload slice (2304 B, not 2560)... ");
    MockSink sink;
    resetBufs();
    sink.addStub(fw::kC2PMSG67, 0);
    sink.auto_fence = true;

    // 切片工具本身：payload = [256, 2560)
    const uint8_t* payload = nullptr;
    uint32_t       payload_size = 0;
    assert(fw::tocPayloadSlice(s_toc_data, sizeof(s_toc_data), &payload, &payload_size) == 0);
    assert(payload == s_toc_data + 256);
    assert(payload_size == 2304);

    // 头无效（缓冲太小）⇒ 拒绝
    const uint8_t* p2 = nullptr; uint32_t s2 = 0;
    assert(fw::tocPayloadSlice(s_toc_data, 16, &p2, &s2) != 0);
    // 越界（ucode_off + ucode_size > size）⇒ 拒绝
    assert(fw::tocPayloadSlice(s_toc_data, 512, &p2, &s2) != 0);

    uint32_t tmr_size = 0;
    int ret = fw::loadToc(sink, &s_rs, s_fw_pri,
                      s_toc_data, sizeof(s_toc_data),
                      0x10000000ULL, 0x30000000ULL, 0x20000000ULL, &tmr_size);
    assert(ret == 0);

    // 帧侧：cmd_id=LOAD_TOC、toc_size = payload 长 = 2304（**不是** 2560）
    const fw::GfxCmdResp* cmd = fw::cmdBufGet(&s_rs);
    assert(cmd->cmd_id == GFX_CMD_ID_LOAD_TOC);
    assert(cmd->cmd_payload[0] == 0x10000000u);  // fw_pri 地址低
    assert(cmd->cmd_payload[1] == 0x00000000u);  // fw_pri 地址高
    assert(cmd->cmd_payload[2] == 2304);

    // fw_pri 偏移 0 处必须是**payload 首字节**，而不是文件头字节。
    // 夹具刻意让二者取值不同：头首字节 0x00 vs payload 首字节 0x80
    // ⇒ 若 loadToc 退回"送整文件（含 256 B 头）"语义，下面断言立即失败。
    assert(s_toc_data[0] == 0x00);   // 头首字节（size_bytes 低 8 位）
    assert(s_toc_data[256] == 0x80); // payload 首字节（0x80 | (256 & 0x7F)）
    assert(s_fw_pri[0] == 0x80);     // ⇒ fw_pri 偏移 0 = payload 起始
    for (uint32_t i = 0; i < 2304; ++i) {
        assert(s_fw_pri[i] == s_toc_data[256 + i]);
    }
    printf("PASS (payload=2304@256, frame toc_size=2304, fw_pri[0]=0x80 not header)\n");
}

//   真值：src/NootedRed/Firmware/psp_13_0_4_ta.bin 前 84 字节（common 头 32
//         + bin_count 4 + 3×desc 16 = 36+48=84），实测
//         desc[0] fw_type=1(ASD) offset=0 size=217344
// ════════════════════════════════════════════════════════════════════
static const uint8_t kTaHeaderReal[84] = {
    0x00, 0xE4, 0x03, 0x00, 0x24, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x0D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE3, 0x03, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x4A, 0x90, 0x31, 0x7F, 0x03, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x15, 0x01, 0x00, 0x21, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x51, 0x03, 0x00, 0x04, 0x00, 0x00, 0x00, 0x52, 0x00, 0x00, 0x17,
    0x00, 0x51, 0x03, 0x00, 0x00, 0x71, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
    0x1E, 0x00, 0x00, 0x12, 0x00, 0xC2, 0x03, 0x00, 0x00, 0x21, 0x00, 0x00
};

static const uint8_t kTaHeaderTest[100] = {
    // CommonFwHeader (32 bytes)
    0x64, 0x00, 0x00, 0x00,  // size_bytes = 100
    0x34, 0x00, 0x00, 0x00,  // header_size_bytes = 52
    0x02, 0x00,              // header_version_major = 2
    0x00, 0x00,              // header_version_minor = 0
    0x0D, 0x00,              // ip_version_major = 13
    0x00, 0x00,              // ip_version_minor = 0
    0x00, 0x00, 0x00, 0x00,  // ucode_version = 0
    0x30, 0x00, 0x00, 0x00,  // ucode_size_bytes = 48 (dummy)
    0x34, 0x00, 0x00, 0x00,  // ucode_array_offset_bytes = 52
    0x00, 0x00, 0x00, 0x00,  // crc32 = 0
    // bin_count = 1
    0x01, 0x00, 0x00, 0x00,
    // desc[0]: fw_type=1(ASD), version=0x12345678, offset=0, size=48
    0x01, 0x00, 0x00, 0x00,
    0x78, 0x56, 0x34, 0x12,
    0x00, 0x00, 0x00, 0x00,
    0x30, 0x00, 0x00, 0x00,
    // payload at offset 52 (48 bytes of 0xAA)
    0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
    0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
    0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
    0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
    0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
    0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
};

static void testTaAsdParse() {
    printf("[test 9] TA v2.0 parse → ASD desc (real header fields + synthetic slice)... ");

    // ★ 真值回归：本机 psp_13_0_4_ta.bin 前 84 字节（kTaHeaderReal）实测字段
    //   仅断言「头字段 + desc[0]」，切片偏移 256 超出 84 字节合成缓冲⇒不调 parseTaAsd
    fw::CommonFwHeader realHdr;
    assert(fw::parseCommonFwHeader(kTaHeaderReal, sizeof(kTaHeaderReal), &realHdr));
    assert(realHdr.size_bytes == 254976);
    assert(realHdr.header_size_bytes == 36);
    assert(realHdr.header_version_major == 2);   // TA v2.0
    assert(realHdr.ip_version_major == 13);      // psp_v13
    assert(realHdr.ucode_array_offset_bytes == 256);
    assert(realHdr.ucode_size_bytes == 254720);

    fw::PspFwBinDesc realD0;
    assert(fw::pspFwBinDescAt(kTaHeaderReal, sizeof(kTaHeaderReal), 0, &realD0));
    assert(realD0.fw_type == fw::kTaFwTypePspAsd);   // desc[0] = ASD
    assert(realD0.fw_version == 0x21000115u);
    assert(realD0.offset_bytes == 0);
    assert(realD0.size_bytes == 217344);
    // desc[1]=HDCP(4)/desc[2]=DTM(5) 亦应可解析（只验证类型，乙线暂不装载）
    fw::PspFwBinDesc realD1, realD2;
    assert(fw::pspFwBinDescAt(kTaHeaderReal, sizeof(kTaHeaderReal), 1, &realD1));
    assert(fw::pspFwBinDescAt(kTaHeaderReal, sizeof(kTaHeaderReal), 2, &realD2));
    assert(realD1.fw_type == 4 && realD1.size_bytes == 28928);
    assert(realD2.fw_type == 5 && realD2.size_bytes == 8448);
    // index 越界（count=3）⇒ 拒绝
    assert(!fw::pspFwBinDescAt(kTaHeaderReal, sizeof(kTaHeaderReal), 3, &realD2));

    // 切片语义（ASD 偏移 = ucode_off + desc.offset）在合成头上验证，见下
    fw::CommonFwHeader hdr;
    assert(fw::parseCommonFwHeader(kTaHeaderTest, sizeof(kTaHeaderTest), &hdr));
    assert(hdr.size_bytes == 100);
    assert(hdr.header_size_bytes == 52);
    assert(hdr.header_version_major == 2);
    assert(hdr.ip_version_major == 13);
    assert(hdr.ucode_array_offset_bytes == 52);
    assert(hdr.ucode_size_bytes == 48);

    // desc[0] 直接取
    fw::PspFwBinDesc d0;
    assert(fw::pspFwBinDescAt(kTaHeaderTest, sizeof(kTaHeaderTest), 0, &d0));
    assert(d0.fw_type == fw::kTaFwTypePspAsd);
    assert(d0.fw_version == 0x12345678u);
    assert(d0.offset_bytes == 0);
    assert(d0.size_bytes == 48);

    // parseTaAsd：偏移 = ucode_array_offset_bytes + desc.offset_bytes = 52
    fw::TaBinDesc asd;
    assert(fw::parseTaAsd(kTaHeaderTest, sizeof(kTaHeaderTest), &asd) == 0);
    assert(asd.found);
    assert(asd.offset_bytes == 52);
    assert(asd.size_bytes == 48);

    // 头版本不是 2 ⇒ 拒绝（等 parse_ta_v2_microcode:4423-4424）
    uint8_t bad[64];
    std::memset(bad, 0, sizeof(bad));
    bad[8] = 1; // header_version_major = 1
    assert(fw::parseTaAsd(bad, sizeof(bad), &asd) != 0);

    // 越界：desc[0].offset_bytes 巨大（使 ucode_off + offset 回绕/超界）⇒ 拒绝
    // 覆盖 P0-3 的 uint32 回绕防御（hdr.ucode_off 先判 ≤ size，再判 offset ≤ size-off）
    uint8_t ovf[100];
    std::memcpy(ovf, kTaHeaderTest, sizeof(kTaHeaderTest));
    // desc[0].offset_bytes 位于 36+8 = 44..47
    ovf[44] = 0xFF; ovf[45] = 0xFF; ovf[46] = 0xFF; ovf[47] = 0x7F; // 0x7FFFFFFF
    assert(fw::parseTaAsd(ovf, sizeof(ovf), &asd) != 0);
    // ucode_array_offset_bytes 自身超界（位于 24..27）⇒ 拒绝
    uint8_t ovf2[100];
    std::memcpy(ovf2, kTaHeaderTest, sizeof(kTaHeaderTest));
    ovf2[24] = 0xFF; ovf2[25] = 0xFF; ovf2[26] = 0xFF; ovf2[27] = 0x7F; // 0x7FFFFFFF
    assert(fw::parseTaAsd(ovf2, sizeof(ovf2), &asd) != 0);

    // 无 ASD 描述符 ⇒ found=false 且返回 0（上游只是不填 bin_desc）
    uint8_t noasd[100];
    std::memcpy(noasd, kTaHeaderTest, sizeof(kTaHeaderTest));
    noasd[36] = 9; // desc[0].fw_type 改为非 ASD（DTM 之外随便一个未处理值）
    fw::TaBinDesc asd2;
    assert(fw::parseTaAsd(noasd, sizeof(noasd), &asd2) == 0);
    assert(!asd2.found);
    // 门控：Phoenix（有显示硬件、13.0.4、非 SRIOV、含 ASD）⇒ 需要装载
    assert(fw::asdLoadRequired(false, true, 13, 0, true));
    // SRIOV ⇒ 跳过（amdgpu_psp.c:1683）
    assert(!fw::asdLoadRequired(true, true, 13, 0, true));
    // 无 ASD ⇒ 跳过（amdgpu_psp.c:1683）
    assert(!fw::asdLoadRequired(false, true, 13, 0, false));
    // 无显示硬件 + MP0 ≥ 13.0.10 ⇒ 跳过（amdgpu_psp.c:1686-1689）
    assert(!fw::asdLoadRequired(false, false, 13, 10, true));
    // 无显示硬件但 13.0.4 < 13.0.10 ⇒ 仍需装载
    assert(fw::asdLoadRequired(false, false, 13, 0, true));

    printf("PASS (ASD@52 size=48, gate=Phoenix requires load)\n");
}

// ════════════════════════════════════════════════════════════════════
// 测试 10：LOAD_ASD 帧构造（P0-3 的帧部分）
//   依据：psp_prep_ta_load_cmd_buf（amdgpu_psp.c:1771-1785）+
//         psp_asd_initialize 的 ASD 参数（amdgpu_psp.c:1691-1693）
// ════════════════════════════════════════════════════════════════════
static void testLoadAsdFrame() {
    printf("[test 10] LOAD_ASD frame build (cmd-only, shared buf=0)... ");
    MockSink sink;
    resetBufs();
    sink.addStub(fw::kC2PMSG67, 0);
    sink.auto_fence = true;

    // 使用测试用合成头（kTaHeaderTest），其 ASD 切片在缓冲内
    fw::TaBinDesc asd;
    assert(fw::parseTaAsd(kTaHeaderTest, sizeof(kTaHeaderTest), &asd) == 0);
    assert(asd.found);

    int ret = fw::executeLoadAsd(sink, &s_rs, asd,
                             0x10000000ULL, 0x30000000ULL, 0x20000000ULL);
    assert(ret == 0);

    const fw::GfxCmdResp* cmd = fw::cmdBufGet(&s_rs);
    assert(cmd->cmd_id == GFX_CMD_ID_LOAD_ASD);
    assert(cmd->cmd_payload[0] == 0x10000000u);  // app_phy_addr_lo
    assert(cmd->cmd_payload[1] == 0x00000000u);  // app_phy_addr_hi
    assert(cmd->cmd_payload[2] == 48);           // app_len (合成头 size=48)
    // PSP_ASD_SHARED_MEM_SIZE = 0（amdgpu_psp.h:70）⇒ cmd_buf_* 全 0
    assert(cmd->cmd_payload[3] == 0);
    assert(cmd->cmd_payload[4] == 0);
    assert(cmd->cmd_payload[5] == 0);

    // ASD 未解析到 ⇒ 原语拒绝（无子固件可送）
    fw::TaBinDesc asd_empty; asd_empty.found = false; asd_empty.size_bytes = 0; asd_empty.offset_bytes = 0;
    assert(fw::executeLoadAsd(sink, &s_rs, asd_empty,
                          0x10000000ULL, 0x30000000ULL, 0x20000000ULL) == -1);

    printf("PASS (cmd_id=0x4, app_len=48, shared=0)\n");
}

// ════════════════════════════════════════════════════════════════════
// 测试 11：RLC autoload_start 帧 + BOOTLOAD 等待（P0-4）
//   依据：psp_rlc_autoload_start（amdgpu_psp.c:3896-3909，cmd-id-only）
//         触发点 psp_load_non_psp_fw（amdgpu_psp.c:3575-3583）
//         13.0.4 只设 boot_time_tmr=false、autoload_supported 保持 true
//         （amdgpu_psp.c:264-267）
// ════════════════════════════════════════════════════════════════════
static void testRlcAutoload() {
    printf("[test 11] RLC autoload_start frame + bootload wait... ");
    MockSink sink;
    resetBufs();
    sink.addStub(fw::kC2PMSG67, 0);
    sink.auto_fence = true;

    int ret = fw::rlcAutoloadStart(sink, &s_rs, 0x30000000ULL, 0x20000000ULL);
    assert(ret == 0);

    // cmd-id-only：cmd_id=0x21，其余全 0（mac-amdgpu psp_v14_0.cpp:1370-1394 同构）
    const fw::GfxCmdResp* cmd = fw::cmdBufGet(&s_rs);
    assert(cmd->cmd_id == GFX_CMD_ID_AUTOLOAD_RLC);
    assert(cmd->buf_size == 0);
    assert(cmd->buf_version == 0);
    for (uint32_t i = 0; i < 16; ++i) assert(cmd->cmd_payload[i] == 0);

    // 等待：CP_STAT=0 且 BOOTLOAD_COMPLETE(bit31)=1 ⇒ 完成
    const display::RegAddr kCpStat    = 0x01000000u; // 调用方按 GC_BASE 传入
    const display::RegAddr kBootload  = 0x02000000u;
    sink.addStub(kCpStat, 0);
    sink.addStub(kBootload, 0x80000000u);
    assert(fw::waitRlcAutoloadComplete(sink, kCpStat, kBootload) == 0);

    // CP_STAT != 0 ⇒ 永不判完成 ⇒ 超时返回 -1
    // （1M×1µs 预算，MockSink delay 为空操作 ⇒ 离线快速跑完）
    MockSink sink2;
    resetBufs();
    sink2.addStub(kCpStat, 1);
    sink2.addStub(kBootload, 0x80000000u);
    assert(fw::waitRlcAutoloadComplete(sink2, kCpStat, kBootload) == -1);

    printf("PASS (cmd_id=0x21, payload all-zero, wait ok, timeout -1)\n");
}

int main() {
    testNormalPath();
    testBlTimeout();
    testRingCreateTimeout();
    testLoadIpFwTimeout();
    testIsSosAliveShortCircuit();
    testErrorCodeDecode();
    testSequenceExport();
    testLoadTocPayloadSlice();
    testTaAsdParse();
    testLoadAsdFrame();
    testRlcAutoload();
    printf("\n所有 PSP13 Bringup 测试通过。\n");
    return 0;
}
