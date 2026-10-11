// NRedWindowProbe.hpp —— A-25 first-false 窗口法 · 最小只读探针（纯逻辑，用户态可测）
//
// 目的：把 B1（31 条）的失败点收窄到"窗口"——按**程序序**读一组水印，
//   **第一个为空的水印**即失败所在窗口（first-false），而非唯一一条（R-3 口径）。
//
// 挂点：`AMDHardware::init`（kc `0x4ba9cea`）只读包装——`ret = org(...)` 后、
//   **`ret == 0` 时**快照 `this`（此时实例仍存活；`0x62f0` 的 release 在其后）。
//
// 水印集（`this` 相对偏移；读法依既有 P3P14/EngTbl 探针的 `rd64`/`rd8` 约定）：
//   P3    : +0x2FC / +0x2FE / +0x300 / +0x302 / +0x30D
//   P5后半: +0x338 / +0x340
//   P12   : +0x528 / +0x530
//   P15   : +0x20810
//   P25   : +0x3B8（引擎表首槽）
//   加速器侧: +0x1A38（AMDHardware*）与 +0x1E89 bit0
//
// P5 时序二值判：同读 `self+0x50` → `node+0xD8` / `node+0xD0`
//   （回答"P5 后半在 S1 时刻是否为 0"）。
//
// 纪律（硬）：
//   · 门控默认关、仅观测（关 ⇒ 零读/零 MMIO/零写）；
//   · 通道走 A-22 内存优先＋安全时点刷盘（本文件**不 include 任何文件/VFS 头**）；
//   · **未新增寄存器探针**（仅内存读）；
//   · 不改 Apple 字节；不动 T1 闸／既有门控解析行／panic 格式串。
//
// ⛔ kext 环境可编译：禁 `<cstdint>`/`<cstddef>`/`std::`（CI run #220 纪律）；用 `<stdint.h>`。

#pragma once

#include <stdint.h>

namespace nred {

// ── 水印标识（程序序 = 枚举序）──
enum WmId : uint32_t {
    WmP3_2FC = 0, WmP3_2FE, WmP3_300, WmP3_302, WmP3_30D,
    WmP5_338, WmP5_340,
    WmP12_528, WmP12_530,
    WmP15_20810,
    WmP25_3B8,
    WmAccel_1A38, WmAccel_1E89B0,
    WmCount
};

// 水印名（判读输出用；与 WmId 同序）
inline const char* wmName(const uint32_t id)
{
    switch (id) {
        case WmP3_2FC:      return "P3+0x2FC";
        case WmP3_2FE:      return "P3+0x2FE";
        case WmP3_300:      return "P3+0x300";
        case WmP3_302:      return "P3+0x302";
        case WmP3_30D:      return "P3+0x30D";
        case WmP5_338:      return "P5+0x338";
        case WmP5_340:      return "P5+0x340";
        case WmP12_528:     return "P12+0x528";
        case WmP12_530:     return "P12+0x530";
        case WmP15_20810:   return "P15+0x20810";
        case WmP25_3B8:     return "P25+0x3B8";
        case WmAccel_1A38:  return "Accel+0x1A38";
        case WmAccel_1E89B0:return "Accel+0x1E89b0";
        default:            return "?";
    }
}

// ── A-30：扩展口径 · 写侧指针哨兵（A-26 §4/§10，判据"字段==0 ⇒ 该条失败"）──
//  程序序（A-26 §2）：P4<P5<P6<P7<P8<P10<P11<P12<P13<P14<P15<P16<P17<P18<P19<P21<…<P31
enum SentryId : uint32_t {
    P4_20630 = 0,
    P5_Services,                     // A-44：P5 合成判据（`+0x338` ∧ `+0x340` 均非 0）
    P5_bit13,                        // A-44：P5 的掩码忠实位（bit13；与上项同值、互为印证）
    P6_370,
    P6Obj_44,                        // A-34：P6 对象（`AMDHWRegisters`）`+0x44` 内部成功位（= P7 通过位）
    P7_bit14, P8_bit15,
    P10_3B0,                         // P11 恒真，无哨兵
    P12_530,
    P13_bit18, P14_2F8,              // {P13,P14} 二元窗口（A-42：bit18 为 `initializeTtl` 返回值的忠实位）
    P16_378, P17_bit19, P18_380, P19_bit20,
    P21_388, P22_bit21, P23_518, P24_205F8,
    P25_bit22, P26_bit23, P27_3A0, P28_bit24,
    P29_368, P30_205C8, P31_205D0,
    SentryCount
};

// A-30：位掩码位分配（accel+0x1e88，A-26 §5.2）
//  位 N ⇔ byte 0x1e88+(N>>3) 的第 N&7 位。
inline uint32_t maskBit(const uint32_t n) { return 1u << (n & 7u); }
inline uint32_t maskByteOff(const uint32_t n) { return 0x1E88 + (n >> 3); }

// A-34：**唯一合法的位取样读法**——把"位号 n"严格按 §5.2 换算为
//   位 n ⇔ byte `0x1e88+(n>>3)` 的第 `n&7` 位。
//  ⚠ `maskBit(n)` 是**字节内**位掩码（`1u<<(n&7)`）；若与"dword 读"混用，位 n 会被误取为
//   `byte 0x1e88`（byte0）的第 `n&7` 位 —— 即 B12 轮暴露的位掩码哨兵读法缺陷根因。
//  ⇒ 本函数先按字节偏移读**单个 byte**（不做更宽的读，`volatile` 读 —— A-38 起参数即
//  `const volatile uint8_t*`，故经本函数的每一次取样都是 volatile 读），再套**字节内**掩码：
//  位号语义同一。
inline uint64_t maskBitAt(const volatile uint8_t* const base, const uint32_t n)
{
    const uint8_t mb = base[maskByteOff(n)];
    return (mb & maskBit(n)) ? 1ULL : 0ULL;
}

// ── A-41：掩码所在对象的**对象基址**取法（可用户态测试）──
//  依据 A-26 §5.1：`AMDHWHandler::init` 把加速器对象存入 `handler+0x10`（Z `0x4bc5d`），
//  掩码取子（kc `0x4b835d4`）返回 `*(handler+0x10) + 0x1e88` ⇒ **掩码在加速器对象上**，
//  而 `AMDHardware::init` 的 `this`（`self`）是 **hwInterface**（另一对象，其 `0x1e88` 区无写者）。
//  @param handler `AMDHardware::init` 第 3 参（`IAMDHWHandler*`）的数值；调用方须先判定其为内核指针
inline uint64_t accelFromHandler(const uint64_t handler)
{
    const volatile uint8_t* const p = reinterpret_cast<const volatile uint8_t*>(handler);
    return *reinterpret_cast<const volatile uint64_t*>(p + 0x10);
}

// ── A-41：位取样（**带基址有效性纪律**）──
//  基址非法（`accelValid==0` 或基址为空）⇒ 返回 0 且**不改写** `*valueOut`
//  ⇒ 调用方须把该哨兵记为 `valid=0`（"不可判"），**不得**当作"读到 0"（否则会被
//  first-false 规则误判为失败）。
inline uint32_t maskBitSample(const volatile uint8_t* const accelBase, const uint32_t accelValid,
                              const uint32_t n, uint64_t* const valueOut)
{
    if (accelValid == 0 || accelBase == nullptr) { return 0; }
    if (valueOut != nullptr) { *valueOut = maskBitAt(accelBase, n); }
    return 1;
}

// ── A-44：P5 的**合成判据**（A-26 §1.2 #2）──
//  `this+0x338`（HW 服务 TTL 对象）与 `this+0x340`（CAIL 对象）**同时非 0** ⟺
//  P5（`AMDHardware::initializeExternalInterfaces`，kc `0x4baa9ba`）返回真。
//  返回 1 = P5 通过、0 = 未通过。
inline uint64_t p5ServicesVerdict(const uint64_t v338, const uint64_t v340)
{
    return (v338 != 0 && v340 != 0) ? 1ULL : 0ULL;
}

// 哨兵名（判读输出用；与 SentryId 同序）
inline const char* sentryName(const uint32_t id)
{
    switch (id) {
        case SentryId::P4_20630:   return "P4+0x20630";
        case SentryId::P5_Services: return "P5 svc(338/340)";
        case SentryId::P5_bit13:   return "P5 bit13";
        case SentryId::P6_370:     return "P6+0x370";
        case SentryId::P6Obj_44:   return "P6obj+0x44";
        case SentryId::P7_bit14:   return "P7 bit14";
        case SentryId::P8_bit15:   return "P8 bit15";
        case SentryId::P10_3B0:    return "P10+0x3B0";
        case SentryId::P12_530:    return "P12+0x530";
        case SentryId::P13_bit18:  return "P13 bit18";
        case SentryId::P14_2F8:    return "P14+0x2F8";
        case SentryId::P16_378:    return "P16+0x378";
        case SentryId::P17_bit19:  return "P17 bit19";
        case SentryId::P18_380:    return "P18+0x380";
        case SentryId::P19_bit20:  return "P19 bit20";
        case SentryId::P21_388:    return "P21+0x388";
        case SentryId::P22_bit21:  return "P22 bit21(部分)";
        case SentryId::P23_518:    return "P23+0x518";
        case SentryId::P24_205F8:  return "P24+0x205F8";
        case SentryId::P25_bit22:  return "P25 bit22";
        case SentryId::P26_bit23:  return "P26 bit23";
        case SentryId::P27_3A0:    return "P27+0x3A0";
        case SentryId::P28_bit24:  return "P28 bit24";
        case SentryId::P29_368:    return "P29+0x368";
        case SentryId::P30_205C8:  return "P30+0x205C8";
        case SentryId::P31_205D0:  return "P31+0x205D0";
        default:                   return "?";
    }
}

// ── A-30：P25 引擎表 11 槽读法（A-26 §11，this+0x3B8..0x408）──
//  Vega10 只填 5 个引擎（i=0,1,2,5,7）；其余槽应恒 0（跳过，非失败）。
struct P25Slots {
    uint64_t slots[11];     // this+0x3B8 + i*8, i=0..10
    uint32_t valid;         // 1 = this 合法且已读
};

// P25 槽位判定（A-26 §11.3）：返回 first-false 归属
//   0 = P25 通过（bit22 已证，或 5 槽全非 0 且 bit22==1）
//   1 = 第 1 次分配失败（slots[0]==0）
//   2..5 = 第 k 次分配失败（k = 第一个为 0 的预期槽：i=1→2, i=2→3, i=5→4, i=7→5）
//   6 = 整表异常（11 槽全非 0，违反 Vega10 只填 5 槽 —— 与位掩码单调位联合判）
inline uint32_t evalP25Slots(const P25Slots& s, const uint32_t bit22)
{
    if (!s.valid) { return 0; }   // 不可判，视作通过（避免误报）——真实判定以 bit22 为准
    const uint32_t expect[5] = {0, 1, 2, 5, 7};
    for (uint32_t k = 0; k < 5; ++k) {
        if (s.slots[expect[k]] == 0) { return k + 1; }  // 1..5 = 第 k+1 次失败
    }
    // 5 槽填满，其余为 0：以 bit22 为准
    return bit22 ? 0 : 6;
}

struct SentryVal {
    uint32_t id;       // SentryId
    uint64_t value;    // 读到的值（指针字段或位值）
    uint32_t valid;    // 1 = 已读到；0 = 未读到（指针非法）
};

// ── first-false 判定结果（A-25 与 A-30 共用）──
struct WindowResult {
    uint32_t firstFalseId;    // 第一个"空"的水印（WmCount/SentryCount = 无空，全部非空）
    uint32_t firstFalseValid; // 1 = 判定有效（该水印确实读到了且值为 0）
    uint32_t readCount;       // 成功读到的水印数
    uint32_t unknownCount;    // 未能读到的水印数（valid==0）
};

// ── A-30：按程序序找"第一个为假的水印"（扩展口径；哨兵/位掩码混合）──
//  "为假" 定义：valid==1 且（指针字段 value==0 || 位掩码比特==0）。
//  valid==0（未读到）不计为假（不可判，与 P3P14 的 0xFF 哨兵纪律一致）。
//  @param vals 已按程序序排列的哨兵读数（SentryId 序 = 程序序）
inline WindowResult findFirstFalseSentry(const SentryVal* const vals, const uint32_t n)
{
    WindowResult out{};
    out.firstFalseId    = SentryId::SentryCount;   // 默认：无假
    out.firstFalseValid = 0;
    out.readCount       = 0;
    out.unknownCount    = 0;
    if (vals == nullptr) { return out; }
    for (uint32_t i = 0; i < n; ++i) {
        if (vals[i].valid) {
            ++out.readCount;
            // A-45：`P13_bit18` 与其它哨兵**同权**参与扫描——A-42 已以地址级证据（`Z 0x5e82b–0x5e85c`
            //  的"值双边写"）证明 bit18 ⟺ P13 结果，A-26 §8.7"无忠实位"作废 ⇒ A-41 的排除**已撤销**。
            if (vals[i].value == 0 && out.firstFalseId == SentryId::SentryCount) {
                out.firstFalseId    = vals[i].id;
                out.firstFalseValid = 1;
            }
        } else {
            ++out.unknownCount;
        }
    }
    return out;
}
// ── 单个水印读数 ──
struct WmVal {
    uint32_t id;      // WmId
    uint64_t value;   // 读到的值
    uint32_t valid;   // 1 = 已读到；0 = 未读到（指针非法，值无意义）
};

// ── P5 时序二值判结果 ──
struct P5Binary {
    uint64_t nodeD8;    // *(node+0xD8)（node = *(self+0x50)）
    uint64_t nodeD0;    // *(node+0xD0)
    uint32_t valid;     // 1 = self/node 指针均合法且已读；0 = 未读到（值无意义）
    uint32_t bothZero;  // 1 = nodeD8==0 && nodeD0==0（P5 后半在 S1 时刻为空）
};

// ── first-false 判定结果（A-25 与 A-30 共用）──

// ── 纯逻辑：按程序序找"第一个为空的水印" ──
//  "空" 定义：valid==1 且 value==0。
//  valid==0（未读到）**不计为假**——它是"不可判"，须与"真的是 0"机械区分
//  （沿用 P3P14 探针的 0xFF 哨兵纪律，此处以 valid 位表达）。
inline WindowResult findFirstFalse(const WmVal* const wms, const uint32_t n)
{
    WindowResult out{};
    out.firstFalseId    = WmCount;   // 默认：无空
    out.firstFalseValid = 0;
    out.readCount       = 0;
    out.unknownCount    = 0;

    if (wms == nullptr) { return out; }
    for (uint32_t i = 0; i < n; ++i) {
        if (wms[i].valid) {
            ++out.readCount;
            if (wms[i].value == 0 && out.firstFalseId == WmCount) {
                out.firstFalseId    = wms[i].id;
                out.firstFalseValid = 1;
            }
        } else {
            ++out.unknownCount;
        }
    }
    return out;
}

// ── 纯逻辑：P5 时序二值判 ──
// @param selfValid/nodeValid 指针合法性（kext 侧 `>= 0xffffff7f80000000` 校验结果）
inline P5Binary evalP5Binary(const uint64_t nodeD8, const uint64_t nodeD0,
                             const uint32_t selfValid, const uint32_t nodeValid)
{
    P5Binary out{};
    out.nodeD8 = nodeD8;
    out.nodeD0 = nodeD0;
    out.valid  = (selfValid && nodeValid) ? 1u : 0u;
    out.bothZero = (out.valid && nodeD8 == 0 && nodeD0 == 0) ? 1u : 0u;
    return out;
}

}  // namespace nred
