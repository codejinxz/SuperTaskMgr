# 文档索引

本目录（`docs/`）存放 SuperTaskMgr 的**设计规格**、**当前有效文档**与**开发过程归档**。你不必通读——要快速上手请看根目录 [README.md](../README.md)，要理解当前架构请看 [ARCHITECTURE.md](ARCHITECTURE.md)，准备接手开发请看 [HANDOVER.md](HANDOVER.md)。

## 目录结构

| 路径 | 内容 | 性质 |
|---|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | **当前有效**的架构说明（分层 / 线程 / 契约 / 数据流 / 关键决策 / 扩展点） | 有效文档 |
| [HANDOVER.md](HANDOVER.md) | 面向新接手开发者 / AI agent 的交接文档（索引 + 增量信息） | 有效文档 |
| `phase/` | **5 份设计规格**：技术选型评估、架构设计文档、功能建议、图表设计、内核调研 | 有效设计 |
| `archive/` | **46 份开发过程记录**：35 份评审/复核 + 11 份阶段/维护报告（索引见 [archive/README.md](archive/README.md)） | 历史归档 |
| `research/` | 6 份技术调研报告（含官方引用与本机实测） | 历史记录 |

> `phase/` 保留的是仍然有效的**设计规格**；`archive/` 与 `research/` 是**开发过程记录**，保留了每一轮的原始发现、证据与决策。过程记录中的具体结论可能已被后续轮次修订——**以 `ARCHITECTURE.md` 与源码为准**。

---

## 推荐阅读路径

**新访客 / 用户**：根目录 [README.md](../README.md) → 完成。

**新接手开发者**：[README.md](../README.md) → [HANDOVER.md](HANDOVER.md)（5 分钟上手路径）→ [ARCHITECTURE.md](ARCHITECTURE.md) → 按模块 grep [archive/](archive/) 中的历史评审记录。

**要改某个功能**：先按下表找到该主题的历史记录，**很多坑已经踩过并记录了修法**，避免重复踩坑。

---

## 一、设计规格（`docs/phase/`，5 份）

仍然有效的设计与调研规格，均为项目的奠基文档：

| 文件 | 内容 |
|---|---|
| [00_技术选型评估报告.md](phase/00_技术选型评估报告.md) | 6 条技术路线（Qt6 / Win32+ImGui / WinUI3 / Avalonia / Tauri / 原生 Win32）对比与选型结论；采集层 API 基线；提权模型；合规工程化 |
| [01_架构设计文档.md](phase/01_架构设计文档.md) | 原始架构设计（模块/线程/契约/提权模型/性能预算/并行开发边界）——**历史版本，当前状态见 [ARCHITECTURE.md](ARCHITECTURE.md)** |
| [05_feature_recs_F4.md](phase/05_feature_recs_F4.md) | 功能扩展评估与推荐（对照 Process Explorer / Lasso / HWiNFO 的能力集，含"不做名单"） |
| [08_chart_design.md](phase/08_chart_design.md) | 性能页图表设计规格（R-ChartEval 评定稿，混合布局四阶段方案） |
| [11_kernel_research.md](phase/11_kernel_research.md) | 内核能力调研：MSR 温度 / SuperIO 风扇电压 / 真抓包的三条路线与本机探测 |

---

## 二、开发过程归档（`docs/archive/`，46 份）

每一轮的实现范围、独立评审发现的问题及其修复证据都在这里——想知道"某个设计为什么是现在这样"或"某个缺陷怎么被发现和修掉的"，这里是原始依据。**索引与逐轮提要见 [archive/README.md](archive/README.md)。**

| 目录 | 数量 | 内容 |
|---|---|---|
| [archive/reviews/](archive/reviews/) | 35 | 独立评审与复验记录：`review_V1–V34`（34 份）+ `verify_V14`/`verify_V23`（2 份独立复核）+ `review_F2`（1 份交互流专项扫描） |
| [archive/reports/](archive/reports/) | 11 | 阶段报告（阶段 0–4，4 份）与维护报告（05–13，7 份）：每轮的汇总与决策 |

工作模式：**每轮 = 并行开发 subagent（文件归属互斥）→ 集成 → 新一批互不知情的评审 subagent → 修复 → 复验**。评审记录是**发现记录**（含文件:行、证据、修法），维护/阶段报告是**汇总与决策**。

---

## `docs/research/` 调研报告（6 份）

选型阶段的 6 条独立调研，全部含官方引用与本机实测：

| 文件 | 主题 |
|---|---|
| [R1_Qt6.md](research/R1_Qt6.md) | Qt 6 路线（Model/View、暗色/HiDPI、许可） |
| [R2_Win32_ImGui.md](research/R2_Win32_ImGui.md) | 原生 Win32 + Dear ImGui(docking) + D3D11 + ImPlot 全自绘路线 |
| [R3_WinUI3.md](research/R3_WinUI3.md) | WinUI 3 / C++/WinRT（Windows App SDK） |
| [R4_DotNet_Tauri.md](research/R4_DotNet_Tauri.md) | .NET（Avalonia/WPF/WinForms）与 Rust + Tauri 2 参照路线 |
| [R5_Collection_APIs.md](research/R5_Collection_APIs.md) | 数据采集层 API 逐项查证（权限矩阵与降级链） |
| [R6_Sensors_Drivers.md](research/R6_Sensors_Drivers.md) | 硬件传感器逐项可达性矩阵（用户态 / 内核驱动） |

---

## 数字口径说明

本文档与 `README.md` / `CHANGELOG.md` 中的数字均取自仓库实测：

- 自动化自测：**176 项**（`src/selftest/` 下 `STM_TEST` 宏注册数）。
- 主程序体积：**2,914,816 字节**（`build/Release/SuperTaskMgr.exe`）。
- 源码：`src/` 下 **7 个子目录**（`core` / `collect` / `ops` / `app` / `app/ui` / `app/ui3` / `selftest`），约 **39,600 行**；仓库总计 **17 个目录**（含仓库根，不含 `build*/`、`third_party/`、`.git/`）。
- 文档：`docs/phase/` **5 份**设计规格；`docs/archive/` **46 份**过程记录（其中评审/复核 **35 份**）；`docs/research/` **6 份**。
