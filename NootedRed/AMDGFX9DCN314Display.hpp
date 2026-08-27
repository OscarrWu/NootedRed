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

class AMDRadeonX5000_AMDGFX9DCN314Display : public AMDRadeonX5000_AMDGFX9DCNDisplay
{
    static VFT vft;

    static void Constructor(AMDRadeonX5000_AMDGFX9DCN314Display* self, const OSMetaClass* metaClass);

    static void initDCNRegOffs(AMDRadeonX5000_AMDGFX9DCN314Display* self);

    static AMDFlipOption getFlipOption(AMDRadeonX5000_AMDHWDisplay*);

public:
    PWDeclareRuntimeMC(AMDRadeonX5000_AMDGFX9DCN314Display, Constructor)

    static void resolve(const char* kext);
};
