// WinVerifyTrust 包装（只用有文档的 wintrust/mscat 接口）。
//
// 协议要点（ops/Signature.h）：
//  - 每次 VERIFY 调用都与对同一 WINTRUST_DATA 的 WTD_STATEACTION_CLOSE
//    调用配对（有文档要求；否则信任状态句柄泄漏）；
//  - 先查内嵌签名（WINTRUST_ACTION_GENERIC_VERIFY_V2 / WTD_CHOICE_FILE）；
//    映像无内嵌签名（TRUST_E_NOSIGNATURE）时改走目录路径
//   （WTD_CHOICE_CATALOG + CryptCATAdmin* API），因为 OS 映像都是目录签名；
//  - 一律 WTD_UI_NONE，吊销保持 WTD_REVOKE_NONE（不产生网络停顿）；
//  - 结果按小写路径缓存，超过 512 条即清空。
#include "ops/Signature.h"
#include "core/HandleGuard.h"
#include <windows.h>
#include <initguid.h>
#include <softpub.h>
#include <wintrust.h>
#include <mscat.h>
#include <algorithm>
#include <cwctype>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace stm::ops {
namespace {

// 一轮 VERIFY 后在同一 WINTRUST_DATA 上执行强制的 CLOSE 轮。
LONG TrustVerifyClose(DWORD unionChoice, void* info, GUID* action) {
    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = unionChoice;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.hWVTStateData = nullptr;
    if (unionChoice == WTD_CHOICE_FILE) {
        wd.pFile = static_cast<WINTRUST_FILE_INFO_*>(info);
    } else if (unionChoice == WTD_CHOICE_CATALOG) {
        wd.pCatalog = static_cast<WINTRUST_CATALOG_INFO_*>(info);
    }
    const LONG result = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), action, &wd);
    wd.dwStateAction = WTD_STATEACTION_CLOSE;  // 必需配对（状态清理）
    (void)WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), action, &wd);
    return result;
}

LONG VerifyEmbedded(const std::wstring& path) {
    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = path.c_str();
    fileInfo.hFile = nullptr;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    return TrustVerifyClose(WTD_CHOICE_FILE, &fileInfo, &action);
}

// 对无内嵌签名映像的目录验证（典型：OS 文件）。
// `subsystem` 选择目录库/信任提供程序；WinVerifyTrust 动作使用
// 同一 GUID，因为在某些系统上目录选择与它策略绑定。
// 子系统不可用或没有目录覆盖该文件时返回 TRUST_E_NOSIGNATURE。
//
LONG VerifyCatalogWithSubsystem(const std::wstring& path, const GUID& subsystem) {
    const size_t namePos = path.find_last_of(L"\\/");
    const std::wstring memberName = namePos == std::wstring::npos ? path : path.substr(namePos + 1);

    GUID action = subsystem;
    HCATADMIN catAdmin = nullptr;
    if (!CryptCATAdminAcquireContext(&catAdmin, &action, 0)) return TRUST_E_NOSIGNATURE;
    LONG verdict = TRUST_E_NOSIGNATURE;
    HANDLE rawFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (rawFile != INVALID_HANDLE_VALUE) {
        const UniqueHandle file(rawFile);
        DWORD hashLen = 0;
        if (CryptCATAdminCalcHashFromFileHandle(file.get(), &hashLen, nullptr, 0) && hashLen > 0) {
            std::vector<BYTE> hash(hashLen);
            if (CryptCATAdminCalcHashFromFileHandle(file.get(), &hashLen, hash.data(), 0)) {
                HCATINFO prevCat = nullptr;
                HCATINFO catInfo = nullptr;
                while ((catInfo = CryptCATAdminEnumCatalogFromHash(catAdmin, hash.data(), hashLen, 0,
                                                                   &prevCat)) != nullptr) {
                    CATALOG_INFO meta{};
                    meta.cbStruct = sizeof(meta);
                    std::wstring catalogFile;
                    if (CryptCATCatalogInfoFromContext(catInfo, &meta, 0)) {
                        catalogFile = meta.wszCatalogFile;
                    }
                    WINTRUST_CATALOG_INFO catalogInfo{};
                    catalogInfo.cbStruct = sizeof(catalogInfo);
                    catalogInfo.pcwszCatalogFilePath = catalogFile.c_str();
                    catalogInfo.pcwszMemberTag = memberName.c_str();
                    catalogInfo.pcwszMemberFilePath = path.c_str();
                    catalogInfo.hMemberFile = file.get();
                    catalogInfo.pbCalculatedFileHash = hash.data();
                    catalogInfo.cbCalculatedFileHash = hashLen;
                    catalogInfo.hCatAdmin = catAdmin;
                    verdict = TrustVerifyClose(WTD_CHOICE_CATALOG, &catalogInfo, &action);
                    CryptCATAdminReleaseCatalogContext(catAdmin, catInfo, 0);
                    break;  // 第一个含有该哈希的目录即定论
                }
            }
        }
    }
    CryptCATAdminReleaseContext(catAdmin, 0);
    return verdict;
}

// 带子系统兜底的目录验证。加固系统有时直接拒绝通用
// 目录子系统（Win11 22631 实测：CryptCATAdminAcquireContext 报
// ERROR_ACCESS_DENIED、WinVerifyTrust 报 TRUST_E_PROVIDER_UNKNOWN），而
// 目录库历史上属于驱动信任提供程序，因此在通用子系统
// 不可用或结论不明时以 DRIVER_ACTION_VERIFY 重试。
LONG VerifyCatalog(const std::wstring& path) {
    LONG verdict = VerifyCatalogWithSubsystem(path, WINTRUST_ACTION_GENERIC_VERIFY_V2);
    if (verdict == TRUST_E_NOSIGNATURE || verdict == TRUST_E_PROVIDER_UNKNOWN) {
        verdict = VerifyCatalogWithSubsystem(path, DRIVER_ACTION_VERIFY);
    }
    return verdict;
}

SigState MapVerdict(LONG verifyResult) {
    if (verifyResult == 0) return SigState::Valid;  // S_OK（成功）
    if (verifyResult == TRUST_E_NOSIGNATURE) return SigState::Unsigned;
    return SigState::Invalid;  // 不受信根、坏哈希、策略失败等
}

std::wstring LowerCopy(const std::wstring& s) {
    std::wstring out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return out;
}

}  // namespace

SigState VerifyFileSignature(const std::wstring& path) {
    if (path.empty()) return SigState::NoCheck;

    static std::mutex mu;
    static std::unordered_map<std::wstring, SigState> cache;
    const std::wstring cacheKey = LowerCopy(path);
    {
        std::lock_guard<std::mutex> lock(mu);
        if (const auto it = cache.find(cacheKey); it != cache.end()) return it->second;
    }

    SigState state = SigState::NoCheck;
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        const LONG embedded = VerifyEmbedded(path);
        if (embedded == 0) {
            state = SigState::Valid;
        } else if (embedded == TRUST_E_NOSIGNATURE) {
            state = MapVerdict(VerifyCatalog(path));
        } else {
            state = SigState::Invalid;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mu);
        cache[cacheKey] = state;
        if (cache.size() > 512) cache.clear();  // 有界，按契约
    }
    return state;
}

}  // namespace stm::ops
