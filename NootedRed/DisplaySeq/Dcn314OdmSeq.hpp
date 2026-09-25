// DCN 3.1.4 ODM 配置序列生成器
//
// 忠实翻译自 Linux（逐行对照，出处逐条注明）：
//   dcn314_hwseq.c:173-224   (dcn314_update_odm)
//   dcn314_hwseq.c:150-171   (get_odm_config)
//   dcn314_optc.c:50-104     (optc314_set_odm_combine)
//   dcn314_optc.c:159-180    (optc314_set_odm_bypass)
//   dcn30_mpc.c:109-127      (mpc3_set_out_rate_control)
//
// ⚠️ 注意 Linux 的两种寄存器写宏在硬件上**形态不同**（证据：`dc/dc_helper.c:59-126` 与真值序列）：
//   · `REG_UPDATE(...)`  → `generic_reg_update_ex` = **读-改-写**（1 读 + 1 写）⇒ 用 `regUpdate`
//   · `REG_SET(...)`     → `generic_reg_set_ex`  = **不读、直写**（0 读 + 1 写）⇒ 用 `regWrite`
//   真值实测印证：`OPTC_MEMORY_CONFIG`（REG_SET）只有写、无读；`OTG_H_TIMING_CNTL`（REG_UPDATE）读写成对。
//
// ⛔ 不含内核头文件、无动态分配、无异常、无浮点。
// ⛔ 禁止 <cstdint>/<cstddef>/std::（kext 环境没有它们）—— 用 <stdint.h>/<stddef.h> + 全局类型名。
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.

#pragma once

#include "RegOp.hpp"

#include <stddef.h>
#include <stdint.h>

namespace display {
namespace dcn314_odm {

// ── 寄存器偏移（值取自 src/NootedRed/Regs/DCN314.hpp；那里含 IOKit 头，故此处复制为 uint32_t）──
//    段基址由调用方传入（BASE_IDX 不同：OPTC/OTG 在 SEG2、MPC_OUTn_MUX 在 SEG3）。
constexpr uint32_t kOptcDataSourceSelect = 0x1ACB;  // ODM0（BASE_IDX=2）
constexpr uint32_t kOptcWidthControl     = 0x1ACE;  // ODM0（BASE_IDX=2）
constexpr uint32_t kOptcMemoryConfig     = 0x1AD0;  // ODM0（BASE_IDX=2）
constexpr uint32_t kOtgHTimingCntl       = 0x1B2E;  // OTG0（BASE_IDX=2）
constexpr uint32_t kMpcOut0Mux           = 0x0580;  // MPC_OUT0（BASE_IDX=3）
constexpr uint32_t kOdmRegStride         = 0x10;    // ODM_REG_STRIDE
constexpr uint32_t kOtgRegStride         = 0x80;    // OTG_REG_STRIDE
constexpr uint32_t kMpcOutMuxStride      = 0x4;     // MPC_OUT_MUX_STRIDE

// ── 字段掩码/移位（逐条取自 Regs/DCN314.hpp，也就是 Linux dcn_3_1_4_sh_mask.h）──
// OPTC_DATA_SOURCE_SELECT
constexpr uint32_t kNumInputSegShift   = 0x0;
constexpr uint32_t kNumInputSegMask    = 0x00000003u;
constexpr uint32_t kSeg0SrcSelShift    = 0x10;
constexpr uint32_t kSeg0SrcSelMask     = 0x000F0000u;
constexpr uint32_t kSeg1SrcSelShift    = 0x14;
constexpr uint32_t kSeg1SrcSelMask     = 0x00F00000u;
constexpr uint32_t kSeg2SrcSelShift    = 0x18;
constexpr uint32_t kSeg2SrcSelMask     = 0x0F000000u;
constexpr uint32_t kSeg3SrcSelShift    = 0x1C;
constexpr uint32_t kSeg3SrcSelMask     = 0xF0000000u;
// OPTC_MEMORY_CONFIG
constexpr uint32_t kMemSelShift        = 0x0;
constexpr uint32_t kMemSelMask         = 0x0000FFFFu;
// OPTC_WIDTH_CONTROL
constexpr uint32_t kSegmentWidthShift  = 0x0;
constexpr uint32_t kSegmentWidthMask   = 0x00001FFFu;
// OTG_H_TIMING_CNTL
constexpr uint32_t kDivModeShift       = 0x0;
constexpr uint32_t kDivModeMask        = 0x00000003u;
// MPC_OUTn_MUX
constexpr uint32_t kOutRateCtrlDisableShift = 8;
constexpr uint32_t kOutRateCtrlDisableMask  = 0x00000100u;
constexpr uint32_t kOutRateCtrlShift        = 9;
constexpr uint32_t kOutRateCtrlMask         = 0x00000200u;

// Linux `enum h_timing_div_mode`（该项目用 0 = NO_DIV；`is_two_pixels_per_container` 返回它）
constexpr uint32_t kHTimingNoDiv    = 0;
constexpr uint32_t kHTimingDivBy2   = 1;

// ── ODM 内存实例掩码（Linux `optc314_set_odm_combine`，dcn314_optc.c:55-79 逐字翻译）──
// 注意分支结构照抄：`opp_cnt == 4` 走一套；**`opp_cnt == 2` 走另一套**（Linux 的 else 分支），
// 两者的算法不同（前者返回固定字，后者按 opp_id 左移）。不要"按 opp 循环"自行改写。
inline uint32_t computeOdmMemoryMask(const uint32_t* oppInst, uint32_t oppCnt, uint32_t sliceWidth)
{
    const uint32_t hActive     = sliceWidth * oppCnt;            // int h_active = segment_width * opp_cnt
    const uint32_t odmMemCount = (hActive + 2047u) / 2048u;      // (h_active + 2047) / 2048

    if (oppCnt == 4) {
        if (odmMemCount <= 2) { return 0x3u; }
        if (odmMemCount <= 4) { return 0xfu; }
        return 0x3fu;
    }

    // opp_cnt == 2（Linux 的 else 分支）
    if (odmMemCount <= 2) {
        return (1u << (oppInst[0] * 2u)) | (1u << (oppInst[1] * 2u));
    }
    if (odmMemCount <= 4) {
        return (3u << (oppInst[0] * 2u)) | (3u << (oppInst[1] * 2u));
    }
    return 0x77u;
}

// 生成"配置一个 OTG 的 ODM 拓扑"的寄存器序列。
//   dcnSeg2Base : DCN 段 2 基址（= 0x34C0；OPTC/OTG 寄存器，BASE_IDX=2）
//   （MPC 部分的段基址由 generateSetOutRateControl 接收，见本文件末尾）
//   otgInst     : OTG/ODM 实例号（Linux 的 `pipe_ctx->stream_res.tg->inst`）
//   oppInst     : 参与拼接的 OPP 实例号数组（长度 = oppCnt；单管时只用 [0]）
//   oppCnt      : 1 = bypass（不拼接）；2 或 4 = combine
//   sliceWidth  : 单段（每 OPP）像素宽（combine 用；bypass 忽略）
//   twoPixelsPerContainer : bypass 时 Linux `is_two_pixels_per_container(dc_crtc_timing)` 的结果
inline void generateUpdateOdm(RegSeq& out, uint32_t dcnSeg2Base, uint32_t otgInst, const uint32_t* oppInst,
                              uint32_t oppCnt, uint32_t sliceWidth, bool twoPixelsPerContainer)
{
    const uint32_t dssAddr  = dcnSeg2Base + kOptcDataSourceSelect + kOdmRegStride * otgInst;
    const uint32_t wctlAddr = dcnSeg2Base + kOptcWidthControl + kOdmRegStride * otgInst;
    const uint32_t mcfgAddr = dcnSeg2Base + kOptcMemoryConfig + kOdmRegStride * otgInst;
    const uint32_t htcAddr  = dcnSeg2Base + kOtgHTimingCntl + kOtgRegStride * otgInst;

    if (oppCnt <= 1) {
        // ═══ set_odm_bypass（dcn314_optc.c:159-180）═══
        //
        // REG_SET_5(OPTC_DATA_SOURCE_SELECT, 0,
        //     OPTC_NUM_OF_INPUT_SEGMENT, 0, OPTC_SEG0_SRC_SEL, optc->inst,
        //     OPTC_SEG1_SRC_SEL, 0xf, OPTC_SEG2_SRC_SEL, 0xf, OPTC_SEG3_SRC_SEL, 0xf)
        // → REG_SET_5 ⇒ generic_reg_set_ex（不读）⇒ 一个 regWrite。
        {
            uint32_t val = 0;
            val |= (0u << kNumInputSegShift) & kNumInputSegMask;                 // NUM_OF_INPUT_SEGMENT = 0
            val |= (otgInst << kSeg0SrcSelShift) & kSeg0SrcSelMask;              // SEG0_SRC_SEL = optc->inst
            val |= (0xFu << kSeg1SrcSelShift) & kSeg1SrcSelMask;                 // SEG1 = 0xf
            val |= (0xFu << kSeg2SrcSelShift) & kSeg2SrcSelMask;                 // SEG2 = 0xf
            val |= (0xFu << kSeg3SrcSelShift) & kSeg3SrcSelMask;                 // SEG3 = 0xf
            // otgInst = 0 时应得 0xFFF00000（真值实测值，见第六步执行记录 §五）
            out.push(regWrite(dssAddr, val, "odm_bypass:data_source_select"));
        }

        // REG_UPDATE(OTG_H_TIMING_CNTL, OTG_H_TIMING_DIV_MODE, h_div)
        // h_div = is_two_pixels_per_container(dc_crtc_timing)（dcn314_optc.c:173）
        // → REG_UPDATE ⇒ 读-改-写 ⇒ 一个 regUpdate。
        out.push(regUpdate(htcAddr, kDivModeMask, kDivModeShift,
                           twoPixelsPerContainer ? kHTimingDivBy2 : kHTimingNoDiv, "odm_bypass:htiming_div_mode"));

        // REG_SET(OPTC_MEMORY_CONFIG, 0, OPTC_MEM_SEL, 0)（dcn314_optc.c:177-178）
        // → REG_SET ⇒ generic_reg_set_ex（不读）⇒ 一个 regWrite，值 = (0 & ~mask) | 0 = 0。
        out.push(regWrite(mcfgAddr, (0u & ~kMemSelMask) | ((0u << kMemSelShift) & kMemSelMask),
                          "odm_bypass:memory_config"));
        return;
    }

    // ═══ set_odm_combine（dcn314_optc.c:50-104）═══
    //
    // REG_SET(OPTC_MEMORY_CONFIG, 0, OPTC_MEM_SEL, memory_mask)（:81-82）
    // → REG_SET ⇒ 一个 regWrite。
    {
        const uint32_t memoryMask = computeOdmMemoryMask(oppInst, oppCnt, sliceWidth);
        out.push(regWrite(mcfgAddr, (0u & ~kMemSelMask) | ((memoryMask << kMemSelShift) & kMemSelMask),
                          "odm_combine:memory_config"));
    }

    // REG_SET_3 / REG_SET_5(OPTC_DATA_SOURCE_SELECT, 0, ...)（:84-96）⇒ 一个 regWrite（不读）。
    {
        uint32_t val = 0;
        if (oppCnt == 2) {
            val |= (1u << kNumInputSegShift) & kNumInputSegMask;                 // NUM_OF_INPUT_SEGMENT = 1
            val |= (oppInst[0] << kSeg0SrcSelShift) & kSeg0SrcSelMask;
            val |= (oppInst[1] << kSeg1SrcSelShift) & kSeg1SrcSelMask;
        } else {  // oppCnt == 4
            val |= (3u << kNumInputSegShift) & kNumInputSegMask;                 // NUM_OF_INPUT_SEGMENT = 3
            val |= (oppInst[0] << kSeg0SrcSelShift) & kSeg0SrcSelMask;
            val |= (oppInst[1] << kSeg1SrcSelShift) & kSeg1SrcSelMask;
            val |= (oppInst[2] << kSeg2SrcSelShift) & kSeg2SrcSelMask;
            val |= (oppInst[3] << kSeg3SrcSelShift) & kSeg3SrcSelMask;
        }
        out.push(regWrite(dssAddr, val, "odm_combine:data_source_select"));
    }

    // REG_UPDATE(OPTC_WIDTH_CONTROL, OPTC_SEGMENT_WIDTH, segment_width)（:98-99）⇒ 一个 regUpdate。
    out.push(regUpdate(wctlAddr, kSegmentWidthMask, kSegmentWidthShift, sliceWidth, "odm_combine:segment_width"));

    // REG_UPDATE(OTG_H_TIMING_CNTL, OTG_H_TIMING_DIV_MODE, opp_cnt - 1)（:101-102）⇒ 一个 regUpdate。
    out.push(regUpdate(htcAddr, kDivModeMask, kDivModeShift, oppCnt - 1u, "odm_combine:htiming_div_mode"));
}

// ── set_out_rate_control（dcn30_mpc.c:109-127）──
// 对每个参与拼接的 OPP 各产生**一次**读改写：
//   Linux: REG_UPDATE_2(MUX[opp_id], MPC_OUT_RATE_CONTROL_DISABLE, 1, MPC_OUT_RATE_CONTROL, 0)
//   底层 `generic_reg_update_ex(ctx, addr, n=2, ...)` = 一次读 + 一次写（两个字段在同一次里改）。
//   RegOp 只有"单掩码 + 单移位 + 单值"，故把两字段合并成一次 Update：
//     (v & ~(DISABLE_MASK | RATE_MASK)) | ((1 << DISABLE_SHIFT) | (0 << RATE_SHIFT))
//   真值实测（地址 0x9580）每次调用正是"1 读 1 写"，与本式一致。
constexpr uint32_t kOutRateCombinedMask = kOutRateCtrlDisableMask | kOutRateCtrlMask;
constexpr uint32_t kOutRateCombinedVal  = ((1u << kOutRateCtrlDisableShift) & kOutRateCtrlDisableMask) |
                                         ((0u << kOutRateCtrlShift) & kOutRateCtrlMask);

inline void generateSetOutRateControl(RegSeq& out, uint32_t dcnSeg3Base, const uint32_t* oppInst, uint32_t oppCnt)
{
    for (uint32_t i = 0; i < oppCnt; ++i) {
        const uint32_t muxAddr = dcnSeg3Base + kMpcOut0Mux + kMpcOutMuxStride * oppInst[i];
        out.push(regUpdate(muxAddr, kOutRateCombinedMask, 0, kOutRateCombinedVal, "out_rate_ctrl:disable+rate"));
    }
}

// 完整的一轮 ODM 配置 = Linux `dcn314_update_odm`（dcn314_hwseq.c:173-224）的寄存器部分：
//   先按拓扑配置（bypass 或 combine），再对每个 OPP 设置 out rate control。
//   （Linux 在该函数末尾还有 DSC 分支 `update_dsc_on_stream` 与 `opp_pipe_clock_control`，
//     本项目暂不支持 DSC / 无 OPP 对象，见第六步执行记录的缺口登记。）
inline void generateUpdateOdmFull(RegSeq& out, uint32_t dcnSeg2Base, uint32_t dcnSeg3Base, uint32_t otgInst,
                                  const uint32_t* oppInst, uint32_t oppCnt, uint32_t sliceWidth,
                                  bool twoPixelsPerContainer)
{
    generateUpdateOdm(out, dcnSeg2Base, otgInst, oppInst, oppCnt, sliceWidth, twoPixelsPerContainer);
    generateSetOutRateControl(out, dcnSeg3Base, oppInst, oppCnt);
}

}  // namespace dcn314_odm
}  // namespace display
