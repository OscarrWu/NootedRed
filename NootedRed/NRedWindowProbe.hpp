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

// ── 单个水印读数 ──
struct WmVal {
    uint32_t id;      // WmId
    uint64_t value;   // 读到的值
    uint32_t valid;   // 1 = 已读到；0 = 未读到（指针非法，值无意义）
};

// ── first-false 判定结果 ──
struct WindowResult {
    uint32_t firstFalseId;    // 第一个"空"的水印（WmCount = 无空，全部非空）
    uint32_t firstFalseValid; // 1 = 判定有效（该水印确实读到了且值为 0）
    uint32_t readCount;       // 成功读到的水印数
    uint32_t unknownCount;    // 未能读到的水印数（valid==0）
};

// ── P5 时序二值判结果 ──
struct P5Binary {
    uint64_t nodeD8;    // *(node+0xD8)（node = *(self+0x50)）
    uint64_t nodeD0;    // *(node+0xD0)
    uint32_t valid;     // 1 = self/node 指针均合法且已读；0 = 未读到（值无意义）
    uint32_t bothZero;  // 1 = nodeD8==0 && nodeD0==0（P5 后半在 S1 时刻为空）
};

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
