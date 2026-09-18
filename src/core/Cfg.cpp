#include "core/Cfg.h"
#include "core/Str.h"
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <windows.h>

namespace stm {

namespace {

std::wstring Trim(std::wstring_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n')) ++b;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r' || s[e - 1] == L'\n')) --e;
    return std::wstring(s.substr(b, e - b));
}

// Minimal JSON string parser: handles \" \\ \/ \n \t \r \uXXXX (BMP only, enough for our config).
bool Unescape(std::wstring_view raw, std::wstring* out) {
    out->clear();
    if (raw.size() < 2 || raw.front() != L'"' || raw.back() != L'"') return false;
    raw = raw.substr(1, raw.size() - 2);
    for (size_t i = 0; i < raw.size(); ++i) {
        wchar_t c = raw[i];
        if (c != L'\\') { out->push_back(c); continue; }
        if (++i >= raw.size()) return false;
        switch (raw[i]) {
            case L'"': out->push_back(L'"'); break;
            case L'\\': out->push_back(L'\\'); break;
            case L'/': out->push_back(L'/'); break;
            case L'n': out->push_back(L'\n'); break;
            case L't': out->push_back(L'\t'); break;
            case L'r': out->push_back(L'\r'); break;
            case L'u': {
                if (i + 4 >= raw.size()) return false;
                const int v = static_cast<int>(wcstoul(std::wstring(raw.substr(i + 1, 4)).c_str(), nullptr, 16));
                out->push_back(static_cast<wchar_t>(v));
                i += 4;
                break;
            }
            default: return false;
        }
    }
    return true;
}

std::wstring Escape(std::wstring_view s) {
    std::wstring out;
    out.push_back(L'"');
    for (wchar_t c : s) {
        switch (c) {
            case L'"': out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n"; break;
            case L'\t': out += L"\\t"; break;
            case L'\r': out += L"\\r"; break;
            default:
                if (static_cast<unsigned>(c) < 0x20) {
                    out += Fmt(L"\\u{:04x}", static_cast<unsigned>(c));
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back(L'"');
    return out;
}

}  // namespace

bool Config::Load(const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    std::string u8;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) u8.append(buf, n);
    fclose(f);

    const std::wstring src = Utf8ToWide(u8);
    if (src.empty() && !u8.empty()) return false;
    items_.clear();

    // Locate '{' ... '}' then split top-level "key": value pairs (values have no braces).
    const size_t begin = src.find(L'{');
    const size_t end = src.rfind(L'}');
    if (begin == std::wstring::npos || end == std::wstring::npos || end <= begin) return false;

    size_t i = begin + 1;
    while (i < end) {
        const size_t kq1 = src.find(L'"', i);
        if (kq1 == std::wstring::npos || kq1 >= end) break;
        const size_t kq2 = src.find(L'"', kq1 + 1);
        if (kq2 == std::wstring::npos) break;
        std::wstring key;
        if (!Unescape(src.substr(kq1, kq2 - kq1 + 1), &key)) { i = kq2 + 1; continue; }

        const size_t colon = src.find(L':', kq2 + 1);
        if (colon == std::wstring::npos || colon >= end) break;
        // value runs until ',' or '}' at depth 0; strings may contain ',' so honor quotes.
        size_t v = colon + 1;
        while (v < end && src[v] == L' ') ++v;
        bool inStr = false;
        size_t ve = v;
        for (; ve < end; ++ve) {
            const wchar_t c = src[ve];
            if (inStr) { if (c == L'\\') ++ve; else if (c == L'"') inStr = false; }
            else if (c == L'"') inStr = true;
            else if (c == L',' || c == L'}') break;
        }
        items_.emplace_back(std::move(key), Trim(src.substr(v, ve - v)));
        i = ve + 1;
    }
    return true;
}

bool Config::Save(const std::wstring& path) const {
    std::wstring j = L"{\n";
    for (size_t i = 0; i < items_.size(); ++i) {
        j += L"  " + Escape(items_[i].first) + L": " + items_[i].second;
        if (i + 1 < items_.size()) j += L",";
        j += L"\n";
    }
    j += L"}\n";
    const std::string u8 = WideToUtf8(j);
    // Atomic write (arch section 7): temp file + MoveFileExW REPLACE_EXISTING, so a
    // crash mid-write can never leave a truncated config/session behind.
    const std::wstring tmp = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) return false;
    const bool ok = fwrite(u8.data(), 1, u8.size(), f) == u8.size();
    fclose(f);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

const std::pair<std::wstring, std::wstring>* Config::Find(std::wstring_view key) const {
    for (const auto& kv : items_) {
        if (kv.first == key) return &kv;
    }
    return nullptr;
}

std::wstring& Config::Slot(std::wstring_view key) {
    for (auto& kv : items_) {
        if (kv.first == key) return kv.second;
    }
    items_.emplace_back(std::wstring(key), std::wstring());
    return items_.back().second;
}

std::wstring Config::GetString(std::wstring_view key, std::wstring_view def) const {
    const auto* kv = Find(key);
    if (!kv) return std::wstring(def);
    std::wstring out;
    return Unescape(kv->second, &out) ? out : std::wstring(def);
}

int64_t Config::GetInt(std::wstring_view key, int64_t def) const {
    const auto* kv = Find(key);
    if (!kv || kv->second.empty()) return def;
    wchar_t* endp = nullptr;
    const long long v = wcstoll(kv->second.c_str(), &endp, 10);
    return (endp && *endp == L'\0') ? v : def;
}

double Config::GetDouble(std::wstring_view key, double def) const {
    const auto* kv = Find(key);
    if (!kv || kv->second.empty()) return def;
    wchar_t* endp = nullptr;
    const double v = wcstod(kv->second.c_str(), &endp);
    return (endp && *endp == L'\0') ? v : def;
}

bool Config::GetBool(std::wstring_view key, bool def) const {
    const auto* kv = Find(key);
    if (!kv) return def;
    return _wcsicmp(kv->second.c_str(), L"true") == 0;
}

void Config::SetString(std::wstring_view key, std::wstring_view v) {
    Slot(key) = Escape(v);   // keep insertion order stable across saves
}
void Config::SetInt(std::wstring_view key, int64_t v) { Slot(key) = Fmt(L"{}", v); }
void Config::SetDouble(std::wstring_view key, double v) { Slot(key) = Fmt(L"{:.3f}", v); }
void Config::SetBool(std::wstring_view key, bool v) { Slot(key) = v ? L"true" : L"false"; }

}  // namespace stm
