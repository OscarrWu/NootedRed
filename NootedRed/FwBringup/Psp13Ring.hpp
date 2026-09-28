// PSP 13.0.4 GPCOM Ring —— 帧构造 / 发送 / 等待响应
//
// 本文件实现非 SRIOV 路径的 GPCOM 环操作（Linux psp_v13_0_4.c / psp_v13_0.c）。
// SRIOV 路径（C2PMSG_101/102/103）仅注释标注差异，未实现。
//
// 依据：
//   - psp_v13_0_4.c:223-291  ring_create（非 SRIOV 端口 L257-287）
//   - psp_v13_0_4.c:311-322  ring_get_wptr；L324-334 ring_set_wptr
//   - amdgpu_psp.c:3911-3961 psp_ring_cmd_submit（帧构造 + wptr 更新）
//   - amdgpu_psp.c:603-633   psp_wait_for（寄存器轮询）
//   - amdgpu_psp.c:720-802   psp_cmd_submit_buf（帧发送 + fence 等待）
//   - psp_gfx_if.h:466-491  psp_gfx_cmd_resp 帧格式（1024 字节）
//   - psp_gfx_if.h:496-512  psp_gfx_rb_frame 环帧格式（64 字节）
//
// 约束：
//   - header-only（内联），命名空间 fw
//   - 寄存器访问经 display::RegSink（read/write/delayMicroseconds）
//   - 无动态分配（固定大小缓冲区由调用方提供）
//   - 段基址取 SEG0（MP0_BASE_0 = 0x16000），与 Smu13Mailbox.hpp 一致
//
// 本实现按 SEG0；SEG1 为对照项，由真机判别实验定论。
// 禁止在代码/注释中断言 "SEG0 正确 / SEG1 必然不可达"。

#pragma once

#include "Regs/PSP13.hpp"
#include "DisplaySeq/RegSink.hpp"
#include "GPUDriversAMD/RavenIPOffset.hpp"

namespace fw {

// =============================================================================
// 绝对寄存器地址（dword 索引，供 RegSink 直接使用）
// PSP13.hpp 常量为 dword 偏移（mp_13_0_4_offset.h BASE_IDX=1），
// 物理地址 = MP0_BASE_0（SEG0）+ 偏移。与 Smu13Mailbox.hpp 做法一致。
// =============================================================================

inline constexpr display::RegAddr kC2PMSG35 = MP0_BASE_0 + MP0_SMN_C2PMSG_35;
inline constexpr display::RegAddr kC2PMSG36 = MP0_BASE_0 + MP0_SMN_C2PMSG_36;
inline constexpr display::RegAddr kC2PMSG64 = MP0_BASE_0 + MP0_SMN_C2PMSG_64;
inline constexpr display::RegAddr kC2PMSG67 = MP0_BASE_0 + MP0_SMN_C2PMSG_67;
inline constexpr display::RegAddr kC2PMSG69 = MP0_BASE_0 + MP0_SMN_C2PMSG_69;
inline constexpr display::RegAddr kC2PMSG70 = MP0_BASE_0 + MP0_SMN_C2PMSG_70;
inline constexpr display::RegAddr kC2PMSG71 = MP0_BASE_0 + MP0_SMN_C2PMSG_71;
inline constexpr display::RegAddr kC2PMSG81 = MP0_BASE_0 + MP0_SMN_C2PMSG_81;
// ⚠️ D4（T6 差分）：Linux 底本 mp_13_0_4_offset.h 对 MP0 C2PMSG 的 BASE_IDX=1 ⇒ SEG1
//    （yellow_carp_offset.h:827 MP0_BASE__INST0_SEG1 = 0x0243FC00）；本实现与现役代码按 SEG0
//    （MP0_BASE_0 = 0x16000，HWLibs.cpp:1965 §16.89 实测回滚背书）。属作战计划 §6 R6 待真机判别项，
//    不得断言哪侧正确。地址若错 = 静默写错寄存器（ROADMAP §3.3 硬件硬约束）。

// =============================================================================
// GPCOM 环帧常量（与 Linux psp_gfx_if.h 一致）
// =============================================================================

/// 单帧大小（struct psp_gfx_rb_frame），bytes
constexpr uint32_t kRbFrameSize   = 64;

/// 环缓冲总大小（4 KB，与 amdgpu_psp.c:78 一致）
constexpr uint32_t kRingSizeBytes = 0x1000;

// 命令缓冲总大小（1024 字节，与 psp_gfx_if.h:490 一致）
// ⚠️ D7（T6）：Linux 按页分配 PSP_CMD_BUFFER_SIZE = PSP_FENCE_BUFFER_SIZE = 0x1000
//    （amdgpu_psp.h:37-38）；本实现按 struct 本体 1024B / fence 4B 分配。协议只引用这些字节，
//    分配粒度差异无功能影响。
constexpr uint32_t kCmdBufSize    = 1024;

/// 环缓冲中帧的 dword 计数
constexpr uint32_t kRbFrameSizeDw = kRbFrameSize / 4;

/// 环缓冲 dword 计数
constexpr uint32_t kRingSizeDw    = kRingSizeBytes / 4;

/// 环缓冲最大帧数
constexpr uint32_t kMaxRBFrames   = kRingSizeBytes / kRbFrameSize;

// 寄存器轮询超时（迭代次数 × 1µs 间隔，对应 Linux psp_wait_for 语义 amdgpu_psp.c:603-633）
// ⚠️ D2（T6）：Linux 循环上限 = adev->usec_timeout（默认 100000 = 100ms，amdgpu.h:274 +
//    amdgpu_device.c:3763；该值可被模块参数/emu 模式改变，【待核实】真机余量）。
//    本实现取 1M×1µs = 1s，比 Linux 更宽松，属有意的安全余量（方向：不假超时）。
constexpr uint32_t kRegPollUs     = 1000000;

// Bootloader 等待重试次数（psp_v13_0_4.c:78-87，10 次）
constexpr uint32_t kBlRetryMax    = 10;

// 环命令响应 fence 轮询超时：次数对齐 Linux psp_timeout = 20000（amdgpu_psp.c:292/726-758）；
// 每轮延时 80µs（Linux usleep_range(60,100) 中值，见 ringWaitForFence；T6 D3 修正）
constexpr uint32_t kFenceTimeout  = 20000;

/// 固件拷贝最大大小（PSP_1_MEG = 0x100000, amdgpu_psp.h:39）
constexpr uint32_t kFwCopyMax     = 0x100000;

/// TEE 状态码（psp_gfx_if.h:516-520）
constexpr uint32_t kTeeSuccess        = 0x00000000;
constexpr uint32_t kTeeErrorCancel    = 0xFFFF0002;
constexpr uint32_t kTeeErrorNotSupp   = 0xFFFF000A;
constexpr uint32_t kPspErrUnknownCmd  = 0x00000100;

/// MBOX_TOS 响应掩码（amdgpu_psp.h:57-65）
constexpr uint32_t kMboxTosRespFlag = 0x80000000;
constexpr uint32_t kMboxTosRespMask = 0x8000FFFF;  // bit31 响应 + 低 16 位状态码

/// 启动加载器就绪标志（bit31 of C2PMSG_35）
constexpr uint32_t kBlReadyFlag = 0x80000000;

// =============================================================================
// GPCOM 环帧结构
// =============================================================================

/// 命令响应帧 —— 对应 struct psp_gfx_cmd_resp（1024 字节, psp_gfx_if.h:466-491）
struct GfxCmdResp {
    uint32_t buf_size;           // +0
    uint32_t buf_version;        // +4
    uint32_t cmd_id;             // +8
    uint32_t resp_buf_addr_lo;   // +12  (RBI only)
    uint32_t resp_buf_addr_hi;   // +16  (RBI only)
    uint32_t resp_offset;        // +20  (RBI only)
    uint32_t resp_buf_size;      // +24  (RBI only)

    // +28: union psp_gfx_commands
    uint32_t cmd_payload[4];     // +28..+43

    uint8_t  reserved_1[864 - 28 - 16]; // padding to +864

    // +864: struct psp_gfx_resp
    uint32_t resp_status;        // +864
    uint32_t resp_session_id;    // +868
    uint32_t resp_fw_addr_lo;    // +872
    uint32_t resp_fw_addr_hi;    // +876
    uint32_t resp_tmr_size;      // +880
    uint32_t resp_reserved[11];  // +884..+928
    uint32_t resp_uresp[8];      // +928..+960

    uint8_t  reserved_2[1024 - 864 - sizeof(uint32_t) * (1+1+1+1+1+11+8)];
};

static_assert(sizeof(GfxCmdResp) == kCmdBufSize,
              "GfxCmdResp must be exactly 1024 bytes");

/// 环缓冲帧 —— 对应 struct psp_gfx_rb_frame（64 字节, psp_gfx_if.h:497-512）
struct RbFrame {
    uint32_t cmd_buf_addr_lo;  // +0
    uint32_t cmd_buf_addr_hi;  // +4
    uint32_t cmd_buf_size;     // +8
    uint32_t fence_addr_lo;    // +12
    uint32_t fence_addr_hi;    // +16
    uint32_t fence_value;      // +20
    uint32_t sid_lo;           // +24 (RBI only)
    uint32_t sid_hi;           // +28 (RBI only)
    uint8_t  vmid;             // +32
    uint8_t  frame_type;       // +33
    uint8_t  reserved1[2];     // +34
    uint32_t reserved2[7];     // +36
};

static_assert(sizeof(RbFrame) == kRbFrameSize,
              "RbFrame must be exactly 64 bytes");

// =============================================================================
// 环运行时状态
// =============================================================================

struct RingState {
    uint8_t*  ring_buf;     // 环缓冲（kRingSizeBytes 字节，调用方分配）
    uint8_t*  cmd_buf;      // 命令缓冲（kCmdBufSize 字节，调用方分配）
    uint32_t* fence_buf;    // fence 缓冲（4 字节，调用方分配）
    uint32_t  wptr;         // 写指针（dword 单位）
    uint32_t  fence_value;  // 递增 fence 序列号
};

// =============================================================================
// 内联实现
// =============================================================================

inline void ringInit(RingState* rs,
                     uint8_t* ring_buf,
                     uint8_t* cmd_buf,
                     uint32_t* fence_buf) {
    rs->ring_buf    = ring_buf;
    rs->cmd_buf     = cmd_buf;
    rs->fence_buf   = fence_buf;
    rs->wptr        = 0;
    rs->fence_value = 0;
}

inline void cmdBufClear(RingState* rs) {
    uint32_t* p    = reinterpret_cast<uint32_t*>(rs->cmd_buf);
    uint32_t* end  = p + kCmdBufSize / sizeof(uint32_t);
    for (; p < end; ++p) *p = 0;
}

inline void cmdBufCopy(RingState* rs, const GfxCmdResp* src) {
    cmdBufClear(rs);
    const uint32_t* s   = reinterpret_cast<const uint32_t*>(src);
    uint32_t* d         = reinterpret_cast<uint32_t*>(rs->cmd_buf);
    uint32_t* end       = d + kCmdBufSize / sizeof(uint32_t);
    for (; d < end; ++d, ++s) *d = *s;
}

inline const GfxCmdResp* cmdBufGet(const RingState* rs) {
    return reinterpret_cast<const GfxCmdResp*>(rs->cmd_buf);
}

/// 通过 GPCOM 环提交一帧（非 SRIOV 路径）
///
/// 对应 Linux psp_ring_cmd_submit (amdgpu_psp.c:3911-3961) +
/// psp_cmd_submit_buf 前半部分 (amdgpu_psp.c:720-802)。
///
/// @return 0 成功，-1 失败（帧位置越界）
inline int ringSubmitFrame(RingState* rs,
                           display::RegSink& sink,
                           uint64_t cmd_buf_mc_addr,
                           uint64_t fence_mc_addr) {
    // 递增 fence value（等 atomic_inc_return, amdgpu_psp.c:737）
    rs->fence_value += 1;
    const uint32_t index = rs->fence_value;

    // 读 PSP 写指针 C2PMSG_67（等 psp_ring_get_wptr, psp_v13_0_4.c:311-322）
    rs->wptr = sink.read(kC2PMSG67);

    // 计算帧位置（amdgpu_psp.c:3932-3935）
    RbFrame* frame;
    if ((rs->wptr % kRingSizeDw) != 0) {
        const uint32_t frame_idx = rs->wptr / kRbFrameSizeDw;
        if (frame_idx >= kMaxRBFrames) return -1;
        // ⚠️ D8（T6）：wptr≥ring_size_dw 且非整倍数时返回 -1；Linux 以 % 回绕
        //    （amdgpu_psp.c:3932-3935）。写侧取模保证正常流程 wptr<1024，防御性差异。
        frame = reinterpret_cast<RbFrame*>(rs->ring_buf) + frame_idx;
    } else {
        frame = reinterpret_cast<RbFrame*>(rs->ring_buf);
    }

    // 清零帧并填充（amdgpu_psp.c:3947-3954）
    for (uint32_t i = 0; i < sizeof(RbFrame) / sizeof(uint32_t); ++i) {
        reinterpret_cast<uint32_t*>(frame)[i] = 0;
    }
    frame->cmd_buf_addr_lo = static_cast<uint32_t>(cmd_buf_mc_addr & 0xFFFFFFFF);
    frame->cmd_buf_addr_hi = static_cast<uint32_t>((cmd_buf_mc_addr >> 32) & 0xFFFFFFFF);
    frame->fence_addr_lo   = static_cast<uint32_t>(fence_mc_addr & 0xFFFFFFFF);
    frame->fence_addr_hi   = static_cast<uint32_t>((fence_mc_addr >> 32) & 0xFFFFFFFF);
    frame->fence_value     = index;

    // 更新写指针（doorbell, psp_v13_0_4.c:324-334）
    rs->wptr = (rs->wptr + kRbFrameSizeDw) % kRingSizeDw;
    sink.write(kC2PMSG67, rs->wptr);

    return 0;
}

/// 等待 fence 完成（轮询 fence 缓冲，等 Linux psp_cmd_submit_buf L745-758）
/// ⚠️ D3（T6 修正）：Linux 每轮 `usleep_range(60, 100)`（amdgpu_psp.c:756），
///    预算 20000×~80µs ≈ 1.2-2s 实时；原实现 20000 次忙等会在真机上微秒级耗尽 ⇒ 必然假超时。
///    现取中值 delayMicroseconds(80)/轮，次数保持 20000。
///    HDP invalidate 由调用方负责（Linux 每轮 invalidate_hdp，amdgpu_psp.c:755；
///    RegSink 无对应原语，见 T5 报告 §3.5）。
/// @return 0 成功，-1 超时
inline int ringWaitForFence(const RingState* rs,
                            display::RegSink& sink,
                            uint32_t index) {
    for (uint32_t t = kFenceTimeout; t > 0; --t) {
        if (*rs->fence_buf == index) return 0;
        sink.delayMicroseconds(80);
    }
    return -1;
}

// =============================================================================
// 寄存器级等待原语（等 Linux psp_wait_for, amdgpu_psp.c:603-633）
// =============================================================================

/// 寄存器轮询等待（语义等 Linux psp_wait_for, amdgpu_psp.c:603-633）。
/// ⚠️ D2（T6）：超时预算 kRegPollUs=1M×1µs 比 Linux（adev->usec_timeout，默认 100000）更宽松，
///    属有意的安全余量（见 kRegPollUs 注释）。
/// @param check_changed  true = PSP_WAITREG_CHANGED（值变化即返回）
///                       false = (val & mask) == reg_val
/// @return 0 成功，-1 超时
inline int waitReg(display::RegSink& sink,
                   display::RegAddr addr,
                   uint32_t reg_val,
                   uint32_t mask,
                   bool check_changed) {
    for (uint32_t i = 0; i < kRegPollUs; ++i) {
        const uint32_t val = sink.read(addr);
        if (check_changed) {
            if (val != reg_val) return 0;
        } else {
            if ((val & mask) == reg_val) return 0;
        }
        sink.delayMicroseconds(1);
    }
    return -1;
}

} // namespace fw
