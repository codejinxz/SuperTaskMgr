// 来自经典 Windows 事件日志的崩溃/挂起历史（契约 ops/CrashLog.h）。
//
// 只用有文档的 API（wevtapi）：EvtQuery(通道路径, XPath) 对 "Application" 与
// "System" 以反向（最新在前）查询 EventID 1000（应用错误）/
// 1001（WER 报告）/ 1002（应用挂起），再 EvtRender(EvtRenderXml) 加一段
// 自包含的 XML 字段扫描取 TimeCreated / Provider / Level / EventData。
// 两个通道都无需管理员可读。不需要管理员、无 ETW、无第三方依赖。
// 所有 EVT_HANDLE 均由 RAII 持有。
//
// EventData 有两种真实形态，两种都解析（P1 修复：形态 B 以前会
// degrade every .NET Runtime row to "未知/-"):
//   A) 具名字段—— <Data Name='AppName'>x.exe</Data>（Application Error/Hang、
//      WER）：AppName/AppPath/Application -> app；FaultingModule*/Module -> module；
//   B) 无名文本块—— <Data>Category: ...&#xA;Exception: ...</Data>（.NET Runtime）：
//      第一条非空行进入 summary；app/module 由尽力而为的
//      文本启发式给出（首个 *.exe 记号；"faulting
//      module"/"模块"). Nothing extractable stays empty.
// 两条路都拿不到 app 的条目回退到 Execution ProcessID。
// ("PID 87360") — never a "未知" placeholder. Attribute values in both quote styles
//（单/双引号）均可接受；数字实体（&#xA;）会解码；CDATA 载荷
//（如今渲染器不会产生）原样透传。
#include "ops/CrashLog.h"
#include "core/Err.h"
#include "core/Log.h"
#include "core/Str.h"
#include <windows.h>
#include <winevt.h>
#include <algorithm>
#include <cwchar>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "wevtapi")

namespace stm::ops {
namespace {

constexpr uint64_t kFileTimeToUnixEpoch = 116444736000000000ull;  // 1601->1970，100ns
constexpr uint64_t kFileTime1Sec = 10'000'000ull;

struct EvtHandleCloser {
    void operator()(void* h) const noexcept { if (h) EvtClose(h); }
};
using UniqueEvt = std::unique_ptr<void, EvtHandleCloser>;

std::wstring ReplaceAll(std::wstring s, const std::wstring& from, const std::wstring& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::wstring::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

bool IsXmlSpace(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}

std::wstring Trim(const std::wstring& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && IsXmlSpace(s[b])) ++b;
    while (e > b && IsXmlSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// 尽力而为的 XML 实体解码：五个命名实体加数字字符引用
//（&#xA; / &#13;——渲染器以数字形式转义换行）。单趟
// 从左到右，因此 "&amp;lt;" 得到 "&lt;"（绝不二次解码）；裸 "&" 或
// 无法解析的引用保持原样。
std::wstring UnescapeXml(const std::wstring& s) {
    if (s.find(L'&') == std::wstring::npos) return s;
    static constexpr struct {
        const wchar_t* ent;
        wchar_t ch;
    } kNamed[] = {
        {L"&lt;", L'<'},    {L"&gt;", L'>'},   {L"&quot;", L'"'},
        {L"&apos;", L'\''}, {L"&amp;", L'&'},
    };
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != L'&') {
            out += s[i++];
            continue;
        }
        bool matched = false;
        for (const auto& e : kNamed) {
            const size_t n = wcslen(e.ent);
            if (s.compare(i, n, e.ent) == 0) {
                out += e.ch;
                i += n;
                matched = true;
                break;
            }
        }
        if (matched) continue;
        if (i + 1 < s.size() && s[i + 1] == L'#') {  // 数字引用
            size_t j = i + 2;
            bool hex = false;
            if (j < s.size() && (s[j] == L'x' || s[j] == L'X')) {
                hex = true;
                ++j;
            }
            wchar_t* endp = nullptr;
            const unsigned long code =
                wcstoul(s.c_str() + j, &endp, hex ? 16 : 10);
            if (endp > s.c_str() + j && endp < s.c_str() + s.size() && *endp == L';' &&
                code != 0 && code <= 0xFFFF) {
                out += static_cast<wchar_t>(code);
                i = static_cast<size_t>(endp - s.c_str()) + 1;
                continue;
            }
        }
        out += s[i++];  // 裸 '&'：原样保留
    }
    return out;
}

// 解码元素载荷：CDATA 块（如今 EvtRender 不会产生，
// 为健壮性而保留）原样透传；普通文本做实体解码。
std::wstring DecodeXmlText(const std::wstring& raw) {
    constexpr wchar_t kCdataOpen[] = L"<![CDATA[";
    constexpr size_t kCdataOpenLen = 9;
    if (raw.size() >= kCdataOpenLen + 3 && raw.compare(0, kCdataOpenLen, kCdataOpen) == 0 &&
        raw.compare(raw.size() - 3, 3, L"]]>") == 0) {
        return raw.substr(kCdataOpenLen, raw.size() - kCdataOpenLen - 3);
    }
    return UnescapeXml(raw);
}

// `p` 处的字符是否像我们要找的标签的开头（避免匹配到
// 只是共享前缀的更长元素名）。
bool TagBoundary(const std::wstring& xml, size_t nameEnd) {
    if (nameEnd >= xml.size()) return false;
    const wchar_t c = xml[nameEnd];
    return c == L'>' || c == L'/' || c == L' ';
}

// "<Tag attr='v'>" 标签文本内属性 `attr` 的值。两种引号风格
// 现实中都存在（EvtRender 与常见抓取工具不同），且属性名
// 必须是完整单词：前面是空白、后面紧跟 '='。
std::wstring AttrValueInTagText(const std::wstring& tagText, const wchar_t* attr) {
    const size_t alen = wcslen(attr);
    size_t p = tagText.find(attr);
    while (p != std::wstring::npos) {
        const size_t after = p + alen;
        if (p > 0 && IsXmlSpace(tagText[p - 1]) && after < tagText.size() &&
            tagText[after] == L'=') {
            const size_t q = after + 1;
            if (q < tagText.size()) {
                const wchar_t quote = tagText[q];
                if (quote == L'"' || quote == L'\'') {
                    const size_t vend = tagText.find(quote, q + 1);
                    if (vend != std::wstring::npos) {
                        return UnescapeXml(tagText.substr(q + 1, vend - q - 1));
                    }
                }
            }
        }
        p = tagText.find(attr, after);
    }
    return {};
}

// XML 中第一个 "<tag ...>" 元素的属性 `attr`（缺失时为空）。
std::wstring AttributeInFirstTag(const std::wstring& xml, const wchar_t* tag,
                                 const wchar_t* attr) {
    const std::wstring open = std::wstring(L"<") + tag;
    size_t p = xml.find(open);
    while (p != std::wstring::npos) {
        if (TagBoundary(xml, p + open.size())) {
            const size_t gt = xml.find(L'>', p);
            if (gt == std::wstring::npos) return {};
            return AttrValueInTagText(xml.substr(p, gt - p), attr);
        }
        p = xml.find(open, p + 1);
    }
    return {};
}

// 第一个 "<tag ...>value</tag>" 元素的文本（缺失/自闭合时为空）。
std::wstring ElementText(const std::wstring& xml, const wchar_t* tag) {
    const std::wstring open = std::wstring(L"<") + tag;
    size_t p = xml.find(open);
    while (p != std::wstring::npos) {
        if (TagBoundary(xml, p + open.size())) break;
        p = xml.find(open, p + 1);
    }
    if (p == std::wstring::npos) return {};
    const size_t gt = xml.find(L'>', p);
    if (gt == std::wstring::npos || xml[gt - 1] == L'/') return {};  // 畸形/自闭合
    const std::wstring close = std::wstring(L"</") + tag + L">";
    const size_t ce = xml.find(close, gt);
    if (ce == std::wstring::npos) return {};
    return DecodeXmlText(xml.substr(gt + 1, ce - gt - 1));
}

// ISO-8601 "2026-09-18T03:14:15[.1234567][Z]" -> Unix 秒（SystemTime 为 UTC）。
// swscanf 在秒字段后停止，因此可选小数部分与 Z 后缀
//（以及它们的缺失）都能容忍。
int64_t ParseEventTime(const std::wstring& s) {
    SYSTEMTIME st{};
    if (swscanf_s(s.c_str(), L"%hu-%hu-%huT%hu:%hu:%hu", &st.wYear, &st.wMonth, &st.wDay,
                  &st.wHour, &st.wMinute, &st.wSecond) != 6) {
        return 0;
    }
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    const uint64_t u = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return static_cast<int64_t>((u - kFileTimeToUnixEpoch) / kFileTime1Sec);
}

std::wstring ImageNameOf(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

uint32_t ToU32(const std::wstring& s) {
    const unsigned long v = wcstoul(s.c_str(), nullptr, 10);
    return static_cast<uint32_t>(v);
}

struct DataField {
    std::wstring name;   // 无名（自由文本块）载荷时为空
    std::wstring value;
};

// 解析每个 "<Data ...>value</Data>"（自闭合形式得到空值）。两种
// 真实形态都保留：具名字段与无名文本块（.NET Runtime 载荷是
// 一个带 &#xA; 换行的自由文本 Data 元素）。
void ParseEventData(const std::wstring& xml, std::vector<DataField>* out) {
    out->clear();
    size_t p = xml.find(L"<Data");
    while (p != std::wstring::npos) {
        if (!TagBoundary(xml, p + 5)) {
            p = xml.find(L"<Data", p + 1);
            continue;
        }
        const size_t gt = xml.find(L'>', p);
        if (gt == std::wstring::npos) break;
        const bool selfClosing = xml[gt - 1] == L'/';
        DataField f;
        f.name = AttrValueInTagText(xml.substr(p, gt - p), L"Name");
        if (!selfClosing) {
            const size_t ce = xml.find(L"</Data>", gt);
            if (ce != std::wstring::npos) {
                f.value = DecodeXmlText(xml.substr(gt + 1, ce - gt - 1));
                p = ce + 7;
            } else {
                p = gt;  // 畸形：停止扫描本元素，从其后继续
            }
        } else {
            p = gt;
        }
        if (!f.name.empty() || !f.value.empty()) out->push_back(std::move(f));
        p = xml.find(L"<Data", p);
    }
}

const std::wstring* FindNamed(const std::vector<DataField>& data, const wchar_t* name) {
    for (const auto& d : data) {
        if (d.name == name) return &d.value;
    }
    return nullptr;
}

//（自 `from` 起）尾部等于 `suffixes` 之一的第一个空白分隔记号
//（大小写不敏感）；剥离常见尾部标点。
std::wstring FindTokenEndingWith(const std::wstring& text, size_t from,
                                 const wchar_t* const* suffixes, size_t count) {
    size_t i = from;
    while (i < text.size()) {
        while (i < text.size() && IsXmlSpace(text[i])) ++i;
        size_t j = i;
        while (j < text.size() && !IsXmlSpace(text[j])) ++j;
        if (j > i) {
            std::wstring tok = text.substr(i, j - i);
            while (!tok.empty() &&
                   (tok.back() == L',' || tok.back() == L';' || tok.back() == L':' ||
                    tok.back() == L')' || tok.back() == L'"' || tok.back() == L'\'')) {
                tok.pop_back();
            }
            for (size_t k = 0; k < count; ++k) {
                const size_t n = wcslen(suffixes[k]);
                if (tok.size() > n &&
                    _wcsnicmp(tok.c_str() + tok.size() - n, suffixes[k], n) == 0) {
                    return tok;
                }
            }
        }
        i = j;
    }
    return {};
}

// 从 `from` 起大小写不敏感地搜索 `needle`；找到时返回 true 及位置。
bool IFindFrom(const std::wstring& text, const wchar_t* needle, size_t from, size_t* pos) {
    const size_t n = wcslen(needle);
    for (size_t i = from; i + n <= text.size(); ++i) {
        if (_wcsnicmp(text.c_str() + i, needle, n) == 0) {
            *pos = i;
            return true;
        }
    }
    return false;
}

// 文本块启发式（B 路）：任何位置的第一个 *.exe 记号即 app；第一个
// *.dll/*.sys token after a "faulting module"/"模块" label is the module. Works with
// the label glued to the token ("模块：ntdll.dll") and with "name:" style suffixes.
std::wstring AppFromBlob(const std::wstring& blob) {
    static const wchar_t* const kExe[] = {L".exe"};
    const std::wstring tok = FindTokenEndingWith(blob, 0, kExe, 1);
    return tok.empty() ? std::wstring() : ImageNameOf(tok);
}

std::wstring ModuleFromBlob(const std::wstring& blob) {
    static const wchar_t* const kModule[] = {L".dll", L".sys"};
    static const wchar_t* const kLabels[] = {L"faulting module", L"模块"};
    for (const wchar_t* label : kLabels) {
        const size_t labelLen = wcslen(label);
        size_t pos = 0;
        while (IFindFrom(blob, label, pos, &pos)) {
            const std::wstring tok = FindTokenEndingWith(blob, pos + labelLen, kModule, 2);
            if (!tok.empty()) return ImageNameOf(tok);
            ++pos;
        }
    }
    return {};
}

std::wstring FirstNonEmptyLine(const std::wstring& text) {
    size_t p = 0;
    while (p < text.size()) {
        const size_t e = text.find(L'\n', p);
        const std::wstring line =
            Trim(text.substr(p, e == std::wstring::npos ? text.size() - p : e - p));
        if (!line.empty()) return line;
        if (e == std::wstring::npos) break;
        p = e + 1;
    }
    return {};
}

std::wstring TruncateForSummary(const std::wstring& s) {
    constexpr size_t kMax = 80;
    if (s.size() <= kMax) return s;
    return s.substr(0, kMax) + L"…";
}

// 按提供程序给出中文标题（用户阅读该行的方式）；未知提供程序
// 原样展示，而不是折叠成通用标签。
std::wstring ProviderHeadline(const std::wstring& provider, uint32_t eventId) {
    if (_wcsicmp(provider.c_str(), L"Application Error") == 0) return L"应用崩溃";
    if (_wcsicmp(provider.c_str(), L"Application Hang") == 0) return L"应用挂起";
    if (_wcsicmp(provider.c_str(), L"Windows Error Reporting") == 0) return L"WER 报告";
    if (_wcsicmp(provider.c_str(), L".NET Runtime") == 0) return L".NET 运行时错误";
    if (!provider.empty()) return provider;
    return eventId == 1000   ? L"应用崩溃"
           : eventId == 1001 ? L"WER 报告"
           : eventId == 1002 ? L"应用挂起"
                             : L"异常事件";  // provider-less fallback
}

std::wstring BuildSummary(const std::wstring& provider, uint32_t eventId,
                          const std::wstring& app, const std::wstring& module,
                          const std::wstring& blobLine) {
    std::wstring s = ProviderHeadline(provider, eventId);
    if (!app.empty()) s += L"：" + app;
    if (!module.empty()) s += L"（模块 " + module + L"）";
    if (!blobLine.empty()) s += L"：" + TruncateForSummary(blobLine);
    return s;
}

CrashEvent ParseEventXmlImpl(const std::wstring& xml) {
    CrashEvent ev;
    ev.unixTime = ParseEventTime(AttributeInFirstTag(xml, L"TimeCreated", L"SystemTime"));
    ev.provider = AttributeInFirstTag(xml, L"Provider", L"Name");
    ev.eventId = ToU32(ElementText(xml, L"EventID"));
    ev.level = static_cast<uint16_t>(ToU32(ElementText(xml, L"Level")));

    std::vector<DataField> data;
    ParseEventData(xml, &data);

    // A 路：具名载荷字段（WER / Application Error / Application Hang）。
    for (const wchar_t* n : {L"AppName", L"AppPath", L"Application"}) {
        if (!ev.app.empty()) break;
        if (const std::wstring* v = FindNamed(data, n)) ev.app = ImageNameOf(*v);
    }
    for (const wchar_t* n :
         {L"FaultingModule", L"FaultingModulePath", L"ModuleName", L"ModulePath", L"Module"}) {
        if (!ev.module.empty()) break;
        if (const std::wstring* v = FindNamed(data, n)) ev.module = ImageNameOf(*v);
    }

    // B 路：无名自由文本载荷（.NET Runtime 形态）——尽力而为。
    std::wstring blob;
    for (const DataField& d : data) {
        if (d.name.empty() && !d.value.empty()) {
            blob += d.value;
            blob += L'\n';
        }
    }
    const std::wstring blobLine = blob.empty() ? std::wstring() : FirstNonEmptyLine(blob);
    if (ev.app.empty()) ev.app = AppFromBlob(blob);
    if (ev.module.empty()) ev.module = ModuleFromBlob(blob);

    // WER 1001：P1 具名参数就是崩溃的应用（真机样本：
    // P1=powershell.exe）——先于裸 PID 兜底使用（V15-P2-1）。
    if (ev.app.empty() && ev.eventId == 1001) {
        if (const std::wstring* v = FindNamed(data, L"P1")) ev.app = ImageNameOf(*v);
    }

    // Last resort: the bare process id beats any "未知" placeholder (never lie).
    if (ev.app.empty()) {
        const std::wstring pid = AttributeInFirstTag(xml, L"Execution", L"ProcessID");
        if (!pid.empty()) ev.app = L"PID " + pid;
    }

    ev.summary = BuildSummary(ev.provider, ev.eventId, ev.app, ev.module, blobLine);
    return ev;
}

// 单通道，最新在前（EvtQueryReverseDirection），以 maxCount 为界。
// 空结果 + 空 chanErr = "没有此类事件"（诚实）；打开/迭代失败
// 以中文写入 chanErr。
bool QueryOneChannel(const wchar_t* channel, uint32_t maxCount,
                     std::vector<CrashEvent>* out, std::wstring* chanErr) {
    out->clear();
    chanErr->clear();
    const wchar_t* xpath =
        L"*[System[(EventID=1000 or EventID=1001 or EventID=1002)]]";
    UniqueEvt query(EvtQuery(nullptr, channel, xpath,
                             EvtQueryChannelPath | EvtQueryReverseDirection));
    if (!query) {
        *chanErr = Fmt(L"事件日志通道 {} 打开失败（系统限制或被策略禁用）",
                       std::wstring_view(channel)) + L"：" +
                   ErrContext(L"EvtQuery", LastHr());
        STM_LOG_WARN("crashlog", L"{}", *chanErr);
        return false;
    }
    constexpr DWORD kBatch = 16;
    std::vector<EVT_HANDLE> batch(kBatch);
    // 对整个 EvtNext 批次做 RAII（评审 V15-P1）：每条退出路径——maxCount
    // 截断导致循环中途离开、通道错误返回、以及取空跳出——
    // 都必须关闭 EvtNext 已交付但我们尚未接管的句柄。
    // 否则触顶 maxCount 的查询每次刷新最多泄漏 kBatch-1 个句柄。
    struct EvtBatchGuard {
        std::vector<EVT_HANDLE>* batch;
        ~EvtBatchGuard() {
            for (EVT_HANDLE& h : *batch) {
                if (h) EvtClose(h);
            }
        }
    } batchGuard{&batch};
    DWORD returned = 0;
    while (out->size() < maxCount) {
        if (!EvtNext(query.get(), kBatch, batch.data(), 2000, 0, &returned)) {
            const uint32_t hr = LastHr();
            if (hr == static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS))) break;
            *chanErr = Fmt(L"事件日志通道 {} 读取失败", std::wstring_view(channel)) + L"：" +
                       ErrContext(L"EvtNext", hr);
            STM_LOG_WARN("crashlog", L"{}", *chanErr);
            return false;
        }
        if (returned == 0) break;
        for (DWORD i = 0; i < returned && out->size() < maxCount; ++i) {
            UniqueEvt ev(batch[i]);  // 本轮迭代剩余时间由它持有句柄
            batch[i] = nullptr;
            DWORD used = 0;
            DWORD props = 0;
            std::vector<wchar_t> buf;
            bool rendered = false;
            for (int attempt = 0; attempt < 2 && !rendered; ++attempt) {
                const BOOL ok = EvtRender(nullptr, ev.get(), EvtRenderEventXml,
                                          static_cast<DWORD>(buf.size() * sizeof(wchar_t)),
                                          buf.empty() ? nullptr : buf.data(), &used, &props);
                if (ok) {
                    out->push_back(ParseEventXmlImpl(std::wstring(buf.data())));
                    rendered = true;
                    break;
                }
                const uint32_t hr = LastHr();
                if (hr != static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) ||
                    used == 0) {
                    break;  // 该事件不可恢复：跳过，保留其余
                }
                buf.assign(used / sizeof(wchar_t) + 1, L'\0');  // 探测结果：需要 `used` 大小
            }
            if (!rendered) STM_LOG_DEBUG("crashlog", L"跳过无法渲染的事件（EvtRender 失败）");
        }
        if (returned < kBatch) break;  // 通道已取空
    }
    return true;
}

CrashEvent Take(const std::vector<CrashEvent>& v, size_t* i) { return v[(*i)++]; }

}  // namespace

// 供测试导出：纯 XML -> CrashEvent 解析（无 I/O，无全局状态）。声明在
// src/selftest/control_test.cpp，刻意不放进冻结的契约头；
// selftest 可执行链接 stm_ops，符号在那里解析。
void ParseEventXml(const std::wstring& xml, CrashEvent* out) {
    *out = ParseEventXmlImpl(xml);
}

std::vector<CrashEvent> QueryCrashEvents(uint32_t maxCount, std::wstring* err) {
    std::vector<CrashEvent> result;
    if (err) err->clear();
    if (maxCount == 0) return result;

    // 两个最新在前的流合并为一个（按 unixTime 稳定二路归并）。
    std::vector<CrashEvent> app;
    std::vector<CrashEvent> sys;
    std::wstring appErr;
    std::wstring sysErr;
    QueryOneChannel(L"Application", maxCount, &app, &appErr);
    QueryOneChannel(L"System", maxCount, &sys, &sysErr);

    size_t i = 0;
    size_t j = 0;
    result.reserve(app.size() + sys.size());
    while ((i < app.size() || j < sys.size()) && result.size() < maxCount) {
        const bool takeApp =
            j >= sys.size() || (i < app.size() && app[i].unixTime >= sys[j].unixTime);
        result.push_back(takeApp ? Take(app, &i) : Take(sys, &j));
    }

    // 诚实的通道限制，聚合展示（评审 V15-P2）：两个通道都失败时，
    // 同时报告两个原因——单一消息可能掩盖另一通道的不同
    // 失败（如 Application 被策略阻止而 System 可写，或反之）。
    if (err) {
        if (!appErr.empty() && !sysErr.empty()) {
            *err = appErr + L"；" + sysErr;
        } else {
            *err = appErr.empty() ? sysErr : appErr;
        }
    }
    return result;
}

}  // namespace stm::ops
