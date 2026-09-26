// DCN 314 Display implementation for Phoenix (Radeon 780M, RDNA3)
// Derivative of AMDRadeonX5000 and AMDRadeonX6000 decompilation
// Ported from Linux amdgpu dcn314 (register offsets verified identical to DCN2)
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#include <AMDGFX9DCN314Display.hpp>
#include <AMDGFX9DCNDisplay.hpp>
#include <HWLibs.hpp>
#include <GPUDriversAMD/Accel/HWDisplay.hpp>
#include <GPUDriversAMD/RavenIPOffset.hpp>
#include <Headers/kern_util.hpp>
#include <StageMark.hpp>
#include <PenguinWizardry/RuntimeMC.hpp>
#include <PenguinWizardry/RuntimeVFT.hpp>
#include <Regs/DCN314.hpp>
#include <DisplaySeq/Dcn314ClkMgr.hpp>
#include <DisplaySeq/Dcn314DccgSeq.hpp>
#include <PixelDiv/PixelDiv.hpp>
#include <GPUDriversAMD/CAIL/HWBlock.hpp>
#include <IOKit/IOLib.h>
#include <DisplaySeq/Dcn314OdmSeq.hpp>
#include <libkern/OSTypes.h>
#include <libkern/c++/OSMetaClass.h>

// ⚠️ 此处原先有两份"像素分频纯策略函数"的手写实现（`dcn314_calc_k1_k2_values` /
//    `dcn314_calc_pix_rate_divider`）与一组 `dc_is_*_signal` helper，均**无调用者**。
//    第七步（集成）把它们删除，改为复用 `PixelDiv/PixelDiv.hpp` 的正式实现 ——
//    依据路线图 §3.2：「三者必须共用同一份生成器，**不能有第二份手写实现**」。
//    （PixelDiv/ 是 TDD 落地的版本，含 15 组断言；本文件那份既重复又无人调用。）

// -----------------------------------------------------------------------------
// 调用点（第七步·集成后只有**一个**入口）
//   init() : 基类 init（含 initDCNRegOffs）完成后、首次 flip 前 →
//            一次完整的显示初始化编排（时钟 → 像素率分频 → ODM → FIFO resync），
//            见 applyInitialDisplaySequence()。
//   守卫: 文件级静态标志 sDisplayInitSeqApplied，保证每次驱动生命周期只执行一次，
//         绝不进入 per-frame flip 路径。
//   （第七步移除了原先挂在 `setCurrentDisplayOffset` 上的 resync 覆写：该 vft 槽在
//     目标系统 Ventura 13.6 上不安装，是死代码；resync 已并入上面的编排。）
// -----------------------------------------------------------------------------
static bool (*superInit)(AMDRadeonX5000_AMDHWDisplay*, void*, void*)                   = nullptr;
static bool sDisplayInitSeqApplied = false;

// 前置声明：编排入口（applyPixelRateDiv / applyInitialDisplaySequence）用到 DCN 段的
// 寄存器通道构造函数，而它的定义在本文件后部的「update_odm / resync_fifo 寄存器级直译」一节。
static void makeDcnChannel(AMDRadeonX5000_AMDHWRegisters& regs, display::RegChannel* const out);

PWDefineRuntimeMC(AMDRadeonX5000_AMDGFX9DCN314Display, Constructor)

AMDRadeonX5000_AMDGFX9DCNDisplay::VFT AMDRadeonX5000_AMDGFX9DCN314Display::vft;

void AMDRadeonX5000_AMDGFX9DCN314Display::Constructor(AMDRadeonX5000_AMDGFX9DCN314Display* const self,
                                                      const OSMetaClass* const                 metaClass)
{
    assert(AMDRadeonX5000_AMDHWDisplay::constructor() != 0);
    FunctionCast(Constructor, AMDRadeonX5000_AMDHWDisplay::constructor())(self, metaClass);
    vft.replaceVFT(self);
    gRTMetaClass.instanceConstructed();
}

void AMDRadeonX5000_AMDGFX9DCN314Display::resolve(const char* const kext)
{
    AMDRadeonX5000_AMDGFX9DCNDisplay::populateVFT(vft);
    PWPopulateRuntimeMCGetMetaClassVFTEntry();
    vft.getExpanded<decltype(initDCNRegOffs)>(0) = initDCNRegOffs;
    vft.getExpanded<decltype(updateDisplayClocks)>(1) = updateDisplayClocks;

    // 覆写 getFlipOption：dcn314 返回 DCN3（基类硬编码 DCN2）
    constants.vftGetFlipOption(vft.inner()) = getFlipOption;

    // 覆写 init：基类 init（含 initDCNRegOffs）之后执行一次完整的显示初始化编排（先保存基类实现作 super 调用）。
    //   注：原先还在 `setCurrentDisplayOffset`（macOS ≤ 10.14 同步提交路径）上挂 resync_fifo 覆写，
    //   第七步已移除 —— 该 vft 槽在目标系统（Ventura 13.6，≥ macOS 13）上**不安装**
    //   （三个守卫一致：本类 resolve、基类 populateVFT、HWDisplay.hpp 的 Constants()），
    //   即那段代码在目标系统上是死代码；resync 已并入下面的统一编排（见 applyInitialDisplaySequence）。
    superInit = constants.vftInit(vft.inner());
    constants.vftInit(vft.inner()) = init;
    PenguinWizardry::RuntimeMCManager::singleton().registerMC(gRTMetaClass, kext,
                                                              AMDRadeonX5000_AMDGFX9DCNDisplay::gRTMetaClass);

    DBGLOG("GFX9DCN314Display", "Module initialised");
}

// dcn314 HUBP/OTG 寄存器偏移：与 DCN2 100% 一致（已审查确认）
// 见 docs/project-knowledge.md「DCN2 与 dcn314 寄存器偏移 100% 一致【已审查✅ 双 Verifier】」
void AMDRadeonX5000_AMDGFX9DCN314Display::initDCNRegOffs(AMDRadeonX5000_AMDGFX9DCN314Display* const self)
{
    auto& expansion = self->getExpansion();
    for (UInt32 i = 0; i < MAX_SUPPORTED_DISPLAYS_RV; i += 1) {
        const UInt32 hubpRegStride               = HUBP_REG_STRIDE * i;
        const UInt32 otgRegStride                = OTG_REG_STRIDE * i;
        auto&        regOffs                     = expansion.regOffs[i];
        regOffs.isValid                          = true;
        regOffs.hubpretControl                   = DCN_BASE_2 + HUBPRET_CONTROL + hubpRegStride;
        regOffs.hubpSurfaceConfig                = DCN_BASE_2 + HUBP_SURFACE_CONFIG + hubpRegStride;
        regOffs.hubpAddrConfig                   = DCN_BASE_2 + HUBP_ADDR_CONFIG + hubpRegStride;
        regOffs.hubpTilingConfig                 = DCN_BASE_2 + HUBP_TILING_CONFIG + hubpRegStride;
        regOffs.hubpPriViewportStart             = DCN_BASE_2 + HUBP_PRI_VIEWPORT_START + hubpRegStride;
        regOffs.hubpPriViewportDimension         = DCN_BASE_2 + HUBP_PRI_VIEWPORT_DIMENSION + hubpRegStride;
        regOffs.hubpreqSurfacePitch              = DCN_BASE_2 + HUBPREQ_SURFACE_PITCH + hubpRegStride;
        regOffs.hubpreqPrimarySurfaceAddress     = DCN_BASE_2 + HUBPREQ_PRIMARY_SURFACE_ADDRESS + hubpRegStride;
        regOffs.hubpreqPrimarySurfaceAddressHigh = DCN_BASE_2 + HUBPREQ_PRIMARY_SURFACE_ADDRESS_HIGH + hubpRegStride;
        regOffs.hubpreqFlipControl               = DCN_BASE_2 + HUBPREQ_FLIP_CONTROL + hubpRegStride;
        regOffs.hubpreqSurfaceEarliestInuse      = DCN_BASE_2 + HUBPREQ_SURFACE_EARLIEST_INUSE + hubpRegStride;
        regOffs.hubpreqSurfaceEarliestInuseHigh  = DCN_BASE_2 + HUBPREQ_SURFACE_EARLIEST_INUSE_HIGH + hubpRegStride;
        regOffs.otgControl                       = DCN_BASE_2 + OTG_CONTROL + otgRegStride;
        regOffs.otgInterlaceControl              = DCN_BASE_2 + OTG_INTERLACE_CONTROL + otgRegStride;
    }

    expansion.regShiftsMasks.viewportYStartMask  = 0x3FFF0000;
    expansion.regShiftsMasks.viewportYStartShift = 16;
    expansion.regShiftsMasks.viewportHeightMask  = 0x3FFF0000;
    expansion.regShiftsMasks.viewportHeightShift = 16;
    expansion.regShiftsMasks.primarySurfaceHi    = 0xFFFF;
    expansion.regShiftsMasks.otgEnable           = 1;
    expansion.regShiftsMasks.otgInterlaceEnable  = 1;
    expansion.regShiftsMasks.isValid             = true;
}

// ═════════════════════════════════════════════════════════════════════════════
// 显示时钟下发（路线图第五步）：Linux `dcn314_clk_mgr.c` + `dcn314_smu.c` 的翻译
//
// 分工（路线图 §3.2「同一份生成器，三处共用」）：
//   · 序列生成：`DisplaySeq/Dcn314ClkMgrSeq.hpp` + `DisplaySeq/VbiosSmcSeq.hpp`（与用户态测试共用）
//   · 状态与编排：`DisplaySeq/Dcn314ClkMgr.hpp`
//   · 落到硬件：本文件只提供"寄存器通道"——MP1 段的 VBIOSSMC 邮箱
//     （段内偏移 0x283/0x293/0x29B），经苹果的 cgs MMIO 通道访问。
//
// ⚠️ 已知缺口（登记于路线图 §5.4）：Linux `dcn314_init_clocks` 还需读 CLK 段寄存器
//   （`CLK6_0_CLK6_spll_field_8` 的 spll_ssc_en、`CLK1_CLK2_BYPASS_CNTL` 的 bypass sel）
//   来决定 dp_dto_source_clock；本驱动当前只接 MP1 通道，故该项未实现（不影响内置屏点亮）。
// ═════════════════════════════════════════════════════════════════════════════

static display::dcn314_clk::ClkMgr sClkMgr;

// 构造 VBIOSSMC 邮箱所在的寄存器通道（MP1 段）。
// 通道不可用时返回 false —— 绝不让轮询在空通道上空转到超时（那会白等 2 秒且毫无意义）。
static bool makeSmuChannel(display::RegChannel* const out)
{
    void* const ctx = X5000HWLibs::smuContext();
    if (ctx == nullptr) {
        SYSLOG("GFX9DCN314Display", "display clocks: SMU context unavailable, sequence not sent");
        return false;
    }

    display::RegChannel ch{};
    ch.ctx           = ctx;
    ch.blockInstance = 0;
    ch.block         = static_cast<UInt32>(kCAILHWBlockMP1);
    ch.regOffBase    = 0;
    ch.read  = [](void* c, UInt32 off, UInt32 bi, UInt32 blk, UInt32 base) -> UInt32 {
        return X5000HWLibs::cgsReadReg(c, off, bi, static_cast<CAILHWBlock>(blk), base);
    };
    ch.write = [](void* c, UInt32 off, UInt32 val, UInt32 bi, UInt32 blk, UInt32 base) -> void {
        X5000HWLibs::cgsWriteReg(c, off, val, bi, static_cast<CAILHWBlock>(blk), base);
    };
    ch.delay = [](UInt32 us) -> void { IODelay(us); };

    *out = ch;
    return true;
}

// 初始化阶段的一次性下发：Linux `dcn314_clk_mgr_construct` + 首次 `update_clocks` 的等价物。
// 与影子运行器共用 `ClkMgr::applyInitial` 这一条路径（真机行为与离线比对的序列因此同源）：
//   ① SMU 版本探测 → ② 首次下发（safe_to_lower = false）→ ③ 允许 zstate（safe_to_lower = true）
static bool applyInitialDisplayClocks()
{
    display::RegChannel ch{};
    if (!makeSmuChannel(&ch)) { return false; }
    display::InjectedRegSink sink(ch);

    if (!sClkMgr.applyInitial(sink)) {
        SYSLOG("GFX9DCN314Display", "initial display clock sequence failed (lastOps=%zu)", sClkMgr.lastOpCount());
        return false;
    }
    DBGLOG("GFX9DCN314Display", "initial display clocks applied (smuVersion=0x%X, lastOps=%zu)", sClkMgr.smuVersion(),
           sClkMgr.lastOpCount());
    return true;
}

// DCN 3.1.4 显示时钟下发（VBIOSSMC 完整主流程）。
//   序列由 `DisplaySeq/` 的生成器产出、经 MP1 段邮箱落到硬件（见本文件上方「显示时钟下发」一节）。
//   本函数把调用方给出的目标时钟转成生成器输入，用于**模式变更时按需更新**；
//   初始化阶段的一次性下发不经过本函数，而走 `ClkMgr::applyInitial`（见 `applyInitialDisplayClocks`）。
void AMDRadeonX5000_AMDGFX9DCN314Display::updateDisplayClocks(AMDRadeonX5000_AMDGFX9DCN314Display* const self,
                                                              const struct dcn314_display_clock_req* const req)
{
    if (req == nullptr) { return; }

    // 目标时钟：Linux 里由带宽/时序层填进 `struct dc_clocks`；本驱动没有该层，
    // 故由调用方直接给出目标值（路线图 §5.4 简化项 1：点亮阶段用固定保守值）。
    display::dcn314_clk::TargetClocks tgt{};
    tgt.dcfclkKhz          = req->hard_min_dcfclk_khz;
    tgt.dcfclkDeepSleepKhz = req->min_deep_sleep_dcfclk_khz;
    tgt.dppclkKhz          = req->dppclk_khz;
    tgt.dispclkKhz         = req->dispclk_khz;
    tgt.zstateSupport      = sClkMgr.profile().zstateSupport;
    tgt.dtbclkEn           = false;   // 真值序列里无 0x17 SetDtbClk ⇒ 不请求开 DTB clk

    display::RegChannel ch{};
    if (!makeSmuChannel(&ch)) { return; }
    display::InjectedRegSink sink(ch);

    if (!sClkMgr.apply(tgt, /*safeToLower=*/false, sink)) {
        SYSLOG("GFX9DCN314Display", "updateDisplayClocks: sequence failed (dispclk %u kHz, lastOps=%zu)",
               req->dispclk_khz, sClkMgr.lastOpCount());
        return;
    }
    DBGLOG("GFX9DCN314Display", "updateDisplayClocks: applied dispclk %u / dppclk %u kHz (lastOps=%zu)",
           req->dispclk_khz, req->dppclk_khz, sClkMgr.lastOpCount());
}

// dcn314 的 DCN3 翻转选项（基类硬编码 DCN2，已审查确认）
AMDFlipOption AMDRadeonX5000_AMDGFX9DCN314Display::getFlipOption(AMDRadeonX5000_AMDHWDisplay*)
{ return AMDFlipOption::DCN3; }

// ═════════════════════════════════════════════════════════════════════════════
// 显示初始化编排（路线图第七步·集成）
//
// 把第五步（时钟）、第七步（像素率分频）、第六步（ODM、FIFO resync）四个功能块串成
// **一条**调用链，落在一个入口上。顺序依据 Linux 源码（逐条出处如下）：
//
//   dcn31_init_hw                       (hwss/dcn31/dcn31_hwseq.c:124-125)   ← ① init_clocks
//   dcn20_enable_stream_timing          (hwss/dcn20/dcn20_hwseq.c:846-847)   ← ② set_pixel_rate_div
//   dcn20_apply_single_controller_ctx   (hwss/dcn20/dcn20_hwseq.c:1954)      ← ③ update_odm
//   dce110_apply_ctx_to_hw              (hwss/dce110/dce110_hwseq.c:2737)    ← ④ resync_fifo
//
// 为什么四个阶段"各自独立、互不中断"：Linux 里它们分属不同的调用点，任一段失败不会阻止
// 后续段（例如 set_pixel_rate_div 拒绝 NA 值后，ODM 照常配置）。本编排保持同一语义：
// 每阶段独立判定成功/失败，失败只记日志、不回滚、不中断。
//
// 可观测标记（路线图第七步·子步骤 2）：每阶段前后各留一条固定前缀的日志，并用一个位图
// 汇总"哪些阶段走到了"，便于真机取回日志后一眼定位断点。**不占用 panic 消息**（铁律 5）。
// ═════════════════════════════════════════════════════════════════════════════

// 阶段位图（仅供日志汇总；不是探针位，不进 panic 消息）
enum DisplayInitStage : UInt32 {
    kStageClocks     = 1u << 0,
    kStagePixelRate  = 1u << 1,
    kStageOdm        = 1u << 2,
    kStageFifoResync = 1u << 3,
};

// 像素率分频：Linux `dccg314_set_pixel_rate_div`（dccg/dcn314/dcn314_dccg.c:101-146）
//
// 与 Linux 的三个门逐条对应：
//   门① NA 拒绝 —— 由生成器 `generateSetPixelRateDiv` 返回 false 实现（见 Dcn314DccgSeq.hpp）；
//   门② 与当前值相同则跳过 —— 在本函数里：先读回、提取字段、比对，相同则不下发；
//   门③ `REG_UPDATE_2(OTG_PIXEL_RATE_DIV, OTGn_K1, k1, OTGn_K2, k2)` —— 生成器合并为一次 Update。
void AMDRadeonX5000_AMDGFX9DCN314Display::applyPixelRateDiv(AMDRadeonX5000_AMDHWRegisters& regs,
                                                            const UInt32                    otgInst)
{
    // ── 输入（点亮阶段）────────────────────────────────────────────────────────
    // Linux 的输入来自 pipe_ctx/stream/timing；本驱动没有该对象模型（路线图 §3.1），
    // 故按"内置屏点亮"这一确定场景给出输入，每条都有判据：
    //   · signal            = SIGNAL_TYPE_EDP —— ThinkBook 14+ 2023 的屏是 eDP；eDP 属 DP 信号族
    //                         （PixelDiv/PixelDiv.hpp 的 isDpSignal 含 SIGNAL_TYPE_EDP）。
    //   · pixelEncoding     = PIXEL_ENCODING_RGB —— 点亮阶段未启用 YCbCr 压缩。
    //   · is128b132bSignal  = false —— 非 128b/132b 压缩链路（该场景仅 DSC/特定 DP 链路使用）。
    //   · twoPixPerContainer= false —— 判据同第六步：真值里 `OTG_H_TIMING_DIV_MODE` 字段读=写。
    //   · odmCombineFactor  = 1 —— 初始拓扑是 bypass（与同批的 update_odm_direct 传 opp_cnt=1 一致）。
    // 这些输入若将来随模式变更而变，应由模式设置路径重新调用本函数（不在点亮范围内）。
    pixdiv::K1K2Inputs in{};
    in.signal             = pixdiv::SIGNAL_TYPE_EDP;
    in.pixelEncoding      = pixdiv::PIXEL_ENCODING_RGB;
    in.is128b132bSignal   = false;
    in.twoPixPerContainer = false;
    in.odmCombineFactor   = 1;

    // K1/K2 决策：复用 `PixelDiv/`（TDD 落地版，零寄存器依赖）——绝不写第二份实现。
    const pixdiv::K1K2Result r = pixdiv::calculateDccgK1K2Values(in);

    display::RegChannel ch{};
    makeDcnChannel(regs, &ch);

    // 门②：先读回当前值（Linux `dccg314_get_pixel_rate_div`），相同时跳过下发。
    {
        static constexpr size_t kMaxOps = 4;
        display::RegOp          rbuf[kMaxOps]{};
        display::RegSeq         rseq(rbuf, kMaxOps);
        display::dcn314_dccg::generateReadPixelRateDiv(rseq, DCN_SEG1_BASE, otgInst);

        display::InjectedRegSink rsink(ch);
        if (rsink.executeAll(rseq) == rseq.size()) {
            const UInt32 cur = rsink.lastValue();
            const UInt32 curK1 = display::dcn314_dccg::extractK1(otgInst, cur);
            const UInt32 curK2 = display::dcn314_dccg::extractK2(otgInst, cur);
            if (curK1 == r.k1Div && curK2 == r.k2Div) {
                DBGLOG("GFX9DCN314Display", "pixel_rate_div: otg=%u 已是 K1=%u K2=%u，跳过下发（Linux 门②）",
                       otgInst, r.k1Div, r.k2Div);
                return;
            }
        } else {
            // 读失败不阻断：按 Linux"读不到就照写"的更保守一档处理（写仍然安全：只改本实例的位域）
            DBGLOG("GFX9DCN314Display", "pixel_rate_div: otg=%u 读回失败，按需直接下发", otgInst);
        }
    }

    // 门①③：生成写序列并执行
    {
        static constexpr size_t kMaxOps = 4;
        display::RegOp          buf[kMaxOps]{};
        display::RegSeq         seq(buf, kMaxOps);
        if (!display::dcn314_dccg::generateSetPixelRateDiv(seq, DCN_SEG1_BASE, otgInst, r.k1Div, r.k2Div)) {
            // 门①（NA）或非法实例：与 Linux 的 BREAK_TO_DEBUGGER + return 同语义
            SYSLOG("GFX9DCN314Display", "pixel_rate_div: otg=%u K1=%u K2=%u 被生成器拒绝（NA/非法实例），不下发",
                   otgInst, r.k1Div, r.k2Div);
            return;
        }

        display::InjectedRegSink sink(ch);
        const size_t             done = sink.executeAll(seq);
        if (done != seq.size()) {
            SYSLOG("GFX9DCN314Display", "pixel_rate_div: 序列执行中断（done=%zu/%zu，otg=%u）", done, seq.size(),
                   otgInst);
            return;
        }
        DBGLOG("GFX9DCN314Display", "pixel_rate_div: otg=%u 下发 K1=%u K2=%u（%zu op，odmFactor=%u）", otgInst,
               r.k1Div, r.k2Div, seq.size(), r.odmCombineFactor);
    }
}

// ─── 编排分阶段探针（第八步第 3 批次）────────────────────────────────────────
//  用途：`-NRedSeqPanicN` 存在时，在第 N 段**完成之后**立刻 panic，把"编排走到哪一段"
//        经已验证可靠的 panic→efivarfs 通道带出来。
//  为什么需要：若系统在编排期间**卡住**（不 panic、不崩溃），调用栈取不到任何证据；
//        分阶段 panic 能把"卡在哪一段"变成可判读文本（且 panic 后仍能自动重启，不挂死）。
//  铁律：实参只能是已求值的局部变量；默认不生效（boot-arg 门控）。
static void seqStageProbe(UInt32 stage, UInt32 stages)
{
    bool want = false;
    switch (stage) {
        case 1: want = checkKernelArgument("-NRedSeqPanic1"); break;
        case 2: want = checkKernelArgument("-NRedSeqPanic2"); break;
        case 3: want = checkKernelArgument("-NRedSeqPanic3"); break;
        case 4: want = checkKernelArgument("-NRedSeqPanic4"); break;
        default: break;
    }
    if (!want) { return; }
    const UInt64 vStage = stage, vStages = stages;
    panic("NRed display-init probe: stage %llu done (stages=0x%llx)", vStage, vStages);
}

// 显示初始化编排入口：由 `init` 调用一次（守卫在 init 里）。
void AMDRadeonX5000_AMDGFX9DCN314Display::applyInitialDisplaySequence(AMDRadeonX5000_AMDGFX9DCN314Display* const self)
{
    UInt32 stages = 0;

    DBGLOG("GFX9DCN314Display", "display-init: sequence start");
    StageMark::mark("seq-start");

    // ① 显示时钟（第五步）：VBIOSSMC 完整主流程。
    //    对应 Linux `dcn31_init_hw` 的第一步 `clk_mgr->funcs->init_clocks`（dcn31_hwseq.c:124-125）。
    //    ⚠️ 顺序修正：原先 init 里是"先 ODM 后时钟"，与 Linux 相反（时钟是显示管线的前提，必须先上）。
    if (applyInitialDisplayClocks()) {
        stages |= kStageClocks;
    } else {
        SYSLOG("GFX9DCN314Display", "display-init: stage clocks FAILED");
    }
    seqStageProbe(1, stages);

    auto* const regs = self->getHWRegisters();
    if (regs == nullptr) {
        SYSLOG("GFX9DCN314Display", "display-init: HW registers unavailable, stages 2-4 skipped (mask=0x%X)", stages);
        return;
    }

    // ② 像素率分频（第七步新增）：Linux `dcn20_enable_stream_timing` 的首步（dcn20_hwseq.c:846-847）。
    //    对全部 OTG 实例下发（Linux 逐 pipe 调用，本驱动没有 pipe 模型，按实例遍历）。
    for (UInt32 i = 0; i < MAX_SUPPORTED_DISPLAYS_RV; i += 1) {
        applyPixelRateDiv(*regs, i);
    }
    stages |= kStagePixelRate;
    seqStageProbe(2, stages);

    // ③ ODM 拓扑（第六步）：Linux `dcn20_apply_single_controller_ctx_to_hw` 里的
    //    `hws->funcs.update_odm`（dcn20_hwseq.c:1954）。
    {
        const UInt32 oppInst[1] = {0};
        for (UInt32 i = 0; i < MAX_SUPPORTED_DISPLAYS_RV; i += 1) {
            update_odm_direct(*regs, i, oppInst, 1, 0);
        }
        DBGLOG("GFX9DCN314Display", "display-init: update_odm_direct (bypass) applied for %u OTG(s)",
               MAX_SUPPORTED_DISPLAYS_RV);
        stages |= kStageOdm;
    }
    seqStageProbe(3, stages);

    // ④ FIFO 重同步（第六步）：Linux `dce110_apply_ctx_to_hw` 每 pipe 应用后调
    //    `resync_fifo_dccg_dio`（dce110_hwseq.c:2737）→ `trigger_dio_fifo_resync`。
    //    ⚠️ 挂接点迁移：原先挂在 `setCurrentDisplayOffset`（该 vft 槽在 Ventura 上**不安装**，
    //    是死代码）；现并入本编排，使目标系统上确实会执行。
    //    与 Linux 的时机差异（Linux 在 stream enable 之后，本驱动在 init 之后）见执行记录 §五。
    resync_fifo_dccg_dio_direct(*regs);
    DBGLOG("GFX9DCN314Display", "display-init: resync_fifo_dccg_dio_direct applied");
    stages |= kStageFifoResync;
    seqStageProbe(4, stages);

    // 汇总标记：真机取回日志后，用这一行判断"编排走到哪一段"。
    DBGLOG("GFX9DCN314Display",
           "display-init: sequence done (stages=0x%X clocks=%u pixrate=%u odm=%u resync=%u)", stages,
           (stages & kStageClocks) != 0, (stages & kStagePixelRate) != 0, (stages & kStageOdm) != 0,
           (stages & kStageFifoResync) != 0);
    // NVRAM 侧的同一信息（`-NRedStageMark` 时生效）：崩溃发生在编排之后也能取回阶段位图
    StageMark::markHex("seq-done", stages);
}

// init 覆写：先走基类 init（super chain：原版 init → isDCN → initDCNRegOffs 经 vft slot 0 分发到本类），
// 完成后（initDCNRegOffs 之后、首次 flip 之前）执行**一次**完整的显示初始化编排。
// 守卫: sDisplayInitSeqApplied 保证只执行一次（模式变更入口，勿随每帧 flip 重复）。
bool AMDRadeonX5000_AMDGFX9DCN314Display::init(AMDRadeonX5000_AMDHWDisplay* const _self, void* const hwInterface,
                                               void* const fbParams)
{
    StageMark::mark("disp-init-enter");
    if (!superInit(_self, hwInterface, fbParams)) {
        StageMark::mark("disp-init-basefail");
        return false;
    }

    if (!sDisplayInitSeqApplied) {
        sDisplayInitSeqApplied = true;
        // 第 3 批次判别性对照（2026-09-26）：`-NRedNoDisplaySeq` 时**整段编排跳过**。
        //   依据：引导 3/4 实测——关掉 D3 后系统能启动到用户态/存储栈（此前从未到达），
        //   但整体**卡住**（watchdog 无 checkin / NVMe 命令超时）。本编排含四段真写硬件的
        //   操作，是"卡住"的首要嫌疑；该门控用于把"编排"这一个变量单独摘掉做对照。
        //   默认（无参数）行为完全不变。
        if (checkKernelArgument("-NRedNoDisplaySeq")) {
            SYSLOG("GFX9DCN314Display", "display-init: SKIPPED by -NRedNoDisplaySeq (control run)");
        } else {
            applyInitialDisplaySequence(static_cast<AMDRadeonX5000_AMDGFX9DCN314Display*>(_self));
        }
    }

    StageMark::mark("disp-init-ok");
    return true;
}

// =============================================================================
// update_odm / resync_fifo 寄存器级直译
//   设计: research/updateodm-resync-directreg-design.md (Verifier 4/4 PASS)
//   所有 mask/shift/偏移均来自已审查文档与 DCN314.hpp，未推测。
// =============================================================================

// 寄存器位操作 helper（展平 REG_SET / REG_UPDATE / REG_GET）——仅供 resync_fifo 使用。
// （ODM 路径已改为"生成器 + sink"，见下方 update_odm_direct。）
static inline void dcn314RegSet(AMDRadeonX5000_AMDHWRegisters& regs, UInt32 addr,
                                UInt32 mask, UInt32 shift, UInt32 val)
{
    UInt32 v = regs.read(addr);
    v = (v & ~mask) | ((val << shift) & mask);
    regs.write(addr, v);
}

// DCN 段的寄存器通道：把 RegOp 的读写落到 display 对象的寄存器访问器（`AMDHWRegisters::read/write`，
// 地址语义 = **绝对 dword 索引**，与 Linux 真值记录同一坐标系）上。
//   为什么单独建通道：ODM 的寄存器在 SEG2/SEG3（不是时钟用的 MP1 段），需要另一条 MMIO 通路；
//   但消费者仍是第五步那个 `InjectedRegSink`（路线图 §3.2：同一份消费者代码，三处共用）。
static void makeDcnChannel(AMDRadeonX5000_AMDHWRegisters& regs, display::RegChannel* const out)
{
    display::RegChannel ch{};
    ch.ctx           = &regs;
    ch.blockInstance = 0;
    ch.block         = 0;
    ch.regOffBase    = 0;
    ch.read  = [](void* c, UInt32 off, UInt32, UInt32, UInt32) -> UInt32 {
        return static_cast<AMDRadeonX5000_AMDHWRegisters*>(c)->read(off);
    };
    ch.write = [](void* c, UInt32 off, UInt32 val, UInt32, UInt32, UInt32) -> void {
        static_cast<AMDRadeonX5000_AMDHWRegisters*>(c)->write(off, val);
    };
    ch.delay = nullptr;  // ODM 序列无延时/轮询
    *out     = ch;
}

void AMDRadeonX5000_AMDGFX9DCN314Display::update_odm_direct(AMDRadeonX5000_AMDHWRegisters& regs,
                                                            UInt32                          otg_inst,
                                                            const UInt32                    opp_inst[],
                                                            UInt32                          opp_cnt,
                                                            UInt32                          slice_width)
{
    // ⚠️ 这里**不再**逐寄存器直写：序列由 `DisplaySeq/Dcn314OdmSeq.hpp` 生成，
    //    与用户态的离线影子运行共用同一份代码（路线图 §3.2「禁止第二份手写实现」）。
    //    生成器逐行对照 Linux `dcn314_optc.c` / `dcn30_mpc.c`，并区分 REG_SET（不读直写）
    //    与 REG_UPDATE（读-改-写）两种硬件形态——真值序列印证了这两种形态。
    static constexpr size_t kMaxOdmOps = 16;
    display::RegOp          buf[kMaxOdmOps]{};
    display::RegSeq         seq(buf, kMaxOdmOps);

    // twoPixelsPerContainer：Linux `optc->funcs->is_two_pixels_per_container(dc_crtc_timing)`
    // （dcn314_optc.c:173）。本驱动没有 timing 对象；点亮阶段按"每容器 1 像素"处理（= NO_DIV）。
    // 判据：真值里 `OTG_H_TIMING_DIV_MODE` 字段读=写（字段值 0），即 Linux 那次也用了 NO_DIV。
    display::dcn314_odm::generateUpdateOdmFull(seq, DCN_SEG2_BASE, DCN_SEG3_BASE, otg_inst, opp_inst, opp_cnt,
                                               slice_width, /*twoPixelsPerContainer=*/false);
    if (seq.overflowed()) {
        SYSLOG("GFX9DCN314Display", "update_odm_direct: 序列溢出（capacity=%zu）", kMaxOdmOps);
        return;
    }

    display::RegChannel ch{};
    makeDcnChannel(regs, &ch);
    display::InjectedRegSink sink(ch);
    const size_t             done = sink.executeAll(seq);
    if (done != seq.size()) {
        SYSLOG("GFX9DCN314Display", "update_odm_direct: 序列执行中断（done=%zu/%zu，otg=%u）", done, seq.size(),
               otg_inst);
        return;
    }
    DBGLOG("GFX9DCN314Display", "update_odm_direct: 下发 %zu 个寄存器操作（otg=%u opp_cnt=%u）", seq.size(),
           otg_inst, opp_cnt);
}

void AMDRadeonX5000_AMDGFX9DCN314Display::resync_fifo_dccg_dio_direct(AMDRadeonX5000_AMDHWRegisters& regs)
{
    // ⚠️ DENTIST_DISPCLK_CNTL 的 BASE_IDX = 1（不是 2）→ 必须用 SEG1 基址（0xC0 + 0x64 = 0x124）。
    //    真值证据：0x124 有 3 个事件（含一次 0x7f1000 → 0x7f1010 的读改写），而 0x3524 零事件。
    const UInt32 addr = DCN_SEG1_BASE + DENTIST_DISPCLK_CNTL;

    // REG_GET(DENTIST_DISPCLK_CNTL, DENTIST_DISPCLK_RDIVIDER, &v)
    const UInt32 rdiv = (regs.read(addr) & DENTIST_DISPCLK_RDIVIDER_MASK) >> DENTIST_DISPCLK_RDIVIDER_SHIFT;
    // REG_UPDATE(DENTIST_DISPCLK_CNTL, DENTIST_DISPCLK_WDIVIDER, rdiv)
    // 保护: RDIVIDER==0 时不写，避免 WDIVIDER=0 非法 (dcn32_dccg.c 风格)
    if (rdiv != 0) {
        dcn314RegSet(regs, addr, DENTIST_DISPCLK_WDIVIDER_MASK, DENTIST_DISPCLK_WDIVIDER_SHIFT, rdiv);
    }
}
