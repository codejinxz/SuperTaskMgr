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

    // ---- 兼容模式诊断（维护轮 10 新增，架构师契约） -------------------------
    // 最近一次启动自检门的逐项结果；未跑过自检（启动中）返回空。
    struct SelfCheckItem {
        const wchar_t* name = L"";    // 检查项名（如 L"CPU 时间字段"）
        bool ran = false;             // 本轮是否执行了该项
        bool passed = false;
        std::wstring detail;          // 测量值对比/失败原因（中文，诊断报告用）
    };
    std::vector<SelfCheckItem> LastSelfCheckReport() const;
    // 请求在下一个采集 tick 重跑自检门（成功则自动退出兼容模式）。线程安全。
    void RequestSelfCheckRetry();
    // 自检门代际：门每真正跑完一次（含报告落库）+1。UI 以"代际变化"判定
    // 一次重跑确实完成（V24 P1-1：tick 计数会提前，代数不会）。线程安全。
    uint64_t LastSelfCheckGeneration() const;

    // 第 3 阶段扩展点（现在就声明以保持契约冻结）：
    void SetNetEtwEnabled(bool on);   // ETW Kernel-Network 每进程速率；仅管理员
    bool NetEtwEnabled() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    SnapshotStore store_;
};

}  // namespace stm
