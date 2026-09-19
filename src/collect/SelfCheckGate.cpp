// SelfCheckGate（架构第 4 节）：在信任 NtQSI 快路径之前，先在存活进程上
// 用有文档的 API 交叉校验其半官方的字段偏移。
// 任何超容差都会把整个服务降级到
// Toolhelp+PSAPI 兼容路径，而不是输出错误数据。
#include "collect/CollectDetail.h"
#include "core/HandleGuard.h"
#include "core/Str.h"
#include <psapi.h>
#include <algorithm>
#include <unordered_map>
#include <cstdlib>

namespace stm {
namespace cd {

namespace {

constexpr int64_t k100nsPerSec = 10'000'000LL;

uint64_t FtU64(const FILETIME& f) {
    return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

// 相对容差 + 绝对余量。余量用于吸收 NtQSI 与每进程 API 读取之间的
// 微小采样偏差（两次调用相隔微秒级，
// 但目标进程仍在运行）。
bool WithinRelative(uint64_t a, uint64_t b, double rel, uint64_t slack) {
    const uint64_t hi = a > b ? a : b;
    const uint64_t diff = a > b ? a - b : b - a;
    const double tol = rel * static_cast<double>(hi) + static_cast<double>(slack);
    return static_cast<double>(diff) <= tol;
}

}  // namespace

GateResult RunSelfCheckGate() {
    std::vector<NtProcRow> rows;
    if (!NtqsiQueryProcesses(&rows) || rows.empty()) {
        return {true, L"NtQuerySystemInformation 不可用，已切换 Toolhelp+PSAPI 兼容模式"};
    }

    int validated = 0;
    for (const NtProcRow& r : rows) {
        if (validated >= 3) break;
        if (r.pid == 0) continue;          // 空闲行没有对应的 PSAPI 数据
        if (r.name.empty() && r.pid != 4) continue;

        UniqueHandle h(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, r.pid));
        if (!h) continue;  // 无法交叉校验该进程；尝试下一个候选

        // --- 1. CPU 时间：kernel/user 各自与 GetProcessTimes 相差 ±1s 内 ---
        FILETIME ct{}, ex{}, kt{}, ut{};
        if (!::GetProcessTimes(h.get(), &ct, &ex, &kt, &ut)) continue;
        const uint64_t apiK = FtU64(kt), apiU = FtU64(ut);
        const int64_t dK = static_cast<int64_t>(apiK) - static_cast<int64_t>(r.kernelTime);
        const int64_t dU = static_cast<int64_t>(apiU) - static_cast<int64_t>(r.userTime);
        if (dK > k100nsPerSec || dK < -k100nsPerSec) {
            return {true, Fmt(L"NtQSI 自校验未通过（PID {} 内核时间偏差>1s），已切换兼容模式", r.pid)};
        }
        if (dU > k100nsPerSec || dU < -k100nsPerSec) {
            return {true, Fmt(L"NtQSI 自校验未通过（PID {} 用户时间偏差>1s），已切换兼容模式", r.pid)};
        }

        // --- 2. 工作集相差 ±25% 内 ---
        PROCESS_MEMORY_COUNTERS_EX mc{};
        mc.cb = sizeof(mc);
        if (::GetProcessMemoryInfo(h.get(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mc),
                                   sizeof(mc))) {
            const bool wsOk =
                WithinRelative(mc.WorkingSetSize, r.workingSet, 0.25, 1024ull * 1024);
            if (!wsOk) {
                return {true, Fmt(L"NtQSI 自校验未通过（PID {} 工作集偏差超25%），已切换兼容模式", r.pid)};
            }
        }

        // --- 3. IO 传输字节相差 ±25% 内（+1 MiB 余量）---
        IO_COUNTERS io{};
        if (::GetProcessIoCounters(h.get(), &io)) {
            const uint64_t apiIo = io.ReadTransferCount + io.WriteTransferCount +
                                   io.OtherTransferCount;
            const uint64_t qsiIo = r.ioReadBytes + r.ioWriteBytes + r.ioOtherBytes;
            const bool ioOk = WithinRelative(apiIo, qsiIo, 0.25, 1024ull * 1024);
            if (!ioOk) {
                return {true, Fmt(L"NtQSI 自校验未通过（PID {} IO 字节偏差超25%），已切换兼容模式", r.pid)};
            }
        }

        // --- 4. 句柄数相差 ±10% 内（+4）---
        DWORD hc = 0;
        if (::GetProcessHandleCount(h.get(), &hc)) {
            const bool hcOk = WithinRelative(hc, r.handles, 0.10, 4);
            if (!hcOk) {
                return {true, Fmt(L"NtQSI 自校验未通过（PID {} 句柄数偏差超10%），已切换兼容模式", r.pid)};
            }
        }

        // --- 5. 线程数相差 ±10% 内（+2）---
        const uint32_t tc = ToolhelpThreadCountOf(r.pid);
        if (tc > 0) {
            const bool tcOk = WithinRelative(tc, r.threads, 0.10, 2);
            if (!tcOk) {
                return {true, Fmt(L"NtQSI 自校验未通过（PID {} 线程数偏差超10%），已切换兼容模式", r.pid)};
            }
        }

        ++validated;
    }

    // --- 6. 私有工作集：NtQSI WorkingSetPrivateSize 对照一次性 PDH
    // \Process(*)\Working Set - Private（±25% + 2 MiB）。按架构裁定，
    // PDH 恰好只在这里使用，绝不进入 tick 循环；tick 路径随后
    // 直接从 NtQSI 填充 privateWorkingSet。PDH 不可用或没有
    // 可比进程时只是跳过本项（其余五项仍会校验布局）；
    // 真正的不匹配才会像其他项一样把
    // 整个快路径降级。
    GateResult item6;  // 原因为空 = 本项通过或被跳过
    {
        std::wstring idTemplate, wsTemplate;
        if (PdhLocalizeEnglishPath(L"\\Process(*)\\ID Process", &idTemplate) &&
            PdhLocalizeEnglishPath(L"\\Process(*)\\Working Set - Private", &wsTemplate)) {
            PDH_HQUERY q = nullptr;
            PDH_HCOUNTER idH = nullptr, wsH = nullptr;
            if (PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS &&
                PdhAddWildcardCounter(q, idTemplate, &idH) &&
                PdhAddWildcardCounter(q, wsTemplate, &wsH) && PdhCollect(q)) {
                std::vector<PdhArrayItem> ids, wss;
                if (PdhFmtArrayDouble(idH, &ids) && PdhFmtArrayDouble(wsH, &wss)) {
                    // 按索引联接：两个数组枚举的是同一次采集的同一批
                    // Process 对象实例表（名称不含 "#N"）。
                    std::unordered_map<uint32_t, double> pwsByPid;
                    const size_t n = std::min(ids.size(), wss.size());
                    for (size_t i = 0; i < n; ++i) {
                        if (ids[i].valid && wss[i].valid && ids[i].value > 0.0 &&
                            ids[i].value < 4294967040.0) {
                            pwsByPid[static_cast<uint32_t>(ids[i].value)] = wss[i].value;
                        }
                    }
                    int checked = 0;
                    for (const NtProcRow& r : rows) {
                        if (checked >= 2) break;
                        if (r.pid == 0 || r.privateWs <= 0) continue;
                        const auto it = pwsByPid.find(r.pid);
                        if (it == pwsByPid.end()) continue;  // 实例已消失 / 未暴露
                        const uint64_t ntqsi = static_cast<uint64_t>(r.privateWs);
                        const uint64_t pdh = static_cast<uint64_t>(it->second);
                        if (!WithinRelative(ntqsi, pdh, 0.25, 2048ull * 1024)) {
                            item6 = {true, Fmt(L"NtQSI 自校验未通过（PID {} 私有工作集偏差超25%），"
                                               L"已切换兼容模式",
                                               r.pid)};
                            break;
                        }
                        ++checked;
                    }
                }
            }
            PdhCloseQuerySafe(&q);
        }
    }
    if (item6.degraded) return item6;

    if (validated == 0) {
        return {true, L"NtQSI 自校验无可用对照进程，已切换 Toolhelp+PSAPI 兼容模式"};
    }
    return {false, L""};
}

}  // namespace cd
}  // namespace stm
