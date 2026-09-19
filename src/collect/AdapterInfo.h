#pragma once
// Phase-R 契约：网络适配器清单（任务管理器风格的 "Ethernet/WLAN"
// 页面）。布局由架构所有，由 R-NetData 实现。
// 来源：GetAdaptersAddresses（iphlpapi，有文档，无需管理员），并带
// GAA_FLAG_INCLUDE_ALL_INTERFACES，让停用/隧道/回环适配器也列出——
// 由 UI 决定展示什么，数据层保持诚实。
// 在工作线程上运行（阻塞调用，毫秒级）。一次调用返回全部适配器；
// 不执行也不需要 WSAStartup（地址转文本的选型见
// AdapterInfo.cpp）。
#include <cstdint>
#include <string>
#include <vector>

namespace stm {

// 适配器的一条单播地址（含永久 + 临时/隐私地址——原样列出，
// 过滤是 UI 策略，不是数据层的职责）。
struct AdapterAddressEntry {
    std::wstring ip;          // 点分 v4 / RFC 5952 v6 文本，不含端口/前缀
    std::wstring family;      // "IPv4" 或 "IPv6"（其他地址族被跳过）
    uint8_t prefixLen = 0;    // OnLinkPrefixLength，单位为位（按 OS 上报值）
};

struct AdapterNetInfo {
    std::wstring friendlyName;   // "以太网"/"WLAN" — user-visible connection name
    std::wstring description;    // 硬件描述（"Realtek PCIe GbE Family Controller"）
    std::wstring typeName;       // "以太网"/"Wi-Fi"/"蓝牙"/"环回"/"隧道"/"其他 (N)" (IF_TYPE map)
    uint32_t ifType = 0;         // 原始 IFTYPE_*（ifdef.h），0 = API 从未给出
    bool up = false;             // OperStatus == IfOperStatusUp
    std::wstring mac;            // "AA-BB-CC-DD-EE-FF" 十六进制；OS 报告无物理地址时为空
                                 //   （回环/隧道伪适配器）
    uint64_t linkSpeedMbps = 0;  // max(TransmitLinkSpeed, ReceiveLinkSpeed)，bits/s -> Mbps。
                                 //   刻意只用一个字段（对齐任务管理器）；多数链路
                                 //   收发对称。0 = API 未上报链路速度。
    bool dhcpEnabled = false;    // IP_ADAPTER_DHCP_ENABLED 标志（IPv4 DHCP；IPv6 有自己
                                 //   的标志，这里不合并）
    std::vector<AdapterAddressEntry> addresses;  // 单播地址（含临时地址）
    std::vector<std::wstring> gateways;          // firstGatewayAddress 链表，已去重
    std::vector<std::wstring> dnsServers;        // DnsServerList 链表，已去重
    bool isLoopback = false;     // IF_TYPE_SOFTWARE_LOOPBACK（UI 可过滤）
    uint64_t ifIndex = 0;        // 统一接口索引（与 GetIfTable2 速率联接）
};

// OS 枚举的所有适配器快照。Windows 上至少始终存在回环伪
// 接口。API 失败时列表为空，且 *err 带面向用户的中文原因；
// 成功时 *err 保持为空。
std::vector<AdapterNetInfo> EnumAdaptersNet(std::wstring* err);

// IF_TYPE_* -> Chinese label. 6 以太网 / 71 Wi-Fi / 24 环回 / 131 隧道 /
// 53 虚拟(propVirtual) / 9 令牌环 / 12 PPP / 117 IEEE 1394; anything else
// (including IF_TYPE_OTHER=1) -> "其他 (N)" with the raw number, never silently
// 误标。头内导出的纯函数，selftest 可钉住这张表；
// EnumAdaptersNet 在其上再加一条细分（蓝牙
// PAN 适配器伪装成 ifType 6——见 AdapterInfo.cpp）。
std::wstring IfTypeLabel(uint32_t ifType);

// 原地去重地址字符串（网关/DNS 列表）：保留首次出现，
// 保持顺序；比较大小写不敏感，因为 IPv6 十六进制大小写
// 不影响同一性。纯函数，为 selftest 导出。
void DedupeAddrs(std::vector<std::wstring>* v);

}  // namespace stm
