// 注入式 RegSink —— 真机（内核态）与离线测试共用同一条执行路径
//
// 设计意图（依据 docs/ROADMAP.md §3.2「序列生成架构：同一份代码，三处使用」）：
//   序列生成器只产出 RegOp；真正读写硬件的动作由本类完成。把"寄存器读写与延时"
//   做成**注入的函数指针**，同一份 sink 代码就能：
//     · 内核态：转发到苹果的 MP1 MMIO 通道（X5000HWLibs 的 cgs 读写）
//     · 用户态：转发到 mock（预置读值），用于验证"真机路径产出的序列 == 生成器序列"
//
// ⛔ 本文件不含任何内核头文件 —— 因此内核态与用户态都能编译，
//    这正是"三消费者共用一份代码"能成立的前提（它也因而能被离线单元测试覆盖）。

// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once

#include "RegSink.hpp"

namespace display {

// 一条寄存器通道的描述：地址是**段内偏移**，段的身份由 (blockInstance, block, regOffBase) 给出。
//
// 为什么这样切：我们的寄存器表里的地址（如 MP1 邮箱 0x283/0x293/0x29B）都是"段内偏移"，
// 而"这个偏移属于哪个 IP 段、段基址是多少"由寄存器通道自己知道（苹果的 cgs 访问器会按
// block 去查硬件上报的基址表）。生成器因此不必知道任何段基址。
struct RegChannel {
    void*         ctx{nullptr};          // 通道上下文（内核态 = 苹果 SMU 上下文）
    std::uint32_t blockInstance{0};      // block 实例号（苹果 cgs 第 2 参）
    std::uint32_t block{0};              // CAILHWBlock（内核态传 kCAILHWBlockMP1）
    std::uint32_t regOffBase{0};         // base index（苹果 cgs 第 5 参）

    std::uint32_t (*read)(void* ctx, std::uint32_t off, std::uint32_t blockInstance, std::uint32_t block,
                          std::uint32_t regOffBase){nullptr};
    void (*write)(void* ctx, std::uint32_t off, std::uint32_t val, std::uint32_t blockInstance,
                  std::uint32_t block, std::uint32_t regOffBase){nullptr};
    void (*delay)(std::uint32_t us){nullptr};
};

// 把 RegOp 序列落到给定通道上执行。轮询的超时上限由 RegSink 基类保证
// （默认 10µs × 200000 次 —— 与 Linux dcn314_smu_wait_for_response 量级一致），
// 因此坏通道不会把内核挂死。
class InjectedRegSink final : public RegSink {
public:
    explicit InjectedRegSink(const RegChannel& ch) : ch_(ch) {}

    RegValue read(RegAddr addr) override {
        if (ch_.read == nullptr) {
            failures_ += 1;
            return 0;
        }
        return ch_.read(ch_.ctx, addr, ch_.blockInstance, ch_.block, ch_.regOffBase);
    }

    void write(RegAddr addr, RegValue val) override {
        if (ch_.write == nullptr) {
            failures_ += 1;
            return;
        }
        ch_.write(ch_.ctx, addr, val, ch_.blockInstance, ch_.block, ch_.regOffBase);
    }

    void delayMicroseconds(std::uint32_t us) override {
        if (ch_.delay != nullptr) { ch_.delay(us); }
    }

    // 通道缺失（函数指针为空）导致的失败次数。调用方据此判断"这次下发根本没落到硬件"。
    std::uint32_t failures() const { return failures_; }

private:
    RegChannel    ch_{};
    std::uint32_t failures_{0};
};

}  // namespace display
