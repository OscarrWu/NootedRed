// 第二步验收测试：DCN314 像素分频纯策略函数
//
// 被测对象：src/NootedRed/PixelDiv/PixelDiv.hpp / PixelDiv.cpp
// 权威来源：Linux amdgpu
//   display/dc/hwss/dcn314/dcn314_hwseq.c
//     - dcn314_calculate_dccg_k1_k2_values()  L329-364
//     - dcn314_calculate_pix_rate_divider()   L366-385
//     - get_odm_config()                      L150-171（纯化替代：由调用方传入 ODM 段数）
//   display/include/signal_types.h            L36-176
//   display/dc/inc/hw/dccg.h                  L67-72
//   display/dc/dc_hw_types.h                  L805-812
//   display/dc/inc/core_types.h               L437-440
//
// 用例来源说明：Linux 侧没有这两个函数的单元测试（全仓检索仅见调用点），
// 因此全部用例从 L329-364 的分支逻辑推导，每个用例注明它覆盖的 Linux 分支行号。
//
// 接口说明（与草案的差异）：草案沿用 Linux 出参风格（*k1_div/*k2_div）；
// 正式版按本批任务约定改为"结构化输入 + 值返回"，并全部 constexpr 化——
// 分支逻辑逐行对应不变，出参/返回值的映射关系见 PixelDiv.hpp 注释。
//
// 编译运行（分析机）：
//   make -f src/NootedRed/PixelDiv/Makefile test
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#include "PixelDiv.hpp"

#include <cassert>
#include <cstdio>

using namespace pixdiv;

// ═══════════════════════════════════════════════════════════════════════════
// 0. 常量与 Linux 源文件一致（防止本地副本漂移——同 test_regop_seq.cpp 2a）
// ═══════════════════════════════════════════════════════════════════════════

// signal_types.h L36-48
static void test_signal_type_values_match_linux() {
    assert(SIGNAL_TYPE_NONE == 0L);
    assert(SIGNAL_TYPE_DVI_SINGLE_LINK == (1 << 0));
    assert(SIGNAL_TYPE_DVI_DUAL_LINK == (1 << 1));
    assert(SIGNAL_TYPE_HDMI_TYPE_A == (1 << 2));
    assert(SIGNAL_TYPE_LVDS == (1 << 3));
    assert(SIGNAL_TYPE_RGB == (1 << 4));
    assert(SIGNAL_TYPE_DISPLAY_PORT == (1 << 5));
    assert(SIGNAL_TYPE_DISPLAY_PORT_MST == (1 << 6));
    assert(SIGNAL_TYPE_EDP == (1 << 7));
    assert(SIGNAL_TYPE_HDMI_FRL == (1 << 8));
    assert(SIGNAL_TYPE_VIRTUAL == (1 << 9));
    std::puts("  [PASS] 0a signal_type 枚举值与 signal_types.h L36-48 一致");
}

// dccg.h L67-72：注意非连续（BY_2=1、BY_4=3、NA=0xF），寄存器域编码不可改动
static void test_pixel_rate_div_values_match_linux() {
    assert(PIXEL_RATE_DIV_BY_1 == 0);
    assert(PIXEL_RATE_DIV_BY_2 == 1);
    assert(PIXEL_RATE_DIV_BY_4 == 3);
    assert(PIXEL_RATE_DIV_NA == 0xF);
    std::puts("  [PASS] 0b pixel_rate_div 枚举值与 dccg.h L67-72 一致（非连续值）");
}

// dc_hw_types.h L805-812
static void test_pixel_encoding_values_match_linux() {
    assert(PIXEL_ENCODING_UNDEFINED == 0);
    assert(PIXEL_ENCODING_RGB == 1);
    assert(PIXEL_ENCODING_YCBCR422 == 2);
    assert(PIXEL_ENCODING_YCBCR444 == 3);
    assert(PIXEL_ENCODING_YCBCR420 == 4);
    assert(PIXEL_ENCODING_COUNT == 5);
    std::puts("  [PASS] 0c dc_pixel_encoding 枚举值与 dc_hw_types.h L805-812 一致");
}

// ═══════════════════════════════════════════════════════════════════════════
// 1. 信号分类 helper：signal_types.h L81-176 的忠实复制
// ═══════════════════════════════════════════════════════════════════════════

static void test_signal_helpers() {
    // L81-84 dc_is_hdmi_tmds_signal：只认 HDMI_TYPE_A（FRL 不算 TMDS）
    assert(isHdmiTmdsSignal(SIGNAL_TYPE_HDMI_TYPE_A));
    assert(!isHdmiTmdsSignal(SIGNAL_TYPE_HDMI_FRL));
    // L86-89 dc_is_hdmi_frl_signal
    assert(isHdmiFrlSignal(SIGNAL_TYPE_HDMI_FRL));
    assert(!isHdmiFrlSignal(SIGNAL_TYPE_HDMI_TYPE_A));
    // L91-94 dc_is_hdmi_signal = TMDS || FRL
    assert(isHdmiSignal(SIGNAL_TYPE_HDMI_TYPE_A));
    assert(isHdmiSignal(SIGNAL_TYPE_HDMI_FRL));
    assert(!isHdmiSignal(SIGNAL_TYPE_DVI_SINGLE_LINK));
    // L102-107 dc_is_dp_signal：DP / EDP / MST 三者
    assert(isDpSignal(SIGNAL_TYPE_DISPLAY_PORT));
    assert(isDpSignal(SIGNAL_TYPE_EDP));
    assert(isDpSignal(SIGNAL_TYPE_DISPLAY_PORT_MST));
    assert(!isDpSignal(SIGNAL_TYPE_HDMI_TYPE_A));
    // L119-129 dc_is_dvi_signal：switch 两例
    assert(isDviSignal(SIGNAL_TYPE_DVI_SINGLE_LINK));
    assert(isDviSignal(SIGNAL_TYPE_DVI_DUAL_LINK));
    assert(!isDviSignal(SIGNAL_TYPE_HDMI_TYPE_A));
    // L173-176 dc_is_virtual_signal
    assert(isVirtualSignal(SIGNAL_TYPE_VIRTUAL));
    assert(!isVirtualSignal(SIGNAL_TYPE_DISPLAY_PORT));
    std::puts("  [PASS] 1  信号分类 helper 与 signal_types.h L81-176 一致");
}

// ═══════════════════════════════════════════════════════════════════════════
// 2. 分支 1（L338-341）：HDMI FRL 或 DP 128b/132b → k1=BY_1, k2=BY_1
//    128b/132b 语义（link_dp_capability.c L384-391）：调用方预解析布尔，
//    Linux 侧该标志只可能在 DP 族信号上为真。
// ═══════════════════════════════════════════════════════════════════════════

static void test_branch1_frl_and_128b132b() {
    // L338: dc_is_hdmi_frl_signal → 走分支 1
    {
        K1K2Inputs in{};
        in.signal = SIGNAL_TYPE_HDMI_FRL;
        in.pixelEncoding = PIXEL_ENCODING_RGB;
        in.twoPixPerContainer = false;
        in.odmCombineFactor = 1;
        const K1K2Result r = calculateDccgK1K2Values(in);
        assert(r.k1Div == PIXEL_RATE_DIV_BY_1);
        assert(r.k2Div == PIXEL_RATE_DIV_BY_1);
        assert(r.odmCombineFactor == 1);      // L363：返回 odm_combine_factor
    }
    // L339: dp_is_128b_132b_signal（预解析为 true）→ 走分支 1（非 FRL 信号也命中）
    {
        K1K2Inputs in{};
        in.signal = SIGNAL_TYPE_DISPLAY_PORT;
        in.pixelEncoding = PIXEL_ENCODING_RGB;
        in.is128b132bSignal = true;
        in.twoPixPerContainer = true;         // 分支 1 优先，twoPix 与 odm 均被无视
        in.odmCombineFactor = 2;
        const K1K2Result r = calculateDccgK1K2Values(in);
        assert(r.k1Div == PIXEL_RATE_DIV_BY_1);
        assert(r.k2Div == PIXEL_RATE_DIV_BY_1);
    }
    // L339: eDP + 128b/132b（HPO 链路）同样命中分支 1
    {
        K1K2Inputs in{};
        in.signal = SIGNAL_TYPE_EDP;
        in.pixelEncoding = PIXEL_ENCODING_YCBCR420;
        in.is128b132bSignal = true;
        in.odmCombineFactor = 4;
        const K1K2Result r = calculateDccgK1K2Values(in);
        assert(r.k1Div == PIXEL_RATE_DIV_BY_1);
        assert(r.k2Div == PIXEL_RATE_DIV_BY_1);
    }
    std::puts("  [PASS] 2  分支 1（L338-341）：FRL / 128b-132b → (BY_1, BY_1)");
}

// ═══════════════════════════════════════════════════════════════════════════
// 3. 分支 2（L342-347）：HDMI TMDS 或 DVI → k1=BY_1；YCBCR420 → k2=BY_2，其余 k2=BY_4
// ═══════════════════════════════════════════════════════════════════════════

static void test_branch2_hdmi_tmds_dvi() {
    // L342 + L346: HDMI TMDS + 非 420（RGB）→ (BY_1, BY_4)
    {
        K1K2Inputs in{};
        in.signal = SIGNAL_TYPE_HDMI_TYPE_A;
        in.pixelEncoding = PIXEL_ENCODING_RGB;
        in.odmCombineFactor = 1;
        const K1K2Result r = calculateDccgK1K2Values(in);
        assert(r.k1Div == PIXEL_RATE_DIV_BY_1);
        assert(r.k2Div == PIXEL_RATE_DIV_BY_4);
    }
    // L344-345: HDMI TMDS + YCBCR420 → (BY_1, BY_2)
    {
        K1K2Inputs in{};
        in.signal = SIGNAL_TYPE_HDMI_TYPE_A;
        in.pixelEncoding = PIXEL_ENCODING_YCBCR420;
        in.odmCombineFactor = 1;
        const K1K2Result r = calculateDccgK1K2Values(in);
        assert(r.k1Div == PIXEL_RATE_DIV_BY_1);
        assert(r.k2Div == PIXEL_RATE_DIV_BY_2);
    }
    // L346: YCBCR422 也归"其余" → (BY_1, BY_4)
    {
        K1K2Inputs in{};
        in.signal = SIGNAL_TYPE_HDMI_TYPE_A;
        in.pixelEncoding = PIXEL_ENCODING_YCBCR422;
        const K1K2Result r = calculateDccgK1K2Values(in);
        assert(r.k1Div == PIXEL_RATE_DIV_BY_1);
        assert(r.k2Div == PIXEL_RATE_DIV_BY_4);
    }
    // L346: UNDEFINED 编码同样只有 420 命中 BY_2（L344 是精确比较）
    {
        K1K2Inputs in{};
        in.signal = SIGNAL_TYPE_HDMI_TYPE_A;
        in.pixelEncoding = PIXEL_ENCODING_UNDEFINED;
        const K1K2Result r = calculateDccgK1K2Values(in);
        assert(r.k1Div == PIXEL_RATE_DIV_BY_1);
        assert(r.k2Div == PIXEL_RATE_DIV_BY_4);
    }
    // L342: DVI 单链路 + 非 420 → (BY_1, BY_4)
    {
        K1K2Inputs in{};
        in.signal = SIGNAL_TYPE_DVI_SINGLE_LINK;
        in.pixelEncoding = PIXEL_ENCODING_YCBCR444;
        in.odmCombineFactor = 1;
        const K1K2Result r = calculateDccgK1K2Values(in);
        assert(r.k1Div == PIXEL_RATE_DIV_BY_1);
        assert(r.k2Div == PIXEL_RATE_DIV_BY_4);
    }
    // L342 + L344: DVI 双链路 + YCBCR420 → (BY_1, BY_2)
    {
        K1K2Inputs in{};
        in.signal = SIGNAL_TYPE_DVI_DUAL_LINK;
        in.pixelEncoding = PIXEL_ENCODING_YCBCR420;
        const K1K2Result r = calculateDccgK1K2Values(in);
        assert(r.k1Div == PIXEL_RATE_DIV_BY_1);
        assert(r.k2Div == PIXEL_RATE_DIV_BY_2);
    }
    // 忠实复刻 L338 的 || 语义：TMDS 信号若调用方误置 is128b132b（真实 HW 不可能，
    // dp_is_128b_132b_signal 自身要求 dc_is_dp_signal，见 link_dp_capability.c L388-390），
    // 分支 1 仍然先行命中。
    {
        K1K2Inputs in{};
        in.signal = SIGNAL_TYPE_HDMI_TYPE_A;
        in.pixelEncoding = PIXEL_ENCODING_RGB;
        in.is128b132bSignal = true;
        const K1K2Result r = calculateDccgK1K2Values(in);
        assert(r.k1Div == PIXEL_RATE_DIV_BY_1);
        assert(r.k2Div == PIXEL_RATE_DIV_BY_1);
    }
    std::puts("  [PASS] 3  分支 2（L342-347）：TMDS/DVI → k1=BY_1，420→BY_2 其余 BY_4");
}

// ═══════════════════════════════════════════════════════════════════════════
// 4. 分支 3（L348-357）：DP / VIRTUAL
//    - two_pix_per_container（L349-351）→ (BY_1, BY_2)
//    - 否则（L352-356）→ (BY_1, BY_4)；odm_combine_factor == 2 精确等于时 k2=BY_2
//    two_pix 语义（dcn10_optc.c L1633-1640）：4:2:0 或 DSC 4:2:2 非 simple，
//    由调用方预解析后传入。
// ═══════════════════════════════════════════════════════════════════════════

static void test_branch3_dp_virtual() {
    // L349-351: DP + two_pix → (BY_1, BY_2)，odm 无关
    {
        K1K2Inputs in{};
        in.signal = SIGNAL_TYPE_DISPLAY_PORT;
        in.pixelEncoding = PIXEL_ENCODING_RGB;
        in.twoPixPerContainer = true;
        in.odmCombineFactor = 1;
        const K1K2Result r = calculateDccgK1K2Values(in);
        assert(r.k1Div == PIXEL_RATE_DIV_BY_1);
        assert(r.k2Div == PIXEL_RATE_DIV_BY_2);
    }
    // L349-351 + L355: DP + two_pix + odm=2 → 仍 (BY_1, BY_2)（two_pix 分支优先）
    {
        K1K2Inputs in{};
        in.signal = SIGNAL_TYPE_DISPLAY_PORT;
        in.pixelEncoding = PIXEL_ENCODING_YCBCR420;
        in.twoPixPerContainer = true;
        in.odmCombineFactor = 2;
        const K1K2Result r = calculateDccgK1K2Values(in);
        assert(r.k1Div == PIXEL_RATE_DIV_BY_1);
        assert(r.k2Div == PIXEL_RATE_DIV_BY_2);
    }
    // L352-354: DP + !two_pix + odm=1（无 ODM 合并）→ (BY_1, BY_4)
    {
        K1K2Inputs in{};
        in.signal = SIGNAL_TYPE_DISPLAY_PORT;
        in.pixelEncoding = PIXEL_ENCODING_RGB;
        in.odmCombineFactor = 1;
        const K1K2Result r = calculateDccgK1K2Values(in);
        assert(r.k1Div == PIXEL_RATE_DIV_BY_1);
        assert(r.k2Div == PIXEL_RATE_DIV_BY_4);
    }
    // L355-356: DP + !two_pix + odm=2（双 pipe ODM 合并）→ k2 改写为 BY_2
    {
        K1K2Inputs in{};
        in.signal = SIGNAL_TYPE_DISPLAY_PORT;
        in.pixelEncoding = PIXEL_ENCODING_RGB;
        in.odmCombineFactor = 2;
        const K1K2Result r = calculateDccgK1K2Values(in);
        assert(r.k1Div == PIXEL_RATE_DIV_BY_1);
        assert(r.k2Div == PIXEL_RATE_DIV_BY_2);
    }
    // L355: odm=4（DCN314 最多 4 段）不满足 == 2 → k2 保持 BY_4。
    //       这是"精确等于"而非">="的关键分界：4 段合并不会降到 BY_2。
    {
        K1K2Inputs in{};
        in.signal = SIGNAL_TYPE_DISPLAY_PORT;
        in.pixelEncoding = PIXEL_ENCODING_RGB;
        in.odmCombineFactor = 4;
        const K1K2Result r = calculateDccgK1K2Values(in);
        assert(r.k1Div == PIXEL_RATE_DIV_BY_1);
        assert(r.k2Div == PIXEL_RATE_DIV_BY_4);
    }
    // L348: MST 同属 dc_is_dp_signal → 分支 3
    {
        K1K2Inputs in{};
        in.signal = SIGNAL_TYPE_DISPLAY_PORT_MST;
        in.pixelEncoding = PIXEL_ENCODING_RGB;
        in.odmCombineFactor = 1;
        const K1K2Result r = calculateDccgK1K2Values(in);
        assert(r.k1Div == PIXEL_RATE_DIV_BY_1);
        assert(r.k2Div == PIXEL_RATE_DIV_BY_4);
    }
    // L348: eDP 同属 dc_is_dp_signal；two_pix 命中 BY_2
    {
        K1K2Inputs in{};
        in.signal = SIGNAL_TYPE_EDP;
        in.pixelEncoding = PIXEL_ENCODING_YCBCR420;
        in.twoPixPerContainer = true;
        const K1K2Result r = calculateDccgK1K2Values(in);
        assert(r.k1Div == PIXEL_RATE_DIV_BY_1);
        assert(r.k2Div == PIXEL_RATE_DIV_BY_2);
    }
    // L348: VIRTUAL（无头/影子显示）→ (BY_1, BY_4)
    {
        K1K2Inputs in{};
        in.signal = SIGNAL_TYPE_VIRTUAL;
        in.pixelEncoding = PIXEL_ENCODING_RGB;
        in.odmCombineFactor = 1;
        const K1K2Result r = calculateDccgK1K2Values(in);
        assert(r.k1Div == PIXEL_RATE_DIV_BY_1);
        assert(r.k2Div == PIXEL_RATE_DIV_BY_4);
    }
    std::puts("  [PASS] 4  分支 3（L348-357）：DP/VIRTUAL 两像素容器与 ODM==2 语义");
}

// ═══════════════════════════════════════════════════════════════════════════
// 5. 三分支全不命中（L360-361 ASSERT(false) 的纯函数等价行为）
//    纯函数不内嵌断言（草案设计说明 L197-198）：k1=k2=NA 原样返回，
//    由调用方决定如何处置（内核侧可在调用点 ASSERT）。
// ═══════════════════════════════════════════════════════════════════════════

static void test_unhandled_signal_yields_na() {
    const SignalType unhandled[] = {
        SIGNAL_TYPE_NONE, SIGNAL_TYPE_LVDS, SIGNAL_TYPE_RGB,
    };
    for (const SignalType sig : unhandled) {
        K1K2Inputs in{};
        in.signal = sig;
        in.pixelEncoding = PIXEL_ENCODING_RGB;
        in.odmCombineFactor = 2;
        const K1K2Result r = calculateDccgK1K2Values(in);
        assert(r.k1Div == PIXEL_RATE_DIV_NA);
        assert(r.k2Div == PIXEL_RATE_DIV_NA);
        assert(r.odmCombineFactor == 2);      // L363：无条件返回 odm_combine_factor
    }
    std::puts("  [PASS] 5  未覆盖信号（NONE/LVDS/RGB）→ (NA, NA)，对应 L360-361");
}

// ═══════════════════════════════════════════════════════════════════════════
// 6. get_odm_config 纯化替代（L150-171）：本函数不做链遍历，odm 段数由调用方
//    传入；语义 = 1 + 从顶 pipe 起的 next_odm_pipe 跳数（L152、L164-168）。
//    返回值逐分支核对（L336 取值、L363 返回）。
// ═══════════════════════════════════════════════════════════════════════════

static void test_return_value_is_odm_combine_factor() {
    const unsigned int odmCases[] = {1, 2, 4};
    const SignalType sigCases[] = {
        SIGNAL_TYPE_HDMI_FRL, SIGNAL_TYPE_HDMI_TYPE_A,
        SIGNAL_TYPE_DISPLAY_PORT, SIGNAL_TYPE_VIRTUAL,
    };
    for (const SignalType sig : sigCases) {
        for (const unsigned int odm : odmCases) {
            K1K2Inputs in{};
            in.signal = sig;
            in.pixelEncoding = PIXEL_ENCODING_RGB;
            in.odmCombineFactor = odm;
            const K1K2Result r = calculateDccgK1K2Values(in);
            assert(r.odmCombineFactor == odm);  // L363：任何分支都原样返回
        }
    }
    std::puts("  [PASS] 6  返回 odm_combine_factor（L336/L363，1/2/4 段全分支）");
}

// 6b. get_odm_config 纯化替代 helper：顶 pipe 的 next_odm_pipe 跳数 → 段数
//     （L152"顶 pipe 必占 1"+ L164-168 逐跳 +1；0→1、1→2、3→4）。
//     产出值直接喂给分支 3 的 odm_combine_factor（L355 精确等于 2 降 BY_2）。
static void test_odm_combine_factor_helper() {
    assert(odmCombineFactorFromNextPipes(0) == 1);   // L152: 无 ODM 合并
    assert(odmCombineFactorFromNextPipes(1) == 2);   // L164-168: 2 段合并
    assert(odmCombineFactorFromNextPipes(3) == 4);   // L164-168: 4 段合并
    // 与分支 3 的衔接：跳数 1 → odm=2 → L355 命中 k2=BY_2
    K1K2Inputs in{};
    in.signal = SIGNAL_TYPE_DISPLAY_PORT;
    in.pixelEncoding = PIXEL_ENCODING_RGB;
    in.odmCombineFactor = odmCombineFactorFromNextPipes(1);
    assert(calculateDccgK1K2Values(in).k2Div == PIXEL_RATE_DIV_BY_2);
    std::puts("  [PASS] 6b get_odm_config 纯化替代（L150-171）：跳数→段数并衔接分支 3");
}

// ═══════════════════════════════════════════════════════════════════════════
// 7. 打包层（L366-385）：calculatePixRateDivider 把 k1/k2 装入
//    PixelRateDivider（等价于 L382-383 写 pipe_ctx->pixel_rate_divider）。
//    原 L376 的 OTG master 资源查找与 L379 的 vtable 存在性检查由调用方承担。
// ═══════════════════════════════════════════════════════════════════════════

static void test_pix_rate_divider_wrapper() {
    // 组合 1：HDMI FRL → (BY_1, BY_1)
    {
        K1K2Inputs in{};
        in.signal = SIGNAL_TYPE_HDMI_FRL;
        in.pixelEncoding = PIXEL_ENCODING_RGB;
        const PixelRateDivider out = calculatePixRateDivider(in);
        assert(out.divFactor1 == PIXEL_RATE_DIV_BY_1);
        assert(out.divFactor2 == PIXEL_RATE_DIV_BY_1);
    }
    // 组合 2：DP + ODM 2 段 → (BY_1, BY_2)
    {
        K1K2Inputs in{};
        in.signal = SIGNAL_TYPE_DISPLAY_PORT;
        in.pixelEncoding = PIXEL_ENCODING_RGB;
        in.odmCombineFactor = 2;
        const PixelRateDivider out = calculatePixRateDivider(in);
        assert(out.divFactor1 == PIXEL_RATE_DIV_BY_1);
        assert(out.divFactor2 == PIXEL_RATE_DIV_BY_2);
    }
    // 组合 3：DP + odm=4 → (BY_1, BY_4)
    {
        K1K2Inputs in{};
        in.signal = SIGNAL_TYPE_DISPLAY_PORT;
        in.pixelEncoding = PIXEL_ENCODING_RGB;
        in.odmCombineFactor = 4;
        const PixelRateDivider out = calculatePixRateDivider(in);
        assert(out.divFactor1 == PIXEL_RATE_DIV_BY_1);
        assert(out.divFactor2 == PIXEL_RATE_DIV_BY_4);
    }
    // 组合 4：DP + two_pix → (BY_1, BY_2)
    {
        K1K2Inputs in{};
        in.signal = SIGNAL_TYPE_DISPLAY_PORT;
        in.pixelEncoding = PIXEL_ENCODING_YCBCR420;
        in.twoPixPerContainer = true;
        const PixelRateDivider out = calculatePixRateDivider(in);
        assert(out.divFactor1 == PIXEL_RATE_DIV_BY_1);
        assert(out.divFactor2 == PIXEL_RATE_DIV_BY_2);
    }
    // 组合 5：HDMI TMDS + YCBCR420 → (BY_1, BY_2)
    {
        K1K2Inputs in{};
        in.signal = SIGNAL_TYPE_HDMI_TYPE_A;
        in.pixelEncoding = PIXEL_ENCODING_YCBCR420;
        const PixelRateDivider out = calculatePixRateDivider(in);
        assert(out.divFactor1 == PIXEL_RATE_DIV_BY_1);
        assert(out.divFactor2 == PIXEL_RATE_DIV_BY_2);
    }
    // 组合 6：未覆盖信号 → 两个 NA 一致落入输出（L373-374 初值语义）
    {
        K1K2Inputs in{};
        in.signal = SIGNAL_TYPE_LVDS;
        const PixelRateDivider out = calculatePixRateDivider(in);
        assert(out.divFactor1 == PIXEL_RATE_DIV_NA);
        assert(out.divFactor2 == PIXEL_RATE_DIV_NA);
    }
    std::puts("  [PASS] 7  打包层（L366-385）：k1/k2 组合装入 PixelRateDivider");
}

// ═══════════════════════════════════════════════════════════════════════════
// 8. 零副作用自证：constexpr 求值（无寄存器访问、无动态分配的编译期证明——
//    含任何非 constexpr 副作用的实现无法通过编译）。
// ═══════════════════════════════════════════════════════════════════════════

static void test_pure_functions_are_constexpr() {
    // DP + odm=2：分支 3 的 L355-356 路径
    constexpr K1K2Inputs dpOdm2{
        SIGNAL_TYPE_DISPLAY_PORT,             // signal
        PIXEL_ENCODING_RGB,                   // pixelEncoding
        false,                                // is128b132bSignal
        false,                                // twoPixPerContainer
        2,                                    // odmCombineFactor
    };
    constexpr K1K2Result r = calculateDccgK1K2Values(dpOdm2);
    static_assert(r.odmCombineFactor == 2, "返回 odm_combine_factor");
    static_assert(r.k1Div == PIXEL_RATE_DIV_BY_1, "DP odm=2 → k1=BY_1");
    static_assert(r.k2Div == PIXEL_RATE_DIV_BY_2, "DP odm=2 → k2=BY_2");
    static_assert(calculatePixRateDivider(dpOdm2).divFactor2 == PIXEL_RATE_DIV_BY_2,
                  "打包层同路径 constexpr 可求值");

    // HDMI FRL：分支 1 路径
    constexpr K1K2Inputs frl{
        SIGNAL_TYPE_HDMI_FRL,
        PIXEL_ENCODING_RGB,
        false,
        false,
        1,
    };
    static_assert(calculateDccgK1K2Values(frl).k2Div == PIXEL_RATE_DIV_BY_1,
                  "FRL → k2=BY_1");
    std::puts("  [PASS] 8  纯函数 constexpr 可求值（零副作用编译期证明）");
}

int main() {
    std::puts("第二步验收测试：DCN314 像素分频纯策略函数");
    std::puts("── 常量与 Linux 源一致 ──");
    test_signal_type_values_match_linux();
    test_pixel_rate_div_values_match_linux();
    test_pixel_encoding_values_match_linux();

    std::puts("── 信号分类 helper ──");
    test_signal_helpers();

    std::puts("── 分支逻辑（dcn314_hwseq.c L329-364）──");
    test_branch1_frl_and_128b132b();
    test_branch2_hdmi_tmds_dvi();
    test_branch3_dp_virtual();
    test_unhandled_signal_yields_na();
    test_return_value_is_odm_combine_factor();
    test_odm_combine_factor_helper();

    std::puts("── 打包层与纯度 ──");
    test_pix_rate_divider_wrapper();
    test_pure_functions_are_constexpr();

    std::puts("全部通过。用户态可编译（判据 1）由本次编译本身证明。");
    return 0;
}
