#pragma once
// 第 6 阶段契约：进程挂起/恢复 + 优先级/亲和性控制。
// 归架构所有，已冻结。执行协议与 ProcessOps.h 一致：
// 先做身份复核（createTime ±1s）与 ProtectedList 硬闸门——
// 挂起关键进程（csrss 等）比杀掉它更糟。
// 挂起使用 NtSuspendProcess/NtResumeProcess（半官方文档，从 ntdll
// 动态绑定；有文档的按线程方案作为兜底）。
#include <cstdint>
#include <string>
#include "core/ProcData.h"

namespace stm {
namespace ops {

enum class ProcPriority : uint32_t {
    Idle = 0, BelowNormal, Normal, AboveNormal, High, Realtime
};

struct ProcessControlInfo {
    uint32_t priorityClass = 0;      // 基础优先级类别（GetPriorityClass 常量）
    uint64_t affinityMask = 0;       // 进程亲和性掩码（GetProcessAffinityMask）
    uint64_t systemAffinityMask = 0;
    bool suspended = false;          // 所有线程均挂起/等待挂起（自包含检查）
    bool suspendedAvail = false;     // 挂起检查无法运行时为 false
};

bool SuspendProcess(const ProcKey& key, std::wstring* err);
bool ResumeProcess(const ProcKey& key, std::wstring* err);
bool SetProcPriority(const ProcKey& key, ProcPriority p, std::wstring* err);
bool SetProcAffinity(const ProcKey& key, uint64_t mask, std::wstring* err);
// 无权限时如实失败（err）；suspended 标志尽力而为。
ProcessControlInfo GetProcessControlInfo(const ProcKey& key, std::wstring* err);

}  // namespace ops
}  // namespace stm
