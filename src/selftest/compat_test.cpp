// 兼容模式诊断自测（维护轮 10）。
// 覆盖：自检逐项报告的契约（6 项齐全、name/detail 非空、结论一致），
// 以及 CompatDiag.h 的纯函数（报告文本 + 落盘）。
//
// 局限说明（可测性）：自检门直接消费真实系统的 NtQSI/PSAPI/PDH 读数，
// selftest 没有可供注入伪造返回值的接缝（不引入钩子层属于设计取舍），
// 因此"失败自动重试至多 2 次 / 连续 2 轮全过才判通过"的加固分支无法在
// 单测里人为触发超差——只能在真机上验证其"应通过而通过"的一面与请求/
// 消费协议的不变量（重跑后报告仍然完整、重试标志被采集线程消费）。
// 超差路径的人工验证方式：日志模块 "selfcheck" 逐项打印了判定与重试。
#include "selftest/TestFramework.h"
#include "app/ui3/CompatDiag.h"
#include "collect/CollectService.h"
#include "core/ProcData.h"
#include "core/Str.h"
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using stm::CollectService;

// 轮询仓库直到 tickId >= n（与 collect_test 相同的等待协议）。
bool WaitForTicks(CollectService& svc, uint64_t n, int timeoutMs) {
    for (int waited = 0; waited < timeoutMs; waited += 50) {
        ::Sleep(50);
        if (svc.Store().Get()->tickId >= n) return true;
    }
    return false;
}

}  // namespace

// --- 自检逐项报告：6 项齐全、name/detail 非空、结论与降级状态一致 ---------
STM_TEST(selfcheck_report_items) {
    CollectService svc;
    if (!svc.Start(500)) {
        *err = L"CollectService::Start 失败";
        return false;
    }
    if (!WaitForTicks(svc, 2, 10000)) {
        *err = L"10 秒内未产出 2 个快照（自检门在第一个 tick 运行）";
        return false;
    }
    const std::vector<CollectService::SelfCheckItem> items = svc.LastSelfCheckReport();
    // V24 P1-1：门真跑完一次（含报告落库）后代际必须为正。
    const uint64_t gen1 = svc.LastSelfCheckGeneration();
    if (gen1 == 0) {
        *err = L"自检门已运行但代际仍为 0";
        return false;
    }
    if (items.empty()) {
        *err = L"自检报告为空（第一个 tick 后必须有逐项结果）";
        return false;
    }
    if (items.size() != 6) {
        *err = stm::Fmt(L"自检报告应有 6 项，实际 {} 项", items.size());
        return false;
    }
    for (size_t i = 0; i < items.size(); ++i) {
        const CollectService::SelfCheckItem& it = items[i];
        if (it.name == nullptr || *it.name == L'\0') {
            *err = stm::Fmt(L"第 {} 项 name 为空", i);
            return false;
        }
        if (it.detail.empty()) {
            *err = stm::Fmt(L"第 {} 项（{}）detail 为空（应给出测量对比或跳过原因）", i,
                            it.name);
            return false;
        }
        // 本实现的约定：未运行的项不得宣称通过（诚实跳过）。
        if (!it.ran && it.passed) {
            *err = stm::Fmt(L"项「{}」未运行却标记通过", it.name);
            return false;
        }
    }
    for (size_t i = 0; i < items.size(); ++i) {
        for (size_t j = i + 1; j < items.size(); ++j) {
            if (std::wstring(items[i].name) == std::wstring(items[j].name)) {
                *err = stm::Fmt(L"检查项名重复：{}", items[i].name);
                return false;
            }
        }
    }
    // 一致性：报告里存在失败的已运行项 ⇔ 快照必须处于降级状态。
    bool anyFailed = false;
    for (const CollectService::SelfCheckItem& it : items) {
        if (it.ran && !it.passed) anyFailed = true;
    }
    const bool snapDegraded = svc.Store().Get()->degraded;
    if (anyFailed && !snapDegraded) {
        *err = L"报告存在失败项但快照未降级（结论不一致）";
        return false;
    }
    if (snapDegraded && svc.Store().Get()->degradeReason.empty()) {
        *err = L"降级快照缺少降级原因";
        return false;
    }

    // 重试协议：请求重跑后，下一个 tick 消费标志并重新产出完整报告。
    svc.RequestSelfCheckRetry();
    if (!WaitForTicks(svc, svc.Store().Get()->tickId + 1, 10000)) {
        *err = L"重新自检请求后 10 秒内未见新 tick（标志未被消费）";
        return false;
    }
    const std::vector<CollectService::SelfCheckItem> items2 = svc.LastSelfCheckReport();
    // V24 P1-1：一次重跑请求 → 门恰好再跑一次 → 代际恰好 +1。
    if (svc.LastSelfCheckGeneration() != gen1 + 1) {
        *err = L"一次重跑后代际应恰好 +1（实得 " +
               std::to_wstring(svc.LastSelfCheckGeneration()) + L"）";
        return false;
    }
    if (items2.size() != 6) {
        *err = stm::Fmt(L"重跑后自检报告应有 6 项，实际 {} 项", items2.size());
        return false;
    }
    for (const CollectService::SelfCheckItem& it : items2) {
        if (it.name == nullptr || *it.name == L'\0' || it.detail.empty()) {
            *err = L"重跑后的报告存在空 name/detail";
            return false;
        }
    }
    svc.Stop();
    return true;
}

// --- 诊断报告纯文本：含系统版本 + 应用版本 + 逐项关键词；落盘可读回 ---------
STM_TEST(compat_diag_report_text) {
    using stm::CollectService;
    using stm::ui3::CompatDiagReportText;
    using stm::ui3::SaveDiagnosticsFile;
    using stm::ui3::SystemVersionLine;

    // 系统版本行：真实环境必得 Windows 前缀（RtlGetVersion 文档化读取）。
    const std::wstring sys = SystemVersionLine();
    if (sys.empty() || sys.find(L"Windows") == std::wstring::npos) {
        *err = L"SystemVersionLine 未包含 Windows 版本：" + sys;
        return false;
    }

    // 伪造一份逐项结果（覆盖 通过/失败/跳过 三种形态；不运行采集服务）。
    std::vector<CollectService::SelfCheckItem> items;
    items.push_back(CollectService::SelfCheckItem{
        L"CPU 时间字段", true, true,
        std::wstring(L"NtQSI CPU时间=3.20s vs GetProcessTimes=3.21s（容差±1s）")});
    items.push_back(CollectService::SelfCheckItem{
        L"线程数", true, false,
        std::wstring(L"PID 4321 线程数 NtQSI=4 vs Toolhelp=9（偏差>10%）")});
    items.push_back(CollectService::SelfCheckItem{L"私有工作集", false, false,
                                                  std::wstring(L"跳过：PDH 计数器不可用")});

    const std::wstring text = CompatDiagReportText(
        L"9.9.9-d1test", items, std::wstring(L"NtQSI 自校验未通过：测试降级原因"));
    struct Contains {
        bool operator()(const std::wstring& hay, const std::wstring& needle) const {
            return hay.find(needle) != std::wstring::npos;
        }
    } has;
    if (!has(text, L"9.9.9-d1test")) { *err = L"报告缺少应用版本"; return false; }
    if (!has(text, sys)) { *err = L"报告缺少系统版本行"; return false; }
    if (!has(text, L"NtQSI 自校验未通过：测试降级原因")) { *err = L"报告缺少降级原因"; return false; }
    if (!has(text, L"CPU 时间字段") || !has(text, L"NtQSI CPU时间=3.20s")) {
        *err = L"报告缺少通过项的名称/测量对比";
        return false;
    }
    if (!has(text, L"线程数") || !has(text, L"PID 4321 线程数 NtQSI=4")) {
        *err = L"报告缺少失败项的名称/测量对比";
        return false;
    }
    if (!has(text, L"私有工作集") || !has(text, L"跳过：PDH 计数器不可用")) {
        *err = L"报告缺少跳过项的名称/原因";
        return false;
    }
    if (!has(text, L"不会自动上传")) { *err = L"报告缺少隐私说明（不自动上传）"; return false; }
    // V24 P2-2：措辞必须与加固实现一致（不是"任一项超差即降级"）。
    if (!has(text, L"连续两轮全部通过")) {
        *err = L"报告缺少数代际措辞（重试后连续两轮未通过才降级）";
        return false;
    }
    if (has(text, L"任一项超差即整体降级")) {
        *err = L"报告仍是旧措辞「任一项超差即整体降级」";
        return false;
    }

    // 空报告（尚未自检）：不崩溃、有占位、仍带版本信息。
    const std::wstring emptyText =
        CompatDiagReportText(L"9.9.9-d1test", {}, std::wstring());
    if (!has(emptyText, L"尚未自检") || !has(emptyText, L"9.9.9-d1test") ||
        !has(emptyText, sys)) {
        *err = L"空报告应包含「尚未自检」占位与版本信息";
        return false;
    }

    // 落盘：返回路径真实存在、非空；同秒连存两次路径必不同（V24 P2-4 序号
    // 后缀防覆盖）；测后清理。
    const std::wstring path = SaveDiagnosticsFile(text);
    if (path.empty()) {
        *err = L"SaveDiagnosticsFile 返回空路径（目录创建或写入失败）";
        return false;
    }
    const std::wstring path2 = SaveDiagnosticsFile(emptyText);
    if (path2.empty() || path2 == path) {
        *err = L"同秒两次导出应得到不同路径（序号后缀防覆盖）";
        return false;
    }
    if (::GetFileAttributesW(path2.c_str()) == INVALID_FILE_ATTRIBUTES) {
        *err = L"第二次导出的诊断文件未落盘：" + path2;
        return false;
    }
    ::DeleteFileW(path2.c_str());
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        *err = L"诊断文件未落盘：" + path;
        return false;
    }
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *err = L"诊断文件无法读取：" + path;
        return false;
    }
    char buf[4096] = {};
    DWORD read = 0;
    ::ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
    ::CloseHandle(h);
    if (read < 10) {
        *err = L"诊断文件内容为空";
        return false;
    }
    ::DeleteFileW(path.c_str());
    return true;
}
