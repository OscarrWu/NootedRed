// PSP 13.0.4 Bringup 主流程编排
//
// 顺序以 Linux `psp_hw_start` (amdgpu_psp.c:2946-3090) 为准，
// 不得自行调整顺序。
//
// 流程概要（13.0.4 / Phoenix）：
//   1. bootloader 链：kdb/spl/sysdrv/soc_drv/intf_drv/dbg_drv/sos
//      — 7 条命令，每条带 is_sos_alive 短路（psp_v13_0_4.c:61-69 is_sos_alive；短路点 L100-104 / L165-166）
//      — 本机因固件未请求（init_microcode 只取 TOC+TA）而全部跳过
//   2. ring_create(KM) — C2PMSG_64/67/69/70/71
//   3. psp_update_fw_reservation — 13.0.4 版本不在白名单内，返回 0
//   4. psp_tmr_init → 内联调用 psp_load_toc（送 TOC，取 tmr_size）
//                   → 分配 TMR
//   5. psp_tmr_load — GFX_CMD_ID_SETUP_TMR
//   6. LOAD_IP_FW 单帧发送原语（M2 阶段暂不调用，M4 必需）
//
// 依据：
//   - amdgpu_psp.c:2946-3090  psp_hw_start（主流程编排）
//   - amdgpu_psp.c:855-878    psp_load_toc
//   - amdgpu_psp.c:881-923    psp_tmr_init
//   - amdgpu_psp.c:925-956    psp_skip_tmr / psp_tmr_load
//   - amdgpu_psp.c:3368-3384  psp_prep_load_ip_fw_cmd_buf
//   - amdgpu_psp.c:3386-3401  psp_execute_ip_fw_load
//   - psp_v13_0_4.c:61-190    bootloader / is_sos_alive；ring_create L223-291
//   - psp_v13_0_4.c:311-334   ring_get_wptr / ring_set_wptr
//
// 约束：
//   - header-only，命名空间 fw
//   - 无异常、无动态分配、无文件 IO
//   - 每一步都有超时与失败返回（绝不挂死）

#pragma once

#include "Psp13Ring.hpp"
#include "Regs/PSP13.hpp"
#include "FwUcodeHeader.hpp"
#ifndef FW_NO_NRED_TRACE
#include "../HWLibs.hpp"          // NRED_TRACE（D-3 追加：tmrInit 子步 trace）
#else
// 测试环境：NRED_TRACE 空实现（离线不落盘；kext 构建由 HWLibs.hpp 提供真宏）
#define NRED_TRACE(fmt, ...) ((void)0)
#endif
namespace fw {
// =============================================================================

enum class BringupStep : uint32_t {
    None               = 0,
    BlLoadKdb          = 1,
    BlLoadSpl          = 2,
    BlLoadSysdrv       = 3,
    BlLoadSocDrv       = 4,
    BlLoadIntfDrv      = 5,
    BlLoadDgbDrv       = 6,
    BlLoadSos          = 7,
    RingCreate         = 8,
    UpdateFwReserve    = 9,
    TmrInit            = 10,
    TmrLoad            = 11,
    Done               = 12,
};

// =============================================================================
// 带回溯的寄存器序列记录接口
// =============================================================================

/// 单条寄存器访问记录
struct RegTraceEntry {
    bool     is_write;   // true=write, false=read
    uint32_t addr;       // 寄存器绝对地址
    uint32_t value;      // 写入值或读出值
};

/// 固定容量寄存器序列记录
template <uint32_t kCapacity>
struct RegTrace {
    RegTraceEntry entries[kCapacity];
    uint32_t      count;

    RegTrace() : count(0) {}

    void add(bool is_write, uint32_t addr, uint32_t value) {
        if (count < kCapacity) {
            entries[count].is_write = is_write;
            entries[count].addr     = addr;
            entries[count].value    = value;
            ++count;
        }
    }

    const RegTraceEntry* begin() const { return entries; }
    const RegTraceEntry* end()   const { return entries + count; }
    uint32_t size() const { return count; }
};

// =============================================================================
// 可追踪的 RegSink —— 记录所有寄存器访问
// =============================================================================

template <uint32_t kCap>
struct TracingSink : public display::RegSink {
    RegTrace<kCap>* trace;

    TracingSink(RegTrace<kCap>* t) : trace(t) {}

    display::RegValue read(display::RegAddr addr) override {
        const display::RegValue val = display::RegSink::read(addr);
        if (trace) trace->add(false, addr, val);
        return val;
    }

    void write(display::RegAddr addr, display::RegValue val) override {
        if (trace) trace->add(true, addr, val);
        display::RegSink::write(addr, val);
    }

    void delayMicroseconds(uint32_t us) override {
        // 轮询等待中的延迟：由子类实现（测试中可压缩或跳过）
        doDelay(us);
    }

    // 由测试替身覆盖：实际的延迟实现
    virtual void doDelay(uint32_t) {}
};

// =============================================================================
// Bootloader 组件描述
// =============================================================================

struct BlComponent {
    const uint8_t* data;       // 固件数据（本机 NULL）
    uint32_t       size;       // 固件大小（本机 0）
    uint32_t       bl_cmd;     // PSP_BL__* 命令
    bool           is_sos;     // 是否为 SOS（特殊处理）
};

// =============================================================================
// Bringup 上下文 —— 调用方分配，每一步失败即返回
// =============================================================================

struct BringupCtx {
    display::RegSink* sink;
    RingState*        ring;
    uint8_t*          fw_pri_buf;   // 固件优先缓冲（kFwCopyMax 字节）
    uint8_t*          tmr_buf;      // TMR 缓冲（由调用方分配/管理）
    uint32_t          tmr_size;     // TMR 大小（由 load_toc 计算）
    const uint8_t*    toc_data;     // TOC 固件数据（psp_13_0_4_toc.bin）
    uint32_t          toc_size;     // TOC 大小
    uint32_t          last_resp_status{0}; // A-10 ③：resp_status 判读（tmrInit/loadToc/tmrLoad 回填）
    BringupStep       last_step;    // 最后成功完成的步骤
    int               last_error;   // 最后错误码
    BlComponent       bl_comps[7];  // bootloader 组件表
};

// =============================================================================
// 内联实现
// =============================================================================

// ---------- 辅助 ----------

/// 等 Linux is_psp_fw_valid (amdgpu_psp.c:4705-4708)
inline bool isFwValid(const BlComponent& comp) {
    return comp.data != nullptr && comp.size > 0;
}

// 等 Linux psp_copy_fw (amdgpu_psp.c:4651-4669)
// ⚠️ D6（T6）：Linux 先 memset(fw_pri_buf, 0, PSP_1_MEG)（amdgpu_psp.c:4664），本实现只拷 size 字节。
//    PSP 只引用 size 字节；调用方需保证缓冲不残留敏感数据。
inline int copyFw(uint8_t* dst, const uint8_t* src, uint32_t size) {
    if (!src || size == 0 || size > kFwCopyMax) return -1;
    for (uint32_t i = 0; i < size; ++i) dst[i] = src[i];
    return 0;
}

// ---------- 1. Bootloader loader ----------

// 等 Linux psp_v13_0_4_is_sos_alive (psp_v13_0_4.c:61-69)
/// 读 C2PMSG_81，非零则 SOS 已存活
inline bool isSosAlive(display::RegSink& sink) {
    return sink.read(kC2PMSG81) != 0;
}

// 等 Linux psp_v13_0_4_wait_for_bootloader (psp_v13_0_4.c:71-90)
/// 轮询 C2PMSG_35 bit31（就绪标志），最多 kBlRetryMax 次
inline int waitForBootloader(display::RegSink& sink) {
    for (uint32_t r = 0; r < kBlRetryMax; ++r) {
        const int ret = waitReg(sink, kC2PMSG35,
                                kBlReadyFlag, kBlReadyFlag, false);
        if (ret == 0) return 0;
        sink.delayMicroseconds(100); // 重试间隔
    }
    return -1;
}

/// bootloader 加载一条组件（非 SOS）
///
// 等 Linux psp_v13_0_4_bootloader_load_component (psp_v13_0_4.c:92-124)
/// 步骤：
///   1. isSosAlive 短路（若 SOS 已存活则跳过全部 bootloader）
///   2. waitForBootloader
///   3. copyFw 到 fw_pri_buf
///   4. WREG32 C2PMSG_36 = fw_pri_mc_addr >> 20
///   5. WREG32 C2PMSG_35 = bl_cmd
///   6. waitForBootloader
inline int bootloaderLoadComponent(display::RegSink& sink,
                                   uint8_t* fw_pri_buf,
                                   const BlComponent& comp,
                                   uint64_t fw_pri_mc_addr) {
    if (isSosAlive(sink)) return 0;

    int ret = waitForBootloader(sink);
    if (ret != 0) return ret;

    ret = copyFw(fw_pri_buf, comp.data, comp.size);
    if (ret != 0) return ret;

    // 提供固件地址（>>20，等 Linux psp_v13_0_4.c:115-119）
    sink.write(kC2PMSG36, static_cast<uint32_t>(fw_pri_mc_addr >> 20));
    sink.write(kC2PMSG35, comp.bl_cmd);

    return waitForBootloader(sink);
}

// 加载 SOS（特殊处理，psp_v13_0_4.c:156-190）
///
/// 区别：拷贝后 `mdelay(20)` + wait C2PMSG_81 值变化
inline int bootloaderLoadSos(display::RegSink& sink,
                             uint8_t* fw_pri_buf,
                             const BlComponent& comp,
                             uint64_t fw_pri_mc_addr) {
    if (isSosAlive(sink)) return 0;

    int ret = waitForBootloader(sink);
    if (ret != 0) return ret;

    ret = copyFw(fw_pri_buf, comp.data, comp.size);
    if (ret != 0) return ret;

    sink.write(kC2PMSG36, static_cast<uint32_t>(fw_pri_mc_addr >> 20));
    sink.write(kC2PMSG35, comp.bl_cmd);

    // PS 有延迟（mdelay(20) in Linux psp_v13_0_4.c:184）
    sink.delayMicroseconds(20000);

    // 等 C2PMSG_81 值变化（等 Linux psp_v13_0_4.c:185-187）
    // psp_wait_for(psp, C2PMSG_81, RREG32(C2PMSG_81), 0, PSP_WAITREG_CHANGED)
    {
        const uint32_t current = sink.read(kC2PMSG81);
        ret = waitReg(sink, kC2PMSG81, current, 0, true);
    }
    return ret;
}

// ---------- 2. Ring create (non-SRIOV) ----------

// 等 Linux psp_v13_0_4_ring_create (psp_v13_0_4.c:223-291) 非 SRIOV 分支
///
/// 步骤：
    // 等待 trust OS 就绪（等 L259-261）
///   2. 写 C2PMSG_69 = ring_addr_lo
///   3. 写 C2PMSG_70 = ring_addr_hi
///   4. 写 C2PMSG_71 = ring_size
///   5. 写 C2PMSG_64 = PSP_RING_TYPE__KM << 16
///   6. mdelay(20)
///   7. 等待 C2PMSG_64 响应（MBOX_TOS_RESP_FLAG）
inline int ringCreate(display::RegSink& sink,
                      uint64_t ring_mc_addr,
                      uint32_t ring_size,
                      uint32_t ring_type) {
    // 等待 trust OS 就绪（等 L107-114）
    int ret = waitReg(sink, kC2PMSG64,
                      kMboxTosRespFlag, kMboxTosRespMask, false);
    if (ret != 0) return ret;

    // 写环参数
    sink.write(kC2PMSG69, static_cast<uint32_t>(ring_mc_addr & 0xFFFFFFFF));
    sink.write(kC2PMSG70, static_cast<uint32_t>((ring_mc_addr >> 32) & 0xFFFFFFFF));
    sink.write(kC2PMSG71, ring_size);

    // 写初始化命令：ring_type << 16（等 L277-279）
    sink.write(kC2PMSG64, ring_type << 16);

    // 延迟（等 mdelay(20) in L282）
    sink.delayMicroseconds(20000);

    // 等待响应标志（等 L285-287）
    ret = waitReg(sink, kC2PMSG64,
                  kMboxTosRespFlag, kMboxTosRespMask, false);
    return ret;
}

// ---------- 3. Update fw reservation ----------

/// 等 Linux psp_update_fw_reservation (amdgpu_psp.c:1059-1121)
/// 13.0.4 不在版本白名单内（只有 14.0.2/14.0.3），恒返回 0
inline int updateFwReservation(uint32_t mp0_ip_version) {
    (void)mp0_ip_version;
    return 0; // 13.0.4 不在此路径中
}

// ---------- 4. TOC loading (inside tmr_init) ----------

/// 等 Linux psp_prep_load_toc_cmd_buf (amdgpu_psp.c:845-852)
inline void prepLoadTocCmd(GfxCmdResp* cmd,
                           uint64_t pri_buf_mc, uint32_t size) {
    cmd->buf_size         = kCmdBufSize;
    cmd->buf_version      = 1;
    cmd->cmd_id           = GFX_CMD_ID_LOAD_TOC;
    cmd->cmd_payload[0]   = static_cast<uint32_t>(pri_buf_mc & 0xFFFFFFFF);
    cmd->cmd_payload[1]   = static_cast<uint32_t>((pri_buf_mc >> 32) & 0xFFFFFFFF);
    cmd->cmd_payload[2]   = size;
}

/// 等 Linux psp_init_toc_microcode (amdgpu_psp.c:3986-4008)
///
/// Linux 先解析 TOC 的 common_firmware_header，只把 payload 切片
/// （`data + ucode_array_offset_bytes` 起、`ucode_size_bytes` 长）存入
/// `psp->toc.start_addr/.size_bytes`；之后 `psp_load_toc` 送的就是这个切片。
/// 本实现同样只送 payload —— 与整文件相比，fw_pri 偏移 0 处是 payload 起始
/// 而非头字节，LOAD_TOC 帧的 `toc_size` 是 payload 长而非文件长。
///
/// @return 0 成功；-1 头无效或切片越界（离线防御：Linux 靠固件配套保证）
inline int tocPayloadSlice(const uint8_t* toc_data, uint32_t toc_size,
                           const uint8_t** out_payload, uint32_t* out_size) {
    CommonFwHeader hdr;
    if (!parseCommonFwHeader(toc_data, toc_size, &hdr)) return -1;
    if (hdr.ucode_array_offset_bytes > toc_size ||
        hdr.ucode_size_bytes > toc_size - hdr.ucode_array_offset_bytes) {
        return -1;
    }
    *out_payload = toc_data + hdr.ucode_array_offset_bytes;
    *out_size    = hdr.ucode_size_bytes;
    return 0;
}

/// 等 Linux psp_load_toc (amdgpu_psp.c:855-878)
///
/// 步骤：
///   1. 解析 common header，取 payload 切片（tocPayloadSlice）
///   2. copyFw(payload) → fw_pri_buf（Linux 拷的是 `psp->toc.start_addr`）
///   3. 构造 LOAD_TOC 帧（`toc_size` = payload 长）
///   4. 帧拷贝到 cmd_buf
///   5. submit 到环
///   6. wait fence
///   7. 从 resp.tmr_size 读出 tmr_size
inline int loadToc(display::RegSink& sink,
                   RingState* ring,
                   uint8_t* fw_pri_buf,
                   const uint8_t* toc_data,
                   uint32_t toc_size,
                   uint64_t fw_pri_mc_addr,
                   uint64_t cmd_buf_mc_addr,
                   uint64_t fence_mc_addr,
                   uint32_t* out_tmr_size,
                   uint32_t* out_resp_status = nullptr) {
    // 切片：只送 payload（等 Linux psp_init_toc_microcode, amdgpu_psp.c:3986-4008）
    const uint8_t* payload = nullptr;
    uint32_t       payload_size = 0;
    int ret = tocPayloadSlice(toc_data, toc_size, &payload, &payload_size);
    if (ret != 0) return ret;

    // copy payload（Linux 拷贝源 = psp->toc.start_addr，即切片起点）
    ret = copyFw(fw_pri_buf, payload, payload_size);
    if (ret != 0) return ret;

    // 准备命令帧（toc_size = payload 长）
    GfxCmdResp cmd;
    for (uint32_t i = 0; i < kCmdBufSize / sizeof(uint32_t); ++i) {
        reinterpret_cast<uint32_t*>(&cmd)[i] = 0;
    }
    prepLoadTocCmd(&cmd, fw_pri_mc_addr, payload_size);

    // 拷贝命令到环的命令缓冲
    cmdBufCopy(ring, &cmd);

    // submit 帧（等 psp_cmd_submit_buf 前半: amdgpu_psp.c:738）
    // ⚠️ 这里必须是**命令缓冲**的 MC 地址，不是固件缓冲地址：Linux `psp_cmd_submit_buf`
    //   （amdgpu_psp.c:738）对**所有**命令一律传 `psp->cmd_buf_mc_addr`——命令结构 `psp_gfx_cmd_resp`
    //   已由上面的 `cmdBufCopy`（= Linux 的 `memcpy(psp->cmd_buf_mem, cmd, ...)`，amdgpu_psp.c:735）
    //   拷进命令缓冲；`fw_pri_mc_addr` 只出现在 LOAD_TOC 命令**负载**里的 `toc_phy_addr_lo/hi`
    //   （prepLoadTocCmd，等 amdgpu_psp.c:845-852），不能用作环帧的 `cmd_buf_addr`。
    //   （2026-09-29 由独立复核发现并修正：原实现误传 `fw_pri_mc_addr`，真机上 PSP 会从 TOC 固件字节处
    //    解析命令结构 ⇒ load_toc 必失败或 fence 超时；离线 MockSink 不校验帧内容，故未暴露。）
    ret = ringSubmitFrame(ring, sink, cmd_buf_mc_addr, fence_mc_addr);
    if (ret != 0) return ret;

    // 等待 fence
    ret = ringWaitForFence(ring, sink, ring->fence_value);
    if (ret != 0) return ret;

    // 读响应中的 tmr_size 与 resp_status
    const GfxCmdResp* resp = cmdBufGet(ring);
    *out_tmr_size = resp->resp_tmr_size;
    if (out_resp_status) { *out_resp_status = resp->resp_status; }
    return 0;
}

// ---------- 5. TMR init ----------

/// 等 Linux psp_skip_tmr (amdgpu_psp.c:925-933)
/// 13.0.4: boot_time_tmr = false ⇒ !boot_time_tmr = true ⇒ skip_tmr 返回 false ⇒ 不跳过
inline bool skipTmr(bool boot_time_tmr, bool autoload_supported) {
    return (!boot_time_tmr || !autoload_supported) ? false : true;
}

/// 等 Linux psp_tmr_init (amdgpu_psp.c:881-923)
///
/// 步骤：
///   1. 默认 tmr_size = PSP_TMR_SIZE(~4MB)
///   2. 若非 SRIOV 且 toc 有效：loadToc() → 更新 tmr_size
///   3. 分配 TMR（本实现中由调用方提供缓冲区）
inline int tmrInit(display::RegSink& sink,
                   RingState* ring,
                   uint8_t* fw_pri_buf,
                   const uint8_t* toc_data,
                   uint32_t toc_size,
                   uint64_t fw_pri_mc_addr,
                   uint64_t cmd_buf_mc_addr,
                   uint64_t fence_mc_addr,
                   uint32_t* tmr_size,
                   uint32_t* out_resp_status = nullptr) {
    // 默认 size (~4MB 或 ~8MB for Aldebaran)
    *tmr_size = 0x400000; // PSP_TMR_SIZE for non-Aldebaran (amdgpu_psp.h:40)

    // D-3 追加：tmrInit 子步 trace（D-3 追加要求：loadToc/fence/PSP 提交各输出 trace）
    NRED_TRACE("tmrInit: start toc_valid=%u toc_size=%u", toc_data != nullptr ? 1u : 0u, toc_size);

    // 如果 toc 有效，通过 load_toc 获取实际所需 size
    if (toc_data && toc_size > 0 && fw_pri_buf) {
        NRED_TRACE("tmrInit: loadToc start toc_size=%u", toc_size);
        int ret = loadToc(sink, ring, fw_pri_buf,
                          toc_data, toc_size,
                          fw_pri_mc_addr, cmd_buf_mc_addr, fence_mc_addr,
                          tmr_size, out_resp_status);
        if (ret != 0) {
            NRED_TRACE("tmrInit: loadToc failed ret=%d", ret);
            return ret;
        }
        NRED_TRACE("tmrInit: loadToc ok tmr_size=0x%X", *tmr_size);
    } else {
        NRED_TRACE("tmrInit: skip loadToc (toc invalid or no fw_pri_buf)");
    }

    // 分配 TMR 由调用方完成（等 amdgpu_psp.c:911-917）
    // 调用方应确保 tmr_buf != nullptr 或跳过

    NRED_TRACE("tmrInit: done tmr_size=0x%X", *tmr_size);
    return 0;
}


// ---------- 5b. TMR prep + submit ----------

/// 等 Linux psp_prep_tmr_cmd_buf (amdgpu_psp.c:820-843) 非 SRIOV 分支
inline void prepTmrCmd(GfxCmdResp* cmd,
                       uint64_t tmr_mc_addr,
                       uint32_t tmr_size) {
    cmd->buf_size        = kCmdBufSize;
    cmd->buf_version     = 1;
    cmd->cmd_id          = GFX_CMD_ID_SETUP_TMR;
    // cmd_payload packed: buf_phy_addr_lo/hi, buf_size, flags, sys_phy_addr_lo/hi
    cmd->cmd_payload[0]  = static_cast<uint32_t>(tmr_mc_addr & 0xFFFFFFFF);
    cmd->cmd_payload[1]  = static_cast<uint32_t>((tmr_mc_addr >> 32) & 0xFFFFFFFF);
    cmd->cmd_payload[2]  = tmr_size;
    cmd->cmd_payload[3]  = 0x00000002; // bitfield: virt_phy_addr = 1
}

inline int tmrLoad(display::RegSink& sink,
                   RingState* ring,
                   uint64_t tmr_mc_addr,
                   uint32_t tmr_size,
                   uint64_t cmd_buf_mc_addr,
                   uint64_t fence_mc_addr,
                   uint32_t* out_resp_status = nullptr) {
    GfxCmdResp cmd;
    for (uint32_t i = 0; i < kCmdBufSize / sizeof(uint32_t); ++i) {
        reinterpret_cast<uint32_t*>(&cmd)[i] = 0;
    }
    prepTmrCmd(&cmd, tmr_mc_addr, tmr_size);

    cmdBufCopy(ring, &cmd);

    int ret = ringSubmitFrame(ring, sink, cmd_buf_mc_addr, fence_mc_addr);
    if (ret != 0) return ret;

    ret = ringWaitForFence(ring, sink, ring->fence_value);
    if (ret != 0) return ret;

    // 解码响应状态：0=成功；>0=TEE 错误码（psp_gfx_if.h:516-520）
    const GfxCmdResp* resp = cmdBufGet(ring);
    if (out_resp_status) { *out_resp_status = resp->resp_status; }
    if (resp->resp_status == kTeeSuccess) return 0;
    return static_cast<int>(resp->resp_status); // TEE 错误码
}

// ---------- 6. LOAD_IP_FW 单帧发送原语 ----------

/// 等 Linux psp_prep_load_ip_fw_cmd_buf (amdgpu_psp.c:3368-3384)
inline void prepLoadIpFwCmd(GfxCmdResp* cmd,
                            uint64_t fw_mc_addr,
                            uint32_t fw_size,
                            uint32_t fw_type) {
    cmd->buf_size        = kCmdBufSize;
    cmd->buf_version     = 1;
    cmd->cmd_id          = GFX_CMD_ID_LOAD_IP_FW;
    cmd->cmd_payload[0]  = static_cast<uint32_t>(fw_mc_addr & 0xFFFFFFFF);
    cmd->cmd_payload[1]  = static_cast<uint32_t>((fw_mc_addr >> 32) & 0xFFFFFFFF);
    cmd->cmd_payload[2]  = fw_size;
    cmd->cmd_payload[3]  = fw_type;
}

/// LOAD_IP_FW 单帧发送（等 Linux psp_execute_ip_fw_load, amdgpu_psp.c:3386-3401）
///
/// 原语完整流程：帧构造 → cmd_buf 拷贝 → ring submit → fence 等待 → 解码响应。
/// M2 阶段可暂不调用，M4 必需。
///
/// @return 0 成功；-1 超时/环错误；>0 TEE 错误码（解码后的响应状态）
inline int executeLoadIpFw(display::RegSink& sink,
                           RingState* ring,
                           uint64_t fw_mc_addr,
                           uint32_t fw_size,
                           uint32_t fw_type,
                           uint64_t cmd_buf_mc_addr,
                           uint64_t fence_mc_addr) {
    GfxCmdResp cmd;
    for (uint32_t i = 0; i < kCmdBufSize / sizeof(uint32_t); ++i) {
        reinterpret_cast<uint32_t*>(&cmd)[i] = 0;
    }
    prepLoadIpFwCmd(&cmd, fw_mc_addr, fw_size, fw_type);

    cmdBufCopy(ring, &cmd);

    int ret = ringSubmitFrame(ring, sink, cmd_buf_mc_addr, fence_mc_addr);
    if (ret != 0) return ret;

    ret = ringWaitForFence(ring, sink, ring->fence_value);
    if (ret != 0) return -1; // 超时

    // 解码响应状态：0=成功；>0=TEE 错误码（psp_gfx_if.h:516-520）
    const GfxCmdResp* resp = cmdBufGet(ring);
    if (resp->resp_status == kTeeSuccess) return 0;
    return static_cast<int>(resp->resp_status);
}

// ---------- 6b. TA 解析 + ASD 装载 ----------

/// TA 子固件描述（等 Linux `psp->asd_context.bin_desc`，amdgpu_psp.c:4306-4315）
struct TaBinDesc {
    uint32_t fw_version;    // 子固件版本（desc->fw_version）
    uint32_t offset_bytes;  // 子固件相对 TA 文件起始的偏移
    uint32_t size_bytes;    // 子固件大小
    bool     found;         // 是否在 TA 头里找到该类型
};

/// 等 Linux psp_init_ta_microcode → parse_ta_v2_microcode (amdgpu_psp.c:4414-4440)
/// + parse_ta_bin_descriptor 的 ASD 分支 (amdgpu_psp.c:4310-4316)
///
/// 只提取 ASD（TA_FW_TYPE_PSP_ASD=1）；本机实测 `psp_13_0_4_ta.bin` 含
/// desc[0]=ASD size=217344（XGMI/RAS/HDCP/DTM/RAP 等其它 TA 类型乙线暂不装载，
/// 需要时再补）。子固件起始 = ta 头 + desc.offset_bytes + header.ucode_array_offset_bytes
/// （等 amdgpu_psp.c:4306-4308）。
///
/// @return 0 成功（含「未找到 ASD」，此时 out->found=false）；-1 头不是 v2.0 或越界
inline int parseTaAsd(const uint8_t* ta_data, uint32_t ta_size, TaBinDesc* out) {
    if (!ta_data || !out) return -1;

    CommonFwHeader hdr;
    if (!parseCommonFwHeader(ta_data, ta_size, &hdr)) return -1;
    // 等 parse_ta_v2_microcode:4423-4424 —— 头版本必须为 2
    if (hdr.header_version_major != 2) return -1;

    uint32_t count = 0;
    if (!readU32Le(ta_data, ta_size, 32, &count)) return -1;
    // 等 parse_ta_v2_microcode:4426（UCODE_MAX_PSP_PACKAGING=26：
    //   ((sizeof(union amdgpu_firmware_header)=0x100 - 32 - 4)/16)*2，
    //   amdgpu_ucode.h:473）；TA 文件实测 count=3
    if (count >= 26) return -1;

    out->found = false;
    for (uint32_t i = 0; i < count; ++i) {
        PspFwBinDesc desc;
        if (!pspFwBinDescAt(ta_data, ta_size, i, &desc)) return -1;
        if (desc.fw_type == kTaFwTypePspAsd) {
            // 子固件偏移 = ucode_array_offset_bytes + desc.offset_bytes（amdgpu_psp.c:4306-4308）
            // 两次边界检查都先防 uint32 下溢/回绕：
            //   1) ucode_array_offset_bytes 自身必须 ≤ ta_size
            //   2) desc.offset_bytes 必须 ≤ ta_size - ucode_array_offset_bytes（否则加法回绕）
            if (hdr.ucode_array_offset_bytes > ta_size) return -1;
            if (desc.offset_bytes > ta_size - hdr.ucode_array_offset_bytes) return -1;
            const uint32_t off = hdr.ucode_array_offset_bytes + desc.offset_bytes;
            if (desc.size_bytes > ta_size - off) return -1;
            out->fw_version  = desc.fw_version;
            out->offset_bytes = off;
            out->size_bytes  = desc.size_bytes;
            out->found       = true;
            return 0;
        }
    }
    return 0;
}

/// 等 Linux psp_asd_initialize 的两个提前返回条件 (amdgpu_psp.c:1683-1689)
///
///  1. SRIOV 或 ASD 子固件不存在 ⇒ 跳过
///  2. 无显示硬件 且 MP0 ≥ 13.0.10 ⇒ 跳过
/// Phoenix（13.0.4）：非 SRIOV、有显示硬件、MP0 < 13.0.10 ⇒ 全部不成立 ⇒ 需要装载
/// @param asd_found ASD 子固件是否已从 TA 解析出来
inline bool asdLoadRequired(bool sriov, bool has_display_hw,
                            uint32_t mp0_major, uint32_t mp0_minor,
                            bool asd_found) {
    if (sriov || !asd_found) return false;
    if (!has_display_hw &&
        (mp0_major > 13 || (mp0_major == 13 && mp0_minor >= 10))) {
        return false;
    }
    return true;
}

/// 等 Linux psp_prep_ta_load_cmd_buf (amdgpu_psp.c:1771-1785)，
/// ASD 专用参数来自 psp_asd_initialize (amdgpu_psp.c:1691-1693)：
///   - cmd_id      = GFX_CMD_ID_LOAD_ASD（ta_load_type）
///   - app_phy_*   = fw_pri_mc_addr（ASD 子固件已拷入 fw_pri）
///   - app_len     = ASD 子固件大小
///   - cmd_buf_*   = shared_mc_addr=0、cmd_buf_len=PSP_ASD_SHARED_MEM_SIZE=0
///                   （amdgpu_psp.c:1691-1692；amdgpu_psp.h:70）
inline void prepLoadAsdCmd(GfxCmdResp* cmd,
                           uint64_t fw_mc_addr,
                           uint32_t fw_size) {
    cmd->cmd_id          = GFX_CMD_ID_LOAD_ASD;
    cmd->cmd_payload[0]  = static_cast<uint32_t>(fw_mc_addr & 0xFFFFFFFF);
    cmd->cmd_payload[1]  = static_cast<uint32_t>((fw_mc_addr >> 32) & 0xFFFFFFFF);
    cmd->cmd_payload[2]  = fw_size;
    cmd->cmd_payload[3]  = 0; // cmd_buf_phy_addr_lo
    cmd->cmd_payload[4]  = 0; // cmd_buf_phy_addr_hi
    cmd->cmd_payload[5]  = 0; // cmd_buf_len（PSP_ASD_SHARED_MEM_SIZE=0）
}

/// 等 Linux psp_ta_load (amdgpu_psp.c:1830-1862) 的 ASD 形态
///
/// 步骤：copyFw(ASD) → 构造 LOAD_ASD 帧 → cmd_buf 拷贝 → submit → fence → 解码响应。
/// 门控（asdLoadRequired）由调用方在 M4 决定；本原语默认可用。
///
/// @return 0 成功；-1 超时/环错误；>0 TEE 错误码
inline int executeLoadAsd(display::RegSink& sink,
                          RingState* ring,
                          const TaBinDesc& asd,
                          uint64_t fw_pri_mc_addr,
                          uint64_t cmd_buf_mc_addr,
                          uint64_t fence_mc_addr) {
    if (!asd.found) return -1;

    GfxCmdResp cmd;
    for (uint32_t i = 0; i < kCmdBufSize / sizeof(uint32_t); ++i) {
        reinterpret_cast<uint32_t*>(&cmd)[i] = 0;
    }

    // ASD 子固件已由调用方拷入 fw_pri（等 Linux psp_ta_load:1837-1842）
    // asd.offset_bytes/size_bytes 为相对 TA 文件起始的切片信息；
    // fw_pri_mc_addr 指向 fw_pri 缓冲（ASD 数据已就位）。

    prepLoadAsdCmd(&cmd, fw_pri_mc_addr, asd.size_bytes);
    cmdBufCopy(ring, &cmd);

    int ret = ringSubmitFrame(ring, sink, cmd_buf_mc_addr, fence_mc_addr);
    if (ret != 0) return ret;

    ret = ringWaitForFence(ring, sink, ring->fence_value);
    if (ret != 0) return -1; // 超时

    const GfxCmdResp* resp = cmdBufGet(ring);
    if (resp->resp_status == kTeeSuccess) return 0;
    return static_cast<int>(resp->resp_status);
}

// ---------- 6c. RLC autoload ----------

/// 等 Linux psp_rlc_autoload_start (amdgpu_psp.c:3896-3909)
///
/// cmd-id-only 帧：acquire_psp_cmd_buf 的 memset 后只写 cmd_id，其余字段全 0
/// （mac-amdgpu psp_v14_0.cpp:1370-1394 同构注释：buf_size/buf_version 故意留 0）。
/// 触发时机（Linux psp_load_non_psp_fw:3575-3583）：autoload_supported 且刚送完
/// AMDGPU_UCODE_ID_RLC_G ⇒ 告知 PSP 所有 GFX 固件已就位，可启动 per-IP autoload。
inline void prepAutoloadRlcCmd(GfxCmdResp* cmd) {
    cmd->cmd_id = GFX_CMD_ID_AUTOLOAD_RLC;
}

/// RLC autoload 帧发送（cmd-id-only + 完整提交流程）
///
/// @return 0 成功；-1 超时/环错误；>0 TEE 错误码
inline int rlcAutoloadStart(display::RegSink& sink,
                            RingState* ring,
                            uint64_t cmd_buf_mc_addr,
                            uint64_t fence_mc_addr) {
    GfxCmdResp cmd;
    for (uint32_t i = 0; i < kCmdBufSize / sizeof(uint32_t); ++i) {
        reinterpret_cast<uint32_t*>(&cmd)[i] = 0;
    }
    prepAutoloadRlcCmd(&cmd);

    cmdBufCopy(ring, &cmd);

    int ret = ringSubmitFrame(ring, sink, cmd_buf_mc_addr, fence_mc_addr);
    if (ret != 0) return ret;

    ret = ringWaitForFence(ring, sink, ring->fence_value);
    if (ret != 0) return -1; // 超时

    const GfxCmdResp* resp = cmdBufGet(ring);
    if (resp->resp_status == kTeeSuccess) return 0;
    return static_cast<int>(resp->resp_status);
}

/// 等 Linux gfx_v11_0_wait_for_rlc_autoload_complete (gfx_v11_0.c:3104-3133)
///
/// 轮询 `cp_status==0 且 BOOTLOAD_COMPLETE==1`，预算 kRegPollUs 轮（1M×1µs）。
/// gfx1103（GC 11.0.3）用 regRLC_RLCS_BOOTLOAD_STATUS=0x4e82
/// （gc_11_0_0_offset.h:10418，BASE_IDX=1；11.0.1/11.0.4/11.5.x/11.7.x 才用 0x4e7e
///  变体，11.0.3 不在该清单内）。BOOTLOAD_COMPLETE=bit31（gc_11_0_0_sh_mask.h:36305-36310）。
///
/// ⚠️ 绝对地址 = (GC_BASE + 0x4e82) × 4：GC_BASE 来自 IP discovery，离线未知
///    （NRed.cpp:112-115 已证静态表推导的 MMHUB 基址读回 0xFFFFFFFF 走不通），
///    故由调用方在 GC_BASE 就绪后传入绝对字节地址；等待缺失不影响发帧本身。
///
/// @return 0 完成；-1 超时
inline int waitRlcAutoloadComplete(display::RegSink& sink,
                                   display::RegAddr cp_stat_addr,
                                   display::RegAddr bootload_status_addr) {
    for (uint32_t i = 0; i < kRegPollUs; ++i) {
        const uint32_t cp_status    = sink.read(cp_stat_addr);
        const uint32_t bootload     = sink.read(bootload_status_addr);
        if (cp_status == 0 && (bootload & 0x80000000u) != 0) return 0;
        sink.delayMicroseconds(1);
    }
    return -1;
}

// ---------- 主流程 ----------

/// 执行 PSP 13.0.4 bringup 主流程
///
/// 顺序严格遵循 Linux psp_hw_start (amdgpu_psp.c:2946-3090)。
///
/// @param ctx    bringup 上下文（调用方初始化）
/// @param boot_time_tmr    由 psp 结构初始化决定（13.0.4: false）
/// @param autoload_supported 由 psp 结构初始化决定
/// @param fw_pri_mc_addr    fw_pri_buf 的模拟 MC 地址
/// @param fence_mc_addr     fence_buf 的模拟 MC 地址
/// @param cmd_buf_mc_addr   cmd_buf 的模拟 MC 地址
/// @param ring_mc_addr      ring_buf 的模拟 MC 地址
/// @return 0 成功，否则失败（ctx->last_step 指示中断位置）

/// Phase 1: 运行到 TmrInit 完成（含），计算 tmr_size
/// @return 0 成功，ctx->tmr_size 已填充；否则失败
inline int bringupRunToTmrInit(BringupCtx* ctx,
                               bool boot_time_tmr,
                               bool autoload_supported,
                               uint64_t fw_pri_mc_addr,
                               uint64_t fence_mc_addr,
                               uint64_t cmd_buf_mc_addr,
                               uint64_t ring_mc_addr) {
    display::RegSink& sink = *ctx->sink;
    ctx->last_step = BringupStep::None;
    ctx->last_error = 0;

    // ========== Step 1: Bootloader chain ==========
    if (isFwValid(ctx->bl_comps[0])) {
        ctx->last_step = BringupStep::BlLoadKdb;
        ctx->last_error = bootloaderLoadComponent(sink, ctx->fw_pri_buf, ctx->bl_comps[0], fw_pri_mc_addr);
        if (ctx->last_error != 0) return ctx->last_error;
    }
    if (isFwValid(ctx->bl_comps[1])) {
        ctx->last_step = BringupStep::BlLoadSpl;
        ctx->last_error = bootloaderLoadComponent(sink, ctx->fw_pri_buf, ctx->bl_comps[1], fw_pri_mc_addr);
        if (ctx->last_error != 0) return ctx->last_error;
    }
    if (isFwValid(ctx->bl_comps[2])) {
        ctx->last_step = BringupStep::BlLoadSysdrv;
        ctx->last_error = bootloaderLoadComponent(sink, ctx->fw_pri_buf, ctx->bl_comps[2], fw_pri_mc_addr);
        if (ctx->last_error != 0) return ctx->last_error;
    }
    if (isFwValid(ctx->bl_comps[3])) {
        ctx->last_step = BringupStep::BlLoadSocDrv;
        ctx->last_error = bootloaderLoadComponent(sink, ctx->fw_pri_buf, ctx->bl_comps[3], fw_pri_mc_addr);
        if (ctx->last_error != 0) return ctx->last_error;
    }
    if (isFwValid(ctx->bl_comps[4])) {
        ctx->last_step = BringupStep::BlLoadIntfDrv;
        ctx->last_error = bootloaderLoadComponent(sink, ctx->fw_pri_buf, ctx->bl_comps[4], fw_pri_mc_addr);
        if (ctx->last_error != 0) return ctx->last_error;
    }
    if (isFwValid(ctx->bl_comps[5])) {
        ctx->last_step = BringupStep::BlLoadDgbDrv;
        ctx->last_error = bootloaderLoadComponent(sink, ctx->fw_pri_buf, ctx->bl_comps[5], fw_pri_mc_addr);
        if (ctx->last_error != 0) return ctx->last_error;
    }
    if (isFwValid(ctx->bl_comps[6])) {
        ctx->last_step = BringupStep::BlLoadSos;
        ctx->last_error = bootloaderLoadSos(sink, ctx->fw_pri_buf, ctx->bl_comps[6], fw_pri_mc_addr);
        if (ctx->last_error != 0) return ctx->last_error;
    }

    // ========== Step 2: Ring create ==========
    ctx->last_step = BringupStep::RingCreate;
    ctx->last_error = ringCreate(sink, ring_mc_addr, kRingSizeBytes, PSP_RING_TYPE__KM);
    if (ctx->last_error != 0) return ctx->last_error;

    // ========== Step 3: Update fw reservation ==========
    ctx->last_step = BringupStep::UpdateFwReserve;
    ctx->last_error = updateFwReservation(0);
    if (ctx->last_error != 0) return ctx->last_error;

    // ========== Step 4: TMR init ==========
    if (!boot_time_tmr || autoload_supported) {
        ctx->last_step = BringupStep::TmrInit;
        ctx->last_error = tmrInit(sink, ctx->ring,
                                  ctx->fw_pri_buf,
                                  ctx->toc_data, ctx->toc_size,
                                  fw_pri_mc_addr,
                                  cmd_buf_mc_addr,
                                  fence_mc_addr,
                                  &ctx->tmr_size,
                                  &ctx->last_resp_status);
        if (ctx->last_error != 0) return ctx->last_error;
    }

    ctx->last_step = BringupStep::TmrInit;
    return 0;
}

/// Phase 2: 运行 TmrLoad (SETUP_TMR)，需 ctx->tmr_buf 已分配
/// @param tmr_mc_addr TMR 物理地址（由调用方分配后填入）
/// @return 0 成功，否则失败
inline int bringupRunTmrLoad(BringupCtx* ctx,
                             bool boot_time_tmr,
                             bool autoload_supported,
                             uint64_t tmr_mc_addr,
                             uint64_t cmd_buf_mc_addr,
                             uint64_t fence_mc_addr) {
    display::RegSink& sink = *ctx->sink;
    ctx->last_error = 0;

    // ========== Step 5: TMR load (SETUP_TMR) ==========
    ctx->last_step = BringupStep::TmrLoad;
    if (!skipTmr(boot_time_tmr, autoload_supported)) {
        NRED_TRACE("tmrLoad: start tmr_size=0x%X", ctx->tmr_size);
        NRED_TRACE("tmrLoad: submit tmr_mc=0x%llX tmr_size=0x%X",
                   (unsigned long long)tmr_mc_addr, ctx->tmr_size);
        ctx->last_error = tmrLoad(sink, ctx->ring,
                                  tmr_mc_addr, ctx->tmr_size,
                                  cmd_buf_mc_addr,
                                  fence_mc_addr,
                                  &ctx->last_resp_status);
        if (ctx->last_error != 0) {
            NRED_TRACE("tmrLoad: failed ret=%d", ctx->last_error);
            return ctx->last_error;
        }
        NRED_TRACE("tmrLoad: ok");
    }

    ctx->last_step = BringupStep::Done;
    return 0;
}

/// 完整流程（Phase 1 + TMR 分配 + Phase 2）——供测试/兼容使用
/// 注：实际 kext 接线应分两阶段调用，以便在 Phase 1/2 间分配 TMR
inline int bringupRun(BringupCtx* ctx,
                      bool boot_time_tmr,
                      bool autoload_supported,
                      uint64_t fw_pri_mc_addr,
                      uint64_t fence_mc_addr,
                      uint64_t cmd_buf_mc_addr,
                      uint64_t ring_mc_addr) {
    int ret = bringupRunToTmrInit(ctx, boot_time_tmr, autoload_supported,
                                  fw_pri_mc_addr, fence_mc_addr,
                                  cmd_buf_mc_addr, ring_mc_addr);
    if (ret != 0) return ret;
    // 注：完整流程中 TMR 分配由调用方在两阶段间完成
    // 此处仅为测试兼容，直接用 ctx->tmr_buf（若为空则用模拟地址）
    uint64_t tmr_mc = (ctx->tmr_buf)
        ? reinterpret_cast<uint64_t>(ctx->tmr_buf)
        : 0xDEAD0000ULL;
    return bringupRunTmrLoad(ctx, boot_time_tmr, autoload_supported,
                             tmr_mc, cmd_buf_mc_addr, fence_mc_addr);
}

} // namespace fw
