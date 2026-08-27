// DCN314 Register Offsets (Phoenix / Radeon 780M, RDNA3)
//
// 来源：Linux amdgpu dcn_3_1_4_offset.h / _sh_mask.h（已双 Verifier 审查确认）
// - HUBP/OTG：与 DCN2.hpp 100% 一致（复用）
// - OPP：与 DCN2.1 逐字节一致
// - MPCC：stride 0x20，位域可复用 DCN2 但偏移用 dcn314 头
// - DCCG：时钟控制寄存器不叫 DCCG_ 前缀，BASE_IDX=1
// - DIG：5 实例 stride 0x100
// - OPTC/ODM：ODM0_ 前缀，stride 0x10，BASE_IDX=2（update_odm 依赖）
// - DENTIST：DENTIST_DISPCLK_CNTL（resync_fifo 依赖，RDIVIDER/WDIVIDER）
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once
#include <IOKit/IOTypes.h>

// ===== HUBP（与 DCN2 100% 一致，已审查确认）=====
constexpr UInt32 HUBP_REG_STRIDE                      = 0xDC;
constexpr UInt32 HUBPRET_CONTROL                      = 0x66C;
constexpr UInt32 HUBP_SURFACE_CONFIG                  = 0x5E5;
constexpr UInt32 HUBP_ADDR_CONFIG                     = 0x5E6;
constexpr UInt32 HUBP_TILING_CONFIG                   = 0x5E7;
constexpr UInt32 HUBP_PRI_VIEWPORT_START              = 0x5E9;
constexpr UInt32 HUBP_PRI_VIEWPORT_DIMENSION          = 0x5EA;
constexpr UInt32 HUBPREQ_SURFACE_PITCH                = 0x607;
constexpr UInt32 HUBPREQ_PRIMARY_SURFACE_ADDRESS      = 0x60A;
constexpr UInt32 HUBPREQ_PRIMARY_SURFACE_ADDRESS_HIGH = 0x60B;
constexpr UInt32 HUBPREQ_FLIP_CONTROL                 = 0x61B;
constexpr UInt32 HUBPREQ_SURFACE_EARLIEST_INUSE       = 0x625;
constexpr UInt32 HUBPREQ_SURFACE_EARLIEST_INUSE_HIGH  = 0x626;

// ===== OTG/OPTC（与 DCN2 部分一致）=====
constexpr UInt32 OTG_REG_STRIDE        = 0x80;
constexpr UInt32 OTG_CONTROL           = 0x1B41;
constexpr UInt32 OTG_INTERLACE_CONTROL = 0x1B44;
constexpr UInt32 OTG_H_TIMING_CNTL     = 0x1B2E; // OTG0（BASE_IDX=2，update_odm 依赖）

// ===== OPTC/ODM（update_odm 依赖，ODM0_ 前缀，stride 0x10，BASE_IDX=2）=====
constexpr UInt32 ODM_REG_STRIDE            = 0x10;
constexpr UInt32 OPTC_DATA_SOURCE_SELECT   = 0x1ACB; // ODM0（ODM combine 源选择）
constexpr UInt32 OPTC_WIDTH_CONTROL        = 0x1ACE; // ODM0（ODM 段宽）
constexpr UInt32 OPTC_MEMORY_CONFIG        = 0x1AD0; // ODM0（ODM 拼接内存掩码）

// ===== OPP（与 DCN2.1 逐字节一致，已审查确认）=====
constexpr UInt32 OPP_REG_STRIDE         = 0x5A;
constexpr UInt32 OPPBUF_3D_PARAMETERS_0 = 0x1884; // OPPBUF0
constexpr UInt32 OPP_PIPE_CONTROL       = 0x188C; // OPP_PIPE0
constexpr UInt32 OPP_TOP_CLK_CONTROL    = 0x1A5E; // OPP_TOP_CLK

// ===== MPCC/MPC（stride 0x20，偏移用 dcn314 头，已审查确认）=====
constexpr UInt32 MPCC_REG_STRIDE      = 0x20;
constexpr UInt32 MPCC_TOP_SEL         = 0x0000; // MPCC0
constexpr UInt32 MPCC_UPDATE_LOCK_SEL = 0x0005; // MPCC0（无 MPCC_UPDATE_CTRL，锁存走此）
constexpr UInt32 MPC_OUT_MUX_STRIDE   = 0x4;
constexpr UInt32 MPC_OUT0_MUX         = 0x0580; // MPC_OUT0（修正：原错写 0x0）

// ===== DCCG 时钟控制（不叫 DCCG_ 前缀，BASE_IDX=1，已审查确认）=====
constexpr UInt32 DISPCLK_FREQ_CHANGE_CNTL   = 0x0071;
constexpr UInt32 DTBCLK_P_CNTL              = 0x0068;
constexpr UInt32 SYMCLK32_SE_CNTL           = 0x0065;
constexpr UInt32 OTG_PIXEL_RATE_CNTL        = 0x0080; // OTG0
constexpr UInt32 DP_DTO_PHASE               = 0x0081; // DP_DTO0
constexpr UInt32 PHYPLLA_PIXCLK_RESYNC_CNTL = 0x0040; // PHYPLLA..E

// ===== DENTIST（resync_fifo 依赖，BASE_IDX 待核）=====
constexpr UInt32 DENTIST_DISPCLK_CNTL = 0x0064; // RDIVIDER/WDIVIDER 字段

// ===== DIG 编码器（5 实例 stride 0x100，已审查确认）=====
constexpr UInt32 DIG_REG_STRIDE = 0x100;
constexpr UInt32 DIG_FE_CNTL    = 0x208B; // DIG0（无 DIG_FE_CNTL2）
constexpr UInt32 DIG_AFMT_CNTL  = 0x20B0; // DIG0
constexpr UInt32 DIG_BE_CNTL    = 0x20B1; // DIG0
constexpr UInt32 DIG_BE_EN_CNTL = 0x20B2; // DIG0

// ===== sh_mask 字段（update_odm/resync_fifo 需要，主控层核实）=====
constexpr UInt32 OPTC_MEMORY_CONFIG_OPTC_MEM_SEL_SHIFT = 0x0;        // ODM0（ODM0_OPTC_MEMORY_CONFIG）
constexpr UInt32 OPTC_MEMORY_CONFIG_OPTC_MEM_SEL_MASK  = 0x0000FFFFL;
constexpr UInt32 OPTC_WIDTH_CONTROL_OPTC_SEGMENT_WIDTH_SHIFT = 0x0;  // ODM0
constexpr UInt32 DENTIST_DISPCLK_WDIVIDER_SHIFT = 0x0;              // DENTIST_DISPCLK_CNTL
constexpr UInt32 DENTIST_DISPCLK_WDIVIDER_MASK  = 0x0000007FL;
constexpr UInt32 DENTIST_DISPCLK_RDIVIDER_SHIFT = 0x8;
constexpr UInt32 DENTIST_DISPCLK_RDIVIDER_MASK  = 0x00007F00L;
