#pragma once
// UTF-8 rendering helper for the UI layer.
// ImGui is a narrow-char API while every data string and UI label is wide; U8()
// converts on first use and caches the result by string value so per-frame
// repeated calls (thousands of table cells) do not re-convert.
// std::unordered_map nodes are stable: pointers returned by U8() stay valid
// until the cache is cleared (which only happens when it exceeds kMaxEntries).
#include <cstddef>
#include <string>
#include <unordered_map>
#include "core/Str.h"

namespace stm {
namespace ui {

inline const char* U8(const std::wstring& w) {
    constexpr size_t kMaxEntries = 8192;  // names + labels; hard upper bound
    thread_local std::unordered_map<std::wstring, std::string> cache;
    auto it = cache.find(w);
    if (it != cache.end()) return it->second.c_str();
    if (cache.size() >= kMaxEntries) cache.clear();
    return cache.emplace(w, WideToUtf8(w)).first->second.c_str();
}

}  // namespace ui
}  // namespace stm
