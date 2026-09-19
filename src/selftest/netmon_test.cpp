// netmon_test：第 10 维护轮（契约 NetMonitor.h）自测。
// 纯函数（差分/端口表/Top 排序/环形溢出）为主；生命周期与默认关闭为
// 环境无关断言；ETW 远程字段与 DNS 解码为管理员分支的"实测探针"
//（只打印测量值 + 断言生命周期机制，不依赖外网环境，保证全绿可复现）。
#include "selftest/TestFramework.h"
#include "collect/CollectDetail.h"
#include "collect/NetMonitor.h"
#include "collect/NetTables.h"
#include "core/Privilege.h"
#include "core/Str.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

// 诊断/诚实性：selftest 输出不缓冲——即使某用例硬崩（fail-fast），
// 已完成的 PASS/FAIL 行也留在控制台/重定向流里，可定位崩溃点。
const int kNetmonUnbufferedStdout = [] {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    return 0;
}();

// 合成一条 TCP/UDP 行。
stm::ConnEntry Row(stm::ConnProto proto, const wchar_t* la, uint16_t lp, const wchar_t* ra,
                   uint16_t rp, uint32_t state, uint32_t pid) {
    stm::ConnEntry c;
    c.proto = proto;
    c.localAddr = la;
    c.localPort = lp;
    c.remoteAddr = ra;
    c.remotePort = rp;
    c.state = state;
    c.pid = pid;
    return c;
}

stm::cd::EndpointKey Key4(uint32_t pid, uint8_t a, uint8_t b, uint8_t c, uint8_t d,
                          uint16_t port, uint8_t proto) {
    stm::cd::EndpointKey k;
    k.pid = pid;
    k.family = AF_INET;
    k.port = port;
    k.proto = proto;
    k.ip[0] = a;
    k.ip[1] = b;
    k.ip[2] = c;
    k.ip[3] = d;
    return k;
}

// x64 上 htons 是 ws2_32 真实导出（selftest 不链 ws2_32）——手工换序。
inline uint16_t Swap16(uint16_t v) { return static_cast<uint16_t>((v << 8) | (v >> 8)); }
// 小端机器上内存字节恰为 a,b,c,d 的 s_addr 值（即网络字节序的 a.b.c.d）。
inline uint32_t LoopbackAddr() {
#ifdef _MSC_VER
    return 0x0100007Fu;  // 127.0.0.1 网络字节序（x64 小端）
#else
    return 0;
#endif
}

// 动态加载 ws2_32 造环回流量（stm_selftest 未链接 ws2_32，与采集库同法）。
// 返回监听端口（0=失败；失败不致命——探针仍有系统本底流量可测）；
// *stageOut 返回失败阶段名（诊断用，可为 nullptr）。端口供字节序断言。
uint16_t LoopbackEcho(const char** stageOut = nullptr) {
    const auto setStage = [stageOut](const char* s) {
        if (stageOut) *stageOut = s;
    };
    setStage("load");
    const HMODULE h = ::LoadLibraryW(L"ws2_32.dll");
    if (!h) return 0;
    const auto startup = reinterpret_cast<int(WINAPI*)(WORD, void*)>(
        ::GetProcAddress(h, "WSAStartup"));
    const auto sockFn = reinterpret_cast<SOCKET(WINAPI*)(int, int, int)>(
        ::GetProcAddress(h, "socket"));
    const auto bindFn = reinterpret_cast<int(WINAPI*)(SOCKET, const void*, int)>(
        ::GetProcAddress(h, "bind"));
    const auto listenFn = reinterpret_cast<int(WINAPI*)(SOCKET, int)>(
        ::GetProcAddress(h, "listen"));
    const auto acceptFn = reinterpret_cast<SOCKET(WINAPI*)(SOCKET, void*, int*)>(
        ::GetProcAddress(h, "accept"));
    const auto connectFn = reinterpret_cast<int(WINAPI*)(SOCKET, const void*, int)>(
        ::GetProcAddress(h, "connect"));
    const auto sendFn = reinterpret_cast<int(WINAPI*)(SOCKET, const char*, int, int)>(
        ::GetProcAddress(h, "send"));
    const auto recvFn = reinterpret_cast<int(WINAPI*)(SOCKET, char*, int, int)>(
        ::GetProcAddress(h, "recv"));
    const auto closeFn = reinterpret_cast<int(WINAPI*)(SOCKET)>(
        ::GetProcAddress(h, "closesocket"));
    const auto sockNameFn = reinterpret_cast<int(WINAPI*)(SOCKET, void*, int*)>(
        ::GetProcAddress(h, "getsockname"));
    const auto ioctlFn = reinterpret_cast<int(WINAPI*)(SOCKET, long, u_long*)>(
        ::GetProcAddress(h, "ioctlsocket"));
    if (!startup || !sockFn || !bindFn || !listenFn || !acceptFn || !connectFn || !sendFn ||
        !recvFn || !closeFn || !sockNameFn || !ioctlFn) {
        return 0;
    }
    WSADATA wsa{};
    setStage("WSAStartup");
    if (startup(0x0202, &wsa) != 0) return 0;
    bool echoed = false;
    SOCKET l = INVALID_SOCKET;
    SOCKET c = INVALID_SOCKET;
    std::thread server;  // 提前声明，保证所有退出路径 join
    auto cleanup = [&] {
        if (c != INVALID_SOCKET) closeFn(c);
        if (l != INVALID_SOCKET) closeFn(l);
        if (server.joinable()) server.join();
        const auto fin = reinterpret_cast<int(WINAPI*)()>(::GetProcAddress(h, "WSACleanup"));
        if (fin) fin();
    };
    setStage("socket");
    l = sockFn(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == INVALID_SOCKET) { cleanup(); return 0; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = LoopbackAddr();  // 127.0.0.1（网络字节序）
    u_long nonBlock = 1;                        // 监听端非阻塞：accept 有界，绝不挂死
    setStage("ioctlsocket");
    if (ioctlFn(l, FIONBIO, &nonBlock) != 0) { cleanup(); return 0; }
    setStage("bind");
    if (bindFn(l, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) { cleanup(); return 0; }
    setStage("listen");
    if (listenFn(l, 1) != 0) { cleanup(); return 0; }
    int n = sizeof addr;
    sockNameFn(l, reinterpret_cast<sockaddr*>(&addr), &n);
    const uint16_t port = Swap16(addr.sin_port);
    server = std::thread([l, acceptFn, recvFn, sendFn, closeFn] {
        for (int i = 0; i < 200; ++i) {  // 最多 ~4s
            const SOCKET s = acceptFn(l, nullptr, nullptr);
            if (s != INVALID_SOCKET) {
                char buf[2048];
                int got = 0;
                while ((got = recvFn(s, buf, sizeof buf, 0)) > 0) sendFn(s, buf, got, 0);
                closeFn(s);
                return;
            }
            ::Sleep(20);
        }
    });
    setStage("client");
    c = sockFn(AF_INET, SOCK_STREAM, IPPROTO_TCP);  // 客户端阻塞
    if (c == INVALID_SOCKET) { cleanup(); return 0; }
    // 监听端已就绪，环回 connect 立即见分晓；send/recv 阻塞、不会挂死。
    setStage("connect");
    const bool connected =
        connectFn(c, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0;
    setStage("transfer");
    if (connected) {
        char buf[2048]{};
        int rounds = 0;
        for (int i = 0; i < 64; ++i) {  // ~128KB 双向来回
            if (sendFn(c, buf, sizeof buf, 0) <= 0) break;
            if (recvFn(c, buf, sizeof buf, 0) <= 0) break;
            ++rounds;
        }
        echoed = rounds > 0;
    }
    cleanup();
    setStage(echoed ? "ok" : (connected ? "transfer" : "connect"));
    return (echoed && port != 0) ? port : static_cast<uint16_t>(0);
}

}  // namespace

// ---------------------------------------------------------------------------
STM_TEST(netmon_diff_pure) {
    using CP = stm::ConnProto;
    using K = stm::ConnEvent::Kind;
    const std::vector<stm::ConnEntry> prev = {
        Row(CP::Tcp4, L"10.0.0.1", 5000, L"1.1.1.1", 443, 5 /*ESTAB*/, 100),
        Row(CP::Tcp4, L"10.0.0.1", 5001, L"2.2.2.2", 80, 8 /*CLOSE_WAIT*/, 101),
        Row(CP::Tcp4, L"0.0.0.0", 445, L"0.0.0.0", 0, 2 /*LISTEN*/, 4),
        Row(CP::Udp4, L"10.0.0.1", 5353, L"", 0, 0, 200),
    };
    const std::vector<stm::ConnEntry> cur = {
        Row(CP::Tcp4, L"10.0.0.1", 5000, L"1.1.1.1", 443, 8 /*CLOSE_WAIT*/, 100),  // 状态变化
        Row(CP::Tcp4, L"10.0.0.1", 6000, L"3.3.3.3", 443, 5, 102),                 // 新建
        // 445 监听消失：不产事件；5001 消失：Closed
        Row(CP::Udp4, L"10.0.0.1", 5000, L"", 0, 0, 200),  // UDP 变化：不产事件
    };

    std::vector<stm::ConnEvent> evs;
    stm::DiffConnSnapshots(prev, cur, 1700000000, CP::Tcp4, &evs);
    if (evs.size() != 3) {
        *err = stm::Fmt(L"TCP 差分应产出 3 个事件，实得 {}", evs.size());
        return false;
    }
    if (evs[0].kind != K::StateChanged || evs[0].localPort != 5000 ||
        evs[0].stateLabel != L"对端关闭") {
        *err = L"事件 0 应为 5000 的 StateChanged（对端关闭）";
        return false;
    }
    if (evs[1].kind != K::New || evs[1].localPort != 6000 || evs[1].remotePort != 443 ||
        evs[1].stateLabel != L"已建立" || evs[1].pid != 102 || evs[1].unixTime != 1700000000 ||
        evs[1].remoteAddr != L"3.3.3.3") {
        *err = L"事件 1 应为 6000 的 New（已建立，pid=102，unixTime 正确）";
        return false;
    }
    if (evs[2].kind != K::Closed || evs[2].localPort != 5001 ||
        evs[2].stateLabel != L"对端关闭") {
        *err = L"事件 2 应为 5001 的 Closed（按旧状态标注）";
        return false;
    }
    for (const stm::ConnEvent& e : evs) {
        if (e.proto != static_cast<uint32_t>(CP::Tcp4) || e.remoteAddr.empty() ||
            !e.processName.empty()) {
            *err = L"事件字段不完整（proto/remote 空，或 processName 未留空给轮询层）";
            return false;
        }
    }

    evs.clear();
    stm::DiffConnSnapshots(prev, cur, 1, CP::Udp4, &evs);
    if (!evs.empty()) {
        *err = L"UDP 差分不应产出任何事件";
        return false;
    }
    evs.clear();
    stm::DiffConnSnapshots(prev, cur, 1, CP::Tcp6, &evs);
    if (!evs.empty()) {
        *err = L"无 Tcp6 行时不产事件";
        return false;
    }
    evs.clear();
    stm::DiffConnSnapshots({}, {}, 1, CP::Tcp4, &evs);
    if (!evs.empty()) {
        *err = L"空快照差分应产出 0 事件";
        return false;
    }

    // V25 P0-1 回归：同一 vector 连续双族调用（PollLoop 的真实用法）——
    // 追加语义下 Tcp4 事件不得被后续 Tcp6/Udp4 调用清空。
    const std::vector<stm::ConnEntry> bothPrev = {
        Row(CP::Tcp4, L"10.0.0.2", 7000, L"4.4.4.4", 443, 5, 300),
        Row(CP::Tcp6, L"::1", 7001, L"::2", 80, 5, 301),
    };
    const std::vector<stm::ConnEntry> bothCur = {
        Row(CP::Tcp4, L"10.0.0.2", 7000, L"4.4.4.4", 443, 8, 300),  // StateChanged
        Row(CP::Tcp6, L"::1", 7001, L"::2", 80, 8, 301),            // StateChanged
    };
    std::vector<stm::ConnEvent> mix;
    stm::DiffConnSnapshots(bothPrev, bothCur, 42, CP::Tcp4, &mix);
    stm::DiffConnSnapshots(bothPrev, bothCur, 42, CP::Tcp6, &mix);
    stm::DiffConnSnapshots(bothPrev, bothCur, 42, CP::Udp4, &mix);
    if (mix.size() != 2) {
        *err = stm::Fmt(L"同一 vector 连续双族差分应累计 2 事件，实得 {}", mix.size());
        return false;
    }
    if (mix[0].proto != static_cast<uint32_t>(CP::Tcp4) || mix[0].localPort != 7000 ||
        mix[1].proto != static_cast<uint32_t>(CP::Tcp6) || mix[1].localPort != 7001) {
        *err = L"双族事件顺序/归属错误（Tcp4 与 Tcp6 都必须在场）";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
STM_TEST(netmon_service_ports) {
    if (stm::ServiceNameForPort(443, false) != L"HTTPS") { *err = L"443/TCP 应为 HTTPS"; return false; }
    if (stm::ServiceNameForPort(123, true) != L"NTP") { *err = L"123/UDP 应为 NTP"; return false; }
    if (stm::ServiceNameForPort(123, false) != L"") { *err = L"123/TCP 应为空（NTP 仅 UDP）"; return false; }
    if (stm::ServiceNameForPort(5353, false) != L"") { *err = L"5353/TCP 应为空（mDNS 仅 UDP）"; return false; }
    if (stm::ServiceNameForPort(53, false) != L"DNS" || stm::ServiceNameForPort(53, true) != L"DNS") {
        *err = L"53 双栈均应为 DNS";
        return false;
    }
    if (stm::ServiceNameForPort(3389, false) != L"RDP") { *err = L"3389 应为 RDP"; return false; }
    if (stm::ServiceNameForPort(8080, false) != L"HTTP-Alt") { *err = L"8080 应为 HTTP-Alt"; return false; }
    if (stm::ServiceNameForPort(27017, false) != L"MongoDB") { *err = L"27017 应为 MongoDB"; return false; }
    if (stm::ServiceNameForPort(1900, true) != L"SSDP") { *err = L"1900/UDP 应为 SSDP"; return false; }
    if (stm::ServiceNameForPort(67, true) != L"DHCP" || stm::ServiceNameForPort(68, true) != L"DHCP") {
        *err = L"67/68 UDP 应为 DHCP";
        return false;
    }
    const uint16_t unknown[] = {0, 1, 9999, 40000, 65535};
    for (uint16_t p : unknown) {
        if (!stm::ServiceNameForPort(p, false).empty() || !stm::ServiceNameForPort(p, true).empty()) {
            *err = stm::Fmt(L"未知端口 {} 应返回空", p);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
STM_TEST(netmon_top_remote) {
    stm::cd::EndpointMap m;
    m[Key4(100, 1, 2, 3, 4, 443, IPPROTO_TCP)].bytesIn = 100;   // 总 150
    m[Key4(100, 1, 2, 3, 4, 443, IPPROTO_TCP)].bytesOut = 50;
    auto& b = m[Key4(101, 5, 6, 7, 8, 80, IPPROTO_TCP)];        // 总 900
    b.bytesIn = 200;
    b.bytesOut = 700;
    auto& c = m[Key4(102, 9, 9, 9, 9, 53, IPPROTO_UDP)];        // 总 60；UDP DNS
    c.bytesIn = 60;
    auto& d = m[Key4(103, 10, 10, 10, 10, 40000, IPPROTO_TCP)]; // 总 60；未知端口
    d.bytesIn = 30;
    d.bytesOut = 30;
    auto& e = m[Key4(104, 11, 11, 11, 11, 8080, IPPROTO_TCP)];  // 平局 60
    e.bytesIn = 60;
    // V25 P2：同 ip:port 不同 proto 的平局——proto 升序 TCP(6)<UDP(17)。
    auto& f = m[Key4(105, 12, 12, 12, 12, 9090, IPPROTO_TCP)];  // 平局 60
    f.bytesIn = 60;
    auto& g = m[Key4(106, 12, 12, 12, 12, 9090, IPPROTO_UDP)];  // 平局 60
    g.bytesIn = 60;

    const auto top = stm::TopRemoteFromEndpoints(m, 3);
    if (top.size() != 3) {
        *err = stm::Fmt(L"topN=3 截断后应 3 行，实得 {}", top.size());
        return false;
    }
    if (top[0].port != 80 || top[0].pid != 101 || top[0].remote != L"5.6.7.8" ||
        top[0].service != L"HTTP") {
        *err = L"第 1 名应为 5.6.7.8:80（总 900，HTTP）";
        return false;
    }
    // 完整降序：80(900) > 443(150) > 五个 60 的平组；平组按 (remote, port,
    // proto, pid) 升序："10.10.10.10" < "11.11.11.11" < "12.12.12.12"(TCP)
    // < "12.12.12.12"(UDP) < "9.9.9.9"。
    if (top[1].port != 443 || top[1].remote != L"1.2.3.4" || top[1].service != L"HTTPS") {
        *err = stm::Fmt(L"第 2 名应为 1.2.3.4:443（实得 {}:{} svc={}）", top[1].remote,
                        top[1].port, top[1].service);
        return false;
    }
    if (top[2].port != 40000 || top[2].remote != L"10.10.10.10" || !top[2].service.empty()) {
        *err = stm::Fmt(L"第 3 名应为 10.10.10.10:40000（平局字符串升序，service 空；实得 {}:{}）",
                        top[2].remote, top[2].port);
        return false;
    }
    const auto all = stm::TopRemoteFromEndpoints(m, 10);
    if (all.size() != 7) {
        *err = stm::Fmt(L"topN=10 应返回全部 7 行，实得 {}", all.size());
        return false;
    }
    if (all[3].port != 8080 || all[3].remote != L"11.11.11.11" ||
        all[3].service != L"HTTP-Alt") {
        *err = L"第 4 名应为 11.11.11.11:8080（HTTP-Alt）";
        return false;
    }
    if (all[4].port != 9090 || all[4].pid != 105 || all[4].remote != L"12.12.12.12") {
        *err = L"第 5 名应为 12.12.12.12:9090 TCP（proto 升序在前）";
        return false;
    }
    if (all[5].port != 9090 || all[5].pid != 106 || all[5].remote != L"12.12.12.12") {
        *err = L"第 6 名应为 12.12.12.12:9090 UDP（proto 升序在后）";
        return false;
    }
    if (all[6].port != 53 || all[6].remote != L"9.9.9.9" || all[6].service != L"DNS") {
        *err = L"末位应为 9.9.9.9:53（UDP DNS）";
        return false;
    }
    for (const auto& r : all) {
        if (!r.processName.empty()) {
            *err = L"纯函数不应伪造进程名";
            return false;
        }
        if (r.bytesIn + r.bytesOut <= 0) {
            *err = L"合成聚合不应出现 0 总量行";
            return false;
        }
    }
    const auto one = stm::TopRemoteFromEndpoints(m, 1);
    if (one.size() != 1 || one[0].port != 80) {
        *err = L"topN=1 应只留总量最大的行";
        return false;
    }
    if (!stm::TopRemoteFromEndpoints(m, 0).empty()) {
        *err = L"topN=0 应返回空";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
STM_TEST(netmon_event_ring_overflow) {
    stm::cd::EventRing<stm::ConnEvent> ring(stm::kNetEventCap);
    constexpr uint32_t kExtra = 1234;
    for (uint32_t i = 0; i < stm::kNetEventCap + kExtra; ++i) {
        stm::ConnEvent e;
        e.localPort = static_cast<uint16_t>(i & 0xFFFFu);  // 序号标记
        ring.Push(std::move(e));
    }
    if (ring.Dropped() != kExtra) {
        *err = stm::Fmt(L"Dropped 应为 {}，实得 {}", kExtra, ring.Dropped());
        return false;
    }
    std::vector<stm::ConnEvent> out;
    ring.Drain(&out);
    if (out.size() != stm::kNetEventCap) {
        *err = stm::Fmt(L"溢出后应保留恰好 {} 条，实得 {}", stm::kNetEventCap, out.size());
        return false;
    }
    // 保新弃旧：最旧被覆盖 => 首条序号 = kExtra；且顺序严格递增。
    for (size_t i = 0; i < out.size(); ++i) {
        const uint32_t want = (static_cast<uint32_t>(i) + kExtra) & 0xFFFFu;
        if (out[i].localPort != want) {
            *err = stm::Fmt(L"环形序错位：位置 {} 应为 {}，实得 {}", i, want, out[i].localPort);
            return false;
        }
    }
    out.clear();
    ring.Drain(&out);
    if (!out.empty()) {
        *err = L"Drain 后再次 Drain 应为空";
        return false;
    }
    ring.Clear();  // 清空同时重置丢弃计数（诚实展示“自上次清空以来”）
    if (ring.Dropped() != 0) {
        *err = L"Clear 应重置丢弃计数";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
STM_TEST(netmon_dns_off_by_default) {
    stm::NetMonitor mon;
    if (mon.EventCaptureEnabled() || mon.RemoteTrafficEnabled() || mon.DnsCaptureEnabled()) {
        *err = L"构造后三个开关都必须为关";
        return false;
    }
    std::vector<stm::ConnEvent> evs;
    mon.DrainEvents(&evs);
    std::vector<stm::DnsEvent> dns;
    mon.DrainDns(&dns);
    if (!evs.empty() || !dns.empty() || mon.DroppedEvents() != 0 ||
        !mon.TopRemoteTraffic(10).empty()) {
        *err = L"默认关闭时应无事件/无端点/无丢弃";
        return false;
    }
    std::wstring dnsErr;
    if (!mon.SetDnsCapture(false, &dnsErr) || mon.DnsCaptureEnabled()) {
        *err = L"关闭已关的 DNS 应为无害 no-op";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
STM_TEST(netmon_lifecycle_toggle) {
    const bool admin = stm::IsProcessElevated();
    const std::wstring etwName = stm::Fmt(L"SuperTaskMgr-NetMon-{}", ::GetCurrentProcessId());
    const std::wstring dnsName = stm::Fmt(L"SuperTaskMgr-Dns-{}", ::GetCurrentProcessId());

    // 连接事件流：开关 ×5（每轮全新实例，析构必须全停）。
    for (int i = 0; i < 5; ++i) {
        stm::NetMonitor mon;
        if (!mon.SetEventCapture(true) || !mon.EventCaptureEnabled()) {
            *err = L"SetEventCapture(true) 应生效（免管理员）";
            return false;
        }
        if (!mon.SetEventCapture(false) || mon.EventCaptureEnabled()) {
            *err = L"SetEventCapture(false) 应生效";
            return false;
        }
    }
    // 单实例三路混合开关 ×5：返回值与状态严格一致。
    {
        stm::NetMonitor mon;
        std::wstring dnsErr;
        for (int i = 0; i < 5; ++i) {
            if (!mon.SetEventCapture(true) || !mon.EventCaptureEnabled()) {
                *err = L"事件流 enable 应总是成功";
                return false;
            }
            const bool trOn = mon.SetRemoteTraffic(true);
            if (trOn != mon.RemoteTrafficEnabled()) {
                *err = L"端点聚合 enable 返回值与状态不一致";
                return false;
            }
            if (!admin && trOn) {
                *err = L"非管理员端点聚合不应启用成功";
                return false;
            }
            const bool dnsOn = mon.SetDnsCapture(true, &dnsErr);
            if (dnsOn != mon.DnsCaptureEnabled()) {
                *err = L"DNS enable 返回值与状态不一致";
                return false;
            }
            if (!mon.SetEventCapture(false) || mon.EventCaptureEnabled()) {
                *err = L"事件流 disable 失败";
                return false;
            }
            if (!mon.SetRemoteTraffic(false) || mon.RemoteTrafficEnabled()) {
                *err = L"端点聚合 disable 失败";
                return false;
            }
            if (!mon.SetDnsCapture(false, &dnsErr) || mon.DnsCaptureEnabled()) {
                *err = L"DNS disable 失败";
                return false;
            }
        }
    }
    // 无孤儿会话。V25 P1-2：同时断言默认名 "Net-<pid>"——若 P0-2 回归
    //（Start 覆写定制名），NetMonitor 会占用默认名并在此留下痕迹。
    const std::wstring defaultEtwName = stm::Fmt(L"SuperTaskMgr-Net-{}", ::GetCurrentProcessId());
    if (stm::cd::EtwNetCollector::SessionExists(etwName.c_str())) {
        *err = L"残留 ETW 会话：" + etwName;
        return false;
    }
    if (stm::cd::EtwNetCollector::SessionExists(defaultEtwName.c_str())) {
        *err = L"默认名会话残留（NetMonitor 不应占用它）：" + defaultEtwName;
        return false;
    }
    if (stm::cd::DnsCollector::SessionExists(dnsName.c_str())) {
        *err = L"残留 DNS 会话：" + dnsName;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// V25 P0-2 回归：同名会话互杀检测。两个收集器用同一会话名，B.Start 的
// "启动前清残留"会停掉 A 的会话——A.Running() 必须诚实变 false（且
// A.Stop() 幂等无悬挂），而非句柄仍在却静默失效。
// ---------------------------------------------------------------------------
STM_TEST(netmon_session_kill_detection) {
    const bool admin = stm::IsProcessElevated();
    const std::wstring shared = stm::Fmt(L"SuperTaskMgr-NetKill-{}", ::GetCurrentProcessId());
    stm::cd::EtwNetCollector a, b;
    a.SetSessionName(shared);
    b.SetSessionName(shared);
    if (!a.Start()) {
        if (admin) {
            *err = L"管理员环境下会话 A 启动失败";
            return false;
        }
        printf("[netmon] 非管理员：跳过会话互杀检测\n");
        return true;
    }
    if (!a.Running()) {
        *err = L"A 启动后 Running 应为 true";
        return false;
    }
    if (!b.Start()) {  // B 的启动前清理会停掉 A 的同名会话
        *err = L"管理员环境下会话 B 启动失败";
        return false;
    }
    // A 的句柄已被外部作废：失效检测必须报告 false（诚实降级）。
    if (a.Running()) {
        *err = L"会话被同名实例顶掉后 A.Running() 仍为 true（失效检测缺失）";
        return false;
    }
    if (!b.Running()) {
        *err = L"B 启动后 Running 应为 true";
        return false;
    }
    b.Stop();
    a.Stop();  // 幂等：内部句柄已作废，应为 no-op 且不悬挂
    if (a.Running() || b.Running()) {
        *err = L"Stop 后 Running 应为 false";
        return false;
    }
    if (stm::cd::EtwNetCollector::SessionExists(shared.c_str())) {
        *err = L"互杀检测后残留会话：" + shared;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 实测探针（管理员分支）：ETW 远程字段解析率 + 端点聚合。只测量与打印，
// 不对本底流量做硬断言（网络环境不可控）；生命周期机制照常断言。
// ---------------------------------------------------------------------------
STM_TEST(netmon_etw_remote_probe) {
    const bool admin = stm::IsProcessElevated();
    stm::cd::EtwNetCollector probe;
    const std::wstring name = stm::Fmt(L"SuperTaskMgr-NetProbe-{}", ::GetCurrentProcessId());
    probe.SetSessionName(name);
    if (!probe.Start()) {
        if (admin) {
            *err = L"管理员环境下 ETW 探针启动失败";
            return false;
        }
        printf("[netmon] 非管理员：ETW 拒绝（预期，跳过实测）\n");
        return true;
    }
    const char* stage = "-";
    const uint16_t echoPort = LoopbackEcho(&stage);  // 保证有确定性的 Kernel-Network 事件
    ::Sleep(2000);                                   // FlushTimer=1s：让缓冲刷出
    const auto st = probe.CopyFieldStats();
    const auto sample = probe.CopyRawSample();
    stm::cd::EndpointMap m;
    probe.CopyEndpoints(&m);
    const auto top = stm::TopRemoteFromEndpoints(m, 8);
    stm::cd::ProcNameCache names;
    const uint64_t addrAll = st.addrHit + st.addrMiss;
    const uint64_t portAll = st.portHit + st.portMiss;
    printf("[netmon] 环回回显=%s(阶段:%s 端口=%u) ETW事件=%llu 端点表=%zu 退化仅pid=%llu\n",
           echoPort != 0 ? "OK" : "NO", stage, echoPort,
           static_cast<unsigned long long>(probe.TotalEvents()), m.size(),
           static_cast<unsigned long long>(st.noRemoteEvents));
    printf("[netmon] saddr/daddr 命中 %llu/%llu（%.1f%%） sport/dport 命中 %llu/%llu（%.1f%%）"
           " direction 字段命中 %llu\n",
           static_cast<unsigned long long>(st.addrHit),
           static_cast<unsigned long long>(addrAll),
           addrAll ? 100.0 * static_cast<double>(st.addrHit) / static_cast<double>(addrAll) : 0.0,
           static_cast<unsigned long long>(st.portHit),
           static_cast<unsigned long long>(portAll),
           portAll ? 100.0 * static_cast<double>(st.portHit) / static_cast<double>(portAll) : 0.0,
           static_cast<unsigned long long>(st.dirFieldHit));
    printf("[netmon] 原始样本：sport=%u(0x%04X) dport=%u(0x%04X) direction=%u proto=%u "
           "saddrFam=%u daddrFam=%u\n",
           sample.sport, sample.sport, sample.dport, sample.dport, sample.direction,
           sample.proto, sample.saddrFamily, sample.daddrFamily);
    for (const auto& r : top) {
        printf("[netmon]   %s:%u %s pid=%u(%s) in=%.0f out=%.0f\n",
               stm::WideToUtf8(r.remote).c_str(), r.port, stm::WideToUtf8(r.service).c_str(),
               r.pid, stm::WideToUtf8(names.NameOf(r.pid)).c_str(), r.bytesIn, r.bytesOut);
    }
    // V25 P0-3 字节序断言：事件可用时，端点表必须出现真实回显端口（主机
    // 序），且不得出现其换序值（443↔47873 那类错误）。无事件（本 VM 的
    // Kernel-Network 不投递）时诚实跳过、不硬断言。
    if (!m.empty() && echoPort != 0) {
        const uint16_t swapped = Swap16(echoPort);
        bool found = false, sawSwapped = false;
        for (const auto& kv : m) {
            if (kv.first.port == echoPort) found = true;
            if (echoPort != swapped && kv.first.port == swapped) sawSwapped = true;
        }
        if (!found || sawSwapped) {
            *err = stm::Fmt(L"端点表端口字节序错误：未见回显端口 {} 或出现换序值 {}", echoPort,
                            swapped);
            return false;
        }
    }
    probe.Stop();
    if (probe.Running()) {
        *err = L"Stop 后仍显示运行中";
        return false;
    }
    if (stm::cd::EtwNetCollector::SessionExists(name.c_str())) {
        *err = L"探针停止后残留会话：" + name;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 实测探针（管理员分支）：DNS-Client 提供者字段解码率。只测量打印；
// 域名查询依赖系统本底（遥测/同步），不做硬断言。
// ---------------------------------------------------------------------------
STM_TEST(netmon_dns_probe) {
    const bool admin = stm::IsProcessElevated();
    stm::cd::DnsCollector probe;
    if (!probe.Start()) {
        if (admin) {
            *err = L"管理员环境下 DNS 探针启动失败";
            return false;
        }
        printf("[netmon] 非管理员：DNS ETW 拒绝（预期，跳过实测）\n");
        return true;
    }
    // 主动触发一次 DNS 查询（dnsapi 动态加载，selftest 未链接 dnsapi）；
    // 失败无碍——系统本底也持续产生查询。
    {
        const HMODULE dh = ::LoadLibraryW(L"dnsapi.dll");
        if (dh) {
            const auto q = reinterpret_cast<int(WINAPI*)(const wchar_t*, uint16_t, uint32_t,
                                                        void*, void**, void*)>(
                ::GetProcAddress(dh, "DnsQuery_W"));
            const auto fr = reinterpret_cast<void(WINAPI*)(void*, int32_t)>(
                ::GetProcAddress(dh, "DnsRecordListFree"));
            if (q && fr) {
                void* recs = nullptr;
                q(L"www.msftconnecttest.com", 1 /*DNS_TYPE_A*/, 0, nullptr, &recs, nullptr);
                if (recs) fr(recs, 0 /*DnsFreeRecordList*/);
            }
        }
    }
    ::Sleep(2500);  // 让查询事件（含系统本底）流入并刷出
    printf("[netmon] DNS 原始事件=%llu 解码成功=%llu\n",
           static_cast<unsigned long long>(probe.RawEvents()),
           static_cast<unsigned long long>(probe.DecodedEvents()));
    std::vector<stm::DnsEvent> evs;
    probe.DrainEvents(&evs);
    for (size_t i = 0; i < evs.size() && i < 8; ++i) {
        printf("[netmon]   pid=%u %s\n", evs[i].pid, stm::WideToUtf8(evs[i].query).c_str());
    }
    probe.Stop();
    // V25 P1-2：孤儿检查用真实会话名（NetMonitor/DnsCollector 的固定名）。
    const std::wstring dnsName = stm::Fmt(L"SuperTaskMgr-Dns-{}", ::GetCurrentProcessId());
    if (stm::cd::DnsCollector::SessionExists(dnsName.c_str())) {
        *err = L"DNS 探针停止后残留会话：" + dnsName;
        return false;
    }
    return true;
}
