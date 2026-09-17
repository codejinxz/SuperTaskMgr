# R3 调研报告：WinUI 3 / C++/WinRT（Windows App SDK）技术路线

调研日期：2026-09-18 ｜ 环境：Win11 23H2 x64，VS2022 Build Tools（无 IDE）+ CMake + Git

## ① 一句话结论
**不推荐**：技术上"提权+便携"勉强可行（WASDK 1.1 起支持非打包提权运行），但构建体系强绑 MSBuild（CMake 官方不支持）、无 C++ 可用的成熟 DataGrid/图表库、内存与启动开销对"任务管理器"自身是讽刺性负担，综合性价比低于原生 Win32 路线。

## ② 八维评分（总分 2.98 / 5）
| 维度 | 权重 | 得分 | 依据（一句话） |
|---|---|---|---|
| 采集层集成 | 20% | 4 | C++/WinRT 与 Win32 采集代码同进程共存无障碍，XAML 线程模型需隔离但可控 |
| UI能力与体验 | 20% | 3 | Fluent/Mica/现代观感一流，但无官方 DataGrid、无经典 {Binding}，数据密集控件要自建 |
| 性能开销 | 15% | 2 | 启动慢于 WPF/Win32（官方文档专文优化指引），内存占用被微软公开承认偏高 |
| C/C++匹配度 | 15% | 3 | C++/WinRT 一等公民、Win2D 支持 C++，但控件生态与构建链均以 C#/MSBuild 为中心 |
| 权限与提权兼容 | 10% | 3 | 非打包提权自 1.1 起官方支持；MSIX 提权受限（allowElevation rescap），提权下 FilePicker 等仍有已知坑 |
| 生态与许可证 | 8% | 3 | WASDK/WinUI/C++/WinRT/Win2D 全 MIT，生态活跃但贡献控件多为 C# 实现 |
| 分发体积 | 7% | 2 | 非打包依赖框架需装 Runtime，自包含"显著增大"输出目录（官方原话），便携目录动辄上百 MB |
| 长期维护 | 5% | 3 | 现代生命周期：每个大版本仅支持约 1 年（2.0 至 2027-04-29），需每年跟版升级 |

## ③ 逐问题回答

### 1. Windows App SDK 现状（2026-09）
- 当前稳定版 **2.5.1**（2026-09-16 发布）；稳定大版本节奏约每 6 个月一个；2.0 于 2026-04-29 发布，服务期至 **2027-04-29**；1.8 维护期已结束（2026-09-09），1.7/1.6 均已出保（来源：release-channels 页生命周期表）。
- 支持策略：受 **Microsoft Modern Lifecycle** 约束，必须始终使用最新补丁才在支持范围内；兼容到 Win10 1809，但仅对仍在支持期的 Windows 提供支持。
- 打包形态：
  - **MSIX 打包**（含"外部位置打包"）：有包标识，支持 Store/AppInstaller 更新、包清单能力。
  - **非打包（WindowsPackageType=None）**：两种子形态——①框架依赖：需目标机装 Windows App Runtime（随附 Runtime 安装器或引导用户装一次）；②**自包含**（WindowsAppSDKSelfContained=true）：Runtime 全量 DLL 复制进应用目录，官方明确"输出目录显著增大"（exact 体积待按 2.5.1 实测，1.x 时代社区实测数十至百余 MB 量级）。非打包无包标识：无自动更新/后台任务/包清单文件关联。自包含+非打包是便携分发的官方形态（来源：unpackage-winui-app 文档）。

### 2. 【重点】提权与打包的摩擦
- **MSIX 打包 + requireAdministrator**：微软官方支持立场是"受限"。常规 MSIX 不兑现 exe 内嵌 manifest 的 requireAdministrator；要让打包应用提权需 `rescap:Capability Name="allowElevation"`（受限能力）+ exe manifest 双管齐下，且此类应用"基本不可能通过商店认证"，需邮件 reportapp@microsoft.com 预批（来源：Microsoft Q&A 5907620）。GitHub 上"WASDK MSIX 提权不被支持"（issue #896）、"所有打包 WinUI3 应用提权启动崩溃（Win10）"（issue #6268，仍 Open、needs-triage）佐证该路径不稳。
- **非打包 + 管理员运行**：1.0 明确"完全不支持提权"（0xc0000409 in Bootstrap.dll），官方修复于 **1.1 落地提权支持**（PR #2066，discussion #2313 维护者确认"elevated/admin support on the roadmap→1.1"）。即当前 2.x 非打包提权是**官方支持路径**。残留已知坑：提权下 FilePicker 抛异常（dotnet/maui#20830，WASDK 2.0 新 StoragePickers 是否缓解：待验证）；提权进程与非提权 Explorer 间拖放被 UIPI 阻断（Windows 通用行为，非 WinUI 特有）；工具菜单"以管理员身份运行"二次启动需自处理。
- **明确结论**：**"默认管理员运行 + 便携目录分发"可以支撑，但仅限"非打包 + 自包含"形态**（WindowsPackageType=None + WindowsAppSDKSelfContained=true + exe manifest requireAdministrator）。MSIX 路线与硬性提权需求冲突，应直接排除。

### 3. 纯 Build Tools CLI 构建可行性
- 官方立场：WinUI 3 XAML 编译（.xaml→.g.h/.pri）、清单合并、MRT Core 资源管线全部是 **MSBuild tasks**（Microsoft.UI.Xaml.Build.Tasks，内部 XamlCompiler.exe 为 net472）；微软在 microsoft-ui-xaml#6792 明确"CMake 现阶段不可行，因依赖仅 MSBuild 提供的 build tasks"；WindowsAppSDK#3901 记录"无 VS/MSBuild 消费 WASDK 不受支持/无文档"。
- 现实路径：**msbuild.exe 命令行构建 .vcxproj 完全可行**——VS2022 Build Tools 需安装：C++ v143 桌面工作负载 + UWP/C++ 构建工具（makepri、appx 清单工具）+ Windows 11 SDK + .NET Framework 4.7.2+ Targeting Pack（XamlCompiler 依赖）+ MSIX 打包工具组件（组件清单随版本变化，待以 `vs_installer export` 实测固化）。NuGet 侧：Microsoft.WindowsAppSDK（含 C++/WinRT 投影与 targets）、Microsoft.Windows.SDK.BuildTools。
- CMake：仅 C++/WinRT **无 XAML** 的库/EXE 可用 cppwinrt 正常走 CMake；XAML 工程需社区方案 res2k/winui3_cmake（第三方、活跃度低、跟版 WASDK 有滞后风险）。**可行折中**：采集引擎/图表核心用 CMake，UI 壳用 MSBuild，一个仓库两套构建。结论：**"无 IDE 纯命令行"可行，但必须用 MSBuild 而非 CMake 驱动 XAML 工程**，工程模板需手工从 WinUI 3 Gallery/模板仓库迁移（VS 模板不可用是额外摩擦）。

### 4. C++/WinRT 开发效率与坑；表格虚拟化
- 常见坑：①`winrt::apartment_context` 回 UI 线程，后台 await 后忘切线程即崩；②事件订阅必须保存 `winrt::event_token` 或用 auto_revoke（`winrt::...::auto_revoke_t`），否则对象销毁后回调悬垂崩溃——高频刷新场景（1 秒级）最易踩；③WinUI 3 **无 DataContext、无经典 {Binding}**，只能 x:Bind 编译绑定（性能好但写法啰嗦，函数绑定须线程正确）；④hstring/string_view 生命周期、IVector 与 std::vector 互转的开销在每秒全表刷新时需仔细设计（增量更新 + 观察者）。
- DataGrid 现状：WinUI 3 **无官方 DataGrid**；Windows Community Toolkit 的 DataGrid 已弃用并归档（官方建议改用 DataTable 或 **WinUI.TableView**）。但 **WinUI.TableView 是 C# 实现**，C++/WinRT 工程直接消费 C# WinRT 组件需托管运行时与 reg-free WinRT 配置，便携场景可操作性差（待验证）。现实选择：ListView 虚拟化 + 自定义列，或 **ItemsRepeater**（原生、低层，无选择/表头/编辑，需自建数千行级表格骨架，估 1–3 周级工作量）。

### 5. 实时图表
- **LiveCharts2 不可用于 C++**：纯 .NET 库（SkiaSharp 封装），无 C++ 绑定。
- C++ 方案：**Win2D（Microsoft.Graphics.Win2D）官方支持 C++/WinRT**，CanvasControl/CanvasAnimatedControl + CanvasDrawingSession（D2D）画滚动曲线；1Hz CPU/内存/网络多序列折线属 CanvasControl + 定时 Invalidate 的常规工作量（数天/图，含缩放悬停提示则数周）。替代：SwapChainPanel + 裸 D2D/D3D（工作量更大，性能上限更高）。自绘是可行且必要的——无现成 C++ 图表控件。

### 6. 应用自身内存/启动开销
- 定性结论：WinUI 3 应用冷启动显著慢于传统 Win32（微软有专文 startup-performance 优化指南；GitHub #8595/#11096 社区实测慢于 WPF/UWP）；内存方面 GH #4606 报告 WinUI 占用远超 WPF/UWP，2026-07 媒体报道微软公开承认 WinUI 原生应用"吃内存"并承诺优化，2026 年优化工作号称启动时间约降 25%（具体幅度随版本浮动）。对比：轻量 Win32 工具工作集常在 10–30MB，WinUI 3 壳空载即常见数十至上百 MB 量级（精确数字随版本/页面复杂度浮动，待实测）。对一个监控内存的工具而言，此开销会被用户直观察觉。

### 7. 综合结论
**不推荐作为首选。** 理由：①提权仅"非打包+自包含"一途可行，体积代价大；②CMake 不支持，与本仓"纯 Build Tools+CMake"约束正面冲突（要么双构建体系要么引入低活跃第三方 CMake 脚本）；③无 C++ 可用的 DataGrid/图表，核心 UI 全自建，团队优势（系统编程）发挥不出来；④内存/启动开销与产品定位（资源监控器常驻）直接矛盾；⑤每年一次大版本升级压力（Modern Lifecycle）。若团队仍想要现代观感，仅建议以"非打包自包含"形态做小规模 PoC 验证提权+便携后再评估。

## ④ 依赖清单 + 许可证
| 组件 | 用途 | 许可证 |
|---|---|---|
| Microsoft.WindowsAppSDK（NuGet，2.5.x） | WinUI 3 运行时/投影/构建 targets | MIT |
| Microsoft.Windows.SDK.BuildTools（NuGet） | SDK 工具链版本锚定 | MIT |
| C++/WinRT（随 SDK/cppwinrt） | WinRT 语言投影 | MIT |
| Microsoft.Graphics.Win2D（NuGet，1.4.x） | 图表自绘 | MIT |
| VS2022 Build Tools：C++ v143 + UWP 工具 + Win11 SDK + .NET FX 4.7.2+ TPKG | MSBuild/XamlCompiler/makepri | 专有（免费用） |
| res2k/winui3_cmake（可选） | CMake 驱动 XAML 编译 | MIT（第三方） |

## ⑤ 风险与降级方案
- 风险：提权下 FilePicker/剪贴板等边缘 API 行为异常（2.0 StoragePickers 待验证）；每年强制升级 WASDK；CMake 第三方脚本跟版断裂；C# 生态控件（TableView、LiveCharts2）对 C++ 工程不可用；Win10 LTSC 打包形态提权崩溃（#6268）若未来走打包会复现。
- 降级方案 A（推荐备选）：C++/WinRT 保留、弃 XAML——纯 Win32 窗口 + D2D 自绘（无 WASDK 依赖，体积/内存最小）。
- 降级方案 B：MSBuild 壳 + CMake 引擎双构建；XAML 页面最小化，数据表用 ListView/ItemsRepeater 自建。
- 降级方案 C：主进程非提权 WinUI 壳 + 提权辅助进程/服务做采集（架构复杂度换打包自由度）。

## ⑥ 参考链接
- 发布渠道与生命周期：https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/release-channels
- 2.x 发布说明：https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/release-notes/windows-app-sdk-2-0
- 非打包分发：https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/unpackage-winui-app
- 提权 Q&A（allowElevation）：https://learn.microsoft.com/en-au/answers/questions/5907620/run-msix-packaged-app-as-admin
- 提权相关 issue：https://github.com/microsoft/WindowsAppSDK/issues/896 ｜ https://github.com/microsoft/WindowsAppSDK/issues/6268 ｜ https://github.com/microsoft/WindowsAppSDK/discussions/2313 ｜ https://github.com/microsoft/WindowsAppSDK/discussions/671
- CMake 不支持：https://github.com/microsoft/microsoft-ui-xaml/issues/6792 ｜ https://github.com/microsoft/WindowsAppSDK/issues/3901 ｜ https://github.com/res2k/winui3_cmake
- DataGrid 弃用归档：https://learn.microsoft.com/en-us/dotnet/communitytoolkit/archive/windows/datagrid ｜ https://github.com/w-ahmad/WinUI.TableView ｜ ItemsRepeater：https://learn.microsoft.com/en-us/windows/apps/develop/ui/controls/items-repeater
- Win2D：https://learn.microsoft.com/en-us/windows/apps/develop/win2d/ ｜ LiveCharts2（.NET）：https://github.com/beto-rodriguez/LiveCharts2
- 性能：https://learn.microsoft.com/en-us/windows/apps/develop/performance/app-startup-performance ｜ https://github.com/microsoft/microsoft-ui-xaml/issues/4606 ｜ https://github.com/microsoft/microsoft-ui-xaml/discussions/8595
