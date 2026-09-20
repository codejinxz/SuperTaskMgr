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
#include <cstring>
#include <string>

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

// ---- 锚点段截断（P1①，纯）------------------------------------------------
// 根因补充（用户报告①「完整采集模式…与相邻槽位叠在一起」）：DrawStatusBar
// 过去用 `GetCursorPosX()` 在绘制锚点文本（「完整模式」/「兼容模式：<原因>」）
// 之后采集槽位起点 —— 而 ImGui 的 ItemSize 在每个条目绘制完成后把
// CursorPos.x 重置回**行首**（imgui.cpp：`window->DC.CursorPos.x =
// IM_TRUNC(window->Pos.x + Indent.x + ColumnsOffset.x)`），所以该值恒为
// 行首（≈WindowPadding.x），与锚点文本的真实末端无关：定宽槽全部压在锚点
// 文本上（降级原因越长叠得越明显）。修复契约：
//   1. 锚点末端只能由 `起点 + 锚点文本实测宽` 计算（本文件
//      AnchorEndX），绝不读回光标；
//   2. 降级原因文本按可用宽度经 EllipsizeTextUtf8 截断加「…」，悬停
//      tooltip 显示完整原因（完整信息另有兼容模式说明模态）；
//   3. 无降级时只有紧凑的「完整模式」徽标，原因槽自然消失不占位。
inline float AnchorEndX(float anchorStartX, float anchorTextW) {
    return anchorStartX + (anchorTextW > 0.0f ? anchorTextW : 0.0f);
}

// UTF-8 感知的按宽截断：在 maxWidth 内找完整字符（码点）边界，放不下的
// 尾部以「…」（U+2026，3 字节）收尾；首字符即超宽时只返回「…」。
// measure(str) 返回整串实测宽（应用注入 ImGui::CalcTextSize；selftest 注入
// 确定性字宽模型）。输出写 out（含 NUL，最多 outCap 字节），返回写入字节数
//（不含 NUL）。非码点首字节（0x80-0xBF）按 ASCII 边界处理，绝越界不崩。
template <typename MeasureF>
inline int EllipsizeTextUtf8(const char* u8, float maxWidth, MeasureF&& measure,
                             char* out, size_t outCap) {
    if (outCap == 0) return 0;
    out[0] = '\0';
    if (u8 == nullptr || maxWidth <= 0.0f) return 0;
    const size_t len = strlen(u8);
    // 快路径：整串本来就放得下（含「…」比整串还宽的情形），原样保留。
    if (measure(u8) <= maxWidth) {
        const size_t n = len < outCap - 1 ? len : outCap - 1;
        std::memcpy(out, u8, n);
        out[n] = '\0';
        return static_cast<int>(n);
    }
    const float ellW = measure("\xE2\x80\xA6");  // "…"
    if (ellW > maxWidth) return 0;               // 连「…」都放不下：什么都不画
    size_t keep = 0;                             // 可保留的字节数
    while (keep < len) {
        const unsigned char c = static_cast<unsigned char>(u8[keep]);
        size_t step = 1;                          // 码点字节数（ASCII=1）
        if (c >= 0xF0) step = 4;
        else if (c >= 0xE0) step = 3;
        else if (c >= 0xC0) step = 2;
        if (keep + step > len) step = len - keep;  // 截断序列防御
        // measure 只读入参（CalcTextSize / FakeMeasure）；std::string 拷贝
        // 自带 NUL 结尾，无需触碰原串。
        const float w = measure(std::string(u8, keep + step).c_str());
        const float ellReserve = (keep + step < len) ? ellW : 0.0f;
        if (w + ellReserve > maxWidth) break;
        maxWidth -= w;
        keep += step;
    }
    if (keep == 0) {                               // 一个字符都放不下
        if (outCap < 4) return 0;
        std::memcpy(out, "\xE2\x80\xA6", 3);
        out[3] = '\0';
        return 3;
    }
    const size_t n = keep < outCap - 4 ? keep : (outCap >= 4 ? outCap - 4 : 0);
    std::memcpy(out, u8, n);
    std::memcpy(out + n, "\xE2\x80\xA6", 3);
    out[n + 3] = '\0';
    return static_cast<int>(n + 3);
}

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
