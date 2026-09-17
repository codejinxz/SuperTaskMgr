#pragma once
// Rolling file logger + OutputDebugString (debug builds only).
// Policy (arch section 9): never log command lines or window titles; no per-tick logging.
#include <string>
#include "core/Str.h"  // Fmt used by the STM_LOG macros

namespace stm {

enum class LogLevel { Debug, Info, Warn, Error };

// dir: e.g. %LOCALAPPDATA%\SuperTaskMgr\logs ; creates dir; rolls at 1MB, keeps 3.
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
