// adapter_test：网络适配器数据层的自测（AdapterInfo.h）。
// 实机枚举对真实 GetAdaptersAddresses API 运行（非管理员），
// 映射/去重以纯函数钉死。
#include "selftest/TestFramework.h"
#include "collect/AdapterInfo.h"
#include "core/Str.h"
#include <string>
#include <vector>

// --- 实机枚举：>=1 适配器、err 为空、友好名非空、
//     回环标记与 IF_TYPE 一致、地址族合法 --------
STM_TEST(adapters_enum_ok) {
    std::wstring apiErr;
    const std::vector<stm::AdapterNetInfo> list = stm::EnumAdaptersNet(&apiErr);
    if (list.empty()) {
        *err = L"EnumAdaptersNet 返回空列表：" + apiErr;
        return false;
    }
    if (!apiErr.empty()) {
        *err = L"枚举成功时 err 应为空，实际：" + apiErr;
        return false;
    }
    bool sawLoopback = false;
    for (const stm::AdapterNetInfo& a : list) {
        if (a.friendlyName.empty()) {
            *err = L"适配器友好名为空：" + a.description;
            return false;
        }
        if (a.isLoopback != (a.ifType == 24 /*IF_TYPE_SOFTWARE_LOOPBACK*/)) {
            *err = L"回环标记与 IF_TYPE 不一致：" + a.friendlyName;
            return false;
        }
        if (a.isLoopback) {
            if (a.typeName != L"环回") {
                *err = L"回环适配器 typeName 应为 环回，实际：" + a.typeName;
                return false;
            }
            sawLoopback = true;
        }
        for (const stm::AdapterAddressEntry& e : a.addresses) {
            if (e.family != L"IPv4" && e.family != L"IPv6") {
                *err = L"地址 family 非法：" + e.family;
                return false;
            }
            if (e.ip.empty()) {
                *err = L"单播地址为空：" + a.friendlyName;
                return false;
            }
        }
    }
    // INCLUDE_ALL_INTERFACES 让回环伪接口在每台 Windows 上可见；
    // 它缺席说明枚举丢掉了接口。
    if (!sawLoopback) {
        *err = L"未枚举到回环适配器（INCLUDE_ALL_INTERFACES 应包含它）";
        return false;
    }
    return true;
}

// --- pure mapping table: common types + unknown -> "其他 (N)" --------------
STM_TEST(adapter_type_mapping) {
    struct Case { uint32_t type; const wchar_t* want; };
    static const Case cases[] = {
        {6, L"以太网"},        // IF_TYPE_ETHERNET_CSMACD
        {71, L"Wi-Fi"},        // IF_TYPE_IEEE80211（无线）
        {24, L"环回"},         // IF_TYPE_SOFTWARE_LOOPBACK
        {131, L"隧道"},        // IF_TYPE_TUNNEL
        {53, L"虚拟"},         // IF_TYPE_PROP_VIRTUAL
        {9, L"令牌环"},        // IF_TYPE_ISO88025
        {12, L"其他 (12)"},    // ds3 — a real but unmapped IANA type
        {23, L"PPP 拨号"},     // IF_TYPE_PPP
        {9999, L"其他 (9999)"},  // unmapped stays honest with the raw number
        {1, L"其他 (1)"},      // IF_TYPE_OTHER also lands in the honest default
    };
    for (const Case& c : cases) {
        const std::wstring got = stm::IfTypeLabel(c.type);
        if (got != c.want) {
            *err = stm::Fmt(L"IfTypeLabel({}) = {}，期望 {}", c.type, got, c.want);
            return false;
        }
    }
    return true;
}

// --- 纯去重：保持顺序、首现优先、IPv6 十六进制大小写不敏感
STM_TEST(adapter_dedupe) {
    std::vector<std::wstring> v{L"192.168.1.1", L"fe80::1A", L"192.168.1.1",
                                L"10.0.0.1", L"fe80::1a"};
    stm::DedupeAddrs(&v);
    if (v.size() != 3) {
        *err = stm::Fmt(L"去重后应剩 3 条，实际 {}", v.size());
        return false;
    }
    if (v[0] != L"192.168.1.1" || v[1] != L"fe80::1A" || v[2] != L"10.0.0.1") {
        *err = L"去重未保持首次出现顺序";
        return false;
    }
    std::vector<std::wstring> empty;
    stm::DedupeAddrs(&empty);
    if (!empty.empty()) {
        *err = L"空列表去重后应仍为空";
        return false;
    }
    std::vector<std::wstring> single{L"::1"};
    stm::DedupeAddrs(&single);
    if (single.size() != 1) {
        *err = L"单元素列表不应被改动";
        return false;
    }
    return true;
}
