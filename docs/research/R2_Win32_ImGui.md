# R2 调研报告：原生 Win32 + Dear ImGui(docking) + D3D11 + ImPlot 全自绘路线

## ① 一句话结论
对「开发者精通 C/C++ + 数据密集型系统监控工具」这一场景，本路线匹配度极高：采集层以 POD 快照直供立即模式 UI、零封送零框架开销、单 exe 免依赖分发，主要代价是无障碍/自动化缺失与非原生观感（对管理员自用工具影响很小）。

## ② 八维评分表（加权总分约 4.75/5）
| 维度 | 权重 | 得分 | 依据（一句话） |
|---|---|---|---|
| 采集层集成 | 20 | 5 | 立即模式每帧只读最新快照，采集线程与 UI 仅隔一层双缓冲 memcpy，无消息封送 |
| UI能力与体验 | 20 | 4 | 树/表格/排序/右键/停靠/悬浮窗/图表全齐且可深度定制；但富文本、精致 IME、原生细节需自补 |
| 性能开销 | 15 | 5 | 本项目量级（800 进程、5 千单元格@1Hz、数千点曲线）远低于 ImGui/ImPlot 实测舒适区 |
| C/C++匹配度 | 15 | 5 | 纯 C++ 源码 vendoring + CMake 即用，无 COM/ABI/UI 框架绑定 |
| 权限与提权兼容 | 10 | 5 | 普通 HWND + 自绘，无 UIPI/框架限制，提权运行无额外约束 |
| 生态与许可证 | 8 | 5 | imgui/implot 双 MIT，社区巨大、示例与后端齐全 |
| 分发体积 | 7 | 5 | 单 exe 约 2-5 MB（估算），除系统 d3d11/dxgi 外零 DLL |
| 长期维护 | 5 | 4 | imgui 极活跃（v1.92.9b，2026-07）；implot 2026-04 换任维护者后发 v1.0，历史有慢速期；docking 仍未入主线 |

## ③ 逐问题回答
### 1. 版本与 docking 分支状态（已查证）
- 主线最新稳定版 **v1.92.9b（2026-07-31，v1.92.9 的热修）**；docking 分支 CHANGELOG 为 **1.93.0 WIP**，含全部 1.92.9b 提交。来源：releases 页与 docking 分支 CHANGELOG.txt。
- **截至 2026-09，docking 尚未并入 master**：官方口径是 master 与 docking 定期互同步、"generally safe and recommended to sync to latest master or docking branches"；docking 长期标注 beta 但被大量商业项目当主分支用（README、issue #4881）。
- v1.92.0（2025-06）是"2015 年以来最大破坏性更新"：动态字体（任意字号按需光栅化）、`ImTextureRef`、后端新纹理协议 `RendererHasTextures`。选 docking 分支 = 主线 + Docking/多视口，风险低。

### 2. 对本类工具的适配度
- **表格**：`BeginTable` 行虚拟化靠 `ImGuiListClipper`，官方注释明言"tens of thousands of items without a problem"→10 万行可行（需行高固定）。**列不虚拟化**：每可见行全部 15 列都要提交，但 500 行×15 列=7500 单元格/帧，按立即模式典型吞吐（简单部件每秒百万级，桌面 CPU）为 1-2ms 量级（估算，待实测）。注意：立即模式 UI 本身每帧重绘，"1Hz 全刷新"不省提交成本，省的是你的数据侧计算。
- **ImPlot**：官方 FAQ 实测口径"tens to hundreds of thousands of points without issue"；1Hz、数千点滚动窗口属轻量（shaped 数据 + set_next_plot_limits 追踪即可）。更大规模用 striding 降采样或实验性 GPU 后端分支。v1.0（2026-04-05）改用 ImPlotSpec 样式 API（旧 SetNextXXX 废弃）。
- **树/右键/悬浮窗/搜索**：TreeNode、BeginPopupContextItem、浮窗、ImGuiTextFilter 内置；表格排序（ImGuiTableSortSpecs）、列宽持久化（Table settings）内置；多选+范围选（Ctrl/Shift）有官方 Multi-Select API 且兼容 clipper。
- **需自补**：单元格内富文本/多色行（自行拼 ImDrawList）、Shell 文件拖放进 UI（Win32 侧 WM_DROPFILES 自己接）、精细滚轮触控板体验、以及所有"原生观感"细节。

### 3. 中文渲染（msyh.ttc）
- 1.92 动态字体后：无需预置 glyph ranges，字形按需光栅化、任意字号；stb_truetype 为默认光栅器，FreeType 经 `IMGUI_ENABLE_FREETYPE` 启用（官方称"generally beneficial"）。来源：v1.92.0 release notes。
- **stb vs FreeType 取舍**：stb 快、零第三方依赖、纯 MIT 链最干净，但无 hinting，小字号中文略糊；FreeType（imgui_freetype）对 CJK 小字号质量更好、对 TTC/命名实例处理更稳。msyh.ttc 是 TTC 集合，两者都支持 face index 选择，FreeType 路线更省心。**推荐 FreeType**（质量优先，代价是许可证链加 FTL 与 ~1MB 体积）。
- 显存/内存：图集纹理随实际用到的字形成长，中文界面常用 2-4 千字形，18px RGBA 约数 MB 纹理（估算）；若强制载入全量 2 万+ 中文字形才会到 4096²/几十 MB 量级（估算，待实测）。
- 字体范围裁剪实践：动态字体下仅"运行时未出现过的字形会缺字"，冷门汉字首次显示即按需加载，一般无需裁剪；追求极致内存可预载常用 3500 字集合。msyh.ttc **不可随包分发**（微软字体许可），但 Win10/11 全版本系统自带，运行时从 `C:\Windows\Fonts\msyh.ttc` 加载即可。

### 4. 与后台采集线程的数据交接（本路线最大加分项）
- 模式：采集线程（可独立静态库，甚至独立优先级/作业对象）每周期产出一帧 POD 快照，写入**双/三缓冲**（SRWLock 或单写者 seqlock）；UI 线程每帧开头 `memcpy` 最新快照（800 进程×~1KB ≈ 0.8MB，memcpy 亚毫秒级）后只读绘制；1s 数据节奏与 60fps 渲染天然解耦。
- 帧内零分配：立即模式下用 `Reserve`/帧栈缓冲即可全程无 malloc，避免 GC 式抖动。
- 对比控制类 UI（Win32 ListView 需 LVM_* 消息封送、每行 SetItem）：本路线采集层是**纯 C++ 无 UI 依赖**，接口就是"给我一份快照结构体"，C++ 工程天然契合，无需任何消息泵/COM/跨线程 GUI 调用。

### 5. 诚实评估短板
- **无障碍/UIA**：完全没有内置支持，为多年未解决的长线请求（#1251、#8022、#4122）；ocornut 表态不反对但自认无力实现（#7892）。后果：AutoHotkey/UIA 自动化、屏幕阅读器、部分远程辅助工具失效；自测需用官方 imgui_test_engine 而非 UIA。对管理员自用影响小，对"可被脚本驱动"有要求则是一票否决项。
- **IME**：Win32 后端处理 WM_IME_SETCONTEXT/WM_IME_COMPOSITION 并经 `PlatformImeData` 定位候选窗，中文输入可用；但合成串由 IME 悬浮窗渲染、焦点竞态等边角问题有已知案例（#4642），多视口下候选窗定位曾有坑。本工具搜索框多为进程名/英文，影响有限（待实测中文过滤体验）。
- **DPI**：1.92 起配合后端默认支持高 DPI（FramebufferScale、`style.FontScaleDpi`、`io.ConfigDpiScaleFonts/Viewports`），Win32 后端有 EnableDpiAwareness/GetDpiScaleForMonitor；动态字体使跨屏 DPI 切换可实时重排。仍需自己接 WM_DPICHANGED 与跨屏样式微调，属"可用但不如原生无感"。
- **观感**：非原生控件外观、无系统主题跟随（暗色为默认审美）；管理员自用可接受，但与 Mica/系统沉浸式体验无缘。

### 6. 原生部件补齐（与 ImGui 共存均无冲突）
- 原生菜单：`SetMenu` 挂在客户区外由 OS 绘制，WM_COMMAND 直达 WndProc，与 ImGui 互不干扰（或干脆用 ImGui 主菜单栏）。
- 托盘：`Shell_NotifyIconW` + 回调消息，纯 Win32；任务栏进度：`ITaskbarList3::SetProgressValue`，纯 COM 调用即可。
- 公共对话框：COM `IFileOpenDialog` 模态运行自带消息循环，期间暂停 ImGui 帧即可，标准做法。
- docking 分支多视口会创建附属 Win32 窗口，`ImGui_ImplWin32` 原生支持（悬浮传感器窗可用其实现，或自己做 WS_POPUP 窗）。

### 7. 分发
- 体积：/MT 静态链接 + imgui + implot + FreeType + D3D11 后端，单 exe 约 **2-5 MB（估算，待实测）**；字体不内嵌（用系统 msyh.ttc）；除系统 d3d11.dll/dxgi.dll 外零第三方 DLL，便携目录 = exe + imgui.ini + LICENSE。
- 许可证链（已查证）：imgui MIT；implot MIT；imgui_freetype（imgui 仓内，MIT）链接 FreeType——FreeType 双许可 **FTL / GPLv2，选 FTL**（BSD 型+署名条款）；stb_truetype MIT/公有领域双许可。若不用 FreeType 则全链纯 MIT。
- 无网络拉源码：可行——imgui 每个 tag 有 release tarball（v1.92.9b / docking 分支 zip）、implot v1.0 tag tarball、FreeType 官方 tarball（savarannah/GitHub 镜像），一次性下载后 vendoring 进仓库，CMake `add_library` 编 13+4 个源文件即可，无需 vcpkg/Conan。

### 8. D3D11 后端兼容性
- 后端着色器以 **vs_4_0/ps_4_0** 编译（imgui_impl_dx11.cpp L472/L525，已核对源码）→ Feature Level 10.0+ 即可；示例默认建 FL 11.0 设备。
- WARP 兜底：`D3D_DRIVER_TYPE_WARP` 提供软件 FL 11.0，Win10/11 必在，RDP/虚拟机/独显损坏场景可靠。
- 备选后端：imgui_impl_dx9（兼容面最广、老机器/精简系统最稳）与 imgui_impl_opengl3（依赖 GL 驱动，RDP 下反不如 WARP+D3D11）。目标定为 Win10 21H2+ 后，D3D11+WARP 已足够，D3D9 仅作保险丝（代码量小，可留）。

## ④ 依赖清单 + 许可证
| 依赖 | 版本(2026-09) | 许可证 | 说明 |
|---|---|---|---|
| ocornut/imgui (docking 分支) | 1.93.0-WIP（含 v1.92.9b） | MIT | 核心 + Win32/DX11 后端 |
| epezent/implot | v1.0 (2026-04-05) | MIT | 图表 |
| FreeType + imgui_freetype | latest | FTL（选）/GPLv2 | CJK 光栅质量；不用则全 MIT |
| stb_truetype | 随 imgui | MIT/PD | 默认光栅器 |
| msyh.ttc | 系统自带 | 微软系统组件 | 只运行时加载，不分发 |

## ⑤ 风险与降级方案
1. docking 长期不并主线 → 锁定 tag vendoring（1.92.9b 已含全部所需），升级节奏自控；docking 分支与主线定期同步，历史从未烂尾。
2. ImPlot 换任维护者的长期风险 → 图表层薄封装（仅用 PlotLine/PlotBars 基础 API），必要时自绘折线（几百行代码）。
3. v1.92 大改 API（网上大量 1.91 前教程失效）→ 只参考 1.92+ 文档与 demo 窗口。
4. UIA/自动化硬需求出现 → 本路线否决级风险；可降级为"核心逻辑与 UI 分层 + imgui_test_engine 自测"，或换 Qt/WinUI 路线（他组报告覆盖）。
5. 中文小字号渲染不满意 → FreeType + 关闭抗锯齿微调/加大字号 + 20px 缓存多档字号。
6. 7500 单元格@1Hz 实测超预算 → 按 dirty 行做条件提交、或列裁剪/折叠、或把 1Hz 数据缓存为纹理化摘要（不太可能需要）。

## ⑥ 参考链接
- https://github.com/ocornut/imgui （README 版本指引；releases：v1.92.9b 2026-07-31）
- https://github.com/ocornut/imgui/releases/tag/v1.92.0 （动态字体/破坏性变更/FreeType 开关/DPI）
- https://github.com/ocornut/imgui/issues/4881 （docking 发布请求，未并主线佐证）
- https://github.com/epezent/implot （README：MIT、性能口径、32 位索引警告）；releases：v1.0 2026-04-05
- imgui.h `ImGuiListClipper` 注释（"tens of thousands of items"）；backends/imgui_impl_dx11.cpp（vs_4_0/ps_4_0）
- 无障碍：https://github.com/ocornut/imgui/issues/8022 、#1251、#4122、#7892
- IME：#4642；`PlatformImeData`/Win32 后端 WM_IME_* 处理
- FreeType 许可：https://freetype.org/license.html （FTL/GPLv2）
