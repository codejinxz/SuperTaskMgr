// P3 任务三：关闭行为 cfg（closeAction）—— 0/1/2 读写、缺省键默认 0、
// 非法值归一 0，以及 config.json 持久化往返。逻辑在 app/ui/ConfirmAction.h
// （kCloseActionCfgKey + NormalizeCloseAction），无 GUI 依赖。
#include "selftest/TestFramework.h"
#include "app/ui/ConfirmAction.h"
#include "core/Cfg.h"
#include <windows.h>
#include <string>

using namespace stm;
using namespace stm::ui;

STM_TEST(ui_close_action_cfg) {
    // 缺省键 -> 默认 0（每次询问）。
    Config fresh;
    if (NormalizeCloseAction(fresh.GetInt(kCloseActionCfgKey, 0)) != 0) {
        *err = L"缺省键未返回默认 0（每次询问）";
        return false;
    }

    // 0/1/2 原样读写。
    Config c;
    for (int64_t v = 0; v <= 2; ++v) {
        c.SetInt(kCloseActionCfgKey, v);
        if (NormalizeCloseAction(c.GetInt(kCloseActionCfgKey, 0)) != static_cast<int>(v)) {
            *err = L"closeAction 读写往返失败：" + std::to_wstring(v);
            return false;
        }
    }

    // 非法值一律归一为 0（宁多问一次，不做危险假设）。
    for (int64_t bad : {-1, 3, 7, 99, 123456789}) {
        if (NormalizeCloseAction(bad) != 0) {
            *err = L"非法 closeAction 未归一为 0：" + std::to_wstring(bad);
            return false;
        }
    }

    // 持久化往返：写 2（最小化到托盘）-> 存盘 -> 重新加载仍为 2。
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring p = std::wstring(temp) + L"stm_selftest_closeaction.json";
    c.SetInt(kCloseActionCfgKey, 2);
    if (!c.Save(p)) { *err = L"closeAction 配置保存失败"; return false; }
    Config reloaded;
    if (!reloaded.Load(p)) { *err = L"closeAction 配置加载失败"; return false; }
    if (NormalizeCloseAction(reloaded.GetInt(kCloseActionCfgKey, 0)) != 2) {
        *err = L"closeAction=2 持久化往返失败";
        return false;
    }
    // 另一值再验证一轮（1 = 直接退出）。
    c.SetInt(kCloseActionCfgKey, 1);
    if (!c.Save(p)) { *err = L"closeAction 配置二次保存失败"; return false; }
    Config reloaded2;
    if (!reloaded2.Load(p) || NormalizeCloseAction(reloaded2.GetInt(kCloseActionCfgKey, 0)) != 1) {
        *err = L"closeAction=1 持久化往返失败";
        return false;
    }
    DeleteFileW(p.c_str());
    return true;
}
