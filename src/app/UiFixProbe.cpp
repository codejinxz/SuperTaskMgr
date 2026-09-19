// ============================================================================
//  R-Fix 探针二进制（Bug1 布局纯函数自测 + Bug2 壁纸 GPU 回读探针）。
//  不随应用发布：除非定义 STM_UIFIX_PROBE，整个编译单元都被编译排除。
//  常规 SuperTaskMgr 构建（CMake，无定义）看到的是空编译单元；
//  私有构建（build_rf）额外把本编译单元编译成独立的 stm_uifix_probe.exe：
//
//    cl /DSTM_UIFIX_PROBE UiFixProbe.cpp ui3/Wallpaper.cpp core/{FsUtil,Log,Str}.cpp
//       imgui*.cpp backends/imgui_impl_dx11.cpp /link d3d11.lib dxgi.lib ...
//
//  模式：
//    --mode layout     纯 HeaderLayout.h 用例：对真实摆位（工具栏右侧 =
//                      仅「?」；状态栏右侧 = [热键][徽标][帧耗时]）
//                      做多宽度不重叠断言，
//                      覆盖任意宽度 >= 600、按优先级单调隐藏、钳制。
//    --mode wallpaper  无头 D3D11 + imgui 1.92.9 渲染：NewFrame ->
//                      WallpaperDrawBackground -> 不透明与透明外壳窗口对比 ->
//                      RenderDrawData -> GPU 回读像素计数。
//                      证明真正的根因（不透明的 ##approot WindowBg
//                      盖住了背景绘制列表图像）与修复效果。
// ============================================================================
#ifdef STM_UIFIX_PROBE

#include "app/ui/HeaderLayout.h"
#include "app/ui3/Wallpaper.h"
#include "core/FsUtil.h"
#include "core/Log.h"
#include "core/Str.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const std::wstring& what) {
    if (!ok) {
        ++g_failures;
        wprintf(L"[FAIL] %s\n", what.c_str());
    }
}

// ---------------------------------------------------------------------------
// --mode layout：HeaderLayout.h 纯函数用例。
// ---------------------------------------------------------------------------

// 对已摆位布局做一次完整不变式清扫。
void AssertInvariants(const wchar_t* tag, float contentW, float leftEnd, float spacing,
                      const stm::ui::HeaderItem* items, const stm::ui::HeaderPlacement* out,
                      int count) {
    for (int i = 0; i < count; ++i) {
        if (!out[i].visible) continue;
        // 1) 绝不遮盖左侧流
        Check(out[i].x >= leftEnd - 0.001f,
              std::wstring(tag) + L" 项" + std::to_wstring(i) +
                  L" 覆盖左侧流程: x=" + std::to_wstring(out[i].x) + L" leftEnd=" +
                  std::to_wstring(leftEnd));
        // 2) 保持在内容区内——除了退化的被钳制 priority-0 兜底
        //   （窗口窄于 leftEnd+width 时无法两全）
        Check(out[i].x + items[i].width <= contentW + 0.001f || out[i].x == leftEnd,
              std::wstring(tag) + L" 项" + std::to_wstring(i) + L" 超出右缘");
        // 3) 优先级可见性单调：更高优先级（更小数字）的条目可见时，
        //    本条目必须可见。
        for (int j = 0; j < count; ++j) {
            if (i == j) continue;
            if (items[j].priority < items[i].priority) {
                Check(out[j].visible,
                      std::wstring(tag) + L" 优先级单调性破坏: 项" + std::to_wstring(i) +
                          L" 可见但更高优先级项" + std::to_wstring(j) + L" 隐藏");
            }
        }
    }
    // 4) 无两两重叠（至少保留装箱间距）
    for (int i = 0; i < count; ++i) {
        if (!out[i].visible) continue;
        for (int j = i + 1; j < count; ++j) {
            if (!out[j].visible) continue;
            // 数组顺序 == 屏幕顺序：j 在 i 右侧
            Check(out[i].x + items[i].width + spacing <= out[j].x + 0.001f,
                  std::wstring(tag) + L" 重叠: 项" + std::to_wstring(i) + L"[" +
                      std::to_wstring(out[i].x) + L"] 与项" + std::to_wstring(j) + L"[" +
                      std::to_wstring(out[j].x) + L"] 宽度 contentW=" +
                      std::to_wstring(contentW));
        }
    }
}

int RunLayoutProbe() {
    // R-Fix Bug1 结构化重构后的真实测量：
    //  - 工具条右侧只剩「?」（实测 = CalcTextSize("?")+2*FramePadding ≈ 26px），
    //    左侧动作流程（滑条+按钮+菜单）在含「内存加速…」时约在 x≈700 结束。
    //  - 状态栏右段 [全局热键复选框][权限徽标][帧耗时]，priority 2/1/0 ——
    //    热键(次要)先藏、帧耗时永不藏；左段（模式/p95/队列/进程数）约 x≈430。
    constexpr float kAboutW = 26.0f;
    constexpr float kSpacing = 8.0f;
    constexpr float kStatusLeftEnd = 430.0f;
    const stm::ui::HeaderItem kToolbarItems[1] = {{kAboutW, 0}};
    const stm::ui::HeaderItem kStatusItems[3] = {{190.0f, 2}, {66.0f, 1}, {72.0f, 0}};

    // Multi-width sweep: contract —— 任意窗宽 >=600 布局零重叠；「?」与帧耗时
    // (priority 0) 永不隐藏；>=900 全部项可见。工具条左端取两种真实形态：
    // 窄窗「⋮」折叠后 ≈560，全量动作 ≈700（退化钳制时允许 x == leftEnd）。
    const float widths[] = {300, 500, 600, 640, 700, 800, 820, 880, 900, 960, 1024,
                            1280, 1440, 1600, 1920, 2560};
    for (float w : widths) {
        for (float toolbarLeftEnd : {560.0f, 700.0f}) {
            stm::ui::HeaderPlacement place[1] = {};
            stm::ui::LayoutHeaderRight(w, toolbarLeftEnd, kSpacing, kToolbarItems, 1,
                                       place);
            AssertInvariants(L"工具条", w, toolbarLeftEnd, kSpacing, kToolbarItems, place,
                             1);
            Check(place[0].visible, L"「?」按钮必须始终可见/可点击");
            if (w >= 600.0f) {
                Check(place[0].x >= toolbarLeftEnd - 0.001f &&
                          (place[0].x + kAboutW <= w + 0.001f ||
                           place[0].x == toolbarLeftEnd),
                      L"窗宽 " + std::to_wstring((int)w) + L"（>=600）「?」应在右缘且不越界");
            }
        }

        stm::ui::HeaderPlacement splace[3] = {};
        const int svis = stm::ui::LayoutHeaderRight(w, kStatusLeftEnd, kSpacing, kStatusItems,
                                                    3, splace);
        AssertInvariants(L"状态栏", w, kStatusLeftEnd, kSpacing, kStatusItems, splace, 3);
        Check(splace[2].visible, L"状态栏帧耗时（priority 0）永不隐藏");
        if (w >= 900.0f) {
            Check(svis == 3, L"窗宽 " + std::to_wstring((int)w) +
                                 L"（>=900）状态栏三项全可见，实际 " + std::to_wstring(svis));
        }
        if (w < 774.0f && w >= kStatusLeftEnd) {
            // 430 左侧流 + 344 右侧组：低于 774px 时热键复选框
            //（重要性最低）必须让位而不是与任何东西重叠。
            Check(!splace[0].visible, L"窄窗口（" + std::to_wstring((int)w) +
                                          L"）状态栏应优先隐藏「全局热键」而非重叠");
        }
    }

    // 模糊测试：宽度 x leftEnd x 条目宽度集合，所有不变式都必须成立
    //（状态栏形态：优先级 {2,1,0}）。
    const float leftEnds[] = {0.0f, 100.0f, 300.0f, 430.0f, 700.0f};
    const float wsets[][3] = {
        {190.0f, 66.0f, 72.0f},   {120.0f, 30.0f, 20.0f}, {260.0f, 90.0f, 40.0f},
        {40.0f, 200.0f, 15.0f},   {0.0f, 0.0f, 0.0f},     {500.0f, 10.0f, 10.0f},
    };
    for (int wi = 200; wi <= 2000; wi += 13) {
        for (float le : leftEnds) {
            for (const auto& ws : wsets) {
                const stm::ui::HeaderItem it[3] = {{ws[0], 2}, {ws[1], 1}, {ws[2], 0}};
                stm::ui::HeaderPlacement place[3] = {};
                stm::ui::LayoutHeaderRight(static_cast<float>(wi), le, kSpacing, it, 3,
                                           place);
                AssertInvariants(L"模糊测试", static_cast<float>(wi), le, kSpacing, it,
                                 place, 3);
                Check(place[2].visible, L"模糊测试：priority 0 永不隐藏");
            }
        }
    }

    // 退化：左侧流超出内容边缘 -> 只有 priority-0 条目
    // 被钳制在左侧流末端。
    {
        stm::ui::HeaderPlacement place[3] = {};
        const stm::ui::HeaderItem it[3] = {{190.0f, 2}, {66.0f, 1}, {26.0f, 0}};
        const int vis =
            stm::ui::LayoutHeaderRight(400.0f, 500.0f, kSpacing, it, 3, place);
        Check(vis == 1 && place[2].visible && place[2].x == 500.0f,
              L"退化布局：应只保留 priority 0 项并钳制在 leftFlowEndX");
    }

    // FlowSegmentFits 基础用例。
    Check(stm::ui::FlowSegmentFits(0.0f, 100.0f, 100.0f), L"FlowSegmentFits 恰好放下");
    Check(!stm::ui::FlowSegmentFits(0.0f, 100.0f, 100.5f), L"FlowSegmentFits 超出拒绝");
    Check(!stm::ui::FlowSegmentFits(40.0f, 100.0f, 61.0f), L"FlowSegmentFits 游标推进后拒绝");

    wprintf(L"layout 探针完成：%d 项断言失败（宽度扫描 15 + 模糊 %d 组 + 退化/流式）\n",
            g_failures, ((2000 - 200) / 13 + 1) * 5 * 6);
    return g_failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// --mode wallpaper：经真实后端管线的 GPU 回读。
// ---------------------------------------------------------------------------

constexpr int kW = 160;
constexpr int kH = 120;

// 64x64 24bpp 自底向上 BMP，纯红。Wallpaper.cpp 经 stb 解码（BMP 开）。
bool WriteTestBmp(const std::wstring& path) {
    const int w = 64, h = 64;
    const int rowBytes = (w * 3 + 3) & ~3;
    const DWORD dataSize = static_cast<DWORD>(rowBytes * h);
    const DWORD fileSize = 54 + dataSize;
    std::vector<BYTE> buf(fileSize, 0);
    buf[0] = 'B';
    buf[1] = 'M';
    *reinterpret_cast<DWORD*>(&buf[2]) = fileSize;
    *reinterpret_cast<DWORD*>(&buf[10]) = 54;
    *reinterpret_cast<DWORD*>(&buf[14]) = 40;   // BITMAPINFOHEADER 头
    *reinterpret_cast<LONG*>(&buf[18]) = w;
    *reinterpret_cast<LONG*>(&buf[22]) = h;
    *reinterpret_cast<WORD*>(&buf[26]) = 1;
    *reinterpret_cast<WORD*>(&buf[28]) = 24;    // 每像素位数
    *reinterpret_cast<DWORD*>(&buf[34]) = dataSize;
    for (int y = 0; y < h; ++y) {
        BYTE* row = &buf[54 + static_cast<size_t>(y) * rowBytes];
        for (int x = 0; x < w; ++x) {
            row[x * 3 + 0] = 255;  // B（蓝）
            row[x * 3 + 1] = 0;    // G（绿）
            row[x * 3 + 2] = 0;    // R -> stbi 得到 (255,0,0) 红色
        }
    }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(f, buf.data(), fileSize, &wrote, nullptr) && wrote == fileSize;
    CloseHandle(f);
    return ok;
}

// 把已存壁纸 wallpaper.<ext> 移开，探针绝不会破坏真实壁纸。
std::wstring StoredWallpaperPath(const wchar_t* ext) {
    return stm::LocalAppDataRoot() + L"\\wallpaper\\wallpaper." + ext;
}

bool BackupStored(const wchar_t* const* exts, int n) {
    for (int i = 0; i < n; ++i) {
        const std::wstring p = StoredWallpaperPath(exts[i]);
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        MoveFileW(p.c_str(), (p + L".probebak").c_str());
    }
    return true;
}

void RestoreStored(const wchar_t* const* exts, int n) {
    for (int i = 0; i < n; ++i) {
        const std::wstring bak = StoredWallpaperPath(exts[i]) + L".probebak";
        if (GetFileAttributesW(bak.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        MoveFileW(bak.c_str(), StoredWallpaperPath(exts[i]).c_str());
    }
}

struct PixelCounts {
    int red = 0;      // 壁纸可见
    int winBg = 0;    // 不透明 approot WindowBg（缺陷的实色）
    int green = 0;    // 清屏哨兵（什么都没画）
    int other = 0;
};

// 经真实管线跑一帧。transparentShell = Bug2 修复后的形态
//（壁纸激活时 DrawShell 推送透明 WindowBg/ChildBg）；
// false = 修复前形态（不透明的 ##approot 全屏窗口）。
bool RenderFrame(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv,
                 ID3D11Texture2D* rt, ID3D11Texture2D* staging, bool transparentShell,
                 PixelCounts* out, int* outTotalVtx, int* outWallpaperCmds) {
    const float clear[4] = {0.0f, 1.0f, 0.0f, 1.0f};  // 绿色哨兵
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->ClearRenderTargetView(rtv, clear);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kW), static_cast<float>(kH));
    io.DeltaTime = 1.0f / 60.0f;
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();

    // 与 main.cpp 每帧所做完全一致（顺序经 V18 探针证实）。
    stm::ui::WallpaperDrawBackground(0.0f);  // mask 0：像素断言清晰

    // 迷你 ##approot（ui/Pages.cpp DrawShell 形态）。
    if (transparentShell) {  // R-Fix Bug2 修复后的形态
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    }
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##approot", nullptr, ImGuiWindowFlags_NoDecoration |
                                            ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::End();
    if (transparentShell) ImGui::PopStyleColor(2);

    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    *outTotalVtx = dd ? dd->TotalVtxCount : -1;
    // 统计绑定到非图集纹理（壁纸 SRV）的绘制命令。
    // 注意：统计在 RenderDrawData 之后收集——在
    // ImGuiBackendFlags_RendererHasTextures 下，字体图集 TexID 只有在
    // 后端处理 draw_data->Textures 时才设置（更早时 GetTexID 会断言）。
    ImGui_ImplDX11_RenderDrawData(dd);
    const ImTextureID fontTex = ImGui::GetIO().Fonts->TexRef.GetTexID();
    *outWallpaperCmds = 0;
    if (dd) {
        for (const ImDrawList* dl : dd->CmdLists) {
            for (const ImDrawCmd& cmd : dl->CmdBuffer) {
                const ImTextureID t = cmd.GetTexID();
                if (t != 0 && t != fontTex) ++*outWallpaperCmds;
            }
        }
    }

    // GPU 回读。
    ctx->CopyResource(staging, rt);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) return false;
    for (int y = 0; y < kH; ++y) {
        const BYTE* row = static_cast<const BYTE*>(map.pData) + static_cast<size_t>(y) * map.RowPitch;
        for (int x = 0; x < kW; ++x) {
            const BYTE b = row[x * 4 + 0], g = row[x * 4 + 1], r = row[x * 4 + 2];
            if (r > 200 && g < 80 && b < 80) ++out->red;
            else if (r < 60 && g < 70 && b < 90) ++out->winBg;   // 0.086/0.090/0.106（主题背景色）
            else if (g > 200 && r < 80 && b < 80) ++out->green;
            else ++out->other;
        }
    }
    ctx->Unmap(staging, 0);
    return true;
}

int RunWallpaperProbe() {
    static const wchar_t* const kExts[] = {L"png", L"jpg", L"jpeg", L"bmp", L"tga"};
    BackupStored(kExts, 5);

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring bmpPath = std::wstring(temp) + L"stm_uifix_probe.bmp";
    if (!WriteTestBmp(bmpPath)) {
        wprintf(L"[FAIL] 测试 BMP 写入失败\n");
        RestoreStored(kExts, 5);
        return 1;
    }

    // WARP：任何机器上像素都确定（近似 CI 环境）。
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) {
        wprintf(L"[FAIL] D3D11 WARP 设备创建失败 hr=0x%08X\n", static_cast<unsigned>(hr));
        RestoreStored(kExts, 5);
        return 1;
    }

    D3D11_TEXTURE2D_DESC rtDesc{};
    rtDesc.Width = kW;
    rtDesc.Height = kH;
    rtDesc.MipLevels = 1;
    rtDesc.ArraySize = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* rt = nullptr;
    dev->CreateTexture2D(&rtDesc, nullptr, &rt);
    D3D11_TEXTURE2D_DESC stDesc = rtDesc;
    stDesc.BindFlags = 0;
    stDesc.Usage = D3D11_USAGE_STAGING;
    stDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    dev->CreateTexture2D(&stDesc, nullptr, &staging);
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateRenderTargetView(rt, nullptr, &rtv);
    if (!rt || !staging || !rtv) {
        wprintf(L"[FAIL] 渲染目标创建失败\n");
        return 1;
    }

    ImGui::CreateContext();
    if (!ImGui_ImplDX11_Init(dev, ctx)) {
        wprintf(L"[FAIL] ImGui_ImplDX11_Init 失败\n");
        return 1;
    }
    ImGui::GetIO().Fonts->AddFontDefault();

    // 真实的生产加载路径（stbi 解码 -> 纹理 -> SRV）。
    stm::ui::WallpaperState st{};
    const bool loaded = stm::ui::WallpaperLoad(dev, ctx, bmpPath, &st);
    wprintf(L"WallpaperLoad: ok=%d size=%dx%d active=%d\n", loaded ? 1 : 0, st.width,
            st.height, stm::ui::WallpaperActive() ? 1 : 0);
    Check(loaded && stm::ui::WallpaperActive(), L"壁纸加载成功路径（stbi 解码非空 + SRV 创建）");

    int totalVtx = 0, wpCmds = 0;
    PixelCounts defect, fixed;

    // 1) 修复前外壳形态：不透明 ##approot -> 壁纸不可见。
    RenderFrame(ctx, rtv, rt, staging, /*transparentShell=*/false, &defect, &totalVtx,
                &wpCmds);
    wprintf(L"缺陷形态（不透明 WindowBg）: vtx=%d wpCmd=%d red=%d winBg=%d\n", totalVtx,
            wpCmds, defect.red, defect.winBg);
    Check(wpCmds > 0 && totalVtx > 0,
          L"DrawData 包含壁纸命令（V18 结论复证：DrawData 层从未缺失）");
    Check(defect.winBg > (kW * kH * 95) / 100,
          L"缺陷复现：不透明外壳下读回几乎全为 WindowBg 纯色");
    Check(defect.red < (kW * kH) / 20, L"缺陷复现：壁纸像素几乎为零");

    // 2) 修复后外壳形态：透明推送 -> 壁纸可见。
    RenderFrame(ctx, rtv, rt, staging, /*transparentShell=*/true, &fixed, &totalVtx,
                &wpCmds);
    wprintf(L"修复形态（透明 WindowBg/ChildBg）: vtx=%d wpCmd=%d red=%d winBg=%d\n",
            totalVtx, wpCmds, fixed.red, fixed.winBg);
    Check(fixed.red > (kW * kH * 80) / 100, L"修复验证：读回像素 80%+ 为壁纸红色");

    stm::ui::WallpaperClear();
    ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext();
    if (rtv) rtv->Release();
    if (staging) staging->Release();
    if (rt) rt->Release();
    ctx->Release();
    dev->Release();
    DeleteFileW(bmpPath.c_str());
    RestoreStored(kExts, 5);

    wprintf(L"wallpaper 探针完成：%d 项断言失败\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    // 面向宽字符的 stdout，wprintf 绝不会在 CJK 文本中途中断。
    _setmode(_fileno(stdout), _O_U16TEXT);
    std::string mode = "layout";
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--mode=", 7) == 0) mode = argv[i] + 7;
    }
    stm::LogInit(stm::LocalAppDataRoot() + L"\\logs");
    if (mode == "wallpaper") return RunWallpaperProbe();
    if (mode == "layout") return RunLayoutProbe();
    wprintf(L"未知模式: %hs（layout|wallpaper）\n", mode.c_str());
    return 2;
}

#endif  // STM_UIFIX_PROBE
