// NRedSegReadout.hpp —— D-3 只读仪表（SEG0/SEG1 双段读数、C2PMSG_91/83 直读、PB 状态位）
//
// 门控：`-NRedSegReadout`（默认关、仅观测）。门控假 ⇒ 零 MMIO、零调用。
// 通道：NRED_TRACE（NRedTrace-NNN.log + SYSLOG/L1），不依赖 panic 门控。
// 只读：不写任何功能寄存器（readReg32Ext 仅写 PCIE_INDEX2 索引寄存器以发起读取）、不发送任何 SMU 消息、不改 Apple 代码/数据。
// 有界：每步单次读取（无轮询、无重试），总耗时 < 1ms。
//
// 结构：
//   ① 纯逻辑（本文件前半，用户态可测）：地址计算 + 只读序列（给定读取函数指针）；
//   ② kext 接线（`FW_SEG_READOUT_NO_KEXT` 未定义时）：nredSegReadoutHook（NRED_TRACE 输出）。
//
// 读数项：
//   A. SEG0/SEG1 双段读数：同一 SMN 偏移经 SEG0/SEG1 两种段基址各读一次，
//      输出两读数与差值（判据：SEG1 读数无效/全 1 ⇒ SEG1 未使能）。
//   B. C2PMSG_91/83 直读：PMFW 应答的直接证据（不经 cgs，经 readReg32Ext）。
//   C. 复位/状态读数：PB 状态位（如 MP1_FIRMWARE_FLAGS）各一次。
//
// ⛔ kext 环境可编译：禁 `<cstdint>`/`<cstddef>`/`std::`（CI run #220 纪律）；用 `<stdint.h>`。

#pragma once

#include <stdint.h>
#include "RegAddr.hpp"

namespace fw {

// ── 单点读取结果（含超时标志——B 项判读正确性：区分"设备未应答"与"真实读回 0xFFFFFFFF"）──
struct SegReadPoint {
    uint32_t value;      // 读回值（超时/无效时为 0xFFFFFFFF）
    uint32_t timedOut;   // 1 = 超时（设备/索引端口未应答）；0 = 已读到值（即使值是 0xFFFFFFFF）
};

// ── 单点双段读数结果 ──
struct SegDualPoint {
    uint32_t regOff;     // dword 偏移（相对段基址）
    uint32_t seg0Val;    // SEG0 基址下的读数
    uint32_t seg1Val;    // SEG1 基址下的读数
    uint32_t seg0TimeOut; // 1 = SEG0 读取超时
    uint32_t seg1TimeOut; // 1 = SEG1 读取超时
    uint32_t diff;       // seg0Val ^ seg1Val（位级差异）
    uint32_t seg1Invalid; // 1 = SEG1 读数无效（超时/全 1，或与 seg0 相同且非 0）——判据：SEG1 未使能
};

// ── 一轮只读仪表的完整读数集 ──
struct SegReadoutReadings {
    SegDualPoint dualSeg0;      // C2PMSG_91（MP0，偏移 0x9B）
    SegDualPoint dualSeg1;      // C2PMSG_83（MP1，偏移 0x293）
    SegDualPoint dualSeg2;      // C2PMSG_91（MP1，偏移 0x29B）双段
    SegDualPoint dualFwFlags;   // MP1_FIRMWARE_FLAGS（偏移 0x3010024 >> 2）
    SegReadPoint c2pmsg91Mp0;   // 直读（SEG0 基址）
    SegReadPoint c2pmsg83Mp1;   // 直读（SEG1 基址）
    SegReadPoint c2pmsg91Mp1;   // 直读（SEG1 基址，MP1 C2PMSG_91 = 0x29B）
    SegReadPoint fwFlags;       // PB 状态位（SEG1 基址）
};

// ── 只读序列（纯逻辑；readFn 返回给定字节地址的读数；不得写）──
// @param readFn  读取回调（字节地址 → SegReadPoint{value, timedOut}）
inline SegReadoutReadings runSegReadout(SegReadPoint (*readFn)(uint64_t byteAddr),
                                        const uint32_t seg0Base,
                                        const uint32_t seg1Base)
{
    SegReadoutReadings out{};

    // A 项：双段读数（同一偏移，两种段基址）
    auto dual = [&](uint32_t regOff) -> SegDualPoint {
        SegDualPoint p{};
        p.regOff       = regOff;
        const SegReadPoint s0 = readFn(smnAddrWithBase(seg0Base, regOff));
        const SegReadPoint s1 = readFn(smnAddrWithBase(seg1Base, regOff));
        p.seg0Val      = s0.value;
        p.seg1Val      = s1.value;
        p.seg0TimeOut  = s0.timedOut;
        p.seg1TimeOut  = s1.timedOut;
        p.diff         = p.seg0Val ^ p.seg1Val;
        p.seg1Invalid  = (p.seg1TimeOut || p.seg1Val == 0xFFFFFFFFu
                          || (p.seg1Val == p.seg0Val && p.seg0Val != 0)) ? 1u : 0u;
        return p;
    };
    out.dualSeg0    = dual(0x9B);                 // MP0_SMN_C2PMSG_91
    out.dualSeg1    = dual(0x293);                // MP1_SMN_C2PMSG_83
    out.dualSeg2    = dual(0x29B);                // MP1_SMN_C2PMSG_91（双段）
    out.dualFwFlags = dual(0x3010024u >> 2);      // MP1_FIRMWARE_FLAGS（dword 偏移）

    // B 项：C2PMSG_91/83 直读
    out.c2pmsg91Mp0 = readFn(smnAddrWithBase(seg0Base, 0x9B));
    out.c2pmsg83Mp1 = readFn(smnAddrWithBase(seg1Base, 0x293));
    out.c2pmsg91Mp1 = readFn(smnAddrWithBase(seg1Base, 0x29B));

    // C 项：PB 状态位
    out.fwFlags = readFn(smnAddrWithBase(seg1Base, 0x3010024u >> 2));

    return out;
}

#ifndef FW_SEG_READOUT_NO_KEXT
// ── kext 接线（仅 kext 环境编译）──
// 注：此文件从 HWLibs.cpp（已在 namespace fw 内）包含，故不再另开 namespace fw
#include "../Regs/NBIO.hpp"        // PCIE_INDEX2 / PCIE_DATA2
#include "../HWLibs.hpp"          // NRED_TRACE
#include "../NRed.hpp"            // NRed::singleton().readReg32Raw/writeReg32Raw/hasRmmio

// 内核侧只读回调：经 NRed::readReg32Ext（间接通道 + 回读刷写；只读）。
// 有界超时（D-3 追加要求：有界超时＋失败即退＋不得重试，防 76/77 轮 ~45s 卡死机制）。
// 返回 SegReadPoint{value, timedOut}：timedOut=1 表示超时（设备/索引端口未应答）。
static inline SegReadPoint segReadoutReadRegExt(const uint64_t byteAddr)
{
    constexpr uint32_t kMaxRetries = 1000;  // 1000 * 10us = 10ms 上界
    for (uint32_t attempt = 0; attempt < kMaxRetries; ++attempt) {
        NRed::singleton().writeReg32Raw(PCIE_INDEX2, static_cast<uint32_t>(byteAddr));
        (void)NRed::singleton().readReg32Raw(PCIE_INDEX2);  // 回读刷写
        const uint32_t val = NRed::singleton().readReg32Raw(PCIE_DATA2);
        if (val != 0xFFFFFFFFu) { return {val, 0u}; }
        IODelay(10);
    }
    return {0xFFFFFFFFu, 1u};  // 超时：值=0xFFFFFFFF，timedOut=1
}

// D-3 只读仪表（kext 入口；由 HWLibs.cpp 门控调用）。
inline void nredSegReadoutHook(void* const /*appleCtx*/)
{
    // 前置判据：BAR5 必须已映射（hwLateInit 已执行）。
    if (!NRed::singleton().hasRmmio()) {
        NRED_TRACE("seg-readout: skipped (rmmio not mapped)");
        return;
    }

    const fw::SegReadoutReadings r = fw::runSegReadout(&segReadoutReadRegExt,
                                                       fw::kMpSeg0Base, fw::kMpSeg1Base);

    // A 项：双段读数（含超时标记 to0/to1）
    NRED_TRACE("seg-readout: dual C2PMSG_91_MP0 off=0x%X seg0=0x%X to0=%u seg1=0x%X to1=%u diff=0x%X inv=%u",
               r.dualSeg0.regOff, r.dualSeg0.seg0Val, r.dualSeg0.seg0TimeOut,
               r.dualSeg0.seg1Val, r.dualSeg0.seg1TimeOut,
               r.dualSeg0.diff, r.dualSeg0.seg1Invalid);
    NRED_TRACE("seg-readout: dual C2PMSG_83_MP1 off=0x%X seg0=0x%X to0=%u seg1=0x%X to1=%u diff=0x%X inv=%u",
               r.dualSeg1.regOff, r.dualSeg1.seg0Val, r.dualSeg1.seg0TimeOut,
               r.dualSeg1.seg1Val, r.dualSeg1.seg1TimeOut,
               r.dualSeg1.diff, r.dualSeg1.seg1Invalid);
    NRED_TRACE("seg-readout: dual C2PMSG_91_MP1 off=0x%X seg0=0x%X to0=%u seg1=0x%X to1=%u diff=0x%X inv=%u",
               r.dualSeg2.regOff, r.dualSeg2.seg0Val, r.dualSeg2.seg0TimeOut,
               r.dualSeg2.seg1Val, r.dualSeg2.seg1TimeOut,
               r.dualSeg2.diff, r.dualSeg2.seg1Invalid);
    // B 项：直读（含超时标记 to）
    NRED_TRACE("seg-readout: direct C2PMSG_91_MP0=0x%X to=%u C2PMSG_83_MP1=0x%X to=%u C2PMSG_91_MP1=0x%X to=%u",
               r.c2pmsg91Mp0.value, r.c2pmsg91Mp0.timedOut,
               r.c2pmsg83Mp1.value, r.c2pmsg83Mp1.timedOut,
               r.c2pmsg91Mp1.value, r.c2pmsg91Mp1.timedOut);

    // C 项：PB 状态位
    NRED_TRACE("seg-readout: FW_FLAGS=0x%X to=%u", r.fwFlags.value, r.fwFlags.timedOut);
}
#endif  // FW_SEG_READOUT_NO_KEXT

}  // namespace fw
