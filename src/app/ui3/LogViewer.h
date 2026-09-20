#pragma once
// 日志查看器纯逻辑（P-C 维护轮）。header-only、不依赖 ImGui——与
// PageHelpers.h / MemCleanup.h 同一模式，stm_selftest 只链 core 即可覆盖
//（LogTail/ReadLogTail/LevelAtLeast/FormatForReport 契约见 core/LogFile.h，
// 本头只做展示层变换，零副作用）。
//
// 职责边界：
//  - 级别过滤模式（全部 / 仅警告+错误）与显示行构建（映射 core::LevelAtLeast）。
//  - 表格行文本：解析行原样保留四字段；未解析行诚实占位（不伪造字段）。
//  - 着色类别：ERROR 红 / WARN 黄 / 其他默认（UI 侧映射 Theme 强调色）。
//  - 底部统计行 / 读取失败说明 / 诊断报告拼接：纯文本生成，UI 与 selftest 共用。
//
// 实际文件读取（stm::ReadLogTail）与报告落盘（CompatDiag.h::SaveDiagnosticsFile）
// 在调用方（app/ui/Pages.cpp 的日志查看器模态）完成。
#include <string>
#include <vector>

#include "core/LogFile.h"
#include "core/Str.h"

namespace stm {
namespace ui3 {

// 大日志只显示尾部 500 行（ReadLogTail 的 maxLines 参数；512KB 尾窗之内再按
// 行数截到最新 500 行）。公开常量便于 UI 与 selftest 引用同一数值。
inline constexpr int kLogViewerMaxLines = 500;

// 级别过滤模式（下拉两态）。
inline constexpr int kLogFilterAll = 0;
inline constexpr int kLogFilterWarnAndAbove = 1;

inline const wchar_t* LogFilterModeLabel(int mode) {
    switch (mode) {
        case kLogFilterWarnAndAbove: return L"仅警告+错误";
        case kLogFilterAll:
        default: return L"全部";
    }
}

// 行着色类别（ERROR 红 / WARN 黄 / 其他默认）。未知级别一律默认——不猜测。
enum class LogRowTone { Default, Warn, Error };

inline LogRowTone LogRowToneOf(const std::wstring& level) {
    if (LevelAtLeast(level, L"ERROR")) return LogRowTone::Error;
    if (LevelAtLeast(level, L"WARN")) return LogRowTone::Warn;
    return LogRowTone::Default;
}

// 表格行文本（时间/级别/模块/消息四列）。解析行原样保留字段。
struct LogRowCells {
    std::wstring time;
    std::wstring level;
    std::wstring module;
    std::wstring message;
};

inline LogRowCells LogRowCellTexts(const stm::LogEntry& e) {
    return LogRowCells{e.timeText, e.level, e.module, e.message};
}

// 未解析行的诚实占位：时间/级别/模块不可恢复 →「—」/「RAW」，消息 = 原文。
inline LogRowCells UnparsedRowCellTexts(const std::wstring& rawLine) {
    return LogRowCells{L"—", L"RAW", L"—", rawLine};
}

// 显示行：unparsed=false 时 index 指向 tail.entries，否则指向 tail.unparsedLines。
struct LogDisplayRow {
    bool unparsed = false;
    int index = 0;
};

// 生成显示行（文件顺序旧→新，最新在表尾）：
//  - 全部：解析行在前（文件序），未解析行追加在表尾。ReadLogTail 的两个数组
//    不保留彼此的原始交错顺序，UI 以统计行如实标注，绝不伪造顺序。
//  - 仅警告+错误：只保留 WARN/ERROR 解析行（未解析行无法定级，过滤态不显示）。
inline std::vector<LogDisplayRow> BuildDisplayRows(const stm::LogTail& tail, int filterMode) {
    std::vector<LogDisplayRow> rows;
    rows.reserve(tail.entries.size() + tail.unparsedLines.size());
    for (int i = 0; i < static_cast<int>(tail.entries.size()); ++i) {
        const stm::LogEntry& e = tail.entries[static_cast<size_t>(i)];
        if (filterMode == kLogFilterWarnAndAbove && !LevelAtLeast(e.level, L"WARN")) continue;
        rows.push_back(LogDisplayRow{false, i});
    }
    if (filterMode == kLogFilterAll) {
        for (int i = 0; i < static_cast<int>(tail.unparsedLines.size()); ++i) {
            rows.push_back(LogDisplayRow{true, i});
        }
    }
    return rows;
}

// 底部统计（诚实口径：总数含未解析行；错误/警告只统计可解析行）。
struct LogViewerStats {
    int totalLines = 0;  // entries + unparsedLines（读取窗口内全部行）
    int errorCount = 0;
    int warnCount = 0;
};

inline LogViewerStats SummarizeTail(const stm::LogTail& tail) {
    LogViewerStats s;
    s.totalLines = static_cast<int>(tail.entries.size() + tail.unparsedLines.size());
    for (const stm::LogEntry& e : tail.entries) {
        const LogRowTone t = LogRowToneOf(e.level);
        if (t == LogRowTone::Error) {
            ++s.errorCount;
        } else if (t == LogRowTone::Warn) {
            ++s.warnCount;
        }
    }
    return s;
}

// 底部统计行文本；读取失败时返回空（失败另有专属文案 LogReadFailureText）。
inline std::wstring LogStatsLine(const stm::LogTail& tail) {
    if (!tail.error.empty()) return std::wstring();
    const LogViewerStats s = SummarizeTail(tail);
    std::wstring t = Fmt(L"共 {} 条，其中 Error {} 条、Warn {} 条", s.totalLines, s.errorCount,
                         s.warnCount);
    if (!tail.unparsedLines.empty()) {
        t += Fmt(L"；{} 行未按日志格式解析（显示在表尾）", tail.unparsedLines.size());
    }
    if (tail.truncatedHeadBytes > 0) {
        t += Fmt(L"；仅显示尾部 {}KB（头部 {} 字节未读）", kLogTailWindowBytes / 1024,
                 tail.truncatedHeadBytes);
    }
    if (tail.incompleteTailDropped) t += L"；尾部正在写入的半行已丢弃";
    if (tail.invalidUtf8Lines > 0) t += Fmt(L"；{} 行非 UTF-8 已跳过", tail.invalidUtf8Lines);
    return t;
}

// 读取失败的诚实文案：原因 + 确切路径（模态内直接展示，不弹独立错误框）。
inline std::wstring LogReadFailureText(const stm::LogTail& tail, const std::wstring& path) {
    if (tail.error.empty()) return std::wstring();
    return Fmt(L"原因：{}\n日志路径：{}\n\n应用运行中会持续写日志；稍后点「刷新」重试，"
               L"或点「打开日志目录」查看文件是否存在。",
               tail.error, path);
}

// 诊断报告拼接：日志部分（stm::FormatForReport，系统信息行由 UI 注入
// ui3::SystemVersionLine()）+ 可选的兼容模式诊断部分（CompatDiag.h 的
// CompatDiagReportText；仅在采集降级时附加，未降级只含日志部分）。
// 拆成纯函数使拼接规则（附加条件 + 分隔线）可被 selftest 无 GUI 验证。
inline std::wstring AssembleDiagnosticReport(const std::wstring& logPart,
                                             const std::wstring& compatPart,
                                             bool includeCompatPart) {
    if (!includeCompatPart || compatPart.empty()) return logPart;
    std::wstring t = logPart;
    t += L"\n========================================\n";
    t += compatPart;
    return t;
}

}  // namespace ui3
}  // namespace stm
