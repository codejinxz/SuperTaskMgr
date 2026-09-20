#pragma once
// 日志读取核心（读取轮）。header-only、纯逻辑、无 Windows 依赖——文件 IO 只用
// <cstdio>，与 PageHelpers.h / CompatDiag.h 同一模式，stm_selftest 可无 GUI 直测。
//
// 职责边界：
//  - 只读消费者：以共享方式独立打开 stm.log（_wfopen_s "rb"，MSVC 默认 _SH_DENYNO，
//    允许写方继续写入）。与 Log.cpp 的互斥/FILE* 完全无关，绝不锁文件、绝不写回。
//  - 只读尾部：至多回退 kLogTailWindowBytes（512KB）窗口，不全量加载；并发写入
//    造成的尾部半行按"不完整"丢弃（incompleteTailDropped 置位，绝不当作真行）。
//  - 诚实呈现：不匹配格式的行原样进 unparsedLines；非法 UTF-8 行跳过并计数
//    （invalidUtf8Lines）；文件不存在/为空/读失败时 error 给出原因、结果为空。
//
// 时间语义（诚实约束）：Log.cpp 每行只写 HH:MM:SS.mmm、不含日期，因此
// LogEntry::unixTime 恒为 -1——不用"当天日期"去猜绝对时间（滚动文件会猜错）。
// 排序辅助字段 daySeconds 为当地当日秒数；跨午夜的窗口内它非单调，展示顺序
// 一律以数组顺序（文件顺序=追加顺序，旧→新）为准。
//
// 复用说明（系统信息）：FormatForReport 的系统信息行由参数注入。ui3 的
// SystemVersionLine()（CompatDiag.h）依赖 Win32（RtlGetVersion/GetNativeSystemInfo），
// 且该头位于 app 层并带入 collect 契约头——core 头文件不可反向依赖 app/ui3，
// 故此处不复用其函数体；UI 接线时传 ui3::SystemVersionLine() 即可与兼容诊断
// 报告的系统信息行完全一致。
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include "core/Str.h"  // Utf8ToWide / Fmt

namespace stm {

// 只读尾部窗口上限：1MB 滚动日志下 512KB 已覆盖一半现场，且避免 UI 一次性
// 拷贝大缓冲。公开为常量便于 selftest 与 UI 说明文档引用同一数值。
inline constexpr int64_t kLogTailWindowBytes = 512 * 1024;

struct LogEntry {
    int64_t unixTime = -1;    // 格式无日期，无法恢复绝对时间：恒为 -1（不伪造）
    int32_t daySeconds = -1;  // 当地当日 0 点起的秒数（HH*3600+MM*60+SS）；未解析为 -1
    std::wstring timeText;    // 原样保留，如 L"12:00:01.002"
    std::wstring level;       // 已去除填充空格：文件里写作 "INFO "/"WARN "（Log.cpp 补齐 5 列）
    std::wstring module;
    std::wstring message;
    bool parsed = true;       // 当前实现中 entries 内恒为 true；不匹配行进 unparsedLines
};

struct LogTail {
    std::vector<LogEntry> entries;            // 时间正序（旧→新，即文件顺序）
    std::vector<std::wstring> unparsedLines;  // 非日志行原样保留（已转宽字符），不伪造
    std::wstring error;                       // 非空 = 读取失败原因（不存在/为空/读失败）
    int64_t truncatedHeadBytes = 0;           // 窗口制下文件头部未读取的字节数（0=全量）
    bool incompleteTailDropped = false;       // 尾部半行（正被并发写入）被丢弃
    int invalidUtf8Lines = 0;                 // 非法 UTF-8 行被诚实跳过的条数
};

// 级别过滤纯函数：DEBUG < INFO < WARN < ERROR。两侧都做去空白 + ASCII 大写
// 归一化（容忍 "INFO " 填充与大小写）。未知级别视为无法定级：level 未知返回
// false，threshold 未知（调用方笔误）也返回 false——宁缺勿滥。
inline bool LevelAtLeast(const std::wstring& level, const wchar_t* threshold) {
    auto normalize = [](const std::wstring& s) {
        const size_t b = s.find_first_not_of(L" \t");
        if (b == std::wstring::npos) return std::wstring();
        std::wstring r = s.substr(b, s.find_last_not_of(L" \t") - b + 1);
        for (wchar_t& c : r) {
            if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
        }
        return r;
    };
    auto rank = [](const std::wstring& s) -> int {
        if (s == L"DEBUG") return 0;
        if (s == L"INFO") return 1;
        if (s == L"WARN") return 2;
        if (s == L"ERROR") return 3;
        return -1;
    };
    if (threshold == nullptr) return false;
    const int rt = rank(normalize(threshold));
    if (rt < 0) return false;  // 阈值本身未知：视为配置错误，不放行
    return rank(normalize(level)) >= rt;
}

namespace logfile_detail {

// 严格 UTF-8 校验（含过长编码、代理区、超界）。文件可能被外部工具改写，
// 转宽前先验证，坏行整行跳过而不是让转换函数悄悄替换。
inline bool IsValidUtf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        int n = 0;
        uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) { n = 2; cp = c & 0x1Fu; }
        else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0Fu; }
        else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07u; }
        else return false;
        if (i + static_cast<size_t>(n) > s.size()) return false;  // 尾部截断的多字节序列
        for (int k = 1; k < n; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (n == 2 && cp < 0x80) return false;          // 过长编码
        if (n == 3 && cp < 0x800) return false;
        if (n == 4 && cp < 0x10000) return false;
        if (cp > 0x10FFFF) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;  // 代理区不是合法字符
        i += static_cast<size_t>(n);
    }
    return true;
}

// 解析单行（UTF-8 字节）。格式（Log.cpp）：
//   "[HH:MM:SS.mmm] [LEVEL] [module] message\r\n"
// 只校验形状（时间 token 固定 12 字符、三段方括号、段间单空格），不校验
// 时分秒数值范围（写入方保证）；message 允许为空。
inline bool ParseLine(std::string_view line, LogEntry* out) {
    *out = LogEntry{};
    if (line.size() < 14 || line[0] != '[') return false;
    const size_t t1 = line.find(']', 1);
    if (t1 != 13) return false;  // HH:MM:SS.mmm 恒为 12 字符（%02d/%03d 补零）
    auto dig = [](char c) { return c >= '0' && c <= '9'; };
    if (!(dig(line[1]) && dig(line[2]) && line[3] == ':' && dig(line[4]) && dig(line[5]) &&
          line[6] == ':' && dig(line[7]) && dig(line[8]) && line[9] == '.' && dig(line[10]) &&
          dig(line[11]) && dig(line[12]))) {
        return false;
    }
    if (line.size() < 16 || line[14] != ' ' || line[15] != '[') return false;
    const size_t l2 = line.find(']', 16);
    if (l2 == std::string_view::npos) return false;
    if (line.size() < l2 + 3 || line[l2 + 1] != ' ' || line[l2 + 2] != '[') return false;
    const size_t l3 = line.find(']', l2 + 3);
    if (l3 == std::string_view::npos) return false;

    auto trim = [](std::string_view s) {
        const size_t b = s.find_first_not_of(" \t");
        if (b == std::string_view::npos) return std::string_view();
        return s.substr(b, s.find_last_not_of(" \t") - b + 1);
    };
    const std::string_view levelTok = trim(line.substr(16, l2 - 16));
    const std::string_view moduleTok = trim(line.substr(l2 + 3, l3 - (l2 + 3)));
    if (levelTok.empty() || moduleTok.empty()) return false;

    std::string_view msg;
    if (l3 + 1 == line.size()) {
        msg = std::string_view();  // 行尾即 ']'：空消息
    } else if (line[l3 + 1] == ' ') {
        msg = line.substr(l3 + 2);
    } else {
        return false;  // 段间不是单空格：不合写入格式，诚实交给 unparsedLines
    }

    out->timeText = Utf8ToWide(line.substr(1, 12));
    out->level = Utf8ToWide(levelTok);
    out->module = Utf8ToWide(moduleTok);
    out->message = Utf8ToWide(msg);
    const int hh = (line[1] - '0') * 10 + (line[2] - '0');
    const int mm = (line[4] - '0') * 10 + (line[5] - '0');
    const int ss = (line[7] - '0') * 10 + (line[8] - '0');
    out->daySeconds = hh * 3600 + mm * 60 + ss;
    out->unixTime = -1;  // 无日期来源，诚实置为"未知"
    out->parsed = true;
    return true;
}

}  // namespace logfile_detail

// 读取日志文件尾部至多 maxLines "行"（entries + unparsedLines 合计，从文件尾
// 往上数——UI"最新在下"直接按数组顺序展示即可）。时间正序返回（旧→新）。
// 失败路径（文件不存在/为空/读失败）：结果为空 + error 说明原因，绝不伪造。
inline LogTail ReadLogTail(const std::wstring& path, int maxLines) {
    LogTail out;
    if (maxLines <= 0) {
        out.error = L"maxLines 必须为正数";
        return out;
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr) {
        out.error = L"无法打开日志文件（可能不存在或被独占占用）";
        return out;
    }
    if (_fseeki64(f, 0, SEEK_END) != 0) {
        fclose(f);
        out.error = L"日志文件定位失败";
        return out;
    }
    const int64_t size = _ftelli64(f);
    if (size < 0) {
        fclose(f);
        out.error = L"获取日志文件大小失败";
        return out;
    }
    if (size == 0) {
        fclose(f);
        out.error = L"日志文件为空";
        return out;
    }
    const int64_t window = size < kLogTailWindowBytes ? size : kLogTailWindowBytes;
    out.truncatedHeadBytes = size - window;
    if (_fseeki64(f, size - window, SEEK_SET) != 0) {
        fclose(f);
        out.error = L"日志文件定位失败";
        return out;
    }
    std::string buf(static_cast<size_t>(window), '\0');
    const size_t got = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    if (got != buf.size()) {
        out.error = L"日志读取不完整（文件可能正被滚动或截断）";
        out.truncatedHeadBytes = 0;
        return out;
    }

    // 窗口覆盖文件头时剥掉 BOM（Log.cpp 不写 BOM，容错外部工具改写过的文件）。
    size_t pos = 0;
    if (out.truncatedHeadBytes == 0 && buf.size() >= 3 &&
        static_cast<unsigned char>(buf[0]) == 0xEF &&
        static_cast<unsigned char>(buf[1]) == 0xBB &&
        static_cast<unsigned char>(buf[2]) == 0xBF) {
        pos = 3;
    }

    // 切行（以 '\n' 分隔；'\r' 在消费时剥除，兼容 LF/CRLF 混排）。
    std::vector<std::string_view> lines;
    bool tailUnterminated = false;
    for (size_t start = pos; start < buf.size();) {
        const size_t nl = buf.find('\n', start);
        if (nl == std::string::npos) {
            lines.emplace_back(buf.data() + start, buf.size() - start);
            tailUnterminated = true;  // 最后一行没有换行符：正被写入的半行
            break;
        }
        lines.emplace_back(buf.data() + start, nl - start);
        start = nl + 1;
    }
    // 窗口起点可能落在某行中间：首行不完整，保守丢弃（宁少一行不猜内容）。
    if (out.truncatedHeadBytes > 0 && !lines.empty()) lines.erase(lines.begin());
    if (tailUnterminated && !lines.empty()) {
        lines.pop_back();
        out.incompleteTailDropped = true;
    }

    // maxLines 从尾部往上取（合计口径），再按文件顺序输出（旧→新）。
    size_t first = 0;
    if (lines.size() > static_cast<size_t>(maxLines)) {
        first = lines.size() - static_cast<size_t>(maxLines);
    }
    out.entries.reserve(lines.size() - first);
    for (size_t i = first; i < lines.size(); ++i) {
        std::string_view ln = lines[i];
        if (!ln.empty() && ln.back() == '\r') ln.remove_suffix(1);
        if (!logfile_detail::IsValidUtf8(ln)) {
            ++out.invalidUtf8Lines;  // 坏编码行：跳过并计数，不进任何展示列表
            continue;
        }
        LogEntry e;
        if (logfile_detail::ParseLine(ln, &e)) {
            out.entries.push_back(std::move(e));
        } else {
            out.unparsedLines.emplace_back(Utf8ToWide(ln));  // 原样保留，不伪造
        }
    }
    return out;
}

// 生成诊断上报格式文本（系统信息 + 最近 N 条 Error/Warn，供用户贴 issue）。
// systemInfo：由调用方注入（UI 层传 ui3::SystemVersionLine()，见文件头复用说明）；
// 为空时诚实标注"未提供"，绝不编造版本号。错误/警告按新→旧排列便于优先阅读。
inline std::wstring FormatForReport(const LogTail& tail, int maxErrors,
                                    const std::wstring& systemInfo = L"") {
    std::wstring t;
    t += L"SuperTaskMgr 日志诊断报告\n";
    t += L"========================================\n";
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);  // MSVC CRT（本工程仅 MSVC 目标）
    t += Fmt(L"生成时间：{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}\n",
             static_cast<unsigned>(local.tm_year + 1900), static_cast<unsigned>(local.tm_mon + 1),
             static_cast<unsigned>(local.tm_mday), static_cast<unsigned>(local.tm_hour),
             static_cast<unsigned>(local.tm_min), static_cast<unsigned>(local.tm_sec));
    t += L"系统版本：" +
         (systemInfo.empty() ? std::wstring(L"（未提供——应由 UI 层传入）") : systemInfo) + L"\n";

    t += L"\n[日志读取]\n";
    if (!tail.error.empty()) {
        t += L"  读取失败：" + tail.error + L"\n";
    } else {
        t += Fmt(L"  读取范围：最近 {} 行（日志条目 {}，未解析行 {}）\n",
                 tail.entries.size() + tail.unparsedLines.size(), tail.entries.size(),
                 tail.unparsedLines.size());
        if (tail.truncatedHeadBytes > 0) {
            t += Fmt(L"  注意：仅读取文件尾部窗口，头部 {} 字节未纳入读取。\n",
                     tail.truncatedHeadBytes);
        }
        if (tail.incompleteTailDropped) t += L"  注意：尾部正在写入的半行已丢弃。\n";
        if (tail.invalidUtf8Lines > 0) {
            t += Fmt(L"  注意：{} 行因非 UTF-8 编码被跳过。\n", tail.invalidUtf8Lines);
        }
    }

    const int limit = maxErrors < 0 ? 0 : maxErrors;
    t += Fmt(L"\n[最近错误与警告]（新→旧，至多 {} 条）\n", limit);
    if (tail.error.empty()) {
        int picked = 0;
        for (auto it = tail.entries.rbegin(); it != tail.entries.rend() && picked < limit; ++it) {
            if (!LevelAtLeast(it->level, L"WARN")) continue;
            t += L"  [" + it->timeText + L"] [" + it->level + L"] [" + it->module + L"] " +
                 it->message + L"\n";
            ++picked;
        }
        if (picked == 0) t += L"  （读取窗口内没有 ERROR/WARN 记录）\n";
    }

    t += L"\n[隐私提示] 提交前请自行确认以上内容不含敏感信息；应用不会自动上传任何数据。\n";
    return t;
}

}  // namespace stm
