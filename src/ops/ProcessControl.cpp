// 进程挂起/恢复 + 优先级/亲和性控制（契约 ops/ProcessControl.h）。
//
// 执行协议（与 ops/ProcessOps.h 一致）：
//  1. ProtectedList 硬闸门先行——挂起关键进程（csrss 等）
//     比杀掉它更糟，因此 Suspend/Resume/Priority/Affinity 都在任何
//     句柄打开之前过闸。GetProcessControlInfo 只读且不过闸。
//  2. 按操作所需的最小访问权 OpenProcess。
//  3. 身份复核：GetProcessTimes -> createTime 相差 +-1s 内，否则拒绝并以
//     "已退出或 PID 已被复用". Never trust pid alone.
//
// Suspend/Resume 从 ntdll 动态绑定 NtSuspendProcess/NtResumeProcess。两者均
// 属半官方文档（MSDN kernel32/ntdll 参考未收录；自 Vista 起稳定，
// PsSuspend/BGInfo 类工具在用）。有文档的按线程兜底（Toolhelp 线程
// 快照 + OpenThread + SuspendThread/ResumeThread）覆盖导出缺失的
// 罕见情形。suspended 标志经 NtQuerySystemInformation(System-
// ProcessInformation) 的线程 State/WaitReason 读取——同样是半官方布局，
// 与采集器同源（彼处已由 SelfCheckGate 交叉校验）。
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

constexpr uint64_t kFileTime1Sec = 10'000'000ull;  // FILETIME 单位为 100 ns

constexpr uint32_t kSystemProcessInformation = 5;  // SYSTEM_INFORMATION_CLASS（系统信息类别）
constexpr uint32_t kStatusInfoLengthMismatch = 0xC0000004u;
constexpr uint32_t kStatusAccessDenied = 0xC0000022u;

constexpr uint32_t kThreadStateWaiting = 5;   // KTHREAD_STATE::Waiting（等待）
constexpr uint32_t kWaitReasonSuspended = 5;  // KWAIT_REASON::Suspended（挂起）

constexpr DWORD kThreadSuspendError = 0xFFFFFFFFu;

bool SameCreateTime(uint64_t a, uint64_t b) {
    const uint64_t d = a > b ? a - b : b - a;
    return d <= kFileTime1Sec;  // +-1s 容差，架构第 6.1 节
}

uint64_t FileTimeToU64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

enum class OpenVerdict { Opened, Gone, Denied };

// 身份复核 + 最小权限打开（与 ProcessOps.cpp 的 OpenVerified 协议相同：
// 拒绝访问时已提权的调用方带 SeDebugPrivilege 重试一次，在此释放）。
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
// ntdll 挂起/恢复（半官方文档的导出函数，像采集器的
// NtQuerySystemInformation 一样动态绑定；ntdll 总是已加载、永不卸载）。
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

// 有文档的按线程兜底（Toolhelp 线程快照；对目标的每个线程
// 各调一次 SuspendThread/ResumeThread——与 ntdll 导出相同的
// 每线程一次计数语义）。仅在 ntdll 缺少进程级导出时使用。
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
            if (!t) continue;  // 正在退出的线程：良性
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
// 挂起检查：NtQSI(SystemProcessInformation) 的线程 State/WaitReason，
// 与采集器解析的半官方 x64 布局相同（自包含镜像；下方结构
// 只为该标志服务）。检查无法诚实运行时 suspendedAvail=false。
// ---------------------------------------------------------------------------
struct NtUnicodeString {
    uint16_t length;        // 字节数，不含 NUL 终止符
    uint16_t maximumLength;
    uint32_t pad;
    wchar_t* buffer;
};
static_assert(sizeof(NtUnicodeString) == 16);

struct SystemThreadInfoX64 {  // SYSTEM_THREAD_INFORMATION，x64（80 字节）
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

// true = 得出结论（*suspended 有效、*avail=true）；false = 检查不可用。
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
            walked = false;  // 该 OS 构建上布局可疑 => 诚实地报不可用
            break;
        }
        if (reinterpret_cast<uintptr_t>(e->uniqueProcessId) == pid) {
            found = true;
            allSusp = e->numberOfThreads > 0;  // 零线程 => 不能断言已挂起
            const uint8_t* t = p + sizeof(SystemProcessInfoX64);  // 线程数组位于 +0x100
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
            break;  // 缓冲内 pid 唯一
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
    return NORMAL_PRIORITY_CLASS;  // 契约内的取值不会走到这里（上文已校验）
}

// 共享前奏：保护硬闸门 -> 身份复核的最小权限打开。必须停止时
// 返回 false 并设置 *err（拒绝 / 已消失 / 被拒）。
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
    // PROCESS_SUSPEND_RESUME 是 NtSuspendProcess 有文档的最小权限。
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
    // High/Realtime 受 SeIncreaseBasePriorityPrivilege 管辖；PROCESS_SET_INFORMATION
    // 承载 SetPriorityClass。
    if (!GateAndOpen(key, L"设置优先级", PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,
                     &h, err)) {
        return false;
    }
    if (p == ProcPriority::Realtime) {
        // 仅额外告警：仍会尝试，失败如实向上传播。
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
    // 只读操作：不过保护闸门（展示 csrss 的优先级/亲和性是安全的），
    // 但身份协议仍适用于该 ProcKey。
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
    // 否则：按契约尽力而为保持 false/false（"suspended 标志尽力而为"）。
    return info;
}

}  // namespace stm::ops
