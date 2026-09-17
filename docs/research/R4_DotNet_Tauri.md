# R4 调研报告：.NET(Avalonia/WPF/WinForms) 与 Rust+Tauri 2 参照路线评估

## ① 一句话结论
对「纯 C++ 背景 + 系统级采集 + 便携单目录分发」场景，**两条路线净收益均为负**：Avalonia 是"可控的小负"（UI 提效换来的采集层与体积代价），Tauri 是"明确的大负"（采集缺口最大、C++ 资产几乎全部重写）。

## ② 八维评分表（1-5 分 / 权重 / 加权分）

| 维度(权重) | Avalonia 11 (.NET) | 依据 | Tauri 2.x (Rust) | 依据 |
|---|---|---|---|---|
| 采集层集成(20) | 4 | P/Invoke 调纯 C++ DLL 顺畅，C++/CLI 可选 | 2 | sysinfo 缺按进程网络/句柄，需 windows-rs/ETW 补写 |
| UI能力与体验(20) | 4 | TreeDataGrid+ScottPlot 可用；DataGrid 大表有坑 | 3 | Web 表格/图表生态最强，但原生观感弱、提权 UI 复杂 |
| 性能开销(15) | 3 | 1Hz×500 行无压力；GC/启动/内存高于原生 | 3 | Tauri2 原始字节 IPC 可绕开 JSON，但渲染进程常驻 40-150MB+ |
| C/C++匹配度(15) | 3 | C# 上手极低，但托管+原生双层调试复杂 | 2 | Rust 所有权曲线 + 采集层重写；C ABI FFI 复用可行但非惯用 |
| 权限与提权兼容(10) | 4 | 标准 UAC 清单即可，.NET 提权运行成熟 | 3 | 可提权，但 WebView2+驱动/服务操作链路更长 |
| 生态与许可证(8) | 5 | MIT 框架+MIT 图表库，NuGet 海量 | 4 | MIT/Apache-2.0 全链宽松，npm+crates 双生态 |
| 分发体积(7) | 2 | Win11 不预装 .NET 8；自包含数十~100+MB | 5 | 包 3-10MB + WebView2 Win11 预装 |
| 长期维护(5) | 4 | 微软 LTS + Avalonia 公司化维护(11.3.x/12) | 3 | Tauri 2 稳定活跃，但 Rust+JS 双栈维护面大 |
| **加权总分** | **3.64** | | **2.87** | |

## ③ 逐问题回答

**1. Avalonia 11 成熟度（Windows）**：框架 MIT（2024-04 起新许可页仍为 MIT），11.3.x 系列持续维护（另已有 12 系列）。官方性能文档明确：大数据量推荐用 **TreeDataGrid** 而非内置 DataGrid；社区实测 ObservableCollection 10 万+ 行有严重性能问题，且 DataGrid 每次集合变更触发全量枚举（Issue #11724），虚拟化仅虚化可视元素、需自做数据级虚拟化。图表：**ScottPlot.Avalonia 5.1.x（MIT，SkiaSharp 渲染，适合实时大点数）**；**LiveCharts2（MIT 免费版 + 付费性能包，SkiaSharpView.Avalonia 2.0.x）**。1Hz 全表刷新 500 行属轻负载，实践上需避免 1Hz 全量替换集合（应原地更新单元格）——此为社区共识做法，无官方基准「待验证」。

**2. WPF/WinForms 老而稳参照**：WPF ListView/ListBox 数据绑定时默认启用 UI 虚拟化（VirtualizingStackPanel，官方文档）；WinForms DataGridView 有 **VirtualMode**（CellValueNeeded 按需供数，官方定位"very large data sets"）。图表 ScottPlot 5 WinForms 版 MIT；LiveCharts2 同样支持 WinForms/WPF。这层 API 冻结 15 年+，坑最少，是 UI 参照的"下限保证"。注意：本应用对 C++ 开发者而言 WinForms 设计器/事件模型属新范式。

**3. 分发体积与启动**：**Windows 11 不预装 .NET 8/9 桌面运行时**（仅带 .NET Framework 4.8.1；dotnet/core#6411 讨论）。framework-dependent 包体极小（MB 级）但要求用户装运行时；self-contained 官方描述为"显著更大"，实测量级数十 MB 至 ~150 MB（未裁剪；精确数字依应用而异「待验证」）。**NativeAOT + WinForms：.NET 9 非官方支持场景**（dotnet/winforms discussion #11257），DataGridView 等重度反射控件兼容性差；NativeAOT 官方限制：无 Reflection.Emit、无 Assembly.LoadFile、**不支持 C++/CLI**、与裁剪互斥。即 AOT 与"DataGrid/互操作"两头不可兼得。

**4. C++ 采集层互操作三方案**：
- a) **C++/CLI 混编桥**：VS2022 支持，仅 Windows、仅 DLL 输出、不可裁剪/AOT。桥层薄、类型转换直接（STL↔托管类），但引入双运行时心智与编译器开关(/clr)维护。工作量：中；崩溃面：原生崩溃直接带崩托管进程；部署：单目录 OK（多一个混合模式 DLL）。
- b) **P/Invoke 纯 C ABI DLL**：推荐。采集 DLL 用 C 接口 + 扁平结构体，C# 端 DllImport/StructLayout，工具链（CMake+MSVC）与现有完全一致。工作量：低（接口设计为主）；崩溃面：同上可被 try/catch 部分隔离但原生崩溃仍致命；部署：一个原生 DLL。
- c) **独立采集进程 + 命名管道/共享内存**：崩溃隔离最好（采集层崩了 UI 不死），且天然支持以更高/更低权限运行、多消费者。代价：协议设计+进程生命周期管理，工作量最高。对"任务管理器"类高可用工具值得。
- 三方案共同点：**采集层 C++ 代码均可 100% 复用**，这是 .NET 路线相对 Tauri 的决定性优势。

**5. 语言切换成本**：C# 对精通 C++ 者上手难度诚实评估为**很低**（语法族相同、GC 免去内存管理、LINQ 即学即用，1-2 周可产出）。真实成本在**双层调试**：托管堆/原生堆两套内存模型、两套诊断器（VS 混合模式调试可用但慢）、两套崩溃转储分析（托管+WER）。UAC/提权、COM、句柄语义仍需 C++ 知识，此部分反而无缝。

**6. Tauri 2 现状与 sysinfo 覆盖度**：Tauri 2.0 于 2024-10 稳定，2026-09 主版本 2.11.x，MIT OR Apache-2.0。后端经 windows-rs（微软官方，MIT/Apache-2.0）可调全部 Win32，能力上限等同 C++。**sysinfo 0.39.x（MIT）Process 结构实测清单**：cpu_usage、memory(RSS)、virtual_memory、disk_usage、parent()、cmd()、exe()、start_time()、user_id、kill、open_files（仅文件描述符级）。**缺失：按进程网络 I/O（仅系统级 Networks）、GDI/USER/全句柄计数、线程级细节有限、服务/驱动/启动项管理完全没有**。即本应用约一半采集需求（网络/句柄/服务/驱动/启动项）必须自行用 windows-rs/ETW 写——等于用 Rust 重写 C++ 已有代码。

**7. 1Hz×500×15 列推送到 WebView2**：Tauri v1 全 JSON IPC 有著名性能问题（issue #4197 报告比 Electron 慢 200 倍的极端案例）；**Tauri 2 新增 `tauri::ipc::Response`/ArrayBuffer 原始字节通道**可绕过 JSON 序列化。量级估算：500 进程×15 列纯文本扁平化约数百 KB/秒，走二进制+共享内存/文件映射可低开销（社区惯用法），走 JSON 事件流则序列化+解析成为瓶颈（多篇文章记录"IPC 带宽墙"）。前端虚拟滚动方案成熟：AG Grid Community(MIT)、TanStack Table+Virtual(MIT)、Glide Data Grid(MIT) 均可支撑数十万行。结论：**可行但要多绕一层**（自定二进制协议+前端解析），这套协议本身就是 C++ 原生路线不需要付的成本。

**8. WebView2 依赖与体积**：官方分发文档明确 **"Evergreen WebView2 Runtime 将作为 Windows 11 操作系统的一部分包含"**（Win10 绝大多数设备也已装）。Tauri 包体 3-10MB（不含运行时，多来源基准一致），便携性好；唯一例外场景是离线精简 Win10（需捆绑 >250MB 的 Fixed Version 或引导安装器）。相比之下 .NET 路线的"运行时缺失"风险更大。

**9. Rust 学习曲线与复用成本**：所有权/借用检查对 C++ 老手也要数周-数月适应（常见共识：写小工具快，写无 panic 的长驻系统程序需重新建立习惯）。C++ 采集代码**不能编译进** Rust，但可通过 `extern "C"` FFI 复用（Rust 调 C ABI 很顺），代价是 cargo+MSVC 双构建系统、unsafe 边界审计；若不用 FFI 则全量重写。UI 层另需 HTML/JS/TS 技能栈——等于**第三门语言**。三层（Rust+JS+WebView2）调试链是两条路线中最长的。

## ④ 依赖清单 + 许可证
| 组件 | 版本线(2026-09 查证) | 许可证 |
|---|---|---|
| Avalonia / Avalonia.Desktop | 11.3.x（12 系列已出） | MIT |
| TreeDataGrid（大表官方推荐） | 随 Avalonia 11 | MIT |
| ScottPlot.Avalonia / ScottPlot.WinForms | 5.1.x | MIT |
| LiveChartsCore.SkiaSharpView.* | 2.0.x（免费版） | MIT（付费性能包另计） |
| .NET Desktop Runtime | 8 LTS(至 2026-11) / 10 LTS | MIT |
| tauri / tauri-build | 2.11.x | MIT OR Apache-2.0 |
| sysinfo | 0.39.x | MIT |
| windows (windows-rs) | 微软官方 | MIT OR Apache-2.0 |
| WebView2 Runtime | Win11 预装(Evergreen) | 系统组件（Fixed Version >250MB） |
| AG Grid Community / TanStack Virtual | 前端可选 | MIT |

## ⑤ 风险
1. **Avalonia**：内置 DataGrid 大集合已知性能问题（官方改推 TreeDataGrid）；11→12 大版本迁移成本待观察；自绘 Skia 栈在远程桌面/多显示器下的表现待验证。
2. **.NET 通用**：Win11 不预装现代运行时 → 便携分发实际只剩 self-contained（百 MB 级）或引导安装；NativeAOT 与 DataGrid/裁剪/C++/CLI 三者互斥，体积优化路径被封死一半。
3. **Tauri**：sysinfo 采集缺口 → 半数采集必须用 windows-rs/ETW 重写；1Hz 高频 IPC 需自建二进制协议；WebView2 渲染进程内存常驻 40-150MB+，与"资源管理器"自我克制定位相悖；提权窗口与 WebView2 生命周期交互「待验证」。
4. **通用**：两路线均引入 GC/JIT 或 JS 引擎的非确定性延迟，与 1 秒级确定性刷新目标存在张力（可缓解，不可消除）。

## ⑥ 参考链接
- Avalonia 性能/TreeDataGrid: https://docs.avaloniaui.net/troubleshooting/app-performance-issues ; https://docs.avaloniaui.net/docs/app-development/performance
- Avalonia DataGrid 变更全枚举问题: https://github.com/AvaloniaUI/Avalonia/issues/11724 ; 大表讨论: https://github.com/AvaloniaUI/Avalonia/discussions/20649
- Avalonia 许可: https://avaloniaui.net/legal/13 ; https://github.com/AvaloniaUI/Avalonia/discussions/11842
- ScottPlot: https://www.nuget.org/packages/ScottPlot.Avalonia/ ; https://scottplot.net/versions/
- LiveCharts2: https://livecharts.dev/ ; https://github.com/Live-Charts/LiveCharts2
- .NET 部署: https://learn.microsoft.com/en-us/dotnet/core/deploying/ ; NativeAOT 限制: https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/
- .NET 8 运行时不随 Windows 预装: https://github.com/dotnet/core/issues/6411
- NativeAOT×WinForms 未官方支持: https://github.com/dotnet/winforms/discussions/11257
- C++/CLI 迁移与限制: https://learn.microsoft.com/en-us/dotnet/core/porting/cpp-cli
- WPF UI 虚拟化: https://learn.microsoft.com/en-us/dotnet/desktop/wpf/advanced/optimizing-performance-controls
- WinForms DataGridView VirtualMode: https://learn.microsoft.com/en-us/dotnet/desktop/winforms/controls/virtual-mode-in-the-windows-forms-datagridview-control
- Tauri 2.0 发布(IPC 原始字节): https://v2.tauri.app/blog/tauri-20/ ; IPC 慢案例: https://github.com/tauri-apps/tauri/issues/4197
- sysinfo API(无按进程网络/句柄): https://docs.rs/sysinfo/latest/sysinfo/struct.Process.html
- WebView2 预装 Win11: https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution
- windows-rs: https://github.com/microsoft/windows-rs
