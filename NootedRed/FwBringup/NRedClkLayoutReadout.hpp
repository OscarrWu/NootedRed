// NRedClkLayoutReadout.hpp —— A-12 读数仪表扩展（并批（甲）（乙）两线）
//
// 门控：`-NRedClkLayoutReadout`（默认关、仅观测）。门控假 ⇒ 本封装不被调用 ⇒ 零读数、零内存访问。
// 通道：NRED_TRACE（NRedTrace-NNN.log + SYSLOG/L1 副本），不依赖 panic 门控。
// 只读：不写任何功能寄存器、不发送任何 SMU/SMN 消息、不改 Apple 代码/数据（仅观测内存对象）。
// 有界：每点单次读取（无轮询、无重试、失败即退）；`to=` 标记 = 指针无效/不可读（与 D-3 同构：
//        区分"读数不可信"与"真实 0 值"）。
//
// 结构：
//   ① 纯逻辑（本文件前半，用户态可测）：给定读取回调（安全内存读），组装读数；
//   ② kext 接线（`FW_CLK_LAYOUT_NO_KEXT` 未定义时）：nredClkLayoutReadoutHook（NRED_TRACE 输出）。
//
// 读数项：
//   （甲）线效果读数（B5 判据）——在 wrapDcClkMgrCreate 返回后：
//     A. clk_mgr+0x130 的 128B bw 表是否被填（非零 dword 计数 + 首/末非零值）；
//     B. pp_smu->f38 是否为我方回调指针（ours=1）；
//     C. 回调调用计数（gNRedPpSmuOverlayCalls，由调用方传入）。
//   （乙）线布局抓取（overlay 前置布局）：
//     D. dc_context->f88(0x58) 指向对象（时钟上下文）前 376B(0x178) 头部：
//        vtable(+0)/f8(+0x8)/f20(+0x20)/f24(+0x24) 是否非 0 + f48(+0x30) 指针；
//     E. 若 f48 非 0：读 f48 对象前 ~0x1C0 字节重点：+0x118 byte、+0x1BA byte。
//
// ⛔ kext 环境可编译：禁 `<cstdint>`/`<cstddef>`/`std::`（CI run #220 纪律）；用 `<stdint.h>`。

#pragma once

#include <stdint.h>
#include "RegAddr.hpp"

namespace fw {

// ── 读取回调（安全内存读；调用方先判指针有效性，不 deref 无效指针）──
// valid 输出：1 = 已读到；0 = 指针无效/跳过（等效 to=1）。
typedef uint64_t (*ClkReadU64Fn)(uint64_t addr, uint32_t* valid);
typedef uint32_t (*ClkReadU32Fn)(uint64_t addr, uint32_t* valid);
typedef uint8_t  (*ClkReadU8Fn)(uint64_t addr, uint32_t* valid);

// ── 一轮读数仪表（（甲）（乙）两线并批）──
struct ClkLayoutReadout {
    // （甲）线 A：bw 表（clk_mgr+0x130，128B）
    uint32_t bwValid;            // 1 = clk_mgr 指针有效
    uint32_t bwNonZeroDwords;    // 128B 中非零 dword 数
    uint32_t bwFirstNonZero;     // 首非零 dword 值（无则 0）
    uint32_t bwLastNonZero;      // 末非零 dword 值（无则 0）
    // （甲）线 B：pp_smu->f38
    uint32_t ppValid;            // 1 = pp_smu 指针有效
    uint64_t ppF38;              // pp_smu->f38 值
    uint32_t ppF38IsOurs;        // 1 = f38 == ourCallbackAddr
    // （甲）线 C：回调调用计数（调用方传入）
    uint64_t overlayCalls;       // gNRedPpSmuOverlayCalls
    // （乙）线 D：dc_context->f88 指向对象头部
    uint32_t ctxValid;           // 1 = dc_ctx 指针有效
    uint64_t ctxF88;             // dc_ctx->f88（0x58）
    uint32_t p58Valid;           // 1 = f88 指针有效
    uint64_t p58Vtable;          // p58+0
    uint64_t p58F8;              // p58+0x8
    uint64_t p58F20;             // p58+0x20
    uint64_t p58F24;             // p58+0x24
    uint64_t p58F48;             // p58+0x30（f48 指针）
    // （乙）线 E：f48 指向对象（若非 0 且有效）
    uint32_t p30Valid;           // 1 = f48 非 0 且有效
    uint64_t p30Vtable;          // p30+0
    uint64_t p30F8;              // p30+0x8
    uint64_t p30F20;             // p30+0x20
    uint64_t p30F24;             // p30+0x24
    uint8_t  p30B118;            // p30+0x118（byte）
    uint8_t  p30B1BA;            // p30+0x1BA（byte）
};

// ── 只读序列（纯逻辑；read 回调不得写）──
// 约定：clk_mgr/ppSmu/dcCtx 任一为 0 ⇒ 对应分支 valid=0 且不读（to=1 语义）。
// bw 表 128B = clk_mgr+0x130 起 16×U64；首/末非零 dword 按 dword 序取。
inline ClkLayoutReadout runClkLayoutReadout(uint64_t clkMgr, uint64_t ppSmu, uint64_t dcCtx,
                                            uint64_t ourCallbackAddr, uint64_t overlayCalls,
                                            ClkReadU64Fn readU64, ClkReadU32Fn readU32,
                                            ClkReadU8Fn readU8)
{
    ClkLayoutReadout out{};
    out.overlayCalls = overlayCalls;

    // ── （甲）线 A：bw 表 ──
    if (clkMgr != 0) {
        uint32_t anyValid = 0;
        uint32_t nonZero = 0;
        uint32_t first = 0;
        uint32_t last = 0;
        for (uint32_t i = 0; i < 16; ++i) {
            uint32_t v = 0;
            const uint64_t raw = readU64(clkMgr + 0x130 + i * 8, &v);
            anyValid |= v;
            if (!v) continue;  // 该 dword 不可读 ⇒ 不计入
            const uint32_t lo = static_cast<uint32_t>(raw & 0xFFFFFFFFu);
            const uint32_t hi = static_cast<uint32_t>(raw >> 32);
            if (lo != 0) { if (!nonZero) first = lo; last = lo; ++nonZero; }
            if (hi != 0) { if (!nonZero) first = hi; last = hi; ++nonZero; }
        }
        out.bwValid         = anyValid;
        out.bwNonZeroDwords = nonZero;
        out.bwFirstNonZero  = first;
        out.bwLastNonZero   = last;
    } else {
        out.bwValid = 0;
    }

    // ── （甲）线 B：pp_smu->f38 ──
    if (ppSmu != 0) {
        uint32_t v = 0;
        const uint64_t f38 = readU64(ppSmu + 0x38, &v);
        out.ppValid  = v;
        out.ppF38    = f38;
        out.ppF38IsOurs = (v && f38 == ourCallbackAddr) ? 1u : 0u;
    } else {
        out.ppValid = 0;
    }

    // ── （乙）线 D：dc_context->f88 ──
    if (dcCtx != 0) {
        uint32_t v = 0;
        const uint64_t f88 = readU64(dcCtx + 0x58, &v);
        out.ctxValid = v;
        out.ctxF88   = f88;
        if (v && f88 != 0) {
            uint32_t v58 = 0;
            out.p58Valid  = 1;
            out.p58Vtable = readU64(f88 + 0x00, &v58);
            out.p58F8     = readU64(f88 + 0x08, &v58);
            out.p58F20    = readU64(f88 + 0x20, &v58);
            out.p58F24    = readU64(f88 + 0x24, &v58);
            out.p58F48    = readU64(f88 + 0x30, &v58);
        } else {
            out.p58Valid = 0;
        }
    } else {
        out.ctxValid = 0;
    }

    // ── （乙）线 E：f48 指向对象 ──
    if (out.p58Valid && out.p58F48 != 0) {
        uint32_t v30 = 0;
        out.p30Valid  = 1;
        out.p30Vtable = readU64(out.p58F48 + 0x00, &v30);
        out.p30F8     = readU64(out.p58F48 + 0x08, &v30);
        out.p30F20    = readU64(out.p58F48 + 0x20, &v30);
        out.p30F24    = readU64(out.p58F48 + 0x24, &v30);
        out.p30B118   = readU8(out.p58F48 + 0x118, &v30);
        out.p30B1BA   = readU8(out.p58F48 + 0x1BA, &v30);
    } else {
        out.p30Valid = 0;
    }

    return out;
}

#ifndef FW_CLK_LAYOUT_NO_KEXT
// ── kext 接线（仅 kext 环境编译）──
// 注：本文件从 X6000FB.cpp（全局命名空间）包含，故自行开 namespace fw（本文件顶部已开）。
#include "../HWLibs.hpp"          // NRED_TRACE

// 内核侧安全读取：调用前必须已判 isKernelPtr（不 deref 无效指针）；valid 恒 1。
static inline uint64_t clkLayoutReadU64(const uint64_t addr, uint32_t* const valid)
{
    *valid = 1;
    return *reinterpret_cast<const volatile uint64_t*>(addr);
}
static inline uint32_t clkLayoutReadU32(const uint64_t addr, uint32_t* const valid)
{
    *valid = 1;
    return *reinterpret_cast<const volatile uint32_t*>(addr);
}
static inline uint8_t clkLayoutReadU8(const uint64_t addr, uint32_t* const valid)
{
    *valid = 1;
    return *reinterpret_cast<const volatile uint8_t*>(addr);
}

// 内核指针判据（沿用既有铁律/阈值：>= 0xffffff7f80000000）。
static inline bool clkLayoutIsKernelPtr(const uint64_t p)
{
    return p >= 0xffffff7f80000000ULL;
}

// A-12 读数仪表（kext 入口；由 X6000FB::wrapDcClkMgrCreate 门控调用，org 调用返回后）。
// @param clkMgr       dc_clk_mgr_create 返回值（rn_clk_mgr 对象）
// @param ppSmu        dc_clk_mgr_create 第 2 参（pp_smu_funcs）
// @param dcCtx        dc_clk_mgr_create 第 1 参（dc_context）
// @param ourCallback  A-6 回调 NRedPpSmuOverlayGetDpmClockTable 地址（f38 判读基准）
// @param overlayCalls A-6 回调调用计数（gNRedPpSmuOverlayCalls）
inline void nredClkLayoutReadoutHook(const uint64_t clkMgr, const uint64_t ppSmu,
                                     const uint64_t dcCtx, const uint64_t ourCallback,
                                     const uint64_t overlayCalls)
{
    // 任一指针无效 ⇒ 传 0 ⇒ 纯逻辑对应分支 valid=0（to=1 语义）；不 deref 无效指针。
    const uint64_t ckMgr = clkLayoutIsKernelPtr(clkMgr) ? clkMgr : 0;
    const uint64_t ckPp  = clkLayoutIsKernelPtr(ppSmu)  ? ppSmu  : 0;
    const uint64_t ckCtx = clkLayoutIsKernelPtr(dcCtx)  ? dcCtx  : 0;

    const ClkLayoutReadout r = runClkLayoutReadout(ckMgr, ckPp, ckCtx,
                                                   ourCallback, overlayCalls,
                                                   &clkLayoutReadU64, &clkLayoutReadU32,
                                                   &clkLayoutReadU8);

    // （甲）线：bw 表 + f38 + 回调计数（to= 标记指针无效）
    NRED_TRACE("clk-layout-A: bw_nonzero=%u bw_first=0x%X bw_last=0x%X bw_to=%u"
               " | pp_f38=0x%llX ours=%u to=%u | calls=%llu",
               r.bwNonZeroDwords, r.bwFirstNonZero, r.bwLastNonZero, r.bwValid ? 0u : 1u,
               (unsigned long long)r.ppF38, r.ppF38IsOurs, r.ppValid ? 0u : 1u,
               (unsigned long long)r.overlayCalls);
    // （乙）线：ctx->f88 头部
    NRED_TRACE("clk-layout-B: ctx58=0x%llX to=%u | p58 vt=0x%llX f8=0x%llX f20=0x%llX f24=0x%llX"
               " f48=0x%llX to=%u",
               (unsigned long long)r.ctxF88, r.ctxValid ? 0u : 1u,
               (unsigned long long)r.p58Vtable, (unsigned long long)r.p58F8,
               (unsigned long long)r.p58F20, (unsigned long long)r.p58F24,
               (unsigned long long)r.p58F48, r.p58Valid ? 0u : 1u);
    // （乙）线：f48 对象重点字节
    NRED_TRACE("clk-layout-C: p30 vt=0x%llX f8=0x%llX f20=0x%llX f24=0x%llX"
               " b118=0x%02X b1BA=0x%02X to=%u",
               (unsigned long long)r.p30Vtable, (unsigned long long)r.p30F8,
               (unsigned long long)r.p30F20, (unsigned long long)r.p30F24,
               r.p30B118, r.p30B1BA, r.p30Valid ? 0u : 1u);
}

#endif  // FW_CLK_LAYOUT_NO_KEXT

}  // namespace fw
