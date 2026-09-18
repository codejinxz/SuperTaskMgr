#pragma once
// Phase-6 contract: process suspend/resume + priority/affinity control.
// Architect-owned, frozen. Execution protocol identical to ProcessOps.h:
// identity re-verify (createTime ±1s) and ProtectedList hard gate FIRST —
// suspending a critical process (csrss etc.) is worse than killing it.
// Suspend uses NtSuspendProcess/NtResumeProcess (semi-documented, dynamically
// bound from ntdll; the documented per-thread alternative is the fallback).
#include <cstdint>
#include <string>
#include "core/ProcData.h"

namespace stm {
namespace ops {

enum class ProcPriority : uint32_t {
    Idle = 0, BelowNormal, Normal, AboveNormal, High, Realtime
};

struct ProcessControlInfo {
    uint32_t priorityClass = 0;      // base priority class (GetPriorityClass constants)
    uint64_t affinityMask = 0;       // process affinity mask (GetProcessAffinityMask)
    uint64_t systemAffinityMask = 0;
    bool suspended = false;          // all threads in suspend/wait-suspended (self-contained check)
    bool suspendedAvail = false;     // false when the suspended check could not run
};

bool SuspendProcess(const ProcKey& key, std::wstring* err);
bool ResumeProcess(const ProcKey& key, std::wstring* err);
bool SetProcPriority(const ProcKey& key, ProcPriority p, std::wstring* err);
bool SetProcAffinity(const ProcKey& key, uint64_t mask, std::wstring* err);
// Fails honestly (err) without rights; suspended flag best-effort.
ProcessControlInfo GetProcessControlInfo(const ProcKey& key, std::wstring* err);

}  // namespace ops
}  // namespace stm
