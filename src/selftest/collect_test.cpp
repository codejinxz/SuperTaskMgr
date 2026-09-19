// collect：采集层的自测用例（架构第 11 节）。
// 每个用例让真实 CollectService 跑几个 tick 并校验快照契约。
// 系统级网络/磁盘字段允许为 kUnavail。
#include "selftest/TestFramework.h"
#include "collect/CollectService.h"
#include "core/ProcData.h"
#include "core/Str.h"
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

// 即使断言提前失败也在作用域退出时停止服务。
struct ServiceGuard {
    CollectService& svc;
    ~ServiceGuard() { svc.Stop(); }
};

// 轮询仓库直到 tickId >= n。返回最新快照。
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

// --- 快照完整性：procs 非空、pid 升序、自身存在 --------
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
    // 自身进程：存在且名称与创建时间正常。
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
    constexpr int64_t k2s = 20'000'000LL;  // 100ns 单位
    if (diff > k2s || diff < -k2s) {
        *err = L"自身 createTime 与 GetProcessTimes 偏差超过 2 秒";
        return false;
    }
    return true;
}

// --- 自检门限结果：快路径或诚实降级，绝不崩溃 ---
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
    // 健康机器上两种结果都有效；降级快照必须
    // always carry a non-empty reason (UI shows it as 兼容模式).
    if (s->degraded && s->degradeReason.empty()) {
        *err = L"降级快照缺少降级原因";
        return false;
    }
    return true;
}

// --- tick 延迟：热 tick p50 < 15ms，lastTickMs < 100 --------------------
// p50 只按热 tick 测量（样本 0 是冷启动）。若首轮超预算，多半是本机
// 正忙于无关工作（并发构建），因此短暂冷却后补测一轮；
// 只有两轮都超预算才判失败。
//
STM_TEST(collect_tick_latency) {
    CollectService svc;
    ServiceGuard guard{svc};
    if (!svc.Start(500)) {
        *err = L"CollectService::Start 失败";
        return false;
    }
    double worstP50 = 0.0;
    for (int round = 0; round < 2; ++round) {
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
        if (p50 < 15.0) {
            if (svc.LastTickMs() >= 100.0) {
                *err = L"lastTickMs ≥ 100ms";
                return false;
            }
            return true;
        }
        worstP50 = p50;
        ::Sleep(2000);  // 冷却：等无关机器负载平息
    }
    *err = stm::Fmt(L"tick p50 ≥ 15ms（两轮 p50≈{:.1f}ms）", worstP50);
    return false;
}

// --- 私有工作集：NtQSI 主来源，门限校验 ---------------
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
        // 门限失败（或 NtQSI 不可用）：该字段诚实降级为
        // kUnavailU64，与其他慢路径空缺一致；有真实值也行。
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

// --- 系统 PDH 速率：出值或诚实降级为 kUnavail ---------
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
    // 每个速率字段要么为 kUnavail（诚实降级，如本 perflib
    // 缺该计数器），要么为合理的非负测量值。
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

// --- GPU 适配器：必须过滤虚拟显示适配器（IddCx）---------
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
