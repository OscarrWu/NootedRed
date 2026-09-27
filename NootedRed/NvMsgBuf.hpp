// =============================================================================
//  NvMsgBuf.hpp —— 内核 console（msgbuf）快照：把内核日志尾部取出来
//
//  为什么需要它
//    "把 Apple / 内核的日志拿到手"是本项目的观测目标。两条载体都被验证过：
//      · 写 NVRAM 变量 —— ⛔ 2026-09-27 第 3 轮实测：会让调用线程长时间阻塞
//        （系统 9 分钟后才以 userspace watchdog 回落；`NRed*` 变量未落盘）⇒ **不可用**；
//      · Lilu 日志文件 —— ⛔ 早已实测本机不产出。
//    ⇒ 剩下的可靠载体是 **panic 通道**（已验证自动重启）。本文件负责"读"，panic 由调用方做。
//
//  事实基础（2026-09-27 离线取证 + 真机验证）
//    · 内核符号表里存在 `_msgbufp`；kext 侧以 `weak` 引用后**真机解析成功**（第 2 轮读数
//      `mbp≠0 / mbMagic=1 / mbSize=131072 / mbBufx=113312`）。
//    · `struct msgbuf`（XNU 13，`bsd/sys/msgbuf.h`）字段顺序：
//        +0x00 int msg_magic (0x063061)  +0x04 int msg_size
//        +0x08 int msg_bufx（写指针，指向"下一个要写"的位置）
//        +0x0c int msg_bufr（读指针）      +0x10 char *msg_bufc（缓冲基址，独立分配）
//      该布局已用 kc 里 `_msgbuf` 的静态初值核对（magic=0x63061、size=0x20000、bufc=&_smsg_bufc）。
//    · 默认缓冲大小 = `CONFIG_MSG_BSIZE` = **128 KB**（`bsd/kern/subr_log.c`）。
//      ⚠️ boot-arg `msgbuf=N` 会被 `log_setsize` 以 `size > MAX_MSG_BSIZE(1MB)` 拒绝
//      ⇒ 现有 boot-args 的 `msgbuf=4194304` 是**无效设置**，实际仍是 128 KB。
//
//  读取方式（无锁、容忍撕裂）
//    XNU 的 `log_putc_locked` 是 `bufc[bufx++] = c`（到边界回 0）⇒ "最后 n 字节"就是
//    `[bufx-n, bufx)` 这段环形区间。并发写入最多造成少数字节撕裂（文本快照可接受）；
//    绝不去拿 `bsd_log_lock`（拿锁就是挂死风险）。
//
//  风险与取舍
//    · 只读内存、不调用任何 Apple 方法 ⇒ 无锁/无分配风险；
//    · 非可打印字节一律替换为 '.'，并补 NUL ⇒ 可安全用 `%s` 交给 panic；
//    · ⚠️ panic 文本变长会改变 efivarfs 分片布局（历史上有"文本变长⇒非确定性挂死"的先例）
//      ⇒ 长度**分档递减起步**（512 → 1k → 4k），逐档验证后再加大。
// =============================================================================

#ifndef NRed_NvMsgBuf_hpp
#define NRed_NvMsgBuf_hpp

#include <libkern/libkern.h>

// 内核消息环形缓冲（XNU bsd/kern/subr_log.c）。弱引用：符号未解析时为空，运行期判空。
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

namespace NvMsgBuf {
	static constexpr int kMagic = 0x063061;
	static constexpr int kMaxDump = 8192;

	// 命名空间作用域静态缓冲（内核不支持函数内静态对象——需 guard variable）
	static char gBuf[kMaxDump + 1];

	// 把 msgbuf 的"最后 want 字节"拷进 gBuf：环形重组 + 字符净化 + NUL 结尾。
	// 返回实际拷贝字节数；0 表示通道不可用（符号未解析 / magic 不符 / 结构异常）。
	inline int dumpTail(int want) {
		auto p = msgbufp;
		if (p == nullptr || p->magic != kMagic) return 0;

		const int size = p->size;
		const int bufx = p->bufx;
		char *bufc = p->bufc;
		// 结构合理性校验（越界即放弃，绝不冒险读）
		if (size <= 0 || size > (64 << 20) || bufc == nullptr) return 0;
		if (bufx < 0 || bufx > size) return 0;

		int n = want;
		if (n > size) n = size;
		if (n > kMaxDump) n = kMaxDump;

		int idx = bufx - n;
		while (idx < 0) idx += size;

		for (int i = 0; i < n; i++) {
			const char c = bufc[idx];
			if (++idx >= size) idx = 0;
			gBuf[i] = (c == '\n' || c == '\t' || (c >= 32 && c < 127)) ? c : '.';
		}
		gBuf[n] = '\0';
		return n;
	}
}

#endif /* NRed_NvMsgBuf_hpp */
