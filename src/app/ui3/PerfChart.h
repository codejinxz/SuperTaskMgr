#pragma once
// 性能页图表纯函数（Phase A/B/C，docs/phase/08_chart_design.md §2/§3/§6）。
// header-only，仅依赖 core/Str（无 ImGui / 无 app 对象），stm_selftest
// 仅链 core+collect+ops 即可覆盖（与 ui3_test.cpp 同形）。
//
// 职责：
//  - Ring / PerfHistory / kHistCap：性能历史环形缓冲（自 app/ui/Pages.cpp
//    迁入，Pages.cpp 与 selftest 共用同一份定义）。kHistCap 120 -> 600，
//    为 60/120/300/600 秒时间窗提供历史容量。
//  - ClampWindowSec / WindowSecIndex / kWindowSecChoices：时间窗白名单
//    （60/120/300/600，非法回落 120；cfg 键 perfWindowSec 由 UI 层持久化）。
//  - MakeRingView / RingViewChunks：尾窗「零拷贝视图」（窗口只影响显示、
//    不改存储：切换时间窗不清空历史）。
//  - RingLogicalAt / PerfReadoutText：逻辑下标取值（越界 -> NaN，诚实缺省）
//    与悬停读数文案（NaN -> "—"；放大视图直接复用）。
//  - Phase B：FollowTick 跟随/检视状态机（纯函数）、ClampZoomBlock（cfg
//    perfZoom 合法化）、ReadoutIndexAt（鼠标 x -> 逻辑下标）。
//  - Phase C：CommitPercent（提交占比，limit<=0 -> NaN）、CoreHeatmapValues
//    （每核热图行主序展平）、GpuUtilClampSum（多卡利用率合计钳 100）。
//  - Phase D：BuildAdapterSeries（每适配器序列构建：排除回环/非 Up、
//    流量 Top8 截断，纯函数可单测）。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <cstring>
#include <string>
#include <vector>

#include "core/Str.h"

namespace stm {
namespace ui3 {

// 历史容量（tick 数，采集 1Hz 时即秒）。Phase A：120 -> 600。
inline constexpr int kHistCap = 600;

// 时间窗白名单（秒）。索引即 ComboBox 选择项序号。
inline constexpr int kWindowSecChoices[] = {60, 120, 300, 600};
inline constexpr int kWindowSecDefault = 120;

// 非法值一律回落默认 120（cfg 手写 perfWindowSec=137 等不崩、不越界）。
inline int ClampWindowSec(int64_t v) {
    for (const int choice : kWindowSecChoices) {
        if (v == static_cast<int64_t>(choice)) return choice;
    }
    return kWindowSecDefault;
}

// 秒 -> ComboBox 选择项序号（0..3；非法回落默认档）。
inline int WindowSecIndex(int64_t sec) {
    const int clamped = ClampWindowSec(sec);
    for (int i = 0; i < 4; ++i) {
        if (kWindowSecChoices[i] == clamped) return i;
    }
    return 1;  // 不可达（ClampWindowSec 保证合法），防御回落 120 档
}

// 单序列环形缓冲：至多 kHistCap 个 float 样本，头自环。NaN 样本照推
// （渲染/读数时诚实跳过），但 hasData 只记「至少一个有效样本」。
struct Ring {
    std::vector<float> v;
    int head = 0;
    // 至少收到过一个有效（非 NaN）样本 —— 不可用计数器（如硬故障/s
    // 在部分机器上 kUnavail）据此渲染诚实空态，而不是一条看不见的空线。
    bool hasData = false;

    void Push(float x) {
        if (x == x) hasData = true;
        if (static_cast<int>(v.size()) < kHistCap) {
            v.push_back(x);
            return;
        }
        v[static_cast<size_t>(head)] = x;
        head = (head + 1) % kHistCap;
    }
    int Count() const { return static_cast<int>(v.size()); }
    // 数据起点在存储中的物理下标：未满时即 0（push_back 顺序 = 逻辑顺序），
    // 满后 head 指向最老样本。
    int Offset() const { return Count() < kHistCap ? 0 : head; }
};

// 逻辑下标（0=最老，Count()-1=最新）取值；越界返回 NaN（诚实缺省）。
inline float RingLogicalAt(const Ring& r, int logicalIdx) {
    if (logicalIdx < 0 || logicalIdx >= r.Count()) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    const int physical = (r.Offset() + logicalIdx) % kHistCap;
    return r.v[static_cast<size_t>(physical)];
}

// 8 块指标（与 PerfPage 网格块顺序一致）；GPU 块无历史序列。
enum PerfBlockId {
    kPerfBlockCpu = 0,
    kPerfBlockPerCore = 1,
    kPerfBlockMem = 2,
    kPerfBlockDisk = 3,
    kPerfBlockNet = 4,
    kPerfBlockHardFaults = 5,
    kPerfBlockCtxSwitch = 6,
    kPerfBlockGpu = 7,
    kPerfBlockCount = 8,
};

struct PerfHistory {
    uint64_t lastTick = 0;
    Ring cpuTotal, physAvail, commit, diskRead, diskWrite, netRecv, netSend;
    // 系统级硬故障/s 与上下文切换/s（聚合口径见 Pages.cpp AppendHistory）。
    Ring hardFaults, ctxSwitch;
    std::vector<Ring> cores;
    // Phase C：磁盘队列深度（PDH，本机无计数器 -> 全 NaN -> hasData=false
    // 诚实空态）、提交占比%（commit/commitLimit 派生，内存块 Y2）、
    // GPU 利用率合计%（sys.gpus 非空的 tick 才推，2s 节拍缺席 tick 由
    // 采集端复用上次值 -> 阶梯无锯齿）。
    Ring diskQueue, commitPct, gpuUtil;
    // Phase D：每适配器吞吐序列（收+发合计 B/s，一条线一个适配器）。
    // 与 cores 同口径：接口集变化 -> 重建、历史重启。ids 与 rings/names
    // 一一对应（SetCompare 用）；total = 重建时 Up 非回环适配器总数
    //（> rings 说明被 Top8 截断，UI 据此注明）。
    std::vector<Ring> netAdapters;
    std::vector<uint64_t> netAdapterIds;
    std::vector<std::wstring> netAdapterNames;
    int netAdapterTotal = 0;
};

// ---- 尾窗零拷贝视图 -------------------------------------------------------
// view = 环存储上「最新 count 个样本」的跨度：起点 offset、点数 count。
// 逻辑 i（0=窗内最老）对应存储 (offset + i) % kHistCap，即 ImPlot
// values-only + spec.Offset 的语义；环绕段用 RingViewChunks 拆开。
struct RingView {
    const float* data = nullptr;  // 环存储首地址（r.v.data()，零拷贝）
    int count = 0;                // min(Count(), window)；window<=0 或空环 -> 0
    int offset = 0;               // 窗内最老样本的物理下标
};

inline RingView MakeRingView(const Ring& r, int window) {
    RingView view;
    view.data = r.v.data();
    const int n = r.Count();
    if (n <= 0 || window <= 0) return view;  // 空视图（count=0）
    view.count = std::min(n, window);
    const int start = n - view.count;  // 窗内最老样本的逻辑下标
    view.offset = (r.Offset() + start) % kHistCap;
    return view;
}

// 把视图拆成至多两段连续内存（a 后接 b，环绕拼接）；供不认识 spec.Offset
// 的消费方逐块取数。na+nb == count 恒成立；不环绕时 b 为空。
inline void RingViewChunks(const RingView& view, const float** a, int* na,
                           const float** b, int* nb) {
    *b = nullptr;
    *nb = 0;
    *na = 0;
    if (view.data == nullptr || view.count <= 0) {
        *a = nullptr;
        return;
    }
    *a = view.data + view.offset;  // 窗内最老样本的物理位置
    const int room = kHistCap - view.offset;  // 起点 -> 存储末尾的连续点数
    if (view.count <= room) {
        *na = view.count;
        return;
    }
    *na = room;
    *b = view.data;
    *nb = view.count - room;
}

// V21-P0：把尾窗"线性化"拷贝到 out（至多 kHistCap 个 float）。
// ImPlot 的 spec.Offset 以"绘制点数"取模，而 MakeRingView 的 offset 以环容量
// kHistCap 取模 —— 直传会画错段（稳态下整窗错位）。线性化后以 Offset=0 绘制，
// data[i] 即逻辑序列。返回点数（0=无数据；outCap 不足时返回 0 防御）。
inline int CopyRingTail(const Ring& r, int window, float* out, int outCap) {
    const RingView view = MakeRingView(r, window);
    if (view.count <= 0 || view.data == nullptr || outCap < view.count) return 0;
    const float* a = nullptr;
    const float* b = nullptr;
    int na = 0, nb = 0;
    RingViewChunks(view, &a, &na, &b, &nb);
    if (na > 0) std::memcpy(out, a, sizeof(float) * static_cast<size_t>(na));
    if (nb > 0) std::memcpy(out + na, b, sizeof(float) * static_cast<size_t>(nb));
    return view.count;
}

// ---- Phase B：放大视图跟随/检视状态机（docs §2.3）-------------------------
// 跟随态默认开；用户平移/框选 -> 检视态（false）；「回到最新」-> 跟随态
// （true）。纯函数：UI 层每帧经 GetPlotLimits 差值判 userMoved 后调用落态。
inline bool FollowTick(bool follow, bool userMoved, bool resumeReq) {
    if (resumeReq) return true;   // 回到最新：无条件恢复自动跟随
    if (userMoved) return false;  // 用户平移/框选：暂停跟随进入检视
    return follow;                // 无事件：保持原态
}

// cfg perfZoom 合法化：-1 = 无放大；0..kPerfBlockCount-1 = 放大块 id；
// 其余（手写配置越界）一律回落 -1。
inline int ClampZoomBlock(int64_t v) {
    if (v < 0 || v >= kPerfBlockCount) return -1;
    return static_cast<int>(v);
}

// 放大态悬停读数：鼠标 x（秒，xscale 换算后的 ImPlot 坐标）-> 逻辑下标
//（0=最老，Count()-1=最新）。吸附到 1Hz 采样栅格（round）；落在窗外/越界
// 返回 -1（UI 据此不弹 tooltip）。count/window<=0 或 tickSec<=0 同样 -1。
inline int ReadoutIndexAt(double xSec, int count, int window, double tickSec) {
    if (count <= 0 || window <= 0 || tickSec <= 0.0) return -1;
    if (xSec != xSec) return -1;  // NaN（鼠标不在绘图区内等）
    const int shown = count < window ? count : window;
    const long long local = std::llround(xSec / tickSec);
    if (local < 0 || local >= shown) return -1;
    return (count - shown) + static_cast<int>(local);
}

// ---- Phase C：新序列派生（纯函数）------------------------------------------
// 提交占比%（内存块 Y2）。limit<=0（异常/未取到）-> NaN 诚实跳过，
// 绝不伪造 0% 或除零。
inline double CommitPercent(uint64_t commit, uint64_t limit) {
    if (limit == 0) return std::numeric_limits<double>::quiet_NaN();
    return 100.0 * static_cast<double>(commit) / static_cast<double>(limit);
}

// 多适配器 GPU 利用率合计：只累加有效读数，钳到 100（与按进程聚合同口径）。
// 全部不可得 -> NaN（诚实：不是 0%）。
inline double GpuUtilClampSum(const double* utils, size_t n) {
    double sum = 0.0;
    bool any = false;
    for (size_t i = 0; i < n; ++i) {
        if (utils[i] == utils[i]) {
            sum += utils[i];
            any = true;
        }
    }
    if (!any) return std::numeric_limits<double>::quiet_NaN();
    return std::min(sum, 100.0);
}

// 每核热图数据：行=核心、列=时间窗尾窗（cols = min(kHeatmapMaxCols, window)，
// 行主序展平 values[core * cols + t]）。样本不足的格位 NaN（渲染为空白而非 0）。
struct CoreHeatmap {
    std::vector<float> values;
    int rows = 0;
    int cols = 0;
};
inline constexpr int kHeatmapMaxCols = 120;  // 设计 §3.5：核数 x min(120, window)

inline CoreHeatmap CoreHeatmapValues(const std::vector<Ring>& cores, int window) {
    CoreHeatmap hm;
    hm.rows = static_cast<int>(cores.size());
    if (hm.rows <= 0 || window <= 0) return hm;
    hm.cols = std::min(kHeatmapMaxCols, window);
    hm.values.assign(static_cast<size_t>(hm.rows) * static_cast<size_t>(hm.cols),
                     std::numeric_limits<float>::quiet_NaN());
    for (int r = 0; r < hm.rows; ++r) {
        const Ring& ring = cores[static_cast<size_t>(r)];
        // 尾窗：逻辑下标 start..Count()-1 对齐到列 0..cols-1；不足补 NaN。
        const int start = ring.Count() - hm.cols;
        for (int c = 0; c < hm.cols; ++c) {
            const float v = RingLogicalAt(ring, start + c);
            hm.values[static_cast<size_t>(r) * static_cast<size_t>(hm.cols) +
                      static_cast<size_t>(c)] = v;
        }
    }
    return hm;
}

// ---- Phase D：每适配器序列构建（纯函数，chart_adapter_series 单测）--------
inline constexpr size_t kMaxAdapterSeries = 8;  // >8 条取流量 Top8（UI 注明）

// 与 SystemInfo::AdapterThroughput 一一对应的构建输入 + 渲染侧过滤位。
// 采集契约（SystemCollector）只产出 Up 且非回环的条目，AppendHistory 恒填
// up=true/loopback=false；两个标志位保留给防御过滤与单测构造。
struct AdapterSeriesIn {
    uint64_t ifIndex = 0;
    std::wstring name;
    double recvBps = 0.0;
    double sendBps = 0.0;
    bool up = true;
    bool loopback = false;
    // 单线序列值 = 收+发合计；任一方 NaN 取另一方，双方 NaN -> NaN（诚实断线）。
    double TotalBps() const {
        const bool rn = recvBps != recvBps, sn = sendBps != sendBps;
        if (rn && sn) return std::numeric_limits<double>::quiet_NaN();
        if (rn) return sendBps;
        if (sn) return recvBps;
        return recvBps + sendBps;
    }
};

// 序列构建：排除回环与非 Up（防御过滤）后按收发合计流量降序稳定排序，
// 截断到 kMaxAdapterSeries。返回的新顺序即序列/图例顺序；调用方以返回值
// 重建 rings/names/ids（接口集变化 -> 历史重启，与 cores 同口径）。
inline std::vector<AdapterSeriesIn> BuildAdapterSeries(const std::vector<AdapterSeriesIn>& in) {
    std::vector<AdapterSeriesIn> out;
    out.reserve(in.size());
    for (const AdapterSeriesIn& a : in) {
        if (a.loopback || !a.up) continue;
        out.push_back(a);
    }
    std::stable_sort(out.begin(), out.end(), [](const AdapterSeriesIn& x, const AdapterSeriesIn& y) {
        const double tx = x.TotalBps(), ty = y.TotalBps();
        const bool xn = tx != tx, yn = ty != ty;
        if (xn != yn) return yn;          // NaN（无有效读数）排末尾
        if (xn && yn) return false;       // 双 NaN 保持原序
        return tx > ty;
    });
    if (out.size() > kMaxAdapterSeries) out.resize(kMaxAdapterSeries);
    return out;
}

// ---- 悬停读数（放大视图 Phase B 吸附 tooltip 用；此处先落纯函数与用例）----
// 数值列：NaN -> "—"（FormatPercent/FormatNumber 自带；字节类在此显式守卫，
// 因为 FormatBytes 只收整数）。计数类用 FormatNumber 千分位。
namespace perfreadout {

inline std::wstring PercentAt(const Ring& r, int idx) {
    return FormatPercent(static_cast<double>(RingLogicalAt(r, idx)));
}

inline std::wstring BytesAt(const Ring& r, int idx) {
    const float v = RingLogicalAt(r, idx);
    if (v != v) return L"—";  // NaN
    return FormatBytes(v <= 0.0f ? 0u : static_cast<uint64_t>(static_cast<double>(v)));
}

inline std::wstring CountAt(const Ring& r, int idx) {
    return FormatNumber(static_cast<double>(RingLogicalAt(r, idx)));
}

// 队列深度类：保留 1 位小数（Current Disk Queue Length 为浮点均值口径），
// NaN -> "—"。
inline std::wstring QueueAt(const Ring& r, int idx) {
    const float v = RingLogicalAt(r, idx);
    if (v != v) return L"—";
    return Fmt(L"{:.1f}", static_cast<double>(v));
}

}  // namespace perfreadout

// 悬停读数：块 blockId 在逻辑下标 idx 处的逐序列值文案。
// 首行为时间标签（「N 秒前」，= (Count()-1-idx) × tickSec），随后每序列一行
// 「名称 值」；NaN 序列显示 "—"。blockId 越界或 idx 越界返回空串。
// tickSec：当前采样间隔（V27-P1-1：x 轴已是真实秒，时间标签必须同步换算）。
inline std::wstring PerfReadoutText(const PerfHistory& h, int blockId, int idx,
                                    double tickSec = 1.0) {
    if (blockId < 0 || blockId >= kPerfBlockCount || blockId == kPerfBlockGpu) {
        return std::wstring();
    }
    const int count = h.cpuTotal.Count();  // 所有系统级 Ring 同步 Push，长度一致
    if (idx < 0 || idx >= count) return std::wstring();

    const double secondsAgo =
        static_cast<double>(count - 1 - idx) * (tickSec > 0.0 ? tickSec : 1.0);
    std::wstring text = secondsAgo < 1.0 ? std::wstring(L"刚刚")
                                         : Fmt(L"{:.0f} 秒前", secondsAgo);
    auto line = [&text](const wchar_t* name, const std::wstring& value) {
        text += L"\n";
        text += name;
        text += L" ";
        text += value;
    };
    switch (blockId) {
    case kPerfBlockCpu:
        line(L"总量", perfreadout::PercentAt(h.cpuTotal, idx));
        for (size_t i = 0; i < h.cores.size(); ++i) {
            line(Fmt(L"核心{}", i).c_str(),
                 perfreadout::PercentAt(h.cores[i], idx));
        }
        break;
    case kPerfBlockPerCore:
        for (size_t i = 0; i < h.cores.size(); ++i) {
            line(Fmt(L"核心{}", i).c_str(),
                 perfreadout::PercentAt(h.cores[i], idx));
        }
        break;
    case kPerfBlockMem:
        line(L"可用物理", perfreadout::BytesAt(h.physAvail, idx));
        line(L"提交", perfreadout::BytesAt(h.commit, idx));
        line(L"提交占比", perfreadout::PercentAt(h.commitPct, idx));
        break;
    case kPerfBlockDisk:
        line(L"读取", perfreadout::BytesAt(h.diskRead, idx));
        line(L"写入", perfreadout::BytesAt(h.diskWrite, idx));
        line(L"队列", perfreadout::QueueAt(h.diskQueue, idx));
        break;
    case kPerfBlockNet:
        line(L"接收", perfreadout::BytesAt(h.netRecv, idx));
        line(L"发送", perfreadout::BytesAt(h.netSend, idx));
        break;
    case kPerfBlockHardFaults:
        line(L"硬故障", perfreadout::CountAt(h.hardFaults, idx));
        break;
    case kPerfBlockCtxSwitch:
        line(L"切换", perfreadout::CountAt(h.ctxSwitch, idx));
        break;
    default:
        return std::wstring();
    }
    return text;
}

}  // namespace ui3
}  // namespace stm
