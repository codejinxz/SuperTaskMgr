// 共享采集工具：NtQSI 动态绑定 + 结构解析、PDH 英文计数器通配数组、
// Toolhelp 兜底（架构第 5 节）。
#include "collect/CollectDetail.h"
#include "core/HandleGuard.h"
#include "core/Log.h"
#include <cwctype>
#include <memory>
#include <set>
#include <tlhelp32.h>

namespace stm {
namespace cd {

// ===========================================================================
// NtQSI：ntdll 动态绑定 + x64 结构布局。
// 布局来源：NtDoc / Geoff Chappell SYSTEM_PROCESS_INFORMATION（x64）。
// 这些偏移只有半官方文档；SelfCheckGate 会在信任快路径前，
// 用有文档的 PSAPI API 交叉校验解析出的值；
// 门限失败时服务降级到 Toolhelp+PSAPI。
// ===========================================================================
namespace {

using NtQuerySystemInformationFn = LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);

NtQuerySystemInformationFn NtqsiFn() {
    // ntdll 在每个 Windows 进程中总是已加载；且永不卸载。
    static NtQuerySystemInformationFn fn = []() -> NtQuerySystemInformationFn {
        const HMODULE h = ::GetModuleHandleW(L"ntdll.dll");
        return h ? reinterpret_cast<NtQuerySystemInformationFn>(
                       ::GetProcAddress(h, "NtQuerySystemInformation"))
                 : nullptr;
    }();
    return fn;
}

constexpr LONG kStatusInfoLengthMismatch = 0xC0000004L;
constexpr ULONG kSystemProcessInformation = 5;
constexpr ULONG kSystemProcessorPerformanceInformation = 8;

struct NtUnicodeStr {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

// SYSTEM_PROCESS_INFORMATION，x64。字段顺序/偏移必须与内核一致。
struct SysProcInfoX64 {
    ULONG NextEntryOffset;               // 0x000
    ULONG NumberOfThreads;               // 0x004
    LARGE_INTEGER WorkingSetPrivateSize; // 0x008
    ULONG HardFaultCount;                // 0x010
    ULONG NumberOfThreadsHighWatermark;  // 0x014
    ULONGLONG CycleTime;                 // 0x018
    LARGE_INTEGER CreateTime;            // 0x020
    LARGE_INTEGER UserTime;              // 0x028
    LARGE_INTEGER KernelTime;            // 0x030
    NtUnicodeStr ImageName;              // 0x038
    LONG BasePriority;                   // 0x048 (+0x04C pad)
    PVOID UniqueProcessId;               // 0x050
    PVOID InheritedFromUniqueProcessId;  // 0x058
    ULONG HandleCount;                   // 0x060
    ULONG SessionId;                     // 0x064
    ULONG_PTR UniqueProcessKey;          // 0x068
    SIZE_T PeakVirtualSize;              // 0x070
    SIZE_T VirtualSize;                  // 0x078
    ULONG PageFaultCount;                // 0x080 (+0x084 pad)
    SIZE_T PeakWorkingSetSize;           // 0x088
    SIZE_T WorkingSetSize;               // 0x090
    SIZE_T QuotaPeakPagedPoolUsage;      // 0x098
    SIZE_T QuotaPagedPoolUsage;          // 0x0A0
    SIZE_T QuotaPeakNonPagedPoolUsage;   // 0x0A8
    SIZE_T QuotaNonPagedPoolUsage;       // 0x0B0
    SIZE_T PagefileUsage;                // 0x0B8
    SIZE_T PeakPagefileUsage;            // 0x0C0
    SIZE_T PrivatePageCount;             // 0x0C8（= 私有提交，等同 PrivateUsage）
    LARGE_INTEGER ReadOperationCount;    // 0x0D0
    LARGE_INTEGER WriteOperationCount;   // 0x0D8
    LARGE_INTEGER OtherOperationCount;   // 0x0E0
    LARGE_INTEGER ReadTransferCount;     // 0x0E8
    LARGE_INTEGER WriteTransferCount;    // 0x0F0
    LARGE_INTEGER OtherTransferCount;    // 0x0F8
    // SYSTEM_THREAD_INFORMATION threads[NumberOfThreads] 位于 0x100
};
static_assert(sizeof(SysProcInfoX64) == 0x100, "SYSTEM_PROCESS_INFORMATION x64 layout drift");
static_assert(offsetof(SysProcInfoX64, CreateTime) == 0x20, "layout drift");
static_assert(offsetof(SysProcInfoX64, UniqueProcessId) == 0x50, "layout drift");
static_assert(offsetof(SysProcInfoX64, WorkingSetPrivateSize) == 0x08, "layout drift");
static_assert(offsetof(SysProcInfoX64, WorkingSetSize) == 0x90, "layout drift");
static_assert(offsetof(SysProcInfoX64, PageFaultCount) == 0x80, "layout drift");
static_assert(offsetof(SysProcInfoX64, HandleCount) == 0x60, "layout drift");
static_assert(offsetof(SysProcInfoX64, ReadTransferCount) == 0xE8, "layout drift");

// SYSTEM_THREAD_INFORMATION，x64（80 字节）。
struct SysThreadInfoX64 {
    LARGE_INTEGER KernelTime;   // 0x00
    LARGE_INTEGER UserTime;     // 0x08
    LARGE_INTEGER CreateTime;   // 0x10
    ULONG WaitTime;             // 0x18 (+0x01C pad)
    PVOID StartAddress;         // 0x20
    PVOID ClientIdUniqueProcess;// 0x28
    PVOID ClientIdUniqueThread; // 0x30
    LONG Priority;              // 0x38
    LONG BasePriority;          // 0x3C
    ULONG ContextSwitches;      // 0x40
    ULONG ThreadState;          // 0x44
    ULONG WaitReason;           // 0x48 (+0x04C pad)
};
static_assert(sizeof(SysThreadInfoX64) == 0x50, "SYSTEM_THREAD_INFORMATION x64 layout drift");
static_assert(offsetof(SysThreadInfoX64, ContextSwitches) == 0x40, "layout drift");
static_assert(offsetof(SysThreadInfoX64, ThreadState) == 0x44, "layout drift");
static_assert(offsetof(SysThreadInfoX64, WaitReason) == 0x48, "layout drift");

constexpr ULONG kThreadStateWaiting = 5;   // KTHREAD_STATE::Waiting
constexpr ULONG kWaitReasonSuspended = 5;  // KWAIT_REASON::Suspended

// SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION，x64（48 字节，每逻辑核心一条）。
struct SysCorePerfX64 {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;   // 含空闲时间（语义同 GetSystemTimes）
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    LARGE_INTEGER Reserved;
};
static_assert(sizeof(SysCorePerfX64) == 48, "SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION drift");

uint64_t LiU64(const LARGE_INTEGER& li) {
    return (static_cast<uint64_t>(li.HighPart) << 32) | static_cast<uint32_t>(li.LowPart);
}

void ParseProcBuffer(const BYTE* base, size_t cap, std::vector<NtProcRow>* out) {
    size_t off = 0;
    for (;;) {
        if (off + sizeof(SysProcInfoX64) > cap) break;
        // 条目在超对齐堆缓冲区内按 8 字节对齐。
        const auto* e = reinterpret_cast<const SysProcInfoX64*>(base + off);
        NtProcRow r;
        r.pid = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e->UniqueProcessId));
        r.parentPid = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e->InheritedFromUniqueProcessId));
        r.handles = e->HandleCount;
        r.threads = e->NumberOfThreads;
        r.sessionId = e->SessionId;
        r.createTime = LiU64(e->CreateTime);
        r.kernelTime = LiU64(e->KernelTime);
        r.userTime = LiU64(e->UserTime);
        r.workingSet = static_cast<uint64_t>(e->WorkingSetSize);
        // 私有工作集（字节），语义同 PDH "Working Set -
        // Private"（架构裁定：privateWorkingSet 的首要来源）。
        // 某些构建会短暂报告小的负值 -> 这里保持有符号，由下游
        // 视为"未知"。
        r.privateWs = e->WorkingSetPrivateSize.QuadPart;
        r.privateCommit = static_cast<uint64_t>(e->PrivatePageCount);
        r.pageFaults = e->PageFaultCount;
        r.ioReadBytes = LiU64(e->ReadTransferCount);
        r.ioWriteBytes = LiU64(e->WriteTransferCount);
        r.ioOtherBytes = LiU64(e->OtherTransferCount);
        if (e->ImageName.Length && e->ImageName.Buffer) {
            r.name.assign(e->ImageName.Buffer, e->ImageName.Length / sizeof(wchar_t));
        }
        // 内核未上报时使用约定名称（与 Toolhelp 一致）。
        if (r.name.empty() && r.pid == 0) r.name = L"System Idle Process";
        if (r.name.empty() && r.pid == 4) r.name = L"System";

        // 每线程聚合：上下文切换次数 + "完全挂起"检测。
        // 只有当*所有*线程都处于 Waiting 状态且 WaitReason=Suspended
        //（可靠的 UWP 冻结特征）时，才把进程视为挂起。
        // 语义含糊的情况刻意不设置该标志。
        uint64_t ctx = 0;
        bool allSusp = r.threads > 0;
        const size_t thrBase = off + sizeof(SysProcInfoX64);
        for (ULONG i = 0; i < r.threads; ++i) {
            const size_t to = thrBase + static_cast<size_t>(i) * sizeof(SysThreadInfoX64);
            if (to + sizeof(SysThreadInfoX64) > cap) { allSusp = false; break; }
            const auto* t = reinterpret_cast<const SysThreadInfoX64*>(base + to);
            ctx += t->ContextSwitches;
            if (t->ThreadState != kThreadStateWaiting || t->WaitReason != kWaitReasonSuspended) {
                allSusp = false;
            }
        }
        r.ctxSwitches = ctx;
        r.allThreadsSuspended = allSusp;
        out->push_back(std::move(r));
        if (e->NextEntryOffset == 0) break;
        off += e->NextEntryOffset;
    }
}

}  // namespace

bool NtqsiQueryProcesses(std::vector<NtProcRow>* out) {
    const NtQuerySystemInformationFn fn = NtqsiFn();
    if (!fn || !out) return false;
    ULONG size = 1u << 20;  // 全系统列表通常几百 KB
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::unique_ptr<BYTE[]> buf(new BYTE[size]);
        ULONG need = 0;
        const LONG st = fn(kSystemProcessInformation, buf.get(), size, &need);
        if (st == 0) {
            out->clear();
            ParseProcBuffer(buf.get(), size, out);
            return true;
        }
        if (st != kStatusInfoLengthMismatch) return false;
        const ULONG next = (need > size) ? need + 65536u : size * 2u;
        if (next <= size) return false;  // 溢出保护
        size = next;
    }
    return false;
}

// SYSTEM_PERFORMANCE_INFORMATION，x64：winternl.h 只暴露 BYTE[312]；
// 下方前部字段偏移遵循 NtDoc 布局。取值已在
// 本机与 GetPerformanceInfo 交叉核对（提交数/提交上限/
// 池页数全部一致）；该结构从 XP 到 Win11 保持稳定。
struct SysPerfInfoX64 {
    LARGE_INTEGER IdleProcessTime;       // 0x00
    LARGE_INTEGER IoReadTransferCount;   // 0x08
    LARGE_INTEGER IoWriteTransferCount;  // 0x10
    LARGE_INTEGER IoOtherTransferCount;  // 0x18
    ULONG IoReadOperationCount;          // 0x20
    ULONG IoWriteOperationCount;         // 0x24
    ULONG IoOtherOperationCount;         // 0x28
    ULONG AvailablePages;                // 0x2C
    ULONG CommittedPages;                // 0x30
    ULONG CommitLimit;                   // 0x34
    ULONG PeakCommitment;                // 0x38
    ULONG PageFaultCount;                // 0x3C
    ULONG CopyOnWriteCount;              // 0x40
    ULONG TransitionCount;               // 0x44
    ULONG CacheTransitionCount;          // 0x48
    ULONG DemandZeroCount;               // 0x4C
    ULONG PageReadCount;                 // 0x50
    ULONG PageReadIoCount;               // 0x54
    ULONG CacheReadCount;                // 0x58
    ULONG CacheIoCount;                  // 0x5C
    ULONG DirtyPagesWriteCount;          // 0x60
    ULONG DirtyWriteIoCount;             // 0x64
    ULONG MappedPagesWriteCount;         // 0x68
    ULONG MappedWriteIoCount;            // 0x6C
    ULONG PagedPoolPages;                // 0x70
    ULONG NonPagedPoolPages;             // 0x74
    ULONG PagedPoolAllocs;               // 0x78
    ULONG PagedPoolFrees;                // 0x7C
    ULONG NonPagedPoolAllocs;            // 0x80
    ULONG NonPagedPoolFrees;             // 0x84
    ULONG FreeSystemPtes;                // 0x88
    BYTE Tail[312 - 0x8C];               // 其余保留计数器
};
static_assert(sizeof(SysPerfInfoX64) == 312, "SYSTEM_PERFORMANCE_INFORMATION x64 size drift");
static_assert(offsetof(SysPerfInfoX64, CommittedPages) == 0x30, "layout drift");
static_assert(offsetof(SysPerfInfoX64, CommitLimit) == 0x34, "layout drift");
static_assert(offsetof(SysPerfInfoX64, PagedPoolPages) == 0x70, "layout drift");
static_assert(offsetof(SysPerfInfoX64, NonPagedPoolPages) == 0x74, "layout drift");

constexpr ULONG kSystemPerformanceInformation = 2;

bool NtqsiQueryMemoryCounters(SysMemCounters* out) {
    const NtQuerySystemInformationFn fn = NtqsiFn();
    if (!fn || !out) return false;
    const std::unique_ptr<BYTE[]> buf(new BYTE[sizeof(SysPerfInfoX64)]);
    const LONG st = fn(kSystemPerformanceInformation, buf.get(), sizeof(SysPerfInfoX64), nullptr);
    if (st != 0) return false;
    const auto* p = reinterpret_cast<const SysPerfInfoX64*>(buf.get());
    out->commitPages = p->CommittedPages;
    out->commitLimitPages = p->CommitLimit;
    out->pagedPoolPages = p->PagedPoolPages;
    out->nonPagedPoolPages = p->NonPagedPoolPages;
    return true;
}

bool NtqsiQueryPerCoreTimes(std::vector<CoreTimes>* out) {
    const NtQuerySystemInformationFn fn = NtqsiFn();
    if (!fn || !out) return false;
    SYSTEM_INFO si{};
    ::GetNativeSystemInfo(&si);
    if (si.dwNumberOfProcessors == 0) return false;
    const ULONG size = si.dwNumberOfProcessors * sizeof(SysCorePerfX64);
    const std::unique_ptr<BYTE[]> buf(new BYTE[size]);
    ULONG need = 0;
    LONG st = fn(kSystemProcessorPerformanceInformation, buf.get(), size, &need);
    if (st == kStatusInfoLengthMismatch && need >= size) {
        st = fn(kSystemProcessorPerformanceInformation, buf.get(), need, &need);
    }
    if (st != 0) return false;
    const ULONG count = need >= sizeof(SysCorePerfX64)
                            ? static_cast<ULONG>(need / sizeof(SysCorePerfX64))
                            : si.dwNumberOfProcessors;
    out->clear();
    out->reserve(count);
    for (ULONG i = 0; i < count; ++i) {
        const auto* c = reinterpret_cast<const SysCorePerfX64*>(buf.get() + i * sizeof(SysCorePerfX64));
        CoreTimes t;
        t.idle = LiU64(c->IdleTime);
        t.kernel = LiU64(c->KernelTime);
        t.user = LiU64(c->UserTime);
        out->push_back(t);
    }
    return !out->empty();
}

// ===========================================================================
// PDH：英文计数器路径 + 通配数组采集模式。
// 通配计数器（"\Process(*)\X"）只添加一次（经
// PdhAddEnglishCounterW + PdhGetCounterInfo 本地化），每次
// PdhCollectQueryData 之后接 PdhGetFormattedCounterArrayW，为每个活动
// 实例返回一个值——实例名带完整 "#N" 后缀。这是跟踪变化实例集的
// 有文档的做法（MSDN "Enumerating Object Instances"）；
// 永远无需重建查询，也让每次 tick 的开销保持低廉。
// 注意：PdhGetCounterInfo 的 szInstanceName 会去掉 "#N" 标记（它报告
// 的是父实例名），因此每实例身份必须来自格式化数组
// 的名称，绝不能来自 PdhGetCounterInfo。
// ===========================================================================

bool PdhLocalizeEnglishPath(const wchar_t* englishPath, std::wstring* localizedTemplate) {
    if (!englishPath || !localizedTemplate) return false;
    // 探测：把（未展开的）英文通配路径加到一个临时查询并读回
    // 其本地化完整路径。PdhAddEnglishCounterW 解析语言无关名称；
    // 本地化模板才是 PdhAddCounterW 所需要的
    //（计数器路径必须本地化，R5 #9b）。
    PDH_HQUERY probe = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &probe) != ERROR_SUCCESS) return false;
    bool ok = false;
    PDH_HCOUNTER wild = nullptr;
    if (PdhAddEnglishCounterW(probe, englishPath, 0, &wild) == ERROR_SUCCESS) {
        DWORD size = 0;
        if (PdhGetCounterInfoW(wild, FALSE, &size, nullptr) == PDH_MORE_DATA && size > 0) {
            std::vector<BYTE> ibuf(size);
            auto* ci = reinterpret_cast<PDH_COUNTER_INFO_W*>(ibuf.data());
            if (PdhGetCounterInfoW(wild, FALSE, &size, ci) == ERROR_SUCCESS && ci->szFullPath) {
                localizedTemplate->assign(ci->szFullPath);
                ok = true;
            }
        }
        PdhRemoveCounter(wild);
    }
    PdhCloseQuery(probe);
    return ok;
}

bool PdhAddWildcardCounter(PDH_HQUERY query, const std::wstring& localizedTemplate,
                           PDH_HCOUNTER* out) {
    return query != nullptr && out != nullptr &&
           PdhAddCounterW(query, localizedTemplate.c_str(), 0, out) == ERROR_SUCCESS;
}

bool PdhFmtArrayDouble(PDH_HCOUNTER h, std::vector<PdhArrayItem>* out) {
    if (!h || !out) return false;
    out->clear();
    DWORD size = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(h, PDH_FMT_DOUBLE, &size, &count, nullptr) != PDH_MORE_DATA) {
        return false;
    }
    for (int attempt = 0; attempt < 4; ++attempt) {
        std::vector<BYTE> buf(size);
        PDH_FMT_COUNTERVALUE_ITEM_W* items =
            reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        const PDH_STATUS st =
            PdhGetFormattedCounterArrayW(h, PDH_FMT_DOUBLE, &size, &count, items);
        if (st == ERROR_SUCCESS) {
            out->reserve(count);
            for (DWORD i = 0; i < count; ++i) {
                PdhArrayItem it;
                it.name = items[i].szName ? items[i].szName : L"";
                it.valid = (items[i].FmtValue.CStatus == ERROR_SUCCESS);
                it.value = it.valid ? items[i].FmtValue.doubleValue : 0.0;
                out->push_back(std::move(it));
            }
            return true;
        }
        if (st != PDH_MORE_DATA) return false;  // 实例集增减：用新大小重试
    }
    return false;
}

bool PdhCollect(PDH_HQUERY q) { return q && PdhCollectQueryData(q) == ERROR_SUCCESS; }

bool PdhFmtDouble(PDH_HCOUNTER h, double* out) {
    PDH_FMT_COUNTERVALUE v{};
    if (!h || PdhGetFormattedCounterValue(h, PDH_FMT_DOUBLE, nullptr, &v) != ERROR_SUCCESS) {
        return false;
    }
    if (v.CStatus != ERROR_SUCCESS) return false;  // 例如只采集了一个样本
    if (out) *out = v.doubleValue;
    return true;
}

// ===========================================================================
// Toolhelp 兜底
// ===========================================================================
bool ToolhelpEnumerate(std::vector<ToolhelpRow>* out) {
    if (!out) return false;
    out->clear();
    UniqueHandle snap(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap) return false;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    if (!::Process32FirstW(snap.get(), &e)) return false;
    do {
        ToolhelpRow r;
        r.pid = e.th32ProcessID;
        r.parentPid = e.th32ParentProcessID;
        r.threads = e.cntThreads;
        r.name = e.szExeFile;
        out->push_back(std::move(r));
    } while (::Process32NextW(snap.get(), &e));
    return true;
}

uint32_t ToolhelpThreadCountOf(uint32_t pid) {
    UniqueHandle snap(::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
    if (!snap) return 0;
    THREADENTRY32 t{};
    t.dwSize = sizeof(t);
    uint32_t n = 0;
    if (::Thread32First(snap.get(), &t)) {
        do {
            if (t.th32OwnerProcessID == pid) ++n;
        } while (::Thread32Next(snap.get(), &t));
    }
    return n;
}

}  // namespace cd
}  // namespace stm
