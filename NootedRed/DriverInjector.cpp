// IOKit Personality Injector
//
// Copyright © 2025 ChefKiss. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#include <DriverInjector.hpp>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>
#include <libkern/OSTypes.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSObject.h>
#include <libkern/c++/OSString.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/pci/IOPCIDevice.h>

static DriverInjector moduleInstance;

static const char com_apple_kext_AMDRadeonX5000[] = {
#embed "Personalities/com.apple.kext.AMDRadeonX5000.xml" suffix(, '\0')
};
static const char com_apple_kext_AMDRadeonX5000HWServices[] = {
#embed "Personalities/com.apple.kext.AMDRadeonX5000HWServices.xml" suffix(, '\0')
};
static const char com_apple_kext_AMDRadeonX6000Framebuffer[] = {
#embed "Personalities/com.apple.kext.AMDRadeonX6000Framebuffer.xml" suffix(, '\0')
};
static const char com_apple_driver_AppleGFXHDA[] = {
#embed "Personalities/com.apple.driver.AppleGFXHDA.xml" suffix(, '\0')
};

DriverInjector::DriverInjector() :
    drivers{
        Driver("com.apple.kext.AMDRadeonX6000Framebuffer", com_apple_kext_AMDRadeonX6000Framebuffer),
        Driver("com.apple.driver.AppleGFXHDA", com_apple_driver_AppleGFXHDA),
        Driver("com.apple.kext.AMDRadeonX5000", com_apple_kext_AMDRadeonX5000),
        Driver("com.apple.kext.AMDRadeonX5000HWServices", com_apple_kext_AMDRadeonX5000HWServices),
    }
{ }

DriverInjector& DriverInjector::singleton() { return moduleInstance; }

void DriverInjector::processPatcher(KernelPatcher& patcher)
{
    KernelPatcher::RouteRequest request{"__ZN11IOCatalogue10addDriversEP7OSArrayb", wrapAddDrivers,
                                        this->orgAddDrivers};
    PANIC_COND(!patcher.routeMultipleLong(KernelPatcher::KernelID, &request, 1), "DriverInjector",
               "Failed to route addDrivers");
}

bool DriverInjector::wrapAddDrivers(void* const self, OSArray* const array, const bool doNubMatching)
{
    UInt32 driverCount = array->getCount();
    for (UInt32 driverIndex = 0; driverIndex < driverCount; driverIndex += 1) {
        OSObject* object = array->getObject(driverIndex);
        if (object == nullptr) { continue; }
        auto* dict = OSDynamicCast(OSDictionary, object);
        if (dict == nullptr) { continue; }
        auto* bundleIdentifier = OSDynamicCast(OSString, dict->getObject("CFBundleIdentifier"));
        if (bundleIdentifier == nullptr || bundleIdentifier->getLength() == 0) { continue; }

        const auto toInjectCount = checkKernelArgument("-NRedNoAccel") ? 2 : arrsize(singleton().drivers);
        for (size_t identifierIndex = 0; identifierIndex < toInjectCount; identifierIndex += 1) {
            auto& driver = singleton().drivers[identifierIndex];

            if ((singleton().matchedDrivers & getBit(identifierIndex)) != 0
                || !bundleIdentifier->isEqualTo(driver.identifier))
            {
                continue;
            }

            singleton().matchedDrivers |= getBit(identifierIndex);

            DBGLOG("DriverInjector", "Matched %s, injecting.", driver.identifier);

            array->merge(driver.personalities);

            break;
        }
    }

    // ─── 补"点火"属性：`LoadAccelerator`（门控 `-NRedSetLoadAccel`，默认关闭）────────
    //  依据（2026-09-26 离线反汇编 + 真机实测，见 `kb/re/加速器注册链报告.md`）：
    //   · Apple 的 `AmdGpuWrangler::vendor_doDeviceAttribute`（Framebuffer VM 0x44b0c）会把三个
    //     属性写到 PCI 设备上：`LoadHWServices` / `LoadController` / **`LoadAccelerator`**；
    //   · 加速器 kext 的 personality（NootedRed 注入的 `com.apple.kext.AMDRadeonX5000.xml`）带
    //     `IOPropertyMatch = { LoadAccelerator = true }` ⇒ **该属性缺失时加速器不会被匹配**；
    //   · 真机实测（第 8 轮探针 `-NRedAccelExist`）：控制器实例、HWServices 实例**都存在**，
    //     而 `AMDRadeonX5000_AMDVega10GraphicsAccelerator` **零实例**、`controller+0x7960` 为空
    //     ⇒ 正是"上升级点火"这一步没有发生。
    //  ⇒ 本段在「注入 personality → 交给 IOCatalogue 匹配」**之前**，把这台显卡
    //    （`device-id == 0x15BF`）的 `LoadAccelerator` 设为真——即替 Apple 走完它本该走的那一步，
    //    **不构造任何对象**。
    //  安全：只对 `device-id` 匹配的那一个 PCI 设备设一个布尔属性；只读遍历；门控默认关闭。
    if (checkKernelArgument("-NRedSetLoadAccel")) {
        auto* iter = IORegistryIterator::iterateOver(gIOServicePlane, kIORegistryIterateRecursively);
        if (iter == nullptr) {
            SYSLOG("DriverInjector", "LoadAccelerator: failed to create registry iterator");
        }
        else {
            UInt32 found = 0;
            while (auto* entry = iter->getNextObject()) {
                auto* dev = OSDynamicCast(IOPCIDevice, entry);
                if (dev == nullptr) { continue; }
                auto* did = OSDynamicCast(OSData, dev->getProperty("device-id"));
                if (did == nullptr || did->getLength() < 4) { continue; }
                const UInt32 id = *reinterpret_cast<const UInt32*>(did->getBytesNoCopy());
                if (id != 0x15BF) { continue; }
                dev->setProperty("LoadAccelerator", kOSBooleanTrue);
                found = 1;
                SYSLOG("DriverInjector", "LoadAccelerator set on PCI device %s", dev->getName());
                break;
            }
            iter->release();
            if (found == 0) { SYSLOG("DriverInjector", "LoadAccelerator: no 0x15BF PCI device found"); }
        }
    }

    return FunctionCast(wrapAddDrivers, singleton().orgAddDrivers)(self, array, doNubMatching);
}
