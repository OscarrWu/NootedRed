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
// 约束：header-only、零动态分配、不直接依赖 IOKit（MMIO 通过函数指针/模板注入），
//       命名空间 fw，可在用户态用 mock 回调编译测试。

#pragma once

#include "../DisplaySeq/RegSink.hpp"
#include "../DisplaySeq/RegOp.hpp"
#include "RegAddr.hpp"
#include <cstdint>
#include <functional>

namespace fw {

// PCIE 间接访问寄存器偏移（dword 单位，相对 NBIO_BASE_2）
// 依据：Linux amdgpu/nbio_v7_11.c:229-237 (get_pcie_index_offset / get_pcie_data_offset)
//       amdgpu_reg_access.c:33 (AMDGPU_PCIE_INDEX_HI_FALLBACK = 0x44 >> 2 = 0x11)
inline constexpr uint32_t kPcieIndex2Offset  = 0x0E;  // PCIE_INDEX2
inline constexpr uint32_t kPcieData2Offset   = 0x0F;  // PCIE_DATA2
inline constexpr uint32_t kPcieIndexHiOffset = 0x11;  // PCIE_INDEX_HI (0x44 >> 2)

// MMIO 读写回调类型：内核态传真实 MMIO 指针，用户态测试传 mock 函数
using MmioReadFn  = std::function<uint32_t(uint32_t dwordOffset)>;
using MmioWriteFn = std::function<void(uint32_t dwordOffset, uint32_t value)>;

// 锁回调类型：内核态用 IOLock，用户态测试用空实现或 std::mutex
using LockFn   = std::function<void()>;
using UnlockFn = std::function<void()>;

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
    static SmnReadResult read(uint64_t byteAddr,
                              const MmioReadFn& mmioRead,
                              const MmioWriteFn& mmioWrite,
                              const LockFn& lock,
                              const UnlockFn& unlock,
                              uint32_t maxRetries = 3) {
        // Phoenix 地址 < 4GB，HI 恒为 0；保留判据以便将来复用
        const uint32_t hiVal = static_cast<uint32_t>((byteAddr >> 32) & 0xFF);
        const bool needHi = (byteAddr >> 32) != 0;
        const uint32_t indexVal = static_cast<uint32_t>(byteAddr & 0xFFFFFFFF);
        SmnAccessError lastErr = SmnAccessError::None;
        
        for (uint32_t attempt = 0; attempt <= maxRetries; ++attempt) {
            SmnAccessError err = SmnAccessError::None;
            
            lock();
            
            // 1. 写索引寄存器（字节地址低 32 位）
            mmioWrite(kPcieIndex2Offset, indexVal);
            
            // 2. ★ 关键：回读索引寄存器（posted-write flush）
            uint32_t readBack = mmioRead(kPcieIndex2Offset);
            if (readBack != indexVal) {
                err = SmnAccessError::IndexWriteFail;
            }
            
            // 3. 若需要 HI：写 HI、回读 HI
            if (err == SmnAccessError::None && needHi) {
                mmioWrite(kPcieIndexHiOffset, hiVal);
                readBack = mmioRead(kPcieIndexHiOffset);
                if (readBack != hiVal) {
                    err = SmnAccessError::HiWriteFail;
                }
            }
            
            // 4. 读数据寄存器
            uint32_t data = 0;
            if (err == SmnAccessError::None) {
                data = mmioRead(kPcieData2Offset);
            }
            
            // 5. 若用了 HI：清 HI、回读 HI
            if (err == SmnAccessError::None && needHi) {
                mmioWrite(kPcieIndexHiOffset, 0);
                readBack = mmioRead(kPcieIndexHiOffset);
                if (readBack != 0) {
                    err = SmnAccessError::HiClearFail;
                }
            }
            
            unlock();
            
            if (err == SmnAccessError::None) {
                return {data, SmnAccessError::None};
            }
            
            lastErr = err;
            // 重试前短延时（内核态用 IOSleep/udelay，用户态测试可空实现）
            if (attempt < maxRetries) {
                // 延时留给调用方通过 lock/unlock 回调实现，或在此处加平台相关睡眠
            }
        }
        return {0xFFFFFFFF, lastErr};
    }
    
    // 写入 SMN 寄存器（字节地址）
    static SmnWriteResult write(uint64_t byteAddr,
                                uint32_t value,
                                const MmioReadFn& mmioRead,
                                const MmioWriteFn& mmioWrite,
                                const LockFn& lock,
                                const UnlockFn& unlock,
                                uint32_t maxRetries = 3) {
        const uint32_t hiVal = static_cast<uint32_t>((byteAddr >> 32) & 0xFF);
        const bool needHi = (byteAddr >> 32) != 0;
        const uint32_t indexVal = static_cast<uint32_t>(byteAddr & 0xFFFFFFFF);
        SmnAccessError lastErr = SmnAccessError::None;
        
        for (uint32_t attempt = 0; attempt <= maxRetries; ++attempt) {
            SmnAccessError err = SmnAccessError::None;
            
            lock();
            
            // 1. 写索引寄存器
            mmioWrite(kPcieIndex2Offset, indexVal);
            
            // 2. ★ 关键：回读索引寄存器（posted-write flush）
            uint32_t readBack = mmioRead(kPcieIndex2Offset);
            if (readBack != indexVal) {
                err = SmnAccessError::IndexWriteFail;
            }
            
            // 3. 若需要 HI：写 HI、回读 HI
            if (err == SmnAccessError::None && needHi) {
                mmioWrite(kPcieIndexHiOffset, hiVal);
                readBack = mmioRead(kPcieIndexHiOffset);
                if (readBack != hiVal) {
                    err = SmnAccessError::HiWriteFail;
                }
            }
            
            // 4. 写数据寄存器
            if (err == SmnAccessError::None) {
                mmioWrite(kPcieData2Offset, value);
                
                // 5. ★ 写数据后回读数据（posted-write flush，Linux indirect_wreg_ext:812-814）
                readBack = mmioRead(kPcieData2Offset);
                if (readBack != value) {
                    err = SmnAccessError::DataWriteFail;
                }
            }
            
            // 6. 若用了 HI：清 HI、回读 HI
            if (err == SmnAccessError::None && needHi) {
                mmioWrite(kPcieIndexHiOffset, 0);
                readBack = mmioRead(kPcieIndexHiOffset);
                if (readBack != 0) {
                    err = SmnAccessError::HiClearFail;
                }
            }
            
            unlock();
            
            if (err == SmnAccessError::None) {
                return SmnAccessError::None;
            }
            
            lastErr = err;
            if (attempt < maxRetries) {
                // 重试延时同上
            }
        }
        
        return lastErr;
    }
};

// RegSinkKernel —— 实现 display::RegSink 的内核态实现类
// 通过构造函数注入 MMIO 读写与锁回调，不依赖 IOKit 头文件
class RegSinkKernel final : public display::RegSink {
public:
    // 回调集合（内核态实例化时传入真实 MMIO 指针与 IOLock）
    struct Callbacks {
        MmioReadFn  mmioRead;
        MmioWriteFn mmioWrite;
        LockFn      lock;
        UnlockFn    unlock;
        uint32_t    maxRetries = 3;  // 间接访问重试上限
    };
    
    explicit RegSinkKernel(const Callbacks& cb) : cb_(cb) {
        // 编译期断言：Phoenix 最大 SMN 地址 < 4GB，HI 恒不需要
        static_assert(fw::smnAddr(0xFF) < (1ull << 32), "Phoenix SMN addresses must fit in 32 bits");
    }
    
    // 禁止拷贝（含有 std::function）
    RegSinkKernel(const RegSinkKernel&) = delete;
    RegSinkKernel& operator=(const RegSinkKernel&) = delete;
    RegSinkKernel(RegSinkKernel&&) = default;
    RegSinkKernel& operator=(RegSinkKernel&&) = default;
    ~RegSinkKernel() override = default;
    
    // 读取寄存器（地址为 display::RegAddr = uint32_t，此处按字节地址传入）
    display::RegValue read(display::RegAddr byteAddr) override {
        auto result = SmnIndirectAccess::read(byteAddr, cb_.mmioRead, cb_.mmioWrite,
                                              cb_.lock, cb_.unlock, cb_.maxRetries);
        lastValue_ = result.value;
        lastError_ = result.error;
        return result.value;
    }
    
    // 写入寄存器
    void write(display::RegAddr byteAddr, display::RegValue value) override {
        lastError_ = SmnIndirectAccess::write(byteAddr, value, cb_.mmioRead, cb_.mmioWrite,
                                              cb_.lock, cb_.unlock, cb_.maxRetries);
        lastValue_ = value;
    }
    
    // —— 64 位 SMN 字节地址专用接口（供 PSP/SMU bringup 直接调用）——
    // RegSink 基类接口仅支持u32，SMN 间接访问可能需要 48 位地址（HI 处理）
    SmnReadResult read64(uint64_t byteAddr) {
        auto result = SmnIndirectAccess::read(byteAddr, cb_.mmioRead, cb_.mmioWrite,
                                              cb_.lock, cb_.unlock, cb_.maxRetries);
        lastValue_ = result.value;
        lastError_ = result.error;
        return result;
    }
    
    SmnWriteResult write64(uint64_t byteAddr, uint32_t value) {
        lastError_ = SmnIndirectAccess::write(byteAddr, value, cb_.mmioRead, cb_.mmioWrite,
                                              cb_.lock, cb_.unlock, cb_.maxRetries);
        lastValue_ = value;
        return lastError_;
    }
    
    // 延时（微秒）：内核态用 IODelay/udelay，用户态测试可空实现
    void delayMicroseconds(uint32_t us) override {
        if (delayFn_) { delayFn_(us); }
    }
    
    // 设置延时回调（可选，用于测试控制时间流逝）
    void setDelayFn(std::function<void(uint32_t)> fn) { delayFn_ = std::move(fn); }
    
    // 获取最后一次操作的错误码
    SmnAccessError lastError() const { return lastError_; }
    
    // 获取最后读/写的值（本类自行维护，不依赖基类私有成员）
    display::RegValue lastValue() const { return lastValue_; }
    
private:
    Callbacks cb_;
    std::function<void(uint32_t)> delayFn_;
    SmnAccessError lastError_{SmnAccessError::None};
    display::RegValue lastValue_{0};
};

}  // namespace fw