// =============================================================================
//  StageMark.hpp —— 真机观测：把"流程走到哪"写进 NVRAM（即使崩溃也能取回）
//
//  为什么需要它（第八步第 1 批次的实测教训）
//    真机引导 macOS 后会在 60~75 秒内 panic，而现存两条观测通道都取不到我们自己的状态：
//      ① Lilu 日志走 `liludump=N`，它是【一次性定时落盘】——panic 早于该时刻则文件根本不生成；
//      ② panic 文本只在 `X6000FB::wrapHandleCriticalError` 里注入，而新崩溃是 page fault，
//         不经过那个函数。
//    于是这里用【第三条通道】：把阶段标记写进 NVRAM；重启回 Manjaro 后可经 efivarfs 直接读到。
//
//  用法（默认关闭，必须显式传 boot-arg；符合本项目"探针默认不生效"的纪律）
//    启用：  -NRedStageMark
//    读取（Manjaro 侧；注意跳过前 4 字节 EFI 属性头）：
//      sudo cat /sys/firmware/efi/efivars/NRedStage-4d1fda02-38c7-4a6a-9cc6-4bcca8b30102 | tail -c +5
//
//  设计要点
//    · 追加式写入（值形如 `|nred-init|disp-init-enter|seq-start|...`），保留完整路径而非只留最后一步；
//    · 只使用 NVStorage 的 OptRaw，不压缩/不加密/不加头 → Linux 侧可直接读明文；
//    · 每次写完调用 sync() 促其落到 flash；
//    · ⛔ 绝不在 panic 路径（wrapHandleCriticalError）里调用——写 NVRAM 可能加锁，会挂死。
//
//  参考实现：src/Lilu/Lilu/Sources/kern_nvram.cpp（`NVStorage` 的 init/read/write/sync）
// =============================================================================

#ifndef NRed_StageMark_hpp
#define NRed_StageMark_hpp

#include <Headers/kern_nvram.hpp>
#include <Headers/kern_util.hpp>
#include <libkern/libkern.h>

namespace StageMark {
	// ⚠️ 内核态约束（CI run #78 实测）：**函数内的静态对象需要 guard variable，kext 不支持**。
	//    故这里用"命名空间作用域的静态实例 + 标量标志位"：
	//    · 实例是文件级静态对象（内部链接，每个 TU 各一份），由 kext 的 module init 正常构造；
	//    · 标志位都是标量（常量初始化），不需要 guard。
	static NVStorage gStorage {};

	// 探针总开关：只认 boot-arg `-NRedStageMark`
	inline bool enabled() {
		static bool checked = false;
		static bool value   = false;
		if (!checked) {
			checked = true;
			value   = checkKernelArgument("-NRedStageMark");
		}
		return value;
	}

	// 惰性初始化 NVStorage（早期阶段可能失败；失败则静默降级，绝不影响引导）
	inline NVStorage *storage() {
		static bool tried = false;
		static bool ok    = false;
		if (!tried) {
			tried = true;
			ok    = gStorage.init();
		}
		return ok ? &gStorage : nullptr;
	}

	// 追加一个标记并把 NVRAM 同步到 flash
	inline void mark(const char *what) {
		if (what == nullptr || !enabled())
			return;

		auto nv = storage();
		if (nv == nullptr)
			return;

		// macOS 的 NVRAM 键名 = "<GUID>:<名字>"；Linux efivarfs 侧对应 "NRedStage-<guid 小写>"
		// （不用 static 数组：避免任何静态存储需求，47 字节的局部拷贝可忽略）
		char key[] = NVRAM_PREFIX(LILU_VENDOR_GUID, "NRedStage");

		char buf[256];
		uint32_t have = 0;
		if (auto cur = nv->read(key, have, NVStorage::OptRaw)) {
			if (have > sizeof(buf) - 64) {
				have = sizeof(buf) - 64;
			}
			lilu_os_memcpy(buf, cur, have);
			Buffer::deleter(cur);
		}

		const size_t len = strlen(what);
		if (have + len + 2 > sizeof(buf)) {
			return;   // 满了就不再追加（保留已有路径，够用）
		}

		buf[have] = '|';
		lilu_os_memcpy(buf + have + 1, what, len);
		have += static_cast<uint32_t>(len + 1);

		nv->write(key, reinterpret_cast<const uint8_t *>(buf), have, NVStorage::OptRaw);
		nv->sync();
	}

	// 带一个 64 位十六进制值的标记（形如 `name=0x0123456789abcdef`）
	inline void markHex(const char *what, uint64_t value) {
		if (what == nullptr || !enabled())
			return;

		char tmp[96];
		size_t n = strlen(what);
		if (n > 60) {
			n = 60;
		}
		lilu_os_memcpy(tmp, what, n);
		tmp[n++] = '=';
		tmp[n++] = '0';
		tmp[n++] = 'x';
		for (int shift = 60; shift >= 0; shift -= 4) {
			tmp[n++] = "0123456789abcdef"[(value >> shift) & 0xF];
		}
		tmp[n] = '\0';

		mark(tmp);
	}
}

#endif /* NRed_StageMark_hpp */
