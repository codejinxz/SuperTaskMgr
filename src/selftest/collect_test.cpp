// collect: selftest cases for the collection layer (arch section 11).
// Each case runs the real CollectService for a few ticks and validates the
// snapshot contract. System-level net/disk fields are allowed to be kUnavail.
#include "selftest/TestFramework.h"
#include "collect/CollectService.h"
#include "core/ProcData.h"
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <memory>
#include <string>
#include <vector>

namespace {

using stm::CollectService;
using stm::kUnavail;
using stm::kUnavailU64;
using stm::ProcInfo;
using stm::Snapshot;

// Stops the service on scope exit even when an assertion fails early.
struct ServiceGuard {
    CollectService& svc;
    ~ServiceGuard() { svc.Stop(); }
};

// Polls the store until tickId >= n. Returns the latest snapshot.
std::shared_ptr<const Snapshot> WaitForTicks(CollectService& svc, uint64_t n, int timeoutMs,
                                             bool* reached) {
    for (int waited = 0; waited < timeoutMs; waited += 50) {
        ::Sleep(50);
        auto s = svc.Store().Get();
        if (s->tickId >= n) {
            *reached = true;
            return s;
        }
    }
    *reached = false;
    return svc.Store().Get();
}

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return s;
}

uint64_t FtU64(const FILETIME& f) {
    return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

}  // namespace

// --- snapshot integrity: procs non-empty, ascending pid, self present --------
STM_TEST(collect_snapshot_integrity) {
    CollectService svc;
    ServiceGuard guard{svc};
    if (!svc.Start(500)) {
        *err = L"CollectService::Start 失败";
        return false;
    }
    bool reached = false;
    const auto s = WaitForTicks(svc, 2, 8000, &reached);
    if (!reached) {
        *err = L"8 秒内未产出 2 个快照";
        return false;
    }
    if (s->procs.empty()) {
        *err = L"快照进程列表为空";
        return false;
    }
    for (size_t i = 1; i < s->procs.size(); ++i) {
        if (s->procs[i - 1].key.pid >= s->procs[i].key.pid) {
            *err = L"进程未按 pid 升序排列";
            return false;
        }
    }
    // Self process: present with a sane name and create time.
    const uint32_t selfPid = ::GetCurrentProcessId();
    const ProcInfo* self = nullptr;
    for (const ProcInfo& p : s->procs) {
        if (p.key.pid == selfPid) {
            self = &p;
            break;
        }
    }
    if (!self) {
        *err = L"快照未包含自身进程";
        return false;
    }
    const std::wstring name = ToLower(self->name);
    if (name != L"supertaskmgr.exe" && name != L"stm_selftest.exe") {
        *err = L"自身进程名异常：" + self->name;
        return false;
    }
    FILETIME ct{}, ex{}, kt{}, ut{};
    if (!::GetProcessTimes(::GetCurrentProcess(), &ct, &ex, &kt, &ut)) {
        *err = L"GetProcessTimes(自身) 失败";
        return false;
    }
    const int64_t diff = static_cast<int64_t>(self->key.createTime) -
                         static_cast<int64_t>(FtU64(ct));
    constexpr int64_t k2s = 20'000'000LL;  // 100ns units
    if (diff > k2s || diff < -k2s) {
        *err = L"自身 createTime 与 GetProcessTimes 偏差超过 2 秒";
        return false;
    }
    return true;
}

// --- self-check gate outcome: fast path or honest degradation, never crash ---
STM_TEST(collect_selfcheck_gate) {
    CollectService svc;
    ServiceGuard guard{svc};
    if (!svc.Start(500)) {
        *err = L"CollectService::Start 失败";
        return false;
    }
    bool reached = false;
    const auto s = WaitForTicks(svc, 2, 8000, &reached);
    if (!reached) {
        *err = L"8 秒内未产出 2 个快照";
        return false;
    }
    // Either outcome is valid on a healthy machine; a degraded snapshot must
    // always carry a non-empty reason (UI shows it as 兼容模式).
    if (s->degraded && s->degradeReason.empty()) {
        *err = L"降级快照缺少降级原因";
        return false;
    }
    return true;
}

// --- tick latency: warm-tick p50 < 15ms, lastTickMs < 100 --------------------
STM_TEST(collect_tick_latency) {
    CollectService svc;
    ServiceGuard guard{svc};
    if (!svc.Start(500)) {
        *err = L"CollectService::Start 失败";
        return false;
    }
    // Record LastTickMs once per completed tick; sample 0 includes the cold
    // start (first-time PDH/registry caches + self-check gate), so the p50 is
    // computed over the warm samples only.
    std::vector<double> samples;
    uint64_t lastId = 0;
    for (int waited = 0; waited < 15000 && samples.size() < 12; waited += 25) {
        ::Sleep(25);
        const auto s = svc.Store().Get();
        if (s->tickId != lastId) {
            lastId = s->tickId;
            samples.push_back(svc.LastTickMs());
        }
    }
    if (samples.size() < 6) {
        *err = L"15 秒内未产出足够的 tick 样本";
        return false;
    }
    std::vector<double> warm(samples.begin() + 1, samples.end());
    std::sort(warm.begin(), warm.end());
    const double p50 = warm[warm.size() / 2];
    if (p50 >= 15.0) {
        *err = L"tick p50 ≥ 15ms";
        return false;
    }
    if (svc.LastTickMs() >= 100.0) {
        *err = L"lastTickMs ≥ 100ms";
        return false;
    }
    return true;
}

// --- private working set: NtQSI primary source, gate-validated ---------------
STM_TEST(collect_privatews_gate) {
    CollectService svc;
    ServiceGuard guard{svc};
    if (!svc.Start(500)) {
        *err = L"CollectService::Start 失败";
        return false;
    }
    bool reached = false;
    const auto s = WaitForTicks(svc, 3, 8000, &reached);
    if (!reached) {
        *err = L"8 秒内未产出 3 个快照";
        return false;
    }
    const uint32_t selfPid = ::GetCurrentProcessId();
    const ProcInfo* self = nullptr;
    for (const ProcInfo& p : s->procs) {
        if (p.key.pid == selfPid) {
            self = &p;
            break;
        }
    }
    if (!self) {
        *err = L"快照未包含自身进程";
        return false;
    }
    if (s->degraded) {
        // Gate failed (or NtQSI unavailable): the field degrades honestly to
        // kUnavailU64 like every other slow-path gap; a real value is fine too.
        if (self->privateWorkingSet != kUnavailU64 && self->privateWorkingSet == 0) {
            *err = L"降级模式下私有工作集为 0（应为 kUnavailU64 或真实值）";
            return false;
        }
        return true;
    }
    if (self->privateWorkingSet == kUnavailU64) {
        *err = L"门通过但私有工作集不可用";
        return false;
    }
    if (self->privateWorkingSet == 0) {
        *err = L"私有工作集为 0";
        return false;
    }
    if (self->privateWorkingSet > self->workingSet) {
        *err = L"私有工作集大于工作集";
        return false;
    }
    return true;
}

// --- system PDH rates: values appear or degrade honestly to kUnavail ---------
STM_TEST(collect_pdh_english_counters) {
    CollectService svc;
    ServiceGuard guard{svc};
    if (!svc.Start(500)) {
        *err = L"CollectService::Start 失败";
        return false;
    }
    bool reached = false;
    const auto s = WaitForTicks(svc, 6, 15000, &reached);
    if (!reached) {
        *err = L"15 秒内未产出 6 个 tick";
        return false;
    }
    // Every rate field must be either kUnavail (honest degrade, e.g. counter
    // absent on this perflib) or a sane non-negative measurement.
    const double rates[] = {s->sys.diskReadBps,   s->sys.diskWriteBps,
                            s->sys.netRecvBps,    s->sys.netSendBps,
                            s->sys.hardFaultsPerSec};
    for (const double r : rates) {
        if (r != kUnavail && (r < 0.0 || r > 1e12)) {
            *err = L"系统速率字段出现异常数值";
            return false;
        }
    }
    return true;
}

// --- GPU adapters: virtual display adapters (IddCx) must be filtered ---------
STM_TEST(collect_gpu_adapters_filtered) {
    CollectService svc;
    ServiceGuard guard{svc};
    if (!svc.Start(500)) {
        *err = L"CollectService::Start 失败";
        return false;
    }
    bool reached = false;
    const auto s = WaitForTicks(svc, 2, 8000, &reached);  // GPU runs on tick 1
    if (!reached) {
        *err = L"8 秒内未产出 2 个快照";
        return false;
    }
    for (const stm::GpuAdapterInfo& g : s->sys.gpus) {
        const std::wstring l = ToLower(g.name);
        if (l.find(L"virtual") != std::wstring::npos || l.find(L"idd") != std::wstring::npos) {
            *err = L"虚拟显示适配器未被过滤：" + g.name;
            return false;
        }
        if (g.virtualAdapter) {
            *err = L"sys.gpus 中出现 virtualAdapter=true 的适配器";
            return false;
        }
    }
    return true;
}
