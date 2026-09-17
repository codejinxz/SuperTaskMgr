// UI SHELL + PAGES: process table, performance charts, detail panel, confirm dialogs,
// toasts and status bar (docs/phase/01_架构设计文档.md sections 8/11).
// All user-facing text is Chinese; it goes through ui::U8() because ImGui is a
// narrow-char (UTF-8) API. The UI thread reads the snapshot exactly once per frame.
#include "app/ui/Pages.h"
#include "app/AppContext.h"
#include "app/ui/SortKey.h"
#include "app/ui/UiText.h"
#include "app/ui/VersionInfo.h"
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

struct ConfirmKind {
    enum E { None = 0, Kill, KillTree, TrimWorkingSet, PurgeStandby };
};

// Pending confirmation. ProcKey/name/path are locked at request time ("确认框打开
// 瞬间锁定副本"); ops re-verifies identity at execution time regardless.
struct ConfirmRequest {
    int kind = ConfirmKind::None;
    ProcKey key;
    uint32_t pid = 0;
    std::wstring name;
    std::wstring path;
    bool serviceHost = false;
    // Tree planning: filled by an ops job, polled by the UI until ready.
    // -1 pending, -2 planning failed, >= 0 planned count.
    std::shared_ptr<std::atomic<int>> planCount;
};

struct UiState {
    std::shared_ptr<const Snapshot> snap;  // one Store().Get() per frame (shell owns it)
    // Owning handle registered by main (BindAppContext). Job lambdas capture it BY
    // VALUE so an in-flight job outlives main's teardown: JobQueue::Shutdown detaches
    // a timed-out worker, and this capture keeps notes/jobs/details alive until the
    // job finishes, whichever thread drops the last reference.
    std::shared_ptr<AppContext> liveCtx;
    std::deque<Toast> toasts;
    std::wstring lastNote;                  // survives toast expiry for the status bar
    ConfirmRequest confirm;
    bool confirmOpenRequested = false;
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
// ops job submission helpers (all destructive ops go through ctx.jobs; results
// come back as notifications -> toasts).
// ===========================================================================

void PostNote(NotificationQueue& notes, Notification::Kind kind, const std::wstring& text) {
    Notification n;
    n.kind = kind;
    n.text = text;
    notes.Push(n);
}

std::wstring FailSuffix(bool elevated) {
    return elevated ? std::wstring()
                    : std::wstring(L"（可能需要管理员权限，可尝试提权重启）");
}

// Every job captures the shared AppContext by value: the capture holds notes/jobs/
// details alive even if main tears down while this job is still in flight
// (JobQueue::Shutdown detaches after its wait timeout). Never touch the raw
// AppContext& from inside a job.
void SubmitKill(const ProcKey& key, const std::wstring& name) {
    std::shared_ptr<AppContext> app = Ui().liveCtx;
    if (!app) return;
    const bool elevated = app->elevated;
    app->jobs.Submit([app, elevated, key, name] {
        std::wstring err;
        if (ops::TerminateProcessById(key, &err)) {
            PostNote(app->notes, Notification::Kind::JobDone,
                     Fmt(L"已终止进程 {} ({})", name, key.pid));
        } else {
            PostNote(app->notes, Notification::Kind::JobFailed,
                     Fmt(L"终止 {} ({}) 失败：{}{}", name, key.pid,
                         err.empty() ? std::wstring(L"未知错误") : err, FailSuffix(elevated)));
        }
    });
}

void SubmitKillTree(const ProcKey& key, const std::wstring& name) {
    std::shared_ptr<AppContext> app = Ui().liveCtx;
    if (!app) return;
    const bool elevated = app->elevated;
    app->jobs.Submit([app, elevated, key, name] {
        ops::TreeResult r;
        std::wstring err;
        if (!ops::TerminateTree(key, &r, &err)) {
            PostNote(app->notes, Notification::Kind::JobFailed,
                     Fmt(L"终止进程树 {} ({}) 失败：{}{}", name, key.pid,
                         err.empty() ? std::wstring(L"未知错误") : err, FailSuffix(elevated)));
            return;
        }
        std::wstring text = Fmt(L"已终止进程树 {} ({})：终止 {} 个", name, key.pid, r.terminated);
        if (r.skippedProtected > 0) text += Fmt(L"，跳过保护进程 {} 个", r.skippedProtected);
        if (r.failed > 0) text += Fmt(L"，失败 {} 个", r.failed);
        PostNote(app->notes, r.failed > 0 ? Notification::Kind::Warn : Notification::Kind::JobDone,
                 text);
    });
}

void SubmitTrimWorkingSet(const ProcKey& key, const std::wstring& name) {
    std::shared_ptr<AppContext> app = Ui().liveCtx;
    if (!app) return;
    const bool elevated = app->elevated;
    app->jobs.Submit([app, elevated, key, name] {
        std::wstring err;
        if (ops::TrimWorkingSet(key, &err)) {
            PostNote(app->notes, Notification::Kind::JobDone,
                     Fmt(L"已请求释放 {} ({}) 的工作集内存", name, key.pid));
        } else {
            PostNote(app->notes, Notification::Kind::JobFailed,
                     Fmt(L"释放 {} ({}) 工作集失败：{}{}", name, key.pid,
                         err.empty() ? std::wstring(L"未知错误") : err, FailSuffix(elevated)));
        }
    });
}

void SubmitPurgeStandby() {
    std::shared_ptr<AppContext> app = Ui().liveCtx;
    if (!app) return;
    const bool elevated = app->elevated;
    app->jobs.Submit([app, elevated] {
        std::wstring err;
        if (ops::PurgeStandbyList(&err)) {
            PostNote(app->notes, Notification::Kind::JobDone, L"已清理系统待机列表");
        } else {
            PostNote(app->notes, Notification::Kind::JobFailed,
                     Fmt(L"清理待机列表失败：{}{}", err.empty() ? std::wstring(L"未知错误") : err,
                         FailSuffix(elevated)));
        }
    });
}

void RequestTreePlan(const ProcInfo& p) {
    auto& req = Ui().confirm;
    req = ConfirmRequest{};
    req.kind = ConfirmKind::KillTree;
    req.key = p.key;
    req.pid = p.key.pid;
    req.name = p.name;
    req.path = p.path;
    req.serviceHost = (p.flags & PF_ServiceHost) != 0;
    req.planCount = std::make_shared<std::atomic<int>>(-1);
    std::shared_ptr<AppContext> app = Ui().liveCtx;
    if (!app) return;
    std::shared_ptr<std::atomic<int>> plan = req.planCount;
    app->jobs.Submit([app, key = p.key, plan] {
        std::vector<ProcKey> members;
        std::wstring err;
        if (ops::PlanTerminateTree(key, &members, &err)) {
            plan->store(static_cast<int>(members.size()));
        } else {
            plan->store(-2);
            PostNote(app->notes, Notification::Kind::JobFailed,
                     Fmt(L"无法规划进程树：{}", err.empty() ? std::wstring(L"未知错误") : err));
        }
    });
}

// Confirm request builders (ProcKey/name/path locked here).
void RequestConfirmKill(const ProcInfo& p) {
    auto& req = Ui().confirm;
    req = ConfirmRequest{};
    req.kind = ConfirmKind::Kill;
    req.key = p.key;
    req.pid = p.key.pid;
    req.name = p.name;
    req.path = p.path;
    req.serviceHost = (p.flags & PF_ServiceHost) != 0;
    Ui().confirmOpenRequested = true;
}

void RequestConfirmTrim(const ProcInfo& p) {
    auto& req = Ui().confirm;
    req = ConfirmRequest{};
    req.kind = ConfirmKind::TrimWorkingSet;
    req.key = p.key;
    req.pid = p.key.pid;
    req.name = p.name;
    req.path = p.path;
    Ui().confirmOpenRequested = true;
}

void RequestConfirmPurgeStandby() {
    auto& req = Ui().confirm;
    req = ConfirmRequest{};
    req.kind = ConfirmKind::PurgeStandby;
    Ui().confirmOpenRequested = true;
}

// ===========================================================================
// Small formatting / color helpers.
// ===========================================================================

ImVec4 ColDone() { return ImVec4(0.45f, 0.80f, 0.45f, 1.0f); }
ImVec4 ColFail() { return ImVec4(0.92f, 0.36f, 0.36f, 1.0f); }
ImVec4 ColWarn() { return ImVec4(0.95f, 0.78f, 0.30f, 1.0f); }
ImVec4 ColInfo() { return ImVec4(0.55f, 0.72f, 0.95f, 1.0f); }

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

void DrawConfirmDialogs() {
    ConfirmRequest& req = Ui().confirm;
    if (req.kind == ConfirmKind::None) return;

    // Tree plan polling: open the dialog only once the plan job reported back.
    if (req.kind == ConfirmKind::KillTree && req.planCount && !Ui().confirmOpenRequested) {
        const int v = req.planCount->load();
        if (v == -2) {  // planning failed; a failure toast was already posted
            req = ConfirmRequest{};
            return;
        }
        if (v >= 0) Ui().confirmOpenRequested = true;
    }
    if (!Ui().confirmOpenRequested) return;

    ImGui::OpenPopup("##confirm");
    Ui().confirmOpenRequested = false;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(460.0f, 0.0f), ImVec2(460.0f, FLT_MAX));
    if (!ImGui::BeginPopupModal("##confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    const wchar_t* title = L"确认";
    const wchar_t* action = L"确认";
    switch (req.kind) {
        case ConfirmKind::Kill: title = L"确认终止进程"; action = L"终止进程"; break;
        case ConfirmKind::KillTree: title = L"确认终止进程树"; action = L"终止进程树"; break;
        case ConfirmKind::TrimWorkingSet: title = L"确认释放工作集"; action = L"释放工作集"; break;
        case ConfirmKind::PurgeStandby: title = L"确认清理待机列表"; action = L"清理待机列表"; break;
        default: break;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ColFail());
    ImGui::TextUnformatted(U8(title));
    ImGui::PopStyleColor();
    ImGui::Separator();

    switch (req.kind) {
        case ConfirmKind::Kill:
            ImGui::TextUnformatted(U8(Fmt(L"目标：{} (PID {})", req.name, req.pid)));
            ImGui::TextUnformatted(U8(Fmt(L"路径：{}", req.path.empty() ? std::wstring(L"—") : req.path)));
            if (req.serviceHost) {
                ImGui::TextColored(ColFail(), "%s",
                                   U8(L"警告：该进程是服务宿主，终止可能影响系统服务。"));
            }
            ImGui::TextDisabled("%s", U8(L"此操作无法撤销。"));
            break;
        case ConfirmKind::KillTree: {
            ImGui::TextUnformatted(U8(Fmt(L"目标：{} (PID {})", req.name, req.pid)));
            int planned = -1;
            if (req.planCount) planned = req.planCount->load();
            // PlanTerminateTree returns descendants only; TreeResult.planned includes
            // the root, so display planned + 1 (target included) to match that count.
            ImGui::TextUnformatted(
                U8(Fmt(L"预计终止 {} 个（含目标进程，执行时可能变化）", planned + 1)));
            if (req.serviceHost) {
                ImGui::TextColored(ColFail(), "%s",
                                   U8(L"警告：该进程是服务宿主，终止将影响其承载的全部服务。"));
            }
            ImGui::TextDisabled("%s", U8(L"此操作无法撤销。"));
            break;
        }
        case ConfirmKind::TrimWorkingSet:
            ImGui::TextUnformatted(U8(Fmt(L"目标：{} (PID {})", req.name, req.pid)));
            ImGui::TextWrapped(
                "%s", U8(L"将提示系统尽量释放该进程的物理工作集内存。工作集页换出后再次访问需要"
                          "重新读入，该进程短期可能变卡；此操作不会回收进程正在使用的内存。"));
            break;
        case ConfirmKind::PurgeStandby:
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
    ImGui::SetKeyboardFocusHere(0);
    if (ImGui::Button(U8(L"取消"), ImVec2(120.0f, 0.0f))) {
        req = ConfirmRequest{};
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    const bool actionPressed =
        ImGui::Button(U8(action), ImVec2(120.0f, 0.0f));
    ImGui::PopItemFlag();
    if (actionPressed) {
        switch (req.kind) {
            case ConfirmKind::Kill: SubmitKill(req.key, req.name); break;
            case ConfirmKind::KillTree: SubmitKillTree(req.key, req.name); break;
            case ConfirmKind::TrimWorkingSet: SubmitTrimWorkingSet(req.key, req.name); break;
            case ConfirmKind::PurgeStandby: SubmitPurgeStandby(); break;
            default: break;
        }
        req = ConfirmRequest{};
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

// Detail-kind bit set fetched whenever the selection changes.
constexpr uint32_t kDetailKinds =
    static_cast<uint32_t>(ops::DetailKind::Signature) | static_cast<uint32_t>(ops::DetailKind::CmdLine) |
    static_cast<uint32_t>(ops::DetailKind::UserInfo) | static_cast<uint32_t>(ops::DetailKind::GuiObjects);

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
        save(12, L"colW_desc", false);
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
        ImGui::PushID(static_cast<int>(p.key.pid));
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
        DrawSignatureField(ctx, p);
        DrawCmdLineField(ctx, p);
        field(L"用户名", DrawUserNameField(ctx, p));
        field(L"GDI / USER 对象", DrawGuiObjectsField(ctx, p));

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

    void DrawSignatureField(AppContext& ctx, const ProcInfo& p) const {
        const ops::ProcessDetails* d = ctx.details->Peek(p.key);
        const wchar_t* text = L"查询中…";
        ImVec4 color(0.6f, 0.6f, 0.6f, 1.0f);
        if (d != nullptr && d->sigResolved) {
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

    void DrawCmdLineField(AppContext& ctx, const ProcInfo& p) const {
        const ops::ProcessDetails* d = ctx.details->Peek(p.key);
        std::wstring value = L"—";
        if (d != nullptr) value = d->cmdLineAvail ? d->cmdLine : L"不可用（无读取权限）";
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
};

// ===========================================================================
// Shell: toolbar / tabs / status bar.
// ===========================================================================

void DrainNotifications(AppContext& ctx) {
    std::vector<Notification> out;
    ctx.notes.Drain(&out);
    for (const Notification& n : out) PushToast(n.kind, n.text);
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
            SaveSessionFromCtx(ctx, nullptr);
            if (ops::RelaunchAsAdmin(L"--relaunched")) ctx.wantExit = true;
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
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text("采集 p95 %.1f ms", ctx.collect.TickP95Ms());
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
    if (mainWnd != nullptr) {
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
        for (size_t i = 0; i < ctx.pages.size(); ++i) {
            IPage* page = ctx.pages[i].get();
            if (ImGui::BeginTabItem(U8(page->Title()))) {
                ctx.activePage = static_cast<int>(i);
                page->Draw(ctx);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    DrawStatusBar(ctx, *Ui().snap);
    ImGui::End();
    ImGui::PopStyleVar();

    DrawToasts();
    DrawConfirmDialogs();
}

}  // namespace stm
