#pragma once
// 第 3 阶段契约：4 来源启动项（架构文档第 5 节；R5 表行 14a-14d）。
// 禁用/启用必须先写原值备份（架构第 9 节：
// 可逆性), see SetStartupEnabled contract below.
#include <cstdint>
#include <string>
#include <vector>

namespace stm {
namespace ops {

enum class StartupSource : uint32_t {
    RegRun = 0,        // HKLM/HKCU ...CurrentVersion\Run（+ 下方 RegRun32 的 Wow6432Node）
    RegRun32,          // Wow6432Node 变体（32 位运行项的 64 位视图）
    StartupFolder,     // 用户 + 公共启动文件夹（.lnk 文件）
    ScheduledTask,     // 登录触发任务（调用方可见 \ 与 \Microsoft\...）
    UwpStartupTask,    // AppModel SystemAppData\<PFN>\<TaskId> 的 State 值
};

struct StartupItem {
    StartupSource source = StartupSource::RegRun;
    std::wstring id;        // 唯一键：注册表路径 + "\\" + 值名 / 文件路径 / 任务路径 / pfn+任务
    std::wstring name;      // 展示名
    std::wstring command;   // 解析后的命令行或文件路径或任务 exe
    std::wstring location;  // 所在位置（注册表键 / 文件夹 / 任务文件夹 / 包）
    bool enabled = true;
    bool canToggle = false; // 写访问需要管理员时为 false（HKLM 任务等）
};

// 枚举全部来源；单来源失败记日志并跳过（部分结果 +
// err 非空说明哪个来源失败）。err 为空 = 全部来源读取成功。
std::vector<StartupItem> EnumStartupItems(std::wstring* err);

// 启用/禁用一个条目。各来源语义：
//  - RegRun/RegRun32/StartupFolder：写 StartupApproved\<Run|Run32|StartupFolder> 值
//   （低位奇字节 = 禁用 + 8 字节 FILETIME），先把原值备份到
//    %LOCALAPPDATA%\SuperTaskMgr\startup_backup\<hash>.txt（名称 -> 原始字节十六进制 + 路径）。
//  - ScheduledTask：IRegisteredTask Enabled=false/true（put_Enabled；C++ 中
//    不可用时回退 RegisterTaskDefinition 重注册）。
//  - UwpStartupTask：写 State 值 0/2（禁用/启用）；先备份旧值。
// 失败时返回 false 并带 err（中文，面向用户）。备份失败 => 拒绝写入。
bool SetStartupEnabled(const StartupItem& item, bool enable, std::wstring* err);

}  // namespace ops
}  // namespace stm
