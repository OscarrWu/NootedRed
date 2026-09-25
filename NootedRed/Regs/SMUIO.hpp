// Copyright © 2024-2025 ChefKiss. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once
#include <IOKit/IOTypes.h>
// ===== ROM 口（VBIOS ROM_INDEX/ROM_DATA，BASE_IDX=0，SMUIO_BASE_0=0x16800）=====
// 偏移按 0xE4/0xE5 修正：苹果 12.5 二进制实测写入绝对索引 0x168E4/0x168E5
// （扫描 0x168E4/0x168E5 立即数各命中 4/5 处，而旧值 0x16828/0x16829 在二进制中 0 命中，
// 见 docs/子任务/第一步偏移审计报告.md §1）。
// Linux 头文件中同值者：smuio_11_0_0_offset.h:230-233（mmROM_INDEX/DATA，Navi10）、
// smuio_13_0_6_offset.h:296-299（Navi3x）、smuio_14_0_2_offset.h:290-293、smuio_15_0_8_offset.h:290-293。
// 旧值 0x28/0x29 是 smuio_9_0（Raven 代）值（smuio_9_0_offset.h:34-37，mmROM_INDEX/DATA），与苹果二进制不符。
// 注意（不确定性）：13.x 各小版本并不一致——smuio_13_0_2_offset.h:237-240 为 0xE5/0xE6，
// 而 Phoenix（780M）的 smuio_13_0_3 头**无任何 ROM 寄存器**（全文件 grep "ROM" 0 命中）；
// Linux 在 APU 上也不读 ROM 口（amdgpu_bios.c:608-610：AMD_IS_APU → return false，
// 注释原文 "APU vbios image is part of sbios image"，函数 amdgpu_soc15_read_bios_from_rom）。
// 故 ROM 口在 780M 上是否可读属待真机验证项（路线图 §5.4 第 13 项）。
constexpr UInt32 ROM_INDEX = 0xE4;
constexpr UInt32 ROM_DATA  = 0xE5;
