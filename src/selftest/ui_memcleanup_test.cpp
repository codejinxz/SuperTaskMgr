// P3 任务一：一键内存优化纯逻辑测试 —— TopN 排序、保护/系统进程排除、
// 默认勾选、结果聚合与 toast 文案。全部 header-only（app/ui3/MemCleanup.h），
// 无 GUI、无 ImGui 依赖。
#include "selftest/TestFramework.h"
#include "app/ui3/MemCleanup.h"
#include "core/ProcData.h"
#include <windows.h>
#include <string>
#include <vector>

using namespace stm;
using namespace stm::ui3;

namespace {

constexpr uint64_t kMiB = 1024ull * 1024ull;

ProcInfo MakeProc(uint32_t pid, const wchar_t* name, const wchar_t* path, uint32_t flags,
                  uint64_t privateWorkingSet) {
    ProcInfo p;
    p.key.pid = pid;
    p.key.createTime = static_cast<uint64_t>(pid) * 1000ull;
    p.name = name;
    p.path = path;
    p.flags = flags;
    p.privateWorkingSet = privateWorkingSet;
    return p;
}

// 覆盖全部类别：保护名单（csrss）、无路径知名系统进程（System）、服务宿主、
// %SystemRoot% 下的 Windows 进程、UWP、普通用户进程，以及一个私有工作集未知项。
Snapshot MakeSnap() {
    Snapshot s;
    s.tickId = 7;
    s.procs.push_back(MakeProc(4, L"System", L"", 0, kUnavailU64));
    s.procs.push_back(MakeProc(300, L"csrss.exe", L"C:\\Windows\\system32\\csrss.exe", 0,
                               4 * kMiB));
    s.procs.push_back(MakeProc(400, L"svchost.exe", L"C:\\Windows\\system32\\svchost.exe",
                               PF_ServiceHost, 30 * kMiB));
    s.procs.push_back(MakeProc(500, L"explorer.exe", L"C:\\Windows\\explorer.exe", 0,
                               120 * kMiB));
    s.procs.push_back(MakeProc(600, L"game.exe", L"D:\\games\\game.exe", 0, 800 * kMiB));
    s.procs.push_back(MakeProc(700, L"chrome.exe", L"C:\\Program Files\\chrome.exe", 0,
                               500 * kMiB));
    s.procs.push_back(MakeProc(800, L"app.exe", L"C:\\Program Files\\WindowsApps\\app\\app.exe",
                               PF_Uwp, 100 * kMiB));
    return s;
}

}  // namespace

// ui_memcleanup_select：TopN 排序 / 保护排除 / 结果聚合。
STM_TEST(ui_memcleanup_select) {
    const Snapshot snap = MakeSnap();

    // --- TopN 排序 + 保护排除 ---
    const std::vector<CleanupCandidate> all = SelectTopCleanupCandidates(snap, 10, L"C:\\Windows");
    if (all.size() != snap.procs.size()) {
        *err = L"TopN=10 时应返回全部候选";
        return false;
    }
    // 降序：800 -> 500 -> 120 -> 100 -> 30 -> 4 -> 未知（kUnavail 排最后）。
    const uint64_t expectSizes[] = {800 * kMiB, 500 * kMiB, 120 * kMiB, 100 * kMiB,
                                    30 * kMiB,  4 * kMiB,   kUnavailU64};
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i].privateWorkingSet != expectSizes[i]) {
            *err = L"按私有工作集降序排列失败（第 " + std::to_wstring(i) + L" 项）";
            return false;
        }
    }
    // 可选性（排序后下标）：0 game / 1 chrome / 3 app(UWP) 为用户进程应可选；
    // 2 explorer(Windows) / 4 svchost(服务宿主) / 5 csrss(保护名单) 不可选。
    if (!all[0].selectable || !all[1].selectable || !all[3].selectable) {
        *err = L"用户进程（含 UWP）应可选";
        return false;
    }
    if (all[2].selectable || all[4].selectable || all[5].selectable) {
        *err = L"系统进程/服务宿主/保护名单进程不应可选";
        return false;
    }
    if (all[2].kind != ProcKind::Windows || all[4].kind != ProcKind::ServiceHost ||
        all[5].kind != ProcKind::Critical) {
        *err = L"分类器结果与预期不符";
        return false;
    }
    if (all[0].name != L"game.exe" || all[1].name != L"chrome.exe") {
        *err = L"Top2 应为占用最高的两个用户进程";
        return false;
    }

    // --- TopN 截断 ---
    const std::vector<CleanupCandidate> top3 = SelectTopCleanupCandidates(snap, 3, L"C:\\Windows");
    if (top3.size() != 3 || top3[0].name != L"game.exe" || top3[1].name != L"chrome.exe" ||
        top3[2].name != L"explorer.exe") {
        *err = L"TopN=3 截断结果错误";
        return false;
    }

    // --- 默认勾选：可选条目按占用降序勾前 5 个（本例仅 3 个可选：0/1/3） ---
    const std::vector<bool> sel = DefaultCleanupSelection(all, 5);
    if (sel.size() != all.size()) { *err = L"默认勾选长度与候选不一致"; return false; }
    if (!sel[0] || !sel[1] || !sel[3]) { *err = L"占用最高的可选进程应默认勾选"; return false; }
    for (size_t i : {size_t(2), size_t(4), size_t(5), size_t(6)}) {
        if (sel[i]) { *err = L"不可选项被默认勾选"; return false; }
    }
    size_t pickedCount = 0;
    for (bool b : sel) {
        if (b) ++pickedCount;
    }
    if (pickedCount != 3) { *err = L"默认勾选个数错误"; return false; }
    if (AnySelected({false, false}) || !AnySelected({false, true, false})) {
        *err = L"AnySelected 判定错误";
        return false;
    }
    if (CountSelected({true, false, true}) != 2) { *err = L"CountSelected 计数错误"; return false; }
    if (SelectedBytes(all, sel) != 800 * kMiB + 500 * kMiB + 100 * kMiB) {
        *err = L"勾选字节合计错误";
        return false;
    }

    // --- 结果聚合与文案 ---
    CleanupOutcome o;
    RecordTrimResult(o, true, 800 * kMiB);
    RecordTrimResult(o, true, 500 * kMiB);
    RecordTrimResult(o, false, 100 * kMiB);
    RecordTrimResult(o, false, 0);
    if (o.attempted != 4 || o.failed != 2 || o.freedBytes != 1300 * kMiB) {
        *err = L"CleanupOutcome 聚合错误（attempted/failed/freedBytes）";
        return false;
    }
    const std::wstring text = FormatCleanupDoneText(o);
    if (text.find(L"已处理 4 个进程") == std::wstring::npos ||
        text.find(L"释放约 1300.0 MiB 工作集") == std::wstring::npos ||
        text.find(L"（失败 2 个）") == std::wstring::npos) {
        *err = L"失败场景 toast 文案错误：" + text;
        return false;
    }
    CleanupOutcome ok0;
    RecordTrimResult(ok0, true, 1536 * kMiB);
    const std::wstring okText = FormatCleanupDoneText(ok0);
    if (okText.find(L"失败") != std::wstring::npos ||
        okText.find(L"释放约 1536.0 MiB") == std::wstring::npos) {
        *err = L"零失败文案不应包含失败子句：" + okText;
        return false;
    }
    return true;
}
