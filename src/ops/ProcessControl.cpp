// Process suspend/resume + priority/affinity control (contract ops/ProcessControl.h).
//
// Execution protocol (identical to ops/ProcessOps.h):
//  1. ProtectedList hard gate FIRST — suspending a critical process (csrss etc.) is
//     worse than killing it, so Suspend/Resume/Priority/Affinity all pass the gate
//     before any handle is opened. GetProcessControlInfo is read-only and ungated.
//  2. OpenProcess with the minimal access right per operation.
//  3. Identity re-verify: GetProcessTimes -> createTime within +-1s, else refuse with
//     "已退出或 PID 已被复用". Never trust pid alone.
//
// Suspend/Resume bind NtSuspendProcess/NtResumeProcess from ntdll dynamically. Both are
// semi-documented (not in the MSDN kernel32/ntdll reference; stable since Vista and used
// by PsSuspend/BGInfo-class tools). The documented per-thread fallback (Toolhelp thread
// snapshot + OpenThread + SuspendThread/ResumeThread) covers the exotic case where the
// exports are missing. The suspended flag is read via NtQuerySystemInformation(System-
// ProcessInformation) thread State/WaitReason — also semi-documented layout, same source
// the collector uses (cross-validated there by SelfCheckGate).
#include "ops/ProcessControl.h"
#include "ops/ProcessOps.h"
#include "core/Err.h"
#include "core/HandleGuard.h"
#include "core/Log.h"
#include "core/Privilege.h"
#include "core/Str.h"
#include <windows.h>
#include <tlhelp32.h>
#include <cstddef>
#include <utility>
#include <vector>

namespace stm::ops {
namespace {

constexpr uint64_t kFileTime1Sec = 10'000'000ull;  // FILETIME unit is 100 ns

constexpr uint32_t kSystemProcessInformation = 5;  // SYSTEM_INFORMATION_CLASS
constexpr uint32_t kStatusInfoLengthMismatch = 0xC0000004u;
constexpr uint32_t kStatusAccessDenied = 0xC0000022u;

constexpr uint32_t kThreadStateWaiting = 5;   // KTHREAD_STATE::Waiting
constexpr uint32_t kWaitReasonSuspended = 5;  // KWAIT_REASON::Suspended

constexpr DWORD kThreadSuspendError = 0xFFFFFFFFu;

bool SameCreateTime(uint64_t a, uint64_t b) {
    const uint64_t d = a > b ? a - b : b - a;
    return d <= kFileTime1Sec;  // +-1s tolerance, arch section 6.1
}

uint64_t FileTimeToU64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

enum class OpenVerdict { Opened, Gone, Denied };

// Identity re-verify + minimal-access open (same protocol as ProcessOps.cpp OpenVerified:
// on ACCESS_DENIED an elevated caller retries once with SeDebugPrivilege, released here).
OpenVerdict OpenVerifiedControl(const ProcKey& key, DWORD access, UniqueHandle* out,
                                std::wstring* err) {
    out->reset();
    HANDLE raw = OpenProcess(access, FALSE, key.pid);
    if (!raw) {
        const uint32_t hr = LastHr();
        if (hr == static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED))) {
            if (IsProcessElevated()) {
                std::wstring perr;
                if (EnablePrivilege(L"SeDebugPrivilege", true, &perr)) {
                    raw = OpenProcess(access, FALSE, key.pid);
                    EnablePrivilege(L"SeDebugPrivilege", false, nullptr);
                } else {
                    STM_LOG_INFO("ops", L"SeDebugPrivilege 启用失败：{}", perr);
                }
            }
            if (!raw) {
                if (err) {
                    *err = L"拒绝访问：目标进程受系统保护或需要管理员权限（" +
                           ErrContext(L"OpenProcess", LastHr()) + L"）";
                }
                return OpenVerdict::Denied;
            }
        } else {
            if (err) {
                *err = L"目标进程已退出或 PID 已被复用（" + ErrContext(L"OpenProcess", hr) + L"）";
            }
            return OpenVerdict::Gone;
        }
    }
    UniqueHandle h(raw);
    FILETIME c{}, x{}, k{}, u{};
    if (!GetProcessTimes(h.get(), &c, &x, &k, &u)) {
        if (err) {
            *err = L"目标进程已退出或 PID 已被复用（" +
                   ErrContext(L"GetProcessTimes", LastHr()) + L"）";
        }
        return OpenVerdict::Gone;
    }
    if (!SameCreateTime(FileTimeToU64(c), key.createTime)) {
        if (err) *err = L"目标进程已退出或 PID 已被复用";
        return OpenVerdict::Gone;
    }
    *out = std::move(h);
    return OpenVerdict::Opened;
}

// ---------------------------------------------------------------------------
// ntdll suspend/resume (semi-documented exports, dynamically bound like the
// collector's NtQuerySystemInformation; ntdll is always loaded, never unloaded).
// ---------------------------------------------------------------------------
using NtSuspendResumeFn = int32_t (WINAPI*)(HANDLE);

NtSuspendResumeFn BindNtExport(const char* name) {
    const HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (!nt) return nullptr;
    return reinterpret_cast<NtSuspendResumeFn>(
        reinterpret_cast<void*>(GetProcAddress(nt, name)));
}

std::wstring NtStatusError(const wchar_t* what, uint32_t status) {
    if (status == kStatusAccessDenied) {
        return std::wstring(what) + L"失败：需要管理员权限";
    }
    return Fmt(L"{}失败（NTSTATUS 0x{:08X}）", what, status);
}

// Documented per-thread fallback (Toolhelp thread snapshot; SuspendThread/ResumeThread
// once per thread of the target — the same one-count-per-thread semantics as the ntdll
// exports). Used only when ntdll does not export the process-wide functions.
bool PerThreadSuspendResume(uint32_t pid, bool suspend, std::wstring* err) {
    const wchar_t* what = suspend ? L"挂起" : L"恢复";
    HANDLE raw = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (raw == INVALID_HANDLE_VALUE) {
        if (err) *err = ErrContext(L"线程快照失败", LastHr());
        return false;
    }
    UniqueHandle snap(raw);
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    DWORD acted = 0;
    if (Thread32First(snap.get(), &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            UniqueHandle t(OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID));
            if (!t) continue;  // exiting thread: benign
            const DWORD rc = suspend ? SuspendThread(t.get()) : ResumeThread(t.get());
            if (rc != kThreadSuspendError) ++acted;
        } while (Thread32Next(snap.get(), &te));
    }
    if (acted == 0) {
        if (err) *err = std::wstring(what) + L"失败：目标线程均不可访问";
        return false;
    }
    STM_LOG_INFO("ops", L"{}进程 pid={} 使用逐线程回退路径（{} 个线程）", what, pid, acted);
    return true;
}

// ---------------------------------------------------------------------------
// Suspended check: NtQSI(SystemProcessInformation) thread State/WaitReason, same
// semi-documented x64 layout the collector parses (self-contained mirror; the struct
// below only feeds this flag). suspendedAvail=false when the check cannot run honestly.
// ---------------------------------------------------------------------------
struct NtUnicodeString {
    uint16_t length;        // bytes, excluding the NUL terminator
    uint16_t maximumLength;
    uint32_t pad;
    wchar_t* buffer;
};
static_assert(sizeof(NtUnicodeString) == 16);

struct SystemThreadInfoX64 {  // SYSTEM_THREAD_INFORMATION, x64 (80 bytes)
    int64_t kernelTime;      // 0x00
    int64_t userTime;        // 0x08
    int64_t createTime;      // 0x10
    uint32_t waitTime;       // 0x18 (+0x01C pad)
    void* startAddress;      // 0x20
    void* clientIdProcess;   // 0x28
    void* clientIdThread;    // 0x30
    int32_t priority;        // 0x38
    int32_t basePriority;    // 0x3C
    uint32_t contextSwitches;// 0x40
    uint32_t threadState;    // 0x44
    uint32_t waitReason;     // 0x48 (+0x04C pad)
};
static_assert(sizeof(SystemThreadInfoX64) == 0x50);
static_assert(offsetof(SystemThreadInfoX64, threadState) == 0x44);
static_assert(offsetof(SystemThreadInfoX64, waitReason) == 0x48);

struct SystemProcessInfoX64 {  // SYSTEM_PROCESS_INFORMATION, x64 (thread array at +0x100)
    uint32_t nextEntryOffset;               // 0x000
    uint32_t numberOfThreads;               // 0x004
    int64_t workingSetPrivateSize;          // 0x008
    uint32_t hardFaultCount;                // 0x010
    uint32_t numberOfThreadsHighWatermark;  // 0x014
    uint64_t cycleTime;                     // 0x018
    int64_t createTime;                     // 0x020
    int64_t userTime;                       // 0x028
    int64_t kernelTime;                     // 0x030
    NtUnicodeString imageName;              // 0x038
    int32_t basePriority;                   // 0x048
    void* uniqueProcessId;                  // 0x050
    void* inheritedFromUniqueProcessId;     // 0x058
    uint32_t handleCount;                   // 0x060
    uint32_t sessionId;                     // 0x064
    uint64_t uniqueProcessKey;              // 0x068
    uint64_t peakVirtualSize;               // 0x070
    uint64_t virtualSize;                   // 0x078
    uint32_t pageFaultCount;                // 0x080
    uint32_t pad084;                        // 0x084
    uint64_t peakWorkingSetSize;            // 0x088
    uint64_t workingSetSize;                // 0x090
    uint64_t quotaPeakPagedPoolUsage;       // 0x098
    uint64_t quotaPagedPoolUsage;           // 0x0A0
    uint64_t quotaPeakNonPagedPoolUsage;    // 0x0A8
    uint64_t quotaNonPagedPoolUsage;        // 0x0B0
    uint64_t pagefileUsage;                 // 0x0B8
    uint64_t peakPagefileUsage;             // 0x0C0
    uint64_t privatePageCount;              // 0x0C8
    int64_t readOperationCount;             // 0x0D0
    int64_t writeOperationCount;            // 0x0D8
    int64_t otherOperationCount;            // 0x0E0
    int64_t readTransferCount;              // 0x0E8
    int64_t writeTransferCount;             // 0x0F0
    int64_t otherTransferCount;             // 0x0F8
};
static_assert(sizeof(SystemProcessInfoX64) == 0x100);
static_assert(offsetof(SystemProcessInfoX64, numberOfThreads) == 0x04);
static_assert(offsetof(SystemProcessInfoX64, uniqueProcessId) == 0x50);

// True = verdict produced (*suspended valid, *avail=true); false = check unavailable.
bool QuerySuspendedFlag(uint32_t pid, bool* suspended, bool* avail) {
    *suspended = false;
    *avail = false;
    using NtQuerySystemInformationFn = uint32_t (WINAPI*)(uint32_t, void*, uint32_t, uint32_t*);
    static const NtQuerySystemInformationFn ntQsi = []() -> NtQuerySystemInformationFn {
        const HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        if (!nt) return nullptr;
        return reinterpret_cast<NtQuerySystemInformationFn>(
            reinterpret_cast<void*>(GetProcAddress(nt, "NtQuerySystemInformation")));
    }();
    if (!ntQsi) return false;

    std::vector<uint8_t> buf(1u << 20);
    uint32_t needed = 0;
    uint32_t status = kStatusInfoLengthMismatch;
    for (int i = 0; i < 8; ++i) {
        status = ntQsi(kSystemProcessInformation, buf.data(),
                       static_cast<uint32_t>(buf.size()), &needed);
        if (status != kStatusInfoLengthMismatch) break;
        if (needed <= buf.size()) break;
        buf.resize(static_cast<size_t>(needed) + (64u << 10));
    }
    if (status != 0) return false;

    const uint8_t* p = buf.data();
    const uint8_t* const end = buf.data() + buf.size();
    bool walked = true;
    bool found = false;
    bool truncated = false;
    bool allSusp = false;
    while (p + sizeof(SystemProcessInfoX64) <= end) {
        const auto* e = reinterpret_cast<const SystemProcessInfoX64*>(p);
        if (e->nextEntryOffset != 0 && e->nextEntryOffset < sizeof(SystemProcessInfoX64)) {
            walked = false;  // layout looks wrong on this OS build => honest unavailable
            break;
        }
        if (reinterpret_cast<uintptr_t>(e->uniqueProcessId) == pid) {
            found = true;
            allSusp = e->numberOfThreads > 0;  // zero threads => cannot claim suspended
            const uint8_t* t = p + sizeof(SystemProcessInfoX64);  // thread array at +0x100
            for (uint32_t i = 0; i < e->numberOfThreads; ++i) {
                if (t + sizeof(SystemThreadInfoX64) > end) {
                    truncated = true;
                    allSusp = false;
                    break;
                }
                const auto* th = reinterpret_cast<const SystemThreadInfoX64*>(t);
                if (th->threadState != kThreadStateWaiting ||
                    th->waitReason != kWaitReasonSuspended) {
                    allSusp = false;
                }
                t += sizeof(SystemThreadInfoX64);
            }
            break;  // pids are unique in the buffer
        }
        if (e->nextEntryOffset == 0) break;
        p += e->nextEntryOffset;
    }
    if (!walked || !found || truncated) return false;
    *suspended = allSusp;
    *avail = true;
    return true;
}

DWORD PriorityClassOf(ProcPriority p) {
    switch (p) {
        case ProcPriority::Idle: return IDLE_PRIORITY_CLASS;
        case ProcPriority::BelowNormal: return BELOW_NORMAL_PRIORITY_CLASS;
        case ProcPriority::Normal: return NORMAL_PRIORITY_CLASS;
        case ProcPriority::AboveNormal: return ABOVE_NORMAL_PRIORITY_CLASS;
        case ProcPriority::High: return HIGH_PRIORITY_CLASS;
        case ProcPriority::Realtime: return REALTIME_PRIORITY_CLASS;
    }
    return NORMAL_PRIORITY_CLASS;  // unreachable for in-contract values (validated above)
}

// Shared prologue: protected hard gate -> identity-verified minimal open. Returns false
// with *err set when the op must stop (refusal / gone / denied).
bool GateAndOpen(const ProcKey& key, const wchar_t* verb, DWORD access, UniqueHandle* out,
                 std::wstring* err) {
    const std::wstring reason = ProtectedReason(key, L"", L"");
    if (!reason.empty()) {
        if (err) *err = std::wstring(L"已拒绝") + verb + L"：" + reason + L"（保护名单强制拦截）";
        STM_LOG_INFO("ops", L"{}请求被保护名单拦截 pid={}", verb, key.pid);
        return false;
    }
    return OpenVerifiedControl(key, access, out, err) == OpenVerdict::Opened;
}

}  // namespace

bool SuspendProcess(const ProcKey& key, std::wstring* err) {
    UniqueHandle h;
    // PROCESS_SUSPEND_RESUME is the documented minimal right for NtSuspendProcess.
    if (!GateAndOpen(key, L"挂起", PROCESS_SUSPEND_RESUME | PROCESS_QUERY_LIMITED_INFORMATION,
                     &h, err)) {
        return false;
    }
    static const NtSuspendResumeFn ntSuspend = BindNtExport("NtSuspendProcess");
    if (ntSuspend) {
        const uint32_t status = static_cast<uint32_t>(ntSuspend(h.get()));
        if (status != 0) {
            if (err) *err = NtStatusError(L"挂起进程", status);
            STM_LOG_WARN("ops", L"NtSuspendProcess pid={} status=0x{:08X}", key.pid, status);
            return false;
        }
    } else {
        if (!PerThreadSuspendResume(key.pid, true, err)) return false;
    }
    return true;
}

bool ResumeProcess(const ProcKey& key, std::wstring* err) {
    UniqueHandle h;
    if (!GateAndOpen(key, L"恢复", PROCESS_SUSPEND_RESUME | PROCESS_QUERY_LIMITED_INFORMATION,
                     &h, err)) {
        return false;
    }
    static const NtSuspendResumeFn ntResume = BindNtExport("NtResumeProcess");
    if (ntResume) {
        const uint32_t status = static_cast<uint32_t>(ntResume(h.get()));
        if (status != 0) {
            if (err) *err = NtStatusError(L"恢复进程", status);
            STM_LOG_WARN("ops", L"NtResumeProcess pid={} status=0x{:08X}", key.pid, status);
            return false;
        }
    } else {
        if (!PerThreadSuspendResume(key.pid, false, err)) return false;
    }
    return true;
}

bool SetProcPriority(const ProcKey& key, ProcPriority p, std::wstring* err) {
    if (static_cast<uint32_t>(p) > static_cast<uint32_t>(ProcPriority::Realtime)) {
        if (err) *err = L"未知的优先级值";
        return false;
    }
    UniqueHandle h;
    // SeIncreaseBasePriorityPrivilege governs High/Realtime; PROCESS_SET_INFORMATION
    // carries SetPriorityClass.
    if (!GateAndOpen(key, L"设置优先级", PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,
                     &h, err)) {
        return false;
    }
    if (p == ProcPriority::Realtime) {
        // Extra warning only: the attempt is still made and failure propagates honestly.
        STM_LOG_WARN("ops", L"实时优先级请求 pid={}：需 SeIncreaseBasePriorityPrivilege，"
                            L"且可能饿死系统线程", key.pid);
    }
    if (!SetPriorityClass(h.get(), PriorityClassOf(p))) {
        const uint32_t hr = LastHr();
        if (err) {
            *err = p == ProcPriority::Realtime
                       ? L"设置优先级失败：实时优先级需要管理员权限且可能造成系统饥饿（" +
                             ErrContext(L"SetPriorityClass", hr) + L"）"
                       : ErrContext(L"设置优先级失败", hr);
        }
        return false;
    }
    return true;
}

bool SetProcAffinity(const ProcKey& key, uint64_t mask, std::wstring* err) {
    UniqueHandle h;
    if (!GateAndOpen(key, L"设置亲和性", PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,
                     &h, err)) {
        return false;
    }
    DWORD_PTR procMask = 0;
    DWORD_PTR sysMask = 0;
    if (!GetProcessAffinityMask(h.get(), &procMask, &sysMask)) {
        if (err) *err = ErrContext(L"读取亲和性掩码失败", LastHr());
        return false;
    }
    if (mask == 0) {
        if (err) *err = L"亲和性掩码为空：至少需要保留一个处理器";
        return false;
    }
    if (mask & ~static_cast<uint64_t>(sysMask)) {
        if (err) {
            *err = Fmt(L"亲和性掩码 0x{:X} 超出系统亲和性掩码 0x{:X}", mask,
                       static_cast<uint64_t>(sysMask));
        }
        return false;
    }
    if (!SetProcessAffinityMask(h.get(), static_cast<DWORD_PTR>(mask))) {
        if (err) *err = ErrContext(L"设置亲和性失败", LastHr());
        return false;
    }
    return true;
}

ProcessControlInfo GetProcessControlInfo(const ProcKey& key, std::wstring* err) {
    ProcessControlInfo info;
    // Read-only op: no protection gate (priority/affinity of csrss is safe to display),
    // but the identity protocol still applies to the ProcKey.
    UniqueHandle h;
    if (OpenVerifiedControl(key, PROCESS_QUERY_LIMITED_INFORMATION, &h, err) !=
        OpenVerdict::Opened) {
        return info;
    }
    const DWORD pc = GetPriorityClass(h.get());
    if (pc == 0) {
        if (err) *err = L"目标进程已退出或 PID 已被复用（" +
                        ErrContext(L"GetPriorityClass", LastHr()) + L"）";
        return info;
    }
    info.priorityClass = pc;
    DWORD_PTR procMask = 0;
    DWORD_PTR sysMask = 0;
    if (GetProcessAffinityMask(h.get(), &procMask, &sysMask)) {
        info.affinityMask = static_cast<uint64_t>(procMask);
        info.systemAffinityMask = static_cast<uint64_t>(sysMask);
    }
    bool suspended = false;
    bool suspendedAvail = false;
    if (QuerySuspendedFlag(key.pid, &suspended, &suspendedAvail)) {
        info.suspended = suspended;
        info.suspendedAvail = suspendedAvail;
    }
    // else: best-effort stays false/false per the contract ("suspended flag best-effort").
    return info;
}

}  // namespace stm::ops
