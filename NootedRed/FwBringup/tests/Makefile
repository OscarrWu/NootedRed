# SMU13 邮箱 / PSP13 bringup 离线单元测试构建
#
# 用法（从项目根执行）：
#   make -f src/NootedRed/FwBringup/tests/Makefile test
#   make -f src/NootedRed/FwBringup/tests/Makefile clean
#
# 这些测试仅覆盖可离线子集（纯计算、协议编解码、寄存器序列）——
# 依据 docs/子任务/乙线自建固件层作战计划.md T3/T5 验收判据。
#
# ⚠️ IOKit 兼容：Regs/PSP13.hpp 包含 <IOKit/IOTypes.h>（UInt32）。
#    本目录下 IOKit/IOTypes.h 是 Linux 用户态编译用的最小替身
#    （仅 typedef，非真实 IOKit）。

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O2

# 由本 Makefile 的调用路径推导「项目根」前缀（"" 或 "src/"）。
MKFILE    = $(lastword $(MAKEFILE_LIST))
ROOT      = $(patsubst %NootedRed/FwBringup/tests/Makefile,%,$(MKFILE))
FWBDIR    = $(ROOT)NootedRed/FwBringup
SEQDIR    = $(ROOT)NootedRed/DisplaySeq
INCLUDES  = -I$(ROOT)NootedRed -I$(FWBDIR)/tests
BUILDDIR  = $(FWBDIR)/tests/build

# 显式目标清单（T3 SMU13 邮箱 + T5 PSP13 ring/bringup + RegSinkKernel + SmnReadProbe）
T3_TEST  = test_smu13_mailbox
T5_TESTS = test_psp13_ring test_psp13_bringup
T6_TESTS = test_reg_sink_kernel
T7_TESTS = test_smn_read_probe
TESTS    = $(addprefix $(BUILDDIR)/,$(T3_TEST) $(T5_TESTS) $(T6_TESTS) $(T7_TESTS))

test: $(TESTS)
	@echo "── 运行 SMU13 邮箱 / PSP13 bringup 离线单元测试 ──"
	@set -e; for t in $(TESTS); do echo "▶ $$t"; $$t; done
	@echo "全部测试通过。"

$(BUILDDIR)/%: $(FWBDIR)/tests/%.cpp $(wildcard $(FWBDIR)/*.hpp) $(wildcard $(SEQDIR)/*.hpp)
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

clean:
	rm -rf $(BUILDDIR)
