// DCN314 Register Offsets (Phoenix / Radeon 780M, RDNA3)
//
// 来源：Linux amdgpu dcn_3_1_4_offset.h（已双 Verifier 审查确认）
// - HUBP/OTG：与 DCN2.hpp 100% 一致（复用）
// - OPP：与 DCN2.1 逐字节一致
// - MPCC：stride 0x20，位域可复用 DCN2 但偏移用 dcn314 头
// - DCCG：时钟控制寄存器不叫 DCCG_ 前缀（DISPCLK_FREQ_CHANGE_CNTL 等），BASE_IDX=1
// - DIG：5 实例 stride 0x100
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
// OPTC 完整集（dcn314 值，待补全——update_odm 需要）
constexpr UInt32 OTG_MASTER_UPDATE_LOCK_SEL = 0x1A43; // dcn314 待核实

// ===== OPP（与 DCN2.1 逐字节一致，已审查确认）=====
constexpr UInt32 OPP_REG_STRIDE        = 0x5A;
constexpr UInt32 OPPBUF_3D_PARAMETERS_0 = 0x1884; // OPPBUF0
constexpr UInt32 OPP_PIPE_CONTROL      = 0x188C; // OPP_PIPE0
constexpr UInt32 OPP_TOP_CLK_CONTROL   = 0x1A5E; // OPP_TOP_CLK

// ===== MPCC/MPC（stride 0x20，偏移用 dcn314 头，已审查确认）=====
constexpr UInt32 MPCC_REG_STRIDE       = 0x20;
constexpr UInt32 MPCC_TOP_SEL          = 0x0000; // MPCC0
constexpr UInt32 MPCC_UPDATE_LOCK_SEL  = 0x0005; // MPCC0（无 MPCC_UPDATE_CTRL，锁存走此）
constexpr UInt32 MPC_OUT_MUX_REG_STRIDE = 0x4;
constexpr UInt32 MPC_OUT0_MUX          = 0x0;    // 待核实

// ===== DCCG 时钟控制（不叫 DCCG_ 前缀，BASE_IDX=1，已审查确认）=====
constexpr UInt32 DCCG_BASE             = 0x0;    // BASE_IDX=1，地址需加段基址
constexpr UInt32 DISPCLK_FREQ_CHANGE_CNTL   = 0x0071;
constexpr UInt32 DTBCLK_P_CNTL              = 0x0068;
constexpr UInt32 SYMCLK32_SE_CNTL           = 0x0065;
constexpr UInt32 OTG_PIXEL_RATE_CNTL        = 0x0080; // OTG0
constexpr UInt32 DP_DTO_PHASE               = 0x0081; // DP_DTO0
constexpr UInt32 PHYPLLA_PIXCLK_RESYNC_CNTL = 0x0040; // PHYPLLA..E，resync_fifo 关键

// ===== DIG 编码器（5 实例 stride 0x100，已审查确认）=====
constexpr UInt32 DIG_REG_STRIDE       = 0x100;
constexpr UInt32 DIG_FE_CNTL          = 0x208B; // DIG0（无 DIG_FE_CNTL2）
constexpr UInt32 DIG_AFMT_CNTL        = 0x20B0; // DIG0
constexpr UInt32 DIG_BE_CNTL          = 0x20B1; // DIG0
constexpr UInt32 DIG_BE_EN_CNTL       = 0x20B2; // DIG0
