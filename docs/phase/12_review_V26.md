# V26 终审报告（PawnIO 内核集成 + 全仓回归）—— 2026-09-20

评审人：终审 subagent V26（与其他评审者互不知情；禁 git；除本报告未改任何文件）。
对象：`src/collect/PawnIoLink.h/.cpp`（IOCTL 0x821/0x841/0x861、IntelMSR 每核 DTS、LpcIO 风扇/电压）、
`src/collect/Sensors.cpp/.h` D6 集成、全仓回归、README 核对。协议基准：`docs/phase/11_kernel_research.md`
+ LHM PawnIo.cs 双源（文件头声明）；本机 PawnIO 2.2.0 已装、官方签名 blob 已放置（IntelMSR 5324B / LpcIO 18076B，见 stm.log）。

## 0. 结论

**P0 = 0，P1 = 3，P2 = 6。** 核心协议（设备路径/CTL_CODE/入出参布局/VERSION 出参定长）与研究文档
及 LHM 用法逐项一致；真机 12600H 上 4 项 pawnio 自测含 `pawnio_live_temps` 双采样稳定通过；
全仓回归（build exit 0、127 项 selftest ×2、--smoke 150、六条 --autotest、60s 双采样内存/句柄）全部通过。
P1 集中在：LpcIO 探测失败路径漏发退出序列（SuperIO 残留配置态）与 README 与新能力矛盾。

## 1. P1（应修，1 个工作日内）

### P1-1 LpcIO 探测失败分支漏发 SuperIO 退出序列，芯片残留配置态
- 文件：`src/collect/PawnIoLink.cpp:300-303`（Nuvoton）与 `:384-436`（ITE）。
- 证据：Nuvoton 分支 `if (!LpcFindBars(h)) break;` 后经 `:363 if (res.chipKnown) return res;`
  直接返回——此前 `NuvotonEnter`（:159，写 0x87 0x87）已进配置态，`NuvotonExit`（0xAA，
  :307 在基址读取之后才执行）未走到。ITE 分支同理：`:384 if (LpcFindBars(h)) {…}` 不成立时
  `:436 return res;`，`It87Exit` 未调用（0x2E 槽应写 0x02<-0x02）。
- 危害：SuperIO 停留在 MB PnP 配置态；下一次 10s 后重探会再发进入键，此时字节落入配置索引
  寄存器（等效乱写配置寄存器 0x87/0x01/0x55 等），与 LHM"总是退出"的卫生约定相悖；虽不触及
  风扇控制/传感值寄存器，但属硬件状态残留，失败路径（BAR 探测失败）并非不可能。
- 修法：两个失败出口在 return 前补 `NuvotonExit(h, regPort)` / `It87Exit(h, regPort)`；
  更稳的是引入 RAII 守卫（构造记 regPort，析构按家族调 Exit），覆盖所有早退路径。

### P1-2 README 与 D6 能力矛盾（用户文档失实）
- 文件：`README.md:19`（"风扇（诚实标注"需要驱动支持"——本应用不内置内核驱动）"）、
  `README.md:49`（"CPU 每核温度、主板风扇转速无法在用户态获取"）。
- 证据：D6 后本机真机已出每核 DTS 与（芯片已知时）真实风扇读数（stm.log：PawnIO 2.2.0 +
  两模块加载成功；pawnio_live_temps PASS）。README 仍声明该数据"无法获取"，且全篇无 PawnIO
  安装指引与安全声明——用户无从得知 opt-in 路径。另 `README.md:34` "自测（40 项）"过时
  （实为 127 项）。`docs/phase/11_kernel_research.md` §5 的声明草稿可直接取用，但其中
  "打开设备的句柄仅在一次读取期间存在，无常驻内核上下文"与实现不符（模块执行器句柄为进程级
  常驻，与 LHM 相同），照抄会引入新的失实。
- 修法：见 §5 建议文本（已按实现改写句柄表述）。

### P1-3 `ReadCpuDtsTemps` 核序号标签在过滤后漂移（诚实性瑕疵）
- 文件：`src/collect/PawnIoLink.cpp:728-729` + `src/collect/Sensors.cpp:1577`。
- 证据：某核读数被合理域钳制拒绝（`tjMax>0 && temp∈[0,120]` 不满足）时该核被跳过且不占
  `count` 槽位，Sensors 按 `i+1` 顺序贴"CPU 核心 N（DTS）"标签——后续核的 N 与实际逻辑核
  序号错位（如第 3 核被滤掉，第 4 核标成"核心 3"）。
- 修法：PawnIoLink 返回结构含逻辑核序号（或由调用方用 `GetLogicalProcessorInformationEx`
  同序枚举），Sensors 用真实核号做标签；最简：把被钳制的核改记为占位失败并保持序号推进。

## 2. P2（建议修）

- **P2-1 模块 blob 无本地校验（文件：PawnIoLink.cpp:600-656）**：`PawnIoLoadModuleFromFile`
  只做尺寸闸（16B–1MiB）后直接提交驱动；非官方签名 blob 由**驱动侧**签名校验兜底拒绝
  （`:577` 注释 + 失败即诚实报"签名/版本不符"），内核安全性成立。但 `%LOCALAPPDATA%` 与
  exe 目录均可被同权限进程替换，建议追加 SHA-256 钉扎（官方 0.2.11 两枚 blob 摘要）作为
  纵深防御 + 加载日志记录摘要便于审计。
- **P2-2 模块句柄重载 use-after-close 竞态（PawnIoLink.cpp:587-594 / :104-108）**：
  `PawnIoLoadModule` 同名重载会 `CloseHandle` 旧句柄，另一线程若正持该句柄执行 EXECUTE_FN
  （`ModuleHandle` 取裸句柄、锁外使用）会撞已关闭句柄（含句柄值复用风险）。当前仓库内无
  重载调用方（仅加载一次），风险低；建议加载侧持有互斥覆盖"查-载-换"全段或引入引用计数。
- **P2-3 无 `PawnIoShutdown()`，进程退出依赖 OS 回收句柄（PawnIoLink.cpp:59, g_modules）**：
  常驻执行器句柄与 LHM 同构、60s 双采样句柄数零增长（693→693），非泄漏；但自测/优雅卸载
  无从显式关闭。建议补 `PawnIoShutdown()`（关闭全部槽位句柄 + 清 probed 缓存）。
- **P2-4 DTS 全失败时 UI 静默（Sensors.cpp:1585-1587）**：仅 `n==-2`（模块未放置）追加备注，
  `n==-3`（MSR 读取失败）/`n==0` 无任何用户可见提示。诚实（无伪数据）但可诊断性差，
  建议 -3 时备注"PawnIO 在但 MSR 读取失败"。
- **P2-5 `RunLpcProbeOnce` 并发去重缺失（PawnIoLink.cpp:451-499）**：缓存检查与回填之间
  无"进行中"标记，两线程同 tick 各做一次全探测（IsaBusMutex 串行化后第二次白白多等）。
  建议加 in-flight 标记或复用 ops 队列去重。
- **P2-6 芯片表为只读子集（PawnIoLink.cpp:215-244, :407）**：IT87 仅 3 路风扇（LHM 新芯片
  有扩展 tach 寄存器如 0x62+）、NCT 电压寄存器取通用 9 路——读不到时诚实不出线，非缺陷；
  登记为后续扩展项，避免被误报为"漏数据"。

## 3. 已查无问题清单

1. **IOCTL 常量与布局**：设备类型 41394、0x821/0x841/0x861、全 METHOD_BUFFERED/FILE_ANY_ACCESS、
   `\\?\GLOBALROOT\Device\PawnIO` 与研究文档 §1 逐字一致（PawnIoLink.cpp:33-37）。
2. **EXECUTE_FN 布局**：char[32] 零填充名（名长 ≥1 且 <32 校验，:77）+ UINT64 参数；出参
   `bytesReturned/8`；outMax=0 时传 nullptr/0（与 LHM expectedReturnCount=0 同法）。
3. **VERSION**：出参强制 `sizeof(ULONG)`、`bytes==sizeof(out)` 复核、版本 ≥2.0 门（:529-533）；
   真机返回 2.2.0。
4. **绑核读 MSR**：`GetThreadGroupAffinity` 保存原亲和性；每核读完即恢复（:733），循环后再
   兜底恢复一次（:741），ExecuteN 失败路径也恢复；恢复失败不吞（继续下一核，状态诚实）。
   `GetLogicalProcessorInformationEx` 用 `std::vector` 承载，无手工释放问题；GroupCount=0
   防御为 1（:707）。
5. **失败码语义**：DTS/Fans/Volts 均 {-1 不可用, -2 模块缺失, -3 读取失败, 0 无数据（诚实非
   失败）}；自测 `pawnio_detect/dts_domain/lpcio_optional` 断言"不可用时不得出正读数"。
6. **LpcIO 只读性**：全部写操作仅为进入/退出键（0x87…/0xAA/0x02<-0x02）、LDN 选择（0x07）、
   NCT bank 选择与 ITE/NCT 索引口写——均为寻址所需，与 LHM 逐寄存器一致；无任何风扇控制/
   传感配置值写入；读数前有厂商号（0x5CA3 / 0x90|0x7F）与基址（`<0x100 || &0xF007`）双重校验，
   校验不过即放弃不猜。
7. **ISA 互斥体**：`Global\Access_ISABUS.HTP.Method`（与 LHM 同名同前缀），RAII 配对，
   WAIT_ABANDONED 视为持有（合规），5s 超时，超时→-3 诚实。
8. **900ms 缓存**：风扇与电压同一 `LpcOutcome` 快照，同 tick 读数与芯片名一致；TTL 检查/回填
   各自持锁，`cachedAt` 语义偏差可忽略（下次调用立即过期重探）。
9. **Sensors 集成诚实性**：DTS 条目 `state=Ok, source="PawnIO"`，0-120°C 钳制在源头；通道
   不可用→一行不出、不伪造；风扇 `nf<=0` → 回落 `NeedDriver` 占位 + notes，`nf>0` → 真实
   RPM + 芯片名标签，与既有 ACPI/WMI/DPTF 条目无重复、无聚合顶替；notes 文案准确
   （"只读寄存器""官方签名模块"）。自测 `sensors_temp_sources_labeled` 白名单含 "PawnIO"。
10. **回归**：Release 构建 exit 0（零告警闸以 build_v26_review.log 为证）；selftest
    127/127 ×2 轮（exit 0）；`--smoke 150` exit 0；`--autotest kill|tree|startup|dialogclick|
    about|wallpaper` 六条 exit 0。
11. **内存/句柄**：真机实例 60s 双采样：WS 131,166,208→130,932,736（-0.2MB）、PM -0.5MB、
    句柄 693→693（含 PawnIO 常驻 2 句柄——零增长，无泄漏）；测试实例随后被评审脚本终止。

## 4. 模块 blob 来源与校验评估（维度 1 专项）

- 来源策略（PawnIoLink.h:20-30）：仅本地文件、两处标准路径、零联网；官方渠道（namazso/
  PawnIO.Modules 0.2.11 或 LHM 仓库内嵌副本）写入注释与用户提示文案——**代码路径无任何
  网络调用**（全文件仅 CreateFileW 本地读），合规。
- 非官方 blob：驱动侧签名校验是真正的安全边界（LOAD_BINARY 失败 GLE 上抛、诚实报错），
  应用侧不伪造成功；故"文件校验缺失"降级为纵深防御项（P2-1），不构成放行漏洞。
- 真机实证：IntelMSR.bin 5324B / LpcIO.bin 18076B 均一次加载成功，与官方 0.2.11 尺寸相符。

## 5. README 建议文本

(a) `README.md:19` 传感器条目"风扇（…）"处改为：

> 风扇与**每核 CPU 温度（DTS）**：默认诚实标注"需要驱动支持"；本机装有 **PawnIO**（官方签名
> 内核运行时，用户自行安装）且已放置官方签名模块时，自动追加每核 DTS 温度、风扇转速与主板
> 电压读数（来源标注 PawnIO，只读寄存器，绝不写任何控制寄存器）。

(b) `README.md:49` 已知限制改为：

> - **不内置/不分发/不静默安装任何内核驱动**：每核 CPU 温度、主板风扇转速默认无法在用户态获取，
>   界面如实标注"需要驱动支持"；可选 **opt-in** 途径：用户自行从官方渠道安装 [PawnIO](https://github.com/namazso/PawnIO.Setup/releases)
>   （签名安装包，HVCI/Secure Boot 兼容），并将官方签名模块 IntelMSR.bin / LpcIO.bin（[PawnIO.Modules 0.2.11](https://github.com/namazso/PawnIO.Modules/releases/tag/0.2.11)
>   或 LibreHardwareMonitor 仓库内嵌副本）放置到 `%LOCALAPPDATA%\SuperTaskMgr\modules\` 或 exe
>   旁 `modules\`——本应用只读本地文件，不下载、不代装、不捆绑；未安装时一切如常（诚实四态）。

(c) 新增"权限与隐私声明"条目（取代 research doc §5 草稿中与实现不符的句柄表述）：

> - PawnIO 通道仅通过其公开设备 IOCTL 调用官方已签名只读模块：MSR 仅读白名单寄存器
>   （0x19C/0x1A2），SuperIO 仅读传感寄存器（进入/退出配置态与 bank 选择除外，与
>   LibreHardwareMonitor 同序列），绝不写风扇/电压控制寄存器、绝不干预调速；已加载模块的
>   执行器句柄进程内常驻（与 LibreHardwareMonitor 相同），进程退出即由系统回收；SuperIO
>   访问全程持有 `Access_ISABUS.HTP.Method` 全局互斥体（5s 超时，超时放弃并如实报告）。

(d) `README.md:34`："自测（40 项…）"→"自测（127 项…）"。

## 6. 证据索引

- 构建：`build_v26_review.log`（exit 0）。自测：`v26_selftest_r1.json`（pass:127,fail:0）、
  `v26_selftest_r2.log`（合计 127 通过 0 失败，:80-83 四项 pawnio PASS 含 live_temps）。
- 冒烟/自动测试：`v26_smoke.log`、`v26_at_{kill,tree,startup,dialogclick,about,wallpaper}.log`（全 exit 0）。
- 真机日志：`%LOCALAPPDATA%\SuperTaskMgr\logs\stm.log`（PawnIO 2.2.0 就绪；IntelMSR 5324B、
  LpcIO 18076B 加载成功；评审窗口内无 WARN）。
- 内存采样：评审会话 PowerShell 输出（T0/T60/delta 全部非增，句柄 693→693）。
