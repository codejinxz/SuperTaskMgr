#pragma once
// Phase-6 contract: custom wallpaper background. Architect-owned, frozen.
// Implementation loads an image via vendored stb_image (public domain), uploads it
// as a D3D11 texture, and draws it on the ImGui background draw list each frame,
// followed by a dark readability mask.
//
// PERFORMANCE NOTICE (shown in UI too): a large wallpaper adds per-frame texture
// composition and VRAM cost; the monitoring UI defaults to a flat dark background.
#include <string>

namespace stm {
namespace ui {

struct WallpaperState {
    bool loaded = false;
    std::wstring sourcePath;   // original picked path
    std::wstring storedPath;   // copy under %LOCALAPPDATA%\SuperTaskMgr\wallpaper
    std::wstring error;        // last failure (Chinese, user-facing)
    int width = 0, height = 0;
};

// Load (or replace) the wallpaper from an image file (png/jpg/bmp/tga via stb_image).
// The file is COPIED to %LOCALAPPDATA%\SuperTaskMgr\wallpaper\wallpaper.<ext> first so
// the UI survives the original being moved/deleted. d3dDevice/d3dContext are
// ID3D11Device*/ID3D11DeviceContext* passed as void* to keep this header light.
// Returns false with out->error set on failure (previous wallpaper stays active).
bool WallpaperLoad(void* d3dDevice, void* d3dContext, const std::wstring& imagePath,
                   WallpaperState* out);

// Remove the wallpaper (releases texture, deletes the stored copy). Safe if none.
void WallpaperClear();

// Draw on ImGui::GetBackgroundDrawList(): fullscreen image + black readability mask
// (maskAlpha 0..0.85). No-op when no wallpaper is loaded. Call once per frame,
// after renderer BeginFrame and before ImGui::NewFrame content windows.
void WallpaperDrawBackground(float maskAlpha);

bool WallpaperActive();
const WallpaperState& WallpaperGet();

// Pure helpers (unit-testable): readability mask clamp [0, 0.85]; extension check.
float ClampMask(float mask);
bool IsSupportedImageExt(const std::wstring& path);

// ---- Phase-6 addendum (H-B implementation below; frozen declarations end above) ----

// Startup restore: reload the stored copy under
// %LOCALAPPDATA%\SuperTaskMgr\wallpaper\wallpaper.<ext> when one exists (call once
// after D3D/ImGui init, before the first frame; the appearance UI may also call it
// right after WallpaperClear/Load wiring). Returns true when a wallpaper was
// restored; on restore, state.sourcePath equals state.storedPath (original pick
// path is unknown at that point). Quiet no-op when no copy exists.
bool WallpaperAutoRestore(void* d3dDevice, void* d3dContext);

// User-facing performance hint shown next to the wallpaper picker in the UI.
const wchar_t* WallpaperPerfNotice();

}  // namespace ui
}  // namespace stm
