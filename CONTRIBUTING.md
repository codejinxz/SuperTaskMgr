# 贡献指南

感谢你有兴趣为 SuperTaskMgr（Windows 超级任务管理器）做贡献。本文件说明如何搭建开发环境、本项目的代码与提交规范，以及提交 Pull Request 前的检查清单。

> 先读 **[README.md](README.md)** 了解产品形态，再读 **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** 了解当前架构；历史设计细节见 [docs/phase/01_架构设计文档.md](docs/phase/01_架构设计文档.md)。

---

## 1. 环境准备

| 项 | 要求 |
|---|---|
| 操作系统 | Windows 10 21H2+ / Windows 11 **x64**（仅 x64） |
| 编译器 | MSVC v143（VS2022 Build Tools，含 C++ 与 CMake 组件） |
| CMake | ≥ 3.20（VS 自带即可，构建脚本会自动定位） |
| 其他 | 无（imgui / implot / stb_image 已 vendoring 入库） |

```bat
:: 首次构建（默认 Release）
scripts\build.bat Release

:: 日常最省事：双击仓库根目录 一键编译.bat（编译 → 跑自测 → 询问是否启动）
```

---

## 2. 构建与测试

### 2.1 三个必过的验证门

任何改动在提交前必须**三条全过**（这也是本项目每一轮开发的验收门）：

```bat
:: ① 双配置零警告（/W4 全仓，0 error 0 warning）
scripts\build.bat Release
scripts\build.bat Debug

:: ② 自测 176 项全绿（退出码 = 失败数；--json 机器可读）
build\Release\stm_selftest.exe

:: ③ GUI 冒烟 + 六条输入注入回归（均 exit 0）
build\Release\SuperTaskMgr.exe --smoke 150
build\Release\SuperTaskMgr.exe --autotest dialogclick
build\Release\SuperTaskMgr.exe --autotest kill
build\Release\SuperTaskMgr.exe --autotest tree
build\Release\SuperTaskMgr.exe --autotest startup
build\Release\SuperTaskMgr.exe --autotest about
build\Release\SuperTaskMgr.exe --autotest wallpaper
```

### 2.2 验证前置条件（务必先读，避免假失败）

1. **跑 `--smoke` / `--autotest` 前必须关闭所有正在运行的 SuperTaskMgr 实例**——它持有单实例互斥体，headless 启动会按设计直接 `return 1`（防止 CI 被弹窗阻塞），表现为"六条 autotest 全失败"的假象。
2. 应用运行期间独占 `logs/stm.log`，外部工具无法打开——要看日志请先退出应用。
3. 提权实例无法被普通权限 shell 终止；遇 `build/` 目录被占用，先关闭应用，或换目录构建（`cmake -S . -B build_x -A x64`）。
4. Git Bash 调用 cmake 需 `MSYS_NO_PATHCONV=1`（否则 `/m` 等参数被路径转换破坏）；GUI 子系统 exe 的退出码建议用 PowerShell 获取。
5. 构建/验证会改动 `%LOCALAPPDATA%\SuperTaskMgr\` 下的 cfg/session——自动化验证前建议备份用户配置，验证后还原。

### 2.3 flaky 用例判定法

`collect_tick_latency`（tick p50 ≤15ms）对宿主负载极敏感，VM / 高负载时偶发假阴性。**判定方法**：若 tick 路径源码零改动（`git diff` 该目录为空）且基线二进制同期同样失败，即为环境性，可在 PR 中注明后放行；否则需排查。

---

## 3. 代码规范

### 3.1 语言与风格

- **注释用中文，标识符用英文**：这是本项目的既定风格（约 110 个文件的注释已全中文化）。新增注释请用中文；类/函数/变量/常量名用英文；UI 文案中文经 `U8()` 包装。
- C++20；`/W4 /permissive- /utf-8 /MP`；静态 CRT（`/MT`）；third_party 以 `/W0` 隔离。
- **全 Unicode**：一律宽字符 API（W 后缀），`std::wstring` 作为模块边界；中文路径与进程名全程正确。
- 文件编码 **UTF-8 无 BOM**。

### 3.2 分层与依赖纪律

- 依赖方向固定：`app → ops → core`、`app → collect → core`；`ops` 与 `collect` **互不依赖**（禁的是库依赖；ops 允许直调 OS API 做最小重枚举）。
- `collect` / `ops` **禁止 include ImGui 或任何 UI 头**。
- 破坏性操作必须走 `ops/ProcessOps.h` 的执行协议（见 §5）。

### 3.3 契约头冻结纪律（重要）

- `src/core/`、`src/collect/`、`src/ops/` 下的**契约头文件已冻结**。契约头由架构师写入，开发只实现对应的 `.cpp`。
- 确需变更契约头时，必须在当轮报告登记，并**同步更新相关的 selftest 断言**。
- `src/core/ProcData.h` 为全仓数据契约，改动牵动全局，需极谨慎；`src/app/AboutInfo.h` 是发布信息单一来源。

### 3.4 纯函数优先（测试策略）

所有可测逻辑——排序比较、解析、布局数学、配置键、状态机——应抽为 **header-only 纯函数**（多放在 `ui3/*.h` / `ui/*.h`），供 `stm_selftest` 直接断言。这是本项目 176 项用例能覆盖 UI 逻辑的原因。**新增功能必须同时增加纯函数与其用例。**

### 3.5 资源与线程章程

- 句柄一律 RAII（`core` 提供 `HandleGuard`）；COM 在 ops 线程内走 `CoInitializeEx(STA)` + `CoUninitialize` 配对。
- 跨线程无裸指针：job lambda 一律**按值捕获** `shared_ptr<AppContext>`。
- UI 线程禁阻塞调用；采集线程禁调 UI API。
- **进程身份 = `(PID, CreateTime)` 二元组**（`ProcKey`）：PID 会被复用，任何跨帧引用、终止前校验、树杀枚举都必须比对 `createTime`（±1s 容差）。这是全项目最常被违背的规则。

---

## 4. 提交信息约定

采用简化版的 [Conventional Commits](https://www.conventionalcommits.org/) 风格，首行写明类型与范围，正文说明**为什么**改（而非复述 diff）：

```text
<类型>(<范围>): <简明描述>

<可选的详细说明：动机、影响的模块、已知副作用>
```

- 类型：`feat`（新功能）、`fix`（缺陷修复）、`refactor`（重构）、`docs`（文档）、`test`（测试）、`build`（构建/依赖）、`perf`（性能）、`chore`（杂项）。
- 范围建议用模块名，如 `collect`、`ops`、`ui`、`ui3`、`docs`。
- 破坏性变更在类型后加 `!`（如 `feat(core)!: ...`），并在正文说明迁移方式。
- 涉及诚实性/权限/保护名单等红线的改动，正文须说明其对红线的保持方式。

示例：

```text
fix(ui): 确认模态改为每帧 BeginPopupModal，修复点击被吞

单帧渲染会留下"僵尸模态"吞掉后续点击；顺带移除非 IsWindowAppearing 的
每帧 SetKeyboardFocusHere（会偷走鼠标按住按钮的 ActiveId）。
附 --autotest dialogclick 回归哨兵。
```

---

## 5. 红线（改代码时不得违背）

1. **诚实数据**：拿不到就显示 `—` / "需提权" / "需驱动"，**绝不显示 0 或假值冒充**。
2. **零持久化 / 零隐蔽 / 零联网上传**：不写自启动、不驻留服务、无监听端口、不上传任何数据。
3. **破坏性操作双层防线**：UI 二次确认 + ops 层保护名单硬拒绝；保护名单内进程不可杀、不可挂起、不可 trim。
4. **内核组件全 opt-in**：本体零驱动；可选增强依赖用户自行安装的官方签名运行时，不捆绑、不分发、不自动安装。
5. **全 Unicode**：宽字符 API，中文路径与进程名全程正确。

新增破坏性操作必须照 `src/ops/ProcessOps.h` 头部的协议注释执行：**身份重验 → 保护名单硬门 → （树操作）两段式**。

---

## 6. Pull Request 检查清单

提交 PR 前逐项确认：

- [ ] 改动范围聚焦，未触碰与本次目标无关的文件。
- [ ] 未修改 `src/**` 的契约头，或已按 §3.3 登记并同步了 selftest。
- [ ] Release 与 Debug 双配置 **0 错误 0 警告**。
- [ ] `stm_selftest` **176/176 全绿**（新增功能已补纯函数用例）。
- [ ] `--smoke 150` exit 0；**六条 `--autotest` 全 PASS**。
- [ ] 新增/修改的行为符合 §5 五条红线；破坏性操作走完 §5 执行协议。
- [ ] 注释为中文、标识符为英文；文件为 UTF-8 无 BOM。
- [ ] 若用户可见行为或数字有变化，已同步更新 `README.md` / `CHANGELOG.md`（数字须与实测一致）。
- [ ] 若涉及环境性失败或无法验证项，已在 PR 描述中如实说明（不得粉饰）。
- [ ] 提交信息符合 §4 约定；未包含 `build/`、`release/`、`*.log` 等构建产物。

---

## 7. 获取帮助

- 有疑问或功能建议：开 [Issue](https://github.com/codejinxz/SuperTaskMgr/issues)。
- 安全相关问题：见 **[SECURITY.md](SECURITY.md)**（请勿在公开 Issue 中报告漏洞细节）。
- 想了解某个模块的历史决策与踩过的坑：在 `docs/phase/` 中按模块名 grep 历史评审记录（很多坑已记录修法）。
