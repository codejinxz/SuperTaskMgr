#pragma once
// PawnIoLink —— PawnIO 内核温度/风扇/电压通道（D6，2026-09-20）。
// 协议来源：PawnIO v2.2.0 源码核实（docs/phase/11_kernel_research.md §1 路线 1；
// D3 尖峰已验证 \Device\PawnIO 打开与 VERSION 调用约定），LHM PawnIo.cs 交叉核对：
//   设备：\\?\GLOBALROOT\Device\PawnIO（设备类型 41394，全 METHOD_BUFFERED/FILE_ANY_ACCESS）
//     LOAD_BINARY = 0x821 槽：入参 = 模块 blob 原始字节
//     EXECUTE_FN  = 0x841 槽：入参 = char[32] 零填充函数名 + UINT64 参数[]
//                             出参 = UINT64[]，bytesReturned/8 = 返回个数
//     VERSION     = 0x861 槽：出参 = ULONG 版本 (major<<16|minor<<8|patch)
//   官方模块（LGPL-2.1，官方已签名 blob，绝不自行签名/修改）：
//     IntelMSR：ioctl_read_msr（in=[msr] out=[value]，白名单只读 MSR）
//     LpcIO   ：ioctl_select_slot → ioctl_find_bars → ioctl_pio_inb/outb、
//               ioctl_superio_inb/inw/outb（端口白名单 = 发现的 BAR）
//   许可证：驱动 GPL-2.0 + IOCTL 例外——仅经设备 IOCTL 通信的独立程序不被感染；
//   模块 LGPL-2.1（官方签名 blob 由用户侧放置，本应用不分发、不联网下载）。
//
// 诚实模型（R6 四态的 PawnIO 延伸）：所有失败路径返回 ≤0 或显式状态，
// 绝不伪造读数；日志模块名 "pawnio"。
//
// 模块 blob 获取策略（零联网承诺）：
//   运行时只读本地文件（按序）：
//     1) %LOCALAPPDATA%\SuperTaskMgr\modules\IntelMSR.bin / LpcIO.bin
//     2) <exe 目录>\modules\IntelMSR.bin / LpcIO.bin
//   且文件 SHA-256 必须命中官方 blob 白名单（V26 P2 纵深防御：签名由驱动
//   校验，哈希白名单再钉扎具体发布物；未知哈希拒载并诚实提示）。
//   blob 由用户或安装脚本从下列官方渠道下载放置（本文件常量仅供指引，
//   代码在任何情况下都不发起网络请求）：
//     - PawnIO.Modules 官方发布（0.2.11，官方签名）：
//         https://github.com/namazso/PawnIO.Modules/releases/tag/0.2.11
//     - LibreHardwareMonitor 仓库内嵌的同一批官方签名副本：
//         .../LibreHardwareMonitorLib/Resources/PawnIo/IntelMSR.bin（及 LpcIO.bin）
//   未放置 → 模块加载失败 → 对应能力诚实进入不可用态，主流程零影响。
//   （官方发布新模块版本时需同步更新白名单常量。）
#include <cstdint>
#include <string>

namespace stm {
namespace pawnio {

// ---- 状态探针 -------------------------------------------------------------

struct PawnIoStatus {
    bool probed = false;          // 是否已执行过设备探测
    bool driverOpenable = false;  // \Device\PawnIO 可打开且 VERSION 成功
    uint32_t driverVersion = 0;   // (major<<16)|(minor<<8)|patch；未打开为 0
    bool msrLoaded = false;       // IntelMSR 模块已加载（本进程内）
    bool lpcLoaded = false;       // LpcIO 模块已加载（本进程内）
    std::wstring detail;          // 非空中文状态说明（诚实，面向用户/自测）
};

// 探测设备并取回状态（首次探测结果缓存，进程内不再重试打开设备——
// 安装 PawnIO 后需重启进程，行为可预测）。不崩溃、不加载驱动。
PawnIoStatus GetPawnIoStatus();

// 设备存在且驱动版本 ≥ 2.0。不含"模块已放置"含义（那需要 blob 文件）。
bool PawnIoAvailable();

// ---- 模块加载与执行 --------------------------------------------------------

// 向驱动提交模块 blob（开辟一个执行器句柄，按 name 记槽位；
// 同名重载会先关闭旧句柄）。size 合法域 [16, 1 MiB]。
bool PawnIoLoadModule(const uint8_t* blob, size_t size, const char* name);

// 从标准位置查找并加载官方签名模块（见文件头获取策略）。
// name 为 "IntelMSR" 或 "LpcIO"。找不到/加载被拒返回 false 并给 detail。
bool PawnIoLoadModuleFromFile(const char* name, std::wstring* detail);

// 在 IntelMSR 模块上执行单入单出函数（典型：ioctl_read_msr）。
// 失败返回 false（含模块未加载、函数返回非成功 NTSTATUS）。
bool PawnIoExecute(const char* fn, uint64_t arg, uint64_t* out);

// 通用执行：在指定模块上调用 fn。outCount 返回实际写出的 UINT64 个数。
bool PawnIoExecuteN(const char* module, const char* fn, const uint64_t* in,
                    size_t inCount, uint64_t* out, size_t outMax, size_t* outCount);

// ---- 高层读数（全部诚实返回，绝不伪造）------------------------------------
//
// 每逻辑核 DTS 温度（°C）：绑核 → ioctl_read_msr(0x19C) 取 Digital Readout
// [22:16]，ioctl_read_msr(0x1A2) 取 TjMax [23:16]，温度 = TjMax - DTS。
// outLpIndex（可空）回填每条读数对应的真实逻辑处理器序号（0 基）——
// 被 0-120°C 合理域滤掉的核不占输出槽位但保留原序号（V26 P1-3：标签
// "CPU 核心 N（DTS）" 的 N = 序号+1，与系统逻辑核编号不漂移）。
// 返回 >0 = 写入 outTemps 的读数个数；0 = 无有效读数；
// -1 = PawnIO 不可用；-2 = IntelMSR 模块缺失/加载失败；-3 = MSR 读取失败。
int ReadCpuDtsTemps(int* outTemps, int* outLpIndex, int maxCores);

// SuperIO 风扇转速（rpm，尽力而为；LpcIO 模块 + 芯片表）。
// 返回 >0 = 写入 rpms 的个数；0 = 无读数（芯片未知/无风扇——不算失败，诚实）；
// -1 = PawnIO 不可用；-2 = LpcIO 模块缺失/加载失败；-3 = SuperIO 访问失败。
int ReadLpcIoFans(int* rpms, int max);

// SuperIO 电压（V，尽力而为，同上语义）。
int ReadLpcIoVoltages(double* volts, int max);

// 最近一次 LpcIO 会话识别到的芯片名（如 L"ITE IT8628E"/L"Nuvoton NCT6779D"），
// 未识别为空。供读数标签使用。
std::wstring LpcIoChipName();

// 模块文件名常量（供集成方写日志/提示）。
constexpr const char* kMsrModuleName = "IntelMSR";
constexpr const char* kLpcModuleName = "LpcIO";

// 显式释放全部模块执行器句柄（含重载退役句柄，见 .cpp）。供进程退出前
// 调用（main.cpp 接线：ctx->jobs.Shutdown(2000) 之后一行）。重复调用安全。
void PawnIoShutdown();

}  // namespace pawnio
}  // namespace stm
