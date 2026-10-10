// test_seg_readout.cpp —— D-3 只读仪表离线单测（g++ 12 可编译：FW_SEG_READOUT_NO_KEXT）
//
// 覆盖：
//   ① smnAddrWithBase 双段等价（SEG0/SEG1 字节地址差恒定）；
//   ② runSegReadout 只读序列：双段读数（seg0/seg1/diff/seg1Invalid）、直读、PB 状态位；
//   ③ 判据：SEG1 读数全 1 ⇒ seg1Invalid=1（SEG1 未使能）；
//   ④ 只读性：无写（回调仅读）。
//
// 编译运行：
//   make -f src/NootedRed/FwBringup/tests/Makefile test

#define FW_SEG_READOUT_NO_KEXT
#include "FwBringup/NRedSegReadout.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace fw;

// ── Mock 存根：固定容量数组（避免 std::vector placement new 问题）──
struct ReadStub {
    uint64_t addr;
    uint32_t value;
    uint32_t timedOut;
};

static ReadStub gStubs[64];
static size_t   gStubCount = 0;
static uint64_t gReadAddrs[64];
static size_t   gReadCount = 0;

static fw::SegReadPoint stubRead(uint64_t byteAddr)
{
    if (gReadCount < 64) { gReadAddrs[gReadCount++] = byteAddr; }
    for (size_t i = 0; i < gStubCount; ++i) {
        if (gStubs[i].addr == byteAddr) { return {gStubs[i].value, gStubs[i].timedOut}; }
    }
    return {0xFFFFFFFFu, 1u};   // 未预置 ⇒ 全 1（读取失败语义），timedOut=1
}

static void resetStubs()
{
    gStubCount = 0;
    gReadCount = 0;
}

static void addStub(uint64_t addr, uint32_t val, uint32_t timedOut = 0)
{
    if (gStubCount < 64) { gStubs[gStubCount++] = {addr, val, timedOut}; }
}

int main()
{
    printf("[test] smnAddrWithBase dual-segment equivalence... ");
    // 公式与差恒定
    assert(smnAddrWithBase(kMpSeg0Base, 0x9B) == (0x00016000u + 0x9B) * 4);
    assert(smnAddrWithBase(kMpSeg1Base, 0x9B) == (0x0243FC00u + 0x9B) * 4);
    assert(smnAddrWithBase(kMpSeg1Base, 0x9B) - smnAddrWithBase(kMpSeg0Base, 0x9B)
           == (kMpSeg1Base - kMpSeg0Base) * 4);
    assert(smnAddrWithBase(kMpSeg1Base, 0x9B) - smnAddrWithBase(kMpSeg0Base, 0x9B) == 0x90A7000u);
    assert(smnAddrWithBase(kMpSeg0Base, 0x9B) == 0x0005826Cu);
    assert(smnAddrWithBase(kMpSeg1Base, 0x9B) == 0x090FF26Cu);
    printf("PASS\n");

    printf("[test] runSegReadout dual-segment readout... ");
    // 预置：SEG0 读数（C2PMSG_91/83/FW_FLAGS）与 SEG1 读数（全 1 ⇒ SEG1 未使能判据）
    resetStubs();
    addStub(smnAddrWithBase(kMpSeg0Base, 0x9B),  0xDEADBEEF, 0);
    addStub(smnAddrWithBase(kMpSeg1Base, 0x9B),  0xFFFFFFFF, 1);  // SEG1 超时
    addStub(smnAddrWithBase(kMpSeg0Base, 0x293), 0x12345678, 0);
    addStub(smnAddrWithBase(kMpSeg1Base, 0x293), 0xFFFFFFFF, 1);  // SEG1 超时
    addStub(0x6B10024u, 0xFFFFFFFF, 1);   // FW_FLAGS 绝对地址 0x6B10024（A-10 (a)）：SEG0/SEG1 均超时
    addStub(smnAddrWithBase(kMpSeg1Base, 0x29B), 0x0, 0);   // C2PMSG_91 MP1 直读

    const SegReadoutReadings r = runSegReadout(&stubRead, kMpSeg0Base, kMpSeg1Base);

    // 双段读数：seg0 有效、seg1 超时 ⇒ seg1Invalid=1
    assert(r.dualSeg0.seg0Val == 0xDEADBEEF);
    assert(r.dualSeg0.seg1Val == 0xFFFFFFFF);
    assert(r.dualSeg0.seg0TimeOut == 0u);
    assert(r.dualSeg0.seg1TimeOut == 1u);
    assert(r.dualSeg0.seg1Invalid == 1u);          // SEG1 未使能判据成立
    assert(r.dualSeg0.diff == (0xDEADBEEF ^ 0xFFFFFFFF));

    assert(r.dualSeg1.seg0Val == 0x12345678);
    assert(r.dualSeg1.seg1Val == 0xFFFFFFFF);
    assert(r.dualSeg1.seg0TimeOut == 0u);
    assert(r.dualSeg1.seg1TimeOut == 1u);
    assert(r.dualSeg1.seg1Invalid == 1u);

    // A-10 (a)：FW_FLAGS 绝对地址 0x6B10024
    assert(r.fwFlags.value == 0xFFFFFFFF);
    assert(r.fwFlags.timedOut == 1u);

    // 直读
    assert(r.c2pmsg91Mp0.value == 0xDEADBEEF);
    assert(r.c2pmsg91Mp0.timedOut == 0u);
    assert(r.c2pmsg83Mp1.value == 0xFFFFFFFF);
    assert(r.c2pmsg83Mp1.timedOut == 1u);
    assert(r.c2pmsg91Mp1.value == 0x0);
    assert(r.c2pmsg91Mp1.timedOut == 0u);

    // PB 状态位
    assert(r.fwFlags.value == 0xFFFFFFFF);
    assert(r.fwFlags.timedOut == 1u);
    printf("PASS\n");

    printf("[test] runSegReadout seg1-invalid when seg1==seg0... ");
    // 预置：SEG0 与 SEG1 读数相同且非 0 ⇒ seg1Invalid=1（同值非 0 判据）
    resetStubs();
    addStub(smnAddrWithBase(kMpSeg0Base, 0x9B), 0xCAFEBABE, 0);
    addStub(smnAddrWithBase(kMpSeg1Base, 0x9B), 0xCAFEBABE, 0);
    const SegReadoutReadings r2 = runSegReadout(&stubRead, kMpSeg0Base, kMpSeg1Base);
    assert(r2.dualSeg0.seg1Invalid == 1u);
    printf("PASS\n");

    printf("=== ALL SEG READOUT UNIT TESTS PASSED ===\n");
    return 0;
}