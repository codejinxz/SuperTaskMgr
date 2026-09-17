#pragma once
// Phase-3 contract: kernel driver list. Architect-owned, frozen.
// Windows 11 24H2+ requires SeDebugPrivilege for EnumDeviceDrivers (documented):
// without it the call "succeeds" but returns zero addresses — we detect that and
// return the honest error 需要管理员权限 instead of an empty list.
#include <cstdint>
#include <string>
#include <vector>

namespace stm {
namespace ops {

struct DriverInfo {
    std::wstring name;      // file name (e.g. nvlddmkm.sys)
    std::wstring path;      // full path (\\SystemRoot\... normalized to a real path when possible)
    uint64_t imageBase = 0;
    uint32_t imageSize = 0; // bytes
};

// Enumerate loaded kernel drivers. Non-elevated on 21H2-23H2 works; 24H2+ returns
// false + err=需要管理员权限 (UI shows the elevate badge). Signature state is NOT
// computed here (expensive) — UI can use ops::VerifyFileSignature via the job queue.
std::vector<DriverInfo> EnumDrivers(std::wstring* err);

}  // namespace ops
}  // namespace stm
