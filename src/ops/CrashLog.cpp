// Crash/hang history from the classic Windows Event Log (contract ops/CrashLog.h).
//
// Documented API only (wevtapi): EvtQuery(channel path, XPath) over "Application" and
// "System" in reverse (newest-first) direction for EventID 1000 (app error) /
// 1001 (WER report) / 1002 (app hang), then EvtRender(EvtRenderXml) and a small
// self-contained XML field scan for TimeCreated / Provider / Level / EventData
// App+module names. Both channels are readable without admin. No admin, no ETW,
// no third-party deps. All EVT_HANDLEs are RAII-owned.
//
// Field names are matched best-effort per the documented WER payload: 1000 uses
// AppName/AppPath/FaultingModule(-Path); 1001/1002 reuse the same names when present
// (WER payloads vary); unmatched fields stay honestly empty.
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

// Best-effort XML entity decode (named entities; the EventLog renderer escapes the
// usual five). &amp; is decoded LAST so "&amp;lt;" yields "&lt;", not "<".
std::wstring UnescapeXml(const std::wstring& s) {
    if (s.find(L'&') == std::wstring::npos) return s;
    std::wstring out = ReplaceAll(s, L"&lt;", L"<");
    out = ReplaceAll(out, L"&gt;", L">");
    out = ReplaceAll(out, L"&quot;", L"\"");
    out = ReplaceAll(out, L"&apos;", L"'");
    out = ReplaceAll(out, L"&amp;", L"&");
    return out;
}

// True when the char at `p` plausibly starts the tag we searched for (avoids matching
// longer element names that merely share a prefix).
bool TagBoundary(const std::wstring& xml, size_t nameEnd) {
    if (nameEnd >= xml.size()) return false;
    const wchar_t c = xml[nameEnd];
    return c == L'>' || c == L'/' || c == L' ';
}

// First attribute value of `attr` inside the "<tag ...>" element that starts the XML.
std::wstring AttributeInFirstTag(const std::wstring& xml, const wchar_t* tag,
                                 const wchar_t* attr) {
    const std::wstring open = std::wstring(L"<") + tag;
    size_t p = xml.find(open);
    if (p == std::wstring::npos || !TagBoundary(xml, p + open.size())) return {};
    const size_t gt = xml.find(L'>', p);
    if (gt == std::wstring::npos) return {};
    const std::wstring tagText = xml.substr(p, gt - p);  // "<Provider Name=\"...\" ..."
    const std::wstring pat = std::wstring(L" ") + attr + L"=\"";
    const size_t a = tagText.find(pat);
    if (a == std::wstring::npos) return {};
    const size_t vstart = a + pat.size();
    const size_t vend = tagText.find(L'"', vstart);
    if (vend == std::wstring::npos) return {};
    return UnescapeXml(tagText.substr(vstart, vend - vstart));
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
    return UnescapeXml(xml.substr(gt + 1, ce - gt - 1));
}

// ISO-8601 "2026-09-18T03:14:15.1234567Z" -> unix seconds (SystemTime is UTC).
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

// Parse every "<Data Name=\"X\">value</Data>" (self-closing forms yield empty values).
void ParseEventData(const std::wstring& xml, std::vector<std::pair<std::wstring, std::wstring>>* out) {
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
        const std::wstring tagText = xml.substr(p, gt - p);
        const std::wstring pat = L" Name=\"";
        std::wstring name;
        const size_t a = tagText.find(pat);
        if (a != std::wstring::npos) {
            const size_t vstart = a + pat.size();
            const size_t vend = tagText.find(L'"', vstart);
            if (vend != std::wstring::npos) {
                name = tagText.substr(vstart, vend - vstart);
            }
        }
        std::wstring value;
        if (!selfClosing) {
            const size_t ce = xml.find(L"</Data>", gt);
            if (ce != std::wstring::npos) {
                value = UnescapeXml(xml.substr(gt + 1, ce - gt - 1));
                p = ce + 7;
            } else {
                p = gt;  // malformed: stop scanning this element, continue after it
            }
        } else {
            p = gt;
        }
        if (!name.empty()) out->emplace_back(std::move(name), std::move(value));
        p = xml.find(L"<Data", p);
    }
}

const std::wstring* FindData(const std::vector<std::pair<std::wstring, std::wstring>>& data,
                             const wchar_t* name) {
    for (const auto& d : data) {
        if (d.first == name) return &d.second;
    }
    return nullptr;
}

std::wstring BuildSummary(uint32_t eventId, const std::wstring& app, const std::wstring& module) {
    const wchar_t* head = eventId == 1000   ? L"应用崩溃"
                          : eventId == 1001 ? L"错误报告"
                          : eventId == 1002 ? L"应用无响应"
                                            : L"异常事件";
    std::wstring s = std::wstring(head) + L"：" + (app.empty() ? L"（应用未知）" : app);
    if (!module.empty()) s += L"（模块 " + module + L"）";
    return s;
}

CrashEvent ParseEventXml(const std::wstring& xml) {
    CrashEvent ev;
    ev.unixTime = ParseEventTime(AttributeInFirstTag(xml, L"TimeCreated", L"SystemTime"));
    ev.provider = AttributeInFirstTag(xml, L"Provider", L"Name");
    ev.eventId = ToU32(ElementText(xml, L"EventID"));
    ev.level = static_cast<uint16_t>(ToU32(ElementText(xml, L"Level")));

    std::vector<std::pair<std::wstring, std::wstring>> data;
    ParseEventData(xml, &data);
    if (const std::wstring* v = FindData(data, L"AppName")) {
        ev.app = ImageNameOf(*v);
    }
    if (ev.app.empty()) {
        if (const std::wstring* v = FindData(data, L"AppPath")) ev.app = ImageNameOf(*v);
    }
    if (const std::wstring* v = FindData(data, L"FaultingModule")) {
        ev.module = ImageNameOf(*v);
    }
    if (ev.module.empty()) {
        if (const std::wstring* v = FindData(data, L"FaultingModulePath")) {
            ev.module = ImageNameOf(*v);
        }
    }
    if (ev.app.empty()) ev.app = ev.provider;  // better than nothing for summary text
    ev.summary = BuildSummary(ev.eventId, ev.app, ev.module);
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
                    out->push_back(ParseEventXml(std::wstring(buf.data())));
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
