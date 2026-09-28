// SMU 13.0.4 邮箱握手原语（header-only，内联函数）
//
// 实现依据：
//   - Linux `drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0.c` 与 `smu_cmn.c`（V1 协议）
//   - 现役实现 `src/NootedRed/HWLibs.cpp:1939-2001`（`smu13SendMsgDirect`，SEG0 路径）
//   - 消息 ID 复用 `src/NootedRed/GPUDriversAMD/PhoenixPPSMC.hpp`
//
// ⚠️ 段基址说明（项目内两条记载冲突，尚未澄清）：
//   - 现役代码（`HWLibs.cpp:1966-1968`）与本实现按 **SEG0**（`MP0_BASE_0 + 0x282/0x292/0x29A`）实现。
//   - `mp_13_0_4_offset.h` 给 MP1 寄存器 `BASE_IDX = 1`，对应 `yellow_carp_offset.h:875-876` 的
//     `MP1_BASE__INST0_SEG1 = 0x0243FC00`（即 SEG1 路径）。
//   - `docs/ROADMAP.md:1619-1622` 记载：VBIOSSMC（显示时钟）走 SEG0，PMFW/PPSMC（电源管理）走 SEG1。
//   - `oldfiles-handoff/docs/ROADMAP.md:271-330` 实测 SEG0 有效、SEG1 因 BAR5 窗口限制判不可达。
//   ⇒ **本实现按 SEG0 实现（与现役代码一致）；SEG1 列为对照项，由真机判别实验定论。**
//   ❌ 禁止在代码或注释里断言"SEG0 正确"或"SEG1 必然不可达"。
//
// 约束：
//   - header-only（内联函数），无 .cpp，无动态分配，无异常
//   - 寄存器访问复用 `display::RegSink`（`display::RegAddr`/`RegValue` 均为 uint32_t）
//   - 命名空间 `fw`；寄存器常量风格对齐 `Regs/SMU.hpp`（裸 constexpr，dword 偏移）
//   - 段基址取自 `GPUDriversAMD/RavenIPOffset.hpp`（`MP0_BASE_0 = 0x16000`），不写死
//
// Linux 时序对照（逐行见注释）：
//   1. 清响应寄存器（C2PMSG_90 = 0）           ← `__smu_msg_v1_send` L319
//   2. 写参数寄存器（C2PMSG_82 = param）       ← `__smu_msg_v1_send` L320-324
//   3. 写命令寄存器（C2PMSG_66 = msgId）       ← `__smu_msg_v1_send` L325
//   4. 轮询响应寄存器直到非 0                  ← `__smu_msg_v1_poll_stat` L301-306
//   5. 解码响应码                              ← `smu_msg_v1_decode_response` L249-292
//
// 超时参数对齐 Linux：`smu_v13_0_4_ppt.c:1139` `ctl->default_timeout = adev->usec_timeout * 20`；
// `amdgpu.h:274` `AMDGPU_MAX_USEC_TIMEOUT = 100000` ⇒ 2,000,000；`smu_cmn.c:295-308` 每轮 `udelay(1)`
// ⇒ 默认预算 2 s。现役 `HWLibs.cpp:1976` `kRespTimeout = 200000 × 10µs` 同样 ≈ 2 s。

#pragma once

#include "../DisplaySeq/RegSink.hpp"
#include "../GPUDriversAMD/RavenIPOffset.hpp"
#include "../GPUDriversAMD/PhoenixPPSMC.hpp"
namespace fw {

// 寄存器偏移（dword 偏移，风格对齐 Regs/SMU.hpp）
// SEG0 路径：MP0_BASE_0 + 偏移
constexpr UInt32 kSmu13RegRespOffset = 0x29A;  // C2PMSG_90：响应（读），写 0 清零
constexpr UInt32 kSmu13RegArgOffset  = 0x292;  // C2PMSG_82：参数（写）
constexpr UInt32 kSmu13RegMsgOffset  = 0x282;  // C2PMSG_66：命令（写即触发）

// 绝对寄存器地址（dword 索引，供 RegSink 直接使用）
inline constexpr display::RegAddr kSmu13RegResp = MP0_BASE_0 + kSmu13RegRespOffset;
inline constexpr display::RegAddr kSmu13RegArg  = MP0_BASE_0 + kSmu13RegArgOffset;
inline constexpr display::RegAddr kSmu13RegMsg  = MP0_BASE_0 + kSmu13RegMsgOffset;

// SMU 响应码（与 Linux `smu_cmn.c:76-82` 与现役 `HWLibs.cpp:1986-1998` 一致）
constexpr UInt32 kSmuRespOk              = 0x01;  // SMU_RESP_OK
constexpr UInt32 kSmuRespFailed          = 0xFF;  // SMU_RESP_CMD_FAIL
constexpr UInt32 kSmuRespUnknownCmd      = 0xFE;  // SMU_RESP_CMD_UNKNOWN
constexpr UInt32 kSmuRespRejectedPrereq  = 0xFD;  // SMU_RESP_CMD_BAD_PREREQ
constexpr UInt32 kSmuRespRejectedBusy    = 0xFC;  // SMU_RESP_BUSY_OTHER
constexpr UInt32 kSmuRespNoResponse      = 0x00;  // SMU_RESP_NONE（轮询超时读到 0）

// 内部：将原始响应码映射为结果枚举
enum class SmuResult : UInt32 {
    Ok               = 0,
    NoResponse       = 1,  // 超时，响应寄存器仍为 0
    Failed           = 2,  // 0xFF
    UnknownCommand   = 3,  // 0xFE
    RejectedPrereq   = 4,  // 0xFD
    RejectedBusy     = 5,  // 0xFC
};

// 默认轮询超时（迭代次数），对齐 Linux：default_timeout = usec_timeout × 20 = 2,000,000（µs），
// 每轮 udelay(1)（smu_cmn.c:295-308 + smu_v13_0_4_ppt.c:1139）⇒ 预算 2 s。
// （T6 D1 修正：原为 200000×1µs = 0.2s，注释误写"= 2s"——算术错且比 Linux 短 10 倍。）
constexpr uint32_t kDefaultPollTimeoutIters = 2000000;
constexpr uint32_t kDefaultPollIntervalUs   = 1;

// 仅发送（不等待响应）——对应 Linux `smu_v13_0_send_msg_without_waiting` / `SMU_MSG_FLAG_ASYNC`
// 时序：清响应 → 写参数 → 写命令；不轮询。
// 返回 true 表示发送动作完成（不代表 SMU 已处理）。
inline bool sendOnly(display::RegSink& sink,
                     uint32_t msgId,
                     uint32_t param = 0)
{
    // ① 清响应寄存器
    sink.write(kSmu13RegResp, 0);
    // ② 写参数
    sink.write(kSmu13RegArg, param);
    // ③ 写命令（触发）
    sink.write(kSmu13RegMsg, msgId);
    return true;
}

// 仅等待响应（不发送命令）——对应 Linux `smu_msg_v1_wait_response` / `smu_cmn_wait_for_response`
// 用于分离式流程：先调用 sendOnly，做其它事，再调用 waitOnly 取回结果。
// 返回 SmuResult，outResp 可选（回传原始响应值）。
inline SmuResult waitOnly(display::RegSink& sink,
                          uint32_t* outResp = nullptr,
                          uint32_t pollTimeoutIters = kDefaultPollTimeoutIters,
                          uint32_t pollIntervalUs = kDefaultPollIntervalUs)
{
    // 配置轮询参数（RegSink 内部的 Poll 逻辑复用这些限制）
    sink.setPollLimits(pollTimeoutIters, pollIntervalUs);

    // 构造一个 Poll op：轮询响应寄存器直到 (值 & mask) != 0
    // 这里 mask = 0xFFFFFFFF（任意非零即就绪），等价于 Linux `reg & CONTENT_MASK != 0`
    display::RegOp pollOp;
    pollOp.kind  = display::RegOp::Kind::Poll;
    pollOp.addr  = kSmu13RegResp;
    pollOp.mask  = 0xFFFFFFFF;
    pollOp.value = 0;
    pollOp.shift = 0;

    // 执行轮询
    bool polled = sink.execute(pollOp);
    uint32_t resp = sink.lastValue();

    if (outResp) *outResp = resp;

    if (!polled) {
        // 超时：响应寄存器始终为 0
        return SmuResult::NoResponse;
    }

    // 解码响应码（对齐 `smu_msg_v1_decode_response`）
    switch (resp) {
        case kSmuRespOk:             return SmuResult::Ok;
        case kSmuRespFailed:         return SmuResult::Failed;
        case kSmuRespUnknownCmd:     return SmuResult::UnknownCommand;
        case kSmuRespRejectedPrereq: return SmuResult::RejectedPrereq;
        case kSmuRespRejectedBusy:   return SmuResult::RejectedBusy;
        default:                     return SmuResult::Failed;  // 未知非零响应视作失败
        // ⚠️ D10（T6）：Linux 解码 default → -EREMOTEIO（smu_cmn.c:284-288）；
        //    本实现映射为 Failed，语义接近（均视为失败）。
    }
}

// 完整发送-等待流程——对应 Linux `smu_msg_v1_send_msg`（同步路径，非 ASYNC）
// 时序：清响应 → 写参数 → 写命令 → 轮询响应 → 解码
// ⚠️ D9（T6）：Linux `smu_msg_v1_send_msg` 发送前有预轮询（等上一次响应完成，
//    smu_cmn.c:513-521）；本实现直接清响应后发送。查询类消息单发单等不重叠，影响低；
//    若将来背靠背连发（不等响应）需补预轮询。
// 返回 SmuResult，outResp 可选（回传原始响应值）。
inline SmuResult send(display::RegSink& sink,
                      uint32_t msgId,
                      uint32_t param = 0,
                      uint32_t* outResp = nullptr,
                      uint32_t pollTimeoutIters = kDefaultPollTimeoutIters,
                      uint32_t pollIntervalUs = kDefaultPollIntervalUs)
{
    // 发送阶段
    sendOnly(sink, msgId, param);
    // 等待阶段
    return waitOnly(sink, outResp, pollTimeoutIters, pollIntervalUs);
}

// 便利封装：查询类消息（TestMessage / GetSmuVersion / GetDriverIfVersion）
// 直接复用 PhoenixPPSMC.hpp 的消息 ID，禁止另造消息表。
inline SmuResult sendTestMessage(display::RegSink& sink, uint32_t* outResp = nullptr) {
    return send(sink, PhoenixPPSMC::PPSMC_MSG_TestMessage, 0, outResp);
}

inline SmuResult sendGetSmuVersion(display::RegSink& sink, uint32_t* outResp = nullptr) {
    return send(sink, PhoenixPPSMC::PPSMC_MSG_GetPmfwVersion, 0, outResp);
}

inline SmuResult sendGetDriverIfVersion(display::RegSink& sink, uint32_t* outResp = nullptr) {
    return send(sink, PhoenixPPSMC::PPSMC_MSG_GetDriverIfVersion, 0, outResp);
}

}  // namespace fw