#include "core/Str.h"
#include "core/ProcData.h"
#include <windows.h>

namespace stm {

std::string WideToUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(std::string_view u) {
    if (u.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      u.data(), static_cast<int>(u.size()), nullptr, 0);
    if (n <= 0) return {};  // invalid utf-8 -> empty, callers treat as missing data
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        u.data(), static_cast<int>(u.size()), out.data(), n);
    return out;
}

std::wstring FormatBytes(uint64_t bytes) {
    static const wchar_t* kUnits[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB"};
    if (bytes == kUnavailU64) return L"—";
    double v = static_cast<double>(bytes);
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    if (i == 0) return Fmt(L"{} B", bytes);
    return Fmt(L"{:.2f} {}", v, kUnits[i]);
}

std::wstring FormatRate(double bytesPerSec) {
    if (bytesPerSec != bytesPerSec) return L"—";  // NaN check (no <cmath> noise)
    return FormatBytes(static_cast<uint64_t>(bytesPerSec)) + L"/s";
}

std::wstring FormatPercent(double percent) {
    if (percent != percent) return L"—";
    return Fmt(L"{:.1f}%", percent);
}

std::wstring FormatNumber(double value) {
    if (value != value) return L"—";
    auto i = static_cast<long long>(value);
    std::wstring digits = Fmt(L"{}", i);
    std::wstring out;
    const int len = static_cast<int>(digits.size());
    for (int pos = 0; pos < len; ++pos) {
        out += digits[pos];
        const int rest = len - 1 - pos;
        if (rest > 0 && rest % 3 == 0 && digits[pos] != L'-') out += L',';
    }
    return out;
}

std::wstring FormatDuration(double seconds) {
    if (seconds != seconds || seconds < 0) return L"—";
    auto total = static_cast<long long>(seconds);
    const long long days = total / 86400; total %= 86400;
    const long long hours = total / 3600; total %= 3600;
    const long long mins = total / 60;
    const long long secs = total % 60;
    if (days > 0) return Fmt(L"{} 天 {} 小时 {} 分", days, hours, mins);
    if (hours > 0) return Fmt(L"{} 小时 {} 分", hours, mins);
    if (mins > 0) return Fmt(L"{} 分 {} 秒", mins, secs);
    return Fmt(L"{} 秒", secs);
}

}  // namespace stm
