// SystemCollector: system-wide metrics per tick.
// CPU: GetSystemTimes for the total (kernel time includes idle -> busy =
// kernel-idle+user) and NtQSI(SystemProcessorPerformanceInformation) per core.
// Memory: GlobalMemoryStatusEx + GetPerformanceInfo. Disk rates + system hard
// faults: PDH English counters. Network: GetIfTable2 octet deltas. All fields
// that cannot be collected stay kUnavail — never 0-by-convention.
#include <winsock2.h>  // must precede iphlpapi/netioapi (LEAN_AND_MEAN hides winsock)
#include <ws2tcpip.h>  // pulls ws2ipdef.h -> defines _WS2IPDEF_ for netioapi MIB_* decls
#include "collect/CollectDetail.h"
#include "core/Log.h"
#include <iphlpapi.h>
#include <ipifcons.h>
#include <netioapi.h>
#include <chrono>
#include <psapi.h>

namespace stm {
namespace cd {

namespace {

constexpr wchar_t kDiskRead[] = L"\\PhysicalDisk(*)\\Disk Read Bytes/sec";
constexpr wchar_t kDiskWrite[] = L"\\PhysicalDisk(*)\\Disk Write Bytes/sec";
constexpr wchar_t kHardFaults[] = L"\\Memory\\Hard Faults/sec";

uint64_t FtU64(const FILETIME& f) {
    return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

bool IsTotalInstance(const std::wstring& inst) { return inst == L"_Total"; }

}  // namespace

// PDH disk rates + hard faults. All counters are wildcard/plain English paths
// added once; the disk instance set is tracked by PDH itself and read back via
// the formatted array (no rebuild). Rate counters need two collects inside the
// current query -> kUnavail on the first tick, which the UI renders as "—".
void SystemCollector::CollectPdh(SystemInfo* out) {
    if (pdhFailed_) return;
    if (pdhQuery_ == nullptr) {
        std::wstring readTemplate, writeTemplate;
        if (!PdhLocalizeEnglishPath(kDiskRead, &readTemplate) ||
            !PdhLocalizeEnglishPath(kDiskWrite, &writeTemplate) ||
            PdhOpenQueryW(nullptr, 0, &pdhQuery_) != ERROR_SUCCESS ||
            !PdhAddWildcardCounter(pdhQuery_, readTemplate, &diskRead_) ||
            !PdhAddWildcardCounter(pdhQuery_, writeTemplate, &diskWrite_)) {
            pdhFailed_ = true;
            PdhCloseQuerySafe(&pdhQuery_);
            diskRead_ = diskWrite_ = nullptr;
            if (!pdhLogged_) {
                pdhLogged_ = true;
                STM_LOG_WARN("collect", L"PDH PhysicalDisk 计数器不可用，磁盘速率显示为 —");
            }
            return;
        }
        // Hard faults degrade to kUnavail alone when this one path fails.
        std::wstring hfTemplate;
        if (PdhLocalizeEnglishPath(kHardFaults, &hfTemplate)) {
            PdhAddWildcardCounter(pdhQuery_, hfTemplate, &hardFaults_);
        }
    }
    if (!PdhCollect(pdhQuery_)) return;

    // Sum per-disk rates; skip "_Total" to avoid double counting, but fall
    // back to it when no per-disk instance reports a value.
    std::vector<PdhArrayItem> reads, writes;
    if (!PdhFmtArrayDouble(diskRead_, &reads) || !PdhFmtArrayDouble(diskWrite_, &writes)) return;
    double readSum = 0, writeSum = 0, totalRead = 0, totalWrite = 0;
    bool sawDisk = false, sawTotal = false;
    for (const PdhArrayItem& it : reads) {
        if (!it.valid) continue;
        if (IsTotalInstance(it.name)) {
            totalRead = it.value;
            sawTotal = true;
        } else {
            readSum += it.value;
            sawDisk = true;
        }
    }
    for (const PdhArrayItem& it : writes) {
        if (!it.valid) continue;
        if (IsTotalInstance(it.name)) {
            totalWrite = it.value;
        } else {
            writeSum += it.value;
        }
    }
    if (sawDisk || sawTotal) {
        out->diskReadBps = sawDisk ? readSum : totalRead;
        out->diskWriteBps = sawDisk ? writeSum : totalWrite;
    }
    double hf = 0;
    if (hardFaults_ && PdhFmtDouble(hardFaults_, &hf)) {
        out->hardFaultsPerSec = hf;  // honest kUnavail when unavailable
    }
}

// Network throughput from GetIfTable2 octet deltas over all non-loopback,
// operational interfaces.
void SystemCollector::CollectNet(SystemInfo* out, double elapsedSec) {
    MIB_IF_TABLE2* tbl = nullptr;
    if (::GetIfTable2(&tbl) != NO_ERROR) return;  // keep previous/kUnavail values
    uint64_t recv = 0, send = 0;
    for (ULONG i = 0; i < tbl->NumEntries; ++i) {
        const MIB_IF_ROW2& r = tbl->Table[i];
        if (r.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (r.OperStatus != IfOperStatusUp) continue;
        recv += r.InOctets;
        send += r.OutOctets;
    }
    ::FreeMibTable(tbl);
    if (haveNetPrev_ && elapsedSec > 0.0) {
        out->netRecvBps = static_cast<double>(recv - prevRecv_) / elapsedSec;
        out->netSendBps = static_cast<double>(send - prevSend_) / elapsedSec;
    }
    prevRecv_ = recv;
    prevSend_ = send;
    haveNetPrev_ = true;
}

void SystemCollector::Collect(const ProcessCollector::TickOut& pt, SystemInfo* out) {
    const double elapsedSec = pt.elapsedSec;

    // --- CPU total: GetSystemTimes (kernel includes idle) ------------------
    FILETIME ftIdle{}, ftKernel{}, ftUser{};
    if (::GetSystemTimes(&ftIdle, &ftKernel, &ftUser)) {
        const uint64_t idle = FtU64(ftIdle), kern = FtU64(ftKernel), user = FtU64(ftUser);
        if (haveCpuPrev_ && elapsedSec > 0.0) {
            const uint64_t dK = kern - prevKernel_, dU = user - prevUser_, dI = idle - prevIdle_;
            const uint64_t denom = dK + dU;  // kernel time already contains idle
            if (denom > 0) {
                out->cpuTotalPercent =
                    100.0 * static_cast<double>(dK - dI + dU) / static_cast<double>(denom);
            }
        }
        prevIdle_ = idle;
        prevKernel_ = kern;
        prevUser_ = user;
        haveCpuPrev_ = true;
    }

    // --- CPU per core: NtQSI SystemProcessorPerformanceInformation ---------
    // Kernel includes idle here too, so busy = kernel - idle + user.
    std::vector<CoreTimes> coresNow;
    if (NtqsiQueryPerCoreTimes(&coresNow)) {
        out->perCorePercent.assign(coresNow.size(), kUnavail);
        if (haveCorePrev_ && prevCore_.size() == coresNow.size() && elapsedSec > 0.0) {
            for (size_t i = 0; i < coresNow.size(); ++i) {
                const uint64_t dK = coresNow[i].kernel - prevCore_[i].kernel;
                const uint64_t dU = coresNow[i].user - prevCore_[i].user;
                const uint64_t dI = coresNow[i].idle - prevCore_[i].idle;
                const uint64_t denom = dK + dU;
                if (denom > 0) {
                    out->perCorePercent[i] =
                        100.0 * static_cast<double>(dK - dI + dU) / static_cast<double>(denom);
                }
            }
        }
        prevCore_ = std::move(coresNow);
        haveCorePrev_ = true;
    }

    // --- Physical memory ----------------------------------------------------
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (::GlobalMemoryStatusEx(&ms)) {
        out->physTotal = ms.ullTotalPhys;
        out->physAvail = ms.ullAvailPhys;
    }

    // --- Commit / kernel memory + counts fallback ---------------------------
    // NtQSI(SystemPerformanceInformation) is ~0.1ms; GetPerformanceInfo costs
    // ~10ms on this machine and is kept only as the degraded-path fallback.
    uint64_t pageSize = 4096;
    {
        SYSTEM_INFO si{};
        ::GetNativeSystemInfo(&si);
        if (si.dwPageSize) pageSize = si.dwPageSize;
    }
    SysMemCounters mc{};
    bool haveMem = NtqsiQueryMemoryCounters(&mc);
    if (haveMem) {
        out->commitTotal = mc.commitPages * pageSize;
        out->commitLimit = mc.commitLimitPages * pageSize;
        out->kernPaged = mc.pagedPoolPages * pageSize;
        out->kernNonpaged = mc.nonPagedPoolPages * pageSize;
    }
    bool perfInfo = false;
    PERFORMANCE_INFORMATION pi{};
    if (!haveMem || !pt.ntsiOk) {
        pi.cb = sizeof(pi);
        perfInfo = ::GetPerformanceInfo(&pi, sizeof(pi)) != 0;
        if (perfInfo && !haveMem) {
            out->commitTotal = pi.CommitTotal * pi.PageSize;
            out->commitLimit = pi.CommitLimit * pi.PageSize;
            out->kernPaged = pi.KernelPaged * pi.PageSize;
            out->kernNonpaged = pi.KernelNonpaged * pi.PageSize;
        }
    }
    if (pt.ntsiOk) {
        // Aggregated from the same NtQSI pass as the process list (spec).
        out->procCount = pt.totals.procCount;
        out->handleTotal = pt.totals.handleTotal;
        out->threadTotal = pt.totals.threadTotal;
    } else if (perfInfo) {
        out->procCount = pi.ProcessCount;
        out->handleTotal = pi.HandleCount;
        out->threadTotal = pi.ThreadCount;
    }

    // --- PDH (disk + hard faults) and network -------------------------------
    CollectPdh(out);
    CollectNet(out, elapsedSec);

    // --- Uptime --------------------------------------------------------------
    out->uptimeSec = static_cast<double>(::GetTickCount64()) / 1000.0;
}

}  // namespace cd
}  // namespace stm
