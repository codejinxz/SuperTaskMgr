# 文档索引

本目录（`docs/`）存放 SuperTaskMgr 的开发过程记录与当前有效文档。**这些是开发过程记录，可按需阅读**——你不必通读；要快速上手请看根目录 [README.md](../README.md)，要理解当前架构请看 [ARCHITECTURE.md](ARCHITECTURE.md)，准备接手开发请看 [HANDOVER.md](HANDOVER.md)。

## 目录结构

| 路径 | 内容 | 性质 |
|---|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | **当前有效**的架构说明（分层 / 线程 / 契约 / 数据流 / 关键决策 / 扩展点） | 有效文档 |
| [HANDOVER.md](HANDOVER.md) | 面向新接手开发者 / AI agent 的交接文档（索引 + 增量信息） | 有效文档 |
| `phase/` | 51 份开发过程记录：阶段报告、维护报告、评审/复核记录、设计规格 | 历史记录 |
| `research/` | 6 份技术调研报告（含官方引用与本机实测） | 历史记录 |

> `phase/` 与 `research/` 是**开发过程记录**，保留了每一轮的原始发现、证据与决策。其中的具体结论可能已被后续轮次修订——**以 `ARCHITECTURE.md` 与源码为准**。

---

## 推荐阅读路径

**新访客 / 用户**：根目录 [README.md](../README.md) → 完成。

**新接手开发者**：[README.md](../README.md) → [HANDOVER.md](HANDOVER.md)（5 分钟上手路径）→ [ARCHITECTURE.md](ARCHITECTURE.md) → 按模块 grep 历史评审记录。

**要改某个功能**：先按下表找到该主题的历史记录，**很多坑已经踩过并记录了修法**，避免重复踩坑。

---

## `docs/phase/` 主题索引

### 一、选型与架构（项目奠基）

| 文件 | 内容 |
|---|---|
| [00_技术选型评估报告.md](phase/00_技术选型评估报告.md) | 6 条技术路线（Qt6 / Win32+ImGui / WinUI3 / Avalonia / Tauri / 原生 Win32）对比与选型结论；采集层 API 基线；提权模型；合规工程化 |
| [00_阶段报告.md](phase/00_阶段报告.md) | 阶段 0 汇总（调研产出、交叉验证问题与修复、遗留风险） |
| [01_架构设计文档.md](phase/01_架构设计文档.md) | 原始架构设计（模块/线程/契约/提权模型/性能预算/并行开发边界）——**历史版本，当前状态见 [ARCHITECTURE.md](ARCHITECTURE.md)** |

### 二、阶段开发报告（阶段 0–4）

| 文件 | 内容 |
|---|---|
| [02_阶段报告.md](phase/02_阶段报告.md) | 阶段 2：核心功能开发（构建体系、采集层、操作层、界面层） |
| [03_阶段报告.md](phase/03_阶段报告.md) | 阶段 3：扩展功能开发（服务/启动项/驱动/网络/传感器 + 五个新页 + 告警） |
| [04_阶段报告.md](phase/04_阶段报告.md) | 阶段 4：集成、测试与打磨（性能预算终验、终审、交付物） |

### 三、维护轮报告（05–13，v1.0.0 的功能演进）

| 文件 | 主题一句话 |
|---|---|
| [05_维护报告.md](phase/05_维护报告.md) | 缺陷根治 + 传感器增强 + 功能评估（确认框点击无反应根因） |
| [06_维护报告.md](phase/06_维护报告.md) | 系统进程区分 + 传感器多值 + 挂起/恢复/窗口/崩溃记录等 F4 推荐落地 |
| [07_维护报告.md](phase/07_维护报告.md) | 关于/主题/壁纸 + 内存优化 + 图表可选 + 关闭行为 + 崩溃解析 + CPU 温度多源 |
| [09_维护报告.md](phase/09_维护报告.md) | 工具条/壁纸/关于三 bug 根治 + Logo + 内存加速可见性 + 注释中文化 |
| [10_维护报告.md](phase/10_维护报告.md) | 外观/关于按钮统一 + 网络适配器卡 + 图表 Phase A |
| [12_维护报告.md](phase/12_维护报告.md) | 兼容模式诊断体系 + 连接级网络监视器 + PawnIO 内核温度/风扇/电压 |
| [13_维护报告.md](phase/13_维护报告.md) | 图表 Phase B/C/D + Npcap 深度抓包 + 布局稳定化 |

> 注：`08_维护报告.md` 不存在（编号 08 保留给图表设计规格 `08_chart_design.md`）；`11` 保留给内核调研 `11_kernel_research.md`。

### 四、设计与调研规格

| 文件 | 内容 |
|---|---|
| [08_chart_design.md](phase/08_chart_design.md) | 性能页图表设计规格（R-ChartEval 评定稿，混合布局四阶段方案） |
| [11_kernel_research.md](phase/11_kernel_research.md) | 内核能力调研：MSR 温度 / SuperIO 风扇电压 / 真抓包的三条路线与本机探测 |
| [05_feature_recs_F4.md](phase/05_feature_recs_F4.md) | 功能扩展评估与推荐（对照 Process Explorer / Lasso / HWiNFO 的能力集，含"不做名单"） |

### 五、评审与复核记录（35 份 = V 编号 V1–V34 共 34 份 + F2 1 份）

工作模式：**每轮 = 并行开发 subagent（文件归属互斥）→ 集成 → 新一批互不知情的评审 subagent → 修复 → 复验**。评审记录是**发现记录**（含文件:行、证据、修法），维护/阶段报告是**汇总与决策**。V 编号中 `05_verify_V14` 与 `10_verify_V23` 是对特定 P0/P1 修复的**独立复核**报告，其余为各轮评审；另有一份 `05_review_F2` 为交互流专项扫描。

| 编号 | 文件 | 评审对象/维度 |
|---|---|---|
| V1–V3 | [00_review_V1](phase/00_review_V1.md) · [V2](phase/00_review_V2.md) · [V3](phase/00_review_V3.md) | 技术选型报告的交叉验证（正确性 / 风险完备性 / 需求覆盖） |
| V4–V5 | [01_review_V4](phase/01_review_V4.md) · [V5](phase/01_review_V5.md) | 架构设计文档（技术正确性 / 完备性·安全性·可测试性） |
| V6–V8 | [02_review_V6](phase/02_review_V6.md) · [V7](phase/02_review_V7.md) · [V8](phase/02_review_V8.md) | 阶段 2（正确性 / 资源泄漏与生命周期 / 安全防线与性能） |
| V9–V10 | [03_review_V9](phase/03_review_V9.md) · [V10](phase/03_review_V10.md) | 阶段 3（正确性与安全 / 一致性与生命周期） |
| V11–V13 | [04_review_V11](phase/04_review_V11.md) · [V12](phase/04_review_V12.md) · [V13](phase/04_review_V13.md) | 阶段 4 终审（回归正确性 / 泄漏终查 / 安全与声明一致性） |
| F2 | [05_review_F2](phase/05_review_F2.md) | 交互流"静默无效"类缺陷与遗漏 bug 扫描 |
| V14 | [05_verify_V14](phase/05_verify_V14.md) | 确认对话框"点击无反应"修复的独立验证 |
| V15–V16 | [06_review_V15](phase/06_review_V15.md) · [V16](phase/06_review_V16.md) | 维护轮 06（新功能正确性/安全 / 回归与生命周期） |
| V17–V18 | [07_review_V17](phase/07_review_V17.md) · [V18](phase/07_review_V18.md) | 维护轮 07（独立评审 / 回归与生命周期） |
| V19–V20 | [09_review_V19](phase/09_review_V19.md) · [V20](phase/09_review_V20.md) | 维护轮 09（三 bug 修复 / Logo / 注释中文化 / 数据层回归） |
| V21–V22 | [10_review_V21](phase/10_review_V21.md) · [V22](phase/10_review_V22.md) | 维护轮 10（图表 Phase A / 网络适配器 / 全仓回归） |
| V23 | [10_verify_V23](phase/10_verify_V23.md) | 独立复核（PlotRing 线性化 / 最小窗宽） |
| V24–V26 | [12_review_V24](phase/12_review_V24.md) · [V25](phase/12_review_V25.md) · [V26](phase/12_review_V26.md) | 维护轮 12（兼容诊断 / 网络监视器 / PawnIO 集成） |
| V27–V28 | [13_review_V27](phase/13_review_V27.md) · [V28](phase/13_review_V28.md) | 维护轮 13（图表 Phase B/C/D / 布局稳定化与全仓回归） |
| V29–V30 | [14_review_V29](phase/14_review_V29.md) · [V30](phase/14_review_V30.md) | UI 交互变更（状态栏锚点 / 列拖动重排 / 一键布局 / 传感器收缩） |
| V31–V33 | [15_review_V31](phase/15_review_V31.md) · [V32](phase/15_review_V32.md) · [V33](phase/15_review_V33.md) | 交付前全功能终验 / UI·UX 与历轮用户报告问题终验 |
| V34 | [16_review_V34](phase/16_review_V34.md) | U1 网络页纵向分栏 + v1.0.0 发布准备终审 |

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
- `docs/phase/`：**51 份**过程记录（其中评审/复核记录 **35 份**）；`docs/research/`：**6 份**。
