// NRedFwBringupHook.hpp —— A-1/A-2（域 A 续卡）固件层第一增量：自包含接线薄封装（header-only，kext-only）
//
// 收敛"从何处调用 bringupRun"：挂点（当前 C2 = X5000HWLibs::wrapSmuInitFunctionPointerList 出口）
// 只需调用 `fw::nredFwBringupHook(ctx)`；将来切换挂点只改调用位置，不改本文件调用代码。
//
// 通道-门控自反：
//   通道：本封装全部诊断经 `NRED_TRACE`（第三通道 `NRedTrace-NNN.log` + SYSLOG/L1 副本）。
//   所需门控：`-NRedFwBringup`（默认关）。门控假 ⇒ 挂点不调用本封装 ⇒ 零 MMIO、零 bringup 调用。
//   不依赖：任何 panic 门控。
//
// 接线内容（自包含）：
//   ① SmnCallbacks：readReg/writeReg → NRed::readReg32Raw/writeReg32Raw（BAR5 直读，dword 索引；
//      与 X6000FB.cpp `smnProbeReadReg/WriteReg` 同型）；delayUs → IODelay（延时注入抽象）；
//      lock/unlock = nullptr（本封装在 SMU 初始化引导期、单 CPU 路径执行，不取锁）。
//   ② RegSinkKernel(cb)：display::RegSink 实现（SEG1 基准 `smnAddr`、经 PCIE_INDEX2/DATA2 间接、
//      不清断 SEG0 —— I13/I14）。
//   ③ A-2 真实固件：内嵌 `psp_13_0_4_toc.bin` / `psp_13_0_4_ta.bin`（来源+哈希见
//      FwFirmwareAsset.hpp 与 A-2 报告 §固件来源）；**fwAssetVerify 校验失败 ⇒ trace 并跳过装载**
//      （可判失败，不静默通过）。BringupCtx 填实：toc_data/toc_size/fw_pri_buf 真值；
//      bl_comps 全无效（本机仅 TOC+TA，bootloader 链按 T6 全跳过）。
//   ④ bringupRun(ctx, ...) 调用 + `NRED_TRACE("bringup: step=%u last_error=%d")`（I10 格式）。
//
//
// ⛔ kext 环境可编译：禁 `<cstdint>`/`<cstddef>`/`std::`（CI run #220 纪律）；用 `<stdint.h>`。
// ⛔ 本文件**不进**用户态 tests 编译（tests 用 MockSink 测纯序列逻辑；本文件是内核侧接线）。

#pragma once

#include "../HWLibs.hpp"          // NRED_TRACE（共享 trace 宏；含 SYSLOG）
#include "../NRed.hpp"            // NRed::singleton().readReg32Raw/writeReg32Raw/hasRmmio
#include <IOKit/IOLib.h>          // IODelay
#include "Psp13Bringup.hpp"       // BringupCtx / bringupRun / BringupStep
#include "Psp13Ring.hpp"          // RingState / ringInit / 缓冲尺寸
#include "RegSinkKernel.hpp"      // SmnCallbacks / RegSinkKernel
#include "FwFirmwareAsset.hpp"     // fnv1a64 / FwAssetDesc / fwAssetVerify（A-2 固件完整性）

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

    // 缓冲（引导期单线程；静态缓冲避免大栈分配）：
    // ring 4KB + cmd 1KB + fence 4B + fw_pri 1MB（PSP_1_MEG 契约，kFwCopyMax）。
    static uint8_t  sRingBuf[kRingSizeBytes];
    static uint8_t  sCmdBuf[kCmdBufSize];
    static uint32_t sFenceBuf;
    static uint8_t  sFwPri[kFwCopyMax];

    fw::RingState ring{};
    fw::ringInit(&ring, sRingBuf, sCmdBuf, &sFenceBuf);

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
    //    bl_comps 全无效（本机仅 TOC+TA，bootloader 链按 T6 全跳过）。
    fw::BringupCtx ctx{};
    ctx.sink       = &sink;
    ctx.ring       = &ring;
    ctx.fw_pri_buf = sFwPri;
    ctx.tmr_buf    = nullptr;
    ctx.tmr_size   = 0;
    ctx.toc_data   = kFwTocAsset.data;
    ctx.toc_size   = kFwTocAsset.size;

    // ④ bringupRun（各步有界轮询：kFenceTimeout×80µs、kRespTimeout≈2s —— I7；
    //    no response 视为超时返回错误码、不 panic —— I8；失败不改变 Apple 行为 —— I5）。
    //    MC 地址全 0 占位：首增量无 DMA 分配，仅验证接线与流程有界失败路径。
    const int rc = fw::bringupRun(&ctx,
                                  false,   // boot_time_tmr（13.0.4 不预分配 TMR）
                                  false,   // autoload_supported
                                  0,       // fw_pri_mc_addr（无 DMA 分配）
                                  0,       // fence_mc_addr
                                  0,       // cmd_buf_mc_addr
                                  0);      // ring_mc_addr

    // I10：结构化逐步 trace（对齐 smu13ProbeState 位图风格；禁 panic 格式串加字段）。
    NRED_TRACE("bringup: step=%u last_error=%d",
               static_cast<unsigned>(ctx.last_step), ctx.last_error);
    // rc 与 ctx.last_error 一致（bringupRun 失败路径均回填 last_error）；step 指示中断步骤。
    (void) rc;
}

}  // namespace fw
