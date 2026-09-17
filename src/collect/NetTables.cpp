// NetTables.cpp — phase 3.
// Part A (contract NetTables.h): TCP/UDP endpoint tables with owning PIDs via
//   GetExtendedTcpTable / GetExtendedUdpTable (TCP_TABLE_OWNER_PID_ALL /
//   UDP_TABLE_OWNER_PID, v4+v6). Buffer size is grown in a retry loop for
//   ERROR_INSUFFICIENT_BUFFER; addresses are formatted with InetNtopW
//   (RFC 5952 for IPv6, loaded dynamically from ws2_32.dll); PID=0 rows are
//   kernel-bound sockets and are preserved verbatim (never dropped).
// Part B (internal, CollectDetail.h): EtwNetCollector — real-time ETW session
//   on Microsoft-Windows-Kernel-Network (R5 #10b), admin only, aggregating
//   per-pid cumulative recv/send bytes for the CollectService diff.
#include <winsock2.h>  // must precede windows.h/iphlpapi (LEAN_AND_MEAN hides winsock)
#include <ws2tcpip.h>  // pulls ws2ipdef.h -> in6_addr needed by the MIB_TCP6/UDP6 rows
#include "collect/CollectDetail.h"
#include "collect/NetTables.h"
#include "core/Log.h"
#include "core/Str.h"
#include <iphlpapi.h>
#include <tdh.h>
#include <cwchar>

namespace stm {

// ===========================================================================
// Part A: connection tables (contract NetTables.h)
// ===========================================================================
namespace {

constexpr DWORD kInitialTableBytes = 32 * 1024;
constexpr int kMaxTableRetries = 8;

// InetNtopW lives in ws2_32.dll; stm_collect does not link ws2_32, so bind it
// dynamically and keep the module for the process lifetime (like ntdll above).
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
    const auto* b = reinterpret_cast<const uint8_t*>(&a);  // network byte order
    return Fmt(L"{}.{}.{}.{}", b[0], b[1], b[2], b[3]);
}

std::wstring FmtAddrV6(const IN6_ADDR& a) {
    wchar_t buf[46]{};
    const InetNtopWFn fn = InetNtopWProc();
    if (fn && fn(AF_INET6, &a, buf, std::size(buf))) return buf;  // RFC 5952 style
    // Fallback: plain lowercase hex groups (only if ws2_32 were unusable).
    std::wstring s;
    for (int i = 0; i < 8; ++i) {
        if (i) s += L':';
        s += Fmt(L"{:x}", (static_cast<uint16_t>(a.u.Byte[i * 2]) << 8) | a.u.Byte[i * 2 + 1]);
    }
    return s;
}

// Port fields arrive in network byte order in the low 16 bits of a DWORD;
// swapped by hand (ws2_32 ntohs is not linked).
uint16_t NetPort(uint32_t dwPort) {
    const uint32_t raw = dwPort & 0xFFFFu;
    return static_cast<uint16_t>(((raw & 0xFFu) << 8) | ((raw >> 8) & 0xFFu));
}

void NoteErr(std::wstring* errs, const wchar_t* what, DWORD rc) {
    if (!errs) return;
    if (!errs->empty()) *errs += L"；";
    *errs += Fmt(L"{} 失败（Win32 {}）", what, rc);
}

// Grows the buffer through ERROR_INSUFFICIENT_BUFFER retries (the table can
// change between the size probe and the call, hence the loop).
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
        e.pid = r.dwOwningPid;  // 0 = kernel-bound, preserved per contract
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
        default: return Fmt(L"0x{:X}", state);  // unknown states stay honest as hex
    }
}

// ===========================================================================
// Part B: EtwNetCollector (internal, CollectDetail.h). We are still inside
// namespace stm here; the class is declared in stm::cd, so only cd reopens.
// ===========================================================================
namespace cd {

namespace {

// Microsoft-Windows-Kernel-Network (manifest provider, Win10+; can be enabled
// on a normal private real-time session — the same thing `netsh trace` does).
constexpr GUID kKernelNetworkGuid = {
    0x7dd42a49, 0x5329, 0x4832, {0x8d, 0xfd, 0x43, 0xd9, 0x79, 0x15, 0x3a, 0x88}};
constexpr USHORT kEvtRecvData = 10;  // kernel:network:recvdata
constexpr USHORT kEvtSendData = 11;  // kernel:network:senddata

// tdh.dll: decode event payloads BY PROPERTY NAME (no layout guessing across
// OS builds). Dynamically bound; stm_collect does not link tdh.lib.
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

// Reads a uint32 payload property by name. Returns false when absent/undecodable.
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

}  // namespace

EtwNetCollector::~EtwNetCollector() { Stop(); }

bool EtwNetCollector::SessionExists(const wchar_t* name) {
    std::vector<BYTE> buf(PropsSize(name) + 512);
    auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
    p->Wnode.BufferSize = static_cast<ULONG>(buf.size());
    p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    const ULONG rc = ::ControlTraceW(0, name, p, EVENT_TRACE_CONTROL_QUERY);
    // ERROR_MORE_DATA also means "exists" (info larger than our buffer).
    return rc == ERROR_SUCCESS || rc == ERROR_MORE_DATA;
}

bool EtwNetCollector::Start() {
    std::lock_guard<std::mutex> lock(mu_);
    if (session_ != 0) return true;

    // Unique session name (pid) prevents collisions between instances; a stale
    // session left behind by a crashed predecessor is stopped first (anti-orphan).
    sessionName_ = Fmt(L"SuperTaskMgr-Net-{}", ::GetCurrentProcessId());

    stopProps_.assign(PropsSize(sessionName_), 0);
    {
        auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(stopProps_.data());
        p->Wnode.BufferSize = static_cast<ULONG>(stopProps_.size());
        p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        ::ControlTraceW(0, sessionName_.c_str(), p, EVENT_TRACE_CONTROL_STOP);
        // ERROR_WMI_INSTANCE_NOT_FOUND simply means there was no leftover.
    }

    // Real-time session, 64 x 64KB buffers to start (max 128), 1 s flush so the
    // per-second rate differential sees fresh events.
    std::vector<BYTE> propsBuf(PropsSize(sessionName_));
    auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propsBuf.data());
    p->Wnode.BufferSize = static_cast<ULONG>(propsBuf.size());
    p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p->Wnode.ClientContext = 1;  // QPC timestamps
    p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    p->BufferSize = 64;  // KB per buffer
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

    // Keyword 0 = no keyword filtering, but the session enables ONLY the
    // Kernel-Network provider, so only network-class events can arrive; the
    // callback additionally keeps recv/send data events only (ids 10/11).
    rc = ::EnableTraceEx2(h, &kKernelNetworkGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                          TRACE_LEVEL_INFORMATION, 0, 0, 0, nullptr);
    if (rc != ERROR_SUCCESS) {
        STM_LOG_ERROR("etw",
                      Fmt(L"EnableTraceEx2(Kernel-Network) 失败（Win32 {}），会话已停止", rc));
        ::ControlTraceW(h, sessionName_.c_str(), p, EVENT_TRACE_CONTROL_STOP);
        stopProps_.clear();
        return false;
    }

    // Keep the (kernel-updated) properties buffer for the later ControlTraceW
    // stop; it must outlive Start, hence the member copy.
    stopProps_ = propsBuf;
    session_ = h;
    bytes_.clear();
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
    ::ProcessTrace(hs, 1, nullptr, nullptr);  // blocks until the session is stopped
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
    // recvdata=10 / senddata=11 (manifest). Accept the kernel-logger spelling
    // (type in Opcode, Id==0) as well, so legacy sessions still work.
    int dir = 0;
    if (id == kEvtRecvData || (id == 0 && op == kEvtRecvData)) {
        dir = 1;
    } else if (id == kEvtSendData || (id == 0 && op == kEvtSendData)) {
        dir = 2;
    } else {
        return;  // connect/disconnect/retransmit/... — not byte-bearing events
    }
    uint32_t size = 0;
    if (!U32Prop(rec, L"size", &size)) {
        std::lock_guard<std::mutex> lock(mu_);
        ++parseFails_;
        return;
    }
    uint32_t pid = 0;
    if (!U32Prop(rec, L"pid", &pid) || pid == 0) pid = rec->EventHeader.ProcessId;
    if (pid == 0) return;  // no identity: never attribute to a fake owner

    std::lock_guard<std::mutex> lock(mu_);
    Agg& a = bytes_[pid];
    if (dir == 1) {
        a.recv += size;
    } else {
        a.send += size;
    }
    ++totalEvents_;
}

void EtwNetCollector::Stop() {
    // Copy the session bookkeeping under the lock, then control the session
    // WITHOUT holding mu_ — the consumer's OnEvent needs mu_ to finish, and
    // ProcessTrace must return before we can join (otherwise: deadlock).
    TRACEHANDLE session = 0;
    std::wstring name;
    std::vector<BYTE> props;
    {
        std::lock_guard<std::mutex> lock(mu_);
        session = session_;
        session_ = 0;  // marked stopped first => Stop/Start idempotent
        name = sessionName_;
        props = stopProps_;
        stopProps_.clear();
    }
    if (session == 0) return;

    if (!props.empty()) {
        auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(props.data());
        ::ControlTraceW(session, name.c_str(), p, EVENT_TRACE_CONTROL_STOP);
    }
    if (consumer_.joinable()) consumer_.join();  // exits once ProcessTrace returns
    ::CloseTrace(session);                       // close the session handle last
    STM_LOG_INFO("etw", Fmt(L"ETW 会话 {} 已停止", name));
}

bool EtwNetCollector::Running() {
    std::lock_guard<std::mutex> lock(mu_);
    return session_ != 0;
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

}  // namespace cd
}  // namespace stm
