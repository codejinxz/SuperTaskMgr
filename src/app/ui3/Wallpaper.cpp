// ui3/Wallpaper.cpp — Phase-6 custom wallpaper background (owner: H-B).
// This is the ONLY translation unit in the project that compiles stb_image
// (STB_IMAGE_IMPLEMENTATION lives here; do not define it anywhere else).
//
// Load pipeline (kept failure-safe for the previously active wallpaper):
//   ext check -> read whole file -> stbi_info size gate (4096x4096) -> decode RGBA8
//   -> create D3D11 texture+SRV -> write stored copy under
//   %LOCALAPPDATA%\SuperTaskMgr\wallpaper\wallpaper.<ext> -> commit (swap SRV+state).
// The stored copy is written only AFTER decode/texture succeeded, so a broken pick
// can never clobber the copy used for auto-restore at next launch.
//
// Headless note: STM_WALLPAPER_HEADLESS is defined ONLY by
// src/selftest/wallpaper_test.cpp, which includes this TU to unit-test the pure
// helpers without linking ImGui. It only removes the imgui.h dependency; the D3D
// code still compiles (interface vtable calls only, no d3d11.lib import) but is
// never executed headless.
#include "app/ui3/Wallpaper.h"

#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

#include "core/FsUtil.h"
#include "core/HandleGuard.h"
#include "core/Log.h"

#ifdef STM_WALLPAPER_HEADLESS
#else
#include "imgui.h"
#endif

// Vendored third-party stb_image (public domain). Reached relatively so this TU also
// compiles inside stm_selftest, which has no third_party include path. Warning level
// 0 around the vendored header: it is not /W4-clean and CMake gives it no /W0 target.
#pragma warning(push, 0)
#define STBI_NO_STDIO  // we decode from memory only
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STB_IMAGE_IMPLEMENTATION
#include "../../../third_party/stb/stb_image.h"
#pragma warning(pop)

namespace stm {
namespace ui {
namespace {

constexpr int kMaxDim = 4096;
constexpr long long kMaxPixels = static_cast<long long>(kMaxDim) * kMaxDim;
constexpr unsigned long long kMaxFileBytes = 512ull << 20;  // 512 MiB sanity gate
constexpr float kMaxMask = 0.85f;
const wchar_t* const kSupportedExts[] = {L"png", L"jpg", L"jpeg", L"bmp", L"tga"};

// UI-thread-only module state (all entry points are called from the frame loop).
WallpaperState g_state;
ID3D11ShaderResourceView* g_srv = nullptr;  // owned; released in WallpaperClear

std::wstring StoredDir() { return stm::EnsureDir(stm::LocalAppDataRoot() + L"\\wallpaper"); }

// Lowercased final extension ("D:\dir.v2\PIC.PNG" -> "png"); empty when the last dot
// belongs to a directory name or the name has no extension.
std::wstring LowerExt(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"/\\");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return {};
    if (slash != std::wstring::npos && dot < slash) return {};
    std::wstring ext = path.substr(dot + 1);
    for (wchar_t& c : ext) {
        c = (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
    }
    return ext;
}

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    UniqueHandle guard(static_cast<void*>(h));
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(guard.get(), &size)) return false;
    if (size.QuadPart <= 0 || static_cast<unsigned long long>(size.QuadPart) > kMaxFileBytes) {
        return false;
    }
    out->resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    if (!ReadFile(guard.get(), out->data(), static_cast<DWORD>(out->size()), &got, nullptr)) {
        return false;
    }
    return got == out->size();
}

bool WriteFileBytes(const std::wstring& path, const std::vector<uint8_t>& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    UniqueHandle guard(static_cast<void*>(h));
    DWORD written = 0;
    if (!WriteFile(guard.get(), data.data(), static_cast<DWORD>(data.size()), &written, nullptr)) {
        return false;
    }
    return written == data.size();
}

// Remove stored copies with the other extensions so exactly one wallpaper.* survives
// (result ignored: a stale extra copy only wastes a few MB).
void DeleteOtherStoredCopies(const std::wstring& keepExt) {
    const std::wstring dir = stm::LocalAppDataRoot() + L"\\wallpaper";
    for (const wchar_t* e : kSupportedExts) {
        if (keepExt == e) continue;
        DeleteFileW((dir + L"\\wallpaper." + e).c_str());
    }
}

// Upload an RGBA8 buffer as a DEFAULT-usage texture initialized from SUBRESOURCE_DATA
// and hand back a shader resource view (1 ref on the caller). Same shape as
// D3DRenderer::CreateTextureFromMemory, but takes the raw device because Wallpaper
// only receives void* (kept self-contained so the headless selftest build links).
ID3D11ShaderResourceView* CreateRgbaTextureSrv(ID3D11Device* device, int w, int h,
                                               const uint8_t* rgba) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(w);
    desc.Height = static_cast<UINT>(h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = rgba;
    init.SysMemPitch = static_cast<UINT>(w) * 4;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&desc, &init, tex.GetAddressOf()))) return nullptr;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf()))) {
        return nullptr;
    }
    return srv.Detach();
}

}  // namespace

float ClampMask(float mask) {
    if (!(mask > 0.0f)) return 0.0f;  // also folds NaN to 0
    if (mask > kMaxMask) return kMaxMask;
    return mask;
}

bool IsSupportedImageExt(const std::wstring& path) {
    const std::wstring ext = LowerExt(path);
    if (ext.empty()) return false;
    for (const wchar_t* e : kSupportedExts) {
        if (ext == e) return true;
    }
    return false;
}

bool WallpaperLoad(void* d3dDevice, void* d3dContext, const std::wstring& imagePath,
                   WallpaperState* out) {
    (void)d3dContext;  // DEFAULT-usage init via SUBRESOURCE_DATA needs no context
    if (out == nullptr) return false;
    out->error.clear();
    const auto fail = [out](const wchar_t* msg, const std::wstring& detail) {
        out->error = msg;
        STM_LOG_WARN("wallpaper", Fmt(L"加载失败：{}（{}）", detail, msg));
        return false;
    };

    auto* device = static_cast<ID3D11Device*>(d3dDevice);
    if (device == nullptr) return fail(L"内部错误：D3D 设备不可用", imagePath);

    const std::wstring ext = LowerExt(imagePath);
    if (!IsSupportedImageExt(imagePath)) {
        return fail(L"不支持的图片格式（支持 png/jpg/jpeg/bmp/tga）", imagePath);
    }

    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(imagePath, &bytes)) {
        return fail(L"无法读取图片文件（可能已被移动、删除或文件过大）", imagePath);
    }

    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp)) {
        return fail(L"无法识别的图片文件（可能已损坏）", imagePath);
    }
    if (w <= 0 || h <= 0 || static_cast<long long>(w) * static_cast<long long>(h) > kMaxPixels) {
        return fail(L"图片尺寸过大（上限 4096×4096），请选择较小的图片", imagePath);
    }

    int cw = 0, ch = 0, ccomp = 0;
    stbi_uc* rgba = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &cw, &ch,
                                          &ccomp, 4);
    if (rgba == nullptr) return fail(L"图片解码失败：文件可能已损坏或格式不受支持", imagePath);
    struct PixelFreer {
        stbi_uc* p;
        ~PixelFreer() { stbi_image_free(p); }
    } freer{rgba};

    ID3D11ShaderResourceView* srv = CreateRgbaTextureSrv(device, cw, ch, rgba);
    if (srv == nullptr) return fail(L"创建显卡纹理失败（显存不足或设备已丢失）", imagePath);

    // Persist the validated bytes; the previous stored copy stays intact until here.
    const std::wstring dir = StoredDir();
    std::wstring stored;
    if (!dir.empty()) {
        stored = dir + L"\\wallpaper." + ext;
        if (!WriteFileBytes(stored, bytes)) stored.clear();
    }
    if (stored.empty()) {
        srv->Release();
        return fail(L"保存壁纸副本失败：无法写入 %LOCALAPPDATA%\\SuperTaskMgr\\wallpaper",
                    imagePath);
    }
    DeleteOtherStoredCopies(ext);

    // Commit point: swap in the new SRV + state (old wallpaper untouched on failure).
    if (g_srv != nullptr) g_srv->Release();
    g_srv = srv;
    g_state = WallpaperState{};
    g_state.loaded = true;
    g_state.sourcePath = imagePath;
    g_state.storedPath = stored;
    g_state.width = cw;
    g_state.height = ch;
    *out = g_state;
    STM_LOG_INFO("wallpaper", Fmt(L"已加载壁纸 {}x{}，副本：{}", cw, ch, stored));
    return true;
}

void WallpaperClear() {
    if (g_srv != nullptr) {
        g_srv->Release();
        g_srv = nullptr;
    }
    const std::wstring dir = stm::LocalAppDataRoot() + L"\\wallpaper";
    for (const wchar_t* e : kSupportedExts) {
        DeleteFileW((dir + L"\\wallpaper." + e).c_str());
    }
    g_state = WallpaperState{};
    STM_LOG_INFO("wallpaper", L"壁纸已清除");
}

void WallpaperDrawBackground(float maskAlpha) {
#ifndef STM_WALLPAPER_HEADLESS
    if (g_srv == nullptr) return;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 maxPt(io.DisplaySize.x, io.DisplaySize.y);
    const ImVec2 zeroPt(0.0f, 0.0f);
    dl->AddImage(ImTextureRef(static_cast<void*>(g_srv)), zeroPt, maxPt);
    const float a = ClampMask(maskAlpha);
    if (a > 0.0f) {
        dl->AddRectFilled(zeroPt, maxPt,
                          IM_COL32(0, 0, 0, static_cast<int>(a * 255.0f + 0.5f)));
    }
#else
    (void)maskAlpha;  // headless build: nothing is ever drawn
#endif
}

bool WallpaperActive() { return g_state.loaded && g_srv != nullptr; }

const WallpaperState& WallpaperGet() { return g_state; }

bool WallpaperAutoRestore(void* d3dDevice, void* d3dContext) {
    const std::wstring dir = StoredDir();
    if (dir.empty()) return false;
    for (const wchar_t* e : kSupportedExts) {
        const std::wstring candidate = dir + L"\\wallpaper." + e;
        const DWORD attr = GetFileAttributesW(candidate.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        WallpaperState restored;
        if (WallpaperLoad(d3dDevice, d3dContext, candidate, &restored)) {
            // Restore keeps sourcePath == storedPath: the original pick is unknown.
            STM_LOG_INFO("wallpaper", Fmt(L"启动恢复壁纸：{}", candidate));
            return true;
        }
        STM_LOG_WARN("wallpaper", Fmt(L"启动恢复失败（{}）：{}", candidate, restored.error));
    }
    return false;
}

const wchar_t* WallpaperPerfNotice() {
    return L"⚠ 自定义壁纸会增加每帧纹理合成与显存开销（高分辨率图片更明显）；"
           L"监控工具建议保持默认纯色背景。";
}

}  // namespace ui
}  // namespace stm
