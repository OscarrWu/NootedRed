// =============================================================================
//  NvDiag.hpp —— 【诊断用探针】NVRAM 写入能力 + msgbuf 可达性 诊断
//
//  为什么需要它（2026-09-27 立案）
//    目标：建立"NVRAM 日志通道"（把诊断信息写进 NVRAM 自定义变量，重启回 Manjaro 后经
//    efivarfs 直读），以解除 panic 通道的四重限制（容量小/格式串冻结/只留最后一次/观测即打断）。
//    方案与风险清单：docs/NVRAM观测通道方案与风险评估.md
//
//    既有实测（2026-09-26 第 2 批次）：`NVStorage::init` 成功、`write` 与 `sync` 均失败
//    （自检位 nvram init=1 write=0 sync=0）。离线分析（kb/re/AppleEFINVRAM写入判据报告.md）
//    已排除 "CSR bit6" 假设，但**无法解释 write=0**。剩余未知只能真机回答：
//      · `/options` 节点的类名（是否 AppleEFINVRAM）
//      · 读通道（getProperty）是否工作
//      · 写自定义 GUID 与 Apple GUID 的差异
//      · safeToSync 的返回值
//
//    同时（2026-09-27 离线取证）：内核符号表里存在 `_msgbufp`（n_type 含 N_EXT），
//    `struct msgbuf` 静态初值实测 magic=0x63061 / size=131072(128KB) / bufc=&_smsg_bufc。
//    ⇒ 本探针顺带用**弱引用**探测该符号能否被 kext 链接/解析，以及运行期是否可读。
//    这是"把 Apple/内核日志全部拿到"路线的第一步可行性验证。
//
//  用法（默认关闭，必须显式传 boot-arg；符合"探针默认不生效"的纪律）
//    · `-NRedNvDiagRead`  ：**只读**诊断（fromPath / cast / getProperty / msgbuf 元数据）
//    · `-NRedNvDiagWrite` ：在只读基础上加 2 条写测试（自定义 GUID + Apple GUID）+ sync
//    ⛔ 两者不要同时开（更早的 panic 会先触发）；先跑 Read 以最小风险取回现场。
//
//  设计要点
//    · 所有读数预先求值为局部标量，供调用方用 panic 带出（panic 实参不得含函数调用）；
//    · 写入的两个变量名（NRedDiagA/NRedDiagB）**只用于诊断**，不与将来的 NRedLog* 冲突；
//    · msgbuf 用 `weak` 引用：若符号不存在/未解析 → 指针为空，静默跳过（不破坏构建）；
//    · ⚠️ 实测教训：`IODTNVRAM::sync()` 返回 **void**（不是 bool），不要接收其返回值。
//
//  生命周期：本文件是**诊断用临时探针**。结论落地后（无论通或不通）都应：
//    · 把结论写进 docs/子任务/ 的执行记录与 kb/re/ 报告；
//    · 删除本文件与 X6000FB.cpp 里的调用点，或按结论收敛为正式通道代码。
// =============================================================================

#ifndef NRed_NvDiag_hpp
#define NRed_NvDiag_hpp

#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOService.h>
#include <IOKit/IONVRAM.h>
#include <libkern/libkern.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>

#include <Headers/kern_util.hpp>

namespace NvDiag {
	// 测试用变量名（含 GUID 前缀；macOS 的 NVRAM 键名格式 = "<GUID>:<名字>"）
	static const char *kKeyCustom = "4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:NRedDiagA";
	static const char *kKeyApple  = "7C436110-AB2A-4BBB-A880-FE41995C9F82:NRedDiagB";

	// 只读参照键：boot-args 必定存在（用它验证"读通道"本身是否工作）
	static const char *kKeyBootArgs = "boot-args";
	static const char *kKeyIOClass  = "IOClass";

	// 内核消息环形缓冲（XNU bsd/kern/subr_log.c）。布局已用 13.6 二进制核对：
	//   +0x00 magic(0x063061)  +0x04 size  +0x08 bufx(写指针)  +0x0c bufr  +0x10 bufc(缓冲基址)
	// 弱引用：符号未解析时为空 → 运行期判空即可，构建不受影响。
	extern "C" {
		struct NvMsgBufMeta {
			int magic;
			int size;
			int bufx;
			int bufr;
			char *bufc;
		};
		extern struct NvMsgBufMeta *msgbufp __attribute__((weak));
	}

	static constexpr int kMsgBufMagic = 0x063061;

	// 全部读数（预先求值后交给 panic 带出；禁止在 panic 实参里做函数调用）
	struct Result {
		UInt64 entry;    // /options 节点指针
		UInt64 isDtn;    // OSDynamicCast(IODTNVRAM) 是否成功
		UInt64 clsWord;  // 该节点 IOClass 字符串的前 8 字节（ASCII，如 "AppleEFI"）
		UInt64 rdLen;    // getProperty("boot-args") 的字节长度
		UInt64 rd0;      // boot-args 的前 8 字节（内容对照）
		UInt64 wrCustom; // 写自定义 GUID 变量是否成功
		UInt64 wrApple;  // 写 Apple GUID 变量是否成功
		UInt64 syncSafe; // IODTNVRAM::safeToSync()
		UInt64 syncDone; // 已调用 IODTNVRAM::sync()（该函数返回 void，只能记"是否调用"）
		UInt64 mbPtr;    // msgbufp 解析到的值（0 = 符号未解析）
		UInt64 mbMagic;  // msgbufp->magic == 0x063061
		UInt64 mbSize;   // msgbufp->size
		UInt64 mbBufx;   // msgbufp->bufx（写指针）
		UInt64 mbBufc;   // msgbufp->bufc（缓冲基址）
	};

	// allowWrite=false ⇒ 纯只读（零持久化副作用）。
	inline Result run(bool allowWrite) {
		Result r {};

		auto entry = IORegistryEntry::fromPath("/options", gIODTPlane);
		if (entry != nullptr) {
			r.entry = reinterpret_cast<UInt64>(entry);
			r.isDtn = OSDynamicCast(IODTNVRAM, entry) != nullptr ? 1 : 0;

			// 节点类名（读内存；只对已验证类型调用 getCStringNoCopy）
			if (auto o = entry->getProperty(kKeyIOClass)) {
				if (auto s = OSDynamicCast(OSString, o)) {
					if (auto c = s->getCStringNoCopy()) {
						lilu_os_memcpy(&r.clsWord, c, sizeof(r.clsWord));
					}
				}
			}

			// 读通道对照：boot-args 必然非空
			if (auto o = entry->getProperty(kKeyBootArgs)) {
				if (auto da = OSDynamicCast(OSData, o)) {
					r.rdLen = da->getLength();
					if (da->getLength() >= sizeof(r.rd0)) {
						lilu_os_memcpy(&r.rd0, da->getBytesNoCopy(), sizeof(r.rd0));
					}
				}
			}

			if (allowWrite) {
				if (auto d = OSData::withBytes("A1", 2)) {
					r.wrCustom = entry->setProperty(kKeyCustom, d) ? 1 : 0;
					d->release();
				}
				if (auto d = OSData::withBytes("B2", 2)) {
					r.wrApple = entry->setProperty(kKeyApple, d) ? 1 : 0;
					d->release();
				}
				if (auto nv = OSDynamicCast(IODTNVRAM, entry)) {
					r.syncSafe = nv->safeToSync() ? 1 : 0;
					nv->sync();      // ⚠️ 返回 void（实测编译错误来源）
					r.syncDone = 1;
				}
			}

			entry->release();
		}

		// msgbuf 可达性（弱引用；为空即符号未解析）
		if (msgbufp != nullptr) {
			r.mbPtr = reinterpret_cast<UInt64>(msgbufp);
			if (msgbufp->magic == kMsgBufMagic) {
				r.mbMagic = 1;
				r.mbSize = static_cast<UInt64>(static_cast<UInt32>(msgbufp->size));
				r.mbBufx = static_cast<UInt64>(static_cast<UInt32>(msgbufp->bufx));
				r.mbBufc = reinterpret_cast<UInt64>(msgbufp->bufc);
			}
		}

		return r;
	}
}

#endif /* NRed_NvDiag_hpp */
