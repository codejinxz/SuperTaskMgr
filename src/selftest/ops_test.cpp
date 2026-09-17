// Ops-layer smoke tests (arch section 6/8): identity re-verify, protection gate,
// working-set trim, tree plan/terminate, signature verify, details provider.
// All cases are designed to pass without elevation; nothing here needs admin.
#include "selftest/TestFramework.h"
#include "core/Err.h"
#include "core/HandleGuard.h"
#include "core/Jobs.h"
#include "core/Notifications.h"
#include "core/ProcData.h"
#include "core/Str.h"
#include "ops/DetailsProvider.h"
#include "ops/ProcessOps.h"
#include "ops/Signature.h"
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kFileTime1Sec = 10'000'000ull;

uint64_t FileTimeToU64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

uint64_t CreateTimeOf(uint32_t pid) {
    stm::UniqueHandle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!h) return 1;
    FILETIME c{}, x{}, k{}, u{};
    if (!GetProcessTimes(h.get(), &c, &x, &k, &u)) return 1;
    return FileTimeToU64(c);
}

uint32_t FindPidByName(const wchar_t* name) {
    HANDLE raw = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (raw == INVALID_HANDLE_VALUE) return 0;
    stm::UniqueHandle snap(raw);
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap.get(), &pe)) return 0;
    do {
        if (_wcsicmp(pe.szExeFile, name) == 0) return pe.th32ProcessID;
    } while (Process32NextW(snap.get(), &pe));
    return 0;
}

uint32_t FindChildPid(uint32_t parentPid, const wchar_t* name) {
    HANDLE raw = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (raw == INVALID_HANDLE_VALUE) return 0;
    stm::UniqueHandle snap(raw);
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap.get(), &pe)) return 0;
    do {
        if (pe.th32ParentProcessID == parentPid && _wcsicmp(pe.szExeFile, name) == 0) {
            return pe.th32ProcessID;
        }
    } while (Process32NextW(snap.get(), &pe));
    return 0;
}

bool ContainsPid(const std::vector<stm::ProcKey>& list, uint32_t pid) {
    return std::any_of(list.begin(), list.end(),
                       [pid](const stm::ProcKey& k) { return k.pid == pid; });
}

std::wstring SelfExePath() {
    wchar_t buf[1024]{};
    GetModuleFileNameW(nullptr, buf, 1024);
    return buf;
}

}  // namespace

// Dead identity: pid never exists + createTime=1 => refuse with the protocol message.
STM_TEST(ops_identity_reverify_dead_pid) {
    const stm::ProcKey dead{0xFFFFFFu, 1};
    std::wstring e;
    if (stm::ops::TerminateProcessById(dead, &e)) {
        *err = L"对不存在 PID 的终止请求不应成功";
        return false;
    }
    if (e.find(L"已退出或 PID") == std::wstring::npos) {
        *err = L"错误信息缺少身份重验说明：" + e;
        return false;
    }
    return true;
}

// Protection gate: csrss.exe must be refused by name before any handle is opened.
// Refusal path only — nothing is ever terminated here.
STM_TEST(ops_protected_refusal) {
    const uint32_t pid = FindPidByName(L"csrss.exe");
    if (pid == 0) return true;  // exotic environment without csrss: skip silently
    const stm::ProcKey key{pid, CreateTimeOf(pid)};
    std::wstring e;
    if (stm::ops::TerminateProcessById(key, &e)) {
        *err = L"csrss.exe 未被保护名单拦截";
        return false;
    }
    if (e.find(L"保护") == std::wstring::npos) {
        *err = L"拒绝信息未提及保护原因：" + e;
        return false;
    }
    return true;
}

// TrimWorkingSet on self: must succeed; working set may drop but must not crash.
STM_TEST(ops_trim_self) {
    const stm::ProcKey self{GetCurrentProcessId(), CreateTimeOf(GetCurrentProcessId())};
    PROCESS_MEMORY_COUNTERS before{};
    PROCESS_MEMORY_COUNTERS after{};
    GetProcessMemoryInfo(GetCurrentProcess(), &before, sizeof(before));
    std::wstring e;
    if (!stm::ops::TrimWorkingSet(self, &e)) {
        *err = L"TrimWorkingSet 失败：" + e;
        return false;
    }
    GetProcessMemoryInfo(GetCurrentProcess(), &after, sizeof(after));
    // No hard assertion on the numbers (WS is allowed to drop or fluctuate); just report.
    printf("  [info] 工作集 trim 前=%llu B 后=%llu B\n",
           static_cast<unsigned long long>(before.WorkingSetSize),
           static_cast<unsigned long long>(after.WorkingSetSize));
    return true;
}

// Spawn cmd.exe -> ping.exe (two levels), plan the tree, then terminate it leaf-first.
STM_TEST(ops_tree_plan) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    wchar_t cmdLine[] = L"cmd.exe /c ping -n 15 127.0.0.1";
    if (!CreateProcessW(L"C:\\Windows\\System32\\cmd.exe", cmdLine, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        *err = stm::ErrContext(L"创建 cmd 子进程失败", stm::LastHr());
        return false;
    }
    stm::UniqueHandle procGuard(pi.hProcess);
    stm::UniqueHandle threadGuard(pi.hThread);
    const stm::ProcKey cmdKey{pi.dwProcessId, CreateTimeOf(pi.dwProcessId)};

    uint32_t pingPid = 0;
    for (int i = 0; i < 30 && pingPid == 0; ++i) {  // up to 3s for cmd to spawn ping
        Sleep(100);
        pingPid = FindChildPid(cmdKey.pid, L"ping.exe");
    }
    if (pingPid == 0) {
        TerminateProcess(procGuard.get(), 1);
        *err = L"3 秒内未观察到 ping 子进程";
        return false;
    }

    std::vector<stm::ProcKey> plan;
    std::wstring e;
    if (!stm::ops::PlanTerminateTree(cmdKey, &plan, &e)) {
        TerminateProcess(procGuard.get(), 1);
        *err = L"PlanTerminateTree 失败：" + e;
        return false;
    }
    if (!ContainsPid(plan, pingPid)) {
        TerminateProcess(procGuard.get(), 1);
        *err = L"计划未包含 ping 子进程";
        return false;
    }
    if (ContainsPid(plan, GetCurrentProcessId())) {
        TerminateProcess(procGuard.get(), 1);
        *err = L"计划误包含自身进程";
        return false;
    }

    stm::ops::TreeResult res;
    std::wstring e2;
    const bool ok = stm::ops::TerminateTree(cmdKey, &res, &e2);
    WaitForSingleObject(procGuard.get(), 5000);  // reap cmd regardless of outcome
    if (!ok || res.terminated < 2 || res.failed != 0 || res.planned < 2) {
        *err = stm::Fmt(L"树杀结果不符：planned={} terminated={} failed={} skipped={} err={}",
                        res.planned, res.terminated, res.failed, res.skippedProtected, e2);
        return false;
    }
    return true;
}

// Self exe is unsigned in dev builds (never Valid); notepad.exe is catalog-signed.
STM_TEST(ops_signature_self) {
    const std::wstring selfPath = SelfExePath();
    const stm::ops::SigState selfState = stm::ops::VerifyFileSignature(selfPath);
    if (selfState != stm::ops::SigState::Unsigned && selfState != stm::ops::SigState::Invalid) {
        *err = L"未签名构建的自身 EXE 签名状态不应为 Valid";
        return false;
    }
    const wchar_t* notepad = L"C:\\Windows\\System32\\notepad.exe";
    if (GetFileAttributesW(notepad) == INVALID_FILE_ATTRIBUTES) return true;  // env skip
    if (stm::ops::VerifyFileSignature(notepad) != stm::ops::SigState::Valid) {
        *err = L"系统文件 notepad.exe 的 catalog 签名校验应为 Valid";
        return false;
    }
    return true;
}

// DetailsProvider on self: signature + user name + GUI objects resolve within 5s.
STM_TEST(ops_details_request) {
    stm::JobQueue jobs;
    stm::NotificationQueue notes;
    if (!jobs.Start()) {
        *err = L"JobQueue 启动失败";
        return false;
    }
    stm::ops::DetailsProvider dp(jobs, notes);
    const stm::ProcKey self{GetCurrentProcessId(), CreateTimeOf(GetCurrentProcessId())};
    const uint32_t kinds = static_cast<uint32_t>(stm::ops::DetailKind::Signature) |
                           static_cast<uint32_t>(stm::ops::DetailKind::UserInfo) |
                           static_cast<uint32_t>(stm::ops::DetailKind::GuiObjects);
    dp.Request(self, SelfExePath(), kinds);

    const stm::ops::ProcessDetails* d = nullptr;
    for (int i = 0; i < 100; ++i) {  // poll up to 5s
        Sleep(50);
        d = dp.Peek(self);
        if (d && d->guiResolved && d->userNameResolved) break;
        d = nullptr;
    }
    uint32_t gdi = 0;
    uint32_t usr = 0;
    const bool ok = d != nullptr && !d->userName.empty() && dp.TryGetGuiObjects(self, &gdi, &usr);
    if (ok) {
        printf("  [info] userName=%s gdi=%lu user=%lu sig=%d\n", stm::WideToUtf8(d->userName).c_str(),
               static_cast<unsigned long>(gdi), static_cast<unsigned long>(usr),
               static_cast<int>(d->sig));
    }
    jobs.Shutdown(2000);
    if (!ok) {
        *err = L"5 秒内未取到 GDI/USER 计数或用户名为空";
        return false;
    }
    return true;
}
