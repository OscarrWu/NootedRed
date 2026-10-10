// NRedTmrAddrReadout.hpp —— A-23 三个只读仪表（默认关、仅观测）
//
// 门控：`-NRedTmrAddrReadout`（默认关、仅观测）。门控假 ⇒ 本封装不被调用 ⇒
//   **零 MMIO／零写／零行为差异**（机械自证）。
//
// 三个输出：
//   ① `SETUP_TMR` 帧构造处的 `buf_phy` 与 `sys_phy` **实际值**（值本身）；
//   ② `tmr_mc % tmr_size`（**自然对齐余数**）；
//   ③ 该 TMR 缓冲是否落在 `fbLocationBase` 窗内（用**既有内存字段** `fbLocationBase`/`fbLocationSize` 判定）。
//
// ⚠️ **读 0 陷阱（必须遵守）**：`fbLocationBase`/`fbLocationSize` 由 `X5000::fixedGetDisplayInfo`
//   捕获，**捕获时点未证** ⇒ **读数为 0 只能读作"尚未捕获"**，**不得**当作"基址为 0"/"窗长为 0"。
//   ⇒ 本卡第 ③ 项在基址或窗长为 0 时输出 `unknown`（不可判定），而非 `in`/`out`。
//
// 实现约束：
//   · **禁止**新增寄存器探针（红线①）⇒ 第 ③ 项只用既有内存字段；
//   · **禁止**在任何早期路径调用文件系统写 ⇒ 输出经 A-22 的"内存优先＋安全时点刷盘"（`nredTraceFlush`）。
//
// ⛔ kext 环境可编译：禁 `<cstdint>`/`<cstddef>`/`std::`（CI run #220 纪律）；用 `<stdint.h>`。

#pragma once

#include <stdint.h>

namespace nred {

// ── 三个只读仪表的读数结果 ──
struct TmrAddrReadout {
    // ① 值本身
    uint64_t buf_phy;      // SETUP_TMR 帧的 buf_phy_addr（payload[0..1] 拼回）
    uint64_t sys_phy;      // SETUP_TMR 帧的 system_phy_addr（payload[4..5] 拼回）
    uint32_t tmr_size;     // 缓冲区大小

    // ② 自然对齐余数
    uint64_t align_rem;    // tmr_mc % tmr_size（tmr_size==0 时为 UINT64_MAX 哨兵）

    // ③ 窗内判定
    //   0 = out（明确不在窗内）；1 = in（明确在窗内）；2 = unknown（基址或窗长为 0 ⇒ 尚未捕获）
    uint32_t in_window;
    uint64_t win_base;     // 观测到的 fbLocationBase（0 ⇒ 尚未捕获）
    uint64_t win_size;     // 观测到的窗长（0 ⇒ 尚未捕获）
};

// ── 纯逻辑组装（用户态可测；不触碰 MMIO/文件/寄存器）──
// @param buf_phy   SETUP_TMR 帧的 buf_phy_addr 值
// @param sys_phy   SETUP_TMR 帧的 system_phy_addr 值
// @param tmr_size  缓冲区大小
// @param win_base  既有内存字段 fbLocationBase（0 ⇒ 尚未捕获）
// @param win_size  既有内存字段 fbLocationSize（0 ⇒ 尚未捕获）
// @param tmr_mc    用于对齐余数计算的地址（通常与 buf_phy 同）
inline TmrAddrReadout runTmrAddrReadout(const uint64_t buf_phy, const uint64_t sys_phy,
                                        const uint32_t tmr_size, const uint64_t win_base,
                                        const uint64_t win_size, const uint64_t tmr_mc)
{
    TmrAddrReadout out{};
    out.buf_phy  = buf_phy;
    out.sys_phy  = sys_phy;
    out.tmr_size = tmr_size;
    out.win_base = win_base;
    out.win_size = win_size;

    // ② 自然对齐余数（tmr_size==0 ⇒ 哨兵，表示不可计算）
    if (tmr_size == 0) {
        out.align_rem = ~static_cast<uint64_t>(0);   // UINT64_MAX 哨兵
    } else {
        out.align_rem = tmr_mc % static_cast<uint64_t>(tmr_size);
    }

    // ③ 窗内判定（0 或窗长 0 ⇒ unknown = 2）
    if (win_base == 0 || win_size == 0) {
        out.in_window = 2;   // unknown：尚未捕获，不得判为 in/out
    } else if (tmr_mc >= win_base && tmr_mc < win_base + win_size) {
        out.in_window = 1;   // in
    } else {
        out.in_window = 0;   // out
    }

    return out;
}

}  // namespace nred
