#pragma once
// Minimal JSON config (flat object; values: string / int64 / double / bool). No third-party deps.
#include <cstdint>
#include <string>
#include <vector>

namespace stm {

class Config {
public:
    // Both return false on parse/IO failure (defaults apply; file kept untouched on parse error).
    bool Load(const std::wstring& path);
    bool Save(const std::wstring& path) const;

    std::wstring GetString(std::wstring_view key, std::wstring_view def = L"") const;
    int64_t GetInt(std::wstring_view key, int64_t def = 0) const;
    double GetDouble(std::wstring_view key, double def = 0.0) const;
    bool GetBool(std::wstring_view key, bool def = false) const;

    void SetString(std::wstring_view key, std::wstring_view v);
    void SetInt(std::wstring_view key, int64_t v);
    void SetDouble(std::wstring_view key, double v);
    void SetBool(std::wstring_view key, bool v);

private:
    const std::pair<std::wstring, std::wstring>* Find(std::wstring_view key) const;
    std::wstring& Slot(std::wstring_view key);  // find-or-append value slot
    // key -> raw JSON literal (quoted string / number / true / false), insertion-ordered.
    std::vector<std::pair<std::wstring, std::wstring>> items_;
};

}  // namespace stm
