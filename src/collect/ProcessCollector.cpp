// ProcessCollector: per-tick process snapshot.
// Fast path: one NtQuerySystemInformation(SystemProcessInformation) call yields
// pid/ppid/create time/CPU times/working set/private working set/IO/handles/
// threads/page faults and per-thread context switches for the whole system.
// Degraded path: Toolhelp + PSAPI (compatibility mode; contextSwitchesPerSec
// and privateWorkingSet stay kUnavail there).
// Delta fields (cpuPercent / diskBytesPerSec / pageFaultsPerSec /
// contextSwitchesPerSec) use (pid, createTime) identity so a recycled PID can
// never produce a bogus delta; the first tick has no baseline -> kUnavail.
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

// Normalized per-process row: identical shape for the NtQSI and Toolhelp paths
// so delta computation / flag assembly / ProcInfo assembly is shared.
struct RawRow {
    uint32_t pid = 0, parentPid = 0, sessionId = 0, handles = 0, threads = 0;
    uint64_t createTime = 0, kernelTime = 0, userTime = 0;
    uint64_t workingSet = 0, commitBytes = 0, pageFaults = 0;
    int64_t privateWs = 0;  // private working set (bytes); valid only when pwsKnown
    uint64_t ioRead = 0, ioWrite = 0, ioOther = 0, ctxSw = 0;
    bool timesKnown = false, wsKnown = false, commitKnown = false;
    bool pfKnown = false, ioKnown = false, ctxKnown = false, pwsKnown = false;
    bool suspended = false;
    std::wstring name;
};

// --- supplementary info helpers (path + flags), shared by both paths ---------

bool IsElevatedProcess(HANDLE h) {
    HANDLE tok = nullptr;
    if (!::OpenProcessToken(h, TOKEN_QUERY, &tok)) return false;
    UniqueHandle tokGuard(tok);
    TOKEN_ELEVATION e{};
    DWORD ret = 0;
    if (!::GetTokenInformation(tokGuard.get(), TokenElevation, &e, sizeof(e), &ret)) return false;
    return e.TokenIsElevated != 0;
}

// True when the process runs under an AppContainer package (UWP). Two-call
// protocol: null buffer -> required length; plain Win32 processes fail with
// APPMODEL_ERROR_NO_PACKAGE (R5 #4b).
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
    // For other-process windows this reads the cached caption; it never blocks.
    if (::GetWindowTextW(hwnd, buf, 511) <= 0) return TRUE;
    out->emplace(pid, buf);  // z-order enumeration: first (top) visible window wins
    return TRUE;
}

// pid -> count of running services hosted (R5 #7b: dwProcessId is only valid
// for non-stopped services, and only SERVICE_WIN32 services carry a pid).
bool EnumRunningServiceHosts(std::unordered_map<uint32_t, uint32_t>* out) {
    out->clear();
    // EnumServicesStatusExW requires a real SCM handle (NULL fails with
    // ERROR_INVALID_HANDLE); SC_MANAGER_ENUMERATE_SERVICE is grantee to
    // non-admin callers.
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

// Path + handle-derived flag bits for one process. `services` / `titles` are
// pre-computed per refresh cycle (every 5th tick) and reused from cache.
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

// Toolhelp+PSAPI slow path rows (compatibility mode). Fields that PSAPI cannot
// provide (context switches) stay unknown -> kUnavail downstream.
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

    // --- 1. raw rows: NtQSI fast path, Toolhelp fallback -------------------
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
                r.privateWs = n.privateWs;  // WorkingSetPrivateSize (signed; see below)
                r.commitBytes = n.privateCommit;  // PrivatePageCount = commit size
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

    // --- 2. supplementary caches -------------------------------------------
    // Whole-map refreshes (services / window titles) every 5th tick; the
    // per-process handle-derived info is sliced: 1/5 of the list per tick so a
    // single tick never pays the full OpenProcess storm.
    const bool suppCycle = (tickId % 5 == 1);
    if (suppCycle) {
        EnumRunningServiceHosts(&servicesByPid_);
        titlesByPid_.clear();
        ::EnumWindows(EnumWindowTitleProc, reinterpret_cast<LPARAM>(&titlesByPid_));
    }
    const size_t slice = static_cast<size_t>(tickId % 5);

    // --- 3. assemble ProcInfo ----------------------------------------------
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

        // Supplementary: compute when uncached, else refresh this 1/5 slice.
        auto sit = supp_.find(key);
        if (sit == supp_.end() || (i % 5) == slice) {
            supp_[key] = ComputeSupp(row.pid, servicesByPid_, titlesByPid_);
            sit = supp_.find(key);
        }
        seen.insert(key);
        if ((sit->second.flags & PF_AccessDenied) != 0) newDenied.insert(key);
        const bool stickyDenied = (denied_.count(key) != 0) || newDenied.count(key) != 0;

        // Delta fields keyed by (pid, createTime): PID reuse has a different
        // create time and therefore resets the baseline instead of lying.
        double cpu = kUnavail, disk = kUnavail, pfps = kUnavail, csps = kUnavail;
        const auto pit = prev_.find(row.pid);
        if (pit != prev_.end() && row.createTime != 0 && pit->second.createTime == row.createTime &&
            elapsed > 0.0) {
            if (row.timesKnown && pit->second.execKnown) {
                const double dExec = static_cast<double>(row.kernelTime + row.userTime -
                                                         pit->second.execTime);
                cpu = 100.0 * dExec / (elapsed * 1e7 * cores);  // 100ns -> sec, all-core
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
        // Private working set: straight from NtQSI WorkingSetPrivateSize on the
        // fast path (source cross-validated once by SelfCheckGate item 6).
        // Negative estimates and the slow path report kUnavailU64.
        p.privateWorkingSet =
            (row.pwsKnown && row.privateWs >= 0) ? static_cast<uint64_t>(row.privateWs)
                                                 : kUnavailU64;
        p.commitBytes = row.commitKnown ? row.commitBytes : kUnavailU64;
        p.ioReadBytes = row.ioKnown ? row.ioRead : kUnavailU64;
        p.ioWriteBytes = row.ioKnown ? row.ioWrite : kUnavailU64;
        p.diskBytesPerSec = disk;
        p.netBytesPerSec = kUnavail;  // only ETW (phase 3) fills this
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

    // --- 4. prune state so recycled/dead pids never leak --------------------
    for (auto it = supp_.begin(); it != supp_.end();) {
        if (seen.count(it->first) == 0) {
            it = supp_.erase(it);
        } else {
            ++it;
        }
    }
    denied_ = newDenied;  // sticky within process lifetime, pruned on exit
    prev_ = std::move(newPrev);

    out->totals = totals;
    out->ntsiOk = ntsi;
}

}  // namespace cd
}  // namespace stm
