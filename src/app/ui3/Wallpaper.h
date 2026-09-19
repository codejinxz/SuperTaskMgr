#pragma once
// 第 6 阶段契约：自定义壁纸背景。归架构所有，已冻结。
// 实现经内嵌的 stb_image（公共领域）加载图像，作为 D3D11 纹理上传，
// 并每帧画在 ImGui 背景绘制列表上，
// 之后叠加一层深色可读性遮罩。
//
// 性能提示（UI 中亦有展示）：大壁纸会增加每帧纹理合成与显存开销；
// 监控 UI 默认使用纯深色背景。
#include <string>

namespace stm {
namespace ui {

struct WallpaperState {
    bool loaded = false;
    std::wstring sourcePath;   // 最初选择的路径
    std::wstring storedPath;   // %LOCALAPPDATA%\SuperTaskMgr\wallpaper 下的副本
    std::wstring error;        // 最近一次失败（中文，面向用户）
    int width = 0, height = 0;
};

// 从图像文件加载（或替换）壁纸（png/jpg/bmp/tga，经 stb_image）。
// 文件会先被复制到 %LOCALAPPDATA%\SuperTaskMgr\wallpaper\wallpaper.<ext>，
// 原件被移动/删除也不影响 UI。d3dDevice/d3dContext 是
// 以 void* 传入的 ID3D11Device*/ID3D11DeviceContext*，保持本头文件轻量。
// 失败时返回 false 并设置 out->error（原壁纸保持生效）。
bool WallpaperLoad(void* d3dDevice, void* d3dContext, const std::wstring& imagePath,
                   WallpaperState* out);

// 移除壁纸（释放纹理、删除存档副本）。无壁纸时安全。
void WallpaperClear();

// R-Fix Bug2：拆除时释放但保留持久化副本。释放 GPU 纹理并复位
// 运行时状态，但不删除 %LOCALAPPDATA% 下的 wallpaper.*——
// 存档副本正是 WallpaperAutoRestore 在下次启动时重载的来源。
// next launch (deleting it at exit made "下次启动自动恢复" dead code). Use
// WallpaperClear for the user-facing「关闭壁纸」action, which must delete.
void WallpaperShutdown();

// 画在 ImGui::GetBackgroundDrawList()：全屏图像 + 黑色可读性遮罩
//（maskAlpha 0..0.85）。未加载壁纸时空操作。每帧调用一次，
// 在渲染器 BeginFrame 之后、ImGui::NewFrame 内容窗口之前。
void WallpaperDrawBackground(float maskAlpha);

bool WallpaperActive();
const WallpaperState& WallpaperGet();

// 纯辅助（可单元测试）：可读性遮罩钳制 [0, 0.85]；扩展名检查。
float ClampMask(float mask);
bool IsSupportedImageExt(const std::wstring& path);

// ---- 第 6 阶段附录（下方为 H-B 实现；上方为冻结声明）----

// 启动恢复：%LOCALAPPDATA%\SuperTaskMgr\wallpaper\wallpaper.<ext>
// 存在时重载存档副本（在 D3D/ImGui 初始化之后、首帧之前调用一次；
// 外观 UI 也可在 WallpaperClear/Load 接线后立即调用）。
// 成功恢复壁纸时返回 true；恢复后 state.sourcePath 等于
// state.storedPath（彼时原始选择路径未知）。
// 无存档副本时静默空操作。
bool WallpaperAutoRestore(void* d3dDevice, void* d3dContext);

// UI 中壁纸选择器旁展示的面向用户的性能提示。
const wchar_t* WallpaperPerfNotice();

}  // namespace ui
}  // namespace stm
