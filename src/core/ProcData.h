#pragma once
// Architect-owned contract header (FROZEN). See docs/phase/01_架构设计文档.md section 4.
// Shared data types between the collection thread (producer) and UI thread (consumer).
// Deliberately free of OS headers so both stm_collect and stm_ops can depend on it.
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace stm {

// Process identity: PID alone is recycled by the OS, (pid, createTime) is stable.
struct ProcKey {
    uint32_t pid = 0;
    uint64_t createTime = 0;  // FILETIME packed as u64
};

inline bool operator==(const ProcKey& a, const ProcKey& b) { return a.pid == b.pid && a.createTime == b.createTime; }
inline bool operator!=(const ProcKey& a, const ProcKey& b) { return !(a == b); }
inline bool operator<(const ProcKey& a, const ProcKey& b) {
    return a.pid != b.pid ? a.pid < b.pid : a.createTime < b.createTime;
}

// Sentinel values: "this metric could not be collected" (rendered as em dash).
// Never use 0 to mean "no data".
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
    PF_AccessDenied = 1u << 7,  // sticky within a snapshot: once denied, keep flag
};

struct ProcInfo {
    ProcKey key;
    uint32_t parentPid = 0;
    uint32_t sessionId = 0;
    std::wstring name;   // image file name, e.g. "explorer.exe"
    std::wstring path;   // full image path; empty when no read rights (honest empty, never faked)
    uint64_t kernelTime = 0;  // cumulative, 100ns units (self-check-gate covered)
    uint64_t userTime = 0;
    double cpuPercent = kUnavail;      // all-core normalized: dTime/(tickSec*cores)*100
    uint64_t workingSet = 0;           // bytes (NtQSI)
    uint64_t privateWorkingSet = kUnavailU64;  // bytes (PDH Working Set - Private)
    uint64_t commitBytes = 0;          // PrivateUsage, aka "Commit size"
    uint64_t ioReadBytes = 0;          // cumulative (NtQSI, self-check-gate covered)
    uint64_t ioWriteBytes = 0;
    double diskBytesPerSec = kUnavail; // NtQSI IO delta; scope = file+network+device total (labeled in UI)
    double netBytesPerSec = kUnavail;  // only when CAP_NET_ETW set, else kUnavail
    double pageFaultsPerSec = kUnavail;       // PageFaultCount delta (soft+hard, documented semantics)
    double contextSwitchesPerSec = kUnavail;  // thread ContextSwitches aggregated delta
    uint32_t handles = 0;
    uint32_t threads = 0;
    uint32_t gdiObjects = 0;   // 0 = not fetched (fetched on demand by DetailsProvider)
    uint32_t userObjects = 0;
    uint32_t flags = 0;
    std::wstring windowTitle;  // empty if none; refreshed every 5th tick
};

struct GpuAdapterInfo {
    std::wstring name;
    uint64_t luid = 0;
    double utilPercent = kUnavail;
    uint64_t memUsed = kUnavailU64;
    uint64_t memTotal = kUnavailU64;
    bool virtualAdapter = false;  // always false in sys.gpus (filtered); kept for diagnostics
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
    std::vector<double> perCorePercent;  // idle time already subtracted
    uint64_t physTotal = 0, physAvail = 0;
    uint64_t commitTotal = 0, commitLimit = 0;
    uint64_t kernPaged = 0, kernNonpaged = 0;
    uint32_t procCount = 0, handleTotal = 0, threadTotal = 0;
    double diskReadBps = kUnavail, diskWriteBps = kUnavail;
    double netRecvBps = kUnavail, netSendBps = kUnavail;
    double hardFaultsPerSec = kUnavail;  // system-wide (PDH \Memory)
    std::vector<GpuAdapterInfo> gpus;    // virtual display adapters filtered out
    double uptimeSec = 0;
};

enum SnapshotCap : uint32_t { CAP_NET_ETW = 1u << 0 };

struct Snapshot {
    uint64_t tickId = 0;
    double tickSec = 0;   // interval used for the deltas in this snapshot
    int64_t timestamp = 0;  // unix seconds
    bool degraded = false;   // true = Toolhelp slow path (compatibility mode)
    std::wstring degradeReason;
    uint32_t caps = 0;
    std::vector<ProcInfo> procs;        // ascending by pid
    std::vector<GpuProcUsage> gpuProcs; // sorted by (luid, pid)
    SystemInfo sys;
};

// Single-writer / multi-reader snapshot cell. UI copies the shared_ptr once per frame.
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
