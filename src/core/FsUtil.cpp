#include "core/FsUtil.h"
#include <windows.h>
#include <shlobj.h>

namespace stm {

std::wstring ExePath() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
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
        if (path[i] == L'\\' && i > 2) {  // skip "C:\" prefix
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
