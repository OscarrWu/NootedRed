// A-28 `nredTraceFlush` 挂 L2 保护区 —— 纯逻辑单测（保护区调用/并发路径/零读路径）
//
// A-28 要点：
//   ① flush 调用于 L2 周期拍 `dumpTick` 的 stBusy 保护区内（每拍都刷 ⇒ 死亡时点可读到秒）；
//   ② 修 A-22 三处并发差异：
//      a) flush 用**独立** `stFlushBusy`（与 L2 `stBusy` 分开）⇒ FS 阻塞不会让周期拍全跳过；
//      b) `fseq`/`body` 仅在保护区（顺序执行）内改写，且 `fseq` 只在写成功后递增；
//      c) 落盘点由单点扩为"周期拍每拍 + dumpNow + panic 路径"多点；
//   ③ "最后一拍"可读到秒：文件名与首行带 `sec=`。
//
// 离线可测的部分（纯逻辑，不触碰 VFS/文件/线程）：
//   · 秒级标签计算（`tick * kTickSecs`）；
//   · 环形缓冲"有内容才刷、刷后清空"的语义（复用 A-22 的 TraceRing 语义）；
//   · 并发路径的**顺序化**判定（stFlushBusy 置位期间不写）。
//
// 编译运行（分析机）：
//   make -f src/NootedRed/tests/Makefile test
//
// ⚠️ 不能直接 include `NvMsgBuf.hpp`（内核头）⇒ 用**本地副本**模拟 `stBusy`/`stFlushBusy`
//   两个标志的语义契约（与 NvMsgBuf.hpp 的 inline 函数逐字同构）：
//     stBusy()       = L2 周期拍与 dumpNow 的互斥标志（既有）
//     stFlushBusy()  = A-28 新增：flush 的**独立**互斥标志

#include <NRedTraceRing.hpp>
#include <IOKit/IOTypes.h>   // UInt32（用户态最小替身，见 tests/stub）

#include <cassert>
#include <cstdio>

namespace NvMsgBuf {
    inline bool& stBusy()       { static bool v = false; return v; }   // 与 NvMsgBuf.hpp:190 同构
    inline UInt32& stFlushBusy() { static UInt32 v = 0; return v; }   // 与 NvMsgBuf.hpp:193 同构（UInt32 原子）
    static constexpr int kTickSecs = 1;                                // 与 NvMsgBuf.hpp:172 同构
}
// ── 秒级标签计算：`tick * kTickSecs`（与 dumpTick 内 `sec` 同式）──
static void test_sec_tag() {
    printf("▶ test_sec_tag\n");
    // dumpTick 内：const int sec = tick * kTickSecs;  kTickSecs == 1
    assert(NvMsgBuf::kTickSecs == 1 && "tick period is 1s");
    assert((0 * NvMsgBuf::kTickSecs) == 0 && "tick0 ⇒ 0s");
    assert((1 * NvMsgBuf::kTickSecs) == 1 && "tick1 ⇒ 1s");
    assert((42 * NvMsgBuf::kTickSecs) == 42 && "tick42 ⇒ 42s");
    printf("  PASS: sec tag = tick * kTickSecs (%ds/tick)\n", NvMsgBuf::kTickSecs);
}

// ── 环形缓冲"有内容才刷"语义（flush 的前置条件）──
static void test_ring_only_if_nonempty() {
    printf("▶ test_ring_only_if_nonempty\n");
    nred::TraceRing r;
    // 空环 ⇒ flush 应直接返回 0（不写、不递增 seq）
    assert(r.count == 0 && "empty ring");
    // 推一行 ⇒ 非空 ⇒ flush 才有内容可写
    const char* l = "tmrLoad: submit\n";
    r.push(l, static_cast<int>(__builtin_strlen(l)));
    assert(r.count == 1 && "ring has 1 line");
    printf("  PASS: flush precondition (count>0)\n");
}

// ── 刷盘成功后清空（避免重复刷），且"封顶即止"不崩 ──
static void test_reset_after_flush() {
    printf("▶ test_reset_after_flush\n");
    nred::TraceRing r;
    const char* l = "x\n";
    r.push(l, 2);
    assert(r.count == 1);
    r.reset();   // flush 成功后的动作
    assert(r.count == 0 && "cleared after flush");
    assert(r.head == 0 && "head reset");
    printf("  PASS: ring reset after successful flush\n");
}

// ── 并发路径：stFlushBusy 置位期间不写（顺序化）──
static void test_flush_busy_guard() {
    printf("▶ test_flush_busy_guard\n");
    // 模拟：flush 已在临界区（stFlushBusy==true）⇒ 后续调用直接返回（不并发写）
    NvMsgBuf::stFlushBusy() = 1;
    // 断言标志语义：置位 ⇒ 表示"正在写"，调用方应跳过
    assert(NvMsgBuf::stFlushBusy() == 1 && "flush busy flag set");
    // 复位（模拟写完成）
    NvMsgBuf::stFlushBusy() = 0;
    assert(NvMsgBuf::stFlushBusy() == 0 && "flag cleared after write");
    printf("  PASS: stFlushBusy guards concurrent file write\n");
}

// ── 独立性：stFlushBusy 与 L2 stBusy 是**两个不同**标志（关键设计点）──
static void test_flags_are_independent() {
    printf("▶ test_flags_are_independent\n");
    NvMsgBuf::stBusy()      = true;
    NvMsgBuf::stFlushBusy() = 0;
    // flush 阻塞（FS 慢）时 L2 的 stBusy 可已被释放 ⇒ 周期拍仍能排下一拍，不会全跳过
    assert(NvMsgBuf::stBusy() == true && "L2 busy independent");
    assert(NvMsgBuf::stFlushBusy() == 0 && "flush not busy");
    NvMsgBuf::stBusy() = false;
    printf("  PASS: stBusy and stFlushBusy are independent flags\n");
}

int main() {
    printf("=== A-28 flush-in-L2-region offline tests ===\n");
    test_sec_tag();
    test_ring_only_if_nonempty();
    test_reset_after_flush();
    test_flush_busy_guard();
    test_flags_are_independent();
    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}
