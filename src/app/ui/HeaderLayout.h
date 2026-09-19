#pragma once
// ============================================================================
//  头部/工具栏与状态栏右侧布局数学（R-Fix Bug1，2026-09）。
//
//  纯函数、仅头文件、不依赖 ImGui：输入是实测像素宽度，输出是
//  x 偏移。工具栏过去用硬编码的 ImGui::SameLine(GetWindowWidth() - NNN)
//  定位右侧，无视条目的真实宽度——窄窗口下条目相互重叠，
//  并盖住工具栏「?」按钮，使“关于”无法点击（Bug3）。R-Fix 重构后，
//  工具栏右侧只有「?」按钮，状态栏右侧为
//  [全局热键][权限徽标][帧耗时]，
//  bar right segment packs [全局热键][权限徽标][帧耗时] with this function.
//
//  算法：把可见条目从右边缘向左装箱（数组顺序 == 屏幕顺序，
//  items[0] 最靠左），间隔用 `spacing`。只要有可见条目放不进
//  左侧流（止于 leftFlowEndX）与已摆位条目之间，
//  就隐藏优先级数字最大（重要性最低）的可见条目并重新装箱。
//  priority == 0 的条目绝不隐藏：当连它也放不下时，
//  将它钳制在 leftFlowEndX 处，并隐藏任何会与之重叠的
//  可见条目。
//
//  契约（由探针自测断言，见 app/UiFixProbe.cpp --mode=layout
//  与 src/selftest/ui_header_test.cpp）：
//   - 摆位条目互不重叠，也绝不遮盖左侧流（x >= leftFlowEndX）
//     ——用真实的工具栏/状态栏测量对任意宽度 >= 600 验证。
//
//   - 可见性单调：某条目可见时，所有更高优先级（更小数字）
//     的条目也可见——重要性低的先隐藏。
//   - 使用真实测量时，任意窗口宽度 >= 900px 下所有条目都可见
//     且右对齐。
// ============================================================================
#include <cstddef>

namespace stm {
namespace ui {

struct HeaderItem {
    float width;
    int priority;  // 0 = 只要有槽位就保留；越大越先隐藏
};

struct HeaderPlacement {
    float x;        // 相对内容左缘的偏移（与 leftFlowEndX 同一坐标系）
    bool visible;
};

// 容量护栏：外壳工具栏用 3 个条目；需要更多的调用方自行调高。
inline constexpr int kHeaderMaxItems = 16;

// 返回可见条目数。`out` 接收 `count` 个条目。
inline int LayoutHeaderRight(float contentWidth, float leftFlowEndX, float spacing,
                             const HeaderItem* items, int count, HeaderPlacement* out) {
    if (count <= 0) return 0;
    if (spacing < 0.0f) spacing = 0.0f;
    if (leftFlowEndX < 0.0f) leftFlowEndX = 0.0f;

    float widths[kHeaderMaxItems] = {};
    bool visible[kHeaderMaxItems] = {};
    float xs[kHeaderMaxItems] = {};
    if (count > kHeaderMaxItems) count = kHeaderMaxItems;
    for (int i = 0; i < count; ++i) {
        widths[i] = items[i].width > 0.0f ? items[i].width : 0.0f;
        visible[i] = true;
    }

    // 对当前可见条目做右 -> 左装箱。
    const auto pack = [&]() {
        float nextRight = contentWidth;
        for (int i = count - 1; i >= 0; --i) {
            if (!visible[i]) {
                xs[i] = 0.0f;
                continue;
            }
            xs[i] = nextRight - widths[i];
            nextRight = xs[i] - spacing;
        }
    };

    // 隐藏优先级最低（数字最大）的越界者，直到全部放得下。
    for (;;) {
        pack();
        int victim = -1;
        for (int i = 0; i < count; ++i) {
            if (!visible[i] || xs[i] >= leftFlowEndX) continue;
            if (victim == -1 || items[i].priority > items[victim].priority) victim = i;
        }
        if (victim == -1) break;
        if (items[victim].priority == 0) break;  // 绝不隐藏；下方钳制
        visible[victim] = false;
    }

    // priority-0 兜底：钳制到左侧流末端，并清掉被钳制矩形
    // 压住的任何可见条目（它们优先级更低）。
    int visibleCount = 0;
    for (int i = 0; i < count; ++i) {
        if (visible[i] && items[i].priority == 0 && xs[i] < leftFlowEndX) {
            const float clamped = leftFlowEndX;
            for (int j = 0; j < count; ++j) {
                if (j == i || !visible[j]) continue;
                if (xs[j] < clamped + widths[i]) visible[j] = false;
            }
            xs[i] = clamped;
        }
    }
    for (int i = 0; i < count; ++i) {
        out[i].x = xs[i];
        out[i].visible = visible[i];
        visibleCount += visible[i] ? 1 : 0;
    }
    return visibleCount;
}

// 判断给定宽度的从左向右流动段在内容右缘之前
// 是否仍放得下（两坐标均在窗口内容空间）。
// 状态栏用它：窄窗口时隐藏次要段，而不是让它们
// 在边框处被裁切。
inline bool FlowSegmentFits(float cursorX, float contentRightX, float segWidth) {
    return cursorX + segWidth <= contentRightX;
}

}  // namespace ui
}  // namespace stm
