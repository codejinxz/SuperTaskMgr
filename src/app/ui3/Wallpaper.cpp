// ui3/Wallpaper.cpp — 第 6 阶段自定义壁纸背景（负责人：H-B）。
// 本项目只有本编译单元编译 stb_image
//（STB_IMAGE_IMPLEMENTATION 在此定义；切勿在其他地方再定义）。
//
// 加载管线（对先前生效的壁纸保持失败安全）：
//   扩展名检查 -> 读取整个文件 -> stbi_info 尺寸闸门（4096x4096）-> 解码 RGBA8
//   -> 创建 D3D11 纹理+SRV -> 在
//   %LOCALAPPDATA%\SuperTaskMgr\wallpaper\wallpaper.<ext> 写存档副本 -> 提交（交换 SRV+状态）。
// 存档副本只在解码/纹理成功之后写入，因此坏选择绝不可能
// 破坏下次启动自动恢复所用的副本。
//
// 无头说明：STM_WALLPAPER_HEADLESS 只由
// src/selftest/wallpaper_test.cpp 定义，它包含本编译单元以在
// 不链接 ImGui 的情况下单元测试纯辅助函数。它只移除 imgui.h 依赖；D3D
// 代码仍然编译（仅接口虚表调用，不导入 d3d11.lib）但
// 无头模式下从不执行。
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

// 内嵌第三方 stb_image（公共领域）。以相对路径引用，使本编译单元也能
// 在没有 third_party 包含路径的 stm_selftest 中编译。内嵌头周围
// 警告等级设 0：它不是 /W4 干净的，CMake 也没有给它 /W0 目标。
#pragma warning(push, 0)
#define STBI_NO_STDIO  // 只从内存解码
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
constexpr unsigned long long kMaxFileBytes = 512ull << 20;  // 512 MiB 合理性闸门
constexpr float kMaxMask = 0.85f;
const wchar_t* const kSupportedExts[] = {L"png", L"jpg", L"jpeg", L"bmp", L"tga"};

// 仅限 UI 线程的模块状态（所有入口都由帧循环调用）。
WallpaperState g_state;
ID3D11ShaderResourceView* g_srv = nullptr;  // 持有；在 WallpaperClear 中释放

std::wstring StoredDir() { return stm::EnsureDir(stm::LocalAppDataRoot() + L"\\wallpaper"); }

// 小写的最终扩展名（"D:\dir.v2\PIC.PNG" -> "png"）；当最后一个点
// 属于目录名或名称没有扩展名时为空。
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

// 删除其他扩展名的存档副本，确保只存活一份 wallpaper.*
//（忽略结果：过期的多余副本只浪费几 MB）。
void DeleteOtherStoredCopies(const std::wstring& keepExt) {
    const std::wstring dir = stm::LocalAppDataRoot() + L"\\wallpaper";
    for (const wchar_t* e : kSupportedExts) {
        if (keepExt == e) continue;
        DeleteFileW((dir + L"\\wallpaper." + e).c_str());
    }
}

// 把 RGBA8 缓冲上传为由 SUBRESOURCE_DATA 初始化的 DEFAULT 用法纹理，
// 并交还着色器资源视图（调用方持 1 引用）。形状与
// D3DRenderer::CreateTextureFromMemory 相同，但接收原始设备，因为 Wallpaper
// 只收到 void*（保持自包含，无头 selftest 构建才能链接）。
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
    if (!(mask > 0.0f)) return 0.0f;  // 同时把 NaN 归为 0
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
    (void)d3dContext;  // 经 SUBRESOURCE_DATA 的 DEFAULT 用法初始化不需要上下文
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

    // 持久化已校验的字节；此前旧存档副本一直完好至此。
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

    // 提交点：换入新 SRV + 状态（失败时旧壁纸分毫不动）。
    if (g_srv != nullptr) g_srv->Release();
    g_srv = srv;
    g_state = WallpaperState{};
    g_state.loaded = true;
    g_state.sourcePath = imagePath;
    g_state.storedPath = stored;
    g_state.width = cw;
    g_state.height = ch;
    *out = g_state;
    // R-Fix Bug2 诊断日志：SRV 指针 + 绑定方式。本仓库 vendored 的
    // imgui 1.92.9 DX11 后端（third_party/imgui/backends/imgui_impl_dx11.cpp
    // 渲染循环 `pcmd->GetTexID()` -> PSSetShaderResources）没有托管纹理注册表
    // （无 ImGui_ImplDX11_RegisterTexture，ImTextureRef 无 TexTag），裸 SRV
    // 指针构造 ImTextureRef 即为最终绑定句柄 —— 无需也不存在注册步骤。
    STM_LOG_INFO("wallpaper", Fmt(L"已加载壁纸 {}x{}，副本：{}", cw, ch, stored));
    STM_LOG_INFO("wallpaper",
                 Fmt(L"SRV=0x{:X}（裸指针直绑 DX11 后端，无需注册）",
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(srv))));
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

void WallpaperShutdown() {
    // R-Fix Bug2：退出路径只释放 GPU 纹理；持久化副本保留，
    // 供 WallpaperAutoRestore 下次启动重载（WallpaperClear，
    // the「关闭壁纸」action, is the one that deletes).
    if (g_srv != nullptr) {
        g_srv->Release();
        g_srv = nullptr;
    }
    g_state = WallpaperState{};
    STM_LOG_INFO("wallpaper", L"退出释放壁纸纹理（保留持久化副本供下次启动恢复）");
}

void WallpaperDrawBackground(float maskAlpha) {
#ifndef STM_WALLPAPER_HEADLESS
    if (g_srv == nullptr) return;
    // R-Fix Bug2 契约：本函数绘制进视口背景绘制列表（1.92 单视口：无参重载 ==
    // Viewports[0]），位于一切普通窗口之下 —— 壁纸能否可见取决于外壳
    // （ui/Pages.cpp DrawShell）在 WallpaperActive() 时推透明 WindowBg/ChildBg。
    // 必须在 ImGui::NewFrame() 之后调用（本帧内被追加才会并入 DrawData，
    // imgui 以 g.Time 戳判定；V18 探针实证）。
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
    (void)maskAlpha;  // 无头构建：什么都不绘制
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
            // 恢复时 sourcePath == storedPath：原始选择路径未知。
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
