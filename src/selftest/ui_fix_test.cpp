// Bug F1 修复测试（2026-09）：确认对话框执行链，完全无 GUI。
//
// Regression context: the confirm buttons of the 终止进程 / 终止进程树 / 确认禁用
// 对话框没有效果。UI 状态机已在 app/ui/Pages.cpp 与
// app/ui3/Pages3.cpp 中修复；动作本身现在位于 ui::ExecuteConfirmedAction()
//（app/ui/ConfirmAction.h，仅头文件、不依赖 ImGui）。这些用例证明整条
// "确认 -> jobs.Submit -> ops 生效 -> notes 回填" chain on a real Windows system:
//   fix_confirm_kill_flow              终止启动的 cmd 子进程，断言其退出
//   fix_confirm_tree_flow              cmd -> ping 树终止，断言两者退出
//   fix_startup_toggle_action          临时 HKCU Run 项，断言 StartupApproved
//   fix_confirm_submit_failure_notify  被拒提交必须产生 JobFailed 通知
//   fix_confirm_kill_reject_protected  ops 保护闸门必须以通知形式浮现
// 所有用例无需提权即可通过。
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

// 已启动的任务队列 + 通知队列；确认动作所需的
// 最小环境（这些路径无需采集服务与详情提供者）。
std::shared_ptr<stm::AppContext> MakeCtx(bool startQueue) {
    auto ctx = std::make_shared<stm::AppContext>();
    if (startQueue) ctx->jobs.Start();
    return ctx;
}

struct SpawnedProc {
    stm::UniqueHandle proc;  // 完全访问句柄（等待 + 终止兜底）
    stm::UniqueHandle query; // 供 GetProcessTimes 用的 PROCESS_QUERY_LIMITED_INFORMATION
    uint32_t pid = 0;
    uint64_t createTime = 0;

    bool Exited() const {
        return proc && WaitForSingleObject(proc.get(), 0) == WAIT_OBJECT_0;
    }
};

// 启动分离的 cmd.exe 运行 `ping -n <n> 127.0.0.1`（寿命约 n 秒）。
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

// 至多等 timeoutMs 的谓词；返回最后一次谓词值。
template <typename Pred>
bool PollUntil(Pred pred, DWORD timeoutMs) {
    const DWORD deadline = GetTickCount() + timeoutMs;
    for (;;) {
        if (pred()) return true;
        if (GetTickCount() >= deadline) return pred();
        Sleep(100);
    }
}

// 取空通知队列直到出现匹配 `match` 的通知。
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

// ---- 临时 HKCU Run 启动项 ---------------------------------------------------

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

// 经 ui::ExecuteConfirmedAction 执行的终止确认必须真的在轮询窗口内
// 终止被启动的目标。
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
    TerminateProcess(p.proc.get(), 1);  // 失败路径上尽力清理
    ctx->jobs.Shutdown(2000);
    return exited;
}

// 终止树的确认请求必须放倒 cmd 根与其 ping 后代。
STM_TEST(fix_confirm_tree_flow) {
    SpawnedProc p;
    if (!SpawnPingCmd(30, &p)) { *err = L"创建 cmd 子进程失败"; return false; }

    uint32_t pingPid = 0;
    stm::UniqueHandle ping(  // 观察而不持有 ping 的生命周期
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

// 对临时 HKCU Run 项的禁用确认必须把 StartupApproved\Run 值
// 翻转为禁用编码（低位奇字节），启用确认必须翻回——
// 然后清理两个注册表值。
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

// 被拒的提交（ops 队列未运行）必须经通知队列表报——
// 确认点击绝不能是静默空操作。
STM_TEST(fix_confirm_submit_failure_notify) {
    auto ctx = MakeCtx(false);  // 队列未启动
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

// ops 保护闸门必须经同一条 notes -> toast 管线以 JobFailed 通知
// 浮现（确认动作没有特权旁路）。
STM_TEST(fix_confirm_kill_reject_protected) {
    auto ctx = MakeCtx(true);
    ui::ConfirmRequest req;
    req.kind = ui::ConfirmKind::Kill;
    req.key = stm::ProcKey{4, 1};  // pid 4 = System，在内置保护名单上
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
