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
     * Probe D1 v2: SMU13 上电序列每步结果的累积状态（旁路记录，不改变任何原有行为）。
     * 编码：bit0-5 = 已执行步掩码；bit8-15 / 16-23 / 24-31 / 32-39 = step0..3 的返回码（CAILResult 低 8 位）。
     * 由 HWLibs 的 smu13PowerUpConfig 累积，在 X6000FB 的 wrapHandleCriticalError（真崩溃出口）读出并注入 panic 消息。
     */
    UInt64 getSmu13ProbeState() const { return this->smu13ProbeState; }
    void   setSmu13ProbeState(UInt64 v) { this->smu13ProbeState = v; }
    void   orSmu13ProbeState(UInt64 v) { this->smu13ProbeState |= v; }

private:
    UInt64 smu13ProbeState{0};
};
