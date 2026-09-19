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
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "core/ProcData.h"

namespace stm {
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
    // PDH：磁盘速率（通配；实例集合由 PDH 自身维护）+
    // 全系统硬缺页（普通计数器）。
    PDH_HQUERY pdhQuery_ = nullptr;
    PDH_HCOUNTER diskRead_ = nullptr, diskWrite_ = nullptr, hardFaults_ = nullptr;
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

    bool Start();   // 幂等；失败返回 false 并记错误日志（非管理员等）
    void Stop();    // 幂等；停止会话并 join 消费线程
    bool Running(); // ETW 会话存活期间为 true
    // pid -> 自 Start 起的累计收发字节（线程安全拷贝）。
    void CopyCumulative(std::unordered_map<uint32_t, uint64_t>* out) const;
    uint64_t TotalEvents() const;  // 自 Start 起解析的收发事件数
    // 存在同名 ETW 会话时为 true（selftest 清理检查用）。
    static bool SessionExists(const wchar_t* name);

private:
    struct Agg { uint64_t recv = 0, send = 0; };
    static void WINAPI OnEvent(PEVENT_RECORD rec);  // 经 .Context 蹦床转发
    void HandleEvent(PEVENT_RECORD rec);
    void Consume();  // 消费线程：OpenTrace -> ProcessTrace -> CloseTrace

    TRACEHANDLE session_ = 0;       // 来自 StartTraceW
    TRACEHANDLE openTrace_ = 0;     // 来自 OpenTraceW（由消费线程关闭）
    std::wstring sessionName_;      // L"SuperTaskMgr-Net-<pid>"
    std::vector<BYTE> stopProps_;   // ControlTraceW 缓冲（由 mu_ 保护）
    std::thread consumer_;
    mutable std::mutex mu_;  // 保护下方所有成员（回调 + 所属线程）
    std::unordered_map<uint32_t, Agg> bytes_;
    uint64_t totalEvents_ = 0;
    uint64_t parseFails_ = 0;
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
