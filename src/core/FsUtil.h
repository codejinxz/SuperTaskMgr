#pragma once
// Well-known directories for the app, rooted at %%LOCALAPPDATA%%\SuperTaskMgr
#include <string>

namespace stm {

std::wstring ExePath();
std::wstring ExeDir();
// Creates the directory (and parents) when missing; returns empty string on failure.
std::wstring EnsureDir(const std::wstring& path);
std::wstring LocalAppDataRoot();  // ...\SuperTaskMgr
std::wstring LogDir();            // ...\logs
std::wstring ConfigPath();        // ...\config.json
std::wstring SessionPath();       // ...\session.json

}  // namespace stm
