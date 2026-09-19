// NetTables.cpp — 第 3 阶段。
// A 部分（契约 NetTables.h）：经 GetExtendedTcpTable / GetExtendedUdpTable
//  （TCP_TABLE_OWNER_PID_ALL / UDP_TABLE_OWNER_PID，v4+v6）取得带持有 PID 的
//   TCP/UDP 端点表。缓冲区在 ERROR_INSUFFICIENT_BUFFER 重试循环中增长；
//   地址用 InetNtopW 格式化（IPv6 按 RFC 5952，从 ws2_32.dll 动态加载）；
//   PID=0 的行是内核绑定的套接字，按原样保留（绝不丢弃）。
//
// B 部分（内部，CollectDetail.h）：EtwNetCollector——实时 ETW 会话
//   （Microsoft-Windows-Kernel-Network，R5 #10b），仅管理员可用，为
//   CollectService 的差分聚合每 pid 累计收发字节。
#include <winsock2.h>  // 必须在 windows.h/iphlpapi 之前包含（LEAN_AND_MEAN 会隐藏 winsock）
#include <ws2tcpip.h>  // 引入 ws2ipdef.h -> MIB_TCP6/UDP6 行所需的 in6_addr
#include "collect/CollectDetail.h"
#include "collect/NetTables.h"
#include "core/Log.h"
#include "core/Str.h"
#include <iphlpapi.h>
#include <tdh.h>
#include <algorithm>
#include <cwchar>

namespace stm {

// ===========================================================================
// A 部分：连接表（契约 NetTables.h）
// ===========================================================================
namespace {

constexpr DWORD kInitialTableBytes = 32 * 1024;
constexpr int kMaxTableRetries = 8;

// InetNtopW 位于 ws2_32.dll；stm_collect 不链接 ws2_32，因此动态绑定，
// 并把模块保留到进程结束（同上文的 ntdll 一样）。
using InetNtopWFn = PCWSTR(WINAPI*)(INT family, const VOID* pAddr, PWSTR pStringBuf,
                                    size_t StringBufSize);

InetNtopWFn InetNtopWProc() {
    static InetNtopWFn fn = []() -> InetNtopWFn {
        const HMODULE h = ::LoadLibraryW(L"ws2_32.dll");
        return h ? reinterpret_cast<InetNtopWFn>(::GetProcAddress(h, "InetNtopW")) : nullptr;
    }();
    return fn;
}

std::wstring FmtAddrV4(const IN_ADDR& a) {
    wchar_t buf[46]{};
    const InetNtopWFn fn = InetNtopWProc();
    if (fn && fn(AF_INET, &a, buf, std::size(buf))) return buf;
    const auto* b = reinterpret_cast<const uint8_t*>(&a);  // 网络字节序
    return Fmt(L"{}.{}.{}.{}", b[0], b[1], b[2], b[3]);
}

std::wstring FmtAddrV6(const IN6_ADDR& a) {
    wchar_t buf[46]{};
    const InetNtopWFn fn = InetNtopWProc();
    if (fn && fn(AF_INET6, &a, buf, std::size(buf))) return buf;  // RFC 5952 风格
    // 兜底：小写十六进制分组（仅在 ws2_32 不可用时才会走到）。
    std::wstring s;
    for (int i = 0; i < 8; ++i) {
        if (i) s += L':';
        s += Fmt(L"{:x}", (static_cast<uint16_t>(a.u.Byte[i * 2]) << 8) | a.u.Byte[i * 2 + 1]);
    }
    return s;
}

// 端口字段以网络字节序存于 DWORD 的低 16 位；
// 手工交换字节序（未链接 ws2_32 的 ntohs）。
uint16_t NetPort(uint32_t dwPort) {
    const uint32_t raw = dwPort & 0xFFFFu;
    return static_cast<uint16_t>(((raw & 0xFFu) << 8) | ((raw >> 8) & 0xFFu));
}

void NoteErr(std::wstring* errs, const wchar_t* what, DWORD rc) {
    if (!errs) return;
    if (!errs->empty()) *errs += L"；";
    *errs += Fmt(L"{} 失败（Win32 {}）", what, rc);
}

// 通过 ERROR_INSUFFICIENT_BUFFER 重试扩大缓冲区（大小探测与调用之间
// 表可能变化，因此需要循环）。
template <typename Api>
bool FetchTable(Api api, std::vector<BYTE>* buf, std::wstring* errs, const wchar_t* what) {
    DWORD size = static_cast<DWORD>(buf->size());
    for (int attempt = 0; attempt < kMaxTableRetries; ++attempt) {
        const DWORD rc = api(buf->data(), &size);
        if (rc == NO_ERROR) return true;
        if (rc != ERROR_INSUFFICIENT_BUFFER) {
            NoteErr(errs, what, rc);
            return false;
        }
        if (size <= buf->size()) size = static_cast<DWORD>(buf->size()) * 2u;
        buf->resize(size);
    }
    NoteErr(errs, what, ERROR_INSUFFICIENT_BUFFER);
    return false;
}

void AppendTcp4(std::vector<BYTE>* buf, std::vector<ConnEntry>* out, std::wstring* errs) {
    if (!FetchTable(
            [buf](PVOID p, PDWORD s) {
                return ::GetExtendedTcpTable(p, s, TRUE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
            },
            buf, errs, L"GetExtendedTcpTable(IPv4)")) {
        return;
    }
    const auto* t = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buf->data());
    out->reserve(out->size() + t->dwNumEntries);
    for (DWORD i = 0; i < t->dwNumEntries; ++i) {
        const MIB_TCPROW_OWNER_PID& r = t->table[i];
        ConnEntry e;
        e.proto = ConnProto::Tcp4;
        e.localAddr = FmtAddrV4(*reinterpret_cast<const IN_ADDR*>(&r.dwLocalAddr));
        e.localPort = NetPort(r.dwLocalPort);
        e.remoteAddr = FmtAddrV4(*reinterpret_cast<const IN_ADDR*>(&r.dwRemoteAddr));
        e.remotePort = NetPort(r.dwRemotePort);
        e.state = static_cast<uint32_t>(r.dwState);
        e.pid = r.dwOwningPid;  // 0 = 内核绑定，按契约保留
        out->push_back(std::move(e));
    }
}

void AppendTcp6(std::vector<BYTE>* buf, std::vector<ConnEntry>* out, std::wstring* errs) {
    if (!FetchTable(
            [buf](PVOID p, PDWORD s) {
                return ::GetExtendedTcpTable(p, s, TRUE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0);
            },
            buf, errs, L"GetExtendedTcpTable(IPv6)")) {
        return;
    }
    const auto* t = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buf->data());
    out->reserve(out->size() + t->dwNumEntries);
    for (DWORD i = 0; i < t->dwNumEntries; ++i) {
        const MIB_TCP6ROW_OWNER_PID& r = t->table[i];
        ConnEntry e;
        e.proto = ConnProto::Tcp6;
        e.localAddr = FmtAddrV6(*reinterpret_cast<const IN6_ADDR*>(r.ucLocalAddr));
        e.localPort = NetPort(r.dwLocalPort);
        e.remoteAddr = FmtAddrV6(*reinterpret_cast<const IN6_ADDR*>(r.ucRemoteAddr));
        e.remotePort = NetPort(r.dwRemotePort);
        e.state = static_cast<uint32_t>(r.dwState);
        e.pid = r.dwOwningPid;
        out->push_back(std::move(e));
    }
}

void AppendUdp4(std::vector<BYTE>* buf, std::vector<ConnEntry>* out, std::wstring* errs) {
    if (!FetchTable(
            [buf](PVOID p, PDWORD s) {
                return ::GetExtendedUdpTable(p, s, TRUE, AF_INET, UDP_TABLE_OWNER_PID, 0);
            },
            buf, errs, L"GetExtendedUdpTable(IPv4)")) {
        return;
    }
    const auto* t = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(buf->data());
    out->reserve(out->size() + t->dwNumEntries);
    for (DWORD i = 0; i < t->dwNumEntries; ++i) {
        const MIB_UDPROW_OWNER_PID& r = t->table[i];
        ConnEntry e;
        e.proto = ConnProto::Udp4;
        e.localAddr = FmtAddrV4(*reinterpret_cast<const IN_ADDR*>(&r.dwLocalAddr));
        e.localPort = NetPort(r.dwLocalPort);
        e.pid = r.dwOwningPid;
        out->push_back(std::move(e));
    }
}

void AppendUdp6(std::vector<BYTE>* buf, std::vector<ConnEntry>* out, std::wstring* errs) {
    if (!FetchTable(
            [buf](PVOID p, PDWORD s) {
                return ::GetExtendedUdpTable(p, s, TRUE, AF_INET6, UDP_TABLE_OWNER_PID, 0);
            },
            buf, errs, L"GetExtendedUdpTable(IPv6)")) {
        return;
    }
    const auto* t = reinterpret_cast<const MIB_UDP6TABLE_OWNER_PID*>(buf->data());    out->reserve(out->size() + t->dwNumEntries);
    for (DWORD i = 0; i < t->dwNumEntries; ++i) {
        const MIB_UDP6ROW_OWNER_PID& r = t->table[i];
        ConnEntry e;
        e.proto = ConnProto::Udp6;
        e.localAddr = FmtAddrV6(*reinterpret_cast<const IN6_ADDR*>(r.ucLocalAddr));
        e.localPort = NetPort(r.dwLocalPort);
        e.pid = r.dwOwningPid;
        out->push_back(std::move(e));
    }
}

}  // namespace

std::vector<ConnEntry> SnapshotConnections(std::wstring* err) {
    std::wstring errs;
    std::vector<ConnEntry> out;
    out.reserve(256);
    std::vector<BYTE> buf(kInitialTableBytes);
    AppendTcp4(&buf, &out, &errs);
    AppendUdp4(&buf, &out, &errs);
    AppendTcp6(&buf, &out, &errs);
    AppendUdp6(&buf, &out, &errs);
    if (err) *err = errs;
    return out;
}

std::wstring TcpStateLabel(uint32_t state) {
    switch (state) {
        case MIB_TCP_STATE_CLOSED: return L"关闭";
        case MIB_TCP_STATE_LISTEN: return L"监听";
        case MIB_TCP_STATE_SYN_SENT: return L"同步发送";
        case MIB_TCP_STATE_SYN_RCVD: return L"同步接收";
        case MIB_TCP_STATE_ESTAB: return L"已建立";
        case MIB_TCP_STATE_FIN_WAIT1: return L"等待关闭1";
        case MIB_TCP_STATE_FIN_WAIT2: return L"等待关闭2";
        case MIB_TCP_STATE_CLOSE_WAIT: return L"对端关闭";
        case MIB_TCP_STATE_CLOSING: return L"正在关闭";
        case MIB_TCP_STATE_LAST_ACK: return L"最后确认";
        case MIB_TCP_STATE_TIME_WAIT: return L"时间等待";
        case MIB_TCP_STATE_DELETE_TCB: return L"删除";
        default: return Fmt(L"0x{:X}", state);  // 未知状态诚实地以十六进制展示
    }
}

// ===========================================================================
// 第 10 维护轮：端点聚合 -> UI 行（契约 NetMonitor.h 的 RemoteTraffic）。
// 复用 A 部分的地址格式化；service 按知名端口表（NetMonitor.cpp）填写。
// ===========================================================================
std::vector<RemoteTraffic> TopRemoteFromEndpoints(const cd::EndpointMap& src, size_t topN) {
    std::vector<std::pair<cd::EndpointKey, RemoteTraffic>> rows;
    rows.reserve(src.size());
    for (const auto& kv : src) {
        const cd::EndpointKey& k = kv.first;
        const cd::EndpointAgg& a = kv.second;
        RemoteTraffic r;
        r.pid = k.pid;
        r.port = k.port;
        if (k.family == AF_INET) {
            IN_ADDR v4{};
            std::memcpy(&v4, k.ip, 4);
            r.remote = FmtAddrV4(v4);
        } else if (k.family == AF_INET6) {
            IN6_ADDR v6{};
            std::memcpy(v6.u.Byte, k.ip, 16);
            r.remote = FmtAddrV6(v6);
        }  // 其他 family（0=退化桶不会入表）：remote 留空（诚实）
        r.service = ServiceNameForPort(k.port, k.proto == IPPROTO_UDP);
        r.bytesIn = static_cast<double>(a.bytesIn);
        r.bytesOut = static_cast<double>(a.bytesOut);
        rows.emplace_back(kv.first, std::move(r));
    }
    const auto totalOf = [](const RemoteTraffic& r) { return r.bytesIn + r.bytesOut; };
    // V25 P2 全序：总字节降序；平局按 (remote, port, proto, family, pid) 升序
    // ——proto/family 进键保证同 ip:port 的 TCP/UDP、v4/v6 行之间也全序
    //（单测可复现，绝不依赖哈希迭代顺序）。
    std::sort(rows.begin(), rows.end(),
              [&totalOf](const auto& x, const auto& y) {
                  const double tx = totalOf(x.second), ty = totalOf(y.second);
                  if (tx != ty) return tx > ty;
                  if (x.second.remote != y.second.remote) {
                      return x.second.remote < y.second.remote;
                  }
                  if (x.second.port != y.second.port) return x.second.port < y.second.port;
                  if (x.first.proto != y.first.proto) return x.first.proto < y.first.proto;
                  if (x.first.family != y.first.family) {
                      return x.first.family < y.first.family;
                  }
                  return x.first.pid < y.first.pid;
              });
    std::vector<RemoteTraffic> out;
    out.reserve(rows.size() < topN ? rows.size() : topN);
    for (auto& row : rows) {
        if (out.size() >= topN) break;
        out.push_back(std::move(row.second));
    }
    return out;
}

// ===========================================================================
// B 部分：EtwNetCollector（内部，CollectDetail.h）。此处仍处于
// namespace stm；类声明在 stm::cd，因此只重开 cd。
// ===========================================================================
namespace cd {

namespace {

// Microsoft-Windows-Kernel-Network（清单提供程序，Win10+；可在普通私有
// 实时会话上启用——与 `netsh trace` 做的是同一件事）。
constexpr GUID kKernelNetworkGuid = {
    0x7dd42a49, 0x5329, 0x4832, {0x8d, 0xfd, 0x43, 0xd9, 0x79, 0x15, 0x3a, 0x88}};
constexpr USHORT kEvtRecvData = 10;  // kernel:network:recvdata 事件
constexpr USHORT kEvtSendData = 11;  // kernel:network:senddata 事件

// tdh.dll：按属性名解码事件载荷（不跨 OS 构建猜布局）。
// 动态绑定；stm_collect 不链接 tdh.lib。
using TdhGetPropertyFn = ULONG(WINAPI*)(PEVENT_RECORD, ULONG, PTDH_CONTEXT, ULONG,
                                        PPROPERTY_DATA_DESCRIPTOR, ULONG, PBYTE);
using TdhGetPropertySizeFn = ULONG(WINAPI*)(PEVENT_RECORD, ULONG, PTDH_CONTEXT, ULONG,
                                            PPROPERTY_DATA_DESCRIPTOR, PULONG);

struct TdhFns {
    TdhGetPropertyFn get = nullptr;
    TdhGetPropertySizeFn getSize = nullptr;
};

TdhFns Tdh() {
    static TdhFns fn = []() -> TdhFns {
        const HMODULE h = ::LoadLibraryW(L"tdh.dll");
        if (!h) return {};
        return {reinterpret_cast<TdhGetPropertyFn>(::GetProcAddress(h, "TdhGetProperty")),
                reinterpret_cast<TdhGetPropertySizeFn>(
                    ::GetProcAddress(h, "TdhGetPropertySize"))};
    }();
    return fn;
}

// 按名称读取 uint32 载荷属性。缺失/无法解码时返回 false。
bool U32Prop(PEVENT_RECORD rec, const wchar_t* name, uint32_t* out) {
    const TdhFns f = Tdh();
    if (!f.get || !f.getSize) return false;
    PROPERTY_DATA_DESCRIPTOR d{};
    d.PropertyName = reinterpret_cast<ULONGLONG>(name);
    d.ArrayIndex = ULONG_MAX;
    ULONG size = 0;
    if (f.getSize(rec, 0, nullptr, 1, &d, &size) != ERROR_SUCCESS || size == 0 || size > 8) {
        return false;
    }
    uint64_t v = 0;
    if (f.get(rec, 0, nullptr, 1, &d, size, reinterpret_cast<PBYTE>(&v)) != ERROR_SUCCESS) {
        return false;
    }
    *out = static_cast<uint32_t>(v);
    return true;
}

size_t PropsSize(const std::wstring& name) {
    return sizeof(EVENT_TRACE_PROPERTIES) + (name.size() + 1) * sizeof(wchar_t);
}

// 按名称读取地址载荷属性。Kernel-Network 清单的 saddr/daddr 依地址族是
// win:IPv4/win:IPv6（4/16 字节）；旧式内核日志拼写下可能是完整 sockaddr
//（16/28 字节，family 在头两字节/地址在 +4/+8）。启发式：16 字节且头两
// 字节恰为小写 AF_INET(0x0200) 时按 sockaddr_in 解释（裸 in6 恰以
// 02 00 开头的地址会被误判——概率可忽略，诚实边界内）。
bool AddrProp(PEVENT_RECORD rec, const wchar_t* name, uint8_t (*out)[16], uint16_t* family) {
    const TdhFns f = Tdh();
    if (!f.get || !f.getSize) return false;
    PROPERTY_DATA_DESCRIPTOR d{};
    d.PropertyName = reinterpret_cast<ULONGLONG>(name);
    d.ArrayIndex = ULONG_MAX;
    ULONG size = 0;
    if (f.getSize(rec, 0, nullptr, 1, &d, &size) != ERROR_SUCCESS || size == 0 || size > 28) {
        return false;
    }
    uint8_t buf[28] = {};
    if (f.get(rec, 0, nullptr, 1, &d, size, buf) != ERROR_SUCCESS) return false;
    if (size == 4) {
        std::memcpy(*out, buf, 4);
        *family = AF_INET;
        return true;
    }
    if (size == 16) {
        if (buf[0] == 0x02 && buf[1] == 0x00) {
            std::memcpy(*out, buf + 4, 4);
            *family = AF_INET;
        } else {
            std::memcpy(*out, buf, 16);
            *family = AF_INET6;
        }
        return true;
    }
    if (size == 28) {  // sockaddr_in6：family(2)+port(2)+flow(4)+addr(16)+scope(4)
        std::memcpy(*out, buf + 8, 16);
        *family = AF_INET6;
        return true;
    }
    return false;
}

}  // namespace

// FNV-1a 逐字节混合（ip + 标量字段；无填充读取——逐字段处理）。
// 注意：外部类的成员函数不能在匿名命名空间内定义（C2888）。
size_t EndpointKeyHash::operator()(const EndpointKey& k) const noexcept {
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](const void* p, size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    mix(&k.pid, sizeof k.pid);
    mix(&k.family, sizeof k.family);
    mix(&k.port, sizeof k.port);
    mix(&k.proto, sizeof k.proto);
    mix(k.ip, sizeof k.ip);
    return static_cast<size_t>(h);
}

EtwNetCollector::~EtwNetCollector() { Stop(); }

bool EtwNetCollector::SessionExists(const wchar_t* name) {
    std::vector<BYTE> buf(PropsSize(name) + 512);
    auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
    p->Wnode.BufferSize = static_cast<ULONG>(buf.size());
    p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    const ULONG rc = ::ControlTraceW(0, name, p, EVENT_TRACE_CONTROL_QUERY);
    // ERROR_MORE_DATA 也表示"已存在"（信息比我们的缓冲区大）。
    return rc == ERROR_SUCCESS || rc == ERROR_MORE_DATA;
}

bool EtwNetCollector::Start() {
    std::lock_guard<std::mutex> lock(mu_);
    if (session_ != 0) return true;

    // 唯一会话名（带 pid）避免实例间冲突；前一个崩溃遗留的同名
    // 残留会话先被停止（防孤儿）。仅当调用方（如 NetMonitor 用
    // L"SuperTaskMgr-NetMon-<pid>"，V25 P0-2）未预先指定时才取默认名，
    // 否则默认名会覆盖定制名，使"启动前清残留"误杀并行实例的同名会话。
    if (sessionName_.empty()) {
        sessionName_ = Fmt(L"SuperTaskMgr-Net-{}", ::GetCurrentProcessId());
    }

    stopProps_.assign(PropsSize(sessionName_), 0);
    {
        auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(stopProps_.data());
        p->Wnode.BufferSize = static_cast<ULONG>(stopProps_.size());
        p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        ::ControlTraceW(0, sessionName_.c_str(), p, EVENT_TRACE_CONTROL_STOP);
        // ERROR_WMI_INSTANCE_NOT_FOUND 只说明没有残留。
    }

    // 实时会话，起始 64 个 64KB 缓冲（最多 128），1s 刷新一次，让每秒
    // 速率差分能看到新事件。
    std::vector<BYTE> propsBuf(PropsSize(sessionName_));
    auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propsBuf.data());
    p->Wnode.BufferSize = static_cast<ULONG>(propsBuf.size());
    p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p->Wnode.ClientContext = 1;  // QPC 时间戳
    p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    p->BufferSize = 64;  // 每缓冲 KB 数
    p->MinimumBuffers = 64;
    p->MaximumBuffers = 128;
    p->FlushTimer = 1;
    p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    TRACEHANDLE h = 0;
    ULONG rc = ::StartTraceW(&h, sessionName_.c_str(), p);
    if (rc != ERROR_SUCCESS) {
        STM_LOG_ERROR("etw", Fmt(L"StartTraceW({}) 失败（Win32 {}）：按进程网络速率需要管理员权限",
                                 sessionName_, rc));
        stopProps_.clear();
        return false;
    }

    // Keyword 0 = 不按关键字过滤，但会话只启用了 Kernel-Network
    // 提供程序，所以只会有网络类事件到达；回调还进一步
    // 只保留收/发数据事件（id 10/11）。
    rc = ::EnableTraceEx2(h, &kKernelNetworkGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                          TRACE_LEVEL_INFORMATION, 0, 0, 0, nullptr);
    if (rc != ERROR_SUCCESS) {
        STM_LOG_ERROR("etw",
                      Fmt(L"EnableTraceEx2(Kernel-Network) 失败（Win32 {}），会话已停止", rc));
        ::ControlTraceW(h, sessionName_.c_str(), p, EVENT_TRACE_CONTROL_STOP);
        stopProps_.clear();
        return false;
    }

    // 保留（内核更新过的）属性缓冲区，供之后 ControlTraceW 停止会话使用；
    // 它必须活得比 Start 久，因此复制为成员。
    stopProps_ = propsBuf;
    myGuid_ = reinterpret_cast<const EVENT_TRACE_PROPERTIES*>(propsBuf.data())->Wnode.Guid;
    session_ = h;
    bytes_.clear();
    endpoints_.clear();
    fieldStats_ = {};
    rawSample_ = {};
    totalEvents_ = 0;
    parseFails_ = 0;

    try {
        consumer_ = std::thread(&EtwNetCollector::Consume, this);
    } catch (const std::exception& e) {
        STM_LOG_ERROR("etw", Fmt(L"ETW 消费线程启动失败：{}", Utf8ToWide(e.what())));
        ::ControlTraceW(h, sessionName_.c_str(), p, EVENT_TRACE_CONTROL_STOP);
        session_ = 0;
        stopProps_.clear();
        return false;
    }
    STM_LOG_INFO("etw", Fmt(L"ETW 会话 {} 已启动（Kernel-Network，缓冲 64x64KB）", sessionName_));
    return true;
}

void EtwNetCollector::Consume() {
    EVENT_TRACE_LOGFILEW logf{};
    logf.LoggerName = &sessionName_[0];
    logf.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_REAL_TIME;
    logf.EventRecordCallback = &EtwNetCollector::OnEvent;
    logf.Context = this;
    const TRACEHANDLE h = ::OpenTraceW(&logf);
    if (h == INVALID_PROCESSTRACE_HANDLE) {
        STM_LOG_ERROR("etw", Fmt(L"OpenTraceW 失败（Win32 {}）", ::GetLastError()));
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        openTrace_ = h;
    }
    TRACEHANDLE hs[1] = {h};
    ::ProcessTrace(hs, 1, nullptr, nullptr);  // 阻塞直到会话被停止
    ::CloseTrace(h);
    {
        std::lock_guard<std::mutex> lock(mu_);
        openTrace_ = 0;
    }
}

void WINAPI EtwNetCollector::OnEvent(PEVENT_RECORD rec) {
    auto* self = static_cast<EtwNetCollector*>(rec->UserContext);
    if (self) self->HandleEvent(rec);
}

void EtwNetCollector::HandleEvent(PEVENT_RECORD rec) {
    if (!InlineIsEqualGUID(rec->EventHeader.ProviderId, kKernelNetworkGuid)) return;
    const USHORT id = rec->EventHeader.EventDescriptor.Id;
    const USHORT op = rec->EventHeader.EventDescriptor.Opcode;
    // recvdata=10 / senddata=11（清单）。同时接受 kernel-logger 拼写
    //（类型放在 Opcode、Id==0），让旧式会话也能工作。
    int dir = 0;
    if (id == kEvtRecvData || (id == 0 && op == kEvtRecvData)) {
        dir = 1;
    } else if (id == kEvtSendData || (id == 0 && op == kEvtSendData)) {
        dir = 2;
    } else {
        return;  // connect/disconnect/retransmit/... — 非携带字节的事件
    }
    uint32_t size = 0;
    if (!U32Prop(rec, L"size", &size)) {
        std::lock_guard<std::mutex> lock(mu_);
        ++parseFails_;
        return;
    }
    uint32_t pid = 0;
    if (!U32Prop(rec, L"pid", &pid) || pid == 0) pid = rec->EventHeader.ProcessId;
    if (pid == 0) return;  // 无身份：绝不归属给伪造的持有者

    // —— 第 10 维护轮：远程端点字段（尽力解析）。recvdata 的对端是源
    //（saddr/sport），senddata 的对端是目的（daddr/dport）；proto 字段
    // 区分 TCP/UDP。任一关键字段取不到就退化为仅-pid 级（既有路径）并
    // 计数——绝不伪造端点。
    uint8_t rip[16] = {};
    uint16_t rfamily = 0;
    const bool haveAddr = AddrProp(rec, dir == 1 ? L"saddr" : L"daddr", &rip, &rfamily);
    uint32_t rportRaw = 0;
    const bool havePort = U32Prop(rec, dir == 1 ? L"sport" : L"dport", &rportRaw);
    uint32_t protoRaw = 0;
    const bool haveProto = U32Prop(rec, L"proto", &protoRaw);
    uint32_t dirRaw = 0;
    const bool haveDir = U32Prop(rec, L"direction", &dirRaw);

    std::lock_guard<std::mutex> lock(mu_);
    Agg& a = bytes_[pid];
    if (dir == 1) {
        a.recv += size;
    } else {
        a.send += size;
    }
    ++totalEvents_;

    FieldStats& f = fieldStats_;
    if (haveAddr) {
        ++f.addrHit;
    } else {
        ++f.addrMiss;
    }
    if (havePort) {
        ++f.portHit;
    } else {
        ++f.portMiss;
    }
    if (haveDir) ++f.dirFieldHit;
    if (!rawSample_.valid) {  // 首个事件原样留档（实测探针/字节序核对用）
        uint32_t v = 0;
        rawSample_.valid = true;
        if (U32Prop(rec, L"sport", &v)) rawSample_.sport = v;
        if (U32Prop(rec, L"dport", &v)) rawSample_.dport = v;
        if (U32Prop(rec, L"direction", &v)) rawSample_.direction = v;
        if (U32Prop(rec, L"proto", &v)) rawSample_.proto = v;
        uint8_t ip[16] = {};
        uint16_t fam = 0;
        if (AddrProp(rec, L"saddr", &ip, &fam)) {
            rawSample_.saddrFamily = fam;
            std::memcpy(rawSample_.saddr, ip, sizeof ip);
        }
        if (AddrProp(rec, L"daddr", &ip, &fam)) {
            rawSample_.daddrFamily = fam;
            std::memcpy(rawSample_.daddr, ip, sizeof ip);
        }
    }
    if (haveAddr && havePort) {
        EndpointKey k;
        k.pid = pid;
        k.family = rfamily;
        // V25 P0-3：载荷端口为网络字节序（实测 443 呈 47873），换为主机序。
        k.port = NetPort(rportRaw);
        k.proto = static_cast<uint8_t>(haveProto ? (protoRaw & 0xFFu) : 0);
        std::memcpy(k.ip, rip, sizeof k.ip);
        EndpointAgg& ea = endpoints_[k];
        if (dir == 1) {
            ea.bytesIn += size;
        } else {
            ea.bytesOut += size;
        }
        ++ea.events;
    } else {
        ++f.noRemoteEvents;
    }
}

void EtwNetCollector::Stop() {
    // 在锁内复制会话簿记信息，然后在不持 mu_ 的情况下控制会话——
    // 消费者的 OnEvent 需要 mu_ 才能结束，且必须等 ProcessTrace
    // 返回后才能 join（否则死锁）。
    TRACEHANDLE session = 0;
    std::wstring name;
    std::vector<BYTE> props;
    {
        std::lock_guard<std::mutex> lock(mu_);
        session = session_;
        session_ = 0;  // 先标记为已停止 => Stop/Start 幂等
        name = sessionName_;
        props = stopProps_;
        stopProps_.clear();
    }

    if (!props.empty()) {
        auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(props.data());
        ::ControlTraceW(session, name.c_str(), p, EVENT_TRACE_CONTROL_STOP);
    }
    // V25 P0-2：无条件 join——Running() 的失效检测会先行作废 session_
    //（外部同名会话顶替/停止），此时绝不能跳过收尸，否则可 join 的
    // std::thread 随析构触发 std::terminate（fail-fast 崩溃）。
    if (consumer_.joinable()) consumer_.join();  // ProcessTrace 返回后即退出
    if (session != 0) ::CloseTrace(session);     // 最后关闭会话句柄
    STM_LOG_INFO("etw", Fmt(L"ETW 会话 {} 已停止", name));
}

bool EtwNetCollector::Running() {
    TRACEHANDLE session = 0;
    std::wstring name;
    GUID myGuid{};
    {
        std::lock_guard<std::mutex> lock(mu_);
        session = session_;
        name = sessionName_;
        myGuid = myGuid_;
    }
    if (session == 0) return false;
    // V25 P0-2 失效检测：句柄仍在但会话已被外部停止（同名"启动前清残留"
    // 或全局清理）。仅按名字 QUERY 不够——同名新会话会顶替出现，因此
    // 再比对内核回填的会话实例 GUID：不一致即"我"的会话已不在。
    std::vector<BYTE> buf(PropsSize(name) + 512);
    auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
    p->Wnode.BufferSize = static_cast<ULONG>(buf.size());
    p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    const ULONG rc = ::ControlTraceW(0, name.c_str(), p, EVENT_TRACE_CONTROL_QUERY);
    const bool alive = (rc == ERROR_SUCCESS || rc == ERROR_MORE_DATA) &&
                       (InlineIsEqualGUID(myGuid, GUID{}) ||
                        InlineIsEqualGUID(p->Wnode.Guid, myGuid));
    if (alive) return true;
    std::lock_guard<std::mutex> lock(mu_);
    session_ = 0;
    stopProps_.clear();
    myGuid_ = GUID{};
    STM_LOG_WARN("etw", Fmt(L"ETW 会话 {} 已在外部失效（QUERY Win32 {}），标记为停止", name, rc));
    return false;
}

void EtwNetCollector::CopyCumulative(std::unordered_map<uint32_t, uint64_t>* out) const {
    std::lock_guard<std::mutex> lock(mu_);
    out->clear();
    out->reserve(bytes_.size());
    for (const auto& kv : bytes_) {
        (*out)[kv.first] = kv.second.recv + kv.second.send;
    }
}

uint64_t EtwNetCollector::TotalEvents() const {
    std::lock_guard<std::mutex> lock(mu_);
    return totalEvents_;
}

void EtwNetCollector::CopyEndpoints(EndpointMap* out) const {
    std::lock_guard<std::mutex> lock(mu_);
    *out = endpoints_;
}

EtwNetCollector::FieldStats EtwNetCollector::CopyFieldStats() const {
    std::lock_guard<std::mutex> lock(mu_);
    return fieldStats_;
}

EtwNetCollector::RawFieldSample EtwNetCollector::CopyRawSample() const {
    std::lock_guard<std::mutex> lock(mu_);
    return rawSample_;
}

}  // namespace cd
}  // namespace stm
