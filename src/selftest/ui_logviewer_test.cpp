// 日志查看器 UI 纯逻辑自测（P-C）。覆盖：级别过滤下拉映射与显示行构建、
// 日志行→表格行文本纯函数（含未解析行诚实占位）、着色类别映射、统计行文本、
// 读取失败文案、诊断报告拼接规则、maxLines=500 上限。
// 全部走 app/ui3/LogViewer.h 纯函数 + 真实 ReadLogTail（临时"假日志"文件，
// 与 logfile_test.cpp 同法，不触碰 Log.cpp 全局写入状态）。
#include "selftest/TestFramework.h"
#include "app/ui3/LogViewer.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// 临时目录下的独立文件名（互不冲突，测试末尾自清理）。
std::wstring TempLog(const wchar_t* name) {
    wchar_t temp[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, temp);
    return std::wstring(temp) + name;
}

// 直接写字节：源文件以 /utf-8 编译，窄字面量即 UTF-8 字节流。
bool WriteBytes(const std::wstring& path, const std::string& bytes) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || f == nullptr) return false;
    const bool ok = bytes.empty() || fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    fclose(f);
    return ok;
}

}  // namespace

// --- 1. 过滤下拉映射 + 显示行构建（全部 vs 仅警告+错误，顺序保持旧→新）------
STM_TEST(logviewer_filter_and_rows) {
    if (std::wstring(stm::ui3::LogFilterModeLabel(stm::ui3::kLogFilterAll)) != L"全部" ||
        std::wstring(stm::ui3::LogFilterModeLabel(stm::ui3::kLogFilterWarnAndAbove)) != L"仅警告+错误" ||
        std::wstring(stm::ui3::LogFilterModeLabel(99)) != L"全部") {
        *err = L"LogFilterModeLabel 映射不符";
        return false;
    }
    const std::wstring p = TempLog(L"stm_ui_logviewer_test_1.log");
    if (!WriteBytes(p, "[09:00:00.000] [INFO ] [app] 启动\r\n"
                       "[09:00:01.000] [WARN ] [cfg] 配置缺失\r\n"
                       "[09:00:02.000] [ERROR] [disk] 磁盘写入失败\r\n"
                       "==== 会话开始 ====\r\n"
                       "[09:00:03.000] [INFO ] [app] 恢复\r\n")) {
        *err = L"写入测试日志失败";
        return false;
    }
    const stm::LogTail tail = stm::ReadLogTail(p, stm::ui3::kLogViewerMaxLines);
    ::DeleteFileW(p.c_str());
    if (!tail.error.empty()) { *err = L"读取报错：" + tail.error; return false; }

    // 全部：4 条解析行 + 1 条未解析行（追加在表尾）。
    const std::vector<stm::ui3::LogDisplayRow> all = stm::ui3::BuildDisplayRows(tail, stm::ui3::kLogFilterAll);
    if (all.size() != 5) { *err = L"全部模式应有 5 行（4 解析 + 1 未解析）"; return false; }
    if (all[0].unparsed || all[0].index != 0 || all[3].unparsed || all[3].index != 3) {
        *err = L"解析行应按文件序在前";
        return false;
    }
    if (!all[4].unparsed || all[4].index != 0) { *err = L"未解析行应追加在表尾"; return false; }

    // 仅警告+错误：WARN、ERROR 各 1 条，顺序保持旧→新；未解析行不放行。
    const std::vector<stm::ui3::LogDisplayRow> warnUp =
        stm::ui3::BuildDisplayRows(tail, stm::ui3::kLogFilterWarnAndAbove);
    if (warnUp.size() != 2) { *err = L"仅警告+错误应恰好 2 行"; return false; }
    if (tail.entries[static_cast<size_t>(warnUp[0].index)].message != L"配置缺失" ||
        tail.entries[static_cast<size_t>(warnUp[1].index)].message != L"磁盘写入失败") {
        *err = L"过滤行应含 WARN 与 ERROR 且保持文件序";
        return false;
    }
    return true;
}

// --- 2. 日志行→表格行文本纯函数（含未解析行诚实占位）+ 着色类别 -------------
STM_TEST(logviewer_row_cells_and_tone) {
    const std::wstring p = TempLog(L"stm_ui_logviewer_test_2.log");
    if (!WriteBytes(p, "[12:00:01.002] [ERROR] [collect] 读取失败: 拒绝访问\r\n")) {
        *err = L"写入测试日志失败";
        return false;
    }
    const stm::LogTail tail = stm::ReadLogTail(p, stm::ui3::kLogViewerMaxLines);
    ::DeleteFileW(p.c_str());
    if (!tail.error.empty() || tail.entries.size() != 1) {
        *err = L"应恰好解析出 1 条";
        return false;
    }
    const stm::ui3::LogRowCells c = stm::ui3::LogRowCellTexts(tail.entries[0]);
    if (c.time != L"12:00:01.002" || c.level != L"ERROR" || c.module != L"collect" ||
        c.message != L"读取失败: 拒绝访问") {
        *err = L"解析行四列文本不符";
        return false;
    }
    const stm::ui3::LogRowCells raw = stm::ui3::UnparsedRowCellTexts(L"不是日志行");
    if (raw.time != L"—" || raw.level != L"RAW" || raw.module != L"—" ||
        raw.message != L"不是日志行") {
        *err = L"未解析行应诚实占位（—/RAW/—/原文）";
        return false;
    }
    // 着色：ERROR 红 / WARN 黄（含填充空格与大小写）/ 其他默认（不猜测）。
    if (stm::ui3::LogRowToneOf(L"ERROR") != stm::ui3::LogRowTone::Error ||
        stm::ui3::LogRowToneOf(L"error") != stm::ui3::LogRowTone::Error ||
        stm::ui3::LogRowToneOf(L"WARN ") != stm::ui3::LogRowTone::Warn ||
        stm::ui3::LogRowToneOf(L"INFO") != stm::ui3::LogRowTone::Default ||
        stm::ui3::LogRowToneOf(L"TRACE") != stm::ui3::LogRowTone::Default ||
        stm::ui3::LogRowToneOf(L"") != stm::ui3::LogRowTone::Default) {
        *err = L"LogRowToneOf 映射不符";
        return false;
    }
    return true;
}

// --- 3. 统计行文本（计数/截断/未解析/坏编码）+ 读取失败文案 -----------------
STM_TEST(logviewer_stats_and_failure_text) {
    const std::wstring p = TempLog(L"stm_ui_logviewer_test_3.log");
    if (!WriteBytes(p, "[09:00:00.000] [INFO ] [app] 启动\r\n"
                       "[09:00:01.000] [WARN ] [cfg] 配置缺失\r\n"
                       "[09:00:02.000] [ERROR] [disk] 磁盘写入失败\r\n"
                       "垃圾行\r\n")) {
        *err = L"写入测试日志失败";
        return false;
    }
    const stm::LogTail tail = stm::ReadLogTail(p, stm::ui3::kLogViewerMaxLines);
    ::DeleteFileW(p.c_str());
    if (!tail.error.empty()) { *err = L"读取报错：" + tail.error; return false; }

    const stm::ui3::LogViewerStats s = stm::ui3::SummarizeTail(tail);
    if (s.totalLines != 4 || s.errorCount != 1 || s.warnCount != 1) {
        *err = L"统计应为 共4/Error1/Warn1";
        return false;
    }
    const std::wstring line = stm::ui3::LogStatsLine(tail);
    if (line.find(L"共 4 条") == std::wstring::npos ||
        line.find(L"Error 1 条") == std::wstring::npos ||
        line.find(L"Warn 1 条") == std::wstring::npos) {
        *err = L"统计行缺计数：" + line;
        return false;
    }
    if (line.find(L"1 行未按日志格式解析") == std::wstring::npos) {
        *err = L"统计行应标注未解析行数：" + line;
        return false;
    }
    // 截断提示：truncatedHeadBytes > 0 时必须出现「仅显示尾部 512KB」。
    stm::LogTail truncated = tail;  // 拷贝后注入窗口截断标记（纯文本变换可测）
    truncated.truncatedHeadBytes = 12345;
    const std::wstring tline = stm::ui3::LogStatsLine(truncated);
    if (tline.find(L"仅显示尾部 512KB（头部 12345 字节未读）") == std::wstring::npos) {
        *err = L"截断时应显示「仅显示尾部 512KB」：" + tline;
        return false;
    }
    // 读取失败：统计行为空，失败文案包含原因与确切路径。
    stm::LogTail bad;
    bad.error = L"日志文件为空";
    if (!stm::ui3::LogStatsLine(bad).empty()) { *err = L"失败时统计行应为空"; return false; }
    const std::wstring fail = stm::ui3::LogReadFailureText(bad, L"C:\\logs\\stm.log");
    if (fail.find(L"日志文件为空") == std::wstring::npos ||
        fail.find(L"C:\\logs\\stm.log") == std::wstring::npos) {
        *err = L"失败文案应含原因与路径";
        return false;
    }
    if (!stm::ui3::LogReadFailureText(tail, L"x").empty()) {
        *err = L"成功读取时失败文案应为空";
        return false;
    }
    return true;
}

// --- 4. 诊断报告拼接规则：未降级只含日志部分，降级附加兼容部分 -------------
STM_TEST(logviewer_report_assemble) {
    const std::wstring logPart = L"[日志部分]\n最近错误与警告…\n";
    const std::wstring compatPart = L"[兼容部分]\n自检逐项结果…\n";

    // 未降级：只含日志部分（compatPart 即使非空也不附加）。
    if (stm::ui3::AssembleDiagnosticReport(logPart, compatPart, false) != logPart) {
        *err = L"includeCompat=false 应原样返回日志部分";
        return false;
    }
    // 降级但兼容部分为空：同样只含日志部分。
    if (stm::ui3::AssembleDiagnosticReport(logPart, L"", true) != logPart) {
        *err = L"compatPart 为空应原样返回日志部分";
        return false;
    }
    // 降级：日志部分在前，分隔线之后是兼容部分（可整段复制上报）。
    const std::wstring full = stm::ui3::AssembleDiagnosticReport(logPart, compatPart, true);
    if (full.compare(0, logPart.size(), logPart) != 0) {
        *err = L"拼接应以日志部分开头";
        return false;
    }
    if (full.compare(full.size() - compatPart.size(), compatPart.size(), compatPart) != 0) {
        *err = L"拼接应以兼容部分结尾";
        return false;
    }
    const std::wstring sepAfterLog = logPart + L"\n====";
    if (full.find(sepAfterLog) != 0) {
        *err = L"两部分之间应有分隔线";
        return false;
    }

    // 端到端：FormatForReport（注入系统信息）经 Assemble 后关键内容齐全。
    const std::wstring p = TempLog(L"stm_ui_logviewer_test_4.log");
    if (!WriteBytes(p, "[09:00:02.000] [ERROR] [disk] 磁盘写入失败\r\n")) {
        *err = L"写入测试日志失败";
        return false;
    }
    const stm::LogTail tail = stm::ReadLogTail(p, stm::ui3::kLogViewerMaxLines);
    ::DeleteFileW(p.c_str());
    if (!tail.error.empty()) { *err = L"读取报错：" + tail.error; return false; }
    const std::wstring report = stm::ui3::AssembleDiagnosticReport(
        stm::FormatForReport(tail, 50, L"Windows 测试（x64）"), compatPart, true);
    if (report.find(L"SuperTaskMgr 日志诊断报告") == std::wstring::npos ||
        report.find(L"Windows 测试（x64）") == std::wstring::npos ||
        report.find(L"磁盘写入失败") == std::wstring::npos ||
        report.find(L"[兼容部分]") == std::wstring::npos) {
        *err = L"端到端报告缺关键段落";
        return false;
    }
    // 未降级（includeCompat=false）时同一日志部分的报告不含兼容段落。
    const std::wstring logOnly = stm::ui3::AssembleDiagnosticReport(
        stm::FormatForReport(tail, 50, L"Windows 测试（x64）"), compatPart, false);
    if (logOnly.find(L"[兼容部分]") != std::wstring::npos) {
        *err = L"未降级报告不应含兼容段落";
        return false;
    }
    return true;
}

// --- 5. 500 行上限：超长日志只显示尾部 kLogViewerMaxLines 行（最新完好）----
STM_TEST(logviewer_maxlines_cap) {
    const std::wstring p = TempLog(L"stm_ui_logviewer_test_5.log");
    std::string content;
    for (int i = 1; i <= 600; ++i) {
        content += "[10:00:00.000] [INFO ] [filler] 行" + std::to_string(i) + "\r\n";
    }
    content += "[23:59:59.999] [ERROR] [end] 最后一条\r\n";
    if (!WriteBytes(p, content)) { *err = L"写入测试日志失败"; return false; }
    const stm::LogTail tail = stm::ReadLogTail(p, stm::ui3::kLogViewerMaxLines);
    ::DeleteFileW(p.c_str());
    if (!tail.error.empty()) { *err = L"读取报错：" + tail.error; return false; }
    if (tail.entries.size() != static_cast<size_t>(stm::ui3::kLogViewerMaxLines)) {
        *err = L"应恰好显示最新 500 行";
        return false;
    }
    const std::vector<stm::ui3::LogDisplayRow> rows = stm::ui3::BuildDisplayRows(tail, stm::ui3::kLogFilterAll);
    if (rows.size() != tail.entries.size()) { *err = L"全部模式行数应等于条目数"; return false; }
    if (tail.entries.back().message != L"最后一条" ||
        stm::ui3::LogRowToneOf(tail.entries.back().level) != stm::ui3::LogRowTone::Error) {
        *err = L"最新一条（ERROR）必须完好且在表尾";
        return false;
    }
    return true;
}
