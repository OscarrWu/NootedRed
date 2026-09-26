// DCN 3.1.4 DCCG 像素率分频序列生成器
//
// 忠实翻译自 Linux（逐行对照，出处逐条注明）：
//   dccg/dcn314/dcn314_dccg.c:101-146  (dccg314_set_pixel_rate_div)
//   dccg/dcn314/dcn314_dccg.c:59-99    (dccg314_get_pixel_rate_div)
//   include/asic_reg/dcn/dcn_3_1_4_offset.h:1389-1440 (regOTG_PIXEL_RATE_DIV / regOTG0..3_PIXEL_RATE_CNTL)
//   include/asic_reg/dcn/dcn_3_1_4_sh_mask.h:8204-8220 (8 个 OTGn_PIXEL_RATE_DIVK1/K2 位域)
//
// 为什么在第七步（集成）补这一块：
//   `dccg314_set_pixel_rate_div` 是 Linux `dcn20_enable_stream_timing` 的**首步**
//   （dcn20_hwseq.c:846-847，dcn314_init.c:132 把 enable_stream_timing 指向它），
//   即"启用一条显示流"必经的第一步。第五步只翻译了时钟主流程（VBIOSSMC 消息），
//   第六步翻译了 ODM；**像素率分频是第三条、也是最后一条初始化期必须下发的 DCCG 序列**。
//   本生成器与 `Dcn314OdmSeq.hpp` 同构：只产出 RegOp，由 `InjectedRegSink` 消费。
//
// ⚠️ 两种写宏的形态差异（同 Dcn314OdmSeq.hpp 的说明）：
//   · `REG_UPDATE_2(...)` → 读-改-写一次（两个字段合并成一次 Update）⇒ `regUpdate`
//   · `REG_GET_2(...)`    → 只读 ⇒ `regRead`
//   本生成器**只做写侧**；读侧（`get_pixel_rate_div` 的"当前值相同则跳过"判断）由
//   调用方在生成前用一条 `regRead` 取回——见 `generateSetPixelRateDivCond`。
//
// ⛔ 不含内核头文件、无动态分配、无异常、无浮点。
// ⛔ 禁止 <cstdint>/<cstddef>/std::（kext 环境没有它们）—— 用 <stdint.h>/<stddef.h> + 全局类型名。
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.

#pragma once

#include "RegOp.hpp"

#include <stddef.h>
#include <stdint.h>

namespace display {
namespace dcn314_dccg {

// ── 寄存器偏移（值取自 src/NootedRed/Regs/DCN314.hpp；那里含 IOKit 头，故此处复制为 uint32_t）──
//    两侧一致性由测试断言把关（见 tests/test_dcn314_dccg_seq.cpp 的静态对照）。
constexpr uint32_t kOtgPixelRateDiv    = 0x006F;  // regOTG_PIXEL_RATE_DIV（BASE_IDX=1；OTG0..3 打包单寄存器）
constexpr uint32_t kOtg0PixelRateCntl  = 0x0080;  // regOTG0_PIXEL_RATE_CNTL（BASE_IDX=1）
constexpr uint32_t kOtgPixelRateCntlStride = 0x4; // OTG1/2/3 = 0x84 / 0x88 / 0x8C

// ── 字段掩码/移位（逐条取自 Linux dcn_3_1_4_sh_mask.h:8205-8220）──
// 注意 OTG0..3 的四组位域**互不重叠**，打包在同一个 32 位寄存器里：
//   OTG0 K1 = bit0、OTG0 K2 = bit1-2、OTG1 K1 = bit3、OTG1 K2 = bit4-5、
//   OTG2 K1 = bit6、OTG2 K2 = bit7-8、OTG3 K1 = bit9、OTG3 K2 = bit10-11
constexpr uint32_t kOtg0K1Shift = 0x0;
constexpr uint32_t kOtg0K1Mask  = 0x00000001u;
constexpr uint32_t kOtg0K2Shift = 0x1;
constexpr uint32_t kOtg0K2Mask  = 0x00000006u;
constexpr uint32_t kOtg1K1Shift = 0x3;
constexpr uint32_t kOtg1K1Mask  = 0x00000008u;
constexpr uint32_t kOtg1K2Shift = 0x4;
constexpr uint32_t kOtg1K2Mask  = 0x00000030u;
constexpr uint32_t kOtg2K1Shift = 0x6;
constexpr uint32_t kOtg2K1Mask  = 0x00000040u;
constexpr uint32_t kOtg2K2Shift = 0x7;
constexpr uint32_t kOtg2K2Mask  = 0x00000180u;
constexpr uint32_t kOtg3K1Shift = 0x9;
constexpr uint32_t kOtg3K1Mask  = 0x00000200u;
constexpr uint32_t kOtg3K2Shift = 0xA;
constexpr uint32_t kOtg3K2Mask  = 0x00000C00u;

// Linux `enum pixel_rate_div`（dccg.h:67-72）—— 值直接写入寄存器域，**非连续**，禁止"补齐"。
constexpr uint32_t kPixelRateDivBy1 = 0;    // PIXEL_RATE_DIV_BY_1
constexpr uint32_t kPixelRateDivBy2 = 1;    // PIXEL_RATE_DIV_BY_2
constexpr uint32_t kPixelRateDivBy4 = 3;    // PIXEL_RATE_DIV_BY_4
constexpr uint32_t kPixelRateDivNA  = 0xF;  // PIXEL_RATE_DIV_NA（非法：K1 域只有 1 位、K2 域只有 2 位）

// 取指定 OTG 实例的 K1/K2 位域（shift/mask）。otgInst > 3 时返回 false。
inline bool otgRateDivFields(uint32_t otgInst, uint32_t* k1Shift, uint32_t* k1Mask, uint32_t* k2Shift,
                             uint32_t* k2Mask)
{
    switch (otgInst) {
    case 0: *k1Shift = kOtg0K1Shift; *k1Mask = kOtg0K1Mask; *k2Shift = kOtg0K2Shift; *k2Mask = kOtg0K2Mask; return true;
    case 1: *k1Shift = kOtg1K1Shift; *k1Mask = kOtg1K1Mask; *k2Shift = kOtg1K2Shift; *k2Mask = kOtg1K2Mask; return true;
    case 2: *k1Shift = kOtg2K1Shift; *k1Mask = kOtg2K1Mask; *k2Shift = kOtg2K2Shift; *k2Mask = kOtg2K2Mask; return true;
    case 3: *k1Shift = kOtg3K1Shift; *k1Mask = kOtg3K1Mask; *k2Shift = kOtg3K2Shift; *k2Mask = kOtg3K2Mask; return true;
    default: return false;
    }
}

// 取指定 OTG 实例的 PIXEL_RATE_CNTL 绝对地址（BASE_IDX=1 的段内偏移；段基址由调用方加）。
inline bool otgPixelRateCntlOffset(uint32_t otgInst, uint32_t* off)
{
    if (otgInst > 3) { return false; }
    *off = kOtg0PixelRateCntl + kOtgPixelRateCntlStride * otgInst;
    return true;
}

// ── generateReadPixelRateDiv ─────────────────────────────────────────────────
//
// Linux `dccg314_get_pixel_rate_div`（dcn314_dccg.c:59-99）的"读当前值"半部分：
//   REG_GET_2(OTG_PIXEL_RATE_DIV, OTGn_..._DIVK1, &val_k1, OTGn_..._DIVK2, &val_k2);
// 只产出一个读 op；字段提取由调用方从 `sink.lastValue()` 做（同 Dcn314ClkMgrSeq 的做法）。
//   seg1Base : DCCG 所在段的基址（DCN_SEG1_BASE = 0xC0）
inline void generateReadPixelRateDiv(RegSeq& out, uint32_t seg1Base, uint32_t otgInst)
{
    uint32_t k1s = 0, k1m = 0, k2s = 0, k2m = 0;
    if (!otgRateDivFields(otgInst, &k1s, &k1m, &k2s, &k2m)) { return; }
    out.push(regRead(seg1Base + kOtgPixelRateDiv, "read_pixel_rate_div"));
}

// ── generateSetPixelRateDiv ──────────────────────────────────────────────────
//
// Linux `dccg314_set_pixel_rate_div`（dcn314_dccg.c:101-146）的写侧翻译。
//
// 原函数的三个门（本生成器**逐条保留语义**，但它们都依赖"读回值"，故拆成两个入口）：
//   ① `if (k1 == PIXEL_RATE_DIV_NA || k2 == PIXEL_RATE_DIV_NA) { BREAK_TO_DEBUGGER(); return; }`
//      —— 0xF 域宽放不下（K1 1 位、K2 2 位），Linux 直接放弃。**本函数同样拒绝**。
//   ② `dccg314_get_pixel_rate_div(...); if (k1 == cur_k1 && k2 == cur_k2) return;`
//      —— 与当前值相同则跳过。需要读回值 ⇒ 见下方 `generateSetPixelRateDivCond`。
//   ③ `switch (otg_inst) { 0..3: REG_UPDATE_2(OTG_PIXEL_RATE_DIV, OTGn_K1, k1, OTGn_K2, k2); }`
//      —— 一次读改写两个字段。RegOp 只有单掩码/单值 ⇒ 合并为一次 `Update`
//         （mask = K1.MASK | K2.MASK、value 按各自 shift 预先就位；与第六步
//          `mpc3_set_out_rate_control` 的合并方式一致）。
//
// 返回 false 表示"两个门之一拒绝，未产出任何 op"（调用方据此判定 no-op，与 Linux 同语义）。
inline bool generateSetPixelRateDiv(RegSeq& out, uint32_t seg1Base, uint32_t otgInst, uint32_t k1, uint32_t k2)
{
    uint32_t k1s = 0, k1m = 0, k2s = 0, k2m = 0;
    if (!otgRateDivFields(otgInst, &k1s, &k1m, &k2s, &k2m)) { return false; }   // otg_inst > 3 → BREAK_TO_DEBUGGER

    // 门 ①：Linux dcn314_dccg.c:113-116。
    if (k1 == kPixelRateDivNA || k2 == kPixelRateDivNA) { return false; }

    // 门 ③：REG_UPDATE_2 合并成一次 Update。
    //   K1/K2 的域互不重叠，故 mask 可直接相或；value 各自左移到自己的 shift 位。
    const uint32_t mask = k1m | k2m;
    const uint32_t val  = ((k1 << k1s) & k1m) | ((k2 << k2s) & k2m);
    out.push(regUpdate(seg1Base + kOtgPixelRateDiv, mask, 0, val, "set_pixel_rate_div"));
    return true;
}

// ── generateSetPixelRateDivCond ──────────────────────────────────────────────
//
// 带门 ② 的完整形态：先读回当前值，再按"与当前值不同"决定是否写。
//
// 为什么要单独一个入口而不是让生成器内部读：RegOp 序列是**静态**的，生成器不知道读回值
// （读值依赖真实硬件，无法离线预测 —— 这就是 Poll 作为一等 op 存在的原因）。
// 因此把门 ② 的判定交给调用方：调用方先在同一个 sink 上执行 `generateReadPixelRateDiv`，
// 取出字段值，与本函数比对后再决定是否执行本函数产出的序列。
//
// 本函数退化为"总是写"（门 ② 的跳过判断在调用方），语义等价、且保持生成器纯函数。
// 若 k1/k2 为 NA，返回 false（门 ①）。
inline bool generateSetPixelRateDivCond(RegSeq& out, uint32_t seg1Base, uint32_t otgInst, uint32_t k1, uint32_t k2)
{
    return generateSetPixelRateDiv(out, seg1Base, otgInst, k1, k2);
}

// ── 字段提取 helper（供调用方从读回值取 K1/K2）──────────────────────────────
//
// 对应 Linux `REG_GET_2` 之后的两个赋值（dcn314_dccg.c:95-97 的 *k1 = val_k1 等）。
inline uint32_t extractK1(uint32_t otgInst, uint32_t regValue)
{
    uint32_t k1s = 0, k1m = 0, k2s = 0, k2m = 0;
    if (!otgRateDivFields(otgInst, &k1s, &k1m, &k2s, &k2m)) { return kPixelRateDivNA; }
    return (regValue & k1m) >> k1s;
}

inline uint32_t extractK2(uint32_t otgInst, uint32_t regValue)
{
    uint32_t k1s = 0, k1m = 0, k2s = 0, k2m = 0;
    if (!otgRateDivFields(otgInst, &k1s, &k1m, &k2s, &k2m)) { return kPixelRateDivNA; }
    return (regValue & k2m) >> k2s;
}

}  // namespace dcn314_dccg
}  // namespace display
