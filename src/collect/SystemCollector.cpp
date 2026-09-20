// SystemCollector：每 tick 的系统级指标。
// CPU：总量用 GetSystemTimes（内核时间含空闲 -> 忙碌 =
// kernel-idle+user），每核用 NtQSI(SystemProcessorPerformanceInformation)。
// 内存：GlobalMemoryStatusEx + GetPerformanceInfo。磁盘速率 + 系统硬缺页 +
// 磁盘队列深度（Phase C）：PDH 英文计数器。网络：GetIfTable2 字节差值
//（总量 + Phase D 每适配器按 ifIndex 差分）。所有采不到的字段
// 保持 kUnavail——绝不用约定俗成的 0。
#include <winsock2.h>  // 必须在 iphlpapi/netioapi 之前包含（LEAN_AND_MEAN 会隐藏 winsock）
#include <ws2tcpip.h>  // 引入 ws2ipdef.h -> 为 netioapi 的 MIB_* 声明定义 _WS2IPDEF_
#include "collect/AdapterInfo.h"  // IfTypeLabel（IF_TYPE -> 中文标签，头内纯函数）
#include "collect/CollectDetail.h"
#include "core/Log.h"
#include "core/Str.h"  // Fmt（接口名兜底文案）
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
// Phase C：队列深度为非速率（原始）计数器；与速率计数器同查询同节奏读取。
constexpr wchar_t kDiskQueue[] = L"\\PhysicalDisk(*)\\Current Disk Queue Length";
constexpr wchar_t kHardFaults[] = L"\\Memory\\Hard Faults/sec";

uint64_t FtU64(const FILETIME& f) {
    return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

bool IsTotalInstance(const std::wstring& inst) { return inst == L"_Total"; }

}  // namespace

// 关闭查询的同时会释放绑定其上的计数器（评审 V7-P1-1）。
SystemCollector::~SystemCollector() { PdhCloseQuerySafe(&pdhQuery_); }

// PDH 磁盘速率 + 硬缺页。所有计数器都是通配/普通英文路径，
// 只添加一次；磁盘实例集合由 PDH 自己维护，经格式化数组读回
//（无需重建）。速率计数器在当前查询内需要两次采集 ->
// 首 tick 为 kUnavail，UI 渲染为 "—"。
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
        // 该路径失败时仅硬缺页单独降级为 kUnavail。
        std::wstring hfTemplate;
        if (PdhLocalizeEnglishPath(kHardFaults, &hfTemplate)) {
            PdhAddWildcardCounter(pdhQuery_, hfTemplate, &hardFaults_);
        }
        // Phase C：磁盘队列深度。部分机器无 PhysicalDisk 计数器对象
        //（或被禁用）——单独降级，不影响同查询的速率/硬缺页计数器；
        // diskQueue_ 为空时 diskQueueDepth 恒保持 kUnavail（诚实）。
        std::wstring dqTemplate;
        if (PdhLocalizeEnglishPath(kDiskQueue, &dqTemplate)) {
            PdhAddWildcardCounter(pdhQuery_, dqTemplate, &diskQueue_);
        }
    }
    if (!PdhCollect(pdhQuery_)) return;

    // 汇总每盘速率；跳过 "_Total" 避免重复计数，但若无任何
    // 单盘实例上报数值，则回退用它。
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
        out->hardFaultsPerSec = hf;  // 不可得时诚实给 kUnavail
    }
    // Phase C：队列深度合计。口径与磁盘速率一致：跳过 "_Total" 防重复
    // 计数；无任何单盘有效值时回退 _Total。队列长度为原始（非速率）计数器，
    // 但首拍走与速率相同的 PdhCollect 流程，无额外预热逻辑。
    if (diskQueue_) {
        std::vector<PdhArrayItem> queue;
        if (PdhFmtArrayDouble(diskQueue_, &queue)) {
            double qSum = 0.0, qTotal = 0.0;
            bool sawDiskQ = false, sawTotalQ = false;
            for (const PdhArrayItem& it : queue) {
                if (!it.valid) continue;
                if (IsTotalInstance(it.name)) {
                    qTotal = it.value;
                    sawTotalQ = true;
                } else {
                    qSum += it.value;
                    sawDiskQ = true;
                }
            }
            if (sawDiskQ || sawTotalQ) {
                out->diskQueueDepth = sawDiskQ ? qSum : qTotal;
            }
        }
    }
}

// 网络吞吐：对所有非回环、处于运行状态的接口取 GetIfTable2 字节差值。
// 总量（netRecvBps/netSendBps）之外，Phase D 起同时产出每适配器差分
//（SystemInfo::netAdapters，仅 Up 非回环；名称取 MIB_IF_ROW2.Alias 即
// 适配器友好名，typeLabel 复用 AdapterInfo 的 IfTypeLabel 映射——不引入
// 第二张 IF_TYPE 表，也不需要 30s 友好名缓存，因为 GetIfTable2 每拍
// 已原样带回 Alias）。
void SystemCollector::CollectNet(SystemInfo* out, double elapsedSec) {
    MIB_IF_TABLE2* tbl = nullptr;
    if (::GetIfTable2(&tbl) != NO_ERROR) return;  // 保留先前值/kUnavail
    uint64_t recv = 0, send = 0;
    std::vector<SystemInfo::AdapterThroughput> adapters;
    std::unordered_map<uint64_t, uint64_t> curRecv, curSend;
    for (ULONG i = 0; i < tbl->NumEntries; ++i) {
        const MIB_IF_ROW2& r = tbl->Table[i];
        if (r.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (r.OperStatus != IfOperStatusUp) continue;
        recv += r.InOctets;
        send += r.OutOctets;
        const uint64_t ifIdx = static_cast<uint64_t>(r.InterfaceIndex);
        curRecv[ifIdx] = r.InOctets;
        curSend[ifIdx] = r.OutOctets;
        SystemInfo::AdapterThroughput a;
        a.ifIndex = ifIdx;
        a.name = r.Alias[0] != L'\0' ? std::wstring(r.Alias)
                                     : (r.Description[0] != L'\0'
                                            ? std::wstring(r.Description)
                                            : Fmt(L"接口 {}", ifIdx));
        a.typeLabel = IfTypeLabel(r.Type);
        // 首拍无基线、新出现接口、或计数回退（驱动重置/掉卡）——该接口
        // 本 tick 速率 kUnavail（防无符号差下溢出假尖峰），下一拍恢复。
        if (haveAdapterPrev_ && elapsedSec > 0.0) {
            const auto pr = prevRecvByIf_.find(ifIdx);
            const auto ps = prevSendByIf_.find(ifIdx);
            if (pr != prevRecvByIf_.end() && r.InOctets >= pr->second) {
                a.recvBps = static_cast<double>(r.InOctets - pr->second) / elapsedSec;
            }
            if (ps != prevSendByIf_.end() && r.OutOctets >= ps->second) {
                a.sendBps = static_cast<double>(r.OutOctets - ps->second) / elapsedSec;
            }
        }
        adapters.push_back(std::move(a));
    }
    ::FreeMibTable(tbl);
    out->netAdapters = std::move(adapters);
    prevRecvByIf_ = std::move(curRecv);
    prevSendByIf_ = std::move(curSend);
    haveAdapterPrev_ = true;
    if (haveNetPrev_ && elapsedSec > 0.0) {
        // 接口集合变化（VPN 断开、网卡热拔、计数器重置）会使
        // 字节总和无符号差回绕到约 1.8e19，
        // 显示虚假尖峰（评审 V6-P1-2）。本 tick 上报 kUnavail；
        // 无论哪种情况都更新 prev，下一 tick 相对新基线
        // 重新有效。收/发各自独立回退。
        if (recv >= prevRecv_) {
            out->netRecvBps = static_cast<double>(recv - prevRecv_) / elapsedSec;
        } else {
            out->netRecvBps = kUnavail;
        }
        if (send >= prevSend_) {
            out->netSendBps = static_cast<double>(send - prevSend_) / elapsedSec;
        } else {
            out->netSendBps = kUnavail;
        }
    }
    prevRecv_ = recv;
    prevSend_ = send;
    haveNetPrev_ = true;
}

void SystemCollector::Collect(const ProcessCollector::TickOut& pt, SystemInfo* out) {
    const double elapsedSec = pt.elapsedSec;

    // --- CPU 总量：GetSystemTimes（内核含空闲）-----------------------------
    FILETIME ftIdle{}, ftKernel{}, ftUser{};
    if (::GetSystemTimes(&ftIdle, &ftKernel, &ftUser)) {
        const uint64_t idle = FtU64(ftIdle), kern = FtU64(ftKernel), user = FtU64(ftUser);
        if (haveCpuPrev_ && elapsedSec > 0.0) {
            const uint64_t dK = kern - prevKernel_, dU = user - prevUser_, dI = idle - prevIdle_;
            const uint64_t denom = dK + dU;  // 内核时间已含空闲
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

    // --- CPU 每核：NtQSI SystemProcessorPerformanceInformation -------------
    // 此处内核同样含空闲，忙碌 = kernel - idle + user。
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

    // --- 物理内存 -----------------------------------------------------------
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (::GlobalMemoryStatusEx(&ms)) {
        out->physTotal = ms.ullTotalPhys;
        out->physAvail = ms.ullAvailPhys;
    }

    // --- 提交/内核内存 + 计数兜底 -------------------------------------------
    // NtQSI(SystemPerformanceInformation) 约 0.1ms；GetPerformanceInfo 在本机
    // 约 10ms，仅作降级路径兜底保留。
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
        // 与进程列表同一次 NtQSI 扫描聚合（规格要求）。
        out->procCount = pt.totals.procCount;
        out->handleTotal = pt.totals.handleTotal;
        out->threadTotal = pt.totals.threadTotal;
    } else if (perfInfo) {
        out->procCount = pi.ProcessCount;
        out->handleTotal = pi.HandleCount;
        out->threadTotal = pi.ThreadCount;
    }

    // --- PDH（磁盘 + 硬缺页）与网络 -----------------------------------------
    CollectPdh(out);
    CollectNet(out, elapsedSec);

    // --- 运行时长 ------------------------------------------------------------
    out->uptimeSec = static_cast<double>(::GetTickCount64()) / 1000.0;
}

}  // namespace cd
}  // namespace stm
