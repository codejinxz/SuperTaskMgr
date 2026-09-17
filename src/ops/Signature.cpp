// WinVerifyTrust wrapper (documented wintrust/mscat surfaces only).
//
// Protocol notes (ops/Signature.h):
//  - every VERIFY call is paired with a WTD_STATEACTION_CLOSE call on the same
//    WINTRUST_DATA (documented requirement; otherwise the trust-state handle leaks);
//  - embedded-signature check first (WINTRUST_ACTION_GENERIC_VERIFY_V2 / WTD_CHOICE_FILE);
//    when the image has no embedded signature (TRUST_E_NOSIGNATURE) the catalog path is
//    tried (WTD_CHOICE_CATALOG + CryptCATAdmin* APIs) because OS images are catalog-signed;
//  - WTD_UI_NONE everywhere, revocation left at WTD_REVOKE_NONE (no network stalls);
//  - results cached per lowercased path, cache cleared when it exceeds 512 entries.
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

// One VERIFY pass followed by the mandatory CLOSE pass on the same WINTRUST_DATA.
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
    wd.dwStateAction = WTD_STATEACTION_CLOSE;  // required pairing (state cleanup)
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

// Catalog verification for images without an embedded signature (typical: OS files).
// `subsystem` selects the catalog store/trust provider; the WinVerifyTrust action uses
// the same GUID because the catalog choice is policy-bound to it on some systems.
// Returns TRUST_E_NOSIGNATURE when the subsystem is unavailable or no catalog covers
// the file.
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
                    break;  // first catalog that carries the hash decides
                }
            }
        }
    }
    CryptCATAdminReleaseContext(catAdmin, 0);
    return verdict;
}

// Catalog verification with subsystem fallback. Hardened systems sometimes deny the
// generic catalog subsystem outright (observed on Win11 22631: ERROR_ACCESS_DENIED on
// CryptCATAdminAcquireContext and TRUST_E_PROVIDER_UNKNOWN on WinVerifyTrust), while the
// catalog store historically belongs to the driver trust provider, so DRIVER_ACTION_VERIFY
// is retried when the generic one is unavailable or inconclusive.
LONG VerifyCatalog(const std::wstring& path) {
    LONG verdict = VerifyCatalogWithSubsystem(path, WINTRUST_ACTION_GENERIC_VERIFY_V2);
    if (verdict == TRUST_E_NOSIGNATURE || verdict == TRUST_E_PROVIDER_UNKNOWN) {
        verdict = VerifyCatalogWithSubsystem(path, DRIVER_ACTION_VERIFY);
    }
    return verdict;
}

SigState MapVerdict(LONG verifyResult) {
    if (verifyResult == 0) return SigState::Valid;  // S_OK
    if (verifyResult == TRUST_E_NOSIGNATURE) return SigState::Unsigned;
    return SigState::Invalid;  // untrusted root, bad hash, policy failures, ...
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
        if (cache.size() > 512) cache.clear();  // bounded, per contract
    }
    return state;
}

}  // namespace stm::ops
