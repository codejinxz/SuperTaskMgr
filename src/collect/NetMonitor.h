#pragma once
// 维护轮 10 契约：连接级实时网络监视（Wireshark 式"连接/流"层，非抓包层）。
// 架构师所有，冻结。实现见 NetMonitor.cpp。
//
// 诚实边界（UI 必须展示）：本监视器不捕获载荷内容、不覆盖 TCP/UDP 之外的协议、
// 不做混杂模式——这些需要 Npcap 等内核驱动，本应用不随包分发。
//  - 连接事件流：轮询差分 GetExtendedTcpTable/GetExtendedUdpTable（免管理员）。
//  - 按远程端点流量：扩展 ETW Kernel-Network 解析出远程地址后按端点聚合（需管理员）。
//  - DNS 解析记录：Microsoft-Windows-DNS-Client ETW（需管理员，默认关，实验性）。
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace stm {

// 连接生命周期事件（由相邻两次连接表快照差分产生）。
struct ConnEvent {
    int64_t unixTime = 0;
    enum class Kind : uint32_t { New = 0, Closed = 1, StateChanged = 2 } kind = Kind::New;
    uint32_t proto = 0;          // ConnProto 枚举值（Tcp4/Tcp6/Udp4/Udp6）
    std::wstring localAddr;
    uint16_t localPort = 0;
    std::wstring remoteAddr;     // UDP 可能为空
    uint16_t remotePort = 0;
    std::wstring stateLabel;     // 中文状态（监听/已建立/…；UDP 为"—"）
    uint32_t pid = 0;
    std::wstring processName;    // 由同刻进程快照匹配；匹配不到为空（诚实）
};

// 按远程端点聚合的流量（仅在 ETW 开启时有数据）。
struct RemoteTraffic {
    std::wstring remote;         // IP 文本
    uint16_t port = 0;
    std::wstring service;        // 知名端口服务名（443=HTTPS…），未知为空
    uint32_t pid = 0;
    std::wstring processName;
    double bytesIn = 0.0;        // 聚合窗口内字节
    double bytesOut = 0.0;
};

// DNS 解析事件（可选监听；需管理员）。
struct DnsEvent {
    int64_t unixTime = 0;
    uint32_t pid = 0;
    std::wstring processName;
    std::wstring query;          // 查询的域名
};

// 知名端口 → 服务名（纯函数：80=HTTP、443=HTTPS、53=DNS、22=SSH、3389=RDP…）。
std::wstring ServiceNameForPort(uint16_t port, bool isUdp);

class NetMonitor {
public:
    NetMonitor();
    ~NetMonitor();

    NetMonitor(const NetMonitor&) = delete;
    NetMonitor& operator=(const NetMonitor&) = delete;

    // 连接事件流：开启后以 ~1s 节拍差分连接表（免管理员）。
    bool SetEventCapture(bool on);
    bool EventCaptureEnabled() const;

    // 按远程端点流量聚合：依赖 ETW Kernel-Network（需管理员；未提权返回 false）。
    bool SetRemoteTraffic(bool on);
    bool RemoteTrafficEnabled() const;

    // DNS 解析记录（需管理员；默认关；实验性——事件字段不可靠时自动禁用并置 error）。
    bool SetDnsCapture(bool on, std::wstring* err);
    bool DnsCaptureEnabled() const;

    // UI 每帧调用：取走新增事件（环形容量 kNetEventCap，溢出丢弃并计数——诚实）。
    void DrainEvents(std::vector<ConnEvent>* out);
    void DrainDns(std::vector<DnsEvent>* out);
    std::vector<RemoteTraffic> TopRemoteTraffic(size_t topN) const;
    uint64_t DroppedEvents() const;   // 因容量丢弃的事件数（诚实展示）
    void ClearEvents();

private:
    struct Impl;
    Impl* impl_ = nullptr;            // PImpl（自持轮询/消费线程）
};

inline constexpr size_t kNetEventCap = 8192;

}  // namespace stm
