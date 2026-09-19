// SelfCheckGate（架构第 4 节 + 维护轮 10 加固）：在信任 NtQSI 快路径之前，
// 先在存活参考进程上用有文档的 API 交叉校验其半官方的字段偏移，共 6 项，
// 逐项产出测量值对比报告（SelfCheckGate.h::GateItem）。
// 加固（消除误报面）：
//   1. 参考进程每轮重选——优先自身进程，其次存活 > 60s 的系统进程；
//      不再拿 NtQSI 列表前 3 行（可能是刚启动/临退出进程）当对照。
//   2. 任一轮未通过自动重试至多 2 次（间隔 200ms），吸收参考进程中途
//      退出、单次采样噪声与安全软件对 NtQSI 的间歇性篡改。
//   3. 连续 2 轮全部通过才判通过；仍失败才降级到 Toolhelp+PSAPI 兼容路径。
// 全部轮次、逐项判定与重试决策写日志（模块 "selfcheck"）。
#include "collect/SelfCheckGate.h"
#include "core/HandleGuard.h"
#include "core/Log.h"
#include "core/Str.h"
#include <psapi.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace stm {
namespace cd {

namespace {

constexpr int64_t k100nsPerSec = 10'000'000LL;

// 6 项的固定顺序（GateReport::items 恒按此顺序，CollectService/UI 依赖）。
enum ItemIdx { kCpu = 0, kWs, kIo, kHandles, kThreads, kPrivWs, kItemCount };

const wchar_t* const kItemNames[kItemCount] = {
    L"CPU 时间字段",   // NtQSI kernel/userTime vs GetProcessTimes
    L"工作集内存",     // NtQSI WorkingSet vs GetProcessMemoryInfo
    L"IO 计数器",      // NtQSI IO 字节 vs GetProcessIoCounters
    L"句柄数",         // NtQSI handles vs GetProcessHandleCount
    L"线程数",         // NtQSI threads vs Toolhelp 快照
    L"私有工作集",     // NtQSI WorkingSetPrivateSize vs 一次性 PDH
};

uint64_t FtU64(const FILETIME& f) {
    return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

uint64_t Now100ns() {
    FILETIME f{};
    ::GetSystemTimeAsFileTime(&f);
    return FtU64(f);
}

// 相对容差 + 绝对余量。余量用于吸收 NtQSI 与每进程 API 读取之间的
// 微小采样偏差（两次调用相隔微秒级，但目标进程仍在运行）。
bool WithinRelative(uint64_t a, uint64_t b, double rel, uint64_t slack) {
    const uint64_t hi = a > b ? a : b;
    const uint64_t diff = a > b ? a - b : b - a;
    const double tol = rel * static_cast<double>(hi) + static_cast<double>(slack);
    return static_cast<double>(diff) <= tol;
}

// ---------------------------------------------------------------------------
// 参考进程选择（每轮重选）：优先自身进程，其次存活 > 60s 的系统进程，
// 按创建时间从旧到新取前 3。
// ---------------------------------------------------------------------------
struct Ref {
    uint32_t pid = 0;
    std::wstring name;
    size_t rowIdx = 0;  // 指回本轮 rows 的下标
};

bool IsKnownSystemImage(const std::wstring& name) {
    static const wchar_t* const kNames[] = {
        L"system", L"registry", L"memory compression", L"smss.exe", L"csrss.exe",
        L"wininit.exe", L"winlogon.exe", L"services.exe", L"lsass.exe", L"svchost.exe",
        L"dwm.exe", L"explorer.exe", L"fontdrvhost.exe", L"spoolsv.exe", L"taskhostw.exe",
        L"sihost.exe", L"ctfmon.exe", L"searchhost.exe", L"runtimebroker.exe",
        L"audiodg.exe", L"msmpeng.exe", L"wudfhost.exe"};
    for (const wchar_t* n : kNames) {
        if (_wcsicmp(name.c_str(), n) == 0) return true;
    }
    return false;
}

// 映像路径位于 %SystemRoot% 下（有文档判定，比名单更通用）。
bool ImageUnderSystemDir(HANDLE h) {
    wchar_t buf[1024];
    DWORD n = static_cast<DWORD>(std::size(buf));
    if (!::QueryFullProcessImageNameW(h, 0, buf, &n)) return false;
    wchar_t sysDir[1024];
    const UINT len = ::GetSystemWindowsDirectoryW(sysDir, static_cast<UINT>(std::size(sysDir)));
    if (len == 0 || len >= std::size(sysDir)) return false;
    return n >= len && _wcsnicmp(buf, sysDir, len) == 0;
}

std::vector<Ref> SelectRefProcs(const std::vector<NtProcRow>& rows) {
    const uint32_t selfPid = ::GetCurrentProcessId();
    const uint64_t now = Now100ns();
    constexpr uint64_t kMinAge = 60ull * static_cast<uint64_t>(k100nsPerSec);
    // (优先级, 创建时间, rows 下标)：自身=2；系统且存活>60s=1。
    std::vector<std::tuple<int, uint64_t, size_t>> cands;
    for (size_t i = 0; i < rows.size(); ++i) {
        const NtProcRow& r = rows[i];
        if (r.pid == 0) continue;  // 空闲行没有对应的 PSAPI 数据
        if (r.name.empty() && r.pid != 4) continue;
        if (r.pid == selfPid) {
            cands.emplace_back(2, r.createTime, i);
            continue;
        }
        if (now <= r.createTime || now - r.createTime < kMinAge) continue;  // 存活 > 60s
        UniqueHandle h(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, r.pid));
        if (!h) continue;
        if (!ImageUnderSystemDir(h.get()) && !IsKnownSystemImage(r.name)) continue;
        cands.emplace_back(1, r.createTime, i);
    }
    std::stable_sort(cands.begin(), cands.end(),
                     [](const auto& a, const auto& b) {
                         if (std::get<0>(a) != std::get<0>(b))
                             return std::get<0>(a) > std::get<0>(b);  // 优先级高在前
                         return std::get<1>(a) < std::get<1>(b);      // 越老越稳定
                     });
    std::vector<Ref> refs;
    refs.reserve(3);
    for (const auto& c : cands) {
        if (refs.size() >= 3) break;
        const NtProcRow& r = rows[std::get<2>(c)];
        refs.push_back({r.pid, r.name, std::get<2>(c)});
    }
    return refs;
}

// 每项的累计对照结果：checked = 本轮完成的有效对比数；fail = 首个超差的
// 测量值细节（含 PID）。checked==0 表示本项本轮未能运行（诚实跳过）。
struct ItemAcc {
    int checked = 0;
    std::wstring fail;
};

// 逐项填充 GateItem；passed 仅在 ran 且无失败对比时为 true。
void FillItem(std::vector<GateItem>* items, int idx, const ItemAcc& acc,
              const std::wstring& passDetail, const std::wstring& skipDetail) {
    GateItem& it = (*items)[idx];
    it.ran = acc.checked > 0;
    it.passed = it.ran && acc.fail.empty();
    it.detail = !it.ran ? skipDetail : (acc.fail.empty() ? passDetail : acc.fail);
}

// ---------------------------------------------------------------------------
// 单轮自检。rows 为本轮新取的 NtQSI 快照；恒产出 6 项结果。
// 返回本轮是否通过（NtQSI 可用 && >=1 个参考进程完成 CPU 对照 && 全部
// ran 项通过）；failReason 输出一行原因（通过时为空）。
// ---------------------------------------------------------------------------
bool RunGateRound(const std::vector<NtProcRow>& rows, bool ntsiOk,
                  std::vector<GateItem>* items, std::wstring* failReason, int attempt) {
    items->assign(kItemCount, GateItem{});
    for (int i = 0; i < kItemCount; ++i) (*items)[i].name = kItemNames[i];
    failReason->clear();

    if (!ntsiOk) {
        // NtQSI 本身不可用：6 项都无从谈起（诚实标注跳过原因）。
        const std::wstring detail =
            L"NtQuerySystemInformation 调用失败或被拦截，取不到快路径读数";
        for (GateItem& it : *items) {
            it.ran = false;
            it.passed = false;
            it.detail = detail;
        }
        *failReason = L"NtQuerySystemInformation 不可用，已切换 Toolhelp+PSAPI 兼容模式";
        STM_LOG_WARN("selfcheck", L"第 {} 轮：NtQSI 不可用，6 项均未运行", attempt);
        return false;
    }

    const std::vector<Ref> refs = SelectRefProcs(rows);
    std::wstring refsText;
    for (const Ref& r : refs) {
        if (!refsText.empty()) refsText += L", ";
        refsText += Fmt(L"{}({})", r.name.empty() ? L"pid" + std::to_wstring(r.pid) : r.name,
                        r.pid);
    }
    STM_LOG_INFO("selfcheck", L"第 {} 轮：参考进程 [{}]（优先自身，其次存活>60s 的系统进程）",
                 attempt, refsText.empty() ? std::wstring(L"无") : refsText);

    if (refs.empty()) {
        const std::wstring detail = L"跳过：本轮无可用参考进程（自身行缺失或全部对照 API 失败）";
        for (GateItem& it : *items) {
            it.ran = false;
            it.passed = false;
            it.detail = detail;
        }
        *failReason = L"NtQSI 自校验无可用对照进程，已切换 Toolhelp+PSAPI 兼容模式";
        STM_LOG_WARN("selfcheck", L"第 {} 轮：无可用参考进程，6 项均未运行", attempt);
        return false;
    }

    // --- 第 1-5 项：逐参考进程，文档化 API 交叉比对 ------------------------
    ItemAcc acc[kItemCount];
    for (const Ref& ref : refs) {
        const NtProcRow& r = rows[ref.rowIdx];
        UniqueHandle h(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ref.pid));
        if (!h) continue;  // 参考进程中途退出：本轮对照数少一个，必要时触发重试
        FILETIME ct{}, ex{}, kt{}, ut{};
        if (::GetProcessTimes(h.get(), &ct, &ex, &kt, &ut)) {
            const uint64_t apiK = FtU64(kt), apiU = FtU64(ut);
            ++acc[kCpu].checked;
            const int64_t dK = static_cast<int64_t>(apiK) - static_cast<int64_t>(r.kernelTime);
            const int64_t dU = static_cast<int64_t>(apiU) - static_cast<int64_t>(r.userTime);
            constexpr int64_t kTol = k100nsPerSec;  // ±1s
            if (dK > kTol || dK < -kTol) {
                acc[kCpu].fail =
                    Fmt(L"PID {} 内核时间 NtQSI={:.2f}s vs GetProcessTimes={:.2f}s（偏差>±1s）",
                        r.pid, static_cast<double>(r.kernelTime) / 1e7,
                        static_cast<double>(apiK) / 1e7);
            } else if (dU > kTol || dU < -kTol) {
                acc[kCpu].fail =
                    Fmt(L"PID {} 用户时间 NtQSI={:.2f}s vs GetProcessTimes={:.2f}s（偏差>±1s）",
                        r.pid, static_cast<double>(r.userTime) / 1e7,
                        static_cast<double>(apiU) / 1e7);
            }
        }
        PROCESS_MEMORY_COUNTERS_EX mc{};
        mc.cb = sizeof(mc);
        if (::GetProcessMemoryInfo(h.get(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mc),
                                   sizeof(mc))) {
            ++acc[kWs].checked;
            if (!WithinRelative(mc.WorkingSetSize, r.workingSet, 0.25, 1024ull * 1024)) {
                acc[kWs].fail = Fmt(L"PID {} 工作集 NtQSI={} vs GetProcessMemoryInfo={}（偏差>25%）",
                                    r.pid, FormatBytes(r.workingSet),
                                    FormatBytes(mc.WorkingSetSize));
            }
        }
        IO_COUNTERS io{};
        if (::GetProcessIoCounters(h.get(), &io)) {
            ++acc[kIo].checked;
            const uint64_t apiIo =
                io.ReadTransferCount + io.WriteTransferCount + io.OtherTransferCount;
            const uint64_t qsiIo = r.ioReadBytes + r.ioWriteBytes + r.ioOtherBytes;
            if (!WithinRelative(apiIo, qsiIo, 0.25, 1024ull * 1024)) {
                acc[kIo].fail = Fmt(L"PID {} IO 传输字节 NtQSI={} vs GetProcessIoCounters={}（偏差>25%）",
                                    r.pid, FormatBytes(qsiIo), FormatBytes(apiIo));
            }
        }
        DWORD hc = 0;
        if (::GetProcessHandleCount(h.get(), &hc)) {
            ++acc[kHandles].checked;
            if (!WithinRelative(hc, r.handles, 0.10, 4)) {
                acc[kHandles].fail = Fmt(L"PID {} 句柄数 NtQSI={} vs GetProcessHandleCount={}（偏差>10%）",
                                         r.pid, r.handles, static_cast<uint64_t>(hc));
            }
        }
        const uint32_t tc = ToolhelpThreadCountOf(r.pid);
        if (tc > 0) {
            ++acc[kThreads].checked;
            if (!WithinRelative(tc, r.threads, 0.10, 2)) {
                acc[kThreads].fail = Fmt(L"PID {} 线程数 NtQSI={} vs Toolhelp={}（偏差>10%）",
                                         r.pid, r.threads, static_cast<uint64_t>(tc));
            }
        }
    }

    // --- 第 6 项：私有工作集 vs 一次性 PDH --------------------------------
    // PDH 恰好只在这里使用，绝不进入 tick 循环（架构裁定）。不可用或没有
    // 可比进程只是跳过本项（诚实标注），不参与失败判定。
    std::wstring privWsSkip =
        L"跳过：PDH「Working Set - Private」计数器不可用（本项不参与判定）";
    {
        bool pdhTried = false;
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
                    pdhTried = true;
                    for (const Ref& ref : refs) {  // 已按稳定性排序，取前 2 个可比者
                        if (acc[kPrivWs].checked >= 2) break;
                        const NtProcRow& r = rows[ref.rowIdx];
                        if (r.pid == 0 || r.privateWs <= 0) continue;
                        const auto it = pwsByPid.find(r.pid);
                        if (it == pwsByPid.end()) continue;  // 实例已消失 / 未暴露
                        ++acc[kPrivWs].checked;
                        const uint64_t ntqsi = static_cast<uint64_t>(r.privateWs);
                        const uint64_t pdh = static_cast<uint64_t>(it->second);
                        if (!WithinRelative(ntqsi, pdh, 0.25, 2048ull * 1024)) {
                            acc[kPrivWs].fail =
                                Fmt(L"PID {} 私有工作集 NtQSI={} vs PDH={}（偏差>25%）", r.pid,
                                    FormatBytes(ntqsi), FormatBytes(pdh));
                        }
                    }
                }
            }
            PdhCloseQuerySafe(&q);
        }
        privWsSkip = pdhTried ? L"跳过：参考进程未出现在 PDH 实例表中（本项不参与判定）"
                              : L"跳过：PDH「Working Set - Private」计数器不可用（本项不参与判定）";
    }

    // --- 汇总逐项结果 ------------------------------------------------------
    FillItem(items, kCpu, acc[kCpu],
             Fmt(L"NtQSI 与 GetProcessTimes 一致：{} 个参考进程内核/用户时间偏差均 ≤±1s",
                 acc[kCpu].checked),
             L"跳过：参考进程的 GetProcessTimes 均不可读（进程中途退出？）");
    FillItem(items, kWs, acc[kWs],
             Fmt(L"工作集一致：{} 个参考进程 NtQSI vs GetProcessMemoryInfo 偏差 ≤25%（+1MiB）",
                 acc[kWs].checked),
             L"跳过：参考进程的 GetProcessMemoryInfo 均不可读");
    FillItem(items, kIo, acc[kIo],
             Fmt(L"IO 传输字节一致：{} 个参考进程 NtQSI vs GetProcessIoCounters 偏差 ≤25%（+1MiB）",
                 acc[kIo].checked),
             L"跳过：参考进程的 GetProcessIoCounters 均不可读");
    FillItem(items, kHandles, acc[kHandles],
             Fmt(L"句柄数一致：{} 个参考进程 NtQSI vs GetProcessHandleCount 偏差 ≤10%（+4）",
                 acc[kHandles].checked),
             L"跳过：参考进程的 GetProcessHandleCount 均不可读");
    FillItem(items, kThreads, acc[kThreads],
             Fmt(L"线程数一致：{} 个参考进程 NtQSI vs Toolhelp 偏差 ≤10%（+2）",
                 acc[kThreads].checked),
             L"跳过：Toolhelp 快照未覆盖参考进程");
    FillItem(items, kPrivWs, acc[kPrivWs],
             Fmt(L"私有工作集一致：{} 个参考进程 NtQSI vs 一次性 PDH「Working Set - Private」偏差 ≤25%",
                 acc[kPrivWs].checked),
             privWsSkip);

    // 逐项判定写日志（全部 6 项，无论成败）。
    for (const GateItem& it : *items) {
        if (!it.ran) {
            STM_LOG_INFO("selfcheck", L"第 {} 轮 [跳过] {}：{}", attempt,
                         it.name == nullptr ? L"" : it.name, it.detail);
        } else if (it.passed) {
            STM_LOG_INFO("selfcheck", L"第 {} 轮 [通过] {}：{}", attempt, it.name, it.detail);
        } else {
            STM_LOG_WARN("selfcheck", L"第 {} 轮 [失败] {}：{}", attempt, it.name, it.detail);
        }
    }

    // 本轮通过 = CPU 项至少对照过一个进程（否则与旧"validated==0"语义一致，
    // 视为无可用对照）且全部 ran 项通过。
    bool pass = (*items)[kCpu].ran;
    for (const GateItem& it : *items) {
        if (it.ran && !it.passed) pass = false;
    }
    if (!pass && failReason->empty()) {
        if (!(*items)[kCpu].ran) {
            *failReason = L"NtQSI 自校验无可用对照进程，已切换 Toolhelp+PSAPI 兼容模式";
        } else {
            for (const GateItem& it : *items) {
                if (it.ran && !it.passed) {
                    *failReason = Fmt(L"NtQSI 自校验未通过：{}", it.detail);
                    break;
                }
            }
        }
    }
    if (pass) {
        STM_LOG_INFO("selfcheck", L"第 {} 轮通过", attempt);
    } else {
        STM_LOG_WARN("selfcheck", L"第 {} 轮未通过：{}", attempt, *failReason);
    }
    return pass;
}

}  // namespace

GateReport RunSelfCheckGateReported() {
    GateReport rep;
    constexpr int kMaxAttempts = 3;  // 首轮 + 至多 2 次自动重试
    int consecutivePass = 0;
    std::wstring lastFailReason;
    std::vector<GateItem> lastFailItems;  // 降级时报告最后一次失败轮的证据
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        // 每轮重取 NtQSI 快照：参考进程按本轮实况重选。
        std::vector<NtProcRow> rows;
        const bool ntsiOk = NtqsiQueryProcesses(&rows) && !rows.empty();
        std::vector<GateItem> items;
        std::wstring failReason;
        const bool pass = RunGateRound(rows, ntsiOk, &items, &failReason, attempt);
        if (pass) {
            ++consecutivePass;
            rep.items = std::move(items);
        } else {
            consecutivePass = 0;
            lastFailReason = failReason;
            lastFailItems = items;
            if (attempt < kMaxAttempts) {
                STM_LOG_INFO("selfcheck", L"200ms 后自动重试（进入第 {} 轮，重选参考进程）",
                             attempt + 1);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
        if (consecutivePass >= 2) break;  // 连续 2 轮全过 → 放行
    }

    if (consecutivePass >= 2) {
        rep.result = GateResult{false, L""};
        STM_LOG_INFO("selfcheck", L"自检门最终判定：连续 {} 轮全部通过，NtQSI 快速路径放行",
                     consecutivePass);
    } else {
        rep.result = GateResult{true, lastFailReason.empty()
                                          ? std::wstring(L"NtQSI 自校验未达成连续两轮通过，"
                                                         L"已切换 Toolhelp+PSAPI 兼容模式")
                                          : lastFailReason};
        if (!lastFailItems.empty()) rep.items = std::move(lastFailItems);
        STM_LOG_WARN("selfcheck", L"自检门最终判定：{} 轮内未达成连续两轮通过，降级兼容模式（{}）",
                     kMaxAttempts, rep.result.reason);
    }
    return rep;
}

// CollectDetail.h 声明的旧入口：委托到加固版（只取结论，丢弃逐项报告）。
GateResult RunSelfCheckGate() { return RunSelfCheckGateReported().result; }

}  // namespace cd
}  // namespace stm
