// NRedTraceSafe.hpp —— A-20 早期落盘安全前置检查（纯逻辑，用户态可测）
//
// 目的：根治"早期 trace 写入导致 Double fault"——根 FS（`rootvnode`）未挂载时
//   `FileIO::writeBufferToFile` 会阻塞内核线程（真机手册 §5.1）⇒ 早期落盘必须**先判后写**，
//   未就绪即**跳过、失败即退、不重试**，并**计数**以便事后得知丢了多少行。
//
// 依据：
//   - 手册既有条目「`NRedTrace` 需 `rootvnode`」（`HWLibs.hpp:21-23`）；
//   - L2 通道已有"每拍先判 `rootvnode != nullptr`"的先例（`HWLibs.cpp` L2 落盘）；
//   - 本卡把同一纪律补到 **trace 落盘通道**（`nredTraceLine`）与早期 PP 通道（`nredPPTrace`）。
//
// 边界：本文件只做**判定与计数**（纯逻辑），**不触碰内核/VFS**；
//   实际写文件由调用方在 `shouldWrite()` 返回真之后执行（kext 侧）。
//
// ⛔ kext 环境可编译：禁 `<cstdint>`/`<cstddef>`/`std::`（CI run #220 纪律）；用 `<stdint.h>`。

#pragma once

#include <stdint.h>

namespace nred {

// ── 落盘安全性判定 + 丢弃/写入计数 ──
struct TraceSafe {
    uint32_t dropped{0};   // 丢弃行数（rootvnode 未就绪，或 n<=0）
    uint32_t written{0};   // 已写入行数

    // @param rootvnodeReady 根 FS 是否已挂载（kext 侧：`rootvnode != nullptr`）
    // @param n              待写字节数
    // @return true = 安全可写（调用方执行写）；false = 跳过（计数已累加，调用方绝不能写）
    bool shouldWrite(const bool rootvnodeReady, const int n)
    {
        if (n <= 0 || !rootvnodeReady) {
            ++dropped;
            return false;   // 失败即退、不重试
        }
        ++written;
        return true;
    }
};

}  // namespace nred
