#pragma once
// 深度抓包「UI 展示」纯逻辑（C2 维护轮）：仅头文件，刻意不依赖 ImGui 与
// 应用对象（与 NetMonUi.h / PageHelpers.h 同一约定，stm_selftest 直接覆盖）。
// 帧解析不在本层——那是 collect 层职责（collect/NpcapParse.h）。
// 所有面向用户的字符串都是中文并以宽字符串返回；UI 在调用点经 ui::U8() 转换。
//
// 诚实边界（文案须在 UI 呈现）：载荷仅保留前 256 字节（collect 层截断）；
// CSV 导出仅元数据列——载荷与 .pcap 文件导出明确不提供。
#include "app/ui3/NetMonUi.h"  // 复用 CsvEscapeField（RFC 4180 转义）
#include "collect/NpcapSource.h"
#include "core/Str.h"
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace stm {
namespace ui3 {

// ---------------------------------------------------------------------------
// 时间列：unix 毫秒 -> 本地 "HH:MM:SS.mmm"；无效时间戳渲染占位（不伪造）。
// ---------------------------------------------------------------------------
inline std::wstring PktClockText(int64_t unixMs) {
    if (unixMs <= 0) return L"--:--:--.---";
    const std::time_t t = static_cast<time_t>(unixMs / 1000);
    std::tm tmv{};
    if (localtime_s(&tmv, &t) != 0) return L"--:--:--.---";
    const int ms = static_cast<int>(unixMs % 1000);
    if (ms < 0 || ms > 999) return L"--:--:--.---";
    wchar_t buf[20] = {};
    if (swprintf_s(buf, L"%02d:%02d:%02d.%03d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                   ms) <= 0) {
        return L"--:--:--.---";
    }
    return buf;
}

// 端点文本：优先 IP:端口（TCP/UDP），无 IP 退 MAC，再退 "?"（诚实：无地址）。
inline std::wstring PktEndpointText(const std::wstring& ip, uint16_t port,
                                    const std::wstring& mac, bool withPort) {
    if (!ip.empty()) {
        return withPort ? ip + L":" + std::to_wstring(static_cast<unsigned>(port))
                        : ip;
    }
    if (!mac.empty()) return mac;
    return L"?";
}

// 行摘要「源 → 目的」（表格第 2 列）；非 TCP/UDP 不带端口。
inline std::wstring PacketRowText(const PktRecord& r) {
    const bool withPort = r.proto == L"TCP" || r.proto == L"UDP";
    const std::wstring src = PktEndpointText(r.srcIp, r.srcPort, r.srcMac, withPort);
    const std::wstring dst = PktEndpointText(r.dstIp, r.dstPort, r.dstMac, withPort);
    return src + L" → " + dst;
}

// ---------------------------------------------------------------------------
// 协议徽标：TCP/UDP/ICMP/ICMPv6 原样；collect 层的 "数字(0x..)" 标注压缩为
// 短徽标——IP 协议号 <256 显示 "IP 47"， EtherType 等 ≥256 显示 "0x88CC"；
// 其余（畸形标注等）原样截断到 10 字符。不伪造名称。
// ---------------------------------------------------------------------------
enum class PktTone { Info, Warn, Muted };

inline PktTone PktProtoTone(const std::wstring& proto) {
    if (proto == L"TCP" || proto == L"UDP") return PktTone::Info;
    if (proto == L"ICMP" || proto == L"ICMPv6") return PktTone::Warn;
    return PktTone::Muted;
}

inline std::wstring PktProtoBadge(const std::wstring& proto) {
    if (proto == L"TCP" || proto == L"UDP" || proto == L"ICMP" ||
        proto == L"ICMPv6") {
        return proto;
    }
    // "数字(0x..)" 形态：取括号前的数字。
    const size_t paren = proto.find(L'(');
    if (paren != std::wstring::npos && paren > 0) {
        const std::wstring digits = proto.substr(0, paren);
        bool allDigits = !digits.empty();
        for (wchar_t c : digits) {
            if (c < L'0' || c > L'9') { allDigits = false; break; }
        }
        if (allDigits) {
            // 括号内应为 "0xHHHH" 形态（collect 层 Fmt 保证；防御性再查）。
            const size_t close = proto.find(L')', paren);
            const std::wstring inner =
                close == std::wstring::npos
                    ? proto.substr(paren + 1)
                    : proto.substr(paren + 1, close - paren - 1);
            // 去前导零后判断位数（避免 "007" 之类的形态依赖）。
            const size_t nz = digits.find_first_not_of(L'0');
            const std::wstring num =
                nz == std::wstring::npos ? L"0" : digits.substr(nz);
            if (inner.size() > 2 && inner[0] == L'0' && inner[1] == L'x') {
                if (num.size() <= 3) return L"IP " + num;   // IPv4/IPv6 协议号
                return L"0x" + inner.substr(2);             // EtherType 0xHHHH
            }
        }
    }
    return proto.size() <= 10 ? proto : proto.substr(0, 10);
}

// ---------------------------------------------------------------------------
// HexDump：16 字节/行双栏（4 位偏移 + 十六进制 + |ASCII|）；不可打印字节
// 渲染 '.'；空载荷返回空串。载荷最多 kPktPayloadCap(256) 字节（collect 层
// 已截断），全量显示，无二次截断。
// ---------------------------------------------------------------------------
inline std::wstring HexDump(const std::vector<uint8_t>& bytes) {
    static const wchar_t* kHex = L"0123456789ABCDEF";
    std::wstring out;
    for (size_t row = 0; row < bytes.size(); row += 16) {
        wchar_t off[8] = {};
        swprintf_s(off, L"%04X", static_cast<unsigned>(row));
        out += off;
        out += L"  ";
        std::wstring ascii;
        for (size_t i = 0; i < 16; ++i) {
            if (i != 0) out += L' ';
            if (row + i < bytes.size()) {
                const uint8_t b = bytes[row + i];
                out += kHex[(b >> 4) & 0xF];
                out += kHex[b & 0xF];
                ascii += (b >= 0x20 && b <= 0x7E) ? static_cast<wchar_t>(b) : L'.';
            } else {
                out += L"   ";  // 对齐填充
            }
        }
        out += L"  |";
        out += ascii;
        out += L"|";
        if (row + 16 < bytes.size()) out += L'\n';
    }
    return out;
}

// 设备下拉项文本：连接名（"以太网"）优先，退硬件描述，再退内部名；
// 回环设备显式标注。信息缺失逐级退化，不伪造。
inline std::wstring PcapDeviceLabel(const PcapDevice& d) {
    std::wstring label = !d.friendlyName.empty()
                             ? d.friendlyName
                             : (!d.description.empty() ? d.description : d.name);
    if (d.loopback) label += L"（回环）";
    return label;
}

// ---------------------------------------------------------------------------
// BPF 捕获过滤器提示（输入框 tooltip）。诚实注明：这里是 BPF 语法，
// Wireshark 显示过滤器的 "ip.src==" 写法在本输入框无效（会被 pcap_compile
// 拒绝并回传原文错误）。
// ---------------------------------------------------------------------------
inline std::wstring BpfHintText() {
    return L"BPF 语法示例：tcp port 443 · udp port 53 · "
           L"src host 192.168.1.10 · net 10.0.0.0/8。"
           L"注意：Wireshark 显示过滤器的写法（如 ip.src==192.168.1.10）"
           L"在这里无效，留空表示不过滤。";
}

// ---------------------------------------------------------------------------
// CSV 导出（纯函数）：仅元数据列（时间,源,目的,协议,源端口,目的端口,长度,信息）；
// 载荷与 .pcap 导出明确不提供（诚实边界）。RFC 4180 转义复用 NetMonUi 的
// CsvEscapeField；UTF-8 BOM 由文件写入方负责。
// ---------------------------------------------------------------------------
template <typename Seq>
std::wstring BuildPcapCsv(const Seq& packets) {
    std::wstring csv = L"时间,源,目的,协议,源端口,目的端口,长度,信息\n";
    for (const PktRecord& r : packets) {
        csv += CsvEscapeField(PktClockText(r.unixTime));
        csv += L"," + CsvEscapeField(PktEndpointText(r.srcIp, r.srcPort, r.srcMac,
                                                     r.proto == L"TCP" ||
                                                         r.proto == L"UDP"));
        csv += L"," + CsvEscapeField(PktEndpointText(r.dstIp, r.dstPort, r.dstMac,
                                                     r.proto == L"TCP" ||
                                                         r.proto == L"UDP"));
        csv += L"," + CsvEscapeField(r.proto);
        csv += L"," + std::to_wstring(static_cast<unsigned>(r.srcPort));
        csv += L"," + std::to_wstring(static_cast<unsigned>(r.dstPort));
        csv += L"," + std::to_wstring(static_cast<unsigned>(r.length));
        csv += L"," + CsvEscapeField(r.info);
        csv += L"\n";
    }
    return csv;
}

// 导出文件名：%LOCALAPPDATA%\SuperTaskMgr\captures\pcap_<时间戳>.csv
inline std::wstring PcapCsvFileName(int64_t unixSec) {
    const std::time_t t = static_cast<time_t>(unixSec);
    std::tm tmv{};
    if (localtime_s(&tmv, &t) != 0) return L"pcap.csv";
    wchar_t buf[48] = {};
    if (swprintf_s(buf, L"pcap_%04d%02d%02d-%02d%02d%02d.csv", tmv.tm_year + 1900,
                   tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min,
                   tmv.tm_sec) <= 0) {
        return L"pcap.csv";
    }
    return buf;
}

}  // namespace ui3
}  // namespace stm
