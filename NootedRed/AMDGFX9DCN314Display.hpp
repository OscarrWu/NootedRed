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

class AMDRadeonX5000_AMDGFX9DCN314Display : public AMDRadeonX5000_AMDGFX9DCNDisplay
{
    static VFT vft;

    static void Constructor(AMDRadeonX5000_AMDGFX9DCN314Display* self, const OSMetaClass* metaClass);

    static void initDCNRegOffs(AMDRadeonX5000_AMDGFX9DCN314Display* self);

public:
    PWDeclareRuntimeMC(AMDRadeonX5000_AMDGFX9DCN314Display, Constructor)

    static void resolve(const char* kext);
};
