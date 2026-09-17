#pragma once
// Phase-3 contract: startup items from 4 sources (arch doc section 5; R5 table rows 14a-14d).
// DISABLE/ENABLE MUST write a backup of the original value first (arch section 9:
// 可逆性), see SetStartupEnabled contract below.
#include <cstdint>
#include <string>
#include <vector>

namespace stm {
namespace ops {

enum class StartupSource : uint32_t {
    RegRun = 0,        // HKLM/HKCU ...CurrentVersion\Run (+Wow6432Node as RegRun32 below)
    RegRun32,          // Wow6432Node variant (64-bit view of 32-bit run entries)
    StartupFolder,     // user + common startup folders (.lnk files)
    ScheduledTask,     // logon-trigger tasks (\ and \Microsoft\... visible to caller)
    UwpStartupTask,    // AppModel SystemAppData\<PFN>\<TaskId> State values
};

struct StartupItem {
    StartupSource source = StartupSource::RegRun;
    std::wstring id;        // unique key: registry path + "\\" + value name / file path / task path / pfn+task
    std::wstring name;      // display name
    std::wstring command;   // resolved command line or file path or task exe
    std::wstring location;  // where it lives (registry key / folder / task folder / package)
    bool enabled = true;
    bool canToggle = false; // false when write access needs admin (HKLM tasks etc.)
};

// Enumerate all sources; single source failures are logged and skipped (partial result
// + err non-empty describing which source failed). err empty = all sources read.
std::vector<StartupItem> EnumStartupItems(std::wstring* err);

// Enable/disable one item. Semantics per source:
//  - RegRun/RegRun32/StartupFolder: StartupApproved\<Run|Run32|StartupFolder> value write
//    (odd low byte = disabled + 8-byte FILETIME), backing up the ORIGINAL value first to
//    %LOCALAPPDATA%\SuperTaskMgr\startup_backup\<hash>.txt (name -> raw bytes hex + path).
//  - ScheduledTask: IRegisteredTask Enabled=false/true (put_Enabled; fall back to
//    RegisterTaskDefinition re-register if put_Enabled unavailable in C++).
//  - UwpStartupTask: State value 0/2 write (Disabled/Enabled); backup old value first.
// Returns false with err (Chinese, user-facing) on failure. Backup failure => refuse write.
bool SetStartupEnabled(const StartupItem& item, bool enable, std::wstring* err);

}  // namespace ops
}  // namespace stm
