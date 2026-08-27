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
