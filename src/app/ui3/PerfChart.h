#pragma once
// 性能页图表纯函数（Phase A，docs/phase/08_chart_design.md §2.4/§2.5/§6-PhaseA）。
// header-only，仅依赖 core/Str（无 ImGui / 无 app 对象），stm_selftest
// 仅链 core+collect+ops 即可覆盖（与 ui3_test.cpp 同形）。
//
// 职责：
//  - Ring / PerfHistory / kHistCap：性能历史环形缓冲（自 app/ui/Pages.cpp
//    迁入，Pages.cpp 与 selftest 共用同一份定义）。kHistCap 120 -> 600，
//    为 60/120/300/600 秒时间窗提供历史容量；600 档内存代价
//    ~26 环 x 600 x 4B ≈ 62KB，可忽略。
//  - ClampWindowSec / WindowSecIndex / kWindowSecChoices：时间窗白名单
//    （60/120/300/600，非法回落 120；cfg 键 perfWindowSec 由 UI 层持久化）。
//  - MakeRingView / RingViewChunks：尾窗「零拷贝视图」——摄取仍每 tick 一次
//    O(procs) 写 Ring，渲染侧只取尾窗跨度（ImPlot 经 spec.Offset 逐块消费；
//    不支持 Offset 的消费方走 RingViewChunks 的至多两段连续内存）。
//    窗口只影响显示不改存储：切换时间窗不清空历史。
//  - RingLogicalAt / PerfReadoutText：逻辑下标取值（越界 -> NaN，诚实缺省）
//    与悬停读数文案（NaN -> "—"；放大视图 Phase B 直接复用）。
#include <algorithm>
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

}  // namespace perfreadout

// 悬停读数：块 blockId 在逻辑下标 idx 处的逐序列值文案。
// 首行为时间标签（「N 秒前」，N = Count()-1-idx），随后每序列一行
// 「名称 值」；NaN 序列显示 "—"。blockId 越界或 idx 越界返回空串。
inline std::wstring PerfReadoutText(const PerfHistory& h, int blockId, int idx) {
    if (blockId < 0 || blockId >= kPerfBlockCount || blockId == kPerfBlockGpu) {
        return std::wstring();
    }
    const int count = h.cpuTotal.Count();  // 所有系统级 Ring 同步 Push，长度一致
    if (idx < 0 || idx >= count) return std::wstring();

    std::wstring text =
        Fmt(L"{} 秒前", count - 1 - idx);
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
        break;
    case kPerfBlockDisk:
        line(L"读取", perfreadout::BytesAt(h.diskRead, idx));
        line(L"写入", perfreadout::BytesAt(h.diskWrite, idx));
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
