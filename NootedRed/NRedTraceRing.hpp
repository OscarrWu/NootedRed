// NRedTraceRing.hpp —— A-22 早期 trace 走"内存通道"（自建静态环形缓冲）
//
// 目的：根治"bringup 早期调用 APFS 写文件不安全"——技术组长背景（原话）：
//  「B7/B10 panic 栈**确实**经过 `nredTraceLine`→`FileIO::writeBufferToFile`→**APFS btree
//   lookup** ⇒ 当时 `rootvnode` 非空（A-20 检查会放行）⇒ 真正的结论是：在 bringup 早期
//   调用"APFS 写文件"本身不安全（即便 rootvnode 非空）⇒ 加检查不解决；必须改"落地方式"」。
//
// 设计：
//   · 早期路径（bringupRun/tmrInit/tmrLoad/hook 内早期段）的一切 trace **只写内存环形缓冲**，
//     **绝不**调 `FileIO::writeBufferToFile`（本文件本身也**不 include** kern_file.hpp）。
//   · 在**既有安全时点**（`NRed::init` / `NvMsgBuf::dumpNow` / panic 分片路径）再**刷盘一次**，
//     与既有 L2 通道（`-NRedObserveDisk` 读 msgbuf 按拍落盘）**同构**。
//   · 行同时经 `SYSLOG` 进内核 console 环形缓冲（NRED_TRACE 已含 SYSLOG）⇒ **双通道冗余**
//     （内存缓冲 + 内核 console），panic 分片可带出后者。
//   · 保留 A-20 的 `dropped`/`written` 计数（内存侧即可）。
//
// 边界：本文件只做**内存环形缓冲 + 计数**（纯逻辑 + 静态存储），**不触碰 VFS/文件/锁**；
//   刷盘由调用方（kext 侧安全时点）执行 `flush()`。
//
// ⛔ kext 环境可编译：禁 `<cstdint>`/`<cstddef>`/`std::`（CI run #220 纪律）；用 `<stdint.h>`。
// ⛔ 无动态分配、无锁（单写者假设：早期路径顺序执行；多写者场景由调用方保证）。

#pragma once

#include <stdint.h>

namespace nred {

// ── 早期 trace 内存环形缓冲（静态，无分配）──
//  容量：kLineMax 行 × kLineBytes 字节/行。溢出即丢弃最旧（环形），并计入 overflow。
struct TraceRing {
    static constexpr uint32_t kLineMax   = 256;   // 最大行数
    static constexpr uint32_t kLineBytes = 192;   // 每行最大字节（含换行，截断到 kLineBytes-1 + '\n'）

    char     lines[kLineMax][kLineBytes];
    uint32_t head{0};        // 下一个写入槽位
    uint32_t count{0};       // 当前有效行数（<= kLineMax）
    uint32_t written{0};     // 累计写入行数（A-20 计数，内存侧）
    uint32_t dropped{0};     // 累计丢弃行数（n<=0 或缓冲满）
    uint32_t overflow{0};    // 环形溢出次数（覆盖旧行）

    // 追加一行（只写内存；n<=0 或超长截断）。返回 true=已入缓冲。
    bool push(const char* const buf, const int n)
    {
        if (n <= 0 || buf == nullptr) { ++dropped; return false; }
        uint32_t len = static_cast<uint32_t>(n);
        if (len > kLineBytes - 1) { len = kLineBytes - 1; }   // 截断，保留 '\n' 位
        char* const dst = lines[head];
        for (uint32_t i = 0; i < len; ++i) { dst[i] = buf[i]; }
        // 确保以换行结束（判读侧按行切分）
        if (len == 0 || dst[len - 1] != '\n') { dst[len] = '\n'; ++len; }
        head = (head + 1) % kLineMax;
        if (count < kLineMax) { ++count; } else { ++overflow; }
        ++written;
        return true;
    }

    // 按序取第 i 行（i: 0..count-1；最旧 → 最新）。返回长度（不含 NUL），行首指针存于 out。
    //  注：环形 ⇒ 最旧槽 = (head - count + kLineMax) % kLineMax。
    uint32_t get(const uint32_t i, const char** const out) const
    {
        if (out == nullptr || i >= count) { return 0; }
        const uint32_t oldest = (head + kLineMax - count) % kLineMax;
        const uint32_t slot   = (oldest + i) % kLineMax;
        *out = lines[slot];
        uint32_t len = 0;
        while (len < kLineBytes && lines[slot][len] != '\0' && lines[slot][len] != '\n') { ++len; }
        return len;   // 不含换行（调用方自行加 "\n"）
    }

    // 清空（刷盘成功后可选调用，避免重复刷）
    void reset()
    {
        head = 0; count = 0;
    }
};

}  // namespace nred
