// 用户态测试用 <IOKit/IOTypes.h> 最小替身：
// 仅提供 Regs/*.hpp、GPUDriversAMD/*.hpp 实际用到的 typedef，
// 使这些 constexpr 常量头可在分析机（Linux, 无 macOS SDK）上直接包含。
// 语义与 macOS IOKit IOTypes.h 一致：固定宽度无符号整型。
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once

typedef unsigned char UInt8;
typedef unsigned int  UInt32;
