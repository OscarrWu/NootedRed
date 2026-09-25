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
// - DSC：只登记核心 4 寄存器（后续步骤暂不依赖；实例步进 0x5C，非常规）
// - 中断：DCN 3.1.4 无 OTG0_INT_* 命名寄存器，按 irq_service_dcn314.c 于源块就地使能/应答
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once
#include <IOKit/IOTypes.h>

// ===== DCN 段基址（Linux `include/yellow_carp_offset.h:385-390` 的 `DCN_BASE__INST0_SEGn`）=====
// ⚠️ **同一个 IP 的寄存器分布在多个段里**：只记偏移而不带对段基址，会静默算到别的地址上。
//    本项目就吃过这个亏——`MPC_OUT0_MUX`（BASE_IDX=3）曾被写成 `DCN_BASE_2 + 0x580` = 0x3A40，
//    而真值序列（Linux 实测）里该地址**零事件**，正确的 0x9580 有 70 个读改写事件。
//    故凡使用本文件的寄存器偏移，**必须同时确认它的 BASE_IDX**（各常量处已逐条注明）。
constexpr UInt32 DCN_SEG0_BASE = 0x00000012;
constexpr UInt32 DCN_SEG1_BASE = 0x000000C0;
constexpr UInt32 DCN_SEG2_BASE = 0x000034C0;  // 与 `DCN_BASE_2` 同值（HUBP/OTG/OPTC/DP 流编码器在此段）
constexpr UInt32 DCN_SEG3_BASE = 0x00009000;  // MPC_OUTn_MUX 在此段
constexpr UInt32 DCN_SEG4_BASE = 0x02403C00;

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
constexpr UInt32 OPPBUF_3D_PARAMETERS_0 = 0x1885; // OPPBUF0（dcn_3_1_4_offset.h:7402，dcn_2_1_0:7311 同值；2026-09-24 审查纠正，旧值 0x1884 系交接笔误）
constexpr UInt32 OPP_PIPE_CONTROL       = 0x188C; // OPP_PIPE0
constexpr UInt32 OPP_TOP_CLK_CONTROL    = 0x1A5E; // OPP_TOP_CLK

// ===== MPCC/MPC（stride 0x20，偏移用 dcn314 头，已审查确认）=====
constexpr UInt32 MPCC_REG_STRIDE      = 0x20;
constexpr UInt32 MPCC_TOP_SEL         = 0x0000; // MPCC0
constexpr UInt32 MPCC_UPDATE_LOCK_SEL = 0x0005; // MPCC0（无 MPCC_UPDATE_CTRL，锁存走此）
constexpr UInt32 MPC_OUT_MUX_STRIDE   = 0x4;
constexpr UInt32 MPC_OUT0_MUX         = 0x0580; // MPC_OUT0（**BASE_IDX=3** → 绝对地址 = DCN_SEG3_BASE + 本偏移 = 0x9580）

// ===== DCCG 时钟控制（不叫 DCCG_ 前缀，BASE_IDX=1，已审查确认）=====
constexpr UInt32 DISPCLK_FREQ_CHANGE_CNTL   = 0x0071;
constexpr UInt32 DTBCLK_P_CNTL              = 0x0068;
constexpr UInt32 SYMCLK32_SE_CNTL           = 0x0065;
constexpr UInt32 OTG_PIXEL_RATE_CNTL        = 0x0080; // OTG0
constexpr UInt32 DP_DTO_PHASE               = 0x0081; // DP_DTO0
constexpr UInt32 PHYPLLA_PIXCLK_RESYNC_CNTL = 0x0040; // PHYPLLA..E

// ===== DCCG 门控/像素分频（时钟主流程：dccg314_*；BASE_IDX=1 除注明外）=====
constexpr UInt32 DCCG_GATE_DISABLE_CNTL     = 0x0074; // regDCCG_GATE_DISABLE_CNTL（dccg2_allow_clock_gating 全局门控）
constexpr UInt32 DCCG_GATE_DISABLE_CNTL2    = 0x007C; // regDCCG_GATE_DISABLE_CNTL2（SYMCLK/PHYxSYMCLK 根门控）
constexpr UInt32 DCCG_GATE_DISABLE_CNTL3    = 0x005A; // regDCCG_GATE_DISABLE_CNTL3（注意：BASE_IDX=2，不与 1/2 同段）
constexpr UInt32 DCCG_DISP_CNTL_REG         = 0x007F; // regDCCG_DISP_CNTL_REG（DCE 遗留单字段，登记备查）
constexpr UInt32 OTG_PIXEL_RATE_DIV         = 0x006F; // regOTG_PIXEL_RATE_DIV（dccg314_set/get_pixel_rate_div，enable_stream 必经）
constexpr UInt32 DPPCLK_DTO_CTRL            = 0x00B6; // regDPPCLK_DTO_CTRL（dccg314_dpp_root_clock_control/update_dpp_dto）
constexpr UInt32 DPPCLK0_DTO_PARAM          = 0x0099; // regDPPCLK0_DTO_PARAM（DPP0，实例步进 0x1）
constexpr UInt32 MICROSECOND_TIME_BASE_DIV  = 0x007B; // regMICROSECOND_TIME_BASE_DIV（REG_WAIT 微秒延时基准）
constexpr UInt32 DC_MEM_GLOBAL_PWR_REQ_CNTL = 0x0072; // regDC_MEM_GLOBAL_PWR_REQ_CNTL（dccg2_enable_memory_low_power）
constexpr UInt32 DENTIST_DISPCLK_CNTL = 0x0064; // RDIVIDER/WDIVIDER 字段（resync_fifo；BASE_IDX=1 已核）


// ===== DP 流编码器（STE 基础集，提交路径核心；BASE_IDX=2，stride 0x100）=====
constexpr UInt32 DP_REG_STRIDE        = 0x100; // DP1(0x2208) - DP0(0x2108)
constexpr UInt32 DP_LINK_CNTL         = 0x2108; // DP0（链路关闭：训练完成复位；eDP 面板模式）
constexpr UInt32 DP_PIXEL_FORMAT      = 0x2109; // DP0（像素编码/分量位深）
constexpr UInt32 DP_CONFIG            = 0x210B; // DP0（UDI lane 配置）
constexpr UInt32 DP_VID_STREAM_CNTL   = 0x210C; // DP0（流使能/状态，enable/disable stream REG_WAIT）
constexpr UInt32 DP_STEER_FIFO        = 0x210D; // DP0（steer FIFO 复位）
constexpr UInt32 DP_MSA_MISC          = 0x210E; // DP0（MSA MISC1..4）
constexpr UInt32 DP_VID_TIMING        = 0x2110; // DP0（M/N 生成控制）
constexpr UInt32 DP_VID_N             = 0x2111; // DP0
constexpr UInt32 DP_VID_M             = 0x2112; // DP0
constexpr UInt32 DP_SEC_CNTL          = 0x212B; // DP0（SEC 流/音频/GSP 使能）
constexpr UInt32 DP_SEC_CNTL1         = 0x212C; // DP0（GSP0 行号等）
constexpr UInt32 DP_SEC_AUD_N         = 0x2131; // DP0
constexpr UInt32 DP_SEC_TIMESTAMP     = 0x2135; // DP0
constexpr UInt32 DP_MSE_RATE_CNTL     = 0x2137; // DP0（MST 码率，预留）
constexpr UInt32 DP_MSE_RATE_UPDATE   = 0x2139; // DP0（MST 码率更新挂起，预留）
constexpr UInt32 DP_MSA_TIMING_PARAM1 = 0x214C; // DP0（VTOTAL/HTOTAL）
constexpr UInt32 DP_MSA_TIMING_PARAM2 = 0x214D; // DP0（VSTART/HSTART）
constexpr UInt32 DP_MSA_TIMING_PARAM3 = 0x214E; // DP0（VSYNC/HSYNC 宽与极性）
constexpr UInt32 DP_MSA_TIMING_PARAM4 = 0x214F; // DP0（VHEIGHT/HWIDTH）
constexpr UInt32 DP_DSC_CNTL          = 0x2152; // DP0（DSC 模式）
constexpr UInt32 DP_SEC_CNTL2         = 0x2153; // DP0
constexpr UInt32 DP_DB_CNTL           = 0x2159; // DP0（双重缓冲锁）
constexpr UInt32 DP_MSA_VBID_MISC     = 0x215A; // DP0（VBID 覆写）

// ===== DSC（后续步骤暂不依赖，仅登记核心；BASE_IDX=2）=====
// 注意：实例步进 0x5C（DSC_TOP1=0x305C、DSC_TOP3=0x3114），勿按 0x100 推算。
constexpr UInt32 DSC_REG_STRIDE  = 0x5C;   // DSC_TOP1(0x305C) - DSC_TOP0(0x3000)
constexpr UInt32 DSC_TOP_CONTROL = 0x3000; // DSC0（regDSC_TOP0_DSC_TOP_CONTROL：DSC_CLOCK_EN）
constexpr UInt32 DSCCIF_CONFIG0  = 0x3005; // DSC0（输入像素格式/位深）
constexpr UInt32 DSCCIF_CONFIG1  = 0x3006; // DSC0（PIC_WIDTH/PIC_HEIGHT）
constexpr UInt32 DSCC_CONFIG0    = 0x300A; // DSC0（切片数）

// ===== 中断（无 OTG0_INT_* 命名寄存器；按 irq_service_dcn314.c 于源块就地使能/应答）=====
constexpr UInt32 OTG_GLOBAL_SYNC_STATUS        = 0x1B89; // OTG0（BASE_IDX=2，stride 0x80；VBLANK/VUPDATE 使能与清除）
constexpr UInt32 DCSURF_SURFACE_FLIP_INTERRUPT = 0x0620; // HUBPREQ0（BASE_IDX=2，stride 0xDC；翻页完成）
constexpr UInt32 HPD_INT_STATUS                = 0x1F14; // HPD0（BASE_IDX=2，stride 8）
constexpr UInt32 HPD_INT_CONTROL               = 0x1F15; // HPD0
constexpr UInt32 HPD_CONTROL                   = 0x1F16; // HPD0

// ===== DIG 编码器（5 实例 stride 0x100，已审查确认）=====
constexpr UInt32 DIG_REG_STRIDE = 0x100;
constexpr UInt32 DIG_FE_CNTL    = 0x208B; // DIG0（无 DIG_FE_CNTL2）
constexpr UInt32 DIG_AFMT_CNTL  = 0x20B0; // DIG0
constexpr UInt32 DIG_BE_CNTL    = 0x20B1; // DIG0
constexpr UInt32 DIG_BE_EN_CNTL = 0x20B2; // DIG0
constexpr UInt32 DIG1_DIG_FE_CNTL = 0x218B; // DIG1（stride 0x100 核对：regDIG1_DIG_FE_CNTL）
constexpr UInt32 DIG4_DIG_FE_CNTL = 0x248B; // DIG4（末实例；DCN 3.1.4 无 DIG5）
constexpr UInt32 DIG_CLOCK_PATTERN = 0x208E; // DIG0（dcn314 流编码器 enable 路径整字段写 0x1F）
constexpr UInt32 DIG_FIFO_CTRL0    = 0x2091; // DIG0（dcn314 流编码器 DIG FIFO 重置/使能）

// ===== sh_mask 字段（update_odm/resync_fifo 需要，主控层核实）=====
// OPTC_DATA_SOURCE_SELECT（ODM0，BASE_IDX=2）
constexpr UInt32 OPTC_NUM_OF_INPUT_SEGMENT_SHIFT  = 0x0;
constexpr UInt32 OPTC_NUM_OF_INPUT_SEGMENT_MASK   = 0x00000003L;
constexpr UInt32 OPTC_NUM_OF_OUTPUT_SEGMENT_SHIFT = 0x8;
constexpr UInt32 OPTC_NUM_OF_OUTPUT_SEGMENT_MASK  = 0x00000300L;
constexpr UInt32 OPTC_SEG0_SRC_SEL_SHIFT          = 0x10;
constexpr UInt32 OPTC_SEG0_SRC_SEL_MASK           = 0x000F0000L;
constexpr UInt32 OPTC_SEG1_SRC_SEL_SHIFT          = 0x14;
constexpr UInt32 OPTC_SEG1_SRC_SEL_MASK           = 0x00F00000L;
constexpr UInt32 OPTC_SEG2_SRC_SEL_SHIFT          = 0x18;
constexpr UInt32 OPTC_SEG2_SRC_SEL_MASK           = 0x0F000000L;
constexpr UInt32 OPTC_SEG3_SRC_SEL_SHIFT          = 0x1C;
constexpr UInt32 OPTC_SEG3_SRC_SEL_MASK           = 0xF0000000L;
// OPTC_MEMORY_CONFIG（ODM0）
constexpr UInt32 OPTC_MEMORY_CONFIG_OPTC_MEM_SEL_SHIFT = 0x0;
constexpr UInt32 OPTC_MEMORY_CONFIG_OPTC_MEM_SEL_MASK  = 0x0000FFFFL;
constexpr UInt32 OPTC_MEMORY_CONFIG_OPTC_MEM_SEL_STATUS_SHIFT = 0x10;
constexpr UInt32 OPTC_MEMORY_CONFIG_OPTC_MEM_SEL_STATUS_MASK  = 0xFFFF0000L;
// OPTC_WIDTH_CONTROL（ODM0）
constexpr UInt32 OPTC_WIDTH_CONTROL_OPTC_SEGMENT_WIDTH_SHIFT = 0x0;
constexpr UInt32 OPTC_WIDTH_CONTROL_OPTC_DSC_SLICE_WIDTH_SHIFT = 0x10;
constexpr UInt32 OPTC_WIDTH_CONTROL_OPTC_DSC_SLICE_WIDTH_MASK = 0x1FFF0000L;
// OTG_H_TIMING_CNTL（OTG0）
constexpr UInt32 OTG_H_TIMING_DIV_MODE_SHIFT = 0x0;
constexpr UInt32 OTG_H_TIMING_DIV_MODE_MASK  = 0x00000003L;
constexpr UInt32 OTG_H_TIMING_DIV_MODE_MANUAL_SHIFT = 0x8;
constexpr UInt32 OTG_H_TIMING_DIV_MODE_MANUAL_MASK  = 0x00000100L;
constexpr UInt32 OTG_H_TIMING_DIV_MODE_CURR_SHIFT = 0x10;
constexpr UInt32 OTG_H_TIMING_DIV_MODE_CURR_MASK  = 0x00030000L;
// DENTIST（resync_fifo，BASE_IDX=1）
constexpr UInt32 DENTIST_DISPCLK_WDIVIDER_SHIFT = 0x0;
constexpr UInt32 DENTIST_DISPCLK_WDIVIDER_MASK  = 0x0000007FL;
constexpr UInt32 DENTIST_DISPCLK_RDIVIDER_SHIFT = 0x8;
constexpr UInt32 DENTIST_DISPCLK_RDIVIDER_MASK  = 0x00007F00L;

// ===== DENTIST 补充：变更模式（dccg31_set_dispclk_change_mode）=====
constexpr UInt32 DENTIST_DISPCLK_CHG_MODE_SHIFT = 0xF;
constexpr UInt32 DENTIST_DISPCLK_CHG_MODE_MASK  = 0x00018000L;

// ===== DCCG 像素分频（dccg314_set/get_pixel_rate_div，enable_stream 必经）=====
// OTG_PIXEL_RATE_DIV（BASE_IDX=1；OTG0..3 打包单寄存器）
constexpr UInt32 OTG0_PIXEL_RATE_DIVK1_SHIFT = 0x0;
constexpr UInt32 OTG0_PIXEL_RATE_DIVK1_MASK  = 0x00000001L;
constexpr UInt32 OTG0_PIXEL_RATE_DIVK2_SHIFT = 0x1;
constexpr UInt32 OTG0_PIXEL_RATE_DIVK2_MASK  = 0x00000006L;
// OTG_PIXEL_RATE_CNTL（OTG0，BASE_IDX=1，stride 0x4：像素率源/DTO 使能）
constexpr UInt32 OTG0_PIXEL_RATE_SOURCE_SHIFT = 0x0;
constexpr UInt32 OTG0_PIXEL_RATE_SOURCE_MASK  = 0x00000003L;
constexpr UInt32 DP_DTO0_ENABLE_SHIFT         = 0x4;
constexpr UInt32 DP_DTO0_ENABLE_MASK          = 0x00000010L;
constexpr UInt32 PIPE0_DTO_SRC_SEL_SHIFT      = 0xC;
constexpr UInt32 PIPE0_DTO_SRC_SEL_MASK       = 0x00003000L;
// DPPCLK_DTO_CTRL / DPPCLK0_DTO_PARAM（dccg314_dpp_root_clock_control）
constexpr UInt32 DPPCLK0_DTO_ENABLE_SHIFT = 0x0;
constexpr UInt32 DPPCLK0_DTO_ENABLE_MASK  = 0x00000001L;
constexpr UInt32 DPPCLK0_DTO_DB_EN_SHIFT  = 0x1;
constexpr UInt32 DPPCLK0_DTO_DB_EN_MASK   = 0x00000002L;
constexpr UInt32 DPPCLK0_DTO_PHASE_SHIFT  = 0x0;
constexpr UInt32 DPPCLK0_DTO_PHASE_MASK   = 0x000000FFL;
constexpr UInt32 DPPCLK0_DTO_MODULO_SHIFT = 0x10;
constexpr UInt32 DPPCLK0_DTO_MODULO_MASK  = 0x00FF0000L;
// DTBCLK_P_CNTL（dccg314_set_dtbclk_p_src：dtbclk_p 源选择/使能）
constexpr UInt32 DTBCLK_P0_SRC_SEL_SHIFT = 0x0;
constexpr UInt32 DTBCLK_P0_SRC_SEL_MASK  = 0x00000003L;
constexpr UInt32 DTBCLK_P0_EN_SHIFT      = 0x2;
constexpr UInt32 DTBCLK_P0_EN_MASK       = 0x00000004L;
// DCCG_GATE_DISABLE_CNTL2/3（根时钟门控：SYMCLK/PHYxSYMCLK）
constexpr UInt32 PHYASYMCLK_GATE_DISABLE_SHIFT = 0x18;
constexpr UInt32 PHYASYMCLK_GATE_DISABLE_MASK  = 0x01000000L;
constexpr UInt32 PHYESYMCLK_GATE_DISABLE_SHIFT = 0x1C;
constexpr UInt32 PHYESYMCLK_GATE_DISABLE_MASK  = 0x10000000L;
constexpr UInt32 SYMCLK32_SE0_GATE_DISABLE_SHIFT = 0x9;
constexpr UInt32 SYMCLK32_SE0_GATE_DISABLE_MASK  = 0x00000200L;
// DC_MEM_GLOBAL_PWR_REQ_CNTL / MICROSECOND_TIME_BASE_DIV
constexpr UInt32 DC_MEM_GLOBAL_PWR_REQ_DIS_SHIFT     = 0x0;
constexpr UInt32 DC_MEM_GLOBAL_PWR_REQ_DIS_MASK      = 0x00000001L;
constexpr UInt32 MICROSECOND_TIME_BASE_DIV_SHIFT     = 0x0;
constexpr UInt32 MICROSECOND_TIME_BASE_DIV_MASK      = 0x0000007FL;

// ===== DIG / 流编码器位域（提交路径：enable/disable stream）=====
// DIG_FE_CNTL（DIG0，BASE_IDX=2，stride 0x100：FE 源选择与 SYMCLK 使能）
constexpr UInt32 DIG_SOURCE_SELECT_SHIFT     = 0x0;
constexpr UInt32 DIG_SOURCE_SELECT_MASK      = 0x00000007L;
constexpr UInt32 DIG_INPUT_PIXEL_SELECT_SHIFT = 0x10;
constexpr UInt32 DIG_INPUT_PIXEL_SELECT_MASK  = 0x00030000L;
constexpr UInt32 DIG_SYMCLK_FE_ON_SHIFT      = 0x18;
constexpr UInt32 DIG_SYMCLK_FE_ON_MASK       = 0x01000000L;
// DIG_BE_CNTL（DIG0：BE 源/模式/HPD 选择；链路关闭路径写 DIG_HPD_SELECT）
constexpr UInt32 DIG_FE_SOURCE_SELECT_SHIFT = 0x8;
constexpr UInt32 DIG_FE_SOURCE_SELECT_MASK  = 0x00007F00L;
constexpr UInt32 DIG_MODE_SHIFT             = 0x10;
constexpr UInt32 DIG_MODE_MASK              = 0x00070000L;
constexpr UInt32 DIG_HPD_SELECT_SHIFT       = 0x1C;
constexpr UInt32 DIG_HPD_SELECT_MASK        = 0x70000000L;
// DIG_BE_EN_CNTL（DIG0：dcn10_is_dig_enabled 读 DIG_ENABLE）
constexpr UInt32 DIG_ENABLE_SHIFT      = 0x0;
constexpr UInt32 DIG_ENABLE_MASK       = 0x00000001L;
constexpr UInt32 DIG_SYMCLK_BE_ON_SHIFT = 0x8;
constexpr UInt32 DIG_SYMCLK_BE_ON_MASK  = 0x00000100L;
// DIG_CLOCK_PATTERN（DIG0：dcn314 流编码器写 0x1F）
constexpr UInt32 DIG_CLOCK_PATTERN_FIELD_SHIFT = 0x0;
constexpr UInt32 DIG_CLOCK_PATTERN_FIELD_MASK  = 0x000003FFL;
// DIG_FIFO_CTRL0（DIG0：dcn314 流编码器 DIG FIFO）
constexpr UInt32 DIG_FIFO_ENABLE_SHIFT         = 0x0;
constexpr UInt32 DIG_FIFO_ENABLE_MASK          = 0x00000001L;
constexpr UInt32 DIG_FIFO_RESET_SHIFT          = 0x1;
constexpr UInt32 DIG_FIFO_RESET_MASK           = 0x00000002L;
constexpr UInt32 DIG_FIFO_READ_START_LEVEL_SHIFT = 0x2;
constexpr UInt32 DIG_FIFO_READ_START_LEVEL_MASK  = 0x0000007CL;
constexpr UInt32 DIG_FIFO_RESET_DONE_SHIFT     = 0x14;
constexpr UInt32 DIG_FIFO_RESET_DONE_MASK      = 0x00100000L;
// AFMT_CNTL（DIG0：音频时钟，既有寄存器位域）
constexpr UInt32 AFMT_AUDIO_CLOCK_EN_SHIFT = 0x0;
constexpr UInt32 AFMT_AUDIO_CLOCK_EN_MASK  = 0x00000001L;
// DP_PIXEL_FORMAT（DP0，BASE_IDX=2，stride 0x100）
constexpr UInt32 DP_PIXEL_ENCODING_SHIFT = 0x0;
constexpr UInt32 DP_PIXEL_ENCODING_MASK  = 0x00000007L;
constexpr UInt32 DP_COMPONENT_DEPTH_SHIFT = 0x18;
constexpr UInt32 DP_COMPONENT_DEPTH_MASK  = 0x07000000L;
// DP_CONFIG（DP0）
constexpr UInt32 DP_UDI_LANES_SHIFT = 0x0;
constexpr UInt32 DP_UDI_LANES_MASK  = 0x00000003L;
// DP_SEC_CNTL1（DP0：GSP0 行号）
constexpr UInt32 DP_SEC_GSP0_LINE_NUM_SHIFT = 0x10;
constexpr UInt32 DP_SEC_GSP0_LINE_NUM_MASK  = 0xFFFF0000L;
// DP_VID_STREAM_CNTL（DP0：流使能/状态，REG_WAIT 依赖）
constexpr UInt32 DP_VID_STREAM_ENABLE_SHIFT    = 0x0;
constexpr UInt32 DP_VID_STREAM_ENABLE_MASK     = 0x00000001L;
constexpr UInt32 DP_VID_STREAM_DIS_DEFER_SHIFT = 0x8;
constexpr UInt32 DP_VID_STREAM_DIS_DEFER_MASK  = 0x00000300L;
constexpr UInt32 DP_VID_STREAM_STATUS_SHIFT    = 0x10;
constexpr UInt32 DP_VID_STREAM_STATUS_MASK     = 0x00010000L;
// DP_STEER_FIFO（DP0）
constexpr UInt32 DP_STEER_FIFO_RESET_SHIFT = 0x0;
constexpr UInt32 DP_STEER_FIFO_RESET_MASK  = 0x00000001L;
// DP_VID_TIMING / DP_VID_M / DP_VID_N
constexpr UInt32 DP_VID_M_N_DOUBLE_BUFFER_MODE_SHIFT = 0x4;
constexpr UInt32 DP_VID_M_N_DOUBLE_BUFFER_MODE_MASK  = 0x00000010L;
constexpr UInt32 DP_VID_M_N_GEN_EN_SHIFT = 0x8;
constexpr UInt32 DP_VID_M_N_GEN_EN_MASK  = 0x00000100L;
constexpr UInt32 DP_VID_M_FIELD_SHIFT = 0x0;
constexpr UInt32 DP_VID_M_FIELD_MASK  = 0x00FFFFFFL;
constexpr UInt32 DP_VID_N_FIELD_SHIFT = 0x0;
constexpr UInt32 DP_VID_N_FIELD_MASK  = 0x00FFFFFFL;
// DP_MSA_TIMING_PARAM1..4（MSA 时序四件套）
constexpr UInt32 DP_MSA_VTOTAL_SHIFT = 0x0;
constexpr UInt32 DP_MSA_VTOTAL_MASK  = 0x0000FFFFL;
constexpr UInt32 DP_MSA_HTOTAL_SHIFT = 0x10;
constexpr UInt32 DP_MSA_HTOTAL_MASK  = 0xFFFF0000L;
constexpr UInt32 DP_MSA_VSTART_SHIFT = 0x0;
constexpr UInt32 DP_MSA_VSTART_MASK  = 0x0000FFFFL;
constexpr UInt32 DP_MSA_HSTART_SHIFT = 0x10;
constexpr UInt32 DP_MSA_HSTART_MASK  = 0xFFFF0000L;
constexpr UInt32 DP_MSA_VSYNCWIDTH_SHIFT = 0x0;
constexpr UInt32 DP_MSA_VSYNCWIDTH_MASK  = 0x00007FFFL;
constexpr UInt32 DP_MSA_HSYNCWIDTH_SHIFT = 0x10;
constexpr UInt32 DP_MSA_HSYNCWIDTH_MASK  = 0x7FFF0000L;
constexpr UInt32 DP_MSA_VHEIGHT_SHIFT = 0x0;
constexpr UInt32 DP_MSA_VHEIGHT_MASK  = 0x0000FFFFL;
constexpr UInt32 DP_MSA_HWIDTH_SHIFT  = 0x10;
constexpr UInt32 DP_MSA_HWIDTH_MASK   = 0xFFFF0000L;
// DP_MSA_MISC / DP_MSA_VBID_MISC
constexpr UInt32 DP_MSA_MISC1_SHIFT = 0x0;
constexpr UInt32 DP_MSA_MISC1_MASK  = 0x000000FFL;
// DP_SEC_CNTL / DP_SEC_TIMESTAMP（SEC 流与时间戳模式）
constexpr UInt32 DP_SEC_STREAM_ENABLE_SHIFT = 0x0;
constexpr UInt32 DP_SEC_STREAM_ENABLE_MASK  = 0x00000001L;
constexpr UInt32 DP_SEC_MPG_ENABLE_SHIFT    = 0x1C;
constexpr UInt32 DP_SEC_MPG_ENABLE_MASK     = 0x10000000L;
constexpr UInt32 DP_SEC_TIMESTAMP_MODE_SHIFT = 0x0;
constexpr UInt32 DP_SEC_TIMESTAMP_MODE_MASK  = 0x00000001L;
// DP_MSE_RATE_CNTL / DP_MSE_RATE_UPDATE（MST 码率，预留）
constexpr UInt32 DP_MSE_RATE_Y_SHIFT = 0x0;
constexpr UInt32 DP_MSE_RATE_Y_MASK  = 0x03FFFFFFL;
constexpr UInt32 DP_MSE_RATE_X_SHIFT = 0x1A;
constexpr UInt32 DP_MSE_RATE_X_MASK  = 0xFC000000L;
constexpr UInt32 DP_MSE_RATE_UPDATE_PENDING_SHIFT = 0x0;
constexpr UInt32 DP_MSE_RATE_UPDATE_PENDING_MASK  = 0x00000001L;
// DP_LINK_CNTL（链路关闭路径：dcn31 link_encoder_disable 复位训练完成位）
constexpr UInt32 DP_LINK_TRAINING_COMPLETE_SHIFT = 0x4;
constexpr UInt32 DP_LINK_TRAINING_COMPLETE_MASK  = 0x00000010L;
constexpr UInt32 DP_EMBEDDED_PANEL_MODE_SHIFT    = 0x11;
constexpr UInt32 DP_EMBEDDED_PANEL_MODE_MASK     = 0x00020000L;
// DP_DB_CNTL（双重缓冲）
constexpr UInt32 DP_DB_LOCK_SHIFT    = 0x8;
constexpr UInt32 DP_DB_LOCK_MASK     = 0x00000100L;
constexpr UInt32 DP_DB_DISABLE_SHIFT = 0xC;
constexpr UInt32 DP_DB_DISABLE_MASK  = 0x00001000L;
// DP_DSC_CNTL（DP0：DSC 模式）
constexpr UInt32 DP_DSC_MODE_SHIFT = 0x0;
constexpr UInt32 DP_DSC_MODE_MASK  = 0x00000001L;

// ===== DSC 位域（核心，后续步骤暂不依赖）=====
// DSC_TOP_CONTROL（DSC0，BASE_IDX=2，stride 0x5C）
constexpr UInt32 DSC_CLOCK_EN_SHIFT = 0x0;
constexpr UInt32 DSC_CLOCK_EN_MASK  = 0x00000001L;

// ===== 中断位域（irq_service_dcn314.c 使能/应答掩码）=====
// OTG_GLOBAL_SYNC_STATUS（OTG0，BASE_IDX=2，stride 0x80：VBLANK=VSTARTUP、VUPDATE_NO_LOCK）
constexpr UInt32 OTG_VSTARTUP_INT_EN_SHIFT           = 0x0;
constexpr UInt32 OTG_VSTARTUP_INT_EN_MASK            = 0x00000001L;
constexpr UInt32 OTG_VSTARTUP_EVENT_CLEAR_SHIFT      = 0x4;
constexpr UInt32 OTG_VSTARTUP_EVENT_CLEAR_MASK       = 0x00000010L;
constexpr UInt32 OTG_VUPDATE_NO_LOCK_INT_EN_SHIFT    = 0xC;
constexpr UInt32 OTG_VUPDATE_NO_LOCK_INT_EN_MASK     = 0x00001000L;
constexpr UInt32 OTG_VUPDATE_NO_LOCK_EVENT_CLEAR_SHIFT = 0x10;
constexpr UInt32 OTG_VUPDATE_NO_LOCK_EVENT_CLEAR_MASK  = 0x00010000L;
// DCSURF_SURFACE_FLIP_INTERRUPT（HUBPREQ0，BASE_IDX=2，stride 0xDC：翻页）
constexpr UInt32 SURFACE_FLIP_INT_MASK_SHIFT = 0x0;
constexpr UInt32 SURFACE_FLIP_INT_MASK_MASK  = 0x00000001L;
constexpr UInt32 SURFACE_FLIP_CLEAR_SHIFT    = 0x8;
constexpr UInt32 SURFACE_FLIP_CLEAR_MASK     = 0x00000100L;
// HPD_INT_CONTROL / HPD_INT_STATUS（HPD0，BASE_IDX=2，stride 8）
constexpr UInt32 HPD_INT_EN_SHIFT   = 0x10;
constexpr UInt32 HPD_INT_EN_MASK    = 0x00010000L;
constexpr UInt32 HPD_INT_ACK_SHIFT  = 0x0;
constexpr UInt32 HPD_INT_ACK_MASK   = 0x00000001L;
constexpr UInt32 HPD_INT_STATUS_FIELD_SHIFT = 0x0;
constexpr UInt32 HPD_INT_STATUS_FIELD_MASK  = 0x00000001L;
// HPD_CONTROL（HPD0：HPD 使能）
constexpr UInt32 DC_HPD_EN_SHIFT = 0x1C;
constexpr UInt32 DC_HPD_EN_MASK  = 0x10000000L;
