#pragma once
// Process destructive operations (arch section 6). Contract header — architect-owned.
//
// Execution protocol (mandatory for every implementer):
//  1. Identity re-verify: OpenProcess -> GetProcessTimes -> compare createTime (±1s).
//     Mismatch => report "已退出或 PID 复用" and refuse to act. NEVER trust pid alone.
//  2. ProtectedList is a hard gate inside ops (UI confirm is only the first gate).
//  3. Tree ops: re-snapshot at execution time; per-member protected check => skip+report.
#include <cstdint>
#include <string>
#include <vector>
#include "core/ProcData.h"

namespace stm {
namespace ops {

struct TreeResult {
    int planned = 0;            // members found at plan/execution time
    int terminated = 0;
    int failed = 0;
    int skippedProtected = 0;   // protected members skipped (reported, not silently dropped)
    std::wstring firstError;    // first failure detail (Chinese, user-facing)
};

// Single-process terminate. Enables SeDebugPrivilege opportunistically when elevated.
bool TerminateProcessById(const ProcKey& key, std::wstring* err);

// Plan-only enumeration of descendants (pure snapshot walk; no side effects).
// Used by the UI confirm dialog: "预计 N 个（执行时可能变化）".
bool PlanTerminateTree(const ProcKey& root, std::vector<ProcKey>* out, std::wstring* err);

// Leaf-first terminate of root + descendants, with re-snapshot and per-member re-verify.
bool TerminateTree(const ProcKey& root, TreeResult* out, std::wstring* err);

// EmptyWorkingSet on the target (PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION).
bool TrimWorkingSet(const ProcKey& key, std::wstring* err);

// Global standby list purge (SeProfileSingleProcessPrivilege; admin only).
// Honest semantics: frees SYSTEM cache pages only, NOT process memory.
bool PurgeStandbyList(std::wstring* err);

// Non-empty reason => process is on the built-in protection list (UI disables menu items).
std::wstring ProtectedReason(const ProcKey& key, const std::wstring& name, const std::wstring& path);

}  // namespace ops
}  // namespace stm
