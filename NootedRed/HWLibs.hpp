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
// A-22：把内存环里的早期 trace 行刷盘（仅允许在**既有安全时点**调用：`NvMsgBuf::dumpNow`／
//  `NRed::init` 定时拍／panic 分片路径）。早期路径（bringupRun/tmrInit/tmrLoad/hook 早期段）
//  **禁止**调用。返回实际写出行数（0 = 无内容或未就绪；写失败保留内存，下拍重试）。
int nredTraceFlush();
// A-28：秒级标签版本（由 L2 周期拍 `dumpTick` 在 stBusy 保护区内调用；文件名与首行带 `sec=`）
int nredTraceFlush(int secTag);
// B19（2026-10-11）：早期 bringup 期间**静音** `NRED_TRACE` 的 SYSLOG（内核日志）通道。
//  根因（B19 Double-fault 归因）：bringup 挂在深栈上执行，而 `NRED_TRACE` 每条都先调 SYSLOG ⇒
//  `lilu_os_log`（内含 1 KB 栈缓冲）→ `IOLog` → `__os_log_internal` ⇒ 深栈 + 大栈耗 ⇒ 触栈保护页
//  ⇒ Double fault（CR2 = RSP−8）。A-22 当时只把**落盘通道**改为内存环，SYSLOG 通道未门控，
//  本标志补上这一遗留通道。置位时 `NRED_TRACE` **跳过 SYSLOG**、只写内存环 ⇒ 由既有安全时点
//  `nredTraceFlush`（NvMsgBuf 周期拍 `dumpTick` / `dumpNow` / panic 分片路径）刷盘 ⇒ 无诊断损失。
//  置位/复位点＝`fw::nredFwBringupHook` 入口 RAII 守卫（仅 `-NRedFwBringup` 打开时进入该函数）。
//  **默认 false ⇒ 非 bringup 路径行为逐字不变**（门控关时该分支永不成立）。
extern bool gNredTraceSyslogMuted;
#define NRED_TRACE(fmt, ...)                                                                        \
    do {                                                                                            \
        if (!gNredTraceSyslogMuted) { SYSLOG("HWLibs", fmt, ##__VA_ARGS__); }                       \
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
    bool                                                         smu13ClkLayoutEnabled{false};    // A-12/A-27（甲）线读数门控（默认关）
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
    mach_vm_address_t                                            orgPred90{0};   // B81 观测点 A：Apple 就绪门谓词 0x6839e（读 C2PMSG_90）
    mach_vm_address_t                                            org93F64{0};    // B88 观测 R2'：Apple 0x93f64（查位过滤器，bm=(obj+0x7B8)>>id & 1）
    mach_vm_address_t                                            orgIpiInit{0};      // P7c 包D：判据③ 0x9405e（IpiInitializeIpInterfaces）
    mach_vm_address_t                                            orgIpEntrySeq{0};   // P7c 包D：判据④ 0x929ac（TlsExecuteIpEntrySeq）
    // 第八步观测（2026-09-28 第十九轮）：TTL 初始化 `0x8b10e` 的失败出口定位（只读）
    mach_vm_address_t                                            orgTtlCollect{0};   // 0x8b5de 收集器
    mach_vm_address_t                                            orgTtlAllocA{0};    // 0x93880
    mach_vm_address_t                                            orgTtlAllocB{0};    // 0x928f4
    mach_vm_address_t                                            orgTtlCheckD{0};    // 0xa285f
    mach_vm_address_t                                            orgTtlInit{0};     // 0x8af60 ttl_initialize（B67；无符号内部函数，地址路由）
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
    mach_vm_address_t                                            orgMode9440f{0};    // 0x9440f（模式 20B 入口；返回 0=失败/非 0=成功）
    mach_vm_address_t                                            orgMode96c4c{0};    // 0x96c4c（发现块回退路径；F0/F1 上游）
    mach_vm_address_t                                            orgMode9e505{0};    // 0x9e505（(8,0) SMU 槽 sw_init 包装入口；B33 读点）
    mach_vm_address_t                                            orgMode6430f{0};    // 0x6430f（smu_sw_init 槽方法本体；B35 读点）
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

    // A-28：公开 slide 供 `nredTraceFlush`（自由函数）写入文件名认属主（只读访问器，零副作用）。
    auto getKcSlide() const { return this->kcSlide; }

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
    static UInt8 wrapPred90(void* ctx);   // B81 观测点 A：Apple 就绪门谓词 0x6839e（读 C2PMSG_90→setne）
    static UInt8 wrap93F64(void* obj, UInt32 id);   // B88 观测 R2'：Apple 0x93f64（查位过滤器，原样透传返回值）
    static UInt32 wrapIpiInit(void* a, UInt32 b, void* c);        // P7c 包D：判据③ 0x9405e（只记返回值）
    static UInt32 wrapIpEntrySeq(void* a, UInt32 b, UInt32 c);    // P7c 包D：判据④ 0x929ac（只记返回值）
    // 第八步观测（2026-09-28 第十九轮）：`0x8b10e` 的 4 个候选失败出口判据函数（只读）
    static void* wrapTtlCollect(void* ctx);
    static void* wrapTtlAllocA(void* ctx);
    static void* wrapTtlAllocB(void* ctx);
    static bool  wrapTtlCheckD(void* obj, void* param);
    static UInt32 wrapTtlInit(void* a1, void* a2);   // B67：ttl_initialize 出入口两读（零行为改动）
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
    static UInt32 wrapMode9440f(void* a, UInt32 b, UInt32 c, void* d);
    static UInt32 wrapMode96c4c(void* obj, UInt32 hw, void* out);
    static UInt32 wrapMode9e505(void* slot);
    static SInt32 wrapMode6430f(void* in, void* out);
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
    // B71：`smuSendMessage` 从 private 区移入 public——供 `fw::nredFwBringupHook` 的
    //  「SMU 起来确认」调用（经 Apple 发送入口发 TestMessage；零我方寄存器操作）。
    //  ⚠️ 仅可见性变更（private→public），零运行时行为变化。
    CAILResult smuSendMessage(void* ctx, UInt32 message, UInt32 param = 0, UInt32* outParam = nullptr) const;
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
