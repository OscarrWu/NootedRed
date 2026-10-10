// NRedSegReadout.hpp —— D-3 只读仪表（SEG0/SEG1 双段读数、C2PMSG_91/83 直读、PB 状态位）
//
// 门控：`-NRedSegReadout`（默认关、仅观测）。门控假 ⇒ 零 MMIO、零调用。
// 通道：NRED_TRACE（NRedTrace-NNN.log + SYSLOG/L1），不依赖 panic 门控。
// 只读：不写任何寄存器、不发送任何 SMU 消息、不改 Apple 代码/数据。
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

// ── 单点双段读数结果 ──
struct SegDualPoint {
    uint32_t regOff;     // dword 偏移（相对段基址）
    uint32_t seg0Val;    // SEG0 基址下的读数
    uint32_t seg1Val;    // SEG1 基址下的读数
    uint32_t diff;       // seg0Val ^ seg1Val（位级差异）
    uint32_t seg1Invalid; // 1 = SEG1 读数无效（全 1，或与 seg0 相同且非 0）——判据：SEG1 未使能
};

// ── 一轮只读仪表的完整读数集 ──
struct SegReadoutReadings {
    SegDualPoint dualSeg0;      // C2PMSG_91（MP0，偏移 0x9B）
    SegDualPoint dualSeg1;      // C2PMSG_83（MP1，偏移 0x293）
    SegDualPoint dualFwFlags;   // MP1_FIRMWARE_FLAGS（偏移 0x3010024 >> 2）
    uint32_t c2pmsg91Mp0;       // 直读（SEG0 基址）
    uint32_t c2pmsg83Mp1;       // 直读（SEG1 基址）
    uint32_t c2pmsg91Mp1;       // 直读（SEG1 基址，MP1 C2PMSG_91 = 0x29B）
    uint32_t fwFlags;           // PB 状态位（SEG1 基址）
};

// ── 只读序列（纯逻辑；readFn 返回给定字节地址的读数；不得写）──
// @param readFn  读取回调（字节地址 → 值）。返回 0xFFFFFFFF 表示读取失败。
inline SegReadoutReadings runSegReadout(uint32_t (*readFn)(uint64_t byteAddr),
                                        const uint32_t seg0Base,
                                        const uint32_t seg1Base)
{
    SegReadoutReadings out{};

    // A 项：双段读数（同一偏移，两种段基址）
    auto dual = [&](uint32_t regOff) -> SegDualPoint {
        SegDualPoint p{};
        p.regOff     = regOff;
        p.seg0Val    = readFn(smnAddrWithBase(seg0Base, regOff));
        p.seg1Val    = readFn(smnAddrWithBase(seg1Base, regOff));
        p.diff       = p.seg0Val ^ p.seg1Val;
        p.seg1Invalid = (p.seg1Val == 0xFFFFFFFFu) || (p.seg1Val == p.seg0Val && p.seg0Val != 0) ? 1u : 0u;
        return p;
    };
    out.dualSeg0    = dual(0x9B);                 // MP0_SMN_C2PMSG_91
    out.dualSeg1    = dual(0x293);                // MP1_SMN_C2PMSG_83
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
#include "../HWLibs.hpp"          // NRED_TRACE
#include "../NRed.hpp"            // NRed::singleton().readReg32Ext/hasRmmio

// 内核侧只读回调：经 NRed::readReg32Ext（间接通道 + 回读刷写；只读）。
static inline uint32_t segReadoutReadRegExt(const uint64_t byteAddr)
{
    return NRed::singleton().readReg32Ext(static_cast<uint32_t>(byteAddr));
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

    // A 项：双段读数
    NRED_TRACE("seg-readout: dual C2PMSG_91_MP0 off=0x%X seg0=0x%X seg1=0x%X diff=0x%X inv=%u",
               r.dualSeg0.regOff, r.dualSeg0.seg0Val, r.dualSeg0.seg1Val, r.dualSeg0.diff, r.dualSeg0.seg1Invalid);
    NRED_TRACE("seg-readout: dual C2PMSG_83_MP1 off=0x%X seg0=0x%X seg1=0x%X diff=0x%X inv=%u",
               r.dualSeg1.regOff, r.dualSeg1.seg0Val, r.dualSeg1.seg1Val, r.dualSeg1.diff, r.dualSeg1.seg1Invalid);
    NRED_TRACE("seg-readout: dual FW_FLAGS_MP1 off=0x%X seg0=0x%X seg1=0x%X diff=0x%X inv=%u",
               r.dualFwFlags.regOff, r.dualFwFlags.seg0Val, r.dualFwFlags.seg1Val, r.dualFwFlags.diff,
               r.dualFwFlags.seg1Invalid);

    // B 项：直读
    NRED_TRACE("seg-readout: direct C2PMSG_91_MP0=0x%X C2PMSG_83_MP1=0x%X C2PMSG_91_MP1=0x%X",
               r.c2pmsg91Mp0, r.c2pmsg83Mp1, r.c2pmsg91Mp1);

    // C 项：PB 状态位
    NRED_TRACE("seg-readout: FW_FLAGS=0x%X", r.fwFlags);
}
#endif  // FW_SEG_READOUT_NO_KEXT

}  // namespace fw