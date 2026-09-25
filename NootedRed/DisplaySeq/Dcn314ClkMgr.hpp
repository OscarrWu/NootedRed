// DCN 3.1.4 显示时钟管理器 —— 管理"已下发状态"并把生成器产出交给 sink 执行
//
// 定位（依据 docs/ROADMAP.md §2.6 第五步）：
//   · 上一层：`Dcn314ClkMgrSeq.hpp`（纯逻辑，按 Linux 的顺序与条件产出 RegOp 序列）
//   · 下一层：`RegSinkInjected.hpp`（把 RegOp 落到真实寄存器通道）
//   · 本文件：把两层接起来，并持有 Linux `clk_mgr->clks` 的等价状态（"上次已下发值"）
//
// ⛔ 本文件不含内核头文件 → 内核态（kext）与用户态（影子运行/单元测试）共用同一份代码。
//    真机侧只负责构造一个带 MP1 通道的 InjectedRegSink（见 AMDGFX9DCN314Display.cpp）。
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once

#include "Dcn314ClkMgrSeq.hpp"
#include "RegSinkInjected.hpp"

namespace display {
namespace dcn314_clk {

// VBIOSSMC 邮箱的段内偏移（MP1 SEG0）。
// 为什么不 include `Regs/SMU.hpp`：那一层含 IOKit 头，而本目录必须能在分析机用户态编译。
// 一致性由测试保证（tests 里断言这三个值与 Linux 头文件一致，防漂移）。
constexpr std::uint32_t kMailboxMsg67    = 0x283;  // MP1_SMN_C2PMSG_67（触发）
constexpr std::uint32_t kMailboxParam83  = 0x293;  // MP1_SMN_C2PMSG_83（参数/读回）
constexpr std::uint32_t kMailboxStatus91 = 0x29B;  // MP1_SMN_C2PMSG_91（状态）

// 保守默认时钟档案（路线图 §5.4 简化项 1：本项目没有 Linux 的带宽/时序计算层，
// 点亮阶段用固定保守值代替真实计算）。
//
// 取值依据（**不是猜的**）：2026-09-25 采集的 Linux 真机真值序列
// `kb/sequences/linux-dcn314-init.json` 里，Linux 在**同一台机器的同一块屏**上
// 实际下发的值——
//   msg 0x4 SetDispclkFreq            = 534 MHz
//   msg 0x6 SetDppclkFreq             = 519 MHz
//   msg 0x7 SetHardMinDcfclkByFreq    = 200 MHz
//   msg 0x8 SetMinDeepSleepDcfclk     =  38 MHz
struct DefaultClockProfile {
    std::uint32_t dispclkKhz{534000};
    std::uint32_t dppclkKhz{519000};
    std::uint32_t hardMinDcfclkKhz{200000};
    std::uint32_t minDeepSleepDcfclkKhz{38000};

    // 目标 zstate 支持等级：取 ALLOW_Z8_Z10_ONLY —— **与真机真值一致**：
    //   真值里 Linux 发的 0x15 事务 param = 0x500，正是该等级对应的编码
    //   （Linux dcn314_smu.c:364-367 的 case DCN_ZSTATE_SUPPORT_ALLOW_Z8_Z10_ONLY）。
    // 注意：本驱动只下发消息、不参与 zstate 的进入/退出判定，故该值只影响"发哪条消息、带什么 param"。
    // 取值域见 `display::vbios_smc::ZStateSupport`（与本结构体字段同为 Linux dc.h:736-743 的枚举）。
    std::uint32_t zstateSupport{vbios_smc::ZSTATE_ALLOW_Z8_Z10_ONLY};
    bool          dtbclkEn{false};   // 真值里没有 0x17 SetDtbClk ⇒ Linux 那次未请求开启

    TargetClocks toTarget() const {
        TargetClocks t{};
        t.dcfclkKhz               = hardMinDcfclkKhz;
        t.dcfclkDeepSleepKhz      = minDeepSleepDcfclkKhz;
        t.dppclkKhz               = dppclkKhz;
        t.dispclkKhz              = dispclkKhz;
        t.zstateSupport           = zstateSupport;
        t.dtbclkEn                = dtbclkEn;
        return t;
    }
};

// 时钟管理器：状态机 + 序列生成 + 执行，三者合一。
//
// 与 Linux 的对应关系：
//   `clk_mgr->clks`（已下发状态）        ↔ `state_`
//   `dcn314_update_clocks(clk_mgr, ctx, safe_to_lower)` ↔ `apply()`
//   `dcn314_init_clocks` / 首次上电下发   ↔ `applyInitial()`
class ClkMgr {
public:
    // 单次下发序列的固定容量。实测最长序列（两笔消息事务 + 探询）约 20 个 op，
    // 512 留足余量；溢出会被 `overflowed()` 检出（绝不静默截断后照常执行）。
    static constexpr std::size_t kSeqCapacity = 512;

    void setProfile(const DefaultClockProfile& p) { profile_ = p; }
    const DefaultClockProfile& profile() const { return profile_; }

    // 状态初值必须对齐 Linux `dnc314_init_clocks`（dcn314_clk_mgr.c:187-206）：
    // 时钟清零、pwr_state = UNKNOWN(-1)、zstate_support = UNKNOWN(0)。
    void resetState() { state_ = initialStateFor(); }

    const ClkMgrState& state() const { return state_; }
    std::uint32_t      smuVersion() const { return smuVersion_; }
    bool               smuPresent() const { return smuPresent_; }
    std::size_t        lastOpCount() const { return lastOpCount_; }

    // 上电/初始化阶段的一次性下发：
    //   ① SMU 版本探测（Linux `dcn314_clk_mgr_construct` 的 `dcn314_smu_get_smu_version`）
    //   ② 首次时钟下发（`safe_to_lower = false`：进入 mission mode 路径）
    //   ③ 允许 zstate（`safe_to_lower = true`：Linux 稳定后允许进入低功耗状态）
    // 返回 false 表示"没有真正落到硬件"（通道不可用 / 轮询超时 / 缓冲区溢出）。
    bool applyInitial(RegSink& sink) {
        if (!probeSmuVersion(sink)) { return false; }
        if (!smuPresent_) { return false; }   // Linux: smu_present 为假时不走时钟下发路径
        if (!apply(profile_.toTarget(), false, sink)) { return false; }
        return apply(profile_.toTarget(), true, sink);
    }

    // SMU 版本探测：产出一笔 get-version 事务并执行，结果记在 smuVersion_。
    bool probeSmuVersion(RegSink& sink) {
        RegSeq seq(buf_, kSeqCapacity);
        vbios_smc::generateGetSmuVersion(seq, kMailboxMsg67, kMailboxParam83, kMailboxStatus91);
        lastOpCount_ = seq.size();
        if (seq.overflowed()) { return false; }
        if (sink.executeAll(seq) != seq.size()) { return false; }
        smuVersion_ = sink.lastValue();
        smuPresent_ = (smuVersion_ != 0);
        return true;
    }

    // 一次时钟更新（对应 Linux `dcn314_update_clocks`）。
    // 执行成功才提交新状态——状态必须始终等于"已真正下发的值"。
    bool apply(const TargetClocks& tgt, bool safeToLower, RegSink& sink) {
        RegSeq seq(buf_, kSeqCapacity);

        const Mailbox mb{kMailboxMsg67, kMailboxParam83, kMailboxStatus91};

        ClkMgrConsts cst{};
        cst.minDispClkKhz      = 0;   // Linux `dc->debug.min_disp_clk_khz` 默认 0（不钳位）
        cst.activeDisplayCount = 1;   // 本驱动当前只处理内置单屏（路线图 §5.4 简化项 7）

        ClkMgrState next = state_;
        const bool  wrote = generateUpdateClocks(seq, mb, state_, tgt, cst, safeToLower, &next);
        lastOpCount_ = seq.size();

        if (seq.overflowed()) { return false; }
        if (wrote) {
            if (sink.executeAll(seq) != seq.size()) { return false; }
            state_ = next;
        }
        return true;
    }

private:
    ClkMgrState        state_ = initialStateFor();
    DefaultClockProfile profile_{};
    std::uint32_t      smuVersion_{0};
    bool               smuPresent_{false};
    std::size_t        lastOpCount_{0};

    // 不用 `ClkMgrState{}` 做初值：那是 pwr_state = 0 = MISSION_MODE，
    // 会让"首次从 UNKNOWN 进入 mission mode"的下发被判为"已在 mission mode"而跳过，
    // 与真机行为不符（真值里首笔即 0x12 SetDisplayIdleOptimizations）。
    static ClkMgrState initialStateFor() {
        std::uint32_t dpDtoSourceClkHz = 0;
        return initClocksState(0, false, 0, 0, 0, &dpDtoSourceClkHz);
    }
    // 固定缓冲区：内核态不做可失败分配（路线图 §3.2）。与 sink 的执行期不重叠。
    RegOp buf_[kSeqCapacity]{};
};

}  // namespace dcn314_clk
}  // namespace display
