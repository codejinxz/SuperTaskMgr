// LhmSource.cpp — 可选的 LibreHardwareMonitor（LHM）HTTP 桥接（见 LhmSource.h）。
// 我们是用户运行的 LHM web 服务器的客户端；应用本身保持
// “不监听端口”的承诺。WinHTTP 从
// C:\Windows\System32\winhttp.dll（绝对路径，驱动/OS 所有）动态绑定，
// 链接行无需新增任何东西；仅回环主机与 1s 超时在此强制执行。
// 不建线程，不用定时器：PollLhm 被调用时同步执行。
//
// data.json（LHM 远程 web 服务器）是一棵节点树：
//   {"id":0,"Text":"LibreHardwareMonitor","Value":null,"Children":[
//      {"Text":"Intel Core i7 ...","Children":[
//         {"Text":"Temperatures","Sensor":[{"Text":"Core #1","Value":"45.000"}]}]}]}
// 每个 "Sensor" 数组条目变成一条 SensorReading；label = 节点路径
//（Text 链）+ "［LHM］"；value = Value 字符串的数字前缀；unit =
// 存在时的非数字余部（"38.000 °C" -> °C）。缺失/null 值映射为
// State::NoHardware——绝不伪造 0。
#include "collect/LhmSource.h"
#include "core/Log.h"
#include "core/Str.h"
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <charconv>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

namespace stm {

namespace {

constexpr wchar_t kTag[] = L"［LHM］";
constexpr size_t kMaxReadings = 512;               // 防失控树的上限
constexpr size_t kMaxBodyBytes = 8u * 1024 * 1024; // 响应 8 MB 上限
constexpr int kMaxDepth = 64;                      // 递归上限

// ---------------------------------------------------------------------------
// 线程安全的全局选项（普通进程级全局状态，按契约）
// ---------------------------------------------------------------------------
std::mutex g_mu;
LhmOptions g_opts;

// ---------------------------------------------------------------------------
// 最小递归 JSON 解析器——只实现 data.json 需要的部分：对象、数组、
// 字符串（\u 转义含代理对）、数字、true/false/null。
// ---------------------------------------------------------------------------
struct JVal {
    enum class T { Null, Bool, Num, Str, Arr, Obj };
    T t = T::Null;
    bool b = false;
    double num = 0.0;
    std::wstring str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::wstring, JVal>> mem;

    const JVal* Find(const wchar_t* key) const {
        for (const auto& kv : mem) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
};

struct JParser {
    const char* p = nullptr;
    const char* end = nullptr;

    bool Parse(JVal* root) {
        SkipWs();
        if (p >= end) return false;
        if (!ParseValue(root, 0)) return false;
        SkipWs();
        return p >= end;  // 拒绝尾部垃圾字符
    }

    void SkipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }

    bool ParseValue(JVal* out, int depth) {
        if (depth > kMaxDepth || p >= end) return false;
        const char c = *p;
        if (c == '{') {
            ++p;
            out->t = JVal::T::Obj;
            SkipWs();
            if (p < end && *p == '}') {
                ++p;
                return true;
            }
            for (;;) {
                SkipWs();
                if (p >= end || *p != '"') return false;
                std::wstring key;
                if (!ParseString(&key)) return false;
                SkipWs();
                if (p >= end || *p != ':') return false;
                ++p;
                JVal child;
                if (!ParseValue(&child, depth + 1)) return false;
                out->mem.emplace_back(std::move(key), std::move(child));
                SkipWs();
                if (p < end && *p == ',') {
                    ++p;
                    continue;
                }
                if (p < end && *p == '}') {
                    ++p;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            ++p;
            out->t = JVal::T::Arr;
            SkipWs();
            if (p < end && *p == ']') {
                ++p;
                return true;
            }
            for (;;) {
                JVal child;
                if (!ParseValue(&child, depth + 1)) return false;
                out->arr.push_back(std::move(child));
                SkipWs();
                if (p < end && *p == ',') {
                    ++p;
                    continue;
                }
                if (p < end && *p == ']') {
                    ++p;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            out->t = JVal::T::Str;
            return ParseString(&out->str);
        }
        if (c == 't' || c == 'f' || c == 'n') {
            static constexpr char kLiterals[3][6] = {"true", "false", "null"};
            const int which = c == 't' ? 0 : (c == 'f' ? 1 : 2);
            const size_t len = which == 0 ? 4 : (which == 1 ? 5 : 4);
            if (static_cast<size_t>(end - p) < len) return false;
            for (size_t i = 0; i < len; ++i) {
                if (p[i] != kLiterals[which][i]) return false;
            }
            p += len;
            out->t = which == 2 ? JVal::T::Null : JVal::T::Bool;
            out->b = which == 0;
            return true;
        }
        // 数字：[-]digits[.digits][e[+-]digits]，经 from_chars（不受区域设置影响）
        const char* s = p;
        if (*p == '-' || *p == '+') ++p;
        const char* d0 = p;
        while (p < end && *p >= '0' && *p <= '9') ++p;
        if (p < end && *p == '.') {
            ++p;
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            const char* save = p;
            ++p;
            if (p < end && (*p == '-' || *p == '+')) ++p;
            const char* ds = p;
            while (p < end && *p >= '0' && *p <= '9') ++p;
            if (p == ds) p = save;  // 悬空的指数：不属于数字的一部分
        }
        if (p == d0) return false;  // 完全没有数字
        const std::from_chars_result res = std::from_chars(s, p, out->num);
        out->t = JVal::T::Num;
        return res.ec == std::errc();
    }

    // 解析一个 JSON 字符串；*p 位于开引号上。字节以 UTF-8 累积
    //（转义解码，含代理对），最后一次性转换。
    bool ParseString(std::wstring* out) {
        ++p;  // 开引号
        std::string u8;
        while (p < end && *p != '"') {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (c == '\\') {
                ++p;
                if (p >= end) return false;
                const char e = *p;
                if (e == 'u') {
                    ++p;
                    uint32_t cp = 0;
                    if (!Hex4(&p, end, &cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {  // 高代理：需要 \uDC00-\uDFFF
                        if (end - p < 6 || p[0] != '\\' || p[1] != 'u') return false;
                        p += 2;
                        uint32_t lo = 0;
                        if (!Hex4(&p, end, &lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return false;
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return false;  // 孤立低代理
                    }
                    AppendUtf8(&u8, cp);
                    continue;
                }
                switch (e) {
                    case '"': u8 += '"'; break;
                    case '\\': u8 += '\\'; break;
                    case '/': u8 += '/'; break;
                    case 'b': u8 += '\b'; break;
                    case 'f': u8 += '\f'; break;
                    case 'n': u8 += '\n'; break;
                    case 'r': u8 += '\r'; break;
                    case 't': u8 += '\t'; break;
                    default: return false;
                }
                ++p;
                continue;
            }
            if (c < 0x20) return false;  // 裸控制字符是非法 JSON
            u8 += static_cast<char>(c);
            ++p;
        }
        if (p >= end) return false;
        ++p;  // 闭引号
        *out = Utf8ToWide(u8);
        return true;
    }

    static bool Hex4(const char** pp, const char* endp, uint32_t* out) {
        if (endp - *pp < 4) return false;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = (*pp)[i];
            v <<= 4;
            if (h >= '0' && h <= '9') {
                v |= static_cast<uint32_t>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
                v |= static_cast<uint32_t>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
                v |= static_cast<uint32_t>(h - 'A' + 10);
            } else {
                return false;
            }
        }
        *pp += 4;
        *out = v;
        return true;
    }

    static void AppendUtf8(std::string* s, uint32_t cp) {
        if (cp < 0x80) {
            *s += static_cast<char>(cp);
        } else if (cp < 0x800) {
            *s += static_cast<char>(0xC0 | (cp >> 6));
            *s += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            *s += static_cast<char>(0xE0 | (cp >> 12));
            *s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            *s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            *s += static_cast<char>(0xF0 | (cp >> 18));
            *s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            *s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            *s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
};

// "45.000" -> 45.0 + 空单位；"38.000 °C" -> 38.0 + "°C"；垃圾输入 -> false。
bool ParseNumUnit(const std::wstring& text, double* value, std::wstring* unit) {
    size_t i = 0;
    while (i < text.size() && (text[i] == L' ' || text[i] == L'\t')) ++i;
    std::string num;
    while (i < text.size() &&
           ((text[i] >= L'0' && text[i] <= L'9') || text[i] == L'-' || text[i] == L'+' ||
            text[i] == L'.' || text[i] == L'e' || text[i] == L'E')) {
        if (text[i] == L'+') {  // from_chars 拒绝前导 '+'：跳过它
            ++i;
            continue;
        }
        num += static_cast<char>(text[i]);
        ++i;
    }
    if (num.empty()) return false;
    const std::from_chars_result res = std::from_chars(num.data(), num.data() + num.size(), *value);
    if (res.ec != std::errc()) return false;
    while (i < text.size() && (text[i] == L' ' || text[i] == L'\t')) ++i;
    *unit = text.substr(i);
    return true;
}

struct WalkCtx {
    std::vector<SensorReading>* out = nullptr;
    bool truncated = false;
};

void WalkNode(const JVal& node, const std::wstring& prefix, WalkCtx* ctx) {
    if (ctx->truncated || node.t != JVal::T::Obj) return;

    // 本节点自身的 Text 追加到路径；无名节点不污染路径。
    const JVal* nodeTxt = node.Find(L"Text");
    const std::wstring self =
        (nodeTxt != nullptr && nodeTxt->t == JVal::T::Str) ? nodeTxt->str : std::wstring();
    std::wstring selfPath;
    if (self.empty()) {
        selfPath = prefix;
    } else if (prefix.empty()) {
        selfPath = self;
    } else {
        selfPath = prefix + L" / " + self;
    }

    // 本节点拥有的 "Sensor" 条目 -> 读数以其完整路径作标签
    //（硬件/子系统名是标签的一部分，HWiNFO 风格）。
    if (const JVal* sensors = node.Find(L"Sensor");
        sensors != nullptr && sensors->t == JVal::T::Arr) {
        for (const JVal& s : sensors->arr) {
            if (ctx->out->size() >= kMaxReadings) {
                ctx->truncated = true;
                return;
            }
            if (s.t != JVal::T::Obj) continue;
            const JVal* txt = s.Find(L"Text");
            if (txt == nullptr || txt->t != JVal::T::Str || txt->str.empty()) continue;
            SensorReading r;
            r.label = selfPath.empty() ? txt->str : selfPath + L" / " + txt->str;
            r.label += kTag;  // 来源标记：该读数来自外部 LHM 源
            const JVal* val = s.Find(L"Value");
            if (val != nullptr && val->t == JVal::T::Str &&
                ParseNumUnit(val->str, &r.value, &r.unit)) {
                r.state = SensorReading::State::Ok;
            } else if (val != nullptr && val->t == JVal::T::Num) {
                r.value = val->num;
                r.state = SensorReading::State::Ok;
            } else {
                r.state = SensorReading::State::NoHardware;  // 节点存在但当前无值
            }
            ctx->out->push_back(std::move(r));
        }
    }

    // 递归进入 Children，用本节点自身的 Text 扩展路径。
    if (const JVal* kids = node.Find(L"Children"); kids != nullptr && kids->t == JVal::T::Arr) {
        for (const JVal& k : kids->arr) WalkNode(k, selfPath, ctx);
    }
}

// ---------------------------------------------------------------------------
// 动态 WinHTTP 绑定（winhttp.dll 属 OS 所有；不改链接行）
// ---------------------------------------------------------------------------
struct WinHttpApi {
    using OpenFn = HINTERNET(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
    using ConnectFn = HINTERNET(WINAPI*)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
    using OpenRequestFn = HINTERNET(WINAPI*)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR,
                                             LPCWSTR*, DWORD);
    using SetTimeoutsFn = BOOL(WINAPI*)(HINTERNET, int, int, int, int);
    using SendRequestFn = BOOL(WINAPI*)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
    using ReceiveResponseFn = BOOL(WINAPI*)(HINTERNET, LPVOID);
    using QueryDataAvailableFn = BOOL(WINAPI*)(HINTERNET, LPDWORD);
    using ReadDataFn = BOOL(WINAPI*)(HINTERNET, LPVOID, DWORD, LPDWORD);
    using QueryHeadersFn = BOOL(WINAPI*)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
    using CloseHandleFn = BOOL(WINAPI*)(HINTERNET);

    OpenFn open = nullptr;
    ConnectFn connect = nullptr;
    OpenRequestFn openRequest = nullptr;
    SetTimeoutsFn setTimeouts = nullptr;
    SendRequestFn sendRequest = nullptr;
    ReceiveResponseFn receiveResponse = nullptr;
    QueryDataAvailableFn queryDataAvailable = nullptr;
    ReadDataFn readData = nullptr;
    QueryHeadersFn queryHeaders = nullptr;
    CloseHandleFn closeHandle = nullptr;

    bool ok() const {
        return open != nullptr && connect != nullptr && openRequest != nullptr &&
               setTimeouts != nullptr && sendRequest != nullptr && receiveResponse != nullptr &&
               queryDataAvailable != nullptr && readData != nullptr && queryHeaders != nullptr &&
               closeHandle != nullptr;
    }
};

const WinHttpApi* WinHttp() {
    static const WinHttpApi api = []() {
        WinHttpApi a;
        // 与 nvml.dll 绑定一样使用绝对路径：绝不按搜索顺序加载。
        const HMODULE mod = ::LoadLibraryW(L"C:\\Windows\\System32\\winhttp.dll");
        if (mod == nullptr) return a;
        auto proc = [&](const char* name) { return ::GetProcAddress(mod, name); };
        a.open = reinterpret_cast<WinHttpApi::OpenFn>(proc("WinHttpOpen"));
        a.connect = reinterpret_cast<WinHttpApi::ConnectFn>(proc("WinHttpConnect"));
        a.openRequest = reinterpret_cast<WinHttpApi::OpenRequestFn>(proc("WinHttpOpenRequest"));
        a.setTimeouts = reinterpret_cast<WinHttpApi::SetTimeoutsFn>(proc("WinHttpSetTimeouts"));
        a.sendRequest = reinterpret_cast<WinHttpApi::SendRequestFn>(proc("WinHttpSendRequest"));
        a.receiveResponse =
            reinterpret_cast<WinHttpApi::ReceiveResponseFn>(proc("WinHttpReceiveResponse"));
        a.queryDataAvailable =
            reinterpret_cast<WinHttpApi::QueryDataAvailableFn>(proc("WinHttpQueryDataAvailable"));
        a.readData = reinterpret_cast<WinHttpApi::ReadDataFn>(proc("WinHttpReadData"));
        a.queryHeaders = reinterpret_cast<WinHttpApi::QueryHeadersFn>(proc("WinHttpQueryHeaders"));
        a.closeHandle = reinterpret_cast<WinHttpApi::CloseHandleFn>(proc("WinHttpCloseHandle"));
        return a;
    }();
    return &api;
}

struct HGuard {  // WinHTTP 句柄链的 RAII
    HINTERNET h = nullptr;
    const WinHttpApi* api = nullptr;
    ~HGuard() {
        if (h != nullptr && api != nullptr && api->closeHandle != nullptr) api->closeHandle(h);
    }
};

bool IsLoopbackHost(const std::wstring& host) {
    std::wstring lower;
    lower.reserve(host.size());
    for (const wchar_t c : host) {
        lower += (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
    }
    return lower == L"127.0.0.1" || lower == L"localhost" || lower == L"::1";
}

}  // namespace

void SetLhmOptions(const LhmOptions& options) {
    const std::lock_guard<std::mutex> lock(g_mu);
    g_opts = options;
}

LhmOptions GetLhmOptions() {
    const std::lock_guard<std::mutex> lock(g_mu);
    return g_opts;
}

bool ParseLhmJson(const std::string& utf8, std::vector<SensorReading>* out) {
    if (out == nullptr) return false;
    out->clear();
    if (utf8.empty() || utf8.size() > kMaxBodyBytes) return false;
    JParser jp;
    jp.p = utf8.data();
    jp.end = utf8.data() + utf8.size();
    JVal root;
    if (!jp.Parse(&root) || root.t != JVal::T::Obj) return false;
    WalkCtx ctx;
    ctx.out = out;
    WalkNode(root, L"", &ctx);
    return !out->empty();
}

bool PollLhm(std::vector<SensorReading>* out, std::wstring* err) {
    if (out != nullptr) out->clear();
    auto fail = [&](const std::wstring& msg) {
        if (err != nullptr) *err = msg;
        STM_LOG_WARN("sensors", msg);
        if (out != nullptr) out->clear();
        return false;
    };

    const LhmOptions opts = GetLhmOptions();
    if (!opts.enabled) {
        return fail(L"LibreHardwareMonitor 数据源未启用（默认关闭）");
    }
    if (!IsLoopbackHost(opts.host)) {  // 红线：本工具只与本机通信
        return fail(L"LHM 数据源仅允许本机地址（127.0.0.1 / localhost / ::1）");
    }
    const WinHttpApi* api = WinHttp();
    if (api == nullptr || !api->ok()) {
        return fail(L"winhttp.dll 不可用，无法轮询 LibreHardwareMonitor");
    }

    HGuard session{api->open(L"SuperTaskMgr-LHM/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0),
                   api};
    if (session.h == nullptr) {
        return fail(L"WinHttpOpen 失败，无法轮询 LibreHardwareMonitor");
    }
    api->setTimeouts(session.h, 1000, 1000, 1000, 1000);  // 解析/连接/发送/接收

    HGuard conn{api->connect(session.h, opts.host.c_str(), opts.port, 0), api};
    if (conn.h == nullptr) {
        const DWORD gle = ::GetLastError();
        return fail(Fmt(L"未检测到 LibreHardwareMonitor 数据源（http://{}:{}/data.json，"
                        L"WinHttpConnect gle={}）",
                        opts.host, opts.port, static_cast<unsigned long>(gle)));
    }
    HGuard req{api->openRequest(conn.h, L"GET", L"/data.json", nullptr, WINHTTP_NO_REFERER,
                                WINHTTP_DEFAULT_ACCEPT_TYPES, 0),
               api};
    if (req.h == nullptr) {
        return fail(L"WinHttpOpenRequest 失败（/data.json）");
    }
    if (!api->sendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
                          0)) {
        const DWORD gle = ::GetLastError();
        return fail(Fmt(L"未检测到 LibreHardwareMonitor 数据源（WinHttpSendRequest gle={}）",
                        static_cast<unsigned long>(gle)));
    }
    if (!api->receiveResponse(req.h, nullptr)) {
        const DWORD gle = ::GetLastError();
        return fail(Fmt(L"未检测到 LibreHardwareMonitor 数据源（WinHttpReceiveResponse gle={}）",
                        static_cast<unsigned long>(gle)));
    }
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!api->queryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                           WINHTTP_NO_HEADER_INDEX) ||
        status != 200) {
        return fail(Fmt(L"LHM 数据源响应异常（HTTP {}）", status));
    }

    std::string body;
    for (;;) {
        DWORD avail = 0;
        if (!api->queryDataAvailable(req.h, &avail)) {
            return fail(L"读取 LHM data.json 失败（QueryDataAvailable）");
        }
        if (avail == 0) break;
        if (body.size() + avail > kMaxBodyBytes) {
            return fail(L"LHM data.json 超过 8MB 上限，已中止");
        }
        const size_t before = body.size();
        body.resize(before + avail);
        DWORD got = 0;
        if (!api->readData(req.h, body.data() + before, avail, &got) || got == 0) {
            return fail(L"读取 LHM data.json 失败（ReadData）");
        }
        body.resize(before + got);
    }

    if (!ParseLhmJson(body, out)) {
        return fail(L"LHM data.json 解析失败（空响应或未知格式）");
    }
    return true;
}

}  // namespace stm
