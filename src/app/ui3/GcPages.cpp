// F4 落地批（G-C）新增页面：崩溃记录（F4#5）与窗口管理（F4 次梯队），
// 以及跨页联动共享模态"宿主服务"（F4#3）与全局热键（F4#10）。
// 设计规则与 Pages.cpp / Pages3.cpp 一致：
//  - 全部中文文案经 ui::U8()；不可用值渲染 "—"；失败如实展示原因
//  - 数据经 ops 队列（ui3::AsyncFetch）拉取，UI 线程零阻塞
//  - 有副作用的操作（关闭窗口）走两段式确认对话框（取消首位 + 键盘焦点）
//  - 窗口操作仅为文档化 API 的纯 UI 工具（见 WindowUtil.h 定位说明），
//    不提供"强制结束窗口"
#include "app/ui3/GcPages.h"
#include "app/ui3/AsyncFetch.h"
#include "app/ui3/CrashUi.h"
#include "app/ui3/JumpState.h"  // FilterServicesByPid
#include "app/ui3/PageHelpers.h"
#include "app/ui3/WindowUtil.h"
#include "app/ui/UiText.h"
#include "core/Str.h"
#include "ops/CrashLog.h"
#include "ops/ServiceOps.h"
#include "imgui.h"
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace stm {
namespace ui3 {

namespace {

using ui::U8;

// ---- 共享小工具（与 Pages3.cpp 同款，本地副本避免跨 TU 耦合） ----------------

ImVec4 ColDone() { return ImVec4(0.45f, 0.80f, 0.45f, 1.0f); }
ImVec4 ColFail() { return ImVec4(0.92f, 0.36f, 0.36f, 1.0f); }
ImVec4 ColWarn() { return ImVec4(0.95f, 0.78f, 0.30f, 1.0f); }
ImVec4 ColMuted() { return ImVec4(0.60f, 0.62f, 0.68f, 1.0f); }

constexpr uint64_t kNeverDrawn = ~0ull;
bool BecameActive(uint64_t& lastFrame) {
    const uint64_t f = ImGui::GetFrameCount();
    const bool active = lastFrame == kNeverDrawn || f > lastFrame + 1;
    lastFrame = f;
    return active;
}

std::wstring LowerCopy(const std::wstring& s) {
    std::wstring lower(s.size(), L'\0');
    std::transform(s.begin(), s.end(), lower.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return lower;
}

std::wstring Truncate(const std::wstring& s, size_t maxChars) {
    if (s.size() <= maxChars) return s;
    return s.substr(0, maxChars) + L"…";
}

void PushNote(AppContext& ctx, Notification::Kind kind, const std::wstring& text) {
    Notification n;
    n.kind = kind;
    n.text = text;
    ctx.notes.Push(n);
}

// Note with teardown safety: LiveP3Ctx() may be null while the app exits —
// skip the note instead of dereferencing null (never fail loudly at teardown).
void PushLiveNote(Notification::Kind kind, const std::wstring& text) {
    if (std::shared_ptr<AppContext> app = LiveP3Ctx()) PushNote(*app, kind, text);
}

void DrawLoading() {
    ImGui::TextColored(ColMuted(), "%s", U8(L"加载中…"));
}

void DrawLoadError(const std::wstring& e, bool* retry) {
    ImGui::TextColored(ColFail(), "%s",
                       U8(Fmt(L"加载失败：{}", e.empty() ? std::wstring(L"未知错误") : e)));
    if (ImGui::Button(U8(L"重试"))) *retry = true;
}

// ===========================================================================
// CrashPage（F4#5）：Application+System 通道 1000/1001/1002 事件，只读。
// ===========================================================================

class CrashPage final : public IPage {
public:
    const wchar_t* Id() const override { return L"crashes"; }
    const wchar_t* Title() const override { return L"崩溃记录"; }

    void Draw(AppContext& ctx) override {
        const bool becameActive = BecameActive(lastFrame_);
        fetch_.MaybeFetch(Produce, becameActive);

        if (ImGui::Button(U8(L"刷新"))) fetch_.MaybeFetch(Produce, true);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", U8(L"手动刷新：立即重新查询事件日志（可越过 10 秒最小间隔）"));
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s",
                            U8(L"最近 200 条（Application + System 通道，只读；事件 ID "
                               L"1000 应用错误 / 1001 错误报告 / 1002 应用挂起）"));
        ImGui::Separator();

        std::shared_ptr<const Result> res = fetch_.Peek();
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
        if (res->data.empty()) {
            // 诚实空态：查询成功但确实没有近期记录
            ImGui::TextColored(ColMuted(), "%s", U8(L"近期没有崩溃/挂起记录"));
            return;
        }
        if (!res->err.empty()) {
            ImGui::TextColored(ColWarn(), "%s", U8(Fmt(L"部分事件读取失败：{}", res->err)));
        }
        DrawTable(*res);
        (void)ctx;
    }

private:
    using Result = AsyncFetch<std::vector<ops::CrashEvent>>::Result;
    static std::vector<ops::CrashEvent> Produce(std::wstring* err) {
        return ops::QueryCrashEvents(200, err);
    }

    static ImVec4 LevelColor(uint16_t level) {
        if (level == 1 || level == 2) return ColFail();  // 严重/错误
        if (level == 3) return ColWarn();                // 警告
        return ColMuted();
    }

    void DrawTable(const Result& res) {
        const int flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_SizingFixedFit;
        if (!ImGui::BeginTable("crashes", 7, flags)) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(U8(L"时间"), ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn(U8(L"级别"), ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableSetupColumn(U8(L"事件ID"), ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn(U8(L"来源"), ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn(U8(L"应用"), ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn(U8(L"模块"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn(U8(L"摘要"), ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(res.data.size()));
        while (clipper.Step()) {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                const ops::CrashEvent& ev = res.data[static_cast<size_t>(r)];
                ImGui::PushID(static_cast<int>(r));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(FormatUnixTimeLocal(ev.unixTime)));
                ImGui::TableNextColumn();
                ImGui::TextColored(LevelColor(ev.level), "%s", U8(CrashLevelLabel(ev.level)));
                ImGui::TableNextColumn();
                ImGui::Text("%u", ev.eventId);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", U8(CrashEventIdLabel(ev.eventId)));
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    U8(ev.provider.empty() ? std::wstring(L"—") : Truncate(ev.provider, 28)));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    U8(ev.app.empty() ? std::wstring(L"—") : Truncate(ev.app, 60)));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    U8(ev.module.empty() ? std::wstring(L"—") : Truncate(ev.module, 46)));
                ImGui::TableNextColumn();
                if (ev.summary.empty()) {
                    ImGui::TextDisabled("%s", U8(L"—"));
                } else {
                    ImGui::TextUnformatted(U8(Truncate(ev.summary, 110)));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", U8(ev.summary));
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    AsyncFetch<std::vector<ops::CrashEvent>> fetch_{10.0};
    uint64_t lastFrame_ = kNeverDrawn;
};

// ===========================================================================
// WindowPage（F4 次梯队）：顶层窗口枚举 + 前置/置顶/最小化/温和关闭。
// ===========================================================================

class WindowPage final : public IPage {
public:
    const wchar_t* Id() const override { return L"windows"; }
    const wchar_t* Title() const override { return L"窗口"; }

    void Draw(AppContext& ctx) override {
        const bool becameActive = BecameActive(lastFrame_);
        fetch_.MaybeFetch(Produce, becameActive);
        std::shared_ptr<const Result> res = fetch_.Peek();

        DrawToolbar(res.get());
        ImGui::Separator();

        if (res == nullptr) {
            DrawLoading();
        } else if (!res->ok && res->data.empty()) {
            bool retry = false;
            DrawLoadError(res->err, &retry);
            if (retry) fetch_.MaybeFetch(Produce, true);
        } else {
            UpdateRows(*res);
            DrawTable(ctx, *res);
        }
        DrawCloseConfirm(ctx);
    }

private:
    using Result = AsyncFetch<std::vector<WindowEntry>>::Result;
    static std::vector<WindowEntry> Produce(std::wstring* err) {
        return EnumTopLevelWindows(err);
    }

    const WindowEntry* Selected(const Result& res) const {
        if (selectedHwnd_ == nullptr) return nullptr;
        for (const WindowEntry& w : res.data) {
            if (w.hwnd == selectedHwnd_) return &w;
        }
        return nullptr;
    }

    void DrawToolbar(const Result* res) {
        if (ImGui::Button(U8(L"刷新"))) fetch_.MaybeFetch(Produce, true);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", U8(L"重新枚举顶层可见窗口（按需枚举，不自动刷新）"));
        }
        ImGui::SameLine();
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", filterUtf8_.c_str());
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::InputTextWithHint("##winfilter", U8(L"过滤标题/类名/进程名"), buf,
                                     sizeof(buf))) {
            filterUtf8_ = buf;
        }
        ImGui::SameLine();
        if (res != nullptr) {
            ImGui::TextDisabled("%s", U8(Fmt(L"{} / {} 个可见窗口", rows_.size(), res->data.size())));
        } else {
            ImGui::TextColored(ColMuted(), "%s", U8(L"统计加载中…"));
        }
        // 行内右键 + 工具条按钮双入口；关闭走确认对话框。
        const WindowEntry* sel = res != nullptr ? Selected(*res) : nullptr;
        ImGui::SameLine();
        if (ImGui::Button(U8(L"前置")) && sel != nullptr) {
            std::wstring e;
            if (ForegroundWindowSafe(sel->hwnd, &e)) {
                PushLiveNote(Notification::Kind::JobDone, L"已将窗口切到前台");
            } else {
                PushLiveNote(Notification::Kind::JobFailed, e);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(U8(L"置顶切换")) && sel != nullptr) {
            std::wstring e;
            if (SetWindowTopmost(sel->hwnd, !sel->topmost, &e)) {
                PushLiveNote(Notification::Kind::JobDone,
                         sel->topmost ? L"已取消置顶" : L"已置顶窗口");
                fetch_.MaybeFetch(Produce, true);  // 状态列回读刷新
            } else {
                PushLiveNote(Notification::Kind::JobFailed, e);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(U8(L"最小化")) && sel != nullptr) {
            std::wstring e;
            MinimizeWindow(sel->hwnd, &e);
            fetch_.MaybeFetch(Produce, true);
        }
        ImGui::SameLine();
        if (ImGui::Button(U8(L"关闭…")) && sel != nullptr) RequestCloseConfirm(*sel);
    }

    bool MatchesFilter(const WindowEntry& w) const {
        if (appliedFilter_.empty()) return true;
        return LowerCopy(w.title).find(appliedFilter_) != std::wstring::npos ||
               LowerCopy(w.className).find(appliedFilter_) != std::wstring::npos ||
               LowerCopy(w.procName).find(appliedFilter_) != std::wstring::npos;
    }

    void UpdateRows(const Result& res) {
        appliedFilter_ = LowerCopy(Utf8ToWide(filterUtf8_));
        rows_.clear();
        rows_.reserve(res.data.size());
        for (size_t i = 0; i < res.data.size(); ++i) {
            if (MatchesFilter(res.data[i])) rows_.push_back(static_cast<int>(i));
        }
    }

    void DrawTable(AppContext& ctx, const Result& res) {
        (void)ctx;
        if (rows_.empty()) {
            ImGui::TextColored(ColMuted(), "%s",
                               U8(res.data.empty() ? L"未枚举到可见窗口"
                                                   : L"没有匹配过滤条件的窗口"));
            return;
        }
        const int flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_SizingFixedFit;
        if (!ImGui::BeginTable("windows", 5, flags)) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(U8(L"标题"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn(U8(L"类名"), ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn(U8(L"进程"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn(U8(L"尺寸"), ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn(U8(L"状态"), ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows_.size()));
        while (clipper.Step()) {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                const WindowEntry& w = res.data[static_cast<size_t>(rows_[static_cast<size_t>(r)])];
                ImGui::PushID(static_cast<int>(reinterpret_cast<intptr_t>(w.hwnd)));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool selected = selectedHwnd_ == w.hwnd;
                if (ImGui::Selectable(U8(w.title.empty() ? std::wstring(L"—")
                                                         : Truncate(w.title, 64)),
                                      selected, ImGuiSelectableFlags_SpanAllColumns)) {
                    selectedHwnd_ = w.hwnd;
                }
                if (ImGui::BeginPopupContextItem("##rowctx")) {
                    DrawRowMenu(w);
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    U8(w.className.empty() ? std::wstring(L"—") : Truncate(w.className, 40)));
                ImGui::TableNextColumn();
                std::wstring proc = w.procName.empty() ? std::wstring(L"—") : w.procName;
                proc += Fmt(L" ({})", w.pid);
                ImGui::TextUnformatted(U8(Truncate(proc, 44)));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(WindowSizeLabel(w)));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(WindowStatusLabel(w)));
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    void DrawRowMenu(const WindowEntry& w) {
        if (ImGui::MenuItem(U8(L"前置"))) {
            std::wstring e;
            if (ForegroundWindowSafe(w.hwnd, &e)) {
                PushLiveNote(Notification::Kind::JobDone, L"已将窗口切到前台");
            } else {
                PushLiveNote(Notification::Kind::JobFailed, e);
            }
        }
        if (ImGui::MenuItem(w.topmost ? U8(L"取消置顶") : U8(L"置顶"))) {
            std::wstring e;
            if (SetWindowTopmost(w.hwnd, !w.topmost, &e)) {
                PushLiveNote(Notification::Kind::JobDone,
                         w.topmost ? L"已取消置顶" : L"已置顶窗口");
                fetch_.MaybeFetch(Produce, true);
            } else {
                PushLiveNote(Notification::Kind::JobFailed, e);
            }
        }
        if (ImGui::MenuItem(U8(L"最小化"))) {
            std::wstring e;
            MinimizeWindow(w.hwnd, &e);
            fetch_.MaybeFetch(Produce, true);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(U8(L"温和关闭…"))) RequestCloseConfirm(w);
        if (ImGui::MenuItem(U8(L"复制标题"))) ImGui::SetClipboardText(U8(w.title));
    }

    // ---- 温和关闭确认（两段式：取消首位 + 焦点在取消） -----------------------
    struct PendClose {
        bool active = false;
        bool openRequested = false;
        HWND hwnd = nullptr;
        std::wstring title;
        uint32_t pid = 0;  // V15-P2-4: 确认时锁定的 pid，执行前复核句柄归属
    };

    void RequestCloseConfirm(const WindowEntry& w) {
        pend_ = PendClose{};
        pend_.active = true;
        pend_.openRequested = true;
        pend_.hwnd = w.hwnd;
        pend_.title = w.title;
        pend_.pid = w.pid;
    }

    void DrawCloseConfirm(AppContext& ctx) {
        if (!pend_.active) return;
        constexpr char kPopup[] = "##confirm_close_window";
        if (pend_.openRequested) {
            ImGui::OpenPopup(kPopup);
            pend_.openRequested = false;
        }
        if (!ImGui::IsPopupOpen(kPopup)) {
            pend_ = PendClose{};
            return;
        }
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(460.0f, 0.0f), ImVec2(460.0f, FLT_MAX));
        if (!ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

        ImGui::PushStyleColor(ImGuiCol_Text, ColWarn());
        ImGui::TextUnformatted(U8(L"确认关闭窗口"));
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::TextUnformatted(U8(Fmt(L"目标：{}", pend_.title.empty() ? std::wstring(L"—")
                                                                          : pend_.title)));
        ImGui::TextWrapped("%s",
                           U8(L"将发送温和关闭消息（WM_CLOSE），进程可能弹出保存提示；"
                              L"不会强制结束进程。控制台窗口的关闭通常等同于终止其进程。"));
        ImGui::Separator();
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(0);
        if (ImGui::Button(U8(L"取消"), ImVec2(120.0f, 0.0f))) {
            pend_ = PendClose{};
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        const bool go = ImGui::Button(U8(L"关闭窗口"), ImVec2(120.0f, 0.0f));
        ImGui::PopItemFlag();
        if (go) {
            // V15-P2-4: 确认框打开期间句柄可能被复用 —— 执行前复核 IsWindow +
            // pid 与确认时一致，不匹配则如实提示且不发送关闭消息。
            if (!WindowMatchesProcess(pend_.hwnd, pend_.pid)) {
                PushNote(ctx, Notification::Kind::Warn,
                         L"窗口已关闭或句柄已变化，未发送关闭消息");
            } else {
                std::wstring e;
                if (GracefulCloseWindow(pend_.hwnd, &e)) {
                    PushNote(ctx, Notification::Kind::JobDone,
                             Fmt(L"已向「{}」发送关闭消息", pend_.title.empty()
                                                                ? std::wstring(L"无标题窗口")
                                                                : pend_.title));
                } else {
                    PushNote(ctx, Notification::Kind::JobFailed, e);
                }
            }
            pend_ = PendClose{};
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    AsyncFetch<std::vector<WindowEntry>> fetch_{2.0};
    std::string filterUtf8_;
    std::wstring appliedFilter_;
    std::vector<int> rows_;
    HWND selectedHwnd_ = nullptr;
    uint64_t lastFrame_ = kNeverDrawn;
    PendClose pend_;
};

// ===========================================================================
// 宿主服务共享缓存 + 模态（F4#3）：网络页/进程页发起，jobs 枚举后按 PID 过滤。
// ===========================================================================

struct HostSvcSlot {
    std::mutex mu;
    bool ready = false;
    std::vector<ops::ServiceInfo> items;
    std::wstring err;
};

std::map<uint32_t, std::shared_ptr<HostSvcSlot>>& HostSvcCache() {
    static std::map<uint32_t, std::shared_ptr<HostSvcSlot>> m;
    return m;
}

struct HostSvcModalState {
    bool openRequested = false;
    uint32_t pid = 0;
    std::wstring procName;
};
HostSvcModalState& HostSvcModal() {
    static HostSvcModalState s;
    return s;
}

}  // namespace

void EnsureHostServices(uint32_t pid) {
    if (pid == 0) return;
    auto& cache = HostSvcCache();
    const auto it = cache.find(pid);
    if (it != cache.end()) return;
    std::shared_ptr<HostSvcSlot> slot = std::make_shared<HostSvcSlot>();
    cache.emplace(pid, slot);
    std::shared_ptr<AppContext> app = LiveP3Ctx();
    if (!app) return;
    if (app->jobs.Submit([app, slot, pid] {
            std::wstring err;
            std::vector<ops::ServiceInfo> all = ops::EnumServices(&err);
            std::vector<ops::ServiceInfo> mine = FilterServicesByPid(all, pid);
            std::lock_guard<std::mutex> lock(slot->mu);
            slot->items = std::move(mine);
            slot->err = std::move(err);
            slot->ready = true;
        }) == 0) {
        std::lock_guard<std::mutex> lock(slot->mu);
        slot->ready = true;
        slot->err = L"操作队列未运行（应用可能正在退出）";
    }
}

std::wstring HostServiceNamesText(uint32_t pid) {
    EnsureHostServices(pid);
    const auto it = HostSvcCache().find(pid);
    if (it == HostSvcCache().end()) return L"查询中…";
    std::lock_guard<std::mutex> lock(it->second->mu);
    if (!it->second->ready) return L"查询中…";
    if (!it->second->err.empty() && it->second->items.empty()) {
        return Fmt(L"宿主服务查询失败：{}", it->second->err);
    }
    if (it->second->items.empty()) return L"该进程当前未承载可枚举的服务";
    std::wstring text = Fmt(L"承载 {} 个服务：", it->second->items.size());
    for (size_t i = 0; i < it->second->items.size(); ++i) {
        text += Fmt(L"\n· {}（{}）", it->second->items[i].displayName.empty()
                                         ? it->second->items[i].name
                                         : it->second->items[i].displayName,
                    ServiceStateLabel(it->second->items[i].state));
    }
    return text;
}

void RequestHostServicesModal(uint32_t pid, const std::wstring& procName) {
    if (pid == 0) return;
    HostSvcModalState& m = HostSvcModal();
    m.pid = pid;
    m.procName = procName;
    m.openRequested = true;
    EnsureHostServices(pid);
}

namespace {

void DrawHostServicesModal() {
    HostSvcModalState& m = HostSvcModal();
    if (!m.openRequested && !ImGui::IsPopupOpen("##hostsvc")) return;
    constexpr char kPopup[] = "##hostsvc";
    if (m.openRequested) {
        ImGui::OpenPopup(kPopup);
        m.openRequested = false;
    }
    if (!ImGui::IsPopupOpen(kPopup)) return;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(480.0f, 0.0f), ImVec2(480.0f, 420.0f));
    if (!ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_None)) return;

    ImGui::TextUnformatted(U8(Fmt(L"PID {}（{}）承载的服务", m.pid,
                                  m.procName.empty() ? std::wstring(L"未知进程") : m.procName)));
    ImGui::Separator();

    const auto it = HostSvcCache().find(m.pid);
    if (it == HostSvcCache().end()) {
        ImGui::TextColored(ColMuted(), "%s", U8(L"查询中…"));
    } else {
        std::vector<ops::ServiceInfo> items;
        std::wstring err;
        bool ready = false;
        {
            std::lock_guard<std::mutex> lock(it->second->mu);
            ready = it->second->ready;
            items = it->second->items;
            err = it->second->err;
        }
        if (!ready) {
            ImGui::TextColored(ColMuted(), "%s", U8(L"查询中…"));
        } else if (!err.empty() && items.empty()) {
            ImGui::TextColored(ColFail(), "%s", U8(Fmt(L"查询失败：{}", err)));
        } else if (items.empty()) {
            ImGui::TextColored(ColMuted(), "%s", U8(L"该进程当前未承载可枚举的服务"));
        } else {
            if (!err.empty()) {
                ImGui::TextColored(ColWarn(), "%s", U8(Fmt(L"部分服务读取失败：{}", err)));
            }
            if (ImGui::BeginTable("hostsvc", 3, ImGuiTableFlags_RowBg |
                                                    ImGuiTableFlags_BordersInnerH |
                                                    ImGuiTableFlags_ScrollY,
                                  ImVec2(0.0f, 260.0f))) {
                ImGui::TableSetupColumn(U8(L"服务名"), ImGuiTableColumnFlags_WidthFixed, 170.0f);
                ImGui::TableSetupColumn(U8(L"显示名"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn(U8(L"状态"), ImGuiTableColumnFlags_WidthFixed, 84.0f);
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < items.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(U8(Truncate(items[i].name, 40)))) {
                        ImGui::SetClipboardText(U8(items[i].name));
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", U8(L"点击复制服务名"));
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(
                        U8(items[i].displayName.empty() ? std::wstring(L"—")
                                                        : Truncate(items[i].displayName, 48)));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(U8(ServiceStateLabel(items[i].state)));
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
    }
    ImGui::Separator();
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(0);
    if (ImGui::Button(U8(L"关闭"), ImVec2(120.0f, 0.0f))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

}  // namespace

void DrawGcModals(AppContext& /*ctx*/) {
    DrawHostServicesModal();
}

// ===========================================================================
// 全局热键 Ctrl+Alt+M（F4#10）：注册/反注册在主线程（窗口所属线程）。
// WM_HOTKEY 由 main.cpp 的 onMessage 钩子处理，复用托盘三分支显隐逻辑。
// ===========================================================================

namespace {
HWND& GcHotkeyHwnd() {
    static HWND h = nullptr;
    return h;
}
}  // namespace

void GcHotkeyBindWindow(HWND hwnd) { GcHotkeyHwnd() = hwnd; }

void GcHotkeyUnbindWindow() {
    if (GcHotkeyHwnd() != nullptr) {
        UnregisterHotKey(GcHotkeyHwnd(), kGcHotkeyId);
    }
    GcHotkeyHwnd() = nullptr;
}

bool GcHotkeySetEnabled(bool enabled, std::wstring* err) {
    const HWND hwnd = GcHotkeyHwnd();
    if (hwnd == nullptr) {
        if (err != nullptr) *err = L"主窗口未就绪，无法注册热键";
        return false;
    }
    UnregisterHotKey(hwnd, kGcHotkeyId);  // 幂等：先清旧注册
    if (!enabled) {
        if (err != nullptr) err->clear();
        return true;
    }
    if (!RegisterHotKey(hwnd, kGcHotkeyId, MOD_CONTROL | MOD_ALT, 'M')) {
        if (err != nullptr)
            *err = Fmt(L"注册热键 Ctrl+Alt+M 失败（错误码 {}）：可能被其他程序占用",
                       GetLastError());
        return false;
    }
    if (err != nullptr) err->clear();
    return true;
}

// ===========================================================================
// 页面注册
// ===========================================================================

}  // namespace ui3
}  // namespace stm

namespace stm {
namespace ui3 {
void RegisterGcPages(AppContext& ctx) {
    // V15-P2-1: perfCsvWanted 只是会话内状态镜像，从不存在"自动恢复"——记录是
    // 显式手动行为，启动时清除上一次会话可能残留的 true，避免误导。
    ctx.cfg.SetBool(L"perfCsvWanted", false);
    ctx.pages.push_back(std::make_unique<CrashPage>());
    ctx.pages.push_back(std::make_unique<WindowPage>());
}
}  // namespace ui3
}  // namespace stm
