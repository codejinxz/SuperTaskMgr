// ui_chart_test：性能页 Phase A 图表纯函数自测（docs/phase/08_chart_design.md
// §6-PhaseA）。PerfChart.h 仅头文件且只依赖 core/Str（无 ImGui / 无 app
// 对象），与 ui3_test.cpp 同形：stm_selftest 内运行（链接 core+collect+ops）。
//
// 覆盖：
//  - chart_window_clamp：时间窗 60/120/300/600 白名单与非法值回落 120；
//  - chart_ring_view：容量 600 环绕索引下的 RingView 正确性（view 起止、
//    环绕拼接、点数上限、零拷贝）——切换时间窗只改显示不动存储，
//    即「切窗不清历史」的纯函数佐证；
//  - chart_readout_text：悬停读数格式（逐序列值 + 时间标签，NaN→"—"）。
#include "selftest/TestFramework.h"
#include "app/ui3/PerfChart.h"
#include <cstdint>
#include <limits>
#include <string>

using stm::ui3::ClampWindowSec;
using stm::ui3::kHistCap;
using stm::ui3::MakeRingView;
using stm::ui3::PerfBlockId;
using stm::ui3::PerfHistory;
using stm::ui3::PerfReadoutText;
using stm::ui3::Ring;
using stm::ui3::RingLogicalAt;
using stm::ui3::RingView;
using stm::ui3::RingViewChunks;
using stm::ui3::WindowSecIndex;

namespace {
constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

// 视图逻辑 i 处的值（经两段连续内存手动拼接，模拟不支持 Offset 的消费方）。
float ChunkedAt(const RingView& view, int i) {
    const float* a = nullptr;
    int na = 0;
    const float* b = nullptr;
    int nb = 0;
    RingViewChunks(view, &a, &na, &b, &nb);
    if (i < 0 || i >= na + nb) return kNan;
    if (i < na) return a[i];
    return b[i - na];
}

// 合法档位与非法样本（含手工乱写的 137、边界 ±1、极大/极小/负数）。
constexpr int64_t kGoodSec[] = {60, 120, 300, 600};
constexpr int64_t kBadSec[] = {0,    1,       59,      61,      119,  121,
                               137,  299,     301,     599,     601,  -1,
                               -60,  -120,    100000,  1000000,
                               INT64_MAX, INT64_MIN};
static_assert(kHistCap == 600, "Phase A: kHistCap 扩容到 600");
}  // namespace

// --- 时间窗白名单：合法档原样、非法一律回落 120 --------------------------
STM_TEST(chart_window_clamp) {
    // 合法档位（cfg perfWindowSec 允许的全部取值）。
    for (const int64_t ok : kGoodSec) {
        if (ClampWindowSec(ok) != ok) {
            *err = stm::Fmt(L"ClampWindowSec({}) 应原样通过", ok);
            return false;
        }
    }
    // 非法值一律回落 120。
    for (const int64_t bad : kBadSec) {
        if (ClampWindowSec(bad) != 120) {
            *err = stm::Fmt(L"ClampWindowSec({}) 应回落 120", bad);
            return false;
        }
    }
    // 秒 -> ComboBox 选择项序号（UI 层 Combo 依赖的纯映射）。
    const int64_t idxCases[][2] = {{60, 0}, {120, 1}, {300, 2}, {600, 3}, {137, 1}};
    for (const auto& c : idxCases) {
        if (WindowSecIndex(c[0]) != static_cast<int>(c[1])) {
            *err = stm::Fmt(L"WindowSecIndex({}) 映射错误", c[0]);
            return false;
        }
    }
    return true;
}

// --- RingView：容量 600 环绕索引、起止、环绕拼接、点数上限、零拷贝 --------
STM_TEST(chart_ring_view) {
    // 1) 满环：推 650 个样本，容量封顶 600，head=50（最老样本物理下标）。
    Ring r;
    for (int i = 0; i < 650; ++i) r.Push(static_cast<float>(i));
    if (r.Count() != 600) {
        *err = stm::Fmt(L"650 次 Push 后 Count={}，应封顶 600", r.Count());
        return false;
    }
    if (r.Offset() != 50) {
        *err = stm::Fmt(L"满环 Offset={}，应为 50", r.Offset());
        return false;
    }
    if (RingLogicalAt(r, 0) != 50.0f || RingLogicalAt(r, 599) != 649.0f) {
        *err = L"满环逻辑首尾应为 50/649（0=最老，599=最新）";
        return false;
    }
    if (RingLogicalAt(r, 600) == RingLogicalAt(r, 600)) {
        *err = L"逻辑下标越界应返回 NaN（诚实缺省）";
        return false;
    }

    // 2) 满窗 600：view 起止 = 全环（count=600、offset=head）。
    const RingView full = MakeRingView(r, 600);
    if (full.count != 600 || full.offset != r.Offset() || full.data != r.v.data()) {
        *err = L"600s 满窗视图应覆盖整环且零拷贝";
        return false;
    }

    // 3) 尾窗 60：起点=逻辑 540（值 590）、终点=最新（值 649）。
    const RingView w60 = MakeRingView(r, 60);
    if (w60.count != 60) {
        *err = stm::Fmt(L"60s 窗 count={}，应为 60", w60.count);
        return false;
    }
    if (w60.offset != (r.Offset() + 540) % 600 || w60.offset != 590) {
        *err = stm::Fmt(L"60s 窗 offset={}，应为 590", w60.offset);
        return false;
    }
    if (w60.data[w60.offset] != 590.0f ||
        w60.data[(w60.offset + 59) % 600] != 649.0f) {
        *err = L"尾窗起止样本错误（590 -> 649）";
        return false;
    }

    // 4) 环绕拼接：若干窗宽下两段连续内存拼接 == 逻辑序列（逐点核对）。
    for (const int win : {1, 37, 60, 300, 599, 600}) {
        const RingView view = MakeRingView(r, win);
        const float* a = nullptr;
        int na = 0;
        const float* b = nullptr;
        int nb = 0;
        RingViewChunks(view, &a, &na, &b, &nb);
        if (na + nb != view.count || na < 0 || nb < 0) {
            *err = stm::Fmt(L"{}s 窗两段长度之和 != count", win);
            return false;
        }
        const int start = 600 - view.count;
        for (int i = 0; i < view.count; ++i) {
            const float want = RingLogicalAt(r, start + i);
            if (ChunkedAt(view, i) != want) {
                *err = stm::Fmt(L"{}s 窗拼接序列在 i={} 处不符", win, i);
                return false;
            }
        }
        // ImPlot spec.Offset 路径（零拷贝语义）：data 指针恒等于存储首址。
        if (view.data != r.v.data()) {
            *err = stm::Fmt(L"{}s 窗视图不是零拷贝", win);
            return false;
        }
    }

    // 5) 不满窗 / 点数上限：10 个样本、window 600 -> count 10（min 语义），
    //    window 5 -> count 5 尾窗（值 5..9）；window<=0 / 空环 -> 空视图。
    Ring small;
    for (int i = 0; i < 10; ++i) small.Push(static_cast<float>(i));
    const RingView sFull = MakeRingView(small, 600);
    if (sFull.count != 10 || sFull.offset != 0) {
        *err = L"不满环整窗应 count=10、offset=0";
        return false;
    }
    const RingView sTail = MakeRingView(small, 5);
    if (sTail.count != 5 || sTail.offset != 5) {
        *err = stm::Fmt(L"10 样本取 5 窗应 count=5、offset=5（实为 {}/{}）",
                        sTail.count, sTail.offset);
        return false;
    }
    if (sTail.data[sTail.offset] != 5.0f) {
        *err = L"未满环尾窗起点应是值 5";
        return false;
    }
    if (MakeRingView(small, 0).count != 0 || MakeRingView(small, -1).count != 0) {
        *err = L"window<=0 应返回空视图";
        return false;
    }
    Ring empty;
    if (MakeRingView(empty, 600).count != 0) {
        *err = L"空环应返回空视图";
        return false;
    }

    // 6) 切窗不清历史佐证：同一环在不同窗宽下共享同一存储（只改显示），
    //    且环本身 Count/Offset 不受取视图影响。
    if (MakeRingView(r, 60).data != MakeRingView(r, 600).data ||
        r.Count() != 600 || r.Offset() != 50) {
        *err = L"切换时间窗不得移动/清空历史存储";
        return false;
    }
    return true;
}

// --- 悬停读数：逐序列值 + 时间标签，NaN→"—"，越界诚实空串 ----------------
STM_TEST(chart_readout_text) {
    PerfHistory h;
    h.cpuTotal.Push(42.5f);
    h.physAvail.Push(1024.0f * 1024.0f);
    h.commit.Push(kNan);
    h.diskRead.Push(kNan);
    h.diskWrite.Push(kNan);
    h.netRecv.Push(kNan);
    h.netSend.Push(kNan);
    h.hardFaults.Push(1234.0f);
    h.ctxSwitch.Push(kNan);
    h.cores.assign(2, Ring{});
    h.cores[0].Push(10.0f);
    h.cores[1].Push(kNan);

    // CPU：总量 42.5%、核心0 10.0%、核心1 NaN→"—"；时间标签 0 秒前。
    const std::wstring cpu0 = PerfReadoutText(h, PerfBlockId::kPerfBlockCpu, 0);
    if (cpu0.find(L"0 秒前") == std::wstring::npos ||
        cpu0.find(L"总量 42.5%") == std::wstring::npos ||
        cpu0.find(L"核心0 10.0%") == std::wstring::npos ||
        cpu0.find(L"核心1 —") == std::wstring::npos) {
        *err = L"CPU 读数格式错误：" + cpu0;
        return false;
    }

    // 内存：提交 NaN→"—"；可用物理 1 MiB。
    const std::wstring mem0 = PerfReadoutText(h, PerfBlockId::kPerfBlockMem, 0);
    if (mem0.find(L"可用物理 1.00 MiB") == std::wstring::npos ||
        mem0.find(L"提交 —") == std::wstring::npos) {
        *err = L"内存读数格式错误：" + mem0;
        return false;
    }

    // 网络：第二拍收到 2048 B/s（最新样本），读 idx=1 -> 「0 秒前」
    // 「接收 2.00 KiB」（时间标签相对最新样本：idx=Count-1 即 0 秒前）。
    h.cpuTotal.Push(50.0f);
    h.physAvail.Push(kNan);
    h.commit.Push(kNan);
    h.diskRead.Push(kNan);
    h.diskWrite.Push(kNan);
    h.netRecv.Push(2048.0f);
    h.netSend.Push(kNan);
    h.hardFaults.Push(kNan);
    h.ctxSwitch.Push(kNan);
    h.cores[0].Push(kNan);
    h.cores[1].Push(kNan);
    const std::wstring net1 = PerfReadoutText(h, PerfBlockId::kPerfBlockNet, 1);
    if (net1.find(L"0 秒前") == std::wstring::npos ||
        net1.find(L"接收 2.00 KiB") == std::wstring::npos ||
        net1.find(L"发送 —") == std::wstring::npos) {
        *err = L"网络读数格式错误：" + net1;
        return false;
    }
    const std::wstring net0 = PerfReadoutText(h, PerfBlockId::kPerfBlockNet, 0);
    if (net0.find(L"1 秒前") == std::wstring::npos ||
        net0.find(L"接收 —") == std::wstring::npos) {
        *err = L"网络历史拍读数格式错误：" + net0;
        return false;
    }

    // 计数类：千分位（1234 -> 1,234）。
    const std::wstring hf0 = PerfReadoutText(h, PerfBlockId::kPerfBlockHardFaults, 0);
    if (hf0.find(L"硬故障 1,234") == std::wstring::npos) {
        *err = L"硬故障读数格式错误：" + hf0;
        return false;
    }

    // 越界与空态：非法块 id / GPU 块 / idx 越界 / 空历史 -> 诚实空串。
    if (!PerfReadoutText(h, -1, 0).empty() ||
        !PerfReadoutText(h, PerfBlockId::kPerfBlockCount, 0).empty() ||
        !PerfReadoutText(h, PerfBlockId::kPerfBlockGpu, 0).empty() ||
        !PerfReadoutText(h, PerfBlockId::kPerfBlockCpu, 2).empty() ||
        !PerfReadoutText(h, PerfBlockId::kPerfBlockCpu, -1).empty()) {
        *err = L"越界读数应返回空串";
        return false;
    }
    PerfHistory e;
    if (!PerfReadoutText(e, PerfBlockId::kPerfBlockCpu, 0).empty()) {
        *err = L"空历史读数应返回空串";
        return false;
    }
    return true;
}

// V21-P0 回归：ImPlot 的 spec.Offset 以"绘制点数"取模，而环形 offset 以容量取模
// —— 直传会整窗画错段。CopyRingTail 线性化后必须与逻辑序列逐点一致。
STM_TEST(chart_plot_ring_linearized) {
    using stm::ui3::Ring;
    Ring r;
    for (int i = 0; i < 620; ++i) r.Push(static_cast<float>(i));  // 满 -> head=20
    std::vector<float> out(kHistCap, 0.0f);
    const int n = stm::ui3::CopyRingTail(r, 60, out.data(), static_cast<int>(out.size()));
    if (n != 60) {
        *err = L"尾窗点数应为 60，实际 " + std::to_wstring(n);
        return false;
    }
    // 满环 head=20、窗口 60：逻辑首下标 = 620-60 = 560，物理 = (20+560)%600 = 580，
    // 需要环绕拼接 [580..599] + [0..39]。逐点校验 == 逻辑值 560..619。
    for (int i = 0; i < 60; ++i) {
        if (out[static_cast<size_t>(i)] != static_cast<float>(560 + i)) {
            *err = L"线性化序列错位：out[" + std::to_wstring(i) + L"] = " +
                   std::to_wstring(static_cast<int>(out[static_cast<size_t>(i)]));
            return false;
        }
    }
    // 窗口大于已有点数：返回全部样本。
    const int n2 = stm::ui3::CopyRingTail(r, 600, out.data(), static_cast<int>(out.size()));
    if (n2 != 600 || out[0] != 20.0f || out[599] != 619.0f) {
        *err = L"全量尾窗不正确";
        return false;
    }
    return true;
}
