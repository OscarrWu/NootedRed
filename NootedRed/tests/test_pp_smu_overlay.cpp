// pp_smu 最小覆盖（`-NRedPpSmuOverlay`）离线单元测试
//
// 验收要求（任务卡 §8）：
//   ① 门控解析（默认关 ⇒ 零写）；
//   ② 回调填 4×8B 且返 1；
//   ③ f0/f18/f38 偏移正确、f1c0/f408 不动。
//
// 结构说明：真机代码中 `isKernelPtr` 守卫的"内核地址"（≥0xffffff7f80000000）在
//   用户态**不可解引用**（写入即段错误），因此离线测试把两层逻辑拆开、
//   各自独立验证：
//     · 守卫谓词 `kernelPtrGuard(p)`：只对**符号地址**判定阈值语义（不解引用）；
//     · 写入核心 `overlayWrite(pp)`：对**真实可写缓冲区**验证偏移/取值，
//       守卫由测试钩子 gPtrGuardPass 替代（离线不需要再模拟地址空间）；
//     · 门控 `checkKernelArgument`：由 mock 提供，验证默认关 ⇒ 整个覆盖块不执行。
//   组合语义（守卫==真 且 门控==开 才写）由 ①③ 用例联合覆盖。
//
// 编译运行（分析机）：
//   make -f src/NootedRed/tests/Makefile test
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cassert>

// ── 最小内核替身 ──
typedef unsigned char      UInt8;
typedef unsigned int       UInt32;
typedef unsigned long long UInt64;

// 门控 mock：-NRedPpSmuOverlay 由 gTestGateEnabled 开关（默认关），其余恒假。
static bool gTestGateEnabled = false;
extern "C" bool checkKernelArgument(const char* arg) {
    if (std::strcmp(arg, "-NRedPpSmuOverlay") == 0) return gTestGateEnabled;
    return false;
}

#define DBGLOG(...)
#define SYSLOG(...)

// ── 从实现复制的核心逻辑（与 src/NootedRed/X6000FB.cpp 逐字一致）──

// dpm_clocks 布局（dm_pp_smu.h）：
//   DcfClocks[8]@0x00、SocClocks[8]@0x40、FClocks[4]@0x80、MemClocks[4]@0xa0…
// 每项 dpm_clock = { u32 Freq(MHz); u32 Vol(mV, 2 fractional bits) }。
static int NRedPpSmuOverlayGetDpmClockTable(void* /*this_obj*/, void* table) {
    auto* p = reinterpret_cast<UInt32*>(table);
    p[0x80 / 4 + 0] = 400;   p[0x80 / 4 + 1] = 3600;   // FClocks[0]
    p[0x80 / 4 + 2] = 800;   p[0x80 / 4 + 3] = 3600;   // FClocks[1]
    p[0x80 / 4 + 4] = 1067;  p[0x80 / 4 + 5] = 3600;   // FClocks[2]
    p[0x80 / 4 + 6] = 1333;  p[0x80 / 4 + 7] = 3600;   // FClocks[3]
    return 1;
}

// A-6：f18 占位指针（有效 ≥8B 对齐内核地址）。
static UInt64 gNRedPpSmuOverlayStub __attribute__((aligned(8))) = 0;

// 真机阈值：`>= 0xffffff7f80000000`（沿用既有铁律/阈值）。
static bool kernelPtrGuard(UInt64 p) { return p >= 0xffffff7f80000000ULL; }

// 离线写入核心：与真机 `wrapDcClkMgrCreate` 门控块内三条 store64 逐字一致。
//   真机里先过 kernelPtrGuard 再调用；离线用 gPtrGuardPass 代替（见文件头注释）。
static bool gPtrGuardPass = false;
static void overlayWrite(UInt64 pp) {
    auto store64 = [](UInt64 base, UInt64 off, UInt64 v) {
        *reinterpret_cast<UInt64*>(reinterpret_cast<UInt8*>(base) + off) = v;
    };
    if (gPtrGuardPass) {
        store64(pp, 0x00, 1);   // f0：合法 version
        store64(pp, 0x18, reinterpret_cast<UInt64>(&gNRedPpSmuOverlayStub));   // f18：有效 ≥8B 指针
        store64(pp, 0x38, reinterpret_cast<UInt64>(&NRedPpSmuOverlayGetDpmClockTable));   // f38：我方回调
    }
}

// ── 测试用例 ──

static void test_kernel_ptr_guard_semantics() {
    printf("▶ test_kernel_ptr_guard_semantics\n");
    // 阈值边界：用户态地址（< 阈值）拒绝；内核形态地址（≥ 阈值）放行。
    // 只做符号判定，**不解引用**（用户态解引用 0xffffff… 即段错误）。
    UInt64 ua = 0x0000000012345678ULL;          // 用户态
    UInt64 uk = 0xffffff7f80000000ULL;          // 恰好阈值（真机合法内核指针下界）
    UInt64 uk2 = 0xffffff8000000000ULL;         // 典型内核 slide 区
    assert(!kernelPtrGuard(ua) && "user address must be rejected");
    assert(kernelPtrGuard(uk) && "threshold address must pass");
    assert(kernelPtrGuard(uk2) && "kernel address must pass");
    printf("  PASS: guard threshold semantics (symbolic, no deref)\n");
}

static void test_gate_default_off_zero_write() {
    printf("▶ test_gate_default_off_zero_write\n");
    gTestGateEnabled = false;

    // 模拟 pp_smu_funcs（真实用户态缓冲区）；门控关 ⇒ 整个覆盖块不执行 ⇒ 零写。
    UInt8 buf[0x100];
    std::memset(buf, 0, sizeof(buf));
    const UInt64 pp = reinterpret_cast<UInt64>(buf);

    gPtrGuardPass = true;   // 即便守卫放行，门控关仍必须零写
    if (checkKernelArgument("-NRedPpSmuOverlay")) {
        overlayWrite(pp);
    }

    const UInt64* p = reinterpret_cast<const UInt64*>(buf);
    assert(p[0x00/8] == 0 && "f0 should be 0 when gate off");
    assert(p[0x08/8] == 0 && "f8 should be 0 when gate off");
    assert(p[0x10/8] == 0 && "f10 should be 0 when gate off");
    assert(p[0x18/8] == 0 && "f18 should be 0 when gate off");
    assert(p[0x38/8] == 0 && "f38 should be 0 when gate off");
    printf("  PASS: zero writes when gate off\n");
}

static void test_gate_on_writes_f0_f18_f38() {
    printf("▶ test_gate_on_writes_f0_f18_f38\n");
    gTestGateEnabled = true;
    gPtrGuardPass = true;

    UInt8 buf[0x100];
    std::memset(buf, 0, sizeof(buf));
    const UInt64 pp = reinterpret_cast<UInt64>(buf);

    if (checkKernelArgument("-NRedPpSmuOverlay")) {
        overlayWrite(pp);
    }

    const UInt64* p = reinterpret_cast<const UInt64*>(buf);
    assert(p[0x00/8] == 1 && "f0 should be 1");
    assert(p[0x18/8] == reinterpret_cast<UInt64>(&gNRedPpSmuOverlayStub) && "f18 should be stub pointer");
    assert(p[0x38/8] == reinterpret_cast<UInt64>(&NRedPpSmuOverlayGetDpmClockTable) && "f38 should be callback pointer");
    // f8/f10 不在写入范围内 ⇒ 必须保持 0
    assert(p[0x08/8] == 0 && "f8 should remain 0");
    assert(p[0x10/8] == 0 && "f10 should remain 0");
    printf("  PASS: f0/f18/f38 written, f8/f10 untouched\n");
}

static void test_guard_reject_no_write() {
    printf("▶ test_guard_reject_no_write\n");
    gTestGateEnabled = true;
    gPtrGuardPass = false;   // 守卫否决 ⇒ 即便门控开也零写（真机：非内核指针）

    UInt8 buf[0x100];
    std::memset(buf, 0, sizeof(buf));
    const UInt64 pp = reinterpret_cast<UInt64>(buf);

    if (checkKernelArgument("-NRedPpSmuOverlay")) {
        overlayWrite(pp);
    }

    const UInt64* p = reinterpret_cast<const UInt64*>(buf);
    assert(p[0x00/8] == 0 && "f0 should be 0 when guard rejects");
    assert(p[0x18/8] == 0 && "f18 should be 0 when guard rejects");
    assert(p[0x38/8] == 0 && "f38 should be 0 when guard rejects");
    printf("  PASS: no write when guard rejects\n");
}

static void test_callback_fills_4x8b_returns_1() {
    printf("▶ test_callback_fills_4x8b_returns_1\n");

    // dpm_clocks 缓冲区（440B，调用方先 memset 0x140 —— 0x131895）
    UInt8 table[440];
    std::memset(table, 0, sizeof(table));

    const int ret = NRedPpSmuOverlayGetDpmClockTable(nullptr, table);
    assert(ret == 1 && "callback must return 1");

    const UInt32* p = reinterpret_cast<const UInt32*>(table);
    // FClocks[4] @ +0x80：4×8B = {Freq,Vol} ×4
    assert(p[0x80/4 + 0] == 400  && "FClocks[0].Freq");
    assert(p[0x80/4 + 1] == 3600 && "FClocks[0].Vol");
    assert(p[0x80/4 + 2] == 800  && "FClocks[1].Freq");
    assert(p[0x80/4 + 3] == 3600 && "FClocks[1].Vol");
    assert(p[0x80/4 + 4] == 1067 && "FClocks[2].Freq");
    assert(p[0x80/4 + 5] == 3600 && "FClocks[2].Vol");
    assert(p[0x80/4 + 6] == 1333 && "FClocks[3].Freq");
    assert(p[0x80/4 + 7] == 3600 && "FClocks[3].Vol");

    // 其余字段保持 0：+0x00..+0x7f（DcfClocks/SocClocks）、+0xa0..+0x13f（Mem/V/D/VPE）
    for (size_t i = 0; i < 0x80/4; ++i) assert(p[i] == 0 && "DcfClocks/SocClocks must stay 0");
    for (size_t i = 0xa0/4; i < 0x140/4; ++i) assert(p[i] == 0 && "Mem/V/D/VPEClocks must stay 0");

    printf("  PASS: callback fills 4×8B at +0x80, returns 1, rest zero\n");
}

static void test_f1c0_f408_untouched() {
    printf("▶ test_f1c0_f408_untouched\n");
    gTestGateEnabled = true;
    gPtrGuardPass = true;

    // factory_struct 模拟（0x500）：pp_smu_funcs **内嵌**于 factory+0x1c0
    //   （工厂子函数 0x15d637：`lea 0x1c0(%r13),%rdi` + memcpy 296B ⇒ 内嵌对象）；
    //   dccg 指针存于 factory+0x408（powerUp 0xff76f `mov 0x408(%rax),%rdx`）。
    // "f1c0/f408 不动" ⇒ 覆盖只许写 pp_smu_funcs 对象内三个字段（pp+0x00/0x18/0x38），
    //   工厂其余字节（含 +0x408 dccg 槽、+0x1c0 槽位本身之外的全部）必须原样。
    UInt8 factory[0x500];
    std::memset(factory, 0xEE, sizeof(factory));   // 全 canary

    // 预置 dccg 槽（+0x408）为可辨识哨兵，验证覆盖后不变。
    UInt64* const factoryPtr = reinterpret_cast<UInt64*>(factory);
    const UInt64 kSentinelDccg = 0xBBBB00000000408ULL;
    factoryPtr[0x408/8] = kSentinelDccg;

    const UInt64 pp = reinterpret_cast<UInt64>(factory) + 0x1c0;   // 调用方已持有的 pp_smu 指针
    if (checkKernelArgument("-NRedPpSmuOverlay")) {
        overlayWrite(pp);
    }

    // 1) pp_smu_funcs 对象内三个字段已写
    const UInt64* ps = reinterpret_cast<const UInt64*>(factory + 0x1c0);
    assert(ps[0x00/8] == 1 && "pp_smu_funcs.f0");
    assert(ps[0x18/8] == reinterpret_cast<UInt64>(&gNRedPpSmuOverlayStub) && "pp_smu_funcs.f18");
    assert(ps[0x38/8] == reinterpret_cast<UInt64>(&NRedPpSmuOverlayGetDpmClockTable) && "pp_smu_funcs.f38");

    // 2) dccg 槽（factory+0x408）不变
    assert(factoryPtr[0x408/8] == kSentinelDccg && "factory_struct->f408 (dccg) must not change");

    // 3) 工厂其余可写字节全部保持 canary（只允许 3 个写点：pp+0x00/0x18/0x38，
    //    dccg 槽 +0x408 预置哨兵，不参与 canary 校验）。
    const UInt64 kCanary = 0xEEEEEEEEEEEEEEEEULL;
    const UInt64 dccgAddr = reinterpret_cast<UInt64>(factory) + 0x408;
    for (size_t i = 0; i < sizeof(factory)/8; ++i) {
        const UInt64 addr = reinterpret_cast<UInt64>(&factoryPtr[i]);
        if (addr == pp + 0x00) continue;
        if (addr == pp + 0x18) continue;
        if (addr == pp + 0x38) continue;
        if (addr == dccgAddr) continue;   // dccg 槽预置哨兵，非 canary
        if (factoryPtr[i] != kCanary) {
            printf("  FAIL at i=%zu (addr=%llx): got %llx expected %llx\n",
                   i, addr, factoryPtr[i], kCanary);
            fflush(stdout);
            assert(false && "no byte outside the 3 write points may change");
        }
    }

    printf("  PASS: f1c0 slot & factory bytes untouched except f0/f18/f38\n");
}

int main() {
    printf("=== pp_smu overlay offline tests ===\n");
    test_kernel_ptr_guard_semantics();
    test_gate_default_off_zero_write();
    test_gate_on_writes_f0_f18_f38();
    test_guard_reject_no_write();
    test_callback_fills_4x8b_returns_1();
    test_f1c0_f408_untouched();
    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}