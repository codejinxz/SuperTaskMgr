#include "core/FsUtil.h"
#include <windows.h>
#include <shlobj.h>
#include <vector>

namespace stm {

std::wstring ExePath() {
    // 对长路径友好：用可增长的缓冲区查询，而不是固定 MAX_PATH。
    std::vector<wchar_t> buf(1024);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size() - 1) return std::wstring(buf.data(), n);
        if (buf.size() >= 32768) return std::wstring(buf.data(), n);  // 达到上限即放弃
        buf.resize(buf.size() * 2);
    }
}

std::wstring ExeDir() {
    std::wstring p = ExePath();
    const size_t slash = p.find_last_of(L'\\');
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

std::wstring EnsureDir(const std::wstring& path) {
    if (path.empty()) return path;
    std::wstring cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur.push_back(path[i]);
        if (path[i] == L'\\' && i > 2) {  // 跳过 "C:\" 前缀
            CreateDirectoryW(cur.substr(0, i).c_str(), nullptr);
        }
    }
    CreateDirectoryW(path.c_str(), nullptr);
    const DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) ? path : std::wstring();
}

std::wstring LocalAppDataRoot() {
    static std::wstring cached = [] {
        PWSTR raw = nullptr;
        std::wstring base;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw))) {
            base = raw;
            CoTaskMemFree(raw);
        }
        return EnsureDir(base + L"\\SuperTaskMgr");
    }();
    return cached;
}

std::wstring LogDir() { return EnsureDir(LocalAppDataRoot() + L"\\logs"); }
std::wstring ConfigPath() { return LocalAppDataRoot() + L"\\config.json"; }
std::wstring SessionPath() { return LocalAppDataRoot() + L"\\session.json"; }

}  // namespace stm
