# 04 安全防线终审 + 声明一致性（终审者 V13）

日期：2026-09-18。范围：D:\coding_files\memory 全仓。方法：代码复核（ProtectedList / ProcessOps / Pages / Pages3 / StartupOps / ServiceOps / NetTables / Sensors / Log / Str / main / Elevate）+ 全仓 grep + 实跑 `build\Release\stm_selftest.exe --json`（40/40 通过，当前环境非管理员）。禁止 git，未修改任何源文件。

## 统计

| 级别 | 数量 |
|---|---|
| P0 | 0 |
| P1 | 0 |
| P2 | 4 |

---

## P0

无。

## P1

无。

## P2

### P2-1 备份文件名的 DOS 设备名边缘可造成"备份成功"假象（契约偏差，数据损失极小）
- 证据：`src/ops/StartupOps.cpp` L140-152（SanitizeId 只清洗 `\/:*?"<>|` 与 <0x20，不处理设备名）+ L262（`dir\SanitizeId(id).<ts>.txt`）+ 契约 `src/ops/StartupOps.h` L41 "Backup failure => refuse write"。若启动项 id 清洗后主干恰为 CON/NUL/AUX/PRN/COM1-9/LPT1-9（可由恶意安装器构造 Run 值名或任务名），`_wfopen_s` 在经典命名规则下可能命中设备对象并"成功"，备份未持久化但写入继续 —— 与"备份失败拒绝写入"契约不符。实际可丢数据极小：多数场景仅 12 字节 StartupApproved 原值（首次禁用时"原值不存在"）、UWP State DWORD；最重为设备名任务（WriteTaskXmlBackup L281-301）丢 XML 备份，而重注册只改 `settings->Enabled`（L798-808），定义本身回写自活动任务。
- 修法：备份名统一加固定前缀（如 `stm_<id>.`），或 SanitizeId 末尾对设备名主干追加前缀/直接拒绝该备份并返回失败。

### P2-2 ETW 崩溃孤儿会话不被清扫（声明对"正常退出"成立）
- 证据：`src/collect/NetTables.cpp` L300-307：会话名 `SuperTaskMgr-Net-<pid>`，Start() 仅按同名（同 pid）反孤儿；进程崩溃/被杀时析构 Stop() 不会执行，管理员 + 用户已显式开启 ETW 的前提下，Kernel-Network 会话遗留到重启（实时会话，64×64KB 缓冲）。README"会话名唯一且退出即清理"对正常退出成立，崩溃路径未覆盖。
- 修法：启动会话前枚举并停止全部 `SuperTaskMgr-Net-*` 前缀会话；或在 README 已知限制中如实补充"异常终止时 ETW 会话可能遗留"。

### P2-3 UI 门与 ops 门的保守性不对称（方向安全，非绕过，建议文档化）
- 证据：采集徽标与菜单禁用用 `core::ProtectedReason`（纯名单，`src/app/ui/Pages.cpp` L824-838、`src/collect/ProcessCollector.cpp` L353），而 ops 执行门 `ops::ProtectedReason`（`src/ops/ProcessOps.cpp` L482-501，V8-P2）对"路径已知且在 Windows 目录外的同名进程"放行。后果：用户目录下的伪造 csrss.exe 在 UI 永远显示 P 徽标且终止菜单被禁用，但 ops 层允许 —— 过阻断（保守），绝不欠阻断。自测 `ops_trim_same_name_allowed` 证实 ops 层语义。
- 修法：保持现状（安全方向），在架构文档 §6 注明两层判定差异；或采集层补路径解析使徽标精确（成本：逐进程 QueryFullProcessImageName）。

### P2-4 README 许可证表声明"本项目代码 MIT"但仓库无 LICENSE 文件
- 证据：README.md L53；根目录仅有 .gitignore / CMakeLists.txt / README.md / docs / scripts / src / third_party（`find -iname "LICENSE*"` 仅命中 third_party/imgui）。
- 修法：补 MIT LICENSE 文本，或将声明改为"仅源码内声明"。

---

## 防线终判（任务 1：攻击路径逐条）

| 攻击路径 | 判定 | 关键证据 |
|---|---|---|
| 同名非系统路径进程（如用户目录 csrss.exe） | 无绕过 | ops 门按路径放行（正确，非系统进程）；真系统目录内关键进程恒硬拒（IsUnderSystemRoot 保守失败时按"在系统目录"处理）；UI 层更保守直接禁用 |
| 树杀根 = 保护进程 | 无绕过 | TerminateTree order 含 root，走同一成员门 → 跳过 + skippedProtected 计数 + 报告（L388-397）；UI 层入口本就被 BeginDisabled 挡住 |
| 树中混保护成员 | 无误伤 | 执行期重拍快照，逐成员以名单（快照名，路径未知=保守）判定，跳过并报告，其余成员照常（L389-397） |
| TrimWorkingSet | 无绕过 | V8-P1-1 门在位（L427-432）；自测 ops_trim_protected_refusal 通过 |
| PID 复用窗口 | 无绕过 | 门解析的是"当前 pid 的现身份"（误判方向=多拒绝）；OpenVerified create time ±1s 重验（L263-272）；树收养要求子进程创建时间 ≥ 父（L212）阻断孤儿；句柄存活期间内核对象不可复用 |
| 服务侧同名绕过 | 无绕过 | ServiceProtectedReason（name+".exe"）在 ControlService 之前硬拒；递归停依赖同样逐个过门（ServiceOps.cpp L140） |

**终判一句话：双层（UI 禁用+两段确认 / ops 硬门+身份重验）与纵深（执行期重拍快照、逐成员门、路径反伪造、先备份后写）防线成立，未发现任何 P0/P1 级残留绕过；唯一偏差是 UI 比 ops 更保守（安全方向）。**

## 附：其余核查结论

- **恶意输入面（任务 2）**：全仓 Fmt 格式串均为字面量（grep 证实无非字面量格式串），中文/引号/换行/超长仅作参数 —— 无格式注入；ImGui 全部 `%s`/TextUnformatted，U8() 缓存（8192 上限，帧内使用）无悬垂；WideToUtf8 对孤立代理输出替换字符不崩溃，Utf8ToWide 失败返回空按缺数据处理；SanitizeId 覆盖非法字符 + >150 截断加 FNV 标记（设备名边缘见 P2-1）；注册表值名按精确长度二进制传递，无注入面；`core_cfg_roundtrip`（中文+引号）自测通过。
- **无隐蔽/持久化/规避（任务 3）**：无 socket/listen/WinHttp/WinInet（ws2_32 仅动态解析 InetNtopW 做地址格式化）；无 WriteProcessMemory/CreateRemoteThread/VirtualAllocEx/SetWindowsHookEx；Run 键只枚举不写自身，计划任务仅 GetTask→put_Enabled 或 TASK_UPDATE 重注册（无 TASK_CREATE/CreateServiceW 路径，确认"只允许修改已有项 Enabled"）；LoadLibrary 仅系统 DLL（nvml 为绝对路径 System32）；提权仅显式 ShellExecuteExW "runas"；ETW 默认关 + read-back 诚实回滚 + Stop 幂等/析构清理，与声明一致（崩溃边缘见 P2-2）。
- **README 声明（任务 4）**：40 项自测实跑 40/40 通过；单文件/静态 CRT（CMakeLists L12 MultiThreaded）/1.7MB（1,733,632 B）/imgui 1.92.9/implot 1.0/docs/research R1-R6 全部吻合；GPU 温度"AMD/Intel 未接入"与 Sensors.cpp（NVML-only，注释"IGCL 接口复杂，明确跳过"）一致；风扇 NeedDriver、PDH 缺失显示"—"一致；**"日志不记录命令行/窗口标题"全仓成立**（全部 STM_LOG 载荷 grep 无 cmdLine/windowTitle；唯一"窗口"命中是 main.cpp L106"窗口创建失败"；启动项命令 hex 只进本地备份文件，不进日志，声明不冲突）。
- **权限矩阵（任务 5，非管理员环境实核）**：服务页 `ctx.elevated` 门 + "（需提权）"后缀与 ops ACCESS_DENIED 文案（"需要管理员权限（...）"）一致；启动项 canToggle 门（HKLM/公共文件夹=elevated）与 ops"修改该项需要管理员权限"一致；驱动页 24H2+ 降级页文案经 DriverErrNeedsAdmin 与 DriverOps 错误串"需要管理员权限（Windows 11 24H2 起...）"精确匹配（本机 22631 非提权实枚举 226 驱动，符合"24H2+ 才需提权"表述）；传感器 NeedAdmin 态 + 提权按钮一致；ETW 开关失败 toast"需要管理员权限"与实跑（StartTraceW 拒绝）一致；非管理员的他人进程路径/命令行显示"不可用（无读取权限）"诚实降级。
