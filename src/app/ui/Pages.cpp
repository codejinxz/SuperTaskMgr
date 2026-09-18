// UI SHELL + PAGES: process table, performance charts, detail panel, confirm dialogs,
// toasts and status bar (docs/phase/01_架构设计文档.md sections 8/11).
// All user-facing text is Chinese; it goes through ui::U8() because ImGui is a
// narrow-char (UTF-8) API. The UI thread reads the snapshot exactly once per frame.
#include "app/ui/Pages.h"
#include "app/AppContext.h"
#include "app/ui/ConfirmAction.h"
#include "app/ui/ModulesUi.h"
#include "app/ui/SortKey.h"
#include "app/ui/UiText.h"
#include "app/ui/VersionInfo.h"
#include "app/ui3/Pages3.h"  // phase-3 extension tabs + shell hooks (additive)
#include "core/ProcData.h"
#include "core/ProtectedList.h"
#include "core/Str.h"
#include "ops/DetailsProvider.h"
#include "ops/Elevate.h"
#include "ops/ProcessOps.h"
#include "imgui.h"
#include "imgui_internal.h"  // TableSetColumnSortDirection + per-column width readback
#include "implot.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <deque>
#include <memory>
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

// ===========================================================================
// Small formatting / color helpers.
// ===========================================================================

ImVec4 ColDone() { return ImVec4(0.45f, 0.80f, 0.45f, 1.0f); }
ImVec4 ColFail() { return ImVec4(0.92f, 0.36f, 0.36f, 1.0f); }
ImVec4 ColWarn() { return ImVec4(0.95f, 0.78f, 0.30f, 1.0f); }
ImVec4 ColInfo() { return ImVec4(0.55f, 0.72f, 0.95f, 1.0f); }

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
    ui::ConfirmRequest& req = Ui().confirm;
    if (req.kind == ui::ConfirmKind::None) {
        g_dialogAutotest = DialogAutotestState{};
        return;
    }
    g_dialogAutotest.requestActive = true;

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
        default:
            break;
    }

    ImGui::Separator();
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
    ImGui::BeginDisabled(planPending);
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
    void Push(float x) {
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
    h.cpuTotal.Push(static_cast<float>(s.sys.cpuTotalPercent));      // NaN ok: rendered skipped
    h.physAvail.Push(static_cast<float>(s.sys.physAvail));
    h.commit.Push(static_cast<float>(s.sys.commitTotal));
    h.diskRead.Push(static_cast<float>(s.sys.diskReadBps));
    h.diskWrite.Push(static_cast<float>(s.sys.diskWriteBps));
    h.netRecv.Push(static_cast<float>(s.sys.netRecvBps));
    h.netSend.Push(static_cast<float>(s.sys.netSendBps));

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
    }

    void PersistSort(AppContext& ctx) {
        ctx.cfg.SetString(L"sortKey", ui::SortColumnId(sortColumn_));
        ctx.cfg.SetInt(L"sortDir", sortDesc_ ? 1 : 0);
    }

    void PersistWidths(AppContext& ctx) {
        // Called at most ~1 Hz from inside the table; cheap key/value writes only
        // (the file itself is saved by main on exit).
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

    void RebuildRows(const Snapshot& snap) {
        rows_.clear();
        rows_.reserve(snap.procs.size());
        for (int i = 0; i < static_cast<int>(snap.procs.size()); ++i) {
            if (MatchesFilter(snap.procs[static_cast<size_t>(i)])) rows_.push_back(i);
        }
        std::stable_sort(rows_.begin(), rows_.end(), [this, &snap](int x, int y) {
            return ui::SortLess(snap.procs[static_cast<size_t>(x)],
                                snap.procs[static_cast<size_t>(y)], sortColumn_, sortDesc_);
        });
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
        ImGui::SameLine();
        ImGui::TextDisabled("%s", U8(Fmt(L"{} / {} 个进程", rows_.size(), snap.procs.size())));
    }

    // ---- table ---------------------------------------------------------------
    void DrawTable(AppContext& ctx, const Snapshot& snap) {
        if (snap.procs.empty()) {
            ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.65f, 1.0f), "%s", U8(L"等待采集数据…"));
            return;
        }

        const int tableFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                               ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingFixedFit;
        if (!ImGui::BeginTable("procs", static_cast<int>(ui::SortColumn::Count) + 2, tableFlags)) {
            return;
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        SetupColumns();
        ReflectPersistedSortOnce();

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

        ImGui::TableHeadersRow();
        DrawHeaderTooltips();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows_.size()));
        while (clipper.Step()) {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                DrawRow(ctx, snap.procs[static_cast<size_t>(rows_[static_cast<size_t>(r)])]);
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

    void DrawRow(AppContext& ctx, const ProcInfo& p) {
        // P2-11 (F2 review): identify rows by the stable ProcKey (pid + createTime),
        // not a display index — a refresh that reorders rows mid-interaction must
        // never retarget an open context menu / tooltip to a different process.
        ImGui::PushID(static_cast<int>(p.key.pid));
        ImGui::PushID(static_cast<int>(p.key.createTime & 0x7fffffff));
        ImGui::TableNextRow();

        // Name + row interaction (selection, double click, context menu).
        ImGui::TableNextColumn();
        const bool selected = selectedValid_ && p.key == selected_;
        if (ImGui::Selectable(U8(p.name), selected,
                              ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
            Select(p);
        }
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

    void DrawBadges(const ProcInfo& p) const {
        auto badge = [&p](uint32_t flag, const char* label, const ImVec4& color) {
            if ((p.flags & flag) == 0) return;
            ImGui::TextColored(color, "%s", label);
            ImGui::SameLine();
        };
        badge(PF_Elevated, "A", ImVec4(0.95f, 0.65f, 0.30f, 1.0f));  // 管理员提权
        badge(PF_Uwp, "U", ImVec4(0.45f, 0.72f, 0.95f, 1.0f));       // UWP
        badge(PF_Wow64, "W", ImVec4(0.50f, 0.85f, 0.55f, 1.0f));     // WOW64
        badge(PF_ServiceHost, "S", ImVec4(0.75f, 0.55f, 0.95f, 1.0f));  // 服务宿主
        badge(PF_Protected, "P", ImVec4(0.92f, 0.36f, 0.36f, 1.0f));    // 受保护
        badge(PF_Suspended, "Z", ImVec4(0.60f, 0.60f, 0.60f, 1.0f));    // 挂起
        static const std::wstring kLegend =
            L"A 管理员提权  U UWP  W WOW64  S 服务宿主  P 受保护  Z 挂起";
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

        if (ImGui::MenuItem(U8(L"查看详情"), nullptr, false, true)) Select(p);
        ImGui::Separator();
        if (protectedProc) {
            ImGui::MenuItem(U8(Fmt(L"受保护：{}", reason.empty() ? std::wstring(L"系统关键进程") : reason)),
                            nullptr, false, false);
        }
        ImGui::BeginDisabled(protectedProc);
        if (ImGui::MenuItem(U8(L"终止进程"))) RequestConfirmKill(p);
        if (ImGui::MenuItem(U8(L"终止进程树"))) RequestTreePlan(p);
        // V8-P1-1: trim is a destructive op too — same gate as the terminate items.
        if (ImGui::MenuItem(U8(L"释放工作集"))) RequestConfirmTrim(p);
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
        DrawModulesSection(ctx, p, alive);
        ImGui::EndChild();
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

    uint64_t lastTickId_ = 0;
    std::vector<int> rows_;  // filtered + sorted indices into snapshot procs
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
        (void)ctx;
        const PerfHistory& h = Hist();
        if (h.lastTick == 0) {
            ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.65f, 1.0f), "%s", U8(L"等待采集数据…"));
            return;
        }

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float plotW = (avail.x - spacing) * 0.5f;
        const float barsH = ImGui::GetFrameHeightWithSpacing() * 2.0f + 8.0f;
        const float plotH = std::max(120.0f, (avail.y - barsH - spacing) * 0.5f);

        DrawCpu(plotW, plotH, h);
        ImGui::SameLine();
        DrawMemory(plotW, plotH, h);
        ImGui::SameLine();
        DrawDisk(plotW, plotH, h);
        ImGui::SameLine();
        DrawNet(plotW, plotH, h);

        const SystemInfo& sys = Ui().snap ? Ui().snap->sys : SystemInfo{};
        DrawMemoryBars(sys);
        // P1-5 (F2 review): GPU data is produced every 2 s by the collector —
        // render adapter utilization/memory + per-process top 5 consumers.
        DrawGpuBlock(*Ui().snap);
        ui3::DrawAlertControls(ctx);  // phase-3: threshold alert controls (additive row)
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
    }

    // ---- P1-5 (F2 review): GPU block ----------------------------------------
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

    static void DrawGpuBlock(const Snapshot& snap) {
        ImGui::Separator();
        ImGui::TextUnformatted(U8(L"GPU"));
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
