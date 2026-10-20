// Plugin Entry Point and Configuration
//
// Copyright © 2022-2025 ChefKiss. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/plugin_start.hpp>
#include <NRed.hpp>
#include <IOKit/IOLib.h>

static const char* bootargOff   = "-NRedOff";
static const char* bootargDebug = "-NRedDebug";
static const char* bootargBeta  = "-NRedBeta";

PluginConfiguration ADDPR(config){
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
    &bootargOff,
    1,
    &bootargDebug,
    1,
    &bootargBeta,
    1,
    KernelVersion::Catalina,
    KernelVersion::Tahoe,
    []() { NRed::singleton().init(); },
};

// 本地构建通道（上游 clang）兼容：
// Apple clang 在 -fapple-kext 下会为 kext 生成 _realmain/_antimain 两个入口指针
// （Mach-O 符号为 __realmain/__antimain），分别指向 <PRODUCT_NAME>_kern_start/_stop；
// MacKernelSDK 的 libkmod.a（__start/__stop）通过 GOT 读取它们并调用。
// 上游 clang 不生成这两个定义，OpenCore 注入时按 0-value 拒绝。
// 取值与 Apple clang 产物一致，不改变任何运行期路径。
#if !defined(__apple_build_version__)
extern "C" kern_return_t NootedRed_kern_start(kmod_info_t *, void *);
extern "C" kern_return_t NootedRed_kern_stop(kmod_info_t *, void *);
extern "C" void* _realmain = reinterpret_cast<void*>(&NootedRed_kern_start);
extern "C" void* _antimain = reinterpret_cast<void*>(&NootedRed_kern_stop);
#endif

// 无条件超早期标记：kext 加载即执行（__mod_init_func，早于 kern_start），
// 不依赖 boot-arg、Lilu 访问判定与 trace 初始化。仅用于判定我方代码是否被执行。
__attribute__((constructor)) static void nredEarlyStartMarker() {
    IOLog("NRed: EARLY-START build=%s\n", __DATE__ " " __TIME__);
}
