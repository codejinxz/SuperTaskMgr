// Crash/hang history from the classic Windows Event Log (contract ops/CrashLog.h).
//
// Documented API only (wevtapi): EvtQuery(channel path, XPath) over "Application" and
// "System" in reverse (newest-first) direction for EventID 1000 (app error) /
// 1001 (WER report) / 1002 (app hang), then EvtRender(EvtRenderXml) and a small
// self-contained XML field scan for TimeCreated / Provider / Level / EventData.
// Both channels are readable without admin. No admin, no ETW, no third-party deps.
// All EVT_HANDLEs are RAII-owned.
//
// EventData has two real-world shapes and BOTH are parsed (P1 fix: shape B used to
// degrade every .NET Runtime row to "未知/-"):
//   A) named fields  — <Data Name='AppName'>x.exe</Data> (Application Error/Hang,
//      WER): AppName/AppPath/Application -> app; FaultingModule*/Module -> module;
//   B) unnamed blobs — <Data>Category: ...&#xA;Exception: ...</Data> (.NET Runtime):
//      the first non-empty line feeds the summary; app/module come from best-effort
//      text heuristics (first *.exe token; *.dll/*.sys token after "faulting
//      module"/"模块"). Nothing extractable stays empty.
// Entries where neither track yields an app fall back to the Execution ProcessID
// ("PID 87360") — never a "未知" placeholder. Attribute values in both quote styles
// (single/double) are accepted; numeric entities (&#xA;) decode; a CDATA payload
// (not emitted by the renderer today) passes through verbatim.
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

constexpr uint64_t kFileTimeToUnixEpoch = 116444736000000000ull;  // 1601->1970, 100ns
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

// Best-effort XML entity decode: the five named entities plus numeric character
// references (&#xA; / &#13; — the renderer escapes newlines numerically). Single-pass
// left-to-right, so "&amp;lt;" yields "&lt;" (never re-decoded); a bare "&" or an
// unparsable reference stays verbatim.
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
        if (i + 1 < s.size() && s[i + 1] == L'#') {  // numeric reference
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
        out += s[i++];  // bare '&': keep verbatim
    }
    return out;
}

// Decode an element payload: a CDATA block (never emitted by EvtRender today, kept
// for robustness) passes through verbatim; plain text gets entity-decoded.
std::wstring DecodeXmlText(const std::wstring& raw) {
    constexpr wchar_t kCdataOpen[] = L"<![CDATA[";
    constexpr size_t kCdataOpenLen = 9;
    if (raw.size() >= kCdataOpenLen + 3 && raw.compare(0, kCdataOpenLen, kCdataOpen) == 0 &&
        raw.compare(raw.size() - 3, 3, L"]]>") == 0) {
        return raw.substr(kCdataOpenLen, raw.size() - kCdataOpenLen - 3);
    }
    return UnescapeXml(raw);
}

// True when the char at `p` plausibly starts the tag we searched for (avoids matching
// longer element names that merely share a prefix).
bool TagBoundary(const std::wstring& xml, size_t nameEnd) {
    if (nameEnd >= xml.size()) return false;
    const wchar_t c = xml[nameEnd];
    return c == L'>' || c == L'/' || c == L' ';
}

// Value of attribute `attr` inside a "<Tag attr='v'>" tag text. Both quote styles
// occur in the wild (EvtRender and common capture tooling differ), and the attribute
// name must be a whole word: whitespace-preceded and immediately followed by '='.
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

// Attribute `attr` of the first "<tag ...>" element in the XML (empty when absent).
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

// Text of the first "<tag ...>value</tag>" element (empty when absent/self-closing).
std::wstring ElementText(const std::wstring& xml, const wchar_t* tag) {
    const std::wstring open = std::wstring(L"<") + tag;
    size_t p = xml.find(open);
    while (p != std::wstring::npos) {
        if (TagBoundary(xml, p + open.size())) break;
        p = xml.find(open, p + 1);
    }
    if (p == std::wstring::npos) return {};
    const size_t gt = xml.find(L'>', p);
    if (gt == std::wstring::npos || xml[gt - 1] == L'/') return {};  // malformed/self-closing
    const std::wstring close = std::wstring(L"</") + tag + L">";
    const size_t ce = xml.find(close, gt);
    if (ce == std::wstring::npos) return {};
    return DecodeXmlText(xml.substr(gt + 1, ce - gt - 1));
}

// ISO-8601 "2026-09-18T03:14:15[.1234567][Z]" -> unix seconds (SystemTime is UTC).
// swscanf stops after the seconds field, so optional fraction and the Z suffix (and
// their absence) are all tolerated.
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
    std::wstring name;   // empty for unnamed (free-text blob) payloads
    std::wstring value;
};

// Parse every "<Data ...>value</Data>" (self-closing forms yield empty values). Both
// real shapes are kept: named fields AND unnamed blobs (the .NET Runtime payload is
// one free-text Data element with &#xA; line breaks).
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
                p = gt;  // malformed: stop scanning this element, continue after it
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

// First whitespace-delimited token (from `from` on) whose tail equals one of
// `suffixes` (case-insensitive); common trailing punctuation is stripped.
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

// Case-insensitive search for `needle` at/after `from`; true + position when found.
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

// Blob heuristics (track B): the first *.exe token anywhere is the app; the first
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

// Chinese headline by provider (how the user reads the row); an unknown provider is
// shown verbatim instead of being collapsed into a generic label.
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

    // Track A: named payload fields (WER / Application Error / Application Hang).
    for (const wchar_t* n : {L"AppName", L"AppPath", L"Application"}) {
        if (!ev.app.empty()) break;
        if (const std::wstring* v = FindNamed(data, n)) ev.app = ImageNameOf(*v);
    }
    for (const wchar_t* n :
         {L"FaultingModule", L"FaultingModulePath", L"ModuleName", L"ModulePath", L"Module"}) {
        if (!ev.module.empty()) break;
        if (const std::wstring* v = FindNamed(data, n)) ev.module = ImageNameOf(*v);
    }

    // Track B: unnamed free-text payloads (.NET Runtime shape) — best effort.
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

    // WER 1001: the P1 named parameter IS the crashing application (real-machine
    // sample: P1=powershell.exe) — use it before the bare-PID fallback (V15-P2-1).
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

// One channel, newest-first (EvtQueryReverseDirection), bounded by maxCount.
// Empty result + empty chanErr = "no such events" (honest); open/next failures go to
// chanErr in Chinese.
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
    // RAII over the whole EvtNext batch (review V15-P1): every exit path — the maxCount
    // truncation that leaves the loop mid-batch, a channel error return, and the drain
    // break — must close handles EvtNext already delivered but we never took over.
    // Without this, a query hitting maxCount leaks up to kBatch-1 handles per refresh.
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
            UniqueEvt ev(batch[i]);  // owns the handle for the rest of this iteration
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
                    break;  // unrecoverable for this event: skip, keep the rest
                }
                buf.assign(used / sizeof(wchar_t) + 1, L'\0');  // probe said: needs `used`
            }
            if (!rendered) STM_LOG_DEBUG("crashlog", L"跳过无法渲染的事件（EvtRender 失败）");
        }
        if (returned < kBatch) break;  // channel drained
    }
    return true;
}

CrashEvent Take(const std::vector<CrashEvent>& v, size_t* i) { return v[(*i)++]; }

}  // namespace

// export for tests: pure XML -> CrashEvent parsing (no I/O, no globals). Declared in
// src/selftest/control_test.cpp, deliberately NOT in the frozen contract header; the
// selftest binary links stm_ops, so the symbol resolves there.
void ParseEventXml(const std::wstring& xml, CrashEvent* out) {
    *out = ParseEventXmlImpl(xml);
}

std::vector<CrashEvent> QueryCrashEvents(uint32_t maxCount, std::wstring* err) {
    std::vector<CrashEvent> result;
    if (err) err->clear();
    if (maxCount == 0) return result;

    // Two newest-first streams merged into one (stable two-way merge on unixTime).
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

    // Honest channel limitation, aggregated (review V15-P2): when BOTH channels fail,
    // report both causes — a single message could mask the other channel's distinct
    // failure (e.g. Application policy-blocked while System is writable, or vice versa).
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
