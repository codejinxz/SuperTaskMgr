#pragma once
// 深度抓包源（C2 维护轮）：Wireshark 式包捕获，基于 Npcap 官方签名驱动。
// 契约（架构师所有，冻结）——实现见 NpcapSource.cpp。
//
// 诚实边界（UI 必须展示）：
//  - 依赖用户显式安装的 Npcap（内核级 NPCAP 服务）；本应用不随包分发驱动，
//    未安装时只展示官方下载指引（NpcapInstallGuidance）。
//  - 抓包需管理员权限（pcap_open_live 打开适配器失败时如实报错）。
//  - 载荷仅保留前 kPktPayloadCap 字节（选中包 hex 查看 / 导出仅元数据；
//    不做 .pcap 载荷导出）。
//  - 协议覆盖：Ethernet II（含单层 VLAN 0x8100）/ IPv4 / IPv6 / TCP / UDP /
//    ICMP(v4/v6)；Npcap 回环设备的 NULL/Loopback 伪装帧特判；其余协议
//    以数字标注（不伪造名称）。
//  - wpcap.dll / packet.dll 全动态加载（LoadLibrary 绝对路径），零链接依赖；
//    任何加载/导出失败都反映在 NpcapInstalled()/错误返回上，绝不半工作。
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace stm {

// 载荷保留上限：环形持有需拷贝，超过截断（诚实计数在 PktRecord.payloadTruncated）。
inline constexpr size_t kPktPayloadCap = 256;
// 环形容量：保新弃旧 + 丢弃计数（与 NetMonitor 事件环同口径语义）。
inline constexpr size_t kPktRingCap = 4096;

// 一个捕获到的包（展示行 + 选中详情）。
struct PktRecord {
    int64_t unixTime = 0;        // ms 精度（pcap_pkthdr.ts 秒+微秒合成）
    std::wstring srcIp;          // IPv4/IPv6 文本；非 IP 帧为空
    std::wstring dstIp;
    std::wstring srcMac;         // "AA:BB:CC:DD:EE:FF"；回环伪装帧为空（诚实）
    std::wstring dstMac;
    std::wstring proto;          // 中文：TCP/UDP/ICMP/ICMPv6/其他→"数字(0x..)"
    uint16_t srcPort = 0;        // 仅 TCP/UDP
    uint16_t dstPort = 0;
    uint32_t length = 0;         // 含全部头的帧长（caplen 口径）
    std::wstring info;           // 摘要：TCP 标志/UDP 服务名/ICMP 类型等
    std::wstring payloadHex;     // 前 64 字节十六进制（表格/CSV 摘要展示）
    std::vector<uint8_t> payloadBytes;  // 载荷 ≤ kPktPayloadCap（截断注明）
    bool payloadTruncated = false;
};

// 枚举出的捕获设备。
struct PcapDevice {
    std::wstring name;           // \\Device\\NPF\\{GUID}（传给 StartCapture）
    std::wstring description;    // Npcap 描述（适配器硬件名）
    std::wstring friendlyName;   // 连接名（"以太网"/"WLAN"；由 name 反查；可空）
    bool loopback = false;       // Npcap 回环设备（\Device\Npcap{"_"}Loopback 兼容两种命名）
};

// ---------------------------------------------------------------------------
// 安装检测（免管理员、无副作用；只查服务与 DLL 存在性，不加载驱动）。
// ---------------------------------------------------------------------------
// NPCAP 服务存在，或 System32\Npcap\wpcap.dll / System32\wpcap.dll 存在。
bool NpcapInstalled();
// wpcap.dll 加载候选路径：优先 System32\Npcap\wpcap.dll，回退 System32\wpcap.dll；
// 两处都不存在返回空串（诚实）。
std::wstring NpcapDllPath();
// 安装指引常量（官方下载地址 + 静默安装参数说明；UI 直接展示）。
const wchar_t* NpcapInstallGuidance();
// 官方下载页（"下载"按钮 ShellExecuteW 打开；用户主动行为）。
const wchar_t* NpcapDownloadUrl();

// 设备枚举（加载 wpcap.dll；失败/未安装时 *err 返回中文原因，返回空表）。
std::vector<PcapDevice> ListDevices(std::wstring* err);

// ---------------------------------------------------------------------------
// 抓包会话（单会话；RAII + 幂等：重复 Start 先停旧会话，Stop 幂等）。
// StartCapture 之后的包在内部消费线程解析并入环形；UI 每帧 DrainPackets。
// ---------------------------------------------------------------------------
class NpcapSource {
public:
    NpcapSource();   // 预建内部状态（不开会话；Running()==false）
    ~NpcapSource();                                  // 析构停线程并 pcap_close
    NpcapSource(const NpcapSource&) = delete;
    NpcapSource& operator=(const NpcapSource&) = delete;

    // 打开设备（snaplen 65535、混杂模式、1s 读超时）并编译应用 BPF 过滤。
    // 失败（设备打开失败/BPF 编译失败/已运行冲突由幂等化解）时 *err 返回
    // 中文原因，含 pcap_geterr 原文。
    bool StartCapture(const std::wstring& deviceName, const std::wstring& bpfFilter,
                      std::wstring* err);
    void StopCapture();                              // 幂等；join 消费线程

    bool Running() const;
    uint64_t DroppedPackets() const;                 // 环形溢出丢弃数（保新弃旧）
    // 取走环形中的包（时间升序；调用后环形清空）。UI 自行倒序做"最新在上"。
    void DrainPackets(std::vector<PktRecord>* out);
    uint64_t CapturedTotal() const;                  // 自会话开始累计捕获数（统计展示）

    // 发送原始帧（4 字节以上；pcap_sendpacket）。失败 *err 返回中文+pcap_geterr。
    // 供后续扩展；当前 UI 未用（契约完整性保留）。
    bool SendPacket(const std::wstring& deviceName, const uint8_t* bytes, size_t len,
                    std::wstring* err);

private:
    struct Impl;
    Impl* impl_ = nullptr;   // PImpl（自持消费线程与 wpcap 绑定）
};

}  // namespace stm
