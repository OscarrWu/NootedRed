// NRedFwBringupHook.hpp —— 固件层带钩薄封装（A-1/A-2/A-10；header-only，kext-only）
//
// 收敛"从何处调用 bringupRun"：挂点（当前 C2 = X5000HWLibs::wrapSmuInitFunctionPointerList 出口）
// 只需调用 `fw::nredFwBringupHook(ctx)`；将来切换挂点只改调用位置，不改本文件调用代码。
//
// 通道-门控自反：
//   通道：本封装全部诊断经 `NRED_TRACE`（第三通道 `NRedTrace-NNN.log` + SYSLOG/L1 副本）。
//   所需门控：`-NRedFwBringup`（默认关；由挂点判定，门控假 ⇒ 本封装不被调用 ⇒ 零 MMIO/零分配）。
//   不依赖：任何 panic 门控。
//
// 接线内容（自包含）：
//   ① SmnCallbacks：readReg/writeReg → NRed::readReg32Raw/writeReg32Raw（BAR5 直读，dword 索引；
//      与 X6000FB.cpp `smnProbeReadReg/WriteReg` 同型）；delayUs → IODelay（延时注入抽象）；
//      lock/unlock = nullptr（本封装在 SMU 初始化引导期、单 CPU 路径执行，不取锁）。
//   ② RegSinkKernel(cb)：display::RegSink 实现；段基址＝**构造默认 `kMpSeg0Base`（SEG0）**
//      （RegSinkKernel.hpp:216/288）。本封装**不**调用 `smnAddr`（其兼容默认才是 SEG1）
//      ⇒ bringup 全链 MMIO 落 SEG0（经 PCIE_INDEX2/DATA2 间接、不清断 SEG0 —— I13/I14）。
//      ⚠️ R6/U4 未定：SEG0/SEG1 孰为真通道无离线定论；此处只记录当前取值，不改段选语义。
//   ③ A-2 真实固件：内嵌 `psp_13_0_4_toc.bin` / `psp_13_0_4_ta.bin`；fwAssetVerify 校验失败
//      ⇒ trace 并跳过装载（可判失败，不静默通过）。
//   ④ A-10：真实 MC 地址替换 0 占位 —— 用 `IOBufferMemoryDescriptor`（物理连续，PSP 可 DMA）
//      分配 ring/cmd/fence/fw_pri 四缓冲，取物理段地址作为 MC 地址；分配/准备/段址失败
//      ⇒ trace 并跳过（可判、不静默）。MC 地址口径走 SEG0（B4-a 已证 SEG0 可读；SEG1 全超时）。
//   ⑤ bringupRun(ctx, ...) + `NRED_TRACE("bringup: step=%u last_error=%d resp_status=0x%X")`。
//
// ⛔ kext 环境可编译：禁 `<cstdint>`/`<cstddef>`/`std::`（CI run #220 纪律）；用 `<stdint.h>`。
// ⛔ 本文件**不进**用户态 tests 编译（tests 用 MockSink 测纯序列逻辑；本文件是内核侧接线）。
// ⚠️ A-10 风险登记：kext 物理连续分配可行性（IOBufferMemoryDescriptor 在早期引导可能失败——
//    失败已判、无静默）；SEG1 可达性仍开口（若后续需 SEG1，依赖 `mc_access_enable`）；
//    TMR 保留区由 SETUP_TMR(0x05) 后续分配（本轮未一并实现，见 A-10 报告结论）。

#pragma once

#include "../HWLibs.hpp"          // NRED_TRACE（共享 trace 宏；含 SYSLOG）
#include "../NRed.hpp"            // NRed::singleton().readReg32Raw/writeReg32Raw/hasRmmio
#include <GPUDriversAMD/PhoenixPPSMC.hpp>  // B71：PPSMC_MSG_TestMessage(0x01) 起来确认
#include <IOKit/IOLib.h>          // IODelay
#include <IOKit/IOMemoryDescriptor.h>       // A-10：addr64_t / IOByteCount
#include <IOKit/IOBufferMemoryDescriptor.h> // A-10：kIOMemoryPhysicallyContiguous / withOptions
#include <IOKit/IODMACommand.h>   // Path B：TMR 缓冲的 DMA 映射（取 IOVA 作 buf_phy_addr）
#include <IOKit/IOMapper.h>       // Path B 护栏：IOMapper::gSystem 过渡态自检
#include "Psp13Bringup.hpp"       // BringupCtx / bringupRun / BringupStep
#include "Psp13Ring.hpp"          // RingState / ringInit / 缓冲尺寸
#include "RegSinkKernel.hpp"      // SmnCallbacks / RegSinkKernel
#include "FwFirmwareAsset.hpp"    // fnv1a64 / FwAssetDesc / fwAssetVerify（A-2 固件完整性）
#include "../NRedTmrAddrReadout.hpp"  // A-23：三个只读仪表（纯逻辑）
// ── A-2：真实固件内嵌（来源：src/NootedRed/Firmware/，既有 T1 资产）──
//  `#embed` 需要 clang 19+（本工程 HWLibs.cpp 已用 53 处）；路径相对本文件（FwBringup/）解析。
//  期望哈希 = 离线对源文件实测的 fnv1a64（TOC 2560 B / TA 254976 B；sha256 见 A-2 报告 §固件来源）。
static const uint8_t sPsp13TocData[] = {
    #embed "../Firmware/psp_13_0_4_toc.bin"
};
static const uint8_t sPsp13TaData[] = {
    #embed "../Firmware/psp_13_0_4_ta.bin"
};
static const uint8_t sSmu1300Data[] = {
    #embed "../Firmware/smu_13_0_0.bin"
};
static const fw::FwAssetDesc kFwTocAsset = {sPsp13TocData, sizeof(sPsp13TocData), 0x5277BF308C9C5C4CULL};
static const fw::FwAssetDesc kFwTaAsset  = {sPsp13TaData, sizeof(sPsp13TaData), 0x3DD1DDCCE682BB8FULL};
static const fw::FwAssetDesc kFwSmuAsset = {sSmu1300Data, sizeof(sSmu1300Data), 0x47420DC80A7B332FULL};
// P2 包A：固件流程关键状态（供 HWLibs.cpp 的 [bringup] 头部戳记读取）。
//  在 hook 内两处**既有**打印点各记一次，不新增任何打印/落盘量；常量初始化（无 guard variable，
//  符合内核态约束）；全为标量 ⇒ 关态读到的即初值，戳记据此不输出（关态输出逐字不变）。
static UInt32 gBringupEnterCount = 0;   // 进入 hook 次数（非 0 ⇒ 进入过）
static UInt32 gBringupStep      = 0;   // 末次 step（哨兵 0＝尚未走到）
static SInt32 gBringupLastError = 0;   // 末次 last_error
static UInt32 gBringupLoadToc   = 0;   // 末次 loadToc_resp
static UInt32 gBringupTmrLoad   = 0;   // 末次 tmrLoad_resp

// 232：本头文件内的 NRED_TRACE 改走 fwTrace（单份 static 行缓冲 + nredTraceLine，零 SYSLOG 栈）；
//  push/pop 限定作用域 ⇒ HWLibs.cpp 其余区域仍用全局 NRED_TRACE。
#include "FwTraceLine.hpp"
#pragma push_macro("NRED_TRACE")
#undef NRED_TRACE
#define NRED_TRACE(...) fwTrace(__VA_ARGS__)



namespace fw {

// ── 内核侧回调（无状态转发到 NRed 公开访问器；与 X6000FB.cpp `smnProbeReadReg/WriteReg` 同型）──
static inline uint32_t hookReadReg(void* /*ctx*/, const uint32_t off)
{
    return NRed::singleton().readReg32Raw(off);
}
static inline void hookWriteReg(void* /*ctx*/, const uint32_t off, const uint32_t val)
{
    NRed::singleton().writeReg32Raw(off, val);
}
static inline void hookDelayUs(void* /*ctx*/, const uint32_t us)
{
    IODelay(us);
}

// ── 薄封装本体 ─────────────────────────────────────────────────────────────────────
// @param appleCtx Apple SMU 上下文（I12 的 `smuCtxCache` 置值由**挂点**完成；本函数只做 bringup）。
// B19：bringup 期间静音 `NRED_TRACE` 的 SYSLOG 通道（深栈 + `lilu_os_log` 1KB 栈缓冲 ⇒ Double fault，
//  见 HWLibs.hpp 中 `gNredTraceSyslogMuted` 声明处根因注释）。RAII ⇒ 覆盖**所有** return 分支；
//  退出时恢复进入前的值（嵌套安全）。trace 行仍入内存环，由既有安全时点刷盘 ⇒ 无诊断损失。
struct NredSyslogMuteGuard {
    bool prev;
    NredSyslogMuteGuard() : prev(gNredTraceSyslogMuted) { gNredTraceSyslogMuted = true; }
    ~NredSyslogMuteGuard() { gNredTraceSyslogMuted = prev; }
};

// 232 栈修正：MMIO 回调集合与 RegSinkKernel 改 static（bringup 单线程 + 一次性守卫）。
//  `RegSinkKernel` 无拷贝但可默认构造 + `setCallbacks()` ⇒ 不再在 hook 栈上构造（省 ~100 B）。
static fw::SmnCallbacks gBringupCb;
static fw::RegSinkKernel gBringupSink;

// 232：实际 bringup 主体（从 hook 拆分，独立栈帧；hook 只留入口标记/守卫，帧 ≤512 B）。
__attribute__((noinline))
static void nredBringupBody();

// ⚠️ β-1 重跑（本卡）：**本函数当前未被调用**（帧的 buf 已改填借用 MC 常量，见 prepTmrCmd）。
//   B40 实证：本函数返回的 IOVA 在本机退化为恒等（== CPU PA）⇒ 未测到 GPU 可寻址域，故本轮绕开。
//   **保留**理由：① 内含挂死护栏（IOMapper 过渡态自检），删除即失去该防御的可复用实现；
//   ② β-1 重跑出结论后大概率回切 Path B ⇒ 保留可最小化回切 diff（恢复只需接回调用）。
//   ⚠️ 副作用登记：保留即接受一处"当前未调用的 static inline 函数"（无编译告警风险：
//      `static inline` 未使用不触发 -Wunused-function；precheck 已实测无新增告警）。
// ── Path B：TMR 缓冲的 DMA 映射（取 GPU 可寻址地址 / IOVA）────────────────────────────
// 依据：外部考证定论 —— `psp_gfx_if.h` 明示 SETUP_TMR 的 `buf_phy_addr` 实为 **GPU 虚拟地址**，
//  Linux `psp_prep_tmr_cmd_buf` 里 `buf=tmr_mc`(MC/IOVA)、`system=amdgpu_gmc_vram_pa`(CPU PA)。
//  本仓库的 IOKit 对应做法参考 `srcs/mac-amdgpu-ref/gmc_v12_0.cpp:alloc_dma_block`
//  （`IODMACommand` → `PrepareForDMA` → `seg.address`），但那是 **DriverKit** 版 API；
//  本机是 **kernel kext**，故用内核版 `IODMACommand::withSpecification` + `gen64IOVMSegments`。
//
// 深栈纪律（B19/B25）：本函数只走 `NRED_TRACE`，**不**用 SYSLOG/FileIO/dumpNow/IOMalloc/DBGLOG。
//
// 优雅降级（本卡第 2.5 条）：任何一步失败（IOMMU/GART 未就绪、API 报错、段数≠1）⇒ 返回 0，
//  调用方据此回退到"两字段都填 CPU PA"的原行为，**不改变崩溃形态**。
// 注：映射**不**在函数末尾释放 —— TMR 缓冲按 D3 契约保留至 kext 卸载（PSP 仍引用该 DRAM 区），
//      提前 complete() 会让 PSP 侧拿到的地址失效 ⇒ 本函数不返回可释放句柄。
//
// @param desc  已 prepare() 过的 TMR 缓冲（物理连续）
// @param size  TMR 大小
// @return GPU 可寻址地址（IOVA）；0 ⇒ 映射不可用，走降级路径
__attribute__((noinline))
static inline uint64_t nredMapTmrForDma(IOBufferMemoryDescriptor* desc, uint64_t size)
{

    if (desc == nullptr || size == 0) {
        NRED_TRACE("tmr-dma: skip (desc=%p size=0x%llX)", desc, (unsigned long long)size);
        return 0;
    }
    // 【高·挂死护栏】系统 mapper 过渡态自检：`kUnknown(1)`/`kHasMapper(2)` 时
    //   `IODMACommand::prepare` 链上的 `IOMapper::checkForSystemMapper()`（IOMapper.h:87-93）
    //   会调 `waitForSystemMapper()`（:76，无超时参数）⇒ 可能挂死。故在此**先判**，
    //   过渡态直接降级，不调用可能阻塞的映射 API。`gSystem` 为 public static（IOMapper.h:85）。
    if ((uintptr_t)IOMapper::gSystem & 3) {
        NRED_TRACE("tmr-dma: system mapper transitional (gSystem=%#llX) -> fallback",
                   (unsigned long long)(uintptr_t)IOMapper::gSystem);
        return 0;
    }

    // kMapped：走系统 mapper（IOMMU/DART）拿 I/O 总线地址；64 位地址，不限段长。
    IODMACommand* dma = IODMACommand::withSpecification(kIODMACommandOutputHost64, 64, 0,
                                                        IODMACommand::kMapped, 0, 1, NULL, NULL);
    if (dma == nullptr) {
        NRED_TRACE("tmr-dma: withSpecification failed -> fallback");
        return 0;
    }
    // autoPrepare=false：不依赖"setMemoryDescriptor 默认已 prepare"这一未文档化行为，
    //   使 prepare 状态转移唯一、可判（下一条显式 prepare）。
    if (dma->setMemoryDescriptor(desc, false) != kIOReturnSuccess) {
        NRED_TRACE("tmr-dma: setMemoryDescriptor failed -> fallback");
        dma->clearMemoryDescriptor();   // 与另两支 cleanup 对称（防残留描述符引用）
        dma->release();
        return 0;
    }
    if (dma->prepare(0, size) != kIOReturnSuccess) {
        NRED_TRACE("tmr-dma: prepare failed -> fallback");
        dma->clearMemoryDescriptor();
        dma->release();
        return 0;
    }
    uint64_t off = 0;
    IODMACommand::Segment64 seg = {};
    uint32_t segCount = 1;
    if (dma->gen64IOVMSegments(&off, &seg, &segCount) != kIOReturnSuccess || segCount != 1 ||
        seg.fIOVMAddr == 0) {
        NRED_TRACE("tmr-dma: genIOVMSegments failed segs=%u -> fallback", segCount);
        dma->complete();
        dma->clearMemoryDescriptor();
        dma->release();
        return 0;
    }
    // 【低·段长防御】单段长不足 TMR 尺寸 ⇒ 映射不完整（防半覆盖 IOVA 进帧），视同失败。
    if (seg.fLength < size) {
        NRED_TRACE("tmr-dma: seg len=0x%llX < size=0x%llX -> fallback",
                   (unsigned long long)seg.fLength, (unsigned long long)size);
        dma->complete();
        dma->clearMemoryDescriptor();
        dma->release();
        return 0;
    }

    // 映射保留（不 complete）：见本函数头注（D3：TMR 缓冲保留至卸载）。
    NRED_TRACE("tmr-dma: iova=0x%llX len=0x%llX", (unsigned long long)seg.fIOVMAddr,
               (unsigned long long)seg.fLength);
    return seg.fIOVMAddr;
}

__attribute__((noinline))
inline void nredFwBringupHook(void* const /*appleCtx*/)
{
    // B39 三层入口标记 ②：`bringup: enter`（纯观测、零副作用、置于**所有 early-return 之前**）。
    //  用途：与 ①`smu-init-list: enter` 配对，分辨 bringup 是"未被调用"还是"调用了但早退"；
    //  与 ③`tmr-alloc:*` 配对可分到 TMR 分配之前/之后。
    //  ⚠️ 本行在 NredSyslogMuteGuard **之前**：此时 `gNredTraceSyslogMuted` 仍为假，若用
    //     `NRED_TRACE` 会走 SYSLOG（`lilu_os_log` 内含 1 KB 栈缓冲）⇒ B19 Double fault 同根因。
    //     故此处**不走 NRED_TRACE**，直连 `nredTraceLine`（内存环，零 FS、零 os_log）。
    {
        // B75：追加进入计数 `n=`（kext 侧真正的 bringup 入口＝**本函数**；
        //  `bringupRun`（Psp13Bringup.hpp 的三阶段兼容封装）只被用户态测试调用 ⇒
        //  那行的 `bringupRun: enter` 不会进包，故此处补上同义计数）。
        static unsigned sGHookEnter = 0;
        char line[64];
        const int n = snprintf(line, sizeof(line), "bringup: enter n=%u\n", ++sGHookEnter);
        if (n > 0) { nredTraceLine(line, n); ++gBringupEnterCount; }   // P2 包A：供头部戳记
    }
    const NredSyslogMuteGuard _syslogMute;   // B19：本 hook 作用域内静音 SYSLOG（RAII，随各 return 复位）

    // I3：RMMIO 前置判据（hwLateInit 映射 BAR5；未映射则跳过，不写任何寄存器）。
    if (!NRed::singleton().hasRmmio()) {
        NRED_TRACE("bringup: skipped (rmmio not mapped)");
        return;
    }

    // I4：一次性守卫（Apple 可能多次调 wrapSmuInitFunctionPointerList ⇒ bringup 只跑一次）。
    static bool bringupAttempted = false;
    if (bringupAttempted) { return; }
    bringupAttempted = true;

    // 232：主体下沉到独立 noinline 帧（hook 自身只剩入口标记/守卫 ⇒ 帧 ≤512 B）。
    nredBringupBody();
}

__attribute__((noinline))
static void nredBringupBody()
{
    // A-2：固件资产完整性校验（大小=编译期 sizeof；哈希=运行期 fnv1a64）。
    // 缺失/损坏 ⇒ trace 并跳过装载（可判失败，不静默通过）。
    if (!fw::fwAssetVerify(kFwTocAsset)) {
        NRED_TRACE("bringup: firmware asset invalid (toc), skipped");
        return;
    }
    if (!fw::fwAssetVerify(kFwTaAsset)) {
        NRED_TRACE("bringup: firmware asset invalid (ta), skipped");
        return;
    }

    // A-10：物理连续缓冲分配（SEG0 基址，PSP 可 DMA）。四缓冲：
    //   ring 4KB（kRingSizeBytes）+ cmd 1KB（kCmdBufSize）+ fence 4B + fw_pri 1MB（kFwCopyMax）。
    // 分配/准备/段址任一步失败 ⇒ trace 并跳过（可判失败，不静默）。
    constexpr UInt32 kFenceBytes = 4;
    constexpr UInt32 kPageAlign  = 4096;
    constexpr UInt32 kFwPriBytes = fw::kFwCopyMax;

    IOBufferMemoryDescriptor* ringDesc = IOBufferMemoryDescriptor::withOptions(
        kIOMemoryPhysicallyContiguous | kIODirectionInOut, fw::kRingSizeBytes, kPageAlign);
    IOBufferMemoryDescriptor* cmdDesc  = IOBufferMemoryDescriptor::withOptions(
        kIOMemoryPhysicallyContiguous | kIODirectionInOut, fw::kCmdBufSize, kPageAlign);
    IOBufferMemoryDescriptor* fenceDesc = IOBufferMemoryDescriptor::withOptions(
        kIOMemoryPhysicallyContiguous | kIODirectionInOut, kFenceBytes, kPageAlign);
    IOBufferMemoryDescriptor* fwPriDesc = IOBufferMemoryDescriptor::withOptions(
        kIOMemoryPhysicallyContiguous | kIODirectionInOut, kFwPriBytes, kPageAlign);

    if (ringDesc == nullptr || cmdDesc == nullptr || fenceDesc == nullptr || fwPriDesc == nullptr) {
        NRED_TRACE("bringup: alloc failed (withOptions null)");
        if (ringDesc) { ringDesc->release(); }
        if (cmdDesc) { cmdDesc->release(); }
        if (fenceDesc) { fenceDesc->release(); }
        if (fwPriDesc) { fwPriDesc->release(); }
        return;
    }
    if (ringDesc->prepare() != kIOReturnSuccess || cmdDesc->prepare() != kIOReturnSuccess ||
        fenceDesc->prepare() != kIOReturnSuccess || fwPriDesc->prepare() != kIOReturnSuccess) {
        NRED_TRACE("bringup: prepare failed");
        ringDesc->complete(); ringDesc->release();
        cmdDesc->complete(); cmdDesc->release();
        fenceDesc->complete(); fenceDesc->release();
        fwPriDesc->complete(); fwPriDesc->release();
        return;
    }

    // 物理段地址（物理连续 ⇒ 单段覆盖整块；segLen 校验防非连续/超短）。
    IOByteCount segLen = 0;
    const addr64_t ringPhys = ringDesc->getPhysicalSegment(0, &segLen, 0);
    const bool ringOk = (ringPhys != 0 && segLen >= fw::kRingSizeBytes);
    const addr64_t cmdPhys = cmdDesc->getPhysicalSegment(0, &segLen, 0);
    const bool cmdOk = (cmdPhys != 0 && segLen >= fw::kCmdBufSize);
    const addr64_t fencePhys = fenceDesc->getPhysicalSegment(0, &segLen, 0);
    const bool fenceOk = (fencePhys != 0 && segLen >= kFenceBytes);
    const addr64_t fwPriPhys = fwPriDesc->getPhysicalSegment(0, &segLen, 0);
    const bool fwPriOk = (fwPriPhys != 0 && segLen >= kFwPriBytes);

    if (!ringOk || !cmdOk || !fenceOk || !fwPriOk) {
        NRED_TRACE("bringup: bad phys ring=0x%llX/%u cmd=0x%llX/%u fence=0x%llX/%u fwpri=0x%llX/%u",
                   (unsigned long long)ringPhys, (unsigned)ringOk, (unsigned long long)cmdPhys, (unsigned)cmdOk,
                   (unsigned long long)fencePhys, (unsigned)fenceOk, (unsigned long long)fwPriPhys, (unsigned)fwPriOk);
        ringDesc->complete(); ringDesc->release();
        cmdDesc->complete(); cmdDesc->release();
        fenceDesc->complete(); fenceDesc->release();
        fwPriDesc->complete(); fwPriDesc->release();
        return;
    }

    // 虚拟地址（Ring/cmd/fence 由 ringInit/cmdBufClear 清零；fw_pri 显式清零——PSP_1_MEG 契约）。
    uint8_t*  const ringVa  = static_cast<uint8_t*>(ringDesc->getBytesNoCopy());
    uint8_t*  const cmdVa   = static_cast<uint8_t*>(cmdDesc->getBytesNoCopy());
    uint32_t* const fenceVa = static_cast<uint32_t*>(fenceDesc->getBytesNoCopy());
    uint8_t*  const fwPriVa = static_cast<uint8_t*>(fwPriDesc->getBytesNoCopy());
    bzero(fwPriVa, kFwPriBytes);

    // 232 栈修正：状态对象改 static（引导期单线程 + bringupAttempted 守卫）；每次进入显式清零。
    static fw::RingState sRing;
    sRing = fw::RingState{};
    fw::RingState& ring = sRing;
    fw::ringInit(&ring, ringVa, cmdVa, fenceVa);

    // ① SmnCallbacks / ② RegSinkKernel：static 实例（232 栈修正；每次进入重置回调后重设）。
    fw::SmnCallbacks& cb = gBringupCb;
    cb = fw::SmnCallbacks{};
    cb.readReg    = &hookReadReg;
    cb.writeReg   = &hookWriteReg;
    cb.delayUs    = &hookDelayUs;
    cb.lock       = nullptr;
    cb.unlock     = nullptr;
    cb.ctx        = nullptr;
    cb.maxRetries = 3;
    // RegSinkKernel（SEG1 基准、PCIE 间接通道、不清断 SEG0 —— I13/I14）。
    fw::RegSinkKernel& sink = gBringupSink;
    sink.setCallbacks(cb);

    // ③ BringupCtx：真实固件（toc_data/toc_size/fw_pri_buf 真值）。
    static fw::BringupCtx sCtx;
    sCtx = fw::BringupCtx{};
    fw::BringupCtx& ctx = sCtx;
    ctx.sink       = &sink;
    ctx.ring       = &ring;
    ctx.fw_pri_buf = fwPriVa;
    ctx.tmr_buf    = nullptr;
    ctx.tmr_size   = 0;
    ctx.toc_data   = kFwTocAsset.data;
    ctx.toc_size   = kFwTocAsset.size;

    // ④ A-10：真实 MC 地址（物理段地址；SEG0 口径——B4-a 已证 SEG0 可读，SEG1 全超时）。
    const uint64_t fwPriMc  = fwPriPhys;
    const uint64_t fenceMc  = fencePhys;
    const uint64_t cmdBufMc = cmdPhys;
    const uint64_t ringMc   = ringPhys;

    // ⑤ Phase 1: 运行到 TmrInit（含 loadToc），得到 tmr_size
    int rc = fw::bringupRunToTmrInit(&ctx,
                                     false,      // boot_time_tmr
                                     false,      // autoload_supported
                                     fwPriMc,
                                     fenceMc,
                                     cmdBufMc,
                                     ringMc);
    if (rc != 0) {
        NRED_TRACE("bringup: phase1 failed step=%u error=%d loadToc_resp=0x%X",
                   static_cast<unsigned>(ctx.last_step), rc, ctx.last_resp_status_load_toc);
        ringDesc->complete(); ringDesc->release();
        cmdDesc->complete(); cmdDesc->release();
        fenceDesc->complete(); fenceDesc->release();
        fwPriDesc->complete(); fwPriDesc->release();
        return;
    }
    // A-13 D1′：分开记录 loadToc resp_status
    NRED_TRACE("bringup: loadToc resp_status=0x%X", ctx.last_resp_status_load_toc);

    // ⑥ A-11：TMR 物理连续缓冲分配（PSP 可 DMA）。tmrInit 计算 tmr_size 后，在此分配。
    // 依据：Linux psp_tmr_init (amdgpu_psp.c:881-923) 先 loadToc 取 tmr_size，再分配 TMR。
    // 分配失败 ⇒ trace 并跳过（可判、不静默）。
    // B39 三层入口标记 ③-a：`tmr-alloc: enter`（进入分配段，早于任何分配/判据）。
    NRED_TRACE("tmr-alloc: enter");
    constexpr UInt32 kTmrAlign = 0x100000;
    IOBufferMemoryDescriptor* tmrDesc = nullptr;
    addr64_t tmrPhys = 0;
    uint8_t* tmrVa = nullptr;
    bool tmrOk = false;
    if (ctx.tmr_size > 0) {
        tmrDesc = IOBufferMemoryDescriptor::withOptions(
            kIOMemoryPhysicallyContiguous | kIODirectionInOut, ctx.tmr_size, kTmrAlign);
        if (tmrDesc != nullptr && tmrDesc->prepare() == kIOReturnSuccess) {
            IOByteCount segLen = 0;
            tmrPhys = tmrDesc->getPhysicalSegment(0, &segLen, 0);
            tmrOk = (tmrPhys != 0 && segLen >= ctx.tmr_size);
            if (tmrOk) {
                tmrVa = static_cast<uint8_t*>(tmrDesc->getBytesNoCopy());
                bzero(tmrVa, ctx.tmr_size);
            }
        }
    }
    if (!tmrOk) {
        // B39 三层入口标记 ③-c：`tmr-alloc: fail`（纯观测；不新增状态、不改控制流。
        //  rc 取既有判定：0=desc 为空/未 prepare，1=段址为 0 或段长不足 —— 由 !tmrOk 的成因直接给出）
        NRED_TRACE("tmr-alloc: fail rc=%d", (tmrDesc == nullptr) ? 0 : 1);
        NRED_TRACE("bringup: tmr alloc failed size=0x%X", ctx.tmr_size);
        if (tmrDesc) { tmrDesc->complete(); tmrDesc->release(); }
        ringDesc->complete(); ringDesc->release();
        cmdDesc->complete(); cmdDesc->release();
        fenceDesc->complete(); fenceDesc->release();
        fwPriDesc->complete(); fwPriDesc->release();
        return;
    }
    ctx.tmr_buf = tmrVa;
    // B39 三层入口标记 ③-b：`tmr-alloc: ok size=0x%X`（分配成功；纯观测，不改控制流）
    NRED_TRACE("tmr-alloc: ok size=0x%X", ctx.tmr_size);

    // ── β-1 重跑：buf_phy_addr 已改填借用 MC 常量（prepTmrCmd 内），此处**不再**调用
    //   `nredMapTmrForDma()`（B40 实证 DMA 映射退化为恒等 IOVA==CPU PA，未测到 GPU 可寻址域）。
    //   该函数与挂死护栏**保留**（上方定义）：β-1 重跑结论后大概率回切 Path B，最小化回切 diff。

    // A-23：三个只读仪表（门控 `-NRedTmrAddrReadout`，默认关、仅观测）。
    //  通道：NRED_TRACE（A-22 内存优先，安全时点刷盘）⇒ 绝不调任何写文件接口。
    //  第 ③ 项用 NRed 既有内存字段（`fbLocationBase`/`fbLocationSize`），**零寄存器探针**。
    if (checkKernelArgument("-NRedTmrAddrReadout")) {
        const uint64_t fbBase = NRed::singleton().getFbLocationBase();
        const uint64_t fbSize = NRed::singleton().getFbLocationSize();
        const nred::TmrAddrReadout r = nred::runTmrAddrReadout(tmrPhys, tmrPhys, ctx.tmr_size,
                                                                fbBase, fbSize, tmrPhys);
        // ① 值本身
        NRED_TRACE("tmr-addr: buf_phy=0x%llX sys_phy=0x%llX size=0x%X",
                   (unsigned long long)r.buf_phy, (unsigned long long)r.sys_phy, r.tmr_size);
        // ② 自然对齐余数
        if (r.align_rem == ~0ULL) {
            NRED_TRACE("tmr-addr: align_rem=哨兵(tmr_size=0)");
        } else {
            NRED_TRACE("tmr-addr: align_rem=%llu (tmr_mc=0x%llX %% tmr_size=0x%X)",
                       (unsigned long long)r.align_rem, (unsigned long long)tmrPhys, ctx.tmr_size);
        }
        // ③ 窗内判定（含"读 0 陷阱"——base/size 为 0 ⇒ unknown）
        const char* inWindowStr = (r.in_window == 2) ? "unknown(0陷阱)"
                                 : (r.in_window == 1) ? "in" : "out";
        NRED_TRACE("tmr-addr: in_window=%s base=0x%llX size=0x%llX tmr_mc=0x%llX",
                   inWindowStr, (unsigned long long)r.win_base, (unsigned long long)r.win_size,
                   (unsigned long long)tmrPhys);
    }

    // ⑦ Phase 2: TmrLoad（SETUP_TMR），使用真实 tmr_mc_addr。
    //   回归恢复：A-23（c0395be）自述本文件改动为 +25/-0（在"TMR 分配后、Phase 2 前"插入读点），
    //   但该提交实际连带删除了本 Phase 2 块与末行结构化 trace。而 A-13 的 D1′、
    //   kb/技术草案/PAeqMC与SETUP_TMR字段语义.md:333（以 bringupRunTmrLoad 前为坐标）以及
    //   ROADMAP 的 B 轮三出口判据（`tmrLoad: ok` / `step=12`）均以本块存在为前提 ⇒ 判为回归，予以恢复。
    rc = fw::bringupRunTmrLoad(&ctx,
                               false,      // boot_time_tmr
                               false,      // autoload_supported
                               tmrPhys,
                               cmdBufMc,
                               fenceMc);
    if (rc != 0) {
        NRED_TRACE("bringup: phase2 failed step=%u error=%d tmrLoad_resp=0x%X",
                   static_cast<unsigned>(ctx.last_step), rc, ctx.last_resp_status_tmr_load);
    }
    // ── B69 phase3（更正版）：第一次真实调用 LOAD_ASD（载荷＝TA 内 ASD 子固件切片）──
    //  依据（技术经理 B69 更正＋Linux 对应实现）：
    //    · 本资产 TA 表 `desc[0]`＝ASD（`Psp13Bringup.hpp` 6b 注释实测：ASD size=217344）⇒
    //      Linux 侧装载用 **`GFX_CMD_ID_LOAD_ASD = 4`**（`amdgpu_psp.c:1693` 设
    //      `asd_context.ta_load_type = GFX_CMD_ID_LOAD_ASD`；`:1775` `cmd->cmd_id =
    //      context->ta_load_type`；常量 `psp_gfx_if.h:92`）——**不是** `LOAD_IP_FW(6)`，
    //      **也**不是通用 `LOAD_TA(1)`（后者用于其它 TA，本资产不用）。
    //    · 载荷布局＝`cmd_load_ta` 专用结构（地址 lo/hi ＋ `ta_size`，psp_gfx_if.h:131-143），
    //      与 `cmd_load_ip_fw` 不同 ⇒ 用既有专用原语 `prepLoadAsdCmd`/`executeLoadAsd`
    //      （`Psp13Bringup.hpp` 6b；等 Linux `psp_prep_ta_load_cmd_buf` amdgpu_psp.c:1771-1785）。
    //  条件：phase2（TMR）**成功**才执行（rc==0）；前置**再**验 TA 资产（fnv1a64，缺失/损坏
    //  ⇒ 记行、不再发任何帧——可判失败，不静默通过）。
    //  流程：assetVerify(TA) → parseTaAsd（取 desc[0] 切片偏移/大小）→ ASD 切片拷入 fw_pri
    //  （≤kFwCopyMax 1 MB）→ executeLoadAsd（LOAD_ASD 单帧，与 tmrLoad 同 ring/cmd/fence 通道）。
    //  判据：ret==0 成功；ret<0＝超时/环错误；ret>0＝TEE 错误码（psp_gfx_if.h:516-520）。
    //  **每个失败分支都有出口**：记一行、停机（不重试、不进任何后续消息阶段），且原语内部
    //  `ringWaitForFence` 自带超时预算 ⇒ **不悬停**。
    // ── B71：SMU/PMFW 发帧（LOAD_IP_FW × GFX_FW_TYPE_SMU=18；插在 ASD 之前）──────────
    //  依据：镜像卡（smu_13_0_0.bin，整文件 292,452 B；payload 切片 261,632 B @256）＋
    //   映射卡（LOAD_IP_FW=6 × fw_type=18；等 Linux `psp_execute_ip_fw_load`
    //   amdgpu_psp.c:3386-3401）＋起来卡 §⑤（TestMessage=0x01 经 Apple 入口）。
    //   ⚠️ 本卡**不做一次性守卫**（技术经理更正：第二遍并未进入 `bringupRun`，
    //     无 `bringupRun: enter`／`smu13: case 13` ⇒ 守卫当前无实际作用，保留会引入
    //     不必要的行为变更）⇒ 本段保持原样，仅按第 1 项在其后补门控真值行。
    //   ⚠️ 零 SMN 读（不碰 0x3010028）；零新门控；零白名单改动；零新增 SYSLOG。

    if (rc == 0) {
        if (!fw::fwAssetVerify(kFwSmuAsset)) {
            NRED_TRACE("fw: load-ip-fw smu asset-invalid, skipped");
        } else {
            fw::CommonFwHeader smuHdr{};
            if (!fw::parseCommonFwHeader(kFwSmuAsset.data, kFwSmuAsset.size, &smuHdr)) {
                NRED_TRACE("fw: load-ip-fw smu hdr-parse fail");
            } else if (smuHdr.ucode_array_offset_bytes > kFwSmuAsset.size ||
                       smuHdr.ucode_size_bytes > kFwSmuAsset.size - smuHdr.ucode_array_offset_bytes) {
                NRED_TRACE("fw: load-ip-fw smu slice-oob off=%u pay=%u",
                           smuHdr.ucode_array_offset_bytes, smuHdr.ucode_size_bytes);
            } else {
                // ① payload 切片 [off, off+pay) 拷入 fw_pri（261,632 B ≤ kFwCopyMax 1 MB）
                const int copyRet = fw::copyFw(fwPriVa, kFwSmuAsset.data + smuHdr.ucode_array_offset_bytes,
                                               smuHdr.ucode_size_bytes);
                if (copyRet != 0) {
                    NRED_TRACE("fw: load-ip-fw smu copy fail ret=%d", copyRet);
                } else {
                    // ② LOAD_IP_FW 单帧（fw_type=GFX_FW_TYPE_SMU=18；与 tmrLoad 同通道）
                    const int smuRet = fw::executeLoadIpFw(sink, &ring,
                                                           fwPriMc, smuHdr.ucode_size_bytes,
                                               GFX_FW_TYPE_SMU,
                                                           cmdBufMc, fenceMc);
                    // B75：`mc=`（本次帧交给 PSP 的 MC 地址）／B77②：`resp_status=`（PSP

                    //  B77②：追加 `resp_status=`（PSP 响应状态字）——仿既有
                    //  `bringup: loadToc resp_status=0x%X` 写法；取法＝调用后读响应帧
                    //  `fw::cmdBufGet(&ring)->resp_status`（既有只读访问器，零行为改动）。
                    //  行长自证：前缀 39 + 4×~8 + mc 18 + resp_status ~14 ≈ 97 ≤ 191 ⇒ 不截断。
                    const fw::GfxCmdResp* const respSmu = fw::cmdBufGet(&ring);
                    NRED_TRACE("fw: load-ip-fw smu ret=%d hdr=%u pay=%u off=%u mc=0x%llx resp_status=0x%X",
                               smuRet, smuHdr.header_size_bytes, smuHdr.ucode_size_bytes,
                               smuHdr.ucode_array_offset_bytes,
                               static_cast<unsigned long long>(fwPriMc),
                               (respSmu != nullptr) ? respSmu->resp_status : 0xFFFFFFFFu);
                    if (smuRet != 0) {
                        NRED_TRACE("bringup: phase3 failed (load-ip-fw smu) ret=%d", smuRet);
                    }
                    // B73：`TestMessage` 起来确认**已移走**——bringup 阶段 `smuContext()`
                    //  恒 0（B72 实测 `smu probe: no ctx yet`）⇒ 改放
                    //  `smuGetUCodeConsts`（HWLibs.cpp:2421；Apple 经
                    //  `smuGetUCodeConstsField` 实证调用、持有 Apple ctx 的发送点）。
                    //  ⚠️ 此处**只留 SMU 发帧**；判据保持既有 T4 行（成功/失败两路都打）。
                }
            }
        }
    }
    if (rc == 0) {
        if (!fw::fwAssetVerify(kFwTaAsset)) {
            NRED_TRACE("fw: load-asd asset-invalid, skipped");
        } else {
            // ① 解析 TA 头，取 ASD（desc[0]）切片偏移/大小
            static fw::TaBinDesc sAsd;
            sAsd = fw::TaBinDesc{};
            fw::TaBinDesc& asd = sAsd;
            const int parseRet = fw::parseTaAsd(kFwTaAsset.data, kFwTaAsset.size, &asd);
            if (parseRet != 0) {
                NRED_TRACE("fw: load-asd parse fail ret=%d", parseRet);
            } else if (!asd.found) {
                NRED_TRACE("fw: load-asd no-asd, skipped");
            } else {
                // ② ASD 切片拷入 fw_pri（MC 可 DMA；217344 B ≤ kFwCopyMax 1 MB）
                const int copyRet = fw::copyFw(fwPriVa, kFwTaAsset.data + asd.offset_bytes,
                                               asd.size_bytes);
                if (copyRet != 0) {
                    NRED_TRACE("fw: load-asd copy fail ret=%d", copyRet);
                } else {
                    // ③ LOAD_ASD 单帧（专用 cmd_load_ta 布局；与 tmrLoad 同通道）
                    const int asdRet = fw::executeLoadAsd(sink, &ring, asd,
                                                          fwPriMc,      // fw_pri_mc_addr
                                                          cmdBufMc,
                                                          fenceMc);
                    NRED_TRACE("fw: load-asd ret=%d", asdRet);
                    if (asdRet != 0) {
                        NRED_TRACE("bringup: phase3 failed (load-asd) ret=%d", asdRet);
                    }
                }
            }
        }
    }
    // A-13 D1′：分开记录 tmrLoad resp_status
    NRED_TRACE("bringup: tmrLoad resp_status=0x%X", ctx.last_resp_status_tmr_load);

    // I10/G6/J-1：结构化逐步 trace（末行携带 last_step，供 J-1「last_step 单调不减」断言）。
    NRED_TRACE("bringup: step=%u last_error=%d loadToc_resp=0x%X tmrLoad_resp=0x%X",
               static_cast<unsigned>(ctx.last_step), ctx.last_error,
               ctx.last_resp_status_load_toc, ctx.last_resp_status_tmr_load);
    // P2 包A：同一处既有打印点顺带记四项（不新增打印/落盘量），供 HWLibs.cpp 头部戳记读取。
    gBringupStep = static_cast<UInt32>(ctx.last_step);
    gBringupLastError = ctx.last_error;
    gBringupLoadToc = ctx.last_resp_status_load_toc;
    gBringupTmrLoad = ctx.last_resp_status_tmr_load;

    // D3: TMR 缓冲保留至卸载（对齐 Linux"保留至卸载"契约，防提前释放）。
    // 此处不释放 tmrDesc，保留至 kext 卸载（static 持有）。
    // 释放（引导期单次执行；结束后释放，避免长期占用）。
    ringDesc->complete(); ringDesc->release();
    cmdDesc->complete(); cmdDesc->release();
    fenceDesc->complete(); fenceDesc->release();
    fwPriDesc->complete(); fwPriDesc->release();
    // tmrDesc 故意不释放：保留至卸载（D3）
}

}  // namespace fw

#pragma pop_macro("NRED_TRACE")