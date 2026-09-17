// On-demand process details (arch section 8). The frozen header fixes the API surface:
// UI thread calls Request/Peek; the single ops worker (JobQueue) does the slow fetching
// and posts a notification. All Win32 handles are RAII (core::UniqueHandle); every cache
// write goes through the same mutex the UI reads under. The signature check reuses
// ops::VerifyFileSignature, whose result cache also dedupes repeated paths, and only
// fixed-disk paths reach WinVerifyTrust (network paths would stall the job queue).
#include "ops/DetailsProvider.h"
#include "core/HandleGuard.h"
#include "core/Log.h"
#include <windows.h>
#include <psapi.h>
#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#pragma comment(lib, "psapi")

namespace stm::ops {
namespace {

// ---- dynamic ntdll (documented export, loaded on demand) ----
using NtQueryInformationProcessFn = int32_t (WINAPI*)(HANDLE, uint32_t, void*, uint32_t, uint32_t*);
constexpr uint32_t kProcessBasicInformation = 0;

NtQueryInformationProcessFn LoadNtQueryInformationProcess() {
    static const NtQueryInformationProcessFn fn = []() -> NtQueryInformationProcessFn {
        const HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        if (!nt) return nullptr;
        return reinterpret_cast<NtQueryInformationProcessFn>(
            reinterpret_cast<void*>(GetProcAddress(nt, "NtQueryInformationProcess")));
    }();
    return fn;
}

// x64 mirror of PROCESS_BASIC_INFORMATION (PebBaseAddress at +0x08; layout stable and
// verified by the static asserts; winternl.h is intentionally not included here).
struct ProcessBasicInfoBlock {
    int32_t exitStatus;        // +0x00
    uint32_t pad0;             // +0x04
    void* pebBaseAddress;      // +0x08
    void* reserved2[2];        // +0x10
    ULONG_PTR uniqueProcessId; // +0x20
    void* reserved3;           // +0x28
};
static_assert(sizeof(ProcessBasicInfoBlock) == 0x30);
static_assert(offsetof(ProcessBasicInfoBlock, pebBaseAddress) == 0x08);

// ---- single-shot fetchers; each records its outcome in ProcessDetails ----

// Command line via PEB -> RTL_USER_PROCESS_PARAMETERS (documented x64 offsets). Any
// failure leaves cmdLineAvail=false; the attempt is final and never retried.
void FetchCmdLine(HANDLE h, ProcessDetails* d) {
    d->cmdLineAvail = false;  // final state either way (single attempt by design)
    const auto nqip = LoadNtQueryInformationProcess();
    if (!nqip) return;
    ProcessBasicInfoBlock pbi{};
    if (nqip(h, kProcessBasicInformation, &pbi, sizeof(pbi), nullptr) != 0 || !pbi.pebBaseAddress) {
        return;
    }
    SIZE_T got = 0;
    ULONGLONG ppAddr = 0;
    const auto* peb = static_cast<const uint8_t*>(pbi.pebBaseAddress);
    // PEB+0x20 holds ProcessParameters (x64 PEB layout).
    if (!ReadProcessMemory(h, peb + 0x20, &ppAddr, sizeof(ppAddr), &got) || ppAddr == 0) {
        return;
    }
    ULONGLONG paramsHead[16]{};  // RTL_USER_PROCESS_PARAMETERS: CommandLine at +0x70
    if (!ReadProcessMemory(h, reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(ppAddr)), paramsHead,
                           sizeof(paramsHead), &got)) {
        return;
    }
    const uint32_t cmdBytes = static_cast<uint32_t>(paramsHead[0x70 / sizeof(ULONGLONG)] & 0xFFFFu);
    const ULONGLONG cmdBuffer = paramsHead[0x78 / sizeof(ULONGLONG)];
    if (cmdBytes == 0) {  // legitimately empty command line, fetch succeeded
        d->cmdLine.clear();
        d->cmdLineAvail = true;
        return;
    }
    if (cmdBuffer == 0 || cmdBytes > 32 * 1024) return;
    std::wstring cmd(cmdBytes / sizeof(wchar_t), L'\0');
    if (!ReadProcessMemory(h, reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(cmdBuffer)),
                           cmd.data(), cmdBytes, &got)) {
        return;
    }
    while (!cmd.empty() && cmd.back() == L'\0') cmd.pop_back();
    d->cmdLine = std::move(cmd);
    d->cmdLineAvail = true;
}

// Owning user via process token (works with a query-limited handle too).
void FetchUserName(HANDLE h, ProcessDetails* d) {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(h, TOKEN_QUERY, &rawToken)) return;
    const UniqueHandle token(rawToken);
    DWORD len = 0;
    if (!GetTokenInformation(token.get(), TokenUser, nullptr, 0, &len) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return;
    }
    if (len == 0) return;
    std::vector<uint8_t> buf(len);
    if (!GetTokenInformation(token.get(), TokenUser, buf.data(), len, &len)) return;
    const auto* tu = reinterpret_cast<const TOKEN_USER*>(buf.data());
    if (!tu->User.Sid) return;
    wchar_t user[256]{};
    wchar_t domain[256]{};
    DWORD userLen = 256;
    DWORD domainLen = 256;
    SID_NAME_USE use = SidTypeUnknown;
    if (!LookupAccountSidW(nullptr, tu->User.Sid, user, &userLen, domain, &domainLen, &use)) return;
    d->userName = domain[0] ? std::wstring(domain) + L"\\" + user : std::wstring(user);
    d->userNameResolved = true;
}

// GDI/USER object counts (needs PROCESS_QUERY_INFORMATION; 0 is a legitimate value).
void FetchGuiObjects(HANDLE h, ProcessDetails* d) {
    d->gdiObjects = static_cast<uint32_t>(GetGuiResources(h, GR_GDIOBJECTS));
    d->userObjects = static_cast<uint32_t>(GetGuiResources(h, GR_USEROBJECTS));
    d->guiResolved = true;
}

// Module list (needs VM_READ). On failure the list stays empty and modulesResolved
// stays false, so a later Request (e.g. after elevation) retries honestly.
void FetchModules(HANDLE h, ProcessDetails* d) {
    DWORD needed = 0;
    if (!EnumProcessModulesEx(h, nullptr, 0, &needed, LIST_MODULES_ALL) || needed == 0) {
        needed = 512 * sizeof(HMODULE);  // size-query quirk fallback
    }
    std::vector<HMODULE> mods(needed / sizeof(HMODULE));
    DWORD written = 0;
    if (!EnumProcessModulesEx(h, mods.data(), needed, &written, LIST_MODULES_ALL)) return;
    mods.resize(written / sizeof(HMODULE));
    d->modules.clear();
    for (const HMODULE m : mods) {
        wchar_t path[1024]{};
        if (GetModuleFileNameExW(h, m, path, 1024) > 0) d->modules.push_back(path);
    }
    if (!d->modules.empty()) d->modulesResolved = true;
}

// Fixed-disk check: only local drives may reach WinVerifyTrust (network timeouts would
// occupy the serial job queue, arch section 5 guardrail).
bool IsFixedDrivePath(const std::wstring& path) {
    std::wstring root;
    if (path.size() >= 2 && path[1] == L':') {
        root = path.substr(0, 3);
    } else {
        root = path;
    }
    return GetDriveTypeW(root.c_str()) == DRIVE_FIXED;
}

// The frozen DetailsProvider::Entry has no per-kind "attempted" flag and the header must
// not grow; single-shot bookkeeping for CmdLine is kept in this provider-keyed side map.
constexpr uint32_t kAttemptedCmdLine = 1u << 0;
std::mutex g_attemptMu;
std::map<std::pair<const void*, ProcKey>, uint32_t> g_attemptedKinds;

bool CmdLineAttempted(const void* self, const ProcKey& key) {
    std::lock_guard<std::mutex> lock(g_attemptMu);
    const auto it = g_attemptedKinds.find({self, key});
    return it != g_attemptedKinds.end() && (it->second & kAttemptedCmdLine) != 0;
}

void MarkCmdLineAttempted(const void* self, const ProcKey& key) {
    std::lock_guard<std::mutex> lock(g_attemptMu);
    g_attemptedKinds[{self, key}] |= kAttemptedCmdLine;
}

void DropAttemptRecord(const void* self, const ProcKey& key) {
    std::lock_guard<std::mutex> lock(g_attemptMu);
    g_attemptedKinds.erase({self, key});
}

}  // namespace

void DetailsProvider::Request(const ProcKey& key, const std::wstring& path, uint32_t kinds) {
    if (kinds == 0) return;

    uint32_t todo = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        Entry* e = BeginEntry(key);  // creates the entry when absent
        if (e->pending) return;      // a fetch for this process is already in flight
        if ((kinds & static_cast<uint32_t>(DetailKind::Signature)) != 0 && !e->data.sigResolved)
            todo |= static_cast<uint32_t>(DetailKind::Signature);
        if ((kinds & static_cast<uint32_t>(DetailKind::CmdLine)) != 0 && !e->data.cmdLineAvail &&
            !CmdLineAttempted(this, key))
            todo |= static_cast<uint32_t>(DetailKind::CmdLine);
        if ((kinds & static_cast<uint32_t>(DetailKind::UserInfo)) != 0 && !e->data.userNameResolved)
            todo |= static_cast<uint32_t>(DetailKind::UserInfo);
        if ((kinds & static_cast<uint32_t>(DetailKind::GuiObjects)) != 0 && !e->data.guiResolved)
            todo |= static_cast<uint32_t>(DetailKind::GuiObjects);
        if ((kinds & static_cast<uint32_t>(DetailKind::Modules)) != 0 && !e->data.modulesResolved)
            todo |= static_cast<uint32_t>(DetailKind::Modules);
        if (todo == 0) return;
        if ((todo & static_cast<uint32_t>(DetailKind::CmdLine)) != 0) MarkCmdLineAttempted(this, key);
        e->pending = true;  // mark before submitting so re-entrant Requests never double-submit
    }

    const auto seqHolder = std::make_shared<std::atomic<uint64_t>>(0);
    const uint64_t seq = jobs_.Submit([this, key, path, todo, seqHolder] {
        ProcessDetails d;
        {
            std::lock_guard<std::mutex> lock(mu_);
            const auto it = cache_.find(key);
            if (it == cache_.end()) return;  // invalidated while queued
            d = it->second.data;             // merge base: keep fields owned by other kinds
        }

        // Tiered open: full access first (VM_READ needed by cmdLine/modules/GUI counts),
        // then query-limited (still enough for the token/user lookup on same-user targets).
        UniqueHandle handle;
        bool fullAccess = false;
        HANDLE raw = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, key.pid);
        if (raw) {
            handle.reset(raw);
            fullAccess = true;
        } else {
            raw = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, key.pid);
            if (raw) handle.reset(raw);
        }

        bool sigSkippedNetwork = false;
        if ((todo & static_cast<uint32_t>(DetailKind::Signature)) != 0) {
            if (IsFixedDrivePath(path)) {
                d.sig = VerifyFileSignature(path);  // signature result cache dedupes the path
            } else {
                d.sig = SigState::NoCheck;  // honest: not checked, reason shown in UI text
                sigSkippedNetwork = true;
                STM_LOG_INFO("ops", L"非固定磁盘路径，跳过签名校验");
            }
            d.sigResolved = true;
        }
        if (handle) {
            if ((todo & static_cast<uint32_t>(DetailKind::CmdLine)) != 0) {
                FetchCmdLine(handle.get(), &d);
            }
            if ((todo & static_cast<uint32_t>(DetailKind::UserInfo)) != 0) {
                FetchUserName(handle.get(), &d);
            }
            if (fullAccess && (todo & static_cast<uint32_t>(DetailKind::GuiObjects)) != 0) {
                FetchGuiObjects(handle.get(), &d);
            }
            if (fullAccess && (todo & static_cast<uint32_t>(DetailKind::Modules)) != 0) {
                FetchModules(handle.get(), &d);
            }
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            if (const auto it = cache_.find(key); it != cache_.end()) {
                it->second.data = d;
                it->second.pending = false;
            }
        }

        Notification n;
        n.seq = seqHolder->load();
        if (handle) {
            n.kind = Notification::Kind::JobDone;
            n.text = sigSkippedNetwork ? L"进程详情已更新（非本地路径未校验签名）" : L"进程详情已更新";
        } else if (todo == static_cast<uint32_t>(DetailKind::Signature)) {
            n.kind = Notification::Kind::JobDone;  // signature needs no process handle
            n.text = L"进程签名已更新";
        } else {
            n.kind = Notification::Kind::JobFailed;
            n.text = L"无法读取进程详情：进程可能已退出或权限不足";
        }
        notes_.Push(n);
    });
    seqHolder->store(seq);

    if (seq == 0) {
        // Queue not running: roll back so a later Request can retry.
        DropAttemptRecord(this, key);
        std::lock_guard<std::mutex> lock(mu_);
        if (const auto it = cache_.find(key); it != cache_.end()) it->second.pending = false;
        STM_LOG_WARN("ops", L"任务队列未运行，进程详情请求被丢弃");
    }
}

const ProcessDetails* DetailsProvider::Peek(const ProcKey& key) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = cache_.find(key);
    return it == cache_.end() ? nullptr : &it->second.data;
}

void DetailsProvider::Invalidate(const ProcKey& key) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        cache_.erase(key);
    }
    DropAttemptRecord(this, key);
}

bool DetailsProvider::TryGetGuiObjects(const ProcKey& key, uint32_t* gdi, uint32_t* user) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = cache_.find(key);
    if (it == cache_.end() || !it->second.data.guiResolved) return false;
    if (gdi) *gdi = it->second.data.gdiObjects;
    if (user) *user = it->second.data.userObjects;
    return true;
}

DetailsProvider::Entry* DetailsProvider::BeginEntry(const ProcKey& key) {
    return &cache_[key];  // caller must hold mu_ (locked contract)
}

}  // namespace stm::ops
