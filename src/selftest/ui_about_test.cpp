// H-A: 主题三态 + 关于对话框的纯逻辑测试。被测头（app/Theme.h、app/ui/AboutUi.h、
// app/ui3/ThemeCfg.h、app/AboutInfo.h）均无 ImGui 依赖，stm_selftest 只链接
// core/collect/ops 即可运行（同 ui_test.cpp 的做法）。
#include "selftest/TestFramework.h"
#include "app/AboutInfo.h"
#include "app/Theme.h"
#include "app/ui/AboutUi.h"
#include "app/ui3/ThemeCfg.h"
#include "core/Cfg.h"
#include <windows.h>

namespace {

std::wstring TempFile(const wchar_t* name) {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    return std::wstring(temp) + name;
}

}  // namespace

// cfg 键 themeMode 的 0/1/2 读写往返 + 越界值钳制（非法一律回退 Dark）。
STM_TEST(theme_mode_roundtrip) {
    using stm::ThemeMode;
    using stm::ThemeModeFromInt;
    stm::Config c;
    for (int64_t v = 0; v <= static_cast<int64_t>(ThemeMode::System); ++v) {
        c.SetInt(L"themeMode", v);
        if (c.GetInt(L"themeMode", 0) != v) {
            *err = L"cfg 内存读写 themeMode 失败";
            return false;
        }
        if (ThemeModeFromInt(v) != static_cast<ThemeMode>(static_cast<int>(v))) {
            *err = L"合法值 0/1/2 的钳制结果不符";
            return false;
        }
    }
    if (ThemeModeFromInt(7) != ThemeMode::Dark || ThemeModeFromInt(-1) != ThemeMode::Dark) {
        *err = L"越界值未回退 Dark";
        return false;
    }
    // 文件往返：跟随系统（2）存盘后重载仍为 System。
    const std::wstring p = TempFile(L"stm_selftest_theme.json");
    c.SetInt(L"themeMode", static_cast<int64_t>(ThemeMode::System));
    if (!c.Save(p)) { *err = L"Config::Save 失败"; return false; }
    stm::Config r;
    if (!r.Load(p)) { *err = L"Config::Load 失败"; return false; }
    DeleteFileW(p.c_str());
    if (ThemeModeFromInt(r.GetInt(L"themeMode", 0)) != ThemeMode::System) {
        *err = L"themeMode 文件往返失败";
        return false;
    }
    return true;
}

// 系统主题解析：读不到注册表（注入 reader 返回 false / 空指针）默认深色；
// 读到 1 => 浅色、0 => 深色。真实探测必须落在 Dark/Light 二者之一。
STM_TEST(resolve_system_registry_missing) {
    using stm::ThemeMode;
    using stm::ResolveSystemWith;
    constexpr auto failReader = [](void*, bool*) { return false; };
    if (ResolveSystemWith(failReader, nullptr) != ThemeMode::Dark) {
        *err = L"注册表读取失败时未回退深色";
        return false;
    }
    if (ResolveSystemWith(nullptr, nullptr) != ThemeMode::Dark) {
        *err = L"空 reader 未回退深色";
        return false;
    }
    constexpr auto lightReader = [](void*, bool* light) { *light = true; return true; };
    constexpr auto darkReader = [](void*, bool* light) { *light = false; return true; };
    if (ResolveSystemWith(lightReader, nullptr) != ThemeMode::Light) {
        *err = L"AppsUseLightTheme=1 未解析为浅色";
        return false;
    }
    if (ResolveSystemWith(darkReader, nullptr) != ThemeMode::Dark) {
        *err = L"AppsUseLightTheme=0 未解析为深色";
        return false;
    }
    return true;
}

// 窗口标题版本后缀规则：空版本 => 纯标题；非空 => "标题 vX"；且与架构契约头
// AboutInfo.h::WindowTitleWithVersion() 对当前常量版本保持一致。
STM_TEST(about_title_version) {
    using stm::ui::AboutWindowTitleFor;
    if (AboutWindowTitleFor(L"") != L"超级任务管理器" ||
        AboutWindowTitleFor(nullptr) != L"超级任务管理器") {
        *err = L"空版本时标题不应带后缀";
        return false;
    }
    if (AboutWindowTitleFor(L"9.9.9") != L"超级任务管理器 9.9.9") {
        *err = L"非空版本时标题缺少 ' vX' 后缀";
        return false;
    }
    const std::wstring contract = stm::WindowTitleWithVersion();
    if (contract != AboutWindowTitleFor(stm::kAppVersion)) {
        *err = L"与 AboutInfo.h::WindowTitleWithVersion 结果不一致";
        return false;
    }
    if (contract.rfind(L"超级任务管理器", 0) != 0) {
        *err = L"契约标题缺少应用名前缀";
        return false;
    }
    return true;
}

// colW_* 键剔除：全量模式剔掉所有 colW_* 行并保留无关键；空值模式只剔软删除
// 残留（"" 值）；格式异常的文件不动（返回 -1）。
STM_TEST(colw_strip_file) {
    // 登记的键清单必须覆盖 PersistWidths 的写入：SortColumn 全列 + 徽标/描述。
    const std::vector<std::wstring> keys = stm::ui3::ColWidthCfgKeys();
    if (keys.size() != static_cast<size_t>(stm::ui::SortColumn::Count) + 2) {
        *err = L"colW_* 键清单数量与表格列不匹配";
        return false;
    }

    // 1) 全量剔除：colW_* 消失，其他键原样保留。
    const std::wstring p = TempFile(L"stm_selftest_colw.json");
    {
        stm::Config c;
        c.SetDouble(L"colW_pid", 123.5);
        c.SetDouble(L"colW_desc", 0.7);
        c.SetInt(L"themeMode", 1);
        c.SetString(L"其他", L"保留值");
        if (!c.Save(p)) { *err = L"Config::Save 失败"; return false; }
    }
    const int removed = stm::ui3::StripColWidthKeysFromFile(p, false);
    stm::Config r;
    if (removed != 2 || !r.Load(p)) {
        DeleteFileW(p.c_str());
        *err = L"全量剔除失败（应剔 2 行且文件仍可解析）";
        return false;
    }
    if (r.GetDouble(L"colW_pid", -7.0) != -7.0 || r.GetDouble(L"colW_desc", -7.0) != -7.0 ||
        r.GetInt(L"themeMode", 0) != 1 || r.GetString(L"其他") != L"保留值") {
        DeleteFileW(p.c_str());
        *err = L"剔除后键集不符（colW 消失，其余必须保留）";
        return false;
    }

    // 2) 空值模式：只剔软删除残留（""），真实宽度（数字）保留。
    {
        stm::Config c;
        c.SetString(L"colW_cpu", L"");   // 软删除：落盘为 ""
        c.SetDouble(L"colW_mem", 55.0);  // 重置后用户重新调整过的宽度
        c.SetBool(L"keep", true);
        if (!c.Save(p)) { *err = L"Config::Save 失败"; return false; }
    }
    const int removedEmpty = stm::ui3::StripColWidthKeysFromFile(p, true);
    stm::Config r2;
    const bool ok2 = removedEmpty == 1 && r2.Load(p) &&
                     r2.GetDouble(L"colW_cpu", -7.0) == -7.0 &&
                     r2.GetDouble(L"colW_mem", -7.0) == 55.0 && r2.GetBool(L"keep", false);
    DeleteFileW(p.c_str());
    if (!ok2) {
        *err = L"空值剔除失败（只应剔 \"\" 值的 colW 行）";
        return false;
    }

    // 3) 格式异常：不重写、返回 -1、内容原样。
    const std::wstring bad = TempFile(L"stm_selftest_colw_bad.json");
    FILE* f = nullptr;
    if (_wfopen_s(&f, bad.c_str(), L"wb") != 0 || !f) { *err = L"测试文件创建失败"; return false; }
    fwrite("not a config {", 1, 14, f);
    fclose(f);
    const int badResult = stm::ui3::StripColWidthKeysFromFile(bad, false);
    bool badOk = badResult == -1;
    if (badOk) {
        FILE* g = nullptr;
        if (_wfopen_s(&g, bad.c_str(), L"rb") == 0 && g) {
            char buf[32] = {};
            const size_t n = fread(buf, 1, sizeof(buf) - 1, g);
            fclose(g);
            badOk = std::string(buf, n) == "not a config {";
        }
    }
    DeleteFileW(bad.c_str());
    if (!badOk) {
        *err = L"格式异常文件被改动（应原样保留并返回 -1）";
        return false;
    }
    return true;
}
