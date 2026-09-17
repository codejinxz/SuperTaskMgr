#pragma once
// Collection service (arch sections 4-5). Contract header — architect-owned.
// Owns the single collection thread; writes completed Snapshot into its SnapshotStore.
// Pimpl: keeps worker internals out of this contract.
#include <cstdint>
#include <memory>
#include "core/ProcData.h"

namespace stm {

class CollectService {
public:
    CollectService();
    ~CollectService();

    bool Start(uint32_t intervalMs);  // default 1000
    void Stop();                      // joins the worker; safe to call twice

    void SetInterval(uint32_t ms);    // clamped to [500, 5000]
    SnapshotStore& Store() { return store_; }

    // Self-observability (status bar + selftest).
    uint64_t TickCount() const;
    double LastTickMs() const;        // duration of most recent tick
    double TickP95Ms() const;         // p95 over rolling window
    double GpuTickP95Ms() const;      // GPU/PDH wildcard query latency (separate 2s cadence)

    void SetGpuEnabled(bool on);      // default on; PDH GPU Engine + DXGI adapters
    bool GpuEnabled() const;

    // Phase 3 extension points (declared now to keep the contract frozen):
    void SetNetEtwEnabled(bool on);   // ETW Kernel-Network per-process rates; admin only
    bool NetEtwEnabled() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    SnapshotStore store_;
};

}  // namespace stm
