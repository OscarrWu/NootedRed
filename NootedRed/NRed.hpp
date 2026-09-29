// Master Logic
//
// Copyright © 2022-2025 ChefKiss. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once
#include <GPUDriversAMD/ATOMBIOS.hpp>
#include <GPUDriversAMD/CAIL/Result.hpp>
#include <GPUDriversAMD/PowerPlay.hpp>
#include <Headers/kern_patcher.hpp>
#include <IOKit/pci/IOPCIDevice.h>

class NRed
{
    class Attributes
    {    // TODO: Remove!
        static constexpr UInt8 IsPicasso      = getBit(0);
        static constexpr UInt8 IsRaven2       = getBit(1);
        static constexpr UInt8 IsRenoir       = getBit(2);
        static constexpr UInt8 IsRenoirE      = getBit(3);
        static constexpr UInt8 IsGreenSardine = getBit(4);
        static constexpr UInt8 IsPhoenix      = getBit(5);

        UInt8 value{0};

    public:
        constexpr bool isPicasso() const { return (this->value & IsPicasso) != 0; }
        constexpr bool isRaven2() const { return (this->value & IsRaven2) != 0; }
        constexpr bool isRenoir() const { return (this->value & IsRenoir) != 0; }
        constexpr bool isRenoirE() const { return (this->value & IsRenoirE) != 0; }
        constexpr bool isGreenSardine() const { return (this->value & IsGreenSardine) != 0; }
        constexpr bool isPhoenix() const { return (this->value & IsPhoenix) != 0; }

        constexpr void setPicasso() { this->value |= IsPicasso; }
        constexpr void setRaven2() { this->value |= IsRaven2; }
        constexpr void setRenoir() { this->value |= IsRenoir; }
        constexpr void setRenoirE() { this->value |= IsRenoirE; }
        constexpr void setGreenSardine() { this->value |= IsGreenSardine; }
        constexpr void setPhoenix() { this->value |= IsPhoenix; }
    };

    Attributes       attributes;           // TODO: Remove!
    IOPCIDevice*     iGPU{nullptr};        // TODO: Remove!
    IOMemoryMap*     rmmio{nullptr};       // TODO: Remove!
    volatile UInt32* rmmioPtr{nullptr};    // TODO: Remove!
    UInt16           deviceID{0};          // TODO: Remove!
    UInt8            pciRevision{0};       // TODO: Remove!
    UInt16           devRevision{0};       // TODO: Remove!
    UInt16           enumRevision{0};      // TODO: Remove!
    UInt64           fbOffset{0};          // TODO: Remove!
    // §简化项 15：Apple 侧认定的 VRAM 基址（`IOFramebuffer::getVRAMRange()` 的地址）。
    // 由 X5000::fixedGetDisplayInfo 捕获；与 fbOffset 的消费方（Apple 的地址换算）天然同源。
    UInt64           fbLocationBase{0};

public:
    static NRed& singleton();

    auto& getAttributes() const { return this->attributes; }    // TODO: Remove!
    auto  getDeviceID() const { return deviceID; }              // TODO: Remove!
    auto  getPciRevision() const { return pciRevision; }        // TODO: Remove!
    auto  getDevRevision() const { return devRevision; }        // TODO: Remove!
    auto  getEnumRevision() const { return enumRevision; }      // TODO: Remove!
    auto  getFbOffset() const { return fbOffset; }              // TODO: Remove!
    auto  getFbLocationBase() const { return this->fbLocationBase; }     // §简化项 15
    void  setFbLocationBase(const UInt64 v) { this->fbLocationBase = v; }    // §简化项 15
    IOPCIDevice* getIGPU() const { return this->iGPU; }         // §16.52: HWLibs 需映射 BAR0 写驱动表

    void init();
    void hwLateInit();        // TODO: Remove!
    void processPatcher();    // TODO: Remove!

    void   setProp32(const char* key, UInt32 value) const;    // TODO: Remove!
    UInt32 readReg32(UInt32 reg) const;                       // TODO: Remove!
    void   writeReg32(UInt32 reg, UInt32 value) const;        // TODO: Remove!

    /**
     * 忠实复刻 Linux `amdgpu_device_indirect_rreg` 的 **SMN 间接读**（越窗寄存器用）。
     *
     * 与 `readReg32` 间接分支的**唯一区别**：写完 `PCIE_INDEX2` 后**回读一次 INDEX2**
     * （posted-write flush）——Linux `amdgpu_reg_access.c:647-648` 正是如此；我们的旧实现缺这一步。
     * Phoenix 的 NBIO（`amdgpu/nbio_v7_11.c:229-237`）只提供 INDEX2/DATA2、**没有**
     * `get_pcie_index_hi_offset` ⇒ Linux 侧 `pcie_index_hi` 恒为 0 ⇒ **无需 INDEX_HI 处理**
     * （这也是本原语省略它的依据）。
     *
     * @param addr **字节地址** = `(段基址[reg##_BASE_IDX] + 寄存器偏移) * 4 + smn_base`
     *             （公式与依据见 `FwBringup/RegAddr.hpp`）
     */
    UInt32 readReg32Ext(UInt32 addr) const;

    /**
     * 乙线探针用的**最小公开 MMIO 直读/直写**（2026-09-29 加）。
     *
     * 为什么需要它：`FwBringup/RegSinkKernel.hpp` 的 `SmnCallbacks` 需要一对"按 dword 偏移
     * 读写 BAR5"的裸回调（它自己负责 SMN 间接序列：写索引 → 回读校验 → 读数据）。
     * 调用点（`X6000FB.cpp` 的 `-NRedSmnRead1` 探针）必须能拿到这对回调，而
     * `rmmioPtr` 是**私有成员**（`NRed.hpp:45`，`public:` 之前）⇒ 加这两个薄访问器。
     *
     * ⚠️ 语义边界（务必分清，勿误用）：
     *   · 本访问器**只做** `rmmioPtr[dwordOffset]` 的裸访问，**不含**任何越窗判断、
     *     **不含** SMN 间接通道（`PCIE_INDEX2/DATA2`）逻辑——那是 `readReg32Ext` 的职责；
     *     若把 SMN 字节地址直接丢进来，它会被当成 dword 索引（正是历史
     *     `readReg32` 间接分支那个 bug 的形态）。
     *   · 仅供**已获所有者批准的**探针/规范访问器使用；常规功能路径请用
     *     `readReg32`/`writeReg32`/`readReg32Ext`。
     *
     * @param dwordOffset 相对 BAR5 映射基址的 **dword 索引**（不是字节地址）
     *
     * ⛔ 调用方**必须**先用 `hasRmmio()` 确认已映射（`hwLateInit()` 之前为 false，
     *    未映射时本函数会返回 0 / 空写而不是崩溃，但语义上那是"读不到"）。
     */
    bool   hasRmmio() const { return this->rmmioPtr != nullptr; }
    UInt32 readReg32Raw(UInt32 dwordOffset) const {
        return this->rmmioPtr != nullptr ? this->rmmioPtr[dwordOffset] : 0;
    }
    void writeReg32Raw(UInt32 dwordOffset, UInt32 value) const {
        if (this->rmmioPtr != nullptr) { this->rmmioPtr[dwordOffset] = value; }
    }

    /// 映射窗口长度（**以 dword 计**）——供探针做"已知值 oracle"扫描用；未映射时返回 0。
    UInt32 getRmmioLengthDw() const {
        return this->rmmio != nullptr ? static_cast<UInt32>(this->rmmio->getLength() / sizeof(UInt32)) : 0;
    }

    /**
     * Probe：SMU13 相关路径的累积探针状态（旁路记录，不改变任何原有行为）。
     *
     * ⚠️ 两个写入者共用这一个变量，位域**曾经相交**，2026-09-25 已解冲突：
     *   · **ctx 路径**（smu13PowerUpConfig，走 Apple SMU 函数指针）：
     *       bit0-3 = 已执行步掩码；bit60-63 = 生命周期
     *       （`63`=smu13InternalHwInit 被调用、`62/61`=WaitForFwLoaded 成败、`60`=消息 hook 触发）
     *       *原 bit8-39 的四步 rc 已退役*——与旁路 rc 域相交，而 ctx 路径已被证实从未执行
     *       （入口无 call 点、route 必然失败），故让位给主力路径。每步 rc 仍见内核日志。
     *   · **SMU 旁路**（wrapControllerPowerUp + smu13SetupDriverTableAndTransfer，`-NRedSmuBypass` 门控）：
     *       bit4=HDP flush、bit5=aperture、bit6=地址域校验拒绝、bit56-59=被拒地址档位；
     *       bit20/21=表+Transfer/Imu 成功、bit24/25/26/27=对应失败、bit29=carve-out 地址；
     *       bit32-39=表+Transfer 步 rc、bit40-47=Imu 步 rc、bit48-55=AddrHigh/Low 步 rc。
     *
     * 判读：先看 bit60-63 判断 ctx 路径是否跑过（当前恒为 0 ⇒ 所有位置均为旁路语义）。
     * 在 X6000FB 的 wrapHandleCriticalError（真崩溃出口）读出并注入 panic 消息。
     */
    // §16.67：驱动表序列各步的真实 resp（发消息时立刻记录；panic 时读的是过期值）
    UInt32 smu13Resp[8] = {0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU,
                           0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU};
    UInt64 getSmu13ProbeState() const { return this->smu13ProbeState; }
    void   setSmu13ProbeState(UInt64 v) { this->smu13ProbeState = v; }
    void   orSmu13ProbeState(UInt64 v) { this->smu13ProbeState |= v; }

private:
    UInt64 smu13ProbeState{0};
};
