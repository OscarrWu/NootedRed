// VBIOSSMC 显示时钟序列生成器
//
// 忠实翻译自 Linux：display/dc/clk_mgr/dcn314/dcn314_smu.c
//   - dcn314_smu_wait_for_response()      L96
//   - dcn314_smu_send_msg_with_param()    L118
//   - dcn314_smu_set_dispclk / set_dppclk / set_hard_min_dcfclk /
//     set_min_deep_sleep_dcfclk           L458-544
//
// 本生成器**只产出 RegOp 序列**，不读写任何硬件。
// 真机侧由 sink 消费（RegSink*.hpp 的具体实现），用户态侧由影子运行器消费。
//
// ⛔ 不含内核头文件。
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.

#pragma once

#include "RegOp.hpp"
#include "VBIOSSMC.hpp"

namespace display {
namespace vbios_smc {

// ── send_msg_with_param 的序列 ────────────────────────────────────────────────
//
// Linux 原文（dcn314_smu.c L118-160）：
//
//   result = dcn314_smu_wait_for_response(clk_mgr, 10, 200000);
//   if (result != VBIOSSMC_Result_OK)  print(...)
//   if (result == VBIOSSMC_Status_BUSY) return -1;
//   REG_WRITE(MP1_SMN_C2PMSG_91, VBIOSSMC_Status_BUSY);   // clear response
//   REG_WRITE(MP1_SMN_C2PMSG_83, param);                  // parameter (MHz)
//   REG_WRITE(MP1_SMN_C2PMSG_67, msg_id);                 // trigger
//   result = dcn314_smu_wait_for_response(clk_mgr, 10, 200000);
//   ...
//   return REG_READ(MP1_SMN_C2PMSG_83);
//
// ⚠️ 顺序是协议的一部分，不可重排：先等闲、再清响应、再写参数、最后写消息触发。
//
// 关于轮询：`wait_for_response` 是 `do { read(91); if (val != BUSY) break; } while(...)`，
// 其循环次数依赖真实硬件状态，无法离线预测 → 生成器产出单个 `Poll` op，
// 由 sink 在运行时执行（见 RegOp.hpp 的说明）。
inline void generateSendMsgWithParam(RegSeq& seq, RegAddr mailbox67, RegAddr mailbox83, RegAddr mailbox91,
                                     std::uint32_t msgId, std::uint32_t paramMHz) {
    // 1) 等待 SMU 空闲
    seq.push(regPollUntilNot(mailbox91, VBIOSSMC_Status_BUSY, "wait_idle_before"));

    // 2) 清响应寄存器
    seq.push(regWrite(mailbox91, VBIOSSMC_Status_BUSY, "clear_response"));

    // 3) 写参数（单位 MHz）
    seq.push(regWrite(mailbox83, paramMHz, "write_param"));

    // 4) 写消息号（触发事务）
    seq.push(regWrite(mailbox67, msgId, "trigger_msg"));

    // 5) 等待完成
    seq.push(regPollUntilNot(mailbox91, VBIOSSMC_Status_BUSY, "wait_complete"));

    // 6) 读回参数寄存器（Linux 用它作为"实际生效频率"的返回值）
    seq.push(regRead(mailbox83, "read_back_param"));
}

// khz → MHz 向上取整（Linux khz_to_mhz_ceil，clk_mgr_internal.h:578）
constexpr std::uint32_t khzToMhzCeil(std::uint32_t khz) { return (khz + 999u) / 1000u; }

// ── 四个改频 wrapper ─────────────────────────────────────────────────────────
//
// Linux 对应函数都在末尾调用 send_msg_with_param，参数为 khz_to_mhz_ceil(请求频率)。
// 四者的**消息号不同、其余序列完全相同**，故共用一个实现。

inline void generateSetDispclk(RegSeq& seq, RegAddr mb67, RegAddr mb83, RegAddr mb91, std::uint32_t khz) {
    generateSendMsgWithParam(seq, mb67, mb83, mb91, VBIOSSMC_MSG_SetDispclkFreq, khzToMhzCeil(khz));
}
inline void generateSetDppclk(RegSeq& seq, RegAddr mb67, RegAddr mb83, RegAddr mb91, std::uint32_t khz) {
    generateSendMsgWithParam(seq, mb67, mb83, mb91, VBIOSSMC_MSG_SetDppclkFreq, khzToMhzCeil(khz));
}
inline void generateSetDprefclk(RegSeq& seq, RegAddr mb67, RegAddr mb83, RegAddr mb91, std::uint32_t khz) {
    generateSendMsgWithParam(seq, mb67, mb83, mb91, VBIOSSMC_MSG_SetDprefclkFreq, khzToMhzCeil(khz));
}
inline void generateSetHardMinDcfclk(RegSeq& seq, RegAddr mb67, RegAddr mb83, RegAddr mb91, std::uint32_t khz) {
    generateSendMsgWithParam(seq, mb67, mb83, mb91, VBIOSSMC_MSG_SetHardMinDcfclkByFreq, khzToMhzCeil(khz));
}
inline void generateSetMinDeepSleepDcfclk(RegSeq& seq, RegAddr mb67, RegAddr mb83, RegAddr mb91, std::uint32_t khz) {
    generateSendMsgWithParam(seq, mb67, mb83, mb91, VBIOSSMC_MSG_SetMinDeepSleepDcfclk, khzToMhzCeil(khz));
}
inline void generateSetDisplayCount(RegSeq& seq, RegAddr mb67, RegAddr mb83, RegAddr mb91, std::uint32_t count) {
    // 注意：Linux 此处 param **不做** khz_to_mhz_ceil（display count 不是频率）
    generateSendMsgWithParam(seq, mb67, mb83, mb91, VBIOSSMC_MSG_SetDisplayCount, count);
}
inline void generateSetDisplayIdleOptimizations(RegSeq& seq, RegAddr mb67, RegAddr mb83, RegAddr mb91,
                                                std::uint32_t idleInfo) {
    generateSendMsgWithParam(seq, mb67, mb83, mb91, VBIOSSMC_MSG_SetDisplayIdleOptimizations, idleInfo);
}

}  // namespace vbios_smc
}  // namespace display
