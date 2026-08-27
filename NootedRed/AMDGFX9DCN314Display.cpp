// DCN 314 Display implementation for Phoenix (Radeon 780M, RDNA3)
// Derivative of AMDRadeonX5000 and AMDRadeonX6000 decompilation
// Ported from Linux amdgpu dcn314 (register offsets verified identical to DCN2)
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#include <AMDGFX9DCN314Display.hpp>
#include <AMDGFX9DCNDisplay.hpp>
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

// dcn314 的 DCN3 翻转选项（基类硬编码 DCN2，已审查确认）
AMDFlipOption AMDRadeonX5000_AMDGFX9DCN314Display::getFlipOption(AMDRadeonX5000_AMDHWDisplay*)
{ return AMDFlipOption::DCN3; }
