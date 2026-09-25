// RegOp —— 寄存器操作与序列容器
//
// 本文件是「序列生成 / 寄存器写入分离」架构的基础类型。
//
// ⛔ 两条硬约束：
//   1. 不含任何内核头文件（IOKit 等）→ 必须能在分析机用户态直接编译
//      （依据：docs/测试与验证设计.md §5.1）
//   2. **零动态分配** —— 序列写入调用方提供的固定缓冲区，不使用 std::vector。
//      原因：真机侧的同一份生成器跑在内核态（NootedRed 是 kext），
//      内核态不做可失败分配、不用异常。用固定容量缓冲使
//      "用户态影子运行"与"内核态真机"共用完全相同的代码路径。
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace display {

using RegAddr  = uint32_t;
using RegValue = uint32_t;

// 一次寄存器操作。序列生成代码只产出它，绝不直接触碰硬件。
//
// 为什么需要 Poll：VBIOSSMC 的发送时序含"轮询响应寄存器直到不忙"的循环
// （Linux dcn314_smu_wait_for_response）。轮询结果依赖真实硬件，无法离线预测，
// 因此**不能**由生成器展开成死循环——必须作为一个 op 交给 sink 执行。
// 这样生成器保持纯函数，运行时行为由 sink 负责。
struct RegOp {
    enum class Kind : uint8_t {
        Write = 0,  // sink 将 value 写入 addr
        Read  = 1,  // sink 读取 addr（值记录到 sink，不参与生成）
        Poll  = 2,  // sink 轮询 addr，直到 (值 & mask) != 0（读到非忙值），或超时
        Delay = 3,  // sink 等待 value 微秒
    };

    Kind    kind{Kind::Write};
    RegAddr addr{0};
    RegValue value{0};

    // Poll 专用：终止条件为 (读值 & mask) != 0。
    // 语义取自 Linux `dcn314_smu_wait_for_response`：`while (val == BUSY)`；
    // 取 mask = ~busy 后，"读到非忙值"等价于 (读值 & ~busy) != 0。
    // 当 busy = 0（VBIOSSMC_Status_BUSY）时 mask = 0xFFFFFFFF，
    // 判据退化为"读值 != 0"——与 Linux 完全一致。
    RegValue mask{0};

    // 溯源标签：指向产该 op 的生成器步骤名（静态字符串，不持有所有权）。
    // 差分器据此定位"第一个分叉"落在哪一步，无需反查地址表。
    const char* step{nullptr};
};

// 带标签构造器
constexpr RegOp regWrite(RegAddr addr, RegValue value, const char* step = nullptr) {
    return RegOp{RegOp::Kind::Write, addr, value, 0, step};
}
constexpr RegOp regRead(RegAddr addr, const char* step = nullptr) {
    return RegOp{RegOp::Kind::Read, addr, 0, 0, step};
}
// 轮询直到读值不再等于 busyValue。
// mask 取 ~busyValue：若 busyValue 为全 0（VBIOSSMC BUSY=0x0），则 mask=0xFFFFFFFF，
// 判据退化为"读值 != 0"——与 Linux `while (val == BUSY)` 完全一致。
constexpr RegOp regPollUntilNot(RegAddr addr, RegValue busyValue, const char* step = nullptr) {
    return RegOp{RegOp::Kind::Poll, addr, 0, static_cast<RegValue>(~busyValue), step};
}
constexpr RegOp regDelay(uint32_t microseconds, const char* step = nullptr) {
    return RegOp{RegOp::Kind::Delay, 0, microseconds, 0, step};
}

// 有序的寄存器操作序列 —— 由调用方提供的固定容量缓冲区承载。
//
// ⚠️ 顺序是语义的一部分：差分器按索引逐项比对，故容器必须保序。
//
// 容量溢出处理：**静默丢弃并置 overflow 标志**。
// 不抛异常、不做动态增长——内核态不允许。调用方生成后应检查 `overflowed()`。
class RegSeq {
public:
    static constexpr size_t kDefaultCapacity = 256;

    RegSeq(RegOp* buffer, size_t capacity) : buf_(buffer), cap_(capacity) {}
    RegSeq(const RegSeq&)            = delete;
    RegSeq& operator=(const RegSeq&) = delete;

    void push(const RegOp& op) {
        if (count_ >= cap_) {
            overflow_ = true;
            return;
        }
        buf_[count_++] = op;
    }

    void clear() {
        count_    = 0;
        overflow_ = false;
    }

    size_t size() const { return count_; }
    bool        empty() const { return count_ == 0; }
    bool        overflowed() const { return overflow_; }
    size_t capacity() const { return cap_; }

    const RegOp& at(size_t i) const { return buf_[i]; }
    const RegOp& operator[](size_t i) const { return buf_[i]; }

    const RegOp* begin() const { return buf_; }
    const RegOp* end() const { return buf_ + count_; }

    // ── 过滤（差分器用）──
    // 把匹配项拷贝到 `out` 的缓冲区。返回写入项数。
    // 不分配内存；溢出时同样置 out 的 overflow 标志。

    // 按 step 标签前缀过滤（保留原顺序）
    size_t copyWhereStepPrefix(RegSeq& out, const char* prefix) const;

    // 只保留 Write（差分器区分读写：读序列的差异通常无害，写序列才是重点）
    size_t copyWrites(RegSeq& out) const;

private:
    RegOp*      buf_;
    size_t cap_;
    size_t count_{0};
    bool        overflow_{false};
};

// ── 过滤的常量时间实现（头文件内联，避免额外的编译单元）──

namespace detail {
inline bool startsWith(const char* s, const char* prefix) {
    if (s == nullptr || prefix == nullptr) { return false; }
    while (*prefix != '\0') {
        if (*s != *prefix) { return false; }
        ++s;
        ++prefix;
    }
    return true;
}
}  // namespace detail

inline size_t RegSeq::copyWhereStepPrefix(RegSeq& out, const char* prefix) const {
    if (prefix == nullptr) { return 0; }
    size_t n = 0;
    for (size_t i = 0; i < count_; ++i) {
        if (detail::startsWith(buf_[i].step, prefix)) {
            out.push(buf_[i]);
            ++n;
        }
    }
    return n;
}

inline size_t RegSeq::copyWrites(RegSeq& out) const {
    size_t n = 0;
    for (size_t i = 0; i < count_; ++i) {
        if (buf_[i].kind == RegOp::Kind::Write) {
            out.push(buf_[i]);
            ++n;
        }
    }
    return n;
}

}  // namespace display
