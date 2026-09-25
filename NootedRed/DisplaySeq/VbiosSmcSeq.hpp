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
// ── 新增：Linux dcn314_smu.c 其余 wrapper 的序列生成 ──────────────────────────
//
// 对应 Linux 函数与行号（dcn314_smu.c）：
//   get_smu_version           L166-172  → msg 0x2, param 0
//   (TestMessage)             L64       → msg 0x1, param 由调用方给
//   PowerUpGfx                L66       → msg 0x3, param 0
//   enable_phy_refclk_pwrdwn  L274-290  → msg 0xC, param = idle_info(df=1, phy=1) 或 0
//   enable_pme_wa             L292-301  → msg 0xD, param 0
//   set_dram_addr_high        L303-310  → msg 0xE, param = addrHigh（不换算）
//   set_dram_addr_low         L312-319  → msg 0xF, param = addrLow（不换算）
//   transfer_dpm_table_smu2dram L321-328 → msg 0x10, param = TABLE_DPMCLOCKS(4)
//   transfer_wm_table_dram2smu  L330-337 → msg 0x11, param = TABLE_WATERMARKS(1)
//   set_zstate_support        L339-386  → msg 0x15, param 依 support 分流（见下）
//   set_dtbclk                L388-398  → msg 0x17, param = enable?1:0

// Linux dcn315_smu.h:102-112 的 display_idle_optimization 位域（dcn314 同构）：
//   bit 0: df_request_disabled
//   bit 1: phy_ref_clk_off
//   bit 2: s0i2_rdy
constexpr std::uint32_t IDLE_OPT_DF_REQUEST_DISABLED = 1u << 0;
constexpr std::uint32_t IDLE_OPT_PHY_REF_CLK_OFF     = 1u << 1;
constexpr std::uint32_t IDLE_OPT_S0I2_RDY            = 1u << 2;

// Linux dcn31_smu.h:217-221 / dcn314_smu.c:326-336 的表 ID 常量
constexpr std::uint32_t TABLE_DPMCLOCKS  = 4;
constexpr std::uint32_t TABLE_WATERMARKS = 1;

// Linux dc.h:736-743 的 zstate 支持枚举值（dcn314_smu.c:346-377 逐 case 对应）
enum ZStateSupport : std::uint32_t {
    ZSTATE_UNKNOWN             = 0,  // DCN_ZSTATE_SUPPORT_UNKNOWN
    ZSTATE_ALLOW               = 1,  // DCN_ZSTATE_SUPPORT_ALLOW
    ZSTATE_ALLOW_Z8_ONLY       = 2,  // DCN_ZSTATE_SUPPORT_ALLOW_Z8_ONLY
    ZSTATE_ALLOW_Z8_Z10_ONLY   = 3,  // DCN_ZSTATE_SUPPORT_ALLOW_Z8_Z10_ONLY
    ZSTATE_ALLOW_Z10_ONLY      = 4,  // DCN_ZSTATE_SUPPORT_ALLOW_Z10_ONLY
    ZSTATE_DISALLOW            = 5,  // DCN_ZSTATE_SUPPORT_DISALLOW
};

// get_smu_version：msg 0x2, param 0
// Linux: dcn314_smu_get_smu_version (L166-172)
inline void generateGetSmuVersion(RegSeq& seq, RegAddr mb67, RegAddr mb83, RegAddr mb91) {
    generateSendMsgWithParam(seq, mb67, mb83, mb91, VBIOSSMC_MSG_GetSmuVersion, 0);
}

// TestMessage：msg 0x1, param 透传
// Linux: VBIOSSMC_MSG_TestMessage (L64)
inline void generateTestMessage(RegSeq& seq, RegAddr mb67, RegAddr mb83, RegAddr mb91, std::uint32_t param) {
    generateSendMsgWithParam(seq, mb67, mb83, mb91, VBIOSSMC_MSG_TestMessage, param);
}

// PowerUpGfx：msg 0x3, param 0
// Linux: VBIOSSMC_MSG_PowerUpGfx (L66)
inline void generatePowerUpGfx(RegSeq& seq, RegAddr mb67, RegAddr mb83, RegAddr mb91) {
    generateSendMsgWithParam(seq, mb67, mb83, mb91, VBIOSSMC_MSG_PowerUpGfx, 0);
}

// enable_phy_refclk_pwrdwn：msg 0xC
// Linux: dcn314_smu_enable_phy_refclk_pwrdwn (L274-290)
//   enable=true  → idle_info = {df_request_disabled=1, phy_ref_clk_off=1, s0i2_rdy=0} = 0x3
//   enable=false → idle_info = 0
inline void generateEnablePhyRefclkPwrdwn(RegSeq& seq, RegAddr mb67, RegAddr mb83, RegAddr mb91, bool enable) {
    std::uint32_t idleInfo = enable ? (IDLE_OPT_DF_REQUEST_DISABLED | IDLE_OPT_PHY_REF_CLK_OFF) : 0;
    generateSendMsgWithParam(seq, mb67, mb83, mb91, VBIOSSMC_MSG_EnableTmdp48MHzRefclkPwrDown, idleInfo);
}

// enable_pme_wa：msg 0xD, param 0
// Linux: dcn314_smu_enable_pme_wa (L292-301)
inline void generateEnablePmeWa(RegSeq& seq, RegAddr mb67, RegAddr mb83, RegAddr mb91) {
    generateSendMsgWithParam(seq, mb67, mb83, mb91, VBIOSSMC_MSG_UpdatePmeRestore, 0);
}

// set_vbios_dram_addr_high：msg 0xE, param = addrHigh（原始地址高位，不换算）
// Linux: dcn314_smu_set_dram_addr_high (L303-310)
inline void generateSetVbiosDramAddrHigh(RegSeq& seq, RegAddr mb67, RegAddr mb83, RegAddr mb91, std::uint32_t addrHigh) {
    generateSendMsgWithParam(seq, mb67, mb83, mb91, VBIOSSMC_MSG_SetVbiosDramAddrHigh, addrHigh);
}

// set_vbios_dram_addr_low：msg 0xF, param = addrLow（原始地址低位，不换算）
// Linux: dcn314_smu_set_dram_addr_low (L312-319)
inline void generateSetVbiosDramAddrLow(RegSeq& seq, RegAddr mb67, RegAddr mb83, RegAddr mb91, std::uint32_t addrLow) {
    generateSendMsgWithParam(seq, mb67, mb83, mb91, VBIOSSMC_MSG_SetVbiosDramAddrLow, addrLow);
}

// transfer_dpm_table_smu2dram：msg 0x10, param = TABLE_DPMCLOCKS(4)
// Linux: dcn314_smu_transfer_dpm_table_smu_2_dram (L321-328)
inline void generateTransferDpmTableSmu2Dram(RegSeq& seq, RegAddr mb67, RegAddr mb83, RegAddr mb91) {
    generateSendMsgWithParam(seq, mb67, mb83, mb91, VBIOSSMC_MSG_TransferTableSmu2Dram, TABLE_DPMCLOCKS);
}

// transfer_wm_table_dram2smu：msg 0x11, param = TABLE_WATERMARKS(1)
// Linux: dcn314_smu_transfer_wm_table_dram_2_smu (L330-337)
inline void generateTransferWmTableDram2Smu(RegSeq& seq, RegAddr mb67, RegAddr mb83, RegAddr mb91) {
    generateSendMsgWithParam(seq, mb67, mb83, mb91, VBIOSSMC_MSG_TransferTableDram2Smu, TABLE_WATERMARKS);
}

// set_zstate_support：msg 0x15, param 依 support 分流
// Linux: dcn314_smu_set_zstate_support (L339-386)
//   所有 case 统一用 msg 0x15 (AllowZstatesEntry)，0x16 (DisallowZstatesEntry) 在 dcn314 未使用
//   参数编码（bit 8=Z8, bit 9=Z9?, bit 10=Z10）：
//     ALLOW               → 0x700 (bits 8,9,10)
//     DISALLOW            → 0x0
//     ALLOW_Z10_ONLY      → 0x400 (bit 10)
//     ALLOW_Z8_Z10_ONLY   → 0x500 (bits 8,10)
//     ALLOW_Z8_ONLY       → 0x100 (bit 8)
//     UNKNOWN (default)   → 0x0
inline void generateSetZstateSupport(RegSeq& seq, RegAddr mb67, RegAddr mb83, RegAddr mb91, std::uint32_t support) {
    std::uint32_t param = 0;
    switch (support) {
    case ZSTATE_ALLOW:
        param = (1u << 10) | (1u << 9) | (1u << 8);  // 0x700
        break;
    case ZSTATE_DISALLOW:
        param = 0;
        break;
    case ZSTATE_ALLOW_Z10_ONLY:
        param = (1u << 10);  // 0x400
        break;
    case ZSTATE_ALLOW_Z8_Z10_ONLY:
        param = (1u << 10) | (1u << 8);  // 0x500
        break;
    case ZSTATE_ALLOW_Z8_ONLY:
        param = (1u << 8);  // 0x100
        break;
    case ZSTATE_UNKNOWN:
    default:
        param = 0;
        break;
    }
    generateSendMsgWithParam(seq, mb67, mb83, mb91, VBIOSSMC_MSG_AllowZstatesEntry, param);
}

// set_dtbclk：msg 0x17, param = enable?1:0
// Linux: dcn314_smu_set_dtbclk (L388-398) "Arg = 1: Turn DTB on; 0: Turn DTB CLK OFF"
inline void generateSetDtbClk(RegSeq& seq, RegAddr mb67, RegAddr mb83, RegAddr mb91, bool enable) {
    generateSendMsgWithParam(seq, mb67, mb83, mb91, VBIOSSMC_MSG_SetDtbClk, enable ? 1u : 0u);
}

}  // namespace vbios_smc
}  // namespace display
