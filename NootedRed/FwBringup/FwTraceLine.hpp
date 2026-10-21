// FwTraceLine.hpp —— FwBringup 专用 trace 通道（232 栈修正）
//
// 背景（R-231 双重故障定因）：`NRED_TRACE` 宏在每个使用点声明 `char _tb[256]`；FwBringup 内有
//   7+ 处 ⇒ `nredFwBringupHook` 帧被推到 9152 B，叠加调用链已用的 9.6 KB 超过 16 KB 内核栈，
//   触发 Double fault。本文件提供 `fw::fwTrace(fmt, ...)`：用**单个 static 行缓冲**格式化后直连
//   `nredTraceLine`（内存环），**不在每个调用点占栈**，也不走 `SYSLOG`（其内部 `lilu_os_log`
//   另有约 1 KB 栈；bringup 期间 SYSLOG 本就被 `NredSyslogMuteGuard` 静音）。
//
// 使用方式：在 FwBringup 的头文件里用 `#pragma push_macro("NRED_TRACE")` ＋ 本地 `#define
//   NRED_TRACE(...) fwTrace(__VA_ARGS__)` 包住函数定义，末尾 `#pragma pop_macro("NRED_TRACE")`
//   ⇒ 不改全局宏本体、不影响 HWLibs.cpp 的其它翻译单元区域、调用点零改动、输出格式不变。
//
// ⚠ 一次性/单线程前提：bringup 由 `bringupAttempted` 静态守卫保证每 boot 只执行一次；
//   缓冲区不可重入（若将来改为多线程，需换成 per-thread 或传缓冲区）。
// ⚠ 用户态测试（`FW_NO_NRED_TRACE`）：`fwTrace` 为空实现，不依赖 nredTraceLine。
#pragma once

#include <stdarg.h>

#ifdef FW_NO_NRED_TRACE

namespace fw {
inline void fwTrace(const char* /*fmt*/, ...) {}
}

#else

#include "../HWLibs.hpp"   // nredTraceLine（内存环；零 FS、零 os_log）

namespace fw {

// 单份行缓冲：236 轮修复——由 192 B 扩到 256 B（`[bgmtab]` 满行含 sel/size/ra/raoff ≈180 字符，
//  加上多字段后接近 192 上限；256 B 留足 NUL/换行余量，避免被截断）。
static char gFwTraceBuf[256];

__attribute__((format(printf, 1, 2)))
inline void fwTrace(const char* const fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(gFwTraceBuf, sizeof(gFwTraceBuf), fmt, ap);
    va_end(ap);
    if (n <= 0) { return; }
    if (n > static_cast<int>(sizeof(gFwTraceBuf)) - 2) { n = static_cast<int>(sizeof(gFwTraceBuf)) - 2; }
    gFwTraceBuf[n] = '\n';
    nredTraceLine(gFwTraceBuf, n + 1);
}

}  // namespace fw

#endif
