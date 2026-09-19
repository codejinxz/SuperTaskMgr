#pragma once
// 滚动文件日志 + OutputDebugString（仅 Debug 构建）。
// 策略（架构第 9 节）：绝不记录命令行或窗口标题；不做每 tick 日志。
#include <string>
#include "core/Str.h"  // STM_LOG 宏会用到 Fmt

namespace stm {

enum class LogLevel { Debug, Info, Warn, Error };

// dir：例如 %LOCALAPPDATA%\SuperTaskMgr\logs；会创建目录；1MB 滚动，保留 3 份。
void LogInit(const std::wstring& dir);
void LogShutdown();

void LogWrite(LogLevel level, const char* module, const std::wstring& msg);
const char* LogLevelName(LogLevel level);

}  // namespace stm

#define STM_LOG(level, mod, ...) ::stm::LogWrite(level, mod, ::stm::Fmt(__VA_ARGS__))
#define STM_LOG_DEBUG(mod, ...) STM_LOG(::stm::LogLevel::Debug, mod, __VA_ARGS__)
#define STM_LOG_INFO(mod, ...) STM_LOG(::stm::LogLevel::Info, mod, __VA_ARGS__)
#define STM_LOG_WARN(mod, ...) STM_LOG(::stm::LogLevel::Warn, mod, __VA_ARGS__)
#define STM_LOG_ERROR(mod, ...) STM_LOG(::stm::LogLevel::Error, mod, __VA_ARGS__)
