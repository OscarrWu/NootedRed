// =============================================================================
//  NvDiag.hpp —— 【诊断用探针】NVRAM 写入能力诊断
//
//  为什么需要它（2026-09-27 立案）
//    目标：建立"NVRAM 日志通道"（把诊断信息写进 NVRAM 自定义变量，重启回 Manjaro 后经
//    efivarfs 直读），以解除 panic 通道的四重限制（容量小/格式串冻结/只留最后一次/观测即打断）。
//    方案与风险清单：docs/NVRAM观测通道方案与风险评估.md
//
//    但既有实测（2026-09-26 第 2 批次）显示：`NVStorage::init` 成功、`write` 与 `sync` 均失败
//    （自检位 nvram init=1 write=0 sync=0）。离线分析（kb/re/AppleEFINVRAM写入判据报告.md）已排除
//    "CSR bit6" 假设，但**无法解释 write=0**：按反汇编路径，内核调用者应通过权限检查。
//    ⇒ 剩余未知只能真机回答：`/options` 节点的**类名**、读通道是否工作、写两种 GUID 的差异、
//    `safeToSync`/`sync` 的返回值。
//
//  用法（默认关闭，必须显式传 boot-arg；符合"探针默认不生效"的纪律）
//    · `-NRedNvDiagRead`  ：**纯只读**诊断（fromPath / cast / getProperty），零持久化副作用
//    · `-NRedNvDiagWrite` ：在只读基础上加 2 条写测试（自定义 GUID + Apple GUID）+ sync
//    ⛔ 两者**不要同时开**（更早的 panic 会先触发）；先跑 Read 以最小风险取回节点信息。
//
//  设计要点
//    · 只做"查节点 / 读属性 / （可选）写两个测试变量并立刻读回"这几件事，不做任何其它副作用；
//    · 所有返回值预先求值为局部标量，供调用方用 panic 带出（panic 实参不得含函数调用）；
//    · 写入的两个变量名（NRedDiagA/NRedDiagB）**只用于诊断**，不与将来的 NRedLog* 冲突；
//    · Apple GUID 的对照项用于区分"是 GUID 相关还是整体写不通"。
//
//  生命周期：本文件是**诊断用临时探针**。结论落地后（无论通或不通）都应：
//    · 把结论写进 docs/子任务/ 的执行记录与 kb/re/ 报告；
//    · 删除本文件与 X6000FB.cpp 里的调用点，或按结论收敛为正式通道代码。
// =============================================================================

#ifndef NRed_NvDiag_hpp
#define NRed_NvDiag_hpp

#include <IOKit/IORegistryEntry.h>
#include <IOKit/IONVRAM.h>
#include <libkern/libkern.h>

#include <Headers/kern_util.hpp>

namespace NvDiag {
	// 测试用变量名（含 GUID 前缀；macOS 的 NVRAM 键名格式 = "<GUID>:<名字>"）
	static const char *kKeyCustom = "4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:NRedDiagA";
	static const char *kKeyApple  = "7C436110-AB2A-4BBB-A880-FE41995C9F82:NRedDiagB";

	// 只读参照键：boot-args 必定存在（用它验证"读通道"本身是否工作）
	static const char *kKeyBootArgs = "boot-args";
	static const char *kKeyIOClass  = "IOClass";

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
		UInt64 syncDone; // IODTNVRAM::sync()
	};

	// allowWrite=false ⇒ 纯只读（零持久化副作用）。
	inline Result run(bool allowWrite) {
		Result r {};

		auto entry = IORegistryEntry::fromPath("/options", gIODTPlane);
		if (entry == nullptr)
			return r;

		r.entry = reinterpret_cast<UInt64>(entry);
		r.isDtn = OSDynamicCast(IODTNVRAM, entry) != nullptr ? 1 : 0;

		// 节点类名（读内存，只对已验证类型调用 getCStringNoCopy）
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
				r.syncDone = nv->sync() ? 1 : 0;
			}
		}

		entry->release();
		return r;
	}
}

#endif /* NRed_NvDiag_hpp */
