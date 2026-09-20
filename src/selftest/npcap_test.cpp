// npcap_test：深度抓包（C2 维护轮）自测。
//  - npcap_detect：安装检测/设备枚举"不崩溃 + 状态输出"（不依赖 Npcap 已安装，
//    未安装机器同样全绿可复现）。
//  - pcapui_parse_*：以手写合成帧逐字段断言 collect 层 NpcapParse.h 纯函数
//   （Ethernet/TCP、VLAN 0x8100、回环 NULL 伪装帧/ICMP/IPv6/畸形输入）。
//  - pcapui_hexdump / pcapui_bpf_hint：ui3/PcapUi.h 展示纯函数（含 PacketRowText
//    与协议徽标的钉死断言）。
#include "selftest/TestFramework.h"
#include "app/ui3/PcapUi.h"
#include "collect/NpcapParse.h"
#include "collect/NpcapSource.h"
#include "core/Str.h"
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace {

// 诊断/诚实性：selftest 输出不缓冲（与 netmon_test 同口径）。
const int kNpcapUnbufferedStdout = [] {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    return 0;
}();

void PutU16BE(std::vector<uint8_t>* v, uint16_t x) {
    v->push_back(static_cast<uint8_t>(x >> 8));
    v->push_back(static_cast<uint8_t>(x));
}

void PutU32BE(std::vector<uint8_t>* v, uint32_t x) {
    v->push_back(static_cast<uint8_t>(x >> 24));
    v->push_back(static_cast<uint8_t>(x >> 16));
    v->push_back(static_cast<uint8_t>(x >> 8));
    v->push_back(static_cast<uint8_t>(x));
}

void PutBytes(std::vector<uint8_t>* v, const char* s) {
    while (*s != '\0') v->push_back(static_cast<uint8_t>(*s++));
}

// Ethernet II 头。
std::vector<uint8_t> EthHeader(const char* dst, const char* src, uint16_t etherType) {
    std::vector<uint8_t> v;
    for (int i = 0; i < 6; ++i) v.push_back(static_cast<uint8_t>(dst[i]));
    for (int i = 0; i < 6; ++i) v.push_back(static_cast<uint8_t>(src[i]));
    PutU16BE(&v, etherType);
    return v;
}

// IPv4 头（20 字节，IHL=5；不计算校验和——解析器不校验）。
std::vector<uint8_t> Ipv4Header(const uint8_t src[4], const uint8_t dst[4],
                                uint8_t proto, uint16_t totalLen) {
    std::vector<uint8_t> v;
    v.push_back(0x45);  // 版本 4 + IHL 5
    v.push_back(0x00);  // TOS
    PutU16BE(&v, totalLen);
    PutU16BE(&v, 0x1234);  // ID
    PutU16BE(&v, 0x0000);  // flags/frag
    v.push_back(64);       // TTL
    v.push_back(proto);
    PutU16BE(&v, 0x0000);  // 校验和
    for (int i = 0; i < 4; ++i) v.push_back(src[i]);
    for (int i = 0; i < 4; ++i) v.push_back(dst[i]);
    return v;
}

// IPv6 头（40 字节，无扩展头）。
std::vector<uint8_t> Ipv6Header(const uint8_t src[16], const uint8_t dst[16],
                                uint8_t next, uint16_t payloadLen) {
    std::vector<uint8_t> v;
    v.push_back(0x60);  // 版本 6
    v.push_back(0x00);
    PutU16BE(&v, 0x0000);  // 流量类/流标签
    PutU16BE(&v, payloadLen);
    v.push_back(next);
    v.push_back(64);  // 跳数限制
    for (int i = 0; i < 16; ++i) v.push_back(src[i]);
    for (int i = 0; i < 16; ++i) v.push_back(dst[i]);
    return v;
}

std::vector<uint8_t> TcpHeader(uint16_t sport, uint16_t dport, uint32_t seq,
                               uint8_t flags, uint16_t win, size_t payloadLen) {
    std::vector<uint8_t> v;
    PutU16BE(&v, sport);
    PutU16BE(&v, dport);
    PutU32BE(&v, seq);
    PutU32BE(&v, 0);  // ack
    v.push_back(0x50);  // 数据偏移 5（20 字节）
    v.push_back(flags);
    PutU16BE(&v, win);
    PutU16BE(&v, 0x0000);  // 校验和
    PutU16BE(&v, 0x0000);  // 紧急指针
    (void)payloadLen;
    return v;
}

std::vector<uint8_t> UdpHeader(uint16_t sport, uint16_t dport, uint16_t dgramLen) {
    std::vector<uint8_t> v;
    PutU16BE(&v, sport);
    PutU16BE(&v, dport);
    PutU16BE(&v, dgramLen);
    PutU16BE(&v, 0x0000);  // 校验和
    return v;
}

std::vector<uint8_t> IcmpEchoHeader(uint8_t type, uint8_t code) {
    std::vector<uint8_t> v;
    v.push_back(type);
    v.push_back(code);
    PutU16BE(&v, 0x0000);  // 校验和
    PutU16BE(&v, 0x0001);  // id
    PutU16BE(&v, 0x0007);  // seq
    return v;
}

bool Contains(const std::wstring& hay, const wchar_t* needle) {
    return hay.find(needle) != std::wstring::npos;
}

}  // namespace

// ---------------------------------------------------------------------------
// npcap_detect：检测/枚举不崩溃 + 状态输出（未安装机器也必须全绿）。
// ---------------------------------------------------------------------------
STM_TEST(npcap_detect) {
    const bool installed = stm::NpcapInstalled();
    const std::wstring dll = stm::NpcapDllPath();
    printf("[npcap_detect] NpcapInstalled=%d DllPath=%ls\n", installed ? 1 : 0,
           dll.empty() ? L"(空)" : dll.c_str());

    std::wstring listErr;
    const std::vector<stm::PcapDevice> devices = stm::ListDevices(&listErr);
    printf("[npcap_detect] ListDevices: %zu 台（err=%ls）\n", devices.size(),
           listErr.empty() ? L"(无)" : listErr.c_str());
    for (const stm::PcapDevice& d : devices) {
        if (d.name.empty()) {
            *err = L"ListDevices 返回了空设备名";
            return false;
        }
        // P1 登记（本文件外维护轮改动）：原断言误写为 "\\Device\\NPF\\"（带
        // 尾斜杠），真实 Npcap 设备名是 "\Device\NPF_{GUID}"（下划线）—— 装
        // 了 Npcap 的机器上该用例必挂。改为前缀 "\\Device\\NPF"（原意即前缀
        // 匹配），`\Device\Npcap` 旧式保留。
        if (!d.loopback && d.name.find(L"\\Device\\NPF") == std::wstring::npos &&
            d.name.find(L"\\Device\\Npcap") == std::wstring::npos) {
            *err = L"设备名不符合 NPF/Npcap 命名：" + d.name;
            return false;
        }
    }

    // 指引常量非空且指向官方站（UI 直接展示的契约）。
    if (stm::NpcapInstallGuidance() == nullptr ||
        !Contains(stm::NpcapInstallGuidance(), L"https://npcap.com/dist/")) {
        *err = L"NpcapInstallGuidance 缺少官方下载地址";
        return false;
    }
    if (std::wstring(stm::NpcapDownloadUrl()) != L"https://npcap.com/dist/") {
        *err = L"NpcapDownloadUrl 与官方地址不符";
        return false;
    }
    // 契约常量钉死（载荷截断 256B / 环形 4096 保新弃旧；constexpr 用断言）。
    static_assert(stm::kPktPayloadCap == 256, "payload cap contract (256B)");
    static_assert(stm::kPktRingCap == 4096, "ring capacity contract (4096)");
    return true;
}

// ---------------------------------------------------------------------------
// pcapui_parse_eth_tcp：合成 Ethernet/IPv4/TCP 帧逐字段断言（含行摘要、
// 徽标、载荷截断 256B）。
// ---------------------------------------------------------------------------
STM_TEST(pcapui_parse_eth_tcp) {
    // --- 基本帧：192.168.1.10:52341 -> 10.0.0.1:443，SYN，载荷 "hello!" ---
    const uint8_t srcIp[4] = {192, 168, 1, 10};
    const uint8_t dstIp[4] = {10, 0, 0, 1};
    const size_t payloadLen = 6;
    const uint16_t totalLen = static_cast<uint16_t>(20 + 20 + payloadLen);

    std::vector<uint8_t> f = EthHeader("\xAA\xBB\xCC\xDD\xEE\xF0",
                                       "\x11\x22\x33\x44\x55\x66", 0x0800);
    std::vector<uint8_t> ip = Ipv4Header(srcIp, dstIp, 6, totalLen);
    std::vector<uint8_t> tcp = TcpHeader(52341, 443, 0x11223344u, 0x02, 64240,
                                         payloadLen);
    PutBytes(&tcp, "hello!");
    f.insert(f.end(), ip.begin(), ip.end());
    f.insert(f.end(), tcp.begin(), tcp.end());

    const stm::PktRecord r = stm::ParseEthernetFrame(f.data(), f.size(), 1721385600123);
    if (r.length != f.size()) { *err = L"length ≠ caplen"; return false; }
    if (r.srcMac != L"11:22:33:44:55:66" || r.dstMac != L"AA:BB:CC:DD:EE:F0") {
        *err = L"MAC 格式化错误：" + r.srcMac + L" / " + r.dstMac;
        return false;
    }
    if (r.srcIp != L"192.168.1.10" || r.dstIp != L"10.0.0.1") {
        *err = L"IPv4 解析错误：" + r.srcIp + L" / " + r.dstIp;
        return false;
    }
    if (r.proto != L"TCP") { *err = L"proto 应为 TCP：" + r.proto; return false; }
    if (r.srcPort != 52341 || r.dstPort != 443) {
        *err = L"端口解析错误";
        return false;
    }
    if (!Contains(r.info, L"[SYN]") || !Contains(r.info, L"Seq=287454020") ||
        !Contains(r.info, L"Win=64240") || !Contains(r.info, L"Len=6")) {
        *err = L"TCP info 摘要不符：" + r.info;
        return false;
    }
    // 行摘要（ui3 纯函数钉死）：IP:端口 → IP:端口。
    if (stm::ui3::PacketRowText(r) != L"192.168.1.10:52341 → 10.0.0.1:443") {
        *err = L"PacketRowText 不符：" + stm::ui3::PacketRowText(r);
        return false;
    }
    // 徽标 + 基调（ui3 纯函数钉死）。
    if (stm::ui3::PktProtoBadge(r.proto) != L"TCP" ||
        stm::ui3::PktProtoTone(r.proto) != stm::ui3::PktTone::Info) {
        *err = L"TCP 徽标/基调不符";
        return false;
    }
    if (r.payloadTruncated || r.payloadBytes.size() != 6) {
        *err = L"载荷长度/截断标志不符";
        return false;
    }
    if (r.payloadHex != L"68 65 6C 6C 6F 21") {
        *err = L"payloadHex 不符：" + r.payloadHex;
        return false;
    }

    // --- 徽标数字标注：未知 IP 协议 47 -> "IP 47"；未知 EtherType -> "0x88CC" ---
    std::vector<uint8_t> g = EthHeader("\x01\x01\x01\x01\x01\x01",
                                       "\x02\x02\x02\x02\x02\x02", 0x0800);
    std::vector<uint8_t> gip = Ipv4Header(srcIp, dstIp, 47, 20);
    g.insert(g.end(), gip.begin(), gip.end());
    const stm::PktRecord rg = stm::ParseEthernetFrame(g.data(), g.size(), 0);
    if (stm::ui3::PktProtoBadge(rg.proto) != L"IP 47") {
        *err = L"未知 IP 协议徽标不符：" + stm::ui3::PktProtoBadge(rg.proto);
        return false;
    }
    std::vector<uint8_t> h = EthHeader("\x01\x01\x01\x01\x01\x01",
                                       "\x02\x02\x02\x02\x02\x02", 0x88CC);
    PutBytes(&h, "ll-mana");
    const stm::PktRecord rh = stm::ParseEthernetFrame(h.data(), h.size(), 0);
    if (stm::ui3::PktProtoBadge(rh.proto) != L"0x88CC") {
        *err = L"未知 EtherType 徽标不符：" + stm::ui3::PktProtoBadge(rh.proto);
        return false;
    }

    // --- 载荷截断：300B 载荷 -> 保留 256B + payloadTruncated + hex 摘要 "…" ---
    const size_t bigPayload = 300;
    const uint16_t bigTotal = static_cast<uint16_t>(20 + 20 + bigPayload);
    std::vector<uint8_t> b = EthHeader("\xAA\xAA\xAA\xAA\xAA\xAA",
                                       "\xBB\xBB\xBB\xBB\xBB\xBB", 0x0800);
    std::vector<uint8_t> bip = Ipv4Header(srcIp, dstIp, 6, bigTotal);
    std::vector<uint8_t> btcp = TcpHeader(1000, 2000, 1, 0x18, 8192, bigPayload);
    for (size_t i = 0; i < bigPayload; ++i) btcp.push_back(static_cast<uint8_t>(i & 0xFF));
    b.insert(b.end(), bip.begin(), bip.end());
    b.insert(b.end(), btcp.begin(), btcp.end());
    const stm::PktRecord rb = stm::ParseEthernetFrame(b.data(), b.size(), 0);
    if (!rb.payloadTruncated || rb.payloadBytes.size() != stm::kPktPayloadCap) {
        *err = L"载荷未按 256B 截断（诚实计数失效）";
        return false;
    }
    if (rb.payloadHex.size() != 64 * 3 - 1 + 1 || rb.payloadHex.back() != L'…') {
        *err = L"payloadHex 摘要截断标注不符";
        return false;
    }

    // --- 畸形输入不越界：13 字节残帧 ---
    const stm::PktRecord rm = stm::ParseEthernetFrame(b.data(), 13, 0);
    if (rm.proto != L"以太网畸形") { *err = L"残帧应标注畸形：" + rm.proto; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// pcapui_parse_vlan：单层 VLAN 0x8100 + UDP。
// ---------------------------------------------------------------------------
STM_TEST(pcapui_parse_vlan) {
    const uint8_t srcIp[4] = {10, 1, 2, 3};
    const uint8_t dstIp[4] = {224, 0, 0, 251};
    const uint16_t udpLen = 8 + 4;  // UDP 头 + "abcd"
    const uint16_t totalLen = 20 + udpLen;

    std::vector<uint8_t> f = EthHeader("\x01\x00\x5E\x00\x00\xFB",
                                       "\xAA\xBB\xCC\xDD\xEE\xF1", 0x8100);
    PutU16BE(&f, 0x0064);  // TCI：PRI 0、VLAN 100
    PutU16BE(&f, 0x0800);  // 内层 EtherType
    std::vector<uint8_t> ip = Ipv4Header(srcIp, dstIp, 17, totalLen);
    std::vector<uint8_t> udp = UdpHeader(5353, 5353, udpLen);
    PutBytes(&udp, "abcd");
    f.insert(f.end(), ip.begin(), ip.end());
    f.insert(f.end(), udp.begin(), udp.end());

    const stm::PktRecord r = stm::ParseEthernetFrame(f.data(), f.size(), 1);
    if (r.proto != L"UDP") { *err = L"proto 应为 UDP：" + r.proto; return false; }
    if (r.srcPort != 5353 || r.dstPort != 5353) { *err = L"VLAN 内 UDP 端口不符"; return false; }
    if (r.srcIp != L"10.1.2.3" || r.dstIp != L"224.0.0.251") {
        *err = L"VLAN 内 IPv4 不符：" + r.srcIp + L" / " + r.dstIp;
        return false;
    }
    if (!Contains(r.info, L"VLAN 100")) { *err = L"info 缺少 VLAN 标注：" + r.info; return false; }
    if (!Contains(r.info, L"Len=4")) { *err = L"info 缺少 UDP 长度：" + r.info; return false; }
    if (r.length != f.size()) { *err = L"length 应为全帧长（含 VLAN 头）"; return false; }
    if (r.payloadBytes.size() != 4 || r.payloadHex != L"61 62 63 64") {
        *err = L"VLAN 内载荷不符";
        return false;
    }
    // 双层 VLAN（Q-in-Q）不展开：内层 0x8100 按数字标注（诚实）。
    std::vector<uint8_t> q = EthHeader("\x01\x01\x01\x01\x01\x01",
                                       "\x02\x02\x02\x02\x02\x02", 0x8100);
    PutU16BE(&q, 0x00C8);  // VLAN 200
    PutU16BE(&q, 0x8100);  // 内层仍是 VLAN 标签
    PutU16BE(&q, 0x0064);
    PutU16BE(&q, 0x0800);
    const stm::PktRecord rq = stm::ParseEthernetFrame(q.data(), q.size(), 0);
    if (!Contains(rq.info, L"VLAN 200") || rq.proto != L"33024(0x8100)") {
        *err = L"Q-in-Q 应只展开一层并数字标注：" + rq.proto + L" / " + rq.info;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// pcapui_parse_loopback：NULL/Loopback 伪装帧（IPv4+ICMP / IPv6+TCP / 未知族）。
// ---------------------------------------------------------------------------
STM_TEST(pcapui_parse_loopback) {
    // (a) AF_INET(2) + IPv4 + ICMP 回显请求（ping 127.0.0.1 的真实形态）。
    const uint8_t lo[4] = {127, 0, 0, 1};
    std::vector<uint8_t> f;
    f.push_back(0x02);
    f.push_back(0x00);
    f.push_back(0x00);
    f.push_back(0x00);
    std::vector<uint8_t> ip = Ipv4Header(lo, lo, 1, 20 + 8 + 3);
    std::vector<uint8_t> icmp = IcmpEchoHeader(8, 0);
    PutBytes(&icmp, "abc");
    f.insert(f.end(), ip.begin(), ip.end());
    f.insert(f.end(), icmp.begin(), icmp.end());
    const stm::PktRecord r = stm::ParseLoopbackFrame(f.data(), f.size(), 2);
    if (r.proto != L"ICMP") { *err = L"回环 ICMP proto 不符：" + r.proto; return false; }
    if (!r.srcMac.empty() || !r.dstMac.empty()) {
        *err = L"回环伪装帧不得伪造 MAC（契约：留空）";
        return false;
    }
    if (r.srcIp != L"127.0.0.1" || r.dstIp != L"127.0.0.1") {
        *err = L"回环 IPv4 不符";
        return false;
    }
    if (!Contains(r.info, L"type=8") || !Contains(r.info, L"回显请求")) {
        *err = L"ICMP info 不符：" + r.info;
        return false;
    }
    if (r.payloadBytes.size() != 3 || r.payloadHex != L"61 62 63") {
        *err = L"ICMP 载荷应跳过 8 字节回显头";
        return false;
    }

    // (b) AF_INET6(23) + IPv6 + TCP，含 IPv6 文本压缩（::1 / fe80::1）。
    uint8_t src6[16] = {}, dst6[16] = {};
    src6[15] = 1;                       // ::1
    dst6[0] = 0xFE, dst6[1] = 0x80;     // fe80::1
    dst6[15] = 1;
    std::vector<uint8_t> g;
    g.push_back(0x17);
    g.push_back(0x00);
    g.push_back(0x00);
    g.push_back(0x00);
    std::vector<uint8_t> ip6 = Ipv6Header(src6, dst6, 6, 20 + 2);
    std::vector<uint8_t> tcp = TcpHeader(443, 52341, 7, 0x10, 32100, 2);
    PutBytes(&tcp, "xy");
    g.insert(g.end(), ip6.begin(), ip6.end());
    g.insert(g.end(), tcp.begin(), tcp.end());
    const stm::PktRecord rg = stm::ParseLoopbackFrame(g.data(), g.size(), 3);
    if (rg.proto != L"TCP") { *err = L"回环 IPv6 TCP proto 不符：" + rg.proto; return false; }
    if (rg.srcIp != L"::1" || rg.dstIp != L"fe80::1") {
        *err = L"IPv6 文本压缩不符：" + rg.srcIp + L" / " + rg.dstIp;
        return false;
    }
    if (rg.srcPort != 443 || rg.dstPort != 52341) { *err = L"IPv6 TCP 端口不符"; return false; }
    if (!Contains(rg.info, L"[ACK]")) { *err = L"TCP 标志不符：" + rg.info; return false; }

    // (c) 未知地址族：诚实标注 + 载荷保留原字节。
    std::vector<uint8_t> u = {0x99, 0x00, 0x00, 0x00, 'r', 'a', 'w'};
    const stm::PktRecord ru = stm::ParseLoopbackFrame(u.data(), u.size(), 4);
    if (ru.proto != L"回环伪装帧" || !Contains(ru.info, L"未识别的回环地址族")) {
        *err = L"未知地址族应诚实标注：" + ru.proto + L" / " + ru.info;
        return false;
    }
    if (ru.payloadHex != L"72 61 77") { *err = L"未知地址族载荷不符"; return false; }

    // (d) 残帧（<4 字节）不越界。
    const uint8_t tiny[2] = {0x02, 0x00};
    const stm::PktRecord rt = stm::ParseLoopbackFrame(tiny, sizeof(tiny), 5);
    if (rt.proto != L"回环伪装帧畸形") { *err = L"残帧应标注畸形：" + rt.proto; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// pcapui_hexdump：16 字节/行双栏（偏移 + 十六进制 + |ASCII|）。
// ---------------------------------------------------------------------------
STM_TEST(pcapui_hexdump) {
    // 空载荷。
    if (!stm::ui3::HexDump({}).empty()) { *err = L"空载荷 HexDump 应为空串"; return false; }

    // 单行 13 字节（含可打印与空格）。
    std::vector<uint8_t> msg;
    PutBytes(&msg, "Hello, World!");
    const std::wstring one = stm::ui3::HexDump(msg);
    const std::wstring wantOne =
        L"0000  48 65 6C 6C 6F 2C 20 57 6F 72 6C 64 21"
        L"              |Hello, World!|";
    if (one != wantOne) { *err = L"HexDump 单行不符：\n" + one; return false; }

    // 两行（17 字节 0x00..0x10）：不可打印渲染 '.'，第二行对齐填充。
    std::vector<uint8_t> seq;
    for (int i = 0; i <= 16; ++i) seq.push_back(static_cast<uint8_t>(i));
    const std::wstring two = stm::ui3::HexDump(seq);
    const std::wstring wantTwo =
        L"0000  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F  |................|\n"
        L"0010  10" + std::wstring(62, L' ') + L"|.|";
    if (two != wantTwo) { *err = L"HexDump 两行不符：\n" + two; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// pcapui_bpf_hint：BPF 示例 + 诚实注明 Wireshark 显示过滤器写法无效。
// ---------------------------------------------------------------------------
STM_TEST(pcapui_bpf_hint) {
    const std::wstring hint = stm::ui3::BpfHintText();
    if (!Contains(hint, L"tcp port 443") || !Contains(hint, L"udp port 53") ||
        !Contains(hint, L"src host 192.168.1.10")) {
        *err = L"BPF 提示缺少示例：" + hint;
        return false;
    }
    // 诚实边界：明确指出 ip.src== 写法无效（该写法会被 pcap_compile 拒绝）。
    if (!Contains(hint, L"ip.src==") || !Contains(hint, L"无效")) {
        *err = L"BPF 提示缺少对显示过滤器写法的诚实注明";
        return false;
    }
    if (!Contains(hint, L"留空")) { *err = L"BPF 提示缺少「留空不过滤」说明"; return false; }
    return true;
}
