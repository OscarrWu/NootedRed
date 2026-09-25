# 离线单元测试构建（分析机用户态，无需 macOS / 内核 SDK）
#
# 用法（从仓库根执行）：
#   make -f src/NootedRed/PixelDiv/Makefile test
#   make -f src/NootedRed/PixelDiv/Makefile clean
#
# 被测对象是 DCN314 像素分频纯策略函数（PixelDiv.hpp 内联 constexpr 实现；
# PixelDiv.cpp 承载编译期不变量，随测试二进制一并编译把关）。
# 布局与 src/NootedRed/DisplaySeq/Makefile 一致。

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O2

PIXDIR    = src/NootedRed/PixelDiv
INCLUDES  = -I$(PIXDIR)
BUILDDIR  = build/pixeldiv-tests

TESTS = $(BUILDDIR)/test_pixeldiv

.PHONY: test clean

test: $(TESTS)
	@echo "── 运行离线单元测试 ──"
	@set -e; for t in $(TESTS); do echo "▶ $$t"; $$t; done
	@echo "全部测试通过。"

$(BUILDDIR)/%: $(PIXDIR)/tests/%.cpp $(PIXDIR)/PixelDiv.cpp $(wildcard $(PIXDIR)/*.hpp)
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< $(PIXDIR)/PixelDiv.cpp -o $@

clean:
	rm -rf $(BUILDDIR)
