#pragma once
// Architect-owned contract header (FROZEN). See docs/phase/01_架构设计文档.md section 4.
// 采集线程（生产者）与 UI 线程（消费者）之间共享的数据类型。
// 刻意不包含任何 OS 头文件，使 stm_collect 与 stm_ops 都能依赖它。
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace stm {

// 进程标识：单靠 PID 会被 OS 回收复用，(pid, createTime) 才是稳定标识。
struct ProcKey {
    uint32_t pid = 0;
    uint64_t createTime = 0;  // FILETIME 打包为 u64
};

inline bool operator==(const ProcKey& a, const ProcKey& b) { return a.pid == b.pid && a.createTime == b.createTime; }
inline bool operator!=(const ProcKey& a, const ProcKey& b) { return !(a == b); }
inline bool operator<(const ProcKey& a, const ProcKey& b) {
    return a.pid != b.pid ? a.pid < b.pid : a.createTime < b.createTime;
}

// 哨兵值：表示"该指标无法采集"（渲染为破折号）。
// 绝不用 0 表示"无数据"。
inline constexpr double   kUnavail    = std::numeric_limits<double>::quiet_NaN();
inline constexpr uint64_t kUnavailU64 = std::numeric_limits<uint64_t>::max();

enum ProcFlag : uint32_t {
    PF_Elevated     = 1u << 0,
    PF_Uwp          = 1u << 1,
    PF_Wow64        = 1u << 2,
    PF_ServiceHost  = 1u << 3,
    PF_HasWindow    = 1u << 4,
    PF_Protected    = 1u << 5,
    PF_Suspended    = 1u << 6,
    PF_AccessDenied = 1u << 7,  // 快照内粘滞：一旦拒绝访问，保留该标志
};

struct ProcInfo {
    ProcKey key;
    uint32_t parentPid = 0;
    uint32_t sessionId = 0;
    std::wstring name;   // 映像文件名，如 "explorer.exe"
    std::wstring path;   // 完整映像路径；无读取权限时为空（诚实留空，绝不伪造）
    uint64_t kernelTime = 0;  // 累计值，100ns 单位（由 SelfCheckGate 把关）
    uint64_t userTime = 0;
    double cpuPercent = kUnavail;      // 按全部核心归一化：dTime/(tickSec*cores)*100
    uint64_t workingSet = 0;           // 字节（NtQSI）
    uint64_t privateWorkingSet = kUnavailU64;  // 字节（PDH Working Set - Private）
    uint64_t commitBytes = 0;          // PrivateUsage，即 "Commit size"
    uint64_t ioReadBytes = 0;          // 累计值（NtQSI，由 SelfCheckGate 把关）
    uint64_t ioWriteBytes = 0;
    double diskBytesPerSec = kUnavail; // NtQSI IO 差值；范围 = 文件+网络+设备总量（UI 中已标注）
    double netBytesPerSec = kUnavail;  // 仅在设置 CAP_NET_ETW 时有效，否则为 kUnavail
    double pageFaultsPerSec = kUnavail;       // PageFaultCount 差值（软+硬缺页，语义见文档）
    double contextSwitchesPerSec = kUnavail;  // 各线程 ContextSwitches 聚合差值
    uint32_t handles = 0;
    uint32_t threads = 0;
    uint32_t gdiObjects = 0;   // 0 = 未获取（由 DetailsProvider 按需获取）
    uint32_t userObjects = 0;
    uint32_t flags = 0;
    std::wstring windowTitle;  // 无则为空；每第 5 个 tick 刷新一次
};

struct GpuAdapterInfo {
    std::wstring name;
    uint64_t luid = 0;
    double utilPercent = kUnavail;
    uint64_t memUsed = kUnavailU64;
    uint64_t memTotal = kUnavailU64;
    bool virtualAdapter = false;  // 在 sys.gpus 中恒为 false（已过滤）；保留用于诊断
};

struct GpuProcUsage {
    ProcKey key;
    uint64_t luid = 0;
    double utilPercent = kUnavail;
    uint64_t dedicatedBytes = kUnavailU64;
    uint64_t sharedBytes = kUnavailU64;
};

struct SystemInfo {
    double cpuTotalPercent = kUnavail;
    std::vector<double> perCorePercent;  // 已扣除空闲时间
    uint64_t physTotal = 0, physAvail = 0;
    uint64_t commitTotal = 0, commitLimit = 0;
    uint64_t kernPaged = 0, kernNonpaged = 0;
    uint32_t procCount = 0, handleTotal = 0, threadTotal = 0;
    double diskReadBps = kUnavail, diskWriteBps = kUnavail;
    double netRecvBps = kUnavail, netSendBps = kUnavail;
    double hardFaultsPerSec = kUnavail;  // 全系统（PDH \Memory）
    double diskQueueDepth = kUnavail;    // 全系统磁盘队列长度（PDH Current Disk Queue Length 合计）
    std::vector<GpuAdapterInfo> gpus;    // 已过滤虚拟显示适配器
    double uptimeSec = 0;

    // 每适配器吞吐（GetIfTable2 按 ifIndex 差分；仅 Up 的非回环物理/虚拟接口，排除环回）。
    struct AdapterThroughput {
        uint64_t ifIndex = 0;
        std::wstring name;      // 适配器友好名（中文系统即中文名，如"以太网"/"WLAN"）
        std::wstring typeLabel; // "以太网"/"Wi-Fi"/…（复用 IF_TYPE 映射）
        double recvBps = kUnavail;
        double sendBps = kUnavail;
    };
    std::vector<AdapterThroughput> netAdapters;
};

enum SnapshotCap : uint32_t { CAP_NET_ETW = 1u << 0 };

struct Snapshot {
    uint64_t tickId = 0;
    double tickSec = 0;   // 本快照中各差值所用的时间间隔
    int64_t timestamp = 0;  // Unix 秒级时间戳
    bool degraded = false;   // true = Toolhelp 慢路径（兼容模式）
    std::wstring degradeReason;
    uint32_t caps = 0;
    std::vector<ProcInfo> procs;        // 按 pid 升序
    std::vector<GpuProcUsage> gpuProcs; // 按 (luid, pid) 排序
    SystemInfo sys;
};

// 单写者/多读者快照单元。UI 每帧只拷贝一次 shared_ptr。
class SnapshotStore {
public:
    SnapshotStore() { Set(std::make_shared<const Snapshot>()); }
    std::shared_ptr<const Snapshot> Get() const {
        std::lock_guard<std::mutex> lock(mu_);
        return snap_;
    }
    void Set(std::shared_ptr<const Snapshot> s) {
        std::lock_guard<std::mutex> lock(mu_);
        snap_ = std::move(s);
    }

private:
    mutable std::mutex mu_;
    std::shared_ptr<const Snapshot> snap_;
};

}  // namespace stm

namespace std {
template <>
struct hash<stm::ProcKey> {
    size_t operator()(const stm::ProcKey& k) const noexcept {
        return hash<uint64_t>()((static_cast<uint64_t>(k.pid) << 32) ^ k.createTime);
    }
};
}  // namespace std
