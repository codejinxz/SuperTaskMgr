#pragma once
// 采集服务（架构第 4-5 节）。契约头——归架构所有。
// 持有唯一采集线程；把完成的 Snapshot 写入其 SnapshotStore。
// Pimpl：把工作线程内部细节隔离在契约之外。
#include <cstdint>
#include <memory>
#include "core/ProcData.h"

namespace stm {

class CollectService {
public:
    CollectService();
    ~CollectService();

    bool Start(uint32_t intervalMs);  // 默认 1000
    void Stop();                      // join 工作线程；可安全调用两次

    void SetInterval(uint32_t ms);    // 限制在 [500, 5000]
    SnapshotStore& Store() { return store_; }

    // 自观测（状态栏 + selftest）。
    uint64_t TickCount() const;
    double LastTickMs() const;        // 最近一次 tick 的耗时
    double TickP95Ms() const;         // 滚动窗口上的 p95
    double GpuTickP95Ms() const;      // GPU/PDH 通配查询延迟（独立 2s 节拍）

    void SetGpuEnabled(bool on);      // 默认开；PDH GPU Engine + DXGI 适配器
    bool GpuEnabled() const;

    // 第 3 阶段扩展点（现在就声明以保持契约冻结）：
    void SetNetEtwEnabled(bool on);   // ETW Kernel-Network 每进程速率；仅管理员
    bool NetEtwEnabled() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    SnapshotStore store_;
};

}  // namespace stm
