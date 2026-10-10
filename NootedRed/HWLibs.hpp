// AMDRadeonX5000HWLibs Patches
//
// Copyright © 2022-2025 ChefKiss. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once
#include <GPUDriversAMD/CAIL/DeviceType.hpp>
#include <GPUDriversAMD/CAIL/HWBlock.hpp>
#include <GPUDriversAMD/CAIL/Result.hpp>
#include <GPUDriversAMD/TTL/COS.hpp>
#include <GPUDriversAMD/TTL/Event.hpp>
#include <GPUDriversAMD/TTL/SWIP/DMCU.hpp>
#include <GPUDriversAMD/TTL/SWIP/GC.hpp>
#include <GPUDriversAMD/TTL/SWIP/IPVersion.hpp>
#include <GPUDriversAMD/TTL/SWIP/SDMA.hpp>
#include <GPUDriversAMD/TTL/SWIP/SMU.hpp>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>

// ── NRedTrace 落盘通道（共享；2026-10-10 T14 从 HWLibs.cpp 提出，供 X6000FB 探针复用）──
//   nredTraceLine（定义于 HWLibs.cpp）：每次调用写**独立文件** `/var/log/NRedTrace-<seq>.log`
//   （不经 msgbuf；seq 无上限、无轮转，文件序==调用序；⚠️ rootvnode 未挂载时只 SYSLOG、不写文件）。
//   NRED_TRACE：同时写内核日志（SYSLOG→L1/L2）与上述落盘（第三通道 `NRedTrace-NNN.log`，手册 §5.7）。
void nredTraceLine(char* const buf, const int n);
#define NRED_TRACE(fmt, ...)                                                                        \
    do {                                                                                            \
        SYSLOG("HWLibs", fmt, ##__VA_ARGS__);                                                       \
        char _tb[256];                                                                              \
        const int _tn = snprintf(_tb, sizeof(_tb), fmt "\n", ##__VA_ARGS__);                        \
        nredTraceLine(_tb, _tn);                                                                    \
    } while (0)
#include <PenguinWizardry/ObjectField.hpp>

class IOBufferMemoryDescriptor;  // 仅作指针成员前向声明；定义见 <IOKit/IOMemoryDescriptor.h>

class X5000HWLibs
{
    using t_createFirmware = void*(const void* data, UInt32 size, UInt32 ipVersion, const char* filename);
    using t_putFirmware    = bool(void* self, AMDDeviceType deviceType, void* fw);

    ObjectField<void*>                                           fwDirField;
    ObjectField<UInt32>                                          pspBootloaderVersionField;
    ObjectField<UInt8>                                           pspSecurityCapsField;
    ObjectField<UInt32>                                          pspTOSVersionField;
    ObjectField<UInt8*>                                          pspCommandDataField;
    ObjectField<bool>                                            smuSwInitialisedFieldBase;
    ObjectField<void*>                                           smuInternalSWInitField;
    ObjectField<void*>                                           smuFullscreenEventField;
    void*                                                        smuCtxCache{nullptr};
    bool                                                         smu13InitAttempted{false};
    bool                                                         smu13SendGateDisabled{false};
    bool                                                         smu13ProbeInjectDisabled{true};
    bool                                                         smu13DirectEnabled{false};   // T15 方案II 直通门控（默认关）
    bool                                                         smu13FwBringupEnabled{false};   // A-1 固件层第一增量门控（默认关）
    bool                                                         smu13SegReadoutEnabled{false};   // D-3 只读仪表门控（默认关）
    // 不释放（SMU 通过 Transfer 后仍指向该 DRAM 地址）。
    IOBufferMemoryDescriptor*                                    smu13MetricsBuffer{nullptr};
    ObjectField<void*>                                           smuGetUCodeConstsField;
    ObjectField<void*>                                           smuInternalHWInitField;
    ObjectField<void*>                                           smuNotifyEventField;
    ObjectField<void*>                                           smuInternalSWExitField;
    ObjectField<void*>                                           smuInternalHWExitField;
    ObjectField<void*>                                           smuFullAsicResetField;
    ObjectField<GCFirmwareInfo>                                  gcSwFirmwareField;
    ObjectField<UInt32>                                          dmcuEnablePSPFWLoadField;
    ObjectField<UInt32>                                          dmcuABMLevelField;
    ObjectField<bool (*)(void* ctx, const SDMAFWConstant** out)> sdmaGetFwConstantsField;
    ObjectField<bool (*)(void* ctx)>                             sdmaStartEngineField;
    mach_vm_address_t                                            orgGetIpFw{0};
    t_createFirmware*                                            orgCreateFirmware{nullptr};
    t_putFirmware*                                               orgPutFirmware{nullptr};
    mach_vm_address_t                                            orgPspCmdKmSubmit{0};
    mach_vm_address_t                                            orgSmuInitFunctionPointerList{0};
    mach_vm_address_t                                            orgSmu90SendMessageWithParameter{0};
    mach_vm_address_t                                            orgGcSetFwEntryInfo{0};
    mach_vm_address_t                                            orgTtlQuery{0};   // 第八步观测：TTL 的跨 kext 查询函数（HWLibs vm 0x90ddc）
    // 第八步观测（2026-09-28 第十九轮）：TTL 初始化 `0x8b10e` 的失败出口定位（只读）
    mach_vm_address_t                                            orgTtlCollect{0};   // 0x8b5de 收集器
    mach_vm_address_t                                            orgTtlAllocA{0};    // 0x93880
    mach_vm_address_t                                            orgTtlAllocB{0};    // 0x928f4
    mach_vm_address_t                                            orgTtlCheckD{0};    // 0xa285f
    mach_vm_address_t                                            orgTtlRegIface{0};  // 0x8b9a5 接口表注册（idx/值）
    mach_vm_address_t                                            orgTlsCreate{0};    // 0x93f76 TlsCreateInstance 判据
    mach_vm_address_t                                            orgTlsSwInit{0};    // 0x95e9f TlsSwInit 内部创建
    mach_vm_address_t                                            kcSlide{0};         // 本 kext 运行时 slide（供探针换算内部全局地址）
    // 第八步观测（2026-09-28 第二十五轮）：`bgm_create` 内层步骤（只读）
    mach_vm_address_t                                            orgBgmInit{0};      // 0x2a9f0f
    mach_vm_address_t                                            orgBgmStep1{0};     // 0x29939f
    mach_vm_address_t                                            orgBgmQuery{0};     // 0x29a159
    mach_vm_address_t                                            orgBgmStep4{0};     // 0x2aa5bc
    mach_vm_address_t                                            orgReadSel{0};      // 0x29a19e（selector 7/8 读取）
    mach_vm_address_t                                            orgCfgRead{0};      // 0x2ab398（写后读回，回填结构）
    mach_vm_address_t                                            orgMode2Tail{0};    // 0x2ab00e（模式 2 的尾段；二值判定它是否被调用/返回什么）
    mach_vm_address_t                                            orgSdmaInitFunctionPointerList{0};
    CAILResult (*smu90SendMessageWithParameter)(void* ctx, UInt32 message, UInt32 param){nullptr};
    CAILResult (*smuCosWaitFor)(void* ctx, CosWaitForFunc* func, void* handle, UInt32 duration){nullptr};
    UInt32     (*smuCgsReadRegister)(void* ctx, UInt32 regOff, UInt32 blockInstance, CAILHWBlock block,
                                     UInt32 regOffBase){nullptr};
    void   (*smuCgsWriteRegister)(void* ctx, UInt32 regOff, UInt32 blockInstance, UInt32 regValue, CAILHWBlock block,
                                  UInt32 regOffBase){nullptr};
    UInt32 (*sdmaCgsReadRegister)(void* ctx, UInt32 regOff, UInt32 blockInstance, CAILHWBlock block){nullptr};
    void   (*sdmaCgsWriteRegister)(void* ctx, UInt32 regOff, UInt32 blockInstance, UInt32 regValue,
                                   CAILHWBlock block){nullptr};

public:
    static X5000HWLibs& singleton();

    X5000HWLibs();

    void processKext(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size);

private:
    static void       wrapPopulateFirmwareDirectory(void* self);
    static bool       wrapGetIpFw(void* self, UInt32 ipVersion, const char* name, void* out);
    static CAILResult pspIsSosRunning();
    static CAILResult retUnsupported();
    static CAILResult retOK();
    static CAILResult pspBootloaderLoadSos10(void* ctx);
    static CAILResult pspSecurityFeatureCapsSet10(void* ctx);
    static CAILResult pspSecurityFeatureCapsSet12(void* ctx);
    static CAILResult wrapPspCmdKmSubmit(void* ctx, void* cmd, void* outData, void* outResponse);
    static void*      wrapTtlQuery(void* buf, UInt32 id, UInt32 flags, UInt64* out);   // 第八步观测（TTL 查询）
    // 第八步观测（2026-09-28 第十九轮）：`0x8b10e` 的 4 个候选失败出口判据函数（只读）
    static void* wrapTtlCollect(void* ctx);
    static void* wrapTtlAllocA(void* ctx);
    static void* wrapTtlAllocB(void* ctx);
    static bool  wrapTtlCheckD(void* obj, void* param);
    // 第八步观测（2026-09-28 第二十一轮）：接口表注册函数 `0x8b9a5(table, idx, obj, val)`（只读）
    static void wrapTtlRegIface(void* table, UInt32 idx, void* obj, void* val);
    // 第八步观测（2026-09-28 第二十三轮）：TlsCreateInstance(0x93f76) 与 TlsSwInit 内部创建(0x95e9f)（只读）
    static bool  wrapTlsCreate(void* obj, void* slots);
    static void* wrapTlsSwInit(void* obj);
    // 第八步观测（2026-09-28 第二十五轮）：`bgm_create` 内层 4 个步骤（只读）
    static UInt32 wrapBgmInit(void* a, void* b, void* c, void* d, void* e);
    static UInt32 wrapBgmStep1(void* a, void* b, void* c, void* d);
    static UInt32 wrapBgmQuery(void* a, void* b, void* c);
    static UInt32 wrapBgmStep4(void* a);
    static UInt32 wrapReadSel(void* obj, UInt32 sel, UInt32 a2, UInt32 a3, void* buf, UInt32 a5);
    static UInt32 wrapCfgRead(void* obj, UInt32 id, UInt64* out, UInt32* flag);
    static UInt32 wrapMode2Tail(void* a, void* b, void* c, void* d, void* e);
    CAILResult        smuSendMessage(void* ctx, UInt32 message, UInt32 param = 0, UInt32* outParam = nullptr) const;
    static CAILResult smuPowerUpConfigCommon(void* ctx);
    static CAILResult smuInternalSwInit(void* ctx, void* input, AMDSMUSWInitOutput* output);
    static CAILResult smuInternalSwInitOld(void* ctx, void* input, AMDSMUSWInitOutput* output);
    static CAILResult smuGetUCodeConsts(void* ctx, AMDSMUUCodeConstants* consts);
    static CAILResult smu10PowerUpConfig(void* ctx);
    static CAILResult smu10InternalHwInit(void* ctx);
    static bool       smu12IsFwLoaded(void* ctx);
    static CAILResult smu12WaitForFwLoaded(void* ctx);
    static CAILResult smu12PowerUpConfig(void* ctx);
    static CAILResult smu12InternalHwInit(void* ctx);
    static bool       smu13IsFwLoaded(void* ctx);
    static CAILResult smu13WaitForFwLoaded(void* ctx);
    static CAILResult smu13PowerUpConfig(void* ctx);
    static CAILResult smu13InternalHwInit(void* ctx);
    static CAILResult smu13NotifyEvent(void* ctx, TTLEventInput* input);
    static CAILResult smu13FullAsicReset(void* ctx, void* data);
public:
    // NRed 直读 MMIO 绕过 Apple SMU ctx 的 PMFW 消息发送（Phoenix 上电序列旁路）
    // public：供 X6000FB::wrapControllerPowerUp 调用（该函数 100% 被调用，而 smu13PowerUpConfig 死点）
    static CAILResult smu13SendMsgDirect(UInt32 msgId, UInt32 param, UInt32 *rawResp = nullptr);
    // 判别性验证辅助（§16.43）：空白对照 / resp 寄存器读写一致性
    static UInt32 smu13ProbeBlank();
    static UInt32 smu13ProbeRegRW();
    // 分配物理连续 256B 缓冲，通知 SMU 驱动表真实 DRAM 地址（0x0D/0x0E）并 Transfer（0x10, TABLE_SMU_METRICS=7）。
    // 缓冲存入 smu13MetricsBuffer 持有，不释放。public：供 X6000FB::wrapControllerPowerUp 调用。
    static CAILResult smu13SetupDriverTableAndTransfer();
private:
    static CAILResult wrapSmu90SendMessageWithParameter(void* ctx, UInt32 message, UInt32 param);
    static CAILResult smuInternalHwExit(void* ctx);
    static CAILResult smuFullAsicReset(void* ctx, void* data);
    static CAILResult smu10NotifyEvent(void* ctx, TTLEventInput* input);
    static CAILResult smu12NotifyEvent(void* ctx, TTLEventInput* input);
    static CAILResult smuFullScreenEvent(void* ctx, TTLFullScreenEvent event);
    static CAILResult wrapSmuInitFunctionPointerList(void* ctx, SWIPIPVersion ipVersion);
    static void       gc91GetFwConstants(void* ctx, GCFirmwareInfo* fwData);
    static void       gc92GetFwConstants(void* ctx, GCFirmwareInfo* fwData);
    static void       gc93GetFwConstants(void* ctx, GCFirmwareInfo* fwData);
    static void       processGCFWEntries(void* ctx, void* initData);
    static CAILResult wrapGcSetFwEntryInfo(void* ctx, SWIPIPVersion ipVersion, void* initData);
    static bool       getDcn1FwConstants(void* ctx, DMCUFirmwareInfo* fwData);
    static bool       getDcn21FwConstants(void* ctx, DMCUFirmwareInfo* fwData);
    static bool       sdma412StartEngine(void* ctx);
    static CAILResult wrapSdmaInitFunctionPointerList(void* ctx, UInt32 major, UInt32 minor, UInt32 patch);

    // VBIOSSMC send primitive and display clock wrappers (DCN 3.1.4)
    static UInt32 vbiossmcSendMsg(void* ctx, UInt32 msgId, UInt32 paramMHz);
    static SInt32 vbiossmcSetDispclk(void* ctx, UInt32 requestedKhz);
    static SInt32 vbiossmcSetDppclk(void* ctx, UInt32 requestedKhz);
    static SInt32 vbiossmcSetDprefclk(void* ctx, UInt32 requestedKhz);
    static SInt32 vbiossmcSetHardMinDcfclk(void* ctx, UInt32 requestedKhz);
    static SInt32 vbiossmcSetMinDeepSleepDcfclk(void* ctx, UInt32 requestedKhz);
    static void   vbiossmcSetDisplayIdleOptimizations(void* ctx, UInt32 idleInfo);
    static void   vbiossmcSetDisplayCount(void* ctx, UInt32 count);
public:
    static SInt32 vbiossmcSetDispclkCached(UInt32 requestedKhz);
    static SInt32 vbiossmcSetDppclkCached(UInt32 requestedKhz);

    // ── 供 DCN314 显示时钟主流程的内核态消费者使用（第五步）────────────────
    // 为什么不把 smuCgsRead/WriteRegister 直接暴露成 public：消费方只需要
    // "按段内偏移读写一段寄存器"这一件事，故在此给出最小面（并统一做空指针兜底）。
    //   smuContext()      : 苹果 SMU 上下文；为空表示 SMU 尚未初始化（消费方应放弃下发）
    //   cgsReadReg/cgsWriteReg: 转发到苹果按 block 查找基址的 MMIO 通道
    static void*  smuContext();
    static UInt32 cgsReadReg(void* ctx, UInt32 off, UInt32 blockInstance, CAILHWBlock block, UInt32 regOffBase);
    static void   cgsWriteReg(void* ctx, UInt32 off, UInt32 val, UInt32 blockInstance, CAILHWBlock block,
                              UInt32 regOffBase);
};
