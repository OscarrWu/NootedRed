// Global Kext Info Definitions
//
// Copyright © 2025 ChefKiss. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#include <Headers/kern_patcher.hpp>
#include <Kexts.hpp>

static const char* pathRadeonX6000Framebuffer =
    "/System/Library/Extensions/AMDRadeonX6000Framebuffer.kext/Contents/MacOS/AMDRadeonX6000Framebuffer";
static const char* pathRadeonX5000HWLibs = "/System/Library/Extensions/AMDRadeonX5000HWServices.kext/Contents/PlugIns/"
                                           "AMDRadeonX5000HWLibs.kext/Contents/MacOS/AMDRadeonX5000HWLibs";
// 批次 psptmrreuse v3：X6000 HWLibs（TMR 可能走这一侧）。位于 X6000HWServices 的 PlugIn 下。
static const char* pathRadeonX6000HWLibs = "/System/Library/Extensions/AMDRadeonX6000HWServices.kext/Contents/PlugIns/"
                                           "AMDRadeonX6000HWLibs.kext/Contents/MacOS/AMDRadeonX6000HWLibs";
static const char* pathRadeonX5000 = "/System/Library/Extensions/AMDRadeonX5000.kext/Contents/MacOS/AMDRadeonX5000";
static const char* pathBacklight   = "/System/Library/Extensions/AppleBacklight.kext/Contents/MacOS/AppleBacklight";
static const char* pathMCCSControl = "/System/Library/Extensions/AppleMCCSControl.kext/Contents/MacOS/AppleMCCSControl";
static const char* pathAGDP        = "/System/Library/Extensions/AppleGraphicsControl.kext/Contents/PlugIns/"
                                     "AppleGraphicsDevicePolicy.kext/Contents/MacOS/AppleGraphicsDevicePolicy";
static const char* pathAppleGFXHDA = "/System/Library/Extensions/AppleGFXHDA.kext/Contents/MacOS/AppleGFXHDA";

KernelPatcher::KextInfo kextRadeonX6000Framebuffer{
    "com.apple.kext.AMDRadeonX6000Framebuffer", &pathRadeonX6000Framebuffer, 1, {true}, {},
    KernelPatcher::KextInfo::Unloaded,
};

KernelPatcher::KextInfo kextRadeonX6000HWLibs{
    "com.apple.kext.AMDRadeonX6000HWLibs", &pathRadeonX6000HWLibs, 1, {true}, {},
    KernelPatcher::KextInfo::Unloaded,
};

KernelPatcher::KextInfo kextRadeonX5000HWLibs{
    "com.apple.kext.AMDRadeonX5000HWLibs", &pathRadeonX5000HWLibs, 1, {true}, {}, KernelPatcher::KextInfo::Unloaded,
};

KernelPatcher::KextInfo kextRadeonX5000{
    "com.apple.kext.AMDRadeonX5000", &pathRadeonX5000, 1, {true}, {}, KernelPatcher::KextInfo::Unloaded,
};

KernelPatcher::KextInfo kextAppleBacklight{
    "com.apple.driver.AppleBacklight", &pathBacklight, 1, {true}, {}, KernelPatcher::KextInfo::Unloaded,
};

KernelPatcher::KextInfo kextAppleMCCSControl{
    "com.apple.driver.AppleMCCSControl", &pathMCCSControl, 1, {true}, {}, KernelPatcher::KextInfo::Unloaded,
};

KernelPatcher::KextInfo kextAGDP{
    "com.apple.driver.AppleGraphicsDevicePolicy", &pathAGDP, 1, {true}, {}, KernelPatcher::KextInfo::Unloaded,
};

KernelPatcher::KextInfo kextAppleGFXHDA{
    "com.apple.driver.AppleGFXHDA", &pathAppleGFXHDA, 1, {true}, {}, KernelPatcher::KextInfo::Unloaded,
};
