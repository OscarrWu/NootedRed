// DCN314.hpp 常量核对测试（分析机用户态，TDD 先行）
//
// 验收（任务）：常量与 Linux 权威头文件逐字节核对一致。
// 真值表由 mk_check.py 直接从 dcn_3_1_4_offset.h / _sh_mask.h 生成
// （build/dcn314_truth.txt，键 = 项目常量名），本测试做全量双向核对：
//   1. 实现文件每个 constexpr 都有出处（出现在真值表中，步进常量除外）；
//   2. 每个真值条目在实现文件中存在且数值一致；
//   3. DIG 实例步进 0x100（Linux 头文件差值）。
//
// 编译运行（分析机，不依赖 IOKit / 内核 SDK）：
//   python3 src/NootedRed/Regs/tests/mk_check.py > build/dcn314_truth.txt
//   g++ -std=c++17 -Wall -Wextra -Werror -O2 src/NootedRed/Regs/tests/test_dcn314_regs.cpp -o build/test_dcn314_regs
//   ./build/test_dcn314_regs [impl.hpp 路径] [真值表路径]
// 两个路径参数可省略；省略时按「相对驱动仓库根」的默认值取：
//   NootedRed/Regs/DCN314.hpp  与  build/dcn314_truth.txt
// （这样无论从项目根还是从 src/ 内执行，由 Makefile 用 -D 传入绝对/正确相对路径）
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#include <cassert>
#include <cstdio>
#include <fstream>
#include <map>
#include <regex>
#include <string>

namespace {

// 从实现文件解析出 constexpr UInt32 <NAME> = <value>; 行。
std::map<std::string, unsigned long long> parse_impl(const std::string &path) {
    std::map<std::string, unsigned long long> out;
    std::ifstream in(path);
    assert(in && "无法打开 DCN314.hpp（需在仓库根目录运行）");
    std::string line;
    const std::regex re(
        R"(^\s*constexpr\s+UInt32\s+([A-Za-z0-9_]+)\s*=\s*(0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]*\s*;)");
    while (std::getline(in, line)) {
        // 去掉行尾注释，避免注释里的示例值被误解析
        const auto pos = line.find("//");
        if (pos != std::string::npos) line = line.substr(0, pos);
        std::smatch m;
        if (std::regex_search(line, m, re))
            out[m[1]] = std::stoull(m[2], nullptr, 0);
    }
    return out;
}

// 解析 mk_check.py 产出的 Linux 头文件真值表（键 = 项目常量名，值 = 十六进制）。
std::map<std::string, unsigned long long> parse_truth(const std::string &path) {
    std::map<std::string, unsigned long long> out;
    std::ifstream in(path);
    assert(in && "真值表缺失：先运行 src/NootedRed/Regs/tests/mk_check.py");
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        assert(eq != std::string::npos && "真值表行格式错误");
        out[line.substr(0, eq)] = std::stoull(line.substr(eq + 1), nullptr, 16);
    }
    return out;
}

bool is_stride_const(const std::string &name) {
    return name.find("_STRIDE") != std::string::npos;
}

} // namespace

int main() {
#ifndef NRED_IMPL_PATH
#define NRED_IMPL_PATH "NootedRed/Regs/DCN314.hpp"
#endif
#ifndef NRED_TRUTH_PATH
#define NRED_TRUTH_PATH "build/dcn314_truth.txt"
#endif
    const auto impl = parse_impl(NRED_IMPL_PATH);
    const auto truth = parse_truth(NRED_TRUTH_PATH);
    assert(!impl.empty() && !truth.empty());

    // 1. 实现文件中的每个常量都有出处（步进常量由相邻实例差值另行验证）。
    for (const auto &kv : impl) {
        if (is_stride_const(kv.first)) continue;
        assert(truth.count(kv.first) && "常量缺少 Linux 头文件出处条目");
    }
    // 2. 与 Linux 头文件真值全量核对（数值必须逐条一致）。
    for (const auto &kv : truth) {
        assert(impl.count(kv.first) && "真值条目在实现文件缺失");
        assert(impl.at(kv.first) == kv.second && "常量与 Linux 头文件不一致");
    }

    // 3. 实例步进：DIG1 = DIG0 + 0x100，DIG4 = DIG0 + 4×0x100。
    assert(impl.at("DIG1_DIG_FE_CNTL") == impl.at("DIG_FE_CNTL") + 0x100);
    assert(impl.at("DIG4_DIG_FE_CNTL") == impl.at("DIG_FE_CNTL") + 4 * 0x100);

    std::printf("  [PASS] 与 Linux dcn_3_1_4 头文件全量核对 %zu 条\n", truth.size());
    std::printf("  [PASS] DIG 实例步进 0x100\n");
    std::puts("全部测试通过。");
    return 0;
}
