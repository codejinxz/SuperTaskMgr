// GpuCollector: DXGI adapter enumeration + PDH GPU Engine / GPU Process Memory
// counters (research R5 #11; the instance-name format pid_<pid>_luid_0x.._phys_
// .. is not officially documented but stable on Win10 1803+).
//
// Virtual display adapter filter (this machine ships two IddCx indirect-display
// adapters). A whitelist of real-GPU keywords is unreliable, so virtual-ness is
// decided from two IDENTITY-STABLE signals:
//   1. Software flag: DXGI_ADAPTER_FLAG_SOFTWARE (WARP / Microsoft Basic Render
//      Driver) is never a hardware GPU -> filtered.
//   2. Name: IddCx indirect display drivers ship self-describing names
//      ("IddDriver...", "Virtual Display...", ...) -> keyword filter.
// A third signal from the original design — "LUID absent from \GPU Engine
// instances" — was REMOVED in review V6-P1-3: engine-instance absence is a
// transient runtime state (a fully idle real dGPU exposes no engine instances
// until a process touches it), so it excluded idle real GPUs and made the
// adapter list flicker; a sticky keep-list cannot recover from a wrong first
// exclusion either. It never fired on the target machine: the IddCx adapters
// there DO expose engine counters and are dropped by the name rule anyway.
#include "collect/CollectDetail.h"
#include "core/Log.h"
#include <dxgi.h>
#include <algorithm>
#include <cwctype>
#include <map>
#include <set>

namespace stm {
namespace cd {

namespace {

// ---- instance name parsing -------------------------------------------------
// Format: pid_<pid>_luid_0x<HighPart>_0x<LowPart>_phys_<n>[_engtype_<t>]
int HexVal(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

bool ParseGpuInstance(const std::wstring& inst, uint32_t* pid, uint64_t* luid) {
    size_t p = inst.find(L"pid_");
    if (p == std::wstring::npos) return false;
    p += 4;
    uint64_t val = 0;
    size_t digits = 0;
    while (p < inst.size() && inst[p] >= L'0' && inst[p] <= L'9') {
        val = val * 10u + static_cast<uint64_t>(inst[p] - L'0');
        ++p;
        ++digits;
    }
    if (digits == 0 || digits > 10 || val > 0xFFFFFFFFull) return false;
    p = inst.find(L"luid_0x", p);
    if (p == std::wstring::npos) return false;
    p += 7;
    auto readHex = [&inst, &p](uint64_t* out) {
        uint64_t v = 0;
        size_t n = 0;
        while (p < inst.size()) {
            const int d = HexVal(inst[p]);
            if (d < 0) break;
            v = v * 16u + static_cast<uint64_t>(d);
            ++p;
            ++n;
        }
        *out = v;
        return n > 0;
    };
    uint64_t hi = 0, lo = 0;
    if (!readHex(&hi)) return false;
    if (p >= inst.size() || inst[p] != L'_') return false;
    ++p;
    if (inst.compare(p, 2, L"0x") == 0) p += 2;
    if (!readHex(&lo)) return false;
    *pid = static_cast<uint32_t>(val);
    *luid = (hi << 32) | lo;
    return true;
}

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return s;
}

// Signal 2 of the filter: IddCx-style virtual display names.
bool NameLooksVirtual(const std::wstring& name) {
    const std::wstring l = ToLower(name);
    return l.find(L"virtual") != std::wstring::npos ||
           l.find(L"idd") != std::wstring::npos ||
           l.find(L"indirect") != std::wstring::npos;
}

struct Adapter {
    std::wstring name;
    uint64_t luid = 0;
    uint64_t dedicated = 0;  // DXGI DedicatedVideoMemory
    bool software = false;
};

}  // namespace

// Closing the query also releases the counters bound to it (review V7-P1-1).
GpuCollector::~GpuCollector() { PdhCloseQuerySafe(&query_); }

void GpuCollector::Collect(const std::unordered_map<uint32_t, uint64_t>& createTimeByPid,
                           std::vector<GpuProcUsage>* procsOut,
                           std::vector<GpuAdapterInfo>* adaptersOut,
                           double* queryMs) {
    const auto t0 = std::chrono::steady_clock::now();
    procsOut->clear();
    adaptersOut->clear();

    // --- 1. DXGI adapters ---------------------------------------------------
    std::vector<Adapter> adapters;
    {
        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            IDXGIAdapter1* a = nullptr;
            for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 d{};
                if (SUCCEEDED(a->GetDesc1(&d))) {
                    Adapter ad;
                    ad.name = d.Description;
                    ad.luid = (static_cast<uint64_t>(static_cast<uint32_t>(d.AdapterLuid.HighPart))
                               << 32) | d.AdapterLuid.LowPart;
                    ad.dedicated = d.DedicatedVideoMemory;
                    ad.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
                    adapters.push_back(std::move(ad));
                }
                a->Release();
            }
            factory->Release();
        }
    }

    // --- 2. PDH GPU Engine + GPU Process Memory -----------------------------
    // (luid, pid) -> aggregated usage. std::map so gpuProcs comes out sorted
    // by (luid, pid) as the Snapshot contract requires.
    std::map<uint64_t, std::map<uint32_t, Agg>> byLuidPid;
    std::set<uint64_t> realLuids;  // LUIDs that survive the virtual filter
    if (!pdhFailed_) {
        if (query_ == nullptr) {
            // Wildcard counters added once; PDH tracks the instance set itself
            // (formatted-array pattern), so no rebuild is ever needed.
            std::wstring utilT, dedT, shrT;
            if (!PdhLocalizeEnglishPath(L"\\GPU Engine(*)\\Utilization Percentage", &utilT) ||
                !PdhLocalizeEnglishPath(L"\\GPU Process Memory(*)\\Dedicated Usage", &dedT) ||
                !PdhLocalizeEnglishPath(L"\\GPU Process Memory(*)\\Shared Usage", &shrT) ||
                PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS ||
                !PdhAddWildcardCounter(query_, utilT, &utilCounter_) ||
                !PdhAddWildcardCounter(query_, dedT, &dedCounter_) ||
                !PdhAddWildcardCounter(query_, shrT, &shrCounter_)) {
                pdhFailed_ = true;
                PdhCloseQuerySafe(&query_);
                utilCounter_ = dedCounter_ = shrCounter_ = nullptr;
                if (!pdhLogged_) {
                    pdhLogged_ = true;
                    STM_LOG_WARN("collect", L"PDH GPU Engine 计数器不可用，GPU 利用率显示为 —");
                }
            }
        }
        if (query_ && PdhCollect(query_)) {
            std::vector<PdhArrayItem> utils, deds, shrs;
            const bool haveArrays = PdhFmtArrayDouble(utilCounter_, &utils) &&
                                    PdhFmtArrayDouble(dedCounter_, &deds) &&
                                    PdhFmtArrayDouble(shrCounter_, &shrs);
            if (haveArrays) {
                // Utilization is a rate counter: fresh instances report invalid
                // for one sample; those are skipped until the next GPU tick.
                for (const PdhArrayItem& it : utils) {
                    uint32_t pid = 0;
                    uint64_t luid = 0;
                    if (!ParseGpuInstance(it.name, &pid, &luid)) continue;
                    if (!it.valid) continue;
                    Agg& agg = byLuidPid[luid][pid];
                    agg.util += it.value;
                    ++agg.utilN;
                }
                for (int memKind = 0; memKind < 2; ++memKind) {
                    const bool dedicated = (memKind == 0);
                    for (const PdhArrayItem& it : (dedicated ? deds : shrs)) {
                        if (!it.valid) continue;
                        uint32_t pid = 0;
                        uint64_t luid = 0;
                        if (!ParseGpuInstance(it.name, &pid, &luid)) continue;
                        Agg& agg = byLuidPid[luid][pid];
                        if (dedicated) {
                            agg.ded += static_cast<uint64_t>(it.value);
                            agg.hasDed = true;
                        } else {
                            agg.shr += static_cast<uint64_t>(it.value);
                            agg.hasShr = true;
                        }
                    }
                }
            }
        }
    }

    // --- 3. adapters after the virtual filter -------------------------------
    // Filter signals (review V6-P1-3: the former signal 3 — "LUID absent from
    // \GPU Engine instances" — was REMOVED):
    //   1. Software flag: WARP / Basic Render Driver is never a hardware GPU.
    //   2. Name: IddCx indirect display drivers ship self-describing names
    //      ("Virtual Display", "IddDriver", ...).
    // Rationale for removing signal 3 instead of making it sticky: engine-
    // instance absence is a TRANSIENT runtime state (a fully idle real dGPU
    // exposes no \GPU Engine instances until a process touches it), not a
    // stable identity signal — it excluded idle real GPUs and made the adapter
    // list flicker across ticks (the exact bug V6 filed). A sticky keep-list
    // only masks the first-seen case and cannot recover from a wrong first
    // exclusion. It added nothing on the target machine either: the two IddCx
    // adapters there DO expose engine counters (they are dropped by the name
    // rule), so counter presence was never a working discriminator. Signals 1
    // and 2 are identity-stable, which is what a filter must be.
    std::vector<const Adapter*> kept;
    for (const Adapter& a : adapters) {
        bool keep = true;
        if (a.software) {
            keep = false;  // signal 1
        } else if (NameLooksVirtual(a.name)) {
            keep = false;  // signal 2
        }
        if (keep) kept.push_back(&a);
    }
    for (const Adapter* a : kept) realLuids.insert(a->luid);

    for (const Adapter* a : kept) {
        GpuAdapterInfo info;
        info.name = a->name;
        info.luid = a->luid;
        info.virtualAdapter = false;  // contract: always false in sys.gpus
        info.memTotal = a->dedicated > 0 ? a->dedicated : kUnavailU64;
        const auto luidAgg = byLuidPid.find(a->luid);
        if (luidAgg != byLuidPid.end()) {
            double util = 0;
            bool hasUtil = false;
            uint64_t ded = 0;
            bool hasDed = false;
            for (const auto& pidEntry : luidAgg->second) {
                if (pidEntry.second.utilN > 0) {
                    util += pidEntry.second.util;
                    hasUtil = true;
                }
                if (pidEntry.second.hasDed) {
                    ded += pidEntry.second.ded;
                    hasDed = true;
                }
            }
            info.utilPercent = hasUtil ? std::min(util, 100.0) : kUnavail;
            info.memUsed = hasDed ? ded : kUnavailU64;
        }
        adaptersOut->push_back(std::move(info));
    }

    // --- 4. per-process usage, matched against this tick's snapshot ---------
    // GPU counter instances carry only a pid; map it to (pid, createTime) via
    // the current snapshot and drop unknown pids (R5 PID-reuse guidance).
    // Usage on filtered virtual display adapters is dropped as well: it is
    // desktop-composition noise, not work on a real GPU.
    for (const auto& luidEntry : byLuidPid) {
        if (realLuids.count(luidEntry.first) == 0) continue;
        for (const auto& pidEntry : luidEntry.second) {
            const auto it = createTimeByPid.find(pidEntry.first);
            if (it == createTimeByPid.end()) continue;  // not in this tick's snapshot
            const Agg& agg = pidEntry.second;
            GpuProcUsage g;
            g.key.pid = pidEntry.first;
            g.key.createTime = it->second;
            g.luid = luidEntry.first;
            g.utilPercent = agg.utilN > 0 ? std::min(agg.util, 100.0) : kUnavail;
            g.dedicatedBytes = agg.hasDed ? agg.ded : kUnavailU64;
            g.sharedBytes = agg.hasShr ? agg.shr : kUnavailU64;
            procsOut->push_back(std::move(g));
        }
    }

    *queryMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                   .count();
}

}  // namespace cd
}  // namespace stm
