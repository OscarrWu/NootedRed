# DisplaySeq —— 寄存器序列生成层

> 本目录是「序列生成 / 寄存器写入分离」架构的落盘位置。
> 依据：`docs/测试与验证设计.md` §5.1/§5.2。

---

## 目录

1. [这个目录解决什么问题](#1-这个目录解决什么问题)
2. [文件清单](#2-文件清单)
3. [分层规则（重要）](#3-分层规则重要)
4. [为什么不用 std::vector](#4-为什么不用-stdvector)
5. [构建与测试](#5-构建与测试)
6. [Xcode 工程登记（待办）](#6-xcode-工程登记待办)
7. [参考文献](#7-参考文献)

---

## 1. 这个目录解决什么问题

移植工作最核心的风险是：**从 Linux 抄寄存器序列时抄错了**。

检测这种错误需要把"我们的序列"与"Linux 在真实硬件上跑出的序列"逐项比对。为此，**同一份序列生成代码必须被三个消费者共用**：

| 消费者 | 在哪跑 | 用途 |
|---|---|---|
| 影子运行 | 分析机用户态 | 把序列落盘，喂给差分器 |
| 单元测试 | 分析机用户态 | 断言序列片段 |
| 真机 | macOS 内核 | 真正写寄存器 |

**三者必须共用同一份生成器，不能有第二份手写实现。** 否则差分器结论失去意义：若真机路径另有手写实现，"离线比对通过"不能保证真机正确。

本目录承载**生成器**（纯逻辑）与**用户态消费者**；内核态消费者属计划 C（见 §3）。

---

## 2. 文件清单

| 文件 | 角色 | 含内核头文件 |
|---|---|---|
| `RegOp.hpp` | `RegOp`（单次操作）/ `RegSeq`（有序序列） | ❌ 否 |
| `RegSink.hpp` | 消费者接口：read/write/delay + execute/executeAll | ❌ 否 |
| `RegSinkUser.hpp` | 用户态影子 sink：JSON 落盘、预置读值 | ❌ 否 |
| `VBIOSSMC.hpp` | VBIOSSMC 消息号与返回码常量（用户态副本） | ❌ 否 |
| `VbiosSmcSeq.hpp` | **VBIOSSMC 发送时序生成器**（路线图第五步复用） | ❌ 否 |
| `tests/test_regop_seq.cpp` | 离线单元测试（13 项断言） | ❌ 否 |
| `Makefile` | 离线构建入口 | —— |

---

## 3. 分层规则（重要）

| 层 | 是否可含 IOKit / 内核头 | 归属环节 | 落盘位置 |
|---|---|---|---|
| **序列生成器** | **禁止** | 前置准备 ✅ | 本目录 |
| **用户态消费者**（影子/测试） | 禁止 | 第三步 | 本目录 + `kb/tools/` |
| **内核态消费者**（真机写寄存器） | 允许（需要 `AmdRegisterAccess`） | 第五步 | 本目录（待落地） |

> ⛔ **生成器绝不可 include 任何内核头文件**。这是它能在分析机用户态编译的唯一前提，也是"三消费者共用一份代码"能成立的基础。
>
> 判定方法：`make -f src/NootedRed/DisplaySeq/Makefile test` 能编译通过，即证明该约束未被破坏。

---

## 4. 为什么不用 `std::vector`

序列写入调用方提供的**固定容量缓冲区**（`RegSeq(buffer, capacity)`）。

**理由**：真机侧的同一份生成器跑在内核态（NootedRed 是 kext）。内核态不做可失败分配、不用异常。用固定容量缓冲，才使"用户态影子运行"与"内核态真机"**共用完全相同的代码路径**——这正是本架构的立论基础。

溢出处理：**静默丢弃并置 `overflowed()` 标志**，调用方必须检查。不抛异常、不动态增长。

---

## 5. 构建与测试

从仓库根执行：

```bash
make -f src/NootedRed/DisplaySeq/Makefile test     # 编译并运行全部离线测试
make -f src/NootedRed/DisplaySeq/Makefile clean    # 清理 build/display-tests/
```

也可以直接编译单个测试：

```bash
g++ -std=c++17 -Wall -Wextra -Werror -O2 \
    -Isrc/NootedRed/DisplaySeq \
    src/NootedRed/DisplaySeq/tests/test_regop_seq.cpp -o /tmp/t && /tmp/t
```

---

## 6. Xcode 工程登记（待办）

⚠️ **本目录的文件尚未登记进 `src/NootedRed.xcodeproj/project.pbxproj`，因此目前不会参与 kext 构建。**

NootedRed 的 Xcode 工程使用**显式文件引用**（`PBXFileReference` + `PBXBuildFile`），新增源文件必须手动登记。核对方法：

```bash
grep -c "DisplaySeq" src/NootedRed.xcodeproj/project.pbxproj   # 当前为 0
```

**处置**：登记属 macOS 侧操作（建议用 Xcode 打开工程添加，而非手工编辑 pbxproj）。已记为**路线图第五步的前置项**。

对前置准备的验收无影响——其判据是"接口无硬件依赖、可在用户态编译"，由本目录的 Makefile 直接证明。

---

## 7. 参考文献

| # | 文献 | 位置 |
|---|---|---|
| [1] | 《780M 驱动项目 —— 验证与操作手册》§5.1 序列生成与寄存器写入必须分离、§5.2 建议的落地形态 | `docs/测试与验证设计.md` |
| [2] | Linux amdgpu —— VBIOSSMC 发送时序 | `oldfiles-handoff/reference/linux/drivers/gpu/drm/amd/display/dc/clk_mgr/dcn314/dcn314_smu.c`（`wait_for_response` L96、`send_msg_with_param` L118） |
| [3] | 本项目路线图 | `docs/ROADMAP.md` §3.2（序列生成架构）、第五步（时钟主流程） |

---

*文档结束。*
