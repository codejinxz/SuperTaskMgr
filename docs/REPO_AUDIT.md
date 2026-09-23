# 仓库非源码文件审计报告（REPO_AUDIT）

> 审计人：审阅者 C｜日期：2026-09-23｜范围：`D:\coding_files\memory` 全仓非源码条目
> 方法：`ls -A` 全量 + `git ls-files`（527 条）+ `du -sh` 体积 + 引用关系 grep + 敏感信息扫描
> 说明：本文件仅给出判定与命令；**未删除任何文件、未执行任何 git 操作**。执行由集成者按清单决策。
>
> **执行状态（2026-09-23 收尾）**：§3.1 方案 A 已落地——46 份过程产物已归档至 `docs/archive/reviews/`（35 份）与 `docs/archive/reports/`（11 份），`docs/phase/` 顶层保留 5 份设计规格，新增索引 `docs/archive/README.md`；本文件中指向已移动文件的路径引用已同步更新为 `docs/archive/...`。§1.2 所称 `docs/README.md` / `docs/ARCHITECTURE.md`「缺失」亦已由并行 agent 补齐。

---

## 0. 审计统计

| 类别 | 数量 | 体积 | 备注 |
|---|---|---|---|
| 根目录条目（`ls -A`，含 `.git`） | 18 | — | 见 §1 |
| git 跟踪文件总数 | 527 | — | `git ls-files` |
| 其中 `src/**` | 157 | 2.0M | 源码，禁删 |
| 其中 `third_party/**` | 302 | 9.6M | vendored 依赖，禁删（1 例外见 §2） |
| 其中 `docs/**` | 58 | 828K | 审计重点 |
| 其中 `assets/` | 2 | 208K | `app.ico` + `logo_256.png` |
| 其中 `scripts/` | 2 | 5.0K | `build.bat` + `make_logo.bat` |
| 其中 `tools/` | 1 | 28K | `gen_logo.cpp` |
| 其中根元文件 | 6 | — | `.gitignore` `CMakeLists.txt` `LICENSE` `README.md` `一键编译.bat`（+ 新增 3 份，见 §4） |
| `docs/phase/` | 51 | 700K | 32 review_V + 2 verify_V + 1 review_F + 4 阶段报告 + 7 维护报告 + 5 设计规格 |
| `docs/research/` | 6 | 92K | R1–R6 技术调研 |
| 未跟踪磁盘目录 `build/` | — | 93M | 已被 `.gitignore` 排除 |
| 未跟踪磁盘目录 `build_a/` | — | 93M | **残留重复构建目录**（见 §2） |
| 未跟踪磁盘目录 `release/` | — | 4.2M | 已被 `.gitignore` 排除 |

未跟踪/改动（`git status --porcelain -uall`）：` M README.md`、`?? CHANGELOG.md`、`?? CONTRIBUTING.md`、`?? SECURITY.md`（后三者为并行 agent 新增，应在提交前纳入或明确归属）。

---

## 1. 全量清单（路径 | 类别 | 判定 | 理由 | 影响）

### 1.1 根目录

| 路径 | 类别 | 判定 | 理由 | 影响 |
|---|---|---|---|---|
| `.git/` | VCS | 保留 | 仓库本体 | — |
| `.gitignore` | 配置 | 保留 | 硬约束禁删 | — |
| `CMakeLists.txt` | 构建 | 保留 | 硬约束禁删；`scripts/build.bat` 依赖 | — |
| `LICENSE` | 合规 | 保留 | 硬约束禁删；MIT，公开仓库必需 | — |
| `README.md` | 文档 | 保留（**修改**） | 硬约束禁删；存在 2 处失效引用（§4） | 用户第一印象 |
| `一键编译.bat` | 构建 | 保留 | 硬约束禁删 | — |
| `assets/app.ico` | 资产 | 保留 | `src/app/app.rc:5` 引用，禁删 | 删则构建失败 |
| `assets/logo_256.png` | 资产 | 保留 | `src/app/app.rc:6` 引用，禁删 | 删则构建失败 |
| `scripts/build.bat` | 构建 | 保留 | 硬约束禁删 | — |
| `scripts/make_logo.bat` | 构建 | 保留 | 再生 `assets/*` 的唯一入口 | 删则无法再生图标 |
| `tools/gen_logo.cpp` | 源码 | 保留 | 硬约束禁删（图标生成器源码，非产物） | — |
| `src/**` | 源码 | 保留 | 硬约束禁删 | — |
| `third_party/**` | 依赖 | 保留 | 硬约束禁删；`CMakeLists.txt:44-65` 引用，离线构建必需 | 删则无法构建 |
| `CHANGELOG.md` | 文档 | 保留（**未跟踪**） | README §文档 引用；公开仓库加分项 | 需 `git add` |
| `CONTRIBUTING.md` | 文档 | 保留（**未跟踪**） | 公开仓库标准件，加分 | 需 `git add` |
| `SECURITY.md` | 文档 | 保留（**未跟踪**） | 声明安全模型/信任边界，公开仓库加分 | 需 `git add` |
| `build/` | 产物 | **删除（磁盘）** | 93M 构建产物，已 ignore，不入库；保留只会本地占体积 | 无（可重新生成） |
| `build_a/` | 产物残留 | **删除（磁盘）** | 93M **重复构建目录**，非预期（`scripts/build.bat` 只生成 `build/`），疑为并行 agent 遗留 | 无 |
| `release/` | 产物 | 保留（已 ignore） | 发布产物（exe+zip+notes.md），经 GitHub Release 分发，不入库；`notes.md` 内容与 README 重复但无害 | 无 |

### 1.2 `docs/` 顶层

| 路径 | 类别 | 判定 | 理由 | 影响 |
|---|---|---|---|---|
| `docs/HANDOVER.md` | 文档 | 保留 | 硬约束禁删；24K，295 行，含构建/测试/评审索引，接手价值高 | — |
| `docs/README.md` | 文档 | **缺失 → 建议创建** | README.md:332 与 §文档 均链接此文件，但**文件不存在**（硬约束亦要求保留=应存在） | 公开仓库死链 |
| `docs/ARCHITECTURE.md` | 文档 | **缺失 → 建议创建** | README.md:238/266/331 与 CONTRIBUTING 均链接，但**文件不存在**（硬约束要求存在） | 公开仓库死链（3 处） |

### 1.3 `docs/phase/`（51 份，700K）—— 逐类判定

| 路径（模式） | 数量 | 类别 | 判定 | 理由 |
|---|---|---|---|---|
| `00_技术选型评估报告.md` | 1 | 设计规格 | **保留** | 6 路线选型结论，公开仓库核心加分项；被 README/HANDOVER 引用 |
| `01_架构设计文档.md` | 1 | 设计规格 | **保留** | 模块/线程/契约/提权模型；HANDOVER:292 指定为必读 |
| `05_feature_recs_F4.md` | 1 | 设计规格 | 保留 | 功能建议与价值评估，非过程流水 |
| `08_chart_design.md` | 1 | 设计规格 | 保留 | 图表设计规格；HANDOVER:293 建议改图表前必读 |
| `11_kernel_research.md` | 1 | 设计规格 | 保留 | 内核访问调研（含测试签名等风险说明） |
| `*_review_V1..V34.md` | 32 | 过程产物 | **归档**（见 §3） | 逐轮评审记录；对公开仓库是噪音，对可追溯性有价值——建议移入 `docs/archive/` 而非删除 |
| `*_verify_V14,V23.md` | 2 | 过程产物 | **归档** | 同上（复验记录） |
| `05_review_F2.md` | 1 | 过程产物 | **归档** | 同上（F2 轮评审） |
| `00/02/03/04_阶段报告.md` | 4 | 过程产物 | **归档** | 交付与交叉审查记录；含实质决策，但属开发流水 |
| `05/06/07/09/10/12/13_维护报告.md` | 7 | 过程产物 | **归档** | 维护轮次报告；含**过时路径**（§4），归档后不误导新读者 |

> 统计：**35 份评审/复核 + 11 份阶段/维护 = 46 份过程产物（532K+76K）**，占 `docs/phase` 体积约 87%，是"公开仓库显得杂乱"的主因。

### 1.4 `docs/research/`（6 份，92K）

| 路径 | 判定 | 理由 |
|---|---|---|
| `R1_Qt6.md` | 保留 | 技术调研含官方引用，公开仓库加分项；README/HANDOVER 引用 |
| `R2_Win32_ImGui.md` | 保留 | 最终选型的证据源 |
| `R3_WinUI3.md` | 保留 | 否决路线的证据 |
| `R4_DotNet_Tauri.md` | 保留 | 否决路线的证据 |
| `R5_Collection_APIs.md` | 保留 | 采集层 API 矩阵，实用 |
| `R6_Sensors_Drivers.md` | 保留 | 传感器可达性调研，实用 |

### 1.5 `third_party/`（302 份，9.6M）—— 仅审例外

| 路径 | 判定 | 理由 |
|---|---|---|
| `third_party/build_log.txt` | **删除** | **24 字节垃圾文件**，内容为 `系统找不到指定的路径。`（GBK 乱码），是构建脚本误重定向的残留，无任何引用（`grep build_log` 零命中），非 vendored 依赖组成部分 |
| `third_party/SHA256SUMS.txt` | 保留 | vendored 包校验和，README:§依赖 引用 |
| `third_party/stb/SHA256SUMS.txt` | 保留 | 同上 |
| `third_party/imgui/{docs,examples,misc,.github}` | 保留 | 上游 vendoring 完整镜像（8.5M 主体）；构建只用 `*.cpp` + `backends/`，其余为上游原样内容。**不建议裁剪**——破坏"与上游一致可核对 SHA256"的可验证性，且体积（9.6M）对 git 仓库无压力 |
| `third_party/implot/{example,.github,TODO.md}` | 保留 | 同上 |

---

## 2. 必须删除清单（含精确命令）

| # | 目标 | 体积 | 理由 | 命令 |
|---|---|---|---|---|
| D1 | `third_party/build_log.txt` | 24 B | 构建误重定向残留（内容=乱码错误串），零引用，非 vendored 内容 | `rm -f /d/coding_files/memory/third_party/build_log.txt` |
| D2 | `build_a/`（磁盘） | 93M | 非预期重复构建目录，`scripts/build.bat` 只产出 `build/`；已 ignore 不入库，纯本地垃圾 | `rm -rf /d/coding_files/memory/build_a` |
| D3 | `build/`（磁盘，可选） | 93M | 构建产物，已 ignore；保留可本地复用，删除可释放 93M。**不影响仓库** | `rm -rf /d/coding_files/memory/build` |

> 合规复核：**全仓未发现 Npcap 安装包**（`git ls-files` 与 `git rev-list --all --objects` 均无 `npcap*.exe`/`pcap-1.*`），此前删除**未回归**；`redist/` 目录在磁盘上**不存在**，`.gitignore` 已含 `redist/` 规则。合规通过。

---

## 3. 建议删除/合并清单（可执行方案）

### 3.1 推荐方案 A（**归档而非删除**，保可追溯性 + 去杂乱）★推荐

将 46 份过程产物移出 `docs/phase/` 顶层，改为归档目录 + 索引，`docs/phase/` 只留 5 份设计规格：

```bash
cd /d/coding_files/memory/docs/phase
mkdir -p ../archive/reviews ../archive/reports
mv *_review_V*.md *_verify_V*.md *_review_F*.md ../archive/reviews/
mv *_阶段报告.md *_维护报告.md ../archive/reports/
```

- 建议新文件名/结构：`docs/archive/reviews/`（35 份，原文件名保留，便于按 V 号溯源）、`docs/archive/reports/`（11 份）。
- 建议新增索引 `docs/archive/README.md`，保留的核心信息：**按 V 号一行一条**——`V号 | 日期 | 审查对象 | 结论(P0/P1/P2 数) | 已修/未修`，并注明"以下为开发过程记录，最新有效设计见 `docs/ARCHITECTURE.md` / `docs/phase/01_架构设计文档.md`"。
- 效果：`docs/phase/` 由 51 份 → 5 份，公开仓库首屏不再被 46 份流水淹没；溯源能力无损（HANDOVER §9 的"改模块前 grep 历史评审"仍可用，只需改路径前缀）。
- 同步修改：`docs/HANDOVER.md:124/205/293`、`README.md`（§文档）、`CHANGELOG.md` 中的 `docs/phase/` 字面路径改为 `docs/archive/`。

### 3.2 备选方案 B（**硬删除**，最简公开仓库）

```bash
cd /d/coding_files/memory/docs/phase
rm -f *_review_V*.md *_verify_V*.md *_review_F*.md *_阶段报告.md *_维护报告.md
```

- 替代方案：先按 §3.1 生成一份 `docs/PHASE_HISTORY.md`（把 46 份的 V 号/日期/对象/结论汇总为一张表，约 100 行），再删除 46 份原始文件。保留最核心信息：缺陷编号、影响面、修法一句话、复验结论。
- 代价：失去逐份细节与"互不知情评审"的原始证据链；对公开项目**可接受**（README 只需说"v1.0.0 经 34 轮独立评审"）。

### 3.3 不建议删除的对照项（说明为何保留）

`docs/phase/` 5 份设计规格、`docs/research/` 6 份、`docs/HANDOVER.md`、`third_party/**`。理由：设计/调研文档是公开仓库的**加分项**（证明选型有据），过程流水才是**减分项**；二者边界不应混淆。

### 3.4 需修补的死链（非删除）

| 位置 | 问题 | 建议 |
|---|---|---|
| `README.md:80` | 引用 `docs/phase/02、03_阶段报告.md`（**不存在**；实际为 `02_阶段报告.md` + `03_阶段报告.md`） | 改为两份链接；若采纳方案 A 则改为 `docs/archive/reports/...` |
| `README.md` §文档（3 处） | 引用 `docs/ARCHITECTURE.md`、`docs/README.md`（**均不存在**） | 创建这两份（硬约束也要求其存在）；或删除链接 |
| `README.md:85` 区域 / `336` 区域 | `redist/` 说明段出现两次且措辞不同（并行 agent 改动中） | 合并为一段 |
| `docs/HANDOVER.md:119` | 称 `assets/` 含"各尺寸预览"（`assets/preview_*.png` 已 gitignore、磁盘无） | 删去"各尺寸预览"字样 |
| `docs/HANDOVER.md:5` | 称"**19 个提交**，尚未配置远端"（实际 **23** 个提交） | 更新为 23 或删去具体数字 |

---

## 4. 必须补充的 .gitignore 规则

现有 `.gitignore` 已覆盖：`build*/`、`out/`、`.vs/`、`*.user`、`vcpkg_installed/`、`*.pdb/ilk/obj/log`、`redist/`、`*.exe.bak`、`tools/gen_logo.exe`、`tools/*.obj`、`assets/preview_*.png`、`build_log*.txt`、`*_selftest*.json`、`*.new`、`imgui.ini`、`dev/`、`selftest*.json`、`release/`。**覆盖良好**，仅补以下缺口（实测均 NOT-ignored）：

```gitignore
# IDE / 工具副产物
.vscode/
.idea/
.zcode/
CMakeUserPresets.json
*.tlog
*.pch
*.ipch
*.aps
Debug/
captures/
*.suo
*.VC.db
```

> 说明：`captures/` 是程序运行时输出目录（`%LOCALAPPDATA%\SuperTaskMgr\captures`，见 `src/app/ui3/Pages3.cpp:604`），仓库根若出现同名目录应忽略。`build_a/` 已被 `build*/` 覆盖，无需新增。根目录 `*.zip` 建议忽略（避免误提交发布包）：`*.zip`。

---

## 5. 体积与一致性发现

**体积异常项**
1. `build/` 93M 与 `build_a/` 93M —— 后者为**意外重复目录**（唯一真正的体积异常）；两者均不入库。
2. `third_party/imgui` 8.5M 中 2.3M 为 `examples/`、844K 为 `docs/` —— 构建不使用，但属上游 vendoring 完整性，建议保留（见 §1.5）。
3. `docs/phase` 700K 中 608K（87%）为 46 份过程产物 —— 采纳 §3.1 后 `docs/` 由 828K 降至约 220K。

**文档内部一致性**
1. `docs/archive/reviews/16_review_V34.md:5/85/94` 与 `docs/archive/reports/13_维护报告.md:15` 仍写 `redist/npcap-1.89.exe`、"手动双击 `redist\npcap-1.89.exe` 默认安装" —— **该路径已不存在**（合规删除后）。属历史记录，已随方案 A 归档至 `docs/archive/`，与 `docs/phase/` 顶层隔离。
2. `README.md` 与 `docs/HANDOVER.md` 对评审份数表述不一（README "34 份独立评审" vs HANDOVER "35 份评审/复核" vs 实际 32 review_V+2 verify_V+1 review_F=35）—— 建议统一为"35 份评审/复核（V1–V34 + F2/V14/V23）"。
3. `docs/HANDOVER.md:5` 提交数（19）与实际（23）不符。
4. `third_party/build_log.txt` 与 vendoring 无关，是**唯一的内容损坏文件**（乱码）。
5. `README.md` §截图 为"_待补_"占位 + HTML 注释块（`assets/screenshot-*.png` 不存在）—— 建议补拍或删除占位块，避免公开仓库出现"待补"。

---

## 6. 敏感信息扫描结果

| 检查项 | 结果 |
|---|---|
| Token/密钥（`ghp_`/`sk-`/`AKIA`/`xox`/`BEGIN * PRIVATE KEY`/`password=`） | **零命中** |
| 私人邮箱（`@gmail/@qq/@163` 等） | 零命中 |
| 机器名/用户名硬编码 | 零命中（`admin` 仅出现为 git 提交 author 与文件系统所属，不在文件内容中） |
| 本地绝对路径（**文档内**） | **2 处命中，低危**：`docs/archive/reviews/04_review_V13.md:3`（`D:\coding_files\memory 全仓`）、`docs/archive/reviews/07_review_V18.md:40`（`D:\tmp_stm_v18\probe.cpp`）。已随方案 A 归档，与 `docs/phase/` 顶层隔离 |
| 本地绝对路径（**源码内**） | 6 处命中，**无害**：`src/selftest/{wallpaper,control,ui_memcleanup,ui_gc}_test.cpp` 中的 `D:\pics\wall.png`、`D:\games\game.exe` 等均为**测试夹具字符串**，非真实隐私；`src/app/ui3/Wallpaper.cpp` 为路径解析逻辑分支。无需处理 |
| 其他（日志中的隐私值、设备 ID、序列号） | 零命中；文档中出现的 `stm_f1_autotest`、`autotest_result.log` 为自测产物名，非敏感 |

> 结论：**无敏感信息泄露风险**。仅建议对归档后的 2 处 `D:\` 路径做脱敏。

---

## 7. 给用户的取舍推荐（结论）

**推荐采纳方案 A（归档，不删除）。** 具体：`docs/phase/` 顶层只保留 5 份设计与调研规格（`00_技术选型评估报告`、`01_架构设计文档`、`05_feature_recs_F4`、`08_chart_design`、`11_kernel_research`），把 46 份过程流水（35 份 `*_review_V*/_verify_*/_review_F*` + 11 份 `*_阶段/维护报告`）整体移入 `docs/archive/reviews/` 与 `docs/archive/reports/`，并新增 `docs/archive/README.md` 索引（一行一 V 号：对象/结论/修复状态）。理由：这 46 份是**开发过程产物**，留在 `docs/phase/` 顶层会让公开仓库首屏显得杂乱、并把陈旧路径（如已移除的 `redist/npcap-1.89.exe`）暴露给新读者；但直接硬删除会丢失"34 轮互不知情评审"这一**差异化卖点**的证据链。归档方案以 **1 份索引 + 2 个目录**换取"顶层干净 + 溯源完整"，成本仅几条 `mv` 与链接改写。若你更看重极简，可执行方案 B（先汇总 `docs/PHASE_HISTORY.md` 再删除原文）；若你更看重零改动，则至少执行 §2 的 D1（删 `third_party/build_log.txt`）与 D2（删 `build_a/`），并补齐 §4 的 `.gitignore` 规则、修复 §3.4 的 5 处死链——这些是**无争议**的最小动作。

**最终决策清单**：必删 `third_party/build_log.txt`、`build_a/`；必补 `.gitignore` 9 条 + `*.zip`；必修 5 处死链（尤以 README 指向的 `docs/ARCHITECTURE.md`/`docs/README.md` 缺失为最严重）；建议归档 46 份过程产物；合规无回归（无 Npcap 安装包）；无敏感信息泄露。
