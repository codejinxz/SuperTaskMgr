#pragma once
// UI 侧文件元数据（描述/公司/版本），用 GetFileVersionInfoW 读取。
// 补足 ops 层 DetailsProvider 不携带的字段；供进程表描述列与
// 详情面板使用。仅限 UI 线程，按线程缓存。
// 注意：查询开销是一次小的磁盘读；UNC/网络路径一律拒绝，
// UI 帧绝不等网络。
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

// 原始查询；不可得时全部字段为空。绝不抛异常，绝不在网络上阻塞。
inline FileMeta QueryFileMeta(const std::wstring& path) {
    FileMeta meta;
    if (path.empty()) return meta;
    if (path.rfind(L"\\\\", 0) == 0) return meta;  // UNC 路径：拒绝（UI 帧预算）

    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), nullptr);
    if (size == 0 || size > 4u * 1024u * 1024u) return meta;
    std::string data(static_cast<size_t>(size), '\0');
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return meta;

    // 经文件自身的翻译表（首个条目）读取字符串。
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

// 有界的每线程缓存。条目节点稳定：Find() 返回的指针在 Store()
// 之间保持有效（只有 Prune 会失效，而调用方从不在任意插入
// 之间持有指针）。
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
        map_.clear();  // 简单的有界缓存替代；进程映像很少复用
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
