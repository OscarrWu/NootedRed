// RegSink —— 序列的消费者接口（三个消费者共用同一份生成器）
//
// 依据：docs/测试与验证设计.md §5.1
//   「同一份序列生成代码，有三个消费者：影子运行 / 单元测试 / 真机」
//
// 本文件定义**接口**，不含内核头文件。真机实现在 RegSinkKernel.hpp（含 IOKit），
// 用户态实现在 kb/tools 的影子里。
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.

#pragma once

#include "RegOp.hpp"

namespace display {

// 寄存器访问抽象。三个消费者各自实现它：
//   - 内核态：调 AmdRegisterAccess 的 read/write
//   - 用户态影子：把 op 落盘，Read/Poll 返回预置值
class RegSink {
public:
    virtual ~RegSink() = default;

    virtual RegValue read(RegAddr addr)                = 0;
    virtual void     write(RegAddr addr, RegValue val) = 0;
    virtual void     delayMicroseconds(uint32_t us) = 0;

    // 执行一条 op。返回 false 表示失败（如 Poll 超时）。
    //
    // Poll 语义：反复 read(addr) 直到 (值 & mask) != 0（即"读到非忙值"）。
    // 其中 mask = ~busy，故等价于 Linux 的 `while (val == BUSY)` 退出条件。
    // 上限由 pollTimeout 决定，避免死循环挂住内核。
    bool execute(const RegOp& op) {
        switch (op.kind) {
        case RegOp::Kind::Write:
            lastValue_ = op.value;
            write(op.addr, op.value);
            return true;
        case RegOp::Kind::Read:
            lastValue_ = read(op.addr);
            return true;
        case RegOp::Kind::Update: {
            // Linux `REG_UPDATE(reg, field, val)` 的等价执行：
            //   v = (v & ~mask) | ((value << shift) & mask)
            // 注意会产生**一次读 + 一次写**两条硬件访问 —— 与真值记录的形态一致。
            const RegValue cur = read(op.addr);
            lastValue_        = (cur & ~op.mask) | ((op.value << op.shift) & op.mask);
            write(op.addr, lastValue_);
            return true;
        }
        case RegOp::Kind::Delay:
            delayMicroseconds(op.value);
            return true;
        case RegOp::Kind::Poll:
            for (uint32_t i = 0; i < pollTimeout_; ++i) {
                lastValue_ = read(op.addr);
                if ((lastValue_ & op.mask) != 0) { return true; }
                delayMicroseconds(pollIntervalUs_);
            }
            return false;  // 超时
        }
        return false;
    }

    // 顺序执行整条序列。遇失败即停，返回最后成功执行的索引 + 1。
    size_t executeAll(const RegSeq& seq) {
        for (size_t i = 0; i < seq.size(); ++i) {
            if (!execute(seq[i])) { return i; }
        }
        return seq.size();
    }

    // 最近一次 Read/Poll 读到的值（含 Write 写入值）。供调用方取回结果。
    RegValue lastValue() const { return lastValue_; }

    void setPollLimits(uint32_t timeoutIters, uint32_t intervalUs) {
        pollTimeout_    = timeoutIters;
        pollIntervalUs_ = intervalUs;
    }

private:
    // 对应 Linux `dcn314_smu_wait_for_response(clk_mgr, 10, 200000)`：
    // 10 µs 间隔 × 200000 次
    uint32_t pollTimeout_{200000};
    uint32_t pollIntervalUs_{10};
    RegValue      lastValue_{0};
};

}  // namespace display
