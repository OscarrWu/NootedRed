// DCN 314 Display implementation for Phoenix (Radeon 780M, RDNA3)
// Derivative of AMDRadeonX5000 and AMDRadeonX6000 decompilation
// Ported from Linux amdgpu dcn314 (register offsets verified identical to DCN2)
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.
//
// 移植说明（2026-08-27）：
// - dcn314 的 HUBP/OTG 寄存器偏移与 DCN2 100% 相同（已双 Verifier 审查确认，见 docs/project-knowledge.md）
// - DCN 基址 0x34C0 与 NootedRed 的 DCN_BASE_2 一致（Linux dmub_dcn314.c:35）
// - 本类继承 AMDGFX9DCNDisplay 基类，仅覆写 initDCNRegOffs，寄存器值从 DCN2.hpp 复用
// - 待办：clk_mgr / resource / hwseq 的 DCN 3.1.4 特有逻辑（P0 优先级，后续移植）

#pragma once
#include <AMDGFX9DCNDisplay.hpp>

// -----------------------------------------------------------------------------
// DCN 3.1.4 显示时钟请求 (VBIOSSMC 下发)：单位 kHz
//   移植自 Linux dcn314_smu_set_dispclk / dcn314_clk_mgr_helper 的时钟请求结构
//
// 注：像素分频的枚举与结构（`signal_type` / `dc_pixel_encoding` / `pixel_rate_div` /
//     `pixel_rate_divider` / `dcn314_k1k2_inputs`）原先在本文件重复定义了一份，
//     只服务于本文件里那两份无调用者的手写实现；第七步（集成）已一并删除，
//     正式版本在 `PixelDiv/PixelDiv.hpp`（TDD 落地，零寄存器依赖）。
// -----------------------------------------------------------------------------
struct dcn314_display_clock_req {
    UInt32 dispclk_khz;              // 目标显示时钟
    UInt32 dppclk_khz;               // 目标 DPP 时钟
    UInt32 hard_min_dcfclk_khz;      // pstate 启用时的 hard min DCF 时钟
    UInt32 min_deep_sleep_dcfclk_khz; // pstate 启用时的 deep sleep DCF 时钟
    bool   pstate_enabled;           // 是否启用 pstate (下发 hard min / deep sleep dcfclk)
};

class AMDRadeonX5000_AMDGFX9DCN314Display : public AMDRadeonX5000_AMDGFX9DCNDisplay
{
    static VFT vft;

    static void Constructor(AMDRadeonX5000_AMDGFX9DCN314Display* self, const OSMetaClass* metaClass);

    static void initDCNRegOffs(AMDRadeonX5000_AMDGFX9DCN314Display* self);

    // DCN 3.1.4 显示时钟下发（VBIOSSMC 完整主流程；路线图第五步）。
    //   序列由 `DisplaySeq/` 的生成器产出（内核态与用户态测试共用同一份），
    //   本函数只把调用方给的目标时钟转成生成器输入，再经 MP1 段邮箱落到硬件。
    //   req 字段：dispclk_khz / dppclk_khz / hard_min_dcfclk_khz / min_deep_sleep_dcfclk_khz。
    //   （req.pstate_enabled 当前未被使用：它只决定 Linux `construct` 是否读 DPM 表，
    //     而本驱动不下发 DPM 表 —— 见第五步执行记录中的缺口登记。）
    static void updateDisplayClocks(AMDRadeonX5000_AMDGFX9DCN314Display* self,
                                    const struct dcn314_display_clock_req* req);

    static AMDFlipOption getFlipOption(AMDRadeonX5000_AMDHWDisplay*);

    // ---- 调用点覆写 ----
    // init 覆写：走完基类 init（含 initDCNRegOffs）之后、首次 flip 之前，执行一次**完整的
    //   显示初始化编排**（时钟 → 像素率分频 → ODM → FIFO resync，见 applyInitialDisplaySequence）；
    //   每次显示初始化只执行一次（守卫: sDisplayInitSeqApplied）
    static bool init(AMDRadeonX5000_AMDHWDisplay* self, void* hwInterface, void* fbParams);

    // ---- 显示初始化编排（第七步·集成）----
    // 顺序依据 Linux 源码（逐条出处见 .cpp 内实现）：
    //   init_clocks（dcn31_hwseq.c:124）→ set_pixel_rate_div（dcn20_hwseq.c:846）
    //   → update_odm（dcn20_hwseq.c:1954）→ resync_fifo（dce110_hwseq.c:2737）。
    // 四个阶段各自独立、互不中断（与 Linux 一致：任一段失败不阻止后续段）。
    static void applyInitialDisplaySequence(AMDRadeonX5000_AMDGFX9DCN314Display* self);

    // 像素率分频（DCCG）：Linux `dccg314_set_pixel_rate_div`（dcn314_dccg.c:101-146）。
    //   K1/K2 由 `PixelDiv/` 的纯策略函数算出（复用正式实现，不写第二份）；
    //   写序列由 `DisplaySeq/Dcn314DccgSeq.hpp` 生成，两者都与离线影子运行同源。
    //   门②（"与当前值相同则跳过"）在此实现：先读回、比对、再决定是否下发。
    static void applyPixelRateDiv(AMDRadeonX5000_AMDHWRegisters& regs, UInt32 otgInst);

    // ---- update_odm / resync_fifo 寄存器级直译 (设计: updateodm-resync-directreg-design.md) ----
    // update_odm_direct: 展平 dcn314_update_odm -> set_odm_combine / set_odm_bypass + set_out_rate_control
    //   regs        : HW 寄存器访问器 (self->getHWRegisters())
    //   otg_inst    : 本 OTG/ODM 实例号
    //   opp_inst[]  : 参与拼接的 OPP 实例号数组 (长度 = opp_cnt)
    //   opp_cnt     : OPP 数量 (1 = bypass, 2/4 = combine)
    //   slice_width : 单段 (每 OPP) 像素宽
    static void update_odm_direct(AMDRadeonX5000_AMDHWRegisters& regs,
                                  UInt32                          otg_inst,
                                  const UInt32                    opp_inst[],
                                  UInt32                          opp_cnt,
                                  UInt32                          slice_width);

    // resync_fifo_dccg_dio_direct: 展平 dcn314_dccg.c trigger_dio_fifo_resync
    //   仅动 DENTIST_DISPCLK_CNTL: 读 RDIVIDER -> 写 WDIVIDER
    static void resync_fifo_dccg_dio_direct(AMDRadeonX5000_AMDHWRegisters& regs);

public:
    PWDeclareRuntimeMC(AMDRadeonX5000_AMDGFX9DCN314Display, Constructor)

    static void resolve(const char* kext);
};
