#pragma once
// INTERNAL header for the stm_collect implementation — NOT a contract header.
// Only src/collect/CollectService.h is architect-owned and frozen; everything
// declared here is private to the collection library and may change freely.
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <chrono>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "core/ProcData.h"

namespace stm {
namespace cd {

// ---------------------------------------------------------------------------
// NtQuerySystemInformation (ntdll loaded dynamically; structure layout per the
// NtDoc / Geoff Chappell x64 layout, static-asserted and *verified at runtime*
// by the SelfCheckGate before the fast path is ever trusted).
// ---------------------------------------------------------------------------
struct NtProcRow {
    uint32_t pid = 0, parentPid = 0, sessionId = 0;
    uint32_t handles = 0, threads = 0;
    uint64_t createTime = 0, kernelTime = 0, userTime = 0;  // cumulative, 100ns
    uint64_t workingSet = 0;                                // bytes
    int64_t privateWs = 0;  // WorkingSetPrivateSize (bytes; may be negative on some builds)
    uint64_t privateCommit = 0;                             // PrivatePageCount (= PrivateUsage)
    uint64_t pageFaults = 0;                                // cumulative count
    uint64_t ioReadBytes = 0, ioWriteBytes = 0, ioOtherBytes = 0;  // transfer counts
    uint64_t ctxSwitches = 0;                               // thread ContextSwitches sum
    bool allThreadsSuspended = false;                       // every thread Waiting+Suspended
    std::wstring name;
};
// One full-system snapshot. Returns false when ntdll/NtQSI is unavailable.
bool NtqsiQueryProcesses(std::vector<NtProcRow>* out);

struct CoreTimes { uint64_t idle = 0, kernel = 0, user = 0; };  // 100ns; kernel includes idle
// NtQSI(SystemProcessorPerformanceInformation) — one entry per logical core.
bool NtqsiQueryPerCoreTimes(std::vector<CoreTimes>* out);

// Commit / kernel memory counters from NtQSI(SystemPerformanceInformation) —
// replaces the ~10ms GetPerformanceInfo call in the tick path (values in PAGES;
// multiply by page size). Cross-checked against GetPerformanceInfo on this
// machine (commit/limit/paged/nonpaged all match).
struct SysMemCounters {
    uint64_t commitPages = 0, commitLimitPages = 0, pagedPoolPages = 0, nonPagedPoolPages = 0;
};
bool NtqsiQueryMemoryCounters(SysMemCounters* out);

// ---------------------------------------------------------------------------
// PDH helpers. Wildcard-array pattern (R5 #9b + MSDN "Enumerating Object
// Instances"): PdhAddEnglishCounterW resolves the language-neutral name and
// PdhGetCounterInfo yields the LOCALIZED wildcard template, which is added to
// the live query ONCE. After each PdhCollectQueryData, PdhGetFormattedCounterArrayW
// returns one value per live instance with the full "#N"-suffixed instance
// name — the instance set is tracked by PDH itself, so no rebuild is needed.
// ---------------------------------------------------------------------------
struct PdhArrayItem {
    std::wstring name;   // instance name, e.g. "svchost#76" / "pid_1_luid_0x.._phys_0"
    double value = 0.0;
    bool valid = false;  // false when PDH has no valid sample yet (rate warm-up)
};
// Resolve a language-neutral English path to the localized template (once per
// counter, at collector init). False = counter set unavailable (hard failure).
bool PdhLocalizeEnglishPath(const wchar_t* englishPath, std::wstring* localizedTemplate);
// Add the localized wildcard template as a counter of `query`.
bool PdhAddWildcardCounter(PDH_HQUERY query, const std::wstring& localizedTemplate,
                           PDH_HCOUNTER* out);
inline void PdhCloseQuerySafe(PDH_HQUERY* q) {
    if (q && *q) { PdhCloseQuery(*q); *q = nullptr; }
}
bool PdhCollect(PDH_HQUERY q);
// All instance values of a wildcard counter. Items with valid=false had no
// usable sample yet (rate counters need two collects; fresh instances).
bool PdhFmtArrayDouble(PDH_HCOUNTER h, std::vector<PdhArrayItem>* out);
// Single (non-wildcard) counter value; false when no valid sample yet.
bool PdhFmtDouble(PDH_HCOUNTER h, double* out);

// ---------------------------------------------------------------------------
// Toolhelp fallback used by the degraded (compatibility) path.
// ---------------------------------------------------------------------------
struct ToolhelpRow {
    uint32_t pid = 0, parentPid = 0, threads = 0;
    std::wstring name;
};
bool ToolhelpEnumerate(std::vector<ToolhelpRow>* out);
// Thread count for one pid (own snapshot; used sparingly — self-check gate).
uint32_t ToolhelpThreadCountOf(uint32_t pid);

// ---------------------------------------------------------------------------
// Collectors. State lives in these objects; methods are defined in the
// matching .cpp translation units (ProcessCollector.cpp etc).
// ---------------------------------------------------------------------------
class ProcessCollector {
public:
    struct Totals { uint32_t procCount = 0, handleTotal = 0, threadTotal = 0; };
    struct TickOut {
        std::vector<ProcInfo> procs;                 // ascending by pid
        Totals totals;                               // NtQSI aggregation (valid when ntsiOk)
        double elapsedSec = 0;                       // since previous Collect (0 on first)
        bool ntsiOk = false;                         // true = NtQSI fast path produced this data
        // pid -> createTime of THIS tick's processes; used by GpuCollector to map
        // GPU counter pids onto (pid, createTime) identities. Unmatched pids drop.
        std::unordered_map<uint32_t, uint64_t> createTimeByPid;
    };
    // tickId drives the 1/5-sliced supplementary refresh; degraded switches to
    // the Toolhelp+PSAPI slow path. Never throws.
    void Collect(uint64_t tickId, bool degraded, TickOut* out);

    struct Supp {  // cached supplementary info, refreshed 1/5 of procs per tick
        std::wstring path, title;
        uint32_t flags = 0;
    };

private:
    struct Prev {  // previous-tick cumulatives for delta fields (pid keyed)
        uint64_t createTime = 0, execTime = 0, ioBytes = 0, pageFaults = 0, ctx = 0;
        bool execKnown = false, ioKnown = false, pfKnown = false, ctxKnown = false;
    };
    std::unordered_map<uint32_t, Prev> prev_;
    std::set<ProcKey> denied_;   // sticky PF_AccessDenied, keyed by (pid, createTime)
    std::unordered_map<ProcKey, Supp> supp_;
    std::unordered_map<uint32_t, uint32_t> servicesByPid_;  // pid -> running svc count
    std::unordered_map<uint32_t, std::wstring> titlesByPid_;
    std::chrono::steady_clock::time_point lastCollect_{};
    bool haveLast_ = false;
    // privateWorkingSet comes straight from NtQSI WorkingSetPrivateSize on the
    // fast path (validated once by SelfCheckGate item 6 against a one-shot PDH
    // read); no per-tick PDH process query exists. The slow path leaves it
    // kUnavailU64.
};

class SystemCollector {
public:
    SystemCollector() = default;
    ~SystemCollector();  // closes the PDH query (review V7-P1-1)
    SystemCollector(const SystemCollector&) = delete;
    SystemCollector& operator=(const SystemCollector&) = delete;
    // pt supplies the NtQSI aggregate totals + elapsed seconds for rates.
    void Collect(const ProcessCollector::TickOut& pt, SystemInfo* out);

private:
    void CollectPdh(SystemInfo* out);
    void CollectNet(SystemInfo* out, double elapsedSec);
    uint64_t prevIdle_ = 0, prevKernel_ = 0, prevUser_ = 0;
    bool haveCpuPrev_ = false;
    std::vector<CoreTimes> prevCore_;
    bool haveCorePrev_ = false;
    uint64_t prevRecv_ = 0, prevSend_ = 0;  // GetIfTable2 octet counters
    bool haveNetPrev_ = false;
    // PDH: disk rates (wildcard; PDH tracks the instance set itself) +
    // system-wide hard faults (plain counter).
    PDH_HQUERY pdhQuery_ = nullptr;
    PDH_HCOUNTER diskRead_ = nullptr, diskWrite_ = nullptr, hardFaults_ = nullptr;
    bool pdhFailed_ = false, pdhLogged_ = false;
};

class GpuCollector {
public:
    GpuCollector() = default;
    ~GpuCollector();  // closes the PDH query (review V7-P1-1)
    GpuCollector(const GpuCollector&) = delete;
    GpuCollector& operator=(const GpuCollector&) = delete;
    // Gathers DXGI adapters + PDH GPU Engine / GPU Process Memory counters.
    // createTimeByPid: this tick's process identities for pid matching (R5 #11:
    // GPU counter instances carry pid only; pids absent from the snapshot drop).
    void Collect(const std::unordered_map<uint32_t, uint64_t>& createTimeByPid,
                 std::vector<GpuProcUsage>* procsOut,
                 std::vector<GpuAdapterInfo>* adaptersOut,
                 double* queryMs);
private:
    struct Agg {
        double util = 0; int utilN = 0;
        uint64_t ded = 0, shr = 0;
        bool hasDed = false, hasShr = false;
    };
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER utilCounter_ = nullptr, dedCounter_ = nullptr, shrCounter_ = nullptr;
    bool pdhFailed_ = false, pdhLogged_ = false;
};

// ---------------------------------------------------------------------------
// SelfCheckGate: at startup, cross-validate the NtQSI fast-path readings of up
// to 3 live processes against GetProcessTimes / GetProcessMemoryInfo /
// GetProcessIoCounters / GetProcessHandleCount / Toolhelp thread count, plus
// WorkingSetPrivateSize vs a one-shot PDH "Working Set - Private" read
// (arch section 4 + architect ruling). Any tolerance violation (time ±1s,
// bytes ±25%, counters ±10%) degrades the whole service to the Toolhelp+PSAPI
// slow path instead of emitting wrong data.
// ---------------------------------------------------------------------------
struct GateResult { bool degraded = false; std::wstring reason; };
GateResult RunSelfCheckGate();

}  // namespace cd
}  // namespace stm
