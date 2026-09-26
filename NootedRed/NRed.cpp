// Master Logic
//
// Copyright © 2022-2025 ChefKiss. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#include <AGDP.hpp>
#include <AppleGFXHDA.hpp>
#include <Backlight.hpp>
#include <DebugEnabler.hpp>
#include <DriverInjector.hpp>
#include <GPUDriversAMD/ATOMBIOS.hpp>
#include <GPUDriversAMD/CAIL/Result.hpp>
#include <GPUDriversAMD/RavenIPOffset.hpp>
#include <GPUDriversAMD/SMU.hpp>
#include <GPUDriversAMD/TTL/SWIP/SMU.hpp>
#include <HWLibs.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_devinfo.hpp>
#include <Headers/kern_iokit.hpp>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>
#include <StageMark.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IOTypes.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <Kexts.hpp>
#include <NRed.hpp>
#include <PenguinWizardry/RuntimeMC.hpp>
#include <Regs/GC.hpp>
#include <Regs/NBIO.hpp>
#include <Regs/SMU.hpp>
#include <X5000.hpp>
#include <X6000FB.hpp>
#include <kern/clock.h>
#include <libkern/OSTypes.h>
#include <libkern/c++/OSMetaClass.h>
#include <mach/i386/vm_types.h>

static NRed moduleInstance;

NRed& NRed::singleton() { return moduleInstance; }

void NRed::init()
{
    // 第八步观测：驱动初始化已进入（NVRAM 侧，`-NRedStageMark` 时生效；崩溃也能取回）
    StageMark::mark("nred-init");
    SYSLOG("NRed", "|-----------------------------------------------------------------|");
    SYSLOG("NRed", "| Copyright 2022-2025 ChefKiss.                                   |");
    SYSLOG("NRed", "| If you've paid for this, you've been scammed. Ask for a refund! |");
    SYSLOG("NRed", "| Do not support tonymacx86. Support us, we truly care.           |");
    SYSLOG("NRed", "| Change the world for the better.                                |");
    SYSLOG("NRed", "|-----------------------------------------------------------------|");

    Backlight::singleton().init();

    lilu.onKextLoadForce(&kextRadeonX6000Framebuffer);
    lilu.onKextLoadForce(&kextRadeonX5000HWLibs);
    lilu.onKextLoadForce(&kextRadeonX5000);
    lilu.onKextLoadForce(&kextAGDP);
    lilu.onKextLoadForce(&kextAppleGFXHDA);

    lilu.onPatcherLoadForce(
        [](void* const, KernelPatcher& patcher)
        {
            singleton().processPatcher();
            DriverInjector::singleton().processPatcher(patcher);
            PenguinWizardry::RuntimeMCManager::singleton().processPatcher(patcher);
        },
        nullptr);

    lilu.onKextLoadForce(
        nullptr, 0,
        [](void* const, KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide, const size_t size)
        {
            AGDP::singleton().processKext(patcher, id, slide, size);
            Backlight::singleton().processKext(patcher, id, slide, size);
            DebugEnabler::singleton().processKext(patcher, id, slide, size);
            X6000FB::singleton().processKext(patcher, id, slide, size);
            AppleGFXHDA::singleton().processKext(patcher, id, slide, size);
            X5000HWLibs::singleton().processKext(patcher, id, slide, size);
            X5000::singleton().processKext(patcher, id, slide, size);
        },
        nullptr);
}

void NRed::hwLateInit()
{
    if (this->rmmio != nullptr) { return; }

    this->iGPU->setMemoryEnable(true);
    this->iGPU->setBusMasterEnable(true);

    this->rmmio =
        this->iGPU->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5, kIOMapInhibitCache | kIOMapAnywhere);
    PANIC_COND(this->rmmio == nullptr || this->rmmio->getLength() == 0, "NRed", "Failed to map RMMIO");
    this->rmmioPtr = reinterpret_cast<volatile UInt32*>(this->rmmio->getVirtualAddress());

    // ── fbOffset：VRAM 基址（消费方要的是"VRAM 起始"，不是"某个映射窗口地址"）
    //    —— 见 ROADMAP §5.4「简化项 15」及其详述 ──
    // ⛔ 两版旧读法都已被证伪，均已移除：
    //   ① 读 BAR0（PCI config 0x10/0x14）：BAR 是 **OS 枚举时分配的映射窗口**，随 OS 而变
    //      （Manjaro 0x8000000000 / macOS 0x8D0000000），**不是** VRAM 基址。
    //   ② 读 MMHUB 的 MC_VM_FB_OFFSET：其绝对地址取决于 **IP discovery 上报的 MMHUB 基址**
    //      （amdgpu_discovery.c：reg_offset[hw_ip][inst] = ip->base_address），**不是**静态表里的 0x13200；
    //      由静态表推导出的四个候选地址，在 macOS（第 25 次真机 panic 日志的 FB c0..c3）与 Manjaro
    //      （只读实测）**两侧都读回 0xFFFFFFFF** ⇒ 那条路走不通。
    // ✅ 现做法：**向 Apple 要** —— `IOFramebuffer::getVRAMRange()` 就是 Apple 自己对 VRAM 范围的认定，
    //    与消费方（Apple 自己的地址换算 wrapAdjustVRAMAddress）**天然同源**；
    //    由 X5000::fixedGetDisplayInfo 捕获（该钩子本就在用它，此前只当布尔用）。
    //    Fallback：固件分配的 UMA carve-out 基址 —— **与 OS 无关**（Manjaro dmesg 实测
    //    `VRAM: 4096M 0x8000000000-0x80FFFFFFFF` 与之吻合）。见 ROADMAP §5.4「简化项 16」。
    if (this->attributes.isPhoenix()) {
        constexpr UInt64 kFirmwareCarveOutBase = 0x8000000000ULL;    // §简化项 16
        this->fbOffset = this->fbLocationBase != 0 ? this->fbLocationBase : kFirmwareCarveOutBase;
        DBGLOG("NRed", "fbOffset 来源=%s value=0x%llX",
               this->fbLocationBase != 0 ? "Apple getVRAMRange" : "固件 carve-out fallback(简化项16)",
               this->fbOffset);
    }
    else {
        this->fbOffset = static_cast<UInt64>(this->readReg32(GC_BASE_0 + MC_VM_FB_OFFSET) & 0xFFFFFF) << 24;
    }
    // devRevision 读 RCC_STRAP1 地址块的 RCC_DEV0_EPF0_STRAP0（0xD20 + 0x15 = 0xD35），
    // 对齐 Linux nbio_v7_11_get_rev_id()：读 regRCC_STRAP1_RCC_DEV0_EPF0_STRAP0
    // （= 0x15，BASE_IDX = 2，nbio_7_11_0_offset.h:8818-8819；段基址 0xD20 见 yellow_carp_offset.h:975），
    // 位域复用 STRAP0 前缀的 STRAP_ATI_REV_ID_DEV0_F0（SHIFT=0x18 / MASK=0x0F000000L，
    // nbio_7_11_0_sh_mask.h:55905/55913、amdgpu/nbio_v7_11.c:43-44）。
    this->devRevision = (this->readReg32(NBIO_BASE_2 + RCC_STRAP1_RCC_DEV0_EPF0_STRAP0) & RCC_DEV0_EPF0_STRAP0_ATI_REV_ID_MASK)
                        >> RCC_DEV0_EPF0_STRAP0_ATI_REV_ID_SHIFT;

    if (this->attributes.isPhoenix()) {
        // Phoenix(RDNA3): Raven 系 devRevision 判断不适用，enumRevision 固定为真机确认值
        this->enumRevision = 0xA1;  // TODO: 真机读 RCC_STRAP1_RCC_DEV0_EPF0_STRAP0（0xD35）确认
    }
    else if (this->attributes.isRenoir() && !this->attributes.isPhoenix()) {
        if (!this->attributes.isGreenSardine() && this->devRevision == 0 && this->pciRevision >= 0x80
            && this->pciRevision <= 0x84)
        {
            this->attributes.setRenoirE();
        }
    }
    else {
        if (this->devRevision >= 0x8) {
            this->attributes.setRaven2();
            this->enumRevision = 0x79;
        }
        else if (this->attributes.isPicasso()) {
            this->enumRevision = 0x41;
        }
        else if (this->devRevision == 1) {
            this->enumRevision = 0x20;
        }
        else {
            this->enumRevision = 0x1;
        }
    }

    DBGLOG("NRed", "deviceID = 0x%X", this->deviceID);
    DBGLOG("NRed", "pciRevision = 0x%X", this->pciRevision);
    DBGLOG("NRed", "fbOffset = 0x%llX", this->fbOffset);
    DBGLOG("NRed", "devRevision = 0x%X", this->devRevision);
    DBGLOG("NRed", "isPicasso = %s", this->attributes.isPicasso() ? "true" : "false");
    DBGLOG("NRed", "isRaven2 = %s", this->attributes.isRaven2() ? "true" : "false");
    DBGLOG("NRed", "isRenoir = %s", this->attributes.isRenoir() ? "true" : "false");
    DBGLOG("NRed", "isGreenSardine = %s", this->attributes.isGreenSardine() ? "true" : "false");
    DBGLOG("NRed", "isPhoenix = %s", this->attributes.isPhoenix() ? "true" : "false");
    DBGLOG("NRed", "enumRevision = 0x%X", this->enumRevision);
}

void NRed::processPatcher()
{
    const auto devInfo = DeviceInfo::create();
    assert(devInfo != nullptr);

    devInfo->processSwitchOff();

    PANIC_COND(devInfo->videoBuiltin == nullptr, "NRed", "No iGPU detected by Lilu");
    this->iGPU = OSDynamicCast(IOPCIDevice, devInfo->videoBuiltin);
    PANIC_COND(WIOKit::readPCIConfigValue(this->iGPU, WIOKit::kIOPCIConfigVendorID) != WIOKit::VendorID::ATIAMD, "NRed",
               "iGPU is not an AMD one");

    WIOKit::renameDevice(this->iGPU, "IGPU");
    WIOKit::awaitPublishing(this->iGPU);
    UInt8 builtInBytes[] = {0x00};
    this->iGPU->setProperty("built-in", builtInBytes, sizeof(builtInBytes));
    char slotNameBytes[] = "built-in";
    this->iGPU->setProperty("AAPL,slot-name", slotNameBytes, sizeof(slotNameBytes));
    char hdaGfxBytes[] = "onboard-1";
    this->iGPU->setProperty("hda-gfx", hdaGfxBytes, sizeof(hdaGfxBytes));

    this->deviceID = static_cast<UInt16>(WIOKit::readPCIConfigValue(this->iGPU, WIOKit::kIOPCIConfigDeviceID));
    switch (this->deviceID) {
        case 0x15D8: {
            this->attributes.setPicasso();
        } break;
        case 0x15DD: {
        } break;
        case 0x164C:
        case 0x1636: {
            this->attributes.setRenoir();
            this->enumRevision = 0x91;
        } break;
        case 0x15E7:
        case 0x1638: {
            this->attributes.setRenoir();
            this->attributes.setGreenSardine();
            this->enumRevision = 0xA1;
        } break;
        case 0x15BF: {  // Phoenix (Radeon 780M, RDNA3)
            this->attributes.setPhoenix();
            this->attributes.setRenoir();   // 仿 Renoir 路径（NootedRed 最成熟分支）
            this->enumRevision = 0xA1;      // 待真机确认实际值
        } break;
        default: PANIC("NRed", "Unknown device ID: 0x%X", this->deviceID);
    }
    this->pciRevision = static_cast<UInt8>(WIOKit::readPCIConfigValue(this->iGPU, WIOKit::kIOPCIConfigRevisionID));

    char name[128];
    for (size_t i = 0, ii = 0; i < devInfo->videoExternal.size(); i++) {
        auto device = OSDynamicCast(IOPCIDevice, devInfo->videoExternal[i].video);
        if (device == nullptr) { continue; }

        snprintf(name, arrsize(name), "GFX%zu", ii++);
        WIOKit::renameDevice(device, name);
        WIOKit::awaitPublishing(device);
    }

    DeviceInfo::deleter(devInfo);
}

void NRed::setProp32(const char* const key, const UInt32 value) const { this->iGPU->setProperty(key, value, 32); }

UInt32 NRed::readReg32(const UInt32 reg) const
{
    if ((reg * sizeof(UInt32)) < this->rmmio->getLength()) { return this->rmmioPtr[reg]; }
    else {
        this->rmmioPtr[PCIE_INDEX2] = reg;
        return this->rmmioPtr[PCIE_DATA2];
    }
}

void NRed::writeReg32(const UInt32 reg, const UInt32 value) const
{
    if ((reg * sizeof(UInt32)) < this->rmmio->getLength()) { this->rmmioPtr[reg] = value; }
    else {
        this->rmmioPtr[PCIE_INDEX2] = reg;
        this->rmmioPtr[PCIE_DATA2]  = value;
    }
}
