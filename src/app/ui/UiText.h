#pragma once
// UI 层的 UTF-8 渲染辅助。
// ImGui 是窄字符 API，而所有数据字符串与 UI 标签都是宽字符；U8()
// 首次使用时转换并按字符串值缓存结果，帧内成千上万次
// 重复调用（海量表格单元格）不会反复转换。
// std::unordered_map 节点稳定：U8() 返回的指针保持有效，
// 直到缓存被清空（只在超过 kMaxEntries 时发生）。
#include <cstddef>
#include <string>
#include <unordered_map>
#include "core/Str.h"

namespace stm {
namespace ui {

inline const char* U8(const std::wstring& w) {
    constexpr size_t kMaxEntries = 8192;  // 名称 + 标签；硬上界
    thread_local std::unordered_map<std::wstring, std::string> cache;
    auto it = cache.find(w);
    if (it != cache.end()) return it->second.c_str();
    if (cache.size() >= kMaxEntries) cache.clear();
    return cache.emplace(w, WideToUtf8(w)).first->second.c_str();
}

}  // namespace ui
}  // namespace stm
