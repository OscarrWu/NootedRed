// AMDRadeonX5000 Patches
//
// Copyright © 2022-2025 ChefKiss. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#include <AMDGFX9DCN1Display.hpp>
#include <AMDGFX9DCN2Display.hpp>
#include <AMDGFX9DCN314Display.hpp>
#include <AMDGFX9DCNDisplay.hpp>
#include <GPUDriversAMD/Accel/HWDisplay.hpp>
#include <GPUDriversAMD/Accel/HWEngine.hpp>
#include <GPUDriversAMD/AddrLib.hpp>
#include <GPUDriversAMD/FB/Attributes.hpp>
#include <GPUDriversAMD/Family.hpp>
#include <Headers/kern_mach.hpp>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>
#include <Kexts.hpp>
#include <NRed.hpp>
#include <PenguinWizardry/KernelVersion.hpp>
#include <PenguinWizardry/PatcherPlus.hpp>
#include <X5000.hpp>
#include <kern/debug.h>    // panic()（第八步加速器 start 探针）
#include <libkern/OSTypes.h>
#include <libkern/c++/OSObject.h>
#include <mach/i386/vm_param.h>
#include <mach/i386/vm_types.h>
#include <mach/kern_return.h>

static const UInt8 kChannelTypesPattern[] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
                                             0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00};

static const UInt8 kHwlConvertChipFamilyPattern[] = {0x81, 0xFE, 0x8D, 0x00, 0x00, 0x00, 0x0F};

// Make for loop stop after one SDMA engine.
static const UInt8 kStartHWEnginesOriginal[] = {0x40, 0x83, 0xF0, 0x02};
static const UInt8 kStartHWEnginesMask[]     = {0xF0, 0xFF, 0xF0, 0xFF};
static const UInt8 kStartHWEnginesPatched[]  = {0x40, 0x83, 0xF0, 0x01};

// The check in `Addr::Lib::Create` on <=10.15 and 13.4+ is `familyId == 0x8D` instead of `familyId - 0x8D < 2`.
// Change the 0x8D (AI) to 0x8E (RV).
static const UInt8 kAddrLibCreateOriginal[] = {0x41, 0x81, 0x7D, 0x08, 0x8D, 0x00, 0x00, 0x00};
static const UInt8 kAddrLibCreatePatched[]  = {0x41, 0x81, 0x7D, 0x08, 0x8E, 0x00, 0x00, 0x00};

// For some reason Navi (`0x8F`) was added in 14.4 here. Lazy copy pasting?
static const UInt8 kAddrLibCreateOriginal1404[] = {0x41, 0x8B, 0x46, 0x08, 0x3D, 0x8F, 0x00, 0x00, 0x00, 0x74, 0x00,
                                                   0x3D, 0x8D, 0x00, 0x00, 0x00, 0x0F, 0x85, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kAddrLibCreateOriginalMask1404[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                       0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                       0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kAddrLibCreatePatched1404[]     = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                      0x00, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kAddrLibCreatePatchedMask1404[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                      0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Catalina only. Change loop condition to skip SDMA1_HP.
static const UInt8 kCreateAccelChannelsOriginal[] = {0x8D, 0x44, 0x09, 0x02};
static const UInt8 kCreateAccelChannelsPatched[]  = {0x8D, 0x44, 0x09, 0x01};

// Ditto, Mojave.
static const UInt8 kCreateAccelChannelsOriginal10_14[] = {0x8D, 0x04, 0x09, 0x8D, 0x4C, 0x09, 0x02};
static const UInt8 kCreateAccelChannelsPatched10_14[]  = {0x8D, 0x04, 0x09, 0x8D, 0x4C, 0x09, 0x01};

static X5000 moduleInstance;

X5000& X5000::singleton() { return moduleInstance; }

X5000::X5000()
{
    if (currentKernelVersion() <= MACOS_10_14_X) {
        this->pm4EngineField               = 0x330;
        this->sdma0EngineField             = 0x338;
        this->supportedDisplayCountField   = 0x2C;
        this->seCountField                 = 0x58;
        this->shCountField                 = 0x5C;
        this->hwMaxCUsField                = 0x80;
        this->hasUVD0Field                 = 0x90;
        this->hasVCEField                  = 0x92;
        this->hasVCN0Field                 = 0x93;
        this->hasSDMAPagingQueueField      = 0xA4;
        this->hasGetAllClockLimitsField    = 0xA3;
        this->familyTypeField              = 0x29C;
        this->chipSettingsField            = 0x5B18;
        this->hwChannelHWInterfaceField    = 0x18;
        this->hwChannelSubmitCommandBuffer = 0x180;
    }
    else if (currentKernelVersion().majorMatches(MACOS_10_15)) {
        this->pm4EngineField               = 0x348;
        this->sdma0EngineField             = 0x350;
        this->supportedDisplayCountField   = 0x2C;
        this->seCountField                 = 0x58;
        this->shCountField                 = 0x5C;
        this->hwMaxCUsField                = 0x80;
        this->hasUVD0Field                 = 0x90;
        this->hasVCEField                  = 0x92;
        this->hasVCN0Field                 = 0x93;
        this->hasSDMAPagingQueueField      = 0xA4;
        this->hasGetAllClockLimitsField    = 0xA3;
        this->dccDisplayableSupportField   = 0xA5;
        this->familyTypeField              = 0x2B4;
        this->chipSettingsField            = 0x5B18;
        this->hwChannelHWInterfaceField    = 0x18;
        this->hwChannelSubmitCommandBuffer = 0x188;
    }
    else {
        this->pm4EngineField    = 0x3B8;
        this->sdma0EngineField  = 0x3C0;
        this->familyTypeField   = 0x308;
        this->chipSettingsField = 0x5B10;

        if (currentKernelVersion() <= MACOS_12_X) {
            this->supportedDisplayCountField   = 0x2C;
            this->seCountField                 = 0x5C;
            this->shCountField                 = 0x64;
            this->hwMaxCUsField                = 0x98;
            this->hasUVD0Field                 = 0xAC;
            this->hasVCEField                  = 0xAE;
            this->hasVCN0Field                 = 0xAF;
            this->hasSDMAPagingQueueField      = 0xC0;
            this->hasGetAllClockLimitsField    = 0xBF;
            this->dccDisplayableSupportField   = 0xC1;
            this->hwChannelHWInterfaceField    = 0x18;
            this->hwChannelSubmitCommandBuffer = 0x190;
        }
        else {
            this->supportedDisplayCountField = 0x34;
            this->seCountField               = 0x64;
            this->shCountField               = 0x6C;
            this->hwMaxCUsField              = 0xA0;
            this->hasUVD0Field               = 0xB4;
            this->hasVCEField                = 0xB6;
            this->hasVCN0Field               = 0xB7;
            this->hasSDMAPagingQueueField    = 0xBF;
            this->hasGetAllClockLimitsField  = 0xBE;
            this->dccDisplayableSupportField = 0xC0;
            this->hwChannelHWInterfaceField  = 0x20;
            if (currentKernelVersion() >= MACOS_14) { this->hwChannelSubmitCommandBuffer = 0x188; }
            else {
                this->hwChannelSubmitCommandBuffer = 0x190;
            }
        }
    }
}

void X5000::processKext(KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide, const size_t size)
{
    if (kextRadeonX5000.loadIndex != id) { return; }

    DBGLOG("X5000", "processKext: X5000 matched, begin (id=%zu slide=0x%llX size=0x%zX)", id, slide, size);

    NRed::singleton().hwLateInit();

    UInt32*           orgChannelTypes;
    mach_vm_address_t orgStartHWEngines;
    void*             pm4ComputeChannelVT;

    PenguinWizardry::PatternSolveRequest solveRequests[] = {
        {currentKernelVersion() <= MACOS_10_15_X ?
             "__ZZN37AMDRadeonX5000_AMDGraphicsAccelerator22getAdditionalQueueListEPPK18_"
             "AMDQueueSpecifierE27additionalQueueList_Default" :
             "__ZZN37AMDRadeonX5000_AMDGraphicsAccelerator19createAccelChannelsEbE12channelTypes",
         orgChannelTypes, kChannelTypesPattern},
        {"__ZN31AMDRadeonX5000_AMDGFX9PM4Engine10gMetaClassE", this->pm4EngineMC},
        {"__ZN32AMDRadeonX5000_AMDGFX9SDMAEngine10gMetaClassE", this->sdmaEngineMC},
        {"__ZN26AMDRadeonX5000_AMDHardware14startHWEnginesEv", orgStartHWEngines},
        {"__ZN30AMDRadeonX5000_AMDGFX9Hardware32setupAndInitializeHWCapabilitiesEv",
         this->orgGFX9SetupAndInitializeHWCapabilities},
        {"__ZTV39AMDRadeonX5000_AMDGFX9PM4ComputeChannel", pm4ComputeChannelVT},
        {"__ZN30AMDRadeonX5000_AMDPM4HWChannel19submitCommandBufferEP30AMD_SUBMIT_COMMAND_BUFFER_INFO",
         this->orgPM4SubmitCommandBuffer},
    };
    PANIC_COND(!PenguinWizardry::PatternSolveRequest::solveAll(patcher, id, solveRequests, slide, size), "X5000",
               "Failed to resolve symbols");

    AMDRadeonX5000_AMDHWAlignManager::resolve(patcher, id, slide, size);
    AMDRadeonX5000_AMDHWDisplay::resolve(patcher, id, slide, size);
    AMDRadeonX5000_AMDGFX9DCNDisplay::registerMC(kextRadeonX5000.id, patcher, id, slide, size);
    AMDRadeonX5000_AMDGFX9DCN1Display::resolve(kextRadeonX5000.id);
    AMDRadeonX5000_AMDGFX9DCN2Display::resolve(kextRadeonX5000.id);
    AMDRadeonX5000_AMDGFX9DCN314Display::resolve(kextRadeonX5000.id);

    PenguinWizardry::PatternRouteRequest requests[] = {
        {"__ZN32AMDRadeonX5000_AMDVega10Hardware17allocateHWEnginesEv", allocateHWEngines},
        {"__ZN32AMDRadeonX5000_AMDVega10Hardware32setupAndInitializeHWCapabilitiesEv",
         wrapSetupAndInitializeHWCapabilities},
        {"__ZN26AMDRadeonX5000_AMDHardware12getHWChannelE20_eAMD_HW_ENGINE_TYPE18_eAMD_HW_RING_TYPE", wrapGetHWChannel,
         this->orgGetHWChannel},
        {"__ZN30AMDRadeonX5000_AMDGFX9Hardware20initializeFamilyTypeEv", initializeFamilyType},
        {"__ZN30AMDRadeonX5000_AMDGFX9Hardware20allocateAMDHWDisplayEv", allocateAMDHWDisplay},
        {"__ZN26AMDRadeonX5000_AMDHWMemory17adjustVRAMAddressEy", wrapAdjustVRAMAddress, this->orgAdjustVRAMAddress},
        {"__ZN30AMDRadeonX5000_AMDGFX9Hardware20writeASICHangLogInfoEPPv", returnZero},
        {"__ZN4Addr2V27Gfx9Lib20HwlConvertChipFamilyEjj", wrapHwlConvertChipFamily, this->orgHwlConvertChipFamily,
         kHwlConvertChipFamilyPattern},
        {"__ZN27AMDRadeonX5000_AMDHWDisplay14getDisplayInfoEjbbPvP17_FRAMEBUFFER_INFO", fixedGetDisplayInfo},
        {"__ZN33AMDRadeonX5000_AMDHWAlignManager214getSurfaceInfoEP24_AMD_SURFACE_INFO_STRUCT", fixedGetSurfaceInfo},
    };
    PANIC_COND(!PenguinWizardry::PatternRouteRequest::routeAll(patcher, id, requests, slide, size), "X5000",
               "Failed to route symbols");

    // 第八步观测（第 7 批次）：hook 加速器的 `start`（**出口**），读注册链的前置字段。
    //  依据（2026-09-26 离线取证）：`AMDGraphicsAccelerator::start`（VM 0x1290）中，只有
    //  `this+0x1f40`（= framebuffer 服务，由 `configureDevice` 按 "ATIFramebuffer" /
    //  "IOFramebuffer" 查得）非空时，才会用 `OSSymbol("SpecialAMDKey")`（`this+0x1f48`）向
    //  controller 发 selector = 0 的注册调用（`call *(controller)->vtable[0x6b8]`）；
    //  为 0 则整段跳过 ⇒ `controller+0x7960` 永远为空。
    //  仅 `-NRedAccelProbe`（panic 通道）或 `-NRedAccelLog`（日志通道，零挂死风险）时安装；
    //  默认零影响。
    if (checkKernelArgument("-NRedAccelProbe") || checkKernelArgument("-NRedAccelLog")) {
        PenguinWizardry::PatternRouteRequest accelStartReq{
            "__ZN37AMDRadeonX5000_AMDGraphicsAccelerator5startEP9IOService", wrapAccelStart, this->orgAccelStart};
        if (!accelStartReq.route(patcher, id, slide, size)) {
            SYSLOG("X5000", "accel-probe: failed to route AMDGraphicsAccelerator::start");
        } else {
            DBGLOG("X5000", "accel-probe: routed AMDGraphicsAccelerator::start");
        }
    }

    // 第八步观测（第 13 轮）：hook 加速器的 `probe`（**纯观测**：不改返回值、不改 score）。
    //  目的：判定"加速器类为何零实例"——probe 若从未被调用 ⇒ personality 未参与匹配（上游问题）；
    //        若被调用而返回 0 ⇒ 其前置判据不满足（离线已排除 `-amd_no_dgpu_accel` 与 `IOPCITunnelled`）。
    //  入口/出口各记录一次，panic 前把 `score` 的进出值一并带出。门控 `-NRedAccelProbe2`（默认关闭）。
    if (checkKernelArgument("-NRedAccelProbe2")) {
        PenguinWizardry::PatternRouteRequest accelProbeReq{
            "__ZN37AMDRadeonX5000_AMDGraphicsAccelerator5probeEP9IOServicePi", wrapAccelProbe, this->orgAccelProbe};
        if (!accelProbeReq.route(patcher, id, slide, size)) {
            SYSLOG("X5000", "accel-probe2: failed to route AMDGraphicsAccelerator::probe");
        } else {
            DBGLOG("X5000", "accel-probe2: routed AMDGraphicsAccelerator::probe");
        }
    }

    if (currentKernelVersion() >= MACOS_11) {
        PenguinWizardry::PatternSolveRequest solveRequest{"__ZN30AMDRadeonX5000_AMDGFX9Hardware15notifyGfxAccessEv",
                                                          this->notifyGfxAccess};
        PANIC_COND(!solveRequest.solve(patcher, id, slide, size), "X5000", "Failed to resolve notifyGfxAccess");
        PANIC_COND(MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS, "X5000",
                   "Failed to enable kernel writing");
        this->orgPM4SubmitCommandBuffer = this->hwChannelSubmitCommandBuffer(pm4ComputeChannelVT);
        this->hwChannelSubmitCommandBuffer(pm4ComputeChannelVT) =
            reinterpret_cast<mach_vm_address_t>(computeSubmitCommandBuffer);
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
    }

    if (currentKernelVersion() >= MACOS_13_4) {
        PenguinWizardry::PatternRouteRequest request{
            "__ZN37AMDRadeonX5000_AMDGraphicsAccelerator23obtainAccelChannelGroupE11SS_"
            "PRIORITYP27AMDRadeonX5000_AMDAccelTask",
            wrapObtainAccelChannelGroup1304, this->orgObtainAccelChannelGroup};
        PANIC_COND(!request.route(patcher, id, slide, size), "X5000", "Failed to route obtainAccelChannelGroup");
    }
    else if (currentKernelVersion() >= MACOS_11) {
        PenguinWizardry::PatternRouteRequest request{
            "__ZN37AMDRadeonX5000_AMDGraphicsAccelerator23obtainAccelChannelGroupE11SS_PRIORITY",
            wrapObtainAccelChannelGroup, this->orgObtainAccelChannelGroup};
        PANIC_COND(!request.route(patcher, id, slide, size), "X5000", "Failed to route obtainAccelChannelGroup");
    }

    if (currentKernelVersion() >= MACOS_14_4) {
        const PenguinWizardry::MaskedLookupPatch patch{
            &kextRadeonX5000,          kAddrLibCreateOriginal1404,    kAddrLibCreateOriginalMask1404,
            kAddrLibCreatePatched1404, kAddrLibCreatePatchedMask1404, 1};
        PANIC_COND(!patch.apply(patcher, slide, size), "X5000", "Failed to apply 14.4+ Addr::Lib::Create patch");
    }
    else if (currentKernelVersion() <= MACOS_10_15_X || currentKernelVersion() >= MACOS_13_4) {
        const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX5000, kAddrLibCreateOriginal, kAddrLibCreatePatched,
                                                       1};
        PANIC_COND(!patch.apply(patcher, slide, size), "X5000", "Failed to apply Addr::Lib::Create patch");
    }

    if (currentKernelVersion() <= MACOS_10_15_X) {
        if (currentKernelVersion().majorMatches(MACOS_10_15)) {
            const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX5000, kCreateAccelChannelsOriginal,
                                                           kCreateAccelChannelsPatched, 2};
            PANIC_COND(!patch.apply(patcher, slide, size), "X5000", "Failed to patch createAccelChannels");
        }
        else {
            const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX5000, kCreateAccelChannelsOriginal10_14,
                                                           kCreateAccelChannelsPatched10_14, 1};
            PANIC_COND(!patch.apply(patcher, slide, size), "X5000", "Failed to patch createAccelChannels");
        }

        // TODO: wait, what is this doing again?
        // 豁免 Phoenix 守卫：本块仅在 macOS 10.15 及以下才执行，目标系统 13.6 不执行，
        // 故 isRenoir() 分支无需 isPhoenix() 排除（路线图附录 A(8) 筛查豁免项）。
        if (NRed::singleton().getAttributes().isRenoir()) {
            UInt32                                   findNonBpp64 = Dcn1NonBpp64SwModeMask1015;
            UInt32                                   replNonBpp64 = Dcn2NonBpp64SwModeMask1015;
            UInt32                                   findBpp64 = Dcn1NonBpp64SwModeMask1015 ^ Dcn1Bpp64SwModeMask1015;
            UInt32                                   replBpp64 = Dcn2NonBpp64SwModeMask1015 ^ Dcn2Bpp64SwModeMask1015;
            UInt32                                   findBpp64Pt2 = Dcn1Bpp64SwModeMask1015;
            UInt32                                   replBpp64Pt2 = Dcn2Bpp64SwModeMask1015;
            const PenguinWizardry::MaskedLookupPatch patches[]    = {
                {&kextRadeonX5000, reinterpret_cast<const UInt8*>(&findNonBpp64),
                 reinterpret_cast<const UInt8*>(&replNonBpp64), sizeof(UInt32), 2},
                {&kextRadeonX5000, reinterpret_cast<const UInt8*>(&findBpp64),
                 reinterpret_cast<const UInt8*>(&replBpp64), sizeof(UInt32), 1},
                {&kextRadeonX5000, reinterpret_cast<const UInt8*>(&findBpp64Pt2),
                 reinterpret_cast<const UInt8*>(&replBpp64Pt2), sizeof(UInt32), 1},
            };
            PANIC_COND(!PenguinWizardry::MaskedLookupPatch::applyAll(patcher, patches, slide, size), "X5000",
                       "Failed to patch swizzle mode");
        }

        PANIC_COND(MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS, "X5000",
                   "Failed to enable kernel writing");
        *orgChannelTypes = 1;    // Make VMPT use SDMA0 instead of SDMA1
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        DBGLOG("X5000", "Applied SDMA1 patches");
    }
    else {
        const PenguinWizardry::MaskedLookupPatch patch{
            &kextRadeonX5000,       kStartHWEnginesOriginal, kStartHWEnginesMask,
            kStartHWEnginesPatched, kStartHWEnginesMask,     currentKernelVersion() >= MACOS_13 ? 2U : 1};
        PANIC_COND(!patch.apply(patcher, orgStartHWEngines, PAGE_SIZE), "X5000", "Failed to patch startHWEngines");

        if (NRed::singleton().getAttributes().isRenoir() && !NRed::singleton().getAttributes().isPhoenix()) {
            UInt32 findBpp64 = Dcn1Bpp64SwModeMask, replBpp64 = Dcn2Bpp64SwModeMask;
            UInt32 findNonBpp64 = Dcn1NonBpp64SwModeMask, replNonBpp64 = Dcn2NonBpp64SwModeMask;
            const PenguinWizardry::MaskedLookupPatch patches[] = {
                {&kextRadeonX5000, reinterpret_cast<const UInt8*>(&findBpp64),
                 reinterpret_cast<const UInt8*>(&replBpp64), sizeof(UInt32),
                 currentKernelVersion() >= MACOS_13_4 ? 2U : 4},
                {&kextRadeonX5000, reinterpret_cast<const UInt8*>(&findNonBpp64),
                 reinterpret_cast<const UInt8*>(&replNonBpp64), sizeof(UInt32),
                 currentKernelVersion() >= MACOS_13_4 ? 2U : 4},
            };
            PANIC_COND(!PenguinWizardry::MaskedLookupPatch::applyAll(patcher, patches, slide, size), "X5000",
                       "Failed to patch swizzle mode");
        }

        PANIC_COND(MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS, "X5000",
                   "Failed to enable kernel writing");
        // createAccelChannels: stop at SDMA0
        orgChannelTypes[5] = 1;
        // getPagingChannel: get only SDMA0
        orgChannelTypes[currentKernelVersion() >= MACOS_12 ? 12 : 11] = 0;
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        DBGLOG("X5000", "Applied SDMA1 patches");
    }
}

bool X5000::allocateHWEngines(void* const self)
{
    [[clang::suppress]] singleton().pm4EngineField(self)   = singleton().pm4EngineMC->alloc();
    [[clang::suppress]] singleton().sdma0EngineField(self) = singleton().sdmaEngineMC->alloc();

    // No VCN? :-(

    return true;
}

// TODO: Replace with IP Discovery?
void X5000::wrapSetupAndInitializeHWCapabilities(void* const self)
{
    auto& seCount  = singleton().seCountField(self);
    auto& shCount  = singleton().shCountField(self);
    auto& hwMaxCUs = singleton().hwMaxCUsField(self);

    seCount = 1;
    shCount = 1;
    if (NRed::singleton().getAttributes().isPhoenix()) { hwMaxCUs = 12; }
    else if (NRed::singleton().getAttributes().isRenoir()) { hwMaxCUs = 8; }
    else if (NRed::singleton().getAttributes().isRaven2()) {
        hwMaxCUs = 3;
    }
    else {
        hwMaxCUs = 11;
    }

    FunctionCast(wrapSetupAndInitializeHWCapabilities, singleton().orgGFX9SetupAndInitializeHWCapabilities)(self);

    singleton().supportedDisplayCountField(self) = 4;
    singleton().hasUVD0Field(self)               = false;
    singleton().hasVCEField(self)                = false;
    singleton().hasVCN0Field(self)               = false;    // TODO
    singleton().hasSDMAPagingQueueField(self)    = false;
    singleton().hasGetAllClockLimitsField(self)  = false;
    if (currentKernelVersion() >= MACOS_10_15) { singleton().dccDisplayableSupportField(self) = true; }
}

// TODO: Investigate why this is needed.
void* X5000::wrapGetHWChannel(void* const self, AMDHWEngineType engineType, const UInt32 ringId)
{
    if (engineType == kAMDHWEngineTypeSDMA1) { engineType = kAMDHWEngineTypeSDMA0; }
    return FunctionCast(wrapGetHWChannel, singleton().orgGetHWChannel)(self, engineType, ringId);
}

void X5000::initializeFamilyType(void* const self) { singleton().familyTypeField(self) = AMD_FAMILY_RAVEN; }

void* X5000::allocateAMDHWDisplay(void* const)
{
    const auto& attrs = NRed::singleton().getAttributes();
    DBGLOG("X5000", "allocateAMDHWDisplay: isPhoenix=%s isRenoir=%s",
           attrs.isPhoenix() ? "true" : "false", attrs.isRenoir() ? "true" : "false");
    if (attrs.isPhoenix()) {
        DBGLOG("X5000", "allocateAMDHWDisplay: returning DCN314Display");
        return AMDRadeonX5000_AMDGFX9DCN314Display::gRTMetaClass.alloc();
    }
    if (attrs.isRenoir()) {
        return AMDRadeonX5000_AMDGFX9DCN2Display::gRTMetaClass.alloc();
    }
    return AMDRadeonX5000_AMDGFX9DCN1Display::gRTMetaClass.alloc();
}

UInt64 X5000::wrapAdjustVRAMAddress(void* const self, const UInt64 addr)
{
    auto ret = FunctionCast(wrapAdjustVRAMAddress, singleton().orgAdjustVRAMAddress)(self, addr);
    if (ret != addr) { return ret + NRed::singleton().getFbOffset(); }
    return ret;
}

UInt32 X5000::returnZero() { return 0; }

// Replaces SDMA1 field with SDMA0 because we don't have SDMA1
// TODO: Investigate why this is needed.
static void* fixAccelGroup(void* const group)
{
    if (group != nullptr) { getMember<void*>(group, 0x18) = getMember<void*>(group, 0x10); }
    return group;
}

void* X5000::wrapObtainAccelChannelGroup(void* const self, const UInt32 priority)
{
    return fixAccelGroup(
        FunctionCast(wrapObtainAccelChannelGroup, singleton().orgObtainAccelChannelGroup)(self, priority));
}

void* X5000::wrapObtainAccelChannelGroup1304(void* const self, const UInt32 priority, void* const task)
{
    return fixAccelGroup(
        FunctionCast(wrapObtainAccelChannelGroup1304, singleton().orgObtainAccelChannelGroup)(self, priority, task));
}

UInt32 X5000::wrapHwlConvertChipFamily(void* const self, const UInt32 family, const UInt32 revision)
{
    DBGLOG("X5000", "HwlConvertChipFamily >> (self: %p family: 0x%X revision: 0x%X)", self, family, revision);
    if (family == AMD_FAMILY_RAVEN) {
        auto& settings          = singleton().chipSettingsField(self);
        settings.isArcticIsland = 1;
        settings.isRaven        = 1;
        if (NRed::singleton().getAttributes().isRenoir() && !NRed::singleton().getAttributes().isPhoenix()) {
            settings.htileAlignFix = 1;
            settings.applyAliasFix = 1;
        }
        else if (!NRed::singleton().getAttributes().isRaven2()) {
            settings.depthPipeXorDisable = 1;
        }
        settings.isDcn1           = 1;    // what to do about this?
        settings.metaBaseAlignFix = 1;
        return ADDR_CHIP_FAMILY_AI;
    }
    return FunctionCast(wrapHwlConvertChipFamily, singleton().orgHwlConvertChipFamily)(self, family, revision);
}

UInt32 X5000::computeSubmitCommandBuffer(void* const self, void* const info)
{
    singleton().notifyGfxAccess(singleton().hwChannelHWInterfaceField(self));
    return FunctionCast(computeSubmitCommandBuffer, singleton().orgPM4SubmitCommandBuffer)(self, info);
}

//  -- VRR was introduced on macOS Big Sur. --

namespace
{

    class Constants
    {
        static void _setVrrTimestampInfoVentura(AMDRadeonX5000_AMDHWDisplay* const self, const UInt64 vTotalMin,
                                                const UInt64 vTotalMax, const UInt64 horizontalLineTime)
        {
            auto& vrrTimestampInfo                      = self->vrrTimestampInfoVentura();
            vrrTimestampInfo.lastTransactionTimestamp   = 0;
            vrrTimestampInfo.currentFrameStartTimestamp = 0;
            vrrTimestampInfo.lastTransactionStartTime   = 0;
            vrrTimestampInfo.currentFrameVTotal         = vTotalMin;
            vrrTimestampInfo.horizontalLineTime         = horizontalLineTime;
            vrrTimestampInfo.currentFrameTime           = 0;
            vrrTimestampInfo.vTotalMin                  = vTotalMin;
            vrrTimestampInfo.vTotalMax                  = vTotalMax;
            vrrTimestampInfo.transactionOnGlassTime     = 0;
        }

        static void _setVrrTimestampInfo(AMDRadeonX5000_AMDHWDisplay* const self, const UInt64 vTotalMin,
                                         const UInt64 vTotalMax, const UInt64 horizontalLineTime)
        {
            auto& vrrTimestampInfo                      = self->vrrTimestampInfo();
            vrrTimestampInfo.field0                     = 0;
            vrrTimestampInfo.lastTransactionTimestamp   = 0;
            vrrTimestampInfo.currentFrameStartTimestamp = 0;
            vrrTimestampInfo.lastTransactionStartTime   = 0;
            vrrTimestampInfo.currentFrameVTotal         = static_cast<UInt32>(vTotalMin);
            vrrTimestampInfo.horizontalLineTime         = static_cast<UInt32>(horizontalLineTime);
            vrrTimestampInfo.currentFrameTime           = 0;
            vrrTimestampInfo.vTotalMin                  = static_cast<UInt32>(vTotalMin);
            vrrTimestampInfo.vTotalMax                  = static_cast<UInt32>(vTotalMax);
            vrrTimestampInfo.transactionOnGlassTime     = 0;
        }

        static void _calcAndSetVrrTimestampInfo(AMDRadeonX5000_AMDHWDisplay* const self,
                                                const FramebufferInfo* const       fbInfo,
                                                const IOTimingInformation&         timingInfo)
        {
            assert(currentKernelVersion() >= MACOS_11);

            if (!fbInfo->isOnline) { return; }

            const auto vTotalMin =
                timingInfo.detailedInfo.v2.verticalBlanking + timingInfo.detailedInfo.v2.verticalActive;
            const auto vTotalMax  = vTotalMin + timingInfo.detailedInfo.v2.verticalBlankingExtension;
            const auto pixelClock = timingInfo.detailedInfo.v2.pixelClock;
            if (pixelClock == 0) { singleton.setVrrTimestampInfo(self, vTotalMin, vTotalMax, 0); }
            else {
                const auto hTotal =
                    timingInfo.detailedInfo.v2.horizontalBlanking + timingInfo.detailedInfo.v2.horizontalActive;
                const auto hLineTimeNs = static_cast<UInt64>(hTotal) * 1000000000ULL / pixelClock;
                UInt64     horizontalLineTime;
                nanoseconds_to_absolutetime(hLineTimeNs, &horizontalLineTime);
                singleton.setVrrTimestampInfo(self, vTotalMin, vTotalMax, horizontalLineTime);
            }
        }

        static void _calcAndSetVrrTimestampInfoDummy(AMDRadeonX5000_AMDHWDisplay*, const FramebufferInfo*,
                                                     const IOTimingInformation&)
        { assert(currentKernelVersion() <= MACOS_10_15_X); }

    public:
        static const Constants singleton;

        void (*setVrrTimestampInfo)(AMDRadeonX5000_AMDHWDisplay* self, UInt64 vTotalMin, UInt64 vTotalMax,
                                    const UInt64 horizontalLineTime){nullptr};
        void (*calcAndSetVrrTimestampInfo)(AMDRadeonX5000_AMDHWDisplay* self, const FramebufferInfo* fbInfo,
                                           const IOTimingInformation& timingInfo){_calcAndSetVrrTimestampInfoDummy};

        explicit Constants()
        {
            if (currentKernelVersion() < MACOS_11) { return; }

            this->calcAndSetVrrTimestampInfo = _calcAndSetVrrTimestampInfo;
            this->setVrrTimestampInfo =
                currentKernelVersion() >= MACOS_13 ? _setVrrTimestampInfoVentura : _setVrrTimestampInfo;
        }
    };

    const Constants Constants::singleton;

    constexpr inline auto& vrrConstants = Constants::singleton;

}    // namespace

bool X5000::fixedGetDisplayInfo(AMDRadeonX5000_AMDHWDisplay* const self, const UInt32 fbIndex, const bool isCRTEnabled,
                                const bool ignoreCRTOffsetCheck, IOFramebuffer* const fb, FramebufferInfo* const fbInfo)
{
    if (fb == nullptr || fbIndex >= self->supportedDisplayCount()) { return false; }

    fbInfo->crtOffset   = 0;
    fbInfo->size        = 0;
    fbInfo->crtSize     = 0;
    fbInfo->pitch       = 0;
    fbInfo->rect.width  = 0;
    fbInfo->rect.height = 0;

    auto& displayState = self->displayStates()[fbIndex];

    displayState.framebuffer = fb;
    displayState.status.setIsEnabled(isCRTEnabled);
    displayState.status.setIsIOFBFlipEnabled(true);
    displayState.status.setIsAccelBacked(false);

    uintptr_t wsaa = -1ULL;
    if (fb->getAttribute(ATTRIBUTE_WSAA, &wsaa) == kIOReturnSuccess) {
        self->wsaaAttributes()[fbIndex] = static_cast<UInt32>(wsaa);
        displayState.status.setIsWSAASupported(true);
    }
    else {
        displayState.status.setIsWSAASupported(false);
    }

    uintptr_t  dpt    = 0;
    const auto dptRet = fb->getAttribute(ATTRIBUTE_DISPLAY_PIPE_TRANSACTION, &dpt);
    displayState.status.setIsDPTSupported(dptRet == kIOReturnSuccess || dptRet == kIOReturnNotReady);

    const auto crtOffset = OSDynamicCast(OSData, fb->getProperty("ATY,fb_offset"));
    if (crtOffset != nullptr) {
        const auto data = crtOffset->getBytesNoCopy();
        if (data != nullptr) { fbInfo->crtOffset = *static_cast<const UInt64*>(data); }
    }

    const auto crtSize = OSDynamicCast(OSData, fb->getProperty("ATY,fb_size"));
    if (crtSize != nullptr) {
        const auto data = crtSize->getBytesNoCopy();
        if (data != nullptr) { fbInfo->crtSize = *static_cast<const UInt32*>(data); }
    }

    auto aperture = fb->getApertureRange(kIOFBSystemAperture);
    auto vram     = fb->getVRAMRange();

    fbInfo->isMapped = aperture != nullptr && vram != nullptr;

    // §简化项 15：捕获 Apple 自己认定的 **VRAM 基址**，供 NRed 计算 fbOffset 用。
    // 为什么在这里：这是 NRed 已有的钩子里最早能拿到 `IOFramebuffer` 的地方，而 `getVRAMRange()`
    //   给出的正是"VRAM 的地址范围"——与消费方（Apple 的 wrapAdjustVRAMAddress 换算）天然同源。
    //   替代了此前两版被证伪的做法（读 BAR0 / 读 MMHUB 寄存器，理由见 NRed.cpp 的 hwLateInit）。
    if (vram != nullptr) {
        const auto vramBase = static_cast<UInt64>(vram->getPhysicalSegment(0, nullptr));
        if (vramBase != 0) { NRed::singleton().setFbLocationBase(vramBase); }
    }

    [[clang::suppress]] OSSafeReleaseNULL(aperture);
    [[clang::suppress]] OSSafeReleaseNULL(vram);

    self->getDisplayModeViewportSpecificInfo(fbIndex, &self->viewportStartYs()[fbIndex],
                                             &self->viewportHeights()[fbIndex]);

    uintptr_t isOnline = 0;
    fbInfo->isOnline =
        fb->getAttributeForConnection(static_cast<IOIndex>(fbIndex), kConnectionEnable, &isOnline) == kIOReturnSuccess
        && isOnline != 0;

    self->fedsParamInfo()[fbIndex].crtIndex = 0;
    self->fedsParamInfo()[fbIndex].scaledW  = 0;
    self->fedsParamInfo()[fbIndex].scaledH  = 0;
    self->fedsParamInfo()[fbIndex].srcW     = 0;
    self->fedsParamInfo()[fbIndex].srcH     = 0;

    self->scalerFlags()[fbIndex] = 0;

    IODisplayModeID displayMode = 0;
    IOIndex         depth       = 0;
    if (fb->getCurrentDisplayMode(&displayMode, &depth) == kIOReturnSuccess) {
        if (fb->getPixelInformation(displayMode, depth, kIOFBSystemAperture, &displayState.pixelInfo)
            == kIOReturnSuccess)
        {
            const auto bytesPerPixel = displayState.pixelInfo.bitsPerPixel / 8;
            if (bytesPerPixel != 0) { fbInfo->pitch = displayState.pixelInfo.bytesPerRow / bytesPerPixel; }
            fbInfo->rect.width  = displayState.pixelInfo.activeWidth;
            fbInfo->rect.height = displayState.pixelInfo.activeHeight;
        }

        IODisplayModeInformation modeInfo;
        memset(&modeInfo, 0, sizeof(modeInfo));
        if (fb->getInformationForDisplayMode(displayMode, &modeInfo) == kIOReturnSuccess
            && (modeInfo.flags & kDisplayModeAcceleratorBackedFlag) != 0)
        {
            displayState.status.setIsAccelBacked(true);
            self->fedsParamInfo()[fbIndex].crtIndex = 1;
        }

        IOTimingInformation timingInfo;
        memset(&timingInfo, 0, sizeof(timingInfo));
        timingInfo.flags = kIODetailedTimingValid;
        if (fb->getTimingInfoForDisplayMode(displayMode, &timingInfo) == kIOReturnSuccess
            && (timingInfo.flags & kIODetailedTimingValid) != 0)
        {
            self->scalerFlags()[fbIndex] = timingInfo.detailedInfo.v2.scalerFlags;

            if (self->fedsParamInfo()[fbIndex].crtIndex == 1) {
                self->fedsParamInfo()[fbIndex].scaledW = timingInfo.detailedInfo.v2.horizontalActive;
                self->fedsParamInfo()[fbIndex].scaledH = timingInfo.detailedInfo.v2.verticalActive;
                self->fedsParamInfo()[fbIndex].srcW    = timingInfo.detailedInfo.v2.horizontalScaled;
                self->fedsParamInfo()[fbIndex].srcH    = timingInfo.detailedInfo.v2.verticalScaled;
            }

            vrrConstants.calcAndSetVrrTimestampInfo(self, fbInfo, timingInfo);
        }
    }

    auto& hwSpecificInfo = self->crtHWSpecificInfos()[fbIndex];

    auto ret = true;

    if (!isCRTEnabled
        || (!ignoreCRTOffsetCheck && fbInfo->crtOffset >= self->getHWInterface()->getHWMemory()->getVisibleSize()))
    {
        fbInfo->crtOffset = 0;
        fbInfo->size      = 0;
    }
    else {
        CRTHWDepth hwDepth;
        switch (displayState.pixelInfo.bitsPerPixel) {
            case 8: {
                hwDepth = CRTHWDepth::DEPTH_8;
            } break;
            case 16: {
                hwDepth = CRTHWDepth::DEPTH_16;
            } break;
            case 32: {
                hwDepth = CRTHWDepth::DEPTH_32;
            } break;
            case 64: {
                hwDepth = CRTHWDepth::DEPTH_64;
            } break;
            default: {
                hwDepth = CRTHWDepth::DEPTH_32;
                ret     = false;
            } break;
        }
        DBGLOG("X5000", "%s hwDepth=%s for bpp %d", __func__, stringifyCRTHWDepth(hwDepth),
               displayState.pixelInfo.bitsPerPixel);
        hwSpecificInfo.graphDepth = hwDepth;

        CRTHWFormat hwFormat;    // bug fix - original code only handled depth 64 and format 10
        switch (displayState.pixelInfo.bitsPerComponent) {
            case 8: {
                hwFormat = CRTHWFormat::FORMAT_8;
            } break;
            case 10: {
                hwFormat = CRTHWFormat::FORMAT_10;
            } break;
            case 12: {
                hwFormat = CRTHWFormat::FORMAT_12;
            } break;
            default: {
                hwFormat = CRTHWFormat::FORMAT_8;
                ret      = false;
            } break;
        }
        DBGLOG("X5000", "%s hwFormat=%s for bpc %d", __func__, stringifyCRTHWFormat(hwFormat),
               displayState.pixelInfo.bitsPerComponent);
        hwSpecificInfo.graphFormat = hwFormat;

        switch (hwDepth) {
            case CRTHWDepth::DEPTH_8: {
                hwSpecificInfo.bytesPerPixel = 1;
            } break;
            case CRTHWDepth::DEPTH_16: {
                hwSpecificInfo.bytesPerPixel = 2;
            } break;
            case CRTHWDepth::DEPTH_32: {
                hwSpecificInfo.bytesPerPixel = 4;
            } break;
            case CRTHWDepth::DEPTH_64: {
                hwSpecificInfo.bytesPerPixel = 8;
            } break;
        }
        hwSpecificInfo.pixelMode = self->getPixelMode(hwDepth, hwFormat);
        DBGLOG("X5000", "%s hwSpecificInfo.pixelMode=%s", __func__, stringifyATIPixelMode(hwSpecificInfo.pixelMode));
        hwSpecificInfo.format = self->getPixelFormat(hwSpecificInfo.pixelMode);
        DBGLOG("X5000", "%s hwSpecificInfo.format=%s", __func__, stringifyATIFormat(hwSpecificInfo.format));
        hwSpecificInfo.isInterlaced = self->isDisplayInterlaceEnabled(fbIndex);
        displayState.status.setIsInterlaced(hwSpecificInfo.isInterlaced);

        ADDR2_COMPUTE_SURFACE_INFO_INPUT surfInfoInput;
        surfInfoInput.width        = fbInfo->rect.width;
        surfInfoInput.height       = fbInfo->rect.height;
        surfInfoInput.bpp          = displayState.pixelInfo.bitsPerPixel;
        surfInfoInput.resourceType = ADDR_RSRC_TEX_2D;
        surfInfoInput.format     = self->getHWInterface()->getHWAlignManager()->getAddrFormat(hwSpecificInfo.pixelMode);
        surfInfoInput.numSamples = 1;
        surfInfoInput.numSlices  = 1;
        surfInfoInput.flags.display = true;
        surfInfoInput.swizzleMode =
            self->getHWInterface()->getHWAlignManager()->getPreferredSwizzleMode2(&surfInfoInput);
        self->savedSwizzleModes()[fbIndex]  = surfInfoInput.swizzleMode;
        self->swizzleModes()[fbIndex]       = surfInfoInput.swizzleMode;
        self->savedResourceTypes()[fbIndex] = surfInfoInput.resourceType;
        if (self->getHWInterface()->getHWAlignManager()->getSurfaceInfo2(&surfInfoInput,
                                                                         &self->surfInfoOutputs()[fbIndex])
            == kIOReturnSuccess)
        {
            fbInfo->size = fbInfo->pitch * self->surfInfoOutputs()[fbIndex].height * hwSpecificInfo.bytesPerPixel;
        }
        else {
            fbInfo->size = 0;
        }
        displayState.status.setIsEnabled(true);
    }

    AMDHWDisplayState::Status combinedStatus;
    for (UInt32 i = 0; i < self->supportedDisplayCount(); i += 1) { combinedStatus |= self->displayStates()[i].status; }
    self->combinedStatus() = combinedStatus;

    fbInfo->savedSize = fbInfo->size;

    if (self->fedsParamInfo()[fbIndex].crtIndex != 0) {
        ADDR2_COMPUTE_SURFACE_INFO_INPUT surfInfoInput;
        surfInfoInput.width        = self->fedsParamInfo()[fbIndex].scaledW;
        surfInfoInput.height       = self->fedsParamInfo()[fbIndex].scaledH;
        surfInfoInput.bpp          = displayState.pixelInfo.bitsPerPixel;
        surfInfoInput.swizzleMode  = self->savedSwizzleModes()[fbIndex];
        surfInfoInput.resourceType = self->savedResourceTypes()[fbIndex];
        surfInfoInput.format     = self->getHWInterface()->getHWAlignManager()->getAddrFormat(hwSpecificInfo.pixelMode);
        surfInfoInput.numSamples = 1;
        surfInfoInput.numSlices  = 1;
        surfInfoInput.flags.display = true;
        ADDR2_COMPUTE_SURFACE_INFO_OUTPUT surfInfoOutput;
        if (self->getHWInterface()->getHWAlignManager()->getSurfaceInfo2(&surfInfoInput, &surfInfoOutput)
            == kIOReturnSuccess)
        {
            fbInfo->savedSize =
                static_cast<UInt64>(surfInfoOutput.height) * surfInfoOutput.pitch * hwSpecificInfo.bytesPerPixel;
        }
    }

    UInt64 baseAlign = self->surfInfoOutputs()[fbIndex].baseAlign;
    UInt8  shift     = 0;
    while (page_size < baseAlign) {
        baseAlign >>= 1;
        shift      += 1;
    }
    fbInfo->pageCount = alignValue(page_size << shift);

    return ret;
}

void X5000::fixedGetSurfaceInfo(AMDRadeonX5000_AMDHWAlignManager* const self, AMD_SURFACE_INFO_STRUCT* const pStruct)
{
    if (pStruct == nullptr) { return; }

    pStruct->outWidth  = 0;
    pStruct->outHeight = 0;

    if (pStruct->version != 1 || pStruct->revision != 0 || pStruct->sizeOf != sizeof(*pStruct)) { return; }

    ADDR2_COMPUTE_SURFACE_INFO_INPUT input;
    input.width         = pStruct->inWidth;
    input.height        = pStruct->inHeight;
    input.bpp           = pStruct->bytesPerPixel * 8;
    input.resourceType  = ADDR_RSRC_TEX_2D;
    input.numSamples    = 1;
    input.numSlices     = 1;
    input.flags.display = 1;
    input.swizzleMode   = self->getPreferredSwizzleMode2(&input);    // this was hardcoded to 0xa

    ADDR2_COMPUTE_SURFACE_INFO_OUTPUT output;
    if (self->getSurfaceInfo2(&input, &output) == kIOReturnSuccess) {
        pStruct->outWidth      = static_cast<UInt16>(output.pitch);
        pStruct->outHeight     = static_cast<UInt16>(output.height);
        pStruct->outTilingMode = input.swizzleMode;    // I think AMD forgot this, albeit seemingly unused
    }
}

// ─── 第八步观测探针：加速器 `start` 的出口（注册链前置字段）──────────────────
//  读 `this+0x1f40`（framebuffer 服务）/ `+0x1f48`（`OSSymbol("SpecialAMDKey")`）/
//  `+0x1f58`（AMDRadeonServiceManager 客户端）/ `+0x1f60`（电源服务管理对象）。
//  判读：
//   · `f140 == 0` ⇒ 加速器找不到 framebuffer ⇒ **注册整段被跳过**（`controller+0x7960` 必空）；
//   · `f140 != 0` ⇒ 注册已被发起（此时同门控的 `callPlatformFunctionFromDrvr` 探针会先 panic，
//     本行不会出现）；`f148` 应是一个有效 OSSymbol 指针（可由真机读数反查）。
//  安全：只读对象字段，不调用任何 Apple 方法；门控 `-NRedAccelProbe`（默认不安装本 hook）。
bool X5000::wrapAccelStart(void* const self, void* const provider)
{
    const auto ret = FunctionCast(wrapAccelStart, singleton().orgAccelStart)(self, provider);

    const UInt64 s = reinterpret_cast<UInt64>(self);
    UInt64       f140 = 0, f148 = 0, f158 = 0, f160 = 0;
    if (s >= 0xffffff7f80000000ULL) {
        auto load64 = [](UInt64 base, UInt64 off) -> UInt64 {
            return *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(base) + off);
        };
        f140 = load64(s, 0x1F40);
        f148 = load64(s, 0x1F48);
        f158 = load64(s, 0x1F58);
        f160 = load64(s, 0x1F60);
    }
    const UInt64 vRet  = ret ? 1 : 0;
    const UInt64 vProv = reinterpret_cast<UInt64>(provider);

    // 通道 A（**推荐**）：日志通道（门控 `-NRedAccelLog`，零挂死风险）——SYSLOG → `liludump` 落盘 → 离线读。
    if (checkKernelArgument("-NRedAccelLog")) {
        SYSLOG("X5000", "accel start probe: self=%llx provider=%llx ret=%llu f140=%llx f148=%llx f158=%llx f160=%llx",
               static_cast<unsigned long long>(s), static_cast<unsigned long long>(vProv),
               static_cast<unsigned long long>(vRet), static_cast<unsigned long long>(f140),
               static_cast<unsigned long long>(f148), static_cast<unsigned long long>(f158),
               static_cast<unsigned long long>(f160));
    }

    // 通道 B：panic 通道（门控 `-NRedAccelProbe`，默认关闭；高风险，慎用）。格式串未改（已投产）。
    if (checkKernelArgument("-NRedAccelProbe")) {
        panic("NRed accel start probe: self=%llx provider=%llx ret=%llu | f140=%llx f148=%llx f158=%llx f160=%llx", s,
              vProv, vRet, f140, f148, f158, f160);
    }

    return ret;
}

// ─── 第八步观测探针：加速器 `probe`（**纯观测**，定位"零实例"之因）──────────────
//  背景（2026-09-26 第 12 轮实测）：加速器 kext 已载入、类存在、注册表条件已放宽，
//  但 `AMDRadeonX5000_AMDVega10GraphicsAccelerator` **零实例**。离线反汇编 `probe`
//  （VM 0x1054）显示它只有两条拒绝路径（`-amd_no_dgpu_accel` / `IOPCITunnelled`），
//  本机都不成立 ⇒ 需要判定 probe **是否被调用**、以及**返回了什么**。
//  本 wrapper **不修改任何返回值/score**（不绕过匹配机制），只读入参、调用原函数、读出参后 panic。
//  判读：
//   · 未出现本行 ⇒ probe 从未被调用 ⇒ personality 未参与匹配（上游问题）；
//   · `ret=0 scoreOut=ffffffff` ⇒ probe 明确拒绝（`*score = -1`）；
//   · `ret=0 scoreOut=0` ⇒ probe 未接受也未拒绝（"not a match"）；
//   · `ret!=0` ⇒ probe 接受（问题在更后面，例如 start/实例化）。
//  门控 `-NRedAccelProbe2`（默认关闭）；格式串为**新开探针位**（不改已投产串）。
IOService* X5000::wrapAccelProbe(void* const self, void* const provider, SInt32* const score)
{
    const SInt32 scoreIn = (score != nullptr) ? *score : 0x7FFFFFFF;
    auto*        ret     = FunctionCast(wrapAccelProbe, singleton().orgAccelProbe)(self, provider, score);
    const SInt32 scoreOut = (score != nullptr) ? *score : 0x7FFFFFFF;

    const UInt64 vSelf = reinterpret_cast<UInt64>(self);
    const UInt64 vProv = reinterpret_cast<UInt64>(provider);
    const UInt64 vRet  = reinterpret_cast<UInt64>(ret);
    const UInt64 vIn   = static_cast<UInt64>(static_cast<UInt32>(scoreIn));
    const UInt64 vOut  = static_cast<UInt64>(static_cast<UInt32>(scoreOut));
    panic("NRed accel probe2: self=%llx prov=%llx ret=%llx scoreIn=%llx scoreOut=%llx", vSelf, vProv, vRet, vIn, vOut);

    return ret;
}
