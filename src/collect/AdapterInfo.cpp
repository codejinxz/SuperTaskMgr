// AdapterInfo.cpp — 契约见 AdapterInfo.h（R-NetData）。
// 一次 GetAdaptersAddresses 调用即返回所有含单播地址的适配器、
// 网关与 DNS 服务器（iphlpapi，有文档，无需管理员，无需 WSAStartup）。
//   * 缓冲区通过 ERROR_BUFFER_OVERFLOW 重试逐步增大：适配器列表可能在
//     探测大小与实际调用之间发生变化，因此需要循环。
//   * SOCKADDR -> 文本用 InetNtopW。不用 WSAAddressToStringW，因为它要求
//     本进程先初始化 winsock（WSAStartup），而采集线程不应强加这一前提；
//     InetNtopW 是纯转换函数，无需 WSAStartup 即可工作。ws2_32.dll 动态加载
//    （stm_collect 并不链接它），与 NetTables.cpp 的做法一致。手写的点分/
//     十六进制组兜底逻辑保证即使导出函数缺失该层也能工作。
//
//   * 网关来自 firstGatewayAddress 链，DNS 来自 FirstDnsServerAddress 链；
//     两者都去重（保持顺序）。
#include <winsock2.h>  // 必须在 windows.h/iphlpapi 之前包含（LEAN_AND_MEAN 会隐藏 winsock）
#include <ws2tcpip.h>  // INET6_ADDRSTRLEN、sockaddr_in/in6 布局
#include "collect/AdapterInfo.h"
#include "core/Str.h"
#include <iphlpapi.h>
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <iterator>

namespace stm {

namespace {

constexpr DWORD kInitialBufBytes = 32 * 1024;  // MSDN 建议的初始大小
constexpr int kMaxRetries = 8;

// InetNtopW 位于 ws2_32.dll；stm_collect 不链接 ws2_32，因此动态绑定，
// 并把模块保留到进程结束（见 NetTables.cpp）。
// 选择它而非 WSAAddressToStringW，正是因为它不需要 WSAStartup。
using InetNtopWFn = PCWSTR(WINAPI*)(INT family, const VOID* pAddr, PWSTR pStringBuf,
                                    size_t StringBufSize);

InetNtopWFn InetNtopWProc() {
    static InetNtopWFn fn = []() -> InetNtopWFn {
        const HMODULE h = ::LoadLibraryW(L"ws2_32.dll");
        return h ? reinterpret_cast<InetNtopWFn>(::GetProcAddress(h, "InetNtopW")) : nullptr;
    }();
    return fn;
}

// SOCKADDR（来自 GAA 地址链表）-> 文本。非 IP 族返回 false，
// 让调用方跳过该条目，而不是编造字符串。
bool SockaddrToString(const SOCKADDR* sa, std::wstring* out) {
    if (!sa) return false;
    wchar_t buf[INET6_ADDRSTRLEN]{};  // 46 可容纳任何 v4/v6 文本
    const InetNtopWFn fn = InetNtopWProc();
    if (sa->sa_family == AF_INET) {
        const IN_ADDR& a = reinterpret_cast<const SOCKADDR_IN*>(sa)->sin_addr;
        if (fn && fn(AF_INET, &a, buf, std::size(buf))) {
            *out = buf;
            return true;
        }
        const auto* b = reinterpret_cast<const uint8_t*>(&a);  // 网络字节序
        *out = Fmt(L"{}.{}.{}.{}", b[0], b[1], b[2], b[3]);
        return true;
    }
    if (sa->sa_family == AF_INET6) {
        const IN6_ADDR& a = reinterpret_cast<const SOCKADDR_IN6*>(sa)->sin6_addr;
        if (fn && fn(AF_INET6, &a, buf, std::size(buf))) {
            *out = buf;  // RFC 5952 压缩形式
            return true;
        }
        // 兜底：小写十六进制分组（仅在 ws2_32 不可用时才会走到）。
        std::wstring s;
        for (int i = 0; i < 8; ++i) {
            if (i) s += L':';
            s += Fmt(L"{:x}", (static_cast<uint16_t>(a.u.Byte[i * 2]) << 8) | a.u.Byte[i * 2 + 1]);
        }
        *out = s;
        return true;
    }
    return false;
}

// PhysicalAddress -> "AA-BB-CC-.."。当 OS 报告没有物理地址时
//（回环/隧道伪适配器）为空——诚实留空，而非占位符。
std::wstring FmtMac(const BYTE* phys, ULONG len) {
    std::wstring s;
    for (ULONG i = 0; i < len; ++i) {
        if (i) s += L'-';
        s += Fmt(L"{:02X}", phys[i]);
    }
    return s;
}

// IPv6 十六进制大小写不影响同一性（实践中 OS 是一致的，但
// 去重不应依赖这一点）。_wcsicmp 是 MSVC CRT——本项目按 CMake 契约
// 仅支持 MSVC。
bool Ieq(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// 蓝牙 PAN 适配器上报的 ifType 是 6（以太网 CSMA/CD），但所有用户
// 都把它们当蓝牙看；这里按名称/描述子串细分。仅作用
// only on top of the 以太网 mapping so Wi-Fi names mentioning "Bluetooth" are
// 不会被改写。
bool NameHintsBluetooth(const IP_ADAPTER_ADDRESSES* a) {
    const auto hit = [](const std::wstring& s) {
        return s.find(L"Bluetooth") != std::wstring::npos ||
               s.find(L"蓝牙") != std::wstring::npos;
    };
    const std::wstring desc(a->Description ? a->Description : L"");
    const std::wstring name(a->FriendlyName ? a->FriendlyName : L"");
    return hit(desc) || hit(name);
}

}  // namespace

std::wstring IfTypeLabel(uint32_t ifType) {
    switch (ifType) {
        case IF_TYPE_ETHERNET_CSMACD: return L"以太网";   // 6
        case IF_TYPE_IEEE80211: return L"Wi-Fi";          // 71
        case IF_TYPE_SOFTWARE_LOOPBACK: return L"环回";   // 24
        case IF_TYPE_PROP_VIRTUAL: return L"虚拟";        // 53 propVirtual (RFC 2863)
        case IF_TYPE_ISO88025_TOKENRING: return L"令牌环";  // 9
        case IF_TYPE_TUNNEL: return L"隧道";              // 131 (Teredo/ISATAP/6to4)
        case IF_TYPE_PPP: return L"PPP 拨号";             // 12
        case IF_TYPE_IEEE1394: return L"IEEE 1394";       // 117
        default: return Fmt(L"其他 ({})", ifType);        // incl. IF_TYPE_OTHER=1
    }
}

void DedupeAddrs(std::vector<std::wstring>* v) {
    if (!v) return;
    std::vector<std::wstring> out;
    out.reserve(v->size());
    for (auto& s : *v) {
        if (!std::any_of(out.begin(), out.end(),
                         [&](const std::wstring& k) { return Ieq(k, s); })) {
            out.push_back(std::move(s));
        }
    }
    *v = std::move(out);
}

std::vector<AdapterNetInfo> EnumAdaptersNet(std::wstring* err) {
    std::wstring errs;
    std::vector<BYTE> buf(kInitialBufBytes);
    // SKIP_ANYCAST/SKIP_MULTICAST：契约只承载单播，因此不要让 OS
    // 构建我们随手丢弃的列表。INCLUDE_ALL_INTERFACES 让停用/隧道
    // /回环适配器保持可见；INCLUDE_GATEWAYS 把离线/不可达的网关
    // 保留在链表中而不是隐藏。
    const ULONG flags = GAA_FLAG_INCLUDE_ALL_INTERFACES | GAA_FLAG_INCLUDE_GATEWAYS |
                        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
    bool ok = false;
    for (int attempt = 0; attempt < kMaxRetries && !ok; ++attempt) {
        ULONG size = static_cast<ULONG>(buf.size());
        const ULONG rc =
            ::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                   reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
        if (rc == ERROR_SUCCESS) {
            ok = true;
            break;
        }
        if (rc == ERROR_NO_DATA) {
            errs = L"系统未报告任何网络适配器";
            break;
        }
        if (rc != ERROR_BUFFER_OVERFLOW) {
            errs = Fmt(L"GetAdaptersAddresses 失败（Win32 {}）", rc);
            break;
        }
        // 带余量增长：探测与下一次调用之间列表可能新增适配器，
        // 因此绝不要用上报的精确大小重试。
        buf.resize(static_cast<size_t>(size) * 2u);
    }

    std::vector<AdapterNetInfo> out;
    if (ok) {
        for (const auto* a = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buf.data()); a;
             a = a->Next) {
            AdapterNetInfo n;
            n.friendlyName = a->FriendlyName ? a->FriendlyName : L"";
            n.description = a->Description ? a->Description : L"";
            n.ifType = a->IfType;
            n.typeName = IfTypeLabel(a->IfType);
            if (n.typeName == L"以太网" && NameHintsBluetooth(a)) n.typeName = L"蓝牙";
            n.up = (a->OperStatus == IfOperStatusUp);
            n.isLoopback = (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK);
            n.ifIndex = a->IfIndex;
            n.mac = FmtMac(a->PhysicalAddress, a->PhysicalAddressLength);
            // 对齐任务管理器：只给一个速度数值，取 tx/rx 中较大者
            //（API 里已是 bits/s，除换算为 Mbps）。GAA 以全 1 表示"未知"——
            //（V20-P1-1）必须归一化为 0（契约：0=API 未上报），否则产生 1.8e13 Mbps 假速度。
            const uint64_t rawSpeed = std::max(a->TransmitLinkSpeed, a->ReceiveLinkSpeed);
            n.linkSpeedMbps = rawSpeed == UINT64_MAX ? 0ULL : rawSpeed / 1'000'000ULL;
            n.dhcpEnabled = (a->Flags & IP_ADAPTER_DHCP_ENABLED) != 0;

            for (const auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                AdapterAddressEntry e;
                if (!SockaddrToString(u->Address.lpSockaddr, &e.ip)) continue;  // 非 IP 族
                e.family = (u->Address.lpSockaddr->sa_family == AF_INET) ? L"IPv4" : L"IPv6";
                e.prefixLen = u->OnLinkPrefixLength;
                n.addresses.push_back(std::move(e));
            }
            for (const auto* g = a->FirstGatewayAddress; g; g = g->Next) {
                std::wstring ip;
                if (SockaddrToString(g->Address.lpSockaddr, &ip)) n.gateways.push_back(std::move(ip));
            }
            DedupeAddrs(&n.gateways);
            for (const auto* d = a->FirstDnsServerAddress; d; d = d->Next) {
                std::wstring ip;
                if (SockaddrToString(d->Address.lpSockaddr, &ip)) {
                    n.dnsServers.push_back(std::move(ip));
                }
            }
            DedupeAddrs(&n.dnsServers);
            out.push_back(std::move(n));
        }
    }
    if (err) *err = errs;
    return out;
}

}  // namespace stm
