# 离线单元测试构建（分析机用户态，无需 macOS / 内核 SDK）
#
# 用法（从「项目根」或「驱动仓库根 src/」执行均可，路径自动推导）：
#   make -f src/NootedRed/DisplaySeq/Makefile test      # 项目根
#   make -f NootedRed/DisplaySeq/Makefile test          # src/ 内
#   make -f src/NootedRed/DisplaySeq/Makefile clean
#
# 这些测试仅覆盖**可离线子集**（纯计算、序列生成、协议编解码）——
# 依据 docs/测试与验证设计.md §5.1 与项目规范的 TDD 边界。

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O2

# 由本 Makefile 的调用路径推导「驱动仓库根」前缀（"" 或 "src/"）。
MKFILE    = $(lastword $(MAKEFILE_LIST))
ROOT      = $(patsubst %NootedRed/DisplaySeq/Makefile,%,$(MKFILE))
SEQDIR    = $(ROOT)NootedRed/DisplaySeq
INCLUDES  = -I$(SEQDIR)
BUILDDIR  = build/display-tests

# 自动发现 tests/*.cpp —— 新增测试文件无需改本 Makefile
TESTS = $(patsubst $(SEQDIR)/tests/%.cpp,$(BUILDDIR)/%,$(wildcard $(SEQDIR)/tests/*.cpp))

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
