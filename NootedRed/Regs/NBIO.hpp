// Copyright © 2024-2025 ChefKiss. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once
#include <IOKit/IOTypes.h>

//-------- Base Index 0 --------//

constexpr UInt32 PCIE_INDEX2 = 0xE;
constexpr UInt32 PCIE_DATA2  = 0xF;

//-------- Base Index 2 --------//

// ── 遗留常量（上游 NootedRed 承袭）─────────────────────────────────────
// 注意：名为 RCC_DEV0_EPF0_STRAP0 的寄存器在 nbio_7_11_0_offset.h 中分属
// RCC_STRAP0/1/2/3 四个地址块（段索引 5/2/5/5，宏名 regRCC_STRAP<N>_RCC_DEV0_EPF0_STRAP0），
// 而此处的 0xF 不对应其中任何一个 *_STRAP* 宏（该族仅 0xd000/5、0x15/2、0x8d35/5、0x2ffc0d35/5）。
// 勿用它读 rev-id；正确来源见下方 RCC_STRAP1_RCC_DEV0_EPF0_STRAP0。
constexpr UInt32 RCC_DEV0_EPF0_STRAP0                  = 0xF;
constexpr UInt8  RCC_DEV0_EPF0_STRAP0_ATI_REV_ID_SHIFT = 0x18;
constexpr UInt32 RCC_DEV0_EPF0_STRAP0_ATI_REV_ID_MASK  = 0xF000000;

// ── rev-id strap（路线图第一步修正：0xD2F → 0xD35）────────────────────
// RCC_STRAP1 地址块中的 RCC_DEV0_EPF0_STRAP0 —— 与上方遗留 0xF 常量不是同一个地址！
// 出处（Linux amdgpu）：
// - regRCC_STRAP1_RCC_DEV0_EPF0_STRAP0 = 0x15、BASE_IDX = 2
//   （nbio_7_11_0_offset.h:8818-8819，addressBlock nbio_nbif0_rcc_strap_BIFDEC1）
// - 驱动读取：nbio_v7_11_get_rev_id() 读此寄存器（amdgpu/nbio_v7_11.c:42）
// - 段基址 NBIO_BASE__INST0_SEG2 = 0xD20（yellow_carp_offset.h:975）→ 绝对地址 0xD35
constexpr UInt32 RCC_STRAP1_RCC_DEV0_EPF0_STRAP0 = 0x15;
// ATI_REV_ID 位域与 RCC_STRAP0 块实例一致（SHIFT=0x18、MASK=0x0F000000L：
// nbio_7_11_0_sh_mask.h:55905/55913 vs 50654/50662）；Linux 亦复用 RCC_STRAP0 前缀
// 位域常量解包（nbio_v7_11.c:43-44），故沿用上方 *_ATI_REV_ID_SHIFT/MASK。
