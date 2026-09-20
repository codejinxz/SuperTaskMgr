// UI 外壳 + 页面：进程表、性能图表、详情面板、确认对话框。
// toasts and status bar (docs/phase/01_架构设计文档.md sections 8/11).
// 所有面向用户的文本都是中文；经 ui::U8() 处理，因为 ImGui 是
// 窄字符（UTF-8）API。UI 线程每帧恰好读取一次快照。
#include "app/ui/Pages.h"
#include "app/AboutInfo.h"        // 维护轮 10: 诊断报告带应用版本（kAppVersion 单一来源）
#include "app/AppContext.h"
#include "app/Theme.h"            // H-A: 主题三态 + 模式感知强调色
#include "app/ui/AboutUi.h"       // H-A/A1: 工具条「关于」按钮（OpenAbout）+ 关于模态
#include "app/ui/ConfirmAction.h"
#include "app/ui/HeaderLayout.h"  // R-Fix Bug1: 状态栏右段实测宽度布局（纯函数）
#include "app/ui/ModulesUi.h"
#include "app/ui/SortKey.h"
#include "app/ui/UiText.h"
#include "app/ui/VersionInfo.h"
#include "app/ui3/CompatDiag.h"   // 维护轮 10: 兼容模式诊断文本（纯函数 + 落盘）
#include "app/ui3/LogViewer.h"    // P-C: 日志查看器纯逻辑（过滤/行文本/统计/报告拼接）
#include "app/ui3/GcPages.h"      // F4: 崩溃记录/窗口页注册 + 宿主服务模态 + 热键
#include "app/ui3/JumpState.h"    // F4#3: 跨页跳转槽
#include "app/ui3/MemCleanup.h"   // P3 任务一: 一键内存优化候选/聚合（纯逻辑）
#include "app/ui3/PageLayout.h"    // P1④: 运行时布局缩放 / P1⑤: 定高区收缩（纯函数）
#include "app/ui3/Pages3.h"       // 第 3 阶段扩展标签 + 外壳钩子（增量式）
#include "app/ui3/PerfChart.h"    // Phase A: 性能历史 Ring/时间窗/RingView 纯函数
#include "app/ui3/PerfCsv.h"      // F4#7: 性能 CSV 记录
#include "app/ui3/ProcControlUi.h"  // F4#2: 优先级/亲和性文案与掩码换算
#include "app/ui3/ProcKind.h"     // F4#1: 系统进程分类
#include "app/ui3/ProcTree.h"     // F4#4: 进程树行序
#include "app/ui3/StatusLayout.h"  // L1 防抖: 状态栏定宽槽位（纯函数 + selftest 共用）
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
#include "imgui_internal.h"  // TableSetColumnSortDirection + 每列宽度回读
#include "implot.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <commdlg.h>  // H-A: 外观菜单「选择图片…」GetOpenFileNameW（comdlg32 已链接）
#include <cstdint>
#include <cwctype>
#include <deque>
#include <limits>
#include <map>      // P1③: GPU 明细表列宽缓存（按表名）
#include <memory>
#include <mutex>
#include <shellapi.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace stm {

namespace {

using ui::U8;  // UTF-8 文本缓存（app/ui/UiText.h）；每个 ImGui 调用点都在用

// ===========================================================================
// 共享的每帧 UI 状态（单主窗口、单 UI 线程）。
// ===========================================================================

struct Toast {
    Notification::Kind kind = Notification::Kind::Info;
    std::wstring text;
    double expireTime = 0.0;  // 基于 ImGui::GetTime()
};

// P-C：日志查看器模态的持久状态（模态关闭后保留过滤/勾选与上次读取结果）。
struct LogViewerUi {
    stm::LogTail tail;                     // 最近一次 ReadLogTail 结果（打开/刷新时更新）
    bool loaded = false;                   // 本次打开是否已读取（打开请求消费时清零）
    int filterMode = ui3::kLogFilterAll;   // 级别过滤（ui3::LogFilterModeLabel）
    bool autoRefresh = false;              // 自动刷新（默认关；勾选后 1s 周期重读）
    bool followNewest = false;             // 跟随最新：刷新后自动滚到表尾
    double lastReadTime = 0.0;             // 上次读取时刻（ImGui::GetTime 基准）
    std::vector<ui3::LogDisplayRow> rows;  // 过滤后的显示行（改过滤/重读时重建）
    bool reportOpen = false;               // 「生成诊断报告」二级展示
    std::wstring reportText;               // 报告全文（复制/保存共用同一文本）
    bool scrollBottomPending = false;      // 本帧滚底请求（消费后清零）
};

// ConfirmKind/ConfirmRequest 已移至 app/ui/ConfirmAction.h（bug F1 修复）：
// 确认动作执行与第 3 阶段启动对话框共享，
// 并由 stm_selftest 在无 GUI 下单元测试。

struct UiState {
    std::shared_ptr<const Snapshot> snap;  // 每帧一次 Store().Get()（外壳持有）
    // main 注册的持有句柄（BindAppContext）。任务 lambda 按值捕获它，
    // 使在途任务活得比 main 的拆除更久：JobQueue::Shutdown 会把超时的
    // 工作线程脱离，而该捕获让 notes/jobs/details 保持存活直到
    // 任务完成，无论哪个线程释放最后引用。
    std::shared_ptr<AppContext> liveCtx;
    std::deque<Toast> toasts;
    std::wstring lastNote;                  // 在 toast 过期后仍保留给状态栏
    ui::ConfirmRequest confirm;
    bool confirmOpenRequested = false;      // 由菜单处理器一次性置位：打开模态框
    bool confirmOpened = false;             // 模态框确实达到打开状态
    bool tabSelectArmed = true;             // P1-1：会话恢复时一次性 SetSelected
    bool paused = false;
    // 会话交接读取的选择簿记（由 ProcessesPage 更新）。
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
    // H-A: 壁纸同步加载进行中（外观状态行提示；加载固定 UI 线程同步，见
    // PickAndLoadWallpaper 注释，几十 ms 阻塞期间模态保持打开）。
    bool wallpaperBusy = false;
    // A1 统一风格改造：工具条「主题…」按钮 -> 外观设置模态。与确认框相同的
    // 「请求长期有效 + 模态每帧渲染」模式（见 DrawAppearanceModal 注释）。
    bool appearanceOpenRequested = false;
    bool appearanceOpened = false;
    // 维护轮 10：状态栏「兼容模式：<原因>」可点击 -> 说明模态（同一渲染模式）。
    bool compatDiagOpenRequested = false;
    bool compatDiagOpened = false;
    // 「重新自检」进行中：请求已提交，等门真正跑完（V24 P1-1：以代际变化
    // 判定，tick 计数会提前于重跑完成，不可作完成信号）。期间按钮禁用。
    bool compatRetryPending = false;
    uint64_t compatRetryGenBase = 0;  // 请求时刻的自检门代际（LastSelfCheckGeneration）
    // P-C：日志查看器模态（工具条「日志」按钮）。读取策略：打开时与手动刷新时
    // 在 UI 线程同步读取——ReadLogTail 只读尾部 512KB 窗口（kLogTailWindowBytes）、
    // 至多 500 行，毫秒级、远低于一帧预算，与壁纸同步加载先例一致；走 ops job
    // 反而引入 LogTail 跨线程拷贝与完成通知的额外复杂度。打开期间不自动刷新；
    // 自动刷新复选框默认关，勾选后按 1s 周期重读。
    bool logViewerOpenRequested = false;
    bool logViewerOpened = false;
    LogViewerUi logViewer;
};

UiState& Ui() {
    static UiState s;
    return s;
}

void PushToast(Notification::Kind kind, const std::wstring& text) {
    auto& toasts = Ui().toasts;
    toasts.push_back({kind, text, ImGui::GetTime() + 5.0});  // 5 秒后自动消失
    while (toasts.size() > 6) toasts.pop_front();
    Ui().lastNote = text;
}

// ===========================================================================
// 确认动作提交：所有破坏性操作都经 ops 工作队列上的 ui::ExecuteConfirmed-
// Action（app/ui/ConfirmAction.h）执行；结果以通知 -> toast 返回。
// 生命周期/反馈契约见 ConfirmAction.h。
// ===========================================================================

void PostNote(NotificationQueue& notes, Notification::Kind kind, const std::wstring& text) {
    Notification n;
    n.kind = kind;
    n.text = text;
    notes.Push(n);
}

// 清除任何待决确认请求（每个对话框退出路径都用）。
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
        // 无存活上下文（拆除中）：没有 notes 队列可通知——仅重置。
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
                // P2（V14）：与其他失败通知相同的管理员提示后缀。
                PostNote(app->notes, Notification::Kind::JobFailed,
                         Fmt(L"无法规划进程树：{}{}", err.empty() ? std::wstring(L"未知错误") : err,
                             ui::AdminHintSuffix(elevated)));
            }
        }) == 0) {
        // P2（V14）：被拒的提交绝不能静默。
        PostNote(app->notes, Notification::Kind::JobFailed,
                 L"操作队列未运行，进程树规划未提交（应用可能正在退出）");
        CloseConfirm();
    }
}

// 确认请求构造器（ProcKey/name/path 在此锁定）。
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
// 小型格式化 / 颜色辅助。
// ===========================================================================

// 模式感知的界面强调色（H-A）：经 Theme 转换后深/浅两套主题都可读。
ImVec4 AccentCol(ThemeAccent a) {
    return ImGui::ColorConvertU32ToFloat4(ThemeAccentColor(a));
}
ImVec4 ColDone() { return AccentCol(ThemeAccent::Done); }
ImVec4 ColFail() { return AccentCol(ThemeAccent::Fail); }
ImVec4 ColWarn() { return AccentCol(ThemeAccent::Warn); }
ImVec4 ColInfo() { return AccentCol(ThemeAccent::Info); }

// 模块路径从尾部读最佳（文件名在末尾）；前部省略号化。
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
// Toast（右上角，2-5 秒自动消失；JobFailed 显示为红色）。
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
// 确认对话框（模态、居中；破坏性操作两段式闸门）。
// ===========================================================================

// --autotest dialogclick 观察点（V14）：驱动据此断言，以回归测试
// 真实渲染的对话框（单帧回归哨兵 +
// 供合成鼠标注入用的动作按钮几何）。
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

    // 树规划失败：规划任务已投递过失败 toast。
    if (req.kind == ui::ConfirmKind::KillTree && req.planCount && req.planCount->load() == -2) {
        CloseConfirm();
        return;
    }

    // Bug F1 修复（2026-09）：旧代码在 confirmOpenRequested 为 false 时提前
    // 返回，BeginPopupModal() 只在请求创建的那一帧运行。
    // 模态框恰好只渲染一帧，永远收不到点击，
    // a click ("终止进程/终止进程树 没有任何效果"), and the abandoned ImGui popup
    // 并以隐形阻塞模态留在 popup 栈中。现在只要请求活动，
    // 模态框每帧都 Begin；OpenPopup 只发一次。
    if (Ui().confirmOpenRequested) {
        if (!ImGui::IsPopupOpen("##confirm")) {
            ImGui::OpenPopup("##confirm");
            Ui().confirmOpened = true;
        }
        Ui().confirmOpenRequested = false;
    }
    if (!Ui().confirmOpened) return;  // 树规划仍在途

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(460.0f, 0.0f), ImVec2(460.0f, FLT_MAX));
    if (!ImGui::BeginPopupModal("##confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        // 模态框之前开着但现在没了：被 Esc 关闭（按钮之外的
        // 唯一关闭路径）。清除请求——绝不留下过期请求
        // 或僵尸隐形模态。
        if (!ImGui::IsPopupOpen("##confirm") && !Ui().confirmOpenRequested) CloseConfirm();
        g_dialogAutotest.modalOpen = false;
        g_dialogAutotest.framesOpen = 0;
        return;
    }
    g_dialogAutotest.modalOpen = true;
    ++g_dialogAutotest.framesOpen;
    // P2（V14）：树规划异步到达——显示诚实的进度行而不是
    // 空对话框，并在其到达前保持动作禁用。
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
        case ui::ConfirmKind::MemCleanup: title = L"内存加速"; action = L"开始优化"; break;
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
            // P2（V14）：过长映像路径会溢出 460px 模态框——换行显示。
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
                // PlanTerminateTree 只返回后代；TreeResult.planned
                // 含根，因此显示 planned + 1（含目标）以
                // 与该数目一致。
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
            // V18-P2-1：本对话框打开期间再次点击 X 会重设待决标志；
            // 最小化后隐藏窗口否则会隐形地重新打开
            // 该对话框。
            if (std::shared_ptr<AppContext> app = Ui().liveCtx) {
                app->closeAskPending.store(false);
            }
            CloseConfirm();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(U8(L"取消"), ImVec2(120.0f, 0.0f))) {
            if (std::shared_ptr<AppContext> app = Ui().liveCtx) {
                app->closeAskPending.store(false);  // V18-P2-1：同样的残留待决防护
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

    // V8-P1-2：键盘焦点落在取消（而非破坏性动作）上，且动作按钮
    // 完全移出键盘导航，因此新开模态框里的 Space/Enter
    // 绝不可能触发终止。
    // Bug F1 修复（2026-09）：SetKeyboardFocusHere 必须在模态框出现时
    // 只运行一次。每帧都发会重复提交指向取消按钮的导航移动，
    // 而 NavMoveRequestApplyResult()（imgui.cpp）在鼠标按住的按钮
    // 不是导航目标时调用 ClearActiveID()——动作按钮的鼠标捕获
    // 在按下与抬起之间被抢走，静默吞掉
    // 每一次确认点击（与第 3 阶段启动对话框同根因）。
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
        // 队列拒绝任务时 ExecuteConfirmedAction 会自行投递 JobFailed
        // 通知——无论哪种情况 toast 管线都会上报。
        ui::ExecuteConfirmedAction(Ui().liveCtx, req);
        CloseConfirm();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ===========================================================================
// 性能历史：最近 kHistCap（600，Phase A 自 120 扩容以支撑 60/120/300/600s
// 时间窗）个采集 tick 的环形缓冲，在 tick id 变化时于外壳中追加（单一
// 摄取点，仍每 tick 一次 O(procs)）。Ring/PerfHistory/kHistCap 已迁入
// app/ui3/PerfChart.h（header-only 纯函数，selftest 共用同一份定义）；
// 渲染侧经 RingView 取尾窗零拷贝视图，切换时间窗只改显示、不清历史。
// ===========================================================================

using ui3::kHistCap;
using ui3::PerfHistory;
using ui3::Ring;

PerfHistory& Hist() {
    static PerfHistory h;
    return h;
}

void AppendHistory(const Snapshot& s) {
    PerfHistory& h = Hist();
    if (s.tickId == 0 || s.tickId == h.lastTick) return;  // 初始为空 / 同一 tick
    h.lastTick = s.tickId;
    // F4#7: 性能 CSV 记录与环形历史共用摄取点（每 tick 一行，1 Hz 小写入）。
    // V15-P1: 写入失败（磁盘满等）→ 记录器已自动停止，这里如实 toast（含路径），
    // 后续 tick 因未激活成为空操作 —— 绝不静默丢行而 UI 仍显示"记录中"。
    if (!ui3::SharedPerfCsv().Append(s)) {
        const std::wstring failedPath = ui3::SharedPerfCsv().Path();
        PushToast(Notification::Kind::JobFailed,
                  Fmt(L"性能 CSV 记录已自动停止——磁盘写入失败：{}", failedPath));
    }
    h.cpuTotal.Push(static_cast<float>(s.sys.cpuTotalPercent));      // NaN 可：渲染时跳过
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

    // Phase C：磁盘队列深度（本机无计数器 -> NaN 照推 -> hasData 诚实空态，
    // 与硬故障/s 同模式）与提交占比%（内存块 Y2 副轴；commitLimit<=0 -> NaN）。
    h.diskQueue.Push(static_cast<float>(s.sys.diskQueueDepth));
    h.commitPct.Push(static_cast<float>(
        ui3::CommitPercent(s.sys.commitTotal, s.sys.commitLimit)));
    // GPU 利用率合计%（2s 节拍；缺席 tick 由采集端复用上次值 -> 阶梯无锯齿）。
    // sys.gpus 为空（GPU 未启用/尚无数据）-> 不推，环保持空 -> hasData 空态。
    if (!s.sys.gpus.empty()) {
        std::vector<double> utils;
        utils.reserve(s.sys.gpus.size());
        for (const GpuAdapterInfo& g : s.sys.gpus) utils.push_back(g.utilPercent);
        h.gpuUtil.Push(static_cast<float>(ui3::GpuUtilClampSum(utils.data(), utils.size())));
    }

    // Phase D：每适配器吞吐序列。契约：sys.netAdapters 仅含 Up 非回环。
    // 空表（GetIfTable2 失败/无接口）-> 本 tick 不推不重建（序列暂停，诚实）。
    // 接口集变化 -> BuildAdapterSeries 重建（流量 Top8 截断 + 名称表），
    // 历史重启 —— 与 cores 核心数变化同口径。
    if (!s.sys.netAdapters.empty()) {
        std::vector<uint64_t> idsCur;
        idsCur.reserve(s.sys.netAdapters.size());
        std::vector<ui3::AdapterSeriesIn> seriesIn;
        seriesIn.reserve(s.sys.netAdapters.size());
        for (const SystemInfo::AdapterThroughput& a : s.sys.netAdapters) {
            idsCur.push_back(a.ifIndex);
            seriesIn.push_back(ui3::AdapterSeriesIn{a.ifIndex, a.name, a.recvBps,
                                                    a.sendBps, true, false});
        }
        std::vector<uint64_t> idsPrev = h.netAdapterIds;
        std::sort(idsCur.begin(), idsCur.end());
        std::sort(idsPrev.begin(), idsPrev.end());
        if (idsCur != idsPrev) {
            const std::vector<ui3::AdapterSeriesIn> picked = ui3::BuildAdapterSeries(seriesIn);
            h.netAdapters.assign(picked.size(), Ring{});
            h.netAdapterIds.clear();
            h.netAdapterNames.clear();
            for (const ui3::AdapterSeriesIn& p : picked) {
                h.netAdapterIds.push_back(p.ifIndex);
                h.netAdapterNames.push_back(p.name);
            }
        }
        h.netAdapterTotal = static_cast<int>(s.sys.netAdapters.size());
        for (size_t i = 0; i < h.netAdapterIds.size(); ++i) {
            double total = std::numeric_limits<double>::quiet_NaN();
            for (const ui3::AdapterSeriesIn& a : seriesIn) {
                if (a.ifIndex == h.netAdapterIds[i]) {
                    total = a.TotalBps();
                    break;
                }
            }
            h.netAdapters[i].Push(static_cast<float>(total));
        }
    }

    const size_t n = s.sys.perCorePercent.size();
    if (n > 0 && h.cores.size() != n) {
        // 核心数发现（或罕见热变更）：（重新）分配，历史重启。
        h.cores.assign(n, Ring{});
    }
    for (size_t i = 0; i < h.cores.size() && i < n; ++i) {
        h.cores[i].Push(static_cast<float>(s.sys.perCorePercent[i]));
    }
}

// ===========================================================================
// ProcessesPage：按架构第 8 节的完整表格 + 固定宽度详情面板。
// ===========================================================================

const ProcInfo* FindByPid(const Snapshot& snap, uint32_t pid) {
    // 快照进程按 pid 升序（契约）；二分查找 + createTime 校验。
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

// 选择变化时抓取详情类别位集（F4：含模块列表——DetailsProvider
// 会采集但此前从未请求/渲染）。
constexpr uint32_t kDetailKinds =
    static_cast<uint32_t>(ops::DetailKind::Signature) | static_cast<uint32_t>(ops::DetailKind::CmdLine) |
    static_cast<uint32_t>(ops::DetailKind::UserInfo) | static_cast<uint32_t>(ops::DetailKind::GuiObjects) |
    static_cast<uint32_t>(ops::DetailKind::Modules);

class ProcessesPage final : public IPage {
public:
    const wchar_t* Id() const override { return L"processes"; }
    const wchar_t* Title() const override { return L"进程"; }

    void Draw(AppContext& ctx) override {
        const std::shared_ptr<const Snapshot>& snap = Ui().snap;  // 外壳已读，绝不为 null
        metaBudget_ = 3;  // 每帧新 GetFileVersionInfoW 查询的上限
        LoadPersistedOnce(ctx);
        // H-A: 外壳「外观→恢复默认列宽」置位世代号后，这里把 widths_ 拉回默认；
        // 表格 id 同步换代（DrawTable），ImGui 内部按 id 记忆的旧列宽随之丢弃。
        // P1②：同一世代号也重置列显示顺序（默认恒等排列，并清待应用队列）。
        if (appliedResetGen_ != Ui().colWidthResetGen) {
            appliedResetGen_ = Ui().colWidthResetGen;
            for (int i = 0; i < kColCount; ++i) widths_[i] = kDefaultWidths[i];
            ui::ProcColumnDefaultOrder(colOrderPending_, ui::kProcColSlots);
            colOrderPendingValid_ = true;  // 换代表格上应用一次（回到默认序）
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
    // ---- 持久化 -----------------------------------------------------------
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
        ctx.cfg.SetInt(L"selPid", 0);  // 一次性：绝不持久化过期选择
        ctx.cfg.SetInt(L"selCreateTime", 0);

        // 名称/描述是拉伸列（值=权重）；其余为固定像素宽。
        // 两种形态存在同一槽位，按列区分。
        for (int i = 0; i <= static_cast<int>(ui::SortColumn::CtxSwitches); ++i) {
            widths_[i] = static_cast<float>(ctx.cfg.GetDouble(
                std::wstring(L"colW_") + ui::SortColumnId(static_cast<ui::SortColumn>(i)),
                kDefaultWidths[i]));
        }
        widths_[11] = static_cast<float>(ctx.cfg.GetDouble(L"colW_badges", kDefaultWidths[11]));
        widths_[12] = static_cast<float>(ctx.cfg.GetDouble(L"colW_desc", kDefaultWidths[12]));

        // P1②：恢复上次会话的列显示顺序（UserID 逗号串）。非法/缺失保持
        // 默认（恒等）顺序 —— 持久化值只在启动时消费一次。
        {
            const std::wstring saved = ctx.cfg.GetString(L"colOrder", L"");
            int parsed[ui::kProcColSlots] = {};
            if (!saved.empty() &&
                ui::ProcColumnOrderFromCfg(saved.c_str(), parsed, ui::kProcColSlots) ==
                    ui::kProcColSlots) {
                for (int i = 0; i < ui::kProcColSlots; ++i) colOrderPending_[i] = parsed[i];
                colOrderPendingValid_ = true;
                colOrderLastSaved_ = saved;
            }
        }
        // P0-2（F2 评审）：描述（与名称）槽位存的是拉伸权重。
        // 旧构建曾把像素（数百）存进这些槽位，导致名称列每次启动
        // 都被压扁；任何 > 10 的旧值都不可能是权重。
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
        // 表格内至多约 1Hz 调用；只做廉价键值写
        //（文件本身由 main 在退出时保存）。
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
        // P0-2（F2 评审）：描述列是 WidthStretch——持久化其拉伸
        // 权重（旧代码把 WidthGiven 的像素写进权重槽位，
        // 导致之后每次启动名称列都被压扁）。
        save(12, L"colW_desc", true);
        save(0, L"colW_name", true);  // 名称列的拉伸权重
        if (changed) lastWidthSave_ = ImGui::GetTime();
    }

    // ---- 选择 -------------------------------------------------------------
    void Select(const ProcInfo& p) {
        if (selectedValid_ && p.key == selected_ && !lost_) return;
        selected_ = p.key;
        selectedValid_ = true;
        lost_ = false;
        lastKnown_ = p;
        detailKey_ = ProcKey{};  // 强制重新请求详情
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

    // ---- 过滤 / 排序 ------------------------------------------------------
    void UpdateFilter() {
        // 在此把搜索词小写一次；ContainsLower 按行小写化干草堆。
        // ASCII 匹配大小写不敏感（"Chrome" 匹配 chrome.exe）。
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

    // ---- 工具栏 ------------------------------------------------------------
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

    // ---- 表格 ---------------------------------------------------------------
    void DrawTable(AppContext& ctx, const Snapshot& snap) {
        if (snap.procs.empty()) {
            ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.65f, 1.0f), "%s", U8(L"等待采集数据…"));
            return;
        }

        // F4#4: 树形模式下禁用表头点击排序（全局排序降级为同级排序，UI 明示）。
        // H-A: 表格 id 带列宽重置世代号 —— 恢复默认后换代，ImGui 丢弃按 id
        // 记忆的旧列宽，SetupColumns 的默认值立即生效。
        // P1②：ImGuiTableFlags_Reorderable 允许拖动表头重排列顺序。
        char tableId[32];
        snprintf(tableId, sizeof(tableId), "procs#%llu",
                 static_cast<unsigned long long>(Ui().colWidthResetGen));
        const int tableFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
                               ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                               ImGuiTableFlags_SizingFixedFit |
                               (treeMode_ ? 0 : ImGuiTableFlags_Sortable);
        if (!ImGui::BeginTable(tableId, static_cast<int>(ui::SortColumn::Count) + 2,
                               tableFlags)) {
            return;
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        SetupColumns();
        ApplyPersistedColumnOrderOnce();
        if (!treeMode_) ReflectPersistedSortOnce();

        if (!treeMode_) {
            if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
                if (specs->SpecsDirty) {
                    // P1②：按 UserID（列槽位）映射排序键，绝不按显示序 ——
                    // 拖动重排后 Specs[0].ColumnIndex 会随显示位置变化，
                    // UserID 与列的逻辑身份的绑定不变。
                    if (specs->SpecsCount > 0) {
                        ui::SortColumn c;
                        if (ui::ProcColumnFromUserId(
                                static_cast<int>(specs->Specs[0].ColumnUserID), &c)) {
                            sortColumn_ = c;
                            sortDesc_ =
                                specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
                            Ui().sortColumn = sortColumn_;
                            Ui().sortDesc = sortDesc_;
                            PersistSort(ctx);
                        }
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
        PersistColumnOrder(ctx);  // P1②：拖动重排后 ~1Hz 写回列顺序
        ImGui::EndTable();
    }

    // P1②：列按槽位序提交，UserID = 槽位值（ui::ProcColumnUserId）。显示
    // 顺序（拖动重排）由 ImGui 管理，与这里的提交顺序解耦；排序回调、
    // widths_[] 与 colW_* 键全部按 UserID/槽位映射（见 SortKey.h 契约）。
    void SetupColumns() {
        ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch |
                                              ImGuiTableColumnFlags_NoHide |
                                              ImGuiTableColumnFlags_NoClip,
                                widths_[0], static_cast<ImGuiID>(ui::ProcColumnUserId(0)));
        ImGui::TableSetupColumn(U8(L"PID"), ImGuiTableColumnFlags_WidthFixed, widths_[1],
                                static_cast<ImGuiID>(ui::ProcColumnUserId(1)));
        ImGui::TableSetupColumn(U8(L"CPU%"), ImGuiTableColumnFlags_WidthFixed, widths_[2],
                                static_cast<ImGuiID>(ui::ProcColumnUserId(2)));
        ImGui::TableSetupColumn(U8(L"内存"), ImGuiTableColumnFlags_WidthFixed, widths_[3],
                                static_cast<ImGuiID>(ui::ProcColumnUserId(3)));
        ImGui::TableSetupColumn(U8(L"提交"), ImGuiTableColumnFlags_WidthFixed, widths_[4],
                                static_cast<ImGuiID>(ui::ProcColumnUserId(4)));
        ImGui::TableSetupColumn(U8(L"磁盘"), ImGuiTableColumnFlags_WidthFixed, widths_[5],
                                static_cast<ImGuiID>(ui::ProcColumnUserId(5)));
        ImGui::TableSetupColumn(U8(L"网络"), ImGuiTableColumnFlags_WidthFixed, widths_[6],
                                static_cast<ImGuiID>(ui::ProcColumnUserId(6)));
        ImGui::TableSetupColumn(U8(L"硬故障/s"), ImGuiTableColumnFlags_WidthFixed, widths_[7],
                                static_cast<ImGuiID>(ui::ProcColumnUserId(7)));
        ImGui::TableSetupColumn(U8(L"句柄"), ImGuiTableColumnFlags_WidthFixed, widths_[8],
                                static_cast<ImGuiID>(ui::ProcColumnUserId(8)));
        ImGui::TableSetupColumn(U8(L"线程"), ImGuiTableColumnFlags_WidthFixed, widths_[9],
                                static_cast<ImGuiID>(ui::ProcColumnUserId(9)));
        ImGui::TableSetupColumn(U8(L"上下文切换/s"), ImGuiTableColumnFlags_WidthFixed,
                                widths_[10], static_cast<ImGuiID>(ui::ProcColumnUserId(10)));
        ImGui::TableSetupColumn(U8(L"徽标"), ImGuiTableColumnFlags_WidthFixed |
                                                 ImGuiTableColumnFlags_NoSort,
                                widths_[11], static_cast<ImGuiID>(ui::kProcColUserIdBadges));
        ImGui::TableSetupColumn(U8(L"描述"), ImGuiTableColumnFlags_WidthStretch |
                                                 ImGuiTableColumnFlags_NoSort,
                                widths_[12], static_cast<ImGuiID>(ui::kProcColUserIdDesc));
    }

    // P1②：把 cfg 恢复的显示顺序应用到 ImGui 表。每个表格 id 世代只应用一次
    //（ImGui 自身的内存设置会在会话内记住拖动结果；这里只负责跨会话恢复）。
    void ApplyPersistedColumnOrderOnce() {
        if (!colOrderPendingValid_) return;
        ImGuiTable* table = ImGui::GetCurrentTable();
        if (table == nullptr) return;
        colOrderPendingValid_ = false;
        colOrderAppliedGen_ = Ui().colWidthResetGen;
        // colOrderPending_[d] = 显示位置 d 处的列 UserID（== 提交序槽位）。
        for (int d = 0; d < ui::kProcColSlots; ++d) {
            const int slot = colOrderPending_[d];
            if (slot < 0 || slot >= ui::kProcColSlots || slot == d) continue;
            ImGui::TableSetColumnDisplayOrder(table, slot, d);  // imgui_internal
        }
    }

    // P1②：拖动重排后把新的显示顺序写回 cfg（~1Hz 节流，与 PersistWidths
    // 同一模式）。读 ImGui 表的提交序 → DisplayOrder，按显示位置排列 UserID。
    void PersistColumnOrder(AppContext& ctx) {
        ImGuiTable* table = ImGui::GetCurrentTable();
        if (table == nullptr) return;
        int byDisplay[ui::kProcColSlots] = {};
        for (int slot = 0; slot < ui::kProcColSlots; ++slot) {
            const int order = table->Columns[slot].DisplayOrder;
            if (order < 0 || order >= ui::kProcColSlots) return;  // 异常：不写
            byDisplay[order] = ui::ProcColumnUserId(slot);
        }
        const std::wstring s = ui::ProcColumnOrderToCfg(byDisplay, ui::kProcColSlots);
        if (s.empty() || s == colOrderLastSaved_) return;
        if (ImGui::GetTime() - lastWidthSave_ < 1.0) return;  // 与列宽同节拍
        colOrderLastSaved_ = s;
        ctx.cfg.SetString(L"colOrder", s);
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
        // P2-11（F2 评审）：用稳定的 ProcKey（pid + createTime）标识行，
        // 而非显示索引——交互中途刷新导致的行重排绝不能把
        // 已打开的右键菜单/提示重定向到另一个进程。
        ImGui::PushID(static_cast<int>(p.key.pid));
        ImGui::PushID(static_cast<int>(p.key.createTime & 0x7fffffff));
        ImGui::TableNextRow();

        // 名称 + 行交互（选择、双击、右键菜单）。
        // P1②：列可拖动重排 —— 行单元格一律用 TableSetColumnIndex(槽位)
        // 定位，与显示顺序解耦（TableNextColumn 会跟随当前显示序，重排后
        // 数据会串列）。
        ImGui::TableSetColumnIndex(0);
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

        ImGui::TableSetColumnIndex(1); ImGui::Text("%u", p.key.pid);
        ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(U8(FormatPercent(p.cpuPercent)));
        ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(U8(FormatBytes(p.privateWorkingSet)));
        ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(U8(FormatBytes(p.commitBytes)));
        ImGui::TableSetColumnIndex(5); ImGui::TextUnformatted(U8(FormatRate(p.diskBytesPerSec)));
        ImGui::TableSetColumnIndex(6); ImGui::TextUnformatted(U8(FormatRate(p.netBytesPerSec)));
        ImGui::TableSetColumnIndex(7); ImGui::TextUnformatted(U8(FormatNumber(p.pageFaultsPerSec)));
        ImGui::TableSetColumnIndex(8); ImGui::Text("%u", p.handles);
        ImGui::TableSetColumnIndex(9); ImGui::Text("%u", p.threads);
        ImGui::TableSetColumnIndex(10); ImGui::TextUnformatted(U8(FormatNumber(p.contextSwitchesPerSec)));
        ImGui::TableSetColumnIndex(11); DrawBadges(p);
        ImGui::TableSetColumnIndex(12); DrawDescription(p);
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
        badge(PF_Uwp, "U", ThemeAccent::Info);         // UWP（通用 Windows 应用）
        badge(PF_Wow64, "W", ThemeAccent::Done);       // WOW64（32 位兼容）
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
        // V8-P1-1：清理工作集也是破坏性操作——与终止项同一闸门。
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

    // ---- 详情面板 -------------------------------------------------------------
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

        // 每进程详情经 DetailsProvider（ops JobQueue）异步到达。
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

        // 父进程：显示名称并允许跳转（按本快照中的 ProcKey）。
        const ProcInfo* parent = p.parentPid != 0 ? FindByPid(snap, p.parentPid) : nullptr;
        ImGui::TextDisabled("%s", U8(L"父进程"));
        ImGui::SameLine(110.0f);
        if (parent != nullptr) {
            ImGui::TextUnformatted(U8(Fmt(L"{} ({})", parent->name, parent->key.pid)));
            if (ImGui::SmallButton(U8(L"跳转到父进程"))) {
                selected_ = parent->key;
                lost_ = false;
                lastKnown_ = *parent;
                detailKey_ = ProcKey{};  // 为新选择重新请求详情
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
        double requestedAt = 0.0;  // 上次请求的 ImGui::GetTime()（判断过期）
        ops::ProcessControlInfo info;
        std::wstring err;
    };

    // 在槽位锁下返回副本（本帧渲染期间工作线程可能刷新槽位
    // ——绝不交出指向共享单元的指针）。
    // V15-P2-2：三态查询结果——Pending（任务在途）、Ready（有值）
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

    // 轮询在途的控制信息查询（单槽位）——保持映射干净，
    // 并在任务落地后复位单飞标志。
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
        (void)ctx;  // 提交经 Ui().liveCtx 走（ExecuteConfirmedAction）
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
            // P2-2（F2 评审）：已退出的进程绝不查询——
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

    // ---- F4：模块列表 ---------------------------------------------------------
    // 签名验证在 ops 工作线程运行；结果按模块路径缓存
    //（DriverPage 槽位模式）。-1 = 在途，否则为 int 形式的 ops::SigState。
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
            moduleSigs_.erase(path);  // 队列已停（拆除中）：保持该行诚实
        }
    }

    void DrawModulesSection(AppContext& ctx, const ProcInfo& p, bool alive) {
        if (!alive) return;  // P2-2：进程已死——这里没有可诚实列出的东西
        const ops::ProcessDetails* d = ctx.details->Peek(p.key);
        const bool entryDone = d != nullptr && d->sigResolved;  // 任务整体完成
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
            // 徽标列
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

    // ---- 状态 ----------------------------------------------------------------
    static constexpr int kColCount = static_cast<int>(ui::SortColumn::Count) + 2;

    // 固定列：像素宽。名称（索引 0）与描述（索引 12）是拉伸列，
    // 其槽位存拉伸权重。
    static constexpr float kDefaultWidths[kColCount] = {
        2.0f,    // 名称（拉伸权重）
        64.0f,   // pid
        72.0f,   // cpu
        110.0f,  // 内存（私有工作集）
        96.0f,   // commit
        100.0f,  // disk
        100.0f,  // net
        96.0f,   // 硬缺页
        72.0f,   // handles
        72.0f,   // threads
        104.0f,  // 上下文切换
        96.0f,   // badges
        0.6f,    // 描述（拉伸权重）
    };

    bool loaded_ = false;
    bool sortReflected_ = false;
    bool rebuildNeeded_ = true;
    ui::SortColumn sortColumn_ = ui::SortColumn::Name;
    bool sortDesc_ = false;
    float widths_[kColCount] = {};  // 固定：像素宽；拉伸：权重
    double lastWidthSave_ = 0.0;
    uint64_t appliedResetGen_ = 0;  // H-A: 已应用的「恢复默认列宽」世代号
    // P1②：列显示顺序持久化（cfg "colOrder"，UserID 逗号串按显示位置）。
    // pending = 待应用到 ImGui 表（每个表格 id 世代只应用一次）；lastSaved =
    // 最近一次写盘的顺序串（拖动重排后 ~1Hz 比对写回）。
    int colOrderPending_[ui::kProcColSlots] = {};
    bool colOrderPendingValid_ = false;
    uint64_t colOrderAppliedGen_ = 0;
    std::wstring colOrderLastSaved_;

    uint64_t lastTickId_ = 0;
    std::string filterUtf8_;
    std::wstring filterWide_;
    std::wstring lastFilterWide_;

    ProcKey selected_{};
    bool selectedValid_ = false;
    bool lost_ = false;
    ProcInfo lastKnown_{};  // 进程退出后展示的冻结副本
    ProcKey detailKey_{};
    uint64_t lastDetailTick_ = 0;
    int metaBudget_ = 3;
    // F4：按模块路径的签名验证缓存（ops 工作线程结果）。
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
// PerfPage：8 块两列网格（CPU/每核/内存/磁盘/网络/硬故障/上下文切换/GPU），
// 时间窗 60/120/300/600s。Phase A：历史容量 600、CopyRingTail 线性化 +
// tickSec xscale、字节类 Y 轴 AutoFit、拖拽阈值线。Phase B：块放大
//（cfg perfZoom）+ 放大态跟随/检视状态机 + 悬停精确读数。Phase C：内存/磁盘
// Y2 副轴、GPU 利用率历史、每核热图。Phase D：每适配器吞吐多序列。
// ===========================================================================

int FmtBytesAxis(double value, char* buff, int size, void* /*user_data*/) {
    if (value != value) {  // NaN（无效值）
        return snprintf(buff, static_cast<size_t>(size), "%s", "—");
    }
    const std::string s = WideToUtf8(FormatBytes(value <= 0.0 ? 0u : static_cast<uint64_t>(value)));
    return snprintf(buff, static_cast<size_t>(size), "%s", s.c_str());
}

// Phase A：经 CopyRingTail 取尾窗（window 秒）线性化渲染 —— ImPlot
// values-only，x = 窗内序号（秒）。
void PlotRing(const char* label, const Ring& r, int window, bool noLegend, double tickSec) {
    // V21-P0：ImPlot 的 spec.Offset 以"绘制点数"取模而非环容量——环形 offset 直传
    // 会整窗画错段。线性化到线程局部缓冲后以 Offset=0 绘制；x 轴经 xscale 换算为
    // 真实秒（tickSec = 当前刷新间隔）。
    static thread_local std::vector<float> buf(kHistCap);
    const int n = ui3::CopyRingTail(r, window, buf.data(), static_cast<int>(buf.size()));
    if (n <= 0) return;
    ImPlotSpec spec;
    if (noLegend) spec.Flags = ImPlotItemFlags_NoLegend;
    ImPlot::PlotLine(label, buf.data(), n, tickSec, 0.0, spec);
}

class PerfPage final : public IPage {
public:
    const wchar_t* Id() const override { return L"perf"; }
    const wchar_t* Title() const override { return L"性能"; }

    static inline double tickSec_ = 1.0;  // V21-P0：图表 x 轴秒换算（Draw 与 static 块函数共用）
    // Phase B：放大视图运行态（页对象唯一，静态成员与 tickSec_ 同风格）。
    static inline int zoomApplied_ = -1;  // 已应用的放大块；变化 -> 跟随复位
    static inline bool follow_ = true;    // 放大态跟随（true）/检视（false）
    static inline double lastXMin_ = 0.0, lastXMax_ = 0.0;  // 本帧提交的 X 设定值

    void Draw(AppContext& ctx) override {
        // V19: 顶部固定操作行（图表区上方）——「内存加速…」+ 内存占用速览。
        // 用户报告「内存加速的我目前也没看到」：原入口绘制在全部图表块之后，
        // 位于首屏折叠区以下、需滚动才能看到，且受图表显隐复选框影响布局。
        // 该行不受 lastTick 早退与图表显隐影响，切到「性能」页首屏即可见。
        // L1（用户报告②）：本行 + 下方「时间窗」行组成固定的顶部控制带 ——
        // 两行都绘制在一切可变内容（含「等待采集数据…」早退分支）之前，
        // 高度与位置不随数据变化。
        DrawMemQuickActionRow(Ui().snap ? Ui().snap->sys : SystemInfo{});
        // V21-P0：图表 x 轴按真实采集间隔换算为秒（PlotRing 的 xscale）。
        tickSec_ = static_cast<double>(std::max<int64_t>(200, ctx.cfg.GetInt(L"intervalMs", 1000))) / 1000.0;

        // Phase A：时间窗（cfg perfWindowSec，60/120/300/600，非法回落 120）。
        // 窗口只影响显示（尾窗）不改摄取 —— 切换不清空历史，
        // perfShow* 显隐复选框语义不变（零迁移）。
        int windowSec = ui3::ClampWindowSec(ctx.cfg.GetInt(L"perfWindowSec", 120));
        int windowIdx = ui3::WindowSecIndex(windowSec);
        ImGui::TextDisabled("%s", U8(L"时间窗"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::Combo("##perfwindow", &windowIdx,
                         "60 秒\0"
                         "120 秒\0"
                         "300 秒\0"
                         "600 秒\0")) {
            windowIdx = std::max(0, std::min(3, windowIdx));
            windowSec = ui3::kWindowSecChoices[windowIdx];
            ctx.cfg.SetInt(L"perfWindowSec", windowSec);
        }
        const int window = windowSec;

        // P-A（用户报告①）：「内存加速+速览」行与「时间窗」行组成固定页头，
        // 其下全部内容（等待数据早退、放大态/热图、8 块网格、内存条、告警与
        // CSV 控件）包进填满剩余高度的滚动 Child —— 内容超出视口时只在此
        // Child 内滚动，页头 y 恒定（与网络/传感器页「顶栏固定」同契约）。
        // 高度经 PageLayout.h::FillScrollRegionHeight 扣除一行条目间距，
        // 防止父级（##pagearea）因「子高+间距」恰好超高而出现微滚动条。
        ImGui::BeginChild("##perfscroll",
                          ImVec2(0.0f, ui3::FillScrollRegionHeight(
                                            ImGui::GetContentRegionAvail().y,
                                            ImGui::GetStyle().ItemSpacing.y)),
                          ImGuiChildFlags_None);
        const PerfHistory& h = Hist();
        if (h.lastTick == 0) {
            // L1：早退分支现在画在固定顶栏之下（原来它出现在顶栏位置，
            // 首个 tick 到达时顶栏会被顶下去一行）。
            ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.65f, 1.0f), "%s", U8(L"等待采集数据…"));
            ImGui::EndChild();
            return;
        }

        // Phase B：块放大（cfg perfZoom，-1=无，越界回落 -1）。放大目标
        // 变化（含首次进入放大）时复位为自动跟随最新。
        int zoomBlock = ui3::ClampZoomBlock(ctx.cfg.GetInt(L"perfZoom", -1));
        if (zoomBlock != zoomApplied_) {
            zoomApplied_ = zoomBlock;
            follow_ = ui3::FollowTick(follow_, false, true);
        }

        const ImVec2 avail = ImGui::GetContentRegionAvail();  // 顶部控制带之下
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        // P3 任务二：页面区宽度 > 1200 时两列网格，否则单列铺满（块按序流入）。
        const bool twoCol = ImGui::GetWindowWidth() > 1200.0f;
        const float cellW = twoCol ? (avail.x - spacing) * 0.5f : avail.x;
        // P1④：图表块高下限按布局缩放（FramePadding 等样式缩放已随 DrawShell
        // 生效 → barsH 随行高自动放大；这里的常量下限与间距再显式缩放）。
        const float barsH = ImGui::GetFrameHeightWithSpacing() * 2.0f + ui3::Scaled(8.0f);
        const float plotH = std::max(ui3::Scaled(120.0f), (avail.y - barsH - spacing) * 0.5f);
        const float plotHz = std::max(ui3::Scaled(200.0f), plotH * 1.8f);  // 放大 = 全宽双高

        // 块标题行：显隐复选框 + 放大/还原按钮；放大态追加「跟随最新」
        // 复选与「回到最新」按钮（Phase B §2.3 状态机的 UI 侧）。
        auto header = [&](const wchar_t* key, bool def, const wchar_t* title, int blockIdx) {
            bool show = ctx.cfg.GetBool(key, def);
            if (ImGui::Checkbox(U8(title), &show)) ctx.cfg.SetBool(key, show);
            ImGui::SameLine();
            const bool zoomed = zoomBlock == blockIdx;
            if (ImGui::SmallButton(
                    U8(Fmt(L"{}##zoom{}", zoomed ? L"还原" : L"放大", blockIdx)))) {
                zoomBlock = zoomed ? -1 : blockIdx;
                ctx.cfg.SetInt(L"perfZoom", zoomBlock);
                if (!zoomed) follow_ = ui3::FollowTick(follow_, false, true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", U8(zoomed ? L"还原网格布局"
                                                  : L"放大为全宽详查视图（可框选缩放/拖拽平移/悬停读数）"));
            }
            if (zoomed) {
                ImGui::SameLine();
                if (ImGui::Checkbox(U8(L"跟随最新"), &follow_)) {
                    follow_ = ui3::FollowTick(follow_, false, follow_);
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(follow_);
                if (ImGui::SmallButton(U8(L"回到最新##perfresume"))) {
                    follow_ = ui3::FollowTick(follow_, false, true);
                }
                ImGui::EndDisabled();
            }
            return show;
        };

        // 块体分发（标题行之后的内容）。i 与 ui3::PerfBlockId 一致。
        auto block = [&](int i, float w, float hgt, bool enlarge) {
            switch (i) {
            case ui3::kPerfBlockCpu:
                if (header(L"perfShowCpu", true, L"CPU", i)) {
                    DrawCpu(ctx, w, hgt, h, window, enlarge);
                }
                break;
            case ui3::kPerfBlockPerCore: {
                if (!header(L"perfShowPerCore", true, L"CPU 每核", i)) break;
                // Phase C：每核热图切换（cfg perfPerCoreHeatmap，默认关=曲线；
                // 互斥渲染）。
                bool heatmap = ctx.cfg.GetBool(L"perfPerCoreHeatmap", false);
                ImGui::SameLine();
                if (ImGui::Checkbox(U8(L"热图"), &heatmap)) {
                    ctx.cfg.SetBool(L"perfPerCoreHeatmap", heatmap);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", U8(L"每核利用率热图（行=核心、列=时间，"
                                              L"0-100 固定色标；最多 120 列）。取消勾选回到曲线。"));
                }
                DrawPerCore(w, hgt, h, window, enlarge, heatmap);
                break;
            }
            case ui3::kPerfBlockMem:
                if (header(L"perfShowMem", true, L"内存", i)) {
                    DrawMemory(w, hgt, h, window, enlarge);
                }
                break;
            case ui3::kPerfBlockDisk:
                if (header(L"perfShowDisk", true, L"磁盘", i)) {
                    DrawDisk(w, hgt, h, window, enlarge);
                }
                break;
            case ui3::kPerfBlockNet: {
                if (!header(L"perfShowNet", true, L"网络", i)) break;
                // Phase D：每适配器吞吐开关（cfg perfShowNetAdapters，默认关；
                // 总收/发线保留）。
                bool adaptersOn = ctx.cfg.GetBool(L"perfShowNetAdapters", false);
                ImGui::SameLine();
                if (ImGui::Checkbox(U8(L"每适配器"), &adaptersOn)) {
                    ctx.cfg.SetBool(L"perfShowNetAdapters", adaptersOn);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", U8(L"叠加每适配器吞吐曲线（收+发合计，序列名=适配器"
                                              L"友好名）。适配器超过 8 个时只显示流量 Top 8。"));
                }
                if (adaptersOn &&
                    h.netAdapterTotal > static_cast<int>(h.netAdapters.size())) {
                    // Phase D「Top8 注明」：被截断时如实告知总数。
                    ImGui::TextDisabled(
                        "%s",
                        U8(Fmt(L"适配器共 {} 个，仅显示流量 Top {}", h.netAdapterTotal,
                               h.netAdapters.size())));
                }
                DrawNet(w, hgt, h, window, enlarge, adaptersOn);
                break;
            }
            case ui3::kPerfBlockHardFaults:
                if (header(L"perfShowHardFaults", true, L"硬故障/s", i)) {
                    DrawHardFaults(w, hgt, h, window, enlarge);
                }
                break;
            case ui3::kPerfBlockCtxSwitch:
                if (header(L"perfShowCtxSwitch", false, L"上下文切换/s", i)) {
                    DrawCtxSwitch(w, hgt, h, window, enlarge);
                }
                break;
            case ui3::kPerfBlockGpu:
                if (header(L"perfShowGpu", true, L"GPU", i)) {
                    DrawGpuBlock(*Ui().snap, h, w, hgt, enlarge);
                }
                break;
            default:
                break;
            }
        };

        if (zoomBlock >= 0) {
            // 放大：该块独占全宽双高，其余块隐藏（任务书 §B1）。
            ImGui::BeginGroup();
            block(zoomBlock, avail.x, plotHz, true);
            ImGui::EndGroup();
        } else {
            // 每块 = BeginGroup{标题栏复选框 + 内容}：组边界取块内最大 y，
            // 两列并排时行高随较高块推进，另一列较矮也不会与下一行重叠。
            auto beginCell = [&](int i) {
                if (twoCol && (i & 1) != 0) ImGui::SameLine();
                ImGui::BeginGroup();
            };
            auto endCell = [] { ImGui::EndGroup(); };
            for (int i = 0; i < ui3::kPerfBlockCount; ++i) {
                beginCell(i);
                block(i, cellW, plotH, false);
                endCell();
            }
        }

        const SystemInfo& sys = Ui().snap ? Ui().snap->sys : SystemInfo{};
        DrawMemoryBars(sys);
        ui3::DrawAlertControls(ctx);  // 第 3 阶段：阈值告警控件（增量行）
        DrawCsvControls(ctx, sys);    // F4#7: 性能 CSV 记录开关
        ImGui::EndChild();  // ##perfscroll（P-A：页头固定的滚动内容区）
    }

private:
    // Phase B：缩略图禁用框选（永远跟随，框选无意义且被 X 覆盖冲掉）；
    // 放大态启用框选/平移并叠加十字线（§2.3①）。
    static bool BeginPlotBox(const char* id, const ImVec2& size, bool enlarge) {
        return ImPlot::BeginPlot(id, size, enlarge ? ImPlotFlags_Crosshairs
                                                   : ImPlotFlags_NoBoxSelect);
    }

    // Phase B §2.3：放大态跟随实现。跟随态每帧在 SetupFinish 前以
    // Cond_Always 提交最新窗（xMin = newest-span，xMax = newest）——vendored
    // ImPlot 对 Cond_Always 立即 SetRange；随后 SetupLock 内的输入处理把
    // 用户的平移/框选/滚轮增量叠加在该范围上。检视态不提交 X limits，
    // 用户视窗由 ImPlot 按绘制 id 持久化。
    static void EnlFollowSetupX(const PerfHistory& hist, int window) {
        if (!follow_) return;
        const int count = hist.cpuTotal.Count();
        const double newest = static_cast<double>(std::max(0, count - 1)) * tickSec_;
        const double span = static_cast<double>(std::max(0, window - 1)) * tickSec_;
        lastXMin_ = std::max(0.0, newest - span);
        lastXMax_ = std::max(newest, tickSec_);  // 单样本时避免 0 宽范围
        ImPlot::SetupAxisLimits(ImAxis_X1, lastXMin_, lastXMax_, ImPlotCond_Always);
    }

    // SetupFinish 后比对当前 X 范围与本帧设定值：差值超 ε 即用户动作
    //（平移/框选/滚轮/双击 fit），经 FollowTick 纯函数落入检视态。
    static void EnlFollowDetect() {
        if (!follow_) return;
        constexpr double kEps = 1e-6;
        const ImPlotRect lim = ImPlot::GetPlotLimits(ImAxis_X1, ImAxis_Y1);
        if (std::fabs(lim.X.Min - lastXMin_) > kEps || std::fabs(lim.X.Max - lastXMax_) > kEps) {
            follow_ = ui3::FollowTick(follow_, true, false);
        }
    }

    // Phase B §2.2：放大态 hover 吸附读数 tooltip。鼠标 x（秒）经
    // ReadoutIndexAt 反推环形逻辑下标（取整到 1Hz 采样栅格），逐序列列值
    // 由 PerfReadoutText 生成（NaN -> "—"）。
    static void EnlReadoutTooltip(const PerfHistory& hist, int blockId, int window) {
        if (!ImPlot::IsPlotHovered()) return;
        const ImPlotPoint mp = ImPlot::GetPlotMousePos(ImAxis_X1, ImAxis_Y1);
        const int idx = ui3::ReadoutIndexAt(mp.x, hist.cpuTotal.Count(), window, tickSec_);
        if (idx < 0) return;
        const std::wstring text = ui3::PerfReadoutText(hist, blockId, idx, tickSec_);
        if (!text.empty()) ImGui::SetTooltip("%s", U8(text));
    }

    static void DrawCpu(AppContext& ctx, float w, float plotH,
                        const PerfHistory& hist, int window, bool enlarge) {
        if (!BeginPlotBox("##cpu", ImVec2(w, plotH), enlarge)) return;
        ImPlot::SetupLegend(ImPlotLocation_NorthEast);
        ImPlot::SetupAxis(ImAxis_X1, U8(L"秒"));
        ImPlot::SetupAxis(ImAxis_Y1, U8(L"CPU"));
        if (enlarge) {
            EnlFollowSetupX(hist, window);
        } else {
            ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, static_cast<double>(window - 1) * tickSec_,
                                    ImPlotCond_Always);
        }
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 100.0, ImPlotCond_Always);
        ImPlot::SetupAxisFormat(ImAxis_Y1, "%g%%");
        ImPlot::SetupFinish();
        if (enlarge) EnlFollowDetect();
        // Phase A：告警阈值线（alertOn 开启时）——可拖拽 DragLineY，
        // 拖拽结果双向同步回 cfg alertCpu（钳 1..100、取整百分比）。
        if (ctx.cfg.GetBool(L"alertOn", false)) {
            int stored = static_cast<int>(ctx.cfg.GetInt(L"alertCpu", 90));
            stored = std::max(1, std::min(100, stored));
            double thr = static_cast<double>(stored);
            if (ImPlot::DragLineY(0, &thr, ImVec4(0.90f, 0.35f, 0.30f, 0.90f))) {
                const int dragged =
                    std::max(1, std::min(100, static_cast<int>(thr + 0.5)));
                if (dragged != stored) ctx.cfg.SetInt(L"alertCpu", dragged);
            }
        }
        PlotRing(U8(L"总量"), hist.cpuTotal, window, false, tickSec_);
        for (const Ring& core : hist.cores) PlotRing("##core", core, window, true, tickSec_);
        if (enlarge) EnlReadoutTooltip(hist, ui3::kPerfBlockCpu, window);
        ImPlot::EndPlot();
    }

    // 字节类（内存/磁盘/网络）Y 轴：ImPlotAxisFlags_AutoFit 每 tick 跟随
    // 数据（Phase A 修复「首帧 fit 后永不调整导致削顶」）。放大检视态去掉
    // AutoFit（用户可框选缩放 Y，ImPlot 持久化；回到跟随态恢复 AutoFit，
    // 设计 §2.4③）；CPU/每核保持恒定 0-100，保证扫视可比性。
    static ImPlotAxisFlags Y1ByteFlags(bool enlarge) {
        return (enlarge && !follow_) ? ImPlotAxisFlags_None : ImPlotAxisFlags_AutoFit;
    }

    static void DrawMemory(float w, float plotH, const PerfHistory& hist, int window,
                           bool enlarge) {
        if (!BeginPlotBox("##mem", ImVec2(w, plotH), enlarge)) return;
        ImPlot::SetupLegend(ImPlotLocation_NorthEast);
        ImPlot::SetupAxis(ImAxis_X1, U8(L"秒"));
        ImPlot::SetupAxis(ImAxis_Y1, U8(L"内存"), Y1ByteFlags(enlarge));
        // Phase C：Y2 副轴 = 提交占比%（commit/commitLimit 派生，0-100 固定；
        // 设计 §3.3）。limit<=0 的样本为 NaN，渲染/读数诚实跳过。
        ImPlot::SetupAxis(ImAxis_Y2, U8(L"占比"), ImPlotAxisFlags_AuxDefault);
        if (enlarge) {
            EnlFollowSetupX(hist, window);
        } else {
            ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, static_cast<double>(window - 1) * tickSec_,
                                    ImPlotCond_Always);
        }
        ImPlot::SetupAxisFormat(ImAxis_Y1, &FmtBytesAxis);
        ImPlot::SetupAxisLimits(ImAxis_Y2, 0.0, 100.0, ImPlotCond_Always);
        ImPlot::SetupAxisFormat(ImAxis_Y2, "%g%%");
        ImPlot::SetupFinish();
        if (enlarge) EnlFollowDetect();
        PlotRing(U8(L"可用物理"), hist.physAvail, window, false, tickSec_);
        PlotRing(U8(L"提交"), hist.commit, window, false, tickSec_);
        ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
        PlotRing(U8(L"提交占比"), hist.commitPct, window, false, tickSec_);
        ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
        if (enlarge) EnlReadoutTooltip(hist, ui3::kPerfBlockMem, window);
        ImPlot::EndPlot();
    }

    static void DrawDisk(float w, float plotH, const PerfHistory& hist, int window,
                         bool enlarge) {
        // Phase C：磁盘队列 Y2 副轴——本机无计数器（hasData=false）时整个
        // Y2 隐藏，不留一根空轴（诚实空态与硬故障/s 同模式）。
        const bool showQueue = hist.diskQueue.hasData;
        if (!BeginPlotBox("##disk", ImVec2(w, plotH), enlarge)) return;
        ImPlot::SetupLegend(ImPlotLocation_NorthEast);
        ImPlot::SetupAxis(ImAxis_X1, U8(L"秒"));
        ImPlot::SetupAxis(ImAxis_Y1, U8(L"磁盘"), Y1ByteFlags(enlarge));
        if (showQueue) {
            ImPlot::SetupAxis(ImAxis_Y2, U8(L"队列"),
                              ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_AutoFit);
        }
        if (enlarge) {
            EnlFollowSetupX(hist, window);
        } else {
            ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, static_cast<double>(window - 1) * tickSec_,
                                    ImPlotCond_Always);
        }
        ImPlot::SetupAxisFormat(ImAxis_Y1, &FmtBytesAxis);
        ImPlot::SetupFinish();
        if (enlarge) EnlFollowDetect();
        PlotRing(U8(L"读取"), hist.diskRead, window, false, tickSec_);
        PlotRing(U8(L"写入"), hist.diskWrite, window, false, tickSec_);
        if (showQueue) {
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            PlotRing(U8(L"队列深度"), hist.diskQueue, window, false, tickSec_);
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
        }
        if (enlarge) EnlReadoutTooltip(hist, ui3::kPerfBlockDisk, window);
        ImPlot::EndPlot();
    }

    static void DrawNet(float w, float plotH, const PerfHistory& hist, int window,
                        bool enlarge, bool adaptersOn) {
        if (!BeginPlotBox("##net", ImVec2(w, plotH), enlarge)) return;
        ImPlot::SetupLegend(ImPlotLocation_NorthEast);
        ImPlot::SetupAxis(ImAxis_X1, U8(L"秒"));
        ImPlot::SetupAxis(ImAxis_Y1, U8(L"网络"), Y1ByteFlags(enlarge));
        if (enlarge) {
            EnlFollowSetupX(hist, window);
        } else {
            ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, static_cast<double>(window - 1) * tickSec_,
                                    ImPlotCond_Always);
        }
        ImPlot::SetupAxisFormat(ImAxis_Y1, &FmtBytesAxis);
        ImPlot::SetupFinish();
        if (enlarge) EnlFollowDetect();
        PlotRing(U8(L"接收"), hist.netRecv, window, false, tickSec_);
        PlotRing(U8(L"发送"), hist.netSend, window, false, tickSec_);
        // Phase D：每适配器吞吐多序列（Up 适配器各一条线 = 收+发合计，
        // 序列名 = 友好名；>8 条已在摄取端按流量 Top8 截断）。适配器环与
        // 系统 ring 不同步（重建即重启），各自线性化渲染。
        if (adaptersOn) {
            for (size_t i = 0; i < hist.netAdapters.size(); ++i) {
                PlotRing(U8(hist.netAdapterNames[i]),
                         hist.netAdapters[i], window, false, tickSec_);
            }
        }
        if (enlarge) EnlReadoutTooltip(hist, ui3::kPerfBlockNet, window);
        ImPlot::EndPlot();
    }

    // ---- P3 任务二：新增图表 --------------------------------------------------
    // 不可用计数器的诚实空态（硬故障/s 在部分机器上 PDH 无此计数器；上下文切换/s
    // 在兼容模式下无每进程数据可聚合）。
    // L1（用户报告②审计）：空态行撑到与图表块同高（plotH）——计数器数据首次
    // 到达与否都不改变网格行高，网格之下的内存条/告警/CSV 行不再被顶动。
    static void DrawCounterUnavailable(float plotH) {
        const float y0 = ImGui::GetCursorPosY();
        ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.65f, 1.0f), "%s", U8(L"本机此计数器不可用"));
        const float deficit = plotH - (ImGui::GetCursorPosY() - y0);
        if (deficit > 0.0f) ImGui::Dummy(ImVec2(0.0f, deficit));
    }

    static void DrawPerCore(float w, float plotH, const PerfHistory& hist, int window,
                            bool enlarge, bool heatmap) {
        // Phase C：每核热图（行=核心、列=尾窗时间，最多 kHeatmapMaxCols 列，
        // 0-100 固定色标 Viridis）。与曲线互斥渲染。
        const ui3::CoreHeatmap hm =
            heatmap ? ui3::CoreHeatmapValues(hist.cores, window) : ui3::CoreHeatmap{};
        const int span = heatmap ? std::max(1, hm.cols) : window;
        if (heatmap && (hm.rows <= 0 || hm.cols <= 0)) {
            DrawCounterUnavailable(plotH);
            return;
        }
        if (!BeginPlotBox("##percore", ImVec2(w, plotH), enlarge)) return;
        ImPlot::SetupLegend(ImPlotLocation_NorthEast);
        ImPlot::SetupAxis(ImAxis_X1, U8(L"秒"));
        ImPlot::SetupAxis(ImAxis_Y1, U8(L"CPU 每核"), heatmap ? ImPlotAxisFlags_Invert : 0);
        if (heatmap) {
            // 热图数据恒为重定基尾窗（x ∈ [0, cols·tick]）——跟随 = 固定
            // 全域，每帧 Cond_Always 提交；不参与跟随/检视状态机。
            ImPlot::SetupAxisLimits(ImAxis_X1, 0.0,
                                    static_cast<double>(span) * tickSec_,
                                    ImPlotCond_Always);
        } else if (enlarge) {
            EnlFollowSetupX(hist, span);
        } else {
            ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, static_cast<double>(span - 1) * tickSec_,
                                    ImPlotCond_Always);
        }
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 100.0, ImPlotCond_Always);
        ImPlot::SetupAxisFormat(ImAxis_Y1, "%g%%");
        ImPlot::SetupFinish();
        if (enlarge && !heatmap) EnlFollowDetect();
        if (heatmap) {
            ImPlot::PushColormap(ImPlotColormap_Viridis);
            ImPlot::PlotHeatmap("##corehm", hm.values.data(), hm.rows, hm.cols, 0.0, 100.0,
                                "%.0f", ImPlotPoint(0, 0),
                                ImPlotPoint(static_cast<double>(hm.cols) * tickSec_,
                                            static_cast<double>(hm.rows)));
            ImPlot::PopColormap();
        } else {
            for (const Ring& core : hist.cores) PlotRing("##core", core, window, true, tickSec_);
        }
        if (enlarge && !heatmap) EnlReadoutTooltip(hist, ui3::kPerfBlockPerCore, window);
        ImPlot::EndPlot();
    }

    static void DrawHardFaults(float w, float plotH, const PerfHistory& hist, int window,
                               bool enlarge) {
        if (!hist.hardFaults.hasData) {  // 从未收到有效样本：诚实空态而非空图
            DrawCounterUnavailable(plotH);
            return;
        }
        if (!BeginPlotBox("##hardfaults", ImVec2(w, plotH), enlarge)) return;
        ImPlot::SetupLegend(ImPlotLocation_NorthEast);
        ImPlot::SetupAxis(ImAxis_X1, U8(L"秒"));
        ImPlot::SetupAxis(ImAxis_Y1, U8(L"硬故障/s"));
        if (enlarge) {
            EnlFollowSetupX(hist, window);
        } else {
            ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, static_cast<double>(window - 1) * tickSec_,
                                    ImPlotCond_Always);
        }
        ImPlot::SetupAxisFormat(ImAxis_Y1, "%g");
        ImPlot::SetupFinish();
        if (enlarge) EnlFollowDetect();
        PlotRing(U8(L"硬故障"), hist.hardFaults, window, false, tickSec_);
        if (enlarge) EnlReadoutTooltip(hist, ui3::kPerfBlockHardFaults, window);
        ImPlot::EndPlot();
    }

    static void DrawCtxSwitch(float w, float plotH, const PerfHistory& hist, int window,
                              bool enlarge) {
        if (!hist.ctxSwitch.hasData) {
            DrawCounterUnavailable(plotH);
            return;
        }
        if (!BeginPlotBox("##ctxswitch", ImVec2(w, plotH), enlarge)) return;
        ImPlot::SetupLegend(ImPlotLocation_NorthEast);
        ImPlot::SetupAxis(ImAxis_X1, U8(L"秒"));
        ImPlot::SetupAxis(ImAxis_Y1, U8(L"上下文切换/s"));
        if (enlarge) {
            EnlFollowSetupX(hist, window);
        } else {
            ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, static_cast<double>(window - 1) * tickSec_,
                                    ImPlotCond_Always);
        }
        ImPlot::SetupAxisFormat(ImAxis_Y1, "%g");
        ImPlot::SetupFinish();
        if (enlarge) EnlFollowDetect();
        PlotRing(U8(L"切换"), hist.ctxSwitch, window, false, tickSec_);
        if (enlarge) EnlReadoutTooltip(hist, ui3::kPerfBlockCtxSwitch, window);
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

    // V19: 性能页顶部固定操作行 ——「内存加速…」入口 + 当前内存占用速览
    // （已用/总量）。复用 RequestConfirmMemCleanup（DrawConfirmDialogs 的
    // MemCleanup kind -> ExecuteConfirmedAction），不新建执行路径。
    static void DrawMemQuickActionRow(const SystemInfo& sys) {
        if (ImGui::Button(U8(L"内存加速…"))) RequestConfirmMemCleanup();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "%s", U8(L"按当前私有工作集挑选高占用进程（默认勾选前 5），批量提示系统"
                          L"释放工作集内存（估计值，被换出的页再次访问有缺页代价）；"
                          L"可同时清理系统待机缓存。系统关键进程不可选。"));
        }
        if (sys.physTotal > 0) {
            ImGui::SameLine();
            ImGui::TextUnformatted(U8(Fmt(L"内存占用：{} / {}",
                                          FormatBytes(sys.physTotal - sys.physAvail),
                                          FormatBytes(sys.physTotal))));
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
        // 「内存加速」入口（原「一键优化」，V19 文案统一）—— 打开 MemCleanup
        // 确认模态（DrawConfirmDialogs 的 MemCleanup kind，每帧渲染模式）。
        // 顶部固定操作行已有同名入口；此处保留块内就近入口。
        if (ImGui::Button(U8(L"内存加速…"))) RequestConfirmMemCleanup();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "%s", U8(L"按当前私有工作集挑选高占用进程（默认勾选前 5），批量提示系统释放"
                          L"工作集内存（估计值，再次访问被换出的页有缺页代价）；可同时清理"
                          L"系统待机缓存。系统关键进程不可选。"));
        }
    }

    // P1-5 (F2 review): GPU block. P3 任务二起标题栏复选框在网格单元里，
    // 这里只剩内容体（GPU 表格 + 进程 Top5）。数据每 2 s 产出一批。
    static std::wstring GpuUtilText(double v) {
        return v == v ? Fmt(L"{:.1f}%", v) : std::wstring(L"—");  // NaN = 不可得
    }

    // 按 pid 聚合 (luid, pid) 行：利用率求和并钳制到 100，
    // 内存求和；利用率不可得的行仍贡献其内存。
    // 至多返回 `top` 条，利用率最高在前。
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

    // L1（用户报告②审计）：GPU 块体（适配器表行数、进程 Top5 出现与否随数据
    // 变化）装入**定高滚动子区**（高度 = 同行图表块的绘图区高）——网格单元高
    // 度因此与纯图表块一致且恒定，下方内容不再被数据增减顶动。内部可滚动。
    // Phase C：块顶追加「利用率历史」小图（多卡合计 %，2s 一点；数据缺席
    // 时 hasData=false 诚实隐藏，不留空图）。
    static void DrawGpuBlock(const Snapshot& snap, const PerfHistory& hist, float cellW,
                             float bodyH, bool enlarge) {
        ImGui::BeginChild("##gpubody", ImVec2(cellW, bodyH), ImGuiChildFlags_None);
        if (hist.gpuUtil.hasData) {
            const float sparkH = enlarge ? bodyH * 0.35f : 90.0f;
            DrawGpuUtilSpark(hist, cellW, sparkH, kHistCap);
        }
        DrawGpuBlockBody(snap);
        ImGui::EndChild();
    }

    // GPU 利用率合计%历史小图（装饰轴 CanvasOnly；0-100 固定，跟随最新窗）。
    static void DrawGpuUtilSpark(const PerfHistory& hist, float w, float h, int window) {
        ImGui::TextDisabled("%s", U8(L"利用率历史（多卡合计 %）"));
        const ImPlotFlags flags = ImPlotFlags_CanvasOnly | ImPlotFlags_NoInputs;
        if (ImPlot::BeginPlot("##gpuutilhist", ImVec2(w, h), flags)) {
            ImPlot::SetupAxis(ImAxis_X1, nullptr,
                              ImPlotAxisFlags_NoDecorations | ImPlotAxisFlags_Lock);
            ImPlot::SetupAxis(ImAxis_Y1, nullptr,
                              ImPlotAxisFlags_NoDecorations | ImPlotAxisFlags_Lock);
            ImPlot::SetupAxisLimits(ImAxis_X1, 0.0,
                                    static_cast<double>(window - 1) * tickSec_,
                                    ImPlotCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 100.0, ImPlotCond_Always);
            ImPlot::SetupFinish();
            PlotRing(U8(L"GPU 利用率"), hist.gpuUtil, window, true, tickSec_);
            ImPlot::EndPlot();
        }
    }

    // P1③：GPU 适配器/进程明细表列宽持久化（netcol_gpuadapters_*/netcol_gpuprocs_*，
    // 登记见 ThemeCfg.h::NetColTableSpecs）。与 Pages3.cpp 的 NetColWidths 同一
    // 模式（该助手在本编译单元的匿名命名空间内不可跨 TU 复用 —— 两处实现都
    // 只经 NetColCfgKey 产键，键名契约由 selftest 钉死）。liveCtx 为空（离屏
    // smoke 尾帧）时退回默认宽，仅跳过持久化。
    struct GpuColCache {
        float w[4] = {};
        bool loaded = false;
        uint64_t loadedGen = 0;  // V29-P1-1：布局重置代际失效
    };
    static std::map<std::string, GpuColCache>& GpuColCaches() {
        static std::map<std::string, GpuColCache> m;
        return m;
    }
    static float* GpuColWidths(const char* table, const float* defaults) {
        GpuColCache& c = GpuColCaches()[table];
        const uint64_t gen = ui3::LayoutResetGeneration();
        if (!c.loaded || c.loadedGen != gen) {
            c.loaded = true;
            c.loadedGen = gen;
            for (int i = 0; i < 4; ++i) c.w[i] = defaults[i];
            if (std::shared_ptr<AppContext> app = Ui().liveCtx) {
                for (int i = 0; i < 4; ++i) {
                    c.w[i] = static_cast<float>(
                        app->cfg.GetDouble(ui3::NetColCfgKey(table, i), defaults[i]));
                }
            }
        }
        return c.w;
    }
    static void GpuColSaveWidths(const char* table, const bool* persistMask) {
        ImGuiTable* t = ImGui::GetCurrentTable();
        std::shared_ptr<AppContext> app = Ui().liveCtx;
        if (t == nullptr || !app) return;
        GpuColCache& c = GpuColCaches()[table];
        for (int i = 0; i < 4; ++i) {
            if (!persistMask[i]) continue;
            const float w = t->Columns[i].WidthGiven;
            if (w <= 0.01f || w == c.w[i]) continue;
            c.w[i] = w;
            app->cfg.SetDouble(ui3::NetColCfgKey(table, i), static_cast<double>(w));
        }
    }

    static void DrawGpuBlockBody(const Snapshot& snap) {
        if (snap.sys.gpus.empty()) {
            // 诚实的空态：首个 tick 未到，或没有适配器
            ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.65f, 1.0f), "%s",
                               U8(L"暂无 GPU 数据（等待采集，或本机无适配器）"));
            return;
        }
        // P1③：适配器明细可拖宽 + cfg 持久化（适配器名列为拉伸列）。
        static constexpr float kDefA[4] = {2.4f, 72.0f, 92.0f, 92.0f};
        static constexpr bool kPersistA[4] = {false, true, true, true};
        float* wa = GpuColWidths("gpuadapters", kDefA);
        if (ImGui::BeginTable("gpuadapters", 4, ImGuiTableFlags_Resizable |
                                                   ImGuiTableFlags_RowBg |
                                                   ImGuiTableFlags_BordersInnerH |
                                                   ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn(U8(L"适配器"), ImGuiTableColumnFlags_WidthStretch, wa[0]);
            ImGui::TableSetupColumn(U8(L"利用率"), ImGuiTableColumnFlags_WidthFixed, wa[1]);
            ImGui::TableSetupColumn(U8(L"显存已用"), ImGuiTableColumnFlags_WidthFixed, wa[2]);
            ImGui::TableSetupColumn(U8(L"显存总量"), ImGuiTableColumnFlags_WidthFixed, wa[3]);
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
            GpuColSaveWidths("gpuadapters", kPersistA);
            ImGui::EndTable();
        }
        const std::vector<GpuProcRow> top = TopGpuProcs(snap, 5);
        if (!top.empty()) {
            ImGui::TextDisabled("%s", U8(L"按进程 GPU 占用（Top 5，跨适配器合计）"));
            // P1③：GPU 进程明细可拖宽 + cfg 持久化（进程名列拉伸，不持久化）。
            static constexpr float kDefP[4] = {2.4f, 72.0f, 92.0f, 92.0f};
            static constexpr bool kPersistP[4] = {false, true, true, true};
            float* wp = GpuColWidths("gpuprocs", kDefP);
            if (ImGui::BeginTable("gpuprocs", 4, ImGuiTableFlags_Resizable |
                                                     ImGuiTableFlags_RowBg |
                                                     ImGuiTableFlags_BordersInnerH |
                                                     ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn(U8(L"进程"), ImGuiTableColumnFlags_WidthStretch, wp[0]);
                ImGui::TableSetupColumn(U8(L"利用率"), ImGuiTableColumnFlags_WidthFixed, wp[1]);
                ImGui::TableSetupColumn(U8(L"专用显存"), ImGuiTableColumnFlags_WidthFixed, wp[2]);
                ImGui::TableSetupColumn(U8(L"共享显存"), ImGuiTableColumnFlags_WidthFixed, wp[3]);
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
                GpuColSaveWidths("gpuprocs", kPersistP);
                ImGui::EndTable();
            }
        }
    }
};

// ===========================================================================
// 外壳：工具栏/标签/状态栏。
// ===========================================================================

void DrainNotifications(AppContext& ctx) {
    std::vector<Notification> out;
    ctx.notes.Drain(&out);
    for (const Notification& n : out) {
        // P2-1（F2 评审）：详情提供者通知在每次行选择时都会触发——
        // 高频噪音。它们只更新状态栏；toast 保留给
        // 破坏性/可观察的操作。
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

// F4#10: 全局热键 Ctrl+Alt+M 的统一开关路径（R-Fix Bug1 起状态栏复选框与窄窗
// 「⋮」菜单项共用）。注册/反注册在主线程完成；失败不落 cfg（下一帧按持久状态
// 回显的诚实状态机），只弹失败 toast。
void ApplyHotkeyEnabled(AppContext& ctx, bool on) {
    std::wstring err;
    if (ui3::GcHotkeySetEnabled(on, &err)) {
        ctx.cfg.SetBool(L"hotkeyEnabled", on);
        PushToast(Notification::Kind::Info,
                  on ? L"已注册全局热键 Ctrl+Alt+M（呼出/隐藏主窗）" : L"已注销全局热键");
    } else {
        PushToast(Notification::Kind::JobFailed, err);
    }
}

// A1 统一风格改造：「外观设置」模态（独立 ##appearance 模态 id）。
// 旧「外观/⋮」菜单壳与窄窗折叠分支删除，菜单体内容（主题三态 + 恢复默认
// 列宽 + 自定义壁纸控件组）整体迁移进本模态——用户找不到主题/壁纸入口的
// 直接修复：入口变成与「暂停采集」同款的工具条按钮「主题…」。
// 模态走 DrawConfirmDialogs 同款每帧渲染模式：请求长期有效，
// OpenPopup 只发一次，BeginPopupModal 每帧执行（Esc/「关闭」均可退出）。
void RequestOpenAppearance() {
    Ui().appearanceOpenRequested = true;
    Ui().appearanceOpened = false;
}

// P1④：实现位于 DrawToolbar 前（外观模态与工具条共用两个入口）。
void ApplyOneClickLayout(AppContext& ctx);
void ResetLayout(AppContext& ctx);

void DrawAppearanceModal(AppContext& ctx) {
    if (Ui().appearanceOpenRequested) {
        if (!ImGui::IsPopupOpen("##appearance")) {
            ImGui::OpenPopup("##appearance");
            Ui().appearanceOpened = true;
        }
        Ui().appearanceOpenRequested = false;
    }
    if (!Ui().appearanceOpened) return;

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(460.0f, 0.0f), ImVec2(460.0f, FLT_MAX));
    if (!ImGui::BeginPopupModal("##appearance", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        // 模态之前开着但现在没了：被 Esc 关闭（「关闭」按钮之外的唯一路径），
        // 复位打开态——绝不残留隐形模态。
        if (!ImGui::IsPopupOpen("##appearance") && !Ui().appearanceOpenRequested) {
            Ui().appearanceOpened = false;
        }
        return;
    }

    // 标题在模态体内绘制（与 ##confirm/##about 同款：##id 窗口、正文自带标题）。
    ImGui::TextUnformatted(U8(L"外观设置"));
    ImGui::Separator();

    // 主题三态单选：点击即时生效（写 cfg + Theme::Apply，与启动应用同一入口；
    // System 模式的实时广播由 main 的 WM_SETTINGCHANGE 钩子处理）。
    const ThemeMode cur = ThemeModeFromInt(ctx.cfg.GetInt(L"themeMode", 0));
    struct ModeItem { ThemeMode mode; const wchar_t* label; };
    static const ModeItem kModes[] = {
        {ThemeMode::Dark,   L"深色"},
        {ThemeMode::Light,  L"浅色"},
        {ThemeMode::System, L"跟随系统"},
    };
    ImGui::TextDisabled("%s", U8(L"主题"));
    for (const ModeItem& m : kModes) {
        if (m.mode != kModes[0].mode) ImGui::SameLine();
        if (ImGui::RadioButton(U8(m.label), m.mode == cur)) {
            // V21-P2-3：点击已选中的主题不再重复 Apply + toast（避免误导性"已切换"）。
            if (m.mode != cur) {
                ctx.cfg.SetInt(L"themeMode", static_cast<int64_t>(m.mode));
                Theme::Apply(m.mode);
                PushToast(Notification::Kind::Info, Fmt(L"主题已切换：{}", m.label));
            }
        }
    }
    ImGui::Separator();

    ImGui::TextDisabled("%s", U8(L"自定义壁纸"));
    DrawAppearancePanel(ctx);  // H-A(Phase-6): 选图/关闭/遮罩（实时生效）/性能提示

    ImGui::Separator();
    // P1④：一键优化布局（分辨率/PPI → 布局缩放；详见 ApplyOneClickLayout）。
    if (ImGui::Button(U8(L"一键优化布局"))) ApplyOneClickLayout(ctx);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", U8(L"按当前分辨率与系统 DPI 自动缩放：区域高度、表格行高、"
                                   L"间距与图表高度（等效工具条上的同名按钮）"));
    }
    ImGui::SameLine();
    if (ImGui::Button(U8(L"重置布局"))) ResetLayout(ctx);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", U8(L"清除布局缩放、列顺序、进程表/网络表列宽与图表放大，"
                                   L"恢复默认布局"));
    }
    ImGui::Separator();
    if (ImGui::Button(U8(L"恢复默认列宽"))) {
        // 键清单登记见 ui3::ColWidthCfgKeys()（app/ui3/ThemeCfg.h，与
        // ProcessesPage::PersistWidths 写入一一对应）。
        ui3::SoftDeleteColWidthKeys(ctx.cfg);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", U8(L"恢复进程表为默认列宽与列顺序；主题/壁纸设置不受影响"));
        }
        ctx.cfg.SetString(L"colOrder", L"");  // P1②：恢复默认列宽同时重置列顺序
        const int removed = ui3::StripColWidthKeysFromFile(ConfigPath(), false);
        ++Ui().colWidthResetGen;  // 进程页下一帧重置 widths_ 并换代表格 id
        PushToast(Notification::Kind::JobDone,
                  removed < 0 ? Fmt(L"已恢复默认列宽（配置文件格式异常，重启后生效）")
                              : Fmt(L"已恢复默认列宽（清除 {} 项自定义列宽）", removed));
    }
    ImGui::SameLine();
    if (ImGui::Button(U8(L"关闭"), ImVec2(120.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ===========================================================================
// 维护轮 10：兼容模式说明模态。
// 入口 = 状态栏「兼容模式：<原因>」（degraded 时可点击，见 DrawStatusBar）。
// 内容：① 降级原因摘要 ② 6 项自检逐项结果表（绿勾/红叉/跳过 + 测量对比）
// ③「为什么会这样」（诚实措辞）④ 降级后受影响的功能 ⑤ 操作按钮：
// [重新自检]（RequestSelfCheckRetry，期间禁用）/ [复制诊断报告]（剪贴板 +
// logs\diagnostics_*.txt，绝不自动上传）/ [关闭]。
// 与 ##confirm/##appearance 同款「请求长期有效 + 模态每帧渲染」模式。
// ===========================================================================

void RequestOpenCompatDiag() {
    Ui().compatDiagOpenRequested = true;
    Ui().compatDiagOpened = false;
}

void DrawCompatDiagModal(AppContext& ctx) {
    UiState& s = Ui();
    if (s.compatDiagOpenRequested) {
        if (!ImGui::IsPopupOpen("##compatdiag")) {
            ImGui::OpenPopup("##compatdiag");
            s.compatDiagOpened = true;
        }
        s.compatDiagOpenRequested = false;
    }
    if (!s.compatDiagOpened) return;

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 0.0f), ImVec2(560.0f, FLT_MAX));
    if (!ImGui::BeginPopupModal("##compatdiag", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        // 模态之前开着但现在没了：被 Esc 关闭（「关闭」按钮之外的唯一路径），
        // 复位打开态与重试待定（V24 P2-1：迟到完成不再弹 stale toast）——
        // 绝不残留隐形模态。重跑请求本身仍会被采集线程消费，只是 UI 不再等它。
        if (!ImGui::IsPopupOpen("##compatdiag") && !s.compatDiagOpenRequested) {
            s.compatDiagOpened = false;
            s.compatRetryPending = false;
        }
        return;
    }

    // V24 P1-1：完成检测前置且以代际为信号。观察到代际变化后再重新拉取
    // 报告判定 toast，避免同帧 stale items。
    uint64_t compatGenNow = ctx.collect.LastSelfCheckGeneration();
    if (s.compatRetryPending && compatGenNow != s.compatRetryGenBase) {
        s.compatRetryPending = false;
        const std::vector<CollectService::SelfCheckItem> fresh = ctx.collect.LastSelfCheckReport();
        bool retryOk = !fresh.empty() && fresh[0].ran;  // 第 1 项（CPU）必须真的跑过
        for (const CollectService::SelfCheckItem& it : fresh) {
            if (it.ran && !it.passed) retryOk = false;
        }
        PushToast(retryOk ? Notification::Kind::JobDone : Notification::Kind::Warn,
                  retryOk ? L"重新自检通过，已退出兼容模式"
                          : L"重新自检仍未通过，保持兼容模式（详见逐项结果）");
    }

    // 每帧取最新逐项报告（6 项小拷贝，帧预算内）与快照状态。
    const std::vector<CollectService::SelfCheckItem> items = ctx.collect.LastSelfCheckReport();
    const bool degraded = s.snap && s.snap->degraded;
    const std::wstring reason =
        s.snap ? (s.snap->degradeReason.empty() ? std::wstring(L"采集能力受限")
                                                : s.snap->degradeReason)
               : std::wstring();

    ImGui::TextUnformatted(U8(L"兼容模式说明"));
    ImGui::Separator();

    // ① 降级原因摘要。
    if (degraded) {
        ImGui::TextColored(ColWarn(), "%s", U8(L"当前处于兼容模式（Toolhelp+PSAPI 慢路径）"));
        ImGui::TextWrapped("%s", U8(Fmt(L"降级原因：{}", reason)));
    } else {
        ImGui::TextColored(ColDone(), "%s", U8(L"当前为完整模式（自检通过）"));
    }

    // ② 逐项结果表：状态列（√ 通过 / × 失败 / — 跳过）+ 检查项与测量值对比。
    ImGui::Spacing();
    ImGui::TextDisabled("%s", U8(L"自检逐项结果（NtQSI 快路径 vs 文档化 API，加固：重试至多 2 次 + 连续 2 轮通过）"));
    if (items.empty()) {
        ImGui::TextDisabled("%s", U8(L"尚未自检（采集服务第一个采集周期运行自检门）"));
    } else if (ImGui::BeginTable("##compatitems", 2,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("res", ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn("item", ImGuiTableColumnFlags_WidthStretch);
        for (const CollectService::SelfCheckItem& it : items) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (!it.ran) {
                ImGui::TextDisabled("%s", U8(L"— 跳过"));
            } else if (it.passed) {
                ImGui::TextColored(ColDone(), "%s", U8(L"√ 通过"));
            } else {
                ImGui::TextColored(ColFail(), "%s", U8(L"× 失败"));
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(U8(it.name == nullptr ? std::wstring() : std::wstring(it.name)));
            ImGui::TextDisabled("%s", U8(it.detail));
        }
        ImGui::EndTable();
    }

    // ③ 为什么会这样（诚实措辞：不甩锅、不过度承诺）。
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("%s", U8(L"为什么会这样？"));
    ImGui::BulletText("%s", U8(L"Windows 更新可能更改 NtQuerySystemInformation 返回的内部结构"
                               L"布局，使旧布局读数与官方 API 不一致。"));
    ImGui::BulletText("%s", U8(L"第三方安全软件可能挂钩 NtQuerySystemInformation 并修改其"
                               L"返回数据。"));
    ImGui::BulletText("%s", U8(L"系统策略或运行环境（虚拟化、精简系统）可能限制读取进程内部"
                               L"计数器。"));
    ImGui::TextWrapped("%s",
                       U8(L"为了不显示错误数据，应用在无法证实快速路径可靠时会整体切换到 "
                          L"Toolhelp+PSAPI 兼容路径（较慢、部分列缺失），这是刻意的保护行为，"
                          L"不代表应用损坏。"));

    // ④ 降级后受影响的功能（哪些列显示「—」）。
    ImGui::Spacing();
    ImGui::TextDisabled("%s", U8(L"降级后受影响的功能"));
    ImGui::BulletText("%s", U8(L"进程表「内存」列（私有工作集）显示「—」，也无法按它排序。"));
    ImGui::BulletText("%s", U8(L"「上下文切换/s」列显示「—」。"));
    ImGui::BulletText("%s", U8(L"已挂起进程的徽标与详情中的挂起状态不再显示。"));
    ImGui::BulletText("%s", U8(L"「内存加速」无法按私有工作集挑选候选。"));
    ImGui::BulletText("%s", U8(L"其余功能（CPU/磁盘/网络速率、进程管理等）不受影响，仅采集稍慢。"));

    // ⑤ 操作按钮：[重新自检]（期间/采集暂停时禁用）[复制诊断报告] [关闭]。
    ImGui::Spacing();
    ImGui::Separator();
    const bool collectPaused = s.paused;  // V24 P2-1：暂停采集时无 tick，重跑无法发生
    ImGui::BeginDisabled(s.compatRetryPending || collectPaused);
    if (ImGui::Button(U8(L"重新自检"))) {
        ctx.collect.RequestSelfCheckRetry();
        s.compatRetryPending = true;
        // V24 P1-1：记录当前门代际；代际变化（而非 tick+1）才是"跑完"。
        s.compatRetryGenBase = ctx.collect.LastSelfCheckGeneration();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", U8(collectPaused
                                       ? L"采集已暂停：恢复采集后才能重新自检"
                                       : (s.compatRetryPending
                                              ? L"自检重跑进行中（含自动重试，约 1-2 秒）"
                                              : L"重跑自检门（含自动重试）；全部通过将自动退出兼容模式")));
    }
    if (collectPaused) {
        ImGui::TextDisabled("%s", U8(L"采集已暂停：无法重新自检，请先恢复采集（工具条「继续采集」）。"));
    }
    ImGui::SameLine();
    if (ImGui::Button(U8(L"复制诊断报告"))) {
        const std::wstring text = ui3::CompatDiagReportText(kAppVersion, items, degraded ? reason : L"");
        ImGui::SetClipboardText(U8(text));
        const std::wstring path = ui3::SaveDiagnosticsFile(text);
        PushToast(Notification::Kind::JobDone,
                  path.empty() ? std::wstring(L"诊断报告已复制到剪贴板（写入日志文件失败）")
                               : Fmt(L"诊断报告已复制到剪贴板，并保存到 {}", path));
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", U8(L"纯文本：系统版本 + 应用版本 + 逐项测量对比；仅本机保存，"
                                   L"不会自动上传"));
    }
    ImGui::SameLine();
    if (ImGui::Button(U8(L"关闭"))) {
        ImGui::CloseCurrentPopup();
        s.compatDiagOpened = false;
        s.compatRetryPending = false;  // V24 P2-1：关模态即不再等待迟到完成
    }

    // （V24 P1-1）完成检测已前置到模态开头：以 LastSelfCheckGeneration 代际
    // 变化为信号并重新拉取报告，避免 tick 计数提前与同帧 stale items。

    ImGui::EndPopup();
}

// ===========================================================================
// P-C：日志查看器模态（工具条「日志」按钮）。
// 内容：控制行（级别过滤[全部/仅警告+错误] / 刷新 / 自动刷新(默认关,1s) /
// 生成诊断报告 / 打开日志目录）+ 日志表格（时间/级别/模块/消息，ListClipper，
// 最新在表尾；ERROR 红/WARN 黄着色）+ 底部统计行（跟随最新 + 共 N 条）。
// 「生成诊断报告」：FormatForReport(tail, 50, SystemVersionLine())，兼容模式
// 降级时附加 CompatDiagReportText（未降级只含日志部分）→ 模态内二级展示全文，
// [复制到剪贴板] + [保存到 logs 目录]（复用 SaveDiagnosticsFile，绝不自动上传）。
// 读取策略：UI 线程同步读（512KB 尾窗 + 500 行上限为毫秒级，理由见 UiState
// 注释）；打开首帧与手动/自动刷新时读取，其余帧不碰文件。
// 与 ##confirm/##appearance/##compatdiag 同款「请求长期有效 + 模态每帧渲染」
// 模式（Esc/「关闭」均可退出，绝不残留隐形模态）。
// ===========================================================================
void RequestOpenLogViewer() {
    Ui().logViewerOpenRequested = true;
    Ui().logViewerOpened = false;
}

void DrawLogViewer(AppContext& ctx) {
    UiState& s = Ui();
    LogViewerUi& lv = s.logViewer;
    if (s.logViewerOpenRequested) {
        if (!ImGui::IsPopupOpen("##logviewer")) {
            ImGui::OpenPopup("##logviewer");
            s.logViewerOpened = true;
            lv.loaded = false;      // 每次打开都重读（打开时读取，见上策略注释）
            lv.reportOpen = false;  // 不残留上次的报告二级视图
        }
        s.logViewerOpenRequested = false;
    }
    if (!s.logViewerOpened) return;

    // 尺寸：目标 860x600，受工作区 80% 钳制（小窗口仍可用），下限 560x380。
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    const float w = std::min(860.0f, std::max(560.0f, vp->WorkSize.x * 0.8f));
    const float h = std::min(600.0f, std::max(380.0f, vp->WorkSize.y * 0.8f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 380.0f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("##logviewer", nullptr, 0)) {
        // 模态之前开着但现在没了：被 Esc 关闭（「关闭」按钮之外的唯一路径），
        // 复位打开态——绝不残留隐形模态。
        if (!ImGui::IsPopupOpen("##logviewer") && !s.logViewerOpenRequested) {
            s.logViewerOpened = false;
            lv.reportOpen = false;
        }
        return;
    }

    const std::wstring logPath = LogDir() + L"\\stm.log";
    auto readNow = [&lv, &logPath] {
        lv.tail = stm::ReadLogTail(logPath, ui3::kLogViewerMaxLines);
        lv.rows = ui3::BuildDisplayRows(lv.tail, lv.filterMode);
        lv.lastReadTime = ImGui::GetTime();
        lv.loaded = true;
        if (lv.followNewest) lv.scrollBottomPending = true;
    };
    // 打开首帧（!loaded）/ 勾选自动刷新后的 1s 周期：UI 线程同步重读。
    if (!lv.loaded || (lv.autoRefresh && ImGui::GetTime() - lv.lastReadTime >= 1.0)) {
        readNow();
    }

    ImGui::TextUnformatted(U8(L"应用日志"));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", U8(Fmt(L"（来源：{}）", logPath)));
    ImGui::Separator();

    // 控制行：级别过滤 / 刷新 / 自动刷新 / 生成诊断报告 / 打开日志目录。
    ImGui::SetNextItemWidth(150.0f);
    if (ImGui::BeginCombo("##logfilter", U8(ui3::LogFilterModeLabel(lv.filterMode)))) {
        for (int m = ui3::kLogFilterAll; m <= ui3::kLogFilterWarnAndAbove; ++m) {
            if (ImGui::Selectable(U8(ui3::LogFilterModeLabel(m)), m == lv.filterMode)) {
                lv.filterMode = m;
                lv.rows = ui3::BuildDisplayRows(lv.tail, lv.filterMode);  // 纯过滤，不重读文件
                if (lv.followNewest) lv.scrollBottomPending = true;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", U8(L"「仅警告+错误」只显示 WARN/ERROR 级别的日志行"));
    }
    ImGui::SameLine();
    if (ImGui::Button(U8(L"刷新"))) readNow();
    ImGui::SameLine();
    ImGui::Checkbox(U8(L"自动刷新"), &lv.autoRefresh);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", U8(L"勾选后每 1 秒重读一次日志尾部（默认关）"));
    }
    ImGui::SameLine();
    if (ImGui::Button(U8(L"生成诊断报告"))) {
        // 日志报告（系统信息行与兼容诊断同源：ui3::SystemVersionLine()）+
        // 仅在采集降级时附加兼容模式诊断全文（未降级只含日志部分）。
        const std::wstring logPart =
            stm::FormatForReport(lv.tail, 50, ui3::SystemVersionLine());
        const bool degraded = s.snap != nullptr && s.snap->degraded;
        std::wstring compatPart;
        if (degraded) {
            const std::wstring reason = s.snap->degradeReason.empty()
                                            ? std::wstring(L"采集能力受限")
                                            : s.snap->degradeReason;
            compatPart =
                ui3::CompatDiagReportText(kAppVersion, ctx.collect.LastSelfCheckReport(), reason);
        }
        lv.reportText = ui3::AssembleDiagnosticReport(logPart, compatPart, degraded);
        lv.reportOpen = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", U8(L"生成可上报的文本：系统信息 + 最近 50 条错误/警告"
                                   L"（兼容模式时附加自检结果）；仅复制/保存到本机，"
                                   L"不会自动上传"));
    }
    ImGui::SameLine();
    if (ImGui::Button(U8(L"打开日志目录"))) {
        ShellExecuteW(nullptr, L"open", L"explorer.exe", LogDir().c_str(), nullptr, SW_SHOWNORMAL);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", U8(L"在资源管理器中打开日志目录（stm.log 所在位置）"));
    }

    // 读取失败：诚实展示原因与确切路径，绝不显示伪造行。
    if (!lv.tail.error.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ColWarn(), "%s", U8(L"无法读取日志"));
        ImGui::TextWrapped("%s", U8(ui3::LogReadFailureText(lv.tail, logPath)));
        ImGui::Spacing();
        if (ImGui::Button(U8(L"关闭"), ImVec2(120.0f, 0.0f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    // 布局：日志表占大头；报告二级展示打开时预览区约占模态高度 32%，
    // 日志表相应收缩；底部留「跟随最新 + 统计 + 关闭」一行。
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const float frameH = ImGui::GetFrameHeight();
    const float spY = ImGui::GetStyle().ItemSpacing.y;
    const float reportChildH =
        lv.reportOpen ? std::max(lineH * 4.0f, ImGui::GetWindowHeight() * 0.32f) : 0.0f;
    const float reserved =
        frameH + spY * 2.0f + (lv.reportOpen ? lineH + spY * 3.0f + reportChildH : 0.0f);
    const float tableH = std::max(ImGui::GetContentRegionAvail().y - reserved, lineH * 4.0f);

    ImGui::BeginChild("##logrows", ImVec2(0.0f, tableH), ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("##logtbl", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(U8(L"时间"), ImGuiTableColumnFlags_WidthFixed, 104.0f);
        ImGui::TableSetupColumn(U8(L"级别"), ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn(U8(L"模块"), ImGuiTableColumnFlags_WidthFixed, 132.0f);
        ImGui::TableSetupColumn(U8(L"消息"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(lv.rows.size()));
        while (clipper.Step()) {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                const ui3::LogDisplayRow& row = lv.rows[static_cast<size_t>(r)];
                const ui3::LogRowCells cells =
                    row.unparsed
                        ? ui3::UnparsedRowCellTexts(
                              lv.tail.unparsedLines[static_cast<size_t>(row.index)])
                        : ui3::LogRowCellTexts(lv.tail.entries[static_cast<size_t>(row.index)]);
                // 每行着色：ERROR 红 / WARN 黄 / 其他默认（未解析行不猜测）。
                ImVec4 toneCol(-1.0f, -1.0f, -1.0f, -1.0f);
                if (!row.unparsed) {
                    const ui3::LogRowTone tone = ui3::LogRowToneOf(cells.level);
                    if (tone == ui3::LogRowTone::Error) {
                        toneCol = ColFail();
                    } else if (tone == ui3::LogRowTone::Warn) {
                        toneCol = ColWarn();
                    }
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(cells.time));
                ImGui::TableNextColumn();
                if (toneCol.w >= 0.0f) ImGui::PushStyleColor(ImGuiCol_Text, toneCol);
                ImGui::TextUnformatted(U8(cells.level));
                if (toneCol.w >= 0.0f) ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(cells.module));
                ImGui::TableNextColumn();
                if (toneCol.w >= 0.0f) ImGui::PushStyleColor(ImGuiCol_Text, toneCol);
                ImGui::TextUnformatted(U8(cells.message));
                if (row.unparsed && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "%s", U8(L"该行不符合日志格式，按原文显示；它与解析行的原始"
                                 L"交错顺序未保留，故统一排在表尾"));
                }
                if (toneCol.w >= 0.0f) ImGui::PopStyleColor();
            }
        }
        ImGui::EndTable();
    }
    if (lv.scrollBottomPending) {
        ImGui::SetScrollY(ImGui::GetScrollMaxY());
        lv.scrollBottomPending = false;
    }
    ImGui::EndChild();

    // 「生成诊断报告」二级展示：全文预览 + 复制/保存（与兼容诊断同款落盘）。
    if (lv.reportOpen) {
        ImGui::Spacing();
        ImGui::TextUnformatted(U8(L"诊断报告预览（提交 issue 时粘贴；应用不会自动上传）"));
        ImGui::BeginChild("##logreport", ImVec2(0.0f, reportChildH), ImGuiChildFlags_Borders);
        ImGui::TextWrapped("%s", U8(lv.reportText));
        ImGui::EndChild();
        if (ImGui::Button(U8(L"复制到剪贴板"))) {
            ImGui::SetClipboardText(U8(lv.reportText));
            PushToast(Notification::Kind::JobDone, L"诊断报告已复制到剪贴板");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", U8(L"全文以纯文本复制，可直接粘贴到 issue"));
        }
        ImGui::SameLine();
        if (ImGui::Button(U8(L"保存到 logs 目录"))) {
            const std::wstring path = ui3::SaveDiagnosticsFile(lv.reportText);
            PushToast(path.empty() ? Notification::Kind::JobFailed : Notification::Kind::JobDone,
                      path.empty() ? std::wstring(L"保存失败（日志目录不可写）")
                                   : Fmt(L"诊断报告已保存到 {}", path));
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", U8(L"保存为 logs\\diagnostics_时间.txt（本机文件）"));
        }
        ImGui::SameLine();
        if (ImGui::Button(U8(L"收起"))) lv.reportOpen = false;
    }

    // 底部行：跟随最新 + 统计（诚实标注截断/未解析/坏编码）+ 右对齐关闭。
    ImGui::Spacing();
    {
        bool follow = lv.followNewest;
        if (ImGui::Checkbox(U8(L"跟随最新"), &follow)) {
            lv.followNewest = follow;
            if (follow) lv.scrollBottomPending = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", U8(L"勾选后每次刷新自动滚动到最新一条（表尾）"));
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", U8(ui3::LogStatsLine(lv.tail)));
    ImGui::SameLine();
    {
        const ImGuiStyle& st = ImGui::GetStyle();
        const float closeW = ImGui::CalcTextSize(U8(L"关闭")).x + st.FramePadding.x * 2.0f;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                      ImGui::GetWindowWidth() - closeW - st.WindowPadding.x));
        if (ImGui::Button(U8(L"关闭"))) ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

// ===========================================================================
// P1④：一键优化布局 / 重置布局。
// 「一键优化布局」：读主视口 WorkSize（工作区高度）与窗口 DPI（Win32
// GetDpiForWindow；vendored ImGui 1.92.9 无 PlatformMonitorDpi 等 DPI API，
// PlatformHandleRaw 即 HWND），经 ui3::LayoutScaleFromEnv（纯函数，策略见
// PageLayout.h：分辨率因子与 DPI 因子取 max，钳 [1.0, 2.0]）算缩放系数，
// 应用到：PageLayout 各区域定高基准（Scaled）、表格行高与 FramePadding 等
// 样式（DrawShell 每帧自基准重算）、图表块高下限（PerfPage）。
// 「重置布局」：软删除 + 从 config.json 剔除布局键（layoutScale/colOrder/
// colW_*/netcol_*/perfZoom），缩放槽回 1，进程表换代丢弃 ImGui 记忆的旧列宽。
// ===========================================================================
void ApplyOneClickLayout(AppContext& ctx) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float dpi = 96.0f;  // 缺省 100%（PlatformHandleRaw 非 HWND / 调用过早）
    if (const HWND hwnd = static_cast<HWND>(vp->PlatformHandleRaw); hwnd != nullptr) {
        if (const UINT d = GetDpiForWindow(hwnd); d > 0) dpi = static_cast<float>(d);
    }
    const float scale = ui3::LayoutScaleFromEnv(vp->WorkSize.y, dpi);
    ui3::SetLayoutScale(scale);
    ctx.cfg.SetDouble(L"layoutScale", static_cast<double>(scale));
    PushToast(Notification::Kind::JobDone,
              Fmt(L"已按当前分辨率/PPI 优化布局（缩放 ×{:.2f}；可用外观中的"
                  L"「重置布局」还原）",
                  scale));
}

void ResetLayout(AppContext& ctx) {
    ui3::SoftDeleteLayoutKeys(ctx.cfg);
    const int removed = ui3::StripLayoutKeysFromFile(ConfigPath());
    ui3::NotifyLayoutReset();   // V29-P1-1：网络/GPU 列宽缓存按代际失效
    ui3::SetLayoutScale(1.0f);  // 下一帧 DrawShell 按缩放=1 重算样式
    ++Ui().colWidthResetGen;    // 进程表换代：丢弃 ImGui 记忆的列宽/列序
    PushToast(Notification::Kind::JobDone,
              removed < 0 ? std::wstring(L"已重置布局（配置文件格式异常，部分键重启后生效）")
                          : Fmt(L"已重置布局（清除 {} 项布局设置）", removed));
}

void DrawToolbar(AppContext& ctx, const Snapshot& snap) {
    (void)snap;
    const float barH = ImGui::GetFrameHeight() + 6.0f;
    ImGui::BeginChild("##toolbar", ImVec2(0.0f, barH), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar);
    // A1 统一风格改造：工具条全部条目与「暂停采集」同款普通控件，自然流式
    // SameLine 排列（每项实测宽度，无一处绝对偏移、无右缘布局、无下拉菜单
    // ——旧「外观/⋮」菜单与「?」迷你按钮已删除，用户找不到主题/壁纸入口、
    // 不认识「?」的问题由此修复）。窗口级信息（全局热键/权限/统计）保持
    // 在底部状态栏（DrawStatusBar），本条右侧不再放任何东西。
    int interval = static_cast<int>(ctx.cfg.GetInt(L"intervalMs", 1000));
    if (Ui().paused) {
        if (ImGui::Button(U8(L"继续采集"))) {
            if (ctx.collect.Start(static_cast<uint32_t>(interval))) {
                Ui().paused = false;
                PushToast(Notification::Kind::Info, L"已恢复采集");
            } else {
                PushToast(Notification::Kind::JobFailed, L"恢复采集失败");
            }
        }
    } else {
        if (ImGui::Button(U8(L"暂停采集"))) {
            ctx.collect.Stop();
            Ui().paused = true;
            PushToast(Notification::Kind::Info, L"已暂停采集");
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderInt(U8(L"刷新间隔"), &interval, 500, 5000, "%d ms")) {
        ctx.cfg.SetInt(L"intervalMs", interval);
        if (!Ui().paused) ctx.collect.SetInterval(static_cast<uint32_t>(interval));
    }
    if (!ctx.elevated && ops::CanElevate()) {
        ImGui::SameLine();
        if (ImGui::Button(U8(L"以管理员身份重启"))) {
            // ShellExecuteExW(runas) 会在 UAC 对话框上阻塞：放到 ops
            // 工作线程上跑，提示框弹出期间 UI 保持响应（第 4 阶段债务）。
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
    // V19: 「内存加速…」与「清理待机缓存」并排 —— 任意页签下工具条常驻可达，
    // 打开同一 MemCleanup 模态（用户报告性能页内入口不可见的兜底入口）。
    ImGui::SameLine();
    if (ImGui::Button(U8(L"内存加速…"))) RequestConfirmMemCleanup();
    // A1: 「主题…」按钮 -> 外观设置模态（主题三态/恢复默认列宽/自定义壁纸
    // 全部收进模态，见 DrawAppearanceModal）。
    ImGui::SameLine();
    if (ImGui::Button(U8(L"主题…"))) RequestOpenAppearance();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", U8(L"主题（深色/浅色/跟随系统）、表格列宽与自定义壁纸"));
    }
    // P1④: 「一键优化布局」—— 按当前分辨率/PPI 自动缩放布局，免手动拖拽。
    // 外观模态内有同名入口；「重置布局」也在外观模态（低频操作不入工具条）。
    ImGui::SameLine();
    if (ImGui::Button(U8(L"一键优化布局"))) ApplyOneClickLayout(ctx);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", U8(L"按当前分辨率与系统 DPI 自动缩放布局：区域高度、"
                                   L"表格行高、间距与图表高度"));
    }
    // P-C: 「日志」按钮 -> 日志查看器模态（查看应用日志 Error/Warn + 生成
    // 诊断报告；DrawLogViewer 消费一次性标志，与「关于」同款模式）。
    ImGui::SameLine();
    if (ImGui::Button(U8(L"日志"))) RequestOpenLogViewer();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", U8(L"查看应用运行日志（重点：错误/警告），可生成诊断报告"));
    }
    // A1: 「关于」按钮 -> 关于模态（ui::OpenAbout 一次性标志，DrawAboutUi 消费）。
    // 按钮矩形发布进 AboutAutotestState（--autotest about 驱动瞄准真实渲染
    // 的按钮）；每帧重置，只有本帧真实提交过才有效（V19-P2-2 同规则）。
    ImGui::SameLine();
    {
        ui::AboutAutotestState& at = ui::AboutAutotestStateMut();
        at.btnValid = false;
        at.btnHovered = false;
        if (ImGui::Button(U8(L"关于"))) ui::OpenAbout();
        if (ImGui::IsItemVisible()) {
            const ImVec2 mn = ImGui::GetItemRectMin();
            const ImVec2 mx = ImGui::GetItemRectMax();
            at.btnValid = true;
            at.btnMinX = mn.x;
            at.btnMinY = mn.y;
            at.btnMaxX = mx.x;
            at.btnMaxY = mx.y;
            at.btnHovered = ImGui::IsItemHovered();
        }
    }
    ImGui::EndChild();
}

void DrawStatusBar(AppContext& ctx, const Snapshot& snap) {
    ImGui::BeginChild("##statusbar", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar);
    // L1 防抖（用户报告①）：右半信息组改为**固定槽位**布局 —— 槽位宽度只来自
    // 常量上限样本（app/ui3/StatusLayout.h：定宽数字文本 + 最宽字形样本的
    // CalcTextSize），槽位坐标逐帧恒定；数值位数变化只改变槽内文本，绝不再
    // 推挤相邻项（旧实现用当前数值的实测宽，"帧 3.2 ms"->"帧 123.4 ms" 会把
    // 整个右段左右平移）。槽内文本统一左对齐（契约见 StatusLayout.h 头注释）。
    // 窄窗降级与旧版一致：放不下的左段槽（LayoutFlowSlots）与右段条目
    //（LayoutHeaderRight：热键先藏、徽标次之、帧耗时永不藏）直接不绘制。
    const ImGuiStyle& st = ImGui::GetStyle();
    const float contentRightX = ImGui::GetWindowWidth() - st.WindowPadding.x;
    const float sp = st.ItemSpacing.x;
    const float pipeW = ImGui::CalcTextSize("|").x;
    const auto measure = [](const char* u8) { return ImGui::CalcTextSize(u8).x; };

    // ---- 常量槽宽：只依赖编译期常量样本（每帧约 15 次缓存字形查询）----
    const float msNumW = ui::MaxSampleWidth(ui::kMsNumberSamples, measure);
    const float cntNumW = ui::MaxSampleWidth(ui::kCountNumberSamples, measure);
    const float p95PrefixW = ImGui::CalcTextSize(ui::kP95Prefix).x;
    const float msSuffixW = ImGui::CalcTextSize(ui::kMsSuffix).x;
    const float queuePrefixW = ImGui::CalcTextSize(ui::kQueuePrefix).x;
    const float procsPrefixW = ImGui::CalcTextSize(ui::kProcsPrefix).x;
    const float csvTextW = ImGui::CalcTextSize(U8(L"● 记录 CSV")).x;
    // 左段槽：[|采集 p95][|操作队列][|进程 N][|● 记录 CSV]（CSV 槽仅记录中绘制）。
    const float slotW[4] = {
        ui::PipeSlotWidth(pipeW, sp, ui::TextSlotWidth(p95PrefixW, msNumW, msSuffixW)),
        ui::PipeSlotWidth(pipeW, sp, ui::AltSlotWidth(queuePrefixW, cntNumW,
                                                      ImGui::CalcTextSize(ui::kQueueIdleText).x)),
        ui::PipeSlotWidth(pipeW, sp, ui::TextSlotWidth(procsPrefixW, cntNumW, 0.0f)),
        ui::PipeSlotWidth(pipeW, sp, csvTextW),
    };

    // P1①（用户报告「完整采集模式…与相邻槽位叠在一起」）根因：锚点绘制后用
    // GetCursorPosX() 采集槽位起点 —— ImGui 的 ItemSize 在条目绘制完成后把
    // CursorPos.x 重置回行首（imgui.cpp），该值恒为 ≈WindowPadding.x，与锚点
    // 真实末端无关，定宽槽全部压在锚点文本上（降级原因越长叠得越明显）。
    // 修复契约（StatusLayout.h）：
    //   1. 锚点末端 = 起点 + 锚点文本实测宽（AnchorEndX），绝不读回光标；
    //   2. 降级原因按可用宽截断加「…」（EllipsizeTextUtf8），截断预算给
    //      首个槽位留位（内容右缘 - 起点 - 首槽宽 - 间距）；悬停 tooltip
    //      显示完整原因（完整信息另有兼容模式说明模态）；
    //   3. 无降级时只有紧凑「完整模式」徽标，原因槽消失不占位；
    //   4. 右段装箱的 leftFlowEndX 用同一套显式推算值（不再读光标），
    //      任何原因长度下左段与右段/槽位互不重叠。
    const float anchorStartX = ImGui::GetCursorPosX();  // 行首（窗口内边距处）
    float leftEndX = anchorStartX;  // 左段流程末端（含锚点/暂停/各槽，喂右段）
    char anchorBuf[512];
    {   // 锚点段（必显）：采集模式（降级时给原因；维护轮 10：可点击打开说明模态）。
        if (snap.degraded) {
            // 截断预算：内容右缘 - 锚点起点 - 首槽宽 - 间距（首槽保底可见）。
            float budget = contentRightX - anchorStartX - slotW[0] - sp;
            // 极窄窗口保底：至少保住「兼容模式：…」标签（放不下的槽交给
            // LayoutFlowSlots 依约隐藏，绝不因预算为负而丢失模式指示）。
            const float minBudget =
                ImGui::CalcTextSize(U8(L"兼容模式：")).x + ImGui::CalcTextSize("\xE2\x80\xA6").x;
            if (budget < minBudget) budget = minBudget;
            const char* full =
                U8(Fmt(L"兼容模式：{}",
                       snap.degradeReason.empty() ? std::wstring(L"采集能力受限")
                                                  : snap.degradeReason));
            ui::EllipsizeTextUtf8(full, budget, measure, anchorBuf,
                                  sizeof(anchorBuf));
            ImGui::TextColored(ColWarn(), "%s", anchorBuf);
            const float anchorEnd =
                ui::AnchorEndX(anchorStartX, ImGui::CalcTextSize(anchorBuf).x);
            leftEndX = anchorEnd;
            // 维护轮 10：可点击 -> 兼容模式说明模态（每帧渲染，见 RequestOpenCompatDiag）。
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                // P1①：tooltip 带完整未截断原因（超长自动换行）。
                ImGui::SetTooltip("%s  ▸%s", U8(L"点击查看兼容模式说明与逐项自检结果"),
                                  full);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) RequestOpenCompatDiag();
            }
        } else {
            ImGui::TextDisabled("%s", U8(L"完整模式"));
            leftEndX =
                ui::AnchorEndX(anchorStartX, ImGui::CalcTextSize(U8(L"完整模式")).x);
        }
    }
    // P2-3（F2 评审）：暂停采集需要全局指示（宽度恒定）。显式定位在锚点
    // 末端 + 间距（SameLine 偏移与槽位坐标同系），此后槽位起点同步右移。
    if (Ui().paused) {
        ImGui::SameLine(leftEndX + sp);
        ImGui::TextColored(ColWarn(), "%s", U8(L"[已暂停]"));
        leftEndX += sp + ImGui::CalcTextSize(U8(L"[已暂停]")).x;
    }

    // 锚点之后布固定槽位（降级原因文本只在模式切换时变化 —— 状态变化而非
    // 逐帧数据变化，槽起点随之一次性移动是可接受的）。
    const float slotsStartX = leftEndX + sp;
    float slotX[4] = {};
    const int firstBadSlot = ui::LayoutFlowSlots(slotsStartX, sp, contentRightX, slotW, 4, slotX);
    const auto drawSlotText = [pipeW, sp](float x, const char* text) {
        ImGui::SameLine(x);
        ImGui::TextDisabled("|");
        ImGui::SameLine(x + pipeW + sp);
        ImGui::TextUnformatted(text);
    };
    {   // 槽 0：采集 p95（定宽数字文本，钳制见 FormatSlotMs）。
        char p95Buf[48];
        ui::FormatSlotMs(p95Buf, ui::kP95Prefix, ctx.collect.TickP95Ms(), ui::kMsSuffix);
        if (0 < firstBadSlot) {
            drawSlotText(slotX[0], p95Buf);
            leftEndX = slotX[0] + slotW[0];
        }
    }
    {   // 槽 1：操作队列（忙=计数 / 闲=「操作队列空闲」，槽宽取两者上限）。
        const size_t pending = ctx.jobs.PendingCount();
        char queueBuf[48];
        if (pending > 0) {
            ui::FormatSlotCount(queueBuf, ui::kQueuePrefix, pending);
        } else {
            snprintf(queueBuf, sizeof(queueBuf), "%s", ui::kQueueIdleText);
        }
        if (1 < firstBadSlot) {
            ImGui::SameLine(slotX[1]);
            ImGui::TextDisabled("|");
            ImGui::SameLine(slotX[1] + pipeW + sp);
            if (pending > 0) {
                ImGui::TextColored(ColWarn(), "%s", queueBuf);
            } else {
                ImGui::TextDisabled("%s", queueBuf);
            }
            leftEndX = slotX[1] + slotW[1];
        }
    }
    {   // 槽 2：进程数。
        char procsBuf[48];
        ui::FormatSlotCount(procsBuf, ui::kProcsPrefix,
                            static_cast<unsigned long long>(snap.procs.size()));
        if (2 < firstBadSlot) {
            drawSlotText(slotX[2], procsBuf);
            leftEndX = slotX[2] + slotW[2];
        }
    }
    // 槽 3：CSV 记录状态（记录中才绘制；悬停显示文件路径）。
    bool csvDrawn = false;
    if (ui3::SharedPerfCsv().Active() && 3 < firstBadSlot) {
        drawSlotText(slotX[3], U8(L"● 记录 CSV"));
        leftEndX = slotX[3] + slotW[3];
        csvDrawn = true;
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", U8(ui3::SharedPerfCsv().Path()));
        }
    }
    // 自由文本（最后一条通知）保持文档流尾：它在最后一个可见槽之后，
    // 宽度变化只向右侧伸展，不再有可被推挤的相邻项。
    if (!Ui().lastNote.empty()) {
        const int lastDrawn = csvDrawn ? 3 : (2 < firstBadSlot ? 2 : firstBadSlot - 1);
        if (lastDrawn >= 0) {
            const float noteX = slotX[lastDrawn] + slotW[lastDrawn] + sp;
            const char* note = U8(Ui().lastNote);
            if (ui::FlowSegmentFits(noteX, contentRightX, ImGui::CalcTextSize(note).x)) {
                ImGui::SameLine(noteX);
                ImGui::TextColored(ImVec4(0.60f, 0.62f, 0.68f, 1.0f), "%s", note);
                leftEndX = noteX + ImGui::CalcTextSize(note).x;
            }
        }
    }
    // --- 右段：外观类与自身开销 [全局热键][权限徽标][帧耗时] ------------------
    // 槽宽全部为常量上限（热键标签恒定；徽标取两种文案较宽者；帧耗时取
    // 数字样本上限）——LayoutHeaderRight 的坐标因此逐帧恒定。
    {
        const float leftEnd = leftEndX;  // 左段流程末端（P1①：显式推算，不读光标）
        const float frameH = ImGui::GetFrameHeight();
        const float hotkeyW = frameH + st.ItemInnerSpacing.x +
                              ImGui::CalcTextSize(U8(L"全局热键 Ctrl+Alt+M")).x;
        const float badgeTextW = std::max(ImGui::CalcTextSize(U8(L"管理员")).x,
                                          ImGui::CalcTextSize(U8(L"普通权限")).x);
        const float badgeW = ui::PipeSlotWidth(pipeW, sp, badgeTextW);
        const float frameW = ui::PipeSlotWidth(
            pipeW, sp, ui::TextSlotWidth(ImGui::CalcTextSize(ui::kFrameMsPrefix).x, msNumW,
                                         msSuffixW));
        // 数组顺序 == 屏幕从左到右；priority 大者先藏：热键(2) → 徽标(1)，
        // 帧耗时(0) 永不隐藏。
        const ui::HeaderItem kRightItems[3] = {{hotkeyW, 2}, {badgeW, 1}, {frameW, 0}};
        ui::HeaderPlacement place[3];
        ui::LayoutHeaderRight(contentRightX, leftEnd, sp, kRightItems, 3, place);

        if (place[0].visible) {
            // F4#10: 全局热键开关（原工具条成员，Bug1 移入状态栏右段）。
            ImGui::SameLine(place[0].x);
            bool hotkey = ctx.cfg.GetBool(L"hotkeyEnabled", false);
            if (ImGui::Checkbox(U8(L"全局热键 Ctrl+Alt+M"), &hotkey)) {
                ApplyHotkeyEnabled(ctx, hotkey);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s",
                                  U8(L"注册 Ctrl+Alt+M 全局热键：任意前台应用下呼出/隐藏本工具"
                                     L"（仅运行期有效，不写注册表）"));
            }
        }
        if (place[1].visible) {
            ImGui::SameLine(place[1].x);
            ImGui::TextDisabled("|");
            ImGui::SameLine(place[1].x + pipeW + sp);
            if (ctx.elevated) {
                ImGui::TextColored(ColInfo(), "%s", U8(L"管理员"));
            } else {
                ImGui::TextDisabled("%s", U8(L"普通权限"));
            }
        }
        if (place[2].visible) {
            char frameMsBuf[48];
            ui::FormatSlotMs(frameMsBuf, ui::kFrameMsPrefix, ctx.frameMs, ui::kMsSuffix);
            ImGui::SameLine(place[2].x);
            ImGui::TextDisabled("|");
            ImGui::SameLine(place[2].x + pipeW + sp);
            ImGui::TextUnformatted(frameMsBuf);
        }
    }
    ImGui::EndChild();
}

}  // namespace

// ===========================================================================
// 公共入口（Pages.h 契约）。
// ===========================================================================

void RegisterPages(AppContext& ctx) {
    ctx.pages.push_back(std::make_unique<ProcessesPage>());
    ctx.pages.push_back(std::make_unique<PerfPage>());
    ui3::RegisterPhase3Pages(ctx);  // phase-3: 网络/启动项/服务/驱动/传感器
    ui3::RegisterGcPages(ctx);      // F4: 崩溃记录/窗口
}

void BindAppContext(std::shared_ptr<AppContext> ctx) {
    // main 在帧循环前在此注册持有句柄；ops 任务 lambda 捕获它，
    // 使在途任务活过拆除（V7-P1-3）。
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
        // 最小化窗口报告 -32000 坐标；持久化它们会把窗口恢复到
        // 屏幕之外（终审 V11-P2-5）——改为保留之前的矩形。
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
    // 整个外壳 + 所有页面每帧只读一次快照。
    Ui().snap = ctx.collect.Store().Get();
    AppendHistory(*Ui().snap);
    DrainNotifications(ctx);
    ui3::AlertTick(ctx);  // 第 3 阶段：阈值告警监视器（默认关）

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
    // P1④：布局缩放 —— 会话首帧从 cfg 恢复（「一键优化布局」写入），
    // 之后每帧自 Theme 基准值重算样式（绝不累计；基准与 Theme.cpp
    // ApplyLayout 同源：FramePadding (8,4)、ItemSpacing (8,6)）。缩放=1 时
    // 写入的值与基准逐位一致（Scaled 恒等），布局与旧版完全相同。
    {
        static bool scaleLoaded = false;
        if (!scaleLoaded) {
            scaleLoaded = true;
            ui3::SetLayoutScale(
                static_cast<float>(ctx.cfg.GetDouble(L"layoutScale", 1.0)));
        }
        const float s = ui3::LayoutScale();
        ImGuiStyle& st = ImGui::GetStyle();
        st.FramePadding = ImVec2(ui3::Scaled(8.0f), ui3::Scaled(4.0f));
        st.ItemSpacing = ImVec2(ui3::Scaled(8.0f), ui3::Scaled(6.0f));
        st.ItemInnerSpacing = ImVec2(ui3::Scaled(6.0f), ui3::Scaled(6.0f));
        st.CellPadding = ImVec2(ui3::Scaled(4.0f), ui3::Scaled(2.0f));
        st.IndentSpacing = ui3::Scaled(20.0f);
        (void)s;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
    // R-Fix Bug2 根因修复：壁纸绘制在视口背景绘制列表里（main.cpp，位于一切
    // 普通窗口之下），而「##approot」是覆盖整个视口的普通窗口 —— 主题给它
    // 不透明 WindowBg，工具条/页面区的子窗口还有不透明 ChildBg，壁纸被完全
    // 盖住，用户只见纯色。壁纸激活期间对这一个窗口推透明背景（弹窗/toast/
    // 确认框/离屏预览窗都绘制在本 End 之后，保持各自颜色不变）；可读性由
    // 壁纸自身的黑色遮罩（wallpaperMask）负责。
    const bool wallpaperUnder = ui::WallpaperActive();
    if (wallpaperUnder) {
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    }
    ImGui::Begin("##approot", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    DrawToolbar(ctx, *Ui().snap);

    const float statusH = ImGui::GetTextLineHeightWithSpacing() + 6.0f;
    ImGui::BeginChild("##pagearea", ImVec2(0.0f, -statusH), ImGuiChildFlags_None);
    if (ImGui::BeginTabBar("##pages")) {
        // P1-1（F2 评审）：会话恢复写入了 activePage 但 TabBar 从未
        // 消费它。（重新）注册后的首个渲染帧为恢复的索引
        // 布设一次性 SetSelected。
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
    if (wallpaperUnder) ImGui::PopStyleColor(2);  // ##approot 专用透明背景（见上）
    ImGui::PopStyleVar();

    ui3::DrawSmokeAllPages(ctx);  // 第 3 阶段：--smoke 在屏外执行每个页面
    ui3::DrawGcModals(ctx);       // F4#3: 宿主服务共享模态    // H-A(Phase-6): --smoke 离屏绘制「自定义壁纸」控件组（滑条/状态行/性能提示
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
    // A1 统一风格改造：关于模态与外观设置模态与确认框同层（隐式窗口级，
    // OpenPopup/BeginPopupModal 成对出现在 DrawAboutUi / DrawAppearanceModal
    // 内部，与 DrawConfirmDialogs 同一渲染模式）。
    ui::DrawAboutUi();
    DrawAppearanceModal(ctx);
    // 维护轮 10：兼容模式说明模态（状态栏「兼容模式：<原因>」点击打开；
    // 与 ##confirm/##about/##appearance 同层、同一每帧渲染模式）。
    DrawCompatDiagModal(ctx);
    // P-C：日志查看器模态（工具条「日志」按钮；同一每帧渲染模式与层级）。
    DrawLogViewer(ctx);
}

// --- --autotest dialogclick (V14) -------------------------------------------
// 经与行右键菜单完全相同的 RequestConfirmKill() 路径布设终止确认；
// DrawConfirmDialogs 记录的状态让 app/AutotestDialog.cpp 中的驱动
// 能把合成鼠标事件瞄准真实渲染的按钮。

void ArmKillConfirmForAutotest(uint32_t pid, uint64_t createTime, const wchar_t* name) {
    ProcInfo p;
    p.key = ProcKey{pid, createTime};
    p.name = name != nullptr ? name : L"";
    RequestConfirmKill(p);
}

const DialogAutotestState& DialogAutotestStateForAutotest() { return g_dialogAutotest; }

}  // namespace stm
