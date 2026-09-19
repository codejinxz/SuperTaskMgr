// NetMonitor.cpp — 第 10 维护轮（契约 NetMonitor.h，冻结）。
// 连接级实时网络监视（Wireshark 式"连接/流"层，非抓包层）：
//  - 连接事件流：内部轻量线程 1s 节拍 SnapshotConnections 差分
//   （DiffConnSnapshots 纯函数）；免管理员。首个节拍只建基线，不为
//   开启瞬间的存量连接刷屏。事件进 kNetEventCap 环形（满时覆盖最旧并
//   计数——保新弃旧，绝不阻塞轮询线程）。
//  - 按远程端点流量：扩展 EtwNetCollector（见 NetTables.cpp）解析
//   saddr/daddr/sport/dport/proto 后按 (pid, 远程 ip:port, proto) 聚合；
//   字段取不到退化为仅-pid 级并计数。需管理员。
//  - DNS 解析记录：DnsCollector 订阅 Microsoft-Windows-DNS-Client
//   （实验性，默认关）；解不出域名/消费线程夭折时自动禁用并记错误。
// pid -> 进程名统一走 5s TTL 的 Toolhelp 快照缓存（ProcNameCache）。
#include <winsock2.h>  // 必须在 windows.h/iphlpapi 之前（LEAN_AND_MEAN 会隐藏 winsock）
#include <ws2tcpip.h>
#include "collect/CollectDetail.h"
#include "collect/NetMonitor.h"
#include "collect/NetTables.h"
#include "core/Log.h"
#include "core/Str.h"
#include <iphlpapi.h>
#include <tdh.h>
#include <algorithm>
#include <condition_variable>
#include <ctime>
#include <exception>
#include <utility>

namespace stm {

// ===========================================================================
// ServiceNameForPort（契约纯函数）：知名端口 -> 服务名；未知返回空。
// ===========================================================================
std::wstring ServiceNameForPort(uint16_t port, bool isUdp) {
    if (isUdp) {
        switch (port) {
            case 53: return L"DNS";
            case 67: return L"DHCP";
            case 68: return L"DHCP";
            case 123: return L"NTP";
            case 137: return L"NetBIOS-NS";
            case 138: return L"NetBIOS-DGM";
            case 139: return L"NetBIOS";
            case 1900: return L"SSDP";
            case 5353: return L"mDNS";
            default: return L"";
        }
    }
    switch (port) {
        case 20: return L"FTP-Data";
        case 21: return L"FTP";
        case 22: return L"SSH";
        case 23: return L"Telnet";
        case 25: return L"SMTP";
        case 53: return L"DNS";
        case 80: return L"HTTP";
        case 110: return L"POP3";
        case 143: return L"IMAP";
        case 443: return L"HTTPS";
        case 445: return L"SMB";
        case 993: return L"IMAPS";
        case 995: return L"POP3S";
        case 1433: return L"MSSQL";
        case 3306: return L"MySQL";
        case 3389: return L"RDP";
        case 5432: return L"PostgreSQL";
        case 8080: return L"HTTP-Alt";
        case 27017: return L"MongoDB";
        default: return L"";
    }
}

// ===========================================================================
// DiffConnSnapshots（契约级纯函数，声明在内部头 CollectDetail.h）：相邻
// 两次快照按四元组差分。UDP 族不产事件；监听消失不算关闭。
// ===========================================================================
namespace {

struct QuadKey {
    std::wstring local, remote;
    uint16_t lp = 0, rp = 0;
    bool operator==(const QuadKey& o) const {
        return lp == o.lp && rp == o.rp && local == o.local && remote == o.remote;
    }
};

struct QuadKeyHash {
    size_t operator()(const QuadKey& k) const noexcept {
        uint64_t h = 1469598103934665603ull;
        const auto mix = [&h](const void* p, size_t n) {
            const auto* b = static_cast<const unsigned char*>(p);
            for (size_t i = 0; i < n; ++i) {
                h ^= b[i];
                h *= 1099511628211ull;
            }
        };
        mix(&k.lp, sizeof k.lp);
        mix(&k.rp, sizeof k.rp);
        mix(k.local.data(), k.local.size() * sizeof(wchar_t));
        mix(k.remote.data(), k.remote.size() * sizeof(wchar_t));
        return static_cast<size_t>(h);
    }
};

}  // namespace

void DiffConnSnapshots(const std::vector<ConnEntry>& prev, const std::vector<ConnEntry>& cur,
                       int64_t unixTime, ConnProto proto, std::vector<ConnEvent>* events) {
    if (events == nullptr) return;
    // V25 P0-1：追加语义（不清空 *events）——PollLoop 用同一 vector 连续
    // 做 Tcp4/Tcp6 双族差分；此前"进入即 clear"会把先差分族的事件整体
    // 清空。需要隔离的调用方自行 clear（单测沿用）。
    if (proto == ConnProto::Udp4 || proto == ConnProto::Udp6) return;  // UDP 不产事件

    const auto keyOf = [](const ConnEntry& c) {
        QuadKey k;
        k.local = c.localAddr;
        k.remote = c.remoteAddr;
        k.lp = c.localPort;
        k.rp = c.remotePort;
        return k;
    };
    const auto push = [&](const ConnEntry& c, ConnEvent::Kind kind, uint32_t labelState) {
        ConnEvent e;
        e.unixTime = unixTime;
        e.kind = kind;
        e.proto = static_cast<uint32_t>(proto);
        e.localAddr = c.localAddr;
        e.localPort = c.localPort;
        e.remoteAddr = c.remoteAddr;
        e.remotePort = c.remotePort;
        e.stateLabel = TcpStateLabel(labelState);
        e.pid = c.pid;
        events->push_back(std::move(e));
    };

    // 建索引（只含指定协议族的行）。
    std::unordered_map<QuadKey, const ConnEntry*, QuadKeyHash> prevIdx, curIdx;
    prevIdx.reserve(prev.size());
    curIdx.reserve(cur.size());
    for (const ConnEntry& c : prev) {
        if (c.proto == proto) prevIdx.emplace(keyOf(c), &c);
    }

    // 新出现 / 状态变化（按 cur 顺序）。
    for (const ConnEntry& c : cur) {
        if (c.proto != proto) continue;
        const QuadKey k = keyOf(c);
        const auto it = prevIdx.find(k);
        curIdx.emplace(k, &c);
        if (it == prevIdx.end()) {
            push(c, ConnEvent::Kind::New, c.state);
        } else if (it->second->state != c.state) {
            push(c, ConnEvent::Kind::StateChanged, c.state);
        }
    }
    // 消失（按 prev 顺序）；监听套接字消失不产 Closed。
    for (const ConnEntry& p : prev) {
        if (p.proto != proto) continue;
        if (p.state == MIB_TCP_STATE_LISTEN) continue;
        if (curIdx.find(keyOf(p)) == curIdx.end()) {
            push(p, ConnEvent::Kind::Closed, p.state);
        }
    }
}

// ===========================================================================
// cd：ProcNameCache 与 DnsCollector。
// ===========================================================================
namespace cd {

namespace {

// Microsoft-Windows-DNS-Client（用户态清单提供者；私有实时会话需管理员）。
constexpr GUID kDnsClientGuid = {
    0x1c95126e, 0x7eea, 0x49a9, {0xa3, 0xfe, 0xa3, 0x78, 0xb0, 0x3d, 0xdb, 0x4d}};
constexpr USHORT kDnsEvtQuery = 3006;     // DNS 客户端"查询"事件
constexpr USHORT kDnsEvtResponse = 3008;  // DNS 客户端"响应"事件

// tdh.dll 按名解码载荷——与 NetTables.cpp 中的孪生（独立编译单元各自动态
// 绑定；stm_collect 不链接 tdh.lib）。
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

// 按名称读取 unicode 字符串载荷属性（域名）。空白/根域视为失败。
bool WideStrProp(PEVENT_RECORD rec, const wchar_t* name, std::wstring* out) {
    const TdhFns f = Tdh();
    if (!f.get || !f.getSize) return false;
    PROPERTY_DATA_DESCRIPTOR d{};
    d.PropertyName = reinterpret_cast<ULONGLONG>(name);
    d.ArrayIndex = ULONG_MAX;
    ULONG size = 0;
    if (f.getSize(rec, 0, nullptr, 1, &d, &size) != ERROR_SUCCESS || size == 0 || size > 2048 ||
        (size % 2) != 0) {
        return false;
    }
    wchar_t buf[1025] = {};  // 2048 字节 = 1024 字符 + 强制终止位
    if (f.get(rec, 0, nullptr, 1, &d, size, reinterpret_cast<PBYTE>(buf)) != ERROR_SUCCESS) {
        return false;
    }
    buf[1024] = L'\0';  // 防不带回终止的载荷
    std::wstring s(buf);  // 截到首个 \0
    if (s.size() <= 1) return false;  // 空串/根域“.”
    *out = std::move(s);
    return true;
}

size_t PropsSize(const std::wstring& name) {
    return sizeof(EVENT_TRACE_PROPERTIES) + (name.size() + 1) * sizeof(wchar_t);
}

}  // namespace

// ---------------------------------------------------------------------------
// ProcNameCache：5s TTL 的 pid -> 进程名。
// ---------------------------------------------------------------------------
void ProcNameCache::RefreshLocked() {
    stamp_ = std::chrono::steady_clock::now();  // 失败也不重锤：5s 后再试
    std::vector<ToolhelpRow> rows;
    if (ToolhelpEnumerate(&rows)) {
        byPid_.clear();
        byPid_.reserve(rows.size());
        for (const ToolhelpRow& r : rows) byPid_.emplace(r.pid, r.name);
    }
}

std::wstring ProcNameCache::NameOf(uint32_t pid) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto now = std::chrono::steady_clock::now();
    if (byPid_.empty() || now - stamp_ > std::chrono::seconds(5)) RefreshLocked();
    const auto it = byPid_.find(pid);
    return it == byPid_.end() ? std::wstring() : it->second;
}

// ---------------------------------------------------------------------------
// DnsCollector：私有实时 ETW 会话订阅 DNS-Client 提供者。
// ---------------------------------------------------------------------------
DnsCollector::~DnsCollector() { Stop(); }

bool DnsCollector::SessionExists(const wchar_t* name) {
    std::vector<BYTE> buf(PropsSize(name) + 512);
    auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
    p->Wnode.BufferSize = static_cast<ULONG>(buf.size());
    p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    const ULONG rc = ::ControlTraceW(0, name, p, EVENT_TRACE_CONTROL_QUERY);
    return rc == ERROR_SUCCESS || rc == ERROR_MORE_DATA;
}

bool DnsCollector::Start() {
    std::lock_guard<std::mutex> lock(mu_);
    if (session_ != 0) return true;

    // 唯一会话名（带 pid）；先清残留同名会话（防孤儿）——同 EtwNetCollector。
    sessionName_ = Fmt(L"SuperTaskMgr-Dns-{}", ::GetCurrentProcessId());
    stopProps_.assign(PropsSize(sessionName_), 0);
    {
        auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(stopProps_.data());
        p->Wnode.BufferSize = static_cast<ULONG>(stopProps_.size());
        p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        ::ControlTraceW(0, sessionName_.c_str(), p, EVENT_TRACE_CONTROL_STOP);
    }

    // DNS 事件很小：16KB x [8,32] 缓冲，1s 刷新。
    std::vector<BYTE> propsBuf(PropsSize(sessionName_));
    auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propsBuf.data());
    p->Wnode.BufferSize = static_cast<ULONG>(propsBuf.size());
    p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p->Wnode.ClientContext = 1;
    p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    p->BufferSize = 16;
    p->MinimumBuffers = 8;
    p->MaximumBuffers = 32;
    p->FlushTimer = 1;
    p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    TRACEHANDLE h = 0;
    ULONG rc = ::StartTraceW(&h, sessionName_.c_str(), p);
    if (rc != ERROR_SUCCESS) {
        STM_LOG_ERROR("netmon", Fmt(L"DNS StartTraceW({}) 失败（Win32 {}）：需要管理员权限",
                                    sessionName_, rc));
        stopProps_.clear();
        return false;
    }
    rc = ::EnableTraceEx2(h, &kDnsClientGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                          TRACE_LEVEL_INFORMATION, 0, 0, 0, nullptr);
    if (rc != ERROR_SUCCESS) {
        STM_LOG_ERROR("netmon", Fmt(L"EnableTraceEx2(DNS-Client) 失败（Win32 {}），会话已停止", rc));
        ::ControlTraceW(h, sessionName_.c_str(), p, EVENT_TRACE_CONTROL_STOP);
        stopProps_.clear();
        return false;
    }

    stopProps_ = propsBuf;
    myGuid_ = reinterpret_cast<const EVENT_TRACE_PROPERTIES*>(propsBuf.data())->Wnode.Guid;
    session_ = h;
    pending_.clear();
    raw_ = 0;
    decoded_ = 0;
    dropped_ = 0;
    consumerDead_ = false;

    try {
        consumer_ = std::thread(&DnsCollector::Consume, this);
    } catch (const std::exception& e) {
        STM_LOG_ERROR("netmon", Fmt(L"DNS 消费线程启动失败：{}", Utf8ToWide(e.what())));
        ::ControlTraceW(h, sessionName_.c_str(), p, EVENT_TRACE_CONTROL_STOP);
        session_ = 0;
        stopProps_.clear();
        return false;
    }
    STM_LOG_INFO("netmon", Fmt(L"DNS ETW 会话 {} 已启动（实验性）", sessionName_));
    return true;
}

void DnsCollector::Consume() {
    EVENT_TRACE_LOGFILEW logf{};
    logf.LoggerName = &sessionName_[0];
    logf.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_REAL_TIME;
    logf.EventRecordCallback = &DnsCollector::OnEvent;
    logf.Context = this;
    const TRACEHANDLE h = ::OpenTraceW(&logf);
    if (h == INVALID_PROCESSTRACE_HANDLE) {
        std::lock_guard<std::mutex> lock(mu_);
        consumerDead_ = true;  // 由 NetMonitor::DrainDns 轮询到后自动禁用
        STM_LOG_ERROR("netmon", Fmt(L"DNS OpenTraceW 失败（Win32 {}）", ::GetLastError()));
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        openTrace_ = h;
    }
    TRACEHANDLE hs[1] = {h};
    ::ProcessTrace(hs, 1, nullptr, nullptr);
    ::CloseTrace(h);
    {
        std::lock_guard<std::mutex> lock(mu_);
        openTrace_ = 0;
    }
}

void WINAPI DnsCollector::OnEvent(PEVENT_RECORD rec) {
    auto* self = static_cast<DnsCollector*>(rec->UserContext);
    if (self) self->HandleEvent(rec);
}

void DnsCollector::HandleEvent(PEVENT_RECORD rec) {
    if (!InlineIsEqualGUID(rec->EventHeader.ProviderId, kDnsClientGuid)) return;
    {
        std::lock_guard<std::mutex> lock(mu_);
        ++raw_;
    }
    const USHORT id = rec->EventHeader.EventDescriptor.Id;
    if (id != kDnsEvtQuery && id != kDnsEvtResponse) return;

    // 域名字段按候选表尝试（清单跨版本字段名不变量未知——诚实尝试）。
    std::wstring query;
    bool ok = false;
    for (const wchar_t* name : {L"QueryName", L"Query", L"DomainName"}) {
        if (WideStrProp(rec, name, &query)) {
            ok = true;
            break;
        }
    }
    if (!ok) return;  // 解不出域名：DecodedEvents 不前进（自动禁用据此判断）

    DnsEvent ev;
    ev.unixTime = static_cast<int64_t>(::time(nullptr));
    // 用户态提供者：事件头 ProcessId 即发起查询的进程。
    uint32_t pid = rec->EventHeader.ProcessId;
    if (uint32_t p = 0; U32Prop(rec, L"ProcessId", &p) && p != 0) pid = p;
    ev.pid = pid;
    ev.query = std::move(query);

    std::lock_guard<std::mutex> lock(mu_);
    ++decoded_;
    if (pending_.size() < kPendingCap) {
        pending_.push_back(std::move(ev));
    } else {
        ++dropped_;  // V25 P2：待取缓冲满——诚实计数（DrainDns 变化时告警）
    }
}

void DnsCollector::Stop() {
    TRACEHANDLE session = 0;
    std::wstring name;
    std::vector<BYTE> props;
    {
        std::lock_guard<std::mutex> lock(mu_);
        session = session_;
        session_ = 0;  // 先标记停止 => Stop/Start 幂等
        name = sessionName_;
        props = stopProps_;
        stopProps_.clear();
    }
    if (!props.empty()) {
        auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(props.data());
        ::ControlTraceW(session, name.c_str(), p, EVENT_TRACE_CONTROL_STOP);
    }
    // V25 P0-2：无条件 join（失效检测作废 session_ 后也必须收尸，
    // 否则可 join 线程随析构触发 std::terminate）。
    if (consumer_.joinable()) consumer_.join();
    if (session != 0) ::CloseTrace(session);
    STM_LOG_INFO("netmon", Fmt(L"DNS ETW 会话 {} 已停止", name));
}

bool DnsCollector::Running() {
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
    // V25 P0-2 失效检测（同 EtwNetCollector）：名字在 ≠ 我还在，需比对
    // 会话实例 GUID——同名新会话顶替时必须诚实报 false。
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
    STM_LOG_WARN("netmon", Fmt(L"DNS ETW 会话 {} 已在外部失效（QUERY Win32 {}），标记为停止", name, rc));
    return false;
}

void DnsCollector::DrainEvents(std::vector<DnsEvent>* out) {
    std::lock_guard<std::mutex> lock(mu_);
    out->clear();
    out->reserve(pending_.size());
    for (DnsEvent& e : pending_) out->push_back(std::move(e));
    pending_.clear();
}

uint64_t DnsCollector::RawEvents() const {
    std::lock_guard<std::mutex> lock(mu_);
    return raw_;
}

uint64_t DnsCollector::DecodedEvents() const {
    std::lock_guard<std::mutex> lock(mu_);
    return decoded_;
}

uint64_t DnsCollector::DroppedEvents() const {
    std::lock_guard<std::mutex> lock(mu_);
    return dropped_;
}

bool DnsCollector::ConsumerDead() const {
    std::lock_guard<std::mutex> lock(mu_);
    return consumerDead_;
}

}  // namespace cd

// ===========================================================================
// NetMonitor（契约 NetMonitor.h）。
// ===========================================================================
struct NetMonitor::Impl {
    static constexpr size_t kDnsRingCap = 1024;       // DNS 环形容量（内部约定）
    static constexpr int64_t kDnsDedupeWindowSec = 2; // 同 pid 同域名去重窗口
    static constexpr uint64_t kDnsDecodeProbe = 32;   // 收到 N 个事件仍 0 解码 => 禁用

    Impl() {
        // 与 CollectService 的 L"SuperTaskMgr-Net-<pid>" 错开，互不挤掉。
        etw.SetSessionName(Fmt(L"SuperTaskMgr-NetMon-{}", ::GetCurrentProcessId()));
    }
    ~Impl() {
        SetEventCapture(false);
        etw.Stop();
        dns.Stop();
    }

    bool SetEventCapture(bool on);
    bool SetRemoteTraffic(bool on);
    bool SetDnsCapture(bool on, std::wstring* err);
    void PollLoop();

    std::atomic<bool> eventsOn{false};
    std::atomic<bool> trafficOn{false};
    std::atomic<bool> dnsOn{false};

    std::mutex lcmu;  // 生命周期串行化（enable/disable/析构互斥，join 不持 mu）
    std::mutex mu;    // 保护下方轮询簿记 + 环 + DNS 去重表
    std::condition_variable cv;
    bool stop = false;
    bool havePrev = false;
    std::vector<ConnEntry> prev;
    std::thread poller;

    cd::EventRing<ConnEvent> ring{kNetEventCap};
    cd::EventRing<DnsEvent> dnsRing{kDnsRingCap};
    std::vector<std::pair<std::pair<uint32_t, std::wstring>, int64_t>> dnsSeen;
    std::wstring dnsLastError;  // 最近一次自动禁用原因（日志；再次启用时清空）
    std::wstring lastSnapErr;   // 上一轮快照错误（去重记日志；mu 保护）
    uint64_t dnsDropsSeen = 0;  // 上次记录的 DNS 累计丢弃数（变化时告警）

    cd::EtwNetCollector etw;
    cd::DnsCollector dns;
    cd::ProcNameCache names;
};

bool NetMonitor::Impl::SetEventCapture(bool on) {
    std::lock_guard<std::mutex> lc(lcmu);
    if (on) {
        std::lock_guard<std::mutex> lock(mu);
        if (eventsOn.load()) return true;
        stop = false;
        havePrev = false;
        prev.clear();
        std::thread t;
        try {
            t = std::thread(&Impl::PollLoop, this);
        } catch (const std::exception& e) {
            STM_LOG_ERROR("netmon", Fmt(L"连接事件轮询线程启动失败：{}", Utf8ToWide(e.what())));
            return false;
        }
        poller = std::move(t);
        eventsOn.store(true);
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!eventsOn.load()) return true;
        stop = true;
    }
    cv.notify_all();
    if (poller.joinable()) poller.join();
    std::lock_guard<std::mutex> lock(mu);
    eventsOn.store(false);
    stop = false;
    havePrev = false;
    prev.clear();
    return true;
}

void NetMonitor::Impl::PollLoop() {
    for (;;) {
        std::unique_lock<std::mutex> lock(mu);
        if (cv.wait_for(lock, std::chrono::seconds(1), [this] { return stop; })) return;
        std::wstring serr;
        std::vector<ConnEntry> cur = SnapshotConnections(&serr);
        // V25 P1-1：任一族查询失败（或整表为空的异常情况）即保持基线、
        // 本轮不做差分——用残表替换基线会把"查询失败"伪造成大规模
        // Closed/New 风暴。错误去重后记日志（恢复时静默）。
        if (!serr.empty() || cur.empty()) {
            if (serr != lastSnapErr) {
                STM_LOG_WARN("netmon", serr.empty()
                                           ? L"连接表快照异常为空，本轮保持基线不做差分"
                                           : L"连接表部分族查询失败，本轮保持基线：" + serr);
                lastSnapErr = serr;
            }
            continue;
        }
        if (!lastSnapErr.empty()) lastSnapErr.clear();  // 恢复正常：静默复位
        if (!havePrev) {
            prev = std::move(cur);  // 首个节拍只建基线（不为存量连接刷屏）
            havePrev = true;
            continue;
        }
        const int64_t now = static_cast<int64_t>(::time(nullptr));
        std::vector<ConnEvent> evs;
        DiffConnSnapshots(prev, cur, now, ConnProto::Tcp4, &evs);
        DiffConnSnapshots(prev, cur, now, ConnProto::Tcp6, &evs);
        for (ConnEvent& e : evs) {
            if (e.pid != 0) e.processName = names.NameOf(e.pid);
            ring.Push(std::move(e));
        }
        prev = std::move(cur);
    }
}

bool NetMonitor::Impl::SetRemoteTraffic(bool on) {
    // 持 mu 调 Start/Stop 安全：EtwNetCollector 的消费者回调只碰它自己的
    // mu_，从不触碰 Impl::mu。
    std::lock_guard<std::mutex> lock(mu);
    if (on) {
        if (trafficOn.load()) {
            if (etw.Running()) return true;
            trafficOn.store(false);  // V25 P0-2：会话已被外部杀死 → 走重启路径
        }
        if (!etw.Start()) return false;  // 非管理员等：保持禁用（诚实）
        trafficOn.store(true);
        return true;
    }
    if (!trafficOn.load()) return true;
    etw.Stop();
    trafficOn.store(false);
    return true;
}

bool NetMonitor::Impl::SetDnsCapture(bool on, std::wstring* err) {
    std::lock_guard<std::mutex> lock(mu);
    if (on) {
        if (dnsOn.load()) {
            if (dns.Running()) return true;
            dnsOn.store(false);  // V25 P0-2：会话已被外部杀死 → 走重启路径
        }
        dnsLastError.clear();
        if (!dns.Start()) {
            if (err != nullptr) {
                *err = L"DNS 捕获启动失败（需要管理员权限；实验性功能）";
            }
            return false;
        }
        dnsOn.store(true);
        return true;
    }
    if (!dnsOn.load()) return true;
    dns.Stop();
    dnsOn.store(false);
    dnsLastError.clear();
    return true;
}

NetMonitor::NetMonitor() : impl_(new Impl()) {}

NetMonitor::~NetMonitor() { delete impl_; }  // Impl 析构：全停（轮询 + 两个会话）

bool NetMonitor::SetEventCapture(bool on) { return impl_->SetEventCapture(on); }

bool NetMonitor::EventCaptureEnabled() const { return impl_->eventsOn.load(); }

bool NetMonitor::SetRemoteTraffic(bool on) { return impl_->SetRemoteTraffic(on); }

bool NetMonitor::RemoteTrafficEnabled() const {
    // V25 P0-2 诚实读回：意图开启但会话已被外部杀死（如并行实例同名
    // 清理）时返回 false（Running() 内部会检测并作废句柄）。
    return impl_->trafficOn.load() && impl_->etw.Running();
}

bool NetMonitor::SetDnsCapture(bool on, std::wstring* err) {
    return impl_->SetDnsCapture(on, err);
}

bool NetMonitor::DnsCaptureEnabled() const {
    // 同上：意图 + 会话存活双重诚实读回。
    return impl_->dnsOn.load() && impl_->dns.Running();
}

void NetMonitor::DrainEvents(std::vector<ConnEvent>* out) {
    if (out == nullptr) return;
    impl_->ring.Drain(out);
}

void NetMonitor::DrainDns(std::vector<DnsEvent>* out) {
    if (out == nullptr) return;
    Impl& s = *impl_;
    std::vector<DnsEvent> fresh;
    s.dns.DrainEvents(&fresh);

    std::lock_guard<std::mutex> lock(s.mu);
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    // 去重表按窗口过期清理；同 pid 同域名窗口内只留一条。
    s.dnsSeen.erase(std::remove_if(s.dnsSeen.begin(), s.dnsSeen.end(),
                                   [&](const auto& kv) {
                                       return kv.second < now - s.kDnsDedupeWindowSec;
                                   }),
                    s.dnsSeen.end());
    for (DnsEvent& e : fresh) {
        const auto key = std::make_pair(e.pid, e.query);
        bool dup = false;
        for (const auto& kv : s.dnsSeen) {
            if (kv.first == key) {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        s.dnsSeen.emplace_back(key, e.unixTime);
        s.dnsRing.Push(std::move(e));
    }
    s.dnsRing.Drain(out);

    // V25 P2：DNS 事件因容量丢弃（待取缓冲满 / 环形溢出）——计数变化时
    // 告警，诚实可查（契约头无 DNS 丢弃 getter，日志是唯一出口）。
    const uint64_t dnsDrops = s.dns.DroppedEvents() + s.dnsRing.Dropped();
    if (dnsDrops != s.dnsDropsSeen) {
        STM_LOG_WARN("netmon",
                     Fmt(L"DNS 事件因容量丢弃 {} 条（累计：待取满 {} + 环形溢出 {}）", dnsDrops,
                         s.dns.DroppedEvents(), s.dnsRing.Dropped()));
        s.dnsDropsSeen = dnsDrops;
    }

    // 自动禁用（诚实降级）：解不出域名（收到足够事件仍 0 解码）或消费线程
    // 夭折。之后 DnsCaptureEnabled() 为 false；再次 SetDnsCapture(true) 会
    // 重试（可能是暂时性字段缺失）。
    if (s.dnsOn.load() && s.dns.Running()) {
        const bool dead = s.dns.ConsumerDead();
        const bool blind = s.dns.RawEvents() >= s.kDnsDecodeProbe && s.dns.DecodedEvents() == 0;
        if (dead || blind) {
            s.dnsLastError = blind ? L"DNS 事件字段解析失败（QueryName 不可得），已自动禁用"
                                   : L"DNS 消费线程启动失败，已自动禁用";
            STM_LOG_ERROR("netmon", s.dnsLastError);
            s.dns.Stop();
            s.dnsOn.store(false);
        }
    }
}

std::vector<RemoteTraffic> NetMonitor::TopRemoteTraffic(size_t topN) const {
    cd::EndpointMap m;
    impl_->etw.CopyEndpoints(&m);
    std::vector<RemoteTraffic> out = TopRemoteFromEndpoints(m, topN);
    for (RemoteTraffic& r : out) {
        if (r.pid != 0) r.processName = impl_->names.NameOf(r.pid);
    }
    return out;
}

uint64_t NetMonitor::DroppedEvents() const { return impl_->ring.Dropped(); }

void NetMonitor::ClearEvents() {
    impl_->ring.Clear();
    impl_->dnsRing.Clear();
}

}  // namespace stm
