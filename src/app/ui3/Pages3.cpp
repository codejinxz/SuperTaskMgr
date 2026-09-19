// 第 3 阶段 UI 扩展：网络/启动项/服务/驱动/传感器标签页。
// plus the optional threshold alert watcher (docs/phase/01_架构设计文档.md §8).
// 继承自 app/ui/Pages.cpp 的设计规则：
//  - 每条面向用户的中文都经 ui::U8()（UTF-8 文本缓存）
//  - 所有数据经 ops 任务队列抓取进每页缓存（ui3::AsyncFetch），
//    带各页最小刷新间隔（网络 2s / 服务+驱动 5s / 传感器 10s）；
//    UI 线程绝不阻塞，
//    pages render a "加载中…" state instead
//  - 破坏性操作打开两段式确认对话框（取消按钮在前且持有键盘焦点，
//    动作按钮移出键盘导航），随后
//    经任务队列提交并经通知队列上报；
//  - 不可得的值渲染为 "—"；未知/错误状态渲染诚实的
//    中文说明，绝不伪造数据。
// 增量式集成点只位于 app/ui/Pages.cpp 与 app/main.cpp。
#include "app/ui3/Pages3.h"
#include "app/ui3/AsyncFetch.h"
#include "app/ui3/GcPages.h"    // F4#3: 宿主服务模态
#include "app/ui3/JumpState.h"  // F4#3: 跨页跳转槽
#include "app/ui3/PageHelpers.h"
#include "app/ui/Pages.h"
#include "app/ui/ConfirmAction.h"
#include "app/ui/UiText.h"
#include "collect/LhmSource.h"
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

// 共享槽位（定义在文件底部；匿名命名空间内使用）。
std::shared_ptr<AppContext>& P3Slot();
BalloonSink& SinkSlot();

namespace {

using ui::U8;

// ===========================================================================
// 小型共享辅助（本编译单元内部使用）。
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

// ASCII 大小写不敏感子串匹配（与进程页同语义）。
bool ContainsLower(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return true;
    return LowerCopy(hay).find(needle) != std::wstring::npos;
}

// P2 (2026-09-18, "CPU 温度信息增强"): refine the shared LHM classifier for
// CPU 组保真。LhmGroupOf（PageHelpers.h，已冻结）对 Intel 风格路径
//（".../ Temperatures / CPU Core #1［LHM］") 的 "cpu" 匹配良好，但 AMD 风格
// 的每核温度传感器不带 "cpu" 记号（".../ Temperatures /
// Core (Tctl/Tdie)"、"CCD1 (Tdie)"），于是落入 Other——这些读数
// 从 CPU 组消失。传感器页本地的保守修复：仅当节点路径同时点名
// TEMPERATURE 段与每核/整包记号时，基础分类为 "Other" 的行才升入 CPU。
// 其余一律保持 Other，因为温度闸门未过（"Voltages / VCore"、
// "Clocks / Core #1"、DIMM 温度……绝不混入 CPU）。
//
LhmGroup LhmGroupOfCpuTemps(const std::wstring& lhmLabel) {
    const LhmGroup base = LhmGroupOf(lhmLabel);
    if (base != LhmGroup::Other) return base;
    const std::wstring s = LowerCopy(lhmLabel);
    if (s.find(L"temperature") == std::wstring::npos) return LhmGroup::Other;
    const bool coreLike = s.find(L"core") != std::wstring::npos ||
                          s.find(L"package") != std::wstring::npos ||
                          s.find(L"tctl") != std::wstring::npos ||
                          s.find(L"tdie") != std::wstring::npos ||
                          s.find(L"ccd") != std::wstring::npos;
    return coreLike ? LhmGroup::Cpu : LhmGroup::Other;
}

std::wstring Truncate(const std::wstring& s, size_t maxChars) {
    if (s.size() <= maxChars) return s;
    return s.substr(0, maxChars) + L"…";
}

// 首次 Draw 调用以及上一帧未绘制本页（切换标签）时为 true。
// kNeverDrawn 哨兵：帧 0 是合法 id。
constexpr uint64_t kNeverDrawn = ~0ull;
bool BecameActive(uint64_t& lastFrame) {
    const uint64_t f = ImGui::GetFrameCount();
    const bool active = lastFrame == kNeverDrawn || f > lastFrame + 1;
    lastFrame = f;
    return active;
}

void LowerFilter(const std::string& utf8, std::wstring& out) { out = LowerCopy(Utf8ToWide(utf8)); }

// P1-4（F2 评审）：ShellExecuteExW(runas) 会阻塞到 UAC 对话框关闭——
// 它绝不能在 UI 线程上运行。页内每个提权按钮都经 ops 工作线程；
// 取消/失败投递 toast（与工具栏按钮一致）。
void RequestElevateRestart(AppContext& ctx) {
    SaveSessionFromCtx(ctx, nullptr);
    std::shared_ptr<AppContext> app = LiveP3Ctx();
    if (!app) return;
    if (app->jobs.Submit([app] {
            if (ops::RelaunchAsAdmin(L"--relaunched")) {
                app->wantExit = true;
            } else {
                PushNote(*app, Notification::Kind::JobFailed, L"提权重启失败或已取消");
            }
        }) == 0) {
        PushNote(ctx, Notification::Kind::JobFailed,
                 L"操作队列未运行，提权重启未执行（应用可能正在退出）");
    }
}

// Shared "以管理员身份重启" button (same flow as the shell toolbar button).
void DrawElevateButton(AppContext& ctx) {
    if (ctx.elevated || !ops::CanElevate()) return;
    if (ImGui::Button(U8(L"以管理员身份重启"))) {
        RequestElevateRestart(ctx);
    }
}

void DrawLoading() {
    ImGui::TextColored(ColMuted(), "%s", U8(L"加载中…"));
}

// 错误行 + 重试按钮；按钮按下的那一帧置 *retry
//（调用方随后发起强制抓取）。
void DrawLoadError(const std::wstring& err, bool* retry) {
    ImGui::TextColored(ColFail(), "%s",
                       U8(Fmt(L"加载失败：{}", err.empty() ? std::wstring(L"未知错误") : err)));
    if (ImGui::Button(U8(L"重试"))) *retry = true;
}

// pid -> 当前采集快照中的进程名（按 pid 升序；
// 仅名称提示，展示用途无需身份校验）。
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
// NetworkPage：TCP/UDP 端点表（只读页，无危险操作）。
// 数据：经任务队列的 collect::SnapshotConnections，抓取间隔至少 2s。
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
        if (!etwLoaded_) {  // 一次性：采集器读回值优先于配置
            etwLoaded_ = true;
            const bool cfgWants = ctx.cfg.GetBool(L"netEtw", false);
            etw_ = cfgWants && ctx.collect.NetEtwEnabled();  // P1-1：诚实的初始状态
            if (etw_ != cfgWants) {
                ctx.cfg.SetBool(L"netEtw", etw_);  // 持久化真实状态
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
        // ETW 开关状态机（P1-1 + P2-1）：复选框只是请求。
        // Start/Stop 在 ops 任务队列上运行，开关在途期间禁用，
        // 真实状态由采集器读回（NetEtwEnabled）决定——不一致时
        // 把复选框与 cfg 一并回滚，
        // 并投递 JobFailed toast。
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
                ctx.cfg.SetBool(L"netEtw", actual);  // 持久化真实值 / 回滚
                if (actual != etwDesired_) {
                    // P2-7（F2 评审）：StartTrace 失败可能不止因为提权
                    //（会话上限、策略）；如实说明原因范围。
                    PushNote(ctx, Notification::Kind::JobFailed,
                             etwDesired_
                                 ? L"按进程流量（ETW）开启失败（原因详见日志：常见为缺少管理员"
                                   L"权限或 ETW 会话数已达上限/被策略阻止）"
                                 : L"按进程流量（ETW）关闭失败（原因详见日志）");
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
        ImGui::BeginDisabled(etwToggle_ != nullptr);  // 在途：防抖动避免重复切换
        if (ImGui::Checkbox(U8(L"按进程流量（ETW）"), &etw_)) {
            const bool desired = etw_;
            etwDesired_ = desired;
            etwToggle_ = std::make_shared<EtwToggle>();
            std::shared_ptr<AppContext> app = LiveP3Ctx();
            std::shared_ptr<EtwToggle> toggle = etwToggle_;
            if (app) {
                if (app->jobs.Submit([app, toggle, desired] {
                        app->collect.SetNetEtwEnabled(desired);
                        const bool actual = app->collect.NetEtwEnabled();  // 读回值 = 真实状态
                        std::lock_guard<std::mutex> lock(toggle->mu);
                        toggle->actual = actual;
                        toggle->ready = true;
                    }) == 0) {
                    etw_ = !desired;  // 队列已关：回滚复选框
                    etwToggle_.reset();
                }
            } else {
                etw_ = !desired;  // 拆除竞争：回滚复选框
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

    // 过滤后的行索引；仅当数据或过滤条件变化时重建。
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
                ImGui::PushID(static_cast<int>(r));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                // F4#3: 行级右键菜单 —— 跳转到进程 / 查看宿主服务。
                if (ImGui::Selectable("##connrow", false,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                }
                if (ImGui::BeginPopupContextItem("##rowctx")) {
                    std::wstring name;
                    std::shared_ptr<const Snapshot> snapNow =
                        LiveP3Ctx() ? LiveP3Ctx()->collect.Store().Get() : nullptr;
                    if (snapNow != nullptr) {
                        if (const ProcInfo* p = FindPid(*snapNow, c.pid)) name = p->name;
                    }
                    if (ImGui::MenuItem(U8(L"跳转到进程"), nullptr, false, c.pid != 0)) {
                        RequestJumpToProcess(c.pid);
                    }
                    if (ImGui::MenuItem(U8(L"查看宿主服务…"), nullptr, false, c.pid != 0)) {
                        RequestHostServicesModal(c.pid, name);
                    }
                    if (ImGui::MenuItem(U8(L"复制本地端点"))) {
                        ImGui::SetClipboardText(
                            U8(ConnEndpoint(c.localAddr, c.localPort)));
                    }
                    ImGui::EndPopup();
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(U8(ProtoLabel(c.proto)));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(ConnEndpoint(c.localAddr, c.localPort)));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(ConnEndpoint(c.remoteAddr, c.remotePort)));
                ImGui::TableNextColumn();
                if (c.proto == ConnProto::Tcp4 || c.proto == ConnProto::Tcp6) {
                    ImGui::TextUnformatted(U8(UiTcpStateLabel(c.state)));
                } else {
                    ImGui::TextDisabled("%s", U8(L"—"));  // UDP 没有状态
                }
                ImGui::TableNextColumn();
                ImGui::Text("%u", c.pid);
                ImGui::TableNextColumn();
                std::wstring name = L"—";  // §8：未知持有者渲染为破折号
                if (c.pid == 0) {
                    name = L"系统";  // contract: pid 0 = bound by kernel/System
                } else if (const ProcInfo* p = FindPid(*snap, c.pid)) {
                    name = p->name;
                }
                ImGui::TextUnformatted(U8(name));
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    // 在途 ETW 开关：由 ops 任务写入，UI 线程轮询。
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
    std::shared_ptr<EtwToggle> etwToggle_;  // null = 没有开关在途
    bool etwDesired_ = false;
    bool etw_ = false;
    bool etwLoaded_ = false;
};

// ===========================================================================
// StartupPage：4 来源启动项；启用/禁用并附备份说明。
// 数据：经任务队列的 ops::EnumStartupItems；标签激活 + 手动刷新。
// ===========================================================================

class StartupPage final : public IPage {
public:
    const wchar_t* Id() const override { return L"startup"; }
    const wchar_t* Title() const override { return L"启动项"; }

    void Draw(AppContext& ctx) override {
        const bool becameActive = BecameActive(lastFrame_);
        fetch_.MaybeFetch(Produce, becameActive);  // 切页刷新 per spec
        if (refetchPending_ && !fetch_.Busy()) {   // 操作落地后刷新
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

    // ---- 确认 + 提交 --------------------------------------------------------
    struct PendOp {
        bool active = false;
        bool openRequested = false;
        bool enable = false;
        ops::StartupItem item;
    };

    void RequestConfirm(const ops::StartupItem& it, bool enable) {
        pend_ = PendOp{};
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
        if (!ImGui::IsPopupOpen(kPopup)) {  // 点击外部被关闭
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
        // Bug F1 修复（2026-09）：出现帧上只聚焦取消按钮一次。
        // 旧的每帧 SetKeyboardFocusHere(0) 每帧重复提交导航移动；
        // 一旦生效，NavMoveRequestApplyResult() 会清除 ActiveId
        // of the mouse-held confirm button, so 确认启用/确认禁用 could never receive
        // 点击（按下被捕获、抬起被静默丢弃）。
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(0);
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
        (void)ctx;
        ui::ConfirmRequest req;
        req.kind = ui::ConfirmKind::StartupToggle;
        req.startupEnable = enable;
        req.startupItem = item;
        // ExecuteConfirmedAction 总是经 notes -> toast 管线上报结果
        //（包括被拒的提交），因此点击绝不会是空操作。
        ui::ExecuteConfirmedAction(LiveP3Ctx(), req);
        refetchPending_ = true;  // 操作落地后重建列表
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
// ServicePage：SCM 服务；启动/停止并警告运行中的依赖者。
// 数据：经任务队列的 ops::EnumServices，抓取至少间隔 5s + 手动刷新。
// ===========================================================================

ImVec4 ServiceStateColor(uint32_t state) {
    switch (state) {
        case 4: return ColDone();                          // 运行中
        case 2: case 3: case 5: case 6: return ColWarn();  // 过渡中状态
        case 7: return ColInfo();                          // 已暂停
        case 1: return ColMuted();                         // 已停止
        default: return ColFail();
    }
}

class ServicePage final : public IPage {
public:
    const wchar_t* Id() const override { return L"services"; }
    const wchar_t* Title() const override { return L"服务"; }

    void Draw(AppContext& ctx) override {
        BecameActive(lastFrame_);
        fetch_.MaybeFetch(Produce, false);  // 5s 规则 + 手动刷新
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
        // P2-5：拦下注定失败的条目。SERVICE_DISABLED 服务无法启动
        //（契约没有启用操作）；接受控制集缺少 SERVICE_ACCEPT_STOP
        // 的服务无法停止。
        const bool canStart = sel != nullptr && sel->startType != 4;  // != SERVICE_DISABLED（非禁用）
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
        // P2-5：注定失败的条目保持可见但禁用，
        // 原因写在标签里（P2-5）/ 由提权闸门变灰。
        ImGui::BeginDisabled(!allowed);
        if (s.state != 4 && s.state != 2) {  // 非运行 / 非启动挂起
            if (s.startType == 4) {          // SERVICE_DISABLED（禁用）
                ImGui::MenuItem(U8(L"启动（服务已禁用）"), nullptr, false, false);
            } else if (ImGui::MenuItem(U8(L"启动…"), nullptr, false, allowed)) {
                RequestConfirm(s, true);
            }
        }
        if (s.state == 4 || s.state == 7) {  // 运行中 / 已暂停
            if (!s.canStop) {
                ImGui::MenuItem(U8(L"停止（不接受停止控制）"), nullptr, false, false);
            } else if (ImGui::MenuItem(U8(L"停止…"), nullptr, false, allowed)) {
                RequestConfirm(s, false);
            }
        }
        ImGui::EndDisabled();
        if (ImGui::MenuItem(U8(L"复制服务名"))) ImGui::SetClipboardText(U8(s.name));
        // F4#3: 服务页反向跳转进程（s.pid==0 表示未运行或驱动服务）。
        if (ImGui::MenuItem(U8(L"跳转到进程"), nullptr, false, s.pid != 0)) {
            RequestJumpToProcess(s.pid);
        }
    }

    // ---- 确认 + 提交 --------------------------------------------------------
    // 运行依赖者规划异步到达（与杀树规划相同）。
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
        std::shared_ptr<DepPlan> plan;  // 仅停止用
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
                // P2（V14）：被拒的提交不能让对话框悬着
                // on "正在查询依赖…" — mark the plan ready and toast the failure.
                if (app->jobs.Submit([app, plan, name = s.name] {
                        std::vector<std::wstring> deps = ops::GetDependentServices(name);
                        std::lock_guard<std::mutex> lock(plan->mu);
                        plan->names = std::move(deps);
                        plan->ready = true;
                    }) == 0) {
                    std::lock_guard<std::mutex> lock(plan->mu);
                    plan->ready = true;
                    PushNote(*app, Notification::Kind::JobFailed,
                             L"操作队列未运行，依赖服务查询未提交（应用可能正在退出）");
                }
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
        // Bug F1 修复：出现帧上只聚焦取消按钮一次（每帧的
        // SetKeyboardFocusHere 会在按下与抬起之间抢走确认按钮的
        // 鼠标 ActiveId——见 StartupPage）。
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(0);
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
        if (app->jobs.Submit([app, s, start, elevated] {
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
            }) == 0) {
            // 队列未运行（拆除中）：绝不静默失败。
            PushNote(*app, Notification::Kind::JobFailed,
                     L"操作队列未运行，指令未能提交（应用可能正在退出）");
        }
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
// DriverPage：已加载内核驱动；按需（选中时）检查签名。
// 数据：经任务队列的 ops::EnumDrivers。24H2+ 未提权时退化为
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
        if (!res->err.empty()) {  // P2-3：部分数据横幅，与其他页面一致
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
        std::atomic<int> state{-1};  // -1 处理中，否则为 int 形式的 ops::SigState
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
            default:  // -1：任务仍在途
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
                    EnsureSig(d.path);  // 选中触发的签名检查
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
// SensorPage（W2 重设计）：诚实的四态读数分入纵向全宽分区
//（P1-3：任何窗口宽度都不裁切），分组可见性可配置，
// 可选显示 toggles persisted in the config, and an optional LibreHardwareMonitor
//（LHM）数据源——默认关闭、仅限本机，经 ops 队列探测
//（P1-4 契约：UI 线程不做 UAC/网络工作）。
// 数据：经任务队列的 collect::ReadSensors，至少 10s；LHM 轮询同拍。
// ===========================================================================

class SensorPage final : public IPage {
public:
    const wchar_t* Id() const override { return L"sensors"; }
    const wchar_t* Title() const override { return L"传感器"; }

    void Draw(AppContext& ctx) override {
        const bool becameActive = BecameActive(lastFrame_);
        LoadPrefsOnce(ctx);
        fetch_.MaybeFetch(Produce, becameActive);  // >=10s 节拍 + 手动刷新
        if (lhmOn_) lhmFetch_.MaybeFetch(LhmProduce, becameActive);  // 同一节拍
        PollLhmProbe(ctx);
        std::shared_ptr<const Result> res = fetch_.Peek();

        DrawToolbar(ctx, res.get());
        DrawVisibilityRow(ctx);
        DrawLhmSection(ctx);
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
        const std::vector<SensorReading> lhm = CurrentLhmRows();
        // 组渲染分发：详细模式逐条渲染全部读数（含完整标签），精简模式为现有概要。
        const auto readings = [&](const std::vector<SensorReading>& items) {
            if (detailMode_) {
                DrawReadingsDetailed(ctx, items);
            } else {
                DrawReadings(ctx, items);
            }
        };

        // Vertical full-width groups; each 可选显示 via cfg (P2 requirement).
        if (SensorGroupVisible(ctx.cfg, SensorGroup::Cpu)) {
            const std::vector<SensorReading> cpuLhm = FilterLhm(lhm, LhmGroup::Cpu);
            BeginGroup("##grp_cpu", L"CPU",
                       snap.cpu.size() + snap.cpuCores.size() + cpuLhm.size());
            readings(snap.cpu);
            readings(cpuLhm);
            // P2 诚实文案（用户反馈"CPU 温度太少不准确"）：每核 DTS 温度必须内核
            // 驱动才能读，本应用不内置驱动；指明 LHM 外接数据源这条出路。
            ImGui::TextDisabled("%s", U8(L"CPU 每核 DTS 温度需要内核驱动读取（本应用不内置）；"
                                        L"安装并运行 LibreHardwareMonitor 后，上面会自动显示其提供的每核温度。"));
            DrawCoreTable(ctx, snap.cpuCores);
            EndGroup();
        }
        if (SensorGroupVisible(ctx.cfg, SensorGroup::Gpu)) {
            BeginGroup("##grp_gpu", L"GPU", snap.gpu.size() + snap.gpus.size());
            readings(snap.gpu);
            readings(snap.gpus);
            readings(FilterLhm(lhm, LhmGroup::Gpu));
            EndGroup();
        }
        if (SensorGroupVisible(ctx.cfg, SensorGroup::Mem)) {
            BeginGroup("##grp_mem", L"内存", snap.memory.size());
            readings(snap.memory);
            readings(FilterLhm(lhm, LhmGroup::Mem));
            EndGroup();
        }
        if (SensorGroupVisible(ctx.cfg, SensorGroup::Disk)) {
            BeginGroup("##grp_disk", L"磁盘", snap.disks.size());
            DrawDisks(ctx, snap.disks);
            readings(FilterLhm(lhm, LhmGroup::Disk));
            EndGroup();
        }
        if (SensorGroupVisible(ctx.cfg, SensorGroup::Net)) {
            BeginGroup("##grp_net", L"网络", snap.network.size());
            readings(snap.network);
            readings(FilterLhm(lhm, LhmGroup::Net));
            EndGroup();
        }
        if (SensorGroupVisible(ctx.cfg, SensorGroup::Battery)) {
            BeginGroup("##grp_battery", L"电池", snap.battery.size());
            readings(snap.battery);   // NoHardware 条目 = 诚实的空态
            readings(FilterLhm(lhm, LhmGroup::Battery));
            EndGroup();
        }
        if (SensorGroupVisible(ctx.cfg, SensorGroup::Fan)) {
            BeginGroup("##grp_fan", L"风扇", snap.fans.size());
            readings(snap.fans);      // 诚实的 NeedDriver 条目
            readings(FilterLhm(lhm, LhmGroup::Fan));
            EndGroup();
        }
        // G-B 的 extra 读数（不归入上述任一组的杂项；契约：空 = 未发现，整组不显示）。
        if (!snap.extra.empty() && SensorGroupVisible(ctx.cfg, SensorGroup::Extra)) {
            BeginGroup("##grp_extra", L"其他", snap.extra.size());
            readings(snap.extra);
            EndGroup();
        }
        // TODO(integrator): G-B 后续如再向 collect/Sensors.h 增补字段，在本函数
        // 对应分组内追加一行 `readings(snap.<newField>);` 即可 —— 精简/详细开关、
        // 四态诚实渲染与组显隐（PageHelpers.h SensorGroup）自动生效。
    }

private:
    using Result = AsyncFetch<SensorSnapshot>::Result;
    using LhmResult = AsyncFetch<std::vector<SensorReading>>::Result;
    static SensorSnapshot Produce(std::wstring* err) { return ReadSensors(err); }
    static std::vector<SensorReading> LhmProduce(std::wstring* err) {
        std::vector<SensorReading> out;
        PollLhm(&out, err);  // 仅限本机的客户端，约 1s 有界超时
        return out;
    }

    static bool AllEmpty(const SensorSnapshot& s) {
        return s.cpu.empty() && s.gpu.empty() && s.disks.empty() && s.fans.empty() &&
               s.cpuCores.empty() && s.gpus.empty() && s.network.empty() &&
               s.battery.empty() && s.memory.empty();
    }

    // ---- 偏好（cfg 持久化）--------------------------------------------------
    void LoadPrefsOnce(AppContext& ctx) {
        if (prefsLoaded_) return;
        prefsLoaded_ = true;
        detailMode_ = ctx.cfg.GetBool(L"sensDetailMode", false);  // 用户需求②：默认关
        lhmPort_ = static_cast<uint16_t>(std::max(
            1024, std::min(65535, static_cast<int>(ctx.cfg.GetInt(L"lhmPort", 8085)))));
        lhmOn_ = ctx.cfg.GetBool(L"lhmEnabled", false);
        if (lhmOn_) {
            // 从上一会话恢复：信任之前先重新探测。
            SetLhmOptions(stm::LhmOptions{lhmOn_, lhmPort_});
            StartLhmProbe(ctx);
        }
    }

    // ---- LHM 来源（默认关；经任务队列校验）----------------------------------
    struct LhmProbe {
        std::mutex mu;
        bool ready = false;
        bool ok = false;
        size_t count = 0;
        std::wstring err;
    };

    void StartLhmProbe(AppContext& /*ctx*/) {
        SetLhmOptions(stm::LhmOptions{true, lhmPort_});
        lhmProbe_ = std::make_shared<LhmProbe>();
        std::shared_ptr<LhmProbe> probe = lhmProbe_;
        std::shared_ptr<AppContext> app = LiveP3Ctx();
        if (!app) {
            std::lock_guard<std::mutex> lock(probe->mu);
            probe->ready = true;
            probe->err = L"操作队列未运行";
            return;
        }
        app->jobs.Submit([app, probe] {
            std::vector<SensorReading> out;
            std::wstring err;
            const bool ok = PollLhm(&out, &err);
            std::lock_guard<std::mutex> lock(probe->mu);
            probe->ok = ok;
            probe->count = out.size();
            probe->err = err;
            probe->ready = true;
        });
    }

    // 驱动探测状态机；绝不阻塞 UI（PollLhm 在 ops 工作线程上
    // 运行）。失败 => 退回关闭并附诚实说明（P2 要求）。
    void PollLhmProbe(AppContext& ctx) {
        if (!lhmProbe_) return;
        bool ready = false, ok = false;
        size_t count = 0;
        std::wstring err;
        {
            std::lock_guard<std::mutex> lock(lhmProbe_->mu);
            ready = lhmProbe_->ready;
            ok = lhmProbe_->ok;
            count = lhmProbe_->count;
            err = lhmProbe_->err;
        }
        if (!ready) return;
        lhmProbe_.reset();
        if (ok) {
            lhmOn_ = true;
            ctx.cfg.SetBool(L"lhmEnabled", true);
            PushNote(ctx, Notification::Kind::Info,
                     Fmt(L"已连接 LibreHardwareMonitor 数据源（{} 项读数，仅本机 127.0.0.1）",
                         count));
        } else {
            lhmOn_ = false;
            ctx.cfg.SetBool(L"lhmEnabled", false);
            PushNote(ctx, Notification::Kind::JobFailed,
                     Fmt(L"未检测到 LibreHardwareMonitor 数据源（确认 LHM 已运行并启用"
                         L" Remote Web Server）：{}",
                         err.empty() ? std::wstring(L"连接失败") : err));
        }
    }

    void DrawLhmSection(AppContext& ctx) {
        ImGui::TextUnformatted(U8(L"LibreHardwareMonitor 数据源"));
        ImGui::SameLine();
        ImGui::TextDisabled("%s",
                            U8(L"需自行运行 LibreHardwareMonitor 并启用其 Web 服务器；"
                               L"本工具仅读本机 127.0.0.1，不上传"));
        if (lhmProbe_) {
            ImGui::TextDisabled("%s", U8(L"正在检测数据源…"));
            return;
        }
        bool on = lhmOn_;
        if (ImGui::Checkbox(U8(L"启用（实验性）"), &on)) {
            if (on) {
                StartLhmProbe(ctx);  // 使用前经 ops 队列校验
            } else {
                SetLhmOptions(stm::LhmOptions{false, lhmPort_});
                lhmOn_ = false;
                ctx.cfg.SetBool(L"lhmEnabled", false);
                PushNote(ctx, Notification::Kind::Info, L"已关闭 LibreHardwareMonitor 数据源");
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        int port = lhmPort_;
        if (ImGui::InputInt(U8(L"端口"), &port, 0, 0)) {
            port = std::max(1024, std::min(65535, port));
            if (port != lhmPort_) {
                lhmPort_ = static_cast<uint16_t>(port);
                ctx.cfg.SetInt(L"lhmPort", lhmPort_);
                if (lhmOn_) StartLhmProbe(ctx);  // 用新端口重新校验
            }
        }
        if (!lhmOn_) {
            ImGui::TextDisabled("%s", U8(L"已关闭"));
        } else {
            std::shared_ptr<const LhmResult> lres = lhmFetch_.Peek();
            if (lres == nullptr) {
                ImGui::TextDisabled("%s", U8(L"等待下一次轮询…"));
            } else if (!lres->err.empty()) {
                ImGui::TextColored(ColWarn(), "%s",
                                   U8(Fmt(L"未检测到数据源（确认 LHM 已运行并启用 Remote Web "
                                          L"Server）：{}",
                                          lres->err)));
            } else {
                ImGui::TextColored(ColDone(), "%s",
                                   U8(Fmt(L"已连接（{} 项读数，标注［LHM］）", lres->data.size())));
            }
        }
    }

    // 当前 LHM 读数（禁用 / 尚未抓取时为空）。
    std::vector<SensorReading> CurrentLhmRows() const {
        std::vector<SensorReading> rows;
        if (!lhmOn_) return rows;
        std::shared_ptr<const LhmResult> res = lhmFetch_.Peek();
        if (res == nullptr || !res->err.empty()) return rows;
        return res->data;
    }

    static std::vector<SensorReading> FilterLhm(const std::vector<SensorReading>& rows,
                                                LhmGroup group) {
        std::vector<SensorReading> out;
        for (const SensorReading& r : rows) {
            // P2：扩展分类器——不带 "cpu" 记号的 AMD 风格每核温度
            // 也进入 CPU 组（见 LhmGroupOfCpuTemps）。
            if (LhmGroupOfCpuTemps(r.label) == group) out.push_back(r);
        }
        return out;
    }

    // ---- 可见性开关 -----------------------------------------------------------
    void DrawVisibilityRow(AppContext& ctx) {
        ImGui::TextDisabled("%s", U8(L"可选显示："));
        auto checkbox = [&](SensorGroup g) {
            bool v = SensorGroupVisible(ctx.cfg, g);
            if (ImGui::Checkbox(U8(Fmt(L"{}", SensorGroupTitle(g))), &v)) {
                ctx.cfg.SetBool(SensorGroupCfgKey(g), v);
            }
        };
        checkbox(SensorGroup::Cpu);
        ImGui::SameLine();
        checkbox(SensorGroup::Gpu);
        ImGui::SameLine();
        checkbox(SensorGroup::Mem);
        ImGui::SameLine();
        checkbox(SensorGroup::Disk);
        ImGui::NewLine();
        checkbox(SensorGroup::Net);
        ImGui::SameLine();
        checkbox(SensorGroup::Battery);
        ImGui::SameLine();
        checkbox(SensorGroup::Fan);
        ImGui::SameLine();
        checkbox(SensorGroup::Extra);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", U8(L"（风扇默认关闭：无内核驱动时仅能如实报告“需要驱动支持”；"
                                     L"「其他」仅在发现杂项读数时出现）"));
        ImGui::Separator();
    }

    // ---- 分组脚手架（纵向、全宽：P1-3）---------------------------------------
    static void BeginGroup(const char* id, const wchar_t* title, size_t readingCount) {
        ImGui::BeginChild(id, ImVec2(0.0f, 0.0f),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        ImGui::TextUnformatted(U8(title));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", U8(Fmt(L"（{} 项）", readingCount)));
        ImGui::Separator();
    }
    static void EndGroup() { ImGui::EndChild(); }

    static void DrawReadings(AppContext& ctx, const std::vector<SensorReading>& items) {
        for (const SensorReading& s : items) DrawReading(ctx, s);
    }

    // F4#9 详细模式：逐条渲染全部读数（完整标签不截断 + 独立数值/状态列）。
    static void DrawReadingsDetailed(AppContext& ctx, const std::vector<SensorReading>& items) {
        if (items.empty()) return;
        const int flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_SizingFixedFit;
        if (!ImGui::BeginTable("##detailreadings", 3, flags)) return;
        ImGui::TableSetupColumn(U8(L"读数"), ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableSetupColumn(U8(L"数值"), ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn(U8(L"状态"), ImGuiTableColumnFlags_WidthFixed, 190.0f);
        ImGui::TableHeadersRow();
        for (const SensorReading& s : items) {
            ImGui::PushID(static_cast<int>(&s - items.data()));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(U8(s.label.empty() ? std::wstring(L"—") : s.label));
            ImGui::TableNextColumn();
            if (s.state == SensorReading::State::Ok) {
                ImGui::TextUnformatted(U8(Fmt(L"{:.2f} {}", s.value, s.unit)));
            } else {
                ImGui::TextDisabled("%s", U8(L"—"));
            }
            ImGui::TableNextColumn();
            switch (s.state) {
                case SensorReading::State::Ok:
                    ImGui::TextColored(ColDone(), "%s", U8(L"正常"));
                    break;
                case SensorReading::State::NeedAdmin: {
                    ImGui::TextColored(ColWarn(), "%s", U8(L"需要管理员权限"));
                    if (!ctx.elevated && ops::CanElevate()) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton(U8(L"提权重启"))) {
                            RequestElevateRestart(ctx);  // P1-4：任务工作线程，而非 UI 线程
                        }
                    }
                    break;
                }
                case SensorReading::State::NeedDriver:
                    ImGui::TextColored(ColMuted(), "%s", U8(L"需要驱动支持"));
                    break;
                case SensorReading::State::NoHardware:
                default:
                    ImGui::TextColored(ColMuted(), "%s", U8(L"本机无此传感器"));
                    break;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // 每核频率 + 利用率表（cpuCores：MHz 与 % 行按核心索引配对；
    // 聚合兜底行改为普通读数展示）。
    void DrawCoreTable(AppContext& ctx, const std::vector<SensorReading>& cores) {
        struct CoreRow {
            double mhz = kUnavail;
            double util = kUnavail;
        };
        std::map<int, CoreRow> byIdx;
        std::vector<SensorReading> aggregates;
        for (const SensorReading& r : cores) {
            bool isFreq = false;
            const int idx = SensorCoreIndex(r.label, &isFreq);
            if (idx < 0) {
                aggregates.push_back(r);
                continue;
            }
            CoreRow& row = byIdx[std::max(0, idx)];
            if (isFreq) {
                row.mhz = r.state == SensorReading::State::Ok ? r.value : kUnavail;
            } else {
                row.util = r.state == SensorReading::State::Ok ? r.value : kUnavail;
            }
        }
        if (byIdx.empty()) {
            DrawReadings(ctx, aggregates);  // e.g. "CPU 频率" aggregate fallback
            return;
        }
        for (const SensorReading& r : aggregates) DrawReading(ctx, r);
        if (ImGui::BeginTable("coretable", 3, ImGuiTableFlags_RowBg |
                                                  ImGuiTableFlags_BordersInnerH |
                                                  ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn(U8(L"核心"), ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn(U8(L"频率 (MHz)"), ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableSetupColumn(U8(L"占用率"), ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableHeadersRow();
            for (const auto& kv : byIdx) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(Fmt(L"核心 {}", kv.first)));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(kv.second.mhz == kv.second.mhz
                                              ? Fmt(L"{:.0f}", kv.second.mhz)
                                              : std::wstring(L"—")));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(kv.second.util == kv.second.util
                                              ? Fmt(L"{:.1f}%", kv.second.util)
                                              : std::wstring(L"—")));
            }
            ImGui::EndTable();
        }
    }

    static void DrawReading(AppContext& ctx, const SensorReading& s) {
        ImGui::TextDisabled("%s", U8(Truncate(s.label, 44)));
        if (s.label.size() > 44 && ImGui::IsItemHovered()) {
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
                        RequestElevateRestart(ctx);  // P1-4: jobs worker, not UI thread
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
        if (disks.empty()) {
            ImGui::TextDisabled("%s", U8(L"本机未发现可查询的磁盘"));
            return;
        }
        const int flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_SizingFixedFit;
        if (ImGui::BeginTable("disks", 7, flags)) {
            ImGui::TableSetupColumn(U8(L"型号"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn(U8(L"总线"), ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn(U8(L"健康"), ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn(U8(L"温度"), ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn(U8(L"备件%"), ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn(U8(L"磨损%"), ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn(U8(L"通电时长"), ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableHeadersRow();
            for (const DiskHealth& d : disks) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    U8(d.model.empty() ? std::wstring(L"—") : Truncate(d.model, 40)));
                if (ImGui::IsItemHovered() && !d.serial.empty()) {
                    std::wstring tip = Fmt(L"序列号：{}", d.serial);
                    if (d.critWarnValid && d.critWarnBits != 0) {
                        tip += Fmt(L"\nNVMe 关键警告位非零（0x{:02X}）", d.critWarnBits);
                    }
                    ImGui::SetTooltip("%s", U8(tip));
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
                ImGui::TextUnformatted(U8(
                    d.sparePct == UINT32_MAX
                        ? std::wstring(L"—")
                        : (d.spareThreshPct == UINT32_MAX
                               ? Fmt(L"{}%", d.sparePct)
                               : Fmt(L"{}%（阈值 {}%）", d.sparePct, d.spareThreshPct))));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(U8(d.wearPct == UINT32_MAX
                                              ? std::wstring(L"—")
                                              : Fmt(L"{}%", d.wearPct)));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    U8(d.powerOnHours == kUnavailU64
                           ? std::wstring(L"—")
                           : Fmt(L"{} 小时", d.powerOnHours)));
            }
            ImGui::EndTable();
        }
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
                        RequestElevateRestart(ctx);  // P1-4: jobs worker, not UI thread
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

    void DrawToolbar(AppContext& ctx, const Result* res) {
        (void)ctx;
        if (res != nullptr) {
            ImGui::TextUnformatted(U8(L"仅使用公开的用户模式数据源，不内置内核驱动"));
        }
        ImGui::SameLine();
        if (ImGui::Button(U8(L"刷新"))) MaybeFetchNow();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "%s",
                U8(L"手动刷新：立即重新读取传感器（可越过 10 秒最小间隔；SMART/温度查询"
                   L"较重，自动刷新受该间隔限制）"));
        }
        // F4#9（用户需求②）：详细模式开关（cfg sensDetailMode，默认关）。
        ImGui::SameLine();
        if (ImGui::Checkbox(U8(L"详细模式"), &detailMode_)) {
            ctx.cfg.SetBool(L"sensDetailMode", detailMode_);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "%s",
                U8(L"精简：各组概要读数；详细：逐条渲染全部读数（完整标签、数值与"
                   L"状态分列，含多实例）"));
        }
    }

    void MaybeFetchNow() { fetch_.MaybeFetch(Produce, true); }

    AsyncFetch<SensorSnapshot> fetch_{10.0};
    AsyncFetch<std::vector<SensorReading>> lhmFetch_{10.0};
    std::shared_ptr<LhmProbe> lhmProbe_;
    bool prefsLoaded_ = false;
    bool detailMode_ = false;  // F4#9: 传感器详细模式（cfg sensDetailMode）
    bool lhmOn_ = false;       // 已校验状态（探测通过），持久化于 cfg
    uint16_t lhmPort_ = 8085;
    uint64_t lastFrame_ = kNeverDrawn;
};

// ===========================================================================
// 阈值告警（可选的第 3 阶段附加）：CPU>90% / 内存>95% 在 UI 线程上
// 监视（数值本来就随每次快照到达）。每次事件一个气泡
// + 一个 toast，每指标 5 分钟冷却，带 5% 迟滞重新布防。
// 配置：alertOn（默认关）/ alertCpu / alertMem。
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
// 公共入口（app/ui3/Pages3.h 契约）。
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
    // 在采集器启动前应用持久化的 ETW 开关（main 在
    // CollectService::Start 之前调用 RegisterPages）。
    ctx.collect.SetNetEtwEnabled(ctx.cfg.GetBool(L"netEtw", false));
}

void AlertTick(AppContext& ctx) {
    AlertState& a = Alerts();
    if (!ctx.cfg.GetBool(L"alertOn", false)) {
        a.armedCpu = true;  // 禁用期间保持重新布防
        a.armedMem = true;
        return;
    }
    const double cpuThr = static_cast<double>(ctx.cfg.GetInt(L"alertCpu", 90));
    const double memThr = static_cast<double>(ctx.cfg.GetInt(L"alertMem", 95));
    std::shared_ptr<const Snapshot> snap = ctx.collect.Store().Get();
    const SystemInfo& sys = snap->sys;
    const double now = ImGui::GetTime();

    if (sys.cpuTotalPercent == sys.cpuTotalPercent) {  // NaN 检查
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
    // 屏外窗口：在 --smoke 下执行每个页面的 Draw 路径
    //（空态/错误态），不打扰可见外壳。
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
