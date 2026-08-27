// DCN 314 Display implementation for Phoenix (Radeon 780M, RDNA3)
// Derivative of AMDRadeonX5000 and AMDRadeonX6000 decompilation
// Ported from Linux amdgpu dcn314 (register offsets verified identical to DCN2)
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.
//
// 移植说明（2026-08-27）：
// - dcn314 的 HUBP/OTG 寄存器偏移与 DCN2 100% 相同（已双 Verifier 审查确认，见 docs/project-knowledge.md）
// - DCN 基址 0x34C0 与 NootedRed 的 DCN_BASE_2 一致（Linux dmub_dcn314.c:35）
// - 本类继承 AMDGFX9DCNDisplay 基类，仅覆写 initDCNRegOffs，寄存器值从 DCN2.hpp 复用
// - 待办：clk_mgr / resource / hwseq 的 DCN 3.1.4 特有逻辑（P0 优先级，后续移植）

#pragma once
#include <AMDGFX9DCNDisplay.hpp>

// -----------------------------------------------------------------------------
// 纯策略函数依赖类型 (Pure-strategy function dependency types)
// 移植自 Linux amdgpu dcn314 (signal_types.h, dccg.h, dc_hw_types.h, core_types.h)
// -----------------------------------------------------------------------------

enum signal_type {
    SIGNAL_TYPE_NONE            = 0L,
    SIGNAL_TYPE_DVI_SINGLE_LINK = (1 << 0),
    SIGNAL_TYPE_DVI_DUAL_LINK   = (1 << 1),
    SIGNAL_TYPE_HDMI_TYPE_A     = (1 << 2),
    SIGNAL_TYPE_LVDS            = (1 << 3),
    SIGNAL_TYPE_RGB             = (1 << 4),
    SIGNAL_TYPE_DISPLAY_PORT    = (1 << 5),
    SIGNAL_TYPE_DISPLAY_PORT_MST = (1 << 6),
    SIGNAL_TYPE_EDP             = (1 << 7),
    SIGNAL_TYPE_HDMI_FRL        = (1 << 8),
    SIGNAL_TYPE_VIRTUAL         = (1 << 9),
};

enum dc_pixel_encoding {
    PIXEL_ENCODING_UNDEFINED,
    PIXEL_ENCODING_RGB,
    PIXEL_ENCODING_YCBCR422,
    PIXEL_ENCODING_YCBCR444,
    PIXEL_ENCODING_YCBCR420,
    PIXEL_ENCODING_COUNT
};

enum pixel_rate_div {
    PIXEL_RATE_DIV_BY_1 = 0,
    PIXEL_RATE_DIV_BY_2 = 1,
    PIXEL_RATE_DIV_BY_4 = 3,
    PIXEL_RATE_DIV_NA   = 0xF
};

struct pixel_rate_divider {
    UInt32 div_factor1;
    UInt32 div_factor2;
};

struct dcn314_k1k2_inputs {
    enum signal_type        signal;
    enum dc_pixel_encoding  pixel_encoding;
    bool                    is_128b_132b_signal;
    bool                    two_pix_per_container;
    UInt32                  odm_combine_factor;
};

// DCN 3.1.4 显示时钟请求 (VBIOSSMC 下发)：单位 kHz
// 移植自 Linux dcn314_smu_set_dispclk / dcn314_clk_mgr_helper 的时钟请求结构
struct dcn314_display_clock_req {
    UInt32 dispclk_khz;              // 目标显示时钟
    UInt32 dppclk_khz;               // 目标 DPP 时钟
    UInt32 hard_min_dcfclk_khz;      // pstate 启用时的 hard min DCF 时钟
    UInt32 min_deep_sleep_dcfclk_khz; // pstate 启用时的 deep sleep DCF 时钟
    bool   pstate_enabled;           // 是否启用 pstate (下发 hard min / deep sleep dcfclk)
};

class AMDRadeonX5000_AMDGFX9DCN314Display : public AMDRadeonX5000_AMDGFX9DCNDisplay
{
    static VFT vft;

    static void Constructor(AMDRadeonX5000_AMDGFX9DCN314Display* self, const OSMetaClass* metaClass);

    static void initDCNRegOffs(AMDRadeonX5000_AMDGFX9DCN314Display* self);

    // DCN 3.1.4 显示时钟下发 (VBIOSSMC)：挂 VFT 时钟更新槽 (方案 A)
    // req 字段：dispclk_khz / dppclk_khz / hard_min_dcfclk_khz / min_deep_sleep_dcfclk_khz / pstate_enabled
    static void updateDisplayClocks(AMDRadeonX5000_AMDGFX9DCN314Display* self,
                                    const struct dcn314_display_clock_req* req);

    static AMDFlipOption getFlipOption(AMDRadeonX5000_AMDHWDisplay*);

    // ---- update_odm / resync_fifo 寄存器级直译 (设计: updateodm-resync-directreg-design.md) ----
    // update_odm_direct: 展平 dcn314_update_odm -> set_odm_combine / set_odm_bypass + set_out_rate_control
    //   regs        : HW 寄存器访问器 (self->getHWRegisters())
    //   otg_inst    : 本 OTG/ODM 实例号
    //   opp_inst[]  : 参与拼接的 OPP 实例号数组 (长度 = opp_cnt)
    //   opp_cnt     : OPP 数量 (1 = bypass, 2/4 = combine)
    //   slice_width : 单段 (每 OPP) 像素宽
    static void update_odm_direct(AMDRadeonX5000_AMDHWRegisters& regs,
                                  UInt32                          otg_inst,
                                  const UInt32                    opp_inst[],
                                  UInt32                          opp_cnt,
                                  UInt32                          slice_width);

    // resync_fifo_dccg_dio_direct: 展平 dcn314_dccg.c trigger_dio_fifo_resync
    //   仅动 DENTIST_DISPCLK_CNTL: 读 RDIVIDER -> 写 WDIVIDER
    static void resync_fifo_dccg_dio_direct(AMDRadeonX5000_AMDHWRegisters& regs);

public:
    PWDeclareRuntimeMC(AMDRadeonX5000_AMDGFX9DCN314Display, Constructor)

    static void resolve(const char* kext);
};
