// 用户态影子运行器 —— 把序列落盘，并支持"预置读值"驱动
//
// 用途（依据 docs/测试与验证设计.md §5.2）：
//   影子运行器 → kb/tools/，消费序列，输出 JSON，喂给差分器。
//
// 本实现提供 docs 所需的「mock 寄存器接口」：预置对某地址的读返回，
// 使真机路径的序列能在用户态重放，从而与影子运行结果比对。
//
// ⛔ **仅用户态**：本文件用 <cstdio>/<cstdint> 与 std::（kext 环境没有这些头，
//    见 README §3.1），因此**绝不可**被内核态代码包含（内核态请用 RegSinkInjected.hpp）。
//
// Copyright © 2026 OscarrWu. Licensed under the Thou Shalt Not Profit License version 1.5.

#pragma once

#include "RegSink.hpp"

#include <cstdio>
#include <cstdint>

namespace display {

// 用户态 sink：可选地把每条实际执行的 op 打印成 JSON 行。
//
// 读值策略：若 addr 在 presetReads_ 中，返回预置值；否则返回 0。
// 这样 Poll 的终止由预设值决定，测试可控且确定（无真实硬件依赖）。
class UserSpaceRegSink final : public RegSink {
public:
    static constexpr std::size_t kMaxPresets = 32;
    static constexpr std::size_t kMaxPresetStr = 64;

    struct Preset {
        RegAddr addr;
        RegValue value;
    };

    UserSpaceRegSink() = default;

    void presetRead(RegAddr addr, RegValue value) {
        if (presetCount_ < kMaxPresets) {
            presets_[presetCount_++] = Preset{addr, value};
        }
    }

    // 打开 JSON 落盘（nullptr = 不落盘）。所有输出走同一 FILE*。
    void setOutput(std::FILE* f) { out_ = f; }

    // RegSink 接口
    RegValue read(RegAddr addr) override {
        std::uint32_t v = 0;
        for (std::size_t i = 0; i < presetCount_; ++i) {
            if (presets_[i].addr == addr) {
                v = presets_[i].value;
                break;
            }
        }
        if (!suppress_) { emit("read", addr, v, nullptr); }
        return v;
    }

    void write(RegAddr addr, RegValue val) override {
        if (!suppress_) { emit("write", addr, val, nullptr); }
    }

    void delayMicroseconds(std::uint32_t us) override {
        if (!suppress_) { emit("delay", 0, us, nullptr); }
    }

    // 为差分器准备的朴素 JSON 行输出（每行一个对象，便于流式处理）
    void emit(const char* kind, RegAddr addr, RegValue value, const char* step) {
        ++opCount_;
        if (out_ == nullptr) { return; }
        if (step != nullptr) {
            std::fprintf(out_, "{\"kind\":\"%s\",\"addr\":%u,\"value\":%u,\"step\":\"%s\"}\n", kind, addr, value, step);
        } else {
            std::fprintf(out_, "{\"kind\":\"%s\",\"addr\":%u,\"value\":%u}\n", kind, addr, value);
        }
    }

    // 记录整条序列（影子运行的主入口）：逐项 execute，并带 step 标签落盘。
    // 返回执行到的索引（== size 表示全部成功）。
    std::size_t runAndRecord(const RegSeq& seq) {
        suppress_ = true;   // 本函数自己负责带 step 的落盘，避免 read/write 重复计数
        struct Guard {
            bool* f;
            ~Guard() { *f = false; }
        } guard{&suppress_};
        for (std::size_t i = 0; i < seq.size(); ++i) {
            const auto& op = seq[i];
            const char* kind = op.kind == RegOp::Kind::Write ? "write" :
                               op.kind == RegOp::Kind::Read  ? "read"  :
                               op.kind == RegOp::Kind::Poll  ? "poll"  : "delay";
            const RegValue v = op.kind == RegOp::Kind::Delay ? op.value : op.value;
            emit(kind, op.addr, v, op.step);
            if (!execute(op)) { return i; }
        }
        return seq.size();
    }

    std::size_t opCount() const { return opCount_; }

private:
    Preset  presets_[kMaxPresets]{};
    std::size_t presetCount_{0};
    std::FILE*  out_{nullptr};
    std::size_t opCount_{0};
    bool        suppress_{false};
};

}  // namespace display
