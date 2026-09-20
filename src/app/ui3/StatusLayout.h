#pragma once
// ============================================================================
//  状态栏定宽槽位布局（L1 布局防抖，2026-09）。
//
//  纯函数、仅头文件、不依赖 ImGui —— 应用（app/ui/Pages.cpp 的
//  DrawStatusBar）与 stm_selftest（src/selftest/ui_status_slots_test.cpp）
//  共用同一份格式化常量与槽位数学，契约由该自测断言。
//
//  根因（用户报告①「右下角的『帧 xx毫秒』不断变化位置」）：状态栏信息组
//  过去把**当前数值文本**的实测像素宽（ImGui::CalcTextSize(当前格式化
//  结果)）直接当作布局宽。数值位数一变（"3.2" -> "123.4"），实测宽随之
//  变化，右对齐坐标整体平移，相邻项被左右推挤。
//
//  修复契约（本头文件 + 调用方共同遵守）：
//   1. 槽位宽度只能来自**常量上限样本**：kMsNumberSamples /
//      kCountNumberSamples 给出数字段的最宽字形样本（覆盖每一位数字、
//      含钳制上限 "99999.9"/"999999"），调用方对样本实测取 max。样本是
//      编译期常量 → 槽宽逐帧恒定。
//   2. 槽位坐标只由（起点、间距、槽宽、窗口宽）决定 —— 数值变化只改变
//      槽内文本，绝不改变任何槽位坐标（含窄窗隐藏阈值）。
//   3. 文本在槽内一律**左对齐**：起笔位置固定，数值变长向槽内右侧的
//      保留空隙伸展；相邻项与分隔符「|」恒定不动。
//   4. 渲染文本必须能放进按样本预留的槽：小数/计数字段只允许经
//      FormatSlotMs / FormatSlotCount 产出（内部钳制到样本覆盖的上限，
//      例如休眠唤醒后首帧的巨大帧耗时），保证永不溢出。
// ============================================================================

#include <cstdio>
#include <cstddef>

namespace stm {
namespace ui {

// ---- 定宽数字文本：应用与 selftest 共用的唯一格式化入口 --------------------

// 小数字段："prefix{v:.1f}suffix"。v 钳制到 [0, 99999.9]：渲染字符数
// 至多 7（"99999.9"），与 kMsNumberSamples 的上限样本逐字符对齐。
inline void FormatSlotMs(char (&out)[48], const char* prefixUtf8, double v,
                         const char* suffixUtf8) {
    if (!(v > 0.0)) v = 0.0;  // NaN/负值 -> 0（NaN 的比较恒为 false）
    if (v > 99999.9) v = 99999.9;
    std::snprintf(out, sizeof(out), "%s%.1f%s", prefixUtf8, v, suffixUtf8);
}

// 计数字段："prefix{v}suffix"（十进制）。钳制到 6 位，与
// kCountNumberSamples 的上限样本对齐。
inline void FormatSlotCount(char (&out)[48], const char* prefixUtf8,
                            unsigned long long v, const char* suffixUtf8 = "") {
    if (v > 999999ull) v = 999999ull;
    std::snprintf(out, sizeof(out), "%s%llu%s", prefixUtf8, v, suffixUtf8);
}

// 状态栏各槽的文案常量（改这里必须同步 kMsNumberSamples/kCountNumberSamples
// 与 src/selftest/ui_status_slots_test.cpp）。
inline constexpr const char* kFrameMsPrefix = "帧 ";        // + ms 数 + " ms"
inline constexpr const char* kP95Prefix = "采集 p95 ";      // + ms 数 + " ms"
inline constexpr const char* kMsSuffix = " ms";
inline constexpr const char* kQueuePrefix = "操作队列 ";    // 忙：+ 数；闲：见下
inline constexpr const char* kQueueIdleText = "操作队列空闲";
inline constexpr const char* kProcsPrefix = "进程 ";        // + 数

// ---- 上限样本：数字段按位覆盖常见字体中最宽的字形组合（含钳制上限）-------

inline constexpr const char* kMsNumberSamples[] = {
    "88888.8", "99999.9", "00000.0", "11111.1", "44444.4", "77777.7"};
inline constexpr const char* kCountNumberSamples[] = {
    "888888", "999999", "000000", "111111"};

// 对一组常量样本实测并取最大宽。运行端 measure = ImGui::CalcTextSize(s).x；
// selftest 注入确定性字宽模型。样本都是编译期常量 → 返回值逐帧恒定。
template <typename MeasureF, std::size_t N>
inline float MaxSampleWidth(const char* const (&samples)[N], MeasureF&& measure) {
    float w = 0.0f;
    for (std::size_t i = 0; i < N; ++i) {
        const float m = measure(samples[i]);
        if (m > w) w = m;
    }
    return w;
}

// ---- 槽宽合成（纯）--------------------------------------------------------
// ImGui 逐字形排版、无字距（kerning）：整串实测宽 == 分段实测宽之和，
// 因此槽宽可以安全地按段合成。

// 文本槽：前缀 + 定宽数字段 + 后缀。
inline float TextSlotWidth(float prefixW, float maxNumberW, float suffixW) {
    return prefixW + maxNumberW + suffixW;
}

// 二选一文案槽（如「操作队列 88」/「操作队列空闲」）：取两种渲染的较大者。
inline float AltSlotWidth(float prefixW, float maxNumberW, float altTextW) {
    return prefixW + (maxNumberW > altTextW ? maxNumberW : altTextW);
}

// 右段条目槽：每个条目前带「|」分隔符（分隔符宽计入槽，槽左缘 = 分隔符位）。
inline float PipeSlotWidth(float pipeW, float spacing, float textW) {
    return pipeW + spacing + textW;
}

// ---- 槽位坐标（纯）--------------------------------------------------------

// 左段固定槽位：从 startX 自左向右布 count 个定宽槽（间隔 spacing）。
// outX[i] = 槽 i 的绝对 x（与 ImGui::SameLine(offset) 的窗口相对坐标同系）。
// 返回第一个 x + width 越过 limitX 的槽下标（该槽及其后不应绘制 —— 与旧
// FlowSegmentFits 的窄窗降级规则一致）；全部放得下返回 count。
// 坐标只依赖入参：槽宽恒定时（契约 1），数值变化绝不改变坐标。
inline int LayoutFlowSlots(float startX, float spacing, float limitX,
                           const float* widths, int count, float* outX) {
    if (spacing < 0.0f) spacing = 0.0f;
    float x = startX;
    for (int i = 0; i < count; ++i) {
        if (i > 0) x += spacing;
        outX[i] = x;
        if (x + widths[i] > limitX) return i;
        x += widths[i];
    }
    return count;
}

}  // namespace ui
}  // namespace stm
