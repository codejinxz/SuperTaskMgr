// PHASE-3 UI EXTENSION: network / startup / services / drivers / sensors tabs
// plus the optional threshold alert watcher (docs/phase/01_架构设计文档.md §8).
// Design rules inherited from app/ui/Pages.cpp:
//  - every user-facing Chinese string goes through ui::U8() (UTF-8 text cache)
//  - all data is fetched through the ops job queue into per-page caches
//    (ui3::AsyncFetch) with page-specific min refresh intervals (2 s net /
//    5 s services+drivers / 10 s sensors); the UI thread never blocks and
//    pages render a "加载中…" state instead
//  - destructive ops open a two-stage confirm dialog (cancel button first and
//    keyboard-focused, action button removed from keyboard navigation), then
//    submit through the job queue and report via the notification queue
//  - unavailable values render as "—"; unknown/error states render honest
//    Chinese explanations, never fake data
// Only additive integration points live in app/ui/Pages.cpp and app/main.cpp.
#include "app/ui3/Pages3.h"
#include "app/ui3/AsyncFetch.h"
#include "app/ui3/PageHelpers.h"
#include "app/ui/Pages.h"
#include "app/ui/UiText.h"
#include "collect/NetTables.h"
#include "collect/Sensors.h"
#include "core/Str.h"
#include "ops/DriverOps.h"
#include "ops/Elevate.h"
#include "ops/ServiceOps.h"
#include "ops/Signature.h"
#include "ops/StartupOps.h"
#include "imgui.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace stm {
namespace ui3 {

// Shared slots (defined at the bottom; used inside the anonymous namespace).
std::shared_ptr<AppContext>& P3Slot();
BalloonSink& SinkSlot();

namespace {

using ui::U8;

// ===========================================================================
// Small shared helpers (local to this translation unit).
// ===========================================================================

ImVec4 ColDone() { return ImVec4(0.45f, 0.80f, 0.45f, 1.0f); }
ImVec4 ColFail() { return ImVec4(0.92f, 0.36f, 0.36f, 1.0f); }
ImVec4 ColWarn() { return ImVec4(0.95f, 0.78f, 0.30f, 1.0f); }
ImVec4 ColInfo() { return ImVec4(0.55f, 0.72f, 0.95f, 1.0f); }
ImVec4 ColMuted() { return ImVec4(0.60f, 0.62f, 0.68f, 1.0f); }

void PushNote(AppContext& ctx, Notification::Kind kind, const std::wstring& text) {
    Notification n;
    n.kind = kind;
    n.text = text;
    ctx.notes.Push(n);
}

std::wstring FailSuffix(bool elevated) {
    return elevated ? std::wstring()
                    : std::wstring(L"（可能需要管理员权限，可尝试提权重启）");
}

std::wstring LowerCopy(const std::wstring& s) {
    std::wstring lower(s.size(), L'\0');
    std::transform(s.begin(), s.end(), lower.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return lower;
}

// Case-insensitive ASCII substring match (same semantics as the process page).
bool ContainsLower(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return true;
    return LowerCopy(hay).find(needle) != std::wstring::npos;
}

std::wstring Truncate(const std::wstring& s, size_t maxChars) {
    if (s.size() <= maxChars) return s;
    return s.substr(0, maxChars) + L"…";
}

// True on the first Draw call and whenever the page was not drawn on the
// previous frame (tab switch). kNeverDrawn sentinel: frame 0 is a valid id.
constexpr uint64_t kNeverDrawn = ~0ull;
bool BecameActive(uint64_t& lastFrame) {
    const uint64_t f = ImGui::GetFrameCount();
    const bool active = lastFrame == kNeverDrawn || f > lastFrame + 1;
    lastFrame = f;
    return active;
}

void LowerFilter(const std::string& utf8, std::wstring& out) { out = LowerCopy(Utf8ToWide(utf8)); }

// Shared "以管理员身份重启" button (same flow as the shell toolbar button).
void DrawElevateButton(AppContext& ctx) {
    if (ctx.elevated || !ops::CanElevate()) return;
    if (ImGui::Button(U8(L"以管理员身份重启"))) {
        SaveSessionFromCtx(ctx, nullptr);
        if (ops::RelaunchAsAdmin(L"--relaunched")) ctx.wantExit = true;
    }
}

void DrawLoading() {
    ImGui::TextColored(ColMuted(), "%s", U8(L"加载中…"));
}

// Error line + retry button; sets *retry on the frame the button is pressed
// (the caller then issues a forced fetch).
void DrawLoadError(const std::wstring& err, bool* retry) {
    ImGui::TextColored(ColFail(), "%s",
                       U8(Fmt(L"加载失败：{}", err.empty() ? std::wstring(L"未知错误") : err)));
    if (ImGui::Button(U8(L"重试"))) *retry = true;
}

// pid -> process name from the current collection snapshot (ascending by pid;
// a name hint only, no identity verification needed for display).
const ProcInfo* FindPid(const Snapshot& snap, uint32_t pid) {
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

// ===========================================================================
// NetworkPage: TCP/UDP endpoint table (read-only page, no dangerous ops).
// Data: collect::SnapshotConnections via jobs, min 2 s between fetches.
// ===========================================================================

const wchar_t* ProtoLabel(ConnProto p) {
    switch (p) {
        case ConnProto::Tcp4: return L"TCP";
        case ConnProto::Tcp6: return L"TCP6";
        case ConnProto::Udp4: return L"UDP";
        case ConnProto::Udp6: return L"UDP6";
        default: return L"—";
    }
}

class NetworkPage final : public IPage {
public:
    const wchar_t* Id() const override { return L"net"; }
    const wchar_t* Title() const override { return L"网络"; }

    void Draw(AppContext& ctx) override {
        if (!etwLoaded_) {  // one-shot: the collector read-back wins over config
            etwLoaded_ = true;
            const bool cfgWants = ctx.cfg.GetBool(L"netEtw", false);
            etw_ = cfgWants && ctx.collect.NetEtwEnabled();  // P1-1: honest initial state
            if (etw_ != cfgWants) {
                ctx.cfg.SetBool(L"netEtw", etw_);  // persist the honest state
            }
        }
        fetch_.MaybeFetch(Produce, false);
        std::shared_ptr<const Result> res = fetch_.Peek();

        DrawToolbar(ctx, res.get());
        ImGui::Separator();

        if (res == nullptr) {
            DrawLoading();
            return;
        }
        if (!res->ok && res->data.empty()) {
            bool retry = false;
            DrawLoadError(res->err, &retry);
            if (retry) fetch_.MaybeFetch(Produce, true);
            return;
        }
        if (!res->err.empty()) {
            ImGui::TextColored(ColWarn(), "%s",
                               U8(Fmt(L"部分数据不可用：{}", res->err)));
        }
        UpdateRows(*res);
        DrawTable(ctx, *res);
    }

private:
    using Result = AsyncFetch<std::vector<ConnEntry>>::Result;
    static std::vector<ConnEntry> Produce(std::wstring* err) {
        return SnapshotConnections(err);
    }

    void DrawToolbar(AppContext& ctx, const Result* res) {
        // ETW toggle state machine (P1-1 + P2-1): the checkbox is only a
        // request. Start/Stop runs on the ops job queue, the checkbox is
        // disabled while a toggle is in flight, and the collector read-back
        // (NetEtwEnabled) decides the real state — a mismatch rolls the
        // checkbox and cfg back and posts a JobFailed toast.
        if (etwToggle_) {
            bool ready = false;
            bool actual = false;
            {
                std::lock_guard<std::mutex> lock(etwToggle_->mu);
                ready = etwToggle_->ready;
                actual = etwToggle_->actual;
            }
            if (ready) {
                etwToggle_.reset();
                etw_ = actual;
                ctx.cfg.SetBool(L"netEtw", actual);  // persist truth / rollback
                if (actual != etwDesired_) {
                    PushNote(ctx, Notification::Kind::JobFailed,
                             etwDesired_ ? L"按进程流量（ETW）开启失败：需要管理员权限"
                                         : L"按进程流量（ETW）关闭失败：需要管理员权限");
                } else {
                    PushNote(ctx, Notification::Kind::Info,
                             actual ? L"已开启按进程流量统计（ETW）"
                                    : L"已关闭按进程流量统计（ETW）");
                }
            }
        }
        if (res != nullptr) {
            size_t tcp = 0, udp = 0;
            for (const ConnEntry& c : res->data) {
                if (c.proto == ConnProto::Tcp4 || c.proto == ConnProto::Tcp6) {
                    ++tcp;
                } else {
                    ++udp;
                }
            }
            ImGui::TextUnformatted(U8(Fmt(L"TCP {} 条 · UDP {} 条", tcp, udp)));
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", U8(L"当前活动的 TCP/UDP 端点数量（含监听）"));
            }
        } else {
            ImGui::TextColored(ColMuted(), "%s", U8(L"统计加载中…"));
        }
        ImGui::SameLine();
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", filterUtf8_.c_str());
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::InputTextWithHint("##netfilter", U8(L"过滤地址/端口/PID/进程名"),
                                     buf, sizeof(buf))) {
            filterUtf8_ = buf;
        }
        ImGui::SameLine();
        if (ImGui::Button(U8(L"刷新"))) fetch_.MaybeFetch(Produce, true);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                              U8(L"手动刷新：立即重新拉取连接表（可越过 2 秒最小间隔；"
                                 L"自动刷新受该间隔限制）"));
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(etwToggle_ != nullptr);  // pending: debounce re-toggles
        if (ImGui::Checkbox(U8(L"按进程流量（ETW）"), &etw_)) {
            const bool desired = etw_;
            etwDesired_ = desired;
            etwToggle_ = std::make_shared<EtwToggle>();
            std::shared_ptr<AppContext> app = LiveP3Ctx();
            std::shared_ptr<EtwToggle> toggle = etwToggle_;
            if (app) {
                if (app->jobs.Submit([app, toggle, desired] {
                        app->collect.SetNetEtwEnabled(desired);
                        const bool actual = app->collect.NetEtwEnabled();  // read-back = truth
                        std::lock_guard<std::mutex> lock(toggle->mu);
                        toggle->actual = actual;
                        toggle->ready = true;
                    }) == 0) {
                    etw_ = !desired;  // queue already shut down: revert the checkbox
                    etwToggle_.reset();
                }
            } else {
                etw_ = !desired;  // teardown race: revert the checkbox
                etwToggle_.reset();
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s",
                              U8(etwToggle_ != nullptr
                                     ? std::wstring(L"正在切换（等待采集器确认）…")
                                     : std::wstring(L"需管理员权限；在本地记录每个进程的网络收发"
                                                    L"统计，仅本机使用、不上传")));
        }
    }

    bool MatchesFilter(const ConnEntry& c, const std::wstring& name) const {
        if (appliedFilter_.empty()) return true;
        wchar_t pid[16] = {};
        swprintf_s(pid, L"%u", c.pid);
        wchar_t lport[8] = {}, rport[8] = {};
        swprintf_s(lport, L"%u", static_cast<unsigned>(c.localPort));
        swprintf_s(rport, L"%u", static_cast<unsigned>(c.remotePort));
        return ContainsLower(c.localAddr, appliedFilter_) ||
               ContainsLower(lport, appliedFilter_) ||
               ContainsLower(c.remoteAddr, appliedFilter_) ||
               ContainsLower(rport, appliedFilter_) ||
               ContainsLower(pid, appliedFilter_) || ContainsLower(name, appliedFilter_);
    }

    // Filtered row indices; rebuilt only when data or filter changed.
    void UpdateRows(const Result& res) {
        LowerFilter(filterUtf8_, filterWide_);
        if (filterWide_ == appliedFilter_ && lastData_ == &res) return;
        appliedFilter_ = filterWide_;
        lastData_ = &res;

        std::shared_ptr<const Snapshot> snap =
            LiveP3Ctx() ? LiveP3Ctx()->collect.Store().Get() : nullptr;
        rows_.clear();
        rows_.reserve(res.data.size());
        for (size_t i = 0; i < res.data.size(); ++i) {
            const ConnEntry& c = res.data[i];
            std::wstring name;
            if (snap != nullptr) {
                if (const ProcInfo* p = FindPid(*snap, c.pid)) name = p->name;
            }
            if (MatchesFilter(c, name)) rows_.push_back(static_cast<int>(i));
        }
    }

    void DrawTable(AppContext& ctx, const Result& res) {
        if (rows_.empty()) {
            ImGui::TextColored(ColMuted(), "%s",
                               U8(res.data.empty() ? L"暂无活动连接"
                                                   : L"没有匹配过滤条件的连接"));
            return;
        }
        std::shared_ptr<const Snapshot> snap = ctx.collect.Store().Get();
        const int flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_SizingFixedFit;
        if (!ImGui::BeginTable("netconn", 6, flags)) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(U8(L"协议"), ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableSetupColumn(U8(L"本地地址:端口"), ImGuiTableColumnFlags_WidthFixed, 210.0f);
        ImGui::TableSetupColumn(U8(L"远程地址:端口"), ImGuiTableColumnFlags_WidthFixed, 210.0f);
        ImGui::TableSetupColumn(U8(L"状态"), ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn(U8(L"PID"), ImGuiTableColumnFlags_WidthFixed, 76.0f);
        ImGui::TableSetupColumn(U8(L"进程名"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows_.size()));
        while (clipper.Step()) {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                const ConnEntry& c = res.data[static_cast<size_t>(rows_[static_cast<size_t>(r)])];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(ProtoLabel(c.proto)));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(ConnEndpoint(c.localAddr, c.localPort)));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(ConnEndpoint(c.remoteAddr, c.remotePort)));
                ImGui::TableNextColumn();
                if (c.proto == ConnProto::Tcp4 || c.proto == ConnProto::Tcp6) {
                    ImGui::TextUnformatted(U8(UiTcpStateLabel(c.state)));
                } else {
                    ImGui::TextDisabled("%s", U8(L"—"));  // UDP has no state
                }
                ImGui::TableNextColumn();
                ImGui::Text("%u", c.pid);
                ImGui::TableNextColumn();
                std::wstring name = L"—";  // §8: unknown owner renders as em dash
                if (c.pid == 0) {
                    name = L"系统";  // contract: pid 0 = bound by kernel/System
                } else if (const ProcInfo* p = FindPid(*snap, c.pid)) {
                    name = p->name;
                }
                ImGui::TextUnformatted(U8(name));
            }
        }
        ImGui::EndTable();
    }

    // In-flight ETW toggle: written by the ops job, polled by the UI thread.
    struct EtwToggle {
        std::mutex mu;
        bool ready = false;
        bool actual = false;
    };

    AsyncFetch<std::vector<ConnEntry>> fetch_{2.0};
    std::string filterUtf8_;
    std::wstring filterWide_;
    std::wstring appliedFilter_;
    const Result* lastData_ = nullptr;
    std::vector<int> rows_;
    std::shared_ptr<EtwToggle> etwToggle_;  // null = no toggle in flight
    bool etwDesired_ = false;
    bool etw_ = false;
    bool etwLoaded_ = false;
};

// ===========================================================================
// StartupPage: 4-source startup items; enable/disable with backup note.
// Data: ops::EnumStartupItems via jobs; refresh on tab activation + manual.
// ===========================================================================

class StartupPage final : public IPage {
public:
    const wchar_t* Id() const override { return L"startup"; }
    const wchar_t* Title() const override { return L"启动项"; }

    void Draw(AppContext& ctx) override {
        const bool becameActive = BecameActive(lastFrame_);
        fetch_.MaybeFetch(Produce, becameActive);  // 切页刷新 per spec
        if (refetchPending_ && !fetch_.Busy()) {   // refresh after an op landed
            refetchPending_ = false;
            fetch_.MaybeFetch(Produce, true);
        }
        std::shared_ptr<const Result> res = fetch_.Peek();

        DrawToolbar(ctx, res.get());
        ImGui::TextDisabled("%s",
                            U8(L"禁用操作会先备份原值到本地日志目录，可随时恢复"));
        ImGui::Separator();

        if (res == nullptr) {
            DrawLoading();
        } else if (!res->ok && res->data.empty()) {
            bool retry = false;
            DrawLoadError(res->err, &retry);
            if (retry) fetch_.MaybeFetch(Produce, true);
        } else {
            if (!res->err.empty()) {
                ImGui::TextColored(ColWarn(), "%s",
                                   U8(Fmt(L"部分来源读取失败：{}", res->err)));
            }
            UpdateRows(*res);
            DrawTable(ctx, *res);
        }
        DrawConfirmDialog(ctx);
    }

private:
    using Result = AsyncFetch<std::vector<ops::StartupItem>>::Result;
    static std::vector<ops::StartupItem> Produce(std::wstring* err) {
        return ops::EnumStartupItems(err);
    }

    const ops::StartupItem* Selected(const Result& res) const {
        if (selectedId_.empty()) return nullptr;
        for (const ops::StartupItem& it : res.data) {
            if (it.id == selectedId_) return &it;
        }
        return nullptr;
    }

    void DrawToolbar(AppContext& ctx, const Result* res) {
        if (ImGui::Button(U8(L"刷新"))) fetch_.MaybeFetch(Produce, true);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                              U8(L"手动刷新：立即重新枚举启动项（可越过 2 秒最小间隔）"));
        }
        ImGui::SameLine();
        const ops::StartupItem* sel = res != nullptr ? Selected(*res) : nullptr;
        const bool needAdmin = sel != nullptr && StartupNeedsElevation(*sel, ctx.elevated);
        const char* adminSuffix = needAdmin ? U8(L"（需提权）") : "";
        char enable[64], disable[64];
        snprintf(enable, sizeof(enable), "%s%s", U8(L"启用"), adminSuffix);
        snprintf(disable, sizeof(disable), "%s%s", U8(L"禁用"), adminSuffix);
        ImGui::BeginDisabled(sel == nullptr || needAdmin);
        if (ImGui::Button(enable) && sel != nullptr) RequestConfirm(*sel, true);
        ImGui::SameLine();
        if (ImGui::Button(disable) && sel != nullptr) RequestConfirm(*sel, false);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (sel != nullptr) {
            ImGui::TextDisabled("%s", U8(Fmt(L"已选：{}", Truncate(sel->name, 32))));
        } else {
            ImGui::TextDisabled("%s", U8(L"未选择（点击行选择，右键操作）"));
        }
    }

    bool MatchesFilter(const ops::StartupItem& it) const {
        if (appliedFilter_.empty()) return true;
        return ContainsLower(it.name, appliedFilter_) ||
               ContainsLower(it.command, appliedFilter_) ||
               ContainsLower(it.location, appliedFilter_) ||
               ContainsLower(StartupSourceLabel(it.source), appliedFilter_);
    }

    void UpdateRows(const Result& res) {
        LowerFilter(filterUtf8_, filterWide_);
        if (filterWide_ == appliedFilter_ && lastData_ == &res) return;
        appliedFilter_ = filterWide_;
        lastData_ = &res;
        rows_.clear();
        for (size_t i = 0; i < res.data.size(); ++i) {
            if (MatchesFilter(res.data[i])) rows_.push_back(static_cast<int>(i));
        }
    }

    void DrawTable(AppContext& ctx, const Result& res) {
        if (rows_.empty()) {
            ImGui::TextColored(ColMuted(), "%s",
                               U8(res.data.empty() ? L"未发现启动项"
                                                   : L"没有匹配过滤条件的启动项"));
            return;
        }
        const int flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_SizingFixedFit;
        if (!ImGui::BeginTable("startup", 5, flags)) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(U8(L"名称"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn(U8(L"命令"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn(U8(L"位置"), ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableSetupColumn(U8(L"来源"), ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn(U8(L"状态"), ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows_.size()));
        while (clipper.Step()) {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                const ops::StartupItem& it =
                    res.data[static_cast<size_t>(rows_[static_cast<size_t>(r)])];
                ImGui::PushID(static_cast<int>(r));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool selected = !selectedId_.empty() && it.id == selectedId_;
                if (ImGui::Selectable(U8(Truncate(it.name, 48)), selected,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    selectedId_ = it.id;
                }
                if (ImGui::BeginPopupContextItem("##rowctx")) {
                    DrawRowMenu(ctx, it);
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(Truncate(it.command, 90)));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(Truncate(it.location, 70)));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(StartupSourceLabel(it.source)));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(it.enabled ? L"已启用" : L"已禁用"));
                if (StartupNeedsElevation(it, ctx.elevated)) {
                    ImGui::SameLine();
                    ImGui::TextColored(ColWarn(), "%s", U8(L"需提权"));
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    void DrawRowMenu(AppContext& ctx, const ops::StartupItem& it) {
        const bool needAdmin = StartupNeedsElevation(it, ctx.elevated);
        if (needAdmin) {
            ImGui::TextColored(ColWarn(), "%s", U8(L"该项需要管理员权限才能修改（需提权）"));
            ImGui::Separator();
        }
        ImGui::BeginDisabled(needAdmin);
        if (it.enabled) {
            if (ImGui::MenuItem(U8(L"禁用…"))) RequestConfirm(it, false);
        } else {
            if (ImGui::MenuItem(U8(L"启用…"))) RequestConfirm(it, true);
        }
        ImGui::EndDisabled();
        if (ImGui::MenuItem(U8(L"复制命令"))) {
            ImGui::SetClipboardText(U8(it.command));
        }
    }

    // ---- confirm + submit ---------------------------------------------------
    struct PendOp {
        bool active = false;
        bool openRequested = false;
        bool enable = false;
        ops::StartupItem item;
    };

    void RequestConfirm(const ops::StartupItem& it, bool enable) {
        pend_.active = true;
        pend_.openRequested = true;
        pend_.enable = enable;
        pend_.item = it;
    }

    void DrawConfirmDialog(AppContext& ctx) {
        if (!pend_.active) return;
        constexpr char kPopup[] = "##confirm_startup";
        if (pend_.openRequested) {
            ImGui::OpenPopup(kPopup);
            pend_.openRequested = false;
        }
        if (!ImGui::IsPopupOpen(kPopup)) {  // dismissed by clicking outside
            pend_ = PendOp{};
            return;
        }
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(460.0f, 0.0f), ImVec2(460.0f, FLT_MAX));
        if (!ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

        const wchar_t* action = pend_.enable ? L"确认启用" : L"确认禁用";
        ImGui::PushStyleColor(ImGuiCol_Text, ColFail());
        ImGui::TextUnformatted(U8(pend_.enable ? L"确认启用启动项" : L"确认禁用启动项"));
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::TextUnformatted(U8(Fmt(L"目标：{}", pend_.item.name)));
        ImGui::TextUnformatted(U8(Fmt(L"命令：{}", Truncate(pend_.item.command, 70))));
        ImGui::TextUnformatted(U8(Fmt(L"将写入的位置：{}", pend_.item.location)));
        ImGui::TextUnformatted(
            U8(Fmt(L"来源：{}（当前状态：{}）", StartupSourceLabel(pend_.item.source),
                   pend_.item.enabled ? L"已启用" : L"已禁用")));
        ImGui::TextDisabled("%s",
                            U8(L"写入前会先备份原值到本地日志目录，可随时恢复。"));
        ImGui::Separator();
        ImGui::SetKeyboardFocusHere(0);
        if (ImGui::Button(U8(L"取消"), ImVec2(120.0f, 0.0f))) {
            pend_ = PendOp{};
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        const bool go = ImGui::Button(U8(action), ImVec2(120.0f, 0.0f));
        ImGui::PopItemFlag();
        if (go) {
            Submit(ctx, pend_.item, pend_.enable);
            pend_ = PendOp{};
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void Submit(AppContext& ctx, const ops::StartupItem& item, bool enable) {
        std::shared_ptr<AppContext> app = LiveP3Ctx();
        if (!app) return;
        const bool elevated = ctx.elevated;
        app->jobs.Submit([app, item, enable, elevated] {
            std::wstring err;
            if (ops::SetStartupEnabled(item, enable, &err)) {
                PushNote(*app, Notification::Kind::JobDone,
                         Fmt(L"已{}启动项「{}」", enable ? L"启用" : L"禁用", item.name));
            } else {
                PushNote(*app, Notification::Kind::JobFailed,
                         Fmt(L"{}启动项「{}」失败：{}{}",
                             enable ? L"启用" : L"禁用", item.name,
                             err.empty() ? std::wstring(L"未知错误") : err,
                             FailSuffix(elevated)));
            }
        });
        refetchPending_ = true;  // rebuild the list once the op lands
    }

    AsyncFetch<std::vector<ops::StartupItem>> fetch_{2.0};
    std::string filterUtf8_;
    std::wstring filterWide_;
    std::wstring appliedFilter_;
    const Result* lastData_ = nullptr;
    std::vector<int> rows_;
    std::wstring selectedId_;
    uint64_t lastFrame_ = kNeverDrawn;
    PendOp pend_;
    bool refetchPending_ = false;
};

// ===========================================================================
// ServicePage: SCM services; start/stop with running-dependents warning.
// Data: ops::EnumServices via jobs, min 5 s between fetches + manual refresh.
// ===========================================================================

ImVec4 ServiceStateColor(uint32_t state) {
    switch (state) {
        case 4: return ColDone();                          // running
        case 2: case 3: case 5: case 6: return ColWarn();  // pending transitions
        case 7: return ColInfo();                          // paused
        case 1: return ColMuted();                         // stopped
        default: return ColFail();
    }
}

class ServicePage final : public IPage {
public:
    const wchar_t* Id() const override { return L"services"; }
    const wchar_t* Title() const override { return L"服务"; }

    void Draw(AppContext& ctx) override {
        BecameActive(lastFrame_);
        fetch_.MaybeFetch(Produce, false);  // 5 s rule + manual refresh
        if (refetchPending_ && !fetch_.Busy()) {
            refetchPending_ = false;
            fetch_.MaybeFetch(Produce, true);
        }
        std::shared_ptr<const Result> res = fetch_.Peek();

        DrawToolbar(ctx, res.get());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", U8(L"启动/停止服务通常需要管理员权限"));
        ImGui::Separator();

        if (res == nullptr) {
            DrawLoading();
        } else if (!res->ok && res->data.empty()) {
            bool retry = false;
            DrawLoadError(res->err, &retry);
            if (retry) fetch_.MaybeFetch(Produce, true);
        } else {
            if (!res->err.empty()) {
                ImGui::TextColored(ColWarn(), "%s",
                                   U8(Fmt(L"部分服务读取失败：{}", res->err)));
            }
            UpdateRows(*res);
            DrawTable(ctx, *res);
        }
        DrawConfirmDialog(ctx);
    }

private:
    using Result = AsyncFetch<std::vector<ops::ServiceInfo>>::Result;
    static std::vector<ops::ServiceInfo> Produce(std::wstring* err) {
        return ops::EnumServices(err);
    }

    const ops::ServiceInfo* Selected(const Result& res) const {
        if (selectedName_.empty()) return nullptr;
        for (const ops::ServiceInfo& s : res.data) {
            if (s.name == selectedName_) return &s;
        }
        return nullptr;
    }

    void DrawToolbar(AppContext& ctx, const Result* res) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", filterUtf8_.c_str());
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::InputTextWithHint("##svcfilter", U8(L"过滤服务名/显示名"), buf, sizeof(buf))) {
            filterUtf8_ = buf;
        }
        ImGui::SameLine();
        if (ImGui::Button(U8(L"刷新"))) fetch_.MaybeFetch(Produce, true);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                              U8(L"手动刷新：立即重新枚举服务（可越过 5 秒最小间隔）"));
        }
        ImGui::SameLine();
        const ops::ServiceInfo* sel = res != nullptr ? Selected(*res) : nullptr;
        const bool allowed = ctx.elevated;
        const char* suffix = allowed ? "" : U8(L"（需提权）");
        char start[64], stop[64];
        snprintf(start, sizeof(start), "%s%s", U8(L"启动"), suffix);
        snprintf(stop, sizeof(stop), "%s%s", U8(L"停止"), suffix);
        // P2-5: gate entries that are guaranteed to fail. A SERVICE_DISABLED
        // service cannot start (the contract has no enable op); a service whose
        // accepted-controls set lacks SERVICE_ACCEPT_STOP cannot stop.
        const bool canStart = sel != nullptr && sel->startType != 4;  // != SERVICE_DISABLED
        const bool canStop = sel != nullptr && sel->canStop;
        ImGui::BeginDisabled(sel == nullptr || !allowed || !canStart);
        if (ImGui::Button(start) && sel != nullptr) RequestConfirm(*sel, true);
        ImGui::EndDisabled();
        if (sel != nullptr && allowed && !canStart &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", U8(L"服务已禁用，无法启动（本应用不提供启用服务的操作）"));
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(sel == nullptr || !allowed || !canStop);
        if (ImGui::Button(stop) && sel != nullptr) RequestConfirm(*sel, false);
        ImGui::EndDisabled();
        if (sel != nullptr && allowed && !canStop &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", U8(L"该服务不接受停止控制（配置的接受控制集不含停止）"));
        }
        ImGui::SameLine();
        if (sel != nullptr) {
            ImGui::TextDisabled("%s",
                                U8(Fmt(L"已选：{}", Truncate(sel->displayName, 28))));
        } else {
            ImGui::TextDisabled("%s", U8(L"未选择（点击行选择，右键操作）"));
        }
    }

    bool MatchesFilter(const ops::ServiceInfo& s) const {
        if (appliedFilter_.empty()) return true;
        return ContainsLower(s.name, appliedFilter_) ||
               ContainsLower(s.displayName, appliedFilter_);
    }

    void UpdateRows(const Result& res) {
        LowerFilter(filterUtf8_, filterWide_);
        if (filterWide_ == appliedFilter_ && lastData_ == &res) return;
        appliedFilter_ = filterWide_;
        lastData_ = &res;
        rows_.clear();
        for (size_t i = 0; i < res.data.size(); ++i) {
            if (MatchesFilter(res.data[i])) rows_.push_back(static_cast<int>(i));
        }
    }

    void DrawTable(AppContext& ctx, const Result& res) {
        if (rows_.empty()) {
            ImGui::TextColored(ColMuted(), "%s",
                               U8(res.data.empty() ? L"未枚举到服务"
                                                   : L"没有匹配过滤条件的服务"));
            return;
        }
        const int flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_SizingFixedFit;
        if (!ImGui::BeginTable("services", 7, flags)) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(U8(L"名称"), ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn(U8(L"显示名"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn(U8(L"状态"), ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn(U8(L"启动类型"), ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn(U8(L"PID"), ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn(U8(L"账户"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(U8(L"描述"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows_.size()));
        while (clipper.Step()) {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                const ops::ServiceInfo& s =
                    res.data[static_cast<size_t>(rows_[static_cast<size_t>(r)])];
                ImGui::PushID(static_cast<int>(r));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool selected = !selectedName_.empty() && s.name == selectedName_;
                if (ImGui::Selectable(U8(Truncate(s.name, 40)), selected,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    selectedName_ = s.name;
                }
                if (ImGui::BeginPopupContextItem("##rowctx")) {
                    DrawRowMenu(ctx, s);
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(Truncate(s.displayName, 48)));
                ImGui::TableNextColumn();
                ImGui::TextColored(ServiceStateColor(s.state), "%s",
                                   U8(ServiceStateLabel(s.state)));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(ServiceStartTypeLabel(s.startType)));
                ImGui::TableNextColumn();
                if (s.pid != 0) {
                    ImGui::Text("%u", s.pid);
                    if (s.sharedProcess && ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s",
                                          U8(L"共享服务宿主（svchost）：该 PID 承载多个服务"));
                    }
                } else {
                    ImGui::TextDisabled("%s", U8(L"—"));
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(Truncate(s.account, 36)));
                ImGui::TableNextColumn();
                if (s.description.empty()) {
                    ImGui::TextDisabled("%s", U8(L"—"));
                } else {
                    ImGui::TextUnformatted(U8(Truncate(s.description, 56)));
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", U8(s.description));
                    }
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    void DrawRowMenu(AppContext& ctx, const ops::ServiceInfo& s) {
        const bool allowed = ctx.elevated;
        if (!allowed) {
            ImGui::TextColored(ColWarn(), "%s", U8(L"服务操作需要管理员权限（需提权）"));
            ImGui::Separator();
        }
        // P2-5: entries that would certainly fail stay visible but disabled,
        // with the reason in the label (P2-5) / greyed by elevation gate.
        ImGui::BeginDisabled(!allowed);
        if (s.state != 4 && s.state != 2) {  // not running / not start-pending
            if (s.startType == 4) {          // SERVICE_DISABLED
                ImGui::MenuItem(U8(L"启动（服务已禁用）"), nullptr, false, false);
            } else if (ImGui::MenuItem(U8(L"启动…"), nullptr, false, allowed)) {
                RequestConfirm(s, true);
            }
        }
        if (s.state == 4 || s.state == 7) {  // running / paused
            if (!s.canStop) {
                ImGui::MenuItem(U8(L"停止（不接受停止控制）"), nullptr, false, false);
            } else if (ImGui::MenuItem(U8(L"停止…"), nullptr, false, allowed)) {
                RequestConfirm(s, false);
            }
        }
        ImGui::EndDisabled();
        if (ImGui::MenuItem(U8(L"复制服务名"))) ImGui::SetClipboardText(U8(s.name));
    }

    // ---- confirm + submit ---------------------------------------------------
    // Running-dependents plan arrives asynchronously (like the kill-tree plan).
    struct DepPlan {
        std::mutex mu;
        bool ready = false;
        std::vector<std::wstring> names;
    };

    struct PendOp {
        bool active = false;
        bool openRequested = false;
        bool start = false;
        ops::ServiceInfo svc;
        std::shared_ptr<DepPlan> plan;  // stop only
    };

    void RequestConfirm(const ops::ServiceInfo& s, bool start) {
        pend_ = PendOp{};
        pend_.active = true;
        pend_.openRequested = true;
        pend_.start = start;
        pend_.svc = s;
        if (!start) {
            pend_.plan = std::make_shared<DepPlan>();
            std::shared_ptr<AppContext> app = LiveP3Ctx();
            std::shared_ptr<DepPlan> plan = pend_.plan;
            if (app) {
                app->jobs.Submit([app, plan, name = s.name] {
                    std::vector<std::wstring> deps = ops::GetDependentServices(name);
                    std::lock_guard<std::mutex> lock(plan->mu);
                    plan->names = std::move(deps);
                    plan->ready = true;
                });
            } else {
                std::lock_guard<std::mutex> lock(plan->mu);
                plan->ready = true;
            }
        }
    }

    void DrawConfirmDialog(AppContext& ctx) {
        if (!pend_.active) return;
        constexpr char kPopup[] = "##confirm_service";
        if (pend_.openRequested) {
            ImGui::OpenPopup(kPopup);
            pend_.openRequested = false;
        }
        if (!ImGui::IsPopupOpen(kPopup)) {
            pend_ = PendOp{};
            return;
        }
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(460.0f, 0.0f), ImVec2(460.0f, FLT_MAX));
        if (!ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

        const wchar_t* action = pend_.start ? L"确认启动" : L"确认停止";
        ImGui::PushStyleColor(ImGuiCol_Text, ColFail());
        ImGui::TextUnformatted(U8(pend_.start ? L"确认启动服务" : L"确认停止服务"));
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::TextUnformatted(
            U8(Fmt(L"目标：{}（{}）", pend_.svc.displayName, pend_.svc.name)));
        ImGui::TextUnformatted(
            U8(Fmt(L"当前状态：{}，启动类型：{}", ServiceStateLabel(pend_.svc.state),
                   ServiceStartTypeLabel(pend_.svc.startType))));
        if (!pend_.start) {
            bool ready = false;
            std::vector<std::wstring> deps;
            if (pend_.plan) {
                std::lock_guard<std::mutex> lock(pend_.plan->mu);
                ready = pend_.plan->ready;
                deps = pend_.plan->names;
            }
            if (!ready) {
                ImGui::TextDisabled("%s", U8(L"正在查询运行中的依赖服务…"));
            } else if (!deps.empty()) {
                std::wstring joined;
                for (size_t i = 0; i < deps.size(); ++i) {
                    if (i > 0) joined += L"、";
                    joined += deps[i];
                }
                ImGui::TextColored(
                    ColFail(), "%s",
                    U8(Fmt(L"警告：以下 {} 个运行中的服务依赖它，停止后它们也会受影响：{}",
                           deps.size(), joined)));
            }
            ImGui::TextDisabled("%s",
                                U8(L"不会强制停止依赖服务；若存在运行中的依赖，停止可能失败。"));
        } else {
            ImGui::TextDisabled("%s",
                                U8(L"启动失败时通常是因为缺少管理员权限或服务已被禁用。"));
        }
        ImGui::Separator();
        ImGui::SetKeyboardFocusHere(0);
        if (ImGui::Button(U8(L"取消"), ImVec2(120.0f, 0.0f))) {
            pend_ = PendOp{};
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        const bool go = ImGui::Button(U8(action), ImVec2(120.0f, 0.0f));
        ImGui::PopItemFlag();
        if (go) {
            Submit(ctx, pend_.svc, pend_.start);
            pend_ = PendOp{};
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void Submit(AppContext& ctx, const ops::ServiceInfo& s, bool start) {
        std::shared_ptr<AppContext> app = LiveP3Ctx();
        if (!app) return;
        const bool elevated = ctx.elevated;
        app->jobs.Submit([app, s, start, elevated] {
            std::wstring err;
            const bool ok = start ? ops::StartServiceByName(s.name, &err)
                                  : ops::StopServiceByName(s.name, false, &err);
            if (ok) {
                PushNote(*app, Notification::Kind::JobDone,
                         Fmt(L"已{}服务「{}」", start ? L"启动" : L"停止", s.displayName));
            } else {
                PushNote(*app, Notification::Kind::JobFailed,
                         Fmt(L"{}服务「{}」失败：{}{}", start ? L"启动" : L"停止",
                             s.displayName, err.empty() ? std::wstring(L"未知错误") : err,
                             FailSuffix(elevated)));
            }
        });
        refetchPending_ = true;
    }

    AsyncFetch<std::vector<ops::ServiceInfo>> fetch_{5.0};
    std::string filterUtf8_;
    std::wstring filterWide_;
    std::wstring appliedFilter_;
    const Result* lastData_ = nullptr;
    std::vector<int> rows_;
    std::wstring selectedName_;
    uint64_t lastFrame_ = kNeverDrawn;
    PendOp pend_;
    bool refetchPending_ = false;
};

// ===========================================================================
// DriverPage: loaded kernel drivers; signature checked on demand (selection).
// Data: ops::EnumDrivers via jobs. 24H2+ non-elevated degrades to a full-page
// notice with the shared elevate button (contract error 需要管理员权限).
// ===========================================================================

class DriverPage final : public IPage {
public:
    const wchar_t* Id() const override { return L"drivers"; }
    const wchar_t* Title() const override { return L"驱动"; }

    void Draw(AppContext& ctx) override {
        BecameActive(lastFrame_);
        fetch_.MaybeFetch(Produce, false);
        std::shared_ptr<const Result> res = fetch_.Peek();

        DrawToolbar(ctx, res.get());
        ImGui::Separator();

        if (res == nullptr) {
            DrawLoading();
            return;
        }
        if (!res->ok && DriverErrNeedsAdmin(res->err)) {
            DrawDegraded(ctx, res->err);
            return;
        }
        if (!res->ok && res->data.empty()) {
            bool retry = false;
            DrawLoadError(res->err, &retry);
            if (retry) fetch_.MaybeFetch(Produce, true);
            return;
        }
        if (!res->err.empty()) {  // P2-3: partial-data banner, same as other pages
            ImGui::TextColored(ColWarn(), "%s",
                               U8(Fmt(L"部分数据不可用：{}", res->err)));
        }
        DrawTable(ctx, *res);
    }

private:
    using Result = AsyncFetch<std::vector<ops::DriverInfo>>::Result;
    static std::vector<ops::DriverInfo> Produce(std::wstring* err) {
        return ops::EnumDrivers(err);
    }

    struct SigSlot {
        std::atomic<int> state{-1};  // -1 pending, else ops::SigState as int
    };

    void DrawDegraded(AppContext& ctx, const std::wstring& err) {
        ImGui::TextColored(ColWarn(), "%s", U8(L"驱动列表需要管理员权限"));
        ImGui::TextWrapped(
            "%s",
            U8(Fmt(L"枚举内核驱动失败：{}。当前 Windows 版本需要以管理员身份运行才能获取"
                   L"完整的驱动列表（否则只会得到空地址，本应用拒绝显示假数据）。",
                   err)));
        DrawElevateButton(ctx);
        ImGui::SameLine();
        if (ImGui::Button(U8(L"重试"))) fetch_.MaybeFetch(Produce, true);
    }

    void DrawToolbar(AppContext& ctx, const Result* res) {
        (void)ctx;
        if (res != nullptr) {
            ImGui::TextUnformatted(U8(Fmt(L"共 {} 个内核驱动", res->data.size())));
        } else {
            ImGui::TextColored(ColMuted(), "%s", U8(L"列表加载中…"));
        }
        ImGui::SameLine();
        if (ImGui::Button(U8(L"刷新"))) fetch_.MaybeFetch(Produce, true);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                              U8(L"手动刷新：立即重新枚举驱动（可越过 5 秒最小间隔）"));
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", U8(L"选中一行后自动校验其文件签名"));
    }

    void EnsureSig(const std::wstring& path) {
        if (path.empty() || sigs_.find(path) != sigs_.end()) return;
        std::shared_ptr<SigSlot> slot = std::make_shared<SigSlot>();
        sigs_.emplace(path, slot);
        std::shared_ptr<AppContext> app = LiveP3Ctx();
        if (!app) return;
        app->jobs.Submit([app, slot, path] {
            slot->state.store(static_cast<int>(ops::VerifyFileSignature(path)));
        });
    }

    void DrawSigCell(const ops::DriverInfo& d) {
        if (d.path.empty()) {
            ImGui::TextDisabled("%s", U8(L"—"));
            return;
        }
        const auto it = sigs_.find(d.path);
        if (it == sigs_.end()) {
            ImGui::TextDisabled("%s", U8(L"未查询"));
            return;
        }
        switch (static_cast<ops::SigState>(it->second->state.load())) {
            case ops::SigState::Valid:
                ImGui::TextColored(ColDone(), "%s", U8(L"有效签名"));
                break;
            case ops::SigState::Unsigned:
                ImGui::TextColored(ColWarn(), "%s", U8(L"未签名"));
                break;
            case ops::SigState::Invalid:
                ImGui::TextColored(ColFail(), "%s", U8(L"签名无效"));
                break;
            case ops::SigState::Unknown:
                ImGui::TextDisabled("%s", U8(L"未知"));
                break;
            case ops::SigState::NoCheck:
            default:  // -1: job still in flight
                ImGui::TextDisabled("%s", U8(L"查询中…"));
                break;
        }
    }

    void DrawTable(AppContext& ctx, const Result& res) {
        (void)ctx;
        if (res.data.empty()) {
            ImGui::TextColored(ColMuted(), "%s", U8(L"未枚举到驱动（异常情况，请重试）"));
            return;
        }
        const int flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_SizingFixedFit;
        if (!ImGui::BeginTable("drivers", 5, flags)) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(U8(L"名称"), ImGuiTableColumnFlags_WidthFixed, 190.0f);
        ImGui::TableSetupColumn(U8(L"路径"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn(U8(L"大小"), ImGuiTableColumnFlags_WidthFixed, 84.0f);
        ImGui::TableSetupColumn(U8(L"基址"), ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn(U8(L"签名"), ImGuiTableColumnFlags_WidthFixed, 92.0f);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(res.data.size()));
        while (clipper.Step()) {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                const ops::DriverInfo& d = res.data[static_cast<size_t>(r)];
                ImGui::PushID(static_cast<int>(r));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool selected = selectedPath_ == d.path && !d.path.empty();
                if (ImGui::Selectable(U8(Truncate(d.name, 44)), selected,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    selectedPath_ = d.path;
                    EnsureSig(d.path);  // signature check triggered by selection
                }
                if (ImGui::BeginPopupContextItem("##rowctx")) {
                    if (ImGui::MenuItem(U8(L"复制路径"))) {
                        ImGui::SetClipboardText(U8(d.path));
                    }
                    if (ImGui::MenuItem(U8(L"重新校验签名"), nullptr, false, !d.path.empty())) {
                        sigs_.erase(d.path);
                        selectedPath_ = d.path;
                        EnsureSig(d.path);
                    }
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(Truncate(d.path, 110)));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(FormatBytes(d.imageSize)));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    U8(d.imageBase != 0 ? Fmt(L"0x{:X}", d.imageBase) : std::wstring(L"—")));
                ImGui::TableNextColumn();
                DrawSigCell(d);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    AsyncFetch<std::vector<ops::DriverInfo>> fetch_{5.0};
    std::wstring selectedPath_;
    std::map<std::wstring, std::shared_ptr<SigSlot>> sigs_;
    uint64_t lastFrame_ = kNeverDrawn;
};

// ===========================================================================
// SensorPage: grouped cards (CPU / GPU / fans) + disk health table.
// Data: collect::ReadSensors via jobs, min 10 s (SMART/temperature queries are
// heavy). The honest trichotomy of SensorReading::State drives the rendering.
// ===========================================================================

class SensorPage final : public IPage {
public:
    const wchar_t* Id() const override { return L"sensors"; }
    const wchar_t* Title() const override { return L"传感器"; }

    void Draw(AppContext& ctx) override {
        BecameActive(lastFrame_);
        fetch_.MaybeFetch(Produce, false);  // 10 s rule + manual refresh only
        std::shared_ptr<const Result> res = fetch_.Peek();

        if (ImGui::Button(U8(L"刷新"))) fetch_.MaybeFetch(Produce, true);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "%s",
                U8(L"手动刷新：立即重新读取传感器（可越过 10 秒最小间隔；SMART/温度查询"
                   L"较重，自动刷新受该间隔限制）"));
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", U8(L"仅使用公开的用户模式数据源，不内置内核驱动"));
        ImGui::Separator();

        if (res == nullptr) {
            DrawLoading();
            return;
        }
        if (!res->err.empty() && AllEmpty(res->data)) {
            bool retry = false;
            DrawLoadError(res->err, &retry);
            if (retry) fetch_.MaybeFetch(Produce, true);
            return;
        }
        if (!res->err.empty()) {
            ImGui::TextColored(ColWarn(), "%s",
                               U8(Fmt(L"部分传感器读取失败：{}", res->err)));
        }
        if (!res->data.notes.empty()) {
            ImGui::TextWrapped("%s", U8(Fmt(L"说明：{}", res->data.notes)));
        }

        const SensorSnapshot& snap = res->data;
        const float half = (ImGui::GetContentRegionAvail().x -
                            ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        DrawGroup(ctx, "##grp_cpu", half, L"CPU", snap.cpu);
        ImGui::SameLine();
        DrawGroup(ctx, "##grp_gpu", half, L"GPU", snap.gpu);
        ImGui::SameLine();
        DrawGroup(ctx, "##grp_fan", half, L"风扇", snap.fans);
        ImGui::NewLine();
        DrawDisks(ctx, snap.disks);
    }

private:
    using Result = AsyncFetch<SensorSnapshot>::Result;
    static SensorSnapshot Produce(std::wstring* err) { return ReadSensors(err); }

    static bool AllEmpty(const SensorSnapshot& s) {
        return s.cpu.empty() && s.gpu.empty() && s.disks.empty() && s.fans.empty();
    }

    static void DrawGroup(AppContext& ctx, const char* id, float width,
                          const wchar_t* title, const std::vector<SensorReading>& items) {
        ImGui::BeginChild(id, ImVec2(width, 0.0f),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        ImGui::TextUnformatted(U8(title));
        ImGui::Separator();
        if (items.empty()) {
            ImGui::TextDisabled("%s", U8(L"本机无此类传感器数据"));
        } else {
            for (const SensorReading& s : items) DrawReading(ctx, s);
        }
        ImGui::EndChild();
    }

    static void DrawReading(AppContext& ctx, const SensorReading& s) {
        ImGui::TextDisabled("%s", U8(Truncate(s.label, 30)));
        if (s.label.size() > 30 && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", U8(s.label));
        }
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 170.0f);
        switch (s.state) {
            case SensorReading::State::Ok:
                ImGui::TextUnformatted(U8(Fmt(L"{:.1f} {}", s.value, s.unit)));
                break;
            case SensorReading::State::NeedAdmin: {
                ImGui::TextColored(ColWarn(), "%s", U8(L"需要管理员权限"));
                if (!ctx.elevated && ops::CanElevate()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(U8(L"提权重启"))) {
                        SaveSessionFromCtx(ctx, nullptr);
                        if (ops::RelaunchAsAdmin(L"--relaunched")) ctx.wantExit = true;
                    }
                }
                break;
            }
            case SensorReading::State::NeedDriver:
                ImGui::TextColored(ColMuted(), "%s",
                                   U8(L"需要驱动支持（本应用不内置内核驱动）"));
                break;
            case SensorReading::State::NoHardware:
            default:
                ImGui::TextColored(ColMuted(), "%s", U8(L"本机无此传感器"));
                break;
        }
    }

    static void DrawDisks(AppContext& ctx, const std::vector<DiskHealth>& disks) {
        ImGui::BeginChild("##grp_disk", ImVec2(0.0f, 0.0f),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        ImGui::TextUnformatted(U8(L"磁盘健康"));
        ImGui::Separator();
        if (disks.empty()) {
            ImGui::TextDisabled("%s", U8(L"本机未发现可查询的磁盘"));
        } else {
            const int flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_SizingFixedFit;
            if (ImGui::BeginTable("disks", 5, flags)) {
                ImGui::TableSetupColumn(U8(L"型号"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
                ImGui::TableSetupColumn(U8(L"总线"), ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn(U8(L"健康"), ImGuiTableColumnFlags_WidthFixed, 90.0f);
                ImGui::TableSetupColumn(U8(L"温度"), ImGuiTableColumnFlags_WidthFixed, 180.0f);
                ImGui::TableSetupColumn(U8(L"通电时长"), ImGuiTableColumnFlags_WidthFixed, 110.0f);
                ImGui::TableHeadersRow();
                for (const DiskHealth& d : disks) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(
                        U8(d.model.empty() ? std::wstring(L"—") : d.model));
                    if (ImGui::IsItemHovered() && !d.serial.empty()) {
                        ImGui::SetTooltip("%s", U8(Fmt(L"序列号：{}", d.serial)));
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(
                        U8(d.busType.empty() ? std::wstring(L"—") : d.busType));
                    ImGui::TableNextColumn();
                    if (d.health.find(L"良好") != std::wstring::npos) {
                        ImGui::TextColored(ColDone(), "%s", U8(d.health));
                    } else if (d.health.find(L"警告") != std::wstring::npos) {
                        ImGui::TextColored(ColFail(), "%s", U8(d.health));
                    } else {
                        ImGui::TextColored(
                            ColMuted(), "%s",
                            U8(d.health.empty() ? std::wstring(L"未知") : d.health));
                    }
                    ImGui::TableNextColumn();
                    DrawTempCell(ctx, d);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(
                        U8(d.powerOnHours == kUnavailU64
                               ? std::wstring(L"—")
                               : Fmt(L"{} 小时", d.powerOnHours)));
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();
    }

    static void DrawTempCell(AppContext& ctx, const DiskHealth& d) {
        switch (d.tempState) {
            case SensorReading::State::Ok:
                ImGui::TextUnformatted(U8(Fmt(L"{:.1f} °C", d.tempC)));
                break;
            case SensorReading::State::NeedAdmin: {
                ImGui::TextColored(ColWarn(), "%s", U8(L"需管理员权限"));
                if (!ctx.elevated && ops::CanElevate()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(U8(L"提权"))) {
                        SaveSessionFromCtx(ctx, nullptr);
                        if (ops::RelaunchAsAdmin(L"--relaunched")) ctx.wantExit = true;
                    }
                }
                break;
            }
            case SensorReading::State::NeedDriver:
                ImGui::TextColored(ColMuted(), "%s", U8(L"需驱动支持"));
                break;
            case SensorReading::State::NoHardware:
            default:
                ImGui::TextColored(ColMuted(), "%s", U8(L"无此传感器"));
                break;
        }
    }

    AsyncFetch<SensorSnapshot> fetch_{10.0};
    uint64_t lastFrame_ = kNeverDrawn;
};

// ===========================================================================
// Threshold alerts (optional phase-3 extra): CPU>90% / memory>95% watched on
// the UI thread (the values already arrive with every snapshot). One balloon
// + one toast per episode, 5 min per-metric cooldown, re-armed with a 5%
// hysteresis. Config: alertOn (default off) / alertCpu / alertMem.
// ===========================================================================

struct AlertState {
    bool armedCpu = true;
    bool armedMem = true;
    double lastFireCpu = -1.0e9;
    double lastFireMem = -1.0e9;
};

AlertState& Alerts() {
    static AlertState s;
    return s;
}

constexpr double kAlertCooldownSec = 300.0;
constexpr double kAlertRearmDelta = 5.0;

void FireAlert(AppContext& ctx, const wchar_t* what, double value, double threshold) {
    const std::wstring text = Fmt(L"{}使用率 {:.0f}%，超过阈值 {:.0f}%", what, value, threshold);
    PushNote(ctx, Notification::Kind::Warn, text);
    BalloonSink& sink = SinkSlot();
    if (sink) sink(L"超级任务管理器资源告警", text);
}

}  // namespace

// ===========================================================================
// Public entry points (app/ui3/Pages3.h contract).
// ===========================================================================

std::shared_ptr<AppContext>& P3Slot() {
    static std::shared_ptr<AppContext> c;
    return c;
}

void BindPhase3Context(std::shared_ptr<AppContext> ctx) { P3Slot() = std::move(ctx); }

std::shared_ptr<AppContext> LiveP3Ctx() { return P3Slot(); }

BalloonSink& SinkSlot() {
    static BalloonSink s;
    return s;
}

void SetBalloonSink(BalloonSink sink) { SinkSlot() = std::move(sink); }

void RegisterPhase3Pages(AppContext& ctx) {
    ctx.pages.push_back(std::make_unique<NetworkPage>());
    ctx.pages.push_back(std::make_unique<StartupPage>());
    ctx.pages.push_back(std::make_unique<ServicePage>());
    ctx.pages.push_back(std::make_unique<DriverPage>());
    ctx.pages.push_back(std::make_unique<SensorPage>());
    // Apply the persisted ETW switch before the collector starts (main calls
    // RegisterPages before CollectService::Start).
    ctx.collect.SetNetEtwEnabled(ctx.cfg.GetBool(L"netEtw", false));
}

void AlertTick(AppContext& ctx) {
    AlertState& a = Alerts();
    if (!ctx.cfg.GetBool(L"alertOn", false)) {
        a.armedCpu = true;  // stay re-armed while disabled
        a.armedMem = true;
        return;
    }
    const double cpuThr = static_cast<double>(ctx.cfg.GetInt(L"alertCpu", 90));
    const double memThr = static_cast<double>(ctx.cfg.GetInt(L"alertMem", 95));
    std::shared_ptr<const Snapshot> snap = ctx.collect.Store().Get();
    const SystemInfo& sys = snap->sys;
    const double now = ImGui::GetTime();

    if (sys.cpuTotalPercent == sys.cpuTotalPercent) {  // NaN check
        if (a.armedCpu && sys.cpuTotalPercent > cpuThr &&
            now - a.lastFireCpu >= kAlertCooldownSec) {
            a.armedCpu = false;
            a.lastFireCpu = now;
            FireAlert(ctx, L"CPU", sys.cpuTotalPercent, cpuThr);
        } else if (sys.cpuTotalPercent < cpuThr - kAlertRearmDelta) {
            a.armedCpu = true;
        }
    }
    if (sys.physTotal > 0) {
        const double memPct = static_cast<double>(sys.physTotal - sys.physAvail) *
                              100.0 / static_cast<double>(sys.physTotal);
        if (a.armedMem && memPct > memThr && now - a.lastFireMem >= kAlertCooldownSec) {
            a.armedMem = false;
            a.lastFireMem = now;
            FireAlert(ctx, L"内存", memPct, memThr);
        } else if (memPct < memThr - kAlertRearmDelta) {
            a.armedMem = true;
        }
    }
}

void DrawAlertControls(AppContext& ctx) {
    bool on = ctx.cfg.GetBool(L"alertOn", false);
    int cpu = static_cast<int>(ctx.cfg.GetInt(L"alertCpu", 90));
    int mem = static_cast<int>(ctx.cfg.GetInt(L"alertMem", 95));
    cpu = std::max(1, std::min(100, cpu));
    mem = std::max(1, std::min(100, mem));
    ImGui::Separator();
    if (ImGui::Checkbox(U8(L"资源告警"), &on)) {
        ctx.cfg.SetBool(L"alertOn", on);
        PushNote(ctx, Notification::Kind::Info,
                 on ? L"已开启资源告警（CPU/内存阈值）" : L"已关闭资源告警");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "%s",
            U8(L"CPU 或内存占用超过阈值时弹一次托盘气泡和提示，回落 5% 后重新监视；"
               L"同一指标 5 分钟内不重复提醒。默认关闭。"));
    }
    if (on) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", U8(L"CPU >"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(64.0f);
        if (ImGui::InputInt("##alertCpu", &cpu, 0, 0)) {
            cpu = std::max(1, std::min(100, cpu));
            ctx.cfg.SetInt(L"alertCpu", cpu);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", U8(L"%"));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", U8(L"内存 >"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(64.0f);
        if (ImGui::InputInt("##alertMem", &mem, 0, 0)) {
            mem = std::max(1, std::min(100, mem));
            ctx.cfg.SetInt(L"alertMem", mem);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", U8(L"%"));
    }
}

bool& SmokeDrawAllSlot() {
    static bool on = false;
    return on;
}

void SetSmokeDrawAll(bool on) { SmokeDrawAllSlot() = on; }

void DrawSmokeAllPages(AppContext& ctx) {
    if (!SmokeDrawAllSlot() || ctx.pages.empty()) return;
    // Offscreen window: exercises every page's Draw path (empty/error states)
    // under --smoke without disturbing the visible shell.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x + 24.0f, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(900.0f, 640.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                   ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("##smoke_all_pages", nullptr, flags)) {
        for (std::unique_ptr<IPage>& p : ctx.pages) p->Draw(ctx);
    }
    ImGui::End();
}

}  // namespace ui3
}  // namespace stm
