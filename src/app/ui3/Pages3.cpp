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
#include "app/ui3/NetAdapterUi.h"  // A2: 适配器区纯逻辑（速度/排序/复制 IP）
#include "app/ui3/NetMonUi.h"  // D5: 实时监视区纯逻辑（过滤/CSV/标签）
#include "app/ui3/PcapUi.h"  // C2: 深度抓包区纯逻辑（HexDump/BPF 提示/行摘要/CSV）
#include "app/ui3/PageHelpers.h"
#include "app/ui3/PageLayout.h"  // M2: 顶栏位置固定的定高区/可见集合纯函数
#include "app/ui3/SplitterUi.h"  // U1: 网络页纵向可拖拽分隔条（手柄薄组件）
#include "app/ui3/ThemeCfg.h"    // P1③: netcol_* 列宽键登记/软删除（纯函数）
#include "app/ui/Pages.h"
#include "app/ui/ConfirmAction.h"
#include "app/ui/UiText.h"
#include "collect/AdapterInfo.h"
#include "collect/LhmSource.h"
#include "collect/NpcapSource.h"  // C2: 深度抓包源（wpcap.dll 动态绑定，零链接依赖）
#include "collect/NetMonitor.h"
#include "collect/NetTables.h"
#include "collect/Sensors.h"
#include "core/Str.h"
#include "ops/DriverOps.h"
#include "ops/Elevate.h"
#include "ops/ServiceOps.h"
#include "ops/Signature.h"
#include "ops/StartupOps.h"
#include "imgui.h"
#include "imgui_internal.h"  // P1③: ImGuiTable 列宽回读（GetCurrentTable）
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <shellapi.h>
#include <utility>
#include <vector>

namespace stm {
namespace ui3 {

// 共享槽位（定义在文件底部；匿名命名空间内使用）。
std::shared_ptr<AppContext>& P3Slot();
BalloonSink& SinkSlot();
std::shared_ptr<AppContext> LiveP3Ctx();

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

// ===========================================================================
// P1③：网络页/适配器明细表格列宽持久化（cfg netcol_<表名>_<列>）。
// 背景：io.IniFilename = nullptr（布局持久化由应用自己的配置负责），ImGui
// 自身的表格列宽记忆不落盘 —— 表格虽已 Resizable，重启后即回默认宽，用户
// 感知为「列宽不能调整」。键名生成/登记见 ThemeCfg.h::NetColCfgKey（纯函数，
// stm_selftest 覆盖）。用法（每表三步）：
//   float* w = NetColWidths("netconn", kDefs, 6);   // 惰性从 cfg 加载
//   TableSetupColumn(…, WidthFixed, w[2]);          // 固定列用缓存宽
//   NetColSaveWidths("netconn", 6);                 // EndTable 前 ~1Hz 回写
// 拉伸列（WidthStretch）不在持久化范围：缓存槽保持默认权重原样透传。
// ===========================================================================

constexpr int kNetColMaxCols = 12;

struct NetColCache {
    float w[kNetColMaxCols] = {};
    bool loaded = false;
    uint64_t loadedGen = 0;  // V29-P1-1：布局重置代际，变化即失效重读
};

// 表级缓存（惰性加载；UI 单线程，static 局部安全）。
NetColCache& NetColCacheFor(const char* table) {
    static std::map<std::string, NetColCache> caches;
    return caches[table];
}

float* NetColWidths(const char* table, const float* defaults, int count) {
    NetColCache& c = NetColCacheFor(table);
    const uint64_t gen = ui3::LayoutResetGeneration();
    if (!c.loaded || c.loadedGen != gen) {
        c.loaded = true;
        c.loadedGen = gen;
        std::shared_ptr<AppContext> app = LiveP3Ctx();
        for (int i = 0; i < count && i < kNetColMaxCols; ++i) {
            c.w[i] = defaults[i];
            if (app) {
                c.w[i] = static_cast<float>(
                    app->cfg.GetDouble(NetColCfgKey(table, i), defaults[i]));
            }
        }
    }
    return c.w;
}

// EndTable 前调用：回读 ImGui 表的每列实际宽（WidthGiven），变化时写回 cfg
//（SetDouble 是内存写，文件在退出时统一保存 —— 与 ProcessesPage::PersistWidths
// 同一模式；拖动停止才产生变化，频率自然受限）。
// persistMask：与列数等长的掩码，false = 拉伸列（不持久化）。
void NetColSaveWidths(const char* table, int count, const bool* persistMask) {
    ImGuiTable* t = ImGui::GetCurrentTable();
    if (t == nullptr) return;
    NetColCache& c = NetColCacheFor(table);
    std::shared_ptr<AppContext> app = LiveP3Ctx();
    if (!app) return;
    for (int i = 0; i < count && i < kNetColMaxCols; ++i) {
        if (persistMask != nullptr && !persistMask[i]) continue;  // 拉伸列跳过
        const float w = t->Columns[i].WidthGiven;
        if (w <= 0.01f || w == c.w[i]) continue;
        c.w[i] = w;
        app->cfg.SetDouble(NetColCfgKey(table, i), static_cast<double>(w));
    }
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
// A2：顶部新增「适配器」区（任务管理器风格的 以太网/WLAN 卡片），
//     独立的 AsyncFetch（EnumAdaptersNet，>=10s 缓存 + 手动刷新），
//     不与连接表过滤框联动；回环适配器不显示。
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

// ---- D5：实时监视数据层实例 -------------------------------------------------
// 文件级生命周期（晚于 main 返回析构）：析构会 join 差分轮询线程并停止
// 全部 ETW 会话（无孤儿会话）；UI 线程之外只有 NetMonitor 自己的线程访问。
// unique_ptr 提升到文件作用域：ShutdownNetMon（V32-P2-4）需要显式置空释放。
std::unique_ptr<stm::NetMonitor> g_netmon = std::make_unique<stm::NetMonitor>();
stm::NetMonitor& NetMonInst() { return *g_netmon; }

// C2：深度抓包会话（单实例）。析构停消费线程并 pcap_close（晚于 main 返回，
// 与 NetMonitor 同生命周期口径）；wpcap.dll 只在首次使用时动态加载。
stm::NpcapSource& PcapSourceInst() {
    static std::unique_ptr<stm::NpcapSource> src = std::make_unique<stm::NpcapSource>();
    return *src;
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
        // U1：纵向分栏高度一次性从 cfg 读回（netAdapterH / netmonH，存的是
        // 真实像素值 —— 拖动手柄按屏幕像素移动；缺键时默认值经 P1④ 布局
        // 缩放，与改版前 Scaled(200)/Scaled(340) 的显示一致）。写侧同 netEtw
        // 口径：松手才 SetDouble（内存写），文件由 main 退出时统一 Save。
        if (!heightsLoaded_ ||
            heightsLoadedGen_ != ui3::LayoutResetGeneration()) {
            // V34-P1-N1：订阅布局重置代际——「重置布局」后分栏高度回到默认。
            heightsLoaded_ = true;
            heightsLoadedGen_ = ui3::LayoutResetGeneration();
            netAdapterH_ = ui3::ClampRegionH(
                static_cast<float>(ctx.cfg.GetDouble(
                    L"netAdapterH", static_cast<double>(ui3::Scaled(
                                        ui3::kNetAdapterRegionDefH)))),
                ui3::kNetAdapterRegionMinH, ui3::kNetAdapterRegionMaxH);
            netmonH_ = ui3::ClampRegionH(
                static_cast<float>(ctx.cfg.GetDouble(
                    L"netmonH",
                    static_cast<double>(ui3::Scaled(ui3::kNetMonRegionDefH)))),
                ui3::kNetMonRegionMinH, ui3::kNetMonRegionMaxH);
        }
        // P-A（用户报告①）：「适配器」头行（标题+计数+刷新按钮+行内溢出
        // 提示+分隔线）保持页顶固定；其后全部区段（适配器卡定高区、实时
        // 监视折叠区、深度抓包折叠区、工具栏、连接表）包进填满剩余高度的
        // 滚动 Child —— 内容超出视口时只在此 Child 内滚动，头行 y 恒定。
        // 高度经 PageLayout.h::FillScrollRegionHeight 扣除一行条目间距，
        // 防止父级（##pagearea）因「子高+间距」超高而出现微滚动条。区内
        // 嵌套滚动（可拖拽卡区 / 可拖拽监视段 / 连接表 ScrollY）不变。
        DrawAdapterHeader();
        ImGui::BeginChild("##netscroll",
                          ImVec2(0.0f, ui3::FillScrollRegionHeight(
                                            ImGui::GetContentRegionAvail().y,
                                            ImGui::GetStyle().ItemSpacing.y)),
                          ImGuiChildFlags_None);
        // U1：本帧三区高度分配（纯函数 NetRegionHeights）。入参只有 cfg 高度
        // 与本滚动区可见高 —— 适配器数量/事件/包数等数据绝不是输入；无拖拽
        // 的帧三区高度逐位恒定（V28：「连接表」标题起始 y 稳定的前提；拖动
        // 中连接表随动是用户主动行为，松手后稳定）。深度抓包段不参与分配：
        // 折叠/展开是唯一形态（用户主动切换，V28 口径容忍该次位移），展开态
        // 维持 kPcapSectionHeight 定高、照旧由本滚动区消化。连接表 = 剩余
        // （BeginTable(0,0) 自动填满；元组的 connTable 用于分配决策与头部
        // 溢出提示）。两条分隔条在此处与监视段尾各一条，拖动分别调整
        // 适配器区 / 监视段高度。
        const ui3::NetRegionTriple netRegions =
            ui3::NetRegionHeights(netAdapterH_, netmonH_,
                                  ImGui::GetContentRegionAvail().y);
        adapterRegionHintH_ = netRegions.adapter;  // 头部溢出提示用（本帧值）
        DrawAdapterRegion(netRegions.adapter);
        // U1：分隔条①（适配器卡区 | 实时监视段）。拖动中只改 netAdapterH_
        // 内存值（下一帧经 NetRegionHeights 生效 = 实时预览）；松手才写 cfg。
        if (ui3::RegionSplitterY("##netadapterspl", &netAdapterH_,
                                 ui3::kNetAdapterRegionMinH,
                                 ui3::kNetAdapterRegionMaxH)) {
            ctx.cfg.SetDouble(L"netAdapterH", static_cast<double>(netAdapterH_));
        }
        DrawNetMon(ctx, netRegions.netmon);  // D5：实时监视区（段尾含分隔条②）
        DrawPcap(ctx);  // C2：深度抓包区（默认折叠；未装 Npcap 时只展示指引）
        ImGui::Separator();
        fetch_.MaybeFetch(Produce, false);
        std::shared_ptr<const Result> res = fetch_.Peek();

        DrawToolbar(ctx, res.get());
        ImGui::Separator();

        if (res == nullptr) {
            DrawLoading();
            ImGui::EndChild();
            return;
        }
        if (!res->ok && res->data.empty()) {
            bool retry = false;
            DrawLoadError(res->err, &retry);
            if (retry) fetch_.MaybeFetch(Produce, true);
            ImGui::EndChild();
            return;
        }
        if (!res->err.empty()) {
            ImGui::TextColored(ColWarn(), "%s",
                               U8(Fmt(L"部分数据不可用：{}", res->err)));
        }
        UpdateRows(*res);
        DrawTable(ctx, *res);
        ImGui::EndChild();  // ##netscroll（P-A：头行固定的滚动内容区）
    }

private:
    using Result = AsyncFetch<std::vector<ConnEntry>>::Result;
    static std::vector<ConnEntry> Produce(std::wstring* err) {
        return SnapshotConnections(err);
    }

    // ---- D5：实时监视区 -----------------------------------------------------
    // 三路数据源：连接事件流（免管理员）、按远程端点聚合（需管理员）、
    // DNS 解析记录（需管理员，实验性）。约定：
    //  - Drain 每帧执行（含折叠/暂停帧）：本地 deque 是展示层暂存，
    //    DrainDns 还承担 DNS 自动禁用检测（契约要求每帧调用）。
    //  - 「暂停显示」只冻结视图重建；后台记录与入队照常，取消暂停即补齐。
    //  - 三路开关经 ops 任务队列执行并诚实读回；失败回滚 + toast。
    struct NetMonToggle {
        std::mutex mu;
        bool ready = false;
        bool desired = false;
        bool actual = false;
        std::wstring err;  // DNS 失败原因（契约 SetDnsCapture 产出）
    };
    enum class MonWhich { Events, Traffic, Dns };

    static const wchar_t* MonToggleName(MonWhich which) {
        switch (which) {
            case MonWhich::Events: return L"连接事件监视";
            case MonWhich::Traffic: return L"按远程端点聚合";
            case MonWhich::Dns: return L"DNS 解析记录";
            default: return L"—";
        }
    }

    static std::vector<stm::RemoteTraffic> TopProduce(std::wstring* err) {
        (void)err;  // TopRemoteTraffic 无错误路径（未开启时为空表）
        return NetMonInst().TopRemoteTraffic(20);
    }

    void DrawNetMon(AppContext& ctx, float sectionH) {
        stm::NetMonitor& mon = NetMonInst();
        // 每帧取走新事件（顺序翻转为最新在前）；溢出按容量弃旧。
        std::vector<stm::ConnEvent> fresh;
        mon.DrainEvents(&fresh);
        if (!fresh.empty()) {
            for (auto it = fresh.rbegin(); it != fresh.rend(); ++it) {
                evAll_.push_front(std::move(*it));
            }
            if (evAll_.size() > kNetMonDisplayCap) evAll_.resize(kNetMonDisplayCap);
            ++evGen_;
        }
        std::vector<stm::DnsEvent> freshDns;
        mon.DrainDns(&freshDns);  // 契约：还承担 DNS 自动禁用检测，每帧必须调用
        if (!freshDns.empty()) {
            for (auto it = freshDns.rbegin(); it != freshDns.rend(); ++it) {
                dnsAll_.push_front(std::move(*it));
            }
            if (dnsAll_.size() > kNetMonDnsCap) dnsAll_.resize(kNetMonDnsCap);
            ++dnsGen_;
        }

        PollMonToggles(ctx);
        // 诚实读回：无开关在途时以采集器真实状态为准（含自动禁用）。
        if (evToggle_ == nullptr) evOn_ = mon.EventCaptureEnabled();
        if (trafficToggle_ == nullptr) trafficOn_ = mon.RemoteTrafficEnabled();
        if (dnsToggle_ == nullptr) {
            const bool actual = mon.DnsCaptureEnabled();
            if (dnsOn_ && !actual) dnsAutoDisabled_ = true;  // 契约的自动禁用路径
            dnsOn_ = actual;
        }

        // 过滤器控件 -> 纯逻辑过滤结构（子串预先小写化）。
        monFilter_.process = AsciiLower(Utf8ToWide(procFilterUtf8_));
        monFilter_.remote = AsciiLower(Utf8ToWide(remoteFilterUtf8_));
        monFilter_.proto = static_cast<ProtoFilter>(protoFilter_);
        monFilter_.kindMask = (kindNew_ ? kKindBitNew : 0u) |
                              (kindClosed_ ? kKindBitClosed : 0u) |
                              (kindState_ ? kKindBitState : 0u);
        // 暂停 = 冻结视图重建（后台照常记录）；恢复时强制重建一次。
        const bool filterChanged = !(monFilter_ == monAppliedFilter_);
        if (!paused_ && (filterChanged || evGen_ != viewEvGen_)) {
            viewEvents_.clear();
            for (const stm::ConnEvent& e : evAll_) {
                if (EventPassesFilter(monFilter_, e)) viewEvents_.push_back(e);
            }
            monAppliedFilter_ = monFilter_;
            viewEvGen_ = evGen_;
        }
        if (!paused_ && (filterChanged || dnsGen_ != viewDnsGen_)) {
            viewDns_.assign(dnsAll_.begin(), dnsAll_.end());
            viewDnsGen_ = dnsGen_;
        }

        // M2 审计（顶栏位置固定）：本头部行的 y 只取决于上方的适配器卡区
        //（U1 起为可拖拽定高，无拖拽的帧恒定）与分隔条。Drain/DNS/开关读回
        // 只改写本地容器（evAll_/dnsAll_/视图向量），全部渲染在头部行**之下**，
        // 且 CollapsingHeader 标签为常量文本 —— 头部行不受 Drain 内容影响。
        // 折叠/展开 = 用户主动行为（可接受）；展开时头部行以下的 y 恒定。
        if (!ImGui::CollapsingHeader(U8(L"实时监视"), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Separator();  // U1：监视段折叠时保留与抓包段之间的分隔线
            return;
        }

        // V28-P1-1（布局稳定化）：折叠头以下整段包进定高滚动段
        //（高度 = U1 可拖拽值 sectionH：cfg netmonH 经 NetRegionHeights
        // 分配/压缩，未拖拽且视口充裕时恒等于加载默认 Scaled(340)，选型理由见
        // NetMonUi.h）—— 诚实边界行、「已丢弃 N 条」/「DNS 自动禁用」警告行
        //（原画在定高区外，随数据出现/消失增减页面行数）、开关/过滤行、连接事件表
        //（空态单行↔300px 定高表互斥切换）、Top 远程目标表（原无定高，
        // ETW 聚合 0→20 行逐行下推 DNS 区）、DNS 表全部画进段内：任何
        // 数据变化只改变段内滚动量，绝不改变段外（深度抓包标题/工具栏/
        // 连接表标题）的 y 坐标。段内结构不变，「暂停显示」语义不变。
        ImGui::BeginChild("##netmonsection", ImVec2(0.0f, sectionH),
                          ImGuiChildFlags_None);
        // 诚实边界 + 丢弃警告。
        ImGui::TextWrapped("%s", U8(L"连接级监视：不捕获通信内容；远程聚合与 DNS 记录需要"
                                   L"管理员权限并依赖 ETW。"));
        const uint64_t dropped = mon.DroppedEvents();
        if (dropped > 0) {
            ImGui::TextColored(ColWarn(), "%s",
                               U8(Fmt(L"事件过多，已丢弃 {} 条（保新弃旧）", dropped)));
        }
        if (dnsAutoDisabled_ && !dnsOn_) {
            ImGui::TextColored(ColWarn(), "%s",
                               U8(L"DNS 记录已被自动禁用（事件字段不可靠或会话失效）；"
                                  L"可重新开启重试"));
        }

        DrawNetMonControls(ctx, mon);
        DrawNetMonFilters();
        DrawNetMonEventTable(ctx);
        DrawNetMonTop();
        DrawNetMonDns();
        ImGui::EndChild();
        // U1：分隔条②（实时监视段 | 深度抓包段，段尾、展开态专属）。拖动中
        // 只改 netmonH_ 内存值（下一帧经 NetRegionHeights 生效）；松手才写
        // cfg —— 落盘节流，与分隔条①同一模式。
        if (ui3::RegionSplitterY("##netmonspl", &netmonH_, ui3::kNetMonRegionMinH,
                                 ui3::kNetMonRegionMaxH)) {
            ctx.cfg.SetDouble(L"netmonH", static_cast<double>(netmonH_));
        }
    }

    void DrawNetMonControls(AppContext& ctx, stm::NetMonitor& mon) {
        // 开启监视（免管理员）。关闭只停止差分线程：已记录事件保留展示。
        ImGui::BeginDisabled(evToggle_ != nullptr);
        if (ImGui::Checkbox(U8(L"开启监视"), &evOn_)) {
            RequestMonToggle(ctx, MonWhich::Events, evOn_);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s",
                              U8(evToggle_ != nullptr
                                     ? std::wstring(L"正在切换（等待确认）…")
                                     : std::wstring(L"以约 1 秒节拍差分连接表，记录连接的新建/"
                                                    L"断开/状态变化（免管理员）。关闭监视不删除"
                                                    L"已记录的事件")));
        }
        // 按远程端点聚合（需管理员；失败回滚 + toast）。
        ImGui::SameLine();
        ImGui::BeginDisabled(trafficToggle_ != nullptr);
        if (ImGui::Checkbox(U8(L"按远程端点聚合（需管理员）"), &trafficOn_)) {
            RequestMonToggle(ctx, MonWhich::Traffic, trafficOn_);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s",
                              U8(std::wstring(L"经 ETW Kernel-Network 按远程 ip:端口 聚合收发"
                                              L"字节（需管理员权限）；失败时开关自动回滚")));
        }
        // DNS 解析记录（需管理员；实验性）。
        ImGui::SameLine();
        ImGui::BeginDisabled(dnsToggle_ != nullptr);
        if (ImGui::Checkbox(U8(L"DNS 解析记录（实验性·需管理员）"), &dnsOn_)) {
            if (dnsOn_) dnsAutoDisabled_ = false;
            RequestMonToggle(ctx, MonWhich::Dns, dnsOn_);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s",
                              U8(std::wstring(L"经 ETW DNS-Client 记录本机进程的域名解析"
                                             L"（需管理员权限，实验性：字段不可靠时会自动"
                                             L"禁用并提示）")));
        }
        // 暂停显示 / 清空 / 导出。
        ImGui::SameLine();
        if (ImGui::Checkbox(U8(L"暂停显示"), &paused_)) {
            if (!paused_) {
                viewEvGen_ = ~0ull;  // 恢复：强制重建冻结的视图
                viewDnsGen_ = ~0ull;
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                              U8(L"冻结表格刷新；后台照常记录，取消暂停后自动补齐"));
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(U8(L"清空"))) {
            mon.ClearEvents();  // 同时重置丢弃计数（诚实口径：自上次清空以来）
            evAll_.clear();
            dnsAll_.clear();
            ++evGen_;
            ++dnsGen_;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", U8(L"清空已记录的连接事件与 DNS 记录（不影响开关状态）"));
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(viewEvents_.empty());
        if (ImGui::SmallButton(U8(L"导出 CSV"))) ExportNetMonCsv(ctx);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s",
                              U8(L"把当前显示（已应用过滤）的事件写入 %LOCALAPPDATA%"
                                 L"\\SuperTaskMgr\\captures\\netmon_*.csv"
                                 L"（UTF-8 BOM，Excel 可直接打开）"));
        }
    }

    void DrawNetMonFilters() {
        ImGui::SetNextItemWidth(170.0f);
        ImGui::InputTextWithHint("##nmproc", U8(L"进程名包含"), procFilterUtf8_,
                                 sizeof(procFilterUtf8_));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(170.0f);
        ImGui::InputTextWithHint("##nmremote", U8(L"远程包含"), remoteFilterUtf8_,
                                 sizeof(remoteFilterUtf8_));
        ImGui::SameLine();
        const char* protoItems[3] = {U8(L"全部"), U8(L"TCP"), U8(L"UDP")};
        ImGui::SetNextItemWidth(90.0f);
        ImGui::Combo("##nmproto", &protoFilter_, protoItems, 3);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", U8(L"事件:"));
        ImGui::SameLine();
        ImGui::Checkbox(U8(L"新建"), &kindNew_);
        ImGui::SameLine();
        ImGui::Checkbox(U8(L"断开"), &kindClosed_);
        ImGui::SameLine();
        ImGui::Checkbox(U8(L"状态"), &kindState_);
    }

    void DrawNetMonEventTable(AppContext& ctx) {
        (void)ctx;
        ImGui::TextUnformatted(
            U8(Fmt(L"连接事件（显示 {} / 记录 {} 条；最新在上）", viewEvents_.size(),
                   evAll_.size())));
        if (paused_) {
            ImGui::SameLine();
            ImGui::TextColored(ColWarn(), "%s", U8(L"已暂停（后台仍在记录）"));
        }
        if (viewEvents_.empty()) {
            ImGui::TextColored(ColMuted(), "%s",
                               U8(evOn_ ? L"暂无匹配的事件（开启后新出现的连接才会产生事件）"
                                        : L"未开启监视；打开「开启监视」开始记录连接事件"));
            return;
        }
        const int flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_SizingFixedFit;
        // P1③：列宽 cfg 持久化（netcol_netmon_events_<列>）；进程列（3）为
        // 拉伸列，缓存槽存默认权重、不持久化。
        static constexpr float kDefCols[8] = {82.0f, 58.0f, 56.0f, 2.0f,
                                              168.0f, 168.0f, 92.0f, 92.0f};
        static constexpr bool kPersistCols[8] = {true, true, true, false,
                                                 true, true, true, true};
        float* w = NetColWidths("netmon_events", kDefCols, 8);
        ImGui::BeginChild("##nmevtchild", ImVec2(0.0f, Scaled(300.0f)), ImGuiChildFlags_Borders);
        if (ImGui::BeginTable("netmon_events", 8, flags)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(U8(L"时间"), ImGuiTableColumnFlags_WidthFixed, w[0]);
            ImGui::TableSetupColumn(U8(L"事件"), ImGuiTableColumnFlags_WidthFixed, w[1]);
            ImGui::TableSetupColumn(U8(L"协议"), ImGuiTableColumnFlags_WidthFixed, w[2]);
            ImGui::TableSetupColumn(U8(L"进程"), ImGuiTableColumnFlags_WidthStretch, w[3]);
            ImGui::TableSetupColumn(U8(L"本地"), ImGuiTableColumnFlags_WidthFixed, w[4]);
            ImGui::TableSetupColumn(U8(L"远程"), ImGuiTableColumnFlags_WidthFixed, w[5]);
            ImGui::TableSetupColumn(U8(L"服务"), ImGuiTableColumnFlags_WidthFixed, w[6]);
            ImGui::TableSetupColumn(U8(L"状态"), ImGuiTableColumnFlags_WidthFixed, w[7]);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(viewEvents_.size()));
            while (clipper.Step()) {
                for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                    const stm::ConnEvent& e = viewEvents_[static_cast<size_t>(r)];
                    ImGui::PushID(r);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(U8(FormatEventClock(e.unixTime)));
                    ImGui::TableNextColumn();
                    const ImVec4 tone =
                        EventToneOf(e.kind) == EventTone::Good
                            ? ColDone()
                            : (EventToneOf(e.kind) == EventTone::Bad ? ColFail() : ColWarn());
                    ImGui::TextColored(tone, "%s", U8(EventKindLabel(e.kind)));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(U8(NetMonProtoLabel(e.proto)));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(U8(EventProcessDisplay(e)));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(U8(MonEndpoint(e.localAddr, e.localPort)));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(U8(MonEndpoint(e.remoteAddr, e.remotePort)));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(U8(EventServiceDisplay(e)));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(
                        U8(e.stateLabel.empty() ? std::wstring(L"—") : e.stateLabel));
                    ImGui::PopID();
                }
            }
            NetColSaveWidths("netmon_events", 8, kPersistCols);
            ImGui::EndTable();
        }
        ImGui::EndChild();
    }

    void DrawNetMonTop() {
        if (!trafficOn_) return;  // 契约：仅 ETW 端点聚合开启时有数据
        topFetch_.MaybeFetch(TopProduce, false);
        std::shared_ptr<const AsyncFetch<std::vector<stm::RemoteTraffic>>::Result> res =
            topFetch_.Peek();
        ImGui::TextUnformatted(U8(L"Top 远程目标"));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", U8(L"（按收发总量排序，最多 20 行；字节为自启用起累计，"
                                     L"非速率）"));
        if (res == nullptr) {
            ImGui::TextColored(ColMuted(), "%s", U8(L"统计加载中…"));
            return;
        }
        if (res->data.empty()) {
            ImGui::TextColored(ColMuted(), "%s", U8(L"暂无端点聚合数据（等待本机网络流量…）"));
            return;
        }
        // P1③：本表原先无 Resizable（列宽完全锁死），连同持久化一并补齐。
        const int flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_SizingFixedFit;
        static constexpr float kDefCols[5] = {190.0f, 100.0f, 2.0f, 100.0f, 100.0f};
        static constexpr bool kPersistCols[5] = {true, true, false, true, true};
        float* w = NetColWidths("netmon_top", kDefCols, 5);
        if (!ImGui::BeginTable("netmon_top", 5, flags)) return;
        ImGui::TableSetupColumn(U8(L"远程:端口"), ImGuiTableColumnFlags_WidthFixed, w[0]);
        ImGui::TableSetupColumn(U8(L"服务"), ImGuiTableColumnFlags_WidthFixed, w[1]);
        ImGui::TableSetupColumn(U8(L"进程"), ImGuiTableColumnFlags_WidthStretch, w[2]);
        ImGui::TableSetupColumn(U8(L"↓入"), ImGuiTableColumnFlags_WidthFixed, w[3]);
        ImGui::TableSetupColumn(U8(L"↑出"), ImGuiTableColumnFlags_WidthFixed, w[4]);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < res->data.size(); ++i) {
            const stm::RemoteTraffic& t = res->data[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(
                U8(t.remote.empty() ? std::wstring(L"—")
                                    : t.remote + L":" + std::to_wstring(static_cast<unsigned>(t.port))));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(U8(t.service.empty() ? std::wstring(L"—") : t.service));
            ImGui::TableNextColumn();
            std::wstring name = t.processName.empty()
                                    ? (t.pid == 0 ? std::wstring(L"系统") : std::wstring(L"—"))
                                    : t.processName;
            ImGui::TextUnformatted(U8(name + L"（" + std::to_wstring(t.pid) + L"）"));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(U8(FormatBytes(static_cast<uint64_t>(t.bytesIn))));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(U8(FormatBytes(static_cast<uint64_t>(t.bytesOut))));
            ImGui::PopID();
        }
        NetColSaveWidths("netmon_top", 5, kPersistCols);
        ImGui::EndTable();
    }

    void DrawNetMonDns() {
        if (!dnsOn_) return;  // 契约：仅 DNS 捕获开启时显示
        ImGui::TextUnformatted(U8(Fmt(L"DNS 解析记录（{} 条；最新在上）", viewDns_.size())));
        if (viewDns_.empty()) {
            ImGui::TextColored(ColMuted(), "%s", U8(L"暂无解析记录（等待本机域名查询…）"));
            return;
        }
        const int flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_SizingFixedFit;
        // P1③：列宽 cfg 持久化（netcol_netmon_dns_<列>）；进程/域名两列为
        // 拉伸列，不持久化。
        static constexpr float kDefCols[3] = {82.0f, 1.4f, 2.6f};
        static constexpr bool kPersistCols[3] = {true, false, false};
        float* w = NetColWidths("netmon_dns", kDefCols, 3);
        ImGui::BeginChild("##nmdnschild", ImVec2(0.0f, Scaled(170.0f)), ImGuiChildFlags_Borders);
        if (ImGui::BeginTable("netmon_dns", 3, flags)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(U8(L"时间"), ImGuiTableColumnFlags_WidthFixed, w[0]);
            ImGui::TableSetupColumn(U8(L"进程"), ImGuiTableColumnFlags_WidthStretch, w[1]);
            ImGui::TableSetupColumn(U8(L"查询域名"), ImGuiTableColumnFlags_WidthStretch, w[2]);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(viewDns_.size()));
            while (clipper.Step()) {
                for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                    const stm::DnsEvent& d = viewDns_[static_cast<size_t>(r)];
                    ImGui::PushID(r);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(U8(FormatEventClock(d.unixTime)));
                    ImGui::TableNextColumn();
                    std::wstring name =
                        d.processName.empty()
                            ? (d.pid == 0 ? std::wstring(L"系统") : std::wstring(L"—"))
                            : d.processName;
                    ImGui::TextUnformatted(U8(name + L"（" + std::to_wstring(d.pid) + L"）"));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(U8(d.query));
                    ImGui::PopID();
                }
            }
            NetColSaveWidths("netmon_dns", 3, kPersistCols);
            ImGui::EndTable();
        }
        ImGui::EndChild();
    }

    void RequestMonToggle(AppContext& ctx, MonWhich which, bool desired) {
        std::shared_ptr<NetMonToggle> toggle = std::make_shared<NetMonToggle>();
        toggle->desired = desired;
        std::shared_ptr<NetMonToggle>* slot =
            which == MonWhich::Events
                ? &evToggle_
                : (which == MonWhich::Traffic ? &trafficToggle_ : &dnsToggle_);
        *slot = toggle;
        std::shared_ptr<AppContext> app = LiveP3Ctx();
        if (!app) {
            *slot = nullptr;  // 拆除竞争：读回同步自动回滚复选框
            PushNote(ctx, Notification::Kind::JobFailed,
                     L"操作队列未运行，开关未执行（应用可能正在退出）");
            return;
        }
        stm::NetMonitor* mon = &NetMonInst();
        if (app->jobs.Submit([app, toggle, mon, which, desired] {
                bool actual = false;
                std::wstring err;
                switch (which) {
                    case MonWhich::Events:
                        actual = mon->SetEventCapture(desired);
                        break;
                    case MonWhich::Traffic:
                        actual = mon->SetRemoteTraffic(desired);
                        break;
                    case MonWhich::Dns:
                        actual = mon->SetDnsCapture(desired, &err);
                        break;
                    default:
                        break;
                }
                std::lock_guard<std::mutex> lock(toggle->mu);
                toggle->actual = actual;
                toggle->err = std::move(err);
                toggle->ready = true;
            }) == 0) {
            *slot = nullptr;  // 队列已关：回滚（诚实读回恢复原值）
            PushNote(ctx, Notification::Kind::JobFailed,
                     L"操作队列未运行，开关未执行（应用可能正在退出）");
        }
    }

    void PollMonToggles(AppContext& ctx) {
        auto poll = [&](std::shared_ptr<NetMonToggle>& slot, MonWhich which) {
            if (!slot) return;
            bool ready = false, desired = false, actual = false;
            std::wstring err;
            {
                std::lock_guard<std::mutex> lock(slot->mu);
                ready = slot->ready;
                desired = slot->desired;
                actual = slot->actual;
                err = slot->err;
            }
            if (!ready) return;
            slot.reset();
            const std::wstring name = MonToggleName(which);
            if (actual == desired) {
                PushNote(ctx, Notification::Kind::Info,
                         Fmt(L"已{}{}", desired ? L"开启" : L"关闭", name));
                return;
            }
            if (desired) {
                PushNote(ctx, Notification::Kind::JobFailed,
                         which == MonWhich::Traffic
                             ? std::wstring(L"按远程端点聚合开启失败：常见为缺少管理员权限"
                                            L"或 ETW 会话数已达上限/被策略阻止")
                             : Fmt(L"{}开启失败：{}", name,
                                   err.empty() ? std::wstring(L"原因详见日志") : err));
            } else {
                PushNote(ctx, Notification::Kind::JobFailed,
                         Fmt(L"{}关闭失败（原因详见日志）", name));
            }
        };
        poll(evToggle_, MonWhich::Events);
        poll(trafficToggle_, MonWhich::Traffic);
        poll(dnsToggle_, MonWhich::Dns);
    }

    void ExportNetMonCsv(AppContext& ctx) {
        if (viewEvents_.empty()) return;
        auto rows = std::make_shared<const std::vector<stm::ConnEvent>>(viewEvents_);
        std::shared_ptr<AppContext> app = LiveP3Ctx();
        if (!app) {
            PushNote(ctx, Notification::Kind::JobFailed,
                     L"操作队列未运行，导出未执行（应用可能正在退出）");
            return;
        }
        if (app->jobs.Submit([app, rows] {
                const std::wstring dir = NetMonCsvDir();
                if (EnsureDir(dir).empty()) {  // 契约：失败返回空串
                    PushNote(*app, Notification::Kind::JobFailed,
                             Fmt(L"创建目录失败：{}", dir));
                    return;
                }
                const std::wstring path =
                    dir + L"\\" + NetMonCsvFileName(static_cast<int64_t>(std::time(nullptr)));
                std::ofstream f(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
                if (!f.is_open()) {
                    PushNote(*app, Notification::Kind::JobFailed,
                             L"无法创建 CSV 文件（被占用或无写入权限）");
                    return;
                }
                f << "\xEF\xBB\xBF";  // UTF-8 BOM：Excel 直接双击可读
                f << WideToUtf8(BuildNetMonCsv(*rows));
                f.flush();
                const bool ok = f.good();
                f.close();
                if (ok) {
                    PushNote(*app, Notification::Kind::JobDone,
                             Fmt(L"已导出 {} 条事件：{}", rows->size(), path));
                } else {
                    PushNote(*app, Notification::Kind::JobFailed, L"写入 CSV 失败（磁盘错误）");
                }
            }) == 0) {
            PushNote(ctx, Notification::Kind::JobFailed,
                     L"操作队列未运行，导出未执行（应用可能正在退出）");
        }
    }

    // ---- C2：深度抓包（Npcap）区 --------------------------------------------
    // 诚实边界（文案在 DrawPcap 内呈现）：依赖用户显式安装的 Npcap（本应用
    // 不下载/不分发/不代装驱动）；抓包需管理员权限，打开失败如实报错；载荷
    // 仅保留前 256 字节；导出 CSV 仅元数据（载荷与 .pcap 导出明确不提供）。
    // 默认折叠：折叠帧只做一次廉价的安装检测（SCM/文件存在性）。
    // V28-P1-1：展开态正文包进定高滚动段，段高与实时监视段
    //（kNetMonSectionHeight，NetMonUi.h 有完整选型说明）同取 340px ——
    // 段位于页中部、其下还有工具栏与连接表，固定值使段下布局只随窗口
    // 缩放变化，内容高度绝不是输入。
    static constexpr float kPcapSectionHeight = 340.0f;

    struct PcapToggle {
        std::mutex mu;
        bool ready = false;
        bool ok = false;
        std::wstring err;
    };

    using DeviceResult = AsyncFetch<std::vector<stm::PcapDevice>>::Result;
    static std::vector<stm::PcapDevice> PcapDevicesProduce(std::wstring* err) {
        return stm::ListDevices(err);
    }

    void DrawPcap(AppContext& ctx) {
        if (!npcapChecked_) {  // 免管理员、无副作用：只查服务与 DLL 存在性
            npcapChecked_ = true;
            npcapPresent_ = stm::NpcapInstalled();
        }
        if (!ImGui::CollapsingHeader(U8(L"深度抓包（Npcap）"))) return;

        if (!npcapPresent_) {
            ImGui::TextWrapped("%s", U8(std::wstring(stm::NpcapInstallGuidance())));
            if (ImGui::Button(U8(L"打开官方下载页"))) {
                const INT_PTR rc = reinterpret_cast<INT_PTR>(::ShellExecuteW(
                    nullptr, L"open", stm::NpcapDownloadUrl(), nullptr, nullptr,
                    SW_SHOWNORMAL));
                if (rc <= 32) {
                    PushNote(ctx, Notification::Kind::JobFailed,
                             L"无法打开浏览器，请手动访问 https://npcap.com/dist/");
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "%s", U8(L"打开 npcap.com 官方下载页（用户主动安装；本应用"
                             L"不下载、不分发、不代装驱动）"));
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(U8(L"重新检测"))) npcapChecked_ = false;
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", U8(L"安装完成后点击重新检测（也可重启应用）"));
            }
            return;
        }

        // V28-P1-1（与实时监视段同原则）：展开后的整段包进定高滚动段
        //（kPcapSectionHeight 经 P1④ 运行时缩放）—— 启动失败错误行、「已捕获…」
        // 统计行、包表空态单行↔300px 定高表互斥切换、选中包详情（徽标行/截断
        // 警告/hex 视图 190px）的出现/消失全部由段内滚动消化，工具栏与连接表
        // 标题不再被顶动。折叠头保留（用户折叠 = 主动行为）；未安装分支为静态
        // 指引（仅「重新检测」用户动作可改变），无数据驱动抖动，保持段外。
        ImGui::BeginChild("##pcapsection", ImVec2(0.0f, Scaled(kPcapSectionHeight)),
                          ImGuiChildFlags_None);
        DrawPcapBody(ctx);
        ImGui::EndChild();
    }

    // V28-P1-1：定高滚动段（##pcapsection）内部的原展开态正文。空态单行、
    // 错误行、详情块等「出现即改变高度」的内容全部画在段内，早退只影响
    // 段内滚动量；Drain 每帧执行（含空态帧）与暂停积压语义不变。
    void DrawPcapBody(AppContext& ctx) {
        PollPcapToggle(ctx);
        // 每帧取走新包（暂停帧也照常取走：积压进 hold，恢复后并入，不丢包）。
        std::vector<stm::PktRecord> fresh;
        PcapSourceInst().DrainPackets(&fresh);
        if (!fresh.empty()) {
            if (pcapPaused_) {
                pktsHold_.insert(pktsHold_.begin(), fresh.rbegin(), fresh.rend());
                if (pktsHold_.size() > stm::kPktRingCap) {
                    pktsHold_.resize(stm::kPktRingCap);
                }
            } else {
                for (auto it = fresh.rbegin(); it != fresh.rend(); ++it) {
                    pktsAll_.push_front(std::move(*it));  // 最新在前
                }
                if (pktsAll_.size() > stm::kPktRingCap) {
                    pktsAll_.resize(stm::kPktRingCap);
                }
            }
        }

        ImGui::TextWrapped(
            "%s",
            U8(L"抓包需管理员权限（依赖 Wireshark 同源的 Npcap 驱动）。仅本机显示、"
               L"不上传。载荷仅保留前 256 字节；「导出 CSV」仅含元数据（载荷与 "
               L".pcap 导出明确不提供）。"));

        // 设备行：下拉 + 重新扫描（枚举走任务队列，毫秒级、不阻塞 UI）。
        pcapDevices_.MaybeFetch(PcapDevicesProduce, false);
        std::shared_ptr<const DeviceResult> devs = pcapDevices_.Peek();
        if (devs != nullptr && lastPcapDevices_ != devs.get()) {
            lastPcapDevices_ = devs.get();
            pcapDevs_ = devs->data;
            pcapDevIndex_ = -1;
            for (size_t i = 0; i < pcapDevs_.size(); ++i) {
                if (pcapDevs_[i].name == pcapDevName_) {
                    pcapDevIndex_ = static_cast<int>(i);
                }
            }
        }
        ImGui::TextUnformatted(U8(L"设备"));
        ImGui::SameLine();
        std::vector<std::string> devLabels;
        devLabels.reserve(pcapDevs_.size());
        for (const stm::PcapDevice& d : pcapDevs_) {
            devLabels.push_back(WideToUtf8(stm::ui3::PcapDeviceLabel(d)));
        }
        const char* preview =
            pcapDevIndex_ >= 0 &&
                    static_cast<size_t>(pcapDevIndex_) < devLabels.size()
                ? devLabels[static_cast<size_t>(pcapDevIndex_)].c_str()
                : U8(L"选择捕获设备…");
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::BeginCombo("##pcapdev", preview)) {
            for (int i = 0; i < static_cast<int>(devLabels.size()); ++i) {
                const bool selected = i == pcapDevIndex_;
                if (ImGui::Selectable(devLabels[static_cast<size_t>(i)].c_str(),
                                      selected)) {
                    pcapDevIndex_ = i;
                    pcapDevName_ = pcapDevs_[static_cast<size_t>(i)].name;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                              U8(pcapDevName_.empty()
                                     ? std::wstring(L"先选择要抓包的网卡；"
                                                    L"回环流量请选「回环」设备")
                                     : pcapDevName_));
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(U8(L"重新扫描"))) {
            pcapDevices_.MaybeFetch(PcapDevicesProduce, true);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", U8(L"立即重新枚举捕获设备（可越过 10 秒最小间隔）"));
        }

        // BPF 过滤 + 启停（pcap_open_live 可能阻塞，走任务队列）。
        ImGui::SetNextItemWidth(300.0f);
        ImGui::InputTextWithHint("##pcapbpf", U8(L"BPF 过滤（留空不过滤）"),
                                 pcapBpfUtf8_, sizeof(pcapBpfUtf8_));
        ImGui::SetItemTooltip("%s", U8(stm::ui3::BpfHintText()));
        ImGui::SameLine();
        const bool running = PcapSourceInst().Running();
        ImGui::BeginDisabled(pcapToggle_ != nullptr || pcapDevName_.empty() ||
                             running);
        if (ImGui::Button(U8(L"开始抓包"))) RequestPcapStart(ctx);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s",
                              U8(pcapDevName_.empty()
                                     ? std::wstring(L"先选择捕获设备")
                                     : std::wstring(L"打开设备（混杂模式，全帧捕获）"
                                                    L"并应用 BPF 过滤器；需管理员权限")));
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(pcapToggle_ != nullptr || !running);
        if (ImGui::Button(U8(L"停止"))) RequestPcapStop(ctx);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", U8(L"停止并丢弃会话（已显示的包保留）"));
        }
        if (!running && !pcapLastErr_.empty()) {
            ImGui::TextColored(ColFail(), "%s", U8(pcapLastErr_));
        }

        // 统计行 + 暂停/清空/导出。
        ImGui::TextUnformatted(
            U8(Fmt(L"已捕获 {} · 丢弃 {}（保新弃旧）· 显示 {} 条（最新在上）",
                   PcapSourceInst().CapturedTotal(),
                   PcapSourceInst().DroppedPackets(), pktsAll_.size())));
        if (running) {
            ImGui::SameLine();
            ImGui::TextColored(ColDone(), "%s", U8(L"抓包进行中"));
        }
        ImGui::SameLine();
        if (ImGui::Checkbox(U8(L"暂停显示"), &pcapPaused_)) {
            if (!pcapPaused_ && !pktsHold_.empty()) {  // 恢复：并入积压的包
                for (auto it = pktsHold_.rbegin(); it != pktsHold_.rend(); ++it) {
                    pktsAll_.push_front(std::move(*it));
                }
                pktsHold_.clear();
                if (pktsAll_.size() > stm::kPktRingCap) {
                    pktsAll_.resize(stm::kPktRingCap);
                }
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                              U8(L"冻结表格刷新；后台照常抓取，取消暂停后自动补齐"
                                 L"（积压超出 4096 条按保新弃旧丢弃）"));
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(U8(L"清空"))) {
            pktsAll_.clear();
            pktsHold_.clear();
            pcapHasSel_ = false;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(pktsAll_.empty());
        if (ImGui::SmallButton(U8(L"导出 CSV"))) ExportPcapCsv(ctx);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "%s", U8(L"把当前显示的包写入 %LOCALAPPDATA%\\SuperTaskMgr\\captures"
                         L"\\pcap_*.csv（UTF-8 BOM）。仅元数据列：时间/源/目的/协议/"
                         L"端口/长度/信息；载荷与 .pcap 文件导出不提供"));
        }

        if (pktsAll_.empty()) {
            // V28-P1-1：空态画在定高段内部 —— 首包到达只增加段内滚动量，
            // 不再使段外（工具栏/连接表标题）一次性下移。
            ImGui::TextColored(
                ColMuted(), "%s",
                U8(running
                       ? L"已开始，等待数据包（可在本机产生流量，如 ping 127.0.0.1；"
                         L"回环设备只显示本机回环流量）"
                       : L"未开始抓包；选择设备后点击「开始抓包」"));
            return;
        }
        const int flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_SizingFixedFit;
        // P1③：列宽 cfg 持久化（netcol_pcap_pkts_<列>）；源→目的/信息两列
        // 为拉伸列，不持久化。
        static constexpr float kDefCols[5] = {118.0f, 2.0f, 84.0f, 66.0f, 3.0f};
        static constexpr bool kPersistCols[5] = {true, false, true, true, false};
        float* w = NetColWidths("pcap_pkts", kDefCols, 5);
        ImGui::BeginChild("##pcapchild", ImVec2(0.0f, Scaled(300.0f)),
                          ImGuiChildFlags_Borders);
        if (ImGui::BeginTable("pcap_pkts", 5, flags)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(U8(L"时间"), ImGuiTableColumnFlags_WidthFixed,
                                    w[0]);
            ImGui::TableSetupColumn(U8(L"源 → 目的"),
                                    ImGuiTableColumnFlags_WidthStretch, w[1]);
            ImGui::TableSetupColumn(U8(L"协议"), ImGuiTableColumnFlags_WidthFixed,
                                    w[2]);
            ImGui::TableSetupColumn(U8(L"长度"), ImGuiTableColumnFlags_WidthFixed,
                                    w[3]);
            ImGui::TableSetupColumn(U8(L"信息"), ImGuiTableColumnFlags_WidthStretch,
                                    w[4]);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(pktsAll_.size()));
            while (clipper.Step()) {
                for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                    const stm::PktRecord& p = pktsAll_[static_cast<size_t>(r)];
                    ImGui::PushID(r);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const bool sel = pcapHasSel_ &&
                                     pcapSel_.unixTime == p.unixTime &&
                                     pcapSel_.length == p.length &&
                                     pcapSel_.info == p.info;
                    if (ImGui::Selectable("##pktrow", sel,
                                          ImGuiSelectableFlags_SpanAllColumns)) {
                        pcapHasSel_ = true;
                        pcapSel_ = p;  // 拷贝载荷供 hex 视图（环形随时被淘汰）
                    }
                    ImGui::SameLine();
                    ImGui::TextUnformatted(U8(stm::ui3::PktClockText(p.unixTime)));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(U8(stm::ui3::PacketRowText(p)));
                    ImGui::TableNextColumn();
                    const stm::ui3::PktTone tone =
                        stm::ui3::PktProtoTone(p.proto);
                    ImGui::TextColored(
                        tone == stm::ui3::PktTone::Info
                            ? ColInfo()
                            : (tone == stm::ui3::PktTone::Warn ? ColWarn()
                                                               : ColMuted()),
                        "%s", U8(stm::ui3::PktProtoBadge(p.proto)));
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", p.length);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(
                        U8(p.info.empty() ? std::wstring(L"—") : p.info));
                    ImGui::PopID();
                }
            }
            NetColSaveWidths("pcap_pkts", 5, kPersistCols);
            ImGui::EndTable();
        }
        ImGui::EndChild();

        // 选中包详情：徽标 + 摘要 + hex/ASCII 双栏视图（≤256B 载荷全显）。
        if (pcapHasSel_) {
            const std::wstring badge = stm::ui3::PktProtoBadge(pcapSel_.proto);
            ImGui::TextColored(ColInfo(), "[%s]", U8(badge));
            ImGui::SameLine();
            ImGui::TextUnformatted(
                U8(Fmt(L"{} 字节（{}）· {}", pcapSel_.length,
                       stm::ui3::PacketRowText(pcapSel_),
                       pcapSel_.info.empty() ? std::wstring(L"—") : pcapSel_.info)));
            if (pcapSel_.payloadTruncated) {
                ImGui::TextColored(ColWarn(), "%s",
                                   U8(L"载荷已截断至 256 字节（展示上限）"));
            }
            if (pcapSel_.payloadBytes.empty()) {
                ImGui::TextDisabled("%s",
                                    U8(L"无载荷可显示（头部之外没有数据或已按协议"
                                       L"截取为空）"));
            } else {
                ImGui::BeginChild("##pkthex", ImVec2(0.0f, 190.0f),
                                  ImGuiChildFlags_Borders);
                ImGui::TextUnformatted(
                    U8(stm::ui3::HexDump(pcapSel_.payloadBytes)));
                ImGui::EndChild();
            }
        }
    }

    void RequestPcapStart(AppContext& ctx) {
        std::shared_ptr<AppContext> app = LiveP3Ctx();
        if (!app) {
            PushNote(ctx, Notification::Kind::JobFailed,
                     L"操作队列未运行，抓包未启动（应用可能正在退出）");
            return;
        }
        std::shared_ptr<PcapToggle> toggle = std::make_shared<PcapToggle>();
        pcapToggle_ = toggle;
        stm::NpcapSource* src = &PcapSourceInst();
        const std::wstring dev = pcapDevName_;
        const std::wstring bpf = Utf8ToWide(std::string(pcapBpfUtf8_));
        if (app->jobs.Submit([app, toggle, src, dev, bpf] {
                std::wstring err;
                const bool ok = src->StartCapture(dev, bpf, &err);
                std::lock_guard<std::mutex> lock(toggle->mu);
                toggle->ok = ok;
                toggle->err = std::move(err);
                toggle->ready = true;
            }) == 0) {
            pcapToggle_.reset();
            PushNote(ctx, Notification::Kind::JobFailed,
                     L"操作队列未运行，抓包未启动（应用可能正在退出）");
        }
    }

    void RequestPcapStop(AppContext& ctx) {
        std::shared_ptr<AppContext> app = LiveP3Ctx();
        if (!app) {
            PushNote(ctx, Notification::Kind::JobFailed,
                     L"操作队列未运行，抓包未停止（应用可能正在退出）");
            return;
        }
        std::shared_ptr<PcapToggle> toggle = std::make_shared<PcapToggle>();
        pcapToggle_ = toggle;
        stm::NpcapSource* src = &PcapSourceInst();
        if (app->jobs.Submit([app, toggle, src] {
                src->StopCapture();  // 幂等；join 消费线程
                std::lock_guard<std::mutex> lock(toggle->mu);
                toggle->ok = true;
                toggle->ready = true;
            }) == 0) {
            pcapToggle_.reset();
            PushNote(ctx, Notification::Kind::JobFailed,
                     L"操作队列未运行，抓包未停止（应用可能正在退出）");
        }
    }

    void PollPcapToggle(AppContext& ctx) {
        if (!pcapToggle_) return;
        bool ready = false, ok = false;
        std::wstring err;
        {
            std::lock_guard<std::mutex> lock(pcapToggle_->mu);
            ready = pcapToggle_->ready;
            ok = pcapToggle_->ok;
            err = pcapToggle_->err;
        }
        if (!ready) return;
        pcapToggle_.reset();
        if (ok) {
            pcapLastErr_.clear();
            // 新会话：清空展示（统计口径自本会话开始，与采集器一致）。
            pktsAll_.clear();
            pktsHold_.clear();
            pcapHasSel_ = false;
            PushNote(ctx, Notification::Kind::Info,
                     PcapSourceInst().Running()
                         ? Fmt(L"已开始抓包：{}", pcapDevName_)
                         : std::wstring(L"已停止抓包"));
        } else {
            // 诚实报错：pcap_geterr 原文 + 常见原因（采集层已拼接提权提示）。
            pcapLastErr_ = err;
            PushNote(ctx, Notification::Kind::JobFailed,
                     Fmt(L"抓包启动失败：{}", err));
        }
    }

    void ExportPcapCsv(AppContext& ctx) {
        if (pktsAll_.empty()) return;
        auto rows = std::make_shared<const std::vector<stm::PktRecord>>(
            pktsAll_.begin(), pktsAll_.end());
        std::shared_ptr<AppContext> app = LiveP3Ctx();
        if (!app) {
            PushNote(ctx, Notification::Kind::JobFailed,
                     L"操作队列未运行，导出未执行（应用可能正在退出）");
            return;
        }
        if (app->jobs.Submit([app, rows] {
                const std::wstring dir = NetMonCsvDir();
                if (EnsureDir(dir).empty()) {  // 契约：失败返回空串
                    PushNote(*app, Notification::Kind::JobFailed,
                             Fmt(L"创建目录失败：{}", dir));
                    return;
                }
                const std::wstring path =
                    dir + L"\\" + PcapCsvFileName(static_cast<int64_t>(std::time(nullptr)));
                std::ofstream f(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
                if (!f.is_open()) {
                    PushNote(*app, Notification::Kind::JobFailed,
                             L"无法创建 CSV 文件（被占用或无写入权限）");
                    return;
                }
                f << "\xEF\xBB\xBF";  // UTF-8 BOM：Excel 直接双击可读
                f << WideToUtf8(BuildPcapCsv(*rows));
                f.flush();
                const bool ok = f.good();
                f.close();
                if (ok) {
                    PushNote(*app, Notification::Kind::JobDone,
                             Fmt(L"已导出 {} 条包元数据（不含载荷）：{}", rows->size(),
                                 path));
                } else {
                    PushNote(*app, Notification::Kind::JobFailed, L"写入 CSV 失败（磁盘错误）");
                }
            }) == 0) {
            PushNote(ctx, Notification::Kind::JobFailed,
                     L"操作队列未运行，导出未执行（应用可能正在退出）");
        }
    }

    // C2 深度抓包状态。
    AsyncFetch<std::vector<stm::PcapDevice>> pcapDevices_{10.0};
    const DeviceResult* lastPcapDevices_ = nullptr;
    std::shared_ptr<PcapToggle> pcapToggle_;  // 非 null = 启停在途
    std::vector<stm::PcapDevice> pcapDevs_;   // 下拉展示副本
    int pcapDevIndex_ = -1;
    std::wstring pcapDevName_;      // 选中的设备名（\\Device\\NPF\\...）
    char pcapBpfUtf8_[256] = {};    // BPF 输入框（UTF-8）
    std::deque<stm::PktRecord> pktsAll_;  // 最新在前；容量 kPktRingCap
    std::vector<stm::PktRecord> pktsHold_;  // 暂停期间积压（恢复并入）
    bool pcapPaused_ = false;
    bool pcapHasSel_ = false;
    stm::PktRecord pcapSel_;
    bool npcapChecked_ = false;
    bool npcapPresent_ = false;
    std::wstring pcapLastErr_;  // StartCapture 失败原文（行内诚实展示）

    // D5 实时监视状态。
    std::shared_ptr<NetMonToggle> evToggle_;      // 非 null = 开关在途
    std::shared_ptr<NetMonToggle> trafficToggle_;
    std::shared_ptr<NetMonToggle> dnsToggle_;
    bool evOn_ = false;             // 复选框镜像（诚实读回）
    bool trafficOn_ = false;
    bool dnsOn_ = false;
    bool dnsAutoDisabled_ = false;  // 契约的自动禁用已发生（提示一次）
    bool paused_ = false;           // 暂停显示：冻结视图重建，后台照常记录
    std::deque<stm::ConnEvent> evAll_;    // 最新在前；容量 kNetMonDisplayCap
    std::deque<stm::DnsEvent> dnsAll_;    // 最新在前；容量 kNetMonDnsCap
    uint64_t evGen_ = 0;            // 任一入队/清空即递增（视图重建判据）
    uint64_t dnsGen_ = 0;
    uint64_t viewEvGen_ = ~0ull;
    uint64_t viewDnsGen_ = ~0ull;
    EventFilter monFilter_;         // 控件当前值
    EventFilter monAppliedFilter_;  // 上次重建视图时应用的值
    std::vector<stm::ConnEvent> viewEvents_;  // 冻结/展示用过滤视图
    std::vector<stm::DnsEvent> viewDns_;
    char procFilterUtf8_[128] = {};
    char remoteFilterUtf8_[128] = {};
    int protoFilter_ = 0;           // ProtoFilter 全部/TCP/UDP
    bool kindNew_ = true;
    bool kindClosed_ = true;
    bool kindState_ = true;
    AsyncFetch<std::vector<stm::RemoteTraffic>> topFetch_{2.0};

    // ---- A2：适配器区 -------------------------------------------------------
    // EnumAdaptersNet 是阻塞调用（毫秒级），只允许在任务工作线程上执行；
    // 这里经 ui3::AsyncFetch 走共享 jobs 队列（>=10s 最小间隔 + 手动刷新）。
    using AdapterResult = AsyncFetch<std::vector<stm::AdapterNetInfo>>::Result;
    static std::vector<stm::AdapterNetInfo> AdapterProduce(std::wstring* err) {
        return stm::EnumAdaptersNet(err);
    }

    // P-A：头行（标签 + 数量 + 手动刷新一行；定高行：溢出提示用 SameLine，
    // 行数恒定）+ 分隔线 —— 绘制在 ##netscroll 滚动区**之外**，y 恒定是
    // 「头行不随内容滚走」契约的固定段。
    void DrawAdapterHeader() {
        adapters_.MaybeFetch(AdapterProduce, false);
        std::shared_ptr<const AdapterResult> res = adapters_.Peek();

        ImGui::TextUnformatted(U8(L"适配器"));
        ImGui::SameLine();
        if (res != nullptr) {
            size_t shown = 0, up = 0;
            for (const stm::AdapterNetInfo& a : res->data) {
                if (a.isLoopback) continue;  // 回环伪接口不计入
                ++shown;
                if (a.up) ++up;
            }
            ImGui::TextDisabled("%s", U8(Fmt(L"{} 个（{} 个已连接）", shown, up)));
        } else {
            ImGui::TextColored(ColMuted(), "%s", U8(L"统计加载中…"));
        }
        // 标签必须区别于工具栏的「刷新」按钮：同一 ImGui 窗口内
        // 同名按钮会撞 ID（两个按钮同时触发）。
        ImGui::SameLine();
        if (ImGui::SmallButton(U8(L"刷新适配器"))) {
            adapters_.MaybeFetch(AdapterProduce, true);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                              U8(L"手动刷新：立即重新枚举适配器（可越过 10 秒最小间隔；"
                                 L"自动刷新受该间隔限制）"));
        }
        // M2：卡片可能超出下方定高区 —— 行内提示（SameLine：出现/消失
        // 不增减行数，头部行高恒定是「顶栏不移动」契约的一部分）。
        // U1：比较基准 = 实际生效的卡区高（拖拽值；首帧回退原口径）。
        if (res != nullptr && AdapterCardsOverflowHint(*res, AdapterHintRegionH())) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", U8(L"卡片较多，在列表框内滚动查看"));
        }
        ImGui::Separator();
    }

    // P-A：适配器卡区（原 DrawAdapterSection 的定高段）—— 现绘制在
    // ##netscroll 滚动区**内部**。M2（顶栏位置固定）：卡区仍为定高
    // 滚动区 —— 适配器数量增减、「更多适配器」展开/折叠、加载/错误态都
    // 只改变区内滚动量。U1：高度 = regionH（cfg netAdapterH 拖拽值经
    // NetRegionHeights 分配/压缩；调用方每帧在滚动区顶部一次性算好，
    // 无拖拽的帧逐位恒定）。
    void DrawAdapterRegion(float regionH) {
        std::shared_ptr<const AdapterResult> res = adapters_.Peek();
        ImGui::BeginChild("##adapters", ImVec2(0.0f, regionH),
                          ImGuiChildFlags_Borders);
        if (res == nullptr) {
            DrawLoading();
        } else if (!res->ok && res->data.empty()) {
            bool retry = false;
            DrawLoadError(res->err, &retry);
            if (retry) adapters_.MaybeFetch(AdapterProduce, true);
        } else {
            DrawAdapterCards(*res);
        }
        ImGui::EndChild();
    }

    // 卡区溢出行内提示的估算（公式在 PageLayout.h，ui_m2_test 覆盖）：
    // 每张非回环卡片 = 1 标题行 + 6 基线明细行 + 超出基线的额外 IPv6 行；
    // 总高含卡片间条目间距。仅决定提示文字是否出现，不参与真实布局。
    // U1：regionH = 实际生效的卡区高（拖拽值；见 AdapterHintRegionH）。
    bool AdapterCardsOverflowHint(const AdapterResult& res, float regionH) const {
        const float rowH = ImGui::GetTextLineHeightWithSpacing();
        std::vector<int> extraRows;
        extraRows.reserve(res.data.size());
        for (const stm::AdapterNetInfo& a : res.data) {
            if (a.isLoopback) continue;
            int v6 = 0;
            for (const AdapterAddressEntry& e : a.addresses) {
                if (e.family != L"IPv4" && !e.ip.empty()) ++v6;
            }
            extraRows.push_back(v6 > 0 ? v6 - 1 : 0);  // 基线明细已含 1 行 IPv6
        }
        const float totalH = AdapterCardsTotalHeight(
            rowH, extraRows.data(), static_cast<int>(extraRows.size()));
        return AdapterCardsOverflow(regionH, totalH);
    }

    // U1：头部溢出提示用的卡区高 = 最近一帧 Draw() 算好的生效高
    //（adapterRegionHintH_；头行绘制先于三区分配，滞后一帧无感知）；
    // 首帧（尚未算过）回退原口径 —— 默认定高按运行时布局缩放、极矮窗口
    // 退化为可用高（语义与改版前一致）。
    float AdapterHintRegionH() const {
        if (adapterRegionHintH_ > 0.0f) return adapterRegionHintH_;
        return AdapterRegionHeightScaled(ImGui::GetContentRegionAvail().y,
                                         ui3::LayoutScale());
    }

    // 定高卡区内部：部分失败警告 + 主/更多适配器分区（纯逻辑判定在
    // NetAdapterUi.h）+ 卡片列表。
    void DrawAdapterCards(const AdapterResult& res) {
        if (!res.err.empty()) {
            ImGui::TextColored(ColWarn(), "%s",
                               U8(Fmt(L"部分数据不可用：{}", res.err)));
        }
        // 已连接的物理适配器直接展示；媒体断开/蓝牙/虚拟等折叠进
        // 「更多适配器」；回环不显示。
        std::vector<const stm::AdapterNetInfo*> primary;
        std::vector<const stm::AdapterNetInfo*> others;
        for (const stm::AdapterNetInfo& a : res.data) {
            if (a.isLoopback) continue;
            if (a.up && IsPhyscialAdapter(a)) {
                primary.push_back(&a);
            } else {
                others.push_back(&a);
            }
        }
        const auto bySortKey = [](const stm::AdapterNetInfo* x,
                                  const stm::AdapterNetInfo* y) {
            return AdapterSortKey(*x, *y);
        };
        std::sort(primary.begin(), primary.end(), bySortKey);
        std::sort(others.begin(), others.end(), bySortKey);

        if (primary.empty() && others.empty()) {
            ImGui::TextColored(ColMuted(), "%s", U8(L"未发现网络适配器"));
            return;
        }
        const double now = ImGui::GetTime();
        for (const stm::AdapterNetInfo* a : primary) DrawAdapterCard(*a, now);
        if (!others.empty()) {
            if (ImGui::CollapsingHeader(U8(Fmt(L"更多适配器（{}）", others.size())))) {
                for (const stm::AdapterNetInfo* a : others) DrawAdapterCard(*a, now);
            }
        }
    }

    // 单个适配器卡片：首行（友好名/类型徽标/状态/链路速度/复制 IP）+
    // 明细行（IPv4 含前缀、IPv6 截断、网关、DNS、MAC、DHCP）。
    void DrawAdapterCard(const stm::AdapterNetInfo& a, double now) {
        ImGui::PushID(static_cast<int>(a.ifIndex));
        ImGui::BeginChild("##adcard", ImVec2(0.0f, 0.0f),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        ImGui::TextUnformatted(
            U8(a.friendlyName.empty() ? std::wstring(L"—") : a.friendlyName));
        if (!a.description.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", U8(a.description));  // 硬件描述
        }
        ImGui::SameLine();
        ImGui::TextColored(ColInfo(), "[%s]",
                           U8(a.typeName.empty() ? std::wstring(L"未知") : a.typeName));
        ImGui::SameLine();
        if (a.up) {
            ImGui::TextColored(ColDone(), "%s", U8(AdapterStatusLabel(a)));
        } else {
            ImGui::TextColored(ColMuted(), "%s", U8(AdapterStatusLabel(a)));
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(U8(FormatAdapterSpeed(a.linkSpeedMbps)));
        // 复制 IP：写剪贴板，按钮变「已复制」1 秒（刻意无 toast）。
        const std::wstring ip = CopyableAdapterIp(a);
        const auto flash = copyFlash_.find(a.ifIndex);
        const bool copied = flash != copyFlash_.end() && now < flash->second;
        ImGui::SameLine();
        ImGui::BeginDisabled(ip.empty());
        if (ImGui::SmallButton(copied ? U8(L"已复制") : U8(L"复制 IP"))) {
            ImGui::SetClipboardText(U8(ip));
            copyFlash_[a.ifIndex] = now + 1.0;
        }
        ImGui::EndDisabled();
        if (!ip.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", U8(Fmt(L"复制 {}", ip)));
        }

        std::vector<std::wstring> v4;
        std::vector<const stm::AdapterAddressEntry*> v6;
        for (const stm::AdapterAddressEntry& e : a.addresses) {
            if (e.family == L"IPv4") {
                v4.push_back(FormatAdapterAddress(e));
            } else if (!e.ip.empty()) {
                v6.push_back(&e);
            }
        }
        DrawDetailLine(L"IPv4", JoinOrDash(v4));
        for (const stm::AdapterAddressEntry* e : v6) {
            DrawDetailLine(L"IPv6", DisplayAdapterAddress(e->ip),
                           AdapterAddressTruncated(e->ip) ? &e->ip : nullptr);
        }
        if (v6.empty()) DrawDetailLine(L"IPv6", L"—");
        DrawDetailLine(L"网关", JoinOrDash(a.gateways));
        DrawDetailLine(L"DNS", JoinOrDash(a.dnsServers));
        DrawDetailLine(L"MAC", a.mac.empty() ? std::wstring(L"—") : a.mac);
        DrawDetailLine(L"DHCP", a.dhcpEnabled ? std::wstring(L"是") : std::wstring(L"否"));
        ImGui::EndChild();
        ImGui::PopID();
    }

    // 明细行：灰色标签 + 值；fullText 非空时悬停显示全量（IPv6 截断）。
    static void DrawDetailLine(const wchar_t* label, const std::wstring& value,
                               const std::wstring* fullText = nullptr) {
        ImGui::TextDisabled("%s", U8(label));
        ImGui::SameLine();
        ImGui::TextUnformatted(U8(value));
        if (fullText != nullptr && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", U8(*fullText));
        }
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
        // P1③：列宽 cfg 持久化（netcol_netconn_<列>）；进程名列（5）为
        // 拉伸列，不持久化。
        static constexpr float kDefCols[6] = {64.0f, 210.0f, 210.0f, 100.0f, 76.0f, 1.5f};
        static constexpr bool kPersistCols[6] = {true, true, true, true, true, false};
        float* w = NetColWidths("netconn", kDefCols, 6);
        if (!ImGui::BeginTable("netconn", 6, flags)) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(U8(L"协议"), ImGuiTableColumnFlags_WidthFixed, w[0]);
        ImGui::TableSetupColumn(U8(L"本地地址:端口"), ImGuiTableColumnFlags_WidthFixed, w[1]);
        ImGui::TableSetupColumn(U8(L"远程地址:端口"), ImGuiTableColumnFlags_WidthFixed, w[2]);
        ImGui::TableSetupColumn(U8(L"状态"), ImGuiTableColumnFlags_WidthFixed, w[3]);
        ImGui::TableSetupColumn(U8(L"PID"), ImGuiTableColumnFlags_WidthFixed, w[4]);
        ImGui::TableSetupColumn(U8(L"进程名"), ImGuiTableColumnFlags_WidthStretch, w[5]);
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
        NetColSaveWidths("netconn", 6, kPersistCols);
        ImGui::EndTable();
    }

    // 在途 ETW 开关：由 ops 任务写入，UI 线程轮询。
    struct EtwToggle {
        std::mutex mu;
        bool ready = false;
        bool actual = false;
    };

    AsyncFetch<std::vector<ConnEntry>> fetch_{2.0};
    // A2：适配器区状态（独立于连接表的缓存与过滤框）。
    AsyncFetch<std::vector<stm::AdapterNetInfo>> adapters_{10.0};
    std::map<uint64_t, double> copyFlash_;  // ifIndex -> 「已复制」截止时刻（ImGui 时间）
    std::string filterUtf8_;
    std::wstring filterWide_;
    std::wstring appliedFilter_;
    const Result* lastData_ = nullptr;
    std::vector<int> rows_;
    std::shared_ptr<EtwToggle> etwToggle_;  // null = 没有开关在途
    bool etwDesired_ = false;
    bool etw_ = false;
    bool etwLoaded_ = false;
    // U1：纵向可拖拽分栏（净高持久化 netAdapterH / netmonH）。拖动中只改
    // 两个 float 内存值（下一帧经 NetRegionHeights 生效 = 实时预览），松手
    // 才 SetDouble；文件由 main 退出时统一 cfg.Save（与 netcol_* 同口径）。
    bool heightsLoaded_ = false;  // cfg 一次性读回（与 etwLoaded_ 同模式）
    uint64_t heightsLoadedGen_ = 0;  // V34-P1-N1：布局重置代际失效
    float netAdapterH_ = ui3::kNetAdapterRegionDefH;  // 适配器卡区期望高（像素）
    float netmonH_ = ui3::kNetMonRegionDefH;          // 实时监视段期望高（像素）
    float adapterRegionHintH_ = 0.0f;  // 最近一帧适配器区生效高（溢出提示用）
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

        // 顶栏三段（工具行 / 显隐行 / LHM 区）保持原位原行数 —— 定高区
        // 之外的元素逐帧恒定是「页头位置固定」的前提。
        DrawToolbar(ctx, res.get());
        DrawVisibilityRow(ctx);
        DrawLhmSection(ctx);
        ImGui::Separator();

        // M2（顶栏位置固定）：工具行/显隐行/LHM 区/分隔线均为恒定行数，
        // 其下把分组展示区整体放进定高滚动区。P-A（用户报告②）起高度改用
        //「页高减顶栏」**精确填满**口径（PageLayout.h::FillScrollRegionHeight，
        // 扣一行条目间距防父级微滚动条）：取代 M2/P1⑤ 的 [240,600] 钳制 +
        // 内容收缩 —— 过去 600px 上限在页面可用高 > 606px 时留下的「页尾
        // 空隙」正是随主题变黑/变白的空白块（内容收缩后同样留隙）。现区高
        // 只由页高决定：页尾空隙消失；内容不足时空底与本页背景同为 ChildBg
        //（壁纸模式下同受透明推送），不再形成可辨区块；内容超出时区内滚动
        //（分组内容增减只改变区内滚动量，绝不改变区外任何元素的 y 坐标 →
        // 页头不随内容上下移动；极矮窗口也不再触发父级滚动条）。
        ImGui::BeginChild("##sensorgroups",
                          ImVec2(0.0f, ui3::FillScrollRegionHeight(
                                            ImGui::GetContentRegionAvail().y,
                                            ImGui::GetStyle().ItemSpacing.y)),
                          ImGuiChildFlags_None);
        DrawSensorBody(ctx, res);
        ImGui::EndChild();
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

    // 定高分组区内部（M2）：加载态/错误态/「说明」行也画进区内 —— 它们的
    // 出现与消失同样只影响区内滚动量，不影响区外顶栏。
    void DrawSensorBody(AppContext& ctx, const std::shared_ptr<const Result>& res) {
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
        // 显隐状态 → 分组可见集合（纯函数 PageLayout.h：本帧内的一致快照，
        // selftest ui_m2_test 覆盖同一份数学；语义与逐组 SensorGroupVisible
        // 完全一致）。
        constexpr int kGroupCount = static_cast<int>(SensorGroup::Count);
        bool visFlags[kGroupCount];
        for (int i = 0; i < kGroupCount; ++i) {
            visFlags[i] = SensorGroupVisible(ctx.cfg, static_cast<SensorGroup>(i));
        }
        const SensorGroupMask visMask = SensorGroupMaskFromFlags(visFlags, kGroupCount);
        const auto groupOn = [visMask](SensorGroup g) {
            return SensorGroupMaskHas(visMask, g);
        };
        // 组渲染分发：详细模式逐条渲染全部读数（含完整标签），精简模式为现有概要。
        const auto readings = [&](const std::vector<SensorReading>& items) {
            if (detailMode_) {
                DrawReadingsDetailed(ctx, items);
            } else {
                DrawReadings(ctx, items);
            }
        };

        // Vertical full-width groups; each 可选显示 via cfg (P2 requirement).
        if (groupOn(SensorGroup::Cpu)) {
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
        if (groupOn(SensorGroup::Gpu)) {
            BeginGroup("##grp_gpu", L"GPU", snap.gpu.size() + snap.gpus.size());
            readings(snap.gpu);
            readings(snap.gpus);
            readings(FilterLhm(lhm, LhmGroup::Gpu));
            EndGroup();
        }
        if (groupOn(SensorGroup::Mem)) {
            BeginGroup("##grp_mem", L"内存", snap.memory.size());
            readings(snap.memory);
            readings(FilterLhm(lhm, LhmGroup::Mem));
            EndGroup();
        }
        if (groupOn(SensorGroup::Disk)) {
            BeginGroup("##grp_disk", L"磁盘", snap.disks.size());
            DrawDisks(ctx, snap.disks);
            readings(FilterLhm(lhm, LhmGroup::Disk));
            EndGroup();
        }
        if (groupOn(SensorGroup::Net)) {
            BeginGroup("##grp_net", L"网络", snap.network.size());
            readings(snap.network);
            readings(FilterLhm(lhm, LhmGroup::Net));
            EndGroup();
        }
        if (groupOn(SensorGroup::Battery)) {
            BeginGroup("##grp_battery", L"电池", snap.battery.size());
            readings(snap.battery);   // NoHardware 条目 = 诚实的空态
            readings(FilterLhm(lhm, LhmGroup::Battery));
            EndGroup();
        }
        if (groupOn(SensorGroup::Fan)) {
            BeginGroup("##grp_fan", L"风扇", snap.fans.size());
            readings(snap.fans);      // 诚实的 NeedDriver 条目
            readings(FilterLhm(lhm, LhmGroup::Fan));
            EndGroup();
        }
        // G-B 的 extra 读数（不归入上述任一组的杂项；契约：空 = 未发现，整组不显示）。
        if (!snap.extra.empty() && groupOn(SensorGroup::Extra)) {
            BeginGroup("##grp_extra", L"其他", snap.extra.size());
            readings(snap.extra);
            EndGroup();
        }
        // TODO(integrator): G-B 后续如再向 collect/Sensors.h 增补字段，在本函数
        // 对应分组内追加一行 `readings(snap.<newField>);` 即可 —— 精简/详细开关、
        // 四态诚实渲染与组显隐（PageHelpers.h SensorGroup）自动生效。
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

// V32-P2-4：显式释放网络监视器（停差分线程与全部 ETW 会话）。
void stm::ui3::ShutdownNetMon() { g_netmon.reset(); }
