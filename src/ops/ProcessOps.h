#pragma once
// 进程破坏性操作（架构第 6 节）。契约头——归架构所有。
//
// 执行协议（对每个实现者都强制）：
//  1. 身份复核：OpenProcess -> GetProcessTimes -> 比较 createTime（±1s）。
//     Mismatch => report "已退出或 PID 复用" and refuse to act. NEVER trust pid alone.
//  2. ProtectedList 是 ops 内的硬闸门（UI 确认只是第一道闸门）。
//  3. 树操作：执行时重新快照；逐成员保护检查 => 跳过并上报。
#include <cstdint>
#include <string>
#include <vector>
#include "core/ProcData.h"

namespace stm {
namespace ops {

struct TreeResult {
    int planned = 0;            // 计划/执行时发现的成员数
    int terminated = 0;
    int failed = 0;
    int skippedProtected = 0;   // 跳过的受保护成员（已上报，绝不静默丢弃）
    std::wstring firstError;    // 首个失败细节（中文，面向用户）
};

// 单进程终止。已提权时相机启用 SeDebugPrivilege。
bool TerminateProcessById(const ProcKey& key, std::wstring* err);

// 仅规划的后代枚举（纯快照遍历；无副作用）。
// Used by the UI confirm dialog: "预计 N 个（执行时可能变化）".
bool PlanTerminateTree(const ProcKey& root, std::vector<ProcKey>* out, std::wstring* err);

// 先叶子后根地终止根 + 后代，带重新快照与逐成员复核。
bool TerminateTree(const ProcKey& root, TreeResult* out, std::wstring* err);

// 对目标执行 EmptyWorkingSet（PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION）。
bool TrimWorkingSet(const ProcKey& key, std::wstring* err);

// 全局待备列表清空（SeProfileSingleProcessPrivilege；仅管理员）。
// 诚实语义：只释放系统缓存页，而非进程内存。
bool PurgeStandbyList(std::wstring* err);

// 原因非空 => 进程在内置保护名单上（UI 据此禁用菜单项）。
std::wstring ProtectedReason(const ProcKey& key, const std::wstring& name, const std::wstring& path);

}  // namespace ops
}  // namespace stm
