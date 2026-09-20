#pragma once
// ============================================================================
//  M2 页面布局防抖（2026-09）：网络页 / 传感器页「顶栏位置固定」的
//  纯函数契约。仅头文件、不依赖 ImGui 渲染状态（调用方注入行高等实测值），
//  与 StatusLayout.h（L1 状态栏定宽槽位）同一思路：
//
//  根因（用户报告「页面顶部控制行/区块标题随内容上下移动」）：
//   - 网络页适配器卡区（AutoResizeY 卡片 + 可展开「更多适配器」）高度随
//     适配器数量/展开状态变化，把下方「实时监视」「连接表」上下顶动；
//   - 传感器页分组区（各组 AutoResizeY）高度随读数条目出现/消失、LHM 行
//     合入、组显隐切换而变化 —— 页面总高变化使父级滚动参与布局，
//     顶栏随滚动偏移上下移动。
//
//  修复契约（本头文件 + Pages3.cpp 调用方共同遵守，selftest
//  src/selftest/ui_m2_test.cpp 断言）：
//   1. 两页的内容区各包进一个**定高滚动区**（BeginChild 固定高度，内部
//      滚动）。定高区高度只由（调用方实测的可用高度、编译期常量上限/
//      下限）决定 —— 内容增减绝不是高度的输入，故区外任何元素的 y 坐标
//      逐帧恒定（窗口缩放是唯一合法的布局变化来源）。
//   2. 加载态/错误态/说明行等出现即改变高度的行，一律画进定高区**内部**。
//   3. 头部行内可能出现的提示文字必须 SameLine（行数恒定）。
//   4. 高度/可视行数/溢出判定全部经本头文件的纯函数计算，便于
//      stm_selftest 无 GUI 覆盖。
// ============================================================================

#include <cstddef>

#include "app/ui3/PageHelpers.h"  // SensorGroup（分组显隐集合的元素类型）

namespace stm {
namespace ui3 {

// ---- 网络页：适配器卡区 ----------------------------------------------------

// 适配器定高卡区的默认高度（约 200px；窗口极矮时退化为可用高度，
// 见 AdapterRegionHeight）。
inline constexpr float kAdapterRegionHeight = 200.0f;

// 单张适配器卡片的固定行数：1 标题行（名称/类型/状态/速度/复制 IP）
// + 6 明细行（IPv4、IPv6、网关、DNS、MAC、DHCP —— 与 DrawAdapterCard 的
// 基线明细一致；超出基线的部分是 extraAddressRows）。
inline constexpr int kAdapterCardFixedRows = 7;

// 卡片子窗口上下内边距的保守行数换算（Border + FramePadding.y*2）。
inline constexpr int kAdapterCardPadRows = 2;

// 适配器卡区高度（纯）：正常窗口恒为 kAdapterRegionHeight；仅当页面
// 可用高度更小（极矮窗口 / 离屏 smoke 顺序绘制耗尽空间）时退化为
// 正的可用值。入参只有可用高度 —— 卡片数量/展开状态绝非输入。
inline float AdapterRegionHeight(float pageAvailY) {
    float h = pageAvailY;
    if (!(h > 0.0f)) h = 1.0f;  // 0/负/NaN 防御（离屏绘制可能耗尽空间）
    if (h > kAdapterRegionHeight) h = kAdapterRegionHeight;
    return h;
}

// 单卡高度估算（纯）：rowH = 单行实测高（GetTextLineHeightWithSpacing），
// extraAddressRows = 超出 6 行基线明细的附加地址行（多 IPv6 地址）。
// 仅用于行内溢出提示（估算），不参与真实布局。
inline float AdapterCardHeightEstimate(float rowH, int extraAddressRows) {
    if (!(rowH > 0.0f)) rowH = 0.0f;  // 0/负/NaN 防御
    const int extra = extraAddressRows > 0 ? extraAddressRows : 0;
    return rowH * static_cast<float>(kAdapterCardFixedRows + kAdapterCardPadRows +
                                     extra);
}

// 卡片总高估算（纯）：Σ 单卡 + 卡片间条目间距（按 1 行/张估算，
// n 张共 n-1 个间隙）。
inline float AdapterCardsTotalHeight(float rowH, const int* extraAddressRows,
                                     int count) {
    if (count <= 0) return 0.0f;
    float total = 0.0f;
    for (int i = 0; i < count; ++i) {
        total += AdapterCardHeightEstimate(
            rowH, extraAddressRows != nullptr ? extraAddressRows[i] : 0);
    }
    return total + rowH * static_cast<float>(count - 1);
}

// 定高区域内可完整可见的行数（纯，floor 语义；非法输入按 0 行）。
inline int VisibleRowsInFixedHeight(float regionH, float rowH) {
    if (!(regionH > 0.0f) || !(rowH > 0.0f)) return 0;
    const int n = static_cast<int>(regionH / rowH);
    return n > 0 ? n : 0;
}

// 卡片是否溢出定高区（纯）：超出时头部出现 SameLine 的「可滚动」提示。
// 提示出现/消失不改变行数 —— 行内文字绝不影响顶栏高度契约。
inline bool AdapterCardsOverflow(float regionH, float cardsTotalH) {
    return cardsTotalH > regionH;
}

// ---- 传感器页：分组展示定高区 ----------------------------------------------

// 分组展示区高度上限：约 600px —— 大窗口下页面总高 = 顶栏 + 600，
// 仍有富余，父级永不出现滚动条。
inline constexpr float kSensorGroupsMaxHeight = 600.0f;

// 分组展示区下限：极矮窗口按可用高度收缩到本值为止；再矮则接受父级
// 滚动条重现（降级：定高仍恒定，顶栏只在用户主动滚动时离开视野）。
inline constexpr float kSensorGroupsMinHeight = 240.0f;

// 收缩时给父级底部保留的余量（防四舍五入诱发的父级滚动条）。
inline constexpr float kSensorRegionBottomMargin = 6.0f;

// 分组展示区高度（纯）：实现选「页高减顶栏」（availBelowChrome 由调用方
// 在顶栏绘制完成后用 GetContentRegionAvail().y 测取 —— 顶栏行数恒定，
// 故该值只随窗口缩放变化），减去常量余量后钳制到 [240, 600]。
// 内容（读数条目、LHM 行、组显隐、加载/说明行）绝不是输入。
inline float SensorGroupsRegionHeight(float availBelowChrome) {
    float h = availBelowChrome - kSensorRegionBottomMargin;
    if (!(h > 0.0f)) h = kSensorGroupsMinHeight;  // 0/负/NaN 防御
    if (h > kSensorGroupsMaxHeight) h = kSensorGroupsMaxHeight;
    if (h < kSensorGroupsMinHeight) h = kSensorGroupsMinHeight;
    return h;
}

// ---- 传感器页：显隐状态 → 分组可见集合（纯）--------------------------------

// 分组可见集合：bit i = SensorGroup 枚举序 i 是否可见。
using SensorGroupMask = unsigned;

static_assert(sizeof(SensorGroupMask) * 8 >=
                  static_cast<int>(SensorGroup::Count),
              "SensorGroupMask 位宽必须容纳全部分组");

// 布尔可见性数组（按 SensorGroup 枚举序）→ 位掩码。count 超出
// SensorGroup::Count 的部分忽略，负数按 0 处理。
inline SensorGroupMask SensorGroupMaskFromFlags(const bool* visible, int count) {
    SensorGroupMask m = 0;
    const int limit = static_cast<int>(SensorGroup::Count);
    const int n = count < limit ? (count < 0 ? 0 : count) : limit;
    for (int i = 0; i < n; ++i) {
        if (visible[i]) m |= (1u << i);
    }
    return m;
}

// 分组是否在可见集合中（枚举越界一律 false）。
inline bool SensorGroupMaskHas(SensorGroupMask m, SensorGroup g) {
    const int i = static_cast<int>(g);
    if (i < 0 || i >= static_cast<int>(SensorGroup::Count)) return false;
    return (m & (1u << i)) != 0;
}

// 可见分组个数（集合基数，供诊断/提示使用）。
inline int SensorGroupMaskCount(SensorGroupMask m) {
    int n = 0;
    for (int i = 0; i < static_cast<int>(SensorGroup::Count); ++i) {
        if ((m & (1u << i)) != 0) ++n;
    }
    return n;
}

}  // namespace ui3
}  // namespace stm
