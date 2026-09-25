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
#include <PenguinWizardry/RuntimeMC.hpp>
#include <PenguinWizardry/RuntimeVFT.hpp>
#include <Regs/DCN314.hpp>
#include <DisplaySeq/Dcn314ClkMgr.hpp>
#include <GPUDriversAMD/CAIL/HWBlock.hpp>
#include <IOKit/IOLib.h>
#include <DisplaySeq/Dcn314OdmSeq.hpp>
#include <libkern/OSTypes.h>
#include <libkern/c++/OSMetaClass.h>

// -----------------------------------------------------------------------------
// 纯策略函数: dc_is_*_signal 纯 helper (移植自 Linux signal_types.h)
// -----------------------------------------------------------------------------
static inline bool dc_is_hdmi_tmds_signal(enum signal_type signal)
{
    return (signal == SIGNAL_TYPE_HDMI_TYPE_A);
}

static inline bool dc_is_hdmi_frl_signal(enum signal_type signal)
{
    return (signal == SIGNAL_TYPE_HDMI_FRL);
}

static inline bool dc_is_hdmi_signal(enum signal_type signal)
{
    return (dc_is_hdmi_tmds_signal(signal) || dc_is_hdmi_frl_signal(signal));
}

static inline bool dc_is_dp_signal(enum signal_type signal)
{
    return (signal == SIGNAL_TYPE_DISPLAY_PORT ||
            signal == SIGNAL_TYPE_EDP ||
            signal == SIGNAL_TYPE_DISPLAY_PORT_MST);
}

static inline bool dc_is_dvi_signal(enum signal_type signal)
{
    switch (signal) {
    case SIGNAL_TYPE_DVI_SINGLE_LINK:
    case SIGNAL_TYPE_DVI_DUAL_LINK:
        return true;
    default:
        return false;
    }
}

static inline bool dc_is_virtual_signal(enum signal_type signal)
{
    return (signal == SIGNAL_TYPE_VIRTUAL);
}

// -----------------------------------------------------------------------------
// 纯策略函数 1: dcn314_calc_k1_k2_values
//   移植自 dcn314_calculate_dccg_k1_k2_values (dcn314_hwseq.c L329)
//   去寄存器化: 所有 vtable/资源遍历输入由 dcn314_k1k2_inputs 预解析提供
// -----------------------------------------------------------------------------
static unsigned int dcn314_calc_k1_k2_values(const struct dcn314_k1k2_inputs *in,
                                              unsigned int *k1_div,
                                              unsigned int *k2_div)
{
    unsigned int odm_combine_factor = in->odm_combine_factor;
    bool two_pix_per_container = in->two_pix_per_container;

    *k1_div = PIXEL_RATE_DIV_NA;
    *k2_div = PIXEL_RATE_DIV_NA;

    if (dc_is_hdmi_frl_signal(in->signal) ||
        in->is_128b_132b_signal) {
        *k1_div = PIXEL_RATE_DIV_BY_1;
        *k2_div = PIXEL_RATE_DIV_BY_1;
    } else if (dc_is_hdmi_tmds_signal(in->signal) ||
               dc_is_dvi_signal(in->signal)) {
        *k1_div = PIXEL_RATE_DIV_BY_1;
        if (in->pixel_encoding == PIXEL_ENCODING_YCBCR420)
            *k2_div = PIXEL_RATE_DIV_BY_2;
        else
            *k2_div = PIXEL_RATE_DIV_BY_4;
    } else if (dc_is_dp_signal(in->signal) ||
               dc_is_virtual_signal(in->signal)) {
        if (two_pix_per_container) {
            *k1_div = PIXEL_RATE_DIV_BY_1;
            *k2_div = PIXEL_RATE_DIV_BY_2;
        } else {
            *k1_div = PIXEL_RATE_DIV_BY_1;
            *k2_div = PIXEL_RATE_DIV_BY_4;
            if (odm_combine_factor == 2)
                *k2_div = PIXEL_RATE_DIV_BY_2;
        }
    }

    return odm_combine_factor;
}

// -----------------------------------------------------------------------------
// 纯策略函数 2: dcn314_calc_pix_rate_divider
//   移植自 dcn314_calculate_pix_rate_divider (dcn314_hwseq.c L366)
//   去寄存器化: 资源查找由调用方完成, 直接接收已解析的 inputs
// -----------------------------------------------------------------------------
static void dcn314_calc_pix_rate_divider(struct pixel_rate_divider *out,
                                          const struct dcn314_k1k2_inputs *in)
{
    unsigned int k1_div = PIXEL_RATE_DIV_NA;
    unsigned int k2_div = PIXEL_RATE_DIV_NA;

    dcn314_calc_k1_k2_values(in, &k1_div, &k2_div);

    out->div_factor1 = k1_div;
    out->div_factor2 = k2_div;
}

// -----------------------------------------------------------------------------
// update_odm / resync_fifo 调用点 (方案: audit-odm-resync-wiring.md)
//   - init()                     : 基类 init（含 initDCNRegOffs）完成后、首次 flip 前 → update_odm_direct
//   - setCurrentDisplayOffset()  : HW 取走 flip 后（基类 isFlipPending 等待结束）→ resync_fifo_dccg_dio_direct
//   守卫: 文件级静态标志，保证每次驱动生命周期只执行一次，绝不进入 per-frame flip 路径。
// -----------------------------------------------------------------------------
static bool (*superInit)(AMDRadeonX5000_AMDHWDisplay*, void*, void*)                   = nullptr;
static void (*superSetCurrentDisplayOffset)(AMDRadeonX5000_AMDHWDisplay*, UInt32, UInt64) = nullptr;
static bool sUpdateOdmApplied  = false;
static bool sResyncFifoApplied = false;

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

    // 覆写 init：基类 init（含 initDCNRegOffs）之后补 update_odm_direct（先保存基类实现作 super 调用）
    superInit = constants.vftInit(vft.inner());
    constants.vftInit(vft.inner()) = init;

    // 覆写 setCurrentDisplayOffset（仅 < macOS 13，与基类 populateVFT 的守卫一致）：
    // HW 取走 flip 后补 resync_fifo_dccg_dio_direct（先保存基类实现作 super 调用）
    if (currentKernelVersion() < MACOS_13) {
        superSetCurrentDisplayOffset = constants.vftSetCurrentDisplayOffset(vft.inner());
        constants.vftSetCurrentDisplayOffset(vft.inner()) = setCurrentDisplayOffset;
    }

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

static bool                        sClocksApplied = false;
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

// init 覆写：先走基类 init（super chain：原版 init → isDCN → initDCNRegOffs 经 vft slot 0 分发到本类），
// 完成后（initDCNRegOffs 之后、首次 flip 之前）为全部 OTG 初始化 ODM bypass 拓扑。
// 守卫: sUpdateOdmApplied 保证只执行一次（模式变更入口，勿随每帧 flip 重复）。
bool AMDRadeonX5000_AMDGFX9DCN314Display::init(AMDRadeonX5000_AMDHWDisplay* const _self, void* const hwInterface,
                                               void* const fbParams)
{
    if (!superInit(_self, hwInterface, fbParams)) { return false; }

    const auto self = static_cast<AMDRadeonX5000_AMDGFX9DCN314Display*>(_self);
    if (!sUpdateOdmApplied) {
        sUpdateOdmApplied = true;
        auto* const regs = self->getHWRegisters();
        if (regs != nullptr) {
            // 初始拓扑：全部 OTG 单管直通（bypass），不拼接（OPPC 拼接由后续模式设置决定）
            const UInt32 oppInst[1] = {0};
            for (UInt32 i = 0; i < MAX_SUPPORTED_DISPLAYS_RV; i += 1) {
                update_odm_direct(*regs, i, oppInst, 1, 0);
            }
            DBGLOG("GFX9DCN314Display", "init: update_odm_direct (bypass) applied for %u OTG(s)",
                   MAX_SUPPORTED_DISPLAYS_RV);
        }
    }

    // 显示时钟：初始化阶段一次性下发（对应 Linux `dcn314_clk_mgr_construct` + 首次 `update_clocks`）。
    // 挂在 init 而不是每次 flip：时钟是模式级配置，随每帧重复下发既无必要也会抖。
    if (!sClocksApplied) {
        sClocksApplied = true;
        applyInitialDisplayClocks();
    }

    return true;
}

// setCurrentDisplayOffset 覆写（macOS ≤ 10.14 同步提交路径）：基类写地址并等待 isFlipPending
// 完成（HW 已取走 flip）之后补 resync_fifo_dccg_dio_direct（DENTIST WDIVIDER = RDIVIDER）。
// 守卫: sResyncFifoApplied 保证只执行一次（模式变更应用后，勿随每帧 flip 重复）。
void AMDRadeonX5000_AMDGFX9DCN314Display::setCurrentDisplayOffset(AMDRadeonX5000_AMDHWDisplay* const _self,
                                                                  const UInt32 fbIndex, const UInt64 value)
{
    superSetCurrentDisplayOffset(_self, fbIndex, value);

    const auto self = static_cast<AMDRadeonX5000_AMDGFX9DCN314Display*>(_self);
    if (!sResyncFifoApplied) {
        sResyncFifoApplied = true;
        auto* const regs = self->getHWRegisters();
        if (regs != nullptr) {
            resync_fifo_dccg_dio_direct(*regs);
            DBGLOG("GFX9DCN314Display", "setCurrentDisplayOffset: resync_fifo_dccg_dio_direct applied");
        }
    }
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
