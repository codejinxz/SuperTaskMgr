#pragma once
// 第 6 阶段契约：来自经典事件日志的崩溃/挂起历史（有文档的
// Windows 事件日志 API；Application+System 通道无需管理员即可读）。
// 归架构所有，已冻结。
#include <cstdint>
#include <string>
#include <vector>

namespace stm {
namespace ops {

struct CrashEvent {
    int64_t unixTime = 0;
    std::wstring provider;   // 如 "Application Error"
    uint32_t eventId = 0;    // 1000 = 应用错误，1001 = WER 报告，1002 = 应用挂起
    uint16_t level = 0;      // 2 = 错误，3 = 警告，…
    std::wstring app;        // 出错应用（来自 EventData，尽力而为）
    std::wstring module;     // 出错模块（尽力而为，可能为空）
    std::wstring summary;    // 简短人读文本（中文标签 + 关键字段）
};

// 最新在前，跨 Application+System 通道、针对 ID
// 1000/1001/1002 最多取 maxCount 条。空向量且 err 为空 = 近期无崩溃（诚实）。
std::vector<CrashEvent> QueryCrashEvents(uint32_t maxCount, std::wstring* err);

}  // namespace ops
}  // namespace stm
