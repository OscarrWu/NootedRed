// AMDRadeonX5000HWLibs Patches
//
// Copyright © 2022-2025 ChefKiss. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#include "GoldenSettings.hpp"
#include <NvMsgBuf.hpp>     // NvMsgBuf::dumpNow()：在关键探针处立即落一拍（见该文件头注释）
#include <ASICCaps.hpp>
#include <GPUDriversAMD/CAIL/ASICCaps.hpp>
#include <GPUDriversAMD/CAIL/DevCaps.hpp>
#include <GPUDriversAMD/CAIL/DeviceType.hpp>
#include <GPUDriversAMD/CAIL/Result.hpp>
#include <GPUDriversAMD/Family.hpp>
#include <GPUDriversAMD/PSP.hpp>
#include <GPUDriversAMD/RavenIPOffset.hpp>
#include <GPUDriversAMD/RenoirPPSMC.hpp>
#include <GPUDriversAMD/PhoenixPPSMC.hpp>
#include <GPUDriversAMD/TTL/Event.hpp>
#include <GPUDriversAMD/TTL/SWIP/DMCU.hpp>
#include <GPUDriversAMD/TTL/SWIP/GC.hpp>
#include <GPUDriversAMD/TTL/SWIP/IPVersion.hpp>
#include <GPUDriversAMD/TTL/SWIP/SDMA.hpp>
#include <GPUDriversAMD/TTL/SWIP/SMU.hpp>
#include <HWLibs.hpp>
#include <FwBringup/NRedFwBringupHook.hpp>   // A-1 固件层第一增量：薄封装（C2 挂点）
#include <FwBringup/NRedSegReadout.hpp>   // D-3 只读仪表（SEG0/SEG1 双段读数、C2PMSG_91/83 直读、PB 状态位）
#include <HWLibsSmuGate.hpp>
#include <Headers/kern_mach.hpp>
#include <Headers/kern_patcher.hpp>
#include <libkern/OSAtomic.h>   // A-28：OSCompareAndSwap 原子互斥（flush 重入保护）
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <Kexts.hpp>
#include <NRed.hpp>
#include <PenguinWizardry/KernelVersion.hpp>
#include <PenguinWizardry/PatcherPlus.hpp>
#include <Regs/SDMA0.hpp>
#include <Regs/SMU.hpp>
#include <Regs/VBIOSSMC.hpp>
#include <pexpert/pexpert.h>   // B70：PE_boot_args()（与 checkKernelArgument 同源 PE_state+8，逐字读生效 boot-args）
#include <Headers/kern_file.hpp>   // FileIO::writeBufferToFile（内核态落盘，见下方 IP 探针）
#include <sys/vnode.h>          // B77①：vnode_open/vnode_setattr/vnode_close（落盘前显式截断）
#include <sys/vnode_if.h>
#include <NRedTraceSafe.hpp>       // A-20：早期落盘安全前置检查（纯逻辑）
#include <NRedTraceRing.hpp>       // A-22：早期 trace 内存环形缓冲（纯逻辑静态存储）
#include <FwBringup/NRedClkLayoutReadout.hpp>   // A-27：（甲）线读数（C2 出口延迟读）

// 根 vnode（XNU `bsd/sys/vnode.h`）：**内核态写文件的必备判据**，非空才表示根 FS 已挂载。
// 与 `NvMsgBuf.hpp` 同一写法（`extern "C"` + weak），此处单独声明以免把它的静态缓冲带进来。
extern "C" void *rootvnode __attribute__((weak));
// A-27（甲）线：由 `X6000FB.cpp`（`wrapDcClkMgrCreate`）存入，本文件 C2 出口**延迟读**。
extern UInt64 gClkMgrPtr;
extern UInt64 gClkMgrCreateCalls;
extern UInt64 gPpSmuPtr;
extern UInt64 gDcCtxPtr;
// A-6 回调地址与调用计数（X6000FB.cpp 存；本处仅读）
extern UInt64 gNRedPpSmuOverlayCbAddr;
extern UInt64 gNRedPpSmuOverlayCalls;
// A-20：落盘安全计数器（文件作用域；早期路径调用 nredTraceLine 时未就绪 ⇒ 丢弃计数累加）
nred::TraceSafe gTraceSafe;
// A-22：早期 trace 内存环形缓冲（静态，无分配；早期路径只 push，不写文件）
nred::TraceRing gTraceRing;
// B91②：最近一次 type=5（`wrapMode9440f` 的 b==5）时的 t30（观测留存：flush-stamp 行使用）。
//  初值 0xFFFFFFFFFFFFFFFFULL＝"尚未发生过 type=5"哨兵（区别于真 0）；仅 R1 块写、flush 读。
UInt64 gLastT30Type5 = 0xFFFFFFFFFFFFFFFFULL;
// B93②：type∈{3,8} 的最近 t30（与 type5 同款哨兵 0xFFFFFFFFFFFFFFFFULL＝未见到该 type）。
UInt64 gLastT30Type3 = 0xFFFFFFFFFFFFFFFFULL;
UInt64 gLastT30Type8 = 0xFFFFFFFFFFFFFFFFULL;
// B94①：最近一次 b==5 时的阶段号 c（sub5；哨兵 0xFFFFFFFF＝未发生过 b==5）——判"(5,0)"是否真被调过。
UInt32 gLastSub5 = 0xFFFFFFFFU;
// P2 包A：固件流程状态（定义在 FwBringup/NRedFwBringupHook.hpp；本处只读，供 [bringup] 头部戳记）。
extern UInt32 gBringupEnterCount;
extern UInt32 gBringupStep;
extern SInt32 gBringupLastError;
extern UInt32 gBringupLoadToc;
extern UInt32 gBringupTmrLoad;
// P7c 包D：allocB 末两道判据与 0x9440f 的返回值（供 [ttlchain] 头部戳记读取）。
//  哨兵语义：gP3/gP4＝-1（未进入过该判据）；gP5＝0xFFFFFFFFFFFFFFFF（0x9440f 未被调用）。
//  仅由相应 wrapper 写、flush 读；常量初始化（无 guard variable）。
static SInt32  gP3 = -1;   // 判据③ `0x9405e`（IpiInitializeIpInterfaces）的返回值
static SInt32  gP4 = -1;   // 判据④ `0x929ac`（TlsExecuteIpEntrySeq）的返回值
static UInt64  gP5 = 0xFFFFFFFFFFFFFFFFULL;   // 0x9440f：(type<<32) | ret（既有 wrapMode9440f 内写）
// P7d 包D″：真 `m_hTtl`＝加速器侧门面对象 `+0x38`（由 X5000.cpp 的既有 `ttlC:` 块顺带存此；
//  与 `ttlC: mHttl=` 同一个值）。哨兵 0xFFFFFFFFFFFFFFFF＝尚未采到。取代原先误用的 gLastT30Type5。
UInt64 gMHttl = 0xFFFFFFFFFFFFFFFFULL;
// P7f 包E″：判别读数 v4（**无条件记录** `0x929ac` 前 3 次调用；R6 证明"按假设过滤"全部落空）。
//  ★三槽不覆盖：gSeq1/2/3 按到达顺序记 (RA, sub, al)；gIpSeqN 为调用总计数（第 4 次起只计数）。
//  RA＝`__builtin_return_address(0)` 减 kcSlide ⇒ 归零 VM（若 kcSlide==0 记原始 RA，可判读）。
//  ⚠️ 限流硬约束：本函数高频 ⇒ 只写标量（不入环、不打印）⇒ 判别行不会被挤出 4096 B 片。
static SInt32  gABRet  = -1;    // `0x928f4`(allocB) 最近一次返回值（由既有 wrapTtlAllocB 写入）
static UInt32  gIpSeqN = 0;     // `0x929ac` 调用总计数
static UInt64  gSeq1RA = 0;     static UInt32 gSeq1Sub = 0xFFFFFFFFU; static UInt32 gSeq1Al = 0xFFFFFFFFU;
static UInt64  gSeq2RA = 0;     static UInt32 gSeq2Sub = 0xFFFFFFFFU; static UInt32 gSeq2Al = 0xFFFFFFFFU;
static UInt64  gSeq3RA = 0;     static UInt32 gSeq3Sub = 0xFFFFFFFFU; static UInt32 gSeq3Al = 0xFFFFFFFFU;
// B95v2（2026-10-18，换落点到 `0xa2691`）：基础启动序列补入的**执行确认通道**标量。
//  只写标量（不入环、不打印）⇒ 由 `nredTraceFlush` 的头部区汇总成一行 `[nredInsert]`。
//  ★ O-1（R12 教训）：本行**无条件化**——判据只看"门控是否开"，**不依赖封装是否被调用**。
//    R12 之所以无法区分"未注册／未执行"，正是因为当时用了 `applied||skip` 这种"跑过才打"的哨兵。
//  ⇒ 门控开 ⇒ 每片必有一行（哪怕 calls=0）；门控关 ⇒ 一字不变。
static UInt32 gInsCalls    = 0;             // 本封装被调用总次数（含 Apple 自身命中的）
static UInt32 gInsAppleHit = 0;             // 其中"Apple 自己命中（返回非 0）"的次数 ⇒ 我方逐字放行
static UInt32 gInsOurs     = 0;             // 其中"由我方接管（返回我方记录）"的次数
static UInt32 gInsSkip     = 0;             // skip 原因码（**只记首次**，免被后续调用覆盖）
static UInt32 gInsK1       = 0;             // 最近一次传入的键1（预期 0x15BF）
static UInt32 gInsK2       = 0;             // 最近一次传入的键2（预期 9）
static UInt32 gInsK3       = 0;             // 最近一次传入的键3（预期 0xC7；我方记录在此位取通配）
enum : UInt32 {
    kInsSkipNone = 0,   // 无 skip
    kInsSkipDev  = 1,   // 键不匹配我方设备（k1≠0x15BF 或 k2≠9）⇒ 不接管
};
static const char* const kInsSkipReason[] = {"none", "dev-mismatch"};
// 227/228：`[ttltrim]` 头部戳记数据（仅由 `nredTrimStage0`/`trimRunStage2` 写、`nredTraceFlush` 头部区读）。
//  ⚠ 环正文区受 4096 B 硬上限"保头丢尾"⇒ 判别行必须落头部区（同 `[ttlchain]`）。
//  ⚠ 关态：两个 seen 恒 0 ⇒ 不输出本行，未命中形态输出逐字不变。
struct NredTrimStat {
    UInt32 seen, n, skip, last, r12, bad;
    SInt32 ret;
    UInt32 failMask, firstFail, nFail;   // 240：失败 client 识别（bit=client；firstFail 哨兵 0xFFFFFFFF）
};
static NredTrimStat gTrimStat[2] = {};   // [0]=stage0、[1]=stage2（各自最后一次裁剪读数）
static void nredTrimResetFail(const UInt32 stage)
{
    NredTrimStat& d = gTrimStat[(stage == 2) ? 1 : 0];
    d.failMask = 0;
    d.firstFail = 0xFFFFFFFFU;
    d.nFail = 0;
}
static void nredTrimRecord(const UInt32 stage, const UInt32 n, const UInt32 skip, const UInt32 last,
                           const UInt32 r12, const UInt32 bad)
{
    NredTrimStat& d = gTrimStat[(stage == 2) ? 1 : 0];
    d.seen = 1; d.n = n; d.skip = skip; d.last = last; d.r12 = r12; d.bad = bad;
    d.ret = static_cast<SInt32>(r12 & 1);
}
// B95v2 门控标量（**默认 false**＝关）：由 `processKext` 解析 `-NRedDevTablePatch` 后置位。
//  ⚠️ 刻意用**文件作用域**而非类成员：`nredTraceFlush` 是**自由函数**（拿不到私有的
//  `X5000HWLibs::singleton()`），而执行确认戳记（O-1）必须由它输出 ⇒ 单点状态、无重复。
//  判定处只读本标量（遵守"探针里不调 checkKernelArgument"的既有纪律）。
static bool gDevTablePatch = false;

// ── H1 观测集（2026-10-18）：`-NRedTtlBisect`（**默认关**）的四组只读读数 ──────────────
//  目的：R13 已证"记录被采纳并使用"，但链仍断在 ③`0x9405e` 或 ④`0x929ac` 之一，
//  离线无法再二分 ⇒ 用一轮只读观测把断点定死（规格：`tmp/tech-manager/H1时序假设与观测轮规格-2026-10-18.md` §2）。
//  ★ 本卡**零行为**：不改任何返回值/控制流/ABI；**不新增任何 wrapper**；全部读数只写 `g*` 标量
//    （唯一例外＝N-3 的守卫式只读解引用，与既有 `tls-swinit` 同类）。
//  ⚠ 关态纪律：零标量写入、零戳记行、零新增解引用 ⇒ 与现状逐字一致。
static bool gTtlBisect = false;
// 223/228/229（`-NRedTtlTrim`，**默认关**）：判据④ stage0（正向）/stage2（反向）按本机真实
//  client 集合裁剪：stage0 跳过 15/10；stage2 跳过 15/10/3（3＝BGM）。其余 stage 与关态一律透传。
static bool gTtlTrim = false;
// 230（`-NRedBgmTab`，**默认关**）：BGM 表区转储。关态 ⇒ 不安装 hook ⇒ 零输出、零行为。
static bool gBgmTab = false;
// 235（`-NRedBgm64`，**默认关**）：BGM 64K 填充链取样（P1–P4）。
//  与 `-NRedBgmTab` 独立：P1 复用该 wrapper，P3/P4 复用既有 cfg-read/mode2-tail 包装（已无条件安装，
//  不能再加同地址路由），P2 为新增的 gated 路由。
static bool   gBgm64 = false;
static UInt32 gBgm64N[5] = {0};          // 每点 dump 次数（索引＝pt）
static bool   gBgm64Once1 = false;       // P1 只采一次（64KB 初次读）
static bool   gBgm64Once2 = false;       // P2 只采一次（64KB memcpy）
static bool   gBgm64Once4 = false;       // P4 只采一次（唯一调用点）
static UInt32 gBgm64LoopN = 0;           // P3 调用计数（热路径限流）
// 238（`-NRedBgmReal`，默认关）：用本机真实 Phoenix 容器接管 `0x29b4e2` 的 64KB 读。
// 数据来源：b21 转储（md5 7100b6b18a9b99c8ccb219f3a90ae56a）经 `--fix-mode drop` 修正：
//   仅删除 4 条 nbase 越界条目（num_ips 39→35），其余逐字节来自本机；
//   成品 md5 = 10b766afba00fde16c3b263e423ddc7c，长度 65536 B。
// ⚠ 必须放**可写**段（非 const）：防苹果侧任何写入落到只读段引发 fault。
static UInt8 gBgmRealContainer[65536] = {
#embed "Firmware/bgm_phoenix_fixed.bin"
};
static_assert(sizeof(gBgmRealContainer) == 65536, "BGM real container size must be 65536");
static const UInt64 kBgmRealFnv1a64 = 0x2A8B614310F42766ULL;   // FNV-1a64（离线对成品文件计算）
static bool   gBgmReal = false;
static SInt32 gBgmRealCheck = -1;        // -1 未校验、0 fail、1 ok
// 241（`-NRedBgmHw`，默认关）：BGM `EVENT__HW_INIT` 上报点观测（0x91222 TrimHook）。
//  限流：每个 (stage,mode) 只打首 2 条，总计 ≤8；表为 BSS 静态，wrapper 零局部数组。
static bool   gBgmHw = false;
static UInt8  gBgmHwCnt[9][5] = {};
static UInt32 gBgmHwTotal = 0;
// 242（`-NRedSt1Pre`，默认关）：stage1 预检观测（护栏 return 0 之前、只读一级指针）。
static bool gSt1Pre = false;
// 243：预检结果写入 BSS 标量，由 `nredTraceFlush` 头部区只读输出（正文区会系统性丢失）。
static UInt32 gSt1Mask  = 0;   // bit i = client i 一级对象存在且指针合法（i∈{2,5,6,7,8,9,14}）
static UInt32 gSt1N     = 0;   // 预检调用次数（饱和 255）
static UInt32 gSt1CtxOk = 0;   // ctx 合法 = 1
// 244（`-NRedStage1`，默认关）：stage1 首批放行（仅 client 14）及观测标量。
static bool   gStage1   = false;
static UInt32 gSt1Try   = 0;   // 放行尝试过的 client 位图
static UInt32 gSt1Ret0  = 0;   // 返回 0 的 client 位图（前置失败或叶返回 0）
static UInt32 gSt1RunN  = 0;   // 放行尝试次数（饱和 255）
// 245：`[st1v]` 补充观测（值在 guard 内读出，头部区零解引用）。
//  §3 约定：`mes=0`（③ 失败）时 m8/m790 未读出 ⇒ 保持 0，不得据此判 G4/G5 失败。
static UInt64 gSt1Mes  = 0;   // ③ 读出的 MES 对象地址
static UInt64 gSt1G14P = 0;   // 264：client14 分支读到的 P14（供 `[st1g14]`）
static UInt64 gSt1M8   = 0;   // ④ *(MES+0x08)
static UInt64 gSt1M790 = 0;   // ⑤ *(MES+0x790)
// 246（`-NRedSt1W`，默认关）：client 7 只读探针（不放行、不调原件）；值由探针读出，头戳零解引用。
static bool   gSt1W    = false;
static UInt64 gSt1P7   = 0;   // *(ctx+0x40)
static UInt64 gSt1P3   = 0;   // 255：*(ctx+0x20)（client 7 stage1 唯一硬 fault 依赖）
static UInt64 gSt1B3P3  = 0;  // 258：client 3 分支读到的 P3（供 `[st1b3]` 头部行）
// 259：剩余 client 放行的观测值（P 与关键二级指针；头部行零解引用）。
static UInt64 gSt1G2P = 0, gSt1G2Key = 0;
static UInt64 gSt1G5P = 0, gSt1G5Ip = 0, gSt1G5Ipr = 0;
static UInt64 gSt1G5P20 = 0, gSt1G5P21 = 0, gSt1G5P22 = 0, gSt1G5P23 = 0;   // 步20-23 四级指针
// ─── 批次 P：client5 护栏的中间量与判定结果（只写 BSS，由 [st1g5x/y/z] 输出）──────────────
static UInt64 gSt1G5H     = 0;   // 步 10：H = *(P5+0x28)（供步 27 读 +0xE88/0xE90/0xE98）
static UInt64 gSt1G5Root  = 0;   // 步 14：root = *(h8)
static UInt64 gSt1G5Head  = 0;   // 步 15（观测）：head = *(dev+0xE8)
static UInt64 gSt1G5Blk   = 0;   // 步 17（观测）：blk = *(head+0x40)
static UInt64 gSt1G5Arr   = 0;   // 步 18（观测）：arr = *(blk+8)
static UInt64 gSt1G5F70   = 0;   // 步 24：*(slot1+0x70)
static UInt64 gSt1G5F268  = 0;   // 步 24：*(slot1+0x268)
static UInt64 gSt1G5F2B8  = 0;   // 步 24：*(slot1+0x2b8)
static UInt64 gSt1G5E88   = 0;   // 步 27：*(H+0xE88)
static UInt64 gSt1G5E90   = 0;   // 步 27：*(H+0xE90)
static UInt64 gSt1G5E98   = 0;   // 步 27：*(H+0xE98)
static UInt32 gSt1G5Verdict = 0xFF;  // 0＝护栏判失败；1＝护栏全过但被 P-7 拦住；0xFF＝未跑
// ─── 批次 Q′：0x1936e 截断器的读数（只写 BSS，由 [qp]/[qp2] 输出）──────────────────────────
static bool   gQp       = false;   // `-NRedQp`（默认关）
static bool   gQpArmed  = false;   // hook 是否**真的**注册成功（0 ⇒ 即使 -NRedQp 开也不放行叶子）
static UInt32 gQpN      = 0;       // 被截断次数
static UInt64 gQpRa     = 0;       // 调用者返回地址（减 slide）
static UInt64 gQpObj    = 0;       // 第 1 实参（＝p1[5]）
static UInt64 gQpClk    = 0;       // 第 2 实参（＝叶子 clk 缓冲，RBP-0xc8）
static UInt64 gQpDesc   = 0;       // 第 3 实参（＝叶子 desc 缓冲，RBP-0x2a0）
static UInt32 gQpC4     = 0;       // clk+0x04：能力位（叶子第 2–13 步的输出）
static UInt32 gQpT20    = 0;       // clk+0x20：type[2].lo（0xa28ad 输出）
static UInt64 gQpT28    = 0;       // clk+0x28：type[1]（0xa28ad 输出）
static UInt64 gQpA30    = 0;       // clk+0x30：0xa2811(h8)
static UInt64 gQpA38    = 0;       // clk+0x38：0xa2847(h8)
static UInt32 gQpA50    = 0;       // clk+0x50：0x90da8()（恒 0x1000）
static UInt64 gQpA58    = 0;       // clk+0x58：0x94ff7(ctx) + 0xa2b51(h8)
static UInt32 gQpL68    = 0;       // local_68（ZFB mode；由 clk+0x68 派生）
static UInt8  gQpL68Ok  = 0;       // 派生自证是否成立（0＝不读，1＝已读）
// （265 的开关/ 观测变量见下方 gSt1Q 组）
// 268（`-NRedQqProbe` / `-NRedQqHdl`，默认关）：client5 的 qq 句柄层（B1）。
//  qq 实测是 0x8b969 构造的 0x228 表（q10==qq），而 0x8b4bf 期望 8B 句柄（*h=表）
//  ⇒ 句柄层缺失导致步22。探针只读；安装只在 7 条判据全满足时改 *(p) 一处。
static bool   gQqProbe = false;
static bool   gQqProbeSeen = false;
static bool   gQqHdl = false;
static bool   gSt1G5Step24 = false;   // 步24 守卫已接入（安装判据 [9]）
alignas(8) static UInt8 gQqHdlStore[8];   // 8B 句柄（static：无分配器；8B 对齐，避免未对齐写）
                                          // 批次 P 起不再写入（方案 C 写入分支已撤回）⇒ 内容恒 0；
                                          // 注意 `[qqh] hstore=` 打印的是本数组的**地址**（非内容），故该字段恒非 0
static UInt64 gQqHdlOld = 0;
static UInt32 gQqHdlRet = 0xFF;
static UInt64 gQqP = 0, gQqQ = 0, gQqQ8 = 0, gQqS1 = 0;
static UInt64 gQqV70 = 0, gQqV258 = 0, gQqV268 = 0, gQqV2B8 = 0;
static bool   gSt1Q = false;
static bool   gSt1QQSeen = false;
static UInt64 gSt1QQ0 = 0, gSt1QQ8 = 0, gSt1QQ10 = 0, gSt1QQ18 = 0;
static UInt32 gSt1QQF = 1;   // 1=尚未执行/任一级失败；0=读取成功
static UInt64 gSt1G6P = 0, gSt1G6Key = 0;
static UInt64 gSt1G9P = 0, gSt1G9Key = 0;
// ─── 批次 P4：client 8（SMU）观测护栏（`-NRedSt1G8`，默认关）──────────────────────────────
static bool   gSt1G8        = false;  // `-NRedSt1G8`（默认关）
static UInt32 gSt1G8Verdict = 0xFF;   // 0＝判据未全过；1＝全过但被观测态拦住；0xFF＝未跑
static UInt64 gSt1G8P   = 0;          // P8 = *(ctx+0x48)
static UInt64 gSt1G8F18 = 0;          // *(P8+0x18)（静态回调 0x6463f）
static UInt64 gSt1G8F28 = 0;          // *(P8+0x28)（SMU ctx）
static UInt64 gSt1G8V6C8 = 0;         // *(SMU+0x6c8)（观测）
static UInt64 gSt1G8V6D8 = 0;         // *(SMU+0x6d8)（观测）
// 260（`-NRedStage1`）：守卫首个失败步号（仅供判读；不影响守卫返回）。
static UInt32 gSt1GfClient = 0xFFFFFFFFU;   // 0xFFFFFFFF＝尚无失败
static UInt32 gSt1GfStep = 0;
// 266（`-NRedTtlFacade`，默认关）：TTL 门面（root 552B 槽表 + slot1/2/3/4）。
//  结构依据：R46 §2（root 槽表 552B；slotN 位于 root+slot*24，项 = {u32 slot, u64 obj, u64 val}；
//  0x8bbc4(root,slot) 返回项的 val；root 初始化把 slot0 的 val 写为 root 自身）。
//  ⚠ 除 slot0 外，各 slot 的 obj/val 语义与 slot1+600/616 等字段仍未穷举（R46/R205 标“未解出”）
//    ⇒ 本实现只填写已被反汇编证实的部分，其余保持 0；默认关，且安装判据极严。
static bool   gTtlFacade = false;
static bool   gTtlFacadeInit = false;
static UInt32 gTtlFacadeInst = 0;   // 0=未安装 1=已安装 2=判据未满足
static UInt8  gTtlFacadeRoot[552];
static UInt8  gTtlFacadeSlot1[800];
static UInt8  gTtlFacadeSlot2[8];
static UInt8  gTtlFacadeSlot3[680];
static UInt8  gTtlFacadeSlot4[1992];
// 261（`-NRedGvm5`，默认关）：client5 GVM provider type 与注册表状态（只读一次）。
static bool   gGvm5 = false;
static bool   gGvm5Seen = false;
static UInt32 gGvm5Type = 0;
static UInt64 gGvm5A0 = 0;      // *(u64*)A（A 的首字段；用于确认 IP 根层级）
static UInt64 gGvm5A58 = 0;     // *(A+0x58)（provider 克隆继承的槽）
static UInt64 gGvm5Iproot = 0;  // *(IP+0x58)（＝0x8b8e7(A)）
static UInt32 gGvm5Reg = 2;   // 0=未命中 1=命中 2=无法判定
// 273（`-NRedGvm5D`，默认关）：client5 stage1 叶（`0x985d0`）的我方完整复刻。
//  ⚠ 描述块必须放 BSS（`0x1c8`+`0x68`+24+24+16 ≈ 624 B，远超 128 B 栈纪律）；
//    调用点串行（`wrapMode9440f` 的 client5/stage1 路径），无并发写者。
static bool   gGvm5D = false;
static UInt32 gGvm5DRet = 0xFFFFFFFFU;   // 最近一次叶返回值（0xFFFFFFFF=未跑）
static UInt64 gGvm5DP1 = 0;              // 最近一次 p1 = *(a+0x30)
static UInt32 gGvm5DLocal68 = 0;         // 最近一次 local_68 低 32 位（类型）
static UInt32 gGvm5DStep = 0;            // 最后执行的步骤号（失败定位；0=未进入）
static UInt32 gGvm5DBusy = 0;            // A2：描述块非阻塞重入守卫（OSCompareAndSwap）
static bool   gGvm5DBusyHit = false;     // A2：是否发生过重入（观测 `busy=1`）
// 274（`-NRedGvm5Phase`，默认关）：client5 stage1 入口只读探针（回答 D1：start 是否走到 client5）。
//  ⚠ 能力边界：只能回答 D1；D2（槽 2/6 失败归属）需 powerUp hook，D3（self+0x100 等语义）
//    需 `AmdPowerPlayHelper::self`——我方在 `wrapMode9440f` 只有 `ctx`，两者均拿不到。
static bool   gGvm5Phase = false;
static UInt32 gGvm5PhaseN = 0;           // 第几次（1-based）
static UInt64 gGvm5PhaseA = 0;           // 入参 a
static UInt64 gGvm5PhaseP1 = 0;          // *(a+0x30)（KPtr 后）
static UInt64 gGvm5PhaseRa = 0;          // 返回地址归零（ra - gHwlibsSlide）
static UInt32 gGvm5PhaseRc = 0xFF;       // 探针不执行 D ⇒ 恒 0xFF（未执行）
// 276（复用 `-NRedGvm5Phase`）：相位只读标记——把 `[gvm5p]` 的调用归属到 start / powerUp。
//  start  = `X5000::wrapInitializeTtl` 入口；powerup = `X6000FB::wrapControllerPowerUp` 入口。
static UInt32 gGvm5PhStart = 0;          // 1=start 相位到过
static UInt32 gGvm5PhPower = 0;          // 1=powerup 相位到过
// 275（`-NRedGvm5Early`，默认关；依赖 `-NRedGvm5D`）：M4′ 早期注入（start/kernel_task 执行 D）。
//  幂等：只有**早期真实成功**才置 `gGvm5EarlyOk=1`，powerUp 阶段据此返回缓存的真实 rc。
static bool   gGvm5Early = false;
static UInt32 gGvm5EarlyOk = 0;          // 0=未执行/失败；1=早期真实成功
static UInt32 gGvm5EarlyRc = 0;          // 早期真实成功时的返回值（原样缓存）
static SInt32 gGvm5EarlyStartRc = -1;    // start 阶段 rc（-1=未跑）
static SInt32 gGvm5EarlyPowerRc = -1;    // powerup 阶段 rc（-1=未跑）
// R3a：幂等跳过前必须比较 p1（两次调用的 ctx/p1 可能不是同一对象）。
static UInt64 gGvm5EarlyP1 = 0;          // 早期成功时的 p1 = *(ctx+0x30)
static bool   gGvm5EarlyPowerSeen = false;  // powerUp 阶段是否已到过 b==5&&c==1
static UInt32 gGvm5EarlyPowerIdem = 0;   // 1=p1 相同已跳过；0=p1 不同/非法未跳过
static UInt64 gGvm5EarlyPowerP1 = 0;     // powerUp 阶段的 p1now（idem=0 时观测）
static UInt32 gGvm5EarlyPowerPanic = 0;  // 1=late path D 失败已 panic（观测 `panic=1`）
// 277：late path panic 文本缓冲（BSS，≤200 B；panic 前用 snprintf 填入读数）。
static char   gGvm5PanicBuf[200];
struct NRedGvm5DBlocks {
    UInt8 desc[0x1c8];   // local_2a8：描述块（首 u32=0x1c8；+0xf0 起 0xd8 字节为待拷贝视图）
    UInt8 clk[0x68];     // local_d0：时钟/能力位块（首 u32=0x68；+4 为位域）
    UInt8 type[24];      // local_68：0xa28ad 输出（[0]=类型，[1]/[2] 供 clk 使用）
    UInt8 hub[24];       // local_40：0xa3d6c 输出 / p1[0xf] 输入输出
    UInt8 save[16];      // local_216：GART save context 输出
};
alignas(8) static NRedGvm5DBlocks gGvm5DBlocks;
// 262（`-NRedGSlot`，默认关）：client5 golden 6 槽（type/实例数/base[0..2]）只读快照。
struct NRedGSlotRec { UInt32 type, n, b0, b1, b2; };
static bool         gGSlot = false;
static bool         gGSlotSeen = false;
static NRedGSlotRec gGSlotRec[6];
static UInt64       gGSlotH = 0, gGSlotT = 0, gGSlotA = 0;
static UInt32       gGSlotGolden = 0;   // 批次 P2：golden armed 位掩码（bit0=70／bit1=27／bit2=34／bit3=28）
// 263／批次 P2（`-NRedIpR`，默认关）：client5 的 IP 根**只读**探针（不写 MMIO、不调苹果函数）。
//  ⚠ 批次 P2／P2-1：原"静态假根"（gIpRoot/gIpProvider/gIpArr/gIpHead/gIpBlk/gIpRInit）已整块删除。
static bool          gIpR = false;
static UInt64        gIpRIp = 0;
static UInt32        gIpRRet = 0xFF;   // 批次 P2：0=只读探针成功；1..6=链上第 1..6 级非法；7=root 非法；8=iproot 非法；0xFF=未执行
static UInt64        gIpRRoot = 0;     // 批次 P2：真实 iproot ＝ *(*(IP)+0x58)（原为 gIpRoot 我方静态）
static UInt64 gSt1H    = 0;   // *(P7+0x28)（GC handle）
static UInt64 gSt1Svc  = 0;   // *(handle+8)（运行期服务对象）
static UInt64 gSt1S[9] = {0}; // svc+0x10/0x18/0x20/0x38/0x40/0x90/0x100/0x118/0x120
static UInt32 gSt1SkipHigh = 0;   // 247：pageOff>0xED8 ⇒ 1（高位槽因越页被跳过）
static bool   gSt1W2   = false;  // 248：槽值探针（deref svc；默认关，与 gSt1W 独立）
// 249（`-NRedSt1O`，默认关）：对齐观测——每 client「首次」与「p 变化」各一条，总 ≤14 条。
struct NRedSt1ORec { UInt64 ctx; UInt64 p; UInt32 seq; UInt32 used; };
static bool        gSt1O = false;
static NRedSt1ORec gSt1ORec[7][2];   // 7 个 client × 至多 2 条
static UInt8       gSt1OSeq[7];      // 逐 client 调用序号（饱和 255）
// 251（`-NRedGc7`，默认关）：client 7 双侧观测（安装侧 + 注销侧）。
struct NRedGc7Rec { UInt64 p7; UInt32 sel; UInt32 hit; };
static bool       gGc7 = false;
static NRedGc7Rec gGc7Rec[2];
static UInt32     gGc7N = 0;
static bool       gGc7HitSeen = false;
static UInt32     gGc7xSlot = 0;
static UInt64     gGc7xVal = 0;
static UInt32     gGc7xSeen = 0;
// 252（`-NRedSt1B`，默认关）：`[st1b]` 原始 `(b,c)` 调次（前 8 次，不论取值）。
struct NRedSt1BRec { UInt32 b; UInt32 c; UInt32 seq; };
// 270（`-NRedP7Hold`，默认关）：client7 一级对象 P7 的窗口化保活。
//  窗口＝安装命中（0x93d18 sel==7 / 0x93f64 id==7 位为 1）→ client7 stage1 返回；
//  窗口内拦截 0x93998 对 id==7 的写 0；窗口外一律透传（保留苹果注销语义）。
static bool   gP7Hold = false;
static bool   gP7Armed = false;
static UInt32 gP7Skip = 0;
static UInt32 gP7Cleared = 0;
// 270：stage1 返回即关窗（RAII：覆盖 wrapMode9440f 的所有 return 路径）。
struct NredP7Window {
    const bool active;
    explicit NredP7Window(const bool a) : active(a) {}
    ~NredP7Window() { if (active) { gP7Armed = false; } }
};
static UInt64 gP7Seen = 0;
// 271：`wrapIpEntrySeq` 的 FunctionCast 结果为 0（org 未解出/被清）时置位，头部区打一行。
static UInt32 gIpSeqCast0 = 0;
static bool       gSt1B = false;
static NRedSt1BRec gSt1BRec[8];
static UInt32     gSt1BN = 0;     // 已记录条数（≤8）
static UInt32     gSt1BSeq = 0;   // 全局调用序号（饱和 255）
// 256（`-NRedSt1C`，默认关）：`[st1c]` 仅记 `c>=1` 的前 8 条调次（钉死 stage1 实际 client 集合）。
struct NRedSt1CRec { UInt32 b; UInt32 c; UInt32 seq; };
static bool       gSt1C = false;
static NRedSt1CRec gSt1CRec[8];
static UInt32     gSt1CN = 0;     // 已记录条数（≤8）
static UInt32     gSt1CSeq = 0;   // stage≥1 调用序号（饱和 255）
// 257（`-NRedSt1P`，默认关）：client 3 stage1 时刻的 P3 观测（仅首次；只读一次）。
static bool   gSt1P = false;
static bool   gSt1PDone = false;
static UInt64 gSt1P3Obs = 0;   // *(ctx+0x20)（原样）
static UInt64 gSt1P3V18 = 0;   // *(P3+0x18)（P3 过 KPtr 才读）
static UInt64 gSt1P3V28 = 0;   // *(P3+0x28)（同上）
// （hex 行数由 len 决定，最多 8 行×32B；不设一次性限制，保持“每次转储都完整”。）
// 本 kext 的运行时基址（＝ `processKext` 的 `slide`）。供 N-2 把 RA 归零到 VM 地址。
//  ⚠ 与 `kcSlide` 无关（该成员全仓库只读不写、恒 0 ⇒ 用它归零会得到假读数）。
static UInt64 gHwlibsSlide = 0;
// ── GVM 软件初始化替换（卡面 2026-10-18）：门控与"头部戳记"用的标量 ──────────────────────────
//  门控（**默认全关**）：`-NRedGvmSwInstall`＝是否安装我方实现；`-NRedGvmSwInitPass`＝软件初始化
//  全部真实成功后是否放行（返 0）。⚠ 关态 ⇒ 不安装、不介入、零新增日志（行为与现状逐字一致）。
//  ⚠ 为什么放在文件头区：头部戳记在 `NRedTraceFlush*` 的头部区（本文件上方）读它们，位置早于
//    实现本体 ⇒ 依本文件既有惯例（`gTtlBisect`／`gDevTablePatch`／`gHwlibsSlide` 同此）前置。
static bool   gGvmSwInstall  = false;   // `-NRedGvmSwInstall`（总开关，默认关）
static bool   gGvmSwInitPass = false;   // `-NRedGvmSwInitPass`（**放行开关**，默认关；R70 §3：语义＝"仅当全部
                                        //  步骤**达可用层**时才允许报成功"⇒ 当前它**无法单独导致成功**）
static UInt32 gSwInitIdsMask = (1U << 5); // `-NRedSwInitIds` 解析结果（1..15 位掩码）＝**默认只启用模块 5**
static UInt32 gGvmSwCalls    = 0;       // 全部调用次数（`[gvmsw] n=`；上一张卡已交付，保持语义）
static UInt32 gSwCallN[16]   = {0};     // 各模块被调用次数（`[swi] n=`＝**按模块号计数**；索引＝模块号 1..15）
// R98 §1（R99 S1）：**按模块结果戳记**的两个标量数组（只读；写入点＝统一入口算完 `res` 之后）。
//  `r[id]`＝该模块**最近一次**我方实现的返回值；**初值 `0xFF`＝该 id 从未被调到我方实现**
//    （与"返回了 255"不可混淆：`kSwRes*` 最大为 8）。
//  `n[id]`＝该模块被调到我方实现的次数（**饱和 255**，防溢出回绕）。
static UInt8 gSwResById[16] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static UInt8 gSwCntById[16] = {0};
static UInt32 gSwLastId      = 0;       // 最近一次送达的模块号（1-based；0＝从未被调到）
// R119／R121：模块 9（VCN）**只读探针**的读数载体（BSS 标量；初值 `0xFF…FF`＝"未读"）。
//  只在探针内写；**戳记产出条件＝`gProbe9N != 0`** ⇒ 探针从未被调 ⇒ `[swprobe9]` 一行不多 ✓。
static UInt64 gProbe9[6] = {0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
                            0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL};
static UInt32 gProbe9N   = 0;   // 探针被调次数（供"是否跑过"的判据；`[swprobe9] n=`）
// R169：模块 2（SDMA）D2 实现的观测载体（BSS 标量；`0xFF…FF`＝"未读到"哨兵）。
//  产出条件＝`gM2N != 0`（本实现真被调到）；成功时 st=00。
static UInt32 gM2N    = 0;                        // 本实现被调次数（`[swsdma] n=`）
static UInt32 gM2Step = 0;                        // 最后失败步编号（R169 §5.1：01–08）
static UInt32 gM2K    = 0xFFFFFFFFU;              // 匹配到的 block type＝`*(u32*)(p1+8)`（调用方 `local_340` 写入）
static UInt64 gM2Cfg  = 0xFFFFFFFFFFFFFFFFULL;    // `*(u64*)(obj+0x2c8)`（清零回读后的首指针，应为 0）
static UInt64 gM2Q    = 0xFFFFFFFFFFFFFFFFULL;    // `*(u64*)(obj+0x2a8)`（queue 对象，应非 0）
// R172：模块 14（MES）D2 实现的观测载体（BSS 标量；`0xFF…FF`＝"未读到"哨兵）。
//  产出条件＝`gM14N != 0`；成功时 st=00。
static UInt32 gM14N    = 0;                       // 本实现被调次数（`[swmes] n=`）
static UInt32 gM14Step = 0;                       // 最后失败步编号（R172 §7.1：01–07）
static UInt32 gM14Code = 0xFFFFFFU;               // A12 处 `0x48632(obj+0x6c)` 的返回值
static UInt64 gM14Cfg  = 0xFFFFFFFFFFFFFFFFULL;   // `*(u64*)(obj+0x790)`（清零回读后，应为 0）
static UInt64 gM14Q    = 0xFFFFFFFFFFFFFFFFULL;   // `*(u64*)(obj+0x7b0)`（清零回读后，应为 0）
// 253（`-NRedMesReal`，默认关）：MES stage0 真实路径（调用苹果 0x441ad）的观测与开关。
static bool   gMesReal = false;
static UInt32 gMesCode = 0xFFFFFFU;              // 0x48632(obj+0x6c) 现场读数
static UInt64 gMesP790 = 0;                      // 0x441ad 之后的 *(u64*)(obj+0x790)
static UInt64 gMesP7B0 = 0;                      // 0x441ad 之后的 *(u64*)(obj+0x7b0)
static UInt32 gMesRet  = 0xFFFFFFFFU;            // 0x441ad 返回值（未调用＝哨兵）
// 254（`-NRedMesB`，默认关）：b 系列最小真实描述符（4 pipe × 0x28B；+0x10＝固件头 uc_start_addr_lo）。
static bool   gMesB = false;
static UInt8  sMesBDesc[4][0x28];
static bool   sMesBInit = false;
static UInt32 gMesBCode = 0xFFFFFFU;
static UInt64 gMesBP790 = 0;
static UInt64 gMesBV10  = 0;
// R175：模块 9（VCN）实实现的观测载体（BSS 标量；`0xFF…FF`＝"未读到"哨兵）。
//  产出条件＝`gVcnN != 0`；成功时 st=00。
static UInt32 gVcnN    = 0;                       // 本实现被调次数（`[swvcn] n=`）
static UInt32 gVcnStep = 0;                       // 最后失败步编号（R175 §7.1：01–06）
static UInt64 gVcnO8   = 0xFFFFFFFFFFFFFFFFULL;   // `*(u64*)(out+8)`（B3）
static UInt64 gVcnL10  = 0xFFFFFFFFFFFFFFFFULL;   // `*(u64*)(local_28+0x10)`（应为 0）
static UInt64 gVcnL18  = 0xFFFFFFFFFFFFFFFFULL;   // `*(u64*)(local_28+0x18)`（应为 0）
static UInt64 gVcnL30  = 0xFFFFFFFFFFFFFFFFULL;   // `*(u64*)(local_28+0x30)`（应为 0）
// R194：模块 15（JPEG）**只读探针**的观测载体（BSS 标量；`0xFF…FF`＝"未读到"哨兵）。
//  产出条件＝`gJpegN != 0`；st：01 空参／02 指针门／03 *p1 不符／04 *p2 不符／05 指针非法／06 读取完成。
static UInt32 gJpegN    = 0;                       // 本探针被调次数（`[swjpeg] n=`）
static UInt32 gJpegStep = 0;                       // 最后步号（R194）
static UInt64 gJpegDev  = 0xFFFFFFFFFFFFFFFFULL;   // `*(u64*)(p1+0x270)`（dev_A）
static UInt64 gJpegSelf = 0xFFFFFFFFFFFFFFFFULL;   // `*(u64*)(dev_A+8)`（devSelf）
static UInt64 gJpegFa   = 0xFFFFFFFFFFFFFFFFULL;   // `*(u64*)(dev_A+0x10)`（fnAlloc 槽）
static UInt64 gJpegFi   = 0xFFFFFFFFFFFFFFFFULL;   // `*(u64*)(dev_A+0x48)`（fnInit 槽）
// R181：模块 10（DMCU）S1 的观测载体（BSS 标量；`[swdmcu]` 一行）。
//  产出条件＝`gDmcuN != 0`；st：01 入参空／02 指针门／03 区标识／04 输出区标识／05 正常 fail-closed。
static UInt32 gDmcuN    = 0;                       // 本实现被调次数（`[swdmcu] n=`）
static UInt32 gDmcuStep = 0;                       // 最后步号（R181 §5.1）
static UInt64 gDmcuF0   = 0;                       // `*(u32*)p1`（形状值，p1 已过指针门）
static UInt64 gDmcuF280 = 0;                       // `*(u64*)(p1+0x280)`（苹果已写；仅观测）
// R191：护栏命中标记（1＝曾命中；纯 BSS，不依赖 `[swgrd]` 落盘）。护栏在 `wrapMode9440f` 内直接
//  返回苹果极性的 0（失败），不进入 `NRedSwInitDispatch` ⇒ `gSwResById` 不会被记成"成功"。
static UInt32 gGrdLastHit = 0;
// R129／R143（观测）：模块 8 的**关键字段留存**（只读我方 BSS；`0xFF…FF`＝"未读到"哨兵）。
//  `gM8V612`＝能力位快照；`gM8H712`＝**D2 不写该字段**（`0x66d84` 属已删的 `#20`）⇒ 回读应为 0（`bit29` 佐证）；
//  `gM8H1728`＝由保留的 `#20a`（`0x64b1a(h,h+1728,240)`）装表；`gM8N`＝模块 8 本体被进入的次数。
//  **纯标量读、零新增解引用方向。**
static UInt32 gM8V612  = 0xFFFFFFFFU;             // `*(u32*)(I+612)`＝**能力位快照 `v`**（R128 §4.3 的判别式；
                                                  //   D2 起 `[20]`/`[23]` 均已删 ⇒ 语义降级为"供后续参照"）
static UInt32 gM8H712  = 0xFFFFFFFFU;             // `*(u32*)(h+712)`（**D2 不调用 `0x66d84` ⇒ 该字段不被写**，
                                                  //   回读应为 0；`omittedMask bit29` 的佐证）
static UInt64 gM8H1728 = 0xFFFFFFFFFFFFFFFFULL;   // `*(u64*)(h+1728)`（由保留的 `#20a` `0x64b1a(h,h+1728,240)` 装表）
static UInt32 gM8N     = 0;                       // 我方模块 8 本体被进入的次数（戳记"跑过"判据）
// R143（D2）：模块 8 的**硬件侧/回读观测**（只读我方 BSS；`0xFF…FF`＝"未读到"哨兵）。
//  依据 R141 §4（`[swhw8]` 字段表）＋复核 Q6（**补 `h+1736`＝槽 1、`h+1960`＝槽 29**）——正是"槽 1..29 未装"
//  与"槽 29 未取得"的**直接读数**；**不含** `*(*(h+16)+72)` 的解引用（复核 Q6 明确禁止）。
static UInt8  gM8H720  = 0xFFU;                   // `*(u8*)(h+720)`＝**字节标志**（**D2 不设** ⇒ 回读应为 0）；
                                                  //  **R145/N-2**：原按 u32 读会带上相邻 3 字节、可能误判"非 0"
static UInt64 gM8H888  = 0xFFFFFFFFFFFFFFFFULL;   // `*(u64*)(h+888)`（D2-1 清零后回读）
static UInt64 gM8H896  = 0xFFFFFFFFFFFFFFFFULL;   // `*(u64*)(h+896)`（同上）
static UInt64 gM8H904  = 0xFFFFFFFFFFFFFFFFULL;   // `*(u64*)(h+904)`（同上）
static UInt64 gM8H912  = 0xFFFFFFFFFFFFFFFFULL;   // `*(u64*)(h+912)`（同上）
static UInt64 gM8H1712 = 0xFFFFFFFFFFFFFFFFULL;   // `*(u64*)(h+1712)`（子对象，`#21` 取建）
static UInt64 gM8H1736 = 0xFFFFFFFFFFFFFFFFULL;   // `*(u64*)(h+1736)`＝**槽 1**（未装 ⇒ 苹果 hw-init 静默返 0）
static UInt64 gM8H1960 = 0xFFFFFFFFFFFFFFFFULL;   // `*(u64*)(h+1960)`＝**槽 29**（uCode 常量，未取得）
// R160（模块 7 D2 观测）：GC 软件初始化的读数留存（**纯标量写**；`nredGcSwInit` 写、`nredSwLogCall` 读）。
//  `0xFF…FF`＝"未读到"哨兵；解引用白名单见 R160 §5.4（不读 `*(h+16)`、不新增任何解引用）。
static UInt32 gM7N       = 0;                       // 模块 7 本体被进入的次数（`[swhw7] n=`）
static UInt32 gM7Step   = 0;                       // 本次失败步编号（R161 §3.1；00＝无失败，0C＝门关）
static UInt32 gM7Code    = 0xFFFFFFU;               // `0xb129(I+12)` 的打包值（入口重置为 0xffffff）
static UInt64 gM7O1048   = 0xFFFFFFFFFFFFFFFFULL;   // `*(u64*)(O+1048)`（#29）
static UInt32 gM7O1056   = 0xFFFFFFFFU;             // `*(u32*)(O+1056)`（#30）
static UInt64 gM7H3520   = 0xFFFFFFFFFFFFFFFFULL;   // `*(u64*)(H+3520)`（#54 的取/建产物）
static UInt32 gM7H3528   = 0xFFFFFFFFU;             // `*(u32*)(H+3528)`（应等于 `0xc312` 的返回）
static UInt64 gM7S3552   = 0xFFFFFFFFFFFFFFFFULL;   // M1-c 回读：`H+3552` 槽
static UInt64 gM7S3560   = 0xFFFFFFFFFFFFFFFFULL;   // M1-c 回读：`H+3560` 槽
static UInt64 gM7S3568   = 0xFFFFFFFFFFFFFFFFULL;   // M1-c 回读：`H+3568` 槽
static UInt32 gM7StubHit = 0;                       // bit0=3552／bit1=3560／bit2=3568（桩被调用过）
static UInt64 gM7Tab[5]  = {0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
                            0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL};   // `H+3544/3784/3944/4000/4024`

// R105：入参结构两个 32 位字段的**留存**（供 `[swinfo]` 戳记；纯读、零新增解引用方向）。
//  `0xFFFFFFFF`＝**从未读到**（哨兵；与真实值可区分——模块判定只需 `==1`／`==0` 之类的小值）。
static UInt32 gSwInfo12 = 0xFFFFFFFFU;   // `*(u32*)(info+12)`（模块 2（SDMA）的关键判别字段）
static UInt32 gSwInfo8  = 0xFFFFFFFFU;   // `*(u32*)(info+8)`（多数模块的"版本/存在"判别字段）
static UInt32 gGvmSwInsCode  = 0;       // 安装结果码：0＝未执行（总开关关/未启用/熔断）；1＝已安装；
                                        //  2＝跳过（槽内已是我方）；3＝形态变化（三重校验③失败）；
                                        //  4＝subobj 非法；5＝该模块未启用；6＝期望值未确证/未解出（不猜、不装）
static UInt32 gSwInsId       = 0;       // 最近一次安装动作对应的模块号（0＝未执行过）
static UInt64 gSwInsGot      = 0;       // 最近一次安装动作读到的原值 `got`
static UInt64 gSwInsExp      = 0;       // 最近一次安装动作的期望值 `exp`（运行期地址；0＝未执行/未确证）
static void*  gOrigGvmSwInit = nullptr; // 最近一次替换掉的苹果实现（＝exp）
// 最近一次调用的入参摘要（供 `[swi]` 戳记；口径＝**最近一次**）
struct NRedSwLastCall {
    UInt64 info, i3656, i3664, rec0;    // `info` 指针、`info+3656`、`info+3664`、记录区首 8 B
    UInt32 out0;                        // `out+0` 初值（我方**从不写**该字段）
    int    res;                         // 最近一次返回值
};
static NRedSwLastCall gSwLastCall = {0, 0, 0, 0, 0, 0};
// 上一张卡已交付的 `[gvmsw]` 行摘要（**保持格式与语义不变**；R60 §1.6 第 6 条）
//  ⚠ 由 `nredSwLogCall` 统一更新 ⇒ 不依赖尾段日志通道（尾段"保头丢尾"会被截断）。
struct NRedGvmSwLast {
    UInt64 i3656, i3664, r0, r1, o8;   // 入参摘要（最近一次）：`info+3656`／`info+3664`／记录区前 16 B／out+8 初值
    UInt32 o0;                          // `out+0` 初值（最近一次；我方**从不写**该字段）
    int    ret;                         // 最近一次返回值
};
static NRedGvmSwLast gGvmSwLast = {0, 0, 0, 0, 0, 0, 0};
// R90：句柄三字段 ＋ 句柄地址的**留存**（供 `[swihand]` 头部戳记）。
//  为什么可"顺手留存"：M-1 守卫**本来就已经读**了 `slot1`／`*(slot1+8)`／`*(slot1+16)`／`*(slot1+24)`
//   （R82 §1／§2.1）⇒ 本卡只把这三个值**存进全局**（几次标量写入，**零新增解引用**）。
//  ⚠ 戳记处**只读这些标量**、不再解引用；口径＝**最近一次**（`[swihand] n=` 给出模块 5 的调用次数，
//    便于判"这些值是否已被下一次调用刷新"）。
static UInt64 gSwS1Slot     = 0;   // `slot1`（＝`info+3664`）
static UInt64 gSwS1f8       = 0;   // `*(slot1+8)`
static UInt64 gSwS1f16      = 0;   // `*(slot1+16)`
static UInt64 gSwS1f24      = 0;   // `*(slot1+24)`（清理路径 `19cc5 callq *24(%rcx)` 的目标）
static UInt64 gSwHandleAddr = 0;   // 通过 `nredKernelPtrOk` 校验的 `handle`（S0 取/建所得；未走到 ⇒ 0）
// ── 观测通道修复（R64 ＋ 需求 v2「标记阶梯」）：三段标记，每段"先置标志 → 写一行静态标记 → 刷写" ──
//  背景（R20 实测）：8 个分片的头部四行全为 0／哨兵；根因＝**崩溃时刻没有新的刷写**（最后一次刷写
//  19 s、崩溃 ≈26 s）⇒ 头部区不是崩溃时刻的快照。且 v1（只在 `0x929ac` 包装入口刷一次）位于
//  "**按模块逐个处理之前**" ⇒ 在那里刷写读到的必然是"处理之前"的旧值（0）⇒ 等于重复本轮盲点。
//  ⇒ 本卡改为**标记阶梯**（需求 v2 R1-v2／R7–R10）：同一门控下按推进顺序**最多刷写 3 次**，每次
//  先写一行互不相同的静态标记（纯内存 ⇒ 保证内存环非空 ⇒ 刷写**必然产出分片**）再刷写：
//    **M1**＝`wrapIpEntrySeq` 入口（证明"进入了 TTL 阶段"）；
//    **M2**＝`wrapMode9440f` 入口且该次调用属**模块 5**（**不额外限制 stage**；证明"安装时机出现了"；
//      **安装动作之后** ⇒ 该片头部区能读到安装侧真实值）；
//    **M3**＝我方替换实现被调用（唯一入口 `NRedSwInitDispatch` 函数体入口；**决定性的一条**）。
//  ⚠ 门控**默认关** ⇒ 关态三处**一行都不执行**（与现状逐字一致）；**不新增机器码挂钩点**（M1／M2 用
//    既有包装入口、M3 在我方自有函数体内）；**既有观测行格式与语义一字不改**（不新增任何头部行）。
static bool gTraceAtIpSeq = false;      // `-NRedTraceAtIpSeq`（**默认关**；存在即开、无取值）
static bool gFlushedAtIpSeq = false;    // M1 幂等：该段只触发一次
static bool gFlushedAtIpSeqM2 = false;  // M2 幂等：该段只触发一次
static bool gFlushedAtIpSeqM3 = false;  // M3 幂等：该段只触发一次
// 三段标记文本（**静态常量、互不相同、分别含 M1／M2／M3 字样、无格式符、无运行期数值**；v2 R8）。
static char kMarkM1[] = "M1: ip-seq entry reached (0x929ac)\n";
static char kMarkM2[] = "M2: install point reached (module5)\n";
static char kMarkM3[] = "M3: our sw-init implementation invoked\n";

// 标记动作（三段共用）：**先置标志 → 写一行静态标记到内存环**（需求 v2 R7／R10）。
//  ⚠ **R78 去刷盘化**：本函数**只写内存环**（`nredTraceLine`），**不再调用落盘刷写**——早期路径
//    （bringup／定时器初始化／钩子早期段）写文件会让内核线程阻塞、随后被 watchdog 强制重启
//    （无 panic ⇒ 无 panic 分片）。⇒ **落盘交回既有安全时点**（`NvMsgBuf::dumpNow()` 与周期拍；
//    见 `NvMsgBuf.hpp` 对应处与本文件顶部"安全时点"注释）。
//  ⚠ **代价（如实承认，R3）**：标记**只存在于内存环** ⇒ 若崩溃发生在"标记之后、下一次安全时点之前"，
//    标记**不会被带出**（本项目 R20／R21 实测即如此）⇒ 该装置当前**大概率读不到**，但**不得**用
//    "另加刷写点／放大缓冲／改其它 I/O"去补（R2／R5）。
//  ⚠ 门控判断在各**调用点**（关态连本函数都不进 ⇒ 零执行）；返回 `void`，调用方不使用其返回值。
static void nredTraceMark(bool& done, char* const mark, const int len)
{
    if (done) { return; }
    done = true;
    nredTraceLine(mark, len);        // ★ 只写内存环（纯内存、零 FS、零 os_log、零大栈缓冲）
}
// N-2：`0x929ac`（判据④）的调用总次数／最近返回值／最近调用点档位／分档计数。
//  ⚠ 调用点判别：该函数有**三个**调用点（`92956`＝判据④、`92dcf`、`92e5e`），
//    只记"总次数/返回值"**无法归属** ⇒ 用 `__builtin_return_address(0)` 减基址后比对三档。
//  ⚠ 纯标量、零解引用（`__builtin_return_address` 取的是寄存器，不解引用）。
static UInt32 gC4n = 0, gC4ret = 0, gC4a4 = 0, gC4b4 = 0, gC4c4 = 0;
static UInt64 gC4off = 0;      // 最近一次归零后的 RA 偏移（未匹配三档时供追加判据）
static UInt32 gC4site = 0;     // 0=? ／1=判据④(0x9295B) ／2=0x92dbe(0x92DD4) ／3=0x92e28(0x92E63)
static const char kC4Site[] = {'?', '4', 'B', 'C'};
// N-3：`0x93f76`（tls-create）**出口**读 `Ee8/r28/r30/s2c/s3c`（在原函数返回之后读 ⇒ 取写入后的值）。
static UInt64 gBisEe8 = 0, gBisR28 = 0, gBisR30 = 0;
static UInt32 gBisS2c = 0, gBisS3c = 0;
// N-4：`0x9440f` 前三次 (client, stage, ret)。该回调曾在 B25 double-fault 栈上 ⇒ 门控关时立即 return。
struct Bis9440f { UInt32 client, stage, ret; };
static Bis9440f gBis9440f[3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
static UInt32 gBis9440fN = 0;
// ── R33 扩读（2026-10-18）：9 个纯标量，取运行期值以三态定案 ──────────────────────
//  规格：`tmp/tech-manager/R33-后段性质与发轮口径-2026-10-18.md` §4（**本卡权威依据**；
//  与 R29 §④ 冲突处以 R33 为准）。
//  背景：`ipi_gvm_sw_init`（`0x98125`）的第五出口失败，其返回码与一条**能力位校验**
//  （`0xa3014` 读 `o3+176` 的位 0/1/2）有关；该值来自**运行期记录** ⇒ 只能上机取。
//  ★ 本卡**只取证**：严禁任何"能力位置位"或行为改动。
//  ⚠ 全部为**纯内存读**；每一级解引用**自备空守卫**，短路即出、写哨兵。
//  哨兵口径：`kBisNone64 = 0xFFFFFFFFFFFFFFFF`（指针级）／`kBisNone32 = 0xFFFFFFFF`（u32 级）。
static const UInt64 kBisNone64 = 0xFFFFFFFFFFFFFFFFULL;
static const UInt32 kBisNone32 = 0xFFFFFFFFU;
static UInt64 gBisO3 = 0, gBisO4 = 0, gBisObj14 = 0, gBisFn112 = 0;
static UInt64 gBisFn600 = 0, gBisFn616 = 0, gBisSub = 0, gBisHost = 0;
static UInt32 gBisV176 = kBisNone32, gBisA = kBisNone32, gBisX = kBisNone32;
// ★ R34 要求：`v176` 的"**是否真读过**"标志。
//  为什么需要：R15 的 `v176=ffffffff` 既可能是"真值恰为 0xFFFFFFFF"也可能是"未读的哨兵"
//  ⇒ **两者不可区分**，本轮判读困难的一半原因即在此。
//  ⇒ 语义：`vb=1` ⇒ `v176` 是**真读到的值**（哪怕它恰等于 0xFFFFFFFF）；
//          `vb=0` ⇒ `v176` 是**初始哨兵**，不得当作任何真值使用。
static UInt32 gBisV176Read = 0;
// ★ R39（2026-10-18）：`arg2` 确认读数（**本卡唯一新增的 u32**）。
//  背景：R38 已定真实提交函数＝`FUN_000a4b90`（`*(slot1+616)`），其成败由第 2 参
//  `arg2` 决定：`arg2=2` ⇒ 打印并返 2 ⇒ **失败**；`arg2=0` ⇒ 返 0 ⇒ 成功。
//  算式（`0x9829e` 逐字，R39 §1）：`arg2 = 2 × ((rec[0]==1) ∧ 0xa28ad 返真)`，
//  其中 `rec` 由 `0xa28ad(host,&rec)` 填：`rec = *(u64*)(slot3obj+56)`（24 B），
//  `slot3obj = 0x8b8e7(root) = 0x8bbc4(*(root),3) = *( *(root) + 16 + 3*24 )`。
//  ⇒ **只读、不调用 Apple 函数**（`0xa28ad` 会写栈缓冲 ⇒ 属介入，禁用）：直接按
//  上述取法读 `rec[0]` 并算 `a2`。
//  ⚠ R39 §0.3 顺带纠正：root 的 slot 3（base＝`*(root)`）**不是** R16 的 `o3`
//  （base＝`*(ctx)`）⇒ **a2 的 base 必须用 `rootIn`＝`*(root)`，不得复用 `o3`**。
static UInt32 gBisA2 = kBisNone32;      // arg2：{0,2}；哨兵＝未读
static UInt32 gBisA2Read = 0;           // rb：1＝真读过 rec[0]；0＝哨兵（未读）
// ★ R44 裁决（2026-10-18）：`r0`＝`rec[0]` 原值 ＋ `r0b` 已读标志（**本回合唯一新增**）。
//  目的：下轮真机用它**自检** `a2 == 2×((r0==1) && (reci!=0))`。
//  ⚠️ 背景：R44 §② 称我方 a2 少一层解引用——**已驳回**（PM 裁决：Apple 与我的复刻链
//  均为 `***subobj` 后 `+88`，逐字一致；R44 把"复刻链"误读成"调用 0xa28ad(*subobj)"）。
//  ⇒ **不得**改链深度；`r0` 与 `a2` 同源同链（同一个 `rec0`）。
static UInt32 gBisR0 = kBisNone32;      // r0：`rec[0]` 原值（u32）；哨兵＝未读
static UInt32 gBisR0Read = 0;           // r0b：1＝真读过；0＝哨兵（未读）
// ★ R42（2026-10-18）：`r14i`／`reci` 两个指针标量（**本卡唯一新增**）。
//  背景：R17 实测 `a2=0` ⇒ H4 否证 ⇒ 失败必在 `0x98125` 的**三个提前返回点**之一：
//   ① `subobj==0`（`98175 je 0x9828e`，静默）；② `0x958d8(host,5)==0`（`981d9 je 0x98251`，
//   "NULL GVM discovery info!"）；③ `*(subobj+8)==0` 或回调返非 0（`981ff/98211 ⇒ 0x98217`，
//   "NULL GVM sw_init entry!"）。⇒ 2 个指针即可把失败点落在 `0x98125` 内部。
//  ⚠ 只读、不调用 Apple 函数（`0x958d8` 为纯查表，按 R34/我的 `a`/`x` 同一算式逐字复刻，
//   非"调用"）。
//  ⚠ 每个字段各配"是否真读到"标志（`ri=`/`rc=`），避免哨兵与真值不可区分。
static UInt64 gBisR14i = 0;             // r14i：`*(u64*)(subobj+8)`（＝ 0x98125 的 `r14`＝回调指针）
static UInt32 gBisR14iRead = 0;         // ri：1＝真读过；0＝哨兵（subobj 空）
static UInt64 gBisReci = 0;             // reci：`0x958d8(host,5)` 的记录指针（非 0 ⟺ 记录可查）
static UInt32 gBisReciRead = 0;         // rc：1＝真读过（"真读到 0"也算）；0＝哨兵
// ★ R47/R49（2026-10-18）：`r0b2`／`r14b2`／`recib2` 出口再读（**本卡唯一新增**）。
//  定位事实：既有读数点整体位于 org 调用**返回之后**（R33 块在 `FunctionCast(orgMode9440f)`
//  之后）⇒ 现有 `r0`/`r14i`/`reci` 本就是"出口值"。本卡按规格在同一读点**再独立
//  解引用一次**并各配已读标志（`r0b2b`/`r14b2b`/`recib2b`）⇒ 作确定性复核：
//  同地址两次读值一致 ⇒ 读链稳定；不一致 ⇒ 读链/对象在区间内被改。
//  ⚠ 纯内存读、同链同守卫、哨兵与真值可区分（Read=1 时值可信，即使值恰为哨兵形态）。
//  ⚠ 哨兵口径（R3）：`r14b2`／`recib2` 的值 `0` 是**合法真值**（`0x98125` 的 X2／X3 触发条件正是
//    `0x958d8==0`／`*(subobj+8)==0`）⇒ 未读到一律写既有指针级哨兵 `kBisNone64`，不得用 0。
static UInt32 gBisR0b2 = kBisNone32;      // r0b2：出口再读 `rec[0]`（u32）；哨兵＝未读（kBisNone32）
static UInt32 gBisR0b2Read = 0;           // r0b2b：1＝真读过；0＝哨兵（未读）
static UInt64 gBisR14b2 = kBisNone64;     // r14b2：出口再读 `*(u64*)(subobj+8)`；哨兵＝未读
static UInt32 gBisR14b2Read = 0;          // r14b2b：1＝真读过；0＝哨兵（subobj 空）
static UInt64 gBisRecib2 = kBisNone64;    // recib2：出口再读 `0x958d8(host,5)` 记录指针；哨兵＝未读
static UInt32 gBisRecib2Read = 0;         // recib2b：1＝真读过（"真读到 0"也算）；0＝哨兵
static UInt32 gBisHit = 0;   // 本扩读是否命中（1＝走到了 client5/stage0 且各前置非空；0＝未命中/未走到）
static UInt32 gBisShort = 0; // 短路原因码（**只记首次**）：0=无
enum : UInt32 {
    kBisShortNone   = 0,   // 无短路（全部读到）
    kBisShortArg    = 1,   // L0：`wrapMode9440f` 的 arg0 为空 ⇒ 无从下手
    kBisShortSub    = 2,   // L1：`subobj = *(a+48)`（＝`0x98050(a)`）为空
    kBisShortHost   = 3,   // L1b：`host = *(subobj)` 为空
    kBisShortSlot1  = 4,   // L2：`slot1 = *( *(root) + 40 )` 为空（R34 修正①）
    kBisShortRoot   = 5,   // L3a：`root = *(host)` 为空
    kBisShortRec    = 6,   // L3b：`record = 0x958d8(host,5)` 为空
    kBisShortRootIn = 7,   // ★R34 新增级：`rootIn = *(root)`（`0x8b4bf` 内那一次解引用）为空
    kBisShortCtx    = 8,   // ★R34 新增级：`ctx = *(slot1+600)` 为空
    kBisShortBase3  = 9,   // ★R34 新增级：`base3 = *(ctx)`（`0x8b8e7` 内那一次解引用）为空
};
static const char* const kBisShortReason[] = {"none", "arg", "sub", "host", "slot1", "root", "rec",
                                              "rootIn", "ctx", "base3"};
// B19：`NRED_TRACE` 的 SYSLOG 通道静音标志（默认 false）。置位/复位＝fw::NredSyslogMuteGuard
//  （NRedFwBringupHook.hpp，仅 `-NRedFwBringup` 的 bringup 期内为真）⇒ 见 HWLibs.hpp 该符号声明处注释。
bool gNredTraceSyslogMuted = false;
// B73b：SMU「起来确认」序列的**一次性（文件级）标志**（技术经理要求文件级共用）：
//  · 语义：本进程内该确认序列至多执行一次（首条 `TestMessage(0x01)` 在 SMU 不应答时
//    最多等 ~2 s，上界＝ctx+0x310 的 2000 步；Apple 可能多次调用发送点 ⇒ 防反复等待）。
//  · **共用约定**：`PPSMC_MSG_GetSmuVersion`(0x02) 在我方有**两处发送点**（`smuInternalSwInitOld`
//    与 `smuGetUCodeConsts`）⇒ 本标志由「SMU 起来确认」序列**共用**；**若将来在其它发送点
//    也插同一确认，必须复用本标志**（否则一次引导会发两次、各等 ~2 s）。
static bool sGSmUpProbeDone = false;


// 诊断行落盘（2026-09-28 第 17 轮）：L2 走内核 `msgbuf`，覆盖窗口实测只有 26–35 s，而 TTL/BGM 的
//  关键读数在 34–39 s ⇒ **必须自建落盘**（自检已证明该通道可用：`selftest: rootvnode=… err=0`）。
//  文件名带 **kext slide** ⇒ 每次启动天然不同，归档后不会混轮次。⚠️ 必须先判 `rootvnode`（手册 §5.1）。
//
// A-22（重写）：bringup 早期路径（bringupRun/tmrInit/tmrLoad/hook 早期段）**只写内存环形缓冲**，
//  **绝不**调 `FileIO::writeBufferToFile`。依据（技术组长原话）：B7/B10 的 panic 栈确实经过
//  `nredTraceLine`→`FileIO::writeBufferToFile`→**APFS btree lookup**，说明当时 `rootvnode` 非空
//  （A-20 检查会放行）⇒ **在 bringup 早期调"APFS 写文件"本身就不安全**，加检查不解决，
//  必须改"落地方式"。⇒ 本函数改为：行写入内存环，由**既有安全时点**（`NvMsgBuf::dumpNow`／
//  `NRed::init` 定时拍／panic 分片路径）再刷盘（与 L2 `-NRedObserveDisk` 同构）。
//  行同时经 `NRED_TRACE` 的 `SYSLOG` 进内核 console 环形缓冲 ⇒ **双通道冗余**。
//  A-20 的 `dropped`/`written` 计数保留在内存侧（`gTraceSafe`）。
void nredTraceLine(char* const buf, const int n)
{
    // A-20 前置检查（保留）+ A-22 改走内存：不再有任何文件系统写接口调用。
    if (!gTraceSafe.shouldWrite(true, n)) { return; }   // 内存侧恒"可写"（rootvnode 无关）
    gTraceRing.push(buf, n);
}

// A4（2026-10-13）：探针解引用的"合法内核指针门"——非空 且 高位形如内核指针（0xFFFF）。
//  口径沿用 wrapMode9e505 的既有判定（`(sv >> 48) == 0xFFFF`，A4 前唯一先例）；防"表内容
//  残留的低位垃圾值"被当指针解引用（B50/B51 实证：base28/base30/t28_0 等字段确实可能为
//  垃圾/0——垃圾指针解引用是 panic 源）。
//  ⚠️ 不防"悬空栈指针"（内核栈地址高半区同为 0xFFFF——但悬空栈读不 panic，只读旧数据）。
static inline bool nredKernelPtrOk(const void* const p)
{
    return p != nullptr && (reinterpret_cast<UInt64>(p) >> 48) == 0xFFFF;
}

// 269：`__text` 函数入口判据（`push %rbp` = 0x55）。仅用于已确认“应为代码指针”的槽；
//  数据指针不得套用此判据。KPtr 仍不证明映射，读首字节属低概率残余风险。
static inline bool nredTextPrologue(const UInt64 p)
{
    return p != 0 && nredKernelPtrOk(reinterpret_cast<const void*>(p)) &&
           *reinterpret_cast<const UInt8*>(p) == 0x55;
}

// 257：boot-arg **精确 token** 匹配（避免 `-NRedSt1P` 与既有 `-NRedSt1Pre`、
//  `-NRedSt1W` 与 `-NRedSt1W2` 这类“前缀开关”互相误开）。只读 `PE_boot_args()` 字符串，
//  不解析值、不分配内存。
static bool nredExactBootArg(const char* const name)
{
    const char* const ba = PE_boot_args();
    if (ba == nullptr || name == nullptr) { return false; }
    const size_t n = strlen(name);
    const char* p = ba;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == '\n') { ++p; }
        const char* const q = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n') { ++p; }
        if (static_cast<size_t>(p - q) == n && strncmp(q, name, n) == 0) { return true; }
    }
    return false;
}

// A-22/A-28：把内存环里的早期 trace 行**刷盘**（仅在**既有安全时点**调用）。
//  安全时点（A-28 起）：
//    ① L2 周期拍 `dumpTick` 的 **stBusy 保护区**内（**本卡新增**，每拍都刷 ⇒ 死亡时点可读到秒）；
//    ② `NvMsgBuf::dumpNow()`（PP helper 入口即时落一拍）；
//    ③ panic 分片路径。
//  ⚠️ 本函数**是唯一**允许在 bringup 之外调 `FileIO::writeBufferToFile` 写 NRedTrace 的地方；
//     早期路径（bringupRun/tmrInit/tmrLoad/hook 早期段）**禁止**调用本函数。
//  返回：实际写出的行数（0 = 无内容或未就绪）。
//
// A-28 修掉的 A-22 三处并发差异（技术组长 R1 规格）：
//  ① **接 stBusy 互斥**：本函数只在 `dumpTick` 已持 `stBusy()` 的临界区内被调用（见 NvMsgBuf.hpp），
//     与 `dumpNow` 共用同一把标志 ⇒ 不会与周期拍并发写同一卷。
//  ② **fseq/body 无锁无原子**：改为**函数内静态 + 仅在 stBusy 保护区内改写**（顺序执行，无并发）；
//     且 `fseq` 只在写成功后递增（与 L2 `stSeq` 同构）。
//  ③ **唯一落盘点＝单点故障**：由"仅 X6000FB.cpp 一处"扩为"L2 周期拍每拍 + dumpNow + panic 路径"多点。
//
// A-28 "最后一拍可读到秒"：文件名带**秒级时间戳**（`sec`），并写一行 `[flush-stamp] sec=<n>` 首行标签
//  ⇒ 判读侧从"最后存在的那个 NRedTraceFlush 文件"即可读出本轮死亡时点（秒级）。
// ── B77①：落盘**先截断**（根因修）────────────────────────────────────────────
//  背景（技术经理离线定案）："受门控的行却落盘"这一矛盾的根因＝**旧轮残留尾部**——
//   写盘时**没有真正截断**，新正文比旧文件短时，旧文件尾部留在后面；而文件名只由
//   `slide(0)/fseq/sec` 决定 ⇒ 跨轮重名是常态 ⇒ 判读时把旧轮尾巴当成本轮读数（已误判多次）。
//  为什么需要本函数：Lilu `FileIO::writeBufferToFile` 的 `fmode` 虽含 `O_TRUNC`，但实测
//   `O_TRUNC` 在该路径未被 honor（旧尾部仍在）⇒ 我方**显式**把目标文件长度截为 0。
//  做法：`vnode_open`（O_CREAT|FWRITE，不截断）→ `vnode_setattr(va_data_size=0)` → `vnode_close`。
//   ⚠️ 只改"落盘前的文件长度"，**不改**落盘路径/频率/正文格式/文件名格式（B77 第 4 件不做）。
//   ⚠️ 失败不致命：截断失败仍照常写（保持既有行为，不新增失效路径）。
static void nredTraceTruncateFile(const char* const path)
{
    vfs_context_t ctx = vfs_context_create(nullptr);
    if (ctx == nullptr) { return; }
    vnode_t vp = nullptr;
    // 仅 O_CREAT|FWRITE：不靠 O_TRUNC（实测不生效），长度由下面的 setattr 显式置 0。
    if (vnode_open(path, O_CREAT | FWRITE | O_NOFOLLOW, S_IRUSR | S_IWUSR, VNODE_LOOKUP_NOFOLLOW, &vp, ctx) == 0) {
        // XNU 惯用写法：`VATTR_INIT` → `VATTR_SET(va, va_data_size, 0)`
        //  （`VATTR_SET` 同时赋值并置 active 位 ⇒ `vnode_setattr` 才会执行截断）。
        vnode_attr va{};
        VATTR_INIT(&va);
        VATTR_SET(&va, va_data_size, static_cast<off_t>(0));
        vnode_setattr(vp, &va, ctx);   // 显式截断到 0（返回值不判：失败则沿用旧行为）
        vnode_close(vp, FWASWRITTEN, ctx);
    }
    vfs_context_rele(ctx);
}

// 243：`[st1m]` 头部行独立成 noinline 帧（保持 nredTraceFlush 帧增量 0）。
//  只读 gSt1Mask/gSt1CtxOk/gSt1N 三个 BSS 标量，零解引用。
__attribute__((noinline))
static void nredSt1mStamp(char* const body, size_t& off, const size_t cap)
{
    const int n = snprintf(body + off, cap, "[st1m] mask=%04x ctx=%u n=%u\n",
                           static_cast<unsigned>(gSt1Mask), static_cast<unsigned>(gSt1CtxOk),
                           static_cast<unsigned>(gSt1N));
    if (n > 0) { off += (static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : cap; }
}

// 244：`[st1r]` 头部行（放行观测；独立 noinline 帧，零解引用）。
__attribute__((noinline))
static void nredSt1rStamp(char* const body, size_t& off, const size_t cap)
{
    const int n = snprintf(body + off, cap, "[st1r] mask=%04x ret0=%04x n=%u\n",
                           static_cast<unsigned>(gSt1Try), static_cast<unsigned>(gSt1Ret0),
                           static_cast<unsigned>(gSt1RunN));
    if (n > 0) { off += (static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : cap; }
}

// 255：`[st1v]` 头部行——按 client 7 放行规格 §3 复用为 `p7/p3`（client 14 暂不生效；
//  将来恢复时另起 `[st1v14]`，不改本行格式）。独立 noinline 帧、零解引用。
__attribute__((noinline))
static void nredSt1vStamp(char* const body, size_t& off, const size_t cap)
{
    const int n = snprintf(body + off, cap, "[st1v] p7=%llx p3=%llx\n",
                           static_cast<unsigned long long>(gSt1P7),
                           static_cast<unsigned long long>(gSt1P3));
    if (n > 0) { off += (static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : cap; }
}

// 246：`[st1w]` 指针值行（不 deref svc；独立 noinline 帧，零解引用）。
__attribute__((noinline))
static void nredSt1WStamp(char* const body, size_t& off, const size_t cap)
{
    const int n = snprintf(body + off, cap, "[st1w] p7=%llx h=%llx svc=%llx\n",
                           static_cast<unsigned long long>(gSt1P7),
                           static_cast<unsigned long long>(gSt1H),
                           static_cast<unsigned long long>(gSt1Svc));
    if (n > 0) { off += (static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : cap; }
}

// 248：`[st1x]/[st1y]` 槽值行（仅 -NRedSt1W2；独立 noinline 帧，零解引用）。
__attribute__((noinline))
static void nredSt1W2Stamp(char* const body, size_t& off, const size_t cap)
{
    size_t room = cap;
    int n = snprintf(body + off, room, "[st1x] s10=%llx s18=%llx s20=%llx s38=%llx s40=%llx\n",
                     static_cast<unsigned long long>(gSt1S[0]), static_cast<unsigned long long>(gSt1S[1]),
                     static_cast<unsigned long long>(gSt1S[2]), static_cast<unsigned long long>(gSt1S[3]),
                     static_cast<unsigned long long>(gSt1S[4]));
    if (n > 0) { const size_t d = (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
                 off += d; room -= d; }
    n = snprintf(body + off, room, "[st1y] s90=%llx s100=%llx s118=%llx s120=%llx sk=%u\n",
                 static_cast<unsigned long long>(gSt1S[5]), static_cast<unsigned long long>(gSt1S[6]),
                 static_cast<unsigned long long>(gSt1S[7]), static_cast<unsigned long long>(gSt1S[8]),
                 static_cast<unsigned>(gSt1SkipHigh));
    if (n > 0) { off += (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room; }
}

// 249：`[st1o]` 头部行（最多 14 条；noinline、零解引用）。
//  250：无任何记录时输出 `[st1o] none seq0=%u`（seq0＝client 2 的调用计数）。
__attribute__((noinline))
static void nredSt1OStamp(char* const body, size_t& off, const size_t cap)
{
    static const UInt8 kIds[7] = {2, 5, 6, 7, 8, 9, 14};
    size_t room = cap;
    bool any = false;
    for (unsigned i = 0; i < 7; ++i) {
        for (unsigned s = 0; s < 2; ++s) {
            const NRedSt1ORec& r = gSt1ORec[i][s];
            if (r.used == 0 || room == 0) { continue; }
            const unsigned aok = nredKernelPtrOk(reinterpret_cast<const void*>(r.ctx)) ? 1u : 0u;
            const int n = snprintf(body + off, room, "[st1o] c=%u aok=%u ctx=%llx p=%llx seq=%u\n",
                                   static_cast<unsigned>(kIds[i]), aok,
                                   static_cast<unsigned long long>(r.ctx),
                                   static_cast<unsigned long long>(r.p),
                                   static_cast<unsigned>(r.seq));
            if (n > 0) { const size_t d = (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
                         off += d; room -= d; any = true; }
        }
    }
    if (!any && room != 0) {
        const int n = snprintf(body + off, room, "[st1o] none seq0=%u\n",
                               static_cast<unsigned>(gSt1OSeq[0]));
        if (n > 0) { off += (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room; }
    }
}

// 250：`[st1g]` 开关状态行——**本轮诊断常开**（定位后应改回“仅 gSt1O 时输出”）。
//  固定 23 字节模板直接落缓冲（不用 snprintf：避免 4 个变参的溢出槽，函数帧 ≤64 B）。
__attribute__((noinline))
static void nredSt1GStamp(char* const body, size_t& off, const size_t cap)
{
    static const char kTpl[] = "[st1g] p=0 w=0 o=0 s=0\n";
    static_assert(sizeof(kTpl) == 24, "st1g line length");
    if (cap < sizeof(kTpl) - 1) { return; }
    __builtin_memcpy(body + off, kTpl, sizeof(kTpl) - 1);
    body[off + 9]  = static_cast<char>('0' + (gSt1Pre ? 1 : 0));
    body[off + 13] = static_cast<char>('0' + ((gSt1W || gSt1W2) ? 1 : 0));
    body[off + 17] = static_cast<char>('0' + (gSt1O ? 1 : 0));
    body[off + 21] = static_cast<char>('0' + (gStage1 ? 1 : 0));
    off += sizeof(kTpl) - 1;
}

// 251：`[gc7]/[gc7x]` 头部行（noinline、零解引用）。
__attribute__((noinline))
static void nredGc7Stamp(char* const body, size_t& off, const size_t cap)
{
    size_t room = cap;
    for (unsigned i = 0; i < gGc7N && i < 2; ++i) {
        const NRedGc7Rec& r = gGc7Rec[i];
        const int n = snprintf(body + off, room, "[gc7] sel=%u hit=%u p7=%llx\n",
                               static_cast<unsigned>(r.sel), static_cast<unsigned>(r.hit),
                               static_cast<unsigned long long>(r.p7));
        if (n > 0) { const size_t d = (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
                     off += d; room -= d; }
    }
    if (gGc7xSeen != 0 && room != 0) {
        const int n = snprintf(body + off, room, "[gc7x] slot=%u val=%llx\n",
                               static_cast<unsigned>(gGc7xSlot),
                               static_cast<unsigned long long>(gGc7xVal));
        if (n > 0) { off += (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room; }
    }
}

// 252：`[st1b]` 原始调次行（≤8 条；noinline、零解引用）。
//  固定 27 字节模板 + 三位十进制直写（避开 snprintf 3 变参的溢出槽，函数帧 ≤64 B）。
__attribute__((noinline))
static void nredSt1BStamp(char* const body, size_t& off, const size_t cap)
{
    static const char kTpl[] = "[st1b] b=000 c=000 seq=000\n";
    static_assert(sizeof(kTpl) == 28, "st1b line length");
    constexpr size_t kLen = sizeof(kTpl) - 1;
    size_t room = cap;
    for (unsigned i = 0; i < gSt1BN && i < 8; ++i) {
        if (room < kLen) { return; }
        __builtin_memcpy(body + off, kTpl, kLen);
        const UInt32 b = gSt1BRec[i].b % 1000;
        const UInt32 c = gSt1BRec[i].c % 1000;
        const UInt32 s = gSt1BRec[i].seq % 1000;
        body[off + 9]  = static_cast<char>('0' + b / 100);
        body[off + 10] = static_cast<char>('0' + (b / 10) % 10);
        body[off + 11] = static_cast<char>('0' + b % 10);
        body[off + 15] = static_cast<char>('0' + c / 100);
        body[off + 16] = static_cast<char>('0' + (c / 10) % 10);
        body[off + 17] = static_cast<char>('0' + c % 10);
        body[off + 23] = static_cast<char>('0' + s / 100);
        body[off + 24] = static_cast<char>('0' + (s / 10) % 10);
        body[off + 25] = static_cast<char>('0' + s % 10);
        off += kLen;
        room -= kLen;
    }
}

// 256：`[st1c]` 仅 c>=1 的调次行（≤8 条；固定模板直写，帧 ≤64 B）。
__attribute__((noinline))
static void nredSt1CStamp(char* const body, size_t& off, const size_t cap)
{
    static const char kTpl[] = "[st1c] b=000 c=000 seq=000\n";
    static_assert(sizeof(kTpl) == 28, "st1c line length");
    constexpr size_t kLen = sizeof(kTpl) - 1;
    size_t room = cap;
    for (unsigned i = 0; i < gSt1CN && i < 8; ++i) {
        if (room < kLen) { return; }
        __builtin_memcpy(body + off, kTpl, kLen);
        const UInt32 b = gSt1CRec[i].b % 1000;
        const UInt32 c = gSt1CRec[i].c % 1000;
        const UInt32 s = gSt1CRec[i].seq % 1000;
        body[off + 9]  = static_cast<char>('0' + b / 100);
        body[off + 10] = static_cast<char>('0' + (b / 10) % 10);
        body[off + 11] = static_cast<char>('0' + b % 10);
        body[off + 15] = static_cast<char>('0' + c / 100);
        body[off + 16] = static_cast<char>('0' + (c / 10) % 10);
        body[off + 17] = static_cast<char>('0' + c % 10);
        body[off + 23] = static_cast<char>('0' + s / 100);
        body[off + 24] = static_cast<char>('0' + (s / 10) % 10);
        body[off + 25] = static_cast<char>('0' + s % 10);
        off += kLen;
        room -= kLen;
    }
}

// 257：`[st1p]` client 3 stage1 时刻的 P3 观测行（noinline、零解引用）。
__attribute__((noinline))
static void nredSt1PStamp(char* const body, size_t& off, const size_t cap)
{
    const int n = snprintf(body + off, cap, "[st1p] p3=%llx v18=%llx v28=%llx\n",
                           static_cast<unsigned long long>(gSt1P3Obs),
                           static_cast<unsigned long long>(gSt1P3V18),
                           static_cast<unsigned long long>(gSt1P3V28));
    if (n > 0) { off += (static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : cap; }
}

// 258：`[st1b3]` client 3 分支读到的 P3（noinline、零解引用）。
__attribute__((noinline))
static void nredSt1B3Stamp(char* const body, size_t& off, const size_t cap)
{
    const int n = snprintf(body + off, cap, "[st1b3] p3=%llx\n",
                           static_cast<unsigned long long>(gSt1B3P3));
    if (n > 0) { off += (static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : cap; }
}

// 259：`[st1g2/5/6/7/9]` 各行（仅 -NRedStage1；noinline、零解引用、每行 ≤64 字符）。
__attribute__((noinline))
static void nredSt1GuardsStamp(char* const body, size_t& off, const size_t cap)
{
    size_t room = cap;
    int n = snprintf(body + off, room, "[st1g2] p2=%llx key=%llx p3=%llx\n",
                     static_cast<unsigned long long>(gSt1G2P),
                     static_cast<unsigned long long>(gSt1G2Key),
                     static_cast<unsigned long long>(gSt1P3));
    if (n > 0) { const size_t d = (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
                 off += d; room -= d; }
    n = snprintf(body + off, room, "[st1g5] p5=%llx ip=%llx ipr=%llx\n",
                 static_cast<unsigned long long>(gSt1G5P), static_cast<unsigned long long>(gSt1G5Ip),
                 static_cast<unsigned long long>(gSt1G5Ipr));
    if (n > 0) { const size_t d = (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
                 off += d; room -= d; }
    n = snprintf(body + off, room, "[st1g5s] p=%llx qq=%llx r=%llx s=%llx\n",
                 static_cast<unsigned long long>(gSt1G5P20), static_cast<unsigned long long>(gSt1G5P21),
                 static_cast<unsigned long long>(gSt1G5P22), static_cast<unsigned long long>(gSt1G5P23));
    if (n > 0) { const size_t d = (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
                 off += d; room -= d; }
    // 批次 P：client5 护栏的中间量与判定（三行，各 ≤191 B；只读 BSS 标量，零解引用）
    n = snprintf(body + off, room, "[st1g5x] h8=%llx root=%llx s1=%llx dev=%llx head=%llx v=%u\n",
                 static_cast<unsigned long long>(gSt1G5Ip),
                 static_cast<unsigned long long>(gSt1G5Root),
                 static_cast<unsigned long long>(gSt1G5P23),
                 static_cast<unsigned long long>(gSt1G5Ipr),
                 static_cast<unsigned long long>(gSt1G5Head),
                 static_cast<unsigned>(gSt1G5Verdict));
    if (n > 0) { const size_t d = (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
                 off += d; room -= d; }
    n = snprintf(body + off, room, "[st1g5y] f70=%llx f268=%llx f2b8=%llx\n",
                 static_cast<unsigned long long>(gSt1G5F70),
                 static_cast<unsigned long long>(gSt1G5F268),
                 static_cast<unsigned long long>(gSt1G5F2B8));
    if (n > 0) { const size_t d = (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
                 off += d; room -= d; }
    n = snprintf(body + off, room, "[st1g5z] e88=%llx e90=%llx e98=%llx\n",
                 static_cast<unsigned long long>(gSt1G5E88),
                 static_cast<unsigned long long>(gSt1G5E90),
                 static_cast<unsigned long long>(gSt1G5E98));
    if (n > 0) { const size_t d = (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
                 off += d; room -= d; }
    n = snprintf(body + off, room, "[st1g6] p6=%llx key=%llx\n",
                 static_cast<unsigned long long>(gSt1G6P), static_cast<unsigned long long>(gSt1G6Key));
    if (n > 0) { const size_t d = (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
                 off += d; room -= d; }
    n = snprintf(body + off, room, "[st1g7] p7=%llx p3=%llx\n",
                 static_cast<unsigned long long>(gSt1P7), static_cast<unsigned long long>(gSt1P3));
    if (n > 0) { const size_t d = (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
                 off += d; room -= d; }
    n = snprintf(body + off, room, "[st1g9] p9=%llx key=%llx p3=%llx\n",
                 static_cast<unsigned long long>(gSt1G9P),
                 static_cast<unsigned long long>(gSt1G9Key),
                 static_cast<unsigned long long>(gSt1P3));
    if (n > 0) { const size_t d = (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
                 off += d; room -= d; }
    // 批次 P4：client 8（SMU）观测护栏读数（**仅 -NRedSt1G8**；关态一字不变）
    //   ⚠ 必须单独门控：本 stamp 函数整体只受 `gStage1` 门控，
    //     若不加 `if (gSt1G8)`，`-NRedSt1G8` 关时也会多出这一行 ⇒ 破坏"关态逐字不变"。
    if (gSt1G8) {
        n = snprintf(body + off, room, "[st1g8] p8=%llx f18=%llx f28=%llx v6c8=%llx v6d8=%llx v=%u\n",
                     static_cast<unsigned long long>(gSt1G8P),
                     static_cast<unsigned long long>(gSt1G8F18),
                     static_cast<unsigned long long>(gSt1G8F28),
                     static_cast<unsigned long long>(gSt1G8V6C8),
                     static_cast<unsigned long long>(gSt1G8V6D8),
                     static_cast<unsigned>(gSt1G8Verdict));
        if (n > 0) { const size_t d = (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
                     off += d; room -= d; }
    }
    n = snprintf(body + off, room, "[st1g14] p14=%llx mes=%llx m790=%llx\n",
                 static_cast<unsigned long long>(gSt1G14P), static_cast<unsigned long long>(gSt1Mes),
                 static_cast<unsigned long long>(gSt1M790));
    if (n > 0) { off += (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room; }
}

// 批次 Q′：截断器读数（两行，各 ≤191 B；只读 BSS 标量，零解引用）
__attribute__((noinline))
static void nredQpStamp(char* const body, size_t& off, const size_t cap)
{
    size_t room = cap;
    int n = snprintf(body + off, room, "[qp] n=%u arm=%u ra=%llx c4=%x t20=%x t28=%llx l68=%x\n",
                     static_cast<unsigned>(gQpN),
                     static_cast<unsigned>(gQpArmed ? 1U : 0U),
                     static_cast<unsigned long long>(gQpRa),
                     static_cast<unsigned>(gQpC4),
                     static_cast<unsigned>(gQpT20),
                     static_cast<unsigned long long>(gQpT28),
                     static_cast<unsigned>(gQpL68Ok ? gQpL68 : 0xFFFFFFFFU));
    if (n > 0) { const size_t d = (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
                 off += d; room -= d; }
    n = snprintf(body + off, room, "[qp2] a30=%llx a38=%llx a50=%x a58=%llx\n",
                 static_cast<unsigned long long>(gQpA30),
                 static_cast<unsigned long long>(gQpA38),
                 static_cast<unsigned>(gQpA50),
                 static_cast<unsigned long long>(gQpA58));
    if (n > 0) { const size_t d = (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
                 off += d; room -= d; }
}
// 265：`[st1qq]` client5 qq 对象四字段（noinline、零解引用、≤120 字符）。
__attribute__((noinline))
static void nredSt1QQStamp(char* const body, size_t& off, const size_t cap)
{
    const int n = snprintf(body + off, cap, "[st1qq] q0=%llx q8=%llx q10=%llx q18=%llx f=%u\n",
                           static_cast<unsigned long long>(gSt1QQ0),
                           static_cast<unsigned long long>(gSt1QQ8),
                           static_cast<unsigned long long>(gSt1QQ10),
                           static_cast<unsigned long long>(gSt1QQ18),
                           static_cast<unsigned>(gSt1QQF));
    if (n > 0) { off += (static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : cap; }
}

// 266：`[ttlfd]` TTL 门面安装状态（noinline、零解引用、≤120 字符）。
__attribute__((noinline))
static void nredTtlFdStamp(char* const body, size_t& off, const size_t cap)
{
    const int n = snprintf(body + off, cap, "[ttlfd] root=%llx slot1=%llx slot3=%llx slot4=%llx inst=%u\n",
                           static_cast<unsigned long long>(reinterpret_cast<UInt64>(gTtlFacadeRoot)),
                           static_cast<unsigned long long>(reinterpret_cast<UInt64>(gTtlFacadeSlot1)),
                           static_cast<unsigned long long>(reinterpret_cast<UInt64>(gTtlFacadeSlot3)),
                           static_cast<unsigned long long>(reinterpret_cast<UInt64>(gTtlFacadeSlot4)),
                           static_cast<unsigned>(gTtlFacadeInst));
    if (n > 0) { off += (static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : cap; }
}

// 268：`[qqv]`/`[qqv2]`（探针）与 `[qqh]`/`[qqh2]`（句柄安装）——noinline、零解引用。
//  行长约束：任务卡格式 8 字段约 190 字符 >128 ⇒ 拆两行（字段与含义不变）。
__attribute__((noinline))
static void nredQqStamp(char* const body, size_t& off, size_t cap, const bool probe, const bool hdl)
{
    size_t room = cap;
    int n = 0;
    if (probe) {
        n = snprintf(body + off, room, "[qqv] p=%llx qq=%llx q8=%llx s1=%llx\n",
                     static_cast<unsigned long long>(gQqP), static_cast<unsigned long long>(gQqQ),
                     static_cast<unsigned long long>(gQqQ8), static_cast<unsigned long long>(gQqS1));
        if (n > 0) { const size_t d = (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
                     off += d; room -= d; }
        n = snprintf(body + off, room, "[qqv2] v70=%llx v258=%llx v268=%llx v2b8=%llx\n",
                     static_cast<unsigned long long>(gQqV70), static_cast<unsigned long long>(gQqV258),
                     static_cast<unsigned long long>(gQqV268), static_cast<unsigned long long>(gQqV2B8));
        if (n > 0) { const size_t d = (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
                     off += d; room -= d; }
    }
    if (hdl) {
        n = snprintf(body + off, room, "[qqh] ret=%u p=%llx qq=%llx hstore=%llx old=%llx\n",
                     static_cast<unsigned>(gQqHdlRet), static_cast<unsigned long long>(gQqP),
                     static_cast<unsigned long long>(gQqQ),
                     static_cast<unsigned long long>(reinterpret_cast<UInt64>(gQqHdlStore)),
                     static_cast<unsigned long long>(gQqHdlOld));
        if (n > 0) { const size_t d = (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
                     off += d; room -= d; }
        n = snprintf(body + off, room, "[qqh2] v70=%llx v258=%llx v268=%llx v2b8=%llx\n",
                     static_cast<unsigned long long>(gQqV70), static_cast<unsigned long long>(gQqV258),
                     static_cast<unsigned long long>(gQqV268), static_cast<unsigned long long>(gQqV2B8));
        if (n > 0) { off += (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room; }
    }
}

// 270：`[p7h]` P7 保活状态（noinline、零解引用、≤128 字符）。
__attribute__((noinline))
static void nredP7hStamp(char* const body, size_t& off, const size_t cap)
{
    const int n = snprintf(body + off, cap, "[p7h] p7=%llx cleared=%u skip=%u armed=%u\n",
                           static_cast<unsigned long long>(gP7Seen),
                           static_cast<unsigned>(gP7Cleared),
                           static_cast<unsigned>(gP7Skip),
                           gP7Armed ? 1u : 0u);
    if (n > 0) { off += (static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : cap; }
}

// 271：`[ipseq] cast=0` —— FunctionCast 结果为 0 时置位（头部区、零解引用、≤128）。
__attribute__((noinline))
static void nredIpSeqCastStamp(char* const body, size_t& off, const size_t cap)
{
    const int n = snprintf(body + off, cap, "[ipseq] cast=0\n");
    if (n > 0) { off += (static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : cap; }
}

// 260：`[st1gf]` 守卫首个失败步号（无失败时输出 c=65535 step=0；noinline、零解引用）。
__attribute__((noinline))
static void nredSt1GfStamp(char* const body, size_t& off, const size_t cap)
{
    const unsigned c = (gSt1GfClient == 0xFFFFFFFFU) ? 0xFFFFU : static_cast<unsigned>(gSt1GfClient);
    const int n = snprintf(body + off, cap, "[st1gf] c=%u step=%u\n",
                           c, static_cast<unsigned>(gSt1GfStep));
    if (n > 0) { off += (static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : cap; }
}

// 261：`[gvm5]` client5 GVM type/注册表读数（noinline、零解引用、≤120 字符）。
__attribute__((noinline))
static void nredGvm5Stamp(char* const body, size_t& off, const size_t cap)
{
    // A3（L5）：`-NRedGvm5D` 会被 `checkKernelArgument("-NRedGvm5")` 前缀命中，但 D 的早退
    //  在探针之前 ⇒ 探针未执行。用 `gGvm5Seen` 哨兵避免输出未采样的初值。
    if (!gGvm5Seen) { return; }
    const int n = snprintf(body + off, cap, "[gvm5] type=%x a0=%llx a58=%llx iproot=%llx reg=%u\n",
                           static_cast<unsigned>(gGvm5Type),
                           static_cast<unsigned long long>(gGvm5A0),
                           static_cast<unsigned long long>(gGvm5A58),
                           static_cast<unsigned long long>(gGvm5Iproot),
                           static_cast<unsigned>(gGvm5Reg));
    if (n > 0) { off += (static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : cap; }
}

// 273：`[gvm5d]` client5 stage1 叶复刻结果（noinline、零解引用、≤128 字符）。
//  ret=0xFFFFFFFF 表示叶未跑；step=最后执行的步骤号（失败定位）；busy=1 表示发生过重入。
__attribute__((noinline))
static void nredGvm5DStamp(char* const body, size_t& off, const size_t cap)
{
    const int n = snprintf(body + off, cap, "[gvm5d] ret=%u p1=%llx local68=%x step=%u busy=%u\n",
                           static_cast<unsigned>(gGvm5DRet),
                           static_cast<unsigned long long>(gGvm5DP1),
                           static_cast<unsigned>(gGvm5DLocal68),
                           static_cast<unsigned>(gGvm5DStep),
                           gGvm5DBusyHit ? 1U : 0U);
    if (n > 0) { off += (static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : cap; }
}

// 274：`[gvm5p]` client5 stage1 入口只读探针（noinline、零解引用、≤128 字符）。
//  能力边界：只能回答 D1（start 是否走到 client5）；D2/D3 需 powerUp hook 或 self 路径，本探针拿不到。
__attribute__((noinline))
static void nredGvm5PStamp(char* const body, size_t& off, const size_t cap)
{
    const int n = snprintf(body + off, cap, "[gvm5p] n=%u a=%llx p1=%llx ra=%llx rc=%02x\n",
                           static_cast<unsigned>(gGvm5PhaseN),
                           static_cast<unsigned long long>(gGvm5PhaseA),
                           static_cast<unsigned long long>(gGvm5PhaseP1),
                           static_cast<unsigned long long>(gGvm5PhaseRa),
                           static_cast<unsigned>(gGvm5PhaseRc));
    if (n > 0) { off += (static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : cap; }
}

// 276：`[gvm5ph]` 相位只读标记（noinline、零解引用、≤128 字符）。
//  把 `[gvm5p]` 的调用归属到 start（wrapInitializeTtl）或 powerUp（wrapControllerPowerUp）。
__attribute__((noinline))
static void nredGvm5PhStamp(char* const body, size_t& off, const size_t cap)
{
    size_t room = cap;
    if (gGvm5PhStart != 0) {
        const int n = snprintf(body + off, room, "[gvm5ph] phase=start\n");
        if (n > 0) { const size_t d = (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
                     off += d; room -= d; }
    }
    if (room == 0) { return; }
    if (gGvm5PhPower != 0) {
        const int n = snprintf(body + off, room, "[gvm5ph] phase=powerup\n");
        if (n > 0) { off += (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room; }
    }
}

// 275：`[gvm5e]` M4′ 早期注入两阶段读数（noinline、零解引用、≤128 字符）。
//  phase=start：早期（kernel_task）执行 D 的 rc；phase=powerup：powerUp 阶段是否幂等跳过。
__attribute__((noinline))
static void nredGvm5EStamp(char* const body, size_t& off, const size_t cap)
{
    size_t room = cap;
    if (gGvm5EarlyStartRc == 0) {
        // 失败：panic 前写 `panic=1`（panic 在 X5000::wrapInitializeTtl 内调用）。
        const int n = snprintf(body + off, room, "[gvm5e] phase=start rc=0 panic=1\n");
        if (n > 0) { const size_t d = (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
                     off += d; room -= d; }
    } else if (gGvm5EarlyStartRc > 0) {
        const int n = snprintf(body + off, room, "[gvm5e] phase=start rc=%d idem=%d\n",
                               static_cast<int>(gGvm5EarlyStartRc), 0);
        if (n > 0) { const size_t d = (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
                     off += d; room -= d; }
    }
    if (room == 0) { return; }
    if (gGvm5EarlyPowerSeen) {
        if (gGvm5EarlyPowerIdem != 0) {
            const int n = snprintf(body + off, room, "[gvm5e] phase=powerup rc=%d idem=1\n",
                                   static_cast<int>(gGvm5EarlyPowerRc));
            if (n > 0) { off += (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room; }
        } else if (gGvm5EarlyPowerPanic != 0) {
            // late path D 失败 ⇒ panic 前写 `panic=1`（panic 在 wrapMode9440f 内调用）
            const int n = snprintf(body + off, room, "[gvm5e] phase=powerup idem=0 rc=%d panic=1\n",
                                   static_cast<int>(gGvm5EarlyPowerRc));
            if (n > 0) { off += (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room; }
        } else {
            // p1 不同/非法 ⇒ 未跳过（不得伪装成成功）
            const int n = snprintf(body + off, room, "[gvm5e] phase=powerup idem=0 p1=%llx early=%llx\n",
                                   static_cast<unsigned long long>(gGvm5EarlyPowerP1),
                                   static_cast<unsigned long long>(gGvm5EarlyP1));
            if (n > 0) { off += (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room; }
        }
    }
}

// 262：`[gslot]` client5 6 槽读数（头行 h/t/a/g + 每槽一行；noinline、零解引用）。
__attribute__((noinline))
static void nredGSlotStamp(char* const body, size_t& off, const size_t cap)
{
    size_t room = cap;
    int n = snprintf(body + off, room, "[gslot] h=%llx t=%llx a=%llx g=%x\n",
                     static_cast<unsigned long long>(gGSlotH),
                     static_cast<unsigned long long>(gGSlotT),
                     static_cast<unsigned long long>(gGSlotA),
                     static_cast<unsigned>(gGSlotGolden));
    if (n > 0) { const size_t d = (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
                 off += d; room -= d; }
    for (unsigned k = 0; k < 6; ++k) {
        if (room == 0) { return; }
        const NRedGSlotRec& r = gGSlotRec[k];
        n = snprintf(body + off, room, "[gslot] n=%u t0=%x n0=%u b0=%x b1=%x b2=%x%s\n", k,
                     static_cast<unsigned>(r.type), static_cast<unsigned>(r.n),
                     static_cast<unsigned>(r.b0), static_cast<unsigned>(r.b1),
                     static_cast<unsigned>(r.b2), (r.n > 3) ? " +" : "");
        if (n > 0) { const size_t d = (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
                     off += d; room -= d; }
    }
}

// 263：`[ipr]` IP 根安装结果（noinline、零解引用、≤96 字符）。
__attribute__((noinline))
static void nredIpRStamp(char* const body, size_t& off, const size_t cap)
{
    const int n = snprintf(body + off, cap, "[ipr] ip=%llx root=%llx ret=%u\n",
                           static_cast<unsigned long long>(gIpRIp),
                           static_cast<unsigned long long>(gIpRRoot),   // 批次 P2：真实 iproot
                           static_cast<unsigned>(gIpRRet));
    if (n > 0) { off += (static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : cap; }
}

// 253：`[mes]` MES stage0 真实路径观测行（noinline、零解引用）。
__attribute__((noinline))
static void nredMesStamp(char* const body, size_t& off, const size_t cap)
{
    const int n = snprintf(body + off, cap, "[mes] code=%x p790=%llx p7b0=%llx ret=%d\n",
                           static_cast<unsigned>(gMesCode),
                           static_cast<unsigned long long>(gMesP790),
                           static_cast<unsigned long long>(gMesP7B0),
                           static_cast<int>(gMesRet == 0xFFFFFFFFU ? -1 : static_cast<int>(gMesRet)));
    if (n > 0) { off += (static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : cap; }
}

// 254：`[mesb]` b 系列提供者观测行（noinline、零解引用）。
__attribute__((noinline))
static void nredMesBStamp(char* const body, size_t& off, const size_t cap)
{
    const int n = snprintf(body + off, cap, "[mesb] code=%x p790=%llx v10=%llx\n",
                           static_cast<unsigned>(gMesBCode),
                           static_cast<unsigned long long>(gMesBP790),
                           static_cast<unsigned long long>(gMesBV10));
    if (n > 0) { off += (static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : cap; }
}

int nredTraceFlush(int secTag)
{
    if (rootvnode == nullptr || gTraceRing.count == 0) { return 0; }
    // A-28 风险①处理：flush 用**独立** `stFlushBusy` 互斥（与 L2 的 `stBusy` 分开）⇒
    //  万一 FS 阻塞，只影响本 flush，不会让 L2 周期拍后续全部跳过（唯一存活计时器保住）。
    //  使用 OSCompareAndSwap 原子 test-and-set 防止 flush 重入写坏缓冲。
    if (!OSCompareAndSwap(0, 1, &NvMsgBuf::stFlushBusy())) { return 0; }
    static unsigned fseq = 0;
    char name[96];
    // A-28 风险②处理：文件名带 **slide**（认属主）+ 首行带 **uptime 指纹 + sec**（认本轮/秒级）。
    const uint64_t slide = static_cast<uint64_t>(X5000HWLibs::singleton().getKcSlide());
    snprintf(name, sizeof(name), "/var/log/NRedTraceFlush-s%llx-%03u-%05ds.txt",
             (unsigned long long)slide, fseq, secTag);
    static char body[4 * 1024];
    size_t off = 0;
    // 首行指纹（复用 NvMsgBuf 的既有指纹格式）+ 秒级标签
    off += NvMsgBuf::formatFingerprint(body, sizeof(body));
    // B93③：身份自证行——每片文件首行即"本轮身份"（build=编译时间、boot=启动指纹、seq=片序号）。
    off += static_cast<size_t>(snprintf(body + off, sizeof(body) - off,
                                        "[trace-ident] build=%s %s boot=%llx seq=%u\n",
                                        __DATE__, __TIME__,
                                        static_cast<unsigned long long>(NvMsgBuf::bootUptimeTicks()), fseq));
    off += static_cast<size_t>(snprintf(body + off, sizeof(body) - off, "[flush-stamp] sec=%d\n", secTag));
    // P2 包A：执行确认通道（头部区——"先写、永不被 4096 上限截到"，故每片必现）。
    //  P7j 修回（R8 起 [bringup] 消失的根因＝C2 挂点 0x6720b 未被调用 ⇒ hook 未执行 ⇒
    //  gBringupEnterCount 恒 0；源码自包A 未变）：输出条件放宽为"接口一开关打开"
    //  （`-NRedFwBringup` 在位即必现）——enter=0 表示 hook 未执行（接口一未到执行点），
    //  enter=N 表示执行了 N 次。**未命中形态（接口一关闭）仍不输出** ⇒ 关态逐字不变。
    //  不受任何观测门控；checkKernelArgument 在刷盘安全时点调用（非深栈）。
    if ((gBringupEnterCount != 0 || checkKernelArgument("-NRedFwBringup")) && off < sizeof(body)) {
        const size_t cap = sizeof(body) - off;
        const int n2 = snprintf(body + off, cap,
                                "[bringup] enter=%u step=%u last_error=%d loadToc=%X tmrLoad=%X\n",
                                static_cast<unsigned>(gBringupEnterCount),
                                static_cast<unsigned>(gBringupStep),
                                static_cast<int>(gBringupLastError),
                                static_cast<unsigned>(gBringupLoadToc),
                                static_cast<unsigned>(gBringupTmrLoad));
        if (n2 > 0) { off += (static_cast<size_t>(n2) < cap) ? static_cast<size_t>(n2) : cap; }
    }
    // P7c 包D：`[ttlchain]` 头部戳记（与 `[bringup]` 并列，同处头部区）。
    //  **为什么必须放头部区**：环正文区受 4096 B 硬上限"保头丢尾"约束，上一轮 `ttl-allocB`
    //  等判别行正是落在被截的尾部而丢失（只在 SYSLOG 侧留下）⇒ 判别读数必须落头部区才每片必现。
    //  哨兵控制：三值全为初值（三个判据均未进入）⇒ **不追加本行** ⇒ 未命中形态输出逐字不变。
    //  ⚠️ 只读：只读四个标量（由相应 wrapper / 既有 ttlC 块写入），不新增解引用/SYSLOG。
    //  P7d 包D″：`mHttl` 改为**真值** gMHttl（＝既有 `ttlC: mHttl=` 采到的门面 +0x38）；
    //  此前误用 gLastT30Type5（那是 `0x9440f` 的 t30，与 m_hTtl 无关）。
    if ((gP3 != -1 || gP4 != -1 || gP5 != 0xFFFFFFFFFFFFFFFFULL) && off < sizeof(body)) {
        const size_t cap = sizeof(body) - off;
        const int n3 = snprintf(body + off, cap, "[ttlchain] p3=%d p4=%d p5=%llx mHttl=%llx\n",
                                static_cast<int>(gP3), static_cast<int>(gP4),
                                static_cast<unsigned long long>(gP5),
                                static_cast<unsigned long long>(gMHttl));
        if (n3 > 0) { off += (static_cast<size_t>(n3) < cap) ? static_cast<size_t>(n3) : cap; }
    }
    // P7f 包E″：`[ttlchain4]` 头部戳记（判别读数 v4，与 [bringup]／[ttlchain] 并列）。
    //  ab＝allocB 返回值（由既有 wrapTtlAllocB 写入）；n＝`0x929ac` 调用总计数；
    //  k=RA/sub/al＝第 k 次调用的三值（三槽按到达顺序、不覆盖；第 4 次起只计数）。
    //  ⚠️ 限流硬约束：本函数高频 ⇒ 只写标量（不入环、不打印），判别行不会挤掉别的读数。
    //  哨兵控制：allocB 未被调用（gABRet==-1）且 `0x929ac` 未被调用（gIpSeqN==0）⇒
    //  **不追加本行** ⇒ 未命中形态输出逐字不变。只读标量，无新增解引用/SYSLOG。单行 ≤191。
    if ((gABRet != -1 || gIpSeqN != 0) && off < sizeof(body)) {
        const size_t cap = sizeof(body) - off;
        const int n6 = snprintf(body + off, cap,
                                "[ttlchain4] ab=%d n=%u 1=%llx/%u/%u 2=%llx/%u/%u 3=%llx/%u/%u\n",
                                static_cast<int>(gABRet), static_cast<unsigned>(gIpSeqN),
                                static_cast<unsigned long long>(gSeq1RA), static_cast<unsigned>(gSeq1Sub),
                                static_cast<unsigned>(gSeq1Al),
                                static_cast<unsigned long long>(gSeq2RA), static_cast<unsigned>(gSeq2Sub),
                                static_cast<unsigned>(gSeq2Al),
                                static_cast<unsigned long long>(gSeq3RA), static_cast<unsigned>(gSeq3Sub),
                                static_cast<unsigned>(gSeq3Al));
        if (n6 > 0) { off += (static_cast<size_t>(n6) < cap) ? static_cast<size_t>(n6) : cap; }
    }
    // 227/228：`[ttltrim]` 头部戳记（stage0／stage2 各一行；仅在裁剪真正执行后输出）。
    //  为什么必须放头部区：与 `[ttlchain]` 同理，环正文区尾部会被 4096 B 上限截掉。
    //  关态或未命中 ⇒ `seen==0` ⇒ 不输出，未命中形态逐字不变。
    for (int k = 0; k < 2; ++k) {
        if (gTrimStat[k].seen == 0 || off >= sizeof(body)) { continue; }
        const size_t cap = sizeof(body) - off;
        const int nt = snprintf(body + off, cap,
                                "[ttltrim] st=%u n=%u skip=%u last=%u r12=%u ret=%d bad=%u "
                                "failmask=%08x firstfail=%u nfail=%u\n",
                                static_cast<unsigned>(k * 2), static_cast<unsigned>(gTrimStat[k].n),
                                static_cast<unsigned>(gTrimStat[k].skip),
                                static_cast<unsigned>(gTrimStat[k].last),
                                static_cast<unsigned>(gTrimStat[k].r12),
                                static_cast<int>(gTrimStat[k].ret),
                                static_cast<unsigned>(gTrimStat[k].bad),
                                static_cast<unsigned>(gTrimStat[k].failMask),
                                static_cast<unsigned>(gTrimStat[k].firstFail),
                                static_cast<unsigned>(gTrimStat[k].nFail));
        if (nt > 0) { off += (static_cast<size_t>(nt) < cap) ? static_cast<size_t>(nt) : cap; }
    }
    // 243：`[st1m]` 头部戳记（stage1 预检结果；仅 -NRedSt1Pre 时输出，关态一字不变）。
    //  **零解引用**：只读 gSt1Mask/gSt1CtxOk/gSt1N 三个 BSS 标量；单行约 32 字符。
    if (gSt1Pre && off < sizeof(body)) { nredSt1mStamp(body, off, sizeof(body) - off); }
    // 250：`[st1g]` 开关状态行（**本轮诊断常开**；定位后改回门控）。
    if (off < sizeof(body)) { nredSt1GStamp(body, off, sizeof(body) - off); }
    // 249：`[st1o]` 对齐观测（仅 -NRedSt1O；关态一字不变）。
    if (gSt1O && off < sizeof(body)) { nredSt1OStamp(body, off, sizeof(body) - off); }
    // 244：`[st1r]` 放行观测（仅 -NRedStage1；关态一字不变）。
    if (gStage1 && off < sizeof(body)) { nredSt1rStamp(body, off, sizeof(body) - off); }
    // 245：`[st1v]` 补充观测（仅 -NRedStage1；值来自 guard 内读数，头部零解引用）。
    if (gStage1 && off < sizeof(body)) { nredSt1vStamp(body, off, sizeof(body) - off); }
    // 258：`[st1b3]` client 3 的 P3（仅 -NRedStage1）。
    if (gStage1 && off < sizeof(body)) { nredSt1B3Stamp(body, off, sizeof(body) - off); }
    // 259：`[st1g2/5/6/7/9]` 各 client 的 P/关键指针（仅 -NRedStage1）。
    if (gStage1 && off < sizeof(body)) { nredSt1GuardsStamp(body, off, sizeof(body) - off); }
    // 批次 Q′：截断器读数（仅 -NRedQp；关态一字不变）
    if (gQp && off < sizeof(body)) { nredQpStamp(body, off, sizeof(body) - off); }
    // 265：`[st1qq]` client5 qq 四字段（仅 -NRedSt1Q）。
    if (gSt1Q && off < sizeof(body)) { nredSt1QQStamp(body, off, sizeof(body) - off); }
    // 266：`[ttlfd]` TTL 门面状态（仅 -NRedTtlFacade）。
    if (gTtlFacade && off < sizeof(body)) { nredTtlFdStamp(body, off, sizeof(body) - off); }
    // 268：qq 探针/句柄安装读数（仅对应开关开）。
    if ((gQqProbe || gQqHdl) && off < sizeof(body)) {
        nredQqStamp(body, off, sizeof(body) - off, gQqProbe, gQqHdl);
    }
    // 270：`[p7h]` P7 保活状态（仅 -NRedP7Hold）。
    if (gP7Hold && off < sizeof(body)) { nredP7hStamp(body, off, sizeof(body) - off); }
    // 271：`[ipseq] cast=0`（仅当真的发生过 cast==0）。
    if (gIpSeqCast0 != 0 && off < sizeof(body)) {
        nredIpSeqCastStamp(body, off, sizeof(body) - off);
    }
    // 260：`[st1gf]` 守卫首个失败步号（仅 -NRedStage1）。
    if (gStage1 && off < sizeof(body)) { nredSt1GfStamp(body, off, sizeof(body) - off); }
    // 261：`[gvm5]` client5 GVM type/注册表（仅 -NRedGvm5）。
    if (gGvm5 && off < sizeof(body)) { nredGvm5Stamp(body, off, sizeof(body) - off); }
    // 262：`[gslot]` client5 6 槽（仅 -NRedGSlot）。
    if (gGSlot && off < sizeof(body)) { nredGSlotStamp(body, off, sizeof(body) - off); }
    // 273：`[gvm5d]` client5 stage1 叶复刻结果（仅 -NRedGvm5D）。
    if (gGvm5D && off < sizeof(body)) { nredGvm5DStamp(body, off, sizeof(body) - off); }
    // 274：`[gvm5p]` client5 stage1 入口只读探针（仅 -NRedGvm5Phase）。
    if (gGvm5Phase && off < sizeof(body)) { nredGvm5PStamp(body, off, sizeof(body) - off); }
    // 276：`[gvm5ph]` 相位只读标记（仅 -NRedGvm5Phase）。
    if (gGvm5Phase && off < sizeof(body)) { nredGvm5PhStamp(body, off, sizeof(body) - off); }
    // 275：`[gvm5e]` M4′ 早期注入两阶段读数（仅 -NRedGvm5Early）。
    if (gGvm5Early && off < sizeof(body)) { nredGvm5EStamp(body, off, sizeof(body) - off); }
    // 263：`[ipr]` IP 根安装结果（仅 -NRedIpR）。
    if (gIpR && off < sizeof(body)) { nredIpRStamp(body, off, sizeof(body) - off); }
    // 246：`[st1w]` client 7 指针值行（仅 -NRedSt1W；关态一字不变）。
    if (gSt1W && off < sizeof(body)) { nredSt1WStamp(body, off, sizeof(body) - off); }
    // 248：槽值行独立开关（-NRedSt1W2）；W 开而 W2 关 ⇒ 不 deref svc、不输出这两行。
    if (gSt1W2 && off < sizeof(body)) { nredSt1W2Stamp(body, off, sizeof(body) - off); }
    // 251：`[gc7]/[gc7x]` 双侧观测（仅 -NRedGc7；关态一字不变）。
    if (gGc7 && off < sizeof(body)) { nredGc7Stamp(body, off, sizeof(body) - off); }
    // 252：`[st1b]` 原始调次（仅 -NRedSt1B；关态一字不变）。
    if (gSt1B && off < sizeof(body)) { nredSt1BStamp(body, off, sizeof(body) - off); }
    // 256：`[st1c]` 仅 c>=1 的调次（仅 -NRedSt1C；关态一字不变）。
    if (gSt1C && off < sizeof(body)) { nredSt1CStamp(body, off, sizeof(body) - off); }
    // 257：`[st1p]` client 3 stage1 时刻的 P3 观测（仅 -NRedSt1P；关态一字不变）。
    if (gSt1P && off < sizeof(body)) { nredSt1PStamp(body, off, sizeof(body) - off); }
    // 253：`[mes]` MES stage0 真实路径观测（仅 -NRedMesReal；关态一字不变）。
    if (gMesReal && off < sizeof(body)) { nredMesStamp(body, off, sizeof(body) - off); }
    // 254：`[mesb]` b 系列提供者观测（仅 -NRedMesB；关态一字不变）。
    if (gMesB && off < sizeof(body)) { nredMesBStamp(body, off, sizeof(body) - off); }
    // B95v2 包F：`[nredInsert]` 头部戳记（基础启动序列补入的**执行确认通道**）。
    //  **为什么必须放头部区**：环正文区受 4096 B 硬上限"保头丢尾"约束；判别读数必须落头部区才每片必现。
    //  ★ **O-1 无条件化**（R12 教训）：判据**只看门控是否开**，**不依赖封装是否被调用**。
    //    R12 用 `applied||skip` 这种"跑过才打"的哨兵 ⇒ "未注册"与"未执行"**不可区分**，
    //    定因因此被拖了一轮。⇒ 现在：门控开 ⇒ **每片必有一行**（哪怕 `calls=0`）；
    //    门控关 ⇒ **一字不变**（降级正确性）。
    //    读法：`armed=1` 证明"门控已解析且本行机制在跑"；`calls` 证明"封装被调用过几次"
    //    （`calls=0` ⇒ 落点仍未走到，需再换落点）；`ours` 证明"我方接管了几次"。
    //  ⚠ 只读：只读我方标量（由 `wrapDevLookup` 写入），**不解引用任何 Apple 对象**、
    //     不新增 SYSLOG、不增 panic 文本。单行 ≤191。
    if (gDevTablePatch && off < sizeof(body)) {
        const size_t cap = sizeof(body) - off;
        const int n7 = snprintf(body + off, cap,
                                "[nredInsert] armed=1 calls=%u hit=%u ours=%u skip=%s k=%x/%x/%x\n",
                                static_cast<unsigned>(gInsCalls), static_cast<unsigned>(gInsAppleHit),
                                static_cast<unsigned>(gInsOurs),
                                kInsSkipReason[(gInsSkip < (sizeof(kInsSkipReason) / sizeof(kInsSkipReason[0])))
                                                   ? gInsSkip : 0],
                                static_cast<unsigned>(gInsK1), static_cast<unsigned>(gInsK2),
                                static_cast<unsigned>(gInsK3));
        if (n7 > 0) { off += (static_cast<size_t>(n7) < cap) ? static_cast<size_t>(n7) : cap; }
    }
    // H1 观测集（2026-10-18）：`[ttlbisect]` 头部戳记 —— 把断点二分到 ③`0x9405e` 或 ④`0x929ac`。
    //  **位置**：头部区（保头丢尾 ⇒ 每片必现），紧跟 `[nredInsert]` 之后、环遍历之前。
    //  **只在门控开时产出**（关态一字不变）。
    //  判读（规格 §3）：
    //    · `c4a4==0` ⇒ 判据④ **从未被调用** ⇒ 断在 ③（`0x9405e`）；
    //    · `c4a4>0 ∧ c4ret==0` ⇒ 断在 ④（`0x929ac`）；
    //    · `Ee8≠0 ∧ r28≠0` ⇒ 我方记录已被采纳并写入 `dev+0xE8`（与 `[nredInsert] ours>0` 互证）；
    //    · `site=?` 占多数 ⇒ 返回值语义/基址与反汇编不符 ⇒ 保留证据上报，不擅改判据。
    //    · 行末 `r0b2/r0b2b`／`r14b2/r14b2b`／`recib2/recib2b`＝出口三值的**同点复读**（各配已读标志）；
    //      `…b=1` ⇒ 值可信（即使恰为哨兵形态）；三者与 `r0/r14i/reci` 逐字相等 ⇒ 该区间读链稳定。
    //  ⚠ 零行为：只读我方标量（由三个**既有** wrapper 写入），本处**零解引用**、零 SYSLOG、零 panic 文本。
    if (gTtlBisect && off < sizeof(body)) {
        const size_t cap = sizeof(body) - off;
        const int n8 = snprintf(body + off, cap,
                                "[ttlbisect] c4n=%u c4r=%u c4a4=%u c4b4=%u c4c4=%u site=%c off=%llx "
                                "Ee8=%llx r28=%llx r30=%llx s2c=%u s3c=%u "
                                "p1=%u/%u/%u p2=%u/%u/%u p3=%u/%u/%u "
                                "o3=%llx o4=%llx c14=%llx f112=%llx f600=%llx f616=%llx "
                                "v176=%08x vb=%u a=%u x=%u a2=%u rb=%u "
                                "r14i=%llx ri=%u reci=%llx rc=%u "
                                "r0=%u r0b=%u hit=%u sh=%s r0b2=%u r0b2b=%u r14b2=%llx r14b2b=%u "
                                "recib2=%llx recib2b=%u\n",
                                static_cast<unsigned>(gC4n), static_cast<unsigned>(gC4ret),
                                static_cast<unsigned>(gC4a4), static_cast<unsigned>(gC4b4),
                                static_cast<unsigned>(gC4c4),
                                kC4Site[(gC4site < (sizeof(kC4Site) / sizeof(kC4Site[0]))) ? gC4site : 0],
                                static_cast<unsigned long long>(gC4off),
                                static_cast<unsigned long long>(gBisEe8),
                                static_cast<unsigned long long>(gBisR28),
                                static_cast<unsigned long long>(gBisR30),
                                static_cast<unsigned>(gBisS2c), static_cast<unsigned>(gBisS3c),
                                static_cast<unsigned>(gBis9440f[0].client),
                                static_cast<unsigned>(gBis9440f[0].stage),
                                static_cast<unsigned>(gBis9440f[0].ret),
                                static_cast<unsigned>(gBis9440f[1].client),
                                static_cast<unsigned>(gBis9440f[1].stage),
                                static_cast<unsigned>(gBis9440f[1].ret),
                                static_cast<unsigned>(gBis9440f[2].client),
                                static_cast<unsigned>(gBis9440f[2].stage),
                                static_cast<unsigned>(gBis9440f[2].ret),
                                static_cast<unsigned long long>(gBisO3),
                                static_cast<unsigned long long>(gBisO4),
                                static_cast<unsigned long long>(gBisObj14),
                                static_cast<unsigned long long>(gBisFn112),
                                static_cast<unsigned long long>(gBisFn600),
                                static_cast<unsigned long long>(gBisFn616),
                                static_cast<unsigned>(gBisV176), static_cast<unsigned>(gBisV176Read),
                                static_cast<unsigned>(gBisA), static_cast<unsigned>(gBisX),
                                static_cast<unsigned>(gBisA2), static_cast<unsigned>(gBisA2Read),
                                static_cast<unsigned long long>(gBisR14i), static_cast<unsigned>(gBisR14iRead),
                                static_cast<unsigned long long>(gBisReci), static_cast<unsigned>(gBisReciRead),
                                static_cast<unsigned>(gBisR0), static_cast<unsigned>(gBisR0Read),
                                static_cast<unsigned>(gBisHit),
                                kBisShortReason[(gBisShort < (sizeof(kBisShortReason) / sizeof(kBisShortReason[0])))
                                                    ? gBisShort : 0],
                                static_cast<unsigned>(gBisR0b2), static_cast<unsigned>(gBisR0b2Read),
                                static_cast<unsigned long long>(gBisR14b2),
                                static_cast<unsigned>(gBisR14b2Read),
                                static_cast<unsigned long long>(gBisRecib2),
                                static_cast<unsigned>(gBisRecib2Read));
        if (n8 > 0) { off += (static_cast<size_t>(n8) < cap) ? static_cast<size_t>(n8) : cap; }
    }
    // GVM 软件初始化替换（2026-10-18）：`[gvmsw]` **头部戳记** —— 执行确认通道的**主判据**。
    //  **位置**：头部区（先写、保头丢尾 ⇒ 每片归档必现），紧跟既有 `[ttlbisect]` 之后、环遍历之前。
    //  **为什么必须在头部**（项目经理补充要求，2026-10-18）：本段"保头丢尾"，逐次日志落在尾段，
    //   既有归档里已被实测截断（2 片尾部损坏）⇒ "我方实现有没有被调到"不能只靠尾段日志。
    //  **口径**：`n`＝**全部**调用次数；其余字段＝**最近一次**调用的值（`n=0` ⇒ 从未被调到，
    //   其余字段无意义）。`ins`／`orig`＝安装动作结果码与被替换的 Apple 实现地址（未安装时 orig=0）。
    //  **只在门控开时产出**（关态一字不变；与既有 `[ttlbisect]` 同纪律：只读我方标量、零解引用）。
    //  判读：`n>=1` ⇒ 已被调到；`ins=1` ⇒ 安装生效；`ins∈{2,3,4}` ⇒ 未安装（原因码见
    //   `gGvmSwInsCode` 注释）；`ret=` ⇒ 最近一次返回值（0＝放行；8＝有步骤未完成；7＝门控未开…）。
    if ((gGvmSwInstall || gGvmSwInitPass) && off < sizeof(body)) {
        const size_t capG = sizeof(body) - off;
        const int nG = snprintf(body + off, capG,
                                "[gvmsw] n=%u ins=%u orig=%llx ret=%d i3656=%llx i3664=%llx "
                                "r0=%016llx r1=%016llx o0=%u o8=%llx\n",
                                static_cast<unsigned>(gGvmSwCalls),
                                static_cast<unsigned>(gGvmSwInsCode),
                                static_cast<unsigned long long>(reinterpret_cast<UInt64>(gOrigGvmSwInit)),
                                gGvmSwLast.ret,
                                static_cast<unsigned long long>(gGvmSwLast.i3656),
                                static_cast<unsigned long long>(gGvmSwLast.i3664),
                                static_cast<unsigned long long>(gGvmSwLast.r0),
                                static_cast<unsigned long long>(gGvmSwLast.r1),
                                static_cast<unsigned>(gGvmSwLast.o0),
                                static_cast<unsigned long long>(gGvmSwLast.o8));
        if (nG > 0) { off += (static_cast<size_t>(nG) < capG) ? static_cast<size_t>(nG) : capG; }
    }
    // 软件初始化替换（R60 §1.5）：`[swi]` **头部戳记（带模块号）** —— 与 `[gvmsw]` 同区、**紧跟其后**。
    //  **为什么新增而不改 `[gvmsw]`**：R60 §1.6 第 6 条要求既有观测项语义不变 ⇒ `[gvmsw]` 原格式保留，
    //   本行按 R60 §1.5 的字段规范单列（含 `id`／`got`／`exp`，用于判"是哪一个模块被调到"与安装校验）。
    //  **口径**：`n`＝**该模块号**的调用次数（按 id 计数）；其余字段＝**最近一次**调用的值；
    //   `got`／`exp`＝**最近一次安装动作**读到的原值与期望值（运行期地址；未执行 ⇒ 0）；`res`＝最近一次返回值。
    //  **只在门控开时产出**；只读我方标量、零解引用；追加写入（不覆盖既有戳记）。
    //  ⚠ 长度说明：本行 10 字段，实测约 **169 字符**（最小取值长度已 >96）⇒ 但本行**不做栈缓冲**
    //    （直接 snprintf 进 `body`，容量 4096；与 R60 §8A-4 的"栈缓冲改 static"同口径）。
    if ((gGvmSwInstall || gGvmSwInitPass) && off < sizeof(body)) {
        const size_t capS = sizeof(body) - off;
        const int nS = snprintf(body + off, capS,
                                "[swi] id=%u n=%u got=%llx exp=%llx info=%llx i3656=%llx i3664=%llx "
                                "rec0=%016llx out0=%u res=%d\n",
                                static_cast<unsigned>(gSwLastId),
                                static_cast<unsigned>((gSwLastId < 16) ? gSwCallN[gSwLastId] : 0),
                                static_cast<unsigned long long>(gSwInsGot),
                                static_cast<unsigned long long>(gSwInsExp),
                                static_cast<unsigned long long>(gSwLastCall.info),
                                static_cast<unsigned long long>(gSwLastCall.i3656),
                                static_cast<unsigned long long>(gSwLastCall.i3664),
                                static_cast<unsigned long long>(gSwLastCall.rec0),
                                static_cast<unsigned>(gSwLastCall.out0),
                                gSwLastCall.res);
        if (nS > 0) { off += (static_cast<size_t>(nS) < capS) ? static_cast<size_t>(nS) : capS; }
    }
    // R90：`[swihand]` **头部戳记（句柄三字段 ＋ 句柄地址的真实值）** —— 紧跟 `[swi]` 之后、同一门控下。
    //  **目的**：**零风险**拿到清理路径 `19cc5 callq *24(%rcx)` 的目标地址（＝`slot1+24` 的值）与句柄本体，
    //   供**离线**定位 `*(slot1+24)` 的身份（R82 §2：该字段"从未被任何归档 dump 过"）。
    //  **口径**：`n`＝模块 5 的调用次数（`gSwCallN[5]`）；其余字段＝**最近一次**（S0 守卫处留存；未走到 ⇒ 0）。
    //  ⚠ 只读我方标量、**零解引用**；既有 `[gvmsw]`／`[swi]` 格式一字不改（本卡**新增独立一行**）。
    if ((gGvmSwInstall || gGvmSwInitPass) && off < sizeof(body)) {
        const size_t capH = sizeof(body) - off;
        const int nH = snprintf(body + off, capH,
                                "[swihand] n=%u slot1=%llx f8=%llx f16=%llx f24=%llx handle=%llx\n",
                                static_cast<unsigned>(gSwCallN[5]),
                                static_cast<unsigned long long>(gSwS1Slot),
                                static_cast<unsigned long long>(gSwS1f8),
                                static_cast<unsigned long long>(gSwS1f16),
                                static_cast<unsigned long long>(gSwS1f24),
                                static_cast<unsigned long long>(gSwHandleAddr));
        if (nH > 0) { off += (static_cast<size_t>(nH) < capH) ? static_cast<size_t>(nH) : capH; }
    }
    // R105：`[swinfo]` **入参两字段戳记** —— 紧跟 `[swihand]` 之后、同一门控下。
    //  `i12`＝`*(u32*)(info+12)`（模块 2 的苹果实现以它 ==1 作判别，本机实值未知）；
    //  `i8`＝`*(u32*)(info+8)`；`c`＝本次送达的模块号（既有 `gSwLastId`）。
    //  ⚠ 只读我方标量、**零解引用**；`ffffffff`＝从未读到（哨兵）；既有各行格式一字不改。
    if ((gGvmSwInstall || gGvmSwInitPass) && off < sizeof(body)) {
        const size_t capI = sizeof(body) - off;
        const int nI = snprintf(body + off, capI, "[swinfo] i12=%08x i8=%08x c=%d\n",
                                static_cast<unsigned>(gSwInfo12),
                                static_cast<unsigned>(gSwInfo8),
                                static_cast<int>(gSwLastId));
        if (nI > 0) { off += (static_cast<size_t>(nI) < capI) ? static_cast<size_t>(nI) : capI; }
    }
    // R98 §1（R99 S1）：**按模块结果戳记** —— 与既有 `[gvmsw]`／`[swi]`／`[swihand]` 同区、同一门控。
    //  **口径**：`r`＝16 个模块**最近一次**我方实现的返回值（`ff`＝从未被调到；`kSwRes*` 最大 8 ⇒ 不与 `ff` 混淆）；
    //   `n`＝16 个模块的调用次数（**饱和 255**）。两串均为 16×2 位小写 hex，顺序＝模块号 0..15。
    //  ⚠ 只读我方 BSS 标量、**零解引用**；hex 缓冲为 **static**（不占栈）；既有各行格式一字不改。
    if ((gGvmSwInstall || gGvmSwInitPass) && off < sizeof(body)) {
        const size_t capR = sizeof(body) - off;
        static char rHex[33];
        static char nHex[33];
        static const char kHexDig[] = "0123456789abcdef";
        for (UInt32 i = 0; i < 16; i += 1) {
            const UInt8 rv = gSwResById[i]; rHex[i * 2] = kHexDig[rv >> 4]; rHex[i * 2 + 1] = kHexDig[rv & 0xFU];
            const UInt8 nv = gSwCntById[i]; nHex[i * 2] = kHexDig[nv >> 4]; nHex[i * 2 + 1] = kHexDig[nv & 0xFU];
        }
        rHex[32] = '\0';
        nHex[32] = '\0';
        const int nR = snprintf(body + off, capR, "[swres] r=%s n=%s\n", rHex, nHex);
        if (nR > 0) { off += (static_cast<size_t>(nR) < capR) ? static_cast<size_t>(nR) : capR; }
    }
    // R119／R121：`[swprobe9]` —— 模块 9 只读探针的 6 个接口表指针读数，**紧跟 `[swres]` 之后**。
    //  **产出条件＝`gProbe9N != 0`**（**比"跟随门控"更严**：只有探针真被调过才产出）⇒ 关态一行不多。
    //  `ffffffffffffffff`＝该项**未读到**（`A`／`B` 形状非法时的哨兵；真实函数指针不可能等于它）。
    //  ⚠ 只读我方 BSS 标量、**零解引用**；既有各行格式一字不改。
    if (gProbe9N != 0 && off < sizeof(body)) {
        const size_t capP = sizeof(body) - off;
        const int np = snprintf(body + off, capP,
            "[swprobe9] n=%u a16=%llx a24=%llx b16=%llx b32=%llx b56=%llx b64=%llx\n",
            static_cast<unsigned>(gProbe9N),
            static_cast<unsigned long long>(gProbe9[0]), static_cast<unsigned long long>(gProbe9[1]),
            static_cast<unsigned long long>(gProbe9[2]), static_cast<unsigned long long>(gProbe9[3]),
            static_cast<unsigned long long>(gProbe9[4]), static_cast<unsigned long long>(gProbe9[5]));
        if (np > 0) { off += (static_cast<size_t>(np) < capP) ? static_cast<size_t>(np) : capP; }
    }
    // R175：`[swvcn]`／`[swvcn2]` —— 模块 9 实实现的逐次读数＋接口指针归位用 slide。
    //  产出条件＝`gVcnN != 0`；两行各自 snprintf 到 flush body（与 `[swprobe9]` 同区）。
    if (gVcnN != 0 && off < sizeof(body)) {
        const size_t capV = sizeof(body) - off;
        const int nv = snprintf(body + off, capV,
            "[swvcn] n=%u st=%02x o8=%llx l10=%llx l18=%llx l30=%llx\n",
            static_cast<unsigned>(gVcnN), static_cast<unsigned>(gVcnStep),
            static_cast<unsigned long long>(gVcnO8),
            static_cast<unsigned long long>(gVcnL10),
            static_cast<unsigned long long>(gVcnL18),
            static_cast<unsigned long long>(gVcnL30));
        if (nv > 0) { off += (static_cast<size_t>(nv) < capV) ? static_cast<size_t>(nv) : capV; }
    }
    if (gVcnN != 0 && off < sizeof(body)) {
        const size_t capV2 = sizeof(body) - off;
        const int nv2 = snprintf(body + off, capV2,
            "[swvcn2] slide=%llx a16=%llx a24=%llx b16=%llx b32=%llx b56=%llx b64=%llx\n",
            static_cast<unsigned long long>(gHwlibsSlide),
            static_cast<unsigned long long>(gProbe9[0]), static_cast<unsigned long long>(gProbe9[1]),
            static_cast<unsigned long long>(gProbe9[2]), static_cast<unsigned long long>(gProbe9[3]),
            static_cast<unsigned long long>(gProbe9[4]), static_cast<unsigned long long>(gProbe9[5]));
        if (nv2 > 0) { off += (static_cast<size_t>(nv2) < capV2) ? static_cast<size_t>(nv2) : capV2; }
    }
    // R129／R143：`[swm8]` —— 模块 8 **能力位快照 ＋ 两处关键字段**（R128 §4.3／§6-X1 引入；语义按 D2 更新）。
    //  **产出条件＝`gM8N != 0`**（比"跟随门控"更严：只有我方模块 8 本体**真被进入**才产出）⇒ 关态一行不多。
    //  `ffffffff`／`ffffffffffffffff`＝该项**未读到**（哨兵；真实值不可能等于它）。
    //  **D2 判读**：`v612`＝能力位快照（历史判别式；`[20]`/`[23]` 已删 ⇒ 不再用于区分停点）；
    //    `h712` 应＝**0**（`0x66d84` 不再执行 ⇒ 本世代标识未写，`bit29`）；`h1728` 由 `#20a` 装表（0 ⇒ 装表未成）。
    //  ⚠ 只读我方 BSS 标量、**零解引用**；既有各行格式一字不改。
    if (gM8N != 0 && off < sizeof(body)) {
        const size_t capM = sizeof(body) - off;
        const int nm = snprintf(body + off, capM, "[swm8] n=%u v612=%08x h712=%08x h1728=%llx\n",
                                static_cast<unsigned>(gM8N),
                                static_cast<unsigned>(gM8V612),
                                static_cast<unsigned>(gM8H712),
                                static_cast<unsigned long long>(gM8H1728));
        if (nm > 0) { off += (static_cast<size_t>(nm) < capM) ? static_cast<size_t>(nm) : capM; }
    }
    // R143（D2）：`[swhw8]` —— **模块 8 的硬件侧/回读观测**（R141 §4 ＋ 复核 Q6），紧跟 `[swm8]` 之后。
    //  **产出条件＝`gM8N != 0`**（模块 8 本体真被进入）⇒ 关态一行不多；`ff`／`ffffffff`／`ffffffffffffffff`＝**未读到**。
    //  ⚠ **R145/N-1 缩短**：**去掉**与 `[swm8]` 重复的 `h712`／`h1728` 两字段（同一 boot 只在其一处更省）⇒
    //    行长落回 ≤191；`h1736`（**槽 1**）与 `h1960`（**槽 29**）**必须保留**（"槽是否被填"的直接读数）。
    //  判读要点：`h720` 应＝**0**（**D2 不设 `h+720`**，R141 §3.2——不得宣告"SMU 上下文已就绪"）；
    //    `h888/896/904/912` 应＝**0**（D2-1 清零后的回读，＝D2-2 的机械证据）；
    //    `h1712 ≠ 0`（子对象已建）；`h1736`（**槽 1**）／`h1960`（**槽 29**）为 `0` ⇒ "槽 1..29 未装"／
    //    "uCode 常量未取得"**坐实**（复核 N-2／Q6）；`h712`／`h1728` 的判读见上一行 `[swm8]`。
    //  ⚠ **不含** `*(*(h+16)+72)` 的解引用（复核 Q6 明确禁止）；只读我方 BSS、零解引用。
    if (gM8N != 0 && off < sizeof(body)) {
        const size_t capW = sizeof(body) - off;
        const int nw = snprintf(body + off, capW,
            "[swhw8] n=%u h720=%02x h888=%llx h896=%llx h904=%llx h912=%llx "
            "h1712=%llx h1736=%llx h1960=%llx\n",
            static_cast<unsigned>(gM8N),
            static_cast<unsigned>(gM8H720),
            static_cast<unsigned long long>(gM8H888), static_cast<unsigned long long>(gM8H896),
            static_cast<unsigned long long>(gM8H904), static_cast<unsigned long long>(gM8H912),
            static_cast<unsigned long long>(gM8H1712),
            static_cast<unsigned long long>(gM8H1736), static_cast<unsigned long long>(gM8H1960));
        if (nw > 0) { off += (static_cast<size_t>(nw) < capW) ? static_cast<size_t>(nw) : capW; }
    }
    const uint32_t n = gTraceRing.count;
    for (uint32_t i = 0; i < n; ++i) {
        const char* line = nullptr;
        const uint32_t len = gTraceRing.get(i, &line);
        if (len == 0 || line == nullptr) { continue; }
        if (off + len + 1 > sizeof(body)) { break; }
        __builtin_memcpy(body + off, line, len);
        off += len;
        body[off++] = '\n';
    }
    if (off == 0) { NvMsgBuf::stFlushBusy() = false; return 0; }
    // B94 修复（s0-006 实测 4122B>4096 且 [flush-t30] 乱码）：snprintf 返回**应写长度**（截断时仍返回
    //  完整长度）⇒ `off += snprintf(...)` 在 off 接近 4096 时越界累加 ⇒ 越界写 body、FileIO 读越界。
    //  ⇒ 改用"实际写入长度"（min(返回值, 余量)）累加，off 恒 ≤ sizeof(body)，文件大小 ≤ 4096。
    if (off < sizeof(body)) {
        const size_t cap = sizeof(body) - off;
        const int fsN = snprintf(body + off, cap, "[flush-stamp] sec=%d overflow=%u t30=%llx\n",
                                 secTag, static_cast<unsigned>(gTraceRing.overflow),
                                 static_cast<unsigned long long>(gLastT30Type5));
        if (fsN > 0) { off += (static_cast<size_t>(fsN) < cap) ? static_cast<size_t>(fsN) : cap; }
    }
    if (off < sizeof(body)) {
        const size_t cap = sizeof(body) - off;
        const int ftN = snprintf(body + off, cap,
                                 "[flush-t30] type3=%llx type8=%llx type5=%llx sub5=%08x\n",
                                 static_cast<unsigned long long>(gLastT30Type3),
                                 static_cast<unsigned long long>(gLastT30Type8),
                                 static_cast<unsigned long long>(gLastT30Type5),
                                 static_cast<unsigned>(gLastSub5));
        if (ftN > 0) { off += (static_cast<size_t>(ftN) < cap) ? static_cast<size_t>(ftN) : cap; }
    }
    // B93①（B77① 截断函数补上调用——此前定义了却未调用，致旧轮内容残留）：
    //  写前显式把目标文件长度截为 0（FileIO::writeBufferToFile 的 O_TRUNC 实测不被 honor）。
    nredTraceTruncateFile(name);
    const int err = FileIO::writeBufferToFile(name, body, off);
    if (err == 0) {
        gTraceRing.reset();
        ++fseq;
        NvMsgBuf::stFlushBusy() = false;
        return static_cast<int>(n);
    }
    NvMsgBuf::stFlushBusy() = false;
    return 0;
}
// 无参兼容封装（既有调用点 X6000FB.cpp 曾用）：秒级标签取"当前拍序号×kTickSecs"。
int nredTraceFlush() { return nredTraceFlush(NvMsgBuf::stTick() * NvMsgBuf::kTickSecs); }



#include <kern/assert.h>
#include <kern/debug.h>   // 276：panic()（late path D 失败主动断言）
#include <libkern/OSTypes.h>
#include <libkern/c++/OSBoolean.h>
#include <mach/i386/vm_types.h>
#include <mach/kern_return.h>

static const char ativvaxy_rv_dat[] = {
#embed "Firmware/ativvaxy_rv.dat"
};
static const char ativvaxy_nv_dat[] = {
#embed "Firmware/ativvaxy_nv.dat"
};
static const char atidmcub_rn_dat[] = {
#embed "Firmware/atidmcub_rn.dat"
};

static const char _dmcu_eram_dcn10_abm_2_1[] = {
#embed "Firmware/dmcu_eram_dcn10_abm_2_1.bin"
};
DMCU_FW_CONSTANT(0x100, dmcu_eram_dcn10_abm_2_1);
static const char _dmcu_eram_dcn10_abm_2_2[] = {
#embed "Firmware/dmcu_eram_dcn10_abm_2_2.bin"
};
DMCU_FW_CONSTANT(0x100, dmcu_eram_dcn10_abm_2_2);
static const char _dmcu_eram_dcn10_abm_2_3[] = {
#embed "Firmware/dmcu_eram_dcn10_abm_2_3.bin"
};
DMCU_FW_CONSTANT(0x100, dmcu_eram_dcn10_abm_2_3);
static const char _dmcu_eram_dcn21_abm_2_1[] = {
#embed "Firmware/dmcu_eram_dcn21_abm_2_1.bin"
};
DMCU_FW_CONSTANT(0x100, dmcu_eram_dcn21_abm_2_1);
static const char _dmcu_eram_dcn21_abm_2_2[] = {
#embed "Firmware/dmcu_eram_dcn21_abm_2_2.bin"
};
DMCU_FW_CONSTANT(0x100, dmcu_eram_dcn21_abm_2_2);
static const char _dmcu_eram_dcn21_abm_2_3[] = {
#embed "Firmware/dmcu_eram_dcn21_abm_2_3.bin"
};
DMCU_FW_CONSTANT(0x100, dmcu_eram_dcn21_abm_2_3);
static const char _dmcu_eram_dcn21_abm_2_4[] = {
#embed "Firmware/dmcu_eram_dcn21_abm_2_4.bin"
};
DMCU_FW_CONSTANT(0x100, dmcu_eram_dcn21_abm_2_4);
static const char _dmcu_intvectors_dcn10_abm_2_1[] = {
#embed "Firmware/dmcu_intvectors_dcn10_abm_2_1.bin"
};
DMCU_FW_CONSTANT(0xFFE0, dmcu_intvectors_dcn10_abm_2_1);
static const char _dmcu_intvectors_dcn10_abm_2_2[] = {
#embed "Firmware/dmcu_intvectors_dcn10_abm_2_2.bin"
};
DMCU_FW_CONSTANT(0xFFE0, dmcu_intvectors_dcn10_abm_2_2);
static const char _dmcu_intvectors_dcn10_abm_2_3[] = {
#embed "Firmware/dmcu_intvectors_dcn10_abm_2_3.bin"
};
DMCU_FW_CONSTANT(0xFFE0, dmcu_intvectors_dcn10_abm_2_3);
static const char _dmcu_intvectors_dcn21_abm_2_1[] = {
#embed "Firmware/dmcu_intvectors_dcn21_abm_2_1.bin"
};
DMCU_FW_CONSTANT(0xFFE0, dmcu_intvectors_dcn21_abm_2_1);
static const char _dmcu_intvectors_dcn21_abm_2_2[] = {
#embed "Firmware/dmcu_intvectors_dcn21_abm_2_2.bin"
};
DMCU_FW_CONSTANT(0xFFE0, dmcu_intvectors_dcn21_abm_2_2);
static const char _dmcu_intvectors_dcn21_abm_2_3[] = {
#embed "Firmware/dmcu_intvectors_dcn21_abm_2_3.bin"
};
DMCU_FW_CONSTANT(0xFFE0, dmcu_intvectors_dcn21_abm_2_3);
static const char _dmcu_intvectors_dcn21_abm_2_4[] = {
#embed "Firmware/dmcu_intvectors_dcn21_abm_2_4.bin"
};
DMCU_FW_CONSTANT(0xFFE0, dmcu_intvectors_dcn21_abm_2_4);

static const char _gc_9_1_ce_ucode[] = {
#embed "Firmware/gc_9_1_ce_ucode.bin"
};
GC_FW_CONSTANT("#80", 0x36, 0x800, 0x60, 0x1, 0x0, gc_9_1_ce_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_1_me_ucode[] = {
#embed "Firmware/gc_9_1_me_ucode.bin"
};
GC_FW_CONSTANT("#167", 0x36, 0x1000, 0x60, 0x1, 0x0, gc_9_1_me_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_1_mec_jt_ucode[] = {
#embed "Firmware/gc_9_1_mec_jt_ucode.bin"
};
GC_FW_CONSTANT("#480", 0x36, 0x10000, 0x0, 0x1, 0x0, gc_9_1_mec_jt_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_1_mec_ucode[] = {
#embed "Firmware/gc_9_1_mec_ucode.bin"
};
GC_FW_CONSTANT("#480", 0x36, 0x0, 0x0, 0x0, 0x0, gc_9_1_mec_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_1_pfp_ucode[] = {
#embed "Firmware/gc_9_1_pfp_ucode.bin"
};
GC_FW_CONSTANT("#196", 0x36, 0x1400, 0x60, 0x1, 0x0, gc_9_1_pfp_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_1_rlc_srlist_cntl[] = {
#embed "Firmware/gc_9_1_rlc_srlist_cntl.bin"
};
GC_FW_CONSTANT("#1", 0x1, 0x0, 0x0, 0x1, 0x0, gc_9_1_rlc_srlist_cntl, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_1_rlc_srlist_gpm_mem[] = {
#embed "Firmware/gc_9_1_rlc_srlist_gpm_mem.bin"
};
GC_FW_CONSTANT("#1", 0x1, 0x0, 0x0, 0x1, 0x0, gc_9_1_rlc_srlist_gpm_mem, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_1_rlc_srlist_srm_mem[] = {
#embed "Firmware/gc_9_1_rlc_srlist_srm_mem.bin"
};
GC_FW_CONSTANT("#1", 0x1, 0x0, 0x0, 0x1, 0x0, gc_9_1_rlc_srlist_srm_mem, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_1_rlc_ucode[] = {
#embed "Firmware/gc_9_1_rlc_ucode.bin"
};
GC_FW_CONSTANT("#110", 0x1, 0x1000, 0x0, 0x1, 0x0, gc_9_1_rlc_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_1_rlc_ucode_a0[] = {
#embed "Firmware/gc_9_1_rlc_ucode_a0.bin"
};
GC_FW_CONSTANT("#568", 0x1, 0x1000, 0x0, 0x1, 0x0, gc_9_1_rlc_ucode_a0, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_1_rlcv_ucode[] = {
#embed "Firmware/gc_9_1_rlcv_ucode.bin"
};
GC_FW_CONSTANT("#28", 0x1, 0x800, 0x0, 0x1, 0x0, gc_9_1_rlcv_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_2_ce_ucode[] = {
#embed "Firmware/gc_9_2_ce_ucode.bin"
};
GC_FW_CONSTANT("#80", 0x35, 0x800, 0x60, 0x1, 0x0, gc_9_2_ce_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_2_me_ucode[] = {
#embed "Firmware/gc_9_2_me_ucode.bin"
};
GC_FW_CONSTANT("#166", 0x35, 0x1000, 0x60, 0x1, 0x0, gc_9_2_me_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_2_mec_jt_ucode[] = {
#embed "Firmware/gc_9_2_mec_jt_ucode.bin"
};
GC_FW_CONSTANT("#480", 0x36, 0x0, 0x0, 0x1, 0x0, gc_9_2_mec_jt_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_2_mec_ucode[] = {
#embed "Firmware/gc_9_2_mec_ucode.bin"
};
GC_FW_CONSTANT("#480", 0x36, 0x0, 0x0, 0x0, 0x0, gc_9_2_mec_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_2_pfp_ucode[] = {
#embed "Firmware/gc_9_2_pfp_ucode.bin"
};
GC_FW_CONSTANT("#196", 0x36, 0x1400, 0x60, 0x1, 0x0, gc_9_2_pfp_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_2_rlc_srlist_cntl[] = {
#embed "Firmware/gc_9_2_rlc_srlist_cntl.bin"
};
GC_FW_CONSTANT("#1", 0x1, 0x0, 0x0, 0x1, 0x0, gc_9_2_rlc_srlist_cntl, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_2_rlc_srlist_gpm_mem[] = {
#embed "Firmware/gc_9_2_rlc_srlist_gpm_mem.bin"
};
GC_FW_CONSTANT("#1", 0x1, 0x0, 0x0, 0x1, 0x0, gc_9_2_rlc_srlist_gpm_mem, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_2_rlc_srlist_srm_mem[] = {
#embed "Firmware/gc_9_2_rlc_srlist_srm_mem.bin"
};
GC_FW_CONSTANT("#1", 0x1, 0x0, 0x0, 0x1, 0x0, gc_9_2_rlc_srlist_srm_mem, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_2_rlc_ucode[] = {
#embed "Firmware/gc_9_2_rlc_ucode.bin"
};
GC_FW_CONSTANT("#73", 0x1, 0x1000, 0x0, 0x1, 0x0, gc_9_2_rlc_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_2_rlcv_ucode[] = {
#embed "Firmware/gc_9_2_rlcv_ucode.bin"
};
GC_FW_CONSTANT("#28", 0x1, 0x800, 0x0, 0x1, 0x0, gc_9_2_rlcv_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_3_ce_ucode[] = {
#embed "Firmware/gc_9_3_ce_ucode.bin"
};
GC_FW_CONSTANT("#80", 0x36, 0x800, 0x60, 0x1, 0x0, gc_9_3_ce_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_3_me_ucode[] = {
#embed "Firmware/gc_9_3_me_ucode.bin"
};
GC_FW_CONSTANT("#167", 0x36, 0x1000, 0x60, 0x1, 0x0, gc_9_3_me_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_3_mec_jt_ucode[] = {
#embed "Firmware/gc_9_3_mec_jt_ucode.bin"
};
GC_FW_CONSTANT("#480", 0x36, 0x10000, 0x0, 0x1, 0x0, gc_9_3_mec_jt_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_3_mec_ucode[] = {
#embed "Firmware/gc_9_3_mec_ucode.bin"
};
GC_FW_CONSTANT("#480", 0x36, 0x0, 0x0, 0x0, 0x0, gc_9_3_mec_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_3_pfp_ucode[] = {
#embed "Firmware/gc_9_3_pfp_ucode.bin"
};
GC_FW_CONSTANT("#196", 0x36, 0x1400, 0x60, 0x1, 0x0, gc_9_3_pfp_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_3_rlc_srlist_cntl[] = {
#embed "Firmware/gc_9_3_rlc_srlist_cntl.bin"
};
GC_FW_CONSTANT("#1", 0x1, 0x0, 0x0, 0x1, 0x0, gc_9_3_rlc_srlist_cntl, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_3_rlc_srlist_gpm_mem[] = {
#embed "Firmware/gc_9_3_rlc_srlist_gpm_mem.bin"
};
GC_FW_CONSTANT("#1", 0x1, 0x0, 0x0, 0x1, 0x0, gc_9_3_rlc_srlist_gpm_mem, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_3_rlc_srlist_srm_mem[] = {
#embed "Firmware/gc_9_3_rlc_srlist_srm_mem.bin"
};
GC_FW_CONSTANT("#1", 0x1, 0x0, 0x0, 0x1, 0x0, gc_9_3_rlc_srlist_srm_mem, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_3_rlc_ucode[] = {
#embed "Firmware/gc_9_3_rlc_ucode.bin"
};
GC_FW_CONSTANT("#60", 0x1, 0x1000, 0x0, 0x1, 0x0, gc_9_3_rlc_ucode, 0x0, 0x0, 0x0, 0x0);

static const char psp_asd_bin[] = {
#embed "Firmware/psp_asd.bin"
};
static const char psp_auc_bin[] = {
#embed "Firmware/psp_auc.bin"
};
static const char psp_dtm_bin[] = {
#embed "Firmware/psp_dtm.bin"
};
static const char psp_fp_bin[] = {
#embed "Firmware/psp_fp.bin"
};
static const char psp_hdcp_bin[] = {
#embed "Firmware/psp_hdcp.bin"
};

static const char _sdma_4_1_ucode[] = {
#embed "Firmware/sdma_4_1_ucode.bin"
};
SDMA_FW_CONSTANT("40", sdma_4_1_ucode, 0x29, 0x0, 0x0);

static const UInt8 kDeviceTypeTablePattern[] = {0x60, 0x68, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x61, 0x68, 0x00, 0x00,
                                                0x00, 0x00, 0x00, 0x00, 0x62, 0x68, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                0x63, 0x68, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x68, 0x00, 0x00,
                                                0x00, 0x00, 0x00, 0x00, 0x67, 0x68, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static const UInt8 kCreateFirmwarePattern[]     = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54,
                                                   0x53, 0x40, 0x89, 0xC0, 0x41, 0x89, 0xD0, 0x41, 0x89, 0xF0,
                                                   0x40, 0x89, 0xF0, 0xBF, 0x20, 0x00, 0x00, 0x00, 0xE8};
// P7c 包D：判据③ `0x9405e` / 判据④ `0x929ac`（allocB 四道判据的末两道；L2-j定因 §D.2）。
//  锚点取 24B（**必须**：两者 16B 序言在 __TEXT.__text 内分别命中 245/164 次，不唯一）。
//  等价核对（hook-check 本机不可用）：24B 各命中 1 次且位于函数头（tmp/prog/pattern-check-p7d.py）。
//  ⚠️ `0x9440f`（判据④内部子函数）**已由既有 wrapMode9440f 挂钩**（route 标签 mode9440f），
//     不重复挂；其 ret/type 由该既有 wrapper 内记入全局供头部戳记使用。
static const UInt8 kIpiInitPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                        0x53, 0x48, 0x83, 0xEC, 0x18, 0x85, 0xF6, 0x74, 0x60, 0x49, 0x89, 0xD7};
static const UInt8 kIpEntrySeqPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                           0x53, 0x48, 0x83, 0xEC, 0x28, 0x49, 0x89, 0xD6, 0x41, 0x89, 0xF7, 0x49};
// P7f 包E″：不再新增 wrapAllocB 钩子（0x928f4 已被既有 wrapTtlAllocB 占位），
//  kAllocBPattern 故删除。
static const UInt8 kCreateFirmwarePatternMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                   0xFF, 0xF0, 0xFF, 0xF0, 0xFF, 0xFF, 0xF0, 0xFF, 0xFF, 0xF0,
                                                   0xF0, 0xFF, 0xF0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static const UInt8 kPutFirmwarePattern[] = {0x55, 0x48, 0x89, 0xE5, 0x83, 0xFE, 0x08, 0x7F};

// 第八步观测：TTL 的跨 kext 查询函数（HWLibs 归零 vm 0x90ddc；`0x8b5de` 内三次调用它 =
//  失败出口 2 的判据）。入口字节：push rbp; mov rsp,rbp; push r15/r14/r13/r12/rbx; sub …
static const UInt8 kTtlQueryPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                         0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x38, 0x48, 0x85, 0xFF,
                                         0x0F, 0x84, 0xBB, 0x00};
// B81 观测点 A：Apple 发送入口 0x683bd 的就绪谓词 0x6839e（读 C2PMSG_90(0x29A)→setne→ret al）。
//  入口字节：push rbp; mov rsp,rbp; mov $0x29A,%esi; xor edx,edx; mov $4,%ecx; xor r8d,r8d; call 0x65530。
//  末 4 字节＝call rel32（0x65530 的 rel32＝7A D1 FF FF，随二进制版本变 ⇒ mask 通配）。
//  hook-check：pattern 唯一@0x6839e ✓、func_head ✓、CALL xref ✗（仅 4 处 DATA 取址——
//  谓词以函数指针经 0x64f12 间接调用，无直接 callq；routeFunction 改入口对取址调用同样生效）。
static const UInt8 kPred90Pattern[]     = {0x55, 0x48, 0x89, 0xE5, 0xBE, 0x9A, 0x02, 0x00, 0x00, 0x31, 0xD2,
                                           0xB9, 0x04, 0x00, 0x00, 0x00, 0x45, 0x31, 0xC0, 0xE8, 0x7A, 0xD1,
                                           0xFF, 0xFF};
static const UInt8 kPred90PatternMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                           0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
                                           0x00, 0x00};
// B88 观测 R2'（95bda 收口规格 §3.1）：Apple 0x93f64（查位过滤器）。
//  入口字节：push rbp; mov rsp,rbp; movl 1976(%rdi),%eax; btl %esi,%eax; setb %al; pop rbp; ret
//  语义：return (*(UInt32*)(obj+0x7B8) >> id) & 1（bm 查位；id=5 为关注位）。
//  等价核对（hook-check 不可用）：__TEXT.__text 内 18B 唯一 1 次@0x93f64（函数头✓，llvm-objdump+od 双证），
//  callq 0x93f64 全库 1 处（0x95c7c）；交叉口径 0x9e505 21B 同法唯一 1 次 ⇒ 搜索方法可信。
static const UInt8 kFlt93F64Pattern[] = {0x55, 0x48, 0x89, 0xE5, 0x8B, 0x87, 0xB8, 0x07, 0x00, 0x00,
                                         0x0F, 0xA3, 0xF0, 0x0F, 0x92, 0xC0, 0x5D, 0xC3};
// 251（`-NRedGc7`）：client 7 双侧观测的两个入口。
//  安装侧 `0x93d18`：`al f(ctx=rdi, sel=esi)`；16B 字面量在 __text 内**唯一 1 次**@0x93d18（函数头✓）。
//  注销侧 `0x93998`：`void f(obj=rdi, id=esi, val=rdx)`；规格给的 16B **不唯一**（0x93982 的 getter
//   0x93980 内部同样匹配）⇒ 延长到 18B（多含 `89 54`，与 getter 的 `8B 44` 区分），唯一 1 次@0x93998。
static const UInt8 kGc7InstallPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x89,
                                           0xF3, 0x49, 0x89, 0xFE, 0x31, 0xC0, 0x8D, 0x4B};
static const UInt8 kGc7SetSlotPattern[] = {0x48, 0x85, 0xFF, 0x74, 0x10, 0x85, 0xF6, 0x74, 0x0C,
                                           0x55, 0x48, 0x89, 0xE5, 0x89, 0xF0, 0x48, 0x89, 0x54};
// 第八步观测（2026-09-28 第十九轮）：定位 TTL 初始化 `0x8b10e` 的失败出口。
//  `0x8b10e` 共 6 条出口，离线已排除 A（`0x90d75` 校验失败⇒将不做任何查询，与实测的 8 次查询矛盾）
//  与 B（`0x8b5de` 返回 NULL⇒但其内部的 3 次查询实测均成功）。剩余候选即下面 4 个函数：
//    C: `0xa1c3c` 返回 NULL → 出口 `0x8b3d4`
//    D: `0xa285f` 返回 false → 出口 `0x8b3cc`（校验 `arg1[0x30]+0x28`）
//    F: `0x93880` 返回 NULL → 出口 `0x8b3cc`
//    G: `0x928f4` 返回 NULL → 出口 `0x8b416` → `0x938a5` → 出口 `0x8b3cc`
static const UInt8 kTtlInitCollectPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53,
                                               0x48, 0x81, 0xEC, 0x30, 0x03, 0x00, 0x00, 0x49, 0x89, 0xFE, 0x48,
                                               0x8D, 0xBD, 0xE0, 0xFD, 0xFF, 0xFF, 0xBE, 0xF0, 0x01, 0x00};
static const UInt8 kTtlInitAllocAPattern[]  = {0x55, 0x48, 0x89, 0xE5, 0x53, 0x50, 0x48, 0x89, 0xFB, 0xE8, 0x00,
                                               0x00, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x03, 0x48, 0x89, 0x18,
                                               0x48, 0x83, 0xC4, 0x08, 0x5B, 0x5D, 0xC3, 0x55, 0x48, 0x89};
static const UInt8 kTtlInitAllocAPatternMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
                                                  0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const UInt8 kTtlInitAllocBPattern[]  = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x49,
                                               0x89, 0xFE, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x89, 0xC3, 0x48,
                                               0x85, 0xC0, 0x74, 0x51, 0x49, 0x89, 0xC7, 0x48, 0x8B, 0x7B};
static const UInt8 kTtlInitAllocBPatternMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                  0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
                                                  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const UInt8 kTtlInitCheckDPattern[]  = {0x55, 0x48, 0x89, 0xE5, 0x53, 0x50, 0x48, 0x89, 0xF3, 0xE8, 0x00,
                                               0x00, 0x00, 0x00, 0x48, 0x89, 0xC1, 0xB0, 0x01, 0x48, 0x85, 0xDB,
                                               0x74, 0x2F, 0x48, 0x85, 0xC9, 0x74, 0x2A, 0x48, 0x8B, 0x53};
static const UInt8 kTtlInitCheckDPatternMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
                                                  0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// B67（2026-10-15）：HWLibs `ttl_initialize`（归零 `0x8af60`；kc 绝对 `0x4DEEF60`；Ghidra 名
//  FUN_0008af60，**无符号** ⇒ 必须地址路由）。模式锚点＝**函数头序言**（push rbp; mov rsp,rbp;
//  push r15/r14/r12/rbx; sub rsp,80; 连续 9 个 `movq $0,-N(%rbp)` 清零栈槽; movl $2,%r15d），
//  共 59 字节，**取自真机同 build 的二进制**（`kb/内核扩展/13.6/extracted/
//  com.apple.kext.AMDRadeonX5000HWLibs.macho`，__text fileoff 口径 0x8bf60 提取）；
//  Ghidra `pattern_search` 实测**唯一**命中 0x8af60。编译期断言字节数＝59（防拷贝错行）。
static const UInt8 kTtlInitPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48,
                                        0x83, 0xEC, 0x50, 0x31, 0xC0, 0x48, 0x89, 0x45, 0xD8, 0x48, 0x89, 0x45,
                                        0xD0, 0x48, 0x89, 0x45, 0xC8, 0x48, 0x89, 0x45, 0xC0, 0x48, 0x89, 0x45,
                                        0xB8, 0x48, 0x89, 0x45, 0xB0, 0x48, 0x89, 0x45, 0xA8, 0x48, 0x89, 0x45,
                                        0xA0, 0x48, 0x89, 0x45, 0x98, 0x41, 0xBF, 0x02, 0x00, 0x00, 0x00};
static_assert(arrsize(kTtlInitPattern) == 59, "B67: kTtlInitPattern must be 59 bytes");

// 第八步观测（2026-09-28 第二十一轮）：接口表注册函数 `0x8b9a5(table, idx, obj, val)`
//  —— 写 `table[idx] = {idx, val, obj}`。离线实测：全 kext 内只有**两处**调用它（idx = 1 与 2），
//  而 `0x8bb9f(pool, 4)`（取 `pIpi`）要求表项 4 存在 ⇒ 需真机确认"表项 4 是否被注册"。
static const UInt8 kTtlRegIfacePattern[] = {0x83, 0xFE, 0x16, 0x77, 0x18, 0x55, 0x48, 0x89, 0xE5, 0x89, 0xF0, 0x48,
                                            0x8D, 0x04, 0x40, 0x89, 0x34, 0xC7, 0x48, 0x89, 0x4C, 0xC7, 0x08, 0x48};

// 第八步观测（2026-09-28 第二十三轮）：`TlsCreateInstance`(0x93f76) 与 `TlsSwInit` 内部创建(0x95e9f)。
//  依据：`0x928f4`（出口 G）在 `0x9291c` 调用 `0x93f76`，其返回 false 即走 `0x9295f` 失败；
//  真机日志的 `TlsCreateInstance` / `NULL != pInstance->pIpi` 正出自 `0x93f76` 的失败分支。
static const UInt8 kTlsCreatePattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54,
                                          0x53, 0x48, 0x83, 0xEC, 0x10, 0x49, 0x89, 0xF6, 0x49, 0x89,
                                          0xFF, 0x48, 0xC7, 0x45, 0xD8, 0x00, 0x00, 0x00};
static const UInt8 kTlsSwInitPattern[]  = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                                           0x81, 0xEC, 0xF8, 0x01, 0x00, 0x00, 0x49, 0x89, 0xFE, 0x48, 0x8B, 0x05, 0x00, 0x00};
static const UInt8 kTlsSwInitPatternMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                              0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00};

// 第八步观测（2026-09-28 第二十五轮）：`bgm_create`(0x297830) 内层 4 步。
//  真机错误码 `0xc00c0202` 出自 `0x297986`（`0x297968` 调 `0x2a9f0f` 返回非 0 时），故需判出是哪一步失败。
static const UInt8 kBgmInitPattern[]   = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                           0x53, 0x48, 0x81, 0xEC, 0x08, 0x01, 0x00, 0x00, 0x4D, 0x89, 0xC4, 0x48};
static const UInt8 kBgmStep1Pattern[]  = {0x55, 0x48, 0x89, 0xE5, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x48, 0x85, 0xFF,
                                           0x74, 0x26, 0x48, 0x85, 0xC9, 0x74, 0x21, 0x48, 0x85, 0xD2, 0x74, 0x17};
static const UInt8 kBgmQueryPattern[]  = {0x55, 0x48, 0x89, 0xE5, 0x48, 0x8B, 0x87, 0x40, 0x01, 0x00, 0x00, 0x48,
                                           0x8B, 0x78, 0x08, 0x48, 0x8B, 0x40, 0x40, 0x5D, 0xFF, 0xE0, 0x55, 0x48};
static const UInt8 kBgmStep4Pattern[]  = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x48, 0x89,
                                           0xFB, 0x4C, 0x8D, 0x7D, 0xE4, 0x41, 0xC7, 0x07, 0x00, 0x00, 0x00, 0x00};
// 230：BGM 表区转储挂点 `0x29b4e2`（服务读方法，无符号）。原型按反汇编：
//  `bool f(void* self, UInt32 sel, UInt64 size, void** out)`；成功时 `*out = resp[0]+16`。
static const UInt8 kBgmTabReadPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x49,
                                           0x89, 0xCE, 0x31, 0xDB, 0x48, 0x8D, 0x45, 0xC8, 0x48, 0x89, 0x58, 0x08};
// 235 P2：`0x298ce5` 通用 memcpy（`dst,src,len`）。⚠ 实施文档给的 18B pattern 中 `7E 1B` 与
//  实际二进制不符（实为 `7E 19`，即 je 偏移）⇒ 以下按 `com.apple.kext.AMDRadeonX5000HWLibs.macho`
//  实际字节（已在全库验证唯一、且位于函数头）。
static const UInt8 kBgm64MemcpyPattern[] = {0x31, 0xC0, 0x48, 0x85, 0xFF, 0x74, 0x22, 0x48, 0x85, 0xF6, 0x74, 0x1D,
                                            0x48, 0x85, 0xD2, 0x7E, 0x19, 0x55, 0x48, 0x89, 0xE5};
// 241：`0x91222` TrimHook（`void(root, client, stage, arg, mode)`）。文档/任务卡无 pattern，按本机实际
//  函数头字节取前 28 B（已离线验证全库唯一、位于函数头，CALL xrefs = 7）。
static const UInt8 kBgmHwNotifyPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                            0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x18, 0xBB, 0x02, 0x00,
                                            0x00, 0x00, 0x48, 0x85, 0xFF, 0x0F, 0x84, 0x03};
// `0x29a19e`：按 selector 读取（7/8 经 `0x29adb0`）。`0x2a9f0f` 用它做 selector=7 的读取 ⇒ 其返回值是判据之一。
static const UInt8 kReadSelPattern[]   = {0x41, 0x89, 0xC9, 0x89, 0xD0, 0x83, 0xFE, 0x08, 0x74, 0x09, 0x83, 0xFE,
                                           0x07, 0x75, 0x13, 0x31, 0xF6, 0xEB, 0x05, 0xBE, 0x01, 0x00, 0x00, 0x00};
// `0x2ab398(obj, id, out, flag)`：先按 id 的 bit31 写 selector 6/0，再读 selector 1 回填 `*out`。
//  模式 2 的判据 ②（`*(UInt16*)(r13+4) == 1`）中的结构即由此函数填充 ⇒ 需观测它。
static const UInt8 kCfgReadPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
                                        0x50, 0x49, 0x89, 0xCC, 0x49, 0x89, 0xD6, 0x48, 0x89, 0xF3, 0x49, 0x89, 0xFF,
                                        0x48, 0xF7, 0xC6, 0x00, 0x00, 0x00, 0x80};
// `0x2ab00e`：`0x2aacbb`（模式 2）的尾段调用。二值判定——它是否被调用、返回什么，
//  即可区分"失败在判据 ①/②"与"失败在尾段内部"。
static const UInt8 kMode2TailPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                          0x53, 0x48, 0x83, 0xEC, 0x28, 0x45, 0x89, 0xC4, 0x49, 0x89, 0xF5, 0xC7};
// `0x9440f`：模式 20B（全 __text 唯一命中），入口原型 `UInt32(void* a, UInt32 b, UInt32 c, void* d)`。
//  返回 0=失败、非 0=成功。调用频率低（每槽 1 次×3-4 槽）。
static const UInt8 kMode9440fPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53,
                                          0x45, 0x31, 0xE4, 0xFF, 0xCE, 0x83, 0xFE, 0x0E, 0x0F, 0x87};
// 批次 Q′：`0x1936e`（client5 stage1 叶第 14 步的被调者）入口 24 B。
//  实测首字节：55 48 89 e5 41 57 41 56 41 54 53 41 bc 02 00 00 00 48 85 ff 0f 84 c9 00
//  其中 `41 BC 02 00 00 00`（mov $2,%r12d）是本函数独有的判别序列。
static const UInt8 kQpGvm5InitPattern[] = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x41,
    0xBC, 0x02, 0x00, 0x00, 0x00, 0x48, 0x85, 0xFF, 0x0F, 0x84, 0xC9, 0x00};
// `0x96c4c`：回退路径（type3 槽 +0x30 虚槽 callee；发现块填充的**回退成败**——F0/F1 直接上游）。
static const UInt8 kMode96c4cPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53,
                                          0x48, 0x81, 0xEC, 0x80, 0x02, 0x00, 0x00, 0x49, 0x89, 0xD4, 0x49,
                                          0x89, 0xFF, 0x89, 0x75, 0xDC};
// `0x9e505`：(8,0) 链的 SMU 槽 sw_init 包装（B32 判读：0x9440f → 0x9e505 → *(X+0x48) 槽对象方法）。
//  模式＝函数头序言 21B（历史实现 0b70bb2 同款，已 PASS）。
static const UInt8 kMode9e505Pattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53,
                                          0x48, 0x81, 0xEC, 0x60, 0x03, 0x00, 0x00, 0x49, 0x89, 0xFF};
// `0x6430f`：smu_sw_init（槽方法本体＝B34 实测 m−slide=0x6430f）。模式＝函数头序言 19B
//  （历史实现 0b70bb2 同款，已 PASS；B31 memdump 已证真机逐字节一致）。
static const UInt8 kMode6430fPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53,
                                          0x48, 0x81, 0xEC, 0xB8, 0x07, 0x00, 0x00, 0x49, 0x89, 0xF6};
// ★ 修复（2026-09-28）：IP 发现表的 `num_base_address` 判据放宽。
//  `0x2ab00e` 在解析 IP 表时要求 `(u8(ip+3) - 1) <= 5`（即 nba ∈ {1..6}），否则跳到失败出口
//  `0x2ab1b5` 返回 1。RDNA3（Phoenix，780M）的 IP 表里 **hw_id 0x96 映射到 0x46 的块有 8 个基址**
//  （真机取证：`kb/sequences/raw/observe-20260928-1046-ipt5/`），于是 `0x2ab00e` 恒返回 1 ⇒
//  `bgm_create` 失败 ⇒ TTL 初始化失败 ⇒ PP 上电 panic（`Failed to get TTL RTS from HW Services`）。
//  放宽到 nba ≤ 9：目标缓冲的布局已核查（每实例 60 字节，8 个基址仅占 32 字节 ⇒ 不越界）。
// ★ M 补丁（B21 终判 2026-10-11）：消除减 1 下溢，通过域精确 {0..8}。
//  原判据 `lea edx,[rcx-1]; cmp dl,08; ja fail` 导致 nba=0 时 dl=0xFF>8 判失败。
//  改为 `lea edx,[rcx+0]`（disp8 00）保留 nba 原值，配合 imm8 08 ⇒ fail iff nba>8（无符号）。
//  nba=0 走零拷贝（拷贝循环执行 0 次，槽已预清零，指针推进有显式 nba=0 分支），安全。
//  ⚠️ 勘误（2026-10-12 张力钉死）：本补丁非 step2 失败点修复——第 62 轮「链路全通」归因已被证伪（失败在 G/0x929ac/0x9440f）；nba 校验在真机路径被前置跳过分支先行跳过，补丁作用未证实（保留无害）。详见 kb/技术草案/第62轮与B21B22张力钉死-2026-10-12.md。
static const UInt8 kIpNbaLimitOriginal[] = {0x8D, 0x51, 0xFF, 0x80, 0xFA, 0x05, 0x0F, 0x87};
static const UInt8 kIpNbaLimitPatched[]  = {0x8D, 0x51, 0x00, 0x80, 0xFA, 0x08, 0x0F, 0x87};
// ─────────────────────────────────────────────────────────────────────────────
// B95v2（2026-10-18）：基础启动序列（SWIP）补入 —— 我方静态序表／记录 ＋ `0xa2691` 的 pattern。
//
// 【为什么需要】Apple 内嵌设备信息表（`0x478180`，步长 0x50、269 条／83 键）里**没有 `0x15BF` 的记录**
//  ⇒ `ttlDevGetBaseSwipSeq`（`0xa3138`）取不到基础启动序列 ⇒ `dev+0xE8=0` ⇒ TTL 电源服务实例建立失败
//  ⇒ `m_hTtl` 恒 0 ⇒ panic（`Failed to get TTL RTS from HW Services`）。本卡在**唯一落点**上补一步。
//
// 【数据来源纪律（不得伪造）】
//  · 序表项的**值**（client 代号与子项编号）：取自 **Apple 全表一致的客户端定义**——本卡把 Apple
//    13 张模式 0 序表全部展开后，按"与 Apple 最近代模板 T12/T13 逐字一致"取定，**无一处我方臆造**。
//    两处"选择项"已在下方逐项标注（client 2＝SDMA 的 `[35]`；client 10＝DMCU 的 `[16]`）。
//  · **键值**（`0x15BF`／`9`／`0xC7`）：**全部来自本机真机实测**（`lspci-nn.txt` 的 `1002:15bf`、
//    `sysfs-device.txt` 的 `revision=0xc7`；三键见 R10 `[ttlkey]`）。
//  · 可复现：见 `tmp/prog/交付-Programmer-基础启动序列补入-2026-10-18.md` §3.3 的两条命令。
//
// 【结构（24 B/项，`client_id==0` 终止）—— 本卡亲手复核，与权威件 M-3 定稿一致】
//    +0x00 u32 client_id ； +0x04 u32 reserved(=0) ； +0x08 u64 sub_count ； +0x10 u64 sub_ids 指针
//  ⚠ **子项元素＝u32（不是 u64）**：Apple 表内 client 5（count=6）按 u64 读出是
//    `0x1b0000000b / 0x4600000022 / …`（每个 u64 塞两个小整数），按 u32 读才是 `[11,27,34,70,66,28]`。
//
// 【Apple 19 值集合（M-4 校验用）】把 13 张模式 0 表全部展开后，Apple 用到的子项编号共 19 个：
//    {4,11,12,15,16,26,27,28,33,34,35,36,37,39,66,70,75,76,77}
//  我方 12 个值（35/11/27/34/70/66/28/75/4/12/16/33）全部 ∈ 该集合 ⇒ M-4 通过。
// ─────────────────────────────────────────────────────────────────────────────

// 子项编号数组（元素＝u32 ⇒ 4 字节对齐即满足；表自身因含 u64 成员天然 8 字节对齐）。
//  ⚠ 顺序＝Apple `0x451db960`（101 条记录在用，表内最主流的一张）的**逐字顺序** `[3,5,6,8,7,2,14,9,15,10]`，
//  ⚠ `constexpr`（非仅 `const`）：编译期校验要在常量表达式里读取它们；放只读段也正合 Apple 只读语义。
static constexpr UInt32 kSubGvm[6]  = {11, 27, 34, 70, 66, 28};   // client 5 = GVM（13/13 表逐字一致）
static constexpr UInt32 kSubPsp[1]  = {75};                       // client 6 = PSP
static constexpr UInt32 kSubSmu[1]  = {4};                        // client 8 = SMU
static constexpr UInt32 kSubGc[1]   = {11};                       // client 7 = GC（13/13 表）
static constexpr UInt32 kSubSdma[1] = {35};                       // client 2 = SDMA【⚠ 选择项 A：备选 [35,36]】
static constexpr UInt32 kSubMes[3]  = {11, 27, 33};               // client 14 = MES（L0 已证本机有 mes_v11_0）
static constexpr UInt32 kSubVcn[1]  = {12};                       // client 9 = VCN（L0 已证 vcn_v4_0）
static constexpr UInt32 kSubJpeg[1] = {12};                       // client 15 = JPEG（L0 已证 jpeg_v4_0）
static constexpr UInt32 kSubDmcu[1] = {16};                       // client 10 = DMCU【⚠ 选择项 B：备选 [15]】

// 序表项（24 B/项）。与 Apple 的布局逐字一致：{u32 client_id; u32 reserved; u64 count; u64 subs_ptr}。
//  ⚠ `subs` 直接声明为 `const UInt32*`（而非 `const void*`）：既与"子项元素＝u32"的事实一致，
//    又让编译期校验不必做常量表达式里非法的 `void*`→`UInt32*` 转换。
struct NRedSwipSeqItem {
    UInt32         clientId;
    UInt32         reserved;
    UInt64         count;
    const UInt32*  subs;
};

// 模式 0 序表（本机实测 boot=0 ⇒ 运行期只走这张）。10 项 ＋ 终止项。
static constexpr NRedSwipSeqItem kNRedSwipSeq0[] = {
    {3,  0, 0, nullptr},          // BGM（恒在客户端；13/13 表；名单恒为空）
    {5,  0, 6, kSubGvm},          // GVM
    {6,  0, 1, kSubPsp},          // PSP
    {8,  0, 1, kSubSmu},          // SMU
    {7,  0, 1, kSubGc},           // GC
    {2,  0, 1, kSubSdma},         // SDMA【选择项 A】
    {14, 0, 3, kSubMes},          // MES
    {9,  0, 1, kSubVcn},          // VCN
    {15, 0, 1, kSubJpeg},         // JPEG
    {10, 0, 1, kSubDmcu},         // DMCU【选择项 B】
    {0,  0, 0, nullptr},          // 终止项（client_id==0）
};
// 模式 1 序表：本机 `boot=0`（实测）⇒ 运行期只走模式 0。本表与 Apple 共用模式 1 表
//  （`0x45191c40`：`{client 3, count 0}` ＋ 终止项）**逐字节同形**；技术经理裁定用我方自建
//  （零 slide 依赖、无保真损失）——理由：该字段只在 `idx==1` 时被读，本机根本不读。
static constexpr NRedSwipSeqItem kNRedSwipSeq1[] = {
    {3, 0, 0, nullptr},           // BGM
    {0, 0, 0, nullptr},           // 终止项
};

// ── 我方记录（0x50）——按 `甲-prime 整合与记录定稿 §3.2` 字段级定稿 ──────────────
//  ⚠ 布局：8 个 u64 槽（+0x00…+0x48）。前 5 个是标量/键，后 5 个是指针。
//  ⚠ **全部指针都指向我方自己的静态数据 ⇒ 零运行期重定位、零硬编码绝对地址**
//    （技术经理 2026-10-18 裁定：`+0x40` 用本地 nullptr[8]；`+0x38`/`+0x48` 用本地副本；
//     `+0x28`/`+0x30` 用我方序表 ⇒ 整条记录保持纯静态，无基址依赖）。
//  ⚠ 绝不写 chained-fixup 编码值——那套编码只在 Apple 镜像里成立。
struct NRedDevRecord {
    UInt64       f00;   // +0x00 = 0x8e（同族模板 idx82/84；非 0 ＝ 在用标志）
    UInt64       f08;   // +0x08 = 0x82（同族模板；两处查表都不读它，语义【未解出】）
    UInt64       f10;   // +0x10 = 0x15BF  ← 键1：**真机实测**（device_id）
    UInt64       f18;   // +0x18 = 9       ← 键2：**真机实测**（M-1 定稿＝真值 9）
    UInt64       f20;   // +0x20 = 0xDEADCAFE（通配；M-2 保守取通配，收紧条件见定稿 §3.2）
    const void*  f28;   // +0x28 → 我方模式 0 序表（`0xa3138` idx=0 ／ `0xa26ff` 模式 0 只读它）
    const void*  f30;   // +0x30 → 我方模式 1 序表（`0xa3138` idx=1；本机 boot=0 ⇒ 运行期不读）
    const void*  f38;   // +0x38 → 我方本地副本（7 位能力字节，Apple 原址 0x45191c7c）
    const void*  f40;   // +0x40 → 我方本地 nullptr[8]（Apple 原址 0x451e16c0，未见读取者）
    const void*  f48;   // +0x48 → 我方本地副本（Apple 原址 0x45191e00，与 type=0x8e 对应那一支）
};

// `+0x38` 的本地副本（16 B）：Apple 原址 `0x45191c7c` 的头 16 字节逐字节照抄
//  （`41 00 00 00 | 0a 00 00 00 | 6e 00 00 00 | 00 00 00 00`）。
//  判据：读者只有 6 个定长位读器（`0xa1d12` 等，只读 `[0]` 的 bit0..6；按字索引的
//  `0xa1d3c` 全库零调用者）⇒ 只需 `[0]` 正确 ⇒ 本地副本功能等价。
static constexpr UInt64 kRecF38[2] = {0x0000000a00000041ULL, 0x000000000000006eULL};
// `+0x48` 的本地副本（16 B）：Apple 原址 `0x45191e00`（`type=0x8e` 对应那一支）头 16 字节
//  （`12 00 00 00 | 00…`）。判据：**未发现任何读取者**，本地等价。
static constexpr UInt64 kRecF48[2] = {0x0000000000000012ULL, 0x0000000000000000ULL};
// `+0x40`：**本地 nullptr[8]**（不照抄）。判据：①未见读取者；②Apple 原址是指针数组、
//  元素又指向语义未解出的数据 ⇒ 本地化无法"搬内容"；③零数组使整条记录保持纯静态。
//  止损：若下一轮出现与该字段相关的新崩点 ⇒ 改回"照抄 Apple 原表 = base + 0x47d6c0"。
static constexpr UInt64 kRecF40[8] = {0, 0, 0, 0, 0, 0, 0, 0};

// 我方记录（唯一一条）。`const` 而**非** `constexpr`：含指针成员，地址在链接期确定。
static const NRedDevRecord kNRedDevRecord = {
    0x8e,            // +0x00 同族模板
    0x82,            // +0x08 同族模板
    0x15BF,          // +0x10 键1＝真机实测 device_id
    9,               // +0x18 键2＝真机实测（M-1）
    0xDEADCAFE,      // +0x20 通配（M-2：保守取通配）
    kNRedSwipSeq0,   // +0x28 我方模式 0 序表
    kNRedSwipSeq1,   // +0x30 我方模式 1 序表
    kRecF38,         // +0x38 本地副本
    kRecF40,         // +0x40 本地 nullptr[8]
    kRecF48,         // +0x48 本地副本
};
static_assert(sizeof(NRedDevRecord) == 0x50, "B95v2: 我方记录必须是 0x50 字节（Apple 步长）");

// ── 编译期校验（权威件 §3.4 六条自检 ＋ M-4）────────────────────────────────
//  ② 记录级／③ 序表级／③′ M-4 全在**编译期**完成 ⇒ 真机上不存在"结构不合法却仍写入"的可能。
//  （① 文件级不适用：无数据文件，我方数据是编译期常量。）
namespace {
constexpr bool nredSwipSubOk(const UInt32* const p, const UInt64 n)
{
    for (UInt64 i = 0; i < n; i += 1) {
        const UInt32 v = p[i];
        // M-4：子项编号必须 ∈ Apple 既有 19 值集合（不得出现 Apple 不认识的编号）。
        if (v != 4 && v != 11 && v != 12 && v != 15 && v != 16 && v != 26 && v != 27 && v != 28 &&
            v != 33 && v != 34 && v != 35 && v != 36 && v != 37 && v != 39 && v != 66 && v != 70 &&
            v != 75 && v != 76 && v != 77) {
            return false;
        }
    }
    return true;
}
constexpr bool nredSwipTableOk(const NRedSwipSeqItem* const t, const UInt64 n)
{
    if (n == 0 || n > 32) { return false; }                 // 自检 3：项数 ≤ 32（`0x93fe3 cmpq $32,%rsi`）
    if (t[0].clientId == 0) { return false; }               // 自检 1 首项非终止
    if (t[n - 1].clientId != 0) { return false; }           // 自检 5 以 client_id==0 终止
    for (UInt64 i = 0; i < n - 1; i += 1) {
        const NRedSwipSeqItem& it = t[i];
        if (it.clientId == 0 || it.clientId > 15) { return false; }   // 自检 1：client ∈ [1,15]
        if (it.reserved != 0) { return false; }                       // 自检 5：reserved == 0
        if (it.count > 64) { return false; }                          // 自检 2：子项数 ≤ 64
        if (it.count != 0 && (it.subs == nullptr || !nredSwipSubOk(it.subs, it.count))) {
            return false;                                             // M-4
        }
    }
    return true;
}
static_assert(nredSwipTableOk(kNRedSwipSeq0, sizeof(kNRedSwipSeq0) / sizeof(kNRedSwipSeq0[0])),
              "B95: 我方模式 0 序表未过编译期校验（记录级/序表级/M-4）");
static_assert(nredSwipTableOk(kNRedSwipSeq1, sizeof(kNRedSwipSeq1) / sizeof(kNRedSwipSeq1[0])),
              "B95: 我方模式 1 序表未过编译期校验（记录级/序表级/M-4）");
static_assert(sizeof(NRedSwipSeqItem) == 24, "B95: 序表项必须是 24 字节（Apple 布局）");
}  // namespace

// `0xa2691`（第二次三键查表）入口 20 B —— **全 macho 唯一命中**（fileoff 0xa3691 = vmaddr 0xa2691）。
//  实测：18／20／24／28 B 均**唯一**（全库仅此一处）⇒ 取 20 B。
//  与反汇编逐字对应：`a2691 pushq %rbp`／`a2692 movq %rsp,%rbp`／
//  `a2695 leaq 4020964(%rip),%r8`（表基址 0x478180）／`a269c cmpq $0,(%r8)`／`a26a0 je`／
//  `a26a2 movl $1,%r11d`。
static const UInt8 kDevLookupPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x4C, 0x8D, 0x05, 0xE4, 0x5A, 0x3D,
                                          0x00, 0x49, 0x83, 0x38, 0x00, 0x74, 0x50, 0x41, 0xBB, 0x01};
static_assert(arrsize(kDevLookupPattern) == 20, "B95v2: kDevLookupPattern must be 20 bytes");

static const UInt8 kCailAsicCapsTableHWLibsPattern[] = {0x6E, 0x00, 0x00, 0x00, 0x98, 0x67, 0x00, 0x00,
                                                        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                                                        0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};

static const UInt8 kPspCmdKmSubmitPattern[]     = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41,
                                                   0x54, 0x53, 0x50, 0x49, 0x89, 0xCD, 0x49, 0x89, 0xD7, 0x49, 0x89,
                                                   0xF4, 0x48, 0x89, 0xFB, 0x48, 0x8D, 0x75, 0xD0, 0xC7, 0x06, 0x00,
                                                   0x00, 0x00, 0x00, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kPspCmdKmSubmitPatternMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                   0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};

static const UInt8 kPspCmdKmSubmitPattern1404[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41,
                                                   0x54, 0x53, 0x48, 0x83, 0xEC, 0x18, 0x49, 0x89, 0xCD, 0x49, 0x89,
                                                   0xD7, 0x49, 0x89, 0xF4, 0x49, 0x89, 0xFE, 0x48, 0x8D, 0x75, 0xD0,
                                                   0xC7, 0x06, 0x00, 0x00, 0x00, 0x00, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kPspCmdKmSubmitPatternMask1404[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};

static const UInt8 kCAILAsicCapsInitTablePattern[] = {0x6E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x98, 0x67,
                                                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                      0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                      0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};

static const UInt8 kDeviceCapabilityTblPattern[] = {0x82, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x00,
                                                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x73, 0x00, 0x00,
                                                    0x00, 0x00, 0x00, 0x00, 0xFE, 0xCA, 0xAD, 0xDE, 0x00, 0x00,
                                                    0x00, 0x00, 0xFE, 0xCA, 0xAD, 0xDE, 0x00, 0x00, 0x00, 0x00};

static const UInt8 kPspReset31Pattern[]     = {0x55, 0x48, 0x89, 0xE5, 0x53, 0x48, 0x83, 0xEC, 0x28, 0x31, 0xC0, 0x48,
                                               0x89, 0x45, 0xF0, 0x48, 0x89, 0x45, 0xE8, 0x48, 0x89, 0x45, 0xE0, 0x48,
                                               0x89, 0x45, 0xD8, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x48, 0x85, 0xFF};
static const UInt8 kPspReset31Pattern1404[] = {0x55, 0x48, 0x89, 0xE5, 0x53, 0x48, 0x83, 0xEC, 0x28,
                                               0x48, 0xC7, 0x45, 0xE8, 0x00, 0x00, 0x00, 0x00, 0xB8,
                                               0x01, 0x00, 0x00, 0x00, 0x48, 0x85, 0xFF};

static const UInt8 kPspBootloaderLoadSysdrv31Pattern[] = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x28, 0x49, 0x89,
    0xFC, 0x31, 0xDB, 0x48, 0x89, 0x5D, 0xD0, 0x48, 0x89, 0x5D, 0xC8, 0x48, 0x89, 0x5D, 0xC0, 0x48, 0x89, 0x5D, 0xB8,
    0x4C, 0x8B, 0xB7, 0x00, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0xBF, 0x00, 0x00, 0x00, 0x00, 0xBE, 0x91, 0x00, 0x00, 0x00};
static const UInt8 kPspBootloaderLoadSysdrv31PatternMask[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const UInt8 kPspBootloaderLoadSysdrv31Pattern1404[] = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x28,
    0x48, 0x89, 0xFB, 0x48, 0xC7, 0x45, 0xC8, 0x00, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0xB7, 0x38, 0x0B, 0x00,
    0x00, 0x4C, 0x8B, 0xBF, 0x48, 0x0B, 0x00, 0x00, 0x45, 0x31, 0xE4, 0xBE, 0x91, 0x00, 0x00, 0x00};

static const UInt8 kPspBootloaderSetEccMode31Pattern[] = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x41, 0x89,
    0xF7, 0x49, 0x89, 0xFE, 0x31, 0xDB, 0x48, 0x89, 0x5D, 0xD8, 0x48, 0x89, 0x5D, 0xD0, 0x48, 0x89, 0x5D,
    0xC8, 0x48, 0x89, 0x5D, 0xC0, 0xBE, 0xA4, 0x00, 0x00, 0x00, 0x31, 0xD2, 0xB9, 0x4B, 0x00, 0x00, 0x00};
static const UInt8 kPspBootloaderSetEccMode31Pattern1404[] = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x41, 0x89, 0xF7,
    0x48, 0x89, 0xFB, 0x48, 0xC7, 0x45, 0xD0, 0x00, 0x00, 0x00, 0x00, 0x45, 0x31, 0xF6, 0xBE, 0xA4, 0x00, 0x00,
    0x00, 0x31, 0xD2, 0xB9, 0x4B, 0x00, 0x00, 0x00, 0xE8, 0xA8, 0x5B, 0xFF, 0xFF, 0x3D, 0x00, 0x0C, 0x0B, 0x00};

static const UInt8 kPspBootloaderIsSosRunning31Pattern[] = {0x55, 0x48, 0x89, 0xE5, 0xBE, 0x91, 0x00, 0x00,
                                                            0x00, 0x31, 0xD2, 0xB9, 0x4B, 0x00, 0x00, 0x00};

static const UInt8 kPspBootloaderLoadSos31Pattern[] = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x49, 0x89, 0xFF, 0x31,
    0xDB, 0x48, 0x89, 0x5D, 0xD8, 0x48, 0x89, 0x5D, 0xD0, 0x48, 0x89, 0x5D, 0xC8, 0x48, 0x89, 0x5D, 0xC0, 0x4C, 0x8B,
    0xB7, 0x00, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0xA7, 0x00, 0x00, 0x00, 0x00, 0xBE, 0x91, 0x00, 0x00, 0x00};
static const UInt8 kPspBootloaderLoadSos31PatternMask[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const UInt8 kPspBootloaderLoadSos31Pattern1404[] = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48,
    0x89, 0xFB, 0x48, 0xC7, 0x45, 0xD0, 0x00, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0xB7, 0x38, 0x0B, 0x00,
    0x00, 0x4C, 0x8B, 0xA7, 0x48, 0x0B, 0x00, 0x00, 0x45, 0x31, 0xFF, 0xBE, 0x91, 0x00, 0x00, 0x00};

static const UInt8 kPspSecurityFeatureCapsSet31Pattern[] = {0x55, 0x48, 0x89, 0xE5, 0x80, 0xA7, 0x20, 0x31, 0x00, 0x00};
static const UInt8 kPspSecurityFeatureCapsSet31Pattern13[] = {0x55, 0x48, 0x89, 0xE5, 0x8B,
                                                              0x87, 0x18, 0x39, 0x00, 0x00};

static const UInt8 kGcSetFwEntryInfoCallPattern[] = {0x14, 0x4C, 0x89, 0xF1, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x80, 0xC0};
static const UInt8 kGcSetFwEntryInfoCallPatternMask[]           = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
                                                                   0x00, 0x00, 0x00, 0xF0, 0xF0};
static constexpr size_t kGcSetFwEntryInfoCallPatternJumpInstOff = 0x4;

static const UInt8      kSdmaInitFuncPtrListCallPattern[]          = {0x44, 0x89, 0x00, 0x44, 0x89, 0x00, 0xE8, 0x00,
                                                                      0x00, 0x00, 0x00, 0x48, 0x8B, 0x00, 0x00, 0x85};
static const UInt8      kSdmaInitFuncPtrListCallPatternMask[]      = {0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0x00, 0xFF, 0x00,
                                                                      0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0xFF};
static constexpr size_t kSdmaInitFuncPtrListCallPatternJumpInstOff = 0x6;

static const UInt8      kDmcuBackdoorLoadFwBranchPattern[]        = {0x8D, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x83, 0xF8,
                                                                     0x02, 0x0F, 0x82, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kDmcuBackdoorLoadFwBranchPatternMask[]    = {0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                     0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr size_t kDmcuBackdoorLoadFwDcn1ConstantsBranchOff = 0x9;
static constexpr size_t kDmcuGetDcn1FwConstantsCallOff            = 0x24;
static constexpr size_t kDmcuGetDcn21FwConstantsCallOff           = 0x4C;

static const UInt8      kSmuInitFunctionPointerListCallPattern[]          = {0x49, 0x8B, 0x00, 0x0C, 0x40, 0x8B, 0x00,
                                                                             0x14, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kSmuInitFunctionPointerListCallPatternMask[]      = {0xFF, 0xFF, 0x00, 0xFF, 0xF0, 0xFF, 0x00,
                                                                             0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr size_t kSmuInitFunctionPointerListCallPatternJumpInstOff = 0x8;

static const UInt8      kSmu90SendMessageWithParameterCallPattern[]          = {0xBE, 0x45, 0x00, 0x00, 0x00, 0x5D,
                                                                                0xE9, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kSmu90SendMessageWithParameterCallPatternMask[]      = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                                0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr size_t kSmu90SendMessageWithParameterCallPatternJumpInstOff = 6;

static const UInt8      kSdmaCgsReadRegisterCallPattern[]          = {0xBE, 0x80, 0x00, 0x00, 0x00, 0x31, 0xD2, 0x44,
                                                                      0x89, 0xF0, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kSdmaCgsReadRegisterCallPatternMask[]      = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                      0xFF, 0xF0, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr size_t kSdmaCgsReadRegisterCallPatternJumpInstOff = 10;

static const UInt8      kSdmaCgsWriteRegisterCallPattern[]     = {0xBE, 0x80, 0x00, 0x00, 0x00, 0x31, 0xD2, 0x89, 0xC1,
                                                                  0x45, 0x89, 0xF0, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kSdmaCgsWriteRegisterCallPatternMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                  0xFF, 0xFF, 0xF0, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr size_t kSdmaCgsWriteRegisterCallPatternJumpInstOff = 12;

static const UInt8      kSmuCosWaitForCallPattern[]          = {0xE8, 0x00, 0x00, 0x00, 0x00, 0x85, 0xC0};
static const UInt8      kSmuCosWaitForCallPatternMask[]      = {0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF};
static constexpr size_t kSmuCosWaitForCallPatternJumpInstOff = 0;

static const UInt8      kSmuCgsWriteRegisterCallPattern[]          = {0x41, 0xB8, 0x04, 0x00, 0x00, 0x00, 0x45,
                                                                      0x31, 0xC9, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kSmuCgsWriteRegisterCallPatternMask[]      = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                      0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr size_t kSmuCgsWriteRegisterCallPatternJumpInstOff = 9;

static const UInt8      kSmuCgsReadRegisterCallPattern[]          = {0xB9, 0x04, 0x00, 0x00, 0x00, 0x45, 0x31,
                                                                     0xC0, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kSmuCgsReadRegisterCallPatternMask[]      = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                     0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr size_t kSmuCgsReadRegisterCallPatternJumpInstOff = 8;

// Replace call to `_gc_get_hw_version` with constant (0x090001).
static const UInt8 kGcSwInitOriginal[]     = {0x0C, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x41, 0x89, 0xC7};
static const UInt8 kGcSwInitOriginalMask[] = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF};
static const UInt8 kGcSwInitPatched[]      = {0x00, 0xB8, 0x01, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kGcSwInitPatchedMask[]  = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00};

// Force PSP 9.x switch case.
static const UInt8 kPspSwInit1Original[] = {0x8B, 0x43, 0x0C, 0x83, 0xC0, 0xF7, 0x83, 0xF8, 0x04};
static const UInt8 kPspSwInit1Patched[]  = {0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x83, 0xF8, 0x04};

// Ditto. macOS 14.4+
static const UInt8 kPspSwInit1Original1404[] = {0x41, 0x8B, 0x46, 0x08, 0x83, 0xC0, 0xF7, 0x83, 0xF8, 0x04};
static const UInt8 kPspSwInit1Patched1404[]  = {0x90, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x83, 0xF8, 0x04};

// Force PSP x.0.0.
static const UInt8 kPspSwInit2Original[]     = {0x83, 0x7B, 0x10, 0x00, 0x0F, 0x85, 0x00, 0x00, 0x00, 0x00,
                                                0x83, 0x7B, 0x14, 0x01, 0x0F, 0x85, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kPspSwInit2OriginalMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
                                                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kPspSwInit2Patched[]      = {0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90,
                                                0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90};

// Ditto. macOS 14.4+
static const UInt8 kPspSwInit2Original1404[]     = {0x41, 0x83, 0x7E, 0x0C, 0x00, 0x0F, 0x85, 0x00, 0x00, 0x00, 0x00,
                                                    0x41, 0x83, 0x7E, 0x10, 0x01, 0x0F, 0x85, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kPspSwInit2OriginalMask1404[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
                                                    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kPspSwInit2Patched1404[]      = {0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66,
                                                    0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90};

// Catalina, for Renoir: Force PSP 11.x switch case. PSP 11.0.3 is actually PSP 12 in Catalina HWLibs.
static const UInt8 kPspSwInit1Original1015[] = {0x41, 0x8B, 0x44, 0x24, 0x0C, 0x83, 0xC0, 0xF7};
static const UInt8 kPspSwInit1Patched1015[]  = {0xB8, 0x02, 0x00, 0x00, 0x00, 0x66, 0x90, 0x90};

// Ditto, part 2: Force PSP X.0.3.
static const UInt8 kPspSwInit2Original1015[]     = {0x41, 0x83, 0x7C, 0x24, 0x10, 0x00, 0x0F, 0x85, 0x00,
                                                    0x00, 0x00, 0x00, 0x41, 0x8B, 0x44, 0x24, 0x14, 0x83,
                                                    0xF8, 0x03, 0x0F, 0x85, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kPspSwInit2OriginalMask1015[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
                                                    0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                    0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kPspSwInit2Patched1015[]      = {0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66,
                                                    0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90,
                                                    0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90};

// Use "correct" PowerTuneServices by changing the switch statement case `0x8D` to `0x8E`.
static const UInt8 kCreatePowerTuneServices1Original[] = {0x41, 0x8B, 0x47, 0x18, 0x83, 0xC0, 0x88, 0x83, 0xF8, 0x17};
static const UInt8 kCreatePowerTuneServices1Patched[]  = {0x41, 0x8B, 0x47, 0x18, 0x83, 0xC0, 0x87, 0x83, 0xF8, 0x17};

// Ditto. macOS 12.0+
static const UInt8 kCreatePowerTuneServices1Original12[] = {0xB8, 0x7E, 0xFF, 0xFF, 0xFF, 0x41,
                                                            0x03, 0x47, 0x18, 0x83, 0xF8, 0x0F};
static const UInt8 kCreatePowerTuneServices1Patched12[]  = {0xB8, 0x7D, 0xFF, 0xFF, 0xFF, 0x41,
                                                            0x03, 0x47, 0x18, 0x83, 0xF8, 0x0F};

// Ditto. macOS 14.4+
static const UInt8 kCreatePowerTuneServices1Original1404[] = {0xB8, 0x7E, 0xFF, 0xFF, 0xFF, 0x03,
                                                              0x43, 0x18, 0x83, 0xF8, 0x0F};
static const UInt8 kCreatePowerTuneServices1Patched1404[]  = {0xB8, 0x7D, 0xFF, 0xFF, 0xFF, 0x03,
                                                              0x43, 0x18, 0x83, 0xF8, 0x0F};

// Remove revision check to always use Vega 10 PowerTune.
static const UInt8 kCreatePowerTuneServices2Original[] = {0x41, 0x8B, 0x47, 0x1C, 0x83, 0xF8, 0x13, 0x77, 0x00};
static const UInt8 kCreatePowerTuneServices2Mask[]     = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
static const UInt8 kCreatePowerTuneServices2Patched[]  = {0x41, 0x8B, 0x47, 0x1C, 0x66, 0x90, 0x66, 0x90, 0x90};

// Ditto. macOS 14.4+
static const UInt8 kCreatePowerTuneServices2Original1404[] = {0x8B, 0x43, 0x1C, 0x83, 0xF8, 0x13, 0x77, 0x00};
static const UInt8 kCreatePowerTuneServices2Mask1404[]     = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
static const UInt8 kCreatePowerTuneServices2Patched1404[]  = {0x8B, 0x43, 0x1C, 0x66, 0x90, 0x66, 0x90, 0x90};

// Ventura added an explicit switch case for family ID.
// Now we have to make the switch case 0x8D be 0x8E.
static const UInt8 kCailQueryAdapterInfoOriginal[] = {0x83, 0xC0, 0x92, 0x83, 0xF8, 0x21};
static const UInt8 kCailQueryAdapterInfoPatched[]  = {0x83, 0xC0, 0x91, 0x83, 0xF8, 0x21};

static X5000HWLibs moduleInstance;

X5000HWLibs& X5000HWLibs::singleton() { return moduleInstance; }

X5000HWLibs::X5000HWLibs()
{
    if (currentKernelVersion() <= MACOS_10_15_X) {
        this->pspCommandDataField      = 0xB00;
        this->smuInternalSWInitField   = 0x378;
        this->smuInternalHWInitField   = 0x380;
        this->smuInternalSWExitField   = 0x388;
        this->smuInternalHWExitField   = 0x390;
        this->smuFullAsicResetField    = 0x3A0;
        this->smuNotifyEventField      = 0x3A8;
        this->smuFullscreenEventField  = 0x3B8;
        this->gcSwFirmwareField        = 0x2E8;
        this->dmcuEnablePSPFWLoadField = 0x248;
        this->dmcuABMLevelField        = 0x24C;
        this->sdmaGetFwConstantsField  = 0x270;
        this->sdmaStartEngineField     = 0x298;
    }
    else if (currentKernelVersion().majorMatches(MACOS_11)) {
        this->fwDirField                = 0xB8;
        this->smuSwInitialisedFieldBase = 0x280;
        this->smuInternalSWInitField    = 0x638;
        this->smuInternalHWInitField    = 0x640;
        this->smuInternalSWExitField    = 0x648;
        this->smuInternalHWExitField    = 0x650;
        this->smuFullAsicResetField     = 0x660;
        this->smuNotifyEventField       = 0x668;
        this->smuFullscreenEventField   = 0x680;
        this->smuGetUCodeConstsField    = 0x720;
        this->pspCommandDataField       = 0xAF8;
        this->pspSecurityCapsField      = 0x3120;
        this->pspBootloaderVersionField = 0x3124;
        this->pspTOSVersionField        = 0x3128;
        this->gcSwFirmwareField         = 0x2F0;
        this->dmcuEnablePSPFWLoadField  = 0x248;
        this->dmcuABMLevelField         = 0x24C;
        this->sdmaGetFwConstantsField   = 0x270;
        this->sdmaStartEngineField      = 0x298;
    }
    else if (currentKernelVersion().majorMatches(MACOS_12)) {
        this->fwDirField                = 0xB0;
        this->smuSwInitialisedFieldBase = 0x280;
        this->smuInternalSWInitField    = 0x648;
        this->smuInternalHWInitField    = 0x650;
        this->smuInternalSWExitField    = 0x658;
        this->smuInternalHWExitField    = 0x660;
        this->smuFullAsicResetField     = 0x670;
        this->smuNotifyEventField       = 0x678;
        this->smuFullscreenEventField   = 0x690;
        this->smuGetUCodeConstsField    = 0x730;
        this->pspCommandDataField       = 0xAF8;
        this->pspSecurityCapsField      = 0x3120;
        this->pspBootloaderVersionField = 0x3124;
        this->pspTOSVersionField        = 0x3128;
        this->gcSwFirmwareField         = 0x2F0;
        this->dmcuEnablePSPFWLoadField  = 0x248;
        this->dmcuABMLevelField         = 0x24C;
        this->sdmaGetFwConstantsField   = 0x268;
        this->sdmaStartEngineField      = 0x290;
    }
    else {
        this->fwDirField                = 0xB0;
        this->smuSwInitialisedFieldBase = 0x2D0;
        this->pspCommandDataField       = 0xB48;
        this->smuInternalSWInitField    = 0x6C0;
        this->smuInternalHWInitField    = 0x6C8;
        this->smuInternalSWExitField    = 0x6D0;
        this->smuInternalHWExitField    = 0x6D8;
        this->smuFullAsicResetField     = 0x6E8;
        this->smuNotifyEventField       = 0x6F0;
        this->smuFullscreenEventField   = 0x708;
        this->smuGetUCodeConstsField    = 0x7A8;
        this->pspSecurityCapsField      = 0x3918;
        this->pspBootloaderVersionField = 0x391C;
        this->pspTOSVersionField        = 0x3920;
        this->gcSwFirmwareField         = 0x340;
        this->sdmaGetFwConstantsField   = 0x2C8;
        this->sdmaStartEngineField      = 0x2F0;

        if (currentKernelVersion() <= MACOS_13_4) {
            this->dmcuEnablePSPFWLoadField = 0x248;
            this->dmcuABMLevelField        = 0x24C;
        }
        else {
            this->dmcuEnablePSPFWLoadField = 0x298;
            this->dmcuABMLevelField        = 0x29C;
        }
    }
}

// 第八步观测（2026-09-28）：TTL 的跨 kext 查询函数（HWLibs 归零 vm `0x90ddc`）。
//  依据（子 agent `TtlExits` 的逐出口分析）：`0x8b10e` 的失败出口 2 = "`0x8b5de` 内的三次
//  `0x90ddc` 查询（ID = 0x228 / 0x320 / 8）返回 NULL"；真机该步以 `status = 4` 失败。
//  本 wrapper **只记录入参 ID、返回值与 out 缓冲区内容**（不改变行为、不调用 Apple 方法）。
//  2026-09-28 run149 实测：8 次调用（id=552/800/8/680/16/1992/168/104）**全部返回非空指针**，
//  但 `ttl_initialize` 仍以 `status = 4` 失败 ⇒ 判决点在查询**之后** ⇒ 需看 out 里写回的数据是否有效。
void* X5000HWLibs::wrapTtlQuery(void* const buf, const UInt32 id, const UInt32 flags, UInt64* const out)
{
    if (singleton().orgTtlQuery == 0) { return nullptr; }
    void* const ret = FunctionCast(wrapTtlQuery, singleton().orgTtlQuery)(buf, id, flags, out);
    if (checkKernelArgument("-NRedAccelLog")) {
        UInt64 v0 = 0, v1 = 0;
        if (nredKernelPtrOk(out)) { v0 = out[0]; v1 = out[1]; }   // 实测各调用点的缓冲区间隔 0xE8 字节，读 16 字节安全
        NRED_TRACE("ttl query: id=%u flags=%u ret=%llx out=%llx v0=%llx v1=%llx", static_cast<unsigned>(id),
               static_cast<unsigned>(flags), static_cast<unsigned long long>(reinterpret_cast<UInt64>(ret)),
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(out)), static_cast<unsigned long long>(v0),
               static_cast<unsigned long long>(v1));
    }
    // B48 挂点 B：查询分发器 out 对象 dump（纯只读、零行为改动）。
    //  `ret`＝0x90ddc 返回的分配对象（=local_40）；dump 其前 0x60 字节 ×2 条（防单行截断）。
    //  ⚠️ 非空门：ret 为空只打哨兵行。深栈纪律：只走 nredTraceLine。
    if (checkKernelArgument("-NRedAccelLog")) {
        if (ret == nullptr) {
            NRED_TRACE("qry-dump: skip (id=%u ret=0)", static_cast<unsigned>(id));
        } else {
            const auto* const p = static_cast<const UInt8*>(ret);
            char hex[97];
            for (UInt32 part = 0; part < 2; part += 1) {
                for (UInt32 k = 0; k < 0x30; k += 1) {
                    snprintf(hex + k * 2, 3, "%02x", p[part * 0x30 + k]);
                }
                NRED_TRACE("qry-dump: id=%u +%x b=%s", static_cast<unsigned>(id), part * 0x30, hex);
            }
        }
    }
    return ret;
}

// 251（`-NRedGc7`）：client 7 双侧观测。
//  安装侧 `0x93d18(ctx=rdi, sel=esi, arg=rdx)`：调原件后、若 `sel==7` 且 ctx 通过 KPtr，读
//    `p7=*(ctx+0x40)`（写入后值）；`al` 原样透传，**第 3 参 rdx 必须原样转发**（各 selector 的
//    creator 以它为实参，见 `93d45 movq %rdx,%rsi`）。限流：首次调用 + 首次 `hit=1` 各 1 条（≤2）。
//  注销侧 `0x93998(obj=rdi, id=esi, val=rdx)`：只在 `val==0 && id==7` 记一次（`[gc7x]`，≤1 条）。
//  两处均只读入参/固定偏移，不跟随函数指针、不解引用返回值；门控关 ⇒ 不安装 hook、零行为。
bool X5000HWLibs::wrapGc7Install(void* const ctx, const UInt32 sel, void* const arg)
{
    const bool hit = (sel == 7);
    if (singleton().orgGc7Install == 0) { return false; }
    const bool ret = FunctionCast(wrapGc7Install, singleton().orgGc7Install)(ctx, sel, arg);
    if (gGc7 && nredKernelPtrOk(ctx) && (!gGc7N || (hit && !gGc7HitSeen))) {
        if (gGc7N < 2) {
            NRedGc7Rec& r = gGc7Rec[gGc7N];
            r.p7 = *reinterpret_cast<const UInt64*>(static_cast<const UInt8*>(ctx) + 0x40);
            r.sel = sel;
            r.hit = hit ? 1u : 0u;
            ++gGc7N;
            if (hit) { gGc7HitSeen = true; }
        }
    }
    // 270：安装命中（selector==7）即开窗（幂等；与 0x93f64 的置位取先到者）。
    if (gP7Hold && sel == 7) { gP7Armed = true; }
    return ret;
}

UInt32 X5000HWLibs::wrapGc7SetSlot(void* const obj, const UInt32 id, const UInt64 val)
{
    // 270：窗口内拦截 client7 的写 0（只读 obj+0x40；P7 空/非法 ⇒ 透传）。
    if (gP7Hold && gP7Armed && id == 7 && val == 0 && nredKernelPtrOk(obj)) {
        const UInt8* const p7 = *reinterpret_cast<const UInt8* const*>(static_cast<const UInt8*>(obj) + 0x40);
        if (nredKernelPtrOk(p7)) {
            gP7Seen = reinterpret_cast<UInt64>(p7);
            ++gP7Skip;
            return id;   // 跳过写 0；返回值仍为原件语义（eax=id）
        }
    }
    if (gP7Hold && id == 7 && val == 0) { ++gP7Cleared; }
    if (singleton().orgGc7SetSlot == 0) { return id; }
    const UInt32 ret = FunctionCast(wrapGc7SetSlot, singleton().orgGc7SetSlot)(obj, id, val);
    if (gGc7 && val == 0 && id == 7 && gGc7xSeen == 0) {
        gGc7xSlot = id;
        gGc7xVal = val;
        gGc7xSeen = 1;
    }
    return ret;   // 原件 eax=id；原样透传，避免改变调用方可见的返回值
}

// B81 观测点 A：拦截 Apple 就绪门谓词 0x6839e（读 C2PMSG_90(0x29A)→setne→ret al）。
//  机制：routeFunction 把 0x6839e 入口改跳本 wrapper；wrapper 原样调回 org（trampoline）并返回其值，
//  仅在结果变化时打印（≤4 行）。零行为改动：返回值/控制流逐字不变。
//  ⚠️ 不新增任何硬件访问（只截获 Apple 已有的那次读）；route 失败时 org=0 ⇒ 返回 0（与"谓词为假"
//  等价，不改变失败路径）。只走 nredTraceLine 内存环（深栈纪律：不走 SYSLOG）。
UInt8 X5000HWLibs::wrapPred90(void* const ctx)
{
    const UInt8 r = (singleton().orgPred90 != 0)
                        ? FunctionCast(wrapPred90, singleton().orgPred90)(ctx)
                        : 0;
    static UInt8 sPrev = 0xFFu; static unsigned sSeq = 0;
    if (r != sPrev) { sPrev = r; if (++sSeq <= 4) NRED_TRACE("pred90: r=%u seq=%u", (unsigned)r, sSeq); }
    return r;
}

// B88 观测 R2'（95bda 收口规格 §3.1）：拦截 Apple 0x93f64（查位过滤器）。
//  机制：routeFunction 改 0x93f64 入口跳本 wrapper；原样调回 org（trampoline）并**原样返回**其值。
//  观测：bm=*(obj+0x7B8)（1 跳，先 nredKernelPtrOk，非法按 0）；仅 id∈{5,8} 时打一行
//  （B89 (b1)：收窄掉"id 变化就打"支——该支几乎恒真、单轮可写 ≥32 行、淹没 256 行环）。
//  ⚠️ 零行为改动（返回 al 逐字透传）、不新增寄存器访问、不受门控；小缓冲 nredTraceLine（深栈纪律）。
UInt8 X5000HWLibs::wrap93F64(void* const obj, const UInt32 id)
{
    // B90 Q4①：入口无条件行（静态哨兵限前 4 次）——区分"(i) 从未被调" vs "(ii) 被调但 id 非 5/8"。
    //  ⚠️ 仍原样透传返回（下方 org 调用与返回值逐字不变）；不受门控；小缓冲 nredTraceLine。
    {
        static unsigned sEnterN = 0;
        if (++sEnterN <= 4) {
            char line[128];
            const int n = snprintf(line, sizeof(line), "flt93f64: enter n=%u obj=%llx id=%u\n",
                                   sEnterN,
                                   static_cast<unsigned long long>(reinterpret_cast<UInt64>(obj)),
                                   static_cast<unsigned>(id));
            if (n > 0) { nredTraceLine(line, n); }
        }
    }
    const UInt8 ret = (singleton().org93F64 != 0)
                          ? FunctionCast(wrap93F64, singleton().org93F64)(obj, id)
                          : 0;
    // 270：能力位检查命中（id==7 且位为 1）也开窗（与 0x93d18 的置位取先到者，幂等）。
    if (gP7Hold && id == 7 && ret != 0) { gP7Armed = true; }
    const UInt32 bm = nredKernelPtrOk(obj)
        ? *reinterpret_cast<const UInt32*>(static_cast<const UInt8*>(obj) + 0x7B8) : 0;
    if (id == 5 || id == 8) {
        char line[128];
        const int n = snprintf(line, sizeof(line), "flt93f64: obj=%llx bm=%08x id=%u ret=%u\n",
                               static_cast<unsigned long long>(reinterpret_cast<UInt64>(obj)),
                               static_cast<unsigned>(bm), static_cast<unsigned>(id),
                               static_cast<unsigned>(ret));
        if (n > 0) { nredTraceLine(line, n); }
    }
    return ret;
}

// 第八步观测（2026-09-28 第十九轮）：`0x8b10e` 的 4 个候选失败出口判据函数（**只读**，仅记录返回值）。
//  签名依据（离线反汇编）：C `0xa1c3c(void*) -> void*`；F `0x93880(void*) -> void*`；
//  G `0x928f4(void*) -> void*`；D `0xa285f(void* obj, void* param) -> bool`。
void* X5000HWLibs::wrapTtlCollect(void* const ctx)
{
    if (singleton().orgTtlCollect == 0) { return nullptr; }
    void* const ret = FunctionCast(wrapTtlCollect, singleton().orgTtlCollect)(ctx);
    if (checkKernelArgument("-NRedAccelLog")) {
        NRED_TRACE("ttl-coll: ctx=%llx ret=%llx (NULL would fail 0x8b10e)",
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(ctx)),
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(ret)));
    }
    // B48 挂点 A：收集器 dump（纯只读、零行为改动）。
    //  `ret`＝收集器本体（0x8b5de 返回 rbx＝查询 3/id=8 对象）；其 +0＝查询 1（id=552）对象
    //  （0x8b6b6 `movq %r15,(%rbx)`）。真机死因：`*(collector+0x58)==0`（interface#3 缺失）。
    //  本 dump 输出收集器 +0 指向对象的前 0x58 字节 ×2 条（0xB0 总量；单行 hex 224 字符过长
    //  ⇒ 按方案文档 §5 拆两条防截断）。⚠️ 非空门：ret 与 *(void**)ret 任一为空即只打哨兵行。
    //  深栈纪律：只走 nredTraceLine（禁 SYSLOG/FileIO/dumpNow）。
    if (checkKernelArgument("-NRedAccelLog")) {
        void* const q1obj = (nredKernelPtrOk(ret)) ? *reinterpret_cast<void**>(ret) : nullptr;
        if (ret == nullptr || q1obj == nullptr) {
            NRED_TRACE("coll-dump: skip (coll=%llx q1=%llx)",
                       static_cast<unsigned long long>(reinterpret_cast<UInt64>(ret)),
                       static_cast<unsigned long long>(reinterpret_cast<UInt64>(q1obj)));
        } else {
            const auto* const p = static_cast<const UInt8*>(q1obj);
            char hex[185];
            for (UInt32 part = 0; part < 2; part += 1) {
                for (UInt32 k = 0; k < 0x58; k += 1) {
                    snprintf(hex + k * 2, 3, "%02x", p[part * 0x58 + k]);
                }
                NRED_TRACE("coll-dump: +%x b=%s", part * 0x58, hex);
            }
        }
    }
    // B52 补注册 interface#3（侦察式、单变量；介入内存：仅写 ret+0x48/0x50/0x58 三字段）。
    //  ⚠️ 层级判定（反编译 + B48 真机双证据）：`ret`＝查询 3(id=8)对象（0x8b5de 返回 plVar3），
    //    `*ret`＝lVar1（id=552 对象＝接口表载体，0x8b969 构造 +0x8b9a5 注册 #1/#2）。
    //    接口表 stride 0x18：index@+idx*0x18、data@+idx*0x18+8、obj@+idx*0x18+0x10。
    //    B48 coll-dump 实测 *#(ret)=lVar1 的 +0=1（#1 index）✓、+0x58=0（#3 未注册）✓
    //    ⇒ 死因字段＝**`*ret` 的 +0x58**（非 ret 本体的 +0x58——0x8b8e7 消费的是 *p 的接口表）。
    //  本块把 #3 槽（lVar1+0x48/0x50/0x58）从 (0,0,0) seed 为 (3,0,obj3=**查询 800 对象**)。
    //  【c1 修正（B54）】obj3 取 tbl+0x28——0x8b9a5(lVar1,1,lVar2,local_30) 写
    //  lVar1+0x10+1*0x18＝lVar1+0x28＝lVar2（查询 id=800 的返回对象）；反编译亲见。
    //  判据链（0x93f76→0xa3138→0x8b8e7/0x8bbc4）要求 #3 obj 的 **+0xe8** 指向 base SWIP
    //  seq 表（+0x28/0x30）且表[0]≠0——自引用（B52 版）在第二跳即失败（lVar1+0xe8=0）；
    //  查询 800 对象是候选载体（若其 +0xe8 仍为 0 ⇒ 转 c2 造两级链）。
    //  ⚠️ 非空门：ret/*ret 任一为空 ⇒ 跳过（不 seed、打 skip 哨兵）。深栈：nredTraceLine。
    //  ⚠️ 风险登记：obj3（查询 800 对象）的 +0xe8 若仍为 0 ⇒ 判据仍失败（c2 备选）；
    //     0x95bda 逐项校验需真实 SWIP 拓扑数据；介入面可能滚到版本数据服务。
    if (checkKernelArgument("-NRedAccelLog")) {
        void* const tbl = (nredKernelPtrOk(ret)) ? *reinterpret_cast<void**>(ret) : nullptr;
        if (ret == nullptr || tbl == nullptr) {
            NRED_TRACE("pool: iface3 skip (ret=%llx tbl=%llx)",
                       static_cast<unsigned long long>(reinterpret_cast<UInt64>(ret)),
                       static_cast<unsigned long long>(reinterpret_cast<UInt64>(tbl)));
        } else {
            uintptr_t* const slots = static_cast<uintptr_t*>(tbl);
            const uintptr_t obj3 = slots[0x28 / sizeof(uintptr_t)];   // +0x28 = #1 obj（查询 800 对象）
            slots[0x48 / sizeof(uintptr_t)] = 3;                      // #3 index
            slots[0x50 / sizeof(uintptr_t)] = 0;                      // #3 data
            slots[0x58 / sizeof(uintptr_t)] = obj3;                   // #3 obj
            NRED_TRACE("pool: iface3 seeded index=3 obj=%llx",
                       static_cast<unsigned long long>(obj3));
        }
    }
    return ret;
}

void* X5000HWLibs::wrapTtlAllocA(void* const ctx)
{
    if (singleton().orgTtlAllocA == 0) { return nullptr; }
    void* const ret = FunctionCast(wrapTtlAllocA, singleton().orgTtlAllocA)(ctx);
    if (checkKernelArgument("-NRedAccelLog")) {
        NRED_TRACE("ttl-allocA: ctx=%llx ret=%llx (NULL -> exit 0x8b3d4)",
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(ctx)),
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(ret)));
    }
    return ret;
}

void* X5000HWLibs::wrapTtlAllocB(void* const ctx)
{
    if (singleton().orgTtlAllocB == 0) { return nullptr; }
    void* const ret = FunctionCast(wrapTtlAllocB, singleton().orgTtlAllocB)(ctx);
    if (checkKernelArgument("-NRedAccelLog")) {
        // 直接读"接口表"：`0x8bb9f(pool, idx)` = `*(pool)` 作表、表项 stride 0x18、字段 +0x10 = 对象。
        UInt64 e4 = 0, e1 = 0, e2 = 0;
        void*     tbl = nullptr;
        if (nredKernelPtrOk(ctx)) {
            tbl = *reinterpret_cast<void**>(ctx);
            if (nredKernelPtrOk(tbl)) {
                e4 = reinterpret_cast<UInt64*>(tbl)[4 * 3 + 2];
                e1 = reinterpret_cast<UInt64*>(tbl)[1 * 3 + 2];
                e2 = reinterpret_cast<UInt64*>(tbl)[2 * 3 + 2];
            }
        }
        NRED_TRACE("ttl-allocB: ctx=%llx ret=%llx tbl=%llx e1=%llx e2=%llx e4=%llx (NULL -> exit 0x8b3cc)",
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(ctx)),
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(ret)),
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(tbl)),
               static_cast<unsigned long long>(e1), static_cast<unsigned long long>(e2),
               static_cast<unsigned long long>(e4));
    }
    // P7f 包E″：gABRet 改由既有 wrapTtlAllocB 在返回处写入（不再新增 wrapAllocB 钩子，
    //  因 0x928f4 已被本 hook 占位，第二个 wrapper 不生效）。仅写标量，不新增打印。
    gABRet = static_cast<SInt32>(reinterpret_cast<UInt64>(ret));
    return ret;
}

// P7c 包D：判据③ `0x9405e`（IpiInitializeIpInterfaces）只读钩子——只记返回值，逐字透传。
//  判假条件（L2-j定因 §D.6）：某 id 查表命中但 `0x93d18` 建子对象返假 ⇒ 断言 ttl_ipi.c:668 ⇒ 返 0。
//  ⚠️ 只读：不改返回值/内存/控制流；不新增 SYSLOG（只写标量供头部戳记）。
UInt32 X5000HWLibs::wrapIpiInit(void* const a, const UInt32 b, void* const c)
{
    const UInt32 ret = (singleton().orgIpiInit != 0)
                           ? FunctionCast(wrapIpiInit, singleton().orgIpiInit)(a, b, c) : 0;
    // 判据③的真实语义＝**布尔低位**：调用者 `0x928f4` 在 `9294b: testb %al,%al`（反汇编确凿，
    //  非 testq %rax,%rax）⇒ 只看 al（低 8 位），高位是残留垃圾 ⇒ 记 `ret & 0xFF`。
    gP3 = static_cast<SInt32>(ret & 0xFF);
    return ret;
}

// ─── 223：判据④ stage0 裁剪（`-NRedTtlTrim`，默认关）─────────────────────────────────
//  目的：原件 `0x929ac`（TlsExecuteIpEntrySeq）的 stage0 正向循环按 client 数组顺序逐个调用
//  `0x9440f`；R222 实测：stage0 的最后一次 `0x9440f` 是 client 15 且返回 0（失败）⇒ r12=0 ⇒ 判据④失败；
//  其余 client 已在此之前被逐条处理（我方 dispatch 对 2/5/7/8/9/14 返回 0，对应 `[swres]` 的 n=1）。
//  ⇒ 裁掉 client 15/10 后，最后一个被处理的 client 14 成功 ⇒ r12=1 ⇒ 判据④应可过。本函数按**文档 v2** 只接管 stage0，
//  以本机真实 client 集合运行原 `0x9440f`：client 15/10 **完全不触碰**（不调钩子/不置位图/不改 r12/progress）。
//
//  控制流逐条对齐 `kb/序列数据/反汇编/HWLibs_full.asm`（0x929ac–0x92be8），并修正实施文档的三处偏差：
//   ① 循环前有 `0x943b1(ctx+8, stage, arg)`（`929de`，文档 §2.2 未列）；
//   ② `0x9440f` **返回 0＝失败、非 0＝成功**（`92a78 testb; je 92a97`：0 走失败支；非 0 走成功支）。
//      r12 的**最终值**由 `92b7d movl -44(%rbp),%r12d` 恢复，而 `-44` 只在失败支被置 0（`92ac9`）：
//        成功支 → 写进度 `ctx+0x94=i+1`，`-44` 保持 1 ⇒ 最终 r12=1（下一轮顶部继续）；
//        失败支 → `0x92e71` + 置位 `ctx+0x160`（bit=client）+ `r12=0`，`-44=0` ⇒ 最终 r12=0（下一轮顶部退出）。
//      ⇒ 循环在**首个失败的 client** 处停下；全部成功则 r12=1（返回 1＝成功）。
//      R222 佐证：`[ttlchain4] n=2`（stage0 返回 0 后才进 stage2）与调用方 `9295b testb; jne` 一致 ⇒ 0＝失败。
//      ⚠ `[swres] r[15]=6` 是我方 dispatch 的返回值，不是 `0x9440f` 的返回，不能直接当极性证据。
//   ③ 失败/0 结尾分支在原件里还会走 `0x92b97` 的 TTL_LOG（ttl_tls.c:357 "SWIP init/power-up failed"）；
//      本实现不复刻该**纯日志**调用（不影响返回值/状态），由 `[ttltrim]` 行替代。
//  ⚠ `ctx+0x1c` 不是写点（原件写的是栈局部 `-60(%rbp)`；文档 §2.2 的 `ctx+0x1c=client` 有误）。
//  ⚠ 接管范围＝stage 0（正向）与 stage 2（反向）；stage 1/3/4/5/6/7/8 一律透传原件。
//  ⚠ 调用方 `wrapIpEntrySeq` 的第三参保持 `UInt32`（关态逐字等价）：stage0/2 的调用点
//     (`928f4`/`92dbe`) 均 `xor edx,edx` ⇒ arg 恒 0，故此处零扩展不丢信息；其它 stage 不走本文件的两个裁剪函数。
//  ⚠ 两级指针校验（ctx / ctx+0、ctx+8）；任一失败 ⇒ 返回 false（调用方透传原件）。
//  返回 true 表示已接管并写出 `retOut`（0=失败/1=成功，同原件 `r12&1`）。
static bool nredTrimStage0(void* const ctx, const UInt32 stage, const UInt64 arg, UInt32& retOut)
{
    nredTrimResetFail(stage);   // 240：入口清零（失败识别只在本次调用内累计）
    if (gHwlibsSlide == 0 || !nredKernelPtrOk(ctx)) { nredTrimRecord(stage, 0, 0, 0, 0, 1); return false; }

    UInt8* const base = static_cast<UInt8*>(ctx);
    if (!nredKernelPtrOk(*reinterpret_cast<void**>(base + 0x00)) ||
        !nredKernelPtrOk(*reinterpret_cast<void**>(base + 0x08))) {
        nredTrimRecord(stage, 0, 0, 0, 0, 1);
        return false;
    }

    // 原件内部调用（slide + 归零 VM 偏移；`0x9440f` 入口已被 `wrapMode9440f` 占位 ⇒ 调用它等价于原路径）。
    using TrimCall = void (*)(void*, UInt32, UInt64);                  // 0x943b1 / 0x94166
    using TrimHook = void (*)(void*, UInt32, UInt32, UInt64, UInt32);  // 0x91222(root, client, stage, arg, mode)
    using TrimFail = void (*)(void*, UInt32, UInt32, UInt64);          // 0x92e71(root, client, stage, arg)
    using TrimStep = UInt8 (*)(void*, UInt32, UInt32, UInt64);         // 0x9440f(obj, client, stage, arg)
    const auto pre  = reinterpret_cast<TrimCall>(gHwlibsSlide + 0x943B1);
    const auto post = reinterpret_cast<TrimCall>(gHwlibsSlide + 0x94166);
    const auto hook = reinterpret_cast<TrimHook>(gHwlibsSlide + 0x91222);
    const auto fail = reinterpret_cast<TrimFail>(gHwlibsSlide + 0x92E71);
    const auto step = reinterpret_cast<TrimStep>(gHwlibsSlide + 0x9440F);

    UInt32 n = 0, skip = 0, last = 0, r12 = 1;

    // 原件顺序：`0x943b1(*(ctx+8), stage, arg)`（929de）→ 读 count（929ed）；且每次调用前重新读
    //  `*(ctx)`／`*(ctx+8)`（92a33/92a64/92a97/92adf）⇒ 以下同法就地重取、不缓存。
    pre(*reinterpret_cast<void**>(base + 0x08), stage, arg);
    const UInt32 count = *reinterpret_cast<UInt32*>(base + 0x10);
    for (UInt32 i = 0; i < count; ++i) {
        const UInt32 client = *reinterpret_cast<UInt32*>(base + 0x14 + i * 4);
        if (client == 0) { r12 = 0; break; }                       // 0 结尾 ⇒ 原件走 92b97（r12=0）
        if (client == 15 || client == 10) { ++skip; continue; }    // 裁剪：完全不触碰（无钩子/无位图/无 progress）
        if ((arg & 1) != 0 && client != 10) { continue; }          // 原 `cl&1` 判定（TlsSwInit 传 arg=0 ⇒ 不触发）
        last = client;
        ++n;
        hook(*reinterpret_cast<void**>(base + 0x00), client, stage, arg, 0);
        const UInt8 ok = step(*reinterpret_cast<void**>(base + 0x08), client, stage, arg);
        if (ok != 0) {                                             // 成功（原件：写进度 ctx+0x94=i+1；r12 保持 1）
            *reinterpret_cast<UInt32*>(base + 0x94) = i + 1;
            r12 = 1;
        } else {                                                   // 失败（原件：0x92e71 + 置位 ctx+0x160 + r12=0）
            fail(*reinterpret_cast<void**>(base + 0x00), client, stage, arg);
            *reinterpret_cast<UInt32*>(base + 0x160) |= (1U << (client & 31));
            gTrimStat[(stage == 2) ? 1 : 0].failMask |= (1U << (client & 31));
            if (gTrimStat[(stage == 2) ? 1 : 0].firstFail == 0xFFFFFFFFU) {
                gTrimStat[(stage == 2) ? 1 : 0].firstFail = client;
            }
            ++gTrimStat[(stage == 2) ? 1 : 0].nFail;
            r12 = 0;
        }
        hook(*reinterpret_cast<void**>(base + 0x00), client, stage, arg, 1);
        if (r12 == 0) { break; }                                   // 原件：下一轮顶部 `testb $1,%r12b` 退出
    }
    post(*reinterpret_cast<void**>(base + 0x08), stage, arg);
    retOut = r12 & 1;

    nredTrimRecord(stage, n, skip, last, r12, 0);
    return true;
}

// ─── 228：stage2 反向路径裁剪（`-NRedTtlTrim`）────────────────────────────────────
//  原件（92bfa→92c24→92c5c–92d46）逐指令复刻：
//   · count/起点 = *(u32*)(ctx+0x94)（92c2e），索引 rbx 从 count 递减；
//   · 读 `*(u32*)(ctx+0x10 + 4*rbx)`（即 client[rbx-1]）；client==0 ⇒ r12=0 + 结束；
//   · 每 client：0x91222(pre) → 0x9440f → 失败（0）：0x92e71 + 置位 ctx+0x160 + r12=0（**粘滞**，
//     不中断循环）→ decq → 0x91222(post) → rbx!=0 继续；
//   · 不写 ctx+0x94；无 `arg&1` 分支；尾部 0x94166；返回 r12&1（1=成功）。
//  裁剪（仅 stage2）：跳过 client 15／10／3（3＝BGM HW_INIT；R-228 判读其失败为独立问题）。
//  跳过 = 完全不触碰（不调钩子/不调 0x9440f/不置位图/不改 r12），但仍 decq 以保持遍历节奏。
//  ⚠ stage0 **不**跳过 3（保持 227 的单变量口径）；stage0 的失败语义与 227 逐字相同。
static bool trimRunStage2(void* const ctx, const UInt32 stage, const UInt64 arg, UInt32& retOut)
{
    nredTrimResetFail(stage);   // 240：入口清零
    if (gHwlibsSlide == 0 || !nredKernelPtrOk(ctx)) { nredTrimRecord(stage, 0, 0, 0, 0, 1); return false; }

    UInt8* const base = static_cast<UInt8*>(ctx);
    if (!nredKernelPtrOk(*reinterpret_cast<void**>(base + 0x00)) ||
        !nredKernelPtrOk(*reinterpret_cast<void**>(base + 0x08))) {
        nredTrimRecord(stage, 0, 0, 0, 0, 1);
        return false;
    }

    using TrimCall = void (*)(void*, UInt32, UInt64);
    using TrimHook = void (*)(void*, UInt32, UInt32, UInt64, UInt32);
    using TrimFail = void (*)(void*, UInt32, UInt32, UInt64);
    using TrimStep = UInt8 (*)(void*, UInt32, UInt32, UInt64);
    const auto pre  = reinterpret_cast<TrimCall>(gHwlibsSlide + 0x943B1);
    const auto post = reinterpret_cast<TrimCall>(gHwlibsSlide + 0x94166);
    const auto hook = reinterpret_cast<TrimHook>(gHwlibsSlide + 0x91222);
    const auto fail = reinterpret_cast<TrimFail>(gHwlibsSlide + 0x92E71);
    const auto step = reinterpret_cast<TrimStep>(gHwlibsSlide + 0x9440F);

    UInt32 n = 0, skip = 0, last = 0, r12 = 1;
    pre(*reinterpret_cast<void**>(base + 0x08), stage, arg);      // 92c41：先 0x943b1
    UInt64 idx = *reinterpret_cast<UInt32*>(base + 0x94);         // 92c2e/92c35：count=*(ctx+0x94)
    while (idx != 0) {
        const UInt32 client = *reinterpret_cast<UInt32*>(base + 0x10 + idx * 4);   // 92c5c
        if (client == 0) { r12 = 0; break; }                       // 92c67 ⇒ 92d4b（Invalid entry）：r12=0 结束
        if (client == 15 || client == 10 || client == 3) { ++skip; --idx; continue; }  // 裁剪：完全不触碰（仅 stage2 含 3）
        last = client;
        ++n;
        hook(*reinterpret_cast<void**>(base + 0x00), client, stage, arg, 0);
        const UInt8 ok = step(*reinterpret_cast<void**>(base + 0x08), client, stage, arg);
        if (ok == 0) {                                             // 92c9a：失败 ⇒ 0x92e71 + 位图 + r12=0
            fail(*reinterpret_cast<void**>(base + 0x00), client, stage, arg);
            *reinterpret_cast<UInt32*>(base + 0x160) |= (1U << (client & 31));
            gTrimStat[(stage == 2) ? 1 : 0].failMask |= (1U << (client & 31));
            if (gTrimStat[(stage == 2) ? 1 : 0].firstFail == 0xFFFFFFFFU) {
                gTrimStat[(stage == 2) ? 1 : 0].firstFail = client;
            }
            ++gTrimStat[(stage == 2) ? 1 : 0].nFail;
            r12 = 0;
        }
        --idx;                                                     // 92ccc decq（在 post 之前）
        hook(*reinterpret_cast<void**>(base + 0x00), client, stage, arg, 1);
    }
    post(*reinterpret_cast<void**>(base + 0x08), stage, arg);
    retOut = r12 & 1;
    nredTrimRecord(stage, n, skip, last, r12, 0);
    return true;
}

// P7c 包D：判据④ `0x929ac`（TlsExecuteIpEntrySeq）钩子——默认只记返回值、逐字透传；
//  `-NRedTtlTrim` 开启且 stage∈{0,2} 时改由 `nredTrimStage0`／`trimRunStage2` 接管
//  （stage0 跳过 15/10；stage2 跳过 15/10/3，其余逐条按原件控制流）；其余 stage 与关态一律透传。
//  ⚠ 原件 `0x9440f` 的 0＝失败（`92a78 testb; je 92a97`）：失败 client 触发 `0x92e71` 与 `ctx+0x160` 位图。
//  ⚠ 第三参保持 466a954 的 `UInt32` 契约（关态逐字等价）：stage0/2 的调用点
//     (`928f4: xor esi,esi; xor edx,edx` / `92dbe: xorl %edx,%edx`) arg 恒 0；其余调用点
//     stage∈{3,4,5,6,7,8} 一律透传，必须维持旧的“转发时 edx 零扩展”行为（223/224 关态回归根因）。
UInt32 X5000HWLibs::wrapIpEntrySeq(void* const a, const UInt32 b, const UInt32 c)
{
    // 观测通道修复（需求 v2）**M1：进入 TTL 阶段**（`0x929ac` 包装入口）⇒ 门控开时打标记并刷一次。
    //  ⚠ 位置＝函数体第一行之前（早于 org 调用）；不触碰 `a`／`b`／`c`／`ret` ⇒ 返回值/控制流/ABI 不变。
    if (gTraceAtIpSeq) { nredTraceMark(gFlushedAtIpSeq, kMarkM1, static_cast<int>(sizeof(kMarkM1) - 1)); }
    UInt32 ret = 0;
    bool handled = false;
    if (gTtlTrim) {
        if (b == 0) { handled = nredTrimStage0(a, b, static_cast<UInt64>(c), ret); }
        else if (b == 2) { handled = trimRunStage2(a, b, static_cast<UInt64>(c), ret); }
    }
    if (!handled) {
        // 271：先取 cast 结果并判空——org 为 0 时绝不 `callq *%rax`（r270 RIP=0 trap）。
        const auto org = FunctionCast(wrapIpEntrySeq, singleton().orgIpEntrySeq);
        if (org != nullptr) {
            ret = org(a, b, c);
        } else {
            ret = 0;                 // 苹果极性：0＝失败（fail-closed）
            gIpSeqCast0 = 1;
        }
    }
    // P7f 包E″：**无条件记录前 3 次**（R6 证明"按假设过滤"全部落空 ⇒ 先看真实形态再谈过滤）。
    //  RA＝`__builtin_return_address(0)` − kcSlide ⇒ 归零 VM；kcSlide==0 时记原始 RA（供判读）。
    //  ★三槽按到达顺序、不覆盖；第 4 次起只 ++gIpSeqN（限流硬约束：只写标量、不入环、不打印）。
    {
        const UInt64 ra = (singleton().kcSlide != 0)
            ? (reinterpret_cast<UInt64>(__builtin_return_address(0)) - singleton().kcSlide)
            : reinterpret_cast<UInt64>(__builtin_return_address(0));
        const UInt32 al = static_cast<UInt32>(ret & 0xFF);
        ++gIpSeqN;
        if (gSeq1Sub == 0xFFFFFFFFU) { gSeq1RA = ra; gSeq1Sub = b; gSeq1Al = al; }
        else if (gSeq2Sub == 0xFFFFFFFFU) { gSeq2RA = ra; gSeq2Sub = b; gSeq2Al = al; }
        else if (gSeq3Sub == 0xFFFFFFFFU) { gSeq3RA = ra; gSeq3Sub = b; gSeq3Al = al; }
    }
    // H1 观测集 N-2（2026-10-18）：判据④ 的**调用点判别**（二分 ③/④ 的关键读数）。
    //  为什么必须按调用点分档：`0x929ac` 有**三个**调用点，分属三个函数：
    //    `92956`（∈`0x928f4`＝**判据④**）／`92dcf`（∈`0x92dbe`）／`92e5e`（∈`0x92e28`）
    //  ⇒ 只记"总次数/返回值"**无法归属**（R13 的 `n=2` 就是这样被误读成"④跑过"的）。
    //  返回地址（反汇编逐字核对）：`9295b testb %al,%al`／`92dd4 movl %eax,%r14d`／`92e63 movl (%rbx),%ecx`。
    //  ⚠ **纯标量、零解引用**：`__builtin_return_address(0)` 取的是寄存器值，不解引用任何内存。
    //  ⚠ 门控关 ⇒ 零写入（不进本块）。
    if (gTtlBisect) {
        const UInt64 ra = reinterpret_cast<UInt64>(__builtin_return_address(0));
        const UInt64 off = (gHwlibsSlide != 0) ? (ra - gHwlibsSlide) : ra;
        gC4off = off;
        ++gC4n;
        gC4ret = static_cast<UInt32>(ret & 0xFF);   // 判据④ 的真实语义＝布尔低位（调用者 `testb %al,%al`）
        if (off == 0x9295B) { gC4site = 1; ++gC4a4; }        // ← **判据④**（`0x928f4` 内）
        else if (off == 0x92DD4) { gC4site = 2; ++gC4b4; }   // `0x92dbe`
        else if (off == 0x92E63) { gC4site = 3; ++gC4c4; }   // `0x92e28`
        else { gC4site = 0; }                                 // 未匹配 ⇒ 记 `off` 原值供追加判据
    }
    gP4 = static_cast<SInt32>(ret & 0xFF);
    // 270 兜底：stage1 遍历返回即关窗（防 client7 未到达时永久 armed）。
    if (gP7Hold) { gP7Armed = false; }
    return ret;
}

// P7f 包E″：不再新增 wrapAllocB 钩子（0x928f4 已被既有 wrapTtlAllocB 占位，
//  第二个 wrapper 不生效）。gABRet 由既有 wrapTtlAllocB 在返回处写入。

bool X5000HWLibs::wrapTtlCheckD(void* const obj, void* const param)
{
    // B62：入口自证（**零解引用**：只打参数值）——纳入门控 `-NRedAccelLog`，
    //  使"基线轮"（不带该 boot-arg）在这条路径上**我方打印整体静默** ⇒ 单变量判
    //  "是不是我方探针把这条链搞崩的"。⚠️ 不改任何读值/判定/控制流/返回值。
    if (checkKernelArgument("-NRedAccelLog")) {
        NRED_TRACE("chkD-enter: obj=%llx p2=%llx",
                   static_cast<unsigned long long>(reinterpret_cast<UInt64>(obj)),
                   static_cast<unsigned long long>(reinterpret_cast<UInt64>(param)));
    }
    // B66（2026-10-14）：修「一跳错位」＋补读 TTL 设备描述符（依据＝技术经理定因报告
    //  `tmp/tech-manager/容器为空定因-2026-10-14.md` §二A／§七「动作 1」）。
    //  ⚠️ **零行为改动**：只改「读值来源与编码字段」，不改判定/控制流/返回值、不新增/删除门控、
    //  不新增 hook；仍**无条件**执行（基线轮也要取值）、仍在 Apple 原函数调用之前、仍只走
    //  `nredTraceLine`（内存环），仍**单行 1 次输出**（B55：我方打印越少对该链扰动越小）。
    //  ── 口径更正（**旧 `off=e001` 行作废，勿与新行混读**）───────────────────────────
    //  ① `tbl = *(obj)` 即 **TTL 服务注册表本体**（552 B ＝ 23 槽 × 24 B；旧日志里就叫 `tbl`），
    //     不是"容器"；旧编码行的 `B=*(tbl)` 是**表第 0 号槽的槽号**——建表时写死 0、此后无任何
    //     `idx=0` 的注册调用 ⇒ **恒 0、不表意** ⇒ 本行**删除**该字段（避免再次误读）。
    //  ② 真链：`0x8b8e7(p)` ＝ `表*(p)` 的**槽 3**（表项 stride 0x18、对象指针在 +0x10 ⇒ `+0x58`）
    //     ⇒ `dev = *(tbl+0x58)`；旧行写成 `E = *(B+0x58)`，**多了一跳**，恒被指针门短路成 0。
    //  ── 编码规则（固定长度、单行、无堆/栈大缓冲）─────────────────────────────────
    //    行 `memdump: off=e002 b=<9×64 位 hex>`，字段次序：
    //      obj, tbl=*(obj)〔**实例配对键**：同轮可见 ≥3 次 TTL 初始化，跨实例混读会误判（B56 教训）〕,
    //      dev=*(tbl+0x58)〔TTL 设备描述符〕,
    //      boot=*(u32*)(dev+0x220)〔启动模式；Apple `ttlDevGetBootMode` 读 544(dev)，**32 位**〕,
    //      Ee8=*(dev+0xE8)〔基础 SWIP 序列指针；Apple `ttlDevGetBaseSwipSeq` 读 232(dev)〕,
    //      base28=*(Ee8+0x28), t28_0=*(base28), base30=*(Ee8+0x30), t30_0=*(base30)
    //  长度自证：前缀 20 + 9×16 hex ＝ 164（+换行 165）≤ 191（`TraceRing::kLineBytes`−1）⇒ 完整不截断。
    //  缓冲 static char line[176]（不占栈）；**逐跳先 nredKernelPtrOk**（不合法 ⇒ 该字段按 0 编码、
    //  不解引用）；`dev+0x220` 只取低 32 位（高 32 位属相邻字段，不得当"高半"读）。
    {
        void* const tbl = (nredKernelPtrOk(obj)) ? *reinterpret_cast<void**>(obj) : nullptr;
        UInt64 dev = 0, boot = 0, Ee8 = 0, base28 = 0, base30 = 0, t28_0 = 0, t30_0 = 0;
        if (nredKernelPtrOk(tbl)) {
            dev = *reinterpret_cast<UInt64*>(static_cast<UInt8*>(tbl) + 0x58);
        }
        void* const devp = reinterpret_cast<void*>(dev);
        if (nredKernelPtrOk(devp)) {
            boot = *reinterpret_cast<UInt32*>(static_cast<UInt8*>(devp) + 0x220);
            Ee8  = *reinterpret_cast<UInt64*>(static_cast<UInt8*>(devp) + 0xe8);
        }
        void* const f = reinterpret_cast<void*>(Ee8);
        if (nredKernelPtrOk(f)) {
            base28 = *reinterpret_cast<UInt64*>(static_cast<UInt8*>(f) + 0x28);
            base30 = *reinterpret_cast<UInt64*>(static_cast<UInt8*>(f) + 0x30);
        }
        if (nredKernelPtrOk(reinterpret_cast<void*>(base28))) {
            t28_0 = *reinterpret_cast<UInt64*>(reinterpret_cast<UInt8*>(base28));
        }
        if (nredKernelPtrOk(reinterpret_cast<void*>(base30))) {
            t30_0 = *reinterpret_cast<UInt64*>(reinterpret_cast<UInt8*>(base30));
        }
        // B66：单行 9 字段（标签 e002 ＝新口径；`boot`/`Ee8` 即新补的两值，`dev` 见上）。
        static char line[176];
        const int n =
            snprintf(line, sizeof(line),
                     "memdump: off=e002 b=%016llx%016llx%016llx%016llx%016llx"
                     "%016llx%016llx%016llx%016llx\n",
                     reinterpret_cast<UInt64>(obj), reinterpret_cast<UInt64>(tbl), dev, boot,
                     Ee8, base28, t28_0, base30, t30_0);
        if (n > 0) { nredTraceLine(line, n); }
        // B73：`smustate=` 与 `attempted=` 移至**无门控**块（B72 实证 `-NRedAccelLog` 未
        //  生效 ⇒ 原处未落盘；此处与 `memdump: off=e002` 同一无条件块，B72 已证会落盘）。
        //  内容＝既有 getSmu13ProbeState()（bit63=InternalHwInit 被调用过、bit60=首发送进过、
        //  bit62/61=等待门成败、bit0-3=PowerUpConfig 各步）＋ 紧邻打印既有单例标志
        //  `smu13InitAttempted`（**不新造语义**：它就是 wrapSmu90SendMessageWithParameter
        //  首发送块里置的那个标志——该 wrapper 是死代码，故该值同时也判"死路径是否跑过"）。
        //  记录行逐字：`smustate=%llx attempted=%d n=%u`（定长 ≤191）；nredTraceLine 内存环。
        //  B77③：追加 `n=`（本行被打印的次数，进入即自增并打自增后值）——解决
        //  "smustate=0 attempted=0 与上一轮值完全相同 ⇒ 无法辨别新鲜度"的问题。
        {
            static char lineSm[96];
            static unsigned sSmStatePrints = 0;
            const int nSm =
                snprintf(lineSm, sizeof(lineSm), "smustate=%llx attempted=%d n=%u\n",
                         static_cast<unsigned long long>(NRed::singleton().getSmu13ProbeState()),
                         singleton().smu13InitAttempted ? 1 : 0, ++sSmStatePrints);
            if (nSm > 0) { nredTraceLine(lineSm, nSm); }
        }
    }
    if (singleton().orgTtlCheckD == 0) { return false; }
    const bool ret = FunctionCast(wrapTtlCheckD, singleton().orgTtlCheckD)(obj, param);
    if (checkKernelArgument("-NRedAccelLog")) {
        // `obj` 此处即收集器（`0x8b10e` 的 r14）；同时读出它的接口表快照，判断 idx=4 是否已注册。
        UInt64 e4 = 0, e1 = 0;
        void*  tbl = nullptr;
        if (nredKernelPtrOk(obj)) {
            tbl = *reinterpret_cast<void**>(obj);
            if (nredKernelPtrOk(tbl)) {
                e4 = reinterpret_cast<UInt64*>(tbl)[4 * 3 + 2];
                e1 = reinterpret_cast<UInt64*>(tbl)[1 * 3 + 2];
            }
        }
        NRED_TRACE("ttl-checkD: obj=%llx param=%llx ret=%d tbl=%llx e1=%llx e4=%llx (false -> exit 0x8b3cc)",
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(obj)),
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(param)), ret ? 1 : 0,
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(tbl)),
               static_cast<unsigned long long>(e1), static_cast<unsigned long long>(e4));
    }
    // B55 三层解引用只读 dump（零行为改动；交叉复核 §4 观测清单）。
    //  链：r15=*(obj+8)（obj=本函数入参，0x92911 调用点传 rdi）；rbx=*r15（0x93f98）；
    //      0x8b8e7(r15)=0x8bbc4(*r15,3)=*(*r15+0x58)（判据实际读的 interface#3）
    //      → 0xa3138: *(0x8b8e7返回+0xe8) → +0x28/0x30 表 → 表项[0] → 0x95bda 校验
    //  一次性判"链在哪一层断"——每层偏移 dump 依据（反编译/disasm 亲见）：
    //    p1=obj（0x93f76 入口 rdi）；rbx=*p1（0x93f98 movq (%r15),%rbx）；
    //    p58=*(p1+0x58)、pp58=*(*p1+0x58)（0x8bbc4 读 obj@+0x10+3*0x18=+0x58 的两跳）；
    //    e8=*(p1+0xe8)（0xa315e MOV RAX,[RCX+0xe8] 的读源）；
    //    t28/t30=*(p1+0x28/0x30)（0xa316a.. 分支取的表指针）。
    //  ⚠️ 非空门：每层先判非空，避免野指针 panic。深栈：nredTraceLine。
    if (checkKernelArgument("-NRedAccelLog")) {
        void* const p1 = obj;
        void* const rbx = (nredKernelPtrOk(p1)) ? *reinterpret_cast<void**>(p1) : nullptr;
        UInt64 p58 = 0, pp58 = 0, e8 = 0, t28 = 0, t30 = 0;
        if (nredKernelPtrOk(p1)) {
            p58 = *reinterpret_cast<UInt64*>(static_cast<UInt8*>(p1) + 0x58);
            e8  = *reinterpret_cast<UInt64*>(static_cast<UInt8*>(p1) + 0xe8);
            t28 = *reinterpret_cast<UInt64*>(static_cast<UInt8*>(p1) + 0x28);
            t30 = *reinterpret_cast<UInt64*>(static_cast<UInt8*>(p1) + 0x30);
        }
        if (nredKernelPtrOk(rbx)) {
            pp58 = *reinterpret_cast<UInt64*>(static_cast<UInt8*>(rbx) + 0x58);
        }
        NRED_TRACE("c3dump: p1=%llx rbx=%llx p58=%llx pp58=%llx e8=%llx t28=%llx t30=%llx",
                   static_cast<unsigned long long>(reinterpret_cast<UInt64>(p1)),
                   static_cast<unsigned long long>(reinterpret_cast<UInt64>(rbx)),
                   static_cast<unsigned long long>(p58),
                   static_cast<unsigned long long>(pp58),
                   static_cast<unsigned long long>(e8),
                   static_cast<unsigned long long>(t28),
                   static_cast<unsigned long long>(t30));
        // B58 c3dump2：判据真正读址的第二/三跳补测（零行为改动，pp58 非空门）。
        //  【指令级依据】判据链＝0x8b8e7(p1)=*(rbx+0x58)=pp58（B50 实测非 0，第一跳已过）→
        //    0xa315e MOV RAX,[RCX+0xe8] 读 ***(pp58+0xe8)***（本行 pp58e8，B50 未测的核心字段）→
        //    0xa316a.. 分支取 +0x28/0x30 表基址（本行 base28/base30）→ 表首项 [0]（本行 t28_0/t30_0，
        //    两个基址都打、标签写明来源）⇒ 一次判“表是否为空/哪一层断”。
        //  ⚠️ 非空门：pp58==0 只打哨兵行；解引用均先判非空。深栈：nredTraceLine。
        if (checkKernelArgument("-NRedAccelLog")) {
            void* const q = (nredKernelPtrOk(reinterpret_cast<void*>(pp58))) ? reinterpret_cast<void*>(pp58) : nullptr;
            if (q == nullptr) {
                NRED_TRACE("c3dump2: skip (pp58=0)");
            } else {
                const UInt64 pp58e8 = *reinterpret_cast<UInt64*>(static_cast<UInt8*>(q) + 0xe8);
                // B84 定因修复（2026-10-15）：① 解引用一律先过 nredKernelPtrOk（本轮 *(q+0x30)
                //  =0x10000000 垃圾值，非空门拦不住 ⇒ page fault；任一跳不合法 ⇒ 后续字段按 0
                //  编码，可机读判据）；② 取址修正：base28/base30 改为基于 pp58e8（与注释自述的
                //  Apple 链 0xa3173/a3182=Ee8+0x28/+0x30 一致，逐指令依据见 c3dump3；原实现读
                //  *(q+0x28/0x30) 与自述不符，读数误导）。观测字段与打印行不变；与 c3dump3 同构。
                UInt64 base28 = 0, base30 = 0, t28_0 = 0, t30_0 = 0;
                if (nredKernelPtrOk(reinterpret_cast<void*>(pp58e8))) {
                    base28 = *reinterpret_cast<UInt64*>(static_cast<UInt8*>(reinterpret_cast<void*>(pp58e8)) + 0x28);
                    base30 = *reinterpret_cast<UInt64*>(static_cast<UInt8*>(reinterpret_cast<void*>(pp58e8)) + 0x30);
                }
                if (nredKernelPtrOk(reinterpret_cast<void*>(base28))) {
                    t28_0 = *reinterpret_cast<UInt64*>(reinterpret_cast<UInt8*>(base28));
                }
                if (nredKernelPtrOk(reinterpret_cast<void*>(base30))) {
                    t30_0 = *reinterpret_cast<UInt64*>(reinterpret_cast<UInt8*>(base30));
                }
                NRED_TRACE("c3dump2: pp58e8=%llx base28=%llx base30=%llx t28_0=%llx t30_0=%llx",
                           static_cast<unsigned long long>(pp58e8),
                           static_cast<unsigned long long>(base28),
                           static_cast<unsigned long long>(base30),
                           static_cast<unsigned long long>(t28_0),
                           static_cast<unsigned long long>(t30_0));
            }
        }
        // B59 c3dump3：**已于 2026-10-18 退役**（H1 观测集卡）。
        //  退役理由：它挂在 `0xA285F`（`wrapTtlCheckD`）上，取到的对象与 `0xa3138` 的取址链
        //  **不同源** ⇒ 第 2 跳即断（R13 实测 `B=0`）⇒ 之后 `E/Ee8/base28/…` 全是守卫默认值 0，
        //  **零信息**且每轮污染判读（R13 已被它误导一次）。其读数已由 **N-3**（挂既有 `tls-create`，
        //  与 `0xa3138` 同源）取代。⇒ 删除后**净减少**一处多跳解引用，扰动面变小。
    }
    return ret;
}
// B67（2026-10-15）：HWLibs `ttl_initialize`（0x8af60；kc 绝对 0x4DEEF60）**唯一新增挂点**。
//  **零行为改动**：wrapper 逐字透传（参数/返回值/控制流原样）；不加门控（无条件执行，与
//  B66 e002 行同性质）；零 SYSLOG、零 panic 文本增量。
//  读数（两行，走 `nredTraceLine` 内存环）：
//    · in 行：只打 a1/a2（a2 ＝ `&门面+0x38`，由调用方 `0xa940b: leaq 56(%rbx),%rsi` 提供——
//      已自核 `HWLibs_full.asm` 行 190313/190315：`a940b leaq 56(%rbx),%rsi` ⇒ `a9416 callq 0x8af60`），
//      **不解引用**；
//    · out 行（调原函数之后）：a2 内核区间校验通过才 `v = *(u64*)a2`（＝写点 `0x8b09f`
//      `movq %rax,(%r14)` 写的那个格子；r14＝rsi＝a2 ⇒ 读的就是它），否则记 0xFF 哨兵（不解引用）；
//    · 按补充决定（保守变体）：**只在 out 行之后 flush 一次**（flush＝磁盘 I/O；本取点在
//      深栈链上（0xa9416→0x8af60），B19 病根＝深栈重 I/O ⇒ I/O 次数取最少；"入口行可能丢失"
//      可接受——若原函数内 panic，panic 回溯自带本 wrapper 帧即证明"已进入"）。
//  长度自证：两行均 ≤ 60 字符 ≤ 191（TraceRing 上限）。缓冲 static（不占栈）。
// ─── 266：TTL 门面（F1/F3；`-NRedTtlFacade`，默认关）──────────────────────────────────
//  只填反汇编已证实的槽位布局；除 slot0（val=root）外，obj/val 以各自静态对象指针填充
//  （属【推断】，消费者读点未穷举）。任一未解字段保持 0，不用常量冒充真实状态。
static void nredTtlFacadeSetSlot(const UInt32 slot, const UInt64 obj, const UInt64 val)
{
    UInt8* const e = gTtlFacadeRoot + slot * 24;
    *reinterpret_cast<UInt32*>(e) = slot;
    *reinterpret_cast<UInt64*>(e + 8) = obj;
    *reinterpret_cast<UInt64*>(e + 16) = val;
}

__attribute__((noinline))
static bool nredTtlFacadeBuild(void)
{
    if (gTtlFacadeInit) { return true; }
    __builtin_memset(gTtlFacadeRoot, 0, sizeof(gTtlFacadeRoot));
    __builtin_memset(gTtlFacadeSlot1, 0, sizeof(gTtlFacadeSlot1));
    __builtin_memset(gTtlFacadeSlot3, 0, sizeof(gTtlFacadeSlot3));
    __builtin_memset(gTtlFacadeSlot4, 0, sizeof(gTtlFacadeSlot4));
    const UInt64 root = reinterpret_cast<UInt64>(gTtlFacadeRoot);
    nredTtlFacadeSetSlot(0, 0, root);                                   // 0x8b969：slot0.val = root
    nredTtlFacadeSetSlot(1, reinterpret_cast<UInt64>(gTtlFacadeSlot1),
                         reinterpret_cast<UInt64>(gTtlFacadeSlot1));
    nredTtlFacadeSetSlot(2, reinterpret_cast<UInt64>(gTtlFacadeSlot2),
                         reinterpret_cast<UInt64>(gTtlFacadeSlot2));
    nredTtlFacadeSetSlot(3, reinterpret_cast<UInt64>(gTtlFacadeSlot3),
                         reinterpret_cast<UInt64>(gTtlFacadeSlot3));
    nredTtlFacadeSetSlot(4, reinterpret_cast<UInt64>(gTtlFacadeSlot4),
                         reinterpret_cast<UInt64>(gTtlFacadeSlot4));
    *reinterpret_cast<UInt64*>(gTtlFacadeSlot2) = root;                 // slot2 是 8B 句柄，+0=root
    gTtlFacadeInit = true;
    return true;
}

// 安装判据（全部满足才返回 true；267 修正：苹果原生模块不再被误判）：
//  ① stage0 主判据（真实返回值合成）：gTrimStat[0].seen ∧ r12==1 ∧ bad==0 ∧ failMask==0；
//  ② 我方替换模块补充判据：对 {2,5,6,7,8,9,14}，仅当 gSwCntById>0 时才要求 gSwResById==0；
//     gSwCntById==0（未经过我方分派，如苹果原生 client3）不参与判定；
//  ③ client3 stage1：gSt1Try bit3 置位 且 gSt1Ret0 bit3 未置位。
static bool nredTtlFacadeReady(void)
{
    if (gTrimStat[0].seen == 0) { return false; }
    if (gTrimStat[0].r12 != 1) { return false; }
    if (gTrimStat[0].bad != 0) { return false; }
    if (gTrimStat[0].failMask != 0) { return false; }
    static const UInt8 kIds[7] = {2, 5, 6, 7, 8, 9, 14};
    for (unsigned i = 0; i < 7; ++i) {
        const UInt8 id = kIds[i];
        if (gSwCntById[id] != 0 && gSwResById[id] != 0) { return false; }
    }
    if ((gSt1Try & (1U << 3)) == 0) { return false; }
    if ((gSt1Ret0 & (1U << 3)) != 0) { return false; }
    return true;
}

UInt32 X5000HWLibs::wrapTtlInit(void* const a1, void* const a2)
{
    {
        // in 行（无条件，只打参数值，不解引用）
        static char lineIn[96];
        const int nIn =
            snprintf(lineIn, sizeof(lineIn), "ttl-init: in a1=%llx a2=%llx\n",
                     static_cast<unsigned long long>(reinterpret_cast<UInt64>(a1)),
                     static_cast<unsigned long long>(reinterpret_cast<UInt64>(a2)));
        if (nIn > 0) { nredTraceLine(lineIn, nIn); }
    }
    // 先把参数逐字透传给原件（stage0/stage1 在原件内部运行）。
    // 271：cast 结果判空（org 为 0 时绝不间接调用；ttl_initialize 的 0＝成功 ⇒ fail-closed 返非 0）。
    const auto orgTtlInit = FunctionCast(wrapTtlInit, singleton().orgTtlInit);
    if (orgTtlInit == nullptr) { return 0xE00002BCU; }
    const UInt32 ret = orgTtlInit(a1, a2);
    // 267：门面评估必须在原件返回之后（入口时 gTrimStat[0] 尚未产生 ⇒ 必然 inst=2）。
    //  判据全满足 ⇒ 用门面覆盖 `*a2` 并返回 0（覆盖原件失败）；否则原样返回 ret。
    if (gTtlFacade) {
        if (nredTtlFacadeBuild() && nredTtlFacadeReady() && nredKernelPtrOk(a2)) {
            *reinterpret_cast<UInt64*>(a2) = reinterpret_cast<UInt64>(gTtlFacadeRoot);
            gTtlFacadeInst = 1;
            return 0;
        }
        gTtlFacadeInst = 2;
    }
    {
        // out 行：`v = *(u64*)a2`（a2 非合法内核地址 ⇒ 0xFF 哨兵，不解引用）
        static char lineOut[96];
        const UInt64 a2u = reinterpret_cast<UInt64>(a2);
        const UInt64 v   = (a2u >= 0xffffff7f80000000ULL) ? *reinterpret_cast<const UInt64*>(a2u)
                                                          : 0xFFULL;
        const int nOut =
            snprintf(lineOut, sizeof(lineOut), "ttl-init: out v=%llx\n",
                     static_cast<unsigned long long>(v));
        if (nOut > 0) {
            nredTraceLine(lineOut, nOut);
            // R83／规矩 28：早期路径**不落盘**（原刷写调用已删）⇒ 只写内存环。
        }
    }
    return ret;
}
// 第八步观测（2026-09-28 第二十一轮）：接口表注册 `0x8b9a5(table, idx, obj, val)`（只读，仅记录索引与值）。
void X5000HWLibs::wrapTtlRegIface(void* const table, const UInt32 idx, void* const obj, void* const val)
{
    if (checkKernelArgument("-NRedAccelLog")) {
        NRED_TRACE("ttl-reg: table=%llx idx=%u obj=%llx val=%llx", static_cast<unsigned long long>(reinterpret_cast<UInt64>(table)),
               static_cast<unsigned>(idx), static_cast<unsigned long long>(reinterpret_cast<UInt64>(obj)),
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(val)));
    }
    if (singleton().orgTtlRegIface == 0) { return; }
    FunctionCast(wrapTtlRegIface, singleton().orgTtlRegIface)(table, idx, obj, val);
    // B94 介入补装（最终用户已授权；回滚＝删本块）：给"收集器表 idx=1 槽对象"补 `+0xE0` 函数指针。
    //  第一因：Apple 0x93880（ttl_initialize step2 内）读并调用 idx=1 槽对象 +0xE0，读到空 ⇒ 返 0 ⇒
    //  m_hTtl 恒 0。穷尽普查确认无合法安装路径 ⇒ 仅能由我方补装（回调值 0xa76e0，规格定稿 §3.2）。
    //  三重前置（缺一不写、绝不覆盖 Apple 已装值、绝不崩）：nredKernelPtrOk(obj) ∧ kcSlide!=0 ∧
    //  *slotE0==nullptr（短路：obj 非法 ⇒ 不解引用 slotE0）。
    //  ⚠️ 门控纪律：本写入分支在 checkKernelArgument 门控**之外**（旧补丁 pool: iface3 整块在门控内
    //  ＝"观测改变行为"违纪前例；写入是无条件执行的介入，打印才有门控约束——本块不打印）。
    if (idx == 1) {
        const UInt8* const fn = reinterpret_cast<const UInt8*>(0xa76e0 + singleton().kcSlide);
        void** const slotE0 = reinterpret_cast<void**>(static_cast<UInt8*>(obj) + 0xE0);
        if (nredKernelPtrOk(obj) && singleton().kcSlide != 0 && *slotE0 == nullptr) {
            *slotE0 = const_cast<UInt8*>(fn);
        }
    }
}

// 第八步观测（2026-09-28 第二十三轮）：`TlsCreateInstance`(0x93f76) 与 `TlsSwInit` 内部创建(0x95e9f)（只读）。
bool X5000HWLibs::wrapTlsCreate(void* const obj, void* const slots)
{
    if (singleton().orgTlsCreate == 0) { return false; }
    const bool ret = FunctionCast(wrapTlsCreate, singleton().orgTlsCreate)(obj, slots);
    if (checkKernelArgument("-NRedAccelLog")) {
        UInt32 s0 = 0, s1 = 0, s2 = 0;
        if (nredKernelPtrOk(slots)) {
            s0 = reinterpret_cast<UInt32*>(slots)[0];
            s1 = reinterpret_cast<UInt32*>(slots)[1];
            s2 = reinterpret_cast<UInt32*>(slots)[2];
        }
        NRED_TRACE("tls-create: obj=%llx slots=%llx s0=%u s1=%u s2=%u ret=%d",
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(obj)),
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(slots)), static_cast<unsigned>(s0),
               static_cast<unsigned>(s1), static_cast<unsigned>(s2), ret ? 1 : 0);
    }
    // H1 观测集 N-3（2026-10-18）：`dev+0xE8` 采纳证据 ＋ 序表字段（**在原函数返回之后**读
    //  ⇒ 取到的就是 `0x95e9f`→`0xa25de`→`a261b` 写入**之后**的值）。
    //  为什么挂在这里（而不是旧 `c3dump3` 挂的 `0xA285F`）：本 wrapper 的 `obj` 与 `0xa3138`
    //  的取址链**同源**（`93f98 movq (%r15),%rbx`）；旧探针挂错对象 ⇒ 第 2 跳 `B=0` ⇒ 读数全是
    //  守卫默认值（`B=0` 伪读数已在 R13 误导过一次）⇒ 本卡同时退役 `c3dump3`。
    //  读链（逐跳 `nredKernelPtrOk` 守卫，任一跳不过 ⇒ 后续字段保持 0）：
    //    obj → o1=*(obj) → B=*(o1) → dev=*(B+0x58) → Ee8=*(dev+0xE8)
    //    r28/r30 = *(Ee8+0x28/+0x30)（我方序表地址）; s2c/s3c = *(dev+0xF0+2*16/+3*16)（槽 2/3 的子项个数）
    //  ⚠ 守卫式**只读**解引用，与既有 `tls-swinit`／旧 `c3dump` 同类；门控关 ⇒ 零解引用。
    if (gTtlBisect) {
        UInt64 ee8 = 0, r28 = 0, r30 = 0;
        UInt32 s2c = 0, s3c = 0;
        const void* const o1 = (nredKernelPtrOk(obj)) ? *reinterpret_cast<void* const*>(obj) : nullptr;
        const void* const B  = (nredKernelPtrOk(o1)) ? *reinterpret_cast<void* const*>(o1) : nullptr;
        void* dev = nullptr;
        if (nredKernelPtrOk(B)) {
            dev = reinterpret_cast<void*>(*reinterpret_cast<const UInt64*>(static_cast<const UInt8*>(B) + 0x58));
        }
        if (nredKernelPtrOk(dev)) {
            const UInt8* const d = static_cast<const UInt8*>(dev);
            ee8 = *reinterpret_cast<const UInt64*>(d + 0xE8);
            s2c = *reinterpret_cast<const UInt32*>(d + 0xF0 + 2 * 16);
            s3c = *reinterpret_cast<const UInt32*>(d + 0xF0 + 3 * 16);
        }
        if (nredKernelPtrOk(reinterpret_cast<void*>(ee8))) {
            const UInt8* const e = static_cast<const UInt8*>(reinterpret_cast<void*>(ee8));
            r28 = *reinterpret_cast<const UInt64*>(e + 0x28);
            r30 = *reinterpret_cast<const UInt64*>(e + 0x30);
        }
        gBisEe8 = ee8; gBisR28 = r28; gBisR30 = r30; gBisS2c = s2c; gBisS3c = s3c;
    }
    return ret;
}
// B95v2（2026-10-18）：`0xa2691`（**第二次三键查表**）**介入封装** —— 换落点后的新落点。
//
// 【为什么换】R12 真机轮证明旧落点 `0xa3138` **不在失败路径上**：`0x93f76` 内
//  `93f93 callq 0x95e9f` → `93f9b testq %rax,%rax` → **`93f9e je 0x94003`**（失败出口）
//  ⇒ `93fb4 callq 0xa3138` 只有 `0x95e9f` **成功后**才到达；而 `0x95e9f` 失败的根因
//  正是"三键查表未命中" ⇒ 旧落点位于根因**下游两跳**，必然空跑（R12 `[nredInsert]` 0 处）。
//  ⇒ 新落点 `0xa2691` 就是那个"未命中返 0"的查表本身，即 **`dev+0xE8 = 0` 的产生处**。
//
// 【真实签名（本卡亲手复核）】`void* a2691(u64 k1, u64 k2, u64 k3)`
//   依据：`a26b8 cmpq %rdi,16(%r8,%rax)`（第 1 参＝键1，64 位比较）／
//         `a26c4 cmpq %rsi,24(%r8,%rax)`（第 2 参＝键2）／`a26d3 cmpq %rdx,32(%r8,%rax)`（第 3 参＝键3）；
//         返回：`a26f6 addq 4008203(%rip),%rax`（命中＝表基址 + 记录偏移）或 `a26f2 xorl %eax,%eax`（未命中＝0）。
//   CALL xrefs：**全库唯一调用者 `a260e`（∈ `0xa25de`）**；`0xa25de` 的唯一调用者 `96553`（∈ `0x95e9f`）。
//
// 【装配由 Apple 自动完成】`0xa25de`：`a260e callq 0xa2691` → 非 0 ⇒ **`a261b movq %r14,232(%rbx)`**
//  （写 `dev+0xE8`）→ `a2677 callq 0xa26ff`（把序表展开到 `dev+0xF0`）⇒ 我方**只返回记录指针**即可。
//
// 【介入语义＝最小面】**仅在原函数返 0（未命中）且传入键匹配我方设备**时返回我方记录指针：
//   · 原函数已命中（返回非 0）⇒ **逐字放行**（既有纪律：绝不覆盖 Apple 已装值）；
//   · 键不匹配 ⇒ 不接管（本机性判据：k1==0x15BF ∧ k2==9，均为真机实测值）。
//   ⚠ 键3 `k3` **不作**接管条件：我方记录在 `+0x20` 取**通配** `0xDEADCAFE`（M-2），
//     而 Apple 查表的键3 比较是"相等 或 通配" ⇒ 任意 k3 都能命中我方记录（这正是通配的意义）。
//
// 【不改 ABI】只改返回值，**不改**参数个数/宽度/返回类型/调用约定。
void* X5000HWLibs::wrapDevLookup(const UInt64 k1, const UInt64 k2, const UInt64 k3)
{
    if (singleton().orgDevLookup == 0) { return nullptr; }
    void* const ret = FunctionCast(wrapDevLookup, singleton().orgDevLookup)(k1, k2, k3);
    if (!gDevTablePatch) { return ret; }
    // O-2：查表读数（**只写标量**，不入环、不打印 ⇒ 由头部戳记汇总成一行）。
    gInsCalls += 1;
    gInsK1 = static_cast<UInt32>(k1);
    gInsK2 = static_cast<UInt32>(k2);
    gInsK3 = static_cast<UInt32>(k3);
    // ① Apple 自己命中 ⇒ **逐字放行**（零扰动）
    if (ret != nullptr) {
        gInsAppleHit += 1;
        return ret;
    }
    // ② 键不匹配我方设备 ⇒ 不接管（只认本机：k1/k2 均为真机实测值）
    if (k1 != 0x15BF || k2 != 9) {
        if (gInsSkip == kInsSkipNone) { gInsSkip = kInsSkipDev; }
        return ret;
    }
    // ③ 未命中 ＋ 键匹配 ⇒ 接管：返回我方记录（0x50）。
    //    后续"写 `dev+0xE8` + 展开序表"全部由 Apple 自己的 `0xa25de` 完成，我方不代劳。
    gInsOurs += 1;
    return const_cast<NRedDevRecord*>(&kNRedDevRecord);
}

void* X5000HWLibs::wrapTlsSwInit(void* const obj)
{
    if (singleton().orgTlsSwInit == 0) { return nullptr; }
    void* const ret = FunctionCast(wrapTlsSwInit, singleton().orgTlsSwInit)(obj);
    if (checkKernelArgument("-NRedAccelLog")) {
        UInt64 f0 = 0, f8 = 0;
        if (nredKernelPtrOk(obj)) {
            f0 = reinterpret_cast<UInt64*>(obj)[0];
            f8 = reinterpret_cast<UInt64*>(obj)[1];
        }
        // provider 全局：`0x910e1` 读 `*(0x2241b60)`（查询函数）与 `*(0x2241b68)`（上下文），任一为 0 即失败。
        UInt64 g60 = 0, g68 = 0;
        if (singleton().kcSlide != 0) {
            g60 = *reinterpret_cast<UInt64*>(singleton().kcSlide + 0x2241b60);
            g68 = *reinterpret_cast<UInt64*>(singleton().kcSlide + 0x2241b68);
        }
        NRED_TRACE("tls-swinit: obj=%llx f0=%llx f8=%llx g60=%llx g68=%llx ret=%llx",
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(obj)),
               static_cast<unsigned long long>(f0), static_cast<unsigned long long>(f8),
               static_cast<unsigned long long>(g60), static_cast<unsigned long long>(g68),
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(ret)));
    }
    return ret;
}


// 第八步观测（2026-09-28 第二十五轮）：`bgm_create` 内层 4 步（只读，仅记录入参与返回值）。
static void nredNoteBgmStep(const char* tag, UInt64 a1, UInt64 a2, UInt64 a3, UInt64 ret)
{
    if (!checkKernelArgument("-NRedAccelLog")) { return; }
    char buf[192];
    const int n = snprintf(buf, sizeof(buf), "%s: a1=%llx a2=%llx a3=%llx ret=%llx\n", tag,
                           static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                           static_cast<unsigned long long>(a3), static_cast<unsigned long long>(ret));
    nredTraceLine(buf, n);
    NRED_TRACE("%s: a1=%llx a2=%llx a3=%llx ret=%llx", tag, static_cast<unsigned long long>(a1),
           static_cast<unsigned long long>(a2), static_cast<unsigned long long>(a3),
           static_cast<unsigned long long>(ret));
}

UInt32 X5000HWLibs::wrapBgmInit(void* const a, void* const b, void* const c, void* const d, void* const e)
{
    if (singleton().orgBgmInit == 0) { return 0; }
    const UInt32 ret = FunctionCast(wrapBgmInit, singleton().orgBgmInit)(a, b, c, d, e);
    nredNoteBgmStep("bgm-init", reinterpret_cast<UInt64>(a), reinterpret_cast<UInt64>(b),
                    reinterpret_cast<UInt64>(c), ret);
    return ret;
}
// 236：step1 观测独立成 noinline 帧（降低 wrapBgmStep1 帧；通道已为 fwTrace）。
__attribute__((noinline))
static void nredBgmStep1Log(const void* const a, const void* const b, const void* const c, const void* const d,
                            const UInt32 ret)
{
    if (!checkKernelArgument("-NRedAccelLog")) { return; }
    // `d` = 输出结构（`0x2aacbb` 的判据 ② 读它的 +4，要求为 1；判据 ① 读 +0）
    UInt64 o0 = 0, o8 = 0;
    if (nredKernelPtrOk(d)) {
        o0 = reinterpret_cast<const UInt64*>(d)[0];
        o8 = reinterpret_cast<const UInt64*>(d)[1];
    }
    fw::fwTrace("bgm-step1: a1=%llx a2=%llx a3=%llx out=%llx o0=%llx o8=%llx ret=%u",
                static_cast<unsigned long long>(reinterpret_cast<UInt64>(a)),
                static_cast<unsigned long long>(reinterpret_cast<UInt64>(b)),
                static_cast<unsigned long long>(reinterpret_cast<UInt64>(c)),
                static_cast<unsigned long long>(reinterpret_cast<UInt64>(d)),
                static_cast<unsigned long long>(o0), static_cast<unsigned long long>(o8),
                static_cast<unsigned>(ret));
}


UInt32 X5000HWLibs::wrapBgmStep1(void* const a, void* const b, void* const c, void* const d)
{
    if (singleton().orgBgmStep1 == 0) { return 0; }
    const UInt32 ret = FunctionCast(wrapBgmStep1, singleton().orgBgmStep1)(a, b, c, d);
    nredBgmStep1Log(a, b, c, d, ret);
    return ret;
}

UInt32 X5000HWLibs::wrapBgmQuery(void* const a, void* const b, void* const c)
{
    // ⚠️ 已静音（2026-09-28）：本函数是热路径（单轮 ~2000 次调用），输出会挤满 L2 环形缓冲、
    //  使关键观测行（`mode2-tail`/`ip`）落不了盘。信息已收集完毕；仍保留 hook（行为不变）。
    if (singleton().orgBgmQuery == 0) { return 0; }
    return FunctionCast(wrapBgmQuery, singleton().orgBgmQuery)(a, b, c);
}

UInt32 X5000HWLibs::wrapBgmStep4(void* const a)
{
    if (singleton().orgBgmStep4 == 0) { return 0; }
    const UInt32 ret = FunctionCast(wrapBgmStep4, singleton().orgBgmStep4)(a);
    if (checkKernelArgument("-NRedAccelLog")) {
        // `0x2aa5bc` 的判据：读 `*(UInt32*)(obj+0x14)` 作为"模式"，只接受 1/2/3，否则返回 1（失败）。
        UInt32 mode = 0;
        UInt64 inner = 0, f2fa8 = 0, f2fb8 = 0;
        if (nredKernelPtrOk(a)) {
            mode = *reinterpret_cast<UInt32*>(reinterpret_cast<UInt8*>(a) + 0x14);
            inner = *reinterpret_cast<UInt64*>(a);
            f2fa8 = *reinterpret_cast<UInt64*>(reinterpret_cast<UInt8*>(a) + 0x2fa8);
            f2fb8 = *reinterpret_cast<UInt64*>(reinterpret_cast<UInt8*>(a) + 0x2fb8);
        }
        fw::fwTrace("bgm-step4: obj=%llx inner=%llx mode=%u f2fa8=%llx f2fb8=%llx ret=%u",
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(a)), static_cast<unsigned long long>(inner),
               static_cast<unsigned>(mode), static_cast<unsigned long long>(f2fa8),
               static_cast<unsigned long long>(f2fb8), static_cast<unsigned>(ret));
    }
    return ret;
}

// ─── 235：BGM 64K 捕获的通用小块转储（只读，≤64 B）────────────────────────────────────
//  pt：1＝provider B 初读缓冲；2＝memcpy 后 dst；3＝SMU 循环体 rdx 目标（16 B）；4＝校验入口 rsi。
//  安全：先做内核指针校验再解引用；len 上限 64；行缓冲 128B；不新增等待。
// 236：hex 分块独立成 noinline 帧（避免其 snprintf/参数溢出把 nredBgm64Dump 帧推高）。
__attribute__((noinline))
static void nredBgm64Hex(const UInt32 pt, const UInt8* const b, const UInt32 cap)
{
    static char line[128];
    for (UInt32 r = 0; r < 2; ++r) {
        const UInt32 off = r * 32;
        if (off >= cap) { break; }
        int k = snprintf(line, sizeof(line), "[bgm64-b] pt=%u off=0x%02X ", static_cast<unsigned>(pt),
                         static_cast<unsigned>(off));
        for (UInt32 i = 0; i < 32 && off + i < cap && k > 0 &&
                          static_cast<size_t>(k) + 2 < sizeof(line); ++i) {
            k += snprintf(line + k, sizeof(line) - static_cast<size_t>(k), "%02X",
                          static_cast<unsigned>(b[off + i]));
        }
        if (k > 0) { fw::fwTrace("%s", line); }
    }
}

static void nredBgm64Dump(const UInt32 pt, const void* const p, const UInt32 len)
{
    const UInt32 cap = (len < 64) ? len : 64;
    const UInt32 n = ++gBgm64N[(pt < 5) ? pt : 0];
    const bool ok = nredKernelPtrOk(p) && cap != 0;
    UInt32 w[4] = {0, 0, 0, 0};
    if (ok) {
        const UInt8* const b = static_cast<const UInt8*>(p);
        for (UInt32 i = 0; i < 4; ++i) {
            if ((i + 1) * 4 <= cap) { w[i] = *reinterpret_cast<const UInt32*>(b + i * 4); }
        }
    }
    fw::fwTrace("[bgm64] pt=%u n=%u buf=%llx len=%u hdr=%08X b0=%08X b4=%08X b8=%08X bc=%08X bad=%u",
               static_cast<unsigned>(pt), static_cast<unsigned>(n),
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(p)), static_cast<unsigned>(len),
               static_cast<unsigned>(w[0]), static_cast<unsigned>(w[1]),
               static_cast<unsigned>(w[2]), static_cast<unsigned>(w[3]), ok ? 0u : 1u);
    if (!ok) { return; }
    nredBgm64Hex(pt, static_cast<const UInt8*>(p), cap);
}

// 236：`[bgmcmd]` 归因与 `[bgmtab-b]` hex 各自独立成 noinline 帧（降低 wrapBgmTabRead 帧）。
__attribute__((noinline))
static void nredBgmCmdLine(const void* const self, void* const* const out, const UInt32 ret,
                           const unsigned selU, const unsigned sizeU)
{
    void* provB = nullptr;
    void* target = nullptr;
    void* method = nullptr;
    unsigned bad = 0;
    if (nredKernelPtrOk(self)) {
        provB = *reinterpret_cast<void* const*>(static_cast<const UInt8*>(self) + 0x148);
        if (nredKernelPtrOk(provB)) {
            target = *reinterpret_cast<void**>(static_cast<UInt8*>(provB) + 8);
            method = *reinterpret_cast<void**>(static_cast<UInt8*>(provB) + 0x10);
        } else {
            bad = 1;
        }
    } else {
        bad = 1;
    }
    void* buf = nullptr;
    // `buf` 仅在成功时有效（成功时 *out = resp[0]+16）；失败时保持 0。
    if (ret == 0 && nredKernelPtrOk(out) && nredKernelPtrOk(*out)) { buf = *out; }
    fw::fwTrace("[bgmcmd] sel=%u size=%u buf=%llx provB=%llx target=%llx method=%llx ret=%d bad=%u",
                selU, sizeU, static_cast<unsigned long long>(reinterpret_cast<UInt64>(buf)),
                static_cast<unsigned long long>(reinterpret_cast<UInt64>(provB)),
                static_cast<unsigned long long>(reinterpret_cast<UInt64>(target)),
                static_cast<unsigned long long>(reinterpret_cast<UInt64>(method)),
                static_cast<int>(ret), bad);
}

__attribute__((noinline))
static void nredBgmTabHex(const UInt8* const p, const UInt32 len, const unsigned selU)
{
    static char line[128];
    for (UInt32 r = 0; r < 8; ++r) {
        const UInt32 off = r * 32;
        if (off >= len) { break; }
        int n = snprintf(line, sizeof(line), "[bgmtab-b] sel=%u off=0x%03X ", selU,
                         static_cast<unsigned>(off));
        for (UInt32 k = 0; k < 32 && off + k < len && n > 0 &&
                          static_cast<size_t>(n) + 2 < sizeof(line); ++k) {
            n += snprintf(line + n, sizeof(line) - static_cast<size_t>(n), "%02X",
                          static_cast<unsigned>(p[off + k]));
        }
        if (n > 0) { fw::fwTrace("%s", line); }
    }
}

// 236：表头解析 + 首行输出独立成帧（降低 wrapBgmTabRead 帧到 ≤128 B）。
__attribute__((noinline))
static void nredBgmTabReport(const UInt8* const p, const UInt32 len, const unsigned selU,
                             const char* const pre, const UInt32 ret)
{
    auto rd32 = [p, len](const UInt32 off) -> UInt32 {
        if (static_cast<UInt64>(off) + 4 > len) { return 0; }
        return *reinterpret_cast<const UInt32*>(p + off);
    };
    const UInt32 hdr = rd32(0x00);
    const UInt32 ver = (len >= 6) ? static_cast<UInt32>(*reinterpret_cast<const UInt16*>(p + 0x04)) : 0;
    // 238 修正：ipds/harv 偏移字段是 **u16**（0x2ab00e 用 movzwl 12/28 读取；此前按 u32 读是错的）。
    const UInt32 ipds = rd32(static_cast<UInt32>(*reinterpret_cast<const UInt16*>(p + 0x0C)));
    const UInt32 harv = rd32(static_cast<UInt32>(*reinterpret_cast<const UInt16*>(p + 0x1C)));
    UInt32 sum = 0;
    for (UInt32 i = 0; i < len; ++i) { sum += p[i]; }
    fw::fwTrace("[bgmtab] %s hdr=%08X ver=%u ipds=%08X harv=%08X sum=%08X len=%u ret=%d bad=0", pre,
                static_cast<unsigned>(hdr), static_cast<unsigned>(ver), static_cast<unsigned>(ipds),
                static_cast<unsigned>(harv), static_cast<unsigned>(sum), static_cast<unsigned>(len),
                static_cast<int>(ret));
    nredBgmTabHex(p, len, selU);
}

// 238：真实容器自检与接管日志（noinline，避免把 wrapBgmTabRead 帧推过 128 B）。
__attribute__((noinline))
static UInt64 nredBgmRealFnv1a64(const UInt8* const p, const UInt32 n)
{
    UInt64 h = 0xCBF29CE484222325ULL;
    for (UInt32 i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

// 输出 `[bgm] src=real … selfcheck=ok`；字段按实施文档 §4（sig/ver/ipds/harv/sum）。
// sum＝前 65536 字节的 32 位字节和（文档允许“32 位简单和”）。
__attribute__((noinline))
static void nredBgmRealLog(const UInt8* const p)
{
    auto rd32 = [p](const UInt32 off) -> UInt32 {
        return *reinterpret_cast<const UInt32*>(p + off);
    };
    // 注意：ipds/harv 偏移是 **u16**（校验器 0x2ab00e 用 movzwl 12/28 读取）。
    const UInt32 ipdsOff = static_cast<UInt32>(*reinterpret_cast<const UInt16*>(p + 0x0C));
    const UInt32 harvOff = static_cast<UInt32>(*reinterpret_cast<const UInt16*>(p + 0x1C));
    const UInt32 sig = rd32(0x00);
    const UInt32 ver = static_cast<UInt32>(*reinterpret_cast<const UInt16*>(p + 0x04));
    const UInt32 ipds = rd32(ipdsOff);
    const UInt32 harv = rd32(harvOff);
    UInt32 sum = 0;
    for (UInt32 i = 0; i < 65536u; ++i) { sum += p[i]; }
    fw::fwTrace("[bgm] src=real len=65536 sig=%08X ver=%u ipds=%08X harv=%08X sum=%08X selfcheck=ok",
                static_cast<unsigned>(sig), static_cast<unsigned>(ver), static_cast<unsigned>(ipds),
                static_cast<unsigned>(harv), static_cast<unsigned>(sum));
}

// ─── 230：BGM 表区转储（`-NRedBgmTab`，默认关）───────────────────────────────────
//  挂点 0x29b4e2（服务读方法，无符号，pattern 路由）。原型（反汇编 29b4e2–29b56d）：
//    `UInt32 f(void* self, UInt32 sel, UInt64 size, void** out)`；
//    内部组 req{+0=(sel≠0), +4=(sel==4), +8=3, +0x10=size+16} → `call *16(container)`；
//    成功（ret==0）时把 `resp[0]`（缓冲指针）与 `resp[1]` 写入缓冲头，`*out = resp[0]+16`。
//  ⚠ 与实施文档的偏差（已核实）：文档写“从 resp 参数取 resp[0]，数据起点 resp[0]+16”；
//    实际签名没有 resp 参数，第四参是 `out`，且 **`*out` 已是 resp[0]+16** ⇒ 直接从 `*out` 读。
//  安全：`out` 与 `*out` 两级内核指针校验；长度 = min(size, 0x10000)；所有头字段读取带边界检查；
//    不建 64KB 栈缓冲；前 256B 按 32B/行分 8 行输出，单行 ≤ 128B 缓冲。
UInt32 X5000HWLibs::wrapBgmTabRead(void* const self, const UInt32 sel, const UInt64 size, void** const out)
{
    // ── 238：真实 Phoenix 容器接管（先于原件）──────────────────────────────────────────────
    //  匹配条件与 0x2aad49 的 64KB 读完全一致（sel=0/size=0x10000）；其它请求逐字透传。
    //  自检：首次调用时对数组算 FNV-1a64，与编译期常量比对（不符则不接管，回退原件）。
    if (gBgmReal && sel == 0 && size == 0x10000ULL && out != nullptr && nredKernelPtrOk(out)) {
        if (gBgmRealCheck < 0) {
            gBgmRealCheck = (nredBgmRealFnv1a64(gBgmRealContainer, 65536u) == kBgmRealFnv1a64) ? 1 : 0;
        }
        if (gBgmRealCheck == 1) {
            *out = gBgmRealContainer;
            nredBgmRealLog(gBgmRealContainer);
            return 0;   // 与原件语义一致：0=成功
        }
        fw::fwTrace("[bgm] src=real len=65536 selfcheck=fail");
        // 自检失败 ⇒ 不接管，继续走原件（保持关态/异常态行为可预测）。
    }
    if (singleton().orgBgmTabRead == 0) { return 0; }
    const UInt32 ret = FunctionCast(wrapBgmTabRead, singleton().orgBgmTabRead)(self, sel, size, out);
    if (!gBgmTab) { return ret; }   // 理论不可达（门控关不安装）；双保险
    // 233：调用点归因。pattern 路由后调用者仍是 `call 0x29b4e2` ⇒ `__builtin_return_address(0)`
    //  落在调用者函数内；`raoff = ra - gHwlibsSlide` 可直接对照 HWLibs_full.asm。
    //  ⚠ slide==0 时只打 ra、不打 raoff（避免给出错误偏移，按实施文档 §3）。
    const UInt64 ra = reinterpret_cast<UInt64>(__builtin_return_address(0));
    const bool hasOff = (gHwlibsSlide != 0 && ra > gHwlibsSlide);
    const UInt64 raOff = hasOff ? (ra - gHwlibsSlide) : 0;
    const unsigned selU  = static_cast<unsigned>(sel);
    const unsigned sizeU = (size > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : static_cast<unsigned>(size);
    static char pre[96];   // 236：static（与 line 同理；BGM 路径单线程诊断）
    if (hasOff) {
        snprintf(pre, sizeof(pre), "sel=%u size=%u ra=%llx raoff=%llx", selU, sizeU,
                 static_cast<unsigned long long>(ra), static_cast<unsigned long long>(raOff));
    } else {
        snprintf(pre, sizeof(pre), "sel=%u size=%u ra=%llx", selU, sizeU,
                 static_cast<unsigned long long>(ra));
    }

    // 234：provider B 归因（独立 noinline 帧，见 nredBgmCmdLine）。
    nredBgmCmdLine(self, out, ret, selU, sizeU);
    if (ret != 0) {
        fw::fwTrace("[bgmtab] %s hdr=0 ver=0 ipds=0 harv=0 sum=0 len=0 ret=%d bad=0", pre,
                   static_cast<int>(ret));
        return ret;
    }
    if (!nredKernelPtrOk(out) || !nredKernelPtrOk(*out)) {
        fw::fwTrace("[bgmtab] %s hdr=0 ver=0 ipds=0 harv=0 sum=0 len=0 ret=0 bad=1", pre);
        return ret;
    }

    const UInt8* const p = static_cast<const UInt8*>(*out);
    const UInt32 len = (size > 0x10000ULL) ? 0x10000U : static_cast<UInt32>(size);
    // 235 P1：provider B 初读缓冲（`*out` 即 resp[0]+16），只对 64KB 读采一次。
    if (gBgm64 && !gBgm64Once1 && len >= 0x10000) {
        gBgm64Once1 = true;
        nredBgm64Dump(1, p, len);
    }
    nredBgmTabReport(p, len, selU, pre, ret);
    return ret;
}

// ─── 235 P2：`0x298ce5` memcpy(dst, src, len)（64KB 拷贝后取样）──────────────────────────
//  反汇编 298ce5：参数 rdi=dst／rsi=src／rdx=len（≤0 直接返回），返回 rax=len 或 0。
//  该函数是通用 memcpy（调用点 30+）⇒ 只在 `len ≥ 0x10000` 且仅第一次取样。
UInt64 X5000HWLibs::wrapBgm64Memcpy(void* const dst, const void* const src, const UInt64 len)
{
    if (singleton().orgBgm64Memcpy == 0) { return 0; }
    const UInt64 ret = FunctionCast(wrapBgm64Memcpy, singleton().orgBgm64Memcpy)(dst, src, len);
    // 长度窗口：64KB ≤ len ≤ 16MB（排除负 size 被当 UInt64 巨值与异常大拷贝）。
    if (gBgm64 && !gBgm64Once2 && len >= 0x10000 && len <= 0x1000000ULL) {
        gBgm64Once2 = true;
        nredBgm64Dump(2, dst, (len > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : static_cast<UInt32>(len));
    }
    return ret;
}

// ─── 241：BGM `EVENT__HW_INIT` 上报点观测（`-NRedBgmHw`，默认关）──────────────────────
//  0x91222 是 0x929ac 的 pre/post/fail 通知（原件仅副作用，无返回值）。只在 client==3 时取样：
//  `sub = *(void**)(root+0x20)`（client3 子对象，先做内核指针校验）。
//  限流：每个 (stage,mode) 首 2 条、总计 ≤8；表与计数在 BSS，wrapper 零局部数组。
void X5000HWLibs::wrapBgmHwNotify(void* const root, const UInt32 client, const UInt32 stage,
                                  const UInt64 arg, const UInt32 mode)
{
    if (gBgmHw && client == 3 && stage < 9 && mode < 5 &&
        gBgmHwCnt[stage][mode] < 2 && gBgmHwTotal < 8) {
        ++gBgmHwCnt[stage][mode];
        ++gBgmHwTotal;
        UInt64 sub = 0;
        if (nredKernelPtrOk(root)) {
            sub = reinterpret_cast<UInt64>(*reinterpret_cast<void* const*>(static_cast<const UInt8*>(root) + 0x20));
            if (!nredKernelPtrOk(reinterpret_cast<const void*>(sub))) { sub = 0; }
        }
        fw::fwTrace("[bgmhw] stage=%u mode=%u arg=%llx sub=%llx", static_cast<unsigned>(stage),
                    static_cast<unsigned>(mode), static_cast<unsigned long long>(arg),
                    static_cast<unsigned long long>(sub));
    }
    if (singleton().orgBgmHwNotify == 0) { return; }
    FunctionCast(wrapBgmHwNotify, singleton().orgBgmHwNotify)(root, client, stage, arg, mode);
}

UInt32 X5000HWLibs::wrapReadSel(void* const obj, const UInt32 sel, const UInt32 a2, const UInt32 a3, void* const buf,
                                const UInt32 a5)
{
    if (singleton().orgReadSel == 0) { return 0; }
    const UInt32 ret = FunctionCast(wrapReadSel, singleton().orgReadSel)(obj, sel, a2, a3, buf, a5);
    // 只记录 BGM 相关的 selector/id（其余为热路径，静默通过，避免刷屏覆盖 L2 环形缓冲）。
    if ((sel == 7 || sel == 8 || a2 == 1 || a2 == 6 || a2 == 0xde5 || a2 == 0x16a00 || a2 == 0x16091) &&
        checkKernelArgument("-NRedAccelLog")) {
        NRED_TRACE("read-sel: obj=%llx sel=%u a2=%u a3=%u buf=%llx a5=%u ret=%u",
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(obj)), static_cast<unsigned>(sel),
               static_cast<unsigned>(a2), static_cast<unsigned>(a3),
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(buf)), static_cast<unsigned>(a5),
               static_cast<unsigned>(ret));
    }
    return ret;
}

UInt32 X5000HWLibs::wrapCfgRead(void* const obj, const UInt32 id, UInt64* const out, UInt32* const flag)
{
    const UInt64 before = (nredKernelPtrOk(out)) ? *out : 0;
    if (singleton().orgCfgRead == 0) { return 0; }
    const UInt32 ret = FunctionCast(wrapCfgRead, singleton().orgCfgRead)(obj, id, out, flag);
    // 235 P3：SMU 写-读循环体（0x2ab398）。原件以 rdx 为目标、rsi 为源（2aaf26/2aaf2e）⇒ 在
    //  wrapper 里 rdx 即第 3 参 `out`。热路径（~1800 次/轮）⇒ 只采前 2 次 + 每 256 次。
    if (gBgm64) {
        const UInt32 n3 = ++gBgm64LoopN;
        if ((n3 <= 2 || (n3 & 0xFF) == 0) && nredKernelPtrOk(out)) { nredBgm64Dump(3, out, 16); }
    }
    // ⚠️ 已静音（2026-09-28）：本函数是热路径（单轮 ~1800 次调用），其输出会把 L2 环形缓冲挤满，
    //  导致真正关键的观测行（`mode2-tail`/`ip`）落不了盘。信息已收集完毕，需要时再临时打开。
    (void)before;
    return ret;
}

UInt32 X5000HWLibs::wrapMode2Tail(void* const a, void* const b, void* const c, void* const d, void* const e)
{
    // 235 P4：校验入口缓冲（0x2ab00e 的第 2 参 rsi，即 `-56(%rbp)` 的初读缓冲）——**调用前**采样。
    if (gBgm64 && !gBgm64Once4 && nredKernelPtrOk(b)) {
        gBgm64Once4 = true;
        nredBgm64Dump(4, b, 0x10000);
    }
    if (singleton().orgMode2Tail == 0) { return 0; }
    const UInt32 ret = FunctionCast(wrapMode2Tail, singleton().orgMode2Tail)(a, b, c, d, e);
    // 238：验证器返回值（与“表被接受”分开记录；仅真实容器开关时输出，走 trace-only 通道）。
    if (gBgmReal) { fw::fwTrace("[bgm] verifier_ret=%d", static_cast<int>(ret)); }
    // 只读观测：`0x2ab00e` 的第 2 形参是一个 64 KiB 缓冲，函数内部按其构造的 **IP 发现表**做校验
    // （`"IPDS"`/`"HARV"` 魔数 + 三段校验和 + 每块 `num_base_address ∈ {1..6}`）。
    // ⚠️ **每次调用只输出这一行、且放在调用之后**：① 多行会被 L2 环形缓冲吞掉（第 8/9 轮实测）；
    //  ② 与 `ret` 合并输出才能保证"哪张表 ⇒ 哪个返回值"不错配。
    // 不调用任何 Apple 方法、不改内存。
    // ⚠️ **只在失败时输出**（2026-09-28 第 12 轮教训）：成功调用的行会把 L2 环形缓冲占满，
    //  导致失败调用的关键行反被挤掉；而成功调用的内容已知固定。
    // ⚠️ 无论成功失败都落盘（2026-09-28 第 16 轮教训）：自检证明落盘通道可用，但"失败才落盘"
    //  一直没产出 ⇒ 与其猜，不如每次都写（每轮仅 3–4 次调用，文件极小）。
    if (checkKernelArgument("-NRedAccelLog") && nredKernelPtrOk(b)) {
        const auto* const p = static_cast<const UInt8*>(b);
        const UInt32 o12 = *reinterpret_cast<const UInt16*>(p + 0xC);   // IPDS 子表偏移（相对缓冲起点）
        const UInt32 o1C = *reinterpret_cast<const UInt16*>(p + 0x1C);  // HARV 子表偏移
        const auto u16 = [p](unsigned o) { return static_cast<UInt32>(*reinterpret_cast<const UInt16*>(p + o)); };
        // 三段校验和：逐条复刻 `0x2ab00e` 的算法（16 位字节累加和）
        UInt32 cksA = 0;
        const UInt32 cntA = u16(0xA);
        if (cntA != 10) {
            cksA = static_cast<UInt8>(cntA);
            const UInt32 n = static_cast<UInt16>(cntA - 10);
            if (n != 1 && n >= 1 && 0xB + n - 1 <= 0x10000) {
                for (UInt32 i = 0; i < n - 1; i++) { cksA = static_cast<UInt16>(cksA + p[0xB + i]); }
            }
        }
        UInt32 cksB = 0;
        if (o12 + 8 <= 0x10000) {
            const UInt32 len = u16(o12 + 6);
            if (o12 + len <= 0x10000) {
                for (UInt32 i = 0; i < len; i++) { cksB = static_cast<UInt16>(cksB + p[o12 + i]); }
            }
        }
        UInt32 cksC = 0;
        if (o1C != 0 && o1C + 4 <= 0x10000) {
            const UInt32 len = u16(0x20);
            if (o1C + len <= 0x10000) {
                for (UInt32 i = 0; i < len; i++) { cksC = static_cast<UInt16>(cksC + p[o1C + i]); }
            }
        }
        // die 记录（偏移是**相对缓冲起点**，不是相对 IPDS 子表）+ 越界块自检
        UInt32 die = 0xFFFFFFFF, cnt = 0, bad = 0, b0 = 0, n0 = 0, b1 = 0, n1 = 0;
        const UInt32 dieOff = u16(o12 + 0x10);
        if (dieOff + 8 <= 0x10000) {
            die = (u16(dieOff) << 16) | u16(dieOff + 2);
            cnt = u16(dieOff + 2);
            UInt32 pos = dieOff + 4;
            for (UInt32 i = 0; i < cnt && pos + 8 <= 0x10000; i++) {
                const UInt32 hw = *reinterpret_cast<const UInt16*>(p + pos);
                const UInt32 nba = p[pos + 3];
                if (nba == 0 || nba > 9) {          // Apple 判据是 nba ∈ {1..6}；补丁已放宽到 ≤9
                    if (bad == 0) { b0 = hw; n0 = nba; }
                    else if (bad == 1) { b1 = hw; n1 = nba; }
                    bad++;
                }
                pos += 8 + 4 * nba;
            }
        }
        // ★ 深栈安全改造（2026-10-12，B25 Double fault 实证——同 wrapMode9440f）：
        //   B25 分片栈＝`tmrInit→bringupRunToTmrInit→nredFwBringupHook→
        //   wrapSmuInitFunctionPointerList→wrapMode9440f+0x67`；本函数（0x2ab00e，
        //   bgm_create 模式 2 尾段）与 0x9440f 同处 TlsSwInit（FUN_000928f4）子链 ⇒
        //   同样在 FW bringup 深栈上可达。`FileIO::writeBufferToFile`（APFS vnode 链）与
        //   `NvMsgBuf::dumpNow`（FS 写）均为重栈耗通道（B19 根因：lilu_os_log tmp[1024]）
        //   ⇒ 本回调内**不做任何文件系统写、不调 lilu 日志通道**。
        //   唯一通道＝`nredTraceLine`（内存环 gTraceRing.push，零 FS、零 os_log；
        //   由 L2 定时拍/dumpNow/panic 分片在安全时点刷盘）。
        {
            char line[256];
            const int n = snprintf(line, sizeof(line),
                "ip-fail: off=%x die=%x cnt=%u bad=%u b0=%x/%u b1=%x/%u ck=%04x/%04x,%04x/%04x,%04x/%04x "
                "m=%08x,%08x ab=%llx/%llx ret=%u\n",
                static_cast<unsigned>(dieOff), static_cast<unsigned>(die), static_cast<unsigned>(cnt),
                static_cast<unsigned>(bad), static_cast<unsigned>(b0), static_cast<unsigned>(n0),
                static_cast<unsigned>(b1), static_cast<unsigned>(n1),
                static_cast<unsigned>(cksA), static_cast<unsigned>(u16(0x8)), static_cast<unsigned>(cksB),
                static_cast<unsigned>(u16(0xE)), static_cast<unsigned>(cksC), static_cast<unsigned>(u16(0x1E)),
                static_cast<unsigned>(*reinterpret_cast<const UInt32*>(p + o12)),
                static_cast<unsigned>(*reinterpret_cast<const UInt32*>(p + o1C)),
                static_cast<unsigned long long>(reinterpret_cast<UInt64>(a)),
                static_cast<unsigned long long>(reinterpret_cast<UInt64>(b)), static_cast<unsigned>(ret));
            if (n > 0) { nredTraceLine(line, n); }
        }
        // ⚠️ SYSLOG（ip: 行）已移除（2026-10-12）：lilu_os_log 内部 `char tmp[1024]` 在
        //   FW bringup 深栈上会栈溢出（B19 根因）；本回调只写内存环（上方 nredTraceLine）。
        // ─── 判据 B：64KB 表区转储哨兵（`-NRedAccelLog`，默认关）──────────────────────
        //  位置：`wrapMode2Tail` 现有 IP 探针块之后，同一门控下。`b` 是 0x2ab00e 读取的 64KB 缓冲。
        //  ⚠️ 深栈安全改造（2026-10-12）：原实现 IOMalloc(64KB)＋`FileIO::writeBufferToFile`
        //   （/var/log/NRedTblDump-*.bin）＋`NvMsgBuf::dumpNow()` 均为重栈耗通道，在 FW bringup
        //   深栈上会触栈保护页（B19/B25 Double fault 同根因）⇒ 本回调**不再写盘**，只发哨兵行
        //   （内存环）；全量表区转储如需恢复须移到安全时点（如 L2 定时拍）再落盘。
        //  哨兵：无论成败输出 `tbl-dump: buf=%llx sz=65536 err=%d`（err=-2 表深栈安全跳过），
        //   保证"门控真⇒必有 tbl-dump 行"的双向自证。
        {
            const UInt64 aVal = reinterpret_cast<UInt64>(a);
            char tline[128];
            const int tn = snprintf(tline, sizeof(tline),
                                    "tbl-dump: buf=%llx sz=65536 err=-2\n",
                                    static_cast<unsigned long long>(aVal));
            if (tn > 0) { nredTraceLine(tline, tn); }
        }
    }
    return ret;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// 【软件初始化替换】我方实现：**单入口 ＋ 按模块号分派**（依据 R60 实施文档）
//  · 苹果启动时逐个依次初始化各 IP 模块（旧文档称"客户"，同一含义）；每个模块的软件初始化实现
//    存在**它自己的子对象**的 `+8` 槽里：`subobj = *(void**)((UInt8*)obj + 8 + 8*id)`
//    （与 `0x93991 movq 8(%rdi,%rax,8)` 同源；模块 5 ⇒ `+48`）。
//  · 共同入口＝既有包装 `wrapMode9440f`（＝`0x9440f`）的**入口**：入参即带模块号（**1-based**）。
//    ⚠ R60 §8A-1（需求 1.1a）：目标落点 `0x94a5b` 的对象在 `%r14`（`94a5b: movq %r14,%rdi`，由
//    `0x9440f: 9442d movq %rdi,%r14` 传入），**不是入参寄存器**；本仓库挂钩机制（`RouteRequest` ＋
//    `FunctionCast`）只把入参寄存器交给 C 包装函数，且**没有**"函数中途挂钩"机制 ⇒ 那里拿不到对象。
//  · 时序（R60 §1.1）：我方在入口写槽（早于 `org(0x9440f)`）⇒ 必然早于苹果在 handler 内
//    `movq 8(%rbx),%r14` 的读取；该槽唯一写入者＝`0x980e9`（子对象装配，早于 `0x928f4`／`0x929ac`）。
//  · **单入口 ＋ 按模块号分派**：槽内装"每模块一个携带 id 的薄壳"（回调签名 `int(void*,void*)`
//    没有 id 入参）→ `NRedSwInitDispatch(clientId, info, out)` → `switch`。**禁止**一份通用逻辑
//    通吃所有模块（各模块记录区布局不同：模块 5 为 3648 B，模块 6 为 632 B）。
//  · 未实现模块 ⇒ `default:` **记日志 ＋ 如实返回非 0**（**禁止空壳成功**）。
//  · 逐模块启用门控 `-NRedSwInitIds=<csv>`，**默认只启用模块 5**（一轮一个变量）。
//  · 契约不变：只读 `info`（**R129 起 `+3656／+3664` 仅当模块号＝5**——其它模块的 info 远小于 3664 B，
//    越界读数无意义；见 `nredSwLogCall` 内同款说明）／`+4..+3651`；`out` **只写 +8**（绝不写 +0）；入参空返 2、
//    已存在返 3、**全部真实成功 ∧ 放行门控开才返 0**、其余非 0。
//  · 栈与缓冲：簿记体在 BSS；日志缓冲 static（零栈占用）；帧 ≤128 B。
// （上一张卡已交付骨架 `d4932c0`／`47e479b`；本卡按 R60 重构为"单入口 ＋ 按模块号分派"。）
// ═══════════════════════════════════════════════════════════════════════════════════════════

// 安装侧内部状态（门控与戳记标量已前置在文件头区，见 `gGvmSwInstall` 处注释）：
static bool gGvmSwFused = false;   // 一次性熔断：回滚过 ⇒ 不再安装（避免"装→回滚→再装"抖动）

// 返回值错误码（R60 §1.2；沿用模块 5 约定）
enum : int {
    kSwResPass        = 0,   // 全部步骤**真实做成** ∧ 放行开关开（R80：A8 已移出判据 ⇒ 本码**恢复可达**）
    kSwResNullArg     = 2,   // 入参为空（与苹果那版同值）
    kSwResExists      = 3,   // 已存在、无需初始化（与苹果那版同值）
    kSwResAllocFail   = 4,   // 我方簿记/结构建立失败（并熔断回滚）
    kSwResParseFail   = 5,   // 记录区解析失败
    kSwResStepFail    = 6,   // 某软件初始化步骤失败（并熔断回滚）
    kSwResPassGateOff = 7,   // 全部步骤真实做成但放行开关未开（R80：失败位不再恒非 0 ⇒ 可达）
    kSwResUnimpl      = 8,   // 该模块尚未实现（本轮：模块 5 之外）
};

// ── 模块 5（GVM）记录区口径（苹果侧同型扫描＝`0x1a9d0`／`0x1dcc9`／`0x1adf4`／`0x3d0cc`，步长 20）──
static const UInt32 kGvmRecBytes    = 3648;
static const UInt32 kGvmRecStride   = 20;
static const UInt32 kGvmKeyMajor    = 11;
static const UInt32 kGvmKeyMinorMax = 4;
static const UInt32 kGvmGartSizeMB  = 512;

// ── 页表表项位值（**逐位核对**；来源＝`kb/参考源码/Linux-7.2.0/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.h`，
//    行号可能漂移，以实际内容为准）──
enum : UInt64 {
    kPteValid      = 1ULL << 0,   // AMDGPU_PTE_VALID        amdgpu_vm.h:57
    kPteSystem     = 1ULL << 1,   // AMDGPU_PTE_SYSTEM       amdgpu_vm.h:58
    kPteExecutable = 1ULL << 4,   // AMDGPU_PTE_EXECUTABLE   amdgpu_vm.h:65
    kPteReadable   = 1ULL << 5,   // AMDGPU_PTE_READABLE     amdgpu_vm.h:67
    kPteWriteable  = 1ULL << 6,   // AMDGPU_PTE_WRITEABLE    amdgpu_vm.h:68
};
static const UInt64 kPteAddrMask = 0x0000FFFFFFFFF000ULL;   // gmc_v11_0.c:828
// GART 表项 flags＝VALID|SYSTEM|EXECUTABLE|READABLE|WRITEABLE＝**0x73**
//  ⚠ **修正**（R60 §8B-4）：上一版为 `0x63`，**漏 EXECUTABLE**（上游 `gart_pte_flags` 含它：
//    `gmc_v11_0.c:738`＝`AMDGPU_PTE_MTYPE_NV10(0ULL, MTYPE_UC) | AMDGPU_PTE_EXECUTABLE`）⇒ 现补为 0x73。
//  ⚠ **不补 SNOOPED**（`amdgpu_vm.h:59`）——上游 GART 组合不含（R60 §8B-4 明示撤回该建议）。
//  ⚠ **MTYPE 留空**（bits 48-50，`amdgpu_vm.h:114-118`）：`MTYPE_UC` 的数值在任何已归档参考中**均无
//    定义**（只有用法）⇒ 按"不得猜测常量"留空，登记缺口（R60 §8D 新 N7）。
//  ⚠ **差异声明**：我方组合含 VALID／SYSTEM／READABLE／WRITEABLE，上游 `gart_pte_flags` 只有
//    `MTYPE_UC<<48 | EXECUTABLE` ⇒ 本组合属**我方自定用途**（该表本轮无任何寄存器指向它 ⇒ 无硬件
//    影响），待硬件初始化阶段按上游既定序列复核。
static const UInt64 kGartPteFlags =
    kPteValid | kPteSystem | kPteExecutable | kPteReadable | kPteWriteable;   // ＝0x73

// ── 中断登记（A8）常量：**本世代编号只取自本世代定义文件**（R60 §8B-1／§8B-5，红线级）──
//  规范路径（行号可能漂移，以实际内容为准）：
//   · kb/参考源码/Linux-7.2.0/drivers/gpu/drm/amd/include/soc15_ih_clientid.h
//       :93 SOC21_IH_CLIENTID_GFX=0x0a｜:99 VMC=0x12｜:102 DF=0x17｜:109 MAX（＝0x20）
//   · .../include/ivsrcid/vmc/irqsrcs_vmc_1_0.h
//       :30 VMC_1_0__SRCID__VM_FAULT=0｜:35 UTCL2_1_0__SRCID__FAULT=0
//   · .../amdgpu/amdgpu_irq.h :32 AMDGPU_MAX_IRQ_SRC_ID=0x100；amdgpu_irq.c :456＝登记写入行
//   · DF ECC 的 src_id：上游直接传字面量 0（gmc_v11_0.c:817）
//  ⚠ **禁止**使用移植树/别世代编号：`srcs/mac-amdgpu-ref/dext/amdgpu/amdgpu_ih.h:97` 的
//    `CLIENT_VMC=0x07` 与本世代 `0x12` 冲突（SOC15 中 0x07＝RLC）⇒ 移植树编号禁止用于本世代。
static const UInt32 kIhClientGfx = 0x0A;      // soc15_ih_clientid.h:93
static const UInt32 kIhClientVmc = 0x12;      // soc15_ih_clientid.h:99
static const UInt32 kIhClientDf  = 0x17;      // soc15_ih_clientid.h:102
static const UInt32 kIhClientMax = 0x20;      // SOC21_IH_CLIENTID_MAX（soc15_ih_clientid.h:109）
static const UInt32 kIhSrcVmFault    = 0x00;  // irqsrcs_vmc_1_0.h:30
static const UInt32 kIhSrcUtcl2Fault = 0x00;  // irqsrcs_vmc_1_0.h:35
static const UInt32 kIhSrcDfEcc      = 0x00;  // gmc_v11_0.c:817（上游字面量）
static const UInt32 kIhSrcMax        = 0x100; // amdgpu_irq.h:32
// 我方"中断源"引用（上游等价物＝`struct amdgpu_irq_src *`；GMC 对 VMC／GFX 用同一个 src 对象）
enum : UInt32 { kIrqSrcNone = 0, kIrqSrcVmFault = 1, kIrqSrcEcc = 2 };

// 我方中断登记表（等价 `amdgpu_irq.c:456`：`adev->irq.client[cid].sources[sid] = source`）
struct NRedSwIrqEntry { UInt32 client; UInt32 src; UInt32 srcRef; };
static NRedSwIrqEntry gIrqEntries[3];
static UInt32         gIrqEntryN     = 0;
static UInt32         gIrqRegistered = 0;   // 1＝所需登记项**全部真实写入**（可参与"返 0"判定）

// 登记动作（逐条镜像 `amdgpu_irq_add_id`，`amdgpu_irq.c:422-457`）：
//  ① `client < SOC21_IH_CLIENTID_MAX`（对应 `:426` 的 `AMDGPU_IRQ_CLIENTID_MAX` 校验）
//  ② `src < AMDGPU_MAX_IRQ_SRC_ID`（`:429`）③"`source->funcs` 非空"（`:432` ⇒ 我方 `srcRef != 0`）
//  ④ 重复登记拒绝（`:443` 的 `-EINVAL` 对应）；全部通过才写入登记表（`:456`）并返回 true。
static bool nredSwIrqAddId(const UInt32 client, const UInt32 src, const UInt32 srcRef)
{
    if (client >= kIhClientMax || src >= kIhSrcMax || srcRef == kIrqSrcNone) { return false; }
    for (UInt32 i = 0; i < gIrqEntryN; i += 1) {
        if (gIrqEntries[i].client == client && gIrqEntries[i].src == src) { return false; }
    }
    if (gIrqEntryN >= (sizeof(gIrqEntries) / sizeof(gIrqEntries[0]))) { return false; }
    gIrqEntries[gIrqEntryN].client = client;
    gIrqEntries[gIrqEntryN].src    = src;
    gIrqEntries[gIrqEntryN].srcRef = srcRef;
    gIrqEntryN += 1;
    return true;
}


// 我方簿记体（BSS；零堆分配、零栈占用）。字段与上游逐步对应，全表见交付文档"副作用清单"。
struct NRedGvmSwBook {
    // A1 记录区解析（真实数据：`info+4` 起 3648 B，步长 20，键位 +0/+4）
    UInt32 recEntries;
    UInt32 gfxKeyMajor, gfxKeyMinor, gfxKeyOff;      // 命中的本机世代键；0xFFFFFFFF＝未命中
    // A2 GMC 簿记（`gmc_v11_0.c:788-789`／`:795`／`:827-828`）
    UInt64 mcMask, pteAddrMask, vmSizeBytes;
    UInt32 vmBlockBytes, vmLevels, vmBits, vmhubsMask;
    // A3 两 hub 簿记（`:749`／`:751`）
    UInt32 hubBit[2], hubReady[2];
    UInt64 hubVaStart[2], hubVaSize[2];
    // A4 显存几何（`:755-760`）——本轮省略（理由见本体注释），字段如实留 0＋标记
    UInt32 vramWidth, vramType, vramVendor, vramOmitted;
    // A5 BO 管理器簿记＋一次自检分配（`:843`）
    UInt64 boPhys;
    UInt32 boBytes, boRegistered;
    // A6 GART 结构与内存可见表项（`:723-742`／`:847`）
    UInt64 gartPhys;
    UInt32 gartBytes, gartPtes;
    // A7 VM 管理器簿记（`:851-859`）
    UInt32 firstKfdVmid, vmidTotal, vmRegistered;
    // A8 中断登记（`:801-821`）：3 条 (client,src)→源 的登记是否**全部真实写入**（1＝是）
    UInt32 irqCount, irqRegistered;
    // A9 RAS（卡面明确"可省"）——本轮跳过，如实登记（不计入失败）
    UInt32 rasSkipped;
    // 逐步结果位图：失败／省略（**不再有"未完成"位**：A8 已真实实现，见 A8 段注释）
    UInt32 failMask, omittedMask;
};
static NRedGvmSwBook gGvmBook;                             // 我方簿记（BSS）
static IOBufferMemoryDescriptor* gGvmBoDesc   = nullptr;   // A5：自检分配（物理连续）
static IOBufferMemoryDescriptor* gGvmGartDesc = nullptr;   // A6：GART 表（物理连续、页对齐）
// 日志行缓冲：**static**（不占栈）。R60 §8A-4：原"≤96 B"与"必须出现 8 项字段"客观互斥
// （最小取值长度已 ≥98 字符）⇒ 采 static 缓冲＝**零栈占用**，长度对齐既有内存环单行容量
// （`TraceRing::kLineBytes`＝192；同文件既有 `static char line[176]`／`lineBoot[192]` 同理）。
static char gGvmSwLog[192];

// 安装绑定表（R60 §1.2：**定长 16 项静态表**，`{id, subobj, orig, installed}`；**不新增分配**）
//  ⚠ 为什么不用"按 `%rbx` 反查"取 id：回调是 C 函数，**读不到 `%rbx`** ⇒ id 由**薄壳**携带
//    （R60 §8A-7①）；本表用于"幂等判定 ＋ **按模块回滚** ＋ 戳记可核"（R60 §8A-5）。
struct NRedSwBind { UInt32 id; void* subobj; void* orig; bool installed; };
static NRedSwBind gSwInstalled[16];

// 统一入口（**唯一**）：按模块号分派（R60 需求 1.2；签名照实施文档）
static int NRedSwInitDispatch(UInt32 clientId, void* info, void* out);
// 模块 5（GVM）本体：返回值由本函数给出（日志由统一入口记一行）
static int nredGvmSwInit(void* info, void* out, UInt64 o8init);

// 每模块薄壳：把模块号带给统一入口（Apple 只回调 `int(void*,void*)`，**无 id 入参**）
template <UInt32 Id>
static int nredSwInitThunk(void* const info, void* const out) { return NRedSwInitDispatch(Id, info, out); }

// 已确证期望值的模块清单（R60 §1.4）：**只列【反汇编·确凿】的 8 条**（R124 起含模块 9）。
//  ⚠ 模块 **2**（【推断·高】）与 3／11／12／13（未解出/未确定）**不入表**（表中不得出现数值 ⇒ 不猜）；
//     模块 9 的入表依据（R124，【反汇编·确凿】）：`9fd07 leaq …##0x797e8` → `9fd0e movq %rax,-64(%rbp)`；
//     `9fd7a leaq -64(%rbp),%rsi` ＋ `9fd7e movl $9,%edi` ＋ `9fd83 callq 0x93e4c`（装配器按内存序写 `dst[0..3]`）；
//     独立印证：worker `9ff37 movq 8(%r15),%r12` → `a00db callq *%r12`。
//  ⚠ **已知限制（R124 明确不改）**：安装动作**只写 `subobj+8`** ⇒ 模块 9 的 `funcs[1..3]`（`+16/+24/+32`）
//     仍是苹果的 ⇒ 探针只拦下 `funcs[0]` 路径（崩点 I／II 所在），其余槽未接管。
struct NRedSwClient { UInt32 id; int (*fn)(void*, void*); UInt64 expect; };
static const NRedSwClient kSwClients[] = {
    { 2,  nredSwInitThunk<2>,  0x58a04 },   // R169：模块 2（SDMA）D2 替换 `0x58a04`（funcs[0] 层 0=成功）
    { 5,  nredSwInitThunk<5>,  0x191dc },   // R58 §1.3【反汇编·确凿】
    { 6,  nredSwInitThunk<6>,  0x48f9c },
    { 7,  nredSwInitThunk<7>,  0x7ce0  },
    { 8,  nredSwInitThunk<8>,  0x6430f },
    { 9,  nredSwInitThunk<9>,  0x797e8 },   // R124：模块 9（VCN）——`0x797e8`【反汇编·确凿】（见上）
    { 10, nredSwInitThunk<10>, 0x4d00  },
    { 14, nredSwInitThunk<14>, 0x42e94 },
    { 15, nredSwInitThunk<15>, 0x3fc39 },
};
static const NRedSwClient* nredSwClientEntry(const UInt32 id)
{
    for (UInt32 i = 0; i < (sizeof(kSwClients) / sizeof(kSwClients[0])); i += 1) {
        if (kSwClients[i].id == id) { return &kSwClients[i]; }
    }
    return nullptr;   // 期望值未确证/未解出 ⇒ 不安装（不猜）
}
static bool nredSwIsOurFn(const void* const p)
{
    for (UInt32 i = 0; i < (sizeof(kSwClients) / sizeof(kSwClients[0])); i += 1) {
        if (reinterpret_cast<const void*>(kSwClients[i].fn) == p) { return true; }
    }
    return false;
}

// 逐模块启用门控解析（R60 §1.3）：`-NRedSwInitIds=<csv>`（十进制 1..15，逗号分隔，容忍空白）
//  · 默认＝`5`；非法项 ⇒ **忽略该项并记一行日志**，保持已解析的有效项；
//  · 给出键但**全部非法/为空** ⇒ **回落默认 5**（记一行）；不得因非法参数改变其它任何行为。
//  ⚠ 只在 `processKext` 调用一次；判定处只读 `gSwInitIdsMask`（位掩码）。
static void nredSwInitIdsParse(void)
{
    static char line[96];                                   // static：零栈占用
    UInt32 mask = 0;
    bool sawKey = false;
    const char* const ba = PE_boot_args();
    if (ba != nullptr) {
        static const char kKey[] = "-NRedSwInitIds=";
        const char* p = nullptr;
        for (const char* q = ba; *q != '\0'; q += 1) {      // 手写定位（不依赖 strnstr 的符号可见性）
            if (q[0] != '-' || q[1] != 'N') { continue; }
            bool hit = true;
            for (UInt32 k = 0; kKey[k] != '\0'; k += 1) {
                if (q[k] != kKey[k]) { hit = false; break; }
            }
            if (hit) { p = q + (sizeof(kKey) - 1); break; }
        }
        if (p != nullptr) {
            sawKey = true;
            for (UInt32 guard = 0; *p != '\0' && *p != ' ' && *p != '\t' && guard < 128; guard += 1) {
                if (*p == ',') { p += 1; continue; }
                if (*p < '0' || *p > '9') {                 // 非数字项 ⇒ 跳过整项并记一行
                    while (*p != '\0' && *p != ',' && *p != ' ' && *p != '\t') { p += 1; }
                    const int m = snprintf(line, sizeof(line), "swinit-ids: ignore non-numeric entry\n");
                    if (m > 0) { nredTraceLine(line, m); }
                    continue;
                }
                UInt32 v = 0, digits = 0;
                while (*p >= '0' && *p <= '9' && digits < 4) {
                    v = v * 10 + static_cast<UInt32>(*p - '0');
                    digits += 1;
                    p += 1;
                }
                if (v >= 1 && v <= 15 && (mask & (1U << v)) == 0) { mask |= (1U << v); }
                else {                                      // 越界/重复 ⇒ 忽略该项并记一行
                    const int m = snprintf(line, sizeof(line),
                                           "swinit-ids: ignore entry v=%u (need 1..15, no dup)\n",
                                           static_cast<unsigned>(v));
                    if (m > 0) { nredTraceLine(line, m); }
                }
            }
        }
    }
    if (mask != 0) { gSwInitIdsMask = mask; return; }        // 有有效项 ⇒ 采用
    if (sawKey) {                                           // 给了键但全非法/为空 ⇒ 回落默认 + 记一行
        const int m = snprintf(line, sizeof(line), "swinit-ids: invalid/empty -> default=5\n");
        if (m > 0) { nredTraceLine(line, m); }
    }
    gSwInitIdsMask = (1U << 5);                             // 默认：只启用模块 5
}

// 逐次日志（尾段通道）＋ **两条头部戳记的摘要更新**（R60 §1.5／§8A-3）。
//  ⚠ 摘要**先存**（日志行是否落盘不影响戳记）；日志失败**静默丢弃**，绝不影响返回值/控制流/ABI。
//  ⚠ 只做 snprintf＋内存环 push（零 FS／零 SYSLOG／零堆分配）。
static void nredSwLogCall(const UInt32 id, const void* const info, const void* const out,
                          const UInt64 o8init, const int res)
{
    UInt64 i3656 = 0, i3664 = 0, r0 = 0, r1 = 0;
    UInt32 o0 = 0;
    const bool ok = (info != nullptr && out != nullptr);
    // R129（观测口径修正）：`i3656`／`i3664` **只对模块 5 的 `info` 布局有意义**（其 info ≈3680 B）。模块 6／7／8／9
    //  的 info 分别仅 632／648／704／(小) B ⇒ 读 `+3656/+3664` **越界约 3 KB**：**不是崩溃，但读数无意义**，
    //  且极易被误当成"该模块的字段"判读（R128 §4.7）⇒ **仅当 `id == 5` 时读取真值，其它模块保持 0**。
    //  ⚠ 下面 printf 的字段名与宽度**一字未动** ⇒ **模块 5 的输出格式与数值逐字不变**（其它模块显示 `0`）。
    const bool readLegacy = (id == 5);
    if (ok) {
        const UInt8* const I = static_cast<const UInt8*>(info);
        const UInt8* const O = static_cast<const UInt8*>(out);
        if (readLegacy) {
            i3656 = *reinterpret_cast<const UInt64*>(I + 3656);
            i3664 = *reinterpret_cast<const UInt64*>(I + 3664);
        }
        r0    = *reinterpret_cast<const UInt64*>(I + 4);     // 记录区首 8 B
        r1    = *reinterpret_cast<const UInt64*>(I + 12);    // 记录区次 8 B（供 `[gvmsw]` 原格式）
        o0    = *reinterpret_cast<const UInt32*>(O);          // `out+0` 初值（只读）
    }
    // 戳记摘要（两行各一套；口径＝**最近一次**）
    gSwLastCall.info  = reinterpret_cast<UInt64>(info);
    gSwLastCall.i3656 = i3656; gSwLastCall.i3664 = i3664; gSwLastCall.rec0 = r0;
    gSwLastCall.out0  = o0;    gSwLastCall.res   = res;
    gGvmSwLast.i3656 = i3656; gGvmSwLast.i3664 = i3664;
    gGvmSwLast.r0 = r0;       gGvmSwLast.r1 = r1;
    gGvmSwLast.o0 = o0;       gGvmSwLast.o8 = o8init;  gGvmSwLast.ret = res;
    int m = 0;
    if (!ok) {
        m = snprintf(gGvmSwLog, sizeof(gGvmSwLog), "[gvmsw] id=%u n=0 in=null out=null res=%d\n",
                     static_cast<unsigned>(id), res);
    } else {
        m = snprintf(gGvmSwLog, sizeof(gGvmSwLog),
                     "[gvmsw] id=%u n=%u i3656=%llx i3664=%llx r0=%016llx out0=%u res=%d f=%x o=%x\n",
                     static_cast<unsigned>(id),
                     static_cast<unsigned>((id < 16) ? gSwCallN[id] : 0),
                     static_cast<unsigned long long>(i3656),
                     static_cast<unsigned long long>(i3664),
                     static_cast<unsigned long long>(r0),
                     static_cast<unsigned>(o0), res,
                     static_cast<unsigned>(gGvmBook.failMask),
                     static_cast<unsigned>(gGvmBook.omittedMask));
    }
    if (m > 0) { nredTraceLine(gGvmSwLog, m); }   // m<=0（缓冲不足等）⇒ 静默丢弃，绝不影响调用方
    // R160（模块 7 D2）：`[swhw7]` 两行，落点＝本函数末尾（复用 `gGvmSwLog[192]`；不使用
    //  `nredTraceFlush` 的 `body[4096]`）。**两行各自一次 `snprintf` ＋ 一次 `nredTraceLine`**（不拼行）。
    //  产出条件＝`gM7N != 0`（模块 7 本体真被进入）⇒ 门控关时一行不多。
    if (gM7N != 0) {
        int m7 = snprintf(gGvmSwLog, sizeof(gGvmSwLog),
                          "[swhw7] n=%u st=%02x hit=%x cw=%06x h3520=%llx h3528=%08x\n",
                          static_cast<unsigned>(gM7N), static_cast<unsigned>(gM7Step),
                          static_cast<unsigned>(gM7StubHit), static_cast<unsigned>(gM7Code),
                          static_cast<unsigned long long>(gM7H3520), static_cast<unsigned>(gM7H3528));
        if (m7 > 0) { nredTraceLine(gGvmSwLog, m7); }
        m7 = snprintf(gGvmSwLog, sizeof(gGvmSwLog),
                      "[swhw7s] s=%llx,%llx,%llx t=%llx,%llx,%llx,%llx,%llx\n",
                      static_cast<unsigned long long>(gM7S3552),
                      static_cast<unsigned long long>(gM7S3560),
                      static_cast<unsigned long long>(gM7S3568),
                      static_cast<unsigned long long>(gM7Tab[0]),
                      static_cast<unsigned long long>(gM7Tab[1]),
                      static_cast<unsigned long long>(gM7Tab[2]),
                      static_cast<unsigned long long>(gM7Tab[3]),
                      static_cast<unsigned long long>(gM7Tab[4]));
        if (m7 > 0) { nredTraceLine(gGvmSwLog, m7); }
    }
    // R169（模块 2 SDMA D2）：`[swsdma]` 一行（一次 snprintf ＋ 一次 nredTraceLine）。
    //  产出条件＝`gM2N != 0`（本实现真被调到）⇒ 未启用/未被调时一行不多。
    if (gM2N != 0) {
        const int ms = snprintf(gGvmSwLog, sizeof(gGvmSwLog),
                                "[swsdma] n=%u st=%02x k=%08x cfg=%llx q=%llx\n",
                                static_cast<unsigned>(gM2N), static_cast<unsigned>(gM2Step),
                                static_cast<unsigned>(gM2K),
                                static_cast<unsigned long long>(gM2Cfg),
                                static_cast<unsigned long long>(gM2Q));
        if (ms > 0) { nredTraceLine(gGvmSwLog, ms); }
    }
    // R172（模块 14 MES D2）：`[swmes]` 一行（一次 snprintf ＋ 一次 nredTraceLine）。
    //  产出条件＝`gM14N != 0`（本实现真被调到）⇒ 未启用/未被调时一行不多。
    if (gM14N != 0) {
        const int mm = snprintf(gGvmSwLog, sizeof(gGvmSwLog),
                                "[swmes] n=%u st=%02x code=%06x cfg=%llx q=%llx\n",
                                static_cast<unsigned>(gM14N), static_cast<unsigned>(gM14Step),
                                static_cast<unsigned>(gM14Code),
                                static_cast<unsigned long long>(gM14Cfg),
                                static_cast<unsigned long long>(gM14Q));
        if (mm > 0) { nredTraceLine(gGvmSwLog, mm); }
    }
    // R194（模块 15 JPEG 只读探针）：`[swjpeg]` 一行（一次 snprintf ＋ 一次 nredTraceLine）。
    //  产出条件＝`gJpegN != 0`；全标量、零解引用。
    if (gJpegN != 0) {
        const int mj = snprintf(gGvmSwLog, sizeof(gGvmSwLog),
                                "[swjpeg] n=%u st=%02x dev=%llx self=%llx fa=%llx fi=%llx\n",
                                static_cast<unsigned>(gJpegN), static_cast<unsigned>(gJpegStep),
                                static_cast<unsigned long long>(gJpegDev),
                                static_cast<unsigned long long>(gJpegSelf),
                                static_cast<unsigned long long>(gJpegFa),
                                static_cast<unsigned long long>(gJpegFi));
        if (mj > 0) { nredTraceLine(gGvmSwLog, mj); }
    }
    // R181（模块 10 DMCU S1）：`[swdmcu]` 一行（一次 snprintf ＋ 一次 nredTraceLine）。
    //  产出条件＝`gDmcuN != 0`；全标量、零解引用。
    if (gDmcuN != 0) {
        const int md = snprintf(gGvmSwLog, sizeof(gGvmSwLog),
                                "[swdmcu] n=%u st=%02x f0=%x f280=%llx\n",
                                static_cast<unsigned>(gDmcuN), static_cast<unsigned>(gDmcuStep),
                                static_cast<unsigned>(gDmcuF0),
                                static_cast<unsigned long long>(gDmcuF280));
        if (md > 0) { nredTraceLine(gGvmSwLog, md); }
    }
}

// A11 对称清理（对照上游 `723-742` 与苹果 `0x19324` 的按序释放）：释放我方资源、复位簿记；
//  **幂等**（重复调用安全）。仅失败路径调用（成功路径由进程生命周期持有）。
static void nredSwFini(void)
{
    // `complete()` 只在 `prepare()` 成功过的描述符上调用：prepare 失败者已在分配处置回 nullptr。
    if (gGvmBoDesc != nullptr) { gGvmBoDesc->complete(); gGvmBoDesc->release(); gGvmBoDesc = nullptr; }
    if (gGvmGartDesc != nullptr) {
        gGvmGartDesc->complete();
        gGvmGartDesc->release();
        gGvmGartDesc = nullptr;
    }
    __builtin_memset(&gGvmBook, 0, sizeof(gGvmBook));
}

// 回滚（R60 §1.6 第 3 条／§8A-5）：**只在"槽内仍是我方薄壳"时写回 `orig`** ⇒ 幂等、
//  **绝不覆盖他人指针**（防事故关键）；**按模块记录** ⇒ 多模块各自回滚（R60 §8A-5 末条）。
static void nredSwUninstallSlot(const UInt32 id)
{
    if (id == 0 || id > 15 || !gSwInstalled[id].installed || gSwInstalled[id].subobj == nullptr) { return; }
    void** const slot = reinterpret_cast<void**>(static_cast<UInt8*>(gSwInstalled[id].subobj) + 8);
    if (nredKernelPtrOk(slot) && nredSwIsOurFn(*reinterpret_cast<void* const*>(slot))) {
        *slot = gSwInstalled[id].orig;                  // 恢复被替换的苹果实现
        gSwInstalled[id].installed = false;
    }
}
static void nredSwUninstallAll(void)
{
    for (UInt32 i = 1; i <= 15; i += 1) { nredSwUninstallSlot(i); }
}

// 安装动作（R60 §1.1／§1.4／§8A-5）：在 `wrapMode9440f` **入口**调用（早于 `org`）。
//  ① 门控：总开关关 ⇒ 调用点已短路；该模块未启用 ⇒ 不动作；已熔断 ⇒ 不动作。
//  ② 三重校验（顺序不可换）：`obj` 有效 → `subobj = *(obj + 8 + 8*id)` 过 `nredKernelPtrOk`
//     → `cur != 0 ∧ gHwlibsSlide != 0 ∧ cur == 期望值(id) + slide`。
//  ③ 幂等：槽内已是我方薄壳 ⇒ 跳过；④ **只写 `+8`**（不动 `funcs[1..3]`＝`+16/+24/+32`，R60 §1.6-7）。
//  结果码（`gGvmSwInsCode`）：1 已安装／2 跳过（已是）／3 形态变化／4 subobj 非法／6 期望值未确证（不猜）。
//  ⚠ 日志只在 `stage == 0` 记录（R60 §5 风险 R6：避免多 stage 噪声）；**安装本身不设 stage 条件**。
static void nredSwInstallSlot(void* const obj, const UInt32 id, const UInt32 stage)
{
    if (obj == nullptr || gGvmSwFused) { return; }
    if (id == 0 || id > 15) { return; }
    if ((gSwInitIdsMask & (1U << id)) == 0) { return; }             // 该模块未启用 ⇒ 不安装（零行为）
    const bool logIt = (stage == 0);
    int m = 0;
    const NRedSwClient* const c = nredSwClientEntry(id);
    gSwInsId = id;
    if (c == nullptr) {                                             // 期望值未确证/未解出 ⇒ 不安装（不猜）
        gGvmSwInsCode = 6; gSwInsGot = 0; gSwInsExp = 0; gOrigGvmSwInit = nullptr;
        if (logIt) {
            m = snprintf(gGvmSwLog, sizeof(gGvmSwLog),
                         "[gvmsw] install: skip id=%u r=6 exp-unconfirmed\n", static_cast<unsigned>(id));
            if (m > 0) { nredTraceLine(gGvmSwLog, m); }
        }
        return;
    }
    const void* const subobj = *reinterpret_cast<void* const*>(static_cast<const UInt8*>(obj) + 8 + 8 * id);
    gSwInsExp = c->expect + gHwlibsSlide;
    if (!nredKernelPtrOk(subobj)) {                                 // ② subobj 非法 ⇒ 不装、记一行
        gGvmSwInsCode = 4; gSwInsGot = reinterpret_cast<UInt64>(subobj);
        if (logIt) {
            m = snprintf(gGvmSwLog, sizeof(gGvmSwLog), "[gvmsw] install: skip id=%u r=4 subobj=%llx\n",
                         static_cast<unsigned>(id),
                         static_cast<unsigned long long>(reinterpret_cast<UInt64>(subobj)));
            if (m > 0) { nredTraceLine(gGvmSwLog, m); }
        }
        return;
    }
    void** const slot = reinterpret_cast<void**>(static_cast<UInt8*>(const_cast<void*>(subobj)) + 8);
    void* const cur = *slot;
    gSwInsGot = reinterpret_cast<UInt64>(cur);
    if (nredSwIsOurFn(cur)) {                                       // ③ 幂等：已是我方 ⇒ 跳过
        gGvmSwInsCode = 2; gOrigGvmSwInit = cur;
        if (logIt) {
            m = snprintf(gGvmSwLog, sizeof(gGvmSwLog), "[gvmsw] install: skip id=%u r=2 already\n",
                         static_cast<unsigned>(id));
            if (m > 0) { nredTraceLine(gGvmSwLog, m); }
        }
        return;
    }
    if (cur == nullptr || gHwlibsSlide == 0 || gSwInsGot != gSwInsExp) {   // ③ 形态变化 ⇒ 不替换
        gGvmSwInsCode = 3; gOrigGvmSwInit = nullptr;
        if (logIt) {
            m = snprintf(gGvmSwLog, sizeof(gGvmSwLog),
                         "[gvmsw] install: skip id=%u r=3 form-change got=%llx exp=%llx\n",
                         static_cast<unsigned>(id),
                         static_cast<unsigned long long>(gSwInsGot),
                         static_cast<unsigned long long>(gSwInsExp));
            if (m > 0) { nredTraceLine(gGvmSwLog, m); }
        }
        return;
    }
    gOrigGvmSwInit = cur;                                           // 先存 orig，后写槽（无中间态）
    gSwInstalled[id].id        = id;
    gSwInstalled[id].subobj    = const_cast<void*>(subobj);
    gSwInstalled[id].orig      = cur;
    gSwInstalled[id].installed = true;
    *slot = reinterpret_cast<void*>(c->fn);                         // **唯一写点**：只写 `+8`
    gGvmSwInsCode = 1;
    if (logIt) {
        m = snprintf(gGvmSwLog, sizeof(gGvmSwLog),
                     "[gvmsw] install: r=1 id=%u orig=%llx new=%llx slot=%llx\n",
                     static_cast<unsigned>(id),
                     static_cast<unsigned long long>(reinterpret_cast<UInt64>(cur)),
                     static_cast<unsigned long long>(reinterpret_cast<UInt64>(c->fn)),
                     static_cast<unsigned long long>(reinterpret_cast<UInt64>(slot)));
        if (m > 0) { nredTraceLine(gGvmSwLog, m); }
    }
}

// 句柄构造所需常量与请求缓冲（R80 §3.2）：尺寸 **3776** ＝苹果 `0x19bfa` 的 size 实参／`0x1a622` 的尺寸。
//  ⚠ 缓冲用 **BSS**（静态）：苹果那版用 3776 B **栈**缓冲（`191e5 subq $3784,%rsp`）；本项目栈纪律
//    （帧 ≤128 B）⇒ **不照搬**（与既有"栈缓冲改 static"先例同理）。
//  ⚠ 重入：本缓冲同一时刻只服务一次调用；本路径（TTL 初始化、模块 5）为**单线程串行**。若将来有并发
//    调用同一实现 ⇒ **必须**改为栈缓冲或加互斥（R80 §7.3 已登记）。
static const UInt32 kGvmHandleBytes = 3776;
static UInt8 gGvmHandleReq[kGvmHandleBytes];

// ── 模块 5（GVM）本体：A1–A11（对照 `gmc_v11_0.c:744-866`）───────────────────────────────
//  返回值由本函数给出（错误码见 `kSwRes*`）；**计数、日志与戳记由统一入口负责**（单点）。
//  ⚠ 契约：只读 `info` 三处（+3656／+3664／+4..+3651）；**只写 `out+8`**；
//    **唯一例外＝调用苹果 `0x19bfa`**（框架通用"取/建句柄"查询；理由与依据见下方 S0 块注释）。
//  ⚠ 本函数在 FW bringup 深栈上被苹果间接调用 ⇒ 日志只走内存环（由统一入口调 `nredTraceLine`）。
static int nredGvmSwInit(void* const info, void* const out, const UInt64 o8init)
{
    const UInt8* const I = static_cast<const UInt8*>(info);
    // 3＝已存在、无需初始化（**我方判据**）：读入参 `out+8` 的**初值**——非 0 ⇒ 此前已初始化过
    //  （`0x98125` 的 X4 段 `9829e movq -48(%rbp),%rax ; 982a2 movq %rax,40(%rbx)` 会把该值搬进
    //  `subobj+40`）。
    //  ⚠ **与苹果实现语义不同（如实写明）**：苹果的"已存在"判据来自框架 `0x19bfa` 返 1，且它在该分支
    //    把 `out+8` **清 0** 后返 3（`1925d/19265/1926a`）；我方**保留现行判据**且**不主动清 `out+8`**
    //    （R80 U-2 裁决）。两者的**共同点**：**都不推进链条**（返 3 被上游判为失败）⇒ **不改变链条状态**。
    if (o8init != 0) { return kSwResExists; }
    gGvmBook.failMask = 0; gGvmBook.omittedMask = 0;
    // ── S0 句柄构造与回填（R80 C-4；照 `0x191dc` 的顺序，与苹果逐字一致）──────────────────────
    //  依据：R80 §2.1（亲读 `kb/序列数据/反汇编/HWLibs_full.asm` 行 27460–27560 的 `191fa..192ae`）。
    //  ⚠ **唯一"我方调用苹果函数"处**＝`0x19bfa`（框架通用"取/建句柄"查询）：它构造的请求**不含任何
    //    世代表征**（R55 §1.3／R57 §1 判"可复用"），且**被替换实现自己就是这么做的**（全库仅此一处调用点）。
    //  ⚠ **顺序不可换**：苹果把"句柄构造＋回填"放在**四个子步骤之前**（`1927d..192ae` 早于 `192b3`）。
    //  ⚠ **不得**自建对象充当句柄（N-7）：该句柄会被苹果的清理路径 `0x19324(handle)` 按序释放。
    if (gHwlibsSlide == 0) {                            // S0-a：kext 基址未解析 ⇒ 不可能安全调用
        gGvmBook.failMask |= (1U << 6);
        nredSwFini();
        return kSwResAllocFail;
    }
    const UInt64 slot1 = *reinterpret_cast<const UInt64*>(I + 3664);          // info+3664（＝slot1）
    const bool   s1ok  = nredKernelPtrOk(reinterpret_cast<const void*>(slot1));
    const UInt64 s1f8  = s1ok ? *reinterpret_cast<const UInt64*>(slot1 + 8)  : 0;
    const UInt64 s1f16 = s1ok ? *reinterpret_cast<const UInt64*>(slot1 + 16) : 0;
    //  M-1（R82；独立复核 9ae9336 §3.2／§6.1）：清理路径 `0x19c7d` 的
    //  `19c9d movq 3664(%rdi),%rcx → 19cbe movq 8(%rcx),%rdi → 19cc5 callq *24(%rcx)`
    //  **三处均无空判**，且 `%rcx` ＝ 本块写入 `handle+3664` 的 `slot1`；若 `*(slot1+24)==0`
    //  则清理路径 `callq *0` ⇒ **trap 型（本机不自动重启）** ⇒ 守卫必须覆盖 `+24`（fail-closed）。
    const UInt64 s1f24 = s1ok ? *reinterpret_cast<const UInt64*>(slot1 + 24) : 0;
    // R90：把守卫**已读**的四个值留存到全局（供 `[swihand]` 戳记；零新增解引用；无论后面返 4 还是继续）。
    gSwS1Slot = slot1; gSwS1f8 = s1f8; gSwS1f16 = s1f16; gSwS1f24 = s1f24;
    if (!s1ok || s1f8 == 0 || s1f16 == 0 || s1f24 == 0) {   // S0-b（R80）＋M-1（R82）
        gGvmBook.failMask |= (1U << 6);
        nredSwFini();
        return kSwResAllocFail;
    }
    __builtin_memset(gGvmHandleReq, 0, sizeof(gGvmHandleReq));                // ①请求缓冲清零（`191fa`）
    *reinterpret_cast<UInt64*>(gGvmHandleReq + 3664) = slot1;                 // ②透传（`0x19bfa` 要读它）
    *reinterpret_cast<UInt64*>(gGvmHandleReq + 3656) = *reinterpret_cast<const UInt64*>(I + 3656);
    void* handle = nullptr;
    using NRedHwlibsGetOrCreate = int (*)(void*, UInt64, UInt32, void**);
    const NRedHwlibsGetOrCreate fnGetOrCreate =
        reinterpret_cast<NRedHwlibsGetOrCreate>(gHwlibsSlide + 0x19bfa);      // ★ 唯一"调用苹果"处
    const int goc = fnGetOrCreate(gGvmHandleReq, kGvmHandleBytes, 1, &handle); // ③取/建（框架）
    if (goc == 1) {                                     // 苹果同分支（`19258/1925d/1926a`）：
        *reinterpret_cast<UInt64*>(static_cast<UInt8*>(out) + 8) = 0;         //   出参清 0 ＋ 返 3
        return kSwResExists;
    }
    if (!nredKernelPtrOk(handle)) {                     // 框架没给对象 ⇒ 如实失败
        gGvmBook.failMask |= (1U << 6);
        nredSwFini();
        return kSwResAllocFail;
    }
    __builtin_memset(handle, 0, kGvmHandleBytes);                             // ④＝`0x1a622` 等价（R80 §2.3）
    *reinterpret_cast<UInt64*>(static_cast<UInt8*>(out) + 8) =
        reinterpret_cast<UInt64>(handle);                                     // ⑤回填出参（`1927d/19281`）
    gSwHandleAddr = reinterpret_cast<UInt64>(handle);                         // R90：留存句柄地址（供戳记）
    *reinterpret_cast<UInt64*>(static_cast<UInt8*>(handle) + 3664) = slot1;   // ⑥**关键**（`19285/1928c`）
    *reinterpret_cast<UInt64*>(static_cast<UInt8*>(handle) + 3656) =
        *reinterpret_cast<const UInt64*>(I + 3656);                           // ⑦（`19293/1929a`）
    __builtin_memcpy(static_cast<UInt8*>(handle) + 8, I + 4, kGvmRecBytes);   // ⑧记录区 3648 B（`192a1..ae`）

    // ── A1 记录区解析（真实数据；对照 R57 A1）────────────────────────────────────────────
    //  条目布局＝{u32 键0, u32 键1, …}；本机世代键取自上游**本驱动自身的版本列表**
    //  （`gmc_v11_0.c:775-779` 的 GC 11.0.0..11.0.4）⇒ 在真实记录区里定位该键。
    UInt32 nEnt = 0, keyOff = 0xFFFFFFFFU, kMaj = 0, kMin = 0;
    {
        const UInt8* const rec = I + 4;
        for (UInt32 off = 0; off + kGvmRecStride <= kGvmRecBytes; off += kGvmRecStride) {
            const UInt32 k0 = *reinterpret_cast<const UInt32*>(rec + off);
            const UInt32 k1 = *reinterpret_cast<const UInt32*>(rec + off + 4);
            if (k0 == 0 && k1 == 0) { continue; }                   // 空条目
            nEnt += 1;
            if (keyOff == 0xFFFFFFFFU && k0 == kGvmKeyMajor && k1 <= kGvmKeyMinorMax) {
                keyOff = off; kMaj = k0; kMin = k1;
            }
        }
    }
    gGvmBook.recEntries  = nEnt;
    gGvmBook.gfxKeyMajor = kMaj;
    gGvmBook.gfxKeyMinor = kMin;
    gGvmBook.gfxKeyOff   = keyOff;
    if (nEnt == 0 || keyOff == 0xFFFFFFFFU) {                       // 无条目／无本机世代键
        gGvmBook.failMask |= (1U << 0);                             // ⇒ 标错，不继续假装成功
        return kSwResParseFail;
    }

    // ── A4 显存几何（对照 `:755-760`）——**本轮如实省略**（R57 明确"暂可省"；R80 U-3 裁决）──────────
    //  **省略了什么**：显存几何＝`vram_width`／`vram_type`／`vram_vendor` **三步全未做**（本回调用点拿不到
    //  数据源：它由 X6000FB 阶段解析 atom/固件产生）⇒ 字段如实留 0 ＋ `omittedMask` bit3；
    //  **且未做任何"硬件就绪确认"**（本轮不读寄存器——红线）。⇒ **不计入失败**，但**不得**据此推断
    //  "几何已就绪"。
    gGvmBook.vramWidth = 0; gGvmBook.vramType = 0; gGvmBook.vramVendor = 0;
    gGvmBook.vramOmitted = 1;
    gGvmBook.omittedMask |= (1U << 3);

    // ── A2 GMC 簿记（对照 `:788-789`／`:795`／`:827-828`）────────────────────────────────
    gGvmBook.mcMask       = 0xFFFFFFFFFFFFULL;                      // :827（48 bit MC）
    gGvmBook.pteAddrMask  = kPteAddrMask;                           // :828（48 bit PA）
    gGvmBook.vmSizeBytes  = 256ULL * (1ULL << 40);                  // :795（256 TB＝256*1024 MB）
    gGvmBook.vmBlockBytes = 512;                                    // :795（块 512＝fragment 9 bit）
    gGvmBook.vmLevels     = 3;                                      // :795（3 级）
    gGvmBook.vmBits       = 48;                                     // :795（48 位）
    gGvmBook.vmhubsMask   = (1U << 0) | (1U << 1);                  // :788/789 GFXHUB0(0)｜MMHUB0(0)

    // ── A3 两个 hub 簿记（对照 `:749`／`:751`；**仅簿记**，不碰 MMIO／寄存器）────────────────
    gGvmBook.hubBit[0] = 1;   // MMHUB0（`AMDGPU_MMHUB0(0)`）
    gGvmBook.hubBit[1] = 0;   // GFXHUB0（`AMDGPU_GFXHUB(0)`）
    for (UInt32 i = 0; i < 2; i += 1) {
        gGvmBook.hubVaStart[i] = 0;
        gGvmBook.hubVaSize[i]  = gGvmBook.vmSizeBytes;
        gGvmBook.hubReady[i]   = 1;
    }

    // ── A5 BO 管理器＋一次自检分配（对照 `:843` `amdgpu_bo_init`）────────────────────────────
    //  真实动作＝物理连续缓冲分配＋登记（沿用本仓库既有 `IOBufferMemoryDescriptor` 惯例）。
    if (gGvmBoDesc == nullptr) {
        gGvmBoDesc = IOBufferMemoryDescriptor::withOptions(
            kIOMemoryPhysicallyContiguous | kIODirectionInOut, 4096, 4096);
        if (gGvmBoDesc != nullptr && gGvmBoDesc->prepare() != kIOReturnSuccess) {
            gGvmBoDesc->release();
            gGvmBoDesc = nullptr;
        }
    }
    IOByteCount boSeg = 0;
    const addr64_t boPhys = (gGvmBoDesc != nullptr) ? gGvmBoDesc->getPhysicalSegment(0, &boSeg, 0) : 0;
    if (gGvmBoDesc == nullptr || boPhys == 0 || boSeg < 4096) {
        gGvmBook.failMask |= (1U << 4);
        nredSwFini();
        return kSwResAllocFail;
    }
    gGvmBook.boPhys = boPhys; gGvmBook.boBytes = 4096; gGvmBook.boRegistered = 1;

    // ── A6 GART 结构＋内存可见表项（对照 `:723-742` `gmc_v11_0_gart_init`／`:847`）────────────
    //  真实动作：分配页表（GART 512 MB ⇒ 131072 项 × 8 B＝1 MB，物理连续、页对齐）→ 清零（无效
    //  表项）→ 把 A5 的自检分配写进表项 0（**内存可见的表项写入**＝本步的物理证据）。
    //  表项组成＝`(boPhys & kPteAddrMask) | kGartPteFlags`（对照 `amdgpu_gmc.c:173-175` 的组合式与
    //  `gmc_v11_0.c:828` 的掩码）；位值逐位核对与 MTYPE 留空理由见 `kGartPteFlags` 处注释。
    const UInt32 kGartPages = kGvmGartSizeMB * 1024U * 1024U / 4096U;   // 131072
    const UInt32 kGartBytes = kGartPages * 8U;                          // 1 MB
    if (gGvmGartDesc == nullptr) {
        gGvmGartDesc = IOBufferMemoryDescriptor::withOptions(
            kIOMemoryPhysicallyContiguous | kIODirectionInOut, kGartBytes, 4096);
        if (gGvmGartDesc != nullptr && gGvmGartDesc->prepare() != kIOReturnSuccess) {
            gGvmGartDesc->release();
            gGvmGartDesc = nullptr;
        }
    }
    IOByteCount gartSeg = 0;
    const addr64_t gartPhys =
        (gGvmGartDesc != nullptr) ? gGvmGartDesc->getPhysicalSegment(0, &gartSeg, 0) : 0;
    UInt64* const ptes = (gGvmGartDesc != nullptr)
        ? reinterpret_cast<UInt64*>(gGvmGartDesc->getBytesNoCopy()) : nullptr;
    if (ptes == nullptr || gartPhys == 0 || gartSeg < kGartBytes) {
        gGvmBook.failMask |= (1U << 5);
        nredSwFini();
        return kSwResAllocFail;
    }
    __builtin_memset(ptes, 0, kGartBytes);                              // 清零：其余表项＝无效
    ptes[0] = (boPhys & kPteAddrMask) | kGartPteFlags;                  // 表项 0（＝0x73 组合）
    gGvmBook.gartPhys = gartPhys;
    gGvmBook.gartBytes = kGartBytes;
    gGvmBook.gartPtes = 1;                                              // 已写入的表项数（真实计数）
    //  ⚠ **层次声明**（R70 §2.2／U-3 裁决）：本步（GART 表结构与表项写入）当前**只达"簿记层"**
    //    ——表已建、表项已写，但**没有消费者**（真实就绪需按 R57 A6 读寄存器确认，红线禁止）⇒
    //    其"完成"**不代表对硬件可用**；本轮**不收紧**其判定（避免扩大改动面，见 U-3 裁决）。

    // ── A7 VM 管理器簿记（对照 `:851-859`）───────────────────────────────────────────────
    gGvmBook.firstKfdVmid = 8;      // VMID 0＝System；1-7＝gfx/compute；8-15＝KFD
    gGvmBook.vmidTotal    = 16;
    gGvmBook.vmRegistered = 1;      // 登记一个 VM（簿记级自检）

    // ── A8 中断登记（**额外步骤：不属于被替换实现的契约 ⇒ 不参与成功判定**；R76 定案／R80 R1）────
    //  上游三条（`gmc_v11_0.c:802`／`:809`／`:817`；`ih_v6_1.c:556` 属 IH 初始化阶段，不在 GMC
    //  sw_init 内 ⇒ 不做）：
    //   ① VMC(0x12) ＋ VMC_1_0__SRCID__VM_FAULT(0)  → 源＝VM fault
    //   ② GFX(0x0a) ＋ UTCL2_1_0__SRCID__FAULT(0)   → 源＝VM fault（上游用同一个 irq 对象）
    //   ③ DF (0x17) ＋ 0（上游字面量）               → 源＝ECC（非 SRIOV 分支；本机非 SRIOV）
    //  完成判据（R60 §2.4／§8B-2）：**登记项已写入我方登记表 ＋ 簿记可查 ＋ 戳记可核**。
    //  ⚠ **分层声明（必须写清）**："故障真的被处理"**不在本轮判据内**——我方登记表当前**没有
    //    消费者**（苹果侧 IH 分发器身份未解出，R60 §8D 新 N6）⇒ **不得**把"登记写入成功"表述为
    //    "中断可用"；硬件侧（IH 环／寄存器）属硬件初始化阶段，本轮不做。
    gIrqEntryN = 0;                                          // 幂等：每次进入重建（表内容确定性）
    gIrqRegistered = 0;
    if (nredSwIrqAddId(kIhClientVmc, kIhSrcVmFault, kIrqSrcVmFault) &&
        nredSwIrqAddId(kIhClientGfx, kIhSrcUtcl2Fault, kIrqSrcVmFault) &&
        nredSwIrqAddId(kIhClientDf, kIhSrcDfEcc, kIrqSrcEcc)) {
        gIrqRegistered = 1;                                  // 三条**全部真实写入**
    }
    // ── A8 的结论（R80 C-3）：**登记照旧真实执行并记录，但不置任何失败位**──────────────────────
    //  机器码依据（R76）：被替换实现 `0x191dc` 全段 82 条**不含任何 (client,src) 登记**；本 kext 内
    //    **无 IH／SRCID 家族**（文件名串家族仅 `ipi_*.c`／`ttl_*.c`；185 条"中断"串全属 SMU/PPP 电源
    //    管理侧）⇒ A8 是本项目按**上游 Linux 语义**多加的一步 ⇒ 它的缺失**不能让"照被替换实现做事"
    //    被判为失败**（已删除原"置 A8 失败位（bit7）"那一行）。
    //  ⚠ **不得**因此宣称"中断可用"：我方登记表当前**没有消费者**（苹果侧中断分发器身份【未解出】）。
    //  ⚠ **恢复条件**（若将来要把它纳入判据）：消费者身份已明 ∧ 能触发一次真实分发 ∧ 有可观测判据齐备
    //     ⇒ 才可换回"**按 `gIrqRegistered` 置 A8 失败位（bit7）**"的判定。
    //  ⚠ **不得**把这三条登记改挂到 SMU/PPP 族的 `register*Interrupt` 接口（语义不等价＝红线）。
    gGvmBook.irqCount      = gIrqEntryN;               // 真实计数（写入成功几条就是几条，供判读）
    gGvmBook.irqRegistered = gIrqRegistered;
    // ── A9 RAS 簿记（对照 `:861`）——**本轮如实省略**（R57 判"可省"；R80 U-3 裁决）────────────────
    //  **省略了什么**：RAS 簿记（错误上报相关结构）**整体未实现**（`amdgpu_gmc_ras_sw_init` 等价物），
    //  如实登记 `rasSkipped=1` ＋ `omittedMask` bit8；**且未做任何硬件就绪确认**（不读寄存器——红线）。
    //  ⇒ **不计入失败**，但**不得**据此推断"RAS 可用"。
    gGvmBook.rasSkipped = 1;
    gGvmBook.omittedMask |= (1U << 8);

    // ── A10 回填：**已前移到 S0（照苹果顺序）**（R80 C-5）──────────────────────────────────────
    //  `0x191dc` 的回填发生在**四个子步骤之前**（`1927d..192ae`）⇒ 我方 S0 块已逐条照做：
    //  `out+8 = handle`（⑤）／`handle+3664 ← info+3664`（⑥，**关键**：下游 `0x19c7d` 会**无空判**读它）／
    //  `handle+3656 ← info+3656`（⑦）／`handle+8 起 3648 B ← info+4 起`（⑧）。
    //  ⚠ **旧 A10（`0x1937f` 段的六字段回填 `3672/3680/3688/3692/3696/3704`）不在本卡范围**：
    //    那段代码属**另一个回调槽** `funcs[3]=0x1936e`（R57 旧 A10；R80 §2.4 亲读函数边界）⇒ 仍由苹果
    //    自带实现执行，我方**不需要**补这六个字段。

    // ── 返回值判定（R60 §1.2／§2.4）：任一失败步 ⇒ 非 0；全部真实成功 ∧ 放行门控开 ⇒ 0 ──────────
    if (gGvmBook.failMask != 0) {                       // 失败 ⇒ 熔断回滚（按模块）＋对称清理（A11）
        nredSwUninstallAll();
        gGvmSwFused = true;
        nredSwFini();
        return kSwResStepFail;
    }
    // ⚠ **放行开关**（R70 §3／需求 R3）：`-NRedGvmSwInitPass` 的语义＝"**全部步骤真实做成之后**，是否
    //    允许把成功报给苹果"的**最终闸门**（默认关 ⇒ 逐轮单变量对照）。
    if (!gGvmSwInitPass) { return kSwResPassGateOff; }
    // ★ **成功出口**（R80 C-6／需求 R1＋R3）：走到这里 ⇒ 上面每一步都**真实做成**——S0（框架取/建句柄
    //    ＋清零＋回填出参＋两个字段搬移＋记录区拷贝）＋ A1／A2／A3／A5／A6／A7 全部成功；
    //    A4／A9 为**如实登记的省略项**（不计入失败；注释已写明省略内容与"未做硬件就绪确认"）；
    //    A8 为**不参与判定的额外步骤**（登记照做、无消费者、不得宣称"中断可用"）⇒ 报成功（0）。
    return kSwResPass;                                  // ★ 真实成功（R80 起**恢复可达**）
}

// ── 模块 8（SMU）本体：**D2＝我方自带 sw-init**（R141 设计＋复核修订；R143 落地）────────────────────
//  被替换实现＝`0x6430f`（装配点 `0x9e4c9`）；寄存器别名：`r15`＝`I`（info）、`r14`＝`O`（out）、
//  `local`＝`rbp-2000`（1968 B）、`scratch`＝`rbp-32`。机器码对照源：
//  `llvm-objdump -d --start-address=0x6432b --stop-address=0x6463b
//   "kb/内核扩展/13.6/extracted/com.apple.kext.AMDRadeonX5000HWLibs.macho"`
//  **D2 动作集**（R141 §1）：**保** `#1..#19`、`#20a`＝`0x64b1a(h,h+1728,240)`、`#20b`＝`0x66f18(h)`、
//    `#21`、`#22`；**删**苹果 `#20`（`0x6720b`：版本分类／版本门／变体装表）与 `#23`（`0x67328`：槽 0 →
//    `0x65156` → 条件 `0x65b34`；含**槽 29 私有常量路径**）；**加** D2-1（显式清零 `h+888/896/904/912`）／
//    D2-2（回读核对）／D2-3（`omittedMask` 登记）／D2-4（**不设 `h+720`**）。
//  ❌ **不得**把本实现写作"模块 8 完成"或"SMU 就绪"——本实现＝**"SMU 软件侧最小初始化（M1–M7）"**；
//     SMU **硬件侧**（H2–H7：固件状态检查／驱动表与工具表地址／表传输／允许特性掩码／显存池位置／消息通道）
//     **未做**（授权或裁决项）⇒ 已逐条登记在 `omittedMask`。
//  ★ **"模块 8 完成"的唯一判据**＝真机 `[swres] r[8]==00` ∧ `f==0` ∧ **槽 1..29 与 M8–M13 已按裁决补齐**；
//     **"链条越过 8"不等于模块 8 完成**——苹果的 hw-init（`0x6463f`→`0x67423`）因**槽 1 为空**会**静默返 0**
//     （＝"成功但未做"）；槽 1..29 我们**没有**对等实现，且**未主动失败**（如实登记，不伪装）。
//  ⚠ **已知关闭项（如实登记）**：`h+888` 置 0 ⇒ `0x687a6`／`0x68daf`／`0x6fb27`／`0x74db6` 四个包装器
//     恒走"空跑／返 0"分支 ⇒ 该 4 项功能在 D2 后关闭（`omittedMask bit27`）；苹果侧常态值**未证**
//     （其值由框架填的 `O+32/40/…` 而来，复核 M-3）⇒ 与苹果的差异**不可离线判定**。
//  ⚠ **失败面**＝2／4（`bit16`）／5（`bit15`）／6（`bit18`／`bit27`／`bit28`）／7；`bit17` **已随 `#20`／`#23`
//     的删除而不再被本函数使用**。成功出口只有一处（函数尾 `return kSwResPass`）。
//  ⚠ **接受的偏差（如实登记；R111/N-2）**：**失败路径不调用苹果清理 `0x64a9c`**（苹果原实现在
//    `6460a..64626` 调用）⇒ `#11` 之后失败的句柄与 `h+1712` 子对象**会泄漏**。理由：该调用**不在
//    R108 §2 的授权清单内** ⇒ 不得自行补调用。
//  ⚠ **残余风险（如实登记）**：`#10`（`0x64b1a(L,h,1968)`，`6440c`）**同样**走 `movq 16(%rdi),%rcx →
//     callq *72(%rcx)`（此处 `rdi`＝`L`，其 `+16`＝`f696`）⇒ 本 D2 guard 依 R141 的位置在 `#19` 之后
//     ⇒ **不覆盖 `#10`**。该风险在苹果原实现中同样存在（苹果亦不查）；若要覆盖须把 guard 前移到 `#8`
//     之后（**本卡未做**，避免偏离 R141 的动作顺序）。
//  ⚠ **默认不可达**：`gSwInitIdsMask` 初值只含模块 5；即便掩码含 8，放行门关也只返 7（R108 §4）。
//  ⚠ **与苹果的如实差异**：`#10`／`#22`（`0x64b1a`，自身约定＝**0 成功／2 失败**）的返回值苹果**未判**，
//     本实现按 R108 §1 判 ⇒ 只会更早失败；`#20a`／`#20b` 的返回值**照苹果丢弃**（`6723b`／`67243` 之后无判据）。
//  ⚠ **先决检查 3 处**（只会更早失败）：① `gHwlibsSlide == 0`；② `nredKernelPtrOk(h)`（`0x649ee` 返回后）；
//     ③ `*(h+1712) != 0`（**R111/N-1 更正**：`0x64b1a` 对 `rsi==0` 自行显式返 2、**不解引用** ⇒ 它不是
//     "防 trap"，而是"提前失败"，与 `#22` 自身判据结果等价——两条路都返 4（`bit16`））。
static const UInt32 kSmuLocalBytes = 1968;   // `0x64334` 的 `$1968`
static UInt8 gSmuLocalBuf[kSmuLocalBytes];   // `local`（`rbp-2000`）⇒ **static＝非可重入**（栈纪律：帧 ≤128 B）
static int nredSmuSwInit(void* const info, void* const out)
{
    const UInt8* const I = static_cast<const UInt8*>(info);
    UInt8* const O = static_cast<UInt8*>(out);
    gGvmBook.failMask = 0; gGvmBook.omittedMask = 0;   // 清零后按 **D2-3** 逐项登记（`bit27`／`29`／`30`／`31`）
    UInt8* const L = gSmuLocalBuf;
    // ── #1~#6 缓冲清零与形状校验（纯读）───────────────────────────────────────────────────────
    __builtin_memset(L, 0, kSmuLocalBytes);                                        // #1（`64334/64339`）bzero(local,1968)
    if (I == nullptr || O == nullptr) { return kSwResNullArg; }                    // #2（`64343..6434f`）⇒ 2
    gM8N += 1;                                                                    // R129：戳记"跑过"判据
    // R145/N-3：**入口重置**这 11 枚读数——同一 boot 内模块 8 若被进入 ≥2 次且后一次早退，戳记必须只反映
    //  **本次**的值 ⇒ 每次进入先归哨兵，再由流程按需写入（哨兵：`0xFF`／`0xFFFFFFFF`／`0xFFFFFFFFFFFFFFFF`）。
    gM8V612 = 0xFFFFFFFFU;
    gM8H712 = 0xFFFFFFFFU; gM8H720 = 0xFFU;
    gM8H1728 = 0xFFFFFFFFFFFFFFFFULL;
    gM8H888 = 0xFFFFFFFFFFFFFFFFULL; gM8H896 = 0xFFFFFFFFFFFFFFFFULL;
    gM8H904 = 0xFFFFFFFFFFFFFFFFULL; gM8H912 = 0xFFFFFFFFFFFFFFFFULL;
    gM8H1712 = 0xFFFFFFFFFFFFFFFFULL;
    gM8H1736 = 0xFFFFFFFFFFFFFFFFULL; gM8H1960 = 0xFFFFFFFFFFFFFFFFULL;
    if (*reinterpret_cast<const UInt32*>(I + 0) != 704) {                          // #3（`6435a/64361`）
        gGvmBook.failMask |= (1U << 15); *reinterpret_cast<UInt64*>(O + 8) = 0; return kSwResParseFail;
    }
    if (*reinterpret_cast<const UInt32*>(O + 0) != 80) {                           // #4（`64367/6436b`）
        gGvmBook.failMask |= (1U << 15); *reinterpret_cast<UInt64*>(O + 8) = 0; return kSwResParseFail;
    }
    if (*reinterpret_cast<const UInt32*>(I + 8) != 1) {                            // #5（`64371/64376`）
        gGvmBook.failMask |= (1U << 15); *reinterpret_cast<UInt64*>(O + 8) = 0; return kSwResParseFail;
    }
    // ── 231：FwBringup 挂点迁移（方案 B）────────────────────────────────────────────────────
    //  原 C2 出口 `0x6720b` 因 D2 删除 #20 而不可达；改在 #5（`*(I+8)==1`）之后、#6 之前调用。
    //  此时 `I` 已过 #3–#5（704/80/1）⇒ 与原挂点所传 ctx 同一对象。仅 `-NRedFwBringup` 时执行；
    //  不写 failMask、不改返回值；关态零行为（门控在 `runFwBringupAtModule8` 内判断）。
    X5000HWLibs::runFwBringupAtModule8(const_cast<void*>(static_cast<const void*>(I)));
    const UInt32 v = *reinterpret_cast<const UInt32*>(I + 612);                    // #6（`6437c`）
    gM8V612 = v;                                                                  // R129：判别式读数（R128 §4.3）
    if ((v & 8U) != 0 && ((((v >> 2) | (v >> 4)) & 1U) == 0)) {                    //  `64383..64393` 观测类判据
        gGvmBook.failMask |= (1U << 15); *reinterpret_cast<UInt64*>(O + 8) = 0; return kSwResParseFail;
    }
    // ── #7#8 两个入参字段（源在 `I`，去向是 `local`）──────────────────────────────────────────
    const UInt64 f696 = *reinterpret_cast<const UInt64*>(I + 696);                 // #7 源（`643be`）
    const UInt64 f688 = *reinterpret_cast<const UInt64*>(I + 688);                 // #8 源（`643d0`）
    //  R108 §6：阶段 1 的"两字段非零登记"**保留**（苹果 `643be/643d0` 只搬不判 ⇒ 本处更严，只会更早失败）。
    if (f688 == 0 || f696 == 0) {
        gGvmBook.failMask |= (1U << 15); *reinterpret_cast<UInt64*>(O + 8) = 0; return kSwResParseFail;
    }
    *reinterpret_cast<UInt64*>(L + 16) = f696;                                     // #7（`643c5/643cc`）
    *reinterpret_cast<UInt64*>(L + 8)  = f688;                                     // #8（`643d0/643d7`）
    if (gHwlibsSlide == 0) {                        // 先决①：kext 基址未解析 ⇒ 不可安全调用（同 m5 的 S0-a）
        gGvmBook.failMask |= (1U << 16); *reinterpret_cast<UInt64*>(O + 8) = 0; return kSwResAllocFail;
    }
    // ── #9#10 取/建 handle 本体＋用 `local` 初始化（**苹果调用**；失败 ⇒ `bit16` ＋ 4）─────────────
    void* h = nullptr;                                                             // `scratch`（`rbp-32`）
    using NRedSmuGetOrCreate = int (*)(void*, UInt64, UInt32, void**);
    using NRedSmuInitFrom    = int (*)(void*, void*, UInt64);
    const NRedSmuGetOrCreate fnGetOrCreate =
        reinterpret_cast<NRedSmuGetOrCreate>(gHwlibsSlide + 0x649ee);              // 授权①（#21 复用）
    const NRedSmuInitFrom fnInitFrom =
        reinterpret_cast<NRedSmuInitFrom>(gHwlibsSlide + 0x64b1a);                 // 授权②（#22 复用）
    const int rc9 = fnGetOrCreate(L, kSmuLocalBytes, 1, &h);                       // #9（`643db..643e9`）非 0＝失败
    if (rc9 != 0) {
        gGvmBook.failMask |= (1U << 16); *reinterpret_cast<UInt64*>(O + 8) = 0; return kSwResAllocFail;
    }
    const int rc10 = fnInitFrom(L, h, kSmuLocalBytes);                             // #10（`643f8..6440c`）
    if (rc10 != 0) {                                                               //  ★ 苹果未判（见头注差异）
        gGvmBook.failMask |= (1U << 16); *reinterpret_cast<UInt64*>(O + 8) = 0; return kSwResAllocFail;
    }
    if (!nredKernelPtrOk(h)) {                     // 先决②：非空 ≠ 合法内核指针（其后全按句柄解引用）
        gGvmBook.failMask |= (1U << 16); *reinterpret_cast<UInt64*>(O + 8) = 0; return kSwResAllocFail;
    }
    UInt8* const H = static_cast<UInt8*>(h);
    // ── #11~#18 写出参、句柄字段、位域与三次纯内存搬运 ─────────────────────────────────────────
    *reinterpret_cast<UInt64*>(O + 8) = reinterpret_cast<UInt64>(h);               // #11（`64411/64415`）
    *(O + 16) = 0;                                                                 // #11（`64419`）u8
    *reinterpret_cast<UInt64*>(H + 16) = f696;                                     // #12（`6441e/64425`）
    *reinterpret_cast<UInt64*>(H + 8)  = f688;                                     // #13（`64429/64430`）
    // ── #14 `*(u16*)(h+728)` 位域：把 `v` 逐位搬运（**完整两组**，共 9 组位操作）────────────────────
    //  苹果**读 32 位**（`64448`）、**回写 16 位**（`6451f`）⇒ 本实现同用 32 位累加器、末尾写 u16。
    {
        UInt32 a = *reinterpret_cast<const UInt32*>(H + 728);                      // `64448`（32 位读）
        a = (a & ~1U)     | (((v >> 2) | (v >> 4)) & 1U);                          // `6443b..64453` → bit0
        a = (a & ~2U)     | ((v >> 2) & 2U);                                       // `6445a..6446c` → bit1
        a = (a & ~16U)    | ((v >> 1) & 16U);                                      // `64473..64484` → bit4
        a = (a & ~64U)    | (v & 64U);                                             // `6448b..6449b` → bit6
        a = (a & ~128U)   | (v & 128U);                                            // `644a2..644b7` → bit7
        a = (a & ~8U)     | ((v & 1U) << 3);                                       // `644be..644ce` → bit3
        a = (a & ~0x100U) | ((v & 2U) << 7);                                       // `644d5..644e9` → bit8
        a = (a & ~0x200U) | ((v & 0x100U) << 1);                                   // `644f0..64504` → bit9
        a = (a & ~0x400U) | ((v & 0x200U) << 1);                                   // `6450b..6451f` → bit10
        *reinterpret_cast<UInt16*>(H + 728) = static_cast<UInt16>(a);              // `6451f`（16 位写）
    }
    *reinterpret_cast<UInt32*>(H + 716) = 0x80000U;                                // #15（`64526`）常量
    *reinterpret_cast<UInt64*>(H + 952) = 0;                                       // #16（`64530`）显式清零
    __builtin_memcpy(H + 32, I + 4, 608);                                          // #17（`6453b..64547`）608 B
    __builtin_memcpy(H + 640, I + 616, 72);                                        // #18（`6454c..64561`）72 B
    // ── #19（`0x65885` setter，**无判据**）＋其后为 **D2 动作**（苹果 `#20`／`#23` 已按 R141 §1 删除）────────
    using NRedSmuSetter = int (*)(void*, UInt32, UInt32);
    const NRedSmuSetter fnSetter = reinterpret_cast<NRedSmuSetter>(gHwlibsSlide + 0x65885);
    // #19（`64566..64571`）：`0x65885(h, 0xC0201, 0)`。**R111/M-A 修正**：该函数是**纯 setter**——
    //  全函数仅 7 条指令、**无分支**（`65889 movl %esi,%eax`／`6588b movq %rax,960(%rdi)`／
    //  `65892 movq %rdx,968(%rdi)`／`65899 retq`）⇒ **返回值＝它自己的第 2 实参**（此处 `0xC0201` ≠ 0）
    //  ⇒ 若判 `rc != 0` 则**判据恒真** ⇒ 阶段 2 的**成功出口不可达**、真机读数恒为失败并**误归因**。
    //  旁证：苹果自身 11 处调用点**无一**以其返回值作判据 ⇒ **本处判据已删除、只保留调用**
    //  （R108 U-5 由此结案：`0x65885`＝纯 setter，"无状态可判"）。
    fnSetter(h, 0xC0201U, 0);
    // ── D2（R143）：**删除苹果 `#20` 与 `#23`**（R141 §1）───────────────────────────────────────────
    //  为什么删：模块 8 的"照做"路线已被四轮取证关闭（槽 29 的常量只存在于苹果私有 `__data`、本机 ASIC
    //  不在其支持集、记录表里也没有）⇒ 苹果 `#20`（`0x6720b`：版本分类＋版本门＋变体装表）与 `#23`
    //  （`0x67328`：槽 0 → `0x65156` → 条件 `0x65b34`；含**槽 29 私有常量路径**）对**本机硬件结构性不适用**。
    //  ⇒ 改由我方自带 sw-init：**做能诚实做的真实动作**，未做项逐条登记（见函数尾部的 `omittedMask`）。
    //  ⚠ **保留了 `#20` 内部两项与"版本门"无关的真实子动作**（R141 §1-5／§1-6 逐字地址）：
    //    `#20a`＝`0x64b1a(h, h+1728, 240)`（框架交表；`6722c..6723b`）——**返回值照苹果丢弃**（其后无 `testl`）；
    //    `#20b`＝`0x66f18(h)`（读 5 个配置值写 `h+784/776/780/824/…`；`67240..67243`）——返回值苹果亦丢弃。
    //  ⚠ **D2 guard（`bit27`）——必须在 `#20a`／`#22` 之前**：`0x64b1a` 会 `movq 16(%rdi),%rcx` 再
    //    `callq *72(%rcx)`（`64b3b`／`64b57`）⇒ 若 `*(h+16)==0` ⇒ 读 `0x8`（CR2=0x8）；若 `*(X+72)==0`
    //    ⇒ `callq *0`（RIP=0）。两者都由本 guard 挡下（R136 §6.1／复核 R6）。
    {
        const UInt64 X = *reinterpret_cast<const UInt64*>(H + 16);
        if (!nredKernelPtrOk(reinterpret_cast<const void*>(X)) ||
            *reinterpret_cast<const UInt64*>(X + 72) == 0) {
            gGvmBook.failMask |= (1U << 27);                                       // ★ D2 guard（"服务对象不可用 ⇒ 6"）
            *reinterpret_cast<UInt64*>(O + 8) = 0;
            return kSwResStepFail;                                                 // 6
        }
    }
    fnInitFrom(h, reinterpret_cast<void*>(H + 1728), 240);   // #20a（`6722c`）**返回值照苹果丢弃**
    using NRedSmuCfgRead = int (*)(void*);
    const NRedSmuCfgRead fnCfgRead = reinterpret_cast<NRedSmuCfgRead>(gHwlibsSlide + 0x66f18);
    fnCfgRead(h);                                            // #20b（`67240`）**返回值苹果亦丢弃**（`67243`）
    // #21（`6458d..645a2`）取/建 `h[1712]` 的 `0x8000` 子对象——**第 4 实参＝`h+1712`（内部指针）**
    const int rc21 = fnGetOrCreate(h, 0x8000U, 1, reinterpret_cast<void**>(H + 1712));
    if (rc21 != 0) {
        gGvmBook.failMask |= (1U << 16); *reinterpret_cast<UInt64*>(O + 8) = 0; return kSwResAllocFail;
    }
    const UInt64 hSub = *reinterpret_cast<const UInt64*>(H + 1712);                // #22 第二实参（`645b1`）
    if (hSub == 0) {                               // 先决③（**R111/N-1 更正**：非"防 trap"——`0x64b1a`
                                                   //   对 `rsi==0` 自行显式返 2、不解引用 ⇒ 本检查只是
                                                   //   "提前失败"，结果与 #22 判据等价：都返 4）
        gGvmBook.failMask |= (1U << 16); *reinterpret_cast<UInt64*>(O + 8) = 0; return kSwResAllocFail;
    }
    const int rc22 = fnInitFrom(h, reinterpret_cast<void*>(hSub), 0x8000U);        // #22（`645ad..645bd`）
    if (rc22 != 0) {                                                               //  ★ 苹果未判（见头注差异）
        gGvmBook.failMask |= (1U << 16); *reinterpret_cast<UInt64*>(O + 8) = 0; return kSwResAllocFail;
    }
    // ── D2-1（R143）：**显式清零**苹果 `#23` 的四个产物槽 `h+888/896/904/912`（R141 §1-11／M7）────────
    //  这四槽的**唯一写者＝`0x65156`**（`651df`／`651ea`／`651f8`／`651ff`），而 `0x65156` 只由**已删的
    //  `#23`** 路径调用 ⇒ 不走它时**必须显式置 0**、**不得留下垃圾值**（真写动作，非空桩）。
    //  ⚠ **已登记**：置 0 使 **4 个下游包装器恒走"空跑／返 0"分支** ⇒ 相关 4 项功能在 D2 后**关闭**
    //    （`omittedMask bit27`；读者＝`0x687a6`／`0x68daf`／`0x6fb27`／`0x74db6`）。
    *reinterpret_cast<UInt64*>(H + 888) = 0;
    *reinterpret_cast<UInt64*>(H + 896) = 0;
    *reinterpret_cast<UInt64*>(H + 904) = 0;
    *reinterpret_cast<UInt64*>(H + 912) = 0;
    // ── D2 观测读数（戳记与下面的回读核对**共用**；全部只读，偏移 ≤ `1960+8` ⇒ 都在 1968 B 对象内）────────
    gM8H712  = *reinterpret_cast<const UInt32*>(H + 712);
    gM8H720  = *reinterpret_cast<const UInt8*>(H + 720);                           // R145/N-2：**按 u8 读**
    gM8H888  = *reinterpret_cast<const UInt64*>(H + 888);
    gM8H896  = *reinterpret_cast<const UInt64*>(H + 896);
    gM8H904  = *reinterpret_cast<const UInt64*>(H + 904);
    gM8H912  = *reinterpret_cast<const UInt64*>(H + 912);
    gM8H1712 = *reinterpret_cast<const UInt64*>(H + 1712);
    gM8H1728 = *reinterpret_cast<const UInt64*>(H + 1728);
    gM8H1736 = *reinterpret_cast<const UInt64*>(H + 1736);                         // 槽 1（苹果 hw-init 入口判据）
    gM8H1960 = *reinterpret_cast<const UInt64*>(H + 1960);                         // 槽 29（uCode 常量）
    // ── D2-2（R143）：**回读核对**（机械可核；R141 §1-12）⇒ 不满足即 `bit28` ＋ 6（fail-closed）──────────
    //  判据＝四槽**均为 0** ∧ `*(h+1712) != 0`（**按卡面／R141 §1-12 的严格形式**：不含设计里那条
    //  "`h+784/776/780` 至少一项非 0"的**弱代理**——复核 N-3 已指出它可能误报失败）。
    if (gM8H888 != 0 || gM8H896 != 0 || gM8H904 != 0 || gM8H912 != 0 || gM8H1712 == 0) {
        gGvmBook.failMask |= (1U << 28);                                           // ★ D2-2 回读核对失败
        *reinterpret_cast<UInt64*>(O + 8) = 0;
        return kSwResStepFail;                                                     // 6
    }
    // ── 守卫（R108 §3 逐字；**唯一差异**＝把局部别名 `H` 改名为 `HG`，避免与外层 `H` 遮蔽）──────────
    //  清理链共**三处**零空判用到 `h` 的派生字段：
    //    (a) `0x64a9c`：`64ac0` 读 `*(h+16)`；`64ada` 用 `*(X+8)`；`64ade` 调 `*(X+24)`；
    //    (b) `0x64c1e`：`64c22` 读 `*(h+16)`；`64c26` 用 `*(X+8)`；`64c2a` 调 `*(X+56)`   ★ M-2
    //    (c) `0x64a9c(h, *(h+1712))`：`64ac8` 读 `*(arg2-8)`（arg2＝`*(h+1712)`）
    //  ※ `0x64c1e` 的第二实参全程未被读 ⇒ `*(h+912)`／`*(h+944)` 为 0 也安全（复核 §2.2）。
    //  **R111/N-3 口径**：`bit18` ⇒ **`xOk` 失败**（`*(X+8)`／`*(X+24)`／`*(X+56)` 任一为 0，或 `X` 非法）；
    //    另一条件 `sub == 0` 已在先决③处（`bit16`）拦下 ⇒ **`sub==0` 分支实流不可达**（保留以逐字对齐 R108 §3）。
    {
        const UInt8* const HG  = static_cast<const UInt8*>(h);
        const bool    hOk      = nredKernelPtrOk(h);
        const UInt64  X        = hOk ? *reinterpret_cast<const UInt64*>(HG + 16) : 0;
        const bool    xOk      = hOk && nredKernelPtrOk(reinterpret_cast<const void*>(X))
                                 && *reinterpret_cast<const UInt64*>(X + 8)  != 0
                                 && *reinterpret_cast<const UInt64*>(X + 24) != 0
                                 && *reinterpret_cast<const UInt64*>(X + 56) != 0;  // ★ M-2
        const UInt64  sub      = hOk ? *reinterpret_cast<const UInt64*>(HG + 1712) : 0;
        //  R111/N-3：实流到达此处 ⇒ **`xOk` 失败**（`sub==0` 已被先决③以 `bit16` 拦下）。
        if (!xOk || sub == 0) {
            gGvmBook.failMask |= (1U << 18);                                       // ★ M-3：`bit18`（"未供给 ⇒ 6"）
            *reinterpret_cast<UInt64*>(O + 8) = 0;                                 // #25：失败一律清 `out+8`
            return kSwResStepFail;                                                 // 6
        }
    }
    // ── D2-3（R143）：`omittedMask` **登记（必须非空）**（R141 §1-13 ＋ 复核 M-2／N-2）──────────────
    //  ⚠ `omittedMask` 与 `failMask` 是**两个独立字段**（日志里分别印 `o=`／`f=`）⇒ 位号可同号不同义。
    gGvmBook.omittedMask |= (1U << 27);   // ★ 复核 M-2：`h+888` 置 0 ⇒ **4 项下游功能被关闭**
                                          //   （读者 `0x687a6`／`0x68daf`／`0x6fb27`／`0x74db6` 的 `cmpb $0,888(%rdi)`
                                          //    为 0 ⇒ 包装器空跑／返 0）。**苹果侧常态值未证**（其值来自框架填的
                                          //    `O+32/40/…`，复核 M-3）⇒ 与苹果的差异**不可离线判定**。
    gGvmBook.omittedMask |= (1U << 29);   // `h+712` **未写**（`0x66d84` 属已删的 `#20`）⇒ 本世代标识未提供
    gGvmBook.omittedMask |= (1U << 30);   // 槽 29／uCode 常量**未取得**；**且槽 1..29 整段 API 表未安装**
                                          //   （`h+1736`＝槽 1 为空 ⇒ 后续苹果 hw-init（`0x6463f`→`0x67423`）
                                          //    会**静默返 0**＝"成功但未做"；复核 N-2）
    gGvmBook.omittedMask |= (1U << 31);   // **未调用苹果 `#23`**（其职责按 Linux 由我方最小实现替代）；
                                          //   同时 `h+720` **未置位**＝**不宣告**"SMU 上下文已就绪"（R141 §3.2）
    // ── 成功出口门控（R108 §4 逐字）：门控关 ⇒ 只返 7（**不报成功**）──────────────────────────────
    //  ★ 返 0 的**含义收窄**：仅表示"**SMU 软件侧最小初始化（M1–M7）完成**"——**不等于"模块 8 完成"**，
    //    更不等于"SMU 就绪"（硬件侧 H2–H7 未做，见头注判据与 `omittedMask`）。
    if (!gGvmSwInitPass) { return kSwResPassGateOff; }
    return kSwResPass;                             // 0（`6463b: xorl %ebx,%ebx`）★ 唯一成功出口
}

// ── 模块 6（PSP）本体：**阶段 1**（R96 §1.2／R98 §4）──────────────────────────────────────────
//  被替换实现＝苹果 `0x48f9c`（【转引·R58 §1.3】）；其真实动作（R96 亲读）：
//   ① 形状校验：*(u32*)(info+0)==632 ∧ *(u32*)(info+8)==1 ∧ *(u64*)(info+616)!=0
//                ∧ *(u64*)(info+624)!=0 ∧ *(u32*)(out+0)==16
//   ② *(u64*)(out+8)=0（先清）
//   ③ 取/建 PSP 对象（`0x4beba`，id 0x7E50）④ 用 `info+624` 初始化（`0x4c024`）
//   ⑤ memcpy(obj+48, info+4, 608)   ⑥ 按版本 9..13 分派子步骤（`0x49580` 统一失败出口）
//  ⚠ **阶段 1 只做 ①②**（零新苹果调用）；③④⑤⑥ 与"`subobj+64` 句柄供给"**如实登记为省略**
//    ⇒ "全过"不成立 ⇒ **必然 fail-closed（返 6）**；**不得**为凑 `res=7` 而放宽（红线）。
static int nredPspSwInit(void* const info, void* const out)
{
    const UInt8* const I = static_cast<const UInt8*>(info);
    UInt8* const O = static_cast<UInt8*>(out);
    gGvmBook.failMask = 0; gGvmBook.omittedMask = 0;
    // ① 形状校验（逐条对齐苹果 `0x48f9c` 的 `48fe0..49008`）
    if (*reinterpret_cast<const UInt32*>(I + 0) != 632)      { gGvmBook.failMask |= (1U << 10); return kSwResParseFail; }
    if (*reinterpret_cast<const UInt32*>(I + 8) != 1)        { gGvmBook.failMask |= (1U << 10); return kSwResParseFail; }
    if (*reinterpret_cast<const UInt64*>(I + 616) == 0)      { gGvmBook.failMask |= (1U << 10); return kSwResParseFail; }
    if (*reinterpret_cast<const UInt64*>(I + 624) == 0)      { gGvmBook.failMask |= (1U << 10); return kSwResParseFail; }
    if (*reinterpret_cast<const UInt32*>(O + 0) != 16)       { gGvmBook.failMask |= (1U << 10); return kSwResParseFail; }
    // ② 与苹果同序：先清 out+8（`4900e`）
    *reinterpret_cast<UInt64*>(O + 8) = 0;
    // ③④⑤⑥ 与句柄供给：**本阶段如实省略**（不调用任何苹果函数；不做硬件动作）
    gGvmBook.omittedMask |= (1U << 10);   // 取/建 PSP 对象（0x4beba）
    gGvmBook.omittedMask |= (1U << 11);   // 用 info+624 初始化（0x4c024）
    gGvmBook.omittedMask |= (1U << 12);   // memcpy(obj+48, info+4, 608)
    gGvmBook.omittedMask |= (1U << 13);   // 版本 9..13 子步骤
    gGvmBook.failMask    |= (1U << 14);   // ★ 句柄/对象未供给 ⇒ **fail-closed（不得报成功）**
    return kSwResStepFail;                // 6
}

// ── 模块 7（GC）本体：**D2**（R154／R155／R158／R160＋R161 第二道门并入）────────────────────
//  被替换实现＝苹果 `0x7ce0`（装配点 `0x99851`）。D2 手法：保留 `#1–#39`／`#43–#55` 的真实动作，
//  消除**两道版本门**：① `#41 callq 0x8df8(h,code)` 及其判据 `#42` 删除（本机 code 必不在其支持集）；
//  ② `#40` 的 `0xa081` **不再调用**，只保留其两次无条件清零（`*(u32*)(O+16)=0`／`*(u32*)(h+832)=0`；
//     `a0a4 xorl` → `a0a6 movl %ebx,16(%r14)` → `a0aa movl %ebx,832(%r15)`，位于首个 code 条件 `a0b1` 之前），
//     版本分派／per-code 初始化／21 项表拷贝／`0xb794(h,7,0,0)` 通知整体跳过并登记（`omittedMask bit16/17`）。
//     本机 code＝`0x0B0001`（client-7 的 `I+12=0x0B`），苹果在该函数必返 2。
//  ★ 被删步骤的产物＝`h+3544..4031` 的 488 B 回调/表区 ⇒ 在**原位置**按唯一正确顺序处理：
//    显式清零 → 61 槽全零回读 → 写三个诚实拒绝桩（`h+3552/3560/3568`）→ 三槽回读比对。
//    **终态＝三槽为桩地址、其余 58 槽为 0**（"488 B 全零"只在**装桩前**那一刻成立，不得当终态）。
//  ★ 三个桩（R160 §4）：3568 返 0＝"无可用块"（消费者译为失败 2，**不是成功**）；3560/3552 返非 0
//    ⇒ 调用方走失败分支。桩只读 `rdi`、不解引用任何入参、不做硬件动作 ⇒ 不会 `callq *0` trap。
//  ⚠ 未复现项：`0x8df8` 的 59 槽苹果回调表未安装（`bit28`）；其默认失败分支的事件 `0xb794(h,8,0,0)`
//    随删点消失（`bit23`）；`0xa081` 的 per-code／表拷贝未做（`bit16`）、其通知未复现（`bit17`）；
//    GC 硬件侧未做（`bit24`）；`h+3880/4024` 来源未知（`bit14`）；句柄供给 `subobj+40` 未做（`bit22`）。
//  ⚠ 口径（R160 §0）：返 0 只表示"GC 软件侧最小初始化完成并越过两道版本门"，**不等于**模块 7 完成／GC 就绪。
static constexpr UInt32 kGcLocalBytes = 4032;
static UInt8 gGcLocalBuf[kGcLocalBytes];   // 苹果同位使用 4032 B 栈缓冲；改 static ⇒ 零栈占用
// 三个诚实拒绝桩：只读 `rdi`（本实现完全不读入参）、不做硬件动作；返回值使调用方走失败分支。
//  `gM7StubHit` 当前**无并发写者**（stage 链单线程串行）⇒ 普通 `|=` 即可；
//  若将来出现并发路径，改用 `__atomic_fetch_or(&gM7StubHit, mask, __ATOMIC_RELAXED)`。
static UInt64 nredGcStub3552(void* /*h*/, UInt64 /*src*/, UInt64 /*dst*/, UInt64 /*len*/)
{ gM7StubHit |= (1U << 0); return 0x80000301ULL; }   // 非 0 ⇒ `0x829e` 经 `0x834b` 返 2
static UInt64 nredGcStub3560(void* /*h*/)
{ gM7StubHit |= (1U << 1); return 0x80000302ULL; }   // 循环内：经 `0x834b` 返 2；尾跳：原样传出
static UInt64 nredGcStub3568(void* /*h*/)
{ gM7StubHit |= (1U << 2); return 0; }               // ★ 必须 0＝"无可用块"（非成功）⇒ `0x829e` 经 `0x82da` 返 2
static int nredGcSwInit(void* const info, void* const out)
{
    const UInt8* const I = static_cast<const UInt8*>(info);
    UInt8* const O = static_cast<UInt8*>(out);
    gGvmBook.failMask = 0; gGvmBook.omittedMask = 0;
    UInt8* const L = gGcLocalBuf;
    // 入口重置（R160 §5.3／R161 §3.2）：同一引导内重复进入时，`[swhw7]` 读数只反映本次。
    gM7N += 1;
    gM7Step = 0;
    gM7Code = 0xFFFFFFU; gM7O1048 = ~0ULL; gM7O1056 = 0xFFFFFFFFU;
    gM7H3520 = ~0ULL; gM7H3528 = 0xFFFFFFFFU; gM7StubHit = 0;
    gM7S3552 = ~0ULL; gM7S3560 = ~0ULL; gM7S3568 = ~0ULL;
    for (UInt32 i = 0; i < 5; i += 1) { gM7Tab[i] = ~0ULL; }

    __builtin_memset(L, 0, kGcLocalBytes);                                   // #2
    if (info == nullptr || out == nullptr) { gM7Step = 0x01; return kSwResNullArg; }   // #4/#5
    if (*reinterpret_cast<const UInt32*>(I + 0) != 648 ||
        *reinterpret_cast<const UInt32*>(O + 0) != 1064 ||
        *reinterpret_cast<const UInt32*>(I + 8) != 1 ||
        *reinterpret_cast<const UInt64*>(I + 616) == 0 ||
        *reinterpret_cast<const UInt64*>(I + 624) == 0) {                    // #7..#9
        gGvmBook.failMask |= (1U << 19); gM7Step = 0x02; return kSwResParseFail;   // 1
    }
    if (gHwlibsSlide == 0) {
        gGvmBook.failMask |= (1U << 22); gM7Step = 0x03; *reinterpret_cast<UInt64*>(O + 8) = 0; return kSwResStepFail;
    }
    *reinterpret_cast<UInt64*>(L + 16) = *reinterpret_cast<const UInt64*>(I + 624);   // #10
    *reinterpret_cast<UInt64*>(L + 8)  = *reinterpret_cast<const UInt64*>(I + 616);   // #11

    using NRedGcGetOrCreate = int (*)(void*, UInt32, UInt32, void**);
    using NRedGcFill        = int (*)(void*, const void*, UInt32);
    using NRedGcCode        = UInt32 (*)(const void*);
    using NRedGcStateInit   = int (*)(void*, UInt32);
    using NRedGcTriple      = int (*)(void*, UInt32);
    using NRedGcTail        = int (*)(void*, void*);
    using NRedGcStub        = int (*)(void*, UInt64);
    using NRedGcConst       = UInt32 (*)(void);
    const NRedGcGetOrCreate fnGetOrCreate = reinterpret_cast<NRedGcGetOrCreate>(gHwlibsSlide + 0x9dbf);
    const NRedGcFill        fnFill        = reinterpret_cast<NRedGcFill>(gHwlibsSlide + 0x9f3d);
    const NRedGcCode        fnCode        = reinterpret_cast<NRedGcCode>(gHwlibsSlide + 0xb129);
    const NRedGcStateInit   fnStateInit   = reinterpret_cast<NRedGcStateInit>(gHwlibsSlide + 0x9573);
    const NRedGcTriple      fnTriple      = reinterpret_cast<NRedGcTriple>(gHwlibsSlide + 0x9bf9);
    const NRedGcTail        fnTail        = reinterpret_cast<NRedGcTail>(gHwlibsSlide + 0x9801);
    const NRedGcStub        fnStub        = reinterpret_cast<NRedGcStub>(gHwlibsSlide + 0x9db1);
    const NRedGcConst       fnConst       = reinterpret_cast<NRedGcConst>(gHwlibsSlide + 0xc312);

    void* h = nullptr;
    const int rc12 = fnGetOrCreate(L, kGcLocalBytes, 1, &h);                 // #12
    if (rc12 != 0) {                                                         // #13
        gGvmBook.failMask |= (1U << 22); gM7Step = 0x05; *reinterpret_cast<UInt64*>(O + 8) = 0; return rc12;
    }
    if (!nredKernelPtrOk(h)) {
        gGvmBook.failMask |= (1U << 22); gM7Step = 0x04; *reinterpret_cast<UInt64*>(O + 8) = 0; return kSwResStepFail;
    }
    UInt8* const H = static_cast<UInt8*>(h);
    fnFill(L, h, kGcLocalBytes);                                             // #15（不判）
    *reinterpret_cast<UInt64*>(O + 8) = reinterpret_cast<UInt64>(h);         // #16
    *reinterpret_cast<UInt64*>(H + 16) = *reinterpret_cast<const UInt64*>(I + 624);   // #17
    *reinterpret_cast<UInt64*>(H + 8)  = *reinterpret_cast<const UInt64*>(I + 616);   // #18
    __builtin_memcpy(H + 192, I + 4, 608);                                   // #19

    const UInt32 code = fnCode(I + 12);                                      // #20
    gM7Code = code;
    fnStateInit(h, code);                                                    // #22（不判）
    fnTriple(h, code);                                                       // #23（不判）

    const UInt32 nSub = *reinterpret_cast<const UInt32*>(H + 3432);          // #24
    if (nSub != 0) {
        const int rc25 = fnGetOrCreate(L, nSub, 1, reinterpret_cast<void**>(H + 3424));  // #25
        if (rc25 != 0) {
            // ⚠ 比苹果严格：苹果此处**不清** `O+8`（`7e1b jne 0x7d76` 直接上抛），我方清（R160 §2 #26）。
            gGvmBook.failMask |= (1U << 22); gM7Step = 0x06; *reinterpret_cast<UInt64*>(O + 8) = 0; return rc25;
        }
    }
    // #27/#28：⚠ 与 R160 §2 #24「n==0 跳过 25–28」的措辞不同——机器码在 n==0 时只跳过 #25/#26，
    //  随后 `7e2d xorl %edx,%edx` 落到 `7e2f..7e3d` 仍调用 `0x9f3d(L, *(h+3424), 0)`（src 为 0 时
    //  该函数在 `9f59/9f5c` 直接返 2，不解引用）⇒ 此处按机器码**无条件调用**；计数在 #25 之后回读
    //  （`7e21 movl 3432(%rcx),%edx`），不使用 #24 的旧值。
    fnFill(L, *reinterpret_cast<const void* const*>(H + 3424),
           *reinterpret_cast<const UInt32*>(H + 3432));                       // #28（返回值不判）
    *reinterpret_cast<UInt64*>(O + 1048) = *reinterpret_cast<const UInt64*>(H + 3424);  // #29
    *reinterpret_cast<UInt32*>(O + 1056) = *reinterpret_cast<const UInt32*>(H + 3432);  // #30
    gM7O1048 = *reinterpret_cast<const UInt64*>(O + 1048);
    gM7O1056 = *reinterpret_cast<const UInt32*>(O + 1056);
    *(H + 3420) = 0;                                                         // #31
    {
        const UInt32 v = *reinterpret_cast<const UInt32*>(I + 632);          // #32..#39
        *(H + 806) = static_cast<UInt8>(v & 1U);
        *(H + 807) = static_cast<UInt8>((v >> 1) & 1U);
        *(H + 808) = static_cast<UInt8>((v >> 2) & 1U);
        UInt32 w = *reinterpret_cast<const UInt32*>(H + 3444);
        w = (w & ~2U)  | ((v >> 2) & 2U);
        w = (w & ~8U)  | ((v >> 2) & 8U);
        w = (w & ~4U)  | ((v >> 2) & 4U);
        w = (w & ~16U) | ((v >> 2) & 16U);
        w = (w & ~32U) | ((v >> 2) & 32U);
        *reinterpret_cast<UInt32*>(H + 3444) = w;
    }
    // #40（D2 化，R161）：苹果 `0xa081` 的两次**无条件清零**（`a0a4 xorl %ebx` → `a0a6 movl %ebx,16(%r14)`
    //  → `a0aa movl %ebx,832(%r15)`，位于首个 code 条件 `a0b1` 之前）——真实动作，保留。
    //  版本分派（A8–A17：9 值支持集／per-code／21 项表拷贝／`0xb794(h,7,0,0)`）整体跳过：
    //  本机 code=0x0B0001 不在支持集 ⇒ 苹果必返 2。该函数不再被调用（R161 §1.2）。
    *reinterpret_cast<UInt32*>(O + 16) = 0;
    *reinterpret_cast<UInt32*>(H + 832) = 0;
    gGvmBook.omittedMask |= (1U << 16);   // a081 的 per-code 初始化与 21 项表拷贝未执行
    gGvmBook.omittedMask |= (1U << 17);   // a081 的 0xb794(h,7,0,0) 事件通知未复现
    // ⚠ `failMask bit21` 自 R161 起**仅**用于 M1／#44／#54／终态回读（#40 已 D2 化、不再失败）。
    // ── M1：删 #41/#42 后，原位置清零＋装桩（唯一正确顺序：memset→全零回读→写桩→回读桩）──────
    __builtin_memset(H + 3544, 0, 488);
    for (UInt32 off = 3544; off <= 4024; off += 8) {
        if (*reinterpret_cast<const UInt64*>(H + off) != 0) {
            gGvmBook.failMask |= (1U << 21); gM7Step = 0x07; *reinterpret_cast<UInt64*>(O + 8) = 0; return kSwResStepFail;
        }
    }
    *reinterpret_cast<UInt64*>(H + 3552) = reinterpret_cast<UInt64>(&nredGcStub3552);
    *reinterpret_cast<UInt64*>(H + 3560) = reinterpret_cast<UInt64>(&nredGcStub3560);
    *reinterpret_cast<UInt64*>(H + 3568) = reinterpret_cast<UInt64>(&nredGcStub3568);
    // 终态判据：三槽 == 桩地址（其余 58 槽由上面的全零回读保证；#43–#50 不再写回调槽）。
    if (*reinterpret_cast<const UInt64*>(H + 3552) != reinterpret_cast<UInt64>(&nredGcStub3552) ||
        *reinterpret_cast<const UInt64*>(H + 3560) != reinterpret_cast<UInt64>(&nredGcStub3560) ||
        *reinterpret_cast<const UInt64*>(H + 3568) != reinterpret_cast<UInt64>(&nredGcStub3568)) {
        gGvmBook.failMask |= (1U << 21); gM7Step = 0x08; *reinterpret_cast<UInt64*>(O + 8) = 0; return kSwResStepFail;
    }
    gM7S3552 = *reinterpret_cast<const UInt64*>(H + 3552);
    gM7S3560 = *reinterpret_cast<const UInt64*>(H + 3560);
    gM7S3568 = *reinterpret_cast<const UInt64*>(H + 3568);
    gGvmBook.omittedMask |= (1U << 15);   // 三回调未实现（拒绝桩）
    gGvmBook.omittedMask |= (1U << 28);   // 59 槽苹果回调表未安装
    gGvmBook.omittedMask |= (1U << 23);   // stage-2 的"22 处回调调用"未执行（注意：`0x8a85` 的 3 条**直接**
                                          //   调用 `0xb794`／`0xb5ec`／`0xb1c1` 仍会执行，它们不是回调，不在本位范围）
    gGvmBook.omittedMask |= (1U << 24);   // GC 硬件侧未做
    gGvmBook.omittedMask |= (1U << 14);   // `h+3880/4024` 来源未知
    gGvmBook.omittedMask |= (1U << 22);   // 句柄供给（`subobj+40`）未做
    // #43..#50（D2 下：3544/3600/3608/3624/4000 均为 0 ⇒ 不调用；只有 3568 槽是桩）
    if (*reinterpret_cast<const UInt64*>(H + 3544) != 0) {                    // 3544=0 ⇒ 跳过
        reinterpret_cast<void (*)(void*)>(*reinterpret_cast<const UInt64*>(H + 3544))(h);
    }
    const int rc44 = fnTail(h, O + 1032);                                    // #44
    if (rc44 != 0 && rc44 != 3) {
        gGvmBook.failMask |= (1U << 21); gM7Step = 0x09; *reinterpret_cast<UInt64*>(O + 8) = 0; return rc44;
    }
    if (*reinterpret_cast<const UInt64*>(H + 3608) != 0) {
        reinterpret_cast<void (*)(void*)>(*reinterpret_cast<const UInt64*>(H + 3608))(h);
    }
    if (*(H + 804) != 0 || *(H + 802) != 0) {                                // #45/#46
        if (*reinterpret_cast<const UInt64*>(H + 3600) != 0) {                // #47
            reinterpret_cast<void (*)(void*)>(*reinterpret_cast<const UInt64*>(H + 3600))(h);
        }
        if (*(H + 804) != 0 && *reinterpret_cast<const UInt64*>(H + 3624) != 0) {  // #48
            reinterpret_cast<void (*)(void*)>(*reinterpret_cast<const UInt64*>(H + 3624))(h);
        }
    }
    if (!(((*reinterpret_cast<const UInt32*>(H + 3444)) & 2U) != 0 && *(H + 820) != 0)) {  // #49
        if (*reinterpret_cast<const UInt64*>(H + 4000) != 0) {                // #50
            reinterpret_cast<void (*)(void*)>(*reinterpret_cast<const UInt64*>(H + 4000))(h);
        }
    }
    (void)fnStub(h, *reinterpret_cast<const UInt64*>(I + 640));              // #51（恒 0 ⇒ 永不失败）
    const UInt32 k = fnConst();                                              // #52
    *reinterpret_cast<UInt32*>(H + 3528) = k;                                // #53
    gM7H3528 = *reinterpret_cast<const UInt32*>(H + 3528);   // 机械回读（§6.1 S5 要求读字段，不硬编码 0x4140）
    const int rc54 = fnGetOrCreate(h, k, 1, reinterpret_cast<void**>(H + 3520));  // #54
    gM7H3520 = *reinterpret_cast<const UInt64*>(H + 3520);
    if (rc54 != 0) { gGvmBook.failMask |= (1U << 21); gM7Step = 0x0A; *reinterpret_cast<UInt64*>(O + 8) = 0; return rc54; }
    if (gM7H3520 == 0 || gM7H3528 != k || *reinterpret_cast<const UInt64*>(O + 8) == 0) {
        gGvmBook.failMask |= (1U << 21); gM7Step = 0x0B; *reinterpret_cast<UInt64*>(O + 8) = 0; return kSwResStepFail;
    }
    gM7Tab[0] = *reinterpret_cast<const UInt64*>(H + 3544);
    gM7Tab[1] = *reinterpret_cast<const UInt64*>(H + 3784);
    gM7Tab[2] = *reinterpret_cast<const UInt64*>(H + 3944);
    gM7Tab[3] = *reinterpret_cast<const UInt64*>(H + 4000);
    gM7Tab[4] = *reinterpret_cast<const UInt64*>(H + 4024);
    if (!gGvmSwInitPass) { gM7Step = 0x0C; return kSwResPassGateOff; }       // #55（门关 7）
    return kSwResPass;                                                       // 门开 0
}

// ── 模块 9（VCN）本体：**D2 式实实现**（R173／R174／R175）────────────────────────────────────────
//  被替换实现＝client-9 的 `funcs[0]`＝`0x797e8`（VCN sw_init）。保留：形状/指针门、B1 0x40 分配
//  （`0x79b6d` 逐字入参）、B2 0x40 初始化（`0x79ca0`）、`out+8`、`local_28` 的 +0/+8/+0x2c 字段；
//  观测 6 个接口指针＋slide。
//  跳过：B7 `0x7a51b`（0x440 上下文）、B9 `0x7b4d8`（0x68）、B10 `0x7d120`（0x30）、B12 `0x7a363`
//  （它读 `local_28[2]`，B7 跳过后为空 ⇒ 调用即崩）；`out+0x50`（主体成功标记）不写。
//  不复制 `0x7a1a5` 的九项检查（R174：替换后不再执行，且其中两处取用在值为 0 时会崩）。
//  ⚠ 残余风险：B1 成功后若 B2/回读失败，0x40 对象**无法释放**（`0x79bf5` 未读/未授权）⇒ 如实返 6 并
//    登记"泄漏一次"；不得伪造释放。
//  ⚠ 机器码修正（相对 R175 §9.1）：① `*(device+8)`（devSelf）也必须过 `nredKernelPtrOk`（§3.3 要求，
//    §9.1 漏写）；② B3–B6 在 B2 调用之后、判 B2 返回值之前执行（`797e8.c:16-20` 的真实顺序）；
//    ③ S2 除 `out+8 != 0` 外还须过 `nredKernelPtrOk`（R175 §5.1 S2）。
//  ⚠ 口径：只可写"VCN 软件侧最小初始化完成（仅 0x40 上下文；不含 0x440/0x68/0x30 子对象）"。
static int nredVcnSwInit(void* const info, void* const out)
{
    gGvmBook.failMask = 0; gGvmBook.omittedMask = 0;
    gVcnN += 1; gVcnStep = 0; gVcnO8 = ~0ULL;
    gVcnL10 = ~0ULL; gVcnL18 = ~0ULL; gVcnL30 = ~0ULL;
    if (info == nullptr || out == nullptr) {                                  // A2
        gGvmBook.failMask |= (1U << 9); gVcnStep = 0x01; return kSwResNullArg;   // 2
    }
    if (!nredKernelPtrOk(info) || !nredKernelPtrOk(out)) {                    // A3
        gGvmBook.failMask |= (1U << 9); gVcnStep = 0x02; return kSwResParseFail; // 1
    }
    const UInt8* const I = static_cast<const UInt8*>(info);
    UInt8* const O = static_cast<UInt8*>(out);
    const UInt64 dev  = *reinterpret_cast<const UInt64*>(I + 0x10);           // device
    const UInt64 devB = *reinterpret_cast<const UInt64*>(I + 8);              // device+304
    if (!nredKernelPtrOk(reinterpret_cast<const void*>(dev))) {
        gGvmBook.failMask |= (1U << 9); gVcnStep = 0x02; return kSwResParseFail;
    }
    // 观测：6 接口指针（与旧探针一致；B 侧仅当 devB 合法时读）
    gProbe9[0] = *reinterpret_cast<const UInt64*>(dev + 16);
    gProbe9[1] = *reinterpret_cast<const UInt64*>(dev + 24);
    const bool bOk = nredKernelPtrOk(reinterpret_cast<const void*>(devB));
    gProbe9[2] = bOk ? *reinterpret_cast<const UInt64*>(devB + 16) : ~0ULL;
    gProbe9[3] = bOk ? *reinterpret_cast<const UInt64*>(devB + 32) : ~0ULL;
    gProbe9[4] = bOk ? *reinterpret_cast<const UInt64*>(devB + 56) : ~0ULL;
    gProbe9[5] = bOk ? *reinterpret_cast<const UInt64*>(devB + 64) : ~0ULL;
    gProbe9N += 1;

    using NRedVcnAlloc = int (*)(UInt64, void*, void**);   // *(dev+0x10)(*(dev+8), desc, &p)
    using NRedVcnInit  = int (*)(UInt64, void*);           // *(dev+0x48)(*(dev+8), arg)
    const UInt64 devSelf = *reinterpret_cast<const UInt64*>(dev + 8);
    const UInt64 fnAlloc = *reinterpret_cast<const UInt64*>(dev + 0x10);
    const UInt64 fnInit  = *reinterpret_cast<const UInt64*>(dev + 0x48);
    if (!nredKernelPtrOk(reinterpret_cast<const void*>(devSelf)) ||
        !nredKernelPtrOk(reinterpret_cast<const void*>(fnAlloc)) ||
        !nredKernelPtrOk(reinterpret_cast<const void*>(fnInit))) {
        gGvmBook.failMask |= (1U << 9); gVcnStep = 0x02; return kSwResParseFail;
    }
    // B1：分配 0x40（0x79b6d 的入参 {kind=1, 9, size+8=0x48}）
    struct NRedVcnDesc { UInt64 kind; UInt64 nine; UInt64 size; } desc;
    desc.kind = 1; desc.nine = 9; desc.size = 0x40 + 8;
    UInt64 local28 = 0;
    const int rc1 = reinterpret_cast<NRedVcnAlloc>(fnAlloc)(devSelf, &desc,
                                                            reinterpret_cast<void**>(&local28));
    if (rc1 != 0 || !nredKernelPtrOk(reinterpret_cast<const void*>(local28))) {
        gGvmBook.failMask |= (1U << 8); gVcnStep = 0x03; return kSwResStepFail;   // 6
    }
    // B2：初始化 0x40（0x79ca0 的入参 {p, size=0x40}）
    struct NRedVcnInitArg { UInt64 p; UInt32 size; UInt32 pad; } iarg;
    iarg.p = local28; iarg.size = 0x40; iarg.pad = 0;
    const int rc2 = reinterpret_cast<NRedVcnInit>(fnInit)(devSelf, &iarg);
    UInt8* const L = reinterpret_cast<UInt8*>(local28);
    *reinterpret_cast<UInt64*>(O + 8) = local28;                              // B3（分配成功后立即写）
    *reinterpret_cast<UInt64*>(L + 8)  = dev;                                 // B4
    *reinterpret_cast<UInt64*>(L + 0)  = devB;                                // B5
    *(L + 0x2C) = static_cast<UInt8>((*reinterpret_cast<const UInt8*>(I + 4) >> 1) & 1U);  // B6
    if (rc2 != 0) {
        // ⚠ 0x40 泄漏一次：0x79bf5 不在授权清单内 ⇒ 如实返 6，不伪造释放。
        gGvmBook.failMask |= (1U << 8); gVcnStep = 0x04; return kSwResStepFail;
    }
    // B7/B9/B10 跳过：清零其产物槽并回读（B12 的 0x28 同）
    *reinterpret_cast<UInt64*>(L + 0x10) = 0;
    *reinterpret_cast<UInt64*>(L + 0x18) = 0;
    *reinterpret_cast<UInt32*>(L + 0x28) = 0;
    *reinterpret_cast<UInt64*>(L + 0x30) = 0;
    if (*reinterpret_cast<const UInt64*>(L + 0x10) != 0 ||
        *reinterpret_cast<const UInt64*>(L + 0x18) != 0 ||
        *reinterpret_cast<const UInt32*>(L + 0x28) != 0 ||
        *reinterpret_cast<const UInt64*>(L + 0x30) != 0) {
        // ⚠ 同上：0x40 泄漏一次，如实返 6。
        gGvmBook.failMask |= (1U << 8); gVcnStep = 0x05; return kSwResStepFail;   // 6
    }
    gGvmBook.omittedMask |= (1U << 19);   // 0x7a51b（0x440）未执行
    gGvmBook.omittedMask |= (1U << 20);   // 0x7b4d8（0x68）/0x7d120（0x30）未创建
    gGvmBook.omittedMask |= (1U << 21);   // out+0x50（＝4）未写
    gVcnO8 = *reinterpret_cast<const UInt64*>(O + 8);
    gVcnL10 = *reinterpret_cast<const UInt64*>(L + 0x10);
    gVcnL18 = *reinterpret_cast<const UInt64*>(L + 0x18);
    gVcnL30 = *reinterpret_cast<const UInt64*>(L + 0x30);
    if (gVcnO8 == 0 || !nredKernelPtrOk(reinterpret_cast<const void*>(gVcnO8))) {   // S2
        gGvmBook.failMask |= (1U << 8); gVcnStep = 0x06; return kSwResStepFail;
    }
    return kSwResPass;                                                        // 0
}

// ── 模块 10（DMCU）S1：fail-closed 最小实现（R181）────────────────────────────────────────────────
//  为什么只做形状：`0x4d00` 的真实动作依赖 `0x51ae`/`0x7365`/`0x7977`（未知原语）与固件头解析
//  （含 M9 trap：指针置 0 后无判空读 +6）。S1 **不调用任何外部函数**、不分配、不写 out、不置就绪位。
//  苹果前置（`0x9f6dd:70–99`）已在我方之前完成：`*p1=0x2B8`、`*p2=0x78`、资源表项 10 判空。
//  位：failMask bit2＝入参/指针门/形状；bit3＝S1 主动省略（如实失败）。omittedMask 本轮 32 位已满，
//    采用 R181 P1：**不占 omitted 位**，模块 10 的省略集见 failMask bit3（待 u64 扩容后补登记）。
//  返回值：0=成功（S1 **永不**返回）；6=如实失败（fail-closed）。
static int nredDmcuSwInit(void* const p1, void* const p2)
{
    gGvmBook.failMask = 0; gGvmBook.omittedMask = 0;
    gDmcuN += 1; gDmcuStep = 0; gDmcuF0 = 0; gDmcuF280 = 0;
    if (p1 == nullptr || p2 == nullptr) {                                     // A1
        gGvmBook.failMask |= (1U << 2); gDmcuStep = 0x01; return kSwResNullArg;    // 2
    }
    if (!nredKernelPtrOk(p1) || !nredKernelPtrOk(p2)) {                        // A2
        gGvmBook.failMask |= (1U << 2); gDmcuStep = 0x02; return kSwResParseFail;  // 5
    }
    const UInt32 f0 = *reinterpret_cast<const UInt32*>(p1);
    gDmcuF0 = f0;
    gDmcuF280 = *reinterpret_cast<const UInt64*>(static_cast<const UInt8*>(p1) + 0x280);
    if (f0 != 0x2B8) {                                                        // A3
        gGvmBook.failMask |= (1U << 2); gDmcuStep = 0x03; return kSwResParseFail;  // 5
    }
    if (*reinterpret_cast<const UInt32*>(p2) != 0x78) {                        // A4
        gGvmBook.failMask |= (1U << 2); gDmcuStep = 0x04; return kSwResParseFail;  // 5
    }
    gGvmBook.failMask |= (1U << 3);                                           // A5：主动省略
    gDmcuStep = 0x05;
    return kSwResStepFail;                                                     // 6
}

// ── 模块 15（JPEG）**只读探针**（R194；替换 R177 的实实现）────────────────────────────────────────
//  目的：只取 `dev_A` 与其两个运行时函数指针槽（fa/fi）的地址，供离线归属；**不调用任何设备原语**、
//  不分配、不写对象、不写 out、不置就绪位。
//  ⚠ 形状常量以机器码为准：`0x3fc39` 的判据是 `*p1==0x278`、`*p2==0x18`（`3fc39.c:30`；`0xa1541`
//    写 local_2a8=0x278 / local_30 低 32=0x18）。任务卡写的 0x2B8/0x78 属模块 10（`0x4d00`），
//    照抄会让探针在 A3 就返回、永远读不到 fa/fi。
//  ⚠ 返回值：恒返 6（fail-closed）；苹果 `0xa1541` 见非 0 ⇒ 返回失败（断言型），**不进入 0x3fc39**。
//  ⚠ 所有解引用前过 nredKernelPtrOk；非法值记哨兵。st：05 指针非法／06 读取完成（均返 6）。
static int nredJpegSwInit(void* const p1, void* const p2)
{
    gGvmBook.failMask = 0; gGvmBook.omittedMask = 0;
    gJpegN += 1; gJpegStep = 0;
    gJpegDev = ~0ULL; gJpegSelf = ~0ULL; gJpegFa = ~0ULL; gJpegFi = ~0ULL;
    if (p1 == nullptr || p2 == nullptr) {                                       // A1
        gGvmBook.failMask |= (1U << 7); gJpegStep = 0x01; return kSwResNullArg;   // 2
    }
    if (!nredKernelPtrOk(p1) || !nredKernelPtrOk(p2)) {                          // A2
        gGvmBook.failMask |= (1U << 7); gJpegStep = 0x02; return kSwResParseFail; // 5
    }
    const UInt8* const P = static_cast<const UInt8*>(p1);
    if (*reinterpret_cast<const UInt32*>(P + 0) != 0x278) {                      // A3
        gGvmBook.failMask |= (1U << 7); gJpegStep = 0x03; return kSwResParseFail; // 5
    }
    if (*reinterpret_cast<const UInt32*>(p2) != 0x18) {                          // A4
        gGvmBook.failMask |= (1U << 7); gJpegStep = 0x04; return kSwResParseFail; // 5
    }
    // dev_A＝discovery 第 2 条记录（p1+0x270）；非法则记哨兵并返 6。
    const UInt64 devA = *reinterpret_cast<const UInt64*>(P + 0x270);
    if (!nredKernelPtrOk(reinterpret_cast<const void*>(devA))) {
        gGvmBook.failMask |= (1U << 17); gJpegStep = 0x05; return kSwResStepFail; // 6
    }
    gJpegDev = devA;
    // 两个原语槽＋devSelf：读后逐一过门；非法值保持哨兵（不调用、不写对象）。
    const UInt64 selfRaw = *reinterpret_cast<const UInt64*>(devA + 0x08);
    const UInt64 faRaw   = *reinterpret_cast<const UInt64*>(devA + 0x10);
    const UInt64 fiRaw   = *reinterpret_cast<const UInt64*>(devA + 0x48);
    const bool selfOk = nredKernelPtrOk(reinterpret_cast<const void*>(selfRaw));
    const bool faOk   = nredKernelPtrOk(reinterpret_cast<const void*>(faRaw));
    const bool fiOk   = nredKernelPtrOk(reinterpret_cast<const void*>(fiRaw));
    if (selfOk) { gJpegSelf = selfRaw; }
    if (faOk)   { gJpegFa = faRaw; }
    if (fiOk)   { gJpegFi = fiRaw; }
    if (!selfOk || !faOk || !fiOk) {
        gGvmBook.failMask |= (1U << 17); gJpegStep = 0x05; return kSwResStepFail; // 6
    }
    gGvmBook.failMask |= (1U << 17);   // 读取完成但**主动 fail-closed**（不伪造成功）
    gJpegStep = 0x06;
    return kSwResStepFail;   // 6（探针永不报成功）
}

// ── 模块 2（SDMA）本体：**D2 式替换**（R169）────────────────────────────────────────────────────
//  被替换实现＝client-2 的 `funcs[0]`＝`0x58a04`（per-instance `sdma_sw_init`，`58a04..58c4c`）。
//  保留的真实动作：栈工作区清零／形状校验／0x348 对象分配＋初始化／`p2+8=obj`／字段搬运／`memcpy 0x260`／
//  三个状态位／键查询（0x59904）／门铃需求（0x5950d）／常量与日志缓冲（0x5b11c＋0x58edf）／
//  queue 对象分配＋初始化（0x5a28d 的前两步）。
//  删除的两道门：① A13 `0x595f8`（引擎类型固定集合＋`return 2`）不调用；其**首个动作**（把 `obj+0x2c8`
//  起 0x80 B 清零，`5961d movl $128,%edx`）由我方显式执行并逐 8 字节回读 16 槽——`0x80` 才覆盖
//  `obj+0x340` 回调槽（R169 的"0x78 B／15 槽"与机器码不符）；② A19 `0x5a28d` 的 `0x595eb` 类型判定与
//  失败释放不调用，只内联其"分配＋初始化"两步。
//  ⚠ 口径：只可写"SDMA 软件侧最小初始化完成（不含队列/环的引擎配置）"，不得写"模块 2 完成／SDMA 就绪"。
//  ⚠ 返回值（`funcs[0]` 层）：**0=成功**；成功前必须通过 S1–S6（`failMask==0`、`p2+8==obj`、
//    `obj+0x2c8..0x347` 全 0、`obj+0x2a8!=0`、`omittedMask ⊇ {0,1,2,9,18}`、返 0）。不设就绪标志。
//  ⚠ 安装器保证 `gHwlibsSlide != 0`（形态校验含该条件）⇒ 下方 slide+偏移解析不会落到低地址。
static constexpr UInt32 kSdmaLocalBytes = 0x348;
static UInt8 gSdmaLocal[kSdmaLocalBytes];   // 苹果同位的 0x348 B 栈工作区；改 static ⇒ 零栈占用（串行单写者）
static int nredSdmaSwInit(void* const p1, void* const p2)
{
    gGvmBook.failMask = 0; gGvmBook.omittedMask = 0;
    gM2N += 1;
    gM2Step = 0;
    gM2K = 0xFFFFFFFFU;
    gM2Cfg = 0xFFFFFFFFFFFFFFFFULL;
    gM2Q = 0xFFFFFFFFFFFFFFFFULL;
    gGvmBook.omittedMask |= (1U << 2);   // SDMA 引擎/队列功能未完成（D2 最小实现；沿用探针位语义）
    __builtin_memset(gSdmaLocal, 0, kSdmaLocalBytes);                        // A1
    if (p1 == nullptr || p2 == nullptr) {                                    // A2
        gGvmBook.failMask |= (1U << 13); gM2Step = 0x01; return kSwResNullArg;   // 2
    }
    const UInt8* const P1 = static_cast<const UInt8*>(p1);
    UInt8* const P2 = static_cast<UInt8*>(p2);
    if (!nredKernelPtrOk(p1) || !nredKernelPtrOk(p2) ||                       // A3（解引用前置门）
        *reinterpret_cast<const UInt32*>(P1 + 0x00) != 0x290 ||
        *reinterpret_cast<const UInt32*>(P2 + 0x00) != 0x40  ||
        *reinterpret_cast<const UInt32*>(P1 + 0x0C) != 1) {
        gGvmBook.failMask |= (1U << 13); gM2Step = 0x02; return kSwResParseFail;   // 1
    }
    gM2K = *reinterpret_cast<const UInt32*>(P1 + 0x08);   // 匹配的 block type（`0x9aaeb` 的 `local_340`）
    // A4：`58a80/58a8b` 把两个服务对象指针写进 0x348 B 工作区（local+0x10／local+8），供 0x58edf/0x58fad 使用。
    *reinterpret_cast<UInt64*>(gSdmaLocal + 0x10) = *reinterpret_cast<const UInt64*>(P1 + 0x270);
    *reinterpret_cast<UInt64*>(gSdmaLocal + 0x08) = *reinterpret_cast<const UInt64*>(P1 + 0x278);

    using NRedSdmaAlloc    = int (*)(void*, UInt64, UInt32, void**);
    using NRedSdmaInit     = int (*)(void*, void*, UInt32);
    using NRedSdmaKeys     = void (*)(void*);
    using NRedSdmaDoorbell = int (*)(void*, void*);
    using NRedSdmaReq      = UInt64 (*)(void*, void*);
    using NRedSdmaConst    = UInt32 (*)(void);
    using NRedSdmaFailSeq  = UInt64 (*)(void*, UInt32);
    const NRedSdmaAlloc    fnAlloc    = reinterpret_cast<NRedSdmaAlloc>(gHwlibsSlide + 0x58edf);
    const NRedSdmaInit     fnInit     = reinterpret_cast<NRedSdmaInit>(gHwlibsSlide + 0x58fad);
    const NRedSdmaKeys     fnKeys     = reinterpret_cast<NRedSdmaKeys>(gHwlibsSlide + 0x59904);
    const NRedSdmaDoorbell fnReq      = reinterpret_cast<NRedSdmaDoorbell>(gHwlibsSlide + 0x5950d);
    const NRedSdmaReq      fnDbellCnt = reinterpret_cast<NRedSdmaReq>(gHwlibsSlide + 0x59ee1);
    const NRedSdmaConst    fnConst    = reinterpret_cast<NRedSdmaConst>(gHwlibsSlide + 0x5b11c);
    const NRedSdmaFailSeq  fnFailSeq  = reinterpret_cast<NRedSdmaFailSeq>(gHwlibsSlide + 0x5b066);

    UInt64 objU = 0;
    const int rc5 = fnAlloc(gSdmaLocal, kSdmaLocalBytes, 1, reinterpret_cast<void**>(&objU));   // A5
    if (rc5 != 0) {
        *reinterpret_cast<UInt64*>(P2 + 8) = 0;                              // 苹果：p2[2]/p2[3]=0
        gGvmBook.failMask |= (1U << 20); gM2Step = 0x03; return rc5;
    }
    UInt8* const obj = reinterpret_cast<UInt8*>(objU);
    if (!nredKernelPtrOk(obj)) {                                             // 解引用前置门（0x58fad 将解引用）
        gGvmBook.failMask |= (1U << 20); gM2Step = 0x03; return kSwResStepFail;   // 6
    }
    const int rc6 = fnInit(gSdmaLocal, obj, kSdmaLocalBytes);                 // A6（苹果不判；我方判）
    if (rc6 != 0) { gGvmBook.failMask |= (1U << 20); gM2Step = 0x04; return rc6; }

    *reinterpret_cast<UInt64*>(P2 + 8) = objU;                               // A7
    *reinterpret_cast<UInt64*>(obj + 0x10) = *reinterpret_cast<const UInt64*>(P1 + 0x270);   // A8
    *reinterpret_cast<UInt64*>(obj + 0x08) = *reinterpret_cast<const UInt64*>(P1 + 0x278);   // A8
    *reinterpret_cast<UInt32*>(obj + 0x278) = *reinterpret_cast<const UInt32*>(P1 + 0x268);  // A9
    __builtin_memcpy(obj + 0x18, P1 + 0x08, 0x260);                          // A10
    const UInt32 engType = *reinterpret_cast<const UInt32*>(P1 + 0x10);      // A11
    const UInt32 engId   = *reinterpret_cast<const UInt32*>(P1 + 0x14);
    const UInt32 nQueues = *reinterpret_cast<const UInt32*>(P1 + 0x18);      // A11（原为被删门的输入）
    (void)nQueues;                                                           // 门已删 ⇒ 仅保留苹果的同宽读取
    {
        const UInt32 b = *reinterpret_cast<const UInt8*>(P1 + 0x280);        // A12（苹果按字节读）
        *(obj + 0x2B1) = static_cast<UInt8>(b & 1U);
        *(obj + 0x2B2) = static_cast<UInt8>((b >> 1) & 1U);
        *(obj + 0x2B3) = static_cast<UInt8>((b >> 3) & 1U);
    }
    // A13（D2）：不调用 0x595f8；执行它的**首个动作**——`obj+0x2c8` 起 0x80 B 清零并逐 8 字节回读 16 槽。
    __builtin_memset(obj + 0x2C8, 0, 0x80);
    for (UInt32 off = 0; off < 0x80; off += 8) {
        if (*reinterpret_cast<const UInt64*>(obj + 0x2C8 + off) != 0) {
            gGvmBook.failMask |= (1U << 20); gM2Step = 0x05; return kSwResStepFail;   // 6
        }
    }
    gGvmBook.omittedMask |= (1U << 0);   // 引擎配置函数指针组（obj+0x2c8..0x347）未安装
    gGvmBook.omittedMask |= (1U << 9);   // 引擎类型门（0x595f8）被跳过
    fnKeys(obj);                                                             // A14（0x59904）
    int rc15 = 0;
    if (*(obj + 0x2B1) == 0 && *(obj + 0x2B2) == 0) {                        // A15
        rc15 = fnReq(obj, P2 + 0x10);                                        // 0x5950d（obj+0x2c8==0 ⇒ 直接返 0）
    } else {
        *(P2 + 0x10) = 0;
        rc15 = 0;
    }
    gGvmBook.omittedMask |= (1U << 1);   // 门铃需求未取得（0x5950d 因 obj+0x2c8==0 自然跳过）
    if (*reinterpret_cast<const UInt64*>(obj + 0x340) != 0) {                 // A16（清零后为 0 ⇒ 不调）
        reinterpret_cast<void (*)(void)>(*reinterpret_cast<const UInt64*>(obj + 0x340))();
    }
    if (rc15 == 0) {                                                         // A17
        const UInt32 v = fnConst();                                          // 0x5b11c → 0x170
        *reinterpret_cast<UInt32*>(obj + 0x2C0) = v;
        const int rc17 = fnAlloc(obj, v, 1, reinterpret_cast<void**>(obj + 0x2B8));
        if (rc17 != 0) { gGvmBook.failMask |= (1U << 20); gM2Step = 0x06; return rc17; }
    }
    if (engType == 4 && engId == 0) {                                        // A18
        *reinterpret_cast<UInt32*>(P2 + 0x38) = 0;
    } else {
        const UInt64 rc18 = fnDbellCnt(obj, P2 + 0x38);
        // `rc18==1` 时苹果调用 0x5b066(obj,1) 并打印断言；D2 下 obj+800 已清零 ⇒ 0x59ee1 恒返 4，
        // 该路径不可达（0x59134 的字符串参数故不复现）；仍保留 0x5b066 调用以对齐结构。
        if (rc18 == 1) { (void)fnFailSeq(obj, 1); }
    }
    // A19（D2）：不调用 0x5a28d；内联其"分配＋初始化"两步，跳过 0x595eb 类型判定与失败释放。
    const int rc19a = fnAlloc(obj, 0x18, 1, reinterpret_cast<void**>(obj + 0x2A8));
    if (rc19a != 0) { gGvmBook.failMask |= (1U << 20); gM2Step = 0x07; return rc19a; }
    void* const q = *reinterpret_cast<void* const*>(obj + 0x2A8);
    if (!nredKernelPtrOk(q)) {                                               // 解引用前置门（0x58fad 将解引用）
        gGvmBook.failMask |= (1U << 20); gM2Step = 0x07; return kSwResStepFail;   // 6
    }
    const int rc19b = fnInit(obj, q, 0x18);
    if (rc19b != 0) { gGvmBook.failMask |= (1U << 20); gM2Step = 0x07; return rc19b; }
    gGvmBook.omittedMask |= (1U << 18);   // 队列管理器类型判定（0x595eb）与失败释放被跳过
    // A20：终态回读（S2/S3/S4）——`p2+8==obj`、首指针 cfg==0、queue 对象 q!=0。
    gM2Cfg = *reinterpret_cast<const UInt64*>(obj + 0x2C8);
    gM2Q   = *reinterpret_cast<const UInt64*>(obj + 0x2A8);
    if (*reinterpret_cast<const UInt64*>(P2 + 8) != objU || gM2Q == 0 || gM2Cfg != 0) {
        gGvmBook.failMask |= (1U << 20); gM2Step = 0x08; return kSwResStepFail;   // 6
    }
    return kSwResPass;                                                       // 0（funcs[0] 层成功）
}

// ── 模块 14（MES）本体：**D2**（R170／R171／R172）────────────────────────────────────────────────
//  被替换实现＝client-14 的 `funcs[0]`＝`0x42e94`（MES `sw_init`，`42e94..43417`）。
//  保留：形状、0xB30 工作区＋0x43c30 分配/0x43cfe 初始化、字段搬运、三段 0x260 记录拷贝（含 break）、
//  0x43a10 开关位、obj+0x9e8 两位、默认回调 0x47d58、0x43aa4。
//  删除：0x441ad 的 per-code 门分派（保留门前两次清零 p2+8=0／obj+0x788=0）；其 per-code 产物
//  obj+0x790..0x7af／obj+0x7b0..0x7cf 清零并逐 8 字节回读；描述符数组不写（计数 0）并登记。
//  ⚠ 机器码修正（相对 R172 §9.1）：① A3 的 p1+0xE48/0xE40 必须写入 0xB30 工作区 +0x10/+8
//    （`42edf..42efa`；0x43c30 经 `*(workspace+0x10)` 取服务对象，否则解引用 NULL）；
//    ② A14 的第二实参是 `p2+0x190`（`433b7 addq $400,%r14`；400=0x190），不是 p2+0x32。
//  ⚠ A12 的默认函数指针组（obj+0xa30..0xb28）不安装（登记 bit7，R172 §4.2／§9.2）；但 0x43aa4 会在
//    obj+0xad0 非 0 时经它盲调（`43aca`）⇒ 显式把该槽清 0，使 §9.2 的"未装 ⇒ 0"成为机械事实。
//  ⚠ 口径：只可写"MES 软件侧最小初始化完成（不含 per-code 队列表）"，不得写"模块 14 完成／MES 就绪"。
static constexpr UInt32 kMesLocalBytes = 0xB30;
static UInt8 gMesLocal[kMesLocalBytes];   // 苹果同位的 0xB30 B 栈工作区；改 static ⇒ 零栈占用（串行单写者）
static int nredMesSwInit(void* const p1, void* const p2)
{
    gGvmBook.failMask = 0; gGvmBook.omittedMask = 0;
    gM14N += 1; gM14Step = 0; gM14Code = 0xFFFFFFU;
    gM14Cfg = ~0ULL; gM14Q = ~0ULL;
    UInt8* const L = gMesLocal;
    __builtin_memset(L, 0, kMesLocalBytes);                                   // A1
    if (p1 == nullptr || p2 == nullptr) {                                     // A2
        gGvmBook.failMask |= (1U << 11); gM14Step = 0x01; return kSwResNullArg;      // 2
    }
    if (!nredKernelPtrOk(p1) || !nredKernelPtrOk(p2)) {
        gGvmBook.failMask |= (1U << 11); gM14Step = 0x01; return kSwResParseFail;    // 1
    }
    const UInt8* const P1 = static_cast<const UInt8*>(p1);
    UInt8* const P2 = static_cast<UInt8*>(p2);
    const UInt64 v48 = *reinterpret_cast<const UInt64*>(P1 + 0xE48);          // A3
    const UInt64 v40 = *reinterpret_cast<const UInt64*>(P1 + 0xE40);
    *reinterpret_cast<UInt64*>(L + 0x10) = v48;   // 机器码 42edf/42eee：工作区 +0x10＝服务对象
    *reinterpret_cast<UInt64*>(L + 0x08) = v40;   // 机器码 42ef2/42efa：工作区 +8

    using NRedMesAlloc = int (*)(void*, UInt64, UInt32, void**);
    using NRedMesInit  = int (*)(void*, void*, UInt32);
    using NRedMesMisc  = void (*)(void*);
    using NRedMesTail  = int (*)(void*, void*);
    using NRedMesCode  = UInt32 (*)(const void*);
    const NRedMesAlloc fnAlloc = reinterpret_cast<NRedMesAlloc>(gHwlibsSlide + 0x43C30);
    const NRedMesInit  fnInit  = reinterpret_cast<NRedMesInit>(gHwlibsSlide + 0x43CFE);
    const NRedMesMisc  fnMisc  = reinterpret_cast<NRedMesMisc>(gHwlibsSlide + 0x43A10);
    const NRedMesTail  fnTail  = reinterpret_cast<NRedMesTail>(gHwlibsSlide + 0x43AA4);
    const NRedMesCode  fnCode  = reinterpret_cast<NRedMesCode>(gHwlibsSlide + 0x48632);

    UInt64 objU = 0;
    const int rc4 = fnAlloc(L, kMesLocalBytes, 1, reinterpret_cast<void**>(&objU));   // A4
    if (rc4 != 0) {
        *reinterpret_cast<UInt64*>(P2 + 0) = 0;                              // 苹果：*p2=0
        gGvmBook.failMask |= (1U << 12); gM14Step = 0x02; return rc4;
    }
    UInt8* const obj = reinterpret_cast<UInt8*>(objU);
    if (!nredKernelPtrOk(obj)) { gGvmBook.failMask |= (1U << 12); gM14Step = 0x02; return kSwResStepFail; }
    const int rc5 = fnInit(L, obj, kMesLocalBytes);                            // A5（苹果不判；我方判）
    if (rc5 != 0) { gGvmBook.failMask |= (1U << 12); gM14Step = 0x03; return rc5; }
    *reinterpret_cast<UInt64*>(P2 + 0) = objU;                                 // A6
    *reinterpret_cast<UInt64*>(obj + 0x10) = v48;                              // A7
    *reinterpret_cast<UInt64*>(obj + 0x08) = v40;
    for (UInt32 i = 0; i != 0xE40; i += 0x260) {                               // A8
        const UInt32 t = *reinterpret_cast<const UInt32*>(P1 + i);
        UInt32 dst;
        if (t == 0x21) dst = 0x524; else if (t == 0x1B) dst = 0x2C4; else if (t == 0x0B) dst = 0x64;
        else break;                                                            // 苹果逐字：其它类型 break
        __builtin_memcpy(obj + dst, P1 + i, 0x260);
    }
    fnMisc(obj);                                                               // A9
    // A10：`-NRedMesReal` 关 ⇒ D2 原行为（W2/W3 + 清零 + 回读为 0，逐字不变）。
    //     开 ⇒ 调用苹果 `0x441ad` 真实初始化 per-code 描述符（第 4 参＝P2 基址）。
    //  ⚠ 两个 code 实参**必须按原件取 8 字节**：`42fbe movq 108(%rdi),%rsi` / `42fc2 movq 116(%rdi),%rdx`
    //    （0x48632 按 `p[0]<<16 | p[1]<<8 | p[2]` 取三个相邻 u32；只传两个 u32 会丢 `obj+0x70` 那一位）。
    *reinterpret_cast<UInt32*>(P2 + 8) = 0;                                    // W2（两路径共用）
    if (gMesB) {
        // 254（`-NRedMesB`）：b 系列提供者。仅当 code 属 b 系列（(code>>16)==0x0b）才提供；
        //  否则 fail-closed（不伪造成 a 系列描述符）。不调用 0x441ad、不写 MMIO。
        const UInt32 codeBclass = fnCode(reinterpret_cast<const void*>(obj + 0x6C));
        gMesBCode = codeBclass;
        if ((codeBclass >> 16) != 0x0BU) {
            gGvmBook.failMask |= (1U << 12); gM14Step = 0x05; return kSwResStepFail;   // 6
        }
        if (!sMesBInit) {
            for (unsigned i = 0; i < 4; ++i) {
                *reinterpret_cast<UInt32*>(sMesBDesc[i] + 0x10) = 0xF0005000U;   // uc_start_addr_lo
            }
            sMesBInit = true;
        }
        for (unsigned i = 0; i < 4; ++i) {
            *reinterpret_cast<UInt64*>(obj + 0x790 + i * 8) =
                reinterpret_cast<UInt64>(sMesBDesc[i]);
        }
        *reinterpret_cast<UInt64*>(obj + 0x7B0) = 0;   // b 系列无 0x43d4f 子对象
        gMesBP790 = *reinterpret_cast<const UInt64*>(obj + 0x790);
        gMesBV10 = (gMesBP790 != 0)
            ? *reinterpret_cast<const UInt32*>(reinterpret_cast<const UInt8*>(gMesBP790) + 0x10) : 0;
        for (unsigned i = 0; i < 4; ++i) {
            if (*reinterpret_cast<const UInt64*>(obj + 0x790 + i * 8) == 0) {
                gGvmBook.failMask |= (1U << 12); gM14Step = 0x05; return kSwResStepFail;   // 6
            }
        }
    }
    else if (gMesReal) {
        using NRedMes441ad = int (*)(void*, UInt64, UInt64, void*);
        const NRedMes441ad fn441ad = reinterpret_cast<NRedMes441ad>(gHwlibsSlide + 0x441AD);
        const UInt64 codeA = *reinterpret_cast<const UInt64*>(obj + 0x6C);
        const UInt64 codeB = *reinterpret_cast<const UInt64*>(obj + 0x74);
        gMesCode = fnCode(reinterpret_cast<const void*>(obj + 0x6C));
        const int rc441 = fn441ad(obj, codeA, codeB, P2);                      // 第 4 参＝P2 基址
        gMesRet = static_cast<UInt32>(rc441);
        gMesP790 = *reinterpret_cast<const UInt64*>(obj + 0x790);
        gMesP7B0 = *reinterpret_cast<const UInt64*>(obj + 0x7B0);
        if (rc441 != 0) { gGvmBook.failMask |= (1U << 12); gM14Step = 0x05; return kSwResStepFail; }
        if (gMesP790 == 0) { gGvmBook.failMask |= (1U << 12); gM14Step = 0x05; return kSwResStepFail; }
    }
    else {
        *reinterpret_cast<UInt32*>(obj + 0x788) = 0;                           // W3
        __builtin_memset(obj + 0x790, 0, 0x20);
        __builtin_memset(obj + 0x7B0, 0, 0x20);
        for (UInt32 off = 0; off < 0x20; off += 8) {
            if (*reinterpret_cast<const UInt64*>(obj + 0x790 + off) != 0 ||
                *reinterpret_cast<const UInt64*>(obj + 0x7B0 + off) != 0) {
                gGvmBook.failMask |= (1U << 12); gM14Step = 0x05; return kSwResStepFail;  // 6
            }
        }
        if (*reinterpret_cast<const UInt32*>(P2 + 8) != 0 ||
            *reinterpret_cast<const UInt32*>(obj + 0x788) != 0) {
            gGvmBook.failMask |= (1U << 12); gM14Step = 0x04; return kSwResStepFail;      // 6
        }
        gGvmBook.omittedMask |= (1U << 4);   // per-code 分派跳过（版本门）
        gGvmBook.omittedMask |= (1U << 5);   // 描述符 0 项导出
        gGvmBook.omittedMask |= (1U << 6);   // 队列对象/字符串指针未创建
    }

    {                                                                          // A11
        const UInt32 b = *reinterpret_cast<const UInt32*>(P1 + 0xE50);
        UInt32 w = *reinterpret_cast<const UInt32*>(obj + 0x9E8);
        w = (w & ~1U) | (b & 1U);
        w = (w & ~2U) | (b & 2U);
        *reinterpret_cast<UInt32*>(obj + 0x9E8) = w;
    }
    gGvmBook.omittedMask |= (1U << 7);   // A12 的函数指针组（obj+0xa30..0xb28）未安装
    *reinterpret_cast<UInt64*>(obj + 0xAD0) = 0;   // 安全守卫：0x43aa4 在 43aca 经此槽盲调
    gM14Code = fnCode(reinterpret_cast<const void*>(obj + 0x6C));   // A12 的 code（0x48632）
    // A13：默认组回调 0x47d58（无间接调用；只写 p2+0x194/0x19c 与 obj+0xa00）
    reinterpret_cast<void (*)(void*, void*)>(gHwlibsSlide + 0x47D58)(
        obj, reinterpret_cast<void*>(P2 + 0x194));
    const int rc14 = fnTail(obj, reinterpret_cast<void*>(P2 + 0x190));          // A14（+0x190，非 +0x32）
    if (rc14 != 0 && rc14 != 3) { gGvmBook.failMask |= (1U << 12); gM14Step = 0x06; return rc14; }
    if (*reinterpret_cast<const UInt64*>(P2 + 0) != objU) {                     // 终态 S2
        gGvmBook.failMask |= (1U << 12); gM14Step = 0x07; return kSwResStepFail; // 6
    }
    gM14Cfg = *reinterpret_cast<const UInt64*>(obj + 0x790);
    gM14Q   = *reinterpret_cast<const UInt64*>(obj + 0x7B0);
    return kSwResPass;                                                          // 0
}

// ── 统一入口（**唯一**）：按模块号分派（R60 需求 1.2）─────────────────────────────────────
//  · 计数：`gGvmSwCalls`（全部）与 `gSwCallN[id]`（按模块号，供 `[swi] n=`）；`gSwLastId`＝本次模块号。
//  · 模块 5 ⇒ 本体；**其余模块 ⇒ 显式"未实现"**（只记日志 ＋ **如实返回非 0**；禁止空壳成功）。
//  · 入参为空 ⇒ 返 2（与苹果那版同值）。
static int NRedSwInitDispatch(const UInt32 clientId, void* const info, void* const out)
{
    // 观测通道修复（需求 v2）**M3：我方替换实现被调到**（决定性的一条）⇒ 在本入口打标记并刷一次。
    //  ⚠ 本函数是**唯一**分派入口（R60 §1.2）⇒ 任何模块的调用都会在此打标记；该分片的戳记行给出送达的模块号。
    if (gTraceAtIpSeq) { nredTraceMark(gFlushedAtIpSeqM3, kMarkM3, static_cast<int>(sizeof(kMarkM3) - 1)); }
    gGvmSwCalls += 1;
    if (clientId < 16) { gSwCallN[clientId] += 1; }
    gSwLastId = clientId;
    const UInt64 o8init = (out != nullptr)
        ? *reinterpret_cast<const UInt64*>(static_cast<const UInt8*>(out) + 8) : 0;
    int res = kSwResUnimpl;                                     // 默认＝该模块未实现（非 0）
    if (info == nullptr || out == nullptr) { res = kSwResNullArg; }
    else {
        // R105：入参两个 32 位字段（**`info` 已过既有非空判断**）⇒ 留存供 `[swinfo]` 戳记（纯读 8 字节）。
        gSwInfo12 = *reinterpret_cast<const UInt32*>(static_cast<const UInt8*>(info) + 12);
        gSwInfo8  = *reinterpret_cast<const UInt32*>(static_cast<const UInt8*>(info) + 8);
        switch (clientId) {
            case 2: res = nredSdmaSwInit(info, out); break;          // 模块 2＝SDMA（R169 D2 替换）
            case 5: res = nredGvmSwInit(info, out, o8init); break;   // 模块 5＝GVM（A1–A11）
            case 6: res = nredPspSwInit(info, out); break;           // 模块 6＝PSP（R96 阶段 1）
            case 8: res = nredSmuSwInit(info, out); break;           // 模块 8＝SMU（R108 阶段 2）
            case 7: res = nredGcSwInit(info, out); break;            // 模块 7＝GC（R100 阶段 1）
            case 14: res = nredMesSwInit(info, out); break;           // 模块 14＝MES（R172 D2 替换）
            case 9: res = nredVcnSwInit(info, out); break;           // 模块 9＝VCN（R175 实实现）
            case 15: res = nredJpegSwInit(info, out); break;          // 模块 15＝JPEG（R177 实实现）
            case 10: res = nredDmcuSwInit(info, out); break;          // 模块 10＝DMCU（R181 S1 fail-closed）
            default: break;                                          // 其余模块：**显式未实现**
        }
    }
    // R98 §1（S1）：按模块留存**最近一次返回值**与**调用次数**（`0xFF`＝从未被调到；计数饱和 255）。
    if (clientId < 16) {
        // ⚠ 护栏命中在 `wrapMode9440f` 内直接返回 0（苹果极性失败），不会进入本函数 ⇒ 本表不被污染。
        gSwResById[clientId] = static_cast<UInt8>(res);
        if (gSwCntById[clientId] != 255) { ++gSwCntById[clientId]; }
    }
    nredSwLogCall(clientId, info, out, o8init, res);
    return res;
}
// ─── 242/243：stage1 预检（`-NRedSt1Pre`，默认关）────────────────────────────────────────
//  仅在护栏分支 c==1 且 `return 0` 之前调用（不调原件、不解引用 P_i 内部）。
//  P_i = *(void**)(ctx + 8 + i*8)（子对象 getter 0x93980 的统一入口）。
//  243：不再直接打印（正文区系统性丢失）——结果写入 BSS 标量，由 `nredTraceFlush` 头部区输出。
//  每次调用刷新 mask（先清零再置位）；n 饱和 255；头部行内零解引用。
static void nredStage1Precheck(const void* const ctx)
{
    if (!nredKernelPtrOk(ctx)) {
        gSt1Mask = 0;
        gSt1CtxOk = 0;
    } else {
        gSt1CtxOk = 1;
        gSt1Mask = 0;
        static const UInt8 kIds[7] = {2, 5, 6, 7, 8, 9, 14};
        for (unsigned k = 0; k < 7; ++k) {
            const UInt32 i = kIds[k];
            const void* const p = *reinterpret_cast<void* const*>(static_cast<const UInt8*>(ctx) + 8 + i * 8u);
            if (p != nullptr && nredKernelPtrOk(p)) { gSt1Mask |= (1U << i); }
        }
    }
    if (gSt1N < 255) { ++gSt1N; }
}

// ─── 258：client 3（BGM）stage1 前置检查（11 条；规格 `tmp/tech/R-client3-stage1叶-2026-10-22.md` §4）
//   ① ctx KPtr ② P3=*(ctx+0x20) KPtr ③ *(P3+0x18) ④ *(P3+0x28) ⑤ X=*(P3) KPtr
//   ⑥ Y=*(X) KPtr ⑦ Z=*(Y) KPtr ⑧ provider=*(Z+0x28) KPtr ⑨ *(P3+0x30) ⑩ *(P3+0x40)
//   ⑪ *(provider+0x268)。每级先校验上一级指针再读固定偏移；不跟随函数指针、不调用苹果方法。
//   独立 noinline 帧，保持 `nredStage1Guard` 分派帧不变。
// 260：记录“首个失败步号”（noinline；只写 BSS，不改返回值/白名单）。
__attribute__((noinline))
static void nredStage1GfMark(const UInt32 client, const UInt32 step)
{
    if (gSt1GfClient != 0xFFFFFFFFU) { return; }
    gSt1GfClient = client;
    gSt1GfStep = step;
}

__attribute__((noinline))
static bool nredStage1Guard3(const void* const ctx)
{
    gSt1B3P3 = 0;
    if (!nredKernelPtrOk(ctx)) { nredStage1GfMark(3, 1); return false; }
    const UInt8* const p3 = *reinterpret_cast<const UInt8* const*>(static_cast<const UInt8*>(ctx) + 0x20);
    if (!nredKernelPtrOk(p3)) { nredStage1GfMark(3, 2); return false; }
    gSt1B3P3 = reinterpret_cast<UInt64>(p3);
    if (*reinterpret_cast<const UInt64*>(p3 + 0x18) == 0) { nredStage1GfMark(3, 3); return false; }
    if (*reinterpret_cast<const UInt64*>(p3 + 0x28) == 0) { nredStage1GfMark(3, 4); return false; }
    const UInt8* const x = *reinterpret_cast<const UInt8* const*>(p3);
    if (!nredKernelPtrOk(x)) { nredStage1GfMark(3, 5); return false; }
    const UInt8* const y = *reinterpret_cast<const UInt8* const*>(x);
    if (!nredKernelPtrOk(y)) { nredStage1GfMark(3, 6); return false; }
    const UInt8* const z = *reinterpret_cast<const UInt8* const*>(y);
    if (!nredKernelPtrOk(z)) { nredStage1GfMark(3, 7); return false; }
    const UInt8* const prov = *reinterpret_cast<const UInt8* const*>(z + 0x28);
    if (!nredKernelPtrOk(prov)) { nredStage1GfMark(3, 8); return false; }
    if (*reinterpret_cast<const UInt64*>(p3 + 0x30) == 0) { nredStage1GfMark(3, 9); return false; }
    if (*reinterpret_cast<const UInt64*>(p3 + 0x40) == 0) { nredStage1GfMark(3, 10); return false; }
    if (*reinterpret_cast<const UInt64*>(prov + 0x268) == 0) { nredStage1GfMark(3, 11); return false; }
    return true;
}

// ─── 244/245/255/258：stage1 放行前置检查（`-NRedStage1`；白名单 {3,7,14}）────────────────
//  client 7（255，规格 `kb/技术草案/R-stage1放行规格-client7-2026-10-21.md` §1，5 跳顺序执行）：
//   ① ctx 过 KPtr ② P7=*(ctx+0x40) 非空且过 KPtr ③ *(P7+0x18)≠0 ④ *(P7+0x28)≠0
//   ⑤ P3=*(ctx+0x20) 非空且过 KPtr（唯一硬 fault 依赖：0x97665 读 P3+0x50）。
//  client 14（244/245 批 1 修订版）：① P14=*(ctx+0x78) ② *(P14+0x18) ③ MES=*(P14+0x28)
//   ④ *(MES+0x08) ⑤ *(MES+0x790)；代码保留（当前默认路径下 G5 恒 0 ⇒ 不生效）。
//  每条都先 `nredKernelPtrOk` 校验上一级指针再读固定偏移；不跟随函数指针、不调用任何东西。
//  读出值存入 BSS（头部行零解引用）；返回 false ⇒ 调用方按苹果极性 `return 0`。

// ─── 259：client 2/5/6/9 的 stage1 前置检查（规格 `tmp/tech-manager/R-stage1剩余client放行规格-2026-10-22.md` §1）
//  统一纪律：每级先 `nredKernelPtrOk` 再读固定偏移；不跟随函数指针、不调用苹果方法；
//  只保留 1 个游标局部（其余存 `gSt1*`），保证每个函数栈帧 ≤64 B。
__attribute__((noinline))
static bool nredStage1Guard2(const void* const ctx)
{
    gSt1G2P = 0; gSt1G2Key = 0;
    gSt1P3 = 0;   // 批次 P4：与 guard7 一致——本 guard 未跑到 P3 检查时，读数不得残留上一个 guard 的值
    if (!nredKernelPtrOk(ctx)) { nredStage1GfMark(2, 1); return false; }
    const UInt8* q = *reinterpret_cast<const UInt8* const*>(static_cast<const UInt8*>(ctx) + 0x18);
    if (!nredKernelPtrOk(q)) { nredStage1GfMark(2, 2); return false; }
    gSt1G2P = reinterpret_cast<UInt64>(q);
    q = *reinterpret_cast<const UInt8* const*>(q);                 // dev=*(P2)
    if (!nredKernelPtrOk(q)) { nredStage1GfMark(2, 3); return false; }
    q = *reinterpret_cast<const UInt8* const*>(q);                 // *(dev)
    if (!nredKernelPtrOk(q)) { nredStage1GfMark(2, 4); return false; }
    q = *reinterpret_cast<const UInt8* const*>(q + 0x28);          // slot1
    if (!nredKernelPtrOk(q)) { nredStage1GfMark(2, 5); return false; }
    gSt1G2Key = reinterpret_cast<UInt64>(q);
    if (*reinterpret_cast<const UInt64*>(q + 0x70) == 0) { nredStage1GfMark(2, 6); return false; }
    const UInt8* const p2 = reinterpret_cast<const UInt8*>(gSt1G2P);
    if (*reinterpret_cast<const UInt64*>(p2 + 0x18) == 0) { nredStage1GfMark(2, 7); return false; }
    // 批次 P4／P4-2：P3 = *(ctx+0x20) 必须非空。
    //   依据（批B §2 行 3）：client2/7/9 的 stage1 都硬依赖 P3（经 0x95e90(dev)=0x93980(dev,3)）；
    //   0x97307 读 P3+0x28 并 call *0x40(P3)、0x97621 读 P3+0x50，**均未判 P3==0**
    //   ⇒ P3==0 时读地址 0x28 ⇒ fault。guard7 已有此检查（步 5），guard2/9 缺。
    //   ⚠ P3 当前＝0（批B M1-1）⇒ 本检查生效后 client2 恒 fail-closed（**预期**）。
    const UInt8* const p3 = *reinterpret_cast<const UInt8* const*>(
        static_cast<const UInt8*>(ctx) + 0x20);
    gSt1P3 = reinterpret_cast<UInt64>(p3);
    if (!nredKernelPtrOk(p3)) { nredStage1GfMark(2, 8); return false; }
    return true;
}

__attribute__((noinline))
static bool nredStage1Guard5(const void* const ctx)
{
    gSt1G5P = 0; gSt1G5Ip = 0; gSt1G5Ipr = 0;
    gSt1G5P20 = 0; gSt1G5P21 = 0; gSt1G5P22 = 0; gSt1G5P23 = 0;
    // 批次 P：新增中间量同样逐次清零（沿用本函数"每次重算"的既有纪律，避免跨次残留读数）。
    gSt1G5H = 0; gSt1G5Root = 0; gSt1G5Head = 0; gSt1G5Blk = 0; gSt1G5Arr = 0;
    gSt1G5F70 = 0; gSt1G5F268 = 0; gSt1G5F2B8 = 0;
    gSt1G5E88 = 0; gSt1G5E90 = 0; gSt1G5E98 = 0;
    if (!nredKernelPtrOk(ctx)) { nredStage1GfMark(5, 1); return false; }
    const UInt8* q = *reinterpret_cast<const UInt8* const*>(static_cast<const UInt8*>(ctx) + 0x30);
    if (!nredKernelPtrOk(q)) { nredStage1GfMark(5, 2); return false; }
    gSt1G5P = reinterpret_cast<UInt64>(q);
    if (*reinterpret_cast<const UInt64*>(q) == 0) { nredStage1GfMark(5, 3); return false; }
    q = *reinterpret_cast<const UInt8* const*>(q);                 // *(P5)
    if (!nredKernelPtrOk(q)) { nredStage1GfMark(5, 4); return false; }
    q = *reinterpret_cast<const UInt8* const*>(q);                 // *(*(P5)) = 0x8b4bf 实参
    if (!nredKernelPtrOk(q)) { nredStage1GfMark(5, 5); return false; }
    q = *reinterpret_cast<const UInt8* const*>(q);                 // 0x8b4bf 内部：*(实参)
    if (!nredKernelPtrOk(q)) { nredStage1GfMark(5, 6); return false; }
    q = *reinterpret_cast<const UInt8* const*>(q + 0x28);          // slot1 = *(helper 结果 +0x28)
    if (!nredKernelPtrOk(q)) { nredStage1GfMark(5, 7); return false; }
    if (*reinterpret_cast<const UInt64*>(q + 0x70) == 0) { nredStage1GfMark(5, 8); return false; }
    const UInt8* p5 = reinterpret_cast<const UInt8*>(gSt1G5P);
    if (*reinterpret_cast<const UInt64*>(p5 + 0x18) == 0) { nredStage1GfMark(5, 9); return false; }
    q = *reinterpret_cast<const UInt8* const*>(p5 + 0x28);         // S
    if (!nredKernelPtrOk(q)) { nredStage1GfMark(5, 10); return false; }
    gSt1G5H = reinterpret_cast<UInt64>(q);                         // 批次 P：供步 27 使用
    q = *reinterpret_cast<const UInt8* const*>(q + 0xE48);         // *(S+0xE48)
    if (!nredKernelPtrOk(q)) { nredStage1GfMark(5, 11); return false; }
    if (*reinterpret_cast<const UInt64*>(q + 0x90) == 0) { nredStage1GfMark(5, 12); return false; }
    q = *reinterpret_cast<const UInt8* const*>(q + 0x08);          // *(..+0x08)
    if (!nredKernelPtrOk(q)) { nredStage1GfMark(5, 13); return false; }
    // 14（批次 P／P-2 修正）：IP=*(A)（＝h8；叶 0x989fd/0x98a00 实测同值）。
    //   ⚠ 264 版把 iproot 写成 *(IP+0x58)，而 IP 是 8 B 句柄 ⇒ 读对象之外
    //   （r271 实测落在内核镜像区 ffffff801155cc00，不是合法 iproot）。
    //   叶子算的是 0x8b8e7(h8) = *(*(h8)+0x58)：
    //     0x8b4bf/0x8b8e7 同族 —— 0x8b4c8 mov (%rdi),%rdi 先取表，
    //     0x8bbd3 mov 0x10(%rdi,%rax,8),%rax 再取槽（槽 3 ⇒ +0x58）。
    q = *reinterpret_cast<const UInt8* const*>(q);                 // IP=*(A)（＝h8）
    if (!nredKernelPtrOk(q)) { nredStage1GfMark(5, 14); return false; }
    gSt1G5Ip = reinterpret_cast<UInt64>(q);
    {
        const UInt8* const root = *reinterpret_cast<const UInt8* const*>(q);   // root=*(h8)
        if (!nredKernelPtrOk(root)) { nredStage1GfMark(5, 14); return false; }
        gSt1G5Root = reinterpret_cast<UInt64>(root);
        q = *reinterpret_cast<const UInt8* const*>(root + 0x58);   // dev=*(root+0x58)
        if (!nredKernelPtrOk(q)) { nredStage1GfMark(5, 14); return false; }
    }
    gSt1G5Ipr = reinterpret_cast<UInt64>(q);
    // 15-18（批次 P／P-3 降级为观测）：该四级模型＝0xa31f6（IP 注册表按 type 查表）。
    //   原型存在（0x1a538 的 0x1a552/0x1a559/0x1a565 ＋ 回调 0xa31f6 的
    //   0xa3208/0xa3214/0xa3220/0xa3224），但它属于 *(T+0x90) 回调的内部实现，
    //   该回调的实际绑定未证（M1 U2）。且 0xa31f6 对 *(dev+0xE8)==0 自带判空并返回 0
    //   ⇒ 该字段为 0 不会 trap。⇒ 只观测，不据此 fail-closed。
    {
        const UInt8* const ipr = reinterpret_cast<const UInt8*>(gSt1G5Ipr);
        if (nredKernelPtrOk(ipr)) {
            const UInt8* const head = *reinterpret_cast<const UInt8* const*>(ipr + 0xE8);
            gSt1G5Head = reinterpret_cast<UInt64>(head);
            if (nredKernelPtrOk(head)) {
                const UInt8* const blk = *reinterpret_cast<const UInt8* const*>(head + 0x40);
                gSt1G5Blk = reinterpret_cast<UInt64>(blk);
                if (nredKernelPtrOk(blk)) {
                    gSt1G5Arr = *reinterpret_cast<const UInt64*>(blk + 0x08);
                }
            }
        }
    }
    // 19（批次 P2／P2-2 **已删除**）：原查 `*(P5+0x38)`。
    //   ⚠ 该字段由**叶子自己的第 15 步**填入：`0x9880c memcpy(P5+0x30, desc+0xf0, 0xd8)`
    //     ⇒ `P5+0x38 ← desc+0xf8`，而 `desc+0xf8` 由第 14 步的 `0x1936e` 写为 `FUN_0001965c`
    //     （仅在 `0x1936e` 的 `return 0` 分支）。⇒ 叶子运行前必为 0，叶子跑到 `0x98a28` 时必非 0。
    //   ⚠ 且若第 14 步失败，叶子在 `0x987e6` 提前返回 ⇒ `0x98a28` 不执行。
    //   ⇒ 该判据既过严（拦掉合法首次调用）又冗余（真实路径上不可能为 0）。
    //   ⇒ 步号 19 保留为空号，不重编号（历史读数可比）。
    // 20-23（批次 P／P-4）：叶内 0x989fd 是 `mov (%rdi),%rax`，rdi = P5（0x989bc）
    //   ⇒ p = *(P5) = ctx（不是 *(ctx) = h8）。逐级复刻并分别编号，便于用 [st1gf] 定位哪一级为空。
    {
        const UInt8* const p = *reinterpret_cast<const UInt8* const*>(
            reinterpret_cast<const UInt8*>(gSt1G5P));
        if (!nredKernelPtrOk(p)) { nredStage1GfMark(5, 20); return false; }
        gSt1G5P20 = reinterpret_cast<UInt64>(p);
    }
    {
        const UInt8* const p = reinterpret_cast<const UInt8*>(gSt1G5P20);
        const UInt8* const qq = *reinterpret_cast<const UInt8* const*>(p);
        if (!nredKernelPtrOk(qq)) { nredStage1GfMark(5, 21); return false; }
        gSt1G5P21 = reinterpret_cast<UInt64>(qq);
    }
    {
        const UInt8* const qq = reinterpret_cast<const UInt8*>(gSt1G5P21);
        const UInt8* const r = *reinterpret_cast<const UInt8* const*>(qq);
        if (!nredKernelPtrOk(r)) { nredStage1GfMark(5, 22); return false; }
        gSt1G5P22 = reinterpret_cast<UInt64>(r);
    }
    {
        const UInt8* const r = reinterpret_cast<const UInt8*>(gSt1G5P22);
        const UInt8* const s = *reinterpret_cast<const UInt8* const*>(r + 0x28);
        if (!nredKernelPtrOk(s)) { nredStage1GfMark(5, 23); return false; }
        gSt1G5P23 = reinterpret_cast<UInt64>(s);
    }
    // 24（批次 P／P-5）：门控从 gQqHdl 改为 gStage1（随护栏本身启用）。
    //   强度按叶内真实调用点分级：
    //     f2b8 —— 叶 0x9894f CALL R12，**成功路径**（前置 0x9891c/0x98925/0x98932 全过）
    //             ⇒ 无条件校验（非 0 则必须是 __text 入口）。
    //     f70  —— 叶 0x9885f CALL RBX，**仅错误路径**（前置 0x98811 MOVSXD/CMP 0x3/JNC，
    //             即 local_68 >= 3 才进）⇒ 放宽为"非 0 则校验"；无条件化比苹果更严。
    //     f268 —— 叶 0x98ac6 CALL [RBP-0x40]，且 0x98a42 判 *(slot1+0x268)==0 即合法
    //             ⇒ 保持"非 0 则校验"。
    if (gStage1) {
        const UInt8* const s1 = reinterpret_cast<const UInt8*>(gSt1G5P23);
        if (!nredKernelPtrOk(s1)) { nredStage1GfMark(5, 24); return false; }
        const UInt64 f70  = *reinterpret_cast<const UInt64*>(s1 + 0x70);
        const UInt64 f268 = *reinterpret_cast<const UInt64*>(s1 + 0x268);
        const UInt64 f2b8 = *reinterpret_cast<const UInt64*>(s1 + 0x2b8);
        gSt1G5F70 = f70; gSt1G5F268 = f268; gSt1G5F2B8 = f2b8;
        // 批次 P2／P2-2b：`-NRedQp` 开 ⇒ 叶子在第 14 步被截断 ⇒ 步 15 及之后不执行
        //   ⇒ 这三个指针**永不被叶子使用** ⇒ 只观测、不 fail-closed。
        //   Q′ 关 ⇒ 叶子本就不放行（`case 5` 的 `ok = (gQp && gQpArmed) ? v : false`）
        //   ⇒ 此处仍强制，以保留真信号。
        if (!gQp) {
            if (f70  != 0 && !nredTextPrologue(f70))  { nredStage1GfMark(5, 24); return false; }
            if (f268 != 0 && !nredTextPrologue(f268)) { nredStage1GfMark(5, 24); return false; }
            // f2b8 **无条件**：叶 0x988fa MOV R12,[RAX+0x2b8] → 0x9894f CALL R12，
            //   中间（0x98901-0x9894c）只做缓冲区清零与两次判空（*(P5+0x28)、*(P5+0x78)），
            //   **不判 R12** ⇒ f2b8==0 时叶子会 call *0（trap）⇒ 为 0 也必须失败。
            //   （nredTextPrologue(0) 本身返回 false，故这一行同时覆盖"为 0"与"非入口"。）
            if (!nredTextPrologue(f2b8)) { nredStage1GfMark(5, 24); return false; }
        }
    }
    // 25／26（批次 P2／P2-2 **已删除**）：原查 `*(P5+0x78)`／`*(P5+0xE0)`。
    //   ⚠ 与步 19 同源：两者都由叶第 15 步的 memcpy 填入
    //     （`P5+0x78 ← desc+0x138`＝`&LAB_000197ee`；`P5+0xE0 ← desc+0x1a0`＝`FUN_000199ac`）
    //     ⇒ 叶子运行前必为 0 ⇒ 假前置。
    //   ⇒ 步号 25／26 保留为空号。
    // 27（批次 P 新增；批次 P2 起：Q′ 关时强制、Q′ 开时只观测）：0x1aef4/0x1dde1/0x3d1b5 的首句直接读
    //   *(H+0xE90)+8 / *(H+0xE88)+8 / *(H+0xE98)+8，**不判空**
    //   （0x1af08 MOV RAX,[RDI+0xe90] ; 0x1af0f MOV EAX,[RAX+0x8]；
    //     0x1ddf8 MOV R13,[RDI+0xe88]；0x3d1c9 MOV RAX,[RDI+0xe98] ; 0x3d1d0 MOV EAX,[RAX+0x8]）
    //   ⇒ 为 0 时叶子必 trap。这是原护栏完全缺失的一项。
    {
        const UInt8* const H = reinterpret_cast<const UInt8*>(gSt1G5H);
        const UInt64 e88 = *reinterpret_cast<const UInt64*>(H + 0xE88);
        const UInt64 e90 = *reinterpret_cast<const UInt64*>(H + 0xE90);
        const UInt64 e98 = *reinterpret_cast<const UInt64*>(H + 0xE98);
        gSt1G5E88 = e88; gSt1G5E90 = e90; gSt1G5E98 = e98;
        // 批次 P2／P2-2b：`-NRedQp` 开 ⇒ 第 14 步被截断 ⇒ 0x1aef4/0x1dde1/0x3d1b5
        //   三个分派器**不执行** ⇒ 这三个字段永不被使用 ⇒ 只观测。
        //   （本机是否非空未测：R278 U1；若为 0 而此处仍强制，会白废一轮 Q′。）
        if (!gQp) {
            if (!nredKernelPtrOk(reinterpret_cast<const void*>(e88)) ||
                !nredKernelPtrOk(reinterpret_cast<const void*>(e90)) ||
                !nredKernelPtrOk(reinterpret_cast<const void*>(e98))) {
                nredStage1GfMark(5, 27); return false;
            }
        }
    }
    return true;
}

__attribute__((noinline))
static bool nredStage1Guard6(const void* const ctx)
{
    gSt1G6P = 0; gSt1G6Key = 0;
    if (!nredKernelPtrOk(ctx)) { nredStage1GfMark(6, 1); return false; }
    const UInt8* q = *reinterpret_cast<const UInt8* const*>(static_cast<const UInt8*>(ctx) + 0x38);
    if (!nredKernelPtrOk(q)) { nredStage1GfMark(6, 2); return false; }
    gSt1G6P = reinterpret_cast<UInt64>(q);
    if (*reinterpret_cast<const UInt64*>(q) == 0) { nredStage1GfMark(6, 3); return false; }
    q = *reinterpret_cast<const UInt8* const*>(q);                 // dev=*(P6)
    if (!nredKernelPtrOk(q)) { nredStage1GfMark(6, 4); return false; }
    q = *reinterpret_cast<const UInt8* const*>(q);                 // 0x8b4bf 实参 = *(dev)
    if (!nredKernelPtrOk(q)) { nredStage1GfMark(6, 5); return false; }
    q = *reinterpret_cast<const UInt8* const*>(q);                 // 0x8b4bf 内部：*(实参)
    if (!nredKernelPtrOk(q)) { nredStage1GfMark(6, 6); return false; }
    q = *reinterpret_cast<const UInt8* const*>(q + 0x28);          // prov（苹果不判，缺失必 trap）
    if (!nredKernelPtrOk(q)) { nredStage1GfMark(6, 7); return false; }
    gSt1G6Key = reinterpret_cast<UInt64>(q);
    const UInt8* const p6 = reinterpret_cast<const UInt8*>(gSt1G6P);
    if (*reinterpret_cast<const UInt64*>(p6 + 0x18) == 0) { nredStage1GfMark(6, 8); return false; }
    if (*reinterpret_cast<const UInt64*>(p6 + 0x40) == 0) { nredStage1GfMark(6, 9); return false; }
    return true;
}

// ─── 批次 P4／P4-1：client 8（SMU）的 stage1 **观测**护栏（`-NRedSt1G8`，默认关）──────────
//  分派路径（批A §4）：0x929ac → 0x9440f(ctx,8,1) → 0x94c38[7]=0x945dd → 0x9e430(ctx)
//    → P8=*(ctx+0x48) → 0x94d94[1]=0x94ae7 → jmp 0x9e8bb
//  3 条必需判据（批A §7 P0）：① P8 KPtr ② *(P8+0x18)≠0 ③ *(P8+0x28)≠0
//  2 条观测（**不参与判定**）：④ *(SMU+0x6c8) ⑤ *(SMU+0x6d8)，SMU=*(P8+0x28)
//  ⚠ **3 条判据性质不同，必须逐条区分**（client8 叶的自判情况）：
//    0x9e8bb TEST RDI,RDI ; 0x9e8be JE 0x9e91c   ⇒ 只判 **null**，**不判 KPtr**
//    0x9e8e5 MOV RCX,[RBX+0x18] ; 0x9e8f9 TEST RCX,RCX ; 0x9e8fc JE 0x9e939  ⇒ 已判非 0
//    0x9e8fe MOV RDI,[RBX+0x28] ; 0x9e902 TEST RDI,RDI ; 0x9e905 JE 0x9e939  ⇒ 已判非 0
//    ⇒ 判据 ①（P8 KPtr）是**真防护**：叶的 null 检查拦不住垃圾指针，
//      而 0x9e8e5 会读 P8+0x18 ⇒ 垃圾 P8 会 trap。
//    ⇒ 判据 ②③ 是**观测**：叶自己已判非 0，护栏不提供额外防护。
//  ⚠ 本函数只返回"判据是否全过"；**放行由 case 8 的 `return false` 恒否决**。
//  ⚠ 观测 ④⑤ 是唯一"额外读"：SMU 至少 0x6D0 字节（苹果 0x67423 读 p+0x6C8），
//    但 +0x6D8 的边界未离线证明 ⇒ 只在 `-NRedSt1G8` 开时读（默认关）。
__attribute__((noinline))
static bool nredStage1Guard8(const void* const ctx)
{
    gSt1G8P = 0; gSt1G8F18 = 0; gSt1G8F28 = 0; gSt1G8V6C8 = 0; gSt1G8V6D8 = 0;
    if (!nredKernelPtrOk(ctx)) { nredStage1GfMark(8, 1); return false; }
    const UInt8* const p8 = *reinterpret_cast<const UInt8* const*>(
        static_cast<const UInt8*>(ctx) + 0x48);
    if (!nredKernelPtrOk(p8)) { nredStage1GfMark(8, 2); return false; }
    gSt1G8P = reinterpret_cast<UInt64>(p8);
    const UInt64 f18 = *reinterpret_cast<const UInt64*>(p8 + 0x18);
    gSt1G8F18 = f18;
    if (f18 == 0) { nredStage1GfMark(8, 3); return false; }
    const UInt64 f28 = *reinterpret_cast<const UInt64*>(p8 + 0x28);
    gSt1G8F28 = f28;
    if (f28 == 0) { nredStage1GfMark(8, 4); return false; }
    // 观测（不参与判定）
    if (nredKernelPtrOk(reinterpret_cast<const void*>(f28))) {
        const UInt8* const smu = reinterpret_cast<const UInt8*>(f28);
        gSt1G8V6C8 = *reinterpret_cast<const UInt64*>(smu + 0x6c8);
        gSt1G8V6D8 = *reinterpret_cast<const UInt64*>(smu + 0x6d8);
    }
    return true;
}

__attribute__((noinline))
static bool nredStage1Guard7(const void* const ctx)
{
    gSt1P7 = 0;
    gSt1P3 = 0;
    if (!nredKernelPtrOk(ctx)) { nredStage1GfMark(7, 1); return false; }
    const UInt8* const p7 = *reinterpret_cast<const UInt8* const*>(static_cast<const UInt8*>(ctx) + 0x40);
    if (!nredKernelPtrOk(p7)) { nredStage1GfMark(7, 2); return false; }
    gSt1P7 = reinterpret_cast<UInt64>(p7);
    if (*reinterpret_cast<const UInt64*>(p7 + 0x18) == 0) { nredStage1GfMark(7, 3); return false; }
    if (*reinterpret_cast<const UInt64*>(p7 + 0x28) == 0) { nredStage1GfMark(7, 4); return false; }
    const UInt8* const p3 = *reinterpret_cast<const UInt8* const*>(static_cast<const UInt8*>(ctx) + 0x20);
    gSt1P3 = reinterpret_cast<UInt64>(p3);
    if (!nredKernelPtrOk(p3)) { nredStage1GfMark(7, 5); return false; }
    return true;
}

__attribute__((noinline))
static bool nredStage1Guard9(const void* const ctx)
{
    gSt1G9P = 0; gSt1G9Key = 0;
    gSt1P3 = 0;   // 批次 P4：与 guard7 一致（见 guard2 入口注释）
    if (!nredKernelPtrOk(ctx)) { nredStage1GfMark(9, 1); return false; }
    const UInt8* q = *reinterpret_cast<const UInt8* const*>(static_cast<const UInt8*>(ctx) + 0x50);
    if (!nredKernelPtrOk(q)) { nredStage1GfMark(9, 2); return false; }
    gSt1G9P = reinterpret_cast<UInt64>(q);
    q = *reinterpret_cast<const UInt8* const*>(q);                 // *(P9)
    if (!nredKernelPtrOk(q)) { nredStage1GfMark(9, 3); return false; }
    gSt1G9Key = reinterpret_cast<UInt64>(q);
    if (*reinterpret_cast<const UInt64*>(q + 0x20) == 0) { nredStage1GfMark(9, 4); return false; }
    const UInt8* const p9 = reinterpret_cast<const UInt8*>(gSt1G9P);
    if (*reinterpret_cast<const UInt64*>(p9 + 0x18) == 0) { nredStage1GfMark(9, 5); return false; }
    if (*reinterpret_cast<const UInt64*>(p9 + 0x28) != 0 &&
        *reinterpret_cast<const UInt64*>(p9 + 0x30) == 0) { nredStage1GfMark(9, 6); return false; }
    // 批次 P4／P4-2：P3 = *(ctx+0x20) 必须非空（同 guard2 步 8 的理由）。
    //   ⚠ P3 当前＝0 ⇒ 本检查生效后 client9 恒 fail-closed（**预期**）。
    const UInt8* const p3 = *reinterpret_cast<const UInt8* const*>(
        static_cast<const UInt8*>(ctx) + 0x20);
    gSt1P3 = reinterpret_cast<UInt64>(p3);
    if (!nredKernelPtrOk(p3)) { nredStage1GfMark(9, 7); return false; }
    return true;
}

__attribute__((noinline))
static bool nredStage1Guard14(const void* const ctx);   // 定义在后（264：通过即放行）

// ─── 259/264：各 client 的 guard 分派（白名单 {2,3,5,6,7,9,14}；8 仍 fail-closed）────────────
static bool nredStage1Guard(const void* const ctx, const UInt32 client)
{
    bool ok = false;
    switch (client) {
        case 2: ok = nredStage1Guard2(ctx); break;
        case 3: ok = nredStage1Guard3(ctx); break;
        case 5: {
            // 批次 P／P-7：判定与放行解耦——照常跑完全部检查并记录读数。
            // 批次 Q′：`-NRedQp` 开 **且截断器已武装** ⇒ 放行叶子（第 14 步由 0x1936e 截断器
            //          拦下，零 MMIO）；否则恒不放行（批次 P 的零行为形态）。
            //   ⚠ `gQpArmed` 不可省：若 hook 注册失败而仍放行，叶子第 14 步会执行**真实的**
            //     0x1936e（真实 MMIO）——那正是 Q′ 要避免的形态。
            //  背景：nredStage1Guard 返回 true ⇒ 本文件下方的 `if (gStage1 && c == 1)` 分支
            //  立即调用 orgMode9440f ⇒ 苹果叶执行 ⇒ 叶内 0x1936e 做真实 MMIO（M1 §2.3）。
            const bool v = nredStage1Guard5(ctx);
            gSt1G5Verdict = v ? 1U : 0U;
            ok = (gQp && gQpArmed) ? v : false;
            break;
        }
        case 6: ok = nredStage1Guard6(ctx); break;
        case 7: ok = nredStage1Guard7(ctx); break;
        case 8: {
            // 批次 P4／P4-1：client 8（SMU）的**观测护栏**。
            //   ⚠ 恒不放行：client8 的 MMIO 经运行期函数指针（*(*(h+8)+0x40/+0xC0)），
            //     不可离线证明有界（R279 §5 建议 3）⇒ 本批只观测。
            //   ⚠ `-NRedSt1G8` 默认关：`nredStage1GfMark` 只记**首个**失败，
            //     client8 的失败会遮蔽 client5 的 `c=5`（r279 的既有形态）。
            //   ⚠ 直接 `return false` 而不走 `ok`：既不放行，也不置 `gSt1Try` 的 bit8。
            if (!gSt1G8) { return false; }
            const bool v8 = nredStage1Guard8(ctx);
            gSt1G8Verdict = v8 ? 1U : 0U;
            return false;
        }
        case 9: ok = nredStage1Guard9(ctx); break;
        case 14: ok = nredStage1Guard14(ctx); break;
        default: return false;   // 8（暂缓）及其它 client：继续 fail-closed
    }
    if (!ok) { return false; }
    gSt1Try |= (1U << client);
    return true;
}

// 14（MES）的 5 条前置；全过 ⇒ 放行（264：0x7b0 的 5 处读者均为搬运/判空，无解引用）。
__attribute__((noinline))
static bool nredStage1Guard14(const void* const ctx)
{
    gSt1Mes = 0;
    gSt1M8 = 0;
    gSt1M790 = 0;
    gSt1G14P = 0;
    if (!nredKernelPtrOk(ctx)) { nredStage1GfMark(14, 1); return false; }
    const UInt8* const p14 = *reinterpret_cast<const UInt8* const*>(static_cast<const UInt8*>(ctx) + 0x78);
    if (!nredKernelPtrOk(p14)) { nredStage1GfMark(14, 2); return false; }
    gSt1G14P = reinterpret_cast<UInt64>(p14);
    if (*reinterpret_cast<const UInt64*>(p14 + 0x18) == 0) { nredStage1GfMark(14, 3); return false; }
    const UInt8* const mes = *reinterpret_cast<const UInt8* const*>(p14 + 0x28);   // ③
    if (!nredKernelPtrOk(mes)) { nredStage1GfMark(14, 4); return false; }
    gSt1Mes = reinterpret_cast<UInt64>(mes);
    gSt1M8 = *reinterpret_cast<const UInt64*>(mes + 0x08);      // ④ G4
    gSt1M790 = *reinterpret_cast<const UInt64*>(mes + 0x790);   // ⑤ G5
    if (gSt1M8 == 0) { nredStage1GfMark(14, 5); return false; }
    if (gSt1M790 == 0) { nredStage1GfMark(14, 6); return false; }
    return true;
}

// ─── 246：client 7 只读探针（`-NRedSt1W`，默认关；不放行、不调原件）──────────────────────
//  规格 `tmp/tech-manager/R-client7只读探针规格-2026-10-21.md` §1/§2。
//  链：ctx → P7=*(ctx+0x40) → handle=*(P7+0x28) → svc=*(handle+8)；每级先 `nredKernelPtrOk`，
//  失败即记 0 并停止（不再解引用）。本函数不写非 BSS 内存、不跟随函数指针、无等待/轮询。
//  247：同页边界保护——`nredKernelPtrOk` 只校验高半区，不保证映射/长度。
//    ① `pageOff > 0xED8`（9 槽整段会越页）⇒ 置 `gSt1SkipHigh=1`，**高位槽整段记 0**；
//    ② 其余槽仍逐个按 `pageOff + off + 8 <= 0x1000` 判断（`pageOff>0xFB8` 时低槽也会越页）。
//  槽读取独立成 noinline 帧，保持探针本体栈帧不变。
__attribute__((noinline))
static void nredStage1WSlots(const UInt8* const svc)
{
    const UInt64 pageOff = reinterpret_cast<UInt64>(svc) & 0xFFFULL;
    if (pageOff > 0xED8ULL) { gSt1SkipHigh = 1; }
    static const UInt32 kOff[9] = {0x10, 0x18, 0x20, 0x38, 0x40, 0x90, 0x100, 0x118, 0x120};
    for (unsigned k = 0; k < 9; ++k) {
        if (gSt1SkipHigh != 0 && k >= 5) { continue; }   // 规格：高位槽整段跳过
        if (pageOff + kOff[k] + 8ULL <= 0x1000ULL) {
            gSt1S[k] = *reinterpret_cast<const UInt64*>(svc + kOff[k]);
        }
    }
}

static void nredStage1WProbe(const void* const ctx)
{
    gSt1P7 = 0;
    gSt1H = 0;
    gSt1Svc = 0;
    gSt1SkipHigh = 0;
    for (unsigned k = 0; k < 9; ++k) { gSt1S[k] = 0; }
    if (!nredKernelPtrOk(ctx)) { return; }
    const UInt8* const p7 = *reinterpret_cast<const UInt8* const*>(static_cast<const UInt8*>(ctx) + 0x40);
    if (!nredKernelPtrOk(p7)) { return; }
    gSt1P7 = reinterpret_cast<UInt64>(p7);
    const UInt8* const h = *reinterpret_cast<const UInt8* const*>(p7 + 0x28);
    if (!nredKernelPtrOk(h)) { return; }
    gSt1H = reinterpret_cast<UInt64>(h);
    const UInt8* const svc = *reinterpret_cast<const UInt8* const*>(h + 8);
    if (!nredKernelPtrOk(svc)) { return; }
    gSt1Svc = reinterpret_cast<UInt64>(svc);
    if (gSt1W2) { nredStage1WSlots(svc); }   // 248：仅 W2 才 deref svc
}
// ─── 249：client 一级对象对齐观测（`-NRedSt1O`，默认关；仅 c==1）───────────────────────
//  位置：wrapMode9440f 入口、护栏之前。`a` 通过 KPtr 后读**一次** `*(a+8+8i)`（原样保存，
//  不做 KPtr 过滤），不 deref `p` 本身；每 client「首次」+「p 变化」各一条（≤2 条），seq 饱和 255。
static void nredStage1ORecord(const void* const a, const UInt32 client)
{
    static const UInt8 kIds[7] = {2, 5, 6, 7, 8, 9, 14};
    unsigned idx = 7;
    for (unsigned k = 0; k < 7; ++k) { if (kIds[k] == client) { idx = k; break; } }
    if (idx == 7) { return; }
    if (gSt1OSeq[idx] < 255) { ++gSt1OSeq[idx]; }
    const bool aok = nredKernelPtrOk(a);
    const UInt64 p = aok
        ? reinterpret_cast<UInt64>(*reinterpret_cast<void* const*>(static_cast<const UInt8*>(a) + 8 + client * 8u))
        : 0;
    NRedSt1ORec& r0 = gSt1ORec[idx][0];
    if (r0.used == 0) {
        r0.ctx = reinterpret_cast<UInt64>(a); r0.p = p; r0.seq = gSt1OSeq[idx]; r0.used = 1;
        return;
    }
    if (r0.p == p) { return; }
    NRedSt1ORec& r1 = gSt1ORec[idx][1];
    if (r1.used != 0) { return; }   // 最多 2 条：后续变化不再记录
    r1.ctx = reinterpret_cast<UInt64>(a); r1.p = p; r1.seq = gSt1OSeq[idx]; r1.used = 1;
}
// ─── 252：`[st1b]` 原始 `(b,c)` 调次（`-NRedSt1B`，默认关）────────────────────────────
//  入口、护栏之前；前 8 次调用（不论 b/c）各记 1 条；全局 seq 饱和 255；零解引用、只写 BSS。
static void nredStage1BRecord(const UInt32 b, const UInt32 c)
{
    if (gSt1BSeq < 255) { ++gSt1BSeq; }
    if (gSt1BN >= 8) { return; }
    NRedSt1BRec& r = gSt1BRec[gSt1BN];
    r.b = b;
    r.c = c;
    r.seq = gSt1BSeq;
    ++gSt1BN;
}
// ─── 256：`[st1c]` 仅 `c>=1` 的调次（`-NRedSt1C`，默认关）──────────────────────────────
//  入口最前、先于其它记录；仅 c>=1 才记，最多 8 条；seq 全局递增（饱和 255）；零解引用。
static void nredStage1CRecord(const UInt32 b, const UInt32 c)
{
    if (gSt1CSeq < 255) { ++gSt1CSeq; }
    if (gSt1CN >= 8) { return; }
    NRedSt1CRec& r = gSt1CRec[gSt1CN];
    r.b = b;
    r.c = c;
    r.seq = gSt1CSeq;
    ++gSt1CN;
}
// ─── 257：`[st1p]` client 3 stage1 时刻的 P3 观测（`-NRedSt1P`，默认关）─────────────────
//  仅在 c==1 && b==3 且尚未记录时执行一次：ctx 过 KPtr 后读 P3=*(ctx+0x20)（原样保存）；
//  P3 再过 KPtr 才读 +0x18/+0x28。只写 BSS、零解引用副作用、不跟随函数指针。
__attribute__((noinline))
static void nredStage1PRecord(const void* const ctx)
{
    gSt1PDone = true;
    gSt1P3Obs = 0;
    gSt1P3V18 = 0;
    gSt1P3V28 = 0;
    if (!nredKernelPtrOk(ctx)) { return; }
    const UInt8* const p3 = *reinterpret_cast<const UInt8* const*>(static_cast<const UInt8*>(ctx) + 0x20);
    gSt1P3Obs = reinterpret_cast<UInt64>(p3);
    if (!nredKernelPtrOk(p3)) { return; }
    gSt1P3V18 = *reinterpret_cast<const UInt64*>(p3 + 0x18);
    gSt1P3V28 = *reinterpret_cast<const UInt64*>(p3 + 0x28);
}


// ─── 261：client5 GVM provider type / 注册表探针（`-NRedGvm5`，默认关；只读一次）──────────
//  链：ctx → P5 → H=*(P5+0x28) → T=*(H+0xE48) → A=*(T+0x08)；逐级 KPtr。
//  reg 复刻 0xa31f6（不调用 0x8b8e7）：
//    IP=*(A)；iproot=*(*(IP)+0x58)（＝0x8b8e7(IP)）；
//    head=*(void**)(iproot+232) → blk=*(void**)(head+64) → arr=*(void**)(blk+8)；
//    固定最多 82 项（0xa3228: rdx=8..0x518 步长 16），键=*(u32*)(arr+i*16)；
//    键==0 ⇒ 停止（未命中）；键==type ⇒ 命中。
//  IP==0 或 iproot==0 ⇒ reg=0；其余环节非法 ⇒ reg=2（无法判定）。
__attribute__((noinline))
static void nredGvm5Probe(const void* const ctx)
{
    gGvm5Seen = true;
    gGvm5Type = 0; gGvm5A0 = 0; gGvm5A58 = 0; gGvm5Iproot = 0; gGvm5Reg = 2;
    if (!nredKernelPtrOk(ctx)) { return; }
    const UInt8* q = *reinterpret_cast<const UInt8* const*>(static_cast<const UInt8*>(ctx) + 0x30);
    if (!nredKernelPtrOk(q)) { return; }
    q = *reinterpret_cast<const UInt8* const*>(q + 0x28);          // H
    if (!nredKernelPtrOk(q)) { return; }
    q = *reinterpret_cast<const UInt8* const*>(q + 0xE48);         // T
    if (!nredKernelPtrOk(q)) { return; }
    q = *reinterpret_cast<const UInt8* const*>(q + 0x08);          // A
    if (!nredKernelPtrOk(q)) { return; }
    gGvm5Type = *reinterpret_cast<const UInt32*>(q);
    gGvm5A0 = *reinterpret_cast<const UInt64*>(q);
    gGvm5A58 = *reinterpret_cast<const UInt64*>(q + 0x58);
    q = reinterpret_cast<const UInt8*>(gGvm5A0);                   // IP=*(A)（＝h8）
    if (q == nullptr) { gGvm5Reg = 0; return; }
    if (!nredKernelPtrOk(q)) { return; }
    // 批次 P2／P2-3：iproot = *(*(IP)+0x58)（＝0x8b8e7(IP)），与护栏步 14 统一。
    //   ⚠ 原写法 `*(IP+0x58)` 是**越界读**：IP 是 8 B 句柄（0x8b5de 经 0x90ddc(…,8,…)），
    //     `+0x58` 在对象外 88 B。r271 实测读到 `ffffff801155cc00`（内核镜像区）；
    //     r278 读到 `ffffff800f55fda0`——那是 `nredIpRInstall` 越界写进去的 `&gIpRoot`（自证伪）。
    {
        const UInt8* const root = *reinterpret_cast<const UInt8* const*>(q);   // root=*(IP)
        if (!nredKernelPtrOk(root)) { return; }
        q = *reinterpret_cast<const UInt8* const*>(root + 0x58);   // iproot=*(root+0x58)
        if (q == nullptr) { gGvm5Reg = 0; return; }
        if (!nredKernelPtrOk(q)) { return; }
    }
    gGvm5Iproot = reinterpret_cast<UInt64>(q);
    q = *reinterpret_cast<const UInt8* const*>(q + 232);           // head
    if (!nredKernelPtrOk(q)) { return; }
    q = *reinterpret_cast<const UInt8* const*>(q + 64);            // blk
    if (!nredKernelPtrOk(q)) { return; }
    q = *reinterpret_cast<const UInt8* const*>(q + 8);             // arr
    if (!nredKernelPtrOk(q)) { return; }
    for (UInt32 i = 0; i < 82; ++i) {
        const UInt32 key = *reinterpret_cast<const UInt32*>(q + i * 16);
        if (key == 0) { gGvm5Reg = 0; return; }
        if (key == gGvm5Type) { gGvm5Reg = 1; return; }
    }
    gGvm5Reg = 0;
}

// ─── 262：client5 golden 6 槽探针（`-NRedGSlot`，默认关；只读一次）────────────────────
//  链：ctx → P5=*(ctx+0x30) → H=*(P5+0x28)；6 槽地址 H+0x008+k*0x260（k=0..5）。
//  每槽：type=*(u32*)s、nInst=*(u32*)(s+4)、base[i]=*(u32*)(s+0x2C+4i)（i<min(n,3)）。
//  H 有效即读槽；(H+0xE48) 的 T 与 *(T+0x08) 的 A 仅用于头部对齐，无效则记 0。
__attribute__((noinline))
static void nredGSlotProbe(const void* const ctx)
{
    gGSlotSeen = true;
    gGSlotH = 0; gGSlotT = 0; gGSlotA = 0;
    for (unsigned k = 0; k < 6; ++k) {
        gGSlotRec[k].type = 0; gGSlotRec[k].n = 0;
        gGSlotRec[k].b0 = 0; gGSlotRec[k].b1 = 0; gGSlotRec[k].b2 = 0;
    }
    if (!nredKernelPtrOk(ctx)) { return; }
    const UInt8* q = *reinterpret_cast<const UInt8* const*>(static_cast<const UInt8*>(ctx) + 0x30);
    if (!nredKernelPtrOk(q)) { return; }
    q = *reinterpret_cast<const UInt8* const*>(q + 0x28);          // H
    if (!nredKernelPtrOk(q)) { return; }
    gGSlotH = reinterpret_cast<UInt64>(q);
    q = *reinterpret_cast<const UInt8* const*>(q + 0xE48);         // T
    if (nredKernelPtrOk(q)) {
        gGSlotT = reinterpret_cast<UInt64>(q);
        q = *reinterpret_cast<const UInt8* const*>(q + 0x08);      // A
        if (nredKernelPtrOk(q)) { gGSlotA = reinterpret_cast<UInt64>(q); }
    }
    q = reinterpret_cast<const UInt8*>(gGSlotH);
    for (unsigned k = 0; k < 6; ++k) {
        const UInt32 type = *reinterpret_cast<const UInt32*>(q + 8 + k * 0x260);
        const UInt32 cnt = *reinterpret_cast<const UInt32*>(q + 8 + k * 0x260 + 4);
        gGSlotRec[k].type = type;
        gGSlotRec[k].n = cnt;
        const UInt32 cap = (cnt > 3) ? 3 : cnt;
        if (cap > 0) { gGSlotRec[k].b0 = *reinterpret_cast<const UInt32*>(q + 8 + k * 0x260 + 0x2C); }
        if (cap > 1) { gGSlotRec[k].b1 = *reinterpret_cast<const UInt32*>(q + 8 + k * 0x260 + 0x30); }
        if (cap > 2) { gGSlotRec[k].b2 = *reinterpret_cast<const UInt32*>(q + 8 + k * 0x260 + 0x34); }
    }
    // 批次 P2／P2-4：golden armed 位掩码。`0x18f7c` 依次查 type 70／27／34／28（W3 §2）；
    //   四位全置 ⇒ golden 路径已 armed（`0x18f7c` 一定会进 MMIO 循环）。
    //   纯 BSS 计算，零解引用。
    {
        static const UInt32 kGolden[4] = {70, 27, 34, 28};
        UInt32 mask = 0;
        for (unsigned g = 0; g < 4; ++g) {
            for (unsigned k = 0; k < 6; ++k) {
                if (gGSlotRec[k].type == kGolden[g]) { mask |= (1U << g); break; }
            }
        }
        gGSlotGolden = mask;
    }
}

// ─── 263／批次 P2：client5 的 IP 根**只读**探针（`-NRedIpR`，默认关）────────────────────
//  链：ctx → P5=*(ctx+0x30) → H=*(P5+0x28) → T=*(H+0xE48) → A=*(T+8) → IP=*(A)（＝h8）
//      → root=*(IP) → iproot=*(root+0x58)（＝0x8b8e7(IP)）
//  ⚠ 批次 P2／P2-1：**已删除**原 `*(q+0x58) = &gIpRoot` 写入。理由：
//     q ＝ IP ＝ h8 是 `0x8b5de` 经 `0x90ddc(…, 8, …)` 分配的 **8 B 句柄**，
//     `+0x58` 在对象外 88 B ⇒ 越界写，覆盖相邻分配（与方案 C 同类，W2 §6.3 已判必须撤回）。
//     r278 实测 `[ipr] ret=0` ⇒ 该写已真实发生。
//  ⚠ 批次 P2／P2-3：iproot 表达式从 `*(IP+0x58)`（越界读）改为 `*(*(IP)+0x58)`，
//     与批次 P 修正后的护栏步 14 统一。
//  ⚠ 本函数**不写任何内存**（除自身 BSS）。
__attribute__((noinline))
static void nredIpRInstall(const void* const ctx)
{
    gIpRIp = 0;
    gIpRRoot = 0;
    gIpRRet = 0xFF;
    if (!nredKernelPtrOk(ctx)) { gIpRRet = 1; return; }
    const UInt8* q = *reinterpret_cast<const UInt8* const*>(static_cast<const UInt8*>(ctx) + 0x30);
    if (!nredKernelPtrOk(q)) { gIpRRet = 2; return; }
    q = *reinterpret_cast<const UInt8* const*>(q + 0x28);          // H
    if (!nredKernelPtrOk(q)) { gIpRRet = 3; return; }
    q = *reinterpret_cast<const UInt8* const*>(q + 0xE48);         // T
    if (!nredKernelPtrOk(q)) { gIpRRet = 4; return; }
    q = *reinterpret_cast<const UInt8* const*>(q + 0x08);          // A
    if (!nredKernelPtrOk(q)) { gIpRRet = 5; return; }
    q = *reinterpret_cast<const UInt8* const*>(q);                 // IP=*(A)（＝h8）
    if (!nredKernelPtrOk(q)) { gIpRRet = 6; return; }
    gIpRIp = reinterpret_cast<UInt64>(q);
    const UInt8* const root = *reinterpret_cast<const UInt8* const*>(q);   // root=*(IP)
    if (!nredKernelPtrOk(root)) { gIpRRet = 7; return; }
    const UInt8* const iproot = *reinterpret_cast<const UInt8* const*>(root + 0x58);
    if (!nredKernelPtrOk(iproot)) { gIpRRet = 8; return; }
    gIpRRoot = reinterpret_cast<UInt64>(iproot);
    gIpRRet = 0;   // 只读探针成功（批次 P2 起不再表示"写入成功"）
}

// ─── 265：client5 的 qq 对象四字段探针（`-NRedSt1Q`，默认关；只读一次）──────────────────
//  链：ctx → p=*(ctx) → qq=*(p)；任一级 KPtr 失败 ⇒ 值全 0 且 f=1。
//  四字段：q0=*(u64*)qq、q8=*(qq+8)、q10=*(qq+0x10)、q18=*(qq+0x18)。
__attribute__((noinline))
static void nredSt1QQProbe(const void* const ctx)
{
    gSt1QQSeen = true;
    gSt1QQ0 = 0; gSt1QQ8 = 0; gSt1QQ10 = 0; gSt1QQ18 = 0;
    gSt1QQF = 1;
    if (!nredKernelPtrOk(ctx)) { return; }
    const UInt8* q = *reinterpret_cast<const UInt8* const*>(static_cast<const UInt8*>(ctx));
    if (!nredKernelPtrOk(q)) { return; }
    q = *reinterpret_cast<const UInt8* const*>(q);
    if (!nredKernelPtrOk(q)) { return; }
    gSt1QQ0 = *reinterpret_cast<const UInt64*>(q);
    gSt1QQ8 = *reinterpret_cast<const UInt64*>(q + 8);
    gSt1QQ10 = *reinterpret_cast<const UInt64*>(q + 0x10);
    gSt1QQ18 = *reinterpret_cast<const UInt64*>(q + 0x18);
    gSt1QQF = 0;
}

// ─── 268：client5 的 qq 句柄层（`-NRedQqProbe` 只读；`-NRedQqHdl` 安装；均默认关）──────────
//  只读探针：ctx→p=*(ctx)→qq=*(p)，**先确认 *(qq+0x10)==qq（0x228 表）**，之后才读
//  q8/slot1 与四值；若 qq 不是表（例如 8B 句柄），只记录 qq 后立即返回。零写入。
__attribute__((noinline))
static void nredQqProbe(const void* const ctx)
{
    gQqProbeSeen = true;
    gQqP = 0; gQqQ = 0; gQqQ8 = 0; gQqS1 = 0;
    gQqV70 = 0; gQqV258 = 0; gQqV268 = 0; gQqV2B8 = 0;
    if (!nredKernelPtrOk(ctx)) { return; }
    const UInt8* q = *reinterpret_cast<const UInt8* const*>(static_cast<const UInt8*>(ctx));
    if (!nredKernelPtrOk(q)) { return; }
    gQqP = reinterpret_cast<UInt64>(q);
    q = *reinterpret_cast<const UInt8* const*>(q);
    if (!nredKernelPtrOk(q)) { return; }
    gQqQ = reinterpret_cast<UInt64>(q);
    if (*reinterpret_cast<const UInt64*>(q + 0x10) != reinterpret_cast<UInt64>(q)) { return; }
    gQqQ8 = *reinterpret_cast<const UInt64*>(q + 8);
    q = *reinterpret_cast<const UInt8* const*>(q + 0x28);
    if (!nredKernelPtrOk(q)) { return; }
    gQqS1 = reinterpret_cast<UInt64>(q);
    gQqV70 = *reinterpret_cast<const UInt64*>(q + 0x70);
    gQqV258 = *reinterpret_cast<const UInt64*>(q + 0x258);
    gQqV268 = *reinterpret_cast<const UInt64*>(q + 0x268);
    gQqV2B8 = *reinterpret_cast<const UInt64*>(q + 0x2b8);
}

//  安装（决策 C）：7 条 fail-closed 全满足才建 8B 句柄（*h=qq）并改写 *(p) 一处。
//  ⚠ 规格写的 `nredKernelMalloc` 在本仓库不存在 ⇒ 用 static 8B 对象（生命周期＝kext）。
//  批次 P（P-1）：**写入分支已撤回**（见函数尾注释）——本函数现在只做只读探针，
//  `gQqHdlRet=0` 表示"只读探针全部通过"，不再表示"已改写 *(p)"。
__attribute__((noinline))
static bool nredQqHdlInstall(const void* const ctx)
{
    gQqHdlRet = 0xFF;
    if (!nredKernelPtrOk(ctx)) { gQqHdlRet = 1; return false; }
    const UInt8* const p = *reinterpret_cast<const UInt8* const*>(static_cast<const UInt8*>(ctx));
    if (!nredKernelPtrOk(p)) { gQqHdlRet = 2; return false; }
    const UInt8* qq = *reinterpret_cast<const UInt8* const*>(p);
    if (!nredKernelPtrOk(qq)) { gQqHdlRet = 3; return false; }
    gQqP = reinterpret_cast<UInt64>(p);
    gQqQ = reinterpret_cast<UInt64>(qq);
    // 先读任何对象都安全的首 8B：非 0 ⇒ 已补过/非预期，直接拒绝（避免对短对象读 +0x10）
    if (*reinterpret_cast<const UInt64*>(qq) != 0) { gQqHdlRet = 5; return false; }
    if (*reinterpret_cast<const UInt64*>(qq + 0x10) != reinterpret_cast<UInt64>(qq)) {
        gQqHdlRet = 4; return false;                                  // 非 C1 表
    }
    const UInt8* const slot1 = *reinterpret_cast<const UInt8* const*>(qq + 0x28);
    if (!nredKernelPtrOk(slot1)) { gQqHdlRet = 6; return false; }
    gQqS1 = reinterpret_cast<UInt64>(slot1);
    gQqQ8 = *reinterpret_cast<const UInt64*>(qq + 8);
    gQqV70 = *reinterpret_cast<const UInt64*>(slot1 + 0x70);
    gQqV258 = *reinterpret_cast<const UInt64*>(slot1 + 0x258);
    gQqV268 = *reinterpret_cast<const UInt64*>(slot1 + 0x268);
    gQqV2B8 = *reinterpret_cast<const UInt64*>(slot1 + 0x2b8);
    if (!nredTextPrologue(gQqV70)) { gQqHdlRet = 7; return false; }
    if (gQqV258 == 0 || !nredKernelPtrOk(reinterpret_cast<const UInt8*>(gQqV258))) { gQqHdlRet = 8; return false; }
    if (gQqV268 != 0 && !nredTextPrologue(gQqV268)) { gQqHdlRet = 8; return false; }
    if (gQqV2B8 != 0 && !nredTextPrologue(gQqV2B8)) { gQqHdlRet = 8; return false; }
    if (!gSt1G5Step24) { gQqHdlRet = 9; return false; }
    // 批次 P（P-1）：撤回方案 C 的写入分支。
    //   理由（W2 §6.3）：C 的写入点 p ＝ *(ctx) ＝ h8；写后 0x8b4bf(h8) 变成 *(h+0x28)
    //   （h 为 8 B 静态对象）＝越界读；更严重的是它会让护栏步 22 由"失败"变"通过"
    //   ⇒ 同时解除护栏。
    //   ⇒ 只保留只读探针。gQqHdlRet=0 的语义改为"只读探针全部通过"（不再是"写入成功"）。
    gQqHdlOld = *reinterpret_cast<const UInt64*>(p);   // 只读：记录原值供 [qq] 观测
    gQqHdlRet = 0;
    return true;
}
// ─── 274：client5 stage1 入口只读探针（`-NRedGvm5Phase`，默认关）──────────────────────────────
//  目的：回答 D1——`start` 是否走到 client5（`[gvm5p] n=1` 出现即成立）。
//  ⚠ 能力边界（必须保留）：本探针**只能**回答 D1；D2（槽 2/6 失败归属）需 hook
//    `AmdPowerPlayHelper::powerUp`，D3（`self+0x100/+0x108/+0x28F8/+0x28F9` 语义）需
//    `AmdPowerPlayHelper::self`——我方在 `wrapMode9440f` 只有 `ctx`，两者均拿不到。
//  纪律：只写 BSS；不写文件、不 SYSLOG、不写寄存器；所有指针先 KPtr；不执行 D。
__attribute__((noinline))
static void nredGvm5Phase(const void* const a, const void* const ra)
{
    if (gGvm5PhaseN < 0xFFFFFFFFU) { ++gGvm5PhaseN; }
    gGvm5PhaseA = reinterpret_cast<UInt64>(a);
    gGvm5PhaseP1 = 0;
    gGvm5PhaseRc = 0xFF;                                  // 探针不执行 D ⇒ 未执行
    const UInt64 raw = reinterpret_cast<UInt64>(ra);
    gGvm5PhaseRa = (gHwlibsSlide != 0 && raw > gHwlibsSlide) ? (raw - gHwlibsSlide) : 0;
    if (!nredKernelPtrOk(a)) { return; }
    const UInt64 p1 = *reinterpret_cast<const UInt64*>(static_cast<const UInt8*>(a) + 0x30);
    if (nredKernelPtrOk(reinterpret_cast<const void*>(p1))) { gGvm5PhaseP1 = p1; }
}

// ─── 273：client5 stage1 叶（`0x985d0`）的我方完整复刻（`-NRedGvm5D`，默认关）──────────────
//  依据：`kb/序列数据/反汇编/证据/HWLibs全量反编译/fn/985d0.c` 与 `HWLibs_full.asm` 的
//        `985d0..989a4` 段（逐字核对；16 步对照见交付件）。
//  替换点＝`wrapMode9440f` 的 dispatch 层（B1）：与 `0x94831` 的尾跳等价——先
//        `p1 = *(a+0x30)`，再进入叶。不写 `*(p)`、不做 MMIO、不新增 hook。
//  失败一律返 0（苹果语义：0＝失败）；任何指针/回调判空即 fail-closed，不 panic。
//  ⚠ 描述块在 BSS（`gGvm5DBlocks`）；本函数局部帧只含标量（≤128 B 纪律）。
//  ⚠ 步骤号写入 `gGvm5DStep`（失败定位）；观测行 `[gvm5d]` 由 `nredGvm5DStamp` 输出。
// A2：描述块非阻塞重入守卫（RAII）。重入 ⇒ 该次调用返 0（失败），不等待、不崩溃。
//  ⚠ 返回语义改变：苹果叶的栈局部描述块天然可重入；我方 BSS 单份 ⇒ 重入必须 fail-closed。
struct NRedGvm5DBusyGuard {
    bool held;
    NRedGvm5DBusyGuard() : held(OSCompareAndSwap(0, 1, &gGvm5DBusy)) {
        if (!held) { gGvm5DBusyHit = true; }
    }
    ~NRedGvm5DBusyGuard() { if (held) { gGvm5DBusy = 0; } }
};

__attribute__((noinline))
static UInt32 nredGvm5Stage1(void* const a, void* const d)
{
    (void)d;
    gGvm5DStep = 0;
    gGvm5DRet  = 0xFFFFFFFFU;
    gGvm5DP1 = 0;
    gGvm5DLocal68 = 0;
    if (gHwlibsSlide == 0 || !nredKernelPtrOk(a)) { gGvm5DRet = 0; return 0; }

    NRedGvm5DBusyGuard busyGuard;
    if (!busyGuard.held) { gGvm5DRet = 0; gGvm5DStep = 0x0E; return 0; }

    // ① p1 = *(a+0x30)（U1：`0x98050(a)` 的语义，纯读）
    gGvm5DStep = 1;
    const UInt64 p1 = *reinterpret_cast<const UInt64*>(static_cast<const UInt8*>(a) + 0x30);
    gGvm5DP1 = p1;
    if (!nredKernelPtrOk(reinterpret_cast<const void*>(p1))) { gGvm5DRet = 0; return 0; }
    const UInt8* const p1b = reinterpret_cast<const UInt8*>(p1);
    const UInt64 host = *reinterpret_cast<const UInt64*>(p1b);                 // *p1
    if (!nredKernelPtrOk(reinterpret_cast<const void*>(host))) { gGvm5DRet = 0; return 0; }
    const UInt64 dev = *reinterpret_cast<const UInt64*>(reinterpret_cast<const void*>(host)); // *(u64*)*p1
    if (!nredKernelPtrOk(reinterpret_cast<const void*>(dev))) { gGvm5DRet = 0; return 0; }

    const UInt64 s = gHwlibsSlide;
    NRedGvm5DBlocks& blk = gGvm5DBlocks;
    UInt8* const desc = blk.desc;
    UInt8* const clk  = blk.clk;
    UInt8* const type = blk.type;
    UInt8* const hub  = blk.hub;
    UInt8* const save = blk.save;

    // ② 两个描述块清零 + 首字段（`0x1c8` / `0x68`）
    gGvm5DStep = 2;
    __builtin_memset(desc, 0, 0x1c8);
    __builtin_memset(clk, 0, 0x68);
    __builtin_memset(type, 0, 24);
    *reinterpret_cast<UInt32*>(clk) = 0x68;
    *reinterpret_cast<UInt32*>(desc) = 0x1c8;

    // ③ 11 个能力位查询（只读；逐字复刻 985d0.c:63–101）
    gGvm5DStep = 3;
    const auto fn_a3542 = reinterpret_cast<UInt32 (*)(UInt64)>(s + 0xa3542);
    const auto fn_a28ad = reinterpret_cast<UInt8 (*)(UInt64, void*)>(s + 0xa28ad);
    const auto fn_a2b6c = reinterpret_cast<UInt8 (*)(UInt64, UInt32)>(s + 0xa2b6c);
    const auto fn_a32d5 = reinterpret_cast<UInt8 (*)(UInt64)>(s + 0xa32d5);
    const auto fn_a1ddd = reinterpret_cast<UInt64 (*)(UInt64)>(s + 0xa1ddd);
    const auto fn_90da8 = reinterpret_cast<UInt64 (*)()>(s + 0x90da8);
    const auto fn_a2b51 = reinterpret_cast<UInt64 (*)(UInt64)>(s + 0xa2b51);
    const auto fn_94ff7 = reinterpret_cast<UInt64 (*)(UInt64)>(s + 0x94ff7);
    const auto fn_a2811 = reinterpret_cast<UInt64 (*)(UInt64)>(s + 0xa2811);
    const auto fn_a2847 = reinterpret_cast<UInt64 (*)(UInt64)>(s + 0xa2847);
    const auto fn_a3d6c = reinterpret_cast<UInt8 (*)(UInt64, void*)>(s + 0xa3d6c);

    const UInt32 iVar5 = fn_a3542(dev);                                        // 0xa3542
    const UInt64 local50 = *reinterpret_cast<const UInt64*>(p1b + 0x18);       // p1[3]

    if (fn_a28ad(dev, type) != 0) {                                            // 0xa28ad
        // local_b0.lo = type[2].lo；local_a8 = type[1]
        *reinterpret_cast<UInt32*>(clk + 0x20) = *reinterpret_cast<const UInt32*>(type + 0x10);
        *reinterpret_cast<UInt64*>(clk + 0x28) = *reinterpret_cast<const UInt64*>(type + 0x08);
    }

    UInt32 uVar6 = 0;
    UInt32 uVar9 = 0;
    if (fn_a2b6c(dev, 1) != 0) {                                               // 0xa2b6c
        uVar6 = fn_a32d5(dev);                                                 // 0xa32d5
        uVar9 = *reinterpret_cast<const UInt32*>(clk + 4) & 0xfffffff8U;
        uVar6 = (uVar6 & 0xffU) | 2U;
    }
    uVar9 = ((iVar5 - 1U < 2U) ? 1U : 0U) + uVar6 * 2U | uVar9;
    *reinterpret_cast<UInt32*>(clk + 4) = uVar9;

    const UInt32 bVar4 = static_cast<UInt32>(fn_a1ddd(dev)) & 0xffU;           // 0xa1ddd
    *reinterpret_cast<UInt32*>(clk + 4) =
        (((*reinterpret_cast<const UInt32*>(type) & 3U) << 7) |
         ((uVar9 & 0xffff0007U) + bVar4 * 8U));

    *reinterpret_cast<UInt32*>(clk + 0x50) = static_cast<UInt32>(fn_90da8());  // 0x90da8
    const UInt64 lVar8a = fn_a2b51(dev);                                       // 0xa2b51
    const UInt64 lVar8b = fn_94ff7(host);                                      // 0x94ff7(*p1)
    *reinterpret_cast<UInt64*>(clk + 0x58) = lVar8b + lVar8a;
    *reinterpret_cast<UInt64*>(clk + 0x30) = fn_a2811(dev);                    // 0xa2811
    *reinterpret_cast<UInt64*>(clk + 0x38) = fn_a2847(dev);                    // 0xa2847

    if (uVar6 == 3U) {
        __builtin_memset(hub, 0, 24);
        if (fn_a3d6c(dev, hub) != 0) {                                         // 0xa3d6c（2 参，汇编已核）
            const UInt32 h0 = *reinterpret_cast<const UInt32*>(hub);
            const UInt32 cur = *reinterpret_cast<const UInt32*>(clk + 4);
            *reinterpret_cast<UInt32*>(clk + 4) =
                ((h0 << 12) & 0x10000U) | (cur & 0xfffcffffU) | ((h0 << 12) & 0x20000U);
        }
    }

    // ④ 判空 p1[3] / p1[5]，调回调（核心）
    gGvm5DStep = 4;
    const UInt64 p15 = *reinterpret_cast<const UInt64*>(p1b + 0x28);           // p1[5]
    if (local50 == 0 || p15 == 0) { gGvm5DRet = 0; return 0; }
    if (!nredKernelPtrOk(reinterpret_cast<const void*>(local50))) { gGvm5DRet = 0; return 0; }
    if (!nredKernelPtrOk(reinterpret_cast<const void*>(p15))) { gGvm5DRet = 0; return 0; }
    const auto fn_slot3 = reinterpret_cast<UInt32 (*)(UInt64, void*, void*)>(local50);
    if (fn_slot3(p15, clk, desc) != 0) { gGvm5DRet = 0; return 0; }

    // ⑤ 拷 0xd8：源为 desc+0xf0（local_1b8 是 local_2a8 的内部视图）
    gGvm5DStep = 5;
    __builtin_memcpy(const_cast<UInt8*>(p1b) + 0x30, desc + 0xf0, 0xd8);

    // ⑥ local_68 分支（<3 查 DAT_0042da78；>=3 走断言）
    gGvm5DStep = 6;
    const SInt32 idx68 = static_cast<SInt32>(*reinterpret_cast<const UInt32*>(type));
    gGvm5DLocal68 = static_cast<UInt32>(idx68);
    UInt32 uVar7 = 0;
    if (idx68 >= 0 && idx68 < 3) {
        uVar7 = *reinterpret_cast<const UInt32*>(s + 0x42da78 + static_cast<UInt64>(idx68) * 4);
    } else {
        // 苹果此处调用断言回调（`(* *(slot+112))(*(slot+8), …, "Invalid ZFB mode!")`）后继续。
        // ⚠ 该回调是否 panic 未确证 ⇒ 按"不 panic"纪律**不调用**，只记录步骤号并继续
        //   （uVar7=8 与苹果一致；唯一差异是少一行断言日志）。
        uVar7 = 8;
    }

    // ⑦ 后续 7 个动作（985d0.c:124–170）
    const auto fn_989a5 = reinterpret_cast<UInt8 (*)(void*, SInt32, UInt32)>(s + 0x989a5);
    const auto fn_a2f00 = reinterpret_cast<void (*)(UInt64, UInt32, UInt64)>(s + 0xa2f00);
    const auto fn_a3f63 = reinterpret_cast<void (*)(UInt64)>(s + 0xa3f63);
    const auto fn_8b4bf = reinterpret_cast<UInt64 (*)(UInt64)>(s + 0x8b4bf);

    gGvm5DStep = 7;
    // A1（M1）：`0x989a5` 内部不判空 `p1[7]`（+0x38）与 `*dev`（`0x8b4bf(dev)` 的入参），
    //  空指针会 panic。苹果原叶同样不判 ⇒ 本门是**刻意 fail-closed**（与苹果不等价，
    //  但只在异常路径生效；正常路径两者行为相同）。
    if (!nredKernelPtrOk(reinterpret_cast<const void*>(
            *reinterpret_cast<const UInt64*>(p1b + 0x38)))) {
        gGvm5DRet = 0; gGvm5DStep = 0x0C; return 0;
    }
    if (!nredKernelPtrOk(reinterpret_cast<const void*>(
            *reinterpret_cast<const UInt64*>(reinterpret_cast<const void*>(dev))))) {
        gGvm5DRet = 0; gGvm5DStep = 0x0D; return 0;
    }
    if ((*reinterpret_cast<const UInt8*>(p1b + 0x108) & 1U) != 0) {
        (void)fn_989a5(const_cast<UInt8*>(p1b), static_cast<SInt32>(uVar7), 6);
    }
    if (static_cast<UInt32>(idx68) == 2U) {
        (void)fn_989a5(const_cast<UInt8*>(p1b), 1, 1);
    }
    fn_a2f00(dev, 3, (*reinterpret_cast<const UInt64*>(desc + 8) & 2U) >> 1);

    gGvm5DStep = 8;
    const UInt64 p1c = *reinterpret_cast<const UInt64*>(p1b + 0xE0);          // p1[0x1c]
    if (p1c != 0) {
        if (!nredKernelPtrOk(reinterpret_cast<const void*>(p1c))) { gGvm5DRet = 0; return 0; }
        // 苹果在 `988be` 同样**重新读** `p1[5]`（`movq 40(%r14),%rdi`）⇒ 逐字复刻。
        const UInt64 p15c = *reinterpret_cast<const UInt64*>(p1b + 0x28);
        if (reinterpret_cast<UInt32 (*)(UInt64)>(p1c)(p15c) != 0) { gGvm5DRet = 0; return 0; }
    }
    if (fn_989a5(const_cast<UInt8*>(p1b), 2, 3) == 0) { gGvm5DRet = 0; return 0; }

    // ⑧ GART save context（985d0.c:139–168）
    gGvm5DStep = 9;
    const UInt64 slot = fn_8b4bf(dev);
    if (!nredKernelPtrOk(reinterpret_cast<const void*>(slot))) { gGvm5DRet = 0; return 0; }
    const UInt64 ctxObj = *reinterpret_cast<const UInt64*>(reinterpret_cast<const void*>(slot + 600));
    const UInt64 ctxFn  = *reinterpret_cast<const UInt64*>(reinterpret_cast<const void*>(slot + 696));
    __builtin_memset(save, 0, 16);
    __builtin_memset(hub, 0, 24);
    *reinterpret_cast<UInt32*>(hub) = 0x10;
    const UInt64 p1f = *reinterpret_cast<const UInt64*>(p1b + 0x78);          // p1[0xf]
    // A4（L1）：苹果在 `9891c` **重新读** `p1[5]`（`movq 40(%r14),%rdi`），不复用前面的值。
    const UInt64 p15g = *reinterpret_cast<const UInt64*>(p1b + 0x28);
    if (p15g == 0 || p1f == 0) {
        // 苹果此处调用断言回调（`"Unrecognized or invalid hub type!"`）后返回 false。
        // ⚠ 同上：不调用断言（避免未确证的 panic 风险），直接 fail-closed。
        gGvm5DRet = 0;
        return 0;
    }
    if (!nredKernelPtrOk(reinterpret_cast<const void*>(p1f))) { gGvm5DRet = 0; return 0; }
    if (reinterpret_cast<UInt32 (*)(UInt64, void*)>(p1f)(p15g, hub) != 0) { gGvm5DRet = 0; return 0; }
    *reinterpret_cast<UInt32*>(save) = *reinterpret_cast<const UInt32*>(hub + 4);
    __builtin_memcpy(save + 4, hub + 8, 8);   // 非对齐写（save+4）⇒ 用 memcpy 避免 UB
    if (ctxFn == 0 || !nredKernelPtrOk(reinterpret_cast<const void*>(ctxFn))) { gGvm5DRet = 0; return 0; }
    const UInt32 ret = (reinterpret_cast<UInt32 (*)(UInt64)>(ctxFn)(ctxObj) == 0) ? 1U : 0U;

    // ⑨ 收尾（0xa3f63 为空实现，逐字调用）
    gGvm5DStep = 10;
    fn_a3f63(dev);
    gGvm5DRet = ret;
    return ret;
}

// ─── 276：相位只读标记（复用 `-NRedGvm5Phase`，默认关）──────────────────────────────────────
//  由 `X5000::wrapInitializeTtl`（phase=1）与 `X6000FB::wrapControllerPowerUp`（phase=2）入口调用。
//  只写 BSS；不执行 D、不改返回值、不写文件、不 SYSLOG、不写寄存器。
void nredGvm5PhMark(int phase)
{
    if (!gGvm5Phase) { return; }
    if (phase == 1) { gGvm5PhStart = 1; }
    else if (phase == 2) { gGvm5PhPower = 1; }
}

// 276：相位开关查询（供 X5000/X6000FB 的 wrapper 入口做显式守卫；gGvm5Phase 是本 TU 的 static）。
int nredGvm5PhaseEnabled(void) { return gGvm5Phase ? 1 : 0; }

// ─── 275／277：早期注入入口（**已废止**；保留函数与 X5000 调用点，避免大改）──────────────────
//  第 276 判读：`wrapInitializeTtl` 入口的 `self+0x50` **不是 client5 的 ctx**（真正的 ctx 是
//  `*(AMDRTHardware+0x338)+0x50`，且 client5 的 slot-4 对象在 `ttl_initialize` 内部、紧邻
//  `0x928f4` 之前才创建）⇒ **入口注入结构性失败**，不能用。
//  同一判读确认：`wrapMode9440f` 的 client5 stage1 调用本就在 start 相位/kernel_task、ctx 正确
//  ⇒ M4′ 不需要入口注入，只需保留 late-path panic（见 `wrapMode9440f`）。
//  ⇒ 本函数**恒返回 -1**（不执行 D、不 panic、不干预）；`-NRedGvm5Early` 现在只表示
//    "D 失败即 panic（late path）"。
int nredGvm5EarlyInject(void* const self)
{
    (void)self;
    return -1;
}

UInt32 X5000HWLibs::wrapMode9440f(void* const a, UInt32 const b, UInt32 const c, void* const d)
{
    // 270：client7 stage1 的窗口在函数返回时关闭（任何 return 路径）。
    NredP7Window p7win(gP7Hold && b == 7 && c == 1);
    // ── 274：client5 stage1 入口只读探针（`-NRedGvm5Phase`，默认关；**在 D 之前**）──────────
    //  只写 BSS；不执行 D、不改返回值。`ra` 取本函数的调用者地址并归零（ra - slide）。
    if (gGvm5Phase && b == 5 && c == 1) { nredGvm5Phase(a, __builtin_return_address(0)); }
    // ── 275：M4′ 幂等（`-NRedGvm5Early` 早期已真实成功 ⇒ powerUp 阶段不再执行 D）──────────
    //  R3a：**必须比较 p1**——两次调用的 ctx/p1 可能不是同一对象；p1 不同/非法时不得跳过
    //  （否则 powerUp 的 client5 未执行，失败仍可能落到 WindowServer）。
    if (gGvm5EarlyOk != 0 && b == 5 && c == 1) {
        gGvm5EarlyPowerSeen = true;
        // `a` 非法 ⇒ 不解引用（p1now=0 ⇒ 不跳过，落到 D；D 内部会判 a 并 fail-closed）。
        const UInt64 p1now = nredKernelPtrOk(a)
            ? *reinterpret_cast<const UInt64*>(static_cast<const UInt8*>(a) + 0x30) : 0;
        if (nredKernelPtrOk(reinterpret_cast<const void*>(p1now)) && p1now == gGvm5EarlyP1) {
            gGvm5EarlyPowerIdem = 1;
            gGvm5EarlyPowerRc = static_cast<SInt32>(gGvm5EarlyRc);
            return gGvm5EarlyRc;   // 同一对象 ⇒ 原样返回早期真实成功的 rc（不伪造）
        }
        // p1 不同或非法 ⇒ 不跳过；落到下方 `-NRedGvm5D` 真实执行（未开 D 则原路径）。
        gGvm5EarlyPowerIdem = 0;
        gGvm5EarlyPowerP1 = p1now;
    }
    // ── 273：client5 stage1 叶替换（`-NRedGvm5D`，默认关）──────────────────────────────────
    //  与 `0x94831` 的尾跳等价：先 `p1 = *(a+0x30)`（在叶内），再执行我方 16 步复刻。
    //  关态 ⇒ 本行不成立，后续路径逐字不变；开态 ⇒ 不进入苹果叶（也不进入下方 fail-closed 护栏）。
    if (gGvm5D && b == 5 && c == 1) {
        const UInt32 rc = nredGvm5Stage1(a, d);
        // 276：`-NRedGvm5Early` 下未命中幂等（p1 不同/非法，或早期未成功）⇒ 这是 late path；
        //  D 失败必须**主动 panic**（否则失败可能拖到 powerUp/WindowServer，违反"失败必早 panic"）。
        //  `gGvm5Early` 为假 ⇒ 保持现有 `-NRedGvm5D` 行为（返 0），不得改变。
        if (rc == 0 && gGvm5Early) {
            gGvm5EarlyPowerSeen = true;
            gGvm5EarlyPowerIdem = 0;
            gGvm5EarlyPowerRc = 0;
            gGvm5EarlyPowerPanic = 1;
            // 277：panic 文本带 D 读数（为下一轮定因）；缓冲在 BSS（≤200 B）。
            const int pn = snprintf(gGvm5PanicBuf, sizeof(gGvm5PanicBuf),
                "NRed: gvm5 client5 stage1 failed (late path) ret=%d step=%u p1=%llx local68=%x startRc=%d",
                static_cast<int>(gGvm5DRet), static_cast<unsigned>(gGvm5DStep),
                static_cast<unsigned long long>(gGvm5DP1), static_cast<unsigned>(gGvm5DLocal68),
                static_cast<int>(gGvm5EarlyStartRc));
            panic("%s", (pn > 0) ? gGvm5PanicBuf : "NRed: gvm5 client5 stage1 failed (late path)");
        }
        return rc;
    }
    // 268：qq 只读探针必须先于句柄安装（安装会改 *(p)）。
    if (gQqProbe && !gQqProbeSeen && c == 1 && b == 5) { nredQqProbe(a); }
    if (gQqHdl && c == 1 && b == 5) { (void)nredQqHdlInstall(a); }
    // 265：client5 的 qq 四字段探针（默认关；只读一次）。
    if (gSt1Q && !gSt1QQSeen && c == 1 && b == 5) { nredSt1QQProbe(a); }
    // 263：client5 的 IP 根提供者（默认关）。必须在守卫之前安装。
    if (gIpR && c == 1 && b == 5) { nredIpRInstall(a); }
    // 262：client5 golden 6 槽探针（默认关；只读一次）。
    if (gGSlot && !gGSlotSeen && c == 1 && b == 5) { nredGSlotProbe(a); }
    // 261：client5 GVM type/注册表探针（默认关；只读一次）。
    if (gGvm5 && !gGvm5Seen && c == 1 && b == 5) { nredGvm5Probe(a); }
    // 257：client 3 stage1 时刻的 P3 观测（默认关；仅首次）。
    if (gSt1P && !gSt1PDone && c == 1 && b == 3) { nredStage1PRecord(a); }
    // 256：仅 c>=1 的调次（默认关）——入口最前，只写 BSS，不改返回值。
    if (gSt1C && c >= 1) { nredStage1CRecord(b, c); }
    // 252：原始调次（默认关）——入口最前，只写 BSS，不改返回值。
    if (gSt1B) { nredStage1BRecord(b, c); }
    // 249：对齐观测（默认关）——入口、护栏之前；仅 c==1；只写 BSS，不改返回值。
    if (gSt1O && c == 1) { nredStage1ORecord(a, b); }
    // ── 软件初始化替换：安装动作（R60 §1.1）——**在入口执行**（入参即带模块号 `b`，1-based）──────
    //  门控关（默认）⇒ 本行不成立＝零行为；哪些模块安装由 `-NRedSwInitIds` 决定（默认只模块 5）。
    //  ⚠ 必须在 org（`0x9440f`）执行**之前**——否则苹果已在 handler 内读过 `*(subobj+8)`（R60 §1.1）。
    if (gGvmSwInstall) { nredSwInstallSlot(a, b, c); }
    // ── 阶段分派层 fail-closed 护栏（R185／R191／R194）────────────────────────────────────────────
    //  ① 208／209 轮实证：苹果模块外层会在 `funcs[0]` 之外读空指针（Apple HWLibs +0xa12df、CR2=0）。
    //     ⇒ client 10 的 stage-0 一律拦；client 15 的 stage-0 **仅在我方探针已安装/启用时放行**
    //     （探针只读、恒返 6，不进入 0x3fc39/0x403c7）。**未安装时放行＝苹果外层执行＝trap 重现**。
    //  ② R185：`0x929ac` 遇失败**不中止**——`0x92a73 callq 0x9440f` → `testb %al,%al` → 失败仅
    //     `0x92a8e movl %eax,148(%r13)` 记录 → `0x92adc xorl %r12d,%r12d` → **继续遍历**。
    //     故只拦 stage-0 无效：拦掉 stage-1 后仍会进入 stage-2/3。stage≥1 在本机从未成功过
    //     （209 首次进入即 trap，**trap 型不会自动重启**）⇒ **所有 stage≥1 一律 fail-closed**。
    //  ③ 条件：`(b==10 && c==0) || (c>=1) || (b==15 && c==0 && !已安装/已启用)`。
    //  ④ `[swgrd]` 每次命中都打（经我方 L2：nredTraceLine→gTraceRing→NRedTraceFlush-*）。
    //     **本轮不以该行作为主判据——主判据是 panic 首行形态（trap vs 断言型）。**
    //     ⚠ 本命中点在 boot 遍历路径，不属于 nredTraceFlush 的安全时点（仅 ①L2 周期拍 ②dumpNow
    //     ③panic 分片）⇒ **不同步刷写**（避免早期 APFS 挂起）。
    //  ⑤ 失败形态：**返 0**——苹果 `0x929ac` 的判据是 `testb %al,%al`，`%al==0` 才走失败分支
    //     （`92a78 testb`／`92a7a je 0x92a97` → `0x92e71 logSwipFailure` → 置 `obj+0xa0` 失败位）。
    //     **此处 0 表示失败**（与 `kSwRes*` 的"0=成功"相反）。护栏在本 wrapper 内直接返回、不进入
    //     `NRedSwInitDispatch` ⇒ 不会把 0 记进 `gSwResById`（保持原值，通常 0xFF）；命中记 `gGrdLastHit=1`。
    // ── 246/248：client 7 只读探针（`-NRedSt1W` 指针值；`-NRedSt1W2` 槽值，两者独立）────────
    //  放在所有 `return 0` 之前 ⇒ 无论护栏走哪条分支都能取到读数；只写 BSS，不改返回值。
    //  仅 W 开 ⇒ 只读指针链，不 deref svc；仅 W2 开 ⇒ 读槽（[st1x]/[st1y]），不输出 [st1w]。
    if ((gSt1W || gSt1W2) && c == 1 && b == 7) { nredStage1WProbe(a); }
    // ── 244：stage1 首批放行（`-NRedStage1`，默认关；批 1 仅 client 14）────────────────────
    //  关 ⇒ 本块不成立，行为与逐字不变的 `c>=1` fail-closed 护栏一致。
    //  开且 c==1 ⇒ 先跑前置检查；通过才调用原件并**原样返回其返回值**（真实执行 stage1）。
    //  其余 client（2/5/6/7/8/9/…）guard 恒 false ⇒ 仍落到下面的守卫 `return 0`。
    if (gStage1 && c == 1) {
        if (!nredStage1Guard(a, b)) {
            gSt1Ret0 |= (1U << b);
            return 0;   // 苹果极性：0＝失败
        }
        gSt1Try |= (1U << b);
        if (gSt1RunN < 255) { ++gSt1RunN; }
        if (singleton().orgMode9440f == 0) { return 0; }
        const UInt32 retSt1 = FunctionCast(wrapMode9440f, singleton().orgMode9440f)(a, b, c, d);
        if (retSt1 == 0) { gSt1Ret0 |= (1U << b); }   // 叶返回失败也计入 ret0
        return retSt1;
    }
    if ((b == 10 && c == 0) || (c >= 1) ||
        (b == 15 && c == 0 && !(gSwInstalled[15].installed && (gSwInitIdsMask & (1U << 15)) != 0))) {
        gGrdLastHit = 1;
        static char grdLine[80];
        const int ng = snprintf(grdLine, sizeof(grdLine), "[swgrd] c=%u st=%u hit=1\n",
                                static_cast<unsigned>(b), static_cast<unsigned>(c));
        if (ng > 0) { nredTraceLine(grdLine, ng); }
        // 242：stage1 预检（只读一级指针；不调原件、不改返回值）。
        if (gSt1Pre && c == 1) { nredStage1Precheck(a); }
        return 0;   // 苹果极性：0＝失败（不是 kSwRes* 的成功语义）
    }
    // 观测通道修复（需求 v2）**M2：安装点出现**（`wrapMode9440f` 入口且属**模块 5**，**不额外限制 stage**）
    //  ⇒ 打标记并刷一次；⚠ 刻意放在**安装动作之后**（上面那行）⇒ 该片头部区能读到安装侧真实值
    //  （`ins=`／`got=`／`exp=`）。
    if (gTraceAtIpSeq && b == 5) {
        nredTraceMark(gFlushedAtIpSeqM2, kMarkM2, static_cast<int>(sizeof(kMarkM2) - 1));
    }
    if (singleton().orgMode9440f == 0) { return 0; }
    const UInt32 ret = FunctionCast(wrapMode9440f, singleton().orgMode9440f)(a, b, c, d);
    // H1 观测集 N-4（2026-10-18）：记**前三次** `(client, stage, ret)`（**零解引用**，只写标量）。
    //  ⚠️ 门控关 ⇒ 本块整段不执行（零写入、零成本）—— 本回调曾在 B25 的 double-fault 栈上
    //  （`wrapMode9440f+0x67`）⇒ 关态绝不能增加任何栈/时间开销（规格 §2.4 硬要求）。
    //  ⚠️ 刻意**放在既有 `-NRedAccelLog` 观测块之前、且不做早退**：早退会让关态丢掉既有日志
    //  （违反"关态与现状逐字一致"）⇒ 用 `if (gTtlBisect)` 包住即可，两条路径互不影响。
    if (gTtlBisect && gBis9440fN < 3) {
        gBis9440f[gBis9440fN].client = b;
        gBis9440f[gBis9440fN].stage  = c;
        gBis9440f[gBis9440fN].ret    = ret;
        ++gBis9440fN;
    }
    // R33 扩读（2026-10-18）：9 个纯标量（**只取证，严禁任何置位/行为改动**）。
    //  【为什么挂在这里】`ipi_gvm_sw_init`（`0x98125`）**没有任何 `callq` 调用者**——本卡亲核：
    //  它是被 `0x9440f` 的**跳转表**尾跳进入的（`0x9440f` client=5／stage=0 分支：
    //  `94571`→stage 表 `0x94e00`[0]=`0x94a5b`：`movq %r14,%rdi ; callq 0x98050 ; jmp 0x98125`）。
    //  ⇒ **`0x98125` 的入参 `rdi` ＝ `0x98050(a)` ＝ `*(a + 8 + 5*8)` ＝ `*(a+48)`**，
    //     其中 `a` 就是**本 wrapper 的 arg0**。⇒ 本处可逐字复现整条链，**无需新增 wrapper**。
    //  【读链（全部逐字核对反汇编，逐级自备空守卫、短路即出）】
    //    subobj = 0x98050(a)                 = *(a+48)                    ; L1（Apple 侧有守卫，我方自备）
    //    host   = *(subobj)                                               ; L1b
    //    slot1  = 0x8b4bf(host) = 0x8bbc4(*(host),1) = *(*(host)+16+1*24) ; L2（Apple 双重守卫）
    //    root   = *(host)                                                 ; L3a（= 0x8bbc4 的 base）
    //    o3     = *(root+88)    = *(root + 16 + 3*24)                     ; TTL 资源槽 3
    //    o4     = *(root+112)   = *(root + 16 + 4*24)                     ; TTL 资源槽 4
    //    obj14  = *(o4+0x78)    = 0x93980(o4,14) = *(o4 + 8 + 14*8)       ; 客户端槽 14（MES）
    //    fn112  = *(obj14+112)                                            ; 提交方法指针
    //    v176   = *(u32*)(o3+176)                                         ; **能力位**（本轮主目标）
    //    record = 0x958d8(host,5) = *(u32*)… 池记录（sub 号 A／载荷 X）
    //    A = *(u32*)(record+4) ; X = *(u32*)(record+44)
    //    fn600/fn616 = *(u64*)(slot1+600) / *(u64*)(slot1+616)            ; 第五出口的 ctx 与提交函数
    //  ⚠ **纯内存读**：不加 SYSLOG、不新增 wrapper、不改任何返回值/ABI、不参与时序。
    //  ⚠ 只在 client==5 && stage==0 时读（那正是 `0x98125` 的入口形态）⇒ 语义对齐、零多余扰动。
    if (gTtlBisect && b == 5 && c == 0 && gBisHit == 0) {
        const auto setShort = [](const UInt32 r) {
            if (gBisShort == kBisShortNone) { gBisShort = r; }
        };
        if (!nredKernelPtrOk(a)) { setShort(kBisShortArg); }
        else {
            const UInt8* const A = static_cast<const UInt8*>(a);
            // L1：subobj = *(a+48)（等价于 Apple 的 `0x98050(a)`）
            void* subobj = reinterpret_cast<void*>(*reinterpret_cast<const UInt64*>(A + 48));
            if (!nredKernelPtrOk(subobj)) { setShort(kBisShortSub); subobj = nullptr; }
            if (subobj != nullptr) {
                gBisSub = reinterpret_cast<UInt64>(subobj);
                // ★ R42：`r14i = *(u64*)(subobj+8)`（＝ 0x98125 的 `r14`，`9817e: movq 8(%rbx),%r14`）。
                //  判 ③a：为 0 ⇒ `0x981fc je` ⇒ "NULL GVM sw_init entry!"。守卫＝subobj 已非空。
                gBisR14i = *reinterpret_cast<const UInt64*>(static_cast<const UInt8*>(subobj) + 8);
                gBisR14iRead = 1;
                // ★ R47/R49：`r14b2`＝出口再读同一地址（**独立再解引用一次**，非复制 rec 变量）。
                gBisR14b2 = *reinterpret_cast<const UInt64*>(static_cast<const UInt8*>(subobj) + 8);
                gBisR14b2Read = 1;
                // L1b：host = *(subobj)
                void* host = *reinterpret_cast<void* const*>(subobj);
                if (!nredKernelPtrOk(host)) { setShort(kBisShortHost); host = nullptr; }
                if (host != nullptr) {
                    gBisHost = reinterpret_cast<UInt64>(host);
                    const UInt8* const H = static_cast<const UInt8*>(host);
                    // ★ R34 修正（2026-10-18）：本块原链**少一层解引用**，R15 的"槽系"7 个读数
                    //   （o3/o4/c14/f112/f600/f616/v176）因此**全部无效**。两处偏差（本卡逐字亲核）：
                    //   偏差① `slot1`：Apple `982ad..982b3` 传 **root＝*(host)** 给 `0x8b4bf`，
                    //          而 `0x8b4bf` **内部再解一次引用**（`8b4c8: movq (%rdi),%rdi`）
                    //          ⇒ **slot1 ＝ *( *(root) + 16 + 1*24 ) ＝ *( *(root) + 40 )**；
                    //          原实现读 `*(root+40)` ⇒ 少一层（这正是 R15 `sh=slot1` 短路的原因）。
                    //   偏差② `o3/o4`：Apple `0xa3014(ctx)` 经 `0x8b8e7(ctx)` ⇒ **`base3 ＝ *(ctx)`**
                    //          ⇒ **o3 ＝ *( base3 + 16 + 3*24 ) ＝ *( *(ctx) + 88 )**；
                    //          原实现误用 `root` 作 base ⇒ base 完全不同。
                    //  （R33 §4.1 的规格此处本身有误，已由 R34 §0.4 更正 ⇒ 以本实现的机器码为准。）
                    // L3a：root = *(host)
                    void* root = *reinterpret_cast<void* const*>(host);
                    if (!nredKernelPtrOk(root)) { setShort(kBisShortRoot); root = nullptr; }
                    // ★新增级：`rootIn = *(root)`（对应 `0x8b4bf` 内部那一次解引用）⇒ 自备守卫
                    void* rootIn = nullptr;
                    if (root != nullptr) {
                        rootIn = *reinterpret_cast<void* const*>(root);
                        if (!nredKernelPtrOk(rootIn)) { setShort(kBisShortRootIn); rootIn = nullptr; }
                    }
                    // L2：slot1 = 0x8b4bf(root) = *( *(root) + 40 )
                    void* slot1 = nullptr;
                    if (rootIn != nullptr) {
                        slot1 = reinterpret_cast<void*>(
                            *reinterpret_cast<const UInt64*>(static_cast<const UInt8*>(rootIn) + 16 + 1 * 24));
                    }
                    if (!nredKernelPtrOk(slot1)) { setShort(kBisShortSlot1); slot1 = nullptr; }
                    if (slot1 != nullptr) {
                        const UInt8* const S = static_cast<const UInt8*>(slot1);
                        gBisFn600 = *reinterpret_cast<const UInt64*>(S + 600);
                        gBisFn616 = *reinterpret_cast<const UInt64*>(S + 616);
                    }
                    // ★ R39：`arg2` 确认读数（**本卡唯一新增**；只读、不调用 Apple 函数）。
                    //  取法（逐字，R39 §2.1）：`slot3obj = 0x8b8e7(root) = 0x8bbc4(*(root),3)
                    //   = *( *(root) + 16 + 3*24 ) = *(rootIn + 88)`（rootIn＝*(root) 已算好）；
                    //  `rec[0] = *(u32*)(slot3obj + 56)`；`a2 = (rec[0]==1) ? 2 : 0`。
                    //  ⚠ 每级自备 nredKernelPtrOk 守卫；slot3obj==0 ⇒ 哨兵＋rb=0。
                    if (rootIn != nullptr) {
                        const UInt8* const RIN = static_cast<const UInt8*>(rootIn);
                        void* slot3obj = reinterpret_cast<void*>(
                            *reinterpret_cast<const UInt64*>(RIN + 16 + 3 * 24));
                        if (!nredKernelPtrOk(slot3obj)) { slot3obj = nullptr; }
                        if (slot3obj != nullptr) {
                            gBisA2Read = 1;   // rb=1：真读过（a2 从此可信，即使值恰为哨兵形态）
                            const UInt32 rec0 = *reinterpret_cast<const UInt32*>(
                                static_cast<const UInt8*>(slot3obj) + 56);
                            gBisA2 = (rec0 == 1) ? 2u : 0u;
                            // ★ R44 裁决：r0＝rec[0] 原值（与 a2 同源同链）＋ r0b 已读标志。
                            gBisR0 = rec0;
                            gBisR0Read = 1;
                            // ★ R47/R49：`r0b2`＝出口再读同一地址（**独立再解引用一次**，非复制 rec0）。
                            gBisR0b2 = *reinterpret_cast<const UInt32*>(
                                static_cast<const UInt8*>(slot3obj) + 56);
                            gBisR0b2Read = 1;
                        }
                    }
                    // ctx = *(slot1+600)（第五出口的 rdi；`982b8: movq 600(%rax),%r14`）
                    void* ctx = nullptr;
                    if (slot1 != nullptr) {
                        ctx = reinterpret_cast<void*>(
                            *reinterpret_cast<const UInt64*>(static_cast<const UInt8*>(slot1) + 600));
                        if (!nredKernelPtrOk(ctx)) { setShort(kBisShortCtx); ctx = nullptr; }
                    }
                    // ★新增级：base3 = *(ctx)（对应 `0x8b8e7` 内部那一次解引用）⇒ 自备守卫
                    void* base3 = nullptr;
                    if (ctx != nullptr) {
                        base3 = *reinterpret_cast<void* const*>(ctx);
                        if (!nredKernelPtrOk(base3)) { setShort(kBisShortBase3); base3 = nullptr; }
                    }
                    if (base3 != nullptr) {
                        const UInt8* const B3 = static_cast<const UInt8*>(base3);
                        gBisO3 = *reinterpret_cast<const UInt64*>(B3 + 16 + 3 * 24);
                        gBisO4 = *reinterpret_cast<const UInt64*>(B3 + 16 + 4 * 24);
                        // obj14 = *(o4+0x78)（＝ `0x93980(o4,14)`＝`0x9bff9`）
                        void* o4 = reinterpret_cast<void*>(gBisO4);
                        if (nredKernelPtrOk(o4)) {
                            const UInt8* const O4 = static_cast<const UInt8*>(o4);
                            gBisObj14 = *reinterpret_cast<const UInt64*>(O4 + 0x78);
                            void* obj14 = reinterpret_cast<void*>(gBisObj14);
                            if (nredKernelPtrOk(obj14)) {
                                gBisFn112 = *reinterpret_cast<const UInt64*>(
                                    static_cast<const UInt8*>(obj14) + 112);
                            }
                        }
                        // v176 = *(u32*)(o3+176)：**主目标**。
                        //  ⚠ R34 要求：必须能区分"真读过"与"未读的哨兵" ⇒ 另置 `gBisV176Read`。
                        void* o3 = reinterpret_cast<void*>(gBisO3);
                        if (nredKernelPtrOk(o3)) {
                            gBisV176 = *reinterpret_cast<const UInt32*>(
                                static_cast<const UInt8*>(o3) + 176);
                            gBisV176Read = 1;   // ← 真读过（v176 从此可信，即使值恰为 0xFFFFFFFF）
                        }
                    }
                    // record = 0x958d8(host, 5)：**逐字复刻 Apple 的算式**（本卡亲核 `958eb..95921`）：
                    //   pool = 0x8bb9f(*(host), 7) = *( *(*(host)) + 16 + 7*24 )    ; `958eb..958f3`
                    //   idx  = *(u32*)(pool + (5<<5))                                ; `95902/95906`
                    //   idx == 0xFFFFFFFF ⇒ 未装配（返 0）                            ; `9590e/95911`
                    //   **record = *(u64*)(pool+608) + 3652 * idx**                  ; `95913/9591a`
                    //     ⚠ 与"pool + 608*idx"**不是**同一式（我先前写错过，已按机器码改正）。
                    // ★ R47/R49：`recib2`＝上述 `0x958d8` 复刻的**出口复读**（同一算式在同一读点再取一次，
                    //  非复制变量、不调用 Apple 函数）⇒ 三条出口分支各紧跟 `gBisRecib2` 赋值，
                    //  哨兵＝`kBisNone64`（Read=0 时值不可信）。
                    void* pool = nullptr;
                    {
                        void* inner = *reinterpret_cast<void* const*>(host);
                        if (nredKernelPtrOk(inner)) {
                            const UInt8* const IN = static_cast<const UInt8*>(inner);
                            // 0x8bb9f(rdi=*(host), 7) = *(  *( *(host) ) + 16 + 7*24 )
                            void* base = reinterpret_cast<void*>(*reinterpret_cast<const UInt64*>(IN));
                            if (nredKernelPtrOk(base)) {
                                pool = reinterpret_cast<void*>(
                                    *reinterpret_cast<const UInt64*>(static_cast<const UInt8*>(base) + 16 + 7 * 24));
                            }
                        }
                    }
                    if (!nredKernelPtrOk(pool)) {
                        // 0x958d8 在 pool==0 时返 0（`958f8 je 0x95923`）⇒ "真读到 0"（② 成立）
                        gBisReci = 0; gBisReciRead = 1;
                        gBisRecib2 = 0; gBisRecib2Read = 1;   // 出口复读：pool 非法 ⇒ 与 reci 同判返 0
                        setShort(kBisShortRec);
                    }
                    else {
                        const UInt8* const P = static_cast<const UInt8*>(pool);
                        const UInt32 idx = *reinterpret_cast<const UInt32*>(P + (5 << 5));
                        if (idx == 0xFFFFFFFFU) {
                            // 0x958d8 在 idx==0xFFFFFFFF 时返 0（`9590e/95911`）⇒ "真读到 0"（② 成立）
                            gBisReci = 0; gBisReciRead = 1;
                            gBisRecib2 = 0; gBisRecib2Read = 1;   // 出口复读：idx 未装配 ⇒ 返 0
                            setShort(kBisShortRec);
                        }
                        else {
                            const UInt64 recAddr = *reinterpret_cast<const UInt64*>(P + 608)
                                                 + 3652ULL * static_cast<UInt64>(idx);
                            // reci＝0x958d8 的原样返回（raw 地址，**不做** ptr-ok：Apple 亦不校验，
                            //  非 0 ⟺ 记录可查 ⟺ ② 不成立）。rc=1 ⇒ 真读到（含真读到 0 的情况）。
                            gBisReci = recAddr; gBisReciRead = 1;
                            gBisRecib2 = recAddr; gBisRecib2Read = 1;   // 出口复读：同一算式同址再取
                            void* rec = reinterpret_cast<void*>(recAddr);
                            if (!nredKernelPtrOk(rec)) { setShort(kBisShortRec); }
                            else {
                                const UInt8* const RC = static_cast<const UInt8*>(rec);
                                gBisA = *reinterpret_cast<const UInt32*>(RC + 4);
                                gBisX = *reinterpret_cast<const UInt32*>(RC + 44);
                            }
                        }
                    }
                }
            }
            gBisHit = 1;   // 已走过一次（不覆盖，取"第一次"形态）
        }
    }
    // 只读观测：`0x9440f` 模式 20B 入口，返回 0=失败、非 0=成功。门控 `-NRedAccelLog`。
    //
    // ⚠️ 深栈安全约束（2026-10-12，B25 Double fault 实证）：
    //   B25 的 NVRAM 分片栈＝ `tmrInit→bringupRunToTmrInit→nredFwBringupHook→
    //   wrapSmuInitFunctionPointerList→wrapMode9440f+0x67` ⇒ 崩在本回调的读数代码内。
    //   根因同 B19（`kb/技术草案/B19-DoubleFault归因-日志通道-os_log.md`）：FW bringup 深栈上
    //   `lilu_os_log`（内部 `char tmp[1024]`）等重栈耗通道 ⇒ 触栈保护页 ⇒ Double fault。
    //   `78a1b25` 只门控了 `NRED_TRACE` 的 SYSLOG 子通道，**未覆盖**本回调直接调用的
    //   `SYSLOG` / `FileIO::writeBufferToFile` / `NvMsgBuf::dumpNow`。
    // ⇒ 本回调内**不做任何文件系统写、不调 lilu 日志通道**；唯一输出通道＝`nredTraceLine`
    //   （内存环，仅 `gTraceRing.push`，无 FS、无 os_log；由 L2 定时拍在安全时点刷盘）。
    if (checkKernelArgument("-NRedAccelLog")) {
        char line[128];
        const int n = snprintf(line, sizeof(line),
                               "9440f: slot=%llx type=%u sub=%u ret=%u\n",
                               static_cast<unsigned long long>(reinterpret_cast<UInt64>(a)),
                               static_cast<unsigned>(b), static_cast<unsigned>(c),
                               static_cast<unsigned>(ret));
        if (n > 0) {
            // ★ 唯一通道：内存环写入（2026-10-12）。与 panic 形态解耦 —— 不管崩在哪/崩不崩，
            //   L2 周期拍（NRedObserve-*）或 dumpNow/panic 分片在**安全时点**带出本行。
            //   深栈上本调用只做 `gTraceRing.push`，零 FS、零 os_log、零大栈缓冲。
            nredTraceLine(line, n);
        }
     }
    // B88 观测 R1（95bda 收口规格 §3.1）：结果层读数 t30=*(a+0x30)（1 跳）。
    //  ≠0 ⟺ "id=5 表项存在∧建槽成功"；仅 t30/ret/overflow 变化时才打（≤4 行/轮）。
    //  B89 (b1')：overflow=gTraceRing.overflow（环溢出丢弃次数，真实成员名）——直接证实/否证"环淹没丢行"。
    //  ⚠️ 零行为改动、不新增寄存器访问、**不受门控**；逐跳 nredKernelPtrOk（非法按 0，B84 铁律四）；
    //  小缓冲 char[160]＋nredTraceLine（深栈纪律：B19/B25 Double fault 同根因）。
    //  B92增：e0（id=5 表项[0]）的表项地址口径【未解出】（新表基址经 0x8b9ea 挂入对象池的字段未证）
    //  ⇒ 不臆造解引用（B84 铁律四）；以 bm=*(a+0x7B8)（能力位图，1 跳、同 t30 纪律）作安全等价读数。
    {
        const UInt64 t30 = nredKernelPtrOk(a)
            ? *reinterpret_cast<const UInt64*>(static_cast<const UInt8*>(a) + 0x30) : 0;
        static UInt64 sPrevT30 = 0xFFFFFFFFFFFFFFFFULL;
        static UInt32 sPrevRet = 0xFFFFFFFFU;
        static UInt32 sPrevOvf = 0xFFFFFFFFU;
        static UInt32 sPrevBm = 0xFFFFFFFFU;
        static unsigned sSeq = 0;
        static bool sType5Done = false;    // B91①：type==5 必打（一次性，豁免 ≤4 上限）
        const UInt32 ovf = gTraceRing.overflow;   // B89 (b1')：环溢出丢弃次数
        const UInt32 bm5 = nredKernelPtrOk(a)     // B92增：能力位图（e0 的安全等价读数，见块注释）
            ? *reinterpret_cast<const UInt32*>(static_cast<const UInt8*>(a) + 0x7B8) : 0;
        const bool isType5 = (b == 5);      // B91①：type=5 那次必打（判 (5,0) 时 obj+0x30 是否为空）
        if (isType5) { gLastT30Type5 = t30; gLastSub5 = c; }   // B91②/B94①：记录供 flush 行留存
        // B93②：另存 type∈{3,8} 的 t30 供 [flush-t30] 行留存（关键读数钉在每片必写的行上）。
        if (b == 3) { gLastT30Type3 = t30; }
        else if (b == 8) { gLastT30Type8 = t30; }
        if (t30 != sPrevT30 || ret != sPrevRet || ovf != sPrevOvf || bm5 != sPrevBm || isType5) {
            sPrevT30 = t30; sPrevRet = ret; sPrevOvf = ovf; sPrevBm = bm5;
            const bool forceType5 = isType5 && !sType5Done;   // B91①：type==5 豁免 ≤4 行上限
            if (forceType5) { sType5Done = true; }
            if (++sSeq <= 4 || forceType5) {
                char line[160];   // B92：追加 bm 后最大行长 130 ⇒ 128 不够
                const int n = snprintf(line, sizeof(line),
                                       "ip5slot: slot=%llx t30=%llx type=%u sub=%u ret=%u overflow=%u bm=%08x\n",
                                       static_cast<unsigned long long>(reinterpret_cast<UInt64>(a)),
                                       static_cast<unsigned long long>(t30), static_cast<unsigned>(b),
                                       static_cast<unsigned>(c), static_cast<unsigned>(ret),
                                       static_cast<unsigned>(ovf), static_cast<unsigned>(bm5));
                if (n > 0) { nredTraceLine(line, n); }
            }
        }
    }
    // P7c 包D：把 `0x9440f` 的 (type, ret) 打包记入全局，供 [ttlchain] 头部戳记的 p5 使用。
    //  （该函数已由本 wrapper 挂钩，不重复挂；此处只写标量，不新增打印/落盘量。）
    gP5 = (static_cast<UInt64>(b) << 32) | static_cast<UInt64>(ret);
    return ret;
}

// ─── 批次 Q′：client5 stage1 叶第 14 步的截断器（`-NRedQp`，默认关）────────────────────────
//  目的：让苹果叶 0x985d0 真实执行第 1–13 步（纯对象/字段操作，**零 MMIO**），
//        在第 14 步（0x987e0 CALL RCX，RCX = *(P5+0x18) = 0x1936e）之前截断并返回失败。
//  依据：M1 §2.1/§2.2/§2.4（13 步清单与零 MMIO 闭包：14 个函数、无 MMIO 助手）、RQ′ §3。
//  机制：routeFunction 把 0x1936e 入口改跳本 wrapper；wrapper 按返回地址判定
//        （ra - slide == 0x987e2）后返回非 0 ⇒ 叶子 0x987e2 TEST EAX,EAX 非零
//        ⇒ 0x987e4 JZ 不跳 ⇒ 0x987e6 XOR EBX,EBX ⇒ 0x987e8 MOV EAX,EBX ⇒ 返回 0（失败）。
//        非叶子路径 ⇒ 原样调回 org（trampoline）。
//  ⚠ **本函数绝不返回 0**：返回 0 会让叶子继续走第 15 步
//     `0x9880c memcpy(p1+0x30, desc+0xf0, 0xd8)`，把**全 0 的 30 个函数指针**
//     装进 P5+0x30..+0x107 ⇒ 后续任何调用都会 call *0 ⇒ trap。
//     （红线：不伪造判定输入、不填 0。）
//  ⚠ **只读**：只读 clk/desc（叶子自己的栈帧）与 BSS 标量；不写任何苹果对象。
//  ⚠ **无重入风险**：0x1936e 全库无直接 call/jmp（仅 0x98099 一处 LEA），
//     且叶子不会递归调用 client5 stage1 ⇒ 无需 CAS 守卫。
SInt32 X5000HWLibs::wrapQpGvm5Init(void* const obj, void* const clk, void* const desc)
{
    const mach_vm_address_t org = singleton().orgQpGvm5Init;
    if (org == 0) { return 1; }   // hook 未成功 ⇒ 也返回失败（绝不调原件、绝不返回 0）

    // RA 守卫：只有"从叶子 0x987e0 的下一条指令返回"才截断
    const UInt64 ra = reinterpret_cast<UInt64>(__builtin_return_address(0));
    const UInt64 raOff = (gHwlibsSlide != 0 && ra > gHwlibsSlide) ? (ra - gHwlibsSlide) : 0;
    if (raOff != 0x987E2ULL) {
        // 非叶子路径 ⇒ 原样调回原件（保证"只影响 client5 stage1 的第 14 步"）
        return FunctionCast(wrapQpGvm5Init, org)(obj, clk, desc);
    }

    if (gQpN < 0xFFFFFFFFU) { ++gQpN; }
    gQpRa   = raOff;
    gQpObj  = reinterpret_cast<UInt64>(obj);
    gQpClk  = reinterpret_cast<UInt64>(clk);
    gQpDesc = reinterpret_cast<UInt64>(desc);

    // 读叶子第 2–13 步的输出缓冲 clk（0x68 B，叶子栈；只读）
    if (nredKernelPtrOk(clk)) {
        const UInt8* const c = static_cast<const UInt8*>(clk);
        gQpC4  = *reinterpret_cast<const UInt32*>(c + 0x04);
        gQpT20 = *reinterpret_cast<const UInt32*>(c + 0x20);
        gQpT28 = *reinterpret_cast<const UInt64*>(c + 0x28);
        gQpA30 = *reinterpret_cast<const UInt64*>(c + 0x30);
        gQpA38 = *reinterpret_cast<const UInt64*>(c + 0x38);
        gQpA50 = *reinterpret_cast<const UInt32*>(c + 0x50);
        gQpA58 = *reinterpret_cast<const UInt64*>(c + 0x58);
        // local_68（ZFB mode）：clk = RBP-0xc8、local_68 = RBP-0x60 ⇒ clk+0x68。
        //   自证：desc = RBP-0x2a0 ⇒ (clk - desc) 必须 == 0x1d8；不等则不读（防帧布局变化）。
        if (nredKernelPtrOk(desc) &&
            (static_cast<const UInt8*>(clk) - static_cast<const UInt8*>(desc)) == 0x1D8) {
            gQpL68   = *reinterpret_cast<const UInt32*>(c + 0x68);
            gQpL68Ok = 1;
        }
    }
    return 1;   // ★ 恒非 0 ⇒ 叶子返回 0（失败）
}

// ─── (8,0) 失败点读点（2026-10-12 技术经理终审；深栈安全——只走 nredTraceLine）────────
//  ⚠️ 本卡单变量＝仅恢复 H-96c4c 一个读点（B31 memdump 已证真机模式一致；其余读点
//  9e505/6430f/6720b/956b8/9e757 本轮不加，后续逐轮单变量）。

// H-96c4c：回退路径读点（type3 槽 +0x30 虚槽 callee）。行 `96c4c: hw=%X ret=%u`。
//  hw = 入参 hw_id（发现块块 hw_id，0x956b8 循环传入）；ret = 返回值（1=填充成功/0=失败）。
//  覆盖函数两个 return 出口（成功填表 return 1 / 失败 return 0，统一在出口读）。
UInt32 X5000HWLibs::wrapMode96c4c(void* const obj, UInt32 const hw, void* const out)
{
    if (singleton().orgMode96c4c == 0) { return 0; }
    const UInt32 ret = FunctionCast(wrapMode96c4c, singleton().orgMode96c4c)(obj, hw, out);
    if (checkKernelArgument("-NRedAccelLog")) {
        char line[128];
        const int n = snprintf(line, sizeof(line), "96c4c: hw=%X ret=%u\n", hw, ret);
        if (n > 0) { nredTraceLine(line, n); }
    }
    return ret;
}

// H-9e505：(8,0) 链的 SMU 槽 sw_init 包装入口。行 `9e505: slot=%llx m=%llx ret=%u`。
//  slot＝入参槽对象指针值（= *(X+0x48)）；m＝*(slot+8) 槽方法指针（区分 B1/B2 返 0 路径用）。
//  ⚠️ 健全性门：仅当 slot!=0 且高 16 位==0xFFFF（内核态典型高位）才解引用，否则 m=0
//   （避免野指针 panic）。ret＝0x9e505 返回值（两个 return 出口统一在出口读）。
//  深栈纪律：只走 nredTraceLine。
UInt32 X5000HWLibs::wrapMode9e505(void* const slot)
{
    // B40 对照标记：`witness: 9e505-enter build=%s %s`（纯观测、零副作用、不改控制流）。
    //  用途：本包装在最近各轮（B35–B39）**确有命中** ⇒ 必然执行，可作"新包是否在跑"的对照：
    //    · 若本行也不出现 ⇒ 新包未被加载（部署/注入面问题），与三条入口标记 0 命中同因；
    //    · 若本行出现而三条标记不出现 ⇒ 包在跑，但那三条标记所在路径确实未执行。
    //  带 __DATE__/__TIME__：即使 CI 增量编译使旧 `build:` 行不变，本行编译时间仍可证新包。
    //  深栈纪律：只走 nredTraceLine（内存环），不调 SYSLOG/文件写。
    if (checkKernelArgument("-NRedAccelLog")) {
        char line[96];
        const int n = snprintf(line, sizeof(line), "witness: 9e505-enter build=%s %s\n",
                               __DATE__, __TIME__);
        if (n > 0) { nredTraceLine(line, n); }
    }
    if (singleton().orgMode9e505 == 0) { return 0; }
    const UInt32 ret = FunctionCast(wrapMode9e505, singleton().orgMode9e505)(slot);
    if (checkKernelArgument("-NRedAccelLog")) {
        UInt64 m = 0;
        const UInt64 sv = reinterpret_cast<UInt64>(slot);
        if (sv != 0 && (sv >> 48) == 0xFFFF) {
            m = *reinterpret_cast<const UInt64*>(static_cast<const UInt8*>(slot) + 8);
        }
        char line[128];
        const int n = snprintf(line, sizeof(line),
                               "9e505: slot=%llx m=%llx ret=%u\n",
                               static_cast<unsigned long long>(sv),
                               static_cast<unsigned long long>(m),
                               ret);
        if (n > 0) { nredTraceLine(line, n); }
    }
    return ret;
}

// H-6430f：smu_sw_init（槽方法本体＝B34 实测 m−slide=0x6430f）入口。
//  行 `6430f: in2=%X ret=%u`：in2＝入参结构 in[2]（入口读，字节偏移 8）；
//  ret＝0x6430f 返回值（两个 return 出口统一在出口读）。深栈纪律：只走 nredTraceLine。
SInt32 X5000HWLibs::wrapMode6430f(void* const in, void* const out)
{
    UInt32 in2 = 0;
    if (nredKernelPtrOk(in)) {
        in2 = *reinterpret_cast<const UInt32*>(static_cast<const UInt8*>(in) + 8);
    }
    // B47 入口参数观测（零行为改动、无条件于 ret 值，置于任何 early-return 之前）：
    //  0x6430f 头部校验 `param_1 && param_2 && *param_1==0x2c0 && *param_2==0x50 && param_1[2]==1`
    //  任一不满足即 return 2（且不经 0x6720b）⇒ 本行读出三要素一刀切开 ret=2 来源。
    //  ⚠️ 解引用**必须先判非空**（防野指针 panic）：v1/v2/in2 均为非空门后的值，空则打 0/-1。
    //  深栈纪律：只走 nredTraceLine，-NRedAccelLog 门控。
    if (checkKernelArgument("-NRedAccelLog")) {
        const UInt64 p2 = reinterpret_cast<UInt64>(out);
        const UInt64 v1 = (nredKernelPtrOk(in)) ? *reinterpret_cast<const UInt64*>(in) : 0;
        const UInt64 v2 = (nredKernelPtrOk(out)) ? *reinterpret_cast<const UInt64*>(out) : 0;
        const int in2v = (nredKernelPtrOk(in)) ? static_cast<const int*>(in)[2] : -1;
        char line[128];
        const int n = snprintf(line, sizeof(line),
                               "6430f: enter p2=%llx v1=%llx v2=%llx in2=%d\n",
                               static_cast<unsigned long long>(p2),
                               static_cast<unsigned long long>(v1),
                               static_cast<unsigned long long>(v2), in2v);
        if (n > 0) { nredTraceLine(line, n); }
    }
    if (singleton().orgMode6430f == 0) { return 0; }
    const SInt32 ret = FunctionCast(wrapMode6430f, singleton().orgMode6430f)(in, out);
    if (checkKernelArgument("-NRedAccelLog")) {
        char line[128];
        const int n = snprintf(line, sizeof(line), "6430f: in2=%X ret=%u\n", in2,
                               static_cast<unsigned>(ret));
        if (n > 0) { nredTraceLine(line, n); }
    }
    return ret;
}

void X5000HWLibs::processKext(KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide, const size_t size)
{
    if (kextRadeonX5000HWLibs.loadIndex != id) { return; }

    // 超时-L1（有界准入）门控：默认闸启用；-NRedSmuGateOff 存在则关闭闸（对照轮用）。
    // 在 kext 早期正常上下文解析一次，存入标量；后续 smuSendMessage 只读标量（避免频繁调用 checkKernelArgument）。
    singleton().smu13SendGateDisabled = checkKernelArgument("-NRedSmuGateOff");
    // T11 第二层注入门控：默认关（仅在第一层零命中时由所有者显式开启）。
    // 解析一次存标量，避免探针路径频繁调用 checkKernelArgument。
    singleton().smu13ProbeInjectDisabled = !checkKernelArgument("-NRedSmuProbeInject");
    // T15 方案II 门控（默认关）：开启后白名单消息改走自有有界直通（D1/D2，案A+案B）。
    // 解析一次存标量，避免 smuSendMessage/注入路径频繁调用 checkKernelArgument。
    singleton().smu13DirectEnabled = checkKernelArgument("-NRedSmuDirectPath");
    // A-1 固件层第一增量门控（默认关）：开启后在 C2（wrapSmuInitFunctionPointerList 出口）执行
    // PSP bringup（fw::nredFwBringupHook）+ 置有效 smuCtxCache。解析一次存标量。
    singleton().smu13FwBringupEnabled = checkKernelArgument("-NRedFwBringup");
    // D-3 只读仪表门控（默认关）：SEG0/SEG1 双段读数、C2PMSG_91/83 直读、PB 状态位读数。
    // 解析一次存标量，仅观测、不写、不发送。
    singleton().smu13SegReadoutEnabled = checkKernelArgument("-NRedSegReadout");
    // A-27（甲）线读数门控（默认关）：C2 出口延迟读 clk_mgr 读数（`gClkMgrPtr` 由
    //  `X6000FB::wrapDcClkMgrCreate` 入口存入）。解析一次存标量，仅观测、不写、不发送。
    singleton().smu13ClkLayoutEnabled = checkKernelArgument("-NRedClkLayoutReadout");
    // B95 门控（**默认关**）：开启后在 `0xa3138` 的失败路径上补入我方基础启动序列表。
    //  解析一次存标量，判定处只读标量（遵守"探针里不调 checkKernelArgument"的既有纪律）。
    //  ⚠ 关闭态 ⇒ 该 hook **根本不注册**（见下方注册处）⇒ 行为与现状逐字一致。
    gDevTablePatch = checkKernelArgument("-NRedDevTablePatch");
    // H1 观测集门控（**默认关**）：换落点后链仍断在 ③／④ 之一 ⇒ 用一轮只读观测把断点二分。
    //  解析一次存标量；判定处只读标量（遵守"探针里不调 checkKernelArgument"的既有纪律）。
    //  ⚠ 关态：零标量写入、零戳记行、零新增解引用 ⇒ 与现状逐字一致。
    gTtlBisect = checkKernelArgument("-NRedTtlBisect");
    // 223：判据④ stage0 裁剪（默认关）。⚠ 判定处只读标量（不在包装内调 checkKernelArgument）。
    gTtlTrim = checkKernelArgument("-NRedTtlTrim");
    // GVM 软件初始化替换门控（**默认全关**）：解析一次存标量；判定处只读标量。
    //  ⚠ 关态：不安装、不介入、零新增日志、零新增解引用 ⇒ 与现状逐字一致。
    gGvmSwInstall  = checkKernelArgument("-NRedGvmSwInstall");
    gGvmSwInitPass = checkKernelArgument("-NRedGvmSwInitPass");
    //  ⚠ **放行开关语义**（R70 §3／需求 R3）：本开关**保留**，但语义改为"**仅当全部步骤均达可用层**时，
    //    才允许把成功报给苹果"⇒ 当前（A8 未达可用层、失败位恒非 0）它**单独无法导致成功**。
    //    保留理由：将来补齐可用层后仍需"是否真的报成功"的单变量开关（逐轮对照），且不改代码即可回滚。
    // 逐模块启用清单（R60 §1.3）：`-NRedSwInitIds=<逗号分隔模块号>`（1..15），**默认 5**（一轮一个变量）。
    //  ⚠ 值型启动参数 ⇒ 用 `PE_boot_args()` 逐字解析（本文件既有 B70 同源）；判定处只读位掩码标量
    //    （遵守"探针内不调 checkKernelArgument"纪律 ⇒ 此处解析一次）。
    nredSwInitIdsParse();
    // 观测通道修复门控（**默认关**）：纯开关（存在即开、无取值）⇒ 用既有 `checkKernelArgument`。
    //  ⚠ 判定处只读标量 `gTraceAtIpSeq`（不在包装内调用 `checkKernelArgument`，遵守既有纪律）。
    gTraceAtIpSeq = checkKernelArgument("-NRedTraceAtIpSeq");
    // 230：BGM 表区转储（默认关）。关态 ⇒ 不注册 hook ⇒ 零输出、零行为。
    gBgmTab = checkKernelArgument("-NRedBgmTab");
    // 235：BGM 64K 捕获取样（默认关）。P3/P4 复用既有 wrapper，仅此开关控制取样。
    gBgm64 = checkKernelArgument("-NRedBgm64");
    // 238：真实容器接管（默认关）。仅当此开关或 -NRedBgmTab 开时需要安装 0x29b4e2 路由。
    gBgmReal = checkKernelArgument("-NRedBgmReal");
    // 241：BGM HW_INIT 事件观测（默认关）。仅门控开时安装 0x91222 函数级 hook。
    gBgmHw = checkKernelArgument("-NRedBgmHw");
    // 242：stage1 预检（默认关）。只影响护栏分支内的只读观测。
    // 257：改用**精确 token** 匹配 —— `-NRedSt1P` 是 `-NRedSt1Pre` 的前缀；若底层解析器做前缀
    //  匹配，任一开关会同时打开另一个（破坏关态零行为）。仅改解析方式，语义不变。
    gSt1Pre = nredExactBootArg("-NRedSt1Pre");
    // 244：stage1 首批放行（默认关；仅 client 14 通过前置检查后调用原件）。
    gStage1 = checkKernelArgument("-NRedStage1");
    // 246：client 7 只读探针（默认关）。只写 BSS 观测值，不影响护栏返回值。
    gSt1W = nredExactBootArg("-NRedSt1W");
    // 248：client 7 槽值探针（默认关）。与 -NRedSt1W 独立；开此开关才会 deref svc。
    gSt1W2 = nredExactBootArg("-NRedSt1W2");
    // 249：client 一级对象对齐观测（默认关）。入口记录，头部区输出。
    gSt1O = checkKernelArgument("-NRedSt1O");
    // 251：client 7 双侧观测（默认关）。仅在开时安装 0x93d18/0x93998 两个只读 hook。
    gGc7 = checkKernelArgument("-NRedGc7");
    // 270：client7 P7 窗口化保活（默认关；与 -NRedGc7 共用两个 hook）。
    gP7Hold = checkKernelArgument("-NRedP7Hold");
    // 252：原始 (b,c) 调次（默认关）。只在前 8 次调用时写 BSS，头部区输出。
    gSt1B = checkKernelArgument("-NRedSt1B");
    // 256：仅 c>=1 的调次（默认关）。钉死 stage1 实际 client 集合。
    gSt1C = checkKernelArgument("-NRedSt1C");
    // 261：client5 GVM type/注册表探针（默认关；只读一次）。
    gGvm5 = checkKernelArgument("-NRedGvm5");
    // 273：client5 stage1 叶替换（**批次 P／P-6 起代码侧撤回**，恒关）。
    //  理由：D 分支位于下方护栏分支**之前** ⇒ D 在则护栏根本不跑，批次 P 的读数目的落空，
    //  且会重复 277 的形态。⇒ 不再解析 `-NRedGvm5D`；D 代码保留但不启用（需求文档 §2 非范围）。
    gGvm5D = false;
    // 274：client5 stage1 入口只读探针（默认关；只写 BSS，不执行 D）。
    gGvm5Phase = nredExactBootArg("-NRedGvm5Phase");
    // 275：M4′ 早期注入（**批次 P／P-6 起代码侧撤回**，恒关）：不再解析 `-NRedGvm5Early`。
    //  它只在 D 的失败分支里被读，D 已不可启用 ⇒ 保留解析只会误导判读。
    gGvm5Early = false;
    // 批次 Q′（**默认关**）：client5 stage1 叶第 14 步的截断器。
    //  关态 ⇒ 不注册 hook、不写读数 BSS、不输出 [qp] ⇒ 与现状逐字一致（本行只写 gQp 自身）。
    gQp = checkKernelArgument("-NRedQp");
    // 262：client5 golden 6 槽（默认关；只读一次）。
    gGSlot = checkKernelArgument("-NRedGSlot");
    // 265：client5 qq 四字段探针（默认关；只读一次）。
    gSt1Q = checkKernelArgument("-NRedSt1Q");
    // 266：TTL 门面（默认关）。仅判据全满足时把静态 root 写入 m_hTtl 槽。
    gTtlFacade = checkKernelArgument("-NRedTtlFacade");
    // 268：client5 qq 句柄层（探针/安装均默认关；步24 守卫随安装开关接入）。
    gQqProbe = checkKernelArgument("-NRedQqProbe");
    gQqHdl = checkKernelArgument("-NRedQqHdl");
    gSt1G5Step24 = gQqHdl;
    // 263：client5 IP 根提供者（默认关）。
    gIpR = checkKernelArgument("-NRedIpR");
    // 257：client 3 stage1 时刻的 P3 观测（默认关；只记首次）。
    gSt1P = nredExactBootArg("-NRedSt1P");
    // 批次 P4（**默认关**）：client 8 观测护栏。关态 ⇒ `case 8` 不可达 ⇒ 逐字不变。
    gSt1G8 = checkKernelArgument("-NRedSt1G8");
    // 253：MES stage0 真实路径（默认关）。关 ⇒ D2 原行为（清零 + 回读为 0）。
    gMesReal = checkKernelArgument("-NRedMesReal");
    // 254：MES b 系列提供者（默认关）。仅构造静态描述符并挂到 +0x790；不调 0x441ad、不写 MMIO。
    gMesB = checkKernelArgument("-NRedMesB");
    // N-2 的 RA 归零基址：`slide` 就是本 kext 的运行时基址（既有 `resolveProbe` 的 `slide + want`
    //  同源于此）。⚠ 与 `kcSlide` 无关（该成员全仓库只读不写、恒 0 ⇒ 用它归零会得到假读数）。
    gHwlibsSlide = static_cast<UInt64>(slide);

    NRed::singleton().hwLateInit();

    CAILAsicCapsEntry*     orgCapsTable       = nullptr;
    CAILAsicCapsInitEntry* orgCapsInitTable   = nullptr;
    AMDDeviceTypeEntry*    orgDeviceTypeTable = nullptr;
    AMDDeviceCapabilities* orgDevCapTable     = nullptr;

    if (currentKernelVersion() >= MACOS_11) {
        PenguinWizardry::PatternSolveRequest solveRequests[] = {
            {"__ZL15deviceTypeTable", orgDeviceTypeTable, kDeviceTypeTablePattern},
            {"__ZN11AMDFirmware14createFirmwareEPhjjPKc", this->orgCreateFirmware, kCreateFirmwarePattern,
             kCreateFirmwarePatternMask},
            {"__ZN20AMDFirmwareDirectory11putFirmwareE16_AMD_DEVICE_TYPEP11AMDFirmware", this->orgPutFirmware,
             kPutFirmwarePattern},
        };
        PANIC_COND(!PenguinWizardry::PatternSolveRequest::solveAll(patcher, id, solveRequests, slide, size), "HWLibs",
                   "Failed to resolve symbols");
    }

    PenguinWizardry::PatternSolveRequest solveRequests[] = {
        {"__ZL20CAIL_ASIC_CAPS_TABLE", orgCapsTable, kCailAsicCapsTableHWLibsPattern},
        {"_CAILAsicCapsInitTable", orgCapsInitTable, kCAILAsicCapsInitTablePattern},
        {"_DeviceCapabilityTbl", orgDevCapTable, kDeviceCapabilityTblPattern},
    };
    PANIC_COND(!PenguinWizardry::PatternSolveRequest::solveAll(patcher, id, solveRequests, slide, size), "HWLibs",
               "Failed to resolve symbols");

    PenguinWizardry::JumpPatternSolveRequest jumpPatternSolveRequests[] = {
        {"_smu_9_0_send_message_with_parameter", this->smu90SendMessageWithParameter,
         kSmu90SendMessageWithParameterCallPattern, kSmu90SendMessageWithParameterCallPatternMask,
         kSmu90SendMessageWithParameterCallPatternJumpInstOff},
        {"_sdma_cgs_read_register", this->sdmaCgsReadRegister, kSdmaCgsReadRegisterCallPattern,
         kSdmaCgsReadRegisterCallPatternMask, kSdmaCgsReadRegisterCallPatternJumpInstOff},
        {"_sdma_cgs_write_register", this->sdmaCgsWriteRegister, kSdmaCgsWriteRegisterCallPattern,
         kSdmaCgsWriteRegisterCallPatternMask, kSdmaCgsWriteRegisterCallPatternJumpInstOff},
    };
    PANIC_COND(!PenguinWizardry::JumpPatternSolveRequest::solveAll(patcher, id, jumpPatternSolveRequests, slide, size),
               "HWLibs", "Failed to solve symbols via jump pattern");

    KernelPatcher::SolveRequest smuRequests[] = {
        {"_smu_cos_wait_for", this->smuCosWaitFor},
        {"_smu_cgs_write_register", this->smuCgsWriteRegister},
        {"_smu_cgs_read_register", this->smuCgsReadRegister},
    };
    if (!patcher.solveMultiple(id, smuRequests, slide, size, true)) {
        PenguinWizardry::JumpPatternSolveRequest smuPatternRequests[] = {
            {nullptr, this->smuCosWaitFor, kSmuCosWaitForCallPattern, kSmuCosWaitForCallPatternMask,
             kSmuCosWaitForCallPatternJumpInstOff},
            {nullptr, this->smuCgsWriteRegister, kSmuCgsWriteRegisterCallPattern, kSmuCgsWriteRegisterCallPatternMask,
             kSmuCgsWriteRegisterCallPatternJumpInstOff},
            {nullptr, this->smuCgsReadRegister, kSmuCgsReadRegisterCallPattern, kSmuCgsReadRegisterCallPatternMask,
             kSmuCgsReadRegisterCallPatternJumpInstOff},
        };
        PANIC_COND(!PenguinWizardry::JumpPatternSolveRequest::solveAll(
                       patcher, id, smuPatternRequests,
                       reinterpret_cast<mach_vm_address_t>(this->smu90SendMessageWithParameter), PAGE_SIZE),
                   "HWLibs", "Failed to solve SMU COS/CGS functions");
    }

    if (currentKernelVersion() <= MACOS_10_15_X) {
        PenguinWizardry::PatternRouteRequest request{"__ZN16AmdTtlFwServices7getIpFwEjPKcP10_TtlFwInfo", wrapGetIpFw,
                                                     this->orgGetIpFw};
        PANIC_COND(!request.route(patcher, id, slide, size), "HWLibs", "Failed to route getIpFw");
    }
    else {
        // TODO: Not override the PSP 9 code.
        PenguinWizardry::PatternRouteRequest requests[] = {
            {"__ZN35AMDRadeonX5000_AMDRadeonHWLibsX500025populateFirmwareDirectoryEv", wrapPopulateFirmwareDirectory,
             this->orgGetIpFw},
            {"_psp_bootloader_is_sos_running_3_1", pspIsSosRunning, kPspBootloaderIsSosRunning31Pattern},
            {"_psp_security_feature_caps_set_3_1",
             NRed::singleton().getAttributes().isRenoir() && !NRed::singleton().getAttributes().isPhoenix() ? pspSecurityFeatureCapsSet12 : pspSecurityFeatureCapsSet10,
             currentKernelVersion() >= MACOS_13 ? kPspSecurityFeatureCapsSet31Pattern13 :
                                                  kPspSecurityFeatureCapsSet31Pattern},
        };
        PANIC_COND(!PenguinWizardry::PatternRouteRequest::routeAll(patcher, id, requests, slide, size), "HWLibs",
                   "Failed to route symbols (>10.15)");
        if (currentKernelVersion() >= MACOS_14_4) {
            PenguinWizardry::PatternRouteRequest pspRequests[] = {
                {"_psp_bootloader_load_sysdrv_3_1", retOK, kPspBootloaderLoadSysdrv31Pattern1404},
                {"_psp_bootloader_set_ecc_mode_3_1", retOK, kPspBootloaderSetEccMode31Pattern1404},
                {"_psp_bootloader_load_sos_3_1", pspBootloaderLoadSos10, kPspBootloaderLoadSos31Pattern1404},
                {"_psp_reset_3_1", retUnsupported, kPspReset31Pattern1404},
            };
            PANIC_COND(!PenguinWizardry::PatternRouteRequest::routeAll(patcher, id, pspRequests, slide, size), "HWLibs",
                       "Failed to route symbols (>=14.4)");
        }
        else {
            PenguinWizardry::PatternRouteRequest pspRequests[] = {
                {"_psp_bootloader_load_sysdrv_3_1", retOK, kPspBootloaderLoadSysdrv31Pattern,
                 kPspBootloaderLoadSysdrv31PatternMask},
                {"_psp_bootloader_set_ecc_mode_3_1", retOK, kPspBootloaderSetEccMode31Pattern},
                {"_psp_bootloader_load_sos_3_1", pspBootloaderLoadSos10, kPspBootloaderLoadSos31Pattern,
                 kPspBootloaderLoadSos31PatternMask},
                {"_psp_reset_3_1", retUnsupported, kPspReset31Pattern},
            };
            PANIC_COND(!PenguinWizardry::PatternRouteRequest::routeAll(patcher, id, pspRequests, slide, size), "HWLibs",
                       "Failed to route symbols (<14.4)");
        }
    }

    if (currentKernelVersion() >= MACOS_14_4) {
        PenguinWizardry::PatternRouteRequest request{"_psp_cmd_km_submit", wrapPspCmdKmSubmit, this->orgPspCmdKmSubmit,
                                                     kPspCmdKmSubmitPattern1404, kPspCmdKmSubmitPatternMask1404};
        PANIC_COND(!request.route(patcher, id, slide, size), "HWLibs", "Failed to route psp_cmd_km_submit (14.4+)");
    }
    else {
        PenguinWizardry::PatternRouteRequest request{"_psp_cmd_km_submit", wrapPspCmdKmSubmit, this->orgPspCmdKmSubmit,
                                                     kPspCmdKmSubmitPattern, kPspCmdKmSubmitPatternMask};
        PANIC_COND(!request.route(patcher, id, slide, size), "HWLibs", "Failed to route psp_cmd_km_submit");
    }

    PenguinWizardry::JumpPatternRouteRequest fwRequests[] = {
        {"_gc_set_fw_entry_info", wrapGcSetFwEntryInfo, this->orgGcSetFwEntryInfo, kGcSetFwEntryInfoCallPattern,
         kGcSetFwEntryInfoCallPatternMask, kGcSetFwEntryInfoCallPatternJumpInstOff},
        {"_sdma_init_function_pointer_list", wrapSdmaInitFunctionPointerList, this->orgSdmaInitFunctionPointerList,
         kSdmaInitFuncPtrListCallPattern, kSdmaInitFuncPtrListCallPatternMask,
         kSdmaInitFuncPtrListCallPatternJumpInstOff},
        {"_smu_init_function_pointer_list", wrapSmuInitFunctionPointerList, this->orgSmuInitFunctionPointerList,
         kSmuInitFunctionPointerListCallPattern, kSmuInitFunctionPointerListCallPatternMask,
         kSmuInitFunctionPointerListCallPatternJumpInstOff},
    };
    // ⛔ 曾尝试在此加 {"_smu_9_0_send_message_with_parameter", wrapSmu90SendMessageWithParameter, ...}
    //    实测 route 失败 → 启动早期 panic "Failed to route FW-related functions"（CONFIRMED §16.6，1dbf4c2）。
    //    原因：该符号无 call 调用点（只被 solve 成函数指针）。**不要再加回**。
    PANIC_COND(!PenguinWizardry::JumpPatternRouteRequest::routeAll(patcher, id, fwRequests, slide, size), "HWLibs",
               "Failed to route FW-related functions");

    // 重定向成员到 trampoline：routeFunction 已把原函数入口改为跳回 wrapper，
    // 若不重指，smuSendMessage→成员 会重新进入 wrapper 造成递归（smu13InternalHwInit 内部发消息时）。
    // 保护：route 失败时 org 为 0，此时保留 solve 到的原始指针（避免成员被置空导致后续崩溃）。
    if (this->orgSmu90SendMessageWithParameter != 0) {
        this->smu90SendMessageWithParameter =
            reinterpret_cast<CAILResult (*)(void*, UInt32, UInt32)>(this->orgSmu90SendMessageWithParameter);
    }

    KernelPatcher::RouteRequest dmcuFwRequests[] = {
        {nullptr, getDcn1FwConstants},
        {nullptr, getDcn21FwConstants},
    };

    dmcuFwRequests[0].from = patcher.solveSymbol(id, "_dmcu_get_dcn1_fw_constants", slide, size, true);
    dmcuFwRequests[1].from = patcher.solveSymbol(id, "_dmcu_get_dcn21_fw_constants", slide, size, true);
    if (dmcuFwRequests[0].from == 0 || dmcuFwRequests[1].from == 0) {
        const auto solveCall =
            [](KernelPatcher::RouteRequest& req, const mach_vm_address_t addr, const mach_vm_address_t end)
        {
            for (size_t off = 0; off < 2; off += 1) {
                // call ...
                // test al, al
                // je ...
                auto* data = reinterpret_cast<const UInt8*>(addr + off);
                if (addr + off + 13 > end) { return false; }
                if (data[0] == 0xE8 && data[5] == 0x84 && data[6] == 0xC0 && data[7] == 0x0F && data[8] == 0x84) {
                    req.from = PenguinWizardry::jumpInstDestination(addr + off, end);
                    return true;
                }
            }
            return false;
        };

        size_t offset;
        PANIC_COND(!KernelPatcher::findPattern(kDmcuBackdoorLoadFwBranchPattern, kDmcuBackdoorLoadFwBranchPatternMask,
                                               arrsize(kDmcuBackdoorLoadFwBranchPattern),
                                               reinterpret_cast<const void*>(slide), size, &offset),
                   "HWLibs", "Failed to find `dmcu_backdoor_load_fw` branch pattern");
        const auto branch = PenguinWizardry::jumpInstDestination(
            slide + offset + kDmcuBackdoorLoadFwDcn1ConstantsBranchOff, slide + size);
        PANIC_COND(branch == 0, "HWLibs", "Failed to get `dmcu_backdoor_load_fw` branch destination");

        PANIC_COND(!solveCall(dmcuFwRequests[0], branch + kDmcuGetDcn1FwConstantsCallOff, slide + size), "HWLibs",
                   "Failed to solve `dmcu_get_dcn1_fw_constants` via call pattern");
        PANIC_COND(!solveCall(dmcuFwRequests[1], slide + offset + kDmcuGetDcn21FwConstantsCallOff, slide + size),
                   "HWLibs", "Failed to solve `dmcu_get_dcn21_fw_constants` via call pattern");
    }

    PANIC_COND(!patcher.routeMultiple(id, dmcuFwRequests, slide, size), "HWLibs",
               "Failed to route DMCU FW-related functions");

    // ─── 纯只读「内存字节转储」诊断（2026-10-12）：终结「真机 HWLibs 是否与本地提取件一致」──
    //  背景：B30 真机日志 `mode6720b: expected-offset bytes mismatch`＋`pattern not found`，
    //  而本地提取件 0x6720b 处与模式逐字节一致 ⇒ 矛盾，需直接看真机内存里的字节。
    //  ⚠️ 纯读＋打印：不写任何内存、不 patch、不调 Apple 方法；只走 nredTraceLine（内存环，
    //  禁 SYSLOG/FileIO/dumpNow——深栈/早期安全，B25/B19 纪律）。门控 -NRedAccelLog（默认关）。
    //  对照组：0x2ab00e（mode2-tail）/0x9440f（mode9440f）为真机已证命中偏移，
    //  用于判读「读法/基址是否正确」。基址自证行（slide/size）供判读映像归属。
    if (checkKernelArgument("-NRedAccelLog")) {
        const auto* const base = reinterpret_cast<const UInt8*>(slide);
        char line[160];
        int n = snprintf(line, sizeof(line), "probe-base: slide=%llx size=%zx\n",
                         static_cast<unsigned long long>(slide), size);
        if (n > 0) { nredTraceLine(line, n); }
        static constexpr mach_vm_address_t kDumpOffs[] = {
            0x6720B, 0x9E505, 0x6430F, 0x96C4C, 0x956B8, 0x9E757,
            0x2AB00E, 0x9440F,
        };
        for (const mach_vm_address_t off : kDumpOffs) {
            if (off + 32 > size || off + 32 < off) {
                n = snprintf(line, sizeof(line), "memdump: off=%llx oob\n",
                             static_cast<unsigned long long>(off));
            } else {
                char hex[65];
                for (size_t k = 0; k < 32; k += 1) {
                    snprintf(hex + k * 2, 3, "%02x", base[off + k]);
                }
                n = snprintf(line, sizeof(line), "memdump: off=%llx b=%s\n",
                             static_cast<unsigned long long>(off), hex);
            }
            if (n > 0) { nredTraceLine(line, n); }
        }
    }

    // 第八步观测（2026-09-28）：route TTL 的跨 kext 查询函数（无符号名 ⇒ 用 pattern）。
    //  ⚠️ 失败**不 panic**（只记 SYSLOG）：pattern 失配不应打断启动。
    {
        PenguinWizardry::PatternRouteRequest ttlReq{"", wrapTtlQuery, this->orgTtlQuery, kTtlQueryPattern};
        if (!ttlReq.route(patcher, id, slide, size)) {
            NRED_TRACE("ttl-query: failed to route (pattern miss)");
        }
        else {
            DBGLOG("HWLibs", "ttl-query: routed");
        }
    }

    // 第八步观测（2026-09-28 第十九轮）：定位 TTL 初始化 `0x8b10e` 的失败出口（4 个候选判据函数）。
    //  ⚠️ 安全策略：先用 `findPattern` 解出地址、**与预期偏移比对一致才挂 hook**——避免模式命中
    //  错误位置而 patch 错函数；不一致只记日志、不挂（绝不影响启动）。
    {
        const auto resolveProbe = [&](const char* tag, const UInt8* pat, const UInt8* mask, const size_t n,
                                      const mach_vm_address_t want) -> mach_vm_address_t
        {
            // ① 优先：按"预期偏移"直接校验字节（最可靠——模式可能在别处重复，导致扫描命中错误位置）
            const auto* const at = reinterpret_cast<const UInt8*>(slide + want);
            bool exact = true;
            for (size_t k = 0; k < n; k += 1) {
                if (mask != nullptr && mask[k] == 0) { continue; }
                if (at[k] != pat[k]) { exact = false; break; }
            }
            if (exact) {
                DBGLOG("HWLibs", "%s: verified at expected offset 0x%llx", tag,
                       static_cast<unsigned long long>(want));
                return slide + want;
            }
            NRED_TRACE("%s: expected-offset bytes mismatch -> falling back to pattern scan", tag);
            // ② 回退：全 kext 扫描，并要求命中偏移与预期一致（否则跳过，绝不 patch 错地方）
            size_t offset = 0;
            if (!KernelPatcher::findPattern(pat, mask, n, reinterpret_cast<const void*>(slide), size, &offset)) {
                NRED_TRACE("%s: pattern not found", tag);
                return 0;
            }
            DBGLOG("HWLibs", "%s: pattern at offset 0x%zx (expected 0x%llx)", tag, offset,
                   static_cast<unsigned long long>(want));
            if (offset != want) {
                NRED_TRACE("%s: matched 0x%zx, expected 0x%llx -> skipped", tag, offset,
                       static_cast<unsigned long long>(want));
                return 0;
            }
            DBGLOG("HWLibs", "%s: resolved at expected offset", tag);
            return slide + offset;
        };
        // 构建身份自证：本 kext 本轮成功注册的 hook 数（hookProbe 成功时 +1）
        //  ⚠️ 刻意用**块局部**而非 static：作用域就是本块（注册完即打印），
        //   若用 static 且 processKext 被多次调用会跨调用累加 ⇒ 计数失真。
        unsigned nHooks = 0;
        const auto hookProbe = [&](const mach_vm_address_t from, KernelPatcher::RouteRequest&& req,
                                   const char* tag) -> bool
        {
            req.from = from;
            if (patcher.routeMultiple(id, &req, 1, slide, size)) {
                DBGLOG("HWLibs", "%s: hooked", tag);
                nHooks += 1;
                return true;
            }
            NRED_TRACE("%s: route failed", tag);
            return false;
        };
        // 235：`[bgm64-h]` 安装自证（仅 -NRedBgm64 时输出；state=1 已安装、0 未安装/未尝试）。
        const auto bgm64H = [](const UInt32 pt, const bool ok, const mach_vm_address_t addr) {
            if (!gBgm64) { return; }
            fw::fwTrace("[bgm64-h] pt=%u state=%u addr=%llx", static_cast<unsigned>(pt),
                       ok ? 1u : 0u, static_cast<unsigned long long>(addr));
        };

        if (const auto from = resolveProbe("ttl-coll", kTtlInitCollectPattern, nullptr,
                                           arrsize(kTtlInitCollectPattern), 0x8B5DE)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapTtlCollect, this->orgTtlCollect}, "ttl-coll");
        }
        if (const auto from = resolveProbe("ttl-allocA", kTtlInitAllocAPattern, kTtlInitAllocAPatternMask,
                                           arrsize(kTtlInitAllocAPattern), 0x93880)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapTtlAllocA, this->orgTtlAllocA}, "ttl-allocA");
        }
        if (const auto from = resolveProbe("ttl-allocB", kTtlInitAllocBPattern, kTtlInitAllocBPatternMask,
                                           arrsize(kTtlInitAllocBPattern), 0x928F4)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapTtlAllocB, this->orgTtlAllocB}, "ttl-allocB");
        }
        if (const auto from = resolveProbe("ttl-checkD", kTtlInitCheckDPattern, kTtlInitCheckDPatternMask,
                                           arrsize(kTtlInitCheckDPattern), 0xA285F)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapTtlCheckD, this->orgTtlCheckD}, "ttl-checkD");
        }
        if (const auto from = resolveProbe("ttl-init", kTtlInitPattern, nullptr, arrsize(kTtlInitPattern),
                                           0x8AF60)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapTtlInit, this->orgTtlInit}, "ttl-init");
        }
        if (const auto from = resolveProbe("ttl-reg", kTtlRegIfacePattern, nullptr, arrsize(kTtlRegIfacePattern),
                                           0x8B9A5)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapTtlRegIface, this->orgTtlRegIface}, "ttl-reg");
        }
        if (const auto from = resolveProbe("tls-create", kTlsCreatePattern, nullptr, arrsize(kTlsCreatePattern),
                                           0x93F76)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapTlsCreate, this->orgTlsCreate}, "tls-create");
        }
        if (const auto from = resolveProbe("tls-swinit", kTlsSwInitPattern, kTlsSwInitPatternMask,
                                           arrsize(kTlsSwInitPattern), 0x95E9F)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapTlsSwInit, this->orgTlsSwInit}, "tls-swinit");
        }
        if (const auto from = resolveProbe("bgm-init", kBgmInitPattern, nullptr, arrsize(kBgmInitPattern),
                                           0x2A9F0F)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapBgmInit, this->orgBgmInit}, "bgm-init");
        }
        if (const auto from = resolveProbe("bgm-step1", kBgmStep1Pattern, nullptr, arrsize(kBgmStep1Pattern),
                                           0x29939F)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapBgmStep1, this->orgBgmStep1}, "bgm-step1");
        }
        if (const auto from = resolveProbe("bgm-query", kBgmQueryPattern, nullptr, arrsize(kBgmQueryPattern),
                                           0x29A159)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapBgmQuery, this->orgBgmQuery}, "bgm-query");
        }
        if (const auto from = resolveProbe("bgm-step4", kBgmStep4Pattern, nullptr, arrsize(kBgmStep4Pattern),
                                           0x2AA5BC)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapBgmStep4, this->orgBgmStep4}, "bgm-step4");
        }
        // 230：BGM 表区转储（默认关）。⚠ 仅在门控开时注册 ⇒ 关态该 wrapper 完全不存在。
        {
            bool ok = false;
            mach_vm_address_t addr = 0;
            if (gBgmTab || gBgmReal) {
                if (const auto from = resolveProbe("bgm-tab", kBgmTabReadPattern, nullptr,
                                                   arrsize(kBgmTabReadPattern), 0x29B4E2)) {
                    ok = hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapBgmTabRead, this->orgBgmTabRead},
                                   "bgm-tab");
                    addr = ok ? from : 0;
                }
            }
            bgm64H(1, ok, addr);   // 235 P1 自证：-NRedBgmTab 未开 ⇒ state=0
        }
        // 235 P2：`0x298ce5` 通用 memcpy（默认关）。⚠ 仅门控开时注册。
        //  P3/P4 不在此注册：`0x2ab398`/`0x2ab00e` 已被上方 cfg-read/mode2-tail 路由占用，
        //  二次 route 不生效 ⇒ 取样逻辑直接并入那两个既有 wrapper（gBgm64 门控）。
        {
            bool ok = false;
            mach_vm_address_t addr = 0;
            if (gBgm64) {
                if (const auto from = resolveProbe("bgm64-memcpy", kBgm64MemcpyPattern, nullptr,
                                                   arrsize(kBgm64MemcpyPattern), 0x298CE5)) {
                    ok = hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapBgm64Memcpy,
                                                                     this->orgBgm64Memcpy},
                                   "bgm64-memcpy");
                    addr = ok ? from : 0;
                }
            }
            bgm64H(2, ok, addr);
        }
        // 241：`0x91222` TrimHook（BGM 事件观测）。⚠ 仅门控开时注册 ⇒ 关态零输出、零行为。
        if (gBgmHw) {
            if (const auto from = resolveProbe("bgmhw-notify", kBgmHwNotifyPattern, nullptr,
                                               arrsize(kBgmHwNotifyPattern), 0x91222)) {
                hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapBgmHwNotify, this->orgBgmHwNotify},
                          "bgmhw-notify");
            }
        }
        if (const auto from = resolveProbe("read-sel", kReadSelPattern, nullptr, arrsize(kReadSelPattern), 0x29A19E)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapReadSel, this->orgReadSel}, "read-sel");
        }
        // 235 P3/P4 自证：这两个 hook 由既有 cfg-read/mode2-tail 安装（与 -NRedBgmTab 无关）。
        {
            bool ok = false;
            mach_vm_address_t addr = 0;
            if (const auto from = resolveProbe("cfg-read", kCfgReadPattern, nullptr, arrsize(kCfgReadPattern),
                                               0x2AB398)) {
                ok = hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapCfgRead, this->orgCfgRead}, "cfg-read");
                addr = ok ? from : 0;
            }
            bgm64H(3, ok, addr);
        }
        {
            bool ok = false;
            mach_vm_address_t addr = 0;
            if (const auto from = resolveProbe("mode2-tail", kMode2TailPattern, nullptr, arrsize(kMode2TailPattern),
                                               0x2AB00E)) {
                ok = hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapMode2Tail, this->orgMode2Tail},
                               "mode2-tail");
                addr = ok ? from : 0;
            }
            bgm64H(4, ok, addr);
        }
        if (const auto from = resolveProbe("mode9440f", kMode9440fPattern, nullptr, arrsize(kMode9440fPattern),
                                           0x9440F)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapMode9440f, this->orgMode9440f}, "mode9440f");
        }
        // 批次 Q′：route 0x1936e（client5 stage1 叶第 14 步的被调者；函数入口型）。
        //   ⚠ 关态 ⇒ 本块不成立 ⇒ 不注册 hook ⇒ 原件逐字不变。
        //   ⚠ resolveProbe 失败只打一行不 panic（沿用既有纪律）。
        if (gQp) {
            if (const auto from = resolveProbe("gvm5-init", kQpGvm5InitPattern, nullptr,
                                               arrsize(kQpGvm5InitPattern), 0x1936E)) {
                if (hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapQpGvm5Init,
                                                                this->orgQpGvm5Init},
                              "gvm5-init")) {
                    gQpArmed = true;   // 只有真的挂上才允许放行叶子（见 nredStage1Guard 的 case 5）
                }
            }
        }
        if (const auto from = resolveProbe("mode96c4c", kMode96c4cPattern, nullptr, arrsize(kMode96c4cPattern),
                                           0x96C4C)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapMode96c4c, this->orgMode96c4c}, "mode96c4c");
        }
        if (const auto from = resolveProbe("mode9e505", kMode9e505Pattern, nullptr, arrsize(kMode9e505Pattern),
                                           0x9E505)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapMode9e505, this->orgMode9e505}, "mode9e505");
        }
        if (const auto from = resolveProbe("mode6430f", kMode6430fPattern, nullptr, arrsize(kMode6430fPattern),
                                           0x6430F)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapMode6430f, this->orgMode6430f}, "mode6430f");
        }
        // B81 观测点 A：route Apple 就绪门谓词 0x6839e（函数入口型；route 失败只打一行不 panic）。
        if (const auto from = resolveProbe("pred90", kPred90Pattern, kPred90PatternMask,
                                           arrsize(kPred90Pattern), 0x6839E)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapPred90, this->orgPred90}, "pred90");
        }
        // B88 观测 R2'：route Apple 0x93f64（查位过滤器；函数入口型；route 失败只打一行不 panic）。
        if (const auto from = resolveProbe("mode93f64", kFlt93F64Pattern, nullptr, arrsize(kFlt93F64Pattern),
                                           0x93F64)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrap93F64, this->org93F64}, "mode93f64");
        }
        // P7c 包D：判据③ `0x9405e` / 判据④ `0x929ac`（函数入口型；route 失败只打一行不 panic）。
        if (const auto from = resolveProbe("ipi-init", kIpiInitPattern, nullptr, arrsize(kIpiInitPattern),
                                           0x9405E)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapIpiInit, this->orgIpiInit}, "ipi-init");
        }
        if (const auto from = resolveProbe("ip-entry-seq", kIpEntrySeqPattern, nullptr, arrsize(kIpEntrySeqPattern),
                                           0x929AC)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapIpEntrySeq, this->orgIpEntrySeq}, "ip-entry-seq");
        }
        // B95v2 介入挂点：`0xa2691`（第二次三键查表；**全库唯一调用者 `a260e`** ∈ `0xa25de`）。
        //  ⚠️ **仅在门控开启时注册** ⇒ 关闭态该 wrapper 完全不存在（降级正确性：行为与现状逐字一致）。
        //  ⚠️ 只改返回值（未命中时返回我方记录指针），**不改 ABI**。
        if (gDevTablePatch) {
            if (const auto from = resolveProbe("dev-lookup", kDevLookupPattern, nullptr,
                                               arrsize(kDevLookupPattern), 0xA2691)) {
                hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapDevLookup, this->orgDevLookup},
                          "dev-lookup");
            }
        }

        // ─── 构建身份自证（2026-10-12）：消除「部署的 kext ≠ 运行的 kext」盲区 ─────────
        //  B24 疑案：ESP 部署 run304，但内核实际运行的 kext 疑似 run301（mode9440f 零命中、
        //  hook 集合与 run301 吻合）。现有「本地/部署/ESP 三方 md5」只证明文件一致，不证明
        //  内核加载的是它。⇒ 在此（**本 kext 的最早无条件执行点**，processKext 入口块内）打一条
        //  编译期构建身份行：`__DATE__`/`__TIME__` 每个 CI 构建唯一 ⇒ 归档直接读出运行构建。
        //  ⚠️ 不受 `-NRedAccelLog` 影响（构建身份始终可查；hook 集合在上一块注册后计数）。
        //  ⚠️ 不 panic、不写文件、不调 Apple 方法；DBGLOG 经 Lilu 日志通道（-liludbgall 时可见）。
        DBGLOG("NRed", "build: date=%s time=%s hooks=%u", __DATE__, __TIME__, nHooks);
        // 同一目的：`-NRedFwBringup` 门控实际解析值（顺带解决「门控是否真的开了」盲区）。
        DBGLOG("NRed", "bootarg FwBringup=%d", singleton().smu13FwBringupEnabled ? 1 : 0);
        // B70：`bootargs:` 逐字行（PE_boot_args，与 checkKernelArgument 同源 PE_state+8；
        //  解决「请求的 ≠ 内核实际看到的」盲区——现有三份 boot-args 记录均只是请求值）。
        //  ⚠️ 位置＝本 kext 最早无条件执行点（构建自报行旁）；**每轮必打**。
        //  通道＝nredTraceLine（内存环，零新增 SYSLOG）；缓冲 static（不占栈）；定长 ≤191。
        //  截断：内容 >180 ⇒ 截到 179 并加 `~` 标记（10 前缀+179+1+换行=191 ≤191）。
        //  失败出口：PE_boot_args() 返回 nullptr ⇒ 记 `bootargs: (null)` 行，不悬停。
        // 251（`-NRedGc7`）：client 7 双侧观测。模式已在离线核对为 __text 唯一；
        //  `resolveProbe` 先按**预期偏移**逐字节校验（等同 hook-check 的“期望偏移＝函数头”），
        //  回退扫描也要求命中偏移一致 ⇒ 绝不 patch 错位置；失败只记日志、不 panic。
        // 270/271：两个 hook 的安装条件分开写（不用 `||`）：
        //  - gGc7：保持 run518 的只读观测行为（含 [gc7x] 注销侧观测）；
        //  - gP7Hold：单独开启时也必须安装（安装侧开窗、注销侧拦截）。
        //  P7 的“写路径拦截”本身在 wrapper 内由 gP7Hold 二次门控 ⇒ 仅 gGc7 时不可能拦截。
        const auto installGc7Hooks = [&]() {
            if (const auto from = resolveProbe("gc7-inst", kGc7InstallPattern, nullptr,
                                               arrsize(kGc7InstallPattern), 0x93D18)) {
                hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapGc7Install, this->orgGc7Install},
                          "gc7-inst");
            }
            if (const auto from = resolveProbe("gc7-set", kGc7SetSlotPattern, nullptr,
                                               arrsize(kGc7SetSlotPattern), 0x93998)) {
                hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapGc7SetSlot, this->orgGc7SetSlot},
                          "gc7-set");
            }
        };
        if (gGc7) {
            installGc7Hooks();
        }
        else if (gP7Hold) {
            installGc7Hooks();
        }
        {
            static char lineBoot[192];
            const char* const ba = PE_boot_args();
            int n = 0;
            if (ba != nullptr) {
                const size_t blen = strlen(ba);
                if (blen <= 180) {
                    n = snprintf(lineBoot, sizeof(lineBoot), "bootargs: %s\n", ba);
                } else {
                    n = snprintf(lineBoot, sizeof(lineBoot), "bootargs: %.179s~\n", ba);
                }
            } else {
                n = snprintf(lineBoot, sizeof(lineBoot), "bootargs: (null)\n");
            }
            if (n > 0) { nredTraceLine(lineBoot, n); }
            // B75：`probe: accelLog=… ttlLog=…` **门控真值行**（纯观测）——把"门控开关"
            //  从推断变成读数：本轮 `bootargs:` 逐字行不含 `-NRedAccelLog`，但同受该门控
            //  包裹的 `tls-create`/`tls-swinit` 却落盘 ⇒ 三者不能同时为真（技术经理未解出项）；
            //  此处直接打印 `checkKernelArgument` 的**返回值**（与 bootargs 行同位置同通道、
            //  紧邻其后，便于对照）。⚠️ 不新增门控、不改任何流程；nredTraceLine 内存环。
            {
                static char lineGate[96];
                const int nGate =
                    snprintf(lineGate, sizeof(lineGate), "probe: accelLog=%d ttlLog=%d\n",
                             checkKernelArgument("-NRedAccelLog") ? 1 : 0,
                             checkKernelArgument("-NRedTtlLog") ? 1 : 0);
                if (nGate > 0) { nredTraceLine(lineGate, nGate); }
            }
        }
    }

    // ★ 落盘通道自检（2026-09-28）：`mode2-tail` 的关键读数发生在 ~34.8s，已晚于 L2 的 msgbuf 覆盖窗口，
    //  必须依赖自建落盘；而自建落盘此前一直没产出文件。这里在**早期**（L2 还看得到的时刻）做一次
    //  同路径的写文件，并把它的事实（`rootvnode` 值 + `FileIO` 返回码）写进日志 ⇒ 一轮即可定位病因。
    {
        static char kSelfTest[] = "nred-selftest\n";
        const int err = (rootvnode != nullptr)
                            ? FileIO::writeBufferToFile("/var/log/NRedSelfTest.txt", kSelfTest, sizeof(kSelfTest) - 1)
                            : -999;
        NRED_TRACE("selftest: rootvnode=%llx err=%d dropped=%u written=%u", static_cast<unsigned long long>(reinterpret_cast<UInt64>(rootvnode)), err, gTraceSafe.dropped, gTraceSafe.written);
    }

    // ★ 修复（2026-09-28）：把 IP 发现表的 `num_base_address` 上限从 6 放宽到 9（详见常量处注释）。
    //  ⚠️ **必须放在 `setKernelWriting(true, ...)` 之外**：`KernelPatcher::applyLookupPatch` 内部
    //  自己会调用 `MachInfo::setKernelWriting(true, kernelWriteLock)`，若调用者已持锁 ⇒ 递归锁 panic
    //  （2026-09-28 真机实测：`hwlock: ... recursively @locks.c:553`）。
    {
        const PenguinWizardry::MaskedLookupPatch ipNbaPatch{&kextRadeonX5000HWLibs, kIpNbaLimitOriginal,
                                                             kIpNbaLimitPatched, 1};
        PANIC_COND(!ipNbaPatch.apply(patcher, slide, size), "HWLibs",
                   "Failed to apply IP num_base_address limit patch");
    }

    PANIC_COND(MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS, "HWLibs",
               "Failed to enable kernel writing");
    if (orgDeviceTypeTable != nullptr) {
        *orgDeviceTypeTable = {.deviceId = NRed::singleton().getDeviceID(), .deviceType = kAMDDeviceTypeNavi10};
    }

    // 豁免 Phoenix 守卫（伪装链设计，刻意保留）：本处 isRenoir() 分支把目标设备 ID 取为
    // 0x1636（Renoir 的 ID），780M 借此匹配 Apple CAILAsicCapsInitTable 中 familyId=RAVEN 的行
    // 并回写为真实 ID 0x15BF；若按真实 ID 匹配则无命中，触发下方 PANIC_COND
    // "Failed to find init caps table entry"。
    // 依据：上游 d53df4a 引入、b9737b2 沿用（改造链条的一环）。
    // 待复核（已登记路线图 §2.2 记录）：该表位于 AMDRadeonX5000HWLibs.kext，离线参考集中无此二进制，
    // 表内容**未独立核验**（在 12.5 现有两个二进制中搜 CAILAsicCapsInitTable 模式 0 命中），
    // 故"是否需要补 !isPhoenix()"待拿到该二进制或真机复核后再定。
    const auto targetDeviceId =
        NRed::singleton().getAttributes().isRenoir() ? 0x1636U : NRed::singleton().getDeviceID();
    for (; orgCapsInitTable->deviceId != 0xFFFFFFFF; orgCapsInitTable++) {
        if (orgCapsInitTable->familyId == AMD_FAMILY_RAVEN && orgCapsInitTable->deviceId == targetDeviceId) {
            orgCapsInitTable->deviceId = NRed::singleton().getDeviceID();
            orgCapsInitTable->revision = NRed::singleton().getDevRevision();
            orgCapsInitTable->extRevision =
                static_cast<UInt64>(NRed::singleton().getEnumRevision()) + NRed::singleton().getDevRevision();
            orgCapsInitTable->pciRevision = NRed::singleton().getPciRevision();
            orgCapsInitTable->ddiCaps     = NRed::singleton().getAttributes().isPhoenix() ? ddiCapsRaven :
                                            NRed::singleton().getAttributes().isRenoirE() ? ddiCapsRenoirE :
                                            NRed::singleton().getAttributes().isRenoir()  ? ddiCapsRenoir :
                                                                                           ddiCapsRaven;
            *orgCapsTable                 = {
                .familyId = AMD_FAMILY_RAVEN,
                .deviceId = NRed::singleton().getDeviceID(),
                .revision = NRed::singleton().getDevRevision(),
                .extRevision =
                    static_cast<UInt32>(NRed::singleton().getEnumRevision()) + NRed::singleton().getDevRevision(),
                .pciRevision = NRed::singleton().getPciRevision(),
                .ddiCaps     = orgCapsInitTable->ddiCaps,
                .skeleton    = orgCapsTable->skeleton,
            };
            break;
        }
    }
    PANIC_COND(orgCapsInitTable->deviceId == 0xFFFFFFFF, "HWLibs", "Failed to find init caps table entry");
    for (; orgDevCapTable->familyId != 0; orgDevCapTable++) {
        if (orgDevCapTable->familyId == AMD_FAMILY_RAVEN && orgDevCapTable->deviceId == targetDeviceId) {
            orgDevCapTable->deviceId = NRed::singleton().getDeviceID();
            orgDevCapTable->extRevision =
                static_cast<UInt64>(NRed::singleton().getEnumRevision()) + NRed::singleton().getDevRevision();
            orgDevCapTable->revision     = DEVICE_CAP_ENTRY_REV_DONT_CARE;
            orgDevCapTable->enumRevision = DEVICE_CAP_ENTRY_REV_DONT_CARE;

            orgDevCapTable->asicGoldenSettings->goldenSettings =
                NRed::singleton().getAttributes().isPhoenix() ? goldenSettingsRaven :
                NRed::singleton().getAttributes().isRaven2() ? goldenSettingsRaven2 :
                NRed::singleton().getAttributes().isRenoir() ? goldenSettingsRenoir :
                                                               goldenSettingsRaven;

            break;
        }
    }
    PANIC_COND(orgDevCapTable->familyId == 0, "HWLibs", "Failed to find device capability table entry");
    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
    DBGLOG("HWLibs", "Applied DDI Caps patches");

    // TODO: Replace this hack with a simple hook.
    if (currentKernelVersion() <= MACOS_10_15_X) {
        // 豁免 Phoenix 守卫：本块仅在 macOS 10.15 及以下才执行，目标系统 13.6 不执行，
        // 故 isRenoir() 分支无需 isPhoenix() 排除（同 X5000.cpp:250 豁免处理）。
        if (NRed::singleton().getAttributes().isRenoir()) {
            const PenguinWizardry::MaskedLookupPatch patches[] = {
                {&kextRadeonX5000HWLibs, kPspSwInit1Original1015, kPspSwInit1Patched1015, 1},
                {&kextRadeonX5000HWLibs, kPspSwInit2Original1015, kPspSwInit2OriginalMask1015, kPspSwInit2Patched1015,
                 1},
            };
            PANIC_COND(!PenguinWizardry::MaskedLookupPatch::applyAll(patcher, patches, slide, size), "HWLibs",
                       "Failed to apply spoof patches");
        }
    }
    else {
        const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX5000HWLibs, kGcSwInitOriginal,
                                                       kGcSwInitOriginalMask,  kGcSwInitPatched,
                                                       kGcSwInitPatchedMask,   1};
        PANIC_COND(!patch.apply(patcher, slide, size), "HWLibs", "Failed to apply gc_sw_init spoof patch");
        if (currentKernelVersion() >= MACOS_14_4) {
            const PenguinWizardry::MaskedLookupPatch patches[] = {
                {&kextRadeonX5000HWLibs, kPspSwInit1Original1404, kPspSwInit1Patched1404, 1},
                {&kextRadeonX5000HWLibs, kPspSwInit2Original1404, kPspSwInit2OriginalMask1404, kPspSwInit2Patched1404,
                 1},
            };
            PANIC_COND(!PenguinWizardry::MaskedLookupPatch::applyAll(patcher, patches, slide, size), "HWLibs",
                       "Failed to apply spoof patches (>=14.4)");
        }
        else {
            const PenguinWizardry::MaskedLookupPatch patches[] = {
                {&kextRadeonX5000HWLibs, kPspSwInit1Original, kPspSwInit1Patched, 1},
                {&kextRadeonX5000HWLibs, kPspSwInit2Original, kPspSwInit2OriginalMask, kPspSwInit2Patched, 1},
            };
            PANIC_COND(!PenguinWizardry::MaskedLookupPatch::applyAll(patcher, patches, slide, size), "HWLibs",
                       "Failed to apply spoof patches (<14.4)");
        }
    }

    // TODO: Replace this hack with a simple hook.
    if (currentKernelVersion() >= MACOS_14_4) {
        const PenguinWizardry::MaskedLookupPatch patches[] = {
            {&kextRadeonX5000HWLibs, kCreatePowerTuneServices1Original1404, kCreatePowerTuneServices1Patched1404, 1},
            {&kextRadeonX5000HWLibs, kCreatePowerTuneServices2Original1404, kCreatePowerTuneServices2Mask1404,
             kCreatePowerTuneServices2Patched1404, 1},
        };
        PANIC_COND(!PenguinWizardry::MaskedLookupPatch::applyAll(patcher, patches, slide, size), "HWLibs",
                   "Failed to apply PowerTuneServices patches (>=14.4)");
    }
    else {
        if (currentKernelVersion() >= MACOS_12) {
            const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX5000HWLibs, kCreatePowerTuneServices1Original12,
                                                           kCreatePowerTuneServices1Patched12, 1};
            PANIC_COND(!patch.apply(patcher, slide, size), "HWLibs", "Failed to apply PowerTuneServices patch (<14.4)");
        }
        else {
            const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX5000HWLibs, kCreatePowerTuneServices1Original,
                                                           kCreatePowerTuneServices1Patched, 1};
            PANIC_COND(!patch.apply(patcher, slide, size), "HWLibs", "Failed to apply PowerTuneServices patch");
        }
        const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX5000HWLibs, kCreatePowerTuneServices2Original,
                                                       kCreatePowerTuneServices2Mask, kCreatePowerTuneServices2Patched,
                                                       1};
        PANIC_COND(!patch.apply(patcher, slide, size), "HWLibs", "Failed to apply PowerTune patch");
    }

    if (currentKernelVersion() >= MACOS_13) {
        const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX5000HWLibs, kCailQueryAdapterInfoOriginal,
                                                       kCailQueryAdapterInfoPatched, 1};
        PANIC_COND(!patch.apply(patcher, slide, size), "HWLibs", "Failed to apply CailQueryAdapterInfo patch");
    }
}

// Taking advantage of the fact device type "Navi 10" is not used in the original function.
void X5000HWLibs::wrapPopulateFirmwareDirectory(void* const self)
{
    if (singleton().orgGetIpFw == 0) { return; }
    FunctionCast(wrapPopulateFirmwareDirectory, singleton().orgGetIpFw)(self);

    const auto fwDir = singleton().fwDirField(self);
    assert(fwDir != nullptr);

    const auto ravenFw =
        singleton().orgCreateFirmware(ativvaxy_rv_dat, sizeof(ativvaxy_rv_dat), 0x0100, "ativvaxy_rv.dat");
    assert(ravenFw != nullptr);
    singleton().orgPutFirmware(fwDir, kAMDDeviceTypeNavi10, ravenFw);

    const auto renoirFw =
        singleton().orgCreateFirmware(ativvaxy_nv_dat, sizeof(ativvaxy_nv_dat), 0x0202, "ativvaxy_nv.dat");
    assert(renoirFw != nullptr);
    singleton().orgPutFirmware(fwDir, kAMDDeviceTypeNavi10, renoirFw);

    const auto dmcubFw =
        singleton().orgCreateFirmware(atidmcub_rn_dat, sizeof(atidmcub_rn_dat), 0x0201, "atidmcub_0.dat");
    assert(dmcubFw != nullptr);
    singleton().orgPutFirmware(fwDir, kAMDDeviceTypeNavi10, dmcubFw);
}

template<const UInt32 N>
static bool setIpFwOutForFW(const char (&data)[N], void* const out)
{
    getMember<const void*>(out, 0x0) = data;
    getMember<UInt32>(out, 0x8)      = N;
    return true;
}

bool X5000HWLibs::wrapGetIpFw(void* const self, const UInt32 ipVersion, const char* const name, void* const out)
{
    if (strncmp(name, "ativvaxy_rv.dat", 15) == 0) {    // VCN 1.0
        return setIpFwOutForFW(ativvaxy_rv_dat, out);
    }
    if (strncmp(name, "ativvaxy_nv.dat", 15) == 0) {    // VCN 2.1
        return setIpFwOutForFW(ativvaxy_nv_dat, out);
    }
    if (strncmp(name, "atidmcub_0.dat", 14) == 0) {    // DMCU version B
        return setIpFwOutForFW(atidmcub_rn_dat, out);
    }
    if (singleton().orgGetIpFw == 0) { return false; }
    return FunctionCast(wrapGetIpFw, singleton().orgGetIpFw)(self, ipVersion, name, out);
}

CAILResult X5000HWLibs::pspIsSosRunning() { return kCAILResultInvalidParameters; }
CAILResult X5000HWLibs::retUnsupported() { return kCAILResultUnsupported; }
CAILResult X5000HWLibs::retOK() { return kCAILResultOK; }

CAILResult X5000HWLibs::pspBootloaderLoadSos10(void* const ctx)
{
    singleton().pspBootloaderVersionField(ctx)  = NRed::singleton().readReg32(MP0_BASE_0 + MP0_SMN_C2PMSG_100);
    singleton().pspTOSVersionField(ctx)         = NRed::singleton().readReg32(MP0_BASE_0 + MP0_SMN_C2PMSG_58);
    (singleton().pspTOSVersionField + 0x4)(ctx) = NRed::singleton().readReg32(MP0_BASE_0 + MP0_SMN_C2PMSG_58);
    return kCAILResultOK;
}

CAILResult X5000HWLibs::pspSecurityFeatureCapsSet10(void* const ctx)
{
    auto& securityCaps     = singleton().pspSecurityCapsField(ctx);
    securityCaps          &= ~1;
    const auto tOSVersion  = singleton().pspTOSVersionField(ctx);
    if ((tOSVersion & 0xFFFF0000) == 0x80000 && (tOSVersion & 0xFF) > 0x50) {
        const auto policyVer = NRed::singleton().readReg32(MP0_BASE_0 + MP0_SMN_C2PMSG_91);
        SYSLOG_COND((policyVer & 0xFF000000) != 0xA000000, "HWLibs", "Invalid security policy version: 0x%X",
                    policyVer);
        if (policyVer == 0xA02031A || ((policyVer & 0xFFFFFF00) == 0xA020400 && (policyVer & 0xFC) > 0x23)
            || ((policyVer & 0xFFFFFF00) == 0xA020300 && (policyVer & 0xFE) > 0x1D))
        {
            securityCaps |= 1;
        }
    }

    return kCAILResultOK;
}

CAILResult X5000HWLibs::pspSecurityFeatureCapsSet12(void* const ctx)
{
    auto& securityCaps     = singleton().pspSecurityCapsField(ctx);
    securityCaps          &= ~1;
    const auto tOSVersion  = singleton().pspTOSVersionField(ctx);
    if ((tOSVersion & 0xFFFF0000) == 0x110000 && (tOSVersion & 0xFF) > 0x2A) {
        const auto policyVer = NRed::singleton().readReg32(MP0_BASE_0 + MP0_SMN_C2PMSG_91);
        SYSLOG_COND((policyVer & 0xFF000000) != 0xB000000, "HWLibs", "Invalid security policy version: 0x%X",
                    policyVer);
        if ((policyVer & 0xFFFF0000) == 0xB090000 && (policyVer & 0xFE) > 0x35) { securityCaps |= 1; }
    }

    return kCAILResultOK;
}

template<const UInt32 N>
static UInt32 replacePspCmdDataWith(void* const data, const char (&fw)[N])
{
    memcpy(data, fw, N);
    return N;
}

CAILResult X5000HWLibs::wrapPspCmdKmSubmit(void* const ctx, void* const cmd, void* const outData,
                                           void* const outResponse)
{
    const auto pspCmd   = getMember<AMDPSPCommand>(cmd, 0x0);
    auto&      dataSize = getMember<UInt32>(cmd, 0xC);
    const auto data     = singleton().pspCommandDataField(ctx);

    switch (pspCmd) {
        case kPSPCommandLoadTA: {
            const char* name = reinterpret_cast<char*>(data + 0x8DB);
            if (strncmp(name, "AMD DTM Application", 19) == 0) { dataSize = replacePspCmdDataWith(data, psp_dtm_bin); }
            else if (strncmp(name, "AMD HDCP Application", 20) == 0) {
                dataSize = replacePspCmdDataWith(data, psp_hdcp_bin);
            }
            else if (strncmp(name, "AMD AUC Application", 19) == 0) {
                dataSize = replacePspCmdDataWith(data, psp_auc_bin);
            }
            else if (strncmp(name, "AMD FP Application", 18) == 0) {
                dataSize = replacePspCmdDataWith(data, psp_fp_bin);
            }
        } break;
        case kPSPCommandLoadASD: {
            dataSize = replacePspCmdDataWith(data, psp_asd_bin);
        } break;
        default: {
        } break;
    }

    if (singleton().orgPspCmdKmSubmit == 0) { return kCAILResultFailed; }
    return FunctionCast(wrapPspCmdKmSubmit, singleton().orgPspCmdKmSubmit)(ctx, cmd, outData, outResponse);
}

CAILResult X5000HWLibs::smuSendMessage(void* const ctx, const UInt32 message, const UInt32 param,
                                       UInt32* const outParam) const
{
    // 超时-L1（有界准入）闸：白名单外的消息直接拒绝，不进入 Apple 侧无界等待。
    // 默认闸启用；-NRedSmuGateOff 存在时关闭闸（对照轮）。
    if (!singleton().smu13SendGateDisabled && !NRedSmuGate::smuMsgAllowedByTimeoutL1(message)) {
        NRED_TRACE("smu13: timeout-L1 gate refused msg 0x%X -> NoResponse", message);
        return kCAILResultNoResponse;
    }

    // T14/U-97-2（2026-10-10）：委托前以纯内存读（零 MMIO）落盘 Apple 入口 `0x683ea` 的
    // ctx 前置就绪检查三项候选值，定位 `rc=2` 的根因（零寄存器访问；指针均作内核地址校验）。
    {
        const auto isValidKernelPtr = [](const UInt64 p) -> bool { return p >= 0xffffff7f80000000ULL; };
        const UInt64 vCtx = reinterpret_cast<UInt64>(ctx);
        // ① ctx+0x310：就绪/绑定标志（Apple 入口 0x683d1 读 `mov 0x310(%rdi),%ecx`）
        const UInt32 v310 = isValidKernelPtr(vCtx)
            ? *reinterpret_cast<volatile const UInt32*>(static_cast<const UInt8*>(ctx) + 0x310) : 0xFFFFFFFF;
        // ② *(ctx+0x10)+0x98：子对象内偏移（Apple 入口 0x683b7→0x64f60 读链）
        const UInt64 p10 = isValidKernelPtr(vCtx)
            ? *reinterpret_cast<volatile const UInt64*>(static_cast<const UInt8*>(ctx) + 0x10) : 0;
        const UInt32 v10_98 = isValidKernelPtr(p10)
            ? *reinterpret_cast<volatile const UInt32*>(reinterpret_cast<const UInt8*>(p10) + 0x98) : 0xFFFFFFFF;
        // ③ *(*(ctx+0x10))+0x128：间接槽位校验（§12 候选；双间接需两次指针校验）
        const UInt64 pp10 = isValidKernelPtr(p10)
            ? *reinterpret_cast<volatile const UInt64*>(reinterpret_cast<const UInt8*>(p10)) : 0;
        const UInt32 vPP10_128 = isValidKernelPtr(pp10)
            ? *reinterpret_cast<volatile const UInt32*>(reinterpret_cast<const UInt8*>(pp10) + 0x128) : 0xFFFFFFFF;
        // ④ ctx+0x428（Apple 入口候选字段）
        const UInt32 v428 = isValidKernelPtr(vCtx)
            ? *reinterpret_cast<volatile const UInt32*>(static_cast<const UInt8*>(ctx) + 0x428) : 0xFFFFFFFF;
        // ⑤ ctx+0x6b8（Apple 入口候选字段）
        const UInt32 v6b8 = isValidKernelPtr(vCtx)
            ? *reinterpret_cast<volatile const UInt32*>(static_cast<const UInt8*>(ctx) + 0x6b8) : 0xFFFFFFFF;
        NRED_TRACE("smu-ctx-check: msg=0x%X v310=0x%X v10_98=0x%X vPP10_128=0x%X v428=0x%X v6b8=0x%X",
                   message, v310, v10_98, vPP10_128, v428, v6b8);
    }
    // T15（方案II，门控 `-NRedSmuDirectPath`，默认关）：白名单消息改走自有有界直通（D1）。
    // 闸仍在前方守门（此处仅白名单可达；gate-off 下的非白名单仍走下方 Apple 委托，对照轮语义守恒）。
    // 门控关闭时本块不执行 ⇒ 默认运行时路径与基线构建等价（仅新增 trace 行输出差异）。
    if (singleton().smu13DirectEnabled && NRedSmuGate::smuMsgAllowedByTimeoutL1(message)) {
        UInt32 rawResp = 0;
        const CAILResult rcDirect = X5000HWLibs::smu13SendMsgDirect(message, param, &rawResp);
        UInt32 outVal = 0;
        if (rcDirect == kCAILResultOK) {
            outVal = NRed::singleton().readReg32(MP0_BASE_0 + MP1_SMN_C2PMSG_82);
        }
        if (outParam != nullptr) { *outParam = outVal; }
        NRED_TRACE("T15-direct: msg=0x%X rc=%u arg=0x%X", message, rcDirect, outVal);
        return rcDirect;
    }

    if (const auto res = this->smu90SendMessageWithParameter(ctx, message, param); res != kCAILResultOK) {
        // T14/U-97-1（2026-10-10）：读响应参数寄存器 C2PMSG_82 分辨 rc=2 的
        // "入口早退"（ctx->0x310 校验失败 → 寄存器未被 Apple 写入）与"分发器兜底"
        // （Apple 已执行发送/等待、有寄存器写入值）；同时输出 regrd=1 标志确认已读。
        const UInt32 uRespArg = this->smuCgsReadRegister(ctx, MP1_SMN_C2PMSG_82, 0, kCAILHWBlockMP1, 0);
        NRED_TRACE("T4: smuSendMessage id=0x%X rc=%u arg=0x%X regrd=1",
                   message, res, uRespArg);
        return res;
    }

    UInt32 outVal = 0;
    if (outParam != nullptr) {
        outVal = this->smuCgsReadRegister(ctx, MP1_SMN_C2PMSG_82, 0, kCAILHWBlockMP1, 0);
        *outParam = outVal;
    }

    // T14（2026-10-10）：T4 增强——追加 regrd 标志（regrd=1 已读响应寄存器；0 未读）。
    //    rc=0 时 regrd=0 若 outParam=nullptr（未请求读取），regrd=1 若 outParam≠nullptr（成功读出）；
    //    rc≠0 时 regrd=1（本行之前已读，见上）—— 与格式前缀 `T4: smuSendMessage …` 保持后向兼容
    //    判读时正则 `T4: smuSendMessage id=0x[0-9A-Fa-f]+ rc=[0-9]+ arg=0x[0-9A-Fa-f]+` 仍应匹配。
    NRED_TRACE("T4: smuSendMessage id=0x%X rc=%u arg=0x%X regrd=%u",
               message, kCAILResultOK, outVal, outParam != nullptr ? 1U : 0U);

    return kCAILResultOK;
}

CAILResult X5000HWLibs::wrapSmu90SendMessageWithParameter(void* const ctx, const UInt32 message, const UInt32 param)
{
    // Probe: 消息 hook 被触发（第 60 位）
    NRed::singleton().orSmu13ProbeState(1ULL << 60);

    if (!singleton().smu13InitAttempted) {
        singleton().smu13InitAttempted = true;
        singleton().smuCtxCache         = ctx;
        smu13InternalHwInit(ctx);
    }

    // T11 第二层：注入式探针（仅当 -NRedSmuProbeInject 显式开启）。
    // 在 Apple 传入活 ctx 的回调点首次进入时，发送白名单三条消息。
    // 约束：不使用 smuCtxCache、不经成员指针调用（避免递归）、禁 panic 门控。
    static bool probeInjected = false;
    if (!probeInjected && !singleton().smu13ProbeInjectDisabled) {
        probeInjected = true;
        constexpr UInt32 kWhitelist[] = {
            PhoenixPPSMC::PPSMC_MSG_TestMessage,        // 0x01
            PhoenixPPSMC::PPSMC_MSG_GetPmfwVersion,     // 0x02
            PhoenixPPSMC::PPSMC_MSG_GetDriverIfVersion  // 0x03
        };
        // T15（方案II，门控 `-NRedSmuDirectPath`，默认关）：第二层注入改走自有有界直通（D2，2 s 上界）。
        // 门控关闭时维持经 Apple org 指针（与基线构建运行时等价；间接层上界未解出，仅探针轮使用）。
        if (singleton().smu13DirectEnabled) {
            for (UInt32 msg : kWhitelist) {
                UInt32 rawResp = 0;
                const CAILResult rc = X5000HWLibs::smu13SendMsgDirect(msg, 0, &rawResp);
                UInt32 argVal = 0;
                if (rc == kCAILResultOK) {
                    argVal = NRed::singleton().readReg32(MP0_BASE_0 + MP1_SMN_C2PMSG_82);
                }
                NRED_TRACE("T11-inject: id=0x%X rc=%u arg=0x%X", msg, rc, argVal);
            }
        }
        else {
            // 默认分支（门控关）：直接调用 org 指针（绕过成员指针，防递归），使用 Apple 传入的 ctx。
            auto orgSend = reinterpret_cast<CAILResult (*)(void*, UInt32, UInt32)>(singleton().orgSmu90SendMessageWithParameter);
            if (orgSend != nullptr) {
                for (UInt32 msg : kWhitelist) {
                    CAILResult rc = orgSend(ctx, msg, 0);
                    NRED_TRACE("T11-inject: id=0x%X rc=%u", msg, rc);
                }
            }
        }
    }

    if (singleton().orgSmu90SendMessageWithParameter == 0) { return kCAILResultFailed; }
    return FunctionCast(wrapSmu90SendMessageWithParameter, singleton().orgSmu90SendMessageWithParameter)(ctx, message,
                                                                                                        param);
}

CAILResult X5000HWLibs::smuPowerUpConfigCommon(void* const ctx)
{
    // ⚠️ Renoir msg：Phoenix 上 0x0E = SetDriverDramAddrLow（同值不同义），T1 闸拦截。
    if (const auto res = singleton().smuSendMessage(ctx, PPSMC_MSG_PowerUpSdma); res != kCAILResultOK) { return res; }
    // ⚠️ Renoir msg：Phoenix 上 0x06 = PowerDownVcn（反向语义，最高危），T1 闸拦截。
    if (const auto res = singleton().smuSendMessage(ctx, PPSMC_MSG_PowerUpGfx); res != kCAILResultOK) { return res; }

    return kCAILResultOK;
}

CAILResult X5000HWLibs::smuInternalSwInit(void* const ctx, void*, AMDSMUSWInitOutput*)
{
    singleton().smuSwInitialisedFieldBase(ctx) = true;
    singleton().smuCtxCache = ctx;
    return kCAILResultOK;
}

CAILResult X5000HWLibs::smuInternalSwInitOld(void* const ctx, void*, AMDSMUSWInitOutput* const output)
{
    // ✅ 同值同义例外：Phoenix 上 0x02 = GetPmfwVersion（同值同义），T1 白名单放行。
    return singleton().smuSendMessage(ctx, PPSMC_MSG_GetSmuVersion, 0, &output->fwConstants.version);
}
CAILResult X5000HWLibs::smuGetUCodeConsts(void* const ctx, AMDSMUUCodeConstants* consts)
{
    if (consts == nullptr) { return kCAILResultInvalidParameters; }
    // ✅ 同值同义例外：Phoenix 上 0x02 = GetPmfwVersion（同值同义），T1 白名单放行。
    // ── B73：SMU「起来确认」**移至此**（原在 NRedFwBringupHook phase3，B72 实测
    //  bringup 阶段 `smuContext()` 恒 0 ⇒ 未发出）────────────────────────────
    //  依据：B72 判读＋技术经理判定——本函数是**实证执行**的 Apple-ctx 持有点：
    //    · Ventura 13.6（>10.15）走 `processKext` 的 else 分支（HWLibs.cpp:3135）
    //      ⇒ `smuGetUCodeConstsField` 装入本函数，Apple 初始化时**必然调用**（B72
    //      的 `T4 id=0x2`（GetPmfwVersion）正是经本函数发出）⇒ Apple 给的 ctx 在；
    //    · 发送走 `smuSendMessage` → 内部调 Apple 原函数（`smu90SendMessageWithParameter`
    //      字段＝solve 到的 Apple 指针；wrapper 未 route、是死代码——见 :1721/:2511 注释）
    //      ⇒ 经 Apple 入口、零我方新增寄存器操作。
    //  序列（逐字）：先 `TestMessage(0x01)` 验活；**仅 rc==0** 再发 `GetPmfwVersion(0x02)`
    //   并把版本值落盘（用途：与资产头 `ucode_version=0x004E8500` 比对）。
    //  判据（既有 T4 行不变）：`T4: smuSendMessage id=0x1 rc=0 arg=非0 regrd=1`＝SMU 活；
    //   `rc=2`＝仍无应答。**每个分支都有出口**（ctx 由本函数语义保证非空；rc!=0 记行返回、
    //   不重试、不悬停——smuSendMessage 内部等待上界＝ctx+0x310 的 2000 步）。
    //  ⚠️ 零 SMN 读（不碰 0x3010028）；零新增 SYSLOG（nredTraceLine 内存环）；定长单行 ≤191。
    // B73b：**一次性保护**（技术经理要求）——Apple 可能多次调用 `smuGetUCodeConsts`，
    //  而该序列首条 `TestMessage` 在 SMU 不应答时最多等 ~2 s（上界＝ctx+0x310 的 2000 步）；
    //  ⇒ 不加保护会**反复等待、拖时间、扰动时序**。故本序列**本进程内至多执行一次**。
    //  标志＝**文件级 `sGSmUpProbeDone`**（:70；技术经理要求共用——0x02 有两处发送点，
    //  将来在其它发送点插同一确认必须复用本标志，否则一次引导发两次）。
    if (!sGSmUpProbeDone) {
        sGSmUpProbeDone = true;
        {
            uint32_t resp = 0;
            const CAILResult rcTest = singleton().smuSendMessage(
                ctx, PhoenixPPSMC::PPSMC_MSG_TestMessage, 0, &resp);
            NRED_TRACE("smu probe: test msg=0x1 rc=%u resp=0x%X",
                       static_cast<unsigned>(rcTest), resp);
            if (rcTest == kCAILResultOK) {
                uint32_t ver = 0;
                const CAILResult rcVer = singleton().smuSendMessage(
                    ctx, PhoenixPPSMC::PPSMC_MSG_GetPmfwVersion, 0, &ver);
                NRED_TRACE("smu probe: ver=0x%X", ver);
                (void)rcVer;   // 版本已落盘；本函数原有 0x02 发送路径不动（保持返回值语义）
            }
        }
    }
    return singleton().smuSendMessage(ctx, PPSMC_MSG_GetSmuVersion, 0, &consts->version);
}

CAILResult X5000HWLibs::smu10PowerUpConfig(void* const ctx)
{
    // ⚠️ Renoir msg：0x39 越出 Phoenix 表（ID 不存在），T1 闸拦截。
    if (const auto res = singleton().smuSendMessage(ctx, PPSMC_MSG_ForceGfxContentSave);
        res != kCAILResultOK && res != kCAILResultUnsupported)
    {
        return res;
    }
    if (const auto res = smuPowerUpConfigCommon(ctx); res != kCAILResultOK) { return res; }
    // ⚠️ Renoir msg：0x35 越出 Phoenix 表（ID 不存在），T1 闸拦截。
    if (const auto res = singleton().smuSendMessage(ctx, PPSMC_MSG_PowerGateMmHub);
        res != kCAILResultOK && res != kCAILResultUnsupported)
    {
        return res;
    }
    return kCAILResultOK;
}

CAILResult X5000HWLibs::smu10InternalHwInit(void* const ctx) { return smu10PowerUpConfig(ctx); }

bool X5000HWLibs::smu12IsFwLoaded(void* const ctx)
{
    return (singleton().smuCgsReadRegister(ctx, MP1_FIRMWARE_FLAGS, 0, kCAILHWBlockMP1, MP1_PUBLIC)
            & MP1_FIRMWARE_FLAGS_INTERRUPTS_ENABLED)
           != 0;
}

CAILResult X5000HWLibs::smu12WaitForFwLoaded(void* const ctx)
{
    return singleton().smuCosWaitFor(ctx, smu12IsFwLoaded, ctx,
                                     /*ctx->waitOnRegisterTimeout*/ PP_WAIT_ON_REGISTER_TIMEOUT_DEFAULT);
}

CAILResult X5000HWLibs::smu12PowerUpConfig(void* const ctx)
{
    if (const auto res = smuPowerUpConfigCommon(ctx); res != kCAILResultOK) { return res; }
    // ⚠️ Renoir msg：0x3D 越出 Phoenix 表（ID 不存在），T1 闸拦截。
    if (const auto res = singleton().smuSendMessage(ctx, PPSMC_MSG_PowerGateAtHub);
        res != kCAILResultOK && res != kCAILResultUnsupported)
    {
        return res;
    }
    return kCAILResultOK;
}

CAILResult X5000HWLibs::smu12InternalHwInit(void* const ctx)
{
    singleton().smuCtxCache = ctx;
    if (const auto res = smu12WaitForFwLoaded(ctx); res != kCAILResultOK) { return res; }

    return smu12PowerUpConfig(ctx);
}

bool X5000HWLibs::smu13IsFwLoaded(void* const ctx)
{
    // B81 观测点 B：打印 0x3010028 原值（值变化才打、≤4 行）。返回语义逐字不变。
    // 注（更正旧注释）：Apple 发送入口 0x683bd 的就绪谓词＝C2PMSG_90(0x29A)≠0【反汇编·确凿】；
    //   0x3010028 是我方判据，两者是否同源【未解出】。
    const UInt32 v = singleton().smuCgsReadRegister(ctx, 0x3010028, 0, kCAILHWBlockMP1, MP1_PUBLIC);
    static UInt32 sPrev = 0xDEADBEEFu; static unsigned sSeq = 0; static unsigned sCall = 0;
    ++sCall;
    if (v != sPrev) { sPrev = v; if (++sSeq <= 4) NRED_TRACE("fwflag28: v=0x%X seq=%u call=%u", v, sSeq, sCall); }
    return (v & MP1_FIRMWARE_FLAGS_INTERRUPTS_ENABLED) != 0;
}

CAILResult X5000HWLibs::smu13WaitForFwLoaded(void* const ctx)
{
    return singleton().smuCosWaitFor(ctx, smu13IsFwLoaded, ctx,
                                     /*ctx->waitOnRegisterTimeout*/ PP_WAIT_ON_REGISTER_TIMEOUT_DEFAULT);
}

CAILResult X5000HWLibs::smu13PowerUpConfig(void* const ctx)
{
    // PMFW 初始化序列（macOS 侧精简版，仿 Linux smu_v13_0）：唤起 BGM/IMU 客户端。
    // 顺序不可跳（SetDriverDramAddr → TransferTableDram2Smu → EnableGfxImu）。
    CAILResult res;

    // NRed 直读 MMIO 旁路：四步序列已移至 X6000FB::wrapControllerPowerUp（100% 被调用的挂载点）。
    // 此处保留原 ctx 路径逻辑不变（smu13PowerUpConfig 本身因 wrapper 休眠不会被调用，见 CONFIRMED §16）。
    //
    // ═══ 探针位改动（2026-09-25）：退役本路径的 rc 记录（原 bit8-39）═══
    //   原因：本路径与 SMU 旁路共用同一个 smu13ProbeState，而两者的位域曾经相交：
    //     ctx  : bit0-3 步掩码、bit8-15/16-23/24-31/32-39 四步 rc、bit60-63 生命周期
    //     旁路 : bit4-6、bit20-21/24-27/29、bit32-39/40-47/48-55 各步 rc + bit56-59
    //   → bit20-27 与 bit32-39 被两边同时使用。
    //   核算：两路径完全隔离需 40 + 34 = 74 位 > 一个 UInt64 的 64 位，无法靠重排解决；
    //   且 rc 值域到 160（kCAILResultPowerControlRefused），压缩到 4 位有损，不可行。
    //   取舍：本路径（ctx）已被证实从未执行——入口 _smu_9_0_send_message_with_parameter
    //   无 call 调用点、route 必然失败、已移出路由表；历史 state 的 bit0-3 恒为 0 佐证。
    //   故退役其 rc（32 位，释放最多），**保留 bit0-3 步掩码**——它仍是"本路径是否执行过"
    //   最直接的判据，且只需 4 位。每步 rc 仍会经 SYSLOG/DBGLOG 落到内核日志，并非完全丢失。
    //   旁路的 rc 字段语义不变（旁路是当前主力路径）。
    //   依据：docs 中关于探针位碰撞的分析（skill 780m-real-machine-test/references/panic-decode.md）。

    // a. SetDriverDramAddrHigh (0x0D), param=0
    DBGLOG("HWLibs", "smu13: sending msg 0x%X", PhoenixPPSMC::PPSMC_MSG_SetDriverDramAddrHigh);
    if ((res = singleton().smuSendMessage(ctx, PhoenixPPSMC::PPSMC_MSG_SetDriverDramAddrHigh, 0)) != kCAILResultOK
        && res != kCAILResultUnsupported)
    {
        NRED_TRACE("smu13: msg 0x%X failed: 0x%X", PhoenixPPSMC::PPSMC_MSG_SetDriverDramAddrHigh, res);
        NRed::singleton().orSmu13ProbeState(1ULL << 0);
        return res;
    }
    NRed::singleton().orSmu13ProbeState(1ULL << 0);

    // b. SetDriverDramAddrLow (0x0E), param=0
    DBGLOG("HWLibs", "smu13: sending msg 0x%X", PhoenixPPSMC::PPSMC_MSG_SetDriverDramAddrLow);
    if ((res = singleton().smuSendMessage(ctx, PhoenixPPSMC::PPSMC_MSG_SetDriverDramAddrLow, 0)) != kCAILResultOK
        && res != kCAILResultUnsupported)
    {
        NRED_TRACE("smu13: msg 0x%X failed: 0x%X", PhoenixPPSMC::PPSMC_MSG_SetDriverDramAddrLow, res);
        NRed::singleton().orSmu13ProbeState(1ULL << 1);
        return res;
    }
    NRed::singleton().orSmu13ProbeState(1ULL << 1);

    // c. TransferTableDram2Smu (0x10), param=0
    DBGLOG("HWLibs", "smu13: sending msg 0x%X", PhoenixPPSMC::PPSMC_MSG_TransferTableDram2Smu);
    if ((res = singleton().smuSendMessage(ctx, PhoenixPPSMC::PPSMC_MSG_TransferTableDram2Smu, 0)) != kCAILResultOK
        && res != kCAILResultUnsupported)
    {
        NRED_TRACE("smu13: msg 0x%X failed: 0x%X", PhoenixPPSMC::PPSMC_MSG_TransferTableDram2Smu, res);
        NRed::singleton().orSmu13ProbeState(1ULL << 2);
        return res;
    }
    NRed::singleton().orSmu13ProbeState(1ULL << 2);

    // d. EnableGfxImu (0x16), param=1 (ENABLE_IMU_ARG_GFXOFF_ENABLE)
    DBGLOG("HWLibs", "smu13: sending msg 0x%X", PhoenixPPSMC::PPSMC_MSG_EnableGfxImu);
    if ((res = singleton().smuSendMessage(ctx, PhoenixPPSMC::PPSMC_MSG_EnableGfxImu, 1)) != kCAILResultOK
        && res != kCAILResultUnsupported)
    {
        NRED_TRACE("smu13: msg 0x%X failed: 0x%X", PhoenixPPSMC::PPSMC_MSG_EnableGfxImu, res);
        NRed::singleton().orSmu13ProbeState(1ULL << 3);
        return res;
    }
    NRed::singleton().orSmu13ProbeState(1ULL << 3);

    return kCAILResultOK;
}

CAILResult X5000HWLibs::smu13InternalHwInit(void* const ctx)
{
    // ── B73：`smu13-init: enter` 入口行（**不依赖任何标志位**）──────────────────
    //  用途：分辨 B72 anomaly 的两个候选——(α) **从未被调用**（本行不出现，且 bit63 为 0）
    //  vs (β) **被调用但没返回/在内部阻塞**（本行出现，而其后的 `smu13: internal HW init
    //  done` 不出现）⇒ 由"本行是否出现 + 后续 done 行是否出现"直接分辨。
    //  位置＝函数**入口最前**（在既有 `setSmu13ProbeState(0)`／置 bit63 **之前**），
    //  理由：① 若内部阻塞，入口行已落盘 ⇒ 仍能判"进过"；② 不掩盖任何后续分支。
    //  通道＝nredTraceLine（内存环，零新增 SYSLOG）；定长单行 ≤191。
    {
        static char lineHi[96];
        const int nHi = snprintf(lineHi, sizeof(lineHi), "smu13-init: enter ctx=%llx\n",
                                 static_cast<unsigned long long>(reinterpret_cast<UInt64>(ctx)));
        if (nHi > 0) { nredTraceLine(lineHi, nHi); }
    }
    // Probe D1 v2: 每轮序列开始清零累积状态（若 WaitForFwLoaded 早退，state 保持 0 = "序列未执行"）
    NRed::singleton().setSmu13ProbeState(0);
    // Probe: 标记 smu13InternalHwInit 被调用过（第 63 位）
    NRed::singleton().orSmu13ProbeState(1ULL << 63);

    singleton().smuCtxCache = ctx;
    CAILResult ret = smu13WaitForFwLoaded(ctx);
    if (ret != kCAILResultOK) {
        NRED_TRACE("smu13: internal HW init done, ret=0x%X", ret);
        // Probe: 标记 WaitForFwLoaded 失败早退（第 61 位）
        NRed::singleton().orSmu13ProbeState(1ULL << 61);
        return ret;
    }

    // Probe: 标记 WaitForFwLoaded 成功（第 62 位）
    NRed::singleton().orSmu13ProbeState(1ULL << 62);
    ret = smu13PowerUpConfig(ctx);
    NRED_TRACE("smu13: internal HW init done, ret=0x%X", ret);
    return ret;
}
CAILResult X5000HWLibs::smu13NotifyEvent(void* const ctx, TTLEventInput* const input)
{
    // ★ A″-(7)（U-e 判别器）：notify event 入口自证 —— 钉死 `arg` 的来源/取值。
    //  ⚠️ 现有代码紧接其后即解引用 `input->arg`（下两行）⇒ 本行**不新增**任何空指针风险。
    NRED_TRACE("smu13: notify event entered, arg=%d", input->arg);
    if (input->arg >= SMU_EVENT_COUNT) {
        NRED_TRACE("Invalid input event to SMU notify event: %d", input->arg);
        return kCAILResultInvalidParameters;
    }

    if (input->arg == SMU_EVENT_POWER_UP || input->arg == 4 || input->arg == 8 || input->arg == SMU_EVENT_REINITIALISE)
    {
        // 超时-L1：power-up 分支进入 smu13PowerUpConfig，其内部消息将受闸过滤（非白名单即拒绝）。
        NRED_TRACE("smu13: notify power-up -> smu13PowerUpConfig (gate active)");
        return smu13PowerUpConfig(ctx);
    }

    return kCAILResultOK;
}

CAILResult X5000HWLibs::smu13FullAsicReset(void* const ctx, void* data)
{ return singleton().smuSendMessage(ctx, PhoenixPPSMC::PPSMC_MSG_GfxDeviceDriverReset, getMember<UInt32>(data, 4)); }

// VBIOSSMC: kHz → MHz (向上取整，对齐 Linux khz_to_mhz_ceil)
static inline UInt32 khzToMhzCeil(UInt32 khz) { return (khz + 999U) / 1000U; }

// ── 供 DCN314 时钟主流程内核态消费者使用的寄存器通道（第五步）─────────────────
// 语义约定：通道不可用（上下文为空 / cgs 指针未 resolve）时——读返回 0、写丢弃。
// 消费方靠"轮询超时或读值不匹配"识别失败；此为**静默退化**，调用方须在
// 调用前检查 smuContext()[见 AMDGFX9DCN314Display 的 applyDisplayClocks]。
void* X5000HWLibs::smuContext() { return singleton().smuCtxCache; }

// 231：FwBringup 挂点迁移（方案 B）。原 C2 出口 `0x6720b` 因模块 8 的 D2 删除 #20 而不可达
//  （230 轮 `[bringup] enter=0`）。新挂点＝`nredSmuSwInit` 的 #5 校验之后：此时 `I` 已证明为
//  704 字节 Apple 形状对象，与原挂点传入的 ctx 同一对象。
//  ⚠ 门控关 ⇒ 直接返回（零 MMIO、零 bringup 调用）；`nredFwBringupHook` 内部另有一次性守卫。
//  ⚠ 不写 `gGvmBook.failMask`、不改模块 8 的返回值。
void X5000HWLibs::runFwBringupAtModule8(void* const ctx)
{
    X5000HWLibs& hw = singleton();
    if (!hw.smu13FwBringupEnabled) { return; }
    fw::nredFwBringupHook(ctx);
    hw.smuCtxCache = ctx;   // 与原 C2 处对齐（I12）：迁移后 smuContext() 仍非空
}

// ⛔ r96：原 A″「早期 FW-loaded 探测」（smu13FwProbeOnly）**已整体删除，勿重建**。
//   删除理由：① 它经 smu13WaitForFwLoaded → smu13IsFwLoaded 读 SMN `0x3010028`，属
//   2026-09-29 所有者明令禁止的"探针式读 AMD 寄存器"类（见本文件 T7 探针族墓碑）；
//   ② 第 95 轮（r95awpb）实测：**真机首次执行即 page fault**（trap @ uptime 46.37 s，
//   与第 76/77 轮"~45 s 卡死"同一时间窗，属【推断·中】的共享根因）；
//   ③ 该路径"从未验证、首次执行即故障"。
//   ⇒ 如需"PMFW 是否加载"的读数：**先与所有者确认寄存器通道的安全性与必要性**；
//   不得复用 `-NRedProbePanic`（同为 PCIE 间接路径寄存器读）。

UInt32 X5000HWLibs::cgsReadReg(void* const ctx, const UInt32 off, const UInt32 blockInstance, const CAILHWBlock block,
                               const UInt32 regOffBase)
{
    auto& hw = singleton();
    if (ctx == nullptr || hw.smuCgsReadRegister == nullptr) { return 0; }
    return hw.smuCgsReadRegister(ctx, off, blockInstance, block, regOffBase);
}

void X5000HWLibs::cgsWriteReg(void* const ctx, const UInt32 off, const UInt32 val, const UInt32 blockInstance,
                              const CAILHWBlock block, const UInt32 regOffBase)
{
    auto& hw = singleton();
    if (ctx == nullptr || hw.smuCgsWriteRegister == nullptr) { return; }
    hw.smuCgsWriteRegister(ctx, off, blockInstance, val, block, regOffBase);
}

UInt32 X5000HWLibs::vbiossmcSendMsg(void* ctx, UInt32 msgId, UInt32 paramMHz)
{
    if (ctx == nullptr) {
        NRED_TRACE("VBIOSSMC send with null ctx (msg 0x%x)", msgId);
        return VBIOSSMC_Result_Failed;
    }

    auto& hw = singleton();

    // 1) Wait for SMU idle (C2PMSG_91 != BUSY)
    UInt32 res = VBIOSSMC_Status_BUSY;
    for (UInt32 tries = 0; tries < 200000; tries += 1) {
        res = hw.smuCgsReadRegister(ctx, MP1_SMN_C2PMSG_91, 0, kCAILHWBlockMP1, 0);
        if (res != VBIOSSMC_Status_BUSY) break;
        IODelay(10);
    }
    if (res == VBIOSSMC_Status_BUSY) {
        NRED_TRACE("VBIOSSMC busy timeout before send (msg 0x%x)", msgId);
        return VBIOSSMC_Result_Failed;
    }

    // 2) Clear response register (write VBIOSSMC_Status_BUSY = 0x0)
    hw.smuCgsWriteRegister(ctx, MP1_SMN_C2PMSG_91, 0, VBIOSSMC_Status_BUSY, kCAILHWBlockMP1, 0);

    // 3) Write parameter (MHz)
    hw.smuCgsWriteRegister(ctx, MP1_SMN_C2PMSG_83, 0, paramMHz, kCAILHWBlockMP1, 0);

    // 4) Write message ID (trigger)
    hw.smuCgsWriteRegister(ctx, MP1_SMN_C2PMSG_67, 0, msgId, kCAILHWBlockMP1, 0);

    // 5) Wait for completion
    res = VBIOSSMC_Status_BUSY;
    for (UInt32 tries = 0; tries < 200000; tries += 1) {
        res = hw.smuCgsReadRegister(ctx, MP1_SMN_C2PMSG_91, 0, kCAILHWBlockMP1, 0);
        if (res != VBIOSSMC_Status_BUSY) break;
        IODelay(10);
    }

    // 6) Handle failure
    if (res == VBIOSSMC_Result_Failed) {
        hw.smuCgsWriteRegister(ctx, MP1_SMN_C2PMSG_91, 0, VBIOSSMC_Result_OK, kCAILHWBlockMP1, 0);
        NRED_TRACE("VBIOSSMC msg 0x%x param %u failed", msgId, paramMHz);
        return VBIOSSMC_Result_Failed;
    }
    if (res == VBIOSSMC_Status_BUSY) {
        NRED_TRACE("VBIOSSMC timeout after send (msg 0x%x)", msgId);
        return VBIOSSMC_Result_Failed;
    }

    // 7) Return actual frequency (MHz) written back by SMU
    return hw.smuCgsReadRegister(ctx, MP1_SMN_C2PMSG_83, 0, kCAILHWBlockMP1, 0);
}

/*!
 * NRed 直读 MMIO 自实现 PMFW 消息发送 —— 绕开 Apple SMU ctx（smu90SendMessageWithParameter / smuCgsReadWriteRegister）。
 * C2PMSG 时序（对齐 Linux v1 协议：smu_cmn.c 的 __smu_msg_v1_send(:311) + __smu_msg_v1_poll_stat(:294) + smu_msg_v1_decode_response(:249)）：清 90 响应 → 写 82 参数 → 写 66 消息(触发) → 轮询 90 直到响应 → 判结果。
 * 基址采用 MP0_BASE_0 + MP0_SMN_C2PMSG_xx（32-bit dword 索引，对应 C2PMSG_90/82/66 偏移 0x29A/0x292/0x282）。
 */
CAILResult X5000HWLibs::smu13SendMsgDirect(const UInt32 msgId, const UInt32 param, UInt32 *rawResp)
{
    auto& nred = NRed::singleton();

    // 段口径按实现描述：现役路径走 SEG0 直接窗口（MP0_BASE_0；字节地址与 Linux SEG0 口径逐字节相等）。
    // "SEG1 才是正确段"作为真机子命题 B-4 另行登记（依据 T7 定论；见 kb/逆向事实与结论/乙线R1-SEG0-SEG1歧义定论.md）。
    const UInt32 regResp = MP0_BASE_0 + 0x29A;  // C2PMSG_90：响应（0=NoResponse，写 0 清）
    const UInt32 regArg  = MP0_BASE_0 + 0x292;  // C2PMSG_82：参数
    const UInt32 regMsg  = MP0_BASE_0 + 0x282;  // C2PMSG_66：命令（写即触发）

    // Linux __smu_msg_v1_send 时序：①清响应(90=0) ②写参数(82) ③写命令(66) ④轮询 90 != 0
    nred.writeReg32(regResp, 0);
    nred.writeReg32(regArg, param);
    nred.writeReg32(regMsg, msgId);

    // 轮询响应（C2PMSG_90 != 0，即 SMU 已写回结果）；0 = kSMUFWResponseNoResponse
    constexpr UInt32 kRespTimeout = 200000;   // 200000 × 10µs = 2s（与 vbiossmc 一致）
    UInt32 res = 0;
    for (UInt32 tries = 0; tries < kRespTimeout; tries += 1) {
        res = nred.readReg32(regResp);
        if (res != 0) { break; }
        IODelay(10);
    }

    if (rawResp != nullptr) { *rawResp = res; }   // 诊断：回传原始响应

    // 结果判定（PMFW 响应值：0x1=OK 0xFE=UnknownCmd 0xFD=RejectedPrereq 0xFC=RejectedBusy 0xFF=Failed）
    if (res == 0) {
        NRED_TRACE("smu13Direct: no response (msg 0x%x param %u)", msgId, param);
        return kCAILResultNoResponse;
    }
    if (res == VBIOSSMC_Result_Failed) {
        NRED_TRACE("smu13Direct: msg 0x%x param %u returned Failed", msgId, param);
        return kCAILResultFailed;
    }
    if (res != VBIOSSMC_Result_OK) {
        NRED_TRACE("smu13Direct: msg 0x%x param %u rejected, resp=0x%X", msgId, param, res);
        return kCAILResultUnsupported;
    }

    return kCAILResultOK;
}

// 空白对照（§16.43-1）：只清 resp、不写 msg，然后走同样的轮询。
// 若也返回非 0 ⇒ 说明"响应"来自垃圾值，判定逻辑假阳性。
UInt32 X5000HWLibs::smu13ProbeBlank()
{
    auto& nred = NRed::singleton();
    const UInt32 regResp = MP0_BASE_0 + 0x29A;   // §16.89 回滚: SEG0
    nred.writeReg32(regResp, 0);
    UInt32 res = 0;
    for (UInt32 i = 0; i < 200000; i += 1) {
        res = nred.readReg32(regResp);
        if (res != 0) { break; }
        IODelay(10);
    }
    return res;   // 期望 0；非 0 ⇒ 假阳性
}

// 读写一致性（§16.43-4）：向 resp 写已知值再读回，确认该地址真可写可读。
UInt32 X5000HWLibs::smu13ProbeRegRW()
{
    auto& nred = NRed::singleton();
    const UInt32 regResp = MP0_BASE_0 + 0x29A;   // §16.89 回滚: SEG0
    nred.writeReg32(regResp, 0x5A5A);
    const UInt32 back = nred.readReg32(regResp);
    nred.writeReg32(regResp, 0);
    return back;   // 期望 0x5A5A
}

CAILResult X5000HWLibs::smu13SetupDriverTableAndTransfer()
{
    // 分配物理连续 256B（SmuMetrics_t 160B + 对齐余量），供 SMU TransferTableDram2Smu DMA 写入。
    // 缓冲由本类持有（smu13MetricsBuffer），kext 生命周期内不释放：SMU 通过 Transfer 后仍指向该 DRAM 地址。
    // PAGE_SIZE 用常量（macOS 页 = 4096）保证物理对齐。
    auto& hw = singleton();
    if (hw.smu13MetricsBuffer != nullptr) {
        NRED_TRACE("smu13SetupDriverTableAndTransfer: buffer already allocated");
        return kCAILResultOK;
    }

    // 【§16.61 纠正】Phoenix = SMU v13_0_4（非 v13_0_7）。
    //   v13_0_4 的 SmuMetrics_t = 160 字节（§16.83 编译器实测；244 是 v13_0_7 残留），对齐 256。
    //   原 256B 分配本来就是对的；§16.56 的"2268B"基于错误版本，已作废。
    constexpr UInt32 kMetricsSize = 160U;                 // SmuMetrics_t v13_0_4: sizeof=160 align=4（§16.83 编译器实测；244 是 v13_0_7 残留）
    constexpr UInt32 kBufferSize  = 256U;                 // 244 向上对齐
    constexpr UInt32 kPageAlign   = 4096U;                // PAGE_SIZE

    IOBufferMemoryDescriptor* buf = IOBufferMemoryDescriptor::withOptions(
        kIOMemoryPhysicallyContiguous | kIODirectionInOut, kBufferSize, kPageAlign);
    if (buf == nullptr) {
        NRED_TRACE("smu13SetupDriverTableAndTransfer: alloc failed");
        return kCAILResultFailed;
    }
    if (buf->prepare() != kIOReturnSuccess) {
        NRED_TRACE("smu13SetupDriverTableAndTransfer: prepare failed");
        buf->release();
        return kCAILResultFailed;
    }

    // 取第一段物理段（应覆盖整块 256B）；addr64_t / IOByteCount 来自 IOMemoryDescriptor.h
    // getPhysicalSegment 第三参为 IOOptionBits（默认 0），传 0 即可。
    IOByteCount segLen = 0;
    const addr64_t phys = buf->getPhysicalSegment(0, &segLen, 0);
    if (phys == 0 || segLen < kMetricsSize) {
        NRED_TRACE("smu13SetupDriverTableAndTransfer: bad phys seg phys=0x%llX len=%llu",
               (unsigned long long)phys, (unsigned long long)segLen);
        buf->complete();
        buf->release();
        return kCAILResultFailed;
    }

    // 1) 通知 SMU 驱动表 DRAM 地址（高低 32 位）
    //    ⚠️ 地址域（§16.19）：PMFW 的 DMA 域只保证覆盖 FB carve-out 窗口——
    //    [fbOffset<<24, +visible_vram)。普通系统 RAM 页在窗口外 → Transfer 被拒（第 8/9 次实证）。
    //    方案：直接用 carve-out 窗口内地址 = (fbOffset<<24) + 0x1000（第二页，避开 VBIOS 常驻的第一页）。
    //    该内存的 CPU 侧映射/访问暂不需要（表内容全 0 即可，SMU 只做 DMA 拷贝）。
    //    探针：bit29 = carve-out 地址已使用（1）。
    // §16.40：getFbOffset() 已是 (raw & 0xFFFFFF) << 24 的结果，不能再左移（此前重复 <<24 导致垃圾地址）
    const UInt64 fbOff      = NRed::singleton().getFbOffset();
    // §16.52 第 3 项：把驱动表内容真正写到 BAR0 aperture 内（而非只给个地址）
    //   理由：PMFW DMA 域只覆盖 FB carve-out 窗口（§16.19/16.32-7）；
    //   系统 RAM buffer 的物理地址不被接受。必须让"地址处真的有内存"。
    //   做法：映射 BAR0，在 bar0Virt+0x1000 处清零 256B（= kBufferSize；SMU 只做 DMA 拷贝，内容全 0 即可）。
    //   失败则回退（地址仍给 carveout+0x1000，但内容无保障）。
    const addr64_t carveout = fbOff;
    const addr64_t addr     = carveout + 0x1000;

    bool apertureWritten = false;
    {
        IOMemoryMap* bar0Map = NRed::singleton().getIGPU()->mapDeviceMemoryWithRegister(
            kIOPCIConfigBaseAddress0, kIOMapInhibitCache | kIOMapAnywhere);
        if (bar0Map != nullptr && bar0Map->getLength() > 0x1000 + kBufferSize) {
            auto* bar0Virt = reinterpret_cast<volatile UInt8*>(bar0Map->getVirtualAddress());
            if (bar0Virt != nullptr) {
                for (UInt32 off = 0; off < kBufferSize; off += 1) {
                    bar0Virt[0x1000 + off] = 0;
                }
                apertureWritten = true;
                NRed::singleton().orSmu13ProbeState(1ULL << 5);    // 探针 bit5：aperture 已写入（bit20/21 已被占用！）
            }
        }
        if (bar0Map != nullptr) { bar0Map->release(); }
    }

    // §16.63/16.64：HDP flush —— 让 SMU 看到 CPU 刚写入的表内容（Linux amdgpu_hdp_flush 等价）
    //   寄存器：HDP_MISC_CNTL = HDP_BASE(0x0F20) + 0x00D3 = dword 0x0FF3
    //   位：FLUSH_INVALIDATE_CACHE（hdp_v4_0.c:151 WREG32_FIELD15(HDP,0,HDP_MISC_CNTL,FLUSH_INVALIDATE_CACHE,1)）
    //   ⚠️ 未做则 SMU 可能读到旧/无效数据（Linux 在 memcpy 后必做）
    {
        constexpr UInt32 kHdpMiscCntl = 0x0FF3;   // HDP_BASE(0x0F20) + regHDP_MISC_CNTL(0x00D3)
        const UInt32 old = NRed::singleton().readReg32(kHdpMiscCntl);
        NRed::singleton().writeReg32(kHdpMiscCntl, 1U << 0);   // FLUSH_INVALIDATE_CACHE = bit0
        (void)NRed::singleton().readReg32(kHdpMiscCntl);       // 回读触发
        NRed::singleton().orSmu13ProbeState(1ULL << 4);    // 探针 bit4：HDP flush 已执行（bit20/21 已被占用！）
        DBGLOG("HWLibs", "smu13: HDP flush (MISC_CNTL old=0x%X)", old);
    }
    DBGLOG("HWLibs", "smu13: aperture written=%s addr=0x%llX",
           apertureWritten ? "yes" : "no", (unsigned long long)addr);
    NRed::singleton().orSmu13ProbeState(1ULL << 29);   // 已改用 carve-out 地址

    // ═══ §16.70 报文响应矩阵【已移除，2026-09-11 23:05】═══
    //   移除理由（自审）：矩阵的目的（诊断 0x0D 为何无响应）已在 Manjaro 侧
    //   实测达成（§16.90：SEG0 有效、arg=8 是版本号、空白对照成立）。
    //   继续保留有害：它会在主序列【之前】发 0x0D(param=0) 和 0x0D(param=0x8)，
    //   把驱动表地址设成无效值（0 / 8），可能让 PMFW 进入异常状态（§16.20 教训：
    //   消息顺序会影响 PMFW 状态）。移除后主序列纯净执行。

    // §16.67：记录每步真实 resp（不能只在 panic 时读——那时早被后续消息覆盖）
    UInt32 respHigh = 0, respLow = 0, respXfer = 0;

    // ⛔ 地址域校验（2026-09-25 新增，防"panic 后不自动重启"复发）
    //   为什么必须挡：SetDriverDramAddrHigh/Low(0x0D/0x0E) 一旦被 PMFW 接受，
    //   后续 Transfer(0x10) 就会真的让 PMFW 接管我们给的驱动表地址；
    //   若地址不在 PMFW 的 DMA 域内，只会被拒（无害）；但若被接受而地址本身
    //   不是有效的驱动表缓冲，PMFW 会进入异常状态 → panic 后无法完成 reboot（§16.75/§16.95）。
    //   实测窗口（Manjaro dmesg + PCI resource）：VRAM 主机物理窗口 = 0x8000000000–0x80FFFFFFFF。
    //   注：fbOffset 的来源曾在 2026-09-11 从 getPhysicalAddress()（给 0x8D0000000，超窗）
    //   改为 PCI config 读 BAR0（给 0x8000000000，窗口内）。本检查保证无论来源如何，
    //   只有落在窗口内的地址才会被发给 PMFW。
    {
        constexpr UInt64 kVramWindowLo = 0x8000000000ULL;
        constexpr UInt64 kVramWindowHi = 0x80FFFFFFFFULL;
        // 表占 256B，要求整段都落在窗口内
        if (addr < kVramWindowLo || (addr + kBufferSize - 1) > kVramWindowHi) {
            // 探针位（逐位核算过全项目占用）：
            //   bit6     = 地址域校验拒绝
            //   bit56-59 = addr>>28 的低 4 位（定位超窗档位）
            // 当前占用全貌（两路径已解冲突）：
            //   ctx 路径：0-3（步掩码）、60-63（生命周期）——原 rc 域 8-39 已退役，见 smu13PowerUpConfig 注释
            //   旁路    ：4-5（HDP/aperture）、6（本次新增的拒绝位）、20/21/24/25/26/27/29（成功失败位）、
            //             32-39/40-47/48-55（各步 rc）、56-59（本次新增的档位）
            //   ⇒ 现在 8-19、22-23、28、30-31 也已空闲（退役后释放）。
            NRed::singleton().orSmu13ProbeState(1ULL << 6);
            NRed::singleton().orSmu13ProbeState(static_cast<UInt64>((addr >> 28) & 0xF) << 56);
            SYSLOG("HWLibs",
                   "smu13: 拒绝发送驱动表地址（超 VRAM 窗口）addr=0x%llX fbOff=0x%llX window=[0x8000000000,0x80FFFFFFFF]",
                   (unsigned long long)addr, (unsigned long long)fbOff);
            buf->complete();
            buf->release();
            return kCAILResultUnsupported;
        }
    }

    const auto rHigh = X5000HWLibs::smu13SendMsgDirect(
        PhoenixPPSMC::PPSMC_MSG_SetDriverDramAddrHigh, static_cast<UInt32>(addr >> 32), &respHigh);
    NRed::singleton().smu13Resp[0] = respHigh;
    DBGLOG("HWLibs", "smu13: SetDriverDramAddrHigh resp=0x%X", respHigh);
    if (rHigh != kCAILResultOK) {
        NRed::singleton().orSmu13ProbeState(1ULL << 26);   // High 设置失败
        NRed::singleton().orSmu13ProbeState(static_cast<UInt64>(rHigh & 0xFF) << 48);
        buf->complete();
        buf->release();
        return rHigh;
    }
    const auto rLow = X5000HWLibs::smu13SendMsgDirect(
        PhoenixPPSMC::PPSMC_MSG_SetDriverDramAddrLow, static_cast<UInt32>(addr & 0xFFFFFFFFU), &respLow);
    // §16.67：Low 的 resp 记到诊断（panic 输出用；不复用 bit20-21——那是成功位，bit22-23 已空闲，见 2213）
    NRed::singleton().smu13Resp[1] = respLow;
    DBGLOG("HWLibs", "smu13: SetDriverDramAddrLow resp=0x%X", respLow);
    if (rLow != kCAILResultOK) {
        NRed::singleton().orSmu13ProbeState(1ULL << 27);   // Low 设置失败
        NRed::singleton().orSmu13ProbeState(static_cast<UInt64>(rLow & 0xFF) << 48);
        buf->complete();
        buf->release();
        return rLow;
    }

    // 2) Transfer：argument=0, table_id=TABLE_SMU_METRICS=7
    //    【§16.83】Phoenix = SMU v13_0_4：driver_if_v13_0_4.h:277 TABLE_SMU_METRICS=7。
    //    （旧值 5 出自 v13_0_7 —— §16.61 版本错误的残留，2026-09-11 沉淀审查发现）
    //    之前 §16.55/16.59 误读了 v13_0_7 的表，结论作废，已回滚。
    //    【§16.19·已作废】Phoenix 表号曾误记为 5（drvif7.h:1601，属 v13_0_7 口径），已作废。
    //    （"此前传 7 已修正为 5"系旧稿误述；当前实现传 TABLE_SMU_METRICS=7，以 §16.83 为准。）
    const CAILResult r = X5000HWLibs::smu13SendMsgDirect(
        PhoenixPPSMC::PPSMC_MSG_TransferTableDram2Smu, static_cast<UInt32>((0U << 16) | 7U), &respXfer);
    NRed::singleton().smu13Resp[2] = respXfer;
    DBGLOG("HWLibs", "smu13: TransferTableDram2Smu resp=0x%X", respXfer);

    // 3) TransferTableSmu2Dram(0x0F)：从 PMFW 把 SmuMetrics 拉回驱动表地址
    //    【§16.84】BGM「可用」= 驱动能读到 metrics。SMU 在 SetDriverDramAddr 后持续
    //    刷新该地址；驱动必须发 0x0F（PMFW→DRAM）才能读回。此前只发 0x10 从未拉取。
    //    param 同为 ((argument<<16)|table_id)：argument=0, table_id=7（§16.83）。
    UInt32 respPull = 0;
    const CAILResult rPull = X5000HWLibs::smu13SendMsgDirect(
        PhoenixPPSMC::PPSMC_MSG_TransferTableSmu2Dram, static_cast<UInt32>((0U << 16) | 7U), &respPull);
    // 注意：不写入 smu13Resp[]（[3]-[7] 被响应矩阵占用，§16.70/16.65；不加 panic 字段=铁律 0a）
    DBGLOG("HWLibs", "smu13: TransferTableSmu2Dram resp=0x%X", respPull);

    // 持有缓冲（不 release），后续 metrics 读取可经 smu13MetricsBuffer 取虚拟地址
    hw.smu13MetricsBuffer = buf;
    // rDram2Smu 为写方向结果；拉取失败不影响写成功，但记录两者
    return (r != kCAILResultOK) ? r : rPull;
}

SInt32 X5000HWLibs::vbiossmcSetDispclk(void* ctx, UInt32 requestedKhz)
{
    UInt32 mhz = vbiossmcSendMsg(ctx, VBIOSSMC_MSG_SetDispclkFreq, khzToMhzCeil(requestedKhz));
    return (mhz == VBIOSSMC_Result_Failed) ? -1 : static_cast<SInt32>(mhz * 1000U);
}

SInt32 X5000HWLibs::vbiossmcSetDppclk(void* ctx, UInt32 requestedKhz)
{
    UInt32 mhz = vbiossmcSendMsg(ctx, VBIOSSMC_MSG_SetDppclkFreq, khzToMhzCeil(requestedKhz));
    return (mhz == VBIOSSMC_Result_Failed) ? -1 : static_cast<SInt32>(mhz * 1000U);
}

SInt32 X5000HWLibs::vbiossmcSetDprefclk(void* ctx, UInt32 requestedKhz)
{
    UInt32 mhz = vbiossmcSendMsg(ctx, VBIOSSMC_MSG_SetDprefclkFreq, khzToMhzCeil(requestedKhz));
    return (mhz == VBIOSSMC_Result_Failed) ? -1 : static_cast<SInt32>(mhz * 1000U);
}

SInt32 X5000HWLibs::vbiossmcSetHardMinDcfclk(void* ctx, UInt32 requestedKhz)
{
    UInt32 mhz = vbiossmcSendMsg(ctx, VBIOSSMC_MSG_SetHardMinDcfclkByFreq, khzToMhzCeil(requestedKhz));
    return (mhz == VBIOSSMC_Result_Failed) ? -1 : static_cast<SInt32>(mhz * 1000U);
}

SInt32 X5000HWLibs::vbiossmcSetMinDeepSleepDcfclk(void* ctx, UInt32 requestedKhz)
{
    UInt32 mhz = vbiossmcSendMsg(ctx, VBIOSSMC_MSG_SetMinDeepSleepDcfclk, khzToMhzCeil(requestedKhz));
    return (mhz == VBIOSSMC_Result_Failed) ? -1 : static_cast<SInt32>(mhz * 1000U);
}

void X5000HWLibs::vbiossmcSetDisplayIdleOptimizations(void* ctx, UInt32 idleInfo)
{
    vbiossmcSendMsg(ctx, VBIOSSMC_MSG_SetDisplayIdleOptimizations, idleInfo);
}

void X5000HWLibs::vbiossmcSetDisplayCount(void* ctx, UInt32 count)
{
    vbiossmcSendMsg(ctx, VBIOSSMC_MSG_SetDisplayCount, count);
}

SInt32 X5000HWLibs::vbiossmcSetDispclkCached(UInt32 requestedKhz)
{
    void* ctx = singleton().smuCtxCache;
    if (ctx == nullptr || !smu12IsFwLoaded(ctx)) {
        return static_cast<SInt32>(requestedKhz);
    }
    return vbiossmcSetDispclk(ctx, requestedKhz);
}

SInt32 X5000HWLibs::vbiossmcSetDppclkCached(UInt32 requestedKhz)
{
    void* ctx = singleton().smuCtxCache;
    if (ctx == nullptr || !smu12IsFwLoaded(ctx)) {
        return static_cast<SInt32>(requestedKhz);
    }
    return vbiossmcSetDppclk(ctx, requestedKhz);
}

CAILResult X5000HWLibs::smuInternalHwExit(void*) { return kCAILResultOK; }

CAILResult X5000HWLibs::smuFullAsicReset(void* const ctx, void* data)
{
    // ⚠️ Renoir msg：Phoenix 上 0x1E = SetSoftMaxFclkByFreq（同值不同义），T1 闸拦截。
    return singleton().smuSendMessage(ctx, PPSMC_MSG_DeviceDriverReset, getMember<UInt32>(data, 4));
}
CAILResult X5000HWLibs::smu10NotifyEvent(void* const ctx, TTLEventInput* const input)
{
    if (input->arg >= SMU_EVENT_COUNT) {
        NRED_TRACE("Invalid input event to SMU notify event: %d", input->arg);
        return kCAILResultInvalidParameters;
    }

    if (input->arg == SMU_EVENT_POWER_UP || input->arg == 4 || input->arg == 8 || input->arg == SMU_EVENT_REINITIALISE)
    {
        return smu10PowerUpConfig(ctx);
    }

    return kCAILResultOK;
}

CAILResult X5000HWLibs::smu12NotifyEvent(void* const ctx, TTLEventInput* const input)
{
    if (input->arg >= SMU_EVENT_COUNT) {
        NRED_TRACE("Invalid input event to SMU notify event: %d", input->arg);
        return kCAILResultInvalidParameters;
    }

    if (input->arg == SMU_EVENT_POWER_UP || input->arg == 4 || input->arg == 8 || input->arg == SMU_EVENT_REINITIALISE)
    {
        return smu12PowerUpConfig(ctx);
    }

    return kCAILResultOK;
}

CAILResult X5000HWLibs::smuFullScreenEvent(void* const ctx, const TTLFullScreenEvent event)
{
    switch (event) {
        case TTL_FULLSCREEN_EVENT_INCREASE:
            singleton().smuCgsWriteRegister(
                ctx, MP1_SMN_FPS_CNT, 0,
                singleton().smuCgsReadRegister(ctx, MP1_SMN_FPS_CNT, 0, kCAILHWBlockMP1, 0) + 1, kCAILHWBlockMP1, 0);
            return kCAILResultOK;
        case TTL_FULLSCREEN_EVENT_RESET:
            singleton().smuCgsWriteRegister(ctx, MP1_SMN_FPS_CNT, 0, 0, kCAILHWBlockMP1, 0);
            return kCAILResultOK;
        default:
            NRED_TRACE("Invalid input event to SMU full screen event: %d", event);
            return kCAILResultInvalidParameters;
    }
}

CAILResult X5000HWLibs::wrapSmuInitFunctionPointerList(void* const ctx, const SWIPIPVersion ipVersion)
{
    // B39 三层入口标记 ①：`smu-init-list: enter`（纯观测、零副作用、不改控制流）。
    //  用途：确认 0x6720b（smu_init_function_pointer_list）包装是否被调用；
    //  与 ②`bringup: enter` 配对即可分辨 0x6430f 的 ret=2 来自"未进 bringup"还是"进了但早退"。
    //  深栈纪律：只走 nredTraceLine（内存环），不调 SYSLOG/文件写。
    if (checkKernelArgument("-NRedAccelLog")) {
        char line[64];
        const int n = snprintf(line, sizeof(line), "smu-init-list: enter\n");
        if (n > 0) { nredTraceLine(line, n); }
    }
    if (singleton().orgSmuInitFunctionPointerList == 0) { return kCAILResultFailed; }
    CAILResult ret =
        FunctionCast(wrapSmuInitFunctionPointerList, singleton().orgSmuInitFunctionPointerList)(ctx, ipVersion);
    // B43 版本门放行（单变量）：0x6720b 对 id=2（0xa0000，本机）落 default 返 2 ⇒
    //  0x6430f 因 iVar7!=0 不调 0x67328 ⇒ (8,0) 判失败。反编译已证：改写 2→0 后
    //  0x6430f 继续 → 0x67328 → 0x6c0==0 → 返 0 ⇒ (8,0) 放行、暴露下一缺口。
    //  只改"门"的返回值；obj+0x2c8（真实版本 id）仍是 org 写下的原值，未被改写。
    //  其余返回值（0/1/其它）原样透传。深栈纪律：只走 nredTraceLine。
    //  `orgRet`：org 的**原始**返回值，专供下方既有观测行 `smu fp list: ret=%d` 使用
    //    （该行规格要求打印 org 原值以判"Apple 原路径报的什么码"）⇒ 不因放行而丢失诊断。
    const CAILResult orgRet = ret;
    // B44 org-ret 观测（零行为改动、无条件，紧邻既有返回处理）：B43 实测 bypass 行未出现而
    //  `smu-init-list: enter` 有 2 条 ⇒ 包装确实执行过 ⇒ 矛盾收窄为"org 原返回值到底是不是 2"。
    //  本行把 org 的**真实返回值**（orgRet）打出来供三分叉定因（enum 值 2=InvalidParameters）。
    //  ipver＝函数形参（Apple 上报的 SMU IP 版本），同函数现成可用，无需新增解引用；
    //  ⚠️ 注：org 内部版本识别读的是 obj 侧字段（FUN_00066d84 入参），与形参 ipver 未必相等，
    //     本行 ipver 仅作参考（判"Apple 认识什么版本"），定因以 org-ret 为准。深栈纪律：nredTraceLine。
    if (checkKernelArgument("-NRedAccelLog")) {
        char line[96];
        const int n = snprintf(line, sizeof(line), "smu-init-list: org-ret=%d ipver=%d.%d\n",
                               static_cast<int>(orgRet), ipVersion.major, ipVersion.minor);
        if (n > 0) { nredTraceLine(line, n); }
    }
    if (ret == 2) {
        if (checkKernelArgument("-NRedAccelLog")) {
            char line[96];
            const int n = snprintf(line, sizeof(line), "smu-init: version-gate bypass ret=2->0\n");
            if (n > 0) { nredTraceLine(line, n); }
        }
        ret = static_cast<CAILResult>(0);   // CAILResult 为枚举 ⇒ 需显式转换
    }
    if (ret == kCAILResultOK && !NRed::singleton().getAttributes().isPhoenix()) { return ret; }

    const auto effectiveMajor = NRed::singleton().getAttributes().isPhoenix() ? 13 : ipVersion.major;
    switch (effectiveMajor) {
        case 10: {
            singleton().smuInternalHWInitField(ctx) = reinterpret_cast<void*>(smu10InternalHwInit);
            singleton().smuNotifyEventField(ctx)    = reinterpret_cast<void*>(smu10NotifyEvent);
        } break;
        case 12: {
            singleton().smuInternalHWInitField(ctx) = reinterpret_cast<void*>(smu12InternalHwInit);
            singleton().smuNotifyEventField(ctx)    = reinterpret_cast<void*>(smu12NotifyEvent);
        } break;
        case 13: {
            NRED_TRACE("smu13: case 13 (SMU13 PMFW init) entered");
            singleton().smuInternalHWInitField(ctx) = reinterpret_cast<void*>(smu13InternalHwInit);
            singleton().smuNotifyEventField(ctx)    = reinterpret_cast<void*>(smu13NotifyEvent);
        } break;
        default: return ret;
    }

    if (currentKernelVersion() <= MACOS_10_15_X) {
        singleton().smuInternalSWInitField(ctx) = reinterpret_cast<void*>(smuInternalSwInitOld);
    }
    else {
        singleton().smuInternalSWInitField(ctx) = reinterpret_cast<void*>(smuInternalSwInit);
        singleton().smuGetUCodeConstsField(ctx) = reinterpret_cast<void*>(smuGetUCodeConsts);
    }
    singleton().smuFullscreenEventField(ctx) = reinterpret_cast<void*>(smuFullScreenEvent);
    singleton().smuInternalSWExitField(ctx)  = reinterpret_cast<void*>(retOK);
    singleton().smuInternalHWExitField(ctx)  = reinterpret_cast<void*>(smuInternalHwExit);
    if (effectiveMajor == 13) {
        singleton().smuFullAsicResetField(ctx) = reinterpret_cast<void*>(smu13FullAsicReset);
    }
    else {
        singleton().smuFullAsicResetField(ctx) = reinterpret_cast<void*>(smuFullAsicReset);
    }

    // ─── B79：盲试轮唯一行为改动——**一次**直调 `smu13InternalHwInit(ctx)` ─────────
    //  依据：技术经理规格（tmp/tech-manager/盲试轮可实现规格-2026-10-15.md §2/§4/§5/§6）：
    //    · Apple 入口 0x67423 语义＝读 `this+0x6C8`（＝刚写成的 smuInternalHWInitField）
    //      非空则调用 ⇒ 直调我方 static 成员在执行内容上等价，且省两处未知契约；
    //    · **一次性守卫**：本函数每引导约被调 5 次（B78 原位 `smu fp list:` 行 ×5）
    //      ⇒ 不加守卫会触发 5 次（5 组寄存器写/等待）⇒ 用函数内 `static bool`（kext
    //      __DATA ⇒ 每次引导归零）＋ `effectiveMajor == 13` 限定（语义＝只在 case 13
    //      分支上触发一次）；`n`＝static 计数器自增后值 ⇒ `n=1` 即"本轮只触发一次"自证。
    //    · 等待上界（**已核实**）：`smu13WaitForFwLoaded`＝`smuCosWaitFor(ctx, …,
    //      PP_WAIT_ON_REGISTER_TIMEOUT_DEFAULT=2000)`（Apple COS 助手，duration 有界）⇒ 有界。
    //    · 失败出口：ctx==null ⇒ 记行跳过；trig!=OK ⇒ 记行后继续；**不改**本函数返回值。
    //    · 记录行走 nredTraceLine 内存环；零新增 SYSLOG/门控/刷盘点；定长 ≤191。
    {
        static bool sB79TrigDone = false;   // kext __DATA ⇒ 每次引导归零（一次性）
        static unsigned sB79TrigN = 0;      // 自增后打印 ⇒ n=1 自证"本轮只触发一次"
        if (effectiveMajor == 13 && !sB79TrigDone) {
            sB79TrigDone = true;
            if (ctx == nullptr) {
                NRED_TRACE("smu-trigger: ctx null, skipped n=%u", ++sB79TrigN);
            } else {
                const CAILResult trig = X5000HWLibs::smu13InternalHwInit(ctx);
                NRED_TRACE("smu-trigger: ret=0x%X (case13) n=%u state=%llx",
                           static_cast<unsigned>(trig), ++sB79TrigN,
                           static_cast<unsigned long long>(NRed::singleton().getSmu13ProbeState()));
            }
        }
    }

    SYSLOG_COND(ADDPR(debugEnabled), "HWLibs", "Ignore error about unsupported SMU HW version.");

    // ─── 区分性日志（技术决策 agent 逐字规格，2026-10-03；**只追加、不改既有串**）───────
    //  背景：根因链已闭合到「SMU/SWIP init 失败 → `ttl_hw_init` 返回 false → `status=4`
    //   → `TTL::initialize` 提前返回 → `m_hTtl(+0x38)`/`pTtlSetPowerState(+0x5F8)` 未装配
    //   → `getRtsInfo` 失败 → panic」，但**尚不知是哪个子调用失败** ⇒ 本行把判定所需的三要素
    //   一次带出（判读用途逐条见规格）：
    //    · `ret`          = `org` 调用返回码 ⇒ 判"Apple 原路径是否确实报 unsupported、报的什么码"
    //    · `ipVersion.major/.minor` = Apple 上报的 SMU IP 版本 ⇒ 判"Apple 是否根本不认识这颗芯片"
    //    · `effectiveMajor` / `phoenix` = **自证用**（预期恒 13 / 1；若非 ⇒ 说明分支判断有异）
    //  ⛔ **纯观测**：不打印指针、**不改返回值**、**不改控制流** —— 本行插在
    //     `return kCAILResultOK;` **之前**，其下一条语句与返回值（常量）逐字未动；
    //     既有串 `Ignore error about unsupported SMU HW version.` 仅在**其后追加**、未改动。
    //  ⛔ 亦**不**为"消除日志/绕过检查"而改任何判定（治症状属禁止项）。
    //  ⚠️ 规格的逐字代码块用 `%d`，其上方注释又建议 `ret` 用 `%#x`——两者冲突；
    //    本实现**照逐字代码块**（`%d`）定稿（以"逐字规格"为准），已在交付中报告该分歧。
    //  ⚠️ B43：下行打印改用 `orgRet`（**放行前**的 org 原值）而非 `ret`，以免 `ret=2->0` 的放行
    //     把"Apple 原路径报的什么码"这一诊断信息抹掉（本行规格：判 Apple 是否报 unsupported 及何码）。
    SYSLOG_COND(ADDPR(debugEnabled), "HWLibs", "smu fp list: ret=%d ipVer=%d.%d effMajor=%d phoenix=%d", orgRet,
                ipVersion.major, ipVersion.minor, effectiveMajor,
                NRed::singleton().getAttributes().isPhoenix() ? 1 : 0);
    // ─── A-1（2026-10-10）：固件层第一增量（C2 挂点＝本函数出口；门控 `-NRedFwBringup` 默认关）──
    //  门控假 ⇒ 零 MMIO、零 bringup 调用（本块不执行）。
    //  门控真 ⇒ 早于 S4（AMDGFX9DCN314Display::init 显示时钟）执行 fw::nredFwBringupHook(ctx)：
    //    构造 BringupCtx/RegSinkKernel（SEG0 基准、RegSink 抽象、IODelay 注入）→ bringupRun → NRED_TRACE；
    //    并置有效 smuCtxCache（I12）⇒ makeSmuChannel 在 S4 不再因 smuContext()==null 返 false。
    //  段基址语义（G4/C-1）：本 bringup 的 MP0/MP1 邮箱取 **SEG0**（`kMpSeg0Base`；A-2 依真机实证
    //    「SEG0 三条有效／SEG1 三条全超时」选定），与 `RegAddr.hpp::smnAddr` 的 SEG1 兼容默认**语义
    //    不同**（SEG1 是 Linux BASE_IDX=1 的理论通道，受 `mc_access_enable` 未使能所限）；SEG0/SEG1
    //    孰为真通道（R6/U4）仍无定论 ⇒ 此处只记录当前取值，不断言哪条正确。
    if (singleton().smu13FwBringupEnabled) {
        fw::nredFwBringupHook(ctx);
        singleton().smuCtxCache = ctx;   // I12：置有效 Apple SMU ctx（早于 S4；门控真才执行）
    }
    // D-3 只读仪表（门控 `-NRedSegReadout`，默认关）：C2 出口执行只读仪表（早于 S4、不写、不发送）。
    if (singleton().smu13SegReadoutEnabled) {
        fw::nredSegReadoutHook(nullptr);
    }
    // A-27（甲）线读数（门控 `-NRedClkLayoutReadout`，默认关）：**C2 出口延迟读**——
    //  读 `wrapDcClkMgrCreate` 存入的 `gClkMgrPtr`/`gPpSmuPtr`/`gDcCtxPtr`。
    //  双向自证：门控真 ⇒ 必出 `clk-layout-*`（含哨兵行 `clk-layout-S:`）；门控假 ⇒ 零输出。
    //  通道：NRED_TRACE → A-22 内存环 → 安全时点刷盘（零文件系统写、零 MMIO、零寄存器探针）。
    if (singleton().smu13ClkLayoutEnabled) {
        const uint32_t sent = fw::clkLayoutSentinel(gClkMgrCreateCalls, gClkMgrPtr);
        // 哨兵行：区分"dc_clk_mgr_create 未被调用"(=1) 与"调用了但 clk_mgr 为空"(=2)
        NRED_TRACE("clk-layout-S: sentinel=%u(%s) calls=%llu clk_mgr=0x%llX",
                   sent, fw::clkLayoutSentinelText(sent),
                   (unsigned long long)gClkMgrCreateCalls, (unsigned long long)gClkMgrPtr);
        if (sent == 0) {
            fw::nredClkLayoutReadoutHook(gClkMgrPtr, gPpSmuPtr, gDcCtxPtr,
                                         gNRedPpSmuOverlayCbAddr,
                                         gNRedPpSmuOverlayCalls);
        }
    }
    return kCAILResultOK;
}

// Actual code creates an `IOMemoryDescriptor` and does nothing with it.
// I ran into issues so I just gave it a random OSObject it can call `release` on.
static void* allocMemHandle()
{
    const auto v = OSBoolean::withBoolean(false);
    assertf(v != nullptr, "Failed to create memory handle!");
    return v;
}

static inline void setGCFWData(void* const ctx, GCFirmwareInfo* const fwData, const GCFirmwareType i,
                               const GCFirmwareConstant* const entry)
{
    fwData->entry[i]                  = entry;
    fwData->handle[i]                 = allocMemHandle();
    getMember<void*[]>(ctx, 0x18)[i]  = fwData->handle[i];
    fwData->count                    += 1;
}

// TODO: Replace this with `gc_read_config_setting_uint32` on `AsicRevForRlcFw`.
static bool isA0()
{
    return !NRed::singleton().getAttributes().isPicasso()
           || ((NRed::singleton().getPciRevision() >= 0xC8 && NRed::singleton().getPciRevision() <= 0xCF)
               || (NRed::singleton().getPciRevision() >= 0xD8 && NRed::singleton().getPciRevision() <= 0xDF));
}

void X5000HWLibs::gc91GetFwConstants(void* const ctx, GCFirmwareInfo* const fwData)
{
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLCSRListCntl, &gc_9_1_rlc_srlist_cntl);
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLCSRListGPMMem, &gc_9_1_rlc_srlist_gpm_mem);
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLCSRListSRMMem, &gc_9_1_rlc_srlist_srm_mem);
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLC, isA0() ? &gc_9_1_rlc_ucode_a0 : &gc_9_1_rlc_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypeME, &gc_9_1_me_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypeCE, &gc_9_1_ce_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypePFP, &gc_9_1_pfp_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypeMEC1, &gc_9_1_mec_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypeMECJT1, &gc_9_1_mec_jt_ucode);
    // AMD: Yes, reuse that shit! Why would we waste a couple of bytes? It's not like we're wasting hundreds of MBs
    // already from the duplicate firmware files.
    fwData->entry[kGCFirmwareTypeMECJT2]   = fwData->entry[kGCFirmwareTypeMECJT1];
    fwData->handle[kGCFirmwareTypeMECJT2]  = fwData->handle[kGCFirmwareTypeMECJT1];
    fwData->count                         += 1;
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLCV, &gc_9_1_rlcv_ucode);
}

void X5000HWLibs::gc92GetFwConstants(void* const ctx, GCFirmwareInfo* const fwData)
{
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLCSRListCntl, &gc_9_2_rlc_srlist_cntl);
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLCSRListGPMMem, &gc_9_2_rlc_srlist_gpm_mem);
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLCSRListSRMMem, &gc_9_2_rlc_srlist_srm_mem);
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLC, &gc_9_2_rlc_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypeME, &gc_9_2_me_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypeCE, &gc_9_2_ce_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypePFP, &gc_9_2_pfp_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypeMEC1, &gc_9_2_mec_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypeMECJT1, &gc_9_2_mec_jt_ucode);
    fwData->entry[kGCFirmwareTypeMECJT2]   = fwData->entry[kGCFirmwareTypeMECJT1];
    fwData->handle[kGCFirmwareTypeMECJT2]  = fwData->handle[kGCFirmwareTypeMECJT1];
    fwData->count                         += 1;
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLCV, &gc_9_2_rlcv_ucode);
}

void X5000HWLibs::gc93GetFwConstants(void* const ctx, GCFirmwareInfo* const fwData)
{
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLCSRListCntl, &gc_9_3_rlc_srlist_cntl);
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLCSRListGPMMem, &gc_9_3_rlc_srlist_gpm_mem);
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLCSRListSRMMem, &gc_9_3_rlc_srlist_srm_mem);
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLC, &gc_9_3_rlc_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypeME, &gc_9_3_me_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypeCE, &gc_9_3_ce_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypePFP, &gc_9_3_pfp_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypeMEC1, &gc_9_3_mec_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypeMECJT1, &gc_9_3_mec_jt_ucode);
}

// Port of `*_char_to_int` from HWLibs.
static constexpr UInt32 charToInt(const char* str, size_t len)
{
    if (str == nullptr || len == 0) { return 0; }
    UInt32 ret = 0;
    while (len > 0) {
        char c = *str;
        if (c < '0' || c > '9') { ret = 0; }
        else {
            ret *= 10;
            ret += static_cast<UInt32>(c - '0');
        }
        str += 1;
        len -= 1;
    }
    return ret;
}

void X5000HWLibs::processGCFWEntries(void* const ctx, void* const initData)
{
    const auto& fwInfo    = singleton().gcSwFirmwareField(ctx);
    auto&       fwEntries = getMember<GCFirmwareEntry[kGCFirmwareTypeCount]>(initData, 0x18);
    for (UInt32 i = 0, swIndex = 0; i < kGCFirmwareTypeCount; i++) {
        if (fwInfo.entry[i] == nullptr) { continue; }

        fwEntries[swIndex].valid       = true;
        fwEntries[swIndex].id          = static_cast<GCFirmwareType>(i);
        fwEntries[swIndex].rom         = fwInfo.entry[i]->rom;
        fwEntries[swIndex].romSize     = fwInfo.entry[i]->romSize;
        fwEntries[swIndex].handle      = fwInfo.handle[i];
        fwEntries[swIndex].payloadOff  = (i == kGCFirmwareTypeMEC1 || i == kGCFirmwareTypeMEC2) ? 0x1000 : 0x0;
        fwEntries[swIndex].version     = charToInt(fwInfo.entry[i]->version, strlen(fwInfo.entry[i]->version));
        fwEntries[swIndex].field24     = fwInfo.entry[i]->field8;
        swIndex                       += 1;
        if (swIndex == fwInfo.count) { break; }
    }
    getMember<UInt32>(initData, 0x10) = fwInfo.count;
}

CAILResult X5000HWLibs::wrapGcSetFwEntryInfo(void* const ctx, const SWIPIPVersion ipVersion, void* const initData)
{
    auto* fwInfo  = &singleton().gcSwFirmwareField(ctx);
    fwInfo->count = 0;
    switch (ipVersion.toHW()) {
        case SWIPIPVersion(9, 1, 0).toHW(): {
            gc91GetFwConstants(ctx, fwInfo);
        } break;
        case SWIPIPVersion(9, 2, 0).toHW(): {
            gc92GetFwConstants(ctx, fwInfo);
        } break;
        case SWIPIPVersion(9, 3, 0).toHW(): {
            gc93GetFwConstants(ctx, fwInfo);
        } break;
        default:
            if (singleton().orgGcSetFwEntryInfo == 0) { return kCAILResultFailed; }
            return FunctionCast(wrapGcSetFwEntryInfo, singleton().orgGcSetFwEntryInfo)(ctx, ipVersion, initData);
    }
    processGCFWEntries(ctx, initData);
    return kCAILResultOK;
}

static void setDMCUFWData(void* const ctx, DMCUFirmwareInfo* const fwData, const DMCUFirmwareType i,
                          const DMCUFirmwareConstant* const fwEntry)
{
    fwData->entry[i].loadAddress = fwEntry->loadAddress;
    fwData->entry[i].romSize     = fwEntry->romSize;
    fwData->entry[i].rom         = fwEntry->rom;
    fwData->entry[i].handle      = allocMemHandle();
    assertf(fwData->entry[i].handle != nullptr, "Failed to create memory handle!");
    getMember<void*[]>(ctx, 0x18)[i]  = fwData->entry[i].handle;
    fwData->count                    += 1;
}

bool X5000HWLibs::getDcn1FwConstants(void* const ctx, DMCUFirmwareInfo* const fwData)
{
    const auto enablePSPFWLoad = singleton().dmcuEnablePSPFWLoadField(ctx);
    if (enablePSPFWLoad == 2) { return true; }

    fwData->count = 0;

    const auto abmLevel = singleton().dmcuABMLevelField(ctx);
    switch (abmLevel) {
        case 0: {
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeERAM, &dmcu_eram_dcn10_abm_2_1);
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeISR, &dmcu_intvectors_dcn10_abm_2_1);
        } break;
        case 1: {
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeERAM, &dmcu_eram_dcn10_abm_2_2);
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeISR, &dmcu_intvectors_dcn10_abm_2_2);
        } break;
        case 2: {
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeERAM, &dmcu_eram_dcn10_abm_2_3);
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeISR, &dmcu_intvectors_dcn10_abm_2_3);
        } break;
        default: NRED_TRACE("Invalid ABM Level (0x%X) for DCN 1!", abmLevel); return false;
    }

    return true;
}

bool X5000HWLibs::getDcn21FwConstants(void* const ctx, DMCUFirmwareInfo* const fwData)
{
    const auto enablePSPFWLoad = singleton().dmcuEnablePSPFWLoadField(ctx);
    if (enablePSPFWLoad == 2) { return true; }

    fwData->count = 0;

    const auto abmLevel = singleton().dmcuABMLevelField(ctx);
    switch (abmLevel) {
        case 0: {
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeERAM, &dmcu_eram_dcn21_abm_2_1);
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeISR, &dmcu_intvectors_dcn21_abm_2_1);
        } break;
        case 1: {
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeERAM, &dmcu_eram_dcn21_abm_2_2);
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeISR, &dmcu_intvectors_dcn21_abm_2_2);
        } break;
        case 2: {
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeERAM, &dmcu_eram_dcn21_abm_2_3);
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeISR, &dmcu_intvectors_dcn21_abm_2_3);
        } break;
        case 3: {
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeERAM, &dmcu_eram_dcn21_abm_2_4);
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeISR, &dmcu_intvectors_dcn21_abm_2_4);
        } break;
        default: NRED_TRACE("Invalid ABM Level (0x%X) for DCN 2.1!", abmLevel); return false;
    }

    return true;
}

static bool sdma41GetFWConstants(void*, const SDMAFWConstant** const out)
{
    *out = &sdma_4_1_ucode;
    return true;
}

bool X5000HWLibs::sdma412StartEngine(void* const ctx)
{
    singleton().sdmaCgsWriteRegister(
        ctx, SDMA0_F32_CNTL, 0,
        singleton().sdmaCgsReadRegister(ctx, SDMA0_F32_CNTL, 0, /*ctx->hwblock.id*/ kCAILHWBlockSDMA0)
            & ~SDMA0_F32_CNTL_HALT,
        /*ctx->hwblock.id*/ kCAILHWBlockSDMA0);
    return true;
}

static constexpr UInt32 sdmaGetHWVersion(const UInt32 major, const UInt32 minor) { return minor | (major << 16); }

CAILResult X5000HWLibs::wrapSdmaInitFunctionPointerList(void* const ctx, const UInt32 major, const UInt32 minor,
                                                        const UInt32 patch)
{
    switch (sdmaGetHWVersion(major, minor)) {
        case sdmaGetHWVersion(4, 1): {
            singleton().sdmaGetFwConstantsField(ctx) = sdma41GetFWConstants;
            if (patch == 2) { singleton().sdmaStartEngineField(ctx) = sdma412StartEngine; }
        } break;
        default:
            if (singleton().orgSdmaInitFunctionPointerList == 0) { return kCAILResultFailed; }
            return FunctionCast(wrapSdmaInitFunctionPointerList,
                                singleton().orgSdmaInitFunctionPointerList)(ctx, major, minor, patch);
    }
    return kCAILResultOK;
}
