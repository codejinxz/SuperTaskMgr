#pragma once
// F4#2 挂起/恢复 + 优先级/亲和性 UI 的纯逻辑部分（header-only）。
// 契约来自 src/ops/ProcessControl.h（G-A 实现，头已冻结）；本头只做
// 文案/映射/掩码换算，便于 stm_selftest 覆盖，无 ImGui / 无 app 对象。
#include <cstdint>
#include <string>
#include <vector>
#include "core/Str.h"
#include "ops/ProcessControl.h"

namespace stm {
namespace ui3 {

// 优先级六档中文文案（与 ops::ProcPriority 枚举序一致）。
inline std::wstring PriorityLabel(ops::ProcPriority p) {
    switch (p) {
        case ops::ProcPriority::Idle:         return L"空闲";
        case ops::ProcPriority::BelowNormal:  return L"低于标准";
        case ops::ProcPriority::Normal:       return L"标准";
        case ops::ProcPriority::AboveNormal:  return L"高于标准";
        case ops::ProcPriority::High:         return L"高";
        case ops::ProcPriority::Realtime:     return L"实时";
        default:                              return L"未知";
    }
}

// GetPriorityClass 常量 -> ops::ProcPriority；未知类返回 false（诚实显示 未知）。
inline bool PriorityFromWin32(uint32_t priorityClass, ops::ProcPriority* out) {
    switch (priorityClass) {
        case 0x00000040: *out = ops::ProcPriority::Idle; return true;          // IDLE_PRIORITY_CLASS
        case 0x00004000: *out = ops::ProcPriority::BelowNormal; return true;   // BELOW_NORMAL_PRIORITY_CLASS
        case 0x00000020: *out = ops::ProcPriority::Normal; return true;        // NORMAL_PRIORITY_CLASS
        case 0x00008000: *out = ops::ProcPriority::AboveNormal; return true;   // ABOVE_NORMAL_PRIORITY_CLASS
        case 0x00000080: *out = ops::ProcPriority::High; return true;          // HIGH_PRIORITY_CLASS
        case 0x00000100: *out = ops::ProcPriority::Realtime; return true;      // REALTIME_PRIORITY_CLASS
        default: return false;
    }
}

inline int AffinityCpuCount(uint64_t mask) {
    int n = 0;
    while (mask != 0) {
        mask &= (mask - 1);  // 清最低位 1
        ++n;
    }
    return n;
}

// 掩码 -> CPU 编号升序列表（至多 64 逻辑核；窗口单组掩码语义）。
inline std::vector<int> AffinityCpuList(uint64_t mask) {
    std::vector<int> out;
    for (int i = 0; i < 64 && mask != 0; ++i) {
        if ((mask & (1ull << i)) != 0) {
            out.push_back(i);
            mask &= ~(1ull << i);
        }
    }
    return out;
}

// 摘要文案："全部 8 个逻辑核" / "3 个（CPU 0,2,5）"；掩码为 0 视为未知 "—"。
inline std::wstring AffinitySummary(uint64_t mask) {
    if (mask == 0) return L"—";
    const std::vector<int> cpus = AffinityCpuList(mask);
    if (cpus.empty()) return L"—";
    // 全选（0..n-1 连续）时的简写
    bool contiguous = true;
    for (size_t i = 0; i < cpus.size(); ++i) {
        if (cpus[i] != static_cast<int>(i)) { contiguous = false; break; }
    }
    if (contiguous) return Fmt(L"全部 {} 个逻辑核", cpus.size());
    std::wstring list;
    for (size_t i = 0; i < cpus.size(); ++i) {
        if (i > 0) list += L",";
        list += std::to_wstring(cpus[i]);
        if (list.size() > 40) {  // 过长截断（极端核数），诚实标注省略
            list += L"…";
            break;
        }
    }
    return Fmt(L"{} 个（CPU {}）", cpus.size(), list);
}

}  // namespace ui3
}  // namespace stm
