// 按需进程详情（架构第 8 节）。冻结的头文件固定了 API 表面：
// UI 线程调用 Request/Peek；唯一的 ops 工作线程（JobQueue）做慢速抓取
// 并投递通知。所有 Win32 句柄均为 RAII（core::UniqueHandle）；每次缓存
// 写入都经过 UI 读取时所用的同一把互斥锁。签名检查复用
// ops::VerifyFileSignature，其结果缓存同时对重复路径去重，且只有
// 固定磁盘路径会进入 WinVerifyTrust（网络路径会卡死任务队列）。
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

// ---- 动态 ntdll（有文档的导出函数，按需加载）----
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

// PROCESS_BASIC_INFORMATION 的 x64 镜像（PebBaseAddress 在 +0x08；布局稳定，
// 且由静态断言验证；这里刻意不包含 winternl.h）。
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

// ---- 单次抓取器；各自把结果记录进 ProcessDetails ----

// 经 PEB -> RTL_USER_PROCESS_PARAMETERS 取命令行（有文档的 x64 偏移）。
// 任何失败都令 cmdLineAvail=false；尝试是一次性的，绝不重试。
void FetchCmdLine(HANDLE h, ProcessDetails* d) {
    d->cmdLineAvail = false;  // 无论成败都是最终状态（设计上只尝试一次）
    const auto nqip = LoadNtQueryInformationProcess();
    if (!nqip) return;
    ProcessBasicInfoBlock pbi{};
    if (nqip(h, kProcessBasicInformation, &pbi, sizeof(pbi), nullptr) != 0 || !pbi.pebBaseAddress) {
        return;
    }
    SIZE_T got = 0;
    ULONGLONG ppAddr = 0;
    const auto* peb = static_cast<const uint8_t*>(pbi.pebBaseAddress);
    // PEB+0x20 存放 ProcessParameters（x64 PEB 布局）。
    if (!ReadProcessMemory(h, peb + 0x20, &ppAddr, sizeof(ppAddr), &got) || ppAddr == 0) {
        return;
    }
    ULONGLONG paramsHead[16]{};  // RTL_USER_PROCESS_PARAMETERS：CommandLine 在 +0x70
    if (!ReadProcessMemory(h, reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(ppAddr)), paramsHead,
                           sizeof(paramsHead), &got)) {
        return;
    }
    const uint32_t cmdBytes = static_cast<uint32_t>(paramsHead[0x70 / sizeof(ULONGLONG)] & 0xFFFFu);
    const ULONGLONG cmdBuffer = paramsHead[0x78 / sizeof(ULONGLONG)];
    if (cmdBytes == 0) {  // 命令行确实为空，抓取成功
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

// 经进程令牌取所属用户（查询受限句柄也可用）。
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

// GDI/USER 对象计数（需要 PROCESS_QUERY_INFORMATION；0 是合法值）。
void FetchGuiObjects(HANDLE h, ProcessDetails* d) {
    d->gdiObjects = static_cast<uint32_t>(GetGuiResources(h, GR_GDIOBJECTS));
    d->userObjects = static_cast<uint32_t>(GetGuiResources(h, GR_USEROBJECTS));
    d->guiResolved = true;
}

// 模块列表（需要 VM_READ）。失败时列表保持空且 modulesResolved
// 保持 false，之后的 Request（如提权后）会诚实地重试。
void FetchModules(HANDLE h, ProcessDetails* d) {
    DWORD needed = 0;
    if (!EnumProcessModulesEx(h, nullptr, 0, &needed, LIST_MODULES_ALL) || needed == 0) {
        needed = 512 * sizeof(HMODULE);  // 大小查询怪癖的兜底
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

// 固定磁盘检查：只有本地盘可进入 WinVerifyTrust（网络超时会
// 占住串行任务队列，架构第 5 节的护栏）。
bool IsFixedDrivePath(const std::wstring& path) {
    std::wstring root;
    if (path.size() >= 2 && path[1] == L':') {
        root = path.substr(0, 3);
    } else {
        root = path;
    }
    return GetDriveTypeW(root.c_str()) == DRIVE_FIXED;
}

// 冻结的 DetailsProvider::Entry 没有按类别的"已尝试"标志，头文件也不能
// 变大；CmdLine 的一次性簿记保存在这个以提供方为键的旁路映射里。
constexpr uint32_t kAttemptedCmdLine = 1u << 0;
// V7-P1-2：每进程缓存与旁路映射都必须有界。缓存超过
// kDetailsCacheCap 时，丢弃所有非 pending 条目，并同步修剪
// 同一键集的已尝试记录（pending 条目保留：它们的在途任务
// 会经 cache_.find 写回，那里已容忍条目缺失）。
constexpr size_t kDetailsCacheCap = 1024;
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
        // V7-P1-2 上界：Invalidate() 已存在但还没有调用方。UI 的集成点是
        // 每次快照后对从 procs 消失的 ProcKey（差集）逐个 Invalidate()，
        // 让选择抖动自然保持在上限之下。在那之前，
        // 超容量时在这里丢弃全部非 pending 条目，并精确按被丢弃的
        // 键集修剪已尝试记录（旁路映射镜像缓存）。
        if (cache_.size() > kDetailsCacheCap) {
            std::vector<ProcKey> dropped;
            for (auto it = cache_.begin(); it != cache_.end();) {
                if (it->second.pending) {
                    ++it;
                    continue;
                }
                dropped.push_back(it->first);
                it = cache_.erase(it);
            }
            if (!dropped.empty()) {
                std::lock_guard<std::mutex> sideLock(g_attemptMu);
                for (const ProcKey& k : dropped) g_attemptedKinds.erase({this, k});
            }
        }
        Entry* e = BeginEntry(key);  // 缺失时创建条目
        if (e->pending) return;      // 该进程的抓取已在途
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
        e->pending = true;  // 提交前先标记，避免重入的 Request 重复提交
    }

    const auto seqHolder = std::make_shared<std::atomic<uint64_t>>(0);
    const uint64_t seq = jobs_.Submit([this, key, path, todo, seqHolder] {
        ProcessDetails d;
        {
            std::lock_guard<std::mutex> lock(mu_);
            const auto it = cache_.find(key);
            if (it == cache_.end()) return;  // 排队期间被失效
            d = it->second.data;             // 合并基底：保留其他类别拥有的字段
        }

        // 分层打开：先请求完全访问（cmdLine/模块/GUI 计数需要 VM_READ），
        // 再退到查询受限（对同用户目标仍足以查令牌/用户）。
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
                d.sig = VerifyFileSignature(path);  // 签名结果缓存对该路径去重
            } else {
                d.sig = SigState::NoCheck;  // 诚实：未检查，原因在 UI 文本中展示
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
            n.kind = Notification::Kind::JobDone;  // 签名检查不需要进程句柄
            n.text = L"进程签名已更新";
        } else {
            n.kind = Notification::Kind::JobFailed;
            n.text = L"无法读取进程详情：进程可能已退出或权限不足";
        }
        notes_.Push(n);
    });
    seqHolder->store(seq);

    if (seq == 0) {
        // 队列未运行：回滚，让之后的 Request 可以重试。
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
    // UI 集成点（架构第 8 节）：在 UI 线程上对每个在快照间
    // 消失的 ProcKey 调用本函数，使缓存跟随存活进程集；
    // Request() 里的容量修剪只是安全网。
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
    return &cache_[key];  // 调用方必须持有 mu_（加锁契约）
}

}  // namespace stm::ops
