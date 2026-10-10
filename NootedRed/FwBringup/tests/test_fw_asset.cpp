// test_fw_asset.cpp —— A-2：固件资产完整性校验离线单测（g++ 12 可编译：不含 #embed）
//
// 覆盖：
//   ① fnv1a64 已知向量（自洽性自检）；
//   ② fwAssetVerify：正常/损坏/缺失/零大小 四路径；
//   ③ 校验失败路径不得静默通过（返回 false，调用方据此跳过装载）。
//
// 编译运行：
//   make -f src/NootedRed/FwBringup/tests/Makefile test
//   （已加入 TESTS 列表）

#include "FwBringup/FwFirmwareAsset.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace fw;

// ── 已知向量自检（FNV-1a 64 标准向量："foobar"）──
// 参考：https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function
static void testFnvKnownAnswer()
{
    printf("[test] fnv1a64 known answer... ");
    const uint8_t data[] = {'f','o','o','b','a','r'};
    const uint64_t expected = 0x85944171F73967E8ULL;  // FNV-1a 64 of "foobar"
    const uint64_t got = fnv1a64(data, sizeof(data));
    assert(got == expected);
    printf("PASS (0x%016llX)\n", (unsigned long long)got);
}

// ── fwAssetVerify 四路径 ──
static void testAssetVerify()
{
    printf("[test] fwAssetVerify paths... ");

    // ① 正常：构造合法描述符（数据=已知向量，哈希=实测值）⇒ true
    const uint8_t data[] = {'f','o','o','b','a','r'};
    FwAssetDesc okDesc = {data, sizeof(data), 0x85944171F73967E8ULL};
    assert(fwAssetVerify(okDesc) == true);

    // ② 损坏：哈希不一致 ⇒ false
    FwAssetDesc badHash = {data, sizeof(data), 0xFFFFFFFFFFFFFFFFULL};
    assert(fwAssetVerify(badHash) == false);

    // ③ 缺失：data == nullptr ⇒ false
    FwAssetDesc missing = {nullptr, 0, 0};
    assert(fwAssetVerify(missing) == false);

    // ④ 零大小：size == 0 ⇒ false
    FwAssetDesc zeroSize = {data, 0, 0x85944171F73967E8ULL};
    assert(fwAssetVerify(zeroSize) == false);

    printf("PASS\n");
}

int main()
{
    testFnvKnownAnswer();
    testAssetVerify();
    printf("=== ALL FW ASSET TESTS PASSED ===\n");
    return 0;
}