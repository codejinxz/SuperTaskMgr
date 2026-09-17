#pragma once
// String utilities: UTF conversions, wide formatting, human-readable metric formatting.
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace stm {

// std::format wrapper for runtime wide format strings (MSVC /std:c++20).
template <typename... Args>
std::wstring Fmt(std::wstring_view fmt, Args&&... args) {
    return std::vformat(fmt, std::make_wformat_args(args...));
}

std::string WideToUtf8(std::wstring_view w);
std::wstring Utf8ToWide(std::string_view u);

// 0 -> "0 B"; 1536 -> "1.50 KiB"; binary (1024) units on purpose.
std::wstring FormatBytes(uint64_t bytes);
// bytes may be kUnavailU64 -> L"—". rate in bytes/sec.
std::wstring FormatRate(double bytesPerSec);
// percent: NaN -> L"—", otherwise "12.3%"
std::wstring FormatPercent(double percent);
// NaN -> L"—", otherwise "1234" with thousands separators.
std::wstring FormatNumber(double value);
// seconds -> "3 天 4 小时" / "12.5 秒" style uptime strings.
std::wstring FormatDuration(double seconds);

}  // namespace stm
