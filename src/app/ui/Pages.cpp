// UI SHELL + PAGES: process table, performance charts, detail panel, confirm dialogs,
// toasts and status bar (docs/phase/01_架构设计文档.md sections 8/11).
// All user-facing text is Chinese; it goes through ui::U8() because ImGui is a
// narrow-char (UTF-8) API. The UI thread reads the snapshot exactly once per frame.
#include "app/ui/Pages.h"
#include "app/AppContext.h"
#include "app/Theme.h"            // H-A: 主题三态 + 模式感知强调色
#include "app/ui/AboutUi.h"       // H-A: 工具条「?」关于按钮 + 模态
#include "app/ui/ConfirmAction.h"
#include "app/ui/ModulesUi.h"
#include "app/ui/SortKey.h"
#include "app/ui/UiText.h"
#include "app/ui/VersionInfo.h"
#include "app/ui3/GcPages.h"      // F4: 崩溃记录/窗口页注册 + 宿主服务模态 + 热键
#include "app/ui3/JumpState.h"    // F4#3: 跨页跳转槽
#include "app/ui3/MemCleanup.h"   // P3 任务一: 一键内存优化候选/聚合（纯逻辑）
#include "app/ui3/Pages3.h"       // phase-3 extension tabs + shell hooks (additive)
#include "app/ui3/PerfCsv.h"      // F4#7: 性能 CSV 记录
#include "app/ui3/ProcControlUi.h"  // F4#2: 优先级/亲和性文案与掩码换算
#include "app/ui3/ProcKind.h"     // F4#1: 系统进程分类
#include "app/ui3/ProcTree.h"     // F4#4: 进程树行序
#include "app/ui3/ThemeCfg.h"     // H-A: colW_* 键清单登记 + 配置文件剔除
#include "app/ui3/Wallpaper.h"    // Phase-6: 自定义壁纸（Load/Clear/状态/提示）
#include "core/FsUtil.h"          // H-A: 外观菜单「恢复默认列宽」需要 ConfigPath()
#include "core/ProcData.h"
#include "core/ProtectedList.h"
#include "core/Str.h"
#include "ops/DetailsProvider.h"
#include "ops/Elevate.h"
#include "ops/ProcessControl.h"
#include "ops/ProcessOps.h"
#include "imgui.h"
#include "imgui_internal.h"  // TableSetColumnSortDirection + per-column width readback
#include "implot.h"
#include <algorithm>
#include <atomic>
#include <commdlg.h>  // H-A: 外观菜单「选择图片…」GetOpenFileNameW（comdlg32 已链接）
#include <cstdint>
#include <cwctype>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <shellapi.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace stm {

namespace {

using ui::U8;  // UTF-8 text cache (app/ui/UiText.h); used at every ImGui call site

// ===========================================================================
// Shared per-frame UI state (single main window, single UI thread).
// ===========================================================================

struct Toast {
    Notification::Kind kind = Notification::Kind::Info;
    std::wstring text;
    double expireTime = 0.0;  // ImGui::GetTime() based
};

// ConfirmKind/ConfirmRequest moved to app/ui/ConfirmAction.h (bug F1 fix): the
// confirm-action execution is shared with the phase-3 startup dialog and is
// unit-tested by stm_selftest without a GUI.

struct UiState {
    std::shared_ptr<const Snapshot> snap;  // one Store().Get() per frame (shell owns it)
    // Owning handle registered by main (BindAppContext). Job lambdas capture it BY
    // VALUE so an in-flight job outlives main's teardown: JobQueue::Shutdown detaches
    // a timed-out worker, and this capture keeps notes/jobs/details alive until the
    // job finishes, whichever thread drops the last reference.
    std::shared_ptr<AppContext> liveCtx;
    std::deque<Toast> toasts;
    std::wstring lastNote;                  // survives toast expiry for the status bar
    ui::ConfirmRequest confirm;
    bool confirmOpenRequested = false;      // set once by the menu handler: open the modal
    bool confirmOpened = false;             // modal actually reached the open state
    bool tabSelectArmed = true;             // P1-1: one-shot SetSelected on session restore
    bool paused = false;
    // Selection bookkeeping the session handoff reads (updated by ProcessesPage).
    ProcKey selectedKey;
    bool selectedValid = false;
    ui::SortColumn sortColumn = ui::SortColumn::Name;
    bool sortDesc = false;
    // F4#3: 跨页跳转（网络/服务页 -> 进程页）。DrawShell 消费槽位后暂存于此，
    // 进程页下一帧解析；找不到时 toast "进程已退出"。
    uint32_t jumpPendingPid = 0;
    // H-A: 「恢复默认列宽」世代号。递增后：进程页把 widths_ 重置回默认并把表格
    // id 换代（ImGui 内部按 id 记忆列宽，换代才能丢弃旧值、立即回默认宽度）。
    uint64_t colWidthResetGen = 0;
    // H-A: 壁纸同步加载进行中（外观菜单状态行提示；加载固定 UI 线程同步，见
    // PickAndLoadWallpaper 注释，几十 ms 阻塞期间菜单保持打开）。
    bool wallpaperBusy = false;
};

UiState& Ui() {
    static UiState s;
    return s;
}

void PushToast(Notification::Kind kind, const std::wstring& text) {
    auto& toasts = Ui().toasts;
    toasts.push_back({kind, text, ImGui::GetTime() + 5.0});  // auto-dismiss in 5 s
    while (toasts.size() > 6) toasts.pop_front();
    Ui().lastNote = text;
}

// ===========================================================================
// Confirm-action submission: all destructive ops go through ui::ExecuteConfirmed-
// Action (app/ui/ConfirmAction.h) on the ops worker queue; results come back as
// notifications -> toasts. See ConfirmAction.h for the lifetime/feedback contract.
// ===========================================================================

void PostNote(NotificationQueue& notes, Notification::Kind kind, const std::wstring& text) {
    Notification n;
    n.kind = kind;
    n.text = text;
    notes.Push(n);
}

// Clears any pending confirm request (used by every dialog exit path).
void CloseConfirm() {
    Ui().confirm = ui::ConfirmRequest{};
    Ui().confirmOpenRequested = false;
    Ui().confirmOpened = false;
}

void RequestTreePlan(const ProcInfo& p) {
    CloseConfirm();
    auto& req = Ui().confirm;
    req.kind = ui::ConfirmKind::KillTree;
    req.key = p.key;
    req.pid = p.key.pid;
    req.name = p.name;
    req.path = p.path;
    req.serviceHost = (p.flags & PF_ServiceHost) != 0;
    req.planCount = std::make_shared<std::atomic<int>>(-1);
    // P2 (V14): the tree dialog opens immediately with a "正在规划进程树…" line.
    Ui().confirmOpenRequested = true;
    std::shared_ptr<AppContext> app = Ui().liveCtx;
    if (!app) {
        // No live context (teardown): no notes queue exists to inform — reset only.
        CloseConfirm();
        return;
    }
    const bool elevated = app->elevated;
    std::shared_ptr<std::atomic<int>> plan = req.planCount;
    if (app->jobs.Submit([app, elevated, key = p.key, plan] {
            std::vector<ProcKey> members;
            std::wstring err;
            if (ops::PlanTerminateTree(key, &members, &err)) {
                plan->store(static_cast<int>(members.size()));
            } else {
                plan->store(-2);
                // P2 (V14): same admin-hint suffix as every other failure note.
                PostNote(app->notes, Notification::Kind::JobFailed,
                         Fmt(L"无法规划进程树：{}{}", err.empty() ? std::wstring(L"未知错误") : err,
                             ui::AdminHintSuffix(elevated)));
            }
        }) == 0) {
        // P2 (V14): refused submission must never be silent.
        PostNote(app->notes, Notification::Kind::JobFailed,
                 L"操作队列未运行，进程树规划未提交（应用可能正在退出）");
        CloseConfirm();
    }
}

// Confirm request builders (ProcKey/name/path locked here).
void RequestConfirmKill(const ProcInfo& p) {
    CloseConfirm();
    auto& req = Ui().confirm;
    req.kind = ui::ConfirmKind::Kill;
    req.key = p.key;
    req.pid = p.key.pid;
    req.name = p.name;
    req.path = p.path;
    req.serviceHost = (p.flags & PF_ServiceHost) != 0;
    Ui().confirmOpenRequested = true;
}

void RequestConfirmTrim(const ProcInfo& p) {
    CloseConfirm();
    auto& req = Ui().confirm;
    req.kind = ui::ConfirmKind::TrimWorkingSet;
    req.key = p.key;
    req.pid = p.key.pid;
    req.name = p.name;
    req.path = p.path;
    Ui().confirmOpenRequested = true;
}

void RequestConfirmPurgeStandby() {
    CloseConfirm();
    auto& req = Ui().confirm;
    req.kind = ui::ConfirmKind::PurgeStandby;
    Ui().confirmOpenRequested = true;
}

// P3 任务一：一键内存优化 —— 打开模态时冻结 Top10 候选快照（名称/私有工作集/
// 可选性），默认勾选前 5 个可选进程；确认后经 ExecuteConfirmedAction 走单个
// job 批量 TrimWorkingSet（ConfirmAction.h::MakeMemCleanupJob）。
void RequestConfirmMemCleanup() {
    CloseConfirm();
    if (!Ui().snap) return;  // 无快照（首帧前）：按钮无副作用
    auto& req = Ui().confirm;
    req.kind = ui::ConfirmKind::MemCleanup;
    req.cleanupItems = ui3::SelectTopCleanupCandidates(*Ui().snap, 10);
    req.cleanupSelected = ui3::DefaultCleanupSelection(req.cleanupItems, 5);
    req.cleanupPurgeStandby = false;
    Ui().confirmOpenRequested = true;
}

// ===========================================================================
// Small formatting / color helpers.
// ===========================================================================

// 模式感知的界面强调色（H-A）：经 Theme 转换后深/浅两套主题都可读。
ImVec4 AccentCol(ThemeAccent a) {
    return ImGui::ColorConvertU32ToFloat4(ThemeAccentColor(a));
}
ImVec4 ColDone() { return AccentCol(ThemeAccent::Done); }
ImVec4 ColFail() { return AccentCol(ThemeAccent::Fail); }
ImVec4 ColWarn() { return AccentCol(ThemeAccent::Warn); }
ImVec4 ColInfo() { return AccentCol(ThemeAccent::Info); }

// Module paths read best tail-first (file name at the end); ellipsize the front.
std::wstring TruncateModulePath(const std::wstring& s) {
    constexpr size_t kMax = 90;
    if (s.size() <= kMax) return s;
    return L"…" + s.substr(s.size() - (kMax - 1));
}

ImVec4 NoteColor(Notification::Kind k) {
    switch (k) {
        case Notification::Kind::JobDone: return ColDone();
        case Notification::Kind::JobFailed: return ColFail();
        case Notification::Kind::Warn: return ColWarn();
        default: return ColInfo();
    }
}

// ===========================================================================
// Toasts (top-right, auto-dismiss 2-5 s; JobFailed shown red).
// ===========================================================================

void DrawToasts() {
    const double now = ImGui::GetTime();
    auto& toasts = Ui().toasts;
    while (!toasts.empty() && toasts.front().expireTime < now) toasts.pop_front();
    if (toasts.empty()) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float width = 360.0f;
    float y = vp->WorkPos.y + 10.0f;
    for (const Toast& t : toasts) {
        char id[32];
        snprintf(id, sizeof(id), "##toast_%p", reinterpret_cast<const void*>(&t));
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 10.0f, y),
                                ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, FLT_MAX));
        ImGui::SetNextWindowBgAlpha(0.94f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_NoInputs;
        ImGui::PushStyleColor(ImGuiCol_Border, NoteColor(t.kind));
        ImGui::Begin(id, nullptr, flags);
        ImGui::TextColored(NoteColor(t.kind), "%s", U8(L"●"));
        ImGui::SameLine();
        ImGui::TextWrapped("%s", U8(t.text));
        ImGui::End();
        ImGui::PopStyleColor();
        y += ImGui::GetWindowHeight() + 6.0f;
    }
}

// ===========================================================================
// Confirm dialogs (modal, centered; two-stage destructive-op gate).
// ===========================================================================

// --autotest dialogclick observation point (V14): the driver asserts on this to
// regression-test the REAL rendered dialog (single-frame regression sentinel +
// action-button geometry for synthetic mouse injection).
DialogAutotestState g_dialogAutotest;

void DrawConfirmDialogs() {
    // P3 任务三：WM_CLOSE -> 三选一关闭询问。标志位由 main 的 onMessage 拦截置位
    // （绝不直接 DestroyWindow），这里每帧消费：与前两个破坏性确认完全相同的
    // 「请求长期有效 + 模态每帧渲染」模式（F1 修复），绝不是单帧模态。
    // 已有确认框在前时保持 pending，当前框关闭后的下一帧再弹。
    if (Ui().confirm.kind == ui::ConfirmKind::None && Ui().liveCtx &&
        Ui().liveCtx->closeAskPending.load()) {
        Ui().liveCtx->closeAskPending.store(false);
        Ui().confirm = ui::ConfirmRequest{};
        Ui().confirm.kind = ui::ConfirmKind::CloseAsk;
        Ui().confirm.rememberChoice = false;
        Ui().confirmOpenRequested = true;
    }

    ui::ConfirmRequest& req = Ui().confirm;
    if (req.kind == ui::ConfirmKind::None) {
        g_dialogAutotest = DialogAutotestState{};
        return;
    }
    // CloseAsk 不是破坏性 op，不参与 --autotest dialogclick 的观察哨兵。
    if (req.kind != ui::ConfirmKind::CloseAsk) g_dialogAutotest.requestActive = true;

    // Tree plan failure: a failure toast was already posted by the plan job.
    if (req.kind == ui::ConfirmKind::KillTree && req.planCount && req.planCount->load() == -2) {
        CloseConfirm();
        return;
    }

    // Bug F1 fix (2026-09): the old code returned early when confirmOpenRequested
    // was false, so BeginPopupModal() ran only on the single frame the request was
    // created. The modal was rendered for exactly one frame and could never receive
    // a click ("终止进程/终止进程树 没有任何效果"), and the abandoned ImGui popup
    // stayed in the popup stack as an invisible blocking modal. Now the modal is
    // BEGUN every frame while a request is active; OpenPopup is issued exactly once.
    if (Ui().confirmOpenRequested) {
        if (!ImGui::IsPopupOpen("##confirm")) {
            ImGui::OpenPopup("##confirm");
            Ui().confirmOpened = true;
        }
        Ui().confirmOpenRequested = false;
    }
    if (!Ui().confirmOpened) return;  // tree plan still pending

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(460.0f, 0.0f), ImVec2(460.0f, FLT_MAX));
    if (!ImGui::BeginPopupModal("##confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        // Modal was open before but is gone now: dismissed with Esc (the only
        // dismiss path outside the buttons). Clear the request — never leave a
        // stale request or a zombie invisible modal behind.
        if (!ImGui::IsPopupOpen("##confirm") && !Ui().confirmOpenRequested) CloseConfirm();
        g_dialogAutotest.modalOpen = false;
        g_dialogAutotest.framesOpen = 0;
        return;
    }
    g_dialogAutotest.modalOpen = true;
    ++g_dialogAutotest.framesOpen;
    // P2 (V14): the tree plan lands asynchronously — show an honest progress line
    // instead of an empty dialog, and keep the action disabled until it arrives.
    const bool planPending = req.kind == ui::ConfirmKind::KillTree && req.planCount &&
                             req.planCount->load() < 0;

    const wchar_t* title = L"确认";
    const wchar_t* action = L"确认";
    switch (req.kind) {
        case ui::ConfirmKind::Kill: title = L"确认终止进程"; action = L"终止进程"; break;
        case ui::ConfirmKind::KillTree: title = L"确认终止进程树"; action = L"终止进程树"; break;
        case ui::ConfirmKind::TrimWorkingSet: title = L"确认释放工作集"; action = L"释放工作集"; break;
        case ui::ConfirmKind::PurgeStandby: title = L"确认清理待机列表"; action = L"清理待机列表"; break;
        case ui::ConfirmKind::Suspend: title = L"确认挂起进程"; action = L"挂起进程"; break;
        case ui::ConfirmKind::SetPriority: title = L"调整进程优先级"; action = L"应用"; break;
        case ui::ConfirmKind::MemCleanup: title = L"一键优化内存"; action = L"开始优化"; break;
        case ui::ConfirmKind::CloseAsk: title = L"关闭超级任务管理器"; action = L"退出程序"; break;
        default: break;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ColFail());
    ImGui::TextUnformatted(U8(title));
    ImGui::PopStyleColor();
    ImGui::Separator();

    switch (req.kind) {
        case ui::ConfirmKind::Kill:
            ImGui::TextUnformatted(U8(Fmt(L"目标：{} (PID {})", req.name, req.pid)));
            // P2 (V14): long image paths overflow the 460px modal — wrap them.
            ImGui::TextWrapped("%s",
                               U8(Fmt(L"路径：{}", req.path.empty() ? std::wstring(L"—") : req.path)));
            if (req.serviceHost) {
                ImGui::TextColored(ColFail(), "%s",
                                   U8(L"警告：该进程是服务宿主，终止可能影响系统服务。"));
            }
            ImGui::TextDisabled("%s", U8(L"此操作无法撤销。"));
            break;
        case ui::ConfirmKind::KillTree: {
            ImGui::TextUnformatted(U8(Fmt(L"目标：{} (PID {})", req.name, req.pid)));
            if (planPending) {
                ImGui::TextDisabled("%s", U8(L"正在规划进程树…（完成后显示预计终止数量）"));
            } else {
                int planned = -1;
                if (req.planCount) planned = req.planCount->load();
                // PlanTerminateTree returns descendants only; TreeResult.planned
                // includes the root, so display planned + 1 (target included) to
                // match that count.
                ImGui::TextUnformatted(
                    U8(Fmt(L"预计终止 {} 个（含目标进程，执行时可能变化）", planned + 1)));
            }
            if (req.serviceHost) {
                ImGui::TextColored(ColFail(), "%s",
                                   U8(L"警告：该进程是服务宿主，终止将影响其承载的全部服务。"));
            }
            ImGui::TextDisabled("%s", U8(L"此操作无法撤销。"));
            break;
        }
        case ui::ConfirmKind::TrimWorkingSet:
            ImGui::TextUnformatted(U8(Fmt(L"目标：{} (PID {})", req.name, req.pid)));
            ImGui::TextWrapped(
                "%s", U8(L"将提示系统尽量释放该进程的物理工作集内存。工作集页换出后再次访问需要"
                          "重新读入，该进程短期可能变卡；此操作不会回收进程正在使用的内存。"));
            break;
        case ui::ConfirmKind::PurgeStandby:
            ImGui::TextWrapped(
                "%s", U8(L"仅释放系统文件缓存（待机列表），不会回收进程正在使用的内存；系统随后"
                          "按需重新缓存。需要管理员权限。"));
            break;
        case ui::ConfirmKind::Suspend:
            ImGui::TextUnformatted(U8(Fmt(L"目标：{} (PID {})", req.name, req.pid)));
            ImGui::TextWrapped(
                "%s",
                U8(L"挂起将冻结该进程的全部线程：它不再响应输入、释放不了锁，也可能连带"
                   L"挂起依赖它的服务或界面。诊断互锁/泄漏时请先尝试，确认后再执行。"));
            if (req.serviceHost) {
                ImGui::TextColored(ColFail(), "%s",
                                   U8(L"警告：该进程是服务宿主，挂起可能导致其承载的全部服务"
                                      L"卡死，且挂起比终止更难排查。"));
            }
            ImGui::TextDisabled("%s", U8(L"挂起后可随时用右键菜单「恢复进程」恢复。"));
            break;
        case ui::ConfirmKind::SetPriority:
            ImGui::TextUnformatted(U8(Fmt(L"目标：{} (PID {})", req.name, req.pid)));
            ImGui::TextUnformatted(
                U8(Fmt(L"新优先级：{}", ui3::PriorityLabel(req.priority))));
            if (req.priority == ops::ProcPriority::Realtime) {
                ImGui::TextColored(
                    ColFail(), "%s",
                    U8(L"警告：实时优先级可能抢占包括输入处理在内的所有系统任务，导致系统"
                       L"失去响应；仅应在独占硬件场景使用。"));
            } else if (req.priority == ops::ProcPriority::High) {
                ImGui::TextColored(ColWarn(), "%s",
                                   U8(L"提示：高优先级进程会优先于普通程序获得 CPU 时间。"));
            }
            ImGui::TextDisabled("%s", U8(L"设置立即生效，不持久化（重启后恢复默认）。"));
            break;
        case ui::ConfirmKind::MemCleanup: {
            // 诚实表述与单个「释放工作集」一致：换出有缺页代价，不是回收内存。
            ImGui::TextWrapped(
                "%s", U8(L"将按勾选项提示系统释放物理工作集。工作集页换出后再次访问需要重新"
                          L"读入，对应进程短期可能变卡；此操作不会回收进程正在使用的内存。"
                          L"系统关键进程/服务宿主已标注为不可选。"));
            if (req.cleanupItems.empty()) {
                ImGui::TextDisabled("%s", U8(L"暂无可优化的候选进程（等待采集数据）。"));
            } else {
                if (ImGui::BeginChild("##cleanuplist", ImVec2(0.0f, 230.0f),
                                      ImGuiChildFlags_Borders)) {
                    if (ImGui::BeginTable("##cleanuptbl", 3, ImGuiTableFlags_RowBg |
                                                                ImGuiTableFlags_BordersInnerH |
                                                                ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn(U8(L"进程"), ImGuiTableColumnFlags_WidthStretch, 2.4f);
                        ImGui::TableSetupColumn(U8(L"PID"), ImGuiTableColumnFlags_WidthFixed, 72.0f);
                        ImGui::TableSetupColumn(U8(L"私有工作集"), ImGuiTableColumnFlags_WidthFixed, 104.0f);
                        ImGui::TableHeadersRow();
                        for (size_t i = 0; i < req.cleanupItems.size(); ++i) {
                            const ui3::CleanupCandidate& c = req.cleanupItems[i];
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            if (c.selectable && i < req.cleanupSelected.size()) {
                                char id[24];
                                snprintf(id, sizeof(id), "##mcsel_%zu", i);
                                // vector<bool> 是代理引用，不能取 &：经栈上 bool 中转。
                                bool checked = req.cleanupSelected[i];
                                ImGui::Checkbox(id, &checked);
                                req.cleanupSelected[i] = checked;
                                ImGui::SameLine();
                                ImGui::TextUnformatted(U8(c.name));
                            } else {
                                // 不可选：保护名单/系统进程按分类器标注，勾选框禁用。
                                ImGui::TextDisabled(
                                    "%s", U8(Fmt(L"{}（{}，不可选）", c.name.empty()
                                                                     ? std::wstring(L"—")
                                                                     : c.name,
                                                                 ui3::ProcKindLegendLabel(c.kind))));
                            }
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(U8(Fmt(L"{}", c.key.pid)));
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(
                                U8(c.privateWorkingSet != kUnavailU64
                                       ? FormatBytes(c.privateWorkingSet)
                                       : std::wstring(L"—")));
                        }
                        ImGui::EndTable();
                    }
                }
                ImGui::EndChild();
                ImGui::Checkbox(U8(L"同时清理系统待机缓存"), &req.cleanupPurgeStandby);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s",
                                      U8(L"附加清理系统文件缓存（待机列表）：仅释放系统缓存页，"
                                         L"不会回收进程内存，系统随后按需重新缓存。需要管理员权限。"));
                }
                ImGui::TextDisabled(
                    "%s", U8(Fmt(L"已选 {} 个进程，涉及约 {} 私有工作集（估计值）",
                                 ui3::CountSelected(req.cleanupSelected),
                                 FormatBytes(ui3::SelectedBytes(req.cleanupItems,
                                                                req.cleanupSelected)))));
            }
            break;
        }
        case ui::ConfirmKind::CloseAsk:
            ImGui::TextUnformatted(U8(L"要退出程序，还是最小化到通知区域托盘？"));
            ImGui::TextDisabled(
                "%s", U8(L"最小化后可从托盘图标左键呼出主窗口；托盘菜单「退出」始终直接退出，"
                          L"不再询问。"));
            break;
        default:
            break;
    }

    ImGui::Separator();

    // P3 任务三：CloseAsk 的三按钮分支（退出程序 / 最小化到托盘 / 取消）+
    // 「记住我的选择」。勾选时写 cfg closeAction（退出时统一持久化），否则不写
    // （保持 0=每次询问）。键盘焦点落在「取消」（V8-P1-2 同规则，不落在首个按钮）。
    if (req.kind == ui::ConfirmKind::CloseAsk) {
        auto remember = [&](int action) {
            std::shared_ptr<AppContext> app = Ui().liveCtx;
            if (app && req.rememberChoice) app->cfg.SetInt(ui::kCloseActionCfgKey, action);
        };
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(2);
        if (ImGui::Button(U8(L"退出程序"), ImVec2(120.0f, 0.0f))) {
            remember(1);
            if (std::shared_ptr<AppContext> app = Ui().liveCtx) app->wantExit = true;
            CloseConfirm();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(U8(L"最小化到托盘"), ImVec2(120.0f, 0.0f))) {
            remember(2);
            if (std::shared_ptr<AppContext> app = Ui().liveCtx; app && app->mainHwnd) {
                ShowWindow(static_cast<HWND>(app->mainHwnd), SW_HIDE);  // 托盘已有恢复逻辑
            }
            // V18-P2-1: a second X click while this dialog was open re-sets the
            // pending flag; after minimize the hidden window would otherwise re-open
            // the dialog invisibly.
            if (std::shared_ptr<AppContext> app = Ui().liveCtx) {
                app->closeAskPending.store(false);
            }
            CloseConfirm();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(U8(L"取消"), ImVec2(120.0f, 0.0f))) {
            if (std::shared_ptr<AppContext> app = Ui().liveCtx) {
                app->closeAskPending.store(false);  // V18-P2-1: same lingering-pending guard
            }
            CloseConfirm();  // 不写 cfg：保持当前行为（默认每次询问）
            ImGui::CloseCurrentPopup();
        }
        ImGui::Checkbox(U8(L"记住我的选择"), &req.rememberChoice);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", U8(L"记住本次选择，之后点标题栏 X 不再询问（工具条暂无"
                                      L"重置入口；重新安装/删除配置文件可恢复默认）。"));
        }
        ImGui::EndPopup();
        return;
    }

    // V8-P1-2: keyboard focus lands on CANCEL (not the destructive action), and the
    // action button is removed from keyboard nav entirely, so Space/Enter in the
    // freshly opened modal can never fire a terminate.
    // Bug F1 fix (2026-09): SetKeyboardFocusHere must run ONCE when the modal
    // appears. Issuing it every frame re-submits a nav move targeting the cancel
    // button, and NavMoveRequestApplyResult() (imgui.cpp) calls ClearActiveID()
    // whenever the mouse-held button is not the nav target — the mouse capture of
    // the action button is stolen between mouse-down and mouse-up, silently eating
    // every confirm click (same root cause as the phase-3 startup dialog).
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(0);
    if (ImGui::Button(U8(L"取消"), ImVec2(120.0f, 0.0f))) {
        CloseConfirm();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    // P3 任务一：一键优化在零勾选时禁用执行（诚实的空选择守卫）。
    const bool actionDisabled =
        planPending || (req.kind == ui::ConfirmKind::MemCleanup &&
                        !ui3::AnySelected(req.cleanupSelected));
    ImGui::BeginDisabled(actionDisabled);
    const bool actionPressed =
        ImGui::Button(U8(action), ImVec2(120.0f, 0.0f));
    const ImVec2 btnMin = ImGui::GetItemRectMin();
    const ImVec2 btnMax = ImGui::GetItemRectMax();
    ImGui::EndDisabled();
    ImGui::PopItemFlag();
    g_dialogAutotest.actionMinX = btnMin.x;
    g_dialogAutotest.actionMinY = btnMin.y;
    g_dialogAutotest.actionMaxX = btnMax.x;
    g_dialogAutotest.actionMaxY = btnMax.y;
    if (actionPressed) {
        // ExecuteConfirmedAction posts a JobFailed note itself when the queue
        // refuses the job — the toast pipeline reports it either way.
        ui::ExecuteConfirmedAction(Ui().liveCtx, req);
        CloseConfirm();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ===========================================================================
// Performance history: ring buffers of the last 120 collection ticks, appended
// in the shell (single ingestion point) when the tick id changes.
// ===========================================================================

constexpr int kHistCap = 120;

struct Ring {
    std::vector<float> v;
    int head = 0;
    // P3 任务二：至少收到过一个有效（非 NaN）样本 —— 不可用计数器（如硬故障/s
    // 在部分机器上 kUnavail）据此渲染诚实空态，而不是一条看不见的空线。
    bool hasData = false;
    void Push(float x) {
        if (x == x) hasData = true;
        if (static_cast<int>(v.size()) < kHistCap) {
            v.push_back(x);
            return;
        }
        v[static_cast<size_t>(head)] = x;
        head = (head + 1) % kHistCap;
    }
    int Count() const { return static_cast<int>(v.size()); }
    int Offset() const { return Count() < kHistCap ? 0 : head; }
};

struct PerfHistory {
    uint64_t lastTick = 0;
    Ring cpuTotal, physAvail, commit, diskRead, diskWrite, netRecv, netSend;
    // P3 任务二：新增系统级硬故障/s 与上下文切换/s（聚合口径见 AppendHistory）。
    Ring hardFaults, ctxSwitch;
    std::vector<Ring> cores;
};

PerfHistory& Hist() {
    static PerfHistory h;
    return h;
}

void AppendHistory(const Snapshot& s) {
    PerfHistory& h = Hist();
    if (s.tickId == 0 || s.tickId == h.lastTick) return;  // initial empty / same tick
    h.lastTick = s.tickId;
    // F4#7: 性能 CSV 记录与环形历史共用摄取点（每 tick 一行，1 Hz 小写入）。
    // V15-P1: 写入失败（磁盘满等）→ 记录器已自动停止，这里如实 toast（含路径），
    // 后续 tick 因未激活成为空操作 —— 绝不静默丢行而 UI 仍显示"记录中"。
    if (!ui3::SharedPerfCsv().Append(s)) {
        const std::wstring failedPath = ui3::SharedPerfCsv().Path();
        PushToast(Notification::Kind::JobFailed,
                  Fmt(L"性能 CSV 记录已自动停止——磁盘写入失败：{}", failedPath));
    }
    h.cpuTotal.Push(static_cast<float>(s.sys.cpuTotalPercent));      // NaN ok: rendered skipped
    h.physAvail.Push(static_cast<float>(s.sys.physAvail));
    h.commit.Push(static_cast<float>(s.sys.commitTotal));
    h.diskRead.Push(static_cast<float>(s.sys.diskReadBps));
    h.diskWrite.Push(static_cast<float>(s.sys.diskWriteBps));
    h.netRecv.Push(static_cast<float>(s.sys.netRecvBps));
    h.netSend.Push(static_cast<float>(s.sys.netSendBps));
    // P3 任务二：硬故障/s 直读 sys（部分机器 PDH 无此计数器 -> NaN -> 诚实空态）。
    // 上下文切换/s 无系统级计数器：聚合各进程 contextSwitchesPerSec（完整模式
    // 有值；兼容模式全 NaN 时聚合不可用，同样渲染「本机此计数器不可用」）。
    h.hardFaults.Push(static_cast<float>(s.sys.hardFaultsPerSec));
    double ctxSum = 0.0;
    bool ctxAny = false;
    for (const ProcInfo& p : s.procs) {
        if (p.contextSwitchesPerSec == p.contextSwitchesPerSec) {
            ctxSum += p.contextSwitchesPerSec;
            ctxAny = true;
        }
    }
    h.ctxSwitch.Push(ctxAny ? static_cast<float>(ctxSum)
                            : std::numeric_limits<float>::quiet_NaN());

    const size_t n = s.sys.perCorePercent.size();
    if (n > 0 && h.cores.size() != n) {
        // Core count discovery (or a rare hot-change): (re)allocate, history restarts.
        h.cores.assign(n, Ring{});
    }
    for (size_t i = 0; i < h.cores.size() && i < n; ++i) {
        h.cores[i].Push(static_cast<float>(s.sys.perCorePercent[i]));
    }
}

// ===========================================================================
// ProcessesPage: full table per arch section 8 + fixed-width detail panel.
// ===========================================================================

const ProcInfo* FindByPid(const Snapshot& snap, uint32_t pid) {
    // Snapshot procs are ascending by pid (contract); binary search + createTime verify.
    size_t lo = 0, hi = snap.procs.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (snap.procs[mid].key.pid < pid) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < snap.procs.size() && snap.procs[lo].key.pid == pid) return &snap.procs[lo];
    return nullptr;
}

// Detail-kind bit set fetched whenever the selection changes (F4: includes the
// module list — collected by DetailsProvider but never requested/rendered before).
constexpr uint32_t kDetailKinds =
    static_cast<uint32_t>(ops::DetailKind::Signature) | static_cast<uint32_t>(ops::DetailKind::CmdLine) |
    static_cast<uint32_t>(ops::DetailKind::UserInfo) | static_cast<uint32_t>(ops::DetailKind::GuiObjects) |
    static_cast<uint32_t>(ops::DetailKind::Modules);

class ProcessesPage final : public IPage {
public:
    const wchar_t* Id() const override { return L"processes"; }
    const wchar_t* Title() const override { return L"进程"; }

    void Draw(AppContext& ctx) override {
        const std::shared_ptr<const Snapshot>& snap = Ui().snap;  // shell-read, never null
        metaBudget_ = 3;  // per-frame cap for new GetFileVersionInfoW queries
        LoadPersistedOnce(ctx);
        // H-A: 外壳「外观→恢复默认列宽」置位世代号后，这里把 widths_ 拉回默认；
        // 表格 id 同步换代（DrawTable），ImGui 内部按 id 记忆的旧列宽随之丢弃。
        if (appliedResetGen_ != Ui().colWidthResetGen) {
            appliedResetGen_ = Ui().colWidthResetGen;
            for (int i = 0; i < kColCount; ++i) widths_[i] = kDefaultWidths[i];
        }
        // F4#3: 消费跨页跳转（DrawShell 已把 activePage 切到本页）。找不到时诚实提示。
        if (Ui().jumpPendingPid != 0) {
            const uint32_t pid = Ui().jumpPendingPid;
            Ui().jumpPendingPid = 0;
            const ProcInfo* jp = FindByPid(*snap, pid);
            if (jp != nullptr) {
                Select(*jp);
            } else {
                PushToast(Notification::Kind::Warn, Fmt(L"进程 {} 已退出，无法跳转", pid));
            }
        }
        if (snap->tickId != lastTickId_) {
            lastTickId_ = snap->tickId;
            RefreshSelection(*snap);
            rebuildNeeded_ = true;
        }
        UpdateFilter();
        if (rebuildNeeded_) {
            RebuildRows(*snap);
            rebuildNeeded_ = false;
        }

        DrawToolbar(*snap);
        if (sysDistMode_ == 1) DrawKindLegend();

        const float detailW = 380.0f;
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::BeginChild("##tablearea",
                          ImVec2(std::max(120.0f, avail.x - detailW - ImGui::GetStyle().ItemSpacing.x),
                                 avail.y));
        DrawTable(ctx, *snap);
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##detailarea", ImVec2(detailW, avail.y), ImGuiChildFlags_Borders);
        DrawDetailPanel(ctx, *snap);
        ImGui::EndChild();
        DrawAffinityModal(ctx);
    }

private:
    // ---- persistence ------------------------------------------------------
    void LoadPersistedOnce(AppContext& ctx) {
        if (loaded_) return;
        loaded_ = true;
        ui::SortColumn c = ui::SortColumn::Name;
        if (ui::ParseSortColumn(ctx.cfg.GetString(L"sortKey", L"name").c_str(), &c)) sortColumn_ = c;
        sortDesc_ = ctx.cfg.GetInt(L"sortDir", 0) != 0;
        Ui().sortColumn = sortColumn_;
        Ui().sortDesc = sortDesc_;

        const int64_t pid = ctx.cfg.GetInt(L"selPid", 0);
        if (pid > 0) {
            selected_ = ProcKey{static_cast<uint32_t>(pid),
                                static_cast<uint64_t>(ctx.cfg.GetInt(L"selCreateTime", 0))};
            selectedValid_ = true;
            Ui().selectedKey = selected_;
            Ui().selectedValid = true;
        }
        ctx.cfg.SetInt(L"selPid", 0);  // one-shot: never persist a stale selection
        ctx.cfg.SetInt(L"selCreateTime", 0);

        // Name/description are stretch columns (value = weight); the rest are fixed
        // pixel widths. Persist both flavors in the same slot, keyed per column.
        for (int i = 0; i <= static_cast<int>(ui::SortColumn::CtxSwitches); ++i) {
            widths_[i] = static_cast<float>(ctx.cfg.GetDouble(
                std::wstring(L"colW_") + ui::SortColumnId(static_cast<ui::SortColumn>(i)),
                kDefaultWidths[i]));
        }
        widths_[11] = static_cast<float>(ctx.cfg.GetDouble(L"colW_badges", kDefaultWidths[11]));
        widths_[12] = static_cast<float>(ctx.cfg.GetDouble(L"colW_desc", kDefaultWidths[12]));
        // P0-2 (F2 review): the description (and name) slots hold STRETCH WEIGHTS.
        // Older builds persisted pixels (hundreds) into these slots, collapsing the
        // name column on every start; any legacy value > 10 cannot be a weight.
        constexpr float kMaxPlausibleWeight = 10.0f;
        if (widths_[0] > kMaxPlausibleWeight || widths_[0] <= 0.0f) widths_[0] = kDefaultWidths[0];
        if (widths_[12] > kMaxPlausibleWeight || widths_[12] <= 0.0f) widths_[12] = kDefaultWidths[12];

        // F4#1/#4: 系统进程区分模式与树形视图开关（cfg 持久化）。
        sysDistMode_ = static_cast<int>(ctx.cfg.GetInt(L"sysDistMode", 0));
        if (sysDistMode_ < 0 || sysDistMode_ > 2) sysDistMode_ = 0;
        treeMode_ = ctx.cfg.GetBool(L"procTreeMode", false);
    }

    void PersistSort(AppContext& ctx) {
        ctx.cfg.SetString(L"sortKey", ui::SortColumnId(sortColumn_));
        ctx.cfg.SetInt(L"sortDir", sortDesc_ ? 1 : 0);
    }

    void PersistWidths(AppContext& ctx) {
        // Called at most ~1 Hz from inside the table; cheap key/value writes only
        // (the file itself is saved by main on exit).
        // H-A 登记：本函数写入的 colW_* 键清单以 ui3::ColWidthCfgKeys()
        // （app/ui3/ThemeCfg.h）为准并一一对应 —— 0..10 = "colW_"+SortColumnId
        // （含 colW_name），另加 colW_badges、colW_desc。「外观→恢复默认列宽」
        // 按该清单软删除并从 config.json 剔除。
        if (ImGui::GetTime() - lastWidthSave_ < 1.0) return;
        ImGuiTable* table = ImGui::GetCurrentTable();
        if (!table) return;
        bool changed = false;
        auto save = [&](int i, const std::wstring& key, bool stretch) {
            const float w = stretch ? table->Columns[i].StretchWeight
                                    : table->Columns[i].WidthGiven;
            if (w > 0.01f && (w - widths_[i]) * (w - widths_[i]) > 0.001f) {
                widths_[i] = w;
                ctx.cfg.SetDouble(key, static_cast<double>(w));
                changed = true;
            }
        };
        for (int i = 0; i <= static_cast<int>(ui::SortColumn::CtxSwitches); ++i) {
            save(i, std::wstring(L"colW_") + ui::SortColumnId(static_cast<ui::SortColumn>(i)), false);
        }
        save(11, L"colW_badges", false);
        // P0-2 (F2 review): the description column is WidthStretch — persist its
        // stretch WEIGHT (the old code wrote WidthGiven pixels into the weight slot,
        // collapsing the name column on every subsequent start).
        save(12, L"colW_desc", true);
        save(0, L"colW_name", true);  // stretch weight for the name column
        if (changed) lastWidthSave_ = ImGui::GetTime();
    }

    // ---- selection ---------------------------------------------------------
    void Select(const ProcInfo& p) {
        if (selectedValid_ && p.key == selected_ && !lost_) return;
        selected_ = p.key;
        selectedValid_ = true;
        lost_ = false;
        lastKnown_ = p;
        detailKey_ = ProcKey{};  // force a details re-request
        Ui().selectedKey = selected_;
        Ui().selectedValid = true;
    }

    void RefreshSelection(const Snapshot& snap) {
        if (!selectedValid_) return;
        const ProcInfo* p = FindByPid(snap, selected_.pid);
        if (p != nullptr && p->key == selected_) {
            lost_ = false;
            lastKnown_ = *p;
        } else if (!lost_) {
            lost_ = true;  // keep lastKnown_ frozen; the panel shows "已退出"
        }
    }

    // ---- filtering / sorting -----------------------------------------------
    void UpdateFilter() {
        // Lowercase the needle once here; ContainsLower lowercases the haystack per
        // row. Matching is case-insensitive for ASCII ("Chrome" matches chrome.exe).
        std::wstring needle = Utf8ToWide(filterUtf8_);
        std::transform(needle.begin(), needle.end(), needle.begin(),
                       [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
        filterWide_ = std::move(needle);
        if (filterWide_ != lastFilterWide_) {
            lastFilterWide_ = filterWide_;
            rebuildNeeded_ = true;
        }
    }

    static bool ContainsLower(const std::wstring& hay, const std::wstring& needle) {
        if (needle.empty()) return true;
        std::wstring lower(hay.size(), L'\0');
        std::transform(hay.begin(), hay.end(), lower.begin(),
                       [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
        return lower.find(needle) != std::wstring::npos;
    }

    bool MatchesFilter(const ProcInfo& p) const {
        if (filterWide_.empty()) return true;
        wchar_t pid[16] = {};
        swprintf_s(pid, L"%u", p.key.pid);
        return ContainsLower(p.name, filterWide_) || ContainsLower(pid, filterWide_);
    }

    // F4#1: "只看用户进程"（sysDistMode 2）——隐藏 Critical/Windows/ServiceHost，
    // Uwp 属用户应用保留。分类按分类器优先级（Critical > ServiceHost > Uwp >
    // Windows > User）；Unknown（无名称无路径无徽标）按可见处理（诚实降级）。
    bool PassesDistFilter(const ProcInfo& p) const {
        if (sysDistMode_ != 2) return true;
        const ui3::ProcKind k = ui3::ClassifyProc(p.key.pid, p.name, p.path, p.flags);
        return ui3::ProcKindVisibleInUserFilter(k);
    }

    // F4#1/#4: 行序重建 = 文本过滤 + 系统进程过滤 + 当前排序键排序，
    // 树形模式再按 (parentPid, createTime 防复用) DFS 展平（先过滤后建树）。
    void RebuildRows(const Snapshot& snap) {
        std::vector<int> filtered;
        filtered.reserve(snap.procs.size());
        for (int i = 0; i < static_cast<int>(snap.procs.size()); ++i) {
            const ProcInfo& p = snap.procs[static_cast<size_t>(i)];
            if (MatchesFilter(p) && PassesDistFilter(p)) filtered.push_back(i);
        }
        std::stable_sort(filtered.begin(), filtered.end(), [this, &snap](int x, int y) {
            return ui::SortLess(snap.procs[static_cast<size_t>(x)],
                                snap.procs[static_cast<size_t>(y)], sortColumn_, sortDesc_);
        });

        rows_.clear();
        rows_.reserve(filtered.size());
        kinds_.clear();
        if (!treeMode_) {
            for (int i : filtered) {
                const ProcInfo& p = snap.procs[static_cast<size_t>(i)];
                rows_.push_back(ui3::TreeRow{i, 0});
                kinds_.push_back(ui3::ClassifyProc(p.key.pid, p.name, p.path, p.flags));
            }
            return;
        }
        // 树形：把过滤后的子集拷出（已按当前排序键排序，兄弟序 = 子集原序），
        // BuildTreeOrder 输出相对下标，再映射回快照绝对下标。
        std::vector<ProcInfo> subset;
        std::vector<int> idxMap;
        subset.reserve(filtered.size());
        idxMap.reserve(filtered.size());
        for (int i : filtered) {
            subset.push_back(snap.procs[static_cast<size_t>(i)]);
            idxMap.push_back(i);
        }
        std::vector<ui3::TreeRow> tree;
        ui3::BuildTreeOrder(subset, [](int a, int b) { return a < b; }, &tree);
        rows_.reserve(tree.size());
        kinds_.reserve(tree.size());
        for (const ui3::TreeRow& r : tree) {
            const int abs = idxMap[static_cast<size_t>(r.index)];
            const ProcInfo& p = snap.procs[static_cast<size_t>(abs)];
            rows_.push_back(ui3::TreeRow{abs, r.depth});
            kinds_.push_back(ui3::ClassifyProc(p.key.pid, p.name, p.path, p.flags));
        }
    }

    // ---- F4#1: 高亮模式表格上方图例 ------------------------------------------
    void DrawKindLegend() const {
        struct Entry { ui3::ProcKind kind; ThemeAccent accent; };
        static const Entry kItems[] = {
            {ui3::ProcKind::Critical,    ThemeAccent::KindCritical},
            {ui3::ProcKind::Windows,     ThemeAccent::KindWindows},
            {ui3::ProcKind::ServiceHost, ThemeAccent::KindServiceHost},
            {ui3::ProcKind::Uwp,         ThemeAccent::KindUwp},
            {ui3::ProcKind::User,        ThemeAccent::KindUser},
        };
        for (const Entry& e : kItems) {
            ImGui::TextColored(AccentCol(e.accent), "%s", U8(L"●"));
            ImGui::SameLine();
            ImGui::TextUnformatted(U8(ui3::ProcKindLegendLabel(e.kind)));
            ImGui::SameLine();
        }
        ImGui::TextDisabled("%s", U8(Fmt(L"模式：{}", ui3::SysDistModeLabel(sysDistMode_))));
    }

    // ---- toolbar ------------------------------------------------------------
    void DrawToolbar(const Snapshot& snap) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", filterUtf8_.c_str());
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::InputTextWithHint("##filter", U8(L"搜索名称或 PID"), buf, sizeof(buf))) {
            filterUtf8_ = buf;
        }
        ImGui::SameLine();
        if (ImGui::Button(U8(L"清空"))) {
            filterUtf8_.clear();
        }
        // F4#1: 区分系统进程（下拉三态，cfg sysDistMode）。
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::BeginCombo("##sysdist", U8(Fmt(L"区分系统进程：{}",
                                                  ui3::SysDistModeLabel(sysDistMode_))))) {
            for (int m = 0; m <= 2; ++m) {
                if (ImGui::Selectable(U8(ui3::SysDistModeLabel(m)), m == sysDistMode_)) {
                    sysDistMode_ = m;
                    if (std::shared_ptr<AppContext> app = Ui().liveCtx) {
                        app->cfg.SetInt(L"sysDistMode", m);
                    }
                    rebuildNeeded_ = true;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                              U8(L"高亮：按类别着色进程名；只看用户进程：隐藏系统关键/"
                                 L"Windows 系统进程/服务宿主（UWP 应用保留）"));
        }
        // F4#4: 平铺/树形切换（cfg procTreeMode）。
        ImGui::SameLine();
        if (ImGui::Button(treeMode_ ? U8(L"平铺视图") : U8(L"树形视图"))) {
            treeMode_ = !treeMode_;
            if (std::shared_ptr<AppContext> app = Ui().liveCtx) {
                app->cfg.SetBool(L"procTreeMode", treeMode_);
            }
            sortReflected_ = false;  // 返回平铺时重新同步表头排序指示
            rebuildNeeded_ = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                              U8(treeMode_ ? L"当前为树形视图（按父进程层级缩进，"
                                             L"排序仅作用于同级）：点击回到平铺"
                                           : L"按父进程层级展示（孤儿/父已退出提升为根）："
                                             L"点击进入树形视图"));
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", U8(Fmt(L"{} / {} 个进程", rows_.size(), snap.procs.size())));
    }

    // ---- table ---------------------------------------------------------------
    void DrawTable(AppContext& ctx, const Snapshot& snap) {
        if (snap.procs.empty()) {
            ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.65f, 1.0f), "%s", U8(L"等待采集数据…"));
            return;
        }

        // F4#4: 树形模式下禁用表头点击排序（全局排序降级为同级排序，UI 明示）。
        // H-A: 表格 id 带列宽重置世代号 —— 恢复默认后换代，ImGui 丢弃按 id
        // 记忆的旧列宽，SetupColumns 的默认值立即生效。
        char tableId[32];
        snprintf(tableId, sizeof(tableId), "procs#%llu",
                 static_cast<unsigned long long>(Ui().colWidthResetGen));
        const int tableFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                               ImGuiTableFlags_SizingFixedFit |
                               (treeMode_ ? 0 : ImGuiTableFlags_Sortable);
        if (!ImGui::BeginTable(tableId, static_cast<int>(ui::SortColumn::Count) + 2,
                               tableFlags)) {
            return;
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        SetupColumns();
        if (!treeMode_) ReflectPersistedSortOnce();

        if (!treeMode_) {
            if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
                if (specs->SpecsDirty) {
                    if (specs->SpecsCount > 0 && specs->Specs[0].ColumnIndex < static_cast<int>(ui::SortColumn::Count)) {
                        // The sort key changes ONLY on a header click by the user.
                        sortColumn_ = static_cast<ui::SortColumn>(specs->Specs[0].ColumnIndex);
                        sortDesc_ = specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
                        Ui().sortColumn = sortColumn_;
                        Ui().sortDesc = sortDesc_;
                        PersistSort(ctx);
                    }
                    specs->SpecsDirty = false;
                    rebuildNeeded_ = true;
                }
            }
        }

        ImGui::TableHeadersRow();
        DrawHeaderTooltips();
        if (treeMode_ && ImGui::TableGetColumnFlags(0) & ImGuiTableColumnFlags_IsHovered) {
            ImGui::SetTooltip("%s",
                              U8(L"树形视图：点击表头排序已禁用，排序键仅在同级进程内生效"));
        }

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows_.size()));
        while (clipper.Step()) {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                const ui3::TreeRow& row = rows_[static_cast<size_t>(r)];
                DrawRow(ctx, snap.procs[static_cast<size_t>(row.index)], row.depth,
                        kinds_[static_cast<size_t>(r)]);
            }
        }

        PersistWidths(ctx);
        ImGui::EndTable();
    }

    void SetupColumns() {
        ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch |
                                              ImGuiTableColumnFlags_NoHide |
                                              ImGuiTableColumnFlags_NoClip, widths_[0]);
        ImGui::TableSetupColumn(U8(L"PID"), ImGuiTableColumnFlags_WidthFixed, widths_[1]);
        ImGui::TableSetupColumn(U8(L"CPU%"), ImGuiTableColumnFlags_WidthFixed, widths_[2]);
        ImGui::TableSetupColumn(U8(L"内存"), ImGuiTableColumnFlags_WidthFixed, widths_[3]);
        ImGui::TableSetupColumn(U8(L"提交"), ImGuiTableColumnFlags_WidthFixed, widths_[4]);
        ImGui::TableSetupColumn(U8(L"磁盘"), ImGuiTableColumnFlags_WidthFixed, widths_[5]);
        ImGui::TableSetupColumn(U8(L"网络"), ImGuiTableColumnFlags_WidthFixed, widths_[6]);
        ImGui::TableSetupColumn(U8(L"硬故障/s"), ImGuiTableColumnFlags_WidthFixed, widths_[7]);
        ImGui::TableSetupColumn(U8(L"句柄"), ImGuiTableColumnFlags_WidthFixed, widths_[8]);
        ImGui::TableSetupColumn(U8(L"线程"), ImGuiTableColumnFlags_WidthFixed, widths_[9]);
        ImGui::TableSetupColumn(U8(L"上下文切换/s"), ImGuiTableColumnFlags_WidthFixed, widths_[10]);
        ImGui::TableSetupColumn(U8(L"徽标"), ImGuiTableColumnFlags_WidthFixed |
                                                 ImGuiTableColumnFlags_NoSort, widths_[11]);
        ImGui::TableSetupColumn(U8(L"描述"), ImGuiTableColumnFlags_WidthStretch |
                                                 ImGuiTableColumnFlags_NoSort, widths_[12]);
    }

    void ReflectPersistedSortOnce() {
        if (sortReflected_) return;
        sortReflected_ = true;
        ImGui::TableSetColumnSortDirection(static_cast<int>(sortColumn_),
                                           sortDesc_ ? ImGuiSortDirection_Descending
                                                     : ImGuiSortDirection_Ascending,
                                           false);
    }

    void DrawHeaderTooltips() const {
        auto hover = [](int i) {
            return (ImGui::TableGetColumnFlags(i) & ImGuiTableColumnFlags_IsHovered) != 0;
        };
        if (hover(3)) ImGui::SetTooltip("%s", U8(L"私有工作集（不含共享页）"));
        if (hover(5)) ImGui::SetTooltip("%s", U8(L"含文件+网络+设备 IO 总和"));
        if (hover(6)) ImGui::SetTooltip("%s", U8(L"需要 ETW 采集能力，未启用时显示 —"));
        if (hover(7)) ImGui::SetTooltip("%s", U8(L"每秒硬缺页：需从磁盘读入的缺页次数"));
    }

    void DrawRow(AppContext& ctx, const ProcInfo& p, int depth, ui3::ProcKind kind) {
        // P2-11 (F2 review): identify rows by the stable ProcKey (pid + createTime),
        // not a display index — a refresh that reorders rows mid-interaction must
        // never retarget an open context menu / tooltip to a different process.
        ImGui::PushID(static_cast<int>(p.key.pid));
        ImGui::PushID(static_cast<int>(p.key.createTime & 0x7fffffff));
        ImGui::TableNextRow();

        // Name + row interaction (selection, double click, context menu).
        ImGui::TableNextColumn();
        // F4#4: 树形模式名称列缩进 depth*12px + 「└」连接符（根行无缩进）。
        if (depth > 0) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 static_cast<float>(depth) * ui3::kTreeIndentPx);
        }
        // F4#1: 高亮模式按类别着色进程名（图例见表格上方）。颜色经 Theme 强调
        // 色（H-A），浅色主题下自动切换为深色可读变体。
        ImVec4 nameColor(-1.0f, -1.0f, -1.0f, -1.0f);
        if (sysDistMode_ == 1) {
            switch (kind) {
                case ui3::ProcKind::Critical:    nameColor = AccentCol(ThemeAccent::KindCritical); break;
                case ui3::ProcKind::Windows:     nameColor = AccentCol(ThemeAccent::KindWindows); break;
                case ui3::ProcKind::ServiceHost: nameColor = AccentCol(ThemeAccent::KindServiceHost); break;
                case ui3::ProcKind::Uwp:         nameColor = AccentCol(ThemeAccent::KindUwp); break;
                default: break;  // User/Unknown 默认色
            }
        }
        const std::wstring displayName =
            depth > 0 ? std::wstring(ui3::TreeBranchGlyph()) + p.name : p.name;
        const bool selected = selectedValid_ && p.key == selected_;
        if (nameColor.w >= 0.0f) ImGui::PushStyleColor(ImGuiCol_Text, nameColor);
        if (ImGui::Selectable(U8(displayName), selected,
                              ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
            Select(p);
        }
        if (nameColor.w >= 0.0f) ImGui::PopStyleColor();
        if (selected) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                   ImGui::GetColorU32(ImGuiCol_Header, 0.45f));
        }
        DrawContextMenu(ctx, p);

        ImGui::TableNextColumn(); ImGui::Text("%u", p.key.pid);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(U8(FormatPercent(p.cpuPercent)));
        ImGui::TableNextColumn(); ImGui::TextUnformatted(U8(FormatBytes(p.privateWorkingSet)));
        ImGui::TableNextColumn(); ImGui::TextUnformatted(U8(FormatBytes(p.commitBytes)));
        ImGui::TableNextColumn(); ImGui::TextUnformatted(U8(FormatRate(p.diskBytesPerSec)));
        ImGui::TableNextColumn(); ImGui::TextUnformatted(U8(FormatRate(p.netBytesPerSec)));
        ImGui::TableNextColumn(); ImGui::TextUnformatted(U8(FormatNumber(p.pageFaultsPerSec)));
        ImGui::TableNextColumn(); ImGui::Text("%u", p.handles);
        ImGui::TableNextColumn(); ImGui::Text("%u", p.threads);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(U8(FormatNumber(p.contextSwitchesPerSec)));
        ImGui::TableNextColumn(); DrawBadges(p);
        ImGui::TableNextColumn(); DrawDescription(p);
        ImGui::PopID();
        ImGui::PopID();
    }

    void DrawBadges(const ProcInfo& p) {
        // H-A: 徽标颜色走 Theme 强调色 —— 浅色主题下徽标字母依然可读。
        auto badge = [&p](uint32_t flag, const char* label, const ThemeAccent accent) {
            if ((p.flags & flag) == 0) return;
            ImGui::TextColored(AccentCol(accent), "%s", label);
            ImGui::SameLine();
        };
        badge(PF_Elevated, "A", ThemeAccent::Warn);    // 管理员提权
        badge(PF_Uwp, "U", ThemeAccent::Info);         // UWP
        badge(PF_Wow64, "W", ThemeAccent::Done);       // WOW64
        if ((p.flags & PF_ServiceHost) != 0) {
            // F4#3: 服务宿主徽标 tooltip 列出宿主服务名（jobs 缓存，见 GcPages）。
            ImGui::TextColored(AccentCol(ThemeAccent::Purple), "S");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", U8(ui3::HostServiceNamesText(p.key.pid)));
            }
            ImGui::SameLine();
        }
        badge(PF_Protected, "P", ThemeAccent::Fail);   // 受保护
        badge(PF_Suspended, "Z", ThemeAccent::Gray);   // 挂起
        static const std::wstring kLegend =
            L"A 管理员提权  U UWP  W WOW64  S 服务宿主（悬停查看承载的服务）  P 受保护  Z 挂起";
        if ((ImGui::TableGetColumnFlags() & ImGuiTableColumnFlags_IsHovered) != 0) {
            ImGui::SetTooltip("%s", U8(kLegend));
        }
    }

    void DrawDescription(const ProcInfo& p) {
        if (p.path.empty()) {
            ImGui::TextDisabled("%s", U8(L"—"));
            return;
        }
        ui::FileMetaCache& cache = ui::FileMetaCacheForThread();
        const ui::FileMeta* m = cache.Find(p.path);
        if (m == nullptr && metaBudget_ > 0) {
            cache.Store(p.path, ui::QueryFileMeta(p.path));
            --metaBudget_;
            m = cache.Find(p.path);
        }
        if (m == nullptr) {
            ImGui::TextDisabled("%s", U8(L"…"));
        } else if (m->description.empty()) {
            ImGui::TextDisabled("%s", U8(L"—"));
        } else {
            ImGui::TextUnformatted(U8(m->description));
        }
    }

    void DrawContextMenu(AppContext& ctx, const ProcInfo& p) {
        if (!ImGui::BeginPopupContextItem("##rowctx")) return;
        std::wstring reason;
        const bool protectedProc = (p.flags & PF_Protected) != 0;
        if (protectedProc) reason = ProtectedReason(p.key.pid, p.name, p.path);
        const bool serviceHost = (p.flags & PF_ServiceHost) != 0;
        const bool suspended = (p.flags & PF_Suspended) != 0;

        if (ImGui::MenuItem(U8(L"查看详情"), nullptr, false, true)) Select(p);
        ImGui::Separator();
        if (protectedProc) {
            ImGui::MenuItem(U8(Fmt(L"受保护：{}", reason.empty() ? std::wstring(L"系统关键进程") : reason)),
                            nullptr, false, false);
        }
        // F4#3: 服务宿主 -> 查看承载的服务（模态，走 jobs 缓存）。
        if (serviceHost && ImGui::MenuItem(U8(L"查看宿主服务…"))) {
            ui3::RequestHostServicesModal(p.key.pid, p.name);
        }
        ImGui::BeginDisabled(protectedProc);
        if (ImGui::MenuItem(U8(L"终止进程"))) RequestConfirmKill(p);
        if (ImGui::MenuItem(U8(L"终止进程树"))) RequestTreePlan(p);
        // V8-P1-1: trim is a destructive op too — same gate as the terminate items.
        if (ImGui::MenuItem(U8(L"释放工作集"))) RequestConfirmTrim(p);
        ImGui::Separator();
        // F4#2: 挂起/恢复 + 优先级 + 亲和性。保护名单进程全部禁用
        //（挂起 csrss 比终止更恶劣）。挂起态徽标 (PF_Suspended) 决定菜单文案；
        // ops 执行时仍按 (pid, createTime) 重验身份 + 保护名单硬拒。
        if (suspended) {
            if (ImGui::MenuItem(U8(L"恢复进程"))) {
                ui::ConfirmRequest req;
                req.kind = ui::ConfirmKind::Resume;
                req.key = p.key;
                req.pid = p.key.pid;
                req.name = p.name;
                req.path = p.path;
                ui::ExecuteConfirmedAction(Ui().liveCtx, req);
                ctrlInfoDirty_ = true;
            }
        } else if (ImGui::MenuItem(U8(L"挂起进程…"))) {
            RequestConfirmSuspend(p);
        }
        if (ImGui::BeginMenu(U8(L"设置优先级"), !protectedProc)) {
            ops::ProcPriority current = ops::ProcPriority::Normal;
            ops::ProcessControlInfo ci;
            std::wstring ciErr;
            const CtrlQuery st = GetCtrlInfo(p.key, &ci, &ciErr);
            const bool hasCurrent = st == CtrlQuery::Ready &&
                                    ui3::PriorityFromWin32(ci.priorityClass, &current);
            for (int i = 0; i <= static_cast<int>(ops::ProcPriority::Realtime); ++i) {
                const ops::ProcPriority pr = static_cast<ops::ProcPriority>(i);
                const bool isRealtime = pr == ops::ProcPriority::Realtime;
                if (isRealtime) ImGui::PushStyleColor(ImGuiCol_Text, ColFail());
                if (ImGui::MenuItem(U8(ui3::PriorityLabel(pr)), nullptr,
                                    hasCurrent && current == pr)) {
                    RequestConfirmPriority(p, pr);
                }
                if (isRealtime) ImGui::PopStyleColor();
            }
            if (st == CtrlQuery::Failed) {
                ImGui::TextColored(ColWarn(), "%s",
                                   U8(Fmt(L"（优先级查询失败：{}）", ciErr)));
            } else if (!hasCurrent) {
                ImGui::TextDisabled("%s", U8(L"（当前优先级查询中…）"));
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(U8(L"设置亲和性…"), nullptr, false, !protectedProc)) {
            RequestAffinityModal(p);
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::MenuItem(U8(L"复制名称"))) ImGui::SetClipboardText(U8(p.name));
        ImGui::BeginDisabled(p.path.empty());
        if (ImGui::MenuItem(U8(L"复制路径"))) ImGui::SetClipboardText(U8(p.path));
        if (ImGui::MenuItem(U8(L"打开文件位置"))) OpenContainingFolder(p.path);
        if (ImGui::MenuItem(U8(L"校验签名"))) {
            Select(p);
            ctx.details->Request(p.key, p.path, static_cast<uint32_t>(ops::DetailKind::Signature));
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    // ---- F4#1/#2 确认请求构建 --------------------------------------------------
    void RequestConfirmSuspend(const ProcInfo& p) {
        CloseConfirm();
        auto& req = Ui().confirm;
        req.kind = ui::ConfirmKind::Suspend;
        req.key = p.key;
        req.pid = p.key.pid;
        req.name = p.name;
        req.path = p.path;
        req.serviceHost = (p.flags & PF_ServiceHost) != 0;
        Ui().confirmOpenRequested = true;
    }

    void RequestConfirmPriority(const ProcInfo& p, ops::ProcPriority pr) {
        CloseConfirm();
        auto& req = Ui().confirm;
        req.kind = ui::ConfirmKind::SetPriority;
        req.key = p.key;
        req.pid = p.key.pid;
        req.name = p.name;
        req.path = p.path;
        req.serviceHost = (p.flags & PF_ServiceHost) != 0;
        req.priority = pr;
        Ui().confirmOpenRequested = true;
    }

    static void OpenContainingFolder(const std::wstring& path) {
        if (path.empty()) return;
        const std::wstring arg = L"/select,\"" + path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    }

    // ---- detail panel ---------------------------------------------------------
    void DrawDetailPanel(AppContext& ctx, const Snapshot& snap) {
        if (!selectedValid_) {
            ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.65f, 1.0f), "%s", U8(L"选择一个进程查看详情"));
            return;
        }
        const ProcInfo* cur = FindByPid(snap, selected_.pid);
        const bool alive = cur != nullptr && cur->key == selected_;
        const ProcInfo& p = alive ? *cur : lastKnown_;

        ImGui::TextUnformatted(U8(p.name));
        ImGui::SameLine();
        ImGui::TextDisabled("PID %u", p.key.pid);
        if (lost_) {
            ImGui::TextColored(ColFail(), "%s", U8(L"该进程已退出（或 PID 已被复用），以下为最后已知信息。"));
        }
        ImGui::Separator();

        // Per-process details arrive async via DetailsProvider (ops JobQueue).
        if (alive) MaybeRequestDetails(ctx, *cur);

        ImGui::BeginChild("##detailscroll", ImVec2(0.0f, ImGui::GetContentRegionAvail().y));

        auto field = [](const wchar_t* label, const std::wstring& value) {
            ImGui::TextDisabled("%s", U8(label));
            ImGui::SameLine(110.0f);
            ImGui::TextWrapped("%s", value.empty() ? U8(L"—") : U8(value));
        };

        field(L"路径", p.path);
        const ui::FileMeta* meta = nullptr;
        if (!p.path.empty()) {
            ui::FileMetaCache& cache = ui::FileMetaCacheForThread();
            if (cache.Find(p.path) == nullptr) cache.Store(p.path, ui::QueryFileMeta(p.path));
            meta = cache.Find(p.path);
        }
        field(L"描述", meta != nullptr ? meta->description : std::wstring());
        field(L"公司", meta != nullptr ? meta->company : std::wstring());
        field(L"版本", meta != nullptr ? meta->version : std::wstring());
        DrawSignatureField(ctx, p, alive);
        DrawCmdLineField(ctx, p, alive);
        field(L"用户名", alive ? DrawUserNameField(ctx, p) : std::wstring(L"—"));
        field(L"GDI / USER 对象", alive ? DrawGuiObjectsField(ctx, p) : std::wstring(L"—"));

        // Parent process: show name and allow jumping (by ProcKey in this snapshot).
        const ProcInfo* parent = p.parentPid != 0 ? FindByPid(snap, p.parentPid) : nullptr;
        ImGui::TextDisabled("%s", U8(L"父进程"));
        ImGui::SameLine(110.0f);
        if (parent != nullptr) {
            ImGui::TextUnformatted(U8(Fmt(L"{} ({})", parent->name, parent->key.pid)));
            if (ImGui::SmallButton(U8(L"跳转到父进程"))) {
                selected_ = parent->key;
                lost_ = false;
                lastKnown_ = *parent;
                detailKey_ = ProcKey{};  // re-request details for the new selection
                Ui().selectedKey = selected_;
            }
        } else {
            ImGui::TextUnformatted(U8(Fmt(L"{}（不在当前快照中）", p.parentPid)));
        }

        field(L"窗口标题", p.windowTitle);
        field(L"会话 ID", Fmt(L"{}", p.sessionId));
        DrawControlSection(ctx, p, alive);
        DrawModulesSection(ctx, p, alive);
        ImGui::EndChild();
    }

    // ---- F4#2: 进程控制信息缓存（GetProcessControlInfo 走 jobs，2 秒新鲜度） ----
    struct CtrlSlot {
        std::mutex mu;
        bool ready = false;
        bool ok = false;
        double requestedAt = 0.0;  // ImGui::GetTime() of last request (staleness)
        ops::ProcessControlInfo info;
        std::wstring err;
    };

    // Returns a COPY under the slot lock (the worker may refresh the slot while
    // this frame renders — never hand out a pointer into the shared cell).
    // V15-P2-2: tri-state query result — Pending (job in flight), Ready (values
    // valid), Failed (ops reported honestly via err, e.g. 无读取权限). The UI must
    // never render a fake "未知（类 0x0）" for a failed query.
    enum class CtrlQuery { Pending, Ready, Failed };
    CtrlQuery GetCtrlInfo(const ProcKey& key, ops::ProcessControlInfo* out,
                          std::wstring* err) {
        EnsureCtrlInfo(key);
        const auto it = ctrl_.find(key);
        if (it == ctrl_.end()) return CtrlQuery::Pending;
        std::lock_guard<std::mutex> lock(it->second->mu);
        if (!it->second->ready) return CtrlQuery::Pending;
        if (!it->second->err.empty()) {  // 契约：err 非空 = 查询失败（诚实）
            if (err != nullptr) *err = it->second->err;
            return CtrlQuery::Failed;
        }
        if (out != nullptr) *out = it->second->info;
        return CtrlQuery::Ready;
    }

    void EnsureCtrlInfo(const ProcKey& key) {
        const double now = ImGui::GetTime();
        const auto it = ctrl_.find(key);
        if (it != ctrl_.end()) {
            std::lock_guard<std::mutex> lock(it->second->mu);
            if (it->second->ready && !ctrlInfoDirty_ &&
                now - it->second->requestedAt < 2.0) {
                return;
            }
        }
        if (ctrlBusy_) return;  // 单飞：等待上一个查询落地
        ctrlInfoDirty_ = false;
        std::shared_ptr<CtrlSlot> slot;
        if (it != ctrl_.end()) {
            slot = it->second;
        } else {
            slot = std::make_shared<CtrlSlot>();
            ctrl_.emplace(key, slot);
        }
        {
            std::lock_guard<std::mutex> lock(slot->mu);
            slot->requestedAt = now;
            slot->ready = false;
        }
        ctrlBusy_ = true;
        std::shared_ptr<AppContext> app = Ui().liveCtx;
        std::shared_ptr<CtrlSlot> capture = slot;
        if (!app || app->jobs.Submit([app, capture, key] {
                std::wstring err;
                ops::ProcessControlInfo info = ops::GetProcessControlInfo(key, &err);
                std::lock_guard<std::mutex> lock(capture->mu);
                capture->info = info;
                capture->err = std::move(err);
                capture->ok = true;  // 失败也有 err，ok 表示查询已完成
                capture->ready = true;
            }) == 0) {
            ctrlBusy_ = false;  // 队列已停（退出中）：下次重试
            return;
        }
        ctrlPending_ = slot;
    }

    // Polls the in-flight control-info query (single slot) — keeps the map clean
    // and resets the single-flight flag once the job lands.
    void PollCtrlInfo() {
        if (!ctrlPending_) return;
        std::lock_guard<std::mutex> lock(ctrlPending_->mu);
        if (ctrlPending_->ready) {
            ctrlPending_.reset();
            ctrlBusy_ = false;
        }
    }

    void DrawControlSection(AppContext& ctx, const ProcInfo& p, bool alive) {
        (void)ctx;
        ImGui::Separator();
        ImGui::TextUnformatted(U8(L"控制"));
        if (!alive) {
            ImGui::TextDisabled("%s", U8(L"进程已退出，无法查询"));
            return;
        }
        PollCtrlInfo();
        EnsureCtrlInfo(p.key);
        ops::ProcessControlInfo ci;
        std::wstring ciErr;
        auto field2 = [](const std::wstring& label, const std::wstring& value) {
            ImGui::TextDisabled("%s", U8(label));
            ImGui::SameLine(110.0f);
            ImGui::TextWrapped("%s", U8(value));
        };
        // V15-P2-2: Pending 与 Failed 分开呈现 —— 失败显示 err，绝不渲染
        // "未知（类 0x0）"这类把零值当数据的输出。失败 2 秒新鲜度到期自动重试。
        const CtrlQuery st = GetCtrlInfo(p.key, &ci, &ciErr);
        if (st == CtrlQuery::Pending) {
            field2(L"优先级", L"查询中…");
            field2(L"亲和性", L"查询中…");
            return;
        }
        if (st == CtrlQuery::Failed) {
            ImGui::TextColored(ColWarn(), "%s", U8(Fmt(L"查询失败：{}", ciErr)));
            return;
        }
        ops::ProcPriority pr = ops::ProcPriority::Normal;
        field2(L"优先级", ui3::PriorityFromWin32(ci.priorityClass, &pr)
                              ? ui3::PriorityLabel(pr)
                              : Fmt(L"未知（类 0x{:X}）", ci.priorityClass));
        field2(L"亲和性", ui3::AffinitySummary(ci.affinityMask));
        std::wstring suspText = L"未知";
        if (ci.suspendedAvail) suspText = ci.suspended ? L"已挂起" : L"运行中";
        field2(L"挂起状态", suspText);
        if (!ci.suspendedAvail) {
            ImGui::TextDisabled("%s", U8(L"（挂起检测不可用：无读取权限）"));
        }
    }

    // ---- F4#2: 亲和性模态（每逻辑核复选框 + 全选/全不选 + 确定，走 jobs） -------
    struct AffinityModal {
        bool openRequested = false;
        ProcKey key;
        uint32_t pid = 0;
        std::wstring name;
        std::vector<char> sel;
        uint64_t systemMask = 0;
        bool inited = false;
    };

    void RequestAffinityModal(const ProcInfo& p) {
        aff_ = AffinityModal{};
        aff_.openRequested = true;
        aff_.key = p.key;
        aff_.pid = p.key.pid;
        aff_.name = p.name;
    }

    void DrawAffinityModal(AppContext& ctx) {
        (void)ctx;  // submission goes through Ui().liveCtx (ExecuteConfirmedAction)
        if (!aff_.openRequested && !ImGui::IsPopupOpen("##affinity")) return;
        constexpr char kPopup[] = "##affinity";
        if (aff_.openRequested) {
            ImGui::OpenPopup(kPopup);
            aff_.openRequested = false;
            aff_.inited = false;
        }
        if (!ImGui::IsPopupOpen(kPopup)) return;
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 0.0f), ImVec2(420.0f, 480.0f));
        if (!ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_None)) return;

        ImGui::TextUnformatted(U8(Fmt(L"设置 {} (PID {}) 的处理器亲和性", aff_.name, aff_.pid)));
        ImGui::Separator();

        PollCtrlInfo();
        ops::ProcessControlInfo cinfo;
        std::wstring affErr;
        if (!aff_.inited) {
            const CtrlQuery st = GetCtrlInfo(aff_.key, &cinfo, &affErr);
            if (st == CtrlQuery::Ready && cinfo.systemAffinityMask != 0) {
                aff_.systemMask = cinfo.systemAffinityMask;
                const std::vector<int> cpus =
                    ui3::AffinityCpuList(cinfo.affinityMask & cinfo.systemAffinityMask);
                aff_.sel.assign(64, 0);
                for (int cpu : cpus) aff_.sel[static_cast<size_t>(cpu)] = 1;
                aff_.inited = true;
            }
        }
        if (!aff_.inited) {
            // V15-P2-3: Pending 与失败态分开 —— 失败显示 err 并允许"重试"，
            // 不再永远停在"正在查询当前亲和性…"。掩码有效但读不到（0）同属失败。
            const CtrlQuery st = GetCtrlInfo(aff_.key, &cinfo, &affErr);
            if (st == CtrlQuery::Pending) {
                ImGui::TextDisabled("%s", U8(L"正在查询当前亲和性…（无读取权限时无法设置）"));
            } else {
                const std::wstring reason =
                    st == CtrlQuery::Failed
                        ? affErr
                        : std::wstring(L"无法读取系统亲和性掩码（权限不足或进程已退出）");
                ImGui::TextColored(ColWarn(), "%s", U8(Fmt(L"查询失败：{}", reason)));
                if (ImGui::Button(U8(L"重试"))) {
                    ctrlInfoDirty_ = true;  // 强制下次 EnsureCtrlInfo 重新查询
                }
                ImGui::SameLine();
            }
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(0);
            if (ImGui::Button(U8(L"取消"), ImVec2(120.0f, 0.0f))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        if (ImGui::Button(U8(L"全选"))) {
            for (int cpu : ui3::AffinityCpuList(aff_.systemMask)) {
                aff_.sel[static_cast<size_t>(cpu)] = 1;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(U8(L"全不选"))) {
            std::fill(aff_.sel.begin(), aff_.sel.end(), static_cast<char>(0));
        }
        int chosen = 0;
        const std::vector<int> sysCpus = ui3::AffinityCpuList(aff_.systemMask);
        const size_t cols = 8;  // 每行 8 个复选框（64 核网格化）
        for (size_t n = 0; n < sysCpus.size(); ++n) {
            const int cpu = sysCpus[n];
            if (n % cols != 0) ImGui::SameLine();
            ImGui::PushID(cpu);
            bool on = aff_.sel[static_cast<size_t>(cpu)] != 0;
            if (ImGui::Checkbox(U8(Fmt(L"CPU {}", cpu)), &on)) {
                aff_.sel[static_cast<size_t>(cpu)] = on ? 1 : 0;
            }
            ImGui::PopID();
        }
        for (char c : aff_.sel) chosen += c != 0 ? 1 : 0;
        ImGui::TextDisabled("%s", U8(Fmt(L"已选 {} / {} 个逻辑核（掩码须落在系统允许组内）",
                                         chosen, sysCpus.size())));
        ImGui::Separator();
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(0);
        if (ImGui::Button(U8(L"取消"), ImVec2(120.0f, 0.0f))) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        ImGui::BeginDisabled(chosen == 0);
        if (ImGui::Button(U8(L"确定"), ImVec2(120.0f, 0.0f)) && chosen > 0) {
            uint64_t mask = 0;
            for (int cpu : sysCpus) {
                if (aff_.sel[static_cast<size_t>(cpu)] != 0) mask |= (1ull << cpu);
            }
            ui::ConfirmRequest req;
            req.kind = ui::ConfirmKind::SetAffinity;
            req.key = aff_.key;
            req.pid = aff_.pid;
            req.name = aff_.name;
            req.affinityMask = mask;
            ui::ExecuteConfirmedAction(Ui().liveCtx, req);
            ctrlInfoDirty_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    void MaybeRequestDetails(AppContext& ctx, const ProcInfo& p) {
        const bool newSelection = detailKey_ != p.key;
        const bool nothingYet = ctx.details->Peek(p.key) == nullptr;
        const bool tickAdvanced = lastDetailTick_ != lastTickId_;
        if (newSelection || (nothingYet && tickAdvanced)) {
            ctx.details->Request(p.key, p.path, kDetailKinds);
            detailKey_ = p.key;
            lastDetailTick_ = lastTickId_;
        }
    }

    void DrawSignatureField(AppContext& ctx, const ProcInfo& p, bool alive) const {
        const ops::ProcessDetails* d = ctx.details->Peek(p.key);
        const wchar_t* text = L"查询中…";
        ImVec4 color(0.6f, 0.6f, 0.6f, 1.0f);
        if (!alive) {
            // P2-2 (F2 review): an exited process is never queried — showing
            // "查询中…" forever would be dishonest.
            text = L"已退出，无法查询";
            color = ColWarn();
        } else if (d != nullptr && d->sigResolved) {
            switch (d->sig) {
                case ops::SigState::Valid:
                    text = L"有效签名";
                    color = ColDone();
                    break;
                case ops::SigState::Unsigned:
                    text = L"未签名";
                    color = ColWarn();
                    break;
                case ops::SigState::Invalid:
                    text = L"签名无效";
                    color = ColFail();
                    break;
                case ops::SigState::NoCheck:
                    text = L"未查询";
                    break;
                case ops::SigState::Unknown:
                    text = L"未知";
                    break;
            }
        }
        ImGui::TextDisabled("%s", U8(L"签名"));
        ImGui::SameLine(110.0f);
        ImGui::TextColored(color, "%s", U8(text));
    }

    void DrawCmdLineField(AppContext& ctx, const ProcInfo& p, bool alive) const {
        const ops::ProcessDetails* d = ctx.details->Peek(p.key);
        std::wstring value = L"—";
        if (!alive) {
            value = L"已退出，无法查询";
        } else if (d != nullptr) {
            value = d->cmdLineAvail ? d->cmdLine : L"不可用（无读取权限）";
        }
        ImGui::TextDisabled("%s", U8(L"命令行"));
        ImGui::SameLine(110.0f);
        ImGui::TextWrapped("%s", U8(value));
    }

    std::wstring DrawUserNameField(AppContext& ctx, const ProcInfo& p) const {
        const ops::ProcessDetails* d = ctx.details->Peek(p.key);
        if (d != nullptr && d->userNameResolved) return d->userName;
        return L"—";
    }

    std::wstring DrawGuiObjectsField(AppContext& ctx, const ProcInfo& p) const {
        const ops::ProcessDetails* d = ctx.details->Peek(p.key);
        if (d != nullptr && d->guiResolved) return Fmt(L"{} / {}", d->gdiObjects, d->userObjects);
        return L"—";
    }

    // ---- F4: module list -------------------------------------------------------
    // Signature verification runs on the ops worker; results are cached per module
    // path (DriverPage slot pattern). -1 = in flight, else ops::SigState as int.
    void EnsureModuleSig(const std::wstring& path) {
        if (path.empty()) return;
        auto it = moduleSigs_.find(path);
        if (it != moduleSigs_.end()) return;
        std::shared_ptr<std::atomic<int>> slot = std::make_shared<std::atomic<int>>(-1);
        moduleSigs_.emplace(path, slot);
        std::shared_ptr<AppContext> app = Ui().liveCtx;
        if (!app || app->jobs.Submit([app, slot, path] {
                slot->store(static_cast<int>(ops::VerifyFileSignature(path)));
            }) == 0) {
            moduleSigs_.erase(path);  // queue down (teardown): keep the row honest
        }
    }

    void DrawModulesSection(AppContext& ctx, const ProcInfo& p, bool alive) {
        if (!alive) return;  // P2-2: dead process — nothing honest to list here
        const ops::ProcessDetails* d = ctx.details->Peek(p.key);
        const bool entryDone = d != nullptr && d->sigResolved;  // job completed overall
        const bool resolved = d != nullptr && d->modulesResolved;
        const ui::ModuleSectionState state =
            ui::DecideModuleSection(entryDone, resolved, ctx.elevated,
                                    d != nullptr ? d->modules.size() : 0);

        ImGui::Separator();
        if (state != ui::ModuleSectionState::Ready) {
            ImGui::TextColored(ColWarn(), "%s", U8(ui::ModuleSectionText(state)));
            return;
        }
        ImGui::TextUnformatted(U8(Fmt(L"模块（{} 个）", d->modules.size())));
        ImGui::TextDisabled("%s",
                            U8(L"点击模块校验文件签名；未签名或签名无效的模块可能是风险项"));
        ImGui::BeginChild("##modulelist", ImVec2(0.0f, 200.0f), ImGuiChildFlags_Borders);
        for (const std::wstring& mod : d->modules) {
            const std::wstring shown = mod.empty() ? std::wstring(L"—") : mod;
            ImGui::PushID(static_cast<int>(&mod - d->modules.data()));
            if (ImGui::Selectable(U8(TruncateModulePath(shown)))) {
                EnsureModuleSig(mod);
            }
            if (ImGui::IsItemHovered() && shown.size() > 90) {
                ImGui::SetTooltip("%s", U8(mod));
            }
            // badge column
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
            auto it = moduleSigs_.find(mod);
            const ui::ModuleBadge badge =
                it == moduleSigs_.end() ? ui::ModuleBadge::Pending
                                        : ui::ModuleBadgeForSig(it->second->load());
            switch (badge) {
                case ui::ModuleBadge::Valid:
                    ImGui::TextColored(ColDone(), "%s", U8(ModuleBadgeLabel(badge)));
                    break;
                case ui::ModuleBadge::Invalid:
                case ui::ModuleBadge::Unsigned:
                    ImGui::TextColored(ColWarn(), "%s", U8(ModuleBadgeLabel(badge)));
                    break;
                default:
                    ImGui::TextDisabled("%s", U8(ModuleBadgeLabel(badge)));
                    break;
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    // ---- state ----------------------------------------------------------------
    static constexpr int kColCount = static_cast<int>(ui::SortColumn::Count) + 2;

    // Fixed columns: pixel widths. Name (index 0) and description (index 12) are
    // stretch columns, so their slot holds the stretch weight instead.
    static constexpr float kDefaultWidths[kColCount] = {
        2.0f,    // name (stretch weight)
        64.0f,   // pid
        72.0f,   // cpu
        110.0f,  // mem (private working set)
        96.0f,   // commit
        100.0f,  // disk
        100.0f,  // net
        96.0f,   // hard faults
        72.0f,   // handles
        72.0f,   // threads
        104.0f,  // context switches
        96.0f,   // badges
        0.6f,    // description (stretch weight)
    };

    bool loaded_ = false;
    bool sortReflected_ = false;
    bool rebuildNeeded_ = true;
    ui::SortColumn sortColumn_ = ui::SortColumn::Name;
    bool sortDesc_ = false;
    float widths_[kColCount] = {};  // fixed: px width; stretch: weight
    double lastWidthSave_ = 0.0;
    uint64_t appliedResetGen_ = 0;  // H-A: 已应用的「恢复默认列宽」世代号

    uint64_t lastTickId_ = 0;
    std::string filterUtf8_;
    std::wstring filterWide_;
    std::wstring lastFilterWide_;

    ProcKey selected_{};
    bool selectedValid_ = false;
    bool lost_ = false;
    ProcInfo lastKnown_{};  // frozen copy shown when the process exits
    ProcKey detailKey_{};
    uint64_t lastDetailTick_ = 0;
    int metaBudget_ = 3;
    // F4: per-module-path signature verification cache (ops worker results).
    std::unordered_map<std::wstring, std::shared_ptr<std::atomic<int>>> moduleSigs_;

    // F4#1/#4: 系统进程区分模式（0 关 / 1 高亮 / 2 只看用户）与树形视图开关。
    int sysDistMode_ = 0;
    bool treeMode_ = false;
    // 行序（平铺 depth=0；树形 DFS 行序带深度）+ 与 rows_ 对齐的类别（着色用）。
    std::vector<ui3::TreeRow> rows_;
    std::vector<ui3::ProcKind> kinds_;

    // F4#2: 进程控制信息缓存（jobs 单飞查询；ctrlInfoDirty_ 在操作提交后失效）。
    std::unordered_map<ProcKey, std::shared_ptr<CtrlSlot>> ctrl_;
    std::shared_ptr<CtrlSlot> ctrlPending_;
    bool ctrlBusy_ = false;
    bool ctrlInfoDirty_ = false;
    AffinityModal aff_;
};

// ===========================================================================
// PerfPage: four ImPlot quadrants (CPU / memory / disk / network), 120 ticks.
// ===========================================================================

int FmtBytesAxis(double value, char* buff, int size, void* /*user_data*/) {
    if (value != value) {  // NaN
        return snprintf(buff, static_cast<size_t>(size), "%s", "—");
    }
    const std::string s = WideToUtf8(FormatBytes(value <= 0.0 ? 0u : static_cast<uint64_t>(value)));
    return snprintf(buff, static_cast<size_t>(size), "%s", s.c_str());
}

void PlotRing(const char* label, const Ring& r, bool noLegend) {
    if (r.Count() <= 0) return;
    ImPlotSpec spec;
    spec.Offset = r.Offset();
    if (noLegend) spec.Flags = ImPlotItemFlags_NoLegend;
    // values-only overload: x = i * xscale, ring offset maps newest sample to the right.
    ImPlot::PlotLine(label, r.v.data(), r.Count(), 1.0, 0.0, spec);
}

class PerfPage final : public IPage {
public:
    const wchar_t* Id() const override { return L"perf"; }
    const wchar_t* Title() const override { return L"性能"; }

    void Draw(AppContext& ctx) override {
        const PerfHistory& h = Hist();
        if (h.lastTick == 0) {
            ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.65f, 1.0f), "%s", U8(L"等待采集数据…"));
            return;
        }

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        // P3 任务二：页面区宽度 > 1200 时两列网格，否则单列铺满（块按序流入）。
        const bool twoCol = ImGui::GetWindowWidth() > 1200.0f;
        const float cellW = twoCol ? (avail.x - spacing) * 0.5f : avail.x;
        const float barsH = ImGui::GetFrameHeightWithSpacing() * 2.0f + 8.0f;
        const float plotH = std::max(120.0f, (avail.y - barsH - spacing) * 0.5f);

        // 每块 = BeginGroup{标题栏复选框 + 内容}：组边界取块内最大 y，两列并排时
        // 行高随较高块推进，另一列较矮也不会与下一行重叠。
        // 显隐 cfg（perfShow*，默认除 CtxSwitch 外全开）：勾选即写 cfg，随退出持久化。
        auto beginCell = [&](int i) {
            if (twoCol && (i & 1) != 0) ImGui::SameLine();
            ImGui::BeginGroup();
        };
        auto endCell = [] { ImGui::EndGroup(); };
        auto visible = [&](const wchar_t* key, bool def, const wchar_t* title) {
            bool show = ctx.cfg.GetBool(key, def);
            if (ImGui::Checkbox(U8(title), &show)) ctx.cfg.SetBool(key, show);
            return show;
        };

        beginCell(0);
        if (visible(L"perfShowCpu", true, L"CPU")) DrawCpu(cellW, plotH, h);
        endCell();
        beginCell(1);
        if (visible(L"perfShowPerCore", true, L"CPU 每核")) DrawPerCore(cellW, plotH, h);
        endCell();
        beginCell(2);
        if (visible(L"perfShowMem", true, L"内存")) DrawMemory(cellW, plotH, h);
        endCell();
        beginCell(3);
        if (visible(L"perfShowDisk", true, L"磁盘")) DrawDisk(cellW, plotH, h);
        endCell();
        beginCell(4);
        if (visible(L"perfShowNet", true, L"网络")) DrawNet(cellW, plotH, h);
        endCell();
        beginCell(5);
        if (visible(L"perfShowHardFaults", true, L"硬故障/s")) DrawHardFaults(cellW, plotH, h);
        endCell();
        beginCell(6);
        if (visible(L"perfShowCtxSwitch", false, L"上下文切换/s")) DrawCtxSwitch(cellW, plotH, h);
        endCell();
        beginCell(7);
        if (visible(L"perfShowGpu", true, L"GPU")) DrawGpuBlockBody(*Ui().snap);
        endCell();

        const SystemInfo& sys = Ui().snap ? Ui().snap->sys : SystemInfo{};
        DrawMemoryBars(sys);
        ui3::DrawAlertControls(ctx);  // phase-3: threshold alert controls (additive row)
        DrawCsvControls(ctx, sys);    // F4#7: 性能 CSV 记录开关
    }

private:
    static bool BeginPlotBox(const char* id, const ImVec2& size) {
        return ImPlot::BeginPlot(id, size);
    }

    static void DrawCpu(float w, float plotH, const PerfHistory& hist) {
        if (!BeginPlotBox("##cpu", ImVec2(w, plotH))) return;
        ImPlot::SetupLegend(ImPlotLocation_NorthEast);
        ImPlot::SetupAxis(ImAxis_X1, U8(L"秒"));
        ImPlot::SetupAxis(ImAxis_Y1, U8(L"CPU"));
        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, static_cast<double>(kHistCap - 1), ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 100.0, ImPlotCond_Always);
        ImPlot::SetupAxisFormat(ImAxis_Y1, "%g%%");
        ImPlot::SetupFinish();
        PlotRing(U8(L"总量"), hist.cpuTotal, false);
        for (const Ring& core : hist.cores) PlotRing("##core", core, true);
        ImPlot::EndPlot();
    }

    static void DrawMemory(float w, float plotH, const PerfHistory& hist) {
        if (!BeginPlotBox("##mem", ImVec2(w, plotH))) return;
        ImPlot::SetupLegend(ImPlotLocation_NorthEast);
        ImPlot::SetupAxis(ImAxis_X1, U8(L"秒"));
        ImPlot::SetupAxis(ImAxis_Y1, U8(L"内存"));
        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, static_cast<double>(kHistCap - 1), ImPlotCond_Always);
        ImPlot::SetupAxisFormat(ImAxis_Y1, &FmtBytesAxis);
        ImPlot::SetupFinish();
        PlotRing(U8(L"可用物理"), hist.physAvail, false);
        PlotRing(U8(L"提交"), hist.commit, false);
        ImPlot::EndPlot();
    }

    static void DrawDisk(float w, float plotH, const PerfHistory& hist) {
        if (!BeginPlotBox("##disk", ImVec2(w, plotH))) return;
        ImPlot::SetupLegend(ImPlotLocation_NorthEast);
        ImPlot::SetupAxis(ImAxis_X1, U8(L"秒"));
        ImPlot::SetupAxis(ImAxis_Y1, U8(L"磁盘"));
        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, static_cast<double>(kHistCap - 1), ImPlotCond_Always);
        ImPlot::SetupAxisFormat(ImAxis_Y1, &FmtBytesAxis);
        ImPlot::SetupFinish();
        PlotRing(U8(L"读取"), hist.diskRead, false);
        PlotRing(U8(L"写入"), hist.diskWrite, false);
        ImPlot::EndPlot();
    }

    static void DrawNet(float w, float plotH, const PerfHistory& hist) {
        if (!BeginPlotBox("##net", ImVec2(w, plotH))) return;
        ImPlot::SetupLegend(ImPlotLocation_NorthEast);
        ImPlot::SetupAxis(ImAxis_X1, U8(L"秒"));
        ImPlot::SetupAxis(ImAxis_Y1, U8(L"网络"));
        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, static_cast<double>(kHistCap - 1), ImPlotCond_Always);
        ImPlot::SetupAxisFormat(ImAxis_Y1, &FmtBytesAxis);
        ImPlot::SetupFinish();
        PlotRing(U8(L"接收"), hist.netRecv, false);
        PlotRing(U8(L"发送"), hist.netSend, false);
        ImPlot::EndPlot();
    }

    // ---- P3 任务二：新增图表 --------------------------------------------------
    // 不可用计数器的诚实空态（硬故障/s 在部分机器上 PDH 无此计数器；上下文切换/s
    // 在兼容模式下无每进程数据可聚合）。
    static void DrawCounterUnavailable() {
        ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.65f, 1.0f), "%s", U8(L"本机此计数器不可用"));
    }

    static void DrawPerCore(float w, float plotH, const PerfHistory& hist) {
        if (!BeginPlotBox("##percore", ImVec2(w, plotH))) return;
        ImPlot::SetupLegend(ImPlotLocation_NorthEast);
        ImPlot::SetupAxis(ImAxis_X1, U8(L"秒"));
        ImPlot::SetupAxis(ImAxis_Y1, U8(L"CPU 每核"));
        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, static_cast<double>(kHistCap - 1), ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 100.0, ImPlotCond_Always);
        ImPlot::SetupAxisFormat(ImAxis_Y1, "%g%%");
        ImPlot::SetupFinish();
        for (const Ring& core : hist.cores) PlotRing("##core", core, true);
        ImPlot::EndPlot();
    }

    static void DrawHardFaults(float w, float plotH, const PerfHistory& hist) {
        if (!hist.hardFaults.hasData) {  // 从未收到有效样本：诚实空态而非空图
            DrawCounterUnavailable();
            return;
        }
        if (!BeginPlotBox("##hardfaults", ImVec2(w, plotH))) return;
        ImPlot::SetupLegend(ImPlotLocation_NorthEast);
        ImPlot::SetupAxis(ImAxis_X1, U8(L"秒"));
        ImPlot::SetupAxis(ImAxis_Y1, U8(L"硬故障/s"));
        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, static_cast<double>(kHistCap - 1), ImPlotCond_Always);
        ImPlot::SetupAxisFormat(ImAxis_Y1, "%g");
        ImPlot::SetupFinish();
        PlotRing(U8(L"硬故障"), hist.hardFaults, false);
        ImPlot::EndPlot();
    }

    static void DrawCtxSwitch(float w, float plotH, const PerfHistory& hist) {
        if (!hist.ctxSwitch.hasData) {
            DrawCounterUnavailable();
            return;
        }
        if (!BeginPlotBox("##ctxswitch", ImVec2(w, plotH))) return;
        ImPlot::SetupLegend(ImPlotLocation_NorthEast);
        ImPlot::SetupAxis(ImAxis_X1, U8(L"秒"));
        ImPlot::SetupAxis(ImAxis_Y1, U8(L"上下文切换/s"));
        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, static_cast<double>(kHistCap - 1), ImPlotCond_Always);
        ImPlot::SetupAxisFormat(ImAxis_Y1, "%g");
        ImPlot::SetupFinish();
        PlotRing(U8(L"切换"), hist.ctxSwitch, false);
        ImPlot::EndPlot();
    }

    // F4#7: 性能 CSV 记录（开始 -> captures 目录；停止 -> toast 路径 + 打开文件夹）。
    static void DrawCsvControls(AppContext& ctx, const SystemInfo& sys) {
        ui3::PerfCsvRecorder& rec = ui3::SharedPerfCsv();
        ImGui::Separator();
        if (!rec.Active()) {
            if (ImGui::Button(U8(L"记录 CSV"))) {
                std::wstring err;
                const size_t cores = sys.perCorePercent.size();
                if (rec.Start(ui3::PerfCsvDefaultDir(), cores, &err)) {
                    ctx.cfg.SetBool(L"perfCsvWanted", true);
                    PushToast(Notification::Kind::JobDone,
                              Fmt(L"已开始记录性能 CSV：{}", rec.Path()));
                } else {
                    PushToast(Notification::Kind::JobFailed,
                              err.empty() ? std::wstring(L"启动 CSV 记录失败") : err);
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s",
                                  U8(L"按当前刷新间隔把系统级指标追加写入 %LOCALAPPDATA%"
                                     L"\\SuperTaskMgr\\captures\\perf_*.csv（含每核 CPU、内存、"
                                     L"磁盘、网络、GPU 利用率；不记录任何进程级数据）"));
            }
            return;
        }
        ImGui::TextColored(ColDone(), "%s", U8(L"● 记录中"));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", U8(TruncateModulePath(rec.Path())));
        if (ImGui::IsItemHovered() && rec.Path().size() > 60) {
            ImGui::SetTooltip("%s", U8(rec.Path()));
        }
        ImGui::SameLine();
        if (ImGui::Button(U8(L"停止记录"))) {
            const std::wstring path = rec.Path();
            rec.Stop();
            ctx.cfg.SetBool(L"perfCsvWanted", false);
            PushToast(Notification::Kind::JobDone, Fmt(L"已停止记录，文件保存于 {}", path));
        }
        ImGui::SameLine();
        if (ImGui::Button(U8(L"打开文件夹"))) {
            const std::wstring arg = L"/select,\"" + rec.Path() + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
        }
    }

    static void DrawMemoryBars(const SystemInfo& sys) {
        char overlay[64];
        if (sys.physTotal > 0) {
            const double frac = static_cast<double>(sys.physTotal - sys.physAvail) /
                                static_cast<double>(sys.physTotal);
            const std::wstring label = Fmt(L"{} / {}（{:.1f}%）",
                                           FormatBytes(sys.physTotal - sys.physAvail),
                                           FormatBytes(sys.physTotal), frac * 100.0);
            snprintf(overlay, sizeof(overlay), "%s", WideToUtf8(label).c_str());
            ImGui::ProgressBar(static_cast<float>(frac), ImVec2(-FLT_MIN, 0.0f), overlay);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", U8(L"物理内存"));
        }
        if (sys.commitLimit > 0) {
            const double frac = static_cast<double>(sys.commitTotal) /
                                static_cast<double>(sys.commitLimit);
            const std::wstring label = Fmt(L"{} / {}（{:.1f}%）", FormatBytes(sys.commitTotal),
                                           FormatBytes(sys.commitLimit), frac * 100.0);
            snprintf(overlay, sizeof(overlay), "%s", WideToUtf8(label).c_str());
            ImGui::ProgressBar(static_cast<float>(frac), ImVec2(-FLT_MIN, 0.0f), overlay);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", U8(L"提交"));
        }
        // P3 任务一：「一键优化」入口 —— 打开 MemCleanup 确认模态
        // （DrawConfirmDialogs 的 MemCleanup kind，每帧渲染模式）。
        if (ImGui::Button(U8(L"一键优化…"))) RequestConfirmMemCleanup();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "%s", U8(L"按当前私有工作集挑选高占用进程（默认勾选前 5），批量提示系统释放"
                          L"工作集内存；可同时清理系统待机缓存。系统关键进程不可选。"));
        }
    }

    // P1-5 (F2 review): GPU block. P3 任务二起标题栏复选框在网格单元里，
    // 这里只剩内容体（GPU 表格 + 进程 Top5）。数据每 2 s 产出一批。
    static std::wstring GpuUtilText(double v) {
        return v == v ? Fmt(L"{:.1f}%", v) : std::wstring(L"—");  // NaN = unavailable
    }

    // Aggregates the (luid, pid) rows per pid: utilization summed and clamped to
    // 100, memory summed; rows whose utilization is unavailable still contribute
    // their memory. Returns at most `top` entries, highest utilization first.
    struct GpuProcRow {
        const ProcInfo* proc = nullptr;
        double utilPercent = kUnavail;
        uint64_t dedicated = 0, shared = 0;
        bool hasUtil = false;
    };
    static std::vector<GpuProcRow> TopGpuProcs(const Snapshot& snap, size_t top) {
        std::vector<GpuProcRow> merged;
        for (const GpuProcUsage& g : snap.gpuProcs) {
            GpuProcRow* row = nullptr;
            for (GpuProcRow& r : merged) {
                if (r.proc != nullptr && r.proc->key.pid == g.key.pid) { row = &r; break; }
            }
            if (row == nullptr) {
                merged.push_back(GpuProcRow{FindByPid(snap, g.key.pid), kUnavail, 0, 0, false});
                row = &merged.back();
            }
            if (g.utilPercent == g.utilPercent) {
                row->utilPercent = row->hasUtil ? row->utilPercent + g.utilPercent : g.utilPercent;
                row->hasUtil = true;
            }
            if (g.dedicatedBytes != kUnavailU64) row->dedicated += g.dedicatedBytes;
            if (g.sharedBytes != kUnavailU64) row->shared += g.sharedBytes;
        }
        for (GpuProcRow& r : merged) {
            if (r.hasUtil && r.utilPercent > 100.0) r.utilPercent = 100.0;
        }
        std::stable_sort(merged.begin(), merged.end(), [](const GpuProcRow& a, const GpuProcRow& b) {
            const double ua = a.hasUtil ? a.utilPercent : -1.0;
            const double ub = b.hasUtil ? b.utilPercent : -1.0;
            return ua > ub;
        });
        if (merged.size() > top) merged.resize(top);
        return merged;
    }

    static void DrawGpuBlockBody(const Snapshot& snap) {
        if (snap.sys.gpus.empty()) {
            // honest empty state: first tick not in yet, or no adapters present
            ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.65f, 1.0f), "%s",
                               U8(L"暂无 GPU 数据（等待采集，或本机无适配器）"));
            return;
        }
        if (ImGui::BeginTable("gpuadapters", 4, ImGuiTableFlags_RowBg |
                                                   ImGuiTableFlags_BordersInnerH |
                                                   ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn(U8(L"适配器"), ImGuiTableColumnFlags_WidthStretch, 2.4f);
            ImGui::TableSetupColumn(U8(L"利用率"), ImGuiTableColumnFlags_WidthFixed, 72.0f);
            ImGui::TableSetupColumn(U8(L"显存已用"), ImGuiTableColumnFlags_WidthFixed, 92.0f);
            ImGui::TableSetupColumn(U8(L"显存总量"), ImGuiTableColumnFlags_WidthFixed, 92.0f);
            ImGui::TableHeadersRow();
            for (const GpuAdapterInfo& a : snap.sys.gpus) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const std::wstring aname =
                    a.name.empty() ? std::wstring(L"—") : TruncateModulePath(a.name);
                ImGui::TextUnformatted(U8(aname));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(GpuUtilText(a.utilPercent)));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(a.memUsed != kUnavailU64
                                              ? FormatBytes(a.memUsed)
                                              : std::wstring(L"—")));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(a.memTotal != kUnavailU64
                                              ? FormatBytes(a.memTotal)
                                              : std::wstring(L"—")));
            }
            ImGui::EndTable();
        }
        const std::vector<GpuProcRow> top = TopGpuProcs(snap, 5);
        if (!top.empty()) {
            ImGui::TextDisabled("%s", U8(L"按进程 GPU 占用（Top 5，跨适配器合计）"));
            if (ImGui::BeginTable("gpuprocs", 4, ImGuiTableFlags_RowBg |
                                                     ImGuiTableFlags_BordersInnerH |
                                                     ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn(U8(L"进程"), ImGuiTableColumnFlags_WidthStretch, 2.4f);
                ImGui::TableSetupColumn(U8(L"利用率"), ImGuiTableColumnFlags_WidthFixed, 72.0f);
                ImGui::TableSetupColumn(U8(L"专用显存"), ImGuiTableColumnFlags_WidthFixed, 92.0f);
                ImGui::TableSetupColumn(U8(L"共享显存"), ImGuiTableColumnFlags_WidthFixed, 92.0f);
                ImGui::TableHeadersRow();
                for (const GpuProcRow& r : top) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const std::wstring name =
                        r.proc != nullptr ? Fmt(L"{} ({})", r.proc->name, r.proc->key.pid)
                                          : std::wstring(L"已退出进程");
                    ImGui::TextUnformatted(U8(name));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(U8(GpuUtilText(r.hasUtil ? r.utilPercent : kUnavail)));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(U8(r.dedicated > 0 ? FormatBytes(r.dedicated)
                                                              : std::wstring(L"—")));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(U8(r.shared > 0 ? FormatBytes(r.shared)
                                                           : std::wstring(L"—")));
                }
                ImGui::EndTable();
            }
        }
    }
};

// ===========================================================================
// Shell: toolbar / tabs / status bar.
// ===========================================================================

void DrainNotifications(AppContext& ctx) {
    std::vector<Notification> out;
    ctx.notes.Drain(&out);
    for (const Notification& n : out) {
        // P2-1 (F2 review): details-provider notes fire on every row selection —
        // high-frequency noise. They update the status bar only; toasts are
        // reserved for destructive/observable ops.
        const bool detailsNoise = n.text.rfind(L"进程详情", 0) == 0 ||
                                  n.text.rfind(L"进程签名", 0) == 0 ||
                                  n.text.rfind(L"无法读取进程详情", 0) == 0;
        if (detailsNoise) {
            Ui().lastNote = n.text;
            continue;
        }
        PushToast(n.kind, n.text);
    }
}

// H-A(Phase-6 接线): 选图 + 同步加载壁纸。
// 线程模型：WallpaperLoad 直接在 D3D11 立即上下文上创建纹理 —— 立即上下文
// 非线程安全，而本应用的渲染线程就是 UI 线程，走 jobs 会在 worker 上与渲染
// 并发使用同一上下文（需设备级同步才安全）。文件拷贝+解码+上传实测几十 ms，
// 故选择 UI 线程同步调用（实现最稳），期间置 wallpaperBusy 在状态行提示
// "加载中…"；阻塞结束后同帧清除，成败都以 toast 反馈。
void PickAndLoadWallpaper() {
    void* device = WallpaperBackendDevice();
    void* context = WallpaperBackendContext();
    if (device == nullptr || context == nullptr) {
        PushToast(Notification::Kind::JobFailed, L"渲染器未就绪，无法加载壁纸");
        return;
    }
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();  // 菜单消息属于主窗（UI 线程 == 窗口线程）
    ofn.lpstrFilter =
        L"图片文件 (*.png;*.jpg;*.jpeg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0"
        L"所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return;  // 用户取消（CommDlgExtendedError 忽略）
    if (!ui::IsSupportedImageExt(path)) {  // 过滤器之外的扩展名：诚实拒绝
        PushToast(Notification::Kind::Warn, Fmt(L"不支持的图片格式：{}", path));
        return;
    }
    Ui().wallpaperBusy = true;
    ui::WallpaperState st;
    const bool ok = ui::WallpaperLoad(device, context, path, &st);
    Ui().wallpaperBusy = false;
    if (ok) {
        PushToast(Notification::Kind::JobDone,
                  Fmt(L"壁纸已加载：{}（{}×{}）", st.sourcePath, st.width, st.height));
    } else {
        PushToast(Notification::Kind::JobFailed,
                  Fmt(L"壁纸加载失败：{}", st.error.empty() ? std::wstring(L"未知错误") : st.error));
    }
}

// H-A(Phase-6 接线): 「外观→自定义壁纸」控件组。外观菜单与 --smoke 离屏预览
// 窗口共用同一绘制函数（覆盖滑条/状态行/性能提示的渲染路径）。
void DrawAppearancePanel(AppContext& ctx) {
    const bool wpActive = ui::WallpaperActive();
    if (Ui().wallpaperBusy) {
        ImGui::TextColored(ColWarn(), "%s", U8(L"加载中…"));
    } else if (wpActive) {
        const ui::WallpaperState& ws = ui::WallpaperGet();
        ImGui::TextWrapped("%s",
                           U8(Fmt(L"已加载：{}（{}×{}）", ws.sourcePath, ws.width, ws.height)));
    } else {
        ImGui::TextDisabled("%s", U8(L"未加载（使用纯色背景）"));
    }
    if (ImGui::Button(U8(L"选择图片…"))) PickAndLoadWallpaper();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s",
                          U8(L"支持 png/jpg/jpeg/bmp/tga；图片会复制到 %LOCALAPPDATA%"
                             L"\\SuperTaskMgr\\wallpaper，下次启动自动恢复"));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!wpActive);
    if (ImGui::Button(U8(L"关闭壁纸"))) {
        ui::WallpaperClear();  // 释放纹理并删除持久化副本（下次启动不再恢复）
        PushToast(Notification::Kind::Info, L"已关闭自定义壁纸");
    }
    ImGui::EndDisabled();
    // 遮罩滑条实时生效：每帧绘制直接读 cfg（wallpaperMask，默认 0.45）。
    float mask = static_cast<float>(ctx.cfg.GetDouble(L"wallpaperMask", 0.45));
    if (ImGui::SliderFloat(U8(L"遮罩不透明度"), &mask, 0.0f, 0.85f, "%.2f")) {
        ctx.cfg.SetDouble(L"wallpaperMask", static_cast<double>(mask));
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", U8(L"壁纸上的黑色可读性遮罩（0=无遮罩，0.85=最深）"));
    }
    // 用户要求的性能提醒：常驻展示（H-B 契约文案）。
    ImGui::TextColored(ColWarn(), "%s", U8(ui::WallpaperPerfNotice()));
}

void DrawToolbar(AppContext& ctx, const Snapshot& snap) {
    (void)snap;
    const float barH = ImGui::GetFrameHeight() + 6.0f;
    ImGui::BeginChild("##toolbar", ImVec2(0.0f, barH), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar);
    int interval = static_cast<int>(ctx.cfg.GetInt(L"intervalMs", 1000));
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderInt(U8(L"刷新间隔"), &interval, 500, 5000, "%d ms")) {
        ctx.cfg.SetInt(L"intervalMs", interval);
        if (!Ui().paused) ctx.collect.SetInterval(static_cast<uint32_t>(interval));
    }
    if (!Ui().paused) {
        ImGui::SameLine();
        if (ImGui::Button(U8(L"暂停采集"))) {
            ctx.collect.Stop();
            Ui().paused = true;
            PushToast(Notification::Kind::Info, L"已暂停采集");
        }
    } else {
        ImGui::SameLine();
        if (ImGui::Button(U8(L"继续采集"))) {
            if (ctx.collect.Start(static_cast<uint32_t>(interval))) {
                Ui().paused = false;
                PushToast(Notification::Kind::Info, L"已恢复采集");
            } else {
                PushToast(Notification::Kind::JobFailed, L"恢复采集失败");
            }
        }
    }
    if (!ctx.elevated && ops::CanElevate()) {
        ImGui::SameLine();
        if (ImGui::Button(U8(L"以管理员身份重启"))) {
            // ShellExecuteExW(runas) blocks on the UAC dialog: run it on the ops
            // worker so the UI stays responsive while the prompt is up (phase-4 debt).
            SaveSessionFromCtx(ctx, nullptr);
            if (auto app = Ui().liveCtx) {
                app->jobs.Submit([app] {
                    if (ops::RelaunchAsAdmin(L"--relaunched")) {
                        app->wantExit = true;
                    } else {
                        Notification n;
                        n.kind = Notification::Kind::JobFailed;
                        n.text = L"提权重启失败或已取消";
                        app->notes.Push(std::move(n));
                    }
                });
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(U8(L"清理待机缓存"))) RequestConfirmPurgeStandby();

    // H-A: 「外观」菜单 —— 主题三态（即时 Apply + 写 cfg themeMode）与
    // 「恢复默认列宽」（软删除 + config.json 剔除 colW_*，toast 反馈）。
    ImGui::SameLine();
    if (ImGui::BeginMenu(U8(L"外观"))) {
        const ThemeMode cur =
            ThemeModeFromInt(ctx.cfg.GetInt(L"themeMode", 0));
        struct ModeItem { ThemeMode mode; const wchar_t* label; };
        static const ModeItem kModes[] = {
            {ThemeMode::Dark,   L"深色"},
            {ThemeMode::Light,  L"浅色"},
            {ThemeMode::System, L"跟随系统"},
        };
        for (const ModeItem& m : kModes) {
            if (ImGui::MenuItem(U8(m.label), nullptr, m.mode == cur)) {
                ctx.cfg.SetInt(L"themeMode", static_cast<int64_t>(m.mode));
                Theme::Apply(m.mode);
                PushToast(Notification::Kind::Info, Fmt(L"主题已切换：{}", m.label));
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(U8(L"恢复默认列宽"))) {
            // 键清单登记见 ui3::ColWidthCfgKeys()（app/ui3/ThemeCfg.h，与
            // ProcessesPage::PersistWidths 写入一一对应）。
            ui3::SoftDeleteColWidthKeys(ctx.cfg);
            const int removed = ui3::StripColWidthKeysFromFile(ConfigPath(), false);
            ++Ui().colWidthResetGen;  // 进程页下一帧重置 widths_ 并换代表格 id
            PushToast(Notification::Kind::JobDone,
                      removed < 0 ? Fmt(L"已恢复默认列宽（配置文件格式异常，重启后生效）")
                                  : Fmt(L"已恢复默认列宽（清除 {} 项自定义列宽）", removed));
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s", U8(L"自定义壁纸"));
        DrawAppearancePanel(ctx);  // H-A(Phase-6): 选图/关闭/遮罩/性能提示
        ImGui::EndMenu();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", U8(L"主题（深色/浅色/跟随系统）与表格列宽"));
    }

    // H-A: 工具条「?」关于按钮 + 模态（tooltip 显示当前版本）。
    ImGui::SameLine(ImGui::GetWindowWidth() - 124.0f);
    ui::DrawAboutUi();

    // F4#10: 全局热键 Ctrl+Alt+M 呼出/隐藏主窗（cfg hotkeyEnabled，默认关；
    // 注册/反注册在主线程完成，失败如实提示，不持久化到注册表）。
    ImGui::SameLine();
    {
        bool hotkey = ctx.cfg.GetBool(L"hotkeyEnabled", false);
        if (ImGui::Checkbox(U8(L"全局热键 Ctrl+Alt+M"), &hotkey)) {
            std::wstring err;
            if (ui3::GcHotkeySetEnabled(hotkey, &err)) {
                ctx.cfg.SetBool(L"hotkeyEnabled", hotkey);
                PushToast(Notification::Kind::Info,
                          hotkey ? L"已注册全局热键 Ctrl+Alt+M（呼出/隐藏主窗）"
                                 : L"已注销全局热键");
            } else {
                // 不落 cfg：下一帧复选框按持久状态回显（诚实状态机）。
                PushToast(Notification::Kind::JobFailed, err);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                              U8(L"注册 Ctrl+Alt+M 全局热键：任意前台应用下呼出/隐藏本工具"
                                 L"（仅运行期有效，不写注册表）"));
        }
    }

    ImGui::SameLine(ImGui::GetWindowWidth() - 90.0f);
    if (ctx.elevated) {
        ImGui::TextColored(ColInfo(), "%s", U8(L"管理员"));
    } else {
        ImGui::TextDisabled("%s", U8(L"普通权限"));
    }
    ImGui::EndChild();
}

void DrawStatusBar(AppContext& ctx, const Snapshot& snap) {
    ImGui::BeginChild("##statusbar", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::Text("帧 %.1f ms", ctx.frameMs);
    // P2-3 (F2 review): paused collection needs a global indicator.
    if (Ui().paused) {
        ImGui::SameLine();
        ImGui::TextColored(ColWarn(), "%s", U8(L"[已暂停]"));
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text("采集 p95 %.1f ms", ctx.collect.TickP95Ms());
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    {
        const size_t pending = ctx.jobs.PendingCount();
        if (pending > 0) {
            ImGui::TextColored(ColWarn(), "%s", U8(Fmt(L"操作队列 {}", pending)));
        } else {
            ImGui::TextDisabled("%s", U8(L"操作队列空闲"));
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text(U8(L"进程 %d"), static_cast<int>(snap.procs.size()));
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (snap.degraded) {
        ImGui::TextColored(ColWarn(), "%s",
                           U8(Fmt(L"兼容模式：{}", snap.degradeReason.empty()
                                                     ? std::wstring(L"采集能力受限")
                                                     : snap.degradeReason)));
    } else {
        ImGui::TextDisabled("%s", U8(L"完整模式"));
    }
    // F4#7: CSV 记录状态在状态栏常显（悬停显示文件路径）。
    if (ui3::SharedPerfCsv().Active()) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::TextColored(ColDone(), "%s", U8(L"● 记录 CSV"));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", U8(ui3::SharedPerfCsv().Path()));
        }
    }
    if (!Ui().lastNote.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.60f, 0.62f, 0.68f, 1.0f), "%s", U8(Ui().lastNote));
    }
    ImGui::EndChild();
}

}  // namespace

// ===========================================================================
// Public entry points (Pages.h contract).
// ===========================================================================

void RegisterPages(AppContext& ctx) {
    ctx.pages.push_back(std::make_unique<ProcessesPage>());
    ctx.pages.push_back(std::make_unique<PerfPage>());
    ui3::RegisterPhase3Pages(ctx);  // phase-3: 网络/启动项/服务/驱动/传感器
    ui3::RegisterGcPages(ctx);      // F4: 崩溃记录/窗口
}

void BindAppContext(std::shared_ptr<AppContext> ctx) {
    // main registers its owning handle here before the frame loop; ops job lambdas
    // capture it so in-flight jobs survive teardown (V7-P1-3).
    Ui().liveCtx = std::move(ctx);
}

void ApplySession(AppContext& ctx, const ops::SessionState& s) {
    if (!ctx.pages.empty()) {
        int page = s.page;
        page = std::max(0, std::min(page, static_cast<int>(ctx.pages.size()) - 1));
        ctx.activePage = page;
    }
    if (!s.sortKey.empty()) ctx.cfg.SetString(L"sortKey", s.sortKey);
    ctx.cfg.SetInt(L"sortDir", s.sortDir);
    if (s.selected.pid != 0) {
        ctx.cfg.SetInt(L"selPid", static_cast<int64_t>(s.selected.pid));
        ctx.cfg.SetInt(L"selCreateTime", static_cast<int64_t>(s.selected.createTime));
    }
}

void SaveSessionFromCtx(const AppContext& ctx, HWND mainWnd) {
    ops::SessionState s;
    s.page = ctx.activePage;
    s.selected = Ui().selectedKey;
    s.sortKey = ui::SortColumnId(Ui().sortColumn);
    s.sortDir = Ui().sortDesc ? 1 : 0;
    s.intervalMs = static_cast<uint32_t>(ctx.cfg.GetInt(L"intervalMs", 1000));
    if (mainWnd != nullptr && !IsIconic(mainWnd)) {
        // Minimized windows report -32000 coords; persisting them would restore the
        // window off-screen (final review V11-P2-5) — keep the previous rect instead.
        RECT r{};
        if (GetWindowRect(mainWnd, &r)) {
            s.winX = r.left;
            s.winY = r.top;
            s.winW = r.right - r.left;
            s.winH = r.bottom - r.top;
        }
    }
    (void)ops::SaveSession(s);
}

void DrawShell(AppContext& ctx) {
    // One snapshot read per frame for the whole shell + all pages.
    Ui().snap = ctx.collect.Store().Get();
    AppendHistory(*Ui().snap);
    DrainNotifications(ctx);
    ui3::AlertTick(ctx);  // phase-3: threshold alert watcher (default off)

    // F4#3: 跨页跳转 —— 任意页发起的"跳转到进程"切换到进程页，由其下一帧解析
    // （找不到目标进程时 toast "进程已退出"）。
    {
        uint32_t jumpPid = 0;
        if (ui3::ConsumeProcessJump(&jumpPid)) {
            Ui().jumpPendingPid = jumpPid;
            if (!ctx.pages.empty()) ctx.activePage = 0;
            Ui().tabSelectArmed = true;  // 让 TabBar 选中进程页
        }
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
    ImGui::Begin("##approot", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    DrawToolbar(ctx, *Ui().snap);

    const float statusH = ImGui::GetTextLineHeightWithSpacing() + 6.0f;
    ImGui::BeginChild("##pagearea", ImVec2(0.0f, -statusH), ImGuiChildFlags_None);
    if (ImGui::BeginTabBar("##pages")) {
        // P1-1 (F2 review): session restore wrote activePage but the TabBar never
        // consumed it. Arm a one-shot SetSelected for the restored index on the
        // first rendered frame after (re)registration.
        for (size_t i = 0; i < ctx.pages.size(); ++i) {
            IPage* page = ctx.pages[i].get();
            ImGuiTabItemFlags tabFlags = 0;
            if (Ui().tabSelectArmed && static_cast<int>(i) == ctx.activePage) {
                tabFlags |= ImGuiTabItemFlags_SetSelected;
            }
            if (ImGui::BeginTabItem(U8(page->Title()), nullptr, tabFlags)) {
                ctx.activePage = static_cast<int>(i);
                page->Draw(ctx);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
        Ui().tabSelectArmed = false;
    }
    ImGui::EndChild();
    DrawStatusBar(ctx, *Ui().snap);
    ImGui::End();
    ImGui::PopStyleVar();

    ui3::DrawSmokeAllPages(ctx);  // phase-3: --smoke exercises every page offscreen
    ui3::DrawGcModals(ctx);       // F4#3: 宿主服务共享模态
    // H-A(Phase-6): --smoke 离屏绘制「自定义壁纸」控件组（滑条/状态行/性能提示
    // 的渲染路径）。选图对话框是模态 Shell 交互，headless 无法驱动 —— 选图与
    // 加载/清除按钮的点击路径留人工验收；smoke 下按钮无输入事件不会触发。
    if (AppearanceSmokePreview()) {
        const ImGuiViewport* smokeVp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(smokeVp->WorkPos.x + smokeVp->WorkSize.x + 24.0f,
                                       smokeVp->WorkPos.y + 660.0f));
        ImGui::SetNextWindowSize(ImVec2(420.0f, 260.0f));
        const ImGuiWindowFlags smokeFlags = ImGuiWindowFlags_NoDecoration |
                                            ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoInputs |
                                            ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin("##smoke_appearance", nullptr, smokeFlags)) {
            DrawAppearancePanel(ctx);
        }
        ImGui::End();
    }
    DrawToasts();
    DrawConfirmDialogs();
}

// --- --autotest dialogclick (V14) -------------------------------------------
// Arms the kill confirm through the exact same RequestConfirmKill() path the row
// context menu uses; the state recorded by DrawConfirmDialogs lets the driver in
// app/AutotestDialog.cpp aim synthetic mouse events at the REAL rendered button.

void ArmKillConfirmForAutotest(uint32_t pid, uint64_t createTime, const wchar_t* name) {
    ProcInfo p;
    p.key = ProcKey{pid, createTime};
    p.name = name != nullptr ? name : L"";
    RequestConfirmKill(p);
}

const DialogAutotestState& DialogAutotestStateForAutotest() { return g_dialogAutotest; }

}  // namespace stm
