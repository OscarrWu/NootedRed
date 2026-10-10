// FwFirmwareAsset.hpp —— A-2：固件资产完整性校验（header-only，kext + 用户态双环境可编译）
//
// 职责：
//   ① 资产描述（数据指针 + 大小 + 期望哈希）；
//   ② FNV-1a 64-bit 哈希（离线实测常量，见 A-2 报告 §固件来源）；
//   ③ fwAssetVerify：大小非空 + 哈希一致 ⇒ true；缺失/损坏 ⇒ false（**可判失败，不静默通过**）。
//
// 本文件**不含 `#embed`**（g++ 12 不支持）：内嵌数组由 kext 侧（NRedFwBringupHook.hpp）提供，
// 本文件只做纯逻辑校验，故可被用户态单测（g++）直接包含。
//
// 约束：header-only、零动态分配、无异常；禁 `<cstdint>`/`<cstddef>`/`std::`
//       （kext 构建环境无 libc++，CI run #220 纪律）；用 `<stdint.h>` + 全局类型名。

#pragma once

#include <stdint.h>

namespace fw {

// FNV-1a 64-bit（完整性校验哈希；固定初值与素数，见 FNV 规范）
inline uint64_t fnv1a64(const uint8_t* data, const uint32_t size)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint32_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

// 固件资产描述：数据 + 大小 + 期望哈希（期望哈希 = 离线对源文件的 fnv1a64 实测值）。
struct FwAssetDesc {
    const uint8_t* data;
    uint32_t       size;
    uint64_t       expectedHash64;
};

// 完整性校验：
//   缺失（data==nullptr / size==0）⇒ false；
//   损坏（哈希不一致）⇒ false；
//   完整 ⇒ true。
// 调用方（hook）在 false 时必须**跳过装载**并 trace（不得静默通过）。
inline bool fwAssetVerify(const FwAssetDesc& a)
{
    if (a.data == nullptr || a.size == 0) { return false; }
    return fnv1a64(a.data, a.size) == a.expectedHash64;
}

}  // namespace fw
