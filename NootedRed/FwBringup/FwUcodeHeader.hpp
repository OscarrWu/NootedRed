// 固件文件头结构体（AMD linux-firmware `.bin` 二进制格式）
//
// 本文件是**纯格式定义**，不含任何内核/DriverKit 依赖（只用 <stdint.h>），
// 因此可在 kext 与 Linux 用户态离线测试中同样编译。
//
// 来源（行号为 2026-09-30 离线核对）：
//   - srcs/mac-amdgpu-ref/dext/amdgpu/amdgpu_ucode_psp.h:25-359
//     （该文件自述为 upstream amdgpu_ucode.h:30-148 / amdgpu_psp.h:93-107 的 1:1 vendored 副本）
//   - 对应上游语义复核：srcs/linux-amdgpu-ref/drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c
//       · psp_init_toc_microcode   :3986-4008（TOC payload = data + ucode_array_offset_bytes，长 ucode_size_bytes）
//       · parse_ta_v2_microcode    :4414-4440（TA v2.0：common + ta_fw_bin_count + psp_fw_bin_desc[]）
//       · parse_ta_bin_descriptor  :4294-4357（子固件地址 = ta_hdr + desc.offset_bytes + header.ucode_array_offset_bytes）
//   - ta_fw_type / psp_fw_type 枚举值复核：
//       · TA_FW_TYPE_PSP_ASD = 1（amdgpu_ucode_psp.h:135-146）
//       · parse_ta_bin_descriptor 的 switch 分支（amdgpu_psp.c:4310-4354）
//
// ⚠️ 移植纪律：
//   - 本文件**只定义乙线当前用到的结构**（common / psp_fw_bin_desc / psp_fw_type /
//     psp_firmware_header_v2_0 / ta_fw_type / ta_firmware_header_v2_0），不是
//     amdgpu_ucode_psp.h 的全量照抄（全量含 rlc_v2_x / mes / imu / sdma_v3 / gfx_v2 等
//     乙线尚未装载的 IP 头，等 T? 装载到该 IP 时再按需补，并同样标注来源行号）。
//   - 命名空间从 `amdgpu` 改为 `fw`（乙线约定）。
//   - 字段顺序/宽度与上游逐字一致；结构体 `__attribute__((packed))` 保留（固件文件
//     是磁盘字节布局，编译器不得插入填充）。
//
// 约束：header-only、无动态分配、无异常、无文件 IO。

#pragma once

#include <stdint.h>

namespace fw {

// ---------------------------------------------------------------------------
// 通用固件头（所有 AMD `.bin` 的前 32 字节）
//   来源：amdgpu_ucode_psp.h:25-38（upstream amdgpu_ucode.h:30-41）
// ---------------------------------------------------------------------------
struct CommonFwHeader {
    uint32_t size_bytes;                 // 整个文件大小
    uint32_t header_size_bytes;          // 头结构体大小
    uint16_t header_version_major;       // 1 或 2
    uint16_t header_version_minor;
    uint16_t ip_version_major;           // 例：psp_v13 ⇒ 13
    uint16_t ip_version_minor;
    uint32_t ucode_version;
    uint32_t ucode_size_bytes;           // payload 区大小（字节）
    uint32_t ucode_array_offset_bytes;   // payload 相对**头起始**的偏移（字节）
    uint32_t crc32;
} __attribute__((packed));

static_assert(sizeof(CommonFwHeader) == 32, "CommonFwHeader must be 32 bytes");

// ---------------------------------------------------------------------------
// PSP v2 带类型标记的子固件描述符（psp_v13+ / RDNA3 起使用，含 13.0.4）
//   来源：amdgpu_ucode_psp.h:87-94
// ---------------------------------------------------------------------------
struct PspFwBinDesc {
    uint32_t fw_type;        // PspFwType 枚举
    uint32_t fw_version;
    uint32_t offset_bytes;   // 相对 header.ucode_array_offset_bytes 的偏移
    uint32_t size_bytes;
} __attribute__((packed));

static_assert(sizeof(PspFwBinDesc) == 16, "PspFwBinDesc must be 16 bytes");

// PSP 子固件类型（amdgpu_ucode_psp.h:96-111）
enum PspFwType : uint32_t {
    kPspFwTypeUnknown       = 0,
    kPspFwTypePspSos        = 1,
    kPspFwTypePspSysDrv     = 2,
    kPspFwTypePspKdb        = 3,
    kPspFwTypePspToc        = 4,
    kPspFwTypePspSpl        = 5,
    kPspFwTypePspRl         = 6,
    kPspFwTypePspSocDrv     = 7,
    kPspFwTypePspIntfDrv    = 8,
    kPspFwTypePspDbgDrv     = 9,
    kPspFwTypePspRasDrv     = 10,
    kPspFwTypePspIpkeymgrDrv = 11,
    kPspFwTypePspSpdmDrv    = 12,
    kPspFwTypeMaxIndex,
};

// PSP 固件头 v2.0（header_version_major == 2, minor == 0）
//   来源：amdgpu_ucode_psp.h:114-118
//   ⚠️ psp_fw_bin[] 为柔性数组；本文件不声明柔性数组（kext 侧 C++17 与 -Werror
//     对 `T x[]` 的支持面不统一），解析时由 fw 命名空间的 `pspFwBinDescAt` 按
//     字节偏移取第 i 个描述符（见 FwUcodeParse.hpp）。
struct PspFirmwareHeaderV2_0 {
    CommonFwHeader header;
    uint32_t       psp_fw_bin_count;
    // PspFwBinDesc psp_fw_bin[];
} __attribute__((packed));

static_assert(sizeof(PspFirmwareHeaderV2_0) == 36, "PspFirmwareHeaderV2_0 must be 36 bytes");

// ---------------------------------------------------------------------------
// TA（Trusted Application）固件头 v2.0 —— psp_<chip>_ta.bin
//   来源：amdgpu_ucode_psp.h:150-154
//   注释明示：与 psp_firmware_header_v2_0 **字节布局相同**，仅描述符的 fw_type
//   取值来自 ta_fw_type（:128-133；上游遍历点 amdgpu_psp.c:4414-4440）。
// ---------------------------------------------------------------------------
struct TaFirmwareHeaderV2_0 {
    CommonFwHeader header;
    uint32_t       ta_fw_bin_count;
    // PspFwBinDesc ta_fw_bin[];
} __attribute__((packed));

static_assert(sizeof(TaFirmwareHeaderV2_0) == 36, "TaFirmwareHeaderV2_0 must be 36 bytes");

// TA 子固件类型（amdgpu_ucode_psp.h:135-146）
enum TaFwType : uint32_t {
    kTaFwTypeUnknown        = 0,
    kTaFwTypePspAsd         = 1,   // ← 本机 psp_13_0_4_ta.bin desc[0] 实测为此类型
    kTaFwTypePspXgmi        = 2,
    kTaFwTypePspRas         = 3,
    kTaFwTypePspHdcp        = 4,
    kTaFwTypePspDtm         = 5,
    kTaFwTypePspRap         = 6,
    kTaFwTypePspSecureDisplay = 7,
    kTaFwTypePspXgmiAux     = 8,
    kTaFwTypeMaxIndex,
};

// ---------------------------------------------------------------------------
// 按字节偏移的安全读取工具（避免在未对齐缓冲上直接 reinterpret_cast）
//
// 固件缓冲来自文件系统/请求固件接口，不保证 4 字节对齐；且 kext 侧不允许
// 引入 memcpy 之外的假设。这里用逐字节组装（小端，与 AMD 固件文件一致）。
// ---------------------------------------------------------------------------

/// 从 `base` 的字节偏移 `off` 处读一个小端 uint32
/// @return false = 越界（off + 4 > size）
inline bool readU32Le(const uint8_t* base, uint32_t size, uint32_t off, uint32_t* out) {
    if (!base || !out) return false;
    if (off > size || size - off < 4) return false;
    const uint8_t* p = base + off;
    *out = static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
    return true;
}

/// 从 `base` 的字节偏移 `off` 处读一个小端 uint16
inline bool readU16Le(const uint8_t* base, uint32_t size, uint32_t off, uint16_t* out) {
    if (!base || !out) return false;
    if (off > size || size - off < 2) return false;
    const uint8_t* p = base + off;
    *out = static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                 (static_cast<uint16_t>(p[1]) << 8));
    return true;
}
inline bool parseCommonFwHeader(const uint8_t* data, uint32_t size, CommonFwHeader* out) {
    if (!data || !out || size < sizeof(CommonFwHeader)) return false;
    uint32_t t0 = 0, t1 = 0, t4 = 0, t5 = 0, t6 = 0, t7 = 0;
    uint16_t t2 = 0, t3 = 0, t8 = 0, t9 = 0;
    bool ok = true;
    ok &= readU32Le(data, size, 0,  &t0);
    ok &= readU32Le(data, size, 4,  &t1);
    ok &= readU16Le(data, size, 8,  &t8);
    ok &= readU16Le(data, size, 10, &t9);
    ok &= readU16Le(data, size, 12, &t2);
    ok &= readU16Le(data, size, 14, &t3);
    ok &= readU32Le(data, size, 16, &t4);
    ok &= readU32Le(data, size, 20, &t5);
    ok &= readU32Le(data, size, 24, &t6);
    ok &= readU32Le(data, size, 28, &t7);
    if (ok) {
        out->size_bytes              = t0;
        out->header_size_bytes       = t1;
        out->header_version_major    = t8;
        out->header_version_minor    = t9;
        out->ip_version_major        = t2;
        out->ip_version_minor        = t3;
        out->ucode_version           = t4;
        out->ucode_size_bytes        = t5;
        out->ucode_array_offset_bytes = t6;
        out->crc32                   = t7;
    }
    return ok;
}

/// 取 PSP/TA v2.0 头的第 `index` 个子固件描述符
///
/// 布局：CommonFwHeader(32) + bin_count(4) + PspFwBinDesc(16) × count
/// 上游取法：`&ta_hdr->ta_fw_bin[ta_index]`（amdgpu_psp.c:4432-4434）。
/// @return false = index 越界或缓冲太小
inline bool pspFwBinDescAt(const uint8_t* data, uint32_t size, uint32_t index,
                           PspFwBinDesc* out) {
    if (!data || !out) return false;
    // 防御：size < 36（连 count 字段都没有）时直接拒绝；随后 index 有上界，
    // 36 + index*16 不可能溢出（index ≤ (size-36)/16 ≤ 2^28）
    if (size < 36) return false;
    if (index > (size - 36) / static_cast<uint32_t>(sizeof(PspFwBinDesc))) return false;
    const uint32_t off = 32 + 4 + index * static_cast<uint32_t>(sizeof(PspFwBinDesc));
    uint32_t t0 = 0, t1 = 0, t2 = 0, t3 = 0;
    bool ok = true;
    ok &= readU32Le(data, size, off + 0,  &t0);
    ok &= readU32Le(data, size, off + 4,  &t1);
    ok &= readU32Le(data, size, off + 8,  &t2);
    ok &= readU32Le(data, size, off + 12, &t3);
    if (ok) {
        out->fw_type      = t0;
        out->fw_version   = t1;
        out->offset_bytes = t2;
        out->size_bytes   = t3;
    }
    return ok;
}

} // namespace fw
