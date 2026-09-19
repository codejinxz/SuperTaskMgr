// ui3/Wallpaper 纯逻辑测试：遮罩钳制、扩展名检查、状态默认值。
// 无 D3D、无 ImGui（纹理上传/绘制/加载路径改由 --smoke
// 集成运行验证）。stm_selftest 不链接应用对象，因此本编译单元
// 以"无头"模式直接包含实现：imgui.h 引用被编译排除，
// 剩余 D3D 调用只是普通接口虚表调用，因此新增的
// 链接依赖只有 stm_core（Log/FsUtil）。这样测试执行的是
// 真实的 ClampMask / IsSupportedImageExt 实现，而非副本。
#define STM_WALLPAPER_HEADLESS 1
#include "../app/ui3/Wallpaper.cpp"

#include <limits>

#include "selftest/TestFramework.h"

// 可读性遮罩：钳制进 [0, 0.85]；越界与 NaN 安全归并。
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

// 扩展名闸门：大小写不敏感，必须有真实的最终扩展名；
// 目录名中的点或中文路径不得使其混淆。
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

// 公共状态结构体（以及全新的全局模块状态）的文档化默认值。
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
