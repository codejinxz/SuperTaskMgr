// Bug F1 fix tests (2026-09): the confirm-dialog execution chain, without any GUI.
//
// Regression context: the confirm buttons of the 终止进程 / 终止进程树 / 确认禁用
// dialogs had no effect. The UI state machines were fixed in app/ui/Pages.cpp and
// app/ui3/Pages3.cpp; the action itself now lives in ui::ExecuteConfirmedAction()
// (app/ui/ConfirmAction.h, header-only, ImGui-free). These cases prove the whole
// "确认 -> jobs.Submit -> ops 生效 -> notes 回填" chain on a real Windows system:
//   fix_confirm_kill_flow              kill a spawned cmd child, assert it exits
//   fix_confirm_tree_flow              cmd -> ping tree kill, assert both exit
//   fix_startup_toggle_action          temp HKCU Run item, assert StartupApproved
//   fix_confirm_submit_failure_notify  refused submit must produce a JobFailed note
//   fix_confirm_kill_reject_protected  ops protection gate must surface as a note
// All cases pass without elevation.
#include "selftest/TestFramework.h"
#include "app/ui/ConfirmAction.h"
#include "core/HandleGuard.h"
#include "core/Notifications.h"
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <vector>

using namespace stm;
using namespace stm::ui;  // test TU: ui::ConfirmKind / ConfirmRequest used throughout

namespace {

constexpr uint64_t kFileTime1Sec = 10'000'000ull;

uint64_t FileTimeToU64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// Started job queue + notification queue; the minimal environment a confirm action
// needs (no collect service, no details provider for these paths).
std::shared_ptr<stm::AppContext> MakeCtx(bool startQueue) {
    auto ctx = std::make_shared<stm::AppContext>();
    if (startQueue) ctx->jobs.Start();
    return ctx;
}

struct SpawnedProc {
    stm::UniqueHandle proc;  // full access handle (wait + terminate fallback)
    stm::UniqueHandle query; // PROCESS_QUERY_LIMITED_INFORMATION for GetProcessTimes
    uint32_t pid = 0;
    uint64_t createTime = 0;

    bool Exited() const {
        return proc && WaitForSingleObject(proc.get(), 0) == WAIT_OBJECT_0;
    }
};

// Spawns a detached cmd.exe running `ping -n <n> 127.0.0.1` (~n seconds lifetime).
bool SpawnPingCmd(int n, SpawnedProc* out) {
    wchar_t cmdLine[128];
    swprintf_s(cmdLine, L"cmd.exe /c ping -n %d 127.0.0.1", n);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(L"C:\\Windows\\System32\\cmd.exe", cmdLine, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    out->proc.reset(pi.hProcess);
    out->pid = pi.dwProcessId;
    FILETIME c{}, x{}, k{}, u{};
    out->query.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pi.dwProcessId));
    if (out->query && GetProcessTimes(out->query.get(), &c, &x, &k, &u)) {
        out->createTime = FileTimeToU64(c);
    }
    return true;
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

// Waits for the predicate up to timeoutMs; returns the last predicate value.
template <typename Pred>
bool PollUntil(Pred pred, DWORD timeoutMs) {
    const DWORD deadline = GetTickCount() + timeoutMs;
    for (;;) {
        if (pred()) return true;
        if (GetTickCount() >= deadline) return pred();
        Sleep(100);
    }
}

// Drains the notification queue until a note matching `match` appears.
bool WaitForNote(stm::AppContext& ctx, DWORD timeoutMs,
                 bool (*match)(const stm::Notification&), stm::Notification* found) {
    std::vector<stm::Notification> all;
    const bool ok = PollUntil([&] {
        std::vector<stm::Notification> out;
        ctx.notes.Drain(&out);
        for (auto& n : out) {
            all.push_back(n);
            if (match(n)) return true;
        }
        return false;
    }, timeoutMs);
    if (found) *found = all.empty() ? stm::Notification{} : all.back();
    return ok;
}

// ---- temporary HKCU Run startup item ---------------------------------------

constexpr wchar_t kTestRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kTestValue[] = L"stm_f1_selftest";
constexpr wchar_t kApprovedRun[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

bool ReadApprovedLowByte(BYTE* low) {
    HKEY raw = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedRun, 0, KEY_QUERY_VALUE, &raw) != ERROR_SUCCESS) {
        return false;
    }
    BYTE buf[16]{};
    DWORD size = sizeof(buf);
    const LONG rc = RegQueryValueExW(raw, kTestValue, nullptr, nullptr, buf, &size);
    RegCloseKey(raw);
    if (rc != ERROR_SUCCESS || size < 1) return false;
    *low = buf[0];
    return true;
}

stm::ops::StartupItem MakeTestStartupItem() {
    stm::ops::StartupItem it;
    it.source = stm::ops::StartupSource::RegRun;
    it.id = std::wstring(L"HKCU\\") + kTestRunKey + L"\\" + kTestValue;
    it.name = kTestValue;
    it.command = L"C:\\Windows\\System32\\notepad.exe";
    it.location = std::wstring(L"HKCU\\") + kTestRunKey;
    it.enabled = true;
    it.canToggle = true;
    return it;
}

}  // namespace

// A confirm kill request executed through ui::ExecuteConfirmedAction must actually
// terminate the spawned target within the poll window.
STM_TEST(fix_confirm_kill_flow) {
    SpawnedProc p;
    if (!SpawnPingCmd(30, &p)) { *err = L"创建 cmd 子进程失败"; return false; }
    if (p.createTime == 0) { *err = L"无法读取目标 createTime"; TerminateProcess(p.proc.get(), 1); return false; }

    auto ctx = MakeCtx(true);
    ui::ConfirmRequest req;
    req.kind = ui::ConfirmKind::Kill;
    req.key = stm::ProcKey{p.pid, p.createTime};
    req.pid = p.pid;
    req.name = L"cmd.exe";
    if (!ui::ExecuteConfirmedAction(ctx, req)) {
        *err = L"ExecuteConfirmedAction 未接受 kill 请求";
        TerminateProcess(p.proc.get(), 1);
        ctx->jobs.Shutdown(2000);
        return false;
    }
    const bool exited = PollUntil([&] { return p.Exited(); }, 8000);
    if (!exited) *err = L"确认终止后 8 秒内目标进程未退出（kill 链路失效）";
    TerminateProcess(p.proc.get(), 1);  // best-effort cleanup on failure paths
    ctx->jobs.Shutdown(2000);
    return exited;
}

// A confirm kill-tree request must take down the cmd root AND its ping descendant.
STM_TEST(fix_confirm_tree_flow) {
    SpawnedProc p;
    if (!SpawnPingCmd(30, &p)) { *err = L"创建 cmd 子进程失败"; return false; }

    uint32_t pingPid = 0;
    stm::UniqueHandle ping(  // observe ping's lifetime without owning it
        [&] {
            PollUntil([&] { return (pingPid = FindChildPid(p.pid, L"ping.exe")) != 0; }, 3000);
            return pingPid ? OpenProcess(SYNCHRONIZE, FALSE, pingPid) : nullptr;
        }());
    if (!ping) {
        *err = L"3 秒内未观察到 ping 子进程";
        TerminateProcess(p.proc.get(), 1);
        return false;
    }

    auto ctx = MakeCtx(true);
    ui::ConfirmRequest req;
    req.kind = ui::ConfirmKind::KillTree;
    req.key = stm::ProcKey{p.pid, p.createTime};
    req.pid = p.pid;
    req.name = L"cmd.exe";
    if (!ui::ExecuteConfirmedAction(ctx, req)) {
        *err = L"ExecuteConfirmedAction 未接受 kill-tree 请求";
        TerminateProcess(p.proc.get(), 1);
        ctx->jobs.Shutdown(2000);
        return false;
    }
    const bool rootExited = PollUntil([&] { return p.Exited(); }, 8000);
    const bool pingExited = WaitForSingleObject(ping.get(), 8000) == WAIT_OBJECT_0;
    if (!rootExited) *err = L"确认终止进程树后 8 秒内根进程未退出";
    if (!pingExited) *err = L"确认终止进程树后 8 秒内 ping 子进程未退出（树杀失效）";
    TerminateProcess(p.proc.get(), 1);
    ctx->jobs.Shutdown(2000);
    return rootExited && pingExited;
}

// A confirm disable request on a temporary HKCU Run item must flip the
// StartupApproved\Run value to the disabled encoding (odd low byte), and the
// enable request must flip it back — then clean up both registry values.
STM_TEST(fix_startup_toggle_action) {
    HKEY raw = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kTestRunKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &raw, nullptr) != ERROR_SUCCESS) {
        *err = L"无法打开 HKCU Run 测试键";
        return false;
    }
    const std::wstring cmd = L"C:\\Windows\\System32\\notepad.exe";
    if (RegSetValueExW(raw, kTestValue, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) {
        RegCloseKey(raw);
        *err = L"无法写入 HKCU Run 测试值";
        return false;
    }
    RegCloseKey(raw);

    auto cleanup = [&] {
        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kTestRunKey, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            RegDeleteValueW(k, kTestValue);
            RegCloseKey(k);
        }
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedRun, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            RegDeleteValueW(k, kTestValue);
            RegCloseKey(k);
        }
    };

    auto ctx = MakeCtx(true);
    ui::ConfirmRequest req;
    req.kind = ui::ConfirmKind::StartupToggle;
    req.startupEnable = false;  // 确认禁用
    req.startupItem = MakeTestStartupItem();

    if (!ui::ExecuteConfirmedAction(ctx, req)) {
        *err = L"ExecuteConfirmedAction 未接受 disable 请求";
        cleanup();
        ctx->jobs.Shutdown(2000);
        return false;
    }
    BYTE low = 0;
    const bool disabled = PollUntil([&] { return ReadApprovedLowByte(&low) && (low & 1) == 1; },
                                    8000);
    if (!disabled) {
        *err = L"确认禁用后 StartupApproved\\Run 值未变为禁用编码（启动项链路失效）";
        cleanup();
        ctx->jobs.Shutdown(2000);
        return false;
    }

    // Reverse direction: 确认启用 must clear the disabled encoding (low byte 0x02).
    req.startupEnable = true;
    if (!ui::ExecuteConfirmedAction(ctx, req)) {
        *err = L"ExecuteConfirmedAction 未接受 enable 请求";
        cleanup();
        ctx->jobs.Shutdown(2000);
        return false;
    }
    const bool enabled = PollUntil([&] { return ReadApprovedLowByte(&low) && (low & 1) == 0; },
                                   8000);
    if (!enabled) *err = L"确认启用后 StartupApproved\\Run 值未恢复为启用编码";
    cleanup();
    ctx->jobs.Shutdown(2000);
    return disabled && enabled;
}

// A refused submission (ops queue not running) must be reported through the
// notification queue — a confirm click may never be a silent no-op.
STM_TEST(fix_confirm_submit_failure_notify) {
    auto ctx = MakeCtx(false);  // queue NOT started
    ui::ConfirmRequest req;
    req.kind = ui::ConfirmKind::Kill;
    req.key = stm::ProcKey{0xDEAD, 0};
    req.pid = 0xDEAD;
    req.name = L"ghost.exe";
    if (ui::ExecuteConfirmedAction(ctx, req)) {
        *err = L"队列未启动时 ExecuteConfirmedAction 不应返回 true";
        return false;
    }
    const bool notified = WaitForNote(
        *ctx, 1000,
        [](const stm::Notification& n) {
            return n.kind == stm::Notification::Kind::JobFailed &&
                   n.text.find(L"操作队列未运行") != std::wstring::npos;
        },
        nullptr);
    if (!notified) *err = L"提交失败未产生 JobFailed 通知（静默失败）";
    return notified;
}

// The ops protection gate must surface as a JobFailed note through the same
// notes -> toast pipeline (no privileged bypass from the confirm action).
STM_TEST(fix_confirm_kill_reject_protected) {
    auto ctx = MakeCtx(true);
    ui::ConfirmRequest req;
    req.kind = ui::ConfirmKind::Kill;
    req.key = stm::ProcKey{4, 1};  // pid 4 = System, on the built-in protection list
    req.pid = 4;
    req.name = L"System";
    if (!ui::ExecuteConfirmedAction(ctx, req)) {
        *err = L"队列运行时受保护目标也应正常入队（由 ops 拒绝并反馈）";
        ctx->jobs.Shutdown(2000);
        return false;
    }
    const bool rejected = WaitForNote(
        *ctx, 8000,
        [](const stm::Notification& n) {
            return n.kind == stm::Notification::Kind::JobFailed &&
                   n.text.find(L"已拒绝终止") != std::wstring::npos;
        },
        nullptr);
    if (!rejected) *err = L"受保护进程终止被拒绝后未产生 JobFailed 通知";
    ctx->jobs.Shutdown(2000);
    return rejected;
}
