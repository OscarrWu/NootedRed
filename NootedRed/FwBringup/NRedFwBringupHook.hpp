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
#include <IOKit/IOLib.h>          // IODelay
#include <IOKit/IOMemoryDescriptor.h>       // A-10：addr64_t / IOByteCount
#include <IOKit/IOBufferMemoryDescriptor.h> // A-10：kIOMemoryPhysicallyContiguous / withOptions
#include <IOKit/IODMACommand.h>   // Path B：TMR 缓冲的 DMA 映射（取 IOVA 作 buf_phy_addr）
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
// B19：bringup 期间静音 `NRED_TRACE` 的 SYSLOG 通道（深栈 + `lilu_os_log` 1KB 栈缓冲 ⇒ Double fault，
//  见 HWLibs.hpp 中 `gNredTraceSyslogMuted` 声明处根因注释）。RAII ⇒ 覆盖**所有** return 分支；
//  退出时恢复进入前的值（嵌套安全）。trace 行仍入内存环，由既有安全时点刷盘 ⇒ 无诊断损失。
struct NredSyslogMuteGuard {
    bool prev;
    NredSyslogMuteGuard() : prev(gNredTraceSyslogMuted) { gNredTraceSyslogMuted = true; }
    ~NredSyslogMuteGuard() { gNredTraceSyslogMuted = prev; }
};

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
static inline uint64_t nredMapTmrForDma(IOBufferMemoryDescriptor* desc, uint64_t size)
{

    if (desc == nullptr || size == 0) {
        NRED_TRACE("tmr-dma: skip (desc=%p size=0x%llX)", desc, (unsigned long long)size);
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

    // 映射保留（不 complete）：见本函数头注（D3：TMR 缓冲保留至卸载）。
    NRED_TRACE("tmr-dma: iova=0x%llX len=0x%llX", (unsigned long long)seg.fIOVMAddr,
               (unsigned long long)seg.fLength);
    return seg.fIOVMAddr;
}

inline void nredFwBringupHook(void* const /*appleCtx*/)
{
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

    // ── Path B：TMR 缓冲 DMA 映射（buf_phy_addr 按语义填 GPU 可寻址地址 / IOVA）──
    // 失败 ⇒ iova=0 ⇒ prepTmrCmd 降级为"两字段都填 CPU PA"（原行为），不改变崩溃形态。
    const uint64_t tmrIova = nredMapTmrForDma(tmrDesc, ctx.tmr_size);

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
                               fenceMc,
                               tmrIova);
    if (rc != 0) {
        NRED_TRACE("bringup: phase2 failed step=%u error=%d tmrLoad_resp=0x%X",
                   static_cast<unsigned>(ctx.last_step), rc, ctx.last_resp_status_tmr_load);
    }
    // A-13 D1′：分开记录 tmrLoad resp_status
    NRED_TRACE("bringup: tmrLoad resp_status=0x%X", ctx.last_resp_status_tmr_load);

    // I10/G6/J-1：结构化逐步 trace（末行携带 last_step，供 J-1「last_step 单调不减」断言）。
    NRED_TRACE("bringup: step=%u last_error=%d loadToc_resp=0x%X tmrLoad_resp=0x%X",
               static_cast<unsigned>(ctx.last_step), ctx.last_error,
               ctx.last_resp_status_load_toc, ctx.last_resp_status_tmr_load);

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