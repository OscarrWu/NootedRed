# 离线单元测试构建（分析机用户态，无需 macOS / 内核 SDK）
#
# 用法（从「项目根」或「驱动仓库根 src/」执行均可，路径自动推导）：
#   make -f src/NootedRed/Regs/tests/Makefile test          # 项目根
#   make -f NootedRed/Regs/tests/Makefile test              # src/ 内
#   make -f src/NootedRed/Regs/tests/Makefile regen-truth   # 需要 Linux 参考树
#   make -f src/NootedRed/Regs/tests/Makefile clean
#
# 被测对象：Regs/DCN314.hpp 的常量值（路线图第二步验收）。
# 真值表 dcn314_truth.txt 由 mk_check.py 从 Linux dcn_3_1_4_offset.h / _sh_mask.h 生成，
# 已随仓库入库（257 条），因此 test 目标**不依赖 oldfiles-handoff**、可离线复查；
# 仅当需要重新生成真值表（Linux 参考树更新后）才用 regen-truth。
# 布局参照 src/NootedRed/PixelDiv/Makefile。

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O2

MKFILE    = $(lastword $(MAKEFILE_LIST))
ROOT      = $(patsubst %NootedRed/Regs/tests/Makefile,%,$(MKFILE))
REGDIR    = $(ROOT)NootedRed/Regs
TRUTH     = $(REGDIR)/tests/dcn314_truth.txt
BUILDDIR  = build/regs-tests

TESTS = $(BUILDDIR)/test_dcn314_regs

.PHONY: test regen-truth clean

test: $(TESTS) $(BUILDDIR)/dcn314_truth.txt
	@echo "── 运行离线单元测试 ──"
	@set -e; for t in $(TESTS); do echo "▶ $$t"; $$t; done
	@echo "全部测试通过。"

$(BUILDDIR)/%: $(REGDIR)/tests/%.cpp $(REGDIR)/DCN314.hpp
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -DNRED_IMPL_PATH='"$(REGDIR)/DCN314.hpp"' -DNRED_TRUTH_PATH='"$(BUILDDIR)/dcn314_truth.txt"' \
	    $< -o $@

# 测试真值表的路径由编译期宏 NRED_TRUTH_PATH 指定（见上方规则），指向 $(BUILDDIR)/dcn314_truth.txt；
# 该文件由入库副本复制而来，故离线运行不依赖 oldfiles-handoff。
$(BUILDDIR)/dcn314_truth.txt: $(TRUTH)
	@mkdir -p $(BUILDDIR)
	cp $< $@

# 重新生成真值表：需要 Linux 参考树（oldfiles-handoff/reference/linux）。
# 生成后需人工确认差异并提交 dcn314_truth.txt。
regen-truth:
	python3 $(REGDIR)/tests/mk_check.py > $(TRUTH)
	@echo "已更新 $(TRUTH)，请 diff 确认后提交。"

clean:
	rm -rf $(BUILDDIR)
