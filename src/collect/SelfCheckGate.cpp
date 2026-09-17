// SelfCheckGate (arch section 4): before the NtQSI fast path is trusted, its
// semi-documented field offsets are cross-validated against documented APIs on
// live processes. Any tolerance violation degrades the whole service to the
// Toolhelp+PSAPI compatibility path instead of emitting wrong data.
#include "collect/CollectDetail.h"
#include "core/HandleGuard.h"
#include "core/Str.h"
#include <psapi.h>
#include <algorithm>
#include <unordered_map>
#include <cstdlib>

namespace stm {
namespace cd {

namespace {

constexpr int64_t k100nsPerSec = 10'000'000LL;

uint64_t FtU64(const FILETIME& f) {
    return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

// Relative tolerance + absolute slack. The slack absorbs the small sampling
// skew between NtQSI and the per-process API read (the two calls are
// microseconds apart but the target process keeps running).
bool WithinRelative(uint64_t a, uint64_t b, double rel, uint64_t slack) {
    const uint64_t hi = a > b ? a : b;
    const uint64_t diff = a > b ? a - b : b - a;
    const double tol = rel * static_cast<double>(hi) + static_cast<double>(slack);
    return static_cast<double>(diff) <= tol;
}

}  // namespace

GateResult RunSelfCheckGate() {
    std::vector<NtProcRow> rows;
    if (!NtqsiQueryProcesses(&rows) || rows.empty()) {
        return {true, L"NtQuerySystemInformation 不可用，已切换 Toolhelp+PSAPI 兼容模式"};
    }

    int validated = 0;
    for (const NtProcRow& r : rows) {
        if (validated >= 3) break;
        if (r.pid == 0) continue;          // idle row has no PSAPI counterpart
        if (r.name.empty() && r.pid != 4) continue;

        UniqueHandle h(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, r.pid));
        if (!h) continue;  // cannot cross-check this one; try the next candidate

        // --- 1. CPU time: kernel/user each within ±1s of GetProcessTimes ---
        FILETIME ct{}, ex{}, kt{}, ut{};
        if (!::GetProcessTimes(h.get(), &ct, &ex, &kt, &ut)) continue;
        const uint64_t apiK = FtU64(kt), apiU = FtU64(ut);
        const int64_t dK = static_cast<int64_t>(apiK) - static_cast<int64_t>(r.kernelTime);
        const int64_t dU = static_cast<int64_t>(apiU) - static_cast<int64_t>(r.userTime);
        if (dK > k100nsPerSec || dK < -k100nsPerSec) {
            return {true, Fmt(L"NtQSI 自校验未通过（PID {} 内核时间偏差>1s），已切换兼容模式", r.pid)};
        }
        if (dU > k100nsPerSec || dU < -k100nsPerSec) {
            return {true, Fmt(L"NtQSI 自校验未通过（PID {} 用户时间偏差>1s），已切换兼容模式", r.pid)};
        }

        // --- 2. Working set within ±25% ---
        PROCESS_MEMORY_COUNTERS_EX mc{};
        mc.cb = sizeof(mc);
        if (::GetProcessMemoryInfo(h.get(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mc),
                                   sizeof(mc))) {
            const bool wsOk =
                WithinRelative(mc.WorkingSetSize, r.workingSet, 0.25, 1024ull * 1024);
            if (!wsOk) {
                return {true, Fmt(L"NtQSI 自校验未通过（PID {} 工作集偏差超25%），已切换兼容模式", r.pid)};
            }
        }

        // --- 3. IO transfer bytes within ±25% (+1 MiB slack) ---
        IO_COUNTERS io{};
        if (::GetProcessIoCounters(h.get(), &io)) {
            const uint64_t apiIo = io.ReadTransferCount + io.WriteTransferCount +
                                   io.OtherTransferCount;
            const uint64_t qsiIo = r.ioReadBytes + r.ioWriteBytes + r.ioOtherBytes;
            const bool ioOk = WithinRelative(apiIo, qsiIo, 0.25, 1024ull * 1024);
            if (!ioOk) {
                return {true, Fmt(L"NtQSI 自校验未通过（PID {} IO 字节偏差超25%），已切换兼容模式", r.pid)};
            }
        }

        // --- 4. Handle count within ±10% (+4) ---
        DWORD hc = 0;
        if (::GetProcessHandleCount(h.get(), &hc)) {
            const bool hcOk = WithinRelative(hc, r.handles, 0.10, 4);
            if (!hcOk) {
                return {true, Fmt(L"NtQSI 自校验未通过（PID {} 句柄数偏差超10%），已切换兼容模式", r.pid)};
            }
        }

        // --- 5. Thread count within ±10% (+2) ---
        const uint32_t tc = ToolhelpThreadCountOf(r.pid);
        if (tc > 0) {
            const bool tcOk = WithinRelative(tc, r.threads, 0.10, 2);
            if (!tcOk) {
                return {true, Fmt(L"NtQSI 自校验未通过（PID {} 线程数偏差超10%），已切换兼容模式", r.pid)};
            }
        }

        ++validated;
    }

    // --- 6. Private working set: NtQSI WorkingSetPrivateSize vs one-shot PDH
    // \Process(*)\Working Set - Private (±25% + 2 MiB). Per the architect
    // ruling, PDH is used exactly HERE and never in the tick loop; the tick
    // path then fills privateWorkingSet straight from NtQSI. PDH being
    // unavailable or yielding no comparable process only SKIPS this item (the
    // other five items still validate the layout); an actual mismatch degrades
    // the whole fast path like the other items.
    GateResult item6;  // empty reason = item passed or was skipped
    {
        std::wstring idTemplate, wsTemplate;
        if (PdhLocalizeEnglishPath(L"\\Process(*)\\ID Process", &idTemplate) &&
            PdhLocalizeEnglishPath(L"\\Process(*)\\Working Set - Private", &wsTemplate)) {
            PDH_HQUERY q = nullptr;
            PDH_HCOUNTER idH = nullptr, wsH = nullptr;
            if (PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS &&
                PdhAddWildcardCounter(q, idTemplate, &idH) &&
                PdhAddWildcardCounter(q, wsTemplate, &wsH) && PdhCollect(q)) {
                std::vector<PdhArrayItem> ids, wss;
                if (PdhFmtArrayDouble(idH, &ids) && PdhFmtArrayDouble(wsH, &wss)) {
                    // Index-join: both arrays enumerate the same Process-object
                    // instance table of the same collect (names lack "#N").
                    std::unordered_map<uint32_t, double> pwsByPid;
                    const size_t n = std::min(ids.size(), wss.size());
                    for (size_t i = 0; i < n; ++i) {
                        if (ids[i].valid && wss[i].valid && ids[i].value > 0.0 &&
                            ids[i].value < 4294967040.0) {
                            pwsByPid[static_cast<uint32_t>(ids[i].value)] = wss[i].value;
                        }
                    }
                    int checked = 0;
                    for (const NtProcRow& r : rows) {
                        if (checked >= 2) break;
                        if (r.pid == 0 || r.privateWs <= 0) continue;
                        const auto it = pwsByPid.find(r.pid);
                        if (it == pwsByPid.end()) continue;  // instance gone / not exposed
                        const uint64_t ntqsi = static_cast<uint64_t>(r.privateWs);
                        const uint64_t pdh = static_cast<uint64_t>(it->second);
                        if (!WithinRelative(ntqsi, pdh, 0.25, 2048ull * 1024)) {
                            item6 = {true, Fmt(L"NtQSI 自校验未通过（PID {} 私有工作集偏差超25%），"
                                               L"已切换兼容模式",
                                               r.pid)};
                            break;
                        }
                        ++checked;
                    }
                }
            }
            PdhCloseQuerySafe(&q);
        }
    }
    if (item6.degraded) return item6;

    if (validated == 0) {
        return {true, L"NtQSI 自校验无可用对照进程，已切换 Toolhelp+PSAPI 兼容模式"};
    }
    return {false, L""};
}

}  // namespace cd
}  // namespace stm
