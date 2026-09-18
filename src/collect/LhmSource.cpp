// LhmSource.cpp — optional LibreHardwareMonitor (LHM) HTTP bridge (LhmSource.h).
// We are a CLIENT of the user-run LHM web server; the app itself keeps the
// "no listening port" guarantee. WinHTTP is bound dynamically from
// C:\Windows\System32\winhttp.dll (absolute path, driver/OS-owned) so the
// link line needs nothing new; loopback-only hosts and 1 s timeouts are
// enforced here. No thread, no timer: PollLhm runs synchronously when called.
//
// data.json (LHM remote web server) is a tree of nodes:
//   {"id":0,"Text":"LibreHardwareMonitor","Value":null,"Children":[
//      {"Text":"Intel Core i7 ...","Children":[
//         {"Text":"Temperatures","Sensor":[{"Text":"Core #1","Value":"45.000"}]}]}]}
// Each "Sensor" array entry becomes one SensorReading; label = node path
// (Text chain) + "［LHM］"; value = numeric prefix of the Value string; unit =
// the non-numeric remainder when present ("38.000 °C" -> °C). Missing/null
// values map to State::NoHardware — never a fabricated 0.
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
constexpr size_t kMaxReadings = 512;               // cap a runaway tree
constexpr size_t kMaxBodyBytes = 8u * 1024 * 1024; // 8 MB response cap
constexpr int kMaxDepth = 64;                      // recursion cap

// ---------------------------------------------------------------------------
// thread-safe global options (plain process-global state, per contract)
// ---------------------------------------------------------------------------
std::mutex g_mu;
LhmOptions g_opts;

// ---------------------------------------------------------------------------
// minimal recursive JSON parser — only what data.json needs: objects, arrays,
// strings (\u escapes incl. surrogate pairs), numbers, true/false/null.
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
        return p >= end;  // reject trailing garbage
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
        // number: [-]digits[.digits][e[+-]digits] via from_chars (locale-free)
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
            if (p == ds) p = save;  // dangling exponent: not part of the number
        }
        if (p == d0) return false;  // no digits at all
        const std::from_chars_result res = std::from_chars(s, p, out->num);
        out->t = JVal::T::Num;
        return res.ec == std::errc();
    }

    // Parses one JSON string; *p is on the opening quote. Bytes accumulate as
    // UTF-8 (escapes decoded, incl. surrogate pairs), converted once at the end.
    bool ParseString(std::wstring* out) {
        ++p;  // opening quote
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
                    if (cp >= 0xD800 && cp <= 0xDBFF) {  // high surrogate: need \uDC00-\uDFFF
                        if (end - p < 6 || p[0] != '\\' || p[1] != 'u') return false;
                        p += 2;
                        uint32_t lo = 0;
                        if (!Hex4(&p, end, &lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return false;
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return false;  // lone low surrogate
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
            if (c < 0x20) return false;  // bare control char is invalid JSON
            u8 += static_cast<char>(c);
            ++p;
        }
        if (p >= end) return false;
        ++p;  // closing quote
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

// "45.000" -> 45.0 + empty unit; "38.000 °C" -> 38.0 + "°C"; junk -> false.
bool ParseNumUnit(const std::wstring& text, double* value, std::wstring* unit) {
    size_t i = 0;
    while (i < text.size() && (text[i] == L' ' || text[i] == L'\t')) ++i;
    std::string num;
    while (i < text.size() &&
           ((text[i] >= L'0' && text[i] <= L'9') || text[i] == L'-' || text[i] == L'+' ||
            text[i] == L'.' || text[i] == L'e' || text[i] == L'E')) {
        if (text[i] == L'+') {  // from_chars rejects a leading '+': skip it
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

    // This node's own Text extends the path; unnamed nodes never pollute it.
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

    // "Sensor" entries owned by THIS node -> readings labeled by its full path
    // (the hardware/subsystem name is part of the label, HWiNFO-style).
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
            r.label += kTag;  // provenance: this reading came from the external LHM source
            const JVal* val = s.Find(L"Value");
            if (val != nullptr && val->t == JVal::T::Str &&
                ParseNumUnit(val->str, &r.value, &r.unit)) {
                r.state = SensorReading::State::Ok;
            } else if (val != nullptr && val->t == JVal::T::Num) {
                r.value = val->num;
                r.state = SensorReading::State::Ok;
            } else {
                r.state = SensorReading::State::NoHardware;  // node exists, no value now
            }
            ctx->out->push_back(std::move(r));
        }
    }

    // Recurse into Children, extending the path with this node's own Text.
    if (const JVal* kids = node.Find(L"Children"); kids != nullptr && kids->t == JVal::T::Arr) {
        for (const JVal& k : kids->arr) WalkNode(k, selfPath, ctx);
    }
}

// ---------------------------------------------------------------------------
// dynamic WinHTTP binding (winhttp.dll is OS-owned; no link-line change)
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
        // Absolute path like the nvml.dll binding: never a search-order load.
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

struct HGuard {  // RAII for the WinHTTP handle chain
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
    if (!IsLoopbackHost(opts.host)) {  // red line: this tool only talks to localhost
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
    api->setTimeouts(session.h, 1000, 1000, 1000, 1000);  // resolve/connect/send/receive

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
