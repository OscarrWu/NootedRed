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
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSMetaClass.h>
#include <string.h>         // strcmp
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

// 第八步观测：保存 X5000 kext 的 slide，供探针在运行时定位其内部符号（如 probe 用的属性名 OSSymbol）
UInt64 gAccelProbeProv    = 0;    // 最后一次 provider 指针
UInt64 gX5000Slide = 0;

// 第八步实验用：记录 `configureDevice` 成功查到的 framebuffer 服务（`this+0x1f40`），
//  供 `start` 入口恢复（该值随后会被基类 `IOGraphicsAccelerator2::start` 的失败清理清空）。
static UInt64 gLastF140 = 0;

void X5000::processKext(KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide, const size_t size)
{
    if (kextRadeonX5000.loadIndex != id) { return; }

    gX5000Slide = slide;

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

    // 第八步观测（2026-09-28）：hook `AMDRTHardware::initializeTtl`（**无条件安装**；内部按
    //  `-NRedTtlLog` 决定是否落盘输出）。只读内存字段、零行为改变 ⇒ 默认轮次也安全。
    {
        PenguinWizardry::PatternRouteRequest ttlIfaceReq{
            "__ZN28AMDRadeonX5000_AMDRTHardware13initializeTtlEP16_GART_PARAMETERS", wrapInitializeTtl,
            this->orgInitializeTtl};
        if (!ttlIfaceReq.route(patcher, id, slide, size)) {
            SYSLOG("X5000", "ttl-iface: failed to route AMDRTHardware::initializeTtl");
        } else {
            DBGLOG("X5000", "ttl-iface: routed AMDRTHardware::initializeTtl");
        }
    }

    // 第八步观测（2026-09-28）：hook `configureDevice` 与 `initLinkToPeer`（**无条件安装**，
    //  内部按 `-NRedAccelLog` 输出）。前者是 `this+0x1f40`（framebuffer 服务）的唯一设置者；
    //  后者按名查 `"ATIFramebuffer"`/`"IOFramebuffer"`。
    {
        PenguinWizardry::PatternRouteRequest cfgReq{
            "__ZN37AMDRadeonX5000_AMDGraphicsAccelerator15configureDeviceEP11IOPCIDevice", wrapConfigureDevice,
            this->orgConfigureDevice};
        if (!cfgReq.route(patcher, id, slide, size)) {
            SYSLOG("X5000", "cfgdev: failed to route configureDevice");
        }
        PenguinWizardry::PatternRouteRequest l2pReq{
            "__ZN37AMDRadeonX5000_AMDGraphicsAccelerator14initLinkToPeerEPKc", wrapInitLinkToPeer,
            this->orgInitLinkToPeer};
        if (!l2pReq.route(patcher, id, slide, size)) {
            SYSLOG("X5000", "cfgdev: failed to route initLinkToPeer");
        }
    }

    // 第八步观测（第 15 轮）：**已移除** `probe` 的 hook —— 第 14 轮实测它**有副作用**
    //  （hook 后系统未再走到 PP 上电、直接跑到 userspace watchdog ⇒ 匹配阶段行为被改变）。
    //  ⇒ 改为纯被动取证：`DriverInjector::wrapAddDrivers` 只记录"注入的 personality 是否在数组里"，
    //     由 `AmdRadeonController::powerUp` 的 `-NRedAccelExist2` 探针统一输出。

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
    // 入口读数（before）：`start` 会自行设置 `0x368`/`0x1e88` ⇒ 与出口对比可区分
    //  "从未设置"与"设置了又被清零"（真机出口 f368=0、f1e88 含 bit6 ⇒ 需 before 判定）。
    const UInt64 sBefore = reinterpret_cast<UInt64>(self);
    UInt64       f368Before = 0, f1e88Before = 0, f140Before = 0, f148Before = 0;
    if (sBefore >= 0xffffff7f80000000ULL) {
        f368Before  = *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(sBefore) + 0x368);
        f1e88Before = *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(sBefore) + 0x1E88);
        f140Before  = *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(sBefore) + 0x1F40);
        f148Before  = *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(sBefore) + 0x1F48);
    }

    // 诊断/修复实验（门控 `-NRedRestoreF140`，默认关闭）：基类 `IOGraphicsAccelerator2::start`
    //  在 `configureDevice` 失败后会**清理** `this+0x1f40`（framebuffer 服务）；而该值此前已被
    //  `configureDevice` 成功查到（真机非 0）⇒ 这里把它**恢复**回去，使 X5000 的 `start` 能走到
    //  "注册段"（判据：`accel drvr probe` 是否出现 / `controller+0x7960` 是否非 0 / IRI 是否成功）。
    //  ⚠️ 实验：写入的是 Apple 自己查到的指针（不构造对象、不调用任何 Apple 方法）。
    if (checkKernelArgument("-NRedRestoreF140") && sBefore >= 0xffffff7f80000000ULL && f140Before == 0 &&
        gLastF140 != 0) {
        *reinterpret_cast<UInt64*>(reinterpret_cast<UInt8*>(sBefore) + 0x1F40) = gLastF140;
        SYSLOG("X5000", "accel start: restored f140 = %llx", static_cast<unsigned long long>(gLastF140));
    }

    const auto ret = FunctionCast(wrapAccelStart, singleton().orgAccelStart)(self, provider);

    const UInt64 s = reinterpret_cast<UInt64>(self);
    UInt64       f140 = 0, f148 = 0, f158 = 0, f160 = 0, f368 = 0, f1e88 = 0;
    UInt64       f1e98 = 0, f1ea0 = 0, f1ea8 = 0, f1eb0 = 0, f1f08 = 0;
    if (s >= 0xffffff7f80000000ULL) {
        auto load64 = [](UInt64 base, UInt64 off) -> UInt64 {
            return *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(base) + off);
        };
        f140 = load64(s, 0x1F40);
        f148 = load64(s, 0x1F48);
        f158 = load64(s, 0x1F58);
        f160 = load64(s, 0x1F60);
        // f140 的查找起点与"曾设置成功"标志（离线：`0x3dae(self+0x368, 名字)` 成功后才 `orb $0x40,0x1e88`）
        f368  = load64(s, 0x368);
        f1e88 = load64(s, 0x1E88);
        // `start`（0x1290）按名查服务：`0x1e98`（provider）/`0x1ea0`（"Sleep/Wake"）/`0x1ea8`；
        //  任一为 0 即跳失败出口 0x1635 ⇒ `start` 返回失败 ⇒ 之后 `0x368` 不会被置 1、注册段不执行。
        f1e98 = load64(s, 0x1E98);
        f1ea0 = load64(s, 0x1EA0);
        f1ea8 = load64(s, 0x1EA8);
        f1eb0 = load64(s, 0x1EB0);
        f1f08 = load64(s, 0x1F08);
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
        SYSLOG("X5000", "accel start extra: f368=%llx f1e88=%llx f1e98=%llx f1ea0=%llx f1ea8=%llx f1eb0=%llx f1f08=%llx",
               static_cast<unsigned long long>(f368), static_cast<unsigned long long>(f1e88),
               static_cast<unsigned long long>(f1e98), static_cast<unsigned long long>(f1ea0),
               static_cast<unsigned long long>(f1ea8), static_cast<unsigned long long>(f1eb0),
               static_cast<unsigned long long>(f1f08));
        SYSLOG("X5000",
               "accel start extra2: f140=%llx f148=%llx f158=%llx f160=%llx | before f368=%llx f1e88=%llx f140=%llx "
               "f148=%llx",
               static_cast<unsigned long long>(f140), static_cast<unsigned long long>(f148),
               static_cast<unsigned long long>(f158), static_cast<unsigned long long>(f160),
               static_cast<unsigned long long>(f368Before), static_cast<unsigned long long>(f1e88Before),
               static_cast<unsigned long long>(f140Before), static_cast<unsigned long long>(f148Before));
    }

    // 通道 B：panic 通道（门控 `-NRedAccelProbe`，默认关闭；高风险，慎用）。格式串未改（已投产）。
    if (checkKernelArgument("-NRedAccelProbe")) {
        panic("NRed accel start probe: self=%llx provider=%llx ret=%llu | f140=%llx f148=%llx f158=%llx f160=%llx", s,
              vProv, vRet, f140, f148, f158, f160);
    }

    return ret;
}

// ─── 第八步观测（2026-09-28）：`AMDRTHardware::initializeTtl` 的运行时链路 ─────────
//  依据（离线反汇编，见 📄 `kb/re/TTL-initialize失败根因报告.md`）：
//   · 本函数（X5000，归零 VM `0x5e548`）就地构造 TTL 的入参结构：`+0x00`/`+0x08` = 两张回调表
//     （在 `this+0x20670` / `this+0x20688` 构造），`+0x20` = `*_GART_PARAMETERS`，
//     `+0x28` = `this+0x530`，`+0x30` = int；而 **`+0x10` 被清零后从未赋值**。
//   · 随后它调 `*(this+0x338)`（HWLibs 的 TTL 类）的 `vtable[0x30]` = `TTL::initialize`；
//     后者在「入参 `+0x10` == 0」时**直接返回 kIOReturnError**（未做任何实际工作）
//     ⇒ `TTL+0x578`（该表副本）保持 0、`TTL+0x5a0`（已初始化标志）保持 0。
//  本探针**只读内存字段、不调用任何 Apple 方法**，且 hook 无条件安装、内部才按门控输出；
//  观测走落盘通道（`-NRedTtlLog`）⇒ 不打断流程，一轮即可取回。
// ─── 第八步诊断实验：TTL 的"第三张回调表"桩（门控 `-NRedTtlStub`）─────────────────
//  用途见 `wrapInitializeTtl` 内的说明。表形态（离线反汇编 + 真机双向确证）为
//  `{ void* ctx; IOReturn (*fn)(void* ctx, void* in, void** out); }`。
static UInt64 gTtlStubTable[2] = {0, 0};

static IOReturn ttlStubCallback(void* ctx, void* in, void** out)
{
    (void)ctx;
    (void)in;
    if (out != nullptr) { *out = nullptr; }
    return kIOReturnSuccess;
}

void X5000::wrapInitializeTtl(void* const self, void* const gartParams)
{
    auto load64 = [](UInt64 base, UInt64 off) -> UInt64 {
        return *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(base) + off);
    };
    auto isKernelPtr = [](UInt64 p) -> bool { return p >= 0xffffff7f80000000ULL; };

    const UInt64 s = reinterpret_cast<UInt64>(self);
    const UInt64 g = reinterpret_cast<UInt64>(gartParams);

    UInt64 vt = 0, f338 = 0, f338vt = 0, slot30 = 0, f528 = 0, f530 = 0, f20690 = 0;
    UInt64 g0 = 0, g8 = 0, g10 = 0;
    if (isKernelPtr(s)) {
        vt     = load64(s, 0x000);
        f338   = load64(s, 0x338);
        f528   = load64(s, 0x528);
        f530   = load64(s, 0x530);
        f20690 = load64(s, 0x20690);
        if (isKernelPtr(f338)) {
            f338vt = load64(f338, 0x000);
            if (isKernelPtr(f338vt)) { slot30 = load64(f338vt, 0x30); }
        }
    }
    if (isKernelPtr(g)) {
        g0  = load64(g, 0x00);
        g8  = load64(g, 0x08);
        g10 = load64(g, 0x10);
    }

    // ── 判别性实验（门控 `-NRedTtlStub`，默认关闭）：补上 TTL 的"第三张回调表" ──
    //  真机（`observe-20260928-0153-ttl2`）判读：`TTL::initialize` 在**非 Safe Boot** 下有两道门——
    //   ① 入参 `+0x10` 非 0 ⇒ 直接返回 kIOReturnError（只有 safeboot 才继续）；
    //   ② 入参 `+0x10` == 0 且 `this+0x578` == 0 ⇒ 直接返回 kIOReturnError。
    //  本块在调用原函数**之前**把 `TTL+0x578` 写成本 kext 内的桩表 `{ctx, fn}`（`fn` 恒返回成功），
    //  使第 ② 道门通过。**判据**：`ttl5a0`（已初始化标志）是否由 0 变 1；以及加速器 `start` 的
    //  `f140` 是否变为非 0（注册段是否被走到）。
    //  ⚠️ 这是**诊断实验**，不是修复：桩表不实现任何真实回调语义。
    if (checkKernelArgument("-NRedTtlStub") && isKernelPtr(f338)) {
        auto store64 = [](UInt64 base, UInt64 off, UInt64 v) {
            *reinterpret_cast<UInt64*>(reinterpret_cast<UInt8*>(base) + off) = v;
        };
        gTtlStubTable[0] = f338;
        gTtlStubTable[1] = reinterpret_cast<UInt64>(&ttlStubCallback);
        const UInt64 tableAddr = reinterpret_cast<UInt64>(&gTtlStubTable[0]);
        store64(f338, 0x578, tableAddr);
        SYSLOG("X5000", "ttl-stub: TTL=%llx TTL+0x578 <- %llx (fn=%llx)", static_cast<unsigned long long>(f338),
               static_cast<unsigned long long>(tableAddr),
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(&ttlStubCallback)));
    }

    // 判别实验（门控 `-NRedTtlStub48`，默认关闭）：真机已证 `*(TTL+0x48) = 0` ⇒ 命中出口 4
    //  （`0xa285f` 要求 `*(TTL+0x48)` 的三个字段非 0）。`0xa285f` 只是**拷贝**这些字段
    //  （`*(rcx+0x38/0x40/0x48) = 桩字段`），**不会把它们当函数调用** ⇒ 用自指的最小桩即可。
    if (checkKernelArgument("-NRedTtlStub48") && isKernelPtr(f338)) {
        auto store64 = [](UInt64 base, UInt64 off, UInt64 v) {
            *reinterpret_cast<UInt64*>(reinterpret_cast<UInt8*>(base) + off) = v;
        };
        const UInt64 stub = f338 + 0xB00;   // TTL 对象 0xb40 字节 ⇒ 用尾部空闲区
        store64(stub, 0x00, stub);
        store64(stub, 0x08, stub);
        store64(stub, 0x10, stub);
        store64(f338, 0x48, stub);
        SYSLOG("X5000", "ttl-stub48: TTL=%llx TTL+0x48 <- %llx", static_cast<unsigned long long>(f338),
               static_cast<unsigned long long>(stub));
    }

    // 入口读数（与出口对比用）：`0x8b10e` 会**改写** `&TTL+0x50` 子结构 ⇒ 若某字段调用前后
    //  完全不变，说明"写它的那一步"没被走到，即可反推失败步。
    if (checkKernelArgument("-NRedTtlLog") && isKernelPtr(f338)) {
        auto ld32 = [](UInt64 b, UInt64 o) -> UInt32 {
            return *reinterpret_cast<const UInt32*>(reinterpret_cast<const UInt8*>(b) + o);
        };
        SYSLOG("X5000",
               "ttl before: t50=%x t54=%x t58=%llx t60=%llx t68=%llx t1a0=%llx t4e0=%llx t4a0=%llx t88=%x ta0=%llx",
               static_cast<unsigned int>(ld32(f338, 0x50)), static_cast<unsigned int>(ld32(f338, 0x54)),
               static_cast<unsigned long long>(load64(f338, 0x58)), static_cast<unsigned long long>(load64(f338, 0x60)),
               static_cast<unsigned long long>(load64(f338, 0x68)), static_cast<unsigned long long>(load64(f338, 0x1A0)),
               static_cast<unsigned long long>(load64(f338, 0x4E0)), static_cast<unsigned long long>(load64(f338, 0x4A0)),
               static_cast<unsigned int>(ld32(f338, 0x88)),
               static_cast<unsigned long long>(load64(f338, 0xA0)));
    }

    FunctionCast(wrapInitializeTtl, singleton().orgInitializeTtl)(self, gartParams);

    UInt64 ttl568 = 0, ttl570 = 0, ttl578 = 0, ttl580 = 0, ttl588 = 0, ttl590 = 0, ttl598 = 0, ttl5a0 = 0;
    // 0x8b10e 的第一步 0x90d75 检查 `&TTL+0x50` 处的一个子结构（离线：`f0 >= 0x130`、
    //  `f4 <= 9`、`f8/f10/f18` 非空）——失败则 status = 4。故读这几项定位不满足者。
    UInt32 ttl50 = 0, ttl54 = 0, ttl88 = 0;
    UInt64 ttl58 = 0, ttl60 = 0, ttl68 = 0, ttl1a0 = 0, ttl4e0 = 0, ttl4a0 = 0, ttla0 = 0;
    UInt64 t48 = 0, t48_0 = 0, t48_40 = 0, t48_48 = 0;
    if (isKernelPtr(f338)) {
        auto load32 = [](UInt64 base, UInt64 off) -> UInt32 {
            return *reinterpret_cast<const UInt32*>(reinterpret_cast<const UInt8*>(base) + off);
        };
        ttl50  = load32(f338, 0x50);
        ttl54  = load32(f338, 0x54);
        ttl58  = load64(f338, 0x58);
        ttl60  = load64(f338, 0x60);
        ttl68  = load64(f338, 0x68);
        ttl1a0 = load64(f338, 0x1A0);
        ttl4e0 = load64(f338, 0x4E0);
        ttl4a0 = load64(f338, 0x4A0);
        ttl88  = load32(f338, 0x88);
        ttla0  = load64(f338, 0xA0);
        // 出口 4 判据（子 agent `TtlExits`）：`0xa285f(r14, config+0x28)` 要求 `*(config+0x28)` 的
        //  +0x00 / +0x40 / +0x48 均非 0（`config` = `&TTL+0x20`，故 `config+0x28` = `&TTL+0x48`）。
        t48 = load64(f338, 0x48);
        if (isKernelPtr(t48)) {
            t48_0  = load64(t48, 0x00);
            t48_40 = load64(t48, 0x40);
            t48_48 = load64(t48, 0x48);
        }
    }
    if (isKernelPtr(f338)) {
        ttl568 = load64(f338, 0x568);
        ttl570 = load64(f338, 0x570);
        ttl578 = load64(f338, 0x578);    // ★ 失败判据：参数+0x10 的副本；== 0 即"零表早退"
        ttl580 = load64(f338, 0x580);    // safeboot 标志
        ttl588 = load64(f338, 0x588);    // = *(_GART_PARAMETERS)（nonlocalMemSizeLimitBytes）
        ttl590 = load64(f338, 0x590);
        ttl598 = load64(f338, 0x598);
        ttl5a0 = load64(f338, 0x5A0);    // 已初始化标志（成功才为 1）
    }

    if (checkKernelArgument("-NRedTtlLog")) {
        SYSLOG("X5000",
               "ttl iface: self=%llx gart=%llx | vt=%llx f338=%llx f338vt=%llx slot30=%llx f528=%llx f530=%llx "
               "f20690=%llx | g0=%llx g8=%llx g10=%llx | ttl568=%llx ttl570=%llx ttl578=%llx ttl580=%llx ttl588=%llx "
               "ttl590=%llx ttl598=%llx ttl5a0=%llx",
               static_cast<unsigned long long>(s), static_cast<unsigned long long>(g),
               static_cast<unsigned long long>(vt), static_cast<unsigned long long>(f338),
               static_cast<unsigned long long>(f338vt), static_cast<unsigned long long>(slot30),
               static_cast<unsigned long long>(f528), static_cast<unsigned long long>(f530),
               static_cast<unsigned long long>(f20690), static_cast<unsigned long long>(g0),
               static_cast<unsigned long long>(g8), static_cast<unsigned long long>(g10),
               static_cast<unsigned long long>(ttl568), static_cast<unsigned long long>(ttl570),
               static_cast<unsigned long long>(ttl578), static_cast<unsigned long long>(ttl580),
               static_cast<unsigned long long>(ttl588), static_cast<unsigned long long>(ttl590),
               static_cast<unsigned long long>(ttl598), static_cast<unsigned long long>(ttl5a0));
        SYSLOG("X5000",
               "ttl sub50: t50=%x t54=%x t58=%llx t60=%llx t68=%llx t1a0=%llx t4e0=%llx t4a0=%llx t88=%x ta0=%llx",
               static_cast<unsigned int>(ttl50), static_cast<unsigned int>(ttl54),
               static_cast<unsigned long long>(ttl58), static_cast<unsigned long long>(ttl60),
               static_cast<unsigned long long>(ttl68), static_cast<unsigned long long>(ttl1a0),
               static_cast<unsigned long long>(ttl4e0), static_cast<unsigned long long>(ttl4a0),
               static_cast<unsigned int>(ttl88), static_cast<unsigned long long>(ttla0));
        SYSLOG("X5000", "ttl exit4: t48=%llx t48_0=%llx t48_40=%llx t48_48=%llx", static_cast<unsigned long long>(t48),
               static_cast<unsigned long long>(t48_0), static_cast<unsigned long long>(t48_40),
               static_cast<unsigned long long>(t48_48));
    }
}

// ─── 第八步观测（2026-09-28）：`configureDevice` 与 `initLinkToPeer` ────────────
//  离线结论：`this+0x1f40`（framebuffer 服务）的**唯一设置者**是
//  `AMDGraphicsAccelerator::configureDevice`（VM 0x3306）：它在 `0x338b`/`0x33a7` 调
//  `initLinkToPeer`（VM 0x3dae）按名查 `"ATIFramebuffer"`，失败再查 `"IOFramebuffer"`，
//  命中才写 `this+0x1f40`（0x3393/0x33ac）并 `orb $0x40,0x1e88`（0x33bd）。
//  真机已证：`start` 入口/出口 `f140` 均为 0、且该对象 `f1e88` 入口为 0（首次 start）。
//  ⚠️ **更正（2026-09-29，由乙线 R1' 离线分析发现）**：此处原写"`this+0x1f40` **从未被设置**"**有误**——
//     第 10 轮真机读数（`docs/真机轮次台账.md` 第 10 行 / 归档 `observe-20260928-0245-cfgdev`）显示
//     `configureDevice` **确被调用，且其时 `this+0x1f40` 非 0**，即它**被设置过**；随后基类
//     `IOGraphicsAccelerator2::start`（IOAF VM 0x3ba10）在失败清理中把 `this+0x1f40` **清空**
//     （第 18 轮 `-NRedRestoreF140` 实测"入口恢复生效、`start` 内部又清掉"即为旁证）。
//  ⇒ 正确的因果是：**`f140=0` 是结果而非原因**；真正的失败点在 `configureDevice` 内的
//     `0x360f` 检查（详见 `docs/子任务/乙线R1-加速器注册链离线分析.md` §Q2）。**勿再据"从未被设置"做推断。**
//  ⇒ 注册段跳过 ⇒ `start` 返回失败。
//  本组探针回答"configureDevice 是否被调用、initLinkToPeer 查到什么、返回值如何"。
//  只读字段 + 记录入参/返回值；落盘通道（`-NRedAccelLog`），hook 无条件安装。
UInt64 X5000::wrapConfigureDevice(void* const self, void* const provider)
{
    const UInt64 s = reinterpret_cast<UInt64>(self);
    if (checkKernelArgument("-NRedAccelLog")) {
        SYSLOG("X5000", "cfgdev enter: self=%llx provider=%llx", static_cast<unsigned long long>(s),
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(provider)));
        // 读 `configureDevice` 在 `0x360f` 检查的那个键（**只读**）。
        //  基址语义已用真机验证：`vptr − gX5000Slide = (0x4D34028 − 0x4B37000) + 0x10`
        //  （Vega10 accelerator vtable 的第一个虚函数）⇒ **`gX5000Slide` = X5000 的运行时基址**，
        //  故 `kc 绝对 → 运行时 = gX5000Slide + (kc绝对 − 0x4B37000)`。
        if (s >= 0xffffff7f80000000ULL && gX5000Slide != 0) {
            const UInt64 gotAbs = gX5000Slide + 0x1ED118ULL;    // 0x1ED118 的归零 vm
            UInt64       got = 0;
            got = *reinterpret_cast<const UInt64*>(gotAbs);
            SYSLOG("X5000", "cfgdev key-probe: base=%llx gotAbs=%llx got=%llx",
                   static_cast<unsigned long long>(gX5000Slide), static_cast<unsigned long long>(gotAbs),
                   static_cast<unsigned long long>(got));
            if (got >= 0xffffff8000000000ULL) {
                // `got` 是内核侧对象（上一轮读 `+0x18` 得垃圾 ⇒ 布局与预期不符）⇒ dump 前 6 个 qword，
                //  并对"像内核指针"的项做**限长**字符串试读（%.32s，零外推、不调用任何 Apple 方法）。
                UInt64 q[6] = {0, 0, 0, 0, 0, 0};
                for (int i = 0; i < 6; ++i) { q[i] = *reinterpret_cast<const UInt64*>(got + static_cast<UInt64>(i) * 8); }
                SYSLOG("X5000", "cfgdev key-q: %llx %llx %llx %llx %llx %llx",
                       static_cast<unsigned long long>(q[0]), static_cast<unsigned long long>(q[1]),
                       static_cast<unsigned long long>(q[2]), static_cast<unsigned long long>(q[3]),
                       static_cast<unsigned long long>(q[4]), static_cast<unsigned long long>(q[5]));
                for (int i = 0; i < 6; ++i) {
                    if (q[i] >= 0xffffff8000000000ULL) {
                        SYSLOG("X5000", "cfgdev key-str[%d]: %.32s", i, reinterpret_cast<const char*>(q[i]));
                    }
                }
            }
        }
    }

    UInt64 ret = FunctionCast(wrapConfigureDevice, singleton().orgConfigureDevice)(self, provider);

    // 判别性实验（门控 `-NRedCfgDevForce`，默认关闭）：`configureDevice` 在 `0x360f` 检查失败后
    //  返回 false ⇒ 基类 `IOGraphicsAccelerator2::start` 判定 "configureDevice failed" ⇒ 失败退出
    //  并**清空 `this+0x1f40`**（framebuffer 服务）⇒ 加速器注册段被跳过。本块把它强制改为"成功"，
    //  用于判定"这一处检查是否为唯一阻塞"（判据：`f140` 是否保留、`start` 是否继续、注册是否发起）。
    //  ⚠️ 诊断实验，不是修复。
    if (ret == 0 && checkKernelArgument("-NRedCfgDevForce")) {
        SYSLOG("X5000", "cfgdev: forcing success (original ret = 0)");
        ret = 1;
    }

    UInt64 f140 = 0, f1e88 = 0, f368 = 0, f1f28 = 0, f1f30 = 0, f1a68 = 0, f1a40 = 0;
    if (s >= 0xffffff7f80000000ULL) {
        auto load64 = [](UInt64 base, UInt64 off) -> UInt64 {
            return *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(base) + off);
        };
        f140  = load64(s, 0x1F40);
        f1e88 = load64(s, 0x1E88);
        f368  = load64(s, 0x368);
        // configureDevice 的 `je 0x368b` 失败出口逐个读回：0x33ee→f1f28、0x342f→f1a68、0x344d→f1a40
        f1f28 = load64(s, 0x1F28);
        f1f30 = load64(s, 0x1F30);
        f1a68 = load64(s, 0x1A68);
        f1a40 = load64(s, 0x1A40);
    }
    // ─── R1'-b 最小读数探针捕获（`-NRedR1Probe`，默认关）───────────────────────
    //  依据：`docs/子任务/乙线R1b-360f检查语义分析.md` §6（真机最小判据集）
    //  仅在 configureDevice 返回时捕获，**不读 GPU 寄存器、不调 Apple 方法**（panic 处只读已捕获标量）。
    if (checkKernelArgument("-NRedR1Probe")) {
        gR1bProbeEnabled = true;
        gR1bCfgDevSelf = s;
        gR1bCfgDevProvider = reinterpret_cast<UInt64>(provider);
        // 读 configureDevice 返回时的四个字段（R1b 报告 §6 项 1、5）
        if (s >= 0xffffff7f80000000ULL) {
            auto load64 = [](UInt64 base, UInt64 off) -> UInt64 {
                return *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(base) + off);
            };
            gR1bCfgDevF1F10 = load64(s, 0x1F10);
            gR1bCfgDevF1F14 = load64(s, 0x1F14);
            gR1bCfgDevF1F18 = load64(s, 0x1F18);
            gR1bCfgDevF1F58 = load64(s, 0x1F58);
            gR1bCfgDevF1F40 = load64(s, 0x1F40);  // framebuffer 服务（用于 AAPL,aux-power-connected）
            // *0x1ed118（GOT 项）所指对象的首字段（R1b 报告 §6 项 3）
            if (gX5000Slide != 0) {
                const UInt64 gotAbs = gX5000Slide + 0x1ED118ULL;
                const UInt64 gotPtr = *reinterpret_cast<const UInt64*>(gotAbs);
                if (gotPtr >= 0xffffff8000000000ULL) {
                    gR1bKeyObjFirstField = *reinterpret_cast<const UInt64*>(gotPtr);
                }
            }
            // this+0x1a38 所指对象的 vtable 与 vtable[0x24]（R1b 报告 §6 项 4）
            const UInt64 obj1a38 = load64(s, 0x1A38);
            gR1bObj1A38 = obj1a38;
            if (obj1a38 >= 0xffffff7f80000000ULL) {
                const UInt64 vtablePtr = *reinterpret_cast<const UInt64*>(obj1a38);
                gR1bObj1A38Vtable = vtablePtr;
                if (vtablePtr >= 0xffffff8000000000ULL) {
                    // vtable[0x24] = vtable + 0x24*8 = vtable + 0x120
                    gR1bObj1A38Vtable24 = *reinterpret_cast<const UInt64*>(vtablePtr + 0x120);
                }
            }
        }
        // provider 的 AAPL,aux-power-connected：存在性 + 类型 + 值（R1b 报告 §6 项 2）
        // 这里的 provider 是 configureDevice 的参数（IOPCIDevice*），但检查在清理分支里用的是 this+0x1f40（framebuffer 服务）。
        // 我们捕获 this+0x1f40 指向的服务对象，并在正常上下文里安全调用 getProperty。
        if (gR1bCfgDevF1F40 >= 0xffffff7f80000000ULL) {
            IOService* fbSvc = reinterpret_cast<IOService*>(gR1bCfgDevF1F40);
            OSObject* propObj = fbSvc->getProperty("AAPL,aux-power-connected");
            if (propObj != nullptr) {
                gR1bAuxPowerExists = true;
                // 简化类型识别：用 metaClass 指针低位做类型码（1=OSData,2=OSNumber,3=OSBoolean,0=其它）
                OSMetaClass* mc = propObj->getMetaClass();
                const char* clsName = mc ? mc->getClassName() : nullptr;
                UInt32 typeCode = 0;
                UInt64 value = 0;
                if (clsName) {
                    if (strcmp(clsName, "OSData") == 0) {
                        typeCode = 1;
                        OSData* data = OSDynamicCast(OSData, propObj);
                        if (data) {
                            const void* bytes = data->getBytesNoCopy();
                            UInt32 len = data->getLength();
                            if (bytes && len >= 8) {
                                value = *reinterpret_cast<const UInt64*>(bytes);
                            } else if (bytes && len > 0) {
                                // 不足 8 字节，逐字节拼装
                                const UInt8* b = static_cast<const UInt8*>(bytes);
                                for (UInt32 i = 0; i < len; ++i) {
                                    value |= (UInt64)b[i] << (i * 8);
                                }
                            }
                        }
                    } else if (strcmp(clsName, "OSNumber") == 0) {
                        typeCode = 2;
                        OSNumber* num = OSDynamicCast(OSNumber, propObj);
                        if (num) { value = num->unsigned64BitValue(); }
                    } else if (strcmp(clsName, "OSBoolean") == 0) {
                        typeCode = 3;
                        OSBoolean* b = OSDynamicCast(OSBoolean, propObj);
                        if (b) { value = b->getValue() ? 1 : 0; }
                    }
                }
                gR1bAuxPowerType = typeCode;
                gR1bAuxPowerValue = value;
            }
        }
    }
    if (f140 >= 0xffffff7f80000000ULL) { gLastF140 = f140; }   // 供 `start` 入口恢复
    if (checkKernelArgument("-NRedAccelLog")) {
        SYSLOG("X5000",
               "cfgdev exit: ret=%llu f140=%llx f1e88=%llx f368=%llx f1f28=%llx f1f30=%llx f1a68=%llx f1a40=%llx",
               static_cast<unsigned long long>(ret), static_cast<unsigned long long>(f140),
               static_cast<unsigned long long>(f1e88), static_cast<unsigned long long>(f368),
               static_cast<unsigned long long>(f1f28), static_cast<unsigned long long>(f1f30),
               static_cast<unsigned long long>(f1a68), static_cast<unsigned long long>(f1a40));
    }
    return ret;
}

void* X5000::wrapInitLinkToPeer(void* const self, const char* const name)
{
    void* const ret = FunctionCast(wrapInitLinkToPeer, singleton().orgInitLinkToPeer)(self, name);
    if (checkKernelArgument("-NRedAccelLog")) {
        // 名称为本 kext 内的字面量（"ATIFramebuffer"/"IOFramebuffer"）⇒ 打印是安全的。
        SYSLOG("X5000", "link2peer: name=%s ret=%llx", (name != nullptr) ? name : "(null)",
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(ret)));
    }
    return ret;
}

// 第八步观测（第 14 轮）：`probe` 的结果**不在原地 panic**（第 13 轮实测：匹配阶段 panic 太早，
// panic 通道未就绪 ⇒ 零分片、不自动重启），改为写入全局静态标量，由**安全位置**
// （`AmdRadeonController::powerUp` 的 `-NRedAccelExist2` 探针）统一输出。
UInt64 gAccelProbeCalls   = 0;    // probe 被调用次数
UInt64 gAccelProbeRet     = 0;    // 最后一次返回对象指针
UInt64 gAccelProbeScoreIn = 0;    // 入口 score
UInt64 gAccelProbeScoreOut = 0;   // 出口 score（0xffffffff = *score 被置 -1 ⇒ 明确拒绝）
// ─── R1'-b 最小读数探针（`-NRedR1Probe`，默认关）─────────────────────────────
//  依据：`docs/子任务/乙线R1b-360f检查语义分析.md` §6（真机最小判据集）
//  仅在 configureDevice 返回时捕获内存字段与属性，**不读 GPU 寄存器、不调 Apple 方法**（panic 处只读已捕获标量）。
bool        gR1bProbeEnabled      = false;
UInt64      gR1bCfgDevSelf        = 0;      // configureDevice 的 this（加速器对象）
UInt64      gR1bCfgDevProvider    = 0;      // configureDevice 的 provider 参数（IOPCIDevice*）
UInt64      gR1bCfgDevF1F10       = 0;      // this+0x1F10
UInt64      gR1bCfgDevF1F14       = 0;      // this+0x1F14
UInt64      gR1bCfgDevF1F18       = 0;      // this+0x1F18
UInt64      gR1bCfgDevF1F58       = 0;      // this+0x1F58
UInt64      gR1bCfgDevF1F40       = 0;      // this+0x1F40（framebuffer 服务，用于读 AAPL,aux-power-connected）
// provider 的 AAPL,aux-power-connected：存在性/类型/值（在 configureDevice 上下文捕获）
bool        gR1bAuxPowerExists    = false;
UInt32      gR1bAuxPowerType      = 0;      // 简化类型码：1=OSData,2=OSNumber,3=OSBoolean,0=失败/其它
UInt64      gR1bAuxPowerValue     = 0;      // 值：OSData 取首 8 字节；OSNumber 取数值；OSBoolean 取 0/1
// *0x1ed118（GOT 项）所指对象的首字段
UInt64      gR1bKeyObjFirstField  = 0;
// this+0x1a38 所指对象的 vtable 指针与 vtable[0x24] 目标地址
UInt64      gR1bObj1A38           = 0;      // this+0x1A38 指向的对象指针
UInt64      gR1bObj1A38Vtable     = 0;      // 该对象的 vtable 指针（对象首字段）
UInt64      gR1bObj1A38Vtable24   = 0;      // vtable[0x24] = vtable + 0x120 处的目标地址


// ─── 第八步观测探针：加速器 `probe`（**纯观测**，定位"零实例"之因）──────────────
//  背景：第 12 轮实测加速器类**零实例**；第 13 轮证明 `probe` **确实被调用**（该轮 panic 太早、
//  零分片）。离线反汇编 `probe`（VM 0x1054）显示其拒绝路径只有 `-amd_no_dgpu_accel` 与
//  `IOPCITunnelled`（本机都不成立）⇒ 需要看清它的返回值与 score。
//  本 wrapper **不修改任何返回值/score**（不绕过匹配机制），只记录到全局标量。
//  判读（读数在 `powerUp` 处输出）：
//   · `calls = 0` ⇒ probe 从未被调用；
//   · `ret = 0 且 scoreOut = 0xffffffff` ⇒ probe 明确拒绝（`*score = -1`）；
//   · `ret = 0 且 scoreOut = 0` ⇒ 未接受也未拒绝（"not a match"）；
//   · `ret != 0` ⇒ 接受（问题在更后面）。
//  门控 `-NRedAccelProbe2`（默认关闭，仅记录、不 panic）。
IOService* X5000::wrapAccelProbe(void* const self, void* const provider, SInt32* const score)
{
    const SInt32 scoreIn = (score != nullptr) ? *score : 0x7FFFFFFF;
    auto*        ret     = FunctionCast(wrapAccelProbe, singleton().orgAccelProbe)(self, provider, score);
    const SInt32 scoreOut = (score != nullptr) ? *score : 0x7FFFFFFF;

    gAccelProbeCalls += 1;
    gAccelProbeRet = reinterpret_cast<UInt64>(ret);
    gAccelProbeScoreIn = static_cast<UInt64>(static_cast<UInt32>(scoreIn));
    gAccelProbeScoreOut = static_cast<UInt64>(static_cast<UInt32>(scoreOut));
    gAccelProbeProv = reinterpret_cast<UInt64>(provider);

    return ret;
}
