#pragma once
// Phase-3 contract: system services (SCM). Architect-owned, frozen.
// Read paths work without admin; start/stop need admin for most system services.
// Stop semantics: warn about dependents first (EnumDependentServices), never force-kill.
#include <cstdint>
#include <string>
#include <vector>

namespace stm {
namespace ops {

struct ServiceInfo {
    std::wstring name;          // service key name
    std::wstring displayName;
    std::wstring description;   // may be empty
    std::wstring account;       // LPWSTR from config (LocalSystem / NT AUTHORITY\...)
    uint32_t state = 0;         // SERVICE_*STATE (1 stopped .. 7 paused-ish, winsvc.h)
    uint32_t startType = 0;     // SERVICE_AUTO_START etc; SERVICE_DISABLED = 4
    uint32_t pid = 0;           // 0 = not running or shared (see acceptPause below)
    bool sharedProcess = false; // svchost-style host: pid shared by multiple services
    bool canStop = false;       // controls accepted from config
};

// Enumerate SERVICE_WIN32 services (drivers come from DriverOps). err on failure.
std::vector<ServiceInfo> EnumServices(std::wstring* err);

// Start; ControlService(stop). stopDependents=true stops dependent services leaf-first
// (still refuses when a dependent is protected/critical). All ops require admin in practice.
bool StartServiceByName(const std::wstring& name, std::wstring* err);
bool StopServiceByName(const std::wstring& name, bool stopDependents, std::wstring* err);
// Convenience for UI warnings: names of running dependent services.
std::vector<std::wstring> GetDependentServices(const std::wstring& name);

}  // namespace ops
}  // namespace stm
