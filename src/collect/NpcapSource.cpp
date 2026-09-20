// 深度抓包源（C2 维护轮）实现：wpcap.dll / packet.dll 全动态绑定 + 单抓包会话。
// 契约见 NpcapSource.h（诚实边界：未安装只报指引；提权不足时 pcap_open_live
// 的失败原文透出；环形溢出保新弃旧并计数）。
//
// 绑定约定（Npcap 官方文档）：System32\Npcap\ 下的 wpcap.dll 依赖同目录的
// packet.dll——必须用 LoadLibraryEx(LOAD_WITH_ALTERED_SEARCH_PATH) 按绝对路径
// 加载，加载器才会到 DLL 自身目录解析依赖；WinPcap 兼容模式安装的
// System32\wpcap.dll 同法可用。全部导出经 GetProcAddress 取用，任何缺失都
// 视为绑定失败（诚实：不半工作）。pcap_sendpacket 在 wpcap.dll 导出；
// packet.dll 作回退绑定源。
#include "collect/NpcapSource.h"
#include "collect/NpcapParse.h"  // collect 层帧解析纯函数（分层：UI 不参与解析）
#include "core/Str.h"
#include <winsock2.h>  // 必须在 windows.h/iphlpapi 之前（LEAN_AND_MEAN 会隐藏 winsock）
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace stm {

// ---------------------------------------------------------------------------
// 安装检测（只查存在性，不加载驱动、免管理员）。
// ---------------------------------------------------------------------------
namespace {

bool FileExists(const std::wstring& path) {
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring SystemDir() {
    wchar_t buf[MAX_PATH]{};
    UINT n = ::GetSystemDirectoryW(buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"C:\\Windows\\System32";
    return buf;
}

bool ServiceExists(const wchar_t* serviceName) {
    SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr) return false;  // 无法访问 SCM：按"服务不可证实"处理
    SC_HANDLE svc = ::OpenServiceW(scm, serviceName, SERVICE_QUERY_STATUS);
    if (svc != nullptr) ::CloseServiceHandle(svc);
    ::CloseServiceHandle(scm);
    return svc != nullptr;
}

}  // namespace

bool NpcapInstalled() {
    if (ServiceExists(L"NPCAP")) return true;
    const std::wstring sys = SystemDir();
    return FileExists(sys + L"\\Npcap\\wpcap.dll") || FileExists(sys + L"\\wpcap.dll");
}

std::wstring NpcapDllPath() {
    const std::wstring sys = SystemDir();
    const std::wstring inDir = sys + L"\\Npcap\\wpcap.dll";
    if (FileExists(inDir)) return inDir;
    const std::wstring inSys = sys + L"\\wpcap.dll";
    if (FileExists(inSys)) return inSys;
    return std::wstring();
}

const wchar_t* NpcapInstallGuidance() {
    return L"深度抓包依赖 Npcap 官方签名驱动（未随本应用分发）。"
           L"请到 https://npcap.com/dist/ 下载 npcap-1.89.exe（最新稳定版），"
           L"双击安装：勾选协议后保持默认选项（含回环抓包支持）即可。"
           L"注：官方免费版安装器不支持静默安装（/S 仅限 Npcap OEM 版，"
           L"免费版 /S 会直接退出）；安装前请核对签名为 Nmap Software LLC"
           L"（原 Insecure.Com LLC）。安装完成后回到本页点击「重新检测」。";
}

const wchar_t* NpcapDownloadUrl() { return L"https://npcap.com/dist/"; }

// ---------------------------------------------------------------------------
// wpcap.dll 动态绑定（进程级单例；失败路径诚实记录 lastError）。
// ---------------------------------------------------------------------------
namespace {

// libpcap/WinPcap ABI 的最小镜像（只含本模块用到的字段；x64 布局）。
struct PcapIf {
    PcapIf* next;
    char* name;
    char* description;
    void* addresses;   // struct pcap_addr*（未用）
    uint32_t flags;    // PCAP_IF_LOOPBACK = 0x00000001
};

// Windows timeval = { long tv_sec; long tv_usec; }（x64 上各 4 字节）。
struct PcapPkthdr {
    int32_t tsSec;
    int32_t tsUsec;
    uint32_t caplen;
    uint32_t len;
};

struct BpfProgram {
    uint32_t bfLen;
    void* bfInsns;  // struct bpf_insn*（不透明）
};

constexpr uint32_t kPcapIfLoopback = 0x00000001u;
// pcap_compile 的 netmask 占位（libpcap ≥1.1；不过滤广播方向的编译语义）。
constexpr uint32_t kPcapNetmaskUnknown = 0xFFFFFFFFu;
// libpcap 的 errbuf 容量（PCAP_ERRBUF_SIZE）。
constexpr size_t kPcapErrbufSize = 256;

// pcap_geterr 的 char* → 宽字符串（空/NULL 一律"未知错误"，绝不伪造）。
std::wstring PcapErrText(const char* e) {
    return e != nullptr && e[0] != '\0' ? std::wstring(e, e + strlen(e))
                                        : std::wstring(L"未知错误");
}

using PcapFindAllDevsFn = int (*)(PcapIf**, char*);
using PcapFreeAllDevsFn = void (*)(PcapIf*);
using PcapOpenLiveFn = void* (*)(const char*, int, int, int, char*);
using PcapCloseFn = void (*)(void*);
using PcapCompileFn = int (*)(void*, BpfProgram*, const char*, int, uint32_t);
using PcapSetFilterFn = int (*)(void*, const BpfProgram*);
using PcapFreeCodeFn = void (*)(BpfProgram*);
using PcapNextExFn = int (*)(void*, PcapPkthdr**, const uint8_t**);
using PcapDataLinkFn = int (*)(void*);
using PcapSendPacketFn = int (*)(void*, const uint8_t*, int);
using PcapGetErrFn = char* (*)(void*);

class WpcapLib {
public:
    static const WpcapLib& Inst() {
        static WpcapLib lib;
        return lib;
    }

    bool ok() const { return ok_; }
    // 绑定失败原因（中文；含 LoadLibrary/GetProcAddress 阶段）。
    const std::wstring& error() const { return err_; }

    PcapFindAllDevsFn findAllDevs = nullptr;
    PcapFreeAllDevsFn freeAllDevs = nullptr;
    PcapOpenLiveFn openLive = nullptr;
    PcapCloseFn close = nullptr;
    PcapCompileFn compile = nullptr;
    PcapSetFilterFn setFilter = nullptr;
    PcapFreeCodeFn freeCode = nullptr;
    PcapNextExFn nextEx = nullptr;
    PcapDataLinkFn datalink = nullptr;
    PcapSendPacketFn sendPacket = nullptr;
    PcapGetErrFn geterr = nullptr;

private:
    WpcapLib() {
        // packet.dll 先行显式加载（同目录依赖）；两者失败路径都如实记录。
        const std::wstring wpcapPath = NpcapDllPath();
        if (wpcapPath.empty()) {
            err_ = L"未找到 wpcap.dll（Npcap 可能未安装）";
            return;
        }
        // packet.dll 与 wpcap.dll 同目录（System32\Npcap\ 或 System32\）。
        const size_t slash = wpcapPath.find_last_of(L'\\');
        const std::wstring dir = slash == std::wstring::npos
                                     ? SystemDir()
                                     : wpcapPath.substr(0, slash);
        const std::wstring packetPath = dir + L"\\packet.dll";
        if (FileExists(packetPath)) {
            packet_ = ::LoadLibraryExW(packetPath.c_str(), nullptr,
                                       LOAD_WITH_ALTERED_SEARCH_PATH);
        }
        wpcap_ = ::LoadLibraryExW(wpcapPath.c_str(), nullptr,
                                  LOAD_WITH_ALTERED_SEARCH_PATH);
        if (wpcap_ == nullptr) {
            err_ = Fmt(L"加载 wpcap.dll 失败（{}）：GetLastError={}", wpcapPath,
                       static_cast<unsigned long>(::GetLastError()));
            FreePacket();
            return;
        }
        // pcap_sendpacket：优先 wpcap.dll 导出；回退 packet.dll（诚实注释：
        // Npcap 1.x 中 wpcap.dll 亦导出该符号；两者都在则用 wpcap 的）。
        bool all = Bind(wpcap_, "pcap_findalldevs", reinterpret_cast<void**>(&findAllDevs));
        all = Bind(wpcap_, "pcap_freealldevs", reinterpret_cast<void**>(&freeAllDevs)) && all;
        all = Bind(wpcap_, "pcap_open_live", reinterpret_cast<void**>(&openLive)) && all;
        all = Bind(wpcap_, "pcap_close", reinterpret_cast<void**>(&close)) && all;
        all = Bind(wpcap_, "pcap_compile", reinterpret_cast<void**>(&compile)) && all;
        all = Bind(wpcap_, "pcap_setfilter", reinterpret_cast<void**>(&setFilter)) && all;
        all = Bind(wpcap_, "pcap_freecode", reinterpret_cast<void**>(&freeCode)) && all;
        all = Bind(wpcap_, "pcap_next_ex", reinterpret_cast<void**>(&nextEx)) && all;
        all = Bind(wpcap_, "pcap_datalink", reinterpret_cast<void**>(&datalink)) && all;
        all = Bind(wpcap_, "pcap_geterr", reinterpret_cast<void**>(&geterr)) && all;
        if (!Bind(wpcap_, "pcap_sendpacket", reinterpret_cast<void**>(&sendPacket)) &&
            (packet_ == nullptr ||
             !Bind(packet_, "pcap_sendpacket", reinterpret_cast<void**>(&sendPacket)))) {
            all = false;
            err_ = L"wpcap.dll / packet.dll 均未导出 pcap_sendpacket";
        }
        if (!all) {
            if (err_.empty()) err_ = L"wpcap.dll 导出表不完整（缺少必需的 pcap_* 函数）";
            FreePacket();
            if (wpcap_ != nullptr) ::FreeLibrary(wpcap_);
            wpcap_ = nullptr;
            return;
        }
        ok_ = true;
    }

    ~WpcapLib() {
        FreePacket();
        if (wpcap_ != nullptr) ::FreeLibrary(wpcap_);
    }

    bool Bind(HMODULE h, const char* name, void** out) {
        *out = reinterpret_cast<void*>(::GetProcAddress(h, name));
        return *out != nullptr;
    }

    void FreePacket() {
        if (packet_ != nullptr) {
            ::FreeLibrary(packet_);
            packet_ = nullptr;
        }
    }

    HMODULE wpcap_ = nullptr;
    HMODULE packet_ = nullptr;
    bool ok_ = false;
    std::wstring err_;
};

// 设备名 "\\Device\\NPF\\{GUID}" → 连接友好名（"以太网"/"WLAN"）。
// 与 GetAdaptersAddresses 的 AdapterName（GUID 文本）尾段做大小写不敏感匹配。
std::wstring FriendlyNameFor(const std::wstring& pcapName) {
    const size_t pos = pcapName.find_last_of(L'\\');
    const std::wstring guid =
        pos == std::wstring::npos ? pcapName : pcapName.substr(pos + 1);
    if (guid.empty()) return std::wstring();

    const std::string guidUtf8 = WideToUtf8(guid);  // AdapterName 是 ANSI GUID 文本
    const ULONG flags =
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER |
        GAA_FLAG_SKIP_UNICAST;  // 只要 AdapterName↔FriendlyName 映射
    for (ULONG size = 16 * 1024;; size *= 2) {
        std::vector<uint8_t> buf(size);
        auto* head = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        const ULONG rc = ::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, head, &size);
        if (rc == ERROR_BUFFER_OVERFLOW && size < 1024 * 1024) continue;
        if (rc != NO_ERROR) return std::wstring();
        for (auto* a = head; a != nullptr; a = a->Next) {
            if (a->AdapterName != nullptr &&
                _stricmp(a->AdapterName, guidUtf8.c_str()) == 0) {
                return a->FriendlyName;
            }
        }
        return std::wstring();  // 表里没有：确无映射（诚实返回空）
    }
}

bool ContainsI(const std::wstring& hay, const wchar_t* needle) {
    std::wstring lower = hay;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    std::wstring n(needle);
    std::transform(n.begin(), n.end(), n.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return lower.find(n) != std::wstring::npos;
}

}  // namespace

std::vector<PcapDevice> ListDevices(std::wstring* err) {
    std::vector<PcapDevice> out;
    const WpcapLib& lib = WpcapLib::Inst();
    if (!lib.ok()) {
        if (err != nullptr) *err = lib.error();
        return out;
    }
    char errbuf[kPcapErrbufSize]{};
    PcapIf* heads = nullptr;
    if (lib.findAllDevs(&heads, errbuf) != 0 || heads == nullptr) {
        if (err != nullptr) {
            *err = Fmt(L"枚举设备失败：{}",
                       heads == nullptr && errbuf[0] == '\0'
                           ? std::wstring(L"未发现任何捕获设备")
                           : std::wstring(errbuf, errbuf + strlen(errbuf)));
        }
        if (heads != nullptr) lib.freeAllDevs(heads);
        return out;
    }
    for (PcapIf* d = heads; d != nullptr; d = d->next) {
        PcapDevice dev;
        dev.name = Utf8ToWide(d->name != nullptr ? d->name : "");
        if (dev.name.empty()) continue;
        dev.description = Utf8ToWide(d->description != nullptr ? d->description : "");
        dev.friendlyName = FriendlyNameFor(dev.name);
        dev.loopback =
            (d->flags & kPcapIfLoopback) != 0 || ContainsI(dev.name, L"loopback");
        out.push_back(std::move(dev));
    }
    lib.freeAllDevs(heads);
    if (out.empty() && err != nullptr) {
        *err = L"未发现任何捕获设备（Npcap 已安装但无可用适配器）";
    }
    return out;
}

// ---------------------------------------------------------------------------
// NpcapSource::Impl：单会话（pcap_t + 消费线程 + 环形）。
// ---------------------------------------------------------------------------
struct NpcapSource::Impl {
    ~Impl() { Stop(); }

    void Stop() {
        stop_.store(true, std::memory_order_relaxed);
        if (thread_.joinable()) thread_.join();
        if (pcap_ != nullptr) {
            WpcapLib::Inst().close(pcap_);
            pcap_ = nullptr;
        }
        running_.store(false, std::memory_order_relaxed);
    }

    // 消费线程：pcap_next_ex 循环 → 帧解析（collect 层 NpcapParse.h 纯逻辑）
    // → 环形。链路层类型在 StartCapture 里于开线程前写入（std::thread
    // 构造建立 happens-before，无数据竞争）。
    void ConsumeLoop() {
        const WpcapLib& lib = WpcapLib::Inst();
        const PcapLinkKind linkKind = linkKind_;
        const int dlt = dlt_;
        while (!stop_.load(std::memory_order_relaxed)) {
            PcapPkthdr* hdr = nullptr;
            const uint8_t* data = nullptr;
            const int rc = lib.nextEx(pcap_, &hdr, &data);
            if (rc == 0) continue;  // 1s 读超时：回去查停止标志
            if (rc < 0) {
                std::lock_guard<std::mutex> lock(mu_);
                lastErr_ = L"读取数据包失败：" +
                           PcapErrText(lib.geterr(pcap_));  // PCAP_ERROR_BREAK(-1) 等
                break;
            }
            if (hdr == nullptr || data == nullptr) continue;
            const int64_t tsMs = static_cast<int64_t>(hdr->tsSec) * 1000 +
                                 static_cast<int64_t>(hdr->tsUsec) / 1000;
            PktRecord rec;
            switch (linkKind) {
                case PcapLinkKind::Ethernet:
                    rec = ParseEthernetFrame(data, hdr->caplen, tsMs);
                    break;
                case PcapLinkKind::NullLoopback:
                    rec = ParseLoopbackFrame(data, hdr->caplen, tsMs);
                    break;
                default:
                    rec = ParseUnknownLinkFrame(data, hdr->caplen, tsMs, dlt);
                    break;
            }
            std::lock_guard<std::mutex> lock(mu_);
            if (ring_.size() >= kPktRingCap) {
                ring_.pop_front();  // 保新弃旧 + 诚实计数
                ++dropped_;
            }
            ring_.push_back(std::move(rec));
            ++captured_;
        }
    }

    void* pcap_ = nullptr;              // pcap_t*
    PcapLinkKind linkKind_ = PcapLinkKind::Unknown;
    int dlt_ = -1;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    mutable std::mutex mu_;
    std::deque<PktRecord> ring_;
    uint64_t dropped_ = 0;
    uint64_t captured_ = 0;
    std::wstring lastErr_;
};

NpcapSource::NpcapSource() : impl_(new Impl()) {}

NpcapSource::~NpcapSource() {
    StopCapture();
    delete impl_;
}

bool NpcapSource::StartCapture(const std::wstring& deviceName,
                               const std::wstring& bpfFilter, std::wstring* err) {
    const WpcapLib& lib = WpcapLib::Inst();
    if (!lib.ok()) {
        if (err != nullptr) *err = lib.error();
        return false;
    }
    if (deviceName.empty()) {
        if (err != nullptr) *err = L"未选择捕获设备";
        return false;
    }
    // 幂等：已在跑则先停旧会话（RAII 语义由 Impl::Stop 保证）。
    StopCapture();

    // pcap_open_live：snaplen 65535（全帧）、混杂模式、1s 读超时。
    char errbuf[kPcapErrbufSize]{};
    const std::string nameUtf8 = WideToUtf8(deviceName);
    void* pcap = lib.openLive(nameUtf8.c_str(), 65535, /*promisc=*/1, /*timeoutMs=*/1000,
                              errbuf);
    if (pcap == nullptr) {
        if (err != nullptr) {
            std::wstring detail = errbuf[0] != '\0'
                                      ? std::wstring(errbuf, errbuf + strlen(errbuf))
                                      : std::wstring(L"未知错误");
            *err = Fmt(L"打开设备失败：{}（常见原因：缺少管理员权限、设备已被"
                       L"其他抓包工具独占）",
                       detail);
        }
        return false;
    }
    impl_->pcap_ = pcap;
    // 链路层类型：开消费线程前写入（线程构造建立 happens-before）。
    impl_->dlt_ = lib.datalink(pcap);
    impl_->linkKind_ = PcapLinkKindFromDlt(impl_->dlt_);
    // BPF 过滤（可为空 = 不过滤）。编译失败必须释放 pcap_t 并如实回传 pcap_geterr。
    if (!bpfFilter.empty()) {
        BpfProgram prog{};
        const std::string filterUtf8 = WideToUtf8(bpfFilter);
        if (lib.compile(pcap, &prog, filterUtf8.c_str(), /*optimize=*/1,
                        kPcapNetmaskUnknown) != 0) {
            std::wstring detail = PcapErrText(lib.geterr(pcap));
            if (err != nullptr) {
                *err = Fmt(L"BPF 过滤器编译失败：{}", detail);
            }
            lib.close(pcap);
            impl_->pcap_ = nullptr;
            return false;
        }
        if (lib.setFilter(pcap, &prog) != 0) {
            std::wstring detail = PcapErrText(lib.geterr(pcap));
            lib.freeCode(&prog);
            lib.close(pcap);
            impl_->pcap_ = nullptr;
            if (err != nullptr) *err = Fmt(L"应用 BPF 过滤器失败：{}", detail);
            return false;
        }
        lib.freeCode(&prog);
    }
    // 清空上一会话的统计口径（诚实：统计自本会话开始）。
    {
        std::lock_guard<std::mutex> lock(impl_->mu_);
        impl_->ring_.clear();
        impl_->dropped_ = 0;
        impl_->captured_ = 0;
        impl_->lastErr_.clear();
    }
    impl_->stop_.store(false, std::memory_order_relaxed);
    impl_->running_.store(true, std::memory_order_relaxed);
    impl_->thread_ = std::thread([this] { impl_->ConsumeLoop(); });
    return true;
}

void NpcapSource::StopCapture() { impl_->Stop(); }

bool NpcapSource::Running() const { return impl_->running_.load(std::memory_order_relaxed); }

uint64_t NpcapSource::DroppedPackets() const {
    std::lock_guard<std::mutex> lock(impl_->mu_);
    return impl_->dropped_;
}

uint64_t NpcapSource::CapturedTotal() const {
    std::lock_guard<std::mutex> lock(impl_->mu_);
    return impl_->captured_;
}

void NpcapSource::DrainPackets(std::vector<PktRecord>* out) {
    if (out == nullptr) return;
    out->clear();
    std::lock_guard<std::mutex> lock(impl_->mu_);
    out->assign(std::make_move_iterator(impl_->ring_.begin()),
                std::make_move_iterator(impl_->ring_.end()));
    impl_->ring_.clear();
}

bool NpcapSource::SendPacket(const std::wstring& deviceName, const uint8_t* bytes,
                             size_t len, std::wstring* err) {
    const WpcapLib& lib = WpcapLib::Inst();
    if (!lib.ok()) {
        if (err != nullptr) *err = lib.error();
        return false;
    }
    if (bytes == nullptr || len == 0 || len > 65535) {
        if (err != nullptr) *err = L"帧长度非法（1..65535 字节）";
        return false;
    }
    char errbuf[kPcapErrbufSize]{};
    const std::string nameUtf8 = WideToUtf8(deviceName);
    void* pcap =
        lib.openLive(nameUtf8.c_str(), 65535, /*promisc=*/0, /*timeoutMs=*/100, errbuf);
    if (pcap == nullptr) {
        if (err != nullptr) *err = Fmt(L"打开设备失败：{}", PcapErrText(errbuf));
        return false;
    }
    const int rc = lib.sendPacket(pcap, bytes, static_cast<int>(len));
    std::wstring detail = PcapErrText(lib.geterr(pcap));
    lib.close(pcap);
    if (rc != 0) {
        if (err != nullptr) *err = Fmt(L"发送失败：{}", detail);
        return false;
    }
    return true;
}

}  // namespace stm
