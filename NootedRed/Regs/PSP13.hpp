// PSP 13.0.4 Register & Constant Definitions
//
// 本文件提供 Phoenix (780M / gfx1103) PSP 13.0.4 所需的寄存器偏移常量与命令 ID，
// 供 fw 命名空间的乙线自建固件层内核态代码（T5）使用。
//
// 依据（两条路线，先 check offset，再找 sh_mask / 驱动源码）：
//   - mp_13_0_4_offset.h   -- 寄存器偏移（BASE_IDX=1，偏移=dword 地址）
//   - mp_13_0_4_sh_mask.h  -- 位域定义（CONTENT 字段全 32 位）
//   - psp_gfx_if.h         -- GFX_CTRL_CMD_ID_* / GFX_CMD_ID_* / GFX_FW_TYPE_* / 标志位掩码
//   - amdgpu_psp.h         -- PSP_BL__* / PSP_RING_TYPE_* / PSP_WAITREG_* / MBOX_TOS_* / MBOX_TOS_*_MASK
//   - psp_v13_0_4.c        -- Phoenix 变体实现（ring / bootloader / is_sos_alive）
//
// 风格对齐 src/NootedRed/Regs/SMU.hpp：
//   裸 constexpr UInt32，值 = dword 偏移（与 Linux offset.h 的 reg* 宏值一致）。
//   每个常量后注释标注来源文件:行号。
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once
#include <IOKit/IOTypes.h>

// =============================================================================
// 1. MP0 SMN C2PMSG 寄存器偏移（dword 地址，加 MP0_BASE_0 得物理地址）
//    source: mp_13_0_4_offset.h (addressBlock: mp_SmuMp0_SmnDec, BASE_IDX=1)
// =============================================================================

constexpr UInt32 MP0_SMN_C2PMSG_35   = 0x0063;  // mp_13_0_4_offset.h:37  — bootloader 命令/状态
constexpr UInt32 MP0_SMN_C2PMSG_36   = 0x0064;  // mp_13_0_4_offset.h:39  — bootloader 固件地址（>>20）
constexpr UInt32 MP0_SMN_C2PMSG_64   = 0x0080;  // mp_13_0_4_offset.h:95  — ring 命令/响应（非 SRIOV）
constexpr UInt32 MP0_SMN_C2PMSG_67   = 0x0083;  // mp_13_0_4_offset.h:101 — ring 写指针 wptr（非 SRIOV）
constexpr UInt32 MP0_SMN_C2PMSG_69   = 0x0085;  // mp_13_0_4_offset.h:105 — ring 地址低 32 位（非 SRIOV）
constexpr UInt32 MP0_SMN_C2PMSG_70   = 0x0086;  // mp_13_0_4_offset.h:107 — ring 地址高 32 位（非 SRIOV）
constexpr UInt32 MP0_SMN_C2PMSG_71   = 0x0087;  // mp_13_0_4_offset.h:109 — ring 大小（非 SRIOV）
constexpr UInt32 MP0_SMN_C2PMSG_81   = 0x0091;  // mp_13_0_4_offset.h:129 — SOS 存活检测（读 != 0）
// ⚠️ `MP0_SMN_C2PMSG_100`（0xA4）**不在此声明**——它已由 `Regs/SMU.hpp` 声明，重复声明会导致
//    同时包含两个头文件的编译单元报重定义。另：按其实际用途它是**显示时钟消息通道的 ARG0**
//    （`display/dc/clk_mgr/dcn60/dcn60_clk_mgr_smu_msg.c:15`：MSG=C2PMSG_98、RESP=C2PMSG_99、
//    ARG0..3=C2PMSG_100..103），**不是** PSP bootloader 版本，勿误用。
// ⚠️ SOS 固件版本寄存器 `MP0_SMN_C2PMSG_58`（0x7A，`psp_v13_0.c:336` `psp->sos.fw_version`）
//    同样已由 `Regs/SMU.hpp` 声明，此处不重复。
constexpr UInt32 MP0_SMN_C2PMSG_33   = 0x0061;  // mp_13_0_4_offset.h:33  — PSP ready（bit31，psp_v13_0.c:188-190）
constexpr UInt32 MP0_SMN_C2PMSG_101  = 0x00A5;  // mp_13_0_4_offset.h:169 — ring 命令（SRIOV 路径）
constexpr UInt32 MP0_SMN_C2PMSG_102  = 0x00A6;  // mp_13_0_4_offset.h:171 — ring 地址低/wptr（SRIOV）
constexpr UInt32 MP0_SMN_C2PMSG_103  = 0x00A7;  // mp_13_0_4_offset.h:173 — ring 地址高（SRIOV）

// =============================================================================
// 2. GFX_CTRL_CMD_ID_* — 寄存器接口命令（写 C2PMSG_64 或 C2PMSG_101）
//    source: psp_gfx_if.h:40-54
// =============================================================================

constexpr UInt32 GFX_CTRL_CMD_ID_INIT_RBI_RING      = 0x00010000;  // psp_gfx_if.h:42
constexpr UInt32 GFX_CTRL_CMD_ID_INIT_GPCOM_RING    = 0x00020000;  // psp_gfx_if.h:43
constexpr UInt32 GFX_CTRL_CMD_ID_DESTROY_RINGS      = 0x00030000;  // psp_gfx_if.h:44
constexpr UInt32 GFX_CTRL_CMD_ID_CAN_INIT_RINGS     = 0x00040000;  // psp_gfx_if.h:45
constexpr UInt32 GFX_CTRL_CMD_ID_ENABLE_INT         = 0x00050000;  // psp_gfx_if.h:46
constexpr UInt32 GFX_CTRL_CMD_ID_DISABLE_INT        = 0x00060000;  // psp_gfx_if.h:47
constexpr UInt32 GFX_CTRL_CMD_ID_MODE1_RST          = 0x00070000;  // psp_gfx_if.h:48
constexpr UInt32 GFX_CTRL_CMD_ID_GBR_IH_SET         = 0x00080000;  // psp_gfx_if.h:49
constexpr UInt32 GFX_CTRL_CMD_ID_CONSUME_CMD        = 0x00090000;  // psp_gfx_if.h:50
constexpr UInt32 GFX_CTRL_CMD_ID_DESTROY_GPCOM_RING = 0x000C0000;  // psp_gfx_if.h:51

// =============================================================================
// 3. GFX_CMD_ID_* — 环缓冲接口命令（通过 GPCOM ring 发送）
//    source: psp_gfx_if.h:87-114
// =============================================================================

constexpr UInt32 GFX_CMD_ID_LOAD_TA                  = 0x00000001;  // psp_gfx_if.h:89
constexpr UInt32 GFX_CMD_ID_UNLOAD_TA                = 0x00000002;  // psp_gfx_if.h:90
constexpr UInt32 GFX_CMD_ID_INVOKE_CMD               = 0x00000003;  // psp_gfx_if.h:91
constexpr UInt32 GFX_CMD_ID_LOAD_ASD                 = 0x00000004;  // psp_gfx_if.h:92
constexpr UInt32 GFX_CMD_ID_SETUP_TMR                = 0x00000005;  // psp_gfx_if.h:93
constexpr UInt32 GFX_CMD_ID_LOAD_IP_FW               = 0x00000006;  // psp_gfx_if.h:94
constexpr UInt32 GFX_CMD_ID_DESTROY_TMR              = 0x00000007;  // psp_gfx_if.h:95
constexpr UInt32 GFX_CMD_ID_SAVE_RESTORE             = 0x00000008;  // psp_gfx_if.h:96
constexpr UInt32 GFX_CMD_ID_SETUP_VMR                = 0x00000009;  // psp_gfx_if.h:97
constexpr UInt32 GFX_CMD_ID_DESTROY_VMR              = 0x0000000A;  // psp_gfx_if.h:98
constexpr UInt32 GFX_CMD_ID_PROG_REG                 = 0x0000000B;  // psp_gfx_if.h:99
constexpr UInt32 GFX_CMD_ID_GET_FW_ATTESTATION      = 0x0000000F;  // psp_gfx_if.h:100
constexpr UInt32 GFX_CMD_ID_LOAD_TOC                 = 0x00000020;  // psp_gfx_if.h:102 — 装载 TOC 并获取 TMR size
constexpr UInt32 GFX_CMD_ID_AUTOLOAD_RLC            = 0x00000021;  // psp_gfx_if.h:103 — 通知 PSP 启动 RLC autoload
constexpr UInt32 GFX_CMD_ID_BOOT_CFG                = 0x00000022;  // psp_gfx_if.h:104
constexpr UInt32 GFX_CMD_ID_SRIOV_SPATIAL_PART      = 0x00000027;  // psp_gfx_if.h:105
constexpr UInt32 GFX_CMD_ID_CONFIG_SQ_PERFMON        = 0x00000046;  // psp_gfx_if.h:107
constexpr UInt32 GFX_CMD_ID_FB_NPS_MODE             = 0x00000048;  // psp_gfx_if.h:109
constexpr UInt32 GFX_CMD_ID_PERF_HW                 = 0x0000004C;  // psp_gfx_if.h:110
constexpr UInt32 GFX_CMD_ID_FB_FW_RESERV_ADDR       = 0x00000050;  // psp_gfx_if.h:111
constexpr UInt32 GFX_CMD_ID_FB_FW_RESERV_EXT_ADDR   = 0x00000051;  // psp_gfx_if.h:112
constexpr UInt32 GFX_CMD_ID_SET_MMHUB_ECO_SEC_LEVEL  = 0x0000005D;  // psp_gfx_if.h:113

// =============================================================================
// 4. PSP_BL__* — Bootloader 命令（写 C2PMSG_35）
//    source: amdgpu_psp.h:95-108 (enum psp_bootloader_cmd)
// =============================================================================

constexpr UInt32 PSP_BL__LOAD_SYSDRV        = 0x10000;       // amdgpu_psp.h:96
constexpr UInt32 PSP_BL__LOAD_SOSDRV        = 0x20000;       // amdgpu_psp.h:97
constexpr UInt32 PSP_BL__LOAD_KEY_DATABASE  = 0x80000;       // amdgpu_psp.h:98
constexpr UInt32 PSP_BL__LOAD_SOCDRV        = 0xB0000;       // amdgpu_psp.h:99
constexpr UInt32 PSP_BL__LOAD_DBGDRV        = 0xC0000;       // amdgpu_psp.h:100
constexpr UInt32 PSP_BL__LOAD_HADDRV        = PSP_BL__LOAD_DBGDRV;  // amdgpu_psp.h:101, alias
constexpr UInt32 PSP_BL__LOAD_INTFDRV       = 0xD0000;       // amdgpu_psp.h:102
constexpr UInt32 PSP_BL__LOAD_RASDRV        = 0xE0000;       // amdgpu_psp.h:103
constexpr UInt32 PSP_BL__LOAD_IPKEYMGRDRV   = 0xF0000;       // amdgpu_psp.h:104
constexpr UInt32 PSP_BL__DRAM_LONG_TRAIN    = 0x100000;      // amdgpu_psp.h:105
constexpr UInt32 PSP_BL__DRAM_SHORT_TRAIN   = 0x200000;      // amdgpu_psp.h:106
constexpr UInt32 PSP_BL__LOAD_TOS_SPL_TABLE = 0x10000000;    // amdgpu_psp.h:107
constexpr UInt32 PSP_BL__LOAD_SPDMDRV       = 0x20000000;    // amdgpu_psp.h:108

// =============================================================================
// 5. PSP GFX 接口标志与掩码
//    source: psp_gfx_if.h:29-32,84; amdgpu_psp.h:57-65
// =============================================================================

// 命令寄存器通用掩码
constexpr UInt32 GFX_CMD_STATUS_MASK     = 0x0000FFFF;  // psp_gfx_if.h:29  — 状态码（低 16 位）
constexpr UInt32 GFX_CMD_ID_MASK         = 0x000F0000;  // psp_gfx_if.h:30  — 命令 ID（bit 16-19）
constexpr UInt32 GFX_CMD_RESERVED_MASK   = 0x7FF00000;  // psp_gfx_if.h:31  — 保留位（bit 20-30）
constexpr UInt32 GFX_CMD_RESPONSE_MASK   = 0x80000000;  // psp_gfx_if.h:32  — 响应标志（bit 31）
constexpr UInt32 GFX_FLAG_RESPONSE       = 0x80000000;  // psp_gfx_if.h:84  — 命令完成标志

// MBOX_TOS 等待标志与掩码
constexpr UInt32 MBOX_TOS_READY_FLAG     = GFX_FLAG_RESPONSE;          // amdgpu_psp.h:57
constexpr UInt32 MBOX_TOS_READY_MASK     = 0x8000FFFF;                 // amdgpu_psp.h:58  (= RESPONSE_MASK | STATUS_MASK)
constexpr UInt32 MBOX_TOS_RESP_FLAG      = GFX_FLAG_RESPONSE;          // amdgpu_psp.h:64
constexpr UInt32 MBOX_TOS_RESP_MASK      = 0x8000FFFF;                 // amdgpu_psp.h:65  (= RESPONSE_MASK | STATUS_MASK)

// USBC PD FW version retrieval 专用命令
constexpr UInt32 C2PMSG_CMD_GFX_USB_PD_FW_VER = 0x2000000;            // psp_gfx_if.h:35

// =============================================================================
// 6. PSP 等待寄存器标志
//    source: amdgpu_psp.h:139-140
// =============================================================================

constexpr UInt32 PSP_WAITREG_CHANGED   = 0x1;   // amdgpu_psp.h:139 — 等待值变化
constexpr UInt32 PSP_WAITREG_NOVERBOSE = 0x2;   // amdgpu_psp.h:140 — 等待超时不报错

// =============================================================================
// 7. PSP Ring 类型
//    source: amdgpu_psp.h:111-118 (enum psp_ring_type)
// =============================================================================

constexpr UInt32 PSP_RING_TYPE__INVALID = 0;  // amdgpu_psp.h:112
constexpr UInt32 PSP_RING_TYPE__UM     = 1;  // amdgpu_psp.h:117 — 用户态环（原 RBI）
constexpr UInt32 PSP_RING_TYPE__KM     = 2;  // amdgpu_psp.h:118 — 内核态环（原 GPCOM）

// =============================================================================
// 8. GFX_FW_TYPE_* — LOAD_IP_FW 的固件类型标识
//    source: psp_gfx_if.h:210-311 (enum psp_gfx_fw_type)
//    仅列出 Phoenix (gfx1103 / SOC21) 相关的类型
// =============================================================================

constexpr UInt32 GFX_FW_TYPE_NONE        = 0;   // psp_gfx_if.h:211
constexpr UInt32 GFX_FW_TYPE_TOC         = 24;  // psp_gfx_if.h:235
constexpr UInt32 GFX_FW_TYPE_RLC_P       = 25;  // psp_gfx_if.h:236
constexpr UInt32 GFX_FW_TYPE_RLC_IRAM    = 26;  // psp_gfx_if.h:237
constexpr UInt32 GFX_FW_TYPE_IMU_I       = 68;  // psp_gfx_if.h:274 — IMU Instruction FW (SOC21)
constexpr UInt32 GFX_FW_TYPE_IMU_D       = 69;  // psp_gfx_if.h:275 — IMU Data FW (SOC21)
constexpr UInt32 GFX_FW_TYPE_LSDMA       = 70;  // psp_gfx_if.h:276 — LSDMA FW (SOC21)
constexpr UInt32 GFX_FW_TYPE_DMUB        = 51;  // psp_gfx_if.h:262 — DMUB FW
constexpr UInt32 GFX_FW_TYPE_TA          = 75;  // psp_gfx_if.h:281 — TA FW UUID (SOC21)
constexpr UInt32 GFX_FW_TYPE_CP_MES      = 33;  // psp_gfx_if.h:244
constexpr UInt32 GFX_FW_TYPE_RS64_MES    = 76;  // psp_gfx_if.h:282
constexpr UInt32 GFX_FW_TYPE_PPTABLE     = 73;  // psp_gfx_if.h:279
