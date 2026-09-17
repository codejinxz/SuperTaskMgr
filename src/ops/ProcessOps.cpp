// Destructive process operations (arch section 6 execution protocol).
//
// Protocol (mandatory, see ops/ProcessOps.h):
//  1. Identity re-verify: OpenProcess -> GetProcessTimes -> createTime within +-1s.
//     Mismatch => refuse with "已退出或 PID 已被复用". Never trust pid alone.
//  2. ProtectedList is a hard gate inside ops. It is checked BEFORE any handle is
//     opened, so even non-elevated runs get an honest "protected" refusal instead of
//     an opaque access-denied (PPL processes deny PROCESS_TERMINATE to everyone).
//  3. Tree ops snapshot on their own (NtQSI, Toolhelp fallback; stm_collect is never
//     included), re-snapshot at execution time, walk leaf-first and skip protected
//     members one by one (reported, never silently dropped).
#include "ops/ProcessOps.h"
#include "core/Err.h"
#include "core/HandleGuard.h"
#include "core/Log.h"
#include "core/Privilege.h"
#include "core/ProtectedList.h"
#include "core/Str.h"
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstddef>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment(lib, "psapi")

namespace stm::ops {
namespace {

constexpr uint64_t kFileTime1Sec = 10'000'000ull;  // FILETIME unit is 100 ns
constexpr DWORD kExitWaitMs = 5000;

constexpr uint32_t kSystemProcessInformation = 5;  // SYSTEM_INFORMATION_CLASS (documented value)
constexpr uint32_t kStatusInfoLengthMismatch = 0xC0000004u;

bool SameCreateTime(uint64_t a, uint64_t b) {
    const uint64_t d = a > b ? a - b : b - a;
    return d <= kFileTime1Sec;  // +-1s tolerance, arch section 6.1
}

uint64_t FileTimeToU64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

uint32_t HandleToU32(HANDLE h) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(h));
}

// One process row of the ops-internal snapshot (self-contained; no stm_collect types).
struct ProcSnap {
    uint32_t pid = 0;
    uint32_t parentPid = 0;
    uint64_t createTime = 0;
    std::wstring name;
};

// x64 mirror of UNICODE_STRING (winternl.h is deliberately not included; layout is
// stable and asserted below).
struct NtUnicodeString {
    uint16_t length;        // bytes, excluding the NUL terminator
    uint16_t maximumLength;
    uint32_t pad;
    wchar_t* buffer;
};
static_assert(sizeof(NtUnicodeString) == 16);

// Documented x64 layout of SYSTEM_PROCESS_INFORMATION. winternl.h keeps the struct
// opaque; the fields below appear on the official NtQuerySystemInformation page
// (docs/research/R5 row 1). We only rely on documented members: create time, image
// name, parent pid.
struct SystemProcessEntry {
    uint32_t nextEntryOffset;               // +0x00
    uint32_t numberOfThreads;               // +0x04
    int64_t workingSetPrivateSize;          // +0x08
    uint32_t hardFaultCount;                // +0x10
    uint32_t numberOfThreadsHighWatermark;  // +0x14
    uint64_t cycleTime;                     // +0x18
    int64_t createTime;                     // +0x20
    int64_t userTime;                       // +0x28
    int64_t kernelTime;                     // +0x30
    NtUnicodeString imageName;              // +0x38
    int32_t basePriority;                   // +0x48
    HANDLE uniqueProcessId;                 // +0x50
    HANDLE inheritedFromUniqueProcessId;    // +0x58
    uint32_t handleCount;                   // +0x60
    uint32_t sessionId;                     // +0x64
    uint64_t pageDirectoryBase;             // +0x68
};
static_assert(offsetof(SystemProcessEntry, createTime) == 0x20);
static_assert(offsetof(SystemProcessEntry, imageName) == 0x38);
static_assert(offsetof(SystemProcessEntry, uniqueProcessId) == 0x50);
static_assert(offsetof(SystemProcessEntry, inheritedFromUniqueProcessId) == 0x58);
static_assert(sizeof(SystemProcessEntry) == 0x70);

// NtQSI snapshot with buffer-growth retry; falls back to the documented Toolhelp
// snapshot (createTime filled best-effort via GetProcessTimes) when the ntdll export
// is missing or the returned buffer does not walk consistently.
bool SnapshotProcesses(std::vector<ProcSnap>* out, std::wstring* err) {
    out->clear();
    using NtQuerySystemInformationFn = uint32_t (WINAPI*)(uint32_t, void*, uint32_t, uint32_t*);
    static const NtQuerySystemInformationFn ntQsi = []() -> NtQuerySystemInformationFn {
        const HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        if (!nt) return nullptr;
        return reinterpret_cast<NtQuerySystemInformationFn>(
            reinterpret_cast<void*>(GetProcAddress(nt, "NtQuerySystemInformation")));
    }();

    if (ntQsi) {
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
        bool walked = (status == 0);
        if (walked) {
            const uint8_t* p = buf.data();
            const uint8_t* const end = buf.data() + buf.size();
            while (p + sizeof(SystemProcessEntry) <= end) {
                const auto* e = reinterpret_cast<const SystemProcessEntry*>(p);
                if (e->nextEntryOffset != 0 && e->nextEntryOffset < sizeof(SystemProcessEntry)) {
                    walked = false;  // layout looks wrong on this OS build => fallback
                    break;
                }
                const uint32_t pid = HandleToU32(e->uniqueProcessId);
                if (pid != 0) {  // idle process is irrelevant for tree ops
                    ProcSnap s;
                    s.pid = pid;
                    s.parentPid = HandleToU32(e->inheritedFromUniqueProcessId);
                    s.createTime = static_cast<uint64_t>(e->createTime);
                    if (e->imageName.buffer && e->imageName.length >= sizeof(wchar_t)) {
                        s.name.assign(e->imageName.buffer,
                                      static_cast<size_t>(e->imageName.length) / sizeof(wchar_t));
                    }
                    out->push_back(std::move(s));
                }
                if (e->nextEntryOffset == 0) break;
                p += e->nextEntryOffset;
            }
        }
        if (walked && !out->empty()) return true;
        out->clear();
    }

    // Toolhelp fallback (documented; no createTime field => per-process GetProcessTimes).
    HANDLE raw = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (raw == INVALID_HANDLE_VALUE) {
        if (err) *err = ErrContext(L"进程快照失败", LastHr());
        return false;
    }
    UniqueHandle snap(raw);
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap.get(), &pe)) {
        if (err) *err = ErrContext(L"进程快照遍历失败", LastHr());
        return false;
    }
    do {
        ProcSnap s;
        s.pid = pe.th32ProcessID;
        s.parentPid = pe.th32ParentProcessID;
        s.name = pe.szExeFile;
        if (UniqueHandle h{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, s.pid)}; h) {
            FILETIME c{}, x{}, k{}, u{};
            if (GetProcessTimes(h.get(), &c, &x, &k, &u)) s.createTime = FileTimeToU64(c);
        }
        out->push_back(std::move(s));
    } while (Process32NextW(snap.get(), &pe));
    return true;
}

const ProcSnap* FindRow(const std::vector<ProcSnap>& snap, uint32_t pid) {
    for (const auto& s : snap) {
        if (s.pid == pid) return &s;
    }
    return nullptr;
}

// Pure tree walk (arch section 6.2): BFS over the snapshot buffer, output = descendant
// keys (root excluded). Parent-pid-reuse guards:
//  - the root row must exist and match createTime (<=1s), otherwise nothing is collected;
//  - a child is adopted only if it was created at/after its parent row (a genuine parent
//    always predates its children), so orphans left by a recycled pid are rejected.
void CollectTreeMembers(const std::vector<ProcSnap>& snap, const ProcKey& root,
                        std::vector<ProcKey>* out) {
    out->clear();
    std::unordered_map<uint32_t, size_t> byPid;
    byPid.reserve(snap.size());
    for (size_t i = 0; i < snap.size(); ++i) byPid.emplace(snap[i].pid, i);  // first row wins
    const auto rootIt = byPid.find(root.pid);
    if (rootIt == byPid.end()) return;
    if (!SameCreateTime(snap[rootIt->second].createTime, root.createTime)) return;
    std::vector<char> visited(snap.size(), 0);
    std::vector<size_t> frontier{rootIt->second};
    visited[rootIt->second] = 1;
    while (!frontier.empty()) {
        std::vector<size_t> next;
        for (const size_t pi : frontier) {
            const ProcSnap& parent = snap[pi];
            for (size_t i = 0; i < snap.size(); ++i) {
                if (visited[i]) continue;
                const ProcSnap& e = snap[i];
                if (e.parentPid != parent.pid || e.pid == parent.pid) continue;
                if (e.createTime + kFileTime1Sec < parent.createTime) continue;  // pre-reuse orphan
                visited[i] = 1;
                out->push_back(ProcKey{e.pid, e.createTime});
                next.push_back(i);
            }
        }
        frontier = std::move(next);
    }
}

enum class OpenVerdict { Opened, Gone, Denied };

// Identity re-verify + minimal-access open (protocol step 1).
// SeDebug policy: with seDebugHeld != null and an elevated caller, the privilege is
// enabled on the first access-denied and the caller owns releasing it (tree loops);
// with nullptr it is released before returning. core::EnablePrivilege logs both ways.
OpenVerdict OpenVerified(const ProcKey& key, DWORD access, bool* seDebugHeld, UniqueHandle* out,
                         std::wstring* err) {
    out->reset();
    HANDLE raw = OpenProcess(access, FALSE, key.pid);
    if (!raw) {
        const uint32_t hr = LastHr();
        if (hr == static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED))) {
            bool releaseHere = false;
            if (IsProcessElevated() && !(seDebugHeld && *seDebugHeld)) {
                std::wstring perr;
                if (EnablePrivilege(L"SeDebugPrivilege", true, &perr)) {
                    if (seDebugHeld) *seDebugHeld = true;
                    else releaseHere = true;
                    raw = OpenProcess(access, FALSE, key.pid);  // retry with SeDebugPrivilege
                } else {
                    STM_LOG_INFO("ops", L"SeDebugPrivilege 启用失败：{}", perr);
                }
            }
            if (releaseHere) EnablePrivilege(L"SeDebugPrivilege", false, nullptr);
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
            *err = L"目标进程已退出或 PID 已被复用（" + ErrContext(L"GetProcessTimes", LastHr()) + L"）";
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

// Terminate + bounded wait for exit confirmation. Handle must carry SYNCHRONIZE.
bool TerminateVerified(const UniqueHandle& h, std::wstring* err) {
    if (!TerminateProcess(h.get(), 1)) {
        const uint32_t hr = LastHr();  // capture before the grace wait clobbers it
        // Grace: the target may already be terminating (e.g. its tree neighbour died and
        // teardown beat us to it; TerminateProcess then reports ERROR_ACCESS_DENIED even
        // on a valid handle). A confirmed imminent exit is still a success.
        if (WaitForSingleObject(h.get(), 500) == WAIT_OBJECT_0) return true;
        if (err) *err = ErrContext(L"终止进程失败", hr);
        return false;
    }
    if (WaitForSingleObject(h.get(), kExitWaitMs) != WAIT_OBJECT_0) {
        if (err) *err = L"终止请求已发出，但进程 5 秒内仍未退出";
        return false;
    }
    return true;
}

std::wstring ImageNameFromPath(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring QueryImagePath(uint32_t pid) {
    UniqueHandle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!h) return {};
    wchar_t buf[1024]{};
    DWORD len = 1024;
    if (!QueryFullProcessImageNameW(h.get(), 0, buf, &len)) return {};
    return std::wstring(buf, len);
}

std::wstring FindImageNameBySnapshot(uint32_t pid) {
    HANDLE raw = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (raw == INVALID_HANDLE_VALUE) return {};
    UniqueHandle snap(raw);
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap.get(), &pe)) return {};
    do {
        if (pe.th32ProcessID == pid) return pe.szExeFile;
    } while (Process32NextW(snap.get(), &pe));
    return {};
}

}  // namespace

bool TerminateProcessById(const ProcKey& key, std::wstring* err) {
    // Hard gate first: refuse before touching the process (arch section 6.2).
    std::wstring reason = ProtectedReason(key, L"", L"");
    if (!reason.empty()) {
        if (err) *err = L"已拒绝终止：" + reason + L"（保护名单强制拦截）";
        STM_LOG_INFO("ops", L"终止请求被保护名单拦截 pid={}", key.pid);
        return false;
    }
    UniqueHandle h;
    const OpenVerdict v = OpenVerified(key,
                                       PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                       nullptr, &h, err);
    if (v != OpenVerdict::Opened) return false;
    return TerminateVerified(h, err);
}

bool PlanTerminateTree(const ProcKey& root, std::vector<ProcKey>* out, std::wstring* err) {
    if (out) out->clear();
    std::vector<ProcSnap> snap;
    if (!SnapshotProcesses(&snap, err)) return false;
    const ProcSnap* row = FindRow(snap, root.pid);
    if (!row || !SameCreateTime(row->createTime, root.createTime)) {
        if (err) *err = L"目标进程已退出或 PID 已被复用";
        return false;
    }
    std::vector<ProcKey> kids;  // descendants only; the root is implied
    CollectTreeMembers(snap, root, &kids);
    if (out) *out = std::move(kids);
    return true;
}

bool TerminateTree(const ProcKey& root, TreeResult* out, std::wstring* err) {
    TreeResult r;
    std::vector<ProcSnap> snap;
    if (!SnapshotProcesses(&snap, err)) return false;
    const ProcSnap* rootRow = FindRow(snap, root.pid);
    if (!rootRow || !SameCreateTime(rootRow->createTime, root.createTime)) {
        if (err) *err = L"目标进程已退出或 PID 已被复用";
        return false;
    }
    std::vector<ProcKey> kids;
    CollectTreeMembers(snap, root, &kids);
    r.planned = static_cast<int>(kids.size()) + 1;  // descendants + root

    // CollectTreeMembers emits BFS level order => reversed it is leaf-first; the root
    // is appended last so it dies after all its descendants (prevents re-parenting races).
    std::vector<ProcKey> order = kids;
    std::reverse(order.begin(), order.end());
    order.push_back(root);

    bool seDebugHeld = false;
    for (const ProcKey& k : order) {
        // Member-level protection gate: skip + count, never refuse the whole tree.
        const ProcSnap* row = FindRow(snap, k.pid);
        if (row) {
            const std::wstring reason = stm::ProtectedReason(row->pid, row->name, L"");
            if (!reason.empty()) {
                ++r.skippedProtected;
                STM_LOG_WARN("ops", L"树杀跳过受保护成员 pid={}：{}", row->pid, reason);
                continue;
            }
        }
        std::wstring e;
        UniqueHandle h;
        const OpenVerdict v = OpenVerified(k,
                                           PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                           &seDebugHeld, &h, &e);
        if (v == OpenVerdict::Gone) continue;  // exited between snapshot and act: benign
        if (v != OpenVerdict::Opened) {
            ++r.failed;
            if (r.firstError.empty()) r.firstError = e;
            continue;
        }
        if (!TerminateVerified(h, &e)) {
            ++r.failed;
            if (r.firstError.empty()) r.firstError = e;
            continue;
        }
        ++r.terminated;
    }
    if (seDebugHeld) EnablePrivilege(L"SeDebugPrivilege", false, nullptr);  // release after use
    if (out) *out = r;
    if (err) *err = r.failed ? r.firstError : std::wstring();
    return r.failed == 0;
}

bool TrimWorkingSet(const ProcKey& key, std::wstring* err) {
    UniqueHandle h;
    bool seDebugHeld = false;
    const OpenVerdict v = OpenVerified(key,
                                       PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION,
                                       &seDebugHeld, &h, err);
    if (seDebugHeld) EnablePrivilege(L"SeDebugPrivilege", false, nullptr);
    if (v != OpenVerdict::Opened) return false;
    if (!EmptyWorkingSet(h.get())) {
        if (err) *err = ErrContext(L"释放工作集失败", LastHr());
        return false;
    }
    return true;
}

bool PurgeStandbyList(std::wstring* err) {
    std::wstring perr;
    if (!EnablePrivilege(L"SeProfileSingleProcessPrivilege", true, &perr)) {
        if (err) *err = L"清理待机列表需要管理员权限（" + perr + L"）";
        return false;
    }
    struct ReleaseOnExit {  // enable-and-release around the single call
        ~ReleaseOnExit() { EnablePrivilege(L"SeProfileSingleProcessPrivilege", false, nullptr); }
    } guard;
    using NtSetSystemInformationFn = int32_t (WINAPI*)(uint32_t, void*, uint32_t);
    const HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    const auto setInfo = nt ? reinterpret_cast<NtSetSystemInformationFn>(
                                  reinterpret_cast<void*>(GetProcAddress(nt, "NtSetSystemInformation")))
                            : nullptr;
    if (!setInfo) {
        if (err) *err = L"系统不支持清理待机列表（缺少 NtSetSystemInformation）";
        return false;
    }
    uint32_t command = 4;  // MemoryPurgeStandbyList
    const int32_t status = setInfo(0x50 /*SystemMemoryListInformation*/, &command, sizeof(command));
    if (status != 0) {
        if (err) {
            *err = status == static_cast<int32_t>(0xC0000061u)  // STATUS_PRIVILEGE_NOT_HELD
                       ? std::wstring(L"清理待机列表失败：需要管理员权限")
                       : Fmt(L"清理待机列表失败（NTSTATUS 0x{:08X}）", static_cast<uint32_t>(status));
        }
        STM_LOG_WARN("ops", L"NtSetSystemInformation(SystemMemoryListInformation) 失败 status=0x{:08X}",
                     static_cast<uint32_t>(status));
        return false;
    }
    // Honest semantics (contract): this frees system cache pages only, not process memory.
    STM_LOG_INFO("ops", L"待机列表已清理（仅释放系统缓存，不回收进程内存）");
    return true;
}

std::wstring ProtectedReason(const ProcKey& key, const std::wstring& name, const std::wstring& path) {
    if (key.pid <= 4) return stm::ProtectedReason(key.pid, name, path);  // idle/System need no name
    if (!name.empty()) return stm::ProtectedReason(key.pid, name, path);
    // No name provided by the caller: resolve it on demand (handle path first, then a
    // Toolhelp scan), then run the list check with the resolved identity.
    const std::wstring resolvedPath = QueryImagePath(key.pid);
    const std::wstring resolvedName =
        resolvedPath.empty() ? FindImageNameBySnapshot(key.pid) : ImageNameFromPath(resolvedPath);
    return stm::ProtectedReason(key.pid, resolvedName,
                                resolvedPath.empty() ? path : resolvedPath);
}

}  // namespace stm::ops
