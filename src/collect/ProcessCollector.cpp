// ProcessCollector：每 tick 的进程快照。
// 快路径：一次 NtQuerySystemInformation(SystemProcessInformation) 调用即可取得
// 全系统的 pid/ppid/创建时间/CPU 时间/工作集/私有工作集/IO/句柄数/
// 线程数/缺页数以及每线程上下文切换。
// 降级路径：Toolhelp + PSAPI（兼容模式；contextSwitchesPerSec 与
// privateWorkingSet 在该路径保持 kUnavail）。
// 差值字段（cpuPercent / diskBytesPerSec / pageFaultsPerSec /
// contextSwitchesPerSec）使用 (pid, createTime) 身份，被复用的 PID
// 绝不会产生虚假差值；首个 tick 没有基线 -> kUnavail。
#include "collect/CollectDetail.h"
#include "core/HandleGuard.h"
#include "core/ProtectedList.h"
#include <appmodel.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <wow64apiset.h>
#include <algorithm>
#include <chrono>
#include <set>
#include <unordered_set>

namespace stm {
namespace cd {

namespace {

using Clock = std::chrono::steady_clock;

uint64_t FtU64(const FILETIME& f) {
    return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

// 归一化的每进程行：NtQSI 与 Toolhelp 两条路径形状一致，
// 差值计算/标志组装/ProcInfo 组装得以共享。
struct RawRow {
    uint32_t pid = 0, parentPid = 0, sessionId = 0, handles = 0, threads = 0;
    uint64_t createTime = 0, kernelTime = 0, userTime = 0;
    uint64_t workingSet = 0, commitBytes = 0, pageFaults = 0;
    int64_t privateWs = 0;  // 私有工作集（字节）；仅 pwsKnown 时有效
    uint64_t ioRead = 0, ioWrite = 0, ioOther = 0, ctxSw = 0;
    bool timesKnown = false, wsKnown = false, commitKnown = false;
    bool pfKnown = false, ioKnown = false, ctxKnown = false, pwsKnown = false;
    bool suspended = false;
    std::wstring name;
};

// --- 补充信息辅助（路径 + 标志），两条路径共享 --------------------------------

bool IsElevatedProcess(HANDLE h) {
    HANDLE tok = nullptr;
    if (!::OpenProcessToken(h, TOKEN_QUERY, &tok)) return false;
    UniqueHandle tokGuard(tok);
    TOKEN_ELEVATION e{};
    DWORD ret = 0;
    if (!::GetTokenInformation(tokGuard.get(), TokenElevation, &e, sizeof(e), &ret)) return false;
    return e.TokenIsElevated != 0;
}

// 进程是否运行于 AppContainer 包（UWP）之下。两次调用协议：
// 空缓冲 -> 返回所需长度；普通 Win32 进程以
// APPMODEL_ERROR_NO_PACKAGE 失败（R5 #4b）。
bool IsPackagedProcess(HANDLE h) {
    UINT32 len = 0;
    const LONG rc = ::GetPackageFullName(h, &len, nullptr);
    return rc == ERROR_INSUFFICIENT_BUFFER;
}

BOOL CALLBACK EnumWindowTitleProc(HWND hwnd, LPARAM lp) {
    auto* out = reinterpret_cast<std::unordered_map<uint32_t, std::wstring>*>(lp);
    if (!::IsWindowVisible(hwnd)) return TRUE;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return TRUE;
    wchar_t buf[512]{};
    // 对其他进程的窗口这里读取缓存标题；绝不阻塞。
    if (::GetWindowTextW(hwnd, buf, 511) <= 0) return TRUE;
    out->emplace(pid, buf);  // 按 Z 序枚举：第一个（最顶层）可见窗口胜出
    return TRUE;
}

// pid -> 承载的运行中服务数（R5 #7b：dwProcessId 仅对非停止服务有效，
// 且只有 SERVICE_WIN32 服务带 pid）。
bool EnumRunningServiceHosts(std::unordered_map<uint32_t, uint32_t>* out) {
    out->clear();
    // EnumServicesStatusExW 需要真实的 SCM 句柄（NULL 会以
    // ERROR_INVALID_HANDLE 失败）；SC_MANAGER_ENUMERATE_SERVICE 授予
    // 非管理员调用者。
    SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return false;
    std::vector<BYTE> buf(64 * 1024);
    DWORD needed = 0, returned = 0, resume = 0;
    BOOL ok = FALSE;
    for (;;) {
        ok = ::EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                                     buf.data(), static_cast<DWORD>(buf.size()), &needed,
                                     &returned, &resume, nullptr);
        if (ok) break;
        if (::GetLastError() != ERROR_MORE_DATA) {
            ::CloseServiceHandle(scm);
            return false;
        }
        buf.resize(needed);
    }
    ::CloseServiceHandle(scm);
    const auto* arr = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buf.data());
    for (DWORD i = 0; i < returned; ++i) {
        const SERVICE_STATUS_PROCESS& s = arr[i].ServiceStatusProcess;
        if (s.dwCurrentState == SERVICE_STOPPED) continue;
        if (s.dwProcessId != 0) ++(*out)[s.dwProcessId];
    }
    return true;
}

// 单个进程的路径 + 句柄派生标志位。`services` / `titles` 按
// 刷新周期（每第 5 个 tick）预计算并从缓存复用。
ProcessCollector::Supp ComputeSupp(uint32_t pid,
                                   const std::unordered_map<uint32_t, uint32_t>& services,
                                   const std::unordered_map<uint32_t, std::wstring>& titles) {
    ProcessCollector::Supp s;
    UniqueHandle h(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!h) {
        if (::GetLastError() == ERROR_ACCESS_DENIED) s.flags |= PF_AccessDenied;
    } else {
        wchar_t buf[1024];
        DWORD n = static_cast<DWORD>(std::size(buf));
        if (!::QueryFullProcessImageNameW(h.get(), 0, buf, &n)) {
            if (::GetLastError() == ERROR_ACCESS_DENIED) s.flags |= PF_AccessDenied;
        } else {
            s.path.assign(buf, n);
        }
        if (IsElevatedProcess(h.get())) s.flags |= PF_Elevated;
        USHORT pm = IMAGE_FILE_MACHINE_UNKNOWN, native = IMAGE_FILE_MACHINE_UNKNOWN;
        if (::IsWow64Process2(h.get(), &pm, &native) && pm != IMAGE_FILE_MACHINE_UNKNOWN) {
            s.flags |= PF_Wow64;
        }
        if (IsPackagedProcess(h.get())) s.flags |= PF_Uwp;
    }
    if (services.count(pid) != 0) s.flags |= PF_ServiceHost;
    const auto tit = titles.find(pid);
    if (tit != titles.end()) {
        s.flags |= PF_HasWindow;
        s.title = tit->second;
    }
    return s;
}

// Toolhelp+PSAPI 慢路径行（兼容模式）。PSAPI 提供不了的字段
//（上下文切换）保持未知 -> 下游为 kUnavail。
bool FillToolhelpRows(std::vector<RawRow>* rows) {
    std::vector<ToolhelpRow> th;
    if (!ToolhelpEnumerate(&th)) return false;
    rows->clear();
    rows->reserve(th.size());
    for (const ToolhelpRow& t : th) {
        RawRow r;
        r.pid = t.pid;
        r.parentPid = t.parentPid;
        r.threads = t.threads;
        r.name = t.name;
        DWORD sid = 0;
        if (::ProcessIdToSessionId(t.pid, &sid)) r.sessionId = sid;
        UniqueHandle h(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, t.pid));
        if (h) {
            FILETIME ct{}, ex{}, kt{}, ut{};
            if (::GetProcessTimes(h.get(), &ct, &ex, &kt, &ut)) {
                r.createTime = FtU64(ct);
                r.kernelTime = FtU64(kt);
                r.userTime = FtU64(ut);
                r.timesKnown = true;
            }
            PROCESS_MEMORY_COUNTERS_EX mc{};
            mc.cb = sizeof(mc);
            if (::GetProcessMemoryInfo(h.get(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mc),
                                       sizeof(mc))) {
                r.workingSet = mc.WorkingSetSize;
                r.wsKnown = true;
                r.commitBytes = mc.PrivateUsage;
                r.commitKnown = true;
                r.pageFaults = mc.PageFaultCount;
                r.pfKnown = true;
            }
            IO_COUNTERS io{};
            if (::GetProcessIoCounters(h.get(), &io)) {
                r.ioRead = io.ReadTransferCount;
                r.ioWrite = io.WriteTransferCount;
                r.ioOther = io.OtherTransferCount;
                r.ioKnown = true;
            }
            DWORD hc = 0;
            if (::GetProcessHandleCount(h.get(), &hc)) r.handles = hc;
        }
        rows->push_back(std::move(r));
    }
    return true;
}

}  // namespace

void ProcessCollector::Collect(uint64_t tickId, bool degraded, TickOut* out) {
    const auto now = Clock::now();
    double elapsed = 0;
    if (haveLast_) elapsed = std::chrono::duration<double>(now - lastCollect_).count();
    lastCollect_ = now;
    haveLast_ = true;
    out->elapsedSec = elapsed;

    SYSTEM_INFO si{};
    ::GetNativeSystemInfo(&si);
    const double cores = si.dwNumberOfProcessors > 0 ? static_cast<double>(si.dwNumberOfProcessors)
                                                     : 1.0;

    // --- 1. 原始行：NtQSI 快路径，Toolhelp 兜底 ----------------------------
    std::vector<RawRow> rows;
    bool ntsi = false;
    if (!degraded) {
        std::vector<NtProcRow> nt;
        if (NtqsiQueryProcesses(&nt)) {
            ntsi = true;
            rows.reserve(nt.size());
            for (const NtProcRow& n : nt) {
                RawRow r;
                r.pid = n.pid;
                r.parentPid = n.parentPid;
                r.sessionId = n.sessionId;
                r.handles = n.handles;
                r.threads = n.threads;
                r.createTime = n.createTime;
                r.kernelTime = n.kernelTime;
                r.userTime = n.userTime;
                r.workingSet = n.workingSet;
                r.privateWs = n.privateWs;  // WorkingSetPrivateSize（有符号；见下文）
                r.commitBytes = n.privateCommit;  // PrivatePageCount = 提交大小
                r.pageFaults = n.pageFaults;
                r.ioRead = n.ioReadBytes;
                r.ioWrite = n.ioWriteBytes;
                r.ioOther = n.ioOtherBytes;
                r.ctxSw = n.ctxSwitches;
                r.timesKnown = r.wsKnown = r.commitKnown = r.pfKnown = true;
                r.pwsKnown = true;
                r.ioKnown = r.ctxKnown = true;
                r.suspended = n.allThreadsSuspended;
                r.name = n.name;
                rows.push_back(std::move(r));
            }
        }
    }
    if (!ntsi) FillToolhelpRows(&rows);
    std::sort(rows.begin(), rows.end(),
              [](const RawRow& a, const RawRow& b) { return a.pid < b.pid; });

    // --- 2. 补充信息缓存 ---------------------------------------------------
    // 整表刷新（服务/窗口标题）每第 5 个 tick 一次；每进程的
    // 句柄派生信息则切片进行：每 tick 处理列表的 1/5，
    // 单个 tick 绝不会承受完整的 OpenProcess 风暴。
    const bool suppCycle = (tickId % 5 == 1);
    if (suppCycle) {
        EnumRunningServiceHosts(&servicesByPid_);
        titlesByPid_.clear();
        ::EnumWindows(EnumWindowTitleProc, reinterpret_cast<LPARAM>(&titlesByPid_));
    }
    const size_t slice = static_cast<size_t>(tickId % 5);

    // --- 3. 组装 ProcInfo --------------------------------------------------
    const size_t n = rows.size();
    std::unordered_map<uint32_t, Prev> newPrev;
    newPrev.reserve(n * 2);
    std::set<ProcKey> newDenied;
    std::unordered_set<ProcKey> seen;
    seen.reserve(n * 2);

    out->procs.clear();
    out->procs.reserve(n);
    out->createTimeByPid.clear();
    out->createTimeByPid.reserve(n);
    Totals totals{};

    for (size_t i = 0; i < n; ++i) {
        const RawRow& row = rows[i];
        const ProcKey key{row.pid, row.createTime};

        // 补充信息：无缓存时计算，否则刷新这 1/5 切片。
        auto sit = supp_.find(key);
        if (sit == supp_.end() || (i % 5) == slice) {
            supp_[key] = ComputeSupp(row.pid, servicesByPid_, titlesByPid_);
            sit = supp_.find(key);
        }
        seen.insert(key);
        if ((sit->second.flags & PF_AccessDenied) != 0) newDenied.insert(key);
        const bool stickyDenied = (denied_.count(key) != 0) || newDenied.count(key) != 0;

        // 差值字段以 (pid, createTime) 为键：复用的 PID 创建时间不同，
        // 因此重置基线而不是撒谎。
        double cpu = kUnavail, disk = kUnavail, pfps = kUnavail, csps = kUnavail;
        const auto pit = prev_.find(row.pid);
        if (pit != prev_.end() && row.createTime != 0 && pit->second.createTime == row.createTime &&
            elapsed > 0.0) {
            if (row.timesKnown && pit->second.execKnown) {
                const double dExec = static_cast<double>(row.kernelTime + row.userTime -
                                                         pit->second.execTime);
                cpu = 100.0 * dExec / (elapsed * 1e7 * cores);  // 100ns -> 秒，按全部核心归一
            }
            if (row.ioKnown && pit->second.ioKnown) {
                const double dIo = static_cast<double>(row.ioRead + row.ioWrite + row.ioOther -
                                                       pit->second.ioBytes);
                disk = dIo / elapsed;
            }
            if (row.pfKnown && pit->second.pfKnown) {
                pfps = static_cast<double>(row.pageFaults - pit->second.pageFaults) / elapsed;
            }
            if (row.ctxKnown && pit->second.ctxKnown) {
                csps = static_cast<double>(row.ctxSw - pit->second.ctx) / elapsed;
            }
        }
        Prev pv;
        pv.createTime = row.createTime;
        pv.execTime = row.kernelTime + row.userTime;
        pv.ioBytes = row.ioRead + row.ioWrite + row.ioOther;
        pv.pageFaults = row.pageFaults;
        pv.ctx = row.ctxSw;
        pv.execKnown = row.timesKnown;
        pv.ioKnown = row.ioKnown;
        pv.pfKnown = row.pfKnown;
        pv.ctxKnown = row.ctxKnown;
        newPrev[row.pid] = pv;

        ProcInfo p;
        p.key = key;
        p.parentPid = row.parentPid;
        p.sessionId = row.sessionId;
        p.name = row.name;
        p.path = sit->second.path;
        p.kernelTime = row.timesKnown ? row.kernelTime : kUnavailU64;
        p.userTime = row.timesKnown ? row.userTime : kUnavailU64;
        p.cpuPercent = cpu;
        p.workingSet = row.wsKnown ? row.workingSet : kUnavailU64;
        // 私有工作集：快路径直接取自 NtQSI WorkingSetPrivateSize
        //（来源已由 SelfCheckGate 第 6 项校验一次）。
        // 负估计与慢路径上报 kUnavailU64。
        p.privateWorkingSet =
            (row.pwsKnown && row.privateWs >= 0) ? static_cast<uint64_t>(row.privateWs)
                                                 : kUnavailU64;
        p.commitBytes = row.commitKnown ? row.commitBytes : kUnavailU64;
        p.ioReadBytes = row.ioKnown ? row.ioRead : kUnavailU64;
        p.ioWriteBytes = row.ioKnown ? row.ioWrite : kUnavailU64;
        p.diskBytesPerSec = disk;
        p.netBytesPerSec = kUnavail;  // 仅 ETW（第 3 阶段）填充此字段
        p.pageFaultsPerSec = pfps;
        p.contextSwitchesPerSec = csps;
        p.handles = row.handles;
        p.threads = row.threads;
        p.flags = sit->second.flags;
        if (stickyDenied) p.flags |= PF_AccessDenied;
        MarkProtectedFlag(row.pid, row.name, p.path, &p.flags);
        if (row.suspended) p.flags |= PF_Suspended;
        p.windowTitle = sit->second.title;
        out->procs.push_back(std::move(p));

        out->createTimeByPid[row.pid] = row.createTime;
        ++totals.procCount;
        totals.handleTotal += row.handles;
        totals.threadTotal += row.threads;
    }

    // --- 4. 修剪状态，复用/死亡的 pid 绝不泄漏 ------------------------------
    for (auto it = supp_.begin(); it != supp_.end();) {
        if (seen.count(it->first) == 0) {
            it = supp_.erase(it);
        } else {
            ++it;
        }
    }
    denied_ = newDenied;  // 进程存活期内粘滞，退出时修剪
    prev_ = std::move(newPrev);

    out->totals = totals;
    out->ntsiOk = ntsi;
}

}  // namespace cd
}  // namespace stm
