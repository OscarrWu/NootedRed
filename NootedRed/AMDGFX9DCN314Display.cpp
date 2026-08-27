// DCN 314 Display implementation for Phoenix (Radeon 780M, RDNA3)
// Derivative of AMDRadeonX5000 and AMDRadeonX6000 decompilation
// Ported from Linux amdgpu dcn314 (register offsets verified identical to DCN2)
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#include <AMDGFX9DCN314Display.hpp>
#include <AMDGFX9DCNDisplay.hpp>
#include <HWLibs.hpp>
#include <GPUDriversAMD/Accel/HWDisplay.hpp>
#include <GPUDriversAMD/RavenIPOffset.hpp>
#include <Headers/kern_util.hpp>
#include <PenguinWizardry/RuntimeMC.hpp>
#include <PenguinWizardry/RuntimeVFT.hpp>
#include <Regs/DCN314.hpp>
#include <libkern/OSTypes.h>
#include <libkern/c++/OSMetaClass.h>

// -----------------------------------------------------------------------------
// 纯策略函数: dc_is_*_signal 纯 helper (移植自 Linux signal_types.h)
// -----------------------------------------------------------------------------
static inline bool dc_is_hdmi_tmds_signal(enum signal_type signal)
{
    return (signal == SIGNAL_TYPE_HDMI_TYPE_A);
}

static inline bool dc_is_hdmi_frl_signal(enum signal_type signal)
{
    return (signal == SIGNAL_TYPE_HDMI_FRL);
}

static inline bool dc_is_hdmi_signal(enum signal_type signal)
{
    return (dc_is_hdmi_tmds_signal(signal) || dc_is_hdmi_frl_signal(signal));
}

static inline bool dc_is_dp_signal(enum signal_type signal)
{
    return (signal == SIGNAL_TYPE_DISPLAY_PORT ||
            signal == SIGNAL_TYPE_EDP ||
            signal == SIGNAL_TYPE_DISPLAY_PORT_MST);
}

static inline bool dc_is_dvi_signal(enum signal_type signal)
{
    switch (signal) {
    case SIGNAL_TYPE_DVI_SINGLE_LINK:
    case SIGNAL_TYPE_DVI_DUAL_LINK:
        return true;
    default:
        return false;
    }
}

static inline bool dc_is_virtual_signal(enum signal_type signal)
{
    return (signal == SIGNAL_TYPE_VIRTUAL);
}

// -----------------------------------------------------------------------------
// 纯策略函数 1: dcn314_calc_k1_k2_values
//   移植自 dcn314_calculate_dccg_k1_k2_values (dcn314_hwseq.c L329)
//   去寄存器化: 所有 vtable/资源遍历输入由 dcn314_k1k2_inputs 预解析提供
// -----------------------------------------------------------------------------
static unsigned int dcn314_calc_k1_k2_values(const struct dcn314_k1k2_inputs *in,
                                              unsigned int *k1_div,
                                              unsigned int *k2_div)
{
    unsigned int odm_combine_factor = in->odm_combine_factor;
    bool two_pix_per_container = in->two_pix_per_container;

    *k1_div = PIXEL_RATE_DIV_NA;
    *k2_div = PIXEL_RATE_DIV_NA;

    if (dc_is_hdmi_frl_signal(in->signal) ||
        in->is_128b_132b_signal) {
        *k1_div = PIXEL_RATE_DIV_BY_1;
        *k2_div = PIXEL_RATE_DIV_BY_1;
    } else if (dc_is_hdmi_tmds_signal(in->signal) ||
               dc_is_dvi_signal(in->signal)) {
        *k1_div = PIXEL_RATE_DIV_BY_1;
        if (in->pixel_encoding == PIXEL_ENCODING_YCBCR420)
            *k2_div = PIXEL_RATE_DIV_BY_2;
        else
            *k2_div = PIXEL_RATE_DIV_BY_4;
    } else if (dc_is_dp_signal(in->signal) ||
               dc_is_virtual_signal(in->signal)) {
        if (two_pix_per_container) {
            *k1_div = PIXEL_RATE_DIV_BY_1;
            *k2_div = PIXEL_RATE_DIV_BY_2;
        } else {
            *k1_div = PIXEL_RATE_DIV_BY_1;
            *k2_div = PIXEL_RATE_DIV_BY_4;
            if (odm_combine_factor == 2)
                *k2_div = PIXEL_RATE_DIV_BY_2;
        }
    }

    return odm_combine_factor;
}

// -----------------------------------------------------------------------------
// 纯策略函数 2: dcn314_calc_pix_rate_divider
//   移植自 dcn314_calculate_pix_rate_divider (dcn314_hwseq.c L366)
//   去寄存器化: 资源查找由调用方完成, 直接接收已解析的 inputs
// -----------------------------------------------------------------------------
static void dcn314_calc_pix_rate_divider(struct pixel_rate_divider *out,
                                          const struct dcn314_k1k2_inputs *in)
{
    unsigned int k1_div = PIXEL_RATE_DIV_NA;
    unsigned int k2_div = PIXEL_RATE_DIV_NA;

    dcn314_calc_k1_k2_values(in, &k1_div, &k2_div);

    out->div_factor1 = k1_div;
    out->div_factor2 = k2_div;
}

PWDefineRuntimeMC(AMDRadeonX5000_AMDGFX9DCN314Display, Constructor)

AMDRadeonX5000_AMDGFX9DCNDisplay::VFT AMDRadeonX5000_AMDGFX9DCN314Display::vft;

void AMDRadeonX5000_AMDGFX9DCN314Display::Constructor(AMDRadeonX5000_AMDGFX9DCN314Display* const self,
                                                      const OSMetaClass* const                 metaClass)
{
    assert(AMDRadeonX5000_AMDHWDisplay::constructor() != 0);
    FunctionCast(Constructor, AMDRadeonX5000_AMDHWDisplay::constructor())(self, metaClass);
    vft.replaceVFT(self);
    gRTMetaClass.instanceConstructed();
}

void AMDRadeonX5000_AMDGFX9DCN314Display::resolve(const char* const kext)
{
    AMDRadeonX5000_AMDGFX9DCNDisplay::populateVFT(vft);
    PWPopulateRuntimeMCGetMetaClassVFTEntry();
    vft.getExpanded<decltype(initDCNRegOffs)>(0) = initDCNRegOffs;
    vft.getExpanded<decltype(updateDisplayClocks)>(1) = updateDisplayClocks;

    // 覆写 getFlipOption：dcn314 返回 DCN3（基类硬编码 DCN2）
    constants.vftGetFlipOption(vft.inner()) = getFlipOption;

    PenguinWizardry::RuntimeMCManager::singleton().registerMC(gRTMetaClass, kext,
                                                              AMDRadeonX5000_AMDGFX9DCNDisplay::gRTMetaClass);

    DBGLOG("GFX9DCN314Display", "Module initialised");
}

// dcn314 HUBP/OTG 寄存器偏移：与 DCN2 100% 一致（已审查确认）
// 见 docs/project-knowledge.md「DCN2 与 dcn314 寄存器偏移 100% 一致【已审查✅ 双 Verifier】」
void AMDRadeonX5000_AMDGFX9DCN314Display::initDCNRegOffs(AMDRadeonX5000_AMDGFX9DCN314Display* const self)
{
    auto& expansion = self->getExpansion();
    for (UInt32 i = 0; i < MAX_SUPPORTED_DISPLAYS_RV; i += 1) {
        const UInt32 hubpRegStride               = HUBP_REG_STRIDE * i;
        const UInt32 otgRegStride                = OTG_REG_STRIDE * i;
        auto&        regOffs                     = expansion.regOffs[i];
        regOffs.isValid                          = true;
        regOffs.hubpretControl                   = DCN_BASE_2 + HUBPRET_CONTROL + hubpRegStride;
        regOffs.hubpSurfaceConfig                = DCN_BASE_2 + HUBP_SURFACE_CONFIG + hubpRegStride;
        regOffs.hubpAddrConfig                   = DCN_BASE_2 + HUBP_ADDR_CONFIG + hubpRegStride;
        regOffs.hubpTilingConfig                 = DCN_BASE_2 + HUBP_TILING_CONFIG + hubpRegStride;
        regOffs.hubpPriViewportStart             = DCN_BASE_2 + HUBP_PRI_VIEWPORT_START + hubpRegStride;
        regOffs.hubpPriViewportDimension         = DCN_BASE_2 + HUBP_PRI_VIEWPORT_DIMENSION + hubpRegStride;
        regOffs.hubpreqSurfacePitch              = DCN_BASE_2 + HUBPREQ_SURFACE_PITCH + hubpRegStride;
        regOffs.hubpreqPrimarySurfaceAddress     = DCN_BASE_2 + HUBPREQ_PRIMARY_SURFACE_ADDRESS + hubpRegStride;
        regOffs.hubpreqPrimarySurfaceAddressHigh = DCN_BASE_2 + HUBPREQ_PRIMARY_SURFACE_ADDRESS_HIGH + hubpRegStride;
        regOffs.hubpreqFlipControl               = DCN_BASE_2 + HUBPREQ_FLIP_CONTROL + hubpRegStride;
        regOffs.hubpreqSurfaceEarliestInuse      = DCN_BASE_2 + HUBPREQ_SURFACE_EARLIEST_INUSE + hubpRegStride;
        regOffs.hubpreqSurfaceEarliestInuseHigh  = DCN_BASE_2 + HUBPREQ_SURFACE_EARLIEST_INUSE_HIGH + hubpRegStride;
        regOffs.otgControl                       = DCN_BASE_2 + OTG_CONTROL + otgRegStride;
        regOffs.otgInterlaceControl              = DCN_BASE_2 + OTG_INTERLACE_CONTROL + otgRegStride;
    }

    expansion.regShiftsMasks.viewportYStartMask  = 0x3FFF0000;
    expansion.regShiftsMasks.viewportYStartShift = 16;
    expansion.regShiftsMasks.viewportHeightMask  = 0x3FFF0000;
    expansion.regShiftsMasks.viewportHeightShift = 16;
    expansion.regShiftsMasks.primarySurfaceHi    = 0xFFFF;
    expansion.regShiftsMasks.otgEnable           = 1;
    expansion.regShiftsMasks.otgInterlaceEnable  = 1;
    expansion.regShiftsMasks.isValid             = true;
}

// DCN 3.1.4 显示时钟下发 (VBIOSSMC, 方案 A)
//   - 内部统一经 X5000HWLibs 的 cached wrapper 下发，wrapper 已用 smuCtxCache + smu12IsFwLoaded 守卫
//   - 单位：输入 kHz；wrapper 内部 khzToMhzCeil → MHz 写入 C2PMSG_83
//   移植自 Linux dcn314_smu_set_dispclk / dcn314_clk_mgr 的 update_clocks 下发路径
//
//   注：pstate 的 hard min / deep sleep dcfclk 因 HWLibs 尚无对应 cached wrapper
//   （smuCtxCache 为 private，本文件无法直接取 ctx），暂不在此下发；待 HWLibs 补
//   vbiossmcSetHardMinDcfclkCached / vbiossmcSetMinDeepSleepDcfclkCached 后接入。
void AMDRadeonX5000_AMDGFX9DCN314Display::updateDisplayClocks(AMDRadeonX5000_AMDGFX9DCN314Display* const self,
                                                              const struct dcn314_display_clock_req* const req)
{
    if (req == nullptr) { return; }

    // 1. dispclk / dppclk 下发 (cached wrapper 内含 FW 加载守卫，未加载时透传原值不报错)
    const SInt32 dispclk = X5000HWLibs::vbiossmcSetDispclkCached(req->dispclk_khz);
    const SInt32 dppclk  = X5000HWLibs::vbiossmcSetDppclkCached(req->dppclk_khz);
    if (dispclk < 0) {
        SYSLOG("GFX9DCN314Display", "updateDisplayClocks: dispclk set failed (req %u kHz)", req->dispclk_khz);
    }
    if (dppclk < 0) {
        SYSLOG("GFX9DCN314Display", "updateDisplayClocks: dppclk set failed (req %u kHz)", req->dppclk_khz);
    }

    DBGLOG("GFX9DCN314Display", "updateDisplayClocks: dispclk %d / dppclk %d kHz",
           dispclk, dppclk);
}

// dcn314 的 DCN3 翻转选项（基类硬编码 DCN2，已审查确认）
AMDFlipOption AMDRadeonX5000_AMDGFX9DCN314Display::getFlipOption(AMDRadeonX5000_AMDHWDisplay*)
{ return AMDFlipOption::DCN3; }

// =============================================================================
// update_odm / resync_fifo 寄存器级直译
//   设计: research/updateodm-resync-directreg-design.md (Verifier 4/4 PASS)
//   所有 mask/shift/偏移均来自已审查文档与 DCN314.hpp，未推测。
// =============================================================================

// 寄存器位操作 helper（展平 REG_SET / REG_UPDATE / REG_GET）
static inline void dcn314RegSet(AMDRadeonX5000_AMDHWRegisters& regs, UInt32 addr,
                                UInt32 mask, UInt32 shift, UInt32 val)
{
    UInt32 v = regs.read(addr);
    v = (v & ~mask) | ((val << shift) & mask);
    regs.write(addr, v);
}

// DCN314.hpp sh_mask 未含的位域常量（值取自设计文档，未推测）
static constexpr UInt32 OPTC_SEGMENT_WIDTH_MASK               = 0x00001FFFUL;
static constexpr UInt32 MPC_OUT_RATE_CONTROL_DISABLE_MASK     = 0x00000100UL;
static constexpr UInt32 MPC_OUT_RATE_CONTROL_DISABLE_SHIFT    = 8;
static constexpr UInt32 MPC_OUT_RATE_CONTROL_MASK             = 0x00000200UL;
static constexpr UInt32 MPC_OUT_RATE_CONTROL_SHIFT            = 9;

// compute_odm_memory_mask: 直译 dcn314_optc.c:60-70
//   h_active = slice_width * opp_cnt；每 2048 像素一个内存实例
static UInt32 compute_odm_memory_mask(const UInt32 opp_inst[], UInt32 opp_cnt, UInt32 slice_width)
{
    const UInt32 h_active      = slice_width * opp_cnt;
    const UInt32 odm_mem_count = (h_active + 2047) / 2048;

    if (opp_cnt == 4) {
        if (odm_mem_count <= 1) { return 0x3; }
        if (odm_mem_count <= 2) { return 0xf; }
        return 0x3f;
    }
    // opp_cnt == 2：每 OPP 占 2 个内存实例（起始 = opp_id*2）
    UInt32 mask = 0;
    for (UInt32 i = 0; i < opp_cnt; ++i) {
        const UInt32 base = opp_inst[i] * 2;
        for (UInt32 m = 0; m < odm_mem_count; ++m) { mask |= (1u << (base + m)); }
    }
    return mask;
}

void AMDRadeonX5000_AMDGFX9DCN314Display::update_odm_direct(AMDRadeonX5000_AMDHWRegisters& regs,
                                                            UInt32                          otg_inst,
                                                            const UInt32                    opp_inst[],
                                                            UInt32                          opp_cnt,
                                                            UInt32                          slice_width)
{
    const UInt32 dss  = DCN_BASE_2 + OPTC_DATA_SOURCE_SELECT + ODM_REG_STRIDE * otg_inst; // DATA_SOURCE_SELECT
    const UInt32 wctl = DCN_BASE_2 + OPTC_WIDTH_CONTROL      + ODM_REG_STRIDE * otg_inst; // WIDTH_CONTROL
    const UInt32 mcfg = DCN_BASE_2 + OPTC_MEMORY_CONFIG      + ODM_REG_STRIDE * otg_inst; // MEMORY_CONFIG
    const UInt32 htc  = DCN_BASE_2 + OTG_H_TIMING_CNTL       + OTG_REG_STRIDE * otg_inst; // OTG_H_TIMING_CNTL

    if (opp_cnt <= 1) {
        // ---- set_odm_bypass 直译 ----
        dcn314RegSet(regs, dss, OPTC_NUM_OF_INPUT_SEGMENT_MASK, OPTC_NUM_OF_INPUT_SEGMENT_SHIFT, 0);
        dcn314RegSet(regs, dss, OPTC_SEG0_SRC_SEL_MASK, OPTC_SEG0_SRC_SEL_SHIFT, otg_inst);
        dcn314RegSet(regs, dss, OPTC_SEG1_SRC_SEL_MASK, OPTC_SEG1_SRC_SEL_SHIFT, 0xF);
        dcn314RegSet(regs, dss, OPTC_SEG2_SRC_SEL_MASK, OPTC_SEG2_SRC_SEL_SHIFT, 0xF);
        dcn314RegSet(regs, dss, OPTC_SEG3_SRC_SEL_MASK, OPTC_SEG3_SRC_SEL_SHIFT, 0xF);
        dcn314RegSet(regs, htc, OTG_H_TIMING_DIV_MODE_MASK, OTG_H_TIMING_DIV_MODE_SHIFT, 0); // bypass: NO_DIV
        dcn314RegSet(regs, mcfg, OPTC_MEMORY_CONFIG_OPTC_MEM_SEL_MASK, OPTC_MEMORY_CONFIG_OPTC_MEM_SEL_SHIFT, 0);
    } else {
        // ---- set_odm_combine 直译 ----
        const UInt32 memory_mask = compute_odm_memory_mask(opp_inst, opp_cnt, slice_width);
        dcn314RegSet(regs, mcfg, OPTC_MEMORY_CONFIG_OPTC_MEM_SEL_MASK, OPTC_MEMORY_CONFIG_OPTC_MEM_SEL_SHIFT, memory_mask);
        dcn314RegSet(regs, dss, OPTC_NUM_OF_INPUT_SEGMENT_MASK, OPTC_NUM_OF_INPUT_SEGMENT_SHIFT,
                     (opp_cnt == 4) ? 3 : 1);
        dcn314RegSet(regs, dss, OPTC_SEG0_SRC_SEL_MASK, OPTC_SEG0_SRC_SEL_SHIFT, opp_inst[0]);
        dcn314RegSet(regs, dss, OPTC_SEG1_SRC_SEL_MASK, OPTC_SEG1_SRC_SEL_SHIFT, opp_inst[1]);
        if (opp_cnt == 4) {
            dcn314RegSet(regs, dss, OPTC_SEG2_SRC_SEL_MASK, OPTC_SEG2_SRC_SEL_SHIFT, opp_inst[2]);
            dcn314RegSet(regs, dss, OPTC_SEG3_SRC_SEL_MASK, OPTC_SEG3_SRC_SEL_SHIFT, opp_inst[3]);
        }
        dcn314RegSet(regs, wctl, OPTC_SEGMENT_WIDTH_MASK,
                     OPTC_WIDTH_CONTROL_OPTC_SEGMENT_WIDTH_SHIFT, slice_width);
        dcn314RegSet(regs, htc, OTG_H_TIMING_DIV_MODE_MASK, OTG_H_TIMING_DIV_MODE_SHIFT, opp_cnt - 1);
    }

    // ---- set_out_rate_control 直译 (DCN30+ 空操作：DISABLE=1 / RATE_CONTROL=0) ----
    for (UInt32 i = 0; i < opp_cnt; ++i) {
        const UInt32 mux = DCN_BASE_2 + MPC_OUT0_MUX + MPC_OUT_MUX_STRIDE * opp_inst[i];
        dcn314RegSet(regs, mux, MPC_OUT_RATE_CONTROL_DISABLE_MASK, MPC_OUT_RATE_CONTROL_DISABLE_SHIFT, 1);
        dcn314RegSet(regs, mux, MPC_OUT_RATE_CONTROL_MASK, MPC_OUT_RATE_CONTROL_SHIFT, 0);
    }
}

void AMDRadeonX5000_AMDGFX9DCN314Display::resync_fifo_dccg_dio_direct(AMDRadeonX5000_AMDHWRegisters& regs)
{
    const UInt32 addr = DCN_BASE_2 + DENTIST_DISPCLK_CNTL; // 0x34C0 + 0x64

    // REG_GET(DENTIST_DISPCLK_CNTL, DENTIST_DISPCLK_RDIVIDER, &v)
    const UInt32 rdiv = (regs.read(addr) & DENTIST_DISPCLK_RDIVIDER_MASK) >> DENTIST_DISPCLK_RDIVIDER_SHIFT;
    // REG_UPDATE(DENTIST_DISPCLK_CNTL, DENTIST_DISPCLK_WDIVIDER, rdiv)
    // 保护: RDIVIDER==0 时不写，避免 WDIVIDER=0 非法 (dcn32_dccg.c 风格)
    if (rdiv != 0) {
        dcn314RegSet(regs, addr, DENTIST_DISPCLK_WDIVIDER_MASK, DENTIST_DISPCLK_WDIVIDER_SHIFT, rdiv);
    }
}
