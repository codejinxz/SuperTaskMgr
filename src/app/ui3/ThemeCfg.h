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
#include <atomic>
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

// ----------------------------------------------------------------------------
// P1③：网络页/适配器明细表格列宽持久化（netcol_<表名>_<列>）。
// ImGui 自身布局因 io.IniFilename = nullptr 不落盘，列宽经 cfg 持久化；
// 表名/列序与 Pages3.cpp / Pages.cpp 中 BeginTable 的 TableSetupColumn
// 一一对应（见 NetColTableSpecs）。拉伸列（WidthStretch）不持久化。
// ----------------------------------------------------------------------------
// 键名生成：表名为 ASCII（BeginTable 的 id），逐字节升宽拼接
//（const char* 不能直接 operator+ 到 wstring）。
inline std::wstring NetColCfgKey(const char* table, int col) {
    std::wstring k = L"netcol_";
    for (const char* p = table; *p != '\0'; ++p) {
        k += static_cast<wchar_t>(*p);
    }
    k += L'_';
    k += std::to_wstring(col);
    return k;
}

inline std::wstring NetColCfgKeyPrefix() { return L"netcol_"; }

// 登记的 netcol 表清单（id 与 BeginTable 第一参一致；cols=固定列数上限，
// 覆盖全部列号，拉伸列在写入端跳过）。新增持久化列宽的表：先改这里。
struct NetColTableSpec {
    const char* id;
    int cols;
};

inline const NetColTableSpec* NetColTableSpecs(int* count) {
    static const NetColTableSpec kSpecs[] = {
        {"netmon_events", 8},  // 实时监视·连接事件表
        {"netmon_top", 5},     // 实时监视·Top 远程目标表
        {"netmon_dns", 3},     // 实时监视·DNS 解析记录表
        {"pcap_pkts", 5},      // 深度抓包表
        {"netconn", 6},        // 连接表
        {"gpuadapters", 4},    // 性能页·GPU 适配器明细
        {"gpuprocs", 4},       // 性能页·GPU 进程明细
    };
    if (count != nullptr) *count = static_cast<int>(sizeof(kSpecs) / sizeof(kSpecs[0]));
    return kSpecs;
}

// 全部 netcol_* 键（「重置布局」的软删除与剔除清单）。
inline std::vector<std::wstring> NetColCfgKeys() {
    std::vector<std::wstring> keys;
    int n = 0;
    const NetColTableSpec* specs = NetColTableSpecs(&n);
    for (int t = 0; t < n; ++t) {
        for (int c = 0; c < specs[t].cols; ++c) {
            keys.push_back(NetColCfgKey(specs[t].id, c));
        }
    }
    return keys;
}

// ----------------------------------------------------------------------------
// P1④：「重置布局」的键清单 —— 布局缩放/进程表列顺序/进程表列宽/网络表
// 列宽/性能页放大块。全部软删除 + 从 config.json 剔除。
// ----------------------------------------------------------------------------
inline std::vector<std::wstring> LayoutResetExactKeys() {
    return {std::wstring(L"layoutScale"), std::wstring(L"colOrder"),
            std::wstring(L"perfZoom")};
}

inline void SoftDeleteLayoutKeys(Config& cfg) {
    SoftDeleteColWidthKeys(cfg);
    for (const std::wstring& k : NetColCfgKeys()) {
        cfg.SetString(k, L"");
    }
    for (const std::wstring& k : LayoutResetExactKeys()) {
        cfg.SetString(k, L"");
    }
}

// 从 config.json 剔除键：键名等于 exactKeys 之一，或以 prefixes 之一开头。
// onlyEmptyValues=true 只剔软删除残留（值为 "" 的行）；false 剔除全部匹配行
//（点击「恢复默认列宽」时磁盘值本就是上一次运行的旧宽度，直接全剔）。
// 行格式不认识的文件（手工编辑过）不动、返回 -1。
// 返回剔除的行数；文件不存在返回 0。
inline int StripCfgKeysFromFile(const std::wstring& cfgPath,
                                const std::vector<std::string>& prefixes,
                                const std::vector<std::string>& exactKeys,
                                bool onlyEmptyValues) {
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
        // 找到 key 的结束引号（容忍 \" 转义；本清单键本身无转义，按原字节匹配）
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
        bool isMatch = false;
        for (const std::string& p : prefixes) {
            if (rawKey.compare(0, p.size(), p) == 0) { isMatch = true; break; }
        }
        if (!isMatch) {
            for (const std::string& k : exactKeys) {
                if (rawKey == k) { isMatch = true; break; }
            }
        }
        const bool isEmptyValue = rawValue == "\"\"";  // Escape(L"") 的输出
        if (isMatch && (!onlyEmptyValues || isEmptyValue)) {
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

// 兼容入口：剔除 colW_*（进程表列宽）。语义同 StripCfgKeysFromFile。
inline int StripColWidthKeysFromFile(const std::wstring& cfgPath, bool onlyEmptyValues) {
    return StripCfgKeysFromFile(cfgPath, {"colW_"}, {}, onlyEmptyValues);
}

// P1④：「重置布局」剔除 —— colW_* / netcol_* 前缀 + layoutScale/colOrder/
// perfZoom 精确键，一次重写完成。
inline int StripLayoutKeysFromFile(const std::wstring& cfgPath) {
    return StripCfgKeysFromFile(cfgPath, {"colW_", "netcol_"},
                                {"layoutScale", "colOrder", "perfZoom"}, false);
}

}  // namespace ui3
}  // namespace stm

// V29-P1-1：布局重置代际。「重置布局」时递增；NetCol/GpuCol 等列宽内存缓存
// 记录 loadedGen 与之对比，变化即失效重读 cfg（否则重置后本次会话不回默认宽，
// 且后续拖列会以旧宽为基线写回——与「已重置布局」的承诺矛盾）。
namespace stm {
namespace ui3 {
inline std::atomic<uint64_t>& LayoutResetGen() {
    static std::atomic<uint64_t> g{0};
    return g;
}
inline uint64_t LayoutResetGeneration() { return LayoutResetGen().load(std::memory_order_acquire); }
inline void NotifyLayoutReset() { LayoutResetGen().fetch_add(1, std::memory_order_release); }
}  // namespace ui3
}  // namespace stm
