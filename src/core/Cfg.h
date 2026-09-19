#pragma once
// 极简 JSON 配置（扁平对象；值类型：string / int64 / double / bool）。无第三方依赖。
#include <cstdint>
#include <string>
#include <vector>

namespace stm {

class Config {
public:
    // 解析/IO 失败时两者都返回 false（使用默认值；解析出错时不改动文件）。
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
    std::wstring& Slot(std::wstring_view key);  // 查找或追加值槽位
    // key -> 原始 JSON 字面量（带引号字符串 / 数字 / true / false），按插入顺序排列。
    std::vector<std::pair<std::wstring, std::wstring>> items_;
};

}  // namespace stm
