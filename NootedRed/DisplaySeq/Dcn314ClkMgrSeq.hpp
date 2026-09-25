// DCN 3.1.4 显示时钟主流程翻译
//
// 忠实翻译自 Linux：
//   dcn314_clk_mgr.c  — dcn314_update_clocks(L208-338)、dcn314_init_clocks(L187-206)、
//                        dcn314_is_spll_ssc_enabled(L177-185)、dcn314_read_ss_info_from_lut(L779-795)
//   clk_mgr_internal.h — should_set_clock(L561)
//   dce_clk_mgr.c      — dce_adjust_dp_ref_freq_for_ss(L100-113)
//
// 本生成器**只产出 RegOp 序列**，不读写任何硬件。
// 真机侧由 sink 消费（RegSink*.hpp），用户态侧由影子运行器消费。
//
// ⛔ 不含内核头文件、无动态分配、无异常、无浮点。
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.

#pragma once

#include "RegOp.hpp"
#include "VbiosSmcSeq.hpp"   // display::vbios_smc::generateSetDispclk, generateSetDppclk, etc.

#include <cstdint>

namespace display {
namespace dcn314_clk {

// ── 枚举（Linux dc.h L730-743）───────────────────────────────────────────────
//
// enum dcn_pwr_state (dc.h L730-734)：**照 Linux 原值**（UNKNOWN = -1，故用有符号类型）。
// 刻意不重编号——重编号会让"与 Linux 逐行对照"变成需要心算的事，是将来出错的来源。
enum PwrState : std::int32_t {
    PWR_UNKNOWN      = -1,  // DCN_PWR_STATE_UNKNOWN      = -1
    PWR_MISSION_MODE = 0,   // DCN_PWR_STATE_MISSION_MODE = 0
    PWR_LOW_POWER    = 3    // DCN_PWR_STATE_LOW_POWER    = 3
};

// zstate 支持等级的**值域唯一来源**是 `display::vbios_smc::ZStateSupport`
// （同为 Linux dc.h:736-743 的枚举：UNKNOWN=0 / ALLOW=1 / ALLOW_Z8_ONLY=2 /
//   ALLOW_Z8_Z10_ONLY=3 / ALLOW_Z10_ONLY=4 / DISALLOW=5）。
// 这里只做别名，避免两套常量各自漂移；下方 static_assert 是防漂移的机器检查。
using ZStateSupport = vbios_smc::ZStateSupport;
constexpr std::uint32_t ZSTATE_UNKNOWN  = vbios_smc::ZSTATE_UNKNOWN;   // = 0
constexpr std::uint32_t ZSTATE_ALLOWED  = vbios_smc::ZSTATE_ALLOW;     // = 1
constexpr std::uint32_t ZSTATE_DISALLOW = vbios_smc::ZSTATE_DISALLOW;  // = 5

static_assert(ZSTATE_UNKNOWN == 0 && ZSTATE_ALLOWED == 1 && ZSTATE_DISALLOW == 5,
              "zstate 取值必须与 Linux dc.h:736-743 一致");
static_assert(PWR_UNKNOWN == -1 && PWR_MISSION_MODE == 0 && PWR_LOW_POWER == 3,
              "pwr_state 取值必须与 Linux dc.h:730-734 一致");

// ── 结构体 ─────────────────────────────────────────────────────────────────────

// Linux struct dc_clocks 的子集 —— 目标时钟（由调用方提供）
struct TargetClocks {
    std::uint32_t dcfclkKhz;
    std::uint32_t dcfclkDeepSleepKhz;
    std::uint32_t dppclkKhz;
    std::uint32_t dispclkKhz;
    std::uint32_t zstateSupport;   // ZStateSupport
    bool          dtbclkEn;
};

// Linux clk_mgr->clks 的子集——"上一次已下发的状态"
struct ClkMgrState {
    std::uint32_t dcfclkKhz;
    std::uint32_t dcfclkDeepSleepKhz;
    std::uint32_t dppclkKhz;
    std::uint32_t dispclkKhz;
    std::uint32_t zstateSupport;
    std::int32_t  pwrState;        // PwrState（Linux 的 UNKNOWN = -1，故有符号）
    bool          dtbclkEn;
};

// 主流程用到的常量/外部输入
struct ClkMgrConsts {
    std::uint32_t minDispClkKhz;      // Linux dc->debug.min_disp_clk_khz（0 = 不 clamp）
    std::uint32_t activeDisplayCount; // Linux dcn314_get_active_display_cnt_wa() 的结果
};

struct Mailbox { std::uint32_t msg67, param83, status91; };

// ── shouldSetClock 纯函数（Linux clk_mgr_internal.h L561-564）─────────────────
//
// Linux 原文：
//   static inline bool should_set_clock(bool safe_to_lower, int calc_clk, int cur_clk) {
//       return ((safe_to_lower && calc_clk < cur_clk) || calc_clk > cur_clk);
//   }
constexpr bool shouldSetClock(bool safeToLower, std::uint32_t calcClk, std::uint32_t curClk) {
    return (safeToLower && calcClk < curClk) || calcClk > curClk;
}

// ── generateReadSpllSscEnabled ────────────────────────────────────────────────
//
// Linux dcn314_is_spll_ssc_enabled (dcn314_clk_mgr.c L177-185)：
//   REG_GET(CLK6_0_CLK6_spll_field_8, spll_ssc_en, &ssc_enable);
//   其中 CLK6_0_CLK6_spll_field_8 = 0x464b (L98)，BASE_IDX=0
//   spll_ssc_en 位域：shift=0xd, mask=0x00002000 (L101-102)
//
// 本函数只生成读 op。位域提取由调用方从 sink.lastValue() 做。
inline void generateReadSpllSscEnabled(RegSeq& out, RegAddr clk6SpllField8) {
    // Linux: REG_GET(CLK6_0_CLK6_spll_field_8, spll_ssc_en, &ssc_enable);
    out.push(regRead(clk6SpllField8, "read_spll_ssc_en"));
}

// ── generateReadSsInfoClockSource ─────────────────────────────────────────────
//
// Linux dcn314_read_ss_info_from_lut (dcn314_clk_mgr.c L779-795) 的第一步：
//   REG_GET(CLK1_CLK2_BYPASS_CNTL, CLK2_BYPASS_SEL, &clock_source);
//   其中 CLK1_CLK2_BYPASS_CNTL = 0x029c (L90)，BASE_IDX=0
//   CLK2_BYPASS_SEL 位域：shift=0x0, mask=0x00000007 (L93)
//
// 调用方从 lastValue() 取 clockSource，再配合 ssPercentageFor 做判断。
inline void generateReadSsInfoClockSource(RegSeq& out, RegAddr clk1Clk2BypassCntl) {
    // Linux: REG_GET(CLK1_CLK2_BYPASS_CNTL, CLK2_BYPASS_SEL, &clock_source);
    out.push(regRead(clk1Clk2BypassCntl, "read_clock_source"));
}

// ── ssPercentageFor 查表 ─────────────────────────────────────────────────────
//
// Linux ss_info_table (dcn314_clk_mgr.c L492-495)：
//   static struct dcn314_ss_info_table ss_info_table = {
//       .ss_divider = 1000,
//       .ss_percentage = {0, 0, 375, 375, 375}   // 索引 0..4
//   };
constexpr std::uint32_t kSsDivider = 1000;   // Linux dcn314_clk_mgr.c L493

inline std::uint32_t ssPercentageFor(std::uint32_t clockSource) {
    // Linux dcn314_clk_mgr.c L494: ss_percentage = {0, 0, 375, 375, 375}
    // 索引对应 CLK2_BYPASS_SEL 值（0..4），超出返回 0
    static constexpr std::uint32_t kTable[] = {0, 0, 375, 375, 375};
    if (clockSource >= 5)
        return 0;
    return kTable[clockSource];
}

// ── adjustDpRefFreqForSs 纯函数（整数运算版）─────────────────────────────────
//
// Linux dce_adjust_dp_ref_freq_for_ss (dce100/dce_clk_mgr.c L100-113)：
//   用 fixed31_32 定点算术：
//     ss_percentage = (ss_percentage / ss_divider) / 200
//     adjusted = floor(dp_ref_clk_khz * (1 - ss_percentage))
//
// 本项目无浮点定点库 → 用 64 位整数精确复现同一计算。
inline std::uint32_t adjustDpRefFreqForSs(std::uint32_t dpRefClkKhz,
                                          std::uint32_t ssPercentage,
                                          std::uint32_t ssDivider) {
    if (ssPercentage == 0 || ssDivider == 0)
        return dpRefClkKhz;

    // 复现 Linux fixed31_32 三步计算（dce_clk_mgr.c L103-110）：
    //   1. ss_percentage = dc_fixpt_div_int(dc_fixpt_from_fraction(ss_pct, div), 200)
    //      = (ss_pct << 32) / (div * 200)
    //   2. ss_percentage = dc_fixpt_sub(dc_fixpt_one, ss_percentage)
    //      = (1 << 32) - ss_percentage
    //   3. adj = dc_fixpt_mul_int(ss_percentage, dp_ref_clk_khz)
    //      = ss_percentage * dp_ref_clk_khz
    //   4. result = dc_fixpt_floor(adj)  →  adj >> 32
    std::int64_t ssPctFixed = (static_cast<std::int64_t>(ssPercentage) << 32)
                            / (static_cast<std::int64_t>(ssDivider) * 200);
    std::int64_t oneMinus = (static_cast<std::int64_t>(1) << 32) - ssPctFixed;
    std::int64_t adj = oneMinus * static_cast<std::int64_t>(dpRefClkKhz);
    return static_cast<std::uint32_t>(adj >> 32);
}

// ── initClocksState 纯函数 ────────────────────────────────────────────────────
//
// Linux dcn314_init_clocks (dcn314_clk_mgr.c L187-206) 的实质：
//   1. memset clks 清零 → 返回零初始化的 ClkMgrState
//   2. ref_dtbclk_khz 保持不变（作为参数传入，未反映在 ClkMgrState 中）
//   3. p_state_change_support / prev_p_state_change_support = true（本项目不跟踪）
//   4. pwr_state = DCN_PWR_STATE_UNKNOWN
//   5. zstate_support = DCN_ZSTATE_SUPPORT_UNKNOWN
//   6. 若 spll_ssc_enabled → dp_dto_source = adjustDpRefFreqForSs(dprefclk)
//      否则 dp_dto_source = dprefclk
inline ClkMgrState initClocksState(std::uint32_t /*refDtbclkKhz*/,
                                   bool spllSscEnabled,
                                   std::uint32_t dprefclkKhz,
                                   std::uint32_t ssPercentage,
                                   std::uint32_t ssDivider,
                                   std::uint32_t* dpDtoSourceClockKhz) {
    // memset -> all zeros (L192)
    ClkMgrState s = {};
    // pwr_state = DCN_PWR_STATE_UNKNOWN (L197)
    s.pwrState = PWR_UNKNOWN;
    // zstate_support = DCN_ZSTATE_SUPPORT_UNKNOWN (L198)
    s.zstateSupport = ZSTATE_UNKNOWN;

    // dp_dto_source (L200-205)
    if (dpDtoSourceClockKhz) {
        if (spllSscEnabled)
            *dpDtoSourceClockKhz = adjustDpRefFreqForSs(dprefclkKhz, ssPercentage, ssDivider);
        else
            *dpDtoSourceClockKhz = dprefclkKhz;
    }

    return s;
}

// ── generateUpdateClocks 主流程 ──────────────────────────────────────────────
//
// 严格按 Linux dcn314_update_clocks (dcn314_clk_mgr.c L208-338) 的顺序与条件翻译。
//
// 返回值：是否产生了任何寄存器操作。
// 新状态写入 *next（next 可为 nullptr）。
inline bool generateUpdateClocks(RegSeq& out, const Mailbox& mb, const ClkMgrState& cur,
                                 const TargetClocks& tgt, const ClkMgrConsts& cst,
                                 bool safeToLower, ClkMgrState* next) {
    bool anyOp = false;
    ClkMgrState tmp = cur;  // 若无 next，暂存本地

    // Linux L221: if (dc->work_arounds.skip_clock_update) return;
    // → 本项目由调用方判断，不在此处处理。

    // display_count = dcn314_get_active_display_cnt_wa(dc, context) — 由 cst.activeDisplayCount 传入

    // ──────────────────────────────────────────────────────────────────────
    // safe_to_lower 分支（Linux L229-252）
    // ──────────────────────────────────────────────────────────────────────
    if (safeToLower) {
        // zstate (L230-234)
        if (tgt.zstateSupport != ZSTATE_DISALLOW &&
            tgt.zstateSupport != cur.zstateSupport) {
            // Linux: dcn314_smu_set_zstate_support(clk_mgr, new_clocks->zstate_support);
            vbios_smc::generateSetZstateSupport(out, mb.msg67, mb.param83, mb.status91, tgt.zstateSupport);
            tmp.zstateSupport = tgt.zstateSupport;
            anyOp = true;
        }

        // dtbclk (L236-239)
        if (cur.dtbclkEn && !tgt.dtbclkEn) {
            // Linux: dcn314_smu_set_dtbclk(clk_mgr, false);
            vbios_smc::generateSetDtbClk(out, mb.msg67, mb.param83, mb.status91, false);
            tmp.dtbclkEn = tgt.dtbclkEn;
            anyOp = true;
        }

        // pwr_state / idle optimization (L241-251)
        if (cur.pwrState != PWR_LOW_POWER) {
            if (cst.activeDisplayCount == 0) {
                // Linux L244-247: union display_idle_optimization_u idle_info = {0};
                //   idle_info.idle_info.df_request_disabled = 1;  // bit 0
                //   idle_info.idle_info.phy_ref_clk_off = 1;     // bit 1
                //   idle_info.idle_info.s0i2_rdy = 1;            // bit 2
                // 三个位域的定义见 dcn314_smu.h L81-86
                std::uint32_t idleInfo = 0;
                idleInfo |= (1u << 0); // df_request_disabled
                idleInfo |= (1u << 1); // phy_ref_clk_off
                idleInfo |= (1u << 2); // s0i2_rdy
                // Linux: dcn314_smu_set_display_idle_optimization(clk_mgr, idle_info.data);
                vbios_smc::generateSetDisplayIdleOptimizations(out, mb.msg67, mb.param83, mb.status91, idleInfo);
                tmp.pwrState = PWR_LOW_POWER;
                anyOp = true;
            }
        }
    }
    // ──────────────────────────────────────────────────────────────────────
    // !safe_to_lower 分支（Linux L253-273）
    // ──────────────────────────────────────────────────────────────────────
    else {
        // zstate (L254-258): 注意 Linux 传的是 DCN_ZSTATE_SUPPORT_DISALLOW 常量，非 new_clocks->值
        if (tgt.zstateSupport == ZSTATE_DISALLOW &&
            tgt.zstateSupport != cur.zstateSupport) {
            // Linux: dcn314_smu_set_zstate_support(clk_mgr, DCN_ZSTATE_SUPPORT_DISALLOW);
            vbios_smc::generateSetZstateSupport(out, mb.msg67, mb.param83, mb.status91, ZSTATE_DISALLOW);
            tmp.zstateSupport = tgt.zstateSupport;
            anyOp = true;
        }

        // dtbclk (L260-263)
        if (!cur.dtbclkEn && tgt.dtbclkEn) {
            // Linux: dcn314_smu_set_dtbclk(clk_mgr, true);
            vbios_smc::generateSetDtbClk(out, mb.msg67, mb.param83, mb.status91, true);
            tmp.dtbclkEn = tgt.dtbclkEn;
            anyOp = true;
        }

        // pwr_state / idle optimization (L266-271)
        if (cur.pwrState != PWR_MISSION_MODE) {
            // Linux L267: union display_idle_optimization_u idle_info = {0}; → data = 0
            vbios_smc::generateSetDisplayIdleOptimizations(out, mb.msg67, mb.param83, mb.status91, 0);
            tmp.pwrState = PWR_MISSION_MODE;
            anyOp = true;
        }
    }

    // ──────────────────────────────────────────────────────────────────────
    // dcfclk (Linux L275-278)
    // ──────────────────────────────────────────────────────────────────────
    if (shouldSetClock(safeToLower, tgt.dcfclkKhz, cur.dcfclkKhz)) {
        tmp.dcfclkKhz = tgt.dcfclkKhz;
        vbios_smc::generateSetHardMinDcfclk(out, mb.msg67, mb.param83, mb.status91, tmp.dcfclkKhz);
        anyOp = true;
    }

    // ──────────────────────────────────────────────────────────────────────
    // deep sleep dcfclk (Linux L280-284)
    // ──────────────────────────────────────────────────────────────────────
    if (shouldSetClock(safeToLower, tgt.dcfclkDeepSleepKhz, cur.dcfclkDeepSleepKhz)) {
        tmp.dcfclkDeepSleepKhz = tgt.dcfclkDeepSleepKhz;
        vbios_smc::generateSetMinDeepSleepDcfclk(out, mb.msg67, mb.param83, mb.status91, tmp.dcfclkDeepSleepKhz);
        anyOp = true;
    }

    // ──────────────────────────────────────────────────────────────────────
    // dppclk 下限钳位（Linux L286-288）
    //   workaround: Limit dppclk to 100Mhz to avoid lower eDP panel switch
    //   to plus 4K monitor underflow.
    //   注意 Linux **就地改** new_clocks，此处用本地变量复现相同语义。
    // ──────────────────────────────────────────────────────────────────────
    std::uint32_t clampedDppclkKhz = tgt.dppclkKhz;
    if (clampedDppclkKhz < 100000)
        clampedDppclkKhz = 100000;

    // ──────────────────────────────────────────────────────────────────────
    // dppclk 比较（Linux L290-295）：判断是否降低 + 是否要更新
    // ──────────────────────────────────────────────────────────────────────
    bool dppClockLowered = false;
    bool updateDppclk = false;

    if (shouldSetClock(safeToLower, clampedDppclkKhz, cur.dppclkKhz)) {
        if (cur.dppclkKhz > clampedDppclkKhz)
            dppClockLowered = true;
        // Linux L293: clk_mgr_base->clks.dppclk_khz = new_clocks->dppclk_khz (钳位后值)
        tmp.dppclkKhz = clampedDppclkKhz;
        updateDppclk = true;
    }

    // ──────────────────────────────────────────────────────────────────────
    // dispclk（Linux L297-312）
    // ──────────────────────────────────────────────────────────────────────
    bool updateDispclk = false;

    if (shouldSetClock(safeToLower, tgt.dispclkKhz, cur.dispclkKhz) &&
        (tgt.dispclkKhz > 0 || (safeToLower && cst.activeDisplayCount == 0))) {
        std::uint32_t requestedDispclkKhz = tgt.dispclkKhz;

        // ────────── Linux L301: dcn314_disable_otg_wa(true) ──────────
        // Linux 在下发 dispclk 前调 dcn314_disable_otg_wa(true)，事后调 (false)。
        // 该函数按 stream 逐个 disable OTG，需要 pipe / stream 对象模型
        // （dcn314_clk_mgr.c L151-175）。
        // ⛔ 本项目无 pipe/stream 模型 → 跳过。
        // 已知影响范围：
        //   - dispclk 更改时 OTG 未关可能导致短暂花屏或 timing 扰动
        //   - 当前阶段（单屏点亮）影响很小，待真机验证
        // ────────── 跳过: dcn314_disable_otg_wa(clk_mgr_base, context, safe_to_lower, true); ──────────

        // clamp (Linux L303-305)
        if (cst.minDispClkKhz > 0 && requestedDispclkKhz < cst.minDispClkKhz)
            requestedDispclkKhz = cst.minDispClkKhz;

        // Linux L307: dcn314_smu_set_dispclk(clk_mgr, requested_dispclk_khz);
        vbios_smc::generateSetDispclk(out, mb.msg67, mb.param83, mb.status91, requestedDispclkKhz);
        // Linux L308: clk_mgr_base->clks.dispclk_khz = new_clocks->dispclk_khz（原始请求值，非钳位值）
        tmp.dispclkKhz = tgt.dispclkKhz;

        // ────────── Linux L309: dcn314_disable_otg_wa(false) ──────────
        // ⛔ 跳过（同上）。
        // ────────── 跳过: dcn314_disable_otg_wa(clk_mgr_base, context, safe_to_lower, false); ──────────

        updateDispclk = true;
        anyOp = true;
    }

    // ──────────────────────────────────────────────────────────────────────
    // dppclk 下发顺序（Linux L314-325）—— 顺序是语义的一部分
    // ──────────────────────────────────────────────────────────────────────
    if (dppClockLowered) {
        // Linux L316: dcn20_update_clocks_update_dpp_dto(clk_mgr, context, safe_to_lower);
        //   ⛔ 本驱动无 pipe 上下文 → 跳过。
        //   该函数在 DPP 时钟降低前增大每管道的 DTO，防止 underflow。
        //   影响范围：dppclk 降低时若缺少 DTO 调整，可能短暂 underflow 导致花屏。
        // ────────── 跳过: dcn20_update_clocks_update_dpp_dto(clk_mgr, context, safe_to_lower); ──────────

        // Linux L317: dcn314_smu_set_dppclk(clk_mgr, clk_mgr_base->clks.dppclk_khz);
        vbios_smc::generateSetDppclk(out, mb.msg67, mb.param83, mb.status91, tmp.dppclkKhz);
        anyOp = true;
    } else {
        if (updateDppclk || updateDispclk) {
            // Linux L321: dcn314_smu_set_dppclk(clk_mgr, clk_mgr_base->clks.dppclk_khz);
            vbios_smc::generateSetDppclk(out, mb.msg67, mb.param83, mb.status91, tmp.dppclkKhz);
            anyOp = true;
        }

        // Linux L323-324: if (new_clocks->dppclk_khz >= dc->current_state->bw_ctx.bw.dcn.clk.dppclk_khz)
        //                    dcn20_update_clocks_update_dpp_dto(...)
        //   ⛔ 本驱动无 pipe 上下文 → 跳过。
        //   Linux 以此保证"上升前 DTO 已处理"。跳过后，dispclk/dppclk 上调时
        //   可能短暂 underflow；当前单屏场景影响小。
        // ────────── 跳过: dcn20_update_clocks_update_dpp_dto(clk_mgr, context, safe_to_lower); ──────────
    }

    // ──────────────────────────────────────────────────────────────────────
    // DMCUB 时钟通知（Linux L327-337）── 不翻译
    // ──────────────────────────────────────────────────────────────────────
    // Linux 在最后向 DMCUB 发 DMUB_CMD__CLK_MGR_NOTIFY_CLOCKS 命令，
    // 通知固件最新的时钟值（dcfclk, deep_sleep, dispclk, dppclk）。
    //
    // ⛔ 本项目已定论：苹果的 DMCUB 通道对 780M 不可用（见 docs/DMCUB决策备忘录.md）。
    //    该命令永远不可能被苹果驱动理解或执行，即使发出也只会被 DMCUB 忽略或报错。
    //    根据路线图 §5.4 简化项 2，不发电钟变通通知，写空壳跳过即可。
    //    DMCUB 时钟通知缺失的影响范围：
    //      - 背光/省电/时钟通知需要时可能受限
    //      - 当前单屏点亮阶段不受影响
    // ────────── 跳过: dc_wake_and_execute_dmub_cmd(dc->ctx, &cmd, DM_DMUB_WAIT_TYPE_WAIT); ──────────

    // ── 写回 next 状态 ───────────────────────────────────────────────────
    if (next)
        *next = tmp;

    return anyOp;
}

}  // namespace dcn314_clk
}  // namespace display
