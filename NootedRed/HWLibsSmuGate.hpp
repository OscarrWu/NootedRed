// SMU 发送有界准入（超时-L1）白名单判定
// 纯逻辑、无依赖（仅 IOTypes.h + 消息 ID 常量），供 HWLibs.cpp 与离线测试共用。
// 依据：tmp/lead/下一步技术裁定-2026-10-09.md §2.2、§4 T1
#pragma once
#include <IOKit/IOTypes.h>
#include <GPUDriversAMD/PhoenixPPSMC.hpp>
#include <GPUDriversAMD/RenoirPPSMC.hpp>

namespace NRedSmuGate {

// 白名单消息 ID（去重后）：
//   0x01 = PPSMC_MSG_TestMessage
//   0x02 = PPSMC_MSG_GetPmfwVersion / PPSMC_MSG_GetSmuVersion（同值）
//   0x03 = PPSMC_MSG_GetDriverIfVersion
// 其余全部拒绝（零等待，直接返回 kCAILResultNoResponse）。
inline bool smuMsgAllowedByTimeoutL1(const UInt32 message) noexcept
{
    return message == PhoenixPPSMC::PPSMC_MSG_TestMessage
        || message == PhoenixPPSMC::PPSMC_MSG_GetPmfwVersion
        || message == PhoenixPPSMC::PPSMC_MSG_GetDriverIfVersion
        || message == PPSMC_MSG_GetSmuVersion;  // 0x02，与 GetPmfwVersion 同值
}

} // namespace NRedSmuGate