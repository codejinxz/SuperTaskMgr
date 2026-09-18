// Pure-logic tests for ui3/Wallpaper: mask clamp, extension check, state defaults.
// No D3D, no ImGui (texture upload / draw / load paths are verified by the --smoke
// integration run instead). stm_selftest does not link app objects, so this TU
// includes the implementation directly in "headless" mode: the imgui.h reference is
// compiled out and the remaining D3D calls are plain interface vtable calls, so the
// only link dependency added is stm_core (Log/FsUtil). This way the tests exercise
// the REAL ClampMask / IsSupportedImageExt implementations, not copies.
#define STM_WALLPAPER_HEADLESS 1
#include "../app/ui3/Wallpaper.cpp"

#include <limits>

#include "selftest/TestFramework.h"

// Readability mask: clamp into [0, 0.85]; out-of-range and NaN fold safely.
STM_TEST(wallpaper_mask_clamp) {
    if (stm::ui::ClampMask(0.0f) != 0.0f || stm::ui::ClampMask(-0.25f) != 0.0f ||
        stm::ui::ClampMask(-1.0e9f) != 0.0f) {
        *err = L"ClampMask 下界应为 0（负值截断）";
        return false;
    }
    if (stm::ui::ClampMask(0.85f) != 0.85f || stm::ui::ClampMask(0.3f) != 0.3f) {
        *err = L"ClampMask 应保留 [0, 0.85] 区间内的值";
        return false;
    }
    if (stm::ui::ClampMask(0.86f) != 0.85f || stm::ui::ClampMask(1.0f) != 0.85f ||
        stm::ui::ClampMask(1.0e9f) != 0.85f) {
        *err = L"ClampMask 上界应为 0.85（越界截断）";
        return false;
    }
    const float nan = std::numeric_limits<float>::quiet_NaN();
    if (!(stm::ui::ClampMask(nan) == 0.0f)) {
        *err = L"ClampMask(NaN) 应折叠为 0";
        return false;
    }
    return true;
}

// Extension gate: case-insensitive, needs a real final extension; dots inside
// directory names or Chinese paths must not confuse it.
STM_TEST(wallpaper_ext_check) {
    struct Case {
        const wchar_t* path;
        bool want;
    };
    const Case cases[] = {
        {L"D:\\pics\\wall.png", true},            {L"D:\\pics\\wall.PNG", true},
        {L"D:\\pics\\photo.JPG", true},           {L"D:\\pics\\photo.jpeg", true},
        {L"D:\\pics\\shot.BMP", true},            {L"D:\\pics\\tex.TGA", true},
        {L"D:\\dir.v2\\pic.png", true},           {L"D:\\截图\\壁纸.JPG", true},
        {L"壁纸.png", true},                      {L"D:\\pics\\wall", false},
        {L"D:\\截图\\壁纸", false},               {L"D:\\pics\\wall.", false},
        {L"D:\\pics\\wall.txt", false},           {L"D:\\pics\\wall.jpeg7", false},
        {L"D:\\dir.png\\readme", false},          {L"D:\\dir.png\\readme.doc", false},
        {L"", false},                             {L".png", true},
    };
    for (const Case& c : cases) {
        if (stm::ui::IsSupportedImageExt(c.path) != c.want) {
            *err = stm::Fmt(L"IsSupportedImageExt({}) 应为 {}", c.path, c.want ? L"true" : L"false");
            return false;
        }
    }
    return true;
}

// Documented defaults of the public state struct (and the fresh global module state).
STM_TEST(wallpaper_state_defaults) {
    const stm::ui::WallpaperState s;
    if (s.loaded || s.width != 0 || s.height != 0) {
        *err = L"WallpaperState 默认值应为 未加载 / 0x0";
        return false;
    }
    if (!s.sourcePath.empty() || !s.storedPath.empty() || !s.error.empty()) {
        *err = L"WallpaperState 默认路径与 error 应为空";
        return false;
    }
    if (stm::ui::WallpaperActive()) {
        *err = L"未加载任何壁纸时 WallpaperActive() 应为 false";
        return false;
    }
    return true;
}
