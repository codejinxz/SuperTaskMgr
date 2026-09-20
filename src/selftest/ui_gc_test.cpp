// F4 落地批（G-C）UI 纯逻辑自测：进程类别判定、进程树行序、优先级/亲和性映射、
// 崩溃记录文案、窗口标签、性能 CSV 表头/行、跨页服务过滤。全部 header-only 纯
// 函数，stm_selftest（core+collect+ops 链接）可直接覆盖，无 ImGui / 无 GUI。
#include "selftest/TestFramework.h"
#include "app/ui3/CrashUi.h"
#include "app/ui3/JumpState.h"
#include "app/ui3/PerfCsv.h"
#include "app/ui3/ProcControlUi.h"
#include "app/ui3/ProcKind.h"
#include "app/ui3/ProcTree.h"
#include "app/ui3/WindowUtil.h"
#include "core/ProcData.h"
#include "core/Str.h"
#include <windows.h>
#include <string>
#include <vector>

using namespace stm;
using namespace stm::ui3;

// ---------------------------------------------------------------------------
// F4#1 ui_prockind_classify：各分支 + 空 path 保守分支 + 中文路径。
// ---------------------------------------------------------------------------
STM_TEST(ui_prockind_classify) {
    const std::wstring root = L"C:\\WINDOWS";  // 大小写在比较时归一

    // Critical：保护名单（core::ProtectedReason 非空）优先于一切。
    if (ClassifyProc(777, L"csrss.exe", L"C:\\Windows\\System32\\csrss.exe", 0, root) !=
        ProcKind::Critical) {
        *err = L"csrss 应判为 Critical";
        return false;
    }
    if (ClassifyProc(0, L"System Idle Process", L"", 0, root) != ProcKind::Critical) {
        *err = L"PID 0 应判为 Critical（保护名单含 pid 0/4）";
        return false;
    }
    // ServiceHost：PF_ServiceHost 优先于 Windows（svchost 路径在 SystemRoot 下）。
    if (ClassifyProc(1000, L"svchost.exe", L"C:\\Windows\\System32\\svchost.exe",
                     PF_ServiceHost, root) != ProcKind::ServiceHost) {
        *err = L"PF_ServiceHost 应判为 ServiceHost（优先于 Windows）";
        return false;
    }
    // Uwp：PF_Uwp 保留为用户应用。
    if (ClassifyProc(1001, L"app.exe",
                     L"C:\\Program Files\\WindowsApps\\pkg\\app.exe", PF_Uwp, root) !=
        ProcKind::Uwp) {
        *err = L"PF_Uwp 应判为 Uwp";
        return false;
    }
    // Windows：%SystemRoot% 之下（大小写不敏感、'/' 与 '\\' 等价）。
    if (ClassifyProc(1002, L"notepad.exe", L"c:\\windows\\system32\\notepad.exe", 0,
                     root) != ProcKind::Windows) {
        *err = L"SystemRoot 下的路径应判为 Windows（小写）";
        return false;
    }
    if (ClassifyProc(1003, L"fontdrvhost.exe", L"C:/Windows/fontdrvhost.exe", 0, root) !=
        ProcKind::Windows) {
        *err = L"正斜杠路径应判为 Windows";
        return false;
    }
    // 近似前缀不得误判："C:\Windowsa" 不是 "C:\Windows" 之下。
    if (ClassifyProc(1004, L"evil.exe", L"C:\\Windowsa\\evil.exe", 0, root) !=
        ProcKind::User) {
        *err = L"近似前缀 C:\\Windowsa 不应判为 Windows";
        return false;
    }
    // 空 path 保守名表分支：知名系统名（且不在保护名单）-> Windows；未知名 -> User。
    // （lsass 等保护名单成员先判为 Critical，轮不到名表 —— 优先级正确）
    if (ClassifyProc(1005, L"taskhostw.exe", L"", 0, root) != ProcKind::Windows) {
        *err = L"无路径的 taskhostw 应按保守名表判为 Windows";
        return false;
    }
    if (ClassifyProc(1005, L"svchost.exe", L"", 0, root) != ProcKind::Windows) {
        *err = L"无路径且无徽标的 svchost 应按保守名表判为 Windows";
        return false;
    }
    if (ClassifyProc(1006, L"chrome.exe", L"", 0, root) != ProcKind::User) {
        *err = L"无路径的未知名不应判为 Windows（宁漏勿错）";
        return false;
    }
    // 中文路径：SystemRoot 下的中文目录仍是 Windows；中文目录但不在根下是 User。
    if (ClassifyProc(1007, L"工具.exe", L"C:\\Windows\\中文目录\\工具.exe", 0, root) !=
        ProcKind::Windows) {
        *err = L"SystemRoot 下中文子目录应判为 Windows";
        return false;
    }
    if (ClassifyProc(1008, L"工具.exe", L"D:\\中文目录\\工具.exe", 0, root) !=
        ProcKind::User) {
        *err = L"非 SystemRoot 的中文路径应判为 User";
        return false;
    }
    // Unknown：名称与路径皆空且无徽标。
    if (ClassifyProc(1009, L"", L"", 0, root) != ProcKind::Unknown) {
        *err = L"无名无路径无徽标应判为 Unknown";
        return false;
    }
    // 只看用户进程过滤：Critical/Windows/ServiceHost 隐藏，Uwp/User 保留。
    if (ProcKindVisibleInUserFilter(ProcKind::Critical) ||
        ProcKindVisibleInUserFilter(ProcKind::Windows) ||
        ProcKindVisibleInUserFilter(ProcKind::ServiceHost)) {
        *err = L"只看用户进程应隐藏 Critical/Windows/ServiceHost";
        return false;
    }
    if (!ProcKindVisibleInUserFilter(ProcKind::Uwp) ||
        !ProcKindVisibleInUserFilter(ProcKind::User)) {
        *err = L"Uwp/User 应在只看用户进程中保留";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// F4#4 ui_proctree_order：多级树 / 孤儿 / 复用校验 / 兄弟排序 / 环兜底。
// ---------------------------------------------------------------------------
STM_TEST(ui_proctree_order) {
    std::vector<ProcInfo> procs(8);
    // 0：近似空闲的根（pid 1）
    procs[0].key = ProcKey{1, 100};
    // 1..3: 中间层父子链 2 -> 3 -> 4
    procs[1].key = ProcKey{2, 200};  // 根（无父）
    procs[2].key = ProcKey{3, 300};
    procs[2].parentPid = 2;
    procs[3].key = ProcKey{4, 400};
    procs[3].parentPid = 3;
    // 4: 孤儿（父 999 不在快照）
    procs[4].key = ProcKey{5, 500};
    procs[4].parentPid = 999;
    // 5: 父 createTime 异常（声称的父 pid4 createTime=400 晚于子 250：判定为
    //    PID 复用，链接无效 -> 提升为根）
    procs[5].key = ProcKey{6, 250};
    procs[5].parentPid = 4;
    // 6/7: pid3 的双子，验证兄弟按给定序排（7 先于 6）
    procs[6].key = ProcKey{7, 310};
    procs[6].parentPid = 3;
    procs[7].key = ProcKey{8, 320};
    procs[7].parentPid = 3;

    std::vector<TreeRow> rows;
    // 兄弟/根按 "下标降序" 的怪序比较器，检验排序真的生效。
    BuildTreeOrder(procs, [](int a, int b) { return a > b; }, &rows);

    if (rows.size() != procs.size()) {
        *err = L"树序应恰好输出每行一次";
        return false;
    }
    auto depthOf = [&](uint32_t pid) -> int {
        for (const TreeRow& r : rows) {
            if (procs[static_cast<size_t>(r.index)].key.pid == pid) return r.depth;
        }
        return -1;
    };
    auto posOf = [&](uint32_t pid) -> int {
        for (size_t i = 0; i < rows.size(); ++i) {
            if (procs[static_cast<size_t>(rows[i].index)].key.pid == pid) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    // 层级：2 根 / 3 深一 / 4、7、8 深二 / 5（孤儿）与 6（复用提升）为根。
    if (depthOf(2) != 0 || depthOf(3) != 1 || depthOf(4) != 2) {
        *err = L"父子链 2->3->4 层级错误";
        return false;
    }
    if (depthOf(5) != 0) { *err = L"孤儿（父不在快照）应提升为根"; return false; }
    if (depthOf(6) != 0) {
        *err = L"父 createTime 异常（复用）应提升为根";
        return false;
    }
    if (depthOf(7) != 2 || depthOf(8) != 2) {
        *err = L"pid3 的子进程层级错误";
        return false;
    }
    // 兄弟排序：比较器为下标降序 => pid8（下标 7）先于 pid7（下标 6）。
    if (!(posOf(8) < posOf(7))) {
        *err = L"兄弟应按给定比较器排序";
        return false;
    }
    // DFS 连续性：父必须先于子出现。
    if (!(posOf(3) < posOf(4) && posOf(3) < posOf(7) && posOf(3) < posOf(8))) {
        *err = L"DFS 应保证父先于子";
        return false;
    }

    // 环兜底：A 父 B、B 父 A（createTime 相同使两条链接都"合法"，构成真环）——
    // 两行都必须输出且提升为根，绝不丢行、绝不死循环。
    std::vector<ProcInfo> cyc(3);
    cyc[0].key = ProcKey{10, 1000};
    cyc[0].parentPid = 11;  // A -> B（成环）
    cyc[1].key = ProcKey{11, 1000};
    cyc[1].parentPid = 10;  // B -> A（环）
    cyc[2].key = ProcKey{12, 1002};
    std::vector<TreeRow> crows;
    BuildTreeOrder(cyc, [](int a, int b) { return a < b; }, &crows);
    if (crows.size() != 3) {
        *err = L"环上节点不得丢行";
        return false;
    }
    if (crows[0].depth != 0 || crows[1].depth != 0) {
        *err = L"环上节点应提升为根";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// F4#2 ui_procctrl_labels：优先级映射与亲和性掩码换算。
// ---------------------------------------------------------------------------
STM_TEST(ui_procctrl_labels) {
    if (PriorityLabel(ops::ProcPriority::Idle) != L"空闲" ||
        PriorityLabel(ops::ProcPriority::Normal) != L"标准" ||
        PriorityLabel(ops::ProcPriority::Realtime) != L"实时") {
        *err = L"优先级中文文案错误";
        return false;
    }
    ops::ProcPriority p = ops::ProcPriority::Idle;
    if (!PriorityFromWin32(0x00000080, &p) || p != ops::ProcPriority::High) {
        *err = L"HIGH_PRIORITY_CLASS 应映射为 高";
        return false;
    }
    if (!PriorityFromWin32(0x00000100, &p) || p != ops::ProcPriority::Realtime) {
        *err = L"REALTIME_PRIORITY_CLASS 应映射为 实时";
        return false;
    }
    if (PriorityFromWin32(0x00000000, &p)) {
        *err = L"未知优先级类应返回 false（诚实 未知）";
        return false;
    }
    // 亲和性：位数统计 + CPU 列表 + 摘要。
    const uint64_t mask = (1ull << 0) | (1ull << 2) | (1ull << 5);
    if (AffinityCpuCount(mask) != 3 || AffinityCpuCount(0) != 0) {
        *err = L"亲和性位数统计错误";
        return false;
    }
    const std::vector<int> cpus = AffinityCpuList(mask);
    if (cpus.size() != 3 || cpus[0] != 0 || cpus[1] != 2 || cpus[2] != 5) {
        *err = L"亲和性 CPU 列表错误";
        return false;
    }
    if (AffinitySummary(mask) != L"3 个（CPU 0,2,5）") {
        *err = L"亲和性摘要错误：" + AffinitySummary(mask);
        return false;
    }
    const uint64_t full = 0xFF;  // 连续 0..7 => “全部”
    if (AffinitySummary(full) != L"全部 8 个逻辑核") {
        *err = L"全选掩码摘要错误：" + AffinitySummary(full);
        return false;
    }
    if (AffinitySummary(0) != L"—") {
        *err = L"0 掩码应显示 —（未知）";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// F4#5 ui_crash_labels：级别/事件 ID 文案 + 本地时间定宽格式。
// ---------------------------------------------------------------------------
STM_TEST(ui_crash_labels) {
    if (CrashLevelLabel(2) != L"错误" || CrashLevelLabel(3) != L"警告" ||
        CrashLevelLabel(1) != L"严重" || CrashLevelLabel(4) != L"信息" ||
        CrashLevelLabel(0) != L"未知") {
        *err = L"崩溃级别中文文案错误";
        return false;
    }
    if (CrashEventIdLabel(1000) != L"应用错误" || CrashEventIdLabel(1002) != L"应用挂起" ||
        CrashEventIdLabel(1001) != L"错误报告") {
        *err = L"事件 ID 文案错误";
        return false;
    }
    const std::wstring t = FormatUnixTimeLocal(0);  // <=0 -> —（未知）
    if (t != L"—") { *err = L"非法时间戳应显示 —"; return false; }
    const std::wstring ts = FormatUnixTimeLocal(1758153600);  // 2025-09-18 前后（本地时区）
    if (ts.size() != 19 || ts[4] != L'-' || ts[7] != L'-' || ts[10] != L' ' ||
        ts[13] != L':' || ts[16] != L':') {
        *err = L"时间格式应为 yyyy-MM-dd HH:mm:ss：" + ts;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// F4 次梯队 ui_windowutil_labels：窗口状态/尺寸标签（纯函数）。
// ---------------------------------------------------------------------------
STM_TEST(ui_windowutil_labels) {
    WindowEntry w;
    w.topmost = false;
    if (WindowStatusLabel(w) != L"正常") { *err = L"普通窗口状态应为 正常"; return false; }
    w.iconic = true;
    if (WindowStatusLabel(w) != L"最小化") { *err = L"最小化状态文案错误"; return false; }
    w.iconic = false;
    w.zoomed = true;
    if (WindowStatusLabel(w) != L"最大化") { *err = L"最大化状态文案错误"; return false; }
    w.topmost = true;
    if (WindowStatusLabel(w) != L"最大化（已置顶）") {
        *err = L"置顶后缀文案错误：" + WindowStatusLabel(w);
        return false;
    }
    WindowEntry s;
    if (WindowSizeLabel(s) != L"—") { *err = L"非法尺寸应显示 —"; return false; }
    s.width = 1280;
    s.height = 800;
    if (WindowSizeLabel(s) != L"1280×800") {
        *err = L"尺寸文案错误：" + WindowSizeLabel(s);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// F4#7 csv_header_roundtrip：表头/行列数一致（含每核列）。
// ---------------------------------------------------------------------------
STM_TEST(csv_header_roundtrip) {
    for (const size_t cores : {size_t{0}, size_t{1}, size_t{4}, size_t{24}}) {
        const std::wstring header = PerfCsvHeader(cores);
        // 列 = 时间 + CPU% + cores + 内存可用/提交 + 磁盘读/写 + 网络收/发 + GPU
        //     + 磁盘队列（Phase C）
        // => 10 + cores 列，逗号数 = 9 + cores
        size_t commas = 0;
        for (wchar_t ch : header) commas += ch == L',' ? 1 : 0;
        if (commas != 9 + cores) {
            *err = L"表头列数错误（cores=" + std::to_wstring(cores) + L"）：" + header;
            return false;
        }
        // 行与表头列数严格一致
        Snapshot snap;
        snap.sys.perCorePercent.assign(cores, 12.5);
        snap.sys.cpuTotalPercent = 33.3;
        snap.sys.physAvail = 1;
        snap.sys.commitTotal = 2;
        snap.sys.diskReadBps = kUnavail;   // NaN -> 空单元格（列仍在）
        snap.sys.diskQueueDepth = kUnavail;  // NaN -> 空单元格（Phase C）
        snap.sys.gpus.push_back(GpuAdapterInfo{});
        snap.sys.gpus[0].utilPercent = 7.0;
        const std::wstring row = PerfCsvRow(snap, L"2026-09-18 12:00:00");
        size_t rowCommas = 0;
        for (wchar_t ch : row) rowCommas += ch == L',' ? 1 : 0;
        if (rowCommas != commas) {
            *err = L"行列数与表头不一致（cores=" + std::to_wstring(cores) + L"）：" + row;
            return false;
        }
        if (cores >= 1 && row.find(L"12.50") == std::wstring::npos) {
            *err = L"每核数值缺失";
            return false;
        }
        if (cores >= 1 && row.find(L",,") == std::wstring::npos) {
            *err = L"NaN（磁盘速率）应写空单元格";
            return false;
        }
        if (row.find(L"7.00") == std::wstring::npos) {
            *err = L"GPU 利用率缺失";
            return false;
        }
        // Phase C：磁盘队列合法读数与表头新列名都必须在。
        if (header.find(L"磁盘队列") == std::wstring::npos) {
            *err = L"表头缺「磁盘队列」列";
            return false;
        }
        snap.sys.diskQueueDepth = 4.0;
        const std::wstring row2 = PerfCsvRow(snap, L"2026-09-18 12:00:01");
        if (row2.find(L",4") == std::wstring::npos ||
            row2.find(L",4") != row2.size() - 2) {
            *err = L"磁盘队列读数应为末列 4：" + row2;
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// F4#3 ui_jump_filter：按 PID 过滤服务（宿主联动数据通路）。
// ---------------------------------------------------------------------------
STM_TEST(ui_jump_filter) {
    std::vector<ops::ServiceInfo> svcs(3);
    svcs[0].name = L"a";
    svcs[0].pid = 42;
    svcs[1].name = L"b";
    svcs[1].pid = 7;
    svcs[2].name = L"c";
    svcs[2].pid = 42;
    const std::vector<ops::ServiceInfo> mine = FilterServicesByPid(svcs, 42);
    if (mine.size() != 2 || mine[0].name != L"a" || mine[1].name != L"c") {
        *err = L"按 PID 过滤服务结果错误";
        return false;
    }
    if (!FilterServicesByPid(svcs, 1234).empty()) {
        *err = L"无匹配 PID 时应返回空表";
        return false;
    }
    // 跳转槽：请求 -> 消费 -> 清零（幂等消费）。
    uint32_t pid = 0;
    if (ConsumeProcessJump(&pid)) { *err = L"空槽不应可消费"; return false; }
    RequestJumpToProcess(42);
    RequestJumpToProcess(0);  // pid 0 忽略
    if (!ConsumeProcessJump(&pid) || pid != 42) {
        *err = L"跳转请求未被正确消费";
        return false;
    }
    if (ConsumeProcessJump(&pid)) { *err = L"消费后槽位未清零"; return false; }
    return true;
}
