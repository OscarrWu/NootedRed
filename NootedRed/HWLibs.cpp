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
// 本 kext 的运行时基址（＝ `processKext` 的 `slide`）。供 N-2 把 RA 归零到 VM 地址。
//  ⚠ 与 `kcSlide` 无关（该成员全仓库只读不写、恒 0 ⇒ 用它归零会得到假读数）。
static UInt64 gHwlibsSlide = 0;
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
    //  ⚠ 零行为：只读我方标量（由三个**既有** wrapper 写入），本处**零解引用**、零 SYSLOG、零 panic 文本。
    if (gTtlBisect && off < sizeof(body)) {
        const size_t cap = sizeof(body) - off;
        const int n8 = snprintf(body + off, cap,
                                "[ttlbisect] c4n=%u c4r=%u c4a4=%u c4b4=%u c4c4=%u site=%c off=%llx "
                                "Ee8=%llx r28=%llx r30=%llx s2c=%u s3c=%u "
                                "p1=%u/%u/%u p2=%u/%u/%u p3=%u/%u/%u "
                                "o3=%llx o4=%llx c14=%llx f112=%llx f600=%llx f616=%llx "
                                "v176=%08x vb=%u a=%u x=%u a2=%u rb=%u "
                                "r14i=%llx ri=%u reci=%llx rc=%u hit=%u sh=%s\n",
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
                                static_cast<unsigned>(gBisHit),
                                kBisShortReason[(gBisShort < (sizeof(kBisShortReason) / sizeof(kBisShortReason[0])))
                                                    ? gBisShort : 0]);
        if (n8 > 0) { off += (static_cast<size_t>(n8) < cap) ? static_cast<size_t>(n8) : cap; }
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

// P7c 包D：判据④ `0x929ac`（TlsExecuteIpEntrySeq）只读钩子——只记返回值，逐字透传。
//  判假条件（同 §D.5）：逐项 `0x9440f(*(obj+8), ids[i], sub, out)` 任一项返 0 ⇒ 断言 ttl_tls.c:357 ⇒ 返 0。
UInt32 X5000HWLibs::wrapIpEntrySeq(void* const a, const UInt32 b, const UInt32 c)
{
    const UInt32 ret = (singleton().orgIpEntrySeq != 0)
                           ? FunctionCast(wrapIpEntrySeq, singleton().orgIpEntrySeq)(a, b, c) : 0;
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
    const UInt32 ret = FunctionCast(wrapTtlInit, singleton().orgTtlInit)(a1, a2);
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
            nredTraceFlush();   // B67 保守变体：仅此处 flush 一次（深栈少 I/O）
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
    const UInt32 ret = FunctionCast(wrapBgmInit, singleton().orgBgmInit)(a, b, c, d, e);
    nredNoteBgmStep("bgm-init", reinterpret_cast<UInt64>(a), reinterpret_cast<UInt64>(b),
                    reinterpret_cast<UInt64>(c), ret);
    return ret;
}

UInt32 X5000HWLibs::wrapBgmStep1(void* const a, void* const b, void* const c, void* const d)
{
    const UInt32 ret = FunctionCast(wrapBgmStep1, singleton().orgBgmStep1)(a, b, c, d);
    if (checkKernelArgument("-NRedAccelLog")) {
        // `d` = 输出结构（`0x2aacbb` 的判据 ② 读它的 +4，要求为 1；判据 ① 读 +0）
        UInt64 o0 = 0, o8 = 0;
        if (nredKernelPtrOk(d)) {
            o0 = reinterpret_cast<UInt64*>(d)[0];
            o8 = reinterpret_cast<UInt64*>(d)[1];
        }
        NRED_TRACE("bgm-step1: a1=%llx a2=%llx a3=%llx out=%llx o0=%llx o8=%llx ret=%u",
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(a)),
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(b)),
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(c)),
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(d)), static_cast<unsigned long long>(o0),
               static_cast<unsigned long long>(o8), static_cast<unsigned>(ret));
    }
    return ret;
}

UInt32 X5000HWLibs::wrapBgmQuery(void* const a, void* const b, void* const c)
{
    // ⚠️ 已静音（2026-09-28）：本函数是热路径（单轮 ~2000 次调用），输出会挤满 L2 环形缓冲、
    //  使关键观测行（`mode2-tail`/`ip`）落不了盘。信息已收集完毕；仍保留 hook（行为不变）。
    return FunctionCast(wrapBgmQuery, singleton().orgBgmQuery)(a, b, c);
}

UInt32 X5000HWLibs::wrapBgmStep4(void* const a)
{
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
        NRED_TRACE("bgm-step4: obj=%llx inner=%llx mode=%u f2fa8=%llx f2fb8=%llx ret=%u",
               static_cast<unsigned long long>(reinterpret_cast<UInt64>(a)), static_cast<unsigned long long>(inner),
               static_cast<unsigned>(mode), static_cast<unsigned long long>(f2fa8),
               static_cast<unsigned long long>(f2fb8), static_cast<unsigned>(ret));
    }
    return ret;
}

UInt32 X5000HWLibs::wrapReadSel(void* const obj, const UInt32 sel, const UInt32 a2, const UInt32 a3, void* const buf,
                                const UInt32 a5)
{
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
    const UInt32 ret = FunctionCast(wrapCfgRead, singleton().orgCfgRead)(obj, id, out, flag);
    // ⚠️ 已静音（2026-09-28）：本函数是热路径（单轮 ~1800 次调用），其输出会把 L2 环形缓冲挤满，
    //  导致真正关键的观测行（`mode2-tail`/`ip`）落不了盘。信息已收集完毕，需要时再临时打开。
    (void)before;
    return ret;
}

UInt32 X5000HWLibs::wrapMode2Tail(void* const a, void* const b, void* const c, void* const d, void* const e)
{
    const UInt32 ret = FunctionCast(wrapMode2Tail, singleton().orgMode2Tail)(a, b, c, d, e);
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

UInt32 X5000HWLibs::wrapMode9440f(void* const a, UInt32 const b, UInt32 const c, void* const d)
{
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
                        setShort(kBisShortRec);
                    }
                    else {
                        const UInt8* const P = static_cast<const UInt8*>(pool);
                        const UInt32 idx = *reinterpret_cast<const UInt32*>(P + (5 << 5));
                        if (idx == 0xFFFFFFFFU) {
                            // 0x958d8 在 idx==0xFFFFFFFF 时返 0（`9590e/95911`）⇒ "真读到 0"（② 成立）
                            gBisReci = 0; gBisReciRead = 1;
                            setShort(kBisShortRec);
                        }
                        else {
                            const UInt64 recAddr = *reinterpret_cast<const UInt64*>(P + 608)
                                                 + 3652ULL * static_cast<UInt64>(idx);
                            // reci＝0x958d8 的原样返回（raw 地址，**不做** ptr-ok：Apple 亦不校验，
                            //  非 0 ⟺ 记录可查 ⟺ ② 不成立）。rc=1 ⇒ 真读到（含真读到 0 的情况）。
                            gBisReci = recAddr; gBisReciRead = 1;
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

// ─── (8,0) 失败点读点（2026-10-12 技术经理终审；深栈安全——只走 nredTraceLine）────────
//  ⚠️ 本卡单变量＝仅恢复 H-96c4c 一个读点（B31 memdump 已证真机模式一致；其余读点
//  9e505/6430f/6720b/956b8/9e757 本轮不加，后续逐轮单变量）。

// H-96c4c：回退路径读点（type3 槽 +0x30 虚槽 callee）。行 `96c4c: hw=%X ret=%u`。
//  hw = 入参 hw_id（发现块块 hw_id，0x956b8 循环传入）；ret = 返回值（1=填充成功/0=失败）。
//  覆盖函数两个 return 出口（成功填表 return 1 / 失败 return 0，统一在出口读）。
UInt32 X5000HWLibs::wrapMode96c4c(void* const obj, UInt32 const hw, void* const out)
{
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
                                   const char* tag)
        {
            req.from = from;
            if (patcher.routeMultiple(id, &req, 1, slide, size)) {
                DBGLOG("HWLibs", "%s: hooked", tag);
                nHooks += 1;
            }
            else { NRED_TRACE("%s: route failed", tag); }
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
        if (const auto from = resolveProbe("read-sel", kReadSelPattern, nullptr, arrsize(kReadSelPattern), 0x29A19E)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapReadSel, this->orgReadSel}, "read-sel");
        }
        if (const auto from = resolveProbe("cfg-read", kCfgReadPattern, nullptr, arrsize(kCfgReadPattern),
                                           0x2AB398)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapCfgRead, this->orgCfgRead}, "cfg-read");
        }
        if (const auto from = resolveProbe("mode2-tail", kMode2TailPattern, nullptr, arrsize(kMode2TailPattern),
                                           0x2AB00E)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapMode2Tail, this->orgMode2Tail}, "mode2-tail");
        }
        if (const auto from = resolveProbe("mode9440f", kMode9440fPattern, nullptr, arrsize(kMode9440fPattern),
                                           0x9440F)) {
            hookProbe(from, KernelPatcher::RouteRequest{nullptr, wrapMode9440f, this->orgMode9440f}, "mode9440f");
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
        default: return FunctionCast(wrapGcSetFwEntryInfo, singleton().orgGcSetFwEntryInfo)(ctx, ipVersion, initData);
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
            return FunctionCast(wrapSdmaInitFunctionPointerList,
                                singleton().orgSdmaInitFunctionPointerList)(ctx, major, minor, patch);
    }
    return kCAILResultOK;
}
