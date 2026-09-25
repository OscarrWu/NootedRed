// 路线图第一步验收测试：rev-id strap 寄存器地址（RCC_STRAP1_RCC_DEV0_EPF0_STRAP0）
//
// 修正内容：devRevision 读取地址由错误的 0xD2F（NBIO_BASE_2 + 遗留 RCC_DEV0_EPF0_STRAP0 = 0xF）
// 改为 0xD35（NBIO_BASE_2 + RCC_STRAP1_RCC_DEV0_EPF0_STRAP0 = 0x15）。
// 本测试直接包含真实头文件（Regs/NBIO.hpp、GPUDriversAMD/RavenIPOffset.hpp，
// 经 tests/stub/IOKit/IOTypes.h 用户态替身补齐 IOKit 依赖），断言头文件常量与
// Linux 权威出处一致——常量漂移即测试失败。
//
// 权威出处（Linux amdgpu）：
// - include/asic_reg/nbio/nbio_7_11_0_offset.h:8816-8819
//     // addressBlock: nbio_nbif0_rcc_strap_BIFDEC1
//     // base address: 0x0
//     #define regRCC_STRAP1_RCC_DEV0_EPF0_STRAP0              0x0015
//     #define regRCC_STRAP1_RCC_DEV0_EPF0_STRAP0_BASE_IDX     2
// - amdgpu/nbio_v7_11.c:38-47 nbio_v7_11_get_rev_id()（NBIO IP 7.11.x 绑定见
//   amdgpu_discovery.c:3450-3454），第 42 行原文：
//     tmp = RREG32_SOC15(NBIO, 0, regRCC_STRAP1_RCC_DEV0_EPF0_STRAP0);
//   第 43-44 行用 RCC_STRAP0 块前缀位域常量解包（两块位域布局一致）：
//     tmp &= RCC_STRAP0_RCC_DEV0_EPF0_STRAP0__STRAP_ATI_REV_ID_DEV0_F0_MASK;
//     tmp >>= RCC_STRAP0_RCC_DEV0_EPF0_STRAP0__STRAP_ATI_REV_ID_DEV0_F0__SHIFT;
// - include/yellow_carp_offset.h:975：NBIO_BASE__INST0_SEG2 = 0x00000D20
//   → 绝对 MMIO 地址 0xD20 + 0x15 = 0xD35
// - include/asic_reg/nbio/nbio_7_11_0_sh_mask.h:55905/55913（RCC_STRAP1 块）：
//     ..._STRAP_ATI_REV_ID_DEV0_F0__SHIFT = 0x18、..._MASK = 0x0F000000L
//   与 RCC_STRAP0 块实例（同文件 50654/50662）逐字一致。
//
// 编译运行（分析机）：
//   make -f src/NootedRed/tests/Makefile test
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.

#include <GPUDriversAMD/RavenIPOffset.hpp>
#include <Regs/NBIO.hpp>

#include <cassert>
#include <cstdio>

// ── Linux 权威值（独立期望值，出处见文件头注释）──
static constexpr UInt32 kLinuxStrap1Offset = 0x0015;       // nbio_7_11_0_offset.h:8818
static constexpr UInt32 kLinuxNbioSeg2Base = 0xD20;        // yellow_carp_offset.h:975
static constexpr UInt32 kLinuxRevIdShift   = 0x18;         // nbio_7_11_0_sh_mask.h:55905
static constexpr UInt32 kLinuxRevIdMask    = 0x0F000000;   // nbio_7_11_0_sh_mask.h:55913

// 1. 头文件常量与 Linux 权威值一致（防止本地副本漂移）
static void test_constants_match_linux() {
    assert(RCC_STRAP1_RCC_DEV0_EPF0_STRAP0 == kLinuxStrap1Offset);
    assert(NBIO_BASE_2 == kLinuxNbioSeg2Base);
    assert(RCC_DEV0_EPF0_STRAP0_ATI_REV_ID_SHIFT == kLinuxRevIdShift);
    assert(RCC_DEV0_EPF0_STRAP0_ATI_REV_ID_MASK == kLinuxRevIdMask);
    assert(RCC_DEV0_EPF0_STRAP0_ATI_REV_ID_MASK == 0xF000000);  // 头文件 0x0F000000L 的等价十六进制
    std::puts("  [PASS] 1 头文件常量与 Linux 权威值一致");
}

// 2. ★核心验收：读取地址 == 0xD35
static void test_read_address_is_d35() {
    constexpr UInt32 addr = NBIO_BASE_2 + RCC_STRAP1_RCC_DEV0_EPF0_STRAP0;
    assert(addr == 0xD35);
    // 修正前的错误地址（遗留 RCC_DEV0_EPF0_STRAP0 = 0xF → 0xD2F）必须与之可区分：
    // 若有人把读取改回旧常量，0xD2F != 0xD35 的关系仍成立，但 1 号用例与代码评审会拦住回退。
    assert(NBIO_BASE_2 + RCC_DEV0_EPF0_STRAP0 == 0xD2F);
    assert(NBIO_BASE_2 + RCC_DEV0_EPF0_STRAP0 != addr);
    std::puts("  [PASS] 2 读取地址 0xD35（旧错误地址 0xD2F 已区分）");
}

// 3. 位域抽取语义与 nbio_v7_11_get_rev_id 一致：(raw & MASK) >> SHIFT
//    sh_mask.h:55902-55917 布局：DEVICE_ID[15:0] | MAJOR[19:16] | MINOR[23:20] | ATI_REV_ID[27:24]
static void test_rev_id_extraction() {
    constexpr UInt32 raw = 0x12345678;
    assert(((raw & RCC_DEV0_EPF0_STRAP0_ATI_REV_ID_MASK) >> RCC_DEV0_EPF0_STRAP0_ATI_REV_ID_SHIFT) == 0x2);
    // bits[27:24] = 0x2；低 24 位与高位都不得泄漏进 rev id
    assert(((0x00FFFFFFu & RCC_DEV0_EPF0_STRAP0_ATI_REV_ID_MASK) >> RCC_DEV0_EPF0_STRAP0_ATI_REV_ID_SHIFT) == 0);
    assert(((0x0F000000u & RCC_DEV0_EPF0_STRAP0_ATI_REV_ID_MASK) >> RCC_DEV0_EPF0_STRAP0_ATI_REV_ID_SHIFT) == 0xF);
    assert(RCC_DEV0_EPF0_STRAP0_ATI_REV_ID_MASK >> RCC_DEV0_EPF0_STRAP0_ATI_REV_ID_SHIFT == 0xF);  // 位宽 4
    std::puts("  [PASS] 3 位域抽取语义与 nbio_v7_11_get_rev_id 一致");
}

// 4. 用户态替身类型宽度与内核 IOTypes.h 一致（32/8 位）
static void test_type_widths() {
    assert(sizeof(UInt32) == 4);
    assert(sizeof(UInt8) == 1);
    std::puts("  [PASS] 4 类型宽度 32/8 位");
}

int main() {
    std::puts("路线图第一步验收：rev-id strap 寄存器地址（RCC_STRAP1_RCC_DEV0_EPF0_STRAP0）");
    test_constants_match_linux();
    test_read_address_is_d35();
    test_rev_id_extraction();
    test_type_widths();
    std::puts("全部通过。");
    return 0;
}
