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

public:
    static NRed& singleton();

    auto& getAttributes() const { return this->attributes; }    // TODO: Remove!
    auto  getDeviceID() const { return deviceID; }              // TODO: Remove!
    auto  getPciRevision() const { return pciRevision; }        // TODO: Remove!
    auto  getDevRevision() const { return devRevision; }        // TODO: Remove!
    auto  getEnumRevision() const { return enumRevision; }      // TODO: Remove!
    auto  getFbOffset() const { return fbOffset; }              // TODO: Remove!
    IOPCIDevice* getIGPU() const { return this->iGPU; }         // §16.52: HWLibs 需映射 BAR0 写驱动表

    void init();
    void hwLateInit();        // TODO: Remove!
    void processPatcher();    // TODO: Remove!

    void   setProp32(const char* key, UInt32 value) const;    // TODO: Remove!
    UInt32 readReg32(UInt32 reg) const;                       // TODO: Remove!
    void   writeReg32(UInt32 reg, UInt32 value) const;        // TODO: Remove!

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
