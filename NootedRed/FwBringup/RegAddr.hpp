// RegAddr —— MP0/MP1 SMN 寄存器字节地址的单一事实源（header-only）
//
// 乙线（自建固件层）所有 PSP/SMU 邮箱寄存器的寻址都经本文件的 smnAddr() 计算，
// 等价于 Linux 的 RREG32_SOC15_EXT / WREG32_SOC15_EXT。
//
// ── 公式与依据（全部来自 oldfiles-handoff/reference/linux/ 的 amdgpu 源码）────
//
// 1. 地址公式 —— RREG32_SOC15_EXT（amdgpu/soc15_common.h:201-204）：
//        SMN_字节地址 = ( 段基址[reg##_BASE_IDX] + 寄存器偏移 ) * 4 + smn_base64
//    即「(段基址 + dword 偏移) × 4」得字节地址，再叠加 smn_base64。
//
// 2. 段基址 —— 本模块涉及的 MP0/MP1 SMN 寄存器在 mp_13_0_4_offset.h 里
//    BASE_IDX = 1（例：regMP0_SMN_C2PMSG_35_BASE_IDX = 1，该文件第 38 行）⇒ 取 SEG1。
//    Phoenix 的 SEG1 取值见本机 Linux 实跑通路径
//    display/dc/clk_mgr/dcn314/dcn314_smu.c:38-43：
//        SEG0 = 0x00016000、SEG1 = 0x0243FC00（SEG2..5 本模块用不到）。
//
// 3. smn_base64 = amdgpu_reg_get_smn_base64(adev, MP0_HWIP, inst)：
//    amdgpu_reg_access.c:321-348 的 amdgpu_reg_smn_v1_0_get_base() 在 die_inst == 0
//    （单 die APU）时返回 0 ⇒ 本机为 0。
//    （用法实例：amdgpu/psp_v13_0.c:171 读 C2PMSG_92 同样走 RREG32_SOC15_EXT。）
//
// 4. 访问方式 —— 算出的字节地址（如 C2PMSG_81 = 0x090FF244）远超 BAR5 的 512 KB
//    MMIO 窗口，必须走 PCIE 间接通道：把字节地址写 PCIE_INDEX2（dword 索引 0x0E，
//    Regs/NBIO.hpp），从 PCIE_DATA2（0x0F）读 —— 等价 Linux 的 RREG32_PCIE_EXT
//    （amdgpu_reg_access.c:612 amdgpu_device_indirect_rreg_ext）。
//    ⚠️ 若地址超过 32 位还需先写 PCIE_INDEX_HI（Linux AMDGPU_PCIE_INDEX_HI_FALLBACK
//       = 0x44 >> 2 = 0x11，amdgpu_reg_access.c:33；写入判据见 :628 `if (reg_addr >> 32)`、
//       :642-650）。本机最大地址 0x090FFA68 < 2^32，故不需要写 HI
//       （判据：addr >> 32 == 0）。
//
// 5. 与现役 NRed::readReg32/writeReg32 的衔接（NRed.cpp:241-257）：
//        readReg32(reg):  if (reg*4 < rmmio长度) rmmioPtr[reg];        // reg 当 dword 索引
//                         else { rmmioPtr[PCIE_INDEX2] = reg; return rmmioPtr[PCIE_DATA2]; }
//    直接分支把 reg 当 dword 索引；越窗分支把 reg **原样**写进 PCIE_INDEX2 —— 即要求
//    调用方传**字节地址**。因此 smnAddr() 返回 (SEG1 + off) * 4 这个字节地址，
//    而非 dword 索引；越窗寄存器一律走间接分支（地址 > BAR5 512K）。
//
// 约束：header-only、零动态分配、不含 IOKit 之外的内核头；命名空间 fw。

#pragma once

#include "../DisplaySeq/RegOp.hpp"  // display::RegAddr (= uint32_t)

namespace fw {

// MP0/MP1 SMN 寄存器的 SEG1 段基址（dword 单位）。
// 依据：dcn314_smu.c:38-43（MP1_BASE__INST0_SEG1 = 0x0243FC00）；
//       mp_13_0_4_offset.h 对勾选的 MP0/MP1 C2PMSG 寄存器 BASE_IDX = 1 ⇒ 用 SEG1。
inline constexpr uint32_t kMpSeg1Base = 0x0243FC00;

// 计算 MP0/MP1 SMN 寄存器的字节地址（供 NRed::readReg32/writeReg32 的越窗间接分支）。
//
// @param regOff  寄存器 dword 偏移（mp_13_0_4_offset.h 的 reg* 值，如 C2PMSG_81 = 0x91）
// @return        SMN 字节地址 = (kMpSeg1Base + regOff) * 4   （smn_base64 = 0，见上 §3）
//
// 等价 Linux：RREG32_SOC15_EXT(MP0_HWIP, inst, reg) 展开为
//   RREG32_PCIE_EXT((reg_offset[MP0_HWIP][inst][reg##_BASE_IDX] + reg) * 4 + smn_base64)
// 其中 reg_offset[...][BASE_IDX=1] 即 SEG1 = kMpSeg1Base，smn_base64 = 0。
inline constexpr display::RegAddr smnAddr(uint32_t regOff) {
    return (kMpSeg1Base + regOff) * 4;
}

// ── 编译期自检：等价关系与定标寄存器的真实字节地址 ──
// smnAddr(off) == (SEG1 + off) * 4  对任意 off 成立（公式本身）
static_assert(smnAddr(0x51) == (0x0243FC00u + 0x51) * 4,
              "smnAddr(off) == (kMpSeg1Base + off) * 4");
// C2PMSG_81 (0x91, SOS 存活检测) —— mp_13_0_4_offset.h:129
static_assert(smnAddr(0x91) == (0x0243FC00u + 0x91) * 4,
              "C2PMSG_81: smnAddr equivalence");
static_assert(smnAddr(0x91) == 0x090FF244u,
              "C2PMSG_81 SMN byte address = 0x090FF244");
// C2PMSG_35 (0x63, bootloader 命令/状态) —— mp_13_0_4_offset.h:37
static_assert(smnAddr(0x63) == 0x090FF18Cu,
              "C2PMSG_35 SMN byte address = 0x090FF18C");

}  // namespace fw
