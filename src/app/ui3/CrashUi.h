#pragma once
// F4#5 崩溃记录页的纯逻辑部分：级别/事件 ID 文案与本地时间格式化。
// 契约来自 src/ops/CrashLog.h（G-A 实现，头已冻结）；无 ImGui / 无 app 对象。
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include "core/Str.h"
#include "ops/CrashLog.h"

namespace stm {
namespace ui3 {

// 事件级别（winevt 渲染级别常量）-> 中文短标签。
inline std::wstring CrashLevelLabel(uint16_t level) {
    switch (level) {
        case 1: return L"严重";  // Critical
        case 2: return L"错误";  // Error
        case 3: return L"警告";  // Warning
        case 4: return L"信息";  // Information
        default: return level == 0 ? std::wstring(L"未知") : Fmt(L"级别 {}", level);
    }
}

// 常见事件 ID 的中文含义（其余如实显示原始 ID）。
inline std::wstring CrashEventIdLabel(uint32_t eventId) {
    switch (eventId) {
        case 1000: return L"应用错误";
        case 1001: return L"错误报告";
        case 1002: return L"应用挂起";
        default: return L"事件";
    }
}

// unix 秒 -> 本地时间 "yyyy-MM-dd HH:mm:ss"（19 字符定宽；失败返回 "—"）。
inline std::wstring FormatUnixTimeLocal(int64_t unixSec) {
    if (unixSec <= 0) return L"—";
    const time_t t = static_cast<time_t>(unixSec);
    std::tm tmv{};
    if (localtime_s(&tmv, &t) != 0) return L"—";
    wchar_t buf[32] = {};
    if (swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
                   tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec) <= 0) {
        return L"—";
    }
    return buf;
}

}  // namespace ui3
}  // namespace stm
