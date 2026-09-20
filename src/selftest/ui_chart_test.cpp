// ui_chart_test：性能页图表纯函数自测（docs/phase/08_chart_design.md
// §2/§3/§6-PhaseA/B/C/D）。PerfChart.h 仅头文件且只依赖 core/Str（无 ImGui /
// 无 app 对象），与 ui3_test.cpp 同形：stm_selftest 内运行（链接
// core+collect+ops）。
//
// 覆盖：
//  - chart_window_clamp：时间窗 60/120/300/600 白名单与非法值回落 120；
//  - chart_ring_view：容量 600 环绕索引下的 RingView 正确性（view 起止、
//    环绕拼接、点数上限、零拷贝）——切换时间窗只改显示不动存储，
//    即「切窗不清历史」的纯函数佐证；
//  - chart_readout_text：悬停读数格式（逐序列值 + 时间标签，NaN→"—"）；
//  - chart_plot_ring_linearized：CopyRingTail 线性化与逻辑序列逐点一致；
//  - follow_tick_state：Phase B 跟随/检视状态机 + perfZoom 合法化；
//  - chart_diskqueue_readout：Phase C 磁盘队列深度读数（合法/NaN→"—"）
//    与悬停读数链路（鼠标 x -> 逻辑下标）；
//  - chart_adapter_series：Phase D 每适配器序列构建（Top8/排除回环）。
#include "selftest/TestFramework.h"
#include "app/ui3/PerfChart.h"
#include <cstdint>
#include <limits>
#include <string>

using stm::ui3::BuildAdapterSeries;
using stm::ui3::ClampWindowSec;
using stm::ui3::ClampZoomBlock;
using stm::ui3::CommitPercent;
using stm::ui3::CoreHeatmapValues;
using stm::ui3::FollowTick;
using stm::ui3::GpuUtilClampSum;
using stm::ui3::kHistCap;
using stm::ui3::kMaxAdapterSeries;
using stm::ui3::MakeRingView;
using stm::ui3::PerfBlockId;
using stm::ui3::PerfHistory;
using stm::ui3::PerfReadoutText;
using stm::ui3::ReadoutIndexAt;
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

    // CPU：总量 42.5%、核心0 10.0%、核心1 NaN→"—"；时间标签 0 秒间隔（tickSec=1）→「刚刚」。
    const std::wstring cpu0 = PerfReadoutText(h, PerfBlockId::kPerfBlockCpu, 0);
    if (cpu0.find(L"刚刚") == std::wstring::npos ||
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

    // 网络：第二拍收到 2048 B/s（最新样本），读 idx=1（最新样本）→「刚刚」
    // 「接收 2.00 KiB」（时间标签相对最新样本：最新样本显示「刚刚」）。
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
    if (net1.find(L"刚刚") == std::wstring::npos ||
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

// --- Phase B：跟随/检视状态机 + perfZoom 合法化 ---------------------------
STM_TEST(follow_tick_state) {
    // 三态迁移表：{当前态, 用户动作, 回到最新请求, 期望下一态}。
    // 跟随态默认保持；用户平移/框选 -> 检视；回到最新 -> 跟随（最高优先）。
    const bool cases[][4] = {
        {true,  false, false, true},   // 跟随 + 无事件 -> 保持跟随
        {true,  true,  false, false},  // 跟随 + 用户平移/框选 -> 检视
        {false, false, false, false},  // 检视 + 无事件 -> 保持检视
        {false, false, true,  true},   // 检视 + 回到最新 -> 跟随
        {true,  false, true,  true},   // 跟随 + 回到最新 -> 跟随
        {false, true,  true,  true},   // 回到最新优先于用户动作
        {false, true,  false, false},  // 检视态的用户动作仍保持检视
    };
    for (const auto& c : cases) {
        const bool got = FollowTick(c[0], c[1], c[2]);
        if (got != c[3]) {
            *err = stm::Fmt(L"FollowTick(follow={}, userMoved={}, resume={}) 应为 {}，实际 {}",
                            c[0], c[1], c[2], c[3], got);
            return false;
        }
    }
    // cfg perfZoom 合法化：-1 = 无放大；0..7 = 块 id；越界一律回落 -1。
    if (ClampZoomBlock(-1) != -1 || ClampZoomBlock(0) != 0 || ClampZoomBlock(7) != 7) {
        *err = L"ClampZoomBlock 合法值应原样通过";
        return false;
    }
    const int64_t kBadZoom[] = {-2, 8, 100, INT64_MAX, INT64_MIN};
    for (const int64_t bad : kBadZoom) {
        if (ClampZoomBlock(bad) != -1) {
            *err = stm::Fmt(L"ClampZoomBlock({}) 应回落 -1", bad);
            return false;
        }
    }
    return true;
}

// --- Phase B/C：磁盘队列深度读数 + 悬停读数链路（鼠标 x -> 下标 -> 文案）--
STM_TEST(chart_diskqueue_readout) {
    constexpr double kUn = std::numeric_limits<double>::quiet_NaN();
    PerfHistory h;
    // 系统级 ring 三拍（读数用 Count 对齐）；diskQueue：合法 -> 合法 -> NaN。
    for (int i = 0; i < 3; ++i) {
        h.cpuTotal.Push(10.0f * i);
        h.diskRead.Push(kNan);
        h.diskWrite.Push(kNan);
        h.commitPct.Push(static_cast<float>(CommitPercent(30, 100)));
        h.diskQueue.Push(i == 2 ? kNan : 4.5f + static_cast<float>(i));
    }
    // 合法读数：1 位小数（0.1f + 4.5f 浮点和应精确落在 4.6）。
    const std::wstring d0 = PerfReadoutText(h, PerfBlockId::kPerfBlockDisk, 0);
    if (d0.find(L"队列 4.5") == std::wstring::npos) {
        *err = L"磁盘队列合法读数错误：" + d0;
        return false;
    }
    // NaN（本机无计数器）-> "—"，绝不冒充 0。
    const std::wstring d2 = PerfReadoutText(h, PerfBlockId::kPerfBlockDisk, 2);
    if (d2.find(L"队列 —") == std::wstring::npos) {
        *err = L"磁盘队列 NaN 应显示 —：" + d2;
        return false;
    }
    // 提交占比副轴读数（30/100 -> 30%）与读取/写入 NaN 同框。
    if (d0.find(L"读取 —") == std::wstring::npos) {
        *err = L"磁盘读数缺读取列：" + d0;
        return false;
    }
    const std::wstring m0 = PerfReadoutText(h, PerfBlockId::kPerfBlockMem, 0);
    if (m0.find(L"提交占比 30.0%") == std::wstring::npos) {
        *err = L"提交占比读数错误：" + m0;
        return false;
    }
    // CommitPercent：limit<=0 -> NaN（诚实缺省），绝不除零/伪造 0%。
    if (CommitPercent(30, 0) == CommitPercent(30, 0)) {
        *err = L"commitLimit=0 应返回 NaN";
        return false;
    }
    if (CommitPercent(0, 0) == CommitPercent(0, 0)) {
        *err = L"commit=limit=0 应返回 NaN";
        return false;
    }
    if (CommitPercent(5, 200) != 2.5) {
        *err = L"CommitPercent(5,200) 应为 2.5";
        return false;
    }

    // 悬停读数链路：count=3、window=60、tickSec=1 —— 鼠标 x=0/1/2 -> 逻辑
    // 下标 0/1/2；x<0、x>=窗、非秒对齐取整吸附、NaN -> -1（不弹 tooltip）。
    if (ReadoutIndexAt(0.0, 3, 60, 1.0) != 0 || ReadoutIndexAt(1.0, 3, 60, 1.0) != 1 ||
        ReadoutIndexAt(2.4, 3, 60, 1.0) != 2 ||  // round 吸附到采样栅格
        ReadoutIndexAt(-0.5, 3, 60, 1.0) != -1 || ReadoutIndexAt(3.0, 3, 60, 1.0) != -1 ||
        ReadoutIndexAt(kUn, 3, 60, 1.0) != -1 || ReadoutIndexAt(1.0, 0, 60, 1.0) != -1 ||
        ReadoutIndexAt(1.0, 3, 60, 0.0) != -1) {
        *err = L"ReadoutIndexAt 逻辑下标反推错误";
        return false;
    }
    // 不满窗：count=3 < window，下标即逻辑下标；窗口裁剪（count=610、
    // window=60 -> 最新 60 个，x=0 对应逻辑 550）。
    if (ReadoutIndexAt(0.0, 3, 2, 1.0) != 1) {  // shown=2，逻辑 1 起
        *err = L"窗口裁剪时 ReadoutIndexAt 应返回 count-shown 起";
        return false;
    }
    return true;
}

// --- Phase D：每适配器序列构建（排除回环/非 Up、流量 Top8、NaN 容错）------
STM_TEST(chart_adapter_series) {
    constexpr double kUn = std::numeric_limits<double>::quiet_NaN();
    auto mk = [](uint64_t id, const wchar_t* name, double rx, double tx, bool up = true,
                 bool loop = false) {
        return stm::ui3::AdapterSeriesIn{id, name, rx, tx, up, loop};
    };

    // 12 条输入：1 回环、1 非 Up、12 个有效中取流量 Top8；NaN 流量排末尾。
    std::vector<stm::ui3::AdapterSeriesIn> in = {
        mk(1, L"回环", 1.0, 1.0, true, true),          // 回环：必须排除
        mk(2, L"禁用", 9.0e9, 9.0e9, false, false),    // 非 Up：必须排除
        mk(10, L"以太网", 100.0, 200.0),               // 合计 300
        mk(11, L"WLAN", 500.0, 500.0),                 // 合计 1000（Top1）
        mk(12, L"VPN", kUn, 400.0),                    // 收 NaN 取发 400
        mk(13, L"蓝牙", kUn, kUn),                     // 双 NaN：有效但排末尾
        mk(14, L"虚拟1", 10.0, 20.0),                  // 30
        mk(15, L"虚拟2", 30.0, 40.0),                  // 70
        mk(16, L"虚拟3", 0.0, 0.0),                    // 0（合法读数）
        mk(17, L"虚拟4", 60.0, 50.0),                  // 110
        mk(18, L"虚拟5", 80.0, 10.0),                  // 90
        mk(19, L"虚拟6", 20.0, 5.0),                   // 25
    };
    const std::vector<stm::ui3::AdapterSeriesIn> out = BuildAdapterSeries(in);
    // 12 -> 排除回环/禁用剩 10 -> Top8 截断；双 NaN（无有效读数）排末尾，
    // 不在流量前 8 内 -> 被截断（诚实：无读数不占序列位）。
    if (out.size() != kMaxAdapterSeries) {
        *err = stm::Fmt(L"序列应截断为 Top {}，实际 {}", kMaxAdapterSeries, out.size());
        return false;
    }
    // 期望顺序（流量降序）：WLAN 1000、VPN 400、以太网 300、虚拟4 110、
    // 虚拟5 90、虚拟2 70、虚拟1 30、虚拟6 25；蓝牙（NaN）与 0 流量的
    // 虚拟3 落选。
    const uint64_t wantIds[] = {11, 12, 10, 17, 18, 15, 14, 19};
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i].ifIndex != wantIds[i]) {
            *err = stm::Fmt(L"序列顺序错误：位 {} 应为 ifIndex {}", i, wantIds[i]);
            return false;
        }
    }
    for (const auto& o : out) {
        if (o.ifIndex == 13 || o.ifIndex == 16) {
            *err = L"NaN/零流量垫底适配器应被 Top8 截断";
            return false;
        }
    }
    // NaN 容错：VPN 单边 NaN 取发。
    if (out[1].TotalBps() != 400.0) {
        *err = L"VPN（收 NaN）应取发送 400";
        return false;
    }
    // NaN 排序：小集合内双 NaN 应排在全部有效读数之后（不截断时可见）。
    const std::vector<stm::ui3::AdapterSeriesIn> withNan = {
        mk(30, L"nan", kUn, kUn), mk(31, L"b", 5.0, 0.0), mk(32, L"a", 1.0, 1.0)};
    const std::vector<stm::ui3::AdapterSeriesIn> outNan = BuildAdapterSeries(withNan);
    if (outNan.size() != 3 || outNan[0].ifIndex != 31 || outNan[1].ifIndex != 32 ||
        outNan[2].ifIndex != 30 || outNan[2].TotalBps() == outNan[2].TotalBps()) {
        *err = L"双 NaN 适配器应排在有效读数之后且保持 NaN";
        return false;
    }
    // 少于 Top8：全保留、按流量降序。
    const std::vector<stm::ui3::AdapterSeriesIn> few = {
        mk(20, L"a", 1.0, 1.0), mk(21, L"b", 5.0, 0.0), mk(22, L"c", 2.0, 2.0)};
    const std::vector<stm::ui3::AdapterSeriesIn> outFew = BuildAdapterSeries(few);
    if (outFew.size() != 3 || outFew[0].ifIndex != 21 || outFew[1].ifIndex != 22 ||
        outFew[2].ifIndex != 20) {
        *err = L"少于 8 条应全保留并按流量降序";
        return false;
    }
    // 回环/禁用过滤后为空 -> 空序列（不崩）。
    if (!BuildAdapterSeries({mk(1, L"回环", 1, 1, true, true)}).empty()) {
        *err = L"仅回环输入应产出空序列";
        return false;
    }

    // GPU 合计口径（Phase C 摄取同款）：多卡求和钳 100、NaN 只跳过有效项。
    const double utils1[] = {30.0, 45.0};
    const double utils2[] = {80.0, 50.0};
    const double utils3[] = {kUn, kUn};
    if (GpuUtilClampSum(utils1, 2) != 75.0 || GpuUtilClampSum(utils2, 2) != 100.0) {
        *err = L"GPU 合计应求和并钳到 100";
        return false;
    }
    if (GpuUtilClampSum(utils3, 2) == GpuUtilClampSum(utils3, 2)) {
        *err = L"GPU 全 NaN 应返回 NaN";
        return false;
    }
    return true;
}

// --- Phase C：每核热图数据（行主序展平 + 尾窗裁剪 + NaN 补位）-------------
STM_TEST(chart_heatmap_values) {
    std::vector<Ring> cores(2, Ring{});
    for (int i = 0; i < 5; ++i) {
        cores[0].Push(static_cast<float>(i));        // 0..4
        cores[1].Push(i == 4 ? kNan : 100.0f - static_cast<float>(i));
    }
    // cols = min(120, window=3)：尾窗 = 逻辑 2..4。
    const stm::ui3::CoreHeatmap hm = CoreHeatmapValues(cores, 3);
    if (hm.rows != 2 || hm.cols != 3 || hm.values.size() != 6) {
        *err = stm::Fmt(L"热图尺寸错误：{}x{}", hm.rows, hm.cols);
        return false;
    }
    // 行主序：values[core*cols + t]；核 0 尾窗 = 2,3,4。
    if (hm.values[0] != 2.0f || hm.values[1] != 3.0f || hm.values[2] != 4.0f) {
        *err = L"核 0 尾窗展平错误";
        return false;
    }
    // 核 1 最新样本 NaN：照推（渲染为空白而非 0）。
    if (hm.values[3] != 98.0f || hm.values[5] == hm.values[5]) {
        *err = L"核 1 尾窗展平/NaN 补位错误";
        return false;
    }
    // window 上限 120 列与不满窗补 NaN：单样本环 + window=120 -> 119 个 NaN。
    std::vector<Ring> one(1, Ring{});
    one[0].Push(7.0f);
    const stm::ui3::CoreHeatmap hm2 = CoreHeatmapValues(one, 120);
    if (hm2.cols != 120 || hm2.values.size() != 120) {
        *err = L"热图列数应取 min(120, window)";
        return false;
    }
    if (hm2.values[119] != 7.0f || hm2.values[0] == hm2.values[0]) {
        *err = L"不满窗应尾部对齐、前方 NaN 补位";
        return false;
    }
    // 空核/非法窗 -> 空热图。
    if (CoreHeatmapValues({}, 60).rows != 0 || CoreHeatmapValues(one, 0).cols != 0) {
        *err = L"空输入应返回空热图";
        return false;
    }
    return true;
}
