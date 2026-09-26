// PixelDiv —— DCN314 像素分频纯策略函数
//
// 把 Linux amdgpu DCN3.1.4 显示管线的 K1/K2 与 pixel rate divider 决策逻辑
// 移植为"零寄存器依赖"的纯策略函数：输入是由调用方预解析的结构化
// K1K2Inputs，输出是 DCCG K1/K2 分频因子。函数体不触碰任何硬件寄存器、
// 不遍历 pipe 链、不做动态分配，全部 constexpr（可编译期求值本身即
// "零副作用"的编译期证明）。
//
// ⛔ 两条硬约束（与 DisplaySeq/RegOp.hpp 相同）：
//   1. 不含任何内核头文件（IOKit 等）→ 必须能在分析机用户态直接编译；
//   2. 零动态分配、零异常 —— 同一份代码将来跑在 kext 内核态。
//
// 权威来源（Linux amdgpu，逐分支对照见 tests/test_pixeldiv.cpp）：
//   display/dc/hwss/dcn314/dcn314_hwseq.c
//     - dcn314_calculate_dccg_k1_k2_values()  L329-364   （分支策略）
//     - dcn314_calculate_pix_rate_divider()   L366-385   （打包层）
//     - get_odm_config()                      L150-171   （ODM 段数，纯化替代见下）
//   display/include/signal_types.h            L36-176    （信号枚举与分类 helper）
//   display/dc/inc/hw/dccg.h                  L67-72     （K1/K2 分频因子枚举）
//   display/dc/dc_hw_types.h                  L805-812   （像素编码枚举）
//   display/dc/inc/core_types.h               L437-440   （PixelRateDivider 结构）
//
// 去 vtable 化的输入解析责任（调用方在调用前完成，函数内不复现）：
//   ┌─────────────────────┬──────────────────────────────────────────────────┐
//   │ K1K2Inputs 字段      │ Linux 原始取值处                                  │
//   ├─────────────────────┼──────────────────────────────────────────────────┤
//   │ signal              │ pipe_ctx->stream->signal               （L338 等）│
//   │ pixelEncoding       │ stream->timing.pixel_encoding          （L344）   │
//   │ is128b132bSignal    │ dc->link_srv->dp_is_128b_132b_signal()（L339）    │
//   │                     │   纯化：布尔预解析。真值条件见                        │
//   │                     │   link_dp_capability.c L384-391（HPO DP 编码器     │
//   │                     │   就绪且信号属 DP 族）。                            │
//   │ twoPixPerContainer  │ tg->funcs->is_two_pixels_per_container()（L335）  │
//   │                     │   纯化：布尔预解析。真值条件见                       │
//   │                     │   dcn10_optc.c L1633-1640（4:2:0，或 DSC 且       │
//   │                     │   4:2:2 非 simple —— 两像素一容器）。              │
//   │ odmCombineFactor    │ get_odm_config(pipe_ctx, NULL)         （L336）   │
//   │                     │   纯化：调用方传段数（odmCombineFactorFromNextPipes）│
//   └─────────────────────┴──────────────────────────────────────────────────┘
//
// 接口与 Linux 原型的映射（草案出参风格 → 值返回）：
//   unsigned int dcn314_calculate_dccg_k1_k2_values(pipe_ctx, *k1_div, *k2_div)
//       → K1K2Result calculateDccgK1K2Values(const K1K2Inputs&)
//         返回结构同时承载原返回值 odm_combine_factor（L363）与两个出参。
//   void dcn314_calculate_pix_rate_divider(dc, context, stream)
//       → PixelRateDivider calculatePixRateDivider(const K1K2Inputs&)
//         原 L376 的 OTG master 资源查找与 L379 的 vtable 存在性检查
//         属副作用/资源遍历，由调用方承担；这里只保留 L380-383 的决策。
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once

#include <stdint.h>

namespace pixdiv {

// ── enum signal_type（signal_types.h L36-48，枚举成员名与数值逐项保持原样）──
enum SignalType {
    SIGNAL_TYPE_NONE            = 0L,       /* no signal */
    SIGNAL_TYPE_DVI_SINGLE_LINK = (1 << 0),
    SIGNAL_TYPE_DVI_DUAL_LINK   = (1 << 1),
    SIGNAL_TYPE_HDMI_TYPE_A     = (1 << 2),
    SIGNAL_TYPE_LVDS            = (1 << 3),
    SIGNAL_TYPE_RGB             = (1 << 4),
    SIGNAL_TYPE_DISPLAY_PORT    = (1 << 5),
    SIGNAL_TYPE_DISPLAY_PORT_MST = (1 << 6),
    SIGNAL_TYPE_EDP             = (1 << 7),
    SIGNAL_TYPE_HDMI_FRL        = (1 << 8),
    SIGNAL_TYPE_VIRTUAL         = (1 << 9), /* Virtual Display */
};

// ── enum pixel_rate_div（dccg.h L67-72）──
// ⚠️ 数值非连续（BY_2=1、BY_4=3、NA=0xF）：这些值直接写入 DCCG 寄存器域，
//    必须保持 Linux 原值，禁止"补齐"成连续序列。
enum PixelRateDiv {
    PIXEL_RATE_DIV_BY_1 = 0,
    PIXEL_RATE_DIV_BY_2 = 1,
    PIXEL_RATE_DIV_BY_4 = 3,
    PIXEL_RATE_DIV_NA   = 0xF
};

// ── enum dc_pixel_encoding（dc_hw_types.h L805-812）──
enum PixelEncoding {
    PIXEL_ENCODING_UNDEFINED,
    PIXEL_ENCODING_RGB,
    PIXEL_ENCODING_YCBCR422,
    PIXEL_ENCODING_YCBCR444,
    PIXEL_ENCODING_YCBCR420,
    PIXEL_ENCODING_COUNT
};

// ── struct pixel_rate_divider（core_types.h L437-440）──
// 布局必须等价 Linux 侧两个 uint32（写入 pipe_ctx->pixel_rate_divider 的对应物）。
struct PixelRateDivider {
    uint32_t divFactor1;
    uint32_t divFactor2;
};

// ── 信号分类 helper（signal_types.h L81-176 的忠实复制，纯函数 constexpr 化）──

// L81-84 dc_is_hdmi_tmds_signal：只认 HDMI_TYPE_A（FRL 不属 TMDS）
constexpr bool isHdmiTmdsSignal(SignalType signal) {
    return (signal == SIGNAL_TYPE_HDMI_TYPE_A);
}

// L86-89 dc_is_hdmi_frl_signal
constexpr bool isHdmiFrlSignal(SignalType signal) {
    return ((signal == SIGNAL_TYPE_HDMI_FRL));
}

// L91-94 dc_is_hdmi_signal
constexpr bool isHdmiSignal(SignalType signal) {
    return (isHdmiTmdsSignal(signal) || isHdmiFrlSignal(signal));
}

// L102-107 dc_is_dp_signal：DP / EDP / MST 三者
constexpr bool isDpSignal(SignalType signal) {
    return (signal == SIGNAL_TYPE_DISPLAY_PORT ||
            signal == SIGNAL_TYPE_EDP ||
            signal == SIGNAL_TYPE_DISPLAY_PORT_MST);
}

// L119-129 dc_is_dvi_signal：switch 两例（单链/双链）
constexpr bool isDviSignal(SignalType signal) {
    switch (signal) {
    case SIGNAL_TYPE_DVI_SINGLE_LINK:
    case SIGNAL_TYPE_DVI_DUAL_LINK:
        return true;
    default:
        return false;
    }
}

// L173-176 dc_is_virtual_signal
constexpr bool isVirtualSignal(SignalType signal) {
    return (signal == SIGNAL_TYPE_VIRTUAL);
}

// ── get_odm_config（dcn314_hwseq.c L150-171）的纯化替代 ──
// 原函数从任一 pipe 沿 prev_odm_pipe 走到顶（L156-157），opp_count 从 1 起
// 数（L152"First pipe is always used"），再沿 next_odm_pipe 逐跳 +1
// （L164-168）。纯化后不遍历链表：调用方只传"顶 pipe 的 next_odm_pipe
// 跳数"，数值语义与原函数完全一致：
//   无 ODM 合并 → 0 跳 → 1；2 段合并（2:1）→ 1 跳 → 2；4 段合并（4:1）→ 3 跳 → 4
constexpr uint32_t odmCombineFactorFromNextPipes(uint32_t nextOdmPipeHops) {
    return 1 + nextOdmPipeHops;
}

// ── 预解析输入（去 vtable 化，字段取值责任见文件头表格）──
struct K1K2Inputs {
    SignalType signal;                    // L331/L338: pipe_ctx->stream->signal
    PixelEncoding pixelEncoding;          // L344: stream->timing.pixel_encoding
    bool is128b132bSignal = false;        // L339: dp_is_128b_132b_signal(pipe_ctx) 预解析
    bool twoPixPerContainer = false;      // L335: is_two_pixels_per_container(&timing) 预解析
    uint32_t odmCombineFactor = 1;   // L336: get_odm_config(pipe_ctx, NULL)，默认无合并
};

// ── 策略函数 1 的返回载体 ──
// 同时承载 Linux 原型的返回值（odm_combine_factor，L363）与两个出参
// （*k1_div / *k2_div，L329），三者一次算出、一致返回。
struct K1K2Result {
    uint32_t odmCombineFactor;
    uint32_t k1Div;
    uint32_t k2Div;
};

// 分支策略：dcn314_calculate_dccg_k1_k2_values（dcn314_hwseq.c L329-364）的
// 忠实搬运。逐分支对照：
//   L332-333 局部变量     → K1K2Inputs 字段（调用方预解析）
//   L338-341 分支 1       → 下方 if
//   L342-347 分支 2       → 下方 else if
//   L348-357 分支 3       → 下方 else if
//   L360-361 ASSERT(false)→ 不内嵌断言：三分支全不命中时 k1/k2 原样返回
//                          PIXEL_RATE_DIV_NA（L373-374 初值语义），由调用方
//                          决定处置（内核侧可在调用点 ASSERT）。
//   L363 返回值           → r.odmCombineFactor
inline constexpr K1K2Result calculateDccgK1K2Values(const K1K2Inputs& in)
{
    // L373-374：原实现依赖调用方先把 k1/k2 初始化为 NA；纯函数显式初始化，
    // 行为等价且对调用方更安全（未覆盖信号 → NA 原样可见）。
    uint32_t k1_div = PIXEL_RATE_DIV_NA;
    uint32_t k2_div = PIXEL_RATE_DIV_NA;

    const uint32_t odm_combine_factor = in.odmCombineFactor;   // L332/L336
    const bool two_pix_per_container = in.twoPixPerContainer;       // L333/L335

    if (isHdmiFrlSignal(in.signal) ||                               // L338
        in.is128b132bSignal) {                                      // L339
        k1_div = PIXEL_RATE_DIV_BY_1;                               // L340
        k2_div = PIXEL_RATE_DIV_BY_1;                               // L341
    } else if (isHdmiTmdsSignal(in.signal) ||                       // L342
               isDviSignal(in.signal)) {
        k1_div = PIXEL_RATE_DIV_BY_1;                               // L343
        if (in.pixelEncoding == PIXEL_ENCODING_YCBCR420) {          // L344
            k2_div = PIXEL_RATE_DIV_BY_2;                           // L345
        } else {
            k2_div = PIXEL_RATE_DIV_BY_4;                           // L347
        }
    } else if (isDpSignal(in.signal) ||                             // L348
               isVirtualSignal(in.signal)) {
        if (two_pix_per_container) {                                // L349
            k1_div = PIXEL_RATE_DIV_BY_1;                           // L350
            k2_div = PIXEL_RATE_DIV_BY_2;                           // L351
        } else {
            k1_div = PIXEL_RATE_DIV_BY_1;                           // L353
            k2_div = PIXEL_RATE_DIV_BY_4;                           // L354
            if (odm_combine_factor == 2) {                          // L355 精确等于
                k2_div = PIXEL_RATE_DIV_BY_2;                       // L356
            }
        }
    }
    // L360-361 的 ASSERT(false) 不内嵌（见上注释）；未覆盖信号 → (NA, NA)。

    return K1K2Result{odm_combine_factor, k1_div, k2_div};          // L363
}

// 打包层：dcn314_calculate_pix_rate_divider（dcn314_hwseq.c L366-385）的
// 纯策略化。原 L376 resource_get_otg_master_for_stream 的资源查找与 L379
// hws->funcs.calculate_dccg_k1_k2_values 的 vtable 存在性检查属副作用/资源
// 遍历，由调用方承担；本函数保留 L380-383：算出 k1/k2 并装入
// PixelRateDivider（等价于写 pipe_ctx->pixel_rate_divider.div_factor1/2）。
// L378 的 `if (pipe_ctx)` 判空同样属调用方的资源查找步骤，此处不涉及。
inline constexpr PixelRateDivider calculatePixRateDivider(const K1K2Inputs& in)
{
    // L373-374: k1_div / k2_div 初始为 NA（策略函数内部已保证等价初始化）
    const K1K2Result r = calculateDccgK1K2Values(in);               // L379-380

    return PixelRateDivider{r.k1Div, r.k2Div};                      // L382-383
}

}  // namespace pixdiv
