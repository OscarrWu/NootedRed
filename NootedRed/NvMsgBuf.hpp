// =============================================================================
//  NvMsgBuf.hpp —— 内核 console（msgbuf）快照：把内核日志取出来
//
//  为什么需要它
//    "把 Apple / 内核的日志拿到手"是本项目的观测目标。三条载体的实测结论：
//      · 写 NVRAM 变量 —— ⛔ 第 3 轮实测：调用线程长时间阻塞（系统 9 分钟后才以
//        userspace watchdog 回落，`NRed*` 变量未落盘）⇒ **不可用**；
//      · Lilu 日志文件 —— ⛔ 早已实测本机不产出（`liludump` 定时落盘，崩得早即无文件）；
//      · **panic 通道 + msgbuf 快照 —— ✅ 实测可用**（本文档负责"读"，panic 由调用方做）。
//
//  事实基础
//    · 内核符号表存在 `_msgbufp`；kext 侧 `weak` 引用后**真机解析成功**（第 2 轮读数）。
//    · `struct msgbuf`（XNU 13 `bsd/sys/msgbuf.h`）字段顺序（已用 kc 静态初值核对）：
//        +0x00 int msg_magic (0x063061)  +0x04 int msg_size
//        +0x08 int msg_bufx（写指针 = 下一个待写位置）
//        +0x0c int msg_bufr（读指针）      +0x10 char *msg_bufc（缓冲基址，独立分配）
//    · 默认缓冲 = `CONFIG_MSG_BSIZE` = **128 KB**；boot-arg `msgbuf=N` 会被 `log_setsize`
//      以 `N > MAX_MSG_BSIZE(1MB)` 拒绝 ⇒ 现有 `msgbuf=4194304` 是**无效设置**（实际仍 128 KB）。
//
//  读取方式（无锁、容忍撕裂）
//    `log_putc_locked` 是 `bufc[bufx++] = c`（到边界回 0）⇒ "尾部区间"就是 `[bufx-off-len, bufx-off)`
//    这段环形区间。并发写入最多造成少数字节撕裂（文本快照可接受）；绝不去拿 `bsd_log_lock`。
//
//  实测记录（2026-09-27，`AmdRadeonController::powerUp` 入口的探针）
//    · 512 B → 7 片 / 5726 B / 自动重启 75 s；
//    · 1 KB  → 8 片 / 6242 B / 75 s；
//    · 4 KB  → 11 片 / 9314 B / 75 s（★ 首次出现"分片名非十进制"：末片是
//      `AAPL,PanicInfo000K` 而不是 `0010` ⇒ 取片必须按**实际变量名**列取，
//      不能拿 `printf "%04d"` 拼名字，否则取到空文件使解码断言失败。见 tmp/fetch-panic.sh 注释）。
//
//  风险与取舍
//    · 只读内存、不调用任何 Apple 方法 ⇒ 无锁/无分配风险；
//    · 非可打印字节替换为 '.'、补 NUL ⇒ 可安全用 `%s` 交给 panic；
//    · ⚠️ panic 文本越长，efivarfs 分片越多（历史上"文本变长⇒非确定性挂死"有先例）
//      ⇒ 档位**递减起步**（512 → 1k → 4k → 8k → …），每档确认真能自动重启再加下一档。
// =============================================================================

#ifndef NRed_NvMsgBuf_hpp
#define NRed_NvMsgBuf_hpp

#include <libkern/libkern.h>

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
}

namespace NvMsgBuf {
	static constexpr int kMagic = 0x063061;
	static constexpr int kMaxDump = 32768;       // 单次最多 32 KB

	// 命名空间作用域静态缓冲（内核不支持函数内静态对象——需 guard variable）
	static char gBuf[kMaxDump + 1];

	// 用 boot-arg 覆盖"长度 / 偏移"（两者是同一引导内的独立变量，便于分段覆盖历史日志）：
	//   nredmsg_len=<字节>   取多长（会被 kMaxDump 截断）
	//   nredmsg_off=<字节>   从"距写指针多少字节"处开始往前取（0 = 紧贴尾部）
	// 注意：boot-arg 名**不带**前导 '-'，形如 `nredmsg_off=16384`。
	inline void applyBootArgs(int &len, int &off) {
		extern "C" int PE_parse_boot_argn(const char *arg_string, void *arg_ptr, unsigned int max_arg_size);
		int v = 0;
		if (PE_parse_boot_argn("nredmsg_len", &v, sizeof(v)) && v > 0) len = v;
		v = 0;
		if (PE_parse_boot_argn("nredmsg_off", &v, sizeof(v)) && v >= 0) off = v;
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
}

#endif /* NRed_NvMsgBuf_hpp */
