// AMDRadeonX6000Framebuffer Patches
//
// Copyright © 2022-2025 ChefKiss. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#include "AmdAtomPspDirectoryDummy.hpp"
#include "AmdAtomVramInfoIGP.hpp"
#include <ASICCaps.hpp>
#include "HWLibs.hpp"
#include <GPUDriversAMD/ATOMBIOS.hpp>
#include <GPUDriversAMD/CAIL/ASICCaps.hpp>
#include <GPUDriversAMD/FB/AmdAsicInfo.hpp>
#include <GPUDriversAMD/FB/AmdDeviceMemoryManager.hpp>
#include <GPUDriversAMD/FB/VidMemType.hpp>
#include <GPUDriversAMD/Family.hpp>
#include <GPUDriversAMD/PhoenixPPSMC.hpp>
#include <GPUDriversAMD/RavenIPOffset.hpp>
#include <Headers/kern_mach.hpp>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>
#include <StageMark.hpp>
#include <kern/debug.h>    // panic() 声明（Probe D1 v2 崩溃出口注入）
#include <IOKit/IOReturn.h>
#include <IOKit/IOTypes.h>
#include <IOKit/acpi/IOACPIPlatformExpert.h>
#include <Kexts.hpp>
#include <NRed.hpp>
#include <PenguinWizardry/KernelVersion.hpp>
#include <PenguinWizardry/PatcherPlus.hpp>
#include <Regs/OSSSYS_4.hpp>
#include <Regs/SMUIO.hpp>
#include <X6000FB.hpp>
#include <libkern/OSTypes.h>
#include <mach/i386/vm_param.h>
#include <mach/i386/vm_types.h>
#include <mach/kern_return.h>

static const UInt8 kCailAsicCapsTablePattern[] = {0x6E, 0x00, 0x00, 0x00, 0x98, 0x67, 0x00, 0x00,
                                                  0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                                                  0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};

static const UInt8 kPopulateVramInfoPattern[]     = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48,
                                                     0x81, 0xEC, 0x08, 0x01, 0x00, 0x00, 0x40, 0x89, 0xF0, 0x40,
                                                     0x89, 0xF0, 0x4C, 0x8D, 0xBD, 0xE0, 0xFE, 0xFF, 0xFF};
static const UInt8 kPopulateVramInfoPatternMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                     0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0, 0xFF, 0xF0, 0xF0,
                                                     0xFF, 0xF0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static const UInt8 kIH40IVRingInitHardwarePattern[]     = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                                           0x55, 0x41, 0x54, 0x53, 0x50, 0x40, 0x89, 0xF0, 0x49,
                                                           0x89, 0xF0, 0x40, 0x8B, 0x00, 0x00, 0x44, 0x00, 0x00};
static const UInt8 kIH40IVRingInitHardwarePatternMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                           0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0, 0xFF, 0xF0, 0xFF,
                                                           0xFF, 0xF0, 0xF0, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF};

static const UInt8      kIRQMGRWriteRegisterCallPattern[]          = {0xBE, 0x4F, 0x0E, 0x00, 0x00, 0x4C, 0x89, 0xF7,
                                                                      0x89, 0xC2, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kIRQMGRWriteRegisterCallPatternMask[]      = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                      0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr size_t kIRQMGRWriteRegisterCallPatternJumpInstOff = 10;

static const UInt8      kIRQMGRReadRegisterCallPattern[]          = {0xBE, 0x4F, 0x0E, 0x00, 0x00, 0x4C, 0x89,
                                                                     0xF7, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kIRQMGRReadRegisterCallPatternMask[]      = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                     0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr size_t kIRQMGRReadRegisterCallPatternJumpInstOff = 8;

static const UInt8 kDpReceiverPowerCtrlPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53,
                                                    0x48, 0x83, 0xEC, 0x10, 0x89, 0xF3, 0xB0, 0x02, 0x28, 0xD8};
static const UInt8 kDpReceiverPowerCtrlPattern1404[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56,
                                                        0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x10, 0x41,
                                                        0x89, 0xF7, 0xB0, 0x02, 0x44, 0x28, 0xF8};

// `dc_clk_mgr_create`（Apple DC 时钟管理器工厂，未导出符号）的入口模式。
// 唯一性已核验：13.6 目标二进制与 12.5 基准各命中 1 处（kb/kexts/13.6/extracted/*.macho 本地搜索）。
// 用途见第八步执行记录：崩溃链 `[[X+0x58]+0x30]+0x118` 位于本函数内。
static const UInt8 kDcClkMgrCreatePattern[] = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x48, 0x89,
    0xFB, 0x8B, 0x47, 0x2C, 0x44, 0x8B, 0x6F, 0x34, 0x3D, 0x86, 0x00, 0x00, 0x00, 0x7E, 0x4A, 0x05,
    0x79, 0xFF, 0xFF, 0xFF, 0x83, 0xF8, 0x08, 0x0F, 0x87, 0x81, 0x01, 0x00, 0x00, 0x49, 0x89, 0xD4};

// 填充 `pp_smu_funcs` 的函数（Apple 侧 `dm_pp_get_funcs` 的落地实现，未导出符号）入口模式。
// 作用：它按"PP 能力标志"选择分支写入该结构的若干偏移；**标志不匹配则走断言路径、什么都不写**。
// 唯一性：在 13.6 目标二进制中命中 1 处（12.5 基准未命中，故本探针仅按 13.6 设计）。
static const UInt8 kPpSmuFillPattern[] = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC, 0x90, 0x00,
    0x00, 0x00, 0x48, 0x89, 0xF3, 0x4C, 0x8B, 0x77, 0x08, 0x4C, 0x89, 0xF7, 0xE8, 0xED, 0xC1, 0xFE,
    0xFF, 0x48, 0x85, 0xDB, 0x0F, 0x84, 0x82, 0x01, 0x00, 0x00, 0x49, 0x89, 0xC7, 0x4C, 0x8D, 0xA5};

static const UInt8      kCreateVramInfoCallPattern[]          = {0x48, 0x8B, 0x7B, 0x18, 0x48, 0x8B, 0x43, 0x20, 0x0F,
                                                                 0xB7, 0x70, 0x3C, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kCreateVramInfoCallPatternMask[]      = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr UInt32 kCreateVramInfoCallPatternJumpInstOff = 12;

static const UInt8      kCreatePspDirectoryCallPattern[]     = {0x48, 0x8B, 0x7B, 0x18, 0x48, 0x8B, 0x43, 0x20, 0x0F,
                                                                0xB7, 0x70, 0x16, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kCreatePspDirectoryCallPatternMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr UInt32 kCreatePspDirectoryCallPatternJumpInstOff = 12;

static const UInt8      kCreateObjectInfoCallPattern[]          = {0x48, 0x8B, 0x7B, 0x18, 0x48, 0x8B, 0x43, 0x20, 0x0F,
                                                                   0xB7, 0x70, 0x30, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kCreateObjectInfoCallPatternMask[]      = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                   0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr UInt32 kCreateObjectInfoCallPatternJumpInstOff = 12;

// Fix register read (0xD31 -> 0xD35) and family ID (0x8F -> 0x8E).
// 0xD35 = NBIO_BASE__INST0_SEG2（0xD20，yellow_carp_offset.h:975）+ regRCC_STRAP1_RCC_DEV0_EPF0_STRAP0
//（0x15，BASE_IDX 2，nbio_7_11_0_offset.h:8818-8819），与 NRed.cpp 读 RCC_STRAP1 的 rev-id 路径一致。
static const UInt8 kPopulateDeviceInfoOriginal[]{0xBE, 0x31, 0x0D, 0x00, 0x00, 0xFF, 0x90, 0x40, 0x01,
                                                 0x00, 0x00, 0xC7, 0x43, 0x00, 0x8F, 0x00, 0x00, 0x00};
static const UInt8 kPopulateDeviceInfoMask[]{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                             0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
static const UInt8 kPopulateDeviceInfoPatched[]{0xBE, 0x35, 0x0D, 0x00, 0x00, 0xFF, 0x90, 0x40, 0x01,
                                                0x00, 0x00, 0xC7, 0x43, 0x00, 0x8E, 0x00, 0x00, 0x00};

// Remove check for Navi family
static const UInt8 kInitializeDmcubServices1Original[] = {0x81, 0x79, 0x2C, 0x8F, 0x00, 0x00, 0x00};
static const UInt8 kInitializeDmcubServices1Patched[]  = {0x39, 0xC0, 0x66, 0x90, 0x66, 0x90, 0x90};

// Set DMCUB ASIC constant to DCN 2.1
static const UInt8 kInitializeDmcubServices2Original[] = {0x83, 0xC0, 0xC4, 0x83, 0xF8, 0x0A, 0xB8,
                                                          0x03, 0x00, 0x00, 0x00, 0x83, 0xD0, 0x00};
static const UInt8 kInitializeDmcubServices2Patched[]  = {0xB8, 0x02, 0x00, 0x00, 0x00, 0x66, 0x90,
                                                          0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x90};

// Ditto, 14.4+
static const UInt8 kInitializeDmcubServices2Original1404[] = {0x83, 0xC0, 0xC4, 0x31, 0xC9, 0x83,
                                                              0xF8, 0x0A, 0x83, 0xD1, 0x03};
static const UInt8 kInitializeDmcubServices2Patched1404[]  = {0xB9, 0x02, 0x00, 0x00, 0x00, 0x66,
                                                              0x90, 0x66, 0x90, 0x66, 0x90};

// Ditto, 10.15
static const UInt8 kInitializeDmcubServices2Original1015[] = {0xC7, 0x46, 0x20, 0x01, 0x00, 0x00, 0x00};
static const UInt8 kInitializeDmcubServices2Patched1015[]  = {0xC7, 0x46, 0x20, 0x02, 0x00, 0x00, 0x00};

// 10.15: Set inst_const_size/bss_data_size to 0. To disable DMCUB firmware loading logic.
static const UInt8 kInitializeHardware1Original[] = {0x49, 0xBC, 0x00, 0x0A, 0x01, 0x00, 0xF4, 0x01, 0x00, 0x00};
static const UInt8 kInitializeHardware1Patched[]  = {0x49, 0xC7, 0xC4, 0x00, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90};

// 10.15: Set fw_inst_const to nullptr, pt.2 of above.
static const UInt8 kInitializeHardware2Original[]     = {0x4C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4C, 0x00, 0x00,
                                                         0x10, 0x4C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4C};
static const UInt8 kInitializeHardware2OriginalMask[] = {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00,
                                                         0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF};
static const UInt8 kInitializeHardware2Patched[]      = {0x49, 0xC7, 0xC5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kInitializeHardware2PatchedMask[]  = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00,
                                                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// 10.15: Disable DMCUB firmware loading from DAL. HWLibs should be doing that.
static const UInt8 kAmdDalServicesInitializeOriginal[]     = {0xBE, 0x01, 0x00, 0x00, 0x00, 0xE8, 0x00,
                                                              0x00, 0x00, 0x00, 0x49, 0x00, 0x00, 0x60};
static const UInt8 kAmdDalServicesInitializeOriginalMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
                                                              0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF};
static const UInt8 kAmdDalServicesInitializePatched[]      = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kAmdDalServicesInitializePatchedMask[]  = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
                                                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Change cursor and underflow tracker count to 4 instead of 6.
static const UInt8 kCreateControllerServicesOriginal[]     = {0x40, 0x00, 0x00, 0x40, 0x83, 0x00, 0x06};
static const UInt8 kCreateControllerServicesOriginalMask[] = {0xF0, 0x00, 0x00, 0xF0, 0xFF, 0x00, 0xFF};
static const UInt8 kCreateControllerServicesPatched[]      = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04};
static const UInt8 kCreateControllerServicesPatchedMask[]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F};

// Ditto, 10.15.
static const UInt8 kCreateControllerServicesOriginal1015[]     = {0x48, 0x00, 0x00, 0x48, 0x83, 0x00, 0x05};
static const UInt8 kCreateControllerServicesOriginalMask1015[] = {0xFF, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0xFF};
static const UInt8 kCreateControllerServicesPatched1015[]      = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03};
static const UInt8 kCreateControllerServicesPatchedMask1015[]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F};

// Change cursor count to 4 instead of 6.
static const UInt8 kSetupCursorsOriginal[]     = {0x40, 0x83, 0x00, 0x05};
static const UInt8 kSetupCursorsOriginalMask[] = {0xF0, 0xFF, 0x00, 0xFF};
static const UInt8 kSetupCursorsPatched[]      = {0x00, 0x00, 0x00, 0x03};
static const UInt8 kSetupCursorsPatchedMask[]  = {0x00, 0x00, 0x00, 0x0F};

// Ditto, 12.0+.
static const UInt8 kSetupCursorsOriginal12[]     = {0x40, 0x83, 0x00, 0x06};
static const UInt8 kSetupCursorsOriginalMask12[] = {0xF0, 0xFF, 0x00, 0xFF};
static const UInt8 kSetupCursorsPatched12[]      = {0x00, 0x00, 0x00, 0x04};
static const UInt8 kSetupCursorsPatchedMask12[]  = {0x00, 0x00, 0x00, 0x0F};

// Change link count to 4 instead of 6.
static const UInt8 kCreateLinksOriginal[]     = {0x06, 0x00, 0x00, 0x00, 0x40};
static const UInt8 kCreateLinksOriginalMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xF0};
static const UInt8 kCreateLinksPatched[]      = {0x04, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kCreateLinksPatchedMask[]  = {0x0F, 0x00, 0x00, 0x00, 0x00};

// Remove new FB count condition so we can restore the original behaviour before Ventura.
static const UInt8 kControllerPowerUpOriginal[]     = {0x38, 0xC8, 0x0F, 0x42, 0xC8, 0x88, 0x8F,
                                                       0xBC, 0x00, 0x00, 0x00, 0x72, 0x00};
static const UInt8 kControllerPowerUpOriginalMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                       0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
static const UInt8 kControllerPowerUpReplace[]      = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                       0x00, 0x00, 0x00, 0x00, 0xEB, 0x00};
static const UInt8 kControllerPowerUpReplaceMask[]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                       0x00, 0x00, 0x00, 0x00, 0xFF, 0x00};

// Remove new problematic Ventura pixel clock multiplier calculation which causes timing validation mishaps.
static const UInt8 kValidateDetailedTimingOriginal[] = {0x66, 0x0F, 0x2E, 0xC1, 0x76, 0x06, 0xF2, 0x0F, 0x5E, 0xC1};
static const UInt8 kValidateDetailedTimingPatched[]  = {0x66, 0x0F, 0x2E, 0xC1, 0x66, 0x90, 0xF2, 0x0F, 0x5E, 0xC1};

static X6000FB moduleInstance;

X6000FB& X6000FB::singleton() { return moduleInstance; }

void X6000FB::processKext(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size)
{
    if (kextRadeonX6000Framebuffer.loadIndex != id) { return; }

    DBGLOG("X6000FB", "processKext: X6000Framebuffer matched, hwLateInit begin (id=%zu slide=0x%llX size=0x%zX)",
           id, slide, size);

    NRed::singleton().hwLateInit();

    CAILAsicCapsEntry*                   orgAsicCapsTable       = nullptr;
    void*                                orgAmdAsicInfoNavi10VT = nullptr;
    PenguinWizardry::PatternSolveRequest solveRequests[]        = {
        {"__ZL20CAIL_ASIC_CAPS_TABLE", orgAsicCapsTable, kCailAsicCapsTablePattern},
        {"__ZN37AMDRadeonX6000_AmdDeviceMemoryManager17mapMemorySubRangeE25AmdReservedMemorySelectoryyj",
         this->mapMemorySubRange},
        {"__ZTV32AMDRadeonX6000_AmdAsicInfoNavi10", orgAmdAsicInfoNavi10VT},
        {"__ZNK34AMDRadeonX6000_AmdBiosParserHelper20readEfiAtomBiosImageEPhm", this->readEfiAtomBiosImage},
        {"__ZNK34AMDRadeonX6000_AmdBiosParserHelper20readPciAtomBiosImageEPhm", this->readPciAtomBiosImage},
        {"__ZNK34AMDRadeonX6000_AmdBiosParserHelper21validateAtomBiosImageEPhm", this->validateAtomBiosImage},
    };
    PANIC_COND(!PenguinWizardry::PatternSolveRequest::solveAll(patcher, id, solveRequests, slide, size), "X6000FB",
               "Failed to resolve symbols");

    PANIC_COND(MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS, "X6000FB",
               "Failed to enable kernel writing");
    getMember<decltype(getGpuBrandingNameListRenoir)*>(orgAmdAsicInfoNavi10VT, 0x228) =
        NRed::singleton().getAttributes().isRenoir() && !NRed::singleton().getAttributes().isPhoenix() ?
            getGpuBrandingNameListRenoir :
        NRed::singleton().getAttributes().isPicasso() ? getGpuBrandingNameListPicasso :
                                                        getGpuBrandingNameListRaven;
    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);

    if (checkKernelArgument("-NRedDPDelay")) {
        if (currentKernelVersion() >= MACOS_14_4) {
            PenguinWizardry::PatternRouteRequest request{"_dp_receiver_power_ctrl", wrapDpReceiverPowerCtrl,
                                                         this->orgDpReceiverPowerCtrl, kDpReceiverPowerCtrlPattern1404};
            PANIC_COND(!request.route(patcher, id, slide, size), "X6000FB",
                       "Failed to route dp_receiver_power_ctrl (14.4+)");
        }
        else {
            PenguinWizardry::PatternRouteRequest request{"_dp_receiver_power_ctrl", wrapDpReceiverPowerCtrl,
                                                         this->orgDpReceiverPowerCtrl, kDpReceiverPowerCtrlPattern};
            PANIC_COND(!request.route(patcher, id, slide, size), "X6000FB", "Failed to route dp_receiver_power_ctrl");
        }
    }

    if (currentKernelVersion() >= MACOS_13) {
        // D3: hook messageAccelerator (was solveSymbol-only; now route to wrapMessageAccelerator)
        KernelPatcher::RouteRequest maRequest{
            "__ZNK34AMDRadeonX6000_AmdRadeonController18messageAcceleratorE25_eAMDAccelIOFBRequestTypePvS1_S1_",
            wrapMessageAccelerator, this->orgMessageAccelerator};
        if (!patcher.routeMultiple(id, &maRequest, 1, slide, size)) {
            SYSLOG("X6000FB", "D3: failed to route messageAccelerator");
        } else {
            DBGLOG("X6000FB", "D3: routed messageAccelerator");
        }

        KernelPatcher::RouteRequest request{"__ZN34AMDRadeonX6000_AmdRadeonController7powerUpEv", wrapControllerPowerUp,
                                            this->orgControllerPowerUp};
        PANIC_COND(!patcher.routeMultiple(id, &request, 1, slide, size), "X6000FB", "Failed to route powerUp");

        // Observe P2 + Probe P1: hook handleCriticalError (always log; suppress panic only with -NRedProbePPLIB on Phoenix)
        {
            KernelPatcher::RouteRequest ppRequest{
                "__ZNK33AMDRadeonX6000_AmdPowerPlayHelper19handleCriticalErrorEPKcS1_S1_",
                wrapHandleCriticalError, this->orgHandleCriticalError};
            if (!patcher.routeMultiple(id, &ppRequest, 1, slide, size)) {
                SYSLOG("X6000FB", "observe P2: failed to route handleCriticalError (symbol may differ on 13.6)");
            } else {
                DBGLOG("X6000FB", "observe P2: routed handleCriticalError");
            }
        }

        // 第八步观测：hook AmdDalHelper::powerUp —— 入口读崩溃链上的指针并写 NVRAM
        //   （仅在 boot-arg `-NRedStageMark` 存在时写；route 失败只记日志，不影响引导）
        {
            KernelPatcher::RouteRequest dhRequest{
                "__ZN27AMDRadeonX6000_AmdDalHelper7powerUpEv",
                wrapDalHelperPowerUp, this->orgDalHelperPowerUp};
            if (!patcher.routeMultiple(id, &dhRequest, 1, slide, size)) {
                SYSLOG("X6000FB", "stage-mark: failed to route AmdDalHelper::powerUp");
            } else {
                DBGLOG("X6000FB", "stage-mark: routed AmdDalHelper::powerUp");
            }
        }

        // 第八步观测：hook Apple 的 dc_clk_mgr_create（**未导出符号** → 用模式定位）
        //   观测点选在它入口：此处 `ctx->f58` 已由 powerUp 内部赋值，等于崩溃时刻的值。
        {
            PenguinWizardry::PatternRouteRequest clkRequest{"dc_clk_mgr_create", wrapDcClkMgrCreate,
                                                             this->orgDcClkMgrCreate, kDcClkMgrCreatePattern};
            if (!clkRequest.route(patcher, id, slide, size)) {
                SYSLOG("X6000FB", "stage-mark: failed to route dc_clk_mgr_create (pattern miss)");
            } else {
                DBGLOG("X6000FB", "stage-mark: routed dc_clk_mgr_create");
            }
        }

        // 第八步观测（终局证据）：hook 填充 `pp_smu_funcs` 的函数（未导出 → 模式定位）。
        //   ⚠️ **条件路由**：该函数的调用者依赖调用遗留的 ZF 标志，无条件 hook 会改变行为；
        //      仅在 boot-arg `-NRedStagePanic3` 存在时才 route，保证默认路径零影响。
        if (checkKernelArgument("-NRedStagePanic3")) {
            PenguinWizardry::PatternRouteRequest ppFillRequest{"pp_smu_fill", wrapPpSmuFill,
                                                               this->orgPpSmuFill, kPpSmuFillPattern};
            if (!ppFillRequest.route(patcher, id, slide, size)) {
                SYSLOG("X6000FB", "stage-mark: failed to route pp_smu fill (pattern miss)");
            } else {
                DBGLOG("X6000FB", "stage-mark: routed pp_smu fill");
            }
        }

        const PenguinWizardry::MaskedLookupPatch patches[] = {
            {&kextRadeonX6000Framebuffer, kControllerPowerUpOriginal, kControllerPowerUpOriginalMask,
             kControllerPowerUpReplace, kControllerPowerUpReplaceMask, 1},
            {&kextRadeonX6000Framebuffer, kValidateDetailedTimingOriginal, kValidateDetailedTimingPatched, 1},
        };
        PANIC_COND(!PenguinWizardry::MaskedLookupPatch::applyAll(patcher, patches, slide, size), "X6000FB",
                   "Failed to apply logic revert patches");
    }

    PenguinWizardry::PatternRouteRequest requests[] = {
        {"__ZNK15AmdAtomVramInfo16populateVramInfoER16AtomFirmwareInfo", wrapPopulateVramInfo, kPopulateVramInfoPattern,
         kPopulateVramInfoPatternMask},
        {"__ZNK32AMDRadeonX6000_AmdAsicInfoNavi1027getEnumeratedRevisionNumberEv", getEnumeratedRevision},
        {"__ZN41AMDRadeonX6000_AmdDeviceMemoryManagerNavi21intializeReservedVramEv", initialiseReservedVRAM},
        {"__ZN38AMDRadeonX6000_AmdRadeonControllerNavi19setupBootWatermarksEv", dummyIOReturnSuccess},
        {"__ZNK30AMDRadeonX6000_AmdAgdcServices13getVendorInfoEP16AGDCVendorInfo_tm", wrapGetVendorInfo,
         this->orgGetVendorInfo},
        {"__ZN34AMDRadeonX6000_AmdBiosParserHelper12readAtomBiosEv", readAtomBios},
    };
    PANIC_COND(!PenguinWizardry::PatternRouteRequest::routeAll(patcher, id, requests, slide, size), "X6000FB",
               "Failed to route symbols");

    PenguinWizardry::JumpPatternRouteRequest atombiosRequests[] = {
        {"__ZN15AmdAtomVramInfo14createVramInfoEP15AmdAtomFwHelperj", wrapCreateVramInfo, this->orgCreateVramInfo,
         kCreateVramInfoCallPattern, kCreateVramInfoCallPatternMask, kCreateVramInfoCallPatternJumpInstOff},
        {"__ZN17AmdAtomObjectInfo16createObjectInfoEP15AmdAtomFwHelperj", wrapCreateObjectInfo,
         this->orgCreateObjectInfo, kCreateObjectInfoCallPattern, kCreateObjectInfoCallPatternMask,
         kCreateObjectInfoCallPatternJumpInstOff},
    };
    PANIC_COND(!PenguinWizardry::JumpPatternRouteRequest::routeAll(patcher, id, atombiosRequests, slide, size),
               "X6000FB", "Failed to route ATOMBIOS-related functions");

    if (currentKernelVersion() >= MACOS_11) {
        PenguinWizardry::JumpPatternRouteRequest createPspDirectoryRequest{
            "__ZN19AmdAtomPspDirectory18createPspDirectoryEP15AmdAtomFwHelperj",
            wrapCreatePspDirectory,
            this->orgCreatePspDirectory,
            kCreatePspDirectoryCallPattern,
            kCreatePspDirectoryCallPatternMask,
            kCreatePspDirectoryCallPatternJumpInstOff};
        PANIC_COND(!createPspDirectoryRequest.route(patcher, id, slide, size), "X6000FB",
                   "Failed to route createPspDirectory");

        if (currentKernelVersion() <= MACOS_12_X) {
            PenguinWizardry::PatternRouteRequest getTriageHardwareDataRequest{
                "__ZN38AMDRadeonX6000_AmdRadeonControllerNavi21getTriageHardwareDataEjP12_AMD_TRIAGE_",
                NRed::singleton().getAttributes().isRenoir() && !NRed::singleton().getAttributes().isPhoenix() ?
                    getTriageHardwareDataRN :
                    getTriageHardwareDataRV};
            PANIC_COND(!getTriageHardwareDataRequest.route(patcher, id, slide, size), "X6000FB",
                       "Failed to route getTriageHardwareData");
        }

        KernelPatcher::RouteRequest request{
            "__ZN32AMDRadeonX6000_AmdRegisterAccess20createRegisterAccessERNS_8InitDataE", wrapCreateRegisterAccess,
            this->orgCreateRegisterAccess};
        PANIC_COND(!patcher.routeMultiple(id, &request, 1, slide, size), "X6000FB",
                   "Failed to route createRegisterAccess");
    }

    if (NRed::singleton().getAttributes().isRenoir() && !NRed::singleton().getAttributes().isPhoenix()) {
        PenguinWizardry::PatternRouteRequest request{"_IH_4_0_IVRing_InitHardware", wrapIH40IVRingInitHardware,
                                                     this->orgIH40IVRingInitHardware, kIH40IVRingInitHardwarePattern,
                                                     kIH40IVRingInitHardwarePatternMask};
        PANIC_COND(!request.route(patcher, id, slide, size), "X6000FB", "Failed to route IH_4_0_IVRing_InitHardware");
        PenguinWizardry::JumpPatternRouteRequest jumpPatternRequest{"_IRQMGR_WriteRegister",
                                                                    wrapIRQMGRWriteRegister,
                                                                    this->orgIRQMGRWriteRegister,
                                                                    kIRQMGRWriteRegisterCallPattern,
                                                                    kIRQMGRWriteRegisterCallPatternMask,
                                                                    kIRQMGRWriteRegisterCallPatternJumpInstOff};
        PANIC_COND(!jumpPatternRequest.route(patcher, id, slide, size), "X6000FB",
                   "Failed to route IRQMGR_WriteRegister");
        PenguinWizardry::JumpPatternSolveRequest jumpPatternSolveRequest{
            "_IRQMGR_ReadRegister", this->irqMGRReadRegister, kIRQMGRReadRegisterCallPattern,
            kIRQMGRReadRegisterCallPatternMask, kIRQMGRReadRegisterCallPatternJumpInstOff};
        PANIC_COND(!jumpPatternSolveRequest.solve(patcher, id, slide, size), "X6000FB",
                   "Failed to solve IRQMGR_ReadRegister");
    }

    const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX6000Framebuffer, kPopulateDeviceInfoOriginal,
                                                   kPopulateDeviceInfoMask,     kPopulateDeviceInfoPatched,
                                                   kPopulateDeviceInfoMask,     1};
    PANIC_COND(!patch.apply(patcher, slide, size), "X6000FB", "Failed to apply populateDeviceInfo patch");

    if (NRed::singleton().getAttributes().isRenoir() && !NRed::singleton().getAttributes().isPhoenix()) {
        const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX6000Framebuffer, kInitializeDmcubServices1Original,
                                                       kInitializeDmcubServices1Patched, 1};
        PANIC_COND(!patch.apply(patcher, slide, size), "X6000FB",
                   "Failed to apply initializeDmcubServices family id patch");
        if (currentKernelVersion() <= MACOS_10_15_X) {
            const PenguinWizardry::MaskedLookupPatch patches[] = {
                {&kextRadeonX6000Framebuffer, kInitializeDmcubServices2Original1015,
                 kInitializeDmcubServices2Patched1015, 1},
                {&kextRadeonX6000Framebuffer, kInitializeHardware1Original, kInitializeHardware1Patched, 1},
                {&kextRadeonX6000Framebuffer, kInitializeHardware2Original, kInitializeHardware2OriginalMask,
                 kInitializeHardware2Patched, kInitializeHardware2PatchedMask, 1},
                {&kextRadeonX6000Framebuffer, kAmdDalServicesInitializeOriginal, kAmdDalServicesInitializeOriginalMask,
                 kAmdDalServicesInitializePatched, kAmdDalServicesInitializePatchedMask, 1},
            };
            PANIC_COND(!PenguinWizardry::MaskedLookupPatch::applyAll(patcher, patches, slide, size), "X6000FB",
                       "Failed to apply AmdDalDmcubService and AmdDalServices::initialize patches (10.15)");
        }
        else if (currentKernelVersion() >= MACOS_14_4) {
            const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX6000Framebuffer,
                                                           kInitializeDmcubServices2Original1404,
                                                           kInitializeDmcubServices2Patched1404, 1};
            PANIC_COND(!patch.apply(patcher, slide, size), "X6000FB",
                       "Failed to apply initializeDmcubServices ASIC patch (14.4+)");
        }
        else {
            const PenguinWizardry::MaskedLookupPatch patch{
                &kextRadeonX6000Framebuffer, kInitializeDmcubServices2Original, kInitializeDmcubServices2Patched, 1};
            PANIC_COND(!patch.apply(patcher, slide, size), "X6000FB",
                       "Failed to apply initializeDmcubServices ASIC patch");
        }
    }

    PANIC_COND(MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS, "X6000FB",
               "Failed to enable kernel writing");
    orgAsicCapsTable->familyId = AMD_FAMILY_RAVEN;
    orgAsicCapsTable->ddiCaps =
        NRed::singleton().getAttributes().isRenoirE() ? ddiCapsRenoirE :
        NRed::singleton().getAttributes().isRenoir() && !NRed::singleton().getAttributes().isPhoenix() ? ddiCapsRenoir :
                                                                                                         ddiCapsRaven;
    orgAsicCapsTable->deviceId = NRed::singleton().getDeviceID();
    orgAsicCapsTable->revision = NRed::singleton().getDevRevision();
    orgAsicCapsTable->extRevision =
        static_cast<UInt32>(NRed::singleton().getEnumRevision()) + NRed::singleton().getDevRevision();
    orgAsicCapsTable->pciRevision = NRed::singleton().getPciRevision();
    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
    DBGLOG("X6000FB", "Applied DDI Caps patches");

    // We need to patch the kext to create only 4 cursors, links and underflow trackers.
    auto* const orgCreateControllerServices = patcher.solveSymbol<void*>(
        id, "__ZN40AMDRadeonX6000_AmdRadeonControllerNavi1024createControllerServicesEv", slide, size, true);
    PANIC_COND(orgCreateControllerServices == nullptr, "X6000FB", "Failed to solve createControllerServices");

    auto* const orgSetupCursors =
        patcher.solveSymbol<void*>(id, "__ZN34AMDRadeonX6000_AmdRadeonController12setupCursorsEv", slide, size, true);
    PANIC_COND(orgSetupCursors == nullptr, "X6000FB", "Failed to solve setupCursors");

    auto* const orgCreateLinks =
        patcher.solveSymbol<void*>(id, "__ZN34AMDRadeonX6000_AmdRadeonController11createLinksEv", slide, size, true);
    PANIC_COND(orgCreateLinks == nullptr, "X6000FB", "Failed to solve createLinks");

    if (currentKernelVersion() <= MACOS_10_15_X) {
        PANIC_COND(!KernelPatcher::findAndReplaceWithMask(
                       orgCreateControllerServices, PAGE_SIZE, kCreateControllerServicesOriginal1015,
                       kCreateControllerServicesOriginalMask1015, kCreateControllerServicesPatched1015,
                       kCreateControllerServicesPatchedMask1015, 1, 0),
                   "X6000FB", "Failed to apply createControllerServices patch (10.15)");
    }
    else {
        PANIC_COND(!KernelPatcher::findAndReplaceWithMask(
                       orgCreateControllerServices, PAGE_SIZE, kCreateControllerServicesOriginal,
                       kCreateControllerServicesOriginalMask, kCreateControllerServicesPatched,
                       kCreateControllerServicesPatchedMask, 2, 0),
                   "X6000FB", "Failed to apply createControllerServices patch");
    }

    if (currentKernelVersion() >= MACOS_12) {
        PANIC_COND(!KernelPatcher::findAndReplaceWithMask(orgSetupCursors, PAGE_SIZE, kSetupCursorsOriginal12,
                                                          kSetupCursorsOriginalMask12, kSetupCursorsPatched12,
                                                          kSetupCursorsPatchedMask12, 1, 0),
                   "X6000FB", "Failed to apply setupCursors patch (12.0+)");
    }
    else {
        PANIC_COND(!KernelPatcher::findAndReplaceWithMask(orgSetupCursors, PAGE_SIZE, kSetupCursorsOriginal,
                                                          kSetupCursorsOriginalMask, kSetupCursorsPatched,
                                                          kSetupCursorsPatchedMask, 1, 0),
                   "X6000FB", "Failed to apply setupCursors patch");
    }

    PANIC_COND(!KernelPatcher::findAndReplaceWithMask(orgCreateLinks, PAGE_SIZE, kCreateLinksOriginal,
                                                      kCreateLinksOriginalMask, kCreateLinksPatched,
                                                      kCreateLinksPatchedMask, 1, 0),
               "X6000FB", "Failed to apply createLinks patch");
}

UInt16 X6000FB::getEnumeratedRevision() { return NRed::singleton().getEnumRevision(); }

enum IRQMgrIVRingMemoryType
{
    IRQMgrIVRingMemoryTypeGART        = 0x0,
    IRQMgrIVRingMemoryTypeSysPhysical = 0x1,
    IRQMgrIVRingMemoryTypeFB          = 0x2,
};

bool X6000FB::wrapIH40IVRingInitHardware(void* const ctx, void* const ring)
{
    if (getMember<IRQMgrIVRingMemoryType>(ring, 0x24) == IRQMgrIVRingMemoryTypeSysPhysical) {
        singleton().orgIRQMGRWriteRegister(ctx, IH_CHICKEN,
                                           singleton().irqMGRReadRegister(ctx, IH_CHICKEN) | IH_MC_SPACE_GPA_ENABLE);
    }
    return FunctionCast(wrapIH40IVRingInitHardware, singleton().orgIH40IVRingInitHardware)(ctx, ring);
}

void X6000FB::wrapIRQMGRWriteRegister(void* const ctx, const UInt64 off, UInt32 value)
{
    if (off == IH_CLK_CTRL) {
        if ((value & getBit(IH_DBUS_MUX_CLK_SOFT_OVERRIDE_SHIFT)) != 0) {
            value |= getBit(IH_IH_BUFFER_MEM_CLK_SOFT_OVERRIDE_SHIFT);
        }
    }
    singleton().orgIRQMGRWriteRegister(ctx, off, value);
}

void* X6000FB::wrapCreateRegisterAccess(void* const initData)
{
    getMember<UInt32>(initData, 0x24) = SMUIO_BASE_0 + ROM_INDEX;
    getMember<UInt32>(initData, 0x28) = SMUIO_BASE_0 + ROM_DATA;
    return FunctionCast(wrapCreateRegisterAccess, singleton().orgCreateRegisterAccess)(initData);
}

IOReturn X6000FB::initialiseReservedVRAM(void* const self)
{
#define CHECK(_expr)                                                     \
    if (const auto ret = _expr; ret != kIOReturnSuccess) { return ret; }
    static constexpr IOOptionBits mapOptions = kIOMapWriteCombineCache | kIOMapAnywhere;
    CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::Cursor1_32bpp, 0, 0x40000, mapOptions));
    CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::Cursor1_2bpp, 0x40000, 0x40000, mapOptions));
    CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::Cursor2_32bpp, 0x80000, 0x40000, mapOptions));
    CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::Cursor2_2bpp, 0xC0000, 0x40000, mapOptions));
    CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::Cursor3_32bpp, 0x100000, 0x40000, mapOptions));
    CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::Cursor3_2bpp, 0x140000, 0x40000, mapOptions));
    CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::Cursor4_32bpp, 0x180000, 0x40000, mapOptions));
    CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::Cursor4_2bpp, 0x1C0000, 0x40000, mapOptions));
    CHECK(
        singleton().mapMemorySubRange(self, AmdReservedMemorySelector::PPLIBReserved, 0x200000, 0x100000, mapOptions));
    if (NRed::singleton().getAttributes().isRenoir() && !NRed::singleton().getAttributes().isPhoenix()) {
        CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::DMCUBReserved, 0x300000, 0x100000,
                                            mapOptions));
        return singleton().mapMemorySubRange(self, AmdReservedMemorySelector::ReserveVRAM, 0, 0x400000, mapOptions);
    }
    return singleton().mapMemorySubRange(self, AmdReservedMemorySelector::ReserveVRAM, 0, 0x300000, mapOptions);
#undef CHECK
}

static const AmdAsicBrandingTableEntry ravenBrandingTable[] = {
    {0x15DD, 0x81, "Radeon RX", "Vega 11"}, {0x15DD, 0x82, "Radeon RX", "Vega 8"},
    {0x15DD, 0x83, "Radeon RX", "Vega 8"},  {0x15DD, 0x84, "Radeon RX", "Vega 6"},
    {0x15DD, 0x85, "Radeon RX", "Vega 3"},  {0x15DD, 0x86, "Radeon RX", "Vega 11"},
    {0x15DD, 0x88, "Radeon RX", "Vega 8"},  {0x15DD, 0xC1, "Radeon RX", "Vega 11"},
    {0x15DD, 0xC2, "Radeon RX", "Vega 8"},  {0x15DD, 0xC3, "Radeon RX", "Vega 10"},
    {0x15DD, 0xC4, "Radeon RX", "Vega 8"},  {0x15DD, 0xC5, "Radeon RX", "Vega 3"},
    {0x15DD, 0xC6, "Radeon RX", "Vega 11"}, {0x15DD, 0xC8, "Radeon RX", "Vega 8"},
    {0x15DD, 0xC9, "Radeon RX", "Vega 11"}, {0x15DD, 0xCA, "Radeon RX", "Vega 8"},
    {0x15DD, 0xCB, "Radeon RX", "Vega 3"},  {0x15DD, 0xCC, "Radeon RX", "Vega 6"},
    {0x15DD, 0xCE, "Radeon RX", "Vega 3"},  {0x15DD, 0xCF, "Radeon RX", "Vega 3"},
    {0x15DD, 0xD0, "Radeon RX", "Vega 10"}, {0x15DD, 0xD1, "Radeon RX", "Vega 8"},
    {0x15DD, 0xD3, "Radeon RX", "Vega 11"}, {0x15DD, 0xD5, "Radeon RX", "Vega 8"},
    {0x15DD, 0xD6, "Radeon RX", "Vega 11"}, {0x15DD, 0xD7, "Radeon RX", "Vega 8"},
    {0x15DD, 0xD8, "Radeon RX", "Vega 3"},  {0x15DD, 0xD9, "Radeon RX", "Vega 6"},
    {0x15DD, 0xE1, "Radeon RX", "Vega 3"},  {0x15DD, 0xE2, "Radeon RX", "Vega 3"},
    {"Radeon RX", "Raven Graphics"},
};

static const AmdAsicBrandingTableEntry picassoBrandingTable[] = {
    {0x15D8, 0x00, "Radeon RX", "Vega 8 WS"}, {0x15D8, 0x91, "Radeon RX", "Vega 3"},
    {0x15D8, 0x92, "Radeon RX", "Vega 3"},    {0x15D8, 0x93, "Radeon RX", "Vega 1"},
    {0x15D8, 0xA1, "Radeon RX", "Vega 10"},   {0x15D8, 0xA2, "Radeon RX", "Vega 8"},
    {0x15D8, 0xA3, "Radeon RX", "Vega 6"},    {0x15D8, 0xA4, "Radeon RX", "Vega 3"},
    {0x15D8, 0xB1, "Radeon RX", "Vega 10"},   {0x15D8, 0xB2, "Radeon RX", "Vega 8"},
    {0x15D8, 0xB3, "Radeon RX", "Vega 6"},    {0x15D8, 0xB4, "Radeon RX", "Vega 3"},
    {0x15D8, 0xC1, "Radeon RX", "Vega 10"},   {0x15D8, 0xC2, "Radeon RX", "Vega 8"},
    {0x15D8, 0xC3, "Radeon RX", "Vega 6"},    {0x15D8, 0xC4, "Radeon RX", "Vega 3"},
    {0x15D8, 0xC5, "Radeon RX", "Vega 3"},    {0x15D8, 0xC8, "Radeon RX", "Vega 11"},
    {0x15D8, 0xC9, "Radeon RX", "Vega 8"},    {0x15D8, 0xCA, "Radeon RX", "Vega 11"},
    {0x15D8, 0xCB, "Radeon RX", "Vega 8"},    {0x15D8, 0xCC, "Radeon RX", "Vega 3"},
    {0x15D8, 0xCE, "Radeon RX", "Vega 3"},    {0x15D8, 0xCF, "Radeon RX", "Vega 3"},
    {0x15D8, 0xD1, "Radeon RX", "Vega 10"},   {0x15D8, 0xD2, "Radeon RX", "Vega 8"},
    {0x15D8, 0xD3, "Radeon RX", "Vega 6"},    {0x15D8, 0xD4, "Radeon RX", "Vega 3"},
    {0x15D8, 0xD8, "Radeon RX", "Vega 11"},   {0x15D8, 0xD9, "Radeon RX", "Vega 8"},
    {0x15D8, 0xDA, "Radeon RX", "Vega 11"},   {0x15D8, 0xDB, "Radeon RX", "Vega 8"},
    {0x15D8, 0xDC, "Radeon RX", "Vega 3"},    {0x15D8, 0xDD, "Radeon RX", "Vega 3"},
    {0x15D8, 0xDE, "Radeon RX", "Vega 3"},    {0x15D8, 0xDF, "Radeon RX", "Vega 3"},
    {0x15D8, 0xE1, "Radeon RX", "Vega 11"},   {0x15D8, 0xE2, "Radeon RX", "Vega 9"},
    {0x15D8, 0xE3, "Radeon RX", "Vega 3"},    {0x15D8, 0xE4, "Radeon RX", "Vega 3"},
    {"Radeon RX", "Picasso Graphics"},
};

static const AmdAsicBrandingTableEntry renoirBrandingTable[] = {
    {0x1636, 0xD1, "Radeon Pro", "Graphics"},
    {0x1636, 0xD3, "Radeon Pro", "Graphics"},
    {"Radeon RX", "Renoir Graphics"},
};

const AmdAsicBrandingTableEntry* X6000FB::getGpuBrandingNameListRaven(const void*) { return ravenBrandingTable; }

const AmdAsicBrandingTableEntry* X6000FB::getGpuBrandingNameListPicasso(const void*) { return picassoBrandingTable; }

const AmdAsicBrandingTableEntry* X6000FB::getGpuBrandingNameListRenoir(const void*) { return renoirBrandingTable; }

IOReturn X6000FB::dummyIOReturnSuccess() { return kIOReturnSuccess; }

IOReturn X6000FB::getTriageHardwareDataRV(void*, const UInt32 fbIndex, void* const triageData)
{
    auto& bufferPointer = getMember<char*>(triageData, 0x0);
    auto& bufferSize    = getMember<UInt32>(triageData, 0x8);

    if (bufferSize < 2) { return kIOReturnNoResources; }
    if (fbIndex >= 4) { return kIOReturnSuccess; }

    // RV 路径四个 DCN 偏移原为苹果 DCN 1.0（Raven）值，已按 dcn_3_1_4_offset.h 修正
    // （Phoenix/780M 用；四寄存器 BASE_IDX 均=2，DCN_BASE_2=0x34C0 段基址不变）：
    //   ODM0_OPTC_INPUT_GLOBAL_CONTROL 0x1ACA，步进 0x10（:7732-7733，ODM1=0x1ADA :7752）——原本正确，保持；
    //   OTG0_OTG_MASTER_EN             0x1B5C，步进 0x80（:7904-7905，OTG1=0x1BDC :8116；旧值 0x1B5F 在 3_1_4 未定义）；
    //   HUBP0_HUBP_CLK_CNTL            0x05F4，步进 0xDC（:3434-3435，HUBP1=0x06D0 :3728；旧值 0x567 实为 DCN_VM_CONTEXT2_CNTL、步进 0xC4 错）；
    //   DIG0_DIG_BE_EN_CNTL            0x20B2，步进 0x100（:9264-9265，DIG1=0x21B2 :9618；旧值 0x20B0 实为 DIG0_AFMT_CNTL）。
    // 注：该路径仅 <=MACOS_12_X 安装（:309 门限），13.6 上不生效——属离线正确性修复（防未来启用）。
    const auto odmOptcInputGlobalControl = NRed::singleton().readReg32(DCN_BASE_2 + 0x1ACA + (0x10 * fbIndex));
    const auto otgMasterEn               = NRed::singleton().readReg32(DCN_BASE_2 + 0x1B5C + (0x80 * fbIndex));
    const auto hubpClkControl            = NRed::singleton().readReg32(DCN_BASE_2 + 0x05F4 + (0xDC * fbIndex));
    const auto digBeEnControl            = NRed::singleton().readReg32(DCN_BASE_2 + 0x20B2 + (0x100 * fbIndex));

    const auto chars = snprintf(bufferPointer, bufferSize, "%x %x %x %x", odmOptcInputGlobalControl, otgMasterEn,
                                hubpClkControl, digBeEnControl);
    if (chars < 0) { return kIOReturnError; }
    const auto realChars = static_cast<UInt32>(chars) > bufferSize ? bufferSize : static_cast<UInt32>(chars);

    bufferSize    -= realChars;
    bufferPointer += realChars;

    return kIOReturnSuccess;
}

IOReturn X6000FB::getTriageHardwareDataRN(void*, const UInt32 fbIndex, void* const triageData)
{
    auto& bufferPointer = getMember<char*>(triageData, 0x0);
    auto& bufferSize    = getMember<UInt32>(triageData, 0x8);

    if (bufferSize < 2) { return kIOReturnNoResources; }
    if (fbIndex >= 4) { return kIOReturnSuccess; }

    const auto odmOptcInputGlobalControl = NRed::singleton().readReg32(DCN_BASE_2 + 0x1ACA + (0x10 * fbIndex));
    const auto otgMasterEn               = NRed::singleton().readReg32(DCN_BASE_2 + 0x1B5C + (0x80 * fbIndex));
    const auto hubpClkControl            = NRed::singleton().readReg32(DCN_BASE_2 + 0x5F4 + (0xDC * fbIndex));
    const auto digBeEnControl            = NRed::singleton().readReg32(DCN_BASE_2 + 0x20B0 + (0x100 * fbIndex));

    const auto chars = snprintf(bufferPointer, bufferSize, "%x %x %x %x", odmOptcInputGlobalControl, otgMasterEn,
                                hubpClkControl, digBeEnControl);
    if (chars < 0) { return kIOReturnError; }
    const auto realChars = static_cast<UInt32>(chars) > bufferSize ? bufferSize : static_cast<UInt32>(chars);

    bufferSize    -= realChars;
    bufferPointer += realChars;

    return kIOReturnSuccess;
}

// D3: route messageAccelerator, dummy IRI send (reqType=3) on Phoenix so powerUp
// continues into its success path (TTL RTS / m_ppInitialized / FB_Boot_PPInitialized)
IOReturn X6000FB::wrapMessageAccelerator(void* const self, const UInt32 reqType, void* arg2, void* arg3, void* arg4)
{
    if (NRed::singleton().getAttributes().isPhoenix() && reqType == 3) {
        SYSLOG("X6000FB", "D3: messageAccelerator IRI (cmd=3) -> dummy success on Phoenix");
        return kIOReturnSuccess;
    }
    return FunctionCast(wrapMessageAccelerator,
                        reinterpret_cast<mach_vm_address_t>(singleton().orgMessageAccelerator))(self, reqType, arg2, arg3, arg4);
}

// 诊断：探针消息原始响应（文件作用域，供 wrapHandleCriticalError 的 panic 消息打印）
static UInt32 gProbeResp[7] = {0, 0, 0, 0, 0, 0, 0};

// ─── 第八步观测探针：dc_clk_mgr_create（精确观测点）───────────────────────────
//  为什么需要它：崩溃链是 `[[A+0x58]+0x30]+0x118`，其中 `A->f58` 由 `AmdDalHelper::powerUp`
//  **内部**创建后再复制给 A（实测：在 powerUp 入口读 `dalHelper->f48->f58` 为 0，
//  而崩溃时 `A->f58` 非空——CI run80 的探针已证）。因此要读崩溃时刻的值，
//  观测点必须落在 `dc_clk_mgr_create` 入口：该函数由 powerUp 内部在完成 `A->f58` 赋值后调用
//  （`0xff660` 赋值 → `0xff77d` 调用），故入口处读到的 `ctx->f58->f30` 就是崩溃时的值。
//  额外收益：本函数的第二个参数就是 `struct pp_smu_funcs *`（PP-SMU 侧），
//  读它即可判定"PPLIB 被抑制后 PP-SMU 侧是否真的可用"——这是决定后续路线的关键证据。
//  观测通道：panic（已验证可靠）；门控 boot-arg `-NRedStagePanic2`（与 DalHelper 探针互斥使用）。
void* X6000FB::wrapDcClkMgrCreate(void* const ctx, void* const ppSmu, void* const dccg)
{
    if (checkKernelArgument("-NRedStagePanic2")) {
        auto isKernelPtr = [](UInt64 p) -> bool { return p >= 0xffffff8000000000ULL; };
        auto load64 = [](UInt64 base, UInt64 off) -> UInt64 {
            return *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(base) + off);
        };

        const UInt64 c = reinterpret_cast<UInt64>(ctx);
        UInt64 p58 = 0, p30 = 0, b118 = 0xff;
        if (isKernelPtr(c) && isKernelPtr(load64(c, 0x58))) {
            p58 = load64(c, 0x58);
            if (isKernelPtr(load64(p58, 0x30))) {
                p30 = load64(p58, 0x30);
                b118 = *reinterpret_cast<const UInt8*>(reinterpret_cast<const UInt8*>(p30) + 0x118);
            }
        }

        const UInt64 pp = reinterpret_cast<UInt64>(ppSmu);
        UInt64 pp0 = 0, pp8 = 0, pp10 = 0, pp18 = 0;
        if (isKernelPtr(pp)) {
            pp0  = load64(pp, 0x00);
            pp8  = load64(pp, 0x08);
            pp10 = load64(pp, 0x10);
            pp18 = load64(pp, 0x18);
        }

        const UInt64 d = reinterpret_cast<UInt64>(dccg);
        const UInt64 v58 = p58, v30 = p30, v118 = b118, v0 = pp0, v8 = pp8, v10 = pp10, v18 = pp18;
        panic("NRed clk_mgr probe: ctx=%llx ctx58=%llx ctx58_30=%llx b118=%llx "
              "| pp_smu=%llx [0]=%llx [8]=%llx [10]=%llx [18]=%llx | dccg=%llx",
              c, v58, v30, v118, pp, v0, v8, v10, v18, d);
    }

    return FunctionCast(wrapDcClkMgrCreate, singleton().orgDcClkMgrCreate)(ctx, ppSmu, dccg);
}

// ─── 第八步观测探针：填充 pp_smu_funcs 的函数（终局证据）──────────────────────
//  目的：`dc_clk_mgr_create` 收到的 `pp_smu_funcs` 是**空结构**（第 2 批次实测：前 4 个字段全 0）。
//        本探针在填充函数**返回之后**读同一结构，直接回答"它到底写了没有、写了什么"。
//  注意：该函数的调用者依赖调用遗留的 ZF 标志，因此**仅在 `-NRedStagePanic3` 时才 route**
//        （见 processKext 中的条件路由），保证默认启动路径零影响。
void* X6000FB::wrapPpSmuFill(void* const ctx, void* const ppSmu)
{
    auto ret = FunctionCast(wrapPpSmuFill, singleton().orgPpSmuFill)(ctx, ppSmu);

    auto isKernelPtr = [](UInt64 p) -> bool { return p >= 0xffffff8000000000ULL; };
    auto load64 = [](UInt64 base, UInt64 off) -> UInt64 {
        return *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(base) + off);
    };

    const UInt64 c  = reinterpret_cast<UInt64>(ctx);
    const UInt64 pp = reinterpret_cast<UInt64>(ppSmu);
    UInt64 v0 = 0, v8 = 0, v18 = 0, v20 = 0, v28 = 0, v30 = 0, v40 = 0, v48 = 0, v60 = 0, v70 = 0;
    if (isKernelPtr(pp)) {
        v0  = load64(pp, 0x00);
        v8  = load64(pp, 0x08);
        v18 = load64(pp, 0x18);
        v20 = load64(pp, 0x20);
        v28 = load64(pp, 0x28);
        v30 = load64(pp, 0x30);
        v40 = load64(pp, 0x40);
        v48 = load64(pp, 0x48);
        v60 = load64(pp, 0x60);
        v70 = load64(pp, 0x70);
    }
    const UInt64 rv = reinterpret_cast<UInt64>(ret);
    panic("NRed pp_smu fill probe: ctx=%llx pp=%llx ret=%llx | [0]=%llx [8]=%llx [18]=%llx [20]=%llx "
          "[28]=%llx [30]=%llx [40]=%llx [48]=%llx [60]=%llx [70]=%llx",
          c, pp, rv, v0, v8, v18, v20, v28, v30, v40, v48, v60, v70);

    return ret;
}

// ─── 第八步观测探针：AmdDalHelper::powerUp ───────────────────────────────────
//  目的：读 `dalHelper->f48->f58->f30` 在 **powerUp 入口**的取值（结论：入口读到的是
//        尚未创建的状态——CI run80 实测 f58=0），用于确认"该字段由 powerUp 内部创建"。
//  安全：只在 boot-arg `-NRedStageMark` 存在时生效；解引用前做内核地址范围校验，探针自身绝不 panic。
UInt32 X6000FB::wrapDalHelperPowerUp(void* const self)
{
    auto isKernelPtr = [](UInt64 p) -> bool { return p >= 0xffffff8000000000ULL; };
    auto load64 = [](UInt64 base, UInt64 off) -> UInt64 {
        return *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(base) + off);
    };

    UInt64 selfAddr = 0;
    UInt64 p48 = 0, p58 = 0, p30 = 0, b118 = 0xff;

    if (StageMark::enabled() && isKernelPtr(reinterpret_cast<UInt64>(self))) {
        selfAddr = reinterpret_cast<UInt64>(self);
        if (isKernelPtr(load64(selfAddr, 0x48))) {
            p48 = load64(selfAddr, 0x48);
        }
        if (p48 != 0 && isKernelPtr(load64(p48, 0x58))) {
            p58 = load64(p48, 0x58);
        }
        if (p58 != 0 && isKernelPtr(load64(p58, 0x30))) {
            p30 = load64(p58, 0x30);
        }
        if (p30 != 0) {
            b118 = *reinterpret_cast<const UInt8*>(reinterpret_cast<const UInt8*>(p30) + 0x118);
        }

        StageMark::mark("dh-enter");
        StageMark::markHex("dh-self", selfAddr);
        StageMark::markHex("dh-f48", p48);
        StageMark::markHex("dh-f58", p58);
        StageMark::markHex("dh-f30", p30);
        if (p30 != 0) {
            StageMark::markHex("dh-b118", b118);
        }
    }

    // 诊断出口（boot-arg `-NRedStagePanic`）：用**已验证可靠**的 panic→efivarfs 通道把
    //   上面读到的指针值带出去。依据：本函数返回后必然发生 page fault（第八步第 1/2 批次
    //   实测：0x1319FD），此处只是把崩溃提前几毫秒，不改变最终结果。
    //   铁律：panic 实参**只能**是已求值的局部变量（禁调可能加锁的函数）→ 全部预先求值。
    if (checkKernelArgument("-NRedStagePanic")) {
        const UInt32 stBits = StageMark::statusBits();
        const UInt64 nvInit  = (stBits & StageMark::kStatusInitOk) ? 1 : 0;
        const UInt64 nvWrite = (stBits & StageMark::kStatusWriteOk) ? 1 : 0;
        const UInt64 nvSync  = (stBits & StageMark::kStatusSyncOk) ? 1 : 0;
        const UInt64 nvUsed  = (stBits & StageMark::kStatusUsed) ? 1 : 0;
        const UInt64 vSelf = selfAddr, v48 = p48, v58 = p58, v30 = p30, v118 = b118;
        panic("NRed DalHelper probe: self=%llx f48=%llx f58=%llx f30=%llx b118=%llx "
              "| nvram init=%llu write=%llu sync=%llu used=%llu",
              vSelf, v48, v58, v30, v118, nvInit, nvWrite, nvSync, nvUsed);
    }

    const auto ret = FunctionCast(wrapDalHelperPowerUp, singleton().orgDalHelperPowerUp)(self);
    if (StageMark::enabled()) {
        StageMark::markHex("dh-ret", ret);
    }
    return ret;
}

UInt32 X6000FB::wrapControllerPowerUp(void* const self)
{
    StageMark::mark("powerUp-enter");
    auto& m_flags  = getMember<UInt8>(self, 0x5F18);
    auto  send     = (m_flags & 2) == 0;
    m_flags       |= 4;    // All framebuffers enabled

    // C1.6: NRed 直读 MMIO 旁路 —— 在 powerUp 之前跑驱动表分配+Transfer 序列，唤醒 BGM/IMU。
    // 挂载点选择依据：本函数 100% 被调用（panic 栈铁证）；而 smu13PowerUpConfig 因 wrapper 休眠是死点。
    // 失败仅记录探针位，不改变原有行为（安全旁路）。
    // 探针位新语义（改用表地址+Transfer 合并 + 独立 EnableGfxImu）：
    //   bit20 = 表地址设置+Transfer 成功；bit21 = EnableGfxImu(0x16,1) 成功；
    //   bit24 = 表地址/Transfer 失败；bit25 = EnableGfxImu 失败；
    //   bit32-39 = 表地址/Transfer 步 rc（低8位）；bit40-47 = EnableGfxImu 步 rc（i=3 域位）。
    //
    // ⛔ 默认关闭（2026-09-25 修复"panic 后不自动重启"）
    //   背景：本旁路会向 PMFW 真实发出 SetDriverDramAddrHigh/Low(0x0D/0x0E)、
    //   TransferTableDram2Smu(0x10)、EnableGfxImu(0x16)。一旦地址被 PMFW 接受
    //   （即 e5b982a 把 fbOffset 修正为 0x8000000000、地址首次落到 VRAM 窗口内），
    //   这几条消息就真正生效，PMFW/驱动表被接管，导致 panic 流程无法完成 reboot
    //   → 表现为"panic 后停在黑屏、只能长按电源键"（第 24 次真机）。
    //   对照：第 23 次之前 fbOffset=0x8D0000000 超出窗口，0x0D 被拒(resp=0)、后续早退，
    //   PMFW 未被改动，因此仍能自动重启。
    //   机制同 §16.75（旁路注入 SMU 消息使 PMFW 异常 → 无法完成 reboot）
    //   与 §16.93/§16.95（同类 L2 状态写曾致整机卡死）。
    //   处置：本旁路属**实验性探针**，不是显示点亮的必需路径——显示时钟走
    //   VBIOSSMC（67/83/91），与 PMFW（66/82/90）是两条独立通道（见交接文档 §6.4）。
    //   故改为 boot-arg `-NRedSmuBypass` 显式启用；默认不注入，保持系统可自动重启。
    if (checkKernelArgument("-NRedSmuBypass") && NRed::singleton().getAttributes().isPhoenix()) {
        // ⭐ 探针三连（§16.39）：TestMessage(0x01) / GetPmfwVersion(0x02) / GetDriverIfVersion(0x03)
        //    目的：判定 PMFW 消息端口是否开着——这三条是最基础的消息，任何固件都应响应。
        //    全静默 → 消息端口未开；有响应 → 通道通，问题在消息内容/时序。
        {
            UInt32 pr[3] = {0, 0, 0};
            X5000HWLibs::smu13SendMsgDirect(PhoenixPPSMC::PPSMC_MSG_TestMessage, 0, &pr[0]);
            X5000HWLibs::smu13SendMsgDirect(PhoenixPPSMC::PPSMC_MSG_GetPmfwVersion, 0, &pr[1]);
            X5000HWLibs::smu13SendMsgDirect(PhoenixPPSMC::PPSMC_MSG_GetDriverIfVersion, 0, &pr[2]);
            gProbeResp[0] = pr[0];
            gProbeResp[1] = pr[1];
            gProbeResp[2] = pr[2];
            // 对照（§16.43）：blank=空白对照，inv=无效消息0xFF，rw=resp读写一致性
            gProbeResp[3] = X5000HWLibs::smu13ProbeBlank();
            gProbeResp[4] = X5000HWLibs::smu13SendMsgDirect(0xFF, 0, nullptr);
            gProbeResp[5] = X5000HWLibs::smu13ProbeRegRW();
            // arg 寄存器（版本号应在此）：GetPmfwVersion 后读 c2p82
            gProbeResp[6] = NRed::singleton().readReg32(MP0_BASE_0 + 0x292);   // §16.89 回滚: SEG0

            // ⭐ 对照实验（§16.42）：区分"真响应"与"假阳性"
            //   C1 空白对照：只清 resp，不写 msg —— 若也"成功"⇒ 判定逻辑假阳性
            //   C2 无效消息对照：发 0xFF（未定义消息）—— 正常固件应返回 Failed/UnknownCmd
            //   C3 读写一致性：写已知值 0x5A5A 到 resp 再读回 —— 验证寄存器真可写可读
            gProbeResp[3] = X5000HWLibs::smu13ProbeBlank();       // C1
            gProbeResp[4] = X5000HWLibs::smu13SendMsgDirect(0xFF, 0, nullptr) == kCAILResultOK ? 1 : 0;  // C2
            gProbeResp[5] = X5000HWLibs::smu13ProbeRegRW();       // C3
        }
        // ⚠️ 顺序对齐 Linux（amdgpu_smu.c）：EnableGfxImu 在 smu_start_smc_engine 之后、
        // driver 表地址设置/TransferTable 之前发送（查证 §16.20）——先前顺序（Imu 在 Transfer 后）
        // 导致 Imu NoResponse（Transfer 失败后 PMFW 消息环状态异常）。
        // ⓪ PowerUpVcn(0x07) + PowerUpJpeg(0x22)：Linux APU 早期初始化必发
        //    （amdgpu_smu.c:1983-85, §16.86）。非 BGM 前置但序列完备性补项。
        //    param: VCN = inst<<16（单实例→0）, Jpeg = 0
        const auto rVcn = X5000HWLibs::smu13SendMsgDirect(PhoenixPPSMC::PPSMC_MSG_PowerUpVcn, 0);
        DBGLOG("X6000FB", "D3: PowerUpVcn resp=0x%X", rVcn);
        const auto rJpeg = X5000HWLibs::smu13SendMsgDirect(PhoenixPPSMC::PPSMC_MSG_PowerUpJpeg, 0);
        DBGLOG("X6000FB", "D3: PowerUpJpeg resp=0x%X", rJpeg);
        // ① 先 EnableGfxImu(0x16, 1)
        const auto rImu = X5000HWLibs::smu13SendMsgDirect(PhoenixPPSMC::PPSMC_MSG_EnableGfxImu, 1);
        if (rImu == kCAILResultOK) {
            NRed::singleton().orSmu13ProbeState(1ULL << 21);
        }
        else {
            NRed::singleton().orSmu13ProbeState(1ULL << 25);
            NRed::singleton().orSmu13ProbeState(static_cast<UInt64>(rImu & 0xFF) << 40);
        }

        // ② 再驱动表地址 + Transfer（Linux 在 hw_setup 阶段做）
        const auto rSetup = X5000HWLibs::smu13SetupDriverTableAndTransfer();
        if (rSetup == kCAILResultOK) {
            NRed::singleton().orSmu13ProbeState(1ULL << 20);
        }
        else {
            NRed::singleton().orSmu13ProbeState(1ULL << 24);
            NRed::singleton().orSmu13ProbeState(static_cast<UInt64>(rSetup & 0xFF) << 32);
        }
    }

    auto ret       = FunctionCast(wrapControllerPowerUp, singleton().orgControllerPowerUp)(self);
    SYSLOG("X6000FB", "D3: controller::powerUp returned 0x%X (isPhoenix=%s)", ret,
           NRed::singleton().getAttributes().isPhoenix() ? "true" : "false");
    if (StageMark::enabled()) {
        // 观测：powerUp 是否**返回**（若在内部 panic，则本条不会出现）
        StageMark::markHex("powerUp-ret", ret);
    }
    if (send) { singleton().orgMessageAccelerator(self, IOFBRequestControllerEnabled, nullptr, nullptr, nullptr); }
    return ret;
}

UInt32 X6000FB::wrapHandleCriticalError(void* self, const char* fmt1, const char* fmt2, const char* fmt3)
{
    // Observe P2: always log the error detail (SYSLOG = visible regardless of debug flag)
    SYSLOG("X6000FB", "handleCriticalError: '%s' | '%s' | '%s'",
           fmt1 ? fmt1 : "(null)", fmt2 ? fmt2 : "(null)", fmt3 ? fmt3 : "(null)");

    // Probe D1 v2: 在真崩溃出口把 SMU13 序列累积状态注入 panic 消息（走已验证的 NVRAM -> .panic 落盘通道）
    // 必须置于 -NRedProbePPLIB 之前：二者同开时以 Panic 优先（先取数据）。
    // panic() 与 Apple doGPUPanic 终点同一原语（DebugEnabler.cpp:250），栈/寄存器照常写入，.panic 不残缺。
    if (checkKernelArgument("-NRedProbePanic") && NRed::singleton().getAttributes().isPhoenix()) {
        // 诊断大打包（§16.21）：panic 消息带宽足够，一次带回所有 SMU 诊断寄存器原始值
        // —— 避免反复猜地址域。全部用 NRed 直读（readReg32，dword 索引）。
        auto& nred = NRed::singleton();
        // ⚠️ 根因修正（§16.34）：NRed::readReg32 的间接分支把入参当【字节地址】写进 PCIE_INDEX2
        // （NRed.cpp:214），而分支判断按 dword 索引算。此前传 "0x3010028>>2" 导致写入
        // 0xC0400A —— 既非 dword 索引也非正确 SMN 地址 → 全 F。
        // 正确：传【完整 SMN 字节地址】= MP1_Public(0x3B00000) | smnMP1_FIRMWARE_FLAGS(0x3010024)
        constexpr UInt32 kMp1Public = 0x3B00000;   // smu_v13_0.h:33
        const UInt32 rFwFlags   = nred.readReg32(kMp1Public | 0x3010028);  // 候选 A
        const UInt32 rFwFlags24 = nred.readReg32(kMp1Public | 0x3010024);  // 候选 B（Linux 用）
        const UInt32 rScratch0  = nred.readReg32(kMp1Public | 0x3010020);  // MP1_SCRATCH0
        const UInt32 rMsg66     = nred.readReg32(MP0_BASE_0 + 0x282);    // §16.89 回滚: SEG0
        const UInt32 rMsg82     = nred.readReg32(MP0_BASE_0 + 0x292);    // §16.89 回滚: SEG0
        const UInt32 rMsg90     = nred.readReg32(MP0_BASE_0 + 0x29A);    // §16.89 回滚: SEG0
        const UInt32 rMsg91     = nred.readReg32(MP0_BASE_0 + 0x29B);    // C2PMSG_91（v11/12 旧邮箱对照）
        // fbOffset 候选地址扫描（§16.47）：一次真机读出所有候选的真值，不再逐个试。
        // 已知：Linux MMHUB_BASE.segment[0]=0x0001A000（*_ip_offset.h），regMMMC_VM_FB_OFFSET=0x0857
        const UInt32 rFbC0 = nred.readReg32(0x13200 + 0x0857);            // 候选A：✅正确地址（yellow_carp MMHUB_BASE seg0）
        const UInt32 rFbC1 = nred.readReg32(0x1A000 + 0x0857);            // 候选B：旧版 MMHUB_BASE（对照）
        const UInt32 rFbC2 = nred.readReg32(0x68000 + 0x0857);            // 候选C：原读法（已知无效）
        const UInt32 rFbC3 = nred.readReg32(0x3B00000 | 0x0857);          // 候选D
        // fbOffset 验证（§16.47）：读正确地址 0x13200+0x0857，确认不再是 ffffffff
        // BAR0 途径（§16.48）：Linux 优先用 pci_resource_start(pdev,0) 作 aper_base
        const UInt64 rBar0 = nred.getFbOffset();   // 若走 BAR0 分支，这里就是 BAR0 物理地址
        const UInt32 rFbNew = nred.readReg32(0x13200 + 0x0857);   // MMHUB 寄存器（应全F）
        const UInt32 rFbOld = nred.readReg32(0x68000 + 0x0857);   // ⛔ 旧地址（对照，应仍全F）
        const UInt32 rFbOffRaw  = rFbOld;      // MMHUB regMMMC_VM_FB_OFFSET（旧，保留字段名）
        const UInt32 rScratch4  = nred.readReg32(kMp1Public | 0x3010060); // MP1_EXT_SCRATCH4
        const UInt32 rFwVer       = nred.readReg32(kMp1Public | 0x3010004); // 固件版本（若存在）
        const UInt32 rMp1Scratch0 = rScratch0;                            // 同上（MP1_SCRATCH0）
        const UInt64 fbOff      = nred.getFbOffset();
        const UInt64 probeState = nred.getSmu13ProbeState();
        // §16.68 事故修复：panic 参数**必须**是已求值的局部变量——
        //   禁止在 panic(...) 实参里调用 singleton() 等可能加锁的函数
        //   （崩溃上下文多 CPU 已停、锁状态未知 → 死锁 → 系统挂死需手动强关）
        const UInt32 respHi   = nred.smu13Resp[0];
        const UInt32 respLo   = nred.smu13Resp[1];
        const UInt32 respXfer = nred.smu13Resp[2];
        // §16.70 响应矩阵（全部预读为局部变量——铁律 0a：panic 实参禁调 singleton()）
        const UInt32 mxDifRc = nred.smu13Resp[3], mxDifArg = nred.smu13Resp[6];
        const UInt32 mxAH0Rc = nred.smu13Resp[4], mxAH0Arg = nred.smu13Resp[7];
        const UInt32 mxAH8Rc = nred.smu13Resp[5];

        panic("NRed SMU13 state=%llx | fwflag28=%x fwflag24=%x c2p66=%x c2p82=%x c2p90=%x c2p91=%x "
              "fbOffRaw=%x fbOff=%llx scratch4=%x mp1s0=%x fwver=%x | PB tm=%x pmfw=%x dif=%x "
              "blank=%x inv=%x rw=%x arg=%x | FB c0=%x c1=%x c2=%x c3=%x bar0=%llx | "
              "RESP hi=%x lo=%x xfer=%x | "
              "orig1:%s | orig2:%s | orig3:%s",
            probeState, rFwFlags, rFwFlags24, rMsg66, rMsg82, rMsg90, rMsg91,
            rFbOffRaw, fbOff, rScratch4, rMp1Scratch0, rFwVer,
            gProbeResp[0], gProbeResp[1], gProbeResp[2],
            gProbeResp[3], gProbeResp[4], gProbeResp[5], gProbeResp[6],
            rFbC0, rFbC1, rFbC2, rFbC3, rBar0,
            respHi, respLo, respXfer,
            fmt1 ? fmt1 : "(null)", fmt2 ? fmt2 : "(null)", fmt3 ? fmt3 : "(null)");
        // panic 不返回
    }

    // Probe P1 behaviour (suppress panic) only under -NRedProbePPLIB on Phoenix
    if (checkKernelArgument("-NRedProbePPLIB") && NRed::singleton().getAttributes().isPhoenix()) {
        return 0;   // suppress panic, let powerUp continue (probe P1)
    }
    // Default: call original (panic as usual, but failure detail already logged above)
    return FunctionCast(wrapHandleCriticalError, singleton().orgHandleCriticalError)(self, fmt1, fmt2, fmt3);
}

void X6000FB::wrapDpReceiverPowerCtrl(void* const link, const bool powerOn)
{
    FunctionCast(wrapDpReceiverPowerCtrl, singleton().orgDpReceiverPowerCtrl)(link, powerOn);
    IOSleep(250);
}

void* X6000FB::wrapCreateObjectInfo(void* const helper, const UInt32 tableOffset)
{
    const auto ret = FunctionCast(wrapCreateObjectInfo, singleton().orgCreateObjectInfo)(helper, tableOffset);
    if (ret == nullptr) { return ret; }

    const auto infoTable = getMember<DispObjInfoTableV1*>(ret, 0x28);
    const auto n         = infoTable->pathCount;
    for (UInt8 i = 0, j = 0; i < n; i++) {
        // Skip invalid device tags
        if (infoTable->paths[i].devTag == 0) { infoTable->pathCount--; }
        else {
            infoTable->paths[j++] = infoTable->paths[i];
        }
    }

    return ret;
}

static UInt32 getTableOffset(const AmdAtomFwHelper* const biosHelper, const UInt32 index)
{
    const auto romTableOffset = static_cast<const UInt16*>(biosHelper->getImage(ATOM_ROM_TABLE_PTR, sizeof(UInt16)));
    if (romTableOffset == nullptr) { return 0; }
    const auto mdtOffset =
        static_cast<const UInt16*>(biosHelper->getImage(*romTableOffset + ATOM_ROM_DATA_PTR, sizeof(UInt32)));
    if (mdtOffset == nullptr) { return 0; }
    const auto mdt =
        static_cast<const UInt8*>(biosHelper->getImage(*mdtOffset, /*sizeof(atom_master_data_table_v2_1)*/ 0x4A));
    if (mdt == nullptr) { return 0; }
    return reinterpret_cast<const UInt16*>(mdt + sizeof(ATOMCommonTableHeader))[index];
}

AmdAtomVramInfo* X6000FB::wrapCreateVramInfo(AmdAtomFwHelper* const biosHelper, const UInt32 tableOffset)
{
    if (biosHelper == nullptr || tableOffset != 0) {
        return FunctionCast(wrapCreateVramInfo, singleton().orgCreateVramInfo)(biosHelper, tableOffset);
    }
    return AmdAtomVramInfoIGP::createVramInfoIGP(biosHelper, getTableOffset(biosHelper, 0x1E));
}

IOReturn X6000FB::wrapPopulateVramInfo(AmdAtomVramInfo* const self, AtomFirmwareInfo& fwInfo)
{ return self->populateVramInfo(fwInfo); }

IOReturn X6000FB::wrapGetVendorInfo(const void* const self, AGDCVendorInfo_t* const vendorInfo,
                                    const size_t sizeofVendorInfo)
{
    const auto ret = FunctionCast(wrapGetVendorInfo, singleton().orgGetVendorInfo)(self, vendorInfo, sizeofVendorInfo);
    if (ret == kIOReturnSuccess) [[likely]] { vendorInfo->VendorClass = kAGDCVendorClassIntegratedGPU; }
    return ret;
}

// Hack
class AppleACPIPlatformExpert : IOACPIPlatformExpert
{
    friend class X6000FB;
};

size_t X6000FB::readVfctAtomBiosImage(void* const self, UInt8* const buffer, const size_t bufferSize, const bool strict)
{
    const auto pciDevice = getMember<IOPCIDevice*>(self, 0x28);

    const auto expert = static_cast<AppleACPIPlatformExpert*>(pciDevice->getPlatform());
    if (expert == nullptr) [[unlikely]] { return 0; }

    const auto vfctData = expert->getACPITableData("VFCT", 0);
    if (vfctData == nullptr) [[unlikely]] { return 0; }

    const auto vfct = static_cast<const VFCT*>(vfctData->getBytesNoCopy());
    if (vfct == nullptr) [[unlikely]] { return 0; }

    if (sizeof(VFCT) > vfctData->getLength()) [[unlikely]] { return 0; }

    const auto deviceID = pciDevice->extendedConfigRead16(kIOPCIConfigDeviceID);
    const auto vendor   = pciDevice->extendedConfigRead16(kIOPCIConfigVendorID);
    const auto busNum   = pciDevice->getBusNumber();
    const auto devNum   = pciDevice->getDeviceNumber();
    const auto devFunc  = pciDevice->getFunctionNumber();

    for (auto offset = vfct->vbiosImageOffset; offset < vfctData->getLength();) {
        auto vHdr =
            static_cast<const GOPVideoBIOSHeader*>(vfctData->getBytesNoCopy(offset, sizeof(GOPVideoBIOSHeader)));
        if (vHdr == nullptr) [[unlikely]] { return 0; }

        const auto vContent =
            static_cast<const UInt8*>(vfctData->getBytesNoCopy(offset + sizeof(GOPVideoBIOSHeader), vHdr->imageLength));
        if (vContent == nullptr) [[unlikely]] { return 0; }

        offset += sizeof(GOPVideoBIOSHeader) + vHdr->imageLength;

        if (vHdr->imageLength != 0 && vHdr->imageLength <= bufferSize
            && (!strict || (vHdr->pciBus == busNum && vHdr->pciDevice == devNum && vHdr->pciFunction == devFunc))
            && vHdr->vendorID == vendor && vHdr->deviceID == deviceID) [[likely]]
        {
            if (singleton().validateAtomBiosImage(self, const_cast<UInt8*>(vContent), vHdr->imageLength)) [[likely]] {
                memcpy(buffer, vContent, vHdr->imageLength);
                return vHdr->imageLength;
            }
            else {
                SYSLOG("X6000FB", "BIOS Validation Failed - Reading from VFCT.");
            }
        }
        else {
            SYSLOG("NRed",
                   "VFCT image does not match, is empty or too long (pciBus: 0x%X pciDevice: 0x%X pciFunction: 0x%X "
                   "vendorID: 0x%X deviceID: 0x%X imageLength: 0x%X).",
                   vHdr->pciBus, vHdr->pciDevice, vHdr->pciFunction, vHdr->vendorID, vHdr->deviceID, vHdr->imageLength);
        }
    }

    SYSLOG("NRed", "VFCT table present but broken.");
    return 0;
}

size_t X6000FB::readVramAtomBiosImage(void* const self, UInt8* const buffer, const size_t bufferSize)
{
    const auto pciDevice = getMember<IOPCIDevice*>(self, 0x28);

    const auto bar0 =
        pciDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapWriteCombineCache | kIOMapAnywhere);
    if (bar0 == nullptr) [[unlikely]] { return 0; }

    if (bar0->getLength() == 0) [[unlikely]] {
        bar0->release();
        return 0;
    }

    const auto fb = reinterpret_cast<UInt8*>(bar0->getVirtualAddress());

    if (singleton().validateAtomBiosImage(self, fb, bufferSize)) [[likely]] {
        memcpy(buffer, fb, bufferSize);
        bar0->release();
        return bufferSize;
    }

    SYSLOG("X6000FB", "BIOS Validation Failed - Reading from VRAM.");
    bar0->release();
    return 0;
}

// TODO: See `amdgpu_device_need_post`, `amdgpu_get_bios_dgpu`, `amdgpu_get_bios_apu`.
IOReturn X6000FB::readAtomBios(void* const self)
{
    auto& biosImage = getMember<UInt8[0x10000]>(self, 0x48);
    auto  size      = singleton().readEfiAtomBiosImage(self, biosImage, sizeof(biosImage));
    if (size == 0) [[likely]] {
        size = readVfctAtomBiosImage(self, biosImage, sizeof(biosImage));
        if (size == 0) [[unlikely]] {
            size = readVramAtomBiosImage(self, biosImage, sizeof(biosImage));
            if (size == 0) [[unlikely]] {
                size = singleton().readPciAtomBiosImage(self, biosImage, sizeof(biosImage));
                if (size == 0) [[likely]] {
                    size = readVfctAtomBiosImage(self, biosImage, sizeof(biosImage), false);
                    if (size == 0) [[unlikely]] { return kIOReturnInternalError; }
                }
                else if (!singleton().validateAtomBiosImage(self, biosImage, sizeof(biosImage))) [[unlikely]] {
                    SYSLOG("X6000FB", "BIOS Validation Failed - Reading from PCI Device.");
                    return kIOReturnInternalError;
                }
            }
        }
    }
    getMember<size_t>(self, 0x10048) = size;
    return kIOReturnSuccess;
}

AmdAtomPspDirectory* X6000FB::wrapCreatePspDirectory(AmdAtomFwHelper* const biosHelper, const UInt32 tableOffset)
{
    if (biosHelper == nullptr || tableOffset != 0) {
        return FunctionCast(wrapCreatePspDirectory, singleton().orgCreatePspDirectory)(biosHelper, tableOffset);
    }
    return AmdAtomPspDirectoryDummy::create();
}
