#pragma once
// 网络页「实时监视」区（D5 维护轮）的仅头文件纯逻辑辅助。
// 与 NetAdapterUi.h / PageHelpers.h 同一约定：刻意不依赖 ImGui 与
// 应用对象，使 stm_selftest 只链接 core+collect+ops 即可覆盖；
// 所有面向用户的字符串都是中文并以宽字符串返回，UI 在调用点经
// ui::U8() 转换。数据契约见 collect/NetMonitor.h（冻结）。
//
// 诚实边界（UI 必须展示，文案在 Pages3.cpp）：连接级监视不捕获
// 通信内容；远程聚合与 DNS 记录需要管理员权限并依赖 ETW。
#include <algorithm>
#include <cstdint>
#include <ctime>
#include <string>
#include <cwctype>
#include "collect/NetMonitor.h"
#include "collect/NetTables.h"
#include "core/FsUtil.h"
#include "core/Str.h"

namespace stm {
namespace ui3 {

// 显示层环形容量：连接事件与 collect 层 kNetEventCap 同口径（保新弃旧）；
// DNS 记录与内部 DNS 环（1024）同口径。
inline constexpr size_t kNetMonDisplayCap = 8192;
inline constexpr size_t kNetMonDnsCap = 1024;

// ---------------------------------------------------------------------------
// V28-P1-1（布局稳定化）：实时监视区定高滚动段高度。
// 「实时监视」折叠头之内的整段内容（诚实边界行、「已丢弃 N 条」/「DNS 自动
// 禁用」警告行、开关行、过滤行、连接事件表、Top 远程目标表、DNS 表）包进
// 一个定高滚动 Child：警告行出现/消失、事件表空态单行↔300px 定高表互斥
// 切换、Top 表按 ETW 聚合逐行生长、Top/DNS 区随开关显隐等数据驱动的行数
// 变化全部由段内滚动消化，段外（深度抓包标题/工具栏/连接表标题）的 y 坐标
// 逐帧恒定（PageLayout.h 契约 2：出现即改变高度的行一律画进定高区内部）。
//
// 高度选型（按要求注明）：取固定 340px，而非复用传感器页
// SensorGroupsRegionHeight 的「页高减顶栏、钳 [240,600]」模式 —— 该模式
// 面向页底收尾的区（其下再无内容，avail 可直接吃满）；本段位于页中部，
// 其下还有深度抓包头、工具栏与连接表，「页高减顶栏」需要对段下元素做
// 行数估算（易碎且随 DPI 漂移）。固定值使段下布局只随窗口缩放变化，
// 内容高度绝不是输入；极矮窗口下父级滚动为已声明降级（与传感器页
// 下限行为一致）。「暂停显示」语义与折叠头行为均不变。
inline constexpr float kNetMonSectionHeight = 340.0f;

// ---------------------------------------------------------------------------
// 事件语义：标签 + 着色基调（纯逻辑层不依赖 ImGui；UI 把 Tone 映射到
// 绿（新建）/红（断开）/黄（状态变化））。
// ---------------------------------------------------------------------------
enum class EventTone { Good, Bad, Warn };

inline const wchar_t* EventKindLabel(stm::ConnEvent::Kind kind) {
    switch (kind) {
        case stm::ConnEvent::Kind::New: return L"新建";
        case stm::ConnEvent::Kind::Closed: return L"断开";
        case stm::ConnEvent::Kind::StateChanged: return L"状态";
        default: return L"—";
    }
}

inline EventTone EventToneOf(stm::ConnEvent::Kind kind) {
    switch (kind) {
        case stm::ConnEvent::Kind::New: return EventTone::Good;
        case stm::ConnEvent::Kind::Closed: return EventTone::Bad;
        case stm::ConnEvent::Kind::StateChanged:
        default: return EventTone::Warn;
    }
}

// 协议短标签（ConnEvent.proto 为 ConnProto 枚举值）。
inline const wchar_t* NetMonProtoLabel(uint32_t proto) {
    switch (static_cast<ConnProto>(proto)) {
        case ConnProto::Tcp4: return L"TCP";
        case ConnProto::Tcp6: return L"TCP6";
        case ConnProto::Udp4: return L"UDP";
        case ConnProto::Udp6: return L"UDP6";
        default: return L"—";
    }
}

inline bool MonProtoIsUdp(uint32_t proto) {
    const ConnProto p = static_cast<ConnProto>(proto);
    return p == ConnProto::Udp4 || p == ConnProto::Udp6;
}

// "addr:port"；空地址（无远端）渲染为破折号（§8 契约，与连接表一致）。
inline std::wstring MonEndpoint(const std::wstring& addr, uint16_t port) {
    if (addr.empty()) return L"—";
    return addr + L":" + std::to_wstring(static_cast<unsigned>(port));
}

// 服务名：优先远端端口（远端为 0 时退化本地端口；UDP 族查 UDP 表）。
// 未知端口返回空（调用方渲染 "—"，绝不伪造）。
inline std::wstring EventServiceName(const stm::ConnEvent& e) {
    const uint16_t port = e.remotePort != 0 ? e.remotePort : e.localPort;
    return ServiceNameForPort(port, MonProtoIsUdp(e.proto));
}

inline std::wstring EventServiceDisplay(const stm::ConnEvent& e) {
    const std::wstring svc = EventServiceName(e);
    return svc.empty() ? std::wstring(L"—") : svc;
}

// 进程列（名 + PID）：名空且 pid 0 -> 系统；名空 -> 破折号（诚实：未匹配到）。
inline std::wstring EventProcessDisplay(const stm::ConnEvent& e) {
    std::wstring name;
    if (!e.processName.empty()) {
        name = e.processName;
    } else {
        name = e.pid == 0 ? std::wstring(L"系统") : std::wstring(L"—");
    }
    return name + L"（" + std::to_wstring(e.pid) + L"）";
}

// unix 秒 -> 本地 "HH:MM:SS"（事件时间列）；无效时间戳渲染占位（不伪造）。
inline std::wstring FormatEventClock(int64_t unixTime) {
    if (unixTime <= 0) return L"--:--:--";
    const std::time_t t = static_cast<time_t>(unixTime);
    std::tm tmv{};
    if (localtime_s(&tmv, &t) != 0) return L"--:--:--";
    wchar_t buf[16] = {};
    if (swprintf_s(buf, L"%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec) <= 0) {
        return L"--:--:--";
    }
    return buf;
}

// ---------------------------------------------------------------------------
// 过滤（纯函数）：进程名包含、远程包含（匹配 "ip:端口" 全文）、
// 协议下拉（全部/TCP/UDP）、事件类型多选（按位掩码，可全空=全滤掉）。
// 两个子串字段调用前须已经 AsciiLower 小写化（与进程页同语义）。
// ---------------------------------------------------------------------------
enum class ProtoFilter : int { All = 0, Tcp = 1, Udp = 2 };

inline constexpr uint32_t kKindBitNew = 1u << 0;
inline constexpr uint32_t kKindBitClosed = 1u << 1;
inline constexpr uint32_t kKindBitState = 1u << 2;
inline constexpr uint32_t kKindMaskAll = kKindBitNew | kKindBitClosed | kKindBitState;

struct EventFilter {
    std::wstring process;  // 已小写；空 = 不过滤
    std::wstring remote;   // 已小写；空 = 不过滤
    ProtoFilter proto = ProtoFilter::All;
    uint32_t kindMask = kKindMaskAll;
    bool operator==(const EventFilter&) const = default;
};

inline std::wstring AsciiLower(const std::wstring& s) {
    std::wstring out(s.size(), L'\0');
    std::transform(s.begin(), s.end(), out.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return out;
}

inline bool EventPassesFilter(const EventFilter& f, const stm::ConnEvent& e) {
    if (!f.process.empty() && AsciiLower(e.processName).find(f.process) == std::wstring::npos) {
        return false;
    }
    if (!f.remote.empty()) {
        // 匹配完整 "ip:端口" 文本（含 IPv6）；无远端的事件不匹配非空过滤。
        if (e.remoteAddr.empty()) return false;
        const std::wstring ep =
            AsciiLower(e.remoteAddr + L":" + std::to_wstring(static_cast<unsigned>(e.remotePort)));
        if (ep.find(f.remote) == std::wstring::npos) return false;
    }
    const ConnProto p = static_cast<ConnProto>(e.proto);
    switch (f.proto) {
        case ProtoFilter::Tcp:
            if (p != ConnProto::Tcp4 && p != ConnProto::Tcp6) return false;
            break;
        case ProtoFilter::Udp:
            if (p != ConnProto::Udp4 && p != ConnProto::Udp6) return false;
            break;
        case ProtoFilter::All:
        default:
            break;
    }
    uint32_t bit = 0;
    switch (e.kind) {
        case stm::ConnEvent::Kind::New: bit = kKindBitNew; break;
        case stm::ConnEvent::Kind::Closed: bit = kKindBitClosed; break;
        case stm::ConnEvent::Kind::StateChanged: bit = kKindBitState; break;
        default: break;
    }
    return (f.kindMask & bit) != 0;
}

// ---------------------------------------------------------------------------
// CSV 导出（纯函数）：RFC 4180 转义（含逗号/引号/换行的字段加引号并把
// 内部引号翻倍）；UTF-8 BOM 由文件写入方负责（与 PerfCsv 同口径）。
// 表头 9 列：时间,事件,协议,进程,PID,本地,远程,服务,状态。
// 进程名列写原始名（匹配不到写空单元格——绝不伪造），不掺展示括号。
// ---------------------------------------------------------------------------
inline std::wstring CsvEscapeField(const std::wstring& v) {
    const bool needQuote =
        v.find(L',') != std::wstring::npos || v.find(L'"') != std::wstring::npos ||
        v.find(L'\n') != std::wstring::npos || v.find(L'\r') != std::wstring::npos;
    if (!needQuote) return v;
    std::wstring out = L"\"";
    for (wchar_t ch : v) {
        out += ch;
        if (ch == L'"') out += L'"';
    }
    out += L'"';
    return out;
}

// 表头行（测试钉死列数与顺序）。
inline std::wstring NetMonCsvHeader() {
    return L"时间,事件,协议,进程,PID,本地,远程,服务,状态";
}

template <typename Seq>
std::wstring BuildNetMonCsv(const Seq& events) {
    std::wstring csv = NetMonCsvHeader() + L"\n";
    for (const stm::ConnEvent& e : events) {
        csv += CsvEscapeField(FormatEventClock(e.unixTime));
        csv += L"," + std::wstring(EventKindLabel(e.kind));
        csv += L"," + std::wstring(NetMonProtoLabel(e.proto));
        csv += L"," + CsvEscapeField(e.processName);
        csv += L"," + std::to_wstring(e.pid);
        csv += L"," + CsvEscapeField(
                          e.localAddr.empty()
                              ? std::wstring()
                              : e.localAddr + L":" +
                                    std::to_wstring(static_cast<unsigned>(e.localPort)));
        csv += L"," + CsvEscapeField(
                          e.remoteAddr.empty()
                              ? std::wstring()
                              : e.remoteAddr + L":" +
                                    std::to_wstring(static_cast<unsigned>(e.remotePort)));
        csv += L"," + CsvEscapeField(EventServiceName(e));
        csv += L"," + CsvEscapeField(e.stateLabel);
        csv += L"\n";
    }
    return csv;
}

// 导出文件名/目录：%LOCALAPPDATA%\SuperTaskMgr\captures\netmon_<时间戳>.csv
inline std::wstring NetMonCsvFileName(int64_t unixSec) {
    const std::time_t t = static_cast<time_t>(unixSec);
    std::tm tmv{};
    if (localtime_s(&tmv, &t) != 0) return L"netmon.csv";
    wchar_t buf[48] = {};
    if (swprintf_s(buf, L"netmon_%04d%02d%02d-%02d%02d%02d.csv", tmv.tm_year + 1900,
                   tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec) <= 0) {
        return L"netmon.csv";
    }
    return buf;
}

inline std::wstring NetMonCsvDir() { return LocalAppDataRoot() + L"\\captures"; }

}  // namespace ui3
}  // namespace stm
