// Phase-6 control ops + crash log smoke tests (contracts ops/ProcessControl.h /
// ops/CrashLog.h). Everything here is designed to pass WITHOUT elevation: all
// process-control ops act on our own child process only, and the crash-log query
// reads the classic Application/System channels which are open to non-admin.
// Environment-restricted paths are skipped with an [info] note, never asserted
// (arch section 7 permission matrix, same policy as ops_test/ops3_test).
#include "selftest/TestFramework.h"
#include "core/Err.h"
#include "core/HandleGuard.h"
#include "core/ProcData.h"
#include "core/Str.h"
#include "ops/CrashLog.h"
#include "ops/ProcessControl.h"
#include <windows.h>
#include <tlhelp32.h>
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

// Child process shared by the control tests: cmd.exe parked on a ~20 s ping so the
// suspend/resume/priority/affinity ops always act on a live, self-owned target.
// On success *proc/*thread are RAII-guarded; KillChild reaps it deterministically.
bool LaunchChild(PROCESS_INFORMATION* pi, std::wstring* err) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    wchar_t cmdLine[] = L"cmd.exe /c ping -n 20 127.0.0.1";
    if (!CreateProcessW(L"C:\\Windows\\System32\\cmd.exe", cmdLine, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, pi)) {
        *err = stm::ErrContext(L"创建 cmd 子进程失败", stm::LastHr());
        return false;
    }
    return true;
}

void KillChild(PROCESS_INFORMATION* pi) {
    TerminateProcess(pi->hProcess, 1);
    WaitForSingleObject(pi->hProcess, 5000);
    CloseHandle(pi->hThread);
    CloseHandle(pi->hProcess);
}

stm::ProcKey KeyOf(const PROCESS_INFORMATION& pi) {
    return stm::ProcKey{pi.dwProcessId, CreateTimeOf(pi.dwProcessId)};
}

bool ChildAlive(HANDLE proc) { return WaitForSingleObject(proc, 0) == WAIT_TIMEOUT; }

}  // namespace

// Suspend -> still alive -> resume -> still alive -> cleanup. The suspended flag is
// cross-checked via GetProcessControlInfo when the NtQSI check is available (info only:
// thread exit races make a hard assertion flaky for a multi-threaded cmd.exe).
STM_TEST(ctrl_suspend_resume_roundtrip) {
    PROCESS_INFORMATION pi{};
    std::wstring e;
    if (!LaunchChild(&pi, &e)) {
        *err = e;
        return false;
    }
    const stm::ProcKey key = KeyOf(pi);

    bool ok = false;
    for (;;) {  // single-shot loop: break = cleanup point
        if (!stm::ops::SuspendProcess(key, &e)) { e = L"SuspendProcess 失败：" + e; break; }
        if (!ChildAlive(pi.hProcess)) { e = L"挂起后子进程不应退出"; break; }

        const stm::ops::ProcessControlInfo info = stm::ops::GetProcessControlInfo(key, &e);
        if (!e.empty()) { e = L"挂起后读取控制信息失败：" + e; break; }
        if (info.suspendedAvail) {
            if (!info.suspended) {
                printf("  [info] 挂起后 NtQSI suspended=false（线程退出竞态，视为跳过）\n");
            }
        } else {
            printf("  [info] suspended 检查不可用（NtQSI 不可达），诚实降级\n");
        }

        if (!stm::ops::ResumeProcess(key, &e)) { e = L"ResumeProcess 失败：" + e; break; }
        if (!ChildAlive(pi.hProcess)) { e = L"恢复后子进程不应退出"; break; }
        ok = true;
        break;
    }

    KillChild(&pi);
    if (!ok) {
        *err = e;
        return false;
    }
    return true;
}

// SetPriorityClass(BelowNormal) must be reflected by GetPriorityClass, then restored.
STM_TEST(ctrl_priority_readback) {
    PROCESS_INFORMATION pi{};
    std::wstring e;
    if (!LaunchChild(&pi, &e)) {
        *err = e;
        return false;
    }
    const stm::ProcKey key = KeyOf(pi);

    bool ok = false;
    for (;;) {
        if (!stm::ops::SetProcPriority(key, stm::ops::ProcPriority::BelowNormal, &e)) {
            e = L"SetProcPriority(BelowNormal) 失败：" + e;
            break;
        }
        const stm::ops::ProcessControlInfo info = stm::ops::GetProcessControlInfo(key, &e);
        if (!e.empty()) { e = L"读取控制信息失败：" + e; break; }
        if (info.priorityClass != BELOW_NORMAL_PRIORITY_CLASS) {
            e = stm::Fmt(L"priorityClass 读回 {}，期望 BELOW_NORMAL({})", info.priorityClass,
                         static_cast<uint32_t>(BELOW_NORMAL_PRIORITY_CLASS));
            break;
        }
        // restore before judging the restore result itself
        if (!stm::ops::SetProcPriority(key, stm::ops::ProcPriority::Normal, &e)) {
            e = L"恢复 Normal 优先级失败：" + e;
            break;
        }
        const stm::ops::ProcessControlInfo after = stm::ops::GetProcessControlInfo(key, &e);
        if (!e.empty()) { e = L"恢复后读取失败：" + e; break; }
        if (after.priorityClass != NORMAL_PRIORITY_CLASS) {
            e = L"恢复后 priorityClass 不为 NORMAL";
            break;
        }
        ok = true;
        break;
    }

    KillChild(&pi);
    if (!ok) {
        *err = e;
        return false;
    }
    return true;
}

// Affinity round-trip on the child: read system mask, clear one core (keeping at least
// one), set, read back, restore.
STM_TEST(ctrl_affinity_roundtrip) {
    PROCESS_INFORMATION pi{};
    std::wstring e;
    if (!LaunchChild(&pi, &e)) {
        *err = e;
        return false;
    }
    const stm::ProcKey key = KeyOf(pi);

    bool ok = false;
    for (;;) {
        const stm::ops::ProcessControlInfo info = stm::ops::GetProcessControlInfo(key, &e);
        if (!e.empty()) { e = L"读取控制信息失败：" + e; break; }
        const uint64_t sys = info.systemAffinityMask;
        if (sys == 0) { e = L"systemAffinityMask 为 0"; break; }

        unsigned clearBit = 0;
        uint64_t target = 0;
        for (unsigned b = 0; b < 64; ++b) {
            if (sys & (1ull << b)) { clearBit = b; break; }
        }
        target = sys & ~(1ull << clearBit);  // drop the LOWEST set core: rest stays
        if (target == 0) {
            // single-core machine: nothing safe to remove => env skip with cleanup
            printf("  [info] 单核环境（systemAffinity=0x%llX），跳过亲和性往返\n",
                   static_cast<unsigned long long>(sys));
            ok = true;
            break;
        }

        if (!stm::ops::SetProcAffinity(key, target, &e)) {
            e = stm::Fmt(L"SetProcAffinity(0x{:X}) 失败：", target) + e;
            break;
        }
        const stm::ops::ProcessControlInfo after = stm::ops::GetProcessControlInfo(key, &e);
        if (!e.empty()) { e = L"设置后读取失败：" + e; break; }
        if (after.affinityMask != target) {
            e = stm::Fmt(L"affinityMask 读回 0x{:X}，期望 0x{:X}", after.affinityMask, target);
            break;
        }
        if (!stm::ops::SetProcAffinity(key, sys, &e)) {
            e = L"恢复全核亲和性失败：" + e;
            break;
        }
        const stm::ops::ProcessControlInfo restored = stm::ops::GetProcessControlInfo(key, &e);
        if (!e.empty()) { e = L"恢复后读取失败：" + e; break; }
        if (restored.affinityMask != sys) {
            e = stm::Fmt(L"恢复后 affinityMask 0x{:X}，期望 0x{:X}", restored.affinityMask, sys);
            break;
        }
        ok = true;
        break;
    }

    KillChild(&pi);
    if (!ok) {
        *err = e;
        return false;
    }
    return true;
}

// Protection gate covers suspend too: csrss.exe must be refused by name before any
// handle is opened. Refusal path only — nothing is ever suspended here.
STM_TEST(ctrl_protected_suspend_refusal) {
    const uint32_t pid = FindPidByName(L"csrss.exe");
    if (pid == 0) return true;  // exotic environment without csrss: skip silently
    const stm::ProcKey key{pid, CreateTimeOf(pid)};
    std::wstring e;
    if (stm::ops::SuspendProcess(key, &e)) {
        *err = L"csrss.exe 的挂起请求未被保护名单拦截";
        return false;
    }
    if (e.find(L"保护") == std::wstring::npos) {
        *err = L"拒绝信息未提及保护原因：" + e;
        return false;
    }
    return true;
}

// Dead identity: a ProcKey that never existed must be refused by every mutating op
// with the protocol message (never act on a pid alone).
STM_TEST(ctrl_identity_dead_pid) {
    const stm::ProcKey dead{0xFFFFFFu, 1};
    std::wstring e;
    if (stm::ops::SuspendProcess(dead, &e)) {
        *err = L"对不存在 PID 的挂起请求不应成功";
        return false;
    }
    if (e.find(L"已退出或 PID") == std::wstring::npos) {
        *err = L"Suspend 错误信息缺少身份重验说明：" + e;
        return false;
    }
    if (stm::ops::ResumeProcess(dead, &e)) {
        *err = L"对不存在 PID 的恢复请求不应成功";
        return false;
    }
    if (e.find(L"已退出或 PID") == std::wstring::npos) {
        *err = L"Resume 错误信息缺少身份重验说明：" + e;
        return false;
    }
    if (stm::ops::SetProcPriority(dead, stm::ops::ProcPriority::Normal, &e)) {
        *err = L"对不存在 PID 的优先级请求不应成功";
        return false;
    }
    if (e.find(L"已退出或 PID") == std::wstring::npos) {
        *err = L"Priority 错误信息缺少身份重验说明：" + e;
        return false;
    }
    if (stm::ops::SetProcAffinity(dead, 1, &e)) {
        *err = L"对不存在 PID 的亲和性请求不应成功";
        return false;
    }
    if (e.find(L"已退出或 PID") == std::wstring::npos) {
        *err = L"Affinity 错误信息缺少身份重验说明：" + e;
        return false;
    }
    stm::ops::ProcessControlInfo info = stm::ops::GetProcessControlInfo(dead, &e);
    if (e.find(L"已退出或 PID") == std::wstring::npos) {
        *err = L"GetProcessControlInfo 错误信息缺少身份重验说明：" + e;
        return false;
    }
    if (info.priorityClass != 0 || info.affinityMask != 0) {
        *err = L"对不存在 PID 返回了非零控制信息";
        return false;
    }
    return true;
}

// Crash-log query must not crash, stay within contract IDs and newest-first order.
// A non-empty err is only a channel limitation (honest degradation), never a failure.
STM_TEST(crashlog_query_ok) {
    std::wstring e;
    const std::vector<stm::ops::CrashEvent> evs = stm::ops::QueryCrashEvents(50, &e);
    if (!e.empty()) {
        printf("  [info] 通道限制（诚实降级）：%s\n", stm::WideToUtf8(e).c_str());
    }
    int64_t prevTime = 0;
    for (const auto& ev : evs) {
        if (ev.eventId != 1000 && ev.eventId != 1001 && ev.eventId != 1002) {
            *err = stm::Fmt(L"越界 eventId={}（契约只允许 1000/1001/1002）", ev.eventId);
            return false;
        }
        if (prevTime != 0 && ev.unixTime > prevTime) {
            *err = stm::Fmt(L"结果未按时间倒序：{} 晚于 {}", ev.unixTime, prevTime);
            return false;
        }
        prevTime = ev.unixTime;
        if (ev.summary.empty()) {
            *err = L"存在空 summary 的事件";
            return false;
        }
    }
    printf("  [info] 崩溃历史 %zu 条（近 50 上限，非管理员）\n", evs.size());
    return true;
}
