#pragma once
// 第 3 阶段契约：内核驱动列表。归架构所有，已冻结。
// Windows 11 24H2+ 上 EnumDeviceDrivers 需要 SeDebugPrivilege（有文档）：
// 没有它调用会"成功"但返回全零地址——我们检测该形态并
// return the honest error 需要管理员权限 instead of an empty list.
#include <cstdint>
#include <string>
#include <vector>

namespace stm {
namespace ops {

struct DriverInfo {
    std::wstring name;      // 文件名（如 nvlddmkm.sys）
    std::wstring path;      // 完整路径（\\SystemRoot\... 尽可能归一化为真实路径）
    uint64_t imageBase = 0;
    uint32_t imageSize = 0; // 字节
};

// 枚举已加载的内核驱动。21H2-23H2 非提权可用；24H2+ 返回
// false + err=需要管理员权限 (UI shows the elevate badge). Signature state is NOT
// 在此计算（开销大）——UI 可经任务队列使用 ops::VerifyFileSignature。
std::vector<DriverInfo> EnumDrivers(std::wstring* err);

}  // namespace ops
}  // namespace stm
