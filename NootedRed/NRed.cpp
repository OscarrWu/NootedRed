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

    // fbOffset（GPU FB aperture 基准）——Phoenix (mmhub 3.0.2) 的寄存器在 MMHUB SMN 段：
    // base 0x68000 + regMMMC_VM_FB_OFFSET(0x0857)（Linux mmhub_3_0_2_offset.h 头注释 + L1356）。
    // ⛔ 旧读法 GC_BASE_0+0x96B 是错的（读回 0，CONFIRMED §16.18）。
    // Raven/老 asic 仍走 GC_BASE_0 + 0x96B（兼容原逻辑）。
    if (this->attributes.isPhoenix()) {
        // ✅ 正确地址（§16.47 + ROADMAP 当前状态）：
        //   MMHUB_BASE.segment[0] = 0x00013200（yellow_carp_offset.h:103，Phoenix/Yellow Carp）
        //   regMMMC_VM_FB_OFFSET  = 0x0857（mmhub_3_0_2_offset.h:1356，BASE_IDX=0）
        //   → 最终 dword 索引 = 0x13200 + 0x0857 = 0x13A57
        // ⛔ 旧读法 0x68000+0x0857 是错的：0x68000 是头文件里另一个 addressBlock
        //   （mmhub_dagbdec）的 base，不适用于本寄存器所在块 → 读回 ffffffff（第 16 次真机实证）
        // ✅ 换途径（§16.48 猜地址终结）：Linux 在 gmc_v11_0.c:697 用的是
        //   adev->gmc.aper_base = pci_resource_start(pdev, 0) —— 即 **PCI BAR0**，
        //   根本不读 MMHUB 寄存器（我们四个候选地址全 ffffffff，证明那条路走不通）。
        //   NRed 只映射了 BAR5，这里补读 BAR0（物理地址）作为 fbOffset 来源。
        // ═══ §16.98 修复（2026-09-11 23:25）：BAR0 物理地址必须用 PCI config 读 ═══
        //   问题：原用 bar0->getPhysicalAddress() 得到 0x8D0000000，
        //        但真实 VRAM 物理基址 = 0x8000000000（Manjaro dmesg + resource 实测）
        //        → 驱动表地址 fbOff+0x1000 超出 VRAM 窗口 → SetDriverDramAddrHigh 被拒(resp=0)
        //   正确方法（mac-amdgpu MacAMDGPU.cpp:574/589-591 参考实现）：
        //        pci->ConfigurationRead32(0x10, &bar0_lo);   // BAR0 低 32 位
        //        pci->ConfigurationRead32(0x14, &bar0_hi);   // BAR0 高 32 位（64-bit BAR）
        //        phys = ((uint64)bar0_hi << 32) | (bar0_lo & 0xFFFFFFF0);  // 屏蔽 [3:0] 标志位
        //   实测验证（Manjaro 读 config）：lo=0x0000000C hi=0x00000080
        //        → 0x8000000000 ✅ 与 Linux pci_resource_start(pdev,0) 一致
        UInt64 bar0Phys = 0;
        {
            UInt32 bar0Lo = 0, bar0Hi = 0;
            // IOPCIDevice 的 config 读取（IOKit: configRead32 为 IOPCIDevice 方法）
            bar0Lo = this->iGPU->configRead32(kIOPCIConfigBaseAddress0);
            bar0Hi = this->iGPU->configRead32(kIOPCIConfigBaseAddress0 + 4);
            if ((bar0Lo & 0xFFFFFFF0) != 0) {
                bar0Phys = (static_cast<UInt64>(bar0Hi) << 32) | (bar0Lo & 0xFFFFFFF0);
            }
        }
        if (bar0Phys == 0) {
            // 回退：旧途径（IOMemoryMap::getPhysicalAddress，已知在某些系统给错值）
            IOMemoryMap *bar0 = this->iGPU->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0,
                                                                        kIOMapInhibitCache | kIOMapAnywhere);
            if (bar0 != nullptr) {
                bar0Phys = bar0->getPhysicalAddress();
                bar0->release();
            }
        }
        if (bar0Phys != 0) {
            this->fbOffset = bar0Phys;      // BAR0 已是完整物理地址，不再 <<24
        }
        else {
            // 回退：MMHUB 寄存器（已知读不到，仅保留对照，值为全F时 fbOffset 为垃圾）
            constexpr UInt32 kMmhubBase   = 0x13200;
            constexpr UInt32 kFbOffOffset = 0x0857;
            const UInt32 raw = this->readReg32(kMmhubBase + kFbOffOffset);
            this->fbOffset = static_cast<UInt64>(raw & 0xFFFFFF) << 24;
        }
    }
    else {
        this->fbOffset = static_cast<UInt64>(this->readReg32(GC_BASE_0 + MC_VM_FB_OFFSET) & 0xFFFFFF) << 24;
    }
    this->devRevision = (this->readReg32(NBIO_BASE_2 + RCC_DEV0_EPF0_STRAP0) & RCC_DEV0_EPF0_STRAP0_ATI_REV_ID_MASK)
                        >> RCC_DEV0_EPF0_STRAP0_ATI_REV_ID_SHIFT;

    if (this->attributes.isPhoenix()) {
        // Phoenix(RDNA3): Raven 系 devRevision 判断不适用，enumRevision 固定为真机确认值
        this->enumRevision = 0xA1;  // TODO: 真机读 RCC_DEV0_EPF0_STRAP0 确认
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
