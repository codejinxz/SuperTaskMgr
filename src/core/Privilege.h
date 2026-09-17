#pragma once
// Token privilege helpers. Every privilege enablement is logged (auditable, arch section 7).
#include <string>

namespace stm {

bool IsProcessElevated();
// Enable/disable a privilege by name on the process token (e.g. L"SeDebugPrivilege").
// Returns false with *err set on failure. Every successful enable/disable is logged.
bool EnablePrivilege(const wchar_t* name, bool enable, std::wstring* err = nullptr);

}  // namespace stm
