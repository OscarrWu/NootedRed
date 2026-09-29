// SmnReadProbe —— 乙线「只读单点规范 SMN 访问」探针的**序列逻辑**（header-only）
//
// ── 本文件是什么 ────────────────────────────────────────────────────────────────
//   把 `docs/子任务/乙线SMN单点验证设计.md` §4.3 的「只读单点草案」实现为**唯一的序列产生者**。
//   它对**一个真寄存器（MP0_SMN_C2PMSG_81，PSP SOS 存活标志）**执行**恰好一次**规范序列读，
//   再对与它**同一间接通道（同一索引基址 + 同一偏移）**的一个**空白对照**地址执行**恰好一次**
//   同样的读；两次读**互不干扰**、**无循环、无扫描、无矩阵、无重试**。
//
// ── 给谁用 ──────────────────────────────────────────────────────────────────────
//   唯一的调用者是内核侧探针块（`X6000FB.cpp` 的 `-NRedSmnRead1` 门控，默认关）。
//   本文件里**没有 MMIO**：访问经调用方注入的 `fw::SmnCallbacks`（`RegSinkKernel.hpp`），
//   使本文件的序列逻辑可在**用户态**用 mock 回调逐条断言（见 `tests/test_smn_read_probe.cpp`）。
//
// ── 依据 ────────────────────────────────────────────────────────────────────────
//   1. 规格书 §4.1/§4.3：读数 = `MP0_SMN_C2PMSG_81`，空白对照，总 MMIO ≤ 6 次，
//      **不回读索引、不处理 HI、不重试**（把 76/77 轮的候选主因「索引回读」排除在本轮之外）。
//   2. 规范序列：`docs/子任务/乙线SMN安全访问调查.md` §3.3（`SmnIndirectAccess::read` 的子集）。
//   3. 地址公式：`FwBringup/RegAddr.hpp`（`smnAddr()`，本项目的单一事实源）——
//      公式里**不写死任何地址**；空白对照也用同一公式（同一基址 + 同一偏移），只是**寄存器 ID 不同**。
//   4. 寄存器偏移：`Regs/PSP13.hpp`（`MP0_SMN_C2PMSG_81 = 0x0091`）。
//
// ── 本轮会发生的全部硬件访问（写死在此，供报告逐条核对）─────────────────────────
//   真寄存器 1 次读：  [写 PCIE_INDEX2] → [读 PCIE_DATA2]          共 2 次 MMIO
//   空白对照 1 次读：  [写 PCIE_INDEX2] → [读 PCIE_DATA2]          共 2 次 MMIO
//   合计 **4 次 MMIO**：**2 次索引寄存器写 + 2 次数据寄存器读**。
//   ⛔ **对 `PCIE_DATA2` 没有任何写** ⇒ **对任何功能寄存器都没有写**（含 SMU/PSP/GPU 状态寄存器）。
//   ⛔ 不做 `PCIE_INDEX2` 回读、不碰 `PCIE_INDEX_HI`（地址 < 2^32）、不重试（`maxRetries = 0`）。
//   **不带门控时：一次 MMIO 都不会发生**（调用方在门控为假时根本不进本文件）。
//
// 约束：header-only、零动态分配、无异常，**kext 环境可编译**。
//  ⛔ 本文件被 `X6000FB.cpp` 包含 ⇒ 进 kext 构建路径 ⇒ **禁止** `<cstdint>`/`<cstddef>`/`std::`
//     （kext 构建环境无 libc++；依据 CI run #220 的 `'cstdint' file not found` 与项目既有约定
//     `DisplaySeq/Dcn314DccgSeq.hpp:23`）。一律用 `<stdint.h>` + 全局类型名。
//     `../Regs/PSP13.hpp` 提供 `UInt32`（走 `<IOKit/IOTypes.h>`，两环境同源），此处不再引入标准库头。
//  命名空间 `fw`，与 `FwBringup/` 下其它模块一致。

#pragma once

#include "RegAddr.hpp"
#include "RegSinkKernel.hpp"
#include "../Regs/PSP13.hpp"
#include <stdint.h>

namespace fw {

// ── 常量 ────────────────────────────────────────────────────────────────────────

// 真寄存器：PSP 的 SOS 存活标志（Linux `psp_v13_0_is_sos_alive()` 读它，`!= 0` 即判 SOS 已存活）。
// 偏移来源：Regs/PSP13.hpp（mp_13_0_4_offset.h:129，BASE_IDX = 1）。
inline constexpr uint32_t kSmnProbeRegOffset = MP0_SMN_C2PMSG_81;   // 0x0091

// 空白对照：与真寄存器**同一间接通道**（同一索引基址 kMpSeg1Base、同一偏移 = 基址+0），
// 因此它在 SMN 空间里占据与真寄存器**相邻同形的字节地址**（真 = 基址×4+0x244，对 = 基址×4）。
// 为什么这样选（规格书只要求"一个已知无效地址的不重复断言"，形式由实现定）：
//   · `smnAddr(0)` = `kMpSeg1Base * 4` 由**同一公式**算出（不引入第二个真寄存器、不写死字面量）；
//   · 它**同通道、同地址形态、同访问序列**、只差偏移 ⇒ 真/对两次读数若出现系统性差异，
//     可归因于**寄存器本身**而不是通道形态（这是把变量压到最小的对照形态）。
// 判据：对照必须**不等于**真寄存器读数，否则说明本轮两次读**落在同一字节地址上**
//      （地址算错 / 索引写入未生效）⇒ 本轮读数整体不可信（规格书 §4.3「对照组」条）。
inline constexpr uint32_t kSmnProbeBlankOffset = 0;

// 本轮固定参数：**单次尝试、无重试、无延时**（规格书 §4.3「访问形态」条）。
//   `maxRetries = 0` ⇒ 每点恰好 1 次序列（写索引 → 读数据），失败即失败，绝不重试。
//   连重试都关掉的原因：76/77 轮卡死的候选主因之一就是"间接通道上多做动作"，
//   本轮要把**除"规范单点读"以外的一切**都排除掉（一次定论，一轮只改一个变量）。
inline constexpr uint32_t kSmnProbeMaxRetries = 0;

// ── 读数结果 ────────────────────────────────────────────────────────────────────

// 一次单点读的结果。`error != SmnAccessError::None` 表示该点未取到有效值
// （此时 `value` 为 `SmnIndirectAccess` 的失败返回值 0xFFFFFFFF，**不得**当作硬件读数）。
struct SmnProbePoint {
    uint32_t      value;
    SmnAccessError error;
};

inline constexpr uint32_t kSmnProbeNoValue = 0xFFFFFFFFu;

// 一轮探针的完整读数（供调用方**预先求值为局部标量**后写进 panic 实参）。
struct SmnProbeReadings {
    uint32_t regAddr;        // 真寄存器的 SMN 字节地址（p = 基址×4 + 0x244）
    uint32_t regValue;       // 真寄存器读数
    uint32_t blankAddr;      // 空白对照的 SMN 字节地址（b = 基址×4）
    uint32_t blankValue;     // 空白对照读数
    uint32_t regError;       // 真寄存器错误码（SmnAccessError，0 = 成功）
    uint32_t blankError;     // 空白对照错误码
    uint32_t retries;        // 本轮固定为 kSmnProbeMaxRetries
};

// ── 探针本体 ────────────────────────────────────────────────────────────────────

// 执行「1 个真寄存器 + 1 个空白对照」的规范只读单点。**不循环、不扫描、不重试、不写数据**。
//
// 调用方职责（三条硬要求）：
//   ① **门控为假时不得调用本函数**（默认零 MMIO、零副作用）；
//   ② 必须在**已验证的失败出口**（`wrapHandleCriticalError`，Apple 已判定失败之后）内调用——
//      在那里读**不可能掩盖成功**（手册 §4A / 作战计划 T7 卡纪律）；
//   ③ `cb` 里**只允许**传 MMIO 读/写回调和锁回调；**`delayUs` 必须为 nullptr**（本轮不依赖延时）。
inline SmnProbeReadings runSmnReadProbe(const SmnCallbacks& cb)
{
    // SmnCallbacks 按值拷贝一次，仅改 maxRetries：不修改调用方的对象。
    SmnCallbacks local = cb;
    local.maxRetries   = kSmnProbeMaxRetries;
    local.delayUs      = nullptr;   // 无重试 ⇒ 延时路径不可达；显式置空，杜绝任何隐藏延时。

    const SmnReadResult reg   = SmnIndirectAccess::read(smnAddr(kSmnProbeRegOffset), local);
    const SmnReadResult blank = SmnIndirectAccess::read(smnAddr(kSmnProbeBlankOffset), local);

    SmnProbeReadings out;
    out.regAddr    = smnAddr(kSmnProbeRegOffset);
    out.regValue   = reg.value;
    out.blankAddr  = smnAddr(kSmnProbeBlankOffset);
    out.blankValue = blank.value;
    out.regError   = static_cast<uint32_t>(reg.error);
    out.blankError = static_cast<uint32_t>(blank.error);
    out.retries    = kSmnProbeMaxRetries;
    return out;
}

// ── 编译期自检（地址与不变式）────────────────────────────────────────────────────

// 真寄存器地址：C2PMSG_81 的 SMN 字节地址（与 RegAddr.hpp 的 static_assert 同源）。
static_assert(smnAddr(kSmnProbeRegOffset) == 0x090FF244u, "C2PMSG_81 SMN byte address = 0x090FF244");
// 空白对照 = SEG1 基址本身（同公式、同通道）。
static_assert(smnAddr(kSmnProbeBlankOffset) == kMpSeg1Base * 4, "blank address = SEG1 base * 4");
static_assert(smnAddr(kSmnProbeBlankOffset) == 0x090FF000u, "blank SMN byte address = 0x090FF000");
// 真 <> 对：必须落在**不同**的字节地址上，否则对照失去意义（见 kSmnProbeBlankOffset 注释）。
static_assert(smnAddr(kSmnProbeRegOffset) != smnAddr(kSmnProbeBlankOffset), "reg and blank must differ");
// 两点都在 32 位内 ⇒ 恒不需要 PCIE_INDEX_HI（与 RegSinkKernel 的判据一致）。
static_assert(smnAddr(kSmnProbeRegOffset) < (1ull << 32), "reg address must fit in 32 bits");
static_assert(smnAddr(kSmnProbeBlankOffset) < (1ull << 32), "blank address must fit in 32 bits");

}  // namespace fw
