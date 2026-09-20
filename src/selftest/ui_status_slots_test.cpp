// L1 布局防抖回归（2026-09）：状态栏定宽槽位布局数学。
// app/ui3/StatusLayout.h（纯函数 + 仅头文件 + 不依赖 ImGui，与
// app/ui/Pages.cpp 的 DrawStatusBar 共用同一份常量与合成函数）+
// app/ui/HeaderLayout.h（右段右对齐装箱，ui_header_test.cpp 已覆盖其不变式）。
//
// 用户报告①的根因：状态栏信息组过去把**当前数值文本**的实测宽喂给布局，
// 数值位数一变整个右段左右平移。修复契约（这里逐条断言）：
//  - 槽位坐标只依赖常量上限样本与窗口宽 —— 不同数值输入下坐标恒定；
//  - 任何经 FormatSlotMs/FormatSlotCount 产出的文本（含钳制上限）必能
//    放进按样本预留的槽（槽容量不变式）；
//  - LayoutFlowSlots 的窄窗降级（放不下的槽及其后隐藏）与旧 FlowSegmentFits
//    规则一致，且不依赖后续槽的宽度。
#include "selftest/TestFramework.h"
#include "app/ui/HeaderLayout.h"
#include "app/ui3/StatusLayout.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>

using namespace stm::ui;

namespace {

// 确定性字宽模型（仿真 ImGui::CalcTextSize 的角色，与真实字体无关）：
// UTF-8 多字节序列（中文/符号）= 18，ASCII 数字 = 8，小数点/空格 = 5，
// 其他 ASCII = 9。只要求对相同字符集合确定 —— 足以验证坐标恒定性与
// 槽容量不变式。
float FakeMeasure(const char* u8) {
    float w = 0.0f;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(u8); *p; ++p) {
        if (*p >= 0xC0) w += 18.0f;                 // 多字节序列首字节
        else if (*p >= 0x80) continue;               // 续字节（已计入首字节）
        else if (*p >= '0' && *p <= '9') w += 8.0f;
        else if (*p == '.' || *p == ' ') w += 5.0f;
        else w += 9.0f;
    }
    return w;
}

// 与 DrawStatusBar 相同的槽宽合成路径（测量函数换成 FakeMeasure）。
struct SlotWidths {
    float p95 = 0.0f;
    float queue = 0.0f;
    float procs = 0.0f;
    float csv = 0.0f;
    float frameRight = 0.0f;  // 右段帧耗时条目（含「|」）
};

SlotWidths ComposeSlotWidths() {
    const float pipeW = FakeMeasure("|");
    constexpr float kSp = 8.0f;
    const float msNumW = MaxSampleWidth(kMsNumberSamples, FakeMeasure);
    const float cntNumW = MaxSampleWidth(kCountNumberSamples, FakeMeasure);
    SlotWidths s;
    s.p95 = PipeSlotWidth(pipeW, kSp, TextSlotWidth(FakeMeasure(kP95Prefix), msNumW,
                                                    FakeMeasure(kMsSuffix)));
    s.queue = PipeSlotWidth(pipeW, kSp, AltSlotWidth(FakeMeasure(kQueuePrefix), cntNumW,
                                                     FakeMeasure(kQueueIdleText)));
    s.procs = PipeSlotWidth(pipeW, kSp, TextSlotWidth(FakeMeasure(kProcsPrefix), cntNumW, 0.0f));
    s.csv = PipeSlotWidth(pipeW, kSp, FakeMeasure("● 记录 CSV"));
    s.frameRight = PipeSlotWidth(pipeW, kSp, TextSlotWidth(FakeMeasure(kFrameMsPrefix), msNumW,
                                                           FakeMeasure(kMsSuffix)));
    return s;
}

// 数值输入组（帧耗时 ms, 采集 p95 ms, 队列数, 进程数）——覆盖 0、个位、
// 两位、三位、四位、钳制上限、超上限（钳制后）。
struct ValueSet {
    double frameMs;
    double p95;
    unsigned long long pending;
    unsigned long long procs;
};
constexpr ValueSet kValueSets[] = {
    {0.0, 0.0, 0, 0},
    {3.2, 2.5, 1, 300},
    {16.7, 99.9, 3, 4321},
    {123.4, 5.0, 0, 12000},
    {9999.9, 77.7, 99, 65535},
    {99999.9, 0.1, 5, 100000},
    {123456.7, 123.4, 12, 1000000},  // 双双超上限：格式化内部钳制
};
constexpr int kValueSetCount = static_cast<int>(sizeof(kValueSets) / sizeof(kValueSets[0]));

}  // namespace

// 核心契约：状态栏槽位在**不同数值输入**下坐标恒定（≥6 组断言）。
STM_TEST(status_slots_coords_constant_across_values) {
    const SlotWidths slots = ComposeSlotWidths();
    constexpr float kSp = 8.0f;
    constexpr float kStartX = 260.0f;
    float baseLeft[4] = {};
    float baseRight[3] = {};
    for (int wi = 0; wi < 3; ++wi) {
        const float contentW = wi == 0 ? 1600.0f : (wi == 1 ? 1100.0f : 900.0f);
        int baseCount = 0;
        for (int i = 0; i < kValueSetCount; ++i) {
            const ValueSet& v = kValueSets[i];
            // 按应用侧相同顺序先格式化当前数值文本（宽度绝不能进入下面的
            // 布局调用 —— 这正是被回归的契约）。
            char frameBuf[48];
            FormatSlotMs(frameBuf, kFrameMsPrefix, v.frameMs, kMsSuffix);
            char p95Buf[48];
            FormatSlotMs(p95Buf, kP95Prefix, v.p95, kMsSuffix);
            char queueBuf[48];
            if (v.pending > 0) FormatSlotCount(queueBuf, kQueuePrefix, v.pending);
            else snprintf(queueBuf, sizeof(queueBuf), "%s", kQueueIdleText);
            // 左段 4 槽（与 DrawStatusBar 相同的布线路径）。
            float x[4] = {};
            const float widths[4] = {slots.p95, slots.queue, slots.procs, slots.csv};
            const int firstBad =
                LayoutFlowSlots(kStartX, kSp, contentW - 6.0f, widths, 4, x);
            // 右段 3 条目：槽宽为常量上限；leftEnd 随“通知”长度小幅变化
            //（第 i 组给不同 leftEnd）——右段坐标仍必须恒定且全可见。
            const HeaderItem items[3] = {
                {190.0f, 2}, {slots.queue + 10.0f, 1}, {slots.frameRight, 0}};
            HeaderPlacement place[3] = {};
            const float leftEnd = 300.0f + static_cast<float>(i) * 5.0f;
            LayoutHeaderRight(contentW, leftEnd, kSp, items, 3, place);
            if (i == 0) {
                baseCount = firstBad;
                for (int k = 0; k < 4; ++k) baseLeft[k] = x[k];
                for (int k = 0; k < 3; ++k) baseRight[k] = place[k].x;
            } else {
                if (firstBad != baseCount) {
                    *err = L"窄窗隐藏槽数随数值变化（应为常量）";
                    return false;
                }
                for (int k = 0; k < firstBad && k < 4; ++k) {
                    if (x[k] != baseLeft[k]) {
                        *err = L"左段槽坐标随数值变化：槽 ";
                        *err += std::to_wstring(k);
                        return false;
                    }
                }
                if (contentW >= 900.0f) {
                    for (int k = 0; k < 3; ++k) {
                        if (!place[k].visible) {
                            *err = L"宽窗右段条目被隐藏（应为常量可见）";
                            return false;
                        }
                        if (place[k].x != baseRight[k]) {
                            *err = L"右段坐标随数值/leftEnd 变化：条目 ";
                            *err += std::to_wstring(k);
                            return false;
                        }
                    }
                }
            }
        }
    }
    return true;
}

// 槽容量不变式：任何数值（含钳制上限与超上限）格式化后的文本都必须
// 放得进按常量样本预留的槽 —— 数值变化被槽吸收，绝不溢出/推挤。
STM_TEST(status_slots_fit_invariant) {
    const SlotWidths slots = ComposeSlotWidths();
    for (int i = 0; i < kValueSetCount; ++i) {
        const ValueSet& v = kValueSets[i];
        char buf[48];
        FormatSlotMs(buf, kFrameMsPrefix, v.frameMs, kMsSuffix);
        if (FakeMeasure(buf) > TextSlotWidth(FakeMeasure(kFrameMsPrefix),
                                             MaxSampleWidth(kMsNumberSamples, FakeMeasure),
                                             FakeMeasure(kMsSuffix)) +
                                  0.001f) {
            *err = L"帧耗时文本溢出槽位";
            return false;
        }
        FormatSlotMs(buf, kP95Prefix, v.p95, kMsSuffix);
        if (FakeMeasure(buf) > TextSlotWidth(FakeMeasure(kP95Prefix),
                                             MaxSampleWidth(kMsNumberSamples, FakeMeasure),
                                             FakeMeasure(kMsSuffix)) +
                                  0.001f) {
            *err = L"采集 p95 文本溢出槽位";
            return false;
        }
        FormatSlotCount(buf, kQueuePrefix, v.pending);
        if (FakeMeasure(buf) > AltSlotWidth(FakeMeasure(kQueuePrefix),
                                            MaxSampleWidth(kCountNumberSamples, FakeMeasure),
                                            FakeMeasure(kQueueIdleText)) +
                                  0.001f) {
            *err = L"操作队列文本溢出槽位";
            return false;
        }
        if (FakeMeasure(kQueueIdleText) > AltSlotWidth(FakeMeasure(kQueuePrefix),
                                                       MaxSampleWidth(kCountNumberSamples,
                                                                      FakeMeasure),
                                                       FakeMeasure(kQueueIdleText)) +
                                             0.001f) {
            *err = L"操作队列空闲文案溢出槽位";
            return false;
        }
        FormatSlotCount(buf, kProcsPrefix, v.procs);
        if (FakeMeasure(buf) > TextSlotWidth(FakeMeasure(kProcsPrefix),
                                             MaxSampleWidth(kCountNumberSamples, FakeMeasure),
                                             0.0f) +
                                  0.001f) {
            *err = L"进程数文本溢出槽位";
            return false;
        }
    }
    // 钳制行为本身：超上限输入的渲染结果与上限样本逐字符同形。
    char buf[48];
    FormatSlotMs(buf, kFrameMsPrefix, 123456.7, kMsSuffix);
    if (std::strcmp(buf, "帧 99999.9 ms") != 0) {
        *err = L"帧耗时应钳制到 99999.9";
        return false;
    }
    FormatSlotCount(buf, kProcsPrefix, 1234567ull);
    if (std::strcmp(buf, "进程 999999") != 0) {
        *err = L"进程数应钳制到 999999";
        return false;
    }
    FormatSlotMs(buf, kFrameMsPrefix, -3.0, kMsSuffix);  // 负值/NaN 防御
    if (std::strcmp(buf, "帧 0.0 ms") != 0) {
        *err = L"负帧耗时应钳制到 0.0";
        return false;
    }
    FormatSlotMs(buf, kFrameMsPrefix, std::numeric_limits<double>::quiet_NaN(), kMsSuffix);
    if (std::strcmp(buf, "帧 0.0 ms") != 0) {
        *err = L"NaN 帧耗时应钳制到 0.0";
        return false;
    }
    return true;
}

// LayoutFlowSlots 数学：等距前进、firstBad 语义（放不下的槽及其后隐藏）、
// 坐标不依赖后续槽宽度 —— 窄窗降级阈值不随数据变化。
STM_TEST(status_flow_slots_math) {
    const float widths[4] = {80.0f, 90.0f, 70.0f, 60.0f};
    constexpr float kSp = 8.0f;
    float x[4] = {};
    // 全放得下：间距恒定，坐标逐槽累加。
    int n = LayoutFlowSlots(100.0f, kSp, 10000.0f, widths, 4, x);
    if (n != 4) { *err = L"宽窗应放下全部 4 槽"; return false; }
    for (int i = 1; i < 4; ++i) {
        if (x[i] - x[i - 1] != widths[i - 1] + kSp) {
            *err = L"槽间距不为常数";
            return false;
        }
    }
    // 恰好放下：x + w == limit 的槽仍然可见（与 FlowSegmentFits 的 <= 一致）。
    float limit = x[3] + widths[3];
    n = LayoutFlowSlots(100.0f, kSp, limit, widths, 4, x);
    if (n != 4) { *err = L"恰好放下的槽不应隐藏"; return false; }
    // 第三槽（下标 2）越界：返回 2（下标 2 起隐藏），且隐藏判定只看该槽自身。
    n = LayoutFlowSlots(100.0f, kSp, x[2] + widths[2] - 0.5f, widths, 4, x);
    if (n != 2) { *err = L"越界应停在槽下标 2"; return false; }
    // 坐标不依赖后续槽宽：缩短第 4 槽不影响前 3 槽。
    const float narrow[4] = {80.0f, 90.0f, 70.0f, 5.0f};
    float x2[4] = {};
    LayoutFlowSlots(100.0f, kSp, 10000.0f, narrow, 4, x2);
    for (int i = 0; i < 3; ++i) {
        if (x2[i] != x[i]) { *err = L"槽坐标依赖了后续槽宽"; return false; }
    }
    // spacing 为负时按 0 处理（防御）。
    float x3[4] = {};
    LayoutFlowSlots(100.0f, -1.0f, 10000.0f, widths, 2, x3);
    if (x3[1] - x3[0] != widths[0]) { *err = L"负间距应按 0 处理"; return false; }
    return true;
}

// 右段装配契约：帧耗时条目槽宽来自常量样本而非当前值 —— 对全部数值输入，
// 合成槽宽逐字节相等；右对齐装箱（LayoutHeaderRight）对恒定槽宽给出恒定坐标。
STM_TEST(status_right_group_slot_width_constant) {
    const SlotWidths slots = ComposeSlotWidths();
    constexpr float kSp = 8.0f;
    constexpr float kContentW = 1600.0f;
    float base[3] = {};
    for (int i = 0; i < kValueSetCount; ++i) {
        const HeaderItem items[3] = {{190.0f, 2}, {66.0f, 1}, {slots.frameRight, 0}};
        HeaderPlacement place[3] = {};
        LayoutHeaderRight(kContentW, 430.0f, kSp, items, 3, place);
        if (i == 0) {
            for (int k = 0; k < 3; ++k) base[k] = place[k].x;
        } else {
            for (int k = 0; k < 3; ++k) {
                if (place[k].x != base[k]) {
                    *err = L"右段帧耗时槽宽随数值变化";
                    return false;
                }
            }
        }
    }
    // 右对齐：帧耗时条目右缘贴内容右缘。
    if (base[2] + slots.frameRight < kContentW - 1.0f) {
        *err = L"帧耗时条目未右对齐到内容右缘";
        return false;
    }
    return true;
}
