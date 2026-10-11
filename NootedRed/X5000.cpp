// AMDRadeonX5000 Patches
//
// Copyright © 2022-2025 ChefKiss. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#include <AMDGFX9DCN1Display.hpp>
#include <AMDGFX9DCN2Display.hpp>
#include <AMDGFX9DCN314Display.hpp>
#include <AMDGFX9DCNDisplay.hpp>
#include <GPUDriversAMD/Accel/HWDisplay.hpp>
#include <GPUDriversAMD/Accel/HWEngine.hpp>
#include <GPUDriversAMD/AddrLib.hpp>
#include <GPUDriversAMD/FB/Attributes.hpp>
#include <GPUDriversAMD/Family.hpp>
#include <Headers/kern_mach.hpp>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>
#include <Kexts.hpp>
#include <NRed.hpp>
#include <PenguinWizardry/KernelVersion.hpp>
#include <PenguinWizardry/PatcherPlus.hpp>
#include <X5000.hpp>
#include <NRedWindowProbe.hpp>   // A-25：first-false 窗口法纯逻辑
#include <HWLibs.hpp>            // NRED_TRACE（A-25：win-probe 输出通道）
#include <libkern/OSTypes.h>
#include <libkern/c++/OSObject.h>
#include <libkern/c++/OSIterator.h>   // A-47：`IOService::getClientIterator()` 的遍历（client 采样）
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSMetaClass.h>
#include <string.h>         // strcmp
#include <mach/i386/vm_param.h>
#include <mach/i386/vm_types.h>
#include <mach/kern_return.h>

static const UInt8 kChannelTypesPattern[] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
                                             0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00};

static const UInt8 kHwlConvertChipFamilyPattern[] = {0x81, 0xFE, 0x8D, 0x00, 0x00, 0x00, 0x0F};

// Make for loop stop after one SDMA engine.
static const UInt8 kStartHWEnginesOriginal[] = {0x40, 0x83, 0xF0, 0x02};
static const UInt8 kStartHWEnginesMask[]     = {0xF0, 0xFF, 0xF0, 0xFF};
static const UInt8 kStartHWEnginesPatched[]  = {0x40, 0x83, 0xF0, 0x01};

// The check in `Addr::Lib::Create` on <=10.15 and 13.4+ is `familyId == 0x8D` instead of `familyId - 0x8D < 2`.
// Change the 0x8D (AI) to 0x8E (RV).
static const UInt8 kAddrLibCreateOriginal[] = {0x41, 0x81, 0x7D, 0x08, 0x8D, 0x00, 0x00, 0x00};
static const UInt8 kAddrLibCreatePatched[]  = {0x41, 0x81, 0x7D, 0x08, 0x8E, 0x00, 0x00, 0x00};

// For some reason Navi (`0x8F`) was added in 14.4 here. Lazy copy pasting?
static const UInt8 kAddrLibCreateOriginal1404[] = {0x41, 0x8B, 0x46, 0x08, 0x3D, 0x8F, 0x00, 0x00, 0x00, 0x74, 0x00,
                                                   0x3D, 0x8D, 0x00, 0x00, 0x00, 0x0F, 0x85, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kAddrLibCreateOriginalMask1404[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                       0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                       0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kAddrLibCreatePatched1404[]     = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                      0x00, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kAddrLibCreatePatchedMask1404[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                      0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Catalina only. Change loop condition to skip SDMA1_HP.
static const UInt8 kCreateAccelChannelsOriginal[] = {0x8D, 0x44, 0x09, 0x02};
static const UInt8 kCreateAccelChannelsPatched[]  = {0x8D, 0x44, 0x09, 0x01};

// Ditto, Mojave.
static const UInt8 kCreateAccelChannelsOriginal10_14[] = {0x8D, 0x04, 0x09, 0x8D, 0x4C, 0x09, 0x02};
static const UInt8 kCreateAccelChannelsPatched10_14[]  = {0x8D, 0x04, 0x09, 0x8D, 0x4C, 0x09, 0x01};

static X5000 moduleInstance;

X5000& X5000::singleton() { return moduleInstance; }

X5000::X5000()
{
    if (currentKernelVersion() <= MACOS_10_14_X) {
        this->pm4EngineField               = 0x330;
        this->sdma0EngineField             = 0x338;
        this->supportedDisplayCountField   = 0x2C;
        this->seCountField                 = 0x58;
        this->shCountField                 = 0x5C;
        this->hwMaxCUsField                = 0x80;
        this->hasUVD0Field                 = 0x90;
        this->hasVCEField                  = 0x92;
        this->hasVCN0Field                 = 0x93;
        this->hasSDMAPagingQueueField      = 0xA4;
        this->hasGetAllClockLimitsField    = 0xA3;
        this->familyTypeField              = 0x29C;
        this->chipSettingsField            = 0x5B18;
        this->hwChannelHWInterfaceField    = 0x18;
        this->hwChannelSubmitCommandBuffer = 0x180;
    }
    else if (currentKernelVersion().majorMatches(MACOS_10_15)) {
        this->pm4EngineField               = 0x348;
        this->sdma0EngineField             = 0x350;
        this->supportedDisplayCountField   = 0x2C;
        this->seCountField                 = 0x58;
        this->shCountField                 = 0x5C;
        this->hwMaxCUsField                = 0x80;
        this->hasUVD0Field                 = 0x90;
        this->hasVCEField                  = 0x92;
        this->hasVCN0Field                 = 0x93;
        this->hasSDMAPagingQueueField      = 0xA4;
        this->hasGetAllClockLimitsField    = 0xA3;
        this->dccDisplayableSupportField   = 0xA5;
        this->familyTypeField              = 0x2B4;
        this->chipSettingsField            = 0x5B18;
        this->hwChannelHWInterfaceField    = 0x18;
        this->hwChannelSubmitCommandBuffer = 0x188;
    }
    else {
        this->pm4EngineField    = 0x3B8;
        this->sdma0EngineField  = 0x3C0;
        this->familyTypeField   = 0x308;
        this->chipSettingsField = 0x5B10;

        if (currentKernelVersion() <= MACOS_12_X) {
            this->supportedDisplayCountField   = 0x2C;
            this->seCountField                 = 0x5C;
            this->shCountField                 = 0x64;
            this->hwMaxCUsField                = 0x98;
            this->hasUVD0Field                 = 0xAC;
            this->hasVCEField                  = 0xAE;
            this->hasVCN0Field                 = 0xAF;
            this->hasSDMAPagingQueueField      = 0xC0;
            this->hasGetAllClockLimitsField    = 0xBF;
            this->dccDisplayableSupportField   = 0xC1;
            this->hwChannelHWInterfaceField    = 0x18;
            this->hwChannelSubmitCommandBuffer = 0x190;
        }
        else {
            this->supportedDisplayCountField = 0x34;
            this->seCountField               = 0x64;
            this->shCountField               = 0x6C;
            this->hwMaxCUsField              = 0xA0;
            this->hasUVD0Field               = 0xB4;
            this->hasVCEField                = 0xB6;
            this->hasVCN0Field               = 0xB7;
            this->hasSDMAPagingQueueField    = 0xBF;
            this->hasGetAllClockLimitsField  = 0xBE;
            this->dccDisplayableSupportField = 0xC0;
            this->hwChannelHWInterfaceField  = 0x20;
            if (currentKernelVersion() >= MACOS_14) { this->hwChannelSubmitCommandBuffer = 0x188; }
            else {
                this->hwChannelSubmitCommandBuffer = 0x190;
            }
        }
    }
}

// 第八步观测：保存 X5000 kext 的 slide，供探针在运行时定位其内部符号（如 probe 用的属性名 OSSymbol）
UInt64 gAccelProbeProv    = 0;    // 最后一次 provider 指针

// ─── R1' 最小读数探针（`-NRedR1Probe`，默认关）───────────────────────────────
//  目的：判定 `X5000::configureDevice` 究竟从**哪个失败出口**返回（该方法有 6 条以上失败出口，
//  此前把观测坐标当成"唯一检查点"，导致结论无法收敛）。
//  依据（主 agent 2026-09-29 逐条核对真机 13.6 二进制；符号锚定：`__text` 归零 vm=0xf60，
//  0x3306 恰为符号 `AMDRadeonX5000_AMDGraphicsAccelerator::configureDevice` 入口）：
//    · 返回值寄存器 %r15 在全函数内**只被写 4 次**：0x3343 初始清零 / 0x35fb 置 1 /
//      0x3620 清零（aux-power 形状检查失败）/ 0x368b 清零（早退族公共出口）。
//    · 早退族 = 5 条 `je 0x368b`：0x33b7(f1f40==0) / 0x33ee(f1f28==0) / 0x342f(f1a68==0)
//      / 0x344d(f1a40==0) / 0x346e(f1a38==0)；另有 0x3354（设备 ID 读回 0xffff）与 0x333a（provider==0）。
//      ⇒ **每个出口都留下一个刚被写空/未写入的特征字段**，故读这几个字段即可一轮定出口。
//    · 早退路径会执行 0x3696 的 `and $~0x30` + `or $0x20` ⇒ **f1e88 的 bit5 是"早退族"的印记**；
//      aux 分支的特征是 **f1f10 被显式清 0**（0x3614）。
//  纪律（手册 §9.1 允许形态）：**纯内存字段读**——不读任何 GPU/SMN 寄存器、不调用任何 Apple
//    方法、不写任何内存。故 panic 时刻无需任何额外操作，也不会引入锁/阻塞风险。
bool        gR1bProbeEnabled     = false;
UInt64      gR1bCfgDevCalls      = 0;      // configureDevice 被调用次数
UInt64      gR1bCfgDevRetNZ      = 0;      // 返回值为"真"的次数
UInt64      gR1bCfgDevSelf       = 0;      // 最后一次的 this
UInt64      gR1bCfgDevProvider   = 0;      // 最后一次的 provider（IOPCIDevice*）
UInt64      gR1bCfgDevRet        = 0;      // 最后一次的返回值
UInt64      gR1bZeroMask         = 0;      // bit0..4 = f1f40/f1f28/f1a68/f1a40/f1a38 曾出现 0
UInt64      gR1bF1F40            = 0;      // this+0x1F40（framebuffer 服务；0 ⇒ 0x33b7 出口）
UInt64      gR1bF1F28            = 0;      // this+0x1F28（0 ⇒ 0x33ee 出口）
UInt64      gR1bF1F30            = 0;      // this+0x1F30
UInt64      gR1bF1A68            = 0;      // this+0x1A68（0 ⇒ 0x342f 出口）
UInt64      gR1bF1A40            = 0;      // this+0x1A40（0 ⇒ 0x344d 出口）
UInt64      gR1bF1A38            = 0;      // this+0x1A38（0 ⇒ 0x346e 出口）
UInt64      gR1bF1F10            = 0;      // this+0x1F10（aux 分支失败时被显式清 0）
UInt64      gR1bF1E88            = 0;      // this+0x1E88（bit5 = 早退族印记）
UInt64      gR1bF368             = 0;      // this+0x368
UInt64      gR1bObj1A38Vtable    = 0;      // f1a38 所指对象的 vtable 指针（纯读，供离线定名）
UInt64      gR1bKeyObjFirstField = 0;      // *0x1ed118 所指对象的首字段

// ─── R1'-B30 身份探针（`-NRedR1B30Probe`，默认关）─────────────────────────────
//  目的（规格书 = `docs/子任务/乙线R1探针判读报告.md` §4；并由
//  `docs/子任务/乙线configureDevice-stub解析报告.md` §9 收窄）：
//  第 78 轮已把 `configureDevice` 的失败出口钉死为 `0x346e`，即
//  `call *0xb30(this, provider)` 返回 0。§9.1 已离线解出该槽的**静态**目标 =
//  `__ZN37AMDRadeonX5000_AMDGraphicsAccelerator23createStatisticsManagerEv`（本 kext，归零 vm 0x61be），
//  且已核实真机绑定的子类 `AMDRadeonX5000_AMDVega10GraphicsAccelerator` **未覆写**这三槽。
//  §9.2 进一步把"返回 0"归到两条路径：
//    A. `0x61dd test %rbx,%rbx / je 0x61fe`：分配器（经 stub 0x903c）返回 NULL；
//    B. `0x61eb call *0x118(%rax) / test %al,%al / jne 0x6200`：新对象 `vtable[0x118]`（槽 0x23）返回 false
//       ⇒ `0x61fb call *0x28`（release）⇒ 归零。兄弟槽 0xb20/0xb28 每次拿到新鲜堆指针 ⇒ 倾向路径 B。
//  本探针要**用一次只读读数**把这两点钉死：① `this` 的 vptr 归零 vm（验证"运行时 vptr == 静态 _ZTV + slide"）；
//  ② 三个槽的**运行时目标**地址（与离线静态值对照）；③ provider 上的两个属性（`IOMatchCategory` /
//  `AAPL,aux-power-connected` 三要素）；④ 兄弟槽结果对象的 vptr 归零 vm（给 0xb20/0xb28 定名）。
//
//  纪律（手册 §9.1 允许形态，逐条）：
//    · **纯内存字段读 + IORegistry 属性读**：不读任何 GPU/SMN 寄存器；
//    · **不调用任何 Apple 方法**——包括不通过 `call *槽` 调用 vtable（规则明令），因此路径 B 的
//      "谁返回了 false"**不在本探针内取**，改为读三个槽的运行时目标地址后**离线定名**（见报告）；
//    · **不写任何内存**（除本组全局量自身）；默认关时**零副作用**（`checkKernelArgument` 一处门控）。
//  注意：`checkKernelArgument` 只在捕获路径调用一次，结果存入 `gR1b30ProbeArmed`，输出侧不再判定，
//    从而保证"默认关 ⇒ 连 boot-arg 都不再被查询"。
bool        gR1b30ProbeArmed     = false;
UInt64      gR1b30Base           = 0;      // 捕获时使用的 kext 基址（= gX5000Slide）
UInt64      gR1b30SelfVptr       = 0;      // *(UInt64*)this  —— 纯内存读
UInt64      gR1b30SelfVptrZvm    = 0;      // selfVptr − gX5000Slide
UInt64      gR1b30SlotB20Target  = 0;      // *(this->vptr + 0xb20)
UInt64      gR1b30SlotB20Zvm     = 0;      // slotB20Target − gX5000Slide
UInt64      gR1b30SlotB28Target  = 0;      // *(this->vptr + 0xb28)
UInt64      gR1b30SlotB28Zvm     = 0;
UInt64      gR1b30SlotB30Target  = 0;      // *(this->vptr + 0xb30)  ← 失败槽
UInt64      gR1b30SlotB30Zvm     = 0;
UInt64      gR1b30Vt1A68         = 0;      // *(this+0x1a68) —— 0xb20 返回对象的 vptr
UInt64      gR1b30Vt1A68Zvm      = 0;
UInt64      gR1b30Vt1A40         = 0;      // *(this+0x1a40) —— 0xb28 返回对象的 vptr
UInt64      gR1b30Vt1A40Zvm      = 0;
UInt64      gR1b30F1A38          = 0;      // this+0x1a38（本机为 0）
// provider 属性三要素（存在性 / 类型 / 值三元组）：0 = 不存在（getProperty 返回空），
//   存在时 `*Declared` = 1（即 `declared`），`*Kind` 见下方属性探针块：1 = OSBoolean，2 = OSNumber，
//   3 = OSData，4 = OSString，0 = 其它 OSMetaClass 类型（存在但不可自证为数值）。
UInt64      gR1b30Prov = 0;
UInt64      gR1b30McDeclared = 0, gR1b30McKind = 0, gR1b30McValue = 0;   // "IOMatchCategory"
UInt64      gR1b30AuxDeclared = 0, gR1b30AuxKind = 0, gR1b30AuxValue = 0; // "AAPL,aux-power-connected"
UInt64      gR1b30AuxOk  = 0;      // 属性探针块是否**整体**成功执行（provider 为内核指针且非空）

// ─── R1'-Cwi 探针（`-NRedR1CwiProbe`，默认关）─────────────────────────────────
//  目的（第 79 轮判读修订后）：把 `configureDevice` 失败出口 `0x346e` 的**下游**再切一刀。
//  修订（本报告 §11 独立复现）：槽 `vptr+0xb30` 的**运行时目标归零 VM = 0x61be**，
//  该函数（`createStatisticsManager` 族工厂）在 `0x61e2` 走 `call *0x118` 后**不写** `this+0x1a38`；
//  真正写 `this+0x1a38` 的是槽 `vptr+0xb40`（归零 VM **0x625c** = `createHWInterface`）。
//  ① `0x6280 call *0xb40(this)`（子类覆写的 `newHWInterface()`）
//  ② `0x6286 this+0x1a38 = 结果`
//  ③ `0x62d1 call *0x118(结果->vptr)` ⇒ 通过则 `0x62db orb $0x1,0x1e89(this)` 并 `jmp 0x6301`
//  ④ 不通过则 `0x62f0 call *0x28`（release）后 `0x62f6` 显式清 `this+0x1a38`
//  真机 `this+0x1a38 == 0` ⇒ 失败在 (A) 分配返回 0（`0x6290 je 0x6301`）或 (B) `0x62d1` 返回假。
//  **本探针只加一个 8 字节读**：`this+0x1e89`。
//  依据【客观观测】：全 `__text` 内 `0x1e89(` 只出现 **1 次**，正是 `0x62db` 的 `orb $0x1`；
//  且它在函数内是 `0x62d1` 检查通过后的**唯一成功印记**（`0x62e4` 起的失败路径不写它）。
//  ⇒ `bit0 == 1` ⇒ 走通了 (B) 的"通过"分支；`bit0 == 0` ⇒ 停在 (A) 或 (B) 的失败分支。
//
//  纪律（手册 §9.1 允许形态）：纯内存字段读——不读任何 GPU/SMN 寄存器、**不调用任何 Apple 方法**
//    （含不 `call *槽`）、不写任何内存；默认关时零副作用。
//  ⚠️ 只读 `this` 自身的 `+0x1e89` 与已有的 `+0x1a38` **值本身**；
//    **绝不**解引用 `this+0x1a38` 所指对象（`f1a38 == 0` 时那是 NULL ⇒ 会解引用空指针）。
bool        gR1cwiProbeArmed   = false;
UInt64      gR1cwiBase         = 0;   // 捕获时的 kext 基址（= gX5000Slide）
UInt64      gR1cwiSelf         = 0;   // 最后一次 configureDevice 的 this
UInt64      gR1cwiF1E89        = 0;   // this+0x1E89（8 字节整值；bit0 = createHWInterface 成功印记）
UInt64      gR1cwiF1E89Lo      = 0;   // 同上，低 8 位（局部标量，便于机械判读 bit0）
UInt64      gR1cwiF1A38        = 0;   // this+0x1A38（既有读数的复核；只读值本身）
UInt64      gR1cwiCalls        = 0;   // 本探针观测到的调用次数
UInt64      gR1cwiF1E89ZeroMask = 0;  // 累积：曾出现 bit0 == 0 则置 1（跨次调用，便于判稳定）

// ─── R1'-EngTbl 探针（`-NRedEngTblProbe`，默认关）──────────────────────────────
//  目的：把一条**离线推理**变成**真机实测**——`AMDHardware` 的**引擎槽表**（`+0x3b8` 起、
//    按 `engineType` 索引的 8 字节指针数组）在本机是否**全为空**。
//  依据（离线逐指令核实，见报告 §13）：
//    · `AMDHardware::getHWChannel(eAMD_HW_ENGINE_TYPE, eAMD_HW_RING_TYPE)`（归零 VM `0x747f4`）：
//        `747fa: mov 0x3b8(%rdi,%rax,8),%rdi`   ← **引擎槽表 = this+0x3b8 + engineType*8**
//        `74802: test %rdi,%rdi`
//        `74805: je 0x74816`                    ← 槽为 0 ⇒
//        `74816: xor %eax,%eax; ret`            ← **返回 0**
//      ⇒【客观观测】**"返回 0" ⟺ "该 engineType 的槽为 NULL"**，与任务描述一致。
//    · **可达性（本探针的关键收获）**：`createAccelChannels`（归零 VM `0x1e8c`）在
//        `1eeb: mov 0x1a38(%rax),%rdi`（rax = accelerator 的 this）
//        `1ef8: call *0x328(%rax)`            ← 在 `this+0x1a38` 上调 `vtable[0x328]`
//      而 `vtable[0x328]` 正是 `AMDHardware::getHWChannel`（`__ZTV26AMDRadeonX5000_AMDHardware`
//      = kc `0x4d480e0`，槽 `+0x328` 目标 = kc `0x4bab7f4` = 归零 VM `0x747f4`；**子类
//      `AMDGFX9Hardware` 的同一槽也指向它**）
//      ⇒ **`configureDevice` 的 `this+0x1a38` 就是 `AMDHardware`（或其子类）实例**
//      ⇒ **无需调用任何方法、无需找单例**：直接从我们已有的 `this` 读 `+0x1a38`，即得 `AMDHardware*`。
//  ⇒ 读法（全程只读内存）：`this+0x1a38` → 该对象的 `+0x3b8 + i*8`（i = 0..N-1）。
//  纪律：纯内存字段读——不读寄存器、**不调用任何 Apple 方法**（含不 `call *槽`）、不写内存；
//    每个解引用前做内核地址范围校验；默认关时零副作用。
bool        gEngTblArmed   = false;
UInt64      gEngTblBase    = 0;    // 捕获时的 kext 基址（= gX5000Slide）
UInt64      gEngTblSelf    = 0;    // configureDevice 的 this
UInt64      gEngTblHw      = 0;    // this+0x1a38（= AMDHardware*；0 表示该对象不存在）
UInt64      gEngTblHwVptr  = 0;    // *(AMDHardware*) 首字段 = vptr（纯读，供离线定名）
UInt64      gEngTblHwZvm   = 0;    // 上者 − gX5000Slide（归零 VM；与 _ZTV 对照）
UInt64      gEngTblCalls   = 0;    // 本探针观测到的调用次数
// 前 N 个引擎槽（N = 16，**不扫描整表**）。全 0 ⇒ "引擎从未建立" 被实测证实。
static constexpr UInt32 kEngTblSlots = 16;
UInt64      gEngTblSlot[kEngTblSlots] = {0};
UInt64      gEngTblNonNull = 0;    // 前 N 槽中非 0 的个数（0 ⇒ 全空）

// ─── R1'-P3P14 只读印记探针（`-NRedP3P14Mark`，默认关）────────────────────────────
//  目的（规格书 = `docs/子任务/乙线R1-只读印记P3P14设计.md`，S1）：
//    为 D-1（受控写寄存器）建立**写入前基线**与**写入后复读**两个读数点，
//    使"写入是否真的改变了 P3/P14 的状态"可归因（RM §4.7 判别性纪律）。
//  两个印记（判定对象与判据逐字见设计稿 §2/§3）：
//    · **P3** = `AMDHardware::init` 的 `0x72ded cmp $0xffff,%ax`：
//      `provider->extendedConfigRead16(0x02)`（PCI 配置空间 Device ID）读回 `0xffff` ⇒ 失败。
//      本印记读**同一个 API、同一个 offset**，另加 `configRead16(0x00)`（Vendor ID）作**阳性对照**
//      （理由：单点判据无法排除"整条读通道都返回 0xffff"的退化情形 —— 设计稿 §2.3）。
//    · **P14** = `AMDHardware` 自身的 `+0x30D`（P3 通过位：`0x72df7 movb $0x1` / `0x732c9 movb $0x0`）
//      ＋ `AMDHWRegisters` 对象的 `+0x44`（P7 成功位：`0x4b8c4b8 movb $0x1,0x44(%r14)`，
//      且 `0x4b8c400 movb $0x0,0x44(%r14)` 在入口清零 ⇒ **0 是明确初值**）。
//      `AMDHWRegisters*` 的取值路径：`AMDHardware+0x370`（`0x72ee3 mov %rax,0x370(%r13)`，
//      紧跟 `call *0x5e8` = `allocateAMDHWRegisters`）—— 本设计稿新解出，见设计稿 §3.2。
//
//  读数点（两点，验收 ④）：
//    · **基线** = 本函数（`wrapConfigureDevice`）的捕获块：取 `this+0x1a38`（= `AMDHardware*`，
//      第 84 轮 `-NRedEngTblProbe` 已证）读 P14 两字段；P3 另经 `NRed::singleton().getIGPU()`
//      读配置空间（设备级，与该 this 无关）⇒ **早于 P3/P6/P7/P14 的全部写入**。
//    · **复读** = `X6000FB.cpp` 的 `wrapPpHelperPowerUp`（全部写入已完成）⇒ 缓存进 `sP3P14Re*`，
//      由 `wrapHandleCriticalError` 的 panic 文本带出（PP 时刻写文件全灭，第 87/88 轮实证）。
//
//  纪律（手册 §9.1 允许形态，逐条）：
//    · **零写入**：不写内存/寄存器/配置空间；唯一调用是 `IOPCIDevice::extendedConfigRead16`
//      （IOKit 公开**读取**接口，非 Apple 驱动虚方法）；**不 `call *槽`**；
//    · 每个解引用前做 `>= 0xffffff7f80000000` 校验（手册 §1.3 铁律 6）；校验失败 ⇒ 字段置
//      **`0xFF` 哨兵**（而非 0），使"真的是 0"与"没读到"在判读时可机械区分（设计稿 §3.3）；
//    · 默认关 ⇒ 零副作用（唯一入口是 `checkKernelArgument` 一处门控）。
UInt64      gP3P14Armed      = 0;      // 门控命中且已捕获（1 = 本探针启用）
UInt64      gP3P14Base       = 0;      // 捕获时的 kext 基址（= gX5000Slide）
UInt64      gP3P14Calls      = 0;      // 捕获次数
UInt64      gP3P14Self       = 0;      // configureDevice 的 this
UInt64      gP3P14Hw         = 0;      // this+0x1a38（= AMDHardware*；0 表示对象不存在）
UInt64      gP3P14F30D       = 0;      // AMDHardware+0x30D 低字节（0xFF = 未读到）
UInt64      gP3P14HwReg      = 0;      // AMDHardware+0x370（= AMDHWRegisters*；0 = 未建立）
UInt64      gP3P14F44        = 0;      // AMDHWRegisters+0x44 低字节（0xFF = 未读到）
UInt64      gP3P14Prov       = 0;      // NRed::singleton().getIGPU()（IOPCIDevice*）
UInt64      gP3P14DevId      = 0;      // extendedConfigRead16(0x02)（Device ID）
UInt64      gP3P14VendId     = 0;      // extendedConfigRead16(0x00)（Vendor ID；阳性对照）
UInt64      gP3P14Called     = 0;      // 1 = 配置空间读取确实发起；0 = 未发起（读数无效）

// ─── G3 探针（`-NRedPluginNode`，默认关）：HWServices 插件节点就绪度 ────────────────
//  规格 = `tmp/R91规格-G3探针.md`。捕获点在 `wrapConfigureDevice`（本文件），
//  panic 输出点在 `X6000FB.cpp` 的 `wrapHandleCriticalError`（**排在三块 panic 之后**）。
//  全部为纯内存标量（+ 一次 IOKit 只读匹配）：不读 GPU/SMN 寄存器、不调 Apple 虚方法、不写内存。
//  判据与偏移来源见 `X6000FB.cpp` 侧同名静态量处的逐条说明。
UInt64      gPluginNodeSelf    = 0;      // 捕获时用的 X5000 侧 this（仅自证）
UInt64      gPluginNodeHwsvc   = 0;      // G3-a 输入：HWServices 实例
UInt64      gPluginNodePlugin  = 0;      // G3-a：*(hwsvc + 0xA8)
UInt64      gPluginNodeTtlFld  = 0xFFULL;   // G3-b：*(plugin + 0xB8)（0xFF = 未读到）
UInt64      gPluginNodeCailFld = 0xFFULL;   // G3-b：*(plugin + 0xC0)（0xFF = 未读到）
UInt64      gPluginNodeHwsvcVt = 0;      // G3-c：*(hwsvc)
UInt64      gPluginNodeTtlVt   = 0xFFULL;   // R92：*(ttlFld)（0xFF = 非法指针未解引用）
UInt64      gPluginNodeCailVt  = 0xFFULL;   // R92：*(cailFld)（0xFF = 非法指针未解引用）
int         gPluginNodeValid   = 0;      // 1 = 捕获侧已装填（且门控命中）
UInt64      gEngTblReadFail = 0;   // 1 = 因指针不合法而**拒绝**读表（如实记录）
UInt64 gX5000Slide = 0;

// 第八步实验用：记录 `configureDevice` 成功查到的 framebuffer 服务（`this+0x1f40`），
//  供 `start` 入口恢复（该值随后会被基类 `IOGraphicsAccelerator2::start` 的失败清理清空）。
static UInt64 gLastF140 = 0;

void X5000::processKext(KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide, const size_t size)
{
    if (kextRadeonX5000.loadIndex != id) { return; }

    gX5000Slide = slide;

    DBGLOG("X5000", "processKext: X5000 matched, begin (id=%zu slide=0x%llX size=0x%zX)", id, slide, size);

    NRed::singleton().hwLateInit();

    UInt32*           orgChannelTypes;
    mach_vm_address_t orgStartHWEngines;
    void*             pm4ComputeChannelVT;

    PenguinWizardry::PatternSolveRequest solveRequests[] = {
        {currentKernelVersion() <= MACOS_10_15_X ?
             "__ZZN37AMDRadeonX5000_AMDGraphicsAccelerator22getAdditionalQueueListEPPK18_"
             "AMDQueueSpecifierE27additionalQueueList_Default" :
             "__ZZN37AMDRadeonX5000_AMDGraphicsAccelerator19createAccelChannelsEbE12channelTypes",
         orgChannelTypes, kChannelTypesPattern},
        {"__ZN31AMDRadeonX5000_AMDGFX9PM4Engine10gMetaClassE", this->pm4EngineMC},
        {"__ZN32AMDRadeonX5000_AMDGFX9SDMAEngine10gMetaClassE", this->sdmaEngineMC},
        {"__ZN26AMDRadeonX5000_AMDHardware14startHWEnginesEv", orgStartHWEngines},
        {"__ZN30AMDRadeonX5000_AMDGFX9Hardware32setupAndInitializeHWCapabilitiesEv",
         this->orgGFX9SetupAndInitializeHWCapabilities},
        {"__ZTV39AMDRadeonX5000_AMDGFX9PM4ComputeChannel", pm4ComputeChannelVT},
        {"__ZN30AMDRadeonX5000_AMDPM4HWChannel19submitCommandBufferEP30AMD_SUBMIT_COMMAND_BUFFER_INFO",
         this->orgPM4SubmitCommandBuffer},
    };
    PANIC_COND(!PenguinWizardry::PatternSolveRequest::solveAll(patcher, id, solveRequests, slide, size), "X5000",
               "Failed to resolve symbols");

    AMDRadeonX5000_AMDHWAlignManager::resolve(patcher, id, slide, size);
    AMDRadeonX5000_AMDHWDisplay::resolve(patcher, id, slide, size);
    AMDRadeonX5000_AMDGFX9DCNDisplay::registerMC(kextRadeonX5000.id, patcher, id, slide, size);
    AMDRadeonX5000_AMDGFX9DCN1Display::resolve(kextRadeonX5000.id);
    AMDRadeonX5000_AMDGFX9DCN2Display::resolve(kextRadeonX5000.id);
    AMDRadeonX5000_AMDGFX9DCN314Display::resolve(kextRadeonX5000.id);

    PenguinWizardry::PatternRouteRequest requests[] = {
        {"__ZN32AMDRadeonX5000_AMDVega10Hardware17allocateHWEnginesEv", allocateHWEngines},
        {"__ZN32AMDRadeonX5000_AMDVega10Hardware32setupAndInitializeHWCapabilitiesEv",
         wrapSetupAndInitializeHWCapabilities},
        {"__ZN26AMDRadeonX5000_AMDHardware12getHWChannelE20_eAMD_HW_ENGINE_TYPE18_eAMD_HW_RING_TYPE", wrapGetHWChannel,
         this->orgGetHWChannel},
        {"__ZN30AMDRadeonX5000_AMDGFX9Hardware20initializeFamilyTypeEv", initializeFamilyType},
        {"__ZN30AMDRadeonX5000_AMDGFX9Hardware20allocateAMDHWDisplayEv", allocateAMDHWDisplay},
        {"__ZN26AMDRadeonX5000_AMDHWMemory17adjustVRAMAddressEy", wrapAdjustVRAMAddress, this->orgAdjustVRAMAddress},
        {"__ZN30AMDRadeonX5000_AMDGFX9Hardware20writeASICHangLogInfoEPPv", returnZero},
        {"__ZN4Addr2V27Gfx9Lib20HwlConvertChipFamilyEjj", wrapHwlConvertChipFamily, this->orgHwlConvertChipFamily,
         kHwlConvertChipFamilyPattern},
        {"__ZN27AMDRadeonX5000_AMDHWDisplay14getDisplayInfoEjbbPvP17_FRAMEBUFFER_INFO", fixedGetDisplayInfo},
        {"__ZN33AMDRadeonX5000_AMDHWAlignManager214getSurfaceInfoEP24_AMD_SURFACE_INFO_STRUCT", fixedGetSurfaceInfo},
    };
    PANIC_COND(!PenguinWizardry::PatternRouteRequest::routeAll(patcher, id, requests, slide, size), "X5000",
               "Failed to route symbols");

    // 第八步观测（第 7 批次）：hook 加速器的 `start`（**出口**），读注册链的前置字段。
    //  依据（2026-09-26 离线取证）：`AMDGraphicsAccelerator::start`（VM 0x1290）中，只有
    //  `this+0x1f40`（= framebuffer 服务，由 `configureDevice` 按 "ATIFramebuffer" /
    //  "IOFramebuffer" 查得）非空时，才会用 `OSSymbol("SpecialAMDKey")`（`this+0x1f48`）向
    //  controller 发 selector = 0 的注册调用（`call *(controller)->vtable[0x6b8]`）；
    //  为 0 则整段跳过 ⇒ `controller+0x7960` 永远为空。
    //  仅 `-NRedAccelProbe`（panic 通道）或 `-NRedAccelLog`（日志通道，零挂死风险）时安装；
    //  默认零影响。
    if (checkKernelArgument("-NRedAccelProbe") || checkKernelArgument("-NRedAccelLog")) {
        PenguinWizardry::PatternRouteRequest accelStartReq{
            "__ZN37AMDRadeonX5000_AMDGraphicsAccelerator5startEP9IOService", wrapAccelStart, this->orgAccelStart};
        if (!accelStartReq.route(patcher, id, slide, size)) {
            SYSLOG("X5000", "accel-probe: failed to route AMDGraphicsAccelerator::start");
        } else {
            DBGLOG("X5000", "accel-probe: routed AMDGraphicsAccelerator::start");
        }
    }

    // 第八步观测（2026-09-28）：hook `AMDRTHardware::initializeTtl`（**无条件安装**；内部按
    //  `-NRedTtlLog` 决定是否落盘输出）。只读内存字段、零行为改变 ⇒ 默认轮次也安全。
    {
        PenguinWizardry::PatternRouteRequest ttlIfaceReq{
            "__ZN28AMDRadeonX5000_AMDRTHardware13initializeTtlEP16_GART_PARAMETERS", wrapInitializeTtl,
            this->orgInitializeTtl};
        if (!ttlIfaceReq.route(patcher, id, slide, size)) {
            SYSLOG("X5000", "ttl-iface: failed to route AMDRTHardware::initializeTtl");
        } else {
            DBGLOG("X5000", "ttl-iface: routed AMDRTHardware::initializeTtl");
        }
    }

    // 第八步观测（2026-09-28）：hook `configureDevice` 与 `initLinkToPeer`（**无条件安装**，
    //  内部按 `-NRedAccelLog` 输出）。前者是 `this+0x1f40`（framebuffer 服务）的唯一设置者；
    //  后者按名查 `"ATIFramebuffer"`/`"IOFramebuffer"`。
    {
        PenguinWizardry::PatternRouteRequest l2pReq{
            "__ZN37AMDRadeonX5000_AMDGraphicsAccelerator14initLinkToPeerEPKc", wrapInitLinkToPeer,
            this->orgInitLinkToPeer};
        if (!l2pReq.route(patcher, id, slide, size)) {
            SYSLOG("X5000", "cfgdev: failed to route initLinkToPeer");
        }
    }

    // A-25：first-false 窗口法 · 最小只读探针——hook `AMDHardware::init`（kc `0x4ba9cea`）。
    //  ✅ 红线②裁定（技术组长）：**"新增只读入口 hook"属合法观测，不属"注入 hook"**——
    //     依据 A-19「调用 Apple 方法 ≠ 改写 Apple 状态」；本探针只在 `AMDHardware::init`
    //     **返回后读内存**，不改 Apple 控制流/数据、不写 MMIO、不新增发送，与既有
    //     `-NRed*Readout` 同类且已被接受。条件：自证"门控关 ⇒ 零读/零行为差异；门控开 ⇒ 仅内存读"。
    //  ⇒ 本 hook **条件路由**（门控 `-NRedWindowProbe`，默认关）⇒ 门控假时**连 hook 都不安装**
    //     （零读、零行为差异的机械自证：不 route ⇒ 不劫持 ⇒ 零影响）。
    if (checkKernelArgument("-NRedWindowProbe")) {
        PenguinWizardry::PatternRouteRequest hwInitReq{
            "__ZN26AMDRadeonX5000_AMDHardware4initEP11IOPCIDeviceP28AMDRadeonX5000_IAMDHWHandlerRj"
            "P16_GART_PARAMETERSP14_FB_PARAMETERS",
            wrapAmdHwInit, this->orgAmdHwInit};
        if (!hwInitReq.route(patcher, id, slide, size)) {
            SYSLOG("X5000", "win-probe: failed to route AMDHardware::init");
        } else {
            DBGLOG("X5000", "win-probe: routed AMDHardware::init");
        }

        // A-47：P5 判据函数 `AMDHardware::initializeExternalInterfaces`（kc `0x4baa9ba`）入口只读采样
        //  （同门控：门控关 ⇒ **连本 hook 也不安装**）。目的＝把 P5 失败的 C2（实例不是该 provider
        //  的 client）／C3（client 上缺 `IOMatchCategory`）／C4（时序）三者三选一（依据 A-46）。
        //  **不干预 IOKit attach／父子关系、不写任何字段**（红线）。
        PenguinWizardry::PatternRouteRequest p5Req{
            "__ZN26AMDRadeonX5000_AMDHardware28initializeExternalInterfacesEv",
            wrapInitExtIfaces, this->orgInitExtIfaces};
        if (!p5Req.route(patcher, id, slide, size)) {
            SYSLOG("X5000", "win-probe-P5: failed to route initializeExternalInterfaces");
        } else {
            DBGLOG("X5000", "win-probe-P5: routed initializeExternalInterfaces");
        }
    }

    // 第八步观测（第 15 轮）：**已移除** `probe` 的 hook —— 第 14 轮实测它**有副作用**
    //  （hook 后系统未再走到 PP 上电、直接跑到 userspace watchdog ⇒ 匹配阶段行为被改变）。
    //  ⇒ 改为纯被动取证：`DriverInjector::wrapAddDrivers` 只记录"注入的 personality 是否在数组里"，
    //     由 `AmdRadeonController::powerUp` 的 `-NRedAccelExist2` 探针统一输出。

    if (currentKernelVersion() >= MACOS_11) {
        PenguinWizardry::PatternSolveRequest solveRequest{"__ZN30AMDRadeonX5000_AMDGFX9Hardware15notifyGfxAccessEv",
                                                          this->notifyGfxAccess};
        PANIC_COND(!solveRequest.solve(patcher, id, slide, size), "X5000", "Failed to resolve notifyGfxAccess");
        PANIC_COND(MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS, "X5000",
                   "Failed to enable kernel writing");
        this->orgPM4SubmitCommandBuffer = this->hwChannelSubmitCommandBuffer(pm4ComputeChannelVT);
        this->hwChannelSubmitCommandBuffer(pm4ComputeChannelVT) =
            reinterpret_cast<mach_vm_address_t>(computeSubmitCommandBuffer);
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
    }

    if (currentKernelVersion() >= MACOS_13_4) {
        PenguinWizardry::PatternRouteRequest request{
            "__ZN37AMDRadeonX5000_AMDGraphicsAccelerator23obtainAccelChannelGroupE11SS_"
            "PRIORITYP27AMDRadeonX5000_AMDAccelTask",
            wrapObtainAccelChannelGroup1304, this->orgObtainAccelChannelGroup};
        PANIC_COND(!request.route(patcher, id, slide, size), "X5000", "Failed to route obtainAccelChannelGroup");
    }
    else if (currentKernelVersion() >= MACOS_11) {
        PenguinWizardry::PatternRouteRequest request{
            "__ZN37AMDRadeonX5000_AMDGraphicsAccelerator23obtainAccelChannelGroupE11SS_PRIORITY",
            wrapObtainAccelChannelGroup, this->orgObtainAccelChannelGroup};
        PANIC_COND(!request.route(patcher, id, slide, size), "X5000", "Failed to route obtainAccelChannelGroup");
    }

    if (currentKernelVersion() >= MACOS_14_4) {
        const PenguinWizardry::MaskedLookupPatch patch{
            &kextRadeonX5000,          kAddrLibCreateOriginal1404,    kAddrLibCreateOriginalMask1404,
            kAddrLibCreatePatched1404, kAddrLibCreatePatchedMask1404, 1};
        PANIC_COND(!patch.apply(patcher, slide, size), "X5000", "Failed to apply 14.4+ Addr::Lib::Create patch");
    }
    else if (currentKernelVersion() <= MACOS_10_15_X || currentKernelVersion() >= MACOS_13_4) {
        const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX5000, kAddrLibCreateOriginal, kAddrLibCreatePatched,
                                                       1};
        PANIC_COND(!patch.apply(patcher, slide, size), "X5000", "Failed to apply Addr::Lib::Create patch");
    }

    if (currentKernelVersion() <= MACOS_10_15_X) {
        if (currentKernelVersion().majorMatches(MACOS_10_15)) {
            const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX5000, kCreateAccelChannelsOriginal,
                                                           kCreateAccelChannelsPatched, 2};
            PANIC_COND(!patch.apply(patcher, slide, size), "X5000", "Failed to patch createAccelChannels");
        }
        else {
            const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX5000, kCreateAccelChannelsOriginal10_14,
                                                           kCreateAccelChannelsPatched10_14, 1};
            PANIC_COND(!patch.apply(patcher, slide, size), "X5000", "Failed to patch createAccelChannels");
        }

        // TODO: wait, what is this doing again?
        // 豁免 Phoenix 守卫：本块仅在 macOS 10.15 及以下才执行，目标系统 13.6 不执行，
        // 故 isRenoir() 分支无需 isPhoenix() 排除（路线图附录 A(8) 筛查豁免项）。
        if (NRed::singleton().getAttributes().isRenoir()) {
            UInt32                                   findNonBpp64 = Dcn1NonBpp64SwModeMask1015;
            UInt32                                   replNonBpp64 = Dcn2NonBpp64SwModeMask1015;
            UInt32                                   findBpp64 = Dcn1NonBpp64SwModeMask1015 ^ Dcn1Bpp64SwModeMask1015;
            UInt32                                   replBpp64 = Dcn2NonBpp64SwModeMask1015 ^ Dcn2Bpp64SwModeMask1015;
            UInt32                                   findBpp64Pt2 = Dcn1Bpp64SwModeMask1015;
            UInt32                                   replBpp64Pt2 = Dcn2Bpp64SwModeMask1015;
            const PenguinWizardry::MaskedLookupPatch patches[]    = {
                {&kextRadeonX5000, reinterpret_cast<const UInt8*>(&findNonBpp64),
                 reinterpret_cast<const UInt8*>(&replNonBpp64), sizeof(UInt32), 2},
                {&kextRadeonX5000, reinterpret_cast<const UInt8*>(&findBpp64),
                 reinterpret_cast<const UInt8*>(&replBpp64), sizeof(UInt32), 1},
                {&kextRadeonX5000, reinterpret_cast<const UInt8*>(&findBpp64Pt2),
                 reinterpret_cast<const UInt8*>(&replBpp64Pt2), sizeof(UInt32), 1},
            };
            PANIC_COND(!PenguinWizardry::MaskedLookupPatch::applyAll(patcher, patches, slide, size), "X5000",
                       "Failed to patch swizzle mode");
        }

        PANIC_COND(MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS, "X5000",
                   "Failed to enable kernel writing");
        *orgChannelTypes = 1;    // Make VMPT use SDMA0 instead of SDMA1
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        DBGLOG("X5000", "Applied SDMA1 patches");
    }
    else {
        const PenguinWizardry::MaskedLookupPatch patch{
            &kextRadeonX5000,       kStartHWEnginesOriginal, kStartHWEnginesMask,
            kStartHWEnginesPatched, kStartHWEnginesMask,     currentKernelVersion() >= MACOS_13 ? 2U : 1};
        PANIC_COND(!patch.apply(patcher, orgStartHWEngines, PAGE_SIZE), "X5000", "Failed to patch startHWEngines");

        if (NRed::singleton().getAttributes().isRenoir() && !NRed::singleton().getAttributes().isPhoenix()) {
            UInt32 findBpp64 = Dcn1Bpp64SwModeMask, replBpp64 = Dcn2Bpp64SwModeMask;
            UInt32 findNonBpp64 = Dcn1NonBpp64SwModeMask, replNonBpp64 = Dcn2NonBpp64SwModeMask;
            const PenguinWizardry::MaskedLookupPatch patches[] = {
                {&kextRadeonX5000, reinterpret_cast<const UInt8*>(&findBpp64),
                 reinterpret_cast<const UInt8*>(&replBpp64), sizeof(UInt32),
                 currentKernelVersion() >= MACOS_13_4 ? 2U : 4},
                {&kextRadeonX5000, reinterpret_cast<const UInt8*>(&findNonBpp64),
                 reinterpret_cast<const UInt8*>(&replNonBpp64), sizeof(UInt32),
                 currentKernelVersion() >= MACOS_13_4 ? 2U : 4},
            };
            PANIC_COND(!PenguinWizardry::MaskedLookupPatch::applyAll(patcher, patches, slide, size), "X5000",
                       "Failed to patch swizzle mode");
        }

        PANIC_COND(MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS, "X5000",
                   "Failed to enable kernel writing");
        // createAccelChannels: stop at SDMA0
        orgChannelTypes[5] = 1;
        // getPagingChannel: get only SDMA0
        orgChannelTypes[currentKernelVersion() >= MACOS_12 ? 12 : 11] = 0;
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        DBGLOG("X5000", "Applied SDMA1 patches");
    }
}

bool X5000::allocateHWEngines(void* const self)
{
    [[clang::suppress]] singleton().pm4EngineField(self)   = singleton().pm4EngineMC->alloc();
    [[clang::suppress]] singleton().sdma0EngineField(self) = singleton().sdmaEngineMC->alloc();

    // No VCN? :-(

    return true;
}

// TODO: Replace with IP Discovery?
void X5000::wrapSetupAndInitializeHWCapabilities(void* const self)
{
    auto& seCount  = singleton().seCountField(self);
    auto& shCount  = singleton().shCountField(self);
    auto& hwMaxCUs = singleton().hwMaxCUsField(self);

    seCount = 1;
    shCount = 1;
    if (NRed::singleton().getAttributes().isPhoenix()) { hwMaxCUs = 12; }
    else if (NRed::singleton().getAttributes().isRenoir()) { hwMaxCUs = 8; }
    else if (NRed::singleton().getAttributes().isRaven2()) {
        hwMaxCUs = 3;
    }
    else {
        hwMaxCUs = 11;
    }

    FunctionCast(wrapSetupAndInitializeHWCapabilities, singleton().orgGFX9SetupAndInitializeHWCapabilities)(self);

    singleton().supportedDisplayCountField(self) = 4;
    singleton().hasUVD0Field(self)               = false;
    singleton().hasVCEField(self)                = false;
    singleton().hasVCN0Field(self)               = false;    // TODO
    singleton().hasSDMAPagingQueueField(self)    = false;
    singleton().hasGetAllClockLimitsField(self)  = false;
    if (currentKernelVersion() >= MACOS_10_15) { singleton().dccDisplayableSupportField(self) = true; }
}

// TODO: Investigate why this is needed.
void* X5000::wrapGetHWChannel(void* const self, AMDHWEngineType engineType, const UInt32 ringId)
{
    if (engineType == kAMDHWEngineTypeSDMA1) { engineType = kAMDHWEngineTypeSDMA0; }
    return FunctionCast(wrapGetHWChannel, singleton().orgGetHWChannel)(self, engineType, ringId);
}

void X5000::initializeFamilyType(void* const self) { singleton().familyTypeField(self) = AMD_FAMILY_RAVEN; }

void* X5000::allocateAMDHWDisplay(void* const)
{
    const auto& attrs = NRed::singleton().getAttributes();
    DBGLOG("X5000", "allocateAMDHWDisplay: isPhoenix=%s isRenoir=%s",
           attrs.isPhoenix() ? "true" : "false", attrs.isRenoir() ? "true" : "false");
    if (attrs.isPhoenix()) {
        DBGLOG("X5000", "allocateAMDHWDisplay: returning DCN314Display");
        return AMDRadeonX5000_AMDGFX9DCN314Display::gRTMetaClass.alloc();
    }
    if (attrs.isRenoir()) {
        return AMDRadeonX5000_AMDGFX9DCN2Display::gRTMetaClass.alloc();
    }
    return AMDRadeonX5000_AMDGFX9DCN1Display::gRTMetaClass.alloc();
}

UInt64 X5000::wrapAdjustVRAMAddress(void* const self, const UInt64 addr)
{
    auto ret = FunctionCast(wrapAdjustVRAMAddress, singleton().orgAdjustVRAMAddress)(self, addr);
    if (ret != addr) { return ret + NRed::singleton().getFbOffset(); }
    return ret;
}

UInt32 X5000::returnZero() { return 0; }

// Replaces SDMA1 field with SDMA0 because we don't have SDMA1
// TODO: Investigate why this is needed.
static void* fixAccelGroup(void* const group)
{
    if (group != nullptr) { getMember<void*>(group, 0x18) = getMember<void*>(group, 0x10); }
    return group;
}

void* X5000::wrapObtainAccelChannelGroup(void* const self, const UInt32 priority)
{
    return fixAccelGroup(
        FunctionCast(wrapObtainAccelChannelGroup, singleton().orgObtainAccelChannelGroup)(self, priority));
}

void* X5000::wrapObtainAccelChannelGroup1304(void* const self, const UInt32 priority, void* const task)
{
    return fixAccelGroup(
        FunctionCast(wrapObtainAccelChannelGroup1304, singleton().orgObtainAccelChannelGroup)(self, priority, task));
}

UInt32 X5000::wrapHwlConvertChipFamily(void* const self, const UInt32 family, const UInt32 revision)
{
    DBGLOG("X5000", "HwlConvertChipFamily >> (self: %p family: 0x%X revision: 0x%X)", self, family, revision);
    if (family == AMD_FAMILY_RAVEN) {
        auto& settings          = singleton().chipSettingsField(self);
        settings.isArcticIsland = 1;
        settings.isRaven        = 1;
        if (NRed::singleton().getAttributes().isRenoir() && !NRed::singleton().getAttributes().isPhoenix()) {
            settings.htileAlignFix = 1;
            settings.applyAliasFix = 1;
        }
        else if (!NRed::singleton().getAttributes().isRaven2()) {
            settings.depthPipeXorDisable = 1;
        }
        settings.isDcn1           = 1;    // what to do about this?
        settings.metaBaseAlignFix = 1;
        return ADDR_CHIP_FAMILY_AI;
    }
    return FunctionCast(wrapHwlConvertChipFamily, singleton().orgHwlConvertChipFamily)(self, family, revision);
}

UInt32 X5000::computeSubmitCommandBuffer(void* const self, void* const info)
{
    singleton().notifyGfxAccess(singleton().hwChannelHWInterfaceField(self));
    return FunctionCast(computeSubmitCommandBuffer, singleton().orgPM4SubmitCommandBuffer)(self, info);
}

//  -- VRR was introduced on macOS Big Sur. --

namespace
{

    class Constants
    {
        static void _setVrrTimestampInfoVentura(AMDRadeonX5000_AMDHWDisplay* const self, const UInt64 vTotalMin,
                                                const UInt64 vTotalMax, const UInt64 horizontalLineTime)
        {
            auto& vrrTimestampInfo                      = self->vrrTimestampInfoVentura();
            vrrTimestampInfo.lastTransactionTimestamp   = 0;
            vrrTimestampInfo.currentFrameStartTimestamp = 0;
            vrrTimestampInfo.lastTransactionStartTime   = 0;
            vrrTimestampInfo.currentFrameVTotal         = vTotalMin;
            vrrTimestampInfo.horizontalLineTime         = horizontalLineTime;
            vrrTimestampInfo.currentFrameTime           = 0;
            vrrTimestampInfo.vTotalMin                  = vTotalMin;
            vrrTimestampInfo.vTotalMax                  = vTotalMax;
            vrrTimestampInfo.transactionOnGlassTime     = 0;
        }

        static void _setVrrTimestampInfo(AMDRadeonX5000_AMDHWDisplay* const self, const UInt64 vTotalMin,
                                         const UInt64 vTotalMax, const UInt64 horizontalLineTime)
        {
            auto& vrrTimestampInfo                      = self->vrrTimestampInfo();
            vrrTimestampInfo.field0                     = 0;
            vrrTimestampInfo.lastTransactionTimestamp   = 0;
            vrrTimestampInfo.currentFrameStartTimestamp = 0;
            vrrTimestampInfo.lastTransactionStartTime   = 0;
            vrrTimestampInfo.currentFrameVTotal         = static_cast<UInt32>(vTotalMin);
            vrrTimestampInfo.horizontalLineTime         = static_cast<UInt32>(horizontalLineTime);
            vrrTimestampInfo.currentFrameTime           = 0;
            vrrTimestampInfo.vTotalMin                  = static_cast<UInt32>(vTotalMin);
            vrrTimestampInfo.vTotalMax                  = static_cast<UInt32>(vTotalMax);
            vrrTimestampInfo.transactionOnGlassTime     = 0;
        }

        static void _calcAndSetVrrTimestampInfo(AMDRadeonX5000_AMDHWDisplay* const self,
                                                const FramebufferInfo* const       fbInfo,
                                                const IOTimingInformation&         timingInfo)
        {
            assert(currentKernelVersion() >= MACOS_11);

            if (!fbInfo->isOnline) { return; }

            const auto vTotalMin =
                timingInfo.detailedInfo.v2.verticalBlanking + timingInfo.detailedInfo.v2.verticalActive;
            const auto vTotalMax  = vTotalMin + timingInfo.detailedInfo.v2.verticalBlankingExtension;
            const auto pixelClock = timingInfo.detailedInfo.v2.pixelClock;
            if (pixelClock == 0) { singleton.setVrrTimestampInfo(self, vTotalMin, vTotalMax, 0); }
            else {
                const auto hTotal =
                    timingInfo.detailedInfo.v2.horizontalBlanking + timingInfo.detailedInfo.v2.horizontalActive;
                const auto hLineTimeNs = static_cast<UInt64>(hTotal) * 1000000000ULL / pixelClock;
                UInt64     horizontalLineTime;
                nanoseconds_to_absolutetime(hLineTimeNs, &horizontalLineTime);
                singleton.setVrrTimestampInfo(self, vTotalMin, vTotalMax, horizontalLineTime);
            }
        }

        static void _calcAndSetVrrTimestampInfoDummy(AMDRadeonX5000_AMDHWDisplay*, const FramebufferInfo*,
                                                     const IOTimingInformation&)
        { assert(currentKernelVersion() <= MACOS_10_15_X); }

    public:
        static const Constants singleton;

        void (*setVrrTimestampInfo)(AMDRadeonX5000_AMDHWDisplay* self, UInt64 vTotalMin, UInt64 vTotalMax,
                                    const UInt64 horizontalLineTime){nullptr};
        void (*calcAndSetVrrTimestampInfo)(AMDRadeonX5000_AMDHWDisplay* self, const FramebufferInfo* fbInfo,
                                           const IOTimingInformation& timingInfo){_calcAndSetVrrTimestampInfoDummy};

        explicit Constants()
        {
            if (currentKernelVersion() < MACOS_11) { return; }

            this->calcAndSetVrrTimestampInfo = _calcAndSetVrrTimestampInfo;
            this->setVrrTimestampInfo =
                currentKernelVersion() >= MACOS_13 ? _setVrrTimestampInfoVentura : _setVrrTimestampInfo;
        }
    };

    const Constants Constants::singleton;

    constexpr inline auto& vrrConstants = Constants::singleton;

}    // namespace

bool X5000::fixedGetDisplayInfo(AMDRadeonX5000_AMDHWDisplay* const self, const UInt32 fbIndex, const bool isCRTEnabled,
                                const bool ignoreCRTOffsetCheck, IOFramebuffer* const fb, FramebufferInfo* const fbInfo)
{
    if (fb == nullptr || fbIndex >= self->supportedDisplayCount()) { return false; }

    fbInfo->crtOffset   = 0;
    fbInfo->size        = 0;
    fbInfo->crtSize     = 0;
    fbInfo->pitch       = 0;
    fbInfo->rect.width  = 0;
    fbInfo->rect.height = 0;

    auto& displayState = self->displayStates()[fbIndex];

    displayState.framebuffer = fb;
    displayState.status.setIsEnabled(isCRTEnabled);
    displayState.status.setIsIOFBFlipEnabled(true);
    displayState.status.setIsAccelBacked(false);

    uintptr_t wsaa = -1ULL;
    if (fb->getAttribute(ATTRIBUTE_WSAA, &wsaa) == kIOReturnSuccess) {
        self->wsaaAttributes()[fbIndex] = static_cast<UInt32>(wsaa);
        displayState.status.setIsWSAASupported(true);
    }
    else {
        displayState.status.setIsWSAASupported(false);
    }

    uintptr_t  dpt    = 0;
    const auto dptRet = fb->getAttribute(ATTRIBUTE_DISPLAY_PIPE_TRANSACTION, &dpt);
    displayState.status.setIsDPTSupported(dptRet == kIOReturnSuccess || dptRet == kIOReturnNotReady);

    const auto crtOffset = OSDynamicCast(OSData, fb->getProperty("ATY,fb_offset"));
    if (crtOffset != nullptr) {
        const auto data = crtOffset->getBytesNoCopy();
        if (data != nullptr) { fbInfo->crtOffset = *static_cast<const UInt64*>(data); }
    }

    const auto crtSize = OSDynamicCast(OSData, fb->getProperty("ATY,fb_size"));
    if (crtSize != nullptr) {
        const auto data = crtSize->getBytesNoCopy();
        if (data != nullptr) { fbInfo->crtSize = *static_cast<const UInt32*>(data); }
    }

    auto aperture = fb->getApertureRange(kIOFBSystemAperture);
    auto vram     = fb->getVRAMRange();

    fbInfo->isMapped = aperture != nullptr && vram != nullptr;

    // §简化项 15：捕获 Apple 自己认定的 **VRAM 基址**，供 NRed 计算 fbOffset 用。
    // 为什么在这里：这是 NRed 已有的钩子里最早能拿到 `IOFramebuffer` 的地方，而 `getVRAMRange()`
    //   给出的正是"VRAM 的地址范围"——与消费方（Apple 的 wrapAdjustVRAMAddress 换算）天然同源。
    //   替代了此前两版被证伪的做法（读 BAR0 / 读 MMHUB 寄存器，理由见 NRed.cpp 的 hwLateInit）。
    if (vram != nullptr) {
        const auto vramBase = static_cast<UInt64>(vram->getPhysicalSegment(0, nullptr));
        if (vramBase != 0) { NRed::singleton().setFbLocationBase(vramBase); }
        // A-23：同点捕获窗长（`getVRAMRange()` 对象自带；同一既有 Apple API，非新增寄存器探针）。
        //  ⚠️ 与 fbLocationBase 同为"读 0 陷阱"字段：0 ⇒ 尚未捕获，不得当作窗长为 0。
        const auto vramLen = static_cast<UInt64>(vram->getLength());
        if (vramLen != 0) { NRed::singleton().setFbLocationSize(vramLen); }
    }

    [[clang::suppress]] OSSafeReleaseNULL(aperture);
    [[clang::suppress]] OSSafeReleaseNULL(vram);

    self->getDisplayModeViewportSpecificInfo(fbIndex, &self->viewportStartYs()[fbIndex],
                                             &self->viewportHeights()[fbIndex]);

    uintptr_t isOnline = 0;
    fbInfo->isOnline =
        fb->getAttributeForConnection(static_cast<IOIndex>(fbIndex), kConnectionEnable, &isOnline) == kIOReturnSuccess
        && isOnline != 0;

    self->fedsParamInfo()[fbIndex].crtIndex = 0;
    self->fedsParamInfo()[fbIndex].scaledW  = 0;
    self->fedsParamInfo()[fbIndex].scaledH  = 0;
    self->fedsParamInfo()[fbIndex].srcW     = 0;
    self->fedsParamInfo()[fbIndex].srcH     = 0;

    self->scalerFlags()[fbIndex] = 0;

    IODisplayModeID displayMode = 0;
    IOIndex         depth       = 0;
    if (fb->getCurrentDisplayMode(&displayMode, &depth) == kIOReturnSuccess) {
        if (fb->getPixelInformation(displayMode, depth, kIOFBSystemAperture, &displayState.pixelInfo)
            == kIOReturnSuccess)
        {
            const auto bytesPerPixel = displayState.pixelInfo.bitsPerPixel / 8;
            if (bytesPerPixel != 0) { fbInfo->pitch = displayState.pixelInfo.bytesPerRow / bytesPerPixel; }
            fbInfo->rect.width  = displayState.pixelInfo.activeWidth;
            fbInfo->rect.height = displayState.pixelInfo.activeHeight;
        }

        IODisplayModeInformation modeInfo;
        memset(&modeInfo, 0, sizeof(modeInfo));
        if (fb->getInformationForDisplayMode(displayMode, &modeInfo) == kIOReturnSuccess
            && (modeInfo.flags & kDisplayModeAcceleratorBackedFlag) != 0)
        {
            displayState.status.setIsAccelBacked(true);
            self->fedsParamInfo()[fbIndex].crtIndex = 1;
        }

        IOTimingInformation timingInfo;
        memset(&timingInfo, 0, sizeof(timingInfo));
        timingInfo.flags = kIODetailedTimingValid;
        if (fb->getTimingInfoForDisplayMode(displayMode, &timingInfo) == kIOReturnSuccess
            && (timingInfo.flags & kIODetailedTimingValid) != 0)
        {
            self->scalerFlags()[fbIndex] = timingInfo.detailedInfo.v2.scalerFlags;

            if (self->fedsParamInfo()[fbIndex].crtIndex == 1) {
                self->fedsParamInfo()[fbIndex].scaledW = timingInfo.detailedInfo.v2.horizontalActive;
                self->fedsParamInfo()[fbIndex].scaledH = timingInfo.detailedInfo.v2.verticalActive;
                self->fedsParamInfo()[fbIndex].srcW    = timingInfo.detailedInfo.v2.horizontalScaled;
                self->fedsParamInfo()[fbIndex].srcH    = timingInfo.detailedInfo.v2.verticalScaled;
            }

            vrrConstants.calcAndSetVrrTimestampInfo(self, fbInfo, timingInfo);
        }
    }

    auto& hwSpecificInfo = self->crtHWSpecificInfos()[fbIndex];

    auto ret = true;

    if (!isCRTEnabled
        || (!ignoreCRTOffsetCheck && fbInfo->crtOffset >= self->getHWInterface()->getHWMemory()->getVisibleSize()))
    {
        fbInfo->crtOffset = 0;
        fbInfo->size      = 0;
    }
    else {
        CRTHWDepth hwDepth;
        switch (displayState.pixelInfo.bitsPerPixel) {
            case 8: {
                hwDepth = CRTHWDepth::DEPTH_8;
            } break;
            case 16: {
                hwDepth = CRTHWDepth::DEPTH_16;
            } break;
            case 32: {
                hwDepth = CRTHWDepth::DEPTH_32;
            } break;
            case 64: {
                hwDepth = CRTHWDepth::DEPTH_64;
            } break;
            default: {
                hwDepth = CRTHWDepth::DEPTH_32;
                ret     = false;
            } break;
        }
        DBGLOG("X5000", "%s hwDepth=%s for bpp %d", __func__, stringifyCRTHWDepth(hwDepth),
               displayState.pixelInfo.bitsPerPixel);
        hwSpecificInfo.graphDepth = hwDepth;

        CRTHWFormat hwFormat;    // bug fix - original code only handled depth 64 and format 10
        switch (displayState.pixelInfo.bitsPerComponent) {
            case 8: {
                hwFormat = CRTHWFormat::FORMAT_8;
            } break;
            case 10: {
                hwFormat = CRTHWFormat::FORMAT_10;
            } break;
            case 12: {
                hwFormat = CRTHWFormat::FORMAT_12;
            } break;
            default: {
                hwFormat = CRTHWFormat::FORMAT_8;
                ret      = false;
            } break;
        }
        DBGLOG("X5000", "%s hwFormat=%s for bpc %d", __func__, stringifyCRTHWFormat(hwFormat),
               displayState.pixelInfo.bitsPerComponent);
        hwSpecificInfo.graphFormat = hwFormat;

        switch (hwDepth) {
            case CRTHWDepth::DEPTH_8: {
                hwSpecificInfo.bytesPerPixel = 1;
            } break;
            case CRTHWDepth::DEPTH_16: {
                hwSpecificInfo.bytesPerPixel = 2;
            } break;
            case CRTHWDepth::DEPTH_32: {
                hwSpecificInfo.bytesPerPixel = 4;
            } break;
            case CRTHWDepth::DEPTH_64: {
                hwSpecificInfo.bytesPerPixel = 8;
            } break;
        }
        hwSpecificInfo.pixelMode = self->getPixelMode(hwDepth, hwFormat);
        DBGLOG("X5000", "%s hwSpecificInfo.pixelMode=%s", __func__, stringifyATIPixelMode(hwSpecificInfo.pixelMode));
        hwSpecificInfo.format = self->getPixelFormat(hwSpecificInfo.pixelMode);
        DBGLOG("X5000", "%s hwSpecificInfo.format=%s", __func__, stringifyATIFormat(hwSpecificInfo.format));
        hwSpecificInfo.isInterlaced = self->isDisplayInterlaceEnabled(fbIndex);
        displayState.status.setIsInterlaced(hwSpecificInfo.isInterlaced);

        ADDR2_COMPUTE_SURFACE_INFO_INPUT surfInfoInput;
        surfInfoInput.width        = fbInfo->rect.width;
        surfInfoInput.height       = fbInfo->rect.height;
        surfInfoInput.bpp          = displayState.pixelInfo.bitsPerPixel;
        surfInfoInput.resourceType = ADDR_RSRC_TEX_2D;
        surfInfoInput.format     = self->getHWInterface()->getHWAlignManager()->getAddrFormat(hwSpecificInfo.pixelMode);
        surfInfoInput.numSamples = 1;
        surfInfoInput.numSlices  = 1;
        surfInfoInput.flags.display = true;
        surfInfoInput.swizzleMode =
            self->getHWInterface()->getHWAlignManager()->getPreferredSwizzleMode2(&surfInfoInput);
        self->savedSwizzleModes()[fbIndex]  = surfInfoInput.swizzleMode;
        self->swizzleModes()[fbIndex]       = surfInfoInput.swizzleMode;
        self->savedResourceTypes()[fbIndex] = surfInfoInput.resourceType;
        if (self->getHWInterface()->getHWAlignManager()->getSurfaceInfo2(&surfInfoInput,
                                                                         &self->surfInfoOutputs()[fbIndex])
            == kIOReturnSuccess)
        {
            fbInfo->size = fbInfo->pitch * self->surfInfoOutputs()[fbIndex].height * hwSpecificInfo.bytesPerPixel;
        }
        else {
            fbInfo->size = 0;
        }
        displayState.status.setIsEnabled(true);
    }

    AMDHWDisplayState::Status combinedStatus;
    for (UInt32 i = 0; i < self->supportedDisplayCount(); i += 1) { combinedStatus |= self->displayStates()[i].status; }
    self->combinedStatus() = combinedStatus;

    fbInfo->savedSize = fbInfo->size;

    if (self->fedsParamInfo()[fbIndex].crtIndex != 0) {
        ADDR2_COMPUTE_SURFACE_INFO_INPUT surfInfoInput;
        surfInfoInput.width        = self->fedsParamInfo()[fbIndex].scaledW;
        surfInfoInput.height       = self->fedsParamInfo()[fbIndex].scaledH;
        surfInfoInput.bpp          = displayState.pixelInfo.bitsPerPixel;
        surfInfoInput.swizzleMode  = self->savedSwizzleModes()[fbIndex];
        surfInfoInput.resourceType = self->savedResourceTypes()[fbIndex];
        surfInfoInput.format     = self->getHWInterface()->getHWAlignManager()->getAddrFormat(hwSpecificInfo.pixelMode);
        surfInfoInput.numSamples = 1;
        surfInfoInput.numSlices  = 1;
        surfInfoInput.flags.display = true;
        ADDR2_COMPUTE_SURFACE_INFO_OUTPUT surfInfoOutput;
        if (self->getHWInterface()->getHWAlignManager()->getSurfaceInfo2(&surfInfoInput, &surfInfoOutput)
            == kIOReturnSuccess)
        {
            fbInfo->savedSize =
                static_cast<UInt64>(surfInfoOutput.height) * surfInfoOutput.pitch * hwSpecificInfo.bytesPerPixel;
        }
    }

    UInt64 baseAlign = self->surfInfoOutputs()[fbIndex].baseAlign;
    UInt8  shift     = 0;
    while (page_size < baseAlign) {
        baseAlign >>= 1;
        shift      += 1;
    }
    fbInfo->pageCount = alignValue(page_size << shift);

    return ret;
}

void X5000::fixedGetSurfaceInfo(AMDRadeonX5000_AMDHWAlignManager* const self, AMD_SURFACE_INFO_STRUCT* const pStruct)
{
    if (pStruct == nullptr) { return; }

    pStruct->outWidth  = 0;
    pStruct->outHeight = 0;

    if (pStruct->version != 1 || pStruct->revision != 0 || pStruct->sizeOf != sizeof(*pStruct)) { return; }

    ADDR2_COMPUTE_SURFACE_INFO_INPUT input;
    input.width         = pStruct->inWidth;
    input.height        = pStruct->inHeight;
    input.bpp           = pStruct->bytesPerPixel * 8;
    input.resourceType  = ADDR_RSRC_TEX_2D;
    input.numSamples    = 1;
    input.numSlices     = 1;
    input.flags.display = 1;
    input.swizzleMode   = self->getPreferredSwizzleMode2(&input);    // this was hardcoded to 0xa

    ADDR2_COMPUTE_SURFACE_INFO_OUTPUT output;
    if (self->getSurfaceInfo2(&input, &output) == kIOReturnSuccess) {
        pStruct->outWidth      = static_cast<UInt16>(output.pitch);
        pStruct->outHeight     = static_cast<UInt16>(output.height);
        pStruct->outTilingMode = input.swizzleMode;    // I think AMD forgot this, albeit seemingly unused
    }
}

// ─── 第八步观测探针：加速器 `start` 的出口（注册链前置字段）──────────────────
//  读 `this+0x1f40`（framebuffer 服务）/ `+0x1f48`（`OSSymbol("SpecialAMDKey")`）/
//  `+0x1f58`（AMDRadeonServiceManager 客户端）/ `+0x1f60`（电源服务管理对象）。
//  判读：
//   · `f140 == 0` ⇒ 加速器找不到 framebuffer ⇒ **注册整段被跳过**（`controller+0x7960` 必空）；
//   · `f140 != 0` ⇒ 注册已被发起（此时同门控的 `callPlatformFunctionFromDrvr` 探针会先 panic，
//     本行不会出现）；`f148` 应是一个有效 OSSymbol 指针（可由真机读数反查）。
//  安全：只读对象字段，不调用任何 Apple 方法；门控 `-NRedAccelProbe`（默认不安装本 hook）。
bool X5000::wrapAccelStart(void* const self, void* const provider)
{
    // 入口读数（before）：`start` 会自行设置 `0x368`/`0x1e88` ⇒ 与出口对比可区分
    //  "从未设置"与"设置了又被清零"（真机出口 f368=0、f1e88 含 bit6 ⇒ 需 before 判定）。
    const UInt64 sBefore = reinterpret_cast<UInt64>(self);
    UInt64       f368Before = 0, f1e88Before = 0, f140Before = 0, f148Before = 0;
    if (sBefore >= 0xffffff7f80000000ULL) {
        f368Before  = *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(sBefore) + 0x368);
        f1e88Before = *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(sBefore) + 0x1E88);
        f140Before  = *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(sBefore) + 0x1F40);
        f148Before  = *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(sBefore) + 0x1F48);
    }

    // 诊断/修复实验（门控 `-NRedRestoreF140`，默认关闭）：基类 `IOGraphicsAccelerator2::start`
    //  在 `configureDevice` 失败后会**清理** `this+0x1f40`（framebuffer 服务）；而该值此前已被
    //  `configureDevice` 成功查到（真机非 0）⇒ 这里把它**恢复**回去，使 X5000 的 `start` 能走到
    //  "注册段"（判据：`accel drvr probe` 是否出现 / `controller+0x7960` 是否非 0 / IRI 是否成功）。
    //  ⚠️ 实验：写入的是 Apple 自己查到的指针（不构造对象、不调用任何 Apple 方法）。
    if (checkKernelArgument("-NRedRestoreF140") && sBefore >= 0xffffff7f80000000ULL && f140Before == 0 &&
        gLastF140 != 0) {
        *reinterpret_cast<UInt64*>(reinterpret_cast<UInt8*>(sBefore) + 0x1F40) = gLastF140;
        SYSLOG("X5000", "accel start: restored f140 = %llx", static_cast<unsigned long long>(gLastF140));
    }

    const auto ret = FunctionCast(wrapAccelStart, singleton().orgAccelStart)(self, provider);

    const UInt64 s = reinterpret_cast<UInt64>(self);
    UInt64       f140 = 0, f148 = 0, f158 = 0, f160 = 0, f368 = 0, f1e88 = 0;
    UInt64       f1e98 = 0, f1ea0 = 0, f1ea8 = 0, f1eb0 = 0, f1f08 = 0;
    if (s >= 0xffffff7f80000000ULL) {
        auto load64 = [](UInt64 base, UInt64 off) -> UInt64 {
            return *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(base) + off);
        };
        f140 = load64(s, 0x1F40);
        f148 = load64(s, 0x1F48);
        f158 = load64(s, 0x1F58);
        f160 = load64(s, 0x1F60);
        // f140 的查找起点与"曾设置成功"标志（离线：`0x3dae(self+0x368, 名字)` 成功后才 `orb $0x40,0x1e88`）
        f368  = load64(s, 0x368);
        f1e88 = load64(s, 0x1E88);
        // `start`（0x1290）按名查服务：`0x1e98`（provider）/`0x1ea0`（"Sleep/Wake"）/`0x1ea8`；
        //  任一为 0 即跳失败出口 0x1635 ⇒ `start` 返回失败 ⇒ 之后 `0x368` 不会被置 1、注册段不执行。
        f1e98 = load64(s, 0x1E98);
        f1ea0 = load64(s, 0x1EA0);
        f1ea8 = load64(s, 0x1EA8);
        f1eb0 = load64(s, 0x1EB0);
        f1f08 = load64(s, 0x1F08);
    }
    const UInt64 vRet  = ret ? 1 : 0;
    const UInt64 vProv = reinterpret_cast<UInt64>(provider);

    // 通道 A（**推荐**）：日志通道（门控 `-NRedAccelLog`，零挂死风险）——SYSLOG → `liludump` 落盘 → 离线读。
    if (checkKernelArgument("-NRedAccelLog")) {
        SYSLOG("X5000", "accel start probe: self=%llx provider=%llx ret=%llu f140=%llx f148=%llx f158=%llx f160=%llx",
               static_cast<unsigned long long>(s), static_cast<unsigned long long>(vProv),
               static_cast<unsigned long long>(vRet), static_cast<unsigned long long>(f140),
               static_cast<unsigned long long>(f148), static_cast<unsigned long long>(f158),
               static_cast<unsigned long long>(f160));
        SYSLOG("X5000", "accel start extra: f368=%llx f1e88=%llx f1e98=%llx f1ea0=%llx f1ea8=%llx f1eb0=%llx f1f08=%llx",
               static_cast<unsigned long long>(f368), static_cast<unsigned long long>(f1e88),
               static_cast<unsigned long long>(f1e98), static_cast<unsigned long long>(f1ea0),
               static_cast<unsigned long long>(f1ea8), static_cast<unsigned long long>(f1eb0),
               static_cast<unsigned long long>(f1f08));
        SYSLOG("X5000",
               "accel start extra2: f140=%llx f148=%llx f158=%llx f160=%llx | before f368=%llx f1e88=%llx f140=%llx "
               "f148=%llx",
               static_cast<unsigned long long>(f140), static_cast<unsigned long long>(f148),
               static_cast<unsigned long long>(f158), static_cast<unsigned long long>(f160),
               static_cast<unsigned long long>(f368Before), static_cast<unsigned long long>(f1e88Before),
               static_cast<unsigned long long>(f140Before), static_cast<unsigned long long>(f148Before));
    }

    // 通道 B：panic 通道（门控 `-NRedAccelProbe`，默认关闭；高风险，慎用）。格式串未改（已投产）。
    if (checkKernelArgument("-NRedAccelProbe")) {
        panic("NRed accel start probe: self=%llx provider=%llx ret=%llu | f140=%llx f148=%llx f158=%llx f160=%llx", s,
              vProv, vRet, f140, f148, f158, f160);
    }

    return ret;
}

// ─── 第八步观测（2026-09-28）：`AMDRTHardware::initializeTtl` 的运行时链路 ─────────
//  依据（离线反汇编，见 📄 `kb/re/TTL-initialize失败根因报告.md`）：
//   · 本函数（X5000，归零 VM `0x5e548`）就地构造 TTL 的入参结构：`+0x00`/`+0x08` = 两张回调表
//     （在 `this+0x20670` / `this+0x20688` 构造），`+0x20` = `*_GART_PARAMETERS`，
//     `+0x28` = `this+0x530`，`+0x30` = int；而 **`+0x10` 被清零后从未赋值**。
//   · 随后它调 `*(this+0x338)`（HWLibs 的 TTL 类）的 `vtable[0x30]` = `TTL::initialize`；
//     后者在「入参 `+0x10` == 0」时**直接返回 kIOReturnError**（未做任何实际工作）
//     ⇒ `TTL+0x578`（该表副本）保持 0、`TTL+0x5a0`（已初始化标志）保持 0。
//  本探针**只读内存字段、不调用任何 Apple 方法**，且 hook 无条件安装、内部才按门控输出；
//  观测走落盘通道（`-NRedTtlLog`）⇒ 不打断流程，一轮即可取回。
// ─── 第八步诊断实验：TTL 的"第三张回调表"桩（门控 `-NRedTtlStub`）─────────────────
//  用途见 `wrapInitializeTtl` 内的说明。表形态（离线反汇编 + 真机双向确证）为
//  `{ void* ctx; IOReturn (*fn)(void* ctx, void* in, void** out); }`。
static UInt64 gTtlStubTable[2] = {0, 0};

static IOReturn ttlStubCallback(void* ctx, void* in, void** out)
{
    (void)ctx;
    (void)in;
    if (out != nullptr) { *out = nullptr; }
    return kIOReturnSuccess;
}

void X5000::wrapInitializeTtl(void* const self, void* const gartParams)
{
    auto load64 = [](UInt64 base, UInt64 off) -> UInt64 {
        return *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(base) + off);
    };
    auto isKernelPtr = [](UInt64 p) -> bool { return p >= 0xffffff7f80000000ULL; };

    const UInt64 s = reinterpret_cast<UInt64>(self);
    const UInt64 g = reinterpret_cast<UInt64>(gartParams);

    UInt64 vt = 0, f338 = 0, f338vt = 0, slot30 = 0, f528 = 0, f530 = 0, f20690 = 0;
    UInt64 g0 = 0, g8 = 0, g10 = 0;
    if (isKernelPtr(s)) {
        vt     = load64(s, 0x000);
        f338   = load64(s, 0x338);
        f528   = load64(s, 0x528);
        f530   = load64(s, 0x530);
        f20690 = load64(s, 0x20690);
        if (isKernelPtr(f338)) {
            f338vt = load64(f338, 0x000);
            if (isKernelPtr(f338vt)) { slot30 = load64(f338vt, 0x30); }
        }
    }
    if (isKernelPtr(g)) {
        g0  = load64(g, 0x00);
        g8  = load64(g, 0x08);
        g10 = load64(g, 0x10);
    }

    // ── 判别性实验（门控 `-NRedTtlStub`，默认关闭）：补上 TTL 的"第三张回调表" ──
    //  真机（`observe-20260928-0153-ttl2`）判读：`TTL::initialize` 在**非 Safe Boot** 下有两道门——
    //   ① 入参 `+0x10` 非 0 ⇒ 直接返回 kIOReturnError（只有 safeboot 才继续）；
    //   ② 入参 `+0x10` == 0 且 `this+0x578` == 0 ⇒ 直接返回 kIOReturnError。
    //  本块在调用原函数**之前**把 `TTL+0x578` 写成本 kext 内的桩表 `{ctx, fn}`（`fn` 恒返回成功），
    //  使第 ② 道门通过。**判据**：`ttl5a0`（已初始化标志）是否由 0 变 1；以及加速器 `start` 的
    //  `f140` 是否变为非 0（注册段是否被走到）。
    //  ⚠️ 这是**诊断实验**，不是修复：桩表不实现任何真实回调语义。
    if (checkKernelArgument("-NRedTtlStub") && isKernelPtr(f338)) {
        auto store64 = [](UInt64 base, UInt64 off, UInt64 v) {
            *reinterpret_cast<UInt64*>(reinterpret_cast<UInt8*>(base) + off) = v;
        };
        gTtlStubTable[0] = f338;
        gTtlStubTable[1] = reinterpret_cast<UInt64>(&ttlStubCallback);
        const UInt64 tableAddr = reinterpret_cast<UInt64>(&gTtlStubTable[0]);
        store64(f338, 0x578, tableAddr);
        SYSLOG("X5000", "ttl-stub: TTL=%llx TTL+0x578 <- %llx (fn=%llx)", static_cast<unsigned long long>(f338),
               static_cast<unsigned long long>(tableAddr),
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(&ttlStubCallback)));
    }

    // 判别实验（门控 `-NRedTtlStub48`，默认关闭）：真机已证 `*(TTL+0x48) = 0` ⇒ 命中出口 4
    //  （`0xa285f` 要求 `*(TTL+0x48)` 的三个字段非 0）。`0xa285f` 只是**拷贝**这些字段
    //  （`*(rcx+0x38/0x40/0x48) = 桩字段`），**不会把它们当函数调用** ⇒ 用自指的最小桩即可。
    if (checkKernelArgument("-NRedTtlStub48") && isKernelPtr(f338)) {
        auto store64 = [](UInt64 base, UInt64 off, UInt64 v) {
            *reinterpret_cast<UInt64*>(reinterpret_cast<UInt8*>(base) + off) = v;
        };
        const UInt64 stub = f338 + 0xB00;   // TTL 对象 0xb40 字节 ⇒ 用尾部空闲区
        store64(stub, 0x00, stub);
        store64(stub, 0x08, stub);
        store64(stub, 0x10, stub);
        store64(f338, 0x48, stub);
        SYSLOG("X5000", "ttl-stub48: TTL=%llx TTL+0x48 <- %llx", static_cast<unsigned long long>(f338),
               static_cast<unsigned long long>(stub));
    }

    // 入口读数（与出口对比用）：`0x8b10e` 会**改写** `&TTL+0x50` 子结构 ⇒ 若某字段调用前后
    //  完全不变，说明"写它的那一步"没被走到，即可反推失败步。
    if (checkKernelArgument("-NRedTtlLog") && isKernelPtr(f338)) {
        auto ld32 = [](UInt64 b, UInt64 o) -> UInt32 {
            return *reinterpret_cast<const UInt32*>(reinterpret_cast<const UInt8*>(b) + o);
        };
        SYSLOG("X5000",
               "ttl before: t50=%x t54=%x t58=%llx t60=%llx t68=%llx t1a0=%llx t4e0=%llx t4a0=%llx t88=%x ta0=%llx",
               static_cast<unsigned int>(ld32(f338, 0x50)), static_cast<unsigned int>(ld32(f338, 0x54)),
               static_cast<unsigned long long>(load64(f338, 0x58)), static_cast<unsigned long long>(load64(f338, 0x60)),
               static_cast<unsigned long long>(load64(f338, 0x68)), static_cast<unsigned long long>(load64(f338, 0x1A0)),
               static_cast<unsigned long long>(load64(f338, 0x4E0)), static_cast<unsigned long long>(load64(f338, 0x4A0)),
               static_cast<unsigned int>(ld32(f338, 0x88)),
               static_cast<unsigned long long>(load64(f338, 0xA0)));
    }

    FunctionCast(wrapInitializeTtl, singleton().orgInitializeTtl)(self, gartParams);

    UInt64 ttl568 = 0, ttl570 = 0, ttl578 = 0, ttl580 = 0, ttl588 = 0, ttl590 = 0, ttl598 = 0, ttl5a0 = 0;
    // 0x8b10e 的第一步 0x90d75 检查 `&TTL+0x50` 处的一个子结构（离线：`f0 >= 0x130`、
    //  `f4 <= 9`、`f8/f10/f18` 非空）——失败则 status = 4。故读这几项定位不满足者。
    UInt32 ttl50 = 0, ttl54 = 0, ttl88 = 0;
    UInt64 ttl58 = 0, ttl60 = 0, ttl68 = 0, ttl1a0 = 0, ttl4e0 = 0, ttl4a0 = 0, ttla0 = 0;
    UInt64 t48 = 0, t48_0 = 0, t48_40 = 0, t48_48 = 0;
    if (isKernelPtr(f338)) {
        auto load32 = [](UInt64 base, UInt64 off) -> UInt32 {
            return *reinterpret_cast<const UInt32*>(reinterpret_cast<const UInt8*>(base) + off);
        };
        ttl50  = load32(f338, 0x50);
        ttl54  = load32(f338, 0x54);
        ttl58  = load64(f338, 0x58);
        ttl60  = load64(f338, 0x60);
        ttl68  = load64(f338, 0x68);
        ttl1a0 = load64(f338, 0x1A0);
        ttl4e0 = load64(f338, 0x4E0);
        ttl4a0 = load64(f338, 0x4A0);
        ttl88  = load32(f338, 0x88);
        ttla0  = load64(f338, 0xA0);
        // 出口 4 判据（子 agent `TtlExits`）：`0xa285f(r14, config+0x28)` 要求 `*(config+0x28)` 的
        //  +0x00 / +0x40 / +0x48 均非 0（`config` = `&TTL+0x20`，故 `config+0x28` = `&TTL+0x48`）。
        t48 = load64(f338, 0x48);
        if (isKernelPtr(t48)) {
            t48_0  = load64(t48, 0x00);
            t48_40 = load64(t48, 0x40);
            t48_48 = load64(t48, 0x48);
        }
    }
    if (isKernelPtr(f338)) {
        ttl568 = load64(f338, 0x568);
        ttl570 = load64(f338, 0x570);
        ttl578 = load64(f338, 0x578);    // ★ 失败判据：参数+0x10 的副本；== 0 即"零表早退"
        ttl580 = load64(f338, 0x580);    // safeboot 标志
        ttl588 = load64(f338, 0x588);    // = *(_GART_PARAMETERS)（nonlocalMemSizeLimitBytes）
        ttl590 = load64(f338, 0x590);
        ttl598 = load64(f338, 0x598);
        ttl5a0 = load64(f338, 0x5A0);    // 已初始化标志（成功才为 1）
    }

    if (checkKernelArgument("-NRedTtlLog")) {
        SYSLOG("X5000",
               "ttl iface: self=%llx gart=%llx | vt=%llx f338=%llx f338vt=%llx slot30=%llx f528=%llx f530=%llx "
               "f20690=%llx | g0=%llx g8=%llx g10=%llx | ttl568=%llx ttl570=%llx ttl578=%llx ttl580=%llx ttl588=%llx "
               "ttl590=%llx ttl598=%llx ttl5a0=%llx",
               static_cast<unsigned long long>(s), static_cast<unsigned long long>(g),
               static_cast<unsigned long long>(vt), static_cast<unsigned long long>(f338),
               static_cast<unsigned long long>(f338vt), static_cast<unsigned long long>(slot30),
               static_cast<unsigned long long>(f528), static_cast<unsigned long long>(f530),
               static_cast<unsigned long long>(f20690), static_cast<unsigned long long>(g0),
               static_cast<unsigned long long>(g8), static_cast<unsigned long long>(g10),
               static_cast<unsigned long long>(ttl568), static_cast<unsigned long long>(ttl570),
               static_cast<unsigned long long>(ttl578), static_cast<unsigned long long>(ttl580),
               static_cast<unsigned long long>(ttl588), static_cast<unsigned long long>(ttl590),
               static_cast<unsigned long long>(ttl598), static_cast<unsigned long long>(ttl5a0));
        SYSLOG("X5000",
               "ttl sub50: t50=%x t54=%x t58=%llx t60=%llx t68=%llx t1a0=%llx t4e0=%llx t4a0=%llx t88=%x ta0=%llx",
               static_cast<unsigned int>(ttl50), static_cast<unsigned int>(ttl54),
               static_cast<unsigned long long>(ttl58), static_cast<unsigned long long>(ttl60),
               static_cast<unsigned long long>(ttl68), static_cast<unsigned long long>(ttl1a0),
               static_cast<unsigned long long>(ttl4e0), static_cast<unsigned long long>(ttl4a0),
               static_cast<unsigned int>(ttl88), static_cast<unsigned long long>(ttla0));
        SYSLOG("X5000", "ttl exit4: t48=%llx t48_0=%llx t48_40=%llx t48_48=%llx", static_cast<unsigned long long>(t48),
               static_cast<unsigned long long>(t48_0), static_cast<unsigned long long>(t48_40),
               static_cast<unsigned long long>(t48_48));
    }
}

// ─── 第八步观测（2026-09-28）：`configureDevice` 与 `initLinkToPeer` ────────────
//  离线结论：`this+0x1f40`（framebuffer 服务）的**唯一设置者**是
//  `AMDGraphicsAccelerator::configureDevice`（VM 0x3306）：它在 `0x338b`/`0x33a7` 调
//  `initLinkToPeer`（VM 0x3dae）按名查 `"ATIFramebuffer"`，失败再查 `"IOFramebuffer"`，
//  命中才写 `this+0x1f40`（0x3393/0x33ac）并 `orb $0x40,0x1e88`（0x33bd）。
//  真机已证：`start` 入口/出口 `f140` 均为 0、且该对象 `f1e88` 入口为 0（首次 start）。
//  ⚠️ **更正（2026-09-29，由乙线 R1' 离线分析发现）**：此处原写"`this+0x1f40` **从未被设置**"**有误**——
//     第 10 轮真机读数（`docs/真机轮次台账.md` 第 10 行 / 归档 `observe-20260928-0245-cfgdev`）显示
//     `configureDevice` **确被调用，且其时 `this+0x1f40` 非 0**，即它**被设置过**；随后基类
//     `IOGraphicsAccelerator2::start`（IOAF VM 0x3ba10）在失败清理中把 `this+0x1f40` **清空**
//     （第 18 轮 `-NRedRestoreF140` 实测"入口恢复生效、`start` 内部又清掉"即为旁证）。
//  ⇒ 正确的因果是：**`f140=0` 是结果而非原因**；真正的失败点在 `configureDevice` 内的
//     `0x360f` 检查（详见 `docs/子任务/乙线R1-加速器注册链离线分析.md` §Q2）。**勿再据"从未被设置"做推断。**
//  ⇒ 注册段跳过 ⇒ `start` 返回失败。
//  本组探针回答"configureDevice 是否被调用、initLinkToPeer 查到什么、返回值如何"。
//  只读字段 + 记录入参/返回值；落盘通道（`-NRedAccelLog`），hook 无条件安装。
// ─── R1'-Diag 只读诊断探针的读数函数（`-NRedDiagProvider`，默认关）──────────────────
//  目的：判别 P8（`ATY,bin_image`）与 P5（HWServices 服务匹配）当前是否已满足，
//    避免在不知情时花真机轮次做注入。依据：`docs/子任务/乙线R1-P8实现前置评估.md` §1.2
//    （setupCAIL 读 provider 的 `ATY,bin_image`，三出口 8-2/8-3/8-4）与
//    `docs/子任务/乙线R1-P5作用点专项.md` §1.2（getTtl/getCail = 节点 +0xd8/+0xd0）。
//  时序（2026-09-30 复审，指令级证据，见 `tmp/re/diagprobe_*`）：
//    configureDevice(kc 0x4b3a306) @0x4b3a45d call *0xb30 = createHWInterface
//    (0x4b3d25c) @0x4b3d2d1 call *0x118 = AMDGFX9Hardware::init (0x4b9a72e)
//    → AMDRTHardware::init (0x4b954be) → AMDHardware::init (0x4ba9cea)
//    @0x4ba9f1d call *0x660 = setupCAIL (0x4babee4) —— **全链同步嵌套在
//    configureDevice 内** ⇒ wrapConfigureDevice 的出口读数（x 相位）在 setupCAIL **之后**，
//    入口读数（e 相位）在 setupCAIL **之前** ⇒ 用 e/x 两个相位把 setupCAIL 夹住。
//  判读：
//    · e 相位读到 `ATY,bin_image` 存在 ⇒ setupCAIL 一定看到（P8 已满足）；
//    · e/x 双相位都读不到 ⇒ setupCAIL 也读不到（P8 8-2 失败，**决定性**）；
//    · e 读不到而 x 读到 ⇒ 属性在 configureDevice 窗口内被写入（生产者时序存疑）⇒
//      须与点 B（powerUp，更晚）及 framebuffer 的 readAtomBios SYSLOG 联合判读。
//  安全上下文：只读 IORegistry 属性（`IOService::getProperty` + OSMetaClass 原生取值器）
//    与裸内存字段；不调 Apple 驱动虚方法、不读寄存器、不写内存、不构造对象。
//    落点与既有 `-NRedR1B30Probe` / `-NRedEngTblProbe` 同函数（R1B30 已真机第 79 轮实跑）。
//    落盘复用 L1（SYSLOG）/L2，不新建通道。
static void diagProviderDump(const char* const phase, void* const provider, const UInt64 selfAddr, const UInt64 ret)
{
    const UInt64 provU = reinterpret_cast<UInt64>(provider);
    SYSLOG("X5000", "R1PDiag enter: ph=%s self=%llx prov=%llx ret=%llu", phase,
           static_cast<unsigned long long>(selfAddr), static_cast<unsigned long long>(provU),
           static_cast<unsigned long long>(ret));
    if (provU < 0xffffff7f80000000ULL) {
        SYSLOG("X5000", "R1PDiag skip: ph=%s prov=%llx", phase, static_cast<unsigned long long>(provU));
        return;
    }
    auto* const provSvc = reinterpret_cast<IOService*>(provider);
    auto probeAttr = [provSvc](const char* const key, UInt64& decl, UInt64& kind, UInt64& val) {
        OSMetaClassBase* const o = provSvc->getProperty(key);
        if (o == nullptr) { return; }
        decl = 1;
        if (OSDynamicCast(OSBoolean, o) != nullptr) {
            kind = 1;
            val  = (static_cast<OSBoolean*>(o))->getValue() ? 1 : 0;
            return;
        }
        if (auto* const num = OSDynamicCast(OSNumber, o)) {
            kind = 2;
            val  = num->unsigned64BitValue();
            return;
        }
        if (auto* const dat = OSDynamicCast(OSData, o)) {
            kind = 3;
            val  = (dat->getLength() >= 1) ? static_cast<UInt64>(*static_cast<const UInt8*>(dat->getBytesNoCopy())) : 0;
            return;
        }
        if (auto* const str = OSDynamicCast(OSString, o)) {
            kind = 4;
            const char* const s2 = str->getCStringNoCopy();
            if (s2 != nullptr && s2[0] != '\0') {
                UInt64 v = 0;
                UInt64 n = 0;
                // 值编码（**前 7 字节**）：`val = (编码字符数<<56) | 前 7 字符(大端)`。
                //  ⚠️ 2026-09-30 独立复核发现：首版沿用 R1B30 的 `n < 15` 写法，但
                //  `UInt64` 只容纳 8 字节，15 次左移会把**前 8 字节溢出丢弃** ⇒ 实得
                //  "后 7 字节"（如 "IOAccelerator" 得 "lerator"），与注释声称的"前 11 字节"不符。
                //  ⇒ 本轮改为 `n < 7`：7 字节 × 8 位 = 56 位，恰好不溢出 ⇒ 得**前 7 字符**。
                for (const char* p = s2; *p != '\0' && n < 7; ++p, ++n) {
                    v = (v << 8) | static_cast<UInt64>(static_cast<UInt8>(*p));
                }
                val = (static_cast<UInt64>(n) << 56) | (v & 0x00FFFFFFFFFFFFFFULL);
            }
            return;
        }
        kind = 0;   // 存在但为其它 OSMetaClass 类型
    };
    const char* const keys[] = {"IOMatchCategory", "LoadHWServices", "LoadAccelerator", "LoadPlugIn",
                                "ATY,VRAM,total",  "ATY,bin_image"};
    for (size_t ki = 0; ki < arrsize(keys); ++ki) {
        UInt64 d = 0, k = 0, v = 0;
        probeAttr(keys[ki], d, k, v);
        SYSLOG("X5000", "R1PDiag attr: ph=%s key=%s decl=%llu kind=%llu val=%llx", phase, keys[ki],
               static_cast<unsigned long long>(d), static_cast<unsigned long long>(k),
               static_cast<unsigned long long>(v));
    }
    // `ATY,bin_image` 专项：OSData 类型、长度（≤0x20000）、前 32 字节（P8 判据）。
    UInt64 biDecl = 0, biIsData = 0, biLen = 0, biInRange = 0;
    UInt8  biHead[32] = {0};
    OSMetaClassBase* const bio = provSvc->getProperty("ATY,bin_image");
    if (bio != nullptr) {
        biDecl = 1;
        if (auto* const bd = OSDynamicCast(OSData, bio)) {
            biIsData = 1;
            biLen    = bd->getLength();
            if (biLen >= 1 && biLen <= 0x20000ULL) { biInRange = 1; }
            if (const void* const bp = bd->getBytesNoCopy()) {
                const size_t n = (biLen < 32) ? static_cast<size_t>(biLen) : 32;
                memcpy(biHead, bp, n);
            }
        }
    }
    SYSLOG("X5000", "R1PDiag binimg: ph=%s decl=%llu isData=%llu len=%llu inRange=%llu", phase,
           static_cast<unsigned long long>(biDecl), static_cast<unsigned long long>(biIsData),
           static_cast<unsigned long long>(biLen), static_cast<unsigned long long>(biInRange));
    // 前 32 字节，分两行打印（每行 **16 字节** = 32 个十六进制字符）。
    //  ⚠️ 每 half 只读 `biHead[half*16 + i]`，i < 16 ⇒ 最大下标 31，不越界。
    for (int half = 0; half < 2; ++half) {
        char hex[33];
        for (int i = 0; i < 16; ++i) {
            const UInt8 b = biHead[half * 16 + i];
            hex[i * 2]     = "0123456789abcdef"[b >> 4];
            hex[i * 2 + 1] = "0123456789abcdef"[b & 0xF];
        }
        hex[32] = '\0';
        SYSLOG("X5000", "R1PDiag binhex: ph=%s idx=%d %s", phase, half, hex);
    }
    // x 相位追加：`self+0x1a38`（createHWInterface 的持有字段，纯内存读；已内核指针校验）。
    //  用途 = P8 判读的**前置闸**（复核方问题 2）：e/x 双 absent ⇒ "P8 8-2 失败（决定性）"
    //  成立的前提是 `setupCAIL` 确实执行过；若 `f1a38 == 0`（对象未建出/已被清 0）则链可能
    //  止于 P8 之前 ⇒ 结论降级为"P8 注册表层面未满足（条件性）"。只在 x 相位打（e 相位它无意义）。
    if (phase[0] == 'x' && selfAddr >= 0xffffff7f80000000ULL) {
        const UInt64 f1a38 = *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(selfAddr) + 0x1A38);
        SYSLOG("X5000", "R1PDiag f1a38: ph=%s self=%llx val=%llx", phase,
               static_cast<unsigned long long>(selfAddr), static_cast<unsigned long long>(f1a38));
    }
}
UInt64 X5000::wrapConfigureDevice(void* const self, void* const provider)
{
    const UInt64 s = reinterpret_cast<UInt64>(self);
    if (checkKernelArgument("-NRedAccelLog")) {
        SYSLOG("X5000", "cfgdev enter: self=%llx provider=%llx", static_cast<unsigned long long>(s),
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(provider)));
        // 读 `configureDevice` 在 `0x360f` 检查的那个键（**只读**）。
        //  基址语义已用真机验证：`vptr − gX5000Slide = (0x4D34028 − 0x4B37000) + 0x10`
        //  （Vega10 accelerator vtable 的第一个虚函数）⇒ **`gX5000Slide` = X5000 的运行时基址**，
        //  故 `kc 绝对 → 运行时 = gX5000Slide + (kc绝对 − 0x4B37000)`。
        if (s >= 0xffffff7f80000000ULL && gX5000Slide != 0) {
            const UInt64 gotAbs = gX5000Slide + 0x1ED118ULL;    // 0x1ED118 的归零 vm
            UInt64       got = 0;
            got = *reinterpret_cast<const UInt64*>(gotAbs);
            SYSLOG("X5000", "cfgdev key-probe: base=%llx gotAbs=%llx got=%llx",
                   static_cast<unsigned long long>(gX5000Slide), static_cast<unsigned long long>(gotAbs),
                   static_cast<unsigned long long>(got));
            if (got >= 0xffffff8000000000ULL) {
                // `got` 是内核侧对象（上一轮读 `+0x18` 得垃圾 ⇒ 布局与预期不符）⇒ dump 前 6 个 qword，
                //  并对"像内核指针"的项做**限长**字符串试读（%.32s，零外推、不调用任何 Apple 方法）。
                UInt64 q[6] = {0, 0, 0, 0, 0, 0};
                for (int i = 0; i < 6; ++i) { q[i] = *reinterpret_cast<const UInt64*>(got + static_cast<UInt64>(i) * 8); }
                SYSLOG("X5000", "cfgdev key-q: %llx %llx %llx %llx %llx %llx",
                       static_cast<unsigned long long>(q[0]), static_cast<unsigned long long>(q[1]),
                       static_cast<unsigned long long>(q[2]), static_cast<unsigned long long>(q[3]),
                       static_cast<unsigned long long>(q[4]), static_cast<unsigned long long>(q[5]));
                for (int i = 0; i < 6; ++i) {
                    if (q[i] >= 0xffffff8000000000ULL) {
                        SYSLOG("X5000", "cfgdev key-str[%d]: %.32s", i, reinterpret_cast<const char*>(q[i]));
                    }
                }
            }
        }
    }

    // ─── R1'-Diag 入口相位（`-NRedDiagProvider`，默认关）──────────────────────────
    //  e 相位 = `FunctionCast(orgConfigureDevice)` **之前**（setupCAIL 之前）：
    //   若此处已读到 `ATY,bin_image` ⇒ setupCAIL 必看到 ⇒ P8 已满足（决定性）。
    //   安全：与出口相位同函数同形态（`diagProviderDump` 只读 provider 属性 + 裸内存字段）；
    //   `provider` 形参此时原样有效（尚未被 Apple 代码使用）。ret 尚不存在 ⇒ 传 0（仅自证用）。
    if (checkKernelArgument("-NRedDiagProvider")) { diagProviderDump("e", provider, s, 0); }

    UInt64 ret = FunctionCast(wrapConfigureDevice, singleton().orgConfigureDevice)(self, provider);

    // 判别性实验（门控 `-NRedCfgDevForce`，默认关闭）：`configureDevice` 在 `0x360f` 检查失败后
    //  返回 false ⇒ 基类 `IOGraphicsAccelerator2::start` 判定 "configureDevice failed" ⇒ 失败退出
    //  并**清空 `this+0x1f40`**（framebuffer 服务）⇒ 加速器注册段被跳过。本块把它强制改为"成功"，
    //  用于判定"这一处检查是否为唯一阻塞"（判据：`f140` 是否保留、`start` 是否继续、注册是否发起）。
    //  ⚠️ 诊断实验，不是修复。
    if (ret == 0 && checkKernelArgument("-NRedCfgDevForce")) {
        SYSLOG("X5000", "cfgdev: forcing success (original ret = 0)");
        ret = 1;
    }

    UInt64 f140 = 0, f1e88 = 0, f368 = 0, f1f28 = 0, f1f30 = 0, f1a68 = 0, f1a40 = 0;
    if (s >= 0xffffff7f80000000ULL) {
        auto load64 = [](UInt64 base, UInt64 off) -> UInt64 {
            return *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(base) + off);
        };
        f140  = load64(s, 0x1F40);
        f1e88 = load64(s, 0x1E88);
        f368  = load64(s, 0x368);
        // configureDevice 的 `je 0x368b` 失败出口逐个读回：0x33ee→f1f28、0x342f→f1a68、0x344d→f1a40
        f1f28 = load64(s, 0x1F28);
        f1f30 = load64(s, 0x1F30);
        f1a68 = load64(s, 0x1A68);
        f1a40 = load64(s, 0x1A40);
    }
    // ─── R1' 最小读数探针捕获（`-NRedR1Probe`，默认关）───────────────────────────
    //  见文件头全局量处的说明：**纯内存读**（不调方法、不读寄存器、不写内存）。
    if (checkKernelArgument("-NRedR1Probe")) {
        gR1bProbeEnabled = true;
        ++gR1bCfgDevCalls;
        gR1bCfgDevSelf     = s;
        gR1bCfgDevProvider = reinterpret_cast<UInt64>(provider);
        gR1bCfgDevRet      = ret;
        if (ret != 0) { ++gR1bCfgDevRetNZ; }
        if (s >= 0xffffff7f80000000ULL) {
            auto load64 = [](UInt64 base, UInt64 off) -> UInt64 {
                return *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(base) + off);
            };
            gR1bF1F40 = load64(s, 0x1F40);
            gR1bF1F28 = load64(s, 0x1F28);
            gR1bF1F30 = load64(s, 0x1F30);
            gR1bF1A68 = load64(s, 0x1A68);
            gR1bF1A40 = load64(s, 0x1A40);
            gR1bF1A38 = load64(s, 0x1A38);
            gR1bF1F10 = load64(s, 0x1F10);
            gR1bF1E88 = load64(s, 0x1E88);
            gR1bF368  = load64(s, 0x368);
            gR1bZeroMask |= (gR1bF1F40 == 0 ? 1ULL : 0ULL) | (gR1bF1F28 == 0 ? 2ULL : 0ULL)
                          | (gR1bF1A68 == 0 ? 4ULL : 0ULL) | (gR1bF1A40 == 0 ? 8ULL : 0ULL)
                          | (gR1bF1A38 == 0 ? 16ULL : 0ULL);
            // f1a38 所指对象的 vtable 指针（供离线定名；纯读，不调用其任何方法）
            if (gR1bF1A38 >= 0xffffff7f80000000ULL) {
                gR1bObj1A38Vtable = *reinterpret_cast<const UInt64*>(gR1bF1A38);
            }
            // *0x1ed118（GOT 项）所指对象的首字段（aux 形状检查第二参的来源）
            if (gX5000Slide != 0) {
                const UInt64 gotPtr = *reinterpret_cast<const UInt64*>(gX5000Slide + 0x1ED118ULL);
                if (gotPtr >= 0xffffff8000000000ULL) {
                    gR1bKeyObjFirstField = *reinterpret_cast<const UInt64*>(gotPtr);
                }
            }
        }
    }
    // ─── R1'-B30 身份探针捕获（`-NRedR1B30Probe`，默认关）────────────────────────
    //  见文件头全局量处的说明：**纯内存读 + 一次属性读**，不调 Apple 方法、不读寄存器、不写内存。
    //  存放于 `wrapConfigureDevice` 的**最终失败出口**（`0x346e` 的对应物），保证读数取的是
    //  第 78 轮判定的那一次 `configureDevice` 返回。
    if (checkKernelArgument("-NRedR1B30Probe") && s >= 0xffffff7f80000000ULL) {
        gR1b30ProbeArmed = true;
        gR1b30Base       = gX5000Slide;
        // 本块自带的读指针 helper：**不得**依赖 R1 探针块内的同名 lambda（那是块作用域，
        //  在本块不可见——CI #222 的 `use of undeclared identifier 'load64'` 即由此而来）。
        auto load64 = [](UInt64 base, UInt64 off) -> UInt64 {
            return *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(base) + off);
        };
        // ① this 的 vptr（纯内存读）⇒ 归零 vm 用于与静态 `__ZTV` 对照
        const UInt64 selfVt = *reinterpret_cast<const UInt64*>(s);
        gR1b30SelfVptr = selfVt;
        if (gX5000Slide != 0 && selfVt >= 0xffffff7f80000000ULL && selfVt > gX5000Slide) {
            gR1b30SelfVptrZvm = selfVt - gX5000Slide;
        }
        // ② vtable 三槽的**运行时目标**（只读槽值，**绝不 call**）⇒ 与离线静态目标对照
        if (selfVt >= 0xffffff7f80000000ULL) {
            gR1b30SlotB20Target = *reinterpret_cast<const UInt64*>(selfVt + 0xB20);
            gR1b30SlotB28Target = *reinterpret_cast<const UInt64*>(selfVt + 0xB28);
            gR1b30SlotB30Target = *reinterpret_cast<const UInt64*>(selfVt + 0xB30);
            if (gX5000Slide != 0) {
                if (gR1b30SlotB20Target > gX5000Slide) { gR1b30SlotB20Zvm = gR1b30SlotB20Target - gX5000Slide; }
                if (gR1b30SlotB28Target > gX5000Slide) { gR1b30SlotB28Zvm = gR1b30SlotB28Target - gX5000Slide; }
                if (gR1b30SlotB30Target > gX5000Slide) { gR1b30SlotB30Zvm = gR1b30SlotB30Target - gX5000Slide; }
            }
        }
        // ③ 兄弟槽结果对象的 vptr（给 0xb20/0xb28 定名；纯内存读，不调用其任何方法）
        const UInt64 o1a68 = load64(s, 0x1A68);
        const UInt64 o1a40 = load64(s, 0x1A40);
        if (o1a68 >= 0xffffff7f80000000ULL) {
            const UInt64 vt = *reinterpret_cast<const UInt64*>(o1a68);
            gR1b30Vt1A68 = vt;
            if (gX5000Slide != 0 && vt >= 0xffffff7f80000000ULL && vt > gX5000Slide) {
                gR1b30Vt1A68Zvm = vt - gX5000Slide;
            }
        }
        if (o1a40 >= 0xffffff7f80000000ULL) {
            const UInt64 vt = *reinterpret_cast<const UInt64*>(o1a40);
            gR1b30Vt1A40 = vt;
            if (gX5000Slide != 0 && vt >= 0xffffff7f80000000ULL && vt > gX5000Slide) {
                gR1b30Vt1A40Zvm = vt - gX5000Slide;
            }
        }
        gR1b30F1A38 = load64(s, 0x1A38);
        // ④ provider 属性（**只读**）：`IOMatchCategory` 与 `AAPL,aux-power-connected` 三项。
        //    与安全位置处的 `readBoolAttr` 同一形态：只调用 IOKit 的 `IOService::getProperty`，
        //    并只对**属性值对象自身**用原生的 OSBoolean/OSNumber/OSData/OSString 取值
        //    （`declared` 与 `value` 均为 OSMetaClass 的原生访问器，不属 Apple 驱动虚方法）。
        {
            const UInt64 provU = reinterpret_cast<UInt64>(provider);
            if (provU >= 0xffffff7f80000000ULL) {
                gR1b30Prov = provU;
                auto* const provSvc = reinterpret_cast<IOService*>(provider);
                auto probeAttr = [provSvc](const char* const key, UInt64& decl, UInt64& kind, UInt64& val) {
                    OSMetaClassBase* const o = provSvc->getProperty(key);
                    if (o == nullptr) { return; }
                    decl = 1;
                    if (OSDynamicCast(OSBoolean, o) != nullptr) {
                        kind = 1;
                        val  = (static_cast<OSBoolean*>(o))->getValue() ? 1 : 0;
                        return;
                    }
                    if (auto* const num = OSDynamicCast(OSNumber, o)) {
                        kind = 2;
                        val  = num->unsigned64BitValue();
                        return;
                    }
                    if (auto* const dat = OSDynamicCast(OSData, o)) {
                        kind = 3;
                        // 只取首字节（限长读；不假定长度）
                        val  = (dat->getLength() >= 1) ? static_cast<UInt64>(*static_cast<const UInt8*>(dat->getBytesNoCopy())) : 0;
                        return;
                    }
                    if (auto* const str = OSDynamicCast(OSString, o)) {
                        kind = 4;
                        // 值用**与机器字长无关**的编码：编码字符数（≤7）+ 前 7 字节。
                        //  首字节放编码字符数，既避免 NUL 截断歧义，也让 `val != 0` 自身即"非空字符串"判据。
                        //  ⚠️ 2026-09-30 独立复核修正（与 `-NRedDiagProvider` 同型缺陷）：
                        //    原 `n < 15` 在 `UInt64` 左移 8 次后**前 8 字节溢出丢弃** ⇒ 实得
                        //    "后 7 字节"（"IOAccelerator" 得 "lerator"），与原注释"前 11 字节"不符。
                        //    ⇒ 改为 `n < 7`（56 位恰好不溢出）⇒ 得**前 7 字符**。
                        //  这样离线判读只需按 ASCII 手工比对，不依赖任何哈希实现：
                        //    "IOAccelerator" (13 字节) ⇒ val = 0x07 + "IOAcceler" 的 7 字节。
                        const char* const s2 = str->getCStringNoCopy();
                        if (s2 != nullptr && s2[0] != '\0') {
                            UInt64 v = 0;
                            UInt64 n = 0;
                            for (const char* p = s2; *p != '\0' && n < 7; ++p, ++n) {
                                v = (v << 8) | static_cast<UInt64>(static_cast<UInt8>(*p));
                            }
                            val = (static_cast<UInt64>(n) << 56) | (v & 0x00FFFFFFFFFFFFFFULL);
                        }
                        return;
                    }
                    kind = 0;   // 存在但为其它 OSMetaClass 类型
                };
                probeAttr("IOMatchCategory", gR1b30McDeclared, gR1b30McKind, gR1b30McValue);
                probeAttr("AAPL,aux-power-connected", gR1b30AuxDeclared, gR1b30AuxKind, gR1b30AuxValue);
                gR1b30AuxOk = 1;
            }
        }
    }
    // ─── R1'-Cwi 探针捕获（`-NRedR1CwiProbe`，默认关）────────────────────────────
    //  见文件头全局量处的说明：**只加一个 8 字节读**（`this+0x1e89`），外加复核既有的 `this+0x1a38`。
    //  ⚠️ 只读 `this` 自身的两个偏移；**不解引用** `this+0x1a38` 所指对象（可能为 NULL）。
    if (checkKernelArgument("-NRedR1CwiProbe") && s >= 0xffffff7f80000000ULL) {
        gR1cwiProbeArmed = true;
        gR1cwiBase       = gX5000Slide;
        gR1cwiSelf       = s;
        ++gR1cwiCalls;
        // 本块自带的读指针 helper（与 B30 块同一形态；**不得**依赖其它块内的同名 lambda）。
        const UInt64 f1e89 = *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(s) + 0x1E89);
        gR1cwiF1E89   = f1e89;
        gR1cwiF1E89Lo = f1e89 & 0xFFULL;
        if ((f1e89 & 0x1ULL) == 0) { gR1cwiF1E89ZeroMask = 1; }
        gR1cwiF1A38 = *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(s) + 0x1A38);
    }
    // ─── R1'-EngTbl 探针捕获（`-NRedEngTblProbe`，默认关）─────────────────────────
    //  见文件头全局量处的说明：**纯内存读**（不调方法、不读寄存器、不写内存）。
    //  读法：`this+0x1a38`（= AMDHardware*）→ 该对象 `+0x3b8 + i*8`（i = 0..15）。
    //  ⚠️ 三重校验后才解引用：① `this` 为内核指针；② `this+0x1a38` 非 0 且为内核指针；
    //     ③ 每个槽读之前不再额外校验（槽本身可以是 0 —— 那正是我们要测的东西）。
    //  ⛔ 不 `call` 任何槽：`this+0x1a38` 的 vptr 只**读**其首字段，绝不调用。
    if (checkKernelArgument("-NRedEngTblProbe") && s >= 0xffffff7f80000000ULL) {
        gEngTblArmed = true;
        gEngTblBase  = gX5000Slide;
        gEngTblSelf  = s;
        ++gEngTblCalls;
        auto rd64 = [](UInt64 base, UInt64 off) -> UInt64 {
            return *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(base) + off);
        };
        const UInt64 hw = rd64(s, 0x1A38);      // = AMDHardware*（本机应为 0）
        gEngTblHw = hw;
        // 只有 hw 是合法内核指针时才继续（`hw == 0` 时**不解引用**）
        if (hw >= 0xffffff7f80000000ULL) {
            const UInt64 hwVt = rd64(hw, 0x000);   // 只读首字段（vptr），**不调用**
            gEngTblHwVptr = hwVt;
            if (gX5000Slide != 0 && hwVt >= 0xffffff7f80000000ULL && hwVt > gX5000Slide) {
                gEngTblHwZvm = hwVt - gX5000Slide;
            }
            // 前 16 个引擎槽（**不扫描整表**）
            UInt64 nn = 0;
            for (UInt32 i = 0; i < kEngTblSlots; ++i) {
                const UInt64 v = rd64(hw, 0x3B8ULL + static_cast<UInt64>(i) * 8ULL);
                gEngTblSlot[i] = v;
                if (v != 0) { ++nn; }
            }
            gEngTblNonNull = nn;
        } else if (hw != 0) {
            gEngTblReadFail = 1;   // 非 0 但不像内核指针 ⇒ 拒绝读表，如实记录
        }
    }
    // ─── R1'-P3P14 只读印记捕获（`-NRedP3P14Mark`，默认关）──────────────────────────
    //  见文件头全局量处的说明（设计稿 = `docs/子任务/乙线R1-只读印记P3P14设计.md`）。
    //  **纯只读**：一处 `extendedConfigRead16`（IOKit 公开读接口）+ 四处裸内存字段读；
    //  不调 Apple 驱动虚方法、不写任何内存/寄存器/配置空间、不设 panic。
    //  ⚠️ 本块自带的读指针 helper（与 B30/EngTbl 块同一形态；**不得**依赖其它块内的同名 lambda）。
    //  ⚠️ 读数点：本块是**写入前基线**（早于 P3/P6/P7/P14 的全部写入）；复读点在
    //     `X6000FB.cpp` 的 `wrapPpHelperPowerUp`（设计稿 §1.3 的时间线）。
    if (checkKernelArgument("-NRedP3P14Mark")) {
        gP3P14Armed = 1;
        gP3P14Base  = gX5000Slide;
        ++gP3P14Calls;
        auto rd64 = [](UInt64 base, UInt64 off) -> UInt64 {
            return *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(base) + off);
        };
        gP3P14Self = s;

        // ① P14：`this+0x1a38` → AMDHardware*（三重校验后才逐级解引用）
        if (s >= 0xffffff7f80000000ULL) {
            const UInt64 hw = rd64(s, 0x1A38);
            gP3P14Hw = hw;
            if (hw >= 0xffffff7f80000000ULL) {
                // `+0x30D` = P3 通过位（`movb`，取低字节）
                gP3P14F30D = static_cast<UInt64>(
                    *reinterpret_cast<const UInt8*>(reinterpret_cast<const UInt8*>(hw) + 0x30D));
                // `+0x370` = AMDHWRegisters*（`0x72ee3 mov %rax,0x370(%r13)`）
                const UInt64 wreg = rd64(hw, 0x370);
                gP3P14HwReg = wreg;
                if (wreg >= 0xffffff7f80000000ULL) {
                    // `+0x44` = AMDHWRegisters 的内部成功位（入口先清零 ⇒ 0 是明确初值）
                    gP3P14F44 = static_cast<UInt64>(
                        *reinterpret_cast<const UInt8*>(reinterpret_cast<const UInt8*>(wreg) + 0x44));
                }
                else {
                    gP3P14F44 = 0xFFULL;   // 哨兵：未读到（与"真的是 0"区分）
                }
            }
            else {
                gP3P14F30D = 0xFFULL;      // 哨兵：AMDHardware 不存在，链早退于本函数
                gP3P14F44  = 0xFFULL;
            }
        }
        else {
            gP3P14F30D = 0xFFULL;
            gP3P14F44  = 0xFFULL;
        }

        // ② P3：经单例持有的 iGPU 读 PCI 配置空间（设备级，与该 this 无关）。
        //    `getIGPU()` 由 `NRed::processPatcher()` 从 `devInfo->videoBuiltin` 取得，与
        //    `AMDHardware::init` 的 provider 同源（同一块内置 AMD 显卡的 IOPCIDevice）。
        //    ⚠️ 指针校验通过才调用；未发起时 `gP3P14Called` 保持 0 ⇒ 判读侧不得把它当"P3 失败"。
        auto* const igpu = NRed::singleton().getIGPU();
        if (igpu != nullptr) {
            const UInt64 igpuU = reinterpret_cast<UInt64>(igpu);
            if (igpuU >= 0xffffff7f80000000ULL) {
                gP3P14Prov   = igpuU;
                gP3P14DevId  = static_cast<UInt64>(igpu->extendedConfigRead16(0x02));   // Device ID
                gP3P14VendId = static_cast<UInt64>(igpu->extendedConfigRead16(0x00));   // Vendor ID（对照）
                gP3P14Called = 1;
            }
        }

        SYSLOG("X5000",
               "R1P3P14 base: self=%llx hw=%llx f30d=%llx wreg=%llx f44=%llx prov=%llx devid=%llx vend=%llx "
               "called=%llu",
               static_cast<unsigned long long>(gP3P14Self), static_cast<unsigned long long>(gP3P14Hw),
               static_cast<unsigned long long>(gP3P14F30D), static_cast<unsigned long long>(gP3P14HwReg),
               static_cast<unsigned long long>(gP3P14F44), static_cast<unsigned long long>(gP3P14Prov),
               static_cast<unsigned long long>(gP3P14DevId), static_cast<unsigned long long>(gP3P14VendId),
               static_cast<unsigned long long>(gP3P14Called));
    }
    // ─── G3 捕获：HWServices 插件节点就绪度（`-NRedPluginNode`，默认关）────────────────
    //  规格 = `tmp/R91规格-G3探针.md`；判据与偏移来源见 `X6000FB.cpp` 静态量处的说明。
    //  **纯内存读**（+ 一次 IOKit 只读匹配）：零 MMIO、不调 Apple 虚方法、不写任何内存。
    //  每级解引用前做 `>= 0xffffff7f80000000` 校验；校验不过 ⇒ 字段置 **0xFF 哨兵**
    //   （使"真的是 0"与"没读到"在判读时可机械区分，沿用 S1/P14 的同类纪律）。
    //  ⚠️ 门控值在此（**正常上下文**）取；`wrapHandleCriticalError` 侧**不再**调
    //   `checkKernelArgument`（panic 路径禁调，既有铁律）。
    if (checkKernelArgument("-NRedPluginNode")) {
        auto probeSvc = [](const char* const cls) -> UInt64 {
            auto*  m = IOService::serviceMatching(cls);
            UInt64 r = 0;
            if (m != nullptr) {
                auto* svc = IOService::copyMatchingService(m);
                m->release();
                if (svc != nullptr) {
                    r = reinterpret_cast<UInt64>(svc);
                    svc->release();
                }
            }
            return r;
        };
        gPluginNodeSelf = s;
        const UInt64 hwsvc = probeSvc("AMDRadeonX5000_AMDRadeonHWServicesVega");
        gPluginNodeHwsvc = hwsvc;
        if (hwsvc >= 0xffffff7f80000000ULL) {
            gPluginNodeHwsvcVt = *reinterpret_cast<const UInt64*>(hwsvc);            // G3-c
            const UInt64 plugin = *reinterpret_cast<const UInt64*>(
                reinterpret_cast<const UInt8*>(hwsvc) + 0xA8);                       // G3-a
            gPluginNodePlugin = plugin;
            if (plugin >= 0xffffff7f80000000ULL) {
                gPluginNodeTtlFld  = *reinterpret_cast<const UInt64*>(
                    reinterpret_cast<const UInt8*>(plugin) + 0xB8);                  // G3-b TTL
                gPluginNodeCailFld = *reinterpret_cast<const UInt64*>(
                    reinterpret_cast<const UInt8*>(plugin) + 0xC0);                  // G3-b CAIL
                // ─── R92 增量：两接口对象的 vptr（第 91 轮判读剩余二选一的分辨题）──────
                //  判读用途：区分 (i) `cailFld` 是合法 CAIL 对象（只是不在 zone map 内）
                //    vs (ii) 它是**非指针值**（问题在 HWServices `+0x858` 返回路径）。
                //  判据：`cailVt` 落在 kext/内核镜像区且可离线定名 ⇒ (i)；否则 (ii)。
                //  ⚠️ **读前必须做指针合法性校验**（沿用铁律 6 的 `>= 0xffffff7f80000000`）：
                //    非法则置 **0xFF 哨兵且绝不 rank 解引用** ⇒ 防止访问非法内核地址。
                //    与上方 `plugin` 的校验同一形态、同一阈值。
                if (gPluginNodeTtlFld >= 0xffffff7f80000000ULL) {
                    gPluginNodeTtlVt = *reinterpret_cast<const UInt64*>(gPluginNodeTtlFld);
                }
                else {
                    gPluginNodeTtlVt = 0xFFULL;   // 哨兵：非法指针，未解引用
                }
                if (gPluginNodeCailFld >= 0xffffff7f80000000ULL) {
                    gPluginNodeCailVt = *reinterpret_cast<const UInt64*>(gPluginNodeCailFld);
                }
                else {
                    gPluginNodeCailVt = 0xFFULL;  // 哨兵：非法指针，未解引用
                }
            }
            else {
                gPluginNodeTtlFld  = 0xFFULL;   // 哨兵：插件节点未找到/非法
                gPluginNodeCailFld = 0xFFULL;
            }
        }
        else {
            gPluginNodePlugin  = 0;
            gPluginNodeTtlFld  = 0xFFULL;
            gPluginNodeCailFld = 0xFFULL;
        }
        gPluginNodeValid = 1;
        SYSLOG("X5000", "R1PluginNode cap: self=%llx hwsvc=%llx plugin=%llx ttlFld=%llx cailFld=%llx hwsvcVt=%llx",
               static_cast<unsigned long long>(gPluginNodeSelf),
               static_cast<unsigned long long>(gPluginNodeHwsvc),
               static_cast<unsigned long long>(gPluginNodePlugin),
               static_cast<unsigned long long>(gPluginNodeTtlFld),
               static_cast<unsigned long long>(gPluginNodeCailFld),
               static_cast<unsigned long long>(gPluginNodeHwsvcVt));
        // R92 增量：两接口对象的 vptr（与 panic 串**同值同源**，此行为 PP 时刻前的 L1 交叉核对；
        //   若 L1 在 configureDevice 窗口内仍有效则同轮可见，否则以 panic 串为准——第 87/88 轮纪律）。
        SYSLOG("X5000", "R1PluginNode vt: ttlFld=%llx ttlVt=%llx cailFld=%llx cailVt=%llx",
               static_cast<unsigned long long>(gPluginNodeTtlFld),
               static_cast<unsigned long long>(gPluginNodeTtlVt),
               static_cast<unsigned long long>(gPluginNodeCailFld),
               static_cast<unsigned long long>(gPluginNodeCailVt));
    }
    // ─── R1'-Diag 出口相位（`-NRedDiagProvider`，默认关）──────────────────────────
    //  x 相位 = `FunctionCast(orgConfigureDevice)` **之后**（setupCAIL 之后）：
    //   若 e 相位读到而 x 相位读不到 ⇒ 属性在 configureDevice 窗口内被**抹除**
    //   （`removeBiosFromRegistry` 嫌疑，P8 报告 U8-3）——单独作为异常信号；
    //   若 e/x 双相位都读不到 ⇒ setupCAIL 必读不到 ⇒ **P8 8-2 失败（决定性）**。
    //   判读规则与依据见函数头部注释与 `docs/子任务/乙线R1-诊断探针实现报告.md` §1.5/§2.1。
    if (checkKernelArgument("-NRedDiagProvider")) { diagProviderDump("x", provider, s, ret); }
    if (f140 >= 0xffffff7f80000000ULL) { gLastF140 = f140; }   // 供 `start` 入口恢复
    if (checkKernelArgument("-NRedAccelLog")) {
        SYSLOG("X5000",
               "cfgdev exit: ret=%llu f140=%llx f1e88=%llx f368=%llx f1f28=%llx f1f30=%llx f1a68=%llx f1a40=%llx",
               static_cast<unsigned long long>(ret), static_cast<unsigned long long>(f140),
               static_cast<unsigned long long>(f1e88), static_cast<unsigned long long>(f368),
               static_cast<unsigned long long>(f1f28), static_cast<unsigned long long>(f1f30),
               static_cast<unsigned long long>(f1a68), static_cast<unsigned long long>(f1a40));
    }
    return ret;
}

void* X5000::wrapInitLinkToPeer(void* const self, const char* const name)
{
    void* const ret = FunctionCast(wrapInitLinkToPeer, singleton().orgInitLinkToPeer)(self, name);
    if (checkKernelArgument("-NRedAccelLog")) {
        // 名称为本 kext 内的字面量（"ATIFramebuffer"/"IOFramebuffer"）⇒ 打印是安全的。
        SYSLOG("X5000", "link2peer: name=%s ret=%llx", (name != nullptr) ? name : "(null)",
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(ret)));
    }
    return ret;
}

// ─── A-25：`AMDHardware::init`（kc `0x4ba9cea`）first-false 窗口法 · 最小只读探针 ──────
//  挂点：`ret = org(...)` 后、**`ret == 0` 时**快照 `this`（实例仍存活，`0x62f0` 的 release 在其后）。
//  水印集（`this` 相对偏移）：P3 +0x2FC/0x2FE/0x300/0x302/0x30D；P5后半 +0x338/0x340；
//  P12 +0x528/0x530；P15 +0x20810；P25 +0x3B8（引擎表首槽）；加速器侧 +0x1A38/+0x1E89bit0。
//  判据：按程序序找"第一个为空的水印"（valid==1 且 value==0）⇒ first-false 窗口。
//  P5 时序二值判：同读 `self+0x50`→`node+0xD8`/`node+0xD0`。
//  纪律（技术组长红线②裁定 + 本卡硬约束）：
//   · 只读内存、零 MMIO、零写、不新增发送；
//   · 门控 `-NRedWindowProbe` 默认关 ⇒ 条件路由（本函数仅在门控真时被安装）；
//   · 通道：NRED_TRACE（A-22 内存优先，安全时点刷盘），**不调任何文件系统写**。
bool X5000::wrapAmdHwInit(void* const self, void* const provider, void* const handler,
                          UInt32* const a3, void* const gart, void* const fb)
{
    const bool ret = FunctionCast(wrapAmdHwInit, singleton().orgAmdHwInit)(self, provider, handler, a3, gart, fb);
    if (ret != false) { return ret; }   // 只在 init 成功（ret==0/false）后快照

    auto isKernelPtr = [](UInt64 p) -> bool { return p >= 0xffffff7f80000000ULL; };
    auto rd64 = [](UInt64 base, UInt64 off) -> UInt64 {
        return *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(base) + off);
    };
    auto rd8 = [](UInt64 base, UInt64 off) -> UInt64 {
        return static_cast<UInt64>(*reinterpret_cast<const UInt8*>(reinterpret_cast<const UInt8*>(base) + off));
    };
    auto rd32 = [](UInt64 base, UInt64 off) -> UInt64 {
        return static_cast<UInt64>(*reinterpret_cast<const UInt32*>(reinterpret_cast<const UInt8*>(base) + off));
    };

    const UInt64 s = reinterpret_cast<UInt64>(self);
    const bool sValid = isKernelPtr(s);
    // ── A-41：掩码与两项"加速器侧"水印的**基址＝加速器对象** ──
    //  依据 A-26 §5.1：掩码取子（kc `0x4b835d4`）返回 `*(handler+0x10) + 0x1e88`，而 `handler+0x10 ≡ accel`
    //  ⇒ 掩码在**加速器**上；`self`（`AMDHardware::init` 的 this）是 **hwInterface**，是**另一个对象**
    //  （其 `0x1e88` 区无任何写者）⇒ 旧写法 `maskBase = s` 读的是无关字节（恒 0、无信号）。
    //  基址非法 ⇒ 掩码位与两项 accel 水印一律 `valid=0`（**不得**当作"读到 0"）。
    const UInt64 h     = reinterpret_cast<UInt64>(handler);
    const UInt64 accel = isKernelPtr(h) ? nred::accelFromHandler(h) : 0;
    const bool accelValid = isKernelPtr(accel);
    uint32_t mask32 = 0;        // A-38 修 3：`accel+0x1e88` 的 4 字节条件结果位掩码真值
    UInt64 maskBaseAddr = 0;    // A-41：实际用于掩码读的基址（＝ accel；`win-probe-B` 自证行打印）
    uint32_t bit9 = 0, bit10 = 0;   // A-44："本次尝试"判据（位 9 = init 被进入、位 10 = 该次结果）
    nred::WmVal wms[nred::WmCount];
    nred::SentryVal sent[nred::SentryId::SentryCount];
    nred::P25Slots p25{};
    for (uint32_t i = 0; i < nred::SentryId::SentryCount; ++i) {
        sent[i].id    = i;
        sent[i].value = 0;
        sent[i].valid = 0;
    }
    for (uint32_t i = 0; i < nred::WmCount; ++i) {
        wms[i].id    = i;
        wms[i].value = 0;
        wms[i].valid = 0;
    }
    if (sValid) {
        const volatile uint8_t* const maskBase =
            accelValid ? reinterpret_cast<const volatile uint8_t*>(accel) : nullptr;
        maskBaseAddr = reinterpret_cast<UInt64>(maskBase);   // 自证：门控开时基址应＝ accel（非 0）

        // ── A-38 修 2：逐项"赋值即置 valid"（取消 `for(i) valid = 1` 批量置位）──
        //  批量置位会把"从未赋值"的项伪装成"已读到 0"——即 A-34 上报的 `WmAccel_1E89B0` 缺陷根因。
        auto putWm   = [](nred::WmVal& dst, const uint64_t v)     { dst.value = v; dst.valid = 1; };
        auto putSent = [](nred::SentryVal& dst, const uint64_t v) { dst.value = v; dst.valid = 1; };
        // A-41：位取样（基址＝加速器；基址非法 ⇒ 该项保持 valid=0，**不出数**）
        auto putBit  = [&](nred::SentryVal& dst, const uint32_t n) {
            uint64_t v = 0;
            if (nred::maskBitSample(maskBase, accelValid ? 1u : 0u, n, &v)) { putSent(dst, v); }
        };

        // ── A-25 水印（this 相对偏移；读法依既有 rd64/rd8 约定）──
        putWm(wms[nred::WmP3_2FC], rd8(s, 0x2FC));
        putWm(wms[nred::WmP3_2FE], rd8(s, 0x2FE));
        putWm(wms[nred::WmP3_300], rd8(s, 0x300));
        putWm(wms[nred::WmP3_302], rd8(s, 0x302));
        putWm(wms[nred::WmP3_30D], rd8(s, 0x30D));
        putWm(wms[nred::WmP5_338], rd64(s, 0x338));
        putWm(wms[nred::WmP5_340], rd64(s, 0x340));
        putWm(wms[nred::WmP12_528], rd64(s, 0x528));
        putWm(wms[nred::WmP12_530], rd64(s, 0x530));
        putWm(wms[nred::WmP15_20810], rd64(s, 0x20810));
        putWm(wms[nred::WmP25_3B8], rd64(s, 0x3B8));
        // A-41：两项"加速器侧"水印（A-26 §4 表第 6/7 项）改读**加速器**（旧写法读 `self` ⇒ 恒 0）：
        //  · `Accel+0x1A38` = 该加速器持有的 hwInterface 指针（其值应 == `self`，可作基址自证）；
        //  · `Accel+0x1E89b0` = bit8 = `createHWInterface` 成功印记（置位 ⟺ `AMDGFX9Hardware::init`
        //    返回真 ⟺ 31 条全过）。位 8 ⇔ byte `0x1e88+(8>>3)` = 0x1e89 的第 `8&7` = 0 位。
        if (accelValid) {
            putWm(wms[nred::WmAccel_1A38], rd64(accel, 0x1A38));
            putWm(wms[nred::WmAccel_1E89B0], nred::maskBitAt(maskBase, 8));
        }

        // ── A-38 修 3（A-41 改基址）：`win-probe-E` 的真实 4 字节掩码（小端逐 byte volatile 读）──
        if (accelValid) {
            mask32 = (uint32_t)maskBase[0x1E88] | ((uint32_t)maskBase[0x1E89] << 8) |
                     ((uint32_t)maskBase[0x1E8A] << 16) | ((uint32_t)maskBase[0x1E8B] << 24);
        }

        // ── A-44："本次尝试"判据（位 9 = `AMDHardware::init` 被进入、位 10 = 该次整体结果）──
        //  位 8 与 13–24 不被 `AMDHardware::free` 清零（A-35 §4.4 ③）⇒ 可能残留**更早一次尝试**的值；
        //  位 9/10/11 会被 `free` 清 ⇒ 可作"本次"的参照。基址沿用 A-41 的加速器侧。
        if (accelValid) {
            bit9  = (uint32_t)nred::maskBitAt(maskBase, 9);
            bit10 = (uint32_t)nred::maskBitAt(maskBase, 10);
        }

        // ── A-30：扩展口径 · 写侧哨兵（13 指针字段 + 位掩码统一解包）──
        //  读法：指针字段 rd64/rd32；位掩码**按位读 byte**（A-34）且**基址＝加速器**（A-41）——
        //  位 n ⇔ byte `0x1E88+(n>>3)` 的第 `n&7` 位（A-26 §5.2）。P14 的 `0x2F8` 是 **u32** 字段
        //  （写点 Z `0x73008` 为 32 位存）⇒ 用 `rd32`；按 u64 读会跨到 P3 写的 `0x2FC/0x2FE`
        //  （PCI 配置空间）⇒ **恒定非零**、判据失效（A-35 Q1）。
        // 哨兵值（指针字段直接读数；位掩码走 putBit）
        putSent(sent[nred::SentryId::P4_20630], rd64(s, 0x20630));
        // A-44：P5 的两枚哨兵——① 合成判据（A-26 §1.2 #2：`+0x338`(TTL) ∧ `+0x340`(CAIL) 均非 0）；
        //  ② 掩码忠实位 bit13（A-26 §5.2：P5 结果的双边写位）。两者同值、互为印证；插在 P4 与 P6
        //  之间以保"数组序＝程序序"（此前缺 P5 ⇒ P5 失败被误报为 P6）。
        putSent(sent[nred::SentryId::P5_Services],
                nred::p5ServicesVerdict(rd64(s, 0x338), rd64(s, 0x340)));
        putBit(sent[nred::SentryId::P5_bit13], 13);
        putSent(sent[nred::SentryId::P6_370], rd64(s, 0x370));
        putBit(sent[nred::SentryId::P7_bit14], 14);
        putBit(sent[nred::SentryId::P8_bit15], 15);
        putSent(sent[nred::SentryId::P10_3B0], rd64(s, 0x3B0));
        putSent(sent[nred::SentryId::P12_530], rd64(s, 0x530));
        putBit(sent[nred::SentryId::P13_bit18], 18);   // A-42：bit18 = `initializeTtl` 返回值的忠实位
        putSent(sent[nred::SentryId::P14_2F8], rd32(s, 0x2F8));
        putSent(sent[nred::SentryId::P16_378], rd64(s, 0x378));
        putBit(sent[nred::SentryId::P17_bit19], 19);
        putSent(sent[nred::SentryId::P18_380], rd64(s, 0x380));
        putBit(sent[nred::SentryId::P19_bit20], 20);
        putSent(sent[nred::SentryId::P21_388], rd64(s, 0x388));
        putBit(sent[nred::SentryId::P22_bit21], 21);
        putSent(sent[nred::SentryId::P23_518], rd64(s, 0x518));
        putSent(sent[nred::SentryId::P24_205F8], rd64(s, 0x205F8));
        putBit(sent[nred::SentryId::P25_bit22], 22);
        putBit(sent[nred::SentryId::P26_bit23], 23);
        putSent(sent[nred::SentryId::P27_3A0], rd64(s, 0x3A0));
        putBit(sent[nred::SentryId::P28_bit24], 24);
        putSent(sent[nred::SentryId::P29_368], rd64(s, 0x368));
        putSent(sent[nred::SentryId::P30_205C8], rd64(s, 0x205C8));
        putSent(sent[nred::SentryId::P31_205D0], rd64(s, 0x205D0));

        // ── A-34：P6 对象的忠实哨兵（`AMDHWRegisters` 对象 `+0x44` 内部成功位）──
        //  写点 Z `0x554b8` = kc `0x4b8c4b8`：`movb $0x1,0x44(%r14)`，**只在
        //  `AMDHWRegisters::init` 的成功支**执行（该函数全部失败支都跳过它，
        //  `kb/序列数据/反汇编/x5_full.asm:96251`）⇒ value==1 ⟺ P7 通过。
        //  读法纪律：须先确认 P6 指针（`this+0x370`）为内核指针（沿用阈值），否则记 valid=0
        //  （**不得**当作 0）；本项自置 valid，不经 `putSent`（A-38 修 2 同口径）。
        {
            const uint64_t p6 = sent[nred::SentryId::P6_370].value;
            if (p6 != 0 && isKernelPtr(p6)) {
                sent[nred::SentryId::P6Obj_44].value = rd8(p6, 0x44);
                sent[nred::SentryId::P6Obj_44].valid = 1;
            }
        }

        // ── A-30：P25 引擎表 11 槽（this+0x3B8..0x408）──
        p25.valid = 1;
        for (uint32_t i = 0; i < 11; ++i) {
            p25.slots[i] = rd64(s, 0x3B8 + i * 8);
        }
    }

    const nred::WindowResult ws = nred::findFirstFalseSentry(sent, nred::SentryId::SentryCount);
    const uint32_t p25verdict = nred::evalP25Slots(p25, static_cast<uint32_t>(sent[nred::SentryId::P25_bit22].value));

    // ── A-30 输出（条级判据）──
    // A-38 修 3（A-41 改基址）：`mask32` = `accel+0x1e88` 的 4 字节真实掩码（小端逐 byte volatile
    //  读；基址非法时保持 0，其"是否读到"由下一行的 `valid` 承担）。
    NRED_TRACE("win-probe-E: mask32=0x%08X", (unsigned)mask32);
    // ── A-41 自证行（新增行；不改任何既有行格式/格式串）──
    //  `maskBase` 应 == `accel`（且 ≠ `self` = hwInterface）；`valid` = 基址（加速器）是否可用。
    NRED_TRACE("win-probe-B: maskBase=0x%llX accel=0x%llX valid=%u",
               (unsigned long long)maskBaseAddr, (unsigned long long)accel, accelValid ? 1u : 0u);
    NRED_TRACE("win-probe-S: firstFalse=%u(%s) read=%u unknown=%u",
               ws.firstFalseId, nred::sentryName(ws.firstFalseId), ws.readCount, ws.unknownCount);
    // ── A-44 新增行（"本次尝试"判据）── 基址无效时两者保持 0，其有效性由 `win-probe-B` 的 valid 承担。
    NRED_TRACE("win-probe-S: bit9=%u bit10=%u", (unsigned)bit9, (unsigned)bit10);
    for (uint32_t i = 0; i < nred::SentryId::SentryCount; ++i) {
        NRED_TRACE("win-probe-S: %s=0x%llX valid=%u",
                   nred::sentryName(sent[i].id), (unsigned long long)sent[i].value, sent[i].valid);
    }
    NRED_TRACE("win-probe-P25: verdict=%u slots=[%llX %llX %llX %llX %llX %llX %llX %llX %llX %llX %llX]",
               p25verdict,
               (unsigned long long)p25.slots[0], (unsigned long long)p25.slots[1],
               (unsigned long long)p25.slots[2], (unsigned long long)p25.slots[3],
               (unsigned long long)p25.slots[4], (unsigned long long)p25.slots[5],
               (unsigned long long)p25.slots[6], (unsigned long long)p25.slots[7],
               (unsigned long long)p25.slots[8], (unsigned long long)p25.slots[9],
               (unsigned long long)p25.slots[10]);
    for (uint32_t i = 0; i < nred::WmCount; ++i) {
        NRED_TRACE("win-probe: %s=0x%llX valid=%u",
                   nred::wmName(wms[i].id), (unsigned long long)wms[i].value, wms[i].valid);
    }
    return ret;
}

// ─── A-47：P5（`initializeExternalInterfaces`）入口**只读采样** ────────────────────────
//  门控＝既有 `-NRedWindowProbe`（hook 只在门控开时 route ⇒ 门控关 ⇒ 本函数永不执行）。
//  依据（A-46）：`findHWServices`（kc `0x4baaa68`/Z `0x73a68`）以 `provider = *(this+0x10)` 调
//   `provider->vptr[0x6a8]()`（= `IOService::getClientIterator`）逐 client 判：是 `IOService`
//   ∧ `getProperty("IOMatchCategory")` 是 `OSString` ∧ `isEqualTo(this->vptr[0x710]())`；
//   期望串 = `"AMDRadeonX5000HWServices"`（kc `0x4c1b49a`）。本函数只做**同一套只读判定**并打印读数。
//  纪律（硬）：零写、零 MMIO、零新增发送；只用既有先例的只读接口（`getProperty`/`getMetaClass`/
//   `getClassName`/`getClientIterator`/`OSString::getCStringNoCopy`）＋一个**按 A-46 槽级证据**取的期望串
//   getter（`this->vptr[0x710]()`，Apple 在 `findHWServices` 内照同法调用，见 `x5_full.asm:134390-134396`）。
//  有界：client 迭代上限 `kP5ClientCap`；逐 client 明细上限 `nred::p5DetailCap()`（= 8）。
//  失败即退：任一项读不到 ⇒ 记 sentinel（`0`/`type=0`），**不重试、不再对该项调用**。
static const UInt32 kP5ClientCap = 64;   // client **计数**硬上限（防病态链表）

static void p5SampleExtIfaces(void* const selfArg, const UInt32 ret)
{
    const UInt64 s = reinterpret_cast<UInt64>(selfArg);
    if (s < 0xffffff7f80000000ULL) { return; }   // self 非法 ⇒ 静默（不产生误导性读数）

    // 只读快照：provider（`*(self+0x10)`）、findHWServices 的结果（`this+0x320`）与 P5 两产物（+0x338/+0x340）
    const UInt8* const sb = reinterpret_cast<const UInt8*>(s);
    const UInt64 prov = *reinterpret_cast<const UInt64*>(sb + 0x10);
    const UInt64 f320 = *reinterpret_cast<const UInt64*>(sb + 0x320);
    const UInt64 f338 = *reinterpret_cast<const UInt64*>(sb + 0x338);
    const UInt64 f340 = *reinterpret_cast<const UInt64*>(sb + 0x340);
    NRED_TRACE("win-probe-P5: ret=%u prov=0x%llX f320=0x%llX f338=0x%llX f340=0x%llX", (unsigned)ret,
               (unsigned long long)prov, (unsigned long long)f320, (unsigned long long)f338,
               (unsigned long long)f340);

    // ② 期望匹配类别串：`this->vptr[0x710]()`（AMDHW vtable 槽，A-46 已钉；只读、不改参数/返回值）
    const char* wantCStr = nullptr;
    UInt64      wantObj  = 0;
    if (prov >= 0xffffff7f80000000ULL) {
        const UInt64 vptr = *reinterpret_cast<const UInt64*>(sb);   // 对象首字 = vptr（纯读）
        if (vptr >= 0xffffff7f80000000ULL) {
            auto const wantFn = reinterpret_cast<void* (*)(void*)>(
                *reinterpret_cast<const UInt64*>(reinterpret_cast<const UInt8*>(vptr) + 0x710));
            if (wantFn != nullptr) {
                void* const o = wantFn(reinterpret_cast<void*>(s));
                wantObj       = reinterpret_cast<UInt64>(o);
                if (auto* const ws = OSDynamicCast(OSString, reinterpret_cast<OSMetaClassBase*>(o))) {
                    wantCStr = ws->getCStringNoCopy();
                }
            }
        }
    }
    if (wantCStr == nullptr || wantCStr[0] == '\0') { wantCStr = nred::p5MatchCategory(); }   // 回退字面值

    // ①③ 逐 client（有界）：计数 + 前 8 个出明细（元类名 / IOMatchCategory 类型与值 / 是否与期望吻合）
    UInt32 clients  = 0;
    UInt64 hitProp  = 0;
    UInt64 hitObj   = 0;
    if (prov >= 0xffffff7f80000000ULL) {
        auto* const provSvc = reinterpret_cast<IOService*>(reinterpret_cast<void*>(prov));
        OSIterator* const it = provSvc->getClientIterator();
        if (it != nullptr) {
            for (UInt32 i = 0; i < kP5ClientCap; ++i) {
                OSObject* const obj = it->getNextObject();
                if (obj == nullptr) { break; }
                ++clients;
                if (nred::p5DetailWanted(i) == 0) { continue; }   // 上限截断：只计数、不再取明细

                const char* cls = nullptr;
                if (const OSMetaClass* const mc = obj->getMetaClass()) { cls = mc->getClassName(); }
                IOService* const svc = OSDynamicCast(IOService, obj);
                OSMetaClassBase* const prop =
                    (svc != nullptr) ? svc->getProperty("IOMatchCategory") : nullptr;
                const uint32_t type =
                    nred::p5PropTypeCode((prop == nullptr) ? 1u : 0u,
                                         (prop != nullptr && OSDynamicCast(OSString, prop) != nullptr) ? 1u : 0u);
                char     iomc[41] = {0};
                if (nred::p5ShouldReadCString(type) != 0) {   // sentinel：仅 OSString 才继续取值
                    const char* const pc = static_cast<OSString*>(prop)->getCStringNoCopy();
                    nred::p5StrCopyBounded(iomc, sizeof(iomc), pc);
                    if (hitProp == 0 && nred::p5StrEq(pc, wantCStr) != 0) {
                        hitProp = reinterpret_cast<UInt64>(prop);
                        hitObj  = reinterpret_cast<UInt64>(obj);
                    }
                }
                char clsBuf[33] = {0};
                nred::p5StrCopyBounded(clsBuf, sizeof(clsBuf), cls);
                NRED_TRACE("win-probe-P5: c%u class=%s iomc=%s type=%u", (unsigned)i, clsBuf, iomc,
                           (unsigned)type);
            }
            it->release();
        }
    }
    NRED_TRACE("win-probe-P5: clients=%u cap=%u", (unsigned)clients, (unsigned)kP5ClientCap);
    NRED_TRACE("win-probe-P5: want=0x%llX got=0x%llX match=0x%llX", (unsigned long long)wantObj,
               (unsigned long long)hitProp, (hitProp != 0) ? 1ULL : 0ULL);

    // ⑤ 命中对象的 `+0xD8`／`+0xD0`（A-42/A-46：＝ `getTtl()`／`getCail()` 的返回；纯内存读）
    UInt64 d8 = 0, d0 = 0;
    if (hitObj >= 0xffffff7f80000000ULL) {
        const UInt8* const hb = reinterpret_cast<const UInt8*>(hitObj);
        d8 = *reinterpret_cast<const UInt64*>(hb + 0xD8);
        d0 = *reinterpret_cast<const UInt64*>(hb + 0xD0);
    }
    NRED_TRACE("win-probe-P5: hit=0x%llX d8=0x%llX d0=0x%llX", (unsigned long long)hitObj,
               (unsigned long long)d8, (unsigned long long)d0);

    // ④ 对照：按**类名**全注册表搜（既有 `probeSvc` 形态）。A-46 登记：与 `findHWServices`
    //   （provider client ＋属性值）**不是同一机制**，故两者并列打印、不得互相替代。
    UInt64 svcByClass = 0;
    auto*  match      = IOService::serviceMatching("AMDRadeonX5000_AMDRadeonHWServicesVega");
    if (match != nullptr) {
        auto* const svc = IOService::copyMatchingService(match);
        match->release();
        if (svc != nullptr) {
            svcByClass = reinterpret_cast<UInt64>(svc);
            svc->release();
        }
    }
    NRED_TRACE("win-probe-P5: probeSvc=0x%llX", (unsigned long long)svcByClass);
}

// A-47：`AMDHardware::initializeExternalInterfaces` 入口包装（**不改参数/返回值/控制流**：
//  返回值原样透传，采样只读）。
bool X5000::wrapInitExtIfaces(void* const self)
{
    const bool ret = FunctionCast(wrapInitExtIfaces, singleton().orgInitExtIfaces)(self);
    p5SampleExtIfaces(self, ret ? 1u : 0u);
    return ret;
}
// 第八步观测（第 14 轮）：`probe` 的结果**不在原地 panic**（第 13 轮实测：匹配阶段 panic 太早，
// panic 通道未就绪 ⇒ 零分片、不自动重启），改为写入全局静态标量，由**安全位置**
// （`AmdRadeonController::powerUp` 的 `-NRedAccelExist2` 探针）统一输出。
UInt64 gAccelProbeCalls   = 0;    // probe 被调用次数
UInt64 gAccelProbeRet     = 0;    // 最后一次返回对象指针
UInt64 gAccelProbeScoreIn = 0;    // 入口 score
UInt64 gAccelProbeScoreOut = 0;   // 出口 score（0xffffffff = *score 被置 -1 ⇒ 明确拒绝）


// ─── 第八步观测探针：加速器 `probe`（**纯观测**，定位"零实例"之因）──────────────
//  背景：第 12 轮实测加速器类**零实例**；第 13 轮证明 `probe` **确实被调用**（该轮 panic 太早、
//  零分片）。离线反汇编 `probe`（VM 0x1054）显示其拒绝路径只有 `-amd_no_dgpu_accel` 与
//  `IOPCITunnelled`（本机都不成立）⇒ 需要看清它的返回值与 score。
//  本 wrapper **不修改任何返回值/score**（不绕过匹配机制），只记录到全局标量。
//  判读（读数在 `powerUp` 处输出）：
//   · `calls = 0` ⇒ probe 从未被调用；
//   · `ret = 0 且 scoreOut = 0xffffffff` ⇒ probe 明确拒绝（`*score = -1`）；
//   · `ret = 0 且 scoreOut = 0` ⇒ 未接受也未拒绝（"not a match"）；
//   · `ret != 0` ⇒ 接受（问题在更后面）。
//  门控 `-NRedAccelProbe2`（默认关闭，仅记录、不 panic）。
IOService* X5000::wrapAccelProbe(void* const self, void* const provider, SInt32* const score)
{
    const SInt32 scoreIn = (score != nullptr) ? *score : 0x7FFFFFFF;
    auto*        ret     = FunctionCast(wrapAccelProbe, singleton().orgAccelProbe)(self, provider, score);
    const SInt32 scoreOut = (score != nullptr) ? *score : 0x7FFFFFFF;

    gAccelProbeCalls += 1;
    gAccelProbeRet = reinterpret_cast<UInt64>(ret);
    gAccelProbeScoreIn = static_cast<UInt64>(static_cast<UInt32>(scoreIn));
    gAccelProbeScoreOut = static_cast<UInt64>(static_cast<UInt32>(scoreOut));
    gAccelProbeProv = reinterpret_cast<UInt64>(provider);

    return ret;
}
