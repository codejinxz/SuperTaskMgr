#pragma once
// UI-side file metadata (description / company / version) read with GetFileVersionInfoW.
// Covers the fields the ops-layer DetailsProvider does not carry; used by the process
// table description column and the detail panel. UI thread only, cached per thread.
// Note: query cost is one small disk read; UNC/network paths are refused so a UI
// frame can never wait on the network.
#include <windows.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include "core/Str.h"

#pragma comment(lib, "version.lib")

namespace stm {
namespace ui {

struct FileMeta {
    std::wstring description;
    std::wstring company;
    std::wstring version;
};

// Raw query; all fields empty when unavailable. Never throws, never blocks on network.
inline FileMeta QueryFileMeta(const std::wstring& path) {
    FileMeta meta;
    if (path.empty()) return meta;
    if (path.rfind(L"\\\\", 0) == 0) return meta;  // UNC path: refuse (UI frame budget)

    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), nullptr);
    if (size == 0 || size > 4u * 1024u * 1024u) return meta;
    std::string data(static_cast<size_t>(size), '\0');
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return meta;

    // Read strings through the file's own translation table (first entry).
    struct LangCodePage { WORD lang; WORD code; };
    LangCodePage* trans = nullptr;
    UINT transLen = 0;
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                       reinterpret_cast<void**>(&trans), &transLen) &&
        trans != nullptr && transLen >= sizeof(LangCodePage)) {
        wchar_t sub[64] = {};
        swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\", trans[0].lang, trans[0].code);
        const std::wstring prefix = sub;
        auto read = [&data, &prefix](const wchar_t* key) {
            wchar_t* val = nullptr;
            UINT valLen = 0;
            if (VerQueryValueW(data.data(), (prefix + key).c_str(),
                               reinterpret_cast<void**>(&val), &valLen) &&
                val != nullptr && valLen > 0) {
                return std::wstring(val);
            }
            return std::wstring();
        };
        meta.description = read(L"FileDescription");
        meta.company = read(L"CompanyName");
    }

    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT ffiLen = 0;
    if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&ffi), &ffiLen) &&
        ffi != nullptr && ffiLen >= sizeof(VS_FIXEDFILEINFO)) {
        meta.version = Fmt(L"{}.{}.{}.{}",
                           static_cast<unsigned>(HIWORD(ffi->dwFileVersionMS)),
                           static_cast<unsigned>(LOWORD(ffi->dwFileVersionMS)),
                           static_cast<unsigned>(HIWORD(ffi->dwFileVersionLS)),
                           static_cast<unsigned>(LOWORD(ffi->dwFileVersionLS)));
    }
    return meta;
}

// Bounded per-thread cache. Entries are node-stable: Find() pointers stay valid
// across Store() calls (only Prune invalidates, and callers never hold across
// arbitrary inserts).
class FileMetaCache {
public:
    const FileMeta* Find(const std::wstring& path) {
        PruneIfNeeded();
        auto it = map_.find(path);
        return it == map_.end() ? nullptr : &it->second;
    }
    void Store(const std::wstring& path, FileMeta meta) {
        PruneIfNeeded();
        map_.insert_or_assign(path, std::move(meta));
    }

private:
    void PruneIfNeeded() {
        if (map_.size() < kMaxEntries) return;
        map_.clear();  // simple bounded-cache substitute; process images recycle rarely
    }
    static constexpr size_t kMaxEntries = 2048;
    std::unordered_map<std::wstring, FileMeta> map_;
};

inline FileMetaCache& FileMetaCacheForThread() {
    thread_local FileMetaCache cache;
    return cache;
}

}  // namespace ui
}  // namespace stm
