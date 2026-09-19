#pragma once
// 第 3 阶段契约：系统服务（SCM）。归架构所有，已冻结。
// 读取路径无需管理员；启动/停止对多数系统服务需要管理员。
// 停止语义：先提示依赖者（EnumDependentServices），绝不强杀。
#include <cstdint>
#include <string>
#include <vector>

namespace stm {
namespace ops {

struct ServiceInfo {
    std::wstring name;          // 服务键名
    std::wstring displayName;
    std::wstring description;   // 可能为空
    std::wstring account;       // 配置中的 LPWSTR（LocalSystem / NT AUTHORITY\...）
    uint32_t state = 0;         // SERVICE_*STATE（1 停止 .. 7 近似暂停，winsvc.h）
    uint32_t startType = 0;     // SERVICE_AUTO_START 等；SERVICE_DISABLED = 4
    uint32_t pid = 0;           // 0 = 未运行或共享（见下方 acceptPause）
    bool sharedProcess = false; // svchost 式宿主：pid 被多个服务共享
    bool canStop = false;       // 配置接受的控制码
};

// 枚举 SERVICE_WIN32 服务（驱动来自 DriverOps）。失败时设置 err。
std::vector<ServiceInfo> EnumServices(std::wstring* err);

// 启动；ControlService(stop)。stopDependents=true 时先叶子后根地停止依赖服务
//（依赖者受保护/关键时仍然拒绝）。实践中所有操作都需要管理员。
bool StartServiceByName(const std::wstring& name, std::wstring* err);
bool StopServiceByName(const std::wstring& name, bool stopDependents, std::wstring* err);
// UI 警告用便捷接口：运行中的依赖服务名称。
std::vector<std::wstring> GetDependentServices(const std::wstring& name);

}  // namespace ops
}  // namespace stm
