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
//   ② RegSinkKernel(cb)：display::RegSink 实现（SEG1 基准 `smnAddr`、经 PCIE_INDEX2/DATA2 间接、
//      不清断 SEG0 —— I13/I14）。
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
#include <IOKit/IOLib.h>          // IODelay
#include <IOKit/IOMemoryDescriptor.h>       // A-10：addr64_t / IOByteCount
#include <IOKit/IOBufferMemoryDescriptor.h> // A-10：kIOMemoryPhysicallyContiguous / withOptions
#include "Psp13Bringup.hpp"       // BringupCtx / bringupRun / BringupStep
#include "Psp13Ring.hpp"          // RingState / ringInit / 缓冲尺寸
#include "RegSinkKernel.hpp"      // SmnCallbacks / RegSinkKernel
#include "FwFirmwareAsset.hpp"    // fnv1a64 / FwAssetDesc / fwAssetVerify（A-2 固件完整性）

// ── A-2：真实固件内嵌（来源：src/NootedRed/Firmware/，既有 T1 资产）──
//  `#embed` 需要 clang 19+（本工程 HWLibs.cpp 已用 53 处）；路径相对本文件（FwBringup/）解析。
//  期望哈希 = 离线对源文件实测的 fnv1a64（TOC 2560 B / TA 254976 B；sha256 见 A-2 报告 §固件来源）。
static const uint8_t sPsp13TocData[] = {
    #embed "../Firmware/psp_13_0_4_toc.bin"
};
static const uint8_t sPsp13TaData[] = {
    #embed "../Firmware/psp_13_0_4_ta.bin"
};
static const fw::FwAssetDesc kFwTocAsset = {sPsp13TocData, sizeof(sPsp13TocData), 0x5277BF308C9C5C4CULL};
static const fw::FwAssetDesc kFwTaAsset  = {sPsp13TaData, sizeof(sPsp13TaData), 0x3DD1DDCCE682BB8FULL};

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
inline void nredFwBringupHook(void* const /*appleCtx*/)
{
    // I3：RMMIO 前置判据（hwLateInit 映射 BAR5；未映射则跳过，不写任何寄存器）。
    if (!NRed::singleton().hasRmmio()) {
        NRED_TRACE("bringup: skipped (rmmio not mapped)");
        return;
    }

    // I4：一次性守卫（Apple 可能多次调 wrapSmuInitFunctionPointerList ⇒ bringup 只跑一次）。
    static bool bringupAttempted = false;
    if (bringupAttempted) { return; }
    bringupAttempted = true;

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

    fw::RingState ring{};
    fw::ringInit(&ring, ringVa, cmdVa, fenceVa);

    // ① SmnCallbacks（内核接线 + IODelay 注入；锁留空——引导期单 CPU、无并发写）。
    fw::SmnCallbacks cb{};
    cb.readReg    = &hookReadReg;
    cb.writeReg   = &hookWriteReg;
    cb.delayUs    = &hookDelayUs;
    cb.lock       = nullptr;
    cb.unlock     = nullptr;
    cb.ctx        = nullptr;
    cb.maxRetries = 3;

    // ② RegSinkKernel（SEG1 基准、PCIE 间接通道、不清断 SEG0 —— I13/I14）。
    fw::RegSinkKernel sink(cb);

    // ③ BringupCtx：真实固件（toc_data/toc_size/fw_pri_buf 真值）。
    fw::BringupCtx ctx{};
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
        NRED_TRACE("bringup: tmr alloc failed size=0x%X", ctx.tmr_size);
        if (tmrDesc) { tmrDesc->complete(); tmrDesc->release(); }
        ringDesc->complete(); ringDesc->release();
        cmdDesc->complete(); cmdDesc->release();
        fenceDesc->complete(); fenceDesc->release();
        fwPriDesc->complete(); fwPriDesc->release();
        return;
    }
    ctx.tmr_buf = tmrVa;

    // M2（零风险仪表）：只读打印已有量（ring/cmd/fence/fw_pri/tmr PA、tmr_size、
    // fbLocationBase 及长度、BAR0 物理地址与长度）。
    // ⚠️ "读 0 的陷阱"：捕获时点未证 ⇒ 0 只能读作"尚未捕获"，不代表真实为 0。
    {
        // fbLocationBase 及长度（Apple getVRAMRange 返回的基址；长度无直接 getter，标注未捕获）
        const UInt64 fbLocBase = NRed::singleton().getFbLocationBase();
        const UInt64 fbOff     = NRed::singleton().getFbOffset();
        // BAR0 物理地址（PCI 配置空间 BAR0 寄存器，mask 掉低 4 位标志位）
        UInt64 bar0Phys = 0;
        if (auto* igpu = NRed::singleton().getIGPU()) {
            const UInt32 bar0Reg = igpu->configRead32(kIOPCIConfigBaseAddress0);
            bar0Phys = (bar0Reg & ~0xFUL);
        }
        // BAR0 长度：无直接可靠读取路径，标注未捕获
        NRED_TRACE("M2: ring_pa=0x%llX cmd_pa=0x%llX fence_pa=0x%llX fwpri_pa=0x%llX tmr_pa=0x%llX tmr_sz=0x%X"
                   " | fbLocBase=0x%llX fbOff=0x%llX fbLen=未捕获"
                   " | bar0_pa=0x%llX bar0_len=未捕获",
                   (unsigned long long)ringPhys, (unsigned long long)cmdPhys,
                   (unsigned long long)fencePhys, (unsigned long long)fwPriPhys,
                   (unsigned long long)tmrPhys, ctx.tmr_size,
                   (unsigned long long)fbLocBase, (unsigned long long)fbOff,
                   (unsigned long long)bar0Phys);
    }

    // ⑦ Phase 2: TmrLoad (SETUP_TMR)，使用真实 tmr_mc_addr
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
    // A-13 D1′：分开记录 tmrLoad resp_status
    NRED_TRACE("bringup: tmrLoad resp_status=0x%X", ctx.last_resp_status_tmr_load);

    // I10：结构化逐步 trace（含 resp_status——A-10 ③，区分超时与固件应答）。
    NRED_TRACE("bringup: step=%u last_error=%d loadToc_resp=0x%X tmrLoad_resp=0x%X",
               static_cast<unsigned>(ctx.last_step), ctx.last_error,
               ctx.last_resp_status_load_toc, ctx.last_resp_status_tmr_load);
    (void) rc;

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