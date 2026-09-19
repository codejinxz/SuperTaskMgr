// stm_selftest：core/collect/ops 的控制台冒烟测试（架构第 11 节）。
// 退出码 = 失败数。--json 输出机器可读摘要。
#include "selftest/TestFramework.h"
#include "core/Cfg.h"
#include "core/Jobs.h"
#include "core/Notifications.h"
#include "core/ProcData.h"
#include "core/ProtectedList.h"
#include "core/Str.h"
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace stmtest {
std::vector<TestCase>& Registry() {
    static std::vector<TestCase> r;
    return r;
}
}  // namespace stmtest

// ---- core：字符串/格式化辅助 ----
STM_TEST(core_wide_utf8_roundtrip) {
    if (stm::WideToUtf8(stm::Utf8ToWide("进程 abc")) != "进程 abc") {
        *err = L"Utf8ToWide/WideToUtf8 往返失败";
        return false;
    }
    const std::wstring src = L"进程管理器 abc 123";
    if (stm::Utf8ToWide(stm::WideToUtf8(src)) != src) { *err = L"宽字符往返失败"; return false; }
    return true;
}

STM_TEST(core_format_bytes) {
    if (stm::FormatBytes(0) != L"0 B") { *err = L"FormatBytes(0)"; return false; }
    if (stm::FormatBytes(1536) != L"1.50 KiB") { *err = L"FormatBytes(1536)=" + stm::FormatBytes(1536); return false; }
    if (stm::FormatBytes(stm::kUnavailU64) != L"—") { *err = L"FormatBytes(unavail)"; return false; }
    return true;
}

STM_TEST(core_format_percent_number) {
    if (stm::FormatPercent(stm::kUnavail) != L"—") { *err = L"FormatPercent(NaN)"; return false; }
    if (stm::FormatPercent(12.34) != L"12.3%") { *err = L"FormatPercent(12.34)=" + stm::FormatPercent(12.34); return false; }
    if (stm::FormatNumber(1234567.0) != L"1,234,567") { *err = L"FormatNumber=" + stm::FormatNumber(1234567.0); return false; }
    return true;
}

// ---- core：配置往返（中文值）----
STM_TEST(core_cfg_roundtrip) {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring p = std::wstring(temp) + L"stm_selftest_cfg.json";

    stm::Config c1;
    c1.SetString(L"名称", L"中文值\"引号\"");
    c1.SetInt(L"interval", 1500);
    c1.SetBool(L"enabled", true);
    c1.SetDouble(L"ratio", 2.5);
    if (!c1.Save(p)) { *err = L"Config::Save 失败"; return false; }

    stm::Config c2;
    if (!c2.Load(p)) { *err = L"Config::Load 失败"; return false; }
    if (c2.GetString(L"名称") != L"中文值\"引号\"") { *err = L"中文字符串往返失败"; return false; }
    if (c2.GetInt(L"interval") != 1500 || !c2.GetBool(L"enabled") ||
        c2.GetDouble(L"ratio") != 2.5) {
        *err = L"数值往返失败";
        return false;
    }
    DeleteFileW(p.c_str());
    return true;
}

// ---- core：任务队列 ----
STM_TEST(core_jobs_submit_shutdown) {
    stm::JobQueue q;
    if (!q.Start()) { *err = L"JobQueue::Start 失败"; return false; }
    auto done = std::make_shared<bool>(false);
    q.Submit([done] { *done = true; });
    // 关停前先等任务开始：排队但未开始的任务按设计会在
    // Shutdown 时被丢弃（架构第 5 节）。
    for (int i = 0; i < 200 && !*done; ++i) Sleep(10);
    q.Shutdown(2000);
    if (!*done) { *err = L"队列任务未执行"; return false; }
    if (q.Submit([] {}) != 0) { *err = L"Shutdown 后仍接受任务"; return false; }
    return true;
}

// ---- core：通知 ----
STM_TEST(core_notifications_drain) {
    stm::NotificationQueue nq;
    stm::Notification n;
    n.kind = stm::Notification::Kind::JobDone;
    n.seq = 7;
    n.text = L"完成";
    nq.Push(n);
    std::vector<stm::Notification> out;
    nq.Drain(&out);
    const bool firstOk = (out.size() == 1 && out[0].seq == 7 && out[0].text == L"完成");
    out.clear();
    nq.Drain(&out);  // 第二次取空必须为空
    if (!firstOk || !out.empty()) {
        *err = L"通知队列行为不符合预期";
        return false;
    }
    return true;
}

// ---- core：快照仓库 ----
STM_TEST(core_snapshot_store) {
    stm::SnapshotStore store;
    auto first = store.Get();
    if (!first) { *err = L"构造后 Get() 为空"; return false; }  // never-null contract
    auto mut = std::make_shared<stm::Snapshot>();
    stm::ProcInfo p;
    p.key.pid = 123;
    p.name = L"测试进程";
    mut->procs.push_back(p);
    mut->tickId = 42;
    store.Set(mut);
    auto got = store.Get();
    if (got->tickId != 42 || got->procs.size() != 1 || got->procs[0].name != L"测试进程") {
        *err = L"快照存取不一致";
        return false;
    }
    return true;
}

// ---- core：保护名单 ----
STM_TEST(core_protected_list) {
    if (stm::ProtectedReason(0, L"System Idle Process", L"").empty()) { *err = L"PID 0 未被保护"; return false; }
    if (stm::ProtectedReason(4, L"System", L"").empty()) { *err = L"PID 4 未被保护"; return false; }
    if (stm::ProtectedReason(777, L"csrss.EXE", L"C:\\Windows\\system32\\csrss.exe").empty()) {
        *err = L"csrss 未被保护（大小写不敏感匹配失败）";
        return false;
    }
    if (!stm::ProtectedReason(777, L"explorer.exe", L"C:\\Windows\\explorer.exe").empty()) {
        *err = L"explorer 不应在保护名单";
        return false;
    }
    return true;
}

// ---- 入口（普通 main + 宽命令行解析）----
int main(int argc, char** argv) {
    bool json = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--json") == 0) json = true;
    }

    int pass = 0, fail = 0;
    std::vector<std::pair<std::string, bool>> results;
    for (const auto& t : stmtest::Registry()) {
        std::wstring err;
        const bool ok = t.fn(&err);
        ok ? ++pass : ++fail;
        results.emplace_back(t.name, ok);
        if (!json) {
            printf("[%s] %s%s\n", ok ? "PASS" : "FAIL", t.name,
                   err.empty() ? "" : stm::WideToUtf8(L"  -> " + err).c_str());
        }
    }
    if (json) {
        printf("{\"pass\":%d,\"fail\":%d,\"cases\":[", pass, fail);
        for (size_t i = 0; i < results.size(); ++i) {
            printf("%s{\"name\":\"%s\",\"pass\":%s}", i ? "," : "",
                   results[i].first.c_str(), results[i].second ? "true" : "false");
        }
        printf("]}\n");
    } else {
        printf("\n合计：%d 通过，%d 失败\n", pass, fail);
    }
    return fail;
}
