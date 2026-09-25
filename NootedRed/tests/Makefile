# 离线单元测试构建（分析机用户态，无需 macOS / 内核 SDK）
#
# 用法（从「项目根」或「驱动仓库根 src/」执行均可，路径自动推导）：
#   make -f src/NootedRed/tests/Makefile test           # 项目根
#   make -f NootedRed/tests/Makefile test               # src/ 内
#   make -f src/NootedRed/tests/Makefile clean
#
# 覆盖范围：Regs/ 纯常量头的寄存器地址正确性（路线图第一步验收）。
# Regs/NBIO.hpp、GPUDriversAMD/RavenIPOffset.hpp 依赖 <IOKit/IOTypes.h>，
# 用户态用 tests/stub/ 下的最小替身（仅 UInt32/UInt8 typedef）补齐，
# 从而测试直接包含真实头文件、断言其实际常量值。
# 布局参照 src/NootedRed/DisplaySeq/Makefile。

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O2

MKFILE    = $(lastword $(MAKEFILE_LIST))
ROOT      = $(patsubst %NootedRed/tests/Makefile,%,$(MKFILE))
NRDIR     = $(ROOT)NootedRed
STUBDIR   = $(NRDIR)/tests/stub
INCLUDES  = -I$(STUBDIR) -I$(NRDIR)
BUILDDIR  = build/nred-tests

TESTS = $(BUILDDIR)/test_nbio_strap

.PHONY: test clean

test: $(TESTS)
	@echo "── 运行离线单元测试 ──"
	@set -e; for t in $(TESTS); do echo "▶ $$t"; $$t; done
	@echo "全部测试通过。"

$(BUILDDIR)/%: $(NRDIR)/tests/%.cpp $(NRDIR)/Regs/NBIO.hpp $(NRDIR)/GPUDriversAMD/RavenIPOffset.hpp $(wildcard $(STUBDIR)/IOKit/*.h)
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

clean:
	rm -rf $(BUILDDIR)
