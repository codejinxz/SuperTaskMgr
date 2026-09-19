#pragma once
// 网络页「适配器」区（A2，任务管理器风格的 以太网/WLAN 展示）的
// 仅头文件纯逻辑辅助。与 PageHelpers.h 同一约定：刻意不依赖
// ImGui 与应用对象，使 stm_selftest 只链接 core+collect+ops 即可
// 覆盖；所有面向用户的字符串都是中文并以宽字符串返回，UI 在
// 调用点经 ui::U8() 转换。数据契约见 collect/AdapterInfo.h。
#include <cstdint>
#include <string>
#include <vector>
#include "collect/AdapterInfo.h"
#include "core/Str.h"

namespace stm {
namespace ui3 {

// ---------------------------------------------------------------------------
// 链路速度：0 = API 未上报 -> "—"（§8 破折号契约）；>=1000 Mbps 以
// Gbps 一位小数自适应（1000 -> "1.0 Gbps"、2500 -> "2.5 Gbps"），
// 低于千兆保持整数 Mbps（100 -> "100 Mbps"）。
// ---------------------------------------------------------------------------
inline std::wstring FormatAdapterSpeed(uint64_t mbps) {
    if (mbps == 0) return L"—";
    if (mbps >= 1000) {
        return Fmt(L"{:.1f} Gbps", static_cast<double>(mbps) / 1000.0);
    }
    return Fmt(L"{} Mbps", mbps);
}

// ---------------------------------------------------------------------------
// 物理适配器启发式（"更多适配器"折叠判据之一）：回环、蓝牙、
// 虚拟（IF_TYPE_PROP_VIRTUAL，Hyper-V 虚拟交换机）与无法归类的
// "其他 (N)" 一律视为非物理。只看 typeName（数据层的 IF_TYPE 映射
// 已保证词汇表），不看友好名/描述——避免把用户自命名的真实网卡
// 误杀。隧道/PPP 等仍算"可展示"，落进折叠区而不是被隐藏。
// ---------------------------------------------------------------------------
inline bool IsPhyscialAdapter(const AdapterNetInfo& a) {
    if (a.isLoopback) return false;
    static const wchar_t* const kNonPhysical[] = {L"环回", L"蓝牙", L"虚拟", L"其他"};
    for (const wchar_t* kw : kNonPhysical) {
        if (a.typeName.find(kw) != std::wstring::npos) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 展示排序键（严格弱序比较器）：up 优先 -> 物理在前 -> 友好名字典序
// 兜底（保证顺序稳定、可被 selftest 钉死）。
// ---------------------------------------------------------------------------
inline bool AdapterSortKey(const AdapterNetInfo& a, const AdapterNetInfo& b) {
    if (a.up != b.up) return a.up;  // 已连接的在前
    const bool pa = IsPhyscialAdapter(a);
    const bool pb = IsPhyscialAdapter(b);
    if (pa != pb) return pa;  // 物理适配器在前
    return a.friendlyName < b.friendlyName;
}

// ---------------------------------------------------------------------------
// "复制 IP" 的取值：首个 IPv4 优先；没有 v4 时退回首条任意地址
//（含 IPv6）；完全没有地址（媒体断开等）返回空串 -> 调用方禁用按钮。
// ---------------------------------------------------------------------------
inline std::wstring CopyableAdapterIp(const AdapterNetInfo& a) {
    const std::wstring* firstAny = nullptr;
    for (const AdapterAddressEntry& e : a.addresses) {
        if (e.family == L"IPv4") return e.ip;
        if (firstAny == nullptr) firstAny = &e.ip;
    }
    return firstAny != nullptr ? *firstAny : std::wstring();
}

// ---------------------------------------------------------------------------
// 地址行文本：携带 OS 上报的 OnLinkPrefixLength（"192.168.1.5/24"）；
// prefixLen == 0（数据缺省）时不画 "/0" 的假前缀。
// ---------------------------------------------------------------------------
inline std::wstring FormatAdapterAddress(const AdapterAddressEntry& e) {
    if (e.ip.empty()) return L"—";
    if (e.prefixLen == 0) return e.ip;
    return e.ip + L"/" + std::to_wstring(static_cast<unsigned>(e.prefixLen));
}

// IPv6 全长（含 IPv4 映射形）可达 45 字符；行内只显示前 39 字符，
// 完整值放 tooltip（调用方以原始 ip 判定并展示）。
inline constexpr size_t kIpv6MaxDisplayChars = 39;

inline bool AdapterAddressTruncated(const std::wstring& ip) {
    return ip.size() > kIpv6MaxDisplayChars;
}

inline std::wstring DisplayAdapterAddress(const std::wstring& ip) {
    if (!AdapterAddressTruncated(ip)) return ip;
    return ip.substr(0, kIpv6MaxDisplayChars) + L"…";
}

// ---------------------------------------------------------------------------
// 列表展示（网关/DNS）：空 -> "—"，否则 "、" 连接（保持数据层
// DedupeAddrs 的首现顺序）。
// ---------------------------------------------------------------------------
inline std::wstring JoinOrDash(const std::vector<std::wstring>& v) {
    if (v.empty()) return L"—";
    std::wstring out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) out += L"、";
        out += v[i];
    }
    return out;
}

// 状态徽标文案：OperStatus Up -> 已连接，否则已断开（媒体断开等）。
inline std::wstring AdapterStatusLabel(const AdapterNetInfo& a) {
    return a.up ? L"已连接" : L"已断开";
}

}  // namespace ui3
}  // namespace stm
