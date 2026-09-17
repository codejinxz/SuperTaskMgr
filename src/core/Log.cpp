#include "core/Log.h"
#include "core/FsUtil.h"
#include "core/Str.h"
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <windows.h>

namespace stm {

namespace {

constexpr size_t kRollBytes = 1024 * 1024;
constexpr int kKeepFiles = 3;

std::mutex g_mu;
FILE* g_file = nullptr;
std::wstring g_path;

void DebugOutput(LogLevel level, const char* module, const std::wstring& line) {
#if defined(_DEBUG)
    if (level >= LogLevel::Info) {
        std::wstring w = Utf8ToWide(module);
        OutputDebugStringW((L"[stm] " + w + L": " + line + L"\n").c_str());
    }
#else
    (void)level; (void)module; (void)line;
#endif
}

void RollIfNeeded() {
    if (!g_file) return;
    const long pos = ftell(g_file);
    if (pos < static_cast<long>(kRollBytes)) return;
    fclose(g_file);
    g_file = nullptr;
    for (int i = kKeepFiles - 1; i >= 1; --i) {
        std::wstring dst = Fmt(L"{}.{}.log", g_path, i);
        std::wstring src = Fmt(L"{}.{}.log", g_path, i - 1);
        if (i == 1) src = g_path;
        MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    _wfopen_s(&g_file, g_path.c_str(), L"ab");
}

}  // namespace

void LogInit(const std::wstring& dir) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!dir.empty()) EnsureDir(dir);
    g_path = dir + L"\\stm.log";
    _wfopen_s(&g_file, g_path.c_str(), L"ab");
}

void LogShutdown() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_file) { fclose(g_file); g_file = nullptr; }
}

const char* LogLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

void LogWrite(LogLevel level, const char* module, const std::wstring& msg) {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const time_t t = system_clock::to_time_t(now);
    tm local{};
    localtime_s(&local, &t);

    std::wstring line = Fmt(L"[{:02d}:{:02d}:{:02d}.{:03d}] [{}] [{}] {}",
                            local.tm_hour, local.tm_min, local.tm_sec,
                            static_cast<int>(ms.count()), Utf8ToWide(LogLevelName(level)),
                            Utf8ToWide(module), msg);
    line += L"\r\n";
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_file) {
            const std::string u8 = WideToUtf8(line);
            fwrite(u8.data(), 1, u8.size(), g_file);
            RollIfNeeded();
        }
    }
    DebugOutput(level, module, msg);
}

}  // namespace stm
