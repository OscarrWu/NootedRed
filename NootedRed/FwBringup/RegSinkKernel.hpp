// RegSinkKernel —— 内核态 SMN 寄存器安全访问实现（header-only）
//
// 实现 display::RegSink 接口，按 Linux amdgpu 安全序列访问 MP0/MP1 SMN 寄存器：
//   加锁 → 写 PCIE_INDEX2 → 回读 PCIE_INDEX2（posted-write flush）
//   → 按需写 PCIE_INDEX_HI + 回读 → 读/写 PCIE_DATA2
//   → 清 HI + 回读 → 解锁
//
// 依据：docs/子任务/乙线SMN安全访问调查.md §3.3/§3.4（SafeChannel 报告）
//       Linux amdgpu_reg_access.c:612-663 (indirect_rreg_ext) / :779-818 (indirect_wreg_ext)
//
// ⚠️ 使用需所有者批准：本文件默认不执行（无调用者），不含任何 boot-arg 门控，
//    不在任何探针/hook 中被调用。真机启用前需所有者明确同意。
//
// 约束：header-only、零动态分配、无异常、不直接依赖 IOKit
//       （MMIO 通过函数指针 + void* 上下文注入），命名空间 fw，
//       可在用户态用 mock 回调编译测试。
//
// ⛔ **kext 环境可编译**（2026-09-29 CI run #220 修复）：本文件曾被编进 kext 路径后失败于
//    `fatal error: 'cstdint' file not found` —— kext 构建环境（MacKernelSDK）**没有 libc++**，
//    因此**禁止** `<cstdint>` / `<cstddef>` / `std::`；一律用 C 头 **`<stdint.h>` / `<stddef.h>`**
//    与全局类型名（`uint32_t` / `uint64_t`）。这与项目既有约定完全一致，见
//    `DisplaySeq/Dcn314DccgSeq.hpp:23` 与 `DisplaySeq/Dcn314OdmSeq.hpp:16` 的同句禁令；
//    离线测试侧由 `tests/IOKit/IOTypes.h` 提供 kext 类型替身，故用户态与 kext 两环境同源可编译。

#pragma once

#include "../DisplaySeq/RegSink.hpp"
#include "../DisplaySeq/RegOp.hpp"
#include "RegAddr.hpp"
#include <stdint.h>

namespace fw {

// PCIE 间接访问寄存器偏移（dword 单位，相对 NBIO_BASE_2）
// 依据：Linux amdgpu/nbio_v7_11.c:229-237 (get_pcie_index_offset / get_pcie_data_offset)
//       amdgpu_reg_access.c:33 (AMDGPU_PCIE_INDEX_HI_FALLBACK = 0x44 >> 2 = 0x11)
inline constexpr uint32_t kPcieIndex2Offset  = 0x0E;  // PCIE_INDEX2
inline constexpr uint32_t kPcieData2Offset   = 0x0F;  // PCIE_DATA2
inline constexpr uint32_t kPcieIndexHiOffset = 0x11;  // PCIE_INDEX_HI (0x44 >> 2)

// 回调集合：全部为普通函数指针 + 一个 void* 上下文（零分配，内核态可用）。
//   readReg / writeReg：MMIO 读写（必填）。
//   lock / unlock：       可为 nullptr（表示不需要锁）。
//   delayUs：             可为 nullptr（表示不延时）。
struct SmnCallbacks {
    uint32_t (*readReg)(void* ctx, uint32_t dwordOffset);
    void     (*writeReg)(void* ctx, uint32_t dwordOffset, uint32_t value);
    void     (*lock)(void* ctx);
    void     (*unlock)(void* ctx);
    void     (*delayUs)(void* ctx, uint32_t us);
    void*    ctx;
    uint32_t maxRetries = 3;
};

// 间接通道访问错误码
enum class SmnAccessError : uint32_t {
    None           = 0,
    LockFailed     = 1,
    IndexWriteFail = 2,  // 写索引后回读不匹配
    HiWriteFail    = 3,  // 写 HI 后回读不匹配
    DataWriteFail  = 4,  // 写数据后回读不匹配
    HiClearFail    = 5,  // 清 HI 后回读不为 0
    Timeout        = 6,  // 超时（轮询场景）
};

// 间接读结果
struct SmnReadResult {
    uint32_t value;
    SmnAccessError error;

    explicit operator bool() const { return error == SmnAccessError::None; }
    uint32_t operator*() const { return value; }
};

// 间接写结果
using SmnWriteResult = SmnAccessError;

// 核心间接访问原语（可被 RegSinkKernel 调用，也可被其它内核代码直接复用）
// 设计为静态函数，便于单元测试独立验证序列逻辑
class SmnIndirectAccess {
public:
    // 读取 SMN 寄存器（字节地址）
    // 返回值包含数据与错误码；error != None 表示失败
    static SmnReadResult read(uint64_t byteAddr, const SmnCallbacks& cb) {
        // Phoenix 地址 < 4GB，HI 恒为 0；保留判据以便将来复用
        const uint32_t hiVal = static_cast<uint32_t>((byteAddr >> 32) & 0xFF);
        const bool needHi = (byteAddr >> 32) != 0;
        const uint32_t indexVal = static_cast<uint32_t>(byteAddr & 0xFFFFFFFF);
        SmnAccessError lastErr = SmnAccessError::None;

        for (uint32_t attempt = 0; attempt <= cb.maxRetries; ++attempt) {
            SmnAccessError err = SmnAccessError::None;

            if (cb.lock) cb.lock(cb.ctx);

            // 1. 写索引寄存器（字节地址低 32 位）
            cb.writeReg(cb.ctx, kPcieIndex2Offset, indexVal);

            // 2. ★ 关键：回读索引寄存器（posted-write flush）
            uint32_t readBack = cb.readReg(cb.ctx, kPcieIndex2Offset);
            if (readBack != indexVal) {
                err = SmnAccessError::IndexWriteFail;
            }

            // 3. 若需要 HI：写 HI、回读 HI
            if (err == SmnAccessError::None && needHi) {
                cb.writeReg(cb.ctx, kPcieIndexHiOffset, hiVal);
                readBack = cb.readReg(cb.ctx, kPcieIndexHiOffset);
                if (readBack != hiVal) {
                    err = SmnAccessError::HiWriteFail;
                }
            }

            // 4. 读数据寄存器
            uint32_t data = 0;
            if (err == SmnAccessError::None) {
                data = cb.readReg(cb.ctx, kPcieData2Offset);
            }

            // 5. 若用了 HI：清 HI、回读 HI
            if (err == SmnAccessError::None && needHi) {
                cb.writeReg(cb.ctx, kPcieIndexHiOffset, 0);
                readBack = cb.readReg(cb.ctx, kPcieIndexHiOffset);
                if (readBack != 0) {
                    err = SmnAccessError::HiClearFail;
                }
            }

            if (cb.unlock) cb.unlock(cb.ctx);

            if (err == SmnAccessError::None) {
                return {data, SmnAccessError::None};
            }

            lastErr = err;
            // 重试前短延时（内核态用 IODelay/udelay，用户态测试可空实现）
            if (attempt < cb.maxRetries && cb.delayUs) {
                cb.delayUs(cb.ctx, 1);
            }
        }
        return {0xFFFFFFFF, lastErr};
    }

    // 写入 SMN 寄存器（字节地址）
    static SmnWriteResult write(uint64_t byteAddr, uint32_t value, const SmnCallbacks& cb) {
        const uint32_t hiVal = static_cast<uint32_t>((byteAddr >> 32) & 0xFF);
        const bool needHi = (byteAddr >> 32) != 0;
        const uint32_t indexVal = static_cast<uint32_t>(byteAddr & 0xFFFFFFFF);
        SmnAccessError lastErr = SmnAccessError::None;

        for (uint32_t attempt = 0; attempt <= cb.maxRetries; ++attempt) {
            SmnAccessError err = SmnAccessError::None;

            if (cb.lock) cb.lock(cb.ctx);

            // 1. 写索引寄存器
            cb.writeReg(cb.ctx, kPcieIndex2Offset, indexVal);

            // 2. ★ 关键：回读索引寄存器（posted-write flush）
            uint32_t readBack = cb.readReg(cb.ctx, kPcieIndex2Offset);
            if (readBack != indexVal) {
                err = SmnAccessError::IndexWriteFail;
            }

            // 3. 若需要 HI：写 HI、回读 HI
            if (err == SmnAccessError::None && needHi) {
                cb.writeReg(cb.ctx, kPcieIndexHiOffset, hiVal);
                readBack = cb.readReg(cb.ctx, kPcieIndexHiOffset);
                if (readBack != hiVal) {
                    err = SmnAccessError::HiWriteFail;
                }
            }

            // 4. 写数据寄存器
            if (err == SmnAccessError::None) {
                cb.writeReg(cb.ctx, kPcieData2Offset, value);

                // 5. ★ 写数据后回读数据（posted-write flush，Linux indirect_wreg_ext:812-814）
                readBack = cb.readReg(cb.ctx, kPcieData2Offset);
                if (readBack != value) {
                    err = SmnAccessError::DataWriteFail;
                }
            }

            // 6. 若用了 HI：清 HI、回读 HI
            if (err == SmnAccessError::None && needHi) {
                cb.writeReg(cb.ctx, kPcieIndexHiOffset, 0);
                readBack = cb.readReg(cb.ctx, kPcieIndexHiOffset);
                if (readBack != 0) {
                    err = SmnAccessError::HiClearFail;
                }
            }

            if (cb.unlock) cb.unlock(cb.ctx);

            if (err == SmnAccessError::None) {
                return SmnAccessError::None;
            }

            lastErr = err;
            if (attempt < cb.maxRetries && cb.delayUs) {
                cb.delayUs(cb.ctx, 1);
            }
        }

        return lastErr;
    }
};

// RegSinkKernel —— 实现 display::RegSink 的内核态实现类
// 通过构造函数注入 SmnCallbacks（函数指针 + void* 上下文），不依赖 IOKit 头文件
// 段基址：默认 SEG0（Apple cgs 现役已实证可达）；可通过 setSegmentBase 切换 SEG1（真机验证后）。
// 传入的地址为 dword 偏移（相对段基址）；内部加段基址后 ×4 得字节地址。
class RegSinkKernel final : public display::RegSink {
public:
    explicit RegSinkKernel(const SmnCallbacks& cb, uint32_t segBase = kMpSeg0Base)
        : segBase_(segBase), cb_(cb) {
        // 编译期断言：Phoenix 最大 SMN 地址 < 4GB，HI 恒不需要
        static_assert(fw::smnAddrWithBase(kMpSeg1Base, 0xFF) < (1ull << 32),
                      "Phoenix SMN addresses must fit in 32 bits");
    }

    // 设置段基址（dword 单位，如 kMpSeg0Base / kMpSeg1Base）。
    // 仅在构造后、首次 read/write 前调用；运行期不可重入。
    void setSegmentBase(uint32_t segBase) { segBase_ = segBase; }

    // 获取当前段基址（dword 单位）。
    uint32_t segmentBase() const { return segBase_; }

    // 禁止拷贝（含函数指针上下文，语义上不可共享）
    RegSinkKernel(const RegSinkKernel&) = delete;
    RegSinkKernel& operator=(const RegSinkKernel&) = delete;
    RegSinkKernel(RegSinkKernel&&) = default;
    RegSinkKernel& operator=(RegSinkKernel&&) = default;
    ~RegSinkKernel() override = default;

    // 读取寄存器（地址为 dword 偏移，相对段基址）。
    display::RegValue read(display::RegAddr dwordOffset) override {
        const uint64_t byteAddr = (static_cast<uint64_t>(segBase_) + dwordOffset) * 4;
        auto result = SmnIndirectAccess::read(byteAddr, cb_);
        lastValue_ = result.value;
        lastError_ = result.error;
        return result.value;
    }

    // 写入寄存器（地址为 dword 偏移，相对段基址）。
    void write(display::RegAddr dwordOffset, display::RegValue value) override {
        const uint64_t byteAddr = (static_cast<uint64_t>(segBase_) + dwordOffset) * 4;
        lastError_ = SmnIndirectAccess::write(byteAddr, value, cb_);
        lastValue_ = value;
    }



    // —— 64 位 SMN 字节地址专用接口（供 PSP/SMU bringup 直接调用）——
    // RegSink 基类接口仅支持 u32，SMN 间接访问可能需要 48 位地址（HI 处理）
    SmnReadResult read64(uint64_t byteAddr) {
        auto result = SmnIndirectAccess::read(byteAddr, cb_);
        lastValue_ = result.value;
        lastError_ = result.error;
        return result;
    }

    SmnWriteResult write64(uint64_t byteAddr, uint32_t value) {
        lastError_ = SmnIndirectAccess::write(byteAddr, value, cb_);
        lastValue_ = value;
        return lastError_;
    }

    // 延时（微秒）：内核态用 IODelay/udelay，用户态测试可空实现
    void delayMicroseconds(uint32_t us) override {
        if (delayFn_) { delayFn_(delayCtx_, us); }
        else if (cb_.delayUs) { cb_.delayUs(cb_.ctx, us); }
    }

    // 设置延时回调（可选，用于测试控制时间流逝）
    // 零分配形态：函数指针 + 独立上下文（不修改 cb_.ctx，避免影响其它回调）
    void setDelay(void (*fn)(void* ctx, uint32_t us), void* ctx = nullptr) {
        delayFn_ = fn;
        delayCtx_ = ctx;
    }

    // 获取最后读/写的值（本类自行维护，不依赖基类私有成员）
    display::RegValue lastValue() const { return lastValue_; }
    // 获取最后一次操作的错误码
    SmnAccessError lastError() const { return lastError_; }

    uint32_t segBase_{kMpSeg0Base};
    SmnCallbacks cb_;
    void (*delayFn_)(void*, uint32_t) = nullptr;
    void* delayCtx_ = nullptr;
    SmnAccessError lastError_{SmnAccessError::None};
    display::RegValue lastValue_{0};
};

}  // namespace fw
