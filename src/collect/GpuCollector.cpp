// GpuCollector：DXGI 适配器枚举 + PDH GPU Engine / GPU Process Memory
// 计数器（调研 R5 #11；实例名格式 pid_<pid>_luid_0x.._phys_..
// 虽无官方文档，但在 Win10 1803+ 上保持稳定）。
//
// 虚拟显示适配器过滤（本机自带两个 IddCx 间接显示适配器）。
// 真实 GPU 关键字白名单并不可靠，因此是否虚拟由两个身份稳定的
// 信号判定：
//   1. 软件标志：DXGI_ADAPTER_FLAG_SOFTWARE（WARP / Microsoft Basic Render
//      Driver）绝不是硬件 GPU -> 过滤。
//   2. 名称：IddCx 间接显示驱动自带描述性名称
//     （"IddDriver..."、"Virtual Display..." 等）-> 关键字过滤。
// 原设计的第三种信号——“LUID 在 \GPU Engine 实例中缺失”——
// 已在评审 V6-P1-3 中移除：引擎实例缺失是一种瞬态运行时状态
//（完全空闲的真实 dGPU 在有进程触碰之前不会暴露任何引擎实例），
// 因此它会错误排除空闲的真实 GPU，导致适配器列表闪烁；
// 粘滞保留表也无法从错误的首次排除中恢复。
// 该信号在目标机器上从未触发：那两块 IddCx 适配器
// 确实暴露引擎计数器，反正也会被名称规则过滤。
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

// ---- 实例名解析 -------------------------------------------------------------
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

// 过滤信号 2：IddCx 风格的虚拟显示名称。
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

// 关闭查询的同时会释放绑定其上的计数器（评审 V7-P1-1）。
GpuCollector::~GpuCollector() { PdhCloseQuerySafe(&query_); }

void GpuCollector::Collect(const std::unordered_map<uint32_t, uint64_t>& createTimeByPid,
                           std::vector<GpuProcUsage>* procsOut,
                           std::vector<GpuAdapterInfo>* adaptersOut,
                           double* queryMs) {
    const auto t0 = std::chrono::steady_clock::now();
    procsOut->clear();
    adaptersOut->clear();

    // --- 1. DXGI 适配器 -----------------------------------------------------
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
    //（luid, pid）-> 聚合用量。用 std::map 使 gpuProcs 按快照契约要求
    // 的 (luid, pid) 顺序输出。
    std::map<uint64_t, std::map<uint32_t, Agg>> byLuidPid;
    std::set<uint64_t> realLuids;  // 通过虚拟过滤的 LUID
    if (!pdhFailed_) {
        if (query_ == nullptr) {
            // 通配计数器只添加一次；实例集合由 PDH 自己维护
            //（格式化数组模式），因此永远无需重建。
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
                // 利用率是速率计数器：新实例首个样本报告无效，
                // 这些样本被跳过，等下一个 GPU tick 再取。
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

    // --- 3. 虚拟过滤后的适配器 ----------------------------------------------
    // 过滤信号（评审 V6-P1-3：原信号 3——“LUID 在
    // \GPU Engine 实例中缺失”——已移除）：
    //   1. 软件标志：WARP / Basic Render Driver 绝不是硬件 GPU。
    //   2. 名称：IddCx 间接显示驱动自带描述性名称
    //     （"Virtual Display"、"IddDriver" 等）。
    // 移除信号 3 而非将其做成粘滞的理由：引擎实例缺失是一种
    // 瞬态运行时状态（完全空闲的真实 dGPU 在有进程触碰
    // 之前不暴露任何 \GPU Engine 实例），不是稳定的身份信号——
    // 它会错误排除空闲的真实 GPU，并让适配器列表跨 tick 闪烁
    //（正是 V6 记录的那个 bug）。粘滞保留表只能掩盖首次出现的情况，
    // 无法从错误的首次排除中恢复。
    // 它在目标机器上也没有任何作用：那两块 IddCx 适配器
    // 确实暴露引擎计数器（反正会被名称规则过滤），
    // 因此“有无计数器”从来就不是一个有效的判据。信号 1
    // 和信号 2 才是身份稳定的，过滤器必须依赖这种性质。
    std::vector<const Adapter*> kept;
    for (const Adapter& a : adapters) {
        bool keep = true;
        if (a.software) {
            keep = false;  // 信号 1
        } else if (NameLooksVirtual(a.name)) {
            keep = false;  // 信号 2
        }
        if (keep) kept.push_back(&a);
    }
    for (const Adapter* a : kept) realLuids.insert(a->luid);

    for (const Adapter* a : kept) {
        GpuAdapterInfo info;
        info.name = a->name;
        info.luid = a->luid;
        info.virtualAdapter = false;  // 契约：在 sys.gpus 中恒为 false
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

    // --- 4. 每进程用量，与本 tick 快照匹配 ----------------------------------
    // GPU 计数器实例只带 pid；通过当前快照把它映射到 (pid, createTime)，
    // 并丢弃未知 pid（R5 关于 PID 复用的指导）。
    // 已过滤虚拟显示适配器上的用量同样丢弃：那是桌面合成的噪音，
    // 不是真实 GPU 上的工作。
    for (const auto& luidEntry : byLuidPid) {
        if (realLuids.count(luidEntry.first) == 0) continue;
        for (const auto& pidEntry : luidEntry.second) {
            const auto it = createTimeByPid.find(pidEntry.first);
            if (it == createTimeByPid.end()) continue;  // 不在本 tick 快照中
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
