#pragma once
// 字符串工具：UTF 转换、宽字符格式化、人类可读的指标格式化。
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace stm {

// std::format 的封装，支持运行期宽字符格式串（MSVC /std:c++20）。
template <typename... Args>
std::wstring Fmt(std::wstring_view fmt, Args&&... args) {
    return std::vformat(fmt, std::make_wformat_args(args...));
}

std::string WideToUtf8(std::wstring_view w);
std::wstring Utf8ToWide(std::string_view u);

// 0 -> "0 B"；1536 -> "1.50 KiB"；刻意使用二进制（1024）单位。
std::wstring FormatBytes(uint64_t bytes);
// bytes 可能是 kUnavailU64 -> L"—"。rate 单位为字节/秒。
std::wstring FormatRate(double bytesPerSec);
// percent：NaN -> L"—"，否则 "12.3%"
std::wstring FormatPercent(double percent);
// NaN -> L"—"，否则输出带千位分隔符的 "1234"。
std::wstring FormatNumber(double value);
// seconds -> "3 天 4 小时" / "12.5 秒" style uptime strings.
std::wstring FormatDuration(double seconds);

}  // namespace stm
