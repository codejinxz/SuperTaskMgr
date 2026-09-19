#pragma once
// 应用的已知目录，根为 %%LOCALAPPDATA%%\SuperTaskMgr
#include <string>

namespace stm {

std::wstring ExePath();
std::wstring ExeDir();
// 目录缺失时创建（含父目录）；失败返回空字符串。
std::wstring EnsureDir(const std::wstring& path);
std::wstring LocalAppDataRoot();  // ...\SuperTaskMgr
std::wstring LogDir();            // ...\logs
std::wstring ConfigPath();        // ...\config.json
std::wstring SessionPath();       // ...\session.json

}  // namespace stm
