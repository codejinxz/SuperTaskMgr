// 破坏性进程操作（架构第 6 节执行协议）。
//
// 协议（强制，见 ops/ProcessOps.h）：
//  1. 身份复核：OpenProcess -> GetProcessTimes -> createTime 相差 +-1s 内。
//     Mismatch => refuse with "已退出或 PID 已被复用". Never trust pid alone.
//  2. ProtectedList 是 ops 内的硬闸门。在任何句柄打开之前检查，
//     因此连非提权运行也会得到诚实的"受保护"拒绝，而不是
//     晦涩的拒绝访问（PPL 进程对所有人都拒绝 PROCESS_TERMINATE）。
//  3. 树操作自行快照（NtQSI，Toolhelp 兜底；绝不包含 stm_collect），
//     执行时再快照一次，先叶子后根遍历，并逐个跳过受保护成员
//（逐个上报，绝不静默丢弃）。
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
#include <cwchar>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment(lib, "psapi")

namespace stm::ops {
namespace {

constexpr uint64_t kFileTime1Sec = 10'000'000ull;  // FILETIME 单位为 100 ns
constexpr DWORD kExitWaitMs = 5000;

constexpr uint32_t kSystemProcessInformation = 5;  // SYSTEM_INFORMATION_CLASS（有文档的值）
constexpr uint32_t kStatusInfoLengthMismatch = 0xC0000004u;

bool SameCreateTime(uint64_t a, uint64_t b) {
    const uint64_t d = a > b ? a - b : b - a;
    return d <= kFileTime1Sec;  // +-1s 容差，架构第 6.1 节
}

uint64_t FileTimeToU64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

uint32_t HandleToU32(HANDLE h) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(h));
}

// ops 内部快照的单进程行（自包含；不含 stm_collect 类型）。
struct ProcSnap {
    uint32_t pid = 0;
    uint32_t parentPid = 0;
    uint64_t createTime = 0;
    std::wstring name;
};

// UNICODE_STRING 的 x64 镜像（刻意不包含 winternl.h；布局
// 稳定并在下方断言）。
struct NtUnicodeString {
    uint16_t length;        // 字节数，不含 NUL 终止符
    uint16_t maximumLength;
    uint32_t pad;
    wchar_t* buffer;
};
static_assert(sizeof(NtUnicodeString) == 16);

// SYSTEM_PROCESS_INFORMATION 的有文档 x64 布局。winternl.h 把该结构
// 视为不透明；下方字段来自官方 NtQuerySystemInformation 页面
//（docs/research/R5 第 1 行）。我们只依赖有文档的成员：创建时间、映像
// 名、父 pid。
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

// 带缓冲增长重试的 NtQSI 快照；当 ntdll 导出缺失或返回缓冲
// 无法一致遍历时，回退到有文档的 Toolhelp 快照
//（createTime 经 GetProcessTimes 尽力填充）。
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
                    walked = false;  // 该 OS 构建上布局可疑 => 走兜底
                    break;
                }
                const uint32_t pid = HandleToU32(e->uniqueProcessId);
                if (pid != 0) {  // 空闲进程与树操作无关
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

    // Toolhelp 兜底（有文档；无 createTime 字段 => 按进程调 GetProcessTimes）。
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

// 纯树遍历（架构第 6.2 节）：在快照缓冲上 BFS，输出=后代键
//（不含根）。父 pid 复用防护：
//  - 根行必须存在且 createTime 匹配（<=1s），否则不收集任何内容；
//  - 只有创建时间不早于父行的子进程才被收编（真正的父进程
//    必然先于子进程存在），从而拒绝 pid 复用留下的孤儿。
void CollectTreeMembers(const std::vector<ProcSnap>& snap, const ProcKey& root,
                        std::vector<ProcKey>* out) {
    out->clear();
    std::unordered_map<uint32_t, size_t> byPid;
    byPid.reserve(snap.size());
    for (size_t i = 0; i < snap.size(); ++i) byPid.emplace(snap[i].pid, i);  // 首行优先
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
                if (e.createTime + kFileTime1Sec < parent.createTime) continue;  // 复用前遗留的孤儿
                visited[i] = 1;
                out->push_back(ProcKey{e.pid, e.createTime});
                next.push_back(i);
            }
        }
        frontier = std::move(next);
    }
}

enum class OpenVerdict { Opened, Gone, Denied };

// 身份复核 + 最小权限打开（协议第 1 步）。
// SeDebug 策略：seDebugHeld != null 且调用方已提权时，特权在
// 首次拒绝访问时启用，由调用方负责释放（树循环场景）；
// 传 nullptr 则在返回前释放。core::EnablePrivilege 两种方式都记日志。
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
                    raw = OpenProcess(access, FALSE, key.pid);  // 带 SeDebugPrivilege 重试
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

// 终止 + 有界等待退出确认。句柄必须带 SYNCHRONIZE。
bool TerminateVerified(const UniqueHandle& h, std::wstring* err) {
    if (!TerminateProcess(h.get(), 1)) {
        const uint32_t hr = LastHr();  // 在宽限等待破坏它之前先捕获
        // 宽限：目标可能已在终止中（例如同树邻居已死、
        // 清理先我们一步；此时 TerminateProcess 即使在有效句柄上
        // 也会报 ERROR_ACCESS_DENIED）。确认即将退出仍算成功。
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

// `path` 是否位于 Windows 目录内（大小写不敏感前缀匹配）。
// 失败时保守处理：无法确定的系统根目录视为"在内"。
bool IsUnderSystemRoot(const std::wstring& path) {
    wchar_t winDir[MAX_PATH]{};
    UINT n = GetSystemWindowsDirectoryW(winDir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) n = GetWindowsDirectoryW(winDir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return true;
    if (path.size() < n) return false;
    if (_wcsnicmp(path.c_str(), winDir, n) != 0) return false;
    const wchar_t next = n < path.size() ? path[n] : L'\0';
    return next == L'\0' || next == L'\\' || next == L'/';
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
    // 先过硬闸门：碰进程之前就拒绝（架构第 6.2 节）。
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
    std::vector<ProcKey> kids;  // 只有后代；根已隐含
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
    r.planned = static_cast<int>(kids.size()) + 1;  // 后代 + 根

    // CollectTreeMembers 按 BFS 层序输出 => 反转后即先叶子；根
    // 最后追加，使其在所有后代之后死掉（防止重新挂父的竞争）。
    std::vector<ProcKey> order = kids;
    std::reverse(order.begin(), order.end());
    order.push_back(root);

    bool seDebugHeld = false;
    for (const ProcKey& k : order) {
        // 成员级保护闸门：跳过并计数，绝不整体拒绝树操作。
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
        if (v == OpenVerdict::Gone) continue;  // 快照与执行之间退出：良性
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
    if (seDebugHeld) EnablePrivilege(L"SeDebugPrivilege", false, nullptr);  // 用后释放
    if (out) *out = r;
    if (err) *err = r.failed ? r.firstError : std::wstring();
    return r.failed == 0;
}

bool TrimWorkingSet(const ProcKey& key, std::wstring* err) {
    // 硬闸门：保护名单同样适用于 EmptyWorkingSet（V8-P1-1）——
    // 清空关键进程的工作集等于拒绝服务，因此碰进程之前先拒绝，
    // 与 TerminateProcessById 一致。
    const std::wstring reason = ProtectedReason(key, L"", L"");
    if (!reason.empty()) {
        if (err) *err = L"已拒绝释放工作集：" + reason + L"（保护名单强制拦截）";
        STM_LOG_INFO("ops", L"工作集释放被保护名单拦截 pid={}", key.pid);
        return false;
    }
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
    struct ReleaseOnExit {  // 围绕单次调用启用并释放
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
    uint32_t command = 4;  // MemoryPurgeStandbyList（清空待备列表）
    const int32_t status = setInfo(0x50 /*SystemMemoryListInformation*/, &command, sizeof(command));
    if (status != 0) {
        if (err) {
            *err = status == static_cast<int32_t>(0xC0000061u)  // STATUS_PRIVILEGE_NOT_HELD（未持有特权）
                       ? std::wstring(L"清理待机列表失败：需要管理员权限")
                       : Fmt(L"清理待机列表失败（NTSTATUS 0x{:08X}）", static_cast<uint32_t>(status));
        }
        STM_LOG_WARN("ops", L"NtSetSystemInformation(SystemMemoryListInformation) 失败 status=0x{:08X}",
                     static_cast<uint32_t>(status));
        return false;
    }
    // 诚实语义（契约）：只释放系统缓存页，不释放进程内存。
    STM_LOG_INFO("ops", L"待机列表已清理（仅释放系统缓存，不回收进程内存）");
    return true;
}

std::wstring ProtectedReason(const ProcKey& key, const std::wstring& name, const std::wstring& path) {
    // pid 0/4 由核心名单裁定，无论名称/路径如何都受保护。
    if (key.pid <= 4) return stm::ProtectedReason(key.pid, name, path);
    // 调用方只有 ProcKey 时按需解析身份（先走句柄路径，
    // 再走 Toolhelp 扫描）。
    std::wstring useName = name;
    std::wstring usePath = path;
    if (useName.empty()) {
        usePath = QueryImagePath(key.pid);
        useName = usePath.empty() ? FindImageNameBySnapshot(key.pid) : ImageNameFromPath(usePath);
    }
    const std::wstring reason = stm::ProtectedReason(key.pid, useName, usePath);
    if (reason.empty()) return {};
    // V8-P2 加固：仅凭映像名匹配不够。当映像路径已知且明显
    // 在 Windows 目录之外时，该进程只是与某个系统进程同名
    //（同名伪装），并不受保护。路径未知时仍按
    // 受保护处理（保守；如树成员只有快照名称）。
    if (!usePath.empty() && !IsUnderSystemRoot(usePath)) return {};
    return reason;
}

}  // namespace stm::ops
