#pragma once
// 帧解析纯函数（C2 维护轮）：collect 层职责，UI 层不参与解析。
// 前一任曾把解析放进 ui3/PcapUi.h（违反分层且文件缺失导致全仓构建失败）；
// 现按架构裁定移入 collect 层：本头文件刻意不依赖 Windows/ImGui，
// stm_selftest 以手写合成帧逐字段断言（selftest/npcap_test.cpp）。
//
// 覆盖范围（诚实边界，与 NpcapSource.h 契约一致）：
//  - Ethernet II（含单层 VLAN 0x8100；Q-in-Q 不展开，按数字标注）
//  - IPv4 / IPv6（IPv6 扩展头不解析，按数字标注）、TCP / UDP、ICMP v4/v6
//  - Npcap 回环设备的 NULL/Loopback 伪装帧（4 字节地址族头，无 MAC）
//  - 其余协议一律 "数字(0x..)" 标注，绝不伪造名称
//  - 载荷截断：PktRecord.payloadBytes 最多 kPktPayloadCap(256) 字节，
//    payloadTruncated 诚实标注；payloadHex 为载荷前 64 字节十六进制摘要。
//
// 所有函数对畸形/截断输入安全：只读 data[0..len)，越界即降级为
// "畸形" 标注并返回已解析部分，绝不越界读。
#include "collect/NpcapSource.h"
#include "collect/NetMonitor.h"  // ServiceNameForPort（TCP/UDP 服务名摘要）
#include "core/Str.h"
#include <cstdint>
#include <string>
#include <vector>

namespace stm {

// libpcap DLT_* 常量（仅判定用；本工具只解析这两类链路层）。
inline constexpr int kDltNull = 0;      // BSD 回环伪装帧
inline constexpr int kDltEn10Mb = 1;    // Ethernet

// 消费线程按 pcap_datalink() 的返回值选择解析入口。
enum class PcapLinkKind { Ethernet, NullLoopback, Unknown };

inline PcapLinkKind PcapLinkKindFromDlt(int dlt) {
    if (dlt == kDltNull) return PcapLinkKind::NullLoopback;
    if (dlt == kDltEn10Mb) return PcapLinkKind::Ethernet;
    return PcapLinkKind::Unknown;
}

// ---------------------------------------------------------------------------
// 解析入口（三个链路层各一个）。参数 (data, caplen, tsMs)。
// ---------------------------------------------------------------------------
inline void AppendInfo(PktRecord& rec, const std::wstring& text) {
    if (rec.info.empty()) {
        rec.info = text;
    } else {
        rec.info += L"；" + text;
    }
}

inline void FillPayload(PktRecord& rec, const uint8_t* p, size_t n) {
    if (p == nullptr || n == 0) return;
    rec.payloadTruncated = n > kPktPayloadCap;
    const size_t take = n < kPktPayloadCap ? n : kPktPayloadCap;
    rec.payloadBytes.assign(p, p + take);
    const size_t hexN = take < 64 ? take : 64;
    static const wchar_t* kHex = L"0123456789ABCDEF";
    std::wstring hex;
    hex.reserve(hexN * 3 + 1);
    for (size_t i = 0; i < hexN; ++i) {
        if (i != 0) hex += L' ';
        hex += kHex[(rec.payloadBytes[i] >> 4) & 0xF];
        hex += kHex[rec.payloadBytes[i] & 0xF];
    }
    if (take > hexN) hex += L"…";  // 摘要截断（诚实标注，全文在 hex 视图）
    rec.payloadHex = std::move(hex);
}

inline std::wstring MacText(const uint8_t* m) {
    static const wchar_t* kHex = L"0123456789ABCDEF";
    std::wstring out;
    out.reserve(17);
    for (int i = 0; i < 6; ++i) {
        if (i != 0) out += L':';
        out += kHex[(m[i] >> 4) & 0xF];
        out += kHex[m[i] & 0xF];
    }
    return out;
}

inline std::wstring Ipv4Text(const uint8_t* b) {
    return Fmt(L"{}.{}.{}.{}", static_cast<unsigned>(b[0]),
               static_cast<unsigned>(b[1]), static_cast<unsigned>(b[2]),
               static_cast<unsigned>(b[3]));
}

// IPv6 文本（RFC 5952：最长零串压缩为 "::"，小写十六进制）。
inline std::wstring Ipv6Text(const uint8_t* b) {
    unsigned g[8];
    for (int i = 0; i < 8; ++i) {
        g[i] = (static_cast<unsigned>(b[i * 2]) << 8) | b[i * 2 + 1];
    }
    // 最长零串（长度 ≥2 才压缩；并列取最左）。
    int bestStart = -1, bestLen = 0;
    for (int i = 0; i < 8;) {
        if (g[i] != 0) { ++i; continue; }
        int j = i;
        while (j < 8 && g[j] == 0) ++j;
        if (j - i > bestLen) { bestStart = i; bestLen = j - i; }
        i = j;
    }
    wchar_t buf[8];  // 每组最多 4 位十六进制
    std::wstring out;
    for (int i = 0; i < 8;) {
        if (i == bestStart) {
            out += L"::";
            i += bestLen;
            continue;
        }
        if (!out.empty() && out.back() != L':') out += L':';
        const int n = swprintf_s(buf, L"%x", g[i]);
        if (n > 0) out.append(buf, static_cast<size_t>(n));
        ++i;
    }
    if (out.empty()) return L"::";  // 全零地址
    return out;
}

inline uint16_t ReadBe16(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<unsigned>(p[0]) << 8 | p[1]);
}

// 传输层解析（num=IPv4 的 protocol / IPv6 的 Next Header）。
inline void ParseTcp(PktRecord& rec, const uint8_t* b, size_t n) {
    rec.proto = L"TCP";
    if (n < 20) {
        AppendInfo(rec, L"TCP 头不完整");
        FillPayload(rec, b, n);
        return;
    }
    rec.srcPort = ReadBe16(b);
    rec.dstPort = ReadBe16(b + 2);
    const unsigned dataOff = (b[12] >> 4) * 4u;
    const unsigned flags = b[13];
    std::wstring fl;
    if (flags & 0x01) fl += L"FIN ";
    if (flags & 0x02) fl += L"SYN ";
    if (flags & 0x04) fl += L"RST ";
    if (flags & 0x08) fl += L"PSH ";
    if (flags & 0x10) fl += L"ACK ";
    if (flags & 0x20) fl += L"URG ";
    if (!fl.empty()) fl.pop_back();  // 去尾空格
    const uint32_t seq = (static_cast<uint32_t>(b[4]) << 24) | (b[5] << 16) |
                         (b[6] << 8) | b[7];
    const uint16_t win = ReadBe16(b + 14);
    const size_t payloadOff = dataOff >= 20 && dataOff < n ? dataOff : n;
    const size_t payloadLen = n - payloadOff;
    AppendInfo(rec, Fmt(L"[{}] Seq={} Win={} Len={}", fl.empty() ? L"无" : fl,
                        static_cast<unsigned long>(seq),
                        static_cast<unsigned>(win),
                        static_cast<unsigned>(payloadLen)));
    const std::wstring svc =
        ServiceNameForPort(rec.dstPort != 0 ? rec.dstPort : rec.srcPort, false);
    if (!svc.empty()) AppendInfo(rec, L"服务 " + svc);
    FillPayload(rec, b + payloadOff, payloadLen);
}

inline void ParseUdp(PktRecord& rec, const uint8_t* b, size_t n) {
    rec.proto = L"UDP";
    if (n < 8) {
        AppendInfo(rec, L"UDP 头不完整");
        FillPayload(rec, b, n);
        return;
    }
    rec.srcPort = ReadBe16(b);
    rec.dstPort = ReadBe16(b + 2);
    const uint16_t dgram = ReadBe16(b + 4);
    const size_t payloadLen = n >= 8 ? n - 8 : 0;
    const std::wstring svc =
        ServiceNameForPort(rec.dstPort != 0 ? rec.dstPort : rec.srcPort, true);
    AppendInfo(rec, Fmt(L"Len={}", static_cast<unsigned>(payloadLen)));
    if (!svc.empty()) AppendInfo(rec, L"服务 " + svc);
    if (dgram != 0 && static_cast<size_t>(dgram) != n) {
        AppendInfo(rec, Fmt(L"长度字段 {} ≠ 实际 {}", static_cast<unsigned>(dgram),
                            static_cast<unsigned>(n)));
    }
    FillPayload(rec, b + 8, payloadLen);
}

// ICMP/ICMPv6 常见类型中文名（RFC 792/4443/4861 记载，非伪造）。
inline const wchar_t* IcmpName(unsigned type, bool v6) {
    if (!v6) {
        switch (type) {
            case 0: return L"回显应答";
            case 3: return L"目标不可达";
            case 8: return L"回显请求";
            case 11: return L"超时";
            default: return nullptr;
        }
    }
    switch (type) {
        case 1: return L"目标不可达";
        case 128: return L"回显请求";
        case 129: return L"回显应答";
        case 133: return L"路由器请求";
        case 134: return L"路由器通告";
        case 135: return L"邻居请求";
        case 136: return L"邻居通告";
        default: return nullptr;
    }
}

inline void ParseIcmp(PktRecord& rec, const uint8_t* b, size_t n, bool v6) {
    rec.proto = v6 ? L"ICMPv6" : L"ICMP";
    if (n < 4) {
        AppendInfo(rec, L"ICMP 头不完整");
        FillPayload(rec, b, n);
        return;
    }
    const unsigned type = b[0];
    const unsigned code = b[1];
    std::wstring text = Fmt(L"type={} code={}", type, code);
    if (const wchar_t* name = IcmpName(type, v6)) {
        text += Fmt(L"（{}）", name);
    }
    AppendInfo(rec, std::move(text));
    // ICMP 报文头 4 字节；回显族的定长部分共 8 字节（id+seq），
    // 之后都算载荷（hex 视图口径，与常用工具一致）。
    const size_t payloadOff = n >= 8 ? 8 : 4;
    FillPayload(rec, b + payloadOff, n - payloadOff);
}

// IP 层解析（Ethernet 载荷或回环伪装帧载荷）。
inline void ParseIpPacket(PktRecord& rec, const uint8_t* ip, size_t n) {
    if (n == 0) {
        rec.proto = L"IP 畸形";
        AppendInfo(rec, L"IP 载荷为空");
        return;
    }
    const unsigned version = ip[0] >> 4;
    if (version == 4) {
        if (n < 20) {
            rec.proto = L"IP 畸形";
            AppendInfo(rec, Fmt(L"IPv4 头不完整（{} 字节）", static_cast<unsigned>(n)));
            FillPayload(rec, ip, n);
            return;
        }
        const unsigned ihl = (ip[0] & 0xF) * 4u;
        const uint16_t total = ReadBe16(ip + 2);
        const uint8_t protoNum = ip[9];
        rec.srcIp = Ipv4Text(ip + 12);
        rec.dstIp = Ipv4Text(ip + 16);
        if (ihl < 20 || static_cast<size_t>(ihl) > n) {
            rec.proto = L"IP 畸形";
            AppendInfo(rec, Fmt(L"IHL={} 超出帧长 {}", ihl / 4u,
                                static_cast<unsigned>(n)));
            FillPayload(rec, ip, n);
            return;
        }
        // 以太网最小帧有填充字节：按 IP 总长裁掉（caplen 口径仍记录全帧长）。
        size_t l3len = n;
        if (total >= ihl && static_cast<size_t>(total) < l3len) l3len = total;
        const uint8_t* body = ip + ihl;
        const size_t bodyLen = l3len - ihl;
        switch (protoNum) {
            case 6: ParseTcp(rec, body, bodyLen); break;
            case 17: ParseUdp(rec, body, bodyLen); break;
            case 1: ParseIcmp(rec, body, bodyLen, false); break;
            default:
                rec.proto = Fmt(L"{}(0x{:02X})", static_cast<unsigned>(protoNum),
                                static_cast<unsigned>(protoNum));
                FillPayload(rec, body, bodyLen);
                break;
        }
        return;
    }
    if (version == 6) {
        if (n < 40) {
            rec.proto = L"IP 畸形";
            AppendInfo(rec, Fmt(L"IPv6 头不完整（{} 字节）", static_cast<unsigned>(n)));
            FillPayload(rec, ip, n);
            return;
        }
        const uint8_t next = ip[6];
        const uint16_t payLen = ReadBe16(ip + 4);
        rec.srcIp = Ipv6Text(ip + 8);
        rec.dstIp = Ipv6Text(ip + 24);
        size_t l3len = n;
        if (static_cast<size_t>(payLen) + 40 < l3len) l3len = payLen + 40;
        const uint8_t* body = ip + 40;
        const size_t bodyLen = l3len - 40;
        switch (next) {
            case 6: ParseTcp(rec, body, bodyLen); break;
            case 17: ParseUdp(rec, body, bodyLen); break;
            case 58: ParseIcmp(rec, body, bodyLen, true); break;
            default:
                // 扩展头/未知协议：不解析（诚实数字标注），载荷从 IP 头后起。
                rec.proto = Fmt(L"{}(0x{:02X})", static_cast<unsigned>(next),
                                static_cast<unsigned>(next));
                FillPayload(rec, body, bodyLen);
                break;
        }
        return;
    }
    rec.proto = L"IP 畸形";
    AppendInfo(rec, Fmt(L"未知 IP 版本号 {}", version));
    FillPayload(rec, ip, n);
}

inline PktRecord ParseEthernetFrame(const uint8_t* data, size_t len, int64_t tsMs) {
    PktRecord rec;
    rec.unixTime = tsMs;
    rec.length = static_cast<uint32_t>(len);
    if (len < 14) {
        rec.proto = L"以太网畸形";
        AppendInfo(rec, Fmt(L"帧长 {} 字节，不足以太网头 14 字节",
                            static_cast<unsigned>(len)));
        FillPayload(rec, data, len);
        return rec;
    }
    rec.dstMac = MacText(data);
    rec.srcMac = MacText(data + 6);
    uint16_t etherType = ReadBe16(data + 12);
    size_t off = 14;
    if (etherType == 0x8100) {
        if (len < 18) {
            rec.proto = L"VLAN 畸形";
            AppendInfo(rec, L"VLAN 头不完整");
            FillPayload(rec, data + off, len - off);
            return rec;
        }
        const uint16_t vid = ReadBe16(data + 14) & 0x0FFF;
        AppendInfo(rec, Fmt(L"VLAN {}", static_cast<unsigned>(vid)));
        etherType = ReadBe16(data + 16);
        off = 18;  // 单层 VLAN（契约）；Q-in-Q 的内层 0x8100 按数字标注
    }
    if (etherType == 0x0800 || etherType == 0x86DD) {
        ParseIpPacket(rec, data + off, len - off);
        return rec;
    }
    rec.proto = Fmt(L"{}(0x{:04X})", static_cast<unsigned>(etherType),
                    static_cast<unsigned>(etherType));
    FillPayload(rec, data + off, len - off);
    return rec;
}

// Npcap 回环设备的 NULL/Loopback 伪装帧：4 字节本机序地址族头，无以太网头
//（MAC 留空 = 诚实：链路层没有 MAC 地址）。
inline PktRecord ParseLoopbackFrame(const uint8_t* data, size_t len, int64_t tsMs) {
    PktRecord rec;
    rec.unixTime = tsMs;
    rec.length = static_cast<uint32_t>(len);
    if (len < 4) {
        rec.proto = L"回环伪装帧畸形";
        AppendInfo(rec, Fmt(L"帧长 {} 字节，不足地址族头 4 字节",
                            static_cast<unsigned>(len)));
        FillPayload(rec, data, len);
        return rec;
    }
    const uint32_t family = static_cast<uint32_t>(data[0]) |
                            (static_cast<uint32_t>(data[1]) << 8) |
                            (static_cast<uint32_t>(data[2]) << 16) |
                            (static_cast<uint32_t>(data[3]) << 24);
    // Windows：AF_INET=2、AF_INET6=23；其余为 BSD 变体（24/28/30）。
    if (family == 2 || family == 23 || family == 24 || family == 28 ||
        family == 30) {
        ParseIpPacket(rec, data + 4, len - 4);
        return rec;
    }
    rec.proto = L"回环伪装帧";
    AppendInfo(rec, Fmt(L"未识别的回环地址族 {}(0x{:08X})", family, family));
    FillPayload(rec, data + 4, len - 4);
    return rec;
}

// 未知链路层：不解析（诚实），仅保留帧长与 ≤256B 原始字节供 hex 查看。
inline PktRecord ParseUnknownLinkFrame(const uint8_t* data, size_t len, int64_t tsMs,
                                       int dlt) {
    PktRecord rec;
    rec.unixTime = tsMs;
    rec.length = static_cast<uint32_t>(len);
    rec.proto = L"未知链路层";
    AppendInfo(rec, Fmt(L"DLT={}（本工具仅解析以太网与回环伪装帧）", dlt));
    FillPayload(rec, data, len);
    return rec;
}

}  // namespace stm
