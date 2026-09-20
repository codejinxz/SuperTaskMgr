// 日志读取核心自测（读取轮）。覆盖：正常行解析（含中文）、并发半行丢弃、
// 非日志行进 unparsedLines、maxLines 截断取最新、LevelAtLeast 级别过滤、
// FormatForReport 上报文本、缺失/空文件失败路径、非法 UTF-8 行跳过、
// 512KB 窗口截头。
//
// 按任务约定：在临时目录手写"假 stm.log"（直接写 UTF-8 字节流，兼容 LF/CRLF），
// 不依赖 LogInit，也不触碰 Log.cpp 的全局写入状态。
#include "selftest/TestFramework.h"
#include "core/LogFile.h"
#include "core/Str.h"
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

// --- 1. 正常行解析：时间/级别/模块/消息逐字段核对（含中文），兼容 LF 与 CRLF -
STM_TEST(log_parse_normal) {
    const std::wstring p = TempLog(L"stm_logfile_test_1.log");
    // 第一行 LF 收尾、第二行 CRLF 收尾；"INFO " 带 Log.cpp 的填充空格。
    if (!WriteBytes(p, "[12:00:01.002] [INFO ] [core] 启动完成\n"
                       "[13:20:05.123] [ERROR] [collect] 读取失败: 拒绝访问\r\n")) {
        *err = L"写入测试日志失败";
        return false;
    }
    const stm::LogTail tail = stm::ReadLogTail(p, 100);
    ::DeleteFileW(p.c_str());
    if (!tail.error.empty()) { *err = L"读取报错：" + tail.error; return false; }
    if (tail.entries.size() != 2 || !tail.unparsedLines.empty()) {
        *err = L"应恰好解析出 2 条且无未解析行";
        return false;
    }
    const stm::LogEntry& a = tail.entries[0];
    if (a.timeText != L"12:00:01.002" || a.level != L"INFO" || a.module != L"core" ||
        a.message != L"启动完成" || a.daySeconds != 12 * 3600 + 1 || a.unixTime != -1 ||
        !a.parsed) {
        *err = L"第一行字段不符：level=" + a.level + L" msg=" + a.message;
        return false;
    }
    const stm::LogEntry& b = tail.entries[1];
    if (b.timeText != L"13:20:05.123" || b.level != L"ERROR" || b.module != L"collect" ||
        b.message != L"读取失败: 拒绝访问" || b.daySeconds != 13 * 3600 + 20 * 60 + 5) {
        *err = L"第二行字段不符：msg=" + b.message;
        return false;
    }
    if (tail.truncatedHeadBytes != 0 || tail.incompleteTailDropped ||
        tail.invalidUtf8Lines != 0) {
        *err = L"小文件不应有截断/半行/坏编码标记";
        return false;
    }
    return true;
}

// --- 2. 并发半行：末行无换行符（写入方正在写）必须整行丢弃并置位标记 --------
STM_TEST(log_incomplete_tail_dropped) {
    const std::wstring p = TempLog(L"stm_logfile_test_2.log");
    if (!WriteBytes(p, "[01:00:00.000] [INFO ] [a] 完整行\r\n"
                       "[01:00:01.000] [ERR")) {
        *err = L"写入测试日志失败";
        return false;
    }
    const stm::LogTail tail = stm::ReadLogTail(p, 100);
    ::DeleteFileW(p.c_str());
    if (!tail.error.empty()) { *err = L"读取报错：" + tail.error; return false; }
    if (!tail.incompleteTailDropped) { *err = L"未标记 incompleteTailDropped"; return false; }
    if (tail.entries.size() != 1 || tail.entries[0].message != L"完整行" ||
        !tail.unparsedLines.empty()) {
        *err = L"半行必须整行丢弃，不得残留在 entries/unparsedLines";
        return false;
    }
    return true;
}

// --- 3. 非日志行：原样保留进 unparsedLines，不伪造、不吞掉 ------------------
STM_TEST(log_unparsed_lines_kept) {
    const std::wstring p = TempLog(L"stm_logfile_test_3.log");
    if (!WriteBytes(p, "==== 会话开始 ====\r\n"
                       "[08:00:00.000] [WARN ] [cfg] 配置缺失\r\n"
                       "不是日志行\r\n")) {
        *err = L"写入测试日志失败";
        return false;
    }
    const stm::LogTail tail = stm::ReadLogTail(p, 100);
    ::DeleteFileW(p.c_str());
    if (!tail.error.empty()) { *err = L"读取报错：" + tail.error; return false; }
    if (tail.entries.size() != 1 || tail.entries[0].level != L"WARN" ||
        tail.entries[0].message != L"配置缺失") {
        *err = L"日志行解析失败";
        return false;
    }
    if (tail.unparsedLines.size() != 2 || tail.unparsedLines[0] != L"==== 会话开始 ====" ||
        tail.unparsedLines[1] != L"不是日志行") {
        *err = L"未解析行应原样保留（共 2 行）";
        return false;
    }
    return true;
}

// --- 4. maxLines 截断：只取最新 N 行，且输出保持旧→新顺序 -------------------
STM_TEST(log_maxlines_newest) {
    const std::wstring p = TempLog(L"stm_logfile_test_4.log");
    std::string content;
    for (int i = 1; i <= 5; ++i) {
        content += "[0" + std::to_string(i) + ":00:00.000] [INFO ] [seq] 第" +
                   std::to_string(i) + "条\r\n";
    }
    if (!WriteBytes(p, content)) { *err = L"写入测试日志失败"; return false; }
    const stm::LogTail tail = stm::ReadLogTail(p, 3);
    ::DeleteFileW(p.c_str());
    if (!tail.error.empty()) { *err = L"读取报错：" + tail.error; return false; }
    if (tail.entries.size() != 3 || !tail.unparsedLines.empty()) {
        *err = L"maxLines=3 应只保留 3 条";
        return false;
    }
    if (tail.entries[0].message != L"第3条" || tail.entries[1].message != L"第4条" ||
        tail.entries[2].message != L"第5条") {
        *err = L"应取最新的 3/4/5 条且保持旧→新顺序";
        return false;
    }
    if (tail.truncatedHeadBytes != 0) { *err = L"未开窗口时截头字节应为 0"; return false; }
    return true;
}

// --- 5. LevelAtLeast：归一化（填充空格/大小写）与未知级别的诚实语义 ---------
STM_TEST(log_level_at_least) {
    struct Case {
        const wchar_t* level;
        const wchar_t* threshold;
        bool want;
    };
    const Case cases[] = {
        {L"ERROR", L"WARN", true},     // ERROR ≥ WARN
        {L"WARN", L"ERROR", false},    // WARN < ERROR
        {L"WARN", L"WARN", true},      // 相等即通过
        {L"INFO", L"WARN", false},
        {L"INFO ", L"INFO", true},     // Log.cpp 填充空格要归一化
        {L"ERROR", L"error", true},    // 阈值大小写不敏感
        {L"DEBUG", L"DEBUG", true},
        {L"DEBUG", L"INFO", false},
        {L"TRACE", L"INFO", false},    // 未知级别：不放行
        {L"ERROR", L"VERBOSE", false}, // 未知阈值：视为配置错误
        {L"", L"WARN", false},
    };
    for (const Case& c : cases) {
        if (stm::LevelAtLeast(c.level, c.threshold) != c.want) {
            *err = std::wstring(L"LevelAtLeast(") + c.level + L", " + c.threshold + L") 应为 " +
                   (c.want ? L"true" : L"false");
            return false;
        }
    }
    if (stm::LevelAtLeast(L"ERROR", nullptr)) { *err = L"空阈值应返回 false"; return false; }
    return true;
}

// --- 6. FormatForReport：系统信息 + Error/Warn 行 + maxErrors 生效 ----------
STM_TEST(log_report_format) {
    const std::wstring p = TempLog(L"stm_logfile_test_6.log");
    if (!WriteBytes(p, "[09:00:00.000] [INFO ] [app] 启动\r\n"
                       "[09:00:01.000] [WARN ] [cfg] 配置缺失\r\n"
                       "[09:00:02.000] [ERROR] [disk] 磁盘写入失败\r\n"
                       "[09:00:03.000] [INFO ] [app] 恢复\r\n")) {
        *err = L"写入测试日志失败";
        return false;
    }
    const stm::LogTail tail = stm::ReadLogTail(p, 100);
    ::DeleteFileW(p.c_str());
    if (!tail.error.empty()) { *err = L"读取报错：" + tail.error; return false; }

    const std::wstring sys = L"Windows 10.0.19045（x64）";
    const std::wstring r10 = stm::FormatForReport(tail, 10, sys);
    if (r10.find(L"SuperTaskMgr 日志诊断报告") == std::wstring::npos ||
        r10.find(L"生成时间：") == std::wstring::npos) {
        *err = L"报告缺标题/生成时间";
        return false;
    }
    if (r10.find(sys) == std::wstring::npos) { *err = L"报告缺注入的系统信息"; return false; }
    // level 已归一化去填充空格：报告里是 "[WARN]" 而非文件里的 "[WARN ]"。
    if (r10.find(L"[ERROR] [disk] 磁盘写入失败") == std::wstring::npos ||
        r10.find(L"[WARN] [cfg] 配置缺失") == std::wstring::npos) {
        *err = L"报告应含 ERROR 与 WARN 条目";
        return false;
    }
    if (r10.find(L"隐私提示") == std::wstring::npos) { *err = L"报告缺隐私提示"; return false; }

    const std::wstring r1 = stm::FormatForReport(tail, 1, sys);
    if (r1.find(L"磁盘写入失败") == std::wstring::npos) {
        *err = L"maxErrors=1 应保留最新的 ERROR";
        return false;
    }
    if (r1.find(L"[WARN") != std::wstring::npos) {
        *err = L"maxErrors=1 不应再出现 WARN 条目";
        return false;
    }
    return true;
}

// --- 7. 失败路径诚实：文件不存在 / 空文件 → 空结果 + 原因 ------------------
STM_TEST(log_missing_and_empty_file) {
    const stm::LogTail missing = stm::ReadLogTail(TempLog(L"stm_logfile_test_no_such_文件.log"), 10);
    if (missing.error.empty() || !missing.entries.empty() || !missing.unparsedLines.empty()) {
        *err = L"文件缺失应返回空结果 + 错误原因";
        return false;
    }
    const std::wstring p = TempLog(L"stm_logfile_test_7.log");
    if (!WriteBytes(p, "")) { *err = L"写入空文件失败"; return false; }
    const stm::LogTail empty = stm::ReadLogTail(p, 10);
    ::DeleteFileW(p.c_str());
    if (empty.error.find(L"空") == std::wstring::npos || !empty.entries.empty()) {
        *err = L"空文件应说明原因为空";
        return false;
    }
    return true;
}

// --- 8. 非法 UTF-8 行：诚实跳过并计数，不影响前后好行 ----------------------
STM_TEST(log_invalid_utf8_skipped) {
    const std::wstring p = TempLog(L"stm_logfile_test_8.log");
    // \xFF\xFE 是非法 UTF-8 起始字节。
    if (!WriteBytes(p, "[07:00:00.000] [INFO ] [a] 前一行\r\n"
                       "\xFF\xFE 坏编码行\r\n"
                       "[07:00:02.000] [INFO ] [a] 后一行\r\n")) {
        *err = L"写入测试日志失败";
        return false;
    }
    const stm::LogTail tail = stm::ReadLogTail(p, 100);
    ::DeleteFileW(p.c_str());
    if (!tail.error.empty()) { *err = L"读取报错：" + tail.error; return false; }
    if (tail.invalidUtf8Lines != 1) { *err = L"应恰好跳过 1 条坏编码行"; return false; }
    if (tail.entries.size() != 2 || tail.entries[0].message != L"前一行" ||
        tail.entries[1].message != L"后一行") {
        *err = L"坏行前后的好行必须完好保留";
        return false;
    }
    if (!tail.unparsedLines.empty()) { *err = L"坏编码行应跳过而非进 unparsedLines"; return false; }
    return true;
}

// --- 9. 大文件窗口：>512KB 只读尾部，截头字节正确、最新行完好 --------------
STM_TEST(log_head_window_truncated) {
    const std::wstring p = TempLog(L"stm_logfile_test_9.log");
    std::string content;
    for (int i = 0; i < 13000; ++i) {  // 约 600KB > 512KB 窗口
        content += "[10:00:00.000] [INFO ] [filler] 填充行" + std::to_string(i) + "\r\n";
    }
    content += "[23:59:59.999] [ERROR] [end] 最后一条\r\n";
    if (!WriteBytes(p, content)) { *err = L"写入测试日志失败"; return false; }
    const stm::LogTail tail = stm::ReadLogTail(p, 2000);
    ::DeleteFileW(p.c_str());
    if (!tail.error.empty()) { *err = L"读取报错：" + tail.error; return false; }
    const int64_t wantTrunc = static_cast<int64_t>(content.size()) - stm::kLogTailWindowBytes;
    if (tail.truncatedHeadBytes != wantTrunc || wantTrunc <= 0) {
        *err = L"截头字节数应为 文件大小-512KB";
        return false;
    }
    if (tail.incompleteTailDropped) { *err = L"文件完整收尾不应报半行"; return false; }
    if (tail.entries.size() != 2000 || !tail.unparsedLines.empty()) {
        *err = L"窗口内应取满 2000 条且全部可解析";
        return false;
    }
    if (tail.entries.back().message != L"最后一条" ||
        tail.entries.back().level != L"ERROR") {
        *err = L"最新一条（ERROR）必须完好";
        return false;
    }
    for (const stm::LogEntry& e : tail.entries) {
        if (e.message == L"填充行0") { *err = L"文件头部的行不应出现在窗口内"; return false; }
    }
    return true;
}
