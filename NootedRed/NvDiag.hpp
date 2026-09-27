// =============================================================================
//  NvDiag.hpp —— 【诊断用探针】NVRAM 读写能力 + msgbuf 可达性 诊断
//
//  为什么需要它（2026-09-27 立案）
//    目标：建立"NVRAM 日志通道"（把诊断信息写进 NVRAM 自定义变量，重启回 Manjaro 后经
//    efivarfs 直读），以解除 panic 通道的四重限制（容量小/格式串冻结/只留最后一次/观测即打断）。
//    方案与风险清单：docs/NVRAM观测通道方案与风险评估.md
//
//  实测进展（真机）
//    第 1 轮（2026-09-27 07:56）：探针挂在 `AmdDalHelper::powerUp` 上**未被触发**——
//      实测调用顺序为 `AmdPowerPlayHelper::powerUp`(+0x17e) 先于 `AmdDalHelper::powerUp`(+0x2a4)，
//      未抑制 PP 的引导在 PP 处就 panic。⇒ 探针改挂 `AmdRadeonController::powerUp` **入口**。
//    第 2 轮（2026-09-27 08:02，`-NRedNvDiagRead`）读数：
//      entry≠0 / dtn=1（/options 确为 IODTNVRAM 实例）/ cls=0 / rdlen=0
//      mbp≠0 / mbMagic=1 / mbSize=131072 / mbBufx=113312 / mbBufc≠0
//      ⇒ ★ **msgbufp 弱引用解析成功**（内核符号可被 kext 链接解析）⇒ 内核 console 可读；
//      ⇒ 但 `getProperty("boot-args")` 与 `getProperty("IOClass")` **都取不到**，
//        怀疑 AppleEFINVRAM 的 getProperty 需要 **"<GUID>:<名字>" 完整形式**（与 setProperty 一致）。
//
//  本轮（v2）要回答的问题
//    · 带 GUID 前缀读（"7C436110-…:boot-args"）能否取到？—— 若可以，则解释"裸 key 读不到"
//    · `getProperty` 返回的是 null 还是"类型不符"？—— 故新增返回对象指针读数（*Obj）
//    · 写自定义 GUID / Apple GUID 变量是否成功？（`-NRedNvDiagWrite` 才做）
//    · 写后立即读回是否成功？
//
//  用法（默认关闭，必须显式传 boot-arg；符合"探针默认不生效"的纪律）
//    · `-NRedNvDiagRead`  ：只读诊断
//    · `-NRedNvDiagWrite` ：在只读基础上加 2 条写测试 + 写后读回
//    ⛔ 两者不要同时开。
//
//  设计要点
//    · 所有读数预先求值为局部标量，供调用方用 panic 带出（panic 实参不得含函数调用）；
//    · `getProperty` 返回的对象**不是** retain 的 ⇒ **不要 release**；`fromPath` 的返回值才要 release；
//    · msgbuf 用 `weak` 引用：符号未解析时为空 → 运行期判空（第 2 轮实测已解析成功）；
//    · ⚠️ 实测教训：`IODTNVRAM::sync()` 返回 **void**（不是 bool）。
//
//  生命周期：诊断用临时探针；结论落地后删除或按结论收敛为正式通道代码。
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

	// 只读参照键：同一变量分别用"裸名字"与"带 GUID 全名"读，对比行为差异
	static const char *kKeyBootArgs    = "boot-args";
	static const char *kKeyBootArgsGuid = "7C436110-AB2A-4BBB-A880-FE41995C9F82:boot-args";
	static const char *kKeyIOClass     = "IOClass";

	// 内核消息环形缓冲（XNU bsd/kern/subr_log.c）。布局已用 13.6 二进制核对：
	//   +0x00 magic(0x063061)  +0x04 size  +0x08 bufx(写指针)  +0x0c bufr  +0x10 bufc(缓冲基址)
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

	// 全部读数（预先求值后交给 panic 带出）
	struct Result {
		UInt64 entry;    // /options 节点指针
		UInt64 isDtn;    // OSDynamicCast(IODTNVRAM) 是否成功
		UInt64 clsObj;   // getProperty("IOClass") 返回的对象指针（区分 null / 类型不符）
		UInt64 clsWord;  // 该对象若为 OSString：其 C 字符串前 8 字节
		UInt64 rd0Obj;   // getProperty("boot-args") 返回对象指针
		UInt64 rd0Len;   // 若为 OSData：长度
		UInt64 rdgObj;   // getProperty("<AppleGUID>:boot-args") 返回对象指针
		UInt64 rdgLen;   // 若为 OSData：长度
		UInt64 wrCustom; // 写自定义 GUID 变量是否成功（仅 Write 模式）
		UInt64 wrApple;  // 写 Apple GUID 变量是否成功（仅 Write 模式）
		UInt64 backObj;  // 写后读回：返回对象指针（仅 Write 模式）
		UInt64 backLen;  // 写后读回：长度
		UInt64 syncSafe; // IODTNVRAM::safeToSync()
		UInt64 mbPtr;    // msgbufp 解析到的值（0 = 符号未解析）
		UInt64 mbMagic;  // msgbufp->magic == 0x063061
		UInt64 mbSize;   // msgbufp->size
		UInt64 mbBufx;   // msgbufp->bufx
		UInt64 mbBufc;   // msgbufp->bufc
	};

	// 若 o 是 OSData，返回其长度，否则 0
	inline UInt64 dataLen(OSObject *o) {
		if (o == nullptr) return 0;
		auto da = OSDynamicCast(OSData, o);
		return da != nullptr ? static_cast<UInt64>(da->getLength()) : 0;
	}

	// allowWrite=false ⇒ 纯只读（零持久化副作用）。
	inline Result run(bool allowWrite) {
		Result r {};

		auto entry = IORegistryEntry::fromPath("/options", gIODTPlane);
		if (entry != nullptr) {
			r.entry = reinterpret_cast<UInt64>(entry);
			r.isDtn = OSDynamicCast(IODTNVRAM, entry) != nullptr ? 1 : 0;

			// ① 节点常规属性：IOClass（判断属性字典是否为空）
			if (auto o = entry->getProperty(kKeyIOClass)) {
				r.clsObj = reinterpret_cast<UInt64>(o);
				if (auto s = OSDynamicCast(OSString, o)) {
					if (auto c = s->getCStringNoCopy()) {
						lilu_os_memcpy(&r.clsWord, c, sizeof(r.clsWord));
					}
				}
			}

			// ② 裸名字读（Lilu NVStorage 的用法）
			if (auto o = entry->getProperty(kKeyBootArgs)) {
				r.rd0Obj = reinterpret_cast<UInt64>(o);
				r.rd0Len = dataLen(o);
			}

			// ③ 带 GUID 全名读（对照：验证是否必须带前缀）
			if (auto o = entry->getProperty(kKeyBootArgsGuid)) {
				r.rdgObj = reinterpret_cast<UInt64>(o);
				r.rdgLen = dataLen(o);
			}

			if (allowWrite) {
				// ④ 写测试（两个 GUID 各一条）
				if (auto d = OSData::withBytes("A1", 2)) {
					r.wrCustom = entry->setProperty(kKeyCustom, d) ? 1 : 0;
					d->release();
				}
				if (auto d = OSData::withBytes("B2", 2)) {
					r.wrApple = entry->setProperty(kKeyApple, d) ? 1 : 0;
					d->release();
				}
				// ⑤ 写后立即读回（同 Key）
				if (auto o = entry->getProperty(kKeyCustom)) {
					r.backObj = reinterpret_cast<UInt64>(o);
					r.backLen = dataLen(o);
				}
				if (auto nv = OSDynamicCast(IODTNVRAM, entry)) {
					r.syncSafe = nv->safeToSync() ? 1 : 0;
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
