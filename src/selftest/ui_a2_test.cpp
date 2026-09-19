// ui_a2_test：网络页「适配器」区（NetAdapterUi.h）的纯逻辑自测。
// NetAdapterUi.h 仅头文件且只依赖 core/collect 头，因此与
// ui3_test.cpp 同形：stm_selftest 内运行（链接 core+collect+ops，
// 无应用对象/无 ImGui）。
#include "selftest/TestFramework.h"
#include "app/ui3/NetAdapterUi.h"
#include <algorithm>
#include <string>
#include <vector>

// --- 链路速度：0 -> "—"；Mbps/Gbps 自适应 -------------------------------
STM_TEST(netadapter_speed_format) {
    struct Case { uint64_t mbps; const wchar_t* want; };
    static const Case cases[] = {
        {0, L"—"},                // API 未上报 -> 破折号
        {1, L"1 Mbps"},           // 低速链路保持整数
        {54, L"54 Mbps"},         // 老Wi-Fi
        {100, L"100 Mbps"},       // 快速以太网
        {999, L"999 Mbps"},       // 未跨阈值仍为 Mbps
        {1000, L"1.0 Gbps"},      // 千兆
        {2500, L"2.5 Gbps"},      // 2.5G NIC
        {10000, L"10.0 Gbps"},    // 万兆
    };
    for (const Case& c : cases) {
        const std::wstring got = stm::ui3::FormatAdapterSpeed(c.mbps);
        if (got != c.want) {
            *err = stm::Fmt(L"FormatAdapterSpeed({}) = {}，期望 {}", c.mbps, got, c.want);
            return false;
        }
    }
    return true;
}

// --- 物理启发式：回环/蓝牙/虚拟/其他 非物理；以太网/Wi-Fi 物理 ----------
STM_TEST(netadapter_is_physical) {
    stm::AdapterNetInfo nic;
    nic.typeName = L"以太网";
    if (!stm::ui3::IsPhyscialAdapter(nic)) {
        *err = L"以太网应判为物理适配器";
        return false;
    }
    nic.typeName = L"Wi-Fi";
    if (!stm::ui3::IsPhyscialAdapter(nic)) {
        *err = L"Wi-Fi 应判为物理适配器";
        return false;
    }
    // 数据层的蓝牙细分（蓝牙 PAN 伪装 ifType 6 -> typeName 蓝牙）。
    for (const wchar_t* t : {L"环回", L"蓝牙", L"其他 (1)", L"虚拟"}) {
        nic.typeName = t;
        if (stm::ui3::IsPhyscialAdapter(nic)) {
            *err = stm::Fmt(L"typeName {} 不应判为物理适配器", t);
            return false;
        }
    }
    // 回环标记兜底：即使映射词缺失（防御）也按非物理处理。
    nic.typeName = L"以太网";
    nic.isLoopback = true;
    if (stm::ui3::IsPhyscialAdapter(nic)) {
        *err = L"isLoopback=true 不应判为物理适配器";
        return false;
    }
    return true;
}

// --- 排序键：up 优先 -> 物理在前 -> 友好名字典序 --------------------------
STM_TEST(netadapter_sort_key) {
    auto mk = [](const wchar_t* name, bool up, const wchar_t* type) {
        stm::AdapterNetInfo a;
        a.friendlyName = name;
        a.up = up;
        a.typeName = type;
        return a;
    };
    std::vector<stm::AdapterNetInfo> v{
        mk(L"WLAN", false, L"Wi-Fi"),      // 断开的物理
        mk(L"以太网 2", true, L"以太网"),  // up 物理，名字典序在后
        mk(L"Loopback", true, L"环回"),    // up 非物理
        mk(L"以太网", true, L"以太网"),    // up 物理，字典序在前
        mk(L"蓝牙网络连接", true, L"蓝牙"),
    };
    std::sort(v.begin(), v.end(), stm::ui3::AdapterSortKey);
    // 期望：up+物理（以太网、以太网 2 按名序）-> up 非物理（"Loopback"
    // 的 ASCII 码位小于"蓝牙…"，宽字符按码位字典序）-> 断开的物理。
    const wchar_t* want[] = {L"以太网", L"以太网 2", L"Loopback", L"蓝牙网络连接", L"WLAN"};
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].friendlyName != want[i]) {
            *err = stm::Fmt(L"排序位置 {} 应为 {}，实际 {}", i, want[i], v[i].friendlyName);
            return false;
        }
    }
    // 严格弱序抽检：相同键元素互不“小于”。
    const stm::AdapterNetInfo a = mk(L"以太网", true, L"以太网");
    const stm::AdapterNetInfo b = mk(L"以太网", true, L"以太网");
    if (stm::ui3::AdapterSortKey(a, b) || stm::ui3::AdapterSortKey(b, a)) {
        *err = L"等值元素应互不小于（严格弱序）";
        return false;
    }
    return true;
}

// --- 复制 IP 的选取：首个 IPv4 优先 -> 首条任意 -> 空 ---------------------
STM_TEST(netadapter_copy_pick) {
    stm::AdapterNetInfo a;
    if (!stm::ui3::CopyableAdapterIp(a).empty()) {
        *err = L"无地址的适配器应返回空串（按钮禁用）";
        return false;
    }
    stm::AdapterAddressEntry v6a{L"fe80::1", L"IPv6", 64};
    stm::AdapterAddressEntry v4{L"192.168.1.5", L"IPv4", 24};
    stm::AdapterAddressEntry v6b{L"fd00::1", L"IPv6", 64};
    a.addresses = {v6a, v4, v6b};
    if (stm::ui3::CopyableAdapterIp(a) != L"192.168.1.5") {
        *err = L"有 IPv4 时必须复制首个 IPv4";
        return false;
    }
    a.addresses = {v6a, v6b};
    if (stm::ui3::CopyableAdapterIp(a) != L"fe80::1") {
        *err = L"无 IPv4 时应退回首条地址（首个 IPv6）";
        return false;
    }
    return true;
}

// --- 地址行格式：前缀长度、IPv6 截断、列表破折号 --------------------------
STM_TEST(netadapter_address_display) {
    const stm::AdapterAddressEntry v4{L"192.168.1.5", L"IPv4", 24};
    if (stm::ui3::FormatAdapterAddress(v4) != L"192.168.1.5/24") {
        *err = L"IPv4 应带前缀长度 /24";
        return false;
    }
    const stm::AdapterAddressEntry noPrefix{L"10.0.0.1", L"IPv4", 0};
    if (stm::ui3::FormatAdapterAddress(noPrefix) != L"10.0.0.1") {
        *err = L"prefixLen=0（数据缺省）不应伪造 /0";
        return false;
    }
    // 短地址原样；超 39 字符截断为 39+省略号，且原串仍可给 tooltip。
    const std::wstring shortIp = L"fe80::1";
    if (stm::ui3::DisplayAdapterAddress(shortIp) != shortIp ||
        stm::ui3::AdapterAddressTruncated(shortIp)) {
        *err = L"未超长的地址不应截断";
        return false;
    }
    std::wstring longIp = L"fe80::";
    for (int i = 0; i < 8; ++i) longIp += L"1234:";  // 远超 39 字符
    const std::wstring shown = stm::ui3::DisplayAdapterAddress(longIp);
    if (!stm::ui3::AdapterAddressTruncated(longIp) || shown.size() != 39 + 1 ||
        shown.substr(0, 39) != longIp.substr(0, 39) || shown.back() != L'…') {
        *err = L"超长地址应截断为前 39 字符+省略号";
        return false;
    }
    if (stm::ui3::JoinOrDash({}) != L"—") {
        *err = L"空网关/DNS 列表应渲染为 —";
        return false;
    }
    if (stm::ui3::JoinOrDash({L"192.168.1.1", L"fe80::1"}) != L"192.168.1.1、fe80::1") {
        *err = L"列表应以「、」连接并保持顺序";
        return false;
    }
    return true;
}
