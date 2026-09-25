// PixelDiv —— 编译单元级不变量（策略实现内联于 PixelDiv.hpp）
//
// 为什么本编译单元没有函数体：
//   两个策略函数是 constexpr —— "可编译期求值"本身就是"零寄存器依赖、
//   零动态分配、零副作用"的编译期证明，而 constexpr 函数的跨编译单元
//   常量求值要求定义可见，因此实现内联于 PixelDiv.hpp（与
//   DisplaySeq/RegOp.hpp 的过滤函数同一理由；内核侧亦不引入额外符号）。
//
// 本文件承载两类内容，在每一个消费者（含 kext 构建）的编译期重复把关：
//   1. ABI/布局不变量 —— 枚举底层类型与结构布局防漂移；
//   2. 关键分支的编译期抽查 —— 与 tests/test_pixeldiv.cpp 的运行时断言同源。
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#include "PixelDiv.hpp"

#include <type_traits>

namespace pixdiv {

// ── 枚举宽度：Linux 侧为 C 枚举（LP64 上 4 字节）。底层类型名（int /
//    unsigned int）由实现按枚举值域挑选（全非负值时 g++ 选 unsigned int），
//    对 ABI 有意义的是 4 字节宽度，逐枚举断言之。──
static_assert(sizeof(SignalType) == sizeof(std::uint32_t),
              "SignalType 宽度必须与 Linux C 枚举一致 (4 字节)");
static_assert(sizeof(PixelRateDiv) == sizeof(std::uint32_t),
              "PixelRateDiv 宽度必须与 Linux C 枚举一致 (4 字节)");
static_assert(sizeof(PixelEncoding) == sizeof(std::uint32_t),
              "PixelEncoding 宽度必须与 Linux C 枚举一致 (4 字节)");

// ── 结构布局：对应 core_types.h L437-440 两个 uint32 与策略返回三元组 ──
static_assert(sizeof(PixelRateDivider) == 2 * sizeof(std::uint32_t),
              "PixelRateDivider 布局必须等价 Linux struct pixel_rate_divider");
static_assert(sizeof(K1K2Result) == 3 * sizeof(std::uint32_t),
              "K1K2Result 应为三个 32 位返回量");

// ── 关键分支编译期抽查（与 tests/test_pixeldiv.cpp 运行时断言同源）──
namespace ct_check {

// 分支 1（L338-341）：HDMI FRL → (BY_1, BY_1)
constexpr K1K2Inputs frl{SIGNAL_TYPE_HDMI_FRL, PIXEL_ENCODING_RGB, false, false, 1};
static_assert(calculateDccgK1K2Values(frl).k1Div == PIXEL_RATE_DIV_BY_1,
              "FRL → k1=BY_1 (L340)");
static_assert(calculateDccgK1K2Values(frl).k2Div == PIXEL_RATE_DIV_BY_1,
              "FRL → k2=BY_1 (L341)");

// 分支 2（L342-347）：TMDS + YCBCR420 → k2=BY_2；非 420 → BY_4
constexpr K1K2Inputs hdmi420{SIGNAL_TYPE_HDMI_TYPE_A, PIXEL_ENCODING_YCBCR420, false, false, 1};
static_assert(calculateDccgK1K2Values(hdmi420).k2Div == PIXEL_RATE_DIV_BY_2,
              "TMDS 420 → k2=BY_2 (L345)");
constexpr K1K2Inputs hdmiRgb{SIGNAL_TYPE_HDMI_TYPE_A, PIXEL_ENCODING_RGB, false, false, 1};
static_assert(calculateDccgK1K2Values(hdmiRgb).k2Div == PIXEL_RATE_DIV_BY_4,
              "TMDS RGB → k2=BY_4 (L347)");

// 分支 3（L348-357）：odm==2 精确等于降 BY_2；odm=4 保持 BY_4
constexpr K1K2Inputs dpOdm2{SIGNAL_TYPE_DISPLAY_PORT, PIXEL_ENCODING_RGB, false, false, 2};
static_assert(calculateDccgK1K2Values(dpOdm2).k2Div == PIXEL_RATE_DIV_BY_2,
              "DP odm=2 → k2=BY_2 (L355-356)");
constexpr K1K2Inputs dpOdm4{SIGNAL_TYPE_DISPLAY_PORT, PIXEL_ENCODING_RGB, false, false, 4};
static_assert(calculateDccgK1K2Values(dpOdm4).k2Div == PIXEL_RATE_DIV_BY_4,
              "DP odm=4 → k2=BY_4 (精确等于语义)");

// 打包层（L366-385）与策略层一致；get_odm_config 纯化替代的数值语义
constexpr PixelRateDivider div = calculatePixRateDivider(dpOdm2);
static_assert(div.divFactor1 == PIXEL_RATE_DIV_BY_1 && div.divFactor2 == PIXEL_RATE_DIV_BY_2,
              "打包层 L382-383 与策略层一致");
static_assert(odmCombineFactorFromNextPipes(0) == 1, "get_odm_config L152: 顶 pipe 必占 1");
static_assert(odmCombineFactorFromNextPipes(1) == 2, "get_odm_config L164-168: 2 段合并");
static_assert(odmCombineFactorFromNextPipes(3) == 4, "get_odm_config L164-168: 4 段合并");

}  // namespace ct_check

}  // namespace pixdiv
