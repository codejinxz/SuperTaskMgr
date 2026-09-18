#pragma once
// ============================================================================
//  外观/持久化纯逻辑（H-A）。Header-only，无 ImGui —— stm_selftest
//  （selftest/ui_about_test.cpp）与 UI（Pages.cpp / main.cpp）共用。
//
//  「恢复默认列宽」的两步语义：
//    1) 内存软删除：colW_* 值置为 ""（Get* 立即回退到调用方默认值）；
//    2) 配置文件剔除：按行重写 config.json 剔除 colW_* 行（原子 tmp+替换，
//       行格式即 core/Cfg.cpp Save 的输出）。退出时 cfg.Save 会把软删除键写回
//       ""，main.cpp 随后用 onlyEmptyValues=true 再剔一次 —— 用户重置后重新
//       调整并落盘的真实宽度（数字值）不会被误删。
// ============================================================================
#include "app/ui/SortKey.h"
#include "core/Cfg.h"
#include <cstdio>
#include <string>
#include <vector>
#include <windows.h>

namespace stm {
namespace ui3 {

// ----------------------------------------------------------------------------
// 登记的 colW_* 键清单（与 app/ui/Pages.cpp ProcessesPage::PersistWidths 的
// 写入一一对应：SortColumn 0..10 = "colW_"+SortColumnId，另加徽标/描述两列）。
// 新增表格列时：先改 PersistWidths/LoadPersistedOnce，再同步这里。
// ----------------------------------------------------------------------------
inline std::vector<std::wstring> ColWidthCfgKeys() {
    std::vector<std::wstring> keys;
    keys.reserve(static_cast<size_t>(ui::SortColumn::Count) + 2);
    for (int i = 0; i <= static_cast<int>(ui::SortColumn::CtxSwitches); ++i) {
        keys.emplace_back(std::wstring(L"colW_") +
                          ui::SortColumnId(static_cast<ui::SortColumn>(i)));
    }
    keys.emplace_back(L"colW_badges");
    keys.emplace_back(L"colW_desc");
    return keys;
}

// 内存软删除（Cfg 无删除 API 且 src/core 冻结）：置 "" 后 Get* 一律回退默认。
inline void SoftDeleteColWidthKeys(Config& cfg) {
    for (const std::wstring& k : ColWidthCfgKeys()) {
        cfg.SetString(k, L"");
    }
}

// 从 config.json 剔除 colW_* 键。onlyEmptyValues=true 只剔软删除残留（值为 ""
// 的行）；false 剔除全部 colW_* 行（点击「恢复默认列宽」时磁盘值本就是上一
// 次运行的旧宽度，直接全剔）。行格式不认识的文件（手工编辑过）不动、返回 -1。
// 返回剔除的行数；文件不存在返回 0。
inline int StripColWidthKeysFromFile(const std::wstring& cfgPath, bool onlyEmptyValues) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, cfgPath.c_str(), L"rb") != 0 || f == nullptr) return 0;
    std::string u8;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) u8.append(buf, n);
    fclose(f);

    std::vector<std::string> lines;  // 每行保留原始字节（含结尾 \n）
    size_t pos = 0;
    while (pos <= u8.size()) {
        const size_t nl = u8.find('\n', pos);
        const size_t end = nl == std::string::npos ? u8.size() : nl + 1;
        lines.push_back(u8.substr(pos, end - pos));
        if (nl == std::string::npos) break;
        pos = end;
    }

    constexpr const char* kPrefix = "colW_";
    constexpr size_t kPrefixLen = 5;  // strlen("colW_")（sizeof 陷阱：kPrefix 是指针）
    int removed = 0;
    std::string out;
    out.reserve(u8.size());
    for (const std::string& line : lines) {
        // 行形状：{ 前空白 } + "key" + ':' + value + [','] + [\r]\n
        size_t b = 0;
        while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
        const char c0 = b < line.size() ? line[b] : '\0';
        if (line.empty() || c0 == '{' || c0 == '}' || c0 == '\n' || c0 == '\r' || c0 == '\0') {
            out += line;  // 结构行/空行：原样保留
            continue;
        }
        if (c0 != '"') return -1;  // 不认识的形状：放弃重写（宁缺毋滥，绝不截断）
        // 找到 key 的结束引号（容忍 \" 转义；colW_* 键本身无转义，前缀按原字节匹配）
        size_t q = b + 1;
        while (q < line.size()) {
            if (line[q] == '\\') { q += 2; continue; }
            if (line[q] == '"') break;
            ++q;
        }
        if (q >= line.size()) return -1;
        const std::string rawKey = line.substr(b + 1, q - b - 1);
        const size_t colon = line.find(':', q + 1);
        if (colon == std::string::npos) return -1;
        size_t vb = colon + 1, ve = line.size();
        while (vb < ve && (line[vb] == ' ' || line[vb] == '\t')) ++vb;
        while (ve > vb && (line[ve - 1] == ',' || line[ve - 1] == '\n' ||
                           line[ve - 1] == '\r' || line[ve - 1] == ' ' || line[ve - 1] == '\t')) {
            --ve;
        }
        const std::string rawValue = line.substr(vb, ve - vb);
        const bool isColW = rawKey.compare(0, kPrefixLen, kPrefix) == 0;
        const bool isEmptyValue = rawValue == "\"\"";  // Escape(L"") 的输出
        if (isColW && (!onlyEmptyValues || isEmptyValue)) {
            ++removed;  // 丢弃该行
            continue;
        }
        out += line;
    }

    if (removed <= 0) return 0;  // 无可剔：不重写（保留原文件 mtime/内容）
    const std::string payload = out;
    const std::wstring tmp = cfgPath + L".tmp";
    FILE* w = nullptr;
    if (_wfopen_s(&w, tmp.c_str(), L"wb") != 0 || w == nullptr) return -1;
    const bool ok = fwrite(payload.data(), 1, payload.size(), w) == payload.size();
    fclose(w);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return -1;
    }
    if (!MoveFileExW(tmp.c_str(), cfgPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return -1;
    }
    return removed;
}

}  // namespace ui3
}  // namespace stm
