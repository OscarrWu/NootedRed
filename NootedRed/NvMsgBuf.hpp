// =============================================================================
//  NvMsgBuf.hpp —— 内核 console（msgbuf）快照与**持续落盘**：把日志取出来
//
//  为什么需要它
//    "把内核 console（我们的驱动日志 + Apple 驱动日志 + 内核消息）拿到手"是本项目的观测目标。
//    三条载体的实测结论：
//      · 写 NVRAM 变量 —— ⛔ 第 3 轮实测：调用线程长时间阻塞（系统 9 分钟后才以
//        userspace watchdog 回落，`NRed*` 变量未落盘）⇒ **不可用**；
//      · **Lilu 日志文件（L1）—— ✅ 已打通**（`-liludbgall … liludump=30`），但它是
//        Lilu **自己的缓冲**、上限固定 **125 834 B**（写满即覆盖早期），且**只含"经 Lilu 打印的"**
//        ⇒ **看不到 Apple 驱动自己 `IOLog` 的内容**，日志量也不够开发使用；
//      · **本文档（L2）—— 读内核 console 并持续落盘**：不受 L1 的 125 834 B 限制、可看到
//        Apple 侧的 `IOLog`、且**不打断流程**（只读内存 + 写文件，绝不 panic）。
//
//  事实基础
//    · 内核符号表存在 `_msgbufp`；kext 侧 `weak` 引用后**真机解析成功**（第 2 轮读数）。
//    · `struct msgbuf`（XNU 13 `bsd/sys/msgbuf.h`）字段顺序（已用 kc 静态初值核对）：
//        +0x00 int msg_magic (0x063061)  +0x04 int msg_size
//        +0x08 int msg_bufx（写指针 = 下一个待写位置，0..size 环形）
//        +0x0c int msg_bufr（读指针）      +0x10 char *msg_bufc（缓冲基址，独立分配）
//    · 默认缓冲 = `CONFIG_MSG_BSIZE` = **128 KB**；boot-arg `msgbuf=N` 会被 `log_setsize`
//      以 `N > MAX_MSG_BSIZE(1MB)` 拒绝 ⇒ 现有 `msgbuf=4194304` 是**无效设置**（实际仍 128 KB）。
//
//  ⛔ L2 零产出的根因与更正（2026-09-27，离线定位）
//    旧实现在 `X6000FB::wrapControllerPowerUp`（**≈48.7 s**，即 WindowServer 打开 framebuffer 时）
//    里调用 `scheduleDumps()`，却安排 **20 / 40 / 60 秒之后**才写 ⇒ 三个落盘点分别落在
//    **68.7 / 88.7 / 108.7 s**，**全部晚于 panic（48.7 s）** ⇒ 一个文件也不会产出。
//    ⇒ 更正（也正是所有者定的设计意图——"**不靠猜时间，靠等条件 + 重试**"）：
//      ① 调度点提前到**最早**（`NRed::init`，kext 加载时）；
//      ② **每 1 秒重试**一次、最多 60 次（覆盖到 60 s > panic 的 48.7 s）；
//      ③ **只写增量**（记住上次成功写出的 `bufx`）⇒ 无重复、随时间线性增长；
//      ④ **写失败 ⇒ 不推进指针** ⇒ 下一拍重试**同一段**（这才是"等条件"，不猜挂载时刻）。
//
//  读取方式（无锁、容忍撕裂）
//    `log_putc_locked` 是 `bufc[bufx++] = c`（到边界回 0）⇒ 可直接按 `bufx` 增量取。
//    并发写入最多造成少数字节撕裂（文本快照可接受）；绝不去拿 `bsd_log_lock`。
//
//  实测记录（2026-09-27，panic 侧的 msgbuf 快照探针）
//    · 512 B → 7 片 / 5726 B / 自动重启 75 s；
//    · 1 KB  → 8 片 / 6242 B / 75 s；
//    · 4 KB  → 11 片 / 9314 B / 75 s（★ 首次出现"分片名非十进制"：末片是
//      `AAPL,PanicInfo000K` 而不是 `0010` ⇒ 取片必须按**实际变量名**列取，
//      不能拿 `printf "%04d"` 拼名字，否则取到空文件使解码断言失败。见 kb/tools/fetch-panic.sh）。
//
//  风险与取舍
//    · 只读内存、不调用任何 Apple 方法 ⇒ 无锁/无分配风险；
//    · 写文件走 `FileIO::writeBufferToFile`（Lilu 导出，内部 `vnode_open`+写），**每拍只写增量**，
//      空闲拍（无新增）**不写文件** ⇒ 开销与日志产出成正比；
//    · 总量与文件数都有上限 ⇒ 不会写爆卷；文件用完由所有者/分析机侧清理。
// =============================================================================

#ifndef NRed_NvMsgBuf_hpp
#define NRed_NvMsgBuf_hpp

#include <libkern/libkern.h>
#include <kern/thread_call.h>      // thread_call_allocate / thread_call_enter_delayed / free

#include <Headers/kern_util.hpp>   // SYSLOG（内部走 IOLog ⇒ 同时进内核 console 与 Lilu 日志）
#include <Headers/kern_file.hpp>   // FileIO::writeBufferToFile（内核态写文件）

// 内核消息环形缓冲（XNU bsd/kern/subr_log.c）。弱引用：未解析则为空，运行期判空。
extern "C" {
	struct NvMsgBufMeta {
		int magic;
		int size;
		int bufx;
		int bufr;
		char *bufc;
	};
	extern struct NvMsgBufMeta *msgbufp __attribute__((weak));

	// ★ 根 vnode（XNU `bsd/sys/vnode.h`: `extern vnode_t rootvnode`）。
	//   **落盘前的必备判据**：非空 ⇒ 根文件系统已挂载。
	//   ⛔ 为什么必须有它（2026-09-27 实测，代价：一轮无效引导）：
	//      `FileIO::writeBufferToFile` 在根 FS **尚未挂载**时被调用会让内核线程**阻塞**，
	//      系统随后被 userspace watchdog 强制重启（**无 panic ⇒ 无 panic 分片**，表现为
	//      "kext 像是让系统起不来"，实测耗时 3.6 分钟恰为 watchdog 超时）。
	//      Lilu 之所以没这个问题，是因为它的 `liludump=N` 由用户给定较大的 N（如 30 秒），
	//      那时根 FS 已挂载；而我们要在**最早**时刻起就周期尝试，故必须自带这个判据。
	extern void *rootvnode __attribute__((weak));
}

// A-28：跨 TU 前向声明（定义于 HWLibs.cpp）。周期拍 `dumpTick` 在 stBusy 保护区内调用它，
//  把 A-22 内存环（`gTraceRing`）里的早期 trace 行刷盘（与 L2 `-NRedObserveDisk` 同构）。
//  @param secTag 秒级时间戳（写入文件名与首行标签 ⇒ 判读"最后一拍"可读到秒）
int nredTraceFlush(int secTag);
int nredTraceFlush();   // 无参兼容封装（既有 dumpNow 调用点）

namespace NvMsgBuf {
	static constexpr int kMagic   = 0x063061;
	static constexpr int kMaxDump = 32768;       // 单次最多 32 KB

	// ⚠️ 命名空间作用域静态数组：**只在 X6000FB.cpp 一个翻译单元里使用**。
	//    若将来有第二个 .cpp 需要它，必须改为 `inline` 访问器（否则每个 TU 各持一份副本）。
	static char gBuf[kMaxDump + 1];

	// ── 注：长度/偏移的取值放在**调用方**（X6000FB.cpp），且用 **flag 档位**表达 ——
	//     刻意不使用数值 boot-arg：`PE_parse_boot_argn` 是 Apple 的 pexpert 函数，
	//     在探针里调用它会导致 panic 流程无法完成（2026-09-27 第 9 轮实测）。

	// ── 门控状态缓存（关键设计，2026-09-27）──────────────────────────────────────
	//  ⛔ **不要在探针里调用 `checkKernelArgument`**：它内部就是 Apple 的 `PE_parse_boot_argn`
	//     （Lilu `kern_util.hpp:432`，会拿锁/耗时）。实测：只要探针里做门控判断，就会出现
	//     "panic 与快照都正常写出，但机器**不自动重启**"（第 8/9/11/12 轮；第 7 轮同样内容却成功）。
	//  ✅ **正确姿势**：在 kext 早期、正常上下文里解析一次并存入标量；探针**只读标量**。

	// 通道元信息（供 panic 头一并带出）：判断"缓冲是否被扩大"与写指针位置。
	struct ChannelInfo {
		int ok;      // magic 校验是否通过
		int size;    // msgbuf 当前容量（默认 128 KB）
		int bufx;    // 写指针当前值
	};

	inline ChannelInfo channelInfo() {
		ChannelInfo ci {};
		auto p = msgbufp;
		if (p != nullptr && p->magic == kMagic) {
			ci.ok = 1;
			ci.size = p->size;
			ci.bufx = p->bufx;
		}
		return ci;
	}

	// 把 msgbuf 中"距写指针 off 字节、长度 len 字节"的区间拷进 gBuf：
	//   环形重组 + **丢弃不完整首行** + 字符净化 + NUL 结尾。
	// 返回实际拷贝字节数；0 表示通道不可用（符号未解析 / magic 不符 / 结构异常）。
	inline int dumpTail(int len, int off) {
		auto p = msgbufp;
		if (p == nullptr || p->magic != kMagic) return 0;

		const int size = p->size;
		const int bufx = p->bufx;
		char *bufc = p->bufc;
		// 结构合理性校验（越界即放弃，绝不冒险读）
		if (size <= 0 || size > (64 << 20) || bufc == nullptr) return 0;
		if (bufx < 0 || bufx > size) return 0;
		if (off < 0) off = 0;
		if (len <= 0) return 0;
		if (len > kMaxDump) len = kMaxDump;
		if (off >= size) return 0;
		if (len + off > size) len = size - off;

		int end = bufx - off;
		while (end < 0) end += size;
		int start = end - len;
		while (start < 0) start += size;

		// 丢弃不完整的首行（最多跳过 512 字节），让输出从行首开始，便于判读
		for (int skip = 0; skip < 512 && skip < len; skip++) {
			if (bufc[(start + skip) % size] == '\n') {
				start = (start + skip + 1) % size;
				len -= (skip + 1);
				break;
			}
		}

		int idx = start;
		for (int i = 0; i < len; i++) {
			const char c = bufc[idx];
			if (++idx >= size) idx = 0;
			gBuf[i] = (c == '\n' || c == '\t' || (c >= 32 && c < 127)) ? c : '.';
		}
		gBuf[len] = '\0';
		return len;
	}

	// ═══════════════════════════════════════════════════════════════════════════
	//  L2：持续落盘（把内核 console 增量写进 APFS 卷，供 Manjaro 侧只读读回）
	//  ⛔ 不 panic、不打断流程：只"读内存 + 写文件"，失败静默并下拍重试。
	// ═══════════════════════════════════════════════════════════════════════════
	static constexpr int kTickSecs  = 1;             // 重试周期（秒）
	static constexpr int kFirstDelay = 8;            // 首拍延迟：保守一点，避开最早期
	static constexpr int kMaxTicks  = 60;            // 最多 60 拍（覆盖到 60 s > panic 的 48.7 s）
	static constexpr int kFirstTail = 32768;         // 首帧：取尾部 32 KB（含启动早期日志）
	static constexpr int kChunkMax  = 32768;         // 单帧增量上限
	static constexpr int kMaxTotal  = (4 << 20);     // 累计写出上限 4 MB（保护卷）

	// 状态一律用 **inline 函数内的静态标量**：C++ 保证全程序唯一（不会因多翻译单元各持一份，
	// 且均为 POD、无需内核不支持的 guard variable）。
	inline int           &stLastBufx()  { static int v = -1;    return v; }   // -1 = 尚未成功写出过
	inline int           &stPendingEnd(){ static int v = 0;     return v; }   // 本帧写入后的 bufx
	inline int           &stSeq()       { static int v = 0;     return v; }   // 已写文件数
	inline int           &stTick()      { static int v = 0;     return v; }
	inline int           &stTotal()     { static int v = 0;     return v; }   // 累计字节
	inline int           &stFirstOkSec(){ static int v = -1;    return v; }   // 首次写成功发生在第几秒
	inline uint64_t      &stUptimeTicks(){ static uint64_t v = 0; return v; } // 本引导 uptime 指纹（首次取到后冻结）
	inline thread_call_t &stCall()      { static thread_call_t v = nullptr; return v; }
	// 立即落盘与周期拍的互斥标志（见 `dumpNow`）：`FileIO::writeBufferToFile` 不是为并发设计的。
	inline bool          &stBusy()      { static bool v = false; return v; }
	// A-28：`nredTraceFlush` 的独立互斥标志（与 stBusy **分开**——flush 写可能变长阻塞，
	//  若与 stBusy 共用则一次 FS 阻塞会让 L2 周期拍全部跳过、唯一存活计时器失效）。
	inline bool          &stFlushBusy() { static bool v = false; return v; }

	// 计算"自上次成功写出之后新增的部分"，填充 gBuf；成功则把本帧终点记入 stPendingEnd。
	// ⚠️ **不推进 stLastBufx**（推进由 commit 完成）⇒ 写失败时下拍重算同一段（自然重试）。
	inline int dumpIncrement() {
		auto p = msgbufp;
		if (p == nullptr || p->magic != kMagic) return 0;
		const int size = p->size;
		const int bufx = p->bufx;
		char *bufc = p->bufc;
		if (size <= 0 || size > (64 << 20) || bufc == nullptr) return 0;
		if (bufx < 0 || bufx > size) return 0;

		const int last = stLastBufx();
		if (last < 0) {                        // 首帧：取尾部快照（复用已验证的 dumpTail）
			const int n = dumpTail(kFirstTail, 0);
			if (n <= 0) return 0;
			stPendingEnd() = bufx;
			return n;
		}
		if (bufx == last) return 0;            // 无新增 ⇒ 本拍不写文件

		int n = bufx - last;
		if (n < 0) n += size;                  // 环形回绕
		if (n > kChunkMax) n = kChunkMax;      // 单帧上限（剩余留下一帧，指针按增量推进）

		int idx = last;
		if (idx >= size) idx -= size;
		for (int i = 0; i < n; i++) {
			const char c = bufc[idx];
			if (++idx >= size) idx = 0;
			gBuf[i] = (c == '\n' || c == '\t' || (c >= 32 && c < 127)) ? c : '.';
		}
		gBuf[n] = '\0';
		stPendingEnd() = (last + n) % size;
		return n;
	}

	// 把刚写成功的这一帧"提交"（推进读起点的指针）
	inline void commitIncrement() {
		stLastBufx() = stPendingEnd();
	}

	// ★★ 引导 uptime 指纹（第 87 轮判读 §4 建议 2 / §5.1，2026-10-02）──────────────
	//  动机：L2 文件**不是每轮干净快照** —— 一是旧轮文件整份残留（本轮根本没写它），
	//   二是**写不截断**（判读 §5.1：`NRedObserve-015-019s.txt` 本轮 = 前 9073 B 本轮内容
	//   + 后 17146 B 与旧轮逐字节相同），二者都会让"旧轮的行"被当成"本轮的证据"。
	//  ⇒ 每次写文件时，在**正文首行**写一行本轮 uptime 指纹；判读侧据此判属主
	//   （`[uptime-fingerprint]` 前缀刻意唯一，不会被既有任何串命中）。
	//  取时源：`mach_absolute_time()`（本文件已用它排下一拍，见 `scheduleNextTick`）
	//   —— 它就是 8 字节绝对时基计数（连续、单调）；**跳过 0**，因为 XNU panic 文本里的
	//   `System uptime in nanoseconds:` 同样不可能为 0 ⇒ `0` 可安全用作"尚未取到"哨兵。
	inline uint64_t bootUptimeTicks() {
		uint64_t &t = stUptimeTicks();
		if (t == 0) t = mach_absolute_time();
		return t;
	}

	// 指纹行长度：`[uptime-fingerprint] 0x…\n`（前缀 22 B + 16 位十六进制 + 换行 + NUL）。
	static constexpr size_t kFingerprintMax = 48;

	// 把指纹行写进 `dst`，返回**写入长度（不含 NUL）**。即便调用方传入长度为 0 的正文，
	//  本函数仍会写满指纹行 ⇒ 0 字节的"占位"用法不成立（也不该成立：落盘即应可判属主）。
	inline size_t formatFingerprint(char *dst, size_t cap) {
		if (dst == nullptr || cap < 26) return 0;
		const int n = snprintf(dst, cap, "[uptime-fingerprint] 0x%llx\n",
		                       static_cast<unsigned long long>(bootUptimeTicks()));
		return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
	}

	// 把"指纹行 + 正文"写进文件（正文前**不再**另起分隔——首行恒为指纹，判读侧只认首行）。
	//  返回 0 表示成功（与 `FileIO::writeBufferToFile` 同语义）。
	//  只有 `hn + len >= sizeof(gBuf)` 时才用第二 buffer 拼接，绝不截断正文。
	inline int writeWithFingerprint(const char *name, const char *body, size_t len) {
		char head[kFingerprintMax];
		const size_t hn = formatFingerprint(head, sizeof(head));
		if (hn == 0) return -1;
		if (hn + len < sizeof(gBuf)) {
			__builtin_memcpy(gBuf, head, hn);
			if (len > 0) __builtin_memcpy(gBuf + hn, body, len);
			return FileIO::writeBufferToFile(name, gBuf, hn + len);
		}
		char merged[2 * kFingerprintMax * 16];   // 1536 B，足够覆盖单帧上限 + 指纹
		if (hn + len > sizeof(merged)) return -1;
		__builtin_memcpy(merged, head, hn);
		if (len > 0) __builtin_memcpy(merged + hn, body, len);
		return FileIO::writeBufferToFile(name, merged, hn + len);
	}

	// 排下一拍（提取成小函数，多处复用）
	inline void scheduleNextTick(int secs) {
		uint64_t abs = 0;
		nanoseconds_to_absolutetime(static_cast<uint64_t>(secs) * 1000000000ULL, &abs);
		thread_call_enter_delayed(stCall(), mach_absolute_time() + abs);
	}

	// ★ 立即落一拍（2026-09-28 第 30 轮）：供**关键探针在 panic 之前主动调用**。
	//   动机：PP 包装函数的读数发生在 ~34.5 s，而周期拍（每 1 s）的下一拍落在 panic 之后
	//   ⇒ 数据进得来 msgbuf、却没有任何一拍照到它（实测 6 轮零产出）。
	//   本函数**只做"读内存 + 写文件"**：不改状态、不 panic、不阻塞（失败静默），
	//   与周期拍共用同一套增量指针（`stLastBufx`）。
	//   ⚠️ 互斥：`FileIO::writeBufferToFile` 不是为并发设计的，而周期拍跑在独立的 thread_call
	//   线程上。两条路径共用 `stBusy` 标志互斥，避免同时写同一卷（最坏只丢一拍，不影响流程）。
	//   位置要求：必须定义在 `dumpIncrement`/`commitIncrement` **之后**（它们是同命名空间的
	//   自由函数，使用前须已声明）。
	inline void dumpNow() {
		if (rootvnode == nullptr || stBusy()) return;
		stBusy() = true;
		const int n = dumpIncrement();
		if (n > 0) {
			char name[80];
			snprintf(name, sizeof(name), "/var/log/NRedNow-%03d.txt", stSeq());
			// 首行写本轮 uptime 指纹（第 87 轮判读 §4 建议 2）：判读侧据此判 L2 文件属主。
			if (writeWithFingerprint(name, gBuf, static_cast<size_t>(n)) == 0) {
				commitIncrement();
				stSeq()++;
			}
		}
		// A-28：把内存环里的早期 trace 行刷盘（**在 stBusy 释放之前**调用 ⇒ 与周期拍互斥，
		//  不会并发写同一卷）。秒级标签取"当前拍序号×kTickSecs"。
		nredTraceFlush(stTick() * kTickSecs);
		stBusy() = false;
	}

	// 一拍：有增量就写一个文件；无论成败都排下一拍，直到用完 kMaxTicks 或写满 kMaxTotal。
	// 由 thread_call 调用 ⇒ **独立线程上下文**（与 Lilu 的 debugDumpCall 同款），不在锁里、不阻塞调用者。
	inline void dumpTick(thread_call_param_t, thread_call_param_t) {
		int &tick = stTick();
		const int sec = tick * kTickSecs;

		if (tick < kMaxTicks && stTotal() < kMaxTotal) {
			tick++;

			// ★★ 根文件系统挂载判据（**本实现的核心安全措施**）：
			//    `rootvnode == nullptr` ⇒ 根 FS 还没挂上 ⇒ **绝不碰文件系统**（否则内核线程会阻塞、
			//    被 watchdog 强制重启且**不产生 panic 分片**）。只排下一拍继续等条件。
			if (rootvnode == nullptr) {
				scheduleNextTick(kTickSecs);
				return;
			}
			// 与探针的即时落盘互斥（见 `dumpNow` 的说明）：撞上就跳过本拍，下拍再来。
			if (stBusy()) {
				scheduleNextTick(kTickSecs);
				return;
			}
			stBusy() = true;

			const int n = dumpIncrement();
			if (n > 0) {
				char name[80];
				snprintf(name, sizeof(name), "/var/log/NRedObserve-%03d-%03ds.txt", stSeq(), sec);
				// 首行写本轮 uptime 指纹（第 87 轮判读 §4 建议 2）：判读侧据此判 L2 文件属主。
				const int err = writeWithFingerprint(name, gBuf, static_cast<size_t>(n));
				if (err == 0) {
					commitIncrement();
					stSeq()++;
					stTotal() += n;
					if (stFirstOkSec() < 0) stFirstOkSec() = sec;
				} else {
					// 写失败 ⇒ **不提交**，下一拍重试同一段
					SYSLOG("NvMsgBuf", "L2 write@%ds failed (%d), will retry", sec, err);
				}
			}

			// A-28：把内存环里的早期 trace 行刷盘（在 stBusy 保护区内，与 L2 同构）。
			//  传本拍秒级标签 ⇒ 文件名与首行带 `sec=`，判读"最后一拍"可读到秒。
			nredTraceFlush(sec);

			scheduleNextTick(kTickSecs);
			stBusy() = false;
			return;
		}

		// 收尾：这条 SYSLOG 会进内核 console 与 Lilu 日志，作为"L2 本轮产出"的自证
		SYSLOG("NvMsgBuf", "L2 done: firstOk=%ds files=%d bytes=%d ticks=%d",
		    stFirstOkSec(), stSeq(), stTotal(), tick);
		if (stCall() != nullptr) {
			thread_call_free(stCall());
			stCall() = nullptr;
		}
	}

	// 在**最早的安全位置**（`NRed::init`）调用一次：启动周期落盘（幂等）。
	inline void scheduleDumps() {
		if (stCall() != nullptr) return;
		stCall() = thread_call_allocate(dumpTick, nullptr);
		if (stCall() == nullptr) return;
		scheduleNextTick(kFirstDelay);
	}
}

#endif /* NRed_NvMsgBuf_hpp */
