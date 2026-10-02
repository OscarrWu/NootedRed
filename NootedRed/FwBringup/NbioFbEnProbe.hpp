// NbioFbEnProbe —— 乙线 B 方案·序①「NBIO `BIF_FB_EN` 写入探针」的**序列逻辑**（header-only）
//
// ── 本文件是什么 ────────────────────────────────────────────────────────────────
//   把 `docs/子任务/乙线B方案-序1NBIO呈请材料.md` §5.1/§5.3 的写入序列实现为**唯一的序列产生者**：
//     ① 写前读原值（窗口内，dword 口径）   → `fbEnBefore`
//     ② 写 `0x3`（`FB_READ_EN | FB_WRITE_EN`，**恰好 1 次**）→ `writeVal`
//     ③ 立即写后读回（同口径）             → `fbEnAfter`（== 0x3 ⇒ 写已落地；≠ ⇒ 写被拒绝）
//     ④ 越窗只读判据 3 点（经 `PCIE_INDEX2/DATA2` 间接通道，**仅 3 点、无循环/无扫描/无矩阵/无重试**）
//          · 真读点   = `MP0_SMN_C2PMSG_81`（`0x91` ⇒ 字节 `0x090FF244`，PSP SOS 存活位）
//          · 阴性对照 = `smnAddr(0)`（字节 `0x090FF000`）
//          · 阳性对照 = **窗口内**点：`PCIE_INDEX2` 写-回读自证 + `PCIE_DATA2` 直读（A 类，零 SMN 副作用）
//
// ── 本文件里没有 MMIO ───────────────────────────────────────────────────────────
//   全部访问经调用方注入的 **`NbioFbEnCallbacks`**（三个裸回调）⇒ 本文件的序列逻辑可在**用户态**
//   用 mock 回调逐条断言（见 `tests/test_nbio_fb_en_probe.cpp`），且**不含任何 IOKit 依赖**。
//
// ── 常量与依据（逐字，供批准者核对）──────────────────────────────────────────────
//   · 寄存器：`regBIF_BX1_BIF_FB_EN`，偏移 `0x0100`、`BASE_IDX = 2`
//     （`srcs/linux-amdgpu-ref/.../asic_reg/nbio/nbio_7_11_0_offset.h:8726-8727`）。
//     Linux 有 BX0（`0x8e20`/idx5）、**BX1（`0x0100`/idx2）**、BX2（`0x2ffc0e20`/idx5）三个变体，
//     `nbio_v7_11_mc_access_enable()`（`amdgpu/nbio_v7_11.c:49-57`）用的是 **BX1** ⇒ 本探针取 BX1。
//   · 使能值 `0x3` = `FB_READ_EN(bit0) | FB_WRITE_EN(bit1)`
//     （`nbio_7_11_0_sh_mask.h:55717-55718`：`__FB_READ_EN_MASK 0x1L` / `__FB_WRITE_EN_MASK 0x2L`）。
//     三个变体的位值相同 ⇒ 不存在"用错位掩码"的风险。
//   · 段基址 `NBIO_BASE__INST0_SEG2 = 0xD20`（`yellow_carp_offset.h:975`；
//     项目现役副本 `GPUDriversAMD/RavenIPOffset.hpp:9` = `NBIO_BASE_2`）。
//   · dword 索引 `0xE20` = `0xD20 + 0x0100`；**字节地址 `0x3880`** = `0xE20 × 4`（说明性换算，**不进代码**）。
//   · 窗口判据 `(reg * 4) < rmmio->getLength()`（`NRed.cpp:243/250` 的窗口内分支），
//     本机窗口 `0x80000`（`kb/re/phoenix-linux-boot-log.md:21-22`：`register mmio size: 524288`）
//     ⇒ `0x3880 < 0x80000` ⇒ **窗口内（A 类直写，不经间接通道）**。
//
// ── 本轮会发生的全部硬件访问（写死在此，供报告逐条核对）─────────────────────────
//   窗口内（A 类）：
//     · 读 `fbEn`      ×1   [读 `rmmioPtr[0xE20]`]
//     · **写 `fbEn` = 0x3 ×1**  [写 `rmmioPtr[0xE20]`]  ← ★ 本轮唯一的寄存器写
//     · 读 `fbEn`      ×1   [写后读回]
//     · 写 `PCIE_INDEX2` ×1、读 `PCIE_INDEX2` ×1（阳性对照自证）、读 `PCIE_DATA2` ×1（阳性对照直读）
//   越窗（B 类，间接通道）：
//     · 真读点 1 次读：  [写 `PCIE_INDEX2`=0x090FF244] → [读 `PCIE_DATA2`]
//     · 阴性对照 1 次读：[写 `PCIE_INDEX2`=0x090FF000] → [读 `PCIE_DATA2`]
//   ⇒ 越窗间接读合计 **4 次 MMIO**（2 次索引写 + 2 次数据读）；**对 `PCIE_DATA2` 零写入**
//     ⇒ **对 PSP/SMU/GPU 任何功能寄存器零写入**。
//   ⛔ 无循环、无扫描、无矩阵、**无重试**（`maxRetries = 0`）、不碰 `PCIE_INDEX_HI`（地址 < 2^32）。
//   **不带门控时：一次 MMIO 都不会发生**（调用方在门控为假时根本不进本文件）。
//
// ── 约束 ────────────────────────────────────────────────────────────────────────
//   约束：header-only、零动态分配、无异常、**kext 环境可编译**。
//   ⛔ 本文件被 `X6000FB.cpp` 包含 ⇒ 进 kext 构建路径 ⇒ **禁止** `<cstdint>`/`<cstddef>`/`std::`
//      （kext 构建环境无 libc++；依据 CI run #220 的 `'cstdint' file not found` 与项目既有约定
//      `DisplaySeq/Dcn314DccgSeq.hpp:23`）。一律用 `<stdint.h>` + 全局类型名。
//   命名空间 `fw`，与 `FwBringup/` 下其它模块一致。
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once

#include "../Regs/NBIO.hpp"                    // PCIE_INDEX2 / PCIE_DATA2（`UInt32` 亦由此经 <IOKit/IOTypes.h> 提供）
#include "../GPUDriversAMD/RavenIPOffset.hpp"   // NBIO_BASE_2（= NBIO_BASE__INST0_SEG2 = 0xD20）
#include "RegAddr.hpp"
#include "RegSinkKernel.hpp"       // fw::SmnIndirectAccess::read（规范间接读序列）
#include "SmnReadProbe.hpp"        // kSmnProbeRegOffset / kSmnProbeBlankOffset
#include <stdint.h>

namespace fw {

// ── 常量（值/偏移/段基址三者的出处逐字见文件头注释）────────────────────────────

// `regBIF_BX1_BIF_FB_EN` 的**寄存器偏移**（dword 单位；`nbio_7_11_0_offset.h:8726`）。
// ⛔ **不得**在代码里写死 `0x3880` 或 `0xE20` 字面量 —— 地址一律由本常量经段基址算出。
inline constexpr uint32_t kRegBifBx1BifFbEn = 0x0100;

// **dword 索引**（相对 BAR5 映射基址）= 段基址 `NBIO_BASE_2` + 寄存器偏移 = `0xE20`。
// 与 `NRed::readReg32/writeReg32` 的窗口内分支同口径（那个分支把入参当 dword 索引）。
inline constexpr uint32_t kFbEnDwordOffset = NBIO_BASE_2 + kRegBifBx1BifFbEn;

// **字节地址**（= dword 索引 × 4 = `0x3880`）；仅用于"窗口判据"与报告核对，**不进任何 MMIO 调用**。
inline constexpr uint32_t kFbEnByteOffset = kFbEnDwordOffset * 4;

// 数据手册口径窗口大小（`phoenix-linux-boot-log.md:21-22`：`register mmio size: 524288`）。
// ⚠️ 运行时判据用 `rmmio->getLength()`（真值），本常量只用于编译期自证与报告。
inline constexpr uint32_t kFbEnMmioWindowBytes = 0x80000;

// 使能值 = `FB_READ_EN(bit0) | FB_WRITE_EN(bit1)`（`nbio_7_11_0_sh_mask.h:55717-55718`）。
inline constexpr uint32_t kFbEnEnableValue = 0x3;

// 越窗判据点（字节地址，由 `RegAddr.hpp` 的单一事实源公式算出，**不写死字面量**）：
//   真读点   = `MP0_SMN_C2PMSG_81`（`0x91`）  ⇒ `0x090FF244`（PSP SOS 存活标志）
//   阴性对照 = 同公式偏移 0                    ⇒ `0x090FF000`
inline constexpr uint32_t kFbEnRegAddr   = smnAddr(kSmnProbeRegOffset);    // 0x090FF244
inline constexpr uint32_t kFbEnBlankAddr = smnAddr(kSmnProbeBlankOffset);  // 0x090FF000

// **阳性对照**（窗口内，A 类）：`PCIE_INDEX2` / `PCIE_DATA2` 的字节地址。
//   自证形态 = 往 `PCIE_INDEX2` 写本探针窗口内点 `kFbEnByteOffset` → 回读 `PCIE_INDEX2`
//              （== 写入值 ⇒ 索引寄存器可写可回读）→ 读 `PCIE_DATA2`（同时刷新 posted write）。
//   与 Linux `RREG32_PCIE`（`amdgpu_reg_access.c` 的窗口内分支）同形态：**点地址本身在窗口内**。
//   ⛔ 这里写的是 `kFbEnByteOffset`（= **本探针自己那个寄存器**），不是任何 SMN 地址
//     ⇒ 即便地址译码异常，也只会落回同一个已被本探针读/写过的位置，**不引入第二个功能寄存器**。
inline constexpr uint32_t kPcieIndex2ByteOffset = PCIE_INDEX2 * 4;   // 0x38
inline constexpr uint32_t kPcieData2ByteOffset  = PCIE_DATA2 * 4;    // 0x3C

// ── 回调集合 ────────────────────────────────────────────────────────────────────
//   三个裸回调（函数指针 + 一个 void* 上下文），零分配、内核态可用：
//     readDw / writeDw  ：按 **dword 索引**读/写 BAR5（窗口内直访）。
//     readExt           ：按 **字节地址**经 `PCIE_INDEX2/DATA2` 间接读（越窗）。
//   ⛔ 无锁回调：本轮**不在**崩溃上下文执行（调用点是 `wrapPpHelperPowerUp` 入口、org 之前，
//     属正常可睡眠上下文），但也**刻意不引入锁** —— 与既有 `-NRedSmnRead1` 探针同一形态
//     （`X6000FB.cpp:2140` 的 `lock = nullptr`），保持"本轮只改一个变量"。
struct NbioFbEnCallbacks {
    uint32_t (*readDw)(void* ctx, uint32_t dwordOffset);
    void     (*writeDw)(void* ctx, uint32_t dwordOffset, uint32_t value);
    uint32_t (*readExt)(void* ctx, uint32_t byteAddr);
    void*    ctx;
};

// ── 读数结果 ────────────────────────────────────────────────────────────────────

// 一轮写入探针的完整读数。
//   哨兵约定（沿用 `SmnReadProbe.hpp` 与 P3P14 设计稿 §3.3 的同类纪律）：
//     · `armed == 0` ⇒ **本探针根本没跑**（门控未命中）——其余字段**一律无意义**；
//     · `fbEnBefore == 0xFFFFFFFF` ⇒ 写前读**可能**是"读不到"而非"真的是全 1"
//       （窗口内直读不会失败，故此值只应出现在"硬件确实回全 1"的情形）。
struct NbioFbEnReadings {
    uint32_t armed;        // 1 = 本探针执行了（入口自证；0 ⇒ 本行其余字段无效）
    uint32_t hasRmmio;     // 1 = BAR5 已映射（未映射 ⇒ 全部 MMIO 跳过，本行读数无效）
    uint32_t fbEnBefore;   // ① 写前原值（窗口内直读）
    uint32_t writeVal;     // ② 本轮写入值（恒为 kFbEnEnableValue = 0x3）
    uint32_t fbEnAfter;    // ③ 写后读回（== writeVal ⇒ 写已落地）
    uint32_t regAddr;      // ④-① 真读点字节地址（0x090FF244）
    uint32_t regValue;     // ④-① 真读点读数
    uint32_t blankAddr;    // ④-② 阴性对照字节地址（0x090FF000）
    uint32_t blankValue;   // ④-② 阴性对照读数
    uint32_t posIdxWr;     // ④-③ 写进 `PCIE_INDEX2` 的窗口内点地址（= 0x3880）
    uint32_t posIdxRb;     // ④-③ 回读 `PCIE_INDEX2`（== posIdxWr ⇒ 索引可写可回读）
    uint32_t posIdx;       // ④-③ 自证判据：posIdxRb == posIdxWr ⇒ 1
    uint32_t posData;      // ④-③ `PCIE_DATA2` 直读
    uint32_t regErr;       // 真读点的 `SmnAccessError`（0 = 序列成功）
    uint32_t blankErr;     // 阴性对照的 `SmnAccessError`
    uint32_t same;         // 真/空读数相同 ⇒ 1（对照守则要求 0，为 1 则本轮读数整体不可信）
    uint32_t retries;      // 恒为 kFbEnMaxRetries = 0
    uint32_t writeCount;   // 本函数对**功能寄存器**的写次数（恒为 1）
};

// 本轮固定参数：**单次尝试、无重试**（呈请材料 §2.4「无循环/无扫描/无矩阵/无重试」）。
inline constexpr uint32_t kFbEnMaxRetries = 0;

// 把本探针的三个回调**适配成** `SmnIndirectAccess` 需要的 `SmnCallbacks` 形态。
//   为什么需要适配：`SmnIndirectAccess` 的读写回调是 `uint32_t (*)(void*, uint32_t)`
//   形态的 **dword 索引**访问器（与 `SmnReadProbe.hpp` 的用法逐字同形），而本探针的
//   `readDw` 恰好是同一形态 ⇒ 直接转发；`readExt` 在本适配里**不使用**
//   （`SmnIndirectAccess` 自己完成"写索引 → 回读索引 → 读数据"的规范序列）。
//   适配结构体是**局部对象**，逐次传入（`SmnIndirectAccess::read` 的 `const&` 语义），
//   **没有**任何跨翻译单元符号、没有 static 状态、不引入锁。
inline SmnCallbacks makeSmnCb(const NbioFbEnCallbacks& cb)
{
    SmnCallbacks s{};
    s.readReg    = cb.readDw;
    s.writeReg   = cb.writeDw;
    s.lock       = nullptr;   // 本轮不取锁（调用点属正常上下文，但刻意保持最小变量）
    s.unlock     = nullptr;
    s.delayUs    = nullptr;   // 无重试 ⇒ 延时路径不可达
    s.ctx        = cb.ctx;
    s.maxRetries = kFbEnMaxRetries;   // = 0（无重试）
    return s;
}

// ── 探针本体 ────────────────────────────────────────────────────────────────────

inline NbioFbEnReadings runNbioFbEnWriteProbe(const NbioFbEnCallbacks& cb)
{
    // `readDw` / `writeDw` 是**裸指针访问**：调用方必须已确认映射（`hasRmmio()`）。
    //   本函数不做映射判定（那是调用方的职责，判据在调用点记录），故此处只做非空兜底。
    NbioFbEnReadings out{};
    out.writeVal   = kFbEnEnableValue;
    out.regAddr    = kFbEnRegAddr;
    out.blankAddr  = kFbEnBlankAddr;
    out.regValue   = kSmnProbeNoValue;
    out.blankValue = kSmnProbeNoValue;
    out.retries    = kFbEnMaxRetries;
    out.writeCount = 1;
    if (cb.readDw == nullptr || cb.writeDw == nullptr || cb.readExt == nullptr) {
        out.writeCount = 0;   // 回调不全 ⇒ 一个动作都没做（如实记录，绝不留"半轮"）
        return out;
    }

    // ① 写前读原值（窗口内直读；支持回滚基线 + 一轮定论 U-3「BIOS 是否已使能」）。
    out.fbEnBefore = cb.readDw(cb.ctx, kFbEnDwordOffset);

    // ② ★ 写入（恰好 1 次）：`0x3880 ← 0x3`。
    cb.writeDw(cb.ctx, kFbEnDwordOffset, kFbEnEnableValue);

    // ③ 立即写后读回：== 0x3 ⇒ 写已落地；≠ 0x3（含全 1）⇒ 写被硬件拒绝。
    out.fbEnAfter = cb.readDw(cb.ctx, kFbEnDwordOffset);

    // ④-① 真读点（越窗间接读，PSP SOS 存活位）。
    const SmnReadResult reg = SmnIndirectAccess::read(kFbEnRegAddr, makeSmnCb(cb));
    out.regValue = reg.value;
    out.regErr   = static_cast<uint32_t>(reg.error);

    // ④-② 阴性对照（同公式、同通道，偏移 = 0）。
    const SmnReadResult blank = SmnIndirectAccess::read(kFbEnBlankAddr, makeSmnCb(cb));
    out.blankValue = blank.value;
    out.blankErr   = static_cast<uint32_t>(blank.error);

    // ④-③ 阳性对照（**窗口内**，A 类）：`PCIE_INDEX2` 写-回读自证 + `PCIE_DATA2` 直读。
    out.posIdxWr = kFbEnByteOffset;
    cb.writeDw(cb.ctx, PCIE_INDEX2, kFbEnByteOffset);
    out.posIdxRb = cb.readDw(cb.ctx, PCIE_INDEX2);
    out.posIdx   = (out.posIdxRb == out.posIdxWr) ? 1u : 0u;
    out.posData  = cb.readDw(cb.ctx, PCIE_DATA2);

    // 对照守则：真/空读数必须不同，否则说明两次读落在同一字节地址 ⇒ 本轮读数整体不可信。
    out.same = (out.regValue == out.blankValue) ? 1u : 0u;
    return out;
}

// ── 编译期自检（四要素与窗口判据）────────────────────────────────────────────────

// dword 索引：`NBIO_BASE_2`(0xD20) + `regBIF_BX1_BIF_FB_EN`(0x0100) = 0xE20。
static_assert(kFbEnDwordOffset == 0xE20u, "BIF_FB_EN dword index = 0xD20 + 0x0100 = 0xE20");
static_assert(NBIO_BASE_2 == 0xD20u, "NBIO_BASE__INST0_SEG2 = 0xD20 (yellow_carp_offset.h:975)");
static_assert(kRegBifBx1BifFbEn == 0x0100u, "regBIF_BX1_BIF_FB_EN = 0x0100, BASE_IDX = 2");
// 字节地址：0xE20 × 4 = 0x3880。
static_assert(kFbEnByteOffset == 0x3880u, "BIF_FB_EN byte address = 0xE20 * 4 = 0x3880");
// 窗口判据：0x3880 < 0x80000 ⇒ **窗口内（A 类直写）**，不经间接通道。
static_assert(kFbEnByteOffset < kFbEnMmioWindowBytes, "BIF_FB_EN must be inside the BAR5 512 KiB window");
// 使能值就是两个使能位（bit0|bit1），且**不等于** Linux 关闭路径写的 0。
static_assert(kFbEnEnableValue == 0x3u, "FB_READ_EN(0x1) | FB_WRITE_EN(0x2) = 0x3");
static_assert(kFbEnEnableValue != 0u, "分派：写 0 是 nbio_v7_11_mc_access_enable(false) 的语义，本探针决不做");
// 越窗判据点：与 `RegAddr.hpp` / `SmnReadProbe.hpp` 的 static_assert 同源（防常量漂移）。
static_assert(kFbEnRegAddr == 0x090FF244u, "C2PMSG_81 SMN byte address = 0x090FF244");
static_assert(kFbEnBlankAddr == 0x090FF000u, "blank SMN byte address = 0x090FF000");
static_assert(kFbEnRegAddr != kFbEnBlankAddr, "reg and blank must differ");
static_assert(kFbEnRegAddr < (1ull << 32), "reg address must fit in 32 bits");
static_assert(kFbEnBlankAddr < (1ull << 32), "blank address must fit in 32 bits");
// 阳性对照点（窗口内，`PCIE_INDEX2/DATA2` 的字节地址）——同样在窗口内。
static_assert(kPcieIndex2ByteOffset == 0x38u, "PCIE_INDEX2 byte offset = 0x0E * 4 = 0x38");
static_assert(kPcieData2ByteOffset == 0x3Cu, "PCIE_DATA2 byte offset = 0x0F * 4 = 0x3C");
static_assert(kPcieIndex2ByteOffset < kFbEnMmioWindowBytes, "PCIE_INDEX2 must be inside the window");
// 阳性对照的自证点 = 本探针自己那个寄存器 ⇒ 窗口内点做 `PCIE_INDEX2` 自证不会引入第二个功能寄存器。
static_assert(kFbEnByteOffset == (NBIO_BASE_2 + kRegBifBx1BifFbEn) * 4u, "self-证点 = BIF_FB_EN 字节地址");

}  // namespace fw
