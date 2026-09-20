#pragma once
// stm_collect 实现的内部头文件——不是契约头。
// 只有 src/collect/CollectService.h 由架构所有并冻结；这里声明的
// 一切都属采集库私有，可自由变更。
#include <windows.h>
#include <evntcons.h>  // EVENT_RECORD（ETW 消费回调的载荷）
#include <evntrace.h>  // TRACEHANDLE / 会话 API
#include <pdh.h>
#include <pdhmsg.h>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "core/ProcData.h"
#include "collect/NetMonitor.h"  // ConnEvent/RemoteTraffic/DnsEvent（内部实现复用）
#include "collect/NetTables.h"   // ConnEntry/ConnProto（差分纯函数签名）

namespace stm {

// ---------------------------------------------------------------------------
// 第 10 维护轮（NetMonitor，契约 NetMonitor.h）：连接事件差分纯函数 +
// 远程端点聚合到 UI 行的换算。二者刻意做成纯函数（不触碰成员/全局态），
// 由 NetMonitor.cpp 的线程与 selftest 共用。
// ---------------------------------------------------------------------------
// 差分相邻两次连接表快照中 `proto` 协议族的行，追加产出 ConnEvent
//（V25 P0-1 追加语义：*不*清空 *events——调用方复用同一 vector 连续做
// Tcp4/Tcp6 双族差分；需要隔离时调用方自行 clear）：
// 新出现的 TCP 行=New；消失且原状态非 LISTEN 的行=Closed；
// 同四元组 state 变化=StateChanged。UDP 族（Udp4/Udp6）不产任何事件。
// unixTime 写入每条事件；processName 留空由调用方用进程名缓存补齐。
void DiffConnSnapshots(const std::vector<ConnEntry>& prev,
                       const std::vector<ConnEntry>& cur, int64_t unixTime,
                       ConnProto proto, std::vector<ConnEvent>* events);

namespace cd {

// ---------------------------------------------------------------------------
// 环形事件缓冲（容量固定，满时覆盖最旧并计数——保新弃旧、绝不阻塞写入
// 线程）。Drain 按时间序取走全部。用于 ConnEvent（kNetEventCap）与
// DnsEvent（内部上限）两个实例。
// ---------------------------------------------------------------------------
template <typename T>
class EventRing {
public:
    explicit EventRing(size_t cap) : slots_(cap == 0 ? 1 : cap) {}
    EventRing(const EventRing&) = delete;
    EventRing& operator=(const EventRing&) = delete;
    void Push(T v) {
        std::lock_guard<std::mutex> lock(mu_);
        if (count_ < slots_.size()) {
            slots_[(head_ + count_) % slots_.size()] = std::move(v);
            ++count_;
        } else {
            slots_[head_] = std::move(v);  // 环回：覆盖最旧
            head_ = (head_ + 1) % slots_.size();
            ++dropped_;
        }
    }
    void Drain(std::vector<T>* out) {
        std::lock_guard<std::mutex> lock(mu_);
        out->clear();
        out->reserve(count_);
        for (size_t i = 0; i < count_; ++i) {
            out->push_back(std::move(slots_[(head_ + i) % slots_.size()]));
        }
        head_ = 0;
        count_ = 0;
    }
    uint64_t Dropped() const {
        std::lock_guard<std::mutex> lock(mu_);
        return dropped_;
    }
    void Clear() {
        std::lock_guard<std::mutex> lock(mu_);
        head_ = 0;
        count_ = 0;
        dropped_ = 0;  // 清空同时重置丢弃计数（“自上次清空以来”的诚实展示）
    }

private:
    mutable std::mutex mu_;
    std::vector<T> slots_;
    size_t head_ = 0;
    size_t count_ = 0;
    uint64_t dropped_ = 0;
};

// ---------------------------------------------------------------------------
// pid -> 进程名缓存（5s TTL，一次 Toolhelp 全系统快照喂满）。NameOf 查
// 不到返回空（诚实——进程可能已退出）。
// ---------------------------------------------------------------------------
class ProcNameCache {
public:
    std::wstring NameOf(uint32_t pid);

private:
    void RefreshLocked();

    std::mutex mu_;
    std::unordered_map<uint32_t, std::wstring> byPid_;
    std::chrono::steady_clock::time_point stamp_{};
};

// ---------------------------------------------------------------------------
// ETW 远程端点聚合的键与值（EtwNetCollector 填写，TopRemoteFromEndpoints
// 换算为 UI 的 RemoteTraffic 行）。family 取 AF_INET/AF_INET6；取不到
// 远程地址的事件不进此映射（退化为既有仅-pid 路径并计数）。
// ---------------------------------------------------------------------------
struct EndpointKey {
    uint32_t pid = 0;
    uint16_t family = 0;  // AF_INET / AF_INET6
    uint16_t port = 0;    // 主机字节序
    uint8_t proto = 0;    // IPPROTO_TCP/UDP；载荷无 proto 字段时为 0
    uint8_t ip[16] = {};  // family=AF_INET 时用前 4 字节
    bool operator==(const EndpointKey& o) const {
        return pid == o.pid && family == o.family && port == o.port && proto == o.proto &&
               std::memcmp(ip, o.ip, sizeof ip) == 0;
    }
};
struct EndpointKeyHash {
    size_t operator()(const EndpointKey& k) const noexcept;
};
struct EndpointAgg {
    uint64_t bytesIn = 0, bytesOut = 0, events = 0;
};
using EndpointMap = std::unordered_map<EndpointKey, EndpointAgg, EndpointKeyHash>;

}  // namespace cd

// 端点聚合映射 -> 按 (入+出) 总字节降序排序并截断 topN 的 RemoteTraffic。
// 纯函数：填 remote/port/service/pid/bytesIn/bytesOut；processName 由调用
// 方用进程名缓存补齐（保持可单测）。
std::vector<RemoteTraffic> TopRemoteFromEndpoints(const cd::EndpointMap& src, size_t topN);

namespace cd {

// ---------------------------------------------------------------------------
// NtQuerySystemInformation（ntdll 动态加载；结构布局采用
// NtDoc / Geoff Chappell 的 x64 布局，静态断言，并在信任快路径前
// 由 SelfCheckGate 在运行时*实际校验*）。
// ---------------------------------------------------------------------------
struct NtProcRow {
    uint32_t pid = 0, parentPid = 0, sessionId = 0;
    uint32_t handles = 0, threads = 0;
    uint64_t createTime = 0, kernelTime = 0, userTime = 0;  // 累计值，100ns
    uint64_t workingSet = 0;                                // 字节
    int64_t privateWs = 0;  // WorkingSetPrivateSize（字节；某些构建上可能为负）
    uint64_t privateCommit = 0;                             // PrivatePageCount（= PrivateUsage）
    uint64_t pageFaults = 0;                                // 累计次数
    uint64_t ioReadBytes = 0, ioWriteBytes = 0, ioOtherBytes = 0;  // 传输字节数
    uint64_t ctxSwitches = 0;                               // 线程 ContextSwitches 之和
    bool allThreadsSuspended = false;                       // 所有线程均为 Waiting+Suspended
    std::wstring name;
};
// 一次全系统快照。ntdll/NtQSI 不可用时返回 false。
bool NtqsiQueryProcesses(std::vector<NtProcRow>* out);

struct CoreTimes { uint64_t idle = 0, kernel = 0, user = 0; };  // 100ns；kernel 含空闲时间
// NtQSI(SystemProcessorPerformanceInformation)——每个逻辑核心一条。
bool NtqsiQueryPerCoreTimes(std::vector<CoreTimes>* out);

// 来自 NtQSI(SystemPerformanceInformation) 的提交/内核内存计数器——
// 取代 tick 路径上约 10ms 的 GetPerformanceInfo 调用（值以 PAGES 计；
// 需乘以页大小）。已在本机与 GetPerformanceInfo 交叉核对
//（commit/limit/paged/nonpaged 全部一致）。
struct SysMemCounters {
    uint64_t commitPages = 0, commitLimitPages = 0, pagedPoolPages = 0, nonPagedPoolPages = 0;
};
bool NtqsiQueryMemoryCounters(SysMemCounters* out);

// ---------------------------------------------------------------------------
// PDH 辅助。通配符数组模式（R5 #9b + MSDN "Enumerating Object
// Instances"）：PdhAddEnglishCounterW 解析语言无关名称，
// PdhGetCounterInfo 取得本地化的通配符模板，只把它添加到活动查询
// 一次。每次 PdhCollectQueryData 之后，PdhGetFormattedCounterArrayW
// 为每个活动实例返回一个值，实例名带完整的 "#N" 后缀——
// 实例集合由 PDH 自己维护，无需重建。
// ---------------------------------------------------------------------------
struct PdhArrayItem {
    std::wstring name;   // 实例名，如 "svchost#76" / "pid_1_luid_0x.._phys_0"
    double value = 0.0;
    bool valid = false;  // PDH 尚无有效样本时为 false（速率预热）
};
// 把语言无关的英文路径解析为本地化模板（每个计数器一次，
// 在采集器初始化时）。false = 计数器集不可用（硬失败）。
bool PdhLocalizeEnglishPath(const wchar_t* englishPath, std::wstring* localizedTemplate);
// 把本地化通配符模板作为 `query` 的计数器加入。
bool PdhAddWildcardCounter(PDH_HQUERY query, const std::wstring& localizedTemplate,
                           PDH_HCOUNTER* out);
inline void PdhCloseQuerySafe(PDH_HQUERY* q) {
    if (q && *q) { PdhCloseQuery(*q); *q = nullptr; }
}
bool PdhCollect(PDH_HQUERY q);
// 通配符计数器的全部实例值。valid=false 的条目表示尚无
// 可用样本（速率计数器需要两次采集；新出现的实例）。
bool PdhFmtArrayDouble(PDH_HCOUNTER h, std::vector<PdhArrayItem>* out);
// 单个（非通配）计数器值；尚无有效样本时为 false。
bool PdhFmtDouble(PDH_HCOUNTER h, double* out);

// ---------------------------------------------------------------------------
// 降级（兼容）路径使用的 Toolhelp 兜底。
// ---------------------------------------------------------------------------
struct ToolhelpRow {
    uint32_t pid = 0, parentPid = 0, threads = 0;
    std::wstring name;
};
bool ToolhelpEnumerate(std::vector<ToolhelpRow>* out);
// 单个 pid 的线程数（自建快照；克制使用——受自检门限约束）。
uint32_t ToolhelpThreadCountOf(uint32_t pid);

// ---------------------------------------------------------------------------
// 各采集器。状态保存在这些对象里；方法定义在对应的
// .cpp 编译单元中（ProcessCollector.cpp 等）。
// ---------------------------------------------------------------------------
class ProcessCollector {
public:
    struct Totals { uint32_t procCount = 0, handleTotal = 0, threadTotal = 0; };
    struct TickOut {
        std::vector<ProcInfo> procs;                 // 按 pid 升序
        Totals totals;                               // NtQSI 聚合（ntsiOk 时有效）
        double elapsedSec = 0;                       // 距上次 Collect（首次为 0）
        bool ntsiOk = false;                         // true = 本数据来自 NtQSI 快路径
        // 本 tick 进程的 pid -> createTime；供 GpuCollector 把 GPU 计数器
        // 的 pid 映射到 (pid, createTime) 身份。匹配不上的 pid 丢弃。
        std::unordered_map<uint32_t, uint64_t> createTimeByPid;
    };
    // tickId 驱动每 tick 轮转刷新 1/5 进程的补充信息；degraded 时切换到
    // Toolhelp+PSAPI 慢路径。绝不抛异常。
    void Collect(uint64_t tickId, bool degraded, TickOut* out);

    struct Supp {  // 缓存的补充信息，每 tick 轮转刷新 1/5 进程
        std::wstring path, title;
        uint32_t flags = 0;
    };

private:
    struct Prev {  // 上一 tick 的累计值，供差值字段使用（按 pid 键）
        uint64_t createTime = 0, execTime = 0, ioBytes = 0, pageFaults = 0, ctx = 0;
        bool execKnown = false, ioKnown = false, pfKnown = false, ctxKnown = false;
    };
    std::unordered_map<uint32_t, Prev> prev_;
    std::set<ProcKey> denied_;   // 粘滞的 PF_AccessDenied，按 (pid, createTime) 键
    std::unordered_map<ProcKey, Supp> supp_;
    std::unordered_map<uint32_t, uint32_t> servicesByPid_;  // pid -> 运行中服务数
    std::unordered_map<uint32_t, std::wstring> titlesByPid_;
    std::chrono::steady_clock::time_point lastCollect_{};
    bool haveLast_ = false;
    // 快路径上 privateWorkingSet 直接取自 NtQSI WorkingSetPrivateSize
    //（由 SelfCheckGate 第 6 项对照一次性 PDH 读取校验一次）；
    // 不存在每 tick 的 PDH 进程查询。慢路径将其留为
    // kUnavailU64。
};

class SystemCollector {
public:
    SystemCollector() = default;
    ~SystemCollector();  // 关闭 PDH 查询（评审 V7-P1-1）
    SystemCollector(const SystemCollector&) = delete;
    SystemCollector& operator=(const SystemCollector&) = delete;
    // pt 提供 NtQSI 聚合总量 + 用于计算速率的经过秒数。
    void Collect(const ProcessCollector::TickOut& pt, SystemInfo* out);

private:
    void CollectPdh(SystemInfo* out);
    void CollectNet(SystemInfo* out, double elapsedSec);
    uint64_t prevIdle_ = 0, prevKernel_ = 0, prevUser_ = 0;
    bool haveCpuPrev_ = false;
    std::vector<CoreTimes> prevCore_;
    bool haveCorePrev_ = false;
    uint64_t prevRecv_ = 0, prevSend_ = 0;  // GetIfTable2 字节计数器
    bool haveNetPrev_ = false;
    // Phase D：每适配器吞吐差分基线（ifIndex -> 上一 tick 的 In/OutOctets）。
    // 首拍无基线、新接口/计数回退 -> 该接口该 tick 速率 kUnavail（防下溢）。
    std::unordered_map<uint64_t, uint64_t> prevRecvByIf_, prevSendByIf_;
    bool haveAdapterPrev_ = false;
    // PDH：磁盘速率（通配；实例集合由 PDH 自身维护）+
    // 全系统硬缺页（普通计数器）+ 磁盘队列深度（Phase C；部分机器
    // 无此计数器 -> 缺席 -> SystemInfo::diskQueueDepth 保持 kUnavail）。
    PDH_HQUERY pdhQuery_ = nullptr;
    PDH_HCOUNTER diskRead_ = nullptr, diskWrite_ = nullptr, hardFaults_ = nullptr;
    PDH_HCOUNTER diskQueue_ = nullptr;
    bool pdhFailed_ = false, pdhLogged_ = false;
};

class GpuCollector {
public:
    GpuCollector() = default;
    ~GpuCollector();  // 关闭 PDH 查询（评审 V7-P1-1）
    GpuCollector(const GpuCollector&) = delete;
    GpuCollector& operator=(const GpuCollector&) = delete;
    // 汇集 DXGI 适配器 + PDH GPU Engine / GPU Process Memory 计数器。
    // createTimeByPid：本 tick 的进程身份，用于 pid 匹配（R5 #11：
    // GPU 计数器实例只带 pid；快照中不存在的 pid 丢弃）。
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
// EtwNetCollector（第 3 阶段，R5 #10b）：在清单提供程序
// Microsoft-Windows-Kernel-Network 上开私有实时 ETW 会话，从
// recvdata(10)/senddata(11) 事件聚合每 pid 累计收发字节。仅管理员可用——
// 否则 Start() 失败并记日志（功能保持禁用，
// 默认关闭）。资源章程（架构 §5）：会话名带本进程 pid 以避免孤儿会话；
// 启动前先停掉残留的同名会话；
// 析构时总是停止会话并 join 消费线程。
// 载荷字段通过 tdh.dll（动态绑定）按名称解码，绝不按
// 猜测的结构偏移解码。
// ---------------------------------------------------------------------------
class EtwNetCollector {
public:
    EtwNetCollector() = default;
    ~EtwNetCollector();  // Stop()
    EtwNetCollector(const EtwNetCollector&) = delete;
    EtwNetCollector& operator=(const EtwNetCollector&) = delete;

    // 可选：在 Start 前改写会话名（NetMonitor 用 L"SuperTaskMgr-NetMon-<pid>"
    // 以免与 CollectService 的 L"SuperTaskMgr-Net-<pid>" 互相挤掉）。
    void SetSessionName(const std::wstring& name) { sessionName_ = name; }

    bool Start();   // 幂等；失败返回 false 并记错误日志（非管理员等）
    void Stop();    // 幂等；停止会话并 join 消费线程
    bool Running(); // ETW 会话存活期间为 true
    // pid -> 自 Start 起的累计收发字节（线程安全拷贝）。
    void CopyCumulative(std::unordered_map<uint32_t, uint64_t>* out) const;
    uint64_t TotalEvents() const;  // 自 Start 起解析的收发事件数
    // (pid, 远程 ip:port, proto) -> 累计收发字节（第 10 维护轮扩展；线程安全拷贝）。
    void CopyEndpoints(EndpointMap* out) const;
    // 远程字段解析统计与一份原始字段样本（自测/实测探针用）。
    struct FieldStats {
        uint64_t addrHit = 0, addrMiss = 0;   // saddr/daddr 取到/缺失
        uint64_t portHit = 0, portMiss = 0;   // sport/dport 取到/缺失
        uint64_t dirFieldHit = 0;             // "direction" 字段存在
        uint64_t noRemoteEvents = 0;          // 因缺地址/端口退化为仅-pid 的事件
    };
    struct RawFieldSample {                   // 首个解析成功的载荷原样保存
        bool valid = false;
        uint16_t saddrFamily = 0, daddrFamily = 0;  // AddrProp 判定；0=未取到
        uint32_t sport = 0, dport = 0, direction = 0, proto = 0;
        uint8_t saddr[16] = {}, daddr[16] = {};
    };
    FieldStats CopyFieldStats() const;
    RawFieldSample CopyRawSample() const;
    // 存在同名 ETW 会话时为 true（selftest 清理检查用）。
    static bool SessionExists(const wchar_t* name);

private:
    struct Agg { uint64_t recv = 0, send = 0; };
    static void WINAPI OnEvent(PEVENT_RECORD rec);  // 经 .Context 蹦床转发
    void HandleEvent(PEVENT_RECORD rec);
    void Consume();  // 消费线程：OpenTrace -> ProcessTrace -> CloseTrace

    TRACEHANDLE session_ = 0;       // 来自 StartTraceW
    TRACEHANDLE openTrace_ = 0;     // 来自 OpenTraceW（由消费线程关闭）
    GUID myGuid_{};                 // 本会话实例 GUID（Start 后内核回填；区分同名新会话）
    std::wstring sessionName_;      // 默认 L"SuperTaskMgr-Net-<pid>"
    std::vector<BYTE> stopProps_;   // ControlTraceW 缓冲（由 mu_ 保护）
    std::thread consumer_;
    mutable std::mutex mu_;  // 保护下方所有成员（回调 + 所属线程）
    std::unordered_map<uint32_t, Agg> bytes_;
    EndpointMap endpoints_;         // 远程端点级聚合（与仅-pid 路径并存）
    FieldStats fieldStats_;
    RawFieldSample rawSample_;
    uint64_t totalEvents_ = 0;
    uint64_t parseFails_ = 0;
};

// ---------------------------------------------------------------------------
// DnsCollector（第 10 维护轮，实验性）：私有实时 ETW 会话订阅
// Microsoft-Windows-Dns-Client {1C95126E-7EEA-49A9-A3FE-A378B03DDB4D}，
// 从 Query/Response 事件解析查询域名 + pid（用户态提供者，事件头即发起
// 进程）。字段名按候选表尝试（QueryName/Query/DomainName），全部解不出
// 时 DecodedEvents 保持 0——NetMonitor 据此（或 ConsumerDead）自动禁用。
// 会话章程与 EtwNetCollector 相同：唯一名带 pid、启动前清残留、RAII 停止。
// ---------------------------------------------------------------------------
class DnsCollector {
public:
    DnsCollector() = default;
    ~DnsCollector();  // Stop()
    DnsCollector(const DnsCollector&) = delete;
    DnsCollector& operator=(const DnsCollector&) = delete;

    bool Start();   // 幂等；失败（非管理员等）返回 false 并记日志
    void Stop();    // 幂等
    bool Running();
    // 取走已解码的 DNS 事件（时间序；out 先被清空）。
    void DrainEvents(std::vector<DnsEvent>* out);
    uint64_t RawEvents() const;     // 到达的 Dns-Client 事件总数（含未解码）
    uint64_t DecodedEvents() const; // 成功解出域名的数量
    uint64_t DroppedEvents() const; // 因待取缓冲满而丢弃的已解码事件（诚实计数）
    bool ConsumerDead() const;      // OpenTraceW 失败：会话在跑但无人消费
    static bool SessionExists(const wchar_t* name);

private:
    static void WINAPI OnEvent(PEVENT_RECORD rec);
    void HandleEvent(PEVENT_RECORD rec);
    void Consume();

    static constexpr size_t kPendingCap = 4096;  // Drain 之间的待取上限

    TRACEHANDLE session_ = 0;
    TRACEHANDLE openTrace_ = 0;
    GUID myGuid_{};                 // 本会话实例 GUID（同 EtwNetCollector，V25 P0-2）
    std::wstring sessionName_;      // L"SuperTaskMgr-Dns-<pid>"
    std::vector<BYTE> stopProps_;
    std::thread consumer_;
    mutable std::mutex mu_;
    std::vector<DnsEvent> pending_;
    uint64_t raw_ = 0;
    uint64_t decoded_ = 0;
    uint64_t dropped_ = 0;  // pending_ 满时丢弃的已解码事件（诚实计数）
    bool consumerDead_ = false;
};

// ---------------------------------------------------------------------------
// SelfCheckGate：启动时把 NtQSI 快路径对最多 3 个存活进程的读数，
// 与 GetProcessTimes / GetProcessMemoryInfo / GetProcessIoCounters /
// GetProcessHandleCount / Toolhelp 线程数，以及 WorkingSetPrivateSize
// 与一次性 PDH "Working Set - Private" 读取的对照
//（架构第 4 节 + 架构裁定）全部交叉校验。任何超容差（时间 ±1s、
// 字节 ±25%、计数器 ±10%）都把整个服务降级到 Toolhelp+PSAPI
// 慢路径，绝不输出错误数据。
// ---------------------------------------------------------------------------
struct GateResult { bool degraded = false; std::wstring reason; };
GateResult RunSelfCheckGate();

}  // namespace cd
}  // namespace stm
