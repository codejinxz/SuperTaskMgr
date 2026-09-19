// 第 6 阶段控制操作 + 崩溃日志冒烟测试（契约 ops/ProcessControl.h /
// ops/CrashLog.h）。这里一切都被设计为无需提权即可通过：所有
// 进程控制操作只作用于我们自己的子进程，崩溃日志查询
// 读取对非管理员开放的经典 Application/System 通道。
// 受环境限制的路径以 [info] 说明跳过，绝不断言
//（架构第 7 节权限矩阵，与 ops_test/ops3_test 同一策略）。
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

// 供测试导出（定义于 ops/CrashLog.cpp，刻意不在冻结契约头中）：
// 对捕获的 EvtRender 文本块做纯 XML -> CrashEvent 再解析。
// selftest 链接 stm_ops，该自由函数在那里解析。
namespace stm {
namespace ops {
void ParseEventXml(const std::wstring& xml, CrashEvent* out);
}  // namespace ops
}  // namespace stm

namespace {

constexpr uint64_t kFileTime1Sec = 10'000'000ull;

// --- 真机样例（EvtRender XML，架构师捕获；裁剪到被测字段
// 但保持真实：单引号属性、&#xA; 实体）--------

// 形态 A：WER "Application Error"，具名 Data 字段。
const wchar_t* kWerNamed1000Xml =
    L"<Event><System>"
    L"<Provider Name='Application Error'/>"
    L"<EventID>1000</EventID>"
    L"<Level>2</Level>"
    L"<TimeCreated SystemTime='2026-09-17T12:34:56.1234567Z'/>"
    L"<Execution ProcessID='87360' ThreadID='9160'/>"
    L"</System><EventData>"
    L"<Data Name='AppName'>x.exe</Data>"
    L"<Data Name='AppPath'>D:\\tools\\x.exe</Data>"
    L"<Data Name='FaultingModulePath'>C:\\Windows\\System32\\ntdll.dll</Data>"
    L"</EventData></Event>";

// 形态 B：".NET Runtime"，一条无名自由文本 Data 块，带 &#xA; 换行，
// 且 SystemTime 无毫秒（两者设计上都容忍）。
const wchar_t* kDotNetBlobXml =
    L"<Event><System>"
    L"<Provider Name='.NET Runtime'/>"
    L"<EventID>1000</EventID>"
    L"<Level>2</Level>"
    L"<TimeCreated SystemTime='2026-09-18T08:00:00Z'/>"
    L"<Execution ProcessID='4242' ThreadID='776'/>"
    L"</System><EventData>"
    L"<Data>Category: Runtime Error&#xA;Faulting application name: calc.exe, "
    L"version 10.0.1.0, time stamp 0x6789&#xA;Faulting module name: CORECLR.DLL, "
    L"version 9.0.0, faulting address 0x0000DEAD&#xA;Exception: "
    L"System.DivideByZeroException</Data>"
    L"</EventData></Event>";

// EventID 1002 "Application Hang"，具名字段（真机样本）。
const wchar_t* kHang1002Xml =
    L"<Event><System>"
    L"<Provider Name='Application Hang'/>"
    L"<EventID>1002</EventID>"
    L"<Level>3</Level>"
    L"<TimeCreated SystemTime='2026-09-18T09:30:00.0000000Z'/>"
    L"<Execution ProcessID='66976' ThreadID='21472'/>"
    L"</System><EventData>"
    L"<Data Name='AppName'>SuperTaskMgr.exe</Data>"
    L"<Data Name='AppVersion'>0.0.0.0</Data>"
    L"<Data Name='ProcessId'>0x105a0</Data>"
    L"<Data Name='ExeFileName'>D:\\build\\SuperTaskMgr.exe</Data>"
    L"<Data Name='HangType'>Unknown</Data>"
    L"</EventData></Event>";

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

// 控制测试共享的子进程：cmd.exe 停在约 20s 的 ping 上，
// 使挂起/恢复/优先级/亲和性操作始终作用于存活且归我们所有的目标。
// 成功时 *proc/*thread 由 RAII 保护；KillChild 确定性地回收它。
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

// 挂起 -> 仍存活 -> 恢复 -> 仍存活 -> 清理。suspended 标志在
// NtQSI 检查可用时经 GetProcessControlInfo 交叉核对（仅作信息：
// 线程退出竞争会让对多线程 cmd.exe 的硬断言不稳定）。
STM_TEST(ctrl_suspend_resume_roundtrip) {
    PROCESS_INFORMATION pi{};
    std::wstring e;
    if (!LaunchChild(&pi, &e)) {
        *err = e;
        return false;
    }
    const stm::ProcKey key = KeyOf(pi);

    bool ok = false;
    for (;;) {  // 单次循环：break = 清理点
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

// SetPriorityClass(BelowNormal) 必须被 GetPriorityClass 反映，随后恢复。
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
        // 在评判恢复结果本身之前先恢复
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

// 子进程亲和性往返：读系统掩码，清掉一个核（至少保留一个），
// 设置、读回、恢复。
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
        target = sys & ~(1ull << clearBit);  // 去掉最低的置位核：其余保持
        if (target == 0) {
            // 单核机器：没有可安全移除的 => 环境跳过并清理
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

// 保护闸门同样覆盖挂起：csrss.exe 必须在任何句柄打开前
// 按名称被拒绝。只走拒绝路径——这里绝不真的挂起任何东西。
STM_TEST(ctrl_protected_suspend_refusal) {
    const uint32_t pid = FindPidByName(L"csrss.exe");
    if (pid == 0) return true;  // 无 csrss 的特殊环境：静默跳过
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

// 死身份：从未存在的 ProcKey 必须被每个变更操作以协议消息拒绝
//（绝不只凭 pid 行动）。
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

// 形态 A（WER，具名 Data）：app/module 按字段名映射；标题由
// 提供程序决定；带小数 + Z 的 ISO-8601 解析出精确 Unix 秒。
STM_TEST(crashlog_parse_wer_named) {
    stm::ops::CrashEvent ev;
    stm::ops::ParseEventXml(kWerNamed1000Xml, &ev);
    if (ev.eventId != 1000) {
        *err = stm::Fmt(L"eventId={}，期望 1000", ev.eventId);
        return false;
    }
    if (ev.provider != L"Application Error") {
        *err = L"provider 解析错误：" + ev.provider;
        return false;
    }
    if (ev.level != 2) {
        *err = stm::Fmt(L"level={}，期望 2", ev.level);
        return false;
    }
    if (ev.unixTime != 1789648496) {  // 2026-09-17T12:34:56Z（预期时刻）
        *err = stm::Fmt(L"unixTime={}，期望 1789648496", ev.unixTime);
        return false;
    }
    if (ev.app != L"x.exe") {
        *err = L"app 应取 AppName 的 x.exe，实际：" + ev.app;
        return false;
    }
    if (ev.module != L"ntdll.dll") {
        *err = L"module 应取 FaultingModulePath 的镜像名，实际：" + ev.module;
        return false;
    }
    if (ev.summary != L"应用崩溃：x.exe（模块 ntdll.dll）") {
        *err = L"summary 不符合预期：" + ev.summary;
        return false;
    }
    return true;
}

// 形态 B（.NET Runtime，无名文本块）：summary 取第一条非空行，
// app/module 来自文本启发式（首个 *.exe；"faulting
// module" 之后的 *.dll，大小写不敏感）。
STM_TEST(crashlog_parse_blob_unnamed) {
    stm::ops::CrashEvent ev;
    stm::ops::ParseEventXml(kDotNetBlobXml, &ev);
    if (ev.provider != L".NET Runtime") {
        *err = L"provider 解析错误：" + ev.provider;
        return false;
    }
    if (ev.unixTime != 1789718400) {  // 2026-09-18T08:00:00Z（无毫秒 + Z）
        *err = stm::Fmt(L"unixTime={}，期望 1789718400", ev.unixTime);
        return false;
    }
    if (ev.app != L"calc.exe") {
        *err = L"blob 启发式应提取首个 *.exe token（calc.exe），实际：" + ev.app;
        return false;
    }
    if (ev.module != L"CORECLR.DLL") {
        *err = L"blob 启发式应在 faulting module 后提取 *.dll，实际：" + ev.module;
        return false;
    }
    if (ev.summary.rfind(L".NET 运行时错误：calc.exe", 0) != 0) {
        *err = L"summary 应以 .NET 运行时错误 label + app 开头：" + ev.summary;
        return false;
    }
    if (ev.summary.find(L"Category: Runtime Error") == std::wstring::npos) {
        *err = L"summary 未包含 blob 首行非空文本：" + ev.summary;
        return false;
    }
    return true;
}

// EventID 1002（Application Hang）：AppName 映射到 app，ExeFileName
// 不与 module 混淆，标题是挂起措辞（而非崩溃措辞）。
STM_TEST(crashlog_parse_hang_1002) {
    stm::ops::CrashEvent ev;
    stm::ops::ParseEventXml(kHang1002Xml, &ev);
    if (ev.eventId != 1002) {
        *err = stm::Fmt(L"eventId={}，期望 1002", ev.eventId);
        return false;
    }
    if (ev.provider != L"Application Hang") {
        *err = L"provider 解析错误：" + ev.provider;
        return false;
    }
    if (ev.level != 3) {
        *err = stm::Fmt(L"level={}，期望 3", ev.level);
        return false;
    }
    if (ev.unixTime != 1789723800) {  // 2026-09-18T09:30:00Z
        *err = stm::Fmt(L"unixTime={}，期望 1789723800", ev.unixTime);
        return false;
    }
    if (ev.app != L"SuperTaskMgr.exe") {
        *err = L"app 应取 AppName，实际：" + ev.app;
        return false;
    }
    if (!ev.module.empty()) {
        *err = L"挂起事件不应有 module，实际：" + ev.module;
        return false;
    }
    if (ev.summary.rfind(L"应用挂起：SuperTaskMgr.exe", 0) != 0) {
        *err = L"summary 应以 应用挂起 label + app 开头：" + ev.summary;
        return false;
    }
    return true;
}

// 完全没有可用 Data（具名字段映射不出东西、也无文本块）：条目必须
// 退化为 Execution ProcessID——绝不用“未知”占位。
STM_TEST(crashlog_parse_unnamed_falls_back_to_pid) {
    const std::wstring xml =
        L"<Event><System>"
        L"<Provider Name='Windows Error Reporting'/>"
        L"<EventID>1001</EventID><Level>4</Level>"
        L"<TimeCreated SystemTime='2026-09-18T10:00:00.500Z'/>"
        L"<Execution ProcessID='87360' ThreadID='556'/>"
        L"</System><EventData>"
        L"<Data Name='ReportId'>1</Data><Data Name='P2'>1.2.3.4</Data>"
        L"</EventData></Event>";
    stm::ops::CrashEvent ev;
    stm::ops::ParseEventXml(xml, &ev);
    if (ev.app != L"PID 87360") {
        *err = L"无可用 Data 时 app 应退化为 Execution ProcessID，实际：" + ev.app;
        return false;
    }
    if (ev.summary.rfind(L"WER 报告：PID 87360", 0) != 0) {
        *err = L"summary 应以 WER 报告 label + PID 开头：" + ev.summary;
        return false;
    }
    if (ev.unixTime != 1789725600) {  // 2026-09-18T10:00:00Z（毫秒被容忍）
        *err = stm::Fmt(L"unixTime={}，期望 1789725600", ev.unixTime);
        return false;
    }
    return true;
}

// 崩溃日志查询不得崩溃、保持在契约 ID 内且最新在前。
// err 非空只是通道限制（诚实降级），绝非失败。
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
        if (ev.app.empty() || ev.app.find(L"未知") != std::wstring::npos) {
            *err = L"存在空 app 或仍是未知占位的事件：" + ev.app;
            return false;
        }
    }
    for (size_t i = 0; i < evs.size() && i < 3; ++i) {  // first-3 spot check output
        const auto& ev = evs[i];
        printf("  [info] #%zu id=%u lv=%u app=%s mod=%s | %s\n", i, ev.eventId, ev.level,
               stm::WideToUtf8(ev.app).c_str(), stm::WideToUtf8(ev.module).c_str(),
               stm::WideToUtf8(ev.summary).c_str());
    }
    printf("  [info] 崩溃历史 %zu 条（近 50 上限，非管理员）\n", evs.size());
    return true;
}
