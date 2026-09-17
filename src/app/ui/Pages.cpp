// UI SHELL + PLACEHOLDER PAGES.
// Phase-2 UI agent: REWRITE THIS FILE with the full process table, performance charts,
// detail panel and confirm dialogs per docs/phase/01_架构设计文档.md section 8/11.
// Keep RegisterPages()/DrawShell() signatures stable.
#include "app/ui/Pages.h"
#include "app/AppContext.h"
#include "app/Theme.h"
#include "core/ProcData.h"
#include "core/Str.h"
#include "ops/Elevate.h"
#include "ops/SessionState.h"
#include "imgui.h"

// clang-format off: ImGui calls below intentionally keep narrow (UTF-8) strings;
// every Chinese text goes through WideToUtf8() at the call site.

namespace stm {

namespace {

// ---- placeholder processes page (full implementation is the UI agent's task) ----
class ProcessesPage final : public IPage {
public:
    const wchar_t* Id() const override { return L"processes"; }
    const wchar_t* Title() const override { return L"进程"; }
    void Draw(AppContext& ctx) override {
        auto snap = ctx.collect.Store().Get();
        if (!snap->procs.empty() && ImGui::BeginTable("procs", 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthFixed, 180.0f);
            ImGui::TableSetupColumn("PID");
            ImGui::TableSetupColumn("CPU");
            ImGui::TableSetupColumn("内存");
            ImGui::TableSetupColumn("线程");
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(snap->procs.size()));
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const ProcInfo& p = snap->procs[static_cast<size_t>(row)];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(WideToUtf8(p.name).c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%u", p.key.pid);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(WideToUtf8(FormatPercent(p.cpuPercent)).c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(WideToUtf8(FormatBytes(p.workingSet)).c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%u", p.threads);
                }
            }
            ImGui::EndTable();
        }
    }
};

// ---- placeholder performance page ----
class PerfPage final : public IPage {
public:
    const wchar_t* Id() const override { return L"perf"; }
    const wchar_t* Title() const override { return L"性能"; }
    void Draw(AppContext& ctx) override {
        auto snap = ctx.collect.Store().Get();
        const std::wstring line = Fmt(L"CPU {}   内存 {} / {}",
                                      FormatPercent(snap->sys.cpuTotalPercent),
                                      FormatBytes(snap->sys.physTotal - snap->sys.physAvail),
                                      FormatBytes(snap->sys.physTotal));
        ImGui::TextUnformatted(WideToUtf8(line).c_str());
        ImGui::TextUnformatted(WideToUtf8(
            L"性能页完整实现（120s 曲线 / 每核 / 磁盘 / 网络）由阶段 2 UI 任务交付").c_str());
    }
};

}  // namespace

void RegisterPages(AppContext& ctx) {
    ctx.pages.push_back(std::make_unique<ProcessesPage>());
    ctx.pages.push_back(std::make_unique<PerfPage>());
}

void DrawShell(AppContext& ctx) {
    // Toolbar
    if (ImGui::BeginMainMenuBar()) {
        int interval = static_cast<int>(ctx.cfg.GetInt(L"intervalMs", 1000));
        if (ImGui::SliderInt("刷新间隔(ms)", &interval, 500, 5000, "%d ms")) {
            ctx.cfg.SetInt(L"intervalMs", interval);
            ctx.collect.SetInterval(static_cast<uint32_t>(interval));
        }
        if (!ctx.elevated) {
            ImGui::SameLine();
            if (ImGui::Button(WideToUtf8(L"以管理员身份重启").c_str())) {
                ops::SessionState s;
                s.page = ctx.activePage;
                s.intervalMs = static_cast<uint32_t>(interval);
                (void)ops::SaveSession(s);
                if (ops::RelaunchAsAdmin(L"--relaunched")) ctx.wantExit = true;
            }
        } else {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.40f, 0.62f, 0.88f, 1.0f), "%s",
                               WideToUtf8(L"管理员").c_str());
        }
        // Right-aligned stats
        ImGui::SameLine(ImGui::GetWindowWidth() - 260);
        ImGui::Text("帧 %.1f ms | tick p95 %.1f ms", ctx.frameMs, ctx.collect.TickP95Ms());
        ImGui::EndMainMenuBar();
    }

    // Tabs (ImGui is narrow-char: titles go through UTF-8)
    if (ImGui::BeginTabBar("pages")) {
        for (int i = 0; i < static_cast<int>(ctx.pages.size()); ++i) {
            if (ImGui::BeginTabItem(WideToUtf8(ctx.pages[static_cast<size_t>(i)]->Title()).c_str())) {
                ctx.activePage = i;
                ctx.pages[static_cast<size_t>(i)]->Draw(ctx);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
}

}  // namespace stm
