# 离线单元测试构建（分析机用户态，无需 macOS / 内核 SDK）
#
# 用法（从仓库根执行）：
#   make -f src/NootedRed/DisplaySeq/Makefile test
#   make -f src/NootedRed/DisplaySeq/Makefile clean
#
# 这些测试仅覆盖**可离线子集**（纯计算、序列生成、协议编解码）——
# 依据 docs/测试与验证设计.md §5.1 与项目规范的 TDD 边界。

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O2

SEQDIR    = src/NootedRed/DisplaySeq
INCLUDES  = -I$(SEQDIR)
BUILDDIR  = build/display-tests

TESTS = $(BUILDDIR)/test_regop_seq

.PHONY: test clean

test: $(TESTS)
	@echo "── 运行离线单元测试 ──"
	@set -e; for t in $(TESTS); do echo "▶ $$t"; $$t; done
	@echo "全部测试通过。"

$(BUILDDIR)/%: $(SEQDIR)/tests/%.cpp $(wildcard $(SEQDIR)/*.hpp)
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

clean:
	rm -rf $(BUILDDIR)
